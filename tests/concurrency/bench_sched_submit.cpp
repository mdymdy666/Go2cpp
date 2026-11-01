/**
 * @file bench_sched_submit.cpp
 * @brief 调度器任务成本基准：提交侧 rdtsc + wall（4P/4M，8 轮配对取 min）。
 *
 * ## 口径（与 docs/reports/2026-11-01-scheduler-opt-review.md 一致）
 *
 * 1. **提交侧**：单生产者线程用 `rdtsc` 包住每次 `scheduler.spawn()`，
 *    统计 cycles/task。这是「任务创建 + 入队」的端到端成本，包含分配、
 *    registry 注册、队列发布与唤醒提示；
 * 2. **wall**：每轮从第一次 spawn 到 `done == tasks` 的墙钟 ns/task；
 * 3. **配对**：同一进程内跑 `rounds` 轮，报告每轮与最小值（min 对宿主噪声
 *    最稳，评审口径用的就是 min）。
 *
 * TSC 频率在进程内实测（50ms 墙钟对 `rdtsc` 计数），把 cycles 折算成 ns。
 *
 * 构建（不改 CMake）：
 *   g++ -O2 -DNDEBUG -std=c++17 -Iinclude tests/concurrency/bench_sched_submit.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_debug \
 *       -lgo2cpp_debug_event -lpthread -o /tmp/bench_sched_submit
 *
 * 用法：/tmp/bench_sched_submit [workers=4] [tasks=100000] [rounds=8] [collect=256]
 */
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

inline std::uint64_t ReadTsc() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline std::uint64_t NowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

/// @brief 实测 TSC 频率（GHz）：50ms 墙钟窗口内的 cycles / 墙钟。
double CalibrateTscGhz() {
    const auto wall_start = NowNs();
    const auto tsc_start = ReadTsc();
    while (NowNs() - wall_start < 50ULL * 1000ULL * 1000ULL) {
        std::this_thread::yield();
    }
    const auto wall = NowNs() - wall_start;
    const auto cycles = ReadTsc() - tsc_start;
    if (wall == 0) {
        return 1.0;
    }
    return static_cast<double>(cycles) / static_cast<double>(wall);
}

}  // namespace

int main(int argc, char** argv) {
    const int workers = argc > 1 ? std::atoi(argv[1]) : 4;
    const std::size_t tasks =
        argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10))
                 : 100000;
    const int rounds = argc > 3 ? std::atoi(argv[3]) : 8;
    const std::size_t collect =
        argc > 4 ? static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10))
                 : 256;

    go2cpp::SchedulerConfig config;
    config.processor_count = static_cast<std::size_t>(workers);
    config.max_workers = static_cast<std::size_t>(workers);
    config.terminal_collect_interval = collect;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    const double tsc_ghz = CalibrateTscGhz();
    std::printf("[cfg] workers=%d tasks=%zu rounds=%d collect=%zu tsc=%.3fGHz\n",
                workers, tasks, rounds, collect, tsc_ghz);

    std::vector<double> submit_cycles;
    std::vector<double> wall_ns;
    submit_cycles.reserve(static_cast<std::size_t>(rounds));
    wall_ns.reserve(static_cast<std::size_t>(rounds));
    // 每任务提交侧 rdtsc 的完整分布：p10/p25 对宿主噪声不敏感，是本次
    // A/B（全局 runnable -> per-P 分片）的主口径。
    std::vector<std::uint64_t> dist;
    dist.reserve(tasks);
    std::vector<double> p10_by_round;
    std::vector<double> p25_by_round;

    for (int round = 0; round < rounds; ++round) {
        std::atomic<std::size_t> done{0};
        std::uint64_t submit_ticks = 0;
        dist.clear();
        const std::uint64_t wall_start = NowNs();
        for (std::size_t i = 0; i < tasks; ++i) {
            const std::uint64_t begin = ReadTsc();
            scheduler.spawn([&done] {
                done.fetch_add(1, std::memory_order_release);
            });
            const std::uint64_t end = ReadTsc();
            const std::uint64_t delta = end - begin;
            submit_ticks += delta;
            dist.push_back(delta);
        }
        while (done.load(std::memory_order_acquire) != tasks) {
            std::this_thread::yield();
        }
        const double wall = static_cast<double>(NowNs() - wall_start) /
                            static_cast<double>(tasks);
        const double cycles =
            static_cast<double>(submit_ticks) / static_cast<double>(tasks);
        submit_cycles.push_back(cycles);
        wall_ns.push_back(wall);
        std::vector<std::uint64_t> sorted = dist;
        std::sort(sorted.begin(), sorted.end());
        const auto quantile = [&sorted](double q) {
            const std::size_t index = static_cast<std::size_t>(
                q * static_cast<double>(sorted.size() - 1));
            return static_cast<double>(sorted[index]);
        };
        p10_by_round.push_back(quantile(0.10));
        p25_by_round.push_back(quantile(0.25));
        std::printf(
            "[round %d] submit %.1f cyc/task (%.1f ns)  wall %.1f ns/task  "
            "p10 %.1f p25 %.1f p50 %.1f p90 %.1f cyc\n",
            round, cycles, cycles / tsc_ghz, wall, quantile(0.10),
            quantile(0.25), quantile(0.50), quantile(0.90));
    }

    const double best_submit =
        *std::min_element(submit_cycles.begin(), submit_cycles.end());
    const double best_wall = *std::min_element(wall_ns.begin(), wall_ns.end());
    const double best_p10 = *std::min_element(p10_by_round.begin(),
                                              p10_by_round.end());
    const double best_p25 = *std::min_element(p25_by_round.begin(),
                                              p25_by_round.end());
    std::printf("[min] submit %.1f cyc/task (%.1f ns)  wall %.1f ns/task\n",
                best_submit, best_submit / tsc_ghz, best_wall);
    std::printf("[pmin] submit_p10 %.1f cyc (%.2f ns)  submit_p25 %.1f cyc "
                "(%.2f ns)  wall %.1f ns/task\n",
                best_p10, best_p10 / tsc_ghz, best_p25, best_p25 / tsc_ghz,
                best_wall);
    scheduler.shutdown();
    return 0;
}
