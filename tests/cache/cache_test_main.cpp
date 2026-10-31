/**
 * @file cache_test_main.cpp
 * @brief `go2cpp_cache_test` 入口：头文件检查 + 7 个策略 + 随机/场景/压测。
 */
#include "cache_test_support.hpp"

#include <cstdio>

void RunHeaderTests();
void RunLruTests();
void RunLruKTests();
void RunTwoQueueTests();
void RunTinyLfuTests();
void RunGdsfTests();
void RunArcTests();
void RunClockTests();
void RunInterfaceTests();
void RunRandomTests();
void RunScenarioTests();
void RunStressTests();

int main() {
    std::printf("[cache] === cache module tests ===\n");
    std::fflush(stdout);

    RunHeaderTests();
    RunLruTests();
    RunLruKTests();
    RunTwoQueueTests();
    RunTinyLfuTests();
    RunGdsfTests();
    RunArcTests();
    RunClockTests();
    RunInterfaceTests();
    RunRandomTests();
    RunScenarioTests();
    RunStressTests();

    const int failures =
        go2cpp_cache_test::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "[cache] failures=%d\n", failures);
        return 1;
    }
    std::printf("[cache] all checks passed\n");
    return 0;
}
