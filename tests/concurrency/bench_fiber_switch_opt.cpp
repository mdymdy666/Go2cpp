/**
 * @file bench_fiber_switch_opt.cpp
 * @brief Fiber 上下文切换优化基准（独立可执行，不注册 ctest）。
 *
 * 覆盖本次优化的四组口径：
 *  1. 底层替换：`detail::JumpContext` 裸往返（理论下限，用于拆分 Fiber 层开销）；
 *  2. Fiber 切换：单 Fiber ping-pong（`resume()` / `resume_from_scheduler()`）、
 *     双 Fiber 交替、park 挂起恢复；
 *  3. 帧开销拆解：1e6 次批量（不含计时器干扰）与逐次采样分位数
 *     （P50/P95/P99，与 audit-base-fiber 的测法一致，便于前后对比）；
 *  4. 栈分配/释放：热缓存命中、冷轮换、创建后直接析构。
 *
 * 构建（不修改 CMake）：
 *   g++ -O3 -DNDEBUG -std=c++17 -Iinclude \
 *       tests/concurrency/bench_fiber_switch_opt.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_fiber -lgo2cpp_debug -lgo2cpp_debug_event -lgo2cpp_error \
 *       -lpthread -o /tmp/bench_fiber_switch
 */
#include "go2cpp/fiber.hpp"

#include "go2cpp/detail/context_backend.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::Fiber;
using go2cpp::SuspendReason;

volatile std::uint64_t g_sink = 0;

inline void Sink(std::uint64_t value) noexcept {
    g_sink = g_sink + value;
}

inline std::uint64_t NowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

/// @brief 批量口径：按 1000 次分块计时，取块中位数（WSL2 宿主停顿只污染少数块）。
template <typename F>
double BenchBatchNs(std::uint64_t iterations, F&& f) {
    const std::uint64_t warmup = iterations / 10 + 1;
    for (std::uint64_t i = 0; i < warmup; ++i) {
        f(i);
    }
    constexpr std::uint64_t kChunk = 1000;
    std::vector<double> chunks;
    chunks.reserve(static_cast<std::size_t>(iterations / kChunk + 1));
    for (std::uint64_t i = 0; i < iterations;) {
        const std::uint64_t count = std::min(kChunk, iterations - i);
        const std::uint64_t start = NowNs();
        for (std::uint64_t j = 0; j < count; ++j) {
            f(i + j);
        }
        const std::uint64_t elapsed = NowNs() - start;
        chunks.push_back(static_cast<double>(elapsed) /
                         static_cast<double>(count));
        i += count;
    }
    std::sort(chunks.begin(), chunks.end());
    return chunks[chunks.size() / 2U];
}

struct Quantiles {
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double min = 0.0;
    double max = 0.0;
};

Quantiles Summarize(std::vector<std::uint32_t>& samples) {
    Quantiles out;
    if (samples.empty()) {
        return out;
    }
    std::sort(samples.begin(), samples.end());
    std::uint64_t sum = 0;
    for (const auto value : samples) {
        sum += value;
    }
    out.mean = static_cast<double>(sum) / static_cast<double>(samples.size());
    const auto at = [&samples](double q) {
        const std::size_t index = static_cast<std::size_t>(
            q * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[index]);
    };
    out.p50 = at(0.50);
    out.p95 = at(0.95);
    out.p99 = at(0.99);
    out.min = static_cast<double>(samples.front());
    out.max = static_cast<double>(samples.back());
    return out;
}

/// @brief 逐次采样口径：每次操作前后取 steady_clock（与 audit-base-fiber 同法）。
///        多轮取 P50 最小的一轮，降低 WSL 宿主调度带来的整轮漂移。
template <typename F>
Quantiles BenchPerOp(std::uint64_t iterations, F&& f) {
    std::vector<std::uint32_t> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    for (std::uint64_t i = 0; i < iterations / 10 + 1; ++i) {
        f(i);
    }
    Quantiles best;
    for (int round = 0; round < 2; ++round) {
        samples.clear();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            const std::uint64_t begin = NowNs();
            f(i);
            const std::uint64_t end = NowNs();
            samples.push_back(static_cast<std::uint32_t>(end - begin));
        }
        Quantiles current = Summarize(samples);
        if (round == 0 || current.p50 < best.p50) {
            best = current;
        }
    }
    return best;
}

