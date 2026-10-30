/**
 * @file bench_spin_mutex_opt.cpp
 * @brief `sync::SpinMutex` 优化前后的 1/4/16 线程 ns/op 基准（独立可执行程序）。
 *
 * @date 2026-10-30
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 为什么是独立程序
 *
 * 基准需要独占 CPU、耗时可观，混进 `go2cpp_tests` 会污染测试时长；而本
 * 文件属于并发线的一次性对照工具，不改 `CMakeLists`（避免与其它改动冲突），
 * 用「手工 g++ + 链接 .build 目录的 .so」运行，命令见文件末尾注释。
 *
 * ## 口径
 *
 * - `pair`：一次完整临界区（Lock + Unlock）记 1 op，单线程；
 * - `contend`：`threads` 个 OS 线程争用同一把锁，每线程
 *   `iterations / threads` 次 pair，总时间 / 总 ops；
 * - 每档重复 `reps` 次，输出**中位数**、最小、最大，避免单次抖动；
 * - 测量前有热身，且只在 Release（`-O3 -DNDEBUG`）下有效。
 *
 * ## 用法
 *
 * ```
 * ./bench_spin_mutex_opt [threads=1,4,16] [iterations=2000000] [reps=5]
 * ```
 *
 * ## 编译
 *
 * ```
 * g++ -O3 -DNDEBUG -std=c++20 -pthread -Iinclude \
 *     tests/concurrency/bench_spin_mutex_opt.cpp \
 *     -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *     -lgo2cpp_sync -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_context \
 *     -lgo2cpp_io -lgo2cpp_channel -lgo2cpp_error -lgo2cpp_control_flow \
 *     -lgo2cpp_future -lgo2cpp_network -lgo2cpp_log -lgo2cpp_config \
 *     -lgo2cpp_os -lgo2cpp_math -lgo2cpp_rand -lgo2cpp_path \
 *     -lgo2cpp_container -lgo2cpp_compress -lgo2cpp_encoding -lgo2cpp_hash \
 *     -lgo2cpp_metrics -lgo2cpp_debug -lz -lssl -lcrypto -ldl \
 *     -o /tmp/bench_spin_mutex_opt
 * ```
 */
#include "go2cpp/concurrency/spin_mutex.hpp"
#include "go2cpp/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// 启动闸门：所有线程就绪后一起放行，避免把创建线程的时间算进测量。
class SpinBarrier {
public:
    explicit SpinBarrier(int participants) : m_participants(participants) {}

    void Wait() {
        m_arrived.fetch_add(1, std::memory_order_acq_rel);
        while (m_arrived.load(std::memory_order_acquire) < m_participants) {
            std::this_thread::yield();
        }
    }

private:
    std::atomic<int> m_arrived{0};
    int m_participants;
};

double Median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const std::size_t middle = samples.size() / 2;
    if (samples.size() % 2U == 0U) {
        return (samples[middle - 1] + samples[middle]) / 2.0;
    }
    return samples[middle];
}

double MinOf(const std::vector<double>& samples) {
    return *std::min_element(samples.begin(), samples.end());
}

double MaxOf(const std::vector<double>& samples) {
    return *std::max_element(samples.begin(), samples.end());
}

/// 统一 `std::mutex` 与 go2cpp 锁的接口差异。
template <typename Lock>
void LockIt(Lock& lock) {
    if constexpr (std::is_same_v<Lock, std::mutex>) {
        lock.lock();
    } else {
        lock.Lock();
    }
}

template <typename Lock>
void UnlockIt(Lock& lock) {
    if constexpr (std::is_same_v<Lock, std::mutex>) {
        lock.unlock();
    } else {
        lock.Unlock();
    }
}

/// 单线程 pair：一次 Lock + 一次 Unlock。
template <typename Lock>
double RunPairOnce(long long iterations) {
    Lock lock;
    for (int i = 0; i < 1000; ++i) {
        LockIt(lock);
        UnlockIt(lock);
    }
    const auto start = Clock::now();
    for (long long i = 0; i < iterations; ++i) {
        LockIt(lock);
        UnlockIt(lock);
    }
    const auto elapsed = Clock::now() - start;
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    return ns / static_cast<double>(iterations);
}

