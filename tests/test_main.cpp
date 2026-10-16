#include "test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

void run_error_tests();
void run_control_flow_tests();
void run_context_tests();
void run_channel_tests();
void run_scheduler_tests();
void run_dynamic_scheduler_tests();
void run_fiber_tests();
void run_sync_tests();
void run_future_tests();
void run_io_tests();
void run_timer_tests();
void run_beginner_api_tests();
void run_log_config_tests();
void run_network_tests();
void run_sse_tests();
void run_hybrid_gate_tests();
void run_parking_condition_tests();
void run_thread_policy_tests();
void run_fiber_local_tests();
void run_event_go_tests();
void run_fd_manager_tests();
void run_errors_namespace_tests();
void run_strings_tests();
void run_strconv_tests();
void run_fmt_tests();
void run_streams_tests();
void run_time_tests();
void run_hook_tests();
void run_debug_fiber_tests();
void run_debug_scheduler_tests();
void run_debug_cli_tests();
void run_debug_sync_tests();
void run_debug_lock_selector_tests();
void run_debug_events_tests();
void run_debug_breakpoints_tests();

/**
 * @brief 统一运行默认测试集并根据过滤器选择测试模块。
 * @return 所有选定测试通过返回 0，否则返回非零值。
 */
int main() {
    const char* const filter_value = std::getenv("GO2CPP_TEST_FILTER");
    const std::string_view filter = filter_value ? filter_value : "";
    // 记录过滤器是否真的命中过某个模块。未知名字会让下面每个 selected() 都返回
    // false，于是不执行任何模块却仍然打印 "all checks passed" 并返回 0，把「空跑」
    // 伪装成「通过」（台账 T-08）。这里显式检测并返回非零。
    bool any_selected = false;
    const auto selected = [filter, &any_selected](std::string_view name) {
        const bool match = filter.empty() || filter == name;
        if (match && !filter.empty()) {
            any_selected = true;
        }
        return match;
    };
    if (selected("error")) run_error_tests();
    if (selected("control_flow") || selected("defer")) run_control_flow_tests();
    if (selected("context")) run_context_tests();
    if (selected("channel")) run_channel_tests();
    if (selected("scheduler")) run_scheduler_tests();
    if (selected("dynamic")) run_dynamic_scheduler_tests();
    if (selected("fiber")) run_fiber_tests();
    if (selected("sync")) run_sync_tests();
    if (selected("future")) run_future_tests();
    if (selected("io")) run_io_tests();
    if (selected("timer")) run_timer_tests();
    if (selected("beginner")) run_beginner_api_tests();
    if (selected("log")) run_log_config_tests();
    if (selected("network")) run_network_tests();
    if (selected("sse")) run_sse_tests();
    if (selected("hybrid_gate")) run_hybrid_gate_tests();
    if (selected("parking_condition")) run_parking_condition_tests();
    if (selected("thread_policy")) run_thread_policy_tests();
    if (selected("fiber_local")) run_fiber_local_tests();
    if (selected("event_go")) run_event_go_tests();
    if (selected("fd_manager")) run_fd_manager_tests();
    if (selected("errors_ns")) run_errors_namespace_tests();
    if (selected("strings")) run_strings_tests();
    if (selected("strconv")) run_strconv_tests();
    if (selected("fmt")) run_fmt_tests();
    if (selected("streams")) run_streams_tests();
    if (selected("time")) run_time_tests();
    if (selected("debug_fiber")) run_debug_fiber_tests();
    if (selected("debug_scheduler")) run_debug_scheduler_tests();
    if (selected("debug_cli")) run_debug_cli_tests();
    if (selected("debug_sync")) run_debug_sync_tests();
    if (selected("debug_lock_selector")) run_debug_lock_selector_tests();
    if (selected("debug_events")) run_debug_events_tests();
    if (selected("debug_breakpoints")) run_debug_breakpoints_tests();
#ifdef GO2CPP_TEST_HOOK
    if (selected("hook")) run_hook_tests();
#endif
    const int failures = go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::cerr << "[test] failures=" << failures << '\n';
        return 1;
    }
    if (!filter.empty() && !any_selected) {
        // 返回 2 以便与「有用例失败」的 1 区分；调用方与 CI 都能据此发现
        // 过滤器拼错，而不是把 0 个用例当成全部通过。
        std::cerr << "[test] GO2CPP_TEST_FILTER=\"" << filter
                  << "\" 未匹配任何测试模块；本次没有执行任何用例\n";
        return 2;
    }
    std::cout << "[test] all checks passed\n";
    return 0;
}