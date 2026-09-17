"""AI 拍照上传（Explain）用的 HTTP 客户端必须**常驻复用**，不得随每次拍照析构。

背景（2026-09 真机崩溃，backtrace 全部落在上游组件里）：

    assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))
    → pthread_mutex_lock_internal → std::mutex::lock()
    → HttpClient::OnTcpDisconnected()              (http_client.cc:281)
    → HttpClient::Open(...)::{lambda()#1}          (http_client.cc:213)
    → EspTcp::DoDisconnect → EspTcp::ReceiveTask   (esp_tcp.cc:103 / 141)

    `pthread_mutex_destroy()` 会把 mutex 的 sem 置空，断言 `pxQueue` 为空即说明
    **mutex 已经被析构**；而调用它的却是 EspTcp 的 `tcp_receive` 任务 —— 对象析构后回调才到。

   链路：本请求带 `Connection: close`，服务器响应完就主动关连接 →
     tcp_receive 任务：recv 返 0 → DoDisconnect(false) → 回调 OnTcpDisconnected()
     主任务：ReadAll() 返回 → Close()（connected_ 已 false → 直接 return，**不等接收任务**）
             → Explain 返回 → unique_ptr<Http> 析构 → **mutex_ 被销毁**
   窗口只有微秒级，所以表现为偶发；任何打乱时序的操作（如先网页拍一张）都会改变
   命中概率 —— 那是躲开子弹，不是治病。

必须固化的约束：
1. Explain 不得每次 `network->CreateHttp()` 新建（那会重新引入析构竞态）；
2. 常驻实例必须只创建一次（判空），否则每轮都覆盖，等于没常驻；
3. **Close() 不能省**：常驻的是对象，不是连接。不关连接占满 LWIP socket 池会把小智的
   UDP 音频通道挤掉（见踩坑 9 与 sdkconfig.defaults 里 LWIP_MAX_SOCKETS=16 的注释）；
4. 超时仍要每次设（15s，默认 30s 失败会卡住设备流程）；
5. 注释必须说明这是对上游缺陷的**规避**，避免后人"顺手优化"回每次新建。
"""

import os
import re
import unittest

CAMERA_CC = ("main", "boards", "common", "esp32_camera.cc")


def _read(*parts):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", *parts)
    with open(path, encoding="utf-8") as f:
        return f.read()


class TestExplainHttpClientLifetime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = _read(*CAMERA_CC)

    def _explain_body(self):
        m = re.search(r"std::string Esp32Camera::Explain\(.*?\n\}", self.cc, re.S)
        self.assertIsNotNone(m, "未找到 Explain")
        return m.group(0)

    def test_http_client_is_not_recreated_per_call(self):
        """每次拍照新建 HttpClient 会让 mutex 又变成"用完即销毁"。"""
        body = self._explain_body()
        self.assertNotIn(
            "auto http = network->CreateHttp", body,
            "Explain 不得每次新建 HttpClient（会重新引入析构竞态：mutex 先于接收任务消失）")

    def test_http_client_is_cached(self):
        body = self._explain_body()
        self.assertIn("static std::unique_ptr<Http> explain_http;", body,
                      "必须用常驻实例承载 Explain 的 HTTP 客户端")
        self.assertIn("auto& http = explain_http;", body)
        # 只创建一次：没有判空就会每轮覆盖，等于没常驻
        self.assertIn("if (explain_http == nullptr)", body)
        self.assertEqual(body.count("CreateHttp"), 1,
                         "CreateHttp 只应在首次创建时出现一次")

    def test_still_closes_connection(self):
        """常驻的是对象不是连接：不 Close 会占满 socket 池并挤掉小智音频通道。"""
        body = self._explain_body()
        self.assertIn("http->Close()", body)
        self.assertIn("SetTimeout(15000)", body)

    def test_documents_that_it_is_a_workaround(self):
        """注释必须写明这是规避上游缺陷，否则后人很容易改回去。"""
        body = self._explain_body()
        self.assertIn("常驻", body)
        self.assertIn("规避", body, "必须写明这是对上游组件缺陷的规避而非根治")
        self.assertIn("OnTcpDisconnected", body, "必须留下崩溃现场的关键符号，便于回溯")


if __name__ == "__main__":
    unittest.main()
