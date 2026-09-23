# Bread Compact Wi-Fi S3Cam AI Robot (面包板)

本板由 `bread-compact-wifi-s3cam` 克隆而来，作为 AI 机器人 DIY 基础版本。

- 主控：**FREENOVE ESP32-S3 WROOM**（板载 TF 卡槽 + OV2640 摄像头 + RGB LED）。
- 音频：数字麦克风 **INMP441** + 数字功放 **MAX98357A** + 喇叭。
- 显示：SPI LCD（ST7789 240x240）。
- 网络：Wi-Fi。

> ⚠️ 本板为 DIY 方案，**引脚与官方 ESP32-S3-CAM 面包板教程不同**，请严格按下表接线。

## 硬件清单

| 硬件 | 型号 | 用途 |
|------|------|------|
| 主控板 | FREENOVE ESP32-S3 WROOM | 运行固件、音视频、网络 |
| 数字麦克风 | INMP441 | 音频输入 |
| 数字功放 | MAX98357A | 音频输出驱动 |
| 喇叭 | 8Ω 2~3W | 扬声器 |
| LCD 屏 | ST7789 240x240 (SPI) | 显示 |
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

## 环境准备：ESP-IDF v6.0.2

> ⚠️ 本项目必须使用 **ESP-IDF v6.0.2**。v6 使用**独立的 Python 虚拟环境**（`idf6.0_py3.12_env`），与 v5.5（`idf5.5_py3.11_env`）**完全隔离**，两者共存于 `D:\Espressif`，互不影响。

### 本机（macOS）快速启动（先看这里）

> 当前开发机为 **macOS**，IDF v6.0.2 已安装在此：
>
> - IDF 根目录：`~/esp/v6.0.2/esp-idf`
> - 激活命令：`source ~/esp/v6.0.2/esp-idf/export.sh`
> - 版本验证：`idf.py --version`（应输出 `ESP-IDF v6.0.2`，**已实测可用**）
>
> 编译/烧录前先执行激活；Windows 上的安装与加载细节见下方章节（备查）。

### 本机（Windows）快速启动（先看这里）

> Windows 开发机（主力）IDF v6.0.2 已安装：
>
> - IDF 根目录：`D:\Espressif\frameworks\esp-idf-v6.0.2`
> - 工具链目录：`D:\Espressif\tools`（按版本子目录与 v5.5 共存）
> - 快捷脚本：`D:\Espressif\idf6.bat`（CMD，双击或命令行运行） / `idf6.ps1`（PowerShell，需点源）
> - 手动激活：`set IDF_TOOLS_PATH=D:\Espressif && set IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf6.0_py3.12_env && call D:\Espressif\frameworks\esp-idf-v6.0.2\export.bat`
> - 版本验证：`idf.py --version`（应显示 `ESP-IDF v6.0.2`）
>
> 编译/烧录前先执行激活；完整安装流程见下方「安装（Windows 11，备查）」。

### 安装（Windows 11，备查）

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

### 加载环境

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

## 编译与烧录（ESP32 固件）

> 编译前先加载环境，见上节「环境准备：ESP-IDF v6.0.2」。

### 方式一：构建脚本（推荐，自动配置板子/屏幕）

```bash
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
```

> `scripts/build.py` 会读取 `config.json` 的 `sdkconfig_append`（屏幕类型、console 配置等）并自动配置，推荐日常使用。

### 方式二：idf.py（需手动 menuconfig）

```bash
idf.py set-target esp32s3
```

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

> ⚠️ `idf.py build` **不读取** config.json 的 `sdkconfig_append`（如屏幕类型、console 配置），需通过 `menuconfig` 手动设置——详见下方「踩坑记录」。

> ⚠️ 用此方式构建时，还需在 `menuconfig` 开启 **WebSocket 支持**：`Component config → ESP HTTP server → HTTPD WS Support`（即 `CONFIG_HTTPD_WS_SUPPORT=y`）。否则本板 `/ws` 网页控制代码（`httpd_ws_*`）在未开启时未声明，会**编译失败**。

### 方式三：一键编译 + 烧应用 + 监视（日常迭代推荐）

```bash
idf.py build app-flash monitor
```

> 等价于“编译 → 只写 app 分区 → 开串口监视”，一条命令走完，日常改代码验证最快。
>
> - 用「方式一」的 `scripts/build.py` 构建过一次后，也可以直接用这条命令编译+烧录（同一个 `build/` 目录，配置已就绪）；但**改了 `config.json` 的 `sdkconfig_append` 或板子选项时，仍要走 `scripts/build.py`** 重新配置。
> - **`app-flash` 只写 app 分区**（本板网页页面、字体等嵌入式资源都编在 app 里，所以改网页/改代码只需它），比 `flash` 快，不动 bootloader / 分区表。**但改了 `partitions.csv` 或 bootloader 相关配置（如 console）时，必须改用 `idf.py build flash` 全量烧**，否则改动不生效。
> - 有多个串口设备时显式指定端口：`idf.py -p COM3 build app-flash monitor`（macOS：`-p /dev/cu.usbserial-XXXX`）。**退出 monitor：`Ctrl+]`**。
> - ⚠️ **本板 monitor 里看不到 `ESP_LOGx`**：日志默认写内存环形缓冲，请看网页「🐞 系统日志」Tab（详见「实时日志与下位机指令」）。串口只剩上电早期的 ROM/bootloader 输出。

### 查看编译日志与运行日志

- `idf.py build` / `scripts/build.py` 都会把编译进度**打印到终端**，报错也会完整输出。若想留存日志：

```bash
idf.py build 2>&1 | tee build.log
```

- 编译产物在 `build/` 目录：`build/xiaozhi.bin`、`build/merged-binary.bin`。
- 烧录后看**运行日志**（monitor，端口按实际修改）：

```bash
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

> ⚠️ **本板的运行日志默认不在串口输出**：ESP32 的 console 与 Arduino 下位机控制指令**共用 UART0(GPIO43/44)**，日志走串口会污染指令流（表现为控制失灵/延迟）。
> 因此运行日志改为写入**内存环形缓冲**，用**网页「🐞 系统日志」Tab**查看（无需断线、无需串口）。
> 串口 monitor 只能看到上电早期的 ROM/bootloader 输出，看不到 `ESP_LOGx`。
> 要临时恢复串口日志：网页日志面板勾选「同时输出到串口」，或对 AI 说「打开串口日志」（会干扰 Arduino，用完记得关）。详见「实时日志与下位机指令」。

### 📦 打包合并固件（Flash 下载工具直接烧录）

把 `build/` 产物合并成**单个 bin 文件**，方便用 **ESP32 Flash Download Tool**（乐鑫串口下载工具）一键烧录，无需命令行烧录。

**运行**（无需进入 IDF 环境，脚本自动定位 esptool）：

```powershell
python scripts\merge_bin.py
```

**输出**（项目根目录 `packages/` 下，文件名自动带板名 + 变体 + 日期）：

```
packages/bread-compact-wifi-s3cam-airobot-no-tfcard-merged-20260830.bin
```

- 板名/变体自动读取 `build/config/sdkconfig.json`：TF 卡版 → `<板名>-merged-<日期>.bin`；无 TF 卡版 → `<板名>-no-tfcard-merged-<日期>.bin`（`.bin` 后缀前的日期为打包当天）。
- 合并内容：bootloader + 分区表 + ota_data + 资源 + 应用（与 `idf.py flash` 写入的内容完全一致）。

**Flash 下载工具烧录步骤**：
1. 芯片选 **ESP32-S3**；
2. Load `packages/` 里的 bin，起始地址填 **`0x0`**；
3. 选好 COM 口 → **START**（可直接用 USB 烧录）。

> **注意**：每次 `idf.py build` 重新编译后，都要**重新运行一次打包命令**（包不会自动更新）；可选参数：`--build-dir`（构建目录）、`--output-dir`（输出目录）。

### 无 TF 卡模式（编译变体，砍掉音乐/上传功能）

板子支持编译成**不依赖 TF 卡**的版本：接线完全不变，只是不挂载 SD 卡、不提供本地音乐播放与网页上传（`self.music.*` 工具、歌词、上传页均无）。适合没插卡或不需要本地音乐的场景。

开关：`CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD`（默认 `y`，开启全部 TF 卡功能）。

**方式一：`idf.py menuconfig`（适合直接 build 的用户，推荐）**

```bash
idf.py menuconfig
```

进入菜单：

```
TF Card Features
  └─ [*] Enable TF card features (local music / web upload) for bread-compact-wifi-s3cam-airobot
```

- **取消勾选**（`[ ]`）→ 保存退出 → `idf.py build` = 无 TF 卡版；
- **勾选**（`[*]`）→ 保存退出 → `idf.py build` = 完整版。

**方式二：`scripts/build.py` 变体（给 CI/发布脚本用）**

```bash
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot-no-tfcard
```

> 无 TF 卡版保留：AI 对话、摄像头（拍照/翻转）、屏幕、按钮、唤醒词、Arduino 双向通信、IP 显示；仅跳过 SD 卡挂载、音乐播放、网页上传。
>
> **引脚自动切换（重要）**：无 TF 卡模式下功放恢复与**原始板 `bread-compact-wifi-s3cam` 完全一致**的接线：DOUT=`39`、BCLK=`40`、LRCK=`41`（TF 版为 `3/14/46`），背光也恢复 `38`（TF 版为 NC）。若你的功放仍按 `3/14/46` 接线（TF 版），无 TF 卡版会无声——此时把功放三根线改回 `39/40/41` 即可。

## Web 控制页（http://<设备IP>/ 功能总览）

> 浏览器打开设备 IP（待机状态屏幕底部会显示，如 `192.168.31.74`）即进入同一网页 `http://<设备IP>/`，聚合了以下功能：

| 功能区 | 说明 | 详见 |
|--------|------|------|
| 🎵 歌曲上传 | 多选上传 .mp3/.lrc/wav/m4a…、**逐条行内进度条**、同名覆盖开关 | 「TF 卡本地歌曲播放」|
| 🔊 提示音上传 | **同一页**（歌曲管理下方）按固定槽位上传 `motion` / `hot` / `beam` 三个传感器播报音，只有“已上传/未上传”两态 | 「传感器提示音」|
| ⏰ AI 闹钟 | 列表查看/删除 + **弹窗新建**（与上传歌曲同一套弹窗样式）| 「AI 闹钟提醒」|
| 🕹️ 机器人摇杆 | 麦克纳姆轮方向/速度控制 + 头部舵机 + **下位机指令记录**（与系统日志同源，只看 `[UNO]` 行） | 「Arduino 下位机」|
| 📷 照片 | 拍照并显示当次那张（含 **🧹 清空**）+ TF 卡相册翻看/删除/「AI 拍照也存卡」开关 | 「网页拍照」「照片相册」|
| 📹 实时视频 | 「机器人控制」面板摇杆**上方**的勾选框：勾选才启流（MJPEG，画面上有帧率/分辨率角标），取消立即停流（拍照与 LCD 预览不受影响）| 「网页实时视频流」|

> 页面有五个 tab（歌曲管理 / 闹钟提醒 / 机器人控制 / 照片 / 系统日志）——「提示音」不占 tab，就在**歌曲管理**页下方；**拍照与照片相册都在「📷 照片」Tab**。
> 2026-09 起「系统日志」是**独立 tab 且排在最末**（原先是折叠在「🎮 机器人控制」面板底部）：边看日志边切面板排查更顺手，机器人面板也不再被日志占长。
> 「🎮 机器人控制」面板里仍保留**下位机指令记录**——它与系统日志同源（同一个环形缓冲），只筛出 `[UNO]` 行。
> 2026-09 起该面板**摇杆上方**多了「📹 实时视频」勾选框：勾选才推流，取消立即停（见「网页实时视频流」）。

- **入口与 IP**：待机时屏幕底部显示本机 IP，照输入浏览器即可；其他状态（说话中/聆听中等）自动隐藏。
- 上传成功自动刷新歌曲列表，AI 立即能查到新歌；具体细节见对应章节。
- **延迟与功耗**：页面打开（WS 已连接）期间，板级会把 WiFi 强制为**性能模式**，避免待机省电导致遥控几百毫秒延迟（详见踩坑 7）；关闭页面后自动恢复省电。
- **连接状态（页首常驻）**：`🟢 已连接` / `🔴 断开，重连中…`。上传、遥控、日志都走这条 WebSocket 长连接，**操作没反应时先看这里**；一次性提示（如“舵机命令发送失败，请重试”）会临时覆写文字，2.5 秒后自动回到连接状态（圆点颜色不受影响）。
- **⛶ 全屏（页首右上角）**：一键切页面全屏（手机浏览器进去后地址栏也收起来，视频/摇杆更宽敞），再点一次或按 `Esc` 退出；按钮文字随全屏状态自动切换。
  iPhone 上的 Safari **不支持元素级全屏**（只有 iPad/桌面支持），点了会在页首提示一行说明，不会静默失败。
  > 放在 `.wsbar`（页首那行）里而不是 `.tabs` 里：`.tabs` 窄屏会横向滚动，塞进去在手机上要滑到最右才看得到，就不叫“右上角”了。
- **按钮反馈约定（改样式时别丢）**：所有可点控件都要有悬停/按下反馈 —— `.btn`/`.btn-danger`/`.btn-quiet`/`.tab`（未选中）/相册 `×`/视频框 ⚙️ 都有 `:hover` + `:active`，浅灰控件还必须带 `cursor:pointer`（否则鼠标放上去像块死板子）。
  ⚠️ ⚙️、相册 `×` 这类**深色浮层按钮的 `background` 必须写在 CSS 类里**：写成内联样式优先级更高，会把手写的 `:hover` 盖掉（回归防护：`scripts/tests/test_web_ui_affordance.py`）。
- **一次性命令无重发**：舵机/回正等命令没有心跳重发，发送失败时页面顶部 `#wslog` 会提示“发送失败，请重试”（详见踩坑 8）。
- **机器人控制面板布局**：摇杆与「左转 / 右转」**同一行**（按钮紧贴摇杆两侧，与摇杆内的「左移 / 右移」标签同高）；**「停止」按钮浮在这一行的左上角**（与摇杆顶部「前进」同一水平线，不单独占行）。
  - 停止按钮是绝对定位在 `.joy-line` 里的。这是安全的：行高由 190px 的摇杆撑开，而「左转」只有 ~30px 高且**垂直居中**，所以贴顶的左侧必然是空地，窄屏也不会压到它；它写在 `#joyBase` **外面**，不会跟摇杆抢 `pointer` 事件。
  - 改 `.joy-line` / `.stop-btn` 时注意别把「左转」改成 `align-self: flex-start`（靠顶），否则两者会重叠。
- **摇杆 / 转弯手感参数**（集中在 `web/index.html` 的「机器人控制」常量区，改完需重新编译 + 浏览器强刷）：
  - `TURN_SPEED`：「左转/右转」按钮的固定转速（**当前 `80`**；原 110，实测试出来“转太快、容易转过头”后调低）。
  - `TURN_MIN_PRESS_MS`：**点一下**的最短转动时长（**当前 `160`**ms；原 250）——它和 `TURN_SPEED` 相乘决定点一下的转弯幅度，嫌转太多就调小这两个（历史：400ms @ 140 → 250 @ 110 → 160 @ 80，点一下的幅度逐步减半）。
  - `JOY_DEAD`：摇杆回中死区（默认 `0.14`）。
  - 注意：`TURN_MIN_PRESS_MS` 不能太小，否则“点一下”会因持续转动还没起步就被 `drive-stop` 刹停，表现为“必须按住才动”。

## TF 卡本地歌曲播放（AI 控制）

本板在面包板基础上新增「TF 卡本地歌曲播放」能力，让 AI 语音助手直接播放 TF 卡上的本地音乐（MP3），替代官方云端曲库中数量有限的歌曲。

### TF 卡准备
- 把歌曲（**MP3** 格式）放入 TF 卡的 `music` 目录：`/sdcard/music/*.mp3`。
- 播放器启动时会扫描该目录，自动列出歌名。
- 歌单访问是**按需遍历、不整表拷贝**（`HasSongs()` / `ForEachSong()`，见踩坑 20）：本板内部 SRAM 紧张，
  `self.music.list` / `self.music.search` / 闹钟响铃前判空都**不会**复制整张歌单。

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
| 「有什么歌」 | `self.music.list` | 列出 TF 卡歌曲（上限 30 首 + 总数）|
| 「有没有某某的歌」 | `self.music.search` | 按关键字搜索全部歌曲（子串/大小写不敏感，如「薛之谦」）|

- **队列播放**：「播放《某歌》」→ 从该歌开始按**文件名字典序**播完列表后自动停止；「随机播放」→ **随机打乱顺序**播完列表后自动停止（播放中再喊随机不打断当前队列）。
- **可被打断**：播放中被唤醒/说话会打断本地播放，恢复语音交互。

### 实现说明
- 本地播放复用项目官方的 `esp_audio_codec` 组件 MP3 **简单解码器**（`esp_audio_simple_dec`，自带 parser：自动跳过 ID3v2 标签、搜索帧同步、处理跨块边界）+ `ESP-Audio-Effects` 声道转换器（`esp_ae_ch_cvt`，立体声降混为单声道，匹配小智全链路 mono 输出）+ `AudioService` 播放链路，**无自定义解析逻辑**。
- LRC 歌词为简单的 `[mm:ss.xx]` 文本格式，用 C++ 标准库字符串解析（约 60 行，无第三方库可用）；编码要求 UTF-8。
- **播放中可被打断**：唤醒词（板级回调钩子直接停歌）、按钮（先停歌再本地回待命，不依赖服务器响应）、状态机检测（Idle→非Idle）三重机制，随时让 AI 接管（不会“失联”）。播放期间设备状态**钉在“说话中”**（Speaking），屏幕明确显示正在播放；暂停时不钉，自然播完自动回待命。
- 播放器源码位于本板目录：`local_music_player.h` / `local_music_player.cc`（由 CMake `file(GLOB)` 自动编译）。
- `AudioService` 仅新增一个 `PushLocalPcm()` 注入接口（最小、纯新增）。

### 转码（重要：原始 320kbps 歌会卡 + 唤醒失灵）

**方式一：网页上传时自动转码（推荐，无需任何脚本）**
打开上传页选好文件后，页面会**先探测参数再决定是否转码**（只读文件头，毫秒级）：

| 探测结果 | 处置 |
|------|------|
| 单声道 + ≤24kHz + ≤128kbps | **无需转码，原样上传**（避免二次音损与无谓等待） |
| 立体声 / 采样率 >24kHz / 码率 >128kbps | 自动转码为 24000Hz 单声道 96kbps |
| VBR（码率不定）/ 解析失败 | 保守转码 |

- **歌词（`.lrc`）单独探测编码**（选文件时完成，只读歌词本身，与 MP3 无关）：

| 探测结果 | 处置 |
|------|------|
| 合法 UTF-8（无 BOM） | **已是 UTF-8，原样上传**（一个字节都不改，不做无谓转码） |
| 合法 UTF-8 + BOM | 去 BOM 后上传（BOM 会让设备端首行多一个不可见字符） |
| 非 UTF-8（GBK / GB18030 / 混合 / UTF-16） | 一律按 GBK 转 UTF-8（设备端无 GBK 转换能力，不转就整篇不显示） |

- 每首 MP3 可在列表里手动改为「强制转码 / 不转码」；
- **设备端零改动、固件零增长**：转码在浏览器里完成（解码用 WebAudio，编码用 JS 版 LAME `lamejs` 1.2.1，按需从 CDN 加载，首次需联网、之后走浏览器缓存）；
- 无网时仍可上传本来就合格的文件；若转码组件加载失败会**回退为直接上传原文件**并提示；
- 实测：233 秒的 320kbps 立体声（9.1MB）在 PC 上约 **3 秒**编完，输出 2.7MB；
- 为什么不用 WebAudio 直接读参数：`decodeAudioData()` 会把音频重采样到 `AudioContext.sampleRate`，`AudioBuffer.sampleRate` 不是文件原始采样率，因此页面改为直接解析 MPEG 帧头（并跳过 ID3v2 封面）；
- 注意：`lamejs` 的 Xing/Info 信息帧码率字段不可信（实测 32kbps 写成 40kbps），页面解析器会跳过该帧取真实音频帧。

**方式二：电脑端批量脚本**（大量文件批量处理时用）
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
- **上传页**（`http://<设备IP>/`）：多选文件（支持 .mp3 / .lrc / .wav / .m4a / .flac 等）、**自动参数检测 + 按需转码**（非 MP3 一律转码；MP3 看声道/采样率/码率，歌词看编码；已是 UTF-8 的歌词原样上传）、**同名覆盖开关**（默认勾选=覆盖；取消勾选=同名跳过，页面提示“同名已存在，跳过”）。
  - **进度与结果都在各自文件那一行**（2026-09 起，用户反馈）：转码/上传进度条、`✅ 完成` / `⚠️ 同名文件已存在，跳过` / `❌ 失败`
    都落在**该文件自己那一条**里 —— 谁在传就显示在谁那一行，轮到它时自动滚进视野；完成后该条直接标记完成。
    不再用【上传】按钮下方的全局进度条：一次选几十个文件（歌 + 歌词）时，那个条离正在传的那条太远，看不出进度。
    按钮下方只留一个汇总/异常区（失败原因、全部完成统计），不再逐条写“上传 xxx / ✅ 成功 xxx”，否则弹窗会被拉得很长。
  - 上传期间「自动 / 强制转码 / 不转码」下拉框置灰（改了会整表重渲染，进度条会跟丢）。
