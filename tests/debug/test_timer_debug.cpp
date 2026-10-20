/**
 * @file test_timer_debug.cpp
 * @brief TimerService 的调试内省测试：快照 / 来源分类 / dump(text+JSON) /
 *        事件环 / DebugServer / 10k 压测 / 并发 dump。
 *
 * 覆盖（按项目规范：真实场景 + 压测 + 随机 + 并发）：
 *   场景 1 只读快照：active、堆顶 deadline、线程状态、dump 序号单调
 *   场景 2 来源分类：context/PC/channel/timed_mutex/scheduler/sleep/user
 *   场景 3 DumpTimer：text 字段稳定 + JSON 可解析 + 一致性标记
 *   场景 4 统计：fired/completed/cancelled/rescheduled 随真实触发变化
 *   场景 5 事件环：kTimerArm/kTimerFire/kTimerCancel/kTimerReschedule
 *   场景 6 DebugServer：RegisterTimer 后 DUMP/JSON/LIST 可用
 *   压测   10k 活动 timer 的 schedule/cancel/dump 延迟（P50/P95/P99）
 *   并发   4 线程并发 dump + 主线程 schedule/cancel，快照不崩且序号单调
 */

#include "go2cpp/concurrency/core/timer.hpp"
#include "go2cpp/debug.hpp"
#include "go2cpp/json.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using go2cpp::core::Timer;
using go2cpp::core::TimerDebugSnapshot;
using go2cpp::core::TimerExecutor;
using go2cpp::core::TimerOptions;
using go2cpp::core::TimerRepeatMode;
using go2cpp::core::TimerService;
using go2cpp::core::TimerSource;
using go2cpp::core::TimerSourceScope;
using go2cpp::core::TimerState;
using go2cpp::core::TimerThreadState;
using go2cpp::core::kTimerSourceCount;
using go2cpp::debug::DebugServer;
using go2cpp::debug::DumpPayload;
using go2cpp::debug::DumpTimer;
using go2cpp::debug::EventRing;
using go2cpp::debug::SchedEventCode;

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// 从 text dump 里取 `key=value` 的数值；找不到返回 0。
std::uint64_t FieldU64(const std::string& text, const std::string& key) {
    const std::string marker = key + "=";
    std::size_t position = 0;
    while ((position = text.find(marker, position)) != std::string::npos) {
        if (position == 0 || text[position - 1] == '\n') {
            const std::size_t begin = position + marker.size();
            std::size_t end = begin;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
                ++end;
            }
            if (end > begin) {
                return std::stoull(text.substr(begin, end - begin));
            }
            return 0;
        }
        position += marker.size();
    }
    return 0;
}

