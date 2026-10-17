
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

timeout 40 ./build-quality/go2cpp_tls_alpn_test "$PORT" 2>&1 | tr -d '\r'
RC=${PIPESTATUS[0]}
kill $SRV 2>/dev/null
wait $SRV 2>/dev/null
exit $RC
