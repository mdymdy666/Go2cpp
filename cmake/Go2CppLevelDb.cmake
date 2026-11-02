# ---------------------------------------------------------------------------
# 官方 LevelDB 存储后端（include/go2cpp/storage/leveldb_kv.hpp +
# src/storage/leveldb_kv.cpp）。
#
# 默认 OFF：LevelDB 是可选依赖（预编译静态库），不打开时本仓库其余目标
# 完全不受影响。
#
#   cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_LEVELDB=ON
#   cmake --build .build-bento-wsl --target go2cpp_leveldb go2cpp_leveldb_kv_test
#   ctest --test-dir .build-bento-wsl -R go2cpp_leveldb
#
# 依赖：libleveldb.a（Release 自编译，默认 ~/.local/lib）+ 头文件
# （默认 ~/.local/include/leveldb）+ pthread。
#
# 注意：CMake 的 find_library/find_path 不会展开 "~"，必须用 $ENV{HOME}；
# find_library 的 NAMES 也不能直接写 "libleveldb.a"（会被拼成
# liblibleveldb.a.a），静态库用 find_file 精确匹配。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_LEVELDB "Build the official LevelDB storage backend" OFF)

if (NOT GO2CPP_WITH_LEVELDB)
    message(STATUS "LevelDB storage: OFF (set GO2CPP_WITH_LEVELDB=ON to enable)")
    return()
endif()

if (NOT UNIX)
    message(STATUS "LevelDB storage: skipped (POSIX only)")
    return()
endif()

include(GNUInstallDirs)
find_package(Threads REQUIRED)

# ---- 定位静态库：优先 ~/.local/lib/libleveldb.a，其次系统 liblibleveldb.a ----
find_file(LEVELDB_STATIC_LIB
    NAMES libleveldb.a
    PATHS "$ENV{HOME}/.local/lib" /usr/local/lib
    NO_DEFAULT_PATH)
if (NOT LEVELDB_STATIC_LIB)
    find_file(LEVELDB_STATIC_LIB NAMES libleveldb.a)
endif()
# 退路：动态库（若用户只装了 .so）。
find_library(LEVELDB_SHARED_LIB
    NAMES leveldb
    PATHS "$ENV{HOME}/.local/lib" /usr/local/lib
    NO_DEFAULT_PATH)

if (LEVELDB_STATIC_LIB)
    set(LEVELDB_LIB "${LEVELDB_STATIC_LIB}")
elseif (LEVELDB_SHARED_LIB)
    set(LEVELDB_LIB "${LEVELDB_SHARED_LIB}")
else()
    message(FATAL_ERROR
        "LevelDB not found: 期望 $ENV{HOME}/.local/lib/libleveldb.a。"
        " 先按 /tmp/leveldb 的源码安装（cmake --install），或传 "
        "-DLEVELDB_STATIC_LIB=/path/to/libleveldb.a")
endif()

# ---- 定位头文件 ----
find_path(LEVELDB_INCLUDE
    NAMES leveldb/db.h
    PATHS "$ENV{HOME}/.local/include" /usr/local/include
    NO_DEFAULT_PATH)
if (NOT LEVELDB_INCLUDE)
    find_path(LEVELDB_INCLUDE NAMES leveldb/db.h)
endif()
if (NOT LEVELDB_INCLUDE)
    message(FATAL_ERROR
        "LevelDB headers not found: 期望 $ENV{HOME}/.local/include/leveldb/db.h")
endif()

message(STATUS "LevelDB storage: lib=${LEVELDB_LIB}")
message(STATUS "LevelDB storage: include=${LEVELDB_INCLUDE}")

# 静态库：预编译的 libleveldb.a 未开 -fPIC，不能链接进 .so
# （ld: relocation R_X86_64_PC32 ... can not be used when making a shared
# object; recompile with -fPIC）。静态库让 LevelDB 的符号只进最终可执行文件。
add_library(go2cpp_leveldb STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/leveldb_kv.cpp)
add_library(go2cpp::leveldb ALIAS go2cpp_leveldb)

target_include_directories(go2cpp_leveldb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage)
# 第三方头：SYSTEM 抑制其警告，避免 -Werror 被上游头文件触发。
target_include_directories(go2cpp_leveldb SYSTEM PRIVATE ${LEVELDB_INCLUDE})
target_compile_features(go2cpp_leveldb PUBLIC cxx_std_17)
# 零警告是验收条件。
target_compile_options(go2cpp_leveldb PRIVATE -Wall -Wextra -Wpedantic -Werror)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
    CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(go2cpp_leveldb PRIVATE
        -fno-semantic-interposition -fno-stack-protector)
endif()

# 静态库的符号要传播到最终可执行文件，故为 PUBLIC；pthread 同理。
target_link_libraries(go2cpp_leveldb
    PUBLIC Threads::Threads "${LEVELDB_LIB}")

if (GO2CPP_BUILD_TESTS)
    add_executable(go2cpp_leveldb_kv_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/test_leveldb_kv.cpp)
    target_link_libraries(go2cpp_leveldb_kv_test PRIVATE go2cpp_leveldb)
    target_compile_options(go2cpp_leveldb_kv_test PRIVATE -Wall -Wextra)
    # 对拍需要自研 KV 作对照；GO2CPP_WITH_KV=OFF 时只跑 LevelDB 单侧。
    if (TARGET go2cpp_kv)
        target_link_libraries(go2cpp_leveldb_kv_test PRIVATE go2cpp_kv)
        target_compile_definitions(go2cpp_leveldb_kv_test
            PRIVATE GO2CPP_LEVELDB_HAVE_KV=1)
    else()
        message(STATUS
            "LevelDB storage: go2cpp_kv 未构建，对拍用例将跳过自研侧")
    endif()
    add_test(NAME go2cpp_leveldb_kv_test COMMAND go2cpp_leveldb_kv_test)
    set_tests_properties(go2cpp_leveldb_kv_test PROPERTIES TIMEOUT 900)
endif()
