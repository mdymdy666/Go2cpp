/**
 * @file bench_m_affinity.cpp
 * @brief Fiber M 亲和 / runnext / 唤醒策略的独立基准（不注册 ctest）。
 *
 * @date 2026-10-30
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 用法
 *
 * ```
 * ./bench_m_affinity              # 全部场景
 * ./bench_m_affinity handoff      # 1P/1M 两个 G 的 channel ping-pong
 * ./bench_m_affinity parkwake     # 1P/1M 单 G park/wake 往返
 * ./bench_m_affinity cross        # 2P/2M 单 G park/wake（观察迁移）
 * ./bench_m_affinity policy       # 三种唤醒策略 A/B（新 API）
 * ./bench_m_affinity runnext      # runnext 命中率（新 API）
 * ```
 *
 * ## 基线对比
 *
 * 同一份源码可用 `-DGO2CPP_BENCH_NEW_API=0` 编译并链接旧库（没有
 * `set_wake_policy`/新增 metrics 字段），得到改动前的 handoff/parkwake
 * 数据；默认 `GO2CPP_BENCH_NEW_API=1`。
 *
 * ## 口径
 *
 * - `handoff`：一次往返 = 两次 park + 两次 resume + 两次 channel 交接；
 * - `parkwake`：主线程忙等 G 进入 Waiting 后 wake，测的是完整往返
 *   （park 提交 → 挂起 → 唤醒 → 恢复 → 再次 park 前），包含调度器路径；
 * - 所有数字为 Release（-O3 -DNDEBUG）单进程实测，环境为 WSL2。
 */
#include "go2cpp/channel.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#ifndef GO2CPP_BENCH_NEW_API
#define GO2CPP_BENCH_NEW_API 1
#endif

