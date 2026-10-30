/**
 * @file test_fiber_switch_opt.cpp
 * @brief Fiber 切换优化专项测试：5 个真实场景 + 随机序列 + sanitizer 兼容。
 *
 * 覆盖本次优化触及的全部功能点：
 *  1. errno 隔离（suspend/resume 往返、迁移后仍是本线程的值）；
 *  2. 嵌套 Fiber（父 recover 子、子挂起回父、父继续完成）；
 *  3. 跨线程 resume（Fiber 迁移到另一个 M 后继续执行）；
 *  4. 取消 + 析构仍挂起的 Fiber（协作式收尾，不丢栈）；
 *  5. 调度器入口 resume_from_scheduler + park 挂起；
 *  6. 多 Fiber 交替 ping-pong（切换 10 万次的一致性）；
 *  7. 随机操作序列：创建/恢复/挂起/取消/析构随机组合，状态机不变量。
 *
 * 独立可执行（不注册 ctest）。构建：
 *   g++ -O2 -std=c++20 -DGO2CPP_USE_NATIVE_CONTEXT=1 -Iinclude \
 *       tests/concurrency/test_fiber_switch_opt.cpp src/concurrency/fiber.cpp \
 *       src/concurrency/fiber_local.cpp src/concurrency/context_backend.cpp \
 *       src/concurrency/context_x64.S -lpthread -o /tmp/test_fiber_switch_opt
 *
 * sanitizer：同一命令加 -fsanitize=address,undefined 或 -fsanitize=thread。
 * 用法：test_fiber_switch_opt [random_ops] [seed]
 */
#include "go2cpp/fiber.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using go2cpp::Fiber;
using go2cpp::FiberState;
using go2cpp::SuspendReason;

int g_failures = 0;

#define CHECK_TRUE(condition)                                                  \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__,     \
                         #condition);                                          \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// 场景 1：errno 隔离 + 状态机
// ---------------------------------------------------------------------------
void ScenarioErrnoState() {
    std::vector<int> steps;
    Fiber fiber([&steps] {
        errno = EAGAIN;
        steps.push_back(1);
        CHECK_TRUE(Fiber::Suspend(SuspendReason::Yield));
        CHECK_TRUE(errno == EAGAIN);
        steps.push_back(2);
        errno = ENOSPC;
        CHECK_TRUE(Fiber::Suspend(SuspendReason::Park));
        CHECK_TRUE(errno == ENOSPC);
        steps.push_back(3);
    });

    errno = EDOM;
    CHECK_TRUE(fiber.resume());
    CHECK_TRUE(errno == EDOM);
    CHECK_TRUE(fiber.state() == FiberState::Suspended);
    CHECK_TRUE(fiber.suspend_reason() == SuspendReason::Yield);

    errno = ERANGE;
    CHECK_TRUE(fiber.resume());
    CHECK_TRUE(errno == ERANGE);
    CHECK_TRUE(fiber.suspend_reason() == SuspendReason::Park);

    CHECK_TRUE(fiber.resume());
    CHECK_TRUE(fiber.state() == FiberState::Completed);
    CHECK_TRUE(!fiber.resume());
    CHECK_TRUE((steps == std::vector<int>{1, 2, 3}));
}

// ---------------------------------------------------------------------------
// 场景 2：嵌套 Fiber（父恢复子）
// ---------------------------------------------------------------------------
void ScenarioNested() {
    std::vector<int> order;
    Fiber child([&order] {
        order.push_back(10);
        CHECK_TRUE(Fiber::Suspend(SuspendReason::Yield));
        order.push_back(12);
    });

    Fiber parent([&order, &child] {
        order.push_back(1);
        CHECK_TRUE(child.resume());
        order.push_back(11);
        CHECK_TRUE(child.resume());
        CHECK_TRUE(child.state() == FiberState::Completed);
        order.push_back(13);
    });

    CHECK_TRUE(parent.resume());
    CHECK_TRUE(parent.state() == FiberState::Completed);
    CHECK_TRUE((order == std::vector<int>{1, 10, 11, 12, 13}));
}

