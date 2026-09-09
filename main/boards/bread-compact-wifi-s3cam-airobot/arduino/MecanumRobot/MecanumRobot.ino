// ==================================================================
// 麦克纳姆轮机器人 (MecanumRobot) —— Arduino 下位机
// ==================================================================
// 【硬件连接】
//   - Emakefun 电机驱动板 (I2C 地址 0x60)
//       * 4 个直流电机(麦克纳姆轮): motors[0~3]
//       * 2 个舵机: servo1(头部), servo2
//   - PS2 手柄 (PS2X 库): config_gamepad(13,11,10,12)
//   - 蜂鸣器: 接 A0 (NewTone)
//   - 串口: 与上位机 ESP32 通信, 波特率 115200; 命令以 '@' 开头
//
// 【上位机(ESP32)串口指令】(必须以 '@' 开头, 其余行忽略)
//   @go-{action}-{steps}   动作控制
//       action: forward back left right leftmove rightmove leftup rightup leftdown rightdown
//       steps : 步数(普通 *100ms, 左/右转 *10ms)
//   @servo-{degree}        设置舵机1 角度(0-180)
//   @speed-{value}         设置电机速度(70-255)
//   @tj-yaotou / @tj-shandian / @tj-zhuanquan / @tj-sxzw / @tj-diaotou   特技
//   @line-start / @line-stop   巡线模式(沿地面黑线自动行驶)开始/停止
//   @drive-{action}-{speed}    进入 web 驾驶模式(非阻塞连续控制): 方向 + 速度(70-255)
//   @drive-stop                退出 web 驾驶模式并停止
//       action 同 go-* : forward back left right leftmove rightmove leftup rightup leftdown rightdown
//       web 驾驶为遥感轮询(类似手柄): 上位机按住期间周期性重发心跳, 无心跳超时自动停
//
// 【双向回执】(Arduino -> ESP32, 供 self.uno.get_status 查询)
//   @busy {动作} / @done {动作}   耗时动作开始/完成(go-*, tj-*, line-follow)
//   @stat s{速度} v{舵机1角度}    速度/舵机变化时上报最新快照(事件驱动, 非常驻周期)
//
// 【PS2 手柄键位】
//   十字键: 移动      PINK/RED: 左右转      GREEN/BLUE(单击): 减速/加速
//   L1/R1/L2/R2: 斜移    SELECT/START: 舵机微调
// ==================================================================

// 串口 RX 缓冲说明(重要): UNO 默认 64B≈4-5 条指令, AI 长指令序列会溢出丢指令。
// arduino:avr 1.8.8+ 的 HardwareSerial 用编译期宏 SERIAL_RX_BUFFER_SIZE 控制缓冲,
// 没有 setRxBufferSize API。因此 .ino 里无法设置, 需在编译时传入宏(见 README):
//   arduino-cli compile --fqbn arduino:avr:uno --build-property "compiler.cpp.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" ...
// Arduino IDE 用户: 在核心目录建 platform.local.txt 写入
//   compiler.cpp.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256

#include <Emakefun_MotorDriver.h>
#include <NewTone.h>
#include <PS2X_lib.h>
#include <string.h>
#include <stdlib.h>

// ---------------- 驱动 & 手柄对象 ----------------
Emakefun_MotorDriver mMotorDriver(0x60);   // 电机驱动板 (I2C 0x60)
PS2X ps2x;                                 // PS2 手柄

// ---------------- 电机 / 舵机对象 ----------------
Emakefun_DCMotor *motors[4] = {            // 麦克纳姆轮电机 [前左,前右,后左,后右]
    mMotorDriver.getMotor(1),
    mMotorDriver.getMotor(2),
    mMotorDriver.getMotor(3),
    mMotorDriver.getMotor(4)
};
Emakefun_Servo *servo1 = mMotorDriver.getServo(1);   // 舵机1 (头部)
Emakefun_Servo *servo2 = mMotorDriver.getServo(2);   // 舵机2

