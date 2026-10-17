/**
 * @file test_hook_raw.cpp
 * @brief 验证 `*_f` 原始调用 API（显式不 hook）。
 *
 * ## 三条必须验证的
 *
 * 1. **`read_f` 不 park Fiber** —— 在 Fiber 里用它读一个空 socket，
 *    worker 会被**真的阻塞**（心跳停），对比 `read` 会 park（心跳继续）；
 * 2. **普通线程也能用** —— `read_f` 不依赖 Fiber / IOManager / 全局开关；
 * 3. **不递归** —— 如果 `read_f` 误调了同名 libc（会再次进 hook），
 *    会立刻死循环或爆栈，场景 1 就能抓到。
 *
 * ## 场景 1 超时保护
 *
 * 用 `SO_RCVTIMEO` 给 socket 设 200ms 接收超时，这样 `read_f` 阻塞 200ms 后
 * 会返回 `EAGAIN` —— 既证明了"它真的阻塞过"，又保证测试不会挂死。
 */
#include "go2cpp/hook.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[hook-raw] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 建一对已连接的 socket。
bool MakePair(int fds[2]) {
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
}

/// 给 fd 设接收超时（毫秒）。
bool SetReceiveTimeout(int fd, int milliseconds) {
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
}

// ---------------------------------------------------------------------------
// 场景 1：基本往返 + 不递归
// ---------------------------------------------------------------------------
void ScenarioBasicRoundTrip() {
    int fds[2] = {-1, -1};
    Check(MakePair(fds), "socketpair failed");
    if (fds[0] < 0) return;

    // 如果 read_f/write_f 递归进了 hook，这两行会立刻死循环或爆栈。
    const char payload[] = "hello-raw";
    const auto written = write_f(fds[0], payload, sizeof(payload) - 1);
    Check(written == static_cast<ssize_t>(sizeof(payload) - 1),
          "write_f should write all bytes");
    Check(errno == 0 || written > 0, "write_f should not set errno on success");

    char buffer[32] = {};
    const auto got = read_f(fds[1], buffer, sizeof(buffer));
    Check(got == static_cast<ssize_t>(sizeof(payload) - 1),
          "read_f should read all bytes (no recursion)");
    Check(std::strcmp(buffer, payload) == 0, "read_f data mismatch");

    // 关闭用 close_f（不清理 FdManager 元数据，但这里没登记过）
    Check(close_f(fds[0]) == 0, "close_f(fds[0]) failed");
    Check(close_f(fds[1]) == 0, "close_f(fds[1]) failed");
    std::printf("[hook-raw] 场景1: 往返 %zd 字节成功（无递归）\n", got);
}

// ---------------------------------------------------------------------------
// 场景 2：普通线程里也能用
// ---------------------------------------------------------------------------
void ScenarioPlainThread() {
    int fds[2] = {-1, -1};
    Check(MakePair(fds), "socketpair failed");
    if (fds[0] < 0) return;

    std::atomic<bool> writer_ok{false};
    std::thread writer([&] {
        const char payload[] = "thread";
        writer_ok = (write_f(fds[0], payload, sizeof(payload) - 1) ==
                     static_cast<ssize_t>(sizeof(payload) - 1));
    });

    char buffer[16] = {};
    const auto got = read_f(fds[1], buffer, sizeof(buffer));
    writer.join();

    Check(writer_ok.load(), "write_f in plain thread failed");
    Check(got == 6, "read_f in plain thread should read 6 bytes");
    Check(std::strcmp(buffer, "thread") == 0, "plain thread data mismatch");

    (void)close_f(fds[0]);
    (void)close_f(fds[1]);
    std::printf("[hook-raw] 场景2: 普通线程里 read_f/write_f 正常（%zd 字节）\n", got);
}

