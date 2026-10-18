/**
 * @file test_math_env.hpp
 * @brief math / rand 测试共享的环境信息输出（CPU 型号、核数、编译器）。
 *
 * @details 两个测试可执行文件各自独立运行，都需要在压测结果前打印硬件环境；
 *          这段代码不是被测逻辑，抽到单独的测试辅助头里，避免复制粘贴。
 */
#pragma once

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

namespace go2cpp_math_test {

/// 从 `/proc/cpuinfo` 读取 CPU 型号（Linux）。读不到时返回 "unknown"。
inline std::string CpuModel() {
    std::ifstream file("/proc/cpuinfo");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("model name", 0) == 0) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::size_t start = line.find_first_not_of(" \t", colon + 1);
            if (start == std::string::npos) {
                continue;
            }
            return line.substr(start);
        }
    }
    return "unknown";
}

/// 打印一行环境信息；`tag` 是测试名（math / rand）。
inline void PrintEnvironment(const char* tag) {
    std::printf("[%s][env] CPU: %s\n", tag, CpuModel().c_str());
    std::printf("[%s][env] hardware_concurrency=%u, compiler=%s\n", tag,
                std::thread::hardware_concurrency(), __VERSION__);
#if defined(__OPTIMIZE__)
    std::printf("[%s][env] optimization: on (-O2/-O3)\n", tag);
#else
    std::printf("[%s][env] optimization: off\n", tag);
#endif
}

}  // namespace go2cpp_math_test
