#!/usr/bin/env bash
# 内存基准入口：编译 + 逐配置运行 + 汇总（TSV/JSON/Markdown）。
#
# 为什么每个配置单独起进程：
#   Fiber 完成后栈会进入 StackCache 复用，glibc 的 malloc arena 也会保留
#   刚释放的堆；在同一个进程里连续测多组配置会互相污染。每个 (mode, stack, n)
#   都由独立进程执行，base 采样发生在 main() 最前面，进程退出即全部归还。
#
# 用法：
#   bash tests/bench/run_memory_bench.sh                 # 全矩阵（约数分钟）
#   QUICK=1 bash tests/bench/run_memory_bench.sh         # 小矩阵，供 CI 可选步骤
#   MODES="thread fiber-direct" STACKS="64" NS="1000" \
#       bash tests/bench/run_memory_bench.sh             # 自定义子集
#
# 环境变量：
#   BUILD_DIR        项目构建目录（默认自动探测 build-work/build-release/...）
#   OUT_DIR          结果目录（默认 tests/bench/results）
#   BIN_DIR          编译产物目录（默认 /tmp/go2cpp-memory-bench）
#   MODES/STACKS/NS  覆盖测试矩阵；QUICK=1 且未显式设置时用小矩阵
#   SAMPLES          每阶段采样次数（默认 3，取中位）
#   SETTLE_MS        触页后等待稳定毫秒数（默认 50）
#   TIMEOUT_PER_RUN  单配置 wall timeout 秒（默认 300）
#   GATE_TIMEOUT_MS  程序内部等待线程/任务到齐的超时（默认 120000）
#   ARENA_MAX        传给程序的 --arena-max（默认 0 不干预；1 = 关闭 glibc
#                    per-thread arena 的 64MiB 虚拟预留，得到纯栈口径）
#   STRICT=1         有非 OK 配置时以 1 退出（默认 0：崩溃也是边界数据）
#
# 退出码：0 完成；2 参数/环境错误；3 编译失败。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BENCH_DIR="$ROOT/tests/bench"
SRC="$BENCH_DIR/fiber_vs_thread_memory.cpp"
SUMMARIZER="$BENCH_DIR/summarize_memory_bench.py"

BIN_DIR="${BIN_DIR:-/tmp/go2cpp-memory-bench}"
OUT_DIR="${OUT_DIR:-$BENCH_DIR/results}"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_DIR="$OUT_DIR/logs-$STAMP"
STATUS="$LOG_DIR/status.tsv"
BIN="$BIN_DIR/fiber_vs_thread_memory"

SAMPLES="${SAMPLES:-3}"
SETTLE_MS="${SETTLE_MS:-50}"
TIMEOUT_PER_RUN="${TIMEOUT_PER_RUN:-300}"
GATE_TIMEOUT_MS="${GATE_TIMEOUT_MS:-120000}"
ARENA_MAX="${ARENA_MAX:-0}"
STRICT="${STRICT:-0}"

QUICK="${QUICK:-0}"
if [ "$QUICK" = "1" ]; then
    MODES="${MODES:-thread fiber-direct}"
    STACKS="${STACKS:-64}"
    NS="${NS:-100 1000}"
else
    MODES="${MODES:-thread fiber-direct fiber-growable fiber-sched mmap}"
    STACKS="${STACKS:-4 64 1024}"
    NS="${NS:-100 1000 5000}"
fi

if [ ! -f "$SRC" ]; then
    echo "找不到基准源码：$SRC" >&2
    exit 2
fi
if [ ! -f "$SUMMARIZER" ]; then
    echo "找不到汇总脚本：$SUMMARIZER" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# 1. 定位构建目录（必须已有 libgo2cpp_fiber.so，本脚本不负责建整个项目）
# ---------------------------------------------------------------------------
BUILD="${BUILD_DIR:-}"
if [ -z "$BUILD" ]; then
    for cand in build-work build-release build-enhanced build-debug; do
        if [ -f "$ROOT/$cand/libgo2cpp_fiber.so" ]; then
            BUILD="$ROOT/$cand"
            break
        fi
    done
fi
if [ -z "$BUILD" ] || [ ! -f "$BUILD/libgo2cpp_fiber.so" ]; then
    cat >&2 <<'EOF'
找不到可用的构建目录（需要 libgo2cpp_fiber.so）。
先构建项目，例如：
    cmake -S . -B build-work -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON
    cmake --build build-work -j
或用 BUILD_DIR=/path/to/build 指定。
EOF
    exit 2
fi

# ---------------------------------------------------------------------------
# 2. 编译（flock 串行化，避免并发 CI 同时写同一产物）
# ---------------------------------------------------------------------------
mkdir -p "$BIN_DIR" "$LOG_DIR" || exit 2
: > "$STATUS"

