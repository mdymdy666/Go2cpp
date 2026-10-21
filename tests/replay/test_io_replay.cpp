/**
 * @file test_io_replay.cpp
 * @brief IOManager（pipe）的录制/回放随机与边界测试。
 *
 * 场景：
 *   1. 合成序列：read/write/wait/wake 四类计数、fd/bytes 字段、超时结果、
 *      未知 fd（-1）与 bytes 累计；
 *   2. 真实 pipe（仅 GO2CPP_DEBUG_RESOURCES=1）：就绪读、阻塞读（对端写入
 *      后唤醒）、超时读（80ms）、写方向就绪。IOManager 只等就绪不搬运数据，
 *      因此 res_io_bytes 恒为 0（在测试输出中注明）。
 */

#include "replay_v5_test_support.hpp"

#include <unistd.h>
#include <sys/syscall.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace go2cpp::replay;
using namespace go2cpp::replay::v5test;

/// 场景 1：合成 IO 事件序列。
void TestSyntheticIoSequence() {
    const std::string path = TempPath("io_synth");
    const auto read = static_cast<std::uint64_t>(ResourceAuxFlags::kRead);
    const auto write = static_cast<std::uint64_t>(ResourceAuxFlags::kWrite);
    std::vector<Event> events;
    events.push_back(MakeV5(EventType::kResIoRead, 400, 1, 0, 0, 0, 5, 0, read,
                            0));
    events.push_back(MakeV5(EventType::kResIoWait, 400, 1, 0, 1, 1, 5, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResIoWake, 400, 0, 1, 0, 0, 5, 0, 0, 0));
    events.push_back(MakeV5(EventType::kResIoWrite, 400, 2, 0, 0, 0, 6, 0,
                            write, 0));
    // 超时读：result=3，fd=7。
    events.push_back(MakeV5(EventType::kResIoRead, 400, 3, 0, 3, 0, 7, 0, read,
                            0));
    events.push_back(MakeV5(EventType::kResIoWait, 400, 3, 0, 1, 1, 7, 0, 0, 0));
    // 完成字节：wake 带 8 字节。
    events.push_back(MakeV5(EventType::kResIoWake, 400, 0, 3, 3, 0, 7, 8, 0, 0));
    // 边界：未知 fd（-1）与 16 字节的 read 请求。
    events.push_back(MakeV5(EventType::kResIoRead, 400, 4, 0, 0, 0, -1, 16,
                            read, 0));

    const RecordedFile recorded = RecordAndReplay(path, events);
    const ReplayReport& report = recorded.report;
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.complete);
    GO2CPP_CHECK(report.stats.resource_events == events.size());
    GO2CPP_CHECK(report.stats.res_io_reads == 3U);
    GO2CPP_CHECK(report.stats.res_io_writes == 1U);
    GO2CPP_CHECK(report.stats.res_io_waits == 2U);
    GO2CPP_CHECK(report.stats.res_io_wakes == 2U);
    GO2CPP_CHECK(report.stats.res_io_bytes == 24U);
    GO2CPP_CHECK(report.stats.resource_order_anomalies == 0U);
    GO2CPP_CHECK(report.resources.size() == events.size());
    GO2CPP_CHECK(report.resources[0].fd == 5);
    GO2CPP_CHECK(report.resources[0].aux == read);
    GO2CPP_CHECK(report.resources[3].aux == write);
    GO2CPP_CHECK(report.resources[4].result == 3U);
    GO2CPP_CHECK(report.resources[6].bytes == 8U);
    GO2CPP_CHECK(report.resources[7].fd == -1);
    GO2CPP_CHECK(report.resources[7].bytes == 16U);
    ExpectConsistent(path, events, report, "io_synth");
    PrintStats("io_synth", report);
    RemoveFile(path);
}

#if GO2CPP_DEBUG_RESOURCES
namespace {

/// 一个 pipe 场景的句柄；析构时关闭两端。
struct PipePair {
    int read_fd{-1};
    int write_fd{-1};
    bool Open() {
        int fds[2]{-1, -1};
        if (::pipe(fds) != 0) {
            return false;
        }
        read_fd = fds[0];
        write_fd = fds[1];
        return true;
    }
    ~PipePair() {
        if (read_fd >= 0) {
            ::close(read_fd);
        }
        if (write_fd >= 0) {
            ::close(write_fd);
        }
    }
};

}  // namespace

