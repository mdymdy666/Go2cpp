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
    std::clog << "[gmp][场景1] 配置 P=4、M=1..4，观察负载下 Worker 扩展\n";
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.min_workers = 1;
    config.max_workers = 4;
    config.idle_worker_timeout = 40ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.Start();
    const auto initial = scheduler.WorkerCount();
    if (initial < config.min_workers || initial > config.max_workers) {
        std::cerr << "initial worker count is outside configured bounds\n";
        scheduler.Shutdown();
        return 1;
    }
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
    std::clog << "[gmp][场景1] completed=" << completed.load()
              << " peak_workers=" << peak << " idle_workers=" << idle
              << " affinity_hits=" << hits << '\n';

    // 场景二：空闲回收是有边界的；min_workers=1 的保底 M 不应被回收。
    // 这里把“空闲后仍保留一个 worker”作为错误路径保护，避免把调度器
    // 误配置成零 worker 后任务永久无人执行。
    const bool retained_min_worker = idle == config.min_workers;
    std::clog << "[gmp][场景2] 空闲回收后保留最小 Worker="
              << retained_min_worker << '\n';

    // 场景三：取消一个排队/即将运行的 G。cancel() 与 worker 可能竞争，
    // 所以示例不武断地承诺“任务一定还没开始”，而是记录 body_ran，验证
    // 更重要的契约：取消请求可观察、任务最终收敛到 Cancelled 终态。
    std::clog << "[gmp][场景3] 取消排队或即将运行的任务并检查终态\n";
    go2cpp::SchedulerConfig cancel_config;
    cancel_config.processor_count = 1;
    cancel_config.min_workers = 1;
    cancel_config.max_workers = 1;
    go2cpp::Scheduler cancel_scheduler(cancel_config);
    cancel_scheduler.Start();
    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> release_blocker{false};
    auto blocker = cancel_scheduler.spawn([&] {
        blocker_entered.store(true, std::memory_order_release);
        while (!release_blocker.load(std::memory_order_acquire)) {
            (void)cancel_scheduler.yield_current();
        }
    });
    const auto blocker_deadline = std::chrono::steady_clock::now() + 1s;
    while (!blocker_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < blocker_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    std::atomic<bool> cancel_body_ran{false};
    auto cancelled = cancel_scheduler.spawn([&] {
        cancel_body_ran.store(true, std::memory_order_release);
        std::clog << "[gmp][场景3] 任务已开始，取消请求与执行发生并发竞争\n";
    });
    const bool cancel_requested = cancelled->cancel();
    release_blocker.store(true, std::memory_order_release);
    const bool cancel_finished = cancelled->wait_for(1s) && blocker->wait_for(1s);
    const bool cancel_ok = blocker_entered.load(std::memory_order_acquire) &&
                           cancel_requested && cancel_finished &&
                           cancelled->state() == go2cpp::GState::kCancelled;
    std::clog << "[gmp][场景3] cancel_requested=" << cancel_requested
              << " cancel_finished=" << cancel_finished
              << " body_ran=" << cancel_body_ran.load(std::memory_order_acquire)
              << " cancelled=" << cancel_ok << '\n';
    cancel_scheduler.Shutdown();
    scheduler.Shutdown();
    std::cout << "P=4 M initial=" << initial << " peak=" << peak
              << " idle=" << idle << " completed=" << completed.load()
              << " retained-worker affinity-hits=" << hits << '\n';
    return joined && completed.load() == 48 && peak > initial &&
                   retained_min_worker && cancel_ok ? 0 : 1;
}
