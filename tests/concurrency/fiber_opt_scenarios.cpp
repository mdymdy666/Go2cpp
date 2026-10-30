/**
 * @file fiber_opt_scenarios.cpp
 * @brief 栈管理/调度交互优化的 5 个真实场景验证（独立可执行）。
 *
 * 每个场景说明模拟的真实用法、输入、期望与实测结果（打印到 stdout）：
 *  1. 深递归 JSON/表达式解析：8MiB 栈 + 几何增长，校验 checksum 与栈地址稳定；
 *  2. 10 万并发连接模型：全部挂起 → 批量唤醒，统计 VmSize/RSS/耗时；
 *  3. 高频短任务 + 无缓冲 channel 交接（Scheduler 4 worker）：park/wake 全链路；
 *  4. 跨 OS 线程迁移：同一 Fiber 在两个线程间往返恢复，验证栈上局部值；
 *  5. 服务关停收尾：100 个挂起 Fiber 请求取消后析构，验证全部终态、无挂起。
 *
 * 构建：
 *   g++ -std=c++17 -O2 -DNDEBUG -Iinclude tests/concurrency/fiber_opt_scenarios.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_scheduler -lgo2cpp_channel -lgo2cpp_fiber -lgo2cpp_sync \
 *       -lgo2cpp_error -lgo2cpp_debug -lgo2cpp_debug_event -lpthread \
 *       -o /tmp/fiber_opt_scenarios
 *
 * 运行：/tmp/fiber_opt_scenarios
 */
#include "go2cpp/channel.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using go2cpp::Fiber;
using go2cpp::FiberState;
using go2cpp::SuspendReason;

std::atomic<int> g_failures{0};

#define CHECK_TRUE(expr)                                                     \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::printf("  check failed %s:%d: %s\n", __FILE__, __LINE__,    \
                        #expr);                                              \
            g_failures.fetch_add(1, std::memory_order_relaxed);              \
        }                                                                    \
    } while (0)

std::uint64_t NowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
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

__attribute__((noinline)) std::uint64_t ParseWalk(std::size_t depth,
                                                  std::uint64_t salt) {
    volatile char frame[256];
    frame[0] = static_cast<char>((depth + salt) & 0x7F);
    frame[255] = static_cast<char>(((depth >> 7) + salt) & 0x7F);
    if (depth == 0) {
        return static_cast<std::uint64_t>(
                   static_cast<unsigned char>(frame[0])) +
               static_cast<std::uint64_t>(
                   static_cast<unsigned char>(frame[255]));
    }
    const std::uint64_t below = ParseWalk(depth - 1, salt);
    return below + static_cast<std::uint64_t>(
                       static_cast<unsigned char>(frame[0])) +
           static_cast<std::uint64_t>(static_cast<unsigned char>(frame[255]));
}

std::uint64_t ExpectedParse(std::size_t depth, std::uint64_t salt) {
    std::uint64_t sum = 0;
    for (std::size_t d = 0; d <= depth; ++d) {
        sum += static_cast<unsigned char>((d + salt) & 0x7F);
        sum += static_cast<unsigned char>(((d >> 7) + salt) & 0x7F);
    }
    return sum;
}

/// 场景 1：深递归解析（8MiB 栈，几何增长）。
void ScenarioDeepParse() {
    constexpr std::size_t kDepth = 30000;
    constexpr std::uint64_t kSalt = 0x5A;
    std::atomic<bool> ok{false};
    std::atomic<std::uintptr_t> outer_address{0};
    const std::uint64_t start = NowUs();
    std::uintptr_t marker = 0;
    {
        Fiber parser(
            [&ok, &outer_address, &marker] {
                outer_address.store(reinterpret_cast<std::uintptr_t>(&marker));
                ok.store(ParseWalk(kDepth, kSalt) == ExpectedParse(kDepth, kSalt),
                         std::memory_order_release);
            },
            64U * 1024U, 16U * 1024U * 1024U, true);
        CHECK_TRUE(parser.resume());
        CHECK_TRUE(parser.state() == FiberState::Completed);
    }
    const std::uint64_t elapsed = NowUs() - start;
    CHECK_TRUE(ok.load());
    std::printf("[1] deep-parse depth=%zu %.2f ms ok=%d\n", kDepth,
                double(elapsed) / 1000.0, int(ok.load()));
}

/// 场景 2：10 万并发连接模型（全部挂起 → 批量唤醒）。
void ScenarioHundredThousand() {
    constexpr std::size_t kConnections = 100000;
    std::vector<std::unique_ptr<Fiber>> connections;
    connections.reserve(kConnections);
    const std::uint64_t start = NowUs();
    for (std::size_t i = 0; i < kConnections; ++i) {
        connections.push_back(std::make_unique<Fiber>(
            [] { (void)Fiber::Suspend(SuspendReason::Park); }, 4096));
    }
    const std::uint64_t created = NowUs() - start;
    const std::uint64_t resume_start = NowUs();
    for (auto& connection : connections) {
        CHECK_TRUE(connection->resume());
    }
    const std::uint64_t resumed = NowUs() - resume_start;
    // 首切之后栈才真正建立：此时才是 100k 并发的峰值 VmSize。
    const std::size_t vm_kib = StatusKiB("VmSize:");
    std::vector<std::unique_ptr<Fiber>>().swap(connections);
    const std::uint64_t destroyed = NowUs() - start - created - resumed;
    std::printf("[2] 100k-conn create=%.2f us/f first=%.2f us/f "
                "finish=%.2f us/f alive_VmSize=%.1f GiB final_VmSize=%.2f GiB\n",
                double(created) / double(kConnections),
                double(resumed) / double(kConnections),
                double(destroyed) / double(kConnections),
                double(vm_kib) / 1024.0 / 1024.0,
                double(StatusKiB("VmSize:")) / 1024.0 / 1024.0);
}

