/**
 * @file select_demo.cpp
 * @brief select 多 channel 竞争：录制 / 重放同一个场景。
 *
 * 场景：ch0 有两个 producer（各 24 条），ch1 有一个 producer（48 条）；
 * consumer 每次 `select { case <-ch0; case <-ch1 }` 收一条，共 96 次。
 * consumer 每 8 次 select sleep 0.5ms，让两个 channel 同时有等待中的 sender，
 * 从而产生真正的「多路竞争」决策。
 *
 * 录制端用真实的 `Scheduler` + 组锁 channel；重放端通过
 * `RECORD_FIBER_ENTRY` 自动注册的入口在单线程 ucontext 上重跑，select 的
 * `chosen` 完全来自记录。
 *
 * 用法：
 * ```
 * ./go2cpp_replay_select record /tmp/select.g2cr
 * ./go2cpp_replay_select replay /tmp/select.g2cr
 * ```
 */

#include "go2cpp/scheduler.hpp"

#include "replay/auto_entry.hpp"
#include "replay/clock.hpp"
#include "replay/recorder.hpp"
#include "replay/select.hpp"
#include "replay/sync_channel.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace demo_select {

using go2cpp::replay::Decision;

inline constexpr std::uint32_t kChannel0 = 0;
inline constexpr std::uint32_t kChannel1 = 1;
inline constexpr std::uint32_t kCh0Messages = 48;  // gid 0/1 各 24 条
inline constexpr std::uint32_t kCh1Messages = 48;  // gid 2
inline constexpr std::uint32_t kTotalMessages = kCh0Messages + kCh1Messages;
inline constexpr std::uint32_t kConsumerGid = 3;
inline constexpr std::uint32_t kFiberCount = 4;
inline constexpr std::uint64_t kConsumerSleepNs = 500000;  // 0.5ms

/// consumer 侧观察到的结果：每个消息来自哪个 channel、值、以及 select 决策。
struct Result {
    void Push(std::uint32_t channel, std::uint64_t value, int chosen) {
        std::lock_guard<std::mutex> lock(mutex);
        received.emplace_back(channel, value);
        chosen_sequence.push_back(static_cast<std::uint32_t>(chosen));
    }

