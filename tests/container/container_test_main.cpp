/**
 * @file container_test_main.cpp
 * @brief `go2cpp_container_test` 的入口：依次运行单元 / 随机 / 真实场景 / 压测。
 *
 * @details 输出约定与项目其他独立测试一致：每个测试段先打印 `[container] 名称`，
 *          失败信息写 stderr；有任何 `CONTAINER_CHECK` 失败则返回 1。
 *          `CONTAINER_REQUIRE` 失败会立即 abort（继续跑会建立在损坏状态上）。
 */
#include "container_test_support.hpp"

#include <cstdio>

void RunHeapTests();
void RunListTests();
void RunRingTests();
void RunRandomTests();
void RunScenarioTests();
void RunStressTests();

int main() {
    std::printf("[container] === container module tests ===\n");
    std::fflush(stdout);

    RunHeapTests();
    RunListTests();
    RunRingTests();
    RunRandomTests();
    RunScenarioTests();
    RunStressTests();

    const int failures =
        go2cpp_container_test::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "[container] failures=%d\n", failures);
        return 1;
    }
    std::printf("[container] all checks passed\n");
    return 0;
}
