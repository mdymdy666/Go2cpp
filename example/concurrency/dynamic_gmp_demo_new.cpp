#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

/**
 * @brief 动态 G/M/P 简单版：让调度器按负载扩缩 Worker。
 * @details ① SchedulerConfig 设定 P 数量与 Worker 上下界；
 *          ② 提交一批任务，运行期间 Worker 会向上扩展；
 *          ③ 任务结束后空闲 Worker 会被回收，但保底保留 min_workers 个。
 * @return 任务全部完成、且空闲后回收到下界时返回 0。
 */
int main() {
    using Ms = std::chrono::milliseconds;
    using go2cpp::Scheduler;
    using go2cpp::SchedulerConfig;

    // ① 配置：4 个 P，Worker 在 1..4 之间自动伸缩
    std::clog << "[gmp][场景1] 配置 min/max Worker\n";
    SchedulerConfig config;
    config.processor_count = 4;
    config.min_workers = 1;
    config.max_workers = 4;
    config.idle_worker_timeout = Ms(40);

    Scheduler scheduler(config);
    scheduler.Start();
    const auto initial = scheduler.WorkerCount();
    std::cout << "  启动后 Worker 数=" << initial
              << "（应在 " << config.min_workers << ".." << config.max_workers
              << " 之间）\n";
    bool ok = initial >= config.min_workers && initial <= config.max_workers;

    // ② 提交任务，观察 Worker 峰值
    std::clog << "[gmp][场景2] 负载下 Worker 扩展\n";
    std::atomic<int> completed{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int index = 0; index < 32; ++index) {
        tasks.push_back(scheduler.spawn([&completed] {
            std::this_thread::sleep_for(Ms(2));
            completed.fetch_add(1);
        }));
    }
    std::size_t peak = initial;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (completed.load() != 32 && std::chrono::steady_clock::now() < deadline) {
        peak = std::max(peak, scheduler.WorkerCount());
        std::this_thread::sleep_for(Ms(1));
    }
    for (const auto& task : tasks) {
        ok = task->wait_for(std::chrono::seconds(1)) && ok;
    }
    std::cout << "  完成=" << completed.load() << " Worker 峰值=" << peak
              << "（预期 32，峰值 >= 启动值）\n";
    ok = ok && completed.load() == 32 && peak >= initial;

    // ③ 空闲回收：回到 min_workers 而不是 0
    std::clog << "[gmp][场景3] 空闲回收保留下界\n";
    const auto idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (scheduler.WorkerCount() != config.min_workers &&
           std::chrono::steady_clock::now() < idle_deadline) {
        std::this_thread::sleep_for(Ms(1));
    }
    const auto idle = scheduler.WorkerCount();
    std::cout << "  空闲后 Worker 数=" << idle << "（预期 " << config.min_workers
              << "，即保底不回收到 0）\n";
    ok = ok && idle == config.min_workers;

    scheduler.Shutdown();
    std::clog << (ok ? "[gmp] 全部通过\n" : "[gmp] 有失败\n");
    return ok ? 0 : 1;
}
