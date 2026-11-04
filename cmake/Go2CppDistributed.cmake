# ---------------------------------------------------------------------------
# 分布式设施（服务发现 / 分布式锁 / 选主）
#
# 定位：在 relay 的 Lease/Heartbeat/Hub 之上做三个基础原语，默认随主构建
# 打开。依赖方向：
#   distributed -> relay（Lease/Heartbeat/Hub）+ network（TCP 协调者）
#               -> Threads
#
# 两种接入方式：
#   1. 完整构建（顶层 CMakeLists 已 include Relay）：直接链接 go2cpp_relay；
#   2. Standalone Bento 构建（GO2CPP_BENTO_STANDALONE=ON，工作树缺少 base
#      模块）：没有 go2cpp_relay 目标，但 relay 的两份实现（hub.cpp/lease.cpp）
#      与 go2cpp_socket_bufio_standalone（network + context + scheduler 最小
#      依赖集）都在，则把 relay 源直接编入本库。两条路径使用**同一份** relay
#      源码，语义一致。
#
# 测试策略：每个测试文件一个可执行目标并打 `distributed` 标签。多进程
# 用例（fork + exec 自身）只在 UNIX 上构建，且打 `long` 标签，可用
# `ctest -LE long` 跳过。bench 只构建不注册，避免计时噪声进回归。
#
# 本文件必须在 Go2CppBento.cmake / Go2CppRelay.cmake / Go2CppModules.cmake
# 之后 include。
# ---------------------------------------------------------------------------

option(GO2CPP_WITH_DISTRIBUTED "Build go2cpp_distributed (registry/lock/election)" ON)
option(GO2CPP_DISTRIBUTED_TESTS "Build tests for the distributed primitives" ON)
option(GO2CPP_DISTRIBUTED_MULTIPROCESS_TESTS
    "Build fork/exec based multi-process tests for distributed primitives" ON)

if (NOT GO2CPP_WITH_DISTRIBUTED)
    message(STATUS "distributed: OFF (GO2CPP_WITH_DISTRIBUTED=OFF)")
    return()
endif()

# 完整构建通常已由 Go2CppModules.cmake include；standalone 构建没有，这里
# 补一次以拿到 CMAKE_INSTALL_INCLUDEDIR（GNUInstallDirs 自身幂等）。
include(GNUInstallDirs)

