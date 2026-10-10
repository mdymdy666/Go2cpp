/**
 * @file timer_task_benchmark.cpp
 * @brief 对比普通线程、TimerService 和 Scheduler/Fiber 的定时任务开销。
 *
 * 该基准只测量短延时任务的调度链路，不代表网络 IO 吞吐。所有 Go2Cpp
 * 任务句柄都会保留到回调完成，避免句柄析构主动取消任务。
 */
#include "go2cpp/core/timer.hpp"
#include "go2cpp/scheduler/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/**
 * @brief 从环境变量读取任务数并限制资源上限。
 * @return 有效任务数；未设置或非法时返回 200。
 */
std::size_t task_count() {
    if (const char* value = std::getenv("GO2CPP_TIMER_BENCH_TASKS")) {
        const auto parsed = std::strtoull(value, nullptr, 10);
        if (parsed > 0 && parsed <= 100000) {
            return static_cast<std::size_t>(parsed);
        }
    }
    return 200;
}

/** @brief 一组基准结果，延迟单位为微秒，总耗时单位为毫秒。 */
struct BenchmarkResult {
    double elapsed_ms{0.0};
    double p50_us{0.0};
    double p99_us{0.0};
};

/**
 * @brief 计算一批延迟的中位数和 P99。
 * @param delays 已经记录的延迟数组；函数会移动并排序它。
 * @param begin 基准开始时间。
 * @param end 基准结束时间。
 * @return 总耗时和两个分位点。
 */
BenchmarkResult summarize(std::vector<long long> delays,
                          Clock::time_point begin, Clock::time_point end) {
    std::sort(delays.begin(), delays.end());
    const auto percentile = [&delays](double quantile) {
        if (delays.empty()) {
            return 0.0;
        }
        const auto index = static_cast<std::size_t>(
            quantile * static_cast<double>(delays.size() - 1));
        return static_cast<double>(delays[index]);
    };
    return {
        std::chrono::duration<double, std::milli>(end - begin).count(),
        percentile(0.50),
        percentile(0.99),
    };
}

/**
 * @brief 打印一个模型的结果。
 * @param name 模型名称。
 * @param tasks 任务总数。
 * @param result 统计结果。
 */
void print_result(const char* name, std::size_t tasks,
                  const BenchmarkResult& result) {
    std::cout << std::left << std::setw(25) << name
              << " tasks=" << std::setw(6) << tasks
              << " elapsed_ms=" << std::fixed << std::setprecision(2)
              << std::setw(10) << result.elapsed_ms
              << " delay_us(p50/p99)=" << result.p50_us << '/'
              << result.p99_us << '\n';
}

/**
 * @brief 运行基于 TimerService 或 Scheduler 的统一任务基准。
 * @param tasks 任务数量。
 * @param submit 注册一个带截止时间和回调的定时任务，并返回句柄。
 * @param stop 可选的执行环境停止函数。
 * @return 回调延迟统计。
 */
template <typename Submit>
BenchmarkResult run_go2cpp_timer_benchmark(
    std::size_t tasks, Submit&& submit,
    std::function<void()> stop = std::function<void()>{}) {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<std::size_t> completed{0};
    std::vector<long long> delays;
    std::vector<go2cpp::core::Timer> handles;
    delays.reserve(tasks);
    handles.reserve(tasks);

    const auto deadline = Clock::now() + 5ms;
    const auto begin = Clock::now();
    for (std::size_t index = 0; index < tasks; ++index) {
        handles.emplace_back(submit(deadline, [&, deadline] {
            const auto now = Clock::now();
            {
                std::lock_guard<std::mutex> lock(mutex);
                delays.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                    now - deadline).count());
            }
            if (completed.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                tasks) {
                condition.notify_one();
            }
        }));
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait_for(lock, 30s,
                           [&] { return completed.load() == tasks; });
    }
    const auto end = Clock::now();
    if (stop) {
        stop();
    }
    return summarize(std::move(delays), begin, end);
}

/**
 * @brief 测量 TimerService 单线程计时器后端。
 * @param tasks 任务数量。
 * @return 回调延迟统计。
 */
BenchmarkResult run_timer_service_benchmark(std::size_t tasks) {
    go2cpp::core::TimerService service;
    return run_go2cpp_timer_benchmark(
        tasks,
        [&service](Clock::time_point deadline, std::function<void()> callback) {
            return service.Schedule(deadline, std::move(callback));
        });
}

/**
 * @brief 测量 Scheduler 将到期回调转成 Fiber 的路径。
 * @param tasks 任务数量。
 * @return 回调延迟统计。
 */
BenchmarkResult run_scheduler_benchmark(std::size_t tasks) {
    go2cpp::scheduler::Scheduler scheduler(1);
    scheduler.start();
    return run_go2cpp_timer_benchmark(
        tasks,
        [&scheduler](Clock::time_point deadline,
                     std::function<void()> callback) {
            return scheduler.schedule_at(deadline, std::move(callback));
        },
        [&scheduler] { scheduler.shutdown(); });
}

/**
 * @brief 测量每个任务创建一个原生线程的基准。
 * @param tasks 任务数量。
 * @return 回调延迟统计。
 */
BenchmarkResult run_thread_benchmark(std::size_t tasks) {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<std::size_t> completed{0};
    std::vector<long long> delays;
    std::vector<std::thread> workers;
    delays.reserve(tasks);
    workers.reserve(tasks);

    const auto deadline = Clock::now() + 5ms;
    const auto begin = Clock::now();
    for (std::size_t index = 0; index < tasks; ++index) {
        workers.emplace_back([&, deadline] {
            std::this_thread::sleep_until(deadline);
            const auto now = Clock::now();
            {
                std::lock_guard<std::mutex> lock(mutex);
                delays.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                    now - deadline).count());
            }
            if (completed.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                tasks) {
                condition.notify_one();
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    return summarize(std::move(delays), begin, Clock::now());
}

}  // 命名空间

/**
 * @brief 运行三种模型的短延时任务基准。
 * @return 基准成功返回 0。
 */
int main() {
    const auto tasks = task_count();
    std::cout << "Go2Cpp timer benchmark tasks=" << tasks << '\n';
    print_result("native-thread-per-task", tasks,
                 run_thread_benchmark(tasks));
    print_result("TimerService", tasks,
                 run_timer_service_benchmark(tasks));
    print_result("Scheduler+Fiber", tasks,
                 run_scheduler_benchmark(tasks));
    std::cout << "coost 请按 docs/timer_benchmark.md 的相同参数独立运行。\n";
    return 0;
}
