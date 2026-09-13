#pragma once

#include <cstddef>
#include <cstdint>

// 网页实时日志捕获（本板专属调试能力）。
//
// 背景：本板 console 与 Arduino 下位机控制指令**共用 UART0(GPIO43)**，日志会物理地
// 送进 Arduino 的 RX。于是"要开 INFO 日志定位问题"和"不能干扰下位机指令"无法两全——
// 要看的日志越多，灌给 Arduino 的噪声越多。
//
// 本模块用 esp_log_set_vprintf() 接管 ESP_LOGx 输出，改写进内部 RAM 环形缓冲，
// 由 web 页面（/ws 的 log_pull）按序号增量拉取。默认**不向 UART0 输出**，
// 因此把级别开到 INFO/DEBUG 也不会污染下位机指令流。
//
// 逃生开关：LogCaptureSetUartMirror(true) 恢复传统串口日志（会干扰 Arduino），
// 供网页不可用时使用（网页开关 / AI 工具 self.debug.log_to_serial）。

// 安装日志捕获钩子（幂等）。应在日志产生前尽早调用。
void LogCaptureInit();

// 追加一条**非日志类**的调试记录（如 ESP32↔Arduino 的指令收发）到同一个环形缓冲。
// 与系统日志共用缓冲、共用网页拉取通道，因此不额外占内存；正常运行时日志级别是
// ERROR、几乎没有系统日志，指令流不会被冲掉。
// 约定格式（前端据此高亮/过滤）：
//   [UNO] > @go-forward-10      实际发出的指令
//   [UNO] ! @go-forward-10 （防抖丢弃）  被防抖拦下、并未发出
//   [UNO] < @done go-forward-10 收到的下位机回执
void LogCaptureAppend(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// 是否把日志同时镜像到 UART0（默认 false = 不干扰 Arduino）。
void LogCaptureSetUartMirror(bool on);
bool LogCaptureGetUartMirror();

// 拉取自 since_seq 之后的日志到 buf，最多写 buf_size 字节。
// since_seq 传 0（或已被环形缓冲滚过的过期值）时从缓冲里最旧的一条开始给。
// 一次装不下时只交付最早的一段，调用方按 next_seq 继续拉即可追平（不丢数据）。
// out_len: 实际写入字节数；next_seq: 已交付到的序号，下次调用回传。
// 注意：buf 由调用方提供，本模块不做任何动态分配。
void LogCapturePull(uint32_t since_seq, char* buf, size_t buf_size, size_t& out_len,
                    uint32_t& next_seq);
