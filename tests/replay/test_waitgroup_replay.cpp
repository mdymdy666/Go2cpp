/**
 * @file test_waitgroup_replay.cpp
 * @brief WaitGroup 的录制/回放随机、并发与边界测试。
 *
 * 场景：
 *   1. 合成事件序列：Add/Done/Wait、负 delta（aux 低 32 位补码）、
 *      Wait 取消；
 *   2. 真实原语（仅 GO2CPP_DEBUG_RESOURCES=1）：8 线程并发 Add(8) +
 *      8 线程并发 Done 各 8 次 + Wait 阻塞唤醒；负数 Add 被拒绝且不产生
 *      事件；Add(0) 的边界记录。
 */

#include "replay_v5_test_support.hpp"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace go2cpp::replay;
using namespace go2cpp::replay::v5test;

/// 场景 1：合成序列，覆盖 Add/Done/Wait 与负 delta 边界。
void TestSyntheticWaitGroupSequence() {
    const std::string path = TempPath("wg_synth");
    std::vector<Event> events;
    // Add(2)：value=操作后计数，aux 低 32 位=delta。
    events.push_back(MakeV5(EventType::kResWgAdd, 300, 1, 0, 0, 0, -1, 0, 2, 2));
    // Add(1)。
    events.push_back(MakeV5(EventType::kResWgAdd, 300, 2, 0, 0, 0, -1, 0, 1, 3));
    // Done：aux=0xFFFFFFFF 表示 -1。
    events.push_back(
        MakeV5(EventType::kResWgDone, 300, 3, 0, 0, 0, -1, 0, 0xFFFFFFFFULL, 2));
    // Wait 阻塞。
    events.push_back(MakeV5(EventType::kResWgWait, 300, 4, 0, 1, 1, -1, 0, 0, 2));
    // 归零 Done 唤醒 waiter 4。
    events.push_back(
        MakeV5(EventType::kResWgDone, 300, 3, 4, 2, 1, -1, 0, 0xFFFFFFFFULL, 0));
    // Wait 返回（result=0）。
    events.push_back(MakeV5(EventType::kResWgWait, 300, 4, 0, 0, 0, -1, 0, 0, 0));
    // 边界：一次带负 delta 的 Add（回放按 aux 低 32 位补码累加）。
    events.push_back(MakeV5(EventType::kResWgAdd, 300, 1, 0, 0, 0, -1, 0,
                            0xFFFFFFFEULL, 0));

    const RecordedFile recorded = RecordAndReplay(path, events);
    const ReplayReport& report = recorded.report;
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.complete);
    GO2CPP_CHECK(report.stats.resource_events == events.size());
    GO2CPP_CHECK(report.stats.res_wg_adds == 3U);
    GO2CPP_CHECK(report.stats.res_wg_dones == 2U);
    GO2CPP_CHECK(report.stats.res_wg_waits == 2U);
    GO2CPP_CHECK(report.stats.resource_order_anomalies == 0U);
    GO2CPP_CHECK(report.resources.size() == events.size());
    GO2CPP_CHECK(report.resources[0].aux == 2U);
    GO2CPP_CHECK(report.resources[0].value == 2);
    GO2CPP_CHECK(report.resources[2].aux == 0xFFFFFFFFULL);
    GO2CPP_CHECK(report.resources[4].target_gid == 4U);
    GO2CPP_CHECK(report.resources[6].aux == 0xFFFFFFFEULL);
    ExpectConsistent(path, events, report, "wg_synth");
    PrintStats("wg_synth", report);
    RemoveFile(path);
}

#if GO2CPP_DEBUG_RESOURCES
/// 场景 2：真实并发 Add/Done + Wait 阻塞唤醒 + 负数 Add 拒绝。
void TestRealWaitGroup() {
    const std::string path = TempPath("wg_real");
    int rejected = 0;
    {
        RecordOptions options;
        options.path = path;
        options.demo_name = "waitgroup-real";
        options.gid_count = 32;
        Recorder recorder(options);
        GO2CPP_CHECK(recorder.ok());
        if (!recorder.ok()) {
            return;
        }

        go2cpp::sync::WaitGroup wg;
        // 8 个线程各 Add(8)：并发 Add，总计 64。
        std::vector<std::thread> adders;
        adders.reserve(8);
        for (int thread = 0; thread < 8; ++thread) {
            adders.emplace_back([&wg] { wg.Add(8); });
        }
        for (auto& thread : adders) {
            thread.join();
        }
        GO2CPP_CHECK(wg.Count() == 64);

        std::atomic<bool> waiter_done{false};
        std::thread waiter([&] {
            const bool ok = wg.Wait();
            if (ok) {
                waiter_done.store(true, std::memory_order_release);
            }
        });
        GO2CPP_CHECK(WaitForWgWaiters(wg, 1));
        // 8 个线程各 Done 8 次：并发 Done。
        std::vector<std::thread> doners;
        doners.reserve(8);
        for (int thread = 0; thread < 8; ++thread) {
            doners.emplace_back([&wg] {
                for (int iteration = 0; iteration < 8; ++iteration) {
                    wg.Done();
                }
            });
        }
        for (auto& thread : doners) {
            thread.join();
        }
        waiter.join();
        GO2CPP_CHECK(waiter_done.load());
        GO2CPP_CHECK(wg.Count() == 0);
        std::printf("[wg_real] count_after_done=%lld\n",
                    static_cast<long long>(wg.Count()));

        // 负数 Add 必须被拒绝，且不产生任何事件。
        try {
            wg.Add(-1);
        } catch (const std::logic_error&) {
            ++rejected;
        }
        GO2CPP_CHECK(rejected == 1);
        GO2CPP_CHECK(wg.Count() == 0);
        // 再来一轮：Add(3) + 3 次 Done（复用同一 WaitGroup）。
        wg.Add(3);
        wg.Done();
        wg.Done();
        wg.Done();
        // count==0 时 Wait 立即返回，不记录 wait 事件。
        GO2CPP_CHECK(wg.Wait());
        // 边界：Add(0) 走 Done 分支（当前实现记录一条 done，delta=0）。
        wg.Add(0);

        recorder.Finalize("test");
        std::printf("[wg_real] rejected=%d recorded=%llu\n", rejected,
                    static_cast<unsigned long long>(recorder.Records()));
    }

    const ReplayReport report = ReplayFile(path);
    const ReplayStats& stats = report.stats;
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.complete);
    // 8 次并发 Add(8) + 1 次 Add(3) = 9；64 次 Done + 3 次 Done + Add(0) = 68；
    // 1 次阻塞 Wait 产生 blocked+ok 两条事件。
    GO2CPP_CHECK(stats.res_wg_adds == 9U);
    GO2CPP_CHECK(stats.res_wg_dones == 68U);
    GO2CPP_CHECK(stats.res_wg_waits == 2U);
    GO2CPP_CHECK(stats.resource_order_anomalies == 0U);
    ExpectFileConsistent(path, report, "wg_real");
    PrintStats("wg_real", report);
    RemoveFile(path);
}
#endif  // GO2CPP_DEBUG_RESOURCES

}  // namespace

void run_waitgroup_replay_tests() {
    go2cpp_tests::announce("waitgroup_replay");
    TestSyntheticWaitGroupSequence();
#if GO2CPP_DEBUG_RESOURCES
    TestRealWaitGroup();
#else
    std::printf(
        "[waitgroup_replay] skip real WaitGroup (GO2CPP_DEBUG_RESOURCES=0)\n");
#endif
}
