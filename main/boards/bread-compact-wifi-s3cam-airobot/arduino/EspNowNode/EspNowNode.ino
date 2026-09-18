/*
 * XiaoZhi 居家演示节点（ESP32-S3，arduino-esp32 核心自带 ESP_NOW 类）
 *
 * 数据驱动设计（2026-09-18 重构）：节点**自描述能力**，主控只负责"存起来 + 转给 AI"。
 * 因此接入一个新设备只需改这个文件里的「能力表」，**主控零改动、无需重新烧录**。
 *
 * 角色：不连接任何 AP。开机在信道 1..13 之间 hop，收到主控的 "@beacon" 广播后锁定
 *       当前信道并登记主控；此后每次收到 beacon 都重报一次能力（主控重启后能自愈）。
 *
 * 节点 1（客厅灯）: 三通道 PWM RGB 灯 + HC-SR04P 超声波
 * 节点 2（玄关感应）: DHT11 温湿度 + 激光头模块
 *   —— 靠下面 NODE_ID 切换角色（编译节点 2 时改成 2）。
 *
 * 协议（与主控 espnow_home.cc 一致，@ 前缀文本行）：
 *   下行  @n<id> do <能力> <动作> [参数]     通用动作（主控不解释语义，原样透传）
 *   上行  @n<id> info <名字> <能力规格>      能力自描述（锁信道后 + 每次收到 beacon）
 *         @n<id> ok <能力> <结果>           执行回执
 *         @n<id> say <名字>                 播报请求（主控播 <名字>.mp3）
 *         @n<id> evt <名字> <值>            状态上报（只更新状态，不播报）
 *         @n<id> err <原因> [细节]          错误
 *
 * 编译: arduino-cli compile --fqbn esp32:esp32:esp32s3 <此目录>
 * 依赖: 灯与 ESP-NOW 用核心自带 API，零第三方库；
 *       仅节点 2 的 DHT11 需要 "DHT sensor library"(Adafruit)，且只在 NODE_ID==2 时包含。
 */

// ============================ 现场可调参数 ============================
// 注意：这些宏必须在下面的 #if 之前定义（预处理按顺序求值）。

#define NODE_ID 1                    // 1=客厅灯(RGB+超声波), 2=玄关感应(DHT11+激光)
#define HOP_INTERVAL_MS 200          // 未锁定信道时的换信道间隔
#define LOST_TIMEOUT_MS 5000         // 锁定后多久收不到主控包就回到 hop
#define HOP_CHANNEL_MIN 1
#define HOP_CHANNEL_MAX 13

// 重复上报：主控待机时 WiFi 省电(MAX_MODEM)只在 DTIM 醒来，单包易漏；
// 连发 EVENT_RESEND 次跨过 DTIM 周期，主控侧按 (kind, 名字) 1 秒去重。
#define EVENT_RESEND 3
#define kEventResendGapMs 150

// 超声波：迟滞(<30cm 触发, >40cm 复位) + 距离上报降频
// （距离每 100ms 采一次，但只在变化达标或保活周期到了才上报，避免刷屏占 2.4G）
#define MOTION_TRIGGER_CM 30
#define MOTION_RELEASE_CM 40
#define SONAR_PERIOD_MS 100
#define SONAR_TIMEOUT_US 30000       // pulseIn 超时(约 5m 对应 ~29ms)
#define DIST_REPORT_DELTA_CM 3       // 距离变化达 3cm 才上报
#define DIST_REPORT_KEEPALIVE_MS 5000

// DHT11 采样周期（DHT11 本身 ≤1Hz），失败重试
#define DHT_PERIOD_MS 5000
#define DHT_RETRY 3

// 温度告警迟滞（业务阈值属于节点自己的语义，主控不再关心温度）
#define HOT_TRIGGER_C 28             // ≥28℃ 播报一次
#define HOT_RELEASE_C 26             // ≤26℃ 复位

// 激光采样：数字输出，两次读数一致才认变化（去抖）
#define LASER_PERIOD_MS 50

// ---- 引脚（与设计文档一致，两节点同编号便于接线）----
#define PIN_RGB_R 4
#define PIN_RGB_G 5
#define PIN_RGB_B 6
#define PIN_SONAR_TRIG 7
#define PIN_SONAR_ECHO 15
#define PIN_DHT 4
#define PIN_LASER 5

