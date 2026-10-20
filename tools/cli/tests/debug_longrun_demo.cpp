// 集成测试用的长跑进程：注册 scheduler/fiber/channel 组件并启动 DebugServer，
// 供 go2cpp-cli status/dump 通过真实 Unix socket 读取。
#include "go2cpp/channel.hpp"
#include "go2cpp/debug/debug_server.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int /*signal*/) { g_stop = 1; }

}  // namespace

int main() {
    using namespace go2cpp;

    std::signal(SIGTERM, OnSignal);
    std::signal(SIGINT, OnSignal);

    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    Scheduler scheduler(config);
    scheduler.start();

    auto recv_channel = Channel<int>::Create(0);
    auto send_channel = Channel<int>::Create(0);

    debug::RegisterScheduler("scheduler", scheduler);
    debug::RegisterChannel("channel", recv_channel);
    debug::RegisterChannel("channel-send", send_channel);

    std::atomic<int> ready{0};
    std::vector<std::shared_ptr<Task>> tasks;

    // 两个 G 阻塞在无缓冲 channel 的 Recv 上（recv_waiters=2）。
    for (int index = 0; index < 2; ++index) {
        tasks.push_back(scheduler.spawn([&recv_channel, &ready, index] {
            const std::string name =
                index == 0 ? "fiber" : "fiber-recv2";
            debug::RegisterFiber(name, *Fiber::Current());
            ready.fetch_add(1);
            (void)recv_channel->Recv();
        }));
    }

    // 一个 G 阻塞在无接收者的 Send 上（send_waiters=1）。
    tasks.push_back(scheduler.spawn([&send_channel, &ready] {
        debug::RegisterFiber("fiber-send", *Fiber::Current());
        ready.fetch_add(1);
        (void)send_channel->Send(99);
    }));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ready.load() < 3 && !g_stop &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (ready.load() < 3) {
        std::fprintf(stderr, "fibers did not reach blocking points\n");
        return 1;
    }
    // 给 Recv/Send 一点时间真正 park。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (!debug::DebugServer::Instance().Start()) {
        std::fprintf(stderr, "DebugServer::Start failed: %s\n",
                     debug::DebugServer::Instance().last_error().c_str());
        return 1;
    }

    std::printf("READY pid=%d socket=%s\n", static_cast<int>(::getpid()),
                debug::DebugServer::Instance().path().c_str());
    std::fflush(stdout);

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    debug::DebugServer::Instance().Stop();
    std::_Exit(0);
}