/// `threads` 个线程争用同一把锁；每线程 iterations/threads 次 pair。
template <typename Lock>
double RunContendedOnce(int threads, long long iterations) {
    Lock lock;
    SpinBarrier barrier(threads);
    const long long per_thread = iterations / threads;
    std::atomic<long long> sink{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int index = 0; index < threads; ++index) {
        workers.emplace_back([&] {
            barrier.Wait();
            for (long long i = 0; i < per_thread; ++i) {
                LockIt(lock);
                sink.fetch_add(1, std::memory_order_relaxed);
                UnlockIt(lock);
            }
        });
    }
    const auto start = Clock::now();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = Clock::now() - start;
    const long long ops = per_thread * threads;
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    const double result = ns / static_cast<double>(ops);
    if (sink.load(std::memory_order_relaxed) != ops) {
        std::fprintf(stderr, "ERROR: lost increments\n");
        std::abort();
    }
    return result;
}

struct Stats {
    double median{0.0};
    double min{0.0};
    double max{0.0};
};

template <typename Fn>
Stats Repeat(Fn&& fn, int reps) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(reps));
    for (int rep = 0; rep < reps; ++rep) {
        samples.push_back(fn());
    }
    return {Median(samples), MinOf(samples), MaxOf(samples)};
}

std::vector<int> ParseThreads(const char* text) {
    std::vector<int> values;
    if (text == nullptr || *text == '\0') {
        return {1, 4, 16};
    }
    const std::string input(text);
    std::size_t start = 0;
    while (start <= input.size()) {
        const std::size_t comma = input.find(',', start);
        const std::string token = input.substr(
            start, comma == std::string::npos ? std::string::npos
                                              : comma - start);
        const int value = std::atoi(token.c_str());
        if (value > 0) {
            values.push_back(value);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (values.empty()) {
        values = {1, 4, 16};
    }
    return values;
}

/// 打印一行可被脚本解析的结果。
void Report(const char* primitive, const char* mode, int threads,
            const Stats& stats, long long iterations) {
    const double ns = stats.median;
    const double ops_per_sec = ns > 0.0 ? 1.0e9 / ns : 0.0;
    std::printf("%-10s %-8s %2d  median=%8.2f ns  min=%8.2f  max=%8.2f  "
                "%10.0f ops/s  (n=%lld)\n",
                primitive, mode, threads, stats.median, stats.min, stats.max,
                ops_per_sec, iterations);
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<int> thread_counts = ParseThreads(argc > 1 ? argv[1] : nullptr);
    const long long iterations =
        argc > 2 ? std::atoll(argv[2]) : 2'000'000LL;
    const int reps = argc > 3 ? std::atoi(argv[3]) : 5;

    std::printf("# bench_spin_mutex_opt: iterations=%lld reps=%d\n",
                iterations, reps);
    std::printf("# primitive  mode      T   median/min/max\n");

    // 参考组：std::mutex（平台原生 futex）、sync::Mutex（项目混合锁）。
    for (int threads : thread_counts) {
        if (threads == 1) {
            const Stats stats = Repeat(
                [&] { return RunPairOnce<std::mutex>(iterations); }, reps);
            Report("std::mutex", "pair", 1, stats, iterations);
            const Stats sync_stats = Repeat(
                [&] { return RunPairOnce<go2cpp::sync::Mutex>(iterations); }, reps);
            Report("sync::Mutex", "pair", 1, sync_stats, iterations);
            const Stats spin_stats = Repeat(
                [&] {
                    return RunPairOnce<go2cpp::sync::SpinMutex>(iterations);
                },
                reps);
            Report("SpinMutex", "pair", 1, spin_stats, iterations);
        } else {
            const Stats sync_stats = Repeat(
                [&] {
                    return RunContendedOnce<go2cpp::sync::Mutex>(threads,
                                                                 iterations);
                },
                reps);
            Report("sync::Mutex", "contend", threads, sync_stats, iterations);
            const Stats spin_stats = Repeat(
                [&] {
                    return RunContendedOnce<go2cpp::sync::SpinMutex>(threads,
                                                                    iterations);
                },
                reps);
            Report("SpinMutex", "contend", threads, spin_stats, iterations);
        }
    }
    std::printf("# done\n");
    return 0;
}
