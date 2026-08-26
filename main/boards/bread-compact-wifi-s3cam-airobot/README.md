# Bread Compact Wi-Fi S3Cam AI Robot (面包板)

本板由 `bread-compact-wifi-s3cam` 克隆而来，作为 AI 机器人 DIY 基础版本。

- 主控：**FREENOVE ESP32-S3 WROOM**（板载 TF 卡槽 + OV2640 摄像头 + RGB LED）。
- 音频：数字麦克风 **INMP441** + 数字功放 **MAX98357A** + 喇叭。
- 显示：SPI LCD（ST7789 240x320）。
- 网络：Wi-Fi。

> ⚠️ 本板为 DIY 方案，**引脚与官方 ESP32-S3-CAM 面包板教程不同**，请严格按下表接线。

## 硬件清单

| 硬件 | 型号 | 用途 |
|------|------|------|
| 主控板 | FREENOVE ESP32-S3 WROOM | 运行固件、音视频、网络 |
| 数字麦克风 | INMP441 | 音频输入 |
| 数字功放 | MAX98357A | 音频输出驱动 |
| 喇叭 | 8Ω 2~3W | 扬声器 |
| LCD 屏 | ST7789 240x320 (SPI) | 显示 |
| 摄像头 | OV2640 | 视觉 |
| TF 卡 | MicroSD（板载槽） | 本地歌曲存储 |

## 开发板信息（ESP32-S3-CAM）

### 板载资源

| 项目 | 信息 |
|------|------|
| 主控 | ESP32-S3 双核 LX7 @240MHz |
| 内存 | 512KB SRAM + 8MB PSRAM |
| Flash | 16MB |
| 板载 TF 卡槽 | 有（SDMMC 1-bit） |
| 板载摄像头 | OV2640（24-Pin FPC 排线直连）|
| 板载 RGB LED | GPIO48 |
| USB | GPIO19/20（本板改作 LCD SPI）|
| 串口 | GPIO43(TX) / 44(RX) |

### GPIO 引脚分配（本板使用）

| 功能 | GPIO |
|------|------|
| 摄像头 D0~D7 | 11, 9, 8, 10, 12, 18, 17, 16 |
| 摄像头 XCLK / PCLK / VSYNC / HREF | 15, 13, 6, 7 |
| 摄像头 I2C SIOD / SIOC | 4, 5 |
| 麦克风 WS / SCK / SD | 1, 2, 42 |
| 功放 DIN / BCLK / LRC | 3, 14, 46 |
| LCD SCLK / MOSI / RST / CS / DC | 19, 20, 21, 45, 47 |
| LCD 背光 | 3.3V 常亮（无 GPIO）|
| TF 卡 SDMMC | 38(CMD) / 39(CLK) / 40(D0) |
| RGB LED / BOOT | 48 / 0 |

### 不可用 / 受限引脚

| GPIO | 原因 |
|------|------|
| 26~32 | 内部 flash/PSRAM |
| 35, 36, 37 | 8MB PSRAM |
| 43, 44 | UART0 TX/RX（串口调试）|
| 19, 20 | USB D+/D-（本板改作 LCD SPI）|

## 接线表（重要，按此接线）

### 麦克风 INMP441

| INMP441 引脚 | ←→ 开发板 | 说明 |
|---|---|---|
| VDD | 3V3 | 供电 |
| GND | GND | 接地，**并短接 L/R 至 GND** |
| WS | GPIO1 | 数据选择 |
| SCK | GPIO2 | 数据时钟 |
| SD | GPIO42 | 数据输出 |

### 功放 MAX98357A

| MAX98357A 引脚 | ←→ 开发板 | 说明 |
|---|---|---|
| Vin | 3.3V | 供电，**并短接 SD 至 Vin**（常开）|
| GND | GND | 接地 |
| **DIN** | **GPIO3** | 数字信号（**已改：原 39**）|
| **BCLK** | **GPIO14** | 位时钟（**已改：原 40**）|
| **LRC** | **GPIO46** | 左右时钟（**已改：原 41**）|
| 音频+ / 音频- | 喇叭正极 / 负极 | 输出 |

### LCD 屏 ST7789（SPI）

| LCD 引脚 | ←→ 开发板 | 说明 |
|---|---|---|
| VCC / GND | 3V3 / GND | 供电 |
| SCLK | GPIO19 | SPI 时钟 |
| MOSI | GPIO20 | SPI 数据 |
| RST | GPIO21 | 复位 |
| CS | GPIO45 | 片选 |
| DC | GPIO47 | 数据/命令 |
| **BL（背光）** | **直接接 3.3V（常亮）** | **已改：不再接 GPIO38** |

