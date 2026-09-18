# 设计规格：ESP-NOW 居家灯控与传感器（比赛现场演示）

- 日期：2026-09-18
- 分支：`feature/espnow-home-control`
- 板级：`main/boards/bread-compact-wifi-s3cam-airobot/`
- 状态：**待用户审查**
- 前置依赖：无（不复用未实现的改动）

## 1. 背景与目标

本板已有：云侧 AI 语音（MCP 工具机制）、Arduino 下位机（UART0 + `@`-文本协议 + 回执）、TF 卡本地音乐播放、闹钟、网页控制页。缺**居家生态**这一块。

用户需求：让机器人能**控制家里的灯、读取环境传感器**，并能**现场演示**（比赛场景）。

演示形态已确认：**桌面上摆真实灯与传感器**，用 **ESP-NOW** 连接 1~2 个自制 ESP32-S3 节点；机器人（主控）作协调者。不接 Home Assistant（下一阶段再评估，见 §8）。

目标：

1. 语音可控制节点上的**三通道 PWM RGB 灯**（开关 / 颜色 / 亮度）；
2. 语音可查询节点上的**传感器**（温湿度 / 距离 / 激光遮挡）；
3. 传感器事件可触发**本地预录语音播报**（复用 TF 卡播放链路）；
4. 节点**零配置**：不写死 SSID、不写死主控 MAC、不手配信道；
5. 不引入新分区、不新增常驻任务、不碰 core 代码、不改动现有行为。

## 2. 已确认的需求决策

| # | 决策点 | 结论 |
|---|---|---|
| 1 | 连通方案 | **ESP-NOW**（非 Home Assistant，非 Matter） |
| 2 | 节点数量 | 本批 **2 个**（节点 1「客厅」/ 节点 2「玄关」），协议预留 N 个 |
| 3 | 节点芯片/框架 | **ESP32-S3 + Arduino**（本机 arduino-esp32 **3.2.0** 已装，不升级） |
| 4 | 节点固件位置 | `arduino/EspNowNode/`（与 `arduino/MecanumRobot/` 平级） |
| 5 | 节点 1 硬件 | 三通道 PWM RGB 模块（R/G/B/GND）+ HC-SR04P 超声波 |
| 6 | 节点 2 硬件 | DHT11（单总线 3 线：+/-/out）+ 激光头模块 |
| 7 | 灯驱动 | 核心自带 `ledcAttach()` / `ledcWrite()`，**零第三方库** |
| 8 | 温湿度库 | `adafruit/DHT-sensor-library`（2026-03 仍在维护）+ `Adafruit Unified Sensor`；**排除 `DHTesp`（2023-04 后停更）** |
| 9 | 播报方式 | **本地预录 MP3**（设备侧无法自行 TTS，见 §5.6） |
| 10 | 信道策略 | 主控周期广播 beacon + 节点 hop 扫描自动发现，**SSID 完全不参与** |
| 11 | ESP-NOW 加密 | `begin(pmk)` 全网统一 PMK；单播 peer 带 LMK；广播 beacon 不加密（官方限制） |
| 12 | 引脚分配 | 由本设计指定（§3.1） |
| 13 | 协议格式 | 复用现有 `@`-文本行风格，与 UART 下位机同构 |

## 3. 架构与组件

### 3.1 硬件与引脚

两侧都用 ESP32-S3，引脚编号取两节点一致（接线方便），避开 USB(19/20)、flash/PSRAM(26-37)、UART0(43/44)、strapping(0/3/45/46)。

| 节点 | 器件 | 引脚 |
|---|---|---|
| 1「客厅」 | RGB 模块 R / G / B | GPIO4 / GPIO5 / GPIO6 |
| 1「客厅」 | HC-SR04P Trig / Echo | GPIO7 / GPIO15 |
| 2「玄关」 | DHT11 DATA | GPIO4 |
| 2「玄关」 | 激光模块 DO | GPIO5 |

