#include "local_photo.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>

#include "esp32_camera.h"

namespace {

Esp32Camera* s_camera = nullptr;
uint8_t* s_jpeg = nullptr;  // PSRAM 常驻，避免每次拍照都分配/释放
size_t s_capacity = 0;
size_t s_len = 0;
// 与 AI 拍照串行：两条路共用同一个 camera 驱动与 current_fb_
std::mutex s_mtx;

}  // namespace

bool LocalPhotoInit(Esp32Camera* camera, size_t capacity_bytes) {
    if (camera == nullptr || capacity_bytes == 0) {
        return false;
    }
    s_camera = camera;
    s_jpeg = static_cast<uint8_t*>(
        heap_caps_malloc(capacity_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_jpeg == nullptr) {
        return false;  // 网页拍照不可用，其余功能不受影响
    }
    s_capacity = capacity_bytes;
    s_len = 0;
    return true;
}

bool LocalPhotoCapture() {
    if (s_camera == nullptr || s_jpeg == nullptr) {
        return false;
    }
    // 与 AI 拍照串行（两者共用 camera 驱动与 current_fb_）。
    // 用 try_lock + 极短重试，不用标准库的定时锁：std::mutex 不提供定时版本，
    // 而 std::timed_mutex 依赖 pthread_mutex_timedlock（ESP-IDF 上不可靠）。
    // 总等待 ≤ 50ms，绝不长时间占住 httpd 任务（它同时还要跑 WS 日志拉取与上传）。
    if (!s_mtx.try_lock()) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!s_mtx.try_lock()) {
            return false;  // AI 正在拍照，让用户重试即可
        }
    }
    std::lock_guard<std::mutex> lock(s_mtx, std::adopt_lock);
    // Capture() 会丢弃旧帧取最新帧，并顺带更新 LCD 预览（与 AI 拍照行为一致）
    if (!s_camera->Capture()) {
        return false;
    }
    size_t len = 0;
    if (!s_camera->EncodeCurrentFrameToJpeg(s_jpeg, s_capacity, len)) {
        return false;
    }
    s_len = len;
    return true;
}

const uint8_t* LocalPhotoData() { return (s_len > 0) ? s_jpeg : nullptr; }

size_t LocalPhotoSize() { return s_len; }
