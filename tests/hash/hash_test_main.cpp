/**
 * @file hash_test_main.cpp
 * @brief `go2cpp_hash_test` 的入口：向量 → 随机对拍 → 真实场景 → 压测。
 *
 * @details 输出约定与项目其他独立测试一致：每段先打印 `[hash] 名称`；
 *          断言失败累计到 `g_failures`，`main` 据此返回 1。致命失败（如 oracle
 *          文件缺失、临时文件写失败）直接 abort，避免在坏状态上继续跑。
 */
#include "hash_test_support.hpp"

#include <cstdio>

void RunVectorTests();
void RunRandomTests();
void RunScenarioTests();
void RunStressTests();

int main() {
    std::printf("[hash] === hash module tests ===\n");
    std::fflush(stdout);

    RunVectorTests();
    RunRandomTests();
    RunScenarioTests();
    RunStressTests();

    const int failures =
        go2cpp_hash_test::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "[hash] failures=%d\n", failures);
        return 1;
    }
    std::printf("[hash] all checks passed\n");
    return 0;
}
