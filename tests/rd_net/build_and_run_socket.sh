#!/usr/bin/env bash
# REQ-006 四态 socket 自测（与 reliability 脚本分开命名）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/include" "$ROOT/src/rd_net/socket.cpp" "$ROOT/tests/rd_net/test_socket.cpp" \
  -pthread -o /tmp/test_socket
/tmp/test_socket
