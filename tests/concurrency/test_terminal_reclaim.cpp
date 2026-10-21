/**
 * @file tests/concurrency/test_terminal_reclaim.cpp
 * @brief D2 回归：常规 worker 循环必须周期性回收终态 G。
 *
 * ## 背景
 *
 * `collect_terminal_locked()` 过去只在 shutdown/cancel 收尾路径调用，常规
 * worker 主循环不回收；终态 G 被 `task_registries` 强持有，RSS 随历史完成
 * 任务数线性增长（D2，见 tests/enhanced/defects.md）。
 *
 * ## 覆盖
 *
 * 1. `terminal_collect_interval=1`：每个终态 G 都会推动一次条带轮转，完成
 *    后的 G 必须从注册表消失（`TaskTerminal()` 返回 kUnknown）；
 * 2. `terminal_collect_interval=0`：关闭周期回收后终态 G 保留，证明 1 的
 *    回收确实来自新节拍而不是别的收尾路径；
 * 3. 默认节拍 256：大批短任务完成后注册表不随历史任务数累积；
 * 4. 持锁退出的终态 G 必须保留：Doctor force-unlock 依赖
 *    `TaskTerminal()` 的 owner-gone 证据，不能被回收掉；
 * 5. `DebugSnapshotTasks()` 仍只返回非终态 G（D2 已修部分不回退）。
 */

#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;

std::size_t CountUnknown(go2cpp::Scheduler& scheduler,
                         const std::vector<go2cpp::GId>& gids) {
    std::size_t unknown = 0;
    for (const auto gid : gids) {
        if (scheduler.TaskTerminal(gid) ==
            go2cpp::TaskTerminalState::kUnknown) {
            ++unknown;
        }
    }
    return unknown;
}

go2cpp::SchedulerConfig SingleWorkerConfig(std::size_t interval) {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    config.collect_metrics = false;
    config.terminal_collect_interval = interval;
    return config;
}

/// 每个完成都触发条带轮转：完成之后的 16 个节拍内必然轮到它的条带。
void TestEveryCompletionReclaims() {
    go2cpp_tests::announce("terminal G reclaimed on every completion tick");
    go2cpp::Scheduler scheduler(SingleWorkerConfig(1));
    scheduler.start();

    constexpr int kChecked = 256;
    constexpr int kFlush = 32;  // 让最后完成的一批 G 也轮到自己的条带
    std::atomic<int> done{0};
    std::vector<std::shared_ptr<go2cpp::Task>> handles;
    std::vector<go2cpp::GId> gids;
    handles.reserve(kChecked + kFlush);
    gids.reserve(kChecked);
    for (int index = 0; index < kChecked + kFlush; ++index) {
        auto task = scheduler.spawn(
            [&done] { done.fetch_add(1, std::memory_order_relaxed); });
        GO2CPP_REQUIRE(static_cast<bool>(task));
        handles.push_back(task);
        if (index < kChecked) {
            gids.push_back(task->id());
        }
    }
    GO2CPP_REQUIRE_EVENTUALLY(
        done.load(std::memory_order_acquire) == kChecked + kFlush, 5s);
    GO2CPP_REQUIRE_EVENTUALLY(CountUnknown(scheduler, gids) == gids.size(),
                              5s);
    GO2CPP_CHECK(scheduler.DebugSnapshotTasks().empty());
    scheduler.shutdown();
}

/// 关闭周期回收时，终态 G 仍留在注册表；作为上一条用例的对照。
void TestDisabledIntervalKeepsTerminal() {
    go2cpp_tests::announce("terminal G retained when reclaim tick is disabled");
    go2cpp::Scheduler scheduler(SingleWorkerConfig(0));
    scheduler.start();

    constexpr int kTasks = 64;
    std::atomic<int> done{0};
    std::vector<std::shared_ptr<go2cpp::Task>> handles;
    std::vector<go2cpp::GId> gids;
    handles.reserve(kTasks);
    gids.reserve(kTasks);
    for (int index = 0; index < kTasks; ++index) {
        auto task = scheduler.spawn(
            [&done] { done.fetch_add(1, std::memory_order_relaxed); });
        GO2CPP_REQUIRE(static_cast<bool>(task));
        handles.push_back(task);
        gids.push_back(task->id());
    }
    GO2CPP_REQUIRE_EVENTUALLY(done.load(std::memory_order_acquire) == kTasks,
                              5s);
    // 没有周期回收，也没有 claim 失败收尾路径：终态条目应当仍在表内。
    GO2CPP_CHECK(CountUnknown(scheduler, gids) == 0U);
    scheduler.shutdown();
}

