"""测试 TF 卡照片相册（photo_store + /photos 接口 + 前端相册）的回归防护。

背景：2026-09-17 新增「有 TF 卡时把拍的照片存卡，并在网页翻看历史」。
看似简单，但下面每条都是踩过或极易踩的坑：

1. 滚动清理必须**只**在写卡成功后做 —— 写失败还清理就会白删用户的旧照片；
2. 文件名必须净化 —— `/photos/file?name=../../config.json` 这类请求不能读到卡上其它文件；
3. 时间未同步（刚开机还没联网）时必须用序号兜底 —— 否则所有照片都叫同一个名字互相覆盖；
4. 单张 JPEG 必须**分块发送** —— 整张读进内存/占 httpd 栈会打爆本就很紧的内部 SRAM；
5. AI 拍照留档靠共享 `Esp32Camera` 的一个纯增量观察者，回调里必须**当场同步写卡**
   （jpeg 指针只在回调期间有效）；
6. 进入照片 Tab 必须停日志轮询 —— httpd 是单线程，相册拉图与 WS 日志会互相排队；
7. `photo_store.cc` 不允许出现 ESP_LOG —— 本板日志与 Arduino 指令共用 UART0。
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


def _func(src, signature):
    """取出某个函数定义体（到行首的 '}' 为止）。"""
    m = re.search(re.escape(signature) + r"\(.*?\n\}", src, re.S)
    assert m is not None, f"未找到 {signature}"
    return m.group(0)


class TestPhotoStore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = _read(*BOARD_DIR, "photo_store.cc")
        cls.header = _read(*BOARD_DIR, "photo_store.h")
        cls.server = _read(*BOARD_DIR, "http_upload_server.cc")
        cls.web = _read(*BOARD_DIR, "web", "index.html")
        cls.board = _read(*BOARD_DIR, "compact_wifi_board_s3cam_airobot.cc")
        cls.camera_h = _read("main", "boards", "common", "esp32_camera.h")
        cls.camera_cc = _read("main", "boards", "common", "esp32_camera.cc")

    # ---------- 存储与保留策略 ----------

    def test_dirs_and_limit(self):
        self.assertIn('"/sdcard/photos"', self.src)
        self.assertIn('"/sdcard/photos_ai"', self.src)
        self.assertIn("kMaxPhotos = 100", self.src, "每目录保留 100 张")

    def test_trim_only_after_successful_write(self):
        """清理必须在写卡成功之后：写失败还清理会白删用户的旧照片。"""
        body = _func(self.src, "bool PhotoStoreSave")
        self.assertIn("fwrite", body)
        self.assertIn("TrimDir(dir)", body)
        self.assertLess(body.index("fwrite"), body.index("TrimDir(dir)"),
                        "TrimDir 必须在 fwrite 之后")
        self.assertIn("written != len", body, "写入长度不符必须判失败")
        self.assertIn("remove(path)", body, "半截文件要删掉")

    def test_trim_removes_oldest_first(self):
        body = _func(self.src, "void TrimDir")
        self.assertIn("a.mtime < b.mtime", body, "清理必须从最旧的开始删")
        self.assertIn("remove_count", body)

    def test_filesystem_ops_serialized(self):
        """写卡可能来自 httpd 与编码线程两个线程，写/删/清必须串行。"""
        self.assertIn("std::mutex s_store_mtx", self.src)
        for sig in ("bool PhotoStoreSave", "bool PhotoStoreDelete", "int PhotoStoreClear"):
            body = _func(self.src, sig)
            self.assertIn("lock_guard", body, f"{sig} 必须持锁")

    # ---------- 文件名与安全 ----------

    def test_name_sanitizing_rejects_traversal(self):
        """文件名净化用「拒绝」而非「替换」：避免 ../ 之类被改写成合法名字后误用。"""
        body = _strip_comments(_func(self.src, "bool IsSafePhotoName"))
        self.assertIn('strstr(name, "..")', body)
        self.assertIn("'/'", body)
        self.assertIn("'\\\\'", body)
        self.assertIn("HasJpgSuffix(name)", body, "只接受 .jpg")
        self.assertNotIn("= '_'", body, "不应做替换式净化")

    def test_read_path_reuses_same_sanitizing(self):
        """读图路径必须复用同一套净化（否则读/删两侧规则会走偏）。"""
        self.assertIn("bool PhotoStoreResolvePath", self.src)
        body = _func(self.src, "bool PhotoStoreResolvePath")
        self.assertIn("IsSafePhotoName(name)", body)
        self.assertIn("PhotoStoreResolvePath", self.server, "HTTP 读图要用它解析路径")

    def test_seq_fallback_when_time_unsynced(self):
        self.assertIn("P_%04d", self.src, "时间未同步要用序号兜底，否则照片互相覆盖")
        self.assertIn("kTimeSyncedThreshold", self.src)

    def test_filename_collision_handled(self):
        """同一秒连拍会同名，必须探测冲突加后缀而不是覆盖已有照片。"""
        body = _func(self.src, "void MakeFileName")
        self.assertIn("stat(path, &st) != 0", body, "要探测文件是否已存在")
        self.assertIn("_%d.jpg", body)

    def test_list_is_sorted_newest_first(self):
        body = _func(self.src, "void SortNewestFirst")
        self.assertIn("a.mtime > b.mtime", body, "列表按拍摄时间倒序")
        self.assertIn("SortNewestFirst", self.src)

    def test_no_esp_log_in_photo_store(self):
        """本板日志与 Arduino 指令共用 UART0：拍照/写卡路径不允许打日志。"""
        self.assertNotIn("ESP_LOG", self.src)
        self.assertIsNone(re.search(r"(?<![a-z_])printf\s*\(", self.src))

    # ---------- AI 拍照开关 ----------

    def test_ai_save_default_on_and_persisted(self):
        body = _func(self.src, "bool PhotoStoreGetAiSave")
        self.assertIn("GetBool(kAiSaveKey, true)", body, "默认必须是开")
        self.assertIn('"photo"', self.src, "NVS 命名空间")
        self.assertIn('"ai_save"', self.src, "NVS 键")
        self.assertIn("SetBool(kAiSaveKey, on)", self.src)

    # ---------- 共享相机钩子 ----------

    def test_ai_photo_hook_wired_in_shared_camera(self):
        """AI 拍照留档靠共享的 JPEG 观察者（纯增量，默认空 → 其它板行为不变）。"""
        self.assertIn("SetJpegObserver", self.camera_h)
        self.assertIn("jpeg_observer_", self.camera_cc)
        # image_to_jpeg_cb 只收**普通函数指针**：捕获 this 的 lambda 编译不过，
        # 所以回调必须写成静态成员函数（拿不到 ctx 就只能靠 EncodeCtx 打包）
        self.assertIn("size_t Esp32Camera::JpegEncodeCb(", self.camera_cc)
        m = re.search(r"bool ok = image_to_jpeg_cb\(.*?JpegEncodeCb, &ctx\);", self.camera_cc, re.S)
        self.assertIsNotNone(m, "Explain 必须用静态回调 + EncodeCtx")
        body = _func(self.camera_cc, "size_t Esp32Camera::JpegEncodeCb")
        self.assertIn("index == 0", body)
        self.assertIn("jpeg_observer_(", body, "必须在拿到完整 JPEG 时通知观察者")

    def test_board_wires_ai_photo_save(self):
        self.assertIn("PhotoStoreInit()", self.board)
        self.assertIn("SetJpegObserver", self.board)
        self.assertIn("PhotoStoreGetAiSave()", self.board, "留档必须受开关控制")
        self.assertIn("PhotoStoreSave(PhotoKind::kAi", self.board)
        self.assertIn("self.camera.ai_save", self.board, "AI 语音也要能切开关")

    # ---------- HTTP 接口 ----------

    def test_http_routes_and_limits(self):
        for uri in ('"/photos"', '"/photos/file"'):
            self.assertIn(uri, self.server)
        for var in ("photo_list_uri", "photo_post_uri", "photo_file_uri"):
            self.assertIn(f"httpd_register_uri_handler(server, &{var})", self.server)
        m = re.search(r"cfg\.max_uri_handlers = (\d+);", self.server)
        self.assertIsNotNone(m)
        self.assertGreaterEqual(int(m.group(1)), 24, "新增 4 个槽位后要调大上限")

    def test_photo_file_streams_in_chunks(self):
        """单张 JPEG 必须分块发：整张读进内存或占满 httpd 栈会打爆内部 SRAM。"""
        body = _func(self.server, "static esp_err_t HandlePhotoFile")
        self.assertIn("httpd_resp_send_chunk", body)
        self.assertIn("fread(send_buf", body)
        self.assertNotIn("httpd_resp_send(req,", body, "不得一次性发整张")
        self.assertIn("no-store", body, "删掉后不应还能从浏览器缓存里看到")

    def test_take_reports_saved(self):
        body = _func(self.server, "static esp_err_t HandlePhotoTake")
        self.assertIn("PhotoStoreSave(PhotoKind::kWeb", body)
        self.assertIn('\\"saved\\":%d', body)

    def test_photo_api_validates_kind(self):
        self.assertIn("ParsePhotoKind", self.server)
        self.assertIn("bad kind", self.server, "未知 kind 必须 400，不能默默当 web 删错目录")

    def test_list_carries_ai_save_flag(self):
        """列表响应带上开关状态，前端一次请求就能初始化勾选框。"""
        self.assertIn('cJSON_AddBoolToObject(root, "ai_save"', self.src)

    # ---------- 前端 ----------

    def test_frontend_album(self):
        self.assertIn("showTab('photos'", self.web)
        self.assertIn('id="panel-photos"', self.web)
        for fn in ("loadPhotos", "deletePhoto", "clearPhotos", "setAiSave", "openPhotoViewer"):
            self.assertIn(f"function {fn}(", self.web)
        self.assertIn("PHOTO_PAGE_SIZE = 12", self.web)
        self.assertIn("img.loading = 'lazy'", self.web, "图片要懒加载，别一次拉 100 张")
        self.assertIn("/photos/file?kind=", self.web)

    def test_photo_tab_stops_log_polling(self):
        """进照片 Tab 要停日志轮询：httpd 单线程，相册拉图与日志会互相排队。"""
        m = re.search(r"function showTab\(id, btn\)\s*\{(.*?)\n    \}", self.web, re.S)
        self.assertIsNotNone(m)
        body = m.group(1)
        self.assertIn("stopLogPolling()", body)
        self.assertIn("if (id === 'photos') loadPhotos();", body)

    def test_take_shows_saved_status(self):
        """拍照后要告诉用户这张有没有存到卡上（写卡失败不影响当次显示）。"""
        self.assertIn('id="photoStatus"', self.web)
        self.assertIn("res.saved", self.web)
        self.assertIn("仅显示", self.web)


if __name__ == "__main__":
    unittest.main()
