"""测试 web 页面的前端转码链路（真实 lamejs + 真实 PCM + ffprobe 验证产物）。

被测对象是 web/index.html 里的真实代码：
  - 「PCM 编码」区块：encodePcmToMp3()（纯函数，把 Float32 PCM 编成 96kbps MP3）
  - 「MP3 参数探测」区块：本测试用它反查产物，验证"转码后的文件会被判定为无需再转码"

测试内容：
  1. CDN 源可达 + 页面里写死的 SRI 哈希与 CDN 实际内容一致（写错哈希会导致脚本永远
     加载失败，必须用真实内容校验）；
  2. ffmpeg 生成 24kHz 单声道 PCM -> 页面真实编码函数 -> ffprobe 验证输出确实
     24000Hz/单声道/96kbps，且时长与输入一致；
  3. 转码产物用页面自己的探测器复查 -> 必须判定为"无需转码"（端到端闭环）；
  4. 空输入/极短输入不抛异常（边界）。

需要网络（下载 lamejs）与 ffmpeg/ffprobe/node；任一缺失则跳过。
"""

import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
INDEX_HTML = os.path.join(
    HERE, "..", "..", "main", "boards", "bread-compact-wifi-s3cam-airobot", "web", "index.html",
)
FFMPEG = shutil.which("ffmpeg")
FFPROBE = shutil.which("ffprobe")
NODE = shutil.which("node")

PROBE_BEGIN, PROBE_END = "// ===== MP3 参数探测 BEGIN", "// ===== MP3 参数探测 END"
ENC_BEGIN, ENC_END = "// ===== PCM 编码 BEGIN", "// ===== PCM 编码 END"
LRC_BEGIN, LRC_END = "// ===== 歌词转码 BEGIN", "// ===== 歌词转码 END"

# 与页面保持一致的目标参数
EXPECT_RATE, EXPECT_CH, EXPECT_KBPS = 24000, 1, 96


def extract_block(begin: str, end: str) -> str:
    with open(INDEX_HTML, encoding="utf-8") as f:
        html = f.read()
    b = html.index(begin)
    e = html.index(end)
    return html[b:e + len(end)]


def extract_sources() -> list:
    """从页面提取 CDN 源列表与 SRI 常量（保证测的就是页面里写的那一份）。"""
    with open(INDEX_HTML, encoding="utf-8") as f:
        html = f.read()
    sri = re.search(r"const LAMEJS_SHA384 = '([^']+)'", html).group(1)
    version = re.search(r"const LAMEJS_VERSION = '([^']+)'", html).group(1)
    urls = re.findall(r"\{ url: '([^']*' \+ LAMEJS_VERSION \+ '[^']*)'", html)
    assert urls, "未能在 index.html 中解析出 CDN 源列表"
    return version, sri, [u.replace("' + LAMEJS_VERSION + '", version) for u in urls]


def fetch(url: str, timeout: int = 30) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "xiaozhi-host-test"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


@unittest.skipUnless(NODE, "需要 node")
class TestWebPageStaticChecks(unittest.TestCase):
    """页面整体静态检查：UI 代码不在可提取的纯函数块里, 靠这两道检查兜底。"""

    def page_html(self) -> str:
        with open(INDEX_HTML, encoding="utf-8") as f:
            return f.read()

    def test_page_script_has_no_syntax_error(self):
        """整段 <script> 必须能通过 node 语法检查（括号/逗号等编辑事故）。"""
        scripts = re.findall(r"<script>(.*?)</script>", self.page_html(), re.S)
        self.assertTrue(scripts, "未找到 <script> 块")
        path = os.path.join(tempfile.gettempdir(), "xiaozhi_page_check.js")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(max(scripts, key=len))
        r = subprocess.run([NODE, "--check", path], capture_output=True,
                           text=True, encoding="utf-8", errors="replace")
        self.assertEqual(r.returncode, 0, f"页面 JS 语法错误:\n{r.stderr}")

    def test_referenced_element_ids_exist(self):
        """代码里 getElementById('x') 引用的 id 必须在页面 HTML 中存在（防漏加元素/拼写错）。"""
        html = self.page_html()
        ids = set(re.findall(r"id=\"([A-Za-z0-9_-]+)\"", html))
        refs = set(re.findall(r"getElementById\(['\"]([A-Za-z0-9_-]+)['\"]\)", html))
        # 已知例外(既有问题，非本次引入)：wslog —— logWs() 内部做了 if (el) 判空所以无副作用，
        # 但板级 README 提到“页面顶部 #wslog 会提示发送失败”，因元素不存在实际不会显示。
        # 登记在此以免掩盖新引入的缺失引用；待后续决定是补元素还是改文档。
        known_missing = {"wslog"}
        missing = sorted(refs - ids - known_missing)
        self.assertFalse(missing, f"以下元素在页面 HTML 中不存在: {missing}")

    def test_utf8_lrc_upload_path_skips_reencode(self):
        """已是 UTF-8 的歌词必须走"原样上传"分支（blob 保持原 File），只有 GBK/BOM 才调 lrcToUtf8。

        回归防护：此前 up() 对任何 .lrc 都无条件转一遍，页面也恒显示"歌词 → 转 UTF-8"，
        用户以为已转好的歌词又被改写了。
        """
        html = self.page_html()
        self.assertIn("if (it.lrcEnc === 'utf-8')", html, "up() 缺少已是 UTF-8 的分支")
        self.assertIn("（已是 UTF-8，原样上传）", html, "缺少原样上传的提示文案")
        self.assertIn("lrcEnc = await lrcProbe(f)", html, "选文件时必须探测歌词编码")
        for enc in ("'utf-8'", "'utf-8-bom'", "'gbk'"):
            self.assertIn(enc + ":", html, f"LRC_MARK 缺少 {enc} 的处理方式提示")