template <typename Predicate>
bool WaitFor(Predicate&& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

/// 同步执行器：ScheduleOn 用它验证 scheduler 来源的自动分类。
class InlineExecutor final : public TimerExecutor {
public:
    bool Submit(std::function<void()> callback) override {
        if (callback) {
            callback();
        }
        return true;
    }
};

std::uint64_t SteadyNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// P50/P95/P99（输入会被排序）。
void PrintPercentiles(const char* tag, std::vector<std::uint64_t> samples) {
    if (samples.empty()) {
        return;
    }
    std::sort(samples.begin(), samples.end());
    const auto at = [&samples](double fraction) {
        const std::size_t index = static_cast<std::size_t>(
            fraction * static_cast<double>(samples.size() - 1));
        return samples[index];
    };
    std::printf("[timer-debug] %s n=%zu p50=%lluns p95=%lluns p99=%lluns "
                "max=%lluns\n",
                tag, samples.size(),
                static_cast<unsigned long long>(at(0.50)),
                static_cast<unsigned long long>(at(0.95)),
                static_cast<unsigned long long>(at(0.99)),
                static_cast<unsigned long long>(samples.back()));
}

// ---------------------------------------------------------------------------
// 场景 1 + 2：只读快照与来源分类
// ---------------------------------------------------------------------------
void ScenarioSnapshotAndSources() {
    TimerService service;
    std::vector<Timer> handles;
    const auto far =
        std::chrono::steady_clock::now() + std::chrono::hours(1);
    std::atomic<int> fired{0};

    struct SourceCase {
        TimerSource source;
        const char* name;
    };
    const SourceCase cases[] = {
        {TimerSource::kContext, "kContext"},
        {TimerSource::kParkingCondition, "kParkingCondition"},
        {TimerSource::kChannel, "kChannel"},
        {TimerSource::kTimedMutex, "kTimedMutex"},
        {TimerSource::kSleep, "kSleep"},
        {TimerSource::kUser, "kUser"},
    };
    std::uint32_t gid = 100;
    for (const auto& item : cases) {
        TimerSourceScope scope(item.source, gid++);
        handles.push_back(service.Schedule(far, [&fired] {
            fired.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    const TimerDebugSnapshot first = service.DebugSnapshot();
    GO2CPP_CHECK(first.active == 6);
    GO2CPP_CHECK(first.next_deadline.has_value());
    GO2CPP_CHECK(first.armed_total == 6);
    GO2CPP_CHECK(first.running);
    GO2CPP_CHECK(!first.stopping);
    GO2CPP_CHECK(first.thread_joinable);
    GO2CPP_CHECK(first.thread_id != 0U);
    GO2CPP_CHECK(first.thread_state == TimerThreadState::kStarting ||
                 first.thread_state == TimerThreadState::kWaiting ||
                 first.thread_state == TimerThreadState::kExecuting);
    GO2CPP_CHECK(first.sources.size() == kTimerSourceCount);
    std::size_t classified = 0;
    for (std::size_t index = 0; index < first.sources.size(); ++index) {
        const auto& stats = first.sources[index];
        if (index == static_cast<std::size_t>(TimerSource::kUnknown) ||
            index == static_cast<std::size_t>(TimerSource::kScheduler)) {
            continue;
        }
        classified += static_cast<std::size_t>(stats.active);
        GO2CPP_CHECK(stats.active == 1);
        GO2CPP_CHECK(stats.armed == 1);
    }
    GO2CPP_CHECK(classified == 6);

    // 线程标签在作用域结束后必须恢复：不会再污染后续注册。
    GO2CPP_CHECK(go2cpp::core::ThreadTimerSource() == TimerSource::kUnknown);
    {
        TimerSourceScope scope(TimerSource::kChannel, 4242);
        GO2CPP_CHECK(go2cpp::core::ThreadTimerSource() ==
                     TimerSource::kChannel);
        GO2CPP_CHECK(go2cpp::core::ThreadTimerSourceGid() == 4242);
    }
    GO2CPP_CHECK(go2cpp::core::ThreadTimerSource() == TimerSource::kUnknown);

    // ScheduleOn 自动归到 scheduler 来源（唯一调用方是调度器）。
    auto executor = std::make_shared<InlineExecutor>();
    handles.push_back(service.ScheduleOn(far, [] {}, executor));
    const TimerDebugSnapshot second = service.DebugSnapshot();
    GO2CPP_CHECK(second.active == 7);
    GO2CPP_CHECK(second.sources[static_cast<std::size_t>(
                                   TimerSource::kScheduler)]
                     .active == 1);
    GO2CPP_CHECK(second.seq > first.seq);

    // 句柄析构即取消：离开作用域前必须把统计考察完。
    GO2CPP_CHECK(fired.load(std::memory_order_relaxed) == 0);
}

// ---------------------------------------------------------------------------
// 场景 3：DumpTimer text + JSON + 一致性标记
// ---------------------------------------------------------------------------
void ScenarioDumpFormats() {
    TimerService service;
    TimerSourceScope scope(TimerSource::kChannel, 7);
    Timer keep = service.ScheduleAfter(std::chrono::hours(1), [] {});

    const DumpPayload payload = DumpTimer(service);
    GO2CPP_CHECK(Contains(payload.text, "schema=go2cpp.dump.timer/1"));
    GO2CPP_CHECK(Contains(payload.text, "version=1"));
    GO2CPP_CHECK(Contains(payload.text, "consistent=per_call"));
    GO2CPP_CHECK(Contains(payload.text, "seq_at_start="));
    GO2CPP_CHECK(Contains(payload.text, "seq_at_end="));
    GO2CPP_CHECK(Contains(payload.text, "changed_during_dump="));
    GO2CPP_CHECK(Contains(payload.text, "event_seq_supported="));
    GO2CPP_CHECK(Contains(payload.text, "next_deadline_ns="));
    GO2CPP_CHECK(Contains(payload.text, "next_deadline_in_ns="));
    GO2CPP_CHECK(Contains(payload.text, "active=1"));
    GO2CPP_CHECK(Contains(payload.text, "armed_total=1"));
    GO2CPP_CHECK(Contains(payload.text, "stats.fired="));
    GO2CPP_CHECK(Contains(payload.text, "stats.cancelled="));
    GO2CPP_CHECK(Contains(payload.text, "stats.rescheduled="));
    GO2CPP_CHECK(Contains(payload.text, "source.count=8"));
    GO2CPP_CHECK(Contains(payload.text, "source.kChannel.active=1"));
    GO2CPP_CHECK(Contains(payload.text, "source.kContext.active=0"));
    GO2CPP_CHECK(Contains(payload.text, "thread.state="));
    GO2CPP_CHECK(Contains(payload.text, "timer_events.count="));
    GO2CPP_CHECK(Contains(payload.text, "thread.wait_wakeups="));
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    GO2CPP_CHECK(Contains(payload.text, "event_seq_supported=true"));
#else
    GO2CPP_CHECK(Contains(payload.text, "event_seq_supported=false"));
#endif
    GO2CPP_CHECK(FieldU64(payload.text, "next_deadline_ns") > 0U);
    GO2CPP_CHECK(FieldU64(payload.text, "active") == 1U);

    GO2CPP_CHECK(Contains(payload.json, "\"schema\":\"go2cpp.dump.timer/1\""));
    GO2CPP_CHECK(Contains(payload.json, "\"sources\":{"));
    GO2CPP_CHECK(Contains(payload.json, "\"kChannel\":{"));
    GO2CPP_CHECK(Contains(payload.json, "\"seq_at_start\":"));
    GO2CPP_CHECK(Contains(payload.json, "\"seq_at_end\":"));
    GO2CPP_CHECK(Contains(payload.json, "\"thread\":{"));
    bool parsed = false;
    try {
        const auto value = go2cpp::Json::parse(payload.json);
        parsed = value.contains("schema") &&
                 value["schema"].get<std::string>() ==
                     "go2cpp.dump.timer/1" &&
                 value.contains("active") &&
                 value.contains("sources") && value.contains("thread");
    } catch (...) {
        parsed = false;
    }
    GO2CPP_CHECK(parsed);
}

// ---------------------------------------------------------------------------
// 场景 4：统计随真实触发/取消/周期重排变化
// ---------------------------------------------------------------------------
void ScenarioStats() {
    TimerService service;
    std::atomic<int> once_fired{0};
    std::atomic<int> period_fired{0};

    Timer once = service.ScheduleAfter(std::chrono::milliseconds(2),
                                       [&once_fired] {
                                           once_fired.fetch_add(
                                               1, std::memory_order_relaxed);
                                       });
    GO2CPP_REQUIRE(WaitFor(
        [&once_fired] {
            return once_fired.load(std::memory_order_relaxed) >= 1;
        },
        std::chrono::milliseconds(2000)));

    Timer cancelled =
        service.ScheduleAfter(std::chrono::hours(1), [] {});
    GO2CPP_CHECK(cancelled.Active());
    cancelled.Cancel();
    GO2CPP_CHECK(!cancelled.Active());

    TimerOptions options;
    options.repeat = TimerRepeatMode::kFixedDelay;
    Timer periodic = service.ScheduleEvery(std::chrono::milliseconds(2),
                                           [&period_fired] {
                                               period_fired.fetch_add(
                                                   1,
                                                   std::memory_order_relaxed);
                                           },
                                           options);
    GO2CPP_REQUIRE(WaitFor(
        [&period_fired] {
            return period_fired.load(std::memory_order_relaxed) >= 3;
        },
        std::chrono::milliseconds(2000)));
    periodic.Cancel();

    const TimerDebugSnapshot snapshot = service.DebugSnapshot();
    GO2CPP_CHECK(snapshot.fired >= 4U);
    GO2CPP_CHECK(snapshot.completed >= 1U);
    GO2CPP_CHECK(snapshot.cancelled >= 2U);
    GO2CPP_CHECK(snapshot.rescheduled >= 2U);
    GO2CPP_CHECK(snapshot.active == 0U);
    GO2CPP_CHECK(snapshot.sources[static_cast<std::size_t>(
                                     TimerSource::kUnknown)]
                     .fired >= 4U);
    GO2CPP_CHECK(snapshot.sources[static_cast<std::size_t>(
                                     TimerSource::kUnknown)]
                     .cancelled >= 2U);
    GO2CPP_CHECK(snapshot.sources[static_cast<std::size_t>(
                                     TimerSource::kUnknown)]
                     .rescheduled >= 2U);
}

// ---------------------------------------------------------------------------
// 场景 5：事件环（仅 GO2CPP_DEBUG_DUMP=ON 构建记录）
// ---------------------------------------------------------------------------
void ScenarioEvents() {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    EventRing::Reset();
    GO2CPP_CHECK(EventRing::Enabled());
    std::atomic<int> fired{0};
    std::vector<go2cpp::debug::EventRecord> records;
    DumpPayload events_dump;
    {
        TimerService service;
        TimerSourceScope scope(TimerSource::kChannel, 77);
        Timer timer = service.ScheduleAfter(std::chrono::milliseconds(1),
                                            [&fired] {
                                                fired.fetch_add(
                                                    1,
                                                    std::memory_order_relaxed);
                                            });
        GO2CPP_REQUIRE(WaitFor(
            [&fired] {
                return fired.load(std::memory_order_relaxed) >= 1;
            },
            std::chrono::milliseconds(2000)));
        Timer far = service.ScheduleAfter(std::chrono::hours(1), [] {});
        far.Cancel();
        const TimerDebugSnapshot snapshot = service.DebugSnapshot();
        GO2CPP_CHECK(snapshot.event_ring_supported);
        // 后台线程退出时它的线程局部 ring 会从全局表摘除：必须在 service
        // 析构前采样，否则 fire 事件会随线程 ring 一起消失。
        records = EventRing::Snapshot(0);
        events_dump = go2cpp::debug::DumpEvents(0);
        // DUMP timer 里应能看到最近 timer 事件的完整字段（7 列文本之外的
        // timer_id/kind/deadline_ns/source_gid 语义视图）。
        const DumpPayload live_dump = DumpTimer(service);
        GO2CPP_CHECK(Contains(live_dump.text, "timer_events.count="));
        GO2CPP_CHECK(Contains(live_dump.text, "timer_event.0.timer_id="));
        GO2CPP_CHECK(Contains(live_dump.text, "timer_event.0.kind="));
        GO2CPP_CHECK(Contains(live_dump.text, "timer_event.0.deadline_ns="));
        GO2CPP_CHECK(Contains(live_dump.text, "timer_event.0.source_gid=77"));
        GO2CPP_CHECK(Contains(live_dump.json, "\"timer_events\":{"));
        GO2CPP_CHECK(Contains(live_dump.json, "\"timer_id\":"));
    }

    bool found_arm = false;
    bool found_fire = false;
    bool found_cancel = false;
    bool found_reschedule = false;
    for (const auto& event : records) {
        const auto code = static_cast<SchedEventCode>(event.code);
        if (code == SchedEventCode::kTimerArm) {
            found_arm = true;
            GO2CPP_CHECK(event.gid > 0U);
            GO2CPP_CHECK(event.arg2 > 0U);
            GO2CPP_CHECK(event.arg4 == 77U);
        } else if (code == SchedEventCode::kTimerFire) {
            found_fire = true;
            GO2CPP_CHECK(event.gid > 0U);
        } else if (code == SchedEventCode::kTimerCancel) {
            found_cancel = true;
        } else if (code == SchedEventCode::kTimerReschedule) {
            found_reschedule = true;
        }
    }
    (void)found_reschedule;
    GO2CPP_CHECK(found_arm);
    GO2CPP_CHECK(found_fire);
    GO2CPP_CHECK(found_cancel);

    GO2CPP_CHECK(Contains(events_dump.text, "timer_event_count="));
    GO2CPP_CHECK(Contains(events_dump.json, "\"timer_event_count\":"));
    GO2CPP_CHECK(Contains(events_dump.json, "\"timer_id\":"));
    GO2CPP_CHECK(Contains(events_dump.json, "\"timer_kind\":"));
    GO2CPP_CHECK(Contains(events_dump.json, "\"deadline_ns\":"));
    GO2CPP_CHECK(Contains(events_dump.json, "\"source_gid\":"));
    GO2CPP_CHECK(Contains(events_dump.json, "\"kTimerArm\""));
    GO2CPP_CHECK(Contains(events_dump.json, "\"kTimerFire\""));
    GO2CPP_CHECK(Contains(events_dump.text, "kTimerArm"));
#else
    GO2CPP_CHECK(!EventRing::Enabled());
    GO2CPP_CHECK(!EventRing::PublishSeq());
    const DumpPayload events = go2cpp::debug::DumpEvents(0);
    GO2CPP_CHECK(Contains(events.text, "enabled=false"));
#endif
}

// ---------------------------------------------------------------------------
// 场景 6：DebugServer 集成（DUMP timer / JSON timer / LIST）
// ---------------------------------------------------------------------------
void ScenarioDebugServer() {
    TimerService service;
    Timer keep = service.ScheduleAfter(std::chrono::hours(1), [] {});
    DebugServer& server = DebugServer::Instance();
    GO2CPP_REQUIRE(go2cpp::debug::RegisterTimer("timer", service));

    const std::string dump = server.HandleCommand("DUMP timer");
    GO2CPP_CHECK(Contains(dump, "GO2CPP-DEBUG/1 OK "));
    GO2CPP_CHECK(Contains(dump, "schema=go2cpp.dump.timer/1"));
    GO2CPP_CHECK(Contains(dump, "active=1"));

    const std::string json = server.HandleCommand("JSON timer");
    GO2CPP_CHECK(Contains(json, "GO2CPP-DEBUG/1 OK "));
    GO2CPP_CHECK(Contains(json, "\"schema\":\"go2cpp.dump.timer/1\""));

    const std::string list = server.HandleCommand("LIST");
    GO2CPP_CHECK(Contains(list, "timer"));

    GO2CPP_CHECK(server.Unregister("timer"));
}

// ---------------------------------------------------------------------------
// 压测：10k 活动 timer 的 schedule / cancel / dump 延迟
// ---------------------------------------------------------------------------
void ScenarioStress() {
    constexpr std::size_t kTimers = 10000;
    TimerService service;
    std::vector<Timer> timers;
    timers.reserve(kTimers);
    std::vector<std::uint64_t> schedule_ns;
    schedule_ns.reserve(kTimers);
    const auto base =
        std::chrono::steady_clock::now() + std::chrono::hours(1);

    for (std::size_t index = 0; index < kTimers; ++index) {
        const auto started = SteadyNowNs();
        timers.push_back(service.Schedule(
            base + std::chrono::nanoseconds(index), [] {}));
        schedule_ns.push_back(SteadyNowNs() - started);
    }
    const TimerDebugSnapshot after_arm = service.DebugSnapshot();
    GO2CPP_CHECK(after_arm.active == kTimers);
    GO2CPP_CHECK(after_arm.armed_total == kTimers);

    const auto dump_started = SteadyNowNs();
    const DumpPayload payload = DumpTimer(service);
    const std::uint64_t dump_ns = SteadyNowNs() - dump_started;
    GO2CPP_CHECK(Contains(payload.text, "active=10000"));

    std::vector<std::uint64_t> cancel_ns;
    cancel_ns.reserve(kTimers);
    for (std::size_t index = 0; index < kTimers; ++index) {
        const auto started = SteadyNowNs();
        timers[index].Cancel();
        cancel_ns.push_back(SteadyNowNs() - started);
    }
    const TimerDebugSnapshot after_cancel = service.DebugSnapshot();
    GO2CPP_CHECK(after_cancel.active == 0U);
    GO2CPP_CHECK(after_cancel.cancelled >= kTimers);
    GO2CPP_CHECK(after_cancel.armed_total == kTimers);

    PrintPercentiles("schedule", schedule_ns);
    PrintPercentiles("cancel", cancel_ns);
    std::printf("[timer-debug] 10k dump=%lluns\n",
                static_cast<unsigned long long>(dump_ns));
    // 宽松上限：只拦截数量级退化（例如 O(n^2) 或意外 sleep），不是性能基准。
    std::sort(schedule_ns.begin(), schedule_ns.end());
    std::sort(cancel_ns.begin(), cancel_ns.end());
    GO2CPP_CHECK(schedule_ns[schedule_ns.size() * 99U / 100U] <
                 50'000'000ULL);
    GO2CPP_CHECK(cancel_ns[cancel_ns.size() * 99U / 100U] < 50'000'000ULL);
    GO2CPP_CHECK(dump_ns < 2'000'000'000ULL);
}

// ---------------------------------------------------------------------------
// 并发：4 线程并发 dump + 主线程 schedule/cancel
// ---------------------------------------------------------------------------
void ScenarioConcurrentDump() {
    TimerService service;
    std::atomic<bool> stop{false};
    std::atomic<int> dumps{0};
    std::atomic<bool> monotonic_violation{false};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&service, &stop, &dumps,
                              &monotonic_violation] {
            std::uint64_t previous = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const TimerDebugSnapshot snapshot = service.DebugSnapshot();
                if (snapshot.seq <= previous) {
                    monotonic_violation.store(true,
                                              std::memory_order_relaxed);
                }
                previous = snapshot.seq;
                if (snapshot.next_deadline) {
                    // 堆顶 deadline 必须与 active 数一致：空表时不能有值。
                    if (snapshot.active == 0U) {
                        monotonic_violation.store(true,
                                                  std::memory_order_relaxed);
                    }
                }
                dumps.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int round = 0; round < 2000; ++round) {
        Timer timer = service.ScheduleAfter(
            std::chrono::milliseconds(1), [] {});
        if (round % 3 == 0) {
            timer.Cancel();
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }
    GO2CPP_CHECK(!monotonic_violation.load(std::memory_order_relaxed));
    GO2CPP_CHECK(dumps.load(std::memory_order_relaxed) > 0);
    const TimerDebugSnapshot final_snapshot = service.DebugSnapshot();
    GO2CPP_CHECK(final_snapshot.active == 0U);
    GO2CPP_CHECK(final_snapshot.running);
    std::printf("[timer-debug] concurrent dumps=%d active_after=%zu\n",
                dumps.load(std::memory_order_relaxed),
                final_snapshot.active);
}

}  // namespace

void run_timer_debug_tests() {
    go2cpp_tests::announce("timer debug: snapshot/sources/dump/events/stress");

    ScenarioSnapshotAndSources();
    ScenarioDumpFormats();
    ScenarioStats();
    ScenarioEvents();
    ScenarioDebugServer();
    ScenarioStress();
    ScenarioConcurrentDump();

    // Demo 输出：让 `GO2CPP_TEST_FILTER=timer_debug` 的 stdout 直接可当
    // Timer dump demo 使用（报告里会重定向到 /tmp/timer_dump_demo.txt）。
    {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        // demo 只展示本轮的事件：清掉前面压测/并发场景留下的 timer 事件。
        EventRing::Reset();
#endif
        TimerService service;
        std::vector<Timer> handles;
        const auto far =
            std::chrono::steady_clock::now() + std::chrono::hours(1);
        const TimerSource demo_sources[] = {
            TimerSource::kContext, TimerSource::kParkingCondition,
            TimerSource::kChannel, TimerSource::kTimedMutex,
            TimerSource::kScheduler, TimerSource::kSleep};
        std::uint32_t gid = 100;
        for (const TimerSource source : demo_sources) {
            TimerSourceScope scope(source, gid++);
            handles.push_back(service.Schedule(far, [] {}));
        }
        // 周期 timer 提供「最近的真实堆顶」与 reschedule 统计。
        Timer periodic = service.ScheduleEvery(
            std::chrono::milliseconds(50), [] {},
            TimerOptions{TimerRepeatMode::kFixedRate,
                         std::chrono::milliseconds(50)});
        const DumpPayload payload = DumpTimer(service);
        std::printf("[timer-debug] dump demo (%zu bytes):\n%s\n",
                    payload.text.size(), payload.text.c_str());
        periodic.Cancel();
    }
}
