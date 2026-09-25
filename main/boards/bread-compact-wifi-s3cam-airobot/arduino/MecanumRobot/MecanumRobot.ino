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
// 【双向回执】(Arduino -> ESP32, 供 ESP32 侧状态接口(web /uno) 查询)
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
#define LINE_BASE_SPEED  90    // 巡线基础速度(低于全局 speed 默认200, 更稳; 弯道过不去就再降)
#define LINE_KP          40    // 差速比例系数(|偏差| 1/2 时差速 ±40/±80; 太大弯道会扭来扭去)
#define LINE_MIN_SPEED   20    // 内侧轮最低速度(别一下停死, 过渡更顺)
#define LINE_PIVOT_MAG   3     // |偏差| >= 此值 -> 原地猛拐(一侧轮倒转), 转弯半径最小
#define LINE_PIVOT_SPEED 110   // 原地猛拐转速(太高容易转过头 -> 左右摆)
#define LINE_LOST_MS     800   // 连续丢线超过此毫秒数 -> 判定巡线结束(期间朝最后方向原地转着找线)
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
int line_last_pos_ = 0;             // 最后一次正常读到的偏差(负数偏左/正数偏右), 丢线时据此决定往哪边找线
char line_raw_[5] = "0000";         // 4 路探头原始状态(S1..S4), '1'=踩到黑线 -> 日志里直接看得到探头看到了什么

// 巡线动作日志: 只在动作变化时打一行(并限速 100ms), 防止抖动时刷屏。
// 文案用大白话, 串口 / 网页日志里直接看得出过程:
//   巡线: 前进 (0) / 左拐 (-1) / 右拐 (2) / 丢线直行 / 路口直行 / 停止
// 说明: 非 '@' 开头的行上位机一律忽略, 不影响控制协议。
void lineLog(char kind, int pos) {
    static char last_kind = 0;
    static int  last_pos = 999;
    static unsigned long last_ms = 0;
    unsigned long now = millis();
    if (kind == last_kind && pos == last_pos) return;   // 和上一次一样, 不重复打
    if (kind != 'S' && now - last_ms < 100) return;     // 抖动时限速(停止不受限)
    last_kind = kind;
    last_pos = pos;
    last_ms = now;
    Serial.print("巡线: ");
    switch (kind) {
        case 'F': Serial.print("前进");     break;
        case 'L': Serial.print("左拐");     break;
        case 'R': Serial.print("右拐");     break;
        case 'X': Serial.print("丢线直行"); break;
        case 'l': Serial.print("丢线往左找"); break;
        case 'r': Serial.print("丢线往右找"); break;
        case 'P': Serial.print("原地左拐"); break;
        case 'Q': Serial.print("原地右拐"); break;
        case '+': Serial.print("路口直行"); break;
        case 'S': Serial.print("停止");     break;
    }
    if (pos >= -3 && pos <= 3) {
        Serial.print(" (");
        Serial.print(pos);
        Serial.print(')');
    }
    Serial.print(" 探头[");
    Serial.print(line_raw_);
    Serial.println(']');
}

// 读取 4 路传感器并计算线位置(-2..+2, 负数偏左, 正数偏右)。
// 返回 99 = 全灭(丢线/线断), 100 = 全亮(十字路口/粗线, 按直行处理)。
// 若传感器方向反了(S1 在右), 把返回值取反即可。
int readLinePosition() {
    bool s1 = digitalRead(LINE_S1) == (LINE_ACTIVE ? HIGH : LOW);
    bool s2 = digitalRead(LINE_S2) == (LINE_ACTIVE ? HIGH : LOW);
    bool s3 = digitalRead(LINE_S3) == (LINE_ACTIVE ? HIGH : LOW);
    bool s4 = digitalRead(LINE_S4) == (LINE_ACTIVE ? HIGH : LOW);
    int on = s1 + s2 + s3 + s4;
    line_raw_[0] = s1 ? '1' : '0';   // 日志用: 探头原始状态(S1..S4)
    line_raw_[1] = s2 ? '1' : '0';
    line_raw_[2] = s3 ? '1' : '0';
    line_raw_[3] = s4 ? '1' : '0';
    if (on == 0) return 99;   // 全灭: 丢线
    if (on == 4) return 100;  // 全亮: 路口/粗线, 按直行
    // 加权平均: 权重 S1=-1.5, S2=-0.5, S3=+0.5, S4=+1.5
    int sum = (s1 ? -3 : 0) + (s2 ? -1 : 0) + (s3 ? 1 : 0) + (s4 ? 3 : 0);
    return sum / on;   // -2..+2
}

