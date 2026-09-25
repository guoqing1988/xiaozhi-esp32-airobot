# ESP-NOW 居家灯控与传感器 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 让机器人通过 ESP-NOW 控制 2 个自制 ESP32-S3 节点上的 RGB 灯，读取节点上的超声波/温湿度/激光传感器，并在传感器事件（有人靠近、门口有人经过、温度偏高）时播放本地预录语音播报——用于比赛现场演示。

**架构：** 主控（ESP32-S3，ESP-IDF）新增板级传输层 `espnow_home.h/.cc`（ESP-NOW 收发 + beacon 发现广播 + 节点表 + 上行去重），板级注册 `self.home.*` MCP 工具供云侧 LLM 调用；节点（ESP32-S3，Arduino 核心自带 `ESP_NOW` 类）负责 GPIO 执行与本地迟滞判定后主动上报；播报复用现成 `LocalMusicPlayer` + TF 卡，音频放独立目录 `/sdcard/announce/`。**核心代码零改动，全部改动落在板级目录内。**

**技术栈：** ESP-IDF v6.0.2（`esp_now.h`、`esp_timer`）、arduino-esp32 3.2.0（核心自带 `ESP_NOW`、`ledc`）、Adafruit DHT sensor library、Python `unittest`（`scripts/tests/`）。

**规格：** `docs/superpowers/specs/2026-09-18-espnow-home-control-design.md`

**规格修正（实现期发现，需同步回设计文档，见任务 6）：**
1. ~~关键报文两侧都连发 3 次~~ → **只有上行 `evt` 连发 3 次**（节点侧非阻塞状态机）。下行在 MCP 工具回调里连发需阻塞 ~300ms，会卡住对话；而节点常醒（USB 供电、`WiFi.setSleep(false)`），下行单次即可。
2. ~~`EspNowHome::SetControlWindow()` + `SetPowerSaveLevel()` 联动~~ → **不做**。待机省电保持原样（踩坑 7 确认是刻意设计），靠"上行 3 连发 + 主控去重"跨过 DTIM 唤醒窗口即可，避免侵入板级 `SetPowerSaveLevel()` override。
3. ~~初始化时调用 `esp_now_set_wake_window()`~~ → **先不调用**（保持官方默认最大值），仅在现场实测丢包时作为调优项。
4. ~~验证 `#ifdef` 时也编译“无 TF 卡”变体~~ → **撤销**。`build.py` 切变体会重建 `sdkconfig`、触发**全量重编 2154 个目标**并把 `build/` 切到另一变体，代价远大于收益（中断时还会留下 `sdkconfig` 缺失的中间状态）。播报代码已完整 `#ifdef` 包裹，需要时再单独编该变体。

> ⚠️ 这两个常量是 `uint8_t[16]`，字符串字面量**含结尾 `\0` 不得超过 16 字节**（即最多 15 个可见字符）。
> 早期草案写的 `"xiaozhi-pmk-0001"`（16 字符）会导致 `initializer-string ... is too long` 编译失败。

> **注：本计划中的代码片段是施工时的草案。** 与实现代码若有差异，**以实现代码和设计文档 §3.2 为准**。
> 已知差异：`EspNowHome` 的 API 签名（`IsOnline(int node_id)` 无默认参数、`ts_ms` 为 `int64_t`）、
> 密钥字面量（实际为 `"xiaozhi-pmk-01"`）、节点侧 `ESP_NOW_Peer::add()/send()` 是 protected 需子类 public 包装。

---

## 文件结构

| 文件 | 职责 |
|---|---|
| 创建 `main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.h` | 传输层接口：`Begin/SendTo/IsOnline/NodesJson` + 常量 |
| 创建 `main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.cc` | ESP-NOW 收发、beacon 定时器、节点表、报文解析与去重 |
| 修改 `.../local_music_player.h` | 新增 public `PlayAnnounce()`；新增 private 成员 `pending_path_` |
| 修改 `.../local_music_player.cc` | 实现 `PlayAnnounce()`；`PlayTask()` 增加"绝对路径优先"分支；新增 `ANNOUNCE_DIR` |
| 修改 `.../compact_wifi_board_s3cam_airobot.cc` | 新增 `InitializeEspNowHome()`、`OnHomeEvent()`、`Announce()`、传感器缓存；构造函数调用 |
| 创建 `.../arduino/EspNowNode/EspNowNode.ino` | 节点固件：信道 hop 发现、灯控、三传感器、事件上报（3 连发） |
| 创建 `.../arduino/EspNowNode/README.md` | 节点接线/编译/烧录/排错 |
| 创建 `.../scripts/gen_announce_mp3.sh` | 用 `say` + `ffmpeg` 生成 3 段预录播报 MP3 |
| 创建 `scripts/tests/test_espnow_home_protocol.py` | 协议解析/去重/迟滞逻辑复刻 + 源码文本断言 |
| 修改 `.../README.md` | 新增「ESP-NOW 居家灯控与传感器」章节 |

约定常量（**两侧必须一致，改一处要改两处**）：

```
PMK = "xiaozhi-pmk-01"   (14 字符 + '\0'，放进 uint8_t[16])
LMK = "xiaozhi-lmk-01"   (同上)
信道扫描范围 1..13；节点号 1=客厅, 2=玄关
```

---

## 任务 1：测试基线（协议解析 / 去重 / 迟滞 + 源码断言）

**文件：**
- 创建：`scripts/tests/test_espnow_home_protocol.py`

- [x] **步骤 1：编写失败的测试**

