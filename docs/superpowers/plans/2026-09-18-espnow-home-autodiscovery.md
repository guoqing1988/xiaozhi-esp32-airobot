# ESP-NOW 居家设备自动发现（数据驱动重构）实现计划

**目标：** 把"节点角色/能力/事件语义/播报映射"从主控固件硬编码改为节点自描述 + 运行时注册表，使接入新设备只需改节点固件。

**规格：** `docs/superpowers/specs/2026-09-18-espnow-home-autodiscovery-design.md`

**技术栈：** ESP-IDF v6.0.2（主控）、arduino-esp32 3.2.0（节点）、Python `unittest`。

---

## 施工纪律（上次事故的教训，必须遵守）

1. **绝不并行下发两个会修改文件的工具调用**——实测 `edit` 与 `bash sed -i` 并行会让文件被截断到 8KB。每轮只改一个文件，改完再动下一个。
2. **不跑无 TF 卡变体编译**：`build.py` 切变体会重建 `sdkconfig` 并全量重编 2154 个目标。只编主变体。
3. **不手改 `sdkconfig`**（生成物）。
4. 文档/代码写完**立即 commit**。
5. 主控编译命令（每次都要 source IDF）：
   ```sh
   source ~/esp/v6.0.2/esp-idf/export.sh
   python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
   ```
   注意：这会覆盖 `build/` 状态；若 `sdkconfig` 缺失（上次事故残留），它会自动重建。
6. 节点编译：
   ```sh
   arduino-cli compile --fqbn esp32:esp32:esp32s3 main/boards/bread-compact-wifi-s3cam-airobot/arduino/EspNowNode
   ```

## 文件清单

| 文件 | 动作 |
|---|---|
| `scripts/tests/test_espnow_home_protocol.py` | 改造：新增 info/do/ok/say 解析、状态缓存、注册表、源码断言 |
| `main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.h` | 改造：`DeviceEntry`/`CapEntry`、接口、回调加 `kind` |
| `main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.cc` | 改造：`info/do/ok/say/evt` 解析、`UpsertState`、`DevicesJson` |
| `.../compact_wifi_board_s3cam_airobot.cc` | 改造：删硬编码状态与事件分支，换 3 个工具 |
| `.../arduino/EspNowNode/EspNowNode.ino` | 改造：能力表 + `sendInfo` + `do` 分发 + `say` 通道 |
| `.../README.md` | 更新工具表 + 接入新设备 5 步 |
| `.../arduino/EspNowNode/README.md` | 更新协议 + 能力表填写说明 |
| `docs/superpowers/specs/2026-09-18-espnow-home-control-design.md` | 加一行指向本次架构修正 |

---

## 任务 1：测试基线（先写红）

**文件：** `scripts/tests/test_espnow_home_protocol.py`

- [ ] **步骤 1：扩展纯逻辑复刻**

新增（与被测 C++ 逻辑一一对应）：
- `parse_info(body)` → `(name, [(cap_name, spec), ...])`：按 `;` 拆能力，每项取 `(` 前的名字；
- `parse_do(body)` → `(cap, action, args)`：2~3 段，`args` 可缺省为空串；
- `parse_upstream(packet)` → `(node_id, kind, name, arg)`，`kind ∈ {info, do, ok, say, evt, err}`；
- `upsert_state(state: str, key: str, value: str) -> str`：存在则替换该 key 段，否则追加，超长截断；
- `DeviceTable`：容量 4、重复 `info` 覆盖、`info_seen`、离线判定。

- [ ] **步骤 2：新增源码约定断言**

| 断言 | 目的 |
|---|---|
| 板级 `self.home.` 工具恰好 3 个（`devices`/`control`/`announce`） | 防回退到硬编码工具 |
| 板级源码**不含** `kHomeHotTrigger`、`evt == "` | 业务语义不得留在主控 |
| `OnHomeEvent` 只判 `kind == "say"` | 播报通道正确 |
| `espnow_home.cc` 含 `info`/`do ` 解析与 `UpsertState` | 核心能力存在 |
| 节点固件含 `CapDef`/`sendInfo`/`"do "` 分发 | 节点自描述存在 |
| 节点固件不含 `ack light` | 旧协议已退役 |
| 两侧都无 `ESP_LOG` | 本板 UART0 共用（沿用既有约束） |

