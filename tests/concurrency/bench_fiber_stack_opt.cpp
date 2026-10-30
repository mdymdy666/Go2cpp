/**
 * @file bench_fiber_stack_opt.cpp
 * @brief Fiber 栈管理优化基准（独立可执行，不注册 ctest）。
 *
 * 覆盖：
 *  1. `lifecycle`：单 Fiber 反复创建→首切→完成→析构（热缓存命中）；
 *  2. `cold`：40 种不同 reserve 轮换（打穿线程缓存，测 arena 大小类 + 回退）；
 *  3. `deep`：约 8MiB 深递归（几何增长，配合 strace -c 数 mprotect 次数）；
 *  4. `concurrent N`：N 个 Fiber 同时存活 → 批量完成 → 析构，报告 VmSize/RSS；
 *  5. `waves N`：3 轮 N 并发，验证槽复用后 VmSize 峰值不随轮次增长。
 *
 * 计时以**线程 CPU 时间**为主（CLOCK_THREAD_CPUTIME_ID）：宿主负载高时墙钟
 * 会被抢占放大数倍，CPU 时间才能跨版本公平对比；墙钟一并打印。
 *
 * 构建（不改 CMake）：
 *   g++ -O2 -DNDEBUG -std=c++17 -Iinclude \
 *       tests/concurrency/bench_fiber_stack_opt.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_fiber -lpthread -o /tmp/bench_fiber_stack_opt
 *
 * 用法：
 *   /tmp/bench_fiber_stack_opt lifecycle 200000
 *   /tmp/bench_fiber_stack_opt cold 2000
 *   /tmp/bench_fiber_stack_opt deep 30000
 *   /tmp/bench_fiber_stack_opt concurrent 100000
 *   /tmp/bench_fiber_stack_opt waves 20000
 */
#include "go2cpp/fiber.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::Fiber;
using go2cpp::SuspendReason;

inline std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

inline std::uint64_t CpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
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

void ReportMemory(const char* tag) {
    std::printf("[%s] VmSize=%.2f GiB Rss=%.1f MiB\n", tag,
                double(StatusKiB("VmSize:")) / 1024.0 / 1024.0,
                double(StatusKiB("VmRSS:")) / 1024.0);
}

__attribute__((noinline)) std::uint64_t DeepWalk(std::size_t depth) {
    volatile char pad[256];
    pad[0] = static_cast<char>(depth & 0x7F);
    pad[255] = static_cast<char>((depth >> 7) & 0x7F);
    if (depth == 0) {
        return static_cast<std::uint64_t>(
            static_cast<unsigned char>(pad[0]) +
            static_cast<unsigned char>(pad[255]));
    }
    const std::uint64_t below = DeepWalk(depth - 1);
    return below + static_cast<std::uint64_t>(
                       static_cast<unsigned char>(pad[0]) +
                       static_cast<unsigned char>(pad[255]));
}

/// @brief 单 Fiber 完整生命周期（热路径：栈槽从 arena 空闲表复用）。
void BenchLifecycle(std::uint64_t iterations) {
    for (std::uint64_t i = 0; i < iterations / 10 + 1; ++i) {
        Fiber warm([] { Fiber::Suspend(SuspendReason::Park); }, 4096);
        warm.resume();
        warm.resume();
    }
    const std::uint64_t wall = NowNs();
    const std::uint64_t cpu = CpuNs();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        Fiber fiber([] { Fiber::Suspend(SuspendReason::Park); }, 4096);
        if (!fiber.resume()) {
            std::printf("[lifecycle] resume failed\n");
            return;
        }
        fiber.resume();
    }
    const std::uint64_t cpu_elapsed = CpuNs() - cpu;
    const std::uint64_t wall_elapsed = NowNs() - wall;
    std::printf("[lifecycle] cpu %.1f ns/op  wall %.1f ns/op  (n=%llu)\n",
                double(cpu_elapsed) / double(iterations),
                double(wall_elapsed) / double(iterations),
                (unsigned long long)iterations);
    ReportMemory("lifecycle");
}

/// @brief 40 种 reserve 轮换：线程缓存必然失配，走 arena 大小类或逐栈回退。
void BenchCold(std::uint64_t iterations) {
    const std::uint64_t wall = NowNs();
    const std::uint64_t cpu = CpuNs();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        const std::size_t reserve = (1 + (i % 40)) * 1024U * 1024U;
        Fiber fiber([] { Fiber::Suspend(SuspendReason::Park); }, 4096,
                    reserve, true);
        if (!fiber.resume()) {
            return;
        }
        fiber.resume();
    }
    const std::uint64_t cpu_elapsed = CpuNs() - cpu;
    const std::uint64_t wall_elapsed = NowNs() - wall;
    std::printf("[cold]      cpu %.1f ns/op  wall %.1f ns/op  (n=%llu, 40 sizes)\n",
                double(cpu_elapsed) / double(iterations),
                double(wall_elapsed) / double(iterations),
                (unsigned long long)iterations);
    ReportMemory("cold");
}

