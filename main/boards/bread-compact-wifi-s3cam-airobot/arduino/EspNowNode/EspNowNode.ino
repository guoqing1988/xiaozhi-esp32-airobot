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

#define NODE_ID 3                    // 1=客厅灯(RGB+超声波) 2=玄关感应(DHT11+激光) 3=融合节点(四件套)
#define HOP_INTERVAL_MS 2000   // 每个信道停留时长。主控每 3000ms 才广播一次，停太短会大部分
                               // 时间错过广播：1200ms 时命中率仅 40%，要转两三圈才撞上一次；
                               // 2000ms 提到 67%，一圈（26 秒）内基本能锁上
#define LOST_TIMEOUT_MS 12000        // 锁定后多久收不到主控包才回 hop。主控 3 秒一次广播，
                                     // 偶尔丢两三个很正常；原来 5 秒就掉线会反复重连
#define HOP_CHANNEL_MIN 1
#define HOP_CHANNEL_MAX 13

// 串口调试日志：1=开（默认），0=关（整段日志编译期消失，零额外开销）
// 节点串口是独占的（USB 转串口），不像主控 UART0 还要与 Arduino 指令共用，可放心开。
#define NODE_LOG 1
#define LOG_BAUD 115200

#if NODE_LOG
#define LOGF(...) Serial.printf(__VA_ARGS__)
#else
#define LOGF(...) ((void)0)
#endif

// 重复上报：主控待机时 WiFi 省电(MAX_MODEM)只在 DTIM 醒来，单包易漏；
// 连发 EVENT_RESEND 次跨过 DTIM 周期，主控侧按 (kind, 名字) 1 秒去重。
#define EVENT_RESEND 3
#define kEventResendGapMs 150

// 超声波：迟滞(<30cm 触发, >40cm 复位) + 距离上报降频
// （距离每 100ms 采一次，但只在变化达标或保活周期到了才上报，避免刷屏占 2.4G）
#define MOTION_TRIGGER_CM 30
#define MOTION_RELEASE_CM 40
#define MOTION_AUTO_OFF_MS 30000     // 人离开后多久自动关灯（只关"人来自动开的"那盏；0 = 不自动关）
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
// 障碍传感器极性：1 = 红外避障模块（FC-51 类，**检测到障碍 = 低电平**）；
//                 0 = 激光接收模块（被遮断 = 高电平）。
// 现象是“没东西也说有人 / 挡住反而说通畅”时，把这一行取反。
#define OBSTACLE_ACTIVE_LOW 1

// ---- 引脚（与设计文档一致，两节点同编号便于接线）----
#define PIN_RGB_R 4
#define PIN_RGB_G 5
#define PIN_RGB_B 6
#define PIN_SONAR_TRIG 7
#define PIN_SONAR_ECHO 15
#if NODE_ID == 3
// 融合节点：RGB 已占用 GPIO4/5/6，DHT11 与激光改用两个空闲脚
// （GPIO16/17 在 S3 上是普通 IO，不撞 flash/PSRAM/USB）
#define PIN_DHT 16
#define PIN_LASER 17
#else
#define PIN_DHT 4
#define PIN_LASER 5
#endif

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

#include <esp_wifi.h>   // esp_wifi_get_channel/set_channel 需要它（WiFi.h 不会间接包含）
#include <ESP32_NOW.h>
#include <WiFi.h>
#include <esp_mac.h>

#if NODE_ID == 2 || NODE_ID == 3
#include <DHT.h>   // 仅玄关/融合节点需要，故按条件包含（客厅节点无需安装该库）
#endif

// ============================== 运行状态 ==============================

static bool locked = false;                 // 是否已锁定主控信道
static uint8_t cur_channel = 1;
static uint32_t last_hop_ms = 0;
static uint32_t last_seen_ms = 0;           // 最近一次收到主控任何报文

static uint32_t info_due_ms = 0;            // 能力重报（收到 beacon 时置位，loop 里安全发送）

