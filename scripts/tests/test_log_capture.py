"""测试网页实时日志捕获（log_capture）的关键约束（回归防护）。

背景：本板 console 与 Arduino 下位机控制指令**共用 UART0(GPIO43)**。为在"不断开
Arduino 接线"的前提下看实时日志，log_capture 用 esp_log_set_vprintf() 接管 ESP_LOGx，
写进内部 RAM 环形缓冲供网页拉取，**默认不落 UART0**。

这里固化三条最容易写错、且一写错就会出事的约束：
1. 钩子默认不得调用原 vprintf —— 否则日志照样灌给 Arduino，污染指令流；
2. 钩子内不得出现 ESP_LOGx/printf —— 会递归自锁；
3. 环形缓冲必须是静态内部 RAM、不能动态分配 —— 临界区内访问 PSRAM 会长时间持锁
   （见 sdkconfig.defaults 里关于中断看门狗的说明）。
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
    """去掉 C/C++ 注释，避免把注释里的说明文字当成真实调用。"""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


class TestLogCapture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = _read(*BOARD_DIR, "log_capture.cc")
        cls.header = _read(*BOARD_DIR, "log_capture.h")
        cls.board = _read(*BOARD_DIR, "compact_wifi_board_s3cam_airobot.cc")
        cls.server = _read(*BOARD_DIR, "http_upload_server.cc")
        cls.web = _read(*BOARD_DIR, "web", "index.html")
        m = re.search(
            r"int LogVprintfHook\(const char\* fmt, va_list args\)\s*\{(.*?)\n\}",
            cls.src, re.S,
        )
        assert m is not None, "未找到 LogVprintfHook 定义"
        cls.hook = _strip_comments(m.group(1))

    def test_uses_esp_log_set_vprintf(self):
        """必须用官方 vprintf 钩子接管日志，否则拿不到 ESP_LOGx 输出。"""
        self.assertIn("esp_log_set_vprintf", self.src)
        self.assertIn("esp_log_set_vprintf(LogVprintfHook)", self.board and self.src)

    def test_init_is_idempotent(self):
        """重复调用不能二次包装（否则原 vprintf 链会被套多层）。"""
        m = re.search(r"void LogCaptureInit\(\)\s*\{(.*?)\n\}", self.src, re.S)
        self.assertIsNotNone(m, "未找到 LogCaptureInit 定义")
        body = _strip_comments(m.group(1))
        self.assertIn("s_orig_vprintf", body)
        self.assertIn("return", body)

    def test_hook_gates_uart_output_behind_flag(self):
        """默认不得写 UART0：原 vprintf 的调用必须被 s_uart_mirror 开关守住。

        若去掉这个开关，日志会重新灌进 Arduino 的 RX，正好是本次要解决的问题。
        """
        self.assertIn("s_uart_mirror", self.hook, "钩子必须用 mirror 开关决定是否落串口")
        self.assertIn("s_orig_vprintf", self.hook, "镜像时必须转发给原 vprintf")
        gate = self.hook.index("s_uart_mirror")
        call = self.hook.index("s_orig_vprintf(fmt, args)")
        self.assertLess(gate, call, "原 vprintf 必须在 mirror 判断之后才调用")

    def test_hook_does_not_log_again(self):
        """钩子内不得出现 ESP_LOGx/printf —— 日志系统内部再打日志会递归自锁。"""
        for bad in ("ESP_LOGE", "ESP_LOGW", "ESP_LOGI", "ESP_LOGD", "ESP_LOGV",
                    "puts(", "fputs("):
            self.assertNotIn(bad, self.hook, f"钩子内不得调用 {bad}")
        # 词边界：否则 vsnprintf/snprintf 会被误判为 printf
        self.assertIsNone(re.search(r"(?<![A-Za-z_])printf\s*\(", self.hook),
                          "钩子内不得调用 printf(（vsnprintf 除外）")

    def test_ring_buffer_is_static(self):
        """环形缓冲必须是静态数组（内部 RAM），临界区内不能碰堆/PSRAM。"""
        self.assertIn("char s_ring[kRingSize];", self.src)
        body = _strip_comments(self.src)
        for bad in ("malloc(", "heap_caps_malloc(", "new ", "calloc("):
            self.assertNotIn(bad, body, f"日志缓冲不得动态分配（{bad}）")
        # 大小必须是编译期常量（供静态数组使用）
        self.assertIn("constexpr size_t kRingSize", self.src)

    def test_pull_memcpy_stays_out_of_allocation(self):
        """拉取接口用调用方提供的缓冲，临界区内只做 memcpy。"""
        self.assertIn("void LogCapturePull(uint32_t since_seq, char* buf, size_t buf_size",
                      self.header)
        self.assertIn("memcpy(buf", self.src)

    def test_board_installs_capture_early(self):
        """板级必须在初始化最前面装钩子，否则启动日志全丢。"""
        ctor = re.search(
            r"CompactWifiBoardS3CamAirobot\(\)\s*:.*?\{(.*?)InitializeSpi\(\);",
            self.board, re.S,
        )
        self.assertIsNotNone(ctor, "未找到板级构造函数")
        body = _strip_comments(ctor.group(1)).strip()
        self.assertTrue(
            body.startswith("LogCaptureInit();"),
            f"LogCaptureInit() 必须是构造函数第一条语句，实际为: {body[:60]!r}",
        )


    def test_append_reuses_ring_write(self):
        """LogCaptureAppend 必须复用 RingWrite，不能自己再拄一份临界区代码。"""
        self.assertIn("void RingWrite(const char* data, size_t len)", self.src)
        m = re.search(r"void LogCaptureAppend\(const char\* fmt, \.\.\.\)\s*\{(.*?)\n\}",
                      self.src, re.S)
        self.assertIsNotNone(m, "未找到 LogCaptureAppend 定义")
        body = _strip_comments(m.group(1))
        self.assertIn("RingWrite(line, len)", body)
        self.assertNotIn("portENTER_CRITICAL", body, "临界区应封在 RingWrite 里")


class TestLogWebApi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = _read(*BOARD_DIR, "http_upload_server.cc")
        cls.web = _read(*BOARD_DIR, "web", "index.html")

    def test_ws_actions_exist(self):
        """日志面板依赖三个 WS action。"""
        for action in ("log_pull", "log_level", "log_mirror"):
            self.assertIn(f'"{action}"', self.server, f"缺少 WS action {action}")

    def test_pull_returns_mirror_and_level(self):
        """回执要带 mirror/level/more，前端才能回填控件并决定是否继续追平。"""
        m = re.search(r'strcmp\(action, "log_pull"\) == 0\)\s*\{(.*?)else if', self.server, re.S)
        self.assertIsNotNone(m, "未找到 log_pull 分支")
        body = m.group(1)
        self.assertIn('"seq"', body)
        self.assertIn('"text"', body)
        self.assertIn('"mirror"', body)
        self.assertIn('"level"', body)
        self.assertIn('"more"', body)

    def test_pull_reports_delivered_seq_not_total(self):
        """next_seq 必须报到“已交付到哪”，否则一次装不下时前端会误以为已经追平、丢掉中间日志。"""
        m = re.search(
            r"void LogCapturePull\(.*?\)\s*\{(.*?)\n\}",
            _read(*BOARD_DIR, "log_capture.cc"), re.S,
        )
        self.assertIsNotNone(m, "未找到 LogCapturePull 定义")
        body = _strip_comments(m.group(1))
        self.assertIn("next_seq = start +", body)

    def test_pull_sanitizes_text(self):
        """日志可能混入二进制：必须清洗成合法 UTF-8，否则前端 JSON.parse 整批失败。"""
        self.assertIn("SanitizeLogText", self.server)

    def test_level_is_clamped(self):
        """级别必须钳在 0-4，越界传给 esp_log_level_set 行为未定义。"""
        m = re.search(r'strcmp\(action, "log_level"\) == 0\)\s*\{(.*?)else if', self.server, re.S)
        self.assertIsNotNone(m, "未找到 log_level 分支")
        body = m.group(1)
        self.assertIn("lv < 0", body)
        self.assertIn("lv > 4", body)

    def test_web_polls_log(self):
        """前端要按 seq 增量拉取，且只在面板可见时轮询。"""
        self.assertIn("log_pull", self.web)
        self.assertIn("startLogPolling", self.web)
        self.assertIn("stopLogPolling", self.web)
        m = re.search(r"function showTab\(id, btn\)\s*\{(.*?)\n    \}", self.web, re.S)
        self.assertIsNotNone(m, "未找到 showTab 定义")
        body = m.group(1)
        self.assertIn("startLogPolling()", body, "切入日志面板要启动轮询")
        self.assertIn("stopLogPolling()", body, "切走要停止轮询，避免无谓流量")

    def test_web_log_lives_in_uno_panel(self):
        """日志区不能再占顶部 tab，要挂在「机器人控制」面板里（折叠展示）。"""
        self.assertNotIn('id="panel-log"', self.web, "顶部不应再有独立的日志面板")
        self.assertNotIn("showTab('log'", self.web, "顶部不应再有日志 tab 按钮")
        for el in ('id="logView"', 'id="logLevel"', 'id="logMirror"', 'id="logPauseBtn"'):
            self.assertIn(el, self.web, f"日志控件 {el} 必须保留（只是换了位置）")
        # 位置校验：日志区必须排在 panel-uno 之后（即在该面板内部）
        self.assertGreater(
            self.web.index('id="logView"'),
            self.web.index('id="panel-uno"'),
            "日志区应在「机器人控制」面板内部",
        )
        # 默认收起，否则机器人面板会变得很长
        self.assertIn('<details class="logbox">', self.web)


class TestUnoCommandTrace(unittest.TestCase):
    """下位机指令收发要能在网页上看到（定位“控制没反应”的关键证据）。"""

    @classmethod
    def setUpClass(cls):
        cls.board = _read(*BOARD_DIR, "compact_wifi_board_s3cam_airobot.cc")
        cls.web = _read(*BOARD_DIR, "web", "index.html")
        cls.log_h = _read(*BOARD_DIR, "log_capture.h")

    def test_header_documents_format(self):
        for tag in ("[UNO] >", "[UNO] !", "[UNO] <"):
            self.assertIn(tag, self.log_h, f"头文件应说明 {tag} 的含义")

    def test_tx_is_recorded(self):
        """实际发出的指令要记录。"""
        m = re.search(r"static std::string SendUartMessage\((.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(m, "未找到 SendUartMessage")
        body = m.group(1)
        self.assertIn('LogCaptureAppend("[UNO] > @%s\\n", command_str)', body)

    def test_debounced_and_failed_are_recorded(self):
        """防抖丢弃/发送失败必须也有痕迹，否则分不清“被拦”和“没发”。"""
        m = re.search(r"static std::string SendUartMessage\((.*?)\n    \}", self.board, re.S)
        body = m.group(1)
        self.assertIn("[UNO] !", body, "防抖丢弃要单独标记（用 ! 而不是 >）")
        self.assertIn("[UNO] x", body, "发送失败要标记")

    def test_rx_is_recorded(self):
        """下位机回执（@busy/@done/@stat）要记录，才能判断 Arduino 是否真执行了。"""
        m = re.search(r"void UnoStatusLoop\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(m, "未找到 UnoStatusLoop")
        body = m.group(1)
        self.assertEqual(body.count('LogCaptureAppend("[UNO] < %s\\n", tok)'), 3,
                         "@busy/@stat/@done 三个分支都要记录回执")

    def test_web_shows_uno_commands(self):
        self.assertIn('id="unoCmdView"', self.web)
        self.assertIn("function feedUnoCmd", self.web)
        self.assertIn("UNO_CMD_MAX_CHARS", self.web)
        # 按行筛选：只留 [UNO] 开头的行
        self.assertIn("l.indexOf('[UNO]') === 0", self.web)

    def test_uno_panel_starts_log_polling(self):
        """日志轮询要跟着「机器人控制」面板的显隐启停，否则第一次进去指令区是空的。"""
        m = re.search(r"function showTab\(id, btn\)\s*\{(.*?)\n    \}", self.web, re.S)
        self.assertIsNotNone(m, "未找到 showTab 定义")
        body = m.group(1)
        self.assertIn("if (id === 'uno') startLogPolling();", body)

    def test_clear_clears_both(self):
        m = re.search(r"function clearLogView\(\)\s*\{(.*?)\n    \}", self.web, re.S)
        self.assertIsNotNone(m, "未找到 clearLogView 定义")
        body = m.group(1)
        self.assertIn("unoCmdText = ''", body)
        self.assertIn("unoCmdPending = ''", body)


if __name__ == "__main__":
    unittest.main()
