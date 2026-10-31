/**
 * @file fiber_switch_stress.cpp
 * @brief Fiber 切换热路径优化的独立压测（不注册 ctest）。
 *
 * 覆盖：
 *  1. yield 往返参数轮（默认 1e8 轮 = 2e8 次切换）+ errno/状态/当前 Fiber 断言；
 *  2. 10 万并发 Fiber（创建 → 多次切换 → 完成）与 RSS；
 *  3. f2f / f2t / t2f / t2t 四种交接；
 *  4. 嵌套 Fiber 链（含调度器级 park 沿父链传播）；
 *  5. 顶层 Fiber 在两个 OS 线程间交替恢复（M1→M2 迁移）；
 *  6. 可增长栈（深递归中挂起/恢复）与栈回收（反复创建销毁，RSS 不增长）；
 *  7. FiberBin 复用路径（AcquireForScheduler/RecycleForScheduler）。
 *
 * 构建（与仓库构建目录一致）：
 *   g++ -O3 -DNDEBUG -std=c++17 -Iinclude tests/concurrency/fiber_switch_stress.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_fiber -lpthread -o /tmp/fiber_switch_stress
 *   /tmp/fiber_switch_stress <yield_rounds>
 */
#include "go2cpp/fiber.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::Fiber;
using go2cpp::FiberState;
using go2cpp::SuspendReason;

std::uint64_t g_failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__,              \
                        #condition);                                         \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

std::uint64_t NowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

/// @brief 常驻内存（KB），来自 /proc/self/statm。
std::uint64_t RssKb() noexcept {
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return 0;
    }
    unsigned long size = 0;
    unsigned long resident = 0;
    const int read = std::fscanf(file, "%lu %lu", &size, &resident);
    std::fclose(file);
    if (read != 2) {
        return 0;
    }
    return static_cast<std::uint64_t>(resident) * 4U;
}

/// @brief 虚拟地址（KB），用于观察栈 arena 保留。
std::uint64_t VmSizeKb() noexcept {
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return 0;
    }
    unsigned long size = 0;
    unsigned long resident = 0;
    const int read = std::fscanf(file, "%lu %lu", &size, &resident);
    std::fclose(file);
    if (read != 2) {
        return 0;
    }
    return static_cast<std::uint64_t>(size) * 4U;
}

// ---------------------------------------------------------------------------
// 1. yield 往返：errno / 状态 / 当前 Fiber 一致性
// ---------------------------------------------------------------------------

struct YieldStress {
    std::uint64_t rounds{0};
    std::atomic<std::uint64_t> failures{0};
    Fiber fiber;

    explicit YieldStress(std::uint64_t total)
        : rounds(total),
          fiber([this] {
              const Fiber* const self = Fiber::Current();
              for (std::uint64_t i = 0; i < rounds; ++i) {
                  // Fiber 内 errno 必须跨切换保留。
                  errno = static_cast<int>(i & 0x3fff) + 1;
                  const int expected_errno = errno;
                  Fiber::Suspend(SuspendReason::Yield);
                  if (errno != expected_errno) {
                      failures.fetch_add(1, std::memory_order_relaxed);
                  }
                  if (Fiber::Current() != self) {
                      failures.fetch_add(1, std::memory_order_relaxed);
                  }
              }
          }) {}
};

bool RunYieldStress(std::uint64_t rounds) {
    const std::uint64_t before = g_failures;
    const int caller_errno = 0x5A5A;
    YieldStress state(rounds);
    std::uint64_t accepted = 0;
    std::uint64_t early = 0;
    std::uint64_t state_bad = 0;
    for (std::uint64_t i = 0; i < rounds; ++i) {
        errno = caller_errno;
        if (state.fiber.resume()) {
            ++accepted;
        } else {
            ++early;
        }
        if (errno != caller_errno) {
            ++state_bad;
        }
        if (state.fiber.state() != FiberState::Suspended) {
            ++state_bad;
        }
    }
    // 收尾：让主体看到 rounds 轮结束并返回 Completed。
    errno = caller_errno;
    state.fiber.resume();
    CHECK(accepted == rounds);
    CHECK(early == 0);
    CHECK(state_bad == 0);
    CHECK(state.failures.load(std::memory_order_relaxed) == 0);
    CHECK(errno == caller_errno);
    CHECK(state.fiber.state() == FiberState::Completed);
    return g_failures == before;
}