- **HC-SR04P 是 3.3V 版本**，Echo 输出 3.3V，可直连 S3（普通 HC-SR04 的 5V Echo 会损伤芯片，不可直连）。
- **激光模块只接 DO**（数字输出，遮挡触发），AO 不接；演示时避免直射人眼。
- RGB 模块共阳/共阴未知：固件用 `#define RGB_COMMON_ANODE 1` 宏切换极性；现象是"颜色反相或常亮"时改该宏。

### 3.2 主控侧新增 `espnow_home.h/.cc`（板级）

单一职责：**ESP-NOW 传输 + 节点表 + 事件出口**。不含 MCP 工具注册（在板级主文件注册），不含播报（板级调 `LocalMusicPlayer`）。

```cpp
class EspNowHome {
public:
    // 事件出口：节点上报的原始载荷（evt/arg 已解析）
    using EventCallback = std::function<void(int node_id, const std::string& evt,
                                             const std::string& arg, uint32_t ts_ms)>;

    bool Begin(EventCallback cb);                     // esp_now_init + 广播 peer + beacon 定时器
    bool SendTo(int node_id, const std::string& body); // 下行正文，内部加 "@n<id> " 前缀
    bool IsOnline(int node_id, uint32_t within_ms = 15000) const;
    std::string NodesJson() const;                    // 在线/最后上线时间(JSON)
    void SetControlWindow(bool active);               // 由板级 SetPowerSaveLevel() 驱动
    // 接收回调内部实现，不对外暴露
};
```

- **节点表**：定长数组（`kMaxNodes = 4`），`{id, mac[6], last_seen_ms, online}`；MAC 由 beacon 阶段的学习过程填充，**不写死**。
- **发送**：`esp_now_send(mac, buf, len)`；`ifidx = WIFI_IF_STA`（与节点侧 `ESP_NOW_Peer(..., WIFI_IF_STA, ...)` 必须一致，否则收不到）。
- **beacon**：`esp_timer` 周期任务（**不新建 FreeRTOS 任务**）广播 `@beacon 1`；启动后 500ms × 前 60 次（≈30 秒），之后降到 3000ms 稳态。
  - 广播报文 ≤ 16 字节，对音频链路无感。
- **信道来源**：`WifiManager::GetInstance().GetChannel()`（`main/boards/common/wifi_board.cc:274` 已在用）。
- **待机也要收得到上报**：待机态是 `WIFI_PS_MAX_MODEM`（station 只在 DTIM 醒来），ESP-NOW 收包会被漏掉。对策 = ① 初始化时调用官方 `esp_now_set_wake_window()`（IDF v6 API，头文件注释明确 "could work at connected status"），窗口值列为**现场实测调优项**（先用默认值）；② 上行事件由节点**连发 3 次**（间隔 150ms，见 §3.6），跨过 DTIM 周期。
- **去重**：主控按 `(node_id, evt)` 做 1 秒窗口去重，抵消节点的 3 次重发。
- **内存**：节点表 + 收发缓冲各 1 个 ≤ 256B 的静态数组，总新增常量级，不动 PSRAM、不建队列。

### 3.3 主控侧 MCP 工具 `self.home.*`（板级主文件注册）

| 工具 | 参数 | 行为 |
|---|---|---|
| `self.home.light` | `node`(1/2), `on`(0/1), `r/g/b`(0-255, 可选), `brightness`(0-255, 可选) | 组包下发；返回"已完成"语义 |
| `self.home.sensor` | `node`(可选，缺省=全部在线节点) | 返回 JSON：温度/湿度/距离/激光/时间戳/`stale` |
| `self.home.status` | — | 节点在线状态（JSON） |
| `self.home.announce` | `name`(motion/beam/hot) | 手动触发预录播报（演示与自测用） |

- 工具描述与返回值遵守踩坑 15 的约定：**回"已完成 + 证据"，禁止出现"防抖/已忽略/失败"这类会被读成"没成功"的词**；描述里写明"调用一次即完成，不要重复调用确认"。
- 节点离线时返回**明确错误文本**（如"节点 1 当前离线，请检查其电源"），不返回假成功，避免 AI 自行脑补。

### 3.4 传感器缓存与事件出口（板级）

