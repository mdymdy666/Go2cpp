/**
 * @file test_tsan_smoke.cpp
 * @brief ThreadSanitizer 冒烟测试：锁 / 条件变量 / 通道 / ParkingCondition。
 *
 * 刻意避开 Scheduler/Fiber（自定义栈与 TSAN 的栈切换不兼容），只使用普通
 * OS 线程 + 原语的线程路径。配合 run_tsan.sh 用 `-fsanitize=thread` 构建。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
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

}  // namespace

int main() {
    std::printf("[tsan] Go2Cpp sync primitives TSAN smoke\n");
    TestMutex();
    TestChannel();
    TestConditionVariable();
    TestParkingCondition();
    if (go2cpp_tests::g_failures.load(std::memory_order_acquire) != 0) {
        std::fprintf(stderr, "[tsan] FAIL: %d checks\n",
                     go2cpp_tests::g_failures.load(std::memory_order_acquire));
        return 1;
    }
    std::printf("[tsan] PASS (4 scenarios)\n");
    return 0;
}
