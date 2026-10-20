/**
 * @file test_timer_replay.cpp
 * @brief TimerService 录制 / 回放测试：多 timer、周期、取消、随机 1000、
 *        10k 压测、timer 与 select/channel 竞争。
 *
 * 覆盖（按项目规范：真实场景 + 压测 + 随机 + 并发）：
 *   场景 1 多 timer 同刻到期：回放顺序与记录一致，trace MATCH
 *   场景 2 周期 timer（>=10 次触发）：每次 fire 都重放，trace MATCH
 *   场景 3 取消：取消后不再触发（记录里 cancel 在 fire 前），trace MATCH
 *   场景 4 随机 1000 个 arm/cancel：录制 vs 回放 MATCH
 *   场景 5 timer vs channel：timer 回调向 buffered channel 发送，select 选中
 *          channel（不是 timeout）；回放由 kTimerFire 派发回调，checksum MATCH
 *   压测   10k 活动 timer 的 schedule/cancel 延迟与 20k trace 回放
 */

#include "go2cpp/scheduler.hpp"

#include "replay/auto_entry.hpp"
#include "replay/clock.hpp"
#include "replay/format.hpp"
#include "replay/recorder.hpp"
#include "replay/replay_engine.hpp"
#include "replay/replay_source.hpp"
#include "replay/select.hpp"
#include "replay/sync_channel.hpp"
#include "replay/timer.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;
using go2cpp::replay::Decision;
using go2cpp::replay::Event;
using go2cpp::replay::EventType;
using go2cpp::replay::FileHeader;
using go2cpp::replay::ReplayClock;
using go2cpp::replay::Recorder;
using go2cpp::replay::RecordOptions;
using go2cpp::replay::ReplayOptions;
using go2cpp::replay::ReplayReport;
using go2cpp::replay::ReplaySource;
using go2cpp::replay::ReplayStatus;
using go2cpp::replay::ReplayTimerService;
using go2cpp::replay::ScopedGid;
using go2cpp::replay::SelectGroup;
using go2cpp::replay::SyncChannel;
using go2cpp::replay::SyncSelect;
using go2cpp::replay::TimerKind;
using go2cpp::replay::TimerSource;
using go2cpp::replay::TimerTraceEntry;

constexpr std::uint32_t kTimerSourceGid = 10;
constexpr std::uint64_t kTimerDelayNs = 1'000'000;       // 1ms
constexpr std::uint64_t kSelectTimeoutNs = 200'000'000;  // 200ms

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

std::uint64_t SteadyNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string PathFor(const char* name) {
    return std::string("/tmp/timer_replay_") + std::to_string(::getpid()) +
           "_" + name + ".g2cr";
}

/// 直接从磁盘解码事件（不依赖引擎内部的 LoadFile）。
bool LoadEvents(const std::string& path, std::vector<Event>* events,
                std::string* error) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        if (error != nullptr) *error = "cannot open " + path;
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(file);
        if (error != nullptr) *error = "ftell failed";
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    const std::size_t read =
        std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        if (error != nullptr) *error = "short read";
        return false;
    }
    FileHeader header;
    if (!go2cpp::replay::DecodeFileHeader(bytes.data(), bytes.size(), &header,
                                          error)) {
        return false;
    }
    std::size_t offset = go2cpp::replay::kFileHeaderSize;
    while (offset < bytes.size()) {
        Event event;
        std::size_t consumed = 0;
        if (!go2cpp::replay::DecodeEvent(bytes.data() + offset,
                                         bytes.size() - offset, &event,
                                         &consumed, error)) {
            return false;
        }
        if (consumed == 0) {
            break;
        }
        offset += consumed;
        if (event.type == EventType::kEnd) {
            break;
        }
        events->push_back(std::move(event));
    }
    return true;
}

/// 纯回放用的最小 ReplaySource（没有 fiber 调度，只驱动 timer）。
class FileSource final : public ReplaySource {
public:
    explicit FileSource(std::vector<Event> events)
        : m_events(std::move(events)) {}

