#!/usr/bin/env bash
# 编译并运行 rd::net 可靠性容器自测；任何编译告警即失败（-Werror）。
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/../.." && pwd)"
out="${TMPDIR:-/tmp}/rd_net_test_reliability.$$"
trap "rm -f ${out}" EXIT

echo "== 编译 =="
echo "g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I${root}/include ${here}/test_reliability.cpp"
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I"${root}/include" "${here}/test_reliability.cpp" -o "${out}"

echo "== 运行 =="
"${out}"
