/**
 * @file test_tsan_smoke.cpp
 * @brief ThreadSanitizer 冒烟测试：锁 / 条件变量 / 通道 / ParkingCondition / Timer。
 *
 * 刻意避开 Scheduler/Fiber（自定义栈与 TSAN 的栈切换不兼容），只使用普通
 * OS 线程 + 原语的线程路径；TimerService 自身只起普通线程，不经过 Fiber
 * 调度器，因此可以纳入。配合 run_tsan.sh 用 `-fsanitize=thread` 构建。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/core/timer.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::MakeChannel;
using go2cpp::core::ParkingCondition;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;

void TestMutex() {
    go2cpp_tests::announce("tsan: mutex 4 threads x 20000");
    constexpr int kThreads = 4;
    constexpr int kOps = 20000;
    Mutex mutex;
    std::int64_t counter = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kOps; ++i) {
                GO2CPP_CHECK(mutex.Lock());
                ++counter;
                mutex.Unlock();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GO2CPP_CHECK(counter == static_cast<std::int64_t>(kThreads) * kOps);
}

void TestChannel() {
    go2cpp_tests::announce("tsan: channel 2 producers x 1000 / 2 consumers");
    constexpr int kProducers = 2;
    constexpr int kPerProducer = 1000;
    auto channel = MakeChannel<int>(64);
    std::atomic<int> received{0};
    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                GO2CPP_CHECK(channel->Send(p * kPerProducer + i).Ok());
            }
        });
    }
    std::thread closer([&] {
        for (auto& thread : threads) {
            thread.join();
        }
        channel->Close();
    });
    std::vector<std::thread> consumers;
    for (int c = 0; c < 2; ++c) {
        consumers.emplace_back([&] {
            for (;;) {
                const auto result = channel->Recv();
                if (!result.Ok()) {
                    return;
                }
                received.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& consumer : consumers) {
        consumer.join();
    }
    closer.join();
    GO2CPP_CHECK(received.load() == kProducers * kPerProducer);
}

void TestConditionVariable() {
    go2cpp_tests::announce("tsan: condition variable 4 waiters + notify_all");
    constexpr int kWaiters = 4;
    Mutex mutex;
    ConditionVariable condition;
    bool ready = false;
    std::atomic<int> woken{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&] {
            GO2CPP_CHECK(mutex.Lock());
            while (!ready) {
                GO2CPP_CHECK(condition.Wait(mutex));
            }
            mutex.Unlock();
            woken.fetch_add(1, std::memory_order_relaxed);
        });
    }
    std::this_thread::sleep_for(20ms);
    GO2CPP_CHECK(mutex.Lock());
    ready = true;
    condition.NotifyAll();
    mutex.Unlock();
    for (auto& waiter : waiters) {
        waiter.join();
    }
    GO2CPP_CHECK(woken.load() == kWaiters);
}

void TestParkingCondition() {
    go2cpp_tests::announce("tsan: parking condition 4 waiters + notify_all");
    constexpr int kWaiters = 4;
    std::mutex mutex;
    ParkingCondition condition;
    bool ready = false;
    std::atomic<int> woken{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&] {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] { return ready; });
            lock.unlock();
            woken.fetch_add(1, std::memory_order_relaxed);
        });
    }
    std::this_thread::sleep_for(20ms);
    {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
    }
    condition.notify_all();
    for (auto& waiter : waiters) {
        waiter.join();
    }
    GO2CPP_CHECK(woken.load() == kWaiters);
}

void TestTimer() {
    go2cpp_tests::announce("tsan: timer 4 threads x 400 schedule/cancel");
    auto& service = go2cpp::core::TimerService::Default();
    constexpr int kThreads = 4;
    constexpr int kOps = 400;
    constexpr int kKept = kOps / 2;
    std::atomic<int> fired{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            std::vector<go2cpp::core::Timer> held;
            held.reserve(kKept);
            for (int i = 0; i < kOps; ++i) {
                auto timer = service.ScheduleAfter(
                    std::chrono::microseconds(50 + (i % 20) * 10), [&fired] {
                        fired.fetch_add(1, std::memory_order_relaxed);
                    });
                if ((i & 1) == 0) {
                    timer.Cancel();
                } else {
                    held.emplace_back(std::move(timer));
                }
            }
            // held 随 lambda 返回析构（等价 Cancel）；已触发的回调不再重入。
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    // TSan 下调度会变慢：给未取消的定时器留出触发窗口，再验证计数稳定。
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fired.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    GO2CPP_CHECK(fired.load(std::memory_order_relaxed) > 0);
    std::this_thread::sleep_for(200ms);
    const int settled = fired.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(100ms);
    GO2CPP_CHECK(fired.load(std::memory_order_relaxed) == settled);
    GO2CPP_CHECK(settled <= kThreads * kKept);
}

}  // namespace

int main() {
    std::printf("[tsan] Go2Cpp sync primitives TSAN smoke\n");
    TestMutex();
    TestChannel();
    TestConditionVariable();
    TestParkingCondition();
    TestTimer();
    if (go2cpp_tests::g_failures.load(std::memory_order_acquire) != 0) {
        std::fprintf(stderr, "[tsan] FAIL: %d checks\n",
                     go2cpp_tests::g_failures.load(std::memory_order_acquire));
        return 1;
    }
    std::printf("[tsan] PASS (5 scenarios)\n");
    return 0;
}