// 灯状态（客厅 1 / 融合 3；玄关节点不含这部分代码）
#if NODE_ID == 1 || NODE_ID == 3
static int light_on = 0;
static int light_r = 255, light_g = 255, light_b = 255, light_bright = 200;
#endif

// 超声波状态（客厅 1 / 融合 3）
#if NODE_ID == 1 || NODE_ID == 3
static bool motion_active = false;
// 自动亮灯的两个标记：
// auto_lit   = 当前这盏灯是"人来到自动开的"（用户一动手就交回人工，不再自动关）
// auto_off_due_ms = 自动关灯的到点时刻（0 = 没在排队）
static bool auto_lit = false;
static uint32_t auto_off_due_ms = 0;
// motion_suppress = 人工（语音/AI）接管过灯之后，就抑制“人来自动开灯”。
// 解除条件必须是“人离开并持续 MOTION_SUPPRESS_HOLD_MS”，不能写成“距离一超
// 过释放阀值就解除”：手在传感器前晃动时距离会在 30~45cm 之间来回跳，
// 那样抑制会被反复清掉，人就一直开灯，现象是“AI 关灯永远关不掉”。
#define MOTION_SUPPRESS_HOLD_MS 10000
static bool motion_suppress = false;
static uint32_t motion_clear_ms = 0;   // 变成“人不在”的时刻（用于计时解除抑制）

static int last_dist = -1;
static uint32_t last_dist_ms = 0;
static uint32_t sonar_ms = 0;
static uint32_t sonar_timeout_log_ms = 0;   // 超声波"无回波"日志的限频时间戳
static uint32_t sonar_ok_log_ms = 0;        // 距离读数日志的限频时间戳（没它就只能靠"有没有 no echo"反推）
#endif

// DHT11 状态（玄关 2 / 融合 3）
#if NODE_ID == 2 || NODE_ID == 3
static int last_temp = -100, last_hum = -100;
static int hot_state = 0;   // 0=正常 1=已告警（迟滞，避免在阈值上反复播报）
static uint32_t dht_ms = 0;
#endif

// 激光状态（玄关 2 / 融合 3）
#if NODE_ID == 2 || NODE_ID == 3
static int last_beam = -1;
static uint32_t laser_ms = 0;
#endif

// ---- 上行发送队列（数据结构）----
// ⚠️ 必须声明在文件前部（第一个函数定义之前）：arduino-cli 会把函数原型自动插到
// 第一个函数定义之前，若 struct 定义在文件中部，`putText(OutMsg&, ...)` 的原型
// 就会引用到未声明的类型而编译失败（本文件曾因此报 "OutMsg was not declared"）。
// 队列实现见下方「上行发送」段。
// 为什么是 6 槽：融合节点(3) 有 4 路状态上报（dist/motion/temp/beam）各占一槽后，
// 仍要留得下 say/ok —— 4 槽会被状态占满，把播报与回执报文挤掉。
#define TXQ_SIZE 6

struct OutMsg {
    char key[32];      // 合并键：去掉参数后的 "@n<id> <kind> <name>"
    char text[200];
    int left;          // 剩余连发次数
    uint32_t due_ms;   // 下一次发送时刻
};

static OutMsg txq[TXQ_SIZE];
static int txq_head = 0;      // 队首（正在连发的那条）
static int txq_count = 0;     // 队列中报文条数
// 入队在 ESP-NOW 回调（WiFi 任务）里发生，出队在 loop 里，故需临界区保护
static portMUX_TYPE txq_mux = portMUX_INITIALIZER_UNLOCKED;

static void queueEvent(const char* text);   // 前置声明（HomePeer 内会用到）

// ============================== 日志小工具 ==============================

// MAC 转 "aa:bb:cc:dd:ee:ff"（仅日志用；返回静态缓冲，一次日志里只调用一次）
static const char* fmtMac(const uint8_t* mac) {
    static char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    return buf;
}

// ============================== 灯控制 ==============================
#if NODE_ID == 1 || NODE_ID == 3

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

#endif   // NODE_ID == 1 || NODE_ID == 3（RGB）

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

#if NODE_ID == 1 || NODE_ID == 3

