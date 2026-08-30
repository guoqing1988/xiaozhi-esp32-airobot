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
//
// 【PS2 手柄键位】
//   十字键: 移动      PINK/RED: 左右转      GREEN/BLUE(单击): 减速/加速
//   L1/R1/L2/R2: 斜移    SELECT/START: 舵机微调
// ==================================================================

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

// ---------------- 全局状态 ----------------
uint8_t motorSpeed[4] = {200, 200, 200, 200};   // 4 个电机速度
uint8_t speed      = 200;                       // 当前(全局)速度
uint8_t servo1Zero = 82;                        // 舵机1 回正角度 (大于=左转, 小于=右转)
uint8_t servo1Angle = 82;                       // 舵机1 当前角度
uint8_t servo2Angle = 90;                       // 舵机2 当前角度
bool     started = true;                        // 主循环开关

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
}

// ==================================================================
// PS2 手柄控制
// ==================================================================

// 读取 PS2 手柄按键并执行对应动作; 无按键时自动停止。
void handleGamepad() {
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
// 上位机(ESP32)串口命令解析   (只认 '@' 开头, char 解析防内存碎片)
// ==================================================================

// 解析上位机(ESP32)下发的串口命令。只认以 '@' 开头且带换行的行,
// 其余(如日志乱码)全部忽略。用固定 char 缓冲解析, 避免 String 动态分配。
void executeCommand() {
    static char line[64];                    // 复用缓冲, 不反复分配内存
    if (Serial.available()) {
        size_t len = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
        if (len == 0) {
            return;
        }
        line[len] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') {    // 去前导空白
            p++;
        }
        if (*p != '@') {                     // 只认 '@' 前缀
            return;
        }
        p++;

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

            Serial.print("@done "); Serial.println(cmd_full);   // 上报动作完成(带动作名)

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
            Serial.print("@done "); Serial.println(p);   // 上报动作完成

        } else if (strncmp(p, "speed-", 6) == 0) {
            // 格式: @speed-{value}
            setSpeed(atoi(p + 6));
        }
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
    Serial.setRxBufferSize(256);        // 加大 RX 缓冲(默认 64B≈4-5 条指令, 长序列会溢出丢指令)
    Serial.begin(9600);                 // 先以 9600 启动(米思齐遗留)

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

    ps2x.config_gamepad(13, 11, 10, 12, true, true);   // PS2 手柄引脚
    delay(300);

    Serial.begin(115200);               // 与上位机 ESP32 通信波特率
    servo1->writeServo(servo1Zero, 10);
    servo2->writeServo(servo2Angle, 10);
    pinMode(A0, OUTPUT);                // 蜂鸣器
}

// ==================================================================
// 主循环
// ==================================================================

// 主循环: 依次处理上位机命令和手柄控制。
void loop() {
    if (started) {
        executeCommand();    // 解析上位机命令
        handleGamepad();     // 解析手柄
    }
}
