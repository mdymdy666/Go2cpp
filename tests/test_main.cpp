#include "test_support.hpp"

#include <iostream>

void run_error_tests();
void run_context_tests();
void run_channel_tests();
void run_scheduler_tests();
void run_panic_defer_tests();

int main() {
    run_error_tests();
    run_context_tests();
    run_channel_tests();
    run_scheduler_tests();
    run_panic_defer_tests();
    const int failures = go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::cerr << "[test] failures=" << failures << '\n';
        return 1;
    }
    std::cout << "[test] all checks passed\n";
    return 0;
}
