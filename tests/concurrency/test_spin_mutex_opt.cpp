/**
 * @file test_spin_mutex_opt.cpp
 * @brief `SpinMutex` 混合自旋改造后的独立正确性测试（自带 main）。
 *
 * @date 2026-10-30
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 为什么独立于 `go2cpp_tests`
 *
 * 本轮改造只允许动 `include/go2cpp/concurrency/spin_mutex.hpp` 与
 * `tests/concurrency` 下的文件，不能改 `tests/test_main.cpp` 与 CMake 的
 * 模块清单。
 * 因此本文件自带 `main`、以独立可执行程序运行（与 `bench_spin_mutex_opt.cpp`
 * 同一编译方式），覆盖**新增机制**：自旋快路径、门路径、超时、fiber 争用。
 * 原有的契约测试仍在 `test_spin_mutex.cpp`（`GO2CPP_TEST_FILTER=spin_mutex`）。
 *
 * ## 覆盖场景（6 个）
 *
 * 1. **高争用互斥性**：8 线程 × 2000 次，临界区内对**非原子** `long long`
 *    自增；丢失更新即失败。覆盖自旋路径与门的交接；
 * 2. **长临界区（门）**：持锁者睡 30ms，6 个等待者必须全部阻塞、且在
 *    持锁者释放后依序进入；覆盖 `m_impl` 门的 park/唤醒；
 * 3. **LockFor 超时**：持锁 40ms，等待者 `LockFor(15ms)` 必须超时返回
 *    false、不持锁，随后能正常获取；
 * 4. **TryLock 竞争**：持锁期间所有 `TryLock` 必须失败，释放后必须成功；
 * 5. **随机混合**：随机线程数/迭代数/临界区长度 + 随机 Lock/TryLock
 *    重试，校验计数守恒；
 * 6. **受管 G 高争用**：4 M × 8 G，临界区内校验 `no_park_depth()==1`，
 *    退出后归零；覆盖 Fiber 自旋/门路径与契约计数。
 *
 * ## 编译与运行
 *
 * ```
 * g++ -O3 -DNDEBUG -std=c++20 -pthread -Iinclude \
 *     tests/concurrency/test_spin_mutex_opt.cpp -L.build-bento-wsl \
 *     -Wl,-rpath,$PWD/.build-bento-wsl \
 *     -lgo2cpp_sync -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_context \
 *     -lgo2cpp_io -lgo2cpp_channel -lgo2cpp_error -lgo2cpp_control_flow \
 *     -lgo2cpp_future -lgo2cpp_network -lgo2cpp_log -lgo2cpp_config \
 *     -lgo2cpp_os -lgo2cpp_math -lgo2cpp_rand -lgo2cpp_path \
 *     -lgo2cpp_container -lgo2cpp_compress -lgo2cpp_encoding -lgo2cpp_hash \
 *     -lgo2cpp_metrics -lgo2cpp_debug -lz -lssl -lcrypto -ldl \
 *     -o /tmp/test_spin_mutex_opt && /tmp/test_spin_mutex_opt
 * ```
 */
#include "go2cpp/concurrency/spin_mutex.hpp"

#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;
using go2cpp::Task;
using go2cpp::sync::SpinMutex;

std::atomic<int> g_failures{0};

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            g_failures.fetch_add(1, std::memory_order_relaxed);            \
        }                                                                  \
    } while (0)

/// 自旋闸门：所有线程就绪后一起放行。
class Gate {
public:
    explicit Gate(int participants) : m_participants(participants) {}
    void ArriveAndWait() {
        m_arrived.fetch_add(1, std::memory_order_acq_rel);
        while (m_arrived.load(std::memory_order_acquire) < m_participants) {
            std::this_thread::yield();
        }
    }

private:
    std::atomic<int> m_arrived{0};
    int m_participants;
};

