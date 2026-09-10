"""测试 web 页面「左转/右转」按钮的手感参数（纯逻辑/源码断言，与硬件解耦）。

背景（真机问题：点一下左转/右转，转弯幅度偏大）
  转弯幅度 ≈ 角速度 × 转动时长，两个旋钮都在 web/index.html：

      const TURN_SPEED = 110;          // PWM 70-255, 决定角速度
      const TURN_MIN_PRESS_MS = 250;   // 「点一下」的最短转动时长

  旧值 400ms @ 140：用户快速点一下（可能只按了 100ms），前端仍会强制转到满 400ms
  才发 drive-stop，因此"点一下"转得比预期多。现调整为 250ms @ 110。

  这两个常量集中在「机器人控制」区块顶部, 便于微调——因此必须断言函数体里不能
  再写死数值(否则改了常量却没生效, 排查成本很高)。

真机手感仍需烧录后实际试车确认(见板级 README「Web 控制页」)。
"""

import os
import re
import unittest

INDEX_HTML = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "main", "boards", "bread-compact-wifi-s3cam-airobot", "web", "index.html",
)

EXPECTED_TURN_SPEED = 110
EXPECTED_TURN_MIN_PRESS_MS = 250

# Arduino 侧 handleDrive() 会把速度钳到 70-255；低于 70 会被抬回 70，麦轮还可能转不动。
SPEED_MIN, SPEED_MAX = 70, 255


class TestTurnButtonParams(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(INDEX_HTML, encoding="utf-8") as f:
            cls.html = f.read()

    def _const_int(self, name: str) -> int:
        m = re.search(rf"\b{name}\s*=\s*(\d+)\s*;", self.html)
        self.assertIsNotNone(m, f"未找到常量 {name}（应定义在「机器人控制」区块顶部）")
        return int(m.group(1))

    def _bind_turn_btn_body(self) -> str:
        m = re.search(r"function bindTurnBtn\(id, dir\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m, "未找到 bindTurnBtn 函数")
        return m.group(1)

    def test_params_are_named_constants(self):
        """两个手感参数必须显式定义, 便于一处微调。"""
        self.assertEqual(self._const_int("TURN_SPEED"), EXPECTED_TURN_SPEED)
        self.assertEqual(self._const_int("TURN_MIN_PRESS_MS"), EXPECTED_TURN_MIN_PRESS_MS)

    def test_turn_speed_within_arduino_clamp_range(self):
        """TURN_SPEED 必须在 Arduino 的 70-255 钳位区间内, 否则实际速度与预期不符。"""
        self.assertTrue(SPEED_MIN <= EXPECTED_TURN_SPEED <= SPEED_MAX)

    def test_min_press_is_short_enough_for_a_click(self):
        """点一下的时长要够短(幅度诉求), 但也不能小到"起步就被刹停"看不出动过。"""
        self.assertTrue(100 <= EXPECTED_TURN_MIN_PRESS_MS <= 300,
                        "点一下的时长应在 100-300ms 之间(过小会像没动, 过大则点一下转太多)")

    def test_bind_turn_btn_uses_constants_not_literals(self):
        """函数体里不能写死旧的 140/400 —— 否则改常量不生效。"""
        body = self._bind_turn_btn_body()
        self.assertIn("unoSetDrive(dir, TURN_SPEED)", body)
        self.assertIn("held < TURN_MIN_PRESS_MS", body)
        self.assertIn("TURN_MIN_PRESS_MS - held", body)
        self.assertNotRegex(body, r"unoSetDrive\(dir,\s*\d+\)", "不应写死转速字面量")
        self.assertNotRegex(body, r"var\s+MIN_PRESS_MS\s*=", "不应保留局部 MIN_PRESS_MS 覆盖常量")

    def test_both_turn_buttons_are_bound(self):
        """两个按钮都必须绑定, 否则某个方向点了没反应。"""
        self.assertIn("bindTurnBtn('btnTurnLeft', 'left')", self.html)
        self.assertIn("bindTurnBtn('btnTurnRight', 'right')", self.html)


if __name__ == "__main__":
    unittest.main()