// 三通道 RGB 模块共阳时 PWM 反相（现象：颜色反相或关不掉时改这里）
#define RGB_COMMON_ANODE 1
#define RGB_PWM_FREQ 5000
#define RGB_PWM_BITS 8

// ---- ESP-NOW 密钥：必须与主控 espnow_home.cc 完全一致（16 字节）----
// 字符串最多 15 个字符（含结尾 '\0' 不能超过数组大小）。
// 不一致的现象是"完全收不到任何包"（不是偶发失败），改动必须同步两侧。
static const uint8_t kPmk[16] = "xiaozhi-pmk-01";
static const uint8_t kLmk[16] = "xiaozhi-lmk-01";

// ============================== include ==============================

#include <ESP32_NOW.h>
#include <WiFi.h>
#include <esp_mac.h>

#if NODE_ID == 2
#include <DHT.h>   // 仅节点 2 需要，故按条件包含（节点 1 无需安装该库）
#endif

// ============================== 运行状态 ==============================

static bool locked = false;                 // 是否已锁定主控信道
static uint8_t cur_channel = 1;
static uint32_t last_hop_ms = 0;
static uint32_t last_seen_ms = 0;           // 最近一次收到主控任何报文

static char evt_buf[200] = {0};             // 待上报报文（info 可能较长）
static int evt_left = 0;                    // 剩余连发次数
static uint32_t evt_due_ms = 0;             // 下一次连发时刻

static uint32_t info_due_ms = 0;            // 能力重报（收到 beacon 时置位，loop 里安全发送）

static int light_on = 0;
static int light_r = 255, light_g = 255, light_b = 255, light_bright = 200;

static bool motion_active = false;
static int last_dist = -1;
static uint32_t last_dist_ms = 0;
static int last_beam = -1;
static uint32_t sonar_ms = 0, dht_ms = 0, laser_ms = 0;

static void queueEvent(const char* text);   // 前置声明（HomePeer 内会用到）

// ============================== 灯控制 ==============================

static void applyLight() {
    int scale = light_bright < 0 ? 0 : (light_bright > 255 ? 255 : light_bright);
    auto duty = [scale](int v) -> uint32_t {
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        v = v * scale / 255;
        if (!light_on) v = 0;
#if RGB_COMMON_ANODE
        v = 255 - v;   // 共阳：低电平点亮
#endif
        return static_cast<uint32_t>(v);
    };
    ledcWrite(PIN_RGB_R, duty(light_r));
    ledcWrite(PIN_RGB_G, duty(light_g));
    ledcWrite(PIN_RGB_B, duty(light_b));
}

