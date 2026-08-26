# MecanumRobot —— Arduino 下位机（麦克纳姆轮机器人）

Arduino 下位机固件，由上位机 ESP32（本板 `bread-compact-wifi-s3cam-airobot`）通过串口控制。

> 该 `.ino` 由 **Arduino IDE / arduino-cli** 编译，烧录到 **Arduino 板**，**不参与** ESP-IDF 固件编译。

## 硬件
- **Arduino 板**：UNO（2KB SRAM）
- **Emakefun 电机驱动板**（I2C 地址 `0x60`）：
  - 4 个直流电机（麦克纳姆轮）：`motors[0~3]`（前左/前右/后左/后右）
  - 2 个舵机：`servo1`（头部）、`servo2`
- **PS2 手柄**：`config_gamepad(13, 11, 10, 12)`
- **蜂鸣器**：A0（`NewTone`）

## 依赖库
需安装到 Arduino 库目录 `~/Documents/Arduino/libraries`：

| 库 | 头文件 | 来源 |
|----|--------|------|
| `MotorDriverBoard`（Emakefun）| `Emakefun_MotorDriver.h` | https://github.com/emakefun/MotorDriverBoard |
| `NewTone` | `NewTone.h` | https://bitbucket.org/teckel12/arduino-new-tone（作者原版）|
| `PS2X_lib` | `PS2X_lib.h` | https://github.com/madsci1016/Arduino-PS2X |

> `PS2X_lib` 源文件在 `Arduino-PS2X/PS2X_lib/` 子目录里，需**把该子目录提升为顶层库目录** `~/Documents/Arduino/libraries/PS2X_lib` 才能被 arduino-cli 索引。

## 编译 & 烧录（arduino-cli）

```bash
# 编译（Arduino UNO）
arduino-cli compile --fqbn arduino:avr:uno <本目录>/MecanumRobot

# 烧录（端口按实际修改）
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn arduino:avr:uno <本目录>/MecanumRobot
```

或用 Arduino IDE：打开 `MecanumRobot.ino` → 库管理器安装 3 个库 → 选 `Arduino UNO` 板和端口 → 编译烧录。

## 上位机串口指令（ESP32 下发，均以 `@` 开头）
| 指令 | 效果 |
|------|------|
| `@go-{action}-{steps}` | 动作：forward/back/left/right/leftmove/rightmove/leftup/rightup/leftdown/rightdown |
| `@servo-{degree}` | 设置舵机1 角度(0-180) |
| `@speed-{value}` | 设置电机速度(70-255) |
| `@tj-yaotou` / `@tj-shandian` / `@tj-zhuanquan` / `@tj-sxzw` / `@tj-diaotou` | 特技动作 |

## PS2 手柄键位
| 按键 | 功能 |
|------|------|
| 十字键 ↑↓←→ | 前进/后退/左移/右移 |
| PINK / RED | 原地左转 / 原地右转 |
| GREEN / BLUE（单击）| 降速 / 加速 |
| L1 / R1 / L2 / R2 | 左前斜 / 右前斜 / 左后斜 / 右后斜 |
| SELECT / START | 舵机1 微调(-2 / +2) |

## 注意
- **看日志 / 烧录 ESP32 时请先断开 Arduino 与 ESP32 的接线**（ESP32 板载 USB-UART 走 GPIO43/44，与 Arduino 共用会干扰）。
- 程序只认 **`@` 开头** 且带 `\n` 结尾的行，其余（如 ESP32 日志）一律忽略。
- 解析用**固定 `char` 缓冲**（不用 `String`），适合 UNO 2KB SRAM，抗内存碎片。