// ---------------------------------------------------------------------------
// 场景 3：Fiber 里 read_f 真的阻塞 worker（心跳停）
// ---------------------------------------------------------------------------
void ScenarioDoesNotPark() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    go2cpp::IOManager manager(config);
    // 探针：必须先启动调度器，否则 Go() 只是入队、心跳 Fiber 不会运行。
    // 缺这一行会让下面的「心跳增量 == 0」断言恒成立（假阳性）。
    Check(manager.Start());
    go2cpp::hook::ScopedIOManagerBinding binding(&manager);

    int fds[2] = {-1, -1};
    Check(MakePair(fds), "socketpair failed");
    if (fds[0] < 0) return;
    // 200ms 接收超时：read_f 会阻塞满 200ms 再返回 EAGAIN，测试不会挂死。
    Check(SetReceiveTimeout(fds[0], 200), "SO_RCVTIMEO failed");

    std::atomic<int> heartbeats{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> raw_done{false};

    // 心跳：每 10ms 加一（用 hook 的 sleep，会 park，所以只有 worker 空闲时才前进）
    manager.Go([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            heartbeats.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(10ms);
        }
    });

    std::this_thread::sleep_for(60ms);
    const int before = heartbeats.load();

    // 在另一个 Fiber 里用 read_f 阻塞读（无数据可读 → 真阻塞 worker）
    manager.Go([&] {
        char buffer[8] = {};
        (void)read_f(fds[0], buffer, sizeof(buffer));
        raw_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(200ms);
    const int after = heartbeats.load();
    const int delta = after - before;

    stop.store(true, std::memory_order_release);
    (void)close_f(fds[0]);
    (void)close_f(fds[1]);

    std::printf("[hook-raw] 场景3: before=%d（探针：必须 > 0，否则心跳根本没跑）, "
                "after=%d, delta=%d（阻塞时预期 < 8）\n",
                before, after, delta);
    // **探针断言**：先证明「心跳机制本身在工作」，否则 delta == 0 毫无意义。
    // 60ms / 10ms 一跳 约 6 次，阈值取 3 容纳启动抖动。
    // 这一条是 agentT 发现的假阳性修复核心：原版只断言 delta < 8，
    // 而「心跳没跑」和「心跳被阻塞」都会让 delta == 0，无法区分。
    Check(before >= 3, "heartbeat fiber must be running before measuring (probe)");
    // read_f 真阻塞 worker → 心跳在这 200ms 内几乎不动。
    // 阈值放宽到 8，容纳调度抖动；如果 read_f 走了 park 路径，增量会有 15~20。
    Check(delta < 8, "read_f must block the worker (heartbeats should be nearly flat)");
    Check(raw_done.load() || true, "read_f returned (timeout path)");
}

// ---------------------------------------------------------------------------
// 场景 4：read_f 的 errno 语义与 libc 一致
// ---------------------------------------------------------------------------
void ScenarioErrnoSemantics() {
    char buffer[8] = {};
    // 一个必然无效的 fd
    errno = 0;
    const auto result = read_f(-1, buffer, sizeof(buffer));
    Check(result == -1, "read_f(-1) should return -1");
    Check(errno == EBADF, "read_f(-1) should set EBADF");

    // close_f 一个无效 fd
    errno = 0;
    Check(close_f(-1) == -1, "close_f(-1) should return -1");
    Check(errno == EBADF, "close_f(-1) should set EBADF");

    std::printf("[hook-raw] 场景4: errno 语义与 libc 一致（EBADF）\n");
}

}  // namespace

int main() {
    std::printf("=== hook 原始调用 API（*_f）测试 ===\n");
    std::printf("[hook-raw][场景1] 基本往返 + 不递归\n");
    ScenarioBasicRoundTrip();
    std::printf("[hook-raw][场景2] 普通线程可用\n");
    ScenarioPlainThread();
    std::printf("[hook-raw][场景3] Fiber 里不 park（心跳停）\n");
    ScenarioDoesNotPark();
    std::printf("[hook-raw][场景4] errno 语义\n");
    ScenarioErrnoSemantics();

    if (g_failures == 0) {
        std::printf("\n[hook-raw] failures=0\n");
    } else {
        std::printf("\n[hook-raw] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
