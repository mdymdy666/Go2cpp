/**
 * @file test_debug_record_correctness.cpp
 * @brief debug 记录（监控/事件环）本身的正确性测试。
 *
 * 与「原语行为正确性」分开：这里只检查调试记录层是否：
 *   1. 事件守恒：N 次 lock ↔ N 次 unlock、N 次 send ↔ N 次 recv、
 *      N 次 wait ↔ N 次 notify（全量/无采样配置下）；
 *   2. 事件配对：mutex lock/unlock 按 resource_id 配对，park/wake 按 gid 配对；
 *   3. 顺序：同线程 local_seq 严格递增；跨线程快照按 (ts_ns, thread_id, seq)；
 *   4. 多线程无丢失：4 线程并发操作 → 累计计数 == 操作数；
 *   5. 采样语义：RecordSampled 每 N 次记录一条；关键事件全量；
 *   6. 环形窗口：普通区写爆后关键事件不被普通事件覆盖；
 *   7. 类别过滤：关闭 primitives 类别后资源事件不再写入；
 *   8. OFF 构建：Enabled()=false 且计数恒 0（零开销契约）。
 *
 * 需要 GO2CPP_DEBUG_DUMP=ON 构建；OFF 构建自动走「零记录」分支。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_resources.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using go2cpp::MakeChannel;
using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;
using go2cpp::Task;
using go2cpp::GState;
using go2cpp::debug::DebugConfig;
using go2cpp::debug::DebugCategory;
using go2cpp::debug::EventCursor;
using go2cpp::debug::EventCursorEqual;
using go2cpp::debug::EventCursorLess;
using go2cpp::debug::EventRecord;
using go2cpp::debug::EventRing;
using go2cpp::debug::ResourceEventRecord;
using go2cpp::debug::ResourceResult;
using go2cpp::debug::SchedEventCode;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;
using go2cpp::sync::WaitGroup;

/// 场景开始前清空累计计数与环形内容（仅无并发写入时调用）。
void ResetAll() {
    EventRing::ResetTotals();
    EventRing::Reset();
}

std::uint64_t CountOf(SchedEventCode code) {
    return EventRing::TotalCount(code);
}

std::size_t CountInSnapshot(const std::vector<EventRecord>& events,
                            SchedEventCode code, bool blocked_only = false) {
    std::size_t count = 0;
    for (const auto& event : events) {
        if (event.code != static_cast<std::uint32_t>(code)) {
            continue;
        }
        if (blocked_only &&
            event.result !=
                static_cast<std::uint32_t>(ResourceResult::kBlocked)) {
            continue;
        }
        ++count;
    }
    return count;
}

void StoreResourceEvent(SchedEventCode code, ResourceResult result,
                        std::uint64_t resource_id = 0) {
    ResourceEventRecord event;
    event.code = code;
    event.resource_id = resource_id;
    event.actor_gid = 1;
    event.result = result;
    go2cpp::debug::RecordResourceEvent(event);
}

// ---------------------------------------------------------------------------
// 1. 事件守恒：Mutex lock/unlock、CV wait/notify、Channel send/recv
// ---------------------------------------------------------------------------
void TestMutexCvConservation() {
    go2cpp_tests::announce("debug: mutex/cv event conservation");
    constexpr int kMutexOps = 200;
    constexpr int kCvOps = 100;
    ResetAll();

    {
        Mutex mutex;
        for (int i = 0; i < kMutexOps; ++i) {
            GO2CPP_CHECK(mutex.Lock());
            mutex.Unlock();
        }
    }
    const auto lock_count = CountOf(SchedEventCode::kMutexLock);
    const auto unlock_count = CountOf(SchedEventCode::kMutexUnlock);
    GO2CPP_CHECK(lock_count == kMutexOps);
    GO2CPP_CHECK(unlock_count == kMutexOps);

    ResetAll();
    {
        Mutex mutex;
        ConditionVariable condition;
        std::atomic<int> waiting{0};
        std::thread waiter([&] {
            for (int i = 0; i < kCvOps; ++i) {
                GO2CPP_CHECK(mutex.Lock());
                waiting.fetch_add(1, std::memory_order_release);
                GO2CPP_CHECK(condition.Wait(mutex));
                mutex.Unlock();
            }
        });
        std::thread notifier([&] {
            for (int i = 0; i < kCvOps; ++i) {
                while (waiting.load(std::memory_order_acquire) <= i) {
                    std::this_thread::yield();
                }
                GO2CPP_CHECK(mutex.Lock());
                condition.NotifyOne();
                mutex.Unlock();
            }
        });
        GO2CPP_JOIN_WITH_WATCHDOG(waiter, 20s);
        GO2CPP_JOIN_WITH_WATCHDOG(notifier, 20s);
        GO2CPP_CHECK(waiting.load() == kCvOps);
    }
    const auto cv_wait = CountOf(SchedEventCode::kCvWait);
    const auto cv_notify = CountOf(SchedEventCode::kCvNotify);
    GO2CPP_CHECK(cv_wait >= static_cast<std::uint64_t>(kCvOps));
    GO2CPP_CHECK(cv_wait <= static_cast<std::uint64_t>(kCvOps * 2));
    GO2CPP_CHECK(cv_notify == kCvOps);
    std::printf(
        "[debug] mutex/cv: lock=%llu unlock=%llu cv_wait=%llu cv_notify=%llu\n",
        static_cast<unsigned long long>(lock_count),
        static_cast<unsigned long long>(unlock_count),
        static_cast<unsigned long long>(cv_wait),
        static_cast<unsigned long long>(cv_notify));
}

void TestChannelConservation() {
    go2cpp_tests::announce("debug: channel send/recv event conservation");
    constexpr int kOps = 100;
    ResetAll();
    auto channel = MakeChannel<int>(128);
    for (int i = 0; i < kOps; ++i) {
        GO2CPP_CHECK(channel->Send(i).Ok());
    }
    for (int i = 0; i < kOps; ++i) {
        const auto result = channel->Recv();
        GO2CPP_CHECK(result.Ok());
        GO2CPP_CHECK(result.ValueOrDefault() == i);
    }
    const auto send_count = CountOf(SchedEventCode::kChannelSend);
    const auto recv_count = CountOf(SchedEventCode::kChannelRecv);
    GO2CPP_CHECK(send_count == kOps);
    GO2CPP_CHECK(recv_count == kOps);
    std::printf("[debug] channel: send=%llu recv=%llu\n",
                static_cast<unsigned long long>(send_count),
                static_cast<unsigned long long>(recv_count));
}

void TestWaitGroupEvents() {
    go2cpp_tests::announce("debug: waitgroup add/done/wait events");
    constexpr int kOps = 20;
    ResetAll();
    {
        WaitGroup group;
        group.Add(kOps);
        for (int i = 0; i < kOps; ++i) {
            group.Done();
        }
        GO2CPP_CHECK(group.Wait());
    }
    const auto add_count = CountOf(SchedEventCode::kWaitGroupAdd);
    const auto done_count = CountOf(SchedEventCode::kWaitGroupDone);
    const auto wait_count = CountOf(SchedEventCode::kWaitGroupWait);
    GO2CPP_CHECK(add_count == 1);
    GO2CPP_CHECK(done_count == kOps);
    GO2CPP_CHECK(wait_count <= 1);
    std::printf("[debug] waitgroup: add=%llu done=%llu wait=%llu\n",
                static_cast<unsigned long long>(add_count),
                static_cast<unsigned long long>(done_count),
                static_cast<unsigned long long>(wait_count));
}

// ---------------------------------------------------------------------------
// 2. 配对：lock/unlock 按 resource_id；park/wake 按 gid
// ---------------------------------------------------------------------------
void TestMutexPairingByResource() {
    go2cpp_tests::announce("debug: mutex lock/unlock resource pairing");
    ResetAll();
    constexpr int kMutexes = 3;
    constexpr int kEach = 30;
    {
        std::vector<std::unique_ptr<Mutex>> mutexes;
        for (int i = 0; i < kMutexes; ++i) {
            mutexes.push_back(std::make_unique<Mutex>());
        }
        for (int round = 0; round < kEach; ++round) {
            for (auto& mutex : mutexes) {
                GO2CPP_CHECK(mutex->Lock());
                mutex->Unlock();
            }
        }
    }
    const auto events = EventRing::Snapshot();
    std::size_t locked = 0;
    std::size_t unlocked = 0;
    for (const auto& event : events) {
        if (event.code == static_cast<std::uint32_t>(SchedEventCode::kMutexLock)) {
            ++locked;
        } else if (event.code ==
                   static_cast<std::uint32_t>(SchedEventCode::kMutexUnlock)) {
            ++unlocked;
        }
    }
    // v7.1（P1）：资源事件改写入**关键区**（默认容量 64），不再受普通区
    // 噪声覆盖，因此只保留最近 kCriticalCapacity 条 —— 90 对（180 条）
    // 会回绕到 64 条。先验证「最后保留的仍是成对的一半一半」。
    const std::size_t kept = locked + unlocked;
    GO2CPP_CHECK(kept == static_cast<std::size_t>(EventRing::kCriticalCapacity));
    GO2CPP_CHECK(locked == unlocked);
    std::printf("[debug] pairing: locked=%zu unlocked=%zu\n", locked, unlocked);
}

void TestParkWakePairing() {
    go2cpp_tests::announce("debug: park/wake pairing (scheduler + channel)");
    ResetAll();
    constexpr int kFibers = 4;
    {
        SchedulerConfig config;
        config.processor_count = 1;
        config.max_workers = 1;
        config.idle_wait = 1ms;
        Scheduler scheduler(config);
        scheduler.start();

        std::vector<go2cpp::ChannelPtr<int>> channels;
        std::vector<std::shared_ptr<Task>> tasks;
        std::atomic<int> completed{0};
        for (int i = 0; i < kFibers; ++i) {
            auto channel = MakeChannel<int>(0);
            channels.push_back(channel);
            tasks.push_back(scheduler.spawn([&, channel] {
                const auto result = channel->RecvFor(5s);
                if (result.Ok()) {
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            }));
        }
        const auto deadline = Clock::now() + 5s;
        for (;;) {
            bool all_waiting = true;
            for (const auto& task : tasks) {
                if (task->state() != GState::kWaiting) {
                    all_waiting = false;
                    break;
                }
            }
            if (all_waiting || Clock::now() >= deadline) {
                GO2CPP_CHECK(all_waiting);
                break;
            }
            std::this_thread::sleep_for(2ms);
        }
        for (int i = 0; i < kFibers; ++i) {
            GO2CPP_CHECK(channels[static_cast<std::size_t>(i)]
                             ->SendFor(i, 5s)
                             .Ok());
        }
        for (auto& task : tasks) {
            GO2CPP_CHECK(task->wait_for(10s));
        }
        GO2CPP_CHECK(completed.load() == kFibers);
        scheduler.shutdown();
    }
    std::this_thread::sleep_for(50ms);
    const auto park_enter = CountOf(SchedEventCode::kParkEnter);
    const auto park_return = CountOf(SchedEventCode::kParkReturn);
    const auto wake_enqueued = CountOf(SchedEventCode::kWakeEnqueued);
    const auto wake_pending = CountOf(SchedEventCode::kWakePending);
    const auto wake_request = CountOf(SchedEventCode::kWakeRequest);
    GO2CPP_CHECK(park_enter == park_return);
    GO2CPP_CHECK(wake_enqueued + wake_pending + wake_request >= kFibers);
    std::printf(
        "[debug] park/wake: enter=%llu return=%llu wake_enq=%llu "
        "wake_pending=%llu wake_req=%llu\n",
        static_cast<unsigned long long>(park_enter),
        static_cast<unsigned long long>(park_return),
        static_cast<unsigned long long>(wake_enqueued),
        static_cast<unsigned long long>(wake_pending),
        static_cast<unsigned long long>(wake_request));

    // 快照级配对：每个 park 过的 gid 至少有一条 wake 事件。
    const auto events = EventRing::Snapshot();
    std::vector<std::uint64_t> parked_gids;
    for (const auto& event : events) {
        if (event.code ==
                static_cast<std::uint32_t>(SchedEventCode::kParkEnter) &&
            event.gid != 0) {
            parked_gids.push_back(event.gid);
        }
    }
    std::size_t paired = 0;
    for (const auto gid : parked_gids) {
        bool found = false;
        for (const auto& event : events) {
            const auto code = static_cast<SchedEventCode>(event.code);
            if (event.gid != gid) {
                continue;
            }
            if (code == SchedEventCode::kWakeRequest ||
                code == SchedEventCode::kWakeEnqueued ||
                code == SchedEventCode::kWakePending) {
                found = true;
                break;
            }
        }
        paired += found ? 1U : 0U;
    }
    GO2CPP_CHECK(paired == parked_gids.size());
    if (parked_gids.empty()) {
        std::printf(
            "[debug] park/wake snapshot: park-enter already evicted from the "
            "normal ring; cumulative park_enter==park_return still proves "
            "pairing.\n");
    } else {
        std::printf("[debug] park/wake snapshot: parked_gids=%zu paired=%zu\n",
                    parked_gids.size(), paired);
    }
    // worker ring 在 shutdown 后注销，关键区快照可能为空；以累计计数为准。
    std::printf("[debug] wake critical total=%llu (cumulative, ring may be "
                "unregistered after shutdown)\n",
                static_cast<unsigned long long>(wake_enqueued + wake_request +
                                                wake_pending));
}

// ---------------------------------------------------------------------------
// 3. 顺序：local_seq、跨线程快照排序、订阅游标不重不漏
// ---------------------------------------------------------------------------
void TestOrderingAndCursor() {
    go2cpp_tests::announce("debug: per-thread seq ordering + cursor paging");
    ResetAll();
    constexpr int kEvents = 100;
    for (int i = 0; i < kEvents; ++i) {
        EventRing::Record(SchedEventCode::kStealFail, 0,
                          static_cast<std::uint64_t>(i));
    }
    const auto events = EventRing::Snapshot();
    std::uint64_t previous_seq = 0;
    std::uint64_t previous_arg2 = 0;
    std::size_t seen = 0;
    for (const auto& event : events) {
        if (event.code != static_cast<std::uint32_t>(SchedEventCode::kStealFail)) {
            continue;
        }
        GO2CPP_CHECK(event.seq > previous_seq);
        GO2CPP_CHECK(event.arg2 == previous_arg2);
        previous_seq = event.seq;
        previous_arg2 = event.arg2 + 1;
        ++seen;
    }
    GO2CPP_CHECK(seen == kEvents);

    // 游标分页：每页 7 条，最后拼接应恰好覆盖 100 条且严格递增。
    const EventCursor start = EventRing::CurrentCursor();
    for (int i = 0; i < kEvents; ++i) {
        EventRing::Record(SchedEventCode::kStealFail, 0,
                          static_cast<std::uint64_t>(1000 + i));
    }
    std::vector<EventRecord> paged;
    EventCursor cursor = start;
    for (;;) {
        const auto page = EventRing::ReadSince(cursor, 7);
        if (page.empty()) {
            break;
        }
        for (const auto& event : page) {
            GO2CPP_CHECK(go2cpp::debug::EventAfterCursor(event, cursor));
            paged.push_back(event);
        }
        cursor = go2cpp::debug::CursorOf(page.back());
    }
    GO2CPP_CHECK(paged.size() == kEvents);
    for (std::size_t i = 1; i < paged.size(); ++i) {
        GO2CPP_CHECK(
            EventCursorLess(go2cpp::debug::CursorOf(paged[i - 1]),
                            go2cpp::debug::CursorOf(paged[i])));
    }
    std::printf("[debug] ordering: snapshot=%zu paged=%zu\n", events.size(),
                paged.size());
}

void TestMultiThreadNoLoss() {
    go2cpp_tests::announce("debug: 4 threads x 500 ops no counter loss");
    ResetAll();
    constexpr int kThreads = 4;
    constexpr int kOps = 500;
    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            Mutex local;
            for (int i = 0; i < kOps; ++i) {
                GO2CPP_CHECK(local.Lock());
                local.Unlock();
            }
            ready.fetch_add(1, std::memory_order_release);
            // 保持线程存活，让 ring 仍可被 Snapshot 读取（计数已独立于 ring）。
            while (ready.load(std::memory_order_acquire) < kThreads) {
                std::this_thread::yield();
            }
        });
    }
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 30s);
    }
    const auto lock_count = CountOf(SchedEventCode::kMutexLock);
    const auto unlock_count = CountOf(SchedEventCode::kMutexUnlock);
    GO2CPP_CHECK(lock_count == kThreads * kOps);
    GO2CPP_CHECK(unlock_count == kThreads * kOps);
    std::printf("[debug] multithread: lock=%llu unlock=%llu expected=%d\n",
                static_cast<unsigned long long>(lock_count),
                static_cast<unsigned long long>(unlock_count),
                kThreads * kOps);
}

// ---------------------------------------------------------------------------
// 4. 采样语义与环形窗口
// ---------------------------------------------------------------------------
void TestSamplingSemantics() {
    go2cpp_tests::announce("debug: sampling (1/32) and critical full capture");
    ResetAll();
    DebugConfig config = DebugConfig::Get();
    config.sample_shift = 0;
    DebugConfig::Set(config);

    constexpr int kSampled = 1000;
    for (int i = 0; i < kSampled; ++i) {
        EventRing::RecordSampled(SchedEventCode::kSysmonPass, 0, 0, 32);
    }
    const auto sampled_count = CountOf(SchedEventCode::kSysmonPass);
    GO2CPP_CHECK(sampled_count == kSampled / 32);

    // 关键事件（wake 系列）全量记录。
    constexpr int kCritical = 10;
    for (int i = 0; i < kCritical; ++i) {
        EventRing::Record(SchedEventCode::kWakeRequest, 7, 0);
    }
    const auto critical_count = CountOf(SchedEventCode::kWakeRequest);
    GO2CPP_CHECK(critical_count == kCritical);
    std::printf("[debug] sampling: sampled=%llu/%d critical=%llu/%d\n",
                static_cast<unsigned long long>(sampled_count), kSampled,
                static_cast<unsigned long long>(critical_count), kCritical);

    // 资源事件采样：非关键 ok 1/4，关键 blocked 全量（v7 语义）。
    go2cpp::debug::SetDebugEventSampleEvery(4, 1);
    ResetAll();
    {
        Mutex mutex;
        for (int i = 0; i < 40; ++i) {
            GO2CPP_CHECK(mutex.Lock());
            mutex.Unlock();
        }
    }
    const auto sampled_ok = CountOf(SchedEventCode::kMutexLock);
    for (int i = 0; i < 10; ++i) {
        StoreResourceEvent(SchedEventCode::kMutexLock, ResourceResult::kBlocked,
                           77);
    }
    const auto sampled_all = CountOf(SchedEventCode::kMutexLock);
    GO2CPP_CHECK(sampled_ok == 40 / 4);
    GO2CPP_CHECK(sampled_all == sampled_ok + 10);
    go2cpp::debug::SetDebugEventSampleEvery(1, 1);
    std::printf(
        "[debug] resource sampling: ok=%llu/%d (1/4) blocked=%llu/%d "
        "(critical full)\n",
        static_cast<unsigned long long>(sampled_ok), 40,
        static_cast<unsigned long long>(sampled_all - sampled_ok), 10);
}

void TestRingWindowPreservesCritical() {
    go2cpp_tests::announce("debug: ring window keeps critical over normal noise");
    ResetAll();
    constexpr int kNormal = 1000;
    for (int i = 0; i < kNormal; ++i) {
        EventRing::Record(SchedEventCode::kStealFail, 0,
                          static_cast<std::uint64_t>(i));
    }
    constexpr int kCritical = 10;
    for (int i = 0; i < kCritical; ++i) {
        EventRing::Record(SchedEventCode::kWakeRequest, 9, 0);
    }
    const auto events = EventRing::Snapshot();
    const auto normal_kept =
        CountInSnapshot(events, SchedEventCode::kStealFail);
    const auto critical_kept =
        CountInSnapshot(events, SchedEventCode::kWakeRequest);
    GO2CPP_CHECK(normal_kept == EventRing::kCapacity);
    GO2CPP_CHECK(critical_kept == kCritical);
    std::printf(
        "[debug] ring window: normal_kept=%zu/%d critical_kept=%zu/%d "
        "capacity=%zu critical_capacity=%zu\n",
        normal_kept, kNormal, critical_kept, kCritical,
        EventRing::kCapacity, EventRing::kCriticalCapacity);

    // 关键区自身也是环形：超过 64 条只保留最近 64 条（设计边界，记录数据）。
    ResetAll();
    constexpr int kOverflow = 100;
    for (int i = 0; i < kOverflow; ++i) {
        EventRing::Record(SchedEventCode::kWakeRequest, 10, 0);
    }
    const auto overflow_events = EventRing::Snapshot();
    const auto overflow_kept =
        CountInSnapshot(overflow_events, SchedEventCode::kWakeRequest);
    GO2CPP_CHECK(overflow_kept == EventRing::kCriticalCapacity);
    std::printf("[debug] critical overflow: kept=%zu/%d\n", overflow_kept,
                kOverflow);
}

// ---------------------------------------------------------------------------
// 5. 慢路径事件覆盖缺口（如实测量，不把它写成通过）
// ---------------------------------------------------------------------------
void TestSlowPathCoverageGap() {
    go2cpp_tests::announce("debug: slow-path coverage vs normal ring");
    ResetAll();

    // 模拟一次慢路径 mutex 事件（阻塞）：当前实现写普通区。
    StoreResourceEvent(SchedEventCode::kMutexLock, ResourceResult::kBlocked, 42);
    auto before = EventRing::Snapshot();
    const auto before_kept =
        CountInSnapshot(before, SchedEventCode::kMutexLock, true);
    for (int i = 0; i < 300; ++i) {
        EventRing::Record(SchedEventCode::kStealFail, 0, 0);
    }
    auto after = EventRing::Snapshot();
    const auto after_kept =
        CountInSnapshot(after, SchedEventCode::kMutexLock, true);
    std::printf(
        "[coverage] mutex blocked event: before=%zu after=%zu "
        "(normal ring capacity=%zu)\n",
        before_kept, after_kept, EventRing::kCapacity);
    GO2CPP_CHECK(before_kept == 1);
    // 期望缺口：慢路径阻塞事件当前走普通区，300 条普通噪声后被覆盖。
    if (after_kept == 0) {
        std::printf(
            "[coverage] GAP CONFIRMED: mutex blocked / channel blocking events "
            "are normal-ring events and can be evicted; parking/wake critical "
            "events are kept.\n");
    } else {
        std::printf(
            "[coverage] slow-path event still present after noise; ring "
            "capacity may be larger than expected.\n");
    }
}

void TestCategoryFilter() {
    go2cpp_tests::announce("debug: category filter drops primitives events");
    ResetAll();
    DebugConfig config = DebugConfig::Get();
    config.primitives = false;
    config.park = true;
    DebugConfig::Set(config);
    GO2CPP_CHECK(!DebugConfig::Enabled(DebugCategory::kPrimitives));

    StoreResourceEvent(SchedEventCode::kMutexLock, ResourceResult::kOk, 7);
    GO2CPP_CHECK(CountOf(SchedEventCode::kMutexLock) == 0);

    config.primitives = true;
    DebugConfig::Set(config);
    StoreResourceEvent(SchedEventCode::kMutexLock, ResourceResult::kOk, 7);
    GO2CPP_CHECK(CountOf(SchedEventCode::kMutexLock) == 1);
    DebugConfig::Reset();
    std::printf("[debug] category filter: primitives off -> 0, on -> 1\n");
}

// ---------------------------------------------------------------------------
// OFF 构建：零记录契约
// ---------------------------------------------------------------------------
void RunOffBuildChecks() {
    go2cpp_tests::announce("debug: OFF build zero-record contract");
    EventRing::ResetTotals();
    EventRing::Reset();
    EventRing::Record(SchedEventCode::kMutexLock, 1, 1);
    EventRing::RecordSampled(SchedEventCode::kMutexFastLock, 1, 1, 1);
    const auto all = EventRing::TotalCountAll();
    const auto snapshot = EventRing::Snapshot();
    GO2CPP_CHECK(!EventRing::Enabled());
    GO2CPP_CHECK(all == 0);
    GO2CPP_CHECK(snapshot.empty());
    std::printf(
        "[debug] OFF: enabled=false total=%llu snapshot=%zu (expected 0/0)\n",
        static_cast<unsigned long long>(all), snapshot.size());
}

}  // namespace

int main() {
    std::printf("[debug] Go2Cpp debug-record correctness tests\n");
    if (!EventRing::Enabled()) {
        RunOffBuildChecks();
    } else {
        // 「关闭采样」：every=1，非关键资源事件也全量记录。
        go2cpp::debug::SetDebugEventSampleEvery(1, 1);
        std::printf(
            "[debug] enabled=true normal_capacity=%zu critical_capacity=%zu "
            "resource_sample=1 sched_sample=1\n",
            EventRing::kCapacity, EventRing::kCriticalCapacity);
        TestMutexCvConservation();
        TestChannelConservation();
        TestWaitGroupEvents();
        TestMutexPairingByResource();
        TestParkWakePairing();
        TestOrderingAndCursor();
        TestMultiThreadNoLoss();
        TestSamplingSemantics();
        TestRingWindowPreservesCritical();
        TestSlowPathCoverageGap();
        TestCategoryFilter();
    }

    if (go2cpp_tests::g_failures.load(std::memory_order_acquire) == 0) {
        std::printf("[debug] PASS\n");
        return 0;
    }
    std::fprintf(stderr, "[debug] FAIL: %d checks failed\n",
                 go2cpp_tests::g_failures.load(std::memory_order_acquire));
    return 1;
}
