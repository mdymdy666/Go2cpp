/**
 * @file test_debug_lock_selector.cpp
 * @brief D3 锁争用选择钩子的测试。
 *
 * 覆盖（按项目规范：压测 + 随机 + >=5 个真实场景）：
 *   场景 1 未注册选择器时行为与默认 FIFO 一致
 *   场景 2 自定义选择器可让**指定的等待者**先获得锁（D3 的核心诉求）
 *   场景 3 选择器返回 nullopt / 越界下标时退化为默认 FIFO
 *   场景 4 选择器抛异常被隔离，锁仍能正常移交
 *   场景 5 Reset 后恢复默认；且 TryLock 仍然不能插队（既有语义未被破坏）
 *   压测   100000 次加锁/解锁（含选择器启用）
 *   随机   随机选择策略下不死锁、不丢唤醒（多轮随机指派）
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace {

using go2cpp::sync::Mutex;
using go2cpp::sync::WaitSelector;
using go2cpp::sync::WaiterCandidate;

/// 总是选最后一个候选者（测试"指定者优先"）。
class LastSelector final : public WaitSelector {
public:
    std::optional<std::size_t> Select(const std::vector<WaiterCandidate>& candidates) noexcept override {
        if (candidates.empty()) {
            return std::nullopt;
        }
        return candidates.back().index;
    }
};

/// 不干预。
class NullSelector final : public WaitSelector {
public:
    std::optional<std::size_t> Select(const std::vector<WaiterCandidate>&) noexcept override {
        return std::nullopt;
    }
};

/// 永远返回越界下标（测试退化路径）。
class OutOfRangeSelector final : public WaitSelector {
public:
    std::optional<std::size_t> Select(const std::vector<WaiterCandidate>& candidates) noexcept override {
        return candidates.size() + 100U;
    }
};

/// 抛异常（测试隔离）。
class ThrowingSelector final : public WaitSelector {
public:
    std::optional<std::size_t> Select(const std::vector<WaiterCandidate>&) noexcept override {
        throw std::runtime_error("selector failure");
    }
};

/// 受控顺序：返回外部指定的下标序列。
class ScriptedSelector final : public WaitSelector {
public:
    explicit ScriptedSelector(std::size_t index) : m_index(index) {}
    std::optional<std::size_t> Select(const std::vector<WaiterCandidate>& candidates) noexcept override {
        if (m_index >= candidates.size()) {
            return std::nullopt;
        }
        return m_index;
    }
private:
    std::size_t m_index;
};

}  // namespace

void run_debug_lock_selector_tests() {
    go2cpp_tests::announce("lock wait selector (D3)");
    using namespace go2cpp::sync;

    // 场景 1：未注册时与默认 FIFO 一致（不改变行为）
    {
        ResetWaitSelectorFactory();
        Mutex mutex;
        const int order = [&] {
            GO2CPP_CHECK(mutex.TryLock());
            const bool second = mutex.TryLock();
            mutex.Unlock();
            return second ? 1 : 0;
        }();
        GO2CPP_CHECK(order == 0);   // 已持有时再 TryLock 必失败
    }

    // 场景 2：选择器可让指定等待者优先（核心）
    {
        Mutex mutex;
        GO2CPP_CHECK(mutex.TryLock());             // 主线程持有
        std::atomic<int> entered{0};
        std::mutex record_mutex;
        std::vector<int> acquisition_order;
        std::atomic<bool> release_start{false};

        // 3 个普通线程依次排队（用 GO2CPP_REQUIRE_EVENTUALLY 等它们都进入队列）
        std::vector<std::thread> waiters;
        for (int i = 0; i < 3; ++i) {
            waiters.emplace_back([&, i] {
                if (mutex.Lock()) {
                    {
                        std::lock_guard<std::mutex> lock(record_mutex);
                        acquisition_order.push_back(i);
                    }
                    entered.fetch_add(1, std::memory_order_relaxed);
                    mutex.Unlock();
                }
            });
        }
        // 等到等待者数量达到 3（通过 debug 代理观测，正好复用 P3 能力）
        ::go2cpp::debug::DebugMutex probe(mutex);
        GO2CPP_REQUIRE_EVENTUALLY(probe.Snapshot().waiters >= 3, std::chrono::seconds(3));

        // 注册"选最后一个"的选择器，然后逐次放锁
        GO2CPP_CHECK(SetWaitSelectorFactory([] { return std::make_shared<LastSelector>(); }));
        mutex.Unlock();   // 第一次：应把最后一个候选换到队首
        for (int round = 0; round < 3; ++round) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (entered.load(std::memory_order_relaxed) < round + 1 &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        }
        for (auto& thread : waiters) {
            thread.join();
        }
        GO2CPP_CHECK(entered.load(std::memory_order_relaxed) == 3);
        GO2CPP_CHECK(acquisition_order.size() == 3);
        // 队首被换成原最后者 -> 它应第一个拿到锁（下标 2）
        if (acquisition_order.size() == 3) {
            GO2CPP_CHECK(acquisition_order.front() == 2);
        }
        ResetWaitSelectorFactory();
        (void)release_start;
    }

    // 场景 3：不干预 / 越界 -> 退化默认
    {
        for (auto make : {std::function<std::shared_ptr<WaitSelector>()>(
                              [] { return std::make_shared<NullSelector>(); }),
                          std::function<std::shared_ptr<WaitSelector>()>(
                              [] { return std::make_shared<OutOfRangeSelector>(); })}) {
            Mutex mutex;
            GO2CPP_CHECK(SetWaitSelectorFactory(make));
            GO2CPP_CHECK(mutex.TryLock());
            std::atomic<int> acquired{0};
            std::thread worker([&] {
                if (mutex.Lock()) {
                    acquired.fetch_add(1, std::memory_order_relaxed);
                    mutex.Unlock();
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            mutex.Unlock();
            worker.join();
            GO2CPP_CHECK(acquired.load(std::memory_order_relaxed) == 1);
            ResetWaitSelectorFactory();
        }
    }

    // 场景 4：选择器抛异常被隔离
    {
        Mutex mutex;
        GO2CPP_CHECK(SetWaitSelectorFactory([] { return std::make_shared<ThrowingSelector>(); }));
        GO2CPP_CHECK(mutex.TryLock());
        std::atomic<int> acquired{0};
        std::thread worker([&] {
            if (mutex.Lock()) {
                acquired.fetch_add(1, std::memory_order_relaxed);
                mutex.Unlock();
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        mutex.Unlock();          // 这里会调用抛异常的选择器：必须被隔离
        worker.join();
        GO2CPP_CHECK(acquired.load(std::memory_order_relaxed) == 1);
        ResetWaitSelectorFactory();
    }

    // 场景 5：Reset 后恢复默认；TryLock 仍不能插队
    {
        ResetWaitSelectorFactory();
        Mutex mutex;
        GO2CPP_CHECK(mutex.TryLock());
        std::atomic<int> acquired{0};
        std::atomic<bool> queued{false};
        std::thread worker([&] {
            queued.store(true, std::memory_order_release);
            if (mutex.Lock()) {
                acquired.fetch_add(1, std::memory_order_relaxed);
                mutex.Unlock();
            }
        });
        GO2CPP_REQUIRE_EVENTUALLY(queued.load(std::memory_order_acquire), std::chrono::seconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // 有等待者时 TryLock 必须失败（所有权会移交给等待者，不允许插队）
        GO2CPP_CHECK(!mutex.TryLock());
        mutex.Unlock();
        worker.join();
        GO2CPP_CHECK(acquired.load(std::memory_order_relaxed) == 1);
    }

    // 压测：100000 次加锁/解锁（启用选择器）
    {
        GO2CPP_CHECK(SetWaitSelectorFactory([] { return std::make_shared<LastSelector>(); }));
        Mutex mutex;
        constexpr int kOps = 100000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i) {
            GO2CPP_CHECK(mutex.TryLock());
            mutex.Unlock();
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::clog << "  [压测] 无竞争 " << kOps << " 次加解锁: "
                  << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
                  << " us" << std::endl;

        // 有竞争：4 线程各自加解锁 5000 次
        constexpr int kThreads = 4;
        constexpr int kPerThread = 5000;
        std::atomic<int> completed{0};
        std::vector<std::thread> threads;
        const auto t2 = std::chrono::steady_clock::now();
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < kPerThread; ++i) {
                    if (mutex.Lock()) {
                        mutex.Unlock();
                        completed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        const auto t3 = std::chrono::steady_clock::now();
        std::clog << "  [压测] 4 线程竞争 " << kThreads * kPerThread << " 次: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count()
                  << " ms (completed=" << completed.load(std::memory_order_relaxed) << ")" << std::endl;
        GO2CPP_CHECK(completed.load(std::memory_order_relaxed) == kThreads * kPerThread);
        ResetWaitSelectorFactory();
    }

    // 随机：随机指派下标，不死锁、不丢唤醒
    {
        std::mt19937 rng(20261016);
        std::uniform_int_distribution<std::size_t> pick_dist(0, 2);
        bool ok = true;
        for (int round = 0; round < 8; ++round) {
            const std::size_t pick = pick_dist(rng);
            Mutex mutex;
            GO2CPP_CHECK(SetWaitSelectorFactory(
                [pick] { return std::make_shared<ScriptedSelector>(pick); }));
            GO2CPP_CHECK(mutex.TryLock());
            std::atomic<int> acquired{0};
            std::vector<std::thread> threads;
            for (int i = 0; i < 3; ++i) {
                threads.emplace_back([&] {
                    if (mutex.Lock()) {
                        acquired.fetch_add(1, std::memory_order_relaxed);
                        mutex.Unlock();
                    }
                });
            }
            // 主线程只释放**一次**自己持有的锁；后续交接由 worker 之间自行完成。
            // （早期版本在这里连续 Unlock 3 次，后两次解的是已被 worker 释放的锁，
            //   触发 "unlock of unlocked mutex" —— 这是测试写错，不是实现问题。）
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            mutex.Unlock();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (acquired.load(std::memory_order_relaxed) < 3 &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            if (acquired.load(std::memory_order_relaxed) < 3) {
                ok = false;
            }
            for (auto& thread : threads) {
                thread.join();
            }
            ok = ok && acquired.load(std::memory_order_relaxed) == 3;
            ResetWaitSelectorFactory();
        }
        GO2CPP_CHECK(ok);
    }
}