- 每个节点维护一份**最近一次上报缓存**（温湿度/距离/激光 + `ts_ms`）。`self.home.sensor` 直接读缓存（ESP-NOW 是异步的，不能"请求-应答"式阻塞等待）。
- 缓存超过 `kStaleMs = 30000` 标注 `stale: true`，AI 据此说明"数据可能过期"。
- 事件出口（回调）在板级做三件事：
  1. 更新缓存；
  2. 事件判定（§4.2 的迟滞与冷却）；
  3. 命中则触发播报（§3.5）。

### 3.5 播报（本地预录 MP3）

- **目录**：`/sdcard/announce/`（**独立于 `/sdcard/music/`**，因此不会被 `self.music.list` 当成歌曲列出）。
- **文件**（固定文件名，24kHz 单声道 96kbps，与项目转码规范一致）：

| 文件 | 触发场景 |
|---|---|
| `motion.mp3` | 超声波检测到有人靠近 |
| `beam.mp3` | 激光被遮挡（门口有人经过） |
| `hot.mp3` | 温度超过阈值 |

- **播放**：`LocalMusicPlayer` 新增**纯增量**公开方法 `bool PlayAnnounce(const char* name)`（内部拼路径后复用既有 `PlayOneSong()` 链路：AudioService 注入、打断机制、播放期间钉 Speaking）。不改动任何现有方法的行为。
- **生成脚本**：`scripts/gen_announce_mp3.sh`，macOS `say -v Tingting` 生成人声 → `ffmpeg -ar 24000 -ac 1 -b:a 96k` 转 MP3 → 输出到 `announce/` 目录，用户拷进 TF 卡 `/sdcard/announce/`。
- **播放条件**：仅在设备 `Idle` 状态播（避免打断对话/音乐）；播报中状态机行为完全复用现有本地播放逻辑。
- **冷却**：同一事件 `kAnnounceCooldownMs = 10000` 内只播一次。
- 未插卡 / 无该文件 → 静默跳过（页面上不刷日志），但 `self.home.announce` 返回值要说明"未找到播报文件"。

### 3.6 节点固件 `arduino/EspNowNode/EspNowNode.ino`

用 arduino-esp32 **核心自带**的官方 `ESP_NOW` 类（`libraries/ESP_NOW/src/ESP32_NOW.h`），**无需安装任何第三方库**（DHT11 除外，见 §3.7）。参照官方示例 `ESP_NOW_Network` 的结构。

```cpp
#include "WiFi.h"
#include "ESP32_NOW.h"
#define NODE_ID 1              // 2 号节点改成 2
#define HOP_INTERVAL_MS 200    // 未锁定时的换信道间隔
#define LOST_TIMEOUT_MS 5000   // 锁定后多久没收到主控包就回到 hop
```

- **信道自发现（核心）**：
  1. `WiFi.mode(WIFI_STA)` → `WiFi.setChannel(ch)`（ch 从 1 循环到 13，每 200ms 换一次，**不连接任何 AP**）；
  2. `ESP_NOW.begin(pmk)` + `ESP_NOW.onNewPeer(cb, arg)`；
  3. 收到主控 beacon（未注册 peer 的数据会进 `onNewPeer`）→ 锁定**当前信道** + 用来源 MAC `addPeer(mac, ch, WIFI_IF_STA, lmk)`；
  4. 锁定后每次收到主控任何包刷新 `last_seen`；`now - last_seen > LOST_TIMEOUT_MS` → 清 peer、回 hop。
- **收发**：自定义 `Peer : public ESP_NOW_Peer` 子类，override `onReceive(data, len, broadcast)`（解析下行 `@...`）与 `onSent(success)`。
- **传感器采样**（非阻塞节奏，不用 `delay()` 卡住 loop）：
  - 超声波：`pulseIn(Echo, HIGH, 30000)`（阻塞 ≤30ms，周期 100ms）；
  - DHT11：周期 **5000ms**（DHT11 本身 ≤1Hz），失败重试 3 次，仍失败则上报 `@n<id> err dht`；
  - 激光：周期 50ms 读 DO，**边沿触发**（0→1 或 1→0），并做 2 次采样确认去抖。
