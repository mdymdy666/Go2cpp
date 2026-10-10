#include "go2cpp/core/hybrid_mutex.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

/**
 * @brief 演示 HybridGate/HybridMutex 的短临界区用法与策略配置。
 * @details 该门只用于极短的唤醒/撤销临界区；不得在持锁期间做阻塞 IO、
 *          调用用户回调或执行系统调用。
 * @return 所有场景按预期完成时返回 0。
 */
int main() {
    using namespace std::chrono_literals;
    go2cpp::core::HybridMutex gate;

    // 场景一（成功路径）：满足 BasicLockable，可与 std::lock_guard 组合。
    {
        std::lock_guard<go2cpp::core::HybridMutex> lock(gate);
        std::cout << "[hybrid][场景一] lock_guard 获取成功\n";
    }

    // 场景二（并发热路径）：多线程轮流进入短临界区，不丢更新。
    std::atomic<long long> counter{0};
    constexpr int kThreads = 4;
    constexpr int kIterations = 20000;
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
        worker.join();
    }
    const bool counter_ok =
        counter.load(std::memory_order_relaxed) == kThreads * kIterations;
    std::cout << "[hybrid][场景二] counter=" << counter.load()
              << "（预期 " << kThreads * kIterations << "）\n";

    // 场景三（策略配置）：自旋次数为 0 被归一化为 1，不影响所有权语义。
    const auto before = go2cpp::core::HybridMutex::DefaultOptions();
    go2cpp::core::HybridMutex::SetDefaultOptions(
        go2cpp::core::HybridMutexOptions{0});
    const auto normalized = go2cpp::core::HybridMutex::DefaultOptions();
    go2cpp::core::HybridMutex::SetDefaultOptions(before);
    const bool policy_ok = normalized.fast_attempts == 1;
    std::cout << "[hybrid][场景三] fast_attempts(0) -> "
              << normalized.fast_attempts << "（预期 1）\n";

    // 场景四（边界）：使用最小自旋的独立实例仍保持互斥。
    go2cpp::core::HybridMutex tight(go2cpp::core::HybridMutexOptions{1});
    std::atomic<int> tight_counter{0};
    std::vector<std::thread> tight_workers;
    for (int index = 0; index < 2; ++index) {
        tight_workers.emplace_back([&tight, &tight_counter] {
            for (int round = 0; round < 5000; ++round) {
                tight.lock();
                tight_counter.fetch_add(1, std::memory_order_relaxed);
                tight.unlock();
            }
        });
    }
    for (auto& worker : tight_workers) {
        worker.join();
    }
    const bool tight_ok = tight_counter.load(std::memory_order_relaxed) == 10000;

    const bool all = counter_ok && policy_ok && tight_ok;
    std::cout << "[hybrid][汇总] counter=" << counter_ok << " policy=" << policy_ok
              << " tight=" << tight_ok << " all=" << std::boolalpha << all << '\n';
    return all ? 0 : 1;
}
