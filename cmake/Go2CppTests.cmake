if (GO2CPP_BUILD_TESTS)
    enable_testing()

    add_executable(go2cpp_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_error.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_control_flow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_context.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_scheduler.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_event_go.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_hybrid_gate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_parking_condition.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_thread_policy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_fiber_local.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_fd_manager.cpp
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
    set_tests_properties(go2cpp_tests PROPERTIES TIMEOUT 30)
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
    set_tests_properties(go2cpp_high_load_stress PROPERTIES TIMEOUT 180)
endif()

if (GO2CPP_BUILD_EXAMPLES)
    # 示例按模块目录维护；目标名取文件名以保持旧版构建接口稳定.
    set(GO2CPP_EXAMPLES
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
        tools/error_full)
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
    endif()
endif()