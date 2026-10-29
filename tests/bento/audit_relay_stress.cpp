/**
 * @file audit_relay_stress.cpp
 * @brief `Relay` 深度审查用的独立压测/随机/场景程序（不注册进 CTest）。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 与 `tests/bento/test_relay.cpp` 的分工：
 *          - `test_relay.cpp` 是**注册进 CTest 的回归测试**，只放确定性、秒级用例；
 *          - 本程序是**审查压测工具**，接受 scale / 随机种子 / 长稳时长，覆盖
 *            100 MiB 以上双向流量、1000 并发 relay、1200 组随机混沌、真实
 *            慢客户端、fd/内存泄漏与 60 秒长稳。
 *
 * 用法：
 * @code
 *   audit_relay_stress 1.0                       # 全量（不含长稳）
 *   audit_relay_stress 1.0 --mode=throughput     # 只跑吞吐
 *   audit_relay_stress 1.0 --mode=bp             # 背压/Defrag 微基准
 *   audit_relay_stress 1.0 --long=60             # 追加 60 秒双向长稳
 *   audit_relay_stress 0.02 --mode=all           # ASan/UBSan 小规模
 *   audit_relay_stress 0.01 --mode=leaks         # valgrind 小规模
 *   audit_relay_stress 1.0 --seed=12345          # 换随机种子
 * @endcode
 *
 * 退出码：0 = 全部断言通过；1 = 有失败（失败用例会打印 CHECK 位置与数值）。
 * 每项测试都先记录 `/proc/self/fd` 与 `VmRSS`/`VmHWM`，结束后复核 fd 回到
 * 基线、RSS 增量有界，避免“跑通但泄漏”。
 */

#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/memory_transport.hpp"
#include "go2cpp/bento/net/relay.hpp"
#include "go2cpp/bento/net/tcp_transport.hpp"

namespace {

using go2cpp::bento::net::Code;
using go2cpp::bento::net::Endpoint;
using go2cpp::bento::net::ITransport;
using go2cpp::bento::net::MemoryTransport;
using go2cpp::bento::net::Relay;
using go2cpp::bento::net::RelayCloseReason;
using go2cpp::bento::net::RelayCloseReasonName;
using go2cpp::bento::net::RelayOptions;
using go2cpp::bento::net::RelayResult;
using go2cpp::bento::net::RelayStats;
using go2cpp::bento::net::Result;
using go2cpp::bento::net::TcpTransport;

using Clock = std::chrono::steady_clock;

constexpr std::size_t kBlockBytes = 64 * 1024;
constexpr std::size_t kDefaultBuffer = 64 * 1024;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(actual, expected)                                                        \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!((actual) == (expected))) {                                                  \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual, \
                         #expected);                                                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
}

std::chrono::milliseconds Ms(std::uint32_t value) {
    return std::chrono::milliseconds(static_cast<std::int64_t>(value));
}

void SleepMs(std::uint32_t value) {
    if (value != 0) {
        std::this_thread::sleep_for(Ms(value));
    }
}

/// 读取 /proc/self/status 里的 VmRSS/VmHWM（KB）；失败返回 -1。
long ProcStatusKb(const char* key) {
    std::ifstream status("/proc/self/status");
    if (!status) {
        return -1;
    }
    const std::size_t key_len = std::strlen(key);
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, key_len, key) == 0) {
            long value = -1;
            if (std::sscanf(line.c_str() + key_len, " %ld kB", &value) == 1) {
                return value;
            }
            return -1;
        }
    }
    return -1;
}

/// 当前进程打开的 fd 数（/proc/self/fd 条目数，已扣除 . 与 ..）。
int FdCount() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (::readdir(dir) != nullptr) {
        ++count;
    }
    ::closedir(dir);
    return count - 2;
}

/// 进程累计 CPU 时间（user+sys，秒）。
double CpuSeconds() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    return static_cast<double>(usage.ru_utime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec) / 1e6 +
           static_cast<double>(usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
}

/// 64 KiB 确定性图案块；数据流就是它的无限重复。
std::vector<std::uint8_t> MakeBlock(std::uint32_t seed) {
    std::vector<std::uint8_t> block(kBlockBytes);
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = static_cast<std::uint8_t>((seed * 131U + i * 17U + (i >> 8U) * 7U) & 0xFFU);
    }
    return block;
}

std::vector<std::uint8_t> MakeBytes(std::uint32_t seed, std::size_t count) {
    std::vector<std::uint8_t> data(count);
    for (std::size_t i = 0; i < count; ++i) {
        data[i] = static_cast<std::uint8_t>((seed * 131U + i * 17U + (i >> 8U) * 7U) & 0xFFU);
    }
    return data;
}

