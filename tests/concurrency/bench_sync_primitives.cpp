/**
 * @file bench_sync_primitives.cpp
 * @brief sync 原语性能基准（Mutex / ConditionVariable / WaitGroup /
 *        RWMutex / SpinMutex）。
 *
 * @date 2026-10-30
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 用法
 *
 * 本文件是**独立可执行程序**，不注册 ctest 用例、不进入 go2cpp_tests：
 * 基准需要独占 CPU 且耗时可观，混进 ctest 只会污染测试时长。
 *
 * ```
 * ./bench_sync_primitives            # 默认：每项 1e7 次
 * ./bench_sync_primitives 1000000    # 自定义迭代次数
 * ```
 *
 * ## 测量项
 *
 * | 名称 | 含义 |
 * |---|---|
 * | `mutex.1t.pair` | 单线程 lock/unlock 一个 pair 的 ns |
 * | `mutex.Nt.contend` | N 线程共享一把锁的总吞吐与 ns/op |
 * | `mutex.trylock.pair` | 单线程 TryLock 成功路径 ns |
 * | `cv.pingpong.wait` | 两线程条件变量往返，每次 wait 的 ns |
 * | `cv.waitfor_timeout` | WaitFor 超时路径 ns |
 * | `wg.adddone` | 无等待者时 Add/Done ns |
 * | `wg.wakeup` | Done → Wait 返回的唤醒延迟 ns |
 * | `rw.read.1t` / `rw.read.Nt` | 读锁 pair ns/op |
 * | `rw.write.1t` / `rw.write.Nt` | 写锁 pair ns/op |
 * | `spin.1t` / `spin.Nt` | SpinMutex pair ns/op |
 *
 * ## 为什么用「pair」
 *
 * 单独统计 Lock 会把 Unlock 的开销藏到下一次测量里，两个原语无法直接对比；
 * 统一按一次完整临界区（lock+unlock）计时。
 */
#include "go2cpp/concurrency/rwmutex.hpp"
#include "go2cpp/concurrency/spin_mutex.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

inline std::uint64_t NowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

inline double NsPer(Clock::duration elapsed, long long ops) noexcept {
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    return ops > 0 ? ns / static_cast<double>(ops) : 0.0;
}

inline double OpsPerSec(Clock::duration elapsed, long long ops) noexcept {
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    return ns > 0.0 ? static_cast<double>(ops) * 1e9 / ns : 0.0;
}

/// 自建屏障（不依赖 C++20 `<barrier>`）。
class SpinBarrier {
public:
    explicit SpinBarrier(int participants) : m_participants(participants) {}

    void Wait() noexcept {
        const std::uint32_t generation =
            m_generation.load(std::memory_order_acquire);
        if (m_arrived.fetch_add(1, std::memory_order_acq_rel) + 1U ==
            static_cast<std::uint32_t>(m_participants)) {
            m_arrived.store(0, std::memory_order_relaxed);
            m_generation.fetch_add(1, std::memory_order_release);
            return;
        }
        while (m_generation.load(std::memory_order_acquire) == generation) {
            CpuRelax();
        }
    }

private:
    int m_participants;
    std::atomic<std::uint32_t> m_arrived{0};
    std::atomic<std::uint32_t> m_generation{0};
};

long long g_iterations = 10'000'000;

void PrintSection(const char* title) { std::printf("\n== %s ==\n", title); }

void Report(const std::string& name, double ns_per_op) {
    std::printf("%-28s %10.2f ns/op\n", name.c_str(), ns_per_op);
    std::fflush(stdout);
}

void ReportThroughput(const std::string& name, double ops_per_sec,
                      double ns_per_op) {
    std::printf("%-28s %10.2f ns/op   %12.0f ops/s\n", name.c_str(), ns_per_op,
                ops_per_sec);
    std::fflush(stdout);
}

// ---------------------------------------------------------------- Mutex ----

void BenchMutexSingleThread() {
    go2cpp::sync::Mutex mutex;
    std::uint64_t local = 0;
    for (int i = 0; i < 1000; ++i) {
        mutex.Lock();
        ++local;
        mutex.Unlock();
    }
    const auto start = Clock::now();
    for (long long i = 0; i < g_iterations; ++i) {
        mutex.Lock();
        ++local;
        mutex.Unlock();
    }
    const auto elapsed = Clock::now() - start;
    Report("mutex.1t.pair", NsPer(elapsed, g_iterations));
    std::printf("%-28s %10llu (guard)\n", "mutex.1t.sink",
                static_cast<unsigned long long>(local));

    const auto try_start = Clock::now();
    long long acquired = 0;
    for (long long i = 0; i < g_iterations; ++i) {
        if (mutex.TryLock()) {
            ++acquired;
            mutex.Unlock();
        }
    }
    const auto try_elapsed = Clock::now() - try_start;
    Report("mutex.trylock.pair", NsPer(try_elapsed, g_iterations));
    std::printf("%-28s %10lld (guard)\n", "mutex.trylock.sink", acquired);
}