### 摄像头 OV2640

- 通过 **24-Pin FPC 排线** 直连开发板，无需手动接线。

### TF 卡（板载 MicroSD 槽）

| 功能 | 开发板引脚 | 说明 |
|---|---|---|
| SD_CMD | GPIO38 | 板载 SDMMC |
| SD_CLK | GPIO39 | 板载 SDMMC |
| SD_D0 | GPIO40 | 板载 SDMMC |

> 板上已集成，无需外接；使用前把歌曲放到 `TF卡:/sdcard/music/` 目录。

### 其它

| 功能 | 开发板引脚 |
|---|---|
| 板载 RGB LED | GPIO48 |
| BOOT 按钮 | GPIO0 |

## 与官方教程的差异（改动点）

1. **功放引脚已换**：DIN/BCLK/LRC 由 `39/40/41` → **`3/14/46`**，原脚让给板载 TF 卡。
2. **LCD 背光改常亮**：BL 不再接 GPIO38，改为直接接 **3.3V 常亮**（无法软件调暗背光）。
3. **TF 卡**：使用板载 SDMMC 引脚 `38/39/40`。

> 麦克风、LCD 主接口、摄像头、LED、BOOT 引脚均与原方案一致。

## 编译配置命令

**① 加载 ESP-IDF v6.0.2 环境：**

```bash
source ~/esp/v6.0.2/esp-idf/export.sh
idf.py --version      # 应显示 ESP-IDF v6.0.2
```

**② 配置编译目标为 ESP32S3：**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig：**

```bash
idf.py menuconfig
```

**选择板子：**

```
Xiaozhi Assistant -> Board Type -> Bread Compact Wi-Fi + LCD + Camera AI Robot (面包板)
```

**编译烧入：**

```bash
idf.py build flash
```

> **查看编译日志**：`idf.py build` / `scripts/build.py` 都会把编译进度**打印到终端**，报错也会完整输出。若想留存日志，用
> ```bash
> idf.py build 2>&1 | tee build.log
> ```
> 编译产物在 `build/` 目录：`build/xiaozhi.bin`、`build/merged-binary.bin`。
> 烧录后看**运行日志**（monitor，端口按实际修改）：
> ```bash
> idf.py -p /dev/cu.usbserial-XXXX flash monitor
> ```

或使用构建脚本（自动配置板子/屏幕，推荐）：

```bash
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
```

> 区别：`idf.py build` **不读取** config.json 的 `sdkconfig_append`（如屏幕类型、console 配置），需通过 `menuconfig` 手动设置；`scripts/build.py` 会读取 config.json 并自动配置。

## 功能：TF 卡本地歌曲播放（AI 控制）

本板在面包板基础上新增「TF 卡本地歌曲播放」能力，让 AI 语音助手直接播放 TF 卡上的本地音乐（MP3），替代官方云端曲库中数量有限的歌曲。

### TF 卡准备
- 把歌曲（**MP3** 格式）放入 TF 卡的 `music` 目录：`/sdcard/music/*.mp3`。
- 播放器启动时会扫描该目录，自动列出歌名。

### AI 语音指令（服务端通过 MCP 工具自动调用）

| 你对助手说的话 | 触发工具 | 效果 |
|--------|---------|------|
| 「播放一首歌 / 随机播放」 | `self.music.play_random` | 随机播放一首 |
| 「播放《歌名》」 | `self.music.play` | 播放指定歌曲（参数 `name`）|
| 「暂停播放」 | `self.music.pause` | 暂停 |
| 「继续播放」 | `self.music.resume` | 继续 |
| 「停止播放」 | `self.music.stop` | 停止 |
| 「有什么歌」 | `self.music.list` | 列出 TF 卡歌曲 |

- **默认连播**：一首播完自动播下一首。
- **可被打断**：播放中被唤醒/说话会打断本地播放，恢复语音交互。

### 实现说明
- 本地播放复用项目官方的 `esp_audio_codec` 组件 MP3 解码器（`esp_mp3_dec`）+ `AudioService` 播放链路，**无自定义解析逻辑**。
- 播放器源码位于本板目录：`local_music_player.h` / `local_music_player.cc`（由 CMake `file(GLOB)` 自动编译）。
- `AudioService` 仅新增一个 `PushLocalPcm()` 注入接口（最小、纯新增）。

## Arduino 下位机（Mecanum 机器人）

本板可选配一个 **Arduino 下位机**（麦克纳姆轮机器人），由 ESP32 通过串口控制。

