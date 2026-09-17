"""测试网页拍照链路（/photo/take + /photo.jpg + 前端按钮）的回归防护。

背景：网页拍照复用 Esp32Camera 已捕获的帧做 JPEG 编码，避免 fb_count=2 —— 后者会让
cam_hal 多占 ~30KB DMA **内部** RAM（本板内部 SRAM 只有几十 KB，且这正是本次要修的
"拍照偶发重启"的根因方向）。

必须固化的约束：
1. 编码必须复用 Capture() 的帧（EncodeCurrentFrameToJpeg），不得自己 fb_get
   （fb_count=1 时唯一那块帧被 current_fb_ 持有，会阻塞）；也不得把 fb_count 改成 2；
2. JPEG 必须常驻 PSRAM，不得落内部 RAM；
3. /photo.jpg 必须带 no-store，否则浏览器缓存旧图，用户以为拍照坏了；
4. /photo/take 必须读掉请求 body，否则 keep-alive 复用时残留会污染下一个请求；
5. 前端必须用时间戳刷新 <img>（同 URL 图片不会重新加载）。
"""

import os
import re
import unittest

BOARD_DIR = ("main", "boards", "bread-compact-wifi-s3cam-airobot")


def _read(*parts):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", *parts)
    with open(path, encoding="utf-8") as f:
        return f.read()