static bool capLight(const char* action, const char* args, char* out, size_t out_len) {
    // 用户/语音一动手就接管这盏灯：取消"人来自动开灯"的自动关灯排队，
    // 只查询（read）不算接管。
    if (strcmp(action, "read") != 0) {
        auto_lit = false;
        auto_off_due_ms = 0;
        // 人工已接管：别再自动开灯。人不在时就从现在开始计时解除
        motion_suppress = true;
        motion_clear_ms = motion_active ? 0 : millis();
    }
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

#endif   // NODE_ID == 1 || NODE_ID == 3（capLight）

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

// ---- 只读能力处理函数（按 NODE_ID 编译需要的组合，避免未使用代码）----

#if NODE_ID == 1 || NODE_ID == 3
static bool capDistRead(const char* action, const char* args, char* out, size_t out_len) {
    (void)args;
    if (strcmp(action, "read") != 0 || last_dist < 0) {
        return false;
    }
    snprintf(out, out_len, "%d", last_dist);
    return true;
}
#endif

#if NODE_ID == 2 || NODE_ID == 3
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
#endif

#if NODE_ID == 1
// 客厅：RGB 灯 + 超声波（人来自动开灯）
static const CapDef kCaps[] = {
    {"light", "(RGB灯):on(0|1),off(),rgb(r,g,b),bright(0-255),read()", capLight},
    {"dist", "(超声波距离cm,只读):read()", capDistRead},
};
static const char kNodeName[] = "客厅灯";

#elif NODE_ID == 2
// 玄关：DHT11 + 激光
static const CapDef kCaps[] = {
    {"temp", "(温湿度℃/%,只读):read()", capTempRead},
    {"beam", "(红外避障0=无1=有,只读):read()", capBeamRead},
};
static const char kNodeName[] = "玄关感应";

#else
// 融合节点：一台设备接全部四个传感器。
// 4 个能力的 info 报文实测约 188B，未超主控 200B 单包上限（描述文案别再拉长）。
static const CapDef kCaps[] = {
    {"light", "(RGB灯):on(0|1),off(),rgb(r,g,b),bright(0-255),read()", capLight},
    {"dist", "(超声波距离cm,只读):read()", capDistRead},
    {"temp", "(温湿度℃/%,只读):read()", capTempRead},
    {"beam", "(红外避障0=无1=有,只读):read()", capBeamRead},
};
static const char kNodeName[] = "融合节点";

#endif

// 能力数用 sizeof 推导：避免新增/删除能力时忘同步计数
static const NodeDef kNodeDef = {kNodeName, kCaps, (int)(sizeof(kCaps) / sizeof(kCaps[0]))};
static const NodeDef* node_def = &kNodeDef;

// ============================== 上行发送 ==============================
// 队列数据结构（TXQ_SIZE / OutMsg / txq[] / txq_mux）声明在文件前部「运行状态」段。
//
// 为什么要排队（而不是单槽缓冲）：一次 tick 里会连续产生多条上行
// （例："dist 上报" + "motion 上报" + "say motion"），单槽会被后一条直接覆盖，
// 前几条静默丢失（主控 state 里缺字段，现场表现为"状态时有时无"）。
// 每槽 200B（info 报文可能很长），6 槽 × 300ms 已远高于实际事件频率。

// 报文键：去掉参数部分（第 3 个空格之前的内容），用于同键合并。
// "@n1 evt dist 42" → "@n1 evt dist "；"@n1 say motion" → 整串。
static void msgKey(const char* text, char* out, size_t out_len) {
    memset(out, 0, out_len);
    const char* p = text;
    int spaces = 0;
    while (*p != '\0' && spaces < 3) {
        if (*p == ' ') {
            spaces++;
        }
        p++;
    }
    size_t n = static_cast<size_t>(p - text);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, text, n);
}

// 写入槽文本（超长截断：info 报文不要写太长）
static void putText(OutMsg& m, const char* text) {
    size_t n = strlen(text);
    if (n >= sizeof(m.text)) {
        n = sizeof(m.text) - 1;
    }
    memcpy(m.text, text, n);
    m.text[n] = '\0';
}

