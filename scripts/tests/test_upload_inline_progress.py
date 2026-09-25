"""上传弹窗的进度/状态显示回归防护（进度条贴在各自文件那一行）。

背景（用户反馈 + 截图）：一次选几十个文件（歌 + 歌词）上传时，进度条和「✅ 成功 xxx」
都在【上传】按钮下面，而正在传的那一条早就滚出视野了 —— 看不出来正在传哪个、传了多少，
而且逐条成功日志把弹窗拉得很长。

改法：进度条与结果都落到**各自文件行内**（谁在传就显示在谁那一行，完成后该条标记完成）；
按钮下方只留一个 #log 放汇总与需要细看的异常。

必须固化的约束：
1. 每个 .fitem 带自己的进度条（id 带 idx），上传/转码进度写回**对应那一行**；
2. 按钮下方不得再有全局进度条（`id="bar"` 已删除）；
3. #log 不得再逐条写「⏳ 上传 / ✅ 成功」（否则弹窗又变长），但失败原因必须保留；
4. 完成/同名跳过/失败三种收尾都要有行内标记；
5. 进度状态存在 item 上（setFileMode 整表重渲染后仍要能照旧显示）；
6. 「跳过不支持的文件」不能被下一句覆盖（旧实现的既存 bug）。
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


class TestUploadInlineProgress(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.html = _read(PAGE)

    def _func(self, name, cls_name=None):
        """取某个 JS 函数体（按缩进 4 的顶格 `\n    }` 收尾）。"""
        m = re.search(r"(?:async )?function " + name + r"\([^)]*\)\s*\{(.*?)\n    \}",
                      self.html, re.S)
        self.assertIsNotNone(m, f"找不到 {name}()")
        return m.group(0)

    # ---- 结构：进度条在行内，按钮下方不再有全局进度条 ----

    def test_no_global_progress_bar_below_button(self):
        """按钮下方的全局进度条必须删掉（用户明确要求挪到每行里）。"""
        self.assertNotIn('id="bar"', self.html, "全局进度条 #bar 应已移除")
        self.assertNotIn("getElementById('bar')", self.html, "不应再引用 #bar")
        self.assertNotIn('class="progress"', self.html, "全局 .progress 结构应已移除")

    def test_each_row_gets_its_own_progress_bar(self):
        """每个文件行都要带自己的进度条，且元素 id 绑定该行的序号。"""
        body = self._func("fprogHtml")
        self.assertIn("id=\"fprog-' + idx + '\"", body,
                      "进度条外层要带 idx（上传时才能只更新这一行）")
        self.assertIn("fbar-' + idx", body, "填充条也要带 idx")
        self.assertIn("hidden", body, "还没轮到该文件时先占位不显示")

    def test_render_file_list_appends_progress_per_row(self):
        """renderFileList 里每行末尾都要拼上该行的进度条，且行本身带 id 便于滚动定位。"""
        body = self._func("renderFileList")
        self.assertIn("fprogHtml(it, idx)", body, "每行都要渲染自己的进度条")
        self.assertIn("id=\"fit-' + idx + '\"", body, "行元素要带 idx（滚动到正在传的那条）")

    # ---- 行为：进度写回对应那一行 ----

    def test_upload_progress_goes_to_matching_row(self):
        """uploadOne 必须接 idx，并把 onprogress 写回该行（而不是某个全局进度条）。"""
        body = self._func("uploadOne")
        self.assertRegex(body, r"function uploadOne\(name, blob, idx\)",
                         "要传入该文件在列表中的下标")
        m = re.search(r"xhr\.upload\.onprogress\s*=\s*function\s*\(e\)\s*\{(.*?)\n        \};",
                      body, re.S)
        self.assertIsNotNone(m, "找不到 onprogress")
        self.assertIn("setItemState(idx, 'upload'", m.group(1),
                      "进度要写到第 idx 条自己的进度条上")

    def test_transcode_progress_also_goes_to_row(self):
        """转码进度同样显示在该行（先「转码 x%」再「上传 y%」）。"""
        body = self._func("up")
        self.assertIn("setItemState(idx, 'transcode'", body)
        self.assertIn("setItemState(idx, 'upload'", body)

    def test_state_persists_on_item(self):
        """进度状态要存在 item 上：setFileMode() 会整表重渲染，重渲染后进度条得照旧。"""
        self.assertIn("state:''|'transcode'|'upload'|'done'|'skip'|'fail'", self.html)
        body = self._func("setItemState")
        self.assertIn("it.state = state", body, "状态要落回 uploadItems[idx]")
        self.assertIn("it.text", body, "行内文字也要落回 item")

    def test_row_scrolled_into_view_when_it_starts(self):
        """文件多时正在传的那条往往不在屏幕上：该行刚开始时要滚到视野内。"""
        body = self._func("setItemState")
        self.assertIn("scrollIntoView", body)
        self.assertIn("block: 'nearest'", body, "用 nearest，避免整页跳动")

    # ---- 收尾标记：完成 / 跳过 / 失败 ----

    def test_finished_states_are_marked_inline(self):
        """三种收尾都要在该行标记：完成 / 同名跳过 / 失败。"""
        body = self._func("up")
        self.assertIn("setItemState(idx, 'done'", body, "完成要标记在该行")
        self.assertIn("同名文件已存在，跳过", body)
        self.assertIn("setItemState(idx, 'fail'", body)
        self.assertIn("✅ 完成", self.html, "完成文字要明确")

    def test_finished_bar_goes_full_and_colored(self):
        """完成后整条走满并换色（蓝→绿/黄/红），一眼能看出这条已经处理完。"""
        body = self._func("setItemState")
        self.assertIn("finished", body)
        for cls in ('.fprog.done>div', '.fprog.skip>div', '.fprog.fail>div'):
            self.assertIn(cls, self.html, f"缺少收尾配色 {cls}")

    # ---- #log 只留汇总/异常 ----

    def test_log_no_longer_logs_every_success(self):
        """#log 不得再逐条写「上传…/成功…」——那正是弹窗被拉长的原因。"""
        body = self._func("up")
        self.assertNotIn("'<div>⏳ 上传 '", body, "逐条上传日志应已移除")
        self.assertNotIn("'<div>✅ 成功 '", body, "逐条成功日志应已移除")
        self.assertIn("全部完成", body, "汇总仍要保留")

    def test_log_keeps_failure_details(self):
        """异常必须仍可读：失败/转码失败要进 #log（行内进度条显示不下原因）。"""
        body = self._func("up")
        self.assertIn("上传失败(", body, "失败要写进 #log 带上服务端返回")
        self.assertIn("网络错误", body)
        self.assertIn("转码失败", body)

    def test_skipped_files_are_not_overwritten(self):
        """跳过不支持的文件：先收集再和「已选 N 个」一起显示（旧实现会被下一句覆盖）。"""
        body = self._func("onFilesPicked")
        self.assertIn("skipped.push(f.name)", body)
        self.assertRegex(body, r"skipped\.length[\s\S]{0,120}跳过不支持的文件",
                         "跳过的文件要在最终提示里列出来")
        self.assertNotIn("log.innerHTML += '<div>⚠️ 跳过不支持的文件", body,
                         "不能再追加（追加后会被下面那句覆盖掉）")

    def test_upload_locks_mode_select_while_running(self):
        """上传中把「自动/强制转码/不转码」置灰：改动会整表重渲染，进度条会跟丢。"""
        body = self._func("renderFileList")
        self.assertRegex(body, r"uploading \? ' disabled' : ''")
        up_body = self._func("up")
        self.assertIn("renderFileList()", up_body, "开始/结束时都要重渲染以更新可选状态")


if __name__ == "__main__":
    unittest.main()
