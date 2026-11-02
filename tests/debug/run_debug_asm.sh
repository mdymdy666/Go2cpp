#!/usr/bin/env bash
# debug/replay 指令级基准驱动：编译 + 运行，结果写到 $OUT/*.tsv。
#
# 前提：已有 $BUILD/on（GO2CPP_DEBUG_DUMP=ON + GO2CPP_REPLAY=ON）与
#       $BUILD/off（全 OFF）两个 Release 构建目录。
#
# 用法：
#   bash tests/debug/run_debug_asm.sh             # 全部（asm + replay）
#   MODE=asm    bash tests/debug/run_debug_asm.sh # 只跑事件环
#   MODE=replay bash tests/debug/run_debug_asm.sh # 只跑 replay
#
# 环境变量：
#   BUILD  独立构建目录（默认 /tmp/build-debug）
#   OUT    结果目录（默认 /tmp/debug-asm）
#   N      1e6 事件规模（默认 1000000）
#   ITERS  dump/write 重复次数（默认 200/100）
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD:-/tmp/build-debug}"
OUT="${OUT:-/tmp/debug-asm}"
N="${N:-1000000}"
ITERS="${ITERS:-200}"
MODE="${MODE:-all}"
BIN="$BUILD/bin"
OBJ="$BUILD/obj"
CXX="${CXX:-g++}"

mkdir -p "$BIN" "$OBJ" "$OUT" || exit 2

FLAGS="-std=c++17 -O3 -DNDEBUG -pthread -I $ROOT/include"

build_asm() {
    echo "[build] asm_on <- $BUILD/on"
    # shellcheck disable=SC2086
    $CXX $FLAGS \
        -DGO2CPP_DEBUG_DUMP=1 \
        -DGO2CPP_DEBUG_EVENT_CAPACITY=256 \
        -DGO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY=64 \
        "$ROOT/tests/debug/debug_asm_bench.cpp" \
        -L "$BUILD/on" -lgo2cpp_debug_event -Wl,-rpath,"$BUILD/on" \
        -o "$BIN/asm_on" || return 3
    echo "[build] asm_off <- $BUILD/off"
    # shellcheck disable=SC2086
    $CXX $FLAGS "$ROOT/tests/debug/debug_asm_bench.cpp" \
        -L "$BUILD/off" -lgo2cpp_debug_event -Wl,-rpath,"$BUILD/off" \
        -o "$BIN/asm_off" || return 3
    return 0
}

build_replay() {
    echo "[build] replay_core objects"
    local f
    for f in format recorder replay_engine timer split auto_entry; do
        "$CXX" -std=c++17 -O3 -DNDEBUG -DGO2CPP_REPLAY=1 \
            -I "$ROOT/include" -I "$ROOT/tools/replay/include" \
            -c "$ROOT/tools/replay/src/$f.cpp" -o "$OBJ/$f.o" || return 3
    done
    echo "[build] replay_bench"
    # shellcheck disable=SC2086
    $CXX $FLAGS -DGO2CPP_REPLAY=1 -I "$ROOT/tools/replay/include" \
        "$ROOT/tests/debug/debug_replay_bench.cpp" "$OBJ"/*.o \
        -o "$BIN/replay_bench" || return 3
    return 0
}

FAILED=0
run() {
    local tag="$1"
    shift
    local out="$OUT/$tag.tsv"
    echo "[run] $tag"
    "$@" >"$out" 2>"$OUT/$tag.err"
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "[FAIL] $tag rc=$rc"
        tail -20 "$OUT/$tag.err"
        FAILED=1
        return $rc
    fi
    head -1 "$out"
    return 0
}

if [ "$MODE" = "all" ] || [ "$MODE" = "asm" ]; then
    build_asm || exit 3
    run asm_on_emit  "$BIN/asm_on"  emit  --n="$N" --threads=4
    run asm_on_dump  "$BIN/asm_on"  dump  --threads=64 --iters="$ITERS"
    run asm_on_write "$BIN/asm_on"  write --iters=100 --path=/tmp/debug_asm_dump.txt
    run asm_off_off  "$BIN/asm_off" off
fi

if [ "$MODE" = "all" ] || [ "$MODE" = "replay" ]; then
    build_replay || exit 3
    REC_PATH="$OUT/replay_${N}.g2cr"
    run replay_record "$BIN/replay_bench" record --n="$N" --path="$REC_PATH"
    run replay_decode "$BIN/replay_bench" decode --n="$N" --path="$REC_PATH"
    run replay_replay "$BIN/replay_bench" replay --n="$N" --path="$REC_PATH"
    run replay_bytes  "$BIN/replay_bench" bytes  --n="$N" --path="$REC_PATH"
    run replay_iobase "$BIN/replay_bench" iobase --path="$REC_PATH"
    # 记录文件保留在 $OUT 供人工复核；大于 64MB 时删除，避免占空间。
    if [ -f "$REC_PATH" ] && [ "$(stat -c %s "$REC_PATH")" -gt 67108864 ]; then
        rm -f "$REC_PATH" "$REC_PATH.a" "$REC_PATH.b"
    fi
fi

# 汇总：每行 <tag>\t<key>\t<value>
: >"$OUT/all.tsv"
for file in "$OUT"/*.tsv; do
    [ "$(basename "$file")" = "all.tsv" ] && continue
    tag="$(basename "$file" .tsv)"
    awk -v tag="$tag" -F '\t' '{printf "%s\t%s\t%s\n", tag, $1, $2}' "$file" \
        >>"$OUT/all.tsv"
done
echo "[done] 汇总: $OUT/all.tsv"
if [ "$FAILED" -ne 0 ]; then
    echo "[FAILED] 有子命令失败"
    exit 1
fi
exit 0