// 入队一条待上报报文（首次由 evtTick 立即发出，后续按时间戳重发）
// 同键合并：dist 这类状态在抖动时会每 100ms 变一次，若不去重，队列会被它占满
// 而把 say/ok 这类事件报文挤掉（现场表现为"播报时有时无"）。
static void queueEvent(const char* text) {
    char key[32];
    msgKey(text, key, sizeof(key));

    bool dropped = false;
    portENTER_CRITICAL(&txq_mux);
    OutMsg* slot = nullptr;
    for (int i = 0; i < txq_count; i++) {
        OutMsg& c = txq[(txq_head + i) % TXQ_SIZE];
        if (c.left > 0 && strncmp(c.key, key, sizeof(key)) == 0) {
            slot = &c;   // 同键：只更新内容，保留原有的重发节奏
            break;
        }
    }
    if (slot == nullptr) {
        if (txq_count == TXQ_SIZE) {
            // 队列满：丢最旧的，保留最新状态（临界区外再打日志）
            txq_head = (txq_head + 1) % TXQ_SIZE;
            txq_count--;
            dropped = true;
        }
        slot = &txq[(txq_head + txq_count) % TXQ_SIZE];
        memcpy(slot->key, key, sizeof(key));
        slot->left = EVENT_RESEND;
        slot->due_ms = millis();
        txq_count++;
    }
    putText(*slot, text);
    portEXIT_CRITICAL(&txq_mux);

    if (dropped) {
        LOGF("[espnow] WARN tx queue full, dropped oldest\n");
    }
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

static class HomePeer* peer = nullptr;      // 主控（加密）：接收下行命令
static class HomePeer* tx_peer = nullptr;   // 广播（明文）：发送上行数据

// 广播地址：ESP-NOW 的广播帧不能加密，只能明文发
static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

class HomePeer : public ESP_NOW_Peer {
public:
    // lmk 传 nullptr 表示明文（广播只能用明文）；不传就用 kLmk 加密
    HomePeer(const uint8_t* mac, uint8_t channel, const uint8_t* lmk = kLmk)
        : ESP_NOW_Peer(mac, channel, WIFI_IF_STA, lmk) {}

    // add()/send()/remove() 在基类里是 protected，需由子类公开包装（官方示例同法）
    bool attach() { return add(); }
    // 从 ESP-NOW 的对端表里摘掉自己。这一步不能省：基类的析构函数是空的，
    // 只 delete 只会释放内存，对端表里仍然留着这个 MAC。后果有两个：
    // 1) 主控的广播会被当成“已注册对端”处理，不再触发 onNewPeerCb，
    //    节点就再也锁不上主控（表现为“收到了 beacon 却永远不 LOCKED”）；
    // 2) 内部对端数组会留下指向已释放对象的悬空指针，之后主控下发的
    //    命令走到 onReceive 就是访问已释放内存，命令会静默丢掉。
    bool detach() { return remove(); }
    bool sendData(const char* text) {
        return send(reinterpret_cast<const uint8_t*>(text), strlen(text)) > 0;
    }

    void onReceive(const uint8_t* data, size_t len, bool broadcast) override {
        last_seen_ms = millis();
        // beacon(广播) 不打印：每 500ms 一条会刷屏，锁定与 info 重报已各有日志
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
        LOGF("[cmd] %s\n", body);   // 下行指令低频，直接打印不影响实时性
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
        // 登记主控为对端时，信道这一项必须填 0 —— 意思是"跟着当前信道走"，
        // 以后换信道找主控时会自动跟上。
        // 以前这里填的是 cur_channel（我们自己数的信道号），隐患很大：万一它和实际
        // 信道对不上，错误就被永久写进对端信息里再也改不掉，结果是——
        // 广播收得到、锁定也成功，但数据每次发送都失败
        // （串口报 Peer channel is not equal to the home channel），主控永远看不到本节点。
        uint8_t real_ch = cur_channel;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&real_ch, &second) != ESP_OK) {
            real_ch = cur_channel;   // 仅用于日志
        }
        locked = true;
        if (peer != nullptr) {   // 重新锁定时先摘掉旧对端（原因见 detach 的注释）
            peer->detach();
            delete peer;
            peer = nullptr;
        }
        peer = new HomePeer(info->src_addr, 0);
        if (!peer->attach()) {
            delete peer;
            peer = nullptr;
            locked = false;   // 登记失败：继续 hop，下一轮 beacon 再试
            LOGF("[espnow] peer add failed, keep hopping\n");
        } else {
            info_due_ms = millis();   // 锁定成功：立刻上报能力
            LOGF("[espnow] LOCKED master %s (real ch %u, cur=%u)\n", fmtMac(info->src_addr), real_ch,
                 cur_channel);
        }
    }
    last_seen_ms = millis();
}

