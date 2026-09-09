"""测试 httpd close_fn 必须自己关闭 socket（回归防护）。

对应真机现象：web 页面一开始控制灵敏，控制一会儿后彻底失联，AI 控制同时失效。

背景：IDF 的 httpd_sess_delete()（components/esp_http_server/src/httpd_sess.c）实现是

    if (hd->config.close_fn) {
        hd->config.close_fn(hd, session->fd);
    } else {
        close(session->fd);
    }

即 close_fn 是**替代** httpd 默认的关闭动作，而不是"关闭前的通知"。若注册了
close_fn 却忘记 close(fd)，每个 HTTP 请求都会泄漏一个 fd，累积到
CONFIG_LWIP_MAX_SOCKETS（本板 16）后新连接全部失败 —— web 页面与小智 AI 协议
（同样需要 socket）一起失联。
"""

import os
import re
import unittest


class TestHttpdCloseFn(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "..",
            "main", "boards", "bread-compact-wifi-s3cam-airobot",
            "http_upload_server.cc",
        )
        with open(src, encoding="utf-8") as f:
            cls.src = f.read()

    def test_close_fn_is_registered(self):
        """注册 close_fn 才能兜底浏览器崩溃/网络中断时的 CLOSE 帧丢失。"""
        self.assertIn("cfg.close_fn =", self.src)

    def test_close_fn_closes_socket(self):
        """close_fn 替代了 httpd 默认的 close(fd)，函数体里必须自己关闭 socket。"""
        m = re.search(
            r"static void OnWsSessionClosed\(httpd_handle_t hd, int sockfd\)\s*\{(.*?)\n\}",
            self.src, re.S,
        )
        self.assertIsNotNone(m, "未找到 OnWsSessionClosed 定义")
        body = m.group(1)
        self.assertIn(
            "close(sockfd)", body,
            "close_fn 必须调用 close(sockfd)：否则每个 HTTP 请求泄漏一个 fd，"
            "累积到 CONFIG_LWIP_MAX_SOCKETS 后 web 与 AI 一起失联",
        )

    def test_unistd_included(self):
        """close() 声明在 <unistd.h>。"""
        self.assertIn("#include <unistd.h>", self.src)

    def test_ws_unregister_does_not_close(self):
        """WsUnregisterClient 只负责登记表；关闭动作应集中在 close_fn 一处，避免双重 close。"""
        m = re.search(r"static void WsUnregisterClient\(int fd\)\s*\{(.*?)\n\}", self.src, re.S)
        self.assertIsNotNone(m, "未找到 WsUnregisterClient 定义")
        self.assertNotIn("close(fd)", m.group(1))


if __name__ == "__main__":
    unittest.main()
