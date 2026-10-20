/**
 * @file full_trace_scenario.cpp
 * @brief 全链路可观测验收场景：≥4 个 Fiber 混合 channel / mutex / timer /
 *        select / ParkingCondition，跑满 10s，供外部 CLI 抓取全部视图。
 *
 * ## 事件覆盖意图
 *
 * | 来源 | 覆盖的事件 |
 * |---|---|
 * | Scheduler | G 创建/启动/恢复/完成、enqueue→runqueue→claim、steal、M park |
 * | channel | 阻塞 send/recv（kChannelSend/kChannelRecv）、立即完成采样、select 决策 |
 * | sync::Mutex | 快路径采样、慢路径争用、unlock+wake |
 * | ParkingCondition | 受管 wait、notify_one/notify_all |
 * | Timer | arm/fire/cancel/reschedule + 回调投递 + 定时 G 入队 |
 *
 * ## 阶段
 *
 * 1. `READY`：4 个 worker + 5 个长期 G 启动（producer/consumer/selector/
 *    pc_waiter/mutex_contender），混合流量持续 `seconds` 秒；
 * 2. `HOLDING`：停止业务 G，创建一个短命 demo G，执行
 *    `create → start → park（无缓冲 Recv）→ wake（主线程 Send）→ resume →
 *    complete`，打印 `demo_gid` 作为 timeline 的完整生命周期样本；
 * 3. 保持 socket `hold_seconds` 秒，供外部 CLI 抓取（`DEMO_DONE` 之后窗口
 *    稳定，没有新的高频事件）。
 *
 * ## 用法
 *
 * ```text
 * go2cpp_full_trace_scenario [seconds] [hold_seconds]
 * ```
 *
 * @note 不注册 ctest（ctest 用例总数保持不变），由
 *       tests/scenarios/full_trace/run_full_trace.sh 驱动。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/core/timer.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_server.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

/// 把定时器回调投递到 Scheduler 的执行器：每次 Submit 都产生一个可执行 G，
/// 用于验证 kTimerFire → kTimerCallback → kTimerEnqueue 的完整链路。
class SchedulerTimerExecutor final : public go2cpp::core::TimerExecutor {
public:
    explicit SchedulerTimerExecutor(go2cpp::Scheduler& scheduler)
        : m_scheduler(scheduler) {}

    bool Submit(std::function<void()> callback) override {
        auto task = m_scheduler.spawn(std::move(callback));
        if (task) {
            go2cpp::debug::EventRing::Record(
                go2cpp::debug::SchedEventCode::kTimerEnqueue, task->id(), 0);
        }
        return static_cast<bool>(task);
    }

private:
    go2cpp::Scheduler& m_scheduler;
};

std::atomic<bool> g_stop{false};
std::atomic<std::uint64_t> g_consumed{0};
std::atomic<std::uint64_t> g_produced{0};
std::atomic<std::uint64_t> g_select_ping{0};
std::atomic<std::uint64_t> g_mutex_ops{0};
std::atomic<std::uint64_t> g_pc_waits{0};

/// 打印一行并立即 flush（外部脚本按行同步阶段）。
void Emit(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    const int hold_seconds = argc > 2 ? std::atoi(argv[2]) : 8;

    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.min_workers = 4;
    config.max_workers = 4;
    config.allow_idle_shrink = false;
    config.collect_metrics = true;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    // timer 组件：普通 TimerService（20ms 周期）用于 arm/fire/reschedule。
    go2cpp::core::TimerService timer_service;
    go2cpp::core::Timer timer_tick = timer_service.ScheduleEvery(20ms, [] {});

    auto buffer = go2cpp::MakeChannel<int>(8);   // 有缓冲：send/recv 阻塞
    auto ping = go2cpp::MakeChannel<int>(0);     // 无缓冲 rendezvous：select

    go2cpp::sync::Mutex sync_mutex;
    go2cpp::core::ParkingCondition parking_condition;
    std::mutex pc_mutex;
    std::atomic<bool> pc_flag{false};

    go2cpp::debug::DebugServer& server = go2cpp::debug::DebugServer::Instance();
    go2cpp::debug::RegisterScheduler("scheduler", scheduler);
    go2cpp::debug::RegisterTimer("timer", timer_service);
    go2cpp::debug::RegisterEventRing("events");
    go2cpp::debug::RegisterChannel("channel.buffer", buffer);
    go2cpp::debug::RegisterChannel("channel.ping", ping);
    go2cpp::debug::RegisterMutex("mutex.sync", sync_mutex);
    go2cpp::debug::RegisterParkingCondition("parking_condition",
                                            parking_condition);

    const std::string socket_path =
        "/tmp/go2cpp-full-trace-" + std::to_string(::getpid()) + ".sock";
    if (!server.Start(socket_path)) {
        std::fprintf(stderr, "DebugServer start failed: %s\n",
                     server.last_error().c_str());
        scheduler.shutdown();
        return 1;
    }

    // 定时器回调：通过 Scheduler 执行器投递（kTimerCallback + kTimerEnqueue）。
    go2cpp::core::Timer ping_timer = scheduler.schedule_every(10ms, [ping] {
        (void)ping->TrySend(1);
    });
    // 带执行器的 TimerService 定时器：fire 事件有 timer_id，回调经
    // SchedulerTimerExecutor 变成可执行 G（kTimerEnqueue）。
    auto timer_executor =
        std::make_shared<SchedulerTimerExecutor>(scheduler);
    go2cpp::core::TimerSourceScope timer_source(
        go2cpp::core::TimerSource::kScheduler, 0);
    go2cpp::core::Timer exec_timer = timer_service.ScheduleEveryOn(
        30ms, [] {}, timer_executor);

    // G1 producer：buffer.Send 在消费者慢时阻塞，覆盖 kChannelSend。
    const auto producer = scheduler.spawn([buffer] {
        std::uint64_t sequence = 0;
        while (!g_stop.load(std::memory_order_relaxed)) {
            auto sent = buffer->Send(static_cast<int>(sequence & 0xFFFF));
            if (sent.status == go2cpp::ChannelStatus::kClosed) {
                break;
            }
            ++sequence;
            g_produced.fetch_add(1, std::memory_order_relaxed);
            if ((sequence % 5U) == 0U) {
                (void)buffer->TrySend(-1);
            }
            if ((sequence % 8U) == 0U) {
                std::this_thread::sleep_for(5ms);
            }
        }
    });

    // G2 consumer：buffer.Recv 阻塞 + Mutex 争用 + PC notify。
    const auto consumer = scheduler.spawn([buffer, &sync_mutex,
                                           &parking_condition] {
        std::uint64_t sequence = 0;
        while (!g_stop.load(std::memory_order_relaxed)) {
            auto result = buffer->Recv();
            if (result.status == go2cpp::ChannelStatus::kClosed) {
                break;
            }
            ++sequence;
            g_consumed.fetch_add(1, std::memory_order_relaxed);
            sync_mutex.lock();
            g_mutex_ops.fetch_add(1, std::memory_order_relaxed);
            sync_mutex.unlock();
            if ((sequence % 8U) == 0U) {
                std::this_thread::sleep_for(5ms);
            }
            if ((sequence % 20U) == 0U) {
                parking_condition.notify_one();
            }
        }
    });

    // G3 selector：select { case <-ping: ... ; case <-time.After(5ms): }。
    const auto selector = scheduler.spawn([ping] {
        while (!g_stop.load(std::memory_order_relaxed)) {
            auto selected = go2cpp::Select(
                {go2cpp::RecvCase(ping), go2cpp::AfterCase(5ms)});
            (void)selected;
            g_select_ping.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // G4 ParkingCondition 等待者：10ms 超时的受管等待（会 park）。
    const auto pc_waiter = scheduler.spawn(
        [&parking_condition, &pc_mutex, &pc_flag] {
            while (!g_stop.load(std::memory_order_relaxed)) {
                std::unique_lock<std::mutex> lock(pc_mutex);
                (void)parking_condition.wait_for(
                    lock, 10ms, [&pc_flag] { return pc_flag.load(); });
                ++g_pc_waits;
            }
        });

    // G5 Mutex 竞争者：与 consumer 争用同一把锁。
    const auto mutex_contender = scheduler.spawn([&sync_mutex] {
        while (!g_stop.load(std::memory_order_relaxed)) {
            sync_mutex.lock();
            g_mutex_ops.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(200us);
            sync_mutex.unlock();
            std::this_thread::sleep_for(500us);
        }
    });

    Emit("socket=%s\npid=%d\n", server.path().c_str(),
         static_cast<int>(::getpid()));
    Emit("gid_producer=%llu\n",
         static_cast<unsigned long long>(producer->id()));
    Emit("gid_consumer=%llu\n",
         static_cast<unsigned long long>(consumer->id()));
    Emit("gid_selector=%llu\n",
         static_cast<unsigned long long>(selector->id()));
    Emit("gid_pc_waiter=%llu\n",
         static_cast<unsigned long long>(pc_waiter->id()));
    Emit("gid_mutex_contender=%llu\n",
         static_cast<unsigned long long>(mutex_contender->id()));
    Emit("READY\n");

    for (int index = 0; index < seconds; ++index) {
        std::this_thread::sleep_for(1s);
        Emit("TICK %d produced=%llu consumed=%llu selects=%llu "
             "mutex_ops=%llu pc_waits=%llu\n",
             index + 1,
             static_cast<unsigned long long>(g_produced.load()),
             static_cast<unsigned long long>(g_consumed.load()),
             static_cast<unsigned long long>(g_select_ping.load()),
             static_cast<unsigned long long>(g_mutex_ops.load()),
             static_cast<unsigned long long>(g_pc_waits.load()));
    }

    // 阶段 1.5：保持业务流量 2s 的稳定窗口，供外部抓取「活动态」视图
    // （gmp 的 G 列表、park 的阻塞图在进程收敛后才会为空）。
    Emit("BUSY_WINDOW\n");
    std::this_thread::sleep_for(2s);

    // 阶段 2：停止业务流量，给 demo G 一个干净的事件窗口。
    Emit("HOLDING\n");
    g_stop.store(true, std::memory_order_relaxed);
    (void)buffer->Close();
    (void)ping->Close();
    ping_timer.Cancel();
    std::this_thread::sleep_for(300ms);
    Emit("QUIESCENT\n");

    auto demo_channel = go2cpp::MakeChannel<int>(0);
    go2cpp::debug::RegisterChannel("channel.demo", demo_channel);
    const auto demo = scheduler.spawn([demo_channel] {
        auto value = demo_channel->Recv();  // 阻塞 → kParkEnter(kChannelRecv)
        (void)value;
    });
    Emit("demo_gid=%llu\ntimeline_gid=%llu\n",
         static_cast<unsigned long long>(demo->id()),
         static_cast<unsigned long long>(demo->id()));
    std::this_thread::sleep_for(150ms);  // 等 demo G 真正 park
    (void)demo_channel->Send(7);         // 唤醒 → resume → complete
    std::this_thread::sleep_for(300ms);
    Emit("DEMO_DONE state=%s\n",
         demo->state() == go2cpp::GState::kDead ? "kDead" : "other");

    std::this_thread::sleep_for(std::chrono::seconds(hold_seconds));
    (void)timer_tick;
    (void)exec_timer;
    (void)timer_service;
    scheduler.shutdown();
    (void)server.Stop();
    Emit("DONE produced=%llu consumed=%llu selects=%llu mutex_ops=%llu "
         "pc_waits=%llu\n",
         static_cast<unsigned long long>(g_produced.load()),
         static_cast<unsigned long long>(g_consumed.load()),
         static_cast<unsigned long long>(g_select_ping.load()),
         static_cast<unsigned long long>(g_mutex_ops.load()),
         static_cast<unsigned long long>(g_pc_waits.load()));
    return 0;
}