// ---------------- 巡线传感器(4路) ----------------
// 接线: S4->D2, S3->D3, S2->D4, S1->D7 (S1..S4 从左到右)
#define LINE_S1  7
#define LINE_S2  4
#define LINE_S3  3
#define LINE_S4  2
// 电平方向: 默认黑线=HIGH(反射型模块常见); 若反向(黑线=LOW)改为 false
#define LINE_ACTIVE  true
// 巡线参数
#define LINE_BASE_SPEED  170   // 巡线基础速度(低于全局 speed 默认200, 更稳)
#define LINE_KP          60    // 差速比例系数(越大转向越猛)
#define LINE_LOST_MS     300   // 连续丢线超过此毫秒数 -> 判定巡线结束
#define LINE_MAX_MS      120000 // 巡线总时长上限(2分钟, 防死循环)

// ---------------- 全局状态 ----------------
uint8_t motorSpeed[4] = {200, 200, 200, 200};   // 4 个电机速度
uint8_t speed      = 200;                       // 当前(全局)速度
uint8_t servo1Zero = 82;                        // 舵机1 回正角度 (大于=左转, 小于=右转)
uint8_t servo1Angle = 82;                       // 舵机1 当前角度
uint8_t servo2Angle = 90;                       // 舵机2 当前角度
bool     started = true;                        // 主循环开关
bool     ps2_ready = false;                     // PS2 手柄是否就绪(config_gamepad 成功); 无手柄时跳过轮询

// ---------------- web 驾驶模式(非阻塞连续控制遥感) ----------------
// 由上位机 @drive-{action}-{speed} 驱动(类似手柄摇杆): 方向 + 速度;
// 上位机按住期间周期性重发心跳, 无心跳超时自动停机(防 web 断连)
bool            web_drive_ = false;             // 是否处于 web 驾驶模式
char            web_drive_action_[16] = "";     // 当前持续方向(forward/back/...)
uint8_t         web_drive_speed_ = 200;         // 当前驾驶速度(70~255)
unsigned long   web_drive_since_ms_ = 0;        // 最近一次心跳时刻(看门狗基准)
#define WEB_DRIVE_WATCHDOG_MS  1500             // 无心跳超时自动停(防 web 断开)
#define WEB_DRIVE_PULSE_MS     30               // 每帧短脉冲时长(ms), 模拟持续运动

// ==================================================================
// 底层控制
// ==================================================================

// 控制单个电机。
// @param idx 电机索引 0~3 (0前左,1前右,2后左,3后右)
// @param dir 方向: "F"正转, "B"反转, 其它=刹车
void motorRun(int idx, String dir) {
    Emakefun_DCMotor *m = motors[idx];
    m->setSpeed(motorSpeed[idx]);

    if (dir == "F") {
        m->run(FORWARD);
    } else if (dir == "B") {
        m->run(BACKWARD);
    } else {
        m->run(BRAKE);
    }
}

// 执行一次动作: 同时设置 4 个电机方向并持续 t 毫秒。
// @param m1..m4 各电机方向("F"/"B"/"S")
// @param t      动作持续时间(毫秒)
void runMotors(String m1, String m2, String m3, String m4, int t) {
    motorRun(0, m1);
    motorRun(1, m2);
    motorRun(2, m3);
    motorRun(3, m4);
    delay(t);
}

// ==================================================================
// 麦克纳姆轮运动
//   电机布局 m0(前左) m1(前右) / m2(后左) m3(后右)
//   dir: "F"正转 "B"反转 "S"刹车
// ==================================================================

// 前进: 前左/前右正转, 后左/后右反转 -> 整体向前
void moveForward(int t) {
    runMotors("F", "F", "B", "B", t);
    Serial.println("F");
}

// 后退: 前左/前右反转, 后左/后右正转 -> 整体向后
void moveBackward(int t) {
    runMotors("B", "B", "F", "F", t);
    Serial.println("B");
}

// 原地左转: 四轮全部反转
void turnLeft(int t) {
    runMotors("B", "B", "B", "B", t);
    Serial.println("L");
}

// 原地右转: 四轮全部正转
void turnRight(int t) {
    runMotors("F", "F", "F", "F", t);
    Serial.println("R");
}