- **事件上报**：`@n<id> evt <name> <arg>`（见 §4.1），**同一事件连发 3 次、间隔 150ms**（主控去重，见 §3.2）。
- **不使用 `Serial`**（节点独立运行，不接串口）；可用一个 GPIO 接 LED 做在线指示（可选，不阻塞）。
- **失联自恢复**：回 hop 模式即可，无需重启。

### 3.7 节点依赖（需用户确认后安装）

```bash
arduino-cli lib install "DHT sensor library"      # 自动带上 Adafruit Unified Sensor
arduino-cli core list                              # 确认 esp32:esp32 3.2.0 已装
arduino-cli compile --fqbn esp32:esp32:esp32s3 main/boards/bread-compact-wifi-s3cam-airobot/arduino/EspNowNode
```
- 除 DHT 库外**零新增依赖**；灯用核心自带 `ledc`，无线用核心自带 `ESP_NOW`。
- DHT11 的已知风险：单总线时序会被 WiFi 中断干扰，偶发校验失败。缓解 = 5 秒周期 + 3 次重试 + 失败上报。
  **后备方案（不阻塞本次实现）**：换 I2C 的 SHT30/DHT20，只需替换节点里的读取函数（约 10 行），协议与主控侧完全不变。

## 4. 数据流

### 4.1 协议（两侧完全一致）

文本行，`@` 前缀，单包 ≤ 200 字节（IDF v1.0 上限 250B）。**节点号由 `@n<id>` 前缀承载，不出现在参数列表里。**

| 方向 | 报文 | 含义 |
|---|---|---|
| 主控→节点 | `@beacon 1` | 发现广播（含协议版本） |
| 主控→节点 | `@n1 light 1 0 0 255 200` | on / r / g / b / brightness（5 个参数） |
| 主控→节点 | `@n1 ping` | 保活探测 |
| 节点→主控 | `@n1 evt dist 23` | 超声波距离 cm |
| 节点→主控 | `@n1 evt temp 26 55` | 温度 ℃ / 湿度 % |
| 节点→主控 | `@n1 evt beam 1` | 激光遮挡（1=遮挡，0=恢复） |
| 节点→主控 | `@n1 evt motion 1` | 人体靠近（迟滞判定后的结果） |
| 节点→主控 | `@n1 ack light 1` | 执行确认 |
| 节点→主控 | `@n1 err dht` | 传感器读取失败 |

**重发策略（两侧统一）**：关键报文（下行 `light`、上行 `evt`）一律**连发 3 次、间隔 150ms**。命令是幂等的（重复执行无害），这样无需应用层 ACK 就能跨过待机 DTIM 唤醒窗口导致的漏包；`ack` 只作节点执行回执（供状态显示），**不作为可靠性依赖**。

### 4.2 三条链路

```
① 语音控灯
   用户说"打开客厅灯" → 云 LLM → self.home.light(1,1) → EspNowHome::SendTo(1,"light 1 ...")
   → ESP-NOW 单播 → 节点 onReceive → ledcWrite ×3 → @n1 ack light 1 → 工具返回"已完成"

② 传感器事件播报（不用说话，现场最抓人）
   超声波 < 30cm（节点迟滞判定）→ @n1 evt motion 1 → 板级事件出口
   → 冷却检查 → LocalMusicPlayer::PlayAnnounce("motion.mp3") → 喇叭播报"检测到有人靠近"

③ 数据查询
   DHT11 每 5 秒 → @n2 evt temp 26 55 → 板级缓存
   用户问"现在多少度" → self.home.sensor → 读缓存 → 返回 JSON → 云 LLM 组织成口语
```

### 4.3 事件判定（迟滞 + 冷却，防抖刷屏）

| 事件 | 节点侧迟滞 | 主板侧冷却 |
|---|---|---|
| 人体靠近 | 距离 < 30cm 触发、> 40cm 复位（避免在阈值上反复跳） | 10 秒 |
| 激光遮挡 | 连续 2 次采样一致才上报边沿 | 10 秒 |
| 温度过高 | 节点不做判定，只上报原始值 | 主控侧 `> 28℃` 触发、`< 26℃` 复位 + 10 秒冷却 |

