"""契约测试：网页实时视频流（MJPEG over HTTP，官方标准做法）。

对应实现（分支 feature/realtime-video）：
  前端  main/boards/bread-compact-wifi-s3cam-airobot/web/index.html
  服务  .../local_video_stream.{h,cc}            官方 MJPEG 做法（独立 httpd，端口 81）
  板级  .../compact_wifi_board_s3cam_airobot.cc  相机全程单一 JPEG 模式（不再 deinit/Reinit）
  通道  .../http_upload_server.{h,cc}            WS action + 帧率/分辨率推送
  驱动  main/boards/common/esp32_camera.{h,cc}   JPEG 直通 + esp_jpeg 解码 LCD 预览

为什么值得测：这条功能最容易悄悄破坏的是「日常拍照」——
用户明确要求「只有开视频时才不预览」，且相机一旦 deinit 过一次就再也 init 不回来
（本板内部 SRAM 最大连续块 ~12800 < JPEG 16384 < RGB565 30720，见 README 踩坑 22），
所以“切模式”这件事本身必须是禁忌。其次 MJPEG 是长连接，若把 handler 挂在主 httpd 上
会把 WS 控制/日志一起拖死，也必须防回归。
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
LOCAL_PHOTO_CC = os.path.join(BOARD, "local_photo.cc")
BOARD_CC = os.path.join(BOARD, "compact_wifi_board_s3cam_airobot.cc")
UPLOAD_CC = os.path.join(BOARD, "http_upload_server.cc")
UPLOAD_H = os.path.join(BOARD, "http_upload_server.h")
CAMERA_H = os.path.join(BOARD, "..", "common", "esp32_camera.h")
CAMERA_CC = os.path.join(BOARD, "..", "common", "esp32_camera.cc")
CONFIG_JSON = os.path.join(BOARD, "config.json")
IMAGE_TO_JPEG = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "main", "display", "lvgl_display", "jpg", "image_to_jpeg.cpp",
)


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
        cls.camera_cc = _read(CAMERA_CC)
        cls.config_json = _read(CONFIG_JSON)
        cls.image_to_jpeg = _read(IMAGE_TO_JPEG)


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

    def _css_rule(self, selector):
        """取某条 CSS 规则的内容（⚙️ 的定位/层级已从内联样式挪到 .video-gear 类里 ——
        内联样式优先级更高，会把 :hover 盖掉，见 test_web_ui_affordance.py）。"""
        m = re.search(re.escape(selector) + r"\s*\{(.*?)\}", self.html, re.S)
        self.assertIsNotNone(m, f"找不到 CSS 规则 {selector}")
        return m.group(1)

    def test_gear_button_and_config_modal(self):
        """视频区左上是帧率角标、右上是 ⚙️；点 ⚙️ 弹出参数弹窗。"""
        self.assertIn("openVideoCfg()", self.html)
        for el in ('id="videoCfgModal"', 'id="vcfgSize"', 'id="vcfgFps"', 'id="vcfgQuality"',
                   'id="vcfgMirror"', 'id="vcfgFlip"'):
            self.assertEqual(self.html.count(el), 1, f"{el} 应恰好出现一次")
        self.assertIn("top:6px;left:6px", self.html, "帧率角标应在左上角")
        gear = self._css_rule(".video-gear")
        self.assertIn("top: 6px", gear, "⚙️ 应在右上角")
        self.assertIn("right: 6px", gear, "⚙️ 应在右上角")

    def test_gear_on_video_corner(self):
        """画面右上角是 ⚙️、左上角是帧率角标（用户要求：设置放右边、帧率放左边）。"""
        wrap = self.html.index('id="videoWrap"')
        corner = self.html.index('id="videoCorner"')
        gear = self.html.index('class="video-gear"')
        self.assertGreater(corner, wrap, "角标应在视频框内")
        self.assertGreater(gear, wrap, "⚙️ 应在视频框内")
        self.assertLess(corner, gear, "帧率角标（左上）应排在 ⚙️（右上）之前")
        self.assertIn("top:6px;left:6px", self.html[corner:corner + 260], "角标在左上")
        rule = self._css_rule(".video-gear")
        self.assertIn("top: 6px", rule, "⚙️ 在右上")
        self.assertIn("right: 6px", rule, "⚙️ 在右上")

    def test_gear_is_clickable(self):
        """提示层铺满整个框，必须不拦点击，否则 ⚙️ 点不动。"""
        self.assertIn("pointer-events:none", self.html, "提示层不能拦截点击")
        self.assertIn("z-index: 2", self._css_rule(".video-gear"), "⚙️ 要压在提示层之上")

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
        self.assertLess(body.index("videoShowBox()"), body.index("'video_start'"), "先出框再问设备")
        self.assertNotIn("alert(", body, "不该弹 alert，框里提示就够")
        self.assertNotIn("chk.checked = false", body, "设备没连不该把勾去掉")

    def test_img_src_set_only_after_device_ready(self):
        """连流必须等设备端 /stream 就绪（video_start 返回 ok）之后。

        反例（曾经的真 bug）：在 video_start 之前就设 <img src>，此时设备端 81 端口
        还没监听，浏览器立刻触发 error → 首次勾选必现“接口不可用”，切走再切回
        （切走会 removeAttribute('src')）才正常。
        """
        m = re.search(r"function toggleVideo\(chk\)\s*\{(.*?)\n    \}", self.html, re.S)
        body = m.group(1)
        self.assertIn("videoOpen()", body, "video_start 成功后要真的去连流")
        self.assertGreater(body.index("videoOpen()"), body.index("'video_start'"),
                           "videoOpen()（设 src）必须在请求 video_start 之后")
        self.assertNotIn("img.src", body, "toggleVideo 不该绕过 videoOpen 直接设 src")

    def test_show_box_does_not_open_stream(self):
        """出框（videoShowBox）只负责显示，不设 src —— 否则又变回“先连流后起服务”。"""
        m = re.search(r"function videoShowBox\(\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m, "找不到 videoShowBox")
        self.assertNotIn("img.src", m.group(1), "videoShowBox 不能设 src")
        self.assertIn("videoSetHint('')", m.group(1))

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


class TestSingleCameraMode(_Base):
    """相机全程只 init 一次（单一 JPEG 模式），不再 deinit/Reinit。

    真机日志（本板 README 踩坑 22）：RGB565 拍照要 30720 字节**连续**内部 SRAM，
    JPEG 推流要 16384，而本板实测最大连续块只有 ~12800，且开过一次视频后再也不回升。
    于是只要 deinit 过一次，两个模式就都 init 不回来（网页拍照 500 + 视频也起不来，
    只能重启）。修法：开机按 JPEG 初始化一次，之后切分辨率/质量只写 sensor 寄存器
    （各一次 I2C 写，零内存分配）；拍照走 JPEG 直通 + esp_jpeg 解码出 LCD 预览，
    所以停流也不必切回 RGB565，预览也不会丢。
    """

    def test_driver_gains_runtime_reinit(self):
        """Reinit 接口保留（其它板可能用），但本板已不再调用。"""
        self.assertIn("bool Reinit(const camera_config_t &config);", self.camera_h)

    def test_board_never_reinits_camera(self):
        """最关键的一条：板级不得再调 Reinit —— 一次 deinit 就够毁掉整块相机。"""
        self.assertNotIn("->Reinit(", self.board, "相机不再切换像素格式（见类注释）")
        self.assertNotIn("MakeCameraConfig(PIXFORMAT_RGB565", self.board,
                         "本板不再用 RGB565：它的 DMA 要 30720 连续字节，本板给不出来")

    def test_init_once_in_jpeg_at_max_frame(self):
        """开机只 init 一次：JPEG + 「只要最新帧」+ 最大档（帧缓冲按最大档分配）。"""
        m = re.search(r"void InitializeCamera\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(m, "找不到 InitializeCamera")
        body = m.group(1)
        self.assertIn("MakeCameraConfig(PIXFORMAT_JPEG, CAMERA_GRAB_LATEST,", body,
                      "视频取帧要「只要最新帧」，否则旧帧排队把延时拖上去")
        self.assertIn("kVideoMaxFrameSize", body, "按最大档初始化，帧缓冲才够切分辨率")
        self.assertEqual(body.count("MakeCameraConfig("), 1, "只允许一处 init")
        self.assertIn("ApplyPhotoSensorParams()", body, "初始化完就按拍照参数待命")

    def test_start_only_writes_sensor(self):
        """开流不再重建相机，只写 sensor 寄存器。"""
        m = re.search(r"std::string VideoStreamStart\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(m, "找不到 VideoStreamStart")
        body = m.group(1)
        self.assertIn("ApplyVideoSensorParams()", body, "开流只写 sensor 寄存器")
        self.assertNotIn("MakeCameraConfig", body, "开流不再重建相机")
        self.assertNotIn("Reinit", body)

    def test_stop_restores_photo_params(self):
        """停流只需把 sensor 写回拍照参数（VGA + 质量 12）：与旧版「切回 RGB565」画面一致。"""
        m = re.search(r"std::string VideoStreamStop\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(m, "找不到 VideoStreamStop")
        body = m.group(1)
        self.assertIn("LocalVideoStreamStop()", body)
        self.assertIn("ApplyPhotoSensorParams()", body, "停流要恢复拍照分辨率/质量")
        self.assertNotIn("Reinit", body)

    def test_failure_path_keeps_camera_alive(self):
        """起流失败只回退拍照参数，不重建相机（重建必然失败，见类注释）。"""
        m = re.search(r"std::string VideoStreamStart\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIn("ApplyPhotoSensorParams()", m.group(1), "失败路径要回退拍照参数")
        self.assertNotIn("Reinit", m.group(1))

    def test_sensor_params_helpers_cover_size_and_quality(self):
        """尺寸/质量都要真的写进 sensor（否则弹窗改了不生效）；拍照档与旧行为一致。"""
        v = re.search(r"void ApplyVideoSensorParams\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(v, "找不到 ApplyVideoSensorParams")
        self.assertIn("set_framesize", v.group(1))
        self.assertIn("VideoFrameSizeOf(video_cfg_.size)", v.group(1))
        self.assertIn("set_quality", v.group(1))
        self.assertIn("video_cfg_.quality", v.group(1))
        p = re.search(r"void ApplyPhotoSensorParams\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(p, "找不到 ApplyPhotoSensorParams")
        self.assertIn("set_framesize", p.group(1))
        self.assertIn("kPhotoFrameSize", p.group(1))
        self.assertIn("set_quality", p.group(1))
        self.assertIn("kPhotoQuality", p.group(1))
        self.assertRegex(self.board, r"static constexpr framesize_t kPhotoFrameSize = FRAMESIZE_VGA")
        self.assertRegex(self.board, r"static constexpr int kPhotoQuality = 12")

    def test_photo_uses_jpeg_passthrough(self):
        """拍照不再软件编码：JPEG 直接透传，且**靠上游自带开关**，不手写直通代码。

        本板只因“相机直出 JPEG”才需要直通：上游 image_to_jpeg.cpp 已内置
        `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT` 分支（cb(0,整张)+cb(1,哨兵)，
        与软件编码回调形状完全一致），开它即可 —— 比在 Explain/MakeConfig 里手写直通
        少改两个上游函数（合并官方代码时冲突面最小）。
        """
        # 1) 两个变体都要开这个开关，否则 build.py 生成的 sdkconfig 里没它 → 拍照变 500
        self.assertEqual(self.config_json.count('"CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y"'), 2,
                         "两个 build 变体都要开：不然 JPEG 帧会被当成原始像素去编码")
        # 2) 上游的直通能力必须在（开关打开后就是这条路径）
        self.assertIn("CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT", self.image_to_jpeg)
        self.assertRegex(self.image_to_jpeg,
                         r"if \(format == V4L2_PIX_FMT_JPEG\) \{\s*\n\s*cb\(arg, 0, src, src_len\);")
        # 3) 相机侧只做格式映射，不得再手写直通（否则又变成“改上游函数”）
        self.assertRegex(self.camera_cc, r"case PIXFORMAT_JPEG:\s*\n(?:\s*//.*\n)*\s*enc_fmt = V4L2_PIX_FMT_JPEG;")
        self.assertNotIn("JpegEncodeCb(&ctx, 0,", self.camera_cc, "不要手写直通，交给上游开关")
        self.assertIn("bool ok = image_to_jpeg_cb(", self.camera_cc, "Explain 的编码流程保持上游原样")

    def test_jpeg_frame_still_gets_lcd_preview(self):
        """用户明确要求保留 LCD 预览：JPEG 帧要解码成 RGB565 再给 LVGL。

        实现刻意全放在文件头的 DecodeJpegPreview()（本项目新增区），
        Capture() 里只留一行调用 —— 上游函数只差那一行，合并好解。
        """
        self.assertRegex(self.camera_cc, r"void DecodeJpegPreview\(", "解码要单独成函数")
        body = re.search(r"void DecodeJpegPreview\(.*?\n\}", self.camera_cc, re.S)
        self.assertIsNotNone(body)
        body = body.group(0)
        self.assertIn("esp_jpeg_decode", body, "必须真的解码，不能只打印日志")
        self.assertIn("esp_jpeg_get_image_info", body,
                      "先读 JPEG 头问真实尺寸——不能拿 fb->width 算缓冲"
                      "（刚改过 framesize 时这一帧可能还是旧尺寸，会写越界）")
        self.assertIn("cfg.outbuf_size = info.output_len", body, "把真实缓冲大小交给解码器")
        self.assertRegex(body, r"static uint8_t work\[\d+\]", "解码草稿纸要用静态缓冲，不占内部堆")
        self.assertIn("working_buffer = work", body, "草稿纸要交给解码器，避免它自己临时 malloc")
        self.assertIn("MALLOC_CAP_SPIRAM", body, "解码输出必须落 PSRAM")
        self.assertIn("LV_COLOR_FORMAT_RGB565", body)
        # Capture() 的 JPEG 分支只能是一行调用（防止以后再把大段代码塞回上游函数）
        m = re.search(r"\} else if \(current_fb_->format == PIXFORMAT_JPEG\) \{(.*?)\n    \}",
                      self.camera_cc, re.S)
        self.assertIsNotNone(m, "找不到 Capture() 的 JPEG 分支")
        branch = m.group(1)
        self.assertIn("DecodeJpegPreview(current_fb_->buf, current_fb_->len);", branch)
        self.assertLessEqual(len(branch.strip().splitlines()), 2,
                             "这个分支要保持“一行调用”，解码细节放 DecodeJpegPreview")

    def test_rgb565_path_untouched(self):
        """其它板还在用 RGB565：软件编码分支（字节序交换 + image_to_jpeg_cb）不能被动。"""
        self.assertIn("__builtin_bswap16", self.camera_cc)
        self.assertRegex(self.camera_cc, r"image_to_jpeg_cb\(")

    def test_fb_count_stays_one(self):
        """fb_count=2 会多占 ~30KB DMA 内部 RAM，本板扛不住（见 esp32_camera.h 注释）。"""
        self.assertIn("config.fb_count = 1;", self.board)

    def test_photo_flip_settings_applied_at_boot(self):
        """翻转只在开机应用一次就够了：单一模式下不再有 Reinit 把它刷掉。"""
        self.assertIn("ApplyCameraFlip();", self.board)
        self.assertNotIn("->Reinit(", self.board)

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
        """尺寸/质量要真的进 sensor（否则弹窗改了也不生效），且不重建相机。"""
        m = re.search(r"std::string VideoStreamStart\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        body = m.group(1)
        self.assertIn("ApplyVideoSensorParams()", body, "分辨率和质量都通过这个 helper 下发")
        v = re.search(r"void ApplyVideoSensorParams\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIn("video_cfg_.quality", v.group(1), "质量要写进 sensor")

    def test_all_params_apply_without_reinit(self):
        """四项参数都不该重启相机（分辨率也走 set_framesize 动态切，不白闪 0.3 秒）。

        依据驱动源码：
          - cam_hal.c:588  fb_size = 宽×高/5，按「初始化时」的分辨率算
          - ll_cam.c:478   JPEG 模式 DMA 缓冲固定 32KB，与分辨率无关
          - ov2640.c:211   set_framesize 只写 sensor 寄存器
          - ov2640.c:334   set_quality  只写 sensor 寄存器
        所以开流时按最大档初始化，之后切分辨率/质量都不用重启。
        """
        m = re.search(r"std::string VideoCfgApply\(int size, int fps, int quality, int flip\)\s*\{(.*?)\n    \}",
                      self.board, re.S)
        self.assertIsNotNone(m, "找不到带 flip 的 VideoCfgApply")
        body = m.group(1)
        self.assertIn("const bool size_changed", body)
        self.assertNotIn("quality != video_cfg_.quality) ||", body, "质量不能进重建条件")
        self.assertIn("set_quality", body, "质量要动态写进 sensor")
        self.assertIn("ApplyVideoFramesize()", body, "分辨率要动态切 sensor")
        self.assertNotIn("VideoStreamStop()", body, "VideoCfgApply 里不该再重启流")

    def test_unchanged_params_skip_work(self):
        """什么都没改也点「应用」→ 不该写 NVS、不该写 sensor 寄存器。

        用户问过这点：参数没变动时就不该再下发一遍。
        无条件下发的代价：NVS 写入磨损 flash、I2C 写寄存器让画面抖一下。
        """
        m = re.search(r"std::string VideoCfgApply\(int size, int fps, int quality, int flip\)\s*\{(.*?)\n    \}",
                      self.board, re.S)
        body = m.group(1)
        for flag in ("size_changed", "quality_changed", "fps_changed", "flip_changed"):
            self.assertIn(f"const bool {flag} = ", body, f"要有 {flag} 判断")
        # NVS 只在真的有变化时落盘
        self.assertRegex(
            body,
            r"if \(size_changed \|\| quality_changed \|\| fps_changed \|\| flip_changed\) \{\s*\n\s*SaveVideoCfg\(\);",
            "NVS 保存要加变化判断")
        # 帧率/翻转必须被各自的判断包住（不能再无条件下发）
        self.assertRegex(body, r"if \(fps_changed\) \{\s*\n\s*LocalVideoStreamSetFps")
        self.assertRegex(body, r"if \(flip_changed\) \{\s*\n\s*SetCameraFlip")
        self.assertEqual(body.count("SetCameraFlip(flip);"), 1)
        # 翻转值不缓存（MCP 工具 self.camera.set_flip 会绕过板级直写 NVS，缓存会过期）
        self.assertNotIn("video_flip_", self.board, "翻转值不能做内存缓存")
        self.assertIn("(flip != GetCameraFlip())", body, "变化判断要读 NVS")

    def test_framesize_change_does_not_reload_img(self):
        """改分辨率也不用重连 <img>。

        MJPEG 流里每帧是独立 JPEG（自带尺寸），浏览器逐帧替换显示，流本身从没断过 →
        重连反而白断画面 500ms。（旧实现 deinit+init 会重启整个 httpd 流，那时才需要。）
        """
        m = re.search(r"function applyVideoCfg\(size, fps, quality, flip\)\s*\{(.*?)\n    \}",
                      self.html, re.S)
        self.assertIsNotNone(m, "找不到 applyVideoCfg")
        self.assertNotIn("removeAttribute('src')", m.group(1), "改分辨率不该断开画面")
        self.assertIn("closeVideoCfg", m.group(1), "成功后就关弹窗即可")
        # 响应里也不该再有 size_changed（前端已不需要）
        self.assertNotIn('"size_changed"', self.board)
        self.assertNotIn("size_changed === true", self.html)

    def test_framesize_inited_at_max_then_switched_dynamically(self):
        """开流按最大分辨率初始化帧缓冲，再 set_framesize 切到用户选的档。

        反例（坑）：按当前分辨率初始化再往大改 → cam_hal 的 FB-OVF + ll_cam_stop() 停摆。
        """
        m = re.search(r"static constexpr framesize_t kVideoMaxFrameSize = (FRAMESIZE_\w+)", self.board)
        self.assertIsNotNone(m, "找不到 kVideoMaxFrameSize")
        self.assertEqual(m.group(1), "FRAMESIZE_SVGA", "最大档必须 = 选项里的 800×600")
        af = re.search(r"void ApplyVideoFramesize\(\)\s*\{(.*?)\n    \}", self.board, re.S)
        self.assertIsNotNone(af, "找不到 ApplyVideoFramesize")
        self.assertIn("set_framesize", af.group(1), "动态切分辨率只能靠 sensor")
        self.assertIn("VideoFrameSizeOf(video_cfg_.size)", af.group(1))

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


class TestPhotoReturnsDriverFrame(_Base):
    """拍照链路用完必须把驱动帧还回去 —— 本板 fb_count=1 的硬约束（2026-09 真机回归）。

    真机现象（用户报告 + 日志）：开视频后拍照（或先拍照再开视频），照片能拍成，
    但视频从此再也出不了一帧，日志每 ~4 秒刷一次
        W cam_hal: Failed to get frame: timeout
        W LocalVideo: fb_get failed
    只有重启才能恢复。

    根因（日志 + 驱动源码双证）：单一 JPEG 模式下相机不再 Reinit，而 Capture() 把驱动
    **唯一**那块帧（fb_count=1）留在 current_fb_ 里不还；cam_hal 的可用帧是 cam_give()
    里 en=1 的计数，被拿走的那块永远是 en=0 → cam_get_next_frame() 找不到空闲缓冲 →
    cam_start_frame() 失败、相机停摆 → 之后每次 esp_camera_fb_get() 都等满
    FB_GET_TIMEOUT（4000ms）返回 NULL —— 与日志里 4.05 秒的间隔完全吻合。

    修法：拍照链路用完即还（AI 拍照在 Explain 任何出口都还、网页拍照编码完就还）。
    """

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.local_photo = _read(LOCAL_PHOTO_CC)

    def test_driver_exposes_release_api(self):
        """归还接口必须存在，且注释写清「不还的后果」，否则容易被当成可选调用删掉。"""
        self.assertIn("void ReleaseCurrentFrame();", self.camera_h)
        idx = self.camera_h.index("void ReleaseCurrentFrame();")
        doc = self.camera_h[max(0, idx - 1200):idx]
        self.assertIn("fb_count=1", doc, "要写明本板只有一块驱动帧")
        self.assertIn("timeout", doc, "要写明不还的后果：视频流取帧永远超时")

    def test_release_helper_is_idempotent(self):
        """归还必须是「还了就置空」的幂等操作（多处调用、重复调用都安全）。"""
        m = re.search(r"void Esp32Camera::ReleaseCurrentFrame\(\)\s*\{(.*?)\n\}",
                      self.camera_cc, re.S)
        self.assertIsNotNone(m, "找不到 ReleaseCurrentFrame 实现")
        body = m.group(1)
        self.assertIn("esp_camera_fb_return(current_fb_)", body, "要真的还给驱动")
        self.assertIn("current_fb_ = nullptr", body, "还完必须置空，避免悬垂指针被再次归还")
        self.assertIn("if (current_fb_ != nullptr)", body, "要判空，允许重复调用")

    def test_explain_releases_frame_on_every_exit(self):
        """AI 拍照：Explain() 有 throw 分支，归还必须覆盖所有出口（用守卫，不靠逐条 return）。"""
        m = re.search(r"std::string Esp32Camera::Explain\(.*?\n\}", self.camera_cc, re.S)
        self.assertIsNotNone(m, "找不到 Explain 实现")
        body = m.group(0)
        # 守卫类型：析构里归还驱动帧
        guard = re.search(r"struct FrameReleaser\s*\{(.*?)\n    \}", body, re.S)
        self.assertIsNotNone(guard, "Explain 里要有归还守卫（throw 路径靠它覆盖）")
        self.assertIn("~FrameReleaser()", guard.group(1), "必须在析构里归还")
        self.assertIn("ReleaseCurrentFrame()", guard.group(1))
        # 守卫必须在启动编码线程**之前**就装好，否则中途 throw 的路径不会归还
        inst = re.search(r"\}\s*(\w+)\s*\{\s*this\s*\}\s*;", body)
        self.assertIsNotNone(inst, "守卫要真的实例化（只定义类型不装就白搭）")
        self.assertLess(inst.start(), body.index("CreateEncoderThread"),
                        "守卫要在启动编码线程之前装好")

    def test_capture_releases_previous_frame_before_grabbing(self):
        """Capture() 也要走同一个归还入口：连取两帧时先还再取（取帧失败也不会把旧帧扣住）。"""
        m = re.search(r"bool Esp32Camera::Capture\(\)\s*\{(.*?)\n\}", self.camera_cc, re.S)
        self.assertIsNotNone(m, "找不到 Capture 实现")
        body = m.group(1)
        self.assertLess(body.index("ReleaseCurrentFrame()"), body.index("esp_camera_fb_get()"),
                        "必须在取新帧之前先归还上一帧")
        self.assertNotIn("esp_camera_fb_return(current_fb_)", body,
                         "归还统一走 ReleaseCurrentFrame()，别再手写一份（容易漏置空）")

    def test_web_photo_releases_frame_after_encode(self):
        """网页拍照：编码一结束（无论成败）就归还，别等到下一次拍照。"""
        m = re.search(r"bool LocalPhotoCapture\(\)\s*\{(.*?)\n\}", self.local_photo, re.S)
        self.assertIsNotNone(m, "找不到 LocalPhotoCapture")
        body = m.group(1)
        i_encode = body.index("EncodeCurrentFrameToJpeg")
        i_release = body.index("ReleaseCurrentFrame()")
        self.assertLess(i_encode, i_release, "要在编码之后归还")
        self.assertLess(i_release, body.index("s_len = len"), "要在返回成功之前归还")

    def test_video_stream_keeps_retrying_after_timeout(self):
        """取帧超时只退让重试，不能让流退出：帧一旦被归还，画面要能自愈。"""
        m = re.search(r"camera_fb_t \*fb = esp_camera_fb_get\(\);(.*?)if \(fb->format",
                      self.video, re.S)
        self.assertIsNotNone(m, "找不到流循环里的取帧失败分支")
        body = m.group(1)
        self.assertIn("fb_get failed", body)
        self.assertIn("vTaskDelay", body, "失败要退让，避免空转烧 CPU")
        self.assertIn("continue", body, "失败只重试，不能退出循环")
        self.assertNotIn("return", body, "超时不是致命错误（帧被归还后要能恢复）")


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
