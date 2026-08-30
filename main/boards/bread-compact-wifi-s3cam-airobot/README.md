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

### 在 Windows 11 上安装 ESP-IDF v6.0.2（备查）

以下是在 **Windows 11 + cmd** 上安装 v6.0.2 的完整流程，与 v5.5 共存于 `D:\Espressif`，互不覆盖。

> **前置**：安装 Python（`python` 与 `git` 在 PATH 中可用）。可复用官方 `idf-env` 已装好的 git（`D:\Espressif\tools\idf-git\...\cmd\git.exe`）。

**① 拉取源码（用 gitee 乐鑫官方镜像，国内快）：**

```cmd
cd /d D:\Espressif\frameworks
set PATH=D:\Espressif\tools\idf-git\2.44.0\cmd;%PATH%
git clone --branch v6.0.2 https://gitee.com/EspressifSystems/esp-idf.git esp-idf-v6.0.2
```

**② 拉取子模块（gitee 无子模块镜像，改写为 GitHub 绝对地址后再拉，避免解析到 gitee 404）：**

```cmd
cd /d D:\Espressif\frameworks\esp-idf-v6.0.2
:: 把相对 URL ../../xx/yy.git 统一改为 github 绝对地址
python -c "import re,io; s=io.open('.gitmodules',encoding='utf-8').read(); io.open('.gitmodules','w',encoding='utf-8',newline='\n').write(re.sub(r'url = \.\./\.\./', 'url = https://github.com/', s))"
git submodule sync
git submodule update --init --recursive --depth 1
```

**③ 安装工具链（`all`，工具链二进制走乐鑫国内镜像 `dl.espressif.cn/github_assets`）：**

```cmd
set IDF_PATH=D:\Espressif\frameworks\esp-idf-v6.0.2
set IDF_TOOLS_PATH=D:\Espressif
set IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets
set IDF_PIP_WHEELS_URL=https://dl.espressif.com/pypi
python %IDF_PATH%\tools\idf_tools.py install --targets=all
```

**④ 创建独立的 v6 Python 环境（必须显式指定路径，避免复用/污染 v5.5 的 `idf5.5_py3.11_env`）：**

```cmd
set IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf6.0_py3.12_env
python %IDF_PATH%\tools\idf_tools.py install-python-env --features=core
```

**⑤ 验证：**

```cmd
idf.py --version   :: 应显示 ESP-IDF v6.0.2(-dirty)
```

安装完即得到了：源码 `D:\Espressif\frameworks\esp-idf-v6.0.2`、共享工具链 `D:\Espressif\tools`（按版本子目录与 v5.5 共存）、隔离 Python 环境 `D:\Espressif\python_env\idf6.0_py3.12_env`。

### 加载 ESP-IDF v6.0.2 环境

> ⚠️ 本项目必须使用 **ESP-IDF v6.0.2**。v6 使用**独立的 Python 虚拟环境**（`idf6.0_py3.12_env`），与 v5.5（`idf5.5_py3.11_env`）**完全隔离**，两者共存于 `D:\Espressif`，互不影响。
>
> 关键：激活时必须设置 `IDF_PYTHON_ENV_PATH` 指向 v6 环境，否则 `export` 会误用 v5.5 环境（会污染/报依赖缺失）。

**Windows（本机已装，推荐用快捷脚本）：**

- **CMD**：`D:\Espressif\idf6.bat`（双击或命令行运行，脚本已内置正确环境变量）
- **PowerShell**：`. .\idf6.ps1`（必须带前导点号点源）

或者手动一条命令激活：

```cmd
set IDF_TOOLS_PATH=D:\Espressif && set IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf6.0_py3.12_env && call D:\Espressif\frameworks\esp-idf-v6.0.2\export.bat
idf.py --version      # 应显示 ESP-IDF v6.0.2-dirty（出现 "-dirty" 是仓库有改动标记，属正常）
```

**Linux / macOS（上游通用写法）：**

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

