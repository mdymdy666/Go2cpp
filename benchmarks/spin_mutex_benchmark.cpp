/**
 * @file spin_mutex_benchmark.cpp
 * @brief `sync::SpinMutex` vs `sync::Mutex` vs `std::mutex` 的开销对比（Release）。
 *
 * @date 2026-10-18
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 目的
 *
 * 验证 `SpinMutex` 的契约检查没有把锁本身变慢：它内部复用 `sync::Mutex`，
 * 只多两次 `no_park_depth` 计数与一次 owner `shared_ptr` 维护。预期结果
 * 与 `Mutex` **几乎相同**；若明显变慢，说明热路径多了不必要的原子操作。
 *
 * ## 两组口径
 *
 * 1. **native threads**：普通 OS 线程。`SpinMutex` 在这里没有受管 G，
 *    `no_park_depth` 无处记录，测的是封装的固定开销；
 * 2. **fiber（GMP）**：`workers` 个 M × `fibers` 个 G。这里才是
 *    `SpinMutex` 的真实路径：每次加锁都会读写当前 G 的计数。
 *
 * ## 指标
 *
 * `ns/op` = 墙钟总时间 / 总操作数（每次 lock+unlock 记 1 op）。
 * 每档重复 `reps` 次，输出最小值与中位数，避免单次抖动。
 *
 * ## 测量纪律
 *
 * - 只在 Release（`-O3 -DNDEBUG`）下有效；
 * - 运行前打印 `/proc/loadavg` 与 `/proc/stat` 的 `procs_running`；
 *   WSL2 的 loadavg 会失真，`procs_running` 更可信；
 * - 外部压测时用 `timeout 120` 包裹，防止异常负载把进程拖死。
 *
 * ## 用法
 *
 *   ./go2cpp_spin_mutex_benchmark [threads=1,2,4,8] [rounds=20000]
 *                                 [fiber_rounds=2000] [reps=3]
 */

#include "go2cpp/concurrency/spin_mutex.hpp"

#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

enum class Mode {
    kSpin,
    kSyncMutex,
    kStdMutex,
};

const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::kSpin:
            return "SpinMutex";
        case Mode::kSyncMutex:
            return "Mutex";
        case Mode::kStdMutex:
            return "std::mutex";
    }
    return "?";
}

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

int ReadProcessesRunning() {
    if (std::FILE* file = std::fopen("/proc/stat", "r")) {
        char line[256];
        int running = -1;
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            if (std::strncmp(line, "procs_running", 13) == 0) {
                (void)std::sscanf(line + 13, "%d", &running);
                break;
            }
        }
        std::fclose(file);
        return running;
    }
    return -1;
}

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 0) {
        return (values[middle - 1] + values[middle]) / 2.0;
    }
    return values[middle];
}

template <typename Lock>
void LockIt(Lock& lock) {
    if constexpr (std::is_same_v<Lock, std::mutex>) {
        lock.lock();
    } else {
        lock.Lock();
    }
}

template <typename Lock>
void UnlockIt(Lock& lock) {
    if constexpr (std::is_same_v<Lock, std::mutex>) {
        lock.unlock();
    } else {
        lock.Unlock();
    }
}

/// 普通线程压测：`threads` 个 OS 线程争用同一把锁。
template <typename Lock>
double RunNativeOnce(int threads, int rounds) {
    Lock mutex;
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
                LockIt(mutex);
                ++counter;
                UnlockIt(mutex);
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
    const long long expected = static_cast<long long>(threads) * rounds;
    if (counter != expected) {
        std::fprintf(stderr, "ERROR: counter=%lld expected=%lld\n", counter,
                     expected);
        std::abort();
    }
    const double total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
            .count());
    return total_ns / static_cast<double>(expected);
}

template <typename Lock>
double RunNative(int threads, int rounds, int reps) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(reps));
    for (int rep = 0; rep < reps; ++rep) {
        samples.push_back(RunNativeOnce<Lock>(threads, rounds));
    }
    return Median(std::move(samples));
}