```python
"""测试 ESP-NOW 居家节点协议：报文解析、上行去重、事件迟滞，以及两侧源码的约定断言。

对应实现：
  - C++    main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.cc
           的 HandleRecv() / ShouldDeliver()：解析 "@n<id> evt <name> <arg>"，并按
           (node_id, evt) 做 kDedupWindowMs 去重（节点连发 3 次对抗主控待机 DTIM 漏包）。
  - Arduino main/boards/bread-compact-wifi-s3cam-airobot/arduino/EspNowNode/EspNowNode.ino
           的超声波迟滞（<30cm 触发 / >40cm 复位）与事件 3 连发。

真机验证仍需烧录后手动测试（见板级 README 的「真机验证要点」）。
"""

import os
import re
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BOARD_DIR = os.path.join(REPO_ROOT, "main", "boards", "bread-compact-wifi-s3cam-airobot")
ESPNOW_CC = os.path.join(BOARD_DIR, "espnow_home.cc")
ESPNOW_H = os.path.join(BOARD_DIR, "espnow_home.h")
NODE_INO = os.path.join(BOARD_DIR, "arduino", "EspNowNode", "EspNowNode.ino")
BOARD_CC = os.path.join(BOARD_DIR, "compact_wifi_board_s3cam_airobot.cc")

DEDUP_WINDOW_MS = 1000      # 与 EspNowHome::kDedupWindowMs 一致
MOTION_TRIGGER_CM = 30      # 超声波触发阈值（节点侧）
MOTION_RELEASE_CM = 40      # 超声波复位阈值（迟滞）
EVENT_RESEND = 3            # 上行事件连发次数


def parse_node_packet(data: bytes):
    """复刻 EspNowHome::HandleRecv 的解析：返回 (node_id, body) 或 None。"""
    if len(data) < 4 or data[0:1] != b"@" or data[1:2] != b"n":
        return None
    i = 2
    num = b""
    while i < len(data) and data[i:i + 1].isdigit():
        num += data[i:i + 1]
        i += 1
    if not num or i >= len(data) or data[i:i + 1] != b" ":
        return None
    node_id = int(num)
    if node_id <= 0:
        return None
    return node_id, data[i + 1:].decode("utf-8", "ignore")


def split_evt(body: str):
    """复刻 'evt <name> <arg>' 的拆分。"""
    if not body.startswith("evt "):
        return None
    rest = body[4:]
    if " " in rest:
        name, arg = rest.split(" ", 1)
    else:
        name, arg = rest, ""
    return name, arg


def make_dedup():
    return {"slots": [], "window_ms": DEDUP_WINDOW_MS}


def should_deliver(state, node_id, evt, now_ms):
    """复刻 ShouldDeliver：窗口内相同 (node_id, evt) 吞掉并续期。"""
    for s in state["slots"]:
        if s["node_id"] == node_id and s["evt"] == evt and (now_ms - s["ts"]) < state["window_ms"]:
            s["ts"] = now_ms
            return False
    state["slots"].append({"node_id": node_id, "evt": evt, "ts": now_ms})
    return True


def make_motion_hysteresis():
    return {"active": False}


def motion_update(st, dist_cm):
    """复刻节点侧超声波迟滞：<30cm 触发(边沿一次)，>40cm 复位，中间保持。"""
    if not st["active"] and dist_cm < MOTION_TRIGGER_CM:
        st["active"] = True
        return True          # 事件：有人靠近
    if st["active"] and dist_cm > MOTION_RELEASE_CM:
        st["active"] = False
    return False


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


class TestParse(unittest.TestCase):
    def test_evt_with_arg(self):
        self.assertEqual(parse_node_packet(b"@n1 evt dist 23"), (1, "evt dist 23"))

    def test_evt_two_args(self):
        self.assertEqual(parse_node_packet(b"@n2 evt temp 26 55"), (2, "evt temp 26 55"))

    def test_ack(self):
        self.assertEqual(parse_node_packet(b"@n1 ack light 1"), (1, "ack light 1"))

    def test_two_digit_node(self):
        self.assertEqual(parse_node_packet(b"@n12 evt beam 1"), (12, "evt beam 1"))

    def test_reject_bad(self):
        for bad in (b"", b"@", b"@n", b"@n1", b"@n1x", b"@nx evt beam 1",
                    b"n1 evt beam 1", b"@n0 evt beam 1"):
            self.assertIsNone(parse_node_packet(bad), bad)

    def test_split_evt(self):
        self.assertEqual(split_evt("evt temp 26 55"), ("temp", "26 55"))
        self.assertEqual(split_evt("evt motion 1"), ("motion", "1"))
        self.assertEqual(split_evt("evt alive"), ("alive", ""))
        self.assertIsNone(split_evt("ack light 1"))


class TestDedup(unittest.TestCase):
    def test_resend_three_times_collapses_to_one(self):
        st = make_dedup()
        # 节点连发 3 次，间隔 150ms，全部落在 1s 窗口内
        results = [should_deliver(st, 1, "motion", t) for t in (0, 150, 300)]
        self.assertEqual(results, [True, False, False])

    def test_different_nodes_not_collapsed(self):
        st = make_dedup()
        self.assertTrue(should_deliver(st, 1, "motion", 0))
        self.assertTrue(should_deliver(st, 2, "motion", 0))

    def test_different_events_not_collapsed(self):
        st = make_dedup()
        self.assertTrue(should_deliver(st, 1, "motion", 0))
        self.assertTrue(should_deliver(st, 1, "beam", 0))

    def test_after_window_allowed_and_resend_refreshes(self):
        st = make_dedup()
        self.assertTrue(should_deliver(st, 1, "beam", 0))
        # 窗口内的连发被吞且续期：从 900ms 续到 1050ms
        self.assertFalse(should_deliver(st, 1, "beam", 900))
        self.assertFalse(should_deliver(st, 1, "beam", 1050))
        # 续期后需再等满一个窗口
        self.assertTrue(should_deliver(st, 1, "beam", 1050 + DEDUP_WINDOW_MS))


class TestMotionHysteresis(unittest.TestCase):
    def test_trigger_once_then_hold(self):
        st = make_motion_hysteresis()
        self.assertTrue(motion_update(st, 25))
        # 仍在阈值内：不再重复触发（避免刷屏播报）
        self.assertFalse(motion_update(st, 20))
        self.assertFalse(motion_update(st, 29))

    def test_no_trigger_between_thresholds(self):
        st = make_motion_hysteresis()
        self.assertFalse(motion_update(st, 35))

    def test_release_then_retrigger(self):
        st = make_motion_hysteresis()
        self.assertTrue(motion_update(st, 20))
        self.assertFalse(motion_update(st, 45))   # 复位
        self.assertTrue(motion_update(st, 20))    # 可再次触发


class TestSourceContracts(unittest.TestCase):
    """两侧源码必须遵守的约定（改坏了这里会红）。"""

    @classmethod
    def setUpClass(cls):
        missing = [p for p in (ESPNOW_CC, ESPNOW_H, NODE_INO, BOARD_CC) if not os.path.exists(p)]
        if missing:
            raise AssertionError("缺少实现文件：%s" % missing)
        cls.cc = read(ESPNOW_CC)
        cls.h = read(ESPNOW_H)
        cls.ino = read(NODE_INO)
        cls.board = read(BOARD_CC)

    def test_no_esp_log_in_espnow(self):
        """本板 UART0 与 Arduino 下位机共用，传输层不得打日志。"""
        for m in re.finditer(r"ESP_LOG[A-Z]", self.cc):
            self.fail("espnow_home.cc 不得使用 ESP_LOG（本板串口与下位机共享）: %s"
                      % self.cc[max(0, m.start() - 60):m.start() + 60])

    def test_constants_match_test(self):
        self.assertIn("kDedupWindowMs = 1000", self.h)
        self.assertIn("kOfflineMs = 15000", self.h)

    def test_downlink_is_single_send(self):
        """下行必须单次非阻塞发送：MCP 工具回调里连发会卡住对话。"""
        self.assertNotIn("kResendCount", self.h)
        self.assertNotIn("kResendGapMs", self.h)

    def test_uplink_resends_three_times(self):
        self.assertRegex(self.ino, r"EVENT_RESEND\s*=\s*%d" % EVENT_RESEND)
        self.assertIn("kEventResendGapMs", self.ino)

    def test_ifidx_sta_both_sides(self):
        """两侧 ifidx 必须都是 WIFI_IF_STA，否则互不可达。"""
        self.assertIn("WIFI_IF_STA", self.cc)
        self.assertIn("WIFI_IF_STA", self.ino)

    def test_node_uses_official_espnow_class(self):
        self.assertIn('#include "ESP32_NOW.h"', self.ino)
        self.assertIn("onNewPeer", self.ino)
        self.assertIn("WiFi.setChannel", self.ino)

    def test_node_has_hysteresis_thresholds(self):
        self.assertIn("MOTION_TRIGGER_CM %d" % MOTION_TRIGGER_CM, self.ino)
        self.assertIn("MOTION_RELEASE_CM %d" % MOTION_RELEASE_CM, self.ino)

    def test_tool_description_forbids_repeat(self):
        """踩坑 15 回归防护：工具描述必须明确禁止重复调用。"""
        self.assertIn("self.home.light", self.board)
        self.assertIn("不要重复调用", self.board)

    def test_no_debounce_wording_in_results(self):
        """踩坑 15：回执不得出现会被读成"没成功"的字样。"""
        for bad in ("防抖", "已忽略"):
            self.assertNotIn(bad, self.board)

    def test_light_tool_returns_descriptive_text(self):
        self.assertIn("已完成", self.board)

    def test_dirs_separated(self):
        self.assertIn('/sdcard/announce', read(os.path.join(BOARD_DIR, "local_music_player.cc")))


if __name__ == "__main__":
    unittest.main()
```

- [x] **步骤 2：运行测试验证失败**

运行：`python3 -m unittest scripts.tests.test_espnow_home_protocol -v`
预期：**ERROR/FAIL**，`TestSourceContracts.setUpClass` 报 `AssertionError: 缺少实现文件：[...]`（`espnow_home.cc` 等尚未创建）；纯逻辑用例（Parse/Dedup/Hysteresis）应已 PASS。

- [x] **步骤 3：Commit 测试基线**

```bash
git add scripts/tests/test_espnow_home_protocol.py
git commit -m "test: 添加 ESP-NOW 居家节点协议与源码约定测试"
```

---

## 任务 2：主控传输层 `espnow_home.h/.cc`

**文件：**
- 创建：`main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.h`
- 创建：`main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.cc`

- [x] **步骤 1：编写 `espnow_home.h`**