// ===================== 参数解析小工具 =====================
// AI 生成的参数格式并不统一（"0 0 255" / "0,0,255" / "[0,0,255]" 都可能出现），
// 所以这里不依赖 sscanf 的固定格式，而是“把任何非数字字符都当分隔符”。
// 返回解析出的整数个数。
static int parseNums(const char* s, int* out, int maxn) {
    int n = 0;
    while (s != nullptr && *s != '\0' && n < maxn) {
        while (*s != '\0' && !((*s >= '0' && *s <= '9') || *s == '-')) {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        out[n++] = atoi(s);
        while ((*s >= '0' && *s <= '9') || *s == '-') {
            s++;
        }
    }
    return n;
}

// 取空格分隔的下一个字段：跳过前导/连续空格（主控也会 trim，这里再兜一层，
// 避免“AI 多打一个空格就整条命令被静默丢弃”）。返回下一个字段的起点。
static const char* nextField(const char* p, char* out, size_t out_len) {
    while (*p == ' ') {
        p++;
    }
    const char* sp = strchr(p, ' ');
    size_t n = (sp == nullptr) ? strlen(p) : static_cast<size_t>(sp - p);
    if (n >= out_len) {
        n = out_len - 1;
    }
    snprintf(out, out_len, "%.*s", static_cast<int>(n), p);
    return (sp == nullptr) ? (p + strlen(p)) : (sp + 1);
}

// ===================== 能力处理函数（动作 → 结果文本）=====================
// 约定：成功把结果写进 out 并返回 true；动作不认识则返回 false（上层回 unknown-action）。
// 动作名建议用标准词汇 on/off/set/rgb/read —— AI 首次调用命中率最高；
// 自定义动作名同样可以（主控原样透传，不校验）。

static bool capLight(const char* action, const char* args, char* out, size_t out_len) {
    int v[3] = {0, 0, 0};
    int n = parseNums(args, v, 3);
    if (strcmp(action, "on") == 0) {
        light_on = (n > 0) ? (v[0] != 0) : 1;   // "on" 不带参数时默认开
        applyLight();
        snprintf(out, out_len, "%d", light_on);
        return true;
    }
    if (strcmp(action, "off") == 0) {
        light_on = 0;
        applyLight();
        snprintf(out, out_len, "0");
        return true;
    }
    if (strcmp(action, "rgb") == 0) {
        if (n < 3) {
            return false;   // 参数不足：回 unknown-action，AI 看规格后能重试
        }
        light_r = v[0];
        light_g = v[1];
        light_b = v[2];
        applyLight();
        snprintf(out, out_len, "%d %d %d", v[0], v[1], v[2]);
        return true;
    }
    if (strcmp(action, "bright") == 0 || strcmp(action, "set") == 0) {
        if (n < 1) {
            return false;
        }
        light_bright = v[0];
        applyLight();
        snprintf(out, out_len, "%d", light_bright);
        return true;
    }
    if (strcmp(action, "read") == 0) {
        // 只读当前状态：不阻塞、不重新采样
        snprintf(out, out_len, "%d %d %d %d %d", light_on, light_r, light_g, light_b,
                 light_bright);
        return true;
    }
    return false;
}

// ============================== 能力表 ==============================
// 这是整个节点唯一的"设备描述来源"：改这里就能改 AI 看到的设备名与能力，
// 主控不需要任何改动。

typedef bool (*CapHandler)(const char* action, const char* args, char* out, size_t out_len);

struct CapDef {
    const char* name;      // 能力名（AI 用它搭配 action 调用）
    const char* spec;      // 规格说明：描述 + 动作(参数)。逗号分隔动作
    CapHandler handler;    // nullptr = 纯上报能力，不接受 do
};

struct NodeDef {
    const char* name;      // 设备名（AI 与语音里对它的称呼）
    const CapDef* caps;
    int cap_count;
};

#if NODE_ID == 1

static bool capDistRead(const char* action, const char* args, char* out, size_t out_len) {
    (void)args;
    if (strcmp(action, "read") != 0 || last_dist < 0) {
        return false;
    }
    snprintf(out, out_len, "%d", last_dist);
    return true;
}

static const CapDef kCaps[] = {
    {"light", "(RGB灯):on(0|1),off(),rgb(r,g,b),bright(0-255),read()", capLight},
    {"dist", "(超声波距离cm,只读):read()", capDistRead},
};
static const char kNodeName[] = "客厅灯";

#else

static int last_temp = -100, last_hum = -100;
static int hot_state = 0;   // 0=正常 1=已告警（迟滞，避免在阈值上反复播报）

static bool capTempRead(const char* action, const char* args, char* out, size_t out_len) {
    (void)args;
    if (strcmp(action, "read") != 0 || last_temp < -50) {
        return false;
    }
    snprintf(out, out_len, "%d %d", last_temp, last_hum);
    return true;
}

static bool capBeamRead(const char* action, const char* args, char* out, size_t out_len) {
    (void)args;
    if (strcmp(action, "read") != 0 || last_beam < 0) {
        return false;
    }
    snprintf(out, out_len, "%d", last_beam);
    return true;
}

static const CapDef kCaps[] = {
    {"temp", "(温湿度℃/%,只读):read()", capTempRead},
    {"beam", "(激光遮挡0=通1=挡,只读):read()", capBeamRead},
};
static const char kNodeName[] = "玄关感应";

#endif

static const NodeDef kNodeDef = {kNodeName, kCaps, 2};
static const NodeDef* node_def = &kNodeDef;

// ============================== 上行发送 ==============================

// 填入一条待上报报文（会替换上一条未发完的；事件频率极低，够用）
static void queueEvent(const char* text) {
    snprintf(evt_buf, sizeof(evt_buf), "%s", text);
    evt_left = EVENT_RESEND;
    evt_due_ms = millis();
}

// 状态上报（只更新主控的状态缓存，不触发播报）
static void queueEvt(const char* name, const char* arg) {
    char buf[64];
    if (arg != nullptr && arg[0] != '\0') {
        snprintf(buf, sizeof(buf), "@n%d evt %s %s", NODE_ID, name, arg);
    } else {
        snprintf(buf, sizeof(buf), "@n%d evt %s", NODE_ID, name);
    }
    queueEvent(buf);
}

// 播报请求：主控播 <名字>.mp3（文件不存在则静默跳过）
// 与 evt 分开的原因：dist 这类高频状态若走播报通道，主控会反复做 SD 卡文件查找。
static void queueSay(const char* name) {
    char buf[32];
    snprintf(buf, sizeof(buf), "@n%d say %s", NODE_ID, name);
    queueEvent(buf);
}

// 能力动作回执：kind = "ok" | "err"
static void qCap(const char* kind, const char* a, const char* b) {
    char buf[96];
    if (b != nullptr && b[0] != '\0') {
        snprintf(buf, sizeof(buf), "@n%d %s %s %s", NODE_ID, kind, a, b);
    } else {
        snprintf(buf, sizeof(buf), "@n%d %s %s", NODE_ID, kind, a);
    }
    queueEvent(buf);
}

// 能力自描述上报：@n<id> info <名字> <规格>;<规格>
// 单包 ≤200B：能力规格要克制（描述别写太长），超长主控会截断。
static void sendInfo() {
    char buf[200];
    int n = snprintf(buf, sizeof(buf), "@n%d info %s ", NODE_ID, node_def->name);
    for (int i = 0; i < node_def->cap_count && n > 0 && n < (int)sizeof(buf); i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s%s%s", (i != 0) ? ";" : "",
                      node_def->caps[i].name, node_def->caps[i].spec);
    }
    queueEvent(buf);
}

