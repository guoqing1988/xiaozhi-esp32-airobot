# EspNowNode — XiaoZhi 居家演示节点

ESP32-S3 节点固件，用 **arduino-esp32 核心自带**的 `ESP_NOW` 类（`ESP32_NOW.h`），
**不连接任何 AP**：开机在信道 1..13 之间 hop，收到主控的 `@beacon` 广播后锁定当前信道
并登记主控，之后双向通信。因此：

- **不需要配置 SSID / 密码 / 信道 / 主控 MAC**；
- 主控换热点、换信道，或节点/主控重启，都能自动重新发现（失联 5 秒即回到 hop）；
- **节点自描述能力**：主控固件里没有本节点的任何硬编码信息，接新设备**主控零改动**。

## 依赖

- arduino-esp32 核心 **3.2.0**（本机已装）
- **节点 1（客厅灯）**：零第三方库（灯用核心自带 `ledc`，无线用核心自带 `ESP_NOW`）
- **节点 2（玄关感应）**：需要 DHT 库：

```bash
arduino-cli lib install "DHT sensor library"   # 自动带上 Adafruit Unified Sensor
```

> 固件里 `#include <DHT.h>` 放在 `#if NODE_ID == 2` 内，所以**编译节点 1 时不需要装这个库**。

## 协议（与主控 `espnow_home.cc` 一致，`@` 前缀文本行，单包 ≤200B）

| 方向 | 报文 | 用途 |
|---|---|---|
| 节点→主控 | `@n1 info 客厅灯 light(RGB灯):on(0\|1),rgb(r,g,b);dist(距离cm,只读):read()` | **能力自描述**：锁信道后 + 每次收到 beacon 重报 |
| 主控→节点 | `@n1 do light rgb 0 0 255` | 通用动作（主控**原样透传、不解释语义**） |
| 节点→主控 | `@n1 ok light 0 0 255` | 执行回执（主控存进该设备的状态） |
| 节点→主控 | `@n1 say motion` | **播报请求**：主控播 `/sdcard/announce/motion.mp3` |
| 节点→主控 | `@n1 evt temp 26 55` | 状态上报（只更新状态，**不播报**） |
| 节点→主控 | `@n1 err unknown-cap light` | 错误（主控会把它显示在状态里） |

**为什么 `say` 与 `evt` 要分开**：`dist` 这类状态 5 秒上报一次，如果走播报通道，
主控会每 5 秒去 SD 卡找一次文件。分开后语义明确：**要出声就用 `say`，只是更新数据就用 `evt`**。

### 分隔符约定（改协议前必读）

- **字段之间用空格**：`do <能力> <动作> [参数…]`。`args` 是**剩余整段**，
  内部如何分隔由各能力的 handler 决定：本固件的 `parseNums()` 把**任何非数字字符**都当分隔符，
  所以 `0 0 255`、`0,0,255`、`[0,0,255]` 都能正确解析（AI 生成的参数格式并不统一）。
- **为什么不用 `-`**（虽然 UART 下位机协议 `@tj-10` 是那样）：`info` 的能力规格文本里已经用了 `-`
  （范围 `bright(0-255)`），同理 `,` `;` `:` `|` `(` `)` 都已被规格语法占用。
  **空格是唯一不与规格文本冲突的字符**，也是主控与节点两侧的共同约定。
- **容错**：主控侧对 `cap`/`action`/`args` 做 `trim`；节点侧 `nextField()` 跳过前导与连续空格。
  所以 AI 多打一个空格也不会导致命令被静默丢弃——协议层不会默不作声：
  解析不了就回 `@n<id> err <原因>`，主控会把它写进该设备的状态里。

## 能力表怎么填（接新设备的关键）

节点固件里**唯一**的设备描述来源是这张表，改它就能改 AI 看到的设备名与能力：

```cpp
static const CapDef kCaps[] = {
    //  能力名   规格：描述 + 动作(参数)                                 处理函数
    {"light", "(RGB灯):on(0|1),off(),rgb(r,g,b),bright(0-255),read()", capLight},
    {"dist",  "(超声波距离cm,只读):read()",                            capDistRead},
};
static const char kNodeName[] = "客厅灯";     // AI 与语音里对它的称呼
```

规则：

1. **能力名**：AI 用它搭配动作调用（`do light on 1`）。用小写英文，别用中文或空格。
2. **规格文本**：`(人类可读描述):动作(参数),动作(参数)`。**描述要写给 AI 看**——
   它会据此决定怎么调你；`|` 表示"或"，`-` 表示范围。
3. **动作名**：建议用标准词汇 `on / off / set / read`（AI 首次命中率最高），
   但**任何自定义名字都能用**（主控不校验，直接透传）。
