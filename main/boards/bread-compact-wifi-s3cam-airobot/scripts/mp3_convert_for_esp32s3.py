#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""批量把 MP3 转码为 ESP32-S3 播放友好的格式。

背景:
    ESP32-S3 的 Helix MP3 解码器对 320kbps 立体声 44.1kHz 实时解码很吃力
    (解码速度 < 播放速度)，表现为：播放卡顿、变慢、有滋滋声。
    转成 24000Hz 单声道低码率 MP3 后：
      - 解码负载降约 4 倍，播放流畅；
      - 24000Hz = 设备 codec 输出采样率，固件端无需重采样(省 CPU、无音损)。

用法:
    python main/boards/bread-compact-wifi-s3cam-airobot/scripts/mp3_convert_for_esp32s3.py <源目录> <输出目录>
    python main/boards/bread-compact-wifi-s3cam-airobot/scripts/mp3_convert_for_esp32s3.py <源目录> <输出目录> --rate 22050 --bitrate 64k

示例:
    python main/boards/bread-compact-wifi-s3cam-airobot/scripts/mp3_convert_for_esp32s3.py "E:/音乐/2026新下" "E:/music_s3"

说明:
    - 输出保留原始文件名(含中文)，转码完把输出目录里的 .mp3 拷贝到 SD 卡 /sdcard/music；
    - 同名 .lrc 歌词也会自动转为 UTF-8 一并输出(设备端无 GBK 转换能力，必须在此转换)；
    - 已转换过的文件(输出存在且不比源旧)自动跳过，可重复运行；
    - 需要 ffmpeg 在 PATH 中(Windows: 确认 `ffmpeg -version` 可用)。
"""
import argparse
import os
import shutil
import subprocess
import sys

FFMPEG = shutil.which("ffmpeg")
if not FFMPEG:
    sys.exit("未找到 ffmpeg，请先安装并加入 PATH，或用 pip 安装 ffmpeg 包后重试")

# 默认参数：24000Hz 单声道 —— 与固件 codec 输出采样率一致，固件端零重采样
DEFAULT_RATE = 24000
DEFAULT_BITRATE = "96k"


def find_mp3(root):
    """递归查找目录下所有 .mp3 文件（忽略大小写）"""
    for dirpath, _dirs, files in os.walk(root):
        for f in sorted(files):
            if f.lower().endswith(".mp3"):
                yield os.path.join(dirpath, f)


def convert_lrc(mp3_path, dst):
    """同目录同名 .lrc：自动探测 GBK/UTF-8 编码并转为 UTF-8 写到输出目录。
    设备端(ESP-IDF v6)没有 GBK->UTF-8 转换能力，歌词必须在这里转好。"""
    base = os.path.splitext(os.path.basename(mp3_path))[0]
    lrc_src = os.path.join(os.path.dirname(mp3_path), base + ".lrc")
    if not os.path.exists(lrc_src):
        return
    lrc_out = os.path.join(dst, base + ".lrc")
    # 幂等：已转好且不比源旧则跳过
    if os.path.exists(lrc_out) and os.path.getmtime(lrc_out) >= os.path.getmtime(lrc_src):
        return
    with open(lrc_src, "rb") as f:
        raw = f.read()
    enc = None
    for cand in ("utf-8-sig", "utf-8", "gbk"):
        try:
            text = raw.decode(cand)
            enc = cand
            break
        except UnicodeDecodeError:
            continue
    if enc is None:
        print(f"    歌词编码无法识别，原样复制: {base}.lrc")
        shutil.copy(lrc_src, lrc_out)
        return
    with open(lrc_out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"    歌词已转 UTF-8: {base}.lrc ({enc})")


def convert(src, dst, rate, bitrate):
    if not os.path.isdir(src):
        sys.exit(f"源目录不存在: {src}")
    os.makedirs(dst, exist_ok=True)

    files = list(find_mp3(src))
    if not files:
        print(f"目录 {src} 下没有找到 .mp3 文件")
        return
    print(f"找到 {len(files)} 首 MP3，目标采样率 {rate}Hz，单声道 {bitrate}")

    ok = skip = fail = 0
    for i, path in enumerate(files, 1):
        name = os.path.basename(path)
        out = os.path.join(dst, name)
        # 幂等：输出已存在且不比源旧则跳过
        if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(path):
            print(f"[{i}/{len(files)}] 跳过(已存在): {name}")
            skip += 1
            convert_lrc(path, dst)  # mp3 已存在也确保歌词转好
            continue

        cmd = [
            FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
            "-i", path,
            "-vn",                       # 去掉封面图等视频流
            "-ac", "1",                  # 单声道（固件输出链路为单声道）
            "-ar", str(rate),            # 采样率
            "-b:a", bitrate,             # 码率
            "-c:a", "libmp3lame",        # LAME MP3 编码
            out,
        ]
        print(f"[{i}/{len(files)}] 转码: {name}")
        try:
            r = subprocess.run(cmd, encoding="utf-8", errors="replace")
            if r.returncode == 0 and os.path.exists(out):
                ok += 1
                convert_lrc(path, dst)
            else:
                print(f"    失败(返回码 {r.returncode}): {name}")
                fail += 1
        except OSError as e:
            print(f"    执行失败: {e}")
            fail += 1

    print(f"\n完成: 转码 {ok}，跳过 {skip}，失败 {fail}")
    print(f"输出目录: {dst}")
    print("下一步: 把输出目录里的 .mp3 和 .lrc 全部拷贝到 SD 卡的 /sdcard/music 目录即可。")


def main():
    ap = argparse.ArgumentParser(description="MP3 转码为 ESP32-S3 播放友好格式")
    ap.add_argument("src", help="源 MP3 目录")
    ap.add_argument("dst", help="输出目录")
    ap.add_argument("--rate", type=int, default=DEFAULT_RATE,
                    help=f"输出采样率 (默认 {DEFAULT_RATE}Hz；更低如 22050 更省 CPU，音质略降)")
    ap.add_argument("--bitrate", default=DEFAULT_BITRATE,
                    help=f"输出码率 (默认 {DEFAULT_BITRATE}；更省 CPU 可用 64k)")
    args = ap.parse_args()
    convert(args.src, args.dst, args.rate, args.bitrate)


if __name__ == "__main__":
    main()
