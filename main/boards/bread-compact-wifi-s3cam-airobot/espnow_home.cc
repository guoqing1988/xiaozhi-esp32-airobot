#include "espnow_home.h"

#include <algorithm>
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

namespace {

// JSON 字符串转义：能力规格由节点提供，可能含引号/反斜杠，
// 直接拼接会产出非法 JSON（AI 侧解析失败）。中文 UTF-8 无需转义。
void AppendJsonEscaped(std::string& out, const char* s) {
    for (; s != nullptr && *s != '\0'; ++s) {
        if (*s == '"' || *s == '\\') {
            out += '\\';
        }
        out += *s;
    }
}

}  // namespace

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
    // 节点靠这条广播锁定信道并学习主控 MAC（数据来自未注册 peer → 走节点的 onNewPeer 回调）；
    // 节点每次收到 beacon 都会重发 info（能力自描述），所以主控重启后注册表能自愈。
    static const char kBeacon[] = "@beacon 1";
    esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(kBeacon), strlen(kBeacon));

    if (++g_home->beacon_count_ == kBeaconFastCount) {
        g_home->StartBeacon(kBeaconSlowMs);
    }
}

EspNowHome::DeviceEntry* EspNowHome::FindDevice(int node_id) {
    for (auto& d : devices_) {
        if (d.known && d.id == node_id) {
            return &d;
        }
    }
    return nullptr;
}

EspNowHome::DeviceEntry* EspNowHome::FindOrCreateDevice(int node_id, const uint8_t* mac) {
    DeviceEntry* found = FindDevice(node_id);
    if (found != nullptr) {
        return found;
    }
    for (auto& d : devices_) {
        if (!d.known) {
            d = DeviceEntry();   // 清空复用槽位，避免上一台设备的能力/状态残留
            d.known = true;
            d.id = node_id;
            memcpy(d.mac, mac, sizeof(d.mac));
            d.last_seen_ms = NowMs();
            return &d;
        }
    }
    return nullptr;   // 节点数超上限：忽略（kMaxNodes=4 已远超演示需求）
}

bool EspNowHome::SendTo(int node_id, const std::string& body) {
    DeviceEntry* d = FindDevice(node_id);
    if (d == nullptr || body.empty()) {
        return false;
    }
    char buf[kMaxPacketLen];
    int len = snprintf(buf, sizeof(buf), "@n%d %s", node_id, body.c_str());
    if (len <= 0 || len >= static_cast<int>(sizeof(buf))) {
        return false;
    }
    // 单播 peer 在上行接收时已登记（见 HandleRecv），此处直接发。
    return esp_now_send(d->mac, reinterpret_cast<const uint8_t*>(buf), len) == ESP_OK;
}

bool EspNowHome::IsOnline(int node_id) const {
    for (const auto& d : devices_) {
        if (d.known && d.id == node_id) {
            return (NowMs() - d.last_seen_ms) < kOfflineMs;
        }
    }
    return false;
}

bool EspNowHome::HasNode(int node_id) const {
    for (const auto& d : devices_) {
        if (d.known && d.id == node_id) {
            return true;
        }
    }
    return false;
}

bool EspNowHome::HasCap(int node_id, const std::string& cap) const {
    for (const auto& d : devices_) {
        if (!d.known || d.id != node_id) {
            continue;
        }
        for (int i = 0; i < d.cap_count; i++) {
            if (cap == d.caps[i].name) {
                return true;
            }
        }
        break;
    }
    return false;
}

const char* EspNowHome::NodeName(int node_id) const {
    for (const auto& d : devices_) {
        if (d.known && d.id == node_id && d.name[0] != '\0') {
            return d.name;   // 注册表内缓冲，调用方须立即使用
        }
    }
    static char fallback[24];
    snprintf(fallback, sizeof(fallback), "节点%d", node_id);
    return fallback;
}

std::string EspNowHome::DevicesJson() const {
    std::string json = "{\"nodes\":[";
    bool first = true;
    int64_t now = NowMs();
    for (const auto& d : devices_) {
        if (!d.known) {
            continue;
        }
        if (!first) {
            json += ",";
        }
        first = false;
        json += "{\"id\":";
        json += std::to_string(d.id);
        json += ",\"name\":\"";
        AppendJsonEscaped(json, d.name);
        json += d.info_seen ? "\",\"online\":" : "\",\"online\":";
        json += (now - d.last_seen_ms) < kOfflineMs ? "true" : "false";
        json += d.info_seen ? ",\"info_seen\":true" : ",\"info_seen\":false";
        json += ",\"caps\":[";
        for (int i = 0; i < d.cap_count; i++) {
            if (i != 0) {
                json += ",";
            }
            json += "{\"name\":\"";
            AppendJsonEscaped(json, d.caps[i].name);
            json += "\",\"spec\":\"";
            AppendJsonEscaped(json, d.caps[i].spec);
            json += "\"}";
        }
        json += "],\"state\":\"";
        AppendJsonEscaped(json, d.state);
        json += "\",\"age_ms\":";
        json += std::to_string(now - d.last_seen_ms);
        json += "}";
    }
    json += "]}";
    return json;
}