// ============================== 主控 peer ==============================

static class HomePeer* peer = nullptr;

class HomePeer : public ESP_NOW_Peer {
public:
    HomePeer(const uint8_t* mac, uint8_t channel)
        : ESP_NOW_Peer(mac, channel, WIFI_IF_STA, kLmk) {}

    // add()/send() 在基类里是 protected，需由子类公开包装（官方示例同法）
    bool attach() { return add(); }
    bool sendData(const char* text) {
        return send(reinterpret_cast<const uint8_t*>(text), strlen(text)) > 0;
    }

    void onReceive(const uint8_t* data, size_t len, bool broadcast) override {
        last_seen_ms = millis();
        if (len < 4 || data[0] != '@') {
            return;
        }
        if (broadcast) {
            // 主控 beacon（广播）：重报一次能力描述。
            // 主控重启后注册表是空的，靠这条自愈；发在 loop 里做，避免在回调里做重活。
            if (strncmp(reinterpret_cast<const char*>(data), "@beacon", 7) == 0) {
                info_due_ms = millis();
            }
            return;
        }
        const char* p = reinterpret_cast<const char*>(data) + 2;
        if (data[1] != 'n' || atoi(p) != NODE_ID) {
            return;   // 不是发给本节点的
        }
        const char* sp = strchr(p, ' ');
        if (sp == nullptr) {
            return;
        }
        handleCommand(sp + 1);
    }

