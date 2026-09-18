#include "espnow_home.h"

#include <cstdio>
#include <cstring>

#include "esp_wifi.h"

// ESP-NOW 密钥（16 字节）：两侧必须一致，改一处必须改另一处（节点固件 EspNowNode.ino）。
// PMK 全网统一；LMK 用于单播加密；广播不支持加密（IDF 官方限制，故 beacon 走明文）。
// 注意：PMK/LMK 不一致时的现象是"完全收不到包"（不是偶发失败），排查困难，改动务必同步两侧。
// 字符串字面量最多 15 个字符（含结尾 '\0' 不能超过 16 字节），否则编译报
// "initializer-string for 'const uint8_t [16]' is too long"。
static const uint8_t kPmk[16] = "xiaozhi-pmk-01";   // 14 字符 + '\0' + 1 字节补零
static const uint8_t kLmk[16] = "xiaozhi-lmk-01";
static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// 静态回调拿不到 this，用文件级指针（板级只有一个实例）。
static EspNowHome* g_home = nullptr;

int64_t EspNowHome::NowMs() {
    return esp_timer_get_time() / 1000;
}

bool EspNowHome::Begin(EventCallback cb) {
    callback_ = std::move(cb);
    if (started_) {
        return true;
    }
    g_home = this;

    if (esp_now_init() != ESP_OK) {
        g_home = nullptr;
        return false;
    }
    esp_now_set_pmk(kPmk);

    if (esp_now_register_recv_cb(&EspNowHome::RecvCb) != ESP_OK) {
        esp_now_deinit();
        g_home = nullptr;
        return false;
    }

    // 注册广播 peer（beacon 用）：channel=0 表示"跟随当前信道"，广播不可加密。
    esp_now_peer_info_t bcast = {};
    memcpy(bcast.peer_addr, kBroadcastMac, sizeof(kBroadcastMac));
    bcast.channel = 0;
    bcast.ifidx = WIFI_IF_STA;
    bcast.encrypt = false;
    esp_now_add_peer(&bcast);   // 已存在时返回错误，忽略即可

    started_ = true;
    beacon_count_ = 0;
    StartBeacon(kBeaconFastMs);
    return true;
}

void EspNowHome::StartBeacon(int period_ms) {
    if (beacon_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EspNowHome::BeaconTimerCb;
        args.name = "espnow_beacon";
        if (esp_timer_create(&args, &beacon_timer_) != ESP_OK) {
            return;
        }
    } else {
        // 允许在自身回调里 stop/start（IDF 明确支持这一用法）
        esp_timer_stop(beacon_timer_);
    }
    esp_timer_start_periodic(beacon_timer_, static_cast<uint64_t>(period_ms) * 1000ULL);
}

void EspNowHome::BeaconTimerCb(void* arg) {
    (void)arg;
    if (g_home == nullptr) {
        return;
    }
    // 节点靠这条广播锁定信道并学习主控 MAC（数据来自未注册 peer → 走节点的 onNewPeer 回调）。
    // 启动期快发（供节点尽快发现），之后降为低频常驻心跳（供节点重启/换信道后重新发现）。
    static const char kBeacon[] = "@beacon 1";
    esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(kBeacon), strlen(kBeacon));

    if (++g_home->beacon_count_ == kBeaconFastCount) {
        g_home->StartBeacon(kBeaconSlowMs);
    }
}

EspNowHome::NodeEntry* EspNowHome::FindNode(int node_id) {
    for (auto& n : nodes_) {
        if (n.known && n.id == node_id) {
            return &n;
        }
    }
    return nullptr;
}

EspNowHome::NodeEntry* EspNowHome::FindOrCreateNode(int node_id, const uint8_t* mac) {
    NodeEntry* found = FindNode(node_id);
    if (found != nullptr) {
        return found;
    }
    for (auto& n : nodes_) {
        if (!n.known) {
            n.known = true;
            n.id = node_id;
            memcpy(n.mac, mac, sizeof(n.mac));
            n.last_seen_ms = NowMs();
            return &n;
        }
    }
    return nullptr;   // 节点数超上限：忽略（kMaxNodes=4 已远超演示需求）
}

