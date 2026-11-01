/**
 * @file test_go_signal.cpp
 * @brief `go2cpp/go/os_signal.hpp` 的兼容性测试（对照 Go `os/signal`）。
 *
 * 覆盖：
 *  1. `Notify(c, SIGINT)` 后由**真实子进程** `kill(2)` 父进程，通道收到值；
 *  2. `Notify` + `Stop`：停止后同一信号不再投递（先重新注册避免默认处置）；
 *  3. `Ignore(SIGUSR1)`：信号被吞掉，进程存活；
 *  4. 两个通道监听同一信号时**都**收到；
 *  5. `Signal::String()` 与 Go `syscall` 名字表一致；
 *  6. `Reset` 之后信号恢复默认处置（子进程验证被 SIGTERM 杀死）；
 *  7. 无缓冲通道 + 连续信号：不阻塞、不崩溃（丢弃语义）；
 *  8. 同一通道多次 `Notify` 是并集（Go 1.16+ 语义）。
 *
 * 说明：信号测试必须在**独立进程**里跑（`ctest` 每个测试一个进程），
 * 且只用 SIGINT/SIGTERM/SIGUSR1/SIGUSR2 这几个可捕获信号。
 */

#include "go2cpp/go/os_signal.hpp"
#include "go_api_check.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

namespace go = go2cpp::go;

/// 在子进程里延迟 `delay_ms` 后向父进程发信号；返回子进程 pid。
pid_t SignalAfterDelay(int number, int delay_ms = 30) {
    const pid_t parent = ::getpid();
    const pid_t child = ::fork();
    if (child == 0) {
        ::usleep(static_cast<useconds_t>(delay_ms) * 1000);
        ::kill(parent, number);
        ::_exit(0);
    }
    return child;
}

/// 等待子进程退出；返回状态（失败返回 -1）。
int ReapChild(pid_t child) {
    int status = 0;
    if (::waitpid(child, &status, 0) != child) {
        return -1;
    }
    return status;
}