    void onSent(bool success) override {
        (void)success;   // 上行已在应用层连发 EVENT_RESEND 次，无需在此重传
    }

private:
    // 通用动作：do <能力> <动作> [参数]
    // 主控不解释语义、本节点按能力表分发——接入新动作只需改能力表。
    static void handleCommand(const char* body) {
        if (strncmp(body, "do ", 3) != 0) {
            return;   // 其它命令（ping 等）：收到即刷新在线（onReceive 已做）
        }
        char cap[16] = {0};
        char action[16] = {0};

        const char* rest = nextField(body + 3, cap, sizeof(cap));
        if (cap[0] == '\0') {
            return;   // 连能力名都没有：丢弃（无法回答是哪个能力出错）
        }
        rest = nextField(rest, action, sizeof(action));
        if (action[0] == '\0') {
            qCap("err", "missing-action", cap);
            return;
        }
        while (*rest == ' ') {
            rest++;   // args 原样交给 handler（它自己做宽容解析）
        }
        const char* args = rest;

        for (int i = 0; i < node_def->cap_count; i++) {
            if (strcmp(node_def->caps[i].name, cap) != 0) {
                continue;
            }
            if (node_def->caps[i].handler == nullptr) {
                qCap("err", "readonly", cap);
                return;
            }
            char out[48];
            out[0] = '\0';
            if (node_def->caps[i].handler(action, args, out, sizeof(out))) {
                qCap("ok", cap, out);
            } else {
                qCap("err", "unknown-action", action);
            }
            return;
        }
        qCap("err", "unknown-cap", cap);
    }
};

// 未注册 peer 的数据会进这里（主控 beacon）→ 锁定当前信道并登记主控。
// 官方 ESP_NOW 类的 onNewPeer 正是为这种"发现"场景提供的。
static void onNewPeerCb(const esp_now_recv_info_t* info, const uint8_t* data, int len, void* arg) {
    (void)arg;
    if (info == nullptr || data == nullptr || len < 7) {
        return;
    }
    if (strncmp(reinterpret_cast<const char*>(data), "@beacon", 7) != 0) {
        return;
    }
    if (!locked) {
        locked = true;
        delete peer;
        peer = new HomePeer(info->src_addr, cur_channel);
        if (!peer->attach()) {
            delete peer;
            peer = nullptr;
            locked = false;   // 登记失败：继续 hop，下一轮 beacon 再试
        } else {
            info_due_ms = millis();   // 锁定成功：立刻上报能力
        }
    }
    last_seen_ms = millis();
}

// ============================== 信道 hop ==============================

static void hopTick() {
    if (locked || millis() - last_hop_ms < HOP_INTERVAL_MS) {
        return;
    }
    last_hop_ms = millis();
    cur_channel = (cur_channel >= HOP_CHANNEL_MAX) ? HOP_CHANNEL_MIN : (cur_channel + 1);
    WiFi.setChannel(cur_channel);
}

// 上行连发（非阻塞：靠时间戳推进，不 delay，避免堵 loop）
static void evtTick() {
    if (evt_left <= 0) {
        return;
    }
    if (static_cast<int32_t>(millis() - evt_due_ms) < 0) {
        return;
    }
    if (peer != nullptr) {
        peer->sendData(evt_buf);
    }
    evt_left--;
    evt_due_ms = millis() + kEventResendGapMs;
}

// 能力重报（收到 beacon 后）
static void infoTick() {
    if (info_due_ms == 0 || static_cast<int32_t>(millis() - info_due_ms) < 0) {
        return;
    }
    info_due_ms = 0;
    sendInfo();
}

// ============================== 传感器 ==============================

#if NODE_ID == 1
static void sonarTick() {
    if (millis() - sonar_ms < SONAR_PERIOD_MS) {
        return;
    }
    sonar_ms = millis();

    digitalWrite(PIN_SONAR_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_SONAR_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_SONAR_TRIG, LOW);
    unsigned long us = pulseIn(PIN_SONAR_ECHO, HIGH, SONAR_TIMEOUT_US);
    if (us == 0) {
        return;   // 超时/无回波：保持上次状态，不误报
    }
    int cm = static_cast<int>(us / 58);

    // 距离上报降频：变化达标或到保活周期才发（每 100ms 全发会占满 2.4G）
    if (last_dist < 0 || abs(cm - last_dist) >= DIST_REPORT_DELTA_CM ||
        millis() - last_dist_ms >= DIST_REPORT_KEEPALIVE_MS) {
        char arg[8];
        snprintf(arg, sizeof(arg), "%d", cm);
        queueEvt("dist", arg);
        last_dist = cm;
        last_dist_ms = millis();
    }

    // 人体靠近：迟滞判定，只在"进入"边沿动作一次。
    // “人来自动开灯”这个策略属于节点自己的业务逻辑（主控不知道也不关心）：
    // 节点自己开灯 + 上报状态 + 请求播报。换成“人来自动开风扇”也只改这里。
    // 人离开只复位标志与状态，**不自动关灯**（避免干扰用户刚用语音开的灯）。
    if (!motion_active && cm < MOTION_TRIGGER_CM) {
        motion_active = true;
        light_on = 1;
        applyLight();
        queueEvt("motion", "1");
        queueSay("motion");
    } else if (motion_active && cm > MOTION_RELEASE_CM) {
        motion_active = false;
        queueEvt("motion", "0");
    }
}
#else
static DHT* dht = nullptr;