bool VerifyBlockAt(const std::vector<std::uint8_t>& block, std::size_t offset,
                   const std::uint8_t* data, std::size_t count) {
    const std::size_t offset_in_block = offset & (kBlockBytes - 1);
    if (offset_in_block + count <= kBlockBytes) {
        // 常见路径：块内连续，直接 memcmp，避免逐字节验证成为压测瓶颈。
        return std::memcmp(data, block.data() + offset_in_block, count) == 0;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (data[i] != block[(offset + i) & (kBlockBytes - 1)]) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// TCP 回环辅助
// ---------------------------------------------------------------------------

struct TcpPair {
    std::unique_ptr<TcpTransport> dialer;
    std::unique_ptr<TcpTransport> acceptor;
};

bool MakeTcpPair(TcpPair* pair) {
    TcpTransport listener;
    if (listener.Listen(Endpoint{"127.0.0.1", 0}, 128).code != Code::kOk) {
        return false;
    }
    Endpoint local;
    if (listener.LocalEndpoint(&local).code != Code::kOk) {
        return false;
    }
    auto dialer = std::make_unique<TcpTransport>();
    if (dialer->Connect(local, Ms(2000)).code != Code::kOk) {
        return false;
    }
    auto acceptor = std::make_unique<TcpTransport>();
    if (listener.Accept(acceptor.get(), Ms(2000)).code != Code::kOk) {
        return false;
    }
    pair->dialer = std::move(dialer);
    pair->acceptor = std::move(acceptor);
    return true;
}

// ---------------------------------------------------------------------------
// 可编排的内存传输：混沌随机、慢客户端、错误注入
// ---------------------------------------------------------------------------

class ChaosTransport final : public ITransport {
public:
    explicit ChaosTransport(std::vector<std::uint8_t> data = {}) : input(std::move(data)) {}

    std::vector<std::uint8_t> input;
    bool eof_after_input = true;
    bool block_forever = false;
    std::uint64_t block_writes = 0;  ///< 前 N 次 Write 返回 kTimeout（无进展）。
    std::size_t max_read = 0;        ///< 单次 Read 上限；0 = 不限。
    std::size_t max_write = 0;       ///< 单次 Write 上限；0 = 不限。
    std::uint32_t write_delay_ms = 0;
    bool read_error = false;
    bool write_error = false;
    bool shutdown_error = false;

    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t shutdown_calls = 0;
    std::uint64_t close_calls = 0;
    std::uint64_t bytes_at_shutdown = 0;
    bool closed = false;
    std::vector<std::uint8_t> received;

    Result Connect(const Endpoint&, std::chrono::milliseconds) override {
        return Result{Code::kOk, 0, 0};
    }

    Result Read(std::uint8_t* buffer, std::size_t capacity,
                std::chrono::milliseconds) override {
        if (m_closed) {
            return Result{Code::kClosed, ENOTCONN, 0};
        }
        if (capacity == 0) {
            return Result{Code::kOk, 0, 0};
        }
        if (read_error) {
            return Result{Code::kError, EIO, 0};
        }
        if (m_pos < input.size()) {
            std::size_t count = std::min(capacity, input.size() - m_pos);
            if (max_read != 0) {
                count = std::min(count, max_read);
            }
            std::memcpy(buffer, input.data() + m_pos, count);
            m_pos += count;
            read_bytes += count;
            return Result{Code::kOk, 0, count};
        }
        if (eof_after_input) {
            return Result{Code::kClosed, 0, 0};
        }
        return Result{Code::kTimeout, ETIMEDOUT, 0};
    }

    Result Write(const std::uint8_t* data, std::size_t size,
                 std::chrono::milliseconds) override {
        if (m_closed || m_write_shutdown) {
            return Result{Code::kClosed, EPIPE, 0};
        }
        if (size == 0) {
            return Result{Code::kOk, 0, 0};
        }
        if (write_error) {
            return Result{Code::kError, EIO, 0};
        }
        if (block_forever) {
            return Result{Code::kTimeout, ETIMEDOUT, 0};
        }
        if (block_writes != 0) {
            --block_writes;
            return Result{Code::kTimeout, ETIMEDOUT, 0};
        }
        std::size_t count = size;
        if (max_write != 0) {
            count = std::min(count, max_write);
        }
        if (count == 0) {
            return Result{Code::kTimeout, ETIMEDOUT, 0};
        }
        SleepMs(write_delay_ms);
        received.insert(received.end(), data, data + count);
        write_bytes += count;
        return Result{Code::kOk, 0, count};
    }

    Result ShutdownWrite() override {
        ++shutdown_calls;
        bytes_at_shutdown = received.size();
        m_write_shutdown = true;
        if (shutdown_error) {
            return Result{Code::kError, EIO, 0};
        }
        return Result{Code::kOk, 0, 0};
    }

    Result Close() override {
        ++close_calls;
        closed = true;
        m_closed = true;
        return Result{Code::kOk, 0, 0};
    }

private:
    std::size_t m_pos = 0;
    bool m_closed = false;
    bool m_write_shutdown = false;
};

/// 只读图案源：不预先分配 N 字节，按 kBlockBytes 重复生成，便于做 100 MiB 级单向压测。
class PatternSource final : public ITransport {
public:
    PatternSource(std::vector<std::uint8_t> block, std::size_t total)
        : m_block(std::move(block)), m_total(total) {}

    std::uint64_t shutdown_calls = 0;
    bool closed = false;

    Result Connect(const Endpoint&, std::chrono::milliseconds) override {
        return Result{Code::kOk, 0, 0};
    }

    Result Read(std::uint8_t* buffer, std::size_t capacity,
                std::chrono::milliseconds) override {
        if (m_closed) {
            return Result{Code::kClosed, ENOTCONN, 0};
        }
        if (capacity == 0) {
            return Result{Code::kOk, 0, 0};
        }
        if (m_pos >= m_total) {
            return Result{Code::kClosed, 0, 0};
        }
        const std::size_t count = std::min(capacity, m_total - m_pos);
        const std::size_t offset_in_block = m_pos & (kBlockBytes - 1);
        if (offset_in_block + count <= kBlockBytes) {
            std::memcpy(buffer, m_block.data() + offset_in_block, count);
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                buffer[i] = m_block[(m_pos + i) & (kBlockBytes - 1)];
            }
        }
        m_pos += count;
        return Result{Code::kOk, 0, count};
    }

    Result Write(const std::uint8_t*, std::size_t, std::chrono::milliseconds) override {
        return Result{Code::kClosed, EPIPE, 0};
    }

    Result ShutdownWrite() override {
        ++shutdown_calls;
        return Result{Code::kOk, 0, 0};
    }

    Result Close() override {
        closed = true;
        m_closed = true;
        return Result{Code::kOk, 0, 0};
    }

private:
    std::vector<std::uint8_t> m_block;
    std::size_t m_total = 0;
    std::size_t m_pos = 0;
    bool m_closed = false;
};

/// 限速接收端：每次 Write 只接受 per_write 字节，其他方向立即 EOF。
class ThrottledSink final : public ITransport {
public:
    ThrottledSink(std::vector<std::uint8_t> block, std::size_t per_write)
        : m_block(std::move(block)), m_per_write(per_write) {}

    std::size_t received = 0;
    bool verified = true;
    std::uint64_t shutdown_calls = 0;
    bool closed = false;

    Result Connect(const Endpoint&, std::chrono::milliseconds) override {
        return Result{Code::kOk, 0, 0};
    }

    Result Read(std::uint8_t*, std::size_t, std::chrono::milliseconds) override {
        return Result{Code::kClosed, 0, 0};
    }

    Result Write(const std::uint8_t* data, std::size_t size,
                 std::chrono::milliseconds) override {
        if (m_closed) {
            return Result{Code::kClosed, EPIPE, 0};
        }
        const std::size_t count = std::min(size, m_per_write);
        if (!VerifyBlockAt(m_block, received, data, count)) {
            verified = false;
        }
        received += count;
        return Result{Code::kOk, 0, count};
    }

    Result ShutdownWrite() override {
        ++shutdown_calls;
        return Result{Code::kOk, 0, 0};
    }

    Result Close() override {
        closed = true;
        m_closed = true;
        return Result{Code::kOk, 0, 0};
    }

private:
    std::vector<std::uint8_t> m_block;
    std::size_t m_per_write = 0;
    bool m_closed = false;
};

// ---------------------------------------------------------------------------
// 双向真实 TCP 流（用于 100 MiB 吞吐与 60 秒长稳）
// ---------------------------------------------------------------------------

struct StreamOutcome {
    RelayResult relay;
    std::size_t client_sent = 0;
    std::size_t server_sent = 0;
    std::size_t client_received = 0;
    std::size_t server_received = 0;
    bool client_write_ok = false;
    bool server_write_ok = false;
    bool client_read_ok = false;
    bool server_read_ok = false;
    std::uint64_t elapsed_ms = 0;
};

bool WritePattern(ITransport& transport, const std::vector<std::uint8_t>& block,
                  std::size_t total, std::uint64_t deadline, std::size_t* written) {
    std::size_t sent = 0;
    while (sent < total) {
        if (NowMs() > deadline) {
            break;
        }
        // 部分写后必须从「已写到的块内偏移」继续；否则重发块首会造成流错位。
        const std::size_t offset = sent & (kBlockBytes - 1);
        const std::size_t count = std::min(kBlockBytes - offset, total - sent);
        const Result result = transport.Write(block.data() + offset, count, Ms(1000));
        if (result.bytes != 0) {
            sent += std::min(result.bytes, count);
        }
        if (result.code == Code::kClosed || result.code == Code::kError) {
            break;
        }
    }
    *written = sent;
    return sent == total;
}

bool ReadPattern(ITransport& transport, const std::vector<std::uint8_t>& block,
                 std::size_t total, std::uint64_t deadline, std::size_t* received) {
    std::vector<std::uint8_t> buffer(kBlockBytes);
    std::size_t got = 0;
    bool verified = true;
    while (got < total) {
        if (NowMs() > deadline) {
            break;
        }
        const std::size_t want = std::min(buffer.size(), total - got);
        const Result result = transport.Read(buffer.data(), want, Ms(1000));
        if (result.bytes != 0) {
            const std::size_t count = std::min(result.bytes, want);
            if (!VerifyBlockAt(block, got, buffer.data(), count)) {
                verified = false;
            }
            got += count;
        }
        if (result.code == Code::kClosed) {
            break;
        }
        if (result.code == Code::kError) {
            break;
        }
    }
    *received = got;
    return verified && got == total;
}

bool RunBidirectional(std::size_t total, const RelayOptions& options, StreamOutcome* out) {
    TcpPair client_side;
    TcpPair server_side;
    if (!MakeTcpPair(&client_side) || !MakeTcpPair(&server_side)) {
        return false;
    }
    const std::vector<std::uint8_t> request = MakeBlock(11);
    const std::vector<std::uint8_t> response = MakeBlock(22);
    const std::uint64_t deadline = NowMs() + 60000;
    const std::uint64_t start = NowMs();

    std::thread relay_thread([&] {
        Relay relay;
        out->relay = relay.Run(*client_side.acceptor, *server_side.dialer, options);
    });
    std::thread client_writer([&] {
        out->client_write_ok = WritePattern(*client_side.dialer, request, total, deadline,
                                            &out->client_sent);
        (void)client_side.dialer->ShutdownWrite();
    });
    std::thread server_writer([&] {
        out->server_write_ok = WritePattern(*server_side.acceptor, response, total, deadline,
                                            &out->server_sent);
        (void)server_side.acceptor->ShutdownWrite();
    });
    std::thread client_reader([&] {
        out->client_read_ok = ReadPattern(*client_side.dialer, response, total, deadline,
                                          &out->client_received);
    });
    std::thread server_reader([&] {
        out->server_read_ok = ReadPattern(*server_side.acceptor, request, total, deadline,
                                          &out->server_received);
    });

    client_writer.join();
    server_writer.join();
    client_reader.join();
    server_reader.join();
    relay_thread.join();
    out->elapsed_ms = NowMs() - start;

    (void)client_side.dialer->Close();
    (void)server_side.acceptor->Close();
    return true;
}

RelayOptions TcpRelayOptions() {
    RelayOptions options;
    options.buffer_bytes = kDefaultBuffer;
    options.read_chunk_bytes = 16 * 1024;
    options.read_slice_ms = 0;
    options.write_slice_ms = 10;
    options.poll_interval_ms = 1;
    options.write_timeout_ms = 3000;
    options.idle_timeout_ms = 30000;
    return options;
}

// ---------------------------------------------------------------------------
// 1) 100 MiB 以上双向吞吐
// ---------------------------------------------------------------------------

void TestThroughput(double scale) {
    const std::size_t minimum = 1024 * 1024;
    std::size_t total = static_cast<std::size_t>(128.0 * 1024.0 * 1024.0 * scale);
    total = std::max(minimum, total);
    total = (total / kBlockBytes) * kBlockBytes;
    const int rounds = scale >= 0.5 ? 3 : 1;
    const int fd_base = FdCount();
    const long rss_base = ProcStatusKb("VmRSS:");
    const long hwm_base = ProcStatusKb("VmHWM:");

    std::printf("[TEST] bidirectional TCP throughput: %zu MiB each direction x %d rounds\n",
                total / (1024 * 1024), rounds);
    for (int round = 1; round <= rounds; ++round) {
        StreamOutcome outcome;
        const bool setup = RunBidirectional(total, TcpRelayOptions(), &outcome);
        CHECK(setup);
        if (!setup) {
            return;
        }
        CHECK(outcome.relay.completed());
        CHECK(outcome.client_write_ok);
        CHECK(outcome.server_write_ok);
        CHECK(outcome.client_read_ok);
        CHECK(outcome.server_read_ok);
        CHECK_EQ(outcome.server_received, total);
        CHECK_EQ(outcome.client_received, total);
        CHECK_EQ(outcome.relay.stats.bytes_src_to_dst, total);
        CHECK_EQ(outcome.relay.stats.bytes_dst_to_src, total);
        CHECK(outcome.relay.stats.peak_pending_src_to_dst <= kDefaultBuffer);
        CHECK(outcome.relay.stats.peak_pending_dst_to_src <= kDefaultBuffer);
        CHECK_EQ(FdCount(), fd_base);

        const double seconds = static_cast<double>(outcome.elapsed_ms) / 1000.0;
        const double per_direction =
            (static_cast<double>(total) / (1024.0 * 1024.0)) / (seconds > 0.0 ? seconds : 1.0);
        std::printf(
            "[DATA] throughput r%d: %.1f MiB/s aggregate (%.1f MiB/s/dir), %llu ms, "
            "peak_pending=%llu/%llu, rss=%ldKB, hwm=%ldKB\n",
            round, per_direction * 2.0, per_direction,
            static_cast<unsigned long long>(outcome.elapsed_ms),
            static_cast<unsigned long long>(outcome.relay.stats.peak_pending_src_to_dst),
            static_cast<unsigned long long>(outcome.relay.stats.peak_pending_dst_to_src),
            ProcStatusKb("VmRSS:"), ProcStatusKb("VmHWM:"));
    }
    CHECK_EQ(FdCount(), fd_base);
    const long hwm_after = ProcStatusKb("VmHWM:");
    if (rss_base >= 0 && hwm_after >= 0) {
        std::printf("[DATA] throughput RSS: base=%ldKB hwm_base=%ldKB hwm_after=%ldKB delta=%ldKB\n",
                    rss_base, hwm_base, hwm_after, hwm_after - hwm_base);
        CHECK(hwm_after - hwm_base < 64 * 1024);
    }
}

// ---------------------------------------------------------------------------
// 2) Defrag/背压微基准：慢接收端每次只收 4 KiB
// ---------------------------------------------------------------------------

void TestBackpressureBenchmark(double scale) {
    std::size_t total = static_cast<std::size_t>(512.0 * 1024.0 * 1024.0 * scale);
    total = std::max<std::size_t>(kBlockBytes, total);
    total = (total / kBlockBytes) * kBlockBytes;
    std::printf("[TEST] backpressure microbenchmark: %zu MiB, 4 KiB per write\n",
                total / (1024 * 1024));

    const std::vector<std::uint8_t> block = MakeBlock(71);
    RelayOptions options;
    options.buffer_bytes = kDefaultBuffer;
    options.read_chunk_bytes = 16 * 1024;
    options.read_slice_ms = 0;
    options.write_slice_ms = 0;
    options.poll_interval_ms = 0;
    options.write_timeout_ms = 0;
    options.idle_timeout_ms = 0;

    PatternSource source(block, total);
    ThrottledSink sink(block, 4 * 1024);
    Relay relay;
    const std::uint64_t start = NowMs();
    const RelayResult result = relay.Run(source, sink, options);
    const std::uint64_t elapsed = NowMs() - start;

    CHECK(result.completed());
    CHECK_EQ(result.stats.bytes_src_to_dst, total);
    CHECK_EQ(sink.received, total);
    CHECK(sink.verified);
    CHECK(result.stats.peak_pending_src_to_dst <= kDefaultBuffer);
    const double mib_per_second =
        (static_cast<double>(total) / (1024.0 * 1024.0)) /
        (static_cast<double>(elapsed) / 1000.0 + 0.000001);
    std::printf("[DATA] backpressure: %.1f MiB/s, %llu ms, peak=%llu, defrag_limited\n",
                mib_per_second, static_cast<unsigned long long>(elapsed),
                static_cast<unsigned long long>(result.stats.peak_pending_src_to_dst));

    // 对照：接收端不设限，只看 Relay 自身循环与系统调用开销。
    PatternSource fast_source(block, total);
    ThrottledSink fast_sink(block, kBlockBytes);
    Relay fast_relay;
    const std::uint64_t fast_start = NowMs();
    const RelayResult fast_result = fast_relay.Run(fast_source, fast_sink, options);
    const std::uint64_t fast_elapsed = NowMs() - fast_start;
    CHECK(fast_result.completed());
    CHECK_EQ(fast_sink.received, total);
    const double fast_mib_per_second =
        (static_cast<double>(total) / (1024.0 * 1024.0)) /
        (static_cast<double>(fast_elapsed) / 1000.0 + 0.000001);
    std::printf("[DATA] fast sink: %.1f MiB/s, %llu ms\n", fast_mib_per_second,
                static_cast<unsigned long long>(fast_elapsed));
}

// ---------------------------------------------------------------------------
// 3) 1000 并发 relay（MemoryTransport）
// ---------------------------------------------------------------------------

void TestConcurrentMemoryRelays(double scale) {
    const std::size_t count = std::max<std::size_t>(1, static_cast<std::size_t>(1000.0 * scale));
    constexpr std::size_t kPayload = 8 * 1024;
    std::printf("[TEST] concurrent memory relays: %zu relays x %zu B each way\n", count,
                kPayload);

    struct Item {
        MemoryTransport::Pair client_pair;
        MemoryTransport::Pair server_pair;
        std::thread thread;
        RelayResult result;
    };
    std::vector<std::unique_ptr<Item>> items;
    items.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto item = std::make_unique<Item>();
        item->client_pair = MemoryTransport::CreatePair();
        item->server_pair = MemoryTransport::CreatePair();
        items.push_back(std::move(item));
    }

    RelayOptions options;
    options.buffer_bytes = kDefaultBuffer;
    options.read_chunk_bytes = 16 * 1024;
    options.read_slice_ms = 0;
    options.write_slice_ms = 5;
    options.poll_interval_ms = 1;
    options.write_timeout_ms = 5000;
    options.idle_timeout_ms = 10000;

    const int fd_base = FdCount();
    const long rss_base = ProcStatusKb("VmRSS:");
    const std::uint64_t start = NowMs();
    bool aborted = false;
    for (std::size_t i = 0; i < count; ++i) {
        Item* item = items[i].get();
        try {
            item->thread = std::thread([item, &options] {
                Relay relay;
                item->result =
                    relay.Run(*item->client_pair.second, *item->server_pair.first, options);
            });
        } catch (const std::system_error& error) {
            std::fprintf(stderr, "[FAIL] thread %zu: %s\n", i, error.what());
            ++g_failures;
            aborted = true;
            break;
        }
    }

    const long rss_during = ProcStatusKb("VmRSS:");
    const int fd_during = FdCount();
    if (!aborted) {
        for (std::size_t i = 0; i < count; ++i) {
            Item* item = items[i].get();
            const std::vector<std::uint8_t> request =
                MakeBytes(static_cast<std::uint32_t>(i + 1), kPayload);
            const std::vector<std::uint8_t> response =
                MakeBytes(static_cast<std::uint32_t>(i + 100000), kPayload);
            std::vector<std::uint8_t> received(kPayload);

            CHECK(item->client_pair.first->WriteFull(request.data(), request.size(), Ms(5000))
                      .code == Code::kOk);
            CHECK(item->server_pair.second->WriteFull(response.data(), response.size(), Ms(5000))
                      .code == Code::kOk);
            CHECK(item->server_pair.second->ReadFull(received.data(), received.size(),
                                                     Ms(5000))
                      .code == Code::kOk);
            CHECK(received == request);
            CHECK(item->client_pair.first->ReadFull(received.data(), received.size(), Ms(5000))
                      .code == Code::kOk);
            CHECK(received == response);
            (void)item->client_pair.first->ShutdownWrite();
            (void)item->server_pair.second->ShutdownWrite();
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            (void)items[i]->client_pair.first->Close();
            (void)items[i]->server_pair.second->Close();
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (items[i]->thread.joinable()) {
            items[i]->thread.join();
        }
    }
    const std::uint64_t elapsed = NowMs() - start;
    const long rss_after_join = ProcStatusKb("VmRSS:");
    const int fd_after = FdCount();

    if (!aborted) {
        for (std::size_t i = 0; i < count; ++i) {
            CHECK(items[i]->result.completed());
            CHECK_EQ(items[i]->result.stats.bytes_src_to_dst, kPayload);
            CHECK_EQ(items[i]->result.stats.bytes_dst_to_src, kPayload);
            CHECK(items[i]->result.stats.peak_pending_src_to_dst <= kDefaultBuffer);
            CHECK(items[i]->result.stats.peak_pending_dst_to_src <= kDefaultBuffer);
        }
    }
    CHECK_EQ(fd_after, fd_base);
    CHECK(fd_during - fd_base < 8);  // MemoryTransport 不占 fd。
    if (rss_base >= 0 && rss_during >= 0) {
        std::printf(
            "[DATA] concurrent relays: %zu in %llu ms, rss base=%ldKB during=%ldKB "
            "after_join=%ldKB (%.1f KB/relay), fd=%d/%d\n",
            count, static_cast<unsigned long long>(elapsed), rss_base, rss_during,
            rss_after_join,
            count == 0 ? 0.0 : static_cast<double>(rss_during - rss_base) / double(count),
            fd_during, fd_after);
        CHECK(rss_during - rss_base < 512 * 1024);
    }
    std::printf("[DATA] concurrent relays aggregate: %.1f MiB/s\n",
                (2.0 * double(count) * double(kPayload) / (1024.0 * 1024.0)) /
                    (double(elapsed) / 1000.0 + 0.000001));

    // 空闲放大：同样数量 relay 完全没流量时的 CPU 占用（poll 间隔的代价）。
    // 线程启动完成后才开始计时，排除 1000 次线程创建的 CPU。
    const std::size_t idle_count = count;
    auto measure_idle_cpu = [&](std::uint32_t poll_ms) -> double {
        std::vector<std::unique_ptr<Item>> idle_items;
        idle_items.reserve(idle_count);
        for (std::size_t i = 0; i < idle_count; ++i) {
            auto item = std::make_unique<Item>();
            item->client_pair = MemoryTransport::CreatePair();
            item->server_pair = MemoryTransport::CreatePair();
            idle_items.push_back(std::move(item));
        }
        RelayOptions idle_options = options;
        idle_options.poll_interval_ms = poll_ms;
        std::atomic<std::size_t> started{0};
        std::size_t started_threads = 0;
        for (std::size_t i = 0; i < idle_count; ++i) {
            Item* item = idle_items[i].get();
            try {
                item->thread = std::thread([item, &idle_options, &started] {
                    started.fetch_add(1);
                    Relay relay;
                    item->result =
                        relay.Run(*item->client_pair.second, *item->server_pair.first,
                                  idle_options);
                });
                ++started_threads;
            } catch (const std::system_error& error) {
                std::fprintf(stderr, "[FAIL] idle thread %zu: %s\n", i, error.what());
                ++g_failures;
                break;
            }
        }
        while (started.load() < started_threads) {
            std::this_thread::yield();
        }
        const double cpu_before = CpuSeconds();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const double cpu_after = CpuSeconds();
        for (std::size_t i = 0; i < idle_count; ++i) {
            (void)idle_items[i]->client_pair.first->Close();
            (void)idle_items[i]->server_pair.second->Close();
        }
        for (std::size_t i = 0; i < idle_count; ++i) {
            if (idle_items[i]->thread.joinable()) {
                idle_items[i]->thread.join();
            }
        }
        return cpu_after - cpu_before;
    };
    const double cpu_poll1 = measure_idle_cpu(1);
    const double cpu_poll5 = measure_idle_cpu(5);
    std::printf(
        "[DATA] idle scaling: %zu relays, 1 s idle, poll1=%.3f s (%.1f%% of one core), "
        "poll5=%.3f s (%.1f%%)\n",
        idle_count, cpu_poll1, cpu_poll1 * 100.0, cpu_poll5, cpu_poll5 * 100.0);
    CHECK(cpu_poll1 < 30000.0);  // 只防失控，不作性能承诺。
}

// ---------------------------------------------------------------------------
// 4) 200 并发真实 TCP relay + fd 记账
// ---------------------------------------------------------------------------

void TestConcurrentTcpRelays(double scale) {
    const std::size_t count = std::max<std::size_t>(2, static_cast<std::size_t>(200.0 * scale));
    constexpr std::size_t kPayload = 4 * 1024;
    std::printf("[TEST] concurrent TCP relays: %zu relays x %zu B each way\n", count, kPayload);
    const int fd_base = FdCount();

    TcpTransport listener_client;
    TcpTransport listener_server;
    CHECK(listener_client.Listen(Endpoint{"127.0.0.1", 0}, 1024).code == Code::kOk);
    CHECK(listener_server.Listen(Endpoint{"127.0.0.1", 0}, 1024).code == Code::kOk);
    Endpoint client_endpoint;
    Endpoint server_endpoint;
    CHECK(listener_client.LocalEndpoint(&client_endpoint).code == Code::kOk);
    CHECK(listener_server.LocalEndpoint(&server_endpoint).code == Code::kOk);

    struct Item {
        std::unique_ptr<TcpTransport> client;
        std::unique_ptr<TcpTransport> relay_src;
        std::unique_ptr<TcpTransport> server;
        std::unique_ptr<TcpTransport> relay_dst;
        std::thread thread;
        RelayResult result;
    };
    std::vector<std::unique_ptr<Item>> items;
    items.reserve(count);
    bool setup_ok = true;
    for (std::size_t i = 0; i < count; ++i) {
        auto item = std::make_unique<Item>();
        item->client = std::make_unique<TcpTransport>();
        item->relay_src = std::make_unique<TcpTransport>();
        item->server = std::make_unique<TcpTransport>();
        item->relay_dst = std::make_unique<TcpTransport>();
        if (item->client->Connect(client_endpoint, Ms(3000)).code != Code::kOk ||
            listener_client.Accept(item->relay_src.get(), Ms(3000)).code != Code::kOk ||
            item->server->Connect(server_endpoint, Ms(3000)).code != Code::kOk ||
            listener_server.Accept(item->relay_dst.get(), Ms(3000)).code != Code::kOk) {
            setup_ok = false;
            break;
        }
        items.push_back(std::move(item));
    }
    CHECK(setup_ok);
    const int fd_during = FdCount();
    std::printf("[DATA] tcp relays: fd base=%d during=%d (%.1f fd/relay)\n", fd_base, fd_during,
                count == 0 ? 0.0 : double(fd_during - fd_base) / double(count));
    CHECK(fd_during - fd_base >= static_cast<int>(4 * count));
    if (!setup_ok) {
        for (auto& item : items) {
            (void)item->client->Close();
            (void)item->relay_src->Close();
            (void)item->server->Close();
            (void)item->relay_dst->Close();
        }
        return;
    }

    RelayOptions options = TcpRelayOptions();
    options.write_timeout_ms = 5000;
    const long rss_base = ProcStatusKb("VmRSS:");
    const std::uint64_t start = NowMs();
    for (std::size_t i = 0; i < count; ++i) {
        Item* item = items[i].get();
        item->thread = std::thread([item, &options] {
            Relay relay;
            item->result = relay.Run(*item->relay_src, *item->relay_dst, options);
        });
    }
    const long rss_during = ProcStatusKb("VmRSS:");

    bool io_ok = true;
    for (std::size_t i = 0; i < count; ++i) {
        Item* item = items[i].get();
        const std::vector<std::uint8_t> request =
            MakeBytes(static_cast<std::uint32_t>(i + 7), kPayload);
        const std::vector<std::uint8_t> response =
            MakeBytes(static_cast<std::uint32_t>(i + 200000), kPayload);
        std::vector<std::uint8_t> received(kPayload);
        if (item->client->WriteFull(request.data(), request.size(), Ms(5000)).code != Code::kOk ||
            item->server->WriteFull(response.data(), response.size(), Ms(5000)).code != Code::kOk ||
            item->server->ReadFull(received.data(), received.size(), Ms(5000)).code != Code::kOk ||
            received != request ||
            item->client->ReadFull(received.data(), received.size(), Ms(5000)).code != Code::kOk ||
            received != response) {
            io_ok = false;
            break;
        }
        (void)item->client->ShutdownWrite();
        (void)item->server->ShutdownWrite();
    }
    CHECK(io_ok);
    for (auto& item : items) {
        if (item->thread.joinable()) {
            item->thread.join();
        }
    }
    const std::uint64_t elapsed = NowMs() - start;
    for (auto& item : items) {
        CHECK(item->result.completed());
        CHECK(item->result.stats.peak_pending_src_to_dst <= kDefaultBuffer);
        (void)item->client->Close();
        (void)item->relay_src->Close();
        (void)item->server->Close();
        (void)item->relay_dst->Close();
    }
    (void)listener_client.Close();
    (void)listener_server.Close();
    CHECK_EQ(FdCount(), fd_base);
    std::printf("[DATA] tcp relays: %zu in %llu ms, rss base=%ldKB during=%ldKB\n", count,
                static_cast<unsigned long long>(elapsed), rss_base, rss_during);
    CHECK(elapsed < 30000);
}

// ---------------------------------------------------------------------------
// 5) 1200 组随机混沌
// ---------------------------------------------------------------------------

struct ChaosPlan {
    std::size_t forward_size = 0;
    std::size_t backward_size = 0;
    bool forward_eof = true;
    bool backward_eof = true;
    bool forward_block_forever = false;
    bool backward_block_forever = false;
    std::uint64_t forward_block_writes = 0;
    std::uint64_t backward_block_writes = 0;
    std::size_t forward_max_read = 0;
    std::size_t backward_max_read = 0;
    std::size_t forward_max_write = 0;
    std::size_t backward_max_write = 0;
    bool shutdown_error = false;
    RelayOptions options;
};

/// 校验 received 是 input 的有序前缀（中途断开时唯一合法的数据形态）。
bool IsPrefix(const std::vector<std::uint8_t>& received, const std::vector<std::uint8_t>& input) {
    return received.size() <= input.size() &&
           std::equal(received.begin(), received.end(), input.begin());
}

std::size_t PickBuffer(std::mt19937& rng) {
    static const std::size_t kSizes[] = {1, 2, 7, 64, 256, 1024, 4096, 16 * 1024, 64 * 1024};
    std::uniform_int_distribution<std::size_t> pick(0, sizeof(kSizes) / sizeof(kSizes[0]) - 1);
    return kSizes[pick(rng)];
}

void RandomChaosRun(std::size_t iterations, std::uint32_t seed) {
    std::printf("[TEST] random chaos: %zu iterations, seed=%u\n", iterations, seed);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> small(0, 4096);
    std::uniform_int_distribution<int> percent(0, 99);
    std::uniform_int_distribution<int> budget(0, 4);
    std::uniform_int_distribution<int> chunk(1, 2048);
    std::uniform_int_distribution<int> slice(0, 2);
    std::uniform_int_distribution<int> idle(10, 40);

    const std::size_t completed_cases = (iterations * 75) / 100;
    const std::size_t idle_cases = (iterations * 10) / 100;
    const std::size_t slow_cases = iterations - completed_cases - idle_cases;
    std::size_t completed_ok_seen = 0;
    std::size_t completion_error_seen = 0;
    std::size_t idle_seen = 0;
    std::size_t slow_seen = 0;
    const long rss_base = ProcStatusKb("VmRSS:");

    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const bool completion_group = iteration < completed_cases;
        const bool idle_group = iteration >= completed_cases &&
                                iteration < completed_cases + idle_cases;

        ChaosPlan plan;
        plan.forward_size = static_cast<std::size_t>(small(rng));
        plan.backward_size = static_cast<std::size_t>(small(rng));
        plan.options.buffer_bytes = PickBuffer(rng);
        plan.options.read_chunk_bytes = static_cast<std::size_t>(chunk(rng));
        plan.options.read_slice_ms = 0;
        plan.options.write_slice_ms = static_cast<std::uint32_t>(slice(rng));
        plan.options.poll_interval_ms = percent(rng) < 80 ? 0 : 1;
        plan.options.write_timeout_ms = 0;
        plan.options.idle_timeout_ms = 0;
        plan.shutdown_error = false;

        if (completion_group) {
            plan.forward_eof = true;
            plan.backward_eof = true;
            plan.forward_block_writes = static_cast<std::uint64_t>(budget(rng));
            plan.backward_block_writes = static_cast<std::uint64_t>(budget(rng));
            plan.forward_max_read = percent(rng) < 50 ? 0 : static_cast<std::size_t>(chunk(rng));
            plan.backward_max_read = percent(rng) < 50 ? 0 : static_cast<std::size_t>(chunk(rng));
            plan.forward_max_write = percent(rng) < 30 ? 0 : static_cast<std::size_t>(chunk(rng));
            plan.backward_max_write = percent(rng) < 30 ? 0 : static_cast<std::size_t>(chunk(rng));
            if (percent(rng) < 5) {
                plan.shutdown_error = true;
            }
        } else if (idle_group) {
            // 至少一端永不 EOF：必须靠 idle_timeout 有界退出。
            plan.forward_eof = percent(rng) < 50;
            plan.backward_eof = !plan.forward_eof || percent(rng) < 50;
            if (plan.forward_eof && plan.backward_eof) {
                plan.backward_eof = false;
            }
            plan.options.idle_timeout_ms = static_cast<std::uint32_t>(idle(rng));
            plan.options.poll_interval_ms = 0;
        } else {
            // 永久写阻塞：必须由 write_timeout 判定 kSlowConsumer。
            plan.forward_eof = true;
            plan.backward_eof = true;
            plan.forward_size = static_cast<std::size_t>(1 + (small(rng) % 2048));
            plan.forward_block_writes = 0;
            plan.backward_block_writes = 0;
            plan.backward_block_forever = true;
            plan.options.buffer_bytes = std::max<std::size_t>(64, PickBuffer(rng));
            plan.options.write_timeout_ms =
                static_cast<std::uint32_t>(10 + (percent(rng) % 30));
            plan.options.poll_interval_ms = 1;
        }

        ChaosTransport forward;
        forward.input = MakeBytes(static_cast<std::uint32_t>(rng()), plan.forward_size);
        forward.eof_after_input = plan.forward_eof;
        forward.block_writes = plan.forward_block_writes;
        forward.max_read = plan.forward_max_read;
        forward.max_write = plan.forward_max_write;
        forward.block_forever = plan.forward_block_forever;

        ChaosTransport backward;
        backward.input = MakeBytes(static_cast<std::uint32_t>(rng()), plan.backward_size);
        backward.eof_after_input = plan.backward_eof;
        backward.block_writes = plan.backward_block_writes;
        backward.max_read = plan.backward_max_read;
        backward.max_write = plan.backward_max_write;
        backward.block_forever = plan.backward_block_forever;
        if (plan.shutdown_error) {
            backward.shutdown_error = true;
        }

        Relay relay;
        const std::uint64_t start = NowMs();
        const RelayResult result = relay.Run(forward, backward, plan.options);
        const std::uint64_t elapsed = NowMs() - start;

        // 通用不变量：不越界、不超时挂死、缓冲有界、数据是前缀。
        CHECK(result.stats.peak_pending_src_to_dst <= plan.options.buffer_bytes);
        CHECK(result.stats.peak_pending_dst_to_src <= plan.options.buffer_bytes);
        CHECK(elapsed < 5000);
        CHECK(IsPrefix(backward.received, forward.input));
        CHECK(IsPrefix(forward.received, backward.input));
        CHECK(backward.received.size() <= forward.input.size());
        CHECK(forward.received.size() <= backward.input.size());
        CHECK(result.stats.read_src_to_dst <= forward.input.size());
        CHECK(result.stats.read_dst_to_src <= backward.input.size());

        if (completion_group && !plan.shutdown_error) {
            ++completed_ok_seen;
            if (!result.completed() || backward.received != forward.input ||
                forward.received != backward.input) {
                std::fprintf(stderr,
                             "[FAIL] chaos completed mismatch iter=%zu reason=%s fwd=%zu/%zu "
                             "bwd=%zu/%zu\n",
                             iteration, RelayCloseReasonName(result.reason),
                             backward.received.size(), forward.input.size(),
                             forward.received.size(), backward.input.size());
                ++g_failures;
            }
            CHECK(result.completed());
            CHECK_EQ(result.stats.half_closes, 2U);
            CHECK_EQ(backward.bytes_at_shutdown, forward.input.size());
            CHECK_EQ(forward.bytes_at_shutdown, backward.input.size());
            CHECK_EQ(backward.shutdown_calls, 1U);
            CHECK_EQ(forward.shutdown_calls, 1U);
            CHECK(backward.closed);
            CHECK(forward.closed);
        } else if (completion_group && plan.shutdown_error) {
            // FIN 传播失败必须显式报 kWriteError，不能伪装成 Completed。
            ++completion_error_seen;
            CHECK(result.reason == RelayCloseReason::kWriteError);
            CHECK_EQ(result.stats.write_errors, 1U);
            CHECK(result.stats.half_closes <= 2U);
            CHECK(result.system_error != 0);
        } else if (idle_group) {
            ++idle_seen;
            CHECK(result.reason == RelayCloseReason::kIdleTimeout);
            CHECK_EQ(result.stats.idle_timeouts, 1U);
            CHECK(result.stats.slow_consumers == 0);
            CHECK(backward.closed);
            CHECK(forward.closed);
        } else {
            ++slow_seen;
            if (result.reason != RelayCloseReason::kSlowConsumer) {
                std::fprintf(stderr, "[FAIL] chaos slow iter=%zu reason=%s size=%zu peak=%llu\n",
                             iteration, RelayCloseReasonName(result.reason),
                             forward.input.size(),
                             static_cast<unsigned long long>(
                                 result.stats.peak_pending_src_to_dst));
                ++g_failures;
            }
            CHECK(result.reason == RelayCloseReason::kSlowConsumer);
            CHECK_EQ(result.stats.slow_consumers, 1U);
            CHECK_EQ(result.stats.bytes_src_to_dst, 0U);
            CHECK(result.stats.read_src_to_dst <= plan.options.buffer_bytes);
            CHECK(result.stats.read_src_to_dst - result.stats.bytes_src_to_dst <=
                  plan.options.buffer_bytes);
            CHECK(backward.received.empty());
        }
    }

    CHECK_EQ(completed_ok_seen + completion_error_seen, completed_cases);
    CHECK_EQ(idle_seen, idle_cases);
    CHECK_EQ(slow_seen, slow_cases);
    const long rss_after = ProcStatusKb("VmRSS:");
    if (rss_base >= 0 && rss_after >= 0) {
        std::printf("[DATA] chaos rss: base=%ldKB after=%ldKB delta=%ldKB\n", rss_base, rss_after,
                    rss_after - rss_base);
        CHECK(rss_after - rss_base < 64 * 1024);
    }
}

