/**
 * @file tests/concurrency/test_worker_lifecycle.cpp
 * @brief 基础 M / 补充 M 回收策略的独立验收测试。
 *
 * 覆盖的需求：
 * - 无 BlockingRegion 时 worker 数恒等于 processor_count（不再空闲缩容）；
 * - 单个 / 10 个并发 BlockingRegion 触发补充 M，结束后立即回落到基线；
 * - 阻塞期间补充 M 参与调度，可运行任务不被饿死；
 * - shutdown 时基础 M 与补充 M 全部退出；
 * - 随机交错 spawn / BlockingRegion 后 worker 数不超 max_workers 且无泄漏。
 *
 * 独立 main：不接入 ctest（cmake/Go2CppTests.cmake 不修改），用
 * `bash scripts/run_worker_lifecycle_test.sh` 编译运行。
 */
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

std::atomic<int> g_failures{0};
std::atomic<int> g_checks{0};

void report(bool ok, const char* expr, const char* file, int line) {
    g_checks.fetch_add(1, std::memory_order_relaxed);
    if (ok) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[FAIL] %s:%d: %s\n", file, line, expr);
}

#define CHECK(expr) report((expr), #expr, __FILE__, __LINE__)

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = 5s,
                std::chrono::milliseconds tick = 1ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(tick);
    }
    return predicate();
}

// 采样极值（无锁）。
void note_min(std::atomic<std::size_t>& target, std::size_t value) {
    auto current = target.load(std::memory_order_relaxed);
    while (value < current &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed)) {
    }
}

void note_max(std::atomic<std::size_t>& target, std::size_t value) {
    auto current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed)) {
    }
}

struct PipePair {
    int read_fd{-1};
    int write_fd{-1};

    bool open() {
        int fds[2];
        if (::pipe(fds) != 0) {
            return false;
        }
        read_fd = fds[0];
        write_fd = fds[1];
        return true;
    }

    void release() {
        if (write_fd >= 0) {
            const char token = 'x';
            if (::write(write_fd, &token, 1) < 0) {
                // 写失败说明管道已关闭，忽略。
            }
        }
    }

    void close() {
        if (read_fd >= 0) {
            ::close(read_fd);
            read_fd = -1;
        }
        if (write_fd >= 0) {
            ::close(write_fd);
            write_fd = -1;
        }
    }

    ~PipePair() { close(); }
};

std::shared_ptr<go2cpp::Task> spawn_blocking_read(
    go2cpp::Scheduler& scheduler, int read_fd,
    std::atomic<int>* entered = nullptr,
    std::atomic<int>* exited = nullptr) {
    return scheduler.spawn([read_fd, entered, exited] {
        go2cpp::BlockingRegion region;
        if (!region.active()) {
            report(false, "BlockingRegion active", __FILE__, __LINE__);
        }
        if (entered != nullptr) {
            entered->fetch_add(1, std::memory_order_release);
        }
        char token = 0;
        if (::read(read_fd, &token, 1) < 0) {
            // 读失败与读到数据同样表示阻塞结束，无需额外处理。
        }
        if (exited != nullptr) {
            exited->fetch_add(1, std::memory_order_release);
        }
    });
}