// 解析能力自描述正文（text = "<名字> <能力规格>"）。
// 关键：只把"能力名"与"其余规格原文"分开，**不解释动作语义**——
// 这样接入新设备/新动作时这里一行都不用改。
// 全程指针 + 栈缓冲，避免在 WiFi 任务上下文做堆分配。
void EspNowHome::HandleInfo(DeviceEntry& dev, const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    const char* sp = strchr(text, ' ');
    size_t name_len = (sp == nullptr) ? strlen(text) : static_cast<size_t>(sp - text);
    snprintf(dev.name, sizeof(dev.name), "%.*s", static_cast<int>(name_len), text);

    dev.cap_count = 0;
    dev.info_seen = true;   // 重报即覆盖：节点改了能力（重刷固件）后旧能力不许残留
    if (sp == nullptr) {
        return;
    }

    const char* cur = sp + 1;
    while (*cur != '\0' && dev.cap_count < kMaxCaps) {
        const char* semi = strchr(cur, ';');
        size_t len = (semi == nullptr) ? strlen(cur) : static_cast<size_t>(semi - cur);
        // 能力名结束于第一个 '(' 或 ':'（取靠前者），与节点固件/测试约定一致
        size_t cut = len;
        for (size_t i = 0; i < len; i++) {
            if (cur[i] == '(' || cur[i] == ':') {
                cut = i;
                break;
            }
        }
        if (cut > 0) {
            CapEntry& cap = dev.caps[dev.cap_count];
            snprintf(cap.name, sizeof(cap.name), "%.*s", static_cast<int>(cut), cur);
            snprintf(cap.spec, sizeof(cap.spec), "%.*s", static_cast<int>(len - cut), cur + cut);
            dev.cap_count++;
        }
        if (semi == nullptr) {
            break;
        }
        cur = semi + 1;
    }
}

// 通用 key=value 状态缓存：存在则替换（同名旧项丢弃），否则追加；超长截断。
// 本层不理解 key 的含义（可能是 light/dist/temp/err 或任何节点自定义项）。
void EspNowHome::UpsertState(DeviceEntry& dev, const char* key, const char* value) {
    if (key == nullptr || key[0] == '\0') {
        return;
    }
    size_t key_len = strlen(key);
    char entry[64];
    snprintf(entry, sizeof(entry), "%s=%s", key, (value == nullptr) ? "" : value);

    char out[kStateLen];
    size_t out_len = 0;
    const char* p = dev.state;
    bool replaced = false;
    while (*p != '\0') {
        const char* end = strchr(p, ' ');
        size_t seg_len = (end == nullptr) ? strlen(p) : static_cast<size_t>(end - p);
        bool is_target = (seg_len > key_len) && (p[key_len] == '=') &&
                         (strncmp(p, key, key_len) == 0);
        if (is_target) {
            if (!replaced) {
                size_t n = strlen(entry);
                if (out_len + n > sizeof(out) - 1) {
                    n = sizeof(out) - 1 - out_len;
                }
                memcpy(out + out_len, entry, n);
                out_len += n;
                replaced = true;
            }
        } else if (seg_len > 0) {
            if (out_len > 0 && out_len < sizeof(out) - 1) {
                out[out_len++] = ' ';
            }
            size_t n = seg_len;
            if (out_len + n > sizeof(out) - 1) {
                n = sizeof(out) - 1 - out_len;
            }
            memcpy(out + out_len, p, n);
            out_len += n;
        }
        if (end == nullptr) {
            break;
        }
        p = end + 1;
    }
    if (!replaced) {
        if (out_len > 0 && out_len < sizeof(out) - 1) {
            out[out_len++] = ' ';
        }
        size_t n = strlen(entry);
        if (out_len + n > sizeof(out) - 1) {
            n = sizeof(out) - 1 - out_len;
        }
        memcpy(out + out_len, entry, n);
        out_len += n;
    }
    out[out_len] = '\0';
    memcpy(dev.state, out, out_len + 1);
}

