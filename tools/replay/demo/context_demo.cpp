/**
 * @file context_demo.cpp
 * @brief Context / 超时 / 虚拟时钟：录制端真实等待，重放端只用虚拟时钟。
 *
 * 时间线（虚拟时间）：
 *   0ms    consumer phase1: select { <-ch, <-ctxA.Done() }，ctxA deadline=30ms
 *   30ms   ctxA deadline 到期 → phase1 返回 timed-out
 *   30ms   consumer phase2: select { <-never, <-ctxB.Done() }
 *   50ms   canceler 调用 ctxB.Cancel() → phase2 返回 cancelled
 *   100ms  producer sleep 结束，向 ch 发送 42
 *   100ms  consumer phase3 的 Recv 收到 42，全部 fiber 退出
 *
 * 录制端真实耗时约 100ms；重放端 `SleepFor` 直接推进虚拟时钟，真实耗时
 * 应远小于 100ms —— 这就是「不依赖真实时钟」的验收点。
 *
 * 用法：
 * ```
 * ./go2cpp_replay_context record /tmp/context.g2cr
 * ./go2cpp_replay_context replay /tmp/context.g2cr
 * ```
 */

#include "go2cpp/scheduler.hpp"

#include "replay/auto_entry.hpp"
#include "replay/clock.hpp"
#include "replay/context.hpp"
#include "replay/recorder.hpp"
#include "replay/select.hpp"
#include "replay/sync_channel.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace demo_context {

using go2cpp::replay::Decision;

inline constexpr std::uint32_t kProducerGid = 0;
inline constexpr std::uint32_t kCancelerGid = 1;
inline constexpr std::uint32_t kConsumerGid = 2;
inline constexpr std::uint32_t kFiberCount = 3;
inline constexpr std::uint32_t kChannel = 0;
inline constexpr std::uint32_t kNeverChannel = 1;
inline constexpr std::uint32_t kCtxA = 1;
inline constexpr std::uint32_t kCtxB = 2;
inline constexpr std::uint64_t kCtxDeadlineNs = 30ULL * 1000ULL * 1000ULL;
inline constexpr std::uint64_t kCancelDelayNs = 50ULL * 1000ULL * 1000ULL;
inline constexpr std::uint64_t kProduceDelayNs = 100ULL * 1000ULL * 1000ULL;
inline constexpr std::uint64_t kValue = 42;

/// consumer 的观测结果。
struct Result {
    void SetPhase1(Decision decision, int chosen, int ctx_case) {
        std::lock_guard<std::mutex> lock(mutex);
        phase1 = decision;
        phase1_chosen = chosen;
        phase1_ctx_case = ctx_case;
    }
    void SetPhase2(Decision decision, int chosen) {
        std::lock_guard<std::mutex> lock(mutex);
        phase2 = decision;
        phase2_chosen = chosen;
    }
    void SetPhase3(bool ok, std::uint64_t value) {
        std::lock_guard<std::mutex> lock(mutex);
        phase3_ok = ok;
        phase3_value = value;
    }
    std::string Describe() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::string("phase1=") +
               go2cpp::replay::DecisionName(phase1) +
               " phase2=" + go2cpp::replay::DecisionName(phase2) +
               " phase3=" + (phase3_ok ? "ok" : "failed") +
               " value=" + std::to_string(phase3_value);
    }

    std::mutex mutex;
    Decision phase1{Decision::kOk};
    Decision phase2{Decision::kOk};
    int phase1_chosen{-1};
    int phase1_ctx_case{-1};
    int phase2_chosen{-1};
    bool phase3_ok{false};
    std::uint64_t phase3_value{0};
};

inline Result& SharedResult() {
    static Result result;
    return result;
}

std::uint64_t WallNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace demo_context

