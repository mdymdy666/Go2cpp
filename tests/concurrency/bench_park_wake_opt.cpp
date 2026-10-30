/**
 * @file bench_park_wake_opt.cpp
 * @brief ParkingCondition / WaitNode 的 park+wake 基准（往返 ns/op 与并发延迟）。
 *
 * @date 2026-10-30
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 用法
 *
 * 独立可执行程序（不注册 ctest、不进入 go2cpp_tests）：
 *
 * ```
 * ./bench_park_wake_opt               # 默认场景集合
 * ./bench_park_wake_opt rtt 20000     # 只跑往返，自定义轮数
 * ./bench_park_wake_opt conc 1000 5   # 并发 park/wake：1000 等待者 × 5 轮
 * ./bench_park_wake_opt one 1000 5    # 逐个 notify_one：1000 等待者 × 5 轮
 * ./bench_park_wake_opt fiber 2000    # 受管 Fiber 往返
 * ```
 *
 * ## 测量口径
 *
 * - `rtt`：2 个线程（1 等待 + 1 通知）。测量方**先确认等待者已真正入队**
 *   （`DebugSnapshot().waiters`），再记录 `notify_one()` 前的时间戳，等待
 *   等待者写完回执后记录结束时间。因此区间内包含一次真实 park（自旋或
 *   FUTEX_WAIT）与一次完整唤醒，不含入队前的准备与回执后的清理；
 * - `conc`：N 个等待线程（每个线程用**自己的**外部 mutex，排除用户锁争用
 *   对唤醒延迟的污染）同时 park，测量 `notify_all()` 到每个等待者返回的
 *   延迟，输出 P50/P95/P99/max 与总唤醒耗时；
 * - `one`：N 个等待者逐个 `notify_one()`，测量每轮总耗时与每次调用均摊；
 * - `fiber`：Scheduler 上的受管 Fiber 往返（验证 Fiber 路径不被 native
 *   优化拖慢）。
 */
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::core::ParkingCondition;

/// 进程累计 CPU 时间（所有线程的 user+sys，微秒精度）。
/// 用它衡量 park/wake 的**真实 CPU 成本**：墙钟会被 OS 调度噪声主导，
/// 而 CPU 时间不受超订影响，能稳定反映队列操作/分配/原子操作的节省。
double ProcessCpuNs() noexcept {
#if defined(__linux__)
    struct rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    const double seconds = static_cast<double>(usage.ru_utime.tv_sec) +
                           static_cast<double>(usage.ru_stime.tv_sec);
    const double micros = static_cast<double>(usage.ru_utime.tv_usec) +
                          static_cast<double>(usage.ru_stime.tv_usec);
    return seconds * 1e9 + micros * 1e3;
#else
    return 0.0;
#endif
}

inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

double NsSince(Clock::time_point start) noexcept {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                             start)
            .count());
}

struct Percentiles {
    double p50{0};
    double p95{0};
    double p99{0};
    double max{0};
    double mean{0};
};

Percentiles Summarize(std::vector<double> samples) {
    Percentiles result;
    if (samples.empty()) {
        return result;
    }
    std::sort(samples.begin(), samples.end());
    const auto at = [&samples](double q) {
        const std::size_t index = static_cast<std::size_t>(
            q * static_cast<double>(samples.size() - 1) + 0.5);
        return samples[index];
    };
    double sum = 0;
    for (const double value : samples) {
        sum += value;
    }
    result.mean = sum / static_cast<double>(samples.size());
    result.p50 = at(0.50);
    result.p95 = at(0.95);
    result.p99 = at(0.99);
    result.max = samples.back();
    return result;
}