// 左平移: 麦克纳姆轮斜向滚动
void moveLeft(int t) {
    runMotors("B", "F", "B", "F", t);
    Serial.println("LL");
}

// 右平移
void moveRight(int t) {
    runMotors("F", "B", "F", "B", t);
    Serial.println("RR");
}

// 左前斜移
void moveLeftForward(int t) {
    runMotors("S", "F", "B", "S", t);
}

// 右前斜移
void moveRightForward(int t) {
    runMotors("F", "S", "S", "B", t);
}

// 左后斜移
void moveLeftBackward(int t) {
    runMotors("B", "S", "S", "F", t);
}

// 右后斜移
void moveRightBackward(int t) {
    runMotors("S", "B", "F", "S", t);
}

// 前进 + 左转 (组合动作, 预留)
void forwardTurnLeft(int t) {
    runMotors("F", "B", "B", "B", t);
}

// 前进 + 右转 (组合动作, 预留)
void forwardTurnRight(int t) {
    runMotors("F", "F", "B", "F", t);
}

// 全部停止
void stopMove(int t) {
    runMotors("S", "S", "S", "S", t);
    Serial.println("STOP");
}

// ==================================================================
// 速度控制
// ==================================================================

// 设置全局速度并同步到 4 个电机。范围 70-255。
// @param value 目标速度
void setSpeed(int value) {
    speed = value;
    for (int i = 0; i < 4; i++) {
        motorSpeed[i] = value;
    }
    Serial.println(String("speed:") + value);
    reportStat();
}

// 增量调整速度(手柄用): 当前速度加 delta, 限制在 70-255。
// @param delta 速度增量(正=加快, 负=减慢)
void updateSpeed(int delta) {
    int v = speed + delta;
    if (v >= 255) {
        v = 255;
    } else if (v <= 70) {
        v = 70;
    }
    setSpeed(v);
}

// ==================================================================
// 巡线 (4路循迹传感器 + 比例差速控制)
// ==================================================================

// 巡线状态
bool line_following_ = false;      // 巡线模式开关
unsigned long line_start_ms_ = 0;  // 巡线开始时间
unsigned long line_lost_since_ = 0; // 丢线起始时刻(0=未丢线)

// 读取 4 路传感器并计算线位置(-2..+2, 负数偏左, 正数偏右)。
// 返回 99 = 全灭(丢线/线断), 100 = 全亮(十字路口/粗线, 按直行处理)。
// 若传感器方向反了(S1 在右), 把返回值取反即可。
int readLinePosition() {
    bool s1 = digitalRead(LINE_S1) == (LINE_ACTIVE ? HIGH : LOW);
    bool s2 = digitalRead(LINE_S2) == (LINE_ACTIVE ? HIGH : LOW);
    bool s3 = digitalRead(LINE_S3) == (LINE_ACTIVE ? HIGH : LOW);
    bool s4 = digitalRead(LINE_S4) == (LINE_ACTIVE ? HIGH : LOW);
    int on = s1 + s2 + s3 + s4;
    if (on == 0) return 99;   // 全灭: 丢线
    if (on == 4) return 100;  // 全亮: 路口/粗线, 按直行
    // 加权平均: 权重 S1=-1.5, S2=-0.5, S3=+0.5, S4=+1.5
    int sum = (s1 ? -3 : 0) + (s2 ? -1 : 0) + (s3 ? 1 : 0) + (s4 ? 3 : 0);
    return sum / on;   // -2..+2
}

