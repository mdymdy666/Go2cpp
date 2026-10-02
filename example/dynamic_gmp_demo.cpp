#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

/**
 * @brief 演示 G/M/P 调度器在负载变化时扩展和回收 Worker。
 * @details 任务被分配到两个任务类别，示例记录初始、峰值和空闲 Worker 数量，
 *          同时观察同类任务在保留 Worker 上的亲和命中次数。
 * @return 任务全部完成且 Worker 回收到最小值时返回 0。
 */
int main() {
    using namespace std::chrono_literals;
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.min_workers = 1;
    config.max_workers = 4;
    config.idle_worker_timeout = 40ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.Start();
    const auto initial = scheduler.WorkerCount();
    std::atomic<int> completed{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int index = 0; index < 48; ++index) {
        tasks.push_back(scheduler.spawn([&] {
            std::this_thread::sleep_for(2ms);
            completed.fetch_add(1);
        }, go2cpp::scheduler::TaskOptions{
               0, static_cast<go2cpp::scheduler::TaskClassId>(index % 2 + 1)}));
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::size_t peak = initial;
    while (completed.load() != 48 && std::chrono::steady_clock::now() < deadline) {
        peak = std::max(peak, scheduler.WorkerCount());
        std::this_thread::sleep_for(1ms);
    }
    bool joined = true;
    for (const auto& task : tasks) {
        joined = task->wait_for(1s) && joined;
    }
    const auto idle_deadline = std::chrono::steady_clock::now() + 2s;
    while (scheduler.WorkerCount() != 1 &&
           std::chrono::steady_clock::now() < idle_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const auto idle = scheduler.WorkerCount();
    std::size_t hits = 0;
    for (const auto& machine : scheduler.machines()) {
        hits += machine.affinity_hits;
    }
    scheduler.Shutdown();
    std::cout << "P=4 M initial=" << initial << " peak=" << peak
              << " idle=" << idle << " completed=" << completed.load()
              << " retained-worker affinity-hits=" << hits << '\n';
    return joined && completed.load() == 48 && peak > initial && idle == 1 ? 0 : 1;
}