- [ ] **步骤 3：运行验证失败**

```sh
python3 -m unittest scripts.tests.test_espnow_home_protocol -v
```
预期：新增用例 FAIL（实现尚未改），旧用例仍 PASS。

- [ ] **步骤 4：Commit** — `test: 扩展居家节点协议测试(能力自描述/通用动作/播报通道)`

---

## 任务 2：传输层改造

**文件：** `espnow_home.h` → 然后 `espnow_home.cc`（**分两次改，不并行**）

- [ ] **步骤 1：`espnow_home.h`**

- 常量：`kMaxCaps=3`、`kNameLen=20`、`kCapNameLen=14`、`kCapSpecLen=48`、`kStateLen=96`；
- `struct CapEntry` / `struct DeviceEntry`（见规格 §5，替换原 `NodeEntry`，保留 `mac/last_seen_ms`）；
- 回调签名加 `kind`：`(int node_id, const std::string& kind, const std::string& name, const std::string& arg, int64_t ts_ms)`；
- 新接口：`DevicesJson() / HasNode(int) / NodeName(int) / HasCap(int, const std::string&)`；
- 私有：`DeviceEntry* FindDevice(int)`、`DeviceEntry* FindOrCreateDevice(int, const uint8_t*)`、`void UpsertState(DeviceEntry&, const char* key, const char* value)`、`void HandleInfo(DeviceEntry&, const std::string&)`。

- [ ] **步骤 2：`espnow_home.cc`**

- `HandleRecv` 解析分支：`info`（更新能力表，**不去重、不回调**）、`do`（仅下行使用，上行忽略）、`ok`（`UpsertState(cap, arg)`）、`evt`（去重 + `UpsertState(name, arg)` + 回调 `kind="evt"`）、`say`（去重 + 回调 `kind="say"`，**不写状态**）、`err`（`UpsertState("err", arg)`）；
- 去重 key 由 `evt` 名改为 `kind + " " + name`（避免 `say motion` 与 `evt motion` 互相吞）；
- `UpsertState`：固定缓冲内字符串替换/追加，零堆分配；
- `DevicesJson`：输出 `id/name/online/info_seen/caps[{name,spec}]/state/age_ms`；
- `NodeName`：未知时返回 `节点<id>`（用静态缓冲，调用方立即使用）。

- [ ] **步骤 3：主控编译验证**（见施工纪律 5）

预期：编译通过；记录固件体积与分区余量。

- [ ] **步骤 4：Commit** — `refactor: 传输层改为设备自描述注册表(info/do/ok/say)`

---

## 任务 3：板级集成改造

**文件：** `compact_wifi_board_s3cam_airobot.cc`（一次编辑，含多处 edit）

- [ ] **步骤 1：删除硬编码**
  - 成员区：删 `HomeNodeState`、`home_state_[]`、`home_hot_state_`、`kHomeHotTrigger`、`kHomeHotRelease`、`kHomeStaleMs`；
  - 保留 `home_announce_ms_[]` 与 `kAnnounceCooldownMs`（播报冷却仍按节点计）。

- [ ] **步骤 2：简化 `OnHomeEvent()`**

```cpp
void OnHomeEvent(int node_id, const std::string& kind, const std::string& name,
                 const std::string& arg, int64_t ts_ms) {
    (void)arg; (void)ts_ms;
    if (kind == "say") { Announce(node_id, name.c_str()); }
}   // 状态缓存由传输层维护
```

- [ ] **步骤 3：替换三个工具**
  - 删 `self.home.light` / `self.home.sensor` / `self.home.status`；
  - 加 `self.home.devices`（无参 → `DevicesJson()`，`espnow_home_` 为空时返回明确的"ESP-NOW 未启动"）；
  - 加 `self.home.control`（`id`/`cap`/`action`/`args` 四属性；校验 `HasNode`/`HasCap`/`IsOnline`，任一不满足则在返回文本里**附上 `DevicesJson()`**；成功则发 `do <cap> <action> [args]`，措辞禁止出现会被读成"没成功"的字样）；
  - 保留 `self.home.announce`。
  - 工具描述写明：调用一次即完成、**不要重复调用**（踩坑 15）。

- [ ] **步骤 4：主控编译验证** + 记录体积/余量。