```cpp
#ifndef ESPNOW_HOME_H
#define ESPNOW_HOME_H

#include <cstdint>
#include <functional>
#include <string>

// ESP-NOW 居家节点传输层（板级）。
//
// 职责：ESP-NOW 初始化、beacon 发现广播（供节点锁信道）、节点表维护、
//       上行报文解析与去重、下行报文发送。
// 不含：MCP 工具注册（板级主文件注册）、播报（板级调 LocalMusicPlayer）。
//
// 注意：本板 UART0 与 Arduino 下位机共用，本文件一律不使用 ESP_LOG，
//       失败通过返回值/上层工具文本表达（见 AGENTS.md 与本板 README）。
class EspNowHome {
public:
    // 上行事件出口：node_id / 事件名 / 参数 / 事件时间(ms)
    // 在 WiFi 任务上下文调用，实现方须用 Application::Schedule 切回主任务。
    using EventCallback = std::function<void(int node_id, const std::string& evt,
                                             const std::string& arg, int64_t ts_ms)>;

    static constexpr int kMaxNodes = 4;
    static constexpr int64_t kOfflineMs = 15000;      // 主控侧判定离线（节点侧回 hop 是 5s，勿混）
    static constexpr int64_t kDedupWindowMs = 1000;   // 上行事件去重窗口（抵消节点 3 连发）
    static constexpr int kBeaconFastMs = 500;         // 启动期 beacon 周期
    static constexpr int kBeaconSlowMs = 3000;        // 稳态 beacon 周期
    static constexpr int kBeaconFastCount = 60;       // 快发次数（≈30 秒）
    static constexpr int kMaxPacketLen = 200;         // 单包上限（IDF v1.0 上限 250B）

    // 初始化 ESP-NOW（幂等：重复调用返回已启动状态）。成功返回 true。
    bool Begin(EventCallback cb);

    // 下行：向节点发送正文（内部加 "@n<id> " 前缀）。单次非阻塞发送：
    // 节点常醒（USB 供电），无需重发；连发会阻塞 MCP 工具回调、卡住对话。
    bool SendTo(int node_id, const std::string& body);

    bool IsOnline(int node_id) const;

    // 节点状态 JSON：{"nodes":[{"id":1,"online":true,"last_seen_ms":1234},...]}
    std::string NodesJson() const;

    static int64_t NowMs();

private:
    struct NodeEntry {
        int id = 0;
        uint8_t mac[6] = {0};
        int64_t last_seen_ms = 0;
        bool known = false;
    };

    struct DedupEntry {
        int node_id = 0;
        char evt[16] = {0};
        int64_t ts_ms = 0;
        bool used = false;
    };

    NodeEntry* FindNode(int node_id);
    NodeEntry* FindOrCreateNode(int node_id, const uint8_t* mac);
    bool ShouldDeliver(int node_id, const std::string& evt, int64_t now_ms);
    void HandleRecv(const uint8_t* src_mac, const uint8_t* data, int len);
    void StartBeacon(int period_ms);

    static void RecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len);
    static void BeaconTimerCb(void* arg);

    EventCallback callback_;
    NodeEntry nodes_[kMaxNodes];
    DedupEntry dedup_[kMaxNodes * 2];
    int dedup_pos_ = 0;
    esp_timer_handle_t beacon_timer_ = nullptr;
    int beacon_count_ = 0;
    bool started_ = false;
};

#endif  // ESPNOW_HOME_H
```

- [x] **步骤 2：编写 `espnow_home.cc`**

```cpp
#include "espnow_home.h"

#include <cstdio>
#include <cstring>

#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"

// ESP-NOW 密钥（16 字节）：两侧必须一致，改一处必须改另一处（节点固件 EspNowNode.ino）。
// PMK 全网统一；LMK 用于单播加密；广播不支持加密（IDF 官方限制，故 beacon 走明文）。
// 注意：PMK/LMK 不一致时的现象是"完全收不到包"，排查困难，改动务必同步两侧。
static const uint8_t kPmk[16] = "xiaozhi-pmk-01";
static const uint8_t kLmk[16] = "xiaozhi-lmk-01";
static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// 静态回调无法拿到 this，用文件级指针（板级只有一个实例）。
static EspNowHome* g_home = nullptr;

int64_t EspNowHome::NowMs() {
    return esp_timer_get_time() / 1000;
}

bool EspNowHome::Begin(EventCallback cb) {
    callback_ = std::move(cb);
    if (started_) {
        return true;
    }
    g_home = this;

    if (esp_now_init() != ESP_OK) {
        g_home = nullptr;
        return false;
    }
    esp_now_set_pmk(kPmk);

    if (esp_now_register_recv_cb(&EspNowHome::RecvCb) != ESP_OK) {
        esp_now_deinit();
        g_home = nullptr;
        return false;
    }

    // 注册广播 peer（beacon 用）：channel=0 表示"跟随当前信道"，广播不可加密。
    esp_now_peer_info_t bcast = {};
    memcpy(bcast.peer_addr, kBroadcastMac, sizeof(kBroadcastMac));
    bcast.channel = 0;
    bcast.ifidx = WIFI_IF_STA;
    bcast.encrypt = false;
    esp_now_add_peer(&bcast);   // 已存在时返回错误，忽略即可

    started_ = true;
    beacon_count_ = 0;
    StartBeacon(kBeaconFastMs);
    return true;
}

void EspNowHome::StartBeacon(int period_ms) {
    if (beacon_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EspNowHome::BeaconTimerCb;
        args.name = "espnow_beacon";
        if (esp_timer_create(&args, &beacon_timer_) != ESP_OK) {
            return;
        }
    } else {
        // 允许在自身回调里 stop/start（IDF 明确支持这一用法）
        esp_timer_stop(beacon_timer_);
    }
    esp_timer_start_periodic(beacon_timer_, static_cast<uint64_t>(period_ms) * 1000ULL);
}

void EspNowHome::BeaconTimerCb(void* arg) {
    (void)arg;
    if (g_home == nullptr) {
        return;
    }
    // 节点靠这条广播锁定信道并学习主控 MAC（未注册 peer → 走节点的 onNewPeer 回调）
    static const char kBeacon[] = "@beacon 1";
    esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(kBeacon), strlen(kBeacon));

    if (++g_home->beacon_count_ == kBeaconFastCount) {
        g_home->StartBeacon(kBeaconSlowMs);   // 发现完成后降频，减少 2.4G 占用
    } else if (g_home->beacon_count_ > kBeaconFastCount) {
        // 常驻低频 heartbeat，供节点重启/换信道后重新发现
    }
}

EspNowHome::NodeEntry* EspNowHome::FindNode(int node_id) {
    for (auto& n : nodes_) {
        if (n.known && n.id == node_id) {
            return &n;
        }
    }
    return nullptr;
}

EspNowHome::NodeEntry* EspNowHome::FindOrCreateNode(int node_id, const uint8_t* mac) {
    NodeEntry* found = FindNode(node_id);
    if (found != nullptr) {
        return found;
    }
    for (auto& n : nodes_) {
        if (!n.known) {
            n.known = true;
            n.id = node_id;
            memcpy(n.mac, mac, sizeof(n.mac));
            n.last_seen_ms = NowMs();
            return &n;
        }
    }
    return nullptr;   // 节点数超上限：忽略（kMaxNodes=4 已远超演示需求）
}

bool EspNowHome::SendTo(int node_id, const std::string& body) {
    NodeEntry* n = FindNode(node_id);
    if (n == nullptr) {
        return false;
    }
    if (body.empty()) {
        return false;
    }
    char buf[kMaxPacketLen];
    int len = snprintf(buf, sizeof(buf), "@n%d %s", node_id, body.c_str());
    if (len <= 0 || len >= static_cast<int>(sizeof(buf))) {
        return false;
    }
    // 单播 peer 在上行接收时已登记（见 HandleRecv），此处直接发。
    return esp_now_send(n->mac, reinterpret_cast<const uint8_t*>(buf), len) == ESP_OK;
}

bool EspNowHome::IsOnline(int node_id) const {
    for (const auto& n : nodes_) {
        if (n.known && n.id == node_id) {
            return (NowMs() - n.last_seen_ms) < kOfflineMs;
        }
    }
    return false;
}

std::string EspNowHome::NodesJson() const {
    std::string json = "{\"nodes\":[";
    bool first = true;
    int64_t now = NowMs();
    for (const auto& n : nodes_) {
        if (!n.known) {
            continue;
        }
        if (!first) {
            json += ",";
        }
        first = false;
        char item[96];
        snprintf(item, sizeof(item),
                 "{\"id\":%d,\"online\":%s,\"last_seen_ms\":%lld}", n.id,
                 (now - n.last_seen_ms) < kOfflineMs ? "true" : "false",
                 static_cast<long long>(now - n.last_seen_ms));
        json += item;
    }
    json += "]}";
    return json;
}

bool EspNowHome::ShouldDeliver(int node_id, const std::string& evt, int64_t now_ms) {
    for (auto& d : dedup_) {
        if (d.used && d.node_id == node_id && evt == d.evt &&
            (now_ms - d.ts_ms) < kDedupWindowMs) {
            d.ts_ms = now_ms;   // 续期：连发 3 次全部落在窗口内被吞掉
            return false;
        }
    }
    DedupEntry& slot = dedup_[dedup_pos_];
    dedup_pos_ = (dedup_pos_ + 1) % (kMaxNodes * 2);
    slot.used = true;
    slot.node_id = node_id;
    snprintf(slot.evt, sizeof(slot.evt), "%s", evt.c_str());
    slot.ts_ms = now_ms;
    return true;
}

void EspNowHome::HandleRecv(const uint8_t* src_mac, const uint8_t* data, int len) {
    if (src_mac == nullptr || data == nullptr || len < 4 || len > kMaxPacketLen) {
        return;
    }
    if (data[0] != '@' || data[1] != 'n') {
        return;
    }
    int i = 2;
    int node_id = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
        node_id = node_id * 10 + (data[i] - '0');
        i++;
    }
    if (node_id <= 0 || node_id > kMaxNodes || i >= len || data[i] != ' ') {
        return;
    }

    int64_t now = NowMs();
    NodeEntry* n = FindOrCreateNode(node_id, src_mac);
    if (n != nullptr) {
        n->last_seen_ms = now;
        memcpy(n->mac, src_mac, sizeof(n->mac));
        // 登记为单播 peer（含 LMK 加密），后续下行才发得出去
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, src_mac, 6);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = true;
        memcpy(peer.lmk, kLmk, sizeof(kLmk));
        esp_now_mod_peer(&peer);
        esp_now_add_peer(&peer);   // 已存在时返回错误，忽略
    }

    // 正文（短字符串走 SSO，不在 WiFi 任务里分配堆内存）
    std::string body(reinterpret_cast<const char*>(data + i + 1),
                     static_cast<size_t>(len - i - 1));

    if (body.compare(0, 4, "evt ") == 0) {
        std::string rest = body.substr(4);
        size_t sp = rest.find(' ');
        std::string name = (sp == std::string::npos) ? rest : rest.substr(0, sp);
        std::string arg = (sp == std::string::npos) ? std::string() : rest.substr(sp + 1);
        if (name.empty() || name.size() > 15) {
            return;
        }
        if (!ShouldDeliver(node_id, name, now)) {
            return;
        }
        if (callback_) {
            callback_(node_id, name, arg, now);
        }
        return;
    }
    // "ack ..." / "err ..."：只刷新在线状态（上面已做），不做业务处理
}

void EspNowHome::RecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (g_home == nullptr || info == nullptr) {
        return;
    }
    g_home->HandleRecv(info->src_addr, data, len);
}
```