/// 等 ParkingCondition 的 native 等待者数量达到 target。
/// 轮询间隔用短睡眠而不是忙等：1000 线程场景下忙轮询会抢走等待者的 CPU，
/// 也会与 DebugSnapshot 的内部队列锁争用，把准备阶段的开销带进测量。
void WaitNativeWaiters(ParkingCondition& condition, std::size_t target) {
    for (;;) {
        if (condition.DebugSnapshot().native_waiters >= target) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
}

// ---------------------------------------------------------------------------
// 场景 1：native 线程 park+wake 往返
// ---------------------------------------------------------------------------
void BenchRtt(int iterations) {
    ParkingCondition condition;
    std::mutex mutex;
    std::atomic<std::uint64_t> seq{0};
    std::atomic<bool> ack{false};
    std::atomic<bool> stop{false};
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));

    std::thread waiter([&] {
        std::uint64_t local_seen = 0;
        while (!stop.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mutex);
            (void)condition.wait(lock, [&] {
                return seq.load(std::memory_order_acquire) != local_seen;
            });
            local_seen = seq.load(std::memory_order_acquire);
            lock.unlock();
            ack.store(true, std::memory_order_release);
        }
    });

    for (int i = 0; i < iterations; ++i) {
        if (i > 0) {
            ack.store(false, std::memory_order_release);
        }
        WaitNativeWaiters(condition, 1);
        seq.fetch_add(1, std::memory_order_release);
        const auto start = Clock::now();
        condition.notify_one();
        while (!ack.load(std::memory_order_acquire)) {
            CpuRelax();
        }
        samples.push_back(NsSince(start));
    }
    stop.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex);
        seq.fetch_add(1, std::memory_order_release);
    }
    condition.notify_one();
    waiter.join();

    const Percentiles stats = Summarize(samples);
    std::printf("rtt.native        n=%d  mean=%7.1f  p50=%7.1f  p95=%7.1f  "
                "p99=%7.1f  max=%8.1f  (ns/op)\n",
                iterations, stats.mean, stats.p50, stats.p95, stats.p99,
                stats.max);
}

// ---------------------------------------------------------------------------
// 场景 2：N 个 native 等待者的 notify_all 延迟
// ---------------------------------------------------------------------------
void BenchConcurrent(int waiters, int rounds) {
    ParkingCondition condition;
    std::vector<std::unique_ptr<std::mutex>> mutexes;
    mutexes.reserve(static_cast<std::size_t>(waiters));
    for (int i = 0; i < waiters; ++i) {
        mutexes.push_back(std::make_unique<std::mutex>());
    }
    std::vector<std::atomic<int>> flags(static_cast<std::size_t>(waiters));
    for (auto& flag : flags) {
        flag.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> done{0};
    std::atomic<bool> stop{false};
    std::vector<std::atomic<long long>> return_ns(
        static_cast<std::size_t>(waiters));
    for (auto& slot : return_ns) {
        slot.store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(waiters));
    for (int index = 0; index < waiters; ++index) {
        threads.emplace_back([&, index] {
            for (;;) {
                std::unique_lock<std::mutex> lock(*mutexes[index]);
                (void)condition.wait(lock, [&] {
                    return flags[index].load(std::memory_order_acquire) != 0;
                });
                lock.unlock();
                return_ns[index].store(
                    static_cast<long long>(
                        Clock::now().time_since_epoch().count()),
                    std::memory_order_release);
                done.fetch_add(1, std::memory_order_acq_rel);
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }
                while (!stop.load(std::memory_order_acquire) &&
                       flags[index].load(std::memory_order_acquire) != 0) {
                    CpuRelax();
                }
                // 本轮结束：回退计数表示「已离开本轮」，主线程据此确认
                // 所有线程都能重新 park，不会把上一轮的唤醒算进下一轮。
                done.fetch_sub(1, std::memory_order_acq_rel);
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }
            }
        });
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(waiters) * rounds);
    std::vector<double> round_totals;
    round_totals.reserve(rounds);
    std::vector<double> notify_costs;
    notify_costs.reserve(rounds);
    std::vector<double> cpu_costs;
    cpu_costs.reserve(rounds);

    for (int round = 0; round < rounds; ++round) {
        WaitNativeWaiters(condition, static_cast<std::size_t>(waiters));
        // 入队计数只说明节点已在队列里，等待者可能还在自旋窗口（~18µs）。
        // 让它们真正睡进 futex 再唤醒，否则测量的是「自旋兜住唤醒」的
        // 快路径，而不是并发场景真正关心的内核唤醒路径。
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        done.store(0, std::memory_order_release);
        for (auto& flag : flags) {
            flag.store(1, std::memory_order_release);
        }
        const double cpu_before = ProcessCpuNs();
        const auto notify_start = Clock::now();
        condition.notify_all();
        const double notify_ns = NsSince(notify_start);
        while (done.load(std::memory_order_acquire) < waiters) {
            CpuRelax();
        }
        cpu_costs.push_back(ProcessCpuNs() - cpu_before);
        const double total_ns = NsSince(notify_start);
        notify_costs.push_back(notify_ns);
        round_totals.push_back(total_ns);
        for (int index = 0; index < waiters; ++index) {
            const auto when = return_ns[index].load(std::memory_order_acquire);
            const double delta = static_cast<double>(
                when - notify_start.time_since_epoch().count());
            samples.push_back(delta);
        }
        for (auto& flag : flags) {
            flag.store(0, std::memory_order_release);
        }
        while (done.load(std::memory_order_acquire) > 0) {
            CpuRelax();
        }
    }

    stop.store(true, std::memory_order_release);
    for (auto& flag : flags) {
        flag.store(1, std::memory_order_release);
    }
    condition.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }

    const Percentiles stats = Summarize(samples);
    const Percentiles totals = Summarize(round_totals);
    const Percentiles notify = Summarize(notify_costs);
    std::printf("conc.native[%d]   mean=%9.1f  p50=%9.1f  p95=%9.1f  p99=%9.1f"
                "  max=%10.1f  (ns/waiter)\n",
                waiters, stats.mean, stats.p50, stats.p95, stats.p99,
                stats.max);
    std::printf("conc.native[%d].notify  mean=%9.1f  max=%10.1f  (ns/call)\n",
                waiters, notify.mean, notify.max);
    std::printf("conc.native[%d].all  mean=%9.1f  max=%10.1f  (ns/round)\n",
                waiters, totals.mean, totals.max);
    const Percentiles cpu = Summarize(cpu_costs);
    std::printf("conc.native[%d].cpu  mean=%9.1f  per-waiter=%7.1f  "
                "max=%10.1f  (ns cpu/round)\n",
                waiters, cpu.mean, cpu.mean / static_cast<double>(waiters),
                cpu.max);
}