// ============================== 信道 hop ==============================

static void hopTick() {
    if (locked || millis() - last_hop_ms < HOP_INTERVAL_MS) {
        return;
    }
    // 连上路由器会让下面切换信道必失败（信道被 AP 锁定），只提醒一次，不刷屏
    static bool warned_ap = false;
    if (!warned_ap && WiFi.isConnected()) {
        warned_ap = true;
        LOGF("[espnow] WARNING: connected to AP, channel is locked -> hop cannot work\n");
    }
    last_hop_ms = millis();
    if (cur_channel >= HOP_CHANNEL_MAX) {
        cur_channel = HOP_CHANNEL_MIN;
        // 每轮(13*2000ms = 26s)打一行：现场据此判断"确实在找信道但没收到 beacon"
        LOGF("[espnow] hopping... (no @beacon yet)\n");
    } else {
        cur_channel++;
    }
    // 换信道找主控：主控固定停在某个信道，本节点靠逐个信道试来找到它。
    // 用 ESP-IDF 的 esp_wifi_set_channel() 而不是 Arduino 的 WiFi.setChannel()：
    // 前者会返回错误码，切换失败能立刻从日志里看出来。
    // 为什么必须能看出来：一旦切换失败却没人察觉，对端记的信道就会和真实信道
    // 永久错位，现象是"广播收得到、数据发不出去"，非常难查。
    esp_err_t ch_err = esp_wifi_set_channel(cur_channel, WIFI_SECOND_CHAN_NONE);
    if (ch_err != ESP_OK) {
        LOGF("[espnow] setChannel(%u) failed: %d\n", cur_channel, (int)ch_err);
    }
}

