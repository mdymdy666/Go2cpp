# ---------------------------------------------------------------------------
# Relay 通用组件（JinShan REQ-003 Hub / REQ-005 Lease+Heartbeat）
#
# 定位：与 network/app 平级的通用层，不依赖 Bento/App，默认随主构建打开。
# 依赖方向：Hub/Lease 只依赖标准库与 streams::Closer；Heartbeat 额外依赖
# go2cpp_context（`Start(Context*, ...)` 的取消源）。
#
# 目标策略：优先复用 RelayPipe 已创建的 `go2cpp_relay` 目标（追加源文件），
# 从而一个 relay 模块只产出一个库。只有目标还不存在时才自己创建。
#
# 测试策略：每个用例单独成可执行文件并用 ctest 的 `relay` 标签分组，不并入
# 现有 go2cpp_tests 巨型二进制——relay 是本轮新增模块，独立目标可以单独
# 构建/重跑，也不会改变现有 ctest 用例的数量与语义。
#
# 本文件必须在 cmake/Go2CppModules.cmake 之后 include（需要 go2cpp_context
# 与 GNUInstallDirs 变量）；入口是 cmake/Go2CppRelay.cmake。
# ---------------------------------------------------------------------------

option(GO2CPP_WITH_RELAY "Build go2cpp_relay (Hub / Lease / Heartbeat)" ON)
option(GO2CPP_RELAY_TSAN "Also build and register the relay Hub ThreadSanitizer test" ON)

if (NOT GO2CPP_WITH_RELAY)
    message(STATUS "relay: OFF (GO2CPP_WITH_RELAY=OFF)")
    return()
endif()