// ---------------------------------------------------------------------------
// 场景 1：无 BlockingRegion，1000 万次纯计算，worker 数恒定不抖动。
// ---------------------------------------------------------------------------
void scenario_stable_base_workers() {
    std::printf("[场景1] 无阻塞声明：1000 万次计算，worker 数应恒为 P\n");
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 8;
    config.enable_sysmon = true;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    CHECK(scheduler.worker_count() == 4);

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> sampled_min{static_cast<std::size_t>(-1)};
    std::atomic<std::size_t> sampled_max{0};
    std::atomic<std::size_t> samples{0};
    std::thread sampler([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto count = scheduler.worker_count();
            note_min(sampled_min, count);
            note_max(sampled_max, count);
            samples.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(2ms);
        }
    });

    constexpr int kTasks = 8;
    constexpr int kIterations = 1250000;  // 8 × 125 万 = 1000 万次
    std::atomic<int> done{0};
    for (int task = 0; task < kTasks; ++task) {
        scheduler.spawn([&done, task] {
            volatile std::uint64_t accumulator = static_cast<std::uint64_t>(task);
            for (int index = 0; index < kIterations; ++index) {
                accumulator += static_cast<std::uint64_t>(index) * 3U;
            }
            (void)accumulator;
            done.fetch_add(1, std::memory_order_release);
        });
    }
    CHECK(wait_until([&] { return done.load(std::memory_order_acquire) == kTasks; },
                     60s));
    // 计算结束后继续观察空闲期：基础 M 不得因 idle_worker_timeout 退出。
    std::this_thread::sleep_for(500ms);
    stop.store(true, std::memory_order_release);
    sampler.join();

    std::printf("    采样 %zu 次：worker min=%zu max=%zu（基线 P=4）\n",
                samples.load(), sampled_min.load(), sampled_max.load());
    CHECK(samples.load() > 20);
    CHECK(sampled_min.load() == 4);
    CHECK(sampled_max.load() == 4);
    CHECK(scheduler.worker_count() == 4);
    for (const auto& machine : scheduler.machines()) {
        CHECK(!machine.replacement);
    }
    scheduler.shutdown();
    CHECK(scheduler.worker_count() == 0);
}

// ---------------------------------------------------------------------------
// 场景 2：单个 1 秒 read_f：worker +1，结束后立即回到 P。
// ---------------------------------------------------------------------------
void scenario_single_long_read() {
    std::printf("[场景2] 单个 read_f 1 秒：worker 数 +1，结束后 -1\n");
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 8;
    config.enable_sysmon = true;
    config.sysmon_interval = 5ms;
    config.long_syscall_threshold = 20ms;
    config.idle_worker_timeout = 50ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    CHECK(scheduler.worker_count() == 4);

    PipePair pipe;
    CHECK(pipe.open());
    std::atomic<int> entered{0};
    std::atomic<int> exited{0};
    auto reader = spawn_blocking_read(scheduler, pipe.read_fd, &entered, &exited);

    // sysmon 检测到长 syscall 后解绑 P 并补一个替代 M。
    CHECK(wait_until([&] { return scheduler.worker_count() == 5; }, 3s));
    CHECK(entered.load(std::memory_order_acquire) == 1);
    std::this_thread::sleep_for(900ms);
    CHECK(scheduler.worker_count() == 5);

    // 记录补充 M 的可观测标记。
    std::size_t replacements = 0;
    for (const auto& machine : scheduler.machines()) {
        if (machine.replacement) {
            ++replacements;
        }
    }
    CHECK(replacements >= 1);

    pipe.release();
    CHECK(reader->wait_for(3s));
    CHECK(exited.load(std::memory_order_acquire) == 1);
    // 阻塞结束 → 补充 M 立即退休；不依赖 250ms 空闲超时。
    CHECK(wait_until([&] { return scheduler.worker_count() == 4; }, 2s));
    std::printf("    阻塞期间 worker=5，结束后 worker=%zu\n",
                scheduler.worker_count());
    std::this_thread::sleep_for(300ms);
    CHECK(scheduler.worker_count() == 4);
    scheduler.shutdown();
    CHECK(scheduler.worker_count() == 0);
    pipe.close();
}