### ⚠️ 踩坑记录：改 config.json 后 `idf.py build` 不生效

> **教训**：只修改 `config.json` 的 `sdkconfig_append`，再用 `idf.py build` 编译，新配置**完全不会生效**——`idf.py build` / `idf.py menuconfig` 根本不读 config.json，它只被 `scripts/build.py` 读取。本次调试 TF 卡中文文件名乱码时就因此误以为改动无效，浪费了时间。

改 Kconfig 配置有三条路径，按推荐度排序：

1. **统一用 `scripts/build.py` 构建**（推荐）：配置只写在 `config.json` 的 `sdkconfig_append`，一处维护，脚本每次重新生成 sdkconfig 自动带上。
2. **`idf.py menuconfig` 手动设置**：直观，但每次改动都要手动操作，易漏。
3. **直接改 `sdkconfig` 文件**：当前构建立即生效，但 `sdkconfig` 是构建生成物（已被 .gitignore），kconfig 在 cmake 阶段可能回写/重建，改动可能被覆盖，不推荐作为长期维护方式。

**本板实例**：TF 卡中文文件名乱码的根因是 FATFS API 编码为 ANSI/OEM(CP437，不含中文字符)，需改为 `CONFIG_FATFS_API_ENCODING_UTF_8=y`。该配置按标准做法写入两处**持久入口**：
- **`sdkconfig.defaults`**（项目级）：`idf.py build` 重建 sdkconfig 时生效；
- **`config.json` 的 `sdkconfig_append`**（本板）：`scripts/build.py` 构建时生效。

> ⚠️ **不要直接改 `sdkconfig` 文件**——它是构建生成物（`.gitignore`），`reconfigure`/`build.py` 重建时会被覆盖丢失，不是配置入口。验证方法：

```bash
# Windows PowerShell
Select-String FATFS_API_ENCODING sdkconfig
# 应看到 CONFIG_FATFS_API_ENCODING_UTF_8=y
```

## 功能：TF 卡本地歌曲播放（AI 控制）

本板在面包板基础上新增「TF 卡本地歌曲播放」能力，让 AI 语音助手直接播放 TF 卡上的本地音乐（MP3），替代官方云端曲库中数量有限的歌曲。

### TF 卡准备
- 把歌曲（**MP3** 格式）放入 TF 卡的 `music` 目录：`/sdcard/music/*.mp3`。
- 播放器启动时会扫描该目录，自动列出歌名。

### 歌词显示（LRC）
- 同名歌词文件（`歌曲名.lrc`，与 .mp3 同目录）会被自动解析，播放时逐行显示在屏幕底部字幕条。
- **歌词必须是 UTF-8 编码**：国内音乐软件下载的 .lrc 多为 GBK，设备端（ESP-IDF v6）没有 GBK→UTF-8 转换能力，直接用会乱码/不显示。
- **用转码脚本处理即可**：本板目录下的 `scripts/mp3_convert_for_esp32s3.py`（见下文「转码脚本」）会把同名 .lrc 自动探测编码并转为 UTF-8 输出，把输出目录里的 .mp3 和 .lrc 一起拷到卡上即可。

### AI 语音指令（服务端通过 MCP 工具自动调用）

| 你对助手说的话 | 触发工具 | 效果 |
|--------|---------|------|
| 「播放一首歌 / 随机播放」 | `self.music.play_random` | 随机播放一首 |
| 「播放《歌名》」 | `self.music.play` | 播放指定歌曲（参数 `name`）|
| 「暂停播放」 | `self.music.pause` | 暂停 |
| 「继续播放」 | `self.music.resume` | 继续 |
| 「停止播放」 | `self.music.stop` | 停止 |
| 「有什么歌」 | `self.music.list` | 列出 TF 卡歌曲 |