set(GO2CPP_RELAY_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(GO2CPP_RELAY_TESTS_DIR ${GO2CPP_RELAY_ROOT}/tests/relay)

if (TARGET go2cpp_relay)
    # RelayPipe 已经建好目标：只追加 Hub/Lease 源文件与必要依赖。
    message(STATUS "relay: 复用已有 go2cpp_relay 目标（追加 Hub/Lease 源）")
    target_sources(go2cpp_relay PRIVATE
        ${GO2CPP_RELAY_ROOT}/src/relay/hub.cpp
        ${GO2CPP_RELAY_ROOT}/src/relay/lease.cpp
    )
    target_compile_features(go2cpp_relay PUBLIC cxx_std_17)
    target_link_libraries(go2cpp_relay PUBLIC go2cpp_context Threads::Threads)
else()
    add_library(go2cpp_relay
        ${GO2CPP_RELAY_ROOT}/src/relay/hub.cpp
        ${GO2CPP_RELAY_ROOT}/src/relay/lease.cpp
    )
    add_library(go2cpp::relay ALIAS go2cpp_relay)
    target_include_directories(go2cpp_relay
        PUBLIC
            $<BUILD_INTERFACE:${GO2CPP_RELAY_ROOT}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_features(go2cpp_relay PUBLIC cxx_std_17)
    set_target_properties(go2cpp_relay PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION 0)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(go2cpp_relay PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
    target_compile_options(go2cpp_relay PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(go2cpp_relay PUBLIC go2cpp_context Threads::Threads)
endif()

if (GO2CPP_BUILD_TESTS)
    # 统一的测试目标写法：链接 go2cpp_relay，并允许 include tests/test_support.hpp。
    function(go2cpp_relay_add_test_target target)
        add_executable(${target} ${ARGN})
        target_link_libraries(${target} PRIVATE go2cpp_relay)
        target_include_directories(${target} PRIVATE ${GO2CPP_RELAY_ROOT}/tests)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endfunction()

    go2cpp_relay_add_test_target(go2cpp_relay_hub_test
        ${GO2CPP_RELAY_TESTS_DIR}/test_hub.cpp)
    go2cpp_relay_add_test_target(go2cpp_relay_lease_test
        ${GO2CPP_RELAY_TESTS_DIR}/test_lease.cpp)
    go2cpp_relay_add_test_target(go2cpp_relay_heartbeat_test
        ${GO2CPP_RELAY_TESTS_DIR}/test_heartbeat.cpp)
    go2cpp_relay_add_test_target(go2cpp_relay_scenarios_test
        ${GO2CPP_RELAY_TESTS_DIR}/test_relay_scenarios.cpp)

    # 真实 90 秒租约用例：单独成目标并打 long 标签；跳过慢用例用 `ctest -LE long`。
    go2cpp_relay_add_test_target(go2cpp_relay_lease_90s_test
        ${GO2CPP_RELAY_TESTS_DIR}/test_lease_90s.cpp)

    # 墙钟跳变垫片：只在测试进程内通过 LD_PRELOAD 生效，不链接 relay 库。
    add_library(go2cpp_relay_fake_clock SHARED
        ${GO2CPP_RELAY_TESTS_DIR}/fake_clock_jump.cpp)
    target_link_libraries(go2cpp_relay_fake_clock PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(go2cpp_relay_fake_clock PRIVATE -Wall -Wextra -Wpedantic)

    go2cpp_relay_add_test_target(go2cpp_relay_lease_clockjump_test
        ${GO2CPP_RELAY_TESTS_DIR}/test_lease_clockjump.cpp)
    target_link_libraries(go2cpp_relay_lease_clockjump_test PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(go2cpp_relay_lease_clockjump_test go2cpp_relay_fake_clock)

    # 压测/基准：只构建，不注册 ctest（避免把计时噪声带进回归）。
    go2cpp_relay_add_test_target(go2cpp_relay_bench
        ${GO2CPP_RELAY_TESTS_DIR}/bench_relay.cpp)

    add_test(NAME relay_hub COMMAND go2cpp_relay_hub_test)
    add_test(NAME relay_lease COMMAND go2cpp_relay_lease_test)
    add_test(NAME relay_heartbeat COMMAND go2cpp_relay_heartbeat_test)
    add_test(NAME relay_scenarios COMMAND go2cpp_relay_scenarios_test)
    add_test(NAME relay_lease_90s COMMAND go2cpp_relay_lease_90s_test)
    add_test(NAME relay_lease_clockjump
        COMMAND ${CMAKE_COMMAND} -E env
            "LD_PRELOAD=$<TARGET_FILE:go2cpp_relay_fake_clock>"
            "GO2CPP_RELAY_FAKE_CLOCK=1"
            $<TARGET_FILE:go2cpp_relay_lease_clockjump_test>)

    set_tests_properties(relay_hub relay_lease relay_heartbeat relay_scenarios
        PROPERTIES LABELS relay TIMEOUT 180)
    set_tests_properties(relay_lease_clockjump PROPERTIES LABELS relay TIMEOUT 180)
    set_tests_properties(relay_lease_90s PROPERTIES LABELS "relay;long" TIMEOUT 150)

    # TSan：Hub 的 1e5 并发用例用同一份源码单独编译一份带 -fsanitize=thread
    # 的可执行文件。只编译 hub.cpp，不拖入运行时，报告因此只覆盖注册表本身。
    if (GO2CPP_RELAY_TSAN)
        if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND
            CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(go2cpp_relay_hub_tsan_test
                ${GO2CPP_RELAY_TESTS_DIR}/test_hub.cpp
                ${GO2CPP_RELAY_ROOT}/src/relay/hub.cpp)
            target_include_directories(go2cpp_relay_hub_tsan_test PRIVATE
                ${GO2CPP_RELAY_ROOT}/include
                ${GO2CPP_RELAY_ROOT}/tests)
            target_compile_options(go2cpp_relay_hub_tsan_test PRIVATE
                -O1 -g -fsanitize=thread -fno-omit-frame-pointer
                -Wall -Wextra -Wpedantic)
            target_link_options(go2cpp_relay_hub_tsan_test PRIVATE -fsanitize=thread)
            target_link_libraries(go2cpp_relay_hub_tsan_test PRIVATE Threads::Threads)

            # WSL/新内核 mmap_rnd_bits 较大时 TSan 会报
            # "FATAL: ThreadSanitizer: unexpected memory mapping"；
            # 用 setarch 关掉地址随机化再跑。
            find_program(GO2CPP_SETARCH_EXECUTABLE setarch)
            if (GO2CPP_SETARCH_EXECUTABLE)
                add_test(NAME relay_hub_tsan COMMAND
                    ${GO2CPP_SETARCH_EXECUTABLE} --addr-no-randomize
                    $<TARGET_FILE:go2cpp_relay_hub_tsan_test>)
            else()
                add_test(NAME relay_hub_tsan COMMAND
                    $<TARGET_FILE:go2cpp_relay_hub_tsan_test>)
            endif()
            set_tests_properties(relay_hub_tsan PROPERTIES
                LABELS "relay;tsan" TIMEOUT 300)
        else()
            message(STATUS "relay: TSan test skipped (requires GCC/Clang on Linux)")
        endif()
    endif()

    message(STATUS "relay: ON (tests: hub/lease/heartbeat/scenarios/90s/clockjump/tsan)")
endif()