    const Event* NextOperation(std::uint32_t) override { return nullptr; }
    std::uint64_t VirtualNowNs() const noexcept override {
        return m_virtual_now;
    }
    void AdvanceVirtualTime(std::uint64_t virtual_ns) override {
        if (virtual_ns > m_virtual_now) {
            m_virtual_now = virtual_ns;
        }
    }
    std::uint32_t CurrentFiberGid() const noexcept override { return m_gid; }
    void ParkCurrent(std::uint32_t, const Event&) override {}
    void RecordResult(std::uint32_t, const Event&, std::int64_t,
                      const std::string&) override {}
    const char* LastError() const override { return ""; }
    const std::vector<Event>& Events() const override { return m_events; }
    void InstallTimerService(ReplayTimerService* service) override {
        m_timer = service;
    }
    ReplayTimerService* TimerService() override { return m_timer; }
    void SetCurrentGidOverride(std::uint32_t gid) override {
        m_gid = gid == go2cpp::replay::kNoChannel ? 0U : gid;
    }

private:
    std::vector<Event> m_events;
    std::uint64_t m_virtual_now{0};
    std::uint32_t m_gid{0};
    ReplayTimerService* m_timer{nullptr};
};

bool CompareOrReport(const std::vector<TimerTraceEntry>& recorded,
                     const std::vector<TimerTraceEntry>& replayed,
                     const char* tag) {
    std::string difference;
    if (ReplayTimerService::CompareTraces(recorded, replayed, &difference)) {
        std::printf("[timer-replay] %s: MATCH (%zu entries)\n", tag,
                    recorded.size());
        return true;
    }
    std::printf("[timer-replay] %s: MISMATCH: %s\n", tag,
                difference.c_str());
    return false;
}

// ---------------------------------------------------------------------------
// 场景 1：多 timer 同刻到期
// ---------------------------------------------------------------------------
struct Recording {
    std::vector<TimerTraceEntry> trace;
    std::vector<std::uint32_t> fired_order;
    std::size_t expected_fires{0};
};

void RecordSameDeadline(const std::string& path, Recording* recording) {
    RecordOptions options;
    options.path = path;
    options.demo_name = "timer-same-deadline";
    options.gid_count = 4;
    options.channel_count = 1;
    Recorder recorder(options);
    GO2CPP_REQUIRE(recorder.ok());
    {
        ReplayClock clock(recorder);
        std::mutex mutex;
        std::vector<std::uint32_t> fired;
        {
            ReplayTimerService timers(recorder, clock);
            const std::uint64_t deadline = clock.NowNs() + 2'000'000;
            for (std::uint32_t index = 0; index < 3; ++index) {
                const std::uint32_t id = timers.ScheduleAt(
                    deadline,
                    [&, index] {
                        std::lock_guard<std::mutex> lock(mutex);
                        fired.push_back(index);
                        return std::int64_t{0};
                    },
                    TimerSource::kUser, index);
                GO2CPP_REQUIRE(id != 0);
            }
            GO2CPP_REQUIRE(WaitFor(
                [&] {
                    std::lock_guard<std::mutex> lock(mutex);
                    return fired.size() >= 3U;
                },
                std::chrono::milliseconds(3000)));
            recording->trace = timers.trace();
        }
        recording->fired_order = fired;
        recording->expected_fires = 3;
    }
    recorder.Finalize("completed");
}

void ReplaySameDeadline(const std::string& path, const Recording& recording) {
    std::vector<Event> events;
    std::string error;
    GO2CPP_REQUIRE(LoadEvents(path, &events, &error));
    FileSource source(std::move(events));
    ReplayTimerService timers(source);
    std::vector<std::uint32_t> fired;
    const std::uint64_t deadline = source.VirtualNowNs() + 2'000'000;
    for (std::uint32_t index = 0; index < 3; ++index) {
        const std::uint32_t id = timers.ScheduleAt(
            deadline,
            [&, index] {
                fired.push_back(index);
                return std::int64_t{0};
            },
            TimerSource::kUser, index);
        GO2CPP_REQUIRE(id != 0);
    }
    const std::size_t count = timers.ReplayAll();
    GO2CPP_CHECK(count == recording.expected_fires);
    GO2CPP_CHECK(fired == recording.fired_order);
    GO2CPP_CHECK(timers.fired_count() == recording.expected_fires);
    GO2CPP_CHECK(CompareOrReport(recording.trace, timers.trace(),
                                 "same-deadline"));
    GO2CPP_CHECK(timers.error().empty());
    timers.Detach();
}

