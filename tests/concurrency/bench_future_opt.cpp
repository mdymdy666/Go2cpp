/**
 * @file bench_future_opt.cpp
 * @brief Future/Promise 优化基准（独立可执行，不进入 ctest）。
 *
 * 覆盖任务要求的五组数据：
 *  1. 创建/完成：MakePromise、SetValue（int/void）、SetError、Cancel、
 *     未完成析构（broken promise）；
 *  2. 已就绪快路径：GetResult / get() / GetOrThrow / Status / TryGet；
 *  3. 未就绪路径：零超时探测、已取消 Context 的 Wait、1ms 超时；
 *  4. 跨线程：SetValue → 等待方唤醒端到端、Context 取消唤醒端到端；
 *  5. 广播：8 个等待者被一次 SetValue 唤醒。
 *
 * 构建（基线 / 优化后都用同一份源码，只换 -I 指向的头文件）：
 *   c++ -O3 -DNDEBUG -std=c++17 -Iinclude tests/concurrency/bench_future_opt.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_future -lgo2cpp_context -lgo2cpp_scheduler -lgo2cpp_sync \
 *       -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_control_flow -lgo2cpp_log \
 *       -lgo2cpp_debug_event -lgo2cpp_debug -lpthread -o /tmp/bench_future
 *
 * 每项跑 5 轮取中位数，先做 1/10 的预热；报告里的 ns/op 均来自本程序。
 */
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/concurrency/future.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

volatile std::uint64_t g_sink = 0;

inline void Sink(std::uint64_t value) noexcept { g_sink = g_sink + value; }

inline std::uint64_t NowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

/// @brief 跑 rounds 轮，每轮 iterations 次，返回 ns/op 中位数。
template <typename F>
double MedianNs(std::uint64_t iterations, int rounds, F&& fn) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(rounds));
    const std::uint64_t warmup = iterations / 10 + 1;
    for (int round = 0; round < rounds; ++round) {
        for (std::uint64_t i = 0; i < warmup; ++i) {
            fn(i);
        }
        const std::uint64_t start = NowNs();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            fn(i);
        }
        const std::uint64_t elapsed = NowNs() - start;
        samples.push_back(static_cast<double>(elapsed) /
                         static_cast<double>(iterations));
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void Print(const char* name, double ns) {
    std::printf("%-54s %10.2f ns/op\n", name, ns);
}

void PrintHeader(const char* title) {
    std::printf("\n-- %s --\n", title);
}

// ---------------------------------------------------------------------------
// 1. 创建 / 完成
// ---------------------------------------------------------------------------
void BenchConstructAndComplete() {
    PrintHeader("创建与完成");
    Print("创建+SetValue+GetResult (int, 全生命周期)",
          MedianNs(200000, 5, [](std::uint64_t i) {
              auto pair = MakePromise<int>();
              (void)pair.first.SetValue(static_cast<int>(i));
              const auto result = pair.second.GetResult();
              Sink(result.Value() ? static_cast<std::uint64_t>(*result.Value())
                                  : 0);
          }));

    Print("创建+SetValue (int)",
          MedianNs(400000, 5, [](std::uint64_t) {
              auto pair = MakePromise<int>();
              Sink(pair.first.SetValue(1) ? 1U : 0U);
          }));

    Print("创建+SetValue (void)",
          MedianNs(400000, 5, [](std::uint64_t) {
              auto pair = MakePromise<void>();
              Sink(pair.first.SetValue() ? 1U : 0U);
          }));

    Print("创建+SetError",
          MedianNs(400000, 5, [](std::uint64_t) {
              auto pair = MakePromise<int>();
              Sink(pair.first.SetError(NewError("bench error")) ? 1U : 0U);
          }));

    Print("创建+Cancel",
          MedianNs(400000, 5, [](std::uint64_t) {
              auto pair = MakePromise<int>();
              Sink(pair.first.Cancel() ? 1U : 0U);
          }));

    Print("创建+未完成析构 (broken promise)",
          MedianNs(400000, 5, [](std::uint64_t) {
              auto pair = MakePromise<int>();
              Sink(pair.second.Valid() ? 1U : 0U);
          }));
}

