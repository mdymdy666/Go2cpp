if (GO2CPP_BUILD_TESTS)
    enable_testing()

    # ---- tools/replay 核心（录制/回放）作为测试用静态库 ----
    # 只在测试目标内部复用 tools/replay 的格式层与回放引擎，不新增
    # ctest 用例（保持 ctest 计数不变），也不改变顶层库的依赖图。
    add_library(go2cpp_replay_core STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/replay/src/format.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/replay/src/recorder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/replay/src/replay_engine.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/replay/src/auto_entry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/replay/src/split.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/replay/src/timer.cpp)
    target_include_directories(go2cpp_replay_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/replay/include)
    target_link_libraries(go2cpp_replay_core PUBLIC go2cpp_runtime)
    target_compile_options(go2cpp_replay_core PRIVATE
        -Wall -Wextra -Wpedantic)
    # 测试构建总是链接 go2cpp_runtime：打开资源事件桥，让 Recorder
    # 能录制 sync/channel 的资源事件（v4）。
    target_compile_definitions(go2cpp_replay_core PRIVATE
        GO2CPP_REPLAY_LINK_RUNTIME=1)

    add_executable(go2cpp_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_error.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_control_flow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_context.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_scheduler_stress.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_terminal_reclaim.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_dynamic_scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_fiber.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_fiber_growable_stack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_sync.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_locks.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_spin_mutex.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_futex_mutex.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_dump.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_timer_debug.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_timer_replay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_config.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_resource_replay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_resources_v5.cpp
    )
    target_link_libraries(go2cpp_tests PRIVATE go2cpp_runtime
        go2cpp_replay_core)
    target_include_directories(go2cpp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    if (TARGET go2cpp_hook)
        target_sources(go2cpp_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_hook.cpp)
        target_compile_definitions(go2cpp_tests PRIVATE GO2CPP_TEST_HOOK=1)
    endif()
    target_compile_options(go2cpp_tests PRIVATE -Wall -Wextra -Wpedantic)
    # v5 资源事件录制/回放 demo：只在 GO2CPP_REPLAY=ON 的构建里编译
    # （OFF 构建没有运行时录制点，demo 只会记录 0 条事件）。
    # 不注册 ctest 用例，因此默认构建的用例数不变。
    if (GO2CPP_REPLAY)
        add_executable(go2cpp_resources_v5_demo
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/resource_v5_demo.cpp)
        target_link_libraries(go2cpp_resources_v5_demo PRIVATE
            go2cpp_runtime go2cpp_replay_core)
        target_compile_options(go2cpp_resources_v5_demo PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    # Doctor 测试只在 GO2CPP_DIAGNOSTIC=ON 时编译。默认 OFF 构建既没有
    # DiagnosticAgent 符号，也没有这些测试（保证 ctest 用例数与默认构建
    # 零开销）。ON 时测试并入同一个 go2cpp_tests，**不新增 add_test**，
    # 因此 ctest 用例总数保持不变。
    if (GO2CPP_DIAGNOSTIC)
        target_sources(go2cpp_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_diagnostic.cpp)

        # ctx.cancel 单测：拒绝路径（bad-argument / not-registered）+
        # gid/name/address 三种成功定位 + 重复取消 + cancel_race。
        # test_diagnostic 的 11 个场景不含 ctx.cancel，这里单独成 ctest，
        # 失败时能直接定位到取消路径而不是整个 go2cpp_tests。
        add_executable(go2cpp_ctx_cancel_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_ctx_cancel.cpp)
        target_link_libraries(go2cpp_ctx_cancel_test PRIVATE go2cpp_runtime)
        target_include_directories(go2cpp_ctx_cancel_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_compile_options(go2cpp_ctx_cancel_test PRIVATE
            -O2 -Wall -Wextra -Wpedantic)
        add_test(NAME go2cpp_ctx_cancel_test COMMAND go2cpp_ctx_cancel_test)
        set_tests_properties(go2cpp_ctx_cancel_test PROPERTIES TIMEOUT 120)

        # 独立 demo 进程：供 tests/debug/run_diagnostic_e2e.sh 从外部诊断。
        # **不注册 ctest**：ctest 用例总数保持不变；端到端验收由脚本执行。
        add_executable(go2cpp_diagnostic_demo
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/diagnostic_deadlock_demo.cpp)
        target_link_libraries(go2cpp_diagnostic_demo PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_diagnostic_demo PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()

    # 并发原语补强：10 类结构压测 / 取消超时 / park-wake 配对 / 资源历史压力。
    add_executable(go2cpp_primitives_stress
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_primitives_stress.cpp)
    target_link_libraries(go2cpp_primitives_stress PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_primitives_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_primitives_stress PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_primitives_stress COMMAND go2cpp_primitives_stress)
    set_tests_properties(go2cpp_primitives_stress PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_primitives_cancel
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_primitives_cancel.cpp)
    target_link_libraries(go2cpp_primitives_cancel PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_primitives_cancel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_primitives_cancel PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_primitives_cancel COMMAND go2cpp_primitives_cancel)
    set_tests_properties(go2cpp_primitives_cancel PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_park_wake_pair
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_park_wake_pair.cpp)
    target_link_libraries(go2cpp_park_wake_pair PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_park_wake_pair PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_park_wake_pair PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_park_wake_pair COMMAND go2cpp_park_wake_pair)
    set_tests_properties(go2cpp_park_wake_pair PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_resource_history_stress
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_resource_history_stress.cpp)
    target_link_libraries(go2cpp_resource_history_stress PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_resource_history_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_resource_history_stress PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_resource_history_stress COMMAND go2cpp_resource_history_stress)
    set_tests_properties(go2cpp_resource_history_stress PROPERTIES TIMEOUT 120)

    # ---- SUBSCRIBE 实时推送 demo（仅 GO2CPP_DEBUG_DUMP=ON）----
    # 不注册 ctest（保持用例总数不变）：由 tests/debug/run_subscribe_demo.sh
    # 在测试脚本里做端到端演示与验收。
    if (GO2CPP_DEBUG_DUMP)
        add_executable(go2cpp_debug_subscribe_demo
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/debug_subscribe_demo.cpp)
        target_link_libraries(go2cpp_debug_subscribe_demo PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_debug_subscribe_demo PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    # ---- 全链路追踪场景（仅 GO2CPP_DEBUG_DUMP=ON）----
    # ≥4 Fiber × channel/mutex/timer/select/PC，跑 10s 后抓取 6 个 JSON 视图。
    # 默认构建只编译 demo、不注册 ctest；增强构建下由 scenario-full-trace
    # 经 tests/scenarios/run_full_trace_ctest.sh 执行（见文件末尾注册段）。
    if (GO2CPP_DEBUG_DUMP)
        add_executable(go2cpp_full_trace_scenario
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/full_trace/full_trace_scenario.cpp)
        target_link_libraries(go2cpp_full_trace_scenario PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_full_trace_scenario PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    # ---- 真实场景：100+ HTTP client 并发 → 锁顺序死锁 → Doctor 受控解锁 ----
    # 需要 DebugServer（dump/事件环）与 Doctor（等待图/force-unlock）同时在线。
    # 默认构建只编译 demo、不注册 ctest；增强构建下由 scenario-deadlock-routes
    # 经 tests/scenarios/run_deadlock_routes_ctest.sh 执行。
    if (GO2CPP_DEBUG_DUMP AND GO2CPP_DIAGNOSTIC)
        add_executable(go2cpp_deadlock_routes
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/deadlock_routes/deadlock_routes_server.cpp)
        target_link_libraries(go2cpp_deadlock_routes PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_deadlock_routes PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    # ---- 场景：锁顺序死锁 → Doctor ctx.cancel → ContextRollback undo 解死锁 ----
    # 演示「取消等待 + 回滚释放已持锁」，与 mutex-unlock --force 的 stale 路径
    # 对照。默认构建不注册；增强构建下由 scenario-ctx-cancel 经
    # tests/scenarios/run_ctx_cancel_ctest.sh 执行。
    if (GO2CPP_DEBUG_DUMP AND GO2CPP_DIAGNOSTIC)
        add_executable(go2cpp_ctx_cancel_rollback
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/ctx_cancel_rollback/ctx_cancel_rollback_demo.cpp)
        target_link_libraries(go2cpp_ctx_cancel_rollback PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_ctx_cancel_rollback PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    # ---- 场景：channel 互等死锁 → select 语义恢复（timeout / multi / default）----
    # 不依赖 Doctor；只用 channel + select。默认构建只编译 demo、不注册 ctest；
    # 增强构建下由 scenario-channel-deadlock 执行（见文件末尾注册段）。
    add_executable(go2cpp_channel_deadlock
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/channel_deadlock/channel_deadlock_demo.cpp)
    target_link_libraries(go2cpp_channel_deadlock PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_channel_deadlock PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_tests COMMAND go2cpp_tests)

    # 回放 v5 压力/一致性测试（独立入口）：合成场景默认构建可跑；真实原语
    # 场景需要 GO2CPP_REPLAY=ON（否则测试内部自动 skip 并记为 PASS）。
    add_executable(go2cpp_replay_v5_stress
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_rwmutex_replay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_cv_replay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_waitgroup_replay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_io_replay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_mixed_v5_replay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/replay_v5_stress_main.cpp)
    target_link_libraries(go2cpp_replay_v5_stress PRIVATE
        go2cpp_runtime go2cpp_replay_core)
    target_include_directories(go2cpp_replay_v5_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_replay_v5_stress PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_replay_v5_stress COMMAND go2cpp_replay_v5_stress)

    # 并发原语复杂场景 / 随机 fuzz / 死锁检测（正确性）。
    add_executable(go2cpp_concurrency_combinations
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_concurrency_combinations.cpp)
    target_link_libraries(go2cpp_concurrency_combinations PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_concurrency_combinations PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_concurrency_combinations PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_concurrency_combinations COMMAND go2cpp_concurrency_combinations)
    set_tests_properties(go2cpp_concurrency_combinations PROPERTIES TIMEOUT 120)

    add_executable(go2cpp_concurrency_fuzz
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_concurrency_fuzz.cpp)
    target_link_libraries(go2cpp_concurrency_fuzz PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_concurrency_fuzz PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_concurrency_fuzz PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_concurrency_fuzz COMMAND go2cpp_concurrency_fuzz)
    set_tests_properties(go2cpp_concurrency_fuzz PROPERTIES TIMEOUT 180)

    add_executable(go2cpp_deadlock_detection
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_deadlock_detection.cpp)
    target_link_libraries(go2cpp_deadlock_detection PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_deadlock_detection PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_deadlock_detection PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_deadlock_detection COMMAND go2cpp_deadlock_detection)
    set_tests_properties(go2cpp_deadlock_detection PROPERTIES TIMEOUT 120)

    # debug 记录正确性（ON 构建验证事件守恒/配对/采样语义；OFF 构建验证零记录契约）。
    add_executable(go2cpp_debug_record_correctness
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_debug_record_correctness.cpp)
    target_link_libraries(go2cpp_debug_record_correctness PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_debug_record_correctness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_debug_record_correctness PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_debug_record_correctness COMMAND go2cpp_debug_record_correctness)
    set_tests_properties(go2cpp_debug_record_correctness PROPERTIES TIMEOUT 120)
    set_tests_properties(go2cpp_replay_v5_stress PROPERTIES TIMEOUT 300)
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
    # 可变长栈专项：go2cpp_tests 内的 fiber_growable 模块已经包含在 test #1
    # 的全量运行里；这里再单独注册一条 ctest，失败时能直接定位到栈增长，
    # 而不是看到整个 go2cpp_tests 超时/崩溃。复用同一二进制避免再链接一份
    # runtime；filter 入口与 go2cpp_known_filter_runs 的机制一致。
    add_test(NAME go2cpp_fiber_growable_stack COMMAND go2cpp_tests)
    set_tests_properties(go2cpp_fiber_growable_stack PROPERTIES
        ENVIRONMENT "GO2CPP_TEST_FILTER=fiber_growable"
        PASS_REGULAR_EXPRESSION "fiber growable stack"
        TIMEOUT 120)

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

    # TLS/ALPN 测试由 `openssl s_server` 提供对端，属非 hermetic 测试，
    # 因此**不注册为 ctest**（避免无 openssl 的 CI 误报），只构建独立
    # 二进制，由 scripts/verify_tls_alpn.sh 启动服务端后执行。
    add_executable(go2cpp_tls_alpn_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_tls_alpn.cpp)
    target_link_libraries(go2cpp_tls_alpn_test PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_tls_alpn_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)

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

    # ---- config/SO 热重载 manifest：解析、静态校验与两阶段事务 ----
    # 假 loader 注入，不编译/不加载真实 .so；ctest -R so_manifest 命中。
    add_executable(go2cpp_so_manifest_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/config/test_so_manifest.cpp)
    target_link_libraries(go2cpp_so_manifest_test PRIVATE go2cpp_config)
    target_compile_options(go2cpp_so_manifest_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_so_manifest_test COMMAND go2cpp_so_manifest_test)
    set_tests_properties(go2cpp_so_manifest_test PROPERTIES TIMEOUT 180)

    # ---- Channel::Select 在 Fiber（受管 G）下的 DoneCase / AfterCase 专项 ----
    # 覆盖普通线程测不到的 park 路径：Select 在 Fiber 里 park，被另一个 Fiber 唤醒。
    add_executable(go2cpp_fiber_select_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_fiber_select.cpp)
    target_link_libraries(go2cpp_fiber_select_test PRIVATE
        go2cpp_channel go2cpp_context go2cpp_scheduler go2cpp_fiber go2cpp_error)
    target_compile_options(go2cpp_fiber_select_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_fiber_select_test COMMAND go2cpp_fiber_select_test)
    set_tests_properties(go2cpp_fiber_select_test PROPERTIES TIMEOUT 120)

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

    # ---- metrics 模块：Counter / Gauge / Histogram + Prometheus 导出 ----
    add_executable(go2cpp_metrics_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/metrics/test_metrics.cpp)
    target_link_libraries(go2cpp_metrics_test PRIVATE
        go2cpp_metrics go2cpp_runtime)
    target_include_directories(go2cpp_metrics_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(go2cpp_metrics_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_metrics_test COMMAND go2cpp_metrics_test)
    set_tests_properties(go2cpp_metrics_test PROPERTIES TIMEOUT 300)

    set_tests_properties(go2cpp_high_load_stress PROPERTIES TIMEOUT 180)

    # ---- 孤儿并发/IO 审计测试（2026-10-21 测试固化）----
    # 这 7 个文件长期存在于 tests/ 但没有任何 target 引用（见
    # docs/test-audit-2026-10-21.md §2.5，合计约 2900 行）。注册前逐个
    # 用 build-work 的 Release 库手工编译并实跑通过，再落到 CMake；
    # 每个都是自带 main 的独立可执行文件，超时按实测耗时留 3~10 倍余量。
    #
    # sync_cancel_stress 是 Fiber cancel+cleanup 丢唤醒的独立复现器：
    # docs/AGENT-HANDOFF.md 与 codex.md 都记录「尚未注册 ctest」，本轮补上。
    add_executable(go2cpp_channel_concurrency
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_channel_concurrency.cpp)
    target_link_libraries(go2cpp_channel_concurrency PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_channel_concurrency PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_channel_concurrency PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_channel_concurrency
             COMMAND go2cpp_channel_concurrency)
    set_tests_properties(go2cpp_channel_concurrency PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_worker_lifecycle
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_worker_lifecycle.cpp)
    target_link_libraries(go2cpp_worker_lifecycle PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_worker_lifecycle PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_worker_lifecycle PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_worker_lifecycle COMMAND go2cpp_worker_lifecycle)
    set_tests_properties(go2cpp_worker_lifecycle PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_future_audit
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_future_audit.cpp)
    target_link_libraries(go2cpp_future_audit PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_future_audit PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_future_audit PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_future_audit COMMAND go2cpp_future_audit)
    set_tests_properties(go2cpp_future_audit PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_sync_cancel_stress
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_sync_cancel_stress.cpp)
    target_link_libraries(go2cpp_sync_cancel_stress PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_sync_cancel_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_sync_cancel_stress PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_sync_cancel_stress COMMAND go2cpp_sync_cancel_stress)
    # 复现器默认 5000 轮；慢机器 + TSan 下余量到 300s。
    set_tests_properties(go2cpp_sync_cancel_stress PROPERTIES TIMEOUT 300)

    add_executable(go2cpp_io_audit
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/network/test_io_audit.cpp)
    target_link_libraries(go2cpp_io_audit PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_io_audit PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_io_audit PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_io_audit COMMAND go2cpp_io_audit)
    set_tests_properties(go2cpp_io_audit PROPERTIES TIMEOUT 300)

    # ParkingCondition 并发压测（futex 代际字丢唤醒护栏）。
    # 源文件原先只有 run_* 函数没有 main、也没有 target 引用；本轮补 main
    # （断言逻辑未改）后注册。放最后一条：它只依赖 core::ParkingCondition，
    # 不经过 hook，任何构建组合都能跑。
    add_executable(go2cpp_parking_condition_stress
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/test_parking_condition_stress.cpp)
    target_link_libraries(go2cpp_parking_condition_stress PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_parking_condition_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_parking_condition_stress PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_parking_condition_stress
             COMMAND go2cpp_parking_condition_stress)
    set_tests_properties(go2cpp_parking_condition_stress PROPERTIES TIMEOUT 300)

    # ---- 资源历史单测（默认构建也跑：OFF 时断言空语义）----
    # 直接调用 debug_resources 的注册表/事件/栈样本与 debug_dump 的四个视图
    # （resources/locks/channels/history），不启 DebugServer socket；
    # 比 run_resource_history_demo.sh 更早暴露字段/计数回归。
    add_executable(go2cpp_resource_history_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/test_resource_history.cpp)
    target_link_libraries(go2cpp_resource_history_test PRIVATE go2cpp_runtime)
    target_include_directories(go2cpp_resource_history_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(go2cpp_resource_history_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_resource_history_test
             COMMAND go2cpp_resource_history_test)
    set_tests_properties(go2cpp_resource_history_test PROPERTIES TIMEOUT 120)

    # ---- 场景测试入 ctest（仅增强构建）----
    # 三个场景脚本以前只能手工执行；现在统一经 tests/scenarios/run_*_ctest.sh
    # 包装后注册。包装脚本负责：
    #   - 解析构建目录（GO2CPP_SCENARIO_BUILD_DIR > build-doctor-full >
    #     build-debug-full > build-enhanced），显式指定无效目录时直接报错；
    #   - 缺构建目录 / 可执行 / CLI / python3 时 exit 77，配合
    #     SKIP_RETURN_CODE=77 记为 Skipped 而不是 Failed。
    # 只在对应能力开关全开时注册；默认构建（GO2CPP_BUILD_ENHANCED=OFF）
    # 不注册任何一条，ctest 用例总数保持 130 不变。
    if (GO2CPP_BUILD_ENHANCED)
        # 死锁路由：需要 DebugServer + Doctor + CLI。
        if (GO2CPP_DEBUG_DUMP AND GO2CPP_DIAGNOSTIC AND GO2CPP_WITH_CLI)
            add_test(NAME scenario-deadlock-routes
                     COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/run_deadlock_routes_ctest.sh)
            set_tests_properties(scenario-deadlock-routes PROPERTIES
                SKIP_RETURN_CODE 77
                TIMEOUT 600)
        endif()

        # 全链路 6 视图：需要 DebugServer（dump）+ CLI。
        if (GO2CPP_DEBUG_DUMP AND GO2CPP_WITH_CLI)
            add_test(NAME scenario-full-trace
                     COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/run_full_trace_ctest.sh)
            set_tests_properties(scenario-full-trace PROPERTIES
                SKIP_RETURN_CODE 77
                TIMEOUT 300)
        endif()

        # ctx.cancel + ContextRollback：需要 Doctor（DIAGNOSTIC）+ dump 事件。
        if (GO2CPP_DEBUG_DUMP AND GO2CPP_DIAGNOSTIC)
            add_test(NAME scenario-ctx-cancel
                     COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/run_ctx_cancel_ctest.sh)
            set_tests_properties(scenario-ctx-cancel PROPERTIES
                SKIP_RETURN_CODE 77
                TIMEOUT 300)
        endif()

        # channel 互等死锁 + select 语义（timeout / multi / default）：
        # 不依赖 Doctor，只需要 demo 可执行文件与 bash；缺文件时脚本 exit 77。
        add_test(NAME scenario-channel-deadlock
                 COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios/channel_deadlock/run_channel_deadlock.sh --ctest)
        set_tests_properties(scenario-channel-deadlock PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 300)

        # 既有增强场景保持注册不变（不属于本次三条；仍走原脚本）。
        if (GO2CPP_DEBUG_DUMP AND GO2CPP_WITH_CLI)
            add_test(NAME go2cpp_scenario_resource_history
                     COMMAND ${CMAKE_COMMAND} -E env
                         CLI=${CMAKE_CURRENT_BINARY_DIR}/tools/cli/go2cpp-cli
                         bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug/run_resource_history_demo.sh
                              ${CMAKE_CURRENT_BINARY_DIR} 10)
            set_tests_properties(go2cpp_scenario_resource_history PROPERTIES
                PASS_REGULAR_EXPRESSION "ALL PASS"
                TIMEOUT 180)
        endif()
        if (GO2CPP_MIGRATION_API AND TARGET go2cpp_fiber_adopt_demo)
            add_test(NAME go2cpp_migration_adopt_roundtrip
                     COMMAND bash
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/migration/tests/adopt_roundtrip.sh
                         ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(go2cpp_migration_adopt_roundtrip PROPERTIES
                TIMEOUT 300)
        endif()
    endif()

    # ---- tools 测试（migration / cli / replay 三个独立工程）----
    # 这三个脚本此前只能手工执行（tests/run_all_tests.sh 第 4 步），本轮
    # 挂进 ctest。脚本需要各自的独立构建目录，但 ctest 不应在跑测试时偷偷
    # 触发一次完整构建，因此统一由 tests/tools/run_*.sh 探测已有构建目录：
    #   - 找到 → 直接执行对应脚本；
    #   - 找不到 → 打印构建命令并以 77 退出（SKIP_RETURN_CODE=77 记 SKIP）。
    # 这样默认 build-work 即使没有 build-cli/build-migration/build-replay，
    # ctest 也不会 FAIL，但也不会把缺失伪造成 PASS。
    add_test(NAME tools-migration
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_migration.sh)
    set_tests_properties(tools-migration PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 900)
    add_test(NAME tools-cli
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_cli.sh)
    set_tests_properties(tools-cli PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 900)
    add_test(NAME tools-replay
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_replay.sh)
    set_tests_properties(tools-replay PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 900)

    # 未注册（实跑不通过，原因如实记录；见 /tmp/test_fix.md）：
    # - tests/network/test_hook_fifo.cpp：场景 3 用 raw write 在**主线程**把
    #   F_SETPIPE_SZ=4096 的管道写满，随后没有读者，write_f 永久阻塞，
    #   测试自身死锁（实测 timeout 300s）；不是产品缺陷，需改成非阻塞填充。
    # - tests/network/test_hook_behavior.cpp：场景 2 断言「kSleep 关闭后
    #   心跳 < 5」（期望真阻塞 worker），实测心跳 39——与文件头注释记录的
    #   已知行为一致（SetOpEnabled 只改标志位，实际 sleep 仍走 park 路径）。
    #   这是被测试记录的产品缺陷，不能改断言来「通过」，故不注册。
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
        concurrency/futex_mutex_demo
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
    add_executable(go2cpp_sync_mutex_contention_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/sync_mutex_contention_benchmark.cpp)
    target_link_libraries(go2cpp_sync_mutex_contention_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_sync_mutex_contention_benchmark PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_executable(go2cpp_futex_mutex_contention_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/futex_mutex_contention_benchmark.cpp)
    target_link_libraries(go2cpp_futex_mutex_contention_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_futex_mutex_contention_benchmark PRIVATE -O2 -Wall -Wextra -Wpedantic)
    # X-36：FutexMutex 与 std::mutex 的临界区长度 × 线程数开销矩阵。
    add_executable(go2cpp_lock_overhead_matrix_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/lock_overhead_matrix_benchmark.cpp)
    target_link_libraries(go2cpp_lock_overhead_matrix_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_lock_overhead_matrix_benchmark PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    # X-36：GMP 环境下 FutexMutex（线程级）vs sync::Mutex（协程级）。
    add_executable(go2cpp_futex_mutex_fiber_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/futex_mutex_fiber_benchmark.cpp)
    target_link_libraries(go2cpp_futex_mutex_fiber_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_futex_mutex_fiber_benchmark PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
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

    # 分层锁：SpinMutex vs Mutex vs std::mutex 的 ns/op（native + fiber 两种口径）。
    add_executable(go2cpp_spin_mutex_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/spin_mutex_benchmark.cpp)
    target_link_libraries(go2cpp_spin_mutex_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_spin_mutex_benchmark PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
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

        # GMP 协程 vs thread+epoll 的多场景对比（fork 子进程隔离，需要 Linux）
        add_executable(go2cpp_gmp_vs_thread_epoll_benchmark
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/gmp_vs_thread_epoll_benchmark.cpp)
        target_link_libraries(go2cpp_gmp_vs_thread_epoll_benchmark PRIVATE go2cpp_runtime)
        target_compile_options(go2cpp_gmp_vs_thread_epoll_benchmark PRIVATE
            -O2 -Wall -Wextra -Wpedantic)
    endif()
endif()
