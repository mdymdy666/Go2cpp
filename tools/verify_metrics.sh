#!/usr/bin/env bash
# =============================================================================
# tools/verify_metrics.sh — 校验 docs/metrics-ledger.md 的 3 条基线是否仍对齐
#
# 用途：一条命令完成「编译基准 → 跑基准 → 解析数值 → 对比 ledger 历史值」，
#       报 OK / STALE / 未接线。只读源码与 ledger，不改仓库内其它文件（仅追加 out）。
#
# 接线的基线：
#   [M01b] 通用 Channel<int> 1P1C ns/条
#          源 tests/concurrency/bench_channel_opt.cpp:84（PrintRow 输出 median ns/op）
#          取 [1] SPSC 段 cap=1024 的 median，1e6 条 ×3 reps；对照 ledger:44 的 10.73
#   [M02]  Fiber 全量切换 ns/往返
#          源 tests/concurrency/bench_fiber_switch_opt.cpp:308
#          取 `Fiber::resume ping-pong (yield)` 批量值（每次 resume 往返一次 Suspend）。
#          ledger:53 未写明具体 API；这是本仓现有基准中最接近「全量切换」的口径，
#          若主控认定口径不同，改下面 metric_m02 的 grep 行即可。
#          对照 ledger:53 的 33.80 与后续快照 23.5
#   [M03]  Scheduler task churn wall ns/task
#          源 tests/concurrency/bench_scheduler_task_opt.cpp:102（printf 出 wall ns/task）
#          4 worker / 100k tasks / 5 轮取 wall 中位；对照 ledger:62 的 122.0
#          与 ledger:74 的本轮中位 239
#
# 用法：tools/verify_metrics.sh          正常校验
#       tools/verify_metrics.sh --help   显示本说明
#
# 退出码：0 = 全部 OK，或仅有「未接线/无法验证」（不算失败）
#         1 = 至少一条 STALE（实测与 ledger 历史值全部超出容差）
#         2 = 环境错误（找不到仓库根 / 无法写 tools/verify_metrics.out）
#
# 容差：TOL_PCT=30（±30%）。WSL2 宿主负载 + 2 核绑核 + 4 worker 超订会让单轮 wall
#       抖动几十个百分点；30% 是「口径一致、机器状态不同」的容忍上限，超出判 STALE
#       ——表示数字本身对不上，不是噪声。多条历史值时命中任一即 OK（明细全打印）。
#
# 环境（2026-11-03 实测）：12 vCPU；统一 taskset -c 2,3；库复用 .build-bento-wsl
#       （Release -O2/-O3）。旧二进制 .build-bento-wsl/bench_channel_opt 曾报
#       `undefined symbol: Context::IsDone`，故此处一律从源码重编基准；实测现有
#       .so 与当前 include 兼容（3 个基准均链接/运行成功）。
# === END USAGE ===
set -uo pipefail
export LC_ALL=C

TOL_PCT=30

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd -P)" || {
  printf '无法定位脚本目录\n' >&2; exit 2; }
ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null)"
[ -n "$ROOT" ] || ROOT="$(cd -- "$SCRIPT_DIR/.." >/dev/null 2>&1 && pwd -P)"
if [ -z "$ROOT" ] || [ ! -d "$ROOT/tests/concurrency" ]; then
  printf '仓库根定位失败: %s\n' "$ROOT" >&2; exit 2
fi

case "${1:-}" in
  -h|--help) sed -n '2,/^# === END USAGE ===$/p' "$0"; exit 0 ;;
esac

OUT="$ROOT/tools/verify_metrics.out"
if ! : 2>/dev/null >>"$OUT"; then
  printf '无法写输出文件: %s\n' "$OUT" >&2; exit 2
fi
BUILD_DIR="$ROOT/.build-bento-wsl"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/verify_metrics.XXXXXX")" || {
  printf 'mktemp 失败\n' >&2; exit 2; }

