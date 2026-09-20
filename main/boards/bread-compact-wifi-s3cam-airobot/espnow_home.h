#ifndef ESPNOW_HOME_H
#define ESPNOW_HOME_H

#include <cstdint>
#include <atomic>
#include <functional>
#include <string>

#include "esp_now.h"
#include "esp_timer.h"

// ESP-NOW 居家节点传输层（板级）。
//
// 数据驱动设计（2026-09-18 重构，取代首版"节点角色硬编码"）：
//   节点上线自描述能力（`@n<id> info <名字> <能力规格>`），本层只做"存起来 + 转出去"，
//   **完全不理解任何业务语义**——能力名、动作名、事件名、播报文件名在此都是不透明字符串。
//   因此接入新设备只需改节点固件，主控无需改动、无需重新烧录。
//
// 职责：ESP-NOW 初始化、beacon 发现广播（供节点锁信道）、设备注册表（名字/能力/状态/在线）、
//       上行报文解析与去重、下行报文发送。
// 不含：MCP 工具注册（板级主文件）、播报（板级调 LocalMusicPlayer）。
//
// 协议（与节点固件 EspNowNode.ino 一致，@ 前缀文本行）：
//   节点→主控  @n1 info <名字> <能力规格>     能力自描述（锁信道后 + 每次收到 beacon 重报）
//   主控→节点  @n1#12 do <能力> <动作> [参数]  通用动作（本层原样透传，不解释）
//   节点→主控  @n1#12 ok <能力> <结果>         执行回执 → 进状态缓存 + 链路确认
//   节点→主控  @n1 say <名字>                 播报请求 → 回调 kind="say"（板级播 <名字>.mp3）
//   节点→主控  @n1 evt <名字> <值>            状态上报 → 进状态缓存，回调 kind="evt"
//   节点→主控  @n1 evt hb 1                  心跳（协议保留名）：只刷新在线 + 推动在途命令重发，
//                                          不进状态缓存、不回调业务
//   节点→主控  @n1 err <原因> [细节]          错误 → 进状态缓存
//
// 序号（`#12`）放在**信封**里，正文格式与首版完全一致，因此向后兼容：
//   旧节点固件用 atoi("1#12 ...") 仍得到 1，正文照旧解析 → 命令照样执行（只是回执不带序号）；
//   旧主控下发的不带序号报文，新节点按 seq=0 处理（不去重、回执也不带序号）。
//   两侧任一没更新都不会"彻底不能用"，只是失去 ACK 的精确匹配。
//
// 可靠性设计（依据 IDF v6.0.2 官方文档 docs/zh_CN/api-reference/network/esp_now.rst）：
//   该文档在"发送 ESP-NOW 数据"一节明确列出失败原因（含"设备的信道不相同"）并建议：
//   "应用层可在接收 ESP-NOW 数据时发回一个应答(ACK)，如果接收 ACK 超时，则将重新传输
//     ESP-NOW 数据。可以为 ESP-NOW 数据设置序列号，从而删除重复的数据。"
//   本层即按此实现：序号 + 应用层 ACK + **非阻塞**重传（esp_timer 驱动，不在调用方 sleep），
//   同时用官方 esp_now_register_send_cb() 拿 MAC 层真实发送结果。
//
// 注意：本板 UART0 与 Arduino 下位机共用，本文件一律不使用 ESP_LOG，
//       失败通过返回值/上层工具文本/链路回调表达（见 AGENTS.md 与本板 README）。
class EspNowHome {
public:
    // 上行回调：kind = "say"（播报请求，已去重）| "evt"（状态上报，已去重）。
    // 状态缓存由本层维护，回调只用于"需要动作"的场景（当前即播报）。
    // 在 WiFi 任务上下文调用，实现方须用 Application::Schedule 切回主任务。
    using EventCallback = std::function<void(int node_id, const std::string& kind,
                                             const std::string& name, const std::string& arg,
                                             int64_t ts_ms)>;

    // 链路事件（信道变化 / 命令未确认 / 发送连续失败）：本层不打日志（UART0 与下位机共用），
    // 交给板级决定怎么显示（板级打到独立 TAG ESP-NOW，网页日志面板可见）。
    // what/detail 一律用 const char*：链路事件路径必须**零堆分配**（现场内存本就紧张，
    // 任何一次 std::string 构造都可能在堆碎片化时失败并触发 abort 重启）。
    using LinkCallback = std::function<void(const char* what, const char* detail)>;