// 巡线一步(非阻塞): 根据线位置差速调整左右轮, 保持沿黑线前进。
// 返回 true=仍在巡线; false=巡线结束(丢线超时/超时上限)。
bool lineFollowOnce() {
    int pos = readLinePosition();
    unsigned long now = millis();

    if (pos == 99) {  // 丢线
        if (line_lost_since_ == 0) line_lost_since_ = now;
        if (now - line_lost_since_ > LINE_LOST_MS) {
            stopMove(0);            // 停车
            return false;           // 巡线结束
        }
        // 丢线初期: 保持直行一小段, 期望重新压线
        moveForward(0);
        return true;
    }
    line_lost_since_ = 0;

    if (now - line_start_ms_ > LINE_MAX_MS) {  // 超时保护
        stopMove(0);
        return false;
    }

    if (pos == 100) {  // 路口: 直行
        moveForward(0);
        return true;
    }

    // 比例差速: 左轮 = base - Kp*pos(偏左时 pos<0 -> 左轮加速右轮减速 -> 向右修正)
    int left = LINE_BASE_SPEED - LINE_KP * pos;
    int right = LINE_BASE_SPEED + LINE_KP * pos;
    left = constrain(left, 60, 255);
    right = constrain(right, 60, 255);
    motorSpeed[0] = left;   // 前左
    motorSpeed[2] = left;   // 后左
    motorSpeed[1] = right;  // 前右
    motorSpeed[3] = right;  // 后右
    // 保持前进方向(不 delay)
    motorRun(0, "F");
    motorRun(1, "F");
    motorRun(2, "B");
    motorRun(3, "B");
    return true;
}

// 进入/退出巡线模式(由 @line-start / @line-stop 触发)
void setLineFollow(bool enable) {
    if (enable == line_following_) return;
    line_following_ = enable;
    if (enable) {
        line_start_ms_ = millis();
        line_lost_since_ = 0;
        setSpeed(LINE_BASE_SPEED);
        Serial.setTimeout(20);             // 巡线中短超时, 避免 checkLineStopCommand 阻塞
        Serial.println("@busy line-follow");   // 上报巡线开始
    } else {
        stopMove(0);
        Serial.setTimeout(1000);           // 恢复默认超时
        Serial.println("@done line-follow");   // 上报巡线结束
    }
}

// ==================================================================
// 舵机控制
// ==================================================================

// 通用: 增量控制某个舵机, 角度限制 0-180。
// @param srv   舵机对象指针
// @param angle 舵机当前角度引用(会被更新)
// @param delta 角度增量
void servoMove(Emakefun_Servo *srv, uint8_t &angle, int delta) {
    int x = angle + delta;
    Serial.println(String("orig:") + angle + " -- " + x);

    if (x >= 180) {
        x = 180;
    } else if (x <= 0) {
        x = 0;
    }

    angle = x;
    srv->writeServo(angle, 10);
    Serial.println(String("angle:") + angle);
    if (srv == servo1) reportStat();   // 只有舵机1(头部)需要上报
}

// 舵机1(头部) 增量控制
void servo1Control(int delta) {
    servoMove(servo1, servo1Angle, delta);
}

// 舵机2 增量控制 (预留)
void servo2Control(int delta) {
    servoMove(servo2, servo2Angle, delta);
}

// 设置舵机1 绝对角度(钳位 0-180)。
// @param angle 目标角度(0-180)
void setServo1(int angle) {
    if (angle < 0) {
        angle = 0;
    } else if (angle > 180) {
        angle = 180;
    }
    servo1Angle = angle;
    servo1->writeServo(angle, 10);
    Serial.println(String("angle:") + angle);
    reportStat();
}

// 状态上报: 速度/舵机1角度变化时上报最新快照(事件驱动, 不做周期轮询)。
// ESP32 侧维护最新一帧, self.uno.get_status 直接返回, 无需下位机实时应答。
// 后续扩充: 在此追加字段即可(如 ToF 距离: " d{cm}"), 解析端按可选字段处理。
void reportStat() {
    Serial.print("@stat s");
    Serial.print(speed);
    Serial.print(" v");
    Serial.println(servo1Angle);
}

// ==================================================================
// PS2 手柄控制
// ==================================================================

