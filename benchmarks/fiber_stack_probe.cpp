/**
 * @file fiber_stack_probe.cpp
 * @brief 测定 Fiber / 线程在不同栈容量与帧大小下可承受的**函数调用深度**。
 *
 * 为什么必须 fork：
 *   爆栈会触碰 guard page → SIGSEGV → 进程被内核杀死（无 handler、无 sigaltstack）。
 *   因此每一次"探深"都必须在独立子进程里进行，父进程通过 waitpid 的终止状态
 *   判断这次是"安全返回"还是"爆栈身亡"。这是唯一能测出临界深度的办法。
 *
 * 三个测量维度：
 *   1. 深度上限 —— 给定 (栈容量, 每帧字节数)，二分搜索"刚好不崩"的最大深度；
 *   2. 帧大小敏感性 —— 验证 深度 × 帧 ≈ 栈容量 这个线性关系是否成立；
 *   3. Fiber 换栈方案 —— 验证"递归到一定深度就 resume 一个新 Fiber"能否
 *      突破单栈上限（即 Go 的栈增长的手动等价物）。
 *
 * 输出为 CSV 行，便于外部统计。
 */
#include "go2cpp/fiber.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <alloca.h>
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// 每个平台的最低可用栈（含 guard 页）。
constexpr std::size_t kMinFiberStack = 8U * 1024U;
/// 线程对照用的栈大小（贴近 Linux 默认 8 MiB，但显式指定以免受 ulimit 影响）。
constexpr std::size_t kThreadStack = 8U * 1024U * 1024U;
/// 递归深度搜索上界，防止在某些大栈 + 小帧组合下无限增长。
constexpr std::size_t kMaxProbeDepth = 4'000'000U;

volatile std::uint64_t g_sink = 0;

/**
 * @brief 吃栈递归：每层用 alloca 申请 frame_bytes 字节并写入。
 * @details noinline 是必须的 —— 否则编译器可能把递归折叠成循环，
 *          那样就不再消耗栈，测出来的深度会虚高一个数量级。
 */
__attribute__((noinline)) std::uint64_t eat_stack(std::size_t depth,
                                                  std::size_t frame_bytes) {
    volatile char* frame = static_cast<volatile char*>(alloca(frame_bytes + 64U));
    frame[0] = static_cast<char>(depth & 0x7F);
    frame[frame_bytes] = static_cast<char>((depth >> 7) & 0x7F);
    if (depth == 0) {
        return static_cast<std::uint64_t>(frame[0]);
    }
    const std::uint64_t below = eat_stack(depth - 1, frame_bytes);
    return below + static_cast<std::uint64_t>(frame[0]);
}

/// 子进程语义：跑一次指定组合，正常返回 0；爆栈则由信号终止。
enum class Outcome { kOk, kCrash, kOther };

/// 在 fork 出来的子进程里执行给定 body，返回其结局。
template <typename Body>
Outcome run_isolated(Body&& body) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return Outcome::kOther;
    }
    if (pid == 0) {
        // 子进程：stdout/stderr 全部丢弃，避免探深过程中的日志污染 CSV。
        if (std::freopen("/dev/null", "w", stdout) == nullptr ||
            std::freopen("/dev/null", "w", stderr) == nullptr) {
            ::_exit(90);  // 无法丢弃输出：让父进程按异常结局处理
        }
        body();
        ::_exit(0);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return Outcome::kOther;
    }
    if (WIFSIGNALED(status)) {
        return Outcome::kCrash;          // SIGSEGV 等
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return Outcome::kOk;
    }
    return Outcome::kOther;
}

/// 在独立 Fiber 里递归指定深度。
Outcome probe_fiber(std::size_t stack_size, std::size_t frame_bytes,
                    std::size_t depth) {
    return run_isolated([&] {
        go2cpp::Fiber fiber([&] { g_sink += eat_stack(depth, frame_bytes); },
                            stack_size);
        (void)fiber.resume();
    });
}

