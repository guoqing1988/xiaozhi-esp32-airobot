"""测试 web 页面「头部舵机」控制（跑页面真实 JS 与 HTML 绑定，非源码断言）。

三个真机问题：
  1. 滑块不好用 —— 手机拖 range "拖到哪算哪"(190px 对应 180°, 1px≈1°), 手指还挡住角度值,
     没法微调。保留滑块做粗调, 两边加「− / +」微调(每次 SERVO_STEP 度)。
     两者共用同一角度状态: 滑块/加减号/状态推送/回正都必须同步。
  2. 回正值输入框点不动 —— 原本带 readonly。去掉后必须处理手动输入: 空值/非数字要回退,
     否则 `Number('') === 0` 会把"清空输入框"当成 0°(舵机直接转端点); 另外原 `homeStep()`
     用 `+el.value || 82` 兜底, 回正值为 0 时点 + 会跳到 83 而不是 1。
  3. 滑块拖动不能每条 oninput 都下发 —— 拖动时事件可达数十 Hz, 会灌爆设备 httpd
     (与小智 UDP 音频抢 socket)。因此 oninput 只更新显示, 松手(onchange)才下发。
  4. 没连设备时点「+」会跳成 5° 并卡住 —— 发送失败时把角度基准回滚成了 null,
     而下一次 `servoDeg + 5` 就是 `null + 5 = 5`(真机上 WS 断线重连的 1 秒窗口同样会中招)。
     角度基准不能参与这种回滚: 失败只提示, 不回滚。

测试方式（与 test_web_mp3_probe.py 同款）：提取 index.html 里「头部舵机控制」的真实代码,
在 node + vm 里配 DOM/wsSend 桩执行, 直接调用真实函数与 HTML 里的 oninput/onchange 表达式,
断言"发出去的 WS 消息"与"界面显示"。这些坑正则断言发现不了: 空值→0°、`|| 82` 吞掉 0°、
拖动中狂发、状态推送不回填, 都必须跑起来才知道。

真机仍需验证一次: 拖滑块松手后舵机转动、点 − / + 微调、回正值可直接输入。
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INDEX_HTML = os.path.join(
    HERE, "..", "..", "main", "boards", "bread-compact-wifi-s3cam-airobot", "web", "index.html",
)
NODE = shutil.which("node")

BEGIN_MARK = "// ===== 头部舵机控制 BEGIN"
END_MARK = "// ===== 头部舵机控制 END"


def extract_servo_js() -> str:
    """提取页面里「头部舵机控制」区块的真实 JS，外加负责回填角度的 renderUnoStatus。"""
    with open(INDEX_HTML, encoding="utf-8") as f:
        html = f.read()
    code = html[html.index(BEGIN_MARK):html.index(END_MARK) + len(END_MARK)]
    m = re.search(r"function renderUnoStatus\(j\)\s*\{.*?\n    \}", html, re.S)
    assert m, "未找到 renderUnoStatus 函数"
    return code + "\n" + m.group(0)


# node 驱动: DOM/wsSend 全是桩, 断言发出去的消息与界面显示
DRIVER = r"""
import fs from 'node:fs';
import vm from 'node:vm';

const els = new Map();
function mkEl(id) {
  if (!els.has(id)) els.set(id, { id, value: '', textContent: '' });
  return els.get(id);
}
const sent = [];
let wsFail = false;   // 模拟"没连设备/WS 未就绪": wsSend 全部失败
const ctx = {
  document: { getElementById: mkEl },
  wsSend: (msg) => {
    sent.push(msg);
    return wsFail ? Promise.reject(new Error('ws not ready')) : Promise.resolve({ ok: true });
  },
  logWs: () => {},
};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(process.argv[2], 'utf8'), ctx);

// 滑块的 oninput/onchange 直接从 HTML 里取来跑(而不是在测试里重写一遍绑定),
// 否则把 oninput 误写成直接下发也测不出来。
const html = fs.readFileSync(process.argv[3], 'utf8');
const rangeTag = html.match(/<input[^>]*id="servoRange"[^>]*>/)[0];
function runAttr(name, el) {
  const expr = rangeTag.match(new RegExp(name + '="([^"]*)"'))[1];
  ctx.__el = el;
  vm.runInContext(expr.replace(/\bthis\b/g, '__el'), ctx);
}

