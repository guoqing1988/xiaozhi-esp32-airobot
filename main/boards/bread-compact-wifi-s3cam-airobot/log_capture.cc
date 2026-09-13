#include "log_capture.h"

#include <esp_log.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"

namespace {

// 环形缓冲刻意放**静态内部 RAM**：临界区内访问 PSRAM 会长时间持锁，
// 可能让等锁的中断超时（见 sdkconfig.defaults 关于中断看门狗的说明），
// 因此这里不用 PSRAM，也不做动态分配。
// 4KB：够存约 30~40 行日志；本板内部 RAM 很紧(实测 free sram 仅 20~25KB)，
// 日志功能静态占用要克制。需要更长的历史再调大。
constexpr size_t kRingSize = 4096;
// 单行格式化缓冲放在栈上：日志可能来自小栈任务(WiFi/定时器回调)，
// 限制单行长度避免把调用方栈打爆；超长行截断（末尾保留换行由格式串自带）。
constexpr size_t kLineMax = 160;

char s_ring[kRingSize];
size_t s_head = 0;     // 下一个写入位置
uint32_t s_total = 0;  // 累计写入字节数(单调递增)，作为拉取序号
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t s_orig_vprintf = nullptr;
std::atomic<bool> s_uart_mirror{false};

// 写环形缓冲（临界区）。钩子与 LogCaptureAppend 共用。
// 可能在任何任务甚至中断上下文被调用，只能用临界区（不可用 mutex/动态分配）。
// 临界区内分两段 memcpy（非逐字节循环）：持锁时间直接决定 WiFi/音频中断被推迟多久，
// 日志级别开到“调试”时这条路径每秒会走很多次，不能用 O(len) 的字节循环。
void RingWrite(const char* data, size_t len) {
    if (len == 0) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    const size_t first = (len < kRingSize - s_head) ? len : kRingSize - s_head;
    memcpy(s_ring + s_head, data, first);
    if (len > first) {
        memcpy(s_ring, data + first, len - first);
    }
    s_head = (s_head + len) % kRingSize;
    s_total += static_cast<uint32_t>(len);
    portEXIT_CRITICAL(&s_mux);
}

// 日志输出钩子：格式化后写环形缓冲。默认不再落 UART0，避免污染 Arduino 指令流。
int LogVprintfHook(const char* fmt, va_list args) {
    char line[kLineMax];
    // va_list 只能消费一次：镜像给原 vprintf 时还要用 args，故此处先复制再格式化。
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n < 0) {
        n = 0;
    }
    size_t len =
        (static_cast<size_t>(n) < sizeof(line)) ? static_cast<size_t>(n) : sizeof(line) - 1;

    // 注意: 这里绝不能调用 ESP_LOGx/printf(会递归自锁)。
    RingWrite(line, len);

    if (s_uart_mirror.load(std::memory_order_relaxed) && s_orig_vprintf != nullptr) {
        return s_orig_vprintf(fmt, args);
    }
    return n;
}

}  // namespace

void LogCaptureInit() {
    if (s_orig_vprintf != nullptr) {
        return;  // 幂等：重复调用不再二次包装
    }
    s_orig_vprintf = esp_log_set_vprintf(LogVprintfHook);
}

void LogCaptureSetUartMirror(bool on) { s_uart_mirror.store(on, std::memory_order_relaxed); }

void LogCaptureAppend(const char* fmt, ...) {
    char line[kLineMax];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    size_t len =
        (static_cast<size_t>(n) < sizeof(line)) ? static_cast<size_t>(n) : sizeof(line) - 1;
    RingWrite(line, len);
}

bool LogCaptureGetUartMirror() { return s_uart_mirror.load(std::memory_order_relaxed); }

void LogCapturePull(uint32_t since_seq, char* buf, size_t buf_size, size_t& out_len,
                    uint32_t& next_seq) {
    out_len = 0;
    if (buf == nullptr || buf_size == 0) {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    const uint32_t total = s_total;
    const size_t head = s_head;
    // 序号为 0(首次拉取)或已被滚过 => 返回环形缓冲里现存的全部内容
    uint32_t start = since_seq;
    if (start == 0 || start > total || (total - start) > kRingSize) {
        start = (total > kRingSize) ? (total - static_cast<uint32_t>(kRingSize)) : 0;
    }
    size_t len = total - start;
    if (len > buf_size) {
        // 一次装不下：只交付**最早**的一段，让调用方按 next_seq 继续追平（不丢数据）
        len = buf_size;
    }
    // start 对应在环形缓冲里的物理位置，按回绕分两段拷贝
    const size_t pos = (head + kRingSize - (total - start)) % kRingSize;
    const size_t first = (len < kRingSize - pos) ? len : kRingSize - pos;
    memcpy(buf, s_ring + pos, first);
    if (len > first) {
        memcpy(buf + first, s_ring, len - first);
    }
    // 交付到哪就报到哪(而不是报到 total)：否则一次装不下时调用方会误以为已经追平
    next_seq = start + static_cast<uint32_t>(len);
    portEXIT_CRITICAL(&s_mux);

    out_len = len;
}