// ---------------------------------------------------------------------------
// 2. 已就绪快路径
// ---------------------------------------------------------------------------
void BenchReadyFastPath() {
    PrintHeader("已就绪快路径");
    auto pair = MakePromise<int>();
    (void)pair.first.SetValue(42);
    const auto future = pair.second;

    Print("GetResult() 已就绪",
          MedianNs(3000000, 5, [&](std::uint64_t) {
              const auto result = future.GetResult();
              Sink(result.Value() ? static_cast<std::uint64_t>(*result.Value())
                                  : 0);
          }));

    Print("get() 已就绪",
          MedianNs(3000000, 5, [&](std::uint64_t) {
              const auto result = future.get();
              Sink(result.Value() ? 1U : 0U);
          }));

    Print("GetOrThrow() 已就绪",
          MedianNs(3000000, 5, [&](std::uint64_t) {
              Sink(static_cast<std::uint64_t>(future.GetOrThrow()));
          }));

    Print("Status() 已就绪",
          MedianNs(3000000, 5, [&](std::uint64_t) {
              Sink(static_cast<std::uint64_t>(future.Status()));
          }));

    Print("get_for(0) 已就绪",
          MedianNs(3000000, 5, [&](std::uint64_t) {
              Sink(future.get_for(ContextDuration::zero()).Ok() ? 1U : 0U);
          }));

    auto void_pair = MakePromise<void>();
    (void)void_pair.first.SetValue();
    const auto void_future = void_pair.second;
    Print("void GetResult() 已就绪",
          MedianNs(3000000, 5, [&](std::uint64_t) {
              Sink(void_future.GetResult().Ok() ? 1U : 0U);
          }));
}

// ---------------------------------------------------------------------------
// 3. 未就绪 / 取消 / 超时
// ---------------------------------------------------------------------------
void BenchPendingPaths() {
    PrintHeader("未就绪与取消");
    auto pair = MakePromise<int>();
    const auto pending = pair.second;

    Print("get_for(0) 未就绪（无 Context）",
          MedianNs(2000000, 5, [&](std::uint64_t) {
              Sink(pending.get_for(ContextDuration::zero()).TimedOut() ? 1U
                                                                      : 0U);
          }));

    Print("TryGet() 未就绪",
          MedianNs(2000000, 5, [&](std::uint64_t) {
              Sink(pending.TryGet().TimedOut() ? 1U : 0U);
          }));

    auto cancellable = WithCancel(Background());
    cancellable.second();
    const auto cancelled = cancellable.first;
    Print("Wait(已取消 Context) 未就绪",
          MedianNs(1000000, 5, [&](std::uint64_t) {
              const auto status = pending.Wait(cancelled);
              Sink(status == FutureStatus::kCancelled ? 1U : 0U);
          }));

    // 1ms 超时：每轮约 1ms，样本少一些。
    Print("GetResultFor(1ms) 未就绪（超时）",
          MedianNs(200, 3, [&](std::uint64_t) {
              Sink(pending.GetResultFor(1ms).TimedOut() ? 1U : 0U);
          }));
}

// ---------------------------------------------------------------------------
// 4. 跨线程唤醒
// ---------------------------------------------------------------------------
void BenchCrossThreadSetWake() {
    PrintHeader("跨线程唤醒（端到端）");
    constexpr std::uint32_t kRounds = 20000;
    std::atomic<Future<int>*> slot{nullptr};
    std::atomic<std::uint32_t> phase{0};
    std::atomic<std::uint32_t> armed{0};
    std::atomic<std::uint32_t> done{0};
    std::atomic<bool> stop{false};

    std::thread worker([&] {
        std::uint32_t seen = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const std::uint32_t current = phase.load(std::memory_order_acquire);
            Future<int>* target = slot.load(std::memory_order_acquire);
            if (current == seen || target == nullptr) {
                std::this_thread::yield();
                continue;
            }
            seen = current;
            armed.store(current, std::memory_order_release);
            const auto result = target->GetResult();
            if (result.Value()) {
                Sink(static_cast<std::uint64_t>(*result.Value()));
            }
            done.store(current, std::memory_order_release);
        }
    });

    std::uint64_t total = 0;
    for (std::uint32_t round = 1; round <= kRounds; ++round) {
        auto pair = MakePromise<int>();
        slot.store(&pair.second, std::memory_order_release);
        const std::uint32_t current =
            phase.fetch_add(1, std::memory_order_acq_rel) + 1;
        while (armed.load(std::memory_order_acquire) != current) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(2));
        const std::uint64_t start = NowNs();
        (void)pair.first.SetValue(static_cast<int>(round));
        while (done.load(std::memory_order_acquire) != current) {
            std::this_thread::yield();
        }
        total += NowNs() - start;
        slot.store(nullptr, std::memory_order_release);
    }
    stop.store(true, std::memory_order_release);
    worker.join();
    Print("SetValue → GetResult 唤醒（跨线程 e2e）",
          static_cast<double>(total) / kRounds);
}

