/**
 * @file fiber_tsan_migrate.cpp
 * @brief Fiber 多线程创建/切换/跨线程迁移的 TSan 压力测试。
 *
 * 独立可执行（不注册 ctest）。目标是「生产路径 0 race」：
 *  1. 4 个 worker 各自创建/恢复/回收 Fiber（TLS FiberBin + 栈缓存）；
 *  2. 主线程创建 Fiber 并挂起后，通过 mutex/condition_variable 交给另一个
 *     worker 恢复；验证同一 Fiber 跨 OS 线程迁移时的状态/元数据同步；
 *  3. 结束时检查所有 Fiber 完成、计数一致。
 *
 * 构建（TSan 下 fiber.cpp 会启用 GO2CPP_FIBER_TSAN 的 fiber 注解）：
 *   g++ -std=c++17 -O1 -g -fsanitize=thread -fno-omit-frame-pointer \
 *       -DGO2CPP_USE_NATIVE_CONTEXT=1 -Iinclude \
 *       tests/concurrency/fiber_tsan_migrate.cpp \
 *       src/concurrency/fiber.cpp src/concurrency/fiber_local.cpp \
 *       src/concurrency/context_backend.cpp src/concurrency/context_x64.S \
 *       -lpthread -o /tmp/fiber_tsan_migrate
 *
 * 运行：
 *   TSAN_OPTIONS=halt_on_error=1:history_size=7 /tmp/fiber_tsan_migrate
 */
#include "go2cpp/fiber.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using go2cpp::Fiber;
using go2cpp::FiberState;
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

/// @brief 4 个 worker 各自做 Fiber 生命周期，共享一个完成计数。
void WorkerChurnScenario() {
    constexpr int kThreads = 4;
    constexpr int kIterations = 300;
    std::atomic<std::size_t> completed{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&completed, index] {
            for (int i = 0; i < kIterations; ++i) {
                auto fiber = Fiber::AcquireForScheduler(
                    [&completed, index, i] {
                        // 每个 Fiber 读写自己的局部变量与共享原子。
                        int local = index * 1000 + i;
                        (void)Fiber::Suspend(SuspendReason::Yield);
                        local += 1;
                        completed.fetch_add(1, std::memory_order_release);
                        (void)local;
                    },
                    4096, 32U);
                CHECK_TRUE(fiber != nullptr);
                CHECK_TRUE(fiber->resume_from_scheduler());
                CHECK_TRUE(fiber->resume_from_scheduler());
                Fiber::RecycleForScheduler(std::move(fiber), 32U);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK_TRUE(completed.load(std::memory_order_acquire) ==
               static_cast<std::size_t>(kThreads) * kIterations);
    std::printf("[tsan-churn] completed=%zu\n",
                completed.load(std::memory_order_acquire));
}

/// @brief 主线程创建并挂起，worker 线程恢复完成后交接回主线程。
void MigrationScenario() {
    struct Handoff {
        std::mutex mutex;
        std::condition_variable condition;
        Fiber* fiber{nullptr};
        bool ready{false};
        std::size_t done{0};
    };
    constexpr int kRounds = 200;
    Handoff handoff;
    std::atomic<bool> stop{false};
    std::thread worker([&handoff, &stop] {
        for (;;) {
            Fiber* fiber = nullptr;
            {
                std::unique_lock<std::mutex> lock(handoff.mutex);
                handoff.condition.wait(lock, [&handoff, &stop] {
                    return handoff.ready || stop.load(std::memory_order_acquire);
                });
                if (!handoff.ready) {
                    return;
                }
                fiber = handoff.fiber;
                handoff.ready = false;
            }
            CHECK_TRUE(fiber != nullptr);
            CHECK_TRUE(fiber->resume_from_scheduler());
            CHECK_TRUE(fiber->state() == FiberState::Suspended ||
                       fiber->state() == FiberState::Completed);
            {
                std::lock_guard<std::mutex> lock(handoff.mutex);
                ++handoff.done;
            }
            handoff.condition.notify_all();
        }
    });

    std::size_t completed = 0;
    for (int round = 0; round < kRounds; ++round) {
        auto fiber = Fiber::AcquireForScheduler(
            [] {
                (void)Fiber::Suspend(SuspendReason::Park);
                (void)Fiber::Suspend(SuspendReason::Park);
            },
            4096, 32U);
        CHECK_TRUE(fiber != nullptr);
        CHECK_TRUE(fiber->resume_from_scheduler());
        {
            std::lock_guard<std::mutex> lock(handoff.mutex);
            handoff.fiber = fiber.get();
            handoff.ready = true;
        }
        handoff.condition.notify_one();
        {
            std::unique_lock<std::mutex> lock(handoff.mutex);
            handoff.condition.wait(lock, [&handoff, round] {
                return handoff.done == static_cast<std::size_t>(round) + 1;
            });
        }
        // worker 已把 Fiber 恢复到下一次 Suspend 或完成。
        CHECK_TRUE(fiber->state() == FiberState::Suspended ||
                   fiber->state() == FiberState::Completed);
        if (fiber->state() == FiberState::Suspended) {
            CHECK_TRUE(fiber->resume_from_scheduler());
        }
        CHECK_TRUE(fiber->state() == FiberState::Completed);
        Fiber::RecycleForScheduler(std::move(fiber), 32U);
        ++completed;
    }
    stop.store(true, std::memory_order_release);
    handoff.condition.notify_all();
    worker.join();
    CHECK_TRUE(completed == kRounds);
    std::printf("[tsan-migrate] rounds=%zu\n", completed);
}

}  // namespace

int main() {
    WorkerChurnScenario();
    MigrationScenario();
    std::printf("[tsan] failures=%d\n", g_failures.load());
    return g_failures.load() == 0 ? 0 : 1;
}
