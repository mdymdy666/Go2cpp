/**
 * @file diagnostic_deadlock_demo.cpp
 * @brief Doctor 端到端演示：构造真实的「丢唤醒」卡死，再从外部诊断并唤醒。
 *
 * ## 场景
 *
 * - 2 个 P / 2 个 M；
 * - G1 在 `ParkingCondition` 上等 `ready == true`；
 * - G2 把 `ready` 置为 true，但**故意不 notify**（模拟丢唤醒）；
 * - G1 永久 park，runqueue 为空，所有 M 睡在调度器空闲等待上 —— 进程卡死，
 *   但 Doctor 的接收/执行线程不经过 Scheduler，仍然可以响应；
 * - 另一个 G 阻塞在无缓冲 Channel 的 `Recv()` 上，供 `dump`/等待图展示。
 *
 * ## 外部用法
 *
 * ```bash
 * ./go2cpp_diagnostic_demo /tmp/demo.sock
 * # 另一个终端：
 * python3 tests/debug/doctor_client.py /tmp/demo.sock status
 * python3 tests/debug/doctor_client.py /tmp/demo.sock deadlock-report
 * python3 tests/debug/doctor_client.py /tmp/demo.sock "force-wake scope=all"
 * # 进程 stdout 出现 RECOVERED 即恢复
 * ```
 *
 * 未指定 socket 路径时使用 `GO2CPP_DOCTOR_SOCKET` 或
 * `/tmp/go2cpp-<pid>-doctor.sock`。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/debug/debug_server.hpp"
#include "go2cpp/debug/diagnostic_agent.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int /*signal*/) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
    using namespace go2cpp;
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGINT, OnSignal);

    std::string socket_path;
    if (argc > 1) {
        socket_path = argv[1];
    }

    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    Scheduler scheduler(config);
    scheduler.start();

    // 既有 DebugServer 组件（`dump` 复用它们的 schema）。
    (void)debug::RegisterScheduler("scheduler", scheduler);

    auto channel = Channel<int>::Create(0);
    debug::RegisterChannel("demo-channel", channel);

    // Doctor 注册：scheduler（可唤醒）、ParkingCondition（可唤醒）、
    // Channel（可唤醒）、以及 dump 用的组件。
    debug::DiagnosticAgent& doctor = debug::DiagnosticAgent::Instance();
    if (!doctor.RegisterScheduler("scheduler", scheduler)) {
        std::fprintf(stderr, "register scheduler failed\n");
        return 1;
    }
    core::ParkingCondition condition;
    if (!doctor.RegisterParkingCondition("demo-cv", condition)) {
        std::fprintf(stderr, "register parking condition failed\n");
        return 1;
    }
    if (!doctor.RegisterChannel("demo-channel", channel)) {
        std::fprintf(stderr, "register channel failed\n");
        return 1;
    }
    // 再注册一个从未持有的 Mutex：让 `dump sections=all` 能展示 mutex 组件，
    // 同时它也进入 mutex-unlock 的白名单判定（未持有 -> refused not-locked）。
    sync::Mutex demo_mutex;
    if (!doctor.RegisterMutex("demo-mutex", demo_mutex)) {
        std::fprintf(stderr, "register mutex failed\n");
        return 1;
    }

    std::mutex mutex;
    bool ready = false;
    std::atomic<bool> recovered{false};
    std::atomic<bool> waiter_parked{false};

    // G1：条件等待；收到唤醒后会重检 ready，因此恢复是安全的。
    auto waiter = scheduler.spawn([&] {
        std::unique_lock<std::mutex> lock(mutex);
        waiter_parked.store(true);
        condition.wait(lock, [&] { return ready; });
        recovered.store(true);
        std::printf("RECOVERED ready=%d\n", ready ? 1 : 0);
        std::fflush(stdout);
    });
    // G2：等 G1 **真正 park** 之后再修改条件，并故意不 notify（丢唤醒）。
    // 如果不等，G2 可能先跑完，G1 进入 wait 时谓词已为真而直接返回，
    // 就构造不出卡死现场了。
    (void)scheduler.spawn([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (condition.DebugSnapshot().waiters == 0U &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
        // 这里本应 condition.notify_all()，故意省略。
    });
    // G3：阻塞在无缓冲 Channel 上，供等待图/dump 展示；它一直存活到进程退出，
    // 因此可以安全地把 Fiber 注册到 DebugServer（不会悬空）。
    (void)scheduler.spawn([&] {
        debug::RegisterFiber("channel-waiter", *Fiber::Current());
        const auto result = channel->Recv();
        std::printf("CHANNEL_RECV status=%d\n",
                    static_cast<int>(result.status));
        std::fflush(stdout);
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!waiter_parked.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!waiter_parked.load()) {
        std::fprintf(stderr, "waiter did not reach parking point\n");
        return 1;
    }
    // 等 G1 真正 park（waiters>0），再等 G2 把 ready 置真但未 notify。
    while (condition.DebugSnapshot().waiters == 0U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    bool setup_ok = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            setup_ok = ready;
        }
        if (setup_ok) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!setup_ok) {
        std::fprintf(stderr, "lost-wake setup failed: ready is false\n");
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    debug::DiagnosticAgentConfig doctor_config;
    doctor_config.socket_path = socket_path;
    doctor_config.recv_timeout_ms = 50;
    doctor_config.detect_interval = std::chrono::milliseconds(100);
    doctor_config.deadlock_threshold = std::chrono::milliseconds(800);
    doctor_config.auto_force_wake = false;
    if (!doctor.Start(doctor_config)) {
        std::fprintf(stderr, "Doctor start failed: %s\n",
                     doctor.last_error().c_str());
        return 1;
    }

    std::printf("READY pid=%d socket=%s deadlock_threshold_ms=%lld\n",
                static_cast<int>(::getpid()), doctor.socket_path().c_str(),
                static_cast<long long>(doctor_config.deadlock_threshold.count()));
    std::fflush(stdout);

    while (!g_stop) {
        (void)recovered.load();
        (void)waiter;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    doctor.Stop();
    std::_Exit(0);
}
