"""测试崩溃日志留存（log_capture 的 .noinit 持久化）的回归防护。

背景：2026-09-17 前，日志环形缓冲在 .bss，设备一重启就清零，导致"拍照片偶发重启"
完全拿不到现场（网页日志在 `Established new connection` 一行后戛然而止，下一行就是
开机日志）。改为 .noinit 段后，软重启(panic/看门狗/OTA/esp_restart)仍能保留崩溃前的
日志，网页重连即可看到；同时打印 esp_reset_reason() 分隔行指明"怎么挂的"。

必须固化的约束：
1. 缓冲必须在 .noinit 段（否则重启即丢，功能等于没做）；
2. 必须有三重校验(magic/version/size/head)并在冷启动时清空
   （否则会把上电时的随机 DRAM 当成日志显示给用户）；
3. 只有"值得保留"的复位原因才保留旧日志（上电/未知必须丢弃，且不得用 default 放行）；
4. 重启分隔行必须带复位原因，且必须走 RingWrite（不能调 ESP_LOGx/printf 递归自锁）；
5. panic/backtrace 不走本钩子这个盲点必须写在头文件里（否则下次又会白找）。
"""

import os
import re
import unittest

BOARD_DIR = ("main", "boards", "bread-compact-wifi-s3cam-airobot")


def _read(*parts):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", *parts)
    with open(path, encoding="utf-8") as f:
        return f.read()


def _strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


class TestLogPersist(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = _read(*BOARD_DIR, "log_capture.cc")
        cls.header = _read(*BOARD_DIR, "log_capture.h")
        m = re.search(r"void LogCaptureInit\(\)\s*\{(.*?)\n\}", cls.src, re.S)
        assert m is not None, "未找到 LogCaptureInit 定义"
        cls.init = _strip_comments(m.group(1))

    def test_ring_lives_in_noinit(self):
        """缓冲与游标必须在 .noinit 段，软重启后才留得住崩溃日志。"""
        self.assertIn('__attribute__((section(".noinit")))', self.src)
        self.assertIn("RingStore s_store", self.src)
        # RingWrite / LogCapturePull 都必须操作同一个持久化结构
        self.assertIn("s_store.ring", self.src)
        self.assertIn("s_store.total", self.src)
        self.assertNotIn("char s_ring[kRingSize];", self.src,
                         "旧的 .bss 数组应已删除，避免两份状态不一致")

    def test_cold_boot_is_validated(self):
        """必须有 magic/version/size/head 校验，冷启动要清空（不能显示随机 RAM）。"""
        for token in ("kRingMagic", "kRingVersion", "s_store.magic", "s_store.size",
                      "s_store.head < kRingSize"):
            self.assertIn(token, self.init, f"LogCaptureInit 缺少校验项 {token}")
        self.assertIn("s_store.head = 0", self.init, "冷启动必须清空游标")
        self.assertIn("s_store.total = 0", self.init, "冷启动必须重置序号")

    def test_reset_reason_gates_and_reports(self):
        """保留与否必须由复位原因决定，且分隔行要带上原因文字。"""
        self.assertIn("esp_reset_reason()", self.init)
        self.assertIn("ShouldKeepPreviousLog(reason)", self.init)
        self.assertIn("ResetReasonText(reason)", self.init)
        # 关键复位原因必须可区分（内存问题 / 看门狗 / 欠压）
        for r in ("ESP_RST_PANIC", "ESP_RST_INT_WDT", "ESP_RST_TASK_WDT", "ESP_RST_BROWNOUT"):
            self.assertIn(r, self.src, f"复位原因文案应覆盖 {r}")
        self.assertIn("esp_system.h", self.src, "esp_reset_reason 需要 esp_system.h")

    def test_poweron_is_discarded(self):
        """上电冷启动绝不能在保留名单里，且不得用 default 放行未知原因。"""
        m = re.search(r"bool ShouldKeepPreviousLog\(esp_reset_reason_t r\)\s*\{(.*?)\n\}",
                      self.src, re.S)
        self.assertIsNotNone(m, "未找到 ShouldKeepPreviousLog")
        body = _strip_comments(m.group(1))
        self.assertNotIn("ESP_RST_POWERON", body)
        self.assertNotIn("default", body, "不得用 default 放行未知复位原因")

    def test_separator_written_through_ring_write(self):
        """分隔行必须走 RingWrite（不能调 ESP_LOGx/printf，会递归自锁）。"""
        self.assertIn("RingWrite(line, len)", self.init)
        self.assertNotIn("ESP_LOGI", self.init)
        self.assertIsNone(re.search(r"(?<![A-Za-z_])printf\s*\(", self.init))

    def test_ring_write_guards_uninitialized_head(self):
        """LogCaptureInit 之前的随机 head 必须归一化：越界索引 ring[] 会踩坏内存。

        日志钩子在 init 之前就已被安装/调用是可能的（全局构造、早期启动日志），
        而 .noinit 冷启动时是随机值 —— 少了这道防护会变成“莫名其妙的重启”。
        """
        m = re.search(r"void RingWrite\(.*?\n\}", self.src, re.S)
        self.assertIsNotNone(m, "未找到 RingWrite")
        body = m.group(0)
        self.assertIn("head >= kRingSize", body, "RingWrite 必须先校验 head 范围")
        self.assertIn("s_store.head = 0", body)

    def test_clear_api_exposed(self):
        self.assertIn("void LogCaptureClear()", self.src)
        self.assertIn("void LogCaptureClear();", self.header)

    def test_web_can_clear_device_buffer(self):
        server = _read(*BOARD_DIR, "http_upload_server.cc")
        web = _read(*BOARD_DIR, "web", "index.html")
        self.assertIn('strcmp(action, "log_clear")', server)
        self.assertIn("LogCaptureClear()", server)
        self.assertIn("clearDeviceLog", web)

    def test_fetch_buffer_keeps_one_byte_for_nul(self):
        """pull_buf 末字节要留给 NUL：传给 LogCapturePull 的长度必须 -1，否则原地清洗会越界。"""
        server = _read(*BOARD_DIR, "http_upload_server.cc")
        m = re.search(r'strcmp\(action, "log_pull"\) == 0\)\s*\{(.*?)else if', server, re.S)
        self.assertIsNotNone(m, "未找到 log_pull 分支")
        body = m.group(1)
        self.assertIn("sizeof(pull_buf) - 1", body)
        self.assertIn("SanitizeLogText(pull_buf, len)", body)

    def test_header_documents_panic_blind_spot(self):
        """头文件必须写清：panic/backtrace 不走钩子，网页看不到，必须接串口。

        这条最容易忘 —— 忘了就会反复去网页日志里找根本不存在的崩溃原因。
        """
        self.assertIn("panic", self.header.lower())
        self.assertIn("UART0", self.header)


if __name__ == "__main__":
    unittest.main()