// ---------------------------------------------------------------------------
// 6) 场景：慢而仍在推进的客户端不能被误杀
// ---------------------------------------------------------------------------

void TestSlowButMoving() {
    std::printf("[TEST] slow but progressing consumer must not be killed\n");
    const std::vector<std::uint8_t> payload = MakeBytes(31, 64 * 1024);
    ChaosTransport source(payload);
    ChaosTransport sink;
    sink.max_write = 4 * 1024;
    sink.write_delay_ms = 10;

    RelayOptions options;
    options.buffer_bytes = kDefaultBuffer;
    options.read_chunk_bytes = 16 * 1024;
    options.write_slice_ms = 5;
    options.poll_interval_ms = 1;
    options.write_timeout_ms = 60;
    options.idle_timeout_ms = 5000;

    Relay relay;
    const std::uint64_t start = NowMs();
    const RelayResult result = relay.Run(source, sink, options);
    const std::uint64_t elapsed = NowMs() - start;

    CHECK(result.completed());
    CHECK_EQ(result.stats.slow_consumers, 0U);
    CHECK_EQ(sink.received.size(), payload.size());
    CHECK(sink.received == payload);
    CHECK(elapsed >= 100);
    CHECK(elapsed < 2000);
    std::printf("[DATA] slow-but-moving: %llu ms, slow_consumers=%llu\n",
                static_cast<unsigned long long>(elapsed),
                static_cast<unsigned long long>(result.stats.slow_consumers));
}

