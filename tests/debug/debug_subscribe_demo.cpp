/**
 * @file debug_subscribe_demo.cpp
 * @brief `SUBSCRIBE events` 实时推送 demo 的服务端进程。
 *
 * 用法：
 * ```text
 * go2cpp_debug_subscribe_demo [socket_path] [seconds]
 * ```
 *
 * 启动后打印 `socket=<path>`（供脚本等待），然后持续制造调度/同步/channel
 * 事件；外部客户端用 `tests/debug/subscribe_client.py` 连接并订阅。
 *
 * 事件来源（覆盖三类插桩）：
 * 1. runqueue/wake：2 个消费者 G 在无缓冲 channel 上 park/被唤醒；
 * 2. sync：2 个原生线程争用同一把 `sync::Mutex`；
 * 3. channel：生产者在 rendezvous channel 上 Send（接收者未就绪时 park）；
 * 4. timer：一个 20ms 周期 TimerService 产生 kTimerArm/kTimerFire/
 *    kTimerReschedule（验证 SUBSCRIBE 里能看到 timer 事件）。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/timer.hpp"
#include "go2cpp/debug.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int /*signal*/) { g_stop.store(true, std::memory_order_relaxed); }

}  // namespace

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/go2cpp-subscribe-demo.sock";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 30;

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
    std::printf("socket=%s\npid=%d\n", server.path().c_str(), ::getpid());
    std::fflush(stdout);

    // timer 事件源：20ms 周期定时器（kTimerArm / kTimerFire / kTimerReschedule）。
    go2cpp::core::TimerService timer_service;
    go2cpp::debug::RegisterTimer("timer", timer_service);
    go2cpp::core::TimerSourceScope timer_scope(
        go2cpp::core::TimerSource::kUser, 0);
    go2cpp::core::Timer periodic_timer =
        timer_service.ScheduleEvery(std::chrono::milliseconds(20), [] {});

    // 无缓冲 channel：Send/Recv 在对方未就绪时 park —— 产生 kChannelSend /
    // kChannelRecv / kChannelWake 与调度器的 wake 事件。
    auto channel = go2cpp::Channel<int>::Create(0);
    std::vector<std::shared_ptr<go2cpp::Task>> consumers;
    for (int i = 0; i < 2; ++i) {
        consumers.push_back(scheduler.spawn([channel] {
            while (!g_stop.load(std::memory_order_relaxed)) {
                auto result = channel->Recv();
                if (result.status != go2cpp::ChannelStatus::kReady) {
                    break;
                }
            }
        }));
    }

    // 两个原生线程争用同一把 sync::Mutex：只有真正排队时才记录
    // kMutexLock/kMutexUnlock（快路径零开销）。
    go2cpp::sync::Mutex mutex;
    std::atomic<long long> mutex_rounds{0};
    std::vector<std::thread> lockers;
    for (int i = 0; i < 2; ++i) {
        lockers.emplace_back([&mutex, &mutex_rounds] {
            while (!g_stop.load(std::memory_order_relaxed)) {
                (void)mutex.Lock();
                // 故意把临界区拉长，制造可观测的争用。
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                mutex.Unlock();
                mutex_rounds.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    long long sent = 0;
    while (!g_stop.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
        // 从一个 Fiber 里发送，使其在接收者未就绪时 park 而不是阻塞线程。
        auto sender = scheduler.spawn([channel, &sent] {
            if (channel->Send(static_cast<int>(sent)).status ==
                go2cpp::ChannelStatus::kReady) {
                ++sent;
            }
        });
        if (sender) {
            (void)sender->wait();
        }
        if ((sent & 7) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    g_stop.store(true, std::memory_order_relaxed);
    for (auto& locker : lockers) {
        locker.join();
    }
    channel->Close();
    for (auto& consumer : consumers) {
        if (consumer) {
            (void)consumer->wait();
        }
    }
    std::printf("stopped sent=%lld mutex_rounds=%lld\n", sent,
                static_cast<long long>(mutex_rounds.load()));
    std::fflush(stdout);
    periodic_timer.Cancel();
    (void)server.Unregister("timer");
    server.Stop();
    scheduler.shutdown();
    return 0;
}
