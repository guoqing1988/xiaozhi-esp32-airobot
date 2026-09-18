# ESP-NOW 居家设备"自动发现 + 数据驱动"重构 设计

- 日期：2026-09-18
- 状态：**设计已获批准，待实现**
- 关联：`main/boards/bread-compact-wifi-s3cam-airobot/`（主控）、`.../arduino/EspNowNode/`（节点）
- 前序设计：`2026-09-18-espnow-home-control-design.md`（本文是它的架构修正）

## 1. 要解决的问题

首版把**节点角色与能力硬编码在主控固件里**：

- `self.home.light(node=1)`：节点 1=客厅、能力=灯，写死在主控；
- `OnHomeEvent()` 里 `evt == "temp"` / `"beam"` / `"motion"` 分支，写死事件语义；
- 高温阈值 28℃ 判在主控，播报文件名 `hot.mp3` 映射也写死。

后果：**每接一个新设备都要改主控固件、重新烧录主控**——这与"随意调整和接入"的目标直接冲突。

本文把上述一切改成**运行时数据**：节点上线时自描述"我叫什么、有什么能力、怎么调"，主控只做"存起来 + 转给 AI"，完全不理解业务语义。

## 2. 已确认的关键决策

| # | 决策 | 理由 |
|---|---|---|
| 1 | **不做**动态 `McpServer::AddTool()`，工具集固定 3 个 | ① 设备侧**没有** `RemoveTool`（`tools_` 只增不减），节点离线后工具撤不回来；② 工具列表由云端在连接初始化后**主动拉取一次**（`docs/mcp-protocol.md:234-249`），而 ESP-NOW 节点 hop 找信道需 2~3 秒，晚于拉取时刻上线的工具云端根本看不到。这条路的效果取决于服务端何时重拉，不可控 |
| 2 | 注册表放**内存**，不落 TF 卡 | ① 节点每次收到 beacon 会重发 `info`，主控重启也能自愈；② 离线设备的能力清单没有控制价值（AI 说"客厅灯离线"比"不存在"有用，但两者都不需要持久化）；③ 本板 TF 卡是**可选**能力（有 `no-tfcard` 变体），依赖它等于自造降级路径；④ 要"拔电重插还记得"应由**节点自己**把能力表放固件/NVS，与主控无关 |
| 3 | 动作语义：**标准词汇约定 + 任意透传**（方案 C） | 协议**建议**节点用 `on/off/set/read` 等标准动作名（提升 AI 首次命中率），但主控对动作名**不做任何校验和解释**，任何自定义动作照原样转发；能力规格里带人类可读描述供 AI 理解 |
| 4 | 高温等业务阈值**移到节点侧** | 主控不该知道"28℃ 算热"。节点自己判定后发 `say hot`，主控只负责播 `hot.mp3` |
| 5 | 播报改为**数据驱动**：`say <名字>` → 播 `/sdcard/announce/<名字>.mp3` | 主控不再有事件名→文件名的映射表。以后接烟雾传感器，节点发 `say smoke`、卡上放 `smoke.mp3` 即生效，主控零改动 |
| 6 | 删除旧工具 `self.home.light` / `self.home.sensor` / `self.home.status` | 它们正是"硬编码节点号+能力"的载体；`self.home.devices` + `self.home.control` 完全覆盖。已有演示台词（"打开客厅灯"）不受影响 |
| 7 | `kMaxNodes` 保持 4，每节点最多 3 个能力 | 演示只需 2 节点；静态数组约 1.5KB，需实测 free sram，紧张则降到 2 |

## 3. 架构

```
[云端 LLM]
   │ MCP tools/call（工具集固定）
   ▼
┌─────────────────────────────────────────────┐
│ 主控 MCP 工具层（3 个，全部语义无关）        │
│   self.home.devices  查注册表                │
│   self.home.control  透传动作                │
│   self.home.announce 本地播报                │
└─────────────────────────────────────────────┘
   │                        ▲
   │ 读/写                   │ 播报
   ▼                        │
┌──────────────────────┐  ┌──────────────────┐
│ 设备注册表（内存）    │  │ LocalMusicPlayer │
│ 名字/能力/状态/在线   │  │ /sdcard/announce │
└──────────────────────┘  └──────────────────┘
   │ ESP-NOW 文本行协议
   ▼
[节点] 固件常量里的"能力表"：
        info 上报 → do 执行 → ok 回执 + say/evt 主动上报
```

**主控完全不认识 `light`、不认识节点 1 是客厅、不认识 `motion.mp3`。**

## 4. 协议（在首版基础上扩展）