namespace {

using namespace go2cpp;
using namespace std::chrono_literals;

using Clock = std::chrono::steady_clock;

double NsPerOp(Clock::time_point begin, Clock::time_point end,
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
    config.collect_metrics = true;
    config.enable_sysmon = false;
    config.terminal_collect_interval = 0;
    config.allow_idle_shrink = false;
    return config;
}

// 1P/1M：两个 G 在无缓冲 channel 上 ping-pong。一次往返 = 2 次上下文切换。
void BenchHandoff(std::size_t rounds) {
    Scheduler scheduler(BenchConfig(1, 1));
    scheduler.start();
    auto channel = Channel<int>::Create(0);
    auto back = Channel<int>::Create(0);
    auto producer = scheduler.spawn([&] {
        for (std::size_t i = 0; i < rounds; ++i) {
            (void)channel->Send(static_cast<int>(i));
            (void)back->Recv();
        }
    });
    auto consumer = scheduler.spawn([&] {
        for (std::size_t i = 0; i < rounds; ++i) {
            (void)channel->Recv();
            (void)back->Send(static_cast<int>(i));
        }
    });
    const auto begin = Clock::now();
    const bool ok = producer->wait_for(120s) && consumer->wait_for(120s);
    const auto end = Clock::now();
    std::printf("[handoff]  1P/1M channel ping-pong rounds=%zu  %.2f ns/round "
                "(%.2f ns/switch, 2 switches/round) ok=%d\n",
                rounds, NsPerOp(begin, end, rounds),
                NsPerOp(begin, end, rounds) / 2.0, static_cast<int>(ok));
    scheduler.shutdown();
}

// 1P/1M：单 G 反复 park/wake，主线程忙等后唤醒。
void BenchParkWake(std::size_t rounds) {
    Scheduler scheduler(BenchConfig(1, 1));
    scheduler.start();
    std::atomic<int> phase{0};  // 0=运行中 1=已 park
    auto task = scheduler.spawn([&] {
        auto self = Scheduler::current_task();
        for (std::size_t i = 0; i < rounds; ++i) {
            phase.store(1, std::memory_order_release);
            if (!self || !scheduler.park(self)) {
                break;
            }
        }
    });
    const auto begin = Clock::now();
    for (std::size_t i = 0; i < rounds; ++i) {
        while (phase.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        if (!scheduler.wake(task)) {
            break;
        }
        phase.store(0, std::memory_order_release);
    }
    const auto end = Clock::now();
    const bool ok = task->wait_for(120s);
    std::printf("[parkwake] 1P/1M park/wake rounds=%zu  %.2f ns/op ok=%d\n",
                rounds, NsPerOp(begin, end, rounds), static_cast<int>(ok));
#if GO2CPP_BENCH_NEW_API
    const auto m = scheduler.metrics();
    const std::uint64_t total = m.same_machine_resumes + m.cross_machine_resumes;
    std::printf("[parkwake] same=%llu cross=%llu same_rate=%.1f%% "
                "runnext=%llu affinity=%llu\n",
                static_cast<unsigned long long>(m.same_machine_resumes),
                static_cast<unsigned long long>(m.cross_machine_resumes),
                total == 0 ? 0.0
                           : 100.0 * static_cast<double>(m.same_machine_resumes) /
                                 static_cast<double>(total),
                static_cast<unsigned long long>(m.runnext_pops),
                static_cast<unsigned long long>(m.affinity_wakes));
#endif
    scheduler.shutdown();
}

// 2P/2M：单 G park/wake，观察跨 M 迁移比例。
void BenchCross(std::size_t rounds) {
    Scheduler scheduler(BenchConfig(2, 2));
    scheduler.start();
    std::atomic<int> phase{0};
    auto task = scheduler.spawn([&] {
        auto self = Scheduler::current_task();
        for (std::size_t i = 0; i < rounds; ++i) {
            phase.store(1, std::memory_order_release);
            if (!self || !scheduler.park(self)) {
                break;
            }
        }
    });
    const auto begin = Clock::now();
    for (std::size_t i = 0; i < rounds; ++i) {
        while (phase.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        if (!scheduler.wake(task)) {
            break;
        }
        phase.store(0, std::memory_order_release);
    }
    const auto end = Clock::now();
    const bool ok = task->wait_for(120s);
    std::printf("[cross]   2P/2M park/wake rounds=%zu  %.2f ns/op ok=%d\n",
                rounds, NsPerOp(begin, end, rounds), static_cast<int>(ok));
#if GO2CPP_BENCH_NEW_API
    const auto m = scheduler.metrics();
    const std::uint64_t total = m.same_machine_resumes + m.cross_machine_resumes;
    std::printf("[cross]   same=%llu cross=%llu same_rate=%.1f%% "
                "runnext=%llu affinity=%llu\n",
                static_cast<unsigned long long>(m.same_machine_resumes),
                static_cast<unsigned long long>(m.cross_machine_resumes),
                total == 0 ? 0.0
                           : 100.0 * static_cast<double>(m.same_machine_resumes) /
                                 static_cast<double>(total),
                static_cast<unsigned long long>(m.runnext_pops),
                static_cast<unsigned long long>(m.affinity_wakes));
#endif
    scheduler.shutdown();
}

#if GO2CPP_BENCH_NEW_API
const char* PolicyName(scheduler::WakePolicy policy) {
    switch (policy) {
        case scheduler::WakePolicy::kLastMachine:
            return "last_m";
        case scheduler::WakePolicy::kRunNext:
            return "runnext";
        case scheduler::WakePolicy::kRoundRobin:
            return "round_robin";
    }
    return "?";
}

void BenchPolicies(std::size_t rounds) {
    const scheduler::WakePolicy policies[] = {
        scheduler::WakePolicy::kLastMachine,
        scheduler::WakePolicy::kRunNext,
        scheduler::WakePolicy::kRoundRobin,
    };
    for (const auto policy : policies) {
        Scheduler scheduler(BenchConfig(2, 2));
        scheduler.set_wake_policy(policy);
        scheduler.start();
        std::atomic<int> phase{0};
        auto task = scheduler.spawn([&] {
            auto self = Scheduler::current_task();
            for (std::size_t i = 0; i < rounds; ++i) {
                phase.store(1, std::memory_order_release);
                if (!self || !scheduler.park(self)) {
                    break;
                }
            }
        });
        const auto begin = Clock::now();
        for (std::size_t i = 0; i < rounds; ++i) {
            while (phase.load(std::memory_order_acquire) == 0) {
                std::this_thread::yield();
            }
            if (!scheduler.wake(task)) {
                break;
            }
            phase.store(0, std::memory_order_release);
        }
        const auto end = Clock::now();
        const bool ok = task->wait_for(120s);
        const auto m = scheduler.metrics();
        const std::uint64_t total =
            m.same_machine_resumes + m.cross_machine_resumes;
        std::printf("[policy]  %-11s %.2f ns/op same_rate=%.1f%% "
                    "affinity=%llu runnext=%llu ok=%d\n",
                    PolicyName(policy), NsPerOp(begin, end, rounds),
                    total == 0 ? 0.0
                               : 100.0 *
                                     static_cast<double>(m.same_machine_resumes) /
                                     static_cast<double>(total),
                    static_cast<unsigned long long>(m.affinity_wakes),
                    static_cast<unsigned long long>(m.runnext_pops),
                    static_cast<int>(ok));
        scheduler.shutdown();
    }
}

void BenchRunnextHitRate() {
    Scheduler scheduler(BenchConfig(2, 2));
    scheduler.start();
    auto channel = Channel<int>::Create(0);
    constexpr int kRounds = 20000;
    auto producer = scheduler.spawn([&] {
        for (int i = 0; i < kRounds; ++i) {
            (void)channel->Send(i);
        }
    });
    auto consumer = scheduler.spawn([&] {
        for (int i = 0; i < kRounds; ++i) {
            (void)channel->Recv();
        }
    });
    const auto begin = Clock::now();
    const bool ok = producer->wait_for(120s) && consumer->wait_for(120s);
    const auto end = Clock::now();
    const auto m = scheduler.metrics();
    const std::uint64_t fast = m.runnext_pops + m.inbox_pops;
    const std::uint64_t queue_pops = m.local_queue_pops + m.steal_pops;
    const std::uint64_t total = fast + queue_pops;
    std::printf("[runnext] 2P/2M channel ping-pong %.2f ns/round ok=%d\n",
                NsPerOp(begin, end, kRounds), static_cast<int>(ok));
    std::printf("[runnext] fast=%llu (runnext=%llu inbox=%llu) queue=%llu "
                "(local=%llu steal=%llu) hit_rate=%.1f%% pushes=%llu "
                "displaced=%llu redirects=%llu\n",
                static_cast<unsigned long long>(fast),
                static_cast<unsigned long long>(m.runnext_pops),
                static_cast<unsigned long long>(m.inbox_pops),
                static_cast<unsigned long long>(queue_pops),
                static_cast<unsigned long long>(m.local_queue_pops),
                static_cast<unsigned long long>(m.steal_pops),
                total == 0 ? 0.0
                           : 100.0 * static_cast<double>(fast) /
                                 static_cast<double>(total),
                static_cast<unsigned long long>(m.runnext_pushes),
                static_cast<unsigned long long>(m.runnext_displaced),
                static_cast<unsigned long long>(m.pinned_redirects));
    scheduler.shutdown();
}
#endif

}  // namespace

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    const std::size_t rounds = argc > 2
                                   ? static_cast<std::size_t>(std::strtoull(
                                         argv[2], nullptr, 10))
                                   : 20000;
    if (only.empty() || only == "handoff") {
        BenchHandoff(rounds);
    }
    if (only.empty() || only == "parkwake") {
        BenchParkWake(rounds);
    }
    if (only.empty() || only == "cross") {
        BenchCross(rounds);
    }
#if GO2CPP_BENCH_NEW_API
    if (only.empty() || only == "policy") {
        BenchPolicies(rounds);
    }
    if (only.empty() || only == "runnext") {
        BenchRunnextHitRate();
    }
#else
    std::printf("[new-api] disabled: policy/runnext scenarios skipped\n");
#endif
    return 0;
}
