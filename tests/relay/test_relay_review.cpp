/**
 * @file test_relay_review.cpp
 * @brief relay 模块审查回归：正常转发、半关闭、RST、取消在途、direct 对比、
 *        splice 正常/取消。
 *
 * @details 本文件是 2026-11-04 relay 审查的复现用例，独立 main，不并入
 *          go2cpp_tests；用 g++ 直接编译（见文件末尾注释）。
 */

#include "relay_test_support.hpp"

#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/relay/splice_pipe.hpp"

#include <cerrno>
#include <fcntl.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <utility>

#if defined(__linux__)
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace go2cpp_relay_test;

using go2cpp::Context;
using go2cpp::ContextPtr;
using go2cpp::ErrorPtr;
using go2cpp::NewError;
using go2cpp::relay::PipeConfig;
using go2cpp::relay::RelayPipe;
using go2cpp::relay::SplicePipe;

/// 读操作立即返回 Connection reset 的流；写正常。
class FailingStream final : public go2cpp::relay::Stream {
public:
    explicit FailingStream(ErrorPtr read_error) : m_read_error(std::move(read_error)) {}

    go2cpp::streams::ReadResult Read(char*, std::size_t) override {
        return {0, m_read_error};
    }
    go2cpp::streams::WriteResult Write(const char* data, std::size_t size) override {
        (void)data;
        m_written += size;
        return {size, {}};
    }
    ErrorPtr Close() override {
        m_closed = true;
        return {};
    }
    std::uint64_t written() const { return m_written; }
    bool closed() const { return m_closed; }

private:
    ErrorPtr m_read_error;
    std::uint64_t m_written{0};
    bool m_closed{false};
};

/// 1. 正常双向转发（普通 / direct 两种模式各跑一遍）。
void RunBidirectional(bool direct) {
    StreamOptions a_opts;
    a_opts.seed = 1;
    a_opts.inbound_bytes = 256U * 1024U;
    StreamOptions b_opts;
    b_opts.seed = 2;
    b_opts.inbound_bytes = 256U * 1024U;
    TestStream a(a_opts);
    TestStream b(b_opts);
    PipeConfig cfg;
    cfg.direct = direct;
    RelayPipe pipe(&a, &b, cfg);
    AsyncRun run(&pipe, ContextPtr{});
    run.Start();
    run.JoinOrAbort(std::chrono::milliseconds(5000), "bidirectional", __FILE__,
                    __LINE__);
    RELAY_CHECK(run.ok());
    const auto stats = pipe.Stats();
    RELAY_CHECK_U64(stats.a_to_b, 256U * 1024U);
    RELAY_CHECK_U64(stats.b_to_a, 256U * 1024U);
    RELAY_CHECK_U64(stats.dropped, 0);
    RELAY_CHECK_U64(stats.timeouts, 0);
    RELAY_CHECK_U64(b.written(), 256U * 1024U);
    RELAY_CHECK_U64(a.written(), 256U * 1024U);
    RELAY_CHECK(!a.closed());
    RELAY_CHECK(!b.closed());
    std::printf("  bidirectional direct=%d ok\n", direct ? 1 : 0);
}

/// 2. 对端半关闭：A 侧 EOF 后，B -> A 方向仍必须能继续写。
void TestHalfClose() {
    StreamOptions a_opts;
    a_opts.seed = 3;
    a_opts.inbound_bytes = 128U * 1024U;
    a_opts.inbound_eof = true;
    StreamOptions b_opts;
    b_opts.seed = 4;
    b_opts.inbound_bytes = 32U * 1024U;
    b_opts.inbound_eof = false;
    b_opts.wait_for_input = true;
    TestStream a(a_opts);
    TestStream b(b_opts);
    RelayPipe pipe(&a, &b, PipeConfig{});
    const auto ctx = Context::WithCancel(Context::Background());
    AsyncRun run(&pipe, ctx.first);
    run.Start();
    RELAY_CHECK(WaitUntil(
        [&] { return b.written() == 128U * 1024U && a.written() == 32U * 1024U; },
        std::chrono::milliseconds(5000)));
    RELAY_CHECK(!a.closed());
    RELAY_CHECK(!b.closed());
    ctx.second();
    run.JoinOrAbort(std::chrono::milliseconds(3000), "half close", __FILE__, __LINE__);
    RELAY_CHECK(!run.ok());
    std::printf("  half close ok (A->B finished, B->A still delivered)\n");
}