| 方向 | 报文 | 说明 |
|---|---|---|
| 节点→主控 | `@n1 info 客厅灯 light(RGB灯):on(0\|1),rgb(r,g,b),bright(0-255);dist(距离cm):read()` | **能力自描述**。锁定信道后立即上报；此后每次收到 beacon 重报一次（幂等覆盖）。连发 3 次（同 evt 可靠性策略） |
| 主控→节点 | `@n1 do light on 1` | 通用动作，**主控不解释**，原样透传 |
| 节点→主控 | `@n1 ok light 1 0 0 255` | 执行回执，进状态缓存 |
| 节点→主控 | `@n1 say motion` | **播报请求**：主控播 `<名字>.mp3`，文件不存在则静默跳过 |
| 节点→主控 | `@n1 evt dist 23` | 状态/事件上报，只进状态缓存，**不播报** |
| 节点→主控 | `@n1 err unknown-cap light` | 错误，进状态缓存 |

**语法要点**

- 能力规格：`能力名(人类可读描述):动作(参数),动作(参数)`；多个能力用 `;` 分隔。
- 主控解析 `info` 时**只做两件事**：按 `;` 拆能力、取每项 `(` 之前的名字作为能力名，其余原样保留给 AI。
- `say` 与 `evt` 分成两个通道的理由：`dist` 这类高频状态 5 秒一次，若走播报通道会反复触发 SD 卡文件查找；分开后语义明确、无歧义。
- 单包上限仍为 200B（`kMaxPacketLen`）：节点侧须保证 `info` 塞得下，超长自行省略描述文字。

## 5. 主控注册表（`espnow_home.h`）

```cpp
static constexpr int kMaxNodes   = 4;
static constexpr int kMaxCaps    = 3;    // 每节点最多能力数
static constexpr int kNameLen    = 20;   // 设备名（UTF-8，中文约 6 字）
static constexpr int kCapNameLen = 14;
static constexpr int kCapSpecLen = 48;
static constexpr int kStateLen   = 96;   // "light=1 0 0 255 temp=26 55" 形式

struct CapEntry   { char name[kCapNameLen]; char spec[kCapSpecLen]; };
struct DeviceEntry {
    bool known = false;
    int id = 0;
    uint8_t mac[6] = {0};
    char name[kNameLen] = {0};
    CapEntry caps[kMaxCaps];
    int cap_count = 0;
    char state[kStateLen] = {0};   // 通用 key=value 状态缓存（非结构化，主控不解释）
    int64_t last_seen_ms = 0;
    bool info_seen = false;        // 是否已收到能力描述
};
```

内存：每节点约 374B × 4 ≈ **1.5KB**（原 `NodeEntry` 为 96B）。实现后必须实测 `free sram`。

**上行回调签名变更**（区分状态与播报两类通道）：

```cpp
using EventCallback = std::function<void(int node_id, const std::string& kind,  // "evt" | "say"
                                         const std::string& name, const std::string& arg,
                                         int64_t ts_ms)>;
```

**新增公开接口**（供 MCP 工具与板级使用）：

```cpp
std::string DevicesJson() const;                    // 设备注册表全量 JSON
bool        HasNode(int node_id) const;             // 是否见过该节点
const char* NodeName(int node_id) const;            // 设备名（未知返回 "节点<N>"）
bool        HasCap(int node_id, const std::string& cap) const;  // 能力是否存在（用于给出可读错误）
```

**状态缓存更新**（在 `HandleRecv` 内，WiFi 任务上下文，零堆分配）：

- `ok <cap> <结果>` → `UpsertState(cap, 结果)`
- `evt <name> <arg>` → `UpsertState(name, arg)`
- `err <原因>` → `UpsertState("err", 原因)`
- `info <名字> <能力>` → 覆盖 `name` / `caps` / `cap_count`，`info_seen = true`

`UpsertState(key, value)`：在 `state_` 文本里找 `key=`，找到就替换该段，否则追加；缓冲不足时丢弃最旧项（简单截断，不报错）。

## 6. MCP 工具：4 → 3

| 工具 | 参数 | 返回 |
|---|---|---|
| `self.home.devices` | 无 | 全量设备 JSON：`id / name / online / caps[]（含描述）/ state / age_ms / info_seen` |
| `self.home.control` | `id`(int) / `cap`(str) / `action`(str) / `args`(str,可省) | 成功：`"已发送: <设备名>.<能力> <动作> <参数>"`；**失败时返回值里直接附上设备清单**（未知节点/未上报能力/离线三种情况都附），让 AI 一步自愈，不必先查后控 |
| `self.home.announce` | `name`(str) | 本地播报（自测/演示用，与节点无关） |

**工具描述必须写明**：控制前不需要先调 `self.home.devices`（失败会自动附清单）；调用一次即完成，**不要重复调用**（踩坑 15）。

`self.home.control` 的下行仍是**单次非阻塞发送**（节点常醒，连发会阻塞对话——首版规格修正 1）。

## 7. 板级简化（`compact_wifi_board_s3cam_airobot.cc`）

删除：`HomeNodeState`、`home_state_[]`、`home_hot_state_`、`kHomeHotTrigger/kHomeHotRelease`、`OnHomeEvent()` 里的四个事件分支。