- **提示音与歌曲共用一套接口**，靠 `dir` 参数区分：上传 `POST /upload?dir=announce&name=motion.mp3`（不带 `dir` 就是歌曲）；列表/删除同理（WS `music_list` / `music_delete` 带 `dir: 'announce'`）。提示音目录只收 `.mp3`，没有 `.lrc` 歌词。
- 上传成功回调会自动刷新歌曲列表，AI 立刻能查到新歌（无需重启）。
- 上传/播放源码位于本板目录：`http_upload_server.h` / `http_upload_server.cc`（由 CMake `file(GLOB)` 自动编译）。
- 日志说明：上传成功路径不打日志（避免刷屏），仅错误（缺参数/写卡失败/同名跳过等）以 ERROR 级打印；启动信息（`Upload server started`）为 INFO 级，板子默认日志级别 ERROR 下不显示，排障时调 INFO 可见。
- **完整性校验**：`/upload` 收完后会核对实收字节数与 `Content-Length`，不符则删除半截文件并返回失败，不再把截断文件当成功（详见踩坑 13）。

### 传感器提示音（录什么、放哪）

节点只上报“要播什么”（`say motion` / `say hot` / `say beam`），**播什么内容由主控 TF 卡决定**，
所以换台词不用重烧任何固件。文件放 `/sdcard/announce/<名字>.mp3`，名字**必须**一字不差：

| 文件名 | 什么时候播 | 建议台词 |
|--------|-----------|---------|
| `motion.mp3` | 超声波发现有人靠近、自动开灯时 | 「检测到有人靠近，已为你开灯。」|
| `hot.mp3` | 温度越过高温阈值（默认 28℃）时，降到 26℃ 以下才复位 | 「室内温度偏高，请注意通风降温。」|
| `beam.mp3` | 红外避障模块检测到障碍（约 2~30cm）时 | 「门口有人经过，请注意。」|

- **在哪上传**：`http://<设备IP>/` → 「🎵 歌曲管理」→ 页面下方「🔊 提示音（传感器播报）」→ 点对应槽位的「上传」；
- **文件名不用管**：槽位名固定，本地文件叫什么都行（手机录音 `xxxx.m4a` 也行），会上传为 `<槽位名>.mp3`；
- **自动转码**：wav/m4a/flac 等非 MP3 会自动转成 24000Hz 单声道 96kbps（与歌曲同一套浏览器端转码）；
- **录音建议**：手机录即可，安静环境、语速平稳，**2~3 秒**最好（太长会显得反应慢）；
- 播报只在主控**待机**状态出声（正在对话时不插嘴）；提示音目录与歌曲目录互相独立，不会被 `self.music.list` 当成歌。

### 网页上传（自动转码）真机验证要点
1. 待机状态记下 IP → 浏览器打开 `http://<设备IP>/` → 点「上传歌曲」（注意浏览器请 **Ctrl+F5** 强刷，否则可能是旧页面）；
2. 选一首 **320kbps 立体声** 歌 → 列表应显示 `44.1kHz 立体声 320kbps`、`将转码 → 24kHz 单声道 96kbps`；
3. 选一首**已转好**的（`24kHz 单声道 96kbps`）→ 应显示 `无需转码，直接上传`；
4. 选一个**已转好（UTF-8）** 的 `.lrc` → 应显示 `已是 UTF-8，直接上传`（**不再**显示旧的 `歌词 → 转 UTF-8`）；再选一个 **GBK** 的 `.lrc` → 应显示 `GBK → 转 UTF-8`；
5. 点「上传」：需转码的先出现「转码 xx%」再上传；播放应与脚本转出的效果一致（流畅、唤醒灵敏）；
6. **断网测试**：关闭手机数据/拔网线后上传需转码的歌 → 应提示“转码失败…改为直接上传原文件”而不是卡死（无需转码的文件不受影响）；
7. `.lrc` 上传后屏幕歌词应显示正常中文（无乱码）；原本就是 UTF-8 的歌词，上传后内容应与源文件逐字节一致（可用 `ffprobe`/`cmp` 对比）；
8. 手动覆盖：把某首改为「不转码」→ 应直接上传原文件（用于对比卡顿差异）。

## 摄像头画面翻转（AI 控制 + 本地持久化）
- 说「画面翻转 / 镜像 / 上下翻转」→ `self.camera.set_flip`（mode: `0`=正常, `1`=左右镜像, `2`=上下翻转, `3`=旋转180）。
- 设置写入 NVS（`camera/flip`），**断电重启自动恢复该设置**。
- 实现：板级 `ApplyCameraFlip()` 开机应用 + `Esp32Camera::SetHMirror/SetVFlip`（官方 sensor 寄存器接口）。

## 网页实时视频流（MJPEG，勾选才推）

「🎮 机器人控制」面板**摇杆上方**勾选 **📹 实时视频** → 摇杆上方出现实时画面，右上角角标显示**实测帧率与分辨率**（如 `12.3fps 640×480`）；取消勾选立即停流。

> **只有真有人看时才推流**：抓帧写在 `/stream` 的 HTTP handler 循环里，**没有浏览器连着就完全不抓帧**（不占 CPU、不占射频）。
> 另外，切到别的 Tab 会主动断开画面（设备随即停止抓帧），但保留勾选，切回来自动重连。

- **技术选型（官方标准做法，不自造协议）**：
  - 设备侧用 MJPEG：`multipart/x-mixed-replace` + `httpd_resp_send_chunk`，照 `espressif/esp32-camera` README 的 `jpg_stream_httpd_handler` 与 `espressif/esp-iot-solution` 的 `video_stream_server` 示例实现（见 `local_video_stream.cc`）。
  - 前端**零解码代码**：`<img src="http://<设备IP>:81/stream">` 浏览器原生就能显示 MJPEG。
  - 独立 httpd（**端口 81**）：`/stream` 是长循环 handler，挂在主 httpd（80，跑着 WS 控制/日志/上传）上会把那条任务占死。
- **相机全程单一 JPEG 模式（本功能的关键设计，2026-09 重构）**：

  | 状态 | 相机像素格式 | LCD 预览 | 说明 |
  |---|---|---|---|
  | 全程（开机 init 一次） | `PIXFORMAT_JPEG` | ✅ | 摄像头模组**自带 JPEG 编码**：推流**零编码零拷贝**（`fb->buf` 直接发）、拍照直通上传、LCD 预览现场解码 |

  - **为什么不按需切换（旧设计的坑，真机实测）**：两种格式各自的 DMA 都要一整块**连续内部 SRAM** ——
    VGA RGB565 要 **30720** 字节，JPEG 只要 **16384**（且与分辨率无关）；
    而本板实测最大连续块只有约 **12800**，且**开过一次视频后再也不回升**。
    于是只要 deinit/Reinit 过一次，两个模式就都 init 不回来：网页拍照 500 + 视频也开不起来，**只能重启**。
    所以改成**开机按 JPEG 初始化一次，之后不再动相机**（真机日志与完整推理见踩坑 22）。
  - 相机配置只写一处：板级 `MakeCameraConfig()`。初始化用**最大档 SVGA**（帧缓冲 `fb_size = 宽×高/5`
    按初始化时的分辨率算，按最大档给后续切换才够）、质量按拍照档 12 起。
  - **推流/停流只写 sensor 寄存器**（各两次 I2C 写、零内存分配、不重启相机）：
    `ApplyVideoSensorParams()`（用户设的分辨率 + JPEG 质量）/ `ApplyPhotoSensorParams()`
    （VGA + 质量 12，即不推流时拍照的画面与以前一致）→ 所以 `video_stop` **不可能失败**。
  - **LCD 预览没丢（用户明确要求保留）**：拍照时把 JPEG 帧用 `esp_jpeg` 的 ROM 解码器解成 RGB565（1/2 缩放）
    挂给 LVGL，见 `Esp32Camera` 文件头的 `DecodeJpegPreview()`；解码只在拍照路径做（httpd/MCP 任务），
    输出留在 PSRAM、草稿纸用静态 `work[3100]`（**不占内部堆**）。
  - JPEG 直通**不自己造**：靠上游 `image_to_jpeg.cpp` 自带的直通分支（需开 `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT`，
    回调形状与软件编码完全一致）—— 这样 `esp32_camera.cc` 里 `Explain()`/`EncodeCurrentFrameToJpeg()`
    都能保持上游原样（合并官方代码时冲突面最小）。**怎么开见下**。
  - **拍照后必须归还驱动帧（`fb_count=1` 的硬约束，2026-09 真机踩坑）**：`Capture()` 借走的是驱动**唯一**
    那块帧缓冲，用完要由 `ReleaseCurrentFrame()` 显式还回去（AI 拍照在 `Explain()` 出口自动还、
    网页拍照在编码后还）。攥着不放 → cam_hal 没空闲缓冲可采集 → 视频流每 ~4 秒刷一次
    `cam_hal: Failed to get frame: timeout` + `LocalVideo: fb_get failed`，**只能重启**。
    现象、根因与验证判据见踩坑 23。

### ▶ `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT` 怎么配（本板必须开）

**它是什么**：上游开关（`main/Kconfig.projbuild` → `Xiaozhi Assistant` → `Camera Configuration` →
`Allow JPEG Input`，**默认 n**，上游是给 USB 摄像头用的）：开启后 `image_to_jpeg_cb()` 遇到
`V4L2_PIX_FMT_JPEG` 就直接把这一整帧按回调投出去（`cb(0,整张)` + `cb(1,哨兵)`），不再送进软件编码器。
本板相机直出 JPEG，就靠它把 JPEG 帧原样送到上传链路。

**推荐做法：写在 `config.json`（已加好），用构建脚本生成 sdkconfig**

两个变体的 `sdkconfig_append` 里都有这一行；**不要手改 `sdkconfig`**（它是生成物、不入库、clean 就没了）：

```jsonc
// main/boards/bread-compact-wifi-s3cam-airobot/config.json
"sdkconfig_append": [
    ...,
    "CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y",
    ...
]
```

```sh
# ⚠️ 改完 config.json 必须走构建脚本：idf.py build 根本不读 config.json（见踩坑「改 config.json 后 idf.py build 不生效」）
source ~/esp/v6.0.2/esp-idf/export.sh
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
#  无 TF 卡变体：  --name bread-compact-wifi-s3cam-airobot-no-tfcard

# 验证：sdkconfig 与生成头里都应该是 y / 1
grep CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT sdkconfig          # → CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y
grep XIAOZHI_CAMERA_ALLOW_JPEG_INPUT build/config/sdkconfig.h  # → #define CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT 1
```

生成一次后，日常照旧 `idf.py build` 即可（该项已在 `sdkconfig` 里，不会丢）。

**手动方式（备查）**：`idf.py menuconfig` → **Xiaozhi Assistant → Camera Configuration → [*] Allow JPEG Input**。
⚠️ 别与 `XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE` 同时开（上游 help 明确说明二者不兼容；本板该项为 `n`）；
`XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER` 虽然 `depends on` 它，但只在 P4 可用，S3 上不会被自动打开。

**没开（或 sdkconfig 陈旧）会怎样**：JPEG 帧被当成“原始像素”送进软件编码器 →
日志 `image_to_jpeg: unsupported format: 0x4745504a`（小端就是 `'JPEG'`）+ `EncodeCurrentFrameToJpeg: JPEG encode failed`
→ **网页拍照 500、AI 拍照失败**；而**推流仍旧正常**（推流直接发 `fb->buf`，不经编码器）——
所以“视频好好的、拍照却 500”时要第一个查这里。
- **帧率/分辨率角标**：浏览器对 MJPEG `<img>` 不暴露逐帧事件、拿不到帧率 → 由**设备侧统计**（滚动 1 秒窗口）经**已有 WebSocket** 每秒推一次 `{"video":1,"fps":…,"w":…,"h":…}`，前端更新角标；停止时推 `{"video":0}` 收回角标。
- **帧率 20fps + 低延时三件套（FPV 遥控用途）**：这条功能的实际用途是**网页遥控机器人走位**（第一视角），
  **延时优先**；实测场景里开视频时不会同时做家里的 ESP-NOW 控制，所以不必为节点控制留空口。为此做了：
  1. 帧率上限 20fps（`local_video_stream.cc` 的 `s_target_fps`，默认 20，网页「⚙️ 视频设置」可改）。实际还受 OV2640 出帧能力限制（VGA JPEG 约 15~20fps），看画面角标即可。
  2. **禁用 Nagle**（`TCP_NODELAY`）：否则内核要攒够一个 MSS 才发，每帧白等几十毫秒；摇杆小包也不再被视频大帧拖在发送缓冲里。
  3. 取帧用 `CAMERA_GRAB_LATEST`，且**落后了不补帧**——宁可丢一帧，也不让队列堆积把延时越拖越长（遥控最忌延迟持续增长）。
- **与 ESP-NOW 节点控制的关系**：两者共用同一个 2.4G 射频，开视频时节点指令会被大帧排队拖慢。
  若确实要同时用（一边看视频一边管家里的节点），把帧率调低（如 12fps）给节点控制让出空口。
- **网页直接改参数（不用重烧固件）**：视频画面**左上角 ⚙️** 打开设置弹窗，改完即时生效：

  | 设置项 | 可选值 | 生效方式 |
  |---|---|---|
  | 分辨率 | 320×240 / 640×480 / 800×600 | **立即生效**（sensor 换分辨率，不重启相机）|

  > 四项参数都是**原地生效、画面不断流**，前端也不需要重连画面：MJPEG 流里每帧都是独立 JPEG（自带尺寸），
  > 浏览器是逐帧解码替换显示的。
  | 帧率上限 | 5 / 10 / 15 / 20 / 25 fps | **立即生效**（下一帧）|
  | 质量（JPEG）| 清晰 / 默认 / 标准 / 省流 | **立即生效**（只写 sensor 寄存器，不重启相机）|
  | 左右镜像 / 上下翻转 | 勾选 | **立即生效** |

  - 参数存**设备 NVS**（`video` 命名空间；镜像复用 `camera/flip`）：改一次永久有效，断电重启仍生效。
  - **为什么四项都能动态改**（依据驱动源码）：
    - 帧率：只是推流的软件节流（每帧算间隔），纯软件。
    - 镜像/翻转：`ov2640.c` 的 `set_hmirror`/`set_vflip` 只写 sensor 寄存器。
    - 画质：`ov2640.c:334` 的 `set_quality` 只写一行寄存器 `QS`，不碰缓冲。
    - 分辨率：**JPEG 模式下 DMA 缓冲固定 16KB**（`esp32s3/ll_cam.c` 的 `dma_half_buffer_cnt = 16` × 1024，与分辨率无关），
      帧缓冲 `fb_size = 宽×高/5` 按**初始化时**的分辨率算（`cam_hal.c:588`）。
      所以初始化就按**最大档 SVGA**（94KB PSRAM），之后用 sensor 的 `set_framesize`
      在 QVGA~SVGA 之间随便切，**不用重启相机**（只写 I2C 寄存器，下一帧生效）。
    - 反例（踩过的坑）：若按**当前**分辨率初始化再往大改，会触发 `cam_hal` 的 `FB-OVF`
      并 `ll_cam_stop()` 把相机停摆，所以必须按最大档初始化。
    - 代价：帧缓冲固定 94KB PSRAM（SVGA 档；旧实现只在推流时段才这么大，现在全程占用）。
      **内部 RAM 反而更省**：单一 JPEG 模式的 DMA 恒为 16KB（旧实现待机时是 RGB565 的 30KB）。
  - **镜像/翻转与 AI 工具 `self.camera.set_flip` 共用同一份 NVS**（位含义：bit0=左右镜像 bit1=上下翻转），
    所以网页改的、AI 改的、开机读回的是同一个值，不会两套配置打架。
  - 想再降延时：弹窗里把分辨率降到 320×240、或质量调「省流」（单帧越小传输越快，延时越低）。
- **没连设备也能用**：
  - 勾选「实时视频」**立刻出现视频框**（不等设备回应）；连不上时框里显示「接口不可用：未连接设备…」，
    不会出现“勾不上 / 空白 / 浏览器破图图标”。
  - ⚙️ 设置弹窗**离线也能打开**：用浏览器 localStorage 缓存上次保存的值回显（从未保存过则显示设备默认值
    640×480 / 20fps / 默认质量 / 不翻转）；设备连上后以设备为准。离线时点「应用」会提示“设置已暂存，
    连上设备后再点一次”。
- **同时只服务一个观众**：本板 `fb_count=1`（帧池只有一块），第二个连接直接返回 `503`。
- **关页面自动停流**：WS 客户端归零（关页面/断网）时板级自动 `VideoStreamStop()`。
  **这只是为了不白白抓帧**（省 CPU 与空口）—— 相机不用切模式，所以拖不拖流都不影响拍照。
- **接口**：WS action `{"action":"video_start"}` / `{"action":"video_stop"}`；流地址 `GET http://<设备IP>:81/stream`。
- **排查**：
  - 勾选后画面不出来 → 浏览器直接打开 `http://<设备IP>:81/stream` 试：能出图说明是前端问题；不出图看网页日志（级别开到「信息」）里 `LocalVideo` 的报错。
  - 拍照时日志出现 `Esp32Camera: JPEG preview decode failed` → JPEG 帧没解成预览图。
    **照片本身仍正常**（网页能看到、AI 也能识别），只是 LCD 上没那一张；先看上一行 `Captured frame: …` 的 `len` 是否正常。
  - **LCD 预览颜色反了（红蓝互换）** → `DecodeJpegPreview()` 里的 `swap_color_bytes`（0/1）与 LVGL 期望的字节序不一致，改成另一个试。
  - 日志 **每 ~4 秒成对**刷 `cam_hal: Failed to get frame: timeout` + `LocalVideo: fb_get failed`，
    且「停流→重开」也救不回来 → 不是网络/编码问题，而是驱动侧**已经没有可用帧**（本板 `fb_count=1`，
    那块帧被拍照链路借走后没归还，相机就此停摆，只能重启）—— 见踩坑 23（已由 `ReleaseCurrentFrame()` 修复）。

### ⚠️ PSRAM DMA 模式（`CONFIG_CAMERA_PSRAM_DMA`）：**实测不可用，不要开**

**背景（为什么曾想开它）**：相机 DMA 缓冲必须在**内部 SRAM** —— 实测 **RGB565 要 30720 字节、JPEG 只要 16384 字节**
（且与分辨率无关，推导见踩坑 22）；而本板内部 SRAM 空载只剩 20~25KB，旧实现“开视频切 JPEG、停流切回 RGB565”
在 deinit 之后就拿不到那块连续内存（真机日志：`cam_dma_config: DMA buffer 30720 Byte malloc failed,
the current largest free block:12800 Byte`）→ 相机停在不可用状态 → **拍照 / AI 拍照全部 500**（详见踩坑 22）。

> 2026-09 起本板改为**单一 JPEG 模式**（开机 init 一次，之后不再 deinit/Reinit），上述“切模式导致分配失败”的路径已不存在。
> 本节保留是因为它记录了一个**踩过的坑**：为省内存去开 PSRAM DMA，结果视频流完全不能用 —— “机理上说得通”不等于实测可行。
8MB PSRAM 表面上看帮不上忙：IDF 里 PSRAM 区域**不带 `MALLOC_CAP_DMA`**（`memory_layout.c`），
而 esp32-camera 的 DMA 缓冲写死用 `MALLOC_CAP_DMA` 分配（`cam_hal.c:522`）——
驱动另提供了一个“PSRAM 直采”模式来绕过它（`CONFIG_CAMERA_PSRAM_DMA`，组件 Kconfig 默认 `n`）。

**实测结论（2026-09，本板 OV2640 + IDF v6.0.2）：开启后实时视频完全不能用；关流后拍照仍然 500。已回退。**
该项默认 `n` 是有原因的：**不要开**。（`CAMERA_PSRAM_DMA_ENABLED = CONFIG_CAMERA_PSRAM_DMA`，
`cam_hal.c:58-64` —— 开了就真的会走 psram_mode，不是没生效。）

**它为什么在这里坏（源码层面的机理，供后人参考）**：
- 开了之后 JPEG 模式的 DMA 链从 **16 × 1024 字节**变成 **`recv_size / 1024` 个节点**
  （SVGA 时 `800×600/5 = 96000` → **93 个描述符**，`esp32s3/ll_cam.c` 的 `ll_cam_dma_sizes`），
  而且全部**直接链到 PSRAM 帧缓冲**（`cam_hal.c:510-516`），改由 GDMA 直接写 PSRAM。