4. **handler**：`bool f(const char* action, const char* args, char* out, size_t out_len)`，
   成功把结果文本写进 `out` 返回 `true`；不认识的动作返回 `false`（主控会收到
   `err unknown-action`）。只上报数据、不接受命令的能力填 `nullptr`（会回 `err readonly`）。
5. **单包 ≤200B**：`info` 报文总长有限，描述别写太长；主控只保留**前 3 个**能力。
6. **只读数据也要给 `read()` 动作**（返回缓存值、不要在里面阻塞采样），
   这样 AI 能主动问"现在多少度"，而不是只能等你自己上报。

**自动动作也在这个文件里**：例如"人靠近自动开灯"就是 `sonarTick()` 里
`light_on = 1; applyLight(); queueEvt("motion","1"); queueSay("motion");`——
主控完全不知道这件事，换成"自动开风扇"也只需改这里。

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

# 节点 1（客厅灯：RGB + 超声波）—— 保持文件顶部 #define NODE_ID 1
arduino-cli compile --fqbn esp32:esp32:esp32s3 .
arduino-cli upload  --fqbn esp32:esp32:esp32s3 -p /dev/cu.usbserial-XXXX .

# 节点 2（玄关感应：DHT11 + 激光）—— 把 #define NODE_ID 改成 2 后重新编译烧录
arduino-cli compile --fqbn esp32:esp32:esp32s3 .
arduino-cli upload  --fqbn esp32:esp32:esp32s3 -p /dev/cu.usbserial-XXXX .
```

## 现场可调项（都在文件顶部）

| 宏 | 默认 | 作用 |
|---|---|---|
| `NODE_ID` | 1 | 1=客厅灯(RGB+超声波)，2=玄关感应(DHT11+激光) |
| `RGB_COMMON_ANODE` | 1 | 共阳模块设 1；颜色反相/关不掉时改成 0 |
| `MOTION_TRIGGER_CM` / `MOTION_RELEASE_CM` | 30 / 40 | 人体靠近的迟滞触发与复位距离 |
| `HOT_TRIGGER_C` / `HOT_RELEASE_C` | 28 / 26 | 高温播报的触发与复位阈值（**属于本节点的语义**） |
| `DIST_REPORT_DELTA_CM` | 3 | 距离变化达 3cm 才上报（降频，避免占满 2.4G） |
| `EVENT_RESEND` / `kEventResendGapMs` | 3 / 150 | 上行连发次数与间隔（对抗主控待机漏包） |
| `HOP_INTERVAL_MS` / `LOST_TIMEOUT_MS` | 200 / 5000 | 信道 hop 间隔 / 失联回 hop 判定 |

## 密钥必须与主控一致

`kPmk` / `kLmk`（各 16 字节，字符串最多 15 个字符）必须与主控
`espnow_home.cc` 中的同名常量**逐字节一致**：

```cpp
static const uint8_t kPmk[16] = "xiaozhi-pmk-01";
static const uint8_t kLmk[16] = "xiaozhi-lmk-01";
```

不一致的现象是「**完全收不到任何包**」（不是偶发失败、不是部分功能异常），排查时优先核对这两处。

## 排错

| 现象 | 检查 |
|---|---|
| 主控里看不到本设备 | 是否上电满 3 秒（hop 找信道）；主控 `self.home.devices` 里 `info_seen=false` 说明 `info` 没发出去 |
| 主控显示离线 | **密钥是否与主控一致**；同信道 2.4G 干扰 |
| 控制无反应 | 能力名/动作名拼写（大小写敏感）；看主控状态里是否出现 `err=unknown-cap` / `err=unknown-action` |
| 颜色反相 / 关不掉 | 改 `RGB_COMMON_ANODE`（0/1 取反） |
| 播报不响 | 主控需在**待机**状态（对话中不插嘴）；TF 卡 `/sdcard/announce/<名字>.mp3` 是否存在 |
| 温度一直失败 | DHT11 数据线接触、供电 3.3V；DHT11 本身 ≤1Hz，5 秒一次属正常节奏 |
| 现场丢包（偶发不响应） | 调大主控 `esp_now_set_wake_window()`（见板级 README 说明） |

## 真机验证

1. 上电 3 秒内，主控 `self.home.devices` 显示本设备、`online: true`、`info_seen: true`；
2. 语音「打开客厅灯」→ 灯亮；「调成蓝色」→ 变蓝；「关灯」→ 灭；
3. 手靠近超声波（<30cm）→ 灯自动亮 + 播报 `motion.mp3`（10 秒内不重复）；
4. 节点 2：手挡激光 → 播报 `beam.mp3`；问「室内多少度」→ 返回温湿度；温度 ≥28℃ 播 `hot.mp3` 一次；
5. 拔电重插 → 自动重新上线，能力不丢（`info` 会重报）。