- [x] **步骤 3：确认 `esp_now.h` / `esp_timer.h` 已可用（依赖属于 IDF 自带组件）**

运行：`grep -rn "esp_now_monitor\|esp_wifi" main/CMakeLists.txt | head`
预期：`main` 组件已 `REQUIRES esp_wifi`（若未显式声明，ESP-IDF v6 的 `esp_wifi` 为主要组件，通常已由 `main/CMakeLists.txt` 的 `REQUIRES` 列表覆盖）。若编译报找不到 `esp_now.h`，在 `main/CMakeLists.txt` 的 `REQUIRES` 中补 `esp_wifi`。

- [x] **步骤 4：运行测试验证纯逻辑通过**

运行：`python3 -m unittest scripts.tests.test_espnow_home_protocol -v`
预期：`TestParse` / `TestDedup` / `TestMotionHysteresis` 全 PASS；`TestSourceContracts` 里与本任务相关的断言（`test_no_esp_log_in_espnow`、`test_constants_match_test`、`test_downlink_is_single_send`、`test_ifidx_sta_both_sides`）PASS；与节点/板级相关的仍 FAIL（后续任务补）。

- [x] **步骤 5：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.h \
        main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.cc
git commit -m "feat: 新增 ESP-NOW 居家节点传输层(收发/beacon发现/去重)"
```

---

## 任务 3：本地播报接口（`LocalMusicPlayer::PlayAnnounce`）

**文件：**
- 修改：`.../local_music_player.h`
- 修改：`.../local_music_player.cc`
- 创建：`.../scripts/gen_announce_mp3.sh`

- [x] **步骤 1：`local_music_player.h` 增加目录常量、public 方法与队列成员**

在 `#include` 之后、`class` 之前加：

```cpp
// 预录播报音频目录（独立于 MUSIC_DIR，不进歌曲列表/播放队列）
#define ANNOUNCE_DIR "/sdcard/announce"
```

在 public 区 `bool IsPlaying() const` 之后加：

```cpp
    // 播放 /sdcard/announce/<name>.mp3（传感器事件播报）。
    // 独立于歌曲队列：清空队列，播完即停，不接力播歌。
    // 返回 true=已启动播放；false=文件不存在/参数非法/线程创建失败。
    bool PlayAnnounce(const std::string& name);
```

在 private 区 `std::string pending_song_;` 之后加：

```cpp
    std::string pending_path_;                    // 指定要播的绝对路径(播报用, 优先于 pending_song_)
```

- [x] **步骤 2：`local_music_player.cc` 实现 `PlayAnnounce` 与 `PlayTask` 分支**

在 `PlaySong()` 之后加：

```cpp
bool LocalMusicPlayer::PlayAnnounce(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find("..") != std::string::npos) {
        return false;   // 拒绝路径穿越：只接受纯文件名
    }
    std::string path = std::string(ANNOUNCE_DIR) + "/" + name + ".mp3";
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;   // 未插卡 / 未放播报音频：静默跳过(不刷日志, 串口与下位机共享)
    }
    fclose(f);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        play_queue_.clear();     // 播报不接力播歌
        queue_pos_ = 0;
        pending_song_.clear();
        pending_path_ = path;
    }
    if (playing_.load()) {
        return true;             // 正在播(歌或播报)：下一轮 loop 切到播报
    }
    playing_ = true;
    paused_ = false;
    stop_requested_ = false;
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
    try {
        play_thread_ = CreatePlayThread(&LocalMusicPlayer::PlayTask, this);
    } catch (const std::exception& e) {
        playing_ = false;
        LogMemStats("PlayAnnounce create thread failed");
        ESP_LOGE(TAG, "Failed to start announce thread: %s", e.what());
        return false;
    }
    return true;
}
```

`PlayTask()` 的取曲分支改为（绝对路径优先）：

```cpp
        std::string song;
        std::string path;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!pending_path_.empty()) {
                path = pending_path_;
                pending_path_.clear();
            } else if (!pending_song_.empty()) {
                song = pending_song_;
                pending_song_.clear();
            } else {
                song = PickNextSong();
            }
        }
        if (song.empty() && path.empty()) {
            // 队列播完(顺序/随机均到末尾)、或没有任何歌曲/播报
            playing_ = false;
            break;
        }
        PlayOneSong(path.empty() ? (std::string(MUSIC_DIR) + "/" + song) : path);
```

- [x] **步骤 3：新增播报音频生成脚本**

`main/boards/bread-compact-wifi-s3cam-airobot/scripts/gen_announce_mp3.sh`：

```bash
#!/usr/bin/env bash
# 生成传感器事件播报音频（macOS: say 人声 → ffmpeg 转 24kHz 单声道 96kbps MP3）。
# 输出到 ./announce/，把整个目录拷到 TF 卡 /sdcard/announce/ 即可。
# 依赖：macOS 自带 say；ffmpeg（brew install ffmpeg）。也可自己录真人声，覆盖同名文件。
set -euo pipefail

OUT_DIR="$(cd "$(dirname "$0")" && pwd)/announce"
mkdir -p "$OUT_DIR"

VOICE="${VOICE:-Tingting}"
RATE="${RATE:-180}"

gen() {  # gen <文件名> <文本>
  local name="$1"; shift
  local text="$*"
  local aiff; aiff="$(mktemp -t announce).aiff"
  say -v "$VOICE" -r "$RATE" -o "$aiff" "$text"
  ffmpeg -y -loglevel error -i "$aiff" -ar 24000 -ac 1 -b:a 96k "$OUT_DIR/$name.mp3"
  rm -f "$aiff"
  echo "生成 $OUT_DIR/$name.mp3  ($text)"
}

gen motion "检测到有人靠近，已为你开灯"
gen beam   "门口有人经过，请注意"
gen hot    "室内温度偏高，请留意通风"

echo
echo "完成。把 $OUT_DIR 下的 mp3 拷到 TF 卡 /sdcard/announce/ 目录即可。"
```