class TestWebPhoto(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = _read(*BOARD_DIR, "http_upload_server.cc")
        cls.header = _read(*BOARD_DIR, "http_upload_server.h")
        cls.web = _read(*BOARD_DIR, "web", "index.html")
        cls.board = _read(*BOARD_DIR, "compact_wifi_board_s3cam_airobot.cc")
        cls.local = _read(*BOARD_DIR, "local_photo.cc")
        cls.camera = _read("main", "boards", "common", "esp32_camera.cc")

    def test_routes_registered(self):
        for uri in ('"/photo.jpg"', '"/photo/take"'):
            self.assertIn(uri, self.server, f"缺少路由 {uri}")
        self.assertIn("httpd_register_uri_handler(server, &photo_jpeg_uri)", self.server)
        self.assertIn("httpd_register_uri_handler(server, &photo_take_uri)", self.server)

    def test_jpeg_not_cached(self):
        m = re.search(r"static esp_err_t HandlePhotoJpeg\(.*?\n\}", self.server, re.S)
        self.assertIsNotNone(m, "未找到 HandlePhotoJpeg")
        body = m.group(0)
        self.assertIn('"image/jpeg"', body)
        self.assertIn("no-store", body, "必须禁缓存，否则浏览器显示的是上一张")

    def test_reuses_captured_frame(self):
        """必须复用 Capture() 的帧，不得自己取帧（会与 fb_count=1 的帧池抢帧而阻塞）。"""
        self.assertIn("EncodeCurrentFrameToJpeg", self.local)
        self.assertIn("->Capture()", self.local)
        self.assertNotIn("esp_camera_fb_get", self.local,
                         "不得自己取帧：fb_count=1 时 current_fb_ 被 Esp32Camera 持有，会阻塞")

    def test_encode_method_is_additive(self):
        """新增的编码方法必须是纯增量：只加方法，不改 Capture/Explain 的签名。"""
        self.assertIn("bool Esp32Camera::EncodeCurrentFrameToJpeg", self.camera)
        self.assertIn("bool EncodeCurrentFrameToJpeg(uint8_t *out, size_t out_capacity, size_t &out_len);",
                      _read("main", "boards", "common", "esp32_camera.h"))
        # 复用与 Explain() 相同的编码源（Capture 换序后的 encode_buf_；否则红蓝互换）
        self.assertIn("encode_buf_", self.camera)

    def test_encode_does_not_double_swap(self):
        """RGB565 字节序已在 Capture() 换好并存进 encode_buf_，编码时不得再换一次。

        二次 bswap 会让网页照片红蓝互换；而颜色错的 bug 很难一眼看出是编码还是 sensor，
        所以用断言钉住（2026-09 实际踩过：第一版实现就重复换序了）。
        """
        m = re.search(r"bool Esp32Camera::EncodeCurrentFrameToJpeg\(.*?\n\}", self.camera, re.S)
        self.assertIsNotNone(m, "未找到 EncodeCurrentFrameToJpeg")
        body = m.group(0)
        self.assertIn("encode_buf_", body, "RGB565 必须复用 Capture() 换序后的 encode_buf_")
        self.assertNotIn("__builtin_bswap16", body, "不得在编码时再次换字节序（会红蓝互换）")

    def test_jpeg_buffer_in_psram(self):
        """JPEG 常驻缓冲必须在 PSRAM，内部 SRAM 留给音频/网络。"""
        self.assertIn("MALLOC_CAP_SPIRAM", self.local)
        self.assertIn("heap_caps_malloc", self.local)

    def test_no_extra_frame_buffer(self):
        """fb_count 必须保持 1：改 2 会多占 ~30KB DMA 内部 RAM。"""
        self.assertIn("config.fb_count = 1;", self.board)

    def test_local_capture_has_bounded_wait(self):
        """与 AI 拍照的互斥必须有界：httpd 任务不能被无限期卡住。

        用 try_lock + 极短 vTaskDelay 重试，不用 try_lock_for（std::mutex 没这个成员，
        std::timed_mutex 依赖 ESP-IDF 上不可靠的 pthread_mutex_timedlock）。
        """
        self.assertIn("s_mtx.try_lock()", self.local)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(", self.local)
        self.assertNotIn("try_lock_for", self.local)

    def test_board_injects_camera_api(self):
        self.assertIn("SetCameraWebApi", self.board)
        self.assertIn("LocalPhotoInit(camera_", self.board)
        self.assertIn("void SetCameraWebApi(const CameraWebApi& api);", self.header)

    def test_frontend_button(self):
        self.assertIn('id="photoBtn"', self.web)
        self.assertIn("function takePhoto()", self.web)
        self.assertIn("'/photo/take'", self.web)
        # 同 URL 图片不会重新加载，必须带时间戳
        self.assertIn("'/photo.jpg?t=' + Date.now()", self.web)
        self.assertIn('id="photoImg"', self.web)

    def test_photo_controls_live_in_photos_tab(self):
        """拍照属于照片功能：按钮与显示区必须在「📷 照片」面板里，不在「🎮 机器人控制」里。

        2026-09 从机器人控制面板搬到照片 Tab：拍出来的照片就存进本 Tab 的相册，控件留在
        机器人面板会让人拍完还得切 Tab 才看得到（搬完把位置钉住，防止以后又搬回去）。
        """
        idx_uno = self.web.index('id="panel-uno"')
        idx_photos = self.web.index('id="panel-photos"')
        self.assertLess(idx_uno, idx_photos, "面板顺序：机器人控制在前、照片在后")
        for el in ('id="photoBtn"', 'id="photoBox"', 'id="photoImg"', 'id="photoStatus"'):
            self.assertGreater(self.web.index(el), idx_photos,
                               f"{el} 必须落在「📷 照片」面板内部")

    def test_clear_photo_view_is_display_only(self):
        """「🧹 清空显示」只清页面，不得动设备/卡上的照片（删照片是「🗑 清空本相册」的事）。

        差一个字的两个按钮很容易被接错：清显示误删卡上照片是不可逆的数据丢失，
        所以用断言钉住「不发任何请求」。
        """
        self.assertIn('onclick="clearPhotoView()"', self.web)
        m = re.search(r"function clearPhotoView\(.*?\n    \}", self.web, re.S)
        self.assertIsNotNone(m, "未找到 clearPhotoView")
        body = m.group(0)
        self.assertNotIn("fetch(", body, "清空显示不得发请求")
        self.assertNotIn("/photos", body, "清空显示不得碰相册接口")
        self.assertIn("photoImg", body)
        self.assertIn("photoBox", body)

    def test_photo_refreshes_album_grid(self):
        """拍完存卡成功要自动刷新相册网格，否则刚拍那张要手动点「🔄 刷新」才出现。"""
        m = re.search(r"function takePhoto\(.*?\n    \}", self.web, re.S)
        self.assertIsNotNone(m, "未找到 takePhoto")
        self.assertIn("loadPhotos();", m.group(0))

    def test_take_handler_drains_body(self):
        """POST body 要读掉，否则 keep-alive 复用连接时残留会导致解析异常。"""
        m = re.search(r"static esp_err_t HandlePhotoTake\(.*?\n\}", self.server, re.S)
        self.assertIsNotNone(m, "未找到 HandlePhotoTake")
        body = m.group(0)
        self.assertIn("httpd_req_recv", body)
        self.assertIn("content_len", body)


if __name__ == "__main__":
    unittest.main()