    std::vector<std::pair<std::uint32_t, std::uint64_t>> Snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return received;
    }

    std::vector<std::uint32_t> Choices() {
        std::lock_guard<std::mutex> lock(mutex);
        return chosen_sequence;
    }

    /// 顺序敏感的 FNV-1a 64：把 (channel, value) 折成一个数。
    std::uint64_t Checksum() {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const auto& item : Snapshot()) {
            hash ^= (static_cast<std::uint64_t>(item.first) << 48U) ^ item.second;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    std::uint64_t ChoiceChecksum() {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const std::uint32_t choice : Choices()) {
            hash ^= choice;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    /// 只对消息值做 FNV（与 report.stats.message_checksum 同算法）。
    std::uint64_t ValueChecksum() {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const auto& item : Snapshot()) {
            hash ^= item.second;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    void Dump(const char* tag, std::size_t limit) {
        const auto items = Snapshot();
        std::printf("[select/%s] first %zu: ", tag,
                    items.size() < limit ? items.size() : limit);
        for (std::size_t index = 0; index < items.size() && index < limit; ++index) {
            std::printf("(%u,%llu) ", items[index].first,
                        static_cast<unsigned long long>(items[index].second));
        }
        std::printf("\n");
    }

    std::mutex mutex;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> received;
    std::vector<std::uint32_t> chosen_sequence;
};

inline Result& SharedResult() {
    static Result result;
    return result;
}

template <typename Channel>
std::uint64_t Produce(std::uint32_t gid, Channel& channel, std::uint32_t count) {
    std::uint64_t sent = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint64_t value =
            static_cast<std::uint64_t>(gid) * 10000ULL + index;
        if (channel.Send(value) != Decision::kOk) {
            break;
        }
        ++sent;
    }
    return sent;
}

/// 录制端 consumer：live select。
template <typename Channel, typename Recorder, typename Clock>
std::uint64_t ConsumeLive(Channel& ch0, Channel& ch1, Recorder& recorder,
                          Clock& clock, go2cpp::replay::SelectGroup& group,
                          Result* result) {
    using go2cpp::replay::SyncSelect;
    std::uint64_t received = 0;
    for (std::uint32_t index = 0; index < kTotalMessages; ++index) {
        std::uint64_t value = 0;
        SyncSelect select(recorder, clock, group);
        const int case0 = select.AddRecv(ch0, &value);
        const int case1 = select.AddRecv(ch1, &value);
        if (case0 < 0 || case1 < 0) {
            std::fprintf(stderr, "[select] AddRecv failed: %s\n",
                         select.error().c_str());
            break;
        }
        const int chosen = select.Wait();
        if (chosen < 0) {
            std::fprintf(stderr, "[select] Wait failed: %s\n",
                         select.error().c_str());
            break;
        }
        const std::uint32_t channel =
            chosen == case0 ? kChannel0 : kChannel1;
        result->Push(channel, value, chosen);
        ++received;
        if ((index + 1U) % 8U == 0U) {
            clock.SleepFor(kConsumerSleepNs);
        }
    }
    return received;
}

/// 重放端 consumer：select 的决策全部来自记录。
template <typename Channel, typename Clock>
std::uint64_t ConsumeReplay(Channel& ch0, Channel& ch1, Clock& clock,
                            go2cpp::replay::ReplaySource& source,
                            Result* result) {
    using go2cpp::replay::SyncSelect;
    std::uint64_t received = 0;
    for (std::uint32_t index = 0; index < kTotalMessages; ++index) {
        std::uint64_t value = 0;
        SyncSelect select(source);
        const int case0 = select.AddRecv(ch0, &value);
        const int case1 = select.AddRecv(ch1, &value);
        if (case0 < 0 || case1 < 0) {
            std::fprintf(stderr, "[select] replay AddRecv failed: %s\n",
                         select.error().c_str());
            break;
        }
        const int chosen = select.Wait();
        if (chosen < 0) {
            std::fprintf(stderr, "[select] replay Wait failed: %s\n",
                         select.error().c_str());
            break;
        }
        const std::uint32_t channel =
            chosen == case0 ? kChannel0 : kChannel1;
        result->Push(channel, value, chosen);
        ++received;
        if ((index + 1U) % 8U == 0U) {
            clock.SleepFor(kConsumerSleepNs);
        }
    }
    return received;
}

}  // namespace demo_select

namespace {

void SelectEntry(std::uint32_t gid, go2cpp::replay::ReplaySource& source) {
    using namespace go2cpp::replay;
    using namespace demo_select;
    ScopedGid guard(gid);
    ReplayClock clock(source);
    SyncChannel<std::uint64_t> ch0(kChannel0, source);
    SyncChannel<std::uint64_t> ch1(kChannel1, source);
    if (gid == 0 || gid == 1) {
        (void)Produce(gid, ch0, 24);
    } else if (gid == 2) {
        (void)Produce(gid, ch1, kCh1Messages);
    } else {
        (void)ConsumeReplay(ch0, ch1, clock, source, &SharedResult());
    }
}

}  // namespace

