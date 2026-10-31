/**
 * @file bench_handoff_floor.cpp
 * @brief 交接原语硬件下限探针（RWMutex 写路径重设计的决策依据）。
 *
 * @date 2026-10-31
 * @author DeepSeek Agent
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 为什么需要这个探针
 *
 * 目标是把 `sync::RWMutex` 纯写 4 线程从 ~550 ns/op 降到 ≤21 ns/op。
 * 21 ns/op 是否物理可行，取决于本机的「跨核缓存行交接」下限：每 op 至少
 * 要把「轮到你了」的信息从一个核送到下一个核。本程序用几种最简原语测
 * 这个下限，用来判断「21 ns 需要每 op 几次原子写」，以及现有实现的距离
 * 是算法问题还是硬件问题。
 *
 * ## 变体
 *
 * 1. `ticket`：`tail.fetch_add` 取票 + 自旋读 `serving` + `store` 交班
 *    （每 op 两次写同一缓存行，与 Go sema / 本项目的票据方案同构）；
 * 2. `cas`：经典 CAS 自旋锁（`exchange` 抢 + `store` 释放，无公平性）；
 * 3. `word`：单 32 位字 CAS(0→1) 抢锁 + store(0) 释放，即写者快路径原语；
 * 4. `flag`：纯 ping-pong，测单次跨核 store→load 传播延迟。
 *
 * ## 口径
 *
 * 与 `bench_rwmutex_opt` 一致：1/4 线程、每线程 N 次、自建自旋屏障对齐、
 * ns/op = 总耗时 / 总 op 数。数字只用于相对比较，绝对值受 WSL2 与机器
 * 频率影响。
 *
 * ```
 * ./bench_handoff_floor            # 默认每线程 20 万次
 * ./bench_handoff_floor 1000000    # 自定义
 * ```
 */
#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

inline void CpuRelax() noexcept {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

void PinToCpu(int cpu) noexcept {
    if (std::getenv("BENCH_NO_PIN") != nullptr) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

struct Barrier {
    std::atomic<std::uint32_t> generation{0};
    std::atomic<std::uint32_t> arrived{0};
    int participants{1};
    void Wait() noexcept {
        const std::uint32_t g = generation.load(std::memory_order_acquire);
        if (arrived.fetch_add(1, std::memory_order_acq_rel) + 1U ==
            static_cast<std::uint32_t>(participants)) {
            arrived.store(0, std::memory_order_relaxed);
            generation.fetch_add(1, std::memory_order_release);
            return;
        }
        while (generation.load(std::memory_order_acquire) == g) {
            std::this_thread::yield();
        }
    }
};

/// @brief 跑一个变体：threads 个线程各 per_thread 次「交接」。
template <typename Body>
double Run(const char* name, int threads, long long per_thread, Body body) {
    Barrier barrier;
    barrier.participants = threads;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            PinToCpu(t);
            barrier.Wait();
            body(t);
        });
    }
    const auto start = Clock::now();
    for (auto& w : workers) {
        w.join();
    }
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                            start)
            .count());
    const double ops = static_cast<double>(per_thread) * threads;
    const double per_op = ns / ops;
    std::printf("%-10s %2dt %8.2f ns/op %14.0f ops/s\n", name, threads,
                per_op, ops * 1e9 / ns);
    std::fflush(stdout);
    return per_op;
}

}  // namespace

int main(int argc, char** argv) {
    long long per_thread = 200'000;
    if (argc > 1) {
        const long long parsed = std::atoll(argv[1]);
        if (parsed > 0) {
            per_thread = parsed;
        }
    }
    std::printf("# bench_handoff_floor per_thread=%lld cpus=%u\n", per_thread,
                std::thread::hardware_concurrency());
    std::fflush(stdout);

    // ---- 1) ticket：取票 RMW + 自旋读 + 交班 store ----
    for (int threads : {1, 4}) {
        std::atomic<std::uint64_t> tail{0};
        std::atomic<std::uint64_t> serving{0};
        Run("ticket", threads, per_thread, [&](int) {
            for (long long i = 0; i < per_thread; ++i) {
                const std::uint64_t ticket =
                    tail.fetch_add(1, std::memory_order_acq_rel);
                while (serving.load(std::memory_order_acquire) != ticket) {
                    CpuRelax();
                }
                serving.store(ticket + 1, std::memory_order_release);
            }
        });
    }

    // ---- 2) cas：exchange 抢 + store 释放（无公平性） ----
    for (int threads : {1, 4}) {
        std::atomic<std::uint32_t> lock{0};
        Run("cas", threads, per_thread, [&](int) {
            for (long long i = 0; i < per_thread; ++i) {
                while (lock.exchange(1, std::memory_order_acquire) != 0U) {
                    while (lock.load(std::memory_order_relaxed) != 0U) {
                        CpuRelax();
                    }
                }
                lock.store(0, std::memory_order_release);
            }
        });
    }

    // ---- 3) word：单字 CAS(0→1) + store(0)（写者快路径原语） ----
    for (int threads : {1, 4}) {
        std::atomic<std::uint32_t> word{0};
        Run("word", threads, per_thread, [&](int) {
            for (long long i = 0; i < per_thread; ++i) {
                std::uint32_t expected = 0;
                while (!word.compare_exchange_weak(
                    expected, 1U, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                    expected = 0;
                    while (word.load(std::memory_order_relaxed) != 0U) {
                        CpuRelax();
                    }
                }
                word.store(0U, std::memory_order_release);
            }
        });
    }

    // ---- 4) flag：纯 ping-pong，测单次跨核传播 ----
    for (int threads : {2, 4}) {
        std::atomic<std::uint32_t> turn{0};
        Run("flag", threads, per_thread, [&](int t) {
            for (long long i = 0; i < per_thread; ++i) {
                while (turn.load(std::memory_order_acquire) !=
                       static_cast<std::uint32_t>(t)) {
                    CpuRelax();
                }
                turn.store(static_cast<std::uint32_t>((t + 1) % threads),
                           std::memory_order_release);
            }
        });
    }
    return 0;
}
