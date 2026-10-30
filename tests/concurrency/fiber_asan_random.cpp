/**
 * @file fiber_asan_random.cpp
 * @brief Fiber 随机操作 + 深栈 + 多线程迁移的 ASan/UBSan 冒烟测试。
 *
 * 独立可执行（不注册 ctest）。ASan 构建下 Fiber 实现按设计退化为固定栈，
 * 本程序覆盖：创建/挂起/恢复/取消/析构、深递归、跨线程恢复、批量并发，
 * 用固定 seed 可复现。退出码 0 表示无 sanitizer 报告、无断言失败。
 *
 * 构建：
 *   g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -DGO2CPP_USE_NATIVE_CONTEXT=1 -Iinclude \
 *       tests/concurrency/fiber_asan_random.cpp \
 *       src/concurrency/fiber.cpp src/concurrency/fiber_local.cpp \
 *       src/concurrency/context_backend.cpp src/concurrency/context_x64.S \
 *       -lpthread -o /tmp/fiber_asan_random
 *
 * 运行：
 *   ASAN_OPTIONS=detect_leaks=0 /tmp/fiber_asan_random
 *   （fiber_local 与隔离区按设计长期存活到进程退出，故关闭 leak 检测；
 *     use-after-free / 越界 / UBSan 仍然生效。）
 */
#include "go2cpp/fiber.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using go2cpp::Fiber;
using go2cpp::SuspendReason;

std::atomic<int> g_failures{0};

#define CHECK_TRUE(expr)                                                     \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::printf("check failed at %s:%d: %s\n", __FILE__, __LINE__,   \
                        #expr);                                              \
            g_failures.fetch_add(1, std::memory_order_relaxed);              \
        }                                                                    \
    } while (0)

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed == 0 ? 1 : seed) {}
    std::uint64_t Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    std::size_t Below(std::size_t bound) {
        return bound == 0 ? 0 : static_cast<std::size_t>(Next() % bound);
    }
};

__attribute__((noinline)) std::uint64_t DeepWalk(std::size_t depth) {
    volatile char pad[256];
    pad[0] = static_cast<char>(depth & 0x7F);
    pad[255] = static_cast<char>((depth >> 7) & 0x7F);
    if (depth == 0) {
        return static_cast<std::uint64_t>(
            static_cast<unsigned char>(pad[0]) +
            static_cast<unsigned char>(pad[255]));
    }
    const std::uint64_t below = DeepWalk(depth - 1);
    return below + static_cast<std::uint64_t>(
                       static_cast<unsigned char>(pad[0]) +
                       static_cast<unsigned char>(pad[255]));
}

std::uint64_t ExpectedWalk(std::size_t depth) {
    std::uint64_t sum = 0;
    for (std::size_t d = 0; d <= depth; ++d) {
        sum += static_cast<unsigned char>(d & 0x7F);
        sum += static_cast<unsigned char>((d >> 7) & 0x7F);
    }
    return sum;
}