static void dhtTick() {
    if (millis() - dht_ms < DHT_PERIOD_MS) {
        return;
    }
    dht_ms = millis();
    for (int i = 0; i < DHT_RETRY; i++) {
        float t = dht->readTemperature();
        float h = dht->readHumidity();
        if (!isnan(t) && !isnan(h)) {
            int ti = static_cast<int>(t + 0.5f);
            int hi = static_cast<int>(h + 0.5f);
            last_temp = ti;
            last_hum = hi;
            char arg[16];
            snprintf(arg, sizeof(arg), "%d %d", ti, hi);
            queueEvt("temp", arg);

            // 高温告警：阈值属于本节点的业务语义，主控只负责播 hot.mp3
            if (hot_state == 0 && ti >= HOT_TRIGGER_C) {
                hot_state = 1;
                queueSay("hot");
            } else if (hot_state == 1 && ti <= HOT_RELEASE_C) {
                hot_state = 0;
            }
            return;
        }
        delay(120);   // DHT11 两次读取需间隔；仅失败路径有这点延迟
    }
    queueEvt("err", "dht");
}

static void laserTick() {
    if (millis() - laser_ms < LASER_PERIOD_MS) {
        return;
    }
    laser_ms = millis();
    int v = (digitalRead(PIN_LASER) == HIGH) ? 1 : 0;
    if (v == last_beam) {
        return;
    }
    delay(2);
    if (((digitalRead(PIN_LASER) == HIGH) ? 1 : 0) != v) {
        return;   // 两次读数不一致：当作抖动，丢弃
    }
    last_beam = v;
    queueEvt("beam", v ? "1" : "0");
    if (v == 1) {
        queueSay("beam");   // 只有"被挡住"这个边沿才播报
    }
}
#endif

// ============================== Arduino ==============================

void setup() {
    // 1) 灯（仅客厅节点有 RGB；玄关节点接了也无害）
    ledcAttach(PIN_RGB_R, RGB_PWM_FREQ, RGB_PWM_BITS);
    ledcAttach(PIN_RGB_G, RGB_PWM_FREQ, RGB_PWM_BITS);
    ledcAttach(PIN_RGB_B, RGB_PWM_FREQ, RGB_PWM_BITS);
    applyLight();

#if NODE_ID == 1
    pinMode(PIN_SONAR_TRIG, OUTPUT);
    pinMode(PIN_SONAR_ECHO, INPUT);
#else
    pinMode(PIN_LASER, INPUT);
    dht = new DHT(PIN_DHT, DHT11);
    dht->begin();
#endif

    // 2) WiFi/ESP-NOW：不连接任何 AP，只用 ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);          // 节点常醒（USB 供电），保证随时收下行命令
    WiFi.setChannel(cur_channel);
    while (!WiFi.STA.started()) {
        delay(10);
    }

    if (!ESP_NOW.begin(kPmk)) {    // 官方 ESP_NOW 类（核心自带）
        delay(1000);
        ESP.restart();
    }
    ESP_NOW.onNewPeer(onNewPeerCb, nullptr);
    last_seen_ms = millis();
}

void loop() {
    hopTick();

    // 锁定后失联 → 回 hop 重新发现（主控重启/换热点/换信道都能自恢复）
    if (locked && millis() - last_seen_ms > LOST_TIMEOUT_MS) {
        locked = false;
        delete peer;
        peer = nullptr;
    }

    evtTick();
    infoTick();

#if NODE_ID == 1
    sonarTick();
#else
    dhtTick();
    laserTick();
#endif
}
