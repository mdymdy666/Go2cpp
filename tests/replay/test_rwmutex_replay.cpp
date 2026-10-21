/**
 * @file test_rwmutex_replay.cpp
 * @brief RWMutex 的录制/回放随机、并发与压测测试。
 *
 * 场景：
 *   1. 合成事件序列（读/写/阻塞/解锁、aux 方向位、target/waiters 保持）；
 *   2. 真实并发：4 读 2 写各 1000 次（仅 GO2CPP_DEBUG_RESOURCES=1 构建）；
 *      读写锁升级/降级探测（本实现明确不支持，记录并断言不可升级/降级）；
 *   3. 压测：10000 条 RW 事件（5000 对 read-lock/unlock）的录制与回放，
 *      记录耗时、文件大小与 RSS。
 */

#include "replay_v5_test_support.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace go2cpp::replay;
using namespace go2cpp::replay::v5test;

/// 场景 1：合成事件序列，逐字段与统计断言。
void TestSyntheticRwSequence() {
    const std::string path = TempPath("rw_synth");
    const auto read = static_cast<std::uint64_t>(ResourceAuxFlags::kRead);
    const auto write = static_cast<std::uint64_t>(ResourceAuxFlags::kWrite);
    std::vector<Event> events;
    // 读锁成功（reader 1）。
    events.push_back(MakeV5(EventType::kResRwReadLock, 100, 1, 0, 0, 0, -1, 0,
                            read, 0));
    // 写锁排队（reader 1 仍持有读锁）。
    events.push_back(MakeV5(EventType::kResRwWriteLock, 100, 2, 0, 1, 1, -1,
                            0, write, 0));
    // 写锁拿到（blocked 之后必然还有一条成功事件）。
    events.push_back(MakeV5(EventType::kResRwWriteLock, 100, 2, 0, 0, 0, -1,
                            0, write, 0));
    // 读解锁（唤醒者 2）。
    events.push_back(MakeV5(EventType::kResRwUnlock, 100, 1, 2, 0, 0, -1, 0,
                            read, 0));
    // 写解锁。
    events.push_back(MakeV5(EventType::kResRwUnlock, 100, 2, 0, 0, 0, -1, 0,
                            write, 0));
    // 第二个读者排队后成功。
    events.push_back(MakeV5(EventType::kResRwReadLock, 100, 3, 0, 1, 1, -1, 0,
                            read, 0));
    events.push_back(MakeV5(EventType::kResRwReadLock, 100, 3, 0, 0, 0, -1, 0,
                            read, 0));
    events.push_back(MakeV5(EventType::kResRwUnlock, 100, 3, 0, 0, 0, -1, 0,
                            read, 0));

    const RecordedFile recorded = RecordAndReplay(path, events);
    const ReplayReport& report = recorded.report;
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.complete);
    GO2CPP_CHECK(report.stats.resource_events == events.size());
    GO2CPP_CHECK(report.stats.res_rw_reads == 2U);
    GO2CPP_CHECK(report.stats.res_rw_writes == 1U);
    GO2CPP_CHECK(report.stats.res_rw_unlocks == 3U);
    GO2CPP_CHECK(report.stats.res_rw_read_blocks == 1U);
    GO2CPP_CHECK(report.stats.res_rw_write_blocks == 1U);
    GO2CPP_CHECK(report.stats.resource_order_anomalies == 0U);
    GO2CPP_CHECK(report.resources.size() == 8U);
    GO2CPP_CHECK(report.resources[0].aux == read);
    GO2CPP_CHECK(report.resources[1].aux == write);
    GO2CPP_CHECK(report.resources[1].waiters == 1U);
    GO2CPP_CHECK(report.resources[3].target_gid == 2U);
    GO2CPP_CHECK(report.resources[7].fd == -1);
    ExpectConsistent(path, events, report, "rw_synth");
    PrintStats("rw_synth", report);
    RemoveFile(path);
}

