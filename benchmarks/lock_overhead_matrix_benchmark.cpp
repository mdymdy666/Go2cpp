/**
 * @file lock_overhead_matrix_benchmark.cpp
 * @brief `core::FutexMutex` 与 `std::mutex` 的临界区长度 × 线程数开销矩阵。
 *
 * ## 目的（任务 ②）
 *
 * 回答两个问题：
 *   1. 短临界区下，自研唤醒门是否显著慢于 glibc 的 `pthread_mutex`
 *      （同样是 CAS + futex，路径应该接近）；
 *   2. 长临界区下，锁开销占比下降，两者是否趋于一致。
 *
 * 临界区用 `volatile` 自增做“不可优化掉的工作量”：
 *   - 1 / 10 / 100 / 1000 次自增，对应极短 / 短 / 中 / 长四档。
 *
 * ## 指标
 *
 * `ns/op` = 墙钟总时间 / (线程数 × 每线程轮数)，即一次 lock+unlock 的
 * 均摊成本（含临界区工作量）。每格跑 5 次取**中位数**。
 *
 * ## 测量纪律（沿用项目约定）
 *
 * - 只用于 Release 构建（`-O3 -DNDEBUG`）；
 * - 每次测量前打印 `/proc/loadavg`，> 1.5 的数据不可信；
 * - 交替测量两种锁，减少机器漂移对单一实现的系统性偏置；
 * - 用 `timeout 60` 从外部兜底（本基准正常几秒内结束）。
 *
 * ## 用法
 *
 *   ./go2cpp_lock_overhead_matrix_benchmark [reps=5]
 */

#include "go2cpp/concurrency/core/futex_mutex.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/resource.h>

namespace {

using Clock = std::chrono::steady_clock;

double ReadLoadAverage() {
    if (std::FILE* file = std::fopen("/proc/loadavg", "r")) {
        double load = -1.0;
        if (std::fscanf(file, "%lf", &load) != 1) {
            load = -1.0;
        }
        std::fclose(file);
        return load;
    }
    return -1.0;
}

/// 临界区工作量对应的每线程轮数：保持各档总工作量在同一量级。
int RoundsForWork(int work) {
    // 短临界区单次测量只有几十纳秒，轮数太少会被调度抖动淹没；
    // 用更多轮数把每个样本拉长到毫秒级，中位数才有意义。
    if (work <= 1) {
        return 100000;
    }
    if (work <= 10) {
        return 50000;
    }
    if (work <= 100) {
        return 10000;
    }
    return 2000;
}

/**
 * @brief 跑一次测量，返回 ns/op。
 * @param work 临界区内 `volatile` 自增次数（编译器无法消除）。
 * @details 临界区内的自增刻意用 `volatile` 而非原子：它既不会被优化掉，
 *          也不会引入额外的原子指令开销，能忠实反映“临界区长度”。
 */
template <typename Mutex>
double RunOnce(int threads, int rounds, int work) {
    Mutex mutex;
    volatile long long counter = 0;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int index = 0; index < threads; ++index) {
        workers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int round = 0; round < rounds; ++round) {
                mutex.lock();
                for (int unit = 0; unit < work; ++unit) {
                    ++counter;
                }
                mutex.unlock();
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < threads) {
        std::this_thread::yield();
    }

    const auto start = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    const auto finish = Clock::now();

    const long long expected =
        static_cast<long long>(threads) * rounds * work;
    if (counter != expected) {
        std::fprintf(stderr, "ERROR: counter=%lld expected=%lld\n",
                     static_cast<long long>(counter), expected);
        std::abort();
    }
    const double total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
            .count());
    return total_ns / (static_cast<double>(threads) * rounds);
}

/// 中位数（reps 次样本）。
double Median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2U];
}

struct Cell {
    double std_mutex{0.0};
    double futex_mutex{0.0};
};

/// 交替测量两种锁，每格收集 reps 个样本后取中位数。
Cell MeasureCell(int threads, int work, int reps) {
    const int rounds = RoundsForWork(work);
    std::vector<double> std_samples;
    std::vector<double> futex_samples;
    std_samples.reserve(static_cast<std::size_t>(reps));
    futex_samples.reserve(static_cast<std::size_t>(reps));
    for (int rep = 0; rep < reps; ++rep) {
        std_samples.push_back(RunOnce<std::mutex>(threads, rounds, work));
        futex_samples.push_back(
            RunOnce<go2cpp::core::FutexMutex>(threads, rounds, work));
    }
    Cell cell;
    cell.std_mutex = Median(std_samples);
    cell.futex_mutex = Median(futex_samples);
    return cell;
}

}  // namespace

int main(int argc, char** argv) {
    int reps = 5;
    if (argc > 1) {
        const int value = std::atoi(argv[1]);
        if (value > 0) {
            reps = value;
        }
    }

    const std::vector<int> works = {1, 10, 100, 1000};
    const std::vector<int> thread_list = {1, 2, 4, 8};

    std::printf("=== 锁开销矩阵：std::mutex vs core::FutexMutex（Release）===\n");
    std::printf("reps=%d（每格取中位数） loadavg=%.2f\n\n", reps,
                ReadLoadAverage());

    std::printf("| 临界区 | 线程 | std::mutex ns/op | FutexMutex ns/op | "
                "Futex/std |\n");
    std::printf("|---|---|---|---|---|\n");
    for (const int work : works) {
        for (const int threads : thread_list) {
            const Cell cell = MeasureCell(threads, work, reps);
            const double ratio =
                cell.std_mutex > 0.0 ? cell.futex_mutex / cell.std_mutex : 0.0;
            std::printf("| %d 次自增 | %d | %.2f | %.2f | %.2fx |\n", work,
                        threads, cell.std_mutex, cell.futex_mutex, ratio);
            std::fflush(stdout);
        }
    }

    std::printf("\nfinal loadavg=%.2f\n", ReadLoadAverage());
    std::printf("log: 临界区越长，锁开销占比越小，两列应趋于一致；"
                "短临界区下 Futex/std 显著 >1 表示 CAS 路径有额外开销。\n");
    return 0;
}
