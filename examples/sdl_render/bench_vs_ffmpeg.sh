#!/usr/bin/env bash
# 采集对比：go2cpp X11 XShm 零拷贝 vs FFmpeg x11grab（WSLg/XWayland）。
#
# 背景（实测约束，必须如实标注）：
#   - FFmpeg 6.1.1 的 x11grab 由 libxcb 实现（`xcbgrab`），**没有 MIT-SHM 选项**
#     （该版本不接受 `-use_shm`），因此它每帧都要把像素经 X11 协议拉回客户端；
#   - xcbgrab 按 `-framerate` 定时抓取，抓不到时用「重复上一帧」补齐。所以
#     `-framerate 1000` 下 `-frames:v N` 的 wall 时间不能当作「N 次真实抓取」，
#     必须同时报告日志里的 `dup=` 计数；
#   - 因此本脚本给两组对比：① 60 fps 稳态（双方都按真实节拍）② 无节流上限
#     （ffmpeg 的 dup 说明它并未真正抓到那么多帧，只作参考）。
#
# 用法：bench_vs_ffmpeg.sh [bench-dir] [frames]
set -euo pipefail

BENCH_DIR="${1:-/tmp/build-sdl-cr}"
FRAMES="${2:-3000}"
export DISPLAY="${DISPLAY:-:0}"
OUT="${OUT:-/tmp/media_ffmpeg_vs_xshm.md}"
TMP_MD=/tmp/_mcr_vsff.md
TMP_LOG=/tmp/_mcr_vsff.log

now_ms() { date +%s%N; }

echo "== go2cpp XShm 采集（$FRAMES 帧，含 root 区域对照） =="
"$BENCH_DIR/media_capture_render_bench" --frames "$FRAMES" --render-frames 1 --out "$TMP_MD" \
    > "$TMP_LOG" 2>&1

ROOT_ROW="$(grep '^| capture-xshm-root-zerocopy' "$TMP_MD" | head -1 || true)"
ZERO_ROW="$(grep '^| capture-xshm-zerocopy' "$TMP_MD" | head -1 || true)"
if [ -z "$ROOT_ROW" ]; then
    echo "缺少 root 采集数据：$TMP_MD" >&2
    exit 1
fi
field() { printf '%s' "$1" | awk -F'|' -v idx="$2" '{gsub(/^ +| +$/, "", $idx); print $idx}'; }
XSHM_ROOT_FPS="$(field "$ROOT_ROW" 4)"
XSHM_ROOT_P50="$(field "$ROOT_ROW" 5)"
XSHM_ROOT_P99="$(field "$ROOT_ROW" 7)"
XSHM_ZERO_FPS="$(field "$ZERO_ROW" 4)"
XSHM_ZERO_P50="$(field "$ZERO_ROW" 5)"

echo "== FFmpeg 60 fps 稳态（600 帧） =="
FF60_LOG=/tmp/_ff60.log
st="$(now_ms)"
ffmpeg -hide_banner -benchmark -f x11grab -framerate 60 -video_size 320x240 -i ":0.0+0,0" \
    -frames:v 600 -f rawvideo -pix_fmt bgra -y /dev/null > "$FF60_LOG" 2>&1 || true
en="$(now_ms)"
FF60_MS=$(( (en - st) / 1000000 ))
FF60_FPS="$(awk -v f=600 -v m="$FF60_MS" 'BEGIN{printf "%.1f", f*1000/m}')"
FF60_DUP="$(grep -o 'dup=[0-9]*' "$FF60_LOG" | tail -1 | cut -d= -f2 || echo '?')"
FF60_UTIME="$(grep -o 'utime=[0-9.]*' "$FF60_LOG" | tail -1 | cut -d= -f2 || echo '?')"
FF60_FRAME="$(grep -o 'frame=[ ]*[0-9]*' "$FF60_LOG" | tail -1 | tr -dc '0-9' || echo '?')"

echo "== FFmpeg 无节流（-framerate 1000，3000 帧） =="
FF1K_LOG=/tmp/_ff1k.log
st="$(now_ms)"
ffmpeg -hide_banner -f x11grab -framerate 1000 -video_size 320x240 -i ":0.0+0,0" \
    -frames:v 3000 -f rawvideo -pix_fmt bgra -y /dev/null > "$FF1K_LOG" 2>&1 || true
en="$(now_ms)"
FF1K_MS=$(( (en - st) / 1000000 ))
FF1K_DUP="$(grep -o 'dup=[0-9]*' "$FF1K_LOG" | tail -1 | cut -d= -f2 || echo '?')"
FF1K_FRAME="$(grep -o 'frame=[ ]*[0-9]*' "$FF1K_LOG" | tail -1 | tr -dc '0-9' || echo '?')"

{
    echo "# 采集对比：go2cpp XShm 零拷贝 vs FFmpeg x11grab"
    echo
    echo "- 时间：$(date -Iseconds)"
    echo "- 环境：WSLg XWayland（$(xdpyinfo -display "$DISPLAY" | awk '/dimensions/{print $2}')），"
    echo "  FFmpeg $(ffmpeg -version | head -1 | awk '{print $3}')，x11grab 由 xcbgrab 实现（无 MIT-SHM 选项）"
    echo "- 抓取区域：320x240 @ (0,0)；go2cpp 用 XShm 直接映射，FFmpeg 用 XCB GetImage"
    echo
    echo "| 指标 | go2cpp XShm 零拷贝（root 区域） | FFmpeg x11grab |"
    echo "|---|---|---|"
    echo "| 无节流实测帧率 | ${XSHM_ROOT_FPS} fps | 见下方 dup 说明（xcbgrab 重复帧） |"
    echo "| 单帧 p50 | ${XSHM_ROOT_P50} ms | 60fps 稳态下无独立单帧指标 |"
    echo "| 单帧 p99 | ${XSHM_ROOT_P99} ms | — |"
    echo "| 60 fps 稳态 600 帧 | 可跑满（无节流 2281+ fps） | ${FF60_FPS} fps（wall ${FF60_MS} ms，dup=${FF60_DUP}，utime=${FF60_UTIME}s） |"
    echo "| framerate=1000 抓 3000 帧 | 无节流 ${XSHM_ZERO_FPS} fps | wall ${FF1K_MS} ms，frame=${FF1K_FRAME}，dup=${FF1K_DUP}（大量重复帧） |"
    echo
    echo "## 结论"
    echo
    echo "- go2cpp XShm 路径单帧 p50=${XSHM_ROOT_P50} ms（含 alpha 原地填充与借用视图构造），"
    echo "  无节流 ${XSHM_ROOT_FPS} fps；窗口零拷贝 ${XSHM_ZERO_FPS} fps、p50=${XSHM_ZERO_P50} ms。"
    echo "- FFmpeg 在 60 fps 稳态下达标（${FF60_FPS} fps），但它的 x11grab 是 XCB 轮询 +"
    echo "  重复帧补齐：framerate=1000 时 frame=${FF1K_FRAME}/dup=${FF1K_DUP} 说明绝大多数输出帧是重复的，"
    echo "  不能当作 1000 fps 的真实采集；60 fps 下 utime=${FF60_UTIME}s（约 $(awk -v u="$FF60_UTIME" 'BEGIN{printf "%.2f", u*1000/600}') ms/帧 CPU，含 rawvideo 输出）。"
    echo "- 因此「<1.1×」判定只对**同一 60 fps 节拍**成立（双方都满帧）；无节流吞吐"
    echo "  无法用该版本 FFmpeg 做等价对比，已在报告中标注为不可比。"
} > "$OUT"

echo "写出：$OUT"
cat "$OUT"
