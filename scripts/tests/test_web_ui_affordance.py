"""网页交互可见性回归防护：灰控件的悬停反馈 + 页首右上角全屏按钮。

背景（用户反馈）：
  1. 「灰色的按钮鼠标放上去没有状态」—— 未选中的 tab 与 .btn-quiet 这类浅灰控件，
     要么没写 :hover，要么只差一点点浅色（还缺 cursor:pointer），看着像死板子。
  2. 页面缺一个全屏入口，要求放在**顶部右上角**（手机进全屏后地址栏收起，视频/摇杆更宽敞）。

必须固化的约束：
1. 所有浅灰可点控件都有 :hover（且变化看得出来）+ cursor:pointer；
2. 深色浮层按钮（视频框 ⚙️、相册 ×）的 background 必须写在 CSS 类里 ——
   否则内联样式优先级更高，会把手写 :hover 盖掉；
3. 全屏按钮放在 .wsbar（页首那行）里靠 margin-left:auto 推到最右，不能塞进 .tabs
   （.tabs 窄屏横向滚动，手机上要滑到最右才看得到，就不叫“右上角”了）；
4. 全屏要带 webkit 前缀兜底、按 Esc 退出后按钮文字要跟着变、不支持/被拒时给提示，不静默失败。
"""

import os
import re
import unittest

PAGE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "main", "boards", "bread-compact-wifi-s3cam-airobot", "web", "index.html",
)


def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class TestWebUiAffordance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.html = _read(PAGE)

    def _rule(self, selector):
        """取某条 CSS 规则的内容（选择器必须精确到 `{` 之前，不含伪类后缀）。"""
        m = re.search(re.escape(selector) + r"\s*\{(.*?)\}", self.html, re.S)
        self.assertIsNotNone(m, f"找不到 CSS 规则 {selector}")
        return m.group(1)

    def _body(self, name):
        m = re.search(r"(?:async )?function " + name + r"\([^)]*\)\s*\{(.*?)\n    \}",
                      self.html, re.S)
        self.assertIsNotNone(m, f"找不到 {name}()")
        return m.group(0)

    # ---- 灰控件的悬停反馈 ----

    def test_inactive_tab_has_hover(self):
        """未选中的灰标签鼠标悬停要有变化（以前完全没有）。"""
        body = self._rule(".tab:not(.active):hover")
        self.assertIn("background", body)
        self.assertIn("color", body)

    def test_quiet_button_has_pointer_and_visible_hover(self):
        """浅灰次要按钮：要有手型，且悬停不能只差一点点底色。"""
        base = self._rule(".btn-quiet")
        self.assertIn("cursor: pointer", base, "缺手型 → 鼠标放上去像块死板子")
        hover = self._rule(".btn-quiet:hover")
        self.assertIn("background", hover)
        self.assertIn("border-color", hover, "只改底色太淡，边框也要跟着变")
        self.assertIn("color", hover)
        self.assertIn(".btn-quiet:active", self.html, "按下也要有反馈")

    def test_primary_buttons_have_pressed_state(self):
        """.btn / .btn-danger 也要有 :active（只有 hover 时按下没反应）。"""
        self.assertIn(".btn:active", self.html)
        self.assertIn(".btn-danger:active", self.html)

    def test_dark_overlay_buttons_have_hover(self):
        """相册 × 与视频框 ⚙️ 这两个深色浮层按钮也要有悬停反馈。"""
        self.assertIn("background", self._rule(".photo-del:hover"))
        self.assertIn("background", self._rule(".video-gear:hover"))

    def test_overlay_button_style_not_inline(self):
        """⚙️ 的 background 必须在 CSS 类里：内联样式优先级更高，会盖掉 :hover。"""
        body = self._func_video_gear()
        self.assertNotIn("background:rgba", body.replace(" ", ""),
                         "内联 background 会把 .video-gear:hover 覆盖掉")
        self.assertIn('class="video-gear"', body)
        self.assertIn("background", self._rule(".video-gear"))

    def _func_video_gear(self):
        m = re.search(r"<button class=\"video-gear\".*?</button>", self.html, re.S)
        self.assertIsNotNone(m, "找不到视频框 ⚙️ 按钮")
        return m.group(0)

    # ---- 全屏按钮 ----

    def test_fullscreen_button_in_top_bar_right(self):
        """按钮在页首 .wsbar 里，靠 margin-left:auto 顶到最右（= 页面右上角）。"""
        m = re.search(r'<div class="wsbar">(.*?)</div>', self.html, re.S)
        self.assertIsNotNone(m, "找不到 .wsbar")
        bar = m.group(1)
        self.assertIn('id="fsBtn"', bar, "全屏按钮要在页首那行里")
        self.assertIn("toggleFullscreen()", bar)
        self.assertRegex(bar, r"margin-left:\s*auto", "要靠 auto 外边距推到右上角")
        # 不能放进 .tabs：窄屏 .tabs 会横向滚动，右上角就看不到了
        tabs = re.search(r'<div class="tabs">(.*?)</div>', self.html, re.S).group(1)
        self.assertNotIn('id="fsBtn"', tabs, ".tabs 窄屏横向滚动，别把全屏塞进去")

    def test_fullscreen_uses_webkit_fallback(self):
        """老 Safari / 部分国产浏览器只认 webkit 前缀，两条路都要走。"""
        body = self._body("toggleFullscreen")
        self.assertIn("requestFullscreen", body)
        self.assertIn("webkitRequestFullscreen", body)
        self.assertIn("exitFullscreen", body)
        self.assertIn("webkitExitFullscreen", body)

    def test_fullscreen_button_label_syncs(self):
        """按 Esc / F11 退出全屏时，按钮文字也要跟着变回「全屏」。"""
        self.assertIn("syncFsBtn", self._body("syncFsBtn"))
        self.assertIn("fullscreenchange", self.html)
        self.assertIn("webkitfullscreenchange", self.html)
        self.assertIn("退出全屏", self._body("syncFsBtn"))

    def test_fullscreen_failures_are_reported(self):
        """不支持（iPhone Safari）或被浏览器拒时给一行提示，不能静默失败。"""
        self.assertIn("fsSupported", self._body("toggleFullscreen"))
        self.assertIn("logWs(", self._body("toggleFullscreen"), "要复用一次性提示通道")
        self.assertIn("catch", self._body("toggleFullscreen"), "全屏被拒要给提示")


if __name__ == "__main__":
    unittest.main()
