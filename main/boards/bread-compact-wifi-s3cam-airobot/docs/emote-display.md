# Emote 表情显示 使用说明

本板支持乐鑫官方表情动画风格（Kconfig：`CONFIG_USE_EMOTE_MESSAGE_STYLE`，本板 `config.json` 已默认开启）。

开启后，屏幕 UI 不走 LVGL，改用乐鑫 `esp_emote_expression` 引擎独立 30fps 渲染，显示内容为：

- **动画表情**：中性 / 开心 / 难过 / 生气 / 哭 / 困惑 / 惊讶 / 眨眼 / 倾听 / 睡觉 / 聆听（官方 .eaf 动画，随对话状态自动切换）
- **对话文本**：顶部/底部滚动字幕（toast），显示 AI 的回复文本
- **时钟**：顶部时间显示（引擎自带 `clock_label`，无需额外代码）
- **状态图标**：WiFi、音量、电量等（左上角）
- **配网二维码**：配网状态下居中显示

所有素材（表情动画、字体、唤醒词模型、布局）打包为 `expression_assets.bin`，烧写到 assets 分区（0x800000，约 2.7MB，分区 8MB 装得下）。

## 一、编译烧录

```sh
cd /Users/liuguoqing/data/www/wwwroot/xiaozhi-esp32-airobot
git fetch origin && git checkout feat/emote-display && git pull

# 激活 ESP-IDF v6.0.2
source ~/esp/v6.0.2/esp-idf/export.sh
idf.py --version   # 确认 v6.0.2

# 编译（自动打包表情资源 + 生成合并固件）
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
```

编译日志出现以下两行即成功：

```
Building airobot emote assets (resolution 240_320)
Project build complete
```

烧录（二选一，端口号用 `ls /dev/cu.usbserial-*` 查）：

```sh
# 方式 A
idf.py -p /dev/cu.usbserial-XXXX flash monitor

# 方式 B：合并固件一把烧
python -m esptool --chip esp32s3 -p /dev/cu.usbserial-XXXX -b 460800 write_flash 0x0 build/merged-binary.bin
```

### 标准流程：直接 `idf.py build`

本板也支持不经过 `scripts/build.py`、直接用 `idf.py` 编译（资源打包是 CMake 的 custom command，`idf.py build` 会自动触发）。

**前提：根目录的 `sdkconfig` 里必须已有本板的关键选项。** 最省事的做法是先用 `scripts/build.py` 跑一次（它会根据 config.json 生成完整正确的 sdkconfig），之后就可以一直用 `idf.py`：