- **队列播放**：「播放《某歌》」→ 从该歌开始按**文件名字典序**播完列表后自动停止；「随机播放」→ **随机打乱顺序**播完列表后自动停止（播放中再喊随机不打断当前队列）。
- **可被打断**：播放中被唤醒/说话会打断本地播放，恢复语音交互。

### 实现说明
- 本地播放复用项目官方的 `esp_audio_codec` 组件 MP3 **简单解码器**（`esp_audio_simple_dec`，自带 parser：自动跳过 ID3v2 标签、搜索帧同步、处理跨块边界）+ `ESP-Audio-Effects` 声道转换器（`esp_ae_ch_cvt`，立体声降混为单声道，匹配小智全链路 mono 输出）+ `AudioService` 播放链路，**无自定义解析逻辑**。
- LRC 歌词为简单的 `[mm:ss.xx]` 文本格式，用 C++ 标准库字符串解析（约 60 行，无第三方库可用）；编码要求 UTF-8。
- **播放中可被打断**：唤醒词（板级回调钩子直接停歌）、按钮（先停歌再本地回待命，不依赖服务器响应）、状态机检测（Idle→非Idle）三重机制，随时让 AI 接管（不会“失联”）。播放期间设备状态**钉在“说话中”**（Speaking），屏幕明确显示正在播放；暂停时不钉，自然播完自动回待命。
- 播放器源码位于本板目录：`local_music_player.h` / `local_music_player.cc`（由 CMake `file(GLOB)` 自动编译）。
- `AudioService` 仅新增一个 `PushLocalPcm()` 注入接口（最小、纯新增）。

### 转码脚本（重要：原始 320kbps 歌会卡 + 唤醒失灵）
本板目录下的 `scripts/mp3_convert_for_esp32s3.py` 批量转码（在项目根目录运行）：
```powershell
python main/boards/bread-compact-wifi-s3cam-airobot/scripts/mp3_convert_for_esp32s3.py "E:/音乐/2026新下" "D:/music_s3"
```
- 输出 24000Hz 单声道 96kbps MP3（解码负载降约 4 倍，播放流畅、唤醒灵敏）；
- **同名 .lrc 自动转 UTF-8** 一并输出；
- 幂等：已转换的文件自动跳过，可重复运行。
- 转码后可拷贝到 TF 卡，或通过 WiFi 网页上传（`http://<设备IP>/`）。

### WiFi 网页上传与 IP 显示
- **IP 显示**：待机（待命）状态下，屏幕底部会显示本机 IP（如 `192.168.31.74`），照着输入浏览器即可打开上传页；其他状态（说话中/聆听中等）自动隐藏。
- **上传页**（`http://<设备IP>/`）：多选文件（支持 .mp3 / .lrc）、实时进度条、**同名覆盖开关**（默认勾选=覆盖；取消勾选=同名跳过，页面提示“同名已存在，跳过”）。
- 上传成功回调会自动刷新歌曲列表，AI 立刻能查到新歌（无需重启）。
- 上传/播放源码位于本板目录：`http_upload_server.h` / `http_upload_server.cc`（由 CMake `file(GLOB)` 自动编译）。
- 日志说明：上传成功路径不打日志（避免刷屏），仅错误（缺参数/写卡失败/同名跳过等）和启动（`Upload server started`）打印；板子日志级别为 ERROR，见串口日志需保留。

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
| **TX → GPIO44** | ESP32 UART0 RX（**双向回执**，需接线）|
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
| `@line-start` / `@line-stop` | 巡线模式（沿地面黑线自动行驶）开始/停止 |

**双向回执（Arduino → ESP32）**：耗时动作（`go-*` / `tj-*` / 巡线）开始执行时回传 `@busy`，执行完毕回传 `@done`。ESP32 的 UART0 RX 解析任务维护状态，AI 可通过 `self.uno.get_status` 查询（返回 `moving` / `idle (last: ...)`）。