// 读取 PS2 手柄按键并执行对应动作; 无按键时自动停止。
void handleGamepad() {
    if (!ps2_ready) return;   // 无手柄: 跳过, 避免 read_gamepad 反复重试阻塞 loop
    ps2x.read_gamepad(false, 0);
    delay(30);

    if (ps2x.ButtonDataByte()) {
        if (ps2x.Button(PSB_PAD_RIGHT))       moveRight(10);
        if (ps2x.Button(PSB_PAD_LEFT))        moveLeft(10);
        if (ps2x.Button(PSB_PAD_UP))          moveForward(10);
        if (ps2x.Button(PSB_PAD_DOWN))        moveBackward(10);
        if (ps2x.Button(PSB_PINK))            turnLeft(10);
        if (ps2x.Button(PSB_RED))             turnRight(10);
        if (ps2x.ButtonPressed(PSB_BLUE))     updateSpeed(5);
        if (ps2x.ButtonPressed(PSB_GREEN))    updateSpeed(-5);
        if (ps2x.ButtonPressed(PSB_L1))       moveLeftForward(10);
        if (ps2x.ButtonPressed(PSB_R1))       moveRightForward(10);
        if (ps2x.ButtonPressed(PSB_L2))       moveLeftBackward(10);
        if (ps2x.ButtonPressed(PSB_R2))       moveRightBackward(10);
        if (ps2x.Button(PSB_SELECT))          servo1Control(-2);
        if (ps2x.Button(PSB_START))           servo1Control(2);
    } else {
        stopMove(10);
    }
}

// ==================================================================
// web 驾驶模式 (非阻塞遥感连续控制)
// ==================================================================

// 按当前方向对 4 个电机发短脉冲(不打印/不蜂鸣), 模拟持续运动; 方向/速度由上位机心跳维持。
void drivePulse(const char* action_str) {
    if (strcmp(action_str, "forward") == 0)       runMotors("F", "F", "B", "B", WEB_DRIVE_PULSE_MS);
    else if (strcmp(action_str, "back") == 0)        runMotors("B", "B", "F", "F", WEB_DRIVE_PULSE_MS);
    else if (strcmp(action_str, "left") == 0)        runMotors("B", "B", "B", "B", WEB_DRIVE_PULSE_MS);
    else if (strcmp(action_str, "right") == 0)       runMotors("F", "F", "F", "F", WEB_DRIVE_PULSE_MS);
    else if (strcmp(action_str, "leftmove") == 0)    runMotors("B", "F", "B", "F", WEB_DRIVE_PULSE_MS);
    else if (strcmp(action_str, "rightmove") == 0)   runMotors("F", "B", "F", "B", WEB_DRIVE_PULSE_MS);
    else if (strcmp(action_str, "leftup") == 0)      runMotors("S", "F", "B", "S", WEB_DRIVE_PULSE_MS);
    else if (strcmp(action_str, "rightup") == 0)     runMotors("F", "S", "S", "B", WEB_DRIVE_PULSE_MS);
    else if (strcmp(action_str, "leftdown") == 0)    runMotors("B", "S", "S", "F", WEB_DRIVE_PULSE_MS);
    else if (strcmp(action_str, "rightdown") == 0)   runMotors("S", "B", "F", "S", WEB_DRIVE_PULSE_MS);
    else runMotors("S", "S", "S", "S", WEB_DRIVE_PULSE_MS);   // 未知/stop -> 刹停
}

// 退出 web 驾驶: 恢复全局速度并刹停
void exitWebDrive() {
    web_drive_ = false;
    for (int i = 0; i < 4; i++) {
        motorSpeed[i] = speed;   // 恢复点动/手柄用的全局速度
    }
    drivePulse("stop");
}

// 处理 web 驾驶命令(由 serialCommand 分发, rest 指向去掉 "drive-" 后的部分, 无 '@')。
// @rest: "stop" 或 "{action}-{speed}"。
void handleDrive(const char* rest) {
    if (strcmp(rest, "stop") == 0) {
        exitWebDrive();
        return;
    }
    // 解析 @drive-{action}-{speed}
    const char* dash = strchr(rest, '-');
    if (!dash) return;   // 格式错误, 忽略
    char action[16] = {0};
    size_t alen = static_cast<size_t>(dash - rest);
    if (alen >= sizeof(action)) alen = sizeof(action) - 1;
    strncpy(action, rest, alen);
    int spd = atoi(dash + 1);
    if (spd < 70) spd = 70;
    if (spd > 255) spd = 255;
    strncpy(web_drive_action_, action, sizeof(web_drive_action_) - 1);
    web_drive_action_[sizeof(web_drive_action_) - 1] = '\0';
    web_drive_speed_ = (uint8_t)spd;
    for (int i = 0; i < 4; i++) motorSpeed[i] = web_drive_speed_;
    web_drive_ = true;
    web_drive_since_ms_ = millis();   // 心跳基准
}