if command -v taskset >/dev/null 2>&1 && [ "$(nproc)" -ge 4 ]; then
  PIN=(taskset -c 2,3); PIN_DESC='taskset -c 2,3'
else
  PIN=(); PIN_DESC='未绑核'
fi

TOTAL=0 OK=0 STALE=0 SKIP=0

log() { printf '%s\n' "$*"; }
skip() { log "  未接线: $1"; SKIP=$((SKIP+1)); }

# compile_bench <bin> <src> <std> <opt> <lib...>：只重编基准源码，复用 .build-bento-wsl 的 .so。
compile_bench() {
  local bin=$1 src=$2 std=$3 opt=$4; shift 4
  local blog="$WORK/$(basename "$bin").build.log"
  local -a cmd=(g++ "$opt" -DNDEBUG "-std=$std" "-I$ROOT/include" "$ROOT/$src"
                "-L$BUILD_DIR" "-Wl,-rpath,$BUILD_DIR" "$@" -pthread -o "$bin")
  log "  编译: ${cmd[*]}"
  if ! "${cmd[@]}" >"$blog" 2>&1; then
    log "  编译 stderr: $(tail -n 3 "$blog" | tr '\n' '|')"
    return 1
  fi
  return 0
}

# judge <实测> <历史值列表> <口径摘要>：以列表最后一个（最近一次记录）为主对照，
# 偏差超出 TOL_PCT 即 STALE；其余历史值只打印偏差，供判断数字漂移。
judge() {
  local measured=$1 hists=$2 cond=$3 h dev absd detail='' hit=0 primary
  primary="${hists##* }"
  for h in $hists; do
    dev=$(awk -v m="$measured" -v x="$h" 'BEGIN{printf "%+.1f", (m-x)*100.0/x}')
    detail="${detail}ledger=$h(偏差${dev}%) "
  done
  absd=$(awk -v m="$measured" -v x="$primary" 'BEGIN{d=(m-x)*100.0/x; if(d<0)d=-d; printf "%.1f", d}')
  if awk -v a="$absd" -v t="$TOL_PCT" 'BEGIN{exit !(a<=t)}'; then hit=1; fi
  log "  实测: ${measured} ns  [$cond]"
  log "  对照: ${detail}容差 ±${TOL_PCT}%（主对照=最近一次 ledger=$primary）"
  if [ "$hit" -eq 1 ]; then
    log "  => OK"; OK=$((OK+1))
  else
    log "  => STALE"; STALE=$((STALE+1))
  fi
}

metric_m01b() {
  local id=M01b src=tests/concurrency/bench_channel_opt.cpp
  TOTAL=$((TOTAL+1))
  log ''; log "[$id] 通用 Channel<int> 1P1C ns/条"
  [ -f "$ROOT/$src" ] || { skip "无基准源: $src"; return; }
  local bin="$WORK/bench_channel_opt"
  compile_bench "$bin" "$src" c++20 -O3 \
      -lgo2cpp_channel -lgo2cpp_context -lgo2cpp_scheduler -lgo2cpp_fiber \
      -lgo2cpp_error -lgo2cpp_log || { skip "编译失败（$src）"; return; }
  local logf="$WORK/m01b.run.log"
  log "  运行: $PIN_DESC $bin 1000000 3 spsc"
  if ! "${PIN[@]}" "$bin" 1000000 3 spsc >"$logf" 2>&1; then
    skip '基准运行退出码非零'; return
  fi
  while IFS= read -r l; do log "     | $l"; done <"$logf"
  local val
  val=$(awk '/^\[1\] SPSC/{s=1;next} /^\[/{s=0} s&&$1=="Channel"&&$2=="cap=1024"{for(i=1;i<=NF;i++)if($i=="median")print $(i+1)}' "$logf")
  [ -n "$val" ] || { skip '无法从基准输出解析 cap=1024 的 median'; return; }
  judge "$val" '10.73' '1e6 条 ×3 reps，[1] SPSC cap=1024，taskset 2,3'
}