// ---------------------------------------------------------------------------
// 场景 2：周期 timer（>=10 次）
// ---------------------------------------------------------------------------
void RecordPeriodic(const std::string& path, Recording* recording) {
    RecordOptions options;
    options.path = path;
    options.demo_name = "timer-periodic";
    options.gid_count = 2;
    Recorder recorder(options);
    GO2CPP_REQUIRE(recorder.ok());
    {
        ReplayClock clock(recorder);
        std::atomic<int> fired{0};
        {
            ReplayTimerService timers(recorder, clock);
            const std::uint32_t id = timers.ScheduleEvery(
                std::chrono::milliseconds(1).count() * 1'000'000ULL,
                [&fired] {
                    fired.fetch_add(1, std::memory_order_relaxed);
                    return std::int64_t{0};
                },
                TimerSource::kScheduler, 0);
            GO2CPP_REQUIRE(id != 0);
            GO2CPP_REQUIRE(WaitFor(
                [&fired] {
                    return fired.load(std::memory_order_relaxed) >= 10;
                },
                std::chrono::milliseconds(3000)));
            GO2CPP_CHECK(timers.Cancel(id));
            recording->trace = timers.trace();
        }
        recording->expected_fires =
            static_cast<std::size_t>(fired.load(std::memory_order_relaxed));
    }
    recorder.Finalize("completed");
}

void ReplayPeriodic(const std::string& path, const Recording& recording) {
    std::vector<Event> events;
    std::string error;
    GO2CPP_REQUIRE(LoadEvents(path, &events, &error));
    FileSource source(std::move(events));
    ReplayTimerService timers(source);
    std::atomic<int> fired{0};
    const std::uint32_t id = timers.ScheduleEvery(
        std::chrono::milliseconds(1).count() * 1'000'000ULL,
        [&fired] {
            fired.fetch_add(1, std::memory_order_relaxed);
            return std::int64_t{0};
        },
        TimerSource::kScheduler, 0);
    GO2CPP_REQUIRE(id != 0);
    GO2CPP_CHECK(timers.Cancel(id));  // 记录里有 cancel
    const std::size_t count = timers.ReplayAll();
    GO2CPP_CHECK(count >= 10U);
    GO2CPP_CHECK(static_cast<std::size_t>(
                     fired.load(std::memory_order_relaxed)) ==
                 recording.expected_fires);
    GO2CPP_CHECK(CompareOrReport(recording.trace, timers.trace(), "periodic"));
    GO2CPP_CHECK(timers.error().empty());
    timers.Detach();
}

// ---------------------------------------------------------------------------
// 场景 3：取消后不再触发
// ---------------------------------------------------------------------------
void RecordCancel(const std::string& path, Recording* recording) {
    RecordOptions options;
    options.path = path;
    options.demo_name = "timer-cancel";
    options.gid_count = 2;
    Recorder recorder(options);
    GO2CPP_REQUIRE(recorder.ok());
    {
        ReplayClock clock(recorder);
        std::atomic<int> far_fired{0};
        std::atomic<int> near_fired{0};
        {
            ReplayTimerService timers(recorder, clock);
            const std::uint32_t far_id = timers.ScheduleAt(
                clock.NowNs() + 50'000'000,
                [&far_fired] {
                    far_fired.fetch_add(1, std::memory_order_relaxed);
                    return std::int64_t{0};
                },
                TimerSource::kUser, 0);
            GO2CPP_REQUIRE(far_id != 0);
            GO2CPP_CHECK(timers.Cancel(far_id));
            const std::uint32_t near_id = timers.ScheduleAt(
                clock.NowNs() + 2'000'000,
                [&near_fired] {
                    near_fired.fetch_add(1, std::memory_order_relaxed);
                    return std::int64_t{0};
                },
                TimerSource::kUser, 0);
            GO2CPP_REQUIRE(near_id != 0);
            GO2CPP_REQUIRE(WaitFor(
                [&near_fired] {
                    return near_fired.load(std::memory_order_relaxed) >= 1;
                },
                std::chrono::milliseconds(3000)));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            GO2CPP_CHECK(far_fired.load(std::memory_order_relaxed) == 0);
            recording->trace = timers.trace();
        }
        recording->expected_fires = 1;
    }
    recorder.Finalize("completed");
}

