#!/usr/bin/env bash
# 构建「池化资源启动开销」基准。
#
# 刻意不改仓库的顶层 CMakeLists：本工具只是树内的一次性测量程序，链接
# 已构建好的 .so 即可。这样既不影响库/测试的默认构建，也不会把基准的
# 依赖（-Isrc/storage 之类的私有头）带进主工程。
#
# 用法：
#   tools/startup_cost/build.sh                 # Release 构建（-O2 -DNDEBUG）
#   CXX=clang++ tools/startup_cost/build.sh     # 换编译器
#   STARTUP_COST_BUILD_DIR=.build-p0-wsl tools/startup_cost/build.sh
#
# 产物：.build-startup-cost/{startup_cost, startup_cost_min}
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${STARTUP_COST_BUILD_DIR:-${REPO_ROOT}/.build-bento-wsl}"
OUT_DIR="${STARTUP_COST_OUT_DIR:-${REPO_ROOT}/.build-startup-cost}"

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "[build] 缺少构建目录：${BUILD_DIR}" >&2
    echo "[build] 先构建依赖库（cmake --build <dir>），或用 STARTUP_COST_BUILD_DIR 指定" >&2
    exit 1
fi

CXX="${CXX:-g++}"
CXXFLAGS=(-O2 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic)
INCLUDES=(-I"${REPO_ROOT}/include" -I"${REPO_ROOT}/src/storage")
# 与被测库保持一致：立即绑定全部 PLT 重定位（见顶层 CMakeLists 的说明）。
LINK_FLAGS=(-Wl,-z,now -Wl,-rpath,"${BUILD_DIR}")
LIBS=(
    -L"${BUILD_DIR}"
    -Wl,--start-group
    -lgo2cpp_scheduler
    -lgo2cpp_fiber
    -lgo2cpp_sync
    -lgo2cpp_channel
    -lgo2cpp_context
    -lgo2cpp_storage
    -lgo2cpp_kv
    -lgo2cpp_media
    -lgo2cpp_app
    -lgo2cpp_os
    -lgo2cpp_io
    -lgo2cpp_error
    -lgo2cpp_debug
    -lgo2cpp_debug_event
    -lgo2cpp_log
    -lgo2cpp_bento
    -lgo2cpp_bento_transport
    -lgo2cpp_bento_http
    -Wl,--end-group
    -lpthread
    -ldl
)
DEFINES=(
    -DSTARTUP_COST_BUILD_DIR="\"$(basename "${BUILD_DIR}")\""
    -DSTARTUP_COST_BUILD_TYPE="\"Release (-O2 -DNDEBUG)\""
)

mkdir -p "${OUT_DIR}"

echo "[build] 最小基线程序 -> ${OUT_DIR}/startup_cost_min"
"${CXX}" -O2 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic \
    "${SCRIPT_DIR}/min_main.cpp" \
    -o "${OUT_DIR}/startup_cost_min"

echo "[build] 基准主程序 -> ${OUT_DIR}/startup_cost"
"${CXX}" "${CXXFLAGS[@]}" "${DEFINES[@]}" "${INCLUDES[@]}" \
    "${SCRIPT_DIR}/main.cpp" \
    "${SCRIPT_DIR}/pools.cpp" \
    "${SCRIPT_DIR}/report.cpp" \
    "${LINK_FLAGS[@]}" "${LIBS[@]}" \
    -o "${OUT_DIR}/startup_cost"

echo "[build] 完成"
