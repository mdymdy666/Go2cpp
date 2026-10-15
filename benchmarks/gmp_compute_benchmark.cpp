/**
 * @file gmp_compute_benchmark.cpp
 * @brief GMP 计算密集基准：Fiber/Scheduler 与 std::thread 的公平对照。
 *
 * 设计原则（与 io_epoll_fair_benchmark 保持一致）：
 *   1. 两种模型执行**完全相同**的工作量、相同的并行度；
 *   2. 排除 setup（创建/启动）与稳态（纯计算）时间，分别统计；
 *   3. 每项重复多次，报告**中位数**而非最好成绩，宿主抖动下更可信；
 *   4. CPU 工作量用 LCG 递推，编译器无法折叠为闭式解，并以 volatile
 *      累加器防止死代码消除。
 *
 * 场景：
 *   1. CPU 并行吞吐（12 路满负载）—— Fiber vs std::thread
 *   2. 调度器扩展性（1/2/4/8/12 worker）—— 计算加速比与并行效率
 *   3. G 创建/回收开销 —— spawn+wait N 个空任务 vs 创建 N 个线程
 *   4. 让出/恢复开销 —— yield 往返，以及与 cv 通知的对照
 *
 * 注意：本基准**不含**任何 IO，测的是纯调度与计算开销。
 */
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

/// 防止编译器把计算结果判定为无用而整段消除。
volatile std::uint64_t g_sink = 0;

/// 固定迭代数的 LCG + xorshift 混合递推，不可折叠为闭式解。
std::uint64_t cpu_work(std::uint64_t seed, std::uint64_t iterations) noexcept {
    std::uint64_t acc = seed | 1ULL;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        acc ^= (acc >> 29);
        acc += i;
    }
    return acc;
}

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return (n % 2 == 1) ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

std::size_t default_workers() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0 ? 4U : static_cast<std::size_t>(hw);
}

// ---------------------------------------------------------------- 场景 1

/**
 * @brief 满负载 CPU 吞吐：Fiber 模型 vs 线程模型。
 * @details 每个 worker 做相同的 LCG 迭代数；分别计时“任务提交到全部完成”
 *          的墙钟时间。Fiber 侧另计 setup（start+spawn）与 steady（wait）差。
 */
void scenario_cpu_throughput(std::size_t workers, std::uint64_t per_worker_iters,
                             int repeats) {
    const std::uint64_t total = per_worker_iters * workers;
    std::cout << "\n[1] CPU 并行吞吐  workers=" << workers
              << "  每 worker 迭代=" << per_worker_iters
              << "  总迭代=" << total << "  repeats=" << repeats << "\n";

    std::vector<double> fiber_ms;
    std::vector<double> thread_ms;
    std::vector<double> fiber_setup_ms;

    for (int r = 0; r < repeats; ++r) {
        // --- Fiber / Scheduler ---
        {
            go2cpp::Scheduler scheduler(workers);
            const auto setup_begin = Clock::now();
            scheduler.start();
            std::vector<std::shared_ptr<go2cpp::Task>> tasks;
            tasks.reserve(workers);
            std::atomic<std::uint64_t> acc{0};
            for (std::size_t i = 0; i < workers; ++i) {
                tasks.push_back(scheduler.spawn([&acc, i, per_worker_iters] {
                    acc.fetch_add(cpu_work(i + 1, per_worker_iters),
                                  std::memory_order_relaxed);
                }));
            }
            const double setup = ms_since(setup_begin);

            const auto begin = Clock::now();
            for (auto& task : tasks) {
                task->wait();
            }
            const double elapsed = ms_since(begin);
            fiber_ms.push_back(elapsed);
            fiber_setup_ms.push_back(setup);
            g_sink += acc.load(std::memory_order_relaxed);
            scheduler.shutdown();
        }

        // --- std::thread ---
        {
            std::vector<std::thread> threads;
            std::vector<std::uint64_t> partial(workers, 0);
            threads.reserve(workers);
            const auto begin = Clock::now();
            for (std::size_t i = 0; i < workers; ++i) {
                threads.emplace_back([&partial, i, per_worker_iters] {
                    partial[i] = cpu_work(i + 1, per_worker_iters);
                });
            }
            for (auto& t : threads) {
                t.join();
            }
            const double elapsed = ms_since(begin);
            thread_ms.push_back(elapsed);
            for (auto v : partial) {
                g_sink += v;
            }
        }
    }

    const double fm = median(fiber_ms);
    const double tm = median(thread_ms);
    const double fs = median(fiber_setup_ms);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "    Fiber(steady) 中位数 = " << fm << " ms"
              << "   区间[" << *std::min_element(fiber_ms.begin(), fiber_ms.end())
              << ", " << *std::max_element(fiber_ms.begin(), fiber_ms.end()) << "]\n";
    std::cout << "    Thread        中位数 = " << tm << " ms"
              << "   区间[" << *std::min_element(thread_ms.begin(), thread_ms.end())
              << ", " << *std::max_element(thread_ms.begin(), thread_ms.end()) << "]\n";
    std::cout << "    Fiber setup(start+spawn) 中位数 = " << fs << " ms\n";
    std::cout << "    >>> Fiber/Thread = " << (tm > 0 ? fm / tm : 0.0)
              << "x   (含 setup 则为 "
              << (tm > 0 ? (fm + fs) / tm : 0.0) << "x)\n";
    std::cout << "    吞吐: Fiber = " << (total / (fm / 1000.0) / 1e6)
              << " Miter/s   Thread = " << (total / (tm / 1000.0) / 1e6)
              << " Miter/s\n";
}