// 驾驶模式每帧: 无心跳超时自动停, 否则持续脉冲驱动当前方向。
void handleWebDrive() {
    if (web_drive_ && (millis() - web_drive_since_ms_ > WEB_DRIVE_WATCHDOG_MS)) {
        exitWebDrive();   // web 断连/松手未收到停止 -> 自动停机, 保证安全
        return;
    }
    if (web_drive_) {
        drivePulse(web_drive_action_);
    }
}

// 统一从串口读取并按前缀分发命令(每次循环只读一次)。
// 旧实现 checkDriveCommand() 用 while(Serial.available()) 把缓冲里所有命令读走并
// 丢弃非 drive- 前缀的命令, 导致后续 executeCommand() 再也读不到 AI 的点动命令
// (go-*/servo-*/tj-*/speed-*/line-*)。表现为: web 上下左右可用, 但头部舵机 & AI 控制
// 全部失效。现在一次读一行并按前缀分发, 不再互相吞命令。
void serialCommand() {
    static char line[64];
    while (Serial.available()) {
        size_t len = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
        if (len == 0) continue;
        line[len] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '@') continue;
        p++;
        if (strncmp(p, "drive-", 6) == 0) {
            handleDrive(p + 6);   // web 摇杆驾驶命令
        } else {
            handleCommand(p);     // AI/舵机点动命令
        }
    }
}

// ==================================================================
// 上位机(ESP32)串口命令解析   (只认 '@' 开头, char 解析防内存碎片)
// ==================================================================

