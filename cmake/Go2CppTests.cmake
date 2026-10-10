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
        concurrency/fiber_sync_demo
        concurrency/managed_pipeline_demo
        concurrency/dynamic_gmp_demo
        concurrency/mixed_runtime_demo
        concurrency/beginner_demo
        tools/control_flow_demo
        network/io_wait_many_demo
        network/network_demo
        network/websocket_demo
        network/sse_demo
        concurrency/context_rollback_demo
        logging/log_config_demo
        concurrency/timer_task_demo)
    if (TARGET go2cpp_hook)
        list(APPEND GO2CPP_EXAMPLES network/io_hook_demo)
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

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(go2cpp_io_epoll_fair_benchmark
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/io_epoll_fair_benchmark.cpp)
        target_link_libraries(go2cpp_io_epoll_fair_benchmark PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_io_epoll_fair_benchmark PRIVATE
            -O2 -Wall -Wextra -Wpedantic)
    endif()
endif()