// ---------------------------------------------------------------------------
// 7) 场景：FIN 必须在 pending 排空之后
// ---------------------------------------------------------------------------

void TestFinAfterDrain() {
    std::printf("[TEST] FIN is sent only after pending bytes are drained\n");
    const std::vector<std::uint8_t> payload = MakeBytes(37, 256 * 1024);
    ChaosTransport source(payload);
    source.max_read = 16 * 1024;
    ChaosTransport sink;
    sink.max_write = 4 * 1024;

    RelayOptions options;
    options.buffer_bytes = kDefaultBuffer;
    options.read_chunk_bytes = 16 * 1024;
    options.write_slice_ms = 0;
    options.poll_interval_ms = 0;
    options.write_timeout_ms = 0;
    options.idle_timeout_ms = 0;

    Relay relay;
    const RelayResult result = relay.Run(source, sink, options);

    CHECK(result.completed());
    CHECK(sink.received == payload);
    CHECK_EQ(sink.bytes_at_shutdown, payload.size());
    CHECK_EQ(sink.shutdown_calls, 1U);
    CHECK_EQ(result.stats.half_closes, 2U);
    std::printf("[DATA] fin-after-drain: bytes_at_shutdown=%llu total=%zu\n",
                static_cast<unsigned long long>(sink.bytes_at_shutdown), payload.size());
}