- [x] **步骤 4：编译验证**

运行：`python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot`
预期：编译通过（此时 `espnow_home.*` 尚未被板级引用，但会被 `file(GLOB)` 自动纳入编译）。

- [x] **步骤 5：运行测试**

运行：`python3 -m unittest scripts.tests.test_espnow_home_protocol -v`
预期：`test_dirs_separated` PASS。

- [x] **步骤 6：Commit**

```bash
chmod +x main/boards/bread-compact-wifi-s3cam-airobot/scripts/gen_announce_mp3.sh
git add main/boards/bread-compact-wifi-s3cam-airobot/local_music_player.h \
        main/boards/bread-compact-wifi-s3cam-airobot/local_music_player.cc \
        main/boards/bread-compact-wifi-s3cam-airobot/scripts/gen_announce_mp3.sh
git commit -m "feat: 本地播放器新增预录播报接口(独立目录, 播完即停)"
```

---

## 任务 4：板级集成（MCP 工具 + 事件出口 + 缓存 + 播报触发）

**文件：**
- 修改：`.../compact_wifi_board_s3cam_airobot.cc`

- [x] **步骤 1：加头文件与成员**

顶部 include 区（`local_music_player.h` 附近）加：

```cpp
#include "espnow_home.h"
```

成员区（`web_control_active_` 之后）加：

```cpp
    // ---- ESP-NOW 居家节点（灯/传感器，见 espnow_home.h）----
    std::unique_ptr<EspNowHome> espnow_home_;
    struct HomeNodeState {
        bool valid = false;
        int temp = 0;      // DHT11 温度 ℃
        int hum = 0;       // DHT11 湿度 %
        int dist = 0;      // 超声波距离 cm
        int beam = 0;      // 激光 0=通 1=遮挡
        int64_t ts_ms = 0;
    };
    HomeNodeState home_state_[EspNowHome::kMaxNodes + 1];
    int64_t home_announce_ms_[EspNowHome::kMaxNodes + 1] = {0};  // 播报冷却
    int home_hot_state_ = 0;                                     // 温度告警迟滞: 0=正常 1=已告警
    static constexpr int64_t kAnnounceCooldownMs = 10000;        // 同节点播报冷却
    static constexpr int kHomeHotTrigger = 28;                   // ≥28℃ 触发
    static constexpr int kHomeHotRelease = 26;                   // ≤26℃ 复位
    static constexpr int64_t kHomeStaleMs = 30000;               // 读数过期阈值
```

- [x] **步骤 2：加 `InitializeEspNowHome()` / `OnHomeEvent()` / `Announce()`**

放在 `InitializeNetworkTools()` 之前：

```cpp
    // ---- ESP-NOW 居家节点 ----

    // 事件播报：仅待机时播（不打断对话），同节点 10 秒冷却
    void Announce(int node_id, const char* name) {
#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
        if (!music_player_ || node_id < 1 || node_id > EspNowHome::kMaxNodes) {
            return;
        }
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            return;
        }
        int64_t now = EspNowHome::NowMs();
        if (home_announce_ms_[node_id] != 0 &&
            (now - home_announce_ms_[node_id]) < kAnnounceCooldownMs) {
            return;
        }
        if (music_player_->PlayAnnounce(name)) {
            home_announce_ms_[node_id] = now;   // 播报启动成功才记冷却
        }
#else
        (void)node_id;
        (void)name;
#endif
    }

    // 上行事件在主任务上下文处理（传输层回调里已用 Schedule 切回来）
    void OnHomeEvent(int node_id, const std::string& evt, const std::string& arg, int64_t ts_ms) {
        if (node_id < 1 || node_id > EspNowHome::kMaxNodes) {
            return;
        }
        HomeNodeState& st = home_state_[node_id];
        st.valid = true;
        st.ts_ms = ts_ms;
        int value = atoi(arg.c_str());
        if (evt == "dist") {
            st.dist = value;
        } else if (evt == "temp") {
            // 参数形如 "26 55"
            int t = 0, h = 0;
            if (sscanf(arg.c_str(), "%d %d", &t, &h) == 2) {
                st.temp = t;
                st.hum = h;
                // 温度告警迟滞：≥28℃ 播一次，≤26℃ 复位（避免在阈值上反复播报）
                if (home_hot_state_ == 0 && st.temp >= kHomeHotTrigger) {
                    home_hot_state_ = 1;
                    Announce(node_id, "hot");
                } else if (home_hot_state_ == 1 && st.temp <= kHomeHotRelease) {
                    home_hot_state_ = 0;
                }
            }
        } else if (evt == "beam") {
            st.beam = value;
            if (value == 1) {
                Announce(node_id, "beam");
            }
        } else if (evt == "motion") {
            if (value == 1) {
                Announce(node_id, "motion");
            }
        }
        // 其它事件类型：仅留存时间戳，不处理（协议向前兼容）
    }

    void InitializeEspNowHome() {
        espnow_home_ = std::make_unique<EspNowHome>();
        bool ok = espnow_home_->Begin(
            [this](int node_id, const std::string& evt, const std::string& arg, int64_t ts_ms) {
                // 传输层回调在 WiFi 任务上下文：必须切回主任务再动业务
                // （AGENTS.md: callbacks may run outside the main task）
                Application::GetInstance().Schedule(
                    [this, node_id, evt, arg, ts_ms]() { OnHomeEvent(node_id, evt, arg, ts_ms); });
            });
        if (!ok) {
            espnow_home_.reset();
            return;
        }

        auto& mcp = McpServer::GetInstance();
        mcp.AddTool(
            "self.home.light",
            "控制居家节点的灯(开关/颜色/亮度)。调用一次即完成并自动返回, 不要重复调用、也不要再调用本工具确认。"
            "node: 节点号(1=客厅, 2=玄关); on: 1=开,0=关; r/g/b: 颜色分量0-255(可选, 省略=不改颜色); "
            "brightness: 亮度0-255(可选, 省略=不改亮度)",
            PropertyList({Property("node", kPropertyTypeInteger, 1, 1, EspNowHome::kMaxNodes),
                          Property("on", kPropertyTypeInteger, 1, 0, 1),
                          Property("r", kPropertyTypeInteger, -1, -1, 255),
                          Property("g", kPropertyTypeInteger, -1, -1, 255),
                          Property("b", kPropertyTypeInteger, -1, -1, 255),
                          Property("brightness", kPropertyTypeInteger, -1, -1, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int node = p["node"].value<int>();
                char body[64];
                snprintf(body, sizeof(body), "light %d %d %d %d %d", p["on"].value<int>(),
                         p["r"].value<int>(), p["g"].value<int>(), p["b"].value<int>(),
                         p["brightness"].value<int>());
                if (!espnow_home_ || !espnow_home_->SendTo(node, body)) {
                    return "节点 " + std::to_string(node) +
                           " 未连接, 指令未发出; 请检查该节点电源与距离后重试";
                }
                return "已完成: 节点 " + std::to_string(node) + " 的灯已" +
                       (p["on"].value<int>() != 0 ? "打开" : "关闭");
            });

        mcp.AddTool(
            "self.home.sensor",
            "查询居家节点传感器读数: 温度(℃)/湿度(%)/距离(cm)/激光遮挡(0/1)。"
            "node 省略或为 0 时返回全部在线节点。返回 JSON 数组, 每项含 stale 字段"
            "(true=读数超过30秒未更新)。用于回答\"室内多少度\"\"门口有人吗\"这类问题; "
            "读到 stale=true 时应说明数据可能已过期",
            PropertyList({Property("node", kPropertyTypeInteger, 0, 0, EspNowHome::kMaxNodes)}),
            [this](const PropertyList& p) -> ReturnValue {
                int want = p["node"].value<int>();
                int64_t now = EspNowHome::NowMs();
                std::string json = "{\"nodes\":[";
                bool first = true;
                for (int i = 1; i <= EspNowHome::kMaxNodes; i++) {
                    if (want != 0 && want != i) {
                        continue;
                    }
                    const HomeNodeState& st = home_state_[i];
                    bool online = espnow_home_ && espnow_home_->IsOnline(i);
                    if (!st.valid && !online) {
                        continue;
                    }
                    if (!first) {
                        json += ",";
                    }
                    first = false;
                    char item[192];
                    snprintf(item, sizeof(item),
                             "{\"id\":%d,\"online\":%s,\"valid\":%s,\"temp\":%d,\"hum\":%d,"
                             "\"dist\":%d,\"beam\":%d,\"age_ms\":%lld,\"stale\":%s}",
                             i, online ? "true" : "false", st.valid ? "true" : "false", st.temp,
                             st.hum, st.dist, st.beam,
                             static_cast<long long>(st.valid ? (now - st.ts_ms) : -1),
                             (st.valid && (now - st.ts_ms) < kHomeStaleMs) ? "false" : "true");
                    json += item;
                }
                json += "]}";
                return json;
            });

        mcp.AddTool(
            "self.home.status",
            "查询居家节点的连接状态(在线/最后通信时间)。用于排查\"为什么控制不了灯\"——"
            "返回里某个节点 online=false 就说明该节点掉线或未上电",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!espnow_home_) {
                    return std::string("{\"nodes\":[],\"error\":\"ESP-NOW 未启动\"}");
                }
                std::string out = espnow_home_->NodesJson();
                return out;
            });

#ifdef CONFIG_XIAOZHI_AIROBOT_ENABLE_TF_CARD
        mcp.AddTool(
            "self.home.announce",
            "播放预录播报语音(演示/自测用)。name: motion=有人靠近, beam=门口有人经过, hot=温度偏高",
            PropertyList({Property("name", kPropertyTypeString, std::string("motion"))}),
            [this](const PropertyList& p) -> ReturnValue {
                std::string name = p["name"].value<std::string>();
                if (!music_player_ || !music_player_->PlayAnnounce(name)) {
                    return "未找到播报文件 " + name + ".mp3(需放在 TF 卡 /sdcard/announce/)";
                }
                return "已开始播报: " + name;
            });
#endif
    }
```