// ---------------------------------------------------------------------------
// 场景 3：N 个等待者逐个 notify_one
// ---------------------------------------------------------------------------
void BenchNotifyOne(int waiters, int rounds) {
    ParkingCondition condition;
    std::vector<std::unique_ptr<std::mutex>> mutexes;
    mutexes.reserve(static_cast<std::size_t>(waiters));
    for (int i = 0; i < waiters; ++i) {
        mutexes.push_back(std::make_unique<std::mutex>());
    }
    std::vector<std::atomic<int>> flags(static_cast<std::size_t>(waiters));
    for (auto& flag : flags) {
        flag.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> done{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(waiters));
    for (int index = 0; index < waiters; ++index) {
        threads.emplace_back([&, index] {
            for (;;) {
                std::unique_lock<std::mutex> lock(*mutexes[index]);
                (void)condition.wait(lock, [&] {
                    return flags[index].load(std::memory_order_acquire) != 0;
                });
                lock.unlock();
                done.fetch_add(1, std::memory_order_acq_rel);
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }
                while (!stop.load(std::memory_order_acquire) &&
                       flags[index].load(std::memory_order_acquire) != 0) {
                    CpuRelax();
                }
                done.fetch_sub(1, std::memory_order_acq_rel);
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }
            }
        });
    }

    std::vector<double> round_totals;
    round_totals.reserve(rounds);
    std::vector<double> notify_costs;
    notify_costs.reserve(rounds);
    std::vector<double> cpu_costs;
    cpu_costs.reserve(rounds);

    for (int round = 0; round < rounds; ++round) {
        WaitNativeWaiters(condition, static_cast<std::size_t>(waiters));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        done.store(0, std::memory_order_release);
        const double cpu_before = ProcessCpuNs();
        const auto notify_start = Clock::now();
        for (auto& flag : flags) {
            flag.store(1, std::memory_order_release);
            condition.notify_one();
        }
        const double notify_ns = NsSince(notify_start);
        while (done.load(std::memory_order_acquire) < waiters) {
            CpuRelax();
        }
        cpu_costs.push_back(ProcessCpuNs() - cpu_before);
        notify_costs.push_back(notify_ns);
        round_totals.push_back(NsSince(notify_start));
        for (auto& flag : flags) {
            flag.store(0, std::memory_order_release);
        }
        while (done.load(std::memory_order_acquire) > 0) {
            CpuRelax();
        }
    }

    stop.store(true, std::memory_order_release);
    for (auto& flag : flags) {
        flag.store(1, std::memory_order_release);
    }
    condition.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }

    const Percentiles totals = Summarize(round_totals);
    const Percentiles notify = Summarize(notify_costs);
    const Percentiles cpu = Summarize(cpu_costs);
    std::printf("one.native[%d]    call mean=%8.1f  per-call=%7.1f  "
                "max=%9.1f  |  round mean=%9.1f  max=%9.1f  (ns)\n",
                waiters, notify.mean, notify.mean / static_cast<double>(waiters),
                notify.max, totals.mean, totals.max);
    std::printf("one.native[%d].cpu  mean=%9.1f  per-waiter=%7.1f  "
                "max=%10.1f  (ns cpu/round)\n",
                waiters, cpu.mean, cpu.mean / static_cast<double>(waiters),
                cpu.max);
}

