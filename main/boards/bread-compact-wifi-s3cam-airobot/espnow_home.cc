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

bool EspNowHome::Begin(EventCallback cb, LinkCallback link) {
    callback_ = std::move(cb);
    link_cb_ = std::move(link);
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
    // 发送回调：MAC 层真实结果（官方 API，文档建议用它判断链路，而不是 esp_now_send 的返回值——
    // 后者只表示"已入队"；信道不对/对端不在时它照旧返回 OK，然后在这里报 FAIL）。
    if (esp_now_register_send_cb(&EspNowHome::SendCb) != ESP_OK) {
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

    // 记录启动时的信道：之后变化即意味着 AP 换信道（节点必须尽快重锁）
    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&ch, &sec) == ESP_OK) {
        last_channel_ = ch;
    }
    channel_check_ms_ = NowMs();

    started_ = true;
    beacon_count_ = 0;
    StartBeacon(kBeaconFastMs);
    return true;
}

void EspNowHome::StartBeacon(int period_ms) {
    beacon_period_ms_ = period_ms;
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

void EspNowHome::NotifyLink(const char* what, const char* detail) {
    if (link_cb_) {
        link_cb_(what, (detail != nullptr) ? detail : "");
    }
}

// 进入快速 beacon 窗口：节点 hop 一圈（13×500ms=6.5s）内必然撞上一次 beacon，
// 于是"AP 换信道 / 命令打空"后的重锁时间从最坏 40s+ 压到 ~10s。
void EspNowHome::EnterFastBeacon(const char* why, const char* detail) {
    int64_t now = NowMs();
    fast_beacon_until_ms_ = now + kFastBeaconWindowMs;
    if (beacon_period_ms_ != kBeaconFastMs) {
        StartBeacon(kBeaconFastMs);
    }
    NotifyLink(why, detail);
}

// 信道看护：主控是 STA，射频信道由 AP 决定，ESP-NOW 只能跟随。
// 用官方 esp_wifi_get_channel() 检测变化 → 提速 beacon 让节点尽快重锁（不依赖路由器配置）。
void EspNowHome::PollChannel(int64_t now_ms) {
    if (channel_check_ms_ != 0 && (now_ms - channel_check_ms_) < kChannelPollMs) {
        return;
    }
    channel_check_ms_ = now_ms;
    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&ch, &sec) != ESP_OK) {
        return;
    }
    if (last_channel_ == 0) {
        last_channel_ = ch;
        return;
    }
    if (ch == last_channel_) {
        return;
    }
    char detail[128];
    snprintf(detail, sizeof(detail), "主控信道 %u -> %u（AP 换信道，节点需重新锁定）",
             last_channel_, ch);
    last_channel_ = ch;
    EnterFastBeacon("channel", detail);
}

void EspNowHome::BeaconTimerCb(void* arg) {
    (void)arg;
    if (g_home == nullptr) {
        return;
    }
    EspNowHome* self = g_home;
    int64_t now = NowMs();

    // 信道看护 + 发送失败提速（都蹭这个定时器，不额外常驻任务/定时器）
    self->PollChannel(now);
    if (self->fast_request_.exchange(false)) {
        self->EnterFastBeacon("txfail", "连续发送失败，节点可能已不在当前信道");
    }

    // 节点靠这条广播锁定信道并学习主控 MAC（数据来自未注册 peer → 走节点的 onNewPeer 回调）；
    // 节点每次收到 beacon 都会重发 info（能力自描述），所以主控重启后注册表能自愈。
    static const char kBeacon[] = "@beacon 1";
    esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(kBeacon), strlen(kBeacon));

    self->beacon_count_++;
    // 启动期快发 kBeaconFastCount 次，之后稳态；处于快速窗口时再提速
    int period = kBeaconSlowMs;
    if (self->beacon_count_ < kBeaconFastCount || now < self->fast_beacon_until_ms_) {
        period = kBeaconFastMs;
    }
    if (period != self->beacon_period_ms_) {
        self->StartBeacon(period);
    }
}