// ---------------------------------------------------------------------------
// 场景 3：10 个并发 read_f：worker = P + 10，结束后回到 P。
// ---------------------------------------------------------------------------
void scenario_ten_concurrent_reads() {
    std::printf("[场景3] 10 个并发 read_f：worker 数 +10，结束后回基线\n");
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 24;
    config.allow_worker_oversubscription = true;
    config.enable_sysmon = true;
    config.sysmon_interval = 5ms;
    config.long_syscall_threshold = 20ms;
    config.idle_worker_timeout = 50ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    CHECK(scheduler.worker_count() == 4);

    std::vector<std::unique_ptr<PipePair>> pipes;
    std::vector<std::shared_ptr<go2cpp::Task>> readers;
    std::atomic<int> exited{0};
    for (int index = 0; index < 10; ++index) {
        auto pipe = std::make_unique<PipePair>();
        CHECK(pipe->open());
        readers.push_back(
            spawn_blocking_read(scheduler, pipe->read_fd, nullptr, &exited));
        pipes.push_back(std::move(pipe));
    }
    CHECK(wait_until([&] { return scheduler.worker_count() == 14; }, 5s));
    std::this_thread::sleep_for(200ms);
    CHECK(scheduler.worker_count() == 14);
    CHECK(scheduler.worker_count() <= config.max_workers);

    for (auto& pipe : pipes) {
        pipe->release();
    }
    for (auto& reader : readers) {
        CHECK(reader->wait_for(3s));
    }
    CHECK(exited.load(std::memory_order_acquire) == 10);
    CHECK(wait_until([&] { return scheduler.worker_count() == 4; }, 3s));
    std::printf("    10 路并发阻塞峰值 worker=14，收尾 worker=%zu\n",
                scheduler.worker_count());
    std::this_thread::sleep_for(300ms);
    CHECK(scheduler.worker_count() == 4);
    scheduler.shutdown();
    CHECK(scheduler.worker_count() == 0);
}

// ---------------------------------------------------------------------------
// 场景 4：read_f 期间大量可运行 G：补充 M 参与调度，没有饿死。
// ---------------------------------------------------------------------------
void scenario_replacement_joins_scheduling() {
    std::printf("[场景4] read_f 期间大量 G：补充 M 参与调度\n");
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 16;
    config.enable_sysmon = true;
    config.sysmon_interval = 5ms;
    config.long_syscall_threshold = 20ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    CHECK(scheduler.worker_count() == 4);

    PipePair pipe;
    CHECK(pipe.open());
    auto reader = spawn_blocking_read(scheduler, pipe.read_fd);
    CHECK(wait_until([&] { return scheduler.worker_count() == 5; }, 3s));

    // 用 4 个「占住 M 直到被释放」的长任务做并发观测。比短任务计数更稳定：
    // 一台基础 M 被 read 占住后，剩下的 3 个基础 M + 1 个补充 M 必须能同时
    // 进入任务体，running 才会达到 4；若补充 M 不存在，最多只有 3。
    std::atomic<int> running{0};
    std::atomic<int> long_done{0};
    std::atomic<bool> release_long{false};
    for (int index = 0; index < 4; ++index) {
        scheduler.spawn([&running, &long_done, &release_long] {
            running.fetch_add(1, std::memory_order_acq_rel);
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!release_long.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            running.fetch_sub(1, std::memory_order_acq_rel);
            long_done.fetch_add(1, std::memory_order_release);
        });
    }
    CHECK(wait_until(
        [&] { return running.load(std::memory_order_acquire) == 4; }, 10s));
    std::printf("    阻塞期间并发运行长任务 running=%d（3 基础 + 1 补充）\n",
                running.load(std::memory_order_acquire));
    CHECK(scheduler.worker_count() <= 5);
    release_long.store(true, std::memory_order_release);
    CHECK(wait_until(
        [&] { return long_done.load(std::memory_order_acquire) == 4; }, 10s));

    // 短任务吞吐：阻塞期间与补充 M 一起推进，不出现饿死。
    constexpr int kTasks = 500;
    std::atomic<int> done{0};
    for (int index = 0; index < kTasks; ++index) {
        scheduler.spawn([&done] {
            volatile std::uint64_t accumulator = 0;
            for (int spin = 0; spin < 2000; ++spin) {
                accumulator += static_cast<std::uint64_t>(spin);
            }
            (void)accumulator;
            done.fetch_add(1, std::memory_order_release);
        });
    }
    CHECK(wait_until([&] { return done.load(std::memory_order_acquire) == kTasks; },
                     15s));
    CHECK(scheduler.worker_count() <= 5);

    pipe.release();
    CHECK(reader->wait_for(3s));
    CHECK(wait_until([&] { return scheduler.worker_count() == 4; }, 3s));
    scheduler.shutdown();
    CHECK(scheduler.worker_count() == 0);
    pipe.close();
}

