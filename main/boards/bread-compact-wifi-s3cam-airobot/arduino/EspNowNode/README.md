# EspNowNode — XiaoZhi 居家演示节点

ESP32-S3 节点固件，用 **arduino-esp32 核心自带**的 `ESP_NOW` 类（`ESP32_NOW.h`），
**不连接任何 AP**：开机在信道 1..13 之间 hop，收到主控的 `@beacon` 广播后锁定当前信道
并登记主控，之后双向通信。因此：

- **不需要配置 SSID / 密码 / 信道 / 主控 MAC**；
- 主控换热点、换信道，或节点/主控重启，都能自动重新发现（失联 5 秒即回到 hop）。

## 依赖

- arduino-esp32 核心 **3.2.0**（本机已装）
- **节点 1（客厅）**：零第三方库（灯用核心自带 `ledc`，无线用核心自带 `ESP_NOW`）
- **节点 2（玄关）**：需要 DHT 库：

```bash
arduino-cli lib install "DHT sensor library"   # 自动带上 Adafruit Unified Sensor
```

> 固件里 `#include <DHT.h>` 放在 `#if NODE_ID == 2` 内，所以**编译节点 1 时不需要装这个库**。

## 接线

| 功能 | GPIO | 说明 |
|---|---|---|
| RGB 模块 R / G / B | 4 / 5 / 6 | 4 线模块（R/G/B/GND）。共阳模块把 `RGB_COMMON_ANODE` 设为 1 |
| HC-SR04P Trig / Echo | 7 / 15 | **必须用 3.3V 版（型号带 P）**；普通 5V 版 HC-SR04 的 Echo 输出 5V，会损伤芯片 |
| DHT11 DATA | 4 | 3 线模块（+/-/out），模块自带上拉 |
| 激光模块 DO | 5 | 只接数字输出 DO，AO 不接 |

> 演示用低压供电（USB 5V 灯带 / 灯珠），**不要接 220V 市电**。

## 编译与烧录

```bash
cd main/boards/bread-compact-wifi-s3cam-airobot/arduino/EspNowNode

# 节点 1（客厅：RGB + 超声波）—— 保持文件顶部 #define NODE_ID 1
arduino-cli compile --fqbn esp32:esp32:esp32s3 .
arduino-cli upload  --fqbn esp32:esp32:esp32s3 -p /dev/cu.usbserial-XXXX .

# 节点 2（玄关：DHT11 + 激光）—— 把 #define NODE_ID 改成 2 后重新编译烧录
arduino-cli compile --fqbn esp32:esp32:esp32s3 .
arduino-cli upload  --fqbn esp32:esp32:esp32s3 -p /dev/cu.usbserial-XXXX .
```

## 现场可调项（都在文件顶部）

| 宏 | 默认 | 作用 |
|---|---|---|
| `NODE_ID` | 1 | 1=客厅(RGB+超声波)，2=玄关(DHT11+激光) |
| `RGB_COMMON_ANODE` | 1 | 共阳模块设 1；颜色反相/关不掉时改成 0 |
| `MOTION_TRIGGER_CM` / `MOTION_RELEASE_CM` | 30 / 40 | 人体靠近的迟滞触发与复位距离 |
| `DIST_REPORT_DELTA_CM` | 3 | 距离变化达 3cm 才上报（降频，避免占满 2.4G） |
| `EVENT_RESEND` / `kEventResendGapMs` | 3 / 150 | 上行连发次数与间隔（对抗主控待机漏包） |
| `HOP_INTERVAL_MS` / `LOST_TIMEOUT_MS` | 200 / 5000 | 信道 hop 间隔 / 失联回 hop 判定 |

## 密钥必须与主控一致

`kPmk` / `kLmk`（各 16 字节，字符串最多 15 个字符）必须与主控
`main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.cc` 中的同名常量**逐字节一致**：

```cpp
static const uint8_t kPmk[16] = "xiaozhi-pmk-01";
static const uint8_t kLmk[16] = "xiaozhi-lmk-01";
```

不一致的现象是「**完全收不到任何包**」（不是偶发失败、不是部分功能异常），排查时优先核对这两处。

## 排错

| 现象 | 检查 |
|---|---|
| 灯不响应 | 主控问「居家节点状态」或看 `self.home.status`：该节点 `online` 是否为 true |
| 一直显示离线 | 节点是否上电；等 3 秒让它 hop 到主控信道；**密钥是否与主控一致** |
| 颜色反相 / 关不掉 | 改 `RGB_COMMON_ANODE`（0/1 取反） |
| 温湿度一直失败 | DHT11 数据线接触、供电 3.3V；DHT11 本身 ≤1Hz，5 秒一次属正常节奏 |
| 手靠近不播报 | 主控需在**待机**状态（对话中不插嘴）；TF 卡 `/sdcard/announce/motion.mp3` 是否存在 |
| 现场丢包（偶发不响应） | 调大主控 `esp_now_set_wake_window()`（见板级 README 说明） |

## 真机验证

1. 上电 3 秒内，主控 `self.home.status` 显示该节点 `online: true`；
2. 语音「打开客厅灯」→ 灯亮；「调成蓝色」→ 变蓝；「关灯」→ 灭；
3. 手靠近超声波（<30cm）→ 主控待机时播放 `motion.mp3`（10 秒内不重复播）；
4. 节点 2：手挡激光 → 播放 `beam.mp3`；问「室内多少度」→ 返回温湿度。
