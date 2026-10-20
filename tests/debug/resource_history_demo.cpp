/**
 * @file resource_history_demo.cpp
 * @brief 资源级历史 + 栈捕获 + Fiber 链 demo（GO2CPP_DEBUG_DUMP=ON）。
 *
 * 用法：
 * ```text
 * resource_history_demo [socket_path] [seconds]
 * ```
 *
 * 启动后在 stdout 打印：
 * ```text
 * socket=<path>
 * pid=<pid>
 * resource mutex=<id>
 * resource rwmutex=<id>
 * resource recursive=<id>
 * resource parking_condition=<id>
 * resource channel=<id>
 * ready=true
 * ```
 * 随后持续制造 5 类原语事件，供外部 CLI 查询：
 * `go2cpp-cli dump --pid <pid> --view history --resource <id>`。
 *
 * 覆盖场景：
 * 1. 两个 OS 线程争用 `sync::Mutex`（阻塞/唤醒 + 线程栈）；
 * 2. 两个读者 + 一个写者争用 `sync::RWMutex`；
 * 3. 同一线程重入 `sync::RecursiveMutex`；
 * 4. 受管 Fiber 在 `ParkingCondition` 上等待、主线程 notify；
 * 5. 无缓冲 `Channel<int>` 的发送/接收 rendezvous（Fiber 上下文）。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/recursive_mutex.hpp"
#include "go2cpp/concurrency/rwmutex.hpp"
#include "go2cpp/debug.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int /*signal*/) { g_stop.store(true, std::memory_order_relaxed); }

void SleepMicros(int micros) {
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
}

}  // namespace

int main(int argc, char** argv) {
    // 空路径 = DebugServer::DefaultPath()（/tmp/go2cpp-<pid>.sock），
    // CLI 的 `dump --pid` 正是按这个路径发现目标进程。
    const std::string socket_path = argc > 1 ? argv[1] : "";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 20;

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.min_workers = 2;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    go2cpp::debug::DebugServer& server = go2cpp::debug::DebugServer::Instance();
    go2cpp::debug::RegisterScheduler("scheduler", scheduler);
    if (!server.Start(socket_path)) {
        std::fprintf(stderr, "DebugServer start failed: %s\n",
                     server.last_error().c_str());
        scheduler.shutdown();
        return 1;
    }

    go2cpp::sync::Mutex mutex;
    go2cpp::sync::RWMutex rwmutex;
    go2cpp::sync::RecursiveMutex recursive_mutex;
    go2cpp::core::ParkingCondition condition;
    std::mutex condition_guard;
    bool condition_flag = false;
    auto channel = go2cpp::Channel<int>::Create(0);

    const auto resource_of = [](const void* address) {
        return go2cpp::debug::FindResourceByAddress(address);
    };

    std::printf("socket=%s\npid=%d\n", server.path().c_str(), ::getpid());
    std::printf("resource mutex=%llu\n",
                static_cast<unsigned long long>(resource_of(&mutex)));
    std::printf("resource rwmutex=%llu\n",
                static_cast<unsigned long long>(resource_of(&rwmutex)));
    std::printf("resource recursive=%llu\n",
                static_cast<unsigned long long>(
                    resource_of(&recursive_mutex)));
    std::printf("resource parking_condition=%llu\n",
                static_cast<unsigned long long>(resource_of(&condition)));
    std::printf("resource channel=%llu\n",
                static_cast<unsigned long long>(resource_of(channel.get())));
    std::printf("ready=true\n");
    std::fflush(stdout);

    // 1) Mutex 争用：两个 OS 线程。
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&mutex] {
            while (!g_stop.load(std::memory_order_relaxed)) {
                (void)mutex.Lock();
                SleepMicros(5000);
                mutex.Unlock();
            }
        });
    }

    // 2) RWMutex：两个读者 + 一个写者。
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&rwmutex] {
            while (!g_stop.load(std::memory_order_relaxed)) {
                rwmutex.RLock();
                SleepMicros(5000);
                rwmutex.RUnlock();
            }
        });
    }
    threads.emplace_back([&rwmutex] {
        while (!g_stop.load(std::memory_order_relaxed)) {
            rwmutex.Lock();
            SleepMicros(8000);
            rwmutex.Unlock();
        }
    });

    // 3) RecursiveMutex：同一线程重入 3 层。
    threads.emplace_back([&recursive_mutex] {
        while (!g_stop.load(std::memory_order_relaxed)) {
            recursive_mutex.Lock();
            recursive_mutex.Lock();
            recursive_mutex.Lock();
            recursive_mutex.Unlock();
            recursive_mutex.Unlock();
            recursive_mutex.Unlock();
            SleepMicros(5000);
        }
    });

    // 4) ParkingCondition：受管 Fiber 等待，主线程通知。
    auto waiter = scheduler.spawn([&condition, &condition_guard,
                                   &condition_flag] {
        while (!g_stop.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lock(condition_guard);
            condition.wait(lock, [&condition_flag] { return condition_flag; });
            condition_flag = false;
        }
    });

    // 5) 无缓冲 channel：接收 Fiber + 发送 Fiber 的 rendezvous。
    auto receiver = scheduler.spawn([channel] {
        while (!g_stop.load(std::memory_order_relaxed)) {
            auto result = channel->Recv();
            if (result.status != go2cpp::ChannelStatus::kReady) {
                break;
            }
            SleepMicros(2000);
        }
    });
    auto sender = scheduler.spawn([channel] {
        int value = 0;
        while (!g_stop.load(std::memory_order_relaxed)) {
            if (channel->Send(++value).status !=
                go2cpp::ChannelStatus::kReady) {
                break;
            }
            SleepMicros(2000);
        }
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!g_stop.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(condition_guard);
            condition_flag = true;
        }
        condition.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    g_stop.store(true, std::memory_order_relaxed);
    for (auto& thread : threads) {
        thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(condition_guard);
        condition_flag = true;
    }
    condition.notify_all();
    (void)waiter;
    (void)receiver;
    (void)sender;
    channel->Close();
    scheduler.shutdown();
    std::printf("stopped=true\n");
    return 0;
}