namespace {

/// 录制端 consumer。
void ConsumerLive(go2cpp::replay::Recorder& recorder,
                  go2cpp::replay::ReplayClock& clock,
                  go2cpp::replay::SelectGroup& group,
                  go2cpp::replay::SyncChannel<std::uint64_t>& channel,
                  go2cpp::replay::SyncChannel<std::uint64_t>& never,
                  go2cpp::replay::ReplayContext& ctx_a,
                  go2cpp::replay::ReplayContext& ctx_b) {
    using namespace go2cpp::replay;
    using namespace demo_context;
    ctx_a.SetDeadline(clock.NowNs() + kCtxDeadlineNs);
    {
        std::uint64_t value = 0;
        SyncSelect select(recorder, clock, group);
        const int recv_case = select.AddRecv(channel, &value);
        const int ctx_case = select.AddContext(ctx_a);
        const int chosen = select.Wait();
        (void)recv_case;
        SharedResult().SetPhase1(select.decision(), chosen, ctx_case);
    }
    {
        std::uint64_t value = 0;
        SyncSelect select(recorder, clock, group);
        (void)select.AddRecv(never, &value);
        const int ctx_case = select.AddContext(ctx_b);
        const int chosen = select.Wait();
        (void)ctx_case;
        SharedResult().SetPhase2(select.decision(), chosen);
    }
    {
        auto outcome = channel.Recv();
        SharedResult().SetPhase3(outcome.ok(),
                                 static_cast<std::uint64_t>(outcome.value));
    }
}

/// 重放端 consumer（逻辑与录制端一致，只是 channel/ctx 来自记录）。
void ConsumerReplay(go2cpp::replay::ReplaySource& source,
                    go2cpp::replay::ReplayClock& clock,
                    go2cpp::replay::SyncChannel<std::uint64_t>& channel,
                    go2cpp::replay::SyncChannel<std::uint64_t>& never,
                    go2cpp::replay::ReplayContext& ctx_a,
                    go2cpp::replay::ReplayContext& ctx_b) {
    using namespace go2cpp::replay;
    using namespace demo_context;
    ctx_a.SetDeadline(clock.NowNs() + kCtxDeadlineNs);
    {
        std::uint64_t value = 0;
        SyncSelect select(source);
        (void)select.AddRecv(channel, &value);
        const int ctx_case = select.AddContext(ctx_a);
        const int chosen = select.Wait();
        SharedResult().SetPhase1(select.decision(), chosen, ctx_case);
    }
    {
        std::uint64_t value = 0;
        SyncSelect select(source);
        (void)select.AddRecv(never, &value);
        const int ctx_case = select.AddContext(ctx_b);
        const int chosen = select.Wait();
        (void)ctx_case;
        SharedResult().SetPhase2(select.decision(), chosen);
    }
    {
        auto outcome = channel.Recv();
        SharedResult().SetPhase3(outcome.ok(),
                                 static_cast<std::uint64_t>(outcome.value));
    }
}

void ContextEntry(std::uint32_t gid, go2cpp::replay::ReplaySource& source) {
    using namespace go2cpp::replay;
    using namespace demo_context;
    ScopedGid guard(gid);
    ReplayClock clock(source);
    SyncChannel<std::uint64_t> channel(kChannel, source);
    SyncChannel<std::uint64_t> never(kNeverChannel, source);
    ReplayContext ctx_a(source, kCtxA);
    ReplayContext ctx_b(source, kCtxB);
    if (gid == kProducerGid) {
        clock.SleepFor(kProduceDelayNs);
        (void)channel.Send(kValue);
    } else if (gid == kCancelerGid) {
        clock.SleepFor(kCancelDelayNs);
        ctx_b.Cancel();
    } else {
        ConsumerReplay(source, clock, channel, never, ctx_a, ctx_b);
    }
}

}  // namespace

RECORD_FIBER_ENTRY_NAMED("context-timeout", ContextEntry);

namespace {

std::uint32_t EnvU32(const char* name, std::uint32_t fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return fallback;
    }
    return static_cast<std::uint32_t>(std::strtoul(raw, nullptr, 10));
}