- 对齐依赖 `ll_cam_get_dma_align()`：`16 << GDMA.channel[].in.conf1.in_ext_mem_bk_size`
  （`esp32s3/ll_cam.c:455`）—— 访问外部存储的 burst 配置没按预期生效时，
  PSRAM 侧的对齐 / cache 一致性就会出问题（本板 `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`）。
- 即：**“让 DMA 直接写 PSRAM”这条路在本板 + 本 IDF 版本下没走通**，不是配置写错。

**如何回退**（若已开）：

```
idf.py menuconfig → Component config → Camera configuration
  → [ ] Enable PSRAM DMA mode by default      # 取消勾选
idf.py build
```

回退后确认 `sdkconfig` 里是 `# CONFIG_CAMERA_PSRAM_DMA is not set`。

> ⚠️ 不要为了“记住这个设置”把它写进 `config.json` / `sdkconfig.defaults`：
> 它是**已验证会坏**的模式，写进持久入口等于把坑固化给以后的自己和别人。
> （它原本只存在本地 `sdkconfig` 里，而 `sdkconfig` 是构建生成物、不入库，所以回退后不会残留。）

**如果以后真要再试它**（需先接 USB 串口看 `cam_hal` / `ll_cam` 的报错）：
先只验证“开视频能不能出画面”，不要在同一轮里同时验证拍照；日记里重点看 `fb_get failed`、
`FB-OVF`、`cam_dma_config`、以及 “PSRAM DMA mode enabled” 这行后面跟的第一个错误。

## 网页拍照（按钮 + 页面显示照片）

「📷 照片」Tab 点 **📷 拍照** → 照片直接显示在按钮下方（同一张也会出现在 LCD 上，因为复用了带预览的抓帧路径）。
显示区右侧的 **🧹 清空** 只清页面上的这一张（隐藏显示区 + 去掉 `<img>` 的 src）——
**设备 PSRAM 里的 JPEG 和卡上的照片都不动**，删卡上照片用相册的「🗑 清空本相册」（两者很容易接错，已在单测里钉住）。

有 TF 卡时这张照片还会**存到卡上**（图片下方会显示「已存卡：`20260917_153002.jpg`（3/100）」），
并在当次相册选的是「网页拍照」时**自动刷新下方网格**（否则刚拍的这张要手动点「🔄 刷新」才出现）；
历史照片在同一个 Tab 的网格里翻看——见下方「照片相册」。

> 2026-09 之前拍照按钮/显示区在「🎮 机器人控制」面板，现已搬到「📷 照片」Tab：拍出来的照片本就存在这个 Tab 的相册里，
> 控件留在机器人面板会让人拍完还得切 Tab 才看得到。

- **接口**：`POST /photo/take` 触发抓帧+编码；`GET /photo.jpg` 取最近一次 JPEG。
- **实现**：`LocalPhotoCapture()`（`local_photo.cc`）→ `Esp32Camera::Capture()` 抓帧 →
  `EncodeCurrentFrameToJpeg()` 把这一帧写进 **PSRAM 常驻缓冲**（128KB 配额，VGA JPEG 通常 30~60KB）→ 页面用
  `<img src="/photo.jpg?t=时间戳">` 拉取显示。全程**不占用内部 SRAM**（相机的 JPEG 与输出缓冲都在 PSRAM）。
  本板相机直出 JPEG，所以这一步是**直通透传**（靠 `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT`，不再跑软件编码器）。
- **为什么不多开一块帧缓冲**：`cam_hal` 每帧需要一整块连续 **DMA 内部 RAM**（本板 JPEG 模式为 16384 字节，
  旧 RGB565 模式为 30720），`fb_count` 从 1 改成 2 会多占这么多内部 SRAM——本板本来就只有几十 KB、
  实测最大连续块只有 ~12800，不能这么花。因此改为**复用已捕获的那一帧**做输出
  （为此在 `Esp32Camera` 上新增了纯增量方法 `EncodeCurrentFrameToJpeg()`，不改任何现有函数，
  其它用同一份 `esp32_camera.cc` 的板子行为不变）。
- **LCD 预览**：`Capture()` 里对 JPEG 帧解码（`DecodeJpegPreview()`，1/2 缩放）挂给 LVGL，
  所以拍照时 LCD 上会出现这张图（用户明确要求保留这个行为）。
- **与 AI 拍照的关系**：两条路复用同一个 camera 驱动，设备侧已加**带超时的互斥**（300ms）。同一时刻点按钮又喊 AI 拍照，可能有一次失败（按钮弹「拍照失败」/AI 回报网络问题），**不会重启**，重试即可。
- **排查**：
  - 点了按钮没反应 → 看网页系统日志（级别调到「信息」）有没有 `Esp32Camera: Captured frame`；没有则是 httpd/互斥超时。
  - **颜色不对（红蓝互换）** → 两条链路分开查：
    - **照片（网页/AI 看到的）** 颜色反：直通上传不做任何颜色变换，所以只能是**sensor 侧**的
      `SetHMirror/SetVFlip` 或 ISP 配置不对；RGB565 时代的“编码源用错 `encode_buf_`”已不适用于本板。
    - **LCD 预览**颜色反：`DecodeJpegPreview()` 里的 `swap_color_bytes`（0/1）与 LVGL 期望的字节序不一致，换另一个值试一试（不影响照片）。
  - 照片是上一张 → 浏览器缓存：`/photo.jpg` 已带 `Cache-Control: no-store`，前端也加了时间戳；若仍出现请检查代理缓存。

## 照片相册（TF 卡留档 + 网页查看）

> 仅 TF 卡功能开启时生效（与本地音乐/上传页同开关 `CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD`）。

有 TF 卡时，拍的照片会**存到卡上**，并在网页新增的「📷 照片」Tab 里翻看。

- **目录分工**：网页按钮拍的 → `/sdcard/photos/`；AI 语音拍的 → `/sdcard/photos_ai/`。分开存，便于分别管理和清理。
- **保留量**：每目录最多 **100** 张（VGA JPEG 约 30~60KB，100 张 ≈ 5MB），**写卡成功后**超出部分自动删除最旧的——所以卡不会被照片塞满。
- **文件名**：时间已同步（联网 NTP 后）→ `20260917_153002.jpg`（一眼看出拍摄时间）；刚开机还没同步时 → `P_0001.jpg`（扫目录取最大编号+1），避免同一秒的照片互相覆盖。
- **AI 拍照存卡开关**：默认**开**。网页「📷 照片」里的「AI 拍照也存卡」勾选框，或对 AI 说「关掉 AI 拍照存卡」→ `self.camera.ai_save`（`on`: 1=开, 0=关）。设置写入 NVS（`photo/ai_save`），断电保留。
- **接口**（也可直接用 curl / 小程序）：
  - `GET /photos?kind=web|ai` → `{"ok":true,"kind":"web","ai_save":1,"count":37,"limit":100,"items":[{"name":…,"size":…,"mtime":…}]}`
  - `GET /photos/file?kind=web&name=20260917_153002.jpg` → 单张 JPEG（`no-store`）
  - `POST /photos` → `{"action":"delete","kind":"web","name":"…"}` / `{"action":"clear","kind":"web"}` / `{"action":"ai_save","on":1}` / `{"action":"status"}`
- **页面行为**：网格 **12 张/批**，滚到底自动加载下一批（另有「加载更多」按钮兼底）；点图放大（再点关闭）；每张右上角 `×` 删除；顶部可切「网页/AI」相册、显示 `n/100 张 · 占用`、清空本相册（二次确认）。
- **不插卡时**：拍照照旧（只存 PSRAM，页面显示当次那张），状态提示「仅显示（未插卡或写卡失败）」；相册 Tab 里没有照片。
- **实现**：板级 `photo_store.h/.cc`（目录、命名、写、列、删、滚动清理、开关）+ `http_upload_server.cc` 的 `/photos` 路由；AI 拍照留档靠共享 `Esp32Camera` 新增的**纯增量**观察者 `SetJpegObserver()`（默认空回调 → 其它共用 `esp32_camera.cc` 的板子行为不变），在拿到完整 JPEG 的回调里**当场同步写卡**。

### 排查（照片相关）

- **提示「仅显示（未插卡或写卡失败）」** → 卡没挂载或写入失败：先用「🎵 歌曲管理」确认能列出卡上文件（同一个挂载点）；卡满/只读也会这样。
- **相册里看不到刚拍的 AI 照片** → 先确认「AI 拍照也存卡」是勾上的；对 AI 说「打开 AI 拍照存卡」可恢复默认。
- **照片颜色反了** → 编码源问题，见「网页拍照」排查（RGB565 字节序在 `Capture()` 里就已处理好，编码时**不要**再换一次）。
- **张数超过 100** → 正常不会；若出现，是清理失败（如卡只读），删掉几张或检查卡。
- **一让 AI 拍照就重启** → 接串口看 backtrace，这是两种完全不同的死法（「共网拍照正常、
  只有 AI 拍照崩」是它们共同的症状：AI 拍照多了「上传给大模型解释」这条 HTTP 路径）：
  - `vApplicationStackOverflowHook` / `A stack overflow in task pthread` → 编码线程栈溢出，见踩坑 18；
  - `xQueueSemaphoreTake` / `OnTcpDisconnected` → ml307 `HttpClient` 析构竞态，见踩坑 19。
- **拍完照后实时视频再也出不了画**（照片本身正常）→ 驱动帧用完没归还，相机停摆：日志每 ~4 秒成对刷
  `cam_hal: Failed to get frame: timeout` + `LocalVideo: fb_get failed`，且「停流→重开」也无效，只能重启。
  见踩坑 23（已在 `ReleaseCurrentFrame()` 修复）。

## 待机全屏大时钟（AI 控制 + 多主题 + 本地持久化）

- 说「切换时钟模式 / 打开时钟 / 关闭时钟」→ `self.clock.set`（`mode`: `1`=开启, `0`=关闭, `-1`=切换开关）。
- 说「切换时钟颜色/主题」→ **同一工具 `self.clock.set`**（`theme`: `0`=黑底白字, `1`=白底黑字, `-1`=切下一个）。`mode` 与 `theme` 可只传其一，未传的参数保持当前不变；两者都传则同时生效。设置写入 NVS（`clock/theme`）+ `clock/mode`）。
- 说「现在是什么时钟主题/时钟开没开」→ `self.clock.current`，返回当前主题名称与是否开启（AI 可读回状态）。
- 开启后，**待机**状态下**整个屏幕**显示**粗壮高瘦数字大时钟**（`HH:MM`，24 小时制，不带秒，**垂直居中**）；时钟显示时状态栏/字幕条/表情区全部隐藏、屏幕底色换成时钟主题色，只留时间，像真实电子钟。对话/聆听/播放时自动隐藏时钟、恢复原 UI，不干扰字幕。（时间上方的**日期小字已隐藏**：`date_label_` 仍创建并更新文本、只是不显示，理由见踩坑 14。）
- **主题（2 套高对比）**：经典黑底白字 / 白底黑字，切换立即生效并入 NVS，断电重启保留。（仅保留两种高对比经典模式，其余 Catppuccin 系均为纯颜色变化、无实际意义，已移除。）
- **多屏自适应（字号按屏幕自动选档）**：三档内嵌 Bebas Neue 字体（48/60/130px，**数字等宽**），`SetupUI` 时按编译期屏幕宽度 `DISPLAY_WIDTH` 从最大往下选能放下 `HH:MM` 的档——覆盖 240×240 / 240×320（→130px，两侧仅余 4px、几乎占满全屏、字高约 93px 高瘦占满）/ 128×160 横屏（→48px）。因数字等宽，任何时间 HH:MM 宽度恒定，不会因窄数字增大两侧留白，"始终占满两边"。
- 时间来自联网后同步的系统时钟（与绝对闹钟同源）；未同步前不显示。
- 设置写入 NVS（`clock/mode` + `clock/theme`），**断电重启自动恢复**。
- 实现：板级显示子类 `airobot_lcd_display.h`（`AirobotLcdDisplay : SpiLcdDisplay`，标准 `SetupUI()` 钩子叠加 LVGL 标签，不改核心 display 代码）+ 1 秒 `esp_timer` 刷新（仅在文本/日期变化时更新标签，省 SPI 刷屏）。字体内嵌见下方「时钟字体说明」。

### 真机验证要点

1. 说「打开时钟」→ 待机时屏幕出现大号 `HH:MM` 时间（分钟正常跳动、垂直居中），**不再显示日期**，状态栏/字幕条隐藏。
2. 说「切换时钟颜色/主题」→ 黑底白字 ↔ 白底黑字 两个经典主题循环切换，立即生效。
3. 说「切换时钟模式」→ 时钟消失、原 UI 恢复；再说一次 → 恢复。
4. 开启时钟后唤醒对话/播放音乐 → 时钟隐藏，结束后回到待机自动恢复显示。
5. 开启时钟 + 切换主题后断电重启 → 均保持（NVS 持久化生效）。

## 网络状态（AI 可读本机 IP/SSID/信号）

- 说「当前 IP 是多少 / 连的哪个 WiFi / 信号好不好」→ `self.network.get_status`，返回 JSON：`ip`(局域网 IPv4)、`connected`(是否已连接)、`ssid`(WiFi 名)、`rssi`(信号原始值 dBm)、`signal`(strong/medium/weak，按 rssi>= -60/ -70 划分)。未连接时 `ip`/`ssid` 为空。
- 用途：AI 引导用户访问本机 Web（如 `http://<ip>/`）、排查网络、判断设备是否在线。
- 工具为板级可扩展入口（后续可加 `channel`/`mac` 等字段）。

## ESP-NOW 居家设备（自动发现 + 数据驱动，比赛演示）

本板作为**主控**，通过 ESP-NOW 与 1~2 个自制 **ESP32-S3 节点**通信。

**核心设计：节点自描述能力，主控只做“存起来 + 转给 AI”。**
主控固件里**没有任何**设备名、能力名、事件名或播报文件名——节点上线时上报
“我叫什么、有什么能力、怎么调”（`@n1 info 客厅灯 light(RGB灯):on(0\|1),rgb(r,g,b)…`），
主控存进内存注册表，AI 通过三个通用工具查看与控制。
**因此接入一个新设备只需改节点固件，主控零改动、无需重新烧录。**

节点固件在 `arduino/EspNowNode/`（Arduino 架构，用核心自带 `ESP_NOW` 类，控灯用核心自带 `ledc`，
**零第三方库**；仅玄关/我的家的 DHT11 需要 `DHT sensor library`）。接线/编译/排错详见该目录的 `README.md`。

### 演示剧本（现场三个场景）

| 场景 | 你的操作 | 设备反应 |
|---|---|---|
| ① 语音控灯 | 「打开客厅灯」「调成蓝色」「暗一点」「关灯」 | 节点 1 的 RGB 灯亮/变色/调光/灭 |
| ② 人来自动开灯 ★ | 手靠近超声波（<30cm），然后拿开 | 灯自动亮 + 喇叭播报「检测到有人靠近，已为你开灯」；**灯本来就亮着时不重复播报、也不动灯**（避免播报说谎）；**人走 30 秒后自动灭** |
| ③ 环境查询 + 门禁 | 「室内多少度」；手靠近红外避障模块 | 返回节点 2 温湿度；播报「门口有人经过，请注意」 |

②③ 不需要说话，是**节点自己判定并主动触发**，现场最有观赏性。
（高温同理：节点 2 侧 ≥28℃ 播一次、≤26℃ 复位——阈值属于节点的业务语义，主控不参与判断。）
这些“自动动作”（如人来自动开灯）都写在**节点固件**里，主控完全不知道。

> **只有一块板时**：把节点固件烧成 **`NODE_ID 3`（我的家）**——一台设备同时接全部四个传感器
> （RGB + 超声波 + DHT11 + 红外避障，后两个因 GPIO4/5 已被 RGB 占用而改用 GPIO16/17），
> 上面三个场景能在一块板子上全部演完。接线见 `arduino/EspNowNode/wiring-node3.svg`。

### AI 语音指令（服务端通过 MCP 工具自动调用）

工具只有三个，且都是**语义无关**的通用工具——AI 通过 `self.home.devices` 拿到设备清单后决定怎么调：

| 你对助手说的话 | 触发工具 | 说明 |
|---|---|---|
| 「家里有哪些设备 / 客厅灯能做什么」 | `self.home.devices` | 返回所有节点的自描述清单（名字/能力/参数说明/最新状态/在线） |
| 「打开客厅灯 / 关灯」 | `self.home.control(id=1, cap=light, action=on, args=1)` | 透传给节点执行 |
| 「调成蓝色 / 暗一点」 | `self.home.control(id=1, cap=light, action=rgb, args="0 0 255")` | 同上 |
| 「室内多少度」 | `self.home.devices`（读 `state` 里的 `temp`）或 `control(cap=temp, action=read)` | 读数由节点周期上报 |
| 「居家节点在线吗」 | `self.home.devices` | 看 `online` 字段 |
| （自测用） | `self.home.announce(name=motion)` | 手动播一次 `motion.mp3` |

> `self.home.control` **调用失败时会在返回值里直接附上设备清单**，AI 一步即可自愈，
> 所以它不需要“先查后控”两步走。

### 播报音频（本地预录 MP3）

设备侧**无法自行生成 TTS**（协议层只有 wake word/listening/abort 这几个主动发送接口，没有"设备主动请求播报"；
`NotifyPlayer` 播的是**服务端下发的音频 URL**），所以事件播报走**本地预录 MP3**，复用现成的 TF 卡播放链路：

- 目录：**`/sdcard/announce/`**（独立于 `/sdcard/music/`，因此不会被 `self.music.list` 当成歌曲列出）；
  **主控启动后第一次扫描歌曲时会自动创建这个目录**——旧固件的上传一律失败就是因为它不存在
  （`fopen(path,"w")` 对不存在的目录永远失败），所以插卡前先在电脑上建好目录也行，但不需要了；
- **文件名由节点决定**：节点发 `@n1 say <名字>`，主控就播 `/sdcard/announce/<名字>.mp3`，
  文件不存在则静默跳过。主控**没有任何“事件名 → 文件名”的映射表**——
  以后接个烟雾传感器，节点发 `say smoke`、卡上放 `smoke.mp3` 即可生效，主控零改动。
  当前节点用到的名字：`motion`（有人靠近，并自动开灯）、`beam`（红外避障检测到障碍）、`hot`（温度偏高）；
- 生成：本板 `scripts/gen_announce_mp3.sh`（macOS `say -v Tingting` + `ffmpeg` → **24kHz 单声道 96kbps**，
  与网页上传的转码规范一致），也可以自己录真人声直接覆盖同名文件；
- 只在**待机**状态播，打断判定只看**用户交互**（`Listening`，以及**非播报期间**的 `Connecting`）：
  即**网络重连/服务端抖动不会掐断正在播的音频**（现场“播到一半就没了”的来源），播完才停；
- 冷却按 **(节点, 音频名)** 记（8 槽环形表，不是按节点一刀切）：我的家连续上报 `motion`/`beam` 时
  两条音频互不压制，同一路事件 10 秒内只播一次；没插卡/没放音频则跳过播放。
  跳过的原因会打在 **`ESP-NOW`** 这个 TAG 上（见下方「排错」表），不再静默：
  `播报失败: 打不开 /sdcard/announce/motion.mp3 (文件不存在?)` / `播报跳过: 设备忙(非待机)` /
  `播报跳过: 冷却中(还剩 N ms)` / `播报开始: motion.mp3`。

> **⚠ “从来没播报过、也没有任何日志”的真正原因（已修）**：播报路径原来直接判 `music_player_`
> 这个成员是不是空，而它**只在“放过歌 / 闹钟响过 / 网页上传过文件”之后才被懒创建** ——
> 插卡拷好提示音、开机直接挥手触发时它一直是空的，每次都静默跳过；而那条“播报跳过”日志是
> WARN 级、网页日志默认级别是 ERROR，所以**连日志都看不到**，现场看就是“这功能完全没实现”。
> 现在播报走 `GetMusicPlayer()` 懒创建（第一次播报顺带建目录 + 扫卡），失败一律按 **ERROR** 打；
> 并且会顺手把 `/sdcard/announce/` 里**实际有哪些文件**列出来
> （`提示音目录 /sdcard/announce: 2 个文件 motion.mp3 beam.mp3`）——
> 名字拼错、传到别的目录、根本没上传，一眼就能分辨。
- 实现：`LocalMusicPlayer::PlayAnnounce()`（**纯增量**：独立目录 + `pending_path_` 绝对路径优先 +
  清空歌曲队列，播完即停，不改动原有播放逻辑）。

### 节点侧约定

| 功能 | GPIO | 说明 |
|---|---|---|
| RGB 模块 R / G / B | 4 / 5 / 6 | 4 线模块；共阳模块把 `RGB_COMMON_ANODE` 设为 1 |
| HC-SR04P Trig / Echo | 7 / 15 | **必须 3.3V 版（型号带 P）**；5V 版 Echo 会损伤芯片 |
| DHT11 DATA | 4 | 3 线模块，自带上拉 |
| 红外避障模块 OUT | 5 | **低电平 = 检测到障碍**；反射式 2~30cm（极性见 `OBSTACLE_ACTIVE_LOW`）|