void BenchCrossThreadCancelWake() {
    constexpr std::uint32_t kRounds = 10000;
    struct Target {
        const Future<int>* future{nullptr};
        ContextPtr context;
    };
    std::atomic<Target*> slot{nullptr};
    std::atomic<std::uint32_t> phase{0};
    std::atomic<std::uint32_t> armed{0};
    std::atomic<std::uint32_t> done{0};
    std::atomic<bool> stop{false};

    std::thread worker([&] {
        std::uint32_t seen = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const std::uint32_t current = phase.load(std::memory_order_acquire);
            Target* target = slot.load(std::memory_order_acquire);
            if (current == seen || target == nullptr) {
                std::this_thread::yield();
                continue;
            }
            seen = current;
            armed.store(current, std::memory_order_release);
            const auto status = target->future->Wait(target->context);
            Sink(status == FutureStatus::kCancelled ? 1U : 0U);
            done.store(current, std::memory_order_release);
        }
    });

    std::uint64_t total = 0;
    for (std::uint32_t round = 1; round <= kRounds; ++round) {
        auto pair = MakePromise<int>();
        auto context_pair = WithCancel(Background());
        Target target;
        target.future = &pair.second;
        target.context = context_pair.first;
        slot.store(&target, std::memory_order_release);
        const std::uint32_t current =
            phase.fetch_add(1, std::memory_order_acq_rel) + 1;
        while (armed.load(std::memory_order_acquire) != current) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(2));
        const std::uint64_t start = NowNs();
        context_pair.second();
        while (done.load(std::memory_order_acquire) != current) {
            std::this_thread::yield();
        }
        total += NowNs() - start;
        slot.store(nullptr, std::memory_order_release);
        Sink(pair.first.Cancel() ? 1U : 0U);
    }
    stop.store(true, std::memory_order_release);
    worker.join();
    Print("Context Cancel → Wait 唤醒（跨线程 e2e）",
          static_cast<double>(total) / kRounds);
}

void BenchMultiWaiterBroadcast() {
    constexpr std::uint32_t kWaiters = 8;
    constexpr std::uint32_t kRounds = 2000;
    std::atomic<Future<int>*> slot{nullptr};
    std::atomic<std::uint32_t> phase{0};
    std::atomic<std::uint32_t> armed{0};
    std::atomic<std::uint32_t> done{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> workers;
    for (std::uint32_t index = 0; index < kWaiters; ++index) {
        workers.emplace_back([&] {
            std::uint32_t seen = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const std::uint32_t current =
                    phase.load(std::memory_order_acquire);
                Future<int>* target = slot.load(std::memory_order_acquire);
                if (current == seen || target == nullptr) {
                    std::this_thread::yield();
                    continue;
                }
                seen = current;
                armed.fetch_add(1, std::memory_order_acq_rel);
                const auto result = target->GetResult();
                if (result.Value()) {
                    Sink(static_cast<std::uint64_t>(*result.Value()));
                }
                done.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }

    std::uint64_t total = 0;
    for (std::uint32_t round = 1; round <= kRounds; ++round) {
        auto pair = MakePromise<int>();
        slot.store(&pair.second, std::memory_order_release);
        const std::uint32_t current =
            phase.fetch_add(1, std::memory_order_acq_rel) + 1;
        const std::uint32_t target_armed = current * kWaiters;
        while (armed.load(std::memory_order_acquire) < target_armed) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(5));
        const std::uint64_t start = NowNs();
        (void)pair.first.SetValue(7);
        while (done.load(std::memory_order_acquire) < target_armed) {
            std::this_thread::yield();
        }
        total += NowNs() - start;
        slot.store(nullptr, std::memory_order_release);
    }
    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    Print("1 次 SetValue 广播 8 个等待者（总时长/8）",
          static_cast<double>(total) / (kRounds * kWaiters));
}

}  // namespace

int main() {
    std::printf("== Future 性能基准（Release，%u 线程）==\n",
                std::thread::hardware_concurrency());
    BenchConstructAndComplete();
    BenchReadyFastPath();
    BenchPendingPaths();
    BenchCrossThreadSetWake();
    BenchCrossThreadCancelWake();
    BenchMultiWaiterBroadcast();
    std::printf("\nsink=%llu\n",
                static_cast<unsigned long long>(g_sink));
    return 0;
}