void BenchMutexThreads(int threads) {
    go2cpp::sync::Mutex mutex;
    SpinBarrier barrier(threads);
    std::atomic<std::uint64_t> counter{0};
    const long long per_thread = g_iterations / threads;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            barrier.Wait();
            for (long long i = 0; i < per_thread; ++i) {
                mutex.Lock();
                counter.fetch_add(1, std::memory_order_relaxed);
                mutex.Unlock();
            }
        });
    }
    const auto start = Clock::now();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = Clock::now() - start;
    const long long ops = per_thread * threads;
    char name[64];
    std::snprintf(name, sizeof(name), "mutex.%dt.contend", threads);
    ReportThroughput(name, OpsPerSec(elapsed, ops), NsPer(elapsed, ops));
    (void)counter.load();
}

// ---------------------------------------------------- ConditionVariable ----

void BenchConditionVariablePingPong() {
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable cv;
    bool turn = false;
    const long long rounds = g_iterations / 20;
    SpinBarrier barrier(2);

    std::thread peer([&] {
        barrier.Wait();
        for (long long i = 0; i < rounds; ++i) {
            mutex.Lock();
            turn = true;
            cv.NotifyOne();
            while (turn) {
                cv.Wait(mutex);
            }
            mutex.Unlock();
        }
    });

    barrier.Wait();
    const auto start = Clock::now();
    for (long long i = 0; i < rounds; ++i) {
        mutex.Lock();
        while (!turn) {
            cv.Wait(mutex);
        }
        turn = false;
        cv.NotifyOne();
        mutex.Unlock();
    }
    const auto elapsed = Clock::now() - start;
    peer.join();
    Report("cv.pingpong.wait", NsPer(elapsed, rounds * 2));
}

void BenchConditionVariableTimeout() {
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable cv;
    const long long rounds = 2000;
    const auto start = Clock::now();
    for (long long i = 0; i < rounds; ++i) {
        mutex.Lock();
        (void)cv.WaitFor(mutex, std::chrono::microseconds(50));
        mutex.Unlock();
    }
    const auto elapsed = Clock::now() - start;
    Report("cv.waitfor_timeout", NsPer(elapsed, rounds));
}

void BenchConditionVariableNotify() {
    go2cpp::sync::ConditionVariable cv;
    const long long rounds = g_iterations / 4;
    const auto start = Clock::now();
    for (long long i = 0; i < rounds; ++i) {
        cv.NotifyOne();
    }
    const auto elapsed = Clock::now() - start;
    Report("cv.notify_one.empty", NsPer(elapsed, rounds));
}

// ------------------------------------------------------------ WaitGroup ----

void BenchWaitGroupAddDone() {
    go2cpp::sync::WaitGroup wg;
    for (int i = 0; i < 1000; ++i) {
        wg.Add(1);
        wg.Done();
    }
    const auto wraps = g_iterations / 4;
    const auto start = Clock::now();
    for (long long i = 0; i < wraps; ++i) {
        wg.Add(1);
        wg.Done();
    }
    const auto elapsed = Clock::now() - start;
    Report("wg.adddone", NsPer(elapsed, wraps));
}

