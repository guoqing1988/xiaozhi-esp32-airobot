#include "log_capture.h"

#include <esp_log.h>
#include <esp_system.h>

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

// 环形缓冲与游标放 .noinit 段：软重启(panic/看门狗/OTA/esp_restart)后 DRAM 内容保留，
// 于是"崩溃前的日志"不会被清掉，网页重连后按序号拉取仍能看到；上电冷启动时该段内容
// 是随机的，用下面 magic/version/size/head 四重校验识别并丢弃（见 LogCaptureInit）。
// 注意：这是**段平移**，不新增任何内存占用（原来就是 4KB 内部 RAM，只是从 .bss 挪过来）。
//
// ⚠ 若实测发现某些复位类型下 .noinit 未保留（例如被 bootloader 覆盖），回退方案是
//   把 s_store 换成 RTC_NOINIT_ATTR（RTC slow mem，S3 上 8KB，够放 4KB），其余逻辑不用改。
constexpr uint32_t kRingMagic = 0x4C4F4731u;  // "LOG1"
constexpr uint32_t kRingVersion = 1u;

struct RingStore {
    uint32_t magic;    // == kRingMagic 才算有效
    uint32_t version;  // == kRingVersion，避免升级后误读旧布局
    uint32_t size;     // == kRingSize，避免改尺寸后误读
    uint32_t head;     // 下一个写入位置，必须 < kRingSize
    uint32_t total;    // 累计写入字节数(单调递增)，作为拉取序号
    uint32_t boots;    // 软重启计数（仅用于日志展示）
    char ring[kRingSize];
};
RingStore s_store __attribute__((section(".noinit")));

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
    // 防护：LogCaptureInit() 之前(或冷启动段内容随机时) head 可能是任意 32 位值，
    // 直接拿去索引 ring[] 会越界踩坏内存(表现为“莫名其妙的重启”)。这里先归一化。
    size_t head = s_store.head;
    if (head >= kRingSize) {
        head = 0;
        s_store.head = 0;
        s_store.total = 0;
    }
    const size_t first = (len < kRingSize - head) ? len : kRingSize - head;
    memcpy(s_store.ring + head, data, first);
    if (len > first) {
        memcpy(s_store.ring, data + first, len - first);
    }
    s_store.head = (head + len) % kRingSize;
    s_store.total += static_cast<uint32_t>(len);
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

// 复位原因 → 人话。用于"设备重启后告诉用户到底是怎么挂的"：
// PANIC 多为内存不足/空指针/栈溢出，INT_WDT/TASK_WDT 多为阻塞或饥饿，BROWNOUT 是供电。
const char* ResetReasonText(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:
            return "上电启动(冷启动)";
        case ESP_RST_EXT:
            return "外部复位脚";
        case ESP_RST_SW:
            return "软件重启(esp_restart/OTA)";
        case ESP_RST_PANIC:
            return "PANIC 异常或 abort(多为内存不足/空指针/栈溢出)";
        case ESP_RST_INT_WDT:
            return "中断看门狗(临界区过长或中断被饿死)";
        case ESP_RST_TASK_WDT:
            return "任务看门狗(某任务长时间不让出 CPU)";
        case ESP_RST_WDT:
            return "其它看门狗";
        case ESP_RST_DEEPSLEEP:
            return "深睡眠唤醒";
        case ESP_RST_BROWNOUT:
            return "欠压重启(供电不足)";
        case ESP_RST_SDIO:
            return "SDIO 复位";
        default:
            return "未知原因";
    }
}

// 只有这些复位原因才认为"上一轮 RAM 里的日志值得保留"。
// 上电/未知/deep sleep 一律丢弃，避免把随机 DRAM 当成日志显示给用户。
// （刻意不写 default 放行：新增枚举值时应显式决定，而不是默认保留。）
bool ShouldKeepPreviousLog(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_SW || r == ESP_RST_BROWNOUT;
}

}  // namespace

void LogCaptureInit() {
    if (s_orig_vprintf != nullptr) {
        return;  // 幂等：重复调用不再二次包装
    }

    const esp_reset_reason_t reason = esp_reset_reason();
    const bool state_ok =
        (s_store.magic == kRingMagic) && (s_store.version == kRingVersion) &&
        (s_store.size == kRingSize) && (s_store.head < kRingSize);

    if (state_ok && ShouldKeepPreviousLog(reason)) {
        // 软重启：保留崩溃前的日志，追加一行醒目的分隔（走 RingWrite，不能调 ESP_LOGx 以免递归）
        s_store.boots++;
        char line[kLineMax];
        int n = snprintf(line, sizeof(line),
                         "\n================ 设备重启 #%u：%s ================\n"
                         "（以上为本轮重启前的日志，已从 RAM 中保留）\n\n",
                         static_cast<unsigned>(s_store.boots), ResetReasonText(reason));
        if (n > 0) {
            size_t len = (static_cast<size_t>(n) < sizeof(line)) ? static_cast<size_t>(n)
                                                                : sizeof(line) - 1;
            RingWrite(line, len);
        }
    } else {
        // 冷启动 / 段内容非法：整段重置（magic 最后写，避免半初始化状态被误判为有效）
        portENTER_CRITICAL(&s_mux);
        s_store.magic = 0;
        s_store.version = kRingVersion;
        s_store.size = kRingSize;
        s_store.head = 0;
        s_store.total = 0;
        s_store.boots = 0;
        s_store.magic = kRingMagic;
        portEXIT_CRITICAL(&s_mux);
    }

    s_orig_vprintf = esp_log_set_vprintf(LogVprintfHook);
}

void LogCaptureClear() {
    portENTER_CRITICAL(&s_mux);
    s_store.head = 0;
    s_store.total = 0;
    portEXIT_CRITICAL(&s_mux);
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
    const uint32_t total = s_store.total;
    // 同上：head 未初始化时按 0 处理，绝不能带进 ring[] 索引
    const size_t head = (s_store.head < kRingSize) ? s_store.head : 0;
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
    memcpy(buf, s_store.ring + pos, first);
    if (len > first) {
        memcpy(buf + first, s_store.ring, len - first);
    }
    // 交付到哪就报到哪(而不是报到 total)：否则一次装不下时调用方会误以为已经追平
    next_seq = start + static_cast<uint32_t>(len);
    portEXIT_CRITICAL(&s_mux);

    out_len = len;
}