    static constexpr int kMaxNodes = 4;
    static constexpr int kMaxCaps = 4;               // 每节点能力上限（融合节点有 4 个；超出的静默忽略）
    static constexpr int kNameLen = 20;              // 设备名缓冲（UTF-8，中文约 6 字）
    static constexpr int kCapNameLen = 14;
    static constexpr int kCapSpecLen = 48;           // 能力规格文本（含描述/动作/参数）
    static constexpr int kStateLen = 96;             // "light=1 0 0 255 dist=23" 形式
    static constexpr int64_t kOfflineMs = 15000;     // 主控侧判离线（节点侧回 hop 是 5s，勿混）
    static constexpr int64_t kDedupWindowMs = 1000;  // 上行去重窗口（抵消节点 3 连发）
    static constexpr int kBeaconFastMs = 500;        // 启动期 / 快速窗口 beacon 周期
    static constexpr int kBeaconSlowMs = 3000;       // 稳态 beacon 周期
    static constexpr int kBeaconFastCount = 60;      // 启动期快发次数（≈30 秒）
    static constexpr int kMaxPacketLen = 200;        // 单包上限（IDF v1.0 上限 250B）

    // ---- 下行可靠性参数（见文件头"可靠性设计"）----
    static constexpr int kCmdRetryMax = 4;           // 首包之后最多重传次数（150ms 一次）
    static constexpr int kCmdRetryGapMs = 150;       // 重传间隔（非阻塞，定时器驱动）
    static constexpr int kCmdPendingMs = 7000;       // 未确认命令挂起上限（节点 hop 一圈 6.5s + 余量）
                                                     // 超时即判"真不在信道"，不无限等，保住实时性
    static constexpr int kRetryTickMs = 100;         // 重传定时器周期（仅在有在途命令时运行）
    static constexpr int kChannelPollMs = 1000;      // 信道看护周期（蹭 beacon 定时器，不额外常驻）
    static constexpr int kFastBeaconWindowMs = 8000; // 快速 beacon 窗口长度（信道变化/命令未确认）
    static constexpr int kTxFailStreakToFast = 3;    // 连续发送失败多少次就提速 beacon

    // 初始化 ESP-NOW（幂等：重复调用返回已启动状态）。成功返回 true。
    bool Begin(EventCallback cb, LinkCallback link = nullptr);

    // 下行：向节点发送正文（内部加 "@n<id>#<seq> " 前缀并登记在途命令）。
    // 非阻塞：只发首包 + 挂起重传（由 esp_timer 驱动），绝不在调用方循环连发
    // （MCP 工具回调里连发会卡住对话）。返回 false 表示节点未知/报文非法/入队失败。
    bool SendTo(int node_id, const std::string& body);

    // 该节点是否还有未被 ACK 的在途命令（工具返回文本用：区分"已确认"与"已下发待确认"）
    bool IsPending(int node_id) const;
    int PendingCount() const;
    int ConfirmedCount() const { return confirmed_count_; }
    int TxFailCount() const { return tx_fail_count_; }

    bool IsOnline(int node_id) const;

    // ---- 设备注册表查询（供板级 MCP 工具使用）----

    // 全量设备 JSON：
    // {"nodes":[{"id":1,"name":"客厅灯","online":true,"info_seen":true,
    //            "caps":[{"name":"light","spec":"(RGB灯):on(0|1)"}],
    //            "state":"light=1 0 0 255","age_ms":1234}, ...]}
    std::string DevicesJson() const;

    // 是否见过该节点（用于区分"从未上线"与"已离线"）
    bool HasNode(int node_id) const;

    // 是否已上报过该能力（用于给出可读的错误提示）
    bool HasCap(int node_id, const std::string& cap) const;

    // 设备名；未知或未上报名字时返回 "节点<id>"。
    // 返回内部静态缓冲，调用方须立即使用（不要保存指针）。
    const char* NodeName(int node_id) const;