void ReplayCancel(const std::string& path, const Recording& recording) {
    std::vector<Event> events;
    std::string error;
    GO2CPP_REQUIRE(LoadEvents(path, &events, &error));
    FileSource source(std::move(events));
    ReplayTimerService timers(source);
    std::atomic<int> far_fired{0};
    std::atomic<int> near_fired{0};
    const std::uint32_t far_id = timers.ScheduleAt(
        source.VirtualNowNs() + 50'000'000,
        [&far_fired] {
            far_fired.fetch_add(1, std::memory_order_relaxed);
            return std::int64_t{0};
        },
        TimerSource::kUser, 0);
    GO2CPP_REQUIRE(far_id != 0);
    GO2CPP_CHECK(timers.Cancel(far_id));
    const std::uint32_t near_id = timers.ScheduleAt(
        source.VirtualNowNs() + 2'000'000,
        [&near_fired] {
            near_fired.fetch_add(1, std::memory_order_relaxed);
            return std::int64_t{0};
        },
        TimerSource::kUser, 0);
    GO2CPP_REQUIRE(near_id != 0);
    const std::size_t count = timers.ReplayAll();
    GO2CPP_CHECK(count == recording.expected_fires);
    GO2CPP_CHECK(far_fired.load(std::memory_order_relaxed) == 0);
    GO2CPP_CHECK(near_fired.load(std::memory_order_relaxed) == 1);
    GO2CPP_CHECK(CompareOrReport(recording.trace, timers.trace(), "cancel"));
    GO2CPP_CHECK(timers.error().empty());
    timers.Detach();
}

// ---------------------------------------------------------------------------
// 场景 4：随机 1000 个 arm/cancel 序列
// ---------------------------------------------------------------------------
struct RandomOp {
    bool cancel{false};
    std::uint64_t deadline_ns{0};
};

std::vector<RandomOp> MakeRandomOps(std::uint32_t seed, std::uint64_t base_ns) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> kind(0, 9);
    std::uniform_int_distribution<std::uint64_t> far(70'000'000, 100'000'000);
    std::vector<RandomOp> ops;
    ops.reserve(1000);
    for (std::size_t index = 0; index < 1000; ++index) {
        RandomOp op;
        op.cancel = kind(rng) < 3;  // 30% 取消
        op.deadline_ns =
            op.cancel
                ? base_ns + far(rng)
                : base_ns + 10'000'000 +
                      static_cast<std::uint64_t>(index % 40) * 1'000'000;
        ops.push_back(op);
    }
    return ops;
}

void RecordRandom(const std::string& path, std::uint32_t seed,
                  Recording* recording) {
    RecordOptions options;
    options.path = path;
    options.demo_name = "timer-random";
    options.gid_count = 2;
    Recorder recorder(options);
    GO2CPP_REQUIRE(recorder.ok());
    {
        ReplayClock clock(recorder);
        std::atomic<int> fired{0};
        {
            ReplayTimerService timers(recorder, clock);
            const auto ops = MakeRandomOps(seed, clock.NowNs());
            std::size_t expected = 0;
            for (const auto& op : ops) {
                const std::uint32_t id = timers.ScheduleAt(
                    op.deadline_ns,
                    [&fired] {
                        fired.fetch_add(1, std::memory_order_relaxed);
                        return std::int64_t{1};
                    },
                    TimerSource::kUser, 0);
                GO2CPP_REQUIRE(id != 0);
                if (op.cancel) {
                    GO2CPP_REQUIRE(timers.Cancel(id));
                } else {
                    ++expected;
                }
            }
            GO2CPP_REQUIRE(WaitFor(
                [&fired, expected] {
                    return static_cast<std::size_t>(
                               fired.load(std::memory_order_relaxed)) ==
                           expected;
                },
                std::chrono::milliseconds(5000)));
            recording->trace = timers.trace();
            recording->expected_fires = expected;
        }
    }
    recorder.Finalize("completed");
}

