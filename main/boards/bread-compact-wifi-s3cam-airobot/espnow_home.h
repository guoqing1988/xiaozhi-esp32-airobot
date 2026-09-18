#ifndef ESPNOW_HOME_H
#define ESPNOW_HOME_H

#include <cstdint>
#include <functional>
#include <string>

#include "esp_now.h"
#include "esp_timer.h"

// ESP-NOW 居家节点传输层（板级）。
//
// 职责：ESP-NOW 初始化、beacon 发现广播（供节点锁信道）、节点表维护、
//       上行报文解析与去重、下行报文发送。
// 不含：MCP 工具注册（板级主文件注册）、播报（板级调 LocalMusicPlayer）。
//
// 注意：本板 UART0 与 Arduino 下位机共用，本文件一律不使用 ESP_LOG，
//       失败通过返回值/上层工具文本表达（见 AGENTS.md 与本板 README）。
class EspNowHome {
public:
    // 上行事件出口：node_id / 事件名 / 参数 / 事件时间(ms)
    // 在 WiFi 任务上下文调用，实现方须用 Application::Schedule 切回主任务。
    using EventCallback = std::function<void(int node_id, const std::string& evt,
                                             const std::string& arg, int64_t ts_ms)>;

    static constexpr int kMaxNodes = 4;
    static constexpr int64_t kOfflineMs = 15000;     // 主控侧判定离线（节点侧回 hop 是 5s，勿混）
    static constexpr int64_t kDedupWindowMs = 1000;  // 上行事件去重窗口（抵消节点 3 连发）
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

    // 节点状态 JSON：{"nodes":[{"id":1,"online":true,"last_seen_ms":1234},...]}
    std::string NodesJson() const;

    static int64_t NowMs();

private:
    struct NodeEntry {
        int id = 0;
        uint8_t mac[6] = {0};
        int64_t last_seen_ms = 0;
        bool known = false;
    };

    struct DedupEntry {
        int node_id = 0;
        char evt[16] = {0};
        int64_t ts_ms = 0;
        bool used = false;
    };

    NodeEntry* FindNode(int node_id);
    NodeEntry* FindOrCreateNode(int node_id, const uint8_t* mac);
    bool ShouldDeliver(int node_id, const std::string& evt, int64_t now_ms);
    void HandleRecv(const uint8_t* src_mac, const uint8_t* data, int len);
    void StartBeacon(int period_ms);

    static void RecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len);
    static void BeaconTimerCb(void* arg);

    EventCallback callback_;
    NodeEntry nodes_[kMaxNodes];
    DedupEntry dedup_[kMaxNodes * 2];
    int dedup_pos_ = 0;
    esp_timer_handle_t beacon_timer_ = nullptr;
    int beacon_count_ = 0;
    bool started_ = false;
};

#endif  // ESPNOW_HOME_H
