/**
 * @file sync_mutex_contention_benchmark.cpp
 * @brief sync::Mutex 在多线程争用下的吞吐基准（Release 构建）。
 *
 * ## 目的
 *
 * 复现并回归 `docs/performance.md` 记录的「4 线程 250 倍退化」：
 * N 个普通线程反复加/解锁同一把 `sync::Mutex`，每次临界区只做一次
 * 自增，因此测出的就是锁本身（含等待/交接）的成本。
 *
 * ## 指标
 *
 * `ns/op` = 墙钟总时间 / (线程数 × 每线程轮数)，即**单次加解锁的均摊
 * 成本**。同时输出总吞吐（Mops/s）便于与外部实现比较。
 *
 * ## 测量纪律
 *
 * - 只用于 Release 构建（`-O3 -DNDEBUG`）；
 * - 每次测量前打印 `/proc/loadavg`，负载 > 1.5 时数据不可信（项目约定）；
 * - 每档重复 `reps` 次，输出最小值与中位数，避免取单次最好值。
 *
 * ## 用法
 *
 *   ./go2cpp_sync_mutex_contention_benchmark [threads=1,2,4,8]
 *                                              [rounds=20000] [reps=3]
 *
 * 线程列表用逗号分隔。
 */

#include "go2cpp/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// 读取 /proc/loadavg 的第一个字段（1 分钟平均负载）。
double ReadLoadAverage() {
    if (std::FILE* file = std::fopen("/proc/loadavg", "r")) {
        double load = -1.0;
        if (std::fscanf(file, "%lf", &load) != 1) {
            load = -1.0;
        }
        std::fclose(file);
        return load;
    }
    return -1.0;
}

/// 一组线程各跑 `rounds` 次 lock/unlock，返回单次均摊纳秒数。
double RunOnce(int threads, int rounds) {
    go2cpp::sync::Mutex mutex;
    long long protected_counter = 0;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int index = 0; index < threads; ++index) {
        workers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int round = 0; round < rounds; ++round) {
                mutex.Lock();
                // 极短临界区：把测量对象限定在锁本身。
                ++protected_counter;
                mutex.Unlock();
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < threads) {
        std::this_thread::yield();
    }
    const auto start = Clock::now();
    go.store(true, std::memory_order_release);

    for (auto& worker : workers) {
        worker.join();
    }
    const auto finish = Clock::now();

    const double total_ns =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
                .count());
    const double operations = static_cast<double>(threads) * rounds;
    if (protected_counter !=
        static_cast<long long>(operations)) {
        // 临界区丢失说明锁语义本身被破坏，按硬错误处理。
        std::fprintf(stderr, "ERROR: counter=%lld expected=%lld\n",
                     protected_counter, static_cast<long long>(operations));
        std::abort();
    }
    return total_ns / operations;
}

/// 解析逗号分隔的线程数列表。
std::vector<int> ParseThreadList(const char* text) {
    std::vector<int> threads;
    std::string input(text);
    std::size_t start = 0;
    while (start <= input.size()) {
        const std::size_t comma = input.find(',', start);
        const std::string token =
            input.substr(start, comma == std::string::npos ? std::string::npos
                                                           : comma - start);
        const int value = std::atoi(token.c_str());
        if (value > 0) {
            threads.push_back(value);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (threads.empty()) {
        threads = {1, 2, 4, 8};
    }
    return threads;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<int> thread_list = {1, 2, 4, 8};
    int rounds = 20000;
    int reps = 3;
    if (argc > 1) {
        thread_list = ParseThreadList(argv[1]);
    }
    if (argc > 2) {
        const int value = std::atoi(argv[2]);
        if (value > 0) {
            rounds = value;
        }
    }
    if (argc > 3) {
        const int value = std::atoi(argv[3]);
        if (value > 0) {
            reps = value;
        }
    }

    std::printf("=== sync::Mutex 多线程争用基准（Release）===\n");
    std::printf("rounds/thread=%d reps=%d loadavg=%.2f\n\n", rounds, reps,
                ReadLoadAverage());
    std::printf("%8s %8s %14s %14s\n", "threads", "reps", "min ns/op",
                "median ns/op");
    for (const int threads : thread_list) {
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(reps));
        for (int rep = 0; rep < reps; ++rep) {
            samples.push_back(RunOnce(threads, rounds));
            std::fflush(stdout);
        }
        std::sort(samples.begin(), samples.end());
        const double best = samples.front();
        const double median = samples[samples.size() / 2U];
        std::printf("%8d %8d %14.2f %14.2f\n", threads, reps, best, median);
        std::fflush(stdout);
    }
    std::printf("\nlog: loadavg > 1.5 的数据不可信；跨机器/跨构建类型不可比较。\n");
    std::printf("final loadavg=%.2f\n", ReadLoadAverage());
    return 0;
}
