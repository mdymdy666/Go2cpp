#!/usr/bin/env bash
# 一键编译抖音 P0 骨架：网关 + 六个服务（各自一个二进制）。
#
# 依赖 Go2Cpp 已编译好的库（默认 $GO2CPP_ROOT/.build-bento-wsl）。
# 只读引用 Go2Cpp 的头文件与 .so，不改动其源码树。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GO2CPP="${GO2CPP_ROOT:-/UserData/CodexWorkSpace/Go2cpp}"
BUILD_LIB="${GO2CPP_BUILD:-$GO2CPP/.build-bento-wsl}"
CXX="${CXX:-g++}"

if [[ ! -d "$GO2CPP/include" ]]; then
  echo "找不到 Go2Cpp 头文件：$GO2CPP/include" >&2
  echo "请用 GO2CPP_ROOT=... 指定源码树路径" >&2
  exit 1
fi
if [[ ! -d "$BUILD_LIB" ]]; then
  echo "找不到 Go2Cpp 构建产物：$BUILD_LIB" >&2
  echo "请用 GO2CPP_BUILD=... 指定构建目录" >&2
  exit 1
fi

# ---- 构建一致性门禁 --------------------------------------------------------
# 本脚本手工调用 g++，不跟踪 Go2Cpp 的头文件依赖：若 include/ 已更新而
# libgo2cpp_*.so 仍是上一批，编出来的 exe 会链接到旧接口/旧 ABI，运行期
# 表现为假 SEGFAULT（2026-11-04 事故）。所以链接前必须先确认两者同批。
FRESH_CHECK="$GO2CPP/scripts/check_build_freshness.sh"
if [[ -f "$FRESH_CHECK" ]]; then
  echo "[build] 校验 Go2Cpp 产物与源码一致性 ..."
  if ! bash "$FRESH_CHECK" --repo "$GO2CPP" --build-dir "$BUILD_LIB" --quiet; then
    if [[ "${DOUYIN_AUTOREBUILD:-0}" == "1" ]]; then
      echo "[build] 产物过期，自动重建 Go2Cpp（DOUYIN_AUTOREBUILD=1）..."
      if ! cmake --build "$BUILD_LIB" --parallel "$(nproc 2>/dev/null || echo 4)"; then
        echo "[build] Go2Cpp 自动重建失败，中止" >&2
        exit 1
      fi
      if ! bash "$FRESH_CHECK" --repo "$GO2CPP" --build-dir "$BUILD_LIB" --quiet; then
        echo "[build] 重建后仍不一致（产物可能被手工替换），中止" >&2
        exit 1
      fi
    else
      echo "[build] 停止：Go2Cpp 产物与源码不一致（旧 exe + 新 so 会造假 SEGFAULT）" >&2
      echo "  先重建：cmake --build $BUILD_LIB --parallel \$(nproc)" >&2
      echo "  或自动：DOUYIN_AUTOREBUILD=1 bash app/douyin/build.sh" >&2
      exit 1
    fi
  fi
  echo "[build] Go2Cpp 产物与源码一致"
else
  echo "[build] 警告：缺少 $FRESH_CHECK，跳过构建一致性校验" >&2
fi

mkdir -p "$ROOT/build" "$ROOT/logs" "$ROOT/data"

CXXFLAGS=(-std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter
          -I"$GO2CPP/include" -I"$ROOT/include"
          -DDOUYIN_WEB_DIR="\"$ROOT/web\"")

# network 依赖 io/sync/context 等，用 --start-group 消掉静态顺序问题。
LDFLAGS=(-L"$BUILD_LIB" -Wl,-rpath,"$BUILD_LIB" -Wl,--start-group
         -lgo2cpp_network -lgo2cpp_io -lgo2cpp_sync -lgo2cpp_context
         -lgo2cpp_error -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log
         -lgo2cpp_hash -lgo2cpp_metrics -Wl,--end-group -lpthread)

# user 服务额外依赖 storage（libgo2cpp_storage.so + 系统 sqlite3）。
EXTRA_LDLIBS=()

build_one() {
  local name="$1"
  shift
  printf '  CC %-20s ' "$name"
  if "$CXX" "${CXXFLAGS[@]}" "$@" "${EXTRA_LDLIBS[@]}" "${LDFLAGS[@]}" -o "$ROOT/build/$name"; then
    echo "ok"
  else
    echo "失败"
    exit 1
  fi
}

echo "[build] Go2Cpp 头文件: $GO2CPP/include"
echo "[build] Go2Cpp 库目录: $BUILD_LIB"

PROTO="$ROOT/src/douyin_proto.cpp"
STORE="$ROOT/src/douyin_store.cpp"
TAG="$ROOT/src/douyin_tag.cpp"
AUTH="$ROOT/src/douyin_auth.cpp"

EXTRA_LDLIBS=(-lgo2cpp_storage -lgo2cpp_encoding -lsqlite3)
build_one user_service        "$ROOT/src/user_service.cpp"        "$STORE" "$AUTH" "$PROTO" "$TAG"
build_one video_service       "$ROOT/src/video_service.cpp"       "$STORE" "$AUTH" "$PROTO" "$TAG"
build_one feed_service        "$ROOT/src/feed_service.cpp"        "$STORE" "$AUTH" "$PROTO" "$TAG"
EXTRA_LDLIBS=(-lgo2cpp_storage -lgo2cpp_encoding -lsqlite3)
build_one interaction_service "$ROOT/src/interaction_service.cpp" "$STORE" "$AUTH" "$PROTO" "$TAG"
build_one live_service        "$ROOT/src/live_service.cpp"        "$STORE" "$AUTH" "$PROTO" "$TAG"
EXTRA_LDLIBS=(-lgo2cpp_storage -lgo2cpp_encoding -lsqlite3)
build_one search_service      "$ROOT/src/search_service.cpp"      "$STORE" "$PROTO" "$TAG"
EXTRA_LDLIBS=(-lgo2cpp_encoding)
# C8 网关：通用 Gateway 组件在 src/app/gateway.cpp，尚未打进 libgo2cpp_app.a，
# 这里把它作为独立编译单元一起编；AUTH 提供 douyin::VerifyToken 做 Bearer 校验。
# 上游 src/app/gateway.cpp 的文档注释里有字面量 "/*"（method 支持字符串/数组/*），
# GCC 会报 -Wcomment；只对这个编译单元关闭该类警告，不修改上游源码。
build_one gateway             "$ROOT/src/gateway.cpp" "$ROOT/src/douyin_observability.cpp" \
  "$AUTH" "$GO2CPP/src/app/gateway.cpp" \
  -Wno-comment

# C9 压测工具：纯 POSIX socket，不依赖 Go2Cpp 库。
printf '  CC %-20s ' loadgen
if "$CXX" "${CXXFLAGS[@]}" "$ROOT/scripts/loadgen.cpp" -o "$ROOT/build/loadgen" -pthread; then
  echo "ok"
else
  echo "失败"
  exit 1
fi

echo "[build] 完成：$ROOT/build/{gateway,loadgen,user_service,video_service,feed_service,interaction_service,live_service,search_service}"
