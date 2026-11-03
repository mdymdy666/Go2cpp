/**
 * @file bench_scheduler_submit.cpp
 * @brief M04：Scheduler::spawn() 提交侧（外部生产者路径）单段耗时基准。
 *
 * @details 与 `bench_scheduler_task_opt.cpp`（M03，wall 端到端 ns/task）的区别：
 *          这里只把 **`spawn()` 调用本身** 放进计时区间，任务是否执行、
 *          执行多久**不计入**。计时区间内只有生产者线程在跑（worker 只在
 *          批次之间消费），因此读数就是「生产者提交一个空任务」的单段成本：
 *          `allocate_shared<Task>` + admission 检查 + 直投 P 队列（或回退
 *          incoming 条带）+ 局部 `shared_ptr` 析构。
 *
 * ## 为什么用「分批提交」
 *
 * `spawn()` 是非阻塞入队，但队列节点持有强引用；若一次提交 1e6 个任务而
 * worker 不消费，内存会线性增长。因此每批提交 `batch` 个任务后，先在
 * 计时区间外等待这批任务跑完，再开始下一批。计时区间严格只在 spawn 循环
 * 内，等待不计入。副作用（会写进报告）：
 *   - 批次边界空闲队列，因此绝大多数提交走「外部生产者直投 P 队列」的快
 *     路径；只有队列满（local_queue_limit=256 及 incoming 积压）才回退到
 *     条带锁。`batch` 越大，回退比例越高，读数越高。
 *   - 每批第一个任务可能触发 worker 唤醒（futex wake），成本按批数摊销。
 *
 * ## 防止编译器消除
 *
 * `spawn()` 在共享库里、有副作用，跨 DSO 调用本身不会被消除；仍用
 * `volatile` 累加返回的 `Task*`，并在结尾打印，确保循环与返回值不被
 * 优化成死代码。
 *
 * ## 构建（复用 .build-bento-wsl，不改 CMake）
 *
 * ```sh
 * g++ -O2 -DNDEBUG -std=c++17 -Iinclude \
 *     tests/concurrency/bench_scheduler_submit.cpp \
 *     -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *     -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_debug \
 *     -lgo2cpp_debug_event -lpthread -o /tmp/bench_scheduler_submit
 * ```
 *
 * ## 用法
 *
 * ```sh
 * taskset -c 2,3 /tmp/bench_scheduler_submit 4 1000000 5 1024
 * #                                    workers tasks  rounds batch
 * ```
 *
 * 输出每轮 `submit ns/task`（= 计时区间总和 / 任务数），最后给中位、最小、最大。
 */

#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t mid = values.size() / 2;
    if (values.size() % 2 == 1) {
        return values[mid];
    }
    return 0.5 * (values[mid - 1] + values[mid]);
}

std::size_t StatusKiB(const char* key) {
    FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return 0;
    }
    char line[256];
    std::size_t kib = 0;
    const std::size_t key_length = std::strlen(key);
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::strncmp(line, key, key_length) == 0) {
            (void)std::sscanf(line + key_length, "%zu", &kib);
            break;
        }
    }
    std::fclose(status);
    return kib;
}

}  // namespace

int main(int argc, char** argv) {
    const int workers = argc > 1 ? std::atoi(argv[1]) : 4;
    const std::size_t tasks =
        argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1000000;
    const int rounds = argc > 3 ? std::atoi(argv[3]) : 5;
    const std::size_t batch =
        argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1024;
    if (workers <= 0 || tasks == 0 || rounds <= 0 || batch == 0) {
        std::fprintf(stderr,
                     "usage: %s [workers] [tasks] [rounds] [batch] "
                     "(defaults 4 1000000 5 1024)\n",
                     argv[0]);
        return 2;
    }

    go2cpp::SchedulerConfig config;
    config.processor_count = static_cast<std::size_t>(workers);
    config.max_workers = static_cast<std::size_t>(workers);
    config.min_workers = static_cast<std::size_t>(workers);
    config.collect_metrics = false;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    std::printf("[cfg] workers=%d tasks=%zu rounds=%d batch=%zu\n", workers,
                tasks, rounds, batch);
    std::printf("[mem] VmRSS=%.1f MiB\n", double(StatusKiB("VmRSS:")) / 1024.0);
    std::fflush(stdout);

    // 预热一批：让 worker 线程、FiberBin 与分配器进入稳态；计时丢弃。
    {
        std::atomic<std::size_t> warm_done{0};
        const std::size_t warm = std::min<std::size_t>(batch, tasks);
        for (std::size_t i = 0; i < warm; ++i) {
            scheduler.spawn([&warm_done] {
                warm_done.fetch_add(1, std::memory_order_release);
            });
        }
        while (warm_done.load(std::memory_order_acquire) != warm) {
            std::this_thread::yield();
        }
    }

    std::vector<double> per_task;
    per_task.reserve(static_cast<std::size_t>(rounds));
    for (int round = 0; round < rounds; ++round) {
        std::atomic<std::size_t> done{0};
        volatile std::uintptr_t sink = 0;
        std::uint64_t submit_ns = 0;
        std::size_t submitted = 0;
        const std::uint64_t wall_start = NowNs();
        while (submitted < tasks) {
            const std::size_t n = std::min(batch, tasks - submitted);
            const std::uint64_t batch_start = NowNs();
            for (std::size_t i = 0; i < n; ++i) {
                auto task = scheduler.spawn([&done] {
                    done.fetch_add(1, std::memory_order_release);
                });
                sink += reinterpret_cast<std::uintptr_t>(task.get());
            }
            submit_ns += NowNs() - batch_start;
            submitted += n;
            // 计时区间之外：等这一批执行完，保证队列不无界积压。
            while (done.load(std::memory_order_acquire) != submitted) {
                std::this_thread::yield();
            }
        }
        const std::uint64_t wall_ns = NowNs() - wall_start;
        const double ns_per_task = double(submit_ns) / double(tasks);
        per_task.push_back(ns_per_task);
        std::printf(
            "[round %d] submit %.1f ns/task  wall %.1f ns/task  "
            "(%.1f k tasks/s submit)  sink=%llx\n",
            round, ns_per_task, double(wall_ns) / double(tasks),
            double(tasks) / (double(submit_ns) / 1e6) / 1000.0,
            static_cast<unsigned long long>(sink));
        std::fflush(stdout);
    }

    std::printf("[summary] rounds=%d median %.1f ns/task  min %.1f  max %.1f\n",
                rounds, Median(per_task),
                *std::min_element(per_task.begin(), per_task.end()),
                *std::max_element(per_task.begin(), per_task.end()));
    std::printf("[mem] VmRSS=%.1f MiB (VmHWM=%.1f MiB)\n",
                double(StatusKiB("VmRSS:")) / 1024.0,
                double(StatusKiB("VmHWM:")) / 1024.0);
    scheduler.shutdown();
    return 0;
}
