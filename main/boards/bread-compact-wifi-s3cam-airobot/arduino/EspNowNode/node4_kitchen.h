/*
 * 节点 4「厨房」：MQ-2 烟雾/可燃气 + 火焰传感器（只监测、只播报）
 *
 * 这个文件是节点 4 的全部硬件相关代码：引脚、阈值、状态、采样 tick。
 * 主文件 EspNowNode.ino 只在 NODE_ID == 4 时 #include 它，并保留两处钩子调用：
 *   setup()  → kitchenSetup()   （引脚/ADC 初始化）
 *   loop()   → kitchenTick()    （燃气 + 火焰采样）
 * 能力表（smoke / flame）仍在主文件里 —— 那是"节点自描述"的统一入口。
 *
 * 依赖主文件已经提供的符号（本文件不重复实现）：
 *   NODE_ID、LOGF()、queueEvt()、queueSay()
 * queueEvt / queueSay 在主文件里定义得比本文件晚（在「上行发送」段），
 * 所以这里先声明它们的原型。
 *
 * 硬件要点（详见 README「节点 4」一节）：
 *   - MQ-2 必须用 5V 供电（加热丝按 5V 标定），DO/AO 都是 5V 域，
 *     必须各自串 1kΩ + 对地 2kΩ 分压后再进 GPIO（≈3.33V）；
 *   - 火焰模块用 3.3V 供电，DO/AO 直连，省掉分压；
 *   - 模拟量只能用 ADC1（GPIO1~GPIO10）：S3 的 ADC2 在 WiFi 打开时不可用，
 *     而本节点全程跑 ESP-NOW（射频常开）。
 */

#ifndef NODE4_KITCHEN_H
#define NODE4_KITCHEN_H

#include <Arduino.h>

// ============================ 现场可调参数 ============================

// 采样周期：燃气看趋势（200ms 足够，MQ-2 响应本身以秒计）；火焰要抓边沿（50ms）
#define GAS_PERIOD_MS 200
#define FLAME_PERIOD_MS 50

// 极性（两个模块都是 LM393 比较器输出）：1 = 低电平 = 超阈值 / 检测到火焰。
// 现象相反（干净空气也报警、没火也说有火）时把对应的一行取反。
#define GAS_ACTIVE_LOW 1
#define FLAME_ACTIVE_LOW 1

// MQ-2 必须预热：上电后读数会一路往下漂，约 3 分钟才稳。
// 预热期只上报不告警，否则每次上电都误报一次。
#define GAS_WARMUP_MS 180000

// 告警迟滞（浓度 0=清洁空气 100=浓）：≥GAS_TRIGGER 报警，≤GAS_RELEASE 才解除
#define GAS_TRIGGER 60
#define GAS_RELEASE 40

// 满量程标定：干净空气基线之上再升高多少 mV 记作浓度 100。
// AO 经 1k/2k 分压后引脚电压是模块输出的 2/3；打火机放气靠近时升幅约 1~2V（分压后 0.7~1.3V），
// 取 1500mV 既够灵敏，又不至于一点扰动就顶到 100。
#define GAS_FULL_SCALE_MV 1500

// 浓度上报降频：变化 ≥5 或 10 秒保活（同 dist 的思路，避免占满 2.4G）
#define SMOKE_REPORT_DELTA 5
#define SMOKE_REPORT_KEEPALIVE_MS 10000

// ---- 引脚 ----
#define PIN_GAS_DO 4
#define PIN_GAS_AO 1
#define PIN_FLAME_DO 5
#define PIN_FLAME_AO 2   // 火焰强度模拟量（预留：固件只用 DO 判有无明火）

// ============================== 运行状态 ==============================

static int smoke_level = 0;           // 0..100：相对干净空气基线的浓度（EMA 平滑后）
static int last_smoke = -1;           // 上次上报的浓度（-1 = 还没报过）
static uint32_t last_smoke_ms = 0;    // 上次上报时刻（保活周期用）
static bool smoke_alarm = false;      // 告警迟滞状态（true = 已报警）
static int gas_base_mv = 0;           // 预热期建立的干净空气基线（mV，预热结束后冻结）
static bool smoke_ema_ready = false;  // 浓度 EMA 是否已用首个样本初始化
static bool gas_warmed = false;       // 预热是否已结束（用标记而非 millis() < GAS_WARMUP_MS：
                                      // 后者在 millis 回绕（约 49 天）后会重新“预热”一次）
