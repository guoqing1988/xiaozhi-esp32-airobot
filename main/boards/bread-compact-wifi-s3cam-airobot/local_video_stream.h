#pragma once

#include <functional>

// 网页实时视频流：MJPEG over HTTP（浏览器 <img src="/stream"> 原生显示，无需前端解码代码）。
//
// 实现照官方标准做法：
//   * espressif/esp32-camera README 的 jpg_stream_httpd_handler()
//   * espressif/esp-iot-solution examples/camera/video_stream_server
// 即：multipart/x-mixed-replace + httpd_resp_send_chunk，JPEG 帧**直接从 fb->buf 发出**（零拷贝）。
//
// 为什么用独立 httpd：/stream 的 handler 是长循环（一直发帧直到客户端断开），
// 挂在主 httpd（端口 80，跑着 WS 日志/控制/上传）上会把那条任务占死。
//
// 前置条件：调用 Start 前，相机必须已切到 PIXFORMAT_JPEG（模组直出 JPEG → 零编码零拷贝）。
// 本模块不做模式切换（那是板级的事），只负责「取帧 → 分块发送 → 统计」。

// 约每秒回调一次实测帧率与分辨率。
// 为什么设备侧统计：浏览器对 MJPEG <img> 不暴露逐帧事件，拿不到帧率；
// 设备侧统计后由板级经已有 WebSocket 推给页面显示在视频角标上。
using VideoStatCallback = std::function<void(float fps, int width, int height)>;

// 启动视频流服务（独立 httpd，端口 81）。成功返回 true。
// on_stat 可为空（为空则不统计/不上报）。
bool LocalVideoStreamStart(VideoStatCallback on_stat);

// 停止视频流服务并释放其资源（httpd 任务/栈）。可重复调用。
void LocalVideoStreamStop();

bool LocalVideoStreamRunning();
