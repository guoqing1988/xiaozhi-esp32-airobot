"""测试 Arduino 下位机指令防抖的"续期"语义与动作刹车（纯逻辑，与硬件解耦）。

对应实现：
  - C++  main/boards/bread-compact-wifi-s3cam-airobot/compact_wifi_board_s3cam_airobot.cc
        的 SendUartMessage()：相同指令在窗口内只放行一次，且命中时刷新时间戳（续期）。
  - Arduino  main/boards/bread-compact-wifi-s3cam-airobot/arduino/MecanumRobot/MecanumRobot.ino
        的 handleCommand()：@go-*/@tj-* 执行完必须显式 stopMove(0)。

修复的两个真机问题：
  1. "一直摇头 / 一直前进停不下来"：旧防抖命中时不刷新时间戳，窗口变成"距上次实际发送的
     时间"，于是 AI 每 0.9s 重复调用时每 2 次就放行 1 次（约每 1.8s 下发一条），
     特技/动作被反复触发。
  2. "AI 让前进后停不下来"：旧实现靠 loop 中 handleGamepad() 在无手柄按键时每轮
     stopMove(10) 的副作用停车；ps2_ready 跳过手柄轮询后该副作用消失。

真机验证仍需烧录后手动测试（见板级 README 验证要点）。
"""

import os
import re
import unittest

# 服务端 AI 实测重试间隔约 700-900ms（见板级 README 踩坑记录 2），取 900ms 作为最坏情况。
AI_RETRY_INTERVAL_US = 900_000

ACTION_WINDOW_US = 3_000_000   # 动作类（go-*/tj-*）窗口
STATUS_WINDOW_US = 1_000_000   # 状态类（servo-*/speed-*）窗口


def make_debouncer() -> dict:
    """复刻 SendUartMessage 里静态变量 s_last_cmd / s_last_us 的状态。"""
    return {"last_cmd": None, "last_us": 0}


def allow(db: dict, cmd: str, now_us: int) -> bool:
    """复刻修复后的防抖判定。返回 True=放行（下发串口），False=防抖拦截。"""
    is_action = cmd.startswith("go-") or cmd.startswith("tj-")
    window = ACTION_WINDOW_US if is_action else STATUS_WINDOW_US
    if db["last_cmd"] == cmd and (now_us - db["last_us"]) < window:
        db["last_us"] = now_us   # 命中时续期：持续重复调用则持续拦截
        return False
    db["last_cmd"] = cmd
    db["last_us"] = now_us
    return True


def allow_legacy(db: dict, cmd: str, now_us: int) -> bool:
    """复刻修复前的旧逻辑（命中时不刷新时间戳），仅用于固化回归证据。"""
    if db["last_cmd"] == cmd and (now_us - db["last_us"]) < STATUS_WINDOW_US:
        return False             # 注意：旧实现不更新 last_us
    db["last_cmd"] = cmd
    db["last_us"] = now_us
    return True


