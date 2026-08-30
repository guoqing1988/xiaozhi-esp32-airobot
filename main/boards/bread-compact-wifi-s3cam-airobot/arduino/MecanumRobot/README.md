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

## 与 ESP32 连接

| Arduino ←→ ESP32 | 说明 |
|---|---|
| **RX ← GPIO43** | ESP32 UART0 TX 发指令 |
| **TX → GPIO44** | ESP32 UART0 RX（**双向回执**，需接线）|
| GND | 共地 |
| 波特率 | **115200** |

> 命令以 **`@`** 开头（如 `@go-forward-3\n`），其余行（如 ESP32 日志）会被 Arduino 忽略。

## 依赖库

需安装到 Arduino 库目录 `~/Documents/Arduino/libraries`：

| 库 | 头文件 | 来源 |
|----|--------|------|
| `MotorDriverBoard`（Emakefun）| `Emakefun_MotorDriver.h` | https://github.com/emakefun/MotorDriverBoard |
| `NewTone` | `NewTone.h` | https://bitbucket.org/teckel12/arduino-new-tone（作者原版）|
| `PS2X_lib` | `PS2X_lib.h` | https://github.com/madsci1016/Arduino-PS2X |

> `PS2X_lib` 源文件在 `Arduino-PS2X/PS2X_lib/` 子目录里，需**把该子目录提升为顶层库目录** `~/Documents/Arduino/libraries/PS2X_lib` 才能被 arduino-cli 索引。

## 编译 & 烧录

### 0. 先确认端口

```bash
arduino-cli board list
```

输出示例：

```
Port  Protocol  Type             Board Name  FQBN  Core
COM3  serial    Serial Port (USB) Unknown
COM1  serial    Serial Port       Unknown
```

- Arduino 板的端口 = 标着 **`Serial Port (USB)`** 的 CH340 端口（示例为 **COM3**）；
- **`COM1` 是主板通信口**，不要选它；
- 换过 USB 口 / 重插后端口号可能变化，每次烧录前先跑一次确认。

### 方式一：一键脚本（推荐，Windows）

sketch 目录下双击或命令行运行：

```bat
build_arduino.bat              :: 仅编译
build_arduino.bat COM5         :: 编译 + 烧录到 COM5
```

> 脚本已内置 `-v`（详细日志）；烧录前用 `arduino-cli board list` 确认实际端口（**别用 COM1**，那是主板通信口）。

### 方式二：arduino-cli

**一条命令（编译 + 烧录）：**

```bash
# Windows 端口用 COMx，Linux/macOS 用 /dev/cu.usbmodemXXXX
arduino-cli compile -v --upload -p COM5 --fqbn arduino:avr:uno \
  main/boards/bread-compact-wifi-s3cam-airobot/arduino/MecanumRobot
```

**分开执行（仅编译 / 仅烧录）：**

```bash
# 仅编译（Arduino UNO，-v 查看详细日志）
arduino-cli compile -v --fqbn arduino:avr:uno main/boards/bread-compact-wifi-s3cam-airobot/arduino/MecanumRobot

# 仅烧录（端口按实际修改，-v 查看烧录详细过程）
arduino-cli upload -v -p COM5 --fqbn arduino:avr:uno main/boards/bread-compact-wifi-s3cam-airobot/arduino/MecanumRobot
```

> 不加 `-v` 时只显示概要结果（编译成功/失败、Flash/RAM 占用）；加 `-v` 可看完整中间过程（库解析/编译命令/烧录握手），排障时建议保留。

### 方式三：Arduino IDE

打开 `MecanumRobot.ino` → 库管理器安装 3 个库 → 选 `Arduino UNO` 板和端口 → 编译烧录。

### RX 缓冲（SERIAL_RX_BUFFER_SIZE）

`arduino:avr 1.8.8+` 无 `setRxBufferSize` API，RX 缓冲由编译期宏 `SERIAL_RX_BUFFER_SIZE` 控制（默认 64B≈4-5 条指令，AI 长指令序列会溢出丢指令）。

- **本机已配置**：核心目录 `platform.local.txt`（内容 `compiler.cpp.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256`），上述命令自动生效，无需额外参数。
- **换电脑 / 重装核心后**：在核心目录 `D:\Arduino15\packages\arduino\hardware\avr\1.8.8\` 重建 `platform.local.txt`，或编译时加：
  ```bash
  arduino-cli compile -v --build-property "compiler.cpp.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" ...
  ```

## 上位机串口指令（ESP32 下发，均以 `@` 开头）

| 指令 | 效果 |
|------|------|
| `@go-{action}-{steps}` | 动作：forward/back/left/right/leftmove/rightmove/leftup/rightup/leftdown/rightdown |
| `@servo-{degree}` | 设置舵机1 角度(0-180) |
| `@speed-{value}` | 设置电机速度(70-255) |
| `@tj-yaotou` / `@tj-shandian` / `@tj-zhuanquan` / `@tj-sxzw` / `@tj-diaotou` | 特技动作 |
| `@line-start` / `@line-stop` | 巡线模式（沿地面黑线自动行驶）开始/停止 |

**双向回执（Arduino → ESP32）**：耗时动作（`go-*` / `tj-*` / 巡线）开始执行时回传 `@busy`，执行完毕回传 `@done`；ESP32 的 UART0 RX 解析任务维护状态，AI 可通过 `self.uno.get_status` 查询。

> 巡线功能参数（基础速度 / 转向强度 / 丢线判定等）与传感器接线详见板级 README「巡线（4 路循迹传感器）」一节。

## PS2 手柄键位

| 按键 | 功能 |
|------|------|
| 十字键 ↑↓←→ | 前进/后退/左移/右移 |
| PINK / RED | 原地左转 / 原地右转 |
| GREEN / BLUE（单击）| 降速 / 加速 |
| L1 / R1 / L2 / R2 | 左前斜 / 右前斜 / 左后斜 / 右后斜 |
| SELECT / START | 舵机1 微调(-2 / +2) |

## 注意事项

- **看日志 / 烧录 ESP32 时请先断开 Arduino 与 ESP32 的接线**（ESP32 板载 USB-UART 走 GPIO43/44，与 Arduino 共用会干扰）。
- 程序只认 **`@` 开头** 且带 `\n` 结尾的行，其余（如 ESP32 日志）一律忽略。
- 解析用**固定 `char` 缓冲**（不用 `String`），适合 UNO 2KB SRAM，抗内存碎片。
- **烧录失败排查**：Windows + CH340 的 UNO 克隆板上传报 `cannot set com-state` 是 CH340 新版驱动（3.8+）在 Win11 的已知 bug，降级驱动到 3.5.2019.1 即可——详见板级 README「踩坑记录：CH340 新驱动导致 Arduino 上传失败」。
