/**
 * @file fixture_parent_park.cpp
 * @brief Debug 父 Fiber park 可见性夹具（测试专用，不修改核心源码）。
 *
 * 拓扑：
 *   main ──spawn──> 父 G ──spawn──> 子 G
 *                     │                │
 *                     └─ Recv(parent_ch) 阻塞（park）
 *                                      └─ Recv(child_ch) 阻塞（park）
 *
 * 父/子都停在无缓冲 channel 的接收上；主线程保持进程存活，等待外部
 * `dump --sections scheduler` 检查：
 *   - 父 G 是否仍在 tasks；
 *   - 父 G 的 state 是否为 kWaiting（park 后的合法状态），而不是 kDead/kFailed；
 *   - tasks 是否携带 parent_id / fiber 栈（当前 schema 没有这些字段）。
 *
 * 用法: fixture_parent_park [socket] [seconds]
 * 输出: READY parent_gid=<id> child_gid=<id> socket=<path>
 */
#include "go2cpp/channel.hpp"
#include "go2cpp/debug.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/go2cpp-parent-park.sock";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 120;

    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.min_workers = 2;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    auto parent_channel = go2cpp::Channel<int>::Create(0);
    auto child_channel = go2cpp::Channel<int>::Create(0);
    std::atomic<unsigned long long> child_gid{0};

    auto parent = scheduler.spawn([&] {
        auto child = scheduler.spawn([&] {
            if (auto* self = go2cpp::Scheduler::current_task_ptr()) {
                child_gid.store(static_cast<unsigned long long>(self->id()));
            }
            (void)child_channel->Recv();  // 子 G 永久 park
        });
        (void)child;
        (void)parent_channel->Recv();     // 父 G 永久 park
    });

    for (int i = 0; i < 500 && child_gid.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // 给父 G 足够时间走到 Recv 的 park 路径。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    go2cpp::debug::DebugServer& server = go2cpp::debug::DebugServer::Instance();
    go2cpp::debug::RegisterScheduler("scheduler", scheduler);
    if (!server.Start(socket_path)) {
        std::fprintf(stderr, "debug server start failed: %s\n",
                     server.last_error().c_str());
        scheduler.shutdown();
        return 1;
    }

    std::printf("READY parent_gid=%llu child_gid=%llu socket=%s\n",
                static_cast<unsigned long long>(parent->id()),
                child_gid.load(), server.path().c_str());
    std::fflush(stdout);

    for (int i = 0; i < seconds * 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.Stop();
    scheduler.shutdown();
    return 0;
}
