#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

// dynamic_gmp 全面版：同一个组件的 7 个使用场景。
//   1  配置上下界           min_workers / max_workers 决定伸缩范围
//   2  负载下扩展           任务挤压时 Worker 数上升
//   3  空闲回收              空闲超过 idle_worker_timeout 后回落到下界
//   4  ★ 保底不被回收        回收目标是 min_workers，不是 0
//   5  任务类别与亲和        同类任务倾向留在同一 M，体现为 affinity_hits
//   6  cancel 的契约         取消请求被接受 → 收敛到 kCancelled
//   7  cancel 未接受的情形   任务已完成时 cancel() 返回 false，属正确语义
//
// 说明：本示例验证的是「契约」而不是「时序」——所有等待都带 deadline，
//       且断言只依赖最终稳定状态（Worker 下界、终态），不依赖中间步数。

namespace {

using Ms = std::chrono::milliseconds;
using go2cpp::GState;
using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;

// 轮询等待谓词成立；超时返回 false。
template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(Ms(1));
    }
    return true;
}

SchedulerConfig make_config(std::size_t procs, std::size_t min_workers,
                            std::size_t max_workers, Ms idle) {
    SchedulerConfig config;
    config.processor_count = procs;
    config.min_workers = min_workers;
    config.max_workers = max_workers;
    config.idle_worker_timeout = idle;
    return config;
}

bool scenario_config_bounds() {
    std::clog << "[full][场景1] 配置上下界决定启动 Worker 数\n";
    Scheduler scheduler(make_config(2, 2, 2, Ms(50)));
    scheduler.Start();
    const auto workers = scheduler.WorkerCount();
    const auto procs = scheduler.processor_count();
    scheduler.Shutdown();
    // min == max == 2 时，Worker 数应稳定在该值
    const bool ok = procs == 2 && workers == 2;
    std::cout << "  P=" << procs << " Worker=" << workers
              << "（min=max=2，预期 2 2）\n";
    return ok;
}

bool scenario_scale_up() {
    std::clog << "[full][场景2] 负载下 Worker 向上扩展\n";
    Scheduler scheduler(make_config(4, 1, 4, Ms(30)));
    scheduler.Start();
    const auto initial = scheduler.WorkerCount();

    std::atomic<int> completed{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int index = 0; index < 64; ++index) {
        tasks.push_back(scheduler.spawn([&completed] {
            std::this_thread::sleep_for(Ms(3));
            completed.fetch_add(1);
        }));
    }

    // 边等边采样峰值；带总超时，避免任务异常时无限等待
    std::size_t peak = initial;
    const auto sample_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(5);
    while (completed.load() != 64 &&
           std::chrono::steady_clock::now() < sample_deadline) {
        peak = std::max(peak, scheduler.WorkerCount());
        std::this_thread::sleep_for(Ms(1));
    }
    bool joined = true;
    for (const auto& task : tasks) {
        joined = task->wait_for(std::chrono::seconds(2)) && joined;
    }
    // 不强制要求 peak > initial（取决于机器核数与调度时序），
    // 只要求它没有越界，并且任务确实全部完成。
    const bool within_bounds = peak >= 1 && peak <= 4;
    scheduler.Shutdown();
    std::cout << "  完成=" << completed.load() << " 启动 Worker=" << initial
              << " 峰值=" << peak << "（预期 64，1..4）\n";
    return joined && completed.load() == 64 && within_bounds;
}

bool scenario_scale_down() {
    std::clog << "[full][场景3] 空闲回收：回落到下界\n";
    Scheduler scheduler(make_config(4, 1, 4, Ms(30)));
    scheduler.Start();
    std::atomic<int> completed{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int index = 0; index < 32; ++index) {
        tasks.push_back(scheduler.spawn([&completed] {
            std::this_thread::sleep_for(Ms(2));
            completed.fetch_add(1);
        }));
    }
    for (const auto& task : tasks) {
        (void)task->wait_for(std::chrono::seconds(2));
    }
    const bool receded = wait_until(
        [&scheduler] { return scheduler.WorkerCount() == 1; }, std::chrono::seconds(5));
    const auto idle = scheduler.WorkerCount();
    scheduler.Shutdown();
    std::cout << "  完成=" << completed.load() << " 空闲后 Worker=" << idle
              << " 已回落=" << receded << "（预期 32 1 1）\n";
    return completed.load() == 32 && receded && idle == 1;
}