/// 场景 1：高争用互斥性（自旋路径 + 门的交接）。
void ScenarioHighContention(int threads, int per_thread, int critical_work) {
    SpinMutex mutex;
    Gate gate(threads);
    long long shared = 0;  // 故意非原子：互斥失效就会丢更新
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int index = 0; index < threads; ++index) {
        workers.emplace_back([&] {
            gate.ArriveAndWait();
            for (int iteration = 0; iteration < per_thread; ++iteration) {
                mutex.Lock();
                for (int step = 0; step < critical_work; ++step) {
                    shared += 1;
                }
                mutex.Unlock();
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const long long expected =
        static_cast<long long>(threads) * per_thread * critical_work;
    CHECK(shared == expected);
    CHECK(!mutex.IsLocked());
}

/// 场景 2：长临界区——等待者必须进入门并全部串行通过。
void ScenarioGatePath() {
    SpinMutex mutex;
    std::atomic<bool> holder_ready{false};
    std::atomic<bool> release_holder{false};
    std::thread holder([&] {
        mutex.Lock();
        holder_ready.store(true, std::memory_order_release);
        while (!release_holder.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        mutex.Unlock();
    });
    while (!holder_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    constexpr int kWaiters = 6;
    std::atomic<int> in_critical{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> completed{0};
    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);
    for (int index = 0; index < kWaiters; ++index) {
        waiters.emplace_back([&] {
            mutex.Lock();
            const int now = in_critical.fetch_add(1, std::memory_order_acq_rel) + 1;
            int observed = max_concurrent.load(std::memory_order_relaxed);
            while (now > observed &&
                   !max_concurrent.compare_exchange_weak(
                       observed, now, std::memory_order_relaxed)) {
            }
            in_critical.fetch_sub(1, std::memory_order_acq_rel);
            completed.fetch_add(1, std::memory_order_release);
            mutex.Unlock();
        });
    }
    // 等待者应全部卡在自旋/门里：给足时间仍为 0。
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(completed.load(std::memory_order_acquire) == 0);
    release_holder.store(true, std::memory_order_release);
    holder.join();
    for (auto& waiter : waiters) {
        waiter.join();
    }
    CHECK(completed.load(std::memory_order_acquire) == kWaiters);
    CHECK(max_concurrent.load(std::memory_order_relaxed) == 1);
    CHECK(!mutex.IsLocked());
}

/// 场景 3：LockFor 超时返回 false、不持锁；释放后可正常获取。
void ScenarioLockForTimeout() {
    SpinMutex mutex;
    mutex.Lock();
    std::atomic<int> outcome{-1};
    std::atomic<long long> elapsed_ms{0};
    std::thread waiter([&] {
        const auto begin = Clock::now();
        const bool acquired = mutex.LockFor(std::chrono::milliseconds(15));
        elapsed_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - begin)
                .count(),
            std::memory_order_release);
        if (acquired) {
            outcome.store(1, std::memory_order_release);
            mutex.Unlock();
        } else {
            outcome.store(0, std::memory_order_release);
        }
    });
    waiter.join();
    CHECK(outcome.load(std::memory_order_acquire) == 0);
    CHECK(elapsed_ms.load(std::memory_order_acquire) >= 10);
    // 超时后不得残留持有状态：锁仍由主线程持有。
    CHECK(mutex.IsLocked());
    mutex.Unlock();
    CHECK(!mutex.IsLocked());
    CHECK(mutex.LockFor(std::chrono::milliseconds(50)));
    mutex.Unlock();
}

/// 场景 4：TryLock 竞争——持锁期间必须全部失败。
void ScenarioTryLockCompetition(int attempts) {
    SpinMutex mutex;
    mutex.Lock();
    std::atomic<int> failures{0};
    std::atomic<int> successes{0};
    Gate gate(4);
    std::vector<std::thread> workers;
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&] {
            gate.ArriveAndWait();
            for (int attempt = 0; attempt < attempts; ++attempt) {
                if (mutex.TryLock()) {
                    successes.fetch_add(1, std::memory_order_relaxed);
                    mutex.Unlock();
                } else {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(successes.load(std::memory_order_relaxed) == 0);
    CHECK(failures.load(std::memory_order_relaxed) == attempts * 4);
    mutex.Unlock();
    // 释放后必须立刻可获取。
    CHECK(mutex.TryLock());
    mutex.Unlock();
}

/// 场景 5：随机线程数/迭代数/临界区长度 + 随机 Lock/TryLock 重试。
void ScenarioRandomized(int rounds) {
    std::mt19937 rng(0x5EED1234U);
    std::uniform_int_distribution<int> thread_dist(2, 8);
    std::uniform_int_distribution<int> iter_dist(200, 2000);
    std::uniform_int_distribution<int> work_dist(0, 8);

    for (int round = 0; round < rounds; ++round) {
        const int threads = thread_dist(rng);
        const int per_thread = iter_dist(rng);
        const int work = work_dist(rng);
        SpinMutex mutex;
        Gate gate(threads);
        long long shared = 0;
        std::vector<std::thread> workers;
        for (int index = 0; index < threads; ++index) {
            workers.emplace_back([&, index] {
                std::mt19937 local_rng(static_cast<unsigned>(index) * 7919U +
                                       static_cast<unsigned>(round));
                gate.ArriveAndWait();
                for (int iteration = 0; iteration < per_thread; ++iteration) {
                    if ((local_rng() & 1U) == 0U) {
                        mutex.Lock();
                    } else {
                        while (!mutex.TryLock()) {
                            std::this_thread::yield();
                        }
                    }
                    for (int step = 0; step < work; ++step) {
                        shared += 1;
                    }
                    mutex.Unlock();
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        CHECK(shared ==
              static_cast<long long>(threads) * per_thread * work);
        CHECK(!mutex.IsLocked());
    }
}

SchedulerConfig MakeConfig(std::size_t processors) {
    SchedulerConfig config;
    config.processor_count = processors;
    config.min_workers = processors;
    config.max_workers = processors;
    config.idle_wait = std::chrono::milliseconds(1);
    return config;
}

/// 场景 6：受管 G 高争用——契约计数正确、互斥正确。
void ScenarioFiberContention() {
    Scheduler scheduler(MakeConfig(4));
    scheduler.start();

    SpinMutex mutex;
    long long shared = 0;
    std::atomic<int> completed{0};
    constexpr int kTasks = 8;
    constexpr int kRounds = 1500;
    std::vector<std::shared_ptr<Task>> tasks;
    tasks.reserve(kTasks);
    for (int index = 0; index < kTasks; ++index) {
        tasks.push_back(scheduler.spawn([&] {
            for (int round = 0; round < kRounds; ++round) {
                mutex.Lock();
                // 临界区内必须能看到恰好 1 层 no_park_depth。
                CHECK(Scheduler::current_task_ptr()->no_park_depth() == 1U);
                shared += 1;
                // 制造一点临界区长度，让其它 G 走到自旋/门。
                volatile long long busy = 0;
                for (int step = 0; step < 32; ++step) {
                    busy += step;
                }
                (void)busy;
                mutex.Unlock();
            }
            CHECK(Scheduler::current_task_ptr()->no_park_depth() == 0U);
            completed.fetch_add(1, std::memory_order_release);
        }));
    }
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (completed.load(std::memory_order_acquire) < kTasks &&
           Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(completed.load(std::memory_order_acquire) == kTasks);
    for (auto& task : tasks) {
        CHECK(task->wait_for(std::chrono::seconds(5)));
    }
    scheduler.shutdown();
    CHECK(shared == static_cast<long long>(kTasks) * kRounds);
    CHECK(!mutex.IsLocked());
}

}  // namespace

int main() {
    // 轻量模式：给 valgrind/helgrind 等 20–100 倍减速的检查器用。
    const bool light = std::getenv("GO2CPP_TEST_LIGHT") != nullptr;
    std::printf("[test] spin_mutex_opt begin%s\n", light ? " (light)" : "");

    if (light) {
        ScenarioHighContention(4, 200, 2);
    } else {
        ScenarioHighContention(8, 2000, 4);
    }
    std::printf("[test] scenario 1 (high contention) done\n");

    ScenarioGatePath();
    std::printf("[test] scenario 2 (gate path) done\n");

    ScenarioLockForTimeout();
    std::printf("[test] scenario 3 (LockFor timeout) done\n");

    ScenarioTryLockCompetition(light ? 200 : 20000);
    std::printf("[test] scenario 4 (TryLock competition) done\n");

    ScenarioRandomized(light ? 2 : 6);
    std::printf("[test] scenario 5 (randomized) done\n");

    // TSan 下 Fiber 的自定义栈切换无法维护 shadow stack，会产生假阳性；
    // 由环境变量显式跳过 fiber 场景（native 场景仍全跑）。
    if (std::getenv("GO2CPP_TEST_SKIP_FIBER") == nullptr) {
        ScenarioFiberContention();
        std::printf("[test] scenario 6 (fiber contention) done\n");
    } else {
        std::printf("[test] scenario 6 (fiber contention) SKIPPED by env\n");
    }

    const int failures = g_failures.load(std::memory_order_relaxed);
    std::printf("[test] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
