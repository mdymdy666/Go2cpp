/**
 * @file test_hook_p0.cpp
 * @brief hook 三条 P0 修复的回归测试（HOOK-01 / HOOK-02 / HOOK-03）。
 *
 * ## 覆盖
 *
 * 1. HOOK-01：已接管 socket 的 `SO_RCVTIMEO` / `SO_SNDTIMEO` 到期必须返回
 *    `-1 + EAGAIN`（POSIX socket(7)），而不是 `ETIMEDOUT`；同时
 *    `hook::error_counters().io_timeouts` 增长，证明返回的 EAGAIN 来自超时
 *    而不是「缓冲区真空」。
 * 2. HOOK-02：白名单外 ioctl（`SIOCGIFINDEX`、未知命令、终端 `TIOCGWINSZ`、
 *    文件系统 `FS_IOC_GETFLAGS`）必须透传给内核，不得伪造 `ENOTSUP`；
 *    `ioctl_passthroughs` 计数同步增长，证明调用确实经过 hook。
 * 3. HOOK-03：EINTR 重试策略有上限（`kEintrRetryLimit`），预算用尽后
 *    `ShouldRetryAfterEintr` 返回 false。
 *
 * ## 为什么 EINTR 只测策略函数
 *
 * hook 接管的 socket 一定被切到 `O_NONBLOCK`，非阻塞 syscall 不会睡眠，
 * 因此真实 `EINTR` 在 cooperative 路径上无法稳定构造（需要向 hook 内部的
 * 原始函数注入返回值）。这里锁定决策函数的边界：预算恰好 3 次、第 4 次
 * 不再重试；connect 的「EINTR 直接返回」不重试由同一份实现中的
 * `cooperative_connect` 注释与代码结构保证。
 */
#include "go2cpp/hook.hpp"
#include "go2cpp/fiber.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef FS_IOC_GETFLAGS
#define FS_IOC_GETFLAGS _IOR('f', 1, long)
#endif

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "[hook-p0] FAILED: %s\n", what);
    ++g_failures;
}

go2cpp::SchedulerConfig OneWorkerConfig() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    return config;
}

bool MakePair(int (&fds)[2]) {
    return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0;
}

void ClosePair(int (&fds)[2]) {
    if (fds[0] >= 0) {
        (void)::close(fds[0]);
        fds[0] = -1;
    }
    if (fds[1] >= 0) {
        (void)::close(fds[1]);
        fds[1] = -1;
    }
}

// ---------------------------------------------------------------------------
// 场景 1：SO_RCVTIMEO 超时 -> EAGAIN
// ---------------------------------------------------------------------------
void ScenarioRecvTimeoutIsEagain() {
    std::printf("[hook-p0][1] SO_RCVTIMEO 超时应返回 EAGAIN\n");
    go2cpp::hook::reset_error_counters();

    go2cpp::IOManager manager(OneWorkerConfig());
    Check(manager.Start(), "IOManager::Start 失败");
    int fds[2]{-1, -1};
    Check(MakePair(fds), "socketpair 失败");

    const timeval timeout{0, 30 * 1000};
    Check(::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) == 0,
          "setsockopt(SO_RCVTIMEO) 失败");

    std::atomic<int> observed_errno{0};
    std::atomic<long long> elapsed_ms{0};
    std::atomic<bool> finished{false};
    auto task = manager.Go([&] {
        const auto begin = std::chrono::steady_clock::now();
        char byte = 0;
        const ssize_t result = ::recv(fds[0], &byte, 1, 0);
        const auto end = std::chrono::steady_clock::now();
        observed_errno.store(result < 0 ? errno : 0,
                             std::memory_order_release);
        elapsed_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - begin)
                .count(),
            std::memory_order_release);
        finished.store(true, std::memory_order_release);
    });
    const bool completed = task && task->wait_for(std::chrono::seconds(3));
    Check(completed && finished.load(std::memory_order_acquire),
          "recv 超时任务未在 3s 内完成");
    const int error = observed_errno.load(std::memory_order_acquire);
    Check(error == EAGAIN, "SO_RCVTIMEO 超时应返回 EAGAIN（POSIX socket(7)）");
    Check(elapsed_ms.load(std::memory_order_acquire) >= 20,
          "SO_RCVTIMEO 超时应真的阻塞过 30ms 量级");
    const auto counters = go2cpp::hook::error_counters();
    Check(counters.io_timeouts >= 1,
          "io_timeouts 统计应记录至少一次数据面超时");
    std::printf("[hook-p0] recv errno=%d（期望 EAGAIN=%d），elapsed=%lldms，"
                "io_timeouts=%llu\n",
                error, EAGAIN,
                static_cast<long long>(elapsed_ms.load()),
                static_cast<unsigned long long>(counters.io_timeouts));

    ClosePair(fds);
    manager.Shutdown();
}