void PrintRow(const char* name, double ns) {
    std::printf("%-52s %10.2f ns/op\n", name, ns);
}

void PrintQuantiles(const char* name, const Quantiles& q) {
    std::printf("%-40s mean %7.2f  P50 %6.0f  P95 %6.0f  P99 %6.0f  "
                "min %6.0f  max %8.0f ns\n",
                name, q.mean, q.p50, q.p95, q.p99, q.min, q.max);
}

// ---------------------------------------------------------------------------
// 1. 底层 JumpContext 裸往返（理论下限）
// ---------------------------------------------------------------------------

struct BackendPingPong {
    go2cpp::detail::Context main_context{nullptr};
    go2cpp::detail::Context child_context{nullptr};
    std::uint64_t rounds{0};
};

void BackendEntry(go2cpp::detail::Transfer transfer) noexcept {
    auto* const state = static_cast<BackendPingPong*>(
        const_cast<void*>(transfer.data));
    state->main_context = transfer.context;
    for (;;) {
        const go2cpp::detail::Transfer next =
            go2cpp::detail::JumpContext(state->main_context, state);
        state->main_context = next.context;
    }
}

double BenchBackendPingPong(std::uint64_t iterations) {
    std::vector<char> stack(64U * 1024U);
    BackendPingPong state;
    state.rounds = iterations;
    state.child_context = go2cpp::detail::MakeContext(
        stack.data(), stack.size(), &BackendEntry);
    // 首次进入 child，child 立即跳回 main。必须保存 child 的新上下文：
    // 初始 MakeContext 保存区会被 trampoline 的栈帧复用，不能再次跳入。
    const go2cpp::detail::Transfer first =
        go2cpp::detail::JumpContext(state.child_context, &state);
    state.child_context = first.context;
    return BenchBatchNs(iterations, [&state](std::uint64_t) {
        const go2cpp::detail::Transfer transfer =
            go2cpp::detail::JumpContext(state.child_context, &state);
        state.child_context = transfer.context;
    });
}

// ---------------------------------------------------------------------------
// 2. Fiber 切换
// ---------------------------------------------------------------------------

/// @brief 单 Fiber ping-pong：每次 resume 往返一次 Suspend。
///        主体跨多轮测量存活，测量结束后由 finish() 正常退出。
struct PingPongFiber {
    std::atomic<bool> done{false};
    Fiber fiber;

    explicit PingPongFiber(SuspendReason reason)
        : fiber([this, reason] {
              for (;;) {
                  Fiber::Suspend(reason);
                  if (done.load(std::memory_order_relaxed)) {
                      return;
                  }
              }
          }) {}

    void enter(bool scheduler_entry) {
        if (scheduler_entry) {
            fiber.resume_from_scheduler();
        } else {
            fiber.resume();
        }
    }

    void resume(bool scheduler_entry) {
        if (scheduler_entry) {
            fiber.resume_from_scheduler();
        } else {
            fiber.resume();
        }
    }

    void finish() {
        done.store(true, std::memory_order_relaxed);
        fiber.resume();
    }
};

Quantiles FiberPingPong(std::uint64_t iterations, bool scheduler_entry,
                        SuspendReason reason, double* batch_ns) {
    PingPongFiber pp(reason);
    pp.enter(scheduler_entry);
    const auto resume_once = [&pp, scheduler_entry](std::uint64_t) {
        pp.resume(scheduler_entry);
    };
    *batch_ns = BenchBatchNs(iterations, resume_once);
    Quantiles q = BenchPerOp(iterations, resume_once);
    pp.finish();
    return q;
}