    static int64_t NowMs();

private:
    struct CapEntry {
        char name[kCapNameLen] = {0};
        char spec[kCapSpecLen] = {0};   // 规格原文，本层不解释其含义
    };

    struct DeviceEntry {
        bool known = false;
        int id = 0;
        uint8_t mac[6] = {0};
        char name[kNameLen] = {0};
        CapEntry caps[kMaxCaps];
        int cap_count = 0;
        char state[kStateLen] = {0};    // 通用 key=value 缓存（light=... / dist=... / err=...）
        int64_t last_seen_ms = 0;
        bool info_seen = false;         // 是否已收到能力描述（决定 devices 里能否给出能力）
    };

    struct DedupEntry {
        int node_id = 0;
        char key[24] = {0};             // "say motion" / "evt dist"
        int64_t ts_ms = 0;
        bool used = false;
    };

    // 在途命令（每节点最多 1 条：同一节点的新命令覆盖旧命令，与"一条一条执行"的语义一致）
    struct PendingCmd {
        bool used = false;
        int node_id = 0;
        uint16_t seq = 0;
        uint8_t mac[6] = {0};
        char body[96] = {0};       // 不含 "@n<id>#<seq> " 前缀的正文（命令都很短）
        int retries_left = 0;      // 还剩几次"按间隔重发"
        int64_t next_ms = 0;       // 下一次重发时刻
        int64_t expire_ms = 0;     // 超过即放弃（记一次未确认）
    };

    DeviceEntry* FindDevice(int node_id);
    DeviceEntry* FindOrCreateDevice(int node_id, const uint8_t* mac);
    void HandleInfo(DeviceEntry& dev, const char* text);   // text = "<名字> <能力规格>"
    static void UpsertState(DeviceEntry& dev, const char* key, const char* value);
    bool ShouldDeliver(int node_id, const std::string& kind, const std::string& name,
                       int64_t now_ms);
    void HandleRecv(const uint8_t* src_mac, const uint8_t* data, int len);
    void StartBeacon(int period_ms);

    // ---- 下行可靠性 ----
    PendingCmd* FindPending(int node_id);
    bool DispatchPending(PendingCmd& p);            // 真正调用 esp_now_send
    void CompletePending(int node_id, bool confirmed, const char* why);
    void RetryTickRaw();                          // 定时器回调体
    void EnsureRetryTimer();
    void StopRetryTimerIfIdle();
    // ---- 信道看护 / 快速 beacon ----
    void PollChannel(int64_t now_ms);
    void EnterFastBeacon(const char* why, const char* detail);
    void NotifyLink(const char* what, const char* detail);

    static void RecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len);
    static void SendCb(const esp_now_send_info_t* tx_info, esp_now_send_status_t status);
    static void BeaconTimerCb(void* arg);
    static void RetryTimerCb(void* arg);

    EventCallback callback_;
    LinkCallback link_cb_;
    DeviceEntry devices_[kMaxNodes];
    DedupEntry dedup_[kMaxNodes * 2];
    int dedup_pos_ = 0;
    PendingCmd pending_[kMaxNodes];
    uint16_t seq_next_ = 1;
    esp_timer_handle_t beacon_timer_ = nullptr;
    esp_timer_handle_t retry_timer_ = nullptr;
    int beacon_count_ = 0;
    int beacon_period_ms_ = 0;            // 当前 beacon 周期（避免每 tick 重启定时器）
    int64_t fast_beacon_until_ms_ = 0;    // 快速窗口结束时刻（0=未启用）
    int64_t channel_check_ms_ = 0;        // 上次检查信道时刻
    uint8_t last_channel_ = 0;            // 上次看到的信道（0=还没查过）
    // send_cb 跑在高优先级 Wi-Fi 任务，定时器在 esp_timer 任务读：用原子量而不是 volatile
    // （volatile 不提供原子性，C++20 起对 volatile 做 ++ 等读改写还会 -Werror）
    std::atomic<int> tx_fail_streak_{0};       // 连续发送失败次数
    std::atomic<bool> fast_request_{false};    // send_cb 请求提速 beacon（由定时器消费）
    int tx_fail_count_ = 0;
    int confirmed_count_ = 0;
    int unconfirmed_count_ = 0;
    bool started_ = false;
};

#endif  // ESPNOW_HOME_H