// 上行连发（非阻塞：靠时间戳推进，不 delay，避免堵 loop）
static void evtTick() {
    char buf[200];
    int left = 0;
    bool ready = false;

    // 临界区内只做拷贝与计数推进，真正的发送（可能阻塞）留在临界区外
    portENTER_CRITICAL(&txq_mux);
    if (txq_count > 0) {
        OutMsg& m = txq[txq_head];
        if (static_cast<int32_t>(millis() - m.due_ms) >= 0) {
            memcpy(buf, m.text, sizeof(buf));
            m.left--;
            left = m.left;
            if (left <= 0) {
                txq_head = (txq_head + 1) % TXQ_SIZE;
                txq_count--;
            } else {
                m.due_ms = millis() + kEventResendGapMs;
            }
            ready = true;
        }
    }
    portEXIT_CRITICAL(&txq_mux);

    if (!ready) {
        return;
    }
    bool ok = (tx_peer != nullptr) && tx_peer->sendData(buf);   // 走广播，见 setup 里的说明
    // 连发只在首包打印完整报文，重发仅在失败时补一行（同一事件不刷三行）
    if (left == EVENT_RESEND - 1) {
        LOGF("[espnow] TX  (%d/%d) %s%s\n", EVENT_RESEND - left, EVENT_RESEND, buf,
             ok ? "" : "  <== SEND FAILED");
    } else if (!ok) {
        LOGF("[espnow] TX  (%d/%d) SEND FAILED\n", EVENT_RESEND - left, EVENT_RESEND);
    }
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

#if NODE_ID == 1 || NODE_ID == 3
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
        // 超时/无回波：保持上次状态，不误报；日志限频 2 秒，避免每 100ms 刷屏
        if (sonar_timeout_log_ms == 0 || millis() - sonar_timeout_log_ms >= 2000) {
            sonar_timeout_log_ms = millis();
            LOGF("[sonar] no echo (timeout), keep last state\n");
        }
        return;
    }
    int cm = static_cast<int>(us / 58);

    // 读数日志（限频 2 秒）：不打印的话，现场"看不到超声波信息"无法区分
    // “确实没测到”与“测到了只是没打印”。
    if (sonar_ok_log_ms == 0 || millis() - sonar_ok_log_ms >= 2000) {
        sonar_ok_log_ms = millis();
        LOGF("[sonar] dist %d cm (motion=%d)\n", cm, motion_active ? 1 : 0);
    }

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
    // 人离开后按 MOTION_AUTO_OFF_MS 自动关灯，但**只关自动开的**：
    // 用户语音开的灯（或又调了颜色的灯）已经交回人工，不再自动动它。
    if (!motion_active && cm < MOTION_TRIGGER_CM) {
        motion_active = true;
        auto_off_due_ms = 0;   // 又有人了：取消排队中的自动关灯
        // 被人工接管过就不开灯；但上报和播报照旧——人来了依旧要播“检测到有人靠近”
        if (!motion_suppress) {
            if (!light_on) {
                auto_lit = true;   // 只接管“本来就是灭的”灯；用户自己开的灯不标记
            }
            light_on = 1;
            applyLight();
            LOGF("[motion] auto light on\n");
        } else {
            LOGF("[motion] suppressed, light untouched\n");   // 人工已接管，不自作主张开灯
        }
        queueEvt("motion", "1");
        queueSay("motion");
    } else if (motion_active && cm > MOTION_RELEASE_CM) {
        motion_active = false;
        motion_clear_ms = millis();   // 记录“人走了”，满 MOTION_SUPPRESS_HOLD_MS 才解除抑制
        queueEvt("motion", "0");
        if (MOTION_AUTO_OFF_MS > 0 && auto_lit) {
            auto_off_due_ms = millis() + MOTION_AUTO_OFF_MS;
            LOGF("[motion] released, auto-off in %d ms\n", MOTION_AUTO_OFF_MS);
        }
    }

    // 抑制解除：只在“连续一段时间没检测到人”之后才解除。
    // 关键在“连续”：一旦又测到人，计时就清零重来。不然手在传感器前晃动时，
    // 每晃到 40cm 外就开始计时、满 5 秒就解除抑制，灯又被自动点亮——
    // 外面看到的就是“AI 关灯永远关不掉”。
    if (motion_suppress) {
        if (motion_active) {
            motion_clear_ms = 0;   // 有人在：不解除，计时清零重来
        } else if (motion_clear_ms == 0) {
            motion_clear_ms = millis();
        } else if (millis() - motion_clear_ms >= MOTION_SUPPRESS_HOLD_MS) {
            motion_suppress = false;
            motion_clear_ms = 0;
            LOGF("[motion] suppress cleared, auto light back on\n");
        }
    }

    // 自动关灯到点：只关“自动开的”那盏；用户接管过的灯不动
    if (MOTION_AUTO_OFF_MS > 0 && auto_off_due_ms != 0 &&
        static_cast<int32_t>(millis() - auto_off_due_ms) >= 0) {
        auto_off_due_ms = 0;
        if (auto_lit) {
            auto_lit = false;
            light_on = 0;
            applyLight();
            LOGF("[motion] auto off (nobody for %d ms)\n", MOTION_AUTO_OFF_MS);
        }
    }
}
#endif   // NODE_ID == 1 || NODE_ID == 3（超声波）

