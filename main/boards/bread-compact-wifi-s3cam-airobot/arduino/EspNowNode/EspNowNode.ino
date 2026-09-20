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
 * 协议（与主控 espnow_home.cc 一致，@ 前缀文本行；序号 `#<seq>` 放在**信封**里，
 * 正文格式与首版一致 → 两侧任一没更新也不会彻底不能用）：
 *   下行  @n<id>[#seq] do <能力> <动作> [参数]  通用动作（主控不解释语义，原样透传）
 *   上行  @n<id> info <名字> <能力规格>      能力自描述（锁信道后 + 每次收到 beacon）
 *         @n<id>[#seq] ok <能力> <结果>      执行回执（带回主控的序号 → 主控确认送达）
 *         @n<id> say <名字>                 播报请求（主控播 <名字>.mp3）
 *         @n<id> evt <名字> <值>            状态上报（只更新状态，不播报）
 *         @n<id> evt hb 1                   心跳（协议保留名：只让主控知道"我在当前信道"）
 *         @n<id> err <原因> [细节]          错误
 *
 * 可靠性（依据 IDF v6.0.2 官方文档 esp_now.rst 的"应用层 ACK + 序列号去重"建议）：
 *   主控下发带序号，收不到回执会重传同一序号；节点对同一序号**只执行一次**，
 *   重复到达时直接重发上次回执，不重复动作（对 speed+ 这类非幂等动作必须如此）。
 *
 * 编译: arduino-cli compile --fqbn esp32:esp32:esp32s3 <此目录>
 * 依赖: 灯与 ESP-NOW 用核心自带 API，零第三方库；
 *       仅节点 2 的 DHT11 需要 "DHT sensor library"(Adafruit)，且只在 NODE_ID==2 时包含。
 */

// ============================ 现场可调参数 ============================
// 注意：这些宏必须在下面的 #if 之前定义（预处理按顺序求值）。

#define NODE_ID 3                    // 1=客厅灯(RGB+超声波) 2=玄关感应(DHT11+激光) 3=我的家(四件套)
#define HOP_INTERVAL_MS 500    // 每个信道停留时长。主控稳态每 3000ms 广播一次，但
                               // "AP 换信道 / 命令未确认"时主控会切到 500ms 快速窗口；
                               // 节点停 500ms → 一圈 13×0.5=6.5s，快速窗口内必然撞上一次
                               // （旧值 2000ms 一圈 26s，配合 3s 稳态 beacon 要转好几圈）
#define LOST_TIMEOUT_MS 5000         // 锁定后多久收不到主控包才回 hop（配合主控快速窗口；
                                     // 旧值 12s 太长，AP 换信道后要等十几秒才开始找）
#define HOP_CHANNEL_MIN 1
#define HOP_CHANNEL_MAX 13

// 心跳：每 5 秒一条 `@n<id> evt hb 1`。不是业务数据，而是让主控知道
// "这个节点还在当前信道上"——否则节点无传感器变化时主控 15 秒就判离线（假离线，
// 表现为 AI 拒绝执行控制命令）。主控侧对 hb 不进状态缓存、不回调业务。
#define HB_PERIOD_MS 5000

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
// 我的家：RGB 已占用 GPIO4/5/6，DHT11 与激光改用两个空闲脚
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
#include <DHT.h>   // 仅玄关/我的家需要，故按条件包含（客厅节点无需安装该库）
#endif

// ============================== 运行状态 ==============================

static bool locked = false;                 // 是否已锁定主控信道
static uint8_t cur_channel = 1;
static uint32_t last_hop_ms = 0;
static uint32_t last_seen_ms = 0;           // 最近一次收到主控任何报文

static uint32_t info_due_ms = 0;            // 能力重报（收到 beacon 时置位，loop 里安全发送）
static uint32_t hb_ms = 0;                  // 上次心跳时刻（链路存活信号，见 HB_PERIOD_MS）

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
// 为什么是 6 槽：我的家(3) 有 4 路状态上报（dist/motion/temp/beam）各占一槽后，
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
// 我的家：一台设备接全部四个传感器。
// 4 个能力的 info 报文实测约 194B，未超主控 200B 单包上限（描述文案别再拉长）。
static const CapDef kCaps[] = {
    {"light", "(RGB灯):on(0|1),off(),rgb(r,g,b),bright(0-255),read()", capLight},
    {"dist", "(超声波距离cm,只读):read()", capDistRead},
    {"temp", "(温湿度℃/%,只读):read()", capTempRead},
    {"beam", "(红外避障0=无1=有,只读):read()", capBeamRead},
};
static const char kNodeName[] = "我的家";

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
        LOGF("[警告] 上行队列已满，丢弃最旧一条\n");
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

// 回执去重缓存：主控重传时序号不变，同一序号只执行一次，重复到达直接重发上次回执。
// （官方文档建议"设置序列号从而删除重复的数据"；对 speed+ 这类非幂等动作必须如此。）
static uint16_t last_cmd_seq = 0;
static char last_reply[96] = {0};

// 能力动作回执：kind = "ok" | "err"；seq 由主控信封（@n<id>#<seq>）带下来，
// 回执原样带回，主控据此确认命令送达（seq=0 表示旧主控，保持原格式、不去重）。
static void qCap(uint16_t seq, const char* kind, const char* a, const char* b) {
    char buf[96];
    int n;
    if (seq != 0) {
        if (b != nullptr && b[0] != '\0') {
            n = snprintf(buf, sizeof(buf), "@n%d#%u %s %s %s", NODE_ID, (unsigned)seq, kind, a, b);
        } else {
            n = snprintf(buf, sizeof(buf), "@n%d#%u %s %s", NODE_ID, (unsigned)seq, kind, a);
        }
        last_cmd_seq = seq;
        if (n > 0) {
            size_t len = ((size_t)n < sizeof(last_reply) - 1) ? (size_t)n : sizeof(last_reply) - 1;
            memcpy(last_reply, buf, len);
            last_reply[len] = '\0';
        }
    } else {
        if (b != nullptr && b[0] != '\0') {
            snprintf(buf, sizeof(buf), "@n%d %s %s %s", NODE_ID, kind, a, b);
        } else {
            snprintf(buf, sizeof(buf), "@n%d %s %s", NODE_ID, kind, a);
        }
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
        // ⚠️ ESP-NOW 回调给的是「原始字节 + 长度」，**没有 '\0' 终止符**
        // （ESP32_NOW.cpp 的 _esp_now_rx_cb 把 IDF 的 data/len 原样透传）。
        // 而下面 handleCommand/nextField 全是 C 字符串解析（strcmp/strchr/strlen），
        // 不补终止符就会越过包尾去读驱动缓冲里的残留字节——症状是**最后一个字段**
        // 带上乱码：`do light off` 被解析成 `offxV??` → 回 err unknown-action
        // → AI 说「关灯」关不掉（带参数的 rgb/bright 因 action 后紧跟空格而侥幸正常，
        // 所以只在 off/on/read 这类无参数动作上暴露，且是否命中取决于缓冲残留）。
        // 必须先拷到本地缓冲并按 len 补 '\0'，之后一律用 text。
        char text[256];
        if (len >= sizeof(text)) {
            return;   // 超长包直接丢弃（主控单包上限 200B，正常不会走到）
        }
        memcpy(text, data, len);
        text[len] = '\0';

        if (broadcast) {
            // 主控 beacon（广播）：重报一次能力描述。
            // 主控重启后注册表是空的，靠这条自愈；发在 loop 里做，避免在回调里做重活。
            if (strncmp(text, "@beacon", 7) == 0) {
                info_due_ms = millis();
                // 低频证明“链路是通的”：beacon 稳态每 3 秒一条，每条都打会把真正
                // 有用的日志冲掉，所以限频 60 秒（想看实时收发看 [发送]/[收到]）
                static uint32_t beacon_log_ms = 0;
                if (beacon_log_ms == 0 || millis() - beacon_log_ms >= 60000) {
                    beacon_log_ms = millis();
                    LOGF("[信道] 收到主控广播（链路正常）\n");
                }
            }
            return;
        }
        const char* p = text + 2;
        if (text[1] != 'n') {
            return;   // 不是发给本节点的
        }
        // 信封：`@n<id>` 或 `@n<id>#<seq>`（序号可选，旧主控不带）
        char* end = nullptr;
        long id = strtol(p, &end, 10);
        if (end == nullptr || id != NODE_ID) {
            return;   // 不是发给本节点的
        }
        uint16_t seq = 0;
        if (*end == '#') {
            seq = static_cast<uint16_t>(strtoul(end + 1, &end, 10));
        }
        if (*end != ' ') {
            return;
        }
        handleCommand(end + 1, seq);
    }

    void onSent(bool success) override {
        (void)success;   // 上行已在应用层连发 EVENT_RESEND 次，无需在此重传
    }

private:
    // 通用动作：do <能力> <动作> [参数]（seq 来自信封，0=旧主控不带序号）
    // 主控不解释语义、本节点按能力表分发——接入新动作只需改能力表。
    static void handleCommand(const char* body, uint16_t seq) {
        if (strncmp(body, "do ", 3) != 0) {
            return;   // 其它命令（ping 等）：收到即刷新在线（onReceive 已做）
        }
        // 主控未收到回执时会重传同一序号：只执行一次，直接重发上次回执
        if (seq != 0 && seq == last_cmd_seq && last_reply[0] != '\0') {
            LOGF("[收到] 第%u条指令重复送达，只重发上次回执、不重复执行\n", (unsigned)seq);
            queueEvent(last_reply);
            return;
        }
        LOGF("[收到] 主控指令（第%u条）：%s\n", (unsigned)seq, body);   // 下行低频，直接打印不影响实时性
        char cap[16] = {0};
        char action[16] = {0};

        const char* rest = nextField(body + 3, cap, sizeof(cap));
        if (cap[0] == '\0') {
            return;   // 连能力名都没有：丢弃（无法回答是哪个能力出错）
        }
        rest = nextField(rest, action, sizeof(action));
        if (action[0] == '\0') {
            qCap(seq, "err", "missing-action", cap);
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
                qCap(seq, "err", "readonly", cap);
                return;
            }
            char out[48];
            out[0] = '\0';
            if (node_def->caps[i].handler(action, args, out, sizeof(out))) {
                qCap(seq, "ok", cap, out);
            } else {
                qCap(seq, "err", "unknown-action", action);
            }
            return;
        }
        qCap(seq, "err", "unknown-cap", cap);
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
            LOGF("[信道] 登记主控失败，继续扫描信道\n");
        } else {
            info_due_ms = millis();   // 锁定成功：立刻上报能力
            LOGF("[信道] 已锁定主控 %s（实际信道 %u，扫描计数 %u）\n", fmtMac(info->src_addr),
                 real_ch, cur_channel);
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
        LOGF("[警告] 本节点已连上路由器，信道被锁死 -> 无法扫描信道\n");
    }
    last_hop_ms = millis();
    if (cur_channel >= HOP_CHANNEL_MAX) {
        cur_channel = HOP_CHANNEL_MIN;
        // 每轮（500ms × 13 ≈ 6.5 秒）打一行：现场据此判断“确实在扫信道但一直没收到主控广播”
        LOGF("[信道] 扫描中...（还没收到主控广播）\n");
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
        LOGF("[信道] 切换到信道 %u 失败：%d\n", cur_channel, (int)ch_err);
    }
}

// 把一条上行报文翻译成一句中文（**仅供串口日志**；不参与协议，也不**不认识任何具体
// 能力名/事件名**—— 只认协议层的 kind，保持“节点自描述”的设计）。
// 为什么要翻译：现场看串口的人（DIY 演示者）看不懂 `@n3 evt dist 57` 这类报文；
// 另外 `info` 报文的规格很长（≈194B），原样打印会把真正有用的日志冲掉，所以只打摘要。
static void describeUplink(const char* text, char* out, size_t out_len) {
    const char* p = text;
    if (p[0] == '@' && p[1] == 'n') {          // 跳过 "@n<id>" 与可选 "#<seq>" 信封
        p += 2;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '#') {
            p++;
            while (*p >= '0' && *p <= '9') p++;
        }
        if (*p == ' ') p++;
    }
    if (strncmp(p, "info ", 5) == 0) {
        snprintf(out, out_len, "上报设备能力清单");
    } else if (strncmp(p, "evt hb ", 7) == 0) {
        snprintf(out, out_len, "心跳");
    } else if (strncmp(p, "evt ", 4) == 0) {
        snprintf(out, out_len, "上报状态：%s", p + 4);
    } else if (strncmp(p, "say ", 4) == 0) {
        snprintf(out, out_len, "请求播放提示音：%s.mp3", p + 4);
    } else if (strncmp(p, "ok ", 3) == 0) {
        snprintf(out, out_len, "回执·执行成功：%s", p + 3);
    } else if (strncmp(p, "err ", 4) == 0) {
        snprintf(out, out_len, "回执·执行失败：%s", p + 4);
    } else {
        snprintf(out, out_len, "上报：%s", p);
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
    // 连发只在首包打印一行（中文描述，一眼看出在干什么）；重发仅在失败时补一行
    if (left == EVENT_RESEND - 1) {
        char desc[96];
        describeUplink(buf, desc, sizeof(desc));
        LOGF("[发送] %s（第%d/%d次）%s\n", desc, EVENT_RESEND - left, EVENT_RESEND,
             ok ? "" : "  <== 发送失败");
    } else if (!ok) {
        LOGF("[发送] 第%d/%d次失败\n", EVENT_RESEND - left, EVENT_RESEND);
    }
}

// 心跳（锁定后每 HB_PERIOD_MS 一条）：给主控"我在、我在这个信道上"的信号。
// 走 evt 通道（不播报）+ 队列同键合并（键固定，永远只占一槽）。
static void hbTick() {
    if (!locked) {
        return;   // 未锁定主控：心跳没意义（主控也收不到）
    }
    if (hb_ms != 0 && millis() - hb_ms < HB_PERIOD_MS) {
        return;
    }
    hb_ms = millis();
    queueEvt("hb", "1");
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
            LOGF("[距离] 无回波（超时），保持上次判断\n");
        }
        return;
    }
    int cm = static_cast<int>(us / 58);

    // 读数日志（限频 2 秒）：不打印的话，现场"看不到超声波信息"无法区分
    // “确实没测到”与“测到了只是没打印”。
    if (sonar_ok_log_ms == 0 || millis() - sonar_ok_log_ms >= 2000) {
        sonar_ok_log_ms = millis();
        LOGF("[距离] %d 厘米（%s）\n", cm, motion_active ? "有人" : "无人");
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
        // 播报**只跟着“真的自动开了灯”走**，不再是人一到就播。两个理由：
        // ① 灯本来就亮着（语音开的 / 刚有人来过）时再播“已为你开灯”是说谎；
        // ② 被人工接管（motion_suppress）时根本没开灯，播报同样说谎。
        // 状态上报（evt motion 1）与播报解耦、仍无条件发 —— AI 始终能知道“有人”。
        bool did_auto_on = false;
        if (!motion_suppress) {
            if (!light_on) {
                auto_lit = true;   // 只接管“本来就是灭的”灯；用户自己开的灯不标记
                light_on = 1;
                applyLight();
                did_auto_on = true;
                LOGF("[人感] 有人靠近：自动开灯（并请求播报）\n");
            } else {
                LOGF("[人感] 有人靠近：灯已亮着，保持不动、不播报\n");
            }
        } else {
            LOGF("[人感] 有人靠近：人工接管中，不动灯、不播报\n");   // 人工已接管，不自作主张开灯
        }
        queueEvt("motion", "1");
        if (did_auto_on) {
            queueSay("motion");   // 只有真开了灯才播“检测到有人靠近，已为你开灯”
        }
    } else if (motion_active && cm > MOTION_RELEASE_CM) {
        motion_active = false;
        motion_clear_ms = millis();   // 记录“人走了”，满 MOTION_SUPPRESS_HOLD_MS 才解除抑制
        queueEvt("motion", "0");
        if (MOTION_AUTO_OFF_MS > 0 && auto_lit) {
            auto_off_due_ms = millis() + MOTION_AUTO_OFF_MS;
            LOGF("[人感] 人已离开，%d 毫秒后自动关灯\n", MOTION_AUTO_OFF_MS);
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
            LOGF("[人感] 人工接管解除，恢复自动开灯\n");
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
            LOGF("[人感] 自动关灯（已 %d 毫秒无人）\n", MOTION_AUTO_OFF_MS);
        }
    }
}
#endif   // NODE_ID == 1 || NODE_ID == 3（超声波）