class TestDebounceRenewal(unittest.TestCase):
    """防抖续期语义：AI 持续重复调用同一指令时，只能放行第一次。"""

    def test_first_call_passes(self):
        db = make_debouncer()
        self.assertTrue(allow(db, "go-forward-10", 0))

    def test_repeat_within_window_blocked(self):
        db = make_debouncer()
        self.assertTrue(allow(db, "go-forward-10", 0))
        self.assertFalse(allow(db, "go-forward-10", 500_000))   # 0.5s 后重复 -> 拦截

    def test_sustained_repeat_only_passes_once(self):
        """核心回归：AI 每 0.9s 重复调用 20 次，只允许下发第 1 条。

        否则特技（tj-yaotou 约 3.5s）会被反复触发，表现为"一直摇头"。
        """
        db = make_debouncer()
        passed = [i for i in range(20) if allow(db, "tj-yaotou", i * AI_RETRY_INTERVAL_US)]
        self.assertEqual(passed, [0], f"应只放行首次调用，实际放行于 {passed}")

    def test_sustained_repeat_forward_only_passes_once(self):
        """同一回归对动作类 go-* 同样成立（对应"一直前进停不下来"）。"""
        db = make_debouncer()
        passed = [i for i in range(20) if allow(db, "go-forward-10", i * AI_RETRY_INTERVAL_US)]
        self.assertEqual(passed, [0])

    def test_legacy_logic_would_repeat(self):
        """固化回归证据：旧逻辑下 20 次调用会放行 10 次（约每 1.8s 一条）。

        若日后有人把续期改回"命中不刷新"，本用例会失败，提示会重新出现"一直摇"。
        """
        db = make_debouncer()
        passed = [i for i in range(20) if allow_legacy(db, "tj-yaotou", i * AI_RETRY_INTERVAL_US)]
        self.assertEqual(len(passed), 10)
        self.assertEqual(passed[:4], [0, 2, 4, 6])

    def test_passes_again_after_window(self):
        """AI 停止重复调用、超过窗口后，应能再次触发（用户说"再摇一次头"要生效）。"""
        db = make_debouncer()
        self.assertTrue(allow(db, "tj-yaotou", 0))
        self.assertFalse(allow(db, "tj-yaotou", 900_000))
        # 最后一次调用后静默超过 3 秒 -> 放行
        self.assertTrue(allow(db, "tj-yaotou", 900_000 + ACTION_WINDOW_US + 1))

    def test_different_command_not_blocked(self):
        """不同指令（动作编排）不受影响，例如 forward -> left -> forward。"""
        db = make_debouncer()
        self.assertTrue(allow(db, "go-forward-10", 0))
        self.assertTrue(allow(db, "go-left-5", 200_000))
        self.assertTrue(allow(db, "go-forward-10", 400_000))   # last_cmd 已变为 go-left-5

    def test_action_window_longer_than_status_window(self):
        """动作类窗口 3s > 状态类 1s：2 秒后重复舵机命令应放行，重复动作命令应拦截。"""
        db = make_debouncer()
        self.assertTrue(allow(db, "servo-100", 0))
        self.assertTrue(allow(db, "servo-100", 2_000_000))     # 状态类 1s 窗口已过
        db2 = make_debouncer()
        self.assertTrue(allow(db2, "go-forward-10", 0))
        self.assertFalse(allow(db2, "go-forward-10", 2_000_000))  # 动作类 3s 窗口内

    def test_status_command_blocked_within_one_second(self):
        db = make_debouncer()
        self.assertTrue(allow(db, "servo-home", 0))
        self.assertFalse(allow(db, "servo-home", 800_000))


class TestArduinoExplicitStop(unittest.TestCase):
    """源码断言：@go-*/@tj-* 分支必须显式刹车，不能依赖 handleGamepad 的隐式 stopMove。

    这是防止回归的静态检查：一旦有人删掉 stopMove(0)，AI 动作执行完电机就会一直转。
    """

    @classmethod
    def setUpClass(cls):
        ino = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "..",
            "main", "boards", "bread-compact-wifi-s3cam-airobot",
            "arduino", "MecanumRobot", "MecanumRobot.ino",
        )
        with open(ino, encoding="utf-8") as f:
            cls.src = f.read()

    def _assert_stop_before(self, marker: str, label: str):
        idx = self.src.find(marker)
        self.assertGreater(idx, 0, f"未找到 {label} 的完成回执: {marker}")
        # 回执前 400 字符内必须出现显式刹车
        window = self.src[max(0, idx - 400):idx]
        self.assertIn("stopMove(0);", window, f"{label} 执行完缺少显式 stopMove(0)")

    def test_go_branch_stops_motors(self):
        self._assert_stop_before(
            'Serial.print("@done "); Serial.println(cmd_full);', "go-* 动作"
        )

    def test_tj_branch_stops_motors(self):
        self._assert_stop_before(
            'Serial.print("@done "); Serial.println(p);', "tj-* 特技"
        )

    def test_ps2_ready_guard_exists(self):
        """ps2_ready 守卫必须存在，否则无手柄时 read_gamepad 会阻塞 loop（延时根因）。"""
        self.assertRegex(self.src, r"if \(!ps2_ready\) return;")

    def test_stop_not_inside_move_helpers(self):
        """stopMove 不能加进 moveForward 等运动函数内部：巡线/手柄依赖"设置方向后不刹车"。"""
        for fn in ("moveForward", "moveBackward", "turnLeft", "turnRight",
                   "moveLeft", "moveRight"):
            m = re.search(rf"void {fn}\(int t\) \{{(.*?)\n\}}", self.src, re.S)
            self.assertIsNotNone(m, f"未找到函数 {fn}")
            self.assertNotIn("stopMove", m.group(1), f"{fn} 内部不应调用 stopMove")


