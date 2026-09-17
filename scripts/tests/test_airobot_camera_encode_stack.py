"""AI 拍照（Explain）编码线程的栈必须显式放大，否则任务 "pthread" 栈溢出直接 panic 重启。

背景（2026-09 真机崩溃）：
    让 AI 拍照 → 立刻重启，串口 backtrace 只有：

        ***ERROR*** A stack overflow in task pthread has been detected.
        vApplicationStackOverflowHook
        vTaskSwitchContext
        _frxt_dispatch

    根因：`Explain()` 用 `std::thread` 建编码线程，而 std::thread 底层走 pthread 默认栈
    `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=3072`（3KB）。这条线程里要跑两件重活：
      1. `image_to_jpeg_cb()` 软件 JPEG 编码（esp_new_jpeg）；
      2. 板级 JPEG 观察者 `SetJpegObserver()` —— 照片相册的**同步写 TF 卡**
         （FatFS + SDMMC + 目录清理，见 photo_store.cc）。
    两者叠加远超 3KB → 栈溢出 panic。

    对照证据（几乎是判决性）：网页拍照在 **httpd 任务（栈 8192）** 里跑**同一段**
    编码 + 写卡代码，从不重启；唯一差别就是任务栈大小。

必须固化的约束：
1. 编码线程必须显式设置栈（≥8KB，当前 16KB），不得依赖 pthread 默认 3KB；
2. 栈优先放 PSRAM（内部 SRAM 本就紧张，与 local_music_player 的播放线程同法）；
3. `esp_pthread_set_cfg()` 改的是**全局默认值**，创建完必须恢复，否则污染后续所有线程；
4. 线程要有可辨认的名字（崩溃日志里的 "pthread" 无法区分到底是哪条线程）。
"""

import os
import re
import unittest

CAMERA_CC = ("main", "boards", "common", "esp32_camera.cc")
CAMERA_H = ("main", "boards", "common", "esp32_camera.h")

# 栈下限：必须明显大于 pthread 默认的 3072，且不小于网页拍照能跑通的 httpd 栈（8192）
MIN_STACK_BYTES = 8 * 1024


def _read(*parts):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", *parts)
    with open(path, encoding="utf-8") as f:
        return f.read()


class TestCameraEncodeThreadStack(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = _read(*CAMERA_CC)
        cls.header = _read(*CAMERA_H)

    def _helper_body(self):
        """取出创建编码线程的辅助函数体（必须由它统一设置栈）。"""
        m = re.search(r"std::thread CreateEncoderThread\(.*?\n\}", self.cc, re.S)
        self.assertIsNotNone(
            m, "未找到 CreateEncoderThread：编码线程必须经它创建才能带上大栈")
        return m.group(0)

    def test_encoder_thread_uses_explicit_stack(self):
        """编码线程不得再直接用 std::thread（那会拿到 3KB 默认栈）。"""
        m = re.search(r"std::string Esp32Camera::Explain\(.*?\n\}", self.cc, re.S)
        self.assertIsNotNone(m, "未找到 Explain")
        body = m.group(0)
        self.assertIn("CreateEncoderThread(", body,
                      "Explain 必须用 CreateEncoderThread 创建编码线程")
        self.assertNotIn("encoder_thread_ = std::thread(", body,
                         "不得直接 std::thread：pthread 默认栈仅 3KB，编码+写卡会溢出")

    def test_stack_size_is_large_enough(self):
        helper = self._helper_body()
        m = re.search(r"kEncoderThreadStackBytes\s*=\s*(\d+)\s*\*\s*(\d+)", self.cc)
        self.assertIsNotNone(m, "未找到 kEncoderThreadStackBytes = N * 1024 形式的常量")
        size = int(m.group(1)) * int(m.group(2))
        self.assertGreaterEqual(
            size, MIN_STACK_BYTES,
            f"编码线程栈只有 {size} 字节，必须 ≥ {MIN_STACK_BYTES}（3KB 会栈溢出重启）")
        self.assertIn("cfg.stack_size = kEncoderThreadStackBytes;", helper)

    def test_stack_prefers_psram(self):
        """内部 SRAM 紧张：栈应优先放 PSRAM，且必须由 ext-mem 开关保护。"""
        helper = self._helper_body()
        self.assertIn("CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM", helper,
                      "PSRAM 栈需要 ext-mem 开关保护，否则无 PSRAM 的配置建线程会失败")
        self.assertIn("cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;", helper)

    def test_restores_pthread_default_config(self):
        """esp_pthread_set_cfg 是全局默认值：建完线程必须恢复，否则污染后续线程。"""
        helper = self._helper_body()
        self.assertIn("esp_pthread_get_default_config()", helper)
        # 保存 → 设置 → 创建 → 恢复，恢复至少出现两次（正常路径 + 异常路径）
        self.assertGreaterEqual(helper.count("esp_pthread_set_cfg(&saved)"), 2,
                                "成功与异常路径都必须恢复默认配置")

    def test_thread_has_name(self):
        """崩溃日志里任务名必须是可辨认的，不能再是 pthread。"""
        helper = self._helper_body()
        self.assertIn("cfg.thread_name", helper)

    def test_documented_contract(self):
        """头文件里要写明：观察者回调跑在**大栈**编码线程里（写卡是允许的重活）。"""
        m = re.search(r"void SetJpegObserver\(.*?;", self.header, re.S)
        self.assertIsNotNone(m, "未找到 SetJpegObserver 声明")
        comment = self.header[max(0, m.start() - 900):m.start()]
        self.assertIn("编码线程", comment)
        self.assertIn("栈", comment, "必须说明该线程已放大栈，可在回调里同步写卡")


if __name__ == "__main__":
    unittest.main()