// ---------------------------------------------------------------------------
// 场景 2：SO_SNDTIMEO 超时 -> EAGAIN
// ---------------------------------------------------------------------------
bool FillSendBuffer(int fd) {
    std::vector<char> chunk(64 * 1024, 'x');
    for (int i = 0; i < 4096; ++i) {
        const ssize_t written =
            ::syscall(SYS_sendto, fd, chunk.data(), chunk.size(), MSG_DONTWAIT,
                      nullptr, 0);
        if (written < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
        if (written == 0) {
            return false;
        }
    }
    return false;
}

void ScenarioSendTimeoutIsEagain() {
    std::printf("[hook-p0][1b] SO_SNDTIMEO 超时应返回 EAGAIN\n");
    go2cpp::hook::reset_error_counters();

    go2cpp::IOManager manager(OneWorkerConfig());
    Check(manager.Start(), "IOManager::Start 失败");
    int fds[2]{-1, -1};
    Check(MakePair(fds), "socketpair 失败");

    Check(FillSendBuffer(fds[1]), "填满 socketpair 发送缓冲失败");
    const timeval timeout{0, 30 * 1000};
    Check(::setsockopt(fds[1], SOL_SOCKET, SO_SNDTIMEO, &timeout,
                       sizeof(timeout)) == 0,
          "setsockopt(SO_SNDTIMEO) 失败");

    std::atomic<int> observed_errno{0};
    std::atomic<long long> elapsed_ms{0};
    std::atomic<bool> finished{false};
    auto task = manager.Go([&] {
        const auto begin = std::chrono::steady_clock::now();
        const char byte = 's';
        const ssize_t result = ::send(fds[1], &byte, 1, MSG_NOSIGNAL);
        const auto end = std::chrono::steady_clock::now();
        observed_errno.store(result < 0 ? errno : 0,
                             std::memory_order_release);
        elapsed_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - begin)
                .count(),
            std::memory_order_release);
        finished.store(true, std::memory_order_release);
    });
    const bool completed = task && task->wait_for(std::chrono::seconds(3));
    Check(completed && finished.load(std::memory_order_acquire),
          "send 超时任务未在 3s 内完成");
    const int error = observed_errno.load(std::memory_order_acquire);
    Check(error == EAGAIN, "SO_SNDTIMEO 超时应返回 EAGAIN（POSIX socket(7)）");
    Check(elapsed_ms.load(std::memory_order_acquire) >= 20,
          "SO_SNDTIMEO 超时应真的阻塞过 30ms 量级");
    const auto counters = go2cpp::hook::error_counters();
    Check(counters.io_timeouts >= 1,
          "io_timeouts 统计应记录至少一次数据面超时");
    std::printf("[hook-p0] send errno=%d（期望 EAGAIN=%d），elapsed=%lldms\n",
                error, EAGAIN,
                static_cast<long long>(elapsed_ms.load()));

    ClosePair(fds);
    manager.Shutdown();
}