- 协议与 Arduino 下位机**同构**（`@` 前缀文本行）：

  | 方向 | 报文 | 用途 |
  |---|---|---|
  | 节点→主控 | `@n1 info 客厅灯 light(RGB灯):on(0\|1),rgb(r,g,b);dist(距离cm):read()` | 能力自描述（锁信道后 + 每次收到 beacon 重报） |
  | 主控→节点 | `@n1#42 do light rgb 0 0 255` | 通用动作，主控原样透传、不解释；`#42` 是**序号信封** |
  | 节点→主控 | `@n1#42 ok light 0 0 255` | 执行回执（进状态缓存，并给主控销掉该序号） |
  | 节点→主控 | `@n1 say motion` | 请求播报 `motion.mp3` |
  | 节点→主控 | `@n1 evt temp 26 55` | 状态上报（只进状态缓存，**不播报**） |
  | 节点→主控 | `@n1 evt hb 1` | **5 秒心跳**（只刷新在线时间 + 推动未确认命令补发，不进 state、不回调业务） |
  | 节点→主控 | `@n1 err unknown-cap light` | 错误 |

  > **序号信封（向后兼容）**：序号写在**信封**里（`@n1#42 …`）而不是正文，所以
  > 旧节点 `atoi("1#42 …")` 仍然得到 `1`、正文一个字节没变——**两侧任一没更新都不会瘫痪**（`#0`/无序号
  > 即退化为旧的“发了就算”语义）。用途是给下行命令做**应用层 ACK**（官方 `esp_now.rst` 的建议做法：
  > 应答超时即重传 + 用序号删重复），因为 `esp_now_send()` 返回 `ESP_OK` 只代表“进了驱动队列”，
  > 不代表对方收到了。
- **节点零配置**：不写 SSID/密码/主控 MAC/信道。主控每 500ms 广播一条 `@beacon`（30 秒后转 3 秒稳态），
  节点在信道 1..13 间 hop（每信道停 500ms），收到即锁定当前信道；失联 5 秒自动回 hop
  （**换热点、双方重启都自恢复，无需重烧固件**）。
- **主控侧信道看护**：AP 自动换信道（ACS）是路由器行为，改不了，但主控**每秒查一次**当前信道
  （`esp_wifi_get_channel`），发现变了就立刻进入 **8 秒快速 beacon 窗口**（500ms 一条）并打一条
  `链路事件(channel)`；另外命令连续 3 次发不出去（`esp_now_register_send_cb` 回报失败）也会触发同样的提速
  （`链路事件(txfail)`）。节点侧失联 5 秒回 hop，两边叠加，通常 1~2 秒内重锁。
  这些都**不额外常驻任务**：信道看护蹭 beacon 定时器，重传定时器只在有未确认命令时才存在。
  （保持信道不用 `esp_now_remain_on_channel`/`esp_now_switch_channel_tx`：官方无使用约束说明，
  且离开 AP 信道会丢 AP 包、影响音频流，风险大于收益。）
- **密钥**：`kPmk`/`kLmk` 必须与节点固件一致（`espnow_home.cc` ↔ `EspNowNode.ino`，各 16 字节）。
  不一致的现象是**完全收不到任何包**（不是偶发失败），排查时优先核对这两处。

### 接入一个新设备（5 步）

**主控完全不用动**，只写节点固件：

1. **填能力表**（`EspNowNode.ino` 顶部）：设备名 + 每个能力的名字与规格
   ```cpp
   static const CapDef kCaps[] = {
       {"fan", "(风扇):on(0|1),speed(0-100)", capFan},   // 可触发的动作配 handler
       {"co2", "(CO2 ppm,只读):read()",        capCo2Read},
   };
   static const char kNodeName[] = "书房风扇";
   ```
2. **写 handler**：`bool capFan(action, args, out, out_len)` —— 分辨 `on`/`speed` 等动作，
   把结果文本写进 `out`（`ok` 回执里会带回主控）。
3. **配 GPIO 与周期上报**：读传感器的代码放在 `sonarTick()` 同级位置；
   需要播报就 `queueSay("smoke")`，只需更新状态就 `queueEvt("co2", "800")`。
4. **烧录**：`arduino-cli compile/upload --fqbn esp32:esp32:esp32s3`（把 `NODE_ID` 改成未占用的编号）。
5. **上电等 3 秒**：主控收到 `info` 后自动出现在 `self.home.devices` 里，直接对它说话即可。

> 播报音频：把 `smoke.mp3` 之类放进 TF 卡 `/sdcard/announce/`，名字与 `queueSay()` 的参数一致。
> 能力规格要克制（单包 ≤200B）：节点侧超长会被截断，主控只保留前 4 个能力。

### 设计取舍（改这块前先读）

1. **上行连发 3 次 + 下行应用层 ACK 重传**（两者都非阻塞）：主控待机是 `WIFI_PS_MAX_MODEM`、只在 DTIM 醒来，
   节点单包上报会被漏掉 → 节点连发 3 次（间隔 150ms）+ 主控按 `(kind, 名字)` 1 秒去重
   （`say` 与 `evt` 是两条通道，即使同名也互不吞）；
   下行**不能在 MCP 工具回调里 sleep 着连发**（会阻塞约 300ms，直接卡住对话），
   所以改成登记后立即返回、由 `esp_timer` 驱动重发：`SendTo()` 给命令编序号（`@n1#42 do …`）
   并登记 pending（每节点最多 1 条），**首包之后最多重发 4 次、间隔 150ms**，节点回 `ok#42` 即销账；
   **7 秒**（= 节点跳一圈 6.5 秒 + 余量）仍未确认就报 `链路事件(cmdfail)`，并给调用方一个明确的
   “已下发但未确认”。为什么不设更长：ESP-NOW 的价值就是实时，超过“节点跳完一圈”还在等，
   说明节点是真不在（断电/密钥不符），继续等只会让“几秒后灯突然亮”这种事后生效更迷惑。
   正常链路下的响应时间不受影响：仍是首包直达 + 节点十几毫秒回执。
   重传定时器周期 100ms 且**只在有在途命令时运行**，发完就销毁；
   收到该节点任何上行（含 `hb` 心跳）会把它的下次重发提前到现在，等价于“信道刚对上，抓紧补发”。
   节点侧上行是 **6 槽队列 + 同键合并**（键 = `@n<id> <kind> <名字>`，同键只留最新）：
   否则 `dist` 这类高频状态会占满队列、把 `say` 挤掉（现场“播报时有时无”）；
   我的家有 4 路状态（dist/motion/temp/beam），槽位数必须留得下 `say`/`ok`。
2. **不为 ESP-NOW 全局提频**：待机省电是刻意设计（见踩坑 7），不改 `SetPowerSaveLevel()`；
   若现场实测仍丢包，再调 `esp_now_set_wake_window()`（IDF v6 API，默认最大窗口）。
3. **主控侧传输层零日志**（`espnow_home.cc` 内无任何 `ESP_LOG`）：本板 UART0 与 Arduino 指令共用。
   **但排查节点问题必须看节点自己的串口**：节点是独立 ESP32-S3、串口独占，日志默认开（115200），
   `[信道] 扫描中…` → `[信道] 已锁定主控` → `[收到]` → `[发送]` 四步即可定位（日志已中文化，
   各行的含义见该目录 README 的「日志行怎么读」）。
4. **抗抖动留在节点侧**：超声波 30/40cm 迟滞、红外避障两次采样一致、距离变化 ≥3cm 才上报。
5. **主控不存业务语义**：传感器字段、能力名、事件名、播报文件名、温度阈值**一律留在节点侧**。
   状态在传输层只是一个通用的 `key=value` 文本（`temp=26 55 light=1 0 0 255 err=dht`），
   主控只负责存与转发。DHT 读失败时节点上报 `evt err dht`，**旧读数仍留在 state 里**，
   AI 能看到“温度 26℃ 但最近一次读取失败”，比清空读数更有用。
   （DHT11 是单总线，时序会被 WiFi 中断干扰；若读数不稳可换 I2C 的 SHT30/DHT20，
   只需改节点 10 行读取函数，主控与协议都不变。）
6. **不做“每个能力注册成独立 MCP 工具”**（这是本项目最初的想法，调研后放弃）：
   ① 设备侧 `McpServer` **没有** `RemoveTool`，`tools_` 只增不减，节点离线后工具撤不回来；
   ② 工具列表由云端在连接初始化后**主动拉取一次**（`docs/mcp-protocol.md:234`），
   而节点 hop 找信道要 2~3 秒，晚于拉取时刻上线的工具**云端根本看不到**。
   结果就是“演示现场 AI 说没有这个设备”。改成“固定通用工具 + 运行时设备清单”后，
   AI 每次调用拿到的清单都是实时的，彻底绕开了服务端的拉取时机。

### 真机验证要点

1. 节点上电 → 3 秒内 `self.home.devices` 显示该设备 `online: true` 且 `info_seen: true`；
2. 「打开客厅灯」→ 灯亮；「调成蓝色」→ 变蓝；「关灯」→ 灭；
3. 手靠近超声波 <30cm → 灯**自动亮** + 播报（10 秒内不重复）；人拿开后 30 秒灯**自动灭**
   （但先说了「开灯」再靠近、或亮灯期间又调过颜色 → 不再自动关，控制权已交回人工）；
   再手靠近一次（灯已亮着）→ **灯不动、也不播报**（播报只跟“真的自动开了灯”走）；
4. 「室内多少度」→ 与实物温度计接近（DHT11 ±2℃）；
5. 手靠近红外避障模块 → 播报；温度 ≥28℃ 播报一次（降到 ≤26℃ 之后才能再播）；
6. 「开灯」后立刻拔掉节点电源再插回 → 命令不会凭空消失：主控最多重发 4 次，节点回 hop 重锁后
   在下一次收到主控包时补发；反过来节点跳信道期间连点两次「开灯/关灯」，最终状态以最后一次为准；
7. 网页日志 `ESP-NOW` TAG：正常一次控制应看到 `收到节点N消息: kind=ok …`；
   只有 `链路事件(cmdfail)` 才代表 7 秒没确认（节点断电/密钥不符）；
8. 拔节点电源 30 秒再插 → 自动恢复在线（`info` 重报，能力不丢）；
9. 主控换热点（信道变化）→ 节点自动重锁定，**无需重烧固件**；
10. **主控重启** → 节点在下一轮 beacon 后重报能力，清单自愈；
11. 全程 AI 对话不卡顿、无重启；`free sram` 不低于改动前。本次改动内存账单：**运行时零新增堆分配**
    （链路事件与播报失败路径原本要构造 `std::string`，现全部改定长 `char` 缓冲 + `const char*`；
    报错路径往往正是内存紧张的时机，绝不在那里做堆分配），静态 `.bss` 仅增约 **0.8KB**
    （4 槽在途命令表 ≈0.5KB + 8 槽播报冷却表 ≈0.26KB），不占堆、不随运行增长；
12. 连续跑一整晚不重启（碎片敏感场景）：在「自我检查」里盯 heap 最低水位；
13. 拔掉 TF 卡 → 播报静默跳过，控制与查询仍正常。

### 排错

| 现象 | 检查 |
|---|---|
| 设备没出现在清单里 | 节点是否上电（hop 找信道需约 3 秒）；`self.home.devices` 里若 `info_seen=false`，说明节点没发 `info`（节点固件是否是新版） |
| 一直离线 | 两侧密钥是否一致；节点是否被同信道的 2.4G 流量干扰 |
| AI 说没有这个能力 | 看 `self.home.devices` 里该设备的 `caps`；节点能力表里的能力名是否拼写一致（**大小写敏感**） |
| `state` 里出现 `err=unknown-cap` | 节点能力表里没有这个名字，或主控发的能力名拼错 |
| `state` 里出现 `err=unknown-action` | 动作名不在该能力的规格里（`spec` 要写全，如 `on(0\|1),rgb(r,g,b)`） |
| `err=unknown-action`，但动作名**明明在规格里**（回执里还带乱码，如 `unknown-action offxV??`）| **节点侧收包没补 `'\0'`**：ESP-NOW 回调给的是「原始字节 + 长度」，没有字符串终止符，而节点用 C 字符串函数解析 → 越过包尾读到驱动缓冲残留字节；**只有最后一个字段**（`off`/`on`/`read` 这类无参数动作）会被污染，带参数的 `rgb`/`bright` 反而正常 → 表现为「调色一直好、关灯偶发失效」。重烧节点固件即可（见踩坑 21 与节点 README「分隔符约定」）|
| 播报不响 | 看网页日志的 **`ESP-NOW`** TAG（该 TAG 单独放开到 INFO，**只要发生就一定能在网页看到，不用调级别**）：① `收到节点N消息: kind=say name=motion` → 上行到了；② 若出现 `提示音目录 /sdcard/announce: N 个文件 …`，对照里面有没有 `motion.mp3`（没有 = 没上传 / 名字不一致 / 传到别的目录）；③ `播报失败: 打不开 …` → 文件不在卡上；④ `播报跳过: 设备忙(非待机)` → 正在对话/播音乐；⑤ `播报跳过: 冷却中` → 10 秒冷却未过；⑥ `播报开始: motion.mp3` → 播放在走，问题在音频输出链路（喇叭/音量）。**连 `收到节点N消息` 都没有 = 上行没到主控**。也可用 `self.home.announce(name=motion)` 手动播一次做二分：能响 → 播放链路没问题，问题在上行；不响 → 按 ②③ 查文件 |
| 播报刷屏 | 检查冷却（10 秒）；节点是否把高频状态也走了 `say`（高频项应走 `evt`） |
| 温度有值但读取失败 | `state` 里同时有 `temp=..` 和 `err=dht` 属正常（保留旧读数），检查 DHT11 接线/供电 |
| 网页日志查不出节点问题 | 主控侧只看 **`ESP-NOW`** TAG（`收到节点N消息`）；节点侧的收发细节仍需**看节点串口**（`[信道] 扫描中…`=没锁定/密钥、`[信道] 已锁定主控`=已锁定、`[收到]`=收到指令、`[发送]`=已回执），见节点 README「日志行怎么读」 |
| **反复掉线**（AI 提示「节点已掉线」） | 先看网页日志 `ESP-NOW` TAG 的 `链路事件(...)`：`链路事件(channel)` = 检测到 AP 换信道，已自动进入 8 秒快速 beacon；`链路事件(txfail)` = 命令连续 3 次发不出去，同样提速。节点侧每 5 秒 `evt hb 1` 心跳，主控 `kOfflineMs`（15 秒）内收到任何包都算在线——所以偶发一次“离线”提示不再等于真的掉线。若仍频繁，再看路由器是否信道跳变过频（ACS），**固定到 6/11 只是“少重锁几次”的优化，不再是必需** |
| **命令偶发要等很久 / 没反应** | 命令现在带序号 + 应用层 ACK：主控重发最多 4 次（150ms 一次），节点执行后回 `ok#<seq>`；节点重锁信道后收到主控任何包都会触发补发。看 `链路事件(cmdfail)` 判断是否 7 秒都没确认（那才是真没送达：节点断电、密钥不符）；正常链路下命令到达节点仍是 5~30ms |
| 设备永远不上线（主控重启循环）| `esp_now_init()` 必须在 WiFi 就绪**之后**调用，否则 `LoadProhibited` 重启循环；板级已用每秒轮询（`OnEspNowWifiWait`）等到 WiFi 拿到 IP 再启动 |

## AI 闹钟提醒（AI 语音 + 网页 + TF 卡持久化）

> 仅 TF 卡功能开启时生效（与本地音乐/上传页同开关 `CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD`）。

本板支持用 **AI 语音**设置闹钟/定时提醒，也可在 **web 页面**（复用 `http://<设备IP>/` 上传页）查看/新增/删除闹钟，到点后**自动播放本地歌曲响铃 + 屏幕显示提醒内容**（卡上无歌时退回内置提示音）。

### 闹钟类型

| 类型 | 触发方式 | 说明 |
|------|----------|------|
| `relative` | 从现在起 N 分钟后（一次性） | 如「30分钟后提醒我喝水」，重启后重新计时 |
| `absolute` | 每天 HH:MM（每天重复） | 如「每天 7:00 叫我起床」；若设置时刻当天已过，则次日触发 |

### AI 语音指令（服务端通过 MCP 工具自动调用）

| 你对助手说的话 | 触发工具 | 效果 |
|--------|---------|------|
| 「30分钟后提醒我喝水」 | `self.alarm.set(type=relative, value=30, label=喝水)` | 创建一个 30 分钟后的相对闹钟，返回编号 |
| 「每天 7:00 叫我」 | `self.alarm.set(type=absolute, value=07:00, label=起床)` | 创建每天 7:00 的闹钟 |
| 「列出闹钟 / 有什么提醒」 | `self.alarm.list` | 返回所有闹钟 JSON 数组（id/type/trigger_sec/label/enabled） |
| 「删除 3 号闹钟」 | `self.alarm.remove(id=3)` | 删除指定编号闹钟 |

### web 页面管理

- 待机状态屏幕底部显示 IP（如 `192.168.31.74`），浏览器打开 `http://<设备IP>/`。
- 页面「⏰ 闹钟提醒」区：点「➕ 新建闹钟」弹出对话框（与上传歌曲同一套弹窗样式），填类型/触发值/内容/铃声后「添加」；列表列出所有闹钟，每张卡片可「删除」。
- **移动端**：屏宽 ≤620px 时，歌曲与闹钟表格自动变成**卡片列表**（每行一张卡，左侧显示列名，列名来自各 `<td>` 的 `data-label`；删除按钮独占一行方便点按），不再需要横向滚动。新增 `<td>` 时记得带 `data-label`，否则手机上看不到该列名字。
- **长按不弹「复制」菜单**：`user-select: none !important` 写在 `*` 上（**不是靠 `body` 继承** —— 实测普通权重压不住 UA 样式，按钮/文案长按照样弹菜单），再拦 `contextmenu` 与 `selectstart`（Android 不认 `-webkit-touch-callout`，那是 iOS 私有属性；部分国产内核长按连 `contextmenu` 都不派发）。**例外**：表单控件保留 `user-select: text !important` —— 要输入、要粘贴。
  - **日志/指令记录区不再支持长按复制**，改用「📋 复制」按钮（`copyPre()`）。本页是 http，非安全上下文下 `navigator.clipboard` 不可用，所以走 `document.execCommand('copy')`。
  - ⚠️ 若某些浏览器自带长按菜单（国产 X5 / UC / QQ 内核常见，不遵网页 CSS），网页层拦不住，需要在浏览器设置里关掉「长按菜单 / 快捷操作」，或换 Chrome。
- 页面通过 `/alarm?action=list`（GET）与 `/alarm`（POST `add` / `remove`）读写闹钟，与 AI 语音共用同一份数据。

### 持久化与数据结构

- 闹钟以 JSON 文件保存在 TF 卡：**`/sdcard/alarms.json`**（断电重启不丢）。
- 每条数据：`{"id":1, "type":"relative", "trigger_sec":1800, "label":"喝水", "enabled":true}`；绝对闹钟额外含 `last_fired_day`（上次触发的天序号，避免当天重复）。

### 到点响铃（音乐闹钟）

- 到点触发时：**先打断正在播放的本地音乐** → **自动随机播放本地歌曲作为铃声**（音乐闹钟，复用「随机播放」链路：声音明显且持续，唤醒词/按钮/说停均可立即打断；随机队列播完且无人打断时自动停回待命）→ **屏幕显示提醒内容**（无 label 时显示「⏰ 闹钟提醒」）。
- **卡上无歌或播放启动失败**时退回单声内置提示音（OGG_POPUP），保证提醒不落空。
- 走零联网方案（本地音乐 + 屏幕显示），稳定可靠；后续可扩展为服务端 TTS 播报个性化语音或指定铃声。

### 实现说明

- 全部逻辑在板级：`alarm_manager.h` / `alarm_manager.cc`（闹钟存储、到点判断、后台检查线程），由 CMake `file(GLOB)` 自动编译；`http_upload_server.*` 提供 `/alarm` REST 接口与嵌入页面。
- 后台检查线程每 `ALARM_CHECK_INTERVAL_MS`（默认 1 秒）扫描一次，到点后通过 `Application::Schedule` 切回主任务再播报（避免跨任务操作音频/显示）。
- 相对闹钟用 `esp_timer` 单调时钟计时（从创建时刻起算）；绝对闹钟依赖小智联网后同步的系统时钟（`time()`/`localtime()`）。

### 真机验证要点

