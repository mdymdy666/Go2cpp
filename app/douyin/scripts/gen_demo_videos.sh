#!/usr/bin/env bash
# 从 orig 原始素材生成网页可播的 demo1-5.mp4 与首帧封面 demo1-5.jpg。
#
# 为什么需要它（2026-11-06 用户视角审查的根因）：
#   仓库 .gitignore 忽略 web/videos/*.mp4，只把 orig/*.orig.mp4 入库。
#   但此前**没有任何脚本负责 orig → demo*.mp4 的转码**，本地环境里的
#   demo1/4/5.mp4 是历史遗留产物，demo2/demo3 从未生成过。结果 seed 写入的
#   8 条视频指向不存在的文件，首页打开就是「视频加载失败」。加这个脚本 +
#   run_all.sh 启动前调用，保证「clone 下来就能播」。
#
# 用法：
#   scripts/gen_demo_videos.sh          # 只补缺失的（已存在则跳过）
#   scripts/gen_demo_videos.sh --force  # 全部重新转码
#   退出码 0=素材齐全；1=ffmpeg 缺失或转码失败。
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VDIR="$ROOT/web/videos"
FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1
QUIET=0
[[ "${1:-}" == "-q" ]] && QUIET=1

log() { [[ "$QUIET" == "1" ]] || echo "[gen_videos] $*"; }

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "[gen_videos] 缺少 ffmpeg，无法生成 demo 视频；请先安装 ffmpeg" >&2
  exit 1
fi

made=0
skipped=0
failed=0
for n in 1 2 3 4 5; do
  src="$VDIR/orig/demo$n.orig.mp4"
  out="$VDIR/demo$n.mp4"
  cover="$VDIR/demo$n.jpg"
  if [[ ! -f "$src" ]]; then
    echo "[gen_videos] 缺少原始素材 $src" >&2
    failed=$((failed + 1))
    continue
  fi
  if [[ -s "$out" && "$FORCE" != "1" ]]; then
    skipped=$((skipped + 1))
    continue
  fi
  # demo3 的原始素材开头有 3 秒淡入黑屏，直接截首帧会是纯黑封面；
  # 统一从第 4 秒起截，用户点开就有画面。
  ss_args=()
  [[ "$n" == "3" ]] && ss_args=(-ss 4)
  # demo2 原始只有 5 秒，循环一次凑够 10 秒，与其它 demo 时长一致。
  loop_args=()
  [[ "$n" == "2" ]] && loop_args=(-stream_loop 1)
  if ! ffmpeg -y -v error "${ss_args[@]}" "${loop_args[@]}" -i "$src" -t 10 \
      -vf "scale=640:360:force_original_aspect_ratio=decrease,pad=640:360:(ow-iw)/2:(oh-ih)/2" \
      -c:v libx264 -preset veryfast -crf 26 -pix_fmt yuv420p -r 25 \
      -c:a aac -b:a 96k -movflags +faststart "$out"; then
    echo "[gen_videos] 转码失败：demo$n" >&2
    failed=$((failed + 1))
    continue
  fi
  if ! ffmpeg -y -v error -i "$out" -frames:v 1 -vf "scale=480:270" -q:v 3 "$cover"; then
    echo "[gen_videos] 抽封面失败：demo$n" >&2
  fi
  log "生成 demo$n.mp4 ($(stat -c%s "$out") B) + demo$n.jpg"
  made=$((made + 1))
done

log "完成：新生成 $made，跳过 $skipped，失败 $failed"
[[ "$failed" == "0" ]] || exit 1
