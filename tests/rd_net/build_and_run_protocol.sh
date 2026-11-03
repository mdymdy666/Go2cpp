#!/usr/bin/env bash
# REQ-008 协议注入入口自测（零拷贝 / echo / ACK 位图 / 入队不阻塞 / 单 tick ≤64 包）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/include" \
  "$ROOT/src/rd_net/socket.cpp" "$ROOT/src/rd_net/event_loop.cpp" \
  "$ROOT/src/rd_net/protocol.cpp" "$ROOT/tests/rd_net/test_protocol.cpp" \
  -L"$ROOT/.build-bento-wsl" \
  -lgo2cpp_network -lgo2cpp_io -lgo2cpp_context -lgo2cpp_error \
  -Wl,-rpath,"$ROOT/.build-bento-wsl" \
  -pthread -o /tmp/test_protocol
/tmp/test_protocol