/// @brief 两个 Fiber 交替恢复（每次样本仍是一次 resume 往返）。
Quantiles TwoFiberPingPong(std::uint64_t iterations, double* batch_ns) {
    PingPongFiber pp_a(SuspendReason::Yield);
    PingPongFiber pp_b(SuspendReason::Yield);
    pp_a.enter(false);
    pp_b.enter(false);
    const auto resume_once = [&](std::uint64_t index) {
        if ((index & 1U) == 0) {
            pp_a.resume(false);
        } else {
            pp_b.resume(false);
        }
    };
    *batch_ns = BenchBatchNs(iterations, resume_once);
    Quantiles q = BenchPerOp(iterations, resume_once);
    pp_a.finish();
    pp_b.finish();
    return q;
}

// ---------------------------------------------------------------------------
// 3. 栈分配 / 释放
// ---------------------------------------------------------------------------

double BenchStackHot(std::uint64_t iterations, std::size_t stack_size,
                     bool growable) {
    return BenchBatchNs(iterations, [stack_size, growable](std::uint64_t) {
        Fiber fiber([] {}, stack_size, 0, growable);
        fiber.resume();
    });
}

double BenchStackCold(std::uint64_t iterations) {
    static constexpr std::size_t kSizes[] = {
        4096U, 8192U, 16384U, 32768U, 65536U, 131072U, 262144U, 524288U};
    return BenchBatchNs(iterations, [](std::uint64_t index) {
        Fiber fiber([] {}, kSizes[index % 8U], 0, false);
        fiber.resume();
    });
}

double BenchCreateDestroyOnly(std::uint64_t iterations, std::size_t stack_size) {
    return BenchBatchNs(iterations, [stack_size](std::uint64_t) {
        Fiber fiber([] {}, stack_size);
    });
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t iterations = 1000000;
    if (argc > 1) {
        iterations = std::strtoull(argv[1], nullptr, 10);
    }
    const std::uint64_t stack_iters =
        std::max<std::uint64_t>(10000, iterations / 20);

    std::printf("== fiber switch bench: iterations=%llu ==\n",
                static_cast<unsigned long long>(iterations));
    std::printf("-- 理论下限：底层 JumpContext 裸往返 --\n");
    PrintRow("JumpContext ping-pong (raw asm)",
             BenchBackendPingPong(iterations));

    std::printf("-- Fiber 切换 --\n");
    double batch = 0.0;
    Quantiles q = FiberPingPong(iterations, false, SuspendReason::Yield, &batch);
    PrintRow("Fiber::resume ping-pong (yield)", batch);
    PrintQuantiles("Fiber::resume ping-pong", q);
    q = FiberPingPong(iterations, false, SuspendReason::Park, &batch);
    PrintRow("Fiber::resume ping-pong (park)", batch);
    PrintQuantiles("Fiber::resume ping-pong park", q);
    q = FiberPingPong(iterations, true, SuspendReason::Park, &batch);
    PrintRow("Fiber::resume_from_scheduler ping-pong (park)", batch);
    PrintQuantiles("resume_from_scheduler park", q);
    q = TwoFiberPingPong(iterations, &batch);
    PrintRow("two-Fiber alternating resume", batch);
    PrintQuantiles("two-Fiber alternating resume", q);

    std::printf("-- 栈分配/释放（创建 + 首切 + 完成 + 析构）--\n");
    PrintRow("stack hot 4KiB fixed", BenchStackHot(stack_iters, 4096U, false));
    PrintRow("stack hot 4KiB growable",
             BenchStackHot(stack_iters, 4096U, true));
    PrintRow("stack hot 128KiB growable",
             BenchStackHot(stack_iters, 128U * 1024U, true));
    PrintRow("stack cold rotating sizes (8 keys)",
             BenchStackCold(stack_iters / 4));
    PrintRow("create+destroy only 4KiB (no resume)",
             BenchCreateDestroyOnly(stack_iters, 4096U));

    Sink(static_cast<std::uint64_t>(g_sink));
    return 0;
}
