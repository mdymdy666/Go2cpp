/**
 * @file resource_v5_demo.cpp
 * @brief G2CRPLY v5 资源事件录制/回放 demo（需要 GO2CPP_REPLAY=ON）。
 *
 * 做三件事：
 *   1. 在真实原语上产生 v5 资源事件：RWMutex 读/写、ConditionVariable
 *      wait/notify/notify_all、WaitGroup add/done/wait、IOManager read/write；
 *   2. 由 Recorder 落盘（事件与 seq 来自运行时 sink，不是手工编造）；
 *   3. ReplayFile 只读重演，打印各计数、checksum 与资源时间线前若干条。
 *
 * 输出契约（脚本据此断言）：
 *   ``[v5-demo] MATCH ...`` 表示 replay 计数与录制侧事件数一致、两次回放
 *   checksum 相同。
 */

#include "go2cpp/concurrency/rwmutex.hpp"
#include "go2cpp/channel.hpp"
#include "go2cpp/io.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include "replay/replay_engine.hpp"
#include "replay/recorder.hpp"

#include <unistd.h>
#include <sys/syscall.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::replay;

bool RecordPrimitives(const std::string& path) {
    RecordOptions options;
    options.path = path;
    options.demo_name = "resources-v5-demo";
    options.gid_count = 32;
    Recorder recorder(options);
    if (!recorder.ok()) {
        std::fprintf(stderr, "[v5-demo] recorder error: %s\n",
                     recorder.error().c_str());
        return false;
    }

    go2cpp::sync::RWMutex rw;
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable cv;
    go2cpp::sync::WaitGroup wg;
    auto rw_gate = go2cpp::MakeChannel<int>(1);
    std::atomic<bool> g1_locked{false};
    std::atomic<bool> g2_done{false};
    std::atomic<bool> cv_done{false};
    std::atomic<bool> wg_done{false};
    wg.Add(1);

    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    const auto wait_flag = [](const std::atomic<bool>& flag) {
        for (int attempt = 0; attempt < 2000 && !flag.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return flag.load();
    };

    // RWMutex：G1 持读锁 park 在 gate 上，G2 在这期间申请写锁（进入等待）。
    scheduler.go([&] {
        rw.RLock();
        g1_locked.store(true, std::memory_order_release);
        (void)rw_gate->RecvFor(std::chrono::seconds(2));
        rw.RUnlock();
    });
    if (!wait_flag(g1_locked)) {
        std::fprintf(stderr, "[v5-demo] reader did not start\n");
        return false;
    }
    scheduler.go([&] {
        rw.Lock();
        rw.Unlock();
        g2_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)rw_gate->Send(1);
    if (!wait_flag(g2_done)) {
        std::fprintf(stderr, "[v5-demo] writer did not finish\n");
        return false;
    }

    // ConditionVariable：等待者先 park，随后 NotifyOne + NotifyAll。
    scheduler.go([&] {
        mutex.Lock();
        (void)cv.WaitFor(mutex, std::chrono::milliseconds(500),
                         go2cpp::Background());
        mutex.Unlock();
        cv_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    scheduler.go([&] {
        mutex.Lock();
        cv.NotifyOne();
        cv.NotifyAll();
        mutex.Unlock();
    });
    if (!wait_flag(cv_done)) {
        std::fprintf(stderr, "[v5-demo] cv wait did not finish\n");
        return false;
    }

    // WaitGroup：Add 已在主线程完成，G5 等待、G6 Done。
    scheduler.go([&] {
        (void)wg.Wait();
        wg_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    scheduler.go([&] { wg.Done(); });
    if (!wait_flag(wg_done)) {
        std::fprintf(stderr, "[v5-demo] waitgroup wait did not finish\n");
        return false;
    }

    if (!scheduler.shutdown_for(std::chrono::seconds(10))) {
        std::fprintf(stderr, "[v5-demo] scheduler shutdown timed out\n");
        return false;
    }

    // IO：独立 IOManager + 真实 pipe 等待。
    {
        go2cpp::SchedulerConfig io_config;
        io_config.processor_count = 1;
        io_config.max_workers = 1;
        go2cpp::IOManager manager(io_config);
        if (!manager.Start()) {
            std::fprintf(stderr, "[v5-demo] io manager start failed\n");
            return false;
        }
        int fds[2]{-1, -1};
        if (::pipe(fds) != 0) {
            return false;
        }
        std::atomic<bool> io_done{false};
        manager.Go([&] {
            (void)manager.WaitFor(fds[0], go2cpp::IOEvent::kRead,
                                  std::chrono::seconds(2));
            char byte = 0;
            (void)::syscall(SYS_read, fds[0], &byte, 1);
            io_done.store(true, std::memory_order_release);
        });
        const char byte = 'v';
        (void)::syscall(SYS_write, fds[1], &byte, 1);
        for (int attempt = 0; attempt < 2000 && !io_done.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        manager.Shutdown();
        ::close(fds[0]);
        ::close(fds[1]);
    }

    recorder.Finalize("demo");
    std::printf("[v5-demo] recorded records=%llu path=%s\n",
                static_cast<unsigned long long>(recorder.Records()),
                path.c_str());
    return true;
}


}  // namespace

int main(int argc, char** argv) {
    const std::string path =
        argc > 1 ? argv[1] : "/tmp/go2cpp_resources_v5_demo.g2cr";
    if (!RecordPrimitives(path)) {
        return 1;
    }

    const ReplayReport report = ReplayFile(path);
    if (report.status != ReplayStatus::kOk) {
        std::fprintf(stderr, "[v5-demo] replay status=%s error=%s\n",
                     ReplayStatusName(report.status), report.error.c_str());
        return 2;
    }
    std::printf("%s", report.Summary().c_str());

    const auto& stats = report.stats;
    std::printf("[v5-demo] rw: reads=%llu writes=%llu unlocks=%llu "
                "read_blocks=%llu write_blocks=%llu\n",
                static_cast<unsigned long long>(stats.res_rw_reads),
                static_cast<unsigned long long>(stats.res_rw_writes),
                static_cast<unsigned long long>(stats.res_rw_unlocks),
                static_cast<unsigned long long>(stats.res_rw_read_blocks),
                static_cast<unsigned long long>(stats.res_rw_write_blocks));
    std::printf("[v5-demo] cv: waits=%llu notifies=%llu notify_alls=%llu "
                "woken=%llu\n",
                static_cast<unsigned long long>(stats.res_cv_waits),
                static_cast<unsigned long long>(stats.res_cv_notifies),
                static_cast<unsigned long long>(stats.res_cv_notify_alls),
                static_cast<unsigned long long>(stats.res_cv_woken));
    std::printf("[v5-demo] wg: adds=%llu dones=%llu waits=%llu\n",
                static_cast<unsigned long long>(stats.res_wg_adds),
                static_cast<unsigned long long>(stats.res_wg_dones),
                static_cast<unsigned long long>(stats.res_wg_waits));
    std::printf("[v5-demo] io: reads=%llu writes=%llu waits=%llu wakes=%llu "
                "bytes=%llu\n",
                static_cast<unsigned long long>(stats.res_io_reads),
                static_cast<unsigned long long>(stats.res_io_writes),
                static_cast<unsigned long long>(stats.res_io_waits),
                static_cast<unsigned long long>(stats.res_io_wakes),
                static_cast<unsigned long long>(stats.res_io_bytes));
    std::printf("[v5-demo] checksum=%llu res_v5_checksum=%llu "
                "replayed_checksum=%llu\n",
                static_cast<unsigned long long>(stats.resource_checksum),
                static_cast<unsigned long long>(stats.res_v5_checksum),
                static_cast<unsigned long long>(
                    stats.replayed_resource_checksum));

    std::printf("[v5-demo] timeline (first 24):\n");
    const std::size_t limit =
        report.resources.size() < 24 ? report.resources.size() : 24;
    for (std::size_t index = 0; index < limit; ++index) {
        const auto& entry = report.resources[index];
        std::printf("  seq=%llu t=%llu %-20s rid=%llu actor=%llu target=%llu "
                    "result=%u waiters=%u fd=%d bytes=%u value=%lld\n",
                    static_cast<unsigned long long>(entry.seq),
                    static_cast<unsigned long long>(entry.vtime_ns),
                    EventTypeName(entry.type),
                    static_cast<unsigned long long>(entry.resource_id),
                    static_cast<unsigned long long>(entry.actor_gid),
                    static_cast<unsigned long long>(entry.target_gid),
                    entry.result, entry.waiters, entry.fd, entry.bytes,
                    static_cast<long long>(entry.value));
    }

    const ReplayReport again = ReplayFile(path);
    const bool checksum_match =
        again.stats.resource_checksum == stats.resource_checksum &&
        again.stats.res_v5_checksum == stats.res_v5_checksum;
    const bool counts_match =
        stats.res_rw_reads > 0 && stats.res_rw_writes > 0 &&
        stats.res_rw_unlocks > 0 && stats.res_cv_waits > 0 &&
        (stats.res_cv_notifies + stats.res_cv_notify_alls) > 0 &&
        stats.res_wg_adds > 0 && stats.res_wg_dones > 0 &&
        stats.res_io_reads > 0 && stats.res_io_wakes > 0;
    if (!checksum_match || !counts_match) {
        std::printf("[v5-demo] MISMATCH checksum_match=%d counts_match=%d\n",
                    checksum_match ? 1 : 0, counts_match ? 1 : 0);
        return 3;
    }
    std::printf("[v5-demo] MATCH events=%llu rw=%llu/%llu cv=%llu wg=%llu "
                "io=%llu checksum=%llu\n",
                static_cast<unsigned long long>(stats.resource_events),
                static_cast<unsigned long long>(stats.res_rw_reads),
                static_cast<unsigned long long>(stats.res_rw_writes),
                static_cast<unsigned long long>(stats.res_cv_waits),
                static_cast<unsigned long long>(stats.res_wg_dones),
                static_cast<unsigned long long>(stats.res_io_reads),
                static_cast<unsigned long long>(stats.resource_checksum));
    return 0;
}
