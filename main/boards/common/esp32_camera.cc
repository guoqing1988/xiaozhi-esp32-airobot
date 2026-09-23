#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_pthread.h>
#include <img_converters.h>
#include <utility>

#include "esp32_camera.h"
#include "board.h"
#include "display.h"
#include "lvgl_display.h"
#include "mcp_server.h"
#include "system_info.h"
#include "jpg/image_to_jpeg.h"
#include "esp_timer.h"

#define TAG "Esp32Camera"

namespace {
// 把一张 JPEG 解码成 RGB565 挂到 LCD 预览上（本板单一 JPEG 模式的拍照预览）。
// 刻意放在这个匿名 namespace 里、由 Capture() 一行调用：共享文件里被改动的
// 「上游代码」就只剩那一行，以后合并官方代码时只需手工解 1 行。
// 取 uint8_t* 而非 const：esp_jpeg_image_cfg.indata 就是非 const（与 image_to_jpeg_cb 同）。
void DecodeJpegPreview(uint8_t *jpeg, size_t len) {
    if (jpeg == nullptr || len == 0) {
        return;
    }
    // tjpgd 草稿纸：ROM 里的解码器固定要 3.1KB（与图像大小无关，见 esp_jpeg 的
    // JPEG_WORK_BUF_SIZE）。放函数静态区而**不走堆**：本板内部 SRAM 紧张又怕碎片化，
    // 与 esp32-camera 自带 conversions/to_bmp.c 里的 `static uint8_t work[3100]` 同一做法。
    static uint8_t work[3100];
    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata = jpeg;
    cfg.indata_size = len;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale = JPEG_IMAGE_SCALE_1_2;  // 1/2 缩放(VGA→320×240)：LCD 才 240×240
    cfg.flags.swap_color_bytes = 0;  // 小端 RGB565 = LVGL 要的字节序（改这里会红蓝互换）
    cfg.advanced.working_buffer = work;
    cfg.advanced.working_buffer_size = sizeof(work);

    // 先只读 JPEG 头问出真实尺寸再分配。不能拿 fb->width/height 算：那是「sensor 当前
    // 配置的分辨率」，刚改过 set_framesize 时这一帧可能还是旧尺寸，按它算缓冲会写越界。
    // 再把 outbuf_size 交给解码器，它自己还会校验一次。
    // 不用组件自带的 jpg2rgb565()：它把 outbuf_size 写死为 UINT32_MAX，没有这两道保护。
    esp_jpeg_image_output_t info = {};
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.output_len == 0) {
        ESP_LOGW(TAG, "JPEG preview: bad header (len=%zu)", len);
        return;
    }
    uint8_t *preview_data =
        (uint8_t *)heap_caps_malloc(info.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (preview_data == nullptr) {
        return;
    }
    cfg.outbuf = preview_data;
    cfg.outbuf_size = info.output_len;
    esp_jpeg_image_output_t out = {};
    if (esp_jpeg_decode(&cfg, &out) != ESP_OK) {
        // 解码失败只影响 LCD 预览；照片本身仍可用（JPEG 直通）
        ESP_LOGW(TAG, "JPEG preview decode failed (len=%zu)", len);
        heap_caps_free(preview_data);
        return;
    }
    auto display = dynamic_cast<LvglDisplay *>(Board::GetInstance().GetDisplay());
    if (display == nullptr) {
        heap_caps_free(preview_data);
        return;
    }
    display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(preview_data, info.output_len,
                                                                 out.width, out.height,
                                                                 out.width * 2,
                                                                 LV_COLOR_FORMAT_RGB565));
    ESP_LOGI(TAG, "JPEG preview decoded: %ux%u (jpeg len=%zu)", out.width, out.height, len);
}

// 编码线程栈：std::thread 底层是 pthread，默认栈只有 3KB
// (CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=3072)。这条线程要跑软件 JPEG 编码
// (image_to_jpeg_cb → esp_new_jpeg)，还要**同步执行**板级 JPEG 观察者
// (SetJpegObserver，如照片相册写 TF 卡：FatFS + SDMMC + 目录清理)，两者叠加远超 3KB，
// 现象就是拍照瞬间 "A stack overflow in task pthread" 直接 panic 重启。
// 对照：网页拍照跑在同一段编码+写卡代码上、但位于 httpd 任务(栈 8192)，从不重启。
// 取 16KB（httpd 栈的两倍余量）；内部 SRAM 本就紧张，故优先放 PSRAM。
constexpr size_t kEncoderThreadStackBytes = 16 * 1024;

