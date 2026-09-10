"""测试 web 页面的 MP3 参数探测（真实 MP3 样本 + 页面真实 JS 代码，非源码断言）。

背景
  网页上传前要判断"这首是否需要转码"（本来合格的就别重编码，避免二次音损与耗时）。

  但 WebAudio 无法回答这个问题：decodeAudioData() 会把音频统一重采样到
  AudioContext.sampleRate，返回的 AudioBuffer.sampleRate 是"上下文的采样率"而非
  文件原始采样率（用 new AudioContext({sampleRate:24000}) 更是把原始值抹掉）。

  因此 web/index.html 里的 mp3ProbeBytes()/mp3ProbeFile() 直接解析 MPEG(Layer3)
  帧头，并先跳过 ID3v2（封面图可能几百 KB）。本测试：
    1. 用 ffmpeg 生成各种采样率/声道/码率/VBR/带封面的**真实 MP3**；
    2. 从 index.html 提取「MP3 参数探测」区块的真实代码；
    3. 用 node 构造 File 对象跑 mp3ProbeFile() —— 与浏览器完全同一条代码路径。

  真机网页仍需人工验证一次（选文件后列表显示是否正确、转码后能否播放）。
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
FFMPEG = shutil.which("ffmpeg")
FFPROBE = shutil.which("ffprobe")
NODE = shutil.which("node")

BEGIN_MARK = "// ===== MP3 参数探测 BEGIN"
END_MARK = "// ===== MP3 参数探测 END"

# 样本表: (文件名, ffmpeg 参数, 期望采样率, 期望声道, 期望码率kbps, 期望是否需要转码)
# 判定规则: 单声道 且 <=24000Hz 且 <=128kbps 才跳过; VBR/解析失败一律转码。
SAMPLES = [
    ("stereo_44k_320.mp3", ["-ac", "2", "-ar", "44100", "-b:a", "320k"], 44100, 2, 320, True),
    ("mono_44k_96.mp3", ["-ac", "1", "-ar", "44100", "-b:a", "96k"], 44100, 1, 96, True),
    ("mono_48k_192.mp3", ["-ac", "1", "-ar", "48000", "-b:a", "192k"], 48000, 1, 192, True),
    ("mono_24k_96.mp3", ["-ac", "1", "-ar", "24000", "-b:a", "96k"], 24000, 1, 96, False),
    ("mono_22k_64.mp3", ["-ac", "1", "-ar", "22050", "-b:a", "64k"], 22050, 1, 64, False),
    ("mono_16k_32.mp3", ["-ac", "1", "-ar", "16000", "-b:a", "32k"], 16000, 1, 32, False),
]


def extract_probe_js() -> str:
    """提取页面里「MP3 参数探测」区块的 JS（标记之间的真实代码）。"""
    with open(INDEX_HTML, encoding="utf-8") as f:
        html = f.read()
    b = html.index(BEGIN_MARK)
    e = html.index(END_MARK)
    return html[b:e + len(END_MARK)]


def run_ffmpeg(args: list) -> None:
    r = subprocess.run(
        [FFMPEG, "-y", "-hide_banner", "-loglevel", "error"] + args,
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if r.returncode != 0:
        raise RuntimeError(f"ffmpeg 失败: {r.stderr}")


@unittest.skipUnless(FFMPEG and FFPROBE and NODE, "需要 ffmpeg/ffprobe/node 才能跑真实 MP3 探测测试")
class TestMp3Probe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = cls.tmp.name
        cls.dir = d

        # 1) 生成测试样本(3 秒正弦波, 足够产生多个完整帧)
        src = ["-f", "lavfi", "-i", "sine=frequency=440:duration=3"]
        cls.samples = {}
        for name, extra, *_ in SAMPLES:
            path = os.path.join(d, name)
            run_ffmpeg(src + extra + ["-c:a", "libmp3lame", path])
            cls.samples[name] = path

        # VBR(带 Xing 头): 码率不定 -> 保守转码
        cls.vbr = os.path.join(d, "vbr_mono_24k.mp3")
        run_ffmpeg(src + ["-ac", "1", "-ar", "24000", "-q:a", "4", "-c:a", "libmp3lame", cls.vbr])

        # 带 ID3v2 封面的文件: 验证能跳过 ID3 找到真正的音频帧
        cover = os.path.join(d, "cover.jpg")
        run_ffmpeg(["-f", "lavfi", "-i", "color=c=red:s=600x600:d=1", "-frames:v", "1", cover])
        cls.with_cover = os.path.join(d, "cover_stereo_44k_320.mp3")
        run_ffmpeg([
            "-i", cls.samples["stereo_44k_320.mp3"], "-i", cover,
            "-map", "0:a", "-map", "1:v", "-c:v", "copy",
            "-c:a", "copy",   # 音频必须原样拷贝, 否则 ffmpeg 会重编码(码率就不是 320k 了)
            "-id3v2_version", "3", "-metadata:s:v", "title=Album cover",
            "-metadata:s:v", "comment=Cover (front)", cls.with_cover,
        ])

        # 非 MP3 内容: 必须判定为无法识别(-> 保守转码)
        cls.not_mp3 = os.path.join(d, "not_mp3.bin")
        with open(cls.not_mp3, "wb") as f:
            f.write(b"this is definitely not an mp3 file, just plain text.\n" * 40)

        # 2) 生成 node 驱动: 页面真实代码 + File 对象(与浏览器同一路径)
        cls.driver = os.path.join(d, "probe.mjs")
        with open(cls.driver, "w", encoding="utf-8", newline="\n") as f:
            f.write("import fs from 'node:fs';\n")
            f.write(extract_probe_js())
            f.write(
                "\nconst buf = fs.readFileSync(process.argv[2]);\n"
                "const info = await mp3ProbeFile(new File([buf], 'x.mp3'));\n"
                "console.log(JSON.stringify({ info, need: mp3NeedsTranscode(info), text: mp3InfoText(info) }));\n"
            )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def probe(self, path: str) -> dict:
        r = subprocess.run([NODE, self.driver, path],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        self.assertEqual(r.returncode, 0, f"node 执行失败: {r.stderr}")
        return json.loads(r.stdout.strip().splitlines()[-1])

    def ffprobe_a(self, path: str):
        """用 ffprobe 读真实参数（独立实现，作为交叉验证的权威参考）。"""
        r = subprocess.run(
            [FFPROBE, "-v", "error", "-select_streams", "a:0",
             "-show_entries", "stream=sample_rate,channels,bit_rate", "-of", "json", path],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        st = json.loads(r.stdout)["streams"][0]
        return int(st["sample_rate"]), int(st["channels"]), int(st.get("bit_rate") or 0) // 1000

    def test_matches_ffprobe(self):
        """交叉验证：页面解析器的结果必须与 ffprobe 一致。

        （写这条是因为手写期望值容易出错：LAME 的 Xing/Info 信息帧码率字段不可信，
        实测 32kbps 文件里写成 40kbps，只有与独立实现对比才能发现这类错误。）
        """
        for name, *_ in SAMPLES:
            with self.subTest(sample=name):
                info = self.probe(self.samples[name])["info"]
                rate, ch, kbps = self.ffprobe_a(self.samples[name])
                self.assertIsNotNone(info)
                self.assertEqual(info["sampleRate"], rate, f"{name} 采样率与 ffprobe 不符")
                self.assertEqual(info["channels"], ch, f"{name} 声道与 ffprobe 不符")
                self.assertEqual(info["bitrateKbps"], kbps, f"{name} 码率与 ffprobe 不符")
        # 带封面样本：音频是 copy 的，参数应与源文件一致
        info = self.probe(self.with_cover)["info"]
        rate, ch, kbps = self.ffprobe_a(self.with_cover)
        self.assertEqual((info["sampleRate"], info["channels"], info["bitrateKbps"]), (rate, ch, kbps))

    def test_sample_parameters_and_decision(self):
        """逐个样本核对 采样率/声道/码率 与"是否需要转码"的决策。"""
        for name, _args, rate, ch, kbps, need in SAMPLES:
            with self.subTest(sample=name):
                got = self.probe(self.samples[name])
                info = got["info"]
                self.assertIsNotNone(info, f"{name} 应能被识别")
                self.assertEqual(info["sampleRate"], rate)
                self.assertEqual(info["channels"], ch)
                self.assertEqual(info["bitrateKbps"], kbps)
                self.assertEqual(info["vbr"], False)
                self.assertEqual(got["need"], need, f"{name} 的转码决策不符: {got}")

    def test_vbr_is_conservatively_transcoded(self):
        """VBR 文件码率不定 -> 必须保守转码(即便采样率/声道本来就合格)。"""
        got = self.probe(self.vbr)
        self.assertIsNotNone(got["info"])
        self.assertTrue(got["info"]["vbr"], f"未识别出 Xing 头: {got}")
        self.assertTrue(got["need"])

    def test_skips_id3v2_cover(self):
        """带大封面(ID3v2)的文件必须跳过 ID3 找到真正的音频帧, 否则会误判为需转码。"""
        got = self.probe(self.with_cover)
        info = got["info"]
        self.assertIsNotNone(info, "带封面的 MP3 应能识别")
        self.assertEqual(info["sampleRate"], 44100)
        self.assertEqual(info["channels"], 2)
        self.assertEqual(info["bitrateKbps"], 320)
        self.assertGreater(info["audioOffset"], 0, "应记录跳过的 ID3v2 长度")
        self.assertTrue(got["need"])

    def test_non_mp3_is_unrecognized(self):
        """非 MP3 内容必须返回 null(而不是随便命中一个伪同步字)。"""
        got = self.probe(self.not_mp3)
        self.assertIsNone(got["info"], f"非 MP3 不应被识别: {got}")
        self.assertTrue(got["need"], "识别失败必须保守转码")

    def test_info_text_is_readable(self):
        """展示文本要能直接放进上传列表(用户据此判断)。"""
        got = self.probe(self.samples["mono_24k_96.mp3"])
        self.assertIn("24kHz", got["text"])
        self.assertIn("单声道", got["text"])
        self.assertIn("96kbps", got["text"])


if __name__ == "__main__":
    unittest.main()
