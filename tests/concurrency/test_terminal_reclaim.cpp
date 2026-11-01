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
    for (int index = 0; index < kChecked; ++index) {
        auto task = scheduler.spawn(
            [&done] { done.fetch_add(1, std::memory_order_relaxed); });
        GO2CPP_REQUIRE(static_cast<bool>(task));
        handles.push_back(task);
        gids.push_back(task->id());
    }
    // 先等被检查的 G 全部完成，再单独提交 flush 批：flush 的完成事件才是
    // 驱动「后续条带轮转」的扫描源。两批混在一起提交时，288 个任务超过
    // 本地环 256 槽，队列溢出会让完成顺序偏离提交顺序；一旦 flush 批先于
    // 被检查的 G 完成，最后完成的那些 G 就等不到覆盖自己条带的扫描，测试
    // 会假红（与实现无关：原版在宿主高负载下 20 次实测 6 次超时）。
    GO2CPP_REQUIRE_EVENTUALLY(done.load(std::memory_order_acquire) == kChecked,
                              5s);
    for (int index = 0; index < kFlush; ++index) {
        auto task = scheduler.spawn(
            [&done] { done.fetch_add(1, std::memory_order_relaxed); });
        GO2CPP_REQUIRE(static_cast<bool>(task));
        handles.push_back(task);
    }
    GO2CPP_REQUIRE_EVENTUALLY(
        done.load(std::memory_order_acquire) == kChecked + kFlush, 5s);
    GO2CPP_REQUIRE_EVENTUALLY(CountUnknown(scheduler, gids) == gids.size(),
                              5s);
    GO2CPP_CHECK(scheduler.DebugSnapshotTasks().empty());
    scheduler.shutdown();
}

/// 关闭周期回收时，**注册过的**终态 G 仍留在注册表；作为上一条用例的对照。
///
/// P7 语义：注册表只登记「可能被外部唤醒 / 被诊断按 gid 查询」的 G ——
/// 进入过 Waiting 的等待者，以及持锁退出的终态 G。run-to-completion 的 G
/// 完全不进表（提交热路径因此不再付条带锁 + CAS + 自持引用），对它们
/// `TaskTerminal()` 返回 kUnknown 是设计语义（kUnknown 不等于「已终止」）。
/// 所以这里用一次真实的 park/wake 往返让 G 拿到注册表条目，再验证节拍
/// 关闭时该条目不会被回收。
void TestDisabledIntervalKeepsTerminal() {
    go2cpp_tests::announce("terminal G retained when reclaim tick is disabled");
    go2cpp::Scheduler scheduler(SingleWorkerConfig(0));
    scheduler.start();

    std::atomic<int> done{0};
    auto task = scheduler.spawn([&done] {
        auto* const current = go2cpp::Scheduler::current_scheduler();
        GO2CPP_REQUIRE(current != nullptr);
        // 挂起直到被外部唤醒：这一步把 G 注册进条带链表。
        (void)current->park_current();
        done.fetch_add(1, std::memory_order_relaxed);
    });
    GO2CPP_REQUIRE(static_cast<bool>(task));
    GO2CPP_REQUIRE_EVENTUALLY(task->state() == go2cpp::GState::kWaiting, 5s);
    GO2CPP_REQUIRE(scheduler.wake(task));
    GO2CPP_REQUIRE_EVENTUALLY(done.load(std::memory_order_acquire) == 1, 5s);
    // 没有周期回收：注册过的终态条目应当仍在表内，且能查到具体终态。
    GO2CPP_CHECK(scheduler.TaskTerminal(task->id()) ==
                 go2cpp::TaskTerminalState::kCompleted);
    scheduler.shutdown();
}

/// P7 新增：run-to-completion 的 G 从不进入注册表。
///
/// 证明两件事：
/// 1. 终态查询返回 kUnknown（设计语义），而不是把这类 G 留在表里；
/// 2. 大量短任务不会让注册表随历史任务数增长（D2 的更强形式）。
void TestRunToCompletionNeverRegistered() {
    go2cpp_tests::announce("run-to-completion G never enters registry");
    go2cpp::Scheduler scheduler(SingleWorkerConfig(1));
    scheduler.start();

    constexpr int kTasks = 512;
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
    // 全部完成且都没有注册：查询一律 kUnknown，对外快照为空。
    GO2CPP_CHECK(CountUnknown(scheduler, gids) == gids.size());
    GO2CPP_CHECK(scheduler.DebugSnapshotTasks().empty());
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
    // Release 构建把检测编译期关闭 → 不记录 owner，跳过 owner 断言。
    const bool detection_active = go2cpp::sync::HeldLockParkDetection();

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
    if (detection_active) {
        GO2CPP_CHECK(snapshot.owner_known);
        GO2CPP_CHECK(snapshot.owner_gid ==
                     static_cast<std::uint64_t>(owner->id()));
    }

    // 足够多的完成把回收游标推过全部 16 个条带。
    constexpr int kFlush = 64;
    std::atomic<int> done{0};
    for (int index = 0; index < kFlush; ++index) {
        GO2CPP_REQUIRE(static_cast<bool>(scheduler.spawn(
            [&done] { done.fetch_add(1, std::memory_order_relaxed); })));
    }
    GO2CPP_REQUIRE_EVENTUALLY(done.load(std::memory_order_acquire) == kFlush,
                              5s);
    if (detection_active) {
        // 只有开启了热路径持锁检测，「持锁退出」才被记录进
        // Task::held_mutex_count()，P7 也才会为这类 G 补注册（Doctor 的
        // owner-gone 证据链依赖它）。Release 构建编译期关闭检测，这类 G
        // 与普通 run-to-completion G 无法区分，按 P7 语义不注册。
        GO2CPP_CHECK(scheduler.TaskTerminal(owner->id()) ==
                     go2cpp::TaskTerminalState::kCompleted);
    }
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
    TestRunToCompletionNeverRegistered();
    TestDefaultIntervalBoundsGrowth();
    TestLockHoldingTerminalRetained();
}
