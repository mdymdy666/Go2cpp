/**
 * @file bench_rwmutex_opt.cpp
 * @brief `sync::RWMutex` 读 / 写 / 混合（90% 读）基准，覆盖 1 / 4 / 8 / 16
 *        线程与 ns/op、ops/s 两项指标。
 *
 * @date 2026-10-30
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 用法
 *
 * 本文件是独立可执行程序（不注册 ctest，也不进入 go2cpp_tests）：
 *
 * ```
 * ./bench_rwmutex_opt              # 默认：每线程 20 万次，3 轮
 * ./bench_rwmutex_opt 100000 5     # 自定义每线程 ops 与轮数
 * ```
 *
 * ## 测量口径
 *
 * - 一次 op = 一次完整的 `RLock/RUnlock`（读）或 `Lock/Unlock`（写）pair；
 * - 混合负载固定 10% 写、90% 读（与 `audit-base-sync.md` 的 90% 读口径
 *   一致），按线程内 `(i + t) % 10 == 0` 决定写，避免随机数生成器把
 *   开销摊进临界区；
 * - 每轮开始前用自建自旋屏障对齐；轮内所有线程的 ops 相加得到总 ops；
 * - 结果为**每轮**独立输出，便于外部取中位数；进程内不做跨轮平均，避免
 *   把冷启动 / 降频轮次混进结论；
 * - 线程用 `pthread_setaffinity_np` 尽量绑定到不同 CPU（WSL 下失败则忽略）。
 */
#include "go2cpp/concurrency/rwmutex.hpp"

#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaxCpu = 64;

std::atomic<std::uint32_t> g_generation{0};
std::atomic<std::uint32_t> g_arrived{0};

class SpinBarrier {
public:
    explicit SpinBarrier(int participants) : m_participants(participants) {}

    void Wait() noexcept {
        const std::uint32_t generation =
            g_generation.load(std::memory_order_acquire);
        if (g_arrived.fetch_add(1, std::memory_order_acq_rel) + 1U ==
            static_cast<std::uint32_t>(m_participants)) {
            g_arrived.store(0, std::memory_order_relaxed);
            g_generation.fetch_add(1, std::memory_order_release);
            return;
        }
        while (g_generation.load(std::memory_order_acquire) == generation) {
            std::this_thread::yield();
        }
    }

private:
    int m_participants;
};

/// @brief 把当前线程绑到指定 CPU；失败（WSL 容器 / 权限）时静默忽略。
void PinToCpu(int cpu) noexcept {
    if (std::getenv("BENCH_NO_PIN") != nullptr) {
        return;
    }
    if (cpu < 0 || cpu >= kMaxCpu) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

double NsPerOp(Clock::duration elapsed, long long ops) noexcept {
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    return ops > 0 ? ns / static_cast<double>(ops) : 0.0;
}

double OpsPerSec(Clock::duration elapsed, long long ops) noexcept {
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    return ns > 0.0 ? static_cast<double>(ops) * 1e9 / ns : 0.0;
}

struct Result {
    double ns_per_op{0.0};
    double ops_per_sec{0.0};
};

enum class Mode { kRead, kWrite, kMixed };

const char* ModeName(Mode mode) noexcept {
    switch (mode) {
        case Mode::kRead:
            return "read";
        case Mode::kWrite:
            return "write";
        case Mode::kMixed:
            return "mixed90";
    }
    return "?";
}

/// @brief 跑一轮：threads 个线程各执行 per_thread 次 op。
Result RunRound(Mode mode, int threads, long long per_thread,
                long long* total_ops_out) {
    go2cpp::sync::RWMutex rw;
    SpinBarrier barrier(threads);
    std::atomic<long long> completed{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));

    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            PinToCpu(t);
            std::uint64_t sink = 0;
            barrier.Wait();
            for (long long i = 0; i < per_thread; ++i) {
                bool write_op = false;
                if (mode == Mode::kWrite) {
                    write_op = true;
                } else if (mode == Mode::kMixed) {
                    // 90% 读：每 10 次里 1 次写；salt 只错开相位，不改变比例。
                    write_op = ((i + t) % 10) == 0;
                }
                if (write_op) {
                    rw.Lock();
                    sink += 1;
                    rw.Unlock();
                } else {
                    rw.RLock();
                    sink += 2;
                    rw.RUnlock();
                }
            }
            completed.fetch_add(per_thread, std::memory_order_relaxed);
            // 防止整个循环被优化掉（sink 只依赖临界区内的常量，编译器
            // 可能把锁调用当作可消除；用可观测副作用固定住）。
            if (sink == 0xFFFFFFFFFFFFFFFFULL) {
                std::printf("unexpected sink\n");
            }
        });
    }

    const auto start = Clock::now();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = Clock::now() - start;
    const long long ops = completed.load(std::memory_order_relaxed);
    *total_ops_out = ops;
    return Result{NsPerOp(elapsed, ops), OpsPerSec(elapsed, ops)};
}

void Report(const char* mode, int threads, int round, long long ops,
            const Result& result) {
    std::printf("%-9s %2dt round=%d ops=%10lld %9.2f ns/op %14.0f ops/s\n",
                mode, threads, round, ops, result.ns_per_op,
                result.ops_per_sec);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    long long per_thread = 200'000;
    int rounds = 3;
    if (argc > 1) {
        const long long parsed = std::atoll(argv[1]);
        if (parsed > 0) {
            per_thread = parsed;
        }
    }
    if (argc > 2) {
        const int parsed = std::atoi(argv[2]);
        if (parsed > 0) {
            rounds = parsed;
        }
    }

    const int cpu_count = static_cast<int>(std::thread::hardware_concurrency());
    std::printf("# bench_rwmutex_opt per_thread=%lld rounds=%d cpus=%d\n",
                per_thread, rounds, cpu_count);
    std::fflush(stdout);

    const int thread_counts[] = {1, 2, 3, 4, 5, 6, 7, 8, 16};
    Mode only_mode = Mode::kRead;
    bool filter_mode = false;
    int max_threads = 16;
    if (argc > 3) {
        const std::string_view mode_arg(argv[3]);
        if (mode_arg == "read") {
            only_mode = Mode::kRead;
            filter_mode = true;
        } else if (mode_arg == "write") {
            only_mode = Mode::kWrite;
            filter_mode = true;
        } else if (mode_arg == "mixed") {
            only_mode = Mode::kMixed;
            filter_mode = true;
        }
    }
    if (argc > 4) {
        const int parsed = std::atoi(argv[4]);
        if (parsed > 0) {
            max_threads = parsed;
        }
    }
    const Mode modes[] = {Mode::kRead, Mode::kWrite, Mode::kMixed};
    int only_threads = 0;
    if (const char* ts = std::getenv("BENCH_THREADS")) {
        only_threads = std::atoi(ts);
    }

    for (Mode mode : modes) {
        if (filter_mode && mode != only_mode) {
            continue;
        }
        for (int threads : thread_counts) {
            if (threads > max_threads) {
                continue;
            }
            if (only_threads > 0 && threads != only_threads) {
                continue;
            }
            for (int round = 1; round <= rounds; ++round) {
                long long ops = 0;
                const Result result =
                    RunRound(mode, threads, per_thread, &ops);
                Report(ModeName(mode), threads, round, ops, result);
            }
        }
    }
    return 0;
}
