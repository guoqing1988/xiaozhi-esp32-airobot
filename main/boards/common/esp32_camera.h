#pragma once
#include "sdkconfig.h"

#include <lvgl.h>
#include <functional>
#include <thread>
#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "esp_camera.h"
#include "jpg/image_to_jpeg.h"

struct JpegChunk
{
    uint8_t *data;
    size_t len;
};

class Esp32Camera : public Camera
{
private:
    bool streaming_on_ = false;
    bool swap_bytes_enabled_ = true;  // Swap pixel byte order for RGB565, enabled by default
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;
    camera_fb_t *current_fb_ = nullptr;
    uint8_t *encode_buf_ = nullptr;  // Buffer for JPEG encoding (with optional byte swap)
    size_t encode_buf_size_ = 0;
    // AI 拍照（Explain）编码完成时的回调，用于板级顺手存卡（默认空 → 其它板行为不变）
    std::function<void(const uint8_t *jpeg, size_t len)> jpeg_observer_;

    // 编码回调的上下文：image_to_jpeg_cb 只收**函数指针**（不能传捕获 lambda），
    // 所以把队列与 this 打包成 arg 传进去，回调本体用静态成员函数。
    struct EncodeCtx {
        QueueHandle_t queue;
        Esp32Camera *self;
    };
    static size_t JpegEncodeCb(void *arg, size_t index, const void *data, size_t len);

    // 释放当前帧/编码缓冲并 deinit（析构与 Reinit 共用；可重复调用）
    void Release();
    // 套用与构造函数相同的 sensor 设置（GC0308 特例 + Kconfig 的 mirror/flip）
    void ApplySensorSettings(const camera_config_t &config);

public:
    Esp32Camera(const camera_config_t &config);
    ~Esp32Camera();

    // 用新配置重新初始化相机（先彻底释放再 init，并重新套用 sensor 设置）。
    // 用途：实时视频流期间切到 PIXFORMAT_JPEG（模组直出 JPEG，零编码零拷贝），
    // 退出视频流时切回 PIXFORMAT_RGB565（恢复拍照与 LCD 预览）。
    // ⚠ 会丢弃 current_fb_ 与 encode_buf_（调用方需确保没有在用）；
    // 失败返回 false 且相机处于**未初始化**状态，调用方必须处理（通常回退到原配置）。
    bool Reinit(const camera_config_t &config);

    virtual void SetExplainUrl(const std::string &url, const std::string &token) override;
    virtual bool Capture() override;
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual bool SetSwapBytes(bool enabled) override;
    virtual std::string Explain(const std::string &question) override;

    // 把**已捕获**的当前帧(RGB565)编码成 JPEG 写入 out（网页拍照用）。
    // 为什么不另抓一帧：本板 fb_count=1，帧池只有一块；另开帧缓冲
    // (fb_count=2) 会让 cam_hal 多占 ~30KB DMA **内部** RAM，本板内部 SRAM 扛不住。
    // 前置条件：先调用 Capture() 成功。返回是否成功，成功时 out_len 为 JPEG 字节数。
    // 复用与 Explain() 相同的字节序处理与编码参数；编码期间不可并发调用 Capture()。
    bool EncodeCurrentFrameToJpeg(uint8_t *out, size_t out_capacity, size_t &out_len);

    // 注册/注销 JPEG 观察者：Explain() 编码完成时（拿到**完整** JPEG 的那一刻）回调。
    // 板级用它把 AI 拍的照片顺手存进 TF 卡；默认未注册 → 行为与以前完全一致。
    // ⚠ 回调运行在 Explain() 的**编码线程**里（栈已显式放大到 16KB，见 .cc 的 CreateEncoderThread），
    //   且 jpeg 指针只在回调期间有效：必须在回调内同步用完（如直接 fwrite），
    //   不得只记下指针稍后再读。
    //   回调也不得阻塞过久（写卡 100~300ms 可接受：上传线程已在并行取队列数据）。
    void SetJpegObserver(std::function<void(const uint8_t *jpeg, size_t len)> cb);
};