```sh
# 首次：用标准脚本生成正确的 sdkconfig（并顺带产出固件）
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot

# 之后：日常快速编译，全部走 idf.py
source ~/esp/v6.0.2/esp-idf/export.sh
idf.py build        # 编译（自动打包 expression_assets.bin）
idf.py flash        # 烧录全部分区（含 assets 0x800000）
idf.py monitor      # 串口日志

# 或者一步到位（指定端口）
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

`sdkconfig` 中本板相关的**关键项**（用 `scripts/build.py` 生成后自动就位，可抽查确认）：

```sh
grep -E "^(CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_S3CAM_AIROBOT|CONFIG_LCD_ST7789_240X320|CONFIG_USE_EMOTE_MESSAGE_STYLE|CONFIG_FLASH_EXPRESSION_ASSETS)=" sdkconfig
# 期望：
# CONFIG_FLASH_EXPRESSION_ASSETS=y
# CONFIG_LCD_ST7789_240X320=y
# CONFIG_USE_EMOTE_MESSAGE_STYLE=y
# CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_S3CAM_AIROBOT=y
```

注意事项：

- **改 `config.json` 的 sdkconfig_append 不会自动同步到现有 sdkconfig**。改完后要么重跑一次 `scripts/build.py`，要么手动把新选项写进 `sdkconfig`（或删掉 `sdkconfig` 重新生成）。
- **换屏幕**：在 sdkconfig 里把 `CONFIG_LCD_ST7789_240X320=y` 改成目标屏选项（其他 LCD 选项自动失效），然后 `rm -f build/expression_assets.bin && idf.py build`（布局目录变了要重打包）。
- **切分支/切板子后**：旧 sdkconfig 里残留着别的板的选项，建议 `rm sdkconfig build -rf` 后重跑 `scripts/build.py` 重新生成。
- 编译日志中确认 `Building airobot emote assets (resolution 240_320)` 出现，说明表情资源已重新打包。

## 二、屏幕选择（多分辨率适配）

屏幕型号由 Kconfig `choice`（互斥单选）决定，**本板已在 `config.json` 的 `sdkconfig_append` 里固定为 ST7789 240×320**，正常编译无需任何操作。

官方默认布局是 320×240 横屏，本板为竖屏且支持多种屏，所以 `emote_assets/` 下每种画布尺寸各有一份 `layout.json`，CMake 按 LCD 选项自动选择：

| 布局目录 | 画布 | 覆盖的屏（Kconfig 选项） |
|----------|------|--------------------------|
| `240_320` | 240×320 竖 | ST7789/ILI9341/NV3030B 240×320（**当前默认**） |
| `240_240` | 240×240 方 | ST7789 240×240、GC9A01 240×240 |
| `320_480` | 320×480 竖 | ST7796 320×480 |
| `240_135` | 240×135 横 | ST7789 240×135 |
| `128_160` | 128×160 竖 | ST7735 128×160 |
| `128_128` | 128×128 方 | ST7735 128×128 |

### 换屏步骤

1. 编辑 `config.json`（两个变体都要改），把 `CONFIG_LCD_ST7789_240X320=y` 换成目标屏选项，如：
   ```json
   "CONFIG_LCD_ST7789_240X240=y",
   ```
2. 清理并重新编译：
   ```sh
   rm -rf build
   python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
   ```
3. 确认日志中是 `Building airobot emote assets (resolution 240_240)`（新分辨率）。

> 若某分辨率目录下还没有 `layout.json`，CMake 会回退到组件自带的官方 `320_240` 布局（横屏布局，竖屏上位置会偏，仅作兜底）。

## 三、布局调整

布局文件：`emote_assets/<分辨率>/layout.json`，是元素数组。每个元素：

```json
{
  "type": "anim",
  "name": "eye_anim",
  "align": "GFX_ALIGN_CENTER",   // 锚点
  "x": 0,                        // 相对锚点的水平偏移(px)
  "y": -70                       // 相对锚点的垂直偏移(px)
}
```

**坐标是"锚点 + 偏移"，不是绝对像素**，所以同一份布局可适配不同尺寸的屏（偏移量按屏调即可）。

### 元素清单（name 必须与引擎常量一致，不能改名）

| name | 作用 | 说明 |
|------|------|------|
| `eye_anim` | 主表情动画 | 平时显示的表情脸 |
| `emerg_dlg` | 紧急对话框动画 | 与 eye_anim 同位即可 |
| `listen_anim` | 聆听动画 | 说话/聆听时替换脸 |
| `toast_label` | 对话文本 | 滚动字幕，`long_mode` 控制滚动 |
| `clock_label` | 时钟文本 | 需与 `clock_timer` 同时存在 |
| `clock_timer` | 时钟刷新定时器 | 无坐标，必须有 |
| `status_icon` | 状态图标（WiFi/音量） | |
| `charge_icon` | 充电图标 | |
| `battery_label` | 电量百分比 | |
| `qrcode` | 配网二维码 | 仅配网时显示 |

### 锚点（align）取值

| 值 | 锚点位置 |
|----|----------|
| `GFX_ALIGN_TOP_LEFT` | 左上 |
| `GFX_ALIGN_TOP_MID` | 上边中 |
| `GFX_ALIGN_TOP_RIGHT` | 右上 |
| `GFX_ALIGN_LEFT_MID` | 左边中 |
| `GFX_ALIGN_CENTER` | 正中心 |
| `GFX_ALIGN_RIGHT_MID` | 右边中 |
| `GFX_ALIGN_BOTTOM_LEFT` | 左下 |
| `GFX_ALIGN_BOTTOM_MID` | 下边中 |
| `GFX_ALIGN_BOTTOM_RIGHT` | 右下 |

### 调整流程

1. 改 `layout.json` 里对应元素的 `x`/`y`/`width`/`height`；
2. **必须删旧资源包**（custom command 只依赖 build.py 脚本，感知不到 layout 变化）：
   ```sh
   rm -f build/expression_assets.bin
   python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
   ```
3. 烧录验证。

常用调整示例（240×320 竖屏）：

- 表情偏上/偏下：改 `eye_anim` 的 `y`（正数向下）
- 字幕位置：改 `toast_label` 的 `align` + `y`，宽度用 `width`
- 表情想放大：.eaf 动画尺寸固定（约 160px 见方），布局只能移动不能缩放

## 四、回退到 LVGL 风格

想暂时用回原 LVGL 界面（旧时钟、IP 显示等）：

- menuconfig：`Xiaozhi Assistant Application → ... → Emote animation style` 关掉，选回 default style；
- 或临时改 sdkconfig：
  ```sh
  sed -i '' 's/CONFIG_USE_EMOTE_MESSAGE_STYLE=y/CONFIG_USE_EMOTE_MESSAGE_STYLE=n/' sdkconfig
  idf.py build
  ```

board 代码里是 `#if CONFIG_USE_EMOTE_MESSAGE_STYLE` 分支，关闭后自动回到 `SpiLcdDisplay`（LVGL），无需改代码。