static uint32_t gas_ms = 0;
static uint32_t gas_warm_log_ms = 0;  // “预热中”日志限频
static int last_flame = -1;           // 上次火焰状态（-1 = 上电首帧还没读到）
static uint32_t flame_ms = 0;

// 依赖主文件（定义在本文件之后，先声明原型）
static void queueEvt(const char* name, const char* arg);
static void queueSay(const char* name);

// ===================== 能力处理函数（动作 → 结果文本）=====================

// 只返回缓存值、不在 handler 里重新采样：AI 追问时要立刻回答，不能阻塞命令回执
static bool capSmokeRead(const char* action, const char* args, char* out, size_t out_len) {
    (void)args;
    if (strcmp(action, "read") != 0) {
        return false;
    }
    snprintf(out, out_len, "%d", smoke_level);
    return true;
}

static bool capFlameRead(const char* action, const char* args, char* out, size_t out_len) {
    (void)args;
    if (strcmp(action, "read") != 0 || last_flame < 0) {
        return false;
    }
    snprintf(out, out_len, "%d", last_flame);
    return true;
}

// ============================== 传感器 tick ==============================

// 读一次引脚真实电平（HIGH/LOW），仅用于日志：能一眼看出分压/接线是否正常
static int readGasDoRaw() {
    return digitalRead(PIN_GAS_DO);
}

static void gasTick() {
    if (millis() - gas_ms < GAS_PERIOD_MS) {
        return;
    }
    gas_ms = millis();

    const int mv = static_cast<int>(analogReadMilliVolts(PIN_GAS_AO));
    if (!gas_warmed && millis() >= GAS_WARMUP_MS) {
        gas_warmed = true;
    }
    const bool warm = !gas_warmed;
    const int do_raw = readGasDoRaw();
#if GAS_ACTIVE_LOW
    const bool do_triggered = (do_raw == LOW);
#else
    const bool do_triggered = (do_raw == HIGH);
#endif

    // 基线：AO 的绝对电压会随温湿度/老化/供电漂移，“浓度”只能相对干净空气看。
    // 预热期读数本身还在一路往下漂，所以用很慢的 EMA（64 拍 ≈ 13 秒）跟着它走；
    // 预热一结束就冻结 —— 否则真报警时基线被抬高，浓度会被自己抹平。
    if (warm) {
        gas_base_mv = (gas_base_mv == 0) ? mv : (gas_base_mv * 63 + mv) / 64;
    }

    // 相对浓度 0..100，再用 EMA 平滑（原始信号有毛刺，直接上报会让 AI 看到锯齿）
    int conc = 0;
    if (gas_base_mv > 0) {
        int delta = mv - gas_base_mv;
        if (delta < 0) {
            delta = 0;
        }
        conc = delta * 100 / GAS_FULL_SCALE_MV;
        if (conc > 100) {
            conc = 100;
        }
    }
    if (!smoke_ema_ready) {
        smoke_level = conc;
        smoke_ema_ready = true;
    } else {
        smoke_level = (smoke_level * 3 + conc) / 4;
    }

    // 预热进度（限频 30 秒）：现场据此知道“没报警不是坏了，是还没热稳”
    if (warm && (gas_warm_log_ms == 0 || millis() - gas_warm_log_ms >= 30000)) {
        gas_warm_log_ms = millis();
        LOGF("[燃气] 预热中（剩 %u 秒），只上报不告警\n",
             (unsigned)((GAS_WARMUP_MS - millis()) / 1000));
    }

    // 上报：预热期也报（只是不告警）；变化 ≥5 或 10 秒保活才发，避免占满 2.4G
    if (last_smoke < 0 || abs(smoke_level - last_smoke) >= SMOKE_REPORT_DELTA ||
        millis() - last_smoke_ms >= SMOKE_REPORT_KEEPALIVE_MS) {
        char arg[8];
        snprintf(arg, sizeof(arg), "%d", smoke_level);
        queueEvt("smoke", arg);
        last_smoke = smoke_level;
        last_smoke_ms = millis();
        if (!warm) {
            LOGF("[燃气] 浓度 %d（DO=%s%s）\n", smoke_level, do_raw ? "HIGH" : "LOW",
                 do_triggered ? "，超过电位器阈值" : "");
        }
    }

    // 报警：DO（电位器阈值）与浓度阈值是两套判据，取“或”。
    // DO 翻低时把有效浓度抬到触发线，避免“电位器已经报了、浓度只到 50”绕过告警；
    // 解除则要求 DO 恢复且浓度回落到 GAS_RELEASE 以下（迟滞防抖）。
    int eff = smoke_level;
    if (do_triggered && eff < GAS_TRIGGER) {
        eff = GAS_TRIGGER;
    }
    if (!warm) {
        if (!smoke_alarm && eff >= GAS_TRIGGER) {
            smoke_alarm = true;
            LOGF("[燃气] 浓度 %d（DO=%s）超过阈值，报警\n", smoke_level, do_raw ? "HIGH" : "LOW");
            queueSay("smoke");   // 主控播 smoke.mp3；同一 (节点,名字) 有 10 秒冷却
        } else if (smoke_alarm && eff <= GAS_RELEASE) {
            smoke_alarm = false;
            LOGF("[燃气] 浓度回落到 %d（DO=%s），告警解除\n", smoke_level,
                 do_raw ? "HIGH" : "LOW");
        }
    }
}