判定放在**节点侧（迟滞）+ 主控侧（冷却+阈值）**：节点只做"抗抖动"，业务阈值留在主控侧便于语音调整。

## 5. 关键约束与理由

1. **内部 SRAM 仅 20~25KB 空闲**（踩坑 16）→ 不建常驻任务（beacon 用 `esp_timer` 回调）、缓冲全部静态定长、ESP-NOW 接收回调只做解析+更新缓存。
2. **UART0 与 Arduino 下位机共用**（AGENTS.md + 踩坑记录）→ 新代码**一律不加 `ESP_LOG`**，失败通过返回值/工具文本表达。
3. **待机 `WIFI_PS_MAX_MODEM`**（踩坑 7）→ 控制期间把 WiFi 提到 `PERFORMANCE`：复用板级已有的 `SetPowerSaveLevel()` override，由 `EspNowHome::SetControlWindow()` 驱动（与现有 `web_control_active_` 同法、同处）。
4. **单 2.4G radio 与云端音频共存** → 控制报文 <40B；稳态 beacon 3 秒一次；**不做周期性心跳刷屏**（只按需 `@n1 ping`，默认 5 秒且仅在有控制窗口时启用）。
5. **官方 API 事实**（已核对本机源码）：
   - IDF v6.0.2：`esp_now_register_recv_cb()` 用 `esp_now_recv_info_t*`；`esp_now_add_peer()` 的 `ifidx` 两侧必须都是 `WIFI_IF_STA`；
   - arduino-esp32 3.2.0：官方 `ESP_NOW` 类 + `onNewPeer()` 是节点发现的官方机制；`neopixelWrite()` 已废弃（本设计不用它，用 `ledc`）。
6. **播报为什么必须本地预录**：项目协议层可主动发送的只有 `SendWakeWordDetected / SendStartListening / SendStopListening / SendAbortSpeaking / SendMcpMessage`，**没有"设备主动请求 TTS"的接口**；`NotifyPlayer` 播的是**服务端下发的音频 URL**。所以设备侧无法自行生成语音 → 预录 MP3 是唯一场内可控方案。
7. **协议与下位机同构**的理由：现有 `@`-文本 + 回执 + 防抖 + 回执语义的踩坑经验（踩坑 2/5/15）可**直接复用**，降低重复踩坑概率。
8. **待机省电与事件上报的矛盾（本设计的关键取舍）**：待机 `WIFI_PS_MAX_MODEM` 下 station 只在 DTIM 醒来，ESP-NOW 上报会漏；但演示又要"手一挥就播报"。取舍 = **不做全局提频**（会牺牲待机功耗，且踩坑 7 已确认省电是刻意设计），改由 `esp_now_set_wake_window()` + 节点连发 3 次 + 主控去重兜住（§3.2 / §6）；仅当现场实测仍丢包时，才把 wake window 调大。

## 6. 错误处理

| 场景 | 行为 |
|---|---|
| 超时判据（两个，勿混） | **节点侧**：5 秒收不到主控任何包 → 清 peer、回 hop 重发现；**主控侧**：15 秒收不到节点任何包 → 才向 AI 报"离线"（更宽松，避免瞬时抖动被报成离线） |
| 待机漏收上行事件 | 节点连发 3 次（150ms 间隔）+ 主控 `(node_id, evt)` 1 秒去重；仍丢则现场调大 `esp_now_set_wake_window()` |
| 节点离线（>15s 无包） | 工具返回明确文本"节点 N 离线，请检查电源"；不返回假成功 |
| 发送失败 | 返回值说明原因；**不自动重试**（避免 AI 陷入重试循环，踩坑 15） |
| DHT11 读失败 | 节点重试 3 次 → 仍失败上报 `@nN err dht`；主控保留上次值并标 `stale` |
| 信道失配 / 主控换热点 | 节点失联 5 秒 → 回 hop 重发现；无需重烧、无需重启 |
| 主控重启 | beacon 恢复广播，节点在 5 秒内重新锁定 |
| 传感器抖动 | 节点侧迟滞（§4.3）+ 主控侧 10 秒冷却 |
| 播报文件缺失 | 静默跳过；`self.home.announce` 返回"未找到播报文件 xxx.mp3" |
| 播报与对话冲突 | 仅 `Idle` 时播；播放中复用现有打断机制 |
| 无 TF 卡编译变体 | 播报能力整体关闭（`CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD` 未开时不注册相关逻辑） |

