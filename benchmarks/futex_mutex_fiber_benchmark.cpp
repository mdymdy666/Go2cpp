/**
 * @file futex_mutex_fiber_benchmark.cpp
 * @brief GMP 环境下 `core::FutexMutex` vs `sync::Mutex` 的多 Fiber 竞争基准。
 *
 * ## 要回答的问题（任务 ③）
 *
 * `core::FutexMutex` 是**线程级**锁：竞争时 `FUTEX_WAIT` 睡的是 worker
 * 线程（M），不是 Fiber。项目是协程运行时，因此必须实测：
 *
 *   1. 多 Fiber 竞争同一个 `FutexMutex`，M = 1 / 4 / 8、Fiber = 2 / 8 / 64；
 *   2. 同样的负载换成 `sync::Mutex`（协程级，通知时 park/unpark G）；
 *   3. 比较总耗时与 ns/op。
 *
 * ## 死锁检测（关键）
 *
 * 若某个 M 的唯一可运行 G 睡在 `FUTEX_WAIT` 上，持有锁的 G 又排在这个 M
 * 的队列里，整个 M 就停摆 —— 表现为**永久不返回**。这不能用更长的
 * 超时“测出来”，只能判定为死锁。做法：
 *
 * - 每个 (锁, M, Fiber) 组合在**独立子进程**里跑（fork，顺序执行，
 *   一次只有一个子进程，不会 fork storm）；
 * - 子进程设 `alarm(4s)` 兜底；
 * - 父进程按 `waitpid` 的状态判定 `ok` / `DEADLOCK(timeout)`。
 *
 * ## 指标
 *
 * `ns/op` = 从 spawn 前到全部 G 完成的总墙钟 / (Fiber × rounds)，
 * 含 G 创建与调度开销（远小于锁竞争本身）。
 *
 * ## 用法
 *
 *   ./go2cpp_futex_mutex_fiber_benchmark [rounds=1000] [alarm_seconds=4]
 */

#include "go2cpp/concurrency/core/futex_mutex.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

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

struct ChildResult {
    double ms{0.0};
    long long ops{0};
};

/// 父进程与子进程之间的结果通道。
struct Pipe {
    int read_fd{-1};
    int write_fd{-1};
    bool Open() {
        int fds[2];
        if (::pipe(fds) != 0) {
            return false;
        }
        read_fd = fds[0];
        write_fd = fds[1];
        return true;
    }
    void CloseWrite() {
        if (write_fd >= 0) {
            ::close(write_fd);
            write_fd = -1;
        }
    }
    void CloseRead() {
        if (read_fd >= 0) {
            ::close(read_fd);
            read_fd = -1;
        }
    }
};

/**
 * @brief 在子进程内跑一个组合，返回总墙钟毫秒数。
 * @details 用 `Scheduler::spawn` 创建 Fiber，每个 Fiber 做 `rounds` 次
 *          lock/unlock。计时覆盖了 G 的创建，这是唯一能避免“部分 G 先跑”
 *          这一计时偏差的公平做法（创建 64 个 G 的开销 < 1%）。
 */
template <typename Mutex>
ChildResult RunCase(int machines, int fibers, int rounds, bool yield_inside) {
    go2cpp::Scheduler scheduler(static_cast<std::size_t>(machines));
    scheduler.start();
    Mutex gate;
    std::atomic<long long> counter{0};

    const auto start = Clock::now();
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(static_cast<std::size_t>(fibers));
    for (int index = 0; index < fibers; ++index) {
        tasks.push_back(scheduler.spawn([&gate, &counter, rounds, yield_inside] {
            for (int round = 0; round < rounds; ++round) {
                gate.lock();
                counter.fetch_add(1, std::memory_order_relaxed);
                if (yield_inside) {
                    // 模拟“把 FutexMutex 当 sync::Mutex 用”：临界区内出现
                    // 协作点（真实代码里的 channel/IO/park 都会这样）。
                    auto* current_scheduler = go2cpp::Scheduler::current_scheduler();
                    auto current = go2cpp::Scheduler::current_task();
                    if (current_scheduler != nullptr && current != nullptr) {
                        current_scheduler->yield(current);
                    }
                }
                gate.unlock();
            }
        }));
    }
    for (auto& task : tasks) {
        task->wait();
    }
    const auto finish = Clock::now();
    scheduler.shutdown();

    ChildResult result;
    result.ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
            .count()) /
        1e6;
    result.ops = counter.load(std::memory_order_relaxed);
    if (result.ops != static_cast<long long>(fibers) * rounds) {
        std::fprintf(stderr, "ERROR: counter=%lld expected=%lld\n", result.ops,
                     static_cast<long long>(fibers) * rounds);
        result.ops = -1;
    }
    return result;
}

