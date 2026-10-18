if (GO2CPP_BUILD_TESTS)
    enable_testing()

    add_executable(go2cpp_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_error.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_control_flow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_context.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_scheduler_stress.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_dynamic_scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_fiber.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_sync.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_future.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_io.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_network.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_sse.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_timer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_beginner_api.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/logging/test_log_config.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_errors_ns.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_strings.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_strconv.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_fmt.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_streams.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_time.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_event_go.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_hybrid_gate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_parking_condition.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_thread_policy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_fiber_local.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_fd_manager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_fiber.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_cli.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_sync.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_lock_selector.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_events.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_breakpoints.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_signal_dump.cpp
    )
    target_link_libraries(go2cpp_tests PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    if (TARGET go2cpp_hook)
        target_sources(go2cpp_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_hook.cpp)
        target_compile_definitions(go2cpp_tests PRIVATE GO2CPP_TEST_HOOK=1)
    endif()
    target_compile_options(go2cpp_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_tests COMMAND go2cpp_tests)
    # 超时从 30 -> 60 -> 120：go2cpp_tests 是全部单元测试的集合。
    # 随调试模块陆续新增测试（debug_* 七个）后整体耗时约 18~30s，且**偶发**更高：
    # 实测撞过两次 Timeout（一次与 build-bench 并行、一次原因不明），
    # 而单独复跑每次都能在 30s 左右通过。为了不让偶发抖动把整条 CI 判成失败，
    # 这里给足余量到 120s —— 注意这**不是**在掩盖真实性能退化：
    # 若真的持续变慢，120s 同样会被打爆，只是不再因一次抖动就误报。
    set_tests_properties(go2cpp_tests PROPERTIES TIMEOUT 120)
    # T-08 回归：未知的 GO2CPP_TEST_FILTER 必须被拒绝。此前它会静默跑 0 个模块
    # 却返回 0，把「空跑」伪装成「通过」。
    # 用 `sh -c` 把「退出码必须是 2」显式断言出来，而不是用 WILL_FAIL：
    # 后者与 PASS_REGULAR_EXPRESSION 会互相反转（正则匹配=通过，WILL_FAIL 再反转
    # 一次=失败），实测该组合无法表达「退出码非零且输出含指定信息」。
    add_test(NAME go2cpp_unknown_filter_rejected
             COMMAND sh -c "GO2CPP_TEST_FILTER=__no_such_module__ $<TARGET_FILE:go2cpp_tests>; test $? -eq 2")
    set_tests_properties(go2cpp_unknown_filter_rejected PROPERTIES
        PASS_REGULAR_EXPRESSION "未匹配任何测试模块"
        TIMEOUT 30)
    # 配对验证：已知的 filter 仍然只跑该模块并正常通过。
    add_test(NAME go2cpp_known_filter_runs COMMAND go2cpp_tests)
    set_tests_properties(go2cpp_known_filter_runs PROPERTIES
        ENVIRONMENT "GO2CPP_TEST_FILTER=error"
        PASS_REGULAR_EXPRESSION "error values and chains"
        TIMEOUT 30)

    add_executable(go2cpp_scheduler_smoke
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/scheduler_smoke.cpp)
    target_compile_definitions(go2cpp_scheduler_smoke PRIVATE
        GO2CPP_SCHEDULER_SMOKE_MAIN=1)
    target_link_libraries(go2cpp_scheduler_smoke PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_scheduler_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_scheduler_smoke PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_scheduler_smoke COMMAND go2cpp_scheduler_smoke)
    set_tests_properties(go2cpp_scheduler_smoke PROPERTIES TIMEOUT 30)

    add_executable(go2cpp_high_load_stress
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/high_load_stress.cpp)
    target_link_libraries(go2cpp_high_load_stress PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_high_load_stress PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_high_load_stress PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_high_load_stress COMMAND go2cpp_high_load_stress)

    add_executable(go2cpp_timer_stress
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_timer_stress.cpp)
    target_link_libraries(go2cpp_timer_stress PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_timer_stress PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_timer_stress COMMAND go2cpp_timer_stress)
    set_tests_properties(go2cpp_timer_stress PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_state_machine
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_state_machine.cpp)
    target_link_libraries(go2cpp_state_machine PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_state_machine PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_state_machine COMMAND go2cpp_state_machine)
    set_tests_properties(go2cpp_state_machine PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_scheduler_queue_latency
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_scheduler_queue_latency.cpp)
    target_link_libraries(go2cpp_scheduler_queue_latency PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_scheduler_queue_latency PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_scheduler_queue_latency
             COMMAND go2cpp_scheduler_queue_latency)
    set_tests_properties(go2cpp_scheduler_queue_latency PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_hpack_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_hpack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/hpack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/huffman.cpp)
    target_include_directories(go2cpp_hpack_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(go2cpp_hpack_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_hpack_test COMMAND go2cpp_hpack_test)
    set_tests_properties(go2cpp_hpack_test PROPERTIES TIMEOUT 180)

    add_executable(go2cpp_frame_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/frame.cpp)
    target_include_directories(go2cpp_frame_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(go2cpp_frame_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_frame_test COMMAND go2cpp_frame_test)
    set_tests_properties(go2cpp_frame_test PROPERTIES TIMEOUT 180)

    add_executable(go2cpp_stream_state_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_stream_state.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/stream_state.cpp)
    target_include_directories(go2cpp_stream_state_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(go2cpp_stream_state_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_stream_state_test COMMAND go2cpp_stream_state_test)
    set_tests_properties(go2cpp_stream_state_test PROPERTIES TIMEOUT 180)

    add_executable(go2cpp_connection_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/hpack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/huffman.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/stream_state.cpp)
    target_include_directories(go2cpp_connection_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(go2cpp_connection_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_connection_test COMMAND go2cpp_connection_test)
    set_tests_properties(go2cpp_connection_test PROPERTIES TIMEOUT 180)

    # ---- os 模块（开荒版）：文件 + 环境变量 + 命令行参数 ----
    add_executable(go2cpp_os_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/os/test_os_file.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/dir.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/file.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/os.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/config.cpp)
    target_include_directories(go2cpp_os_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(go2cpp_os_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    target_link_libraries(go2cpp_os_test PRIVATE go2cpp_config)
    add_test(NAME go2cpp_os_test COMMAND go2cpp_os_test)
    set_tests_properties(go2cpp_os_test PROPERTIES TIMEOUT 180)

    add_executable(go2cpp_os_dir_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/os/test_os_dir.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/dir.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/file.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/os.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/config.cpp)
    target_include_directories(go2cpp_os_dir_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(go2cpp_os_dir_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    target_link_libraries(go2cpp_os_dir_test PRIVATE go2cpp_config)
    add_test(NAME go2cpp_os_dir_test COMMAND go2cpp_os_dir_test)
    set_tests_properties(go2cpp_os_dir_test PROPERTIES TIMEOUT 180)

    add_executable(go2cpp_os_hook_conflict_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/os/test_os_hook_conflict.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/dir.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/file.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/os.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/os/config.cpp)
    target_link_libraries(go2cpp_os_hook_conflict_test PRIVATE go2cpp_config go2cpp_hook)
    target_compile_options(go2cpp_os_hook_conflict_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_os_hook_conflict_test COMMAND go2cpp_os_hook_conflict_test)
    set_tests_properties(go2cpp_os_hook_conflict_test PROPERTIES TIMEOUT 120)


    add_executable(go2cpp_hook_ops_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_hook_ops.cpp)
    target_link_libraries(go2cpp_hook_ops_test PRIVATE go2cpp_hook)
    target_compile_options(go2cpp_hook_ops_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_hook_ops_test COMMAND go2cpp_hook_ops_test)
    set_tests_properties(go2cpp_hook_ops_test PROPERTIES TIMEOUT 120)

    add_executable(go2cpp_hook_raw_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_hook_raw.cpp)
    target_include_directories(go2cpp_hook_raw_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(go2cpp_hook_raw_test PRIVATE
        go2cpp_hook go2cpp_io go2cpp_scheduler go2cpp_fiber
        go2cpp_sync go2cpp_error)
    target_compile_options(go2cpp_hook_raw_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_hook_raw_test COMMAND go2cpp_hook_raw_test)
    set_tests_properties(go2cpp_hook_raw_test PROPERTIES TIMEOUT 120)

    # hook 正确性验证：结合 os::File / hook / 网络 socket，用心跳 Fiber 判定
    # 「park 了 Fiber」还是「阻塞了 worker」。验证使用者的红线：
    # hook 绝不能影响正常的系统调用（非 socket fd 必须零影响）。
    add_executable(go2cpp_hook_correctness_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_hook_correctness.cpp)
    target_include_directories(go2cpp_hook_correctness_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(go2cpp_hook_correctness_test PRIVATE
        go2cpp_hook go2cpp_os go2cpp_io go2cpp_network go2cpp_config
        go2cpp_scheduler go2cpp_fiber go2cpp_sync go2cpp_error)
    target_compile_options(go2cpp_hook_correctness_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_hook_correctness_test COMMAND go2cpp_hook_correctness_test)
    set_tests_properties(go2cpp_hook_correctness_test PROPERTIES TIMEOUT 180)


    # nanosleep / usleep / clock_nanosleep 的 hook 行为：Fiber 里应 park，
    # 普通线程里真阻塞；clock_nanosleep 还要验证 TIMER_ABSTIME 的两种分支。
    add_executable(go2cpp_hook_sleep_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_hook_sleep.cpp)
    target_link_libraries(go2cpp_hook_sleep_test PRIVATE go2cpp::runtime go2cpp_hook pthread)
    target_include_directories(go2cpp_hook_sleep_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME go2cpp_hook_sleep_test COMMAND go2cpp_hook_sleep_test)
    set_tests_properties(go2cpp_hook_sleep_test PROPERTIES TIMEOUT 180)


        add_executable(go2cpp_fiber_transport_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_fiber_transport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/hpack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/huffman.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/stream_state.cpp)
    target_include_directories(go2cpp_fiber_transport_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(go2cpp_fiber_transport_test PRIVATE
        go2cpp_scheduler go2cpp_network go2cpp_io go2cpp_hook
        go2cpp_fiber go2cpp_sync)
    target_compile_options(go2cpp_fiber_transport_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_fiber_transport_test
             COMMAND go2cpp_fiber_transport_test)
    set_tests_properties(go2cpp_fiber_transport_test PROPERTIES TIMEOUT 180)

    # ---- HTTP/2 压力与边界测试（子代理新增）----
    add_executable(go2cpp_http2_stress
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_http2_stress.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/hpack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/huffman.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/stream_state.cpp)
    target_include_directories(go2cpp_http2_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(go2cpp_http2_stress PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_http2_stress COMMAND go2cpp_http2_stress)
    set_tests_properties(go2cpp_http2_stress PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_http2_boundary
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_http2_boundary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/hpack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/huffman.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/stream_state.cpp)
    target_include_directories(go2cpp_http2_boundary PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(go2cpp_http2_boundary PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_http2_boundary COMMAND go2cpp_http2_boundary)
    set_tests_properties(go2cpp_http2_boundary PROPERTIES TIMEOUT 180)

    # ---- 状态机穷举矩阵（状态机正确性 + 并发风险审查新增）----
    # 说明：矩阵测试按「N 状态 x M 事件」逐格断言合法/非法转移；复现测试
    # 断言期望语义，缺陷修复前用 DISABLED 注册，避免把已知缺陷算成回归。
    add_executable(go2cpp_state_matrix
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_state_machine_matrix.cpp)
    target_link_libraries(go2cpp_state_matrix PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_state_matrix PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_state_matrix COMMAND go2cpp_state_matrix)
    set_tests_properties(go2cpp_state_matrix PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_http2_state_matrix
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_http2_state_matrix.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/hpack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/huffman.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/stream_state.cpp)
    target_include_directories(go2cpp_http2_state_matrix PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(go2cpp_http2_state_matrix PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_http2_state_matrix PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_http2_state_matrix COMMAND go2cpp_http2_state_matrix)
    set_tests_properties(go2cpp_http2_state_matrix PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_http2_state_desync_repro
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_http2_state_desync_repro.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/hpack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/huffman.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/stream_state.cpp)
    target_include_directories(go2cpp_http2_state_desync_repro PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(go2cpp_http2_state_desync_repro PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_http2_state_desync_repro PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_http2_state_desync_repro
             COMMAND go2cpp_http2_state_desync_repro)
    # R1/R2/R13 修复后已转正：本测试从"缺陷复现"变成"回归测试"，
    # 任何一条断言回退都会让 ctest 直接失败。
    set_tests_properties(go2cpp_http2_state_desync_repro PROPERTIES
        TIMEOUT 120)

    # ---- log 缓冲区与 Formatter（子代理新增）----
    add_executable(go2cpp_log_buffer_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/logging/test_log_buffer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/logging/log.cpp)
    target_include_directories(go2cpp_log_buffer_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(go2cpp_log_buffer_test PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_log_buffer_test COMMAND go2cpp_log_buffer_test)
    set_tests_properties(go2cpp_log_buffer_test PROPERTIES TIMEOUT 120)



    # ---- math / math/rand 模块（math 委托 libm；rand 为 xoshiro256**）----
    # 两个目标只链接各自的模块库，不拖入整个 runtime：math/rand 是基础库，
    # 测试也应当能在最小依赖下独立编译运行。
    add_executable(go2cpp_math_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/math/test_math.cpp)
    target_link_libraries(go2cpp_math_test PRIVATE go2cpp_math)
    target_compile_options(go2cpp_math_test PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_math_test COMMAND go2cpp_math_test)
    set_tests_properties(go2cpp_math_test PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_rand_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/math/test_rand.cpp)
    target_link_libraries(go2cpp_rand_test PRIVATE go2cpp_rand)
    target_compile_options(go2cpp_rand_test PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_rand_test COMMAND go2cpp_rand_test)
    set_tests_properties(go2cpp_rand_test PROPERTIES TIMEOUT 300)

    # ---- path / path/filepath 模块 ----
    # path 是纯字符串模块，测试不拖入 runtime；filepath 测试走真实文件系统。
    # 对拍数据由 tests/path/scripts/cross_check.py 生成（Go path.Clean 为 oracle），
    # 平时 ctest 只读数据文件，不依赖本机安装 Go。
    add_executable(go2cpp_path_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/path/test_path.cpp)
    target_link_libraries(go2cpp_path_test PRIVATE go2cpp_path)
    target_compile_definitions(go2cpp_path_test PRIVATE
        GO2CPP_PATH_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/path/testdata")
    target_compile_options(go2cpp_path_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_path_test COMMAND go2cpp_path_test)
    set_tests_properties(go2cpp_path_test PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_path_filepath_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/path/test_path_filepath.cpp)
    target_link_libraries(go2cpp_path_filepath_test PRIVATE go2cpp_path)
    target_compile_options(go2cpp_path_filepath_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_path_filepath_test COMMAND go2cpp_path_filepath_test)
    set_tests_properties(go2cpp_path_filepath_test PROPERTIES TIMEOUT 300)

    # ---- container 模块：Heap / List / Ring ----
    # 纯模板容器，测试直接编译头文件；含单元、随机、6 个真实场景与 1M 规模压测。
    add_executable(go2cpp_container_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/container/container_test_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/container/test_heap.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/container/test_list.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/container/test_ring.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/container/test_random.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/container/test_scenarios.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/container/test_stress.cpp)
    target_include_directories(go2cpp_container_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(go2cpp_container_test PRIVATE go2cpp_container)
    target_compile_options(go2cpp_container_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_container_test COMMAND go2cpp_container_test)
    set_tests_properties(go2cpp_container_test PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_config_registry_macro_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/config/test_config_registry_macro.cpp)
    target_link_libraries(go2cpp_config_registry_macro_test PRIVATE go2cpp_config)
    target_compile_options(go2cpp_config_registry_macro_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_config_registry_macro_test
             COMMAND go2cpp_config_registry_macro_test)
    set_tests_properties(go2cpp_config_registry_macro_test PROPERTIES TIMEOUT 120)

    # ---- compress 模块：flate / zlib / gzip（薄封装系统 zlib）----
    # 一个可执行文件包含全部测试段：单元、随机、系统 gzip 互操作、8 个真实场景
    # 与低熵/高熵压测。拆成多个 target 只会重复编译模板实例，没有收益。
    # 依赖：ZLIB 由 go2cpp_compress 的 PUBLIC 依赖传递（ZLIB::ZLIB）。
    add_executable(go2cpp_compress_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compress/compress_test_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compress/compress_test_support.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compress/test_flate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compress/test_zlib.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compress/test_gzip.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compress/test_random.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compress/test_interop.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compress/test_scenarios.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compress/test_stress.cpp)
    target_include_directories(go2cpp_compress_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compress)
    target_link_libraries(go2cpp_compress_test PRIVATE go2cpp_compress)
    target_compile_options(go2cpp_compress_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_compress_test COMMAND go2cpp_compress_test)
    # 300s：压测 + 随机测试在 -O2 下约 10s，留足慢机器/Sanitizer 余量。
    set_tests_properties(go2cpp_compress_test PROPERTIES TIMEOUT 300)

    # ---- encoding 模块：base64 / hex / binary / csv ----
    # 四个子模块只依赖 error；测试是单个可执行文件，内含单元 / 随机 /
    # 6 个真实场景 / 1 MiB 压测。Python 对拍数据由
    # tests/encoding/scripts/gen_python_vectors.py 生成，ctest 只读文件。
    add_executable(go2cpp_encoding_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding/encoding_test_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding/test_base64.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding/test_hex.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding/test_binary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding/test_csv.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding/test_random.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding/test_scenarios.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding/test_stress.cpp)
    target_include_directories(go2cpp_encoding_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(go2cpp_encoding_test PRIVATE go2cpp_encoding)
    target_compile_definitions(go2cpp_encoding_test PRIVATE
        GO2CPP_ENCODING_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding/testdata")
    target_compile_options(go2cpp_encoding_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_encoding_test COMMAND go2cpp_encoding_test)
    set_tests_properties(go2cpp_encoding_test PROPERTIES TIMEOUT 300)

    # ---- hash 模块：CRC32 / Adler32 / MD5 / SHA-1 / SHA-256 ----
    # oracle 由 tests/hash/scripts/gen_oracle.py 预生成（Python zlib/hashlib +
    # 逐位 CRC32C/K 参考实现），ctest 运行时不依赖本机 Python。
    add_executable(go2cpp_hash_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/hash/hash_test_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/hash/test_hash_vectors.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/hash/test_hash_random.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/hash/test_hash_scenarios.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/hash/test_hash_stress.cpp)
    target_link_libraries(go2cpp_hash_test PRIVATE go2cpp_hash)
    target_include_directories(go2cpp_hash_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(go2cpp_hash_test PRIVATE
        GO2CPP_HASH_ORACLE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/hash/testdata/oracle.tsv
        GO2CPP_HASH_ORACLE_RANDOM_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/hash/testdata/oracle_random.tsv)
    target_compile_options(go2cpp_hash_test PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_hash_test COMMAND go2cpp_hash_test)
    set_tests_properties(go2cpp_hash_test PROPERTIES TIMEOUT 300)

    set_tests_properties(go2cpp_high_load_stress PROPERTIES TIMEOUT 180)
endif()

    # ---- bufio 模块：带缓冲的 IO（语义对齐 Go bufio）----
    add_executable(go2cpp_bufio_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/bufio/test_bufio.cpp)
    target_link_libraries(go2cpp_bufio_test PRIVATE go2cpp_bufio)
    target_include_directories(go2cpp_bufio_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_bufio_test PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_bufio_test COMMAND go2cpp_bufio_test)
    set_tests_properties(go2cpp_bufio_test PROPERTIES TIMEOUT 180)

    add_executable(go2cpp_bufio_stress
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/bufio/test_bufio_stress.cpp)
    target_link_libraries(go2cpp_bufio_stress PRIVATE go2cpp_bufio)
    target_include_directories(go2cpp_bufio_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_bufio_stress PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_bufio_stress COMMAND go2cpp_bufio_stress)
    set_tests_properties(go2cpp_bufio_stress PROPERTIES TIMEOUT 300)

if (GO2CPP_BUILD_EXAMPLES)
    # 示例按模块目录维护；目标名取文件名以保持旧版构建接口稳定.
    set(GO2CPP_EXAMPLES
        debug/debug_module_demo
        concurrency/runtime_demo
        concurrency/runtime_basic_demo
        concurrency/runtime_full
        concurrency/fiber_sync_demo
        concurrency/managed_pipeline_demo
        concurrency/dynamic_gmp_demo
        concurrency/dynamic_gmp_demo_new
        concurrency/dynamic_gmp_full
        concurrency/mixed_runtime_demo
        concurrency/beginner_demo
        concurrency/channel_demo
        concurrency/channel_full
        concurrency/context_demo
        concurrency/context_full
        concurrency/sync_demo
        concurrency/sync_full
        concurrency/future_full
        config/config_demo
        config/config_full
        config/runtime_config_demo
        config/runtime_config_full
        config/config_center_demo
        config/config_center_full
        tools/control_flow_demo
        tools/control_flow_full
        network/io_wait_many_demo
        network/network_demo
        network/websocket_demo
        network/websocket_full
        network/sse_demo
        network/sse_basic_demo
        network/sse_full
        concurrency/context_rollback_demo
        logging/log_config_demo
        logging/log_demo
        logging/log_full
        concurrency/timer_task_demo
        concurrency/timer_full
        concurrency/parking_condition_demo
        concurrency/parking_condition_full
        concurrency/scheduler_demo
        concurrency/scheduler_full
        concurrency/thread_policy_demo
        concurrency/thread_policy_full
        concurrency/fiber_local_demo
        concurrency/fiber_local_full
        concurrency/fiber_stack_limit_demo
        concurrency/fiber_stack_limit_full
        concurrency/future_demo
        concurrency/hybrid_mutex_demo
        network/fd_manager_demo
        network/http_demo
        network/http_full
        network/fd_manager_full
        network/io_backend_demo
        network/io_backend_basic_demo
        network/io_backend_full
        network/address_bytearray_demo
        network/address_full
        network/bytearray_full
        tools/errors_demo
        tools/error_full
        tools/strings_demo
        tools/strings_full
        tools/strconv_demo
        tools/strconv_full
        tools/fmt_demo
        tools/fmt_full
        tools/streams_demo
        tools/streams_full
        tools/time_demo
        tools/time_full)
    if (TARGET go2cpp_hook)
        list(APPEND GO2CPP_EXAMPLES network/io_hook_demo)
        list(APPEND GO2CPP_EXAMPLES network/io_hook_full)
    endif()
    foreach(example_name IN LISTS GO2CPP_EXAMPLES)
        get_filename_component(example_target ${example_name} NAME_WE)
        add_executable(go2cpp_${example_target}
            ${CMAKE_CURRENT_SOURCE_DIR}/example/${example_name}.cpp)
        target_link_libraries(go2cpp_${example_target} PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_${example_target} PRIVATE
            -Wall -Wextra -Wpedantic)
        if (GO2CPP_BUILD_TESTS)
            add_test(NAME go2cpp_${example_target} COMMAND go2cpp_${example_target})
            set_tests_properties(go2cpp_${example_target} PROPERTIES TIMEOUT 30)
        endif()
    endforeach()
endif()

if (GO2CPP_BUILD_BENCHMARKS)

    add_executable(go2cpp_channel_vs_queue_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/channel_vs_queue_benchmark.cpp)
    target_link_libraries(go2cpp_channel_vs_queue_benchmark PRIVATE go2cpp_runtime)
    add_executable(go2cpp_debug_observer_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/debug_observer_overhead_benchmark.cpp)
    target_link_libraries(go2cpp_debug_observer_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_debug_observer_benchmark PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_executable(go2cpp_debug_lock_selector_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/debug_lock_selector_overhead_benchmark.cpp)
    target_link_libraries(go2cpp_debug_lock_selector_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_debug_lock_selector_benchmark PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_executable(go2cpp_debug_event_collector_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/debug_event_collector_benchmark.cpp)
    target_link_libraries(go2cpp_debug_event_collector_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_debug_event_collector_benchmark PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_executable(go2cpp_parking_condition_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/parking_condition_benchmark.cpp)
    target_link_libraries(go2cpp_parking_condition_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_parking_condition_benchmark PRIVATE -O2 -Wall -Wextra -Wpedantic)
    target_compile_options(go2cpp_channel_vs_queue_benchmark PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_executable(go2cpp_timer_task_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/timer_task_benchmark.cpp)
    target_link_libraries(go2cpp_timer_task_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_timer_task_benchmark PRIVATE -O2 -Wall -Wextra -Wpedantic)

    add_executable(go2cpp_gmp_compute_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/gmp_compute_benchmark.cpp)
    target_link_libraries(go2cpp_gmp_compute_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_gmp_compute_benchmark PRIVATE
        -O2 -Wall -Wextra -Wpedantic)

    add_executable(go2cpp_fiber_stack_probe
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/fiber_stack_probe.cpp)
    target_link_libraries(go2cpp_fiber_stack_probe PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_fiber_stack_probe PRIVATE
        -O2 -Wall -Wextra -Wpedantic)

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(go2cpp_io_epoll_fair_benchmark
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/io_epoll_fair_benchmark.cpp)
        target_link_libraries(go2cpp_io_epoll_fair_benchmark PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_io_epoll_fair_benchmark PRIVATE
            -O2 -Wall -Wextra -Wpedantic)

        add_executable(go2cpp_mixed_random_benchmark
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/mixed_random_benchmark.cpp)
        target_link_libraries(go2cpp_mixed_random_benchmark PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_mixed_random_benchmark PRIVATE
            -O2 -Wall -Wextra -Wpedantic)
    endif()
endif()
