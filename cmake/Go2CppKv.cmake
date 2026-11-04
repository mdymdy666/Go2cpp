# ---------------------------------------------------------------------------
# 自研 KV 存储引擎（include/go2cpp/storage/kv.hpp + src/storage/*.cpp）
#
# 与 cmake/Go2CppStorage.cmake（SQLite 嵌入式层）互不依赖：本文件只注册
# go2cpp_kv 一个目标与三个测试目标，默认 ON 但仅构建 5 个 .cpp（~10s）。
#
#   cmake -S . -B build -DGO2CPP_WITH_KV=ON
#   cmake --build build --target go2cpp_kv go2cpp_kv_test
#   ctest --test-dir build -R go2cpp_kv
#
# 仅支持 POSIX（open/pread/mmap/fdatasync）。Windows 走 WSL 构建。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_KV "Build the embedded KV storage engine" ON)

if (NOT GO2CPP_WITH_KV)
    message(STATUS "KV storage: OFF (set GO2CPP_WITH_KV=ON to enable)")
    return()
endif()

if (NOT UNIX)
    message(STATUS "KV storage: skipped (POSIX only)")
    return()
endif()

include(GNUInstallDirs)
find_package(Threads REQUIRED)

add_library(go2cpp_kv
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/arena.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/memtable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/wal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/sstable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/kv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/seglog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/watch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/memcache.cpp)
add_library(go2cpp::kv ALIAS go2cpp_kv)

target_include_directories(go2cpp_kv
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage)
target_compile_features(go2cpp_kv PUBLIC cxx_std_17)
# 零警告是验收条件：新模块用 -Werror 把回归挡在构建期。
target_compile_options(go2cpp_kv PRIVATE -Wall -Wextra -Wpedantic -Werror)

# 热路径编译选项与其余模块保持一致：
#   -fno-semantic-interposition：库内调用不走 PLT/GOT；
#   -fno-stack-protector：避免给含取地址局部变量的热函数插 canary。
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
    CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(go2cpp_kv PRIVATE
        -fno-semantic-interposition -fno-stack-protector)
    if (BUILD_SHARED_LIBS)
        target_link_options(go2cpp_kv PRIVATE
            "LINKER:-Bsymbolic-functions")
    endif()
endif()

# 失败路径（WAL/SSTable 打开、fsync、校验）需要统一日志；go2cpp_log 只依赖
# C++17 标准库，不反向依赖 kv，因此 PUBLIC 链接不构成依赖环。standalone
# 构建（未注册 go2cpp_log 目标）时把日志实现并入本目标，保持可独立编译。
if (TARGET go2cpp_log)
    target_link_libraries(go2cpp_kv PUBLIC Threads::Threads go2cpp_log)
else()
    target_link_libraries(go2cpp_kv PUBLIC Threads::Threads)
    target_sources(go2cpp_kv PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/logging/log.cpp)
endif()

if (GO2CPP_BUILD_TESTS)
    # ---- 正确性：功能/边界/持久化/迭代器/批 ----
    add_executable(go2cpp_kv_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/kv_test.cpp)
    target_link_libraries(go2cpp_kv_test PRIVATE go2cpp_kv)
    target_compile_options(go2cpp_kv_test PRIVATE -Wall -Wextra)
    add_test(NAME go2cpp_kv_test COMMAND go2cpp_kv_test)
    set_tests_properties(go2cpp_kv_test PROPERTIES TIMEOUT 600)

    # ---- 崩溃恢复：kill -9 / WAL 截断 / WAL 位翻转 ----
    add_executable(go2cpp_kv_recovery_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/kv_recovery_test.cpp)
    target_link_libraries(go2cpp_kv_recovery_test PRIVATE go2cpp_kv)
    target_compile_options(go2cpp_kv_recovery_test PRIVATE -Wall -Wextra)
    add_test(NAME go2cpp_kv_recovery_test COMMAND go2cpp_kv_recovery_test)
    set_tests_properties(go2cpp_kv_recovery_test PROPERTIES TIMEOUT 600)

    # ---- 并发：多线程写 / 读写混合 / 并发删除 / 边写边迭代 ----
    add_executable(go2cpp_kv_concurrent_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/kv_concurrent_test.cpp)
    target_link_libraries(go2cpp_kv_concurrent_test PRIVATE go2cpp_kv)
    target_compile_options(go2cpp_kv_concurrent_test PRIVATE -Wall -Wextra)
    add_test(NAME go2cpp_kv_concurrent_test COMMAND go2cpp_kv_concurrent_test)
    set_tests_properties(go2cpp_kv_concurrent_test PROPERTIES TIMEOUT 600)

    # ---- 基准：ctest 跑 1e6 冒烟；1e7 数据手动跑 ----
    add_executable(go2cpp_kv_bench
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/kv_bench.cpp)
    target_link_libraries(go2cpp_kv_bench PRIVATE go2cpp_kv)
    target_include_directories(go2cpp_kv_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage)
    target_compile_options(go2cpp_kv_bench PRIVATE -Wall -Wextra)
    add_test(NAME go2cpp_kv_bench COMMAND go2cpp_kv_bench --n=1000000)
    set_tests_properties(go2cpp_kv_bench PROPERTIES TIMEOUT 900)

    # ---- 功能层：Queue（MQ）/ Watch（订阅）/ MemCache（内存缓存） ----
    # 三者共用同一个 LSM：Queue/Watch 的游标是 KV 记录，MemCache 可写穿。
    foreach(feature queue watch memcache)
        add_executable(go2cpp_${feature}_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/${feature}_test.cpp)
        target_link_libraries(go2cpp_${feature}_test PRIVATE go2cpp_kv)
        target_include_directories(go2cpp_${feature}_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/storage)
        target_compile_options(go2cpp_${feature}_test PRIVATE -Wall -Wextra)
        add_test(NAME go2cpp_${feature}_test COMMAND go2cpp_${feature}_test)
        set_tests_properties(go2cpp_${feature}_test PROPERTIES TIMEOUT 900)
    endforeach()

    # ---- 功能层基准：Queue ops/s、Watch 回调开销、MemCache Get/Set ops/s ----
    add_executable(go2cpp_storage_features_bench
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/features_bench.cpp)
    target_link_libraries(go2cpp_storage_features_bench PRIVATE go2cpp_kv)
    target_include_directories(go2cpp_storage_features_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage)
    target_compile_options(go2cpp_storage_features_bench PRIVATE -Wall -Wextra)
    add_test(NAME go2cpp_storage_features_bench
             COMMAND go2cpp_storage_features_bench --n=1000000)
    set_tests_properties(go2cpp_storage_features_bench PROPERTIES TIMEOUT 900)
endif()
