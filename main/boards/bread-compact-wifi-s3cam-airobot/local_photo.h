#pragma once

#include <cstddef>
#include <cstdint>

class Esp32Camera;  // 前置声明：只需指针，避免头文件互相包含

// 本板「网页拍照」服务（板级私有）。
//
// 与 AI 拍照(Esp32Camera::Explain)的分工：
//   * AI 拍照：Capture() + 编码 + **上传到云端**解释（走 api.xiaozhi.me，内存峰值高）
//   * 网页拍照：Capture() + 编码 → 存进本模块的 PSRAM 常驻缓冲 → GET /photo.jpg 回给浏览器
// 两条路复用同一个 camera 驱动与同一个编码器，串行调用（内部有带超时的互斥）。
//
// 内存：JPEG 缓冲常驻 PSRAM（VGA 约 30~60KB，配额 128KB），**不申请内部 RAM**；
//       编码临时缓冲由 esp32-camera 在 PSRAM 分配。
// 失败一律通过返回值上报（本模块不打 ESP_LOG，避免污染 UART0 上的 Arduino 指令流）。

// 初始化：记住 camera 实例并分配 PSRAM JPEG 缓冲。
// camera 由板级持有（`camera_`），本模块只借用、不负责释放。
// 失败返回 false（网页拍照按钮会回 HTTP 500，不影响其它功能）。
bool LocalPhotoInit(Esp32Camera* camera, size_t capacity_bytes);

// 抓一帧并编码成 JPEG（阻塞约 200ms，在 httpd 任务里调用即可）。成功返回 true。
// 与 AI 拍照互斥且有 300ms 超时：拿不到锁直接返回 false，绝不长时间占住 httpd 任务。
bool LocalPhotoCapture();

// 最近一次成功的 JPEG 数据/长度（未拍过时返回 nullptr/0）。
const uint8_t* LocalPhotoData();
size_t LocalPhotoSize();
