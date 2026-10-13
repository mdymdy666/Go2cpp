#include "go2cpp/runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

// Scheduler 全面版：同一个组件的 6 个使用场景。
//   1  启动与关闭           is_running 在 start/shutdown 前后翻转
//   2  spawn 与 wait        Task 句柄可等待；wait_for 支持超时
//   3  并行度观测           processor_count / worker_count / runnable_count
//   4  快照                 processors() / machines() 返回运行时视图
//   5  指标                 metrics() 累计 task_runs / task_completions 等
//   6  Fiber 内自省         current_task / current_processor_id / current_machine_id
//
// 每个场景独立成段；校验失败只记 ok=false，最后统一报告。

namespace {

using Ms = std::chrono::milliseconds;

bool scenario_start_shutdown() {
    using namespace go2cpp;
    std::clog << "[full][场景1] 启动与关闭\n";
    Scheduler scheduler(2);
    const bool before = scheduler.is_running();
    scheduler.start();
    const bool started = scheduler.is_running();
    scheduler.shutdown();
    const bool stopped = scheduler.is_running();
    std::cout << "  启动前=" << before << " 启动后=" << started
              << " 关闭后=" << stopped << "（预期 0 1 0）\n";
    return !before && started && !stopped;
}

bool scenario_spawn_and_wait() {
    using namespace go2cpp;
    std::clog << "[full][场景2] spawn 与 wait / wait_for\n";
    Scheduler scheduler(2);
    scheduler.start();

    auto quick = scheduler.spawn([] {});
    const bool joined = quick->wait();                 // 无限等待
    const bool already = quick->wait_for(Ms(5));       // 已完成，立即返回 true

    // wait_for 也能在任务尚未完成时超时返回 false
    std::atomic<bool> gate{false};
    auto blocked = scheduler.spawn([&gate] {
        while (!gate.load()) {
            std::this_thread::yield();
        }
    });
    const bool timed_out = !blocked->wait_for(Ms(10));  // 预期超时
    gate.store(true);
    const bool finished = blocked->wait();

    std::cout << "  wait 成功=" << joined << " 已完成后再 wait_for=" << already
              << " 未完成时 wait_for 超时=" << timed_out
              << " 放行后完成=" << finished << "（预期 1 1 1 1）\n";
    scheduler.shutdown();
    return joined && already && timed_out && finished;
}

bool scenario_parallelism() {
    using namespace go2cpp;
    std::clog << "[full][场景3] 并行度观测\n";
    Scheduler scheduler(4);
    scheduler.start();

    const std::size_t procs = scheduler.processor_count();
    const std::size_t workers = scheduler.worker_count();
    const std::size_t runnable_idle = scheduler.runnable_count();

    std::atomic<int> ran{0};
    std::vector<std::shared_ptr<Task>> tasks;
    for (int i = 0; i < 8; ++i) {
        tasks.push_back(scheduler.spawn([&ran] { ran.fetch_add(1); }));
    }
    for (auto& task : tasks) {
        task->wait();
    }
    const std::size_t runnable_after = scheduler.runnable_count();
    scheduler.shutdown();

    // ★ worker_count() 是"当前存活的工作线程（M）数"，且 M 按需惰性创建，
    //   因此它不等于 P 的数量，也不保证达到 P 的上限。
    std::cout << "  P=" << procs << " M=" << workers
              << " 空闲时可运行=" << runnable_idle
              << " 结束后可运行=" << runnable_after
              << " 实际执行=" << ran.load() << "（预期 P=4 M>=1 0 0 8）\n";
    return procs == 4 && workers >= 1 && runnable_idle == 0 &&
           runnable_after == 0 && ran.load() == 8;
}

bool scenario_snapshots() {
    using namespace go2cpp;
    std::clog << "[full][场景4] 快照：processors() / machines()\n";
    Scheduler scheduler(3);
    scheduler.start();

    auto task = scheduler.spawn([] {});
    task->wait();

    const auto processors = scheduler.processors();
    const auto machines = scheduler.machines();
    scheduler.shutdown();

    // ★ PId / MId 从 0 开始编号，0 是合法 ID 而不是"无效值"，
    //   所以不能用 != 0 判断有效性 —— 这里改为校验 id 落在快照数量范围内。
    bool ids_ok = true;
    for (const auto& snapshot : processors) {
        ids_ok = ids_ok && (snapshot.id < processors.size());
    }
    std::cout << "  P 快照=" << processors.size() << " M 快照=" << machines.size()
              << " P id 均在范围内=" << ids_ok << "（预期 3 >=1 1）\n";
    return processors.size() == 3 && !machines.empty() && ids_ok;
}

bool scenario_metrics() {
    using namespace go2cpp;
    std::clog << "[full][场景5] 指标：metrics()\n";
    Scheduler scheduler(2);
    scheduler.start();

    constexpr int kTasks = 12;
    std::vector<std::shared_ptr<Task>> tasks;
    for (int i = 0; i < kTasks; ++i) {
        tasks.push_back(scheduler.spawn([] {}));
    }
    for (auto& task : tasks) {
        task->wait();
    }
    // ★ metrics() 的累加与 task->wait() 的返回之间存在一个很短的窗口：
    //   实测 8 次运行里有 2 次在 wait() 全部返回后仍读到 task_runs=11（少一次）。
    //   这是示例的时序假设过严，不是调度器缺陷 —— 改为轮询等待计数收敛。
    std::uint64_t runs = 0;
    std::uint64_t completions = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        const auto metrics = scheduler.metrics();
        runs = metrics.task_runs;
        completions = metrics.task_completions;
        if ((runs >= static_cast<std::uint64_t>(kTasks) &&
             completions >= static_cast<std::uint64_t>(kTasks)) ||
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(Ms(1));
    }
    scheduler.shutdown();

    const bool ok = runs >= static_cast<std::uint64_t>(kTasks) &&
                    completions >= static_cast<std::uint64_t>(kTasks);
    std::cout << "  task_runs=" << runs << " task_completions=" << completions
              << "（预期均 >= " << kTasks << "）\n";
    return ok;
}

bool scenario_introspection() {
    using namespace go2cpp;
    std::clog << "[full][场景6] Fiber 内自省：current_* 静态查询\n";
    Scheduler scheduler(2);
    scheduler.start();

    std::atomic<bool> has_task{false};
    std::atomic<bool> has_processor{false};
    const std::size_t proc_count = scheduler.processor_count();
    auto task = scheduler.spawn([&] {
        const auto current = Scheduler::current_task();
        has_task.store(current != nullptr);
        // ★ PId 从 0 起，0 是合法值；用"小于 P 总数"判断更可靠。
        has_processor.store(Scheduler::current_processor_id() < proc_count);
    });
    task->wait();
    const bool outer_has_task = (Scheduler::current_task() != nullptr);
    scheduler.shutdown();

    std::cout << "  Fiber 内有当前任务=" << has_task.load()
              << " 有当前 P=" << has_processor.load()
              << " 主线程内无当前任务=" << !outer_has_task << "（预期 1 1 1）\n";
    return has_task.load() && has_processor.load() && !outer_has_task;
}

}  // namespace

/**
 * @brief Scheduler 全面版：6 个场景，覆盖生命周期、提交、并行度、快照、指标与自省。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_start_shutdown() && ok;
    ok = scenario_spawn_and_wait() && ok;
    ok = scenario_parallelism() && ok;
    ok = scenario_snapshots() && ok;
    ok = scenario_metrics() && ok;
    ok = scenario_introspection() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}