/// @brief 随机 walk：seed 可复现。
void RandomWalkScenario() {
    for (std::uint64_t seed : {std::uint64_t(999), std::uint64_t(7),
                               std::uint64_t(42), std::uint64_t(20261030)}) {
        Rng rng(seed);
        constexpr std::size_t kOps = 3000;
        std::vector<std::unique_ptr<Fiber>> live;
        live.reserve(64);
        std::atomic<std::size_t> bodies{0};
        std::atomic<std::size_t> cancelled{0};
        std::size_t created = 0;
        for (std::size_t op = 0; op < kOps; ++op) {
            const std::size_t choice = rng.Below(100);
            if (choice < 28 && live.size() < 64) {
                const std::size_t steps = rng.Below(6);
                const std::size_t depth = rng.Below(8) == 0 ? rng.Below(2000) : 0;
                const bool yield_only = rng.Below(4) != 0;
                auto fiber = Fiber::AcquireForScheduler(
                    [steps, depth, yield_only, &bodies, &cancelled] {
                        bodies.fetch_add(1, std::memory_order_relaxed);
                        if (!yield_only && Fiber::CancellationRequested()) {
                            cancelled.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        if (depth != 0) {
                            const std::uint64_t got = DeepWalk(depth);
                            CHECK_TRUE(got == ExpectedWalk(depth));
                        }
                        for (std::size_t step = 0; step < steps; ++step) {
                            if (!Fiber::Suspend(SuspendReason::Yield)) {
                                return;
                            }
                            if (Fiber::CancellationRequested()) {
                                cancelled.fetch_add(1,
                                                    std::memory_order_relaxed);
                                return;
                            }
                        }
                    },
                    4U * 1024U * 1024U, 32U);
                CHECK_TRUE(fiber != nullptr);
                live.push_back(std::move(fiber));
                ++created;
            } else if (choice < 62 && !live.empty()) {
                const std::size_t index = rng.Below(live.size());
                Fiber* fiber = live[index].get();
                if (fiber->state() == go2cpp::FiberState::Completed ||
                    fiber->state() == go2cpp::FiberState::Failed) {
                    Fiber::RecycleForScheduler(std::move(live[index]), 32U);
                    live.erase(live.begin() +
                               static_cast<std::ptrdiff_t>(index));
                } else {
                    (void)fiber->resume_from_scheduler();
                }
            } else if (choice < 72 && !live.empty()) {
                live[rng.Below(live.size())]->RequestCancellation();
            } else if (choice < 84 && !live.empty()) {
                const std::size_t index = rng.Below(live.size());
                Fiber::RecycleForScheduler(std::move(live[index]), 32U);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
            } else if (!live.empty()) {
                // 跨线程恢复：把 Fiber 交给另一个线程跑完。
                const std::size_t index = rng.Below(live.size());
                Fiber* fiber = live[index].get();
                if (fiber->state() == go2cpp::FiberState::Suspended) {
                    bool resumed = false;
                    std::thread worker([fiber, &resumed] {
                        resumed = fiber->resume_from_scheduler();
                    });
                    worker.join();
                    CHECK_TRUE(resumed || fiber->state() ==
                                             go2cpp::FiberState::Completed);
                }
            }
        }
        for (auto& fiber : live) {
            fiber->RequestCancellation();
            while (fiber->state() != go2cpp::FiberState::Completed &&
                   fiber->state() != go2cpp::FiberState::Failed) {
                if (!fiber->resume_from_scheduler()) {
                    break;
                }
            }
            Fiber::RecycleForScheduler(std::move(fiber), 32U);
        }
        std::printf("[asan-random] seed=%llu ops=%zu created=%zu bodies=%zu "
                    "cancelled=%zu\n",
                    (unsigned long long)seed, kOps, created,
                    bodies.load(std::memory_order_relaxed),
                    cancelled.load(std::memory_order_relaxed));
    }
}

/// @brief 深递归 + 完成后再析构。
/// @note ASan 构建按设计禁用可变长栈，因此这里用显式 8MiB 固定栈。
void DeepStackScenario() {
    std::atomic<bool> ok{false};
    constexpr std::size_t kDepth = 3000;
    {
        Fiber deep(
            [&ok] { ok.store(DeepWalk(kDepth) == ExpectedWalk(kDepth)); },
            8U * 1024U * 1024U, 0, false);
        CHECK_TRUE(deep.resume());
        CHECK_TRUE(deep.state() == go2cpp::FiberState::Completed);
    }
    CHECK_TRUE(ok.load());
    std::printf("[asan-deep] depth=%zu ok=%d\n", kDepth, int(ok.load()));
}

/// @brief 4 线程各跑 200 个 Fiber，验证并发创建/回收无 UAF。
void ConcurrentScenario() {
    std::atomic<std::size_t> done{0};
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        workers.emplace_back([&done] {
            for (int i = 0; i < 200; ++i) {
                auto fiber = Fiber::AcquireForScheduler(
                    [&done] {
                        done.fetch_add(1, std::memory_order_relaxed);
                        (void)Fiber::Suspend(SuspendReason::Yield);
                        done.fetch_add(1, std::memory_order_relaxed);
                    },
                    65536, 16U);
                CHECK_TRUE(fiber != nullptr);
                CHECK_TRUE(fiber->resume_from_scheduler());
                CHECK_TRUE(fiber->resume_from_scheduler());
                Fiber::RecycleForScheduler(std::move(fiber), 16U);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK_TRUE(done.load() == 4 * 200 * 2);
    std::printf("[asan-concurrent] done=%zu\n", done.load());
}

}  // namespace

int main() {
    ConcurrentScenario();
    DeepStackScenario();
    RandomWalkScenario();
    std::printf("[asan] failures=%d\n", g_failures.load());
    return g_failures.load() == 0 ? 0 : 1;
}
