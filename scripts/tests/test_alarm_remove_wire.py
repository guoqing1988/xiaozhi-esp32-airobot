"""测试 web 页面"删除闹钟"的 WS 消息字段约定（纯逻辑，与硬件解耦）。

背景（真机问题：网页删除闹钟无效）
  前端 web/index.html 的 wsSend() 是这样发消息的:

      const id = wsId++;                                          // 请求序号, 1,2,3...
      ws.send(JSON.stringify(Object.assign({}, obj, { id: id })));

  `Object.assign` 后面对象的 `id` 会覆盖调用方传入的 `id`。而删除闹钟原本正是用 `id`
  传闹钟编号:

      wsSend({ action: 'alarm_remove', id: id })

  于是服务端 http_upload_server.cc 的 alarm_remove 分支读到的 `id` 是**请求序号**，
  不是闹钟编号 -> AlarmManager::Remove() 找不到目标:
    - 序号 != 闹钟编号: 返回 {"ok":false}, 前端旧代码又不检查 ok, 列表原样不动
      => 用户看到"删除无效", 且没有任何提示;
    - 序号 == 某个闹钟编号: 误删另一个闹钟(更隐蔽)。

  修复: 业务字段改用不与 wsSend 序号冲突的 `alarm_id`, 服务端优先读它、回退 `id`
  (兼容 HTTP /alarm 的 {"action":"remove","id":N} 与旧页面缓存)。

真机验证仍需烧录后手动测试: 网页删除闹钟应生效, 且删除不存在的编号应给出提示。
"""

import os
import re
import unittest

BOARD_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "main", "boards", "bread-compact-wifi-s3cam-airobot",
)
INDEX_HTML = os.path.join(BOARD_DIR, "web", "index.html")
HTTP_SERVER = os.path.join(BOARD_DIR, "http_upload_server.cc")


def ws_send_payload(obj: dict, ws_id: int) -> dict:
    """复刻 index.html wsSend(): Object.assign({}, obj, {id: wsId++}) —— 后者覆盖前者。"""
    return {**obj, "id": ws_id}


def server_pick_alarm_id(payload: dict) -> int:
    """复刻修复后的 alarm_remove 分支: 优先 alarm_id, 回退 id(兼容 HTTP / 旧页面)。"""
    if "alarm_id" in payload:
        return int(payload["alarm_id"])
    if "id" in payload:
        return int(payload["id"])
    return -1


def server_pick_alarm_id_legacy(payload: dict) -> int:
    """复刻修复前的分支: 只读 id, 仅用于固化回归证据。"""
    return int(payload["id"]) if "id" in payload else -1


class TestAlarmRemoveWireProtocol(unittest.TestCase):
    """前后端字段约定: 闹钟编号必须经 alarm_id 传递, 不能被 wsSend 的序号覆盖。"""

    def test_alarm_id_survives_ws_id_overwrite(self):
        """修复后: 即使 wsSend 覆盖顶层 id, alarm_id 仍原样送达("删除 3 号闹钟"删的就是 3)。"""
        sent = ws_send_payload({"action": "alarm_remove", "alarm_id": 3}, ws_id=7)
        self.assertEqual(sent["id"], 7)                    # 序号覆盖了顶层 id
        self.assertEqual(server_pick_alarm_id(sent), 3)    # 但业务编号未受影响

    def test_legacy_id_field_is_overwritten_by_seq(self):
        """固化根因: 旧协议下服务端拿到的是请求序号, 不是闹钟编号。"""
        sent = ws_send_payload({"action": "alarm_remove", "id": 3}, ws_id=7)
        self.assertEqual(server_pick_alarm_id_legacy(sent), 7)   # 7 != 3 -> 删不掉
        self.assertNotEqual(server_pick_alarm_id_legacy(sent), 3)

    def test_legacy_coincidence_would_delete_wrong_alarm(self):
        """旧协议偶发"成功": 序号恰好等于某个闹钟编号时, 会误删那一个闹钟。"""
        sent = ws_send_payload({"action": "alarm_remove", "id": 3}, ws_id=3)
        self.assertEqual(server_pick_alarm_id_legacy(sent), 3)   # 碰巧命中, 与用户意图无关

    def test_backend_keeps_id_fallback_for_http_clients(self):
        """HTTP /alarm 的 {"action":"remove","id":N} 与旧页面缓存必须继续可用。"""
        self.assertEqual(server_pick_alarm_id({"action": "remove", "id": 5}), 5)

    def test_missing_id_is_rejected(self):
        """两个字段都没有时不能误删(返回 -1, Remove 必然失败)。"""
        self.assertEqual(server_pick_alarm_id({"action": "alarm_remove"}), -1)


class TestAlarmRemoveSourceGuards(unittest.TestCase):
    """源码断言: 前后端字段约定一旦被改回 id, 立即失败(防止回归)。"""

    @classmethod
    def setUpClass(cls):
        with open(INDEX_HTML, encoding="utf-8") as f:
            cls.html = f.read()
        with open(HTTP_SERVER, encoding="utf-8") as f:
            cls.cc = f.read()

    def test_del_alarm_uses_alarm_id(self):
        m = re.search(r"function delAlarm\(id\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m, "未找到 delAlarm 函数")
        body = m.group(1)
        self.assertIn("action: 'alarm_remove', alarm_id: id", body,
                      "delAlarm 必须用 alarm_id 传业务编号")
        self.assertIsNone(re.search(r"action: 'alarm_remove',\s*id:", body),
                          "delAlarm 不能用 id 传闹钟编号(会被 wsSend 的请求序号覆盖)")
        self.assertIn("j.ok", body, "delAlarm 必须检查服务端 ok, 失败要有提示")

    def test_ws_send_still_overwrites_top_level_id(self):
        """根因仍然成立: wsSend 依旧用序号覆盖 id —— 提醒后来者别再用 id 传业务字段。"""
        m = re.search(r"function wsSend\(obj, timeout\)\s*\{(.*?)\n    \}", self.html, re.S)
        self.assertIsNotNone(m, "未找到 wsSend 函数")
        self.assertIn("Object.assign({}, obj, { id: id })", m.group(1))

    def test_backend_prefers_alarm_id_then_falls_back(self):
        idx = self.cc.find('strcmp(action, "alarm_remove") == 0')
        self.assertGreater(idx, 0, "未找到 alarm_remove 分支")
        branch = self.cc[idx:idx + 900]
        # 按实际取值调用的先后顺序断言(不能直接 find 字符串: 注释里的示例如 "id":N 会误命中)
        picked = re.findall(r'cJSON_GetObjectItem\(root, "(alarm_id|id)"\)', branch)
        self.assertEqual(picked[:2], ["alarm_id", "id"],
                         f"alarm_remove 必须优先取 alarm_id 再回退 id, 实际: {picked}")

    def test_backend_remove_branch_has_no_log(self):
        """该分支不得打印日志: 日志与下位机控制指令共用 UART0, 会干扰指令下发。

        失败信息通过 ok=false 与前端提示传达, 不需要串口日志(删除是低频操作,
        真机定位可临时加, 但不得留在代码里)。
        """
        idx = self.cc.find('strcmp(action, "alarm_remove") == 0')
        self.assertGreater(idx, 0, "未找到 alarm_remove 分支")
        branch = self.cc[idx:self.cc.find("\n    }", idx)]
        self.assertNotIn("ESP_LOG", branch, f"alarm_remove 分支不应有日志调用: {branch}")


if __name__ == "__main__":
    unittest.main()