void ReplayRandom(const std::string& path, std::uint32_t seed,
                  const Recording& recording) {
    std::vector<Event> events;
    std::string error;
    GO2CPP_REQUIRE(LoadEvents(path, &events, &error));
    FileSource source(std::move(events));
    ReplayTimerService timers(source);
    std::atomic<int> fired{0};
    const auto ops = MakeRandomOps(seed, source.VirtualNowNs());
    for (const auto& op : ops) {
        const std::uint32_t id = timers.ScheduleAt(
            op.deadline_ns,
            [&fired] {
                fired.fetch_add(1, std::memory_order_relaxed);
                return std::int64_t{1};
            },
            TimerSource::kUser, 0);
        GO2CPP_REQUIRE(id != 0);
        if (op.cancel) {
            GO2CPP_REQUIRE(timers.Cancel(id));
        }
    }
    const std::size_t count = timers.ReplayAll();
    GO2CPP_CHECK(count == recording.expected_fires);
    GO2CPP_CHECK(static_cast<std::size_t>(
                     fired.load(std::memory_order_relaxed)) ==
                 recording.expected_fires);
    GO2CPP_CHECK(CompareOrReport(recording.trace, timers.trace(), "random-1000"));
    GO2CPP_CHECK(timers.error().empty());
    timers.Detach();
}

// ---------------------------------------------------------------------------
// 场景 5：timer 回调向 channel 发送，select 选中 channel（不是 timeout）
// ---------------------------------------------------------------------------
std::unique_ptr<SyncChannel<std::uint64_t>> g_engine_channel;
std::atomic<int> g_engine_timer_fired{0};
int g_engine_chosen{-99};
std::uint64_t g_engine_value{0};

void TimerSelectEntry(std::uint32_t gid, ReplaySource& source) {
    if (gid != 0) {
        return;
    }
    ScopedGid guard(gid);
    g_engine_channel =
        std::make_unique<SyncChannel<std::uint64_t>>(0, source, 4);
    // timer 服务必须活到引擎遍历完事件：全局持有（Run 结束时引擎会 Detach）。
    static std::unique_ptr<ReplayTimerService> timers;
    timers = std::make_unique<ReplayTimerService>(source);
    const std::uint32_t id = timers->ScheduleAfter(
        kTimerDelayNs,
        [] {
            if (g_engine_channel) {
                g_engine_channel->Send(7);
            }
            g_engine_timer_fired.fetch_add(1, std::memory_order_relaxed);
            return std::int64_t{0};
        },
        TimerSource::kChannel, kTimerSourceGid);
    if (id == 0) {
        std::printf("[timer-replay] engine entry: ScheduleAfter failed: %s\n",
                    timers->error().c_str());
    }
    SyncSelect select(source);
    std::uint64_t value = 0;
    select.AddRecv(*g_engine_channel, &value);
    select.AddTimeout(kSelectTimeoutNs);
    g_engine_chosen = select.Wait();
    g_engine_value = value;
}