set(GO2CPP_DISTRIBUTED_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(GO2CPP_DISTRIBUTED_TESTS_DIR ${GO2CPP_DISTRIBUTED_ROOT}/tests/distributed)

set(GO2CPP_DISTRIBUTED_SOURCES
    ${GO2CPP_DISTRIBUTED_ROOT}/src/distributed/coordinator.cpp
    ${GO2CPP_DISTRIBUTED_ROOT}/src/distributed/coordinator_snapshot.cpp
    ${GO2CPP_DISTRIBUTED_ROOT}/src/distributed/metadata_store.cpp
    ${GO2CPP_DISTRIBUTED_ROOT}/src/distributed/node_id.cpp
    ${GO2CPP_DISTRIBUTED_ROOT}/src/distributed/lease_session.cpp
    ${GO2CPP_DISTRIBUTED_ROOT}/src/distributed/registry.cpp
    ${GO2CPP_DISTRIBUTED_ROOT}/src/distributed/lock.cpp
    ${GO2CPP_DISTRIBUTED_ROOT}/src/distributed/election.cpp
    ${GO2CPP_DISTRIBUTED_ROOT}/src/distributed/transport.cpp
)

if (TARGET go2cpp_relay)
    # 完整构建：relay 已作为独立目标存在（Lease/Heartbeat/Hub 的正式归属）。
    set(GO2CPP_DISTRIBUTED_RELAY_LIB go2cpp_relay)
    set(GO2CPP_DISTRIBUTED_BASE_LIB "")
elseif (EXISTS ${GO2CPP_DISTRIBUTED_ROOT}/src/relay/hub.cpp AND
        EXISTS ${GO2CPP_DISTRIBUTED_ROOT}/src/relay/lease.cpp)
    # Standalone 构建：relay 没有单独目标，把同一份实现编入本库。
    message(STATUS "distributed: relay target missing, compiling src/relay/{hub,lease}.cpp inline")
    list(APPEND GO2CPP_DISTRIBUTED_SOURCES
        ${GO2CPP_DISTRIBUTED_ROOT}/src/relay/hub.cpp
        ${GO2CPP_DISTRIBUTED_ROOT}/src/relay/lease.cpp)
    set(GO2CPP_DISTRIBUTED_RELAY_LIB "")
    if (TARGET go2cpp_network AND TARGET go2cpp_context AND TARGET go2cpp_scheduler)
        set(GO2CPP_DISTRIBUTED_BASE_LIB
            go2cpp_network go2cpp_context go2cpp_scheduler go2cpp_io go2cpp_sync)
    elseif (TARGET go2cpp_socket_bufio_standalone)
        # 该静态库由 Go2CppBento.cmake 在 standalone 模式下创建，包含
        # socket/context/scheduler 等最小依赖集。
        set(GO2CPP_DISTRIBUTED_BASE_LIB go2cpp_socket_bufio_standalone)

    elseif (TARGET go2cpp_network AND TARGET go2cpp_context AND TARGET go2cpp_scheduler)
        set(GO2CPP_DISTRIBUTED_BASE_LIB
            go2cpp_network go2cpp_context go2cpp_scheduler go2cpp_io go2cpp_sync)
    else()
        message(WARNING
            "distributed: no network/runtime target available (need go2cpp_relay or "
            "go2cpp_socket_bufio_standalone); distributed is disabled")
        return()
    endif()
else()
    message(WARNING "distributed: relay sources are missing; distributed is disabled")
    return()
endif()

add_library(go2cpp_distributed ${GO2CPP_DISTRIBUTED_SOURCES})
add_library(go2cpp::distributed ALIAS go2cpp_distributed)
target_include_directories(go2cpp_distributed
    PUBLIC
        $<BUILD_INTERFACE:${GO2CPP_DISTRIBUTED_ROOT}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(go2cpp_distributed PUBLIC cxx_std_17)
target_compile_options(go2cpp_distributed PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(go2cpp_distributed PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(go2cpp_distributed PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
# TCP 协调者需要 go2cpp::net::Socket/Address 等共享符号，所有分支统一补链接。
if (TARGET go2cpp_network)
    list(APPEND GO2CPP_DISTRIBUTED_BASE_LIB go2cpp_network)
endif()
target_link_libraries(go2cpp_distributed
    PUBLIC ${GO2CPP_DISTRIBUTED_RELAY_LIB} ${GO2CPP_DISTRIBUTED_BASE_LIB}
           Threads::Threads)
# P0：Coordinator / LeaseSession / Lock 接入进程级 metrics 注册表。
if (TARGET go2cpp_metrics)
    target_link_libraries(go2cpp_distributed PUBLIC go2cpp_metrics)
else()
    message(STATUS "distributed: go2cpp_metrics missing, metrics hooks stay unused")
endif()

if (GO2CPP_DISTRIBUTED_TESTS)
    enable_testing()
    function(go2cpp_distributed_add_test target)
        add_executable(${target} ${ARGN})
        target_link_libraries(${target} PRIVATE go2cpp_distributed)
        target_include_directories(${target} PRIVATE ${GO2CPP_DISTRIBUTED_ROOT}/tests)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endfunction()

    go2cpp_distributed_add_test(go2cpp_distributed_registry_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_registry.cpp)
    go2cpp_distributed_add_test(go2cpp_distributed_lock_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_lock.cpp)
    go2cpp_distributed_add_test(go2cpp_distributed_election_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_election.cpp)
    go2cpp_distributed_add_test(go2cpp_distributed_random_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_random.cpp)
    go2cpp_distributed_add_test(go2cpp_distributed_concurrent_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_concurrent.cpp)
    go2cpp_distributed_add_test(go2cpp_distributed_transport_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_transport.cpp)
    go2cpp_distributed_add_test(go2cpp_distributed_scenarios_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_scenarios.cpp)
    go2cpp_distributed_add_test(go2cpp_distributed_node_id_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_node_id.cpp)
    go2cpp_distributed_add_test(go2cpp_distributed_trace_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_trace.cpp)
    go2cpp_distributed_add_test(go2cpp_distributed_metadata_store_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_metadata_store.cpp)
    go2cpp_distributed_add_test(go2cpp_distributed_coordinator_snapshot_test
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_coordinator_snapshot.cpp)
    if (TARGET go2cpp_rpc)
        # trace 透传用例直接验证 rpc 请求帧编解码（P0）。
        target_link_libraries(go2cpp_distributed_trace_test PRIVATE go2cpp_rpc)
    endif()

    add_test(NAME distributed_registry COMMAND go2cpp_distributed_registry_test)
    add_test(NAME distributed_lock COMMAND go2cpp_distributed_lock_test)
    add_test(NAME distributed_election COMMAND go2cpp_distributed_election_test)
    add_test(NAME distributed_random COMMAND go2cpp_distributed_random_test)
    add_test(NAME distributed_concurrent COMMAND go2cpp_distributed_concurrent_test)
    add_test(NAME distributed_transport COMMAND go2cpp_distributed_transport_test)
    add_test(NAME distributed_scenarios COMMAND go2cpp_distributed_scenarios_test)
    add_test(NAME distributed_node_id COMMAND go2cpp_distributed_node_id_test)
    add_test(NAME distributed_trace COMMAND go2cpp_distributed_trace_test)
    add_test(NAME distributed_metadata_store
             COMMAND go2cpp_distributed_metadata_store_test)
    add_test(NAME distributed_coordinator_snapshot
             COMMAND go2cpp_distributed_coordinator_snapshot_test)
    set_tests_properties(
        distributed_registry distributed_lock distributed_election
        distributed_random distributed_concurrent distributed_transport
        distributed_scenarios distributed_node_id distributed_trace
        distributed_metadata_store distributed_coordinator_snapshot
        PROPERTIES LABELS distributed TIMEOUT 180)

    if (UNIX AND GO2CPP_DISTRIBUTED_MULTIPROCESS_TESTS)
        # 多进程用例通过 /proc/self/exe 重新执行自身（不用 fork 后直接跑
        # 线程代码，避免继承父进程中被锁住的 mutex）。
        go2cpp_distributed_add_test(go2cpp_distributed_multiprocess_test
            ${GO2CPP_DISTRIBUTED_TESTS_DIR}/test_multiprocess.cpp)
        add_test(NAME distributed_multiprocess
            COMMAND go2cpp_distributed_multiprocess_test)
        set_tests_properties(distributed_multiprocess
            PROPERTIES LABELS "distributed;long" TIMEOUT 240)
    else()
        message(STATUS "distributed: multi-process tests skipped (requires UNIX)")
    endif()

    # 压测/基准：只构建，不注册 ctest。
    go2cpp_distributed_add_test(go2cpp_distributed_bench
        ${GO2CPP_DISTRIBUTED_TESTS_DIR}/bench_distributed.cpp)

    message(STATUS "distributed: ON (tests: registry/lock/election/random/concurrent/transport/scenarios)")
endif()
