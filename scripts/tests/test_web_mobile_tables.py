"""静态断言：闹钟新建走弹窗、列表在移动端变成卡片。

对应实现：main/boards/bread-compact-wifi-s3cam-airobot/web/index.html
  - 闹钟表单从面板内联改成 #alarmModal 弹窗（与上传歌曲共用 .modal 样式）
  - 屏宽 ≤620px 时 .card-table 从表格变成卡片列表，列名取自各 <td> 的 data-label

这两件事都很容易被后续改动悄悄破坏：表单挪回面板、新加的 <td> 忘带 data-label
（手机上那一列就没有名字）、或漏了 data-label 属性导致卡片化后信息缺失。
"""

import os
import re
import unittest

PAGE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "main", "boards", "bread-compact-wifi-s3cam-airobot", "web", "index.html",
)


class _PageBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(PAGE, encoding="utf-8") as f:
            cls.html = f.read()


class TestAlarmModal(_PageBase):
    """「添加闹钟」必须是弹窗，面板上只留触发按钮。"""

    def test_modal_sits_between_alarm_and_uno_panels(self):
        idx_alarm = self.html.index('id="panel-alarm"')
        idx_modal = self.html.index('id="alarmModal"')
        idx_uno = self.html.index('id="panel-uno"')
        self.assertLess(idx_alarm, idx_modal, "闹钟弹窗应在闹钟面板之后")
        self.assertLess(idx_modal, idx_uno, "闹钟弹窗应在机器人面板之前")

    def test_form_controls_are_inside_modal(self):
        idx_modal = self.html.index('id="alarmModal"')
        for el in ('id="atype"', 'id="avalue"', 'id="alabel"', 'id="asong"'):
            self.assertEqual(self.html.count(el), 1, f"{el} 应只出现一次（不能面板和弹窗各留一份）")
            self.assertGreater(self.html.index(el), idx_modal, f"{el} 应在弹窗内部")

    def test_panel_only_has_trigger_buttons(self):
        panel = self.html[self.html.index('id="panel-alarm"'):self.html.index('id="alarmModal"')]
        self.assertIn('onclick="openAlarm()"', panel, "面板上要有「新建闹钟」按钮")
        for el in ('id="atype"', 'id="avalue"', 'id="alabel"', 'id="asong"'):
            self.assertNotIn(el, panel, f"{el} 不该还留在面板里")

    def test_modal_handlers_exist(self):
        self.assertRegex(self.html, r"function openAlarm\(\)\s*\{")
        self.assertRegex(self.html, r"function closeAlarm\(\)\s*\{")

    def test_add_closes_modal_before_toasting(self):
        """弹窗盖着列表，必须先关弹窗，否则「已添加」提示被遮住看不见。"""
        m = re.search(r"function addAlarm\(\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m, "未找到 addAlarm 定义")
        body = m.group(1)
        self.assertIn("closeAlarm();", body, "addAlarm 应先关闭弹窗")
        self.assertLess(body.index("closeAlarm();"),
                        body.index("wsSend({ action: 'alarm_add'"),
                        "关闭弹窗要在发出添加请求之前（否则提示时弹窗还盖着）")

    def test_open_clears_stale_input(self):
        """打开弹窗要清空上次填的内容，避免误提交旧值。"""
        m = re.search(r"function openAlarm\(\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m, "未找到 openAlarm 定义")
        body = m.group(1)
        self.assertIn("getElementById('avalue').value = ''", body)
        self.assertIn("getElementById('alabel').value = ''", body)

    def test_song_options_loaded_when_opening(self):
        """铃声下拉应在打开弹窗时刷新，保证能选到刚上传的歌。"""
        m = re.search(r"function openAlarm\(\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIn("loadSongOptions()", m.group(1))
        m2 = re.search(r"function loadAlarms\(\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertNotIn("loadSongOptions()", m2.group(1),
                         "loadAlarms 不该再重复拉铃声列表")

    def test_relative_value_is_validated(self):
        """相对时间要挡住 NaN，否则提交 NaN 后端解析失败而页面无提示。"""
        m = re.search(r"function addAlarm\(\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIn("isFinite(sec)", m.group(1))


class TestMobileCardTables(_PageBase):
    """屏宽 ≤620px 时表格要变成卡片列表。"""

    def test_both_tables_opt_in(self):
        self.assertEqual(self.html.count('class="card-table"'), 2,
                         "歌曲表与闹钟表都要加 card-table（漏一个则该表手机上仍要横向滚）")

    def test_media_query_and_layout(self):
        self.assertIn("@media (max-width: 620px)", self.html)
        # 表头隐藏、行变卡片
        self.assertRegex(self.html, r"\.card-table thead\s*\{\s*display: none")
        self.assertRegex(self.html, r"\.card-table tr\s*\{[^}]*border-radius")
        # 列名由属性提供
        self.assertIn("content: attr(data-label)", self.html)

    def test_song_cells_carry_data_label(self):
        for lbl in ("歌曲名", "大小", "上传时间", "操作"):
            self.assertIn(f"setAttribute('data-label', '{lbl}')", self.html,
                          f"歌曲表的「{lbl}」列缺少 data-label")

    def test_alarm_cells_carry_data_label(self):
        for lbl in ("ID", "类型", "触发", "内容", "铃声", "状态", "操作"):
            self.assertIn(f'data-label="{lbl}"', self.html,
                          f"闹钟表的「{lbl}」列缺少 data-label")

    def test_action_cell_spans_full_row(self):
        """删除按钮在手机上独占一行、右对齐，手指好点。"""
        self.assertIn('.card-table td[data-label="操作"]', self.html)

    def test_placeholder_rows_have_no_label(self):
        """占位行（colspan 的“暂无歌曲”）不带 data-label，::before 才不会凭空显示列名。"""
        for colspan in ('colspan="4"', 'colspan="7"'):
            self.assertIn(colspan, self.html)
        m = re.search(r'const empty = \'<tr><td colspan="4">([^<]*)</td>', self.html)
        self.assertIsNotNone(m, "未找到歌曲空列表占位行")
        self.assertNotIn("data-label", m.group(0))


class TestNoLongPressCopy(_PageBase):
    """手机长按不该弹出「复制/全选」菜单。

    iOS 看 -webkit-touch-callout（私有属性，Android 无效），Android 只能靠 user-select
    + selectstart 拦截；图方便改成只设 body 会在部分国产内核上失效。
    """

    def test_selection_killed_with_important(self):
        """必须写在 * 上且带 !important。

        实测：靠 body 继承 + 普通权重的 user-select: none，在部分 Android 浏览器上
        长按文案/按钮依旧弹「复制」，所以改成通配符 + !important 直接压过 UA 样式。
        """
        m = re.search(r"\n    \* \{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m, "未找到 * 通配规则")
        block = m.group(1)
        self.assertIn("user-select: none !important", block)
        self.assertIn("-webkit-touch-callout: none !important", block)
        self.assertIn("-webkit-user-select: none !important", block)
        self.assertIn("-webkit-user-drag: none", block)

    def test_selectstart_is_blocked(self):
        """Android 部分内核长按不派发 contextmenu，只认 selectstart。"""
        m = re.search(r"addEventListener\('selectstart'.*?\n    \}\);", self.html, re.S)
        self.assertIsNotNone(m, "未找到 selectstart 拦截")
        self.assertIn("contextmenu", self.html, "原来的 contextmenu 拦截应保留")

    def test_form_controls_stay_copyable(self):
        """表单控件要能选/粘贴；这里的 !important 是必要的，否则被 * 上那条压掉。"""
        m = re.search(r"\n    input,\s*\n    select,\s*\n    textarea\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m, "未找到表单控件规则")
        self.assertIn("user-select: text !important", m.group(1))
        m2 = re.search(r"addEventListener\('selectstart'.*?\n    \}\);", self.html, re.S)
        body = m2.group(0)
        self.assertIn("'INPUT'", body)
        self.assertIn("'TEXTAREA'", body)

    def test_log_copy_uses_button(self):
        """日志不再靠长按复制（全局已禁），改用「📋 复制」按钮，功能不倒退。"""
        self.assertIn("function copyPre(", self.html)
        self.assertIn("copyPre('logView', this)", self.html)
        self.assertIn("copyPre('unoCmdView', this)", self.html)
        # 本页是 http:// — 非安全上下文下 navigator.clipboard 不可用，只能走 execCommand
        self.assertIn("execCommand('copy')", self.html)


if __name__ == "__main__":
    unittest.main()