// 丢线找线/原地猛拐: 一侧两轮倒转 + 另一侧两轮正转 -> 原地转(转弯半径最小)。
// 方向复用本工程已验证的原地左转/右转定义(turnLeft/turnRight, 网页端 left/right 同此):
//   左转 = 四轮 "B", 右转 = 四轮 "F"
void pivotTurn(char dir, int spd) {          // 参数名避开全局 speed, 防止误用
    for (int i = 0; i < 4; i++) {
        motorSpeed[i] = spd;
    }
    if (dir == 'L') {
        runMotors("B", "B", "B", "B", 0);   // 同 turnLeft
    } else {
        runMotors("F", "F", "F", "F", 0);   // 同 turnRight
    }
}

// 巡线一步(非阻塞): 根据线位置差速调整左右轮, 保持沿黑线前进。
// 返回 true=仍在巡线; false=巡线结束(丢线超时/超时上限)。
bool lineFollowOnce() {
    int pos = readLinePosition();
    unsigned long now = millis();

    if (pos == 99) {  // 丢线
        if (line_lost_since_ == 0) line_lost_since_ = now;
        if (now - line_lost_since_ > LINE_LOST_MS) {
            lineLog('S', 99);       // 日志: 停止
            stopMove(0);            // 停车
            return false;           // 巡线结束
        }
        // 丢线初期: 按"最后看到线的方向"原地猛拐找线。
        // 千万不能改成直行: 直线接弯道时线一跑出探头范围, 直行就会让车直接冲出弯道。
        if (line_last_pos_ < 0) {              // 线最后在左边 -> 原地往左转着找
            pivotTurn('L', LINE_PIVOT_SPEED);
            lineLog('l', 99);                  // 日志: 丢线往左找
        } else if (line_last_pos_ > 0) {       // 线最后在右边 -> 原地往右转着找
            pivotTurn('R', LINE_PIVOT_SPEED);
            lineLog('r', 99);                  // 日志: 丢线往右找
        } else {                               // 从没偏过(如起步就在线外) -> 直行找
            for (int i = 0; i < 4; i++) {
                motorSpeed[i] = LINE_BASE_SPEED;
            }
            runMotors("F", "F", "B", "B", 0);
            lineLog('X', 99);                  // 日志: 丢线直行
        }
        return true;
    }
    line_lost_since_ = 0;

    if (now - line_start_ms_ > LINE_MAX_MS) {  // 超时保护
        lineLog('S', 99);       // 日志: 停止
        stopMove(0);
        return false;
    }

    if (pos == 100) {  // 路口: 直行
        for (int i = 0; i < 4; i++) {
            motorSpeed[i] = LINE_BASE_SPEED;   // 拉平, 确保真的是直行
        }
        runMotors("F", "F", "B", "B", 0);
        lineLog('+', 100);      // 日志: 路口直行
        return true;
    }

    // 转向: 按偏差大小平滑加/减速(线性), 偏差最大才原地猛拐。
    // 符号: 线偏左(pos<0) -> 左轮慢/右轮快 -> 车往左拐(往线那边拐), 与对照表一致。
    // 注: 之前是"偏一点给 60 差速, 偏得多直接给 255/0"的开兕式控制, 力度跳变太大
    //     -> 车在弯道左右扭来扭去。现在按偏差平滑过渡。
    int mag = pos < 0 ? -pos : pos;
    if (mag >= LINE_PIVOT_MAG) {             // 偏差最大: 原地猛拐(一侧轮倒转)
        pivotTurn(pos < 0 ? 'L' : 'R', LINE_PIVOT_SPEED);
        lineLog(pos < 0 ? 'P' : 'Q', pos);   // 日志: 原地左拐 / 原地右拐
        return true;
    }
    int outer = LINE_BASE_SPEED + LINE_KP * mag;
    int inner = LINE_BASE_SPEED - LINE_KP * mag;
    if (inner < LINE_MIN_SPEED) inner = LINE_MIN_SPEED;   // 内侧留一点力, 不要一下停死
    if (outer > 255) outer = 255;
    int left  = (pos < 0) ? inner : outer;   // 线在左 -> 左轮是内侧(慢)
    int right = (pos < 0) ? outer : inner;
    motorSpeed[0] = left;   // 前左
    motorSpeed[2] = left;   // 后左
    motorSpeed[1] = right;  // 前右
    motorSpeed[3] = right;  // 后右
    // 保持前进方向(不 delay)
    motorRun(0, "F");
    motorRun(1, "F");
    motorRun(2, "B");
    motorRun(3, "B");
    line_last_pos_ = pos;   // 记住最后看到的偏差方向, 丢线时按它决定往哪边找
    lineLog(pos < 0 ? 'L' : (pos > 0 ? 'R' : 'F'), pos);   // 日志: 左拐 / 右拐 / 前进
    return true;
}

