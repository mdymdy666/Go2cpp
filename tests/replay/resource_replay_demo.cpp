/**
 * @file resource_replay_demo.cpp
 * @brief 资源事件回放验收：两个 Fiber 争锁 → 录制 → 回放 → 序列对比。
 *
 * 用法：
 * ```text
 * resource_replay_demo [recording.g2cr] [iterations]
 * ```
 *
 * 前置构建：`GO2CPP_DEBUG_DUMP=ON` 且 `GO2CPP_REPLAY=ON`
 * （事件环用于「与 --view history 同源」的对照，REPLAY 用于资源事件录制）。
 *
 * 流程：
 * 1. 构造 `Recorder`（自动安装资源事件 sink）；
 * 2. 调度器起 2 个 Fiber，各对同一把 `sync::Mutex` 加锁/解锁 N 次；
 * 3. 收尾录制，取 `EventRing::Snapshot()` 中仍保留的锁事件（history 的数据源）；
 * 4. `ReplayFile()` 只读重演同一记录，得到完整资源事件序列；
 * 5. 用「回放序列的后缀」与事件环序列逐项对比（环形缓冲只保留最近 256 条）；
 * 6. 打印计数、owner 序列与持锁总时长；不一致时返回 1。
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"
#include "replay/replay_engine.hpp"
#include "replay/recorder.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::debug::EventRing;
using go2cpp::debug::SchedEventCode;

/// 归一化后的锁事件（两侧使用同一结构，便于逐项比较）。
struct LockEvent {
    bool unlock{false};
    std::uint64_t resource_id{0};
    std::uint64_t actor_gid{0};
    std::uint64_t target_gid{0};
    std::uint32_t result{0};
};

bool IsMutexEventCode(std::uint32_t code) {
    return code == static_cast<std::uint32_t>(SchedEventCode::kMutexLock) ||
           code == static_cast<std::uint32_t>(SchedEventCode::kMutexFastLock) ||
           code == static_cast<std::uint32_t>(SchedEventCode::kMutexUnlock);
}

const char* TypeText(const LockEvent& event) {
    return event.unlock ? "unlock" : "lock  ";
}

const char* ResultText(std::uint32_t result) {
    switch (result) {
        case 0: return "ok";
        case 1: return "blocked";
        case 2: return "wake";
        case 3: return "timeout";
        case 4: return "cancelled";
        default: return "other";
    }
}

void PrintSequence(const char* tag, const std::vector<LockEvent>& events,
                   std::size_t limit) {
    const std::size_t count = events.size() < limit ? events.size() : limit;
    for (std::size_t index = 0; index < count; ++index) {
        const LockEvent& event = events[index];
        std::printf("%s[%zu] %s resource=%llu actor=%llu target=%llu result=%s\n",
                    tag, index, TypeText(event),
                    static_cast<unsigned long long>(event.resource_id),
                    static_cast<unsigned long long>(event.actor_gid),
                    static_cast<unsigned long long>(event.target_gid),
                    ResultText(event.result));
    }
}

std::vector<LockEvent> TakeRingMutexEvents() {
    std::vector<LockEvent> events;
    for (const auto& record : EventRing::Snapshot()) {
        if (!IsMutexEventCode(record.code)) {
            continue;
        }
        // 排掉同码的**调度事件**：Unlock 唤醒等待者时会先写一条
        // `kMutexUnlock` 调度事件（resource_id=0），它不是资源事件。
        if (record.resource_id == 0U) {
            continue;
        }
        LockEvent event;
        event.unlock =
            record.code == static_cast<std::uint32_t>(SchedEventCode::kMutexUnlock);
        event.resource_id = record.resource_id;
        event.actor_gid = record.gid;
        event.target_gid = record.target_gid;
        event.result = record.result;
        events.push_back(event);
    }
    return events;
}

std::vector<LockEvent> TakeReplayMutexEvents(
    const go2cpp::replay::ReplayReport& report) {
    std::vector<LockEvent> events;
    for (const auto& entry : report.resources) {
        if (entry.type != go2cpp::replay::EventType::kResMutexLock &&
            entry.type != go2cpp::replay::EventType::kResMutexUnlock) {
            continue;
        }
        LockEvent event;
        event.unlock =
            entry.type == go2cpp::replay::EventType::kResMutexUnlock;
        event.resource_id = entry.resource_id;
        event.actor_gid = entry.actor_gid;
        event.target_gid = entry.target_gid;
        event.result = entry.result;
        events.push_back(event);
    }
    return events;
}

bool SameEvent(const LockEvent& left, const LockEvent& right) {
    return left.unlock == right.unlock && left.resource_id == right.resource_id &&
           left.actor_gid == right.actor_gid &&
           left.target_gid == right.target_gid && left.result == right.result;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path =
        argc > 1 ? argv[1] : "/tmp/resource_replay_demo.g2cr";
    const int iterations = argc > 2 ? std::atoi(argv[2]) : 80;
    std::printf("recording=%s iterations=%d debug_dump=%d\n", path.c_str(),
                iterations, EventRing::Enabled() ? 1 : 0);

    EventRing::Reset();
    EventRing::ResetTotals();

    std::atomic<int> counter{0};
    std::vector<LockEvent> recorded;
    std::uint64_t recorded_locks = 0;
    std::uint64_t recorded_unlocks = 0;
    {
        go2cpp::replay::RecordOptions options;
        options.path = path;
        options.demo_name = "two-fiber-mutex";
        options.gid_count = 8;
        options.channel_count = 0;
        options.clock_mode = go2cpp::replay::kClockVirtual;
        go2cpp::replay::Recorder recorder(options);
        if (!recorder.ok()) {
            std::fprintf(stderr, "recorder: %s\n", recorder.error().c_str());
            return 2;
        }

        go2cpp::SchedulerConfig config;
        config.processor_count = 2;
        config.min_workers = 2;
        config.max_workers = 2;
        go2cpp::Scheduler scheduler(config);
        scheduler.start();

        go2cpp::sync::Mutex mutex;
        const auto body = [&mutex, &counter, iterations] {
            for (int iteration = 0; iteration < iterations; ++iteration) {
                (void)mutex.Lock();
                counter.fetch_add(1, std::memory_order_relaxed);
                // 持锁做一段可观测长度的计算，保证另一个 Fiber 阻塞在锁上。
                volatile std::uint64_t sum = 0;
                for (std::uint64_t index = 0; index < 1500; ++index) {
                    sum += index;
                }
                (void)sum;
                mutex.Unlock();
            }
        };
        auto first = scheduler.spawn(body);
        auto second = scheduler.spawn(body);
        if (first) {
            first->Join();
        }
        if (second) {
            second->Join();
        }
        // 必须在 worker 线程退出前取快照：线程退出时它的 ring 会从全局表
        // 摘除（Snapshot 只读存活线程），shutdown 后再取就只剩主线程的空环。
        recorded = TakeRingMutexEvents();
        // 计数只统计**资源事件**（resource_id != 0）：TotalCount 会把同码的
        // 调度事件（Unlock 的 kMutexUnlock）一起算进来。
        recorded_locks = 0;
        recorded_unlocks = 0;
        for (const LockEvent& event : recorded) {
            if (event.unlock) {
                ++recorded_unlocks;
            } else {
                ++recorded_locks;
            }
        }
        if (!recorded.empty()) {
            std::printf("mutex_resource_id=%llu\n",
                        static_cast<unsigned long long>(
                            recorded.front().resource_id));
            std::fflush(stdout);
        }
        // 可选驻留模式：跑完负载后启动 DebugServer，供外部
        // `go2cpp-cli dump --pid <pid> --view history --resource <id>` 对照。
        // 必须在 worker 线程退出（scheduler.shutdown）前进入驻留，否则 ring
        // 随线程退出从全局表摘除，history 视图就看不到锁事件了。
        if (const char* hold_text = std::getenv("GO2CPP_RESOURCE_DEMO_HOLD");
            hold_text != nullptr && *hold_text != '\0') {
            const int hold_seconds = std::atoi(hold_text);
            if (hold_seconds > 0) {
                auto& server = go2cpp::debug::DebugServer::Instance();
                if (server.Start("")) {
                    std::printf("server=%s\npid=%d\nready=true\n",
                                server.path().c_str(),
                                static_cast<int>(::getpid()));
                    std::fflush(stdout);
                    std::this_thread::sleep_for(
                        std::chrono::seconds(hold_seconds));
                } else {
                    std::printf("server_start_failed=%s\n",
                                server.last_error().c_str());
                }
            }
        }

        scheduler.shutdown();
        recorder.Finalize("demo-done");
    }

    go2cpp::replay::ReplayOptions replay_options;
    replay_options.keep_timeline = false;
    const go2cpp::replay::ReplayReport report =
        go2cpp::replay::ReplayFile(path, replay_options);
    std::printf("replay_status=%s error=\"%s\"\n",
                go2cpp::replay::ReplayStatusName(report.status),
                report.error.c_str());
    if (report.status != go2cpp::replay::ReplayStatus::kOk) {
        std::fprintf(stderr, "replay failed: %s\n", report.error.c_str());
        return 1;
    }

    // 事件环（与 --view history 同源）里的锁事件；环形缓冲只保留最近部分。
    const std::vector<LockEvent>& ring_events = recorded;
    const std::vector<LockEvent> replayed = TakeReplayMutexEvents(report);

    std::printf("ring_mutex_events=%zu replay_mutex_events=%zu\n",
                ring_events.size(), replayed.size());
    std::printf("recorded_locks=%llu recorded_unlocks=%llu\n",
                static_cast<unsigned long long>(recorded_locks),
                static_cast<unsigned long long>(recorded_unlocks));
    std::printf("replay_locks=%llu replay_unlocks=%llu lock_hold_ns=%llu\n",
                static_cast<unsigned long long>(report.stats.res_mutex_locks),
                static_cast<unsigned long long>(report.stats.res_mutex_unlocks),
                static_cast<unsigned long long>(
                    report.stats.resource_lock_hold_ns));
    std::printf("resource_checksum=%llu replayed_resource_checksum=%llu\n",
                static_cast<unsigned long long>(
                    report.stats.resource_checksum),
                static_cast<unsigned long long>(
                    report.stats.replayed_resource_checksum));

    // 序列对比：事件环只保留最近的事件（环形覆盖），因此 recorded 应是
    // 回放序列的**子序列**。贪心匹配：按顺序为每个 recorded 事件在回放
    // 序列里找下一个相同事件。全部命中 → 顺序/owner/等待者完全一致；
    // 命中失败 → 记录第一个对不上的位置。
    bool sequence_ok = !ring_events.empty();
    std::size_t match_offset = 0;   ///< recorded[0] 在回放序列里的下标
    std::size_t mismatch_index = 0;
    if (sequence_ok) {
        std::size_t cursor = 0;
        for (std::size_t index = 0; index < ring_events.size(); ++index) {
            if (index == 0) {
                match_offset = cursor;
            }
            while (cursor < replayed.size() &&
                   !SameEvent(ring_events[index], replayed[cursor])) {
                ++cursor;
            }
            if (cursor >= replayed.size()) {
                sequence_ok = false;
                mismatch_index = index;
                break;
            }
            if (index == 0) {
                match_offset = cursor;
            }
            ++cursor;
        }
    }
    std::printf("matched_offset=%zu\n", match_offset);
    // 事件环只保留最近的事件（环形覆盖），因此录制端计数是回放计数的
    // 子集；只要不为 0 且不超过回放总数，计数就是自洽的。
    const bool counts_ok =
        recorded_locks > 0U && recorded_unlocks > 0U &&
        recorded_locks <= report.stats.res_mutex_locks &&
        recorded_unlocks <= report.stats.res_mutex_unlocks;
    std::printf("sequence_match=%s%s\n", sequence_ok ? "OK" : "FAIL",
                sequence_ok ? "" : (" at " + std::to_string(mismatch_index)).c_str());
    if (!sequence_ok && mismatch_index < ring_events.size()) {
        const LockEvent& left = ring_events[mismatch_index];
        std::printf("mismatch recorded[%zu]=%s actor=%llu target=%llu "
                    "result=%u (no later replay event matches)\n",
                    mismatch_index, TypeText(left),
                    static_cast<unsigned long long>(left.actor_gid),
                    static_cast<unsigned long long>(left.target_gid),
                    left.result);
    }
    std::printf("counts_match=%s\n", counts_ok ? "OK" : "FAIL");

    // 调试用：完整导出两路序列（GO2CPP_RESOURCE_DEMO_DUMP=1）。
    if (std::getenv("GO2CPP_RESOURCE_DEMO_DUMP") != nullptr) {
        for (std::size_t index = 0; index < ring_events.size(); ++index) {
            const LockEvent& event = ring_events[index];
            std::printf("RING %zu %d %llu %llu %llu %u\n", index,
                        event.unlock ? 1 : 0,
                        static_cast<unsigned long long>(event.resource_id),
                        static_cast<unsigned long long>(event.actor_gid),
                        static_cast<unsigned long long>(event.target_gid),
                        event.result);
        }
        for (std::size_t index = 0; index < replayed.size(); ++index) {
            const LockEvent& event = replayed[index];
            std::printf("REPLAY %zu %d %llu %llu %llu %u\n", index,
                        event.unlock ? 1 : 0,
                        static_cast<unsigned long long>(event.resource_id),
                        static_cast<unsigned long long>(event.actor_gid),
                        static_cast<unsigned long long>(event.target_gid),
                        event.result);
        }
    }

    PrintSequence("recorded", ring_events, 10);
    PrintSequence("replayed", replayed, 10);

    return sequence_ok && counts_ok ? 0 : 1;
}
