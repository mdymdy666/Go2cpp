#!/usr/bin/env bash
# gzip 响应压缩验收（第 11 轮性能优化）。
#
# 覆盖：
#   [1] 准备：启动 gateway(28080)，清空日志
#   [2] 首页未压缩基线：GET / → 200、Content-Length=原始字节、无 Content-Encoding
#   [3] Accept-Encoding: gzip → 200、Content-Encoding: gzip、Vary、字节数明显下降、
#       解压后与原始 body 逐字节一致、gzip -t 校验完整
#   [4] 不压缩场景：identity / gzip;q=0 不压缩；小响应（<1 KiB）不压缩
#   [5] HEAD 与 GET 的 Content-Length 一致（压缩后）
#   [6] 性能：taskset 绑核，20 次 TTFB 均值；记录前/后字节数
#
# 进程管理：pids 写入 logs/accept_gzip.pids，退出时校验 exe 后 kill。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT/build"
LOG_DIR="$ROOT/logs"
PID_FILE="$LOG_DIR/accept_gzip.pids"
DB="$ROOT/data/accept_gzip.db"
TMP="$(mktemp -d)"
PASSED=0
FAILED=0

GW_PORT="${DOUYIN_GATEWAY_PORT:-28080}"
GW="http://127.0.0.1:$GW_PORT"

mkdir -p "$LOG_DIR" "$ROOT/data" "$TMP"
: >"$PID_FILE"

stop_all() {
  [[ -f "$PID_FILE" ]] || return 0
  local name pid port expected actual
  while read -r name pid port; do
    [[ -n "${name:-}" && -n "${pid:-}" ]] || continue
    expected="$BIN_DIR/$name"
    actual="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
    if [[ -n "$actual" && "$actual" != "$expected" ]]; then
      continue
    fi
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.02
      done
      kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done <"$PID_FILE"
  : >"$PID_FILE"
}