/// 生产默认节拍：大批短任务完成后，注册表不随历史任务数线性累积。
void TestDefaultIntervalBoundsGrowth() {
    go2cpp_tests::announce("default terminal collect interval bounds registry");
    go2cpp::SchedulerConfig config = SingleWorkerConfig(0);
    config.terminal_collect_interval = 256;  // 与 SchedulerConfig 默认值一致
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    constexpr int kTasks = 8192;
    std::atomic<int> done{0};
    std::vector<std::shared_ptr<go2cpp::Task>> handles;
    std::vector<go2cpp::GId> gids;
    handles.reserve(kTasks);
    gids.reserve(kTasks);
    for (int index = 0; index < kTasks; ++index) {
        auto task = scheduler.spawn(
            [&done] { done.fetch_add(1, std::memory_order_relaxed); });
        GO2CPP_REQUIRE(static_cast<bool>(task));
        handles.push_back(task);
        gids.push_back(task->id());
    }
    GO2CPP_REQUIRE_EVENTUALLY(done.load(std::memory_order_acquire) == kTasks,
                              10s);
    // 8192 个完成产生 32 次条带轮转 = 每条带 2 轮。终态 G 的滞留上限是
    // interval * 16（默认 4096）：最后一批完成的任务要等自己的条带再次
    // 轮到，因此不会全部立刻消失；但至少一半应已回收，且残留不超过上限。
    GO2CPP_REQUIRE_EVENTUALLY(CountUnknown(scheduler, gids) * 2 >= gids.size(),
                              10s);
    const std::size_t retained = gids.size() - CountUnknown(scheduler, gids);
    GO2CPP_CHECK(retained <= config.terminal_collect_interval * 16U);
    scheduler.shutdown();
}

/// 持锁退出的终态 G 是 Doctor owner-gone 的证据，回收必须保留它。
void TestLockHoldingTerminalRetained() {
    go2cpp_tests::announce("terminal G holding a sync::Mutex stays registered");
    const bool saved_detection = go2cpp::sync::HeldLockParkDetection();
    go2cpp::sync::SetHeldLockParkDetection(true);

    go2cpp::Scheduler scheduler(SingleWorkerConfig(1));
    scheduler.start();
    go2cpp::sync::Mutex abandoned;
    auto owner = scheduler.spawn([&abandoned] {
        GO2CPP_CHECK(abandoned.Lock());
        // 故意不 Unlock：G 结束后锁永久持有（模拟临界区泄漏）。
    });
    GO2CPP_REQUIRE(static_cast<bool>(owner));
    GO2CPP_REQUIRE(owner->wait_for(5s));
    const auto snapshot = abandoned.DebugSnapshotInternal();
    GO2CPP_CHECK(snapshot.held);
    GO2CPP_CHECK(snapshot.owner_known);
    GO2CPP_CHECK(snapshot.owner_gid ==
                 static_cast<std::uint64_t>(owner->id()));

    // 足够多的完成把回收游标推过全部 16 个条带。
    constexpr int kFlush = 64;
    std::atomic<int> done{0};
    for (int index = 0; index < kFlush; ++index) {
        GO2CPP_REQUIRE(static_cast<bool>(scheduler.spawn(
            [&done] { done.fetch_add(1, std::memory_order_relaxed); })));
    }
    GO2CPP_REQUIRE_EVENTUALLY(done.load(std::memory_order_acquire) == kFlush,
                              5s);
    GO2CPP_CHECK(scheduler.TaskTerminal(owner->id()) ==
                 go2cpp::TaskTerminalState::kCompleted);
    // D2 已修部分不回退：终态 G 不出现在对外快照里。
    for (const auto& task_snapshot : scheduler.DebugSnapshotTasks()) {
        GO2CPP_CHECK(task_snapshot.id != owner->id());
    }

    abandoned.Unlock();
    scheduler.shutdown();
    go2cpp::sync::SetHeldLockParkDetection(saved_detection);
}

}  // namespace

void run_terminal_reclaim_tests() {
    TestEveryCompletionReclaims();
    TestDisabledIntervalKeepsTerminal();
    TestDefaultIntervalBoundsGrowth();
    TestLockHoldingTerminalRetained();
}
