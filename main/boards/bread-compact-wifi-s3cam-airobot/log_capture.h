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
//
// 崩溃现场留存的边界（重要，改代码时保持一致）：
//  * 本模块**只能保留 ESP_LOGx 产生的日志**。panic/abort 的 Guru Meditation、backtrace、
//    `stack overflow in task xxx` 由 IDF panic handler 用 ROM printf 直写 UART0，
//    **不经过** esp_log_set_vprintf() 的钩子 —— 所以网页里永远看不到它们；
//    要拿 backtrace 必须接 USB 串口（且不必打开串口镜像开关）。
//  * 软重启(panic/看门狗/OTA/esp_restart)后环形缓冲内容会保留，网页能看到"崩溃前的日志"；
//    掉电重启(冷启动)则丢弃，避免把随机 RAM 当日志显示。
//  * 重启原因由 esp_reset_reason() 给出，以分隔行的形式写进日志流（含 PANIC/看门狗/欠压等文案）。

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

// 清空设备侧环形缓冲（网页「🗑 清空设备缓冲」按钮用）。
// 注意：只影响设备侧历史，不影响浏览器已显示的内容（那部分由前端脚本清）。
void LogCaptureClear();