// 用显式栈创建编码线程。esp_pthread_set_cfg() 改的是**全局默认值**，
// 创建完必须立刻恢复，否则会污染调用方后续创建的任何线程。
std::thread CreateEncoderThread(std::function<void()> fn) {
    esp_pthread_cfg_t saved = esp_pthread_get_default_config();
    esp_pthread_cfg_t cfg = saved;
    cfg.stack_size = kEncoderThreadStackBytes;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    // 与播放线程同法：栈放 PSRAM，少占内部 RAM（需 ext-mem 允许，见 sdkconfig）
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
    cfg.thread_name = "cam_encode";  // 崩溃日志里 "pthread" 无法区分是哪条线程
    esp_pthread_set_cfg(&cfg);
    std::thread t;
    try {
        t = std::thread(std::move(fn));
    } catch (...) {
        esp_pthread_set_cfg(&saved);  // 创建失败也要恢复，避免残留影响后续线程
        throw;
    }
    esp_pthread_set_cfg(&saved);
    return t;
}
}  // namespace

#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED
#if CONFIG_XIAOZHI_CAMERA_HMIRROR
static constexpr bool kConfiguredHMirror = true;
#else
static constexpr bool kConfiguredHMirror = false;
#endif
#if CONFIG_XIAOZHI_CAMERA_VFLIP
static constexpr bool kConfiguredVFlip = true;
#else
static constexpr bool kConfiguredVFlip = false;
#endif
#endif

Esp32Camera::Esp32Camera(const camera_config_t &config) {
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed with error 0x%x", err);
        return;
    }

    ApplySensorSettings(config);
    streaming_on_ = true;
}

// sensor 设置：GC0308 特例 + Kconfig 里配置好的 mirror/flip（构造与 Reinit 共用）
void Esp32Camera::ApplySensorSettings(const camera_config_t &config) {
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 0); // Control camera mirror: 1 for mirror, 0 for normal
        }
#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED
        s->set_hmirror(s, kConfiguredHMirror ? 1 : 0);
        s->set_vflip(s, kConfiguredVFlip ? 1 : 0);
#endif
        ESP_LOGI(TAG, "Camera initialized: format=%d", config.pixel_format);
    }
}

// 释放资源并 deinit（析构与 Reinit 共用；可重复调用）
// 先 join 编码线程：它可能正引用 current_fb_ 或 encoder 内部状态。
void Esp32Camera::Release() {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
    if (current_fb_) {
        esp_camera_fb_return(current_fb_);
        current_fb_ = nullptr;
    }
    if (encode_buf_) {
        heap_caps_free(encode_buf_);
        encode_buf_ = nullptr;
        encode_buf_size_ = 0;
    }
    if (streaming_on_) {
        esp_camera_deinit();
        streaming_on_ = false;
    }
}

Esp32Camera::~Esp32Camera() {
    Release();
}

bool Esp32Camera::Reinit(const camera_config_t &config) {
    Release();  // 彻底释放旧帧池与编码缓冲，避免跨模式残留
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reinit: esp_camera_init failed with error 0x%x", err);
        return false;
    }
    ApplySensorSettings(config);
    streaming_on_ = true;
    return true;
}