// ---------------------------------------------------------------------------
// 8) 场景：有流量时 idle 计时必须复位
// ---------------------------------------------------------------------------

void TestIdleResetByTraffic() {
    std::printf("[TEST] idle timer resets while traffic flows\n");
    auto client_pair = MemoryTransport::CreatePair();
    auto server_pair = MemoryTransport::CreatePair();
    RelayOptions options;
    options.buffer_bytes = 16 * 1024;
    options.read_chunk_bytes = 4 * 1024;
    options.read_slice_ms = 0;
    options.write_slice_ms = 5;
    options.poll_interval_ms = 1;
    options.write_timeout_ms = 1000;
    options.idle_timeout_ms = 200;

    RelayResult result;
    std::thread relay_thread([&] {
        Relay relay;
        result = relay.Run(*client_pair.second, *server_pair.first, options);
    });

    bool io_ok = true;
    const std::vector<std::uint8_t> chunk = MakeBytes(41, 1024);
    std::vector<std::uint8_t> received(1024);
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if (client_pair.first->WriteFull(chunk.data(), chunk.size(), Ms(2000)).code != Code::kOk ||
            server_pair.second->ReadFull(received.data(), received.size(), Ms(2000)).code !=
                Code::kOk ||
            received != chunk) {
            io_ok = false;
            break;
        }
    }
    (void)client_pair.first->ShutdownWrite();
    (void)server_pair.second->ShutdownWrite();
    relay_thread.join();

    CHECK(io_ok);
    CHECK(result.completed());
    CHECK_EQ(result.stats.idle_timeouts, 0U);
    std::printf("[DATA] idle-reset: elapsed=%llu ms idle_timeouts=%llu\n",
                static_cast<unsigned long long>(result.elapsed_ms),
                static_cast<unsigned long long>(result.stats.idle_timeouts));
}