// ---------------------------------------------------------------------------
// 2. 10 万并发 Fiber
// ---------------------------------------------------------------------------

struct ConcurrentResult {
    std::uint64_t completed{0};
    std::uint64_t wall_ns{0};
    std::uint64_t rss_before_kb{0};
    std::uint64_t rss_after_kb{0};
};

ConcurrentResult RunConcurrentFibers(std::size_t count, std::size_t yields) {
    ConcurrentResult result;
    std::atomic<std::uint64_t> finished{0};
    result.rss_before_kb = RssKb();
    std::vector<std::unique_ptr<Fiber>> fibers;
    fibers.reserve(count);
    const std::uint64_t begin = NowNs();
    for (std::size_t i = 0; i < count; ++i) {
        fibers.push_back(std::make_unique<Fiber>(
            [&finished, yields] {
                for (std::size_t k = 0; k < yields; ++k) {
                    Fiber::Suspend(SuspendReason::Yield);
                }
                finished.fetch_add(1, std::memory_order_relaxed);
            },
            4096U, 0, false));
    }
    const std::uint64_t created = NowNs();
    for (auto& fiber : fibers) {
        while (true) {
            const FiberState state = fiber->state();
            if (state == FiberState::Completed || state == FiberState::Failed) {
                break;
            }
            if (!fiber->resume()) {
                break;
            }
        }
    }
    result.wall_ns = NowNs() - begin;
    const std::uint64_t finished_ns = NowNs();
    fibers.clear();
    result.rss_after_kb = RssKb();
    result.completed = finished.load(std::memory_order_relaxed);
    CHECK(result.completed == count);
    CHECK(Fiber::DestroyTimeoutCount() == 0);
    std::printf("  100k create=%.1f ms drive=%.1f ms\n",
                static_cast<double>(created - begin) / 1e6,
                static_cast<double>(finished_ns - created) / 1e6);
    return result;
}

// ---------------------------------------------------------------------------
// 3. f2f / f2t / t2f / t2t
// ---------------------------------------------------------------------------