// 玄关 / 我的家：DHT11 + 激光。
// ⚠️ 这里必须是**独立的 #if 块**，不能写成上面的 #elif —— 我的家两块都要编译，
// #elif 是互斥的，会让我的家丢掉 dhtTick/laserTick（曾因此报 "'dht' was not declared"）。
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
            LOGF("[温度] %d°C 湿度 %d%%\n", ti, hi);
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
    LOGF("[温度] 读取失败（重试 %d 次，上次成功 %d°C %d%%）\n", DHT_RETRY, last_temp, last_hum);
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
        LOGF("[避障] 上电首帧：%s（引脚=%s）\n", v ? "检测到障碍" : "无障碍",
             readObstacleRaw() ? "HIGH" : "LOW");
        return;
    }

    // 只在状态变化时打印（采样周期 50ms，不能每次都打）
    LOGF("[避障] %s（引脚=%s）\n", v ? "检测到障碍" : "无障碍",
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
    LOGF("\n[启动] ==== 节点 %d 上电 ====\n", NODE_ID);
    LOGF("[启动] 芯片=%s 剩余内存=%u 设备名=%s 能力数=%d\n", ESP.getChipModel(),
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
    LOGF("[灯] 初始化：开关=%d 亮度=%d 颜色=%d,%d,%d 共阳=%d -> 占空比 %u/%u/%u\n", light_on,
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
        LOGF("[信道] 本机MAC=%s 扫描信道 %d..%d 已连路由器=%d 当前信道=%u\n",
             WiFi.macAddress().c_str(), HOP_CHANNEL_MIN, HOP_CHANNEL_MAX,
             WiFi.isConnected() ? 1 : 0, boot_ch);
    }

    if (!ESP_NOW.begin(kPmk)) {    // 官方 ESP_NOW 类（核心自带）
        LOGF("[信道] ESP-NOW 初始化失败，重启\n");
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
        LOGF("[信道] 广播对端登记失败，重启\n");
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
        LOGF("[信道] 与主控失联（%lu 毫秒未收到包），重新扫描信道\n",
             (unsigned long)(millis() - last_seen_ms));
    }

    evtTick();
    infoTick();
    hbTick();

#if NODE_ID == 1 || NODE_ID == 3
    sonarTick();
#endif
#if NODE_ID == 2 || NODE_ID == 3
    dhtTick();
    laserTick();
#endif
}
