/**
 * @file bench_scheduler_park.cpp
 * @brief Scheduler park/wake 路径基准（park+resume 往返、并发 park/wake、
 *        Fiber 裸切换、Channel / Mutex / ConditionVariable 的 park 路径）。
 *
 * @date 2026-10-30
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 用法
 *
 * 独立可执行程序（不注册 ctest、不进入 go2cpp_tests）：
 *
 * ```
 * ./bench_scheduler_park                 # 默认场景集合
 * ./bench_scheduler_park pingpong 200000 # 两个 G 互 park/wake 的往返
 * ./bench_scheduler_park ext 20000       # 外部线程唤单个 park G（含 M 唤醒）
 * ./bench_scheduler_park conc 1000 4     # 1000 个 G 同时 park 后批量唤醒
 * ./bench_scheduler_park fiber 200000    # Fiber 裸 resume/suspend 往返
 * ./bench_scheduler_park channel 200000  # 无缓冲 channel 收发往返
 * ./bench_scheduler_park cv 200000       # ParkingCondition 1P1C 往返
 * ./bench_scheduler_park mutex 200000 4  # 4 个 G 争用同一把 Mutex
 * ./bench_scheduler_park yield 199999    # Fiber yield 基线（同 M 换出）
 * ```
 *
 * ## 测量口径（拆解用）
 *
 * - `fiber`：不经过 Scheduler 的裸 Fiber 往返（主线程 resume → Fiber 内
 *   Suspend）。一次往返 = 2 次上下文切换，用来给出 Fiber 切换的底价；
 * - `pingpong`：1 P / 1 M 上两个 G 互相 `wake_registered` + `park_wait`。
 *   一次 park+resume 往返 = 1 次 park、1 次 wake、2 次 Fiber 切换；
 * - `ext`：外部线程唤醒 park 的 G，含 worker 从空闲条件变量被唤醒；
 * - `conc`：N 个 G 同时 park，主线程批量 `wake_registered`；报告
 *   wake 调用均摊与全部恢复的端到端均摊；
 * - `channel`：两个 G 在无缓冲 channel 上 ping-pong（Send/Recv 阻塞路径）；
 * - `cv`：两个 G 在 `core::ParkingCondition` 上 1P1C 往返（CondVar 的
 *   park 路径）；一次往返含 2 次 wait/notify；
 * - `mutex`：默认 1 P / 2 G（无真实争用，快路径），`mutex N P` 可用多个
 *   G/P 制造争用（争用方走 WaitNode → park_wait）；
 * - `yield`：单 G `yield_current()`，同 M 换出 + 本地队列往返的基线。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace go2cpp;
using namespace std::chrono_literals;

double NsBetween(Clock::time_point begin, Clock::time_point end,
                 std::size_t operations) {
    if (operations == 0) {
        return 0.0;
    }
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
            .count();
    return static_cast<double>(ns) / static_cast<double>(operations);
}

SchedulerConfig BenchConfig(std::size_t processors, std::size_t workers) {
    SchedulerConfig config;
    config.processor_count = processors;
    config.max_workers = workers;
    config.min_workers = workers;
    config.idle_wait = 1ms;
    config.local_queue_limit = 256;
    config.fiber_stack_size = 64U * 1024U;
    config.collect_metrics = false;
    config.enable_sysmon = false;
    config.terminal_collect_interval = 0;
    config.allow_idle_shrink = false;
    return config;
}

void PrintResult(const char* name, double ns_per_op, std::size_t ops,
                 std::size_t rounds) {
    std::printf("%-18s %10.2f ns/op   ops=%zu rounds=%zu\n", name, ns_per_op,
                ops, rounds);
}

/// @brief 非拥有别名句柄：任务的强引用在注册表/主线程，别名只用于调用。
std::shared_ptr<Task> AliasTask(Task* raw) {
    return std::shared_ptr<Task>(std::shared_ptr<Task>{}, raw);
}

bool WaitFlag(const std::atomic<bool>& flag, int spins, int sleep_us) {
    for (int spin = 0; spin < spins; ++spin) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }
    return flag.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Fiber 裸切换：不经过 Scheduler
// ---------------------------------------------------------------------------

void BenchFiber(std::size_t rounds) {
    Fiber fiber([rounds] {
        for (std::size_t i = 0; i < rounds; ++i) {
            Fiber::Suspend(SuspendReason::Yield);
        }
    });
    // 首次 resume 进入主体，其余每次恢复一次挂起点。
    const auto begin = Clock::now();
    for (std::size_t i = 0; i < rounds; ++i) {
        (void)fiber.resume();
    }
    const auto end = Clock::now();
    // 一次往返 = 两次上下文切换（进入 + 返回）。
    PrintResult("fiber_switch", NsBetween(begin, end, rounds * 2),
                rounds * 2, rounds);
}

// ---------------------------------------------------------------------------
// Scheduler park/wake
// ---------------------------------------------------------------------------

/// @brief 两个 G 在同一 M 上互相 park/wake 的往返（无 futex 唤醒）。
void BenchPingPong(std::size_t rounds) {
    Scheduler scheduler(BenchConfig(1, 1));
    scheduler.start();
    std::atomic<Task*> task_a_raw{nullptr};
    std::atomic<Task*> task_b_raw{nullptr};
    std::atomic<bool> done_a{false};
    std::atomic<bool> done_b{false};

    std::shared_ptr<Task> task_a = scheduler.spawn(
        [&scheduler, &task_a_raw, &task_b_raw, &done_a, rounds] {
            Task* const self_raw = Scheduler::current_task().get();
            task_a_raw.store(self_raw, std::memory_order_release);
            // 等 B 首次进入 kWaiting：此时唤醒 B 才是一次真正的 resume，
            // 而不是被 park 消费掉的 pending 令牌。
            for (int spin = 0; spin < 200000; ++spin) {
                Task* const other = task_b_raw.load(std::memory_order_acquire);
                if (other != nullptr && other->state() == GState::kWaiting) {
                    break;
                }
                if (!scheduler.yield_current()) {
                    done_a.store(true, std::memory_order_release);
                    return;
                }
            }
            for (std::size_t i = 0; i < rounds; ++i) {
                if (Task* const other =
                        task_b_raw.load(std::memory_order_acquire)) {
                    (void)scheduler.wake_registered(AliasTask(other));
                }
                if (!scheduler.park_wait(AliasTask(self_raw))) {
                    break;
                }
            }
            // 最后一轮把仍在 park 的对端放出来，避免收尾依赖 shutdown 取消。
            if (Task* const other =
                    task_b_raw.load(std::memory_order_acquire)) {
                (void)scheduler.wake_registered(AliasTask(other));
            }
            done_a.store(true, std::memory_order_release);
        });
    std::shared_ptr<Task> task_b = scheduler.spawn(
        [&scheduler, &task_a_raw, &task_b_raw, &done_b, rounds] {
            Task* const self_raw = Scheduler::current_task().get();
            task_b_raw.store(self_raw, std::memory_order_release);
            for (std::size_t i = 0; i < rounds; ++i) {
                // 第一次 park 之前不唤醒 A：A 还在等待 B 进入 park。
                if (i != 0) {
                    if (Task* const other =
                            task_a_raw.load(std::memory_order_acquire)) {
                        (void)scheduler.wake_registered(AliasTask(other));
                    }
                }
                if (!scheduler.park_wait(AliasTask(self_raw))) {
                    break;
                }
            }
            // A 的最后一次 park 由这里放出来（对称模型里总有一方先跑完）。
            if (Task* const other =
                    task_a_raw.load(std::memory_order_acquire)) {
                (void)scheduler.wake_registered(AliasTask(other));
            }
            done_b.store(true, std::memory_order_release);
        });

    const auto begin = Clock::now();
    if (!WaitFlag(done_a, 400000, 50) || !WaitFlag(done_b, 400000, 50)) {
        std::printf("pingpong: timeout waiting tasks (rounds=%zu)\n", rounds);
        scheduler.shutdown();
        return;
    }
    const auto end = Clock::now();
    scheduler.shutdown();
    PrintResult("pingpong", NsBetween(begin, end, rounds * 2), rounds * 2,
                rounds);
}

/// @brief 外部线程唤醒单个 park G（含 worker 从空闲等待中被唤醒）。
void BenchExternal(std::size_t rounds) {
    Scheduler scheduler(BenchConfig(1, 1));
    scheduler.start();
    std::atomic<bool> done{false};
    auto task = scheduler.spawn([&done, rounds] {
        auto self = Scheduler::current_task();
        auto* scheduler_ptr = Scheduler::current_scheduler();
        for (std::size_t i = 0; i < rounds; ++i) {
            if (!scheduler_ptr->park_wait(self)) {
                break;
            }
        }
        done.store(true, std::memory_order_release);
    });
    if (!task) {
        std::printf("ext: spawn failed\n");
        return;
    }
    for (int spin = 0; spin < 200000 && task->state() != GState::kWaiting;
         ++spin) {
        std::this_thread::sleep_for(1us);
    }
    const auto begin = Clock::now();
    std::size_t woken = 0;
    while (woken < rounds) {
        if (task->state() == GState::kWaiting) {
            (void)scheduler.wake_registered(task);
            ++woken;
        }
        if (done.load(std::memory_order_acquire)) {
            break;
        }
    }
    const auto end = Clock::now();
    scheduler.shutdown();
    if (woken == 0) {
        std::printf("ext: no wake observed\n");
        return;
    }
    PrintResult("ext_wake", NsBetween(begin, end, woken), woken, 1);
}

/// @brief N 个 G 同时 park，然后批量 wake；报告均摊吞吐。
void BenchConcurrent(std::size_t count, int processors) {
    if (count == 0) {
        return;
    }
    Scheduler scheduler(BenchConfig(static_cast<std::size_t>(processors),
                                    static_cast<std::size_t>(processors)));
    scheduler.start();
    std::atomic<std::size_t> parked{0};
    std::atomic<std::size_t> resumed{0};
    std::vector<std::shared_ptr<Task>> tasks;
    tasks.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        tasks.push_back(scheduler.spawn([&parked, &resumed] {
            auto self = Scheduler::current_task();
            auto* scheduler_ptr = Scheduler::current_scheduler();
            parked.fetch_add(1, std::memory_order_release);
            (void)scheduler_ptr->park_wait(self);
            resumed.fetch_add(1, std::memory_order_release);
        }));
    }
    for (int spin = 0; spin < 40000; ++spin) {
        if (parked.load(std::memory_order_acquire) == count) {
            bool all_waiting = true;
            for (const auto& task : tasks) {
                if (!task || task->state() != GState::kWaiting) {
                    all_waiting = false;
                    break;
                }
            }
            if (all_waiting) {
                break;
            }
        }
        std::this_thread::sleep_for(50us);
    }
    const auto begin = Clock::now();
    for (const auto& task : tasks) {
        if (task) {
            (void)scheduler.wake_registered(task);
        }
    }
    const auto wake_end = Clock::now();
    for (int spin = 0; spin < 40000 &&
                       resumed.load(std::memory_order_acquire) < count;
         ++spin) {
        std::this_thread::sleep_for(10us);
    }
    const auto end = Clock::now();
    scheduler.shutdown();
    const auto wake_total =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wake_end - begin)
            .count();
    const auto all_total =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
            .count();
    std::printf(
        "conc G=%zu P=%d  wake-call %.1f ns/G  end-to-end %.1f ns/G\n",
        count, processors,
        static_cast<double>(wake_total) / static_cast<double>(count),
        static_cast<double>(all_total) / static_cast<double>(count));
}

/// @brief 两个 G 在无缓冲 channel 上 ping-pong（Send/Recv 阻塞路径）。
void BenchChannel(std::size_t rounds) {
    Scheduler scheduler(BenchConfig(1, 2));
    scheduler.start();
    auto forward = MakeChannel<int>(0);
    auto backward = MakeChannel<int>(0);
    std::atomic<bool> done{false};
    scheduler.spawn([forward, backward, rounds] {
        for (std::size_t i = 0; i < rounds; ++i) {
            (void)forward->Send(static_cast<int>(i));
            (void)backward->Recv();
        }
    });
    scheduler.spawn([forward, backward, rounds, &done] {
        for (std::size_t i = 0; i < rounds; ++i) {
            (void)forward->Recv();
            (void)backward->Send(static_cast<int>(i));
        }
        done.store(true, std::memory_order_release);
    });
    const auto begin = Clock::now();
    for (int spin = 0; spin < 40000 && !done.load(std::memory_order_acquire);
         ++spin) {
        std::this_thread::sleep_for(100us);
    }
    const auto end = Clock::now();
    scheduler.shutdown();
    // 一次往返包含 2 次阻塞 Send + 2 次阻塞 Recv。
    PrintResult("channel", NsBetween(begin, end, rounds * 2), rounds * 2,
                rounds);
}

/// @brief 两个 G 在 ParkingCondition 上 1P1C 往返（CondVar 的 park 路径）。
void BenchCondition(std::size_t rounds) {
    Scheduler scheduler(BenchConfig(1, 2));
    scheduler.start();
    std::mutex mutex;
    core::ParkingCondition condition;
    std::size_t turn = 0;
    std::atomic<bool> done{false};
    scheduler.spawn([&mutex, &condition, &turn, rounds] {
        for (std::size_t i = 0; i < rounds; ++i) {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&turn] { return turn == 1; });
            turn = 0;
            condition.notify_one();
        }
    });
    scheduler.spawn([&mutex, &condition, &turn, &done, rounds] {
        for (std::size_t i = 0; i < rounds; ++i) {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&turn] { return turn == 0; });
            turn = 1;
            condition.notify_one();
        }
        done.store(true, std::memory_order_release);
    });
    const auto begin = Clock::now();
    for (int spin = 0; spin < 400000 && !done.load(std::memory_order_acquire);
         ++spin) {
        std::this_thread::sleep_for(50us);
    }
    const auto end = Clock::now();
    scheduler.shutdown();
    PrintResult("cv_1p1c", NsBetween(begin, end, rounds), rounds, rounds);
}

/// @brief N 个 G 反复争用同一把 Mutex（争用走 WaitNode park/wake）。
void BenchMutex(std::size_t rounds, int task_count, int processors) {
    if (task_count < 1) {
        task_count = 1;
    }
    Scheduler scheduler(BenchConfig(static_cast<std::size_t>(processors),
                                    static_cast<std::size_t>(processors)));
    scheduler.start();
    sync::Mutex mutex;
    std::atomic<std::size_t> completed{0};
    std::atomic<bool> done{false};
    const std::size_t total =
        static_cast<std::size_t>(task_count) * rounds;
    for (int t = 0; t < task_count; ++t) {
        scheduler.spawn([&mutex, &completed, rounds] {
            for (std::size_t i = 0; i < rounds; ++i) {
                mutex.Lock();
                mutex.Unlock();
            }
            completed.fetch_add(1, std::memory_order_release);
        });
    }
    const auto begin = Clock::now();
    for (int spin = 0; spin < 400000 &&
                       completed.load(std::memory_order_acquire) <
                           static_cast<std::size_t>(task_count);
         ++spin) {
        std::this_thread::sleep_for(50us);
    }
    const auto end = Clock::now();
    scheduler.shutdown();
    (void)done;
    const char* label = task_count == 1 ? "mutex" : "mutex_conc";
    PrintResult(label, NsBetween(begin, end, total), total, rounds);
}

/// @brief 单 G yield 基线：同 M 换出 + 本地队列往返。
void BenchYield(std::size_t rounds) {
    Scheduler scheduler(BenchConfig(1, 1));
    scheduler.start();
    std::atomic<bool> done{false};
    scheduler.spawn([&done, rounds] {
        auto* scheduler_ptr = Scheduler::current_scheduler();
        for (std::size_t i = 0; i < rounds; ++i) {
            if (!scheduler_ptr->yield_current()) {
                break;
            }
        }
        done.store(true, std::memory_order_release);
    });
    const auto begin = Clock::now();
    for (int spin = 0; spin < 400000 && !done.load(std::memory_order_acquire);
         ++spin) {
        std::this_thread::sleep_for(100us);
    }
    const auto end = Clock::now();
    scheduler.shutdown();
    PrintResult("yield", NsBetween(begin, end, rounds), rounds, 1);
}

std::size_t ParseSize(const char* text, std::size_t fallback) {
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    return end == text ? fallback : static_cast<std::size_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    const std::size_t value = argc > 2 ? ParseSize(argv[2], 0) : 0;
    const int extra = argc > 3 ? static_cast<int>(ParseSize(argv[3], 0)) : 0;
    std::printf("== scheduler park/wake bench ==\n");
    if (mode == "all" || mode == "fiber") {
        BenchFiber(value != 0 ? value : 200000);
    }
    if (mode == "all" || mode == "pingpong") {
        BenchPingPong(value != 0 ? value : 200000);
    }
    if (mode == "all" || mode == "ext") {
        BenchExternal(value != 0 ? value : 20000);
    }
    if (mode == "all" || mode == "yield") {
        BenchYield(value != 0 ? value : 200000);
    }
    if (mode == "all" || mode == "channel") {
        BenchChannel(value != 0 ? value : 200000);
    }
    if (mode == "all" || mode == "cv") {
        BenchCondition(value != 0 ? value : 200000);
    }
    if (mode == "all" || mode == "mutex") {
        const int tasks = extra != 0 ? extra : 4;
        BenchMutex(value != 0 ? value : 100000, tasks, tasks);
    }
    if (mode == "all" || mode == "conc") {
        BenchConcurrent(value != 0 ? value : 1000, extra != 0 ? extra : 4);
    }
    return 0;
}