/// 3. 对端 RST：读失败不关闭目的端，Run 返回 false 且错误非空。
void TestPeerReset() {
    FailingStream a(NewError("connection reset by peer"));
    StreamOptions b_opts;
    b_opts.seed = 5;
    b_opts.inbound_bytes = 8U * 1024U;
    TestStream b(b_opts);
    RelayPipe pipe(&a, &b, PipeConfig{});
    AsyncRun run(&pipe, ContextPtr{});
    run.Start();
    run.JoinOrAbort(std::chrono::milliseconds(3000), "peer reset", __FILE__, __LINE__);
    RELAY_CHECK(!run.ok());
    RELAY_CHECK(run.error() != nullptr);
    RELAY_CHECK_U64(a.written(), 8U * 1024U);
    RELAY_CHECK(!b.closed());
    RELAY_CHECK(!a.closed());
    std::printf("  peer reset ok (read error propagated, destination kept open)\n");
}

/// 4. Stop 时有在途数据：取消后队列内数据计入 dropped，线程全部回收。
void TestStopWithInFlight() {
    StreamOptions a_opts;
    a_opts.seed = 6;
    a_opts.inbound_bytes = 1U * 1024U * 1024U;
    StreamOptions b_opts;
    b_opts.block_writes = true;
    TestStream a(a_opts);
    TestStream b(b_opts);
    PipeConfig cfg;
    cfg.copy_buf = 16U * 1024U;
    cfg.queue_cap = 64U * 1024U;
    RelayPipe pipe(&a, &b, cfg);
    const auto ctx = Context::WithCancel(Context::Background());
    AsyncRun run(&pipe, ctx.first);
    run.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ctx.second();
    run.JoinOrAbort(std::chrono::milliseconds(3000), "stop in flight", __FILE__,
                    __LINE__);
    RELAY_CHECK(!run.ok());
    RELAY_CHECK_U64(pipe.Stats().a_to_b, 0);
    RELAY_CHECK(pipe.Stats().dropped > 0);
    RELAY_CHECK(a.closed());
    RELAY_CHECK(b.closed());
    std::printf("  stop in flight ok (dropped=%llu)\n",
                static_cast<unsigned long long>(pipe.Stats().dropped));
}

/// 5. direct 与队列模式的写超时行为对比：都只关目的端、只记一次 timeout。
void RunWriteTimeout(bool direct) {
    StreamOptions a_opts;
    a_opts.seed = 7;
    a_opts.inbound_bytes = 1U * 1024U * 1024U;
    StreamOptions b_opts;
    b_opts.block_writes = true;
    TestStream a(a_opts);
    TestStream b(b_opts);
    PipeConfig cfg;
    cfg.direct = direct;
    cfg.write_timeout = std::chrono::milliseconds(100);
    RelayPipe pipe(&a, &b, cfg);
    AsyncRun run(&pipe, ContextPtr{});
    run.Start();
    RELAY_CHECK(WaitUntil([&] { return b.closed(); },
                          std::chrono::milliseconds(3000)));
    run.JoinOrAbort(std::chrono::milliseconds(3000), "write timeout", __FILE__,
                    __LINE__);
    RELAY_CHECK(!run.ok());
    RELAY_CHECK_U64(pipe.Stats().timeouts, 1);
    RELAY_CHECK(b.closed());
    std::printf("  write timeout direct=%d ok (timeouts=1)\n", direct ? 1 : 0);
}

#if defined(__linux__)

/// 后台跑 SplicePipe::Run，主线程观察结果。
struct SpliceRun {
    SplicePipe* pipe{nullptr};
    ContextPtr ctx;
    std::thread thread;
    std::atomic<bool> done{false};
    bool ok{false};
    ErrorPtr error;