int RunRecord(const char* path) {
    using namespace go2cpp;
    using namespace go2cpp::replay;
    using namespace demo_context;

    RecordOptions options;
    options.path = path;
    options.demo_name = "context-timeout";
    options.gid_count = kFiberCount;
    options.channel_count = 2;
    options.ctx_count = 4;
    options.value_kind = 0;
    options.drop_wake_at = EnvU32("GO2CPP_REPLAY_DROP_WAKE", 0);

    Recorder recorder(options);
    if (!recorder.ok()) {
        std::fprintf(stderr, "[context/record] %s\n", recorder.error().c_str());
        return 1;
    }
    const std::uint64_t started_ns = WallNowNs();
    {
        Scheduler scheduler(3);
        scheduler.start();
        SelectGroup group;
        ReplayClock clock(recorder);
        SyncChannel<std::uint64_t> channel(kChannel, recorder, 0, &group);
        SyncChannel<std::uint64_t> never(kNeverChannel, recorder, 0, &group);
        ReplayContext ctx_a(recorder, clock, kCtxA);
        ReplayContext ctx_b(recorder, clock, kCtxB);

        std::vector<std::shared_ptr<Task>> tasks;
        tasks.push_back(scheduler.spawn([&] {
            ScopedFiberGid guard(kProducerGid);
            Event spawn;
            spawn.type = EventType::kSpawn;
            spawn.gid = kProducerGid;
            recorder.Write(spawn);
            clock.SleepFor(kProduceDelayNs);
            const Decision decision = channel.Send(kValue);
            Event exit;
            exit.type = EventType::kExit;
            exit.gid = kProducerGid;
            exit.value = decision == Decision::kOk ? 1 : 0;
            recorder.Write(exit);
        }));
        tasks.push_back(scheduler.spawn([&] {
            ScopedFiberGid guard(kCancelerGid);
            Event spawn;
            spawn.type = EventType::kSpawn;
            spawn.gid = kCancelerGid;
            recorder.Write(spawn);
            clock.SleepFor(kCancelDelayNs);
            ctx_b.Cancel();
            Event exit;
            exit.type = EventType::kExit;
            exit.gid = kCancelerGid;
            exit.value = 0;
            recorder.Write(exit);
        }));
        tasks.push_back(scheduler.spawn([&] {
            ScopedFiberGid guard(kConsumerGid);
            Event spawn;
            spawn.type = EventType::kSpawn;
            spawn.gid = kConsumerGid;
            recorder.Write(spawn);
            ConsumerLive(recorder, clock, group, channel, never, ctx_a, ctx_b);
            Event exit;
            exit.type = EventType::kExit;
            exit.gid = kConsumerGid;
            exit.value = 0;
            exit.aux64 = SharedResult().phase3_ok ? 1 : 0;
            recorder.Write(exit);
        }));
        for (auto& task : tasks) {
            (void)task->wait();
        }
        scheduler.shutdown();
    }
    recorder.Finalize("completed");
    const std::uint64_t elapsed_ns = WallNowNs() - started_ns;

    std::printf("[context/record] path=%s wall_ms=%.3f\n", path,
                static_cast<double>(elapsed_ns) / 1e6);
    std::printf("[context/record] %s\n", SharedResult().Describe().c_str());
    const bool ok = SharedResult().phase1 == Decision::kTimedOut &&
                    SharedResult().phase2 == Decision::kCancelled &&
                    SharedResult().phase3_ok &&
                    SharedResult().phase3_value == kValue;
    std::printf("[context/record] expected=%s\n", ok ? "MATCH" : "MISMATCH");
    return ok ? 0 : 1;
}

int RunReplay(const char* path) {
    using namespace go2cpp::replay;
    using namespace demo_context;

    ReplayOptions options;
    options.drop_wake_at = EnvU32("GO2CPP_REPLAY_DROP_WAKE", 0);
    const std::uint64_t started_ns = WallNowNs();
    const ReplayReport report = ReplayFileAuto(path, options);
    const std::uint64_t elapsed_ns = WallNowNs() - started_ns;

    std::printf("%s", report.Summary().c_str());
    std::printf("[context/replay] %s\n", SharedResult().Describe().c_str());
    std::printf("[context/replay] wall_ms=%.3f virtual_ms=%.3f timers=%llu ctx=%llu\n",
                static_cast<double>(elapsed_ns) / 1e6,
                static_cast<double>(report.virtual_ns) / 1e6,
                static_cast<unsigned long long>(report.stats.timers),
                static_cast<unsigned long long>(report.stats.ctx_events));
    const bool consistent =
        report.status == ReplayStatus::kOk &&
        SharedResult().phase1 == Decision::kTimedOut &&
        SharedResult().phase2 == Decision::kCancelled &&
        SharedResult().phase3_ok && SharedResult().phase3_value == kValue;
    // 虚拟时钟验收：真实耗时应远小于记录里的虚拟时间（100ms）。
    const bool virtual_clock_ok =
        report.virtual_ns >= 100ULL * 1000ULL * 1000ULL &&
        elapsed_ns < report.virtual_ns / 2ULL;
    std::printf("[context/replay] consistency=%s virtual_clock=%s\n",
                consistent ? "MATCH" : "MISMATCH",
                virtual_clock_ok ? "OK" : "SLOW");
    if (!consistent) {
        std::printf("[context/replay] status=%s error=%s\n",
                    ReplayStatusName(report.status), report.error.c_str());
    }
    return (consistent && virtual_clock_ok) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (std::strcmp(argv[1], "record") != 0 &&
                     std::strcmp(argv[1], "replay") != 0)) {
        std::fprintf(stderr,
                     "usage: %s <record|replay> <recording.g2cr>\n", argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "record") == 0) {
        return RunRecord(argv[2]);
    }
    return RunReplay(argv[2]);
}