const fails = [];
const checks = [];
function check(name, fn) { checks.push({ name: name, fn: fn }); }
const tick = () => new Promise((r) => setTimeout(r, 0));   // 等 then/catch 回调跑完
function eq(got, want, msg) {
  if (got !== want) throw new Error((msg ? msg + ' ' : '') + 'got=' + JSON.stringify(got) + ' want=' + JSON.stringify(want));
}
let mark = 0;
function reset() { mark = sent.length; }
function last() { return sent[sent.length - 1]; }
function sentCount() { return sent.length - mark; }
function label() { return mkEl('servoLabel').textContent; }

check('角度钳位: 边界/越界/空值', () => {
  eq(ctx.clampServoDeg(0), 0);
  eq(ctx.clampServoDeg(180), 180);
  eq(ctx.clampServoDeg(-5), 0);
  eq(ctx.clampServoDeg(999), 180);
  eq(ctx.clampServoDeg(82.6), 83);
  eq(ctx.clampServoDeg(''), null, '空值必须判为非法');
  eq(ctx.clampServoDeg('abc'), null, '非数字必须判为非法');
});

check('头部角度: 值变了才下发, 越界钳位, 同步滑块', () => {
  const range = mkEl('servoRange');
  reset();
  ctx.setServoDeg(90, true);
  eq(sentCount(), 1, '首次应下发');
  eq(last().action, 'uno_servo');
  eq(last().degree, 90);
  eq(label(), '90°');
  eq(range.value, 90, '滑块应同步');

  ctx.setServoDeg(90, true);
  eq(sentCount(), 1, '值未变不应重发');

  ctx.setServoDeg(200, true);
  eq(last().degree, 180, '越界应钳到 180');
  eq(range.value, 180);
  ctx.setServoDeg(200, true);
  eq(sentCount(), 2, '已在边界不应重发');

  ctx.setServoDeg(50, false);
  eq(sentCount(), 2, 'send=false 不应下发');
  eq(label(), '50°', 'send=false 仍要更新显示');
  eq(range.value, 50, 'send=false 也要同步滑块');

  ctx.setServoDeg('', true);
  eq(sentCount(), 2, '非法值不应下发');
  eq(label(), '50°', '非法值不应改显示');
});

check('滑块: 拖动中只显示, 松手才下发 (跑 HTML 里的真实绑定)', () => {
  const range = mkEl('servoRange');
  ctx.setServoDeg(90, false);
  reset();
  range.value = '140';
  runAttr('oninput', range);
  eq(sentCount(), 0, '拖动中不应下发(每条都发会灌爆设备 httpd)');
  eq(label(), '140°', '拖动中应实时显示');

  runAttr('onchange', range);
  eq(sentCount(), 1, '松手应下发');
  eq(last().degree, 140);
  eq(range.value, 140);
});

check('加减按钮: 步进并带动滑块 (直接跑 HTML onclick 表达式)', () => {
  const range = mkEl('servoRange');
  ctx.setServoDeg(90, false);
  eq(range.value, 90);

  reset();
  vm.runInContext('servoStep(SERVO_STEP)', ctx);    // 与 HTML onclick 完全一致
  eq(sentCount(), 1);
  eq(last().degree, 95);
  eq(range.value, 95, '点 + 后滑块应跟到 95');
  eq(label(), '95°');

  reset();
  vm.runInContext('servoStep(-SERVO_STEP)', ctx);
  eq(last().degree, 90);
  eq(range.value, 90);

  reset();
  ctx.setServoDeg(178, false);
  vm.runInContext('servoStep(SERVO_STEP)', ctx);     // 178+5 应钳到 180
  eq(last().degree, 180);
  eq(range.value, 180);
});

check('没连设备(wsSend 全失败)时连续点 + : 角度持续累加, 不跳变', async () => {
  const range = mkEl('servoRange');
  const logs = [];
  const origLog = ctx.logWs;
  ctx.logWs = (t) => logs.push(t);
  wsFail = true;
  ctx.setServoDeg(82, false);
  reset();

  for (const want of ['87°', '92°', '97°']) {
    vm.runInContext('servoStep(SERVO_STEP)', ctx);
    await tick();
    eq(label(), want, '连续点 + 应持续累加');
  }
  eq(range.value, 97, '滑块应跟到 97');
  eq(logs.length, 3, '每次失败都要提示, 不能静默丢弃');

  ctx.logWs = origLog;
  wsFail = false;
});