### 巡线（4 路循迹传感器）
- **接线**：传感器 `S1→D7, S2→D4, S3→D3, S4→D2`（`S1..S4` 从左到右），`GND→GND`，`5V→5V`（VCC）。
- **原理**：4 路数字输出读线位置（加权 -1.5/-0.5/+0.5/+1.5）→ 比例差速（`LINE_KP`）控制左右轮保持沿黑线前进。
- **AI 指令**：说「开始巡线 / 沿着线走」→ `self.uno.line_follow(1)`；「停止巡线」→ `(0)`。
- **结束条件**：连续丢线超过 `LINE_LOST_MS`（默认 300ms，线断/到终点）或总时长超 `LINE_MAX_MS`（默认 2 分钟）→ 自动停车并回传 `@done line-follow`，AI 会汇报「巡线结束」。
- **可调参数**（文件顶部宏）：`LINE_BASE_SPEED` 基础速度、`LINE_KP` 转向强度、`LINE_LOST_MS` 丢线判定、`LINE_MAX_MS` 时长上限。
- **若黑线电平反向**（黑线=LOW）：把 `LINE_ACTIVE` 改为 `false`；**若传感器左右朝向装反**：把 `readLinePosition()` 返回值取反。
- **巡线中**：`@line-stop` 可随时急停（非阻塞检测）；PS2 手柄仍可用作干预。

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

### 指令执行模型与串口缓冲（重要）
- **顺序执行**：Arduino 读一条执行一条（`runMotors` 内 `delay` 阻塞），先到先执行，**不会乱序**。
- **RX 缓冲**：UNO 默认 HardwareSerial 接收缓冲仅 **64 字节（≈4-5 条指令）**。动作阻塞执行期间（如 `go-forward-15` 执行 1.5 秒）不读串口，后续指令积压在缓冲里，超出部分**溢出丢弃**（表现为“后面的指令跳过了”）。已改为 `Serial.setRxBufferSize(256)`（≈17 条），正常 AI 编排序列（3-10 条）不会丢；若实测超长序列仍丢，可再加大或让 ESP32 读 Arduino 回执（`Serial.println("F")` 等已存在）判断动作完成再发下一条。

### 踩坑：AI 重复调用 uno 工具（已修复）
**现象**：说一次“前进”，ESP32 串口发出几十次 `@go-forward-10`，机器人反复动。
**根因**：服务端 AI 无执行确认机制，对 `uno` 工具反复生成相同调用（实测 30 次、间隔约 700-900ms、JSON-RPC id 递增）；设备端每次毫秒级正确回复，**设备端无 bug**——同一日志里 `music.*` 工具全部只调用一次（同样返回 `true` 却不重复），对照即可实锤。
**修复**（ESP32 端三重机制，均在板级文件 `compact_wifi_board_s3cam_airobot.cc`）：
1. **工具描述**明确“调用本工具一次即完成整个动作并自动停止，不要重复调用”——治本，实测 AI 不再重复（前进/后退/走 20 步/特技编排均只调用一次，组合动作正常）；
2. `SendUartMessage` 返回**描述性文本**（`指令已发送: xxx` / `指令发送失败: xxx`）而非裸 `true/false`——AI 能确认执行结果；
3. **指令防抖**：相同指令 1 秒内只发送一次（防抖命中返回“指令已发送(防抖): xxx”视为成功），AI 不听话也挡得住；不同指令（组合编排）不受影响。
**排查方法备忘**：在 `McpServer::ReplyResult` 临时加一行日志打印 payload，可确认设备端每次调用都发出结果；比较重复调用 id 递增（服务器独立请求）还是相同（重发）；对比不同工具（uno 重复 vs music 正常）即可定位是设备端还是服务器端。

## 与上游合并提示

作为独立命名的 board（`bread-compact-wifi-s3cam-airobot`），其目录与 `config.json` 的 `type`/`name` 均为唯一标识，不会与上游同名板冲突。合并上游代码时注意保留 `main/Kconfig.projbuild` 与 `main/CMakeLists.txt` 中本板的注册分支。
