/**
 * @file replay_v5_test_support.hpp
 * @brief 回放 v5 随机/压测/边界测试的公共辅助（仅测试使用）。
 *
 * 提供四类能力：
 *   1. 临时文件与「录制 -> 回放」的最短路径；
 *   2. **独立校验和**：不复用 replay_engine 的累计函数，按磁盘事件
 *      重新实现 FNV-1a 64，防止「统计与校验和同源、一起错」；
 *   3. 真实原语的等待/就绪轮询（仅在 GO2CPP_DEBUG_RESOURCES=1 时被调用）；
 *   4. 压测所需的 steady_clock / /proc/self/status 读数。
 */

#pragma once

#include "go2cpp/debug/debug_resources.hpp"
// 默认构建（GO2CPP_DEBUG_RESOURCES=0）下 rwmutex.hpp 的 Unlock() 里
// `woke` 只被调试事件宏使用，会触发 -Wunused-variable。测试包含该头
// 只是为了使用原语；在测试侧局部抑制这条既有警告，避免污染测试日志。
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "go2cpp/concurrency/rwmutex.hpp"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "go2cpp/io.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include "replay/format.hpp"
#include "replay/recorder.hpp"
#include "replay/replay_engine.hpp"

#include "test_support.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace go2cpp::replay::v5test {

using SteadyClock = std::chrono::steady_clock;

/// 临时录制文件路径：/tmp/go2cpp_replay_v5_<tag>_<pid>_<序号>.g2cr。
inline std::string TempPath(const char* tag) {
    static std::atomic<std::uint64_t> counter{0};
    return std::string("/tmp/go2cpp_replay_v5_") + tag + "_" +
           std::to_string(static_cast<long long>(::getpid())) + "_" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) +
           ".g2cr";
}

inline void RemoveFile(const std::string& path) { std::remove(path.c_str()); }

/// 构造一条 v5 资源事件；address 用一个稳定的伪地址，便于区分同类资源。
inline Event MakeV5(EventType type, std::uint64_t resource_id,
                    std::uint64_t actor_gid, std::uint64_t target_gid,
                    std::uint32_t result, std::uint32_t waiters,
                    std::int32_t fd, std::uint32_t bytes, std::uint64_t aux,
                    std::int64_t value) {
    return MakeResourceEvent(type, resource_id, actor_gid, target_gid, result,
                             0x1000U + resource_id, waiters, fd, bytes, aux,
                             value);
}

/// 一次录制 + 回放的结果。
struct RecordedFile {
    ReplayReport report;
    std::uint64_t records{0};  ///< Recorder 写出的事件条数（不含文件头/kEnd）
};

/**
 * @brief 把事件序列写入临时文件并回放。
 * @note 事件里的 seq 由 Recorder::Write 重新分配，调用方无需填写。
 */
inline RecordedFile RecordAndReplay(const std::string& path,
                                    const std::vector<Event>& events,
                                    bool keep_timeline = false,
                                    const char* demo = "replay-v5-test") {
    RecordedFile out;
    {
        RecordOptions options;
        options.path = path;
        options.demo_name = demo;
        options.gid_count = 64;
        Recorder recorder(options);
        GO2CPP_CHECK(recorder.ok());
        if (!recorder.ok()) {
            return out;
        }
        for (const auto& event : events) {
            GO2CPP_CHECK(recorder.Write(event));
        }
        recorder.Finalize("test");
        out.records = recorder.Records();
    }
    ReplayOptions replay_options;
    replay_options.keep_timeline = keep_timeline;
    out.report = ReplayFile(path, replay_options);
    return out;
}

/// 资源事件条数（与回放的 resource_events 定义一致）。
inline std::uint64_t CountResourceEvents(const std::vector<Event>& events) {
    std::uint64_t count = 0;
    for (const auto& event : events) {
        if (IsResourceEventType(event.type)) {
            ++count;
        }
    }
    return count;
}

