/**
 * @file futex_mutex_demo.cpp
 * @brief 演示内部唤醒门 `core::FutexMutex`：CAS 快路径 + futex 阻塞慢路径。
 *
 * 这是取代 `HybridGate` 之后的最小示例（X-35）：
 * 1. `std::lock_guard` 作用域用法；
 * 2. 多线程争用同一把门，临界区计数必须精确；
 * 3. **跨线程解锁**：A 线程加锁、B 线程解锁（Go 风格，`std::mutex` 不支持）。
 */

#include "go2cpp/concurrency/core/futex_mutex.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

int main() {
    go2cpp::core::FutexMutex gate;
    long long counter = 0;
    constexpr int kThreads = 4;
    constexpr int kIterations = 10000;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&] {
            for (int round = 0; round < kIterations; ++round) {
                std::lock_guard<go2cpp::core::FutexMutex> lock(gate);
                ++counter;
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const long long expected =
        static_cast<long long>(kThreads) * kIterations;
    std::printf("contended counter=%lld expected=%lld %s\n", counter, expected,
                counter == expected ? "OK" : "MISMATCH");

    // 跨线程解锁：主线程持有，工作线程释放。
    gate.lock();
    std::thread releaser([&] { gate.unlock(); });
    releaser.join();
    const bool reacquired = gate.try_lock();
    if (reacquired) {
        gate.unlock();
    }
    std::printf("cross-thread unlock: %s\n", reacquired ? "OK" : "FAILED");
    return (counter == expected && reacquired) ? 0 : 1;
}
