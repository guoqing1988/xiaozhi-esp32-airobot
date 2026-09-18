"""测试 ESP-NOW 居家节点协议：报文解析、上行去重、事件迟滞，以及两侧源码的约定断言。

对应实现：
  - C++     main/boards/bread-compact-wifi-s3cam-airobot/espnow_home.cc
            的 HandleRecv() / ShouldDeliver()：解析 "@n<id> evt <name> <arg>"，并按
            (node_id, evt) 做 kDedupWindowMs 去重（节点连发 3 次对抗主控待机 DTIM 漏包）。
  - Arduino main/boards/bread-compact-wifi-s3cam-airobot/arduino/EspNowNode/EspNowNode.ino
            的超声波迟滞（<30cm 触发 / >40cm 复位）与事件 3 连发。

修复/固化的两个真机隐患：
  1. 主控待机时 WiFi 为 MAX_MODEM，只在 DTIM 醒来，ESP-NOW 单包上行会被漏掉 ——
     故节点必须连发 EVENT_RESEND 次，主控按 (node_id, evt) 1 秒窗口去重。
  2. 下行若也连发，会在 MCP 工具回调里阻塞约 (N-1)*150ms，卡住对话 ——
     而节点常醒（USB 供电、WiFi.setSleep(false)），故下行必须单次非阻塞发送。

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

MAX_NODES = 4               # 与 EspNowHome::kMaxNodes 一致
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
    if node_id <= 0 or node_id > MAX_NODES:
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

    def test_max_node_ok(self):
        self.assertEqual(parse_node_packet(b"@n4 evt beam 1"), (4, "evt beam 1"))

    def test_reject_beyond_max_node(self):
        # 节点表只有 kMaxNodes 项，超出的必须被拒（否则会写坏节点表的语义）
        self.assertIsNone(parse_node_packet(b"@n5 evt beam 1"))
        self.assertIsNone(parse_node_packet(b"@n12 evt beam 1"))

    def test_reject_bad(self):
        for bad in (b"", b"@", b"@n", b"@n1", b"@n1x", b"@nx evt beam 1",
                    b"n1 evt beam 1", b"@n0 evt beam 1", b"@n1evt"):
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
        self.assertIn("kMaxNodes = %d" % MAX_NODES, self.h)

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
        """播报音频必须独立目录，否则会被 self.music.list 当成歌曲列出。"""
        self.assertIn("/sdcard/announce", read(PLAYER_CC))
        self.assertIn("/sdcard/music", read(PLAYER_CC))

    def test_announce_rejects_path_traversal(self):
        player = read(PLAYER_CC)
        idx = player.find("PlayAnnounce")
        self.assertGreater(idx, -1, "local_music_player.cc 必须有 PlayAnnounce")
        body = player[idx:idx + 900]
        self.assertIn('find("/")', body)
        self.assertIn('find("..")', body)

    def test_announce_only_when_idle(self):
        """播报不得打断对话：必须判 Idle。"""
        idx = self.board.find("void Announce(")
        self.assertGreater(idx, -1, "板级必须有 Announce()")
        body = self.board[idx:idx + 900]
        self.assertIn("kDeviceStateIdle", body)


if __name__ == "__main__":
    unittest.main()