// ---------------------------------------------------------------------------
// 场景 5：shutdown 时仍有补充 M：全部退出，无挂起。
// ---------------------------------------------------------------------------
void scenario_shutdown_with_replacements() {
    std::printf("[场景5] shutdown 时有补充 M：全部退出\n");
    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 8;
    config.enable_sysmon = true;
    config.sysmon_interval = 5ms;
    config.long_syscall_threshold = 20ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    CHECK(scheduler.worker_count() == 2);

    PipePair first;
    PipePair second;
    CHECK(first.open());
    CHECK(second.open());
    auto a = spawn_blocking_read(scheduler, first.read_fd);
    auto b = spawn_blocking_read(scheduler, second.read_fd);
    CHECK(wait_until([&] { return scheduler.worker_count() == 4; }, 3s));

    std::thread releaser([&] {
        std::this_thread::sleep_for(200ms);
        first.release();
        second.release();
    });
    const auto started = std::chrono::steady_clock::now();
    const bool clean = scheduler.shutdown_for(10s);
    releaser.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    CHECK(clean);
    CHECK(elapsed < 10s);
    CHECK(scheduler.worker_count() == 0);
    a.reset();
    b.reset();
    first.close();
    second.close();
}

// ---------------------------------------------------------------------------
// 场景 6：随机交错 spawn / BlockingRegion：不超上限、无泄漏。
// ---------------------------------------------------------------------------
void scenario_random_interleaving() {
    std::printf("[场景6] 随机交错 spawn/BlockingRegion：上限与泄漏检查\n");
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 16;
    config.enable_sysmon = true;
    config.sysmon_interval = 3ms;
    config.long_syscall_threshold = 10ms;
    config.idle_worker_timeout = 20ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    CHECK(scheduler.worker_count() == 4);

    std::mt19937 rng(20251018U);
    std::vector<std::unique_ptr<PipePair>> pipes;
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    std::atomic<int> done{0};
    int spawned = 0;

    for (int round = 0; round < 120; ++round) {
        const int kind = static_cast<int>(rng() % 10U);
        if (kind < 3) {
            auto pipe = std::make_unique<PipePair>();
            CHECK(pipe->open());
            tasks.push_back(spawn_blocking_read(scheduler, pipe->read_fd));
            if ((rng() & 1U) != 0U) {
                pipe->release();
            }
            pipes.push_back(std::move(pipe));
            ++spawned;
        } else {
            const int batch = 1 + static_cast<int>(rng() % 12U);
            for (int index = 0; index < batch; ++index) {
                tasks.push_back(scheduler.spawn([&done] {
                    volatile std::uint64_t accumulator = 0;
                    for (int spin = 0; spin < 64; ++spin) {
                        accumulator += static_cast<std::uint64_t>(spin);
                    }
                    (void)accumulator;
                    done.fetch_add(1, std::memory_order_release);
                }));
                ++spawned;
            }
        }
        CHECK(scheduler.worker_count() <= config.max_workers);
    }

    for (auto& pipe : pipes) {
        pipe->release();
    }
    for (auto& task : tasks) {
        CHECK(task->wait_for(15s));
    }
    CHECK(wait_until([&] { return scheduler.worker_count() == 4; }, 5s));
    std::this_thread::sleep_for(300ms);
    CHECK(scheduler.worker_count() == 4);
    CHECK(scheduler.runnable_count() == 0);
    scheduler.shutdown();
    CHECK(scheduler.worker_count() == 0);
    for (auto& pipe : pipes) {
        pipe->close();
    }
    std::printf("    随机交错：spawn %d 个任务，全部收敛\n", spawned);
}

