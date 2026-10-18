/**
 * @file test_futex_mutex.cpp
 * @brief 验证内部唤醒门 `core::FutexMutex`（X-35 后取代 HybridGate）。
 *
 * 覆盖点：
 * 1. BasicLockable：可与 `std::lock_guard` / `std::unique_lock` 组合；
 * 2. 高竞争互斥：8 线程 × 20000 次读改写，丢失更新即失败；
 * 3. `try_lock` 语义：仅在完全空闲时成功；
 * 4. **跨线程解锁**：这是它取代 `std::mutex` 的唯一理由（hook 会把
 *    `lock/unlock` 分派到不同线程）。
 *
 * 不测自旋策略（该实现没有自旋）：竞争时直接 futex 睡眠。
 */

#include "go2cpp/concurrency/core/futex_mutex.hpp"

#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/**
 * @brief 验证满足 BasicLockable，可与作用域锁组合。
 */
void check_scoped_lock() {
    go2cpp::core::FutexMutex gate;
    {
        std::lock_guard<go2cpp::core::FutexMutex> lock(gate);
        GO2CPP_CHECK(true);
    }
    {
        std::unique_lock<go2cpp::core::FutexMutex> lock(gate);
        GO2CPP_CHECK(lock.owns_lock());
    }
    gate.lock();
    gate.unlock();
}

/**
 * @brief 多线程高竞争下验证临界区串行化。
 * @details 读-改-写刻意使用非原子的两步操作，只有互斥成立时结果才正确。
 */
void check_mutual_exclusion() {
    go2cpp::core::FutexMutex gate;
    std::atomic<long long> counter{0};
    constexpr int kThreads = 8;
    constexpr int kIterations = 20000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&gate, &counter] {
            for (int round = 0; round < kIterations; ++round) {
                gate.lock();
                counter.store(counter.load(std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
                gate.unlock();
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 60s);
    }
    GO2CPP_REQUIRE(counter.load(std::memory_order_relaxed) ==
                   static_cast<long long>(kThreads) * kIterations);
}

/**
 * @brief 验证 try_lock 只在门完全空闲时成功。
 */
void check_try_lock() {
    go2cpp::core::FutexMutex gate;
    GO2CPP_CHECK(gate.try_lock());
    GO2CPP_CHECK(!gate.try_lock());
    gate.unlock();
    GO2CPP_CHECK(gate.try_lock());
    gate.unlock();
}

/**
 * @brief 验证跨线程解锁：持有在 A 线程，释放在 B 线程。
 * @details 这正是 `std::mutex` 无法满足、而唤醒门必须支持的场景。
 */
void check_cross_thread_unlock() {
    go2cpp::core::FutexMutex gate;
    gate.lock();
    std::atomic<bool> other_thread_saw_lock{false};
    std::thread other([&] {
        other_thread_saw_lock.store(!gate.try_lock(),
                                    std::memory_order_release);
        gate.unlock();  // 跨线程释放
    });
    GO2CPP_JOIN_WITH_WATCHDOG(other, 60s);
    GO2CPP_REQUIRE(other_thread_saw_lock.load(std::memory_order_acquire));
    // 释放后必须能立即重新取得，证明 unlock 真正生效。
    GO2CPP_REQUIRE(gate.try_lock());
    gate.unlock();
}

}  // namespace

/**
 * @brief 运行 FutexMutex 的全部用例。
 * @return 无；失败项由测试支持代码累计。
 */
void run_futex_mutex_tests() {
    go2cpp_tests::announce("futex mutex semantics");
    check_scoped_lock();
    check_mutual_exclusion();
    check_try_lock();
    check_cross_thread_unlock();
}