static int readFlameDoRaw() {
    return digitalRead(PIN_FLAME_DO);
}

// 归一化：1 = 检测到火焰；极性由 FLAME_ACTIVE_LOW 决定
static int readFlame() {
#if FLAME_ACTIVE_LOW
    return (readFlameDoRaw() == LOW) ? 1 : 0;
#else
    return (readFlameDoRaw() == HIGH) ? 1 : 0;
#endif
}

// 与主文件的 laserTick 同一套写法：去抖 + 只在边沿上报/播报 + 上电首帧只记录
static void flameTick() {
    if (millis() - flame_ms < FLAME_PERIOD_MS) {
        return;
    }
    flame_ms = millis();
    int v = readFlame();
    if (v == last_flame) {
        return;
    }
    delay(2);
    if (readFlame() != v) {
        return;   // 两次读数不一致：当作抖动，丢弃
    }

    // 上电首帧只记录不播报：否则模块前本来就点着火/有阳光直射时会误播一次
    bool first_read = (last_flame < 0);
    last_flame = v;
    if (first_read) {
        LOGF("[火焰] 上电首帧：%s（引脚=%s）\n", v ? "检测到火焰" : "无火焰",
             readFlameDoRaw() ? "HIGH" : "LOW");
        return;
    }
    LOGF("[火焰] %s（引脚=%s）\n", v ? "检测到火焰" : "无火焰",
         readFlameDoRaw() ? "HIGH" : "LOW");
    queueEvt("flame", v ? "1" : "0");
    if (v == 1) {
        queueSay("flame");   // 只有“检测到明火”这个边沿才播报
    }
}

// ===================== 主文件 setup() / loop() 的钩子 =====================

static void kitchenSetup() {
    // 两个模块的 DO 都是数字输出；模拟量只用 MQ-2 那一路（火焰只用 DO 判有无）。
    // AO 显式指定 11db 衰减（约 0~3.1V 量程，与默认值一致，写出来是为了让看接线的人一眼看到量程）。
    pinMode(PIN_GAS_DO, INPUT);
    pinMode(PIN_FLAME_DO, INPUT);
    analogSetPinAttenuation(PIN_GAS_AO, ADC_11db);
}

static void kitchenTick() {
    gasTick();
    flameTick();
}

#endif   // NODE4_KITCHEN_H