- [ ] **步骤 5：Commit** — `refactor: 居家工具改为 devices/control 数据驱动, 播报走 say 通道`

---

## 任务 4：节点固件改造

**文件：** `.../arduino/EspNowNode/EspNowNode.ino`

- [ ] **步骤 1：引入能力表**

`CapDef{name, spec, handler}` + `NodeDef{name, caps, cap_count}`；节点 1 = 客厅灯(light, dist)，节点 2 = 玄关感应(temp, beam)。
`capLight(action, args, out, out_len)` 处理 `on/rgb/bright`（沿用现有 `applyLight()` 与状态变量）。

- [ ] **步骤 2：`sendInfo()`**

组装 `@n<id> info <名字> <cap><spec>;<cap><spec>`，走 `queueEvent()`（连发 3 次）；在**锁定信道成功后**以及**每次收到 beacon 时**调用。注意单包 200B 上限。

- [ ] **步骤 3：`do` 分发**

`onReceive` 里把 `light ` 硬编码分支换成通用解析：`do <cap> <action> [args]` → 查表 → `handler` → 成功 `@n<id> ok <cap> <结果>`；找不到 → `@n<id> err unknown-cap <cap>`；`handler==nullptr` → `err readonly <cap>`。

- [ ] **步骤 4：播报通道与阈值下移**

- `motion` 触发处：改为 `queueSay("motion")`（不再发 `evt motion`）；
- 激光遮挡：`queueSay("beam")`；
- **新增节点 2 的温度迟滞**（≥28 ℃ 发一次 `queueSay("hot")`，≤26 ℃ 复位）；
- `dist`/`temp` 仍走 `evt`（只更新状态，不播报）。

- [ ] **步骤 5：节点编译验证**（施工纪律 6）

预期：编译通过，记录 flash/RAM 占用。

- [ ] **步骤 6：Commit** — `refactor: 节点改为能力表自描述, 播报走 say 通道`

---

## 任务 5：文档与全量验证

- [ ] **步骤 1：板级 README**
  - 「AI 语音指令」表：`self.home.light/sensor` → `self.home.devices` / `self.home.control`；
  - 新增「接入一个新设备（5 步）」小节：填能力表 → 写 handler → 配 GPIO → 烧录 → 上电 3 秒自动出现；
  - 「设计取舍」补一条：为什么不做动态注册 MCP 工具（服务端拉取时机 + 设备无 `RemoveTool`）；
  - 排错表补：`info_seen=false`、`err unknown-cap`。

- [ ] **步骤 2：节点 README**
  - 协议表更新（`info`/`do`/`ok`/`say`/`evt`/`err`）；
  - 「能力表怎么填」：`spec` 语法、动作名建议用 `on/off/set/read`、单包 200B 内；
  - 加：**新节点无需改主控**。

- [ ] **步骤 3：首版设计文档加指引**

在 `2026-09-18-espnow-home-control-design.md` 状态行下方加一行：
> 架构已由 `2026-09-18-espnow-home-autodiscovery-design.md` 修正（节点自描述 + 通用工具），本文的 §3.3/§3.5/§4.1 仅作首版记录。

- [ ] **步骤 4：全量验证**

```sh
python3 -m unittest discover -s scripts/tests -v      # 全绿
```
主控主变体编译通过；节点固件编译通过。记录 `free sram` 对比（注册表约 1.5KB，若掉幅过大则把 `kMaxNodes` 降为 2）。

- [ ] **步骤 5：Commit** — `docs: 居家节点改为数据驱动, 补充接入新设备指引`

- [ ] **步骤 6：交付报告**（不 commit）：改动清单、测试/编译证据、**需真机验证的项**、SRAM 实测数据。

---

## 验收标准（用户视角）

1. 语音"打开客厅灯"仍可成功（AI 走 `devices` → `control`，或直接 `control` 失败后自愈）；
2. 新增一个节点（只改节点固件）→ 上电 3 秒后 AI 能说出它的名字和能力，**主控固件未改动**；
3. 主控源码里搜不到任何具体能力名/事件名（`light`/`temp`/`motion`/`hot`）与节点号语义；
4. 拔掉节点 → `devices` 显示离线，`control` 返回明确失败 + 可用清单，无卡顿。