check('没连设备时拖滑块松手: 显示不回跳', async () => {
  const range = mkEl('servoRange');
  wsFail = true;
  ctx.setServoDeg(82, false);
  reset();
  range.value = '140';
  runAttr('oninput', range);
  eq(label(), '140°', '拖动中应实时显示');

  runAttr('onchange', range);
  await tick();
  eq(label(), '140°', '发送失败不应把显示回滚');
  eq(range.value, 140, '滑块不应回跳');
  wsFail = false;
});

check('回正值手动输入: 生效/非法回退/不重发', () => {
  const home = mkEl('servoHomeVal');
  reset();
  home.value = '100';
  ctx.onServoHomeInput(home);
  eq(sentCount(), 1, '合法输入应下发');
  eq(last().action, 'uno_servo_home_set');
  eq(last().value, 100);
  eq(home.value, 100);

  home.value = '';
  ctx.onServoHomeInput(home);
  eq(sentCount(), 1, '空值不应下发');
  eq(home.value, 100, '空值应回退, 不能变成 0°');

  home.value = 'abc';
  ctx.onServoHomeInput(home);
  eq(sentCount(), 1, '非数字不应下发');
  eq(home.value, 100, '非数字应回退');

  home.value = '100';
  ctx.onServoHomeInput(home);
  eq(sentCount(), 1, '值未变不应重发');

  home.value = '999';
  ctx.onServoHomeInput(home);
  eq(last().value, 180, '越界应钳位');
  eq(home.value, 180, '输入框应回显钳位后的值');
});

check('回归: 回正值为 0 时 0+1=1 (不是 83)', () => {
  const home = mkEl('servoHomeVal');
  reset();
  ctx.applyServoHome(0);
  eq(last().value, 0, '0 是合法回正值');
  eq(home.value, 0);

  reset();
  ctx.homeStep(1);
  eq(last().value, 1, '0 点 + 应为 1');
  eq(home.value, 1);
});

check('「回正」使用输入框当前值', () => {
  mkEl('servoHomeVal').value = '150';
  reset();
  ctx.servoHome();
  eq(sentCount(), 1);
  eq(last().action, 'uno_servo_home');
  eq(label(), '150°', '回正后显示应同步');
  eq(mkEl('servoRange').value, 150, '回正后滑块应同步');
});

check('状态推送回填设备真实角度', () => {
  reset();
  ctx.renderUnoStatus({ mode: 'idle', action: '', speed: null, servo: 120 });
  eq(label(), '120°', '应显示设备上报的角度');
  eq(mkEl('servoRange').value, 120, '滑块应跟到设备角度');
  eq(sentCount(), 0, '回填不应反向下发指令');

  ctx.renderUnoStatus({ mode: 'idle', action: '', speed: null, servo: null });
  eq(label(), '120°', '设备未上报(null)时应保留本地值');
});

for (const c of checks) {
  try { await c.fn(); } catch (e) { fails.push(c.name + ': ' + e.message); }
}
console.log(JSON.stringify({ failures: fails }));
"""


@unittest.skipUnless(NODE, "需要 node 才能跑 web 真实代码测试")
class TestServoStep(unittest.TestCase):
    def test_real_servo_code_behaviour(self):
        """在 node 里跑页面真实舵机代码, 断言发出去的 WS 消息与界面显示。"""
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "servo.js")
            with open(src, "w", encoding="utf-8", newline="\n") as f:
                f.write(extract_servo_js())
            drv = os.path.join(d, "driver.mjs")
            with open(drv, "w", encoding="utf-8", newline="\n") as f:
                f.write(DRIVER)
            r = subprocess.run([NODE, drv, src, INDEX_HTML],
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(r.returncode, 0, f"node 执行失败: {r.stderr}")
            out = json.loads(r.stdout.strip().splitlines()[-1])
        self.assertEqual(out["failures"], [], "舵机行为断言失败:\n" + "\n".join(out["failures"]))


if __name__ == "__main__":
    unittest.main()