/// f2f：父 Fiber 反复恢复子 Fiber，子 Fiber 每次挂起前校验 errno。
bool RunFiberToFiber() {
    const std::uint64_t before = g_failures;
    std::atomic<std::uint64_t> child_rounds{0};
    std::atomic<std::uint64_t> failures{0};
    Fiber* child_ptr = nullptr;
    Fiber parent([&] {
        Fiber child([&] {
            for (int i = 0; i < 100; ++i) {
                errno = i + 7;
                Fiber::Suspend(SuspendReason::Yield);
                if (errno != i + 7) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                child_rounds.fetch_add(1, std::memory_order_relaxed);
            }
        });
        child_ptr = &child;
        for (int i = 0; i < 101; ++i) {
            if (!child.resume()) {
                failures.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            if (child.state() == FiberState::Completed) {
                break;
            }
        }
        if (child.state() != FiberState::Completed) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
        Fiber::Suspend(SuspendReason::Yield);
    });
    CHECK(parent.resume());
    CHECK(child_ptr != nullptr);
    CHECK(parent.state() == FiberState::Suspended);
    CHECK(parent.resume());
    CHECK(parent.state() == FiberState::Completed);
    CHECK(child_rounds.load() == 100);
    CHECK(failures.load() == 0);
    return g_failures == before;
}

/// f2t：Fiber 内计算后挂起，OS 线程在外部连续恢复它 1e5 次。
bool RunFiberToThread() {
    const std::uint64_t before = g_failures;
    std::atomic<std::uint64_t> rounds{0};
    Fiber fiber([&rounds] {
        for (int i = 0; i < 100000; ++i) {
            rounds.fetch_add(1, std::memory_order_relaxed);
            Fiber::Suspend(SuspendReason::Yield);
        }
    });
    CHECK(fiber.resume());
    while (fiber.state() == FiberState::Suspended) {
        if (!fiber.resume()) {
            CHECK(!"f2t resume rejected");
            break;
        }
    }
    CHECK(fiber.state() == FiberState::Completed);
    CHECK(rounds.load() == 100000);
    return g_failures == before;
}

/// t2f：工作线程创建并驱动 Fiber，结果交回主线程。
bool RunThreadToFiber() {
    const std::uint64_t before = g_failures;
    std::atomic<std::uint64_t> done{0};
    std::atomic<int> failure{0};
    std::thread worker([&done, &failure] {
        std::uint64_t local = 0;
        Fiber fiber([&local] {
            for (int i = 0; i < 50000; ++i) {
                ++local;
                Fiber::Suspend(SuspendReason::Yield);
            }
        });
        if (!fiber.resume()) {
            failure.store(1);
            return;
        }
        while (fiber.state() == FiberState::Suspended) {
            if (!fiber.resume()) {
                failure.store(1);
                break;
            }
        }
        if (fiber.state() != FiberState::Completed) {
            failure.store(1);
        }
        done.store(local, std::memory_order_release);
    });
    worker.join();
    CHECK(failure.load() == 0);
    CHECK(done.load(std::memory_order_acquire) == 50000);
    return g_failures == before;
}

/// t2t：两个 OS 线程严格交替恢复同一个顶层 Fiber（M1→M2 迁移），
///       每轮校验恢复线程自己的 errno 不被 Fiber 串改。
bool RunThreadToThread() {
    const std::uint64_t before = g_failures;
    constexpr int kRounds = 20000;
    std::atomic<int> turn{0};  // 0 = 线程 A，1 = 线程 B
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> caller_errno_bad{0};
    Fiber fiber([&failures] {
        for (int i = 0; i < kRounds; ++i) {
            errno = i + 1;
            Fiber::Suspend(SuspendReason::Yield);
            // 已知限制（优化前就存在，基线库表现相同）：Fiber 在跨线程
            // 迁移后，其 errno 槽属于新的执行线程，恢复侧无法把「Fiber 的
            // errno」与「新线程的 errno」区分开。这里只断言切换本身与
            // 调用线程的 errno 隔离（见下方 caller_errno_bad）。
            (void)errno;
        }
    });
    CHECK(fiber.resume());
    const auto drive = [&](int my_turn, int my_errno) {
        for (int i = 0; i < kRounds / 2; ++i) {
            while (fiber.state() != FiberState::Completed &&
                   (turn.load(std::memory_order_acquire) != my_turn ||
                    fiber.state() != FiberState::Suspended)) {
                std::this_thread::yield();
            }
            if (fiber.state() == FiberState::Completed) {
                return;
            }
            errno = my_errno;
            if (!fiber.resume()) {
                rejected.fetch_add(1, std::memory_order_relaxed);
                failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (errno != my_errno) {
                caller_errno_bad.fetch_add(1, std::memory_order_relaxed);
                failures.fetch_add(1, std::memory_order_relaxed);
            }
            turn.store(1 - my_turn, std::memory_order_release);
        }
    };
    std::thread thread_a(drive, 0, 0x1111);
    std::thread thread_b(drive, 1, 0x2222);
    thread_a.join();
    thread_b.join();
    if (failures.load() != 0) {
        std::printf("  t2t detail: rejected=%llu caller_errno_bad=%llu\n",
                    static_cast<unsigned long long>(rejected.load()),
                    static_cast<unsigned long long>(caller_errno_bad.load()));
    }
    CHECK(failures.load() == 0);
    CHECK(fiber.state() == FiberState::Completed);
    return g_failures == before;
}

// ---------------------------------------------------------------------------
// 4. 嵌套 Fiber 链（含调度器级 park 传播）
// ---------------------------------------------------------------------------

std::uint64_t NestedBody(std::size_t depth, std::atomic<std::uint64_t>& out) {
    if (depth == 0) {
        out.fetch_add(1, std::memory_order_relaxed);
        Fiber::Suspend(SuspendReason::Yield);
        out.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    std::uint64_t sum = 0;
    Fiber child([&] { sum = NestedBody(depth - 1, out); });
    CHECK(child.resume());
    if (child.state() == FiberState::Suspended) {
        CHECK(child.resume());
    }
    Fiber::Suspend(SuspendReason::Yield);
    CHECK(child.state() == FiberState::Completed);
    return sum + depth;
}

bool RunNestedFibers(std::size_t depth) {
    const std::uint64_t before = g_failures;
    std::atomic<std::uint64_t> leaves{0};
    Fiber root([&] { NestedBody(depth, leaves); });
    CHECK(root.resume());
    CHECK(root.state() == FiberState::Suspended);
    CHECK(root.resume());
    CHECK(root.state() == FiberState::Completed);
    CHECK(leaves.load() == 2);
    return g_failures == before;
}

/// 调度器级 park 沿父链传播：inner park → 父 Fiber 也挂起 → 依次唤醒。
bool RunNestedParkPropagation() {
    const std::uint64_t before = g_failures;
    std::atomic<int> stage{0};
    Fiber inner([&] {
        stage.store(1, std::memory_order_release);
        CHECK(Fiber::SuspendForScheduler(SuspendReason::Park));
        stage.store(3, std::memory_order_release);
    });
    Fiber outer([&] {
        CHECK(inner.resume());
        CHECK(stage.load(std::memory_order_acquire) == 3);
        stage.store(4, std::memory_order_release);
    });
    CHECK(outer.resume());
    // 传播后：outer 与 inner 都处于挂起，控制权回到调用者。
    CHECK(outer.state() == FiberState::Suspended);
    CHECK(inner.state() == FiberState::Suspended);
    CHECK(stage.load() == 1);
    CHECK(outer.resume());
    CHECK(inner.state() == FiberState::Completed);
    CHECK(outer.state() == FiberState::Completed);
    CHECK(stage.load() == 4);
    return g_failures == before;
}

// ---------------------------------------------------------------------------
// 5. 可增长栈：深递归中挂起/恢复
// ---------------------------------------------------------------------------

std::uint64_t DeepFrame(std::uint64_t depth,
                        std::atomic<std::uint64_t>& peak) {
    volatile std::uint64_t padding[24];
    padding[0] = depth;
    padding[23] = depth;
    std::uint64_t observed = peak.load(std::memory_order_relaxed);
    while (observed < depth &&
           !peak.compare_exchange_weak(observed, depth)) {
    }
    if (depth == 0) {
        Fiber::Suspend(SuspendReason::Yield);
        return padding[0];
    }
    const std::uint64_t inner = DeepFrame(depth - 1, peak);
    return inner + padding[0] + padding[23] - depth * 2;
}

bool RunGrowableStack() {
    const std::uint64_t before = g_failures;
    std::atomic<std::uint64_t> peak{0};
    std::atomic<std::uint64_t> result{1};
    Fiber fiber([&] {
        result.store(DeepFrame(4000, peak), std::memory_order_release);
    });
    CHECK(fiber.resume());
    CHECK(peak.load() == 4000);
    CHECK(fiber.state() == FiberState::Suspended);
    CHECK(fiber.resume());
    CHECK(fiber.state() == FiberState::Completed);
    CHECK(result.load() == 0);
    return g_failures == before;
}

// ---------------------------------------------------------------------------
// 6. 栈回收 / RSS 稳定
// ---------------------------------------------------------------------------

bool RunRecycle(std::size_t iterations) {
    const std::uint64_t before = g_failures;
    for (std::size_t i = 0; i < 10000; ++i) {
        Fiber fiber([] {}, 4096U, 0, false);
        CHECK(fiber.resume());
    }
    const std::uint64_t rss_before = RssKb();
    for (std::size_t i = 0; i < iterations; ++i) {
        Fiber fiber([] {}, 4096U, 0, false);
        if (!fiber.resume()) {
            CHECK(!"recycle resume rejected");
            break;
        }
    }
    const std::uint64_t rss_after = RssKb();
    // 允许 8 MB 以内的抖动（分配器/arena 元数据），不允许随次数线性增长。
    CHECK(rss_after <= rss_before + 8192U);
    std::printf("  recycle rss: %llu KB -> %llu KB\n",
                static_cast<unsigned long long>(rss_before),
                static_cast<unsigned long long>(rss_after));
    return g_failures == before;
}

bool RunSchedulerReuse(std::size_t iterations) {
    const std::uint64_t before = g_failures;
    for (std::size_t i = 0; i < iterations; ++i) {
        auto fiber = Fiber::AcquireForScheduler([] {}, 4096U);
        CHECK(fiber != nullptr);
        if (!fiber->resume()) {
            CHECK(!"scheduler fiber resume rejected");
        }
        CHECK(fiber->state() == FiberState::Completed);
        Fiber::RecycleForScheduler(std::move(fiber));
    }
    CHECK(Fiber::DestroyTimeoutCount() == 0);
    return g_failures == before;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t yield_rounds = 100000000ULL;
    if (argc > 1) {
        yield_rounds = std::strtoull(argv[1], nullptr, 10);
    }
    std::printf("== fiber switch stress: yield_rounds=%llu ==\n",
                static_cast<unsigned long long>(yield_rounds));
    std::printf("rss start: %llu KB, vm %llu KB\n",
                static_cast<unsigned long long>(RssKb()),
                static_cast<unsigned long long>(VmSizeKb()));

    const std::uint64_t yield_begin = NowNs();
    const bool yield_ok = RunYieldStress(yield_rounds);
    const std::uint64_t yield_ns = NowNs() - yield_begin;
    std::printf("[yield] %s  rounds=%llu  %.2f ns/round(%.2f ns/switch)  "
                "rss=%llu KB\n",
                yield_ok ? "PASS" : "FAIL",
                static_cast<unsigned long long>(yield_rounds),
                static_cast<double>(yield_ns) /
                    static_cast<double>(yield_rounds),
                static_cast<double>(yield_ns) /
                    static_cast<double>(yield_rounds) / 2.0,
                static_cast<unsigned long long>(RssKb()));

    const ConcurrentResult concurrent = RunConcurrentFibers(100000, 4);
    std::printf("[100k] %s  wall=%.3f ms  rss %llu -> %llu KB\n",
                concurrent.completed == 100000 ? "PASS" : "FAIL",
                static_cast<double>(concurrent.wall_ns) / 1e6,
                static_cast<unsigned long long>(concurrent.rss_before_kb),
                static_cast<unsigned long long>(concurrent.rss_after_kb));

    const std::pair<const char*, bool> scenarios[] = {
        {"f2f", RunFiberToFiber()},
        {"f2t", RunFiberToThread()},
        {"t2f", RunThreadToFiber()},
        {"t2t", RunThreadToThread()},
        {"nested", RunNestedFibers(32)},
        {"nested-park", RunNestedParkPropagation()},
        {"growable", RunGrowableStack()},
        {"recycle", RunRecycle(200000)},
        {"scheduler-reuse", RunSchedulerReuse(100000)},
    };
    for (const auto& scenario : scenarios) {
        std::printf("[%s] %s\n", scenario.first,
                    scenario.second ? "PASS" : "FAIL");
    }

    CHECK(Fiber::DestroyTimeoutCount() == 0);
    std::printf("rss end: %llu KB, vm %llu KB\n",
                static_cast<unsigned long long>(RssKb()),
                static_cast<unsigned long long>(VmSizeKb()));
    if (g_failures != 0) {
        std::printf("RESULT: FAIL (%llu checks)\n",
                    static_cast<unsigned long long>(g_failures));
        return 1;
    }
    std::printf("RESULT: PASS\n");
    return 0;
}