CXX="${CXX:-g++}"
CXXFLAGS_COMMON=(
    -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic
    -pthread
    -I"$ROOT/include" -I"$ROOT/third_party"
)

LIBS=(
    -Wl,--no-as-needed
    -lgo2cpp_channel -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_context
    -lgo2cpp_sync -lgo2cpp_config -lgo2cpp_log -lgo2cpp_error
    -lgo2cpp_io -lgo2cpp_metrics -lgo2cpp_debug
    -lpthread -ldl -lm
)
# hook 是可选的 interposer；构建里没有时不能硬链 -lgo2cpp_hook。
if [ -f "$BUILD/libgo2cpp_hook.so" ]; then
    LIBS=(-Wl,--no-as-needed -lgo2cpp_hook "${LIBS[@]:1}")
fi

echo "== 编译基准 =="
echo "   构建目录：$BUILD"
echo "   产物：$BIN"
BUILD_LOCK="${TMPDIR:-/tmp}/go2cpp-memory-bench-build.lock"
if ! flock "$BUILD_LOCK" "$CXX" "${CXXFLAGS_COMMON[@]}" "$SRC" -o "$BIN" \
        -Wl,-rpath,"$BUILD" -L"$BUILD" "${LIBS[@]}"; then
    echo "编译失败" >&2
    exit 3
fi
if [ ! -x "$BIN" ]; then
    echo "编译产物不可执行：$BIN" >&2
    exit 3
fi

# ---------------------------------------------------------------------------
# 3. 逐配置运行
# ---------------------------------------------------------------------------
TOTAL=0
OK=0
BAD=0

run_one() {
    local mode="$1" stack="$2" n="$3"
    local log="$LOG_DIR/${mode}_${stack}kb_${n}.tsv"
    local err="$LOG_DIR/${mode}_${stack}kb_${n}.err"
    local t0 t1 rc ms
    t0=$(date +%s%N)
    timeout "$TIMEOUT_PER_RUN" "$BIN" \
        --mode="$mode" --n="$n" --stack-kb="$stack" \
        --samples="$SAMPLES" --settle-ms="$SETTLE_MS" \
        --timeout-ms="$GATE_TIMEOUT_MS" --arena-max="$ARENA_MAX" >"$log" 2>"$err"
    rc=$?
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    printf '%s\t%s\t%s\t%s\t%s\n' "$mode" "$stack" "$n" "$rc" "$ms" >> "$STATUS"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ] && grep -q $'\ttouched\t' "$log" 2>/dev/null; then
        OK=$((OK + 1))
        printf '  %-14s stack=%-5s n=%-5s %6s ms  OK\n' "$mode" "$stack" "$n" "$ms"
    else
        BAD=$((BAD + 1))
        local reason="exit=$rc"
        [ "$rc" -eq 124 ] && reason="timeout(${TIMEOUT_PER_RUN}s)"
        [ "$rc" -ge 128 ] && reason="crash/signal(exit=$rc)"
        printf '  %-14s stack=%-5s n=%-5s %6s ms  %s（边界数据，不伪造）\n' \
            "$mode" "$stack" "$n" "$ms" "$reason"
    fi
}

echo "== 运行矩阵 =="
echo "   modes=$MODES  stacks=$STACKS  n=$NS  samples=$SAMPLES  arena_max=$ARENA_MAX"
for mode in $MODES; do
    for stack in $STACKS; do
        for n in $NS; do
            run_one "$mode" "$stack" "$n"
        done
    done
done

# ---------------------------------------------------------------------------
# 4. 汇总
# ---------------------------------------------------------------------------
echo
echo "== 汇总 =="
JSON="$OUT_DIR/memory_bench_${STAMP}.json"
TSV="$OUT_DIR/memory_bench_${STAMP}.tsv"
MD="$OUT_DIR/summary_${STAMP}.md"
python3 "$SUMMARIZER" \
    --logs "$LOG_DIR" \
    --status "$STATUS" \
    --out-json "$JSON" \
    --out-tsv "$TSV" \
    --out-md "$MD" \
    --build "$BUILD"
SUMMARIZE_RC=$?

echo
echo "== 结果 =="
echo "  配置数：$TOTAL（OK=$OK，非 OK=$BAD）"
echo "  原始日志：$LOG_DIR"
echo "  JSON：$JSON"
echo "  派生 TSV：$TSV"
echo "  Markdown：$MD"

if [ "$SUMMARIZE_RC" -ne 0 ]; then
    echo "汇总失败（rc=$SUMMARIZE_RC）" >&2
    exit "$SUMMARIZE_RC"
fi
if [ "$BAD" -gt 0 ] && [ "$STRICT" = "1" ]; then
    echo "STRICT=1 且存在非 OK 配置" >&2
    exit 1
fi
exit 0