// 玄关 / 融合节点：DHT11 + 激光。
// ⚠️ 这里必须是**独立的 #if 块**，不能写成上面的 #elif —— 融合节点两块都要编译，
// #elif 是互斥的，会让融合节点丢掉 dhtTick/laserTick（曾因此报 "'dht' was not declared"）。
#if NODE_ID == 2 || NODE_ID == 3
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
            LOGF("[dht] %d C %d %%\n", ti, hi);
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
    LOGF("[dht] read failed after %d retries (last ok: %d C %d %%)\n", DHT_RETRY, last_temp,
         last_hum);
    queueEvt("err", "dht");
}

// 读一次原始引脚电平（HIGH/LOW），仅用于日志显示：能一眼看出接线是否正常
static int readObstacleRaw() {
    return digitalRead(PIN_LASER);
}

// 归一化：1 = 检测到障碍；极性由 OBSTACLE_ACTIVE_LOW 决定
static int readObstacle() {
#if OBSTACLE_ACTIVE_LOW
    return (readObstacleRaw() == LOW) ? 1 : 0;
#else
    return (readObstacleRaw() == HIGH) ? 1 : 0;
#endif
}

static void laserTick() {
    if (millis() - laser_ms < LASER_PERIOD_MS) {
        return;
    }
    laser_ms = millis();
    int v = readObstacle();
    if (v == last_beam) {
        return;
    }
    delay(2);
    if (readObstacle() != v) {
        return;   // 两次读数不一致：当作抖动，丢弃
    }

    // 上电后第一次读到只记录、不上报不播报：否则模块前面本来就挡着东西时，
    // 一上电就会误播一次“有人经过”。
    bool first_read = (last_beam < 0);
    last_beam = v;
    // 日志同时打“逻辑结论”和“引脚真实电平”：之前写成 (DO=1) 是把归一化后的
    // 逻辑值当成了引脚电平，而红外避障是低电平有效，导致“检测到障碍”却显示 DO=1，误导排查
    if (first_read) {
        LOGF("[obstacle] init %s (pin=%s)\n", v ? "detected" : "clear",
             readObstacleRaw() ? "HIGH" : "LOW");
        return;
    }

    // 只在状态变化时打印（采样周期 50ms，不能每次都打）
    LOGF("[obstacle] %s (pin=%s)\n", v ? "detected" : "clear",
         readObstacleRaw() ? "HIGH" : "LOW");
    queueEvt("beam", v ? "1" : "0");
    if (v == 1) {
        queueSay("beam");   // 只有“检测到障碍”这个边沿才播报
    }
}
#endif

// ============================== Arduino ==============================