// ---------------------------------------------------------------------------
// 9) 场景：Normalize 边界（buffer=0 / read_chunk 超限）
// ---------------------------------------------------------------------------

void TestNormalizeBoundaries() {
    std::printf("[TEST] option normalization boundaries\n");
    const std::vector<std::uint8_t> block = MakeBlock(43);

    {
        PatternSource source(block, 256 * 1024);
        ThrottledSink sink(block, kBlockBytes);
        RelayOptions options;
        options.buffer_bytes = 0;                // → 默认 64 KiB
        options.read_chunk_bytes = 1024 * 1024;  // → 被收敛到 buffer
        options.read_slice_ms = 0;
        options.write_slice_ms = 0;
        options.write_timeout_ms = 0;
        options.idle_timeout_ms = 0;
        Relay relay;
        const RelayResult result = relay.Run(source, sink, options);
        CHECK(result.completed());
        CHECK_EQ(result.stats.bytes_src_to_dst, 256U * 1024U);
        CHECK(result.stats.peak_pending_src_to_dst <= 64 * 1024);
        std::printf("[DATA] normalize buffer=0: peak=%llu\n",
                    static_cast<unsigned long long>(result.stats.peak_pending_src_to_dst));
    }
    {
        PatternSource source(block, 8 * 1024);
        ThrottledSink sink(block, 64);
        RelayOptions options;
        options.buffer_bytes = 1024;
        options.read_chunk_bytes = 0;  // → buffer
        options.read_slice_ms = 0;
        options.write_slice_ms = 0;
        options.write_timeout_ms = 0;
        options.idle_timeout_ms = 0;
        Relay relay;
        const RelayResult result = relay.Run(source, sink, options);
        CHECK(result.completed());
        CHECK_EQ(result.stats.bytes_src_to_dst, 8U * 1024U);
        CHECK(result.stats.peak_pending_src_to_dst <= 1024);
    }
}

