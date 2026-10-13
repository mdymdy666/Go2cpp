#include "go2cpp/runtime.hpp"

#include <iostream>

/**
 * @brief Scheduler 简单版：启动、提交任务、等待、查询规模、关闭。
 * @details ① Scheduler(n) + start() —— 创建 n 个 P（Processor）；
 *          ② spawn() —— 提交一个 Fiber 任务，返回 Task 句柄；
 *          ③ Task::wait() —— 等待它结束；
 *          ④ worker_count() / runnable_count() —— 查询运行时规模；
 *          ⑤ shutdown() —— 排空并回收工作线程。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    bool ok = true;

    // ① 创建 3 个 P 并启动
    std::clog << "[scheduler][场景1] 启动调度器\n";
    Scheduler scheduler(3);
    std::cout << "  is_running（启动前）=" << scheduler.is_running() << '\n';
    scheduler.start();
    const bool running = scheduler.is_running();
    std::cout << "  is_running（启动后）=" << running
              << " processor_count=" << scheduler.processor_count() << "（预期 1 3）\n";
    ok = ok && running && scheduler.processor_count() == 3;

    // ② 提交若干任务并等待它们全部结束
    std::clog << "[scheduler][场景2] 提交任务\n";
    const std::size_t before = scheduler.runnable_count();
    std::vector<std::shared_ptr<Task>> tasks;
    for (int i = 0; i < 5; ++i) {
        tasks.push_back(scheduler.spawn([i] { (void)i; }));
    }
    for (auto& task : tasks) {
        ok = ok && task->wait();
    }
    const std::size_t after = scheduler.runnable_count();
    std::cout << "  提交前可运行=" << before << " 全部结束后=" << after
              << " 任务数=" << tasks.size() << "（预期 0 0 5）\n";
    ok = ok && after == 0 && tasks.size() == 5;

    // ③ 运行时规模
    std::clog << "[scheduler][场景3] 运行时规模\n";
    const std::size_t workers = scheduler.worker_count();
    std::cout << "  worker_count=" << workers
              << "（应 >= processor_count=3）\n";
    ok = ok && workers >= scheduler.processor_count();

    scheduler.shutdown();
    std::cout << "  is_running（关闭后）=" << scheduler.is_running() << "（预期 0）\n";
    ok = ok && !scheduler.is_running();

    std::clog << (ok ? "[scheduler] 全部通过\n" : "[scheduler] 有失败\n");
    return ok ? 0 : 1;
}
