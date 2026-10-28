/**
 * @file test_relay.cpp
 * @brief Relay 真实场景测试：100KB 双向、请求-响应半关闭、慢客户端、有界缓冲背压、
 *        空闲/连接超时、随机分块、写错误与小缓冲边界。
 * @details 场景与验证目标：
 *          - 真实 TCP 双向 100KB + 吞吐：验证字节原样、全双工、统计准确；
 *          - 请求-响应半关闭：客户端 FIN 后仍必须把服务端响应完整搬回（若中继
 *            读到 EOF 就 Close，这个用例会失败）；
 *          - 慢客户端：目标端写永久超时必须断开、返回 kSlowConsumer，且读入量
 *            不超过一方缓冲（不 OOM）；
 *          - 有界缓冲：目标端每次只收 4KB，验证 pending 峰值 ≤ buffer_bytes；
 *          - 空闲/连接超时：无流量、拨号失败两类超时都必须有界退出；
 *          - 随机分块：每次 Write 上限随机，验证不丢字节、不越界；
 *          - 写错误与极小缓冲：异常路径与边界值。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <sys/resource.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/relay.hpp"
#include "go2cpp/bento/net/tcp_transport.hpp"
#include "test_util.hpp"

namespace {

using Clock = std::chrono::steady_clock;

using go2cpp::bento::net::Code;
using go2cpp::bento::net::Endpoint;
using go2cpp::bento::net::ITransport;
using go2cpp::bento::net::Relay;
using go2cpp::bento::net::RelayCloseReason;
using go2cpp::bento::net::RelayOptions;
using go2cpp::bento::net::RelayResult;
using go2cpp::bento::net::Result;
using go2cpp::bento::net::TcpTransport;

constexpr std::size_t kBufferBytes = 64 * 1024;

std::chrono::milliseconds Ms(std::uint32_t value) {
    return std::chrono::milliseconds(static_cast<std::int64_t>(value));
}

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
}

/// 与 test_tcp_proxy 相同的确定性字节图案，便于在失败时定位首个差异。
std::uint8_t PatternByte(std::uint32_t seed, std::size_t index) {
    return static_cast<std::uint8_t>((seed * 131U + index * 17U + (index >> 8U) * 7U) & 0xFFU);
}

std::vector<std::uint8_t> MakePattern(std::uint32_t seed, std::size_t bytes) {
    std::vector<std::uint8_t> data(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        data[i] = PatternByte(seed, i);
    }
    return data;
}

bool SameBytes(const std::vector<std::uint8_t>& actual,
               const std::vector<std::uint8_t>& expected) {
    if (actual.size() != expected.size()) {
        std::fprintf(stderr, "[FAIL] size mismatch: actual=%zu expected=%zu\n", actual.size(),
                     expected.size());
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != expected[i]) {
            std::fprintf(stderr, "[FAIL] byte %zu: actual=%u expected=%u\n", i,
                         static_cast<unsigned>(actual[i]), static_cast<unsigned>(expected[i]));
            return false;
        }
    }
    return true;
}

/// 当前进程常驻内存（KB）；读取失败返回 -1，让断言不会因此误报。
long CurrentRssKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            long kb = -1;
            if (std::sscanf(line.c_str(), "VmRSS: %ld kB", &kb) == 1) {
                return kb;
            }
            return -1;
        }
    }
    return -1;
}

bool WriteFull(ITransport& transport, const std::vector<std::uint8_t>& data) {
    const Result result = transport.WriteFull(data.data(), data.size(), Ms(5000));
    return result.code == Code::kOk && result.bytes == data.size();
}

bool ReadFull(ITransport& transport, std::vector<std::uint8_t>* out) {
    const Result result = transport.ReadFull(out->data(), out->size(), Ms(10000));
    return result.code == Code::kOk && result.bytes == out->size();
}

/// 读到 EOF 为止（最多 limit 字节）。返回是否在 deadline 内看到 EOF。
bool ReadUntilEof(ITransport& transport, std::vector<std::uint8_t>* out, std::size_t limit,
                  std::uint32_t timeout_ms) {
    const std::uint64_t deadline = NowMs() + timeout_ms;
    std::uint8_t scratch[16 * 1024];
    while (NowMs() < deadline) {
        const Result result = transport.Read(scratch, sizeof(scratch), Ms(100));
        if (result.bytes != 0) {
            if (out->size() + result.bytes > limit) {
                return false;
            }
            out->insert(out->end(), scratch, scratch + result.bytes);
        }
        if (result.code == Code::kClosed) {
            return true;
        }
        if (result.code == Code::kError) {
            return false;
        }
    }
    return false;
}

/// 可编排的内存传输：模拟源、慢客户端与坏客户端。所有 Write 只接受配置允许的部分。
class ScriptedTransport final : public ITransport {
public:
    explicit ScriptedTransport(std::vector<std::uint8_t> input = {})
        : m_input(std::move(input)) {}

    // ---- 行为配置 ----
    bool eof_after_input = true;     ///< 输入耗尽后 Read 返回 kClosed（否则超时）。
    bool block_writes = false;       ///< Write 永远超时（模拟发送缓冲长期满）。
    std::size_t max_write_per_call = 0;  ///< 单次 Write 接受上限；0 = 全接受。
    std::function<std::size_t()> write_limit;  ///< 每次 Write 的随机上限（优先）。
    std::uint64_t fail_writes_after = std::numeric_limits<std::uint64_t>::max();
    bool connect_timeout = false;
    bool connect_error = false;

    // ---- 观测 ----
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t shutdown_write_calls = 0;
    std::uint64_t close_calls = 0;
    bool closed = false;
    std::vector<std::uint8_t> received;

    Result Connect(const Endpoint&, std::chrono::milliseconds) override {
        if (connect_timeout) {
            return Result{Code::kTimeout, ETIMEDOUT, 0};
        }
        if (connect_error) {
            return Result{Code::kError, ECONNREFUSED, 0};
        }
        return Result{Code::kOk, 0, 0};
    }

    Result Read(std::uint8_t* buffer, std::size_t capacity,
                std::chrono::milliseconds timeout) override {
        if (m_closed) {
            return Result{Code::kClosed, ENOTCONN, 0};
        }
        if (capacity == 0) {
            return Result{Code::kOk, 0, 0};
        }
        if (m_pos < m_input.size()) {
            const std::size_t count = std::min(capacity, m_input.size() - m_pos);
            std::memcpy(buffer, m_input.data() + m_pos, count);
            m_pos += count;
            read_bytes += count;
            return Result{Code::kOk, 0, count};
        }
        if (eof_after_input) {
            return Result{Code::kClosed, 0, 0};
        }
        SleepFor(timeout);
        return Result{Code::kTimeout, ETIMEDOUT, 0};
    }

    Result Write(const std::uint8_t* data, std::size_t size,
                 std::chrono::milliseconds timeout) override {
        if (m_closed || m_write_shutdown) {
            return Result{Code::kClosed, EPIPE, 0};
        }
        if (size == 0) {
            return Result{Code::kOk, 0, 0};
        }
        if (block_writes) {
            SleepFor(timeout);
            return Result{Code::kTimeout, ETIMEDOUT, 0};
        }
        if (write_bytes >= fail_writes_after) {
            return Result{Code::kClosed, EPIPE, 0};
        }
        std::size_t count = size;
        if (max_write_per_call != 0) {
            count = std::min(count, max_write_per_call);
        }
        if (write_limit) {
            count = std::min(count, write_limit());
        }
        count = std::min<std::uint64_t>(count, fail_writes_after - write_bytes);
        if (count == 0) {
            return Result{Code::kClosed, EPIPE, 0};
        }
        received.insert(received.end(), data, data + count);
        write_bytes += count;
        return Result{Code::kOk, 0, count};
    }

    Result ShutdownWrite() override {
        ++shutdown_write_calls;
        m_write_shutdown = true;
        return Result{Code::kOk, 0, 0};
    }

    Result Close() override {
        ++close_calls;
        closed = true;
        m_closed = true;
        return Result{Code::kOk, 0, 0};
    }

private:
    static void SleepFor(std::chrono::milliseconds timeout) {
        if (timeout.count() > 0) {
            std::this_thread::sleep_for(timeout);
        }
    }

    std::vector<std::uint8_t> m_input;
    std::size_t m_pos = 0;
    bool m_closed = false;
    bool m_write_shutdown = false;
};

/// 一条真实 loopback TCP 连接的两端：dialer 发起，acceptor 由 listen 端接受。
struct TcpPair {
    std::unique_ptr<TcpTransport> dialer;
    std::unique_ptr<TcpTransport> acceptor;
};

bool MakeTcpPair(TcpPair* pair) {
    TcpTransport listener;
    if (listener.Listen(Endpoint{"127.0.0.1", 0}).code != Code::kOk) {
        return false;
    }
    Endpoint local;
    if (listener.LocalEndpoint(&local).code != Code::kOk) {
        return false;
    }
    auto dialer = std::make_unique<TcpTransport>();
    if (dialer->Connect(local, Ms(1000)).code != Code::kOk) {
        return false;
    }
    auto acceptor = std::make_unique<TcpTransport>();
    if (listener.Accept(acceptor.get(), Ms(1000)).code != Code::kOk) {
        return false;
    }
    pair->dialer = std::move(dialer);
    pair->acceptor = std::move(acceptor);
    return true;
}

RelayOptions MakeTcpOptions() {
    RelayOptions options;
    options.buffer_bytes = kBufferBytes;
    options.read_chunk_bytes = 16 * 1024;
    options.read_slice_ms = 0;
    options.write_slice_ms = 10;
    options.poll_interval_ms = 1;
    options.write_timeout_ms = 3000;
    options.idle_timeout_ms = 10000;
    return options;
}

/// 场景 1：真实 TCP 上 100KB 双向同时传输，验证完整性与统计。
void TestTcpBothDirections() {
    std::printf("[CASE] tcp 100KB both directions\n");
    TcpPair client_side;
    TcpPair server_side;
    if (!MakeTcpPair(&client_side) || !MakeTcpPair(&server_side)) {
        bento_test::Skip("loopback TCP unavailable");
        return;
    }

    constexpr std::size_t kBytes = 100 * 1024;
    const std::vector<std::uint8_t> request = MakePattern(11, kBytes);
    const std::vector<std::uint8_t> response = MakePattern(22, kBytes);
    std::vector<std::uint8_t> server_got(kBytes);
    std::vector<std::uint8_t> client_got(kBytes);

    bool client_write = false;
    bool client_read = false;
    bool server_write = false;
    bool server_read = false;
    RelayResult relay_result;

    std::thread relay_thread([&] {
        Relay relay;
        relay_result = relay.Run(*client_side.acceptor, *server_side.dialer, MakeTcpOptions());
    });
    std::thread client_writer([&] { client_write = WriteFull(*client_side.dialer, request); });
    std::thread client_reader([&] { client_read = ReadFull(*client_side.dialer, &client_got); });
    std::thread server_writer([&] { server_write = WriteFull(*server_side.acceptor, response); });
    std::thread server_reader([&] { server_read = ReadFull(*server_side.acceptor, &server_got); });

    client_writer.join();
    client_reader.join();
    server_writer.join();
    server_reader.join();

    // 让中继看到两个方向的 EOF，才能正常完成（否则它会等空闲超时）。
    (void)client_side.dialer->ShutdownWrite();
    (void)server_side.acceptor->ShutdownWrite();
    relay_thread.join();

    (void)client_side.dialer->Close();
    (void)server_side.acceptor->Close();

    BENTO_CHECK(client_write);
    BENTO_CHECK(client_read);
    BENTO_CHECK(server_write);
    BENTO_CHECK(server_read);
    BENTO_CHECK(SameBytes(server_got, request));
    BENTO_CHECK(SameBytes(client_got, response));
    BENTO_CHECK(relay_result.completed());
    BENTO_CHECK_EQ(relay_result.stats.bytes_src_to_dst, kBytes);
    BENTO_CHECK_EQ(relay_result.stats.bytes_dst_to_src, kBytes);
    BENTO_CHECK(relay_result.stats.peak_pending_src_to_dst <= kBufferBytes);
    BENTO_CHECK(relay_result.stats.peak_pending_dst_to_src <= kBufferBytes);
    std::printf("[INFO] both directions: elapsed=%llums reason=%s\n",
                static_cast<unsigned long long>(relay_result.elapsed_ms),
                go2cpp::bento::net::RelayCloseReasonName(relay_result.reason));
}

/// 场景 2：请求-响应 + 半关闭。客户端 FIN 后中继必须继续把响应搬回来。
void TestTcpHalfClose() {
    std::printf("[CASE] tcp request/response half close\n");
    TcpPair client_side;
    TcpPair server_side;
    if (!MakeTcpPair(&client_side) || !MakeTcpPair(&server_side)) {
        bento_test::Skip("loopback TCP unavailable");
        return;
    }

    const std::vector<std::uint8_t> request = MakePattern(31, 32 * 1024);
    const std::vector<std::uint8_t> response = MakePattern(32, 48 * 1024);
    std::vector<std::uint8_t> server_got;
    std::vector<std::uint8_t> client_got;
    bool server_ok = false;
    bool client_ok = false;
    RelayResult relay_result;

    std::thread relay_thread([&] {
        Relay relay;
        relay_result = relay.Run(*client_side.acceptor, *server_side.dialer, MakeTcpOptions());
    });
    std::thread server_thread([&] {
        const bool read_ok = ReadUntilEof(*server_side.acceptor, &server_got, 64 * 1024, 5000);
        const bool write_ok = WriteFull(*server_side.acceptor, response);
        (void)server_side.acceptor->ShutdownWrite();
        server_ok = read_ok && write_ok;
    });

    const bool client_write_ok = WriteFull(*client_side.dialer, request);
    (void)client_side.dialer->ShutdownWrite();
    const bool client_read_ok = ReadUntilEof(*client_side.dialer, &client_got, 64 * 1024, 5000);
    client_ok = client_write_ok && client_read_ok;

    server_thread.join();
    (void)client_side.dialer->Close();
    (void)server_side.acceptor->Close();
    relay_thread.join();

    BENTO_CHECK(server_ok);
    BENTO_CHECK(client_ok);
    BENTO_CHECK(SameBytes(server_got, request));
    BENTO_CHECK(SameBytes(client_got, response));
    BENTO_CHECK(relay_result.completed());
    BENTO_CHECK_EQ(relay_result.stats.half_closes, 2U);
    std::printf("[INFO] half close: request=%zu response=%zu half_closes=%llu\n", server_got.size(),
                client_got.size(),
                static_cast<unsigned long long>(relay_result.stats.half_closes));
}

/// 场景 3：慢客户端。目标端写永久超时 → 断开、有界读入、RSS 不膨胀。
void TestSlowConsumer() {
    std::printf("[CASE] slow consumer write stall\n");
    std::vector<std::uint8_t> source = MakePattern(41, 4 * 1024 * 1024);
    ScriptedTransport src(std::move(source));
    ScriptedTransport dst;
    dst.block_writes = true;

    const long rss_before = CurrentRssKb();

    RelayOptions options;
    options.buffer_bytes = kBufferBytes;
    options.read_chunk_bytes = 16 * 1024;
    options.read_slice_ms = 0;
    options.write_slice_ms = 5;
    options.poll_interval_ms = 1;
    options.write_timeout_ms = 60;
    options.idle_timeout_ms = 5000;

    Relay relay;
    const RelayResult result = relay.Run(src, dst, options);
    const long rss_after = CurrentRssKb();

    BENTO_CHECK(result.reason == RelayCloseReason::kSlowConsumer);
    BENTO_CHECK_EQ(result.stats.slow_consumers, 1U);
    BENTO_CHECK_EQ(dst.write_bytes, 0U);
    // 关键：读入量被一方缓冲卡住，不会把源端 4MB 全吸进内存。
    BENTO_CHECK(src.read_bytes <= kBufferBytes);
    BENTO_CHECK(result.stats.read_src_to_dst <= kBufferBytes);
    BENTO_CHECK(result.stats.peak_pending_src_to_dst <= kBufferBytes);
    BENTO_CHECK(result.elapsed_ms >= 50);
    BENTO_CHECK(result.elapsed_ms < 3000);
    BENTO_CHECK(src.closed);
    BENTO_CHECK(dst.closed);
    if (rss_before >= 0 && rss_after >= 0) {
        BENTO_CHECK(rss_after - rss_before < 8 * 1024);
    }
    std::printf("[INFO] slow consumer: elapsed=%llums read=%llu peak=%llu rss_delta=%ldKB\n",
                static_cast<unsigned long long>(result.elapsed_ms),
                static_cast<unsigned long long>(src.read_bytes),
                static_cast<unsigned long long>(result.stats.peak_pending_src_to_dst),
                rss_after - rss_before);
}

/// 场景 4：有界缓冲 + 背压。目标端每次只收 4KB，pending 不得超过 64KB。
void TestBoundedBufferBackpressure() {
    std::printf("[CASE] bounded buffer backpressure\n");
    constexpr std::size_t kBytes = 512 * 1024;
    const std::vector<std::uint8_t> expected = MakePattern(43, kBytes);
    ScriptedTransport src(expected);
    ScriptedTransport dst;
    dst.max_write_per_call = 4 * 1024;

    RelayOptions options;
    options.buffer_bytes = kBufferBytes;
    options.read_chunk_bytes = 8 * 1024;
    options.read_slice_ms = 0;
    options.write_slice_ms = 5;
    options.poll_interval_ms = 1;
    options.write_timeout_ms = 2000;
    options.idle_timeout_ms = 5000;

    Relay relay;
    const RelayResult result = relay.Run(src, dst, options);

    BENTO_CHECK(result.completed());
    BENTO_CHECK(SameBytes(dst.received, expected));
    BENTO_CHECK_EQ(result.stats.bytes_src_to_dst, kBytes);
    BENTO_CHECK_EQ(result.stats.read_src_to_dst, kBytes);
    BENTO_CHECK(result.stats.peak_pending_src_to_dst <= kBufferBytes);
    BENTO_CHECK(result.stats.read_src_to_dst - result.stats.bytes_src_to_dst <= kBufferBytes);
    std::printf("[INFO] backpressure: bytes=%llu peak_pending=%llu\n",
                static_cast<unsigned long long>(result.stats.bytes_src_to_dst),
                static_cast<unsigned long long>(result.stats.peak_pending_src_to_dst));
}

/// 场景 5：双向半关闭传播（内存传输）。FIN 各发一次，两个方向数据都完整。
void TestHalfClosePropagation() {
    std::printf("[CASE] half close propagation\n");
    const std::vector<std::uint8_t> forward = MakePattern(51, 8 * 1024);
    const std::vector<std::uint8_t> backward = MakePattern(52, 4 * 1024);
    ScriptedTransport src(forward);
    ScriptedTransport dst(backward);

    RelayOptions options;
    options.buffer_bytes = 16 * 1024;
    options.read_chunk_bytes = 4 * 1024;
    options.read_slice_ms = 1;
    options.write_slice_ms = 2;
    options.write_timeout_ms = 1000;
    options.idle_timeout_ms = 3000;

    Relay relay;
    const RelayResult result = relay.Run(src, dst, options);

    BENTO_CHECK(result.completed());
    BENTO_CHECK_EQ(src.shutdown_write_calls, 1U);
    BENTO_CHECK_EQ(dst.shutdown_write_calls, 1U);
    BENTO_CHECK(SameBytes(dst.received, forward));
    BENTO_CHECK(SameBytes(src.received, backward));
    BENTO_CHECK_EQ(result.stats.half_closes, 2U);
    BENTO_CHECK_EQ(result.stats.bytes_src_to_dst, forward.size());
    BENTO_CHECK_EQ(result.stats.bytes_dst_to_src, backward.size());
}

/// 场景 6：空流边界。双向立即 EOF，应正常完成且只发两个 FIN。
void TestEmptyStreams() {
    std::printf("[CASE] empty streams\n");
    ScriptedTransport src;
    ScriptedTransport dst;
    Relay relay;
    const RelayResult result = relay.Run(src, dst);

    BENTO_CHECK(result.completed());
    BENTO_CHECK_EQ(result.stats.bytes_src_to_dst, 0U);
    BENTO_CHECK_EQ(result.stats.bytes_dst_to_src, 0U);
    BENTO_CHECK_EQ(result.stats.half_closes, 2U);
    BENTO_CHECK_EQ(src.shutdown_write_calls, 1U);
    BENTO_CHECK_EQ(dst.shutdown_write_calls, 1U);
}

/// 场景 7：空闲超时。双向都不动时必须有界退出，不能永久占用线程。
void TestIdleTimeout() {
    std::printf("[CASE] idle timeout\n");
    ScriptedTransport src;
    ScriptedTransport dst;
    src.eof_after_input = false;
    dst.eof_after_input = false;

    RelayOptions options;
    options.read_slice_ms = 2;
    options.write_slice_ms = 2;
    options.poll_interval_ms = 1;
    options.write_timeout_ms = 1000;
    options.idle_timeout_ms = 100;

    Relay relay;
    const auto start = Clock::now();
    const RelayResult result = relay.Run(src, dst, options);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();

    BENTO_CHECK(result.reason == RelayCloseReason::kIdleTimeout);
    BENTO_CHECK_EQ(result.stats.idle_timeouts, 1U);
    // NowMs 是截断毫秒：判定时刻与 Run 起点各截断一次，实测可能差 1~2ms，
    // 因此下限留 5ms 余量，上界仍验证「有界退出」。
    BENTO_CHECK(elapsed >= 95);
    BENTO_CHECK(elapsed < 2000);
    BENTO_CHECK(src.closed);
    BENTO_CHECK(dst.closed);
    std::printf("[INFO] idle timeout: elapsed=%lldms\n", static_cast<long long>(elapsed));
}

/// 场景 8：拨号超时。ConnectAndRun 必须返回 kConnectTimeout 且不开始搬运。
void TestConnectTimeout() {
    std::printf("[CASE] connect timeout\n");
    ScriptedTransport src;
    ScriptedTransport dst;
    dst.connect_timeout = true;

    RelayOptions options;
    options.connect_timeout_ms = 50;

    Relay relay;
    const RelayResult result =
        relay.ConnectAndRun(src, dst, Endpoint{"127.0.0.1", 9}, options);

    BENTO_CHECK(result.reason == RelayCloseReason::kConnectTimeout);
    BENTO_CHECK_EQ(result.stats.connect_timeouts, 1U);
    BENTO_CHECK(src.closed);
    BENTO_CHECK(dst.closed);
    BENTO_CHECK_EQ(src.read_bytes, 0U);
    BENTO_CHECK_EQ(dst.read_bytes, 0U);
}

/// 场景 9：拨号失败（拒绝连接等）。返回 kConnectFailed，同样关闭两端。
void TestConnectFailure() {
    std::printf("[CASE] connect failure\n");
    ScriptedTransport src;
    ScriptedTransport dst;
    dst.connect_error = true;

    Relay relay;
    const RelayResult result = relay.ConnectAndRun(src, dst, Endpoint{"127.0.0.1", 9});

    BENTO_CHECK(result.reason == RelayCloseReason::kConnectFailed);
    BENTO_CHECK_EQ(result.stats.connect_failures, 1U);
    BENTO_CHECK(src.closed);
    BENTO_CHECK(dst.closed);
}

/// 场景 10：随机分块。每次 Write 上限随机，验证不丢字节且缓冲有界。
void TestRandomChunkIntegrity() {
    std::printf("[CASE] random chunk integrity\n");
    constexpr std::size_t kBytes = 256 * 1024;
    const std::vector<std::uint8_t> expected = MakePattern(61, kBytes);
    ScriptedTransport src(expected);
    ScriptedTransport dst;

    std::mt19937 rng(20261028U);
    std::uniform_int_distribution<std::size_t> limit(1, 8192);
    dst.write_limit = [&rng, &limit]() { return limit(rng); };

    RelayOptions options;
    options.buffer_bytes = 32 * 1024;
    options.read_chunk_bytes = 4 * 1024;
    options.read_slice_ms = 0;
    options.write_slice_ms = 5;
    options.write_timeout_ms = 2000;
    options.idle_timeout_ms = 5000;

    Relay relay;
    const RelayResult result = relay.Run(src, dst, options);

    BENTO_CHECK(result.completed());
    BENTO_CHECK(SameBytes(dst.received, expected));
    BENTO_CHECK(result.stats.peak_pending_src_to_dst <= 32 * 1024);
    std::printf("[INFO] random chunks: bytes=%llu peak=%llu\n",
                static_cast<unsigned long long>(result.stats.bytes_src_to_dst),
                static_cast<unsigned long long>(result.stats.peak_pending_src_to_dst));
}

/// 场景 11：目标端写错误（EPIPE/RST）。必须立即结束并保留已写字节数。
void TestWriteError() {
    std::printf("[CASE] write error\n");
    constexpr std::size_t kBytes = 64 * 1024;
    ScriptedTransport src(MakePattern(71, kBytes));
    ScriptedTransport dst;
    dst.fail_writes_after = 10 * 1024;

    RelayOptions options;
    options.buffer_bytes = kBufferBytes;
    options.read_chunk_bytes = 16 * 1024;
    options.read_slice_ms = 0;
    options.write_slice_ms = 5;
    options.write_timeout_ms = 1000;
    options.idle_timeout_ms = 5000;

    Relay relay;
    const RelayResult result = relay.Run(src, dst, options);

    BENTO_CHECK(result.reason == RelayCloseReason::kWriteError);
    BENTO_CHECK_EQ(result.stats.write_errors, 1U);
    BENTO_CHECK_EQ(result.stats.bytes_src_to_dst, 10U * 1024U);
    BENTO_CHECK(result.stats.read_src_to_dst > result.stats.bytes_src_to_dst);
    BENTO_CHECK(result.system_error != 0);
}

/// 场景 12：极小缓冲边界。buffer=64B、写上限 7B，验证不会卡死或越界。
void TestTinyBuffer() {
    std::printf("[CASE] tiny buffer boundary\n");
    const std::vector<std::uint8_t> expected = MakePattern(91, 4096);
    ScriptedTransport src(expected);
    ScriptedTransport dst;
    dst.max_write_per_call = 7;

    RelayOptions options;
    options.buffer_bytes = 64;
    options.read_chunk_bytes = 0;  // 0 = 使用剩余空间
    options.read_slice_ms = 0;
    options.write_slice_ms = 2;
    options.write_timeout_ms = 2000;
    options.idle_timeout_ms = 5000;

    Relay relay;
    const RelayResult result = relay.Run(src, dst, options);

    BENTO_CHECK(result.completed());
    BENTO_CHECK(SameBytes(dst.received, expected));
    BENTO_CHECK(result.stats.peak_pending_src_to_dst <= 64);
    std::printf("[INFO] tiny buffer: bytes=%llu peak=%llu\n",
                static_cast<unsigned long long>(result.stats.bytes_src_to_dst),
                static_cast<unsigned long long>(result.stats.peak_pending_src_to_dst));
}

/// 场景 13：真实 TCP 单向 4MB 压测。给出吞吐指标，验证大流量下不丢字节。
void TestTcpThroughput() {
    std::printf("[CASE] tcp 4MB throughput\n");
    TcpPair client_side;
    TcpPair server_side;
    if (!MakeTcpPair(&client_side) || !MakeTcpPair(&server_side)) {
        bento_test::Skip("loopback TCP unavailable");
        return;
    }

    constexpr std::size_t kBytes = 4 * 1024 * 1024;
    const std::vector<std::uint8_t> payload = MakePattern(81, kBytes);
    std::vector<std::uint8_t> received(kBytes);
    bool client_write = false;
    bool server_read = false;
    RelayResult relay_result;

    std::thread relay_thread([&] {
        Relay relay;
        relay_result = relay.Run(*client_side.acceptor, *server_side.dialer, MakeTcpOptions());
    });
    std::thread reader([&] {
        server_read = ReadFull(*server_side.acceptor, &received);
        (void)server_side.acceptor->ShutdownWrite();
    });

    const auto begin = Clock::now();
    client_write = WriteFull(*client_side.dialer, payload);
    (void)client_side.dialer->ShutdownWrite();
    reader.join();
    const auto end = Clock::now();
    const double seconds = std::chrono::duration<double>(end - begin).count();

    (void)client_side.dialer->Close();
    (void)server_side.acceptor->Close();
    relay_thread.join();

    BENTO_CHECK(client_write);
    BENTO_CHECK(server_read);
    BENTO_CHECK(SameBytes(received, payload));
    BENTO_CHECK(relay_result.completed());
    const double mib_per_second =
        (static_cast<double>(kBytes) / (1024.0 * 1024.0)) / (seconds > 0.0 ? seconds : 1.0);
    BENTO_CHECK(mib_per_second > 1.0);
    std::printf("[INFO] throughput: %.2f MiB/s (%.0f ms, %zu bytes)\n", mib_per_second,
                seconds * 1000.0, kBytes);
}

}  // namespace

int main() {
    TestTcpBothDirections();
    TestTcpHalfClose();
    TestSlowConsumer();
    TestBoundedBufferBackpressure();
    TestHalfClosePropagation();
    TestEmptyStreams();
    TestIdleTimeout();
    TestConnectTimeout();
    TestConnectFailure();
    TestRandomChunkIntegrity();
    TestWriteError();
    TestTinyBuffer();
    TestTcpThroughput();
    return bento_test::Finish("test_relay");
}