/**
 * @brief 二分搜索刚好不崩的最大深度。
 * @return 安全深度；若连 depth=1 都崩则返回 0。
 */
template <typename Probe>
std::size_t find_critical_depth(Probe&& probe) {
    if (probe(1) != Outcome::kOk) {
        return 0;
    }
    std::size_t lo = 1;                    // 已知安全
    std::size_t hi = 2;                    // 未知
    // 指数扩张找到第一个崩溃点
    while (hi <= kMaxProbeDepth && probe(hi) == Outcome::kOk) {
        lo = hi;
        hi *= 2;
    }
    if (hi > kMaxProbeDepth) {
        return lo;
    }
    // 在 (lo, hi] 里二分
    while (lo + 1 < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (probe(mid) == Outcome::kOk) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/// 用 pthread 显式指定栈大小跑递归。
Outcome probe_pthread(std::size_t stack_size, std::size_t frame_bytes,
                      std::size_t depth) {
    return run_isolated([&] {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        const std::size_t minimum_stack =
            static_cast<std::size_t>(PTHREAD_STACK_MIN);
        pthread_attr_setstacksize(&attr, stack_size < minimum_stack
                                             ? minimum_stack
                                             : stack_size);
        pthread_t tid;
        auto trampoline = [](void* arg) -> void* {
            auto* payload = static_cast<std::pair<std::size_t, std::size_t>*>(arg);
            g_sink += eat_stack(payload->first, payload->second);
            return nullptr;
        };
        std::pair<std::size_t, std::size_t> payload{depth, frame_bytes};
        if (pthread_create(&tid, &attr, trampoline, &payload) != 0) {
            pthread_attr_destroy(&attr);
            return;
        }
        pthread_join(tid, nullptr);
        pthread_attr_destroy(&attr);
    });
}

}  // namespace

int main(int argc, char** argv) {
    const bool quick = (argc > 1 && std::strcmp(argv[1], "--quick") == 0);

    std::printf("kind,stack_bytes,frame_bytes,critical_depth,bytes_per_depth,"
                "stack_utilization\n");

    std::vector<std::size_t> stacks = {kMinFiberStack, 16U * 1024U, 32U * 1024U,
                                       64U * 1024U, 128U * 1024U, 256U * 1024U,
                                       512U * 1024U, 1024U * 1024U};
    std::vector<std::size_t> frames = {0U, 16U, 64U, 256U, 1024U};
    if (quick) {
        stacks = {128U * 1024U, 1024U * 1024U};
        frames = {0U, 256U};
    }

    // ---- 场景 A：Fiber 栈 ----
    for (const std::size_t stack : stacks) {
        for (const std::size_t frame : frames) {
            const std::size_t depth = find_critical_depth(
                [&](std::size_t d) { return probe_fiber(stack, frame, d); });
            const double per_depth =
                depth == 0 ? 0.0
                           : static_cast<double>(stack) / static_cast<double>(depth);
            std::printf("fiber,%zu,%zu,%zu,%.2f,%.4f\n", stack, frame, depth,
                        per_depth,
                        depth == 0 ? 0.0
                                   : (per_depth * static_cast<double>(depth)) /
                                         static_cast<double>(stack));
            std::fflush(stdout);
        }
    }

    // ---- 场景 B：线程（pthread，8 MiB）对照 ----
    for (const std::size_t frame : frames) {
        const std::size_t depth = find_critical_depth(
            [&](std::size_t d) { return probe_pthread(kThreadStack, frame, d); });
        const double per_depth =
            depth == 0 ? 0.0
                       : static_cast<double>(kThreadStack) / static_cast<double>(depth);
        std::printf("pthread,%zu,%zu,%zu,%.2f,%.4f\n", kThreadStack, frame, depth,
                    per_depth,
                    depth == 0 ? 0.0
                               : (per_depth * static_cast<double>(depth)) /
                                     static_cast<double>(kThreadStack));
        std::fflush(stdout);
    }

    std::printf("# sink=%llu\n", static_cast<unsigned long long>(g_sink));
    return 0;
}