// 点动/AI 命令分发(由 serialCommand 调用, p 已去掉 '@' 前缀)。
// 处理 go-*/servo-*/tj-*/speed-*/line-* 等一次性点动命令。
// 说明: 旧 executeCommand 内自身的串口读取与 checkDriveCommand 各自 while(Serial.available())
// 抢读同一串口缓冲, 导致 checkDriveCommand 把 AI 点动命令读走丢弃, AI 控制与头部舵机全部失效。
// 读取现统一由 serialCommand 完成, 本函数只做命令分发, 不再自行读串口。
void handleCommand(char* p) {

        // AI 点动命令优先: 若还在 web 摇杆驾驶模式, 先退出, 否则 handleWebDrive() 会每轮
        // 持续脉冲 web 方向, 把 AI 动作覆盖掉(表现为"左转一直转/前进停不下来")。
        // 典型触发: 移动端拖完摇杆后切到小智 App, pointerup 不触发, 心跳未停。
        if (web_drive_) exitWebDrive();

        if (strncmp(p, "go-", 3) == 0) {
            // 格式: @go-{action}-{steps}
            char cmd_full[32];
            strncpy(cmd_full, p, sizeof(cmd_full) - 1);   // 保存完整命令(供回执带动作名)
            cmd_full[sizeof(cmd_full) - 1] = '\0';
            p += 3;
            char *dash = strchr(p, '-');     // action 与 steps 之间的 '-'
            if (!dash) {                     // 格式错误, 忽略
                return;
            }
            char action[16] = {0};
            size_t alen = dash - p;
            if (alen >= sizeof(action)) {
                alen = sizeof(action) - 1;
            }
            strncpy(action, p, alen);

            int steps = atoi(dash + 1);
            int t = steps * 100;             // 步数 -> 毫秒
            if (strcmp(action, "left") == 0 || strcmp(action, "right") == 0) {
                t = steps * 10;
            }

            Serial.print("@busy "); Serial.println(cmd_full);   // 上报动作开始(带动作名)

            if (strcmp(action, "forward") == 0) {
                NewTone(A0, 2349, 50);
                moveForward(t);
            } else if (strcmp(action, "back") == 0) {
                moveBackward(t);
            } else if (strcmp(action, "stop") == 0) {
                stopMove(t);
            } else if (strcmp(action, "left") == 0) {
                turnLeft(t);
            } else if (strcmp(action, "right") == 0) {
                turnRight(t);
            } else if (strcmp(action, "leftmove") == 0) {
                moveLeft(t);
            } else if (strcmp(action, "rightmove") == 0) {
                moveRight(t);
            } else if (strcmp(action, "leftup") == 0) {
                moveLeftForward(t);
            } else if (strcmp(action, "rightup") == 0) {
                moveRightForward(t);
            } else if (strcmp(action, "leftdown") == 0) {
                moveLeftBackward(t);
            } else if (strcmp(action, "rightdown") == 0) {
                moveRightBackward(t);
            }

            // 动作执行完毕显式刹车: 旧实现依赖 loop 中 handleGamepad() 在"无手柄按键"时
            // 每轮 stopMove(10) 的副作用来停车; ps2_ready 跳过手柄轮询后该副作用消失,
            // @go-* 执行完电机会持续转动(表现为"AI 让前进后停不下来")。
            stopMove(0);
            Serial.print("@done "); Serial.println(cmd_full);   // 上报动作完成(带动作名)

        } else if (strncmp(p, "servo-home-set", 14) == 0) {
            // 设置回正角度(0-180)并回正; 由上位机下发(可配置值存于 ESP32 NVS)
            int v = atoi(p + 14);
            if (v < 0) v = 0;
            else if (v > 180) v = 180;
            servo1Zero = v;
            setServo1(v);
        } else if (strcmp(p, "servo-home") == 0) {
            // 头部舵机回正(回到当前回正角度)
            setServo1(servo1Zero);
        } else if (strncmp(p, "servo-", 6) == 0) {
            // 格式: @servo-{degree}
            setServo1(atoi(p + 6));

        } else if (strncmp(p, "tj-", 3) == 0) {
            // 特技动作
            Serial.print("@busy "); Serial.println(p);   // 上报动作开始(带动作名, p 指向 tj-xxx)
            if (strcmp(p, "tj-yaotou") == 0) {
                // 特技: 摇头
                setServo1(180);
                delay(1000);
                setServo1(90);
                delay(1000);
                setServo1(0);
                delay(1000);
                setServo1(servo1Zero);
                delay(500);
                NewTone(A0, 3136, 500);

            } else if (strcmp(p, "tj-shandian") == 0) {
                // 特技: 闪电走位
                moveRightForward(1000);
                moveLeft(1000);
                moveRightForward(2000);
                NewTone(A0, 587, 500);

            } else if (strcmp(p, "tj-zhuanquan") == 0) {
                // 特技: 转圈
                turnLeft(1500);
                turnRight(1500);
                NewTone(A0, 587, 500);

            } else if (strcmp(p, "tj-sxzw") == 0) {
                // 特技: 蛇形走位
                moveRightForward(1500);
                moveLeftForward(1500);
                moveRightForward(1500);
                moveLeftForward(1000);
                moveRightForward(1000);
                moveLeftForward(1000);

            } else if (strcmp(p, "tj-diaotou") == 0) {
                // 特技: 调头
                turnLeft(900);
            }
            stopMove(0);   // 特技结束刹车(同上, 不再依赖 handleGamepad 的隐式停车)
            Serial.print("@done "); Serial.println(p);   // 上报动作完成

        } else if (strncmp(p, "speed-", 6) == 0) {
            // 格式: @speed-{value}
            setSpeed(atoi(p + 6));

        } else if (strcmp(p, "line-start") == 0) {
            // 进入巡线模式(沿地面黑线自动行驶)
            setLineFollow(true);

        } else if (strcmp(p, "line-stop") == 0) {
            // 退出巡线模式(急停)
            setLineFollow(false);
        }
}

