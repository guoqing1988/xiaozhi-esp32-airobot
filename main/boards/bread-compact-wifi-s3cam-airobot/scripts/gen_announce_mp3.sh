#!/usr/bin/env bash
# 生成传感器事件播报音频（macOS: say 人声 → ffmpeg 转 24kHz 单声道 96kbps MP3）。
#
# 输出到脚本同级的 announce/ 目录，把该目录整体拷到 TF 卡 /sdcard/announce/ 即可：
#   cp -r announce/* /Volumes/<TF卡>/announce/
#
# 依赖：macOS 自带 say；ffmpeg（brew install ffmpeg）。
# 也可以用自己录的真人声直接覆盖同名文件（文件名固定，见板级 README）。
#
# 用法：
#   ./gen_announce_mp3.sh            # 用默认中文语音 Tingting
#   VOICE=Meijia ./gen_announce_mp3.sh
set -euo pipefail

OUT_DIR="$(cd "$(dirname "$0")" && pwd)/announce"
mkdir -p "$OUT_DIR"

VOICE="${VOICE:-Tingting}"
RATE="${RATE:-180}"

if ! command -v say >/dev/null 2>&1; then
  echo "未找到 say（仅 macOS 自带）。请改用其它 TTS 生成 MP3 后放入 $OUT_DIR" >&2
  exit 1
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "未找到 ffmpeg，请先 brew install ffmpeg" >&2
  exit 1
fi

gen() {  # gen <文件名> <文本>
  local name="$1"; shift
  local text="$*"
  local aiff
  aiff="$(mktemp -t announce).aiff"
  say -v "$VOICE" -r "$RATE" -o "$aiff" "$text"
  # 与项目转码规范一致：24000Hz 单声道 96kbps（解码负载低、播放流畅、唤醒灵敏）
  ffmpeg -y -loglevel error -i "$aiff" -ar 24000 -ac 1 -b:a 96k "$OUT_DIR/$name.mp3"
  rm -f "$aiff"
  echo "生成 $OUT_DIR/$name.mp3  <= \"$text\""
}

gen motion "检测到有人靠近，已为你开灯"
gen beam   "门口有人经过，请注意"
gen hot    "室内温度偏高，请留意通风"

echo
echo "完成。把 $OUT_DIR 下的 mp3 拷到 TF 卡 /sdcard/announce/ 目录即可。"