保留并简化：

```cpp
void OnHomeEvent(int node_id, const std::string& kind, const std::string& name,
                 const std::string& arg, int64_t ts_ms) {
    (void)arg; (void)ts_ms;
    if (kind == "say") {
        Announce(node_id, name.c_str());   // 状态缓存已由传输层维护
    }
}
```

`Announce()`（待机才播 + 同节点 10 秒冷却）**逻辑不变**，只去掉对具体事件名的依赖。

## 8. 节点侧改造（`EspNowNode.ino`）

引入"能力表"作为唯一的设备描述来源：

```cpp
// 能力处理函数：把结果文本写进 out，返回是否成功
typedef bool (*CapHandler)(const char* action, const char* args, char* out, size_t out_len);

struct CapDef {
    const char* name;      // 能力名（AI 用它与 action 调用）
    const char* spec;      // 规格说明：描述 + 动作(参数)
    CapHandler handler;    // nullptr = 只读/只上报，不接受 do
};
struct NodeDef {
    const char* name;      // 设备名
    const CapDef* caps;
    int cap_count;
};
```

- 节点 1：`{"客厅灯", {{"light","(RGB灯):on(0|1),rgb(r,g,b),bright(0-255)",capLight},{"dist","(距离cm):read()",nullptr}}, 2}`
- 节点 2：`{"玄关感应", {{"temp","(温湿度):read()",nullptr},{"beam","(激光遮挡0|1):read()",nullptr}}, 2}`

行为变更：

| 项 | 首版 | 现在 |
|---|---|---|
| 上报能力 | 无 | 锁定信道后 + 每次收到 beacon → `sendInfo()` |
| 下行解析 | 硬编码 `light ` 前缀 | 通用 `do <cap> <action> [args]` → 查表 → handler |
| 回执 | `ack light 1` | `ok <cap> <结果文本>` |
| 高温判定 | 主控侧（28/26℃） | **节点 2 侧**，迟滞后发 `say hot`（去重后只发一次） |
| 事件上报 | `evt temp/dist/beam/motion` | 不变（但**只是状态**，播报走 `say`） |
| 人靠近 | 主控按 `evt motion` 播报 | 节点发 `say motion` |

**接入新设备的 5 步**（节点 README 要写）：填能力表 → 写 handler → 配 GPIO → 烧录 → 上电等 3 秒。**主控零改动**。

## 9. 错误处理

| 情况 | 处理 |
|---|---|
| 节点能力数超 `kMaxCaps` | 静默忽略多余项（不报错、不阻塞） |
| `info` 超过单包长度 | 节点侧截断描述；主控按已解析到的部分入库 |
| `do` 的能力/动作不存在 | 节点回 `err unknown-cap <cap>` / `err unknown-action <action>` → 进状态缓存 |
| 节点离线时 control | **不发包**，直接返回"<设备名> 离线（最后通信 Xs 前）"+ 可用设备清单 |
| 节点号超 `kMaxNodes` | 忽略（已有逻辑） |
| 注册表里存在但 `info_seen=false` | `devices` 里标出来，AI 可说"该设备还没上报能力" |
| 状态下发失败 | `SendTo()` 返回 false → 工具返回明确失败文本（不含"已发送"字样） |

## 10. 测试策略

主机测试（`scripts/tests/test_espnow_home_protocol.py` 扩展，仍为纯逻辑复刻 + 源码文本断言）：

- **解析**：`info`（名字 + 多能力拆分、含描述与括号）、`do`（cap/action/args 缺省）、`ok`、`say`、`evt`；
- **状态缓存**：`UpsertState` 的新增/覆盖/截断；
- **注册表**：容量上限、重复 `info` 覆盖、离线判定、`info_seen` 标记；
- **源码断言**：`self.home.*` 工具数 = 3；板级不再出现 `kHomeHotTrigger` 与 `evt == "`；节点固件存在能力表与 `sendInfo`；播报只走 `say`；`control` 失败分支含设备清单；
- 真机验证见板级 README（节点上线自动出现、语音控制、换节点不改主控）。

## 11. 明确不做（YAGNI）

- TF 卡持久化注册表、人工别名覆盖配置（与"自动发现"目标相反）；
- 动态 `AddTool` 注册、`RemoveTool`（服务端时机不可控，见决策 1）；
- 能力参数的类型校验（主控保持语义无关；错误由节点回 `err`）；
- 多主控/节点间互发现、固件 OTA、能力热更新。

## 12. 影响与兼容

- 板级 README 的「AI 语音指令」表要改（`self.home.light/sensor` → `devices/control`），并补"接入新设备 5 步"；
- 节点 README 增"能力表怎么填"；
- 首版设计文档的 §3.3/§3.5/§4.1 需标注"已被本文架构修正"（在首版文档加一行指向本文的说明即可）；
- 演示台词不受影响；`self.home.announce`、播报冷却、beacon、去重、连发策略全部沿用。