#if GO2CPP_DEBUG_RESOURCES
/// 场景 2：真实 4 读 2 写并发（各 1000 次）+ 升级/降级探测。
void TestRealConcurrentRw() {
    const std::string path = TempPath("rw_real");
    {
        RecordOptions options;
        options.path = path;
        options.demo_name = "rwmutex-real";
        options.gid_count = 32;
        Recorder recorder(options);
        GO2CPP_CHECK(recorder.ok());
        if (!recorder.ok()) {
            return;
        }

        go2cpp::sync::RWMutex rw;
        std::atomic<int> shared{0};
        std::atomic<int> readers_done{0};
        std::atomic<int> writers_done{0};
        std::vector<std::thread> threads;
        threads.reserve(6);
        for (int reader = 0; reader < 4; ++reader) {
            threads.emplace_back([&rw, &shared, &readers_done] {
                for (int iteration = 0; iteration < 1000; ++iteration) {
                    rw.RLock();
                    (void)shared.load(std::memory_order_relaxed);
                    rw.RUnlock();
                }
                readers_done.fetch_add(1, std::memory_order_release);
            });
        }
        for (int writer = 0; writer < 2; ++writer) {
            threads.emplace_back([&rw, &shared, &writers_done] {
                for (int iteration = 0; iteration < 1000; ++iteration) {
                    rw.Lock();
                    shared.fetch_add(1, std::memory_order_relaxed);
                    rw.Unlock();
                }
                writers_done.fetch_add(1, std::memory_order_release);
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        GO2CPP_CHECK(readers_done.load() == 4);
        GO2CPP_CHECK(writers_done.load() == 2);
        recorder.Finalize("test");
        std::printf("[rw_real] recorded=%llu\n",
                    static_cast<unsigned long long>(recorder.Records()));
    }

    const ReplayReport report = ReplayFile(path);
    const ReplayStats& stats = report.stats;
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.complete);
    // 每个成功 RLock 恰好一条 kOk 事件；每次 Lock 先 blocked 再 ok；
    // 解锁各一条。
    GO2CPP_CHECK(stats.res_rw_reads == 4000U);
    GO2CPP_CHECK(stats.res_rw_writes == 2000U);
    GO2CPP_CHECK(stats.res_rw_unlocks == 6000U);
    GO2CPP_CHECK(stats.res_rw_write_blocks >= 1U);
    GO2CPP_CHECK(stats.res_rw_read_blocks + stats.res_rw_write_blocks > 0U);
    // 并发录制的事件写入顺序可能落后于线性化顺序，anomaly 只记录不判损坏。
    std::printf("[rw_real] anomalies=%llu read_blocks=%llu write_blocks=%llu\n",
                static_cast<unsigned long long>(
                    stats.resource_order_anomalies),
                static_cast<unsigned long long>(stats.res_rw_read_blocks),
                static_cast<unsigned long long>(stats.res_rw_write_blocks));
    ExpectFileConsistent(path, report, "rw_real");
    PrintStats("rw_real", report);
    RemoveFile(path);

    // 升级/降级探测：本实现不提供 Upgrade/Downgrade，必须显式失败而不是死锁。
    go2cpp::sync::RWMutex probe;
    probe.RLock();
    const bool upgrade = probe.TryLock();
    const bool reentrant_read = probe.TryRLock();
    if (reentrant_read) {
        probe.RUnlock();
    }
    probe.RUnlock();
    probe.Lock();
    const bool downgrade = probe.TryRLock();
    probe.Unlock();
    std::printf(
        "[rw_real] upgrade_supported=%d downgrade_supported=%d "
        "reentrant_read=%d\n",
        upgrade ? 1 : 0, downgrade ? 1 : 0, reentrant_read ? 1 : 0);
    GO2CPP_CHECK(!upgrade);
    GO2CPP_CHECK(reentrant_read);
    GO2CPP_CHECK(!downgrade);
}
#endif  // GO2CPP_DEBUG_RESOURCES

/// 场景 3：10000 条 RW 事件的录制/回放压测。
void TestReplayStress10k() {
    const std::string path = TempPath("rw_stress");
    constexpr int kPairs = 5000;  // 5000 对 = 10000 条事件
    const auto read = static_cast<std::uint64_t>(ResourceAuxFlags::kRead);
    std::vector<Event> events;
    events.reserve(static_cast<std::size_t>(kPairs) * 2U);
    for (int index = 0; index < kPairs; ++index) {
        const std::uint64_t resource =
            100U + static_cast<std::uint64_t>(index % 8);
        const std::uint64_t actor =
            1U + static_cast<std::uint64_t>(index % 4);
        events.push_back(MakeV5(EventType::kResRwReadLock, resource, actor, 0U,
                                0U, 0U, -1, 0U, read, 0));
        events.push_back(MakeV5(EventType::kResRwUnlock, resource, actor, 0U,
                                0U, 0U, -1, 0U, read, 0));
    }
    GO2CPP_CHECK(events.size() == 10000U);

    const std::uint64_t rss_before = RssKb();
    const std::uint64_t peak_before = PeakRssKb();
    const std::uint64_t record_start = NowNs();
    std::uint64_t records = 0;
    {
        RecordOptions options;
        options.path = path;
        options.demo_name = "rwmutex-stress-10k";
        options.gid_count = 64;
        Recorder recorder(options);
        GO2CPP_CHECK(recorder.ok());
        if (!recorder.ok()) {
            return;
        }
        for (const auto& event : events) {
            GO2CPP_CHECK(recorder.Write(event));
        }
        recorder.Finalize("stress");
        records = recorder.Records();
    }
    const std::uint64_t record_ns = NowNs() - record_start;
    const std::uint64_t rss_after_record = RssKb();

    std::uint64_t best_replay_ns = UINT64_MAX;
    std::uint64_t total_replay_ns = 0;
    ReplayReport report;
    for (int attempt = 0; attempt < 5; ++attempt) {
        const std::uint64_t start = NowNs();
        report = ReplayFile(path);
        const std::uint64_t elapsed = NowNs() - start;
        best_replay_ns = std::min(best_replay_ns, elapsed);
        total_replay_ns += elapsed;
    }
    const std::uint64_t rss_after_replay = RssKb();
    const std::uint64_t peak_after = PeakRssKb();

    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.stats.resource_events == events.size());
    GO2CPP_CHECK(report.stats.res_rw_reads == static_cast<std::uint64_t>(kPairs));
    GO2CPP_CHECK(report.stats.res_rw_writes == 0U);
    GO2CPP_CHECK(report.stats.res_rw_unlocks ==
                 static_cast<std::uint64_t>(kPairs));
    GO2CPP_CHECK(report.stats.resource_order_anomalies == 0U);
    ExpectConsistent(path, events, report, "rw_stress");

    const std::uint64_t average_replay_ns = total_replay_ns / 5U;
    const double events_per_second =
        average_replay_ns == 0U
            ? 0.0
            : (static_cast<double>(events.size()) * 1e9 /
               static_cast<double>(average_replay_ns));
    std::printf(
        "[rw_stress] events=%llu pairs=%d file_bytes=%llu records=%llu "
        "record_ms=%.3f replay_min_ms=%.3f replay_avg_ms=%.3f "
        "replay_eps=%.0f rss_before_kb=%llu rss_after_record_kb=%llu "
        "rss_after_replay_kb=%llu peak_before_kb=%llu peak_after_kb=%llu\n",
        static_cast<unsigned long long>(events.size()), kPairs,
        static_cast<unsigned long long>(report.file_size),
        static_cast<unsigned long long>(records),
        static_cast<double>(record_ns) / 1e6,
        static_cast<double>(best_replay_ns) / 1e6,
        static_cast<double>(average_replay_ns) / 1e6, events_per_second,
        static_cast<unsigned long long>(rss_before),
        static_cast<unsigned long long>(rss_after_record),
        static_cast<unsigned long long>(rss_after_replay),
        static_cast<unsigned long long>(peak_before),
        static_cast<unsigned long long>(peak_after));
    PrintStats("rw_stress", report);
    RemoveFile(path);
}

}  // namespace

void run_rwmutex_replay_tests() {
    go2cpp_tests::announce("rwmutex_replay");
    TestSyntheticRwSequence();
#if GO2CPP_DEBUG_RESOURCES
    TestRealConcurrentRw();
#else
    std::printf(
        "[rwmutex_replay] skip real concurrent (GO2CPP_DEBUG_RESOURCES=0)\n");
#endif
    TestReplayStress10k();
}