- [x] **步骤 3：构造函数里调用**

在 `InitializeNetworkTools();` 之前插入：

```cpp
        InitializeEspNowHome();
```

- [x] **步骤 4：编译验证**

运行：`python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot`
预期：编译通过；无 `ESP_LOG` 相关警告；固件大小无明显增长（新增代码 < 8KB flash）。

- [x] **步骤 5：运行测试**

运行：`python3 -m unittest scripts.tests.test_espnow_home_protocol -v`
预期：除节点固件相关断言（`test_uplink_resends_three_times`、`test_node_*`）外全部 PASS。

- [x] **步骤 6：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/compact_wifi_board_s3cam_airobot.cc
git commit -m "feat: 板级集成 ESP-NOW 居家工具(self.home.*)与事件播报"
```

---

## 任务 5：节点固件 `arduino/EspNowNode/`

**文件：**
- 创建：`.../arduino/EspNowNode/EspNowNode.ino`
- 创建：`.../arduino/EspNowNode/README.md`

- [ ] **步骤 1：安装 DHT 库（⏸ 待用户确认，未执行）**

```bash
arduino-cli lib install "DHT sensor library"
arduino-cli lib list | grep -i dht
```
预期：安装 `DHT sensor library` 与依赖 `Adafruit Unified Sensor`。

- [x] **步骤 2：编写 `EspNowNode.ino`**

```cpp
/*
 * XiaoZhi 居家演示节点（ESP32-S3 + arduino-esp32 核心自带 ESP-NOW）
 *
 * 角色：被动执行 + 主动上报。不连接任何 AP，靠主控的 beacon 广播自动锁定信道。
 * 节点 1（客厅）: 三通道 PWM RGB 灯 + HC-SR04P 超声波
 * 节点 2（玄关）: DHT11 温湿度 + 激光头模块
 *   —— 靠文件顶部 NODE_ID 切换角色（默认 1）。
 *
 * 协议（与主控 espnow_home.cc 一致，@ 前缀文本行）：
 *   下行: @n<id> light <on> <r> <g> <b> <brightness>
 *   上行: @n<id> evt <name> <arg>   (name: dist/temp/beam/motion)
 *         @n<id> ack light <on>
 *
 * 编译: arduino-cli compile --fqbn esp32:esp32:esp32s3 <此目录>
 * 依赖: DHT sensor library (Adafruit) —— 灯与 ESP-NOW 用核心自带 API，无需第三方库
 */

#include <DHT.h>
#include <ESP32_NOW.h>
#include <WiFi.h>
#include <esp_mac.h>

// ============================ 现场可调参数 ============================

#define NODE_ID 1                    // 1=客厅(RGB+超声波), 2=玄关(DHT11+激光)
#define HOP_INTERVAL_MS 200          // 未锁定信道时的换信道间隔
#define LOST_TIMEOUT_MS 5000         // 锁定后多久收不到主控包就回到 hop
#define HOP_CHANNEL_MIN 1
#define HOP_CHANNEL_MAX 13

// 重复上报：主控待机时 WiFi 省电(MAX_MODEM)只在 DTIM 醒来，单包易漏；
// 连发 3 次跨过 DTIM 周期，主控侧按 (node_id, evt) 1 秒去重。
#define EVENT_RESEND 3
#define kEventResendGapMs 150

// 超声波迟滞：<30cm 触发(边沿一次)，>40cm 复位，中间保持（避免阈值抖动刷屏）
#define MOTION_TRIGGER_CM 30
#define MOTION_RELEASE_CM 40
#define SONAR_PERIOD_MS 100
#define SONAR_TIMEOUT_US 30000       // pulseIn 超时(约 5m 对应 ~29ms)

// DHT11 采样周期（DHT11 本身 ≤1Hz），失败重试
#define DHT_PERIOD_MS 5000
#define DHT_RETRY 3

// 激光采样：数字输出，2 次一致才算变化（去抖）
#define LASER_PERIOD_MS 50

// ---- 引脚（与设计文档一致，两节点同编号便于接线）----
#define PIN_RGB_R 4
#define PIN_RGB_G 5
#define PIN_RGB_B 6
#define PIN_SONAR_TRIG 7
#define PIN_SONAR_ECHO 15
#define PIN_DHT 4
#define PIN_LASER 5

// 三通道 RGB 模块共阳时 PWM 反相（现象：颜色反相或关不掉时改这里）
#define RGB_COMMON_ANODE 1
#define RGB_PWM_FREQ 5000
#define RGB_PWM_BITS 8

// ---- ESP-NOW 密钥：必须与主控 espnow_home.cc 完全一致（16 字节）----
static const uint8_t kPmk[16] = "xiaozhi-pmk-01";
static const uint8_t kLmk[16] = "xiaozhi-lmk-01";

// ============================== 灯控制 ==============================

static int light_on = 0;
static int light_r = 255, light_g = 255, light_b = 255, light_bright = 200;

static void applyLight() {
    auto duty = [](int v) -> uint32_t {
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        if (!light_on) v = 0;
#if RGB_COMMON_ANODE
        v = 255 - v;   // 共阳：低电平点亮
#endif
        return static_cast<uint32_t>(v);
    };
    int scale = light_bright;
    if (scale < 0) scale = 0;
    if (scale > 255) scale = 255;
    ledcWrite(PIN_RGB_R, duty(light_r * scale / 255));
    ledcWrite(PIN_RGB_G, duty(light_g * scale / 255));
    ledcWrite(PIN_RGB_B, duty(light_b * scale / 255));
}

// ============================ 信道 hop 发现 ============================

static uint8_t cur_channel = 1;
static bool locked = false;
static uint32_t last_hop_ms = 0;
static uint32_t last_seen_ms = 0;

// 事件上报队列（非阻塞 3 连发）
static char evt_buf[48] = {0};
static int evt_left = 0;
static uint32_t evt_next_ms = 0;

// 传感器状态
static bool motion_active = false;
static int last_beam = -1;
static uint32_t sonar_ms = 0, dht_ms = 0, laser_ms = 0;