BOARD_SRC = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "main", "boards", "bread-compact-wifi-s3cam-airobot",
    "compact_wifi_board_s3cam_airobot.cc",
)


class TestAiRetryLoopFix(unittest.TestCase):
    """源码断言：防抖命中必须回"已完成"语义，否则 AI 会陷入重试死循环。

    真机复现（日志实证）：AI 先 action(左转) 再 action(停止)，返回
    "动作约500毫秒后自动停止"让 AI 以为动作尚未完成，于是每 0.7~1 秒重复调用一次；
    防抖命中后返回的 "指令已发送(防抖): xxx" 又被 AI 读成"没发出去"，而续期逻辑让窗口
    永不失效 -> 工具调用死循环刷 40+ 次、对话卡在 speaking 达 30 秒才退出。
    """

    @classmethod
    def setUpClass(cls):
        with open(BOARD_SRC, encoding="utf-8") as f:
            cls.src = f.read()

    def test_debounce_returns_completed_semantics(self):
        """命中防抖不能再回"防抖/失败"语义 —— 那是 AI 重试的直接诱因。"""
        # 先剥掉注释：修复说明的注释里故意保留了旧文案作为反面例子
        code = re.sub(r"//[^\n]*", "", self.src)
        self.assertNotIn("指令已发送(防抖)", code, "不能再用‘防抖’字样回复 AI")
        self.assertIn("本次重复调用已忽略", self.src)
        self.assertIn("毫秒前执行完成", self.src)

    def test_debounce_reports_time_since_real_send(self):
        """ago_ms 必须基于"上次真正发出"的时间，且在续期之前算，否则恒为 0。"""
        self.assertIn("s_last_sent_us", self.src, "需要单独记录真正发出的时间")
        self.assertGreater(
            self.src.index("s_last_sent_us = now;"),
            self.src.index("uart_write_bytes"),
            "s_last_sent_us 只能在真正写出 UART 之后更新",
        )
        m = re.search(r"命中时刷新时间戳.*?s_last_us = now;", self.src, re.S)
        self.assertIsNotNone(m, "未找到防抖命中分支")
        self.assertIn("ago_ms", m.group(0), "ago_ms 要在 s_last_us = now 之前算出来")

    def test_stop_forces_zero_steps(self):
        """停止发 go-stop-0：下位机 stopMove 立即刹车，后面的 delay(t) 纯属白等。

        同时确认 stop 切到独立分支，不再回"动作约500毫秒后自动停止"
        （那句等于告诉 AI 动作还没完成，是重试循环的诱因之一）。
        """
        self.assertRegex(self.src, r"if \(is_stop\) \{\s*steps = 0;")
        self.assertIn("机器人已停止，无需再次调用确认", self.src)
        stop_branch = re.search(
            r"if \(is_stop\) \{.*?result \+= \"，机器人已停止，无需再次调用确认\";\s*\} else \{",
            self.src,
            re.S,
        )
        self.assertIsNotNone(stop_branch, "stop 必须走独立分支，不能在 else 里算 t_ms")

    def test_turn_has_minimum_steps(self):
        """左/右转每步仅 10ms，AI 常用的默认 10 步只有 100ms，几乎看不出转动。"""
        self.assertRegex(self.src, r"if \(is_turn && steps < 25\) \{\s*steps = 25;")

    def test_result_tells_ai_not_to_repeat(self):
        """返回值要明确请 AI 别再来确认，这是打断重试循环的最后一道提示。"""
        self.assertIn("无需再次调用确认", self.src)


if __name__ == "__main__":
    unittest.main()
