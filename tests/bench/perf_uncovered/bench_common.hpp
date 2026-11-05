#pragma once

// 补无性能报告模块的微基准公共脚手架。
//
// 设计约束（2 核 4 GB / 100 ms–1 s 单样本窗口）：
//   - 每个基准由外部脚本跑 5 轮，本文件只负责「一轮内的一次测量」；
//   - 迭代数自适应：从 n0 起按实测吞吐外推，直到单次执行 >= target_ms，
//     保证样本窗口落在规范区间，避免 <100 ms 的计时噪声；
//   - 输出单行 key=value，便于 shell/Python 解析中位与 CV。

#include <chrono>
#include <cstddef>
#include <cstdio>

namespace bench {

using Clock = std::chrono::steady_clock;

inline double MsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 防止编译器把整段循环优化掉；volatile 写足以形成可观测副作用。
volatile std::size_t g_sink = 0;

template <class F>
void RunAdaptive(const char* name, std::size_t n0, double target_ms, F&& body) {
    std::size_t n = n0 == 0 ? 1 : n0;
    double ms = 0.0;
    for (int attempt = 0; attempt < 30; ++attempt) {
        const Clock::time_point t0 = Clock::now();
        body(n);
        ms = MsSince(t0);
        if (ms >= target_ms) {
            break;
        }
        const double safe_ms = ms > 1e-6 ? ms : 1e-6;
        double scale = target_ms / safe_ms;
        if (scale < 2.0) {
            scale = 2.0;
        }
        const auto next = static_cast<std::size_t>(static_cast<double>(n) * (scale * 1.1));
        n = next > n ? next : n * 2;
    }
    const Clock::time_point t0 = Clock::now();
    body(n);
    const double total_ms = MsSince(t0);
    const double ops = static_cast<double>(n);
    std::printf("%s ops=%.0f total_ms=%.3f ops_per_sec=%.0f ns_per_op=%.3f\n", name, ops,
                total_ms, ops / (total_ms / 1000.0), total_ms * 1e6 / ops);
    std::fflush(stdout);
}

}  // namespace bench
