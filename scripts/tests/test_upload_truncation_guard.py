"""测试 /upload 必须校验实收字节数与 Content-Length 一致（回归防护）。

对应隐患：上传大文件（转码后的 MP3）或歌词时若浏览器/网络中途断开，
esp_http_server 的 httpd_req_recv() 会返回 0 —— 与"body 已读满"的返回值相同。
旧实现只判 ret < 0，于是**截断的文件被当成上传成功留在 TF 卡上**：
MP3 会提前结束、.lrc 会只显示前半段歌词，而页面提示"上传成功"，极难排查。

修复：以 req->content_len 为准复核实收 total，不一致则删除文件并返回 500。
content_len == 0（无 body/无长度信息，如 chunked）时只保留 ret < 0 判断，避免误杀。
"""

import os
import re
import unittest

SRC = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "main", "boards", "bread-compact-wifi-s3cam-airobot", "http_upload_server.cc",
)


def legacy_upload_ok(recv_results: list) -> bool:
    """旧逻辑：只判 ret < 0 才算失败；返回 True = 文件被保留（视为上传成功）。"""
    total, ret = 0, 0
    for r in recv_results:
        ret = r
        if ret <= 0:
            break
        total += r
    return ret >= 0


def fixed_upload_ok(recv_results: list, content_len: int) -> bool:
    """新逻辑：ret < 0 或实收字节数与 Content-Length 不符都算失败。"""
    total, ret = 0, 0
    for r in recv_results:
        ret = r
        if ret <= 0:
            break
        total += r
    if ret < 0:
        return False
    if content_len > 0 and total != content_len:
        return False
    return True


class TestUploadTruncationGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(SRC, encoding="utf-8") as f:
            cls.src = f.read()

    # ---------- 1. 根因固化：旧逻辑会留下截断文件 ----------

    def test_legacy_keeps_truncated_file(self):
        """固化根因：声明 4096 字节、实际只收到 1000 字节后对端断开（recv 返回 0）。

        旧逻辑把这种情况当上传成功 —— 这正是要修的 bug。
        """
        self.assertTrue(
            legacy_upload_ok([1000, 0]),
            "旧逻辑应当会把截断文件当成功保留（根因固化；若此处失败说明根因描述有误）",
        )
        self.assertFalse(
            fixed_upload_ok([1000, 0], 4096),
            "新逻辑必须判定为失败并删除截断文件",
        )

    # ---------- 2. 新逻辑不得误杀正常上传 ----------

    def test_normal_upload_passes(self):
        """正常上传：读满 Content-Length 后 httpd_req_recv 返回 0，必须判为成功。"""
        self.assertTrue(fixed_upload_ok([4096, 4096, 0], 8192))
        self.assertTrue(fixed_upload_ok([100, 0], 100))
        self.assertTrue(fixed_upload_ok([4096], 4096))

    def test_negative_ret_still_fails(self):
        """超时/错误（负数返回）仍必须判失败。"""
        self.assertFalse(fixed_upload_ok([2048, -3], 4096))

    def test_no_content_length_skips_size_check(self):
        """content_len == 0（无 body/无长度信息）时只保留 ret < 0 判断，避免误杀。"""
        self.assertTrue(fixed_upload_ok([100, 0], 0))
        self.assertFalse(fixed_upload_ok([-3], 0))

    # ---------- 3. 源码静态约束 ----------

    def handle_upload_body(self) -> str:
        m = re.search(r"static esp_err_t HandleUpload\(httpd_req_t\* req\)\s*\{(.*?)\n\}\n",
                      self.src, re.S)
        self.assertIsNotNone(m, "未找到 HandleUpload 定义")
        return m.group(1)

    def test_source_verifies_received_length(self):
        """HandleUpload 必须用 req->content_len 校验实收字节数。"""
        body = self.handle_upload_body()
        self.assertIn("content_len", body, "HandleUpload 必须用 req->content_len 校验完整性")
        self.assertIn("static_cast<size_t>(total) != req->content_len", body,
                      "必须把实收 total 与 Content-Length 对比")

    def test_failure_path_removes_file_and_returns_error(self):
        """完整性校验失败必须删除截断文件并返回错误（不能留下半截文件）。"""
        body = self.handle_upload_body()
        guard = re.search(r"if \(ret < 0 \|\|.*?\n    \}", body, re.S)
        self.assertIsNotNone(guard, "未找到 'ret<0 || 字节数不符' 的完整性校验分支")
        g = guard.group(0)
        self.assertIn("remove(path)", g, "校验失败必须删除截断文件")
        self.assertIn("httpd_resp_send_err", g, "校验失败必须返回错误响应")
        self.assertNotIn('httpd_resp_sendstr(req, "OK")', g, "校验失败分支不得回 OK")

    def test_success_reply_only_after_guard(self):
        """成功响应必须出现在完整性校验之后（顺序不可颠倒）。"""
        body = self.handle_upload_body()
        self.assertLess(body.index("static_cast<size_t>(total) != req->content_len"),
                        body.index('httpd_resp_sendstr(req, "OK")'),
                        '"OK" 必须在完整性校验之后才发送')


if __name__ == "__main__":
    unittest.main()