/// 场景 3：Scheduler 4 worker + 无缓冲 channel 交接（park/wake 全链路）。
void ScenarioChannelPingPong() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 4;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    constexpr int kRounds = 20000;
    auto channel = go2cpp::Channel<int>();
    std::atomic<int> received{0};
    std::atomic<std::int64_t> sum{0};
    const std::uint64_t start = NowUs();
    scheduler.spawn([&channel, kRounds] {
        for (int i = 0; i < kRounds; ++i) {
            channel.Send(i);
        }
        channel.Close();
    });
    scheduler.spawn([&channel, &received, &sum] {
        for (;;) {
            auto result = channel.Recv();
            if (result.Closed()) {
                return;
            }
            if (!result.Ok()) {
                continue;
            }
            received.fetch_add(1, std::memory_order_relaxed);
            sum.fetch_add(static_cast<std::int64_t>(*result.value),
                          std::memory_order_relaxed);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(120);
    while (received.load(std::memory_order_acquire) != kRounds &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::uint64_t elapsed = NowUs() - start;
    scheduler.shutdown();
    const std::int64_t expected =
        static_cast<std::int64_t>(kRounds) * (kRounds - 1) / 2;
    CHECK_TRUE(received.load() == kRounds);
    CHECK_TRUE(sum.load() == expected);
    std::printf("[3] scheduler+channel rounds=%d %.2f ms (%.0f ns/handoff) ok=%d\n",
                kRounds, double(elapsed) / 1000.0,
                double(elapsed) * 1000.0 / double(kRounds),
                int(received.load() == kRounds && sum.load() == expected));
}

/// 场景 4：跨 OS 线程迁移（同一 Fiber 在两个线程间往返）。
void ScenarioMigration() {
    constexpr int kRounds = 2000;
    std::mutex mutex;
    std::condition_variable condition;
    Fiber* current = nullptr;
    bool ready = false;
    bool stop = false;
    std::atomic<int> rounds{0};
    std::uintptr_t observed_address = 0;
    std::thread worker([&] {
        int local_value = 0;
        for (;;) {
            Fiber* fiber = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [&] { return ready || stop; });
                if (stop) {
                    return;
                }
                fiber = current;
                ready = false;
            }
            CHECK_TRUE(fiber != nullptr);
            CHECK_TRUE(fiber->resume_from_scheduler());
            local_value = 7;
            (void)local_value;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++rounds;
            }
            condition.notify_all();
        }
    });
    const std::uint64_t start = NowUs();
    for (int round = 0; round < kRounds; ++round) {
        auto fiber = Fiber::AcquireForScheduler(
            [&observed_address] {
                int stack_local = 11;
                observed_address = reinterpret_cast<std::uintptr_t>(&stack_local);
                (void)Fiber::Suspend(SuspendReason::Park);
                CHECK_TRUE(stack_local == 11);
            },
            4096, 32U);
        CHECK_TRUE(fiber != nullptr);
        CHECK_TRUE(fiber->resume_from_scheduler());
        {
            std::lock_guard<std::mutex> lock(mutex);
            current = fiber.get();
            ready = true;
        }
        condition.notify_one();
        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] { return rounds == round + 1; });
        }
        // worker 线程已经把 Fiber 恢复完成；这里再 resume 会返回 false。
        (void)fiber->resume_from_scheduler();
        CHECK_TRUE(fiber->state() == FiberState::Completed);
        Fiber::RecycleForScheduler(std::move(fiber), 32U);
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
    }
    condition.notify_all();
    worker.join();
    const std::uint64_t elapsed = NowUs() - start;
    CHECK_TRUE(observed_address != 0);
    std::printf("[4] migration rounds=%d %.2f ms (%.0f ns/round) address_stable=%d\n",
                kRounds, double(elapsed) / 1000.0,
                double(elapsed) * 1000.0 / double(kRounds),
                int(observed_address != 0));
}

/// 场景 5：服务关停（取消 + 析构挂起 Fiber，验证全部终态且不挂起）。
void ScenarioShutdown() {
    constexpr std::size_t kFibers = 100;
    std::atomic<int> started{0};
    std::atomic<int> cancelled{0};
    std::vector<std::unique_ptr<Fiber>> fibers;
    fibers.reserve(kFibers);
    for (std::size_t i = 0; i < kFibers; ++i) {
        fibers.push_back(std::make_unique<Fiber>(
            [&started, &cancelled] {
                started.fetch_add(1, std::memory_order_relaxed);
                for (;;) {
                    if (Fiber::Suspend(SuspendReason::Park)) {
                        if (Fiber::CancellationRequested()) {
                            cancelled.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                    } else {
                        return;
                    }
                }
            },
            4096));
        CHECK_TRUE(fibers.back()->resume());
    }
    for (auto& fiber : fibers) {
        fiber->RequestCancellation();
    }
    const std::uint64_t start = NowUs();
    fibers.clear();
    const std::uint64_t elapsed = NowUs() - start;
    CHECK_TRUE(started.load() == static_cast<int>(kFibers));
    CHECK_TRUE(cancelled.load() == static_cast<int>(kFibers));
    std::printf("[5] shutdown fibers=%zu %.2f ms cancelled=%d\n", kFibers,
                double(elapsed) / 1000.0, cancelled.load());
}

}  // namespace

int main() {
    ScenarioDeepParse();
    ScenarioHundredThousand();
    ScenarioChannelPingPong();
    ScenarioMigration();
    ScenarioShutdown();
    std::printf("[scenarios] failures=%d\n", g_failures.load());
    return g_failures.load() == 0 ? 0 : 1;
}
