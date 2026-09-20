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
        """已废弃：改为先出框再问设备（见 test_check_shows_frame_before_asking_device）。"""
        raise unittest.SkipTest("需求已改为：勾选立即出框，框里提示不可用")

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

    def test_gear_button_and_config_modal(self):
        """视频区左上有 ⚙️、右上角是帧率角标；点 ⚙️ 弹出参数弹窗。"""
        self.assertIn("openVideoCfg()", self.html)
        for el in ('id="videoCfgModal"', 'id="vcfgSize"', 'id="vcfgFps"', 'id="vcfgQuality"',
                   'id="vcfgMirror"', 'id="vcfgFlip"'):
            self.assertEqual(self.html.count(el), 1, f"{el} 应恰好出现一次")
        self.assertIn("top:6px;left:6px", self.html, "⚙️ 应在左上角")
        self.assertIn("top:6px;right:6px", self.html, "帧率角标应在右上角")

    def test_gear_on_video_corner(self):
        """画面右上角是 ⚙️、左上角是帧率角标（用户要求：设置放右边、帧率放左边）。"""
        wrap = self.html.index('id="videoWrap"')
        corner = self.html.index('id="videoCorner"')
        gear = self.html.index('onclick="openVideoCfg()"')
        self.assertGreater(corner, wrap, "角标应在视频框内")
        self.assertGreater(gear, wrap, "⚙️ 应在视频框内")
        self.assertLess(corner, gear, "帧率角标（左上）应排在 ⚙️（右上）之前")
        self.assertIn("top:6px;left:6px", self.html[corner:corner + 260], "角标在左上")
        self.assertIn("top:6px;right:6px", self.html[gear:gear + 260], "⚙️ 在右上")

    def test_gear_is_clickable(self):
        """提示层铺满整个框，必须不拦点击，否则 ⚙️ 点不动。"""
        self.assertIn("pointer-events:none", self.html, "提示层不能拦截点击")
        self.assertIn("z-index:2", self.html, "⚙️ 要压在提示层之上")

    def test_corner_has_default_fps(self):
        """帧率角标默认就显示 0fps（不能空白，否则只剩一条小黑条）。"""
        corner = self.html.index('id="videoCorner"')
        self.assertIn(">0fps</span>", self.html[corner:corner + 400], "角标要有默认值")

    def test_video_frame_has_background(self):
        """视频框要有深色背景（否则没画面时一片白，看不出是视频区）。"""
        wrap = self.html.index('id="videoWrap"')
        self.assertIn("background:#111", self.html[wrap:wrap + 200])

    def test_check_shows_frame_before_asking_device(self):
        """勾选就立刻出框（没连设备也出），不能等设备回应、更不能失败就把勾去掉。"""
        m = re.search(r"function toggleVideo\(chk\)\s*\{(.*?)\n    \}", self.html, re.S)
        body = m.group(1)
        self.assertLess(body.index("videoOpen()"), body.index("'video_start'"), "先出框再问设备")
        self.assertNotIn("alert(", body, "不该弹 alert，框里提示就够")
        self.assertNotIn("chk.checked = false", body, "设备没连不该把勾去掉")

    def test_offline_hint_layer(self):
        """连不上时用画面上的提示层说明“接口不可用”（不是空白也不是破图）。"""
        self.assertIn('id="videoHint"', self.html)
        self.assertIn("videoSetHint(", self.html)
        self.assertIsNotNone(re.search(r"img\.addEventListener\('error'", self.html),
                             "要监听 <img> 加载失败，挡住浏览器破图图标")

    def test_modal_opens_offline_with_cached_values(self):
        """没连设备也要能打开弹窗并看到上次的值（用 localStorage 缓存兜底）。"""
        m = re.search(r"function openVideoCfg\(\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m, "找不到 openVideoCfg")
        body = m.group(1)
        self.assertLess(body.index("classList.add('show')"), body.index("wsSend("),
                        "先把窗口显示出来，再去问设备")
        self.assertIn("videoCfgCacheGet()", body)
        self.assertIn("localStorage", self.html, "要用浏览器缓存兜底离线显示")

    def test_apply_caches_values_locally(self):
        """点应用就写本地缓存（即使设备没连上，下次打开也能看到刚设的值）。"""
        m = re.search(r"function applyVideoCfg\(size, fps, quality, flip\)\s*\{(.*?)\n    \}",
                      self.html, re.S)
        self.assertIsNotNone(m, "找不到 applyVideoCfg")
        body = m.group(1)
        self.assertIn("videoCfgCacheSet(", body)
        self.assertIn("flip", body)

    def test_apply_reconnects_stream(self):
        """改了尺寸/质量会重启流，前端必须重连 <img>（否则看不到新画面）。"""
        m = re.search(r"function applyVideoCfg\(size, fps, quality, flip\)\s*\{(.*?)\n    \}",
                      self.html, re.S)
        self.assertIsNotNone(m, "找不到 applyVideoCfg")
        body = m.group(1)
        self.assertIn("removeAttribute('src')", body)
        self.assertIn("videoStreamUrl()", body)

    def test_modal_reads_current_cfg(self):
        """打开弹窗要先从设备读当前参数（否则显示的是浏览器默认值）。"""
        m = re.search(r"function openVideoCfg\(\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIn("video_cfg_get", m.group(1))


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

    def test_low_latency_tuning(self):
        """用途是 FPV 遥控（第一视角），延时优先：三处优化别被改回去。

        1. 帧率不能像“与 ESP-NOW 共存”时那样压到 12（太顿）
        2. 必须禁用 Nagle，否则每帧白等几十毫秒
        3. 取帧必须是“只要最新帧”，否则旧帧排队会把延时越拖越长
        """
        m = re.search(r"std::atomic<int> s_target_fps\{(\d+)\}", self.video)
        self.assertIsNotNone(m, "找不到 s_target_fps")
        self.assertGreaterEqual(int(m.group(1)), 15, "遥控用途帧率不应低于 15fps")
        self.assertIn("TCP_NODELAY", self.video, "禁用 Nagle 是低延时的硬要求")
        self.assertIn("CAMERA_GRAB_LATEST", self.board, "视频取帧要用“只要最新帧”")

    def test_send_timeout_is_bounded(self):
        """慢客户端不能把发送任务永久占住（否则整个流卡死）。"""
        self.assertIn("cfg.send_wait_timeout", self.video)


class TestCameraModeSwitch(_Base):
    """按需切换：开视频才 JPEG，停了必须切回 RGB565（保住拍照的 LCD 预览）。"""

    def test_driver_gains_runtime_reinit(self):
        self.assertIn("bool Reinit(const camera_config_t &config);", self.camera_h)

    def test_start_switches_to_jpeg(self):
        m = re.search(r"std::string VideoStreamStart\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(m, "找不到 VideoStreamStart")
        self.assertIn("MakeCameraConfig(PIXFORMAT_JPEG, CAMERA_GRAB_LATEST,", m.group(1),
                      "视频模式取帧要用“只要最新帧”，否则旧帧排队把延时拖上去")

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

    def test_cfg_persisted_in_nvs(self):
        """参数要存进 NVS：改一次永久有效，不用重烧固件。"""
        self.assertIn('Settings s("video"', self.board)
        for key in ('"size"', '"fps"', '"quality"'):
            self.assertIn(f"s.SetInt({key}", self.board)
            self.assertIn(f"s.GetInt({key}", self.board)

    def test_cfg_loaded_at_boot(self):
        m = re.search(r"void InitializeCamera\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(m, "找不到 InitializeCamera")
        self.assertIn("LoadVideoCfg()", m.group(1), "开机要读回保存的视频参数")

    def test_fps_applies_instantly(self):
        """帧率是运行时变量：改完下一帧就生效，不用重启相机/流。"""
        self.assertIn("void LocalVideoStreamSetFps(int fps)", self.video)
        self.assertIn("s_target_fps = fps", self.video)
        self.assertIn("s_target_fps.load()", self.video)

    def test_size_and_quality_reach_camera(self):
        """尺寸/质量要真的传进相机配置（否则弹窗改了也不生效）。"""
        m = re.search(r"std::string VideoStreamStart\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        body = m.group(1)
        self.assertIn("VideoFrameSizeOf(video_cfg_.size)", body)
        self.assertIn("video_cfg_.quality", body)

    def test_quality_change_does_not_reinit_camera(self):
        """改质量不该重启相机（不白闪 0.3 秒）。

        依据驱动源码：
          - cam_hal.c cam_config()：fb_size = width × height × 2  → 跟分辨率走，与质量无关
          - ov2640.c set_quality()：仅 write_reg(sensor, BANK_DSP, QS, quality)
        所以质量能运行时动态改，只有分辨率变了才必须 deinit + init。
        """
        m = re.search(r"std::string VideoCfgApply\(int size, int fps, int quality, int flip\)\s*\{(.*?)\n    \}",
                      self.board, re.S)
        self.assertIsNotNone(m, "找不到带 flip 的 VideoCfgApply")
        body = m.group(1)
        self.assertIn("const bool size_changed", body, "重建条件只看尺寸")
        self.assertNotIn("quality != video_cfg_.quality) ||", body, "质量不能进重建条件")
        self.assertIn("set_quality", body, "质量要动态写进 sensor")
        self.assertIn("if (!size_changed || !LocalVideoStreamRunning())", body,
                      "只有尺寸变化才重启流")

    def test_flip_shares_nvs_with_ai_tool(self):
        """镜像/翻转要复用 AI 工具那份 NVS（camera/flip），否则两套配置会打架。"""
        m = re.search(r"void SetCameraFlip\(int mode\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(m, "找不到 SetCameraFlip")
        body = m.group(1)
        self.assertIn('Settings s("camera", true)', body)
        self.assertIn('s.SetInt("flip", mode)', body)
        self.assertIn("SetHMirror(mode & 1)", body)
        self.assertIn("SetVFlip((mode & 2) != 0)", body)

    def test_flip_exposed_and_applied(self):
        """flip 要能读出来（弹窗回显）且保存时会应用（立即生效）。"""
        self.assertIn("GetCameraFlip()", self.board)
        m = re.search(r"std::string VideoCfgApply\(int size, int fps, int quality, int flip\)\s*\{(.*?)\n    \}",
                      self.board, re.S)
        self.assertIsNotNone(m, "找不到带 flip 的 VideoCfgApply")
        self.assertIn("SetCameraFlip(flip)", m.group(1))


class TestWsWiring(_Base):
    """WS 通道：视频启停 action 与帧率推送。"""

    def test_actions_are_dispatched(self):
        for act in ("video_start", "video_stop", "video_cfg_get", "video_cfg_set"):
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