cleanup() {
  stop_all
  rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check() { # name expected actual [detail]
  local name="$1" expected="$2" actual="$3" detail="${4:-}"
  if [[ "$expected" == "$actual" ]]; then
    printf '  %-46s PASS  %s\n' "$name" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  %-46s FAIL  %s（期望 %s）%s\n' "$name" "$actual" "$expected" "$detail"
    FAILED=$((FAILED + 1))
  fi
}

header_value() { # file name
  grep -i "^$2:" "$1" 2>/dev/null | head -1 | tr -d '\r' | sed "s/^[^:]*: *//"
}

content_length() { # file
  header_value "$1" 'Content-Length'
}

wait_ready() { # base name
  local base="$1" name="$2"
  for _ in $(seq 1 200); do
    if curl -fsS --max-time 1 "$base/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  echo "$name 启动失败，日志：" >&2
  tail -20 "$LOG_DIR/accept_gzip_$name.log" >&2
  return 1
}

echo "[1/6] 准备：启动 gateway"
if [[ ! -x "$BIN_DIR/gateway" ]]; then
  echo "找不到 $BIN_DIR/gateway，请先跑 app/douyin/build.sh" >&2
  exit 1
fi
rm -f "$LOG_DIR/accept_gzip_"*.log
if curl -fsS --max-time 1 "$GW/health" >/dev/null 2>&1; then
  echo "端口已被占用：$GW（请先停止旧服务再跑本脚本）" >&2
  exit 1
fi
DOUYIN_DB="$DB" "$BIN_DIR/gateway" "$GW_PORT" >"$LOG_DIR/accept_gzip_gateway.log" 2>&1 &
GW_PID=$!
printf 'gateway %s %s\n' "$GW_PID" "$GW_PORT" >>"$PID_FILE"
wait_ready "$GW" gateway || exit 1
printf '[start] gateway %s pid=%s\n' "$GW" "$GW_PID"

echo "[2/6] 首页未压缩基线"
CODE=$(curl -s --max-time 10 -D "$TMP/h-plain" -o "$TMP/body-plain" \
  -w '%{http_code}' "$GW/")
check "GET / 无 Accept-Encoding" 200 "$CODE"
PLAIN_LEN=$(stat -c '%s' "$TMP/body-plain")
PLAIN_CL=$(content_length "$TMP/h-plain")
check "Content-Length = 实际字节" "$PLAIN_LEN" "$PLAIN_CL"
check "无 Content-Encoding" "" "$(header_value "$TMP/h-plain" 'Content-Encoding')"
check "无 Vary（非文本响应才不加；首页必须加）" "Accept-Encoding" \
  "$(header_value "$TMP/h-plain" 'Vary')"
if [[ "$PLAIN_LEN" -gt 102400 ]]; then
  printf '  %-46s PASS  %s 字节\n' "首页体积（未压缩）" "$PLAIN_LEN"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  %s 字节（期望 >100 KiB）\n' "首页体积（未压缩）" "$PLAIN_LEN"
  FAILED=$((FAILED + 1))
fi

echo "[3/6] gzip 压缩响应"
CODE=$(curl -s --max-time 10 -H 'Accept-Encoding: gzip' -D "$TMP/h-gzip" \
  -o "$TMP/body-gzip" -w '%{http_code}' "$GW/")
check "GET / + gzip" 200 "$CODE"
check "Content-Encoding: gzip" "gzip" "$(header_value "$TMP/h-gzip" 'Content-Encoding')"
check "Vary: Accept-Encoding" "Accept-Encoding" "$(header_value "$TMP/h-gzip" 'Vary')"
GZIP_LEN=$(stat -c '%s' "$TMP/body-gzip")
check "gzip 响应 Content-Length 匹配" "$GZIP_LEN" "$(content_length "$TMP/h-gzip")"
if [[ "$GZIP_LEN" -lt $((PLAIN_LEN * 60 / 100)) ]]; then
  printf '  %-46s PASS  %s → %s 字节（%.1f%%）\n' "压缩率 >40%" \
    "$PLAIN_LEN" "$GZIP_LEN" \
    "$(python3 -c "print(100.0 * $GZIP_LEN / $PLAIN_LEN)")"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  %s → %s 字节\n' "压缩率 >40%" "$PLAIN_LEN" "$GZIP_LEN"
  FAILED=$((FAILED + 1))
fi
if gzip -t "$TMP/body-gzip" 2>/dev/null; then
  printf '  %-46s PASS  gzip 流完整（crc32/ISIZE 校验通过）\n' "gzip -t"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  压缩流损坏\n' "gzip -t"
  FAILED=$((FAILED + 1))
fi
gzip -dc "$TMP/body-gzip" >"$TMP/body-unzip" 2>/dev/null
if cmp -s "$TMP/body-plain" "$TMP/body-unzip"; then
  printf '  %-46s PASS\n' "解压后与原始 body 逐字节一致"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL\n' "解压后与原始 body 逐字节一致"
  FAILED=$((FAILED + 1))
fi
CODE=$(curl -s --max-time 10 --compressed -o "$TMP/body-compressed" \
  -w '%{http_code}' "$GW/")
check "curl --compressed 自动解压" 200 "$CODE"
check "自动解压后大小" "$PLAIN_LEN" "$(stat -c '%s' "$TMP/body-compressed")"

echo "[4/6] 不压缩场景"
CODE=$(curl -s --max-time 10 -H 'Accept-Encoding: identity' -D "$TMP/h-id" \
  -o "$TMP/body-id" -w '%{http_code}' "$GW/")
check "identity 请求" 200 "$CODE"
check "identity 无 Content-Encoding" "" \
  "$(header_value "$TMP/h-id" 'Content-Encoding')"
check "identity 原始大小" "$PLAIN_LEN" "$(stat -c '%s' "$TMP/body-id")"
CODE=$(curl -s --max-time 10 -H 'Accept-Encoding: gzip;q=0' -D "$TMP/h-q0" \
  -o "$TMP/body-q0" -w '%{http_code}' "$GW/")
check "gzip;q=0 请求" 200 "$CODE"
check "gzip;q=0 不压缩" "" "$(header_value "$TMP/h-q0" 'Content-Encoding')"
CODE=$(curl -s --max-time 10 -H 'Accept-Encoding: gzip' -D "$TMP/h-small" \
  -o "$TMP/body-small" -w '%{http_code}' "$GW/health")
check "小响应 GET /health" 200 "$CODE"
check "小响应无 Content-Encoding" "" \
  "$(header_value "$TMP/h-small" 'Content-Encoding')"
SMALL_LEN=$(stat -c '%s' "$TMP/body-small")
if [[ "$SMALL_LEN" -le 1024 ]]; then
  printf '  %-46s PASS  %s 字节（<1 KiB 不压缩）\n' "小响应体积" "$SMALL_LEN"
  PASSED=$((PASSED + 1))
else
  printf '  %-46s FAIL  %s 字节\n' "小响应体积" "$SMALL_LEN"
  FAILED=$((FAILED + 1))
fi
CODE=$(curl -s --max-time 10 -I -H 'Accept-Encoding: gzip' "$GW/" \
  -D "$TMP/h-head" -o /dev/null -w '%{http_code}')
check "HEAD / + gzip" 200 "$CODE"
check "HEAD Content-Encoding 与 GET 一致" "gzip" \
  "$(header_value "$TMP/h-head" 'Content-Encoding')"
HEAD_CL=$(content_length "$TMP/h-head")
GET_CL=$(content_length "$TMP/h-gzip")
check "HEAD 与 GET 的 Content-Length 一致" "$GET_CL" "$HEAD_CL"
DUP=$(grep -ci '^Content-Encoding:' "$TMP/h-gzip")
check "Content-Encoding 只出现一次（不重复压缩）" 1 "$DUP"

echo "[5/6] 上游文本响应同样压缩（首页与上下游同一条路径）"
# 网关转发响应与本地 handler 共用 DecorateGzip；这里用 404 JSON（小响应）验证
# 转发路径也经过装饰器（无 Content-Encoding，且带 Vary 的条件判定不误伤）。
CODE=$(curl -s --max-time 10 -H 'Accept-Encoding: gzip' -D "$TMP/h-404" \
  -o "$TMP/body-404" -w '%{http_code}' "$GW/no-such-route")
check "未知路由 404" 404 "$CODE"
check "404 小响应无 Content-Encoding" "" \
  "$(header_value "$TMP/h-404" 'Content-Encoding')"

echo "[6/6] 性能：TTFB（taskset 绑核，20 次均值）"
CORE="${DOUYIN_PERF_CORE:-0}"
ttfb20() { # url extra_header_or_empty
  local url="$1" header="${2:-}" total=0 i t
  for i in $(seq 1 20); do
    if [[ -n "$header" ]]; then
      t=$(taskset -c "$CORE" curl -s -o /dev/null --max-time 10 \
            -H "$header" -w '%{time_starttransfer}' "$url")
    else
      t=$(taskset -c "$CORE" curl -s -o /dev/null --max-time 10 \
            -w '%{time_starttransfer}' "$url")
    fi
    total=$(python3 -c "print($total + $t)")
  done
  python3 -c "print(round($total / 20 * 1000, 2))"
}
PLAIN_MS=$(ttfb20 "$GW/")
GZIP_MS=$(ttfb20 "$GW/" 'Accept-Encoding: gzip')
printf '  首页未压缩：%s 字节，TTFB %s ms\n' "$PLAIN_LEN" "$PLAIN_MS"
printf '  首页 gzip ：%s 字节，TTFB %s ms\n' "$GZIP_LEN" "$GZIP_MS"
check "未压缩 TTFB < 200ms" 1 "$(python3 -c "print(1 if $PLAIN_MS < 200 else 0)")"
check "gzip TTFB < 200ms" 1 "$(python3 -c "print(1 if $GZIP_MS < 200 else 0)")"

echo
printf '结果：%d PASS / %d FAIL\n' "$PASSED" "$FAILED"
[[ "$FAILED" -eq 0 ]]
