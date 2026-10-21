/**
 * @file replay_v5_stress_main.cpp
 * @brief 回放 v5 随机/压测/边界测试的独立入口。
 *
 * 用法：
 *   go2cpp_replay_v5_stress [rwmutex|cv|waitgroup|io|mixed|all]
 *
 * 退出码：0 = 全部断言通过；1 = 有失败（失败信息由 GO2CPP_CHECK 打印）。
 *
 * 说明：该文件只提供 main；5 个测试实现也可直接并入 go2cpp_tests（在
 * tests/test_main.cpp 调用对应的 run_*_tests 即可，不新增 ctest 用例）。
 */

#include "test_support.hpp"

#include <cstdio>
#include <cstring>

void run_rwmutex_replay_tests();
void run_cv_replay_tests();
void run_waitgroup_replay_tests();
void run_io_replay_tests();
void run_mixed_v5_replay_tests();

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : "all";
    go2cpp_tests::announce("replay_v5_stress");

    bool selected = false;
    const auto run_if = [&](const char* name, void (*function)()) {
        if (std::strcmp(filter, "all") == 0 ||
            std::strcmp(filter, name) == 0) {
            selected = true;
            function();
        }
    };
    run_if("rwmutex", run_rwmutex_replay_tests);
    run_if("cv", run_cv_replay_tests);
    run_if("waitgroup", run_waitgroup_replay_tests);
    run_if("io", run_io_replay_tests);
    run_if("mixed", run_mixed_v5_replay_tests);

    if (!selected) {
        std::printf("unknown filter: %s\n", filter);
        return 2;
    }
    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::printf("[replay_v5_stress] FAILED failures=%d\n", failures);
        return 1;
    }
    std::printf("[replay_v5_stress] ALL PASS\n");
    return 0;
}
