/**
 * @file compress_test_main.cpp
 * @brief `go2cpp_compress_test` 入口：依次运行全部测试段。
 *
 * @details 运行顺序由浅入深：先三个格式的单元测试（格式/边界/错误语义），
 *   再随机测试，然后是**互操作**（依赖系统 gzip / python3），最后真实场景
 *   与压测。任何 `COMPRESS_CHECK` 失败都会让进程返回 1，`ctest` 立即失败。
 *
 * 直接运行：
 * @code
 * ./build-quality/go2cpp_compress_test
 * ctest --test-dir build-quality -R go2cpp_compress_test --output-on-failure
 * @endcode
 */
#include "compress_test_support.hpp"

#include <cstdio>

void RunFlateTests();
void RunZlibTests();
void RunGzipTests();
void RunRandomTests();
void RunInteropTests();
void RunScenarioTests();
void RunStressTests();

int main() {
  std::printf("[compress] === compress module tests ===\n");
  std::fflush(stdout);

  RunFlateTests();
  RunZlibTests();
  RunGzipTests();
  RunRandomTests();
  RunInteropTests();
  RunScenarioTests();
  RunStressTests();

  const int failures =
      go2cpp_compress_test::g_failures.load(std::memory_order_relaxed);
  const int checks =
      go2cpp_compress_test::g_checks.load(std::memory_order_relaxed);
  if (failures != 0) {
    std::fprintf(stderr, "[compress] failures=%d (checks=%d)\n", failures,
                 checks);
    return 1;
  }
  std::printf("[compress] all checks passed (checks=%d)\n", checks);
  return 0;
}