void RecordEngineScenario(const std::string& path) {
    RecordOptions options;
    options.path = path;
    options.demo_name = "timer-select";
    options.gid_count = 2;
    options.channel_count = 1;
    Recorder recorder(options);
    GO2CPP_REQUIRE(recorder.ok());
    {
        ReplayClock clock(recorder);
        ReplayTimerService timers(recorder, clock);
        // channel 必须绑定到 select 的组锁：CheckChannel 会校验 group 指针。
        SelectGroup group;
        SyncChannel<std::uint64_t> channel(0, recorder, 4, &group);
        SchedulerConfig config;
        config.processor_count = 2;
        config.max_workers = 2;
        config.min_workers = 1;
        Scheduler scheduler(config);
        scheduler.start();
        std::atomic<int> timer_fired{0};
        std::atomic<int> chosen{-99};
        std::uint64_t received = 0;
        auto task = scheduler.spawn([&] {
            ScopedGid guard(0);
            // 录制端必须显式写 kSpawn/kExit：回放引擎靠它们创建/结束 fiber。
            Event spawn;
            spawn.type = EventType::kSpawn;
            spawn.gid = 0;
            recorder.Write(spawn);
            const std::uint32_t id = timers.ScheduleAfter(
                kTimerDelayNs,
                [&channel, &timer_fired] {
                    const Decision decision = channel.Send(7);
                    timer_fired.fetch_add(1, std::memory_order_relaxed);
                    return decision == Decision::kOk ? std::int64_t{0}
                                                     : std::int64_t{-1};
                },
                TimerSource::kChannel, kTimerSourceGid);
            if (id == 0) {
                std::printf("[timer-replay] record: ScheduleAfter failed\n");
            }
            SyncSelect select(recorder, clock, group);
            std::uint64_t value = 0;
            const int recv_case = select.AddRecv(channel, &value);
            const int timeout_case = select.AddTimeout(kSelectTimeoutNs);
            const int case_index = select.Wait();
            std::printf("[timer-replay] record select: recv_case=%d "
                        "timeout_case=%d chosen=%d value=%llu error=%s\n",
                        recv_case, timeout_case, case_index,
                        static_cast<unsigned long long>(value),
                        select.error().c_str());
            chosen.store(case_index);
            received = value;
            Event exit;
            exit.type = EventType::kExit;
            exit.gid = 0;
            exit.value = 0;
            // 本 fiber 只做了一次 select-recv；静态分析按 kSelect 的 recv
            // case 统计 recvs，因此这里必须与之一致（否则 exit counters
            // mismatch 会让完整重放直接跳过）。
            exit.aux64 = received == 7 ? 1 : 0;
            recorder.Write(exit);
        });
        (void)task->wait();
        scheduler.shutdown();
        GO2CPP_REQUIRE(WaitFor(
            [&timer_fired] {
                return timer_fired.load(std::memory_order_relaxed) >= 1;
            },
            std::chrono::milliseconds(3000)));
        std::printf("[timer-replay] record engine: chosen=%d received=%llu "
                    "timer_fired=%d\n",
                    chosen.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(received),
                    timer_fired.load(std::memory_order_relaxed));
        GO2CPP_CHECK(chosen.load(std::memory_order_relaxed) >= 0);
        GO2CPP_CHECK(received == 7);
    }
    recorder.Finalize("completed");
}

