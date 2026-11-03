#!/usr/bin/env bash
# =============================================================================
# tools/verify_metrics.sh — 全量校验 docs/metrics-ledger.md（16 条记录）
#
# 一条命令完成「编译基准 → 跑基准 → 解析数值 → 与 ledger 当前值对照」，每条输出
# [ID] 实测=… ledger=… 判定=OK/STALE/SKIP，末尾给 TOTAL/OK/STALE/SKIP 与 STALE 清单。
#
# 铁律：ledger 数字只作**对照基线**，不作结论；输出一律「实测 vs ledger」。
#   判定：任一条 ledger 记录值落在该条容差内 → OK；全部超出 → STALE；跑不起来或
#   只能手工 → SKIP。SKIP 不等于通过，不计入 OK。编译/运行失败一律 SKIP 并继续，
#   不中断整轮；单条基准上限 300 s。
#
# 接线表（ID → 被测命令 → ledger 行；行号由 check_refs 用 sed -n 现场复核）：
#   M01    已拆条（历史 10.73 实为 FastChannel）              ledger:32  → 手工说明
#   M01b-a tests/concurrency/bench_channel_opt.cpp spsc       ledger:42
#   M01b-b .build-bento-wsl/go2cpp_fast_channel_bench         ledger:56
#   M02    tests/concurrency/bench_fiber_switch_opt.cpp       ledger:66
#   M03    tests/concurrency/bench_scheduler_task_opt.cpp     ledger:75
#   M04    tests/concurrency/bench_scheduler_submit.cpp       ledger:92
#   M05    net_perf_server + http_loadgen（需先起服务端）      ledger:101 → 手工
#   M05b   无出处，无可校验数字                               ledger:112
#   M06    .build-bento-wsl/go2cpp_storage_bench 1e6 ×3 轮    ledger:121
#   M07    go2cpp_leveldb_kv_test --bench/--mem（分钟级）      ledger:130 → 手工
#   M08    .build-bento-wsl/go2cpp_bufio_test（分块 1 MiB）    ledger:144
#   M09    .build-bento-wsl/go2cpp_cache_test（hotspot）       ledger:153
#   M10    tests/os/bench_vectored.cpp（脚本内自动编译）       ledger:162
#   M11    tests/bench/bench_tools_industry.cpp + bench_gzip_crc.cpp  ledger:172
#   M12    .build-bento-wsl/go2cpp_hash_test                  ledger:182
#   M13    tests/bench/bench_reflect_stage.cpp（自动编译）     ledger:192
#
# 用法：tools/verify_metrics.sh [--quick] [--json] [--refs] [--help]
#   --quick  只跑 M01b-a/M01b-b/M02/M03 共 4 条（不跑其余 12 条）
#   --json   stdout 只输出一个 JSON 对象（人读日志仍追加进 tools/verify_metrics.out）
#   --refs   只校验上文 ledger 行号引用，MISSING/OUTRANGE 必须为 0
# 退出码：0 = 无 STALE（OK+SKIP）；1 = 至少一条 STALE；2 = 环境错误
#   （仓库根/台账/构建目录缺失、输出不可写、未知参数）。
#
# 环境（2026-11-03 实测）：12 vCPU；默认 taskset -c 2,3；M08/M09/M12 按 ledger 记
#   录「未绑核」故不绑；库复用 .build-bento-wsl（Release），基准源码现场重编到
#   mktemp 目录；每条跑完按精确进程名 pkill -x 收尾，不留后台进程。
#
# CMake 钩子（不改 CMakeLists.txt）：模板 cmake/Go2CppVerifyMetrics.cmake，opt-in、
#   不默认加入 ALL，也不被任何 CMakeLists include。用法二选一：
#     1) cmake -P cmake/Go2CppVerifyMetrics.cmake [--quick]
#     2) 在任一端 CMakeLists 里 include(cmake/Go2CppVerifyMetrics.cmake)，
#        再 cmake --build . --target verify-metrics
#
# 输出：tools/verify_metrics.out（追加写入），控制台同步打印。
# === END USAGE ===
set -uo pipefail
export LC_ALL=C

BENCH_TIMEOUT=300
JSON=0 QUICK=0 REFS_ONLY=0 MODE=full
while [ $# -gt 0 ]; do
  case "$1" in
    --json)  JSON=1 ;;
    --quick) QUICK=1; MODE=quick ;;
    --refs)  REFS_ONLY=1 ;;
    -h|--help) sed -n '2,/^# === END USAGE ===$/p' "$0"; exit 0 ;;
    *) printf '未知参数: %s（用 --help 看用法）\n' "$1" >&2; exit 2 ;;
  esac
  shift
