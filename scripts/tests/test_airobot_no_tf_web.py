"""无 TF 卡时保留 Web 控制及相机，禁用依赖存储的入口。"""

import json
import pathlib
import unittest

BOARD = pathlib.Path(__file__).resolve().parents[2] / "main/boards/bread-compact-wifi-s3cam-airobot"


class NoTfWebTest(unittest.TestCase):
    def test_variant_enables_websocket_and_jpeg(self):
        config = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        variant = next(v for v in config["builds"] if v["name"].endswith("-no-tfcard"))
        flags = variant["sdkconfig_append"]
        for flag in ("CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD=n", "CONFIG_HTTPD_WS_SUPPORT=y",
                     "CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y"):
            self.assertEqual(flags.count(flag), 1)

    def test_storage_is_optional_and_routes_are_gated(self):
        board = (BOARD / "compact_wifi_board_s3cam_airobot.cc").read_text(encoding="utf-8")
        server = (BOARD / "http_upload_server.cc").read_text(encoding="utf-8")
        page = (BOARD / "web/index.html").read_text(encoding="utf-8")
        self.assertIn("StartUploadServer(false)", board)
        self.assertIn("StartUploadServer(sd_card_mounted_", board)
        self.assertIn("s_storage_available && data != nullptr", server)
        self.assertIn('strcmp(action, "capabilities")', server)
        self.assertIn("s_storage_available && (httpd_register_uri_handler", server)
        self.assertIn("webStorageAvailable = caps.storage === true", page)
        self.assertIn("id=\"photoAlbum\"", page)


if __name__ == "__main__":
    unittest.main()
