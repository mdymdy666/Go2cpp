#include "test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

void run_error_tests();
void run_context_tests();
void run_channel_tests();
void run_scheduler_tests();
void run_dynamic_scheduler_tests();
void run_panic_defer_tests();
void run_fiber_tests();
void run_sync_tests();
void run_future_tests();
void run_io_tests();
void run_timer_tests();
void run_beginner_api_tests();
void run_hook_tests();

int main() {
    const char* const filter_value = std::getenv("GO2CPP_TEST_FILTER");
    const std::string_view filter = filter_value ? filter_value : "";
    const auto selected = [filter](std::string_view name) {
        return filter.empty() || filter == name;
    };
    if (selected("error")) run_error_tests();
    if (selected("context")) run_context_tests();
    if (selected("channel")) run_channel_tests();
    if (selected("scheduler")) run_scheduler_tests();
    if (selected("dynamic")) run_dynamic_scheduler_tests();
    if (selected("panic")) run_panic_defer_tests();
    if (selected("fiber")) run_fiber_tests();
    if (selected("sync")) run_sync_tests();
    if (selected("future")) run_future_tests();
    if (selected("io")) run_io_tests();
    if (selected("timer")) run_timer_tests();
    if (selected("beginner")) run_beginner_api_tests();
#ifdef GO2CPP_TEST_HOOK
    if (selected("hook")) run_hook_tests();
#endif
    const int failures = go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::cerr << "[test] failures=" << failures << '\n';
        return 1;
    }
    std::cout << "[test] all checks passed\n";
    return 0;
}