inline std::uint64_t FnvStep(std::uint64_t hash, std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

/**
 * @brief 按回放引擎的序列化规则**独立重算**资源校验和。
 *
 * 与 replay_engine.cpp 的 AccumulateResourceEvent 逐字段一致：v4 的四步
 * （类型+资源号 / actor / target / result+waiters）进入 resource_checksum；
 * v5 扩展字段（fd/bytes/aux/value）单独进入 res_v5_checksum，见 V5Checksum。
 * 本函数刻意不复用回放代码，防止「统计与校验和同源、一起错」。
 */
inline std::uint64_t ResourceChecksum(const std::vector<Event>& events) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& event : events) {
        if (!IsResourceEventType(event.type)) {
            continue;
        }
        const std::uint64_t actor =
            event.actor_gid != 0U ? event.actor_gid : event.gid;
        hash = FnvStep(hash,
                       (static_cast<std::uint64_t>(event.type) << 48U) ^
                           (event.resource_id & 0xFFFFFFFFFFFFULL));
        hash = FnvStep(hash, actor);
        hash = FnvStep(hash, event.target_gid);
        hash = FnvStep(
            hash,
            (static_cast<std::uint64_t>(event.resource_result) << 32U) ^
                static_cast<std::uint64_t>(event.waiters));
    }
    return hash;
}

/// v5 四族事件的独立校验和（只累计 fd/bytes/aux/value）。
inline std::uint64_t V5Checksum(const std::vector<Event>& events) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& event : events) {
        if (!IsResourceEventV5(event.type)) {
            continue;
        }
        hash = FnvStep(
            hash,
            static_cast<std::uint64_t>(
                static_cast<std::int64_t>(event.res_fd)));
        hash = FnvStep(hash, event.res_bytes);
        hash = FnvStep(hash, event.res_aux);
        hash = FnvStep(hash, static_cast<std::uint64_t>(event.value));
    }
    return hash;
}

/// 打印一份固定的统计摘要（验收数据来源；字段顺序稳定）。
inline void PrintStats(const char* tag, const ReplayReport& report) {
    const ReplayStats& s = report.stats;
    std::printf(
        "[%s] status=%s complete=%d events=%llu checksum=%llu v5=%llu "
        "anomalies=%llu\n",
        tag, ReplayStatusName(report.status), report.complete ? 1 : 0,
        static_cast<unsigned long long>(s.resource_events),
        static_cast<unsigned long long>(s.resource_checksum),
        static_cast<unsigned long long>(s.res_v5_checksum),
        static_cast<unsigned long long>(s.resource_order_anomalies));
    std::printf(
        "[%s] rw reads=%llu writes=%llu unlocks=%llu read_blocks=%llu "
        "write_blocks=%llu\n",
        tag, static_cast<unsigned long long>(s.res_rw_reads),
        static_cast<unsigned long long>(s.res_rw_writes),
        static_cast<unsigned long long>(s.res_rw_unlocks),
        static_cast<unsigned long long>(s.res_rw_read_blocks),
        static_cast<unsigned long long>(s.res_rw_write_blocks));
    std::printf(
        "[%s] cv waits=%llu notifies=%llu notify_alls=%llu woken=%llu\n", tag,
        static_cast<unsigned long long>(s.res_cv_waits),
        static_cast<unsigned long long>(s.res_cv_notifies),
        static_cast<unsigned long long>(s.res_cv_notify_alls),
        static_cast<unsigned long long>(s.res_cv_woken));
    std::printf(
        "[%s] wg adds=%llu dones=%llu waits=%llu\n", tag,
        static_cast<unsigned long long>(s.res_wg_adds),
        static_cast<unsigned long long>(s.res_wg_dones),
        static_cast<unsigned long long>(s.res_wg_waits));
    std::printf(
        "[%s] io reads=%llu writes=%llu waits=%llu wakes=%llu bytes=%llu\n",
        tag, static_cast<unsigned long long>(s.res_io_reads),
        static_cast<unsigned long long>(s.res_io_writes),
        static_cast<unsigned long long>(s.res_io_waits),
        static_cast<unsigned long long>(s.res_io_wakes),
        static_cast<unsigned long long>(s.res_io_bytes));
}

/**
 * @brief 对「事件序列 -> 录制文件 -> 回放报告」做完整一致性断言。
 *
 * 断言项：状态、完整性、事件条数、独立校验和、seq 严格递增、
 * 二次回放（同一文件）的计数与校验和逐位一致。
 */
