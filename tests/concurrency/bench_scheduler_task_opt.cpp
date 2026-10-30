/**
 * @file bench_scheduler_task_opt.cpp
 * @brief Scheduler 任务 churn 基准：验证 FiberBin 回收的 Fiber 保留栈后，
 *        每个任务不再重复申请/提交栈。
 *
 * 场景：4 个 worker，分 N 轮提交 M 个空任务，统计 CPU ns/task、墙钟 ns/task
 * 与 VmSize。计时用进程 CPU 时间（CLOCK_PROCESS_CPUTIME_ID），宿主负载高时
 * 仍可跨版本公平对比。
 *
 * 构建（不改 CMake）：
 *   g++ -O2 -DNDEBUG -std=c++17 -Iinclude \
 *       tests/concurrency/bench_scheduler_task_opt.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_debug \
 *       -lgo2cpp_debug_event -lpthread -o /tmp/bench_scheduler_task_opt
 *
 * 用法：/tmp/bench_scheduler_task_opt 4 100000 3
 */
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

std::uint64_t ProcessCpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
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

void Report(const char* tag) {
    std::printf("[%s] VmSize=%.2f GiB Rss=%.1f MiB\n", tag,
                double(StatusKiB("VmSize:")) / 1024.0 / 1024.0,
                double(StatusKiB("VmRSS:")) / 1024.0);
}

}  // namespace

int main(int argc, char** argv) {
    const int workers = argc > 1 ? std::atoi(argv[1]) : 4;
    const std::size_t tasks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 100000;
    const int rounds = argc > 3 ? std::atoi(argv[3]) : 3;

    go2cpp::SchedulerConfig config;
    config.processor_count = static_cast<std::size_t>(workers);
    config.max_workers = static_cast<std::size_t>(workers);
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    std::printf("[cfg] workers=%d tasks=%zu rounds=%d\n", workers, tasks, rounds);
    Report("start");

    for (int round = 0; round < rounds; ++round) {
        std::atomic<std::size_t> done{0};
        const std::uint64_t cpu = ProcessCpuNs();
        const std::uint64_t wall = NowNs();
        for (std::size_t i = 0; i < tasks; ++i) {
            scheduler.spawn([&done] {
                done.fetch_add(1, std::memory_order_release);
            });
        }
        while (done.load(std::memory_order_acquire) != tasks) {
            std::this_thread::yield();
        }
        const std::uint64_t cpu_elapsed = ProcessCpuNs() - cpu;
        const std::uint64_t wall_elapsed = NowNs() - wall;
        std::printf("[round %d] cpu %.0f ns/task  wall %.0f ns/task  (%.0f k tasks/s)\n",
                    round, double(cpu_elapsed) / double(tasks),
                    double(wall_elapsed) / double(tasks),
                    double(tasks) / (double(wall_elapsed) / 1e6) / 1000.0);
        Report("round");
    }
    scheduler.shutdown();
    Report("shutdown");
    return 0;
}