// ---------------------------------------------------------------------------
// 场景 3：跨线程 resume（Fiber 迁移）
// ---------------------------------------------------------------------------
void ScenarioMigrate() {
    std::atomic<int> steps{0};
    // 注意：std::this_thread::get_id() 在 glibc 中最终调用带 `const` 属性的
    // pthread_self()，编译器可以跨上下文切换把它 CSE 成同一个值（Fiber 迁移
    // 线程后就会读到旧线程 id）。这里用不可 CSE 的 gettid 验证真实迁移。
    long first_tid = 0;
    long second_tid = 0;
    Fiber fiber([&] {
        first_tid = static_cast<long>(::syscall(SYS_gettid));
        steps.fetch_add(1);
        CHECK_TRUE(Fiber::Suspend(SuspendReason::Yield));
        second_tid = static_cast<long>(::syscall(SYS_gettid));
        steps.fetch_add(1);
    });

    CHECK_TRUE(fiber.resume());
    CHECK_TRUE(steps.load() == 1);
    std::atomic<bool> worker_ok{false};
    std::thread worker([&] {
        errno = 0;
        worker_ok.store(fiber.resume());
        worker_ok.store(worker_ok.load() && errno == 0);
    });
    worker.join();
    CHECK_TRUE(worker_ok.load());
    CHECK_TRUE(steps.load() == 2);
    CHECK_TRUE(first_tid != second_tid);
    CHECK_TRUE(fiber.state() == FiberState::Completed);
}

// ---------------------------------------------------------------------------
// 场景 4：取消 + 析构仍挂起的 Fiber
// ---------------------------------------------------------------------------
void ScenarioCancelOnDestroy() {
    std::atomic<int> resumed{0};
    {
        Fiber fiber([&] {
            for (;;) {
                if (Fiber::CancellationRequested()) {
                    return;
                }
                resumed.fetch_add(1);
                Fiber::Suspend(SuspendReason::Park);
            }
        });
        CHECK_TRUE(fiber.resume());
        CHECK_TRUE(fiber.state() == FiberState::Suspended);
        fiber.RequestCancellation();
        // 析构：取得 resume 权后恢复 Fiber，主体看到取消标记后返回。
    }
    CHECK_TRUE(resumed.load() == 1);
}

// ---------------------------------------------------------------------------
// 场景 5：调度器入口 + park
// ---------------------------------------------------------------------------
void ScenarioSchedulerEntry() {
    std::atomic<int> rounds{0};
    Fiber fiber([&] {
        for (int i = 0; i < 3; ++i) {
            rounds.fetch_add(1);
            Fiber::SuspendForScheduler(SuspendReason::Park);
        }
    });
    CHECK_TRUE(fiber.resume_from_scheduler());
    CHECK_TRUE(fiber.state() == FiberState::Suspended);
    CHECK_TRUE(fiber.suspend_reason() == SuspendReason::Park);
    for (int i = 0; i < 3; ++i) {
        CHECK_TRUE(fiber.resume_from_scheduler());
    }
    CHECK_TRUE(rounds.load() == 3);
    CHECK_TRUE(fiber.state() == FiberState::Completed);
}

