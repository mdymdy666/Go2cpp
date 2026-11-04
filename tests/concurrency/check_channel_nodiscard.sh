#!/usr/bin/env bash
# 验证 ChannelSendResult/ChannelRecvResult 的 [[nodiscard]] 确实生效：
# 丢弃返回值必须在 -Wunused-result 下编译失败；显式使用则通过。
# 用法：bash tests/concurrency/check_channel_nodiscard.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CXX="${CXX:-c++}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() {
  echo "[FAIL] $*" >&2
  exit 1
}

cat > "$TMP/discard_send.cpp" <<'EOF'
#include "go2cpp/concurrency/channel.hpp"
int main() {
  auto ch = go2cpp::MakeChannel<int>(1);
  ch->Send(1);  // 关闭/失败结果被静默丢弃：必须触发 -Wunused-result
  return 0;
}
EOF

cat > "$TMP/discard_recv.cpp" <<'EOF'
#include "go2cpp/concurrency/channel.hpp"
int main() {
  auto ch = go2cpp::MakeChannel<int>(1);
  ch->TryRecv();  // 同上
  return 0;
}
EOF

cat > "$TMP/used.cpp" <<'EOF'
#include "go2cpp/concurrency/channel.hpp"
int main() {
  auto ch = go2cpp::MakeChannel<int>(1);
  const auto sent = ch->Send(1);
  const auto got = ch->TryRecv();
  return sent.Ok() && got.Ok() ? 0 : 1;
}
EOF

for source in discard_send discard_recv; do
  if "$CXX" -std=c++17 -I"$ROOT/include" -Wall -Wextra \
      -Werror=unused-result -fsyntax-only "$TMP/$source.cpp" \
      2>"$TMP/$source.log"; then
    fail "$source.cpp 丢弃结果却编译通过，[[nodiscard]] 未生效"
  fi
  if ! grep -q 'nodiscard\|unused-result\|ignoring return value' \
      "$TMP/$source.log"; then
    fail "$source.cpp 编译失败但不是 unused-result：$(cat "$TMP/$source.log")"
  fi
done

"$CXX" -std=c++17 -I"$ROOT/include" -Wall -Wextra \
  -Werror=unused-result -fsyntax-only "$TMP/used.cpp" \
  || fail "used.cpp 正常使用返回值却编译失败"

echo "[ok] [[nodiscard]] 生效：Send/TryRecv 丢弃告警，使用返回值通过"
