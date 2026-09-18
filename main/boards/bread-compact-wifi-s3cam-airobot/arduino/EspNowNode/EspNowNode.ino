/*
 * XiaoZhi 居家演示节点（ESP32-S3，arduino-esp32 核心自带 ESP_NOW 类）
 *
 * 角色：被动执行 + 主动上报。不连接任何 AP：开机在信道 1..13 之间 hop，
 *       收到主控的 "@beacon" 广播后锁定当前信道并登记主控，之后双向通信。
 *
 * 节点 1（客厅）: 三通道 PWM RGB 灯 + HC-SR04P 超声波
 * 节点 2（玄关）: DHT11 温湿度 + 激光头模块
 *   —— 靠下面 NODE_ID 切换角色（默认 1，编译节点 2 时改成 2）。
 *
 * 协议（与主控 espnow_home.cc 一致，@ 前缀文本行）：
 *   下行: @n<id> light <on> <r> <g> <b> <brightness>
 *   上行: @n<id> evt <name> <arg>     (name: dist/temp/beam/motion)
 *         @n<id> ack light <on>
 *
 * 编译: arduino-cli compile --fqbn esp32:esp32:esp32s3 <此目录>
 * 依赖: 灯与 ESP-NOW 用核心自带 API，零第三方库；
 *       仅节点 2 的 DHT11 需要 "DHT sensor library"(Adafruit)，且只在 NODE_ID==2 时包含。
 */

// ============================ 现场可调参数 ============================
// 注意：这些宏必须在下面的 #if 之前定义（预处理按顺序求值）。

#define NODE_ID 1                    // 1=客厅(RGB+超声波), 2=玄关(DHT11+激光)
#define HOP_INTERVAL_MS 200          // 未锁定信道时的换信道间隔
#define LOST_TIMEOUT_MS 5000         // 锁定后多久收不到主控包就回到 hop
#define HOP_CHANNEL_MIN 1
#define HOP_CHANNEL_MAX 13

// 重复上报：主控待机时 WiFi 省电(MAX_MODEM)只在 DTIM 醒来，单包易漏；
// 连发 EVENT_RESEND 次跨过 DTIM 周期，主控侧按 (node_id, evt) 1 秒去重。
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

static char evt_buf[48] = {0};              // 待上报报文
static int evt_left = 0;                    // 剩余连发次数
static uint32_t evt_due_ms = 0;             // 下一次连发时刻

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

// ============================== 上行队列 ==============================

// 填入一条待上报报文（会替换上一条未发完的；事件频率极低，够用）
static void queueEvent(const char* text) {
    snprintf(evt_buf, sizeof(evt_buf), "%s", text);
    evt_left = EVENT_RESEND;
    evt_due_ms = millis();
}

// 组装并排队一条 evt 报文
static void queueEvt(const char* name, const char* arg) {
    char buf[48];
    if (arg != nullptr && arg[0] != '\0') {
        snprintf(buf, sizeof(buf), "@n%d evt %s %s", NODE_ID, name, arg);
    } else {
        snprintf(buf, sizeof(buf), "@n%d evt %s", NODE_ID, name);
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
        if (broadcast || len < 4 || data[0] != '@' || data[1] != 'n') {
            return;
        }
        const char* p = reinterpret_cast<const char*>(data) + 2;
        if (atoi(p) != NODE_ID) {
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
    static void handleCommand(const char* body) {
        if (strncmp(body, "light ", 6) == 0) {
            int on = 0, r = -1, g = -1, b = -1, br = -1;
            if (sscanf(body + 6, "%d %d %d %d %d", &on, &r, &g, &b, &br) < 1) {
                return;
            }
            light_on = (on != 0) ? 1 : 0;
            if (r >= 0) light_r = r;
            if (g >= 0) light_g = g;
            if (b >= 0) light_b = b;
            if (br >= 0) light_bright = br;
            applyLight();
            char ack[32];
            snprintf(ack, sizeof(ack), "@n%d ack light %d", NODE_ID, light_on);
            queueEvent(ack);
        }
        // 其它命令（ping 等）：收到即刷新 last_seen（已在 onReceive 里做）
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

    // 人体靠近：迟滞判定，只在"进入"边沿上报一次
    if (!motion_active && cm < MOTION_TRIGGER_CM) {
        motion_active = true;
        queueEvt("motion", "1");
    } else if (motion_active && cm > MOTION_RELEASE_CM) {
        motion_active = false;
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
            char arg[16];
            snprintf(arg, sizeof(arg), "%d %d", static_cast<int>(t + 0.5f),
                     static_cast<int>(h + 0.5f));
            queueEvt("temp", arg);
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

#if NODE_ID == 1
    sonarTick();
#else
    dhtTick();
    laserTick();
#endif
}