    void Start() {
        thread = std::thread([this] {
            ok = pipe->Run(ctx, &error);
            done.store(true, std::memory_order_release);
        });
    }
    bool Join(std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (!done.load(std::memory_order_acquire)) {
            if (Clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        thread.join();
        return true;
    }
};

/// 把 fd 写满指定字节（假定 socket 缓冲足够）。
bool WriteAll(int fd, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t n = ::write(fd, data + offset, size - offset);
        if (n <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

/// 从 fd 读满指定字节。
bool ReadAll(int fd, char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t n = ::read(fd, data + offset, size - offset);
        if (n <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

/// 6. splice 正常双向转发 + 双向半关闭（FIN 传播）。
///
/// 拓扑：localA <-> proxyA [SplicePipe] proxyB <-> localB。
/// 必须用两个 socketpair：若把一对 socketpair 的两端都交给 SplicePipe，
/// 测试端会与中继线程抢读同一个 fd，统计与数据都不可信。
void TestSpliceForward() {
    int a_pair[2] = {-1, -1};
    int b_pair[2] = {-1, -1};
    RELAY_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, a_pair) == 0);
    RELAY_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, b_pair) == 0);
    constexpr std::size_t kBytes = 64U * 1024U;
    SplicePipe pipe(a_pair[1], b_pair[1]);
    SpliceRun run;
    run.pipe = &pipe;
    run.ctx = ContextPtr{};
    run.Start();

    std::vector<char> out(kBytes, 0x5A);
    std::vector<char> in(kBytes, 0);
    RELAY_CHECK(WriteAll(a_pair[0], out.data(), kBytes));
    RELAY_CHECK(ReadAll(b_pair[0], in.data(), kBytes));
    RELAY_CHECK(in == out);
    std::vector<char> back(kBytes, 0x33);
    std::vector<char> in_back(kBytes, 0);
    RELAY_CHECK(WriteAll(b_pair[0], back.data(), kBytes));
    RELAY_CHECK(ReadAll(a_pair[0], in_back.data(), kBytes));
    RELAY_CHECK(in_back == back);

    RELAY_CHECK(::shutdown(a_pair[0], SHUT_WR) == 0);
    RELAY_CHECK(::shutdown(b_pair[0], SHUT_WR) == 0);
    RELAY_CHECK(run.Join(std::chrono::milliseconds(3000)));
    if (run.error) {
        std::fprintf(stderr, "  splice error: %s\n",
                     go2cpp::ErrorMessage(run.error).c_str());
    }
    RELAY_CHECK(run.ok);
    RELAY_CHECK_U64(pipe.Stats().a_to_b, kBytes);
    RELAY_CHECK_U64(pipe.Stats().b_to_a, kBytes);
    RELAY_CHECK(::close(a_pair[0]) == 0);
    RELAY_CHECK(::close(a_pair[1]) == 0);
    RELAY_CHECK(::close(b_pair[0]) == 0);
    RELAY_CHECK(::close(b_pair[1]) == 0);
    std::printf("  splice forward ok (FIN propagated both ways)\n");
}

/// 7. splice 取消：空闲连接取消后 Run 返回、线程回收、fd 不被关闭。
void TestSpliceCancel() {
    int fds[2] = {-1, -1};
    RELAY_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    SplicePipe pipe(fds[0], fds[1]);
    const auto ctx = Context::WithCancel(Context::Background());
    SpliceRun run;
    run.pipe = &pipe;
    run.ctx = ctx.first;
    run.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ctx.second();
    RELAY_CHECK(run.Join(std::chrono::milliseconds(3000)));
    RELAY_CHECK(!run.ok);
    RELAY_CHECK(::fcntl(fds[0], F_GETFD) >= 0);
    RELAY_CHECK(::fcntl(fds[1], F_GETFD) >= 0);
    RELAY_CHECK(::close(fds[0]) == 0);
    RELAY_CHECK(::close(fds[1]) == 0);
    std::printf("  splice cancel ok (threads joined, fds owned by caller)\n");
}

#endif

}  // namespace

int main() {
    std::printf("[review] relay 模块审查回归\n");
    RunBidirectional(false);
    RunBidirectional(true);
    TestHalfClose();
    TestPeerReset();
    TestStopWithInFlight();
    RunWriteTimeout(false);
    RunWriteTimeout(true);
#if defined(__linux__)
    TestSpliceForward();
    TestSpliceCancel();
#endif
    if (Failures().load() != 0) {
        std::fprintf(stderr, "[review] %d failure(s)\n", Failures().load());
        return 1;
    }
    std::printf("[review] all passed\n");
    return 0;
}