// 发送回调（运行在高优先级 Wi-Fi 任务）：只累加计数 + 置一次性提速标志，
// 不做任何耗时操作（官方文档明确要求）。
void EspNowHome::SendCb(const esp_now_send_info_t* tx_info, esp_now_send_status_t status) {
    if (g_home == nullptr) {
        return;
    }
    bool ok = (tx_info != nullptr) ? (tx_info->tx_status == WIFI_SEND_SUCCESS)
                                   : (status == ESP_NOW_SEND_SUCCESS);
    if (ok) {
        g_home->tx_fail_streak_.store(0);
        return;
    }
    g_home->tx_fail_count_++;
    int streak = g_home->tx_fail_streak_.fetch_add(1) + 1;
    if (streak >= kTxFailStreakToFast) {
        g_home->fast_request_.store(true);
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

EspNowHome::PendingCmd* EspNowHome::FindPending(int node_id) {
    for (auto& p : pending_) {
        if (p.used && p.node_id == node_id) {
            return &p;
        }
    }
    return nullptr;
}

// 真正发送一条在途命令（首包与重传共用）
bool EspNowHome::DispatchPending(PendingCmd& p) {
    char buf[kMaxPacketLen];
    int len = snprintf(buf, sizeof(buf), "@n%d#%u %s", p.node_id, (unsigned)p.seq, p.body);
    if (len <= 0 || len >= static_cast<int>(sizeof(buf))) {
        return false;
    }
    return esp_now_send(p.mac, reinterpret_cast<const uint8_t*>(buf), len) == ESP_OK;
}

void EspNowHome::EnsureRetryTimer() {
    if (retry_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EspNowHome::RetryTimerCb;
        args.name = "espnow_retry";
        if (esp_timer_create(&args, &retry_timer_) != ESP_OK) {
            return;
        }
        esp_timer_start_periodic(retry_timer_, static_cast<uint64_t>(kRetryTickMs) * 1000ULL);
        return;
    }
    // 只在有在途命令时运行：待机功耗不变（信道看护蹭 beacon 定时器）
    if (!esp_timer_is_active(retry_timer_)) {
        esp_timer_start_periodic(retry_timer_, static_cast<uint64_t>(kRetryTickMs) * 1000ULL);
    }
}

void EspNowHome::StopRetryTimerIfIdle() {
    for (const auto& p : pending_) {
        if (p.used) {
            return;
        }
    }
    if (retry_timer_ != nullptr && esp_timer_is_active(retry_timer_)) {
        esp_timer_stop(retry_timer_);
    }
}

void EspNowHome::CompletePending(int node_id, bool confirmed, const char* why) {
    PendingCmd* p = FindPending(node_id);
    if (p == nullptr) {
        return;
    }
    uint16_t seq = p->seq;
    p->used = false;
    if (confirmed) {
        confirmed_count_++;
    } else {
        unconfirmed_count_++;
        char detail[128];
        snprintf(detail, sizeof(detail), "命令未确认 #%u（%s）", (unsigned)seq,
                 (why != nullptr) ? why : "");
        NotifyLink("cmdfail", detail);
    }
    StopRetryTimerIfIdle();
}

void EspNowHome::RetryTimerCb(void* arg) {
    (void)arg;
    if (g_home != nullptr) {
        g_home->RetryTickRaw();
    }
}

// 重传节奏（非阻塞）：150ms 间隔重发 kCmdRetryMax 次；仍无 ACK 则 1s 一次低频探测，
// 直到挂起时限（kCmdPendingMs）。这样节点掉出信道 hop 一圈（~6.5s）后重锁时，
// 命令会自动补上，用户不必重说（官方文档建议的"ACK 超时则重传"）。
void EspNowHome::RetryTickRaw() {
    int64_t now = NowMs();
    PollChannel(now);
    if (fast_request_.exchange(false)) {
        EnterFastBeacon("txfail", "连续发送失败，节点可能已不在当前信道");
    }

    bool any = false;
    for (auto& p : pending_) {
        if (!p.used) {
            continue;
        }
        if (now >= p.expire_ms) {
            CompletePending(p.node_id, false, "超过挂起时限，节点仍不在信道或未上电");
            continue;
        }
        if (now >= p.next_ms) {
            if (p.retries_left > 0) {
                p.retries_left--;
                // 间隔重传用尽后直接转 1s 低频探测，不留半次快节奏尾巴
                p.next_ms = now + ((p.retries_left > 0) ? kCmdRetryGapMs : 1000);
            } else {
                p.next_ms = now + 1000;   // 低频探测，等节点重锁
            }
            DispatchPending(p);
        }
        any = true;
    }
    if (!any) {
        StopRetryTimerIfIdle();
    }
}

bool EspNowHome::IsPending(int node_id) const {
    for (const auto& p : pending_) {
        if (p.used && p.node_id == node_id) {
            return true;
        }
    }
    return false;
}

int EspNowHome::PendingCount() const {
    int n = 0;
    for (const auto& p : pending_) {
        if (p.used) {
            n++;
        }
    }
    return n;
}

// 下行：生成序号 → 发首包 → 登记在途命令，由 esp_timer 驱动重传。
// 为什么要 ACK/重传：主控是 STA，信道由 AP 决定；节点在 hop 期间收不到任何单播，
// 而 esp_now_send() 只表示"已入队"（不表示送达）——不加 ACK 就是静默丢失（用户看到"控制失败/延迟"）。
bool EspNowHome::SendTo(int node_id, const std::string& body) {
    DeviceEntry* d = FindDevice(node_id);
    if (d == nullptr || body.empty() || body.size() >= sizeof(PendingCmd::body)) {
        return false;
    }
    PendingCmd* p = FindPending(node_id);
    if (p == nullptr) {
        for (auto& slot : pending_) {
            if (!slot.used) {
                p = &slot;
                break;
            }
        }
    }
    if (p == nullptr) {
        return false;   // 每节点最多 1 条在途命令，表满即失败（正常不会发生）
    }

    int64_t now = NowMs();
    p->used = true;
    p->node_id = node_id;
    p->seq = seq_next_++;
    if (seq_next_ == 0) {
        seq_next_ = 1;   // 0 保留给"无序号"的旧格式
    }
    memcpy(p->mac, d->mac, sizeof(p->mac));
    snprintf(p->body, sizeof(p->body), "%s", body.c_str());
    p->retries_left = kCmdRetryMax;
    p->next_ms = now + kCmdRetryGapMs;
    p->expire_ms = now + kCmdPendingMs;

    EnsureRetryTimer();
    // 单播 peer 在上行接收时已登记（见 HandleRecv），此处直接发。
    // 返回值只表示"首包已入队"；真实结果由 send_cb + 节点 ACK 决定。
    return DispatchPending(*p);
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
    // 可选序号信封 `@n1#12 ...`：只消费真的带数字的 '#(#seq)，单独一个 '#' 不予理会
    uint16_t seq = 0;
    if (i < len && data[i] == '#') {
        int j = i + 1;
        while (j < len && data[j] >= '0' && data[j] <= '9') {
            seq = static_cast<uint16_t>(seq * 10 + static_cast<uint16_t>(data[j] - '0'));
            j++;
        }
        if (j > i + 1) {
            i = j;
        }
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

    // 只要收到该节点的**任何**上行，就说明它回到了主控信道：
    // 把在途命令的重发时刻拉到当前（下一个 retry tick 立刻补发）。
    // 这是"AP 换信道后节点重锁，命令自动补上"的关键一环，用户不必重说。
    for (auto& p : pending_) {
        if (p.used && p.node_id == node_id && now >= p.next_ms) {
            p.next_ms = now;
            EnsureRetryTimer();
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
        // 链路确认：序号匹配（或旧节点回执不带序号）→ 命令送达
        PendingCmd* pend = FindPending(node_id);
        if (pend != nullptr && (seq == 0 || pend->seq == seq)) {
            CompletePending(node_id, true, nullptr);
        }
        return;
    }

    // 错误：err <原因> [细节]
    if (strncmp(body, "err ", 4) == 0) {
        if (dev != nullptr) {
            UpsertState(*dev, "err", body + 4);
        }
        // 错误回执同样是链路反馈：节点收到了命令（只是执行不了），不必再重传
        PendingCmd* pend = FindPending(node_id);
        if (pend != nullptr && (seq == 0 || pend->seq == seq)) {
            CompletePending(node_id, true, nullptr);
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
        // 心跳是链路层保留名（协议约定，不是业务事件）：只刷新在线状态并推动在途命令重发。
        // 不进状态缓存（否则 AI 会看到无意义的 hb=1），也不回调业务
        // （否则每 5 秒一行网页日志，把有用的日志冲掉）。
        if (strcmp(nm, "hb") == 0) {
            return;
        }
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