void Esp32Camera::SetExplainUrl(const std::string &url, const std::string &token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    if (!streaming_on_) {
        return false;
    }

    // Get the latest frame, discard old frames for real-time performance
    for (int i = 0; i < 2; i++) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
        }
        current_fb_ = esp_camera_fb_get();
        if (!current_fb_) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }
    }

    // Prepare encode buffer for RGB565 format (with optional byte swapping)
    if (current_fb_->format == PIXFORMAT_RGB565) {
        size_t pixel_count = current_fb_->width * current_fb_->height;
        size_t data_size = pixel_count * 2;

        // Allocate or reallocate encode buffer if needed
        if (encode_buf_size_ < data_size) {
            if (encode_buf_) {
                heap_caps_free(encode_buf_);
            }
            encode_buf_ = (uint8_t *)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (encode_buf_ == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate memory for encode buffer");
                encode_buf_size_ = 0;
                return false;
            }
            encode_buf_size_ = data_size;
        }

        // Copy data to encode buffer with optional byte swapping
        uint16_t *src = (uint16_t *)current_fb_->buf;
        uint16_t *dst = (uint16_t *)encode_buf_;
        if (swap_bytes_enabled_) {
            for (size_t i = 0; i < pixel_count; i++) {
                dst[i] = __builtin_bswap16(src[i]);
            }
        } else {
            memcpy(encode_buf_, current_fb_->buf, data_size);
        }

        // Allocate separate buffer for preview display
        uint8_t *preview_data = (uint8_t *)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (preview_data != nullptr) {
            memcpy(preview_data, encode_buf_, data_size);
            auto display = dynamic_cast<LvglDisplay *>(Board::GetInstance().GetDisplay());
            if (display != nullptr) {
                display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(preview_data, data_size, current_fb_->width, current_fb_->height, current_fb_->width * 2, LV_COLOR_FORMAT_RGB565));
            } else {
                heap_caps_free(preview_data);
            }
        }
    } else if (current_fb_->format == PIXFORMAT_JPEG) {
        // 本板相机直出 JPEG：解码成 RGB565 才能给 LVGL 预览（实现见文件头的 DecodeJpegPreview）。
        DecodeJpegPreview(current_fb_->buf, current_fb_->len);
    }

    ESP_LOGI(TAG, "Captured frame: %dx%d, len=%zu, format=%d",
             current_fb_->width, current_fb_->height, current_fb_->len, current_fb_->format);

    return true;
}