/// GMP 压测：`workers` 个 M 上跑 `fibers` 个 G，全部争用同一把锁。
double RunFiberOnce(int workers, int fibers, int rounds, Mode mode) {
    go2cpp::SchedulerConfig config;
    config.processor_count = static_cast<std::size_t>(workers);
    config.min_workers = static_cast<std::size_t>(workers);
    config.max_workers = static_cast<std::size_t>(workers);
    config.idle_wait = std::chrono::milliseconds(1);

    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    go2cpp::sync::SpinMutex spin;
    go2cpp::sync::Mutex sync_mutex;
    std::atomic<long long> counter{0};
    std::atomic<int> done{0};
    std::atomic<bool> go{false};

    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(static_cast<std::size_t>(fibers));
    for (int index = 0; index < fibers; ++index) {
        tasks.push_back(scheduler.spawn([&] {
            while (!go.load(std::memory_order_acquire)) {
                scheduler.yield_current();
            }
            for (int round = 0; round < rounds; ++round) {
                switch (mode) {
                    case Mode::kSpin:
                        spin.Lock();
                        counter.fetch_add(1, std::memory_order_relaxed);
                        spin.Unlock();
                        break;
                    case Mode::kSyncMutex:
                        sync_mutex.Lock();
                        counter.fetch_add(1, std::memory_order_relaxed);
                        sync_mutex.Unlock();
                        break;
                    case Mode::kStdMutex:
                        sync_mutex.Lock();
                        counter.fetch_add(1, std::memory_order_relaxed);
                        sync_mutex.Unlock();
                        break;
                }
            }
            done.fetch_add(1, std::memory_order_release);
        }));
    }

    const auto start = Clock::now();
    go.store(true, std::memory_order_release);
    while (done.load(std::memory_order_acquire) < fibers) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto finish = Clock::now();
    scheduler.shutdown();

    const long long expected = static_cast<long long>(fibers) * rounds;
    if (counter.load(std::memory_order_relaxed) != expected) {
        std::fprintf(stderr, "ERROR: fiber counter=%lld expected=%lld\n",
                     counter.load(std::memory_order_relaxed), expected);
        std::abort();
    }
    const double total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
            .count());
    return total_ns / static_cast<double>(expected);
}

double RunFiber(int workers, int fibers, int rounds, Mode mode, int reps) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(reps));
    for (int rep = 0; rep < reps; ++rep) {
        samples.push_back(RunFiberOnce(workers, fibers, rounds, mode));
    }
    return Median(std::move(samples));
}

std::vector<int> ParseList(const char* text, std::vector<int> fallback) {
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    std::vector<int> values;
    const std::string input(text);
    std::size_t start = 0;
    while (start <= input.size()) {
        const std::size_t comma = input.find(',', start);
        const std::string token =
            input.substr(start, comma == std::string::npos ? std::string::npos
                                                           : comma - start);
        const int value = std::atoi(token.c_str());
        if (value > 0) {
            values.push_back(value);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (values.empty()) {
        return fallback;
    }
    return values;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<int> thread_counts =
        ParseList(argc > 1 ? argv[1] : nullptr, {1, 2, 4, 8});
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 20000;
    const int fiber_rounds = argc > 3 ? std::atoi(argv[3]) : 2000;
    const int reps = argc > 4 ? std::atoi(argv[4]) : 3;

    std::printf("loadavg=%.3f procs_running=%d rounds=%d fiber_rounds=%d "
                "reps=%d\n",
                ReadLoadAverage(), ReadProcessesRunning(), rounds,
                fiber_rounds, reps);
    std::printf("%-10s %-14s %-14s %-14s\n", "threads", ModeName(Mode::kSpin),
                ModeName(Mode::kSyncMutex), ModeName(Mode::kStdMutex));

    std::printf("=== native threads (ns/op, median of %d) ===\n", reps);
    for (int threads : thread_counts) {
        const double spin = RunNative<go2cpp::sync::SpinMutex>(threads, rounds, reps);
        const double sync_mutex = RunNative<go2cpp::sync::Mutex>(threads, rounds, reps);
        const double std_mutex = RunNative<std::mutex>(threads, rounds, reps);
        std::printf("%-10d %-14.1f %-14.1f %-14.1f\n", threads, spin,
                    sync_mutex, std_mutex);
    }

    std::printf("=== fibers (workers x 2 G, ns/op, median of %d) ===\n", reps);
    for (int workers : thread_counts) {
        const int fibers = workers * 2;
        const double spin =
            RunFiber(workers, fibers, fiber_rounds, Mode::kSpin, reps);
        const double sync_mutex =
            RunFiber(workers, fibers, fiber_rounds, Mode::kSyncMutex, reps);
        std::printf("%-10d %-14.1f %-14.1f %-14s\n", workers, spin, sync_mutex,
                    "n/a");
    }

    std::printf("done\n");
    return 0;
}