RECORD_FIBER_ENTRY_NAMED("select-ch0ch1", SelectEntry);

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
    using namespace demo_select;

    RecordOptions options;
    options.path = path;
    options.demo_name = "select-ch0ch1";
    options.gid_count = kFiberCount;
    options.channel_count = 2;
    options.value_kind = 0;
    options.drop_wake_at = EnvU32("GO2CPP_REPLAY_DROP_WAKE", 0);

    Recorder recorder(options);
    if (!recorder.ok()) {
        std::fprintf(stderr, "[select/record] %s\n", recorder.error().c_str());
        return 1;
    }

    std::uint64_t consumer_received = 0;
    {
        Scheduler scheduler(3);
        scheduler.start();
        SelectGroup group;
        ReplayClock clock(recorder);
        SyncChannel<std::uint64_t> ch0(kChannel0, recorder, 0, &group);
        SyncChannel<std::uint64_t> ch1(kChannel1, recorder, 0, &group);

        std::vector<std::shared_ptr<Task>> tasks;
        auto spawn_producer = [&](std::uint32_t gid, SyncChannel<std::uint64_t>& ch,
                                  std::uint32_t count) {
            tasks.push_back(scheduler.spawn([&recorder, &ch, gid, count] {
                ScopedFiberGid guard(gid);
                Event spawn;
                spawn.type = EventType::kSpawn;
                spawn.gid = gid;
                recorder.Write(spawn);
                const std::uint64_t sent = Produce(gid, ch, count);
                Event exit;
                exit.type = EventType::kExit;
                exit.gid = gid;
                exit.value = static_cast<std::int64_t>(sent);
                exit.aux64 = 0;
                recorder.Write(exit);
            }));
        };
        spawn_producer(0, ch0, 24);
        spawn_producer(1, ch0, 24);
        spawn_producer(2, ch1, kCh1Messages);
        tasks.push_back(scheduler.spawn([&recorder, &clock, &group, &ch0, &ch1,
                                         &consumer_received] {
            ScopedFiberGid guard(kConsumerGid);
            Event spawn;
            spawn.type = EventType::kSpawn;
            spawn.gid = kConsumerGid;
            recorder.Write(spawn);
            consumer_received = ConsumeLive(ch0, ch1, recorder, clock, group,
                                            &SharedResult());
            Event exit;
            exit.type = EventType::kExit;
            exit.gid = kConsumerGid;
            exit.value = 0;
            exit.aux64 = static_cast<std::uint64_t>(consumer_received);
            recorder.Write(exit);
        }));
        for (auto& task : tasks) {
            (void)task->wait();
        }
        scheduler.shutdown();
    }
    recorder.Finalize("completed");

    std::printf("[select/record] path=%s\n", path);
    SharedResult().Dump("record", 8);
    std::printf("[select/record] value_checksum=%llu\n",
                static_cast<unsigned long long>(SharedResult().ValueChecksum()));
    std::printf("[select/record] received=%llu checksum=%llu choices=%llu\n",
                static_cast<unsigned long long>(consumer_received),
                static_cast<unsigned long long>(SharedResult().Checksum()),
                static_cast<unsigned long long>(SharedResult().ChoiceChecksum()));
    return consumer_received == kTotalMessages ? 0 : 1;
}

int RunReplay(const char* path) {
    using namespace go2cpp::replay;
    using namespace demo_select;

    ReplayOptions options;
    options.drop_wake_at = EnvU32("GO2CPP_REPLAY_DROP_WAKE", 0);
    const ReplayReport report = ReplayFileAuto(path, options);
    const auto received = SharedResult().Snapshot();

    std::printf("%s", report.Summary().c_str());
    SharedResult().Dump("replay", 8);
    std::printf("[select/replay] value_checksum=%llu\n",
                static_cast<unsigned long long>(SharedResult().ValueChecksum()));
    std::printf("[select/replay] received=%zu checksum=%llu choices=%llu\n",
                received.size(),
                static_cast<unsigned long long>(SharedResult().Checksum()),
                static_cast<unsigned long long>(SharedResult().ChoiceChecksum()));
    std::printf("[select/replay] recorded messages=%llu checksum=%llu selects=%llu\n",
                static_cast<unsigned long long>(report.stats.messages),
                static_cast<unsigned long long>(report.stats.message_checksum),
                static_cast<unsigned long long>(report.stats.selects));
    std::printf("[select/replay] recorded select_checksum=%llu "
                "replayed=%llu\n",
                static_cast<unsigned long long>(report.stats.select_checksum),
                static_cast<unsigned long long>(
                    report.stats.replayed_select_checksum));

    const bool consistent =
        report.status == ReplayStatus::kOk &&
        received.size() == kTotalMessages &&
        SharedResult().ValueChecksum() == report.stats.message_checksum &&
        report.stats.select_checksum == report.stats.replayed_select_checksum;
    std::printf("[select/replay] consistency=%s\n",
                consistent ? "MATCH" : "MISMATCH");
    if (!consistent) {
        std::printf("[select/replay] status=%s error=%s\n",
                    ReplayStatusName(report.status), report.error.c_str());
    }
    return consistent ? 0 : 1;
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