## 7. 测试策略

沿用项目 `scripts/tests/` 的既有风格（Python + 源码文本断言 + 纯逻辑复刻），无硬件也能跑。

1. `scripts/tests/test_espnow_home_protocol.py`
   - 复刻并断言：报文组包/解析（`@n1 light ...` / `@n1 evt temp ...`）、非法报文（超长、缺字段、坏 node id）被拒绝而不崩；
   - 复刻迟滞（30/40cm）与冷却（10s）判定，固化边界值；
   - 静态断言：`espnow_home.cc` 内**不得出现 `ESP_LOG`**（本板 UART0 共享约定）；
   - 静态断言：`self.home.light` 工具描述含"不要重复调用"，且返回文本不含"防抖/已忽略"字样（踩坑 15 回归防护）。
2. 节点固件编译门禁：`arduino-cli compile --fqbn esp32:esp32:esp32s3 .../arduino/EspNowNode` 必须 0 错误 0 警告。
3. 主控固件：`python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot` 通过。
4. 真机验证清单见 §9。

## 8. 不做的（YAGNI）

- **Home Assistant / Matter 接入**：本阶段不做（HA 需现场带服务器 + 局域网，且本板无 TLS 栈只能明文局域网；Matter controller 在 20~25KB 内部 SRAM 下不现实）。作为下一阶段独立设计。
- ESP-NOW 动态配对/密钥协商（用固定 PMK+LMK 足够）。
- 跨信道 `esp_now_remain_on_channel()` / `esp_now_switch_channel_tx()`（IDF v6 新 API，本设计的 hop 方案已够用）。
- 节点 OTA、节点配网页面、节点电量上报。
- 传感器历史数据存储 / 曲线图（只在主控内存里保留最近一次）。
- 多主控、网状网络、超过 4 个节点。
- 网页端灯控 UI（语音 + 现有网页结构不动）。

## 9. 真机验证要点（需硬件）

1. 节点上电 → **3 秒内**被主控发现（`self.home.status` 显示在线）。
2. 语音「打开客厅灯」→ 灯亮；「调成蓝色」→ 变蓝；「暗一点」→ 变暗；「关灯」→ 灭。
3. 手靠近超声波（<30cm）→ 灯自动亮 + 喇叭播报「检测到有人靠近」（同一动作 10 秒内不重复播报）。
4. 问「现在室内多少度」→ 返回温湿度且与实物温度计接近（DHT11 ±2℃）。
5. 手挡激光 → 播报「门口有人经过，请注意」。
6. 拔节点电源 30 秒再插 → 自动恢复在线（信道重发现）。
7. 主控换热点（信道变化）→ 节点自动重锁定，**无需重烧固件**。
8. 演示全程 AI 对话不卡顿、无重启；网页日志无 ERROR；`free sram` 不低于改动前水平（拍照等高内存操作时仍可正常）。
9. 拔掉 TF 卡 → 播报静默跳过，语音控灯与查询仍然正常。

## 10. 交付物清单

| 类型 | 路径 |
|---|---|
| 节点固件（Arduino） | `main/boards/bread-compact-wifi-s3cam-airobot/arduino/EspNowNode/EspNowNode.ino` |
| 节点说明 | `.../arduino/EspNowNode/README.md` |
| 主控传输层 | `.../espnow_home.h` / `espnow_home.cc` |
| 主控工具与事件出口 | `.../compact_wifi_board_s3cam_airobot.cc`（增量） |
| 播报接口 | `.../local_music_player.h/.cc`（纯增量方法） |
| 播报音频生成 | `.../scripts/gen_announce_mp3.sh` |
| 测试 | `scripts/tests/test_espnow_home_protocol.py` |
| 文档 | 本文件 + 板级 `README.md` 新增章节 |