// ---------------------------------------------------------------------------
// 场景 4：受管 Fiber 往返
// ---------------------------------------------------------------------------
void BenchFiberRtt(int iterations) {
    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.idle_wait = std::chrono::milliseconds(1);
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    ParkingCondition condition;
    std::mutex mutex;
    std::atomic<std::uint64_t> seq{0};
    std::atomic<bool> ack{false};
    std::atomic<bool> stop{false};
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));

    scheduler.spawn([&] {
        std::uint64_t local_seen = 0;
        while (!stop.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mutex);
            (void)condition.wait(lock, [&] {
                return seq.load(std::memory_order_acquire) != local_seen;
            });
            local_seen = seq.load(std::memory_order_acquire);
            lock.unlock();
            ack.store(true, std::memory_order_release);
        }
    });

    const auto deadline = Clock::now() + std::chrono::seconds(60);
    for (int i = 0; i < iterations; ++i) {
        if (i > 0) {
            ack.store(false, std::memory_order_release);
        }
        while (condition.DebugSnapshot().fiber_waiters == 0) {
            CpuRelax();
            if (Clock::now() > deadline) {
                std::printf("rtt.fiber         TIMEOUT after %d iters\n", i);
                stop.store(true, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    seq.fetch_add(1, std::memory_order_release);
                }
                condition.notify_all();
                scheduler.shutdown();
                return;
            }
        }
        seq.fetch_add(1, std::memory_order_release);
        const auto start = Clock::now();
        condition.notify_one();
        while (!ack.load(std::memory_order_acquire)) {
            CpuRelax();
        }
        samples.push_back(NsSince(start));
    }
    stop.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex);
        seq.fetch_add(1, std::memory_order_release);
    }
    condition.notify_one();
    scheduler.shutdown();

    const Percentiles stats = Summarize(samples);
    std::printf("rtt.fiber         n=%d  mean=%7.1f  p50=%7.1f  p95=%7.1f  "
                "p99=%7.1f  max=%8.1f  (ns/op)\n",
                iterations, stats.mean, stats.p50, stats.p95, stats.p99,
                stats.max);
}

int ParseInt(const char* text, int fallback) {
    if (text == nullptr) {
        return fallback;
    }
    const int value = std::atoi(text);
    return value > 0 ? value : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "rtt") {
        BenchRtt(ParseInt(argc > 2 ? argv[2] : nullptr, 20000));
    } else if (mode == "conc") {
        BenchConcurrent(ParseInt(argc > 2 ? argv[2] : nullptr, 1000),
                        ParseInt(argc > 3 ? argv[3] : nullptr, 3));
    } else if (mode == "one") {
        BenchNotifyOne(ParseInt(argc > 2 ? argv[2] : nullptr, 1000),
                       ParseInt(argc > 3 ? argv[3] : nullptr, 3));
    } else if (mode == "fiber") {
        BenchFiberRtt(ParseInt(argc > 2 ? argv[2] : nullptr, 5000));
    } else {
        BenchRtt(20000);
        BenchConcurrent(1000, 3);
        BenchNotifyOne(1000, 3);
        BenchFiberRtt(5000);
    }
    return 0;
}
