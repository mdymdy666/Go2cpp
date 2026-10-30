/**
 * @file test_worker_chain_opt.cpp
 * @brief Worker 调度链优化（transition_mutex / TLS / P 队列锁）的独立回归。
 *
 * @date 2026-10-30
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * 覆盖调度链改动最容易出错的路径：park/wake 往返、无缓冲 Channel 交接、
 * ParkingCondition、yield 往返、多 P work stealing、shutdown 竞态。
 * 独立可执行（不依赖 go2cpp_tests 的框架），失败时返回非零退出码。
 *
 * ```
 * ./test_worker_chain_opt            # 全部场景
 * ./test_worker_chain_opt pingpong   # 单个场景
 * ```
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace go2cpp;
using namespace std::chrono_literals;

int g_failures = 0;

#define WC_CHECK(cond)                                                     \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

SchedulerConfig MakeConfig(std::size_t processors, std::size_t workers) {
    SchedulerConfig config;
    config.processor_count = processors;
    config.max_workers = workers;
    config.min_workers = workers;
    config.idle_wait = 1ms;
    config.local_queue_limit = 256;
    config.fiber_stack_size = 64U * 1024U;
    config.collect_metrics = false;
    config.enable_sysmon = false;
    config.terminal_collect_interval = 0;
    config.allow_idle_shrink = false;
    return config;
}

// 等待异步 spawn 的 G 真正完成，再调用 shutdown()。spawn() 只是入队，
// 立即 shutdown 会让尚未出队的 G 走取消路径，测试会误判。
bool WaitUntil(const std::atomic<int>& value, int expected,
               std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (value.load(std::memory_order_acquire) != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(200us);
    }
    return value.load(std::memory_order_acquire) == expected;
}

void Print(const char* name, bool ok) {
    std::printf("%-28s %s\n", name, ok ? "PASS" : "FAIL");
}

// 1) 两个 G 在无缓冲 Channel 上严格 1:1 交接（park/wake 往返 + 队列预留路径）。
bool TestPingPong() {
    Scheduler scheduler(MakeConfig(1, 2));
    scheduler.start();
    auto channel = MakeChannel<int>(0);
    constexpr int kRounds = 5000;
    std::atomic<int> received{0};
    scheduler.spawn([&] {
        for (int i = 0; i < kRounds; ++i) {
            (void)channel->Send(i);
        }
        channel->Close();
    });
    scheduler.spawn([&] {
        for (;;) {
            auto result = channel->Recv();
            if (!result.Ok()) {
                break;
            }
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });
    (void)WaitUntil(received, kRounds);
    scheduler.shutdown();
    WC_CHECK(received.load() == kRounds);
    return received.load() == kRounds;
}

// 2) 多生产者/多消费者无缓冲 Channel（跨 M 唤醒 + 队列预留竞争）。
bool TestChannelMPMC() {
    Scheduler scheduler(MakeConfig(4, 4));
    scheduler.start();
    auto channel = MakeChannel<int>(0);
    constexpr int kProducers = 6;
    constexpr int kConsumers = 6;
    constexpr int kPerProducer = 3000;
    std::atomic<long long> sum{0};
    std::atomic<int> sent{0};
    for (int p = 0; p < kProducers; ++p) {
        scheduler.spawn([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                if (channel->Send(i).Ok()) {
                    sent.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        scheduler.spawn([&] {
            for (;;) {
                auto result = channel->Recv();
                if (!result.Ok()) {
                    break;
                }
                sum.fetch_add(result.ValueOrDefault(),
                              std::memory_order_relaxed);
            }
        });
    }
    scheduler.spawn([&] {
        // 所有消费者退出后关闭，唤醒剩余 Recv。
        std::this_thread::sleep_for(200ms);
        channel->Close();
    });
    const auto total = kProducers * kPerProducer;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (sent.load(std::memory_order_acquire) < total &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(200us);
    }
    scheduler.shutdown();
    WC_CHECK(sent.load() > 0);
    return sent.load() > 0;
}

// 3) ParkingCondition：G 与 G 之间的 wait/notify（与 cv_1p1c 基准同构）。
bool TestParkingCondition() {
    Scheduler scheduler(MakeConfig(1, 2));
    scheduler.start();
    std::mutex mutex;
    core::ParkingCondition condition;
    std::size_t turn = 0;
    constexpr std::size_t kRounds = 20000;
    std::atomic<bool> done{false};
    scheduler.spawn([&] {
        for (std::size_t i = 0; i < kRounds; ++i) {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&turn] { return turn == 1; });
            turn = 0;
            condition.notify_one();
        }
    });
    scheduler.spawn([&] {
        for (std::size_t i = 0; i < kRounds; ++i) {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&turn] { return turn == 0; });
            turn = 1;
            condition.notify_one();
        }
        done.store(true, std::memory_order_release);
    });
    for (int spin = 0; spin < 4000 && !done.load(std::memory_order_acquire);
         ++spin) {
        std::this_thread::sleep_for(200us);
    }
    scheduler.shutdown();
    WC_CHECK(done.load());
    return done.load();
}

// 4) yield 往返 + 同步 Mutex（yield 快路径 / 自旋锁竞态）。
bool TestYieldAndMutex() {
    Scheduler scheduler(MakeConfig(2, 2));
    scheduler.start();
    sync::Mutex mutex;
    std::atomic<int> executed{0};
    constexpr int kTasks = 8;
    for (int t = 0; t < kTasks; ++t) {
        scheduler.spawn([&] {
            for (int i = 0; i < 500; ++i) {
                {
                    std::lock_guard<sync::Mutex> lock(mutex);
                    executed.fetch_add(1, std::memory_order_relaxed);
                }
                (void)scheduler.yield_current();
            }
        });
    }
    (void)WaitUntil(executed, kTasks * 500, 20s);
    scheduler.shutdown();
    WC_CHECK(executed.load() == kTasks * 500);
    return executed.load() == kTasks * 500;
}

// 5) 多 P work stealing：大量短任务，跨 P 窃取 + 本地队列自旋锁竞态。
bool TestWorkStealing() {
    Scheduler scheduler(MakeConfig(4, 4));
    scheduler.start();
    constexpr int kTasks = 2000;
    std::atomic<int> executed{0};
    for (int t = 0; t < kTasks; ++t) {
        scheduler.spawn([&, t] {
            volatile int sink = t;
            for (int i = 0; i < 64; ++i) {
                sink = sink * 31 + i;
            }
            (void)sink;
            executed.fetch_add(1, std::memory_order_relaxed);
        });
    }
    (void)WaitUntil(executed, kTasks, 20s);
    scheduler.shutdown();
    WC_CHECK(executed.load() == kTasks);
    return executed.load() == kTasks;
}

// 6) shutdown 竞态：反复启动/立即关停，park 与 draining 交错。
bool TestShutdownRace() {
    constexpr int kIterations = 40;
    for (int i = 0; i < kIterations; ++i) {
        Scheduler scheduler(MakeConfig(2, 2));
        scheduler.start();
        for (int t = 0; t < 8; ++t) {
            scheduler.spawn([&] {
                auto channel = MakeChannel<int>(0);
                (void)channel->Send(1);
            });
        }
        (void)scheduler.shutdown_for(800ms);
    }
    return true;
}

// 7) 嵌套 Fiber + park：验证执行绑定在 M 之间往返不会错乱。
bool TestNestedFiber() {
    Scheduler scheduler(MakeConfig(1, 2));
    scheduler.start();
    std::atomic<int> nested_done{0};
    scheduler.spawn([&] {
        Fiber inner([&] { nested_done.fetch_add(1); });
        inner.resume();
    });
    scheduler.spawn([&] { nested_done.fetch_add(1); });
    (void)WaitUntil(nested_done, 2, 10s);
    scheduler.shutdown();
    WC_CHECK(nested_done.load() == 2);
    return nested_done.load() == 2;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    const bool all = mode == "all";
    if (all || mode == "pingpong") {
        Print("pingpong", TestPingPong());
    }
    if (all || mode == "channel") {
        Print("channel_mpmc", TestChannelMPMC());
    }
    if (all || mode == "cv") {
        Print("parking_condition", TestParkingCondition());
    }
    if (all || mode == "yield") {
        Print("yield_and_mutex", TestYieldAndMutex());
    }
    if (all || mode == "steal") {
        Print("work_stealing", TestWorkStealing());
    }
    if (all || mode == "shutdown") {
        Print("shutdown_race", TestShutdownRace());
    }
    if (all || mode == "nested") {
        Print("nested_fiber", TestNestedFiber());
    }
    std::printf("failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