1. 设一个 1 分钟的临时相对闹钟 → 到点自动响起本地歌曲（唤醒说一句话可打断）+ 屏幕显示提醒内容。
2. 设置闹钟后断电重启 → `self.alarm.list` 仍能查到（`/sdcard/alarms.json` 存在且正确）。
3. 网页打开 `http://<设备IP>/` → 可看到闹钟列表，新增/删除后设备端 `self.alarm.list` 同步。
4. 绝对闹钟（设一个当天下一个未到时刻）→ 到点触发，且当天重复设置不重复响。

## 时钟字体（LVGL 字体生成与更换）

### 时钟字体说明（内嵌，无需任何 menuconfig 配置）

时间/日期使用**板内嵌 Bebas Neue 等宽高瘦数字字体**（`clock_bebas_130/60/48.c` 时间大字，`clock_bebas_date.c` 日期小字，仅含 `0-9` `:` `-` 字形，Bebas Neue 风格）：

- 字体为开源 **Bebas Neue**（Google Fonts，SIL OFL）。高瘦(condensed)标题型无衬线字体，且**数字严格等宽**（0-9 与冒号每字符 adw 相同）——保证任何时间 HH:MM 宽度恒定，不会因窄数字(如 '1')让时间变窄、两侧留白忽大忽小，实现"始终占满两边"。高瘦字形让同屏宽能上更大字号、字更高，"长方形"视觉更显高大。
- 全部按 `lv_font_conv --bpp 2` 生成（**bpp2 抗锯齿**），未做任何手工加粗/拉伸/收紧字距（**不再有** `scripts/thicken_clock_font.py` / `stretch_clock_font_76.py`）。
- 字形数据是 `const` 数组，放 **flash**（`.rodata`），LVGL 按需从 flash 读取位图，**运行时不占 RAM**。
- **档位宏门控**（`clock_fonts_config.h`）：按屏幕 `DISPLAY_WIDTH` 编译期只启用本屏用到的档，其余档位 `.c` 内容为 `#if 0` 空、**不占 flash**。
- 仓库不存字体源文件，只存生成的位图 `clock_bebas_*.c`。

> ⚠️ **横/竖屏是编译期决定**（`config.h` 的 `DISPLAY_SWAP_XY`）。想横屏：把对应分支的 `DISPLAY_WIDTH`/`DISPLAY_HEIGHT` 一并改成横的值（如 128×160 屏横屏 → `WIDTH=160, HEIGHT=128, SWAP_XY=true`），**不是**只改 `SWAP_XY`。重编后生效。

若以后想**换字体 / 调字号 / 换样式**，只需重新生成**同名**字体文件即可——**不改任何显示逻辑**（`PickClockFont()` 运行时实测 `88:88` 宽度自适应选档）、**不改 CMakeLists**、**不用清 build**。

### ▶ 关键原则：文件名固定，只改内容

所有字体**文件名已经固定**（`clock_bebas_130/60/48.c` 时间 + `clock_bebas_date.c` 日期），代表「大屏主档 / 中档 / 小屏兜底 / 日期」四个**角色**，**不随字号或字体变**。换字体/改字号时，只把**新内容写进同名的 `.c`**（文件名不变 → `file(GLOB)` 列表不变 → CMake 不重扫 → 永不删 build、永不改逻辑）。

> ⚠️ **千万不要给字体文件改新名字**（如新建 `clock_xxx_140.c`）。一旦文件名变了，GLOB 列表就变，需要清 build 才能让 CMake 重新扫描。

### ▶ 换字体完整流程（约 5 步）

**1. 准备源字体**（开源、SIL OFL，任选粗壮/等宽数字字体）：
```bash
npm i @fontsource/<字体名>        # 例: @fontsource/anton / archivo-black / bebas-neue

# 源字体文件: node_modules/@fontsource/<字体名>/files/<字体名>-latin-400-normal.woff
```

**2. 确定各档该用多大字号**（不同字体宽高比不同，不能沿用旧字号！）：
换字体后用旧字号很可能放不下 240 宽（或偏小）。用 `lv_font_conv` 生成一个临时档，实测 `88:88` 宽度，选出**刚好放得下 240 宽**（avail = `DISPLAY_WIDTH - 4`）的最大字号：
```bash

# 先用某字号试生成, 读时钟文件里数字/冒号的 adv_w 算宽度
npx lv_font_conv --size 130 --font 新字体.woff --range 0x30-0x39,0x3A --lv-font-name probe -o /tmp/probe.c

# 宽度 ≈ (4*数字adv + 冒号adv)/16, 目标 ≤ 236px(240-4)。偏大→降字号, 偏小→升字号。

# 宽高比更宽的字体会需要更小字号; 更窄的(高瘦)可用更大字号。
```

**3. 正式生成**（写进**同名**文件，覆盖 Bebas）：
```bash

# 时间大字: 大屏/中屏/小屏三档, 用上一步定好的字号填入 size
npx lv_font_conv --bpp 2 --size <新字号> --format lvgl --no-compress \
  --font 新字体.woff --range 0x30-0x39,0x3A --lv-font-name clock_bebas_130 -o clock_bebas_130.c

# 日期小字: 0-9 与 '-' 一个档
npx lv_font_conv --bpp 2 --size <日期字号> --format lvgl --no-compress \
  --font 新字体.woff --range 0x30-0x39,0x2D --lv-font-name clock_bebas_date -o clock_bebas_date.c
```

**4. 编译烧录**（**不用清 build**）：
```bash
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

**5. 看效果**：若大屏主档没被选中（显示偏小），说明新字体特定字号放不进 240 宽，`PickClockFont` 自动落到了小档——回第 2 步调大或调小主档字号。

### ▶ 现有 Bebas Neue 的生成命令（参考）

```bash

# 源字体(npm): npm i @fontsource/bebas-neue, 解包取 files/bebas-neue-latin-400-normal.woff

# 时间大字 130/60/48px (0-9 与 ':'); 这里是当前定稿的三档字号:
npx lv_font_conv --bpp 2 --size 130 --format lvgl --no-compress --font bebas-neue-latin-400-normal.woff \
  --range 0x30-0x39,0x3A --lv-font-name clock_bebas_130 -o clock_bebas_130.c
npx lv_font_conv --bpp 2 --size 60  --format lvgl --no-compress --font bebas-neue-latin-400-normal.woff \
  --range 0x30-0x39,0x3A --lv-font-name clock_bebas_60  -o clock_bebas_60.c
npx lv_font_conv --bpp 2 --size 48  --format lvgl --no-compress --font bebas-neue-latin-400-normal.woff \
  --range 0x30-0x39,0x3A --lv-font-name clock_bebas_48  -o clock_bebas_48.c

# 日期小字 18px (0-9 与 '-'):
npx lv_font_conv --bpp 2 --size 18 --format lvgl --no-compress --font bebas-neue-latin-400-normal.woff \
  --range 0x30-0x39,0x2D --lv-font-name clock_bebas_date -o clock_bebas_date.c
```

> ℹ️ `lv_font_conv` 直接生成即 LVGL9 兼容格式，无需手工改 `adv_w`。若改 bpp/字号后 flash 吃紧，可把 **bpp 降到 1**（粗体下仍清晰）或**减少档位**（整机只一块屏时只需 1-2 档）。

编译完成后烧录看日志（`scripts/build.py` 只管配置+编译，**不支持** flash/monitor 参数，烧录统一用 `idf.py`，端口按实际修改）：

```bash
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

## Arduino 下位机（Mecanum 机器人）

本板可选配一个 **Arduino 下位机**（麦克纳姆轮机器人），由 ESP32 通过串口控制。

> 完整操作手册（编译/烧录/排错）见 **`arduino/MecanumRobot/README.md`**，以下为要点。

### 代码位置
```
main/boards/bread-compact-wifi-s3cam-airobot/arduino/MecanumRobot/MecanumRobot.ino
```
> 该 `.ino` 是 **Arduino 代码**，由 Arduino IDE 编译烧录到 Arduino 板，**不参与 ESP-IDF 固件编译**。

### 硬件
- Emakefun 电机驱动板（I2C 0x60）：4 个直流电机（麦克纳姆轮）+ **2 个舵机**（servo1 头部 / servo2）
- PS2 手柄（`config_gamepad(13,11,10,12)`）：**可选**。未接时 `setup()` 的 `config_gamepad()` 失败 → `ps2_ready=false`，`handleGamepad()` 每轮直接返回（**必须如此**，否则无手柄时 `read_gamepad()` 每轮阻塞约 300ms，见板级 README 踩坑 6）
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

**双向回执（Arduino → ESP32）**：
- 耗时动作（`go-*` / `tj-*` / 巡线）开始执行时回传 `@busy {动作}`，执行完毕回传 `@done {动作}`；
- 速度/舵机1角度变化时回传状态快照 `@stat s{速度} v{舵机1角度}`（事件驱动，非周期轮询；后续扩充在此追加字段，如 ToF 距离 ` d{cm}`）。

ESP32 的 UART0 RX 解析任务维护状态（含 30 秒 busy 看门狗，防 `@done` 丢失导致状态卡死），可通过 `/uno` 接口读取（GET 查询 / WS 推送），返回 JSON：`mode`(idle/moving/line_follow)、`action`(当前或最近动作名)、`speed`(电机速度，未上报为 null)、`servo`(头部舵机角度，未上报为 null)。

> 页面目前只消费其中的 **`servo`**（把 Arduino 侧 SELECT/START 微调、AI 改角度同步回滑块）；`mode`/`action`/`speed` 已**不再在页面显示**（原「机器人控制」面板顶部的状态条已移除），要看下位机到底收没收到指令、收了什么，看面板底部的**下位机指令记录**更直接（见「实时日志与下位机指令」）。

> **不提供 AI 查询下位机状态的 MCP 工具**（原 `self.uno.get_status` 已移除）：动作执行完自动停止、无需确认，而 AI 每次动作后额外查询会多一轮云端工具调用往返，明显拖慢响应。

### 实时日志与下位机指令（网页查看）

**需求**：ESP32 的 console 与 Arduino 控制指令共用 UART0(GPIO43/44)，一旦把日志级别调高就会污染指令流（控制失灵/延迟）；而接线又一直连着 Arduino，串口 monitor 根本看不了日志。

**做法**：用 `esp_log_set_vprintf()` 接管 `ESP_LOGx`，把日志写进**内存环形缓冲**（`log_capture.cc/h`），网页通过 WebSocket 按序号增量拉取。**默认不再写 UART0**，所以日志级别开到信息/调试也不会干扰 Arduino。

- **入口**：网页顶部的「🐞 系统日志」Tab（排在最末）。日志区与「🎮 机器人控制」面板底部的**下位机指令记录**是同一份数据（同一个环形缓冲），只是这里看全文、那里只筛 `[UNO]` 行，不额外占内存。
  - **级别**：无 / 错误 / 警告 / 信息 / 调试。默认 `错误`（`ERROR`）；排查完请调回「错误」（级别调高会增加 CPU 与内存环写入量）。
  - **同时输出到串口**：逃生开关。勾上后日志除进网页外**也照旧写 UART0**（会干扰 Arduino，仅网页打不开或需要接 USB-TTL 抓日志时用）。
  - **清空 / 暂停**：只影响浏览器画面，不影响设备缓冲。
  - **清空设备**（`🗑`）：清掉**设备侧**环形缓冲历史（含重启后保留的崩溃前日志）。想重新观察一轮重启日志时用它。
  - **拉取频率**：停在「🐞 系统日志」Tab 时 1 秒/次（跟手）；停在「🎮 机器人控制」Tab 时 3 秒/次（够用）。切到其他 Tab 则停止——但**这两个 Tab 都会拉**，因为下位机指令记录与系统日志**同源**（停了指令回执就不刷新）。
- **下位机指令记录**：「🎮 机器人控制」面板底部（同一份数据，已按 `[UNO]` 前缀自动筛出）。看懂它就基本能定位控制问题：

| 标记 | 含义 |
|------|------|
| `[UNO] > @go-forward-10` | ESP32 **确实发出了**该指令 |
| `[UNO] ! @go-forward-10 （重复调用已忽略，2300 毫秒前已执行）` | 命中防抖窗口，**本次没有发出**（AI 重复调用时常见，属正常拦截），并给出“上次真正发出的时间”供排查 |
| `[UNO] x @... （指令过长 / UART 写入失败）` | 发送失败 |
| `[UNO] < @busy go-forward-10` | Arduino 回执：**开始执行**（`@done` 为执行完毕，`@stat` 为速度/舵机快照）|

  排查经验：**有 `>` 无 `<`** = 指令发出了但 Arduino 没执行或回执没回来（看接线/供电）；**只有 `!`** = 被防抖挡了；**什么都没有** = 指令没走到发送环节（问题在上层/MCP 调用）。
- **AI 侧逃生与调试工具**（网页打不开时用语音）：
  - `self.debug.log_to_serial(on)`：0=关闭（默认，日志只在网页）、1=打开串口日志（会干扰 Arduino）。
  - `self.debug.set_log_level(level)`：0~4 切换日志级别。
- **内存开销**：环形缓冲 4KB（放在 `.noinit` 段，**不额外增加占用**——只是从 `.bss` 平移过来）+ 拉取缓冲 0.5KB；指令记录与系统日志**共用**同一个缓冲，因此指令功能不额外占内存。
  - ⚠️ **另外还有两笔常驻占用，做重内存操作前要想到**：httpd 任务栈 8KB（上传写 SD 卡需要，见 `http_upload_server.cc`）与一条 WebSocket 连接。本板内部 SRAM（**不是 PSRAM**）实测空载 `free sram` 仅 20~25KB、历史最低 `minimal sram` 曾掉到 6KB 量级——所以**拍照/上传这类重内存操作时建议先关掉网页**（页面常驻的 WS 会一直占着内部 SRAM）。
  - 注意 `SystemInfo: free sram` 打印的是 `MALLOC_CAP_INTERNAL`（内部 SRAM），与 8MB PSRAM 无关，别被“内存很大”误导；`minimal sram` 是**历史最低值**，比瞬时值更能暴露峰值不足。
  - 为何定 4KB：本板内部 RAM 很紧（实测 `free sram` 仅 20~25KB），日志功能静态占用要克制；4KB 够存约 30~40 行日志。需要更长历史时调大 `log_capture.cc` 的 `kRingSize` 即可。
  - 为何不放 PSRAM：临界区内访问 PSRAM 可能长时间持锁，会让等锁的中断超时（见 `sdkconfig.defaults` 关于中断看门狗的说明）。
  - 写入路径用两段 `memcpy`（非逐字节循环）：日志级别开到“调试”时这条路径每秒走很多次，临界区持锁时间直接决定 WiFi/音频中断被推迟多久。
- **崩溃日志怎么看**（偶发重启排查）：
  - 设备**软重启后**（panic / 看门狗 / OTA / `self.reboot`）环形缓冲内容会保留，网页系统日志里会出现一行：
    `================ 设备重启 #N：PANIC 异常或 abort(多为内存不足/空指针/栈溢出) ================`
    **这一行的上方就是崩溃前的日志**（重启前最后做了什么，一目了然）。上电冷启动（拔电/按复位）则缓冲清空——这是刻意的，避免把随机 RAM 当成日志显示。
  - 分隔行里的**重启原因**直接指向方向：`PANIC` 多为内存不足/空指针/栈溢出；`中断看门狗` 多为临界区过长或中断被饿死；`任务看门狗` 多为某任务长时间不让出 CPU；`欠压重启` 是供电问题。
  - ⚠️ **panic 的 `Guru Meditation Error`、backtrace、`stack overflow in task xxx` 永远不在网页里**：它们由 IDF panic handler 用 ROM printf **直写 UART0**，不经过 `esp_log_set_vprintf` 的钩子。**要抓 backtrace 必须接 USB 串口**（按上面说明先断开 Arduino 接线），而且**不需要**打开「同时输出到串口」开关——panic 输出本来就写 UART0。
  - 拿到地址后用踩坑 14 里的 `xtensa-esp32s3-elf-addr2line -pfiaC -e build/xiaozhi.elf <地址>` 解析，并比对日志里的 `ELF file SHA256` 确认固件版本一致。
- **注意**：日志级别默认 `ERROR`，正常运行时网页日志面板几乎是空的，**只有下位机指令行**——这正是想要的（指令不被系统日志冲掉）。

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

**方式一：一键脚本（推荐）**
```bat
:: 在 sketch 目录双击或命令行运行（.ino 同目录）
build_arduino.bat              :: 仅编译
build_arduino.bat COM5         :: 编译 + 烧录到 COM5
```

**方式二：命令行（arduino-cli）**
```bash

# 1) 把 3 个库 clone 到库目录(示例)
cd ~/Documents/Arduino/libraries
git clone <Emakefun_MotorDriver仓库URL>
git clone <NewTone仓库URL>
git clone <PS2X_lib仓库URL>

#    (Emakefun 库取仓库内 arduino_lib/ 目录, PS2X_lib 取仓库内 PS2X_lib/ 目录)

# 2) 编译(Arduino UNO)  —— 打印编译进度、依赖库列表、Flash/RAM 占用
arduino-cli compile --fqbn arduino:avr:uno \
  main/boards/bread-compact-wifi-s3cam-airobot/arduino/MecanumRobot

#     查看详细日志(编译命令/警告): 加 -v

#     保留日志: 末尾加 2>&1 | tee build.log

# 3) 烧录(示例端口, 按实际修改)
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn arduino:avr:uno main/boards/bread-compact-wifi-s3cam-airobot/arduino/MecanumRobot
```
> **RX 缓冲（无需额外参数）**：`arduino:avr 1.8.8+` 的 HardwareSerial 没有 `setRxBufferSize` API，RX 缓冲由编译期宏 `SERIAL_RX_BUFFER_SIZE` 控制（默认 64B≈4-5 条指令，AI 长指令序列会溢出丢指令）。已在本机核心目录建好 `platform.local.txt`（内容 `compiler.cpp.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256`），**IDE 和 CLI 编译都会自动带上**，命令无需再加长参数。
> **换电脑/重装核心后**：在核心目录 `D:\Arduino15\packages\arduino\hardware\avr\1.8.8\` 重新建 `platform.local.txt`，或改用 `arduino-cli compile --build-property "compiler.cpp.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" ...`。

**方式三：Arduino IDE**
1. 用 Arduino IDE 打开 `MecanumRobot.ino`。
2. 库管理器搜索安装上述 3 个库（或手动安装）。
3. 选择板型（Arduino UNO）和端口，编译烧录。

### 使用注意
- **看日志不再需要断开 Arduino 接线**：ESP32 运行日志已改写到内存环形缓冲，用网页「🐞 系统日志」Tab 查看（详见「实时日志与下位机指令」）。仅**烧录**（USB 接电脑）时仍需断开 Arduino 接线，避免串口冲突。
- ESP32 默认日志已降到 **`ERROR`**，且命令带 **`@` 前缀**（Arduino 只认 `@` 开头的行），日志乱码会被忽略，命令更稳定。
- Arduino 程序用**固定 `char` 缓冲**解析命令（不用 `String`），适合 UNO 的 2KB SRAM，抗内存碎片。

### 指令执行模型与串口缓冲（重要）
- **顺序执行**：Arduino 读一条执行一条（`runMotors` 内 `delay` 阻塞），先到先执行，**不会乱序**。
- **RX 缓冲**：UNO 默认 HardwareSerial 接收缓冲仅 **64 字节（≈4-5 条指令）**。动作阻塞执行期间（如 `go-forward-15` 执行 1.5 秒）不读串口，后续指令积压在缓冲里，超出部分**溢出丢弃**（表现为“后面的指令跳过了”）。已通过编译期宏 `SERIAL_RX_BUFFER_SIZE=256`（≈17 条）加大——**注意该宏须在编译时传入**（见上文 arduino-cli `--build-property` / IDE `platform.local.txt`），.ino 内无法设置（`arduino:avr 1.8.8+` 无 `setRxBufferSize` API）；正常 AI 编排序列（3-10 条）不会丢；若实测超长序列仍丢，可再加大或让 ESP32 读 Arduino 回执（`Serial.println("F")` 等已存在）判断动作完成再发下一条。
- **点动命令必须显式停车**：`moveForward()` 等只做 `runMotors(方向, t)` + `delay(t)`，**自身不停车**；`handleCommand()` 的 `go-*`/`tj-*` 分支末尾必须 `stopMove(0)`。**不要**依赖 `handleGamepad()` 无手柄时每轮 `stopMove(10)` 的副作用（`ps2_ready=false` 时它已直接返回，见踩坑 6），否则 AI 动作执行完电机停不下来（踩坑 5）。
- **loop 周期**：未接 PS2 手柄时为亚毫秒级；接手柄时每轮有 `read_gamepad()` + `delay(30)`（约 35ms）。web 遥控驾驶分支（`web_drive_`）每轮 `drivePulse()` 的短脉冲为 `WEB_DRIVE_PULSE_MS`（30ms）。

## ⚠️ 踩坑记录

### 改 config.json 后 `idf.py build` 不生效

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

**本板另一个必须开的开关（2026-09 起）**：`CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y`（相机直出 JPEG 的直通编码）。
它**只在 `config.json` 的 `sdkconfig_append` 里**（两个变体都有），**没有** `sdkconfig.defaults` 兜底 ——
因为那是项目级文件，开了会影响其它板（本板专属配置就该放本板 `config.json`）。
后果：拿一个**陈旧的 `sdkconfig`** 直接 `idf.py build` 编出来的固件，会表现为
**「网页拍照 500 / AI 拍照失败，而推流正常」**（日志 `image_to_jpeg: unsupported format: 0x4745504a`）。
完整配置方法、验证命令与失效症状见「网页实时视频流 → ▶ `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT` 怎么配」。

### 1. CH340 新驱动导致 Arduino 上传失败（`cannot set com-state`）

**现象**（Windows + CH340 方案的 UNO 克隆板，本机实测）：

- 上传报 `avrdude: ser_open(): can't set com-state for "\\.\COMx"` / `Error: unable to open port ...`；PowerShell 直接打开串口报「设备没有发挥作用」（ERROR_GEN_FAILURE）。
- **间歇性**：同一板、同一端口时好时坏；用 Mixly 先「打开串口监视器 → 关掉窗口」再上传基本能成功（即「预热」现象，可作应急 workaround）。
- 设备管理器里 CH340 **枚举正常、驱动正常**——注意：**枚举正常 ≠ 串口可用**（设备不响应 `SetCommState`）。