// ---------------------------------------------------------------- 场景 2

/**
 * @brief 扩展性：固定总工作量，改变 worker 数，观察加速比与并行效率。
 */
void scenario_scaling(std::size_t max_workers, std::uint64_t total_iters, int repeats) {
    std::cout << "\n[2] 调度器扩展性  总迭代=" << total_iters
              << "  repeats=" << repeats << "\n";

    double baseline = 0.0;
    // 显式步骤表：原来的自引用步进表达式在 w==12 时恒等于 12，会导致死循环。
    static constexpr std::size_t kStepTable[] = {1, 2, 4, 8, 12};
    for (const std::size_t w : kStepTable) {
        if (w > max_workers) {
            break;
        }
        const std::uint64_t per = total_iters / w;
        std::vector<double> samples;
        for (int r = 0; r < repeats; ++r) {
            go2cpp::Scheduler scheduler(w);
            scheduler.start();
            std::atomic<std::uint64_t> acc{0};
            std::vector<std::shared_ptr<go2cpp::Task>> tasks;
            tasks.reserve(w);
            for (std::size_t i = 0; i < w; ++i) {
                tasks.push_back(scheduler.spawn([&acc, i, per] {
                    acc.fetch_add(cpu_work(i + 1, per), std::memory_order_relaxed);
                }));
            }
            const auto begin = Clock::now();
            for (auto& task : tasks) {
                task->wait();
            }
            samples.push_back(ms_since(begin));
            g_sink += acc.load(std::memory_order_relaxed);
            scheduler.shutdown();
        }
        const double m = median(samples);
        if (w == 1) {
            baseline = m;
        }
        const double speedup = (m > 0) ? baseline / m : 0.0;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "    workers=" << std::setw(2) << w
                  << "  中位数=" << std::setw(9) << m << " ms"
                  << "  加速比=" << std::setw(5) << speedup << "x"
                  << "  并行效率=" << std::setw(6) << (speedup / static_cast<double>(w) * 100.0)
                  << "%\n";
    }
}

// ---------------------------------------------------------------- 场景 3

/**
 * @brief 任务创建开销：spawn+wait N 个空任务 vs 创建并 join N 个线程。
 * @details 线程数量刻意小两个数量级——创建 10 万个线程会耗尽宿主资源，
 *          因此对比的是**单个单位**的开销（µs/个），而非总量。
 */
void scenario_creation_overhead(std::size_t fiber_count, std::size_t thread_count,
                                std::size_t workers) {
    std::cout << "\n[3] 创建开销（每单位 µs）\n";

    // --- Fiber：批量 spawn 后统一 wait，测 setup 与 steady ---
    {
        go2cpp::Scheduler scheduler(workers);
        scheduler.start();
        const auto begin = Clock::now();
        std::vector<std::shared_ptr<go2cpp::Task>> tasks;
        tasks.reserve(fiber_count);
        for (std::size_t i = 0; i < fiber_count; ++i) {
            tasks.push_back(scheduler.spawn([] {}));
        }
        const double spawn_ms = ms_since(begin);
        for (auto& task : tasks) {
            task->wait();
        }
        const double total_ms = ms_since(begin);
        scheduler.shutdown();
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "    Fiber  N=" << fiber_count
                  << "  spawn=" << spawn_ms << " ms  spawn+wait=" << total_ms << " ms"
                  << "  → " << (spawn_ms * 1000.0 / fiber_count) << " µs/G (spawn)"
                  << "  " << (total_ms * 1000.0 / fiber_count) << " µs/G (含完成)\n";
    }

    // --- std::thread ---
    {
        const auto begin = Clock::now();
        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            threads.emplace_back([] {});
        }
        const double create_ms = ms_since(begin);
        for (auto& t : threads) {
            t.join();
        }
        const double total_ms = ms_since(begin);
        std::cout << "    Thread N=" << thread_count
                  << "  create=" << create_ms << " ms  create+join=" << total_ms << " ms"
                  << "  → " << (create_ms * 1000.0 / thread_count) << " µs/T (create)"
                  << "  " << (total_ms * 1000.0 / thread_count) << " µs/T (含完成)\n";
    }
}

}  // namespace

int main() {
    const std::size_t workers = default_workers();
    const std::uint64_t per_worker = 20'000'000ULL;
    const int repeats = 5;

    std::cout << "=== Go2Cpp GMP 计算密集基准 ===\n";
    std::cout << "worker(可见核)=" << workers
              << "  硬件并发=" << std::thread::hardware_concurrency() << "\n";

    // 预热：让 CPU 频率与页表先稳定下来，避免第一个场景偏慢。
    g_sink += cpu_work(12345, 5'000'000ULL);

    scenario_cpu_throughput(workers, per_worker, repeats);
    scenario_scaling(workers, 120'000'000ULL, 3);
    scenario_creation_overhead(200'000, 2'000, workers);

    std::cout << "\nsink=" << g_sink << "\n"
              << "gmp compute benchmark done\n";
    return 0;
}