void ReplayEngineScenario(const std::string& path, std::uint64_t* recorded_timeout_ns) {
    ReplayOptions options;
    options.keep_timeline = true;
    const ReplayReport report =
        go2cpp::replay::ReplayFileAuto(path, options);
    if (report.status != ReplayStatus::kOk) {
        std::vector<Event> raw;
        std::string load_error;
        if (LoadEvents(path, &raw, &load_error)) {
            for (const auto& event : raw) {
                std::printf("[timer-replay] event seq=%llu type=%s gid=%u "
                            "kind=%u timer_id=%u deadline=%llu fired=%llu "
                            "blocked=%u\n",
                            static_cast<unsigned long long>(event.seq),
                            go2cpp::replay::EventTypeName(event.type),
                            event.gid,
                            static_cast<unsigned>(event.timer_kind),
                            event.timer_id,
                            static_cast<unsigned long long>(
                                event.deadline_ns),
                            static_cast<unsigned long long>(
                                event.fired_at_ns),
                            static_cast<unsigned>(event.blocked));
            }
        } else {
            std::printf("[timer-replay] load failed: %s\n",
                        load_error.c_str());
        }
    }
    if (report.status != ReplayStatus::kOk) {
    for (std::size_t index = 0;
         index < report.timeline.size() && index < 40; ++index) {
        const auto& entry = report.timeline[index];
        std::printf("[timer-replay] timeline[%zu] seq=%llu type=%s gid=%u "
                    "blocked=%u chosen=%u reason=%d\n",
                    index,
                    static_cast<unsigned long long>(entry.seq),
                    go2cpp::replay::EventTypeName(entry.type),
                    entry.gid, static_cast<unsigned>(entry.blocked),
                    static_cast<unsigned>(entry.chosen),
                    static_cast<int>(entry.reason));
    }
    }
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    if (report.status != ReplayStatus::kOk) {
        std::printf("[timer-replay] engine report: status=%s error=%s\n",
                    go2cpp::replay::ReplayStatusName(report.status),
                    report.error.c_str());
    }
    GO2CPP_CHECK(report.stats.timer_arms == 1U);
    GO2CPP_CHECK(report.stats.timer_fires == 1U);
    GO2CPP_CHECK(report.stats.timer_cancels <= report.stats.timer_arms);
    GO2CPP_CHECK(report.stats.timer_checksum ==
                 report.stats.replayed_timer_checksum);
    GO2CPP_CHECK(g_engine_timer_fired.load(std::memory_order_relaxed) == 1);
    GO2CPP_CHECK(g_engine_chosen == 0);  // 选中 channel 的 recv case
    GO2CPP_CHECK(g_engine_value == 7);
    if (recorded_timeout_ns != nullptr) {
        *recorded_timeout_ns = report.stats.timers;
    }
}

// ---------------------------------------------------------------------------
// 压测：10k 活动 timer 的 schedule/cancel + 20k trace 回放
// ---------------------------------------------------------------------------
void ScenarioStress10k() {
    constexpr std::size_t kTimers = 10000;
    const std::string path = PathFor("stress10k");
    Recording recording;

    std::uint64_t record_ns = 0;
    std::vector<std::uint64_t> schedule_ns;
    std::vector<std::uint64_t> cancel_ns;
    {
        RecordOptions options;
        options.path = path;
        options.demo_name = "timer-stress10k";
        options.gid_count = 2;
        Recorder recorder(options);
        GO2CPP_REQUIRE(recorder.ok());
        {
            ReplayClock clock(recorder);
            ReplayTimerService timers(recorder, clock);
            const std::uint64_t base = clock.NowNs() + 3'600'000'000'000ULL;
            std::vector<std::uint32_t> ids;
            ids.reserve(kTimers);
            schedule_ns.reserve(kTimers);
            cancel_ns.reserve(kTimers);
            for (std::size_t index = 0; index < kTimers; ++index) {
                const auto started = SteadyNowNs();
                const std::uint32_t id = timers.ScheduleAt(
                    base + static_cast<std::uint64_t>(index), [] {
                        return std::int64_t{0};
                    });
                GO2CPP_REQUIRE(id != 0);
                schedule_ns.push_back(SteadyNowNs() - started);
                ids.push_back(id);
            }
            const auto cancel_started = SteadyNowNs();
            for (const std::uint32_t id : ids) {
                const auto started = SteadyNowNs();
                GO2CPP_REQUIRE(timers.Cancel(id));
                cancel_ns.push_back(SteadyNowNs() - started);
            }
            record_ns = SteadyNowNs() - cancel_started;
            recording.trace = timers.trace();
        }
        recorder.Finalize("completed");
    }

    std::vector<Event> events;
    std::string error;
    GO2CPP_REQUIRE(LoadEvents(path, &events, &error));
    FileSource source(std::move(events));
    ReplayTimerService timers(source);
    std::vector<std::uint32_t> ids;
    ids.reserve(kTimers);
    const std::uint64_t base = source.VirtualNowNs() + 3'600'000'000'000ULL;
    const auto replay_started = SteadyNowNs();
    for (std::size_t index = 0; index < kTimers; ++index) {
        const std::uint32_t id = timers.ScheduleAt(
            base + static_cast<std::uint64_t>(index),
            [] { return std::int64_t{0}; });
        GO2CPP_REQUIRE(id != 0);
        ids.push_back(id);
    }
    for (const std::uint32_t id : ids) {
        GO2CPP_REQUIRE(timers.Cancel(id));
    }
    const std::size_t count = timers.ReplayAll();
    const std::uint64_t replay_ns = SteadyNowNs() - replay_started;
    GO2CPP_CHECK(count == 0U);
    GO2CPP_CHECK(CompareOrReport(recording.trace, timers.trace(), "stress10k"));
    GO2CPP_CHECK(timers.error().empty());
    timers.Detach();

    std::sort(schedule_ns.begin(), schedule_ns.end());
    std::sort(cancel_ns.begin(), cancel_ns.end());
    const auto percentile = [](const std::vector<std::uint64_t>& samples,
                               double fraction) {
        return samples[static_cast<std::size_t>(
            fraction * static_cast<double>(samples.size() - 1))];
    };
    std::printf("[timer-replay] stress10k: schedule p50=%lluns p95=%lluns "
                "p99=%lluns; cancel p50=%lluns p95=%lluns p99=%lluns; "
                "replay=%lluns; record_total=%lluns; trace=%zu\n",
                static_cast<unsigned long long>(percentile(schedule_ns, 0.50)),
                static_cast<unsigned long long>(percentile(schedule_ns, 0.95)),
                static_cast<unsigned long long>(percentile(schedule_ns, 0.99)),
                static_cast<unsigned long long>(percentile(cancel_ns, 0.50)),
                static_cast<unsigned long long>(percentile(cancel_ns, 0.95)),
                static_cast<unsigned long long>(percentile(cancel_ns, 0.99)),
                static_cast<unsigned long long>(replay_ns),
                static_cast<unsigned long long>(record_ns),
                recording.trace.size());
    // 宽松上限：拦截数量级退化（O(n^2) / 意外 sleep），不是性能基准。
    GO2CPP_CHECK(percentile(schedule_ns, 0.99) < 50'000'000ULL);
    GO2CPP_CHECK(percentile(cancel_ns, 0.99) < 50'000'000ULL);
    GO2CPP_CHECK(replay_ns < 5'000'000'000ULL);
    (void)std::remove(path.c_str());
}

}  // namespace

