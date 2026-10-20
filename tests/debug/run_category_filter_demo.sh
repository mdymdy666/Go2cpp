#!/usr/bin/env bash
# 粒度配置（DebugConfig）验收脚本。
#
# 用法：
#   bash tests/debug/run_category_filter_demo.sh [build-dir]
#
# 前置：build-dir 必须是 GO2CPP_DEBUG_DUMP=ON 的构建（默认 build-debug-full）。
#
# 步骤：
#   1. 用 g++ 把 tests/debug/category_filter_demo.cpp 链接到 build 的 .so；
#   2. 默认配置（全开）跑一次，记录各类别计数；
#   3. GO2CPP_DEBUG_CATEGORIES=mutex 再跑一次；
#   4. 断言：全开时 sched/park/primitives 都 > 0；mutex-only 时
#      lifecycle/sched/park/timer/io = 0、primitives > 0、kMutexLock > 0。
set -uo pipefail

BUILD="${1:-build-debug-full}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-g++}"
OUT_DIR="${OUT_DIR:-/tmp/category-filter-demo}"
DEMO_BIN="$BUILD/category_filter_demo"
mkdir -p "$OUT_DIR"

if [[ ! -f "$BUILD/libgo2cpp_debug.so" ]]; then
  echo "FAIL: $BUILD 不是 GO2CPP_DEBUG_DUMP=ON 构建" >&2
  exit 2
fi

echo "== 编译 category_filter_demo =="
# shellcheck disable=SC2046
"$CXX" -std=c++17 -O2 -g -rdynamic -DGO2CPP_DEBUG_DUMP=1 \
  -I include -I third_party \
  -Wl,-rpath,"$ROOT/$BUILD" \
  tests/debug/category_filter_demo.cpp -o "$DEMO_BIN" \
  $(ls "$BUILD"/libgo2cpp_*.so | grep -v '\.so\.') \
  -lpthread -ldl || { echo "FAIL: demo 编译失败" >&2; exit 2; }

echo "== 全开跑一次 =="
"$DEMO_BIN" > "$OUT_DIR/all.txt" 2>&1 || { echo "FAIL: demo(all) 退出非零"; exit 1; }
cat "$OUT_DIR/all.txt"

echo "== GO2CPP_DEBUG_CATEGORIES=mutex 跑一次 =="
GO2CPP_DEBUG_CATEGORIES=mutex "$DEMO_BIN" > "$OUT_DIR/mutex.txt" 2>&1 || {
  echo "FAIL: demo(mutex) 退出非零"; exit 1; }
cat "$OUT_DIR/mutex.txt"

fail=0
value_of() {
  # $1=file $2=key；输出最后一个匹配行的数值
  grep "^$2=" "$1" | tail -1 | cut -d= -f2
}

check_positive() {
  local name="$1" file="$2" key="$3" value
  value="$(value_of "$file" "$key")"
  if [[ -n "$value" && "$value" =~ ^[0-9]+$ && "$value" -gt 0 ]]; then
    echo "PASS: $name $key=$value (>0)"
  else
    echo "FAIL: $name $key=$value (expected >0)"
    fail=1
  fi
}

check_zero() {
  local name="$1" file="$2" key="$3" value
  value="$(value_of "$file" "$key")"
  if [[ "$value" == "0" ]]; then
    echo "PASS: $name $key=0"
  else
    echo "FAIL: $name $key=$value (expected 0)"
    fail=1
  fi
}

echo "== 断言：全开配置 =="
check_positive "all" "$OUT_DIR/all.txt" "category lifecycle"
check_positive "all" "$OUT_DIR/all.txt" "category sched"
check_positive "all" "$OUT_DIR/all.txt" "category park"
check_positive "all" "$OUT_DIR/all.txt" "category primitives"
check_positive "all" "$OUT_DIR/all.txt" "event kMutexLock"

echo "== 断言：mutex-only 配置 =="
check_zero "mutex-only" "$OUT_DIR/mutex.txt" "category lifecycle"
check_zero "mutex-only" "$OUT_DIR/mutex.txt" "category sched"
check_zero "mutex-only" "$OUT_DIR/mutex.txt" "category park"
check_zero "mutex-only" "$OUT_DIR/mutex.txt" "category timer"
check_zero "mutex-only" "$OUT_DIR/mutex.txt" "category io"
check_positive "mutex-only" "$OUT_DIR/mutex.txt" "category primitives"
check_positive "mutex-only" "$OUT_DIR/mutex.txt" "event kMutexLock"
check_zero "mutex-only" "$OUT_DIR/mutex.txt" "event kParkEnter"
check_zero "mutex-only" "$OUT_DIR/mutex.txt" "event kSchedulerStart"

if [[ "$fail" -ne 0 ]]; then
  echo "RESULT: FAIL"
  exit 1
fi
echo "RESULT: PASS"