inline void ExpectConsistent(const std::string& path,
                             const std::vector<Event>& events,
                             const ReplayReport& report, const char* tag) {
    GO2CPP_CHECK(report.status == ReplayStatus::kOk);
    GO2CPP_CHECK(report.complete);
    const std::uint64_t expected = CountResourceEvents(events);
    GO2CPP_CHECK(report.stats.resource_events == expected);
    GO2CPP_CHECK(report.resources.size() == expected);
    const std::uint64_t independent = ResourceChecksum(events);
    if (report.stats.resource_checksum != independent) {
        std::fprintf(stderr,
                     "[%s] checksum mismatch: replay=%llu independent=%llu\n",
                     tag,
                     static_cast<unsigned long long>(
                         report.stats.resource_checksum),
                     static_cast<unsigned long long>(independent));
    }
    GO2CPP_CHECK(report.stats.resource_checksum == independent);
    const std::uint64_t independent_v5 = V5Checksum(events);
    if (report.stats.res_v5_checksum != independent_v5) {
        std::fprintf(stderr,
                     "[%s] v5 checksum mismatch: replay=%llu independent=%llu\n",
                     tag,
                     static_cast<unsigned long long>(
                         report.stats.res_v5_checksum),
                     static_cast<unsigned long long>(independent_v5));
    }
    GO2CPP_CHECK(report.stats.res_v5_checksum == independent_v5);
    for (std::size_t index = 1; index < report.resources.size(); ++index) {
        GO2CPP_CHECK(report.resources[index].seq >
                     report.resources[index - 1].seq);
    }
    const ReplayReport again = ReplayFile(path);
    GO2CPP_CHECK(again.stats.resource_events ==
                 report.stats.resource_events);
    GO2CPP_CHECK(again.stats.resource_checksum ==
                 report.stats.resource_checksum);
    GO2CPP_CHECK(again.stats.res_v5_checksum ==
                 report.stats.res_v5_checksum);
}

/// 从磁盘重新加载事件，再做 ExpectConsistent（真实录制场景用）。
inline void ExpectFileConsistent(const std::string& path,
                                 const ReplayReport& report,
                                 const char* tag) {
    std::vector<Event> events;
    std::string error;
    GO2CPP_CHECK(LoadRecordingEvents(path, &events, &error));
    GO2CPP_CHECK(error.empty());
    ExpectConsistent(path, events, report, tag);
}

/// 从 /proc/self/status 读取一个 kB 字段（找不到返回 0）。
inline std::uint64_t ReadStatusKb(const char* key) {
    std::ifstream input("/proc/self/status");
    if (!input) {
        return 0;
    }
    std::string line;
    const std::size_t key_len = std::strlen(key);
    while (std::getline(input, line)) {
        if (line.compare(0, key_len, key) == 0) {
            unsigned long long value = 0;
            if (std::sscanf(line.c_str() + key_len, "%llu", &value) == 1) {
                return static_cast<std::uint64_t>(value);
            }
        }
    }
    return 0;
}

inline std::uint64_t RssKb() { return ReadStatusKb("VmRSS:"); }
inline std::uint64_t PeakRssKb() { return ReadStatusKb("VmHWM:"); }

inline std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            SteadyClock::now().time_since_epoch())
            .count());
}

/// 轮询等待原子标志（毫秒级；超时返回 false，不 abort）。
inline bool WaitForFlag(const std::atomic<bool>& flag, int timeout_ms = 3000) {
    for (int elapsed = 0; elapsed < timeout_ms && !flag.load(); ++elapsed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load();
}

/// 轮询等待 CV 的等待者数量达到期望值（使用原语公开的调试快照）。
inline bool WaitForCvWaiters(const go2cpp::sync::ConditionVariable& cv,
                             std::size_t want, int timeout_ms = 3000) {
    for (int elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        if (cv.DebugSnapshotInternal().waiters == want) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// 轮询等待 WaitGroup 的等待者数量达到期望值。
inline bool WaitForWgWaiters(const go2cpp::sync::WaitGroup& wg,
                             std::size_t want, int timeout_ms = 3000) {
    for (int elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        if (wg.DebugSnapshotInternal().waiters == want) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// 固定种子的随机源（跨进程可复现；测试失败时给出种子）。
class Rng {
public:
    explicit Rng(std::uint64_t seed) : m_engine(seed) {}

    std::uint64_t Next() { return m_engine(); }
    std::uint32_t Below(std::uint32_t bound) {
        return bound == 0U ? 0U
                           : static_cast<std::uint32_t>(m_engine() % bound);
    }
    bool Chance(std::uint32_t percent) { return Below(100U) < percent; }

private:
    std::mt19937_64 m_engine;
};

}  // namespace go2cpp::replay::v5test
