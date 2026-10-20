/**
 * @file category_filter_demo.cpp
 * @brief 粒度配置验收 demo：同一负载在「全开」与「只看 mutex」下的类别计数。
 *
 * 用法：
 * ```text
 * category_filter_demo
 * ```
 *
 * 负载：2 个受管 Fiber 争用同一把 `sync::Mutex`，各加锁/解锁 200 次。
 * 先 `EventRing::ResetTotals()` 再启动调度器，因此计数只包含本 demo 的事件。
 *
 * 输出（stdout，供 run_category_filter_demo.sh 断言）：
 * ```text
 * config lifecycle=1 sched=1 park=1 primitives=1 timer=1 io=1 stacks=1 chains=1 sample_shift=0
 * category lifecycle=...
 * ...
 * event kMutexLock=...
 * ...
 * ```
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

using go2cpp::debug::DebugCategory;
using go2cpp::debug::DebugCategoryName;
using go2cpp::debug::DebugCategoryOf;
using go2cpp::debug::DebugConfig;
using go2cpp::debug::EventRing;
using go2cpp::debug::SchedEventCode;

void PrintConfig() {
    const DebugConfig config = DebugConfig::Get();
    std::printf(
        "config lifecycle=%d sched=%d park=%d primitives=%d timer=%d io=%d "
        "stacks=%d chains=%d sample_shift=%u\n",
        config.lifecycle ? 1 : 0, config.sched ? 1 : 0, config.park ? 1 : 0,
        config.primitives ? 1 : 0, config.timer ? 1 : 0, config.io ? 1 : 0,
        config.stacks ? 1 : 0, config.chains ? 1 : 0, config.sample_shift);
}

struct CategoryTotal {
    DebugCategory category;
    std::uint64_t count;
};

void PrintCounts() {
    CategoryTotal totals[] = {
        {DebugCategory::kLifecycle, 0}, {DebugCategory::kSched, 0},
        {DebugCategory::kPark, 0},      {DebugCategory::kPrimitives, 0},
        {DebugCategory::kTimer, 0},     {DebugCategory::kIo, 0}};
    std::uint64_t uncategorized = 0;
    std::uint64_t all = 0;
    for (std::uint32_t value = 0; value <= 0xFFU; ++value) {
        const auto code = static_cast<SchedEventCode>(value);
        const std::uint64_t count = EventRing::TotalCount(code);
        if (count == 0) {
            continue;
        }
        all += count;
        const DebugCategory category = DebugCategoryOf(code);
        bool placed = false;
        for (auto& total : totals) {
            if (total.category == category) {
                total.count += count;
                placed = true;
                break;
            }
        }
        if (!placed) {
            uncategorized += count;
        }
    }
    for (const auto& total : totals) {
        std::printf("category %s=%llu\n", DebugCategoryName(total.category),
                    static_cast<unsigned long long>(total.count));
    }
    std::printf("category none=%llu\n",
                static_cast<unsigned long long>(uncategorized));
    std::printf("events total=%llu\n", static_cast<unsigned long long>(all));
    const SchedEventCode detail[] = {
        SchedEventCode::kSchedulerStart, SchedEventCode::kSchedulerStop,
        SchedEventCode::kGCreated,       SchedEventCode::kGStarted,
        SchedEventCode::kEnqueue,        SchedEventCode::kParkEnter,
        SchedEventCode::kParkReturn,     SchedEventCode::kWakeEnqueued,
        SchedEventCode::kMutexLock,      SchedEventCode::kMutexUnlock,
        SchedEventCode::kMutexFastLock,  SchedEventCode::kTimerArm,
        SchedEventCode::kIoWait};
    for (const SchedEventCode code : detail) {
        std::printf("event %s=%llu\n",
                    go2cpp::debug::SchedEventCodeName(code),
                    static_cast<unsigned long long>(
                        EventRing::TotalCount(code)));
    }
}

}  // namespace

int main() {
    std::printf("debug_dump=%d\n", EventRing::Enabled() ? 1 : 0);
    EventRing::Reset();
    EventRing::ResetTotals();

    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    go2cpp::sync::Mutex mutex;
    std::atomic<int> counter{0};
    const auto body = [&mutex, &counter] {
        for (int iteration = 0; iteration < 200; ++iteration) {
            (void)mutex.Lock();
            counter.fetch_add(1, std::memory_order_relaxed);
            // 持锁做一段可观测长度的计算，保证另一个 Fiber 会阻塞在锁上。
            volatile std::uint64_t sum = 0;
            for (std::uint64_t index = 0; index < 2000; ++index) {
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
    scheduler.shutdown();
    std::printf("counter=%d\n", counter.load(std::memory_order_relaxed));

    PrintConfig();
    PrintCounts();
    return 0;
}
