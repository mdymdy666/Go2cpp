/**
 * @file encoding_test_main.cpp
 * @brief `go2cpp_encoding_test` 的入口：单元 / 随机 / 真实场景 / 压测。
 *
 * @details 输出约定与项目其他独立测试一致：每段先打印 `[encoding] 名称`，
 *          失败信息写 stderr；任一 `ENCODING_CHECK` 失败则进程返回 1。
 *
 * 运行：
 * ```
 * ctest --test-dir build-quality -R go2cpp_encoding_test --output-on-failure
 * ./build-quality/go2cpp_encoding_test
 * ```
 */

#include "encoding_test_support.hpp"

#include <cstdio>

void RunBase64Tests();
void RunHexTests();
void RunBinaryTests();
void RunCsvTests();
void RunRandomTests();
void RunScenarioTests();
void RunStressTests();

int main() {
    std::printf("[encoding] === encoding module tests ===\n");
    std::printf("[encoding] Python-referenced data: %s\n",
                go2cpp_encoding_test::TestdataPath("python_vectors.txt").c_str());
    std::fflush(stdout);

    RunBase64Tests();
    RunHexTests();
    RunBinaryTests();
    RunCsvTests();
    RunRandomTests();
    RunScenarioTests();
    RunStressTests();

    const int failures =
        go2cpp_encoding_test::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "[encoding] failures=%d\n", failures);
        return 1;
    }
    std::printf("[encoding] all checks passed\n");
    return 0;
}
