/**
 * @file fiber_arena_stress.cpp
 * @brief 栈 arena 多线程压力测试（Release 构建，不注册 ctest）。
 *
 * ASan/TSan 构建按设计强制固定栈（不走 arena），因此 arena 的并发/复用
 * 路径只能由 Release 构建覆盖。本程序：
 *  1. 8 个线程各自做 2000 次「创建（8MiB 预留）→ 深递归校验 → 挂起 →
 *     恢复 → 完成 → 析构」，验证槽在多线程间复用不串数据、不崩溃；
 *  2. 跨线程交接：A 线程创建并挂起，B 线程恢复完成并析构，验证跨线程
 *     归还 arena 的路径；
 *  3. 结束后 VmSize 必须回落到基线附近（有界保留），且校验和全部正确。
 *
 * 构建：
 *   g++ -std=c++17 -O2 -DNDEBUG -Iinclude tests/concurrency/fiber_arena_stress.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_fiber -lpthread -o /tmp/fiber_arena_stress
 *
 * 运行：/tmp/fiber_arena_stress [threads] [iterations]
 */
#include "go2cpp/fiber.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {

using go2cpp::Fiber;
using go2cpp::SuspendReason;

std::atomic<std::size_t> g_failures{0};

#define CHECK_TRUE(expr)                                                     \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::printf("check failed at %s:%d: %s\n", __FILE__, __LINE__,   \
                        #expr);                                              \
            g_failures.fetch_add(1, std::memory_order_relaxed);              \
        }                                                                    \
    } while (0)

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

/// @brief 每层约 256B 的递归，返回可校验的 checksum。
__attribute__((noinline)) std::uint64_t Walk(std::size_t depth,
                                             std::uint64_t salt) {
    volatile char pad[256];
    pad[0] = static_cast<char>((depth + salt) & 0x7F);
    pad[255] = static_cast<char>(((depth >> 7) + salt) & 0x7F);
    if (depth == 0) {
        return static_cast<std::uint64_t>(
                   static_cast<unsigned char>(pad[0])) +
               static_cast<std::uint64_t>(
                   static_cast<unsigned char>(pad[255]));
    }
    const std::uint64_t below = Walk(depth - 1, salt);
    return below + static_cast<std::uint64_t>(
                       static_cast<unsigned char>(pad[0])) +
           static_cast<std::uint64_t>(static_cast<unsigned char>(pad[255]));
}

std::uint64_t Expected(std::size_t depth, std::uint64_t salt) {
    std::uint64_t sum = 0;
    for (std::size_t d = 0; d <= depth; ++d) {
        sum += static_cast<unsigned char>((d + salt) & 0x7F);
        sum += static_cast<unsigned char>(((d >> 7) + salt) & 0x7F);
    }
    return sum;
}

void WorkerScenario(int thread_index, std::size_t iterations) {
    for (std::size_t i = 0; i < iterations; ++i) {
        const std::uint64_t salt =
            static_cast<std::uint64_t>(thread_index) * 131 + i;
        const std::size_t depth = 200 + (i % 800);
        std::atomic<bool> body_ok{false};
        auto fiber = Fiber::AcquireForScheduler(
            [depth, salt, &body_ok] {
                const std::uint64_t got = Walk(depth, salt);
                body_ok.store(got == Expected(depth, salt),
                              std::memory_order_release);
                (void)Fiber::Suspend(SuspendReason::Yield);
                (void)Fiber::Suspend(SuspendReason::Yield);
            },
            4096, 64U);
        CHECK_TRUE(fiber != nullptr);
        CHECK_TRUE(fiber->resume_from_scheduler());
        CHECK_TRUE(fiber->resume_from_scheduler());
        CHECK_TRUE(fiber->resume_from_scheduler());
        CHECK_TRUE(fiber->state() == go2cpp::FiberState::Completed);
        CHECK_TRUE(body_ok.load(std::memory_order_acquire));
        Fiber::RecycleForScheduler(std::move(fiber), 64U);
    }
}

/// @brief A 线程创建并挂起，B 线程恢复完成并析构（跨线程归还 arena 槽）。
void CrossThreadScenario() {
    constexpr std::size_t kRounds = 500;
    for (std::size_t round = 0; round < kRounds; ++round) {
        auto fiber = Fiber::AcquireForScheduler(
            [] {
                (void)Fiber::Suspend(SuspendReason::Park);
            },
            4096, 16U);
        CHECK_TRUE(fiber != nullptr);
        CHECK_TRUE(fiber->resume_from_scheduler());
        std::atomic<bool> completed{false};
        std::thread other([&fiber, &completed] {
            CHECK_TRUE(fiber->resume_from_scheduler());
            completed.store(fiber->state() == go2cpp::FiberState::Completed,
                            std::memory_order_release);
        });
        other.join();
        CHECK_TRUE(completed.load(std::memory_order_acquire));
        Fiber::RecycleForScheduler(std::move(fiber), 16U);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int threads = argc > 1 ? std::atoi(argv[1]) : 8;
    const std::size_t iterations =
        argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2000;
    const std::size_t before_kib = StatusKiB("VmSize:");
    std::printf("[cfg] threads=%d iterations=%zu baseline VmSize=%.2f GiB\n",
                threads, iterations,
                double(before_kib) / 1024.0 / 1024.0);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int index = 0; index < threads; ++index) {
        workers.emplace_back(WorkerScenario, index, iterations);
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CrossThreadScenario();

    const std::size_t after_kib = StatusKiB("VmSize:");
    std::printf("[result] failures=%zu final VmSize=%.2f GiB (+%.1f MiB)\n",
                g_failures.load(std::memory_order_relaxed),
                double(after_kib) / 1024.0 / 1024.0,
                double(after_kib - before_kib) / 1024.0);
    // 8 个线程各保留 ≤64 个槽，槽 8MiB：≤4GiB；同时允许测试进程本身的映射。
    CHECK_TRUE(after_kib <= before_kib + 5U * 1024U * 1024U);
    return g_failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}