// ---------------------------------------------------------------------------
// 场景 3：白名单外 ioctl 透传
// ---------------------------------------------------------------------------
void ScenarioUnknownIoctlPassthrough() {
    std::printf("[hook-p0][2] 白名单外 ioctl 透传\n");
    go2cpp::hook::reset_error_counters();

    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    Check(fd >= 0, "创建 UDP socket 失败");
    if (fd < 0) {
        return;
    }

    // 3a. 内核支持、但不在 hook 白名单内的带指针命令：必须原样成功。
    ifreq request{};
    std::strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);
    errno = 0;
    const int index_result = ::ioctl(fd, SIOCGIFINDEX, &request);
    const int index_errno = errno;
    Check(index_result == 0, "SIOCGIFINDEX 应透传成功");
    Check(request.ifr_ifindex > 0, "SIOCGIFINDEX 应写回 lo 的接口索引");
    Check(go2cpp::hook::error_counters().ioctl_passthroughs >= 1,
          "ioctl_passthroughs 应记录 SIOCGIFINDEX 透传");
    std::printf("[hook-p0] SIOCGIFINDEX rc=%d errno=%d ifindex=%d\n",
                index_result, index_errno, request.ifr_ifindex);

    // 3b. 内核也不认识的命令：错误必须来自内核（ENOTTY 等），不能是
    //     hook 伪造的 ENOTSUP。显式传 nullptr 避免在可变参数里塞整数。
    errno = 0;
    const int unknown_result = ::ioctl(fd, 0xDEADBEEFUL, nullptr);
    const int unknown_errno = errno;
    Check(unknown_result == -1, "未知 ioctl 应返回 -1");
    Check(unknown_errno != ENOTSUP,
          "未知 ioctl 必须透传内核错误，不得伪造 ENOTSUP");
    Check(go2cpp::hook::error_counters().ioctl_passthroughs >= 2,
          "ioctl_passthroughs 应记录未知命令透传");
    std::printf("[hook-p0] 未知 ioctl rc=%d errno=%d（期望非 ENOTSUP=%d）\n",
                unknown_result, unknown_errno, ENOTSUP);

    // 3c. 终端命令：非 tty 时内核返回 ENOTTY，同样必须是透传结果。
    winsize window{};
    errno = 0;
    const int tty_result = ::ioctl(STDIN_FILENO, TIOCGWINSZ, &window);
    const int tty_errno = errno;
    Check(tty_result == 0 || tty_errno != ENOTSUP,
          "TIOCGWINSZ 不得返回 hook 伪造的 ENOTSUP");
    std::printf("[hook-p0] TIOCGWINSZ rc=%d errno=%d（tty=%d）\n",
                tty_result, tty_errno, ::isatty(STDIN_FILENO) ? 1 : 0);

    // 3d. 文件系统命令：ext4/overlayfs 支持则成功，否则内核错误；
    //     两种情况都必须不是 ENOTSUP。
    char path[] = "/tmp/go2cpp_hook_p0_XXXXXX";
    const int file_fd = ::mkstemp(path);
    if (file_fd >= 0) {
        int flags = 0;
        errno = 0;
        const int flags_result = ::ioctl(file_fd, FS_IOC_GETFLAGS, &flags);
        const int flags_errno = errno;
        Check(flags_result == 0 || flags_errno != ENOTSUP,
              "FS_IOC_GETFLAGS 不得返回 hook 伪造的 ENOTSUP");
        std::printf("[hook-p0] FS_IOC_GETFLAGS rc=%d errno=%d flags=0x%x\n",
                    flags_result, flags_errno, flags);
        (void)::close(file_fd);
        (void)::unlink(path);
    } else {
        std::printf("[hook-p0] mkstemp 失败，跳过 FS_IOC_GETFLAGS 场景\n");
    }

    (void)::close(fd);
}

// ---------------------------------------------------------------------------
// 场景 4：EINTR 重试预算
// ---------------------------------------------------------------------------
void ScenarioEintrRetryLimit() {
    std::printf("[hook-p0][3] EINTR 重试限次\n");
    using go2cpp::hook::kEintrRetryLimit;
    using go2cpp::hook::ShouldRetryAfterEintr;

    Check(kEintrRetryLimit == 3, "EINTR 重试上限应为 3");
    Check(ShouldRetryAfterEintr(0), "首次 EINTR 后应允许重试");
    Check(ShouldRetryAfterEintr(1), "第 2 次尝试前应允许重试");
    Check(ShouldRetryAfterEintr(2), "第 3 次尝试前应允许重试");
    Check(!ShouldRetryAfterEintr(3), "3 次重试用尽后不得再重试");
    Check(!ShouldRetryAfterEintr(4), "超限后不得再重试");
    Check(!ShouldRetryAfterEintr(100), "远超上限后不得再重试");
    Check(ShouldRetryAfterEintr(-1), "负值按 0 处理（预算仍是满的）");

    int retries = 0;
    for (; ShouldRetryAfterEintr(retries); ++retries) {
    }
    Check(retries == kEintrRetryLimit, "模拟循环应在恰好 3 次重试后停止");
    std::printf("[hook-p0] kEintrRetryLimit=%d，模拟循环重试 %d 次\n",
                kEintrRetryLimit, retries);
}

}  // namespace

int main() {
    std::printf("=== hook 三条 P0 回归测试 ===\n");
    ScenarioRecvTimeoutIsEagain();
    ScenarioSendTimeoutIsEagain();
    ScenarioUnknownIoctlPassthrough();
    ScenarioEintrRetryLimit();

    if (g_failures == 0) {
        std::printf("\n[hook-p0] failures=0\n");
    } else {
        std::printf("\n[hook-p0] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