**根因**：WCH CH340 **新版驱动 3.8+（含 3.9.2024.9）在 Windows 11 上的驱动 bug**。avrdude 官方确认（[avrdudes/avrdude#1328](https://github.com/avrdudes/avrdude/issues/1328)，标签 `not our bug`）：Win10 一般正常、Win11 复现；**与 avrdude 版本无关**（6.3/8.0 同样受影响），不是板子/线材/代码问题。多台 Windows 电脑出现同类问题属正常。

**解决：降级 CH340 驱动到 3.5.2019.1**（官方验证可靠，一步到位）：

1. 获取驱动：本机备份在 `C:\Users\liuguoqing\Downloads\ch340_v35\wch340_v35\`；或下载 avrdude issue 附件 `wch340_v35.zip` / `https://github.com/wemos/ch340_driver`。
2. 设备管理器 → 端口(COM 和 LPT) → **USB-SERIAL CH340** → 右键 → **更新驱动程序** → 浏览我的电脑 → **让我从列表选取** → **从磁盘安装** → 选 `CH341SER.INF` → 弹警告选「仍然安装」。
3. 确认版本变为 **3.5.2019.1** → 拔插 USB 线。
4. ⚠️ **Windows 更新可能自动升回新版驱动**——若日后复现，设备管理器 → CH340 属性 → 驱动 → **回滚驱动**。

**备选方案**（仅 3.8 驱动有效，3.9 无此选项）：设备管理器 → CH340 属性 → **高级** 选项卡 → 勾选 **「Enabling the Serial Port Enumerator (SerEnum)」** → 拔插 USB。

**烧录前检查清单**：

- 端口用 `arduino-cli board list` 确认（**别用 COM1**，那是主板通信口；Arduino 在 CH340 端口上）。
- **完全退出 Mixly / Arduino IDE**（它们会独占串口；被占用时报「拒绝访问」而非上面的错误）。
- 烧录 ESP32 时保持 Arduino 接线断开（见上方「使用注意」）。

### 2. AI 重复调用 uno 工具（已修复）

**现象**：说一次“前进”，ESP32 串口发出几十次 `@go-forward-10`，机器人反复动。
**根因**：服务端 AI 无执行确认机制，对 `uno` 工具反复生成相同调用（实测 30 次、间隔约 700-900ms、JSON-RPC id 递增）；设备端每次毫秒级正确回复，**设备端无 bug**——同一日志里 `music.*` 工具全部只调用一次（同样返回 `true` 却不重复），对照即可实锤。
**修复**（ESP32 端三重机制，均在板级文件 `compact_wifi_board_s3cam_airobot.cc`）：
1. **工具描述**明确“调用本工具一次即完成整个动作并自动停止，不要重复调用”——治本，实测 AI 不再重复（前进/后退/走 20 步/特技编排均只调用一次，组合动作正常）；
2. `SendUartMessage` 返回**描述性文本**（`指令已发送: xxx` / `指令发送失败: xxx`）而非裸 `true/false`——AI 能确认执行结果；
3. **指令防抖**：相同指令在窗口内只发送一次（命中时**续期**，AI 持续重复调用也挡得住），AI 不听话也挡得住；不同指令（组合编排）不受影响。**窗口按命令类型区分**：动作类（`go-*`/`tj-*`）3 秒（特技最长约 7.5 秒，窗口须大于服务端重试间隔），状态类（`servo-*`/`speed-*`）1 秒。命中时的**回执语义**很关键——必须回“已完成”而不是“防抖/失败”，否则会把 AI 推进重试死循环，详见踩坑 15。
**排查方法备忘**：在 `McpServer::ReplyResult` 临时加一行日志打印 payload，可确认设备端每次调用都发出结果；比较重复调用 id 递增（服务器独立请求）还是相同（重发）；对比不同工具（uno 重复 vs music 正常）即可定位是设备端还是服务器端。

### 3. Web 控制与 AI 控制互斥（AI 指令 & 头部舵机失效，已修复）

**现象**：启用 web 页面机器人摇杆后，上下左右（`@drive-*`）正常，但**头部舵机 & AI 全部控制指令失效**；AI 发 `@go-*` 无反应，于是 AI 反复调用 `uno.get_status` 查询状态（因指令未生效只能反复确认）。
**根因**：Arduino 下位机 `MecanumRobot.ino` 的 `loop()` 里，`checkDriveCommand()`（处理 web 摇杆 `@drive-*`）与 `executeCommand()`（处理 AI 的 `@go-*`/`@servo-*`/`@tj-*`）**各自用 `while(Serial.available())`/`if(Serial.available())` 抢读同一串口**。而 `checkDriveCommand()` 的 `while` 会**一次性读空整个 RX 缓冲**，只识别 `drive-` 前缀，其余命令（含 AI 的 go/servo/tj）被 `continue` 丢弃，导致随后执行的 `executeCommand()` 永远读不到——AI 控制与头部舵机全部失效。web 摇杆心跳（`@drive-*`）本身能被 `checkDriveCommand` 识别，所以上下左右正常。
**修复**（Arduino）：将串口命令读取统一收敛为 `serialCommand()`，一次读一行并按前缀**分发**：`drive-*` → `handleDrive()`（保留原 web 驾驶逻辑），其余 → `handleCommand()`（原 `executeCommand` 的点动逻辑）。`loop()` 的三个分支统一调用 `serialCommand()`，不再有两个函数抢读互相吞命令。

### 4. `/uno` HTTP 接口慢（~1s，已修复）

**现象**：web 摇杆按下后 `/uno` GET 每次约 1s、POST 几百 ms~1s；“网页多控制时延迟特别高”。
**根因**：ESP32 侧 `InitializeEchoUart()` 里 `uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE*2, 0, ...)` 的 **`tx_buffer_size=0`**（仅 128B 硬件 FIFO）。web 摇杆约 4Hz（250ms）心跳（`@drive-*`，走 `SendUartMessage(..., false)`，绕过防抖）+ AI 指令同时涌入时，`uart_write_bytes` 会因 TX FIFO 填满而**阻塞**。而 `/uno` 的 `HandleUnoGet`/`HandleUnoPost` 都运行在 **httpd 单任务** 里，一旦被阻塞，后续所有 `/uno` 请求（含 GET）**串行排队**，表现为每次调用 1s、多控制时延迟叠加。
**修复**：`uart_driver_install` 的 TX buffer 从 `0` 改为 `BUF_SIZE`（1024，软件环形缓冲），避免短时高频写阻塞 httpd。
**备注**：该修复是降低阻塞概率；若 web 心跳频率仍过高，可考虑前端加大心跳间隔或在 ESP32 侧改为非阻塞/带超时写，需实测确认。

### 5. AI 重复调用时“一直摇头 / 一直前进停不下来”（已修复）

**现象**：说一次“摇头”，机器人**一直摇**；说一次“前进”，AI 反复发 `@go-forward-*`，动作间隙电机也不停。

**根因（两个独立缺陷叠加）**：
1. **防抖不续期**（ESP32）：`SendUartMessage` 的防抖命中分支直接返回，**不刷新 `s_last_us`**，于是窗口语义变成“距上次**实际发送**的时间”。AI 每 0.9 秒重复调用一次时，每 2 次调用就放行 1 次 → **约每 1.8 秒下发一条**；而 `tj-yaotou` 本身要跑 3.5 秒、`tj-sxzw` 约 7.5 秒 → 缓冲里永远有下一条 → “一直摇”。
2. **动作执行完不刹车**（Arduino）：`moveForward()` 等只做 `runMotors(方向, t)` + `delay(t)`，**本身不停车**；旧实现靠 `loop()` 里 `handleGamepad()` 在“无手柄按键”时每轮 `stopMove(10)` 的**副作用**停车。加入 `ps2_ready` 守卫（见踩坑 6）后该副作用消失 → `@go-*` 执行完电机持续转动。

**修复**：
- ESP32：防抖命中时**刷新时间戳（续期）**——只要 AI 持续重复调用同一指令就一直拦截，停止调用超过窗口后才允许再次触发；窗口按类型区分（动作类 3 秒 / 状态类 1 秒）。中间夹了别的命令（`forward`→`left`→`forward`）不受影响。
- Arduino：`handleCommand()` 的 `go-*` 与 `tj-*` 分支末尾**显式 `stopMove(0)`**。注意**不能**加进 `moveForward()` 等函数内部——巡线的 `moveForward(0)` 和手柄的 `moveForward(10)` 都依赖“设置方向后不刹车”。

**回归防护**：`scripts/tests/test_uno_debounce.py` 用 Python 复刻防抖判定并断言 `.ino` 源码里的 `stopMove(0)`，同时固化“旧逻辑每 1.8 秒放行一条”的证据。

### 6. 无 PS2 手柄时 loop 每轮被阻塞 ≈300ms（延时根因，已修复）

**现象**：web 摇杆按下后要等半秒以上才动；舵机点一下“经常没反应”。

**根因**：`loop()` 的 `else` 分支每轮无条件调用 `handleGamepad()`，其中 `ps2x.read_gamepad()` 在**没有手柄**时会失败重试 5 次、每次失败都 `reconfig_gamepad()` + `delay(read_delay)`，而 `read_delay` 每次失败 +1（上限 10）。实测 `read_delay=10` 时单次约 250ms，加 `delay(30)` 与 `stopMove(10)` ≈ **每轮 loop 300ms**。

**为什么首帧要 600ms+**：命令到达时 loop 正阻塞在 `handleGamepad()`（最多 300ms）；该轮结束进入下一轮时 `web_drive_` 仍为 false → 又走 `else` 分支**再执行一次** `handleGamepad()`（再 300ms）→ 第三轮才进入 `web_drive_` 分支开始驱动。

**修复**（Arduino）：`setup()` 记录 `ps2_ready = (config_gamepad(...) == 0)`，`handleGamepad()` 开头 `if (!ps2_ready) return;`。
**注意**：该守卫是“一次性判定”，若手柄接触不良导致 `config_gamepad` 失败，手柄会一直不可用（旧实现每轮重试可自愈）——如需两者兼顾，可改为“失败后每 2 秒重试一次”。

### 7. 待机时 web 遥控有几百毫秒延迟（WiFi 省电，已修复）

**现象**：web 摇杆/舵机**所有**命令都有几百毫秒延迟；**激活小智对话后延迟明显变小**。

**根因**：设备待机时 WiFi 处于**最大省电**。链路：`application.cc` 的 `OnAudioChannelClosed` → `SetPowerSaveLevel(LOW_POWER)` → `wifi_board.cc` → `WifiManager` → `wifi_station.cc` 的 `esp_wifi_set_ps(WIFI_PS_MAX_MODEM)`。该模式下 station 大部分时间睡眠，AP 只能把 WS 帧缓存到下一个 DTIM beacon 才下发（beacon interval 100ms 量级）→ **几百毫秒**。对话中走 `OnAudioChannelOpened` → `PERFORMANCE`（`WIFI_PS_NONE`），所以激活后延迟变小。

**修复**：web 控制页 WS 连接期间强制性能模式。
- `UnoWebApi` 新增 `on_client_change(count)` 回调；`http_upload_server.cc` 在 WS 会话登记/注销时通知板级，并用 `cfg.close_fn` 兜底 CLOSE 帧丢失（浏览器崩溃/网络中断）的情况。
- 板级 `override SetPowerSaveLevel()`：`web_control_active_` 为真时强制 `PERFORMANCE`。**必须用 override**——`Application` 在状态切换（如对话结束回 Idle）时会再把级别设回 `LOW_POWER`，只有在 override 里拦截才挡得住。

**代价**：web 页面打开期间 WiFi 不省电（功耗略增），关闭页面后自动恢复。对需要实时遥控的机器人是必要取舍。

**验证方法**：待机状态下摇杆延迟约几百毫秒 → 唤醒小智进入对话后再摇，延迟明显变小 → 即为此根因。

### 8. 舵机“拖回原角度没反应 / 回正偶发失效”（已修复）

**现象**：拖滑块到 120° → 点回正 → 再拖回 120°，舵机**完全不动**；偶尔点回正没反应且无任何提示。

**根因（两个前端缺陷）**：
1. **`lastServoDeg` 不同步**：`servoHome()` / `homeStep()` 改了滑块 UI 却没有更新拖动缓存 `lastServoDeg`，于是再拖回同一角度时 `onServoInput` 的“值未变不重复发送”判断成立 → 一条命令都不发。
2. **一次性命令静默丢失**：舵机/回正是**没有心跳重发**的一次性命令，`wsSend` 在 WS 未就绪（页面刚加载、断线重连的 1 秒窗口、切后台回来）时直接 reject，而 `servoHome()` 没有 `.catch()` → 命令静默丢弃，用户毫无感知。摇杆因为有 250ms 心跳重发，丢一条下一条补上，所以只表现为卡顿。

**修复**（`web/index.html`）：`servoHome()` / `homeStep()` 同步 `lastServoDeg`；三处舵机命令补 `.catch()` 并在失败时**回滚 `lastServoDeg`**（否则失败后拖回同一角度仍会被拦）并在页面 `#wslog` 提示“发送失败，请重试”。

### 9. web 控制一会儿后彻底失联、AI 同时失效（fd 泄漏，已修复）

**现象**：web 页面打开后摇杆/舵机**一开始灵敏**（延迟已修好），控制一会儿后**突然失联**，之后**任何控制都没反应**——包括小智 AI 语音控制。

**根因**：注册 `httpd_config_t.close_fn` 后**没有关闭 socket**。IDF 的 `httpd_sess_delete()`（`components/esp_http_server/src/httpd_sess.c`）实现是：

```c
if (hd->config.close_fn) {
    hd->config.close_fn(hd, session->fd);
} else {
    close(session->fd);          // ← 默认才关
}
```

即 **`close_fn` 是“替代”默认的 `close(fd)`，而不是“关闭前的通知”**。回调只清了 WS 登记表，于是**每个 HTTP 请求都泄漏一个 fd**；累积到 `CONFIG_LWIP_MAX_SOCKETS`（本板 16）后 `socket()` 失败 → **web 页面与小智协议（同样需要 socket）一起失联**。

**修复**：`OnWsSessionClosed()` 末尾补 `close(sockfd)`（并 `#include <unistd.h>`）。

**回归防护**：`scripts/tests/test_http_close_fn.py` 断言 `OnWsSessionClosed` 函数体必须含 `close(sockfd)`、且 `WsUnregisterClient` 不重复 close（避免双重关闭）。

**排查备忘**：“用着用着彻底没反应”的现象优先怀疑 **fd/socket 泄漏**（而非 WiFi 或内存）——可临时在 `lwip` 打印 socket 计数确认；`close_fn` 是 IDF 里少见的“替代式”回调，务必对照 `httpd_sess.c` 源码确认语义。

### 10. web 页面删除闹钟无效（已修复）

**现象**：网页「⏰ 闹钟提醒」里点「删除」，列表原样不动；偶尔又能删掉，但删掉的并不是点的那一条。
**根因**：前端 `wsSend()` 用**请求序号覆盖了消息体的 `id` 字段**：

```js
const id = wsId++;                                            // 请求序号, 1,2,3...
ws.send(JSON.stringify(Object.assign({}, obj, { id: id })));  // ← 覆盖调用方传入的 id
```

而删除闹钟恰好用 `id` 传闹钟编号（`wsSend({ action: 'alarm_remove', id: id })`）→ 服务端 `alarm_remove` 拿到的是**请求序号**而非闹钟编号：序号与编号不符时 `AlarmManager::Remove()` 查不到 → `{"ok":false}`；旧前端又不检查 `ok`、照旧刷新列表 → 表现为「删除无效且毫无提示」。若序号碰巧等于某个闹钟编号，则会**误删另一个闹钟**。
> 其它功能不受影响的原因：`music_delete` 用 `name`、`alarm_add` 无业务 id，都不与 `wsSend` 的序号字段冲突——这也是问题只出现在「删除闹钟」的原因。

**修复**：业务字段改用不与序号冲突的 `alarm_id`（前端 `delAlarm`），服务端 `alarm_remove` 优先取 `alarm_id`、回退 `id`（兼容 HTTP `/alarm` 的 `{"action":"remove","id":N}` 与旧页面缓存）；前端失败时不再静默刷新，而是提示「未找到编号 N 的闹钟」（**不在服务端加日志**：板级日志与下位机控制指令共用 UART0，打日志会干扰指令下发）。

**回归防护**：`scripts/tests/test_alarm_remove_wire.py` —— 复刻 `Object.assign` 覆盖语义 + 服务端取值逻辑，固化根因（旧字段必然取到序号）；并静态断言 `delAlarm` 使用 `alarm_id`、检查 `ok`，C++ 分支优先 `alarm_id`、且该分支**不含日志调用**。

**排查备忘**：`wsSend` 的协议是「请求序号与业务字段共用同一个 JSON 对象」，**任何业务字段都不要叫 `id`**，否则会被序号覆盖（不读 `wsSend` 实现几乎无法从现象推断）。另：`web/index.html` 经 `EMBED_FILES` 嵌入固件，改完**必须重新编译**才生效；浏览器若缓存了旧页面，删除仍会失败（旧 JS 依旧发 `id`），需强制刷新（Ctrl+F5）。

### 11. web 页面首屏歌曲列表为空（已修复）

**现象**：首次打开网页（默认「歌曲管理」）列表空白，点「刷新列表」才出来——加载入口只有 tab 点击，初始化路径上没有任何加载调用。
**修复**：`ws.onopen` 首次连上时调 `loadActivePanel()`，按当前可见面板分发（`wsFirstOpen` 保证只补一次）。
**坑**：不能写 `window.onload -> loadSongs()`——那时 `ws.readyState` 还是 `CONNECTING`，`wsSend` 直接 reject，表格会显示「接口不可用」。

### 12. web 页面头部舵机滑块不好用 / 回正值输入框点不动（已修复）

**现象**：滑块在手机上 1px≈1°、手指还挡住角度值，没法微调；`#servoHomeVal` 带 `readonly`，只能一点点调。
**修复**：滑块保留做粗调，两侧加「− / +」（每次 5°），三者共用 `setServoDeg()` 同步显示与去重下发（设备上报的 `servo` 也回填到这里）；`#servoHomeVal` 去 readonly + `onchange` 手动输入（空值/非数字回退）。
**两个坑**：
1. 滑块 `oninput` 只更新显示，`onchange`（松手）才下发——拖动时事件可达数十 Hz，每条都发会灌爆设备 httpd。
2. 发送失败**不能回滚角度基准**：`servoDeg = null` 会让下一次 `null + 5 = 5`，表现为「点一下变 5° 后卡住」（没连设备 / WS 重连窗口必现）。0 是合法角度，兜底也不能用 `||`。
**回归防护**：`scripts/tests/test_web_servo_step.py`（含 wsSend 失败路径）。

### 13. 上传中断留下截断文件，页面却提示「上传成功」（已修复）

**现象**：上传大文件（转码后的 MP3）或歌词时，若浏览器/网络中途断开，TF 卡上会留下**半个文件**，页面却显示上传成功——表现为歌曲播到一半就结束、歌词只显示前半段，且因为文件名已存在，重传时容易被误当成「覆盖成功」。
**根因**：`esp_http_server` 的 `httpd_req_recv()` 在**对端提前断开**时返回 `0`，与「body 已读满」的返回值相同；旧代码只判 `ret < 0`，截断文件于是被当成上传成功保留。
**修复**：循环结束后以 `req->content_len` 为准复核实收字节数，不一致则 `remove(path)` 并回 500 `receive interrupted`；`content_len == 0`（无 body/无长度信息，如 chunked）时只保留 `ret < 0` 判断，避免误杀正常上传。
**回归防护**：`scripts/tests/test_upload_truncation_guard.py`（固化「旧逻辑保留截断文件」+「新逻辑不误杀正常上传」）。

### 14. 删掉时钟日期小字导致开机无限重启（LVGL 对象野指针，已修复）

**现象**：为「去掉时钟上方的日期小字」，把 `date_label_` 的创建、`clock_date_text_` 成员、`LV_FONT_DECLARE(clock_bebas_date)` 声明一并删除（时间对齐偏移同时归零）。编译烧录后**开机即无限重启**：`Guru Meditation Error: LoadProhibited`，`EXCVADDR: 0x25`，崩在 `is_transformed`（`lv_obj_pos.c:1347`）。
**根因**：`addr2line` 定位到崩溃链为 LVGL 内部——`update_layout_completed_cb`（`lv_label.c:1076`）→ `lv_label_refr_text(obj)` → `lv_obj_update_layout` → `lv_obj_refr_size` → `lv_obj_invalidate` → `obj_invalidate_area_internal` → `lv_obj_tree_walk(blur_walk_cb)` → `is_transformed`。其中 `update_layout_completed_cb` 是 LVGL 挂在 **display** 上、以 label 为 `user_data` 的回调（`lv_label_set_text` 后注册，用于重启 `SCROLL_CIRCULAR` 滚动），**不随对象删除而清理**；删掉一个 label 改变了对象数量与堆分配序列，就使这个上游弱点现形（A2=`0x1d`，即 `obj->spec_attr->layer_type` 读到非法地址）。注意整条崩溃链**没有任何本板代码**，极易误判为“跟我改的无关”。
**修复**：**不删对象**——保留 `date_label_` 的创建与每分钟的文本更新（对象数量、堆分配、display 级回调注册与历史版本逐一一致），只在 `UpdateClock` 的显示分支**不再 `lv_obj_remove_flag(date_label_, LV_OBJ_FLAG_HIDDEN)`**，让它永远停在 `SetupUI` 设的 HIDDEN 状态；`clock_label_` 对齐偏移由“给日期腾位”的 `+14px` 改为 `0`，实现真正的垂直居中（纯坐标改动）。
**教训**：**本板去掉 UI 元素只改可见性（`LV_OBJ_FLAG_HIDDEN`）或坐标，不要删除 LVGL 对象的创建。** 对象数量一变，堆布局随之变化，很容易触发上游 label 回调的野指针问题——表现却是与本板代码毫无关系的 LVGL 内部崩溃。定位手段：`xtensa-esp32s3-elf-addr2line -pfiaC -e build/xiaozhi.elf <地址>`，并用 `sha256sum build/xiaozhi.elf` 与日志的 `ELF file SHA256` 比对，确认抓到的就是设备上那份固件。

### 15. AI 狂发 stop、对话卡在「说话中」约 30 秒（已修复）

**现象**：语音说「左转」，动作走了，但之后日志里 `[UNO] ! @go-stop-5` 刷 40 多次（对应云端 AI 每 0.7~1 秒调用一次 `self.uno.action`），页面底部对话文字赖着不消失、状态机停在 `speaking` 约 30 秒才回到 `listening`。

**根因（两层叠加，缺一不可）**：

1. **回执文案在暗示“动作还没完成”**：AI 左转后想停下，调 `action(0,5)`；而工具把 `stop` 也按 `steps*100` 算时长，回了「指令已发送: go-stop-5，动作约500毫秒后自动停止」。AI 读成“动作尚未完成”，于是隔一会儿再调一次确认。
2. **防抖回执是“失败语义”**：命中防抖时回「指令已发送(防抖): xxx」——**“防抖”两个字让 AI 认为没发出去**，于是继续重试；而防抖的**续期**逻辑（踩坑 5 引入）让窗口永不失效 → **AI 越试越被拦、越被拦越试，永不收敛**。工具调用不结束，对话就不会结束，状态机也就一直卡在 `speaking`（表现就是“卡在说话中，实际没声音”）。

**为何“卡在说话中”是必然的**：`speaking` 状态要等 TTS 结束/通道关闭才退出，而 AI 还在连续发工具调用轮次 → 对话不结束 → 状态机不动。**这不是音频问题**，别去查喇叭和音频通道。

**修复**（均在板级文件）：
- 防抖命中改为回**已完成语义 + 时间证据**：「该指令已于 2300 毫秒前执行完成（go-stop-5），本次重复调用已忽略，动作会自动完成并停止，无需再次调用」。单独记录 `s_last_sent_us` 是因为续期会把 `s_last_us` 一直刷成“刚刚”，报不出“多久前真正执行过”。
- `stop` 单独分支：**强制 `steps=0`**（下位机 `stopMove` 本就是立即刹车，后面的 `delay(t)` 纯属白等），回「机器人已停止，无需再次调用确认」，不再走时长文案。
- 左/右转**最小 25 步**：`turnLeft` 每步仅 10ms，AI 常用的默认 `steps=10` 只有 **100ms**，短到几乎看不出转动、也容易让 AI 误判“没执行”；与网页端左转的最小有效时长（250ms）对齐。
- 工具描述补「返回即代表已执行完毕，不要重复调用、也不要再调用本工具确认」。

**回归防护**：`scripts/tests/test_uno_debounce.py` 的 `TestAiRetryLoopFix` 断言源码不得再出现 `指令已发送(防抖)`、`stop` 必须 `steps=0`、左/右转必须钳到 25 步、`s_last_sent_us` 只能在真正写出 UART 之后更新。

**教训**：给 AI 的工具**回执不能有歧义**。“防抖/已忽略/失败”这类词会被读成“没成功”，而带**时间证据**的“已完成”才能终结重试。另外：防抖拦截本身是**正常行为**（AI 重复调用同一指令），看到 `[UNO] !` 刷屏不必惊慌，真正要盯的是**回执有没有让 AI 以为失败**。

### 16. AI 拍照片偶发重启（内部 SRAM 耗尽，2026-09 修复）

**现象**：加了网页日志功能后，让 AI 拍照，有时照片已拍成功、屏幕也显示了，ESP32 却直接重启。
网页日志在 `HttpClient: Established new connection ... cost=40` 一行后戛然而止，下一行就是开机日志
（`I (365) Display: ...`），**没有任何错误信息**；`SystemInfo` 显示 `free sram: 24579 minimal sram: 6175`。

**根因（两层叠加）**：

1. **内部 SRAM 余量被 web 功能吃掉**：本板 `free sram`（`MALLOC_CAP_INTERNAL`，**非 PSRAM**）空载只有 20~25KB。
   web 日志功能常驻占用 = 4KB 环形缓冲 + 0.5~1KB 拉取缓冲 + **httpd 任务栈 8KB** + 一条 WS 连接；
   而**拍照上传那一刻**主任务还要开一条到 `api.xiaozhi.me` 的 HTTP 连接、创建 JPEG 编码线程
   （pthread 默认栈 3KB），同时 LVGL 任务在把 640×480 RGB565 预览图缩放渲染到 240×240
   （2026-09 改单一 JPEG 模式后预览是解码好的 320×240，LVGL 侧工作量更小）——
   多方并发抢内部 SRAM，于是“有时够、有时不够”（第一次成功、第二次崩）。
   崩溃点落在没有 try/catch 的上下文（HTTP 接收任务 / LVGL 任务 / esp_timer），所以表现为直接重启。
2. **lwIP/WiFi 缓冲不能落 PSRAM**：`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 默认关闭，
   8MB PSRAM 完全帮不上忙；而 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048` 又把所有
   ≤2KB 的分配塞进内部 RAM，碎片化更严重。

**为何完全看不到崩溃原因（最惨的教训）**：`log_capture` 用 `esp_log_set_vprintf()` 只接管 `ESP_LOGx`；
panic 的 `Guru Meditation`、backtrace、`abort()` 消息由 IDF panic handler 用 ROM printf
**直写 UART0**，不经过该钩子 —— 网页日志**永远看不到崩溃原因**；而设备一重启，`.bss` 里的
环形缓冲又被清零，崩溃前的日志也一起没了。**结果就是“重启了但什么线索都没有”。**

**修复**：
- 环形缓冲迁到 `.noinit` 段（软重启保留崩溃前日志），并打印 `esp_reset_reason()` 分隔行指明重启原因；
  新增网页「🗑 清空设备」按钮。
- 本板 `config.json`：开 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`，
  `LWIP_TCP_SND_BUF_DEFAULT` / `LWIP_TCP_WND_DEFAULT` 5760→2920。
  > ⚠️ **这两项只在用 `scripts/build.py` 构建时才会写进 `sdkconfig`**；`idf.py build` 不读 `config.json`，
  > 所以走 `idf.py build` 路线时它们从未生效（如上表实测）。要生效只能 `menuconfig` 手改一次，
  > 或跑一次 `build.py`（它会重建 `sdkconfig`）。
  >
  > ⚠️ 原先这里还有 `LWIP_MAX_SOCKETS=10`，**已删除**：它与 `sdkconfig.defaults` 里“故意设 16 以免
  > socket 池被占满挤掉小智 UDP 音频通道”的注释直接冲突（详见踩坑 9）。
- 日志拉取路径瘦身（原地 UTF-8 清洗代替字符串拷贝 + 缓冲 1KB→0.5KB + WS 会话上限 4→2）、
  前端拉取频率分层（展开 1s / 收起 3s）。

**排查手段（已固化）**：以后遇到偶发重启，先用网页日志的重启分隔行拿**复位原因**，
再接 USB 串口抓 backtrace（panic 输出不受网页日志开关影响）。**不要再只盯网页日志找崩溃原因。**

**回归防护**：`scripts/tests/test_airobot_log_persist.py`（`.noinit` 段、三重校验、
冷启动必须清空、复位原因必须覆盖 PANIC/看门狗/欠压）+ `test_airobot_web_photo.py`
（拍照必须复用已捕获帧、不得新增帧缓冲、JPEG 必须落 PSRAM）。

> 后续更正（见踩坑 18）：真正的根因是 **JPEG 编码线程 3KB 栈溢出**，不是内部 SRAM 耗尽。
> 本节当时只有网页日志、没抓到 backtrace，归因偏了；内存优化本身无害，但治不了这个崩溃。

### 17. 照片相册的 4 个坑（2026-09）

**① ESP32 上做不了缩略图**：JPEG 缩略图必须先解码（640×480 解码约 100~300ms + 一块解码缓冲），
而本板内部 SRAM 只有几十 KB。因此列表**直接用原图**让浏览器缩放，靠 `loading="lazy"` + 分批
（12 张/批）避免一次把 100 张全拉下来——设备是从 TF 卡**逐张**读取发送的（每张约 50KB、100~300ms），
一次全量拉图会把**单线程的 httpd 任务**卡住（连 WS 日志拉取都要排队）。

**② httpd 的 URI 通配符匹配默认关闭**：`/photos/<name>.jpg` 这种路径需要
`CONFIG_HTTPD_URI_MATCH_WILDCARD`（本项目未开启），所以文件名用 **query 参数**传
（`/photos/file?kind=web&name=…`），零额外配置。

**③ 滚动清理必须放在写卡成功之后**：先清理再写，写失败就把用户的旧照片白删了。
同理，`fwrite` 长度不符要**删掉半截文件**再返回失败（半截 JPEG 打不开，比没有更糟）。
删除/读取的文件名都要净化（拒绝 `/`、`\`、`..`，只收 `.jpg`），否则
`/photos/file?name=../../config.json` 这类请求能读走卡上其它文件。

**④ AI 拍照的 JPEG 是流式的，没有现成缓冲**：`Explain()` 边编码边入队上传，
要顺带存卡只能在编码回调拿到完整 JPEG（`index==0`）的那一刻**同步写卡**——
那个指针只在回调期间有效，记下来稍后再读就是野指针。为此在共享的 `Esp32Camera` 上
加了一个默认空的观察者 `SetJpegObserver()`（纯增量，其它板行为不变）。
写卡与上传是**并行**的（上传线程已从队列取到数据），AI 响应只慢约 100~300ms。

另一种思路是重写一遗 JPEG 去存卡（复用 `Capture()` 的帧），但会多一次编码（多 100~200ms CPU
且内存峰值更高）——本板内部 SRAM 很紧，不值得。

> ⚠ 这个观察者回调跑在 `Explain()` 的**编码线程**里，写卡是重活：该线程栈已显式放大到
> **16KB**（见踩坑 18）。改编码线程的创建方式时**别退回 `std::thread` 默认栈**（3KB 必崩）。

### 18. AI 拍照必重启（编码线程 3KB 栈溢出，2026-09 修复）

**现象**：网页「📷 拍照」一切正常，但**只要让 AI 拍照**就立刻重启。网页日志只有复位分隔行
（`设备重启 #N：PANIC 异常或 abort`），**真正的 backtrace 只有接 USB 串口才看得到**：

```
***ERROR*** A stack overflow in task pthread has been detected.
--- vApplicationStackOverflowHook
--- vTaskSwitchContext
```

**根因**：`Esp32Camera::Explain()` 用 `std::thread` 起编码线程，而 `std::thread` 底层是 pthread，
栈只有 **3KB**（`CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=3072`）。这条线程里要干两件重活：

1. 软件 JPEG 编码（`image_to_jpeg_cb` → esp_new_jpeg）；
2. 板级 JPEG 观察者里的**同步写 TF 卡**（照片相册那版新加的：FatFS + SDMMC + 目录清理）。

两者叠加远超 3KB → 任务 `pthread` 栈溢出 → panic 重启。同一条线程此后还背着
`JpegEncodeCb` 的入队与 `std::function` 调用，栈本来就在临界点上。

**判决性对照**：网页拍照在 **httpd 任务（栈 8192）** 里跑**同一段**编码 + 写卡代码，
从来不重启——唯一差别就是任务栈大小（这也是「网页拍照没问题、AI 拍照就崩」的原因）。

**修复**（`esp32_camera.cc`）：新增 `CreateEncoderThread()`，显式给编码线程 **16KB** 栈并优先放
PSRAM（与 `local_music_player` 的播放线程同法），任务名从 `pthread` 改成 `cam_encode`
（崩溃日志一眼认出是谁）；`esp_pthread_set_cfg()` 改的是**全局默认值**，创建完（含异常路径）
立即恢复。编码耗时日志顺带打印 `stack free=`（栈剩余字节）——接近 0 就是又快爆栈了。

**排查手段**：AI 拍照重启时先接串口看 backtrace 里的**任务名**：`pthread` = 无名 std::thread
（本条）、`cam_encode` = 本条修复后的编码线程、`httpd` = 网页拍照那条路、`music_play` = 本地音乐。
**只盯网页日志永远看不到这条 backtrace**（panic 输出由 IDF panic handler 直写 UART0）。

**回归防护**：`scripts/tests/test_airobot_camera_encode_stack.py`
（必须经 `CreateEncoderThread` 建线程、栈 ≥8KB、优先 PSRAM、创建后恢复全局默认配置、
线程必须有可辨认名字）。

**教训**：本仓库 `std::thread` 的默认栈只有 3KB，**凡是带着重活（编解码、写卡、JSON、网络）
的线程都必须显式放大栈**；尤其是往已有回调/线程里加新调用时，先问一句「这条线程的栈还剩多少」。

> 后续：栈修好后 AI 拍照又露出了下一层问题（ml307 `HttpClient` 析构竞态）——见踩坑 19。
> 两次崩溃的 backtrace 完全不同，不要混为一谈：栈那层看 `vApplicationStackOverflowHook`，
> 下一层看 `xQueueSemaphoreTake` / `OnTcpDisconnected`。

### 19. AI 拍照偶发重启（ml307 HttpClient 析构竞态，2026-09 规避）

**现象**：栈溢出修好后，AI 拍照仍会重启，且“偶发”得很有规律（重启几次后又正常几次）。
串口 backtrace **全部落在上游组件里**（本项目自己的代码一帧都没出现）：

```
assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))
--- pthread_mutex_lock_internal → std::mutex::lock()
--- HttpClient::OnTcpDisconnected()        http_client.cc:281
--- HttpClient::Open(...)::{lambda()#1}    http_client.cc:213
--- EspTcp::DoDisconnect → EspTcp::ReceiveTask
```

**根因**（组件生命周期竞态，不是内存问题）：`pthread_mutex_destroy()` 会把 mutex 的 sem 置空，
断言 `pxQueue` 为空就说明 **mutex 已经被析构**；而调用它的却是 EspTcp 的 `tcp_receive` 任务
—— 对象析构后回调才到：

```
tcp_receive 任务                    主任务（Explain）
──────────────────────────────────  ─────────────────────────────────
recv 返 0（服务器关连接）            ReadAll() 返回
DoDisconnect(false)                 Close()   ← connected_ 已 false，直接 return，
  connected_ = false                             **不等接收任务退出**
  close(fd)                         Explain 返回 → unique_ptr<Http> 析构
  回调 OnTcpDisconnected() ──┐              → **mutex_ 被销毁**
                             └────→ 此刻才 lock(mutex_) → assert → 重启
```

触发条件是 `Connection: close`（`HttpClient::keep_alive_` 默认 false，`Explain()` 也没开它）：
服务器响应完就主动关连接——也就是**几乎每次拍照都会走到这条路径**，只是窗口只有**微秒级**，
命中与否取决于两条线程的相对速度。

**为什么会“先网页拍一张，AI 拍照就正常”**：网页拍照会引入几百毫秒额外活动（抓帧 + 编码 +
httpd 回图 + WS 推送），把时序错开，于是那一枪没打中。**那是躲开子弹，不是治病** —— 多试几次仍会崩。
反过来，这也是判断“偶发重启是不是竞态”的一个实用信号：**任何打乱时序的操作都能改变命中概率**。

**为什么升级组件也修不掉**：拉过 `78/esp-ml307` **main 分支**的两个文件，
`~HttpClient()`（仍只做 Close + 删 event group）与 `EspTcp::Disconnect()`（仍 `if (!connected_) return;`）
**一字未改**；上游 3.7.1/3.7.2 修的是 `ReadAll` 死锁与栈缓冲，不是这条竞态 —— 升级到 3.7.x 无效
（还带 `NetworkResult` 的破坏性 API 变更）。

**规避做法**（板级做不了：线程与对象都在共享的 `Esp32Camera::Explain()` 里）：把 Explain 的
HttpClient 改成**常驻复用**（`static std::unique_ptr<Http> explain_http`，判空只建一次）：

- 对象不析构 → `mutex_` 一直有效 → 晚一步的回调不再致命（最多一次无害的重复状态更新）；
- 下一次拍照重建 TCP 时，上一次的接收任务早已退出（间隔是“秒”，它只需“毫秒”）；
- `Close()` 一个都不能省：常驻的是**对象**不是**连接**，不关连接会占满 LWIP socket 池并把
  小智的 UDP 音频通道挤掉（踩坑 9）。

> ⚠ **这是规避，不是根治**。等上游把 `~HttpClient` / `EspTcp::Disconnect` 改成“析构前等待接收任务退出”
> 后，可以改回每次新建（代码注释里已写明，单测也会拦住“顺手改回去”）。

**排查手段**：这类崩溃的 backtrace 里看不到本项目源码 —— 只要看到 `http_client.cc` / `esp_tcp.cc`，
就是组件的 HTTP 生命周期问题，不要去查摄像头或内存。

**同类隐患（本次未动）**：`main/mcp_server.cc`（屏幕快照上传）与 `main/boards/common/esp_video.cc`
也各自 `CreateHttp(3)` 用完即弃，同一条竞态路径；本板目前只有 AI 拍照会稳定触发。

**回归防护**：`scripts/tests/test_airobot_camera_explain_http.py`（不得每次新建、常驻必须判空只建一次、
Close/超时必须保留、注释必须写明是规避）。

### 20. 歌单“整表按值拷贝”持续碎化内部 SRAM（2026-09 修复）

> **现象**：`self.music.list`（列歌单）、`self.music.search`（搜歌）以及**闹钟响铃前的判空**，
> 每次都会把整张歌单按值拷贝一份 —— N 首歌就是 N 次堆分配。而本板内部 SRAM 空载只剩
> 20~25KB、历史最低 6KB（见踩坑 16/18），歌多时这些短命小分配会不断碎化内部堆，
> 是“偶发分配失败 / 重启”的隐性来源，且**平时完全看不出来**。

**根因**：`LocalMusicPlayer::ListSongs()` 按值返回 `std::vector<std::string>`：

| 调用点 | 实际只需要 | 却做了 |
|---|---|---|
| `self.music.list` | 前 30 首歌名 + 总数 | 先拷整表 |
| `self.music.search` | 匹配的歌名 | 先拷整表 |
| 闹钟响铃前 | 判断“有没有歌” | `!ListSongs().empty()` —— **只为判空也拷整表** |

**修复**：接口改成按需访问，**锁内零拷贝**（`local_music_player.{h,cc}`）：

```cpp
bool HasSongs() const;                                        // 判空（不拷贝）
void ForEachSong(const std::function<bool(const std::string&)>& cb) const;  // 锁内遍历
```

板级三处调用点全部改走这两个接口；`ListSongs()` 已删除。

> ⚠️ **使用约束**：`ForEachSong` 的回调运行在 `songs_mutex_` 持锁状态下，
> **回调内不得再调用 `LocalMusicPlayer` 的任何方法**（`std::mutex` 非递归，会死锁）。
> 当前两处回调只做字符串拼接，安全。

**回归防护**：`scripts/tests/test_music_list_api.py` —— 拦住“把按值返回整张列表的接口加回来”、
要求遍历持锁且支持提前退出、要求板级不得再出现 `.ListSongs()`。

> **同类检查思路**：本板内部 SRAM 紧张的根因往往不是“一次性大分配”，而是**高频短命小分配**
> （`std::string` 拷贝、JSON 拼接、`vector<string>` 复制）。改内存相关代码时先问一句：
> “这个接口是不是为了判空 / 取前几个而复制了全部？”

### 21. 「关灯」偶发失效（节点侧 ESP-NOW 收包未补 `'\0'`，2026-09 修复）

**现象**：对 AI 说「关灯」，灯不灭；节点串口（115200）里动作名后面挂着乱码：

```
[收到] 主控指令（第15条）：do light offxV??
[发送] 回执·执行失败：unknown-action offxV??（第1/3次）
```

> （日志已中文化；更早的固件里这两行显示为 `[cmd] seq=15 …` 与 `[espnow] TX  (1/3) …`。）

更迷惑的是**只有部分命令失效**：调色「调成蓝色」、调暗「暗一点」一直正常，
关灯 / 开灯 / 查询（`read`）则偶发失效。

**根因**：ESP-NOW 回调给的是「**原始字节 + 长度**」，**没有字符串终止符** ——
Arduino 核心 `ESP32_NOW.cpp` 的 `_esp_now_rx_cb()` 把 IDF 的 `data`/`len` **原样透传**给
`onReceive()`（无拷贝、无补零；`esp_now_send()` 发的也是长度不含终止符的裸字节）。
而节点侧 `handleCommand()` / `nextField()` 全程是 C 字符串函数（`strcmp`/`strchr`/`strlen`）：

```cpp
// nextField()：取字段时用 strchr 找空格，找不到就 strlen
const char* sp = strchr(p, ' ');
size_t n = (sp == nullptr) ? strlen(p) : (size_t)(sp - p);   // ← 越过包尾读残留
```

**为什么只有“最后一个字段”中招**（这也是“调色正常、关灯失效”的全部原因）：

| 下行报文 | 节点解析到的 action | 结果 |
|---|---|---|
| `do light off` | `off` + 残留（无空格可截断）| ❌ `strcmp` 失败 → `err unknown-action` |
| `do light on` / `do dist read` | 同上 | ❌ |
| `do light rgb 0 0 255` | `rgb`（后面紧跟空格，截断干净）| ✅ args 虽带残留，但 `parseNums` 只挑数字 |
| `do light bright 100` | `bright` | ✅ |

残留字节来自驱动接收缓冲（上一次更长的包 / 未初始化内存），所以**命中与否是概率**，
这就是“偶发”的来源。连带影响：主控收到 `err` 会当作“节点已收到、只是执行不了”而
`CompletePending(confirmed=true)` **销账不再重传**，连 ACK 重传兜底都失效了。

**修复**：`EspNowNode.ino` 的 `onReceive()` 先拷贝到本地缓冲并按 `len` 补 `'\0'`，之后一律用该缓冲：

```cpp
char text[256];
if (len >= sizeof(text)) return;
memcpy(text, data, len);
text[len] = '\0';          // ← 关键：此后才能安全地当 C 字符串解析
```

> **对照**：主控侧 `espnow_home.cc` 的 `HandleRecv()` 一直是对的（`body[body_len] = '\0'` 后才解析），
> 只有节点侧漏了 —— 所以两侧共用一份协议文本时，**“按长度收包”这条约束必须两侧都写进注释**。

**回归防护**：`scripts/tests/test_espnow_home_protocol.py` 的
`TestEspNowPayloadTermination`（复刻“无终止符 → 最后字段被污染”，固化旧逻辑必然出错）
+ `test_node_terminates_espnow_payload_before_parsing`（源码必须 `memcpy` + 补 `'\0'`，
且拷贝后不得再把裸 `data` 当 C 字符串用）。

**教训**：ESP-NOW / UART / socket 这类“字节流 + 长度”的接口，**收到的都是裸字节，不是字符串**。
只要后面用了 `strcmp`/`strchr`/`strlen`/`printf("%s")`，就必须先按长度拷贝并补 `'\0'`；
能“大部分时候正常”只是因为没越界到非法字节而已 —— 这种 bug 永远是**概率性的、且只在某类字段上**。

### 22. 关掉实时视频后拍照必 500（相机 deinit 后再也 init 不回来，2026-09 定位并修复）

**现象**：开过「📹 实时视频」再取消勾选，切到「📷 照片」拍照 → 网页 **500**，AI 拍照也失败；
**不碰视频时一切正常**。真机日志（网页日志，级别「错误」，按时间顺序）：

```
E image_to_jpeg: unsupported format: 0x4745504a      # 0x4745504a 小端就是 'JPEG' FOURCC
E Esp32Camera: EncodeCurrentFrameToJpeg: JPEG encode failed
E cam_hal: cam_dma_config(524): DMA buffer 16384 Byte malloc failed, the current largest free block:12800 Byte
E Esp32Camera: Reinit: esp_camera_init failed with error 0xffffffff
E cam_hal: cam_dma_config(524): DMA buffer 30720 Byte malloc failed, the current largest free block:12800 Byte
E Esp32Camera: restore RGB565 camera failed
E Esp32Camera: Camera capture failed
E MCP: tools/call: Failed to capture photo
```

**关键数字（源码 + 日志双证）**：两种格式的 DMA 缓冲都要一整块**连续内部 SRAM**，而且**与分辨率无关**：

| 格式 | DMA 缓冲 | 来源（`managed_components/espressif__esp32-camera/target/esp32s3/ll_cam.c`） |
|---|---|---|
| VGA RGB565 | **30720** 字节 | `ll_cam_calc_rgb_dma()`：half buffer = 12 行 × 1280 B = 15360，`dma_buffer_size = 2 × half` = 30720 |
| JPEG（任意分辨率） | **16384** 字节 | `ll_cam_dma_sizes()`：`dma_half_buffer_cnt = 16` × 1024 = 16384 |

（RGB565 那档依赖 `CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX=32768`，本板 `sdkconfig` 正是这个值；日志里的 30720 与推导吻合。）

而本板实测**最大连续块只有 12800 字节**（内部 SRAM 空载也才 20~25KB，见踩坑 16）：

```
12800  <  16384（JPEG 需要）  <  30720（RGB565 需要）   →  两种模式都 init 不回来
```

**根因链**（旧实现的致命处）：

1. **旧设计“按需切格式”**：`VideoStreamStart()` → `Reinit(PIXFORMAT_JPEG)`；`VideoStreamStop()` → `Reinit(PIXFORMAT_RGB565)`。
2. **`Reinit` = deinit + init**：`Release()` 先 `esp_camera_deinit()`，随后 init 又要同一块连续 DMA 内存。
3. **这块内存回不来**：日志里 21:48 与 21:49 两次相隔 **64 秒**，最大连续块都还是 12800 ——
   说明不是瞬时碎片，而是**开过视频之后不再回升**（内存归还了，但堆已被切碎/无法合并）。
4. **失败没有兜底**：`Reinit` 失败后相机停在“已 deinit / init 失败”的残留态，
   而 `VideoStreamStop()` **只打一行 ERROR，仍返回 `{"ok":true,"msg":"视频已关闭"}`** ——
   用户看到的是“一切正常 + 拍照莫名 500”。

**两种“拍照 500”要分清**（历史日志对照；新设计下第一种只可能是开关没开）：

| 日志 | 相机实际状态 | 为什么拍照失败 |
|---|---|---|
| `Esp32Camera: EncodeCurrentFrameToJpeg: JPEG encode failed`<br>+ `image_to_jpeg: unsupported format: 0x4745504a` | **还能取到帧** | JPEG 帧被送进不支持 JPEG 输入的软件编码器 —— 现在只可能是 `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT` **没开**（见「网页实时视频流 → 怎么配」）|
| `Esp32Camera: Camera capture failed`<br>+ `MCP: tools/call: Failed to capture photo` | **连帧都取不到**（`streaming_on_` 仍为 true，但 `esp_camera_fb_get()` 返回 NULL，`esp32_camera.cc:153`） | 旧实现 `Reinit` 失败后相机停在“已 deinit / init 失败”的残留态；新设计不再有这条路径 |

**修复（2026-09 已实现）：相机全程单一 JPEG 模式，不再 deinit/Reinit**

| 环节 | 旧做法 | 现在 |
|---|---|---|
| 初始化 | 每次切模式重建相机 | **开机按 JPEG init 一次**（最大档 SVGA、`CAMERA_GRAB_LATEST`、`fb_count=1`）|
| 开视频 | `Reinit(JPEG)` | 只写 sensor：`set_framesize(用户档)` + `set_quality(用户质量)` |
| 停视频 | `Reinit(RGB565)`（**会失败**） | 只写 sensor：`set_framesize(VGA)` + `set_quality(12)` → **不可能失败** |
| 拍照编码 | RGB565 帧 → 软件编码 | JPEG 帧**直通**（`CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y`）|
| LCD 预览 | RGB565 直接给 LVGL | JPEG 用 `esp_jpeg` 的 ROM 解码器解成 RGB565（1/2 缩放）→ **预览保留** |

为什么选 JPEG 当“唯一模式”：**JPEG 的 DMA 只要 16384，比 RGB565 的 30720 少一半**，
而推流本来就要 JPEG，拍照与预览都能由 JPEG 派生出来（预览解码输出在 PSRAM、
草稿纸用静态 `work[3100]`，**不占内部堆**）。

- ~~开 `CONFIG_CAMERA_PSRAM_DMA=y`~~ → **实测不可用**（视频流完全不能用、关流后拍照仍 500），已回退；
  机理与回退方法见「网页实时视频流 → PSRAM DMA 模式：实测不可用」。
- **前端：`<img src>` 必须等 `video_start` 返回 ok 之后再设**。
  原来在“先出框”里就把 src 设了，而设备端 81 端口还没监听 → 首次勾选必现“接口不可用：未连接设备，
  或视频服务未启动”，切走再切回（切走会 `removeAttribute('src')` 重连）才正常。
  现在拆成 `videoShowBox()`（只出框、不连流）+ `videoOpen()`（服务就绪后才设 src）。
- ✅ **旧方案的“待做项”已全部作废**（不是没做，是换了解法）：
  - 不需要“停流失败重试”：停流现在**不可能失败**（只写 sensor 寄存器，零内存分配）。
  - 不需要“降级 QVGA 恢复”：**降 QVGA 也救不了 RGB565** —— 那 30720 是 `ll_cam_calc_rgb_dma()`
    算出的双缓冲总量，真正的瓶颈是**连续块**不够（12800），不是总量不够。
  - 不需要“JPEG 直通补丁”：改由上游开关提供（见上表），`EncodeCurrentFrameToJpeg()` 保持上游原样；
    `Explain()` 只多了一个归还驱动帧的守卫（见踩坑 23，同一块 `current_fb_` 的释放时机问题）。
  - 历史疑点（`free sram` 是否回升、deinit 是否归还）**不再影响决策**（新设计根本不走 deinit），
    但第 3 条“最大连续块不回升”的实测事实**必须保留** —— 它正是“永不 deinit”的依据。

**真机验证要点**：开机日志 `cam_hal: buffer_size:` 应为 **16384**；
「开视频 → 关视频 → 网页拍照 → AI 拍照 → 再开视频」来回 ≥10 次不坏（照片、LCD 预览、AI 识别都正常），
且**拍照后视频画面必须能继续出画**（这条当时漏验，随即暴露了踩坑 23）。

**过程教训（本条也应当记住）**：拿“源码里看起来能行”的开关去解决内存问题，**必须先在真机上只验证它本身**再往下推 ——
`CONFIG_CAMERA_PSRAM_DMA` 就是这样一次失败尝试：机理上说得通（跳过内部 DMA 分配），
实际却让视频流直接不可用。**未实测的推断不要写进文档当结论**。

**教训**：

- 本板的“内存不够”往往不是**总量**不够，而是**连续块**不够：`largest free block` 比 `free sram` 更能定位问题。
- **8MB PSRAM 不是万能**：IDF 里 PSRAM 区域不带 `MALLOC_CAP_DMA`，凡是用 `MALLOC_CAP_DMA` 分配的
  大块（相机 DMA、部分驱动缓冲）都只能在内部 SRAM 里找。驱动自带的 PSRAM 模式开关
  （`CONFIG_CAMERA_PSRAM_DMA` / 运行时 `esp_camera_set_psram_mode()`）是本板试过的**唯一**绕开途径，
  但**实测不可用**（见上文）—— 所以只能从“减少内部连续块需求 / **永不 deinit 相机**”下手
  （本板即如此：全程单一 JPEG 模式，见上）。
- **失败路径必须如实返回**：当时 `video_stop` 失败仍回 `ok:true`，用户看到的是“一切正常 + 拍照莫名 500”，
  排查成本全转嫁到了现象端（无重试、无降级、无错误文案）。

**回归防护**：`scripts/tests/test_web_realtime_video.py` 的 `TestSingleCameraMode`：
板级不得再出现 `Reinit`、`Capture()` 的 JPEG 分支只允许一行调用、
`config.json` 两个变体都必须带 `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y`、
预览解码必须有两道越界保护（先读 JPEG 头定尺寸 + 把 `outbuf_size` 交给解码器）；
另有 `test_img_src_set_only_after_device_ready`（`<img src>` 必须在 `video_start` 之后设）
+ `test_show_box_does_not_open_stream`（出框不许连流）。

### 23. 拍照后实时视频再也出不了画（驱动帧不归还，2026-09 修复）

**现象**（用户真机报告，两条路径都中招）：

- 先开「📹 实时视频」→ 再拍照：**照片能拍成**，但拍完之后视频**再也不动**（定格/全黑）；
- 先拍照 → 再开视频：视频**从头一帧都没有**；
- 两者都**只能重启设备**恢复，且「停流 → 重开」也救不回来。

真机日志（网页「🐞 系统日志」）特征极固定 —— 两条 WARN **成对出现、每 ~4 秒一次**：

```
W (149619) cam_hal: Failed to get frame: timeout
W (149619) LocalVideo: fb_get failed
W (153669) cam_hal: Failed to get frame: timeout
W (153669) LocalVideo: fb_get failed
I (153789) LocalVideo: stream server stopped
I (153799) CompactWifiBoardS3CamAirobot: video stream stopped
I (159979) LocalVideo: stream server started on port 81
I (159979) CompactWifiBoardS3CamAirobot: video stream started (jpeg mode)
W (164029) cam_hal: Failed to get frame: timeout   ← 停流重开照样失败
```

**关键线索**：间隔 **4.05 秒** —— 正好是驱动取帧超时 `FB_GET_TIMEOUT = 4000ms`
（`managed_components/espressif__esp32-camera/driver/esp_camera.c:387`）。
“超时”意味着驱动侧**根本没有可用帧**，与网络/编码无关，所以“停流重开”当然无效。

**根因（驱动源码双证）**：本板 `fb_count=1`（帧池只有一块），cam_hal 判断某块帧“可用”看的是
`frames[x].en`：`cam_give()`（= `esp_camera_fb_return()`）置 1、采集时置 0。
`Esp32Camera::Capture()` 取走帧后放进 `current_fb_` **长期不还** —— 旧实现靠 `Reinit()` 里的
`Release()` 顺手归还，所以从不暴露；而“单一 JPEG 模式永不 Reinit”之后，那块帧的 `en` 永远是 0，
cam_task 的 `cam_get_next_frame()` 找不到空闲缓冲 → 相机停摆 → 之后每次 `esp_camera_fb_get()`
都等满 4 秒返回 NULL。
**拍照本身反而正常**（`Capture()` 读的是自己 `current_fb_` 里那份数据），所以现象看起来是“照片好、视频坏”。

**为什么“先拍照”更惨**：网页拍照/AI 拍照都不归还，于是**开机后第一张照片就是相机停摆的时刻**，
此后视频无论怎么开都拿不到帧 —— 与用户描述完全一致。

**修复（2026-09）**：新增 `Esp32Camera::ReleaseCurrentFrame()`（幂等：还了就置空），拍照链路用完即还：

| 链路 | 归还点 | 备注 |
|---|---|---|
| AI 拍照（`Explain()`）| 函数**最开头**的 `FrameReleaser` 守卫 | 覆盖全部出口（含 6 处 `throw`）；必须等编码线程 join 之后 |
| 网页拍照（`LocalPhotoCapture()`）| `EncodeCurrentFrameToJpeg()` 之后 | 成功失败都要还：编码要读 `current_fb_` |
| `Capture()` 自己 | 取新帧之前先 `ReleaseCurrentFrame()` | 连取两帧时先还再取，取帧失败也不会把旧帧扣住 |
| 实时视频流（`local_video_stream.cc`）| 每帧 `fb_get` → 发送 → `fb_return(fb)` | 本来就是配对的，本次未改动 |

> ⚠️ 不要图省事改成“等下一次 `Capture()` 再归还”：那样一次拍照之后到下次拍照之间帧一直被攥着，
> 期间只要开视频（或视频正在跑）必然全黑 —— 那就正是本次的 bug。

**真机验证要点**（上一节的“≥10 次来回”按这三条判）：

1. 开视频 → 拍照（网页 + AI 各一次）→ 画面应在 1~2 帧内恢复；
2. 拍照 → 开视频必须能出画；
3. 日志里**不应**再出现 `cam_hal: Failed to get frame: timeout` + `LocalVideo: fb_get failed` 成对刷屏。

**教训**：

- `fb_count=1` 不只是“少占内存”，它把帧变成了**全局唯一资源**：谁 `Capture()` 谁就必须
  **显式归还**，而且归还时机要写进接口注释（否则下一个改代码的人一定会漏）。
- 单一模式/长生命周期对象会把“借用”变成“持有”：旧代码里那条顺手归还的路径一消失，问题才浮出来 ——
  **删掉/绕过一条清理路径时，要顺查它顺带兜住了什么**。
- 日志里**稳定的时间间隔**往往就是某个超时常量（这里 4.05s ≈ 4000ms）：先把它和源码对上，再往下查会少走很多弯路。

**回归防护**：`scripts/tests/test_web_realtime_video.py` 的 `TestPhotoReturnsDriverFrame`（归还接口存在且幂等、
`Explain()` 的守卫必须在启动编码线程之前就装好、`Capture()` 先还再取且不再手写裸 `fb_return`、
网页拍照必须在编码后归还、取帧超时只重试不退出）
+ `test_airobot_web_photo.py::test_reuses_captured_frame`（“不得自己取帧”的另一半：**拍完必须归还**）。

## 与上游合并提示

作为独立命名的 board（`bread-compact-wifi-s3cam-airobot`），其目录与 `config.json` 的 `type`/`name` 均为唯一标识，不会与上游同名板冲突。合并上游代码时注意保留 `main/Kconfig.projbuild` 与 `main/CMakeLists.txt` 中本板的注册分支。
本板新增的 `local_photo.*`、`photo_store.*` 由 `main/CMakeLists.txt` 的 `file(GLOB boards/<BOARD_DIR>/*.cc)` 自动纳入，无需在核心 CMake 里登记。

### 共享文件的改动面（2026-09 重构后，刻意压到最小）

“相机全程单一 JPEG 模式”这个设计，落在共享文件 `main/boards/common/esp32_camera.cc` 上的改动只有两处：

| 位置 | 改了什么 | 为什么不用改更多 |
|---|---|---|
| 文件头的匿名 namespace | **新增** `DecodeJpegPreview()`（约 55 行，纯新增，本项目自有区）| 解码逻辑集中在这里，不往上游函数里塞 |
| `Capture()` | 上游那 2 行“JPEG 不解码、只打日志” → **1 行调用**；归还旧帧的 `if` 块（2 行）→ `ReleaseCurrentFrame()`（1 行）| 解上游冲突时只需手工解这几行 |
| `Explain()` | 函数开头**新增** 1 个归还驱动帧的 `FrameReleaser` 守卫（+4 行注释）| 守卫放在函数出口，不逐条改 `return`/`throw`；见踩坑 23 |

`EncodeCurrentFrameToJpeg()`（本项目自有方法）保持上游原样，
JPEG 直通改由上游开关 `CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT` 提供（见「网页实时视频流 → 怎么配」）。
同样，`Esp32Camera::Reinit()` 保留但本板不再调用（其它板可能用）。

板级文件（`compact_wifi_board_s3cam_airobot.cc`）里唯一需要上游留意的是它对 `Board`/`Display` 接口的依赖：
`DecodeJpegPreview()` 用了 `Board::GetInstance().GetDisplay()` + `LvglDisplay::SetPreviewImage()`，
上游若改这两个接口的签名，这里要跟着改（编译期就能发现）。