RECORD_FIBER_ENTRY_NAMED("timer-select", TimerSelectEntry);

void run_timer_replay_tests() {
    go2cpp_tests::announce(
        "timer replay: same-deadline/periodic/cancel/random/select/stress");

    // 场景 1
    {
        const std::string path = PathFor("same_deadline");
        Recording recording;
        RecordSameDeadline(path, &recording);
        ReplaySameDeadline(path, recording);
        (void)std::remove(path.c_str());
    }
    // 场景 2
    {
        const std::string path = PathFor("periodic");
        Recording recording;
        RecordPeriodic(path, &recording);
        ReplayPeriodic(path, recording);
        (void)std::remove(path.c_str());
    }
    // 场景 3
    {
        const std::string path = PathFor("cancel");
        Recording recording;
        RecordCancel(path, &recording);
        ReplayCancel(path, recording);
        (void)std::remove(path.c_str());
    }
    // 场景 4：随机 1000（固定种子，录制与回放生成同一序列）
    {
        constexpr std::uint32_t kSeed = 20261020U;
        const std::string path = PathFor("random1000");
        Recording recording;
        RecordRandom(path, kSeed, &recording);
        ReplayRandom(path, kSeed, recording);
        (void)std::remove(path.c_str());
    }
    // 场景 5：timer vs select/channel
    {
        const std::string path = PathFor("engine");
        g_engine_timer_fired.store(0, std::memory_order_relaxed);
        g_engine_chosen = -99;
        g_engine_value = 0;
        RecordEngineScenario(path);
        std::uint64_t timers = 0;
        ReplayEngineScenario(path, &timers);
        (void)std::remove(path.c_str());
    }
    // 压测
    ScenarioStress10k();
}
