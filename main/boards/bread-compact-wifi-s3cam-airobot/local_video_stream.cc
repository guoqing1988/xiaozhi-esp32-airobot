#include "local_video_stream.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_camera.h"

#define TAG "LocalVideo"

namespace {

// 独立端口：主 httpd 用 80（WS 日志/控制/上传），这里避开以免长循环的 /stream 把那条任务占死。
// ctrl_port 也必须与主 httpd 的默认值(32768)错开，否则第二个实例起不来。
constexpr int kStreamPort = 81;
constexpr int kStreamCtrlPort = 32769;

// 目标帧率上限：实时视频与 ESP-NOW 共用同一个 2.4G 射频，限帧=限空口占用，保住节点控制的实时性。
constexpr int kTargetFps = 12;

// 官方 multipart 流的分隔与头部（照 esp32-camera README 的样例，社区事实标准）
constexpr char kStreamContentType[] = "multipart/x-mixed-replace;boundary="
                                      "123456789000000000000987654321";
constexpr char kStreamBoundary[] = "\r\n--123456789000000000000987654321\r\n";
constexpr char kStreamPart[] = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t s_hd = nullptr;
VideoStatCallback s_on_stat;
volatile bool s_stop = false;
// 同时只服务一个观众：第二个连接直接拒绝（本板 fb_count=1，帧池只有一块，两路流会互抢）
std::atomic<bool> s_streaming{false};

// 帧率统计（滚动时间窗，约每秒上报一次）
int s_frames_in_window = 0;
int64_t s_window_start_us = 0;
int s_last_w = 0;
int s_last_h = 0;

void CountFrame(int width, int height) {
    const int64_t now = esp_timer_get_time();
    s_last_w = width;
    s_last_h = height;
    if (s_window_start_us == 0) {
        s_window_start_us = now;
        s_frames_in_window = 0;
        return;
    }
    s_frames_in_window++;
    const int64_t elapsed = now - s_window_start_us;
    if (elapsed >= 1000000) {
        if (s_on_stat) {
            s_on_stat(static_cast<float>(s_frames_in_window) * 1000000.0f / static_cast<float>(elapsed),
                      s_last_w, s_last_h);
        }
        s_frames_in_window = 0;
        s_window_start_us = now;
    }
}

// 实际出流循环（由 StreamHandler 在抢到“唯一观众”名额后调用）。
// 结构照官方 jpg_stream_httpd_handler：boundary → part 头 → JPEG 数据，三段式分块发送。
// ⚠ 这条循环**只在有浏览器连着 /stream 时才跑**：没人看就不抓帧、不发包、不耗射频。
esp_err_t StreamLoop(httpd_req_t *req) {
    esp_err_t res = httpd_resp_set_type(req, kStreamContentType);
    if (res != ESP_OK) {
        return res;
    }
    // 长连接必须禁止缓存与中间缓冲，否则浏览器攒够一大块才渲染（延时飙升、看着像卡顿）
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    const int64_t frame_interval_us = 1000000 / kTargetFps;
    int64_t next_frame_us = esp_timer_get_time();

    while (!s_stop) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == nullptr) {
            // 偶发抓帧失败（自动曝光切换等）不该终止整条流，退让一下重试
            ESP_LOGW(TAG, "fb_get failed");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (fb->format != PIXFORMAT_JPEG) {
            // 不该发生：Start 之前板级必须已把相机切到 JPEG 模式。
            // 若没切，这里编码不了（会白烧 CPU）且带宽浪费，直接报错退出更利于定位。
            ESP_LOGE(TAG, "not JPEG (format=%d): camera mode not switched?", fb->format);
            esp_camera_fb_return(fb);
            return ESP_FAIL;
        }

        res = httpd_resp_send_chunk(req, kStreamBoundary, strlen(kStreamBoundary));
        char part_buf[64];
        if (res == ESP_OK) {
            const size_t hlen = snprintf(part_buf, sizeof(part_buf), kStreamPart,
                                         static_cast<unsigned>(fb->len));
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            // 零拷贝：直接把驱动帧缓冲交给发送，不经任何中间拷贝
            res = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(fb->buf), fb->len);
        }
        CountFrame(fb->width, fb->height);
        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            // 客户端断开（取消勾选/关页面）属正常收尾，不是错误
            ESP_LOGI(TAG, "client disconnected");
            break;
        }

        // 限帧：追上就等到下一帧时间点，落后了则重置基准（不补帧，避免延时累积）
        next_frame_us += frame_interval_us;
        const int64_t now = esp_timer_get_time();
        if (next_frame_us > now) {
            vTaskDelay(pdMS_TO_TICKS((next_frame_us - now + 999) / 1000));
        } else {
            next_frame_us = now;
        }
    }

    if (s_on_stat) {
        s_on_stat(0.0f, s_last_w, s_last_h);  // 告诉页面流已停（角标显示 --）
    }
    return ESP_OK;
}

// /stream 入口：先抢“唯一观众”名额。
// 为什么限制：本板帧池只有一块，两个观众会互抢帧缓冲；两路流也会把 2.4G 空口占满，
// 节点控制必然卡顿。拒绝时给 503，页面可据此提示。
esp_err_t StreamHandler(httpd_req_t *req) {
    bool expected = false;
    if (!s_streaming.compare_exchange_strong(expected, true)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "another viewer is connected", HTTPD_RESP_USE_STRLEN);
        ESP_LOGW(TAG, "reject second viewer");
        return ESP_FAIL;
    }
    const esp_err_t res = StreamLoop(req);  // 所有 return 路径都在里面，名额在此统一归还
    s_streaming = false;
    return res;
}

httpd_uri_t s_stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = StreamHandler,
    .user_ctx = nullptr,
};

}  // namespace

bool LocalVideoStreamStart(VideoStatCallback on_stat) {
    if (s_hd != nullptr) {
        return true;  // 幂等：已在运行
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = kStreamPort;
    cfg.ctrl_port = kStreamCtrlPort;
    cfg.max_uri_handlers = 2;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    // 慢客户端（网络差）不能永久占住这条任务：超时即断开
    cfg.send_wait_timeout = 5;

    if (httpd_start(&s_hd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed on port %d", kStreamPort);
        s_hd = nullptr;
        return false;
    }
    if (httpd_register_uri_handler(s_hd, &s_stream_uri) != ESP_OK) {
        ESP_LOGE(TAG, "register /stream failed");
        httpd_stop(s_hd);
        s_hd = nullptr;
        return false;
    }

    s_on_stat = std::move(on_stat);
    s_stop = false;
    s_frames_in_window = 0;
    s_window_start_us = 0;
    ESP_LOGI(TAG, "stream server started on port %d", kStreamPort);
    return true;
}

void LocalVideoStreamStop() {
    if (s_hd == nullptr) {
        return;  // 幂等
    }
    // 先置停止标志：让正在跑的 handler 循环主动退出，再 httpd_stop 回收任务与栈。
    // 最坏等一帧时间（~80ms）或 send_wait_timeout。
    s_stop = true;
    httpd_stop(s_hd);
    s_hd = nullptr;
    s_on_stat = nullptr;
    s_streaming = false;  // 防万一：httpd 任务回收时 handler 可能被硬中止
    ESP_LOGI(TAG, "stream server stopped");
}

bool LocalVideoStreamRunning() { return s_hd != nullptr; }
