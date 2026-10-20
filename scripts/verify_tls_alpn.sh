
#!/bin/bash
# 验证 TlsTransport 的 TLS 握手与 ALPN 协商（需要 openssl 与 s_server）
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=${1:-18443}
CERT=/tmp/tls_test_cert.pem
KEY=/tmp/tls_test_key.pem

if ! command -v openssl >/dev/null 2>&1; then
  echo "SKIP: 未安装 openssl"
  exit 0
fi
if [ ! -f "$CERT" ]; then
  openssl req -x509 -newkey rsa:2048 -keyout "$KEY" -out "$CERT" \
    -days 1 -nodes -subj "/CN=localhost" >/dev/null 2>&1
fi

openssl s_server -alpn h2 -accept "$PORT" -cert "$CERT" -key "$KEY" -www \
  >/tmp/s_server.log 2>&1 &
SRV=$!
for _ in $(seq 1 60); do
  if ss -ltn 2>/dev/null | grep -q ":$PORT "; then break; fi
  sleep 0.2
done

# 二进制随 CMake 构建在构建目录里（默认 build-work），不再引用历史上
# 从未生成的 build-quality/ 目录。
BUILD_DIR=${BUILD_DIR:-build-work}
BIN="$BUILD_DIR/go2cpp_tls_alpn_test"
if [ ! -x "$BIN" ]; then
  echo "ERROR: 未找到 $BIN" >&2
  echo "先构建：cmake -S . -B $BUILD_DIR -DGO2CPP_BUILD_TESTS=ON && cmake --build $BUILD_DIR --target go2cpp_tls_alpn_test" >&2
  kill $SRV 2>/dev/null
  wait $SRV 2>/dev/null
  exit 1
fi
timeout 40 "$BIN" "$PORT" 2>&1 | tr -d '\r'
RC=${PIPESTATUS[0]}
kill $SRV 2>/dev/null
wait $SRV 2>/dev/null
exit $RC