// 去重键必须是 kind+名字：否则 "say motion" 会吞掉 "evt motion"（两者语义不同）。
bool EspNowHome::ShouldDeliver(int node_id, const std::string& kind, const std::string& name,
                               int64_t now_ms) {
    char key[24];
    snprintf(key, sizeof(key), "%s %s", kind.c_str(), name.c_str());
    for (auto& d : dedup_) {
        if (d.used && d.node_id == node_id && strncmp(d.key, key, sizeof(d.key)) == 0 &&
            (now_ms - d.ts_ms) < kDedupWindowMs) {
            d.ts_ms = now_ms;   // 续期：连发 3 次全部落在窗口内被吞掉
            return false;
        }
    }
    DedupEntry& slot = dedup_[dedup_pos_];
    dedup_pos_ = (dedup_pos_ + 1) % (kMaxNodes * 2);
    slot.used = true;
    slot.node_id = node_id;
    snprintf(slot.key, sizeof(slot.key), "%s", key);
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
    DeviceEntry* dev = FindOrCreateDevice(node_id, src_mac);
    if (dev != nullptr) {
        dev->last_seen_ms = now;
        memcpy(dev->mac, src_mac, sizeof(dev->mac));
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

    // 正文拷到栈缓冲（200B），后续全用 C 字符串处理，不在 WiFi 任务里分配堆内存
    size_t body_len = static_cast<size_t>(len - i - 1);
    char body[kMaxPacketLen];
    if (body_len >= sizeof(body)) {
        return;
    }
    memcpy(body, data + i + 1, body_len);
    body[body_len] = '\0';

    // 能力自描述：幂等覆盖，不去重（节点每次收到 beacon 都会重报）、不回调（无需业务动作）
    if (strncmp(body, "info ", 5) == 0) {
        if (dev != nullptr) {
            HandleInfo(*dev, body + 5);
        }
        return;
    }

    // do 是下行专用；节点若把它发上来说明状态错乱，忽略（防回环）
    if (strncmp(body, "do ", 3) == 0) {
        return;
    }

    // 执行回执：ok <能力> <结果>
    if (strncmp(body, "ok ", 3) == 0) {
        const char* cap = body + 3;
        const char* sp = strchr(cap, ' ');
        if (sp == nullptr || sp == cap) {
            return;
        }
        char key[kCapNameLen];
        snprintf(key, sizeof(key), "%.*s", static_cast<int>(sp - cap), cap);
        if (dev != nullptr) {
            UpsertState(*dev, key, sp + 1);
        }
        return;
    }

    // 错误：err <原因> [细节]
    if (strncmp(body, "err ", 4) == 0) {
        if (dev != nullptr) {
            UpsertState(*dev, "err", body + 4);
        }
        return;
    }

    // 播报请求：say <名字> → 板级播 <名字>.mp3（本层不关心名字含义）
    if (strncmp(body, "say ", 4) == 0) {
        const char* name = body + 4;
        size_t name_len = strlen(name);
        if (name_len == 0 || name_len > 15) {
            return;
        }
        if (!ShouldDeliver(node_id, "say", name, now)) {
            return;
        }
        if (callback_) {
            callback_(node_id, "say", name, std::string(), now);
        }
        return;
    }

    // 状态上报：evt <名字> <值> → 只进状态缓存，不触发播报
    // （若走播报通道，dist 这类 5 秒一次的高频项会反复做 SD 卡文件查找）
    if (strncmp(body, "evt ", 4) == 0) {
        const char* name = body + 4;
        const char* sp = strchr(name, ' ');
        size_t name_len = (sp == nullptr) ? strlen(name) : static_cast<size_t>(sp - name);
        if (name_len == 0 || name_len > 15) {
            return;
        }
        char nm[16];
        snprintf(nm, sizeof(nm), "%.*s", static_cast<int>(name_len), name);
        const char* value = (sp == nullptr) ? "" : sp + 1;
        if (dev != nullptr) {
            UpsertState(*dev, nm, value);
        }
        if (!ShouldDeliver(node_id, "evt", nm, now)) {
            return;
        }
        if (callback_) {
            callback_(node_id, "evt", nm, value, now);
        }
        return;
    }

    // 其它正文：只刷新在线状态（上面已做），不做业务处理
}

void EspNowHome::RecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (g_home == nullptr || info == nullptr) {
        return;
    }
    g_home->HandleRecv(info->src_addr, data, len);
}