// ---------------------------------------------------------------------------
// 场景 7：频繁 BlockingRegion（1000 次 read_f）：反复扩容/回收无泄漏。
// ---------------------------------------------------------------------------
void scenario_frequent_blocking_regions() {
    std::printf("[场景7] 100 轮 × 10 个并发 read_f：反复扩容/回收\n");
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 32;
    config.allow_worker_oversubscription = true;
    config.enable_sysmon = true;
    config.sysmon_interval = 5ms;
    config.long_syscall_threshold = 50ms;  // 短阻塞主要靠 runnable 需求扩容
    config.idle_worker_timeout = 20ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    CHECK(scheduler.worker_count() == 4);

    std::size_t peak = 0;
    int blocked_total = 0;
    for (int round = 0; round < 100; ++round) {
        std::vector<std::unique_ptr<PipePair>> pipes;
        std::vector<std::shared_ptr<go2cpp::Task>> tasks;
        std::atomic<int> done{0};
        for (int index = 0; index < 10; ++index) {
            auto pipe = std::make_unique<PipePair>();
            CHECK(pipe->open());
            tasks.push_back(spawn_blocking_read(scheduler, pipe->read_fd));
            pipes.push_back(std::move(pipe));
            ++blocked_total;
        }
        for (int index = 0; index < 40; ++index) {
            tasks.push_back(scheduler.spawn([&done] {
                volatile std::uint64_t accumulator = 0;
                for (int spin = 0; spin < 2000; ++spin) {
                    accumulator += static_cast<std::uint64_t>(spin);
                }
                (void)accumulator;
                done.fetch_add(1, std::memory_order_release);
            }));
        }
        for (int sample = 0; sample < 10; ++sample) {
            peak = std::max(peak, scheduler.worker_count());
            std::this_thread::sleep_for(500us);
        }
        for (auto& pipe : pipes) {
            pipe->release();
        }
        bool all_done = true;
        for (auto& task : tasks) {
            all_done = task->wait_for(5s) && all_done;
        }
        CHECK(all_done);
        CHECK(wait_until([&] { return scheduler.worker_count() == 4; }, 3s));
    }
    const auto records = scheduler.machines().size();
    std::printf(
        "    %d 次 read_f，worker 峰值=%zu（上限 14），收尾 worker=%zu，"
        "machines 记录=%zu\n",
        blocked_total, peak, scheduler.worker_count(), records);
    CHECK(peak <= 14);
    CHECK(peak >= 5);
    CHECK(scheduler.worker_count() == 4);
    // 死亡 M 记录必须被 reap：1000 次补充 M 创建/退休后不能线性累积。
    CHECK(records <= 32);
    scheduler.shutdown();
    CHECK(scheduler.worker_count() == 0);
}

// ---------------------------------------------------------------------------
// 场景 8：allow_idle_shrink=true 时保留历史的空闲缩容行为（配置项有效）。
// ---------------------------------------------------------------------------
void scenario_legacy_idle_shrink_opt_in() {
    std::printf("[场景8] allow_idle_shrink=true：历史空闲缩容仍可用\n");
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.min_workers = 1;
    config.max_workers = 4;
    config.allow_idle_shrink = true;
    config.idle_worker_timeout = 20ms;
    config.enable_sysmon = false;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    CHECK(scheduler.worker_count() == 1);

    std::atomic<int> done{0};
    for (int index = 0; index < 32; ++index) {
        scheduler.spawn([&done] {
            volatile std::uint64_t accumulator = 0;
            for (int spin = 0; spin < 20000; ++spin) {
                accumulator += static_cast<std::uint64_t>(spin);
            }
            (void)accumulator;
            done.fetch_add(1, std::memory_order_release);
        });
    }
    CHECK(wait_until([&] { return done.load(std::memory_order_acquire) == 32; },
                     10s));
    CHECK(wait_until([&] { return scheduler.worker_count() == 4; }, 3s));
    CHECK(wait_until([&] { return scheduler.worker_count() == 1; }, 3s));
    std::printf("    缩容模式：1 -> 4 -> %zu\n", scheduler.worker_count());
    scheduler.shutdown();
    CHECK(scheduler.worker_count() == 0);
}

}  // namespace

int main() {
    std::printf("=== 基础 M / 补充 M 回收策略验收 ===\n");
    scenario_stable_base_workers();
    scenario_single_long_read();
    scenario_ten_concurrent_reads();
    scenario_replacement_joins_scheduling();
    scenario_shutdown_with_replacements();
    scenario_frequent_blocking_regions();
    scenario_legacy_idle_shrink_opt_in();
    scenario_random_interleaving();

    std::printf("=== checks=%d failures=%d ===\n", g_checks.load(),
                g_failures.load());
    return g_failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}