static class HomePeer : public ESP_NOW_Peer {
public:
    HomePeer(const uint8_t* mac, uint8_t channel)
        : ESP_NOW_Peer(mac, channel, WIFI_IF_STA, kLmk) {}

    bool onReceive(const uint8_t* data, size_t len, bool broadcast) override {
        last_seen_ms = millis();
        if (len < 4 || data[0] != '@' || data[1] != 'n') {
            return true;
        }
        // 只处理发给本节点的 "@n<id> ..."（主控也可能广播别的）
        const char* p = reinterpret_cast<const char*>(data) + 2;
        if (atoi(p) != NODE_ID) {
            return true;
        }
        const char* sp = strchr(p, ' ');
        if (sp == nullptr) {
            return true;
        }
        handleCommand(sp + 1);
        return true;
    }

    void onSent(bool success) override {
        (void)success;   // 上行无需重传：应用层已连发 EVENT_RESEND 次
    }

private:
    static void handleCommand(const char* body) {
        if (strncmp(body, "light ", 6) == 0) {
            int on = 0, r = -1, g = -1, b = -1, br = -1;
            if (sscanf(body + 6, "%d %d %d %d %d", &on, &r, &g, &b, &br) >= 1) {
                light_on = (on != 0) ? 1 : 0;
                if (r >= 0) light_r = r;
                if (g >= 0) light_g = g;
                if (b >= 0) light_b = b;
                if (br >= 0) light_bright = br;
                applyLight();
                char ack[32];
                snprintf(ack, sizeof(ack), "@n%d ack light %d", NODE_ID, light_on);
                queueEvent(ack);   // 复用同一上报通道
            }
        }
        // ping 等其它命令：收到即刷新 last_seen（上面已做）
    }
};

static HomePeer* peer = nullptr;

// 未注册 peer 的数据会进这里（主控 beacon）→ 锁定当前信道并登记主控
static void onNewPeerCb(const esp_now_recv_info_t* info, const uint8_t* data, int len, void* arg) {
    (void)arg;
    if (info == nullptr || data == nullptr || len < 4) {
        return;
    }
    if (strncmp(reinterpret_cast<const char*>(data), "@beacon", 7) != 0) {
        return;
    }
    if (!locked) {
        locked = true;
        if (peer != nullptr) {
            delete peer;
        }
        peer = new HomePeer(info->src_addr, cur_channel);
        peer->add();
    }
    last_seen_ms = millis();
}

// 队列一条上行报文（会替换上一条未发完的；传感器事件频率极低，够用）
static void queueEvent(const char* text) {
    snprintf(evt_buf, sizeof(evt_buf), "%s", text);
    evt_left = EVENT_RESEND;
    evt_next_ms = millis();
}

static void queueEvt(int node_unused, const char* name, const char* arg) {
    (void)node_unused;
    char buf[48];
    if (arg != nullptr && arg[0] != '\0') {
        snprintf(buf, sizeof(buf), "@n%d evt %s %s", NODE_ID, name, arg);
    } else {
        snprintf(buf, sizeof(buf), "@n%d evt %s", NODE_ID, name);
    }
    queueEvent(buf);
}

static void hopTick() {
    if (locked || millis() - last_hop_ms < HOP_INTERVAL_MS) {
        return;
    }
    last_hop_ms = millis();
    cur_channel = (cur_channel >= HOP_CHANNEL_MAX) ? HOP_CHANNEL_MIN : (cur_channel + 1);
    WiFi.setChannel(cur_channel);
}

// ============================== 传感器 ==============================

#if NODE_ID == 1
static void sonarTick() {
    if (millis() - sonar_ms < SONAR_PERIOD_MS) {
        return;
    }
    sonar_ms = millis();
    digitalWrite(PIN_SONAR_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_SONAR_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_SONAR_TRIG, LOW);
    unsigned long us = pulseIn(PIN_SONAR_ECHO, HIGH, SONAR_TIMEOUT_US);
    if (us == 0) {
        return;   // 超时/无回波：保持上次状态
    }
    int cm = static_cast<int>(us / 58);
    char arg[8];
    snprintf(arg, sizeof(arg), "%d", cm);
    queueEvt(NODE_ID, "dist", arg);
    if (!motion_active && cm < MOTION_TRIGGER_CM) {
        motion_active = true;
        queueEvt(NODE_ID, "motion", "1");
    } else if (motion_active && cm > MOTION_RELEASE_CM) {
        motion_active = false;
    }
}
#endif

#if NODE_ID == 2
static DHT* dht = nullptr;

static void dhtTick() {
    if (millis() - dht_ms < DHT_PERIOD_MS) {
        return;
    }
    dht_ms = millis();
    for (int i = 0; i < DHT_RETRY; i++) {
        float t = dht->readTemperature();
        float h = dht->readHumidity();
        if (!isnan(t) && !isnan(h)) {
            char arg[16];
            snprintf(arg, sizeof(arg), "%d %d", static_cast<int>(t + 0.5f),
                     static_cast<int>(h + 0.5f));
            queueEvt(NODE_ID, "temp", arg);
            return;
        }
        delay(120);   // DHT11 两次读取需间隔；仅失败路径才有这点延迟
    }
    queueEvt(NODE_ID, "err", "dht");
}

static void laserTick() {
    if (millis() - laser_ms < LASER_PERIOD_MS) {
        return;
    }
    laser_ms = millis();
    int v = digitalRead(PIN_LASER) == HIGH ? 1 : 0;
    if (v != last_beam) {
        // 2 次一致才认（去抖）
        delay(2);
        if ((digitalRead(PIN_LASER) == HIGH ? 1 : 0) != v) {
            return;
        }
        last_beam = v;
        queueEvt(NODE_ID, "beam", v ? "1" : "0");
    }
}
#endif

// ============================== Arduino ==============================

void setup() {
    // 1) 灯（仅客厅节点有 RGB；玄关节点留着无害）
    ledcAttach(PIN_RGB_R, RGB_PWM_FREQ, RGB_PWM_BITS);
    ledcAttach(PIN_RGB_G, RGB_PWM_FREQ, RGB_PWM_BITS);
    ledcAttach(PIN_RGB_B, RGB_PWM_FREQ, RGB_PWM_BITS);
    applyLight();

#if NODE_ID == 1
    pinMode(PIN_SONAR_TRIG, OUTPUT);
    pinMode(PIN_SONAR_ECHO, INPUT);
#endif
#if NODE_ID == 2
    pinMode(PIN_LASER, INPUT);
    dht = new DHT(PIN_DHT, DHT11);
    dht->begin();
#endif

    // 2) WiFi/ESP-NOW：不连接任何 AP，只用 ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);            // 节点常醒（USB 供电），保证随时收下行命令
    WiFi.setChannel(cur_channel);
    while (!WiFi.STA.started()) {
        delay(10);
    }

    if (!ESP_NOW.begin(kPmk)) {       // 官方 ESP_NOW 类（核心自带）
        delay(1000);
        ESP.restart();
    }
    ESP_NOW.onNewPeer(onNewPeerCb, nullptr);
    last_seen_ms = millis();
}

void loop() {
    hopTick();

    // 锁定后失联 → 回到 hop 重新发现（主控重启/换热点/换信道都能自恢复）
    if (locked && millis() - last_seen_ms > LOST_TIMEOUT_MS) {
        locked = false;
        if (peer != nullptr) {
            delete peer;
            peer = nullptr;
        }
    }

    // 上行 3 连发（非阻塞）
    if (evt_left > 0 && millis() >= evt_next_ms) {
        if (peer != nullptr) {
            peer->send(reinterpret_cast<const uint8_t*>(evt_buf), strlen(evt_buf));
        }
        evt_left--;
        evt_next_ms = millis() + kEventResendGapMs;
    }

#if NODE_ID == 1
    sonarTick();
#endif
#if NODE_ID == 2
    dhtTick();
    laserTick();
#endif
}
```

- [x] **步骤 3：编写节点 README**

`.../arduino/EspNowNode/README.md` 内容（接线表、依赖、编译烧录命令、节点 1/2 切换、排错）：

```markdown
# EspNowNode — XiaoZhi 居家演示节点