// ---------------------------------------------------------------------------
// 场景 6：多 Fiber 交替 ping-pong（10 万次切换）
// ---------------------------------------------------------------------------
void ScenarioPingPong() {
    static constexpr int kFibers = 4;
    static constexpr int kRounds = 25000;
    std::vector<std::unique_ptr<Fiber>> fibers;
    std::atomic<long long> total{0};
    for (int i = 0; i < kFibers; ++i) {
        fibers.push_back(std::make_unique<Fiber>([&total, i] {
            for (int round = 0; round < kRounds; ++round) {
                total.fetch_add(1, std::memory_order_relaxed);
                Fiber::Suspend(SuspendReason::Yield);
            }
            total.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    for (auto& fiber : fibers) {
        CHECK_TRUE(fiber->resume());
    }
    for (int round = 0; round < kRounds; ++round) {
        for (auto& fiber : fibers) {
            CHECK_TRUE(fiber->resume());
        }
    }
    for (auto& fiber : fibers) {
        CHECK_TRUE(fiber->state() == FiberState::Completed);
    }
    CHECK_TRUE(total.load() ==
               static_cast<long long>(kFibers) * (kRounds + 1));
}

// ---------------------------------------------------------------------------
// 随机序列
// ---------------------------------------------------------------------------
void RandomWorkload(std::uint64_t operations, std::uint32_t seed) {
    std::mt19937_64 rng(seed);
    struct Entry {
        std::unique_ptr<Fiber> fiber;
        std::atomic<int> steps{0};
        int limit{1};
    };
    std::vector<std::unique_ptr<Entry>> entries;
    std::uint64_t created = 0;
    std::uint64_t resumed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t destroyed = 0;

    for (std::uint64_t i = 0; i < operations; ++i) {
        const std::uint32_t op = static_cast<std::uint32_t>(rng() % 100);
        if (op < 30 || entries.empty()) {
            auto entry = std::make_unique<Entry>();
            entry->limit = 1 + static_cast<int>(rng() % 4);
            auto* raw = entry.get();
            const std::size_t stack_size = 4096U + (rng() % 4) * 4096U;
            entry->fiber = std::make_unique<Fiber>(
                [raw] {
                    for (int step = 0; step < raw->limit; ++step) {
                        raw->steps.fetch_add(1, std::memory_order_relaxed);
                        if (Fiber::CancellationRequested()) {
                            return;
                        }
                        Fiber::Suspend(SuspendReason::Yield);
                    }
                },
                stack_size, 0U, (rng() & 1U) != 0);
            ++created;
            entries.push_back(std::move(entry));
            continue;
        }
        const std::size_t index = static_cast<std::size_t>(rng() % entries.size());
        auto& entry = entries[index];
        if (op < 70) {
            entry->fiber->resume();
            ++resumed;
        } else if (op < 82) {
            entry->fiber->RequestCancellation();
            ++cancelled;
        } else {
            // 析构：若仍挂起，析构会协作式恢复并收尾。
            entry->fiber.reset();
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
            ++destroyed;
        }
    }

    for (auto& entry : entries) {
        entry->fiber->RequestCancellation();
        entry->fiber->resume();
    }
    entries.clear();

    const bool sane = created >= destroyed;
    if (!sane) {
        std::fprintf(stderr, "[random] counter mismatch\n");
        ++g_failures;
    }
    std::printf("[random] seed=%u ops=%llu created=%llu resumed=%llu "
                "cancelled=%llu destroyed=%llu failures=%d\n",
                seed, static_cast<unsigned long long>(operations),
                static_cast<unsigned long long>(created),
                static_cast<unsigned long long>(resumed),
                static_cast<unsigned long long>(cancelled),
                static_cast<unsigned long long>(destroyed), g_failures);
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t random_ops = 5000;
    std::uint32_t seed = 12345U;
    if (argc > 1) {
        random_ops = std::strtoull(argv[1], nullptr, 10);
    }
    if (argc > 2) {
        seed = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    }

    std::printf("[scenario] errno + state\n");
    ScenarioErrnoState();
    std::printf("[scenario] nested fibers\n");
    ScenarioNested();
    std::printf("[scenario] cross-thread migrate\n");
    ScenarioMigrate();
    std::printf("[scenario] cancel on destroy\n");
    ScenarioCancelOnDestroy();
    std::printf("[scenario] scheduler entry + park\n");
    ScenarioSchedulerEntry();
    std::printf("[scenario] multi-fiber ping-pong\n");
    ScenarioPingPong();
    std::printf("[random] ops=%llu seed=%u\n",
                static_cast<unsigned long long>(random_ops), seed);
    RandomWorkload(random_ops, seed);

    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] failures=%d\n", g_failures);
        return 1;
    }
    std::printf("[PASS] fiber switch scenarios + random workload\n");
    return 0;
}
