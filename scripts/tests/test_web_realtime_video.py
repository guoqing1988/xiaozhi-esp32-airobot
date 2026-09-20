"""契约测试：网页实时视频流（MJPEG over HTTP，官方标准做法）。

对应实现（分支 feature/realtime-video）：
  前端  main/boards/bread-compact-wifi-s3cam-airobot/web/index.html
  服务  .../local_video_stream.{h,cc}            官方 MJPEG 做法（独立 httpd，端口 81）
  板级  .../compact_wifi_board_s3cam_airobot.cc  相机模式按需切换（JPEG ↔ RGB565）
  通道  .../http_upload_server.{h,cc}            WS action + 帧率/分辨率推送
  驱动  main/boards/common/esp32_camera.{h,cc}   Reinit：运行时重配置相机

为什么值得测：这条功能最容易悄悄破坏的是「日常拍照」——
相机一旦忘了切回 RGB565，拍照就没有 LCD 预览了（用户明确要求“只有开视频时
才不预览”）。其次 MJPEG 是长连接，若把 handler 挂在主 httpd 上会把 WS
控制/日志一起拖死，也必须防回归。
"""

import os
import re
import unittest

BOARD = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "main", "boards", "bread-compact-wifi-s3cam-airobot",
)
PAGE = os.path.join(BOARD, "web", "index.html")
VIDEO_CC = os.path.join(BOARD, "local_video_stream.cc")
BOARD_CC = os.path.join(BOARD, "compact_wifi_board_s3cam_airobot.cc")
UPLOAD_CC = os.path.join(BOARD, "http_upload_server.cc")
UPLOAD_H = os.path.join(BOARD, "http_upload_server.h")
CAMERA_H = os.path.join(BOARD, "..", "common", "esp32_camera.h")


def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class _Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.html = _read(PAGE)
        cls.video = _read(VIDEO_CC)
        cls.board = _read(BOARD_CC)
        cls.upload_cc = _read(UPLOAD_CC)
        cls.upload_h = _read(UPLOAD_H)
        cls.camera_h = _read(CAMERA_H)