bool Esp32Camera::SetHMirror(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_hmirror(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_vflip(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetSwapBytes(bool enabled) {
    swap_bytes_enabled_ = enabled;
    return true;
}

namespace {
// image_to_jpeg_cb 的回调：一次性把整块 JPEG 拷进调用方缓冲（不做任何动态分配）。
struct JpegSink {
    uint8_t *buf;
    size_t capacity;
    size_t len;
};

size_t JpegSinkCb(void *arg, size_t index, const void *data, size_t len) {
    auto *sink = static_cast<JpegSink *>(arg);
    // index==0 携带完整 JPEG；后续调用为分块/哨兵，本用法忽略。
    if (index == 0 && data != nullptr && len > 0 && len <= sink->capacity) {
        memcpy(sink->buf, data, len);
        sink->len = len;
    }
    return len;
}
}  // namespace

bool Esp32Camera::EncodeCurrentFrameToJpeg(uint8_t *out, size_t out_capacity, size_t &out_len) {
    out_len = 0;
    if (current_fb_ == nullptr || out == nullptr || out_capacity == 0) {
        return false;
    }
    // 源数据与 Explain() 保持一致：RGB565 必须用 Capture() 里**已换好字节序**的 encode_buf_，
    // 若在此再换一次会导致网页照片红蓝互换；其余格式直接用原始帧。
    uint8_t *src = current_fb_->buf;  // image_to_jpeg_cb 需要非 const 指针
    size_t src_len = current_fb_->len;
    v4l2_pix_fmt_t enc_fmt;
    switch (current_fb_->format) {
        case PIXFORMAT_RGB565: {
            const size_t need = static_cast<size_t>(current_fb_->width) * current_fb_->height * 2;
            if (encode_buf_ == nullptr || encode_buf_size_ < need) {
                ESP_LOGE(TAG, "EncodeCurrentFrameToJpeg: encode buffer not ready");
                return false;
            }
            src = encode_buf_;
            src_len = encode_buf_size_;
            enc_fmt = V4L2_PIX_FMT_RGB565;
            break;
        }
        case PIXFORMAT_YUV422:
            enc_fmt = V4L2_PIX_FMT_YUYV;  // YUV422 is actually YUYV format
            break;
        case PIXFORMAT_YUV420:
            enc_fmt = V4L2_PIX_FMT_YUV420;
            break;
        case PIXFORMAT_GRAYSCALE:
            enc_fmt = V4L2_PIX_FMT_GREY;
            break;
        case PIXFORMAT_JPEG:
            // 相机直出 JPEG（本板单一模式）：不动，走上游 image_to_jpeg_cb 的直通分支
            // （由 CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT 开启，形状：cb(0,整张)+cb(1,哨兵)）。
            enc_fmt = V4L2_PIX_FMT_JPEG;
            break;
        case PIXFORMAT_RGB888:
            enc_fmt = V4L2_PIX_FMT_RGB24;
            break;
        default:
            ESP_LOGE(TAG, "EncodeCurrentFrameToJpeg: unsupported format %d", current_fb_->format);
            return false;
    }

    JpegSink sink = {out, out_capacity, 0};
    if (!image_to_jpeg_cb(src, src_len, current_fb_->width, current_fb_->height, enc_fmt, 80,
                          JpegSinkCb, &sink) ||
        sink.len == 0) {
        ESP_LOGE(TAG, "EncodeCurrentFrameToJpeg: JPEG encode failed");
        return false;
    }
    out_len = sink.len;
    return true;
}

void Esp32Camera::SetJpegObserver(std::function<void(const uint8_t *jpeg, size_t len)> cb) {
    jpeg_observer_ = std::move(cb);
}

// image_to_jpeg_cb 的输出回调：把完整 JPEG 投递给上传队列，并通知板级观察者（TF 卡留档）。
// 注意：image_to_jpeg_cb 只收普通函数指针，捕获 this 的 lambda 转不过去，
// 所以这里必须是**静态成员函数**，需要的东西都从 EncodeCtx(arg) 里取。
// 静态成员函数仍可访问类的私有成员，所以 jpeg_observer_ 直接调用。
size_t Esp32Camera::JpegEncodeCb(void *arg, size_t index, const void *data, size_t len) {
    auto *ctx = static_cast<EncodeCtx *>(arg);
    QueueHandle_t jpeg_queue = ctx->queue;
    JpegChunk chunk = {.data = nullptr, .len = len};
    if (index == 0 && data != nullptr && len > 0) {
        chunk.data =
            (uint8_t *)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (chunk.data == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes for JPEG chunk", len);
            chunk.len = 0;
        } else {
            memcpy(chunk.data, data, len);
        }
        // 板级观察者：index==0 就是完整 JPEG。先备好上传数据再通知，
        // 即使观察者写卡较慢也不影响上传（上传线程从队列取数据，两者并行）。
        // ⚠ data 只在本回调期间有效，观察者必须**当场**用完（如直接 fwrite）。
        if (ctx->self->jpeg_observer_) {
            ctx->self->jpeg_observer_(static_cast<const uint8_t *>(data), len);
        }
    } else {
        chunk.len = 0;  // Sentinel or error
    }
    xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
    return len;
}

std::string Esp32Camera::Explain(const std::string &question) {
    if (explain_url_.empty()) {
        throw std::runtime_error("Image explain URL or token is not set");
    }

    if (current_fb_ == nullptr) {
        throw std::runtime_error("No camera frame captured");
    }

    // Create local JPEG queue
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        throw std::runtime_error("Failed to create JPEG queue");
    }

    // Start encoding thread（必须经 CreateEncoderThread：默认 3KB pthread 栈会栈溢出）
    encoder_thread_ = CreateEncoderThread([this, jpeg_queue]() {
        int64_t start_time = esp_timer_get_time();
        uint16_t w = current_fb_->width;
        uint16_t h = current_fb_->height;
        v4l2_pix_fmt_t enc_fmt;
        switch (current_fb_->format) {
            case PIXFORMAT_RGB565:
                enc_fmt = V4L2_PIX_FMT_RGB565;
                break;
            case PIXFORMAT_YUV422:
                enc_fmt = V4L2_PIX_FMT_YUYV;  // YUV422 is actually YUYV format
                break;
            case PIXFORMAT_YUV420:
                enc_fmt = V4L2_PIX_FMT_YUV420;
                break;
            case PIXFORMAT_GRAYSCALE:
                enc_fmt = V4L2_PIX_FMT_GREY;
                break;
            case PIXFORMAT_JPEG:
                enc_fmt = V4L2_PIX_FMT_JPEG;
                break;
            case PIXFORMAT_RGB888:
                enc_fmt = V4L2_PIX_FMT_RGB24;
                break;
            default:
                ESP_LOGE(TAG, "Unsupported pixel format: %d", current_fb_->format);
                return;
        }

        // Use encode buffer for RGB565, otherwise use original frame buffer
        uint8_t *jpeg_src_buf = current_fb_->buf;
        size_t jpeg_src_len = current_fb_->len;
        if (current_fb_->format == PIXFORMAT_RGB565 && encode_buf_ != nullptr) {
            jpeg_src_buf = encode_buf_;
            jpeg_src_len = encode_buf_size_;
        }

        EncodeCtx ctx = {jpeg_queue, this};
        bool ok = image_to_jpeg_cb(jpeg_src_buf, jpeg_src_len, w, h, enc_fmt, 80,
                                   JpegEncodeCb, &ctx);

        if (!ok) {
            JpegChunk chunk = {.data = nullptr, .len = 0};
            xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
        }
        int64_t end_time = esp_timer_get_time();
        // stack free：编码+写卡后的栈剩余量（字节）。接近 0 说明栈又要不够了，先看这里。
        ESP_LOGI(TAG, "JPEG encoding time: %ld ms, stack free=%u", int((end_time - start_time) / 1000),
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    });

    auto network = Board::GetInstance().GetNetwork();
    // 上传解释用的 HTTP 客户端**刻意常驻复用**，不随每次拍照析构。
    //
    // 原因（2026-09 真机崩溃，backtrace 落在 http_client.cc 的 OnTcpDisconnected）：
    //   本请求头是 `Connection: close`，服务器响应完就主动关连接，于是两条线程赛跑：
    //     tcp_receive 任务：recv 返 0 → EspTcp::DoDisconnect(false) → 回调 OnTcpDisconnected()
    //     主任务：ReadAll() 返 → Close()（connected_ 已 false，**直接 return，不等接收任务**）
    //             → Explain 返回 → unique_ptr<Http> 析构 → **mutex_ 被销毁**
    //   接收任务这时才 lock(mutex_) → pthread_mutex_lock 拿到已销毁的 sem
    //     → assert(xQueueSemaphoreTake … (pxQueue)) → panic 重启。
    //   窗口只有微秒级，所以表现为“偶发”，且任何打乱时序的操作（比如先网页拍一张）
    //   都会改变命中概率 —— 那是躲开子弹，不是治病。
    //
    // 常驻后 mutex_ 一直有效，晚一步的回调不再致命；下一次拍照重建 TCP 连接时，
    // 上一次的接收任务早已退出（间隔是“秒”，它只需“毫秒”）。
    //
    // ⚠ 这是对上游组件缺陷的**规避**而非根治：78/esp-ml307 到 main 分支的
    //   ~HttpClient / EspTcp::Disconnect 仍是同样写法，上游修好后可改回每次新建。
    // ⚠ Explain 由 MCP 工具串行调用，所以共用单个实例是安全的。
    static std::unique_ptr<Http> explain_http;
    if (explain_http == nullptr) {
        explain_http = network->CreateHttp(3);
    }
    auto& http = explain_http;
    http->SetTimeout(15000);  // 上传+解释总超时 15s(默认 30s 太长, 失败会卡住设备流程)
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        encoder_thread_.join();
        JpegChunk chunk;
        while (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) == pdPASS) {
            if (chunk.data != nullptr) {
                heap_caps_free(chunk.data);
            } else {
                break;
            }
        }
        vQueueDelete(jpeg_queue);
        throw std::runtime_error("Failed to connect to explain URL");
    }

    {
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    size_t total_sent = 0;
    bool saw_terminator = false;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            saw_terminator = true;
            break;
        }
        http->Write((const char *)chunk.data, chunk.len);
        total_sent += chunk.len;
        heap_caps_free(chunk.data);
    }
    encoder_thread_.join();
    vQueueDelete(jpeg_queue);

    if (!saw_terminator || total_sent == 0) {
        ESP_LOGE(TAG, "JPEG encoder failed or produced empty output");
        throw std::runtime_error("Failed to encode image to JPEG");
    }

    {
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    http->Write("", 0);

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", http->GetStatusCode());
        throw std::runtime_error("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Explain image size=%dx%d, compressed size=%d, remain stack size=%d, question=%s\n%s",
             current_fb_->width, current_fb_->height, (int)total_sent, (int)remain_stack_size, question.c_str(), result.c_str());
    return result;
}
