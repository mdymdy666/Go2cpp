#!/usr/bin/env bash
# REQ-007 事件循环 + 毫秒定时器自测（单线程 poll + eventfd + 最小堆定时器）
# socket.cpp 已绑定 go2cpp::net::Socket，故需链接 network/io/context/error。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
B="$ROOT/.build-bento-wsl"
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/include" "$ROOT/src/rd_net/socket.cpp" "$ROOT/src/rd_net/event_loop.cpp" \
  "$ROOT/tests/rd_net/test_event_loop.cpp" \
  -L"$B" -Wl,-rpath,"$B" \
  -lgo2cpp_network -lgo2cpp_io -lgo2cpp_context -lgo2cpp_error -pthread \
  -o /tmp/test_event_loop
/tmp/test_event_loop