bool EspNowHome::SendTo(int node_id, const std::string& body) {
    NodeEntry* n = FindNode(node_id);
    if (n == nullptr || body.empty()) {
        return false;
    }
    char buf[kMaxPacketLen];
    int len = snprintf(buf, sizeof(buf), "@n%d %s", node_id, body.c_str());
    if (len <= 0 || len >= static_cast<int>(sizeof(buf))) {
        return false;
    }
    // 单播 peer 在上行接收时已登记（见 HandleRecv），此处直接发。
    return esp_now_send(n->mac, reinterpret_cast<const uint8_t*>(buf), len) == ESP_OK;
}

bool EspNowHome::IsOnline(int node_id) const {
    for (const auto& n : nodes_) {
        if (n.known && n.id == node_id) {
            return (NowMs() - n.last_seen_ms) < kOfflineMs;
        }
    }
    return false;
}

std::string EspNowHome::NodesJson() const {
    std::string json = "{\"nodes\":[";
    bool first = true;
    int64_t now = NowMs();
    for (const auto& n : nodes_) {
        if (!n.known) {
            continue;
        }
        if (!first) {
            json += ",";
        }
        first = false;
        char item[96];
        snprintf(item, sizeof(item), "{\"id\":%d,\"online\":%s,\"last_seen_ms\":%lld}", n.id,
                 (now - n.last_seen_ms) < kOfflineMs ? "true" : "false",
                 static_cast<long long>(now - n.last_seen_ms));
        json += item;
    }
    json += "]}";
    return json;
}

bool EspNowHome::ShouldDeliver(int node_id, const std::string& evt, int64_t now_ms) {
    for (auto& d : dedup_) {
        if (d.used && d.node_id == node_id && evt == d.evt &&
            (now_ms - d.ts_ms) < kDedupWindowMs) {
            d.ts_ms = now_ms;   // 续期：连发 3 次全部落在窗口内被吞掉
            return false;
        }
    }
    DedupEntry& slot = dedup_[dedup_pos_];
    dedup_pos_ = (dedup_pos_ + 1) % (kMaxNodes * 2);
    slot.used = true;
    slot.node_id = node_id;
    snprintf(slot.evt, sizeof(slot.evt), "%s", evt.c_str());
    slot.ts_ms = now_ms;
    return true;
}

void EspNowHome::HandleRecv(const uint8_t* src_mac, const uint8_t* data, int len) {
    if (src_mac == nullptr || data == nullptr || len < 4 || len > kMaxPacketLen) {
        return;
    }
    if (data[0] != '@' || data[1] != 'n') {
        return;
    }
    int i = 2;
    int node_id = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
        node_id = node_id * 10 + (data[i] - '0');
        i++;
    }
    if (node_id <= 0 || node_id > kMaxNodes || i >= len || data[i] != ' ') {
        return;
    }

    int64_t now = NowMs();
    NodeEntry* n = FindOrCreateNode(node_id, src_mac);
    if (n != nullptr) {
        n->last_seen_ms = now;
        memcpy(n->mac, src_mac, sizeof(n->mac));
        // 首次见到该节点时登记为单播 peer（含 LMK 加密），否则后续下行发不出去。
        // 接收本身不要求 peer 已登记（节点侧正是靠这一点用 onNewPeer 发现主控）。
        if (!esp_now_is_peer_exist(src_mac)) {
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, src_mac, sizeof(peer.peer_addr));
            peer.channel = 0;
            peer.ifidx = WIFI_IF_STA;
            peer.encrypt = true;
            memcpy(peer.lmk, kLmk, sizeof(kLmk));
            esp_now_add_peer(&peer);
        }
    }

    // 正文（短字符串走 SSO，不在 WiFi 任务里分配堆内存）
    std::string body(reinterpret_cast<const char*>(data + i + 1),
                     static_cast<size_t>(len - i - 1));

    if (body.compare(0, 4, "evt ") == 0) {
        std::string rest = body.substr(4);
        size_t sp = rest.find(' ');
        std::string name = (sp == std::string::npos) ? rest : rest.substr(0, sp);
        std::string arg = (sp == std::string::npos) ? std::string() : rest.substr(sp + 1);
        if (name.empty() || name.size() > 15) {
            return;
        }
        if (!ShouldDeliver(node_id, name, now)) {
            return;
        }
        if (callback_) {
            callback_(node_id, name, arg, now);
        }
        return;
    }
    // "ack ..." / "err ..."：只刷新在线状态（上面已做），不做业务处理
}

void EspNowHome::RecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (g_home == nullptr || info == nullptr) {
        return;
    }
    g_home->HandleRecv(info->src_addr, data, len);
}