/// 场景 2：真实 pipe 的 ready / blocking / timeout / write-ready。
void TestRealIo() {
    const std::string path = TempPath("io_real");
    {
        RecordOptions options;
        options.path = path;
        options.demo_name = "io-manager-real";
        options.gid_count = 32;
        Recorder recorder(options);
        GO2CPP_CHECK(recorder.ok());
        if (!recorder.ok()) {
            return;
        }

        go2cpp::SchedulerConfig config;
        config.processor_count = 1;
        config.max_workers = 1;
        go2cpp::IOManager manager(config);
        GO2CPP_CHECK(manager.Start());

        // 1. 就绪读：先写 1 字节，再 WaitFor(kRead)。
        {
            PipePair pipe;
            GO2CPP_CHECK(pipe.Open());
            const char byte = 'r';
            GO2CPP_CHECK(::syscall(SYS_write, pipe.write_fd, &byte, 1) == 1);
            std::atomic<bool> done{false};
            manager.Go([&] {
                (void)manager.WaitFor(pipe.read_fd, go2cpp::IOEvent::kRead,
                                      std::chrono::seconds(2));
                char out = 0;
                (void)::syscall(SYS_read, pipe.read_fd, &out, 1);
                done.store(true, std::memory_order_release);
            });
            GO2CPP_CHECK(WaitForFlag(done));
        }
        // 2. 阻塞读：等待者先 park，主线程随后写入。
        {
            PipePair pipe;
            GO2CPP_CHECK(pipe.Open());
            std::atomic<bool> started{false};
            std::atomic<bool> done{false};
            manager.Go([&] {
                started.store(true, std::memory_order_release);
                (void)manager.WaitFor(pipe.read_fd, go2cpp::IOEvent::kRead,
                                      std::chrono::seconds(2));
                done.store(true, std::memory_order_release);
            });
            GO2CPP_CHECK(WaitForFlag(started));
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            const char byte = 'b';
            GO2CPP_CHECK(::syscall(SYS_write, pipe.write_fd, &byte, 1) == 1);
            GO2CPP_CHECK(WaitForFlag(done));
        }
        // 3. 超时读：80ms 内无数据。
        {
            PipePair pipe;
            GO2CPP_CHECK(pipe.Open());
            std::atomic<bool> done{false};
            std::atomic<int> status{-1};
            manager.Go([&] {
                const auto result = manager.WaitFor(
                    pipe.read_fd, go2cpp::IOEvent::kRead,
                    std::chrono::milliseconds(80));
                status.store(static_cast<int>(result.status));
                done.store(true, std::memory_order_release);
            });
            GO2CPP_CHECK(WaitForFlag(done));
            GO2CPP_CHECK(status.load() == static_cast<int>(
                                              go2cpp::io::WaitStatus::kTimeout));
            std::printf("[io_real] timeout_status=%d\n", status.load());
        }
        // 4. 写方向就绪：pipe 写端缓冲区为空。
        {
            PipePair pipe;
            GO2CPP_CHECK(pipe.Open());
            std::atomic<bool> done{false};
            manager.Go([&] {
                (void)manager.WaitFor(pipe.write_fd,
                                      go2cpp::IOEvent::kWrite,
                                      std::chrono::seconds(2));
                done.store(true, std::memory_order_release);
            });
            GO2CPP_CHECK(WaitForFlag(done));
        }

        manager.Shutdown();
        recorder.Finalize("test");
        std::printf("[io_real] recorded=%llu\n",
                    static_cast<unsigned long long>(recorder.Records()));
    }

    const ReplayReport report = ReplayFile(path);
    const ReplayStats& stats = report.stats;
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.complete);
    // 4 次 WaitFor：3 次读方向、1 次写方向；IOManager 不搬运数据，bytes=0。
    GO2CPP_CHECK(stats.res_io_reads == 3U);
    GO2CPP_CHECK(stats.res_io_writes == 1U);
    GO2CPP_CHECK(stats.res_io_bytes == 0U);
    // 每个真正 park 的等待最终都会被唤醒（就绪/超时/取消路径一致）。
    GO2CPP_CHECK(stats.res_io_waits == stats.res_io_wakes);
    GO2CPP_CHECK(stats.res_io_waits >= 2U);
    GO2CPP_CHECK(stats.resource_order_anomalies == 0U);
    std::printf("[io_real] waits=%llu wakes=%llu bytes=%llu\n",
                static_cast<unsigned long long>(stats.res_io_waits),
                static_cast<unsigned long long>(stats.res_io_wakes),
                static_cast<unsigned long long>(stats.res_io_bytes));
    ExpectFileConsistent(path, report, "io_real");
    PrintStats("io_real", report);
    RemoveFile(path);
}
#endif  // GO2CPP_DEBUG_RESOURCES

}  // namespace

void run_io_replay_tests() {
    go2cpp_tests::announce("io_replay");
    TestSyntheticIoSequence();
#if GO2CPP_DEBUG_RESOURCES
    TestRealIo();
#else
    std::printf("[io_replay] skip real IOManager (GO2CPP_DEBUG_RESOURCES=0)\n");
#endif
}