## 五、与 LVGL 风格的功能差异

| 功能 | LVGL 风格 | Emote 风格 |
|------|-----------|------------|
| 待机时钟 | 48px 大字时钟（`self.clock.set` MCP 工具可控制） | 引擎自带小时钟（顶部，无 MCP 控制） |
| AI 对话文本 | 大号多行文本区 | 顶部滚动字幕（toast） |
| IP 地址显示 | 有 | 无 |
| 歌词字幕 | 有（TF 卡音乐） | 无 |
| 摄像头预览 | 有 | 无（`SetPreviewImage` 不生效） |
| 表情动画 | 无 | 官方 11 种 .eaf 动画，随状态切换 |
| 配网二维码 | 有 | 有（引擎自带） |

## 六、常见问题

**Q1：构建报 `Invalid board configuration ... missing non-empty top-level "type"`**
`main/boards/` 下有被误识别为板配置的 `config.json`。⚠️ 表情布局目录（`emote_assets/*/`）里**绝不能放 `config.json`**——`scripts/build.py` 会递归扫描 `main/boards/**/config.json` 当板配置校验。布局目录里只放 `layout.json` + `emote.json`，字体/表情集合参数由 `main/CMakeLists.txt` 直调组件 `build.py` 显式传入。

**Q2：改了 layout.json 但屏上没变化**
资源包没重新打包。执行 `rm -f build/expression_assets.bin` 后重新编译（`scripts/build.py` 或 `idf.py build` 均可）。

**Q3：换了屏幕但表情位置不对**
确认编译日志里的 `resolution XXX_XXX` 是否为新屏对应目录；该目录缺 `layout.json` 时会回退官方 320×240 横屏布局。

**Q4：屏幕全黑**
- 确认烧录完整（assets 分区 0x800000 也烧了，用合并固件或 idf.py flash 都包含）；
- 确认 sdkconfig 里 `CONFIG_USE_EMOTE_MESSAGE_STYLE=y`、`CONFIG_FLASH_EXPRESSION_ASSETS=y`；
- 串口日志搜 `emote` 关键字看引擎初始化是否报错（字体/布局加载失败会打警告）。

**Q5：想换表情风格（小脸/大脸）**
当前用 `emoji_large`（大脸）。组件里还有 `emoji_small`，改 `main/CMakeLists.txt` 中 `AIROBOT_EMOTE_EMOJI_DIR` 指向 `emoji_small` 即可（布局里的偏移可能也要跟着微调）。

## 七、文件位置速查

| 内容 | 路径 |
|------|------|
| 布局文件 | `main/boards/bread-compact-wifi-s3cam-airobot/emote_assets/<分辨率>/layout.json` |
| 表情清单（.eaf 映射） | 同上目录 `emote.json` |
| 屏幕/emote 开关 | 本板 `config.json`（sdkconfig_append） |
| 分辨率→布局目录映射 | `main/CMakeLists.txt`（搜 `AIROBOT_EMOTE_RESOLUTION`） |
| 资源打包命令 | 同上（搜 `FLASH_EXPRESSION_ASSETS` 里的 airobot 分支） |
| 显示类切换 | 本板 `compact_wifi_board_s3cam_airobot.cc`（搜 `CONFIG_USE_EMOTE_MESSAGE_STYLE`） |
| 表情素材（组件，勿手改） | `managed_components/espressif2022__esp_emote_assets/` |
| 引擎代码（组件，勿手改） | `managed_components/espressif2022__esp_emote_expression/`、`.../esp_emote_gfx/` |