@unittest.skipUnless(FFMPEG and FFPROBE and NODE, "需要 ffmpeg/ffprobe/node")
class TestWebTranscode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.version, cls.sri, cls.sources = extract_sources()
        cls.tmp = tempfile.TemporaryDirectory()
        d = cls.tmp.name
        cls.lame_js = os.path.join(d, "lame.min.js")
        cls.download_ok = True
        try:
            data = fetch(cls.sources[0])
            with open(cls.lame_js, "wb") as f:
                f.write(data)
        except Exception as e:      # 无网络/被墙: 相关用例跳过
            cls.download_ok = False
            cls.download_err = str(e)

        # PCM 编码驱动: 页面真实代码 + 真实 lamejs + 真实 PCM
        cls.driver = os.path.join(d, "encode.mjs")
        with open(cls.driver, "w", encoding="utf-8", newline="\n") as f:
            f.write("import fs from 'node:fs';\n")
            f.write("import vm from 'node:vm';\n")
            f.write(extract_block(PROBE_BEGIN, PROBE_END))       # 提供 TRANSCODE_RATE / TRANSCODE_KBPS
            f.write("\n")
            f.write(extract_block(ENC_BEGIN, ENC_END))           # 提供 encodePcmToMp3
            f.write(
                # lame.min.js 没有 module.exports: 它靠顶层 `function lamejs(){...}` 声明 + 立即调用
                # 把 Mp3Encoder/WavHeader 挂在函数对象上, 因此**浏览器 <script> 加载后是 window.lamejs**。
                # 这里用 vm.runInThisContext 在全局上下文执行, 精确模拟该行为;
                # 顺便断言页面 loadLamejs() 之后 `window.lamejs.Mp3Encoder` 确实可用。
                "\nvm.runInThisContext(fs.readFileSync(process.argv[2], 'utf8'));\n"
                "const lamejs = globalThis.lamejs;\n"
                "if (!lamejs || typeof lamejs.Mp3Encoder !== 'function') {\n"
                "  throw new Error('lamejs 未按浏览器方式暴露 Mp3Encoder: ' + typeof lamejs);\n"
                "}\n"
                "const raw = fs.readFileSync(process.argv[3]);\n"
                "const s16 = new Int16Array(raw.buffer, raw.byteOffset, Math.floor(raw.byteLength / 2));\n"
                "const pcm = new Float32Array(s16.length);\n"
                "for (let i = 0; i < s16.length; i++) pcm[i] = s16[i] / 32768;\n"
                "const blob = await encodePcmToMp3(lamejs, pcm, null);\n"
                "fs.writeFileSync(process.argv[4], Buffer.from(await blob.arrayBuffer()));\n"
                "console.log(JSON.stringify({ bytes: blob.size, samples: pcm.length }));\n"
            )

        # 歌词转 UTF-8 驱动(同时输出探测器结论, 供断言"已是 UTF-8 不转码")
        cls.lrc_driver = os.path.join(d, "lrc.mjs")
        with open(cls.lrc_driver, "w", encoding="utf-8", newline="\n") as f:
            f.write("import fs from 'node:fs';\n")
            f.write(extract_block(LRC_BEGIN, LRC_END))
            f.write(
                "\nconst file = new File([fs.readFileSync(process.argv[2])], 'x.lrc');\n"
                "const enc = await lrcProbe(file);\n"
                "const blob = await lrcToUtf8(file);\n"
                "fs.writeFileSync(process.argv[3], Buffer.from(await blob.arrayBuffer()));\n"
                "console.log(JSON.stringify({ enc: enc }));\n"
            )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def require_net(self):
        if not self.download_ok:
            self.skipTest(f"无法下载 lamejs（需要网络）: {self.download_err}")

    # ---------- 1. CDN 与 SRI ----------

    def test_cdn_sources_reachable(self):
        """三个 CDN 源都应可达（页面按顺序回退，全挂则转码不可用）。"""
        for url in self.sources:
            with self.subTest(url=url):
                try:
                    data = fetch(url)
                except Exception as e:
                    self.skipTest(f"网络不可用，跳过可达性检查: {e}")
                self.assertGreater(len(data), 50000, "内容过小，可能不是 lamejs")

    def test_page_sri_matches_cdn_content(self):
        """页面里写死的 SRI 必须与 CDN 实际内容一致，否则脚本会被浏览器拒绝加载。"""
        self.require_net()
        for url in self.sources:
            if url.endswith("lame.min.js") and "npmmirror" in url:
                continue    # 该源不返回 CORS 头，页面里本就没配 SRI
            with self.subTest(url=url):
                digest = base64.b64encode(hashlib.sha384(fetch(url)).digest()).decode()
                self.assertEqual("sha384-" + digest, self.sri, f"{url} 的内容与页面 SRI 不一致")

    # ---------- 2/3. 真实编码链路 ----------

    def make_pcm(self, seconds: int, name: str) -> str:
        path = os.path.join(self.tmp.name, name)
        r = subprocess.run(
            [FFMPEG, "-y", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
             "-i", f"sine=frequency=440:duration={seconds}", "-ac", "1", "-ar", str(EXPECT_RATE),
             "-f", "s16le", path],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        return path

    def ffprobe_a(self, path: str):
        r = subprocess.run(
            [FFPROBE, "-v", "error", "-select_streams", "a:0",
             "-show_entries", "stream=sample_rate,channels,bit_rate:format=duration",
             "-of", "json", path],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        j = json.loads(r.stdout)
        st = j["streams"][0]
        return int(st["sample_rate"]), int(st["channels"]), int(st.get("bit_rate") or 0), float(j["format"]["duration"])

    def probe_output(self, mp3: str) -> dict:
        """用页面自己的探测器复查产物（与浏览器同一代码路径）。"""
        drv = os.path.join(self.tmp.name, "probe.mjs")
        with open(drv, "w", encoding="utf-8", newline="\n") as f:
            f.write("import fs from 'node:fs';\n")
            f.write(extract_block(PROBE_BEGIN, PROBE_END))
            f.write(
                "\nconst buf = fs.readFileSync(process.argv[2]);\n"
                "const info = await mp3ProbeFile(new File([buf], 'x.mp3'));\n"
                "console.log(JSON.stringify({ info, need: mp3NeedsTranscode(info) }));\n"
            )
        r = subprocess.run([NODE, drv, mp3], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout.strip().splitlines()[-1])

    def encode(self, pcm_path: str, out_name: str) -> str:
        out = os.path.join(self.tmp.name, out_name)
        r = subprocess.run([NODE, self.driver, self.lame_js, pcm_path, out],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        self.assertEqual(r.returncode, 0, f"编码失败: {r.stderr}")
        self.assertTrue(os.path.exists(out) and os.path.getsize(out) > 0)
        return out

    def test_encoded_mp3_has_device_friendly_parameters(self):
        """编码产物必须是 24000Hz / 单声道 / ~96kbps，且时长与输入一致。"""
        self.require_net()
        pcm = self.make_pcm(4, "in4.pcm")
        mp3 = self.encode(pcm, "out4.mp3")
        rate, ch, kbps, duration = self.ffprobe_a(mp3)
        self.assertEqual(rate, EXPECT_RATE)
        self.assertEqual(ch, EXPECT_CH)
        self.assertAlmostEqual(kbps, EXPECT_KBPS * 1000, delta=2000, msg=f"码率异常: {kbps}")
        self.assertAlmostEqual(duration, 4.0, delta=0.25, msg=f"时长异常: {duration}")

    def test_encoded_mp3_passes_own_probe(self):
        """端到端闭环：转码产物用页面探测器复查，必须判定为"无需转码"。"""
        self.require_net()
        pcm = self.make_pcm(4, "in5.pcm")
        mp3 = self.encode(pcm, "out5.mp3")
        got = self.probe_output(mp3)
        info = got["info"]
        self.assertIsNotNone(info, f"产物无法被解析器识别: {got}")
        self.assertEqual(info["sampleRate"], EXPECT_RATE)
        self.assertEqual(info["channels"], EXPECT_CH)
        self.assertEqual(info["bitrateKbps"], EXPECT_KBPS)
        self.assertFalse(got["need"], "转码产物不应再被判定为需要转码（否则会反复转码）")

    # ---------- 4. 歌词转 UTF-8 ----------

    def lrc_roundtrip(self, src_bytes: bytes, name: str):
        """跑页面真实 lrcProbe() + lrcToUtf8()，返回 (探测到的编码, 转换后字节)。"""
        src = os.path.join(self.tmp.name, name)
        out = os.path.join(self.tmp.name, name + ".out")
        with open(src, "wb") as f:
            f.write(src_bytes)
        r = subprocess.run([NODE, self.lrc_driver, src, out], capture_output=True,
                           text=True, encoding="utf-8", errors="replace")
        self.assertEqual(r.returncode, 0, f"歌词转换失败: {r.stderr}")
        enc = json.loads(r.stdout.strip().splitlines()[-1])["enc"]
        with open(out, "rb") as f:
            return enc, f.read()

    def test_gbk_lrc_is_converted_to_utf8(self):
        """GBK 歌词必须转成 UTF-8（设备端没有 GBK 转换能力，否则屏幕显示乱码）。

        国内下载的 .lrc 大量是 GBK，实测《三拜红尘凉.lrc》就是 GBK。
        """
        text = "[00:00.15]三拜红尘凉 - 黄龄\n[00:02.88]民乐录制：星舟爱乐乐团\n"
        enc, out = self.lrc_roundtrip(text.encode("gbk"), "gbk.lrc")
        self.assertEqual(enc, "gbk", "GBK 歌词必须被探测为 gbk（需转码）")
        self.assertEqual(out.decode("utf-8"), text, "必须是合法 UTF-8 且内容一致")

    def test_utf8_lrc_kept_unchanged(self):
        text = "[00:00.15]已经 UTF-8 的歌词\n[00:01.00]abc 123\n"
        enc, out = self.lrc_roundtrip(text.encode("utf-8"), "utf8.lrc")
        self.assertEqual(enc, "utf-8", "已是 UTF-8 的歌词应被探测为 utf-8，页面不再做无谓转码")
        self.assertEqual(out, text.encode("utf-8"), "已是 UTF-8 必须逐字节原样输出")

    def test_utf8_bom_is_stripped(self):
        """带 BOM 的歌词要去掉 BOM，否则设备端首行会多一个不可见字符（可能影响首行时间标签）。"""
        enc, out = self.lrc_roundtrip(b"\xef\xbb\xbf[00:00.15]BOM \xe6\xad\x8c\xe8\xaf\x8d\n", "bom.lrc")
        self.assertEqual(enc, "utf-8-bom", "带 BOM 应由探测器单独标出（只有 BOM 需要处理）")
        self.assertFalse(out.startswith(b"\xef\xbb\xbf"), "输出不应保留 BOM")
        self.assertTrue(out.decode("utf-8").startswith("[00:00.15]"))

    def test_ascii_lrc_unchanged(self):
        enc, out = self.lrc_roundtrip(b"[00:00.15]plain ascii only\n", "ascii.lrc")
        self.assertEqual(enc, "utf-8")
        self.assertEqual(out, b"[00:00.15]plain ascii only\n")

    def test_mixed_encoding_probe_is_conservative(self):
        """混合编码/UTF-16 等非法 UTF-8 一律探测为 gbk（保守转码），与 py 脚本"无法识别就不转"不同:
        设备端整篇不显示比转出乱码更难排查，故这里必须先能转出来。"""
        mixed = "[00:00.15]歌词甲\n".encode("utf-8") + "丙".encode("gbk")
        enc, _ = self.lrc_roundtrip(mixed, "mixed.lrc")
        self.assertEqual(enc, "gbk")
        utf16 = "\ufeff[00:00.15]歌词甲\n".encode("utf-16-le")
        enc16, _ = self.lrc_roundtrip(utf16, "utf16.lrc")
        self.assertEqual(enc16, "gbk")

    def test_empty_and_tiny_input_do_not_crash(self):
        """空/极短输入不应抛异常（用户可能拖入损坏或极短文件）。"""
        self.require_net()
        for name, seconds in (("tiny", 1),):
            pcm = self.make_pcm(seconds, f"{name}.pcm")
            mp3 = self.encode(pcm, f"{name}.mp3")
            self.assertGreater(os.path.getsize(mp3), 0)
        empty = os.path.join(self.tmp.name, "empty.pcm")
        open(empty, "wb").close()
        mp3 = self.encode(empty, "empty.mp3")
        # 空输入产不出音频帧，但必须正常返回（不抛异常）
        self.assertGreaterEqual(os.path.getsize(mp3), 0)


if __name__ == "__main__":
    unittest.main()
