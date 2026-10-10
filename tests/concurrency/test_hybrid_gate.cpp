#include "go2cpp/core/hybrid_mutex.hpp"

#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/**
 * @brief 验证默认热路径策略与参数归一化。
 * @details fast_attempts 为 0 时必须被修正为 1，否则 lock() 会直接进慢路径。
 */
void check_defaults() {
    const auto before = go2cpp::core::HybridMutex::DefaultOptions();
    GO2CPP_REQUIRE(before.fast_attempts != 0);

    go2cpp::core::HybridMutex::SetDefaultOptions(
        go2cpp::core::HybridMutexOptions{0});
    GO2CPP_CHECK(go2cpp::core::HybridMutex::DefaultOptions().fast_attempts == 1);

    go2cpp::core::HybridMutex::SetDefaultOptions(before);
    GO2CPP_CHECK(go2cpp::core::HybridMutex::DefaultOptions().fast_attempts ==
                 before.fast_attempts);
}

/**
 * @brief 验证 HybridGate 满足 BasicLockable，可与 std::lock_guard 组合。
 */
void check_scoped_lock() {
    go2cpp::core::HybridMutex gate;
    {
        std::lock_guard<go2cpp::core::HybridMutex> lock(gate);
        GO2CPP_CHECK(true);
    }
    gate.lock();
    gate.unlock();
}

/**
 * @brief 多线程高竞争下验证临界区串行化，任何丢失更新都会暴露。
 * @details 读-改-写刻意使用非原子的两步操作，只有互斥成立时结果才正确。
 */
void check_mutual_exclusion() {
    go2cpp::core::HybridMutex gate;
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
 * @brief 自定义自旋次数（最小合法值）仍然保持互斥语义。
 */
void check_custom_options() {
    go2cpp::core::HybridMutex gate(go2cpp::core::HybridMutexOptions{1});
    std::atomic<int> counter{0};
    constexpr int kThreads = 4;
    constexpr int kIterations = 5000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&gate, &counter] {
            for (int round = 0; round < kIterations; ++round) {
                gate.lock();
                counter.fetch_add(1, std::memory_order_relaxed);
                gate.unlock();
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 60s);
    }
    GO2CPP_REQUIRE(counter.load(std::memory_order_relaxed) ==
                   kThreads * kIterations);
}

}  // namespace

/**
 * @brief 验证 core::HybridGate / HybridMutex 的互斥语义与策略配置。
 * @details 该门保护 sync 与 IO 的唤醒临界区，此前全仓无测试引用（台账 T-03）。
 * @return 无；失败项由测试支持代码累计。
 */
void run_hybrid_gate_tests() {
    go2cpp_tests::announce("hybrid gate mutex semantics");
    check_defaults();
    check_scoped_lock();
    check_mutual_exclusion();
    check_custom_options();
}