// 进入/退出巡线模式(由 @line-start / @line-stop 触发)
void setLineFollow(bool enable) {
    if (enable == line_following_) return;
    line_following_ = enable;
    if (enable) {
        line_start_ms_ = millis();
        line_lost_since_ = 0;
        line_last_pos_ = 0;                // 清掉上次巡线残留的偏差方向
        // 注意: 这里**不能**调 setSpeed(LINE_BASE_SPEED)。
        // setSpeed() 改的是全局 speed(=手柄/网页驾驶/点动用的速度, 也是 @stat 报给网页的值),
        // 而且退出时不恢复 -> 巡线一次后手柄就莫名变很慢。
        // 巡线本身的左右轮速由 lineFollowOnce() 每轮显式设置, 不需要改全局 speed。
        Serial.setTimeout(20);             // 巡线中短超时, 避免 checkLineStopCommand 阻塞
        Serial.println("@busy line-follow");   // 上报巡线开始
    } else {
        stopMove(0);
        // 巡线过程中 motorSpeed[] 被改成了左右不同(差速/原地转), 退出时必须拉回左右对称,
        // 否则之后用手柄 / AI 点动走直线会跑偏 —— “手柄控制出问题”的根因。
        for (int i = 0; i < 4; i++) {
            motorSpeed[i] = speed;         // speed = 用户设定的速度(巡线不再修改它)
        }
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
// ESP32 侧维护最新一帧, 状态接口直接返回, 无需下位机实时应答。
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

// PS2 手柄: 初始化失败后定期重试。
// 背景(板级 README 踩坑 6): ps2_ready 原来是"一次性判定", 开机时手柄没插好/没开机 ->
// config_gamepad() 失败 -> 手柄永久不可用(旧实现每轮重试能自愈, 但会把 loop 拖到 300ms/轮,
// 网页遥控延迟极大, 所以才加了一次性守卫)。
// 折中方案: 失败后每 PS2_RETRY_MS 重试一次, 且只在"没在 web 驾驶、没有待处理串口命令"
// 时做 -> 那次 ~100~250ms 的阻塞只在空闲时刻发生, 不影响摇杆遥控/AI 指令的延迟。
#define PS2_RETRY_MS 3000
unsigned long ps2_retry_ms_ = 0;

// 连接 PS2 手柄(引脚 13/11/10/12), 返回是否连上。
bool ps2Connect() {
    ps2_ready = (ps2x.config_gamepad(13, 11, 10, 12, true, true) == 0);
    return ps2_ready;
}

// 上报手柄是否连上(网页「下位机指令记录」里能看到):
//   @stat s200 v82 ps2:1   (1=已连上, 0=没连上/正在重试)
// 末尾多一个字段对 ESP32 解析无影响(只取 s/v 两个字段), 也不影响前端(只用 servo)。
void reportPs2() {
    Serial.print("@stat s");
    Serial.print(speed);
    Serial.print(" v");
    Serial.print(servo1Angle);
    Serial.print(" ps2:");
    Serial.println(ps2_ready ? 1 : 0);
}

// 空闲时重试连接手柄(见上面说明)。手柄一旦连上, 本函数直接返回, 不再有任何开销。
void ps2RetryIfNeeded() {
    if (ps2_ready) return;
    if (web_drive_) return;          // web 摇杆驾驶中: 不重试, 保住遥控延迟
    if (Serial.available()) return;  // 有命令待处理: 先让命令走
    unsigned long now = millis();
    if (now - ps2_retry_ms_ < PS2_RETRY_MS) return;
    ps2_retry_ms_ = now;
    if (ps2Connect()) {
        reportPs2();                 // 只在"连上了"这一刻上报, 不刷屏
    }
}

// 读取 PS2 手柄按键并执行对应动作。
// @param idle_brake 无按键时是否刹停: true(默认)=手柄单用时“松手即停”的实现;
//   巡线分支里传 false —— 巡线期间电机由巡线控制, 若每轮再 BRAKE 10ms(外加 delay(30)),
//   巡线速度会被压到 ~1/4 而且一顿一顿(实测: 手柄后接入能被识别后, 巡线突然变得特别慢)。
// @param wait 是否 delay(30) 节流按键重复率: 巡线中关掉, 保住巡线控制频率。
void handleGamepad(bool idle_brake = true, bool wait = true) {
    if (!ps2_ready) return;   // 无手柄: 跳过, 避免 read_gamepad 反复重试阻塞 loop
    ps2x.read_gamepad(false, 0);
    if (wait) delay(30);

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
    } else if (idle_brake) {
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
    ps2Connect();                       // PS2 手柄引脚 13/11/10/12 (失败不致命: loop 里会定期重试)
    delay(300);

    Serial.begin(115200);               // 与上位机 ESP32 通信波特率
    ps2_retry_ms_ = millis();           // 手柄重试计时基准
    reportPs2();                        // 上报手柄连接状态(网页「下位机指令记录」可见)
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
        // 手柄只读按键, 不刹车/不 delay: 否则每轮 BRAKE 10ms 会把巡线速度压到 ~1/4
        handleGamepad(false, false);
    } else if (web_drive_) {
        // web 遥感驾驶: 非阻塞持续控制(方向+速度), 心跳看门狗自动停
        serialCommand();         // 统一读串口并分发: drive 心跳/停止 + AI 点动(不互相吞)
        handleWebDrive();        // 持续脉冲驱动 + 超时保护
    } else {
        serialCommand();         // 统一读串口并分发: drive-* + 点动命令(修复 AI 控制被吞)
        ps2RetryIfNeeded();      // 没接上手柄时在空闲里定期重试(避免开机没插好 -> 手柄永久失效)
        handleGamepad();         // 解析手柄
    }
}

// 巡线模式下非阻塞读取串口。
// 旧实现只认 @line-stop, 其它命令(包括网页摇杆心跳 @drive-* 和网页「停止」按钮的
// @drive-stop)一律读走丢弃 -> 巡线中网页那排控件(摇杆/停止)推了没反应。
// 手柄在巡线时本来就可以干预(loop 里调了 handleGamepad), 网页端却不行, 这里对齐:
//   @line-stop / @drive-stop -> 立即退出巡线
//   @drive-*                  -> 人工意图优先, 退出巡线并交给摇杆驾驶
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
        if (strcmp(p, "line-stop") == 0 || strcmp(p, "drive-stop") == 0) {
            setLineFollow(false);
        } else if (strncmp(p, "drive-", 6) == 0) {
            setLineFollow(false);
            handleDrive(p + 6);
        }
    }
}