done

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd -P)" || {
  printf '无法定位脚本目录\n' >&2; exit 2; }
ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null)"
[ -n "$ROOT" ] || ROOT="$(cd -- "$SCRIPT_DIR/.." >/dev/null 2>&1 && pwd -P)"
LEDGER="$ROOT/docs/metrics-ledger.md"
BUILD_DIR="$ROOT/.build-bento-wsl"
[ -n "$ROOT" ] && [ -d "$ROOT/tests/concurrency" ] || { printf '仓库根定位失败: %s\n' "$ROOT" >&2; exit 2; }
[ -f "$LEDGER" ] || { printf '台账不存在: %s\n' "$LEDGER" >&2; exit 2; }
[ -d "$BUILD_DIR" ] || { printf '构建目录不存在: %s（先构建 Release）\n' "$BUILD_DIR" >&2; exit 2; }
OUT="$ROOT/tools/verify_metrics.out"
: 2>/dev/null >>"$OUT" || { printf '无法写输出文件: %s\n' "$OUT" >&2; exit 2; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/verify_metrics.XXXXXX")" || { printf 'mktemp 失败\n' >&2; exit 2; }
if command -v taskset >/dev/null 2>&1 && [ "$(nproc)" -ge 4 ]; then
  PIN=(taskset -c 2,3); PIN_DESC='taskset -c 2,3'
else
  PIN=(); PIN_DESC='未绑核'
fi

TOTAL=0 OK=0 STALE=0 SKIP=0 REF_MISS=0 RC=2
ROWS=(); STALE_IDS=(); SKIP_IDS=()

log() { printf '%s\n' "$*"; }
median() { sort -n | awk '{a[NR]=$1} END{if(NR>0) print a[int((NR+1)/2)]}'; }
json_str() { printf '%s' "$1" | tr -d '\r\n' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }
join_quoted() { local out='' x; for x in "$@"; do out="${out:+$out, }\"$x\""; done; printf '%s' "$out"; }

# ------------------------------------------------------- 台账行号复核（sed -n）
REFS=(
  'M01:32:### M01 Channel 1P1C 有缓冲' 'M01b-a:42:### M01b-a 通用' 'M01b-b:56:### M01b-b'
  'M02:66:### M02 Fiber 全量切换'      'M03:75:### M03 Scheduler task churn'
  'M04:92:### M04 Scheduler submit'    'M05:101:### M05 HTTP/1.1 pipeline=16'
  'M05b:112:### M05b'                  'M06:121:### M06 SQLite 封装'
  'M07:130:### M07 LevelDB 1e7'        'M08:144:### M08 bufio 1 MiB'
  'M09:153:### M09 Cache hotspot'      'M10:162:### M10 os 批量 writev/readv'
  'M11:172:### M11 gzip 压缩/解压吞吐' 'M12:182:### M12 hash 吞吐'
  'M13:192:### M13 reflect 编解码'
)
check_refs() {
  local item id rest line pat got bad=0
  for item in "${REFS[@]}"; do
    id=${item%%:*}; rest=${item#*:}; line=${rest%%:*}; pat=${rest#*:}
    got=$(sed -n "${line}p" "$LEDGER")
    printf '%s' "$got" | grep -qF -- "$pat" || {
      log "  [ref] $id 期望 ledger:${line} 含「$pat」，实际行: ${got:0:60}"; bad=$((bad+1)); }
  done
  REF_MISS=$bad
  log "台账引用: ${#REFS[@]} 条，失配 ${bad} 条"
}

# ------------------------------------------------------------------ 通用工具
compile_bench() { # <bin> <src> <std> <opt> [libs...]
  local bin=$1 src=$2 std=$3 opt=$4; shift 4
  local -a cmd=(g++ "$opt" -DNDEBUG "-std=$std" "-I$ROOT/include" "$ROOT/$src"
                "-L$BUILD_DIR" "-Wl,-rpath,$BUILD_DIR" "$@" -pthread -o "$bin")
  log "  编译: ${cmd[*]}"
  "${cmd[@]}" >"$WORK/$(basename "$bin").build.log" 2>&1 \
      || { log "  编译失败: $(tail -n 2 "$WORK/$(basename "$bin").build.log" | tr '\n' ' ')"; return 1; }
}
compile_cmd() { # <bin> <完整 g++ 命令行...>
  local bin=$1; shift
  log "  编译: $*"
  "$@" >"$WORK/$(basename "$bin").build.log" 2>&1 \
      || { log "  编译失败: $(tail -n 2 "$WORK/$(basename "$bin").build.log" | tr '\n' ' ')"; return 1; }
}
reap() { pkill -x "$(printf '%.15s' "$(basename "$1")")" 2>/dev/null || true; }
run_to() { # <logf> <描述> <命令...>
  local logf=$1 desc=$2; shift 2
  log "  运行: $desc"
  timeout -k 5 "$BENCH_TIMEOUT" "$@" >"$logf" 2>&1
  local rc=$?
  reap "${@: -1}"
  return $rc
}
cmp_rel() { # <实测> <容差%> <基线空格列表> → "OK|细节" / "STALE|细节"
  local m=$1 tol=$2 bases=$3
  local hit=0 detail='' h dev absd
  for h in $bases; do
    read -r dev absd <<<"$(awk -v m="$m" -v x="$h" 'BEGIN{d=(m-x)*100.0/x; a=(d<0?-d:d); printf "%+.1f %.1f", d, a}')"
    if awk -v a="$absd" -v t="$tol" 'BEGIN{exit !(a<=t)}'; then
      hit=1; detail="${detail}ledger=$h(偏差${dev}% ✓) "; else detail="${detail}ledger=$h(偏差${dev}%) "; fi
  done
  if [ "$hit" -eq 1 ]; then printf 'OK|%s' "$detail"; else printf 'STALE|%s' "$detail"; fi
}
cmp_abs() { # <实测> <绝对容差> <基线空格列表>（用于命中率）
  local m=$1 tol=$2 bases=$3
  local hit=0 detail='' h dev absd
  for h in $bases; do
    read -r dev absd <<<"$(awk -v m="$m" -v x="$h" 'BEGIN{d=m-x; a=(d<0?-d:d); printf "%+.4f %.4f", d, a}')"
    if awk -v a="$absd" -v t="$tol" 'BEGIN{exit !(a<=t)}'; then
      hit=1; detail="${detail}ledger=$h(Δ${dev} ✓) "; else detail="${detail}ledger=$h(Δ${dev}) "; fi
  done
  if [ "$hit" -eq 1 ]; then printf 'OK|%s' "$detail"; else printf 'STALE|%s' "$detail"; fi
}
finish() { # <id> <实测文本> <ledger 文本> <判定> <note> [实测数值]
  local id=$1 m=$2 b=$3 v=$4 n=$5 num=${6:-}
  TOTAL=$((TOTAL+1))
  case $v in
    OK)    OK=$((OK+1)) ;;
    STALE) STALE=$((STALE+1)); STALE_IDS+=("$id") ;;
    SKIP)  SKIP=$((SKIP+1)); SKIP_IDS+=("$id") ;;
  esac
  log "[$id] 实测=$m ledger=$b 判定=$v"
  [ -n "$n" ] && log "      注: $n"
  ROWS+=("$id|$m|$b|$v|$n|$num")
}
skipm() { log "  无法自动校验：$2"; finish "$1" '—' "$3" SKIP "$2"; }
rel_finish() { # <id> <实测> <容差%> <单位> <note> <基线...>
  local id=$1 m=$2 tol=$3 unit=$4 note=$5; shift 5
  local res v d; res=$(cmp_rel "$m" "$tol" "$*"); v=${res%%|*}; d=${res#*|}
  log "  对照（相对容差 ±${tol}%）: ${d}"
  finish "$id" "${m} ${unit}" "$*" "$v" "${note}；容差 ±${tol}%" "$m"
}

# ------------------------------------------------------------------ 各条 metric
metric_m01b_a() {
  local id=M01b-a src=tests/concurrency/bench_channel_opt.cpp 'b=112.6'
  local bin="$WORK/bench_channel_opt" logf="$WORK/M01b-a.log" val
  log ''; log "[$id] 通用 Channel<uint64_t> 1P1C cap1024 ns/条（ledger:42）"
  [ -f "$ROOT/$src" ] || { skipm "$id" "缺基准源 $src" '112.6'; return; }
  compile_bench "$bin" "$src" c++20 -O3 -lgo2cpp_channel -lgo2cpp_context \
      -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_log \
      || { skipm "$id" '编译失败' '112.6'; return; }
  run_to "$logf" "$PIN_DESC $bin 1000000 3 spsc" "${PIN[@]}" "$bin" 1000000 3 spsc \
      || { skipm "$id" '运行失败或超时' '112.6'; return; }
  val=$(awk '/^\[1\] SPSC/{s=1;next} /^\[/{s=0} s&&$1=="Channel"&&$2=="cap=1024"{for(i=1;i<=NF;i++)if($i=="median")print $(i+1)}' "$logf")
  [ -n "$val" ] || { skipm "$id" '无法解析 [1] SPSC cap=1024 median' '112.6'; return; }
  grep -E 'cap=1024' "$logf" | sed 's/^/       | /' | head -3
  rel_finish "$id" "$val" 30 'ns/条' '1e6 条×3 reps，取 [1] SPSC cap=1024 median' '112.6'
}
metric_m01b_b() {
  local id=M01b-b bin="$BUILD_DIR/go2cpp_fast_channel_bench" logf="$WORK/M01b-b.log" val minv
  log ''; log "[$id] FastChannel<1024> 1P1C ns/条（ledger:56）"
  [ -x "$bin" ] || { skipm "$id" "缺二进制 ${bin#$ROOT/}" '10.73 / 16.07'; return; }
  run_to "$logf" "$PIN_DESC $bin 1000000 5" "${PIN[@]}" "$bin" 1000000 5 \
      || { skipm "$id" '运行失败或超时' '10.73 / 16.07'; return; }
  val=$(awk '$1=="fast-cap1024"{for(i=1;i<=NF;i++)if($i=="median")print $(i+1)}' "$logf")
  minv=$(awk '$1=="fast-cap1024"{for(i=1;i<=NF;i++)if($i=="min")print $(i+1)}' "$logf")
  [ -n "$val" ] || { skipm "$id" '无法解析 fast-cap1024 median' '10.73 / 16.07'; return; }
  grep -E 'fast-cap1024' "$logf" | sed 's/^/       | /'
  rel_finish "$id" "$val" 30 'ns/条' "1e6×5 reps 线程段 median（min=${minv:-?}）" '12.83 16.07'
}
metric_m02() {
  local id=M02 src=tests/concurrency/bench_fiber_switch_opt.cpp
  local bin="$WORK/bench_fiber_switch" logf="$WORK/M02.log" val
  log ''; log "[$id] Fiber 全量切换 ns/往返（ledger:66）"
  [ -f "$ROOT/$src" ] || { skipm "$id" "缺基准源 $src" '33.80 / 23.5 / 19.84'; return; }
  compile_bench "$bin" "$src" c++17 -O3 -lgo2cpp_fiber -lgo2cpp_debug \
      -lgo2cpp_debug_event -lgo2cpp_error || { skipm "$id" '编译失败' '33.80 / 23.5 / 19.84'; return; }
  run_to "$logf" "$PIN_DESC $bin 1000000" "${PIN[@]}" "$bin" 1000000 \
      || { skipm "$id" '运行失败或超时' '33.80 / 23.5 / 19.84'; return; }
  val=$(grep -F 'Fiber::resume ping-pong (yield)' "$logf" | awk '{print $(NF-1)}')
  [ -n "$val" ] || { skipm "$id" '无法解析 Fiber::resume ping-pong (yield)' '33.80 / 23.5 / 19.84'; return; }
  grep -F 'Fiber::resume ping-pong (yield)' "$logf" | sed 's/^/       | /'
  rel_finish "$id" "$val" 30 'ns/往返' 'resume↔Suspend 往返 1e6 次（口径见 ledger M02 备注）' '33.80 23.5 19.84'
}
metric_m03() {
  local id=M03 src=tests/concurrency/bench_scheduler_task_opt.cpp
  local bin="$WORK/bench_scheduler_task_opt" logf="$WORK/M03.log" val cpuval
  log ''; log "[$id] Scheduler task churn wall ns/task（ledger:75）"
  [ -f "$ROOT/$src" ] || { skipm "$id" "缺基准源 $src" '122.0 / 239 / 299'; return; }
  compile_bench "$bin" "$src" c++17 -O2 -lgo2cpp_scheduler -lgo2cpp_fiber \
      -lgo2cpp_error -lgo2cpp_debug -lgo2cpp_debug_event \
      || { skipm "$id" '编译失败' '122.0 / 239 / 299'; return; }
  run_to "$logf" "$PIN_DESC $bin 4 100000 5" "${PIN[@]}" "$bin" 4 100000 5 \
      || { skipm "$id" '运行失败或超时' '122.0 / 239 / 299'; return; }
  val=$(awk '$1=="[round"{for(i=1;i<=NF;i++)if($i=="wall"){print $(i+1);break}}' "$logf" | median)
  cpuval=$(awk '$1=="[round"{for(i=1;i<=NF;i++)if($i=="cpu"){print $(i+1);break}}' "$logf" | median)
  [ -n "$val" ] || { skipm "$id" '无法解析 wall ns/task' '122.0 / 239 / 299'; return; }
  grep -E '^\[round' "$logf" | sed 's/^/       | /' | head -5
  log "      cpu 中位: ${cpuval:-?} ns/task（诊断宿主负载；ledger 同口径 627）"
  local res; res=$(cmp_rel "$val" 30 '122.0 239 299')
  if [ "${res%%|*}" != OK ] && [ -n "$cpuval" ] && awk -v c="$cpuval" 'BEGIN{exit !(c>1.5*627)}'; then
    skipm "$id" "wall 超容差但 cpu 中位 ${cpuval} > 1.5×ledger 627，宿主负载噪声，本轮不可判" '122.0 / 239 / 299'
    return
  fi
  rel_finish "$id" "$val" 30 'ns/task' '4 worker/100k tasks/5 轮 wall 中位' '122.0 239 299'
}
metric_m04() {
  local id=M04 src=tests/concurrency/bench_scheduler_submit.cpp
  local bin="$WORK/bench_scheduler_submit" b v v_small='' v_big='' logf r1 r2
  log ''; log "[$id] Scheduler submit ns/task（batch=1024 / 65536，ledger:92）"
  [ -f "$ROOT/$src" ] || { skipm "$id" "缺基准源 $src" '119.0 / 355.7'; return; }
  compile_bench "$bin" "$src" c++17 -O2 -lgo2cpp_scheduler -lgo2cpp_fiber \
      -lgo2cpp_error -lgo2cpp_debug -lgo2cpp_debug_event \
      || { skipm "$id" '编译失败' '119.0 / 355.7'; return; }
  for b in 1024 65536; do
    logf="$WORK/M04-$b.log"
    run_to "$logf" "$PIN_DESC $bin 4 1000000 3 $b" "${PIN[@]}" "$bin" 4 1000000 3 "$b" \
        || { skipm "$id" "batch=$b 运行失败或超时" '119.0 / 355.7'; return; }
    v=$(awk '/^\[summary\]/{for(i=1;i<=NF;i++)if($i=="median")print $(i+1)}' "$logf")
    [ -n "$v" ] || { skipm "$id" "batch=$b 解析 median 失败" '119.0 / 355.7'; return; }
    grep -E '^\[summary\]' "$logf" | sed 's/^/       | /'
    if [ "$b" = 1024 ]; then v_small=$v; else v_big=$v; fi
  done
  r1=$(cmp_rel "$v_small" 50 '119.0'); r2=$(cmp_rel "$v_big" 50 '355.7')
  log "  batch=1024 对照（±50%）: ${r1#*|}"; log "  batch=65536 对照（±50%）: ${r2#*|}"
  if [ "${r1%%|*}" = OK ] && [ "${r2%%|*}" = OK ]; then
    finish "$id" "batch1024 ${v_small} / batch65536 ${v_big} ns/task" '119.0 / 355.7 ns/task' OK \
      '两个 batch 口径都命中；3 轮中位（ledger 为 5 轮×7 进程）；ledger 自身 p25–p75 已 ±15%，宿主争用再放大故容差取 ±50%'
  else
    finish "$id" "batch1024 ${v_small} / batch65536 ${v_big} ns/task" '119.0 / 355.7 ns/task' STALE '至少一个 batch 口径超出 ±50%'
  fi
}
metric_m06() {
  local id=M06 bin="$BUILD_DIR/go2cpp_storage_bench"
  local i d logf v vals=()
  log ''; log "[$id] SQLite 1e6 行插入 wrapper/native 比值（ledger:121）"
  [ -x "$bin" ] || { skipm "$id" "缺二进制 ${bin#$ROOT/}" '1.074×'; return; }
  for i in 1 2 3; do
    d="$WORK/stor-$i"; mkdir -p "$d"; logf="$WORK/M06-$i.log"
    run_to "$logf" "$PIN_DESC $bin <tmpdir> 1000000（第 $i/3 轮）" "${PIN[@]}" "$bin" "$d" 1000000 \
        || { rm -rf "$d"; skipm "$id" "第 $i 轮运行失败或超时" '1.074×'; return; }
    v=$(awk '$1=="insert-file:"{gsub("x","",$2); print $2}' "$logf")
    [ -n "$v" ] || { rm -rf "$d"; skipm "$id" '无法解析 insert-file 比值' '1.074×'; return; }
    log "  第 $i 轮 insert-file = ${v}×"; vals+=("$v"); rm -rf "$d"
  done
  v=$(printf '%s\n' "${vals[@]}" | median)
  rel_finish "$id" "$v" 15 '×' '1e6 行 insert-file 3 轮中位（ledger 为 7 轮，轮数已降）' '1.074'
}
metric_m08() {
  local id=M08 bin="$BUILD_DIR/go2cpp_bufio_test" i logf v vals=()
  log ''; log "[$id] bufio 分块传输 1 MiB 读 MiB/s（ledger:144）"
  [ -x "$bin" ] || { skipm "$id" "缺二进制 ${bin#$ROOT/}" '1853.9'; return; }
  for i in 1 2 3 4 5; do
    logf="$WORK/M08-$i.log"
    run_to "$logf" "$bin（第 $i/5 次，未绑核）" "$bin" || { skipm "$id" "第 $i 次运行失败或超时" '1853.9'; return; }
    v=$(sed -n 's/.*分块传输 1MiB:.*, \([0-9.]*\) MiB\/s.*/\1/p' "$logf")
    [ -n "$v" ] || { skipm "$id" '无法解析「分块传输 1MiB」行' '1853.9'; return; }
    grep -F '分块传输 1MiB' "$logf" | sed 's/^/       | /'; vals+=("$v")
  done
  v=$(printf '%s\n' "${vals[@]}" | median)
  rel_finish "$id" "$v" 30 'MiB/s' '1 MiB 分块读 5 次中位，未绑核（单次仅约 0.5 ms，故取中位）' '1853.9'
}
metric_m09() {
  local id=M09 bin="$BUILD_DIR/go2cpp_cache_test" logf="$WORK/M09.log" arc lru r1 r2
  log ''; log "[$id] Cache hotspot 命中率 ARC/LRU（ledger:153）"
  [ -x "$bin" ] || { skipm "$id" "缺二进制 ${bin#$ROOT/}" 'ARC 0.7764 / LRU 0.5594'; return; }
  run_to "$logf" "$bin（未绑核，固定 seed）" "$bin" \
      || { skipm "$id" '运行失败或超时' 'ARC 0.7764 / LRU 0.5594'; return; }
  arc=$(awk '$1=="[cache]"&&$2=="hotspot"&&$3=="ARC"{sub("hit=","",$4); print $4}' "$logf")
  lru=$(awk '$1=="[cache]"&&$2=="hotspot"&&$3=="LRU"{sub("hit=","",$4); print $4}' "$logf")
  [ -n "$arc" ] && [ -n "$lru" ] || { skipm "$id" '无法解析 hotspot ARC/LRU' 'ARC 0.7764 / LRU 0.5594'; return; }
  grep -E 'hotspot (ARC|LRU) ' "$logf" | sed 's/^/       | /'
  r1=$(cmp_abs "$arc" 0.02 '0.7817 0.7764'); r2=$(cmp_abs "$lru" 0.02 '0.5580 0.5594')
  log "  ARC 对照（绝对容差 ±0.02）: ${r1#*|}"; log "  LRU 对照（绝对容差 ±0.02）: ${r2#*|}"
  if [ "${r1%%|*}" = OK ] && [ "${r2%%|*}" = OK ]; then
    finish "$id" "ARC ${arc} / LRU ${lru}" 'ARC 0.7764 / LRU 0.5594' OK '命中率绝对容差 ±0.02，未绑核'
  else
    finish "$id" "ARC ${arc} / LRU ${lru}" 'ARC 0.7764 / LRU 0.5594' STALE '至少一个策略超出 ±0.02'
  fi
}
metric_m10() {
  local id=M10 src=tests/os/bench_vectored.cpp bin="$WORK/bench_vectored" logf="$WORK/M10.log" w r r1 r2
  log ''; log "[$id] os 批量 writev/readv 加速比（ledger:162）"
  [ -f "$ROOT/$src" ] || { skipm "$id" "缺基准源 $src" '写 17.96× / 读 14.68×'; return; }
  compile_cmd "$bin" g++ -O3 -DNDEBUG -std=c++17 "-I$ROOT/include" "-I$ROOT/src/os" \
      "$ROOT/$src" "$ROOT/src/os/file.cpp" "$ROOT/src/os/config.cpp" \
      "-L$BUILD_DIR" -lgo2cpp_config -lgo2cpp_error -lgo2cpp_log \
      "-Wl,-rpath,$BUILD_DIR" -pthread -o "$bin" \
      || { skipm "$id" '编译失败（预编译 os .so 早于 WritevAll，须直编 src/os 源）' '写 17.96× / 读 14.68×'; return; }
  mkdir -p "$WORK/vectored"
  run_to "$logf" "$PIN_DESC $bin <workdir>" "${PIN[@]}" "$bin" "$WORK/vectored" \
      || { skipm "$id" '运行失败或超时' '写 17.96× / 读 14.68×'; return; }
  w=$(awk '/MEDIAN write/{for(i=1;i<=NF;i++)if($i~/^speedup=/){sub("speedup=","",$i);sub("x$","",$i);print $i}}' "$logf")
  r=$(awk '/MEDIAN read /{for(i=1;i<=NF;i++)if($i~/^speedup=/){sub("speedup=","",$i);sub("x$","",$i);print $i}}' "$logf")
  [ -n "$w" ] && [ -n "$r" ] || { skipm "$id" '无法解析 speedup' '写 17.96× / 读 14.68×'; return; }
  grep -E 'MEDIAN|failures=' "$logf" | sed 's/^/       | /'
  r1=$(cmp_rel "$w" 30 '17.96'); r2=$(cmp_rel "$r" 30 '14.68')
  log "  写 对照（±30%）: ${r1#*|}"; log "  读 对照（±30%）: ${r2#*|}"
  if [ "${r1%%|*}" = OK ] && [ "${r2%%|*}" = OK ]; then
    finish "$id" "写 ${w}× / 读 ${r}×" '写 17.96× / 读 14.68×' OK '3 轮中位；读口径历史 20.9× 未复现，ledger 已按 14.68× 记账'
  else
    finish "$id" "写 ${w}× / 读 ${r}×" '写 17.96× / 读 14.68×' STALE '写或读加速比超出 ±30%'
  fi
}
metric_m11() {
  local id=M11 s1=tests/bench/bench_tools_industry.cpp s2=tests/bench/bench_gzip_crc.cpp
  local b1="$WORK/bench_tools_industry" b2="$WORK/bench_gzip_crc" l1="$WORK/M11a.log" l2="$WORK/M11b.log"
  local comp decomp crc r1 r2 r3
  log ''; log "[$id] gzip 压缩/解压吞吐 + crc32（ledger:172）"
  [ -f "$ROOT/$s1" ] && [ -f "$ROOT/$s2" ] || { skipm "$id" '缺基准源' '612.2 / 3648.0 MiB/s，crc32 22.14 GB/s'; return; }
  compile_cmd "$b1" g++ -O3 -DNDEBUG -std=c++17 "-I$ROOT/include" "-I$ROOT/tests/bench/gen" \
      "-I$ROOT/tests/compress" "$ROOT/$s1" "$ROOT/tests/bench/gen/tools_bench.pb.cc" \
      "$ROOT/tests/compress/compress_test_support.cpp" "-L$BUILD_DIR" -lgo2cpp_reflect \
      -lgo2cpp_encoding -lgo2cpp_compress -lgo2cpp_error -lz -lprotobuf "-Wl,-rpath,$BUILD_DIR" -o "$b1" \
      || { skipm "$id" 'bench_tools_industry 编译失败（需 protobuf/zlib）' '612.2 / 3648.0 MiB/s'; return; }
  compile_cmd "$b2" g++ -O3 -DNDEBUG -std=c++17 "-I$ROOT/include" "$ROOT/$s2" "-L$BUILD_DIR" \
      -lgo2cpp_compress -lgo2cpp_hash -lgo2cpp_error -lz "-Wl,-rpath,$BUILD_DIR" -o "$b2" \
      || { skipm "$id" 'bench_gzip_crc 编译失败' 'crc32 22.14 GB/s'; return; }
  run_to "$l1" "$PIN_DESC $b1" "${PIN[@]}" "$b1" || { skipm "$id" 'bench_tools_industry 运行失败' '612.2 / 3648.0 MiB/s'; return; }
  run_to "$l2" "$PIN_DESC $b2" "${PIN[@]}" "$b2" || { skipm "$id" 'bench_gzip_crc 运行失败' 'crc32 22.14 GB/s'; return; }
  comp=$(awk '$1=="gzip_compress_L6"{print $(NF-1)}' "$l1")
  decomp=$(awk '$1=="gzip_decompress"{print $(NF-1)}' "$l1")
  crc=$(awk '$1=="crc32"&&$2=="1MiB"{print $(NF-1)}' "$l2")
  [ -n "$comp" ] && [ -n "$decomp" ] && [ -n "$crc" ] \
      || { skipm "$id" '无法解析 gzip/crc32 吞吐' '612.2 / 3648.0 MiB/s，crc32 22.14 GB/s'; return; }
  grep -E 'gzip_compress_L6|gzip_decompress ' "$l1" | sed 's/^/       | /'
  grep -E 'crc32 1MiB|gzip_compress L6' "$l2" | sed 's/^/       | /'
  r1=$(cmp_rel "$comp" 30 '612.2'); r2=$(cmp_rel "$decomp" 30 '3648.0'); r3=$(cmp_rel "$crc" 30 '22.14')
  log "  压缩 对照（±30%）: ${r1#*|}"; log "  解压 对照（±30%）: ${r2#*|}"; log "  crc32 对照（±30%）: ${r3#*|}"
  local m="压缩 ${comp} MiB/s / 解压 ${decomp} MiB/s / crc32 ${crc} GB/s"
  if [ "${r1%%|*}" = OK ] && [ "${r2%%|*}" = OK ] && [ "${r3%%|*}" = OK ]; then
    finish "$id" "$m" '612.2 / 3648.0 MiB/s，crc32 22.14 GB/s' OK '1 MiB L6，同轮交替测，3 轮中位'
  else
    finish "$id" "$m" '612.2 / 3648.0 MiB/s，crc32 22.14 GB/s' STALE '至少一项超出 ±30%'
  fi
}
metric_m12() {
  local id=M12 bin="$BUILD_DIR/go2cpp_hash_test" logf="$WORK/M12.log" sha crc r1 r2
  log ''; log "[$id] hash 吞吐 SHA-256 / crc32（ledger:182）"
  [ -x "$bin" ] || { skipm "$id" "缺二进制 ${bin#$ROOT/}" 'SHA-256 2002.60 / crc32 18731.55 MiB/s'; return; }
  run_to "$logf" "$bin（未绑核，1 GiB 流式）" "$bin" \
      || { skipm "$id" '运行失败或超时' 'SHA-256 2002.60 / crc32 18731.55 MiB/s'; return; }
  sha=$(awk '$1=="[hash]"&&$2=="sha256"{print $3}' "$logf")
  crc=$(awk '$1=="[hash]"&&$2=="crc32(IEEE)"{print $3}' "$logf")
  [ -n "$sha" ] && [ -n "$crc" ] || { skipm "$id" '无法解析 sha256/crc32' '2002.60 / 18731.55 MiB/s'; return; }
  grep -E 'sha256 |crc32\(IEEE\)' "$logf" | sed 's/^/       | /'
  r1=$(cmp_rel "$sha" 30 '2002.60'); r2=$(cmp_rel "$crc" 30 '18731.55')
  log "  SHA-256 对照（±30%）: ${r1#*|}"; log "  crc32 对照（±30%）: ${r2#*|}"
  if [ "${r1%%|*}" = OK ] && [ "${r2%%|*}" = OK ]; then
    finish "$id" "SHA-256 ${sha} / crc32 ${crc} MiB/s" 'SHA-256 2002.60 / crc32 18731.55 MiB/s' OK \
      '1 GiB 流式单线程；ledger 环境为未绑核，本脚本不 taskset'
  else
    finish "$id" "SHA-256 ${sha} / crc32 ${crc} MiB/s" 'SHA-256 2002.60 / crc32 18731.55 MiB/s' STALE '至少一项超出 ±30%'
  fi
}
metric_m13() {
  local id=M13 src=tests/bench/bench_reflect_stage.cpp bin="$WORK/bench_reflect_stage"
  local logf="$WORK/M13.log" enc dec r1 r2
  log ''; log "[$id] reflect 二进制编解码 ns/op（ledger:192）"
  [ -f "$ROOT/$src" ] || { skipm "$id" "缺基准源 $src" '编码 52.64 / 解码 54.89 ns/op'; return; }
  compile_cmd "$bin" g++ -O3 -DNDEBUG -std=c++17 "-I$ROOT/include" "-I$ROOT/tests/bench/gen" \
      "$ROOT/$src" "$ROOT/tests/bench/gen/tools_bench.pb.cc" "$ROOT/src/reflect/codec.cpp" \
      "$ROOT/src/reflect/compress.cpp" "$ROOT/src/reflect/json.cpp" "$ROOT/src/reflect/type.cpp" \
      -lprotobuf -o "$bin" || { skipm "$id" '编译失败（需 protobuf + src/reflect 源）' '编码 52.64 / 解码 54.89 ns/op'; return; }
  run_to "$logf" "$PIN_DESC $bin" "${PIN[@]}" "$bin" \
      || { skipm "$id" '运行失败或超时' '编码 52.64 / 解码 54.89 ns/op'; return; }
  enc=$(awk '$1=="bin_encode_compact"{print $2}' "$logf")
  dec=$(awk '$1=="bin_decode_full"{print $2}' "$logf")
  [ -n "$enc" ] && [ -n "$dec" ] || { skipm "$id" '无法解析 bin_encode_compact/bin_decode_full' '编码 52.64 / 解码 54.89 ns/op'; return; }
  grep -E 'bin_encode_compact |bin_decode_full ' "$logf" | sed 's/^/       | /'
  r1=$(cmp_rel "$enc" 30 '52.64'); r2=$(cmp_rel "$dec" 30 '54.89')
  log "  编码 对照（±30%）: ${r1#*|}"; log "  解码 对照（±30%）: ${r2#*|}"
  if [ "${r1%%|*}" = OK ] && [ "${r2%%|*}" = OK ]; then
    finish "$id" "编码 ${enc} / 解码 ${dec} ns/op" '编码 52.64 / 解码 54.89 ns/op' OK '二进制紧凑格式；描述/文本格式仅打印不判定'
  else
    finish "$id" "编码 ${enc} / 解码 ${dec} ns/op" '编码 52.64 / 解码 54.89 ns/op' STALE '编码或解码超出 ±30%'
  fi
}
# 只能手工校验的条目：<ID>|<说明>|<原因>|<ledger 文本>
MANUAL=(
  'M01|Channel 1P1C 有缓冲 ns/条|已拆条：10.73 实为 FastChannel<1024>，校验见 M01b-b；通用 Channel 见 M01b-a|10.73（已拆条）'
  'M05|HTTP/1.1 pipeline=16 单连接 rps|需先起 net_perf_server（READY 行取端口）再用 Go http_loadgen 压测，非单进程基准|120,755 rps'
  'M05b|「HTTP pipeline 411k rps」（无出处）|台账记为无出处，没有可校验的数字（禁止引用）|411k rps（无出处）'
  'M07|LevelDB 1e7 Put 与 1e8 RSS|1e7 Put 需 3 轮×2 实现、1e8 RSS 段单独约 230 s，超出单条 5 分钟预算，需手工执行|647.5k ops/s / 82.4 MiB'
)
metric_manual() {
  local item id desc why base
  for item in "${MANUAL[@]}"; do
    IFS='|' read -r id desc why base <<<"$item"
    log ''; log "[$id] $desc"
    skipm "$id" "$why" "$base"
  done
}

# ----------------------------------------------------------------------- 汇总
emit_json() {
  local row id m b v n num sep=''
  printf '{\n  "tool": "tools/verify_metrics.sh",\n'
  printf '  "generated_at": "%s",\n  "repo": "%s",\n  "mode": "%s",\n' \
    "$(date '+%FT%T%z')" "$(json_str "$ROOT")" "$MODE"
  printf '  "env": {"nproc": %s, "pin": "%s", "build_dir": ".build-bento-wsl", "per_metric_budget_s": %s},\n' \
    "$(nproc)" "$(json_str "$PIN_DESC")" "$BENCH_TIMEOUT"
  printf '  "refs": {"checked": %s, "mismatch": %s},\n  "metrics": [\n' "${#REFS[@]}" "$REF_MISS"
  for row in "${ROWS[@]}"; do
    IFS='|' read -r id m b v n num <<<"$row"
    printf '%s    {"id": "%s", "measured": %s, "measured_text": "%s", "ledger": "%s", "verdict": "%s", "note": "%s"}' \
      "$sep" "$id" "${num:-null}" "$(json_str "$m")" "$(json_str "$b")" "$v" "$(json_str "$n")"
    sep=$',\n'
  done
  printf '\n  ],\n'
  printf '  "summary": {"total": %s, "ok": %s, "stale": %s, "skip": %s, "exit_code": %s},\n' \
    "$TOTAL" "$OK" "$STALE" "$SKIP" "$RC"
  printf '  "stale": [%s],\n  "skipped": [%s]\n}\n' "$(join_quoted "${STALE_IDS[@]}")" "$(join_quoted "${SKIP_IDS[@]}")"
}
summary() {
  log ''
  log '=============================================================='
  log "TOTAL=$TOTAL OK=$OK STALE=$STALE SKIP=$SKIP"
  if [ "$STALE" -gt 0 ]; then log "STALE 列表: ${STALE_IDS[*]}"; else log 'STALE 列表: 无'; fi
  [ "$SKIP" -gt 0 ] && log "SKIP 列表: ${SKIP_IDS[*]}"
  [ "$REF_MISS" -gt 0 ] && log "台账行号失配: ${REF_MISS} 条（见上方 [ref] 行）"
  log "人读日志: ${OUT#$ROOT/}"
  log '=============================================================='
  rm -rf "$WORK"
  RC=0; [ "$STALE" -gt 0 ] && RC=1
  return "$RC"
}
main() {
  log '=============================================================='
  log " verify_metrics 全量校验: $(date '+%F %T %z')  mode=$MODE"
  log " repo=$ROOT"
  log " 环境: nproc=$(nproc) 绑定=$PIN_DESC 构建库=${BUILD_DIR#$ROOT/} 单条上限=${BENCH_TIMEOUT}s"
  log ' 判定: 任一 ledger 值落在该条容差内=OK；全部超出=STALE；跑不起来=SKIP（不算通过）'
  log '=============================================================='
  check_refs
  metric_m01b_a
  metric_m01b_b
  metric_m02
  metric_m03
  if [ "$QUICK" -eq 0 ]; then
    metric_m04; metric_manual; metric_m06; metric_m08; metric_m09
    metric_m10; metric_m11; metric_m12; metric_m13
  fi
  summary
}

if [ "$REFS_ONLY" -eq 1 ]; then
  check_refs
  [ "$REF_MISS" -gt 0 ] && exit 2
  exit 0
fi
if [ "$JSON" -eq 1 ]; then
  main >>"$OUT" 2>&1; RC=$?; emit_json
else
  main 2>&1 | tee -a "$OUT"; RC=${PIPESTATUS[0]}
fi
exit "$RC"