void setup() {
    // 0) 串口日志（不加 while(!Serial)：未接 USB 时不能卡住启动）
#if NODE_LOG
    Serial.begin(LOG_BAUD);
#endif
    LOGF("\n[espnow] ==== node %d boot ====\n", NODE_ID);
    LOGF("[espnow] chip=%s heap=%u name=%s caps=%d\n", ESP.getChipModel(),
         (unsigned)ESP.getFreeHeap(), node_def->name, node_def->cap_count);

    // 1) 传感器与灯：只初始化本节点真正接了的硬件。
    //    （原先 RGB 的三个 ledcAttach 是无条件执行的，会把玄关节点的 DHT11/激光脚
    //     （GPIO4/5）一并配成 LEDC 输出 —— 玄关节点没有 RGB，必须按条件跳过。）
#if NODE_ID == 1 || NODE_ID == 3
    ledcAttach(PIN_RGB_R, RGB_PWM_FREQ, RGB_PWM_BITS);
    ledcAttach(PIN_RGB_G, RGB_PWM_FREQ, RGB_PWM_BITS);
    ledcAttach(PIN_RGB_B, RGB_PWM_FREQ, RGB_PWM_BITS);
    applyLight();
    // 开局打印实际占空比：共阳模块在“关灯”时 duty=255（引脚高电平，与 + 同电位 = 灭）。
    // 若日志显 255 而灯仍亮，就是接线/极性反了（+ 接到了 GND，或模块其实是共阴），
    // 不是程序问题 —— 别再改代码了。
    LOGF("[led] init on=%d bright=%d rgb=%d,%d,%d anode=%d -> duty %u/%u/%u\n", light_on,
         light_bright, light_r, light_g, light_b, RGB_COMMON_ANODE,
         (unsigned)ledcRead(PIN_RGB_R), (unsigned)ledcRead(PIN_RGB_G),
         (unsigned)ledcRead(PIN_RGB_B));
#endif

#if NODE_ID == 1 || NODE_ID == 3
    pinMode(PIN_SONAR_TRIG, OUTPUT);
    pinMode(PIN_SONAR_ECHO, INPUT);
#endif

#if NODE_ID == 2 || NODE_ID == 3
    pinMode(PIN_LASER, INPUT);
    dht = new DHT(PIN_DHT, DHT11);
    dht->begin();
#endif

    // 2) WiFi/ESP-NOW：不连接任何 AP，只用 ESP-NOW
    WiFi.mode(WIFI_STA);
    // 千万不能连路由器：一旦连上 AP，信道就被 AP 锁死，而本节点要靠"逐个信道试"找主控，
    // 信道锁死就永远找不到。有些板子 NVS 里残留着以前存过的 WiFi（auto_connect=true），
    // WiFi.mode() 之后驱动会自己连上去，所以这里显式断开并清掉保存的 AP。
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, true);
    WiFi.setSleep(false);          // 节点常醒（USB 供电），保证随时收下行命令
    WiFi.setChannel(cur_channel);
    while (!WiFi.STA.started()) {
        delay(10);
    }
    // 开机打印真实状态：connected=1 说明节点偷偷连上了路由器（就是上面那个坑），
    // ch 是实际生效的信道 —— 排查"连不上"时先看这一行。
    {
        uint8_t boot_ch = cur_channel;
        wifi_second_chan_t boot_second = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&boot_ch, &boot_second);
        LOGF("[espnow] mac=%s hop ch %d..%d connected=%d ch=%u\n", WiFi.macAddress().c_str(),
             HOP_CHANNEL_MIN, HOP_CHANNEL_MAX, WiFi.isConnected() ? 1 : 0, boot_ch);
    }

    if (!ESP_NOW.begin(kPmk)) {    // 官方 ESP_NOW 类（核心自带）
        LOGF("[espnow] ESP_NOW.begin failed, restart\n");
        delay(1000);
        ESP.restart();
    }
    ESP_NOW.onNewPeer(onNewPeerCb, nullptr);

    // 上行一律用【明文广播】发，这是必须的：
    // ESP-NOW 的加密包要求接收端【事先】注册好带同一把 LMK 的对端，否则驱动解不开、
    // 直接把包丢掉；而主控是「收到包之后才注册本节点」，两边互相等待就成了死锁——
    // 主控永远看不到本节点，AI 那边一个设备都没有（这正是之前一直连不上的原因）。
    // 明文广播不需要预先注册，正好用来打破死锁；主控收到后会把本节点登记为加密对端，
    // 之后主控下发的加密命令，本节点靠上面那个 peer 照样能解开。
    tx_peer = new HomePeer(kBroadcastMac, 0, nullptr);
    if (tx_peer == nullptr || !tx_peer->attach()) {
        LOGF("[espnow] broadcast peer add failed, restart\n");
        delay(1000);
        ESP.restart();
    }
    last_seen_ms = millis();
}

void loop() {
    hopTick();

    // 锁定后失联 → 回 hop 重新发现（主控重启/换热点/换信道都能自恢复）
    if (locked && millis() - last_seen_ms > LOST_TIMEOUT_MS) {
        locked = false;
        // 必须先 detach 再 delete：只 delete 的话对端表里还留着主控 MAC，
        // 之后广播不再走 onNewPeerCb，节点就永远锁不回来了
        if (peer != nullptr) {
            peer->detach();
            delete peer;
            peer = nullptr;
        }
        LOGF("[espnow] lost master (%lu ms no packet), back to hop\n",
             (unsigned long)(millis() - last_seen_ms));
    }

    evtTick();
    infoTick();

#if NODE_ID == 1 || NODE_ID == 3
    sonarTick();
#endif
#if NODE_ID == 2 || NODE_ID == 3
    dhtTick();
    laserTick();
#endif
}