void BenchWaitGroupWakeup() {
    // 每轮一个独立的 WaitGroup，保证「Wait 已在等 / Done 触发唤醒」的
    // 时序确定：worker 取走指针后主线程睡 20µs（远大于 Wait 内部 18µs
    // 的自旋预算），确保等待者已经进入内核 futex，测到的就是真实唤醒延迟。
    const long long rounds = 5000;
    std::atomic<go2cpp::sync::WaitGroup*> pending{nullptr};
    std::atomic<std::uint64_t> wake_ns{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> stop{false};
    double done_cost_ns = 0.0;

    std::thread worker([&] {
        for (;;) {
            go2cpp::sync::WaitGroup* wg = nullptr;
            while ((wg = pending.load(std::memory_order_acquire)) == nullptr) {
                if (stop.load(std::memory_order_relaxed)) {
                    return;
                }
                CpuRelax();
            }
            pending.store(nullptr, std::memory_order_release);
            wg->Wait();
            wake_ns.store(NowNs(), std::memory_order_release);
            completed.store(true, std::memory_order_release);
        }
    });

    double total_ns = 0.0;
    for (long long i = 0; i < rounds; ++i) {
        go2cpp::sync::WaitGroup wg;
        wg.Add(1);
        pending.store(&wg, std::memory_order_release);
        while (pending.load(std::memory_order_acquire) != nullptr) {
            CpuRelax();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(20));
        // sleep 返回后 CPU 可能刚从低功耗态恢复；先忙等约 8µs 让流水线
        // 与时钟稳定，再开始计时，避免把唤醒抖动算进 Done。
        for (int k = 0; k < 1000; ++k) {
            CpuRelax();
        }
        const std::uint64_t start = NowNs();
        wg.Done();
        const std::uint64_t done_end = NowNs();
        done_cost_ns +=
            static_cast<double>(done_end - start);
        while (!completed.load(std::memory_order_acquire)) {
            CpuRelax();
        }
        completed.store(false, std::memory_order_release);
        total_ns += static_cast<double>(
            wake_ns.load(std::memory_order_acquire) - start);
    }
    stop.store(true, std::memory_order_release);
    worker.join();
    Report("wg.wakeup", total_ns / static_cast<double>(rounds));
    Report("wg.done.cost", done_cost_ns / static_cast<double>(rounds));
}

// ------------------------------------------------------------- RWMutex ----

void BenchRWMutexSingleThread() {
    go2cpp::sync::RWMutex rw;
    for (int i = 0; i < 1000; ++i) {
        rw.RLock();
        rw.RUnlock();
    }
    const auto read_start = Clock::now();
    for (long long i = 0; i < g_iterations; ++i) {
        rw.RLock();
        rw.RUnlock();
    }
    const auto read_elapsed = Clock::now() - read_start;
    Report("rw.read.1t", NsPer(read_elapsed, g_iterations));

    const auto write_start = Clock::now();
    for (long long i = 0; i < g_iterations; ++i) {
        rw.Lock();
        rw.Unlock();
    }
    const auto write_elapsed = Clock::now() - write_start;
    Report("rw.write.1t", NsPer(write_elapsed, g_iterations));
}

void BenchRWMutexReaders(int threads) {
    go2cpp::sync::RWMutex rw;
    SpinBarrier barrier(threads);
    const long long per_thread = g_iterations / threads;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            barrier.Wait();
            for (long long i = 0; i < per_thread; ++i) {
                rw.RLock();
                rw.RUnlock();
            }
        });
    }
    const auto start = Clock::now();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = Clock::now() - start;
    const long long ops = per_thread * threads;
    char name[64];
    std::snprintf(name, sizeof(name), "rw.read.%dt", threads);
    ReportThroughput(name, OpsPerSec(elapsed, ops), NsPer(elapsed, ops));
}

void BenchRWMutexWriters(int threads) {
    go2cpp::sync::RWMutex rw;
    SpinBarrier barrier(threads);
    const long long per_thread = g_iterations / threads / 10;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            barrier.Wait();
            for (long long i = 0; i < per_thread; ++i) {
                rw.Lock();
                rw.Unlock();
            }
        });
    }
    const auto start = Clock::now();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = Clock::now() - start;
    const long long ops = per_thread * threads;
    char name[64];
    std::snprintf(name, sizeof(name), "rw.write.%dt", threads);
    ReportThroughput(name, OpsPerSec(elapsed, ops), NsPer(elapsed, ops));
}

// ----------------------------------------------------------- SpinMutex ----

void BenchSpinMutexSingleThread() {
    go2cpp::sync::SpinMutex spin;
    for (int i = 0; i < 1000; ++i) {
        spin.Lock();
        spin.Unlock();
    }
    const auto start = Clock::now();
    for (long long i = 0; i < g_iterations; ++i) {
        spin.Lock();
        spin.Unlock();
    }
    const auto elapsed = Clock::now() - start;
    Report("spin.1t", NsPer(elapsed, g_iterations));
}

void BenchSpinMutexThreads(int threads) {
    go2cpp::sync::SpinMutex spin;
    SpinBarrier barrier(threads);
    const long long per_thread = g_iterations / threads;
    std::atomic<std::uint64_t> counter{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            barrier.Wait();
            for (long long i = 0; i < per_thread; ++i) {
                spin.Lock();
                counter.fetch_add(1, std::memory_order_relaxed);
                spin.Unlock();
            }
        });
    }
    const auto start = Clock::now();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = Clock::now() - start;
    const long long ops = per_thread * threads;
    char name[64];
    std::snprintf(name, sizeof(name), "spin.%dt", threads);
    ReportThroughput(name, OpsPerSec(elapsed, ops), NsPer(elapsed, ops));
    (void)counter.load();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const long long parsed = std::atoll(argv[1]);
        if (parsed > 0) {
            g_iterations = parsed;
        }
    }
    std::printf("iterations per case: %lld\n", g_iterations);

    PrintSection("Mutex");
    BenchMutexSingleThread();
    BenchMutexThreads(4);
    BenchMutexThreads(16);

    PrintSection("ConditionVariable");
    BenchConditionVariablePingPong();
    BenchConditionVariableTimeout();
    BenchConditionVariableNotify();

    PrintSection("WaitGroup");
    BenchWaitGroupAddDone();
    BenchWaitGroupWakeup();

    PrintSection("RWMutex");
    BenchRWMutexSingleThread();
    BenchRWMutexReaders(4);
    BenchRWMutexReaders(16);
    BenchRWMutexWriters(4);

    PrintSection("SpinMutex");
    BenchSpinMutexSingleThread();
    BenchSpinMutexThreads(4);
    return 0;
}
