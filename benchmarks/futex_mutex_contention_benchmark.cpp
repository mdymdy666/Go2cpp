/**
 * @file futex_mutex_contention_benchmark.cpp
 * @brief 内部唤醒门 `core::FutexMutex` 的吞吐与慢路径 CPU 消耗基准。
 *
 * ## 背景（X-35）
 *
 * 这里取代原 `HybridGate` 基准。HybridGate 的慢路径是无界自旋；新实现
 * 是「CAS 快路径 + futex 睡眠」。本基准要证明两件事：
 *
 * A. 短临界区争用下，唤醒门没有明显退化（它只用于几个指针的赋值）；
 * B. 长临界区 + 多等待者时，等待者**不再烧 CPU**（旧实现会让一个等待者
 *    在慢路径里空转）。
 *
 * ## 用法
 *
 *   ./go2cpp_futex_mutex_contention_benchmark [threads=1,2,4,8]
 *                                             [rounds=20000] [reps=5]
 */

#include "go2cpp/concurrency/core/futex_mutex.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::core::FutexMutex;

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

/// 进程累计 CPU 时间（用户 + 系统，所有线程）。
double ProcessCpuSeconds() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1.0;
    }
    return static_cast<double>(usage.ru_utime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec) / 1e6 +
           static_cast<double>(usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
}

double RunContendedOnce(int threads, int rounds) {
    FutexMutex gate;
    long long counter = 0;
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
                gate.lock();
                ++counter;
                gate.unlock();
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
    const double total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
            .count());
    const double operations = static_cast<double>(threads) * rounds;
    if (counter != static_cast<long long>(operations)) {
        std::fprintf(stderr, "ERROR: counter=%lld expected=%lld\n", counter,
                     static_cast<long long>(operations));
        std::abort();
    }
    return total_ns / operations;
}

/// 持有者占住门 2ms，K 个等待者同时尝试 lock()。返回等待者消耗的进程 CPU。
void RunParkingOnce(int waiters) {
    FutexMutex gate;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(waiters));
    for (int index = 0; index < waiters; ++index) {
        workers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            gate.lock();
            gate.unlock();
        });
    }
    gate.lock();
    while (ready.load(std::memory_order_acquire) < waiters) {
        std::this_thread::yield();
    }
    const double cpu_before = ProcessCpuSeconds();
    const auto start = Clock::now();
    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    gate.unlock();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto finish = Clock::now();
    const double cpu_after = ProcessCpuSeconds();
    const double elapsed_s =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
                .count()) /
        1e9;
    const double extra = cpu_after > cpu_before ? cpu_after - cpu_before : 0.0;
    std::printf("parking waiters=%d elapsed=%.3fms process_cpu=%.3fms "
                "cpu_per_waiter=%.3fms\n",
                waiters, elapsed_s * 1000.0, extra * 1000.0,
                waiters > 0 ? extra * 1000.0 / waiters : 0.0);
    std::fflush(stdout);
}

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
    int reps = 5;
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

    std::printf("=== core::FutexMutex 争用基准（Release）===\n");
    std::printf("rounds/thread=%d reps=%d loadavg=%.2f\n\n", rounds, reps,
                ReadLoadAverage());
    std::printf("%8s %8s %14s %14s\n", "threads", "reps", "min ns/op",
                "median ns/op");
    for (const int threads : thread_list) {
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(reps));
        for (int rep = 0; rep < reps; ++rep) {
            samples.push_back(RunContendedOnce(threads, rounds));
        }
        std::sort(samples.begin(), samples.end());
        std::printf("%8d %8d %14.2f %14.2f\n", threads, reps, samples.front(),
                    samples[samples.size() / 2U]);
        std::fflush(stdout);
    }

    std::printf("\n--- 长临界区（2ms）下的等待者 CPU 消耗 ---\n");
    for (const int waiters : {1, 2, 4, 8}) {
        RunParkingOnce(waiters);
    }
    std::printf("\nlog: loadavg > 1.5 的数据不可信；比较时须同一负载条件。\n");
    std::printf("final loadavg=%.2f\n", ReadLoadAverage());
    return 0;
}