enum class LockKind { kFutex, kSync };

const char* LockName(LockKind kind) {
    return kind == LockKind::kFutex ? "FutexMutex" : "sync::Mutex";
}

/**
 * @brief 在独立子进程中跑一个组合。
 * @param out 成功时写入子进程结果。
 * @return true 正常完成；false 表示子进程被 alarm 杀死（死锁/超时）或失败。
 */
bool RunInChild(LockKind kind, int machines, int fibers, int rounds,
                bool yield_inside, int alarm_seconds, ChildResult* out) {
    Pipe pipe;
    if (!pipe.Open()) {
        return false;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        // 子进程：只写结果，所有诊断走 stderr。
        pipe.CloseRead();
        ::alarm(static_cast<unsigned>(alarm_seconds));
        const ChildResult result =
            kind == LockKind::kFutex
                ? RunCase<go2cpp::core::FutexMutex>(machines, fibers, rounds,
                                                    yield_inside)
                : RunCase<go2cpp::sync::Mutex>(machines, fibers, rounds,
                                               yield_inside);
        const ssize_t written =
            ::write(pipe.write_fd, &result, sizeof(result));
        (void)written;
        ::_exit(0);
    }
    pipe.CloseWrite();
    if (pid < 0) {
        pipe.CloseRead();
        return false;
    }

    ChildResult result{};
    ssize_t received = 0;
    // 子进程最多写 sizeof(result) 字节；循环读直到收满或 EOF。
    while (received < static_cast<ssize_t>(sizeof(result))) {
        const ssize_t chunk =
            ::read(pipe.read_fd, reinterpret_cast<char*>(&result) + received,
                   sizeof(result) - static_cast<std::size_t>(received));
        if (chunk <= 0) {
            break;
        }
        received += chunk;
    }
    pipe.CloseRead();

    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return false;  // 被 alarm 杀死 = 死锁/超时
    }
    if (received != static_cast<ssize_t>(sizeof(result))) {
        return false;
    }
    if (result.ops < 0) {
        return false;
    }
    *out = result;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int rounds = 500;
    int alarm_seconds = 3;
    if (argc > 1) {
        const int value = std::atoi(argv[1]);
        if (value > 0) {
            rounds = value;
        }
    }
    if (argc > 2) {
        const int value = std::atoi(argv[2]);
        if (value > 0) {
            alarm_seconds = value;
        }
    }

    const std::vector<int> machine_list = {1, 4, 8};
    const std::vector<int> fiber_list = {2, 8, 64};

    std::printf("=== GMP 环境：FutexMutex（线程级）vs sync::Mutex（协程级）===\n");
    std::printf("rounds/fiber=%d  alarm=%ds  loadavg=%.2f\n\n", rounds,
                alarm_seconds, ReadLoadAverage());

    // 两种临界区形态：
    //   plain —— 只做一次自增（FutexMutex 契约内的极短门用法）；
    //   yield —— 临界区内让出一次（把它当 sync::Mutex 用的真实后果）。
    for (const bool yield_inside : {false, true}) {
        std::printf("## %s\n\n",
                    yield_inside
                        ? "场景 B：临界区内含协作点（yield）——检验能否替代 sync::Mutex"
                        : "场景 A：纯自增临界区（契约内用法）");
        std::printf("| 锁 | M | Fiber | 总耗时 ms | ns/op | 结果 |\n");
        std::printf("|---|---|---|---|---|---|\n");
        std::fflush(stdout);

        for (const LockKind kind : {LockKind::kFutex, LockKind::kSync}) {
            for (const int machines : machine_list) {
                for (const int fibers : fiber_list) {
                    ChildResult result{};
                    const bool ok =
                        RunInChild(kind, machines, fibers, rounds, yield_inside,
                                   alarm_seconds, &result);
                    const long long ops =
                        static_cast<long long>(fibers) * rounds;
                    if (ok) {
                        std::printf("| %s | %d | %d | %.2f | %.1f | ok |\n",
                                    LockName(kind), machines, fibers, result.ms,
                                    result.ms * 1e6 / static_cast<double>(ops));
                    } else {
                        std::printf(
                            "| %s | %d | %d | - | - | **DEADLOCK/超时** |\n",
                            LockName(kind), machines, fibers);
                    }
                    std::fflush(stdout);
                }
            }
        }
        std::printf("\n");
    }

    std::printf("final loadavg=%.2f\n", ReadLoadAverage());
    std::printf("log: FutexMutex 竞争时 FUTEX_WAIT 睡的是 worker 线程（M）。"
                "M 被睡住后该 M 上的其他 G 全部停摆；若持有锁的 G 也在等 M，"
                "即死锁。sync::Mutex 应全部 ok。\n");
    return 0;
}