// ---------------------------------------------------------------------------
// 10) 场景：读错误 / 写错误 / FIN 传播错误
// ---------------------------------------------------------------------------

void TestErrorMapping() {
    std::printf("[TEST] error mapping: read / write / shutdown\n");
    {
        ChaosTransport source;
        source.read_error = true;
        ChaosTransport sink;
        Relay relay;
        const RelayResult result = relay.Run(source, sink);
        CHECK(result.reason == RelayCloseReason::kReadError);
        CHECK_EQ(result.stats.read_errors, 1U);
        CHECK_EQ(result.stats.write_errors, 0U);
        CHECK_EQ(result.system_error, EIO);
        CHECK(source.closed);
        CHECK(sink.closed);
    }
    {
        ChaosTransport source(MakeBytes(47, 4096));
        ChaosTransport sink;
        sink.write_error = true;
        Relay relay;
        const RelayResult result = relay.Run(source, sink);
        CHECK(result.reason == RelayCloseReason::kWriteError);
        CHECK_EQ(result.stats.write_errors, 1U);
        CHECK_EQ(result.system_error, EIO);
        CHECK_EQ(result.stats.bytes_src_to_dst, 0U);
    }
    {
        ChaosTransport source;
        ChaosTransport sink;
        sink.shutdown_error = true;
        Relay relay;
        const RelayResult result = relay.Run(source, sink);
        CHECK(result.reason == RelayCloseReason::kWriteError);
        CHECK_EQ(result.stats.write_errors, 1U);
        CHECK_EQ(result.stats.half_closes, 0U);
        CHECK_EQ(result.system_error, EIO);
        CHECK(source.closed);
        CHECK(sink.closed);
    }
    CHECK(std::strcmp(RelayCloseReasonName(RelayCloseReason::kCompleted), "Completed") == 0);
    CHECK(std::strcmp(RelayCloseReasonName(static_cast<RelayCloseReason>(99)), "Unknown") == 0);
}

// ---------------------------------------------------------------------------
// 11) 场景：Relay::Stats 跨运行累计（峰值取 max）
// ---------------------------------------------------------------------------

void TestStatsAggregation() {
    std::printf("[TEST] stats aggregation across runs\n");
    Relay relay;
    {
        ChaosTransport source(MakeBytes(53, 4096));
        ChaosTransport sink;
        const RelayResult result = relay.Run(source, sink);
        CHECK(result.completed());
    }
    {
        ChaosTransport source(MakeBytes(54, 2048));
        ChaosTransport sink;
        const RelayResult result = relay.Run(source, sink);
        CHECK(result.completed());
        CHECK_EQ(result.stats.bytes_src_to_dst, 2048U);
    }
    const RelayStats total = relay.Stats();
    CHECK_EQ(total.bytes_src_to_dst, 6144U);
    CHECK_EQ(total.read_src_to_dst, 6144U);
    CHECK_EQ(total.half_closes, 4U);

    RelayStats left;
    left.peak_pending_src_to_dst = 111;
    left.peak_buffered_bytes = 222;
    left.bytes_src_to_dst = 7;
    RelayStats right;
    right.peak_pending_src_to_dst = 99;
    right.peak_buffered_bytes = 333;
    right.bytes_src_to_dst = 5;
    left += right;
    CHECK_EQ(left.peak_pending_src_to_dst, 111U);
    CHECK_EQ(left.peak_buffered_bytes, 333U);
    CHECK_EQ(left.bytes_src_to_dst, 12U);
}

// ---------------------------------------------------------------------------
// 12) 真实 TCP 慢客户端（对端停止读取，内核发送缓冲写满）
// ---------------------------------------------------------------------------

