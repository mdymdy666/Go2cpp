#!/usr/bin/env bash
# MJPEG AVI 写入器验证：ffmpeg 生成 JPEG 序列 → C++ 封装 AVI → ffprobe/ffmpeg 校验。
#
# 用法：
#   tests/media/build_and_run_avi.sh
#
# 环境变量：
#   OUT_DIR  输出目录（默认 /tmp/go2cpp_avi_test）
#   CXX      编译器（默认 g++）
#   FRAMES   帧数（默认 30）
#
# 依赖：ffmpeg / ffprobe（本机已安装）。不修改主 CMake，不链接媒体库。
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT_DIR:-/tmp/go2cpp_avi_test}
CXX=${CXX:-g++}
FRAMES=${FRAMES:-30}
SRC="$ROOT/tests/media/test_avi_mjpeg.cpp"
BIN="$OUT/test_avi_mjpeg"
FRAME_DIR="$OUT/frames"
AVI_OUT="$OUT/mjpeg_test.avi"

mkdir -p "$FRAME_DIR" || exit 2
rm -f "$FRAME_DIR"/*.jpg "$AVI_OUT"

command -v ffmpeg >/dev/null 2>&1 || { echo "缺少 ffmpeg" >&2; exit 2; }
command -v ffprobe >/dev/null 2>&1 || { echo "缺少 ffprobe" >&2; exit 2; }

echo "[gen] ffmpeg 生成 $FRAMES 帧 testsrc2 320x240 JPEG"
ffmpeg -y -v error -f lavfi -i "testsrc2=size=320x240:rate=30" \
  -frames:v "$FRAMES" -q:v 5 "$FRAME_DIR/frame_%03d.jpg" || exit 3

echo "[build] $SRC"
"$CXX" -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/include" "$SRC" -o "$BIN" || exit 4

echo "[run] $BIN"
"$BIN" --frames "$FRAME_DIR" --out "$AVI_OUT" --fps 30 || exit 5

echo "[check] 文件头"
head -c 12 "$AVI_OUT" | od -An -c | tr -s ' ' | head -1
# RIFF....AVI 四字符检查（偏移 0 = RIFF，偏移 8 = AVI ）。
python3 - "$AVI_OUT" <<'PY' || exit 6
import sys
data = open(sys.argv[1], "rb").read(12)
assert data[0:4] == b"RIFF", data[0:4]
assert data[8:12] == b"AVI ", data[8:12]
riff_size = int.from_bytes(data[4:8], "little")
import os
total = os.path.getsize(sys.argv[1])
assert riff_size == total - 8, (riff_size, total)
print(f"  RIFF size={riff_size} 文件大小={total}（一致）")
PY

echo "[check] ffprobe"
PROBE=$(ffprobe -v error -select_streams v:0 -count_frames \
  -show_entries stream=codec_name,width,height,nb_read_frames,duration \
  -of default=noprint_wrappers=1 "$AVI_OUT") || exit 7
echo "$PROBE" | sed 's/^/  /'
grep -q '^codec_name=mjpeg$' <<<"$PROBE" || { echo "codec 不是 mjpeg" >&2; exit 7; }
grep -q '^width=320$' <<<"$PROBE" || { echo "宽度不是 320" >&2; exit 7; }
grep -q '^height=240$' <<<"$PROBE" || { echo "高度不是 240" >&2; exit 7; }
grep -q "^nb_read_frames=$FRAMES$" <<<"$PROBE" || { echo "帧数不是 $FRAMES" >&2; exit 7; }

echo "[check] ffmpeg 全量解码"
DECODE_LOG=$(ffmpeg -v error -i "$AVI_OUT" -f null - 2>&1) || { echo "解码失败：$DECODE_LOG" >&2; exit 8; }
if [[ -n "$DECODE_LOG" ]]; then
  echo "解码有错误输出：" >&2
  echo "$DECODE_LOG" >&2
  exit 8
fi

echo "[PASS] AVI 容器校验通过（$FRAMES 帧 MJPEG 320x240@30，ffmpeg 全量解码无错误）"