### 代码位置
```
main/boards/bread-compact-wifi-s3cam-airobot/arduino/MecanumRobot/MecanumRobot.ino
```
> 该 `.ino` 是 **Arduino 代码**，由 Arduino IDE 编译烧录到 Arduino 板，**不参与 ESP-IDF 固件编译**。

### 硬件
- Emakefun 电机驱动板（I2C 0x60）：4 个直流电机（麦克纳姆轮）+ **2 个舵机**（servo1 头部 / servo2）
- PS2 手柄（`config_gamepad(13,11,10,12)`）
- 蜂鸣器（A0，NewTone）

### 与 ESP32 连接
| Arduino ←→ ESP32 | 说明 |
|---|---|
| **RX ← GPIO43** | ESP32 UART0 TX 发指令 |
| **TX → GPIO44** | ESP32 UART0 RX（可选回传）|
| GND | 共地 |
| 波特率 | **115200** |

> 命令以 **`@`** 开头（如 `@go-forward-3\n`），其余行（如 ESP32 日志）会被 Arduino 忽略。

### 上位机串口指令（对应 ESP32 的 `self.uno.*` MCP 工具，均以 `@` 开头）
| 指令 | 效果 |
|------|------|
| `@go-{action}-{steps}` | 动作：forward/back/left/right/leftmove/rightmove/leftup/rightup/leftdown/rightdown |
| `@servo-{degree}` | 设置舵机1 角度(0-180) |
| `@speed-{value}` | 设置电机速度(70-255) |
| `@tj-yaotou` / `@tj-shandian` / `@tj-zhuanquan` / `@tj-sxzw` / `@tj-diaotou` | 特技动作 |

### PS2 手柄键位
| 按键 | 功能 |
|------|------|
| 十字键 ↑↓←→ | 前进/后退/左移/右移 |
| PINK / RED | 原地左转 / 原地右转 |
| GREEN / BLUE（单击）| 降速 / 加速 |
| L1 / R1 / L2 / R2 | 左前斜 / 右前斜 / 左后斜 / 右后斜 |
| SELECT / START | 舵机1 微调(-2 / +2) |

### 编译 & 烧录方法

**依赖库（需安装到 Arduino 库目录 `~/Documents/Arduino/libraries`）：**
- `Emakefun_MotorDriver`
- `NewTone`
- `PS2X_lib`
> 这三个库**不在 arduino-cli 官方库管理器**中，需从各自 GitHub 仓库手动 clone 到库目录（以实际仓库地址为准）。

**方式一：命令行（arduino-cli，推荐）**
```bash
# 1) 把 3 个库 clone 到库目录(示例)
cd ~/Documents/Arduino/libraries
git clone <Emakefun_MotorDriver仓库URL>
git clone <NewTone仓库URL>
git clone <PS2X_lib仓库URL>

# 2) 编译(Arduino UNO)  —— 打印编译进度、依赖库列表、Flash/RAM 占用
arduino-cli compile --fqbn arduino:avr:uno main/boards/bread-compact-wifi-s3cam-airobot/arduino/MecanumRobot
#     查看详细日志(编译命令/警告): 加 -v
#     保留日志: 末尾加 2>&1 | tee build.log

# 3) 烧录(示例端口, 按实际修改)
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn arduino:avr:uno main/boards/bread-compact-wifi-s3cam-airobot/arduino/MecanumRobot
```

**方式二：Arduino IDE**
1. 用 Arduino IDE 打开 `MecanumRobot.ino`。
2. 库管理器搜索安装上述 3 个库（或手动安装）。
3. 选择板型（Arduino UNO）和端口，编译烧录。

### ⚠️ 使用注意
- **看日志/烧录 ESP32 时，请先断开 Arduino 与 ESP32 的接线**（因为 ESP32 的 GPIO43/44（板载 USB-UART）与 Arduino 共用，插电脑会产生干扰）。
- ESP32 默认日志已降到 **`ERROR`**，且命令带 **`@` 前缀**（Arduino 只认 `@` 开头的行），日志乱码会被忽略，命令更稳定。
- Arduino 程序用**固定 `char` 缓冲**解析命令（不用 `String`），适合 UNO 的 2KB SRAM，抗内存碎片。

## 与上游合并提示

作为独立命名的 board（`bread-compact-wifi-s3cam-airobot`），其目录与 `config.json` 的 `type`/`name` 均为唯一标识，不会与上游同名板冲突。合并上游代码时注意保留 `main/Kconfig.projbuild` 与 `main/CMakeLists.txt` 中本板的注册分支。