/// @brief 约 8MiB 的深递归，验证几何增长的耗时（配合 strace -c 数 mprotect）。
void BenchDeep(std::size_t depth) {
    std::uint64_t checksum = 0;
    Fiber fiber(
        [&checksum, depth] { checksum = DeepWalk(depth); },
        64U * 1024U, 16U * 1024U * 1024U, true);
    const std::uint64_t wall = NowNs();
    const std::uint64_t cpu = CpuNs();
    if (!fiber.resume()) {
        std::printf("[deep] resume failed\n");
        return;
    }
    const std::uint64_t cpu_elapsed = CpuNs() - cpu;
    const std::uint64_t wall_elapsed = NowNs() - wall;
    std::printf("[deep]      depth=%zu checksum=%llu cpu %.2f ms (%.0f ns/frame) "
                "wall %.2f ms\n",
                depth, (unsigned long long)checksum,
                double(cpu_elapsed) / 1e6,
                double(cpu_elapsed) / double(depth + 1),
                double(wall_elapsed) / 1e6);
}

/// @brief N 个 Fiber 同时存活 → 批量完成 → 析构，报告分阶段成本与内存。
void BenchConcurrent(std::size_t count) {
    std::vector<std::unique_ptr<Fiber>> fibers;
    fibers.reserve(count);
    std::uint64_t cpu = CpuNs();
    std::uint64_t wall = NowNs();
    for (std::size_t i = 0; i < count; ++i) {
        fibers.push_back(std::make_unique<Fiber>(
            [] { Fiber::Suspend(SuspendReason::Park); }, 4096));
    }
    std::printf("[create]    cpu %.2f us/fiber  wall %.2f us/fiber\n",
                double(CpuNs() - cpu) / double(count) / 1000.0,
                double(NowNs() - wall) / double(count) / 1000.0);
    ReportMemory("alive-objects");

    cpu = CpuNs();
    wall = NowNs();
    for (auto& fiber : fibers) {
        (void)fiber->resume();
    }
    std::printf("[first]     cpu %.2f us/fiber  wall %.2f us/fiber\n",
                double(CpuNs() - cpu) / double(count) / 1000.0,
                double(NowNs() - wall) / double(count) / 1000.0);
    ReportMemory("alive-stacks");

    cpu = CpuNs();
    wall = NowNs();
    for (auto& fiber : fibers) {
        (void)fiber->resume();
    }
    std::printf("[finish]    cpu %.2f us/fiber  wall %.2f us/fiber\n",
                double(CpuNs() - cpu) / double(count) / 1000.0,
                double(NowNs() - wall) / double(count) / 1000.0);
    ReportMemory("finished");

    cpu = CpuNs();
    wall = NowNs();
    fibers.clear();
    std::printf("[destroy]   cpu %.2f us/fiber  wall %.2f us/fiber\n",
                double(CpuNs() - cpu) / double(count) / 1000.0,
                double(NowNs() - wall) / double(count) / 1000.0);
    ReportMemory("destroyed");
}

/// @brief 3 轮 N 并发：验证第二批起的峰值不随轮次增长（槽复用）。
void BenchWaves(std::size_t count) {
    for (int wave = 0; wave < 3; ++wave) {
        std::vector<std::unique_ptr<Fiber>> fibers;
        fibers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            fibers.push_back(std::make_unique<Fiber>(
                [] { Fiber::Suspend(SuspendReason::Park); }, 4096));
        }
        for (auto& fiber : fibers) {
            (void)fiber->resume();
        }
        std::printf("[wave %d] peak VmSize=%.2f GiB Rss=%.1f MiB\n", wave,
                    double(StatusKiB("VmSize:")) / 1024.0 / 1024.0,
                    double(StatusKiB("VmRSS:")) / 1024.0);
        for (auto& fiber : fibers) {
            (void)fiber->resume();
        }
        fibers.clear();
        std::printf("[wave %d] after VmSize=%.2f GiB Rss=%.1f MiB\n", wave,
                    double(StatusKiB("VmSize:")) / 1024.0 / 1024.0,
                    double(StatusKiB("VmRSS:")) / 1024.0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "lifecycle";
    const std::uint64_t value =
        argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
    if (mode == "lifecycle") {
        BenchLifecycle(value != 0 ? value : 200000);
    } else if (mode == "cold") {
        BenchCold(value != 0 ? value : 2000);
    } else if (mode == "deep") {
        BenchDeep(value != 0 ? static_cast<std::size_t>(value) : 30000);
    } else if (mode == "concurrent") {
        BenchConcurrent(value != 0 ? static_cast<std::size_t>(value) : 100000);
    } else if (mode == "waves") {
        BenchWaves(value != 0 ? static_cast<std::size_t>(value) : 20000);
    } else {
        std::printf("unknown mode: %s\n", mode.c_str());
        return 2;
    }
    return 0;
}
