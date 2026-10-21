/**
 * @file test_cv_replay.cpp
 * @brief ConditionVariable 的录制/回放随机与边界测试。
 *
 * 场景：
 *   1. 合成事件序列：wait / notify / notify_all / 取消 / 空广播 / 负数广播；
 *   2. 真实原语（仅 GO2CPP_DEBUG_RESOURCES=1）：3 次虚假唤醒（谓词未满足
 *      的 NotifyOne）+ 1 次真实 NotifyAll + 1 次 wait 超时 + 1 次空 NotifyAll；
 *      断言 waits/notifies/notify_alls/woken 与事件条数完全一致。
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

/// 场景 1：合成序列，覆盖各结果与 NotifyAll 的 value/等待者数边界。
void TestSyntheticCvSequence() {
    const std::string path = TempPath("cv_synth");
    const auto notify_all =
        static_cast<std::uint64_t>(ResourceAuxFlags::kNotifyAll);
    std::vector<Event> events;
    // wait 入队（result=1/blocked, waiters=1）。
    events.push_back(MakeV5(EventType::kResCvWait, 200, 1, 0, 1, 1, -1, 0, 0,
                            0));
    // wait 被通知（result=0/ok）。
    events.push_back(MakeV5(EventType::kResCvWait, 200, 2, 0, 0, 0, -1, 0, 0,
                            0));
    // NotifyOne 命中 actor 2。
    events.push_back(MakeV5(EventType::kResCvNotify, 200, 3, 2, 2, 0, -1, 0,
                            0, 0));
    // NotifyAll：value=3 表示实际唤醒 3 个，waiters=3，aux 高位带候选数。
    const std::uint64_t notify_all_aux =
        notify_all | (3ULL << kResourceAuxCountShift);
    events.push_back(MakeV5(EventType::kResCvNotifyAll, 200, 3, 1, 2, 3, -1,
                            0, notify_all_aux, 3));
    // wait 被取消/超时（result=4）。
    events.push_back(MakeV5(EventType::kResCvWait, 200, 4, 0, 4, 0, -1, 0, 0,
                            0));
    // NotifyOne 没有等待者（target=0 -> woken 不增加）。
    events.push_back(MakeV5(EventType::kResCvNotify, 200, 5, 0, 0, 0, -1, 0,
                            0, 0));
    // 空 NotifyAll：value=0。
    events.push_back(MakeV5(EventType::kResCvNotifyAll, 200, 5, 0, 0, 0, -1,
                            0, notify_all, 0));
    // 边界：value=-1 的广播不得把 woken 减成负数。
    events.push_back(MakeV5(EventType::kResCvNotifyAll, 200, 5, 0, 0, 0, -1,
                            0, notify_all, -1));

    const RecordedFile recorded = RecordAndReplay(path, events);
    const ReplayReport& report = recorded.report;
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.complete);
    GO2CPP_CHECK(report.stats.resource_events == events.size());
    GO2CPP_CHECK(report.stats.res_cv_waits == 3U);
    GO2CPP_CHECK(report.stats.res_cv_notifies == 2U);
    GO2CPP_CHECK(report.stats.res_cv_notify_alls == 3U);
    GO2CPP_CHECK(report.stats.res_cv_woken == 4U);
    GO2CPP_CHECK(report.stats.resource_order_anomalies == 0U);
    GO2CPP_CHECK(report.resources.size() == events.size());
    GO2CPP_CHECK(report.resources[3].aux == notify_all_aux);
    GO2CPP_CHECK(report.resources[3].waiters == 3U);
    GO2CPP_CHECK(report.resources[3].value == 3);
    GO2CPP_CHECK(report.resources[7].value == -1);
    ExpectConsistent(path, events, report, "cv_synth");
    PrintStats("cv_synth", report);
    RemoveFile(path);
}

#if GO2CPP_DEBUG_RESOURCES
/// 场景 2：真实 CV——虚假唤醒 x3 + 真实唤醒 + 超时 + 空广播。
void TestRealCv() {
    const std::string path = TempPath("cv_real");
    {
        RecordOptions options;
        options.path = path;
        options.demo_name = "condition-variable-real";
        options.gid_count = 32;
        Recorder recorder(options);
        GO2CPP_CHECK(recorder.ok());
        if (!recorder.ok()) {
            return;
        }

        go2cpp::SchedulerConfig config;
        config.processor_count = 2;
        config.max_workers = 2;
        go2cpp::Scheduler scheduler(config);
        scheduler.start();

        go2cpp::sync::Mutex mutex;
        go2cpp::sync::ConditionVariable cv;
        std::atomic<bool> ready{false};
        std::atomic<bool> waiter_done{false};
        std::atomic<int> wake_count{0};
        // 等待者：每次唤醒后检查谓词，未满足则再次等待（虚假唤醒语义）。
        scheduler.go([&] {
            mutex.Lock();
            while (!ready.load(std::memory_order_acquire)) {
                if (cv.WaitFor(mutex, std::chrono::milliseconds(1000),
                               go2cpp::Background())) {
                    wake_count.fetch_add(1, std::memory_order_release);
                }
            }
            mutex.Unlock();
            waiter_done.store(true, std::memory_order_release);
        });
        for (int round = 0; round < 3; ++round) {
            GO2CPP_CHECK(WaitForCvWaiters(cv, 1));
            scheduler.go([&] {
                mutex.Lock();
                cv.NotifyOne();
                mutex.Unlock();
            });
            for (int spin = 0; spin < 3000 &&
                               wake_count.load() < round + 1;
                 ++spin) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (wake_count.load() < round + 1) {
                std::fprintf(stderr, "[cv_real] spurious wake %d missed\n",
                             round);
            }
            GO2CPP_CHECK(wake_count.load() >= round + 1);
        }
        // 最后一轮：等待者重新入队后再广播，保证 woken=1。
        GO2CPP_CHECK(WaitForCvWaiters(cv, 1));
        ready.store(true, std::memory_order_release);
        scheduler.go([&] {
            mutex.Lock();
            cv.NotifyAll();
            mutex.Unlock();
        });
        GO2CPP_CHECK(WaitForFlag(waiter_done));

        // 超时等待：60ms 内没有通知，记录 blocked + cancelled 两条事件。
        go2cpp::sync::Mutex timeout_mutex;
        go2cpp::sync::ConditionVariable timeout_cv;
        std::atomic<bool> timeout_done{false};
        std::atomic<bool> timeout_result{true};
        scheduler.go([&] {
            timeout_mutex.Lock();
            const bool ok = timeout_cv.WaitFor(
                timeout_mutex, std::chrono::milliseconds(60),
                go2cpp::Background());
            timeout_mutex.Unlock();
            timeout_result.store(ok);
            timeout_done.store(true, std::memory_order_release);
        });
        GO2CPP_CHECK(WaitForFlag(timeout_done));
        GO2CPP_CHECK(!timeout_result.load());
        // 空广播：没有等待者也必须记录一条 notify_all（value=0）。
        timeout_cv.NotifyAll();

        GO2CPP_CHECK(scheduler.shutdown_for(std::chrono::seconds(10)));
        recorder.Finalize("test");
        std::printf("[cv_real] recorded=%llu wakes=%d\n",
                    static_cast<unsigned long long>(recorder.Records()),
                    wake_count.load());
    }

    const ReplayReport report = ReplayFile(path);
    const ReplayStats& stats = report.stats;
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.complete);
    // 4 次成功 Wait（每次 blocked+ok = 2 条）+ 1 次超时（blocked+cancelled）。
    GO2CPP_CHECK(stats.res_cv_waits == 10U);
    GO2CPP_CHECK(stats.res_cv_notifies == 3U);
    GO2CPP_CHECK(stats.res_cv_notify_alls == 2U);
    GO2CPP_CHECK(stats.res_cv_woken == 4U);
    GO2CPP_CHECK(stats.resource_order_anomalies == 0U);
    ExpectFileConsistent(path, report, "cv_real");
    PrintStats("cv_real", report);
    RemoveFile(path);
}
#endif  // GO2CPP_DEBUG_RESOURCES

}  // namespace

void run_cv_replay_tests() {
    go2cpp_tests::announce("cv_replay");
    TestSyntheticCvSequence();
#if GO2CPP_DEBUG_RESOURCES
    TestRealCv();
#else
    std::printf("[cv_replay] skip real CV (GO2CPP_DEBUG_RESOURCES=0)\n");
#endif
}
