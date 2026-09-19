"""测试 ESP-NOW 居家节点协议：报文解析、上行去重、状态缓存、设备注册表，以及两侧源码约定断言。

架构（2026-09-18 数据驱动重构后）：
  - 节点自描述能力：`@n1 info <名字> <能力规格>`（规格里带人类可读描述与动作/参数）
  - 主控语义无关：只把 `<能力名>` 与 `<其余规格文本>` 拆开存进注册表，**不解释**动作含义
  - 通用执行：`@n1 do <能力> <动作> [参数]` → 节点 `@n1 ok <能力> <结果>`
  - 播报与状态分离：`say <名字>` 触发播 `<名字>.mp3`；`evt <名字> <值>` 只更新状态缓存

对应实现：
  - C++     main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.cc
            的 HandleRecv() / ShouldDeliver() / UpsertState() / DevicesJson()
  - Arduino main/boards/bread-compact-wifi-s3cam-airobot/arduino/EspNowNode/EspNowNode.ino
            的能力表（CapDef/NodeDef）、sendInfo()、do 分发、超声波迟滞与 3 连发

固化的三个真机隐患：
  1. 主控待机时 WiFi 为 MAX_MODEM，只在 DTIM 醒来，ESP-NOW 单包上行会被漏掉 ——
     故节点必须连发 EVENT_RESEND 次，主控按 (kind, 名字) 1 秒窗口去重。
  2. 下行若也连发，会在 MCP 工具回调里阻塞约 (N-1)*150ms 卡住对话 ——
     而节点常醒（USB 供电、WiFi.setSleep(false)），故下行必须单次非阻塞发送。
  3. 主控不得硬编码任何业务语义（能力名/事件名/播报文件名/温度阈值），
     否则接新设备就要重烧主控 —— 这是本文件大部分源码断言的由来。

真机验证仍需烧录后手动测试（见板级 README 的「ESP-NOW 居家灯控与传感器」章节）。
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
PLAYER_CC = os.path.join(BOARD_DIR, "local_music_player.cc")
WEB_INDEX = os.path.join(BOARD_DIR, "web", "index.html")
UPLOAD_CC = os.path.join(BOARD_DIR, "http_upload_server.cc")

MAX_NODES = 4               # 与 EspNowHome::kMaxNodes 一致
MAX_CAPS = 4                # 与 EspNowHome::kMaxCaps 一致（融合节点有 4 个能力）
NAME_LEN = 20
CAP_NAME_LEN = 14
CAP_SPEC_LEN = 48
STATE_LEN = 96
DEDUP_WINDOW_MS = 1000      # 与 EspNowHome::kDedupWindowMs 一致
MOTION_TRIGGER_CM = 30      # 超声波触发阈值（节点侧）
MOTION_RELEASE_CM = 40      # 超声波复位阈值（迟滞）
EVENT_RESEND = 3            # 上行事件连发次数

KINDS = ("info", "do", "ok", "say", "evt", "err")


# ============================ 协议解析复刻 ============================

def pkt(s: str) -> bytes:
    """构造上行报文。含中文时必须显式编码：bytes 字面量只能是 ASCII。"""
    return s.encode("utf-8")


def parse_node_packet(data: bytes):
    """复刻 HandleRecv 的信封解析：返回 (node_id, body) 或 None。"""
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
    if node_id <= 0 or node_id > MAX_NODES:
        return None
    return node_id, data[i + 1:].decode("utf-8", "ignore")


def parse_upstream(data: bytes):
    """复刻 HandleRecv 的正文分类：返回 (node_id, kind, name, arg) 或 None。

    - info: name=设备名, arg=能力规格串
    - ok  : name=能力名, arg=结果文本
    - say : name=播报名, arg=""（播报不带参数）
    - evt : name=状态项, arg=值
    - err : name=原因标签, arg=""（实现在 err 后整段作为 name 也可，测试用两段式）
    """
    parsed = parse_node_packet(data)
    if parsed is None:
        return None
    node_id, body = parsed
    parts = body.split(" ", 2)
    kind = parts[0]
    if kind not in KINDS:
        return None
    if kind == "info":
        if len(parts) < 3:
            return None
        return node_id, "info", parts[1], parts[2]
    name = parts[1] if len(parts) > 1 else ""
    arg = parts[2] if len(parts) > 2 else ""
    if not name:
        return None
    return node_id, kind, name, arg


def parse_caps(text: str):
    """复刻注册表对能力规格的解析：只拆能力名与其余规格文本，不解释动作语义。

    'light(RGB灯):on(0|1),rgb(r,g,b);dist(距离cm):read()'
      → [('light', '(RGB灯):on(0|1),rgb(r,g,b)'), ('dist', '(距离cm):read()')]

    能力名结束于第一个 '(' 或 ':'（取靠前者），因此 'light:on(0|1)' → ('light', ':on(0|1)')。
    """
    out = []
    for item in text.split(";"):
        item = item.strip()
        if not item:
            continue
        # 能力名结束于第一个 '(' 或 ':'（取靠前者）：
        #   'light(RGB灯):on(0|1)' → ('light', '(RGB灯):on(0|1)')
        #   'light:on(0|1)'        → ('light', ':on(0|1)')
        cut = len(item)
        for sep in ("(", ":"):
            pos = item.find(sep)
            if pos > 0:
                cut = min(cut, pos)
        out.append((item[:cut][:CAP_NAME_LEN], item[cut:][:CAP_SPEC_LEN]))
    return out[:MAX_CAPS]


def upsert_state(state: str, key: str, value: str) -> str:
    """复刻 UpsertState：'key=value' 已存在则替换，否则追加；超长截断。"""
    entry = "%s=%s" % (key, value)
    out = []
    replaced = False
    for pair in state.split(" "):
        if not pair:
            continue
        if pair.split("=", 1)[0] == key:
            if not replaced:
                out.append(entry)
                replaced = True
            # 同名旧项直接丢弃（保持唯一）
        else:
            out.append(pair)
    if not replaced:
        out.append(entry)
    return " ".join(out)[:STATE_LEN]


def make_dedup():
    return {"slots": [], "window_ms": DEDUP_WINDOW_MS}


def dedup_key(kind: str, name: str) -> str:
    """去重键必须是 kind+名字：否则 say motion 会吞掉 evt motion（两者语义不同）。"""
    return "%s %s" % (kind, name)


def should_deliver(state, node_id, kind, name, now_ms):
    """复刻 ShouldDeliver：窗口内相同 (node_id, key) 吞掉并续期。"""
    key = dedup_key(kind, name)
    for s in state["slots"]:
        if s["node_id"] == node_id and s["key"] == key and (now_ms - s["ts"]) < state["window_ms"]:
            s["ts"] = now_ms
            return False
    state["slots"].append({"node_id": node_id, "key": key, "ts": now_ms})
    return True


def make_motion_hysteresis():
    return {"active": False}


def motion_update(st, dist_cm):
    """复刻节点侧超声波迟滞：<30cm 触发(边沿一次)，>40cm 复位，中间保持。"""
    if not st["active"] and dist_cm < MOTION_TRIGGER_CM:
        st["active"] = True
        return True          # 事件：有人靠近 → 节点发 say motion
    if st["active"] and dist_cm > MOTION_RELEASE_CM:
        st["active"] = False
    return False


class DeviceTable:
    """复刻主控注册表（容量/覆盖/在线/info_seen）。"""

    def __init__(self, max_nodes=MAX_NODES, offline_ms=15000):
        self.max_nodes = max_nodes
        self.offline_ms = offline_ms
        self.devices = {}          # id -> dict

    def touch(self, node_id, now_ms):
        """收到任何报文都刷新在线时间；返回是否为新节点（超出容量返回 None）。"""
        dev = self.devices.get(node_id)
        if dev is None:
            if len(self.devices) >= self.max_nodes:
                return None
            dev = {"id": node_id, "name": "", "caps": [], "state": "",
                   "info_seen": False, "last_seen_ms": now_ms}
            self.devices[node_id] = dev
        else:
            dev["last_seen_ms"] = now_ms
        return dev

    def apply_info(self, node_id, name, caps_text, now_ms=0):
        dev = self.touch(node_id, now_ms)
        if dev is None:
            return None
        dev["name"] = name[:NAME_LEN]
        dev["caps"] = parse_caps(caps_text)     # 整体覆盖：旧能力不许残留
        dev["info_seen"] = True
        return dev

    def apply_state(self, node_id, key, value, now_ms=0):
        dev = self.touch(node_id, now_ms)
        if dev is None:
            return None
        dev["state"] = upsert_state(dev["state"], key, value)
        return dev

    def online(self, node_id, now_ms):
        dev = self.devices.get(node_id)
        if dev is None:
            return False
        return (now_ms - dev["last_seen_ms"]) < self.offline_ms

    def has_cap(self, node_id, cap):
        dev = self.devices.get(node_id)
        if dev is None:
            return False
        return any(c[0] == cap for c in dev["caps"])

    def name_of(self, node_id):
        dev = self.devices.get(node_id)
        if dev is None or not dev["name"]:
            return "节点%d" % node_id
        return dev["name"]


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


# ================================ 测试 ================================

class TestParseEnvelope(unittest.TestCase):
    def test_evt_with_arg(self):
        self.assertEqual(parse_node_packet(b"@n1 evt dist 23"), (1, "evt dist 23"))

    def test_evt_two_args(self):
        self.assertEqual(parse_node_packet(b"@n2 evt temp 26 55"), (2, "evt temp 26 55"))

    def test_ok_with_result(self):
        self.assertEqual(parse_node_packet(b"@n1 ok light 1 0 0 255"),
                         (1, "ok light 1 0 0 255"))

    def test_say(self):
        self.assertEqual(parse_node_packet(b"@n2 say beam"), (2, "say beam"))

    def test_info(self):
        self.assertEqual(parse_node_packet(pkt("@n1 info 客厅灯 light(x):on(0|1)")),
                         (1, "info 客厅灯 light(x):on(0|1)"))

    def test_max_node_ok(self):
        self.assertEqual(parse_node_packet(b"@n4 evt beam 1"), (4, "evt beam 1"))

    def test_reject_beyond_max_node(self):
        # 节点表只有 kMaxNodes 项，超出的必须被拒（否则会写坏节点表语义）
        self.assertIsNone(parse_node_packet(b"@n5 evt beam 1"))
        self.assertIsNone(parse_node_packet(b"@n12 evt beam 1"))

    def test_reject_bad(self):
        for bad in (b"", b"@", b"@n", b"@n1", b"@n1x", b"@nx evt beam 1",
                    b"n1 evt beam 1", b"@n0 evt beam 1", b"@n1evt"):
            self.assertIsNone(parse_node_packet(bad), bad)


class TestParseUpstream(unittest.TestCase):
    def test_info_splits_device_name_and_caps(self):
        self.assertEqual(parse_upstream(pkt("@n1 info 客厅灯 light(a):on(0|1);dist(b):read()")),
                         (1, "info", "客厅灯", "light(a):on(0|1);dist(b):read()"))

    def test_info_without_caps_rejected(self):
        # 能力描述是自描述的核心，缺了就丢弃（避免注册出空壳设备）
        self.assertIsNone(parse_upstream(pkt("@n1 info 客厅灯")))

    def test_ok(self):
        self.assertEqual(parse_upstream(b"@n1 ok light 1 0 0 255"),
                         (1, "ok", "light", "1 0 0 255"))

    def test_say_has_no_arg(self):
        self.assertEqual(parse_upstream(b"@n1 say motion"), (1, "say", "motion", ""))

    def test_evt(self):
        self.assertEqual(parse_upstream(b"@n2 evt temp 26 55"), (2, "evt", "temp", "26 55"))

    def test_err(self):
        self.assertEqual(parse_upstream(b"@n1 err unknown-cap light"),
                         (1, "err", "unknown-cap", "light"))

    def test_unknown_kind_rejected(self):
        # 旧协议的 ack/light 必须被拒（协议已退役，避免半新半旧状态）
        self.assertIsNone(parse_upstream(b"@n1 ack light 1"))
        self.assertIsNone(parse_upstream(b"@n1 light 1 0 0 255"))

    def test_do_is_not_an_uplink(self):
        """do 是下行专用：节点若把它发上来必须被忽略（防回环）。"""
        self.assertEqual(parse_upstream(b"@n1 do light on 1")[1], "do")


class TestCapSpecParsing(unittest.TestCase):
    def test_split_name_and_spec(self):
        caps = parse_caps("light(RGB灯):on(0|1),rgb(r,g,b);dist(距离cm):read()")
        self.assertEqual(caps, [("light", "(RGB灯):on(0|1),rgb(r,g,b)"),
                                ("dist", "(距离cm):read()")])

    def test_plain_name_without_description(self):
        self.assertEqual(parse_caps("light:on(0|1)"), [("light", ":on(0|1)")])

    def test_capacity_limited(self):
        # 不写死 3：融合节点把上限抬到 4，这里跟着 MAX_CAPS 走
        items = ";".join("c%d(x):m()" % i for i in range(MAX_CAPS + 1))
        caps = parse_caps(items)
        self.assertEqual(len(caps), MAX_CAPS)
        self.assertEqual([c[0] for c in caps], ["c%d" % i for i in range(MAX_CAPS)])

    def test_empty_items_ignored(self):
        self.assertEqual([c[0] for c in parse_caps("light(x):m();;dist(y):r()")],
                         ["light", "dist"])

    def test_length_truncated(self):
        name, spec = parse_caps("x" * 40 + "(" + "y" * 200)[0]
        self.assertEqual(len(name), CAP_NAME_LEN)
        self.assertEqual(len(spec), CAP_SPEC_LEN)


class TestStateCache(unittest.TestCase):
    def test_append(self):
        self.assertEqual(upsert_state("", "dist", "23"), "dist=23")

    def test_replace_existing(self):
        self.assertEqual(upsert_state("dist=23 temp=26 55", "dist", "40"),
                         "dist=40 temp=26 55")

    def test_append_after_existing(self):
        self.assertEqual(upsert_state("dist=23", "temp", "26 55"), "dist=23 temp=26 55")

    def test_no_duplicate_keys(self):
        self.assertEqual(upsert_state("light=1 light=0", "light", "1"), "light=1")

    def test_truncated_to_buffer(self):
        st = upsert_state("", "k", "v" * 500)
        self.assertLessEqual(len(st), STATE_LEN)


class TestDedup(unittest.TestCase):
    def test_resend_three_times_collapses_to_one(self):
        st = make_dedup()
        # 节点连发 3 次，间隔 150ms，全部落在 1s 窗口内
        results = [should_deliver(st, 1, "say", "motion", t) for t in (0, 150, 300)]
        self.assertEqual(results, [True, False, False])

    def test_different_nodes_not_collapsed(self):
        st = make_dedup()
        self.assertTrue(should_deliver(st, 1, "say", "motion", 0))
        self.assertTrue(should_deliver(st, 2, "say", "motion", 0))

    def test_different_events_not_collapsed(self):
        st = make_dedup()
        self.assertTrue(should_deliver(st, 1, "say", "motion", 0))
        self.assertTrue(should_deliver(st, 1, "say", "beam", 0))

    def test_say_and_evt_same_name_not_collapsed(self):
        """say 与 evt 是两条通道：同名不得互相吞（否则播报会被状态上报吃掉）。"""
        st = make_dedup()
        self.assertTrue(should_deliver(st, 1, "say", "motion", 0))
        self.assertTrue(should_deliver(st, 1, "evt", "motion", 0))

    def test_after_window_allowed_and_resend_refreshes(self):
        st = make_dedup()
        self.assertTrue(should_deliver(st, 1, "say", "beam", 0))
        # 窗口内的连发被吞且续期：从 900ms 续到 1050ms
        self.assertFalse(should_deliver(st, 1, "say", "beam", 900))
        self.assertFalse(should_deliver(st, 1, "say", "beam", 1050))
        # 续期后需再等满一个窗口
        self.assertTrue(should_deliver(st, 1, "say", "beam", 1050 + DEDUP_WINDOW_MS))


class TestDeviceTable(unittest.TestCase):
    def test_info_registers_device(self):
        t = DeviceTable()
        dev = t.apply_info(1, "客厅灯", "light(RGB灯):on(0|1),rgb(r,g,b);dist(距离cm):read()", 0)
        self.assertTrue(dev["info_seen"])
        self.assertEqual(dev["name"], "客厅灯")
        self.assertEqual([c[0] for c in dev["caps"]], ["light", "dist"])
        self.assertTrue(t.has_cap(1, "light"))
        self.assertFalse(t.has_cap(1, "temp"))

    def test_repeat_info_overwrites_caps(self):
        """节点能力改了（重刷固件）后重报 info，旧能力不许残留。"""
        t = DeviceTable()
        t.apply_info(1, "客厅灯", "light(x):on(0|1);dist(y):read();motion(z):read()", 0)
        t.apply_info(1, "客厅灯", "light(x):on(0|1)", 0)
        self.assertEqual([c[0] for c in t.devices[1]["caps"]], ["light"])

    def test_capacity_limit(self):
        t = DeviceTable(max_nodes=MAX_NODES)
        for nid in range(1, MAX_NODES + 2):      # 多喂一个
            t.touch(nid, 0)
        self.assertEqual(len(t.devices), MAX_NODES)
        self.assertIsNone(t.devices.get(MAX_NODES + 1))
        # 已注册的节点仍可更新
        self.assertIsNotNone(t.touch(1, 100))

    def test_online_window(self):
        t = DeviceTable(offline_ms=15000)
        t.touch(1, 0)
        self.assertTrue(t.online(1, 14999))
        self.assertFalse(t.online(1, 15000))
        self.assertFalse(t.online(999, 0))       # 未知节点

    def test_name_fallback(self):
        t = DeviceTable()
        self.assertEqual(t.name_of(3), "节点3")
        t.apply_info(3, "玄关感应", "temp(x):read()", 0)
        self.assertEqual(t.name_of(3), "玄关感应")

    def test_state_cached_independently_of_caps(self):
        t = DeviceTable()
        t.apply_state(2, "temp", "26 55", 0)
        self.assertEqual(t.devices[2]["state"], "temp=26 55")
        self.assertFalse(t.devices[2]["info_seen"])   # 还没 info：只算见过，不算认识


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
        missing = [p for p in (ESPNOW_CC, ESPNOW_H, NODE_INO, BOARD_CC, WEB_INDEX, UPLOAD_CC)
                   if not os.path.exists(p)]
        if missing:
            raise AssertionError("缺少实现文件：%s" % missing)
        cls.cc = read(ESPNOW_CC)
        cls.h = read(ESPNOW_H)
        cls.ino = read(NODE_INO)
        cls.board = read(BOARD_CC)
        cls.web = read(WEB_INDEX)
        cls.upload = read(UPLOAD_CC)

    # ---------- 障碍检测：红外避障模块 ----------

    def test_obstacle_uses_infrared_module_with_configurable_polarity(self):
        """障碍检测用红外避障模块（低电平有效），极性可配置，且上电首次不播报。

        红外避障（FC-51 类）是“检测到障碍 = 低电平”，与激光接收模块（高电平）相反，
        所以极性抽成 OBSTACLE_ACTIVE_LOW；另外上电第一次读到的值只能记录，
        否则模块前方本来就挡着东西时，一上电就误播一次“有人经过”。
        """
        self.assertIn("#define OBSTACLE_ACTIVE_LOW", self.ino)
        self.assertRegex(self.ino, r"readObstacleRaw\(\) == LOW")   # 低电平有效分支
        self.assertIn("[obstacle]", self.ino)
        self.assertNotIn("[laser]", self.ino)
        # 日志必须打真实引脚电平：早期把归一化后的 0/1 当成了 DO 电平，拿万用表一对就矛盾
        self.assertIn("(pin=%s)", self.ino)
        self.assertNotIn("(DO=%d)", self.ino)
        self.assertIn("[obstacle] init", self.ino)   # 上电首帧只记录、不播报

    # ---------- Web 提示音（/sdcard/announce） ----------

    def test_web_manages_announce_files_via_dir_parameter(self):
        """提示音与歌曲共用一套上传/列表/删除接口，靠 dir 参数区分。"""
        self.assertIn("ANNOUNCE_DIR", self.upload)
        self.assertIn("DirFromQuery", self.upload)
        self.assertIn("IsAnnounceDir", self.upload)
        # 提示音目录只收 .mp3（没有 .lrc 歌词）
        self.assertIn("announce dir only accepts .mp3", self.upload)
        # 前端：固定槽位（名字必须与节点上报的事件名一致），一个都不能少
        self.assertIn("ANNOUNCE_SLOTS", self.web)
        for slot in ("motion", "hot", "beam"):
            self.assertIn("'" + slot + "'", self.web,
                          "提示音槽位 %s 缺失：它对应节点上报的 say %s" % (slot, slot))
        self.assertIn("openUpload('announce'", self.web)
        self.assertIn("dir: 'announce'", self.web)
        # 非 MP3（wav/m4a/flac 等）也要能上传：一律转码成 MP3
        self.assertIn("'audio'", self.web)

    def test_announce_dir_matcher_accepts_both_forms(self):
        """dir 有两种形态（查询值 "announce" 与目录路径 /sdcard/announce），匹配函数必须都认。

        WebSocket 传的是 "announce"，HTTP 上传经 DirFromQuery 得到的是目录路径；
        只认一种就会出现“上传成功但列表为空”（列表实际列的是歌曲目录，
        前端找不到 motion.mp3，于是永远显示未上传）。
        """
        self.assertIn('strcmp(dir, "announce") == 0', self.upload)
        self.assertIn('strcmp(dir, ANNOUNCE_DIR) == 0', self.upload)

    # ---------- 沿用首版的有效约束 ----------

    def test_no_esp_log_in_espnow(self):
        """本板 UART0 与 Arduino 下位机共用，传输层不得打日志。"""
        for m in re.finditer(r"ESP_LOG[A-Z]", self.cc):
            self.fail("espnow_home.cc 不得使用 ESP_LOG（本板串口与下位机共享）: %s"
                      % self.cc[max(0, m.start() - 60):m.start() + 60])

    def test_constants_match_test(self):
        self.assertIn("kDedupWindowMs = 1000", self.h)
        self.assertIn("kOfflineMs = 15000", self.h)
        self.assertIn("kMaxNodes = %d" % MAX_NODES, self.h)
        self.assertIn("kMaxCaps = %d" % MAX_CAPS, self.h)

    def test_downlink_is_single_send(self):
        """下行必须单次非阻塞发送：MCP 工具回调里连发会卡住对话。"""
        self.assertNotIn("kResendCount", self.h)
        self.assertNotIn("kResendGapMs", self.h)

    def test_uplink_resends_three_times(self):
        self.assertRegex(self.ino, r"EVENT_RESEND\s+%d" % EVENT_RESEND)
        self.assertIn("kEventResendGapMs", self.ino)

    def test_ifidx_sta_both_sides(self):
        """两侧 ifidx 必须都是 WIFI_IF_STA，否则互不可达。"""
        self.assertIn("WIFI_IF_STA", self.cc)
        self.assertIn("WIFI_IF_STA", self.ino)

    def test_node_uses_official_espnow_class(self):
        # 尖括号/双引号两种写法都接受（核心自带库两种都能解析）
        self.assertRegex(self.ino, r'#include\s*[<"]ESP32_NOW\.h[>"]')
        self.assertIn("onNewPeer", self.ino)
        self.assertIn("WiFi.setChannel", self.ino)

    def test_node_has_hysteresis_thresholds(self):
        self.assertIn("MOTION_TRIGGER_CM %d" % MOTION_TRIGGER_CM, self.ino)
        self.assertIn("MOTION_RELEASE_CM %d" % MOTION_RELEASE_CM, self.ino)

    def test_dirs_separated(self):
        """播报音频必须独立目录，否则会被 self.music.list 当成歌曲列出。"""
        player = read(PLAYER_CC)
        self.assertIn("/sdcard/announce", player)
        self.assertIn("/sdcard/music", player)

    def test_announce_rejects_path_traversal(self):
        player = read(PLAYER_CC)
        idx = player.find("PlayAnnounce")
        self.assertGreater(idx, -1, "local_music_player.cc 必须有 PlayAnnounce")
        body = player[idx:idx + 900]
        self.assertIn("find('/')", body)
        self.assertIn('find("..")', body)

    def test_announce_only_when_idle(self):
        """播报不得打断对话：必须判 Idle。"""
        idx = self.board.find("void Announce(")
        self.assertGreater(idx, -1, "板级必须有 Announce()")
        body = self.board[idx:idx + 900]
        self.assertIn("kDeviceStateIdle", body)

    # ---------- 数据驱动重构后的新约束 ----------

    def test_exactly_three_home_tools(self):
        """工具集固定：devices/control/announce；不得回退成按设备/能力硬编码的工具。"""
        found = set(re.findall(r'"(self\.home\.[a-z_]+)"', self.board))
        self.assertEqual(found, {"self.home.devices", "self.home.control",
                                 "self.home.announce"},
                         "self.home.* 工具必须恰好这三个，实际：%s" % sorted(found))

    def test_old_hardcoded_tools_removed(self):
        for gone in ("self.home.light", "self.home.sensor", "self.home.status"):
            self.assertNotIn(gone, self.board, "旧工具 %s 必须删除（它硬编码了节点角色）" % gone)

    def test_board_has_no_business_semantics(self):
        """主控不得再认识任何业务语义：能力名/事件名/温度阈值都不许出现。"""
        for gone in ("kHomeHotTrigger", "kHomeHotRelease", "kHomeStaleMs",
                     "HomeNodeState", 'evt == "', "home_hot_state_"):
            self.assertNotIn(gone, self.board,
                             "主控源码不得出现 %s（业务语义必须留在节点侧）" % gone)

    def test_board_only_dispatches_say_channel(self):
        """板级只处理播报通道；状态缓存由传输层维护。"""
        idx = self.board.find("void OnHomeEvent(")
        self.assertGreater(idx, -1, "板级必须有 OnHomeEvent()")
        body = self.board[idx:idx + 700]
        self.assertIn('kind == "say"', body)
        self.assertIn("Announce(", body)

    def test_espnow_layer_parses_new_protocol(self):
        for need in ('"info "', '"do "', '"ok "', '"say "', "UpsertState", "DevicesJson"):
            self.assertIn(need, self.cc, "espnow_home.cc 缺少 %s" % need)

    def test_espnow_exposes_registry_api(self):
        for need in ("DevicesJson", "HasNode", "NodeName", "HasCap", "kMaxCaps"):
            self.assertIn(need, self.h, "espnow_home.h 缺少 %s" % need)

    def test_registry_entry_has_expected_fields(self):
        for need in ("info_seen", "cap_count", "last_seen_ms"):
            self.assertIn(need, self.h, "注册表项缺少字段 %s" % need)

    def test_callback_carries_kind(self):
        """回调必须带 kind，板级才能区分"播报"与"状态"。"""
        self.assertRegex(self.h, r"EventCallback\s*=\s*std::function<void\(int node_id,\s*"
                                 r"const std::string& kind")

    def test_control_tool_appends_device_list_on_failure(self):
        """control 失败时返回设备清单，让 AI 一步自愈（不必先查再控）。"""
        idx = self.board.find('"self.home.control"')
        self.assertGreater(idx, -1, "必须有 self.home.control")
        body = self.board[idx:idx + 2200]
        self.assertIn("DevicesJson", body)

    def test_control_tool_forbids_repeat_call(self):
        """踩坑 15 回归防护：工具描述必须明确禁止重复调用。"""
        idx = self.board.find('"self.home.control"')
        body = self.board[idx:idx + 1200]
        self.assertIn("不要重复调用", body)

    def test_node_self_describes(self):
        """节点固件必须有能力表（CapDef/NodeDef）与 info 上报。"""
        for need in ("CapDef", "NodeDef", "sendInfo", "qCap"):
            self.assertIn(need, self.ino, "节点固件缺少 %s" % need)

    def test_node_dispatches_generic_action(self):
        # 从定义点起算窗口：调用点在 onReceive 里，距定义有几百字符
        idx = self.ino.find("static void handleCommand")
        self.assertGreater(idx, -1, "节点必须有 handleCommand 定义")
        body = self.ino[idx:idx + 2200]
        self.assertIn('"do "', body)
        self.assertIn("unknown-cap", body)
        self.assertIn("readonly", body)

    def test_node_parses_args_tolerantly(self):
        """参数格式必须宽容：AI 会传 "0,0,255"、"[0,0,255]" 或带多余空格。

        sscanf 的固定格式会拒掉这些写法，所以节点侧用“任何非数字字符即分隔符”的
        parseNums()，并用 nextField() 跳过连续空格——
        否则 "do light  rgb" 这种空字段会让命令被静默丢弃。
        """
        self.assertIn("parseNums", self.ino)
        self.assertIn("nextField", self.ino)
        self.assertRegex(self.ino, r"while \(\*p == ' '\)")

    def test_board_trims_ai_strings(self):
        """主控侧必须 trim cap/action/args。

        空格是协议分隔符，AI 传 "light " 会拼出 "do light  rgb"，
        节点侧会解析出空字段，导致命令被静默丢弃。
        """
        self.assertIn("find_first_not_of", self.board)
        self.assertIn("auto trim = [](std::string s)", self.board)

    def test_node_old_protocol_retired(self):
        """旧协议 ack/light 下行必须退役，避免两套语义并存。"""
        self.assertNotIn("ack light", self.ino)
        self.assertNotIn('"light "', self.ino)

    def test_node_serial_log_is_switchable(self):
        """节点串口是独占的（不像主控 UART0 与 Arduino 指令共用），日志默认开且可一键关。

        置 NODE_LOG=0 时整段日志必须在编译期消失（空宏）——现场收不到日志时
        要能彻底关掉，而不是只能改代码注释。
        """
        self.assertRegex(self.ino, r"#define NODE_LOG\s+1")
        self.assertIn("Serial.begin(LOG_BAUD)", self.ino)
        self.assertIn("#define LOGF(...) Serial.printf(__VA_ARGS__)", self.ino)
        self.assertIn("#define LOGF(...) ((void)0)", self.ino)

    def test_node_uplink_is_queued_and_coalesced(self):
        """上行必须排队 + 同键合并（见 spec §2.1 修正 9）。

        单槽缓冲会被同一 tick 的后一条上行静默覆盖（dist + motion + say），
        主控 state 因此缺字段、播报时有时无；但只排队又会被高频 dist 占满，
        所以还要按 "@n<id> <kind> <名字>" 同键合并。
        """
        self.assertNotIn("evt_buf", self.ino, "单槽缓冲已退役（spec §2.1 修正 9）")
        for need in ("TXQ_SIZE", "txq_count", "msgKey", "putText"):
            self.assertIn(need, self.ino, "上行队列缺少 %s" % need)
        self.assertIn("portENTER_CRITICAL", self.ino)   # 回调入队 ↔ loop 出队

    def test_node_queue_types_declared_before_first_function(self):
        """队列 struct 必须声明在第一个函数定义之前（防重现编译错误）。

        arduino-cli 会把函数原型自动插到第一个函数定义之前；若 `struct OutMsg` 定义在
        文件中部，`putText(OutMsg&, ...)` 的原型就会引用未声明的类型，报
        "OutMsg was not declared in this scope"（已实际重现过）。
        """
        struct_idx = self.ino.find("struct OutMsg")
        first_fn_idx = self.ino.find("static const char* fmtMac")
        self.assertGreater(struct_idx, -1, "缺少 struct OutMsg")
        self.assertGreater(first_fn_idx, -1, "未找到首个函数 fmtMac（测试需同步）")
        self.assertLess(struct_idx, first_fn_idx,
                        "OutMsg 必须声明在第一个函数定义之前，否则原型注入会编译失败")

    def test_node_say_channel_for_announce(self):
        """播报走 say 通道；状态类走 evt（否则距离会反复触发 SD 卡文件查找）。"""
        self.assertIn("queueSay", self.ino)
        self.assertIn("say %s", self.ino)        # "@n%d say %s"
        self.assertIn("queueEvt(\"dist\"", self.ino)

    def test_fusion_node_covers_all_four_sensors(self):
        """节点 3 = 融合节点：一块板接全部四个传感器（RGB + 超声波 + DHT11 + 激光）。

        关键约束：
        - RGB 与超声波沿用 GPIO4/5/6/7/15；DHT11/激光必须换到 16/17（4/5 已被 RGB 占用）；
        - 能力 4 个，正好等于主控 kMaxCaps，所以 kMaxCaps 必须 ≥4，否则第 4 个
          能力（beam）会被静默忽略、AI 看不见；
        - RGB 相关代码只能对节点 1/3 编译：原先无条件 ledcAttach(GPIO4/5/6) 会把
          玄关节点的 DHT11/激光脚一并配成 LEDC 输出。
        """
        self.assertIn("#if NODE_ID == 3", self.ino)
        self.assertRegex(self.ino, r"#define PIN_DHT\s+16")
        self.assertRegex(self.ino, r"#define PIN_LASER\s+17")
        self.assertGreaterEqual(MAX_CAPS, 4, "融合节点 4 个能力，主控 kMaxCaps 必须 ≥4")
        for need in ('"客厅灯"', '"玄关感应"', '"融合节点"'):
            self.assertIn(need, self.ino, "缺少节点名 %s" % need)

        # ledcAttach 必须落在条件编译块里（玄关节点的 GPIO4/5 是 DHT11 与激光）
        idx = self.ino.find("ledcAttach(PIN_RGB_R")
        self.assertGreater(idx, -1, "未找到 RGB 初始化")
        self.assertIn("#if NODE_ID == 1 || NODE_ID == 3", self.ino[max(0, idx - 400):idx],
                      "ledcAttach 必须按 NODE_ID 条件编译，否则会抢走玄关节点的 GPIO4/5")
        # 4 路状态上报 + say/ok 要都放得下
        self.assertRegex(self.ino, r"#define TXQ_SIZE\s+6")

    def test_fusion_node_info_packet_fits_single_packet_limit(self):
        """节点 3 的 info 报文必须 ≤200B（主控 kMaxPacketLen），否则描述被截断。

        直接按固定里真实的能力文案拼报文核长度：文案一拉长就会红，
        避免出现“AI 看不到第四个能力”这种难查的问题。
        """
        specs = dict(re.findall(r'\{"(light|dist|temp|beam)", "([^"]+)"', self.ino))
        self.assertEqual({"light", "dist", "temp", "beam"}, set(specs),
                         "四个能力必须都在节点固件的能力表里")
        names = ("light", "dist", "temp", "beam")
        text = "@n3 info 融合节点 " + ";".join(n + specs[n] for n in names)
        n_bytes = len(text.encode("utf-8"))
        self.assertLessEqual(
            n_bytes, 200,
            "info 报文 %dB 超主控单包上限 200B（会被截断，AI 看不到后面的能力）" % n_bytes)

    def test_motion_auto_off_only_touches_auto_lit_lamp(self):
        """人走超时只关"自动开的"那盏灯（见 spec §2.1 修正 11）。

        若无条件关灯，用户刚用语音开的灯会被 30 秒后偷偷关掉；
        所以用 auto_lit 标记"这盏灯是自动开的"，并在 capLight 里一动手就清标记。
        """
        self.assertRegex(self.ino, r"#define MOTION_AUTO_OFF_MS\s+30000")
        self.assertIn("auto_lit", self.ino)
        self.assertIn("auto_off_due_ms", self.ino)
        idx = self.ino.find("static bool capLight")
        self.assertGreater(idx, -1)
        body = self.ino[idx:idx + 700]
        self.assertIn("auto_lit = false", body, "capLight 必须取消自动关灯排队")
        self.assertIn('strcmp(action, "read")', body, "read 只是查询，不该取消排队")

    def test_node_sensor_blocks_are_independent_and_paired(self):
        """超声波块与 DHT/激光块必须是两个独立 #if，且预处理块必须配对。

        融合节点(3) 两块都要编译；曾把它写成 `#if(1||3) … #elif(2||3) …` ——
        #elif 是互斥的，融合节点因此丢掉 dhtTick/laserTick，报
        "'dht' was not declared in this scope"；而节点 1/2 各自只需要一块，照样能编过，
        所以只有真的编译 NODE_ID=3 才会暴露（已实际踩过）。
        """
        self.assertNotIn("#elif NODE_ID == 2 || NODE_ID == 3", self.ino,
                         "DHT/激光块必须是独立 #if，写成 #elif 会让融合节点缺代码")
        depth = 0
        for line in self.ino.splitlines():
            t = line.strip()
            if t.startswith("//"):
                continue
            if re.match(r"#(if|ifdef|ifndef)\b", t):
                depth += 1
            elif t.startswith("#endif"):
                depth -= 1
                self.assertGreaterEqual(depth, 0, "多余的 #endif")
        self.assertEqual(depth, 0, "#if 与 #endif 不配对（还剩 %d 个未闭合）" % depth)

    def test_high_temp_threshold_moved_to_node(self):
        """温度阈值属于业务语义，必须在节点侧判定。"""
        self.assertRegex(self.ino, r"HOT_TRIGGER_C\s*28")
        self.assertRegex(self.ino, r"HOT_RELEASE_C\s*26")

    def test_no_hardcoded_announce_filename_in_board(self):
        """播报文件名不得在主控里做映射：say <名字> → <名字>.mp3。"""
        for gone in ('"motion.mp3"', '"beam.mp3"', '"hot.mp3"'):
            self.assertNotIn(gone, self.board)


if __name__ == "__main__":
    unittest.main()