// 预留: 早期简单命令处理 (未在 loop 调用)。char 解析, 只认 '@' 开头。
void aiControl() {
    static char line[64];
    if (Serial.available()) {
        size_t len = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
        if (len == 0) {
            return;
        }
        line[len] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != '@') {
            return;
        }
        p++;

        if (strcmp(p, "go-forward") == 0) {
            moveForward(500);
        } else if (strcmp(p, "go-back") == 0) {
            moveBackward(500);
        } else if (strcmp(p, "go-stop") == 0) {
            stopMove(100);
        } else if (strcmp(p, "go-left") == 0) {
            turnLeft(100);
        } else if (strcmp(p, "go-right") == 0) {
            turnRight(100);
        }
    } else {
        stopMove(10);
    }
}

// ==================================================================
// 初始化
// ==================================================================

// 初始化串口、电机驱动、速度/舵机角度、PS2 手柄和蜂鸣器。
void setup() {
    Serial.begin(9600);                 // 先以 9600 启动(米思齐遗留)
    // 注: RX 缓冲由文件顶部宏 SERIAL_RX_BUFFER_SIZE=256 控制(arduino:avr 1.8.8+ 无 setRxBufferSize API)

    mMotorDriver.begin(50);             // 电机驱动板初始化, PWM 频率 50Hz

    // 初始化速度 / 舵机角度
    for (int i = 0; i < 4; i++) {
        motorSpeed[i] = 200;
    }
    speed = 200;
    servo1Zero = 82;
    servo1Angle = 82;
    servo2Angle = 90;
    started = true;

    // 记录 PS2 手柄初始化结果: 失败(通常=没接手柄)时 handleGamepad() 直接跳过。
    // 否则 PS2X 的 read_gamepad() 会重试 5 次 + 每次 reconfig_gamepad(), 单次阻塞
    // 约 300~400ms, 且 read_delay 动态增长 -> 非 web 驾驶时每轮 loop 都被拖慢,
    // 表现为 web/AI 命令延时高且忽大忽小。
    ps2_ready = (ps2x.config_gamepad(13, 11, 10, 12, true, true) == 0);   // PS2 手柄引脚
    delay(300);

    Serial.begin(115200);               // 与上位机 ESP32 通信波特率
    servo1->writeServo(servo1Zero, 10);
    servo2->writeServo(servo2Angle, 10);
    pinMode(A0, OUTPUT);                // 蜂鸣器
    pinMode(LINE_S1, INPUT);            // 巡线传感器 S1->D7
    pinMode(LINE_S2, INPUT);            // 巡线传感器 S2->D4
    pinMode(LINE_S3, INPUT);            // 巡线传感器 S3->D3
    pinMode(LINE_S4, INPUT);            // 巡线传感器 S4->D2
}

// ==================================================================
// 主循环
// ==================================================================

// 主循环: 依次处理上位机命令和手柄控制。
void loop() {
    if (!started) return;
    if (line_following_) {
        // 巡线模式: 专注巡线, 非阻塞检查 @line-stop 急停, 手柄保留作急停
        if (!lineFollowOnce()) {
            setLineFollow(false);   // 丢线超时/超时上限 -> 自动退出并回传 @done line-follow
        }
        checkLineStopCommand();
        handleGamepad();
    } else if (web_drive_) {
        // web 遥感驾驶: 非阻塞持续控制(方向+速度), 心跳看门狗自动停
        serialCommand();         // 统一读串口并分发: drive 心跳/停止 + AI 点动(不互相吞)
        handleWebDrive();        // 持续脉冲驱动 + 超时保护
    } else {
        serialCommand();         // 统一读串口并分发: drive-* + 点动命令(修复 AI 控制被吞)
        handleGamepad();         // 解析手柄
    }
}

// 巡线模式下非阻塞读取串口, 只响应 @line-stop(急停), 其他命令忽略。
void checkLineStopCommand() {
    while (Serial.available()) {
        static char line[64];
        size_t len = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
        if (len == 0) continue;
        line[len] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '@') continue;
        p++;
        if (strcmp(p, "line-stop") == 0) {
            setLineFollow(false);
        }
    }
}