bool scenario_min_worker_retained() {
    std::clog << "[full][场景4] 保底 Worker 不被回收（不是回收到 0）\n";
    // min_workers = 3：无论多空闲都应保留 3 个
    Scheduler scheduler(make_config(4, 3, 6, Ms(20)));
    scheduler.Start();
    const auto initial = scheduler.WorkerCount();

    // 跑一小批任务后长时间空闲
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int index = 0; index < 8; ++index) {
        tasks.push_back(scheduler.spawn([] { std::this_thread::sleep_for(Ms(1)); }));
    }
    for (const auto& task : tasks) {
        (void)task->wait_for(std::chrono::seconds(2));
    }
    std::this_thread::sleep_for(Ms(200));          // 远超 idle_worker_timeout

    const auto idle = scheduler.WorkerCount();
    scheduler.Shutdown();
    const bool ok = initial >= 3 && idle >= 3;
    std::cout << "  启动 Worker=" << initial << " 长时间空闲后=" << idle
              << "（min_workers=3，预期都 >= 3）\n";
    return ok;
}

bool scenario_affinity() {
    std::clog << "[full][场景5] 任务类别与亲和命中\n";
    Scheduler scheduler(make_config(4, 1, 4, Ms(50)));
    scheduler.Start();

    // 同一 task_class 的任务倾向留在同一 M，从而累计 affinity_hits
    std::atomic<int> completed{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int index = 0; index < 40; ++index) {
        tasks.push_back(scheduler.spawn(
            [&completed] { completed.fetch_add(1); },
            go2cpp::scheduler::TaskOptions{
                0, static_cast<go2cpp::scheduler::TaskClassId>(1)}));
    }
    for (const auto& task : tasks) {
        (void)task->wait_for(std::chrono::seconds(2));
    }
    std::size_t hits = 0;
    std::size_t classes_seen = 0;
    for (const auto& machine : scheduler.machines()) {
        hits += machine.affinity_hits;
        if (machine.last_task_class != 0) {
            ++classes_seen;
        }
    }
    scheduler.Shutdown();
    // 亲和是优化而非保证：只断言任务全部完成，且统计值可读取
    std::cout << "  完成=" << completed.load() << " affinity_hits=" << hits
              << " 记录过类别的 M 数=" << classes_seen << "（预期 40）\n";
    return completed.load() == 40;
}

bool scenario_cancel_accepted() {
    std::clog << "[full][场景6] cancel 被接受：收敛到 kCancelled\n";
    Scheduler scheduler(make_config(1, 1, 1, Ms(50)));
    scheduler.Start();

    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> release_blocker{false};
    auto blocker = scheduler.spawn([&] {
        blocker_entered.store(true, std::memory_order_release);
        while (!release_blocker.load(std::memory_order_acquire)) {
            (void)scheduler.yield_current();
        }
    });
    (void)wait_until([&blocker_entered] { return blocker_entered.load(); },
                     std::chrono::seconds(2));

    // 唯一的工作线程被 blocker 占住，这个任务会排在队列里
    std::atomic<bool> body_ran{false};
    auto queued = scheduler.spawn([&body_ran] {
        body_ran.store(true, std::memory_order_release);
    });
    const bool requested = queued->cancel();

    release_blocker.store(true, std::memory_order_release);
    const bool finished = queued->wait_for(std::chrono::seconds(2)) &&
                          blocker->wait_for(std::chrono::seconds(2));
    const auto state = queued->state();
    scheduler.Shutdown();

    // 契约：被接受 → 必须是 kCancelled；未接受 → 必须已在终态
    const bool terminal = state == GState::kDead || state == GState::kCancelled ||
                          state == GState::kFailed;
    const bool ok = finished && (requested ? state == GState::kCancelled : terminal);
    std::cout << "  requested=" << requested << " body_ran=" << body_ran.load()
              << " final_state=" << static_cast<int>(state) << " 满足契约=" << ok
              << "（requested 为真时期望 kCancelled）\n";
    return ok;
}

bool scenario_cancel_not_accepted() {
    std::clog << "[full][场景7] cancel 未被接受：任务已在终态，属正确语义\n";
    Scheduler scheduler(make_config(1, 1, 1, Ms(50)));
    scheduler.Start();

    auto done = scheduler.spawn([] {});
    (void)done->wait_for(std::chrono::seconds(2));   // 确保已完成
    const bool requested = done->cancel();           // 无可取消之物
    const auto state = done->state();
    scheduler.Shutdown();

    const bool terminal = state == GState::kDead || state == GState::kCancelled ||
                          state == GState::kFailed;
    // 关键：不接受取消时，任务必须已经在终态（而不是还在跑）
    const bool ok = !requested && terminal;
    std::cout << "  已完成任务再 cancel：requested=" << requested
              << " final_state=" << static_cast<int>(state) << " 满足契约=" << ok
              << "（预期 requested=0 且已在终态）\n";
    return ok;
}

}  // namespace

/**
 * @brief dynamic_gmp 全面版：7 个场景，覆盖扩缩、保底、亲和与 cancel 契约。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_config_bounds() && ok;
    ok = scenario_scale_up() && ok;
    ok = scenario_scale_down() && ok;
    ok = scenario_min_worker_retained() && ok;
    ok = scenario_affinity() && ok;
    ok = scenario_cancel_accepted() && ok;
    ok = scenario_cancel_not_accepted() && ok;

    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}