void TestSlowConsumerTcp() {
    std::printf("[TEST] real TCP slow consumer (peer stops reading)\n");
    TcpPair client_side;
    TcpPair server_side;
    CHECK(MakeTcpPair(&client_side));
    CHECK(MakeTcpPair(&server_side));
    if (!client_side.dialer || !server_side.acceptor) {
        return;
    }
    RelayOptions options;
    options.buffer_bytes = kDefaultBuffer;
    options.read_chunk_bytes = 16 * 1024;
    options.read_slice_ms = 0;
    options.write_slice_ms = 5;
    options.poll_interval_ms = 1;
    options.write_timeout_ms = 300;
    options.idle_timeout_ms = 30000;

    RelayResult result;
    const std::uint64_t start = NowMs();
    std::thread relay_thread([&] {
        Relay relay;
        result = relay.Run(*client_side.acceptor, *server_side.dialer, options);
    });
    std::thread server_thread([&] {
        std::uint8_t scratch[16 * 1024];
        (void)server_side.acceptor->Read(scratch, sizeof(scratch), Ms(2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        (void)server_side.acceptor->Close();
    });
    std::size_t written = 0;
    const std::vector<std::uint8_t> payload = MakeBlock(59);
    std::thread client_thread([&] {
        const std::uint64_t deadline = NowMs() + 8000;
        while (written < 64U * 1024U * 1024U && NowMs() < deadline) {
            const Result r = client_side.dialer->Write(payload.data(), payload.size(), Ms(1000));
            if (r.bytes != 0) {
                written += r.bytes;
            }
            if (r.code == Code::kClosed || r.code == Code::kError) {
                break;
            }
        }
        (void)client_side.dialer->ShutdownWrite();
    });

    relay_thread.join();
    const std::uint64_t elapsed = NowMs() - start;
    server_thread.join();
    client_thread.join();
    (void)client_side.dialer->Close();

    CHECK(result.reason == RelayCloseReason::kSlowConsumer);
    CHECK_EQ(result.stats.slow_consumers, 1U);
    CHECK(result.stats.peak_pending_src_to_dst <= kDefaultBuffer);
    CHECK(result.stats.read_src_to_dst - result.stats.bytes_src_to_dst <= kDefaultBuffer);
    CHECK(result.stats.bytes_src_to_dst < 64U * 1024U * 1024U);
    CHECK(elapsed < 4000);
    CHECK(written < 64U * 1024U * 1024U);
    std::printf("[DATA] tcp slow consumer: %llu ms, read=%llu written_to_kernel=%llu, "
                "client_sent=%zu\n",
                static_cast<unsigned long long>(elapsed),
                static_cast<unsigned long long>(result.stats.read_src_to_dst),
                static_cast<unsigned long long>(result.stats.bytes_src_to_dst), written);
}

// ---------------------------------------------------------------------------
// 13) fd 泄漏：200 次 TCP relay 建立/销毁
// ---------------------------------------------------------------------------

void TestFdCycles(double scale) {
    const std::size_t cycles = std::max<std::size_t>(3, static_cast<std::size_t>(200.0 * scale));
    std::printf("[TEST] fd cycles: %zu short TCP relays\n", cycles);
    const int fd_base = FdCount();
    for (std::size_t i = 0; i < cycles; ++i) {
        TcpPair client_side;
        TcpPair server_side;
        CHECK(MakeTcpPair(&client_side));
        CHECK(MakeTcpPair(&server_side));
        if (!client_side.dialer || !server_side.acceptor) {
            return;
        }
        RelayResult result;
        std::thread relay_thread([&] {
            Relay relay;
            result = relay.Run(*client_side.acceptor, *server_side.dialer, TcpRelayOptions());
        });
        const std::vector<std::uint8_t> request = MakeBytes(static_cast<std::uint32_t>(i), 4096);
        std::vector<std::uint8_t> received(4096);
        CHECK(client_side.dialer->WriteFull(request.data(), request.size(), Ms(5000)).code ==
              Code::kOk);
        CHECK(server_side.acceptor->ReadFull(received.data(), received.size(), Ms(5000)).code ==
              Code::kOk);
        CHECK(received == request);
        (void)client_side.dialer->ShutdownWrite();
        (void)server_side.acceptor->ShutdownWrite();
        relay_thread.join();
        CHECK(result.completed());
        (void)client_side.dialer->Close();
        (void)server_side.acceptor->Close();
    }
    const int fd_after = FdCount();
    CHECK_EQ(fd_after, fd_base);
    std::printf("[DATA] fd cycles: base=%d after=%d\n", fd_base, fd_after);
}

// ---------------------------------------------------------------------------
// 14) 60 秒长稳：双向持续流量 + RSS/fd 采样
// ---------------------------------------------------------------------------

void TestLongRun(int seconds, double scale) {
    (void)scale;
    if (seconds <= 0) {
        std::printf("[SKIP] long run disabled (pass --long=60 to enable)\n");
        return;
    }
    const std::size_t chunk = kBlockBytes;
    const std::uint32_t period_ms = 16;
    std::printf("[TEST] long run: %d s bidirectional, ~4 MiB/s/dir\n", seconds);

    const int fd_base = FdCount();
    TcpPair client_side;
    TcpPair server_side;
    CHECK(MakeTcpPair(&client_side));
    CHECK(MakeTcpPair(&server_side));
    if (!client_side.dialer || !server_side.acceptor) {
        return;
    }
    RelayOptions options = TcpRelayOptions();
    options.write_timeout_ms = 5000;
    options.idle_timeout_ms = 30000;

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> client_sent{0};
    std::atomic<std::size_t> server_sent{0};
    std::atomic<std::size_t> client_received{0};
    std::atomic<std::size_t> server_received{0};
    std::atomic<bool> client_ok{true};
    std::atomic<bool> server_ok{true};
    const std::vector<std::uint8_t> request = MakeBlock(61);
    const std::vector<std::uint8_t> response = MakeBlock(62);

    const long rss_start = ProcStatusKb("VmRSS:");
    const std::uint64_t start = NowMs();
    const double cpu_start = CpuSeconds();

    RelayResult result;
    std::thread relay_thread([&] {
        Relay relay;
        result = relay.Run(*client_side.acceptor, *server_side.dialer, options);
    });
    std::thread client_writer([&] {
        std::size_t offset = 0;
        while (!stop.load()) {
            const std::size_t in_block = offset & (kBlockBytes - 1);
            const std::size_t count = std::min(kBlockBytes - in_block, chunk);
            const Result r =
                client_side.dialer->Write(request.data() + in_block, count, Ms(1000));
            if (r.bytes != 0) {
                const std::size_t moved = std::min(r.bytes, count);
                offset += moved;
                client_sent.fetch_add(moved);
            }
            if (r.code == Code::kClosed || r.code == Code::kError) {
                client_ok.store(false);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        }
        (void)client_side.dialer->ShutdownWrite();
    });
    std::thread server_writer([&] {
        std::size_t offset = 0;
        while (!stop.load()) {
            const std::size_t in_block = offset & (kBlockBytes - 1);
            const std::size_t count = std::min(kBlockBytes - in_block, chunk);
            const Result r =
                server_side.acceptor->Write(response.data() + in_block, count, Ms(1000));
            if (r.bytes != 0) {
                const std::size_t moved = std::min(r.bytes, count);
                offset += moved;
                server_sent.fetch_add(moved);
            }
            if (r.code == Code::kClosed || r.code == Code::kError) {
                server_ok.store(false);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        }
        (void)server_side.acceptor->ShutdownWrite();
    });
    std::thread client_reader([&] {
        std::vector<std::uint8_t> buffer(chunk);
        std::size_t offset = 0;
        for (;;) {
            const Result r = client_side.dialer->Read(buffer.data(), buffer.size(), Ms(1000));
            if (r.bytes != 0) {
                const std::size_t count = std::min(r.bytes, buffer.size());
                if (!VerifyBlockAt(response, offset, buffer.data(), count)) {
                    client_ok.store(false);
                }
                offset += count;
                client_received.store(offset);
            }
            if (r.code == Code::kClosed) {
                break;
            }
            if (r.code == Code::kError) {
                client_ok.store(false);
                break;
            }
        }
    });
    std::thread server_reader([&] {
        std::vector<std::uint8_t> buffer(chunk);
        std::size_t offset = 0;
        for (;;) {
            const Result r = server_side.acceptor->Read(buffer.data(), buffer.size(), Ms(1000));
            if (r.bytes != 0) {
                const std::size_t count = std::min(r.bytes, buffer.size());
                if (!VerifyBlockAt(request, offset, buffer.data(), count)) {
                    server_ok.store(false);
                }
                offset += count;
                server_received.store(offset);
            }
            if (r.code == Code::kClosed) {
                break;
            }
            if (r.code == Code::kError) {
                server_ok.store(false);
                break;
            }
        }
    });

    long rss_max = rss_start;
    long rss_warm = -1;
    int fd_max = fd_base;
    const std::uint64_t deadline = start + static_cast<std::uint64_t>(seconds) * 1000;
    while (NowMs() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        const long rss = ProcStatusKb("VmRSS:");
        const int fds = FdCount();
        rss_max = std::max(rss_max, rss);
        if (rss_warm < 0) {
            rss_warm = rss;
        }
        fd_max = std::max(fd_max, fds);
    }
    stop.store(true);
    client_writer.join();
    server_writer.join();
    client_reader.join();
    server_reader.join();
    relay_thread.join();
    const std::uint64_t elapsed = NowMs() - start;
    const double cpu_elapsed = CpuSeconds() - cpu_start;

    CHECK(result.completed());
    CHECK(client_ok.load());
    CHECK(server_ok.load());
    // 方向容易写反：client_sent 是「请求」，应等于 server_received；
    // client_received 是「响应」，应等于 server_sent。
    CHECK_EQ(client_sent.load(), server_received.load());
    CHECK_EQ(server_sent.load(), client_received.load());
    CHECK(client_sent.load() > 0);
    CHECK_EQ(fd_max, fd_base + 4);  // 两个对端 + 中继两端，共 4 条 socket。
    (void)client_side.dialer->Close();
    (void)server_side.acceptor->Close();
    CHECK_EQ(FdCount(), fd_base);
    if (rss_warm > 0) {
        CHECK(rss_max - rss_warm < 16 * 1024);
    }
    std::printf(
        "[DATA] long run: %llu ms, sent=%.1f MiB/%.1f MiB, recv=%.1f MiB/%.1f MiB, "
        "rss start=%ldKB warm=%ldKB max=%ldKB, fd max=%d, cpu=%.2fs\n",
        static_cast<unsigned long long>(elapsed),
        double(client_sent.load()) / (1024.0 * 1024.0),
        double(server_sent.load()) / (1024.0 * 1024.0),
        double(client_received.load()) / (1024.0 * 1024.0),
        double(server_received.load()) / (1024.0 * 1024.0), rss_start, rss_warm, rss_max, fd_max,
        cpu_elapsed);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

struct Config {
    double scale = 1.0;
    std::string mode = "all";
    std::uint32_t seed = 20261029U;
    int long_seconds = 0;
};

Config ParseArgs(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--scale=", 0) == 0) {
            config.scale = std::stod(argument.substr(8));
        } else if (argument.rfind("--mode=", 0) == 0) {
            config.mode = argument.substr(7);
        } else if (argument.rfind("--seed=", 0) == 0) {
            config.seed = static_cast<std::uint32_t>(std::stoul(argument.substr(7)));
        } else if (argument.rfind("--long=", 0) == 0) {
            config.long_seconds = std::stoi(argument.substr(7));
        } else if (!argument.empty() && argument[0] != '-') {
            config.scale = std::stod(argument);
        } else {
            std::printf("[WARN] unknown argument: %s\n", argument.c_str());
        }
    }
    if (config.scale <= 0.0) {
        config.scale = 1.0;
    }
    return config;
}

bool WantsMode(const Config& config, const char* name) {
    return config.mode == "all" || config.mode == name;
}

}  // namespace

int main(int argc, char** argv) {
    const Config config = ParseArgs(argc, argv);
    std::printf("audit_relay_stress: mode=%s scale=%.3f seed=%u long=%ds\n", config.mode.c_str(),
                config.scale, config.seed, config.long_seconds);
    std::printf("[ENV] cpus=%u rss=%ldKB fd=%d\n",
                static_cast<unsigned>(std::thread::hardware_concurrency()),
                ProcStatusKb("VmRSS:"), FdCount());

    if (WantsMode(config, "throughput")) {
        TestThroughput(config.scale);
        TestBackpressureBenchmark(config.scale);
    }
    if (WantsMode(config, "concurrent")) {
        TestConcurrentMemoryRelays(config.scale);
    }
    if (WantsMode(config, "tcp")) {
        TestConcurrentTcpRelays(config.scale);
    }
    if (WantsMode(config, "random")) {
        RandomChaosRun(std::max<std::size_t>(6, static_cast<std::size_t>(1200.0 * config.scale)),
                       config.seed);
    }
    if (WantsMode(config, "scenarios")) {
        TestSlowButMoving();
        TestFinAfterDrain();
        TestIdleResetByTraffic();
        TestNormalizeBoundaries();
        TestErrorMapping();
        TestStatsAggregation();
        TestSlowConsumerTcp();
    }
    if (WantsMode(config, "leaks")) {
        TestFdCycles(config.scale);
    }
    if (config.mode == "all" || config.mode == "long") {
        TestLongRun(config.long_seconds, config.scale);
    }
    if (config.mode == "bp") {
        TestBackpressureBenchmark(config.scale);
    }

    std::printf("[SUMMARY] checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
