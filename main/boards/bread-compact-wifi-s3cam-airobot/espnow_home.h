#ifndef ESPNOW_HOME_H
#define ESPNOW_HOME_H

#include <cstdint>
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
//   主控→节点  @n1 do <能力> <动作> [参数]    通用动作（本层原样透传，不解释）
//   节点→主控  @n1 ok <能力> <结果>           执行回执 → 进状态缓存
//   节点→主控  @n1 say <名字>                 播报请求 → 回调 kind="say"（板级播 <名字>.mp3）
//   节点→主控  @n1 evt <名字> <值>            状态上报 → 进状态缓存，回调 kind="evt"
//   节点→主控  @n1 err <原因> [细节]          错误 → 进状态缓存
//
// 注意：本板 UART0 与 Arduino 下位机共用，本文件一律不使用 ESP_LOG，
//       失败通过返回值/上层工具文本表达（见 AGENTS.md 与本板 README）。
class EspNowHome {
public:
    // 上行回调：kind = "say"（播报请求，已去重）| "evt"（状态上报，已去重）。
    // 状态缓存由本层维护，回调只用于"需要动作"的场景（当前即播报）。
    // 在 WiFi 任务上下文调用，实现方须用 Application::Schedule 切回主任务。
    using EventCallback = std::function<void(int node_id, const std::string& kind,
                                             const std::string& name, const std::string& arg,
                                             int64_t ts_ms)>;

    static constexpr int kMaxNodes = 4;
    static constexpr int kMaxCaps = 3;               // 每节点能力上限（超出的静默忽略）
    static constexpr int kNameLen = 20;              // 设备名缓冲（UTF-8，中文约 6 字）
    static constexpr int kCapNameLen = 14;
    static constexpr int kCapSpecLen = 48;           // 能力规格文本（含描述/动作/参数）
    static constexpr int kStateLen = 96;             // "light=1 0 0 255 dist=23" 形式
    static constexpr int64_t kOfflineMs = 15000;     // 主控侧判离线（节点侧回 hop 是 5s，勿混）
    static constexpr int64_t kDedupWindowMs = 1000;  // 上行去重窗口（抵消节点 3 连发）
    static constexpr int kBeaconFastMs = 500;        // 启动期 beacon 周期
    static constexpr int kBeaconSlowMs = 3000;       // 稳态 beacon 周期
    static constexpr int kBeaconFastCount = 60;      // 快发次数（≈30 秒）
    static constexpr int kMaxPacketLen = 200;        // 单包上限（IDF v1.0 上限 250B）

    // 初始化 ESP-NOW（幂等：重复调用返回已启动状态）。成功返回 true。
    bool Begin(EventCallback cb);

    // 下行：向节点发送正文（内部加 "@n<id> " 前缀）。单次非阻塞发送：
    // 节点常醒（USB 供电），无需重发；连发会阻塞 MCP 工具回调、卡住对话。
    bool SendTo(int node_id, const std::string& body);

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

    DeviceEntry* FindDevice(int node_id);
    DeviceEntry* FindOrCreateDevice(int node_id, const uint8_t* mac);
    void HandleInfo(DeviceEntry& dev, const char* text);   // text = "<名字> <能力规格>"
    static void UpsertState(DeviceEntry& dev, const char* key, const char* value);
    bool ShouldDeliver(int node_id, const std::string& kind, const std::string& name,
                       int64_t now_ms);
    void HandleRecv(const uint8_t* src_mac, const uint8_t* data, int len);
    void StartBeacon(int period_ms);

    static void RecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len);
    static void BeaconTimerCb(void* arg);

    EventCallback callback_;
    DeviceEntry devices_[kMaxNodes];
    DedupEntry dedup_[kMaxNodes * 2];
    int dedup_pos_ = 0;
    esp_timer_handle_t beacon_timer_ = nullptr;
    int beacon_count_ = 0;
    bool started_ = false;
};

#endif  // ESPNOW_HOME_H