/// 轮询接收（不依赖调度器/定时器服务）：最多等 `timeout_ms`。
template <typename T>
bool TryRecvWithin(go::Chan<T>& channel, T* out, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (channel.TryRecv(*out)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return channel.TryRecv(*out);
}

/// 场景 1：SIGINT 端到端（子进程 → 父进程 → channel）。
void TestNotifyInterrupt() {
    GO_ANNOUNCE("signal: Notify(SIGINT) from child process");
    go::Chan<go::Signal> channel = go::Chan<go::Signal>::Make(1);
    go::signal::Notify(channel, {go::Interrupt});
    const pid_t child = SignalAfterDelay(SIGINT);
    GO_REQUIRE(child > 0);
    go::Signal received;
    const bool got = TryRecvWithin(channel, &received);
    GO_CHECK(got);
    if (got) {
        GO_CHECK(received == go::Interrupt);
        GO_CHECK(received.Number() == SIGINT);
        GO_CHECK(received.String() == "interrupt");
    }
    GO_CHECK(ReapChild(child) == 0);
    go::signal::Stop(channel);
}

/// 场景 2：Stop 之后不再投递（先重新注册再发，避免默认处置杀进程）。
void TestStop() {
    GO_ANNOUNCE("signal: Stop");
    go::Chan<go::Signal> first = go::Chan<go::Signal>::Make(1);
    go::signal::Notify(first, {go::Signal(SIGTERM)});
    go::signal::Stop(first);
    // 此处 SIGTERM 已无监听者；立刻重新注册到第二个通道，避免窗口内杀进程。
    go::Chan<go::Signal> second = go::Chan<go::Signal>::Make(1);
    go::signal::Notify(second, {go::Signal(SIGTERM)});
    GO_CHECK(first.Len() == 0);  // 第一个通道不应有残留

    const pid_t child = SignalAfterDelay(SIGTERM);
    go::Signal received;
    GO_CHECK(TryRecvWithin(second, &received));
    GO_CHECK(received.Number() == SIGTERM);
    GO_CHECK(!first.TryRecv(received));
    GO_CHECK(ReapChild(child) == 0);
    go::signal::Stop(second);
    // 收尾：用 SIGTERM 之外的方式复位，避免后续测试被默认处置影响。
    go::signal::Reset(go::Signal(SIGTERM));
}

/// 场景 3：Ignore 吞掉信号（默认处置本应终止进程）。
void TestIgnore() {
    GO_ANNOUNCE("signal: Ignore(SIGUSR1)");
    go::signal::Ignore(go::Signal(SIGUSR1));
    const pid_t child = SignalAfterDelay(SIGUSR1);
    GO_CHECK(ReapChild(child) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // 能走到这里说明忽略生效（SIGUSR1 的默认处置是终止进程）。
    GO_CHECK(true);
    go::signal::Reset(go::Signal(SIGUSR1));
    // 复位后 SIGUSR1 又变成默认终止；后续测试不再发送它。
}

/// 场景 4：两个通道都收到同一信号。
void TestMultipleChannels() {
    GO_ANNOUNCE("signal: two channels both receive");
    go::Chan<go::Signal> left = go::Chan<go::Signal>::Make(1);
    go::Chan<go::Signal> right = go::Chan<go::Signal>::Make(1);
    go::signal::Notify(left, {go::Interrupt});
    go::signal::Notify(right, {go::Interrupt});
    const pid_t child = SignalAfterDelay(SIGINT);
    go::Signal first;
    go::Signal second;
    const bool got_left = TryRecvWithin(left, &first);
    const bool got_right = TryRecvWithin(right, &second);
    GO_CHECK(got_left);
    GO_CHECK(got_right);
    GO_CHECK(first == go::Interrupt);
    GO_CHECK(second == go::Interrupt);
    GO_CHECK(ReapChild(child) == 0);
    go::signal::Stop(left);
    go::signal::Stop(right);
    go::signal::Reset(go::Interrupt);
}

/// 场景 5：Signal::String() 名字表。
void TestSignalString() {
    GO_ANNOUNCE("signal: Signal::String names");
    GO_CHECK(go::Signal(SIGHUP).String() == "hangup");
    GO_CHECK(go::Signal(SIGINT).String() == "interrupt");
    GO_CHECK(go::Signal(SIGQUIT).String() == "quit");
    GO_CHECK(go::Signal(SIGKILL).String() == "killed");
    GO_CHECK(go::Signal(SIGSEGV).String() == "segmentation fault");
    GO_CHECK(go::Signal(SIGPIPE).String() == "broken pipe");
    GO_CHECK(go::Signal(SIGALRM).String() == "alarm clock");
    GO_CHECK(go::Signal(SIGTERM).String() == "terminated");
    GO_CHECK(go::Signal(SIGCHLD).String() == "child exited");
    GO_CHECK(go::Signal(1234).String() == "signal 1234");
    GO_CHECK(!go::Signal().Valid());
    GO_CHECK(go::Interrupt.Valid());
}

/// 场景 6：Stop/Reset 之后恢复默认处置（子进程里验证被 SIGTERM 杀死）。
void TestDefaultDispositionRestored() {
    GO_ANNOUNCE("signal: Stop restores default disposition");
    go::Chan<go::Signal> channel = go::Chan<go::Signal>::Make(1);
    go::signal::Notify(channel, {go::Signal(SIGTERM)});
    go::signal::Stop(channel);  // 无监听者 → SIG_DFL

    const pid_t child = ::fork();
    if (child == 0) {
        // 父进程已完成注册/注销；子进程只 raise，验证默认处置是否恢复。
        ::raise(SIGTERM);
        ::_exit(0);
    }
    GO_REQUIRE(child > 0);
    const int status = ReapChild(child);
    GO_CHECK(WIFSIGNALED(status));
    GO_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    go::signal::Reset(go::Signal(SIGTERM));
}

/// 场景 7：无缓冲通道 + 连续信号不阻塞（丢弃语义）。
void TestUnbufferedChannel() {
    GO_ANNOUNCE("signal: unbuffered channel drops instead of blocking");
    go::Chan<go::Signal> channel = go::Chan<go::Signal>::Make(0);
    go::signal::Notify(channel, {go::Signal(SIGUSR2)});
    for (int round = 0; round < 100; ++round) {
        ::raise(SIGUSR2);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    GO_CHECK(true);  // 没死、没挂住
    // 至少能取到一个（handler 写 pipe 是异步的，取不到也合法，因此只验证不阻塞）。
    go::Signal received;
    (void)channel.TryRecv(received);
    go::signal::Stop(channel);
    go::signal::Reset(go::Signal(SIGUSR2));
}

/// 场景 8：同一通道多次 Notify 是并集。
void TestNotifyUnion() {
    GO_ANNOUNCE("signal: repeated Notify is a union");
    go::Chan<go::Signal> channel = go::Chan<go::Signal>::Make(2);
    go::signal::Notify(channel, {go::Interrupt});
    go::signal::Notify(channel, {go::Signal(SIGUSR1)});
    const pid_t first = SignalAfterDelay(SIGINT);
    go::Signal received;
    GO_CHECK(TryRecvWithin(channel, &received));
    GO_CHECK(received == go::Interrupt);
    GO_CHECK(ReapChild(first) == 0);
    const pid_t second = SignalAfterDelay(SIGUSR1);
    GO_CHECK(TryRecvWithin(channel, &received));
    GO_CHECK(received.Number() == SIGUSR1);
    GO_CHECK(ReapChild(second) == 0);
    go::signal::Stop(channel);
    go::signal::Reset(go::Interrupt);
    go::signal::Reset(go::Signal(SIGUSR1));
}

}  // namespace

int main() {
    if (!go::signal::Supported()) {
        std::fprintf(stderr, "[go_api] os/signal not supported on this platform\n");
        return 0;
    }
    TestSignalString();
    TestNotifyInterrupt();
    TestStop();
    TestIgnore();
    TestMultipleChannels();
    TestDefaultDispositionRestored();
    TestUnbufferedChannel();
    TestNotifyUnion();
    return go_api_test::ExitCode();
}