class TestFrontendPanel(_Base):
    """机器人面板：勾选框在摇杆上方，视频区与角标齐备。"""

    def test_checkbox_sits_above_joystick(self):
        """用户明确要求：勾选框在摇杆上方。"""
        self.assertLess(self.html.index('id="videoChk"'),
                        self.html.index('class="joy-wrap"'))

    def test_video_elements_exist_once(self):
        for el in ('id="videoChk"', 'id="videoWrap"', 'id="videoImg"', 'id="videoCorner"'):
            self.assertEqual(self.html.count(el), 1, f"{el} 应恰好出现一次")

    def test_video_area_hidden_by_default(self):
        """默认不显示视频区：不勾选就不该占地方，也不该去连流。"""
        idx = self.html.index('id="videoWrap"')
        self.assertIn('display:none', self.html[idx:idx + 200])

    def test_uncheck_closes_stream_and_tells_device(self):
        """取消勾选必须既断开 <img> 又通知设备停流（否则相机卡在 JPEG 模式）。"""
        m = re.search(r"function toggleVideo\(chk\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m, "找不到 toggleVideo")
        body = m.group(1)
        self.assertIn("video_stop", body, "取消勾选要通知设备停流")
        self.assertIn("videoClose()", body, "取消勾选要断开 <img> 长连接")

    def test_check_asks_device_first(self):
        """勾选先请求设备（切 JPEG + 起服务），成功后才设 <img src>。"""
        m = re.search(r"function toggleVideo\(chk\)\s*\{(.*?)\n    \}", self.html, re.S)
        body = m.group(1)
        self.assertIn("video_start", body)
        self.assertIn("videoOpen()", body)
        self.assertLess(body.index("video_start"), body.index("videoOpen()"))

    def test_close_removes_src(self):
        """断开 MJPEG 靠移除 src（浏览器随即关掉这个请求）。"""
        m = re.search(r"function videoClose\(\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIn("removeAttribute('src')", m.group(1))

    def test_ws_message_updates_corner(self):
        """设备推来的帧率/分辨率要更新到角标上。"""
        self.assertIn("if (j && j.video != null) { updateVideoStat(j); return; }", self.html)
        m = re.search(r"function updateVideoStat\(j\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m)
        self.assertIn("videoCorner", m.group(1))
        self.assertIn("fps", m.group(1))

    def test_leaving_panel_drops_stream(self):
        """切走面板就断开流（没人看时不占空口），但保留勾选以便切回自动重连。"""
        m = re.search(r"function showTab\(id, btn\)\s*\{(.*?)\n    \}", self.html, re.S)
        body = m.group(1)
        self.assertIn("videoClose()", body)
        self.assertIn("videoChk", body)

    def test_stream_url_uses_dedicated_port(self):
        """流走独立端口 81（主 httpd 的 80 要留给 WS 控制/日志）。"""
        self.assertIn(":81/stream", self.html)


class TestStreamServer(_Base):
    """设备侧 /stream：官方 MJPEG 做法，且没人看时不抓帧。"""

    def test_uses_standard_mjpeg_multipart(self):
        self.assertIn("multipart/x-mixed-replace;boundary=", self.video)

    def test_sends_frame_without_copy(self):
        """零拷贝：驱动帧缓冲直接交给发送。"""
        self.assertIn("reinterpret_cast<const char *>(fb->buf), fb->len", self.video)
        self.assertIn("esp_camera_fb_return(fb)", self.video)

    def test_runs_on_its_own_httpd(self):
        """长循环的 handler 不能挂在主 httpd 上（会拖死 WS 控制）。"""
        self.assertIn("constexpr int kStreamPort = 81;", self.video)
        self.assertIn("ctrl_port = kStreamCtrlPort", self.video.replace("cfg.ctrl_port", "ctrl_port"))

    def test_capture_happens_inside_handler_loop(self):
        """抓帧必须在 handler 循环里：没有观众连接时不会抓帧，不占射频/CPU。"""
        idx_loop = self.video.index("while (!s_stop)")
        idx_get = self.video.index("esp_camera_fb_get()")
        self.assertLess(idx_loop, idx_get, "抓帧应在流循环内，而不是启动时就一直抓")

    def test_single_viewer_only(self):
        """本板帧池只有一块，必须拒绝第二个观众。"""
        self.assertIn("compare_exchange_strong", self.video)
        self.assertIn("503 Service Unavailable", self.video)

    def test_refuses_non_jpeg_frames(self):
        """相机没切到 JPEG 时直接报错退出，避免白烧 CPU。"""
        self.assertIn("not JPEG", self.video)
        self.assertIn("PIXFORMAT_JPEG", self.video)

    def test_reports_fps_and_resolution(self):
        self.assertIn("VideoStatCallback", self.video)
        self.assertIn("esp_timer_get_time()", self.video)


class TestCameraModeSwitch(_Base):
    """按需切换：开视频才 JPEG，停了必须切回 RGB565（保住拍照的 LCD 预览）。"""

    def test_driver_gains_runtime_reinit(self):
        self.assertIn("bool Reinit(const camera_config_t &config);", self.camera_h)

    def test_start_switches_to_jpeg(self):
        m = re.search(r"std::string VideoStreamStart\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(m, "找不到 VideoStreamStart")
        self.assertIn("MakeCameraConfig(PIXFORMAT_JPEG)", m.group(1))

    def test_stop_restores_rgb565(self):
        """最关键的一条：停流必须切回 RGB565，否则拍照没有 LCD 预览。"""
        m = re.search(r"std::string VideoStreamStop\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(m, "找不到 VideoStreamStop")
        body = m.group(1)
        self.assertIn("LocalVideoStreamStop()", body)
        self.assertIn("MakeCameraConfig(PIXFORMAT_RGB565)", body, "停流必须切回 RGB565")

    def test_failure_paths_fall_back(self):
        """切 JPEG 或起服务失败都要退回原模式，不能把相机丢在未初始化状态。"""
        m = re.search(r"std::string VideoStreamStart\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertGreaterEqual(m.group(1).count("MakeCameraConfig(PIXFORMAT_RGB565)"), 2,
                                "两条失败路径都要回退 RGB565")

    def test_fb_count_stays_one(self):
        """fb_count=2 会多占 ~30KB DMA 内部 RAM，本板扛不住（见 esp32_camera.h 注释）。"""
        self.assertIn("config.fb_count = 1;", self.board)

    def test_photo_flip_settings_survive_switch(self):
        """Reinit 只套 Kconfig 默认翻转，用户 NVS 里的设置要重新生效。"""
        self.assertGreaterEqual(self.board.count("ApplyCameraFlip()"), 3)

    def test_close_page_stops_stream(self):
        """关页面/断网（WS 客户端归零）要自动停视频，别把相机留在 JPEG 模式。"""
        self.assertIn("count == 0 && LocalVideoStreamRunning()", self.board)


class TestWsWiring(_Base):
    """WS 通道：视频启停 action 与帧率推送。"""

    def test_actions_are_dispatched(self):
        for act in ("video_start", "video_stop"):
            self.assertIn(f'strcmp(action, "{act}")', self.upload_cc)

    def test_video_api_is_injectable(self):
        self.assertIn("struct VideoWebApi", self.upload_h)
        self.assertIn("void SetVideoWebApi(const VideoWebApi& api);", self.upload_h)
        self.assertIn("SetVideoWebApi({", self.board)

    def test_stat_push_exists_in_both_branches(self):
        """开/关 WS 支持两种编译分支都要有实现（否则关掉 WS 时链接失败）。"""
        self.assertIn("void WebNotifyVideoStat(float fps, int width, int height, bool running)",
                      self.upload_cc)
        self.assertIn("void WebNotifyVideoStat(float, int, int, bool) {}", self.upload_cc)

    def test_stat_push_reports_stop(self):
        """停止时也推一条（video=0），前端才能把角标收回。"""
        m = re.search(r"void WebNotifyVideoStat\(float fps, int width, int height, bool running\)\s*\{(.*?)\n\}",
                      self.upload_cc, re.S)
        self.assertIsNotNone(m)
        self.assertIn('"video"', m.group(1))


if __name__ == "__main__":
    unittest.main()