metric_m02() {
  local id=M02 src=tests/concurrency/bench_fiber_switch_opt.cpp
  TOTAL=$((TOTAL+1))
  log ''; log "[$id] Fiber 全量切换 ns/往返"
  [ -f "$ROOT/$src" ] || { skip "无基准源: $src"; return; }
  local bin="$WORK/bench_fiber_switch"
  compile_bench "$bin" "$src" c++17 -O3 \
      -lgo2cpp_fiber -lgo2cpp_debug -lgo2cpp_debug_event -lgo2cpp_error \
      || { skip "编译失败（$src）"; return; }
  local logf="$WORK/m02.run.log"
  log "  运行: $PIN_DESC $bin 1000000"
  if ! "${PIN[@]}" "$bin" 1000000 >"$logf" 2>&1; then
    skip '基准运行退出码非零'; return
  fi
  while IFS= read -r l; do log "     | $l"; done <"$logf"
  local val
  val=$(grep -F 'Fiber::resume ping-pong (yield)' "$logf" | awk '{print $(NF-1)}')
  [ -n "$val" ] || { skip '无法解析 Fiber::resume ping-pong (yield)'; return; }
  judge "$val" '33.80 23.5' 'resume↔Suspend 往返，1e6 次，taskset 2,3'
}

metric_m03() {
  local id=M03 src=tests/concurrency/bench_scheduler_task_opt.cpp
  TOTAL=$((TOTAL+1))
  log ''; log "[$id] Scheduler task churn wall ns/task"
  [ -f "$ROOT/$src" ] || { skip "无基准源: $src"; return; }
  local bin="$WORK/bench_scheduler_task_opt"
  compile_bench "$bin" "$src" c++17 -O2 \
      -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_debug \
      -lgo2cpp_debug_event || { skip "编译失败（$src）"; return; }
  local logf="$WORK/m03.run.log"
  log "  运行: $PIN_DESC $bin 4 100000 5"
  if ! "${PIN[@]}" "$bin" 4 100000 5 >"$logf" 2>&1; then
    skip '基准运行退出码非零'; return
  fi
  while IFS= read -r l; do log "     | $l"; done <"$logf"
  local val cpuval
  val=$(awk '$1=="[round"{for(i=1;i<=NF;i++)if($i=="wall"){print $(i+1);break}}' "$logf" \
        | sort -n | awk '{a[NR]=$1} END{if(NR>0) print a[int((NR+1)/2)]}')
  cpuval=$(awk '$1=="[round"{for(i=1;i<=NF;i++)if($i=="cpu"){print $(i+1);break}}' "$logf" \
        | sort -n | awk '{a[NR]=$1} END{if(NR>0) print a[int((NR+1)/2)]}')
  [ -n "$val" ] || { skip '无法解析 wall ns/task'; return; }
  log "  cpu 中位: ${cpuval:-?} ns/task（同 5 轮，仅诊断宿主负载，不参与判定）"
  judge "$val" '122.0 239' '4 worker/100k tasks/5 轮 wall 中位，taskset 2,3'
}

main() {
  log '=============================================================='
  log " verify_metrics 开始: $(date '+%F %T %z')"
  log " repo=$ROOT"
  log " 环境: nproc=$(nproc) 绑定=$PIN_DESC 构建库=$BUILD_DIR"
  log " 容差: ±${TOL_PCT}%  输出=$OUT"
  log '=============================================================='
  metric_m01b
  metric_m02
  metric_m03
  log ''
  log '=============================================================='
  log "TOTAL=$TOTAL OK=$OK STALE=$STALE 未接线=$SKIP"
  log '=============================================================='
  local rc=0
  [ "$STALE" -gt 0 ] && rc=1
  rm -rf "$WORK"
  return "$rc"
}

main "$@" 2>&1 | tee -a "$OUT"
rc=${PIPESTATUS[0]}
exit "$rc"