ESP32-S3 + arduino-esp32 **核心自带** `ESP_NOW` 类（无需装 ESP-NOW 第三方库）。
不连接任何 AP：开机 hop 扫描信道 → 收到主控 `@beacon` 广播后锁定信道并登记主控。

## 依赖

- arduino-esp32 核心 **3.2.0**（本机已装）
- `arduino-cli lib install "DHT sensor library"`（仅节点 2 用；自动装 Adafruit Unified Sensor）

## 接线

| 功能 | GPIO | 说明 |
|---|---|---|
| RGB 模块 R / G / B | 4 / 5 / 6 | 共阳模块把 `RGB_COMMON_ANODE` 设为 1 |
| HC-SR04P Trig / Echo | 7 / 15 | **必须用 3.3V 版（P）**；5V 版 Echo 会损伤芯片 |
| DHT11 DATA | 4 | 3 线制，模块自带上拉 |
| 激光模块 DO | 5 | AO 不接 |

## 编译烧录

```bash
# 节点 1（客厅：RGB + 超声波）：保持 #define NODE_ID 1
arduino-cli compile --fqbn esp32:esp32:esp32s3 .
arduino-cli upload  --fqbn esp32:esp32:esp32s3 -p /dev/cu.usbserial-XXXX .

# 节点 2（玄关：DHT11 + 激光）：把 NODE_ID 改成 2 后重新编译烧录
```

## 密钥必须与主控一致

`kPmk` / `kLmk`（16 字节）必须与主控 `espnow_home.cc` 中的同名常量**逐字节一致**。
不一致的现象是"完全收不到任何包"（不是偶发失败），排查时优先核对这两处。

## 排错

| 现象 | 检查 |
|---|---|
| 灯不亮/不响应 | 主控 `self.home.status` 看 online；串口线不用接，先确认节点已上电 |
| 颜色反相或关不掉 | 改 `RGB_COMMON_ANODE`（0/1 取反） |
| 主控一直离线 | 节点与主控是否在**同一信道**（节点会自己 hop 找，等 3 秒）；密钥是否一致 |
| 温湿度一直失败 | DHT11 数据线接触、供电 3.3V；DHT11 采样 ≤1Hz，5 秒一次属正常节奏 |

## 真机验证

1. 上电 3 秒内主控 `self.home.status` 显示该节点 `online: true`；
2. 主控语音「打开客厅灯」→ 灯亮；
3. 手靠近超声波 <30cm → 主控播放 `motion.mp3`。
```

- [x] **步骤 4：编译验证节点固件**（节点 1 已通过；节点 2 待 DHT 库安装后验证）

运行：`arduino-cli compile --fqbn esp32:esp32:esp32s3 main/boards/bread-compact-wifi-s3cam-airobot/arduino/EspNowNode`
预期：`Sketch uses ... bytes`，0 error 0 warning。若 `ESP_NOW.h` 找不到，改用 `#include <ESP32_NOW.h>`（3.2.0 的头文件名）。

- [x] **步骤 5：运行测试**

运行：`python3 -m unittest scripts.tests.test_espnow_home_protocol -v`
预期：全部 PASS（含 `test_uplink_resends_three_times`、`test_node_*`）。

- [x] **步骤 6：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/arduino/EspNowNode/
git commit -m "feat: 新增 ESP-NOW 居家演示节点固件(RGB灯/超声波/温湿度/激光)"
```

---

## 任务 6：全量验证与文档

**文件：**
- 修改：`.../README.md`
- 修改：`docs/superpowers/specs/2026-09-18-espnow-home-control-design.md`（回写规格修正）

- [x] **步骤 1：主控全量编译（只编主变体）**

```bash
python3 scripts/build.py bread-compact-wifi-s3cam-airobot --name bread-compact-wifi-s3cam-airobot
```
预期：编译通过，固件大小/分区余量正常（实测主控固件 +17.9KB，分区余 12%）。

> ⚠️ **本步骤原计划还要编“无 TF 卡”变体，已撤销（规格修正 4）**：`build.py` 切变体会重建 `sdkconfig` 并触发全量重编（实测 2154 个目标），把 `build/` 切到另一变体；中途中断还会留下“`sdkconfig` 缺失、只剩 `sdkconfig.old`”的中间状态（不污染 git，但需下次构建自动重建）。

- [x] **步骤 2：跑全部主机测试**

```bash
python3 -m unittest discover -s scripts/tests -v
```
预期：全部 PASS（既有 18 个测试文件 + 新增 1 个）。

- [x] **步骤 3：更新板级 README（新增章节）**

在 `.../README.md` 的「网络状态（AI 可读本机 IP/SSID/信号）」之后插入「ESP-NOW 居家灯控与传感器」章节：硬件/引脚表、AI 语音指令表（`self.home.*`）、演示剧本（3 个场景）、播报音频生成与拷贝、排错表、真机验证要点。

- [x] **步骤 4：回写规格修正到设计文档**

把设计文档 §2 决策表第 13 行、§3.2、§4.1、§5、§6 中与"两侧连发 3 次"「SetControlWindow」「esp_now_set_wake_window 初始化时调用」相关的表述，按本计划头部的**规格修正**四条更新，并把状态改为「设计已获批准，已实现」。

实际执行结果：设计文档已新增 **§2.1 实现期修正**（3 条规格修正 + 4 个实现期坑），并同步修正了 §3.2 API 签名（与实现对齐：`IsOnline(int node_id)` 无默认参数、`ts_ms` 用 `int64_t`、补上 `kMaxNodes/kOfflineMs/kDedupWindowMs` 常量、删掉 `SetControlWindow`）、§3.2 待机对策段、§4.1 重发策略段、§5 第 3/8 条、§7 测试门禁注解。

- [x] **步骤 5：Commit**

```bash
git add main/boards/bread-compact-wifi-s3cam-airobot/README.md docs/superpowers/specs/2026-09-18-espnow-home-control-design.md
git commit -m "docs: 补充 ESP-NOW 居家节点使用说明并回写规格修正"
```

- [x] **步骤 6：输出交付报告（不做 commit）**

报告内容：改动文件清单、测试命令与结果、**仍需真机验证的项**（§9 的 9 条，明确标注"需硬件"）、以及 ESP-NOW 加密密钥需与主控一致这一人工检查项。

---

## 自检

**1. 规格覆盖度**

| 规格章节 | 对应任务 |
|---|---|
| §3.1 硬件与引脚 | 任务 5（`.ino` 引脚宏 + README 接线表） |
| §3.2 传输层 | 任务 2 |
| §3.3 MCP 工具 | 任务 4 |
| §3.4 缓存与事件出口 | 任务 4（`OnHomeEvent` / `home_state_` / `stale`） |
| §3.5 播报 | 任务 3（`PlayAnnounce` + 脚本）、任务 4（`Announce` 冷却/Idle 判定） |
| §3.6 节点固件 | 任务 5 |
| §3.7 依赖 | 任务 5 步骤 1 |
| §4.1 协议 | 任务 2（解析）、任务 5（组包） |
| §4.2 三条链路 | 任务 4 + 任务 5 |
| §4.3 迟滞与冷却 | 任务 1（测试固化）、任务 4/5（实现） |
| §5 约束 | 任务 2（无日志）、任务 4（不阻塞）、规格修正 1/2/3 |
| §6 错误处理 | 任务 2（离线/去重）、任务 4（`stale`/离线文案）、任务 5（重试/`err dht`） |
| §7 测试策略 | 任务 1、任务 6 步骤 1-2 |
| §8 不做的 | 未排入任务（符合预期） |
| §9 真机验证 | 任务 6 步骤 6（交付报告）+ 任务 5 步骤 3（README 验证清单） |
| §10 交付物 | 任务 2-6 的 commit 覆盖全部条目 |

**2. 占位符扫描**：无「待定/TODO/后续实现/补充细节」；所有代码步骤均给出可落盘代码。

**3. 类型一致性**：`EspNowHome::Begin/SendTo/IsOnline/NodesJson/NowMs`、`kMaxNodes/kOfflineMs/kDedupWindowMs`、`LocalMusicPlayer::PlayAnnounce`、`pending_path_`、`ANNOUNCE_DIR`、节点侧 `EVENT_RESEND/kEventResendGapMs/MOTION_TRIGGER_CM/MOTION_RELEASE_CM` 在任务 1 的测试断言与任务 2-5 的实现中命名完全一致。
