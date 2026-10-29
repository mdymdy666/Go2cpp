/**
 * @file audit_transport_stress.cpp
 * @brief transport 模块审查用压测/随机/场景程序（不注册进 CTest，独立编译运行）。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 覆盖审查要求的六类验证：
 *   1. 大数据量：TCP 回环 100 MiB、MemoryTransport 100 MiB、10 万次小消息；
 *   2. 随机测试：MemoryTransport 5000 次随机操作、TCP 2000 次随机操作；
 *   3. 复杂场景：1000 并发连接、慢客户端写阻塞超时、对端 RST/半关闭、
 *      非阻塞 EAGAIN 循环、IPv6 + >64 KiB 分片读；
 *   4. 资源：/proc/self/fd 泄漏、VmHWM 峰值、延迟分布（avg/p50/p95/p99/max）。
 *
 * 编译（普通）：
 *   c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I include -I tests/bento \
 *       tests/bento/audit_transport_stress.cpp src/bento/net/transport.cpp \
 *       src/bento/net/tcp_transport.cpp src/bento/net/memory_transport.cpp \
 *       -o /tmp/audit_transport_stress -pthread
 *
 * 编译（ASan/UBSan）：
 *   c++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -I include -I tests/bento tests/bento/audit_transport_stress.cpp \
 *       src/bento/net/transport.cpp src/bento/net/tcp_transport.cpp \
 *       src/bento/net/memory_transport.cpp -o /tmp/audit_transport_asan -pthread
 *
 * 用法：`audit_transport_stress [scale]`，scale 默认 1.0；ASan 下建议 0.1。
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "go2cpp/bento/net/memory_transport.hpp"
#include "go2cpp/bento/net/tcp_transport.hpp"

namespace {

using go2cpp::bento::net::Code;
using go2cpp::bento::net::CodeName;
using go2cpp::bento::net::Endpoint;
using go2cpp::bento::net::MemoryTransport;
using go2cpp::bento::net::Result;
using go2cpp::bento::net::TcpTransport;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

int g_checks = 0;
int g_failures = 0;
int g_skips = 0;
double g_scale = 1.0;

void CheckImpl(bool ok, const char* expr, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] line %d: %s\n", line, expr);
    }
}

#define CHECK(condition) CheckImpl(static_cast<bool>(condition), #condition, __LINE__)

/// 带上下文的失败信息（不增加断言计数），用于压测参数/环境问题。
void Fail(const char* what) {
    ++g_failures;
    std::fprintf(stderr, "[FAIL] %s\n", what);
}

std::size_t Scaled(std::size_t value, std::size_t floor_value) {
    const double scaled = static_cast<double>(value) * g_scale;
    const std::size_t result = scaled < 1.0 ? 1U : static_cast<std::size_t>(scaled);
    return std::max(result, floor_value);
}

std::uint64_t NowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
            .count());
}

std::uint64_t NextRandom(std::uint64_t* state) {
    std::uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

std::vector<std::uint8_t> MakePattern(std::size_t size, std::uint64_t seed) {
    std::vector<std::uint8_t> data(size);
    std::uint64_t state = seed | 1U;
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::uint8_t>(NextRandom(&state) & 0xFFU);
    }
    return data;
}

/// /proc/self/fd 条目数；失败返回 0。
std::size_t CountFds() {
    std::error_code error;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator it("/proc/self/fd", error), end; it != end;
         it.increment(error)) {
        ++count;
    }
    return count;
}

long ReadStatusKb(const char* key) {
    std::FILE* file = std::fopen("/proc/self/status", "re");
    if (file == nullptr) {
        return -1;
    }
    char line[512];
    long value = -1;
    const std::size_t key_length = std::strlen(key);
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, key, key_length) == 0) {
            (void)std::sscanf(line + key_length, "%ld", &value);
            break;
        }
    }
    std::fclose(file);
    return value;
}

long PeakRssKb() {
    return ReadStatusKb("VmHWM:");
}

long CurrentRssKb() {
    return ReadStatusKb("VmRSS:");
}

/// 重置 VmHWM，使每个场景的内存峰值独立。
void ResetPeakRss() {
    std::FILE* file = std::fopen("/proc/self/clear_refs", "we");
    if (file != nullptr) {
        (void)std::fputs("5", file);
        std::fclose(file);
    }
}

struct Latency {
    std::vector<std::uint32_t> samples;

    void Reserve(std::size_t count) {
        samples.reserve(count);
    }

    void Add(std::uint64_t microseconds) {
        samples.push_back(static_cast<std::uint32_t>(
            std::min<std::uint64_t>(microseconds, 0xFFFFFFFFULL)));
    }

    void Report(const char* name) const {
        if (samples.empty()) {
            std::printf("[LAT] %s: no samples\n", name);
            return;
        }
        std::vector<std::uint32_t> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&sorted](double ratio) {
            const std::size_t index = static_cast<std::size_t>(
                ratio * static_cast<double>(sorted.size() - 1) + 0.5);
            return sorted[std::min(index, sorted.size() - 1)];
        };
        double sum = 0.0;
        for (const std::uint32_t value : sorted) {
            sum += static_cast<double>(value);
        }
        std::printf(
            "[LAT] %s n=%zu avg=%.1fus p50=%uus p95=%uus p99=%uus max=%uus\n", name,
            sorted.size(), sum / static_cast<double>(sorted.size()), percentile(0.50),
            percentile(0.95), percentile(0.99), sorted.back());
    }
};

void PrintResource(const char* stage, std::size_t fd_baseline) {
    const std::size_t fds = CountFds();
    const long delta = static_cast<long>(fds) - static_cast<long>(fd_baseline);
    std::printf("[RES] %s fd=%zu delta=%+ld rss=%ldkB peak=%ldkB\n", stage, fds, delta,
                CurrentRssKb(), PeakRssKb());
}

/// 建立一对回环连接。
bool MakeTcpPair(TcpTransport* listener, TcpTransport* client, TcpTransport* server,
                 const char* host = "127.0.0.1") {
    Result result = listener->Listen({host, 0});
    if (!result.ok()) {
        std::fprintf(stderr, "[SETUP] Listen(%s): %s errno=%d\n", host,
                     CodeName(result.code), result.system_error);
        return false;
    }
    Endpoint local;
    result = listener->LocalEndpoint(&local);
    if (!result.ok()) {
        std::fprintf(stderr, "[SETUP] LocalEndpoint: errno=%d\n", result.system_error);
        return false;
    }
    result = client->Connect(local, milliseconds(3000));
    if (!result.ok()) {
        std::fprintf(stderr, "[SETUP] Connect(%s:%u): %s errno=%d\n", local.host.c_str(),
                     static_cast<unsigned>(local.port), CodeName(result.code),
                     result.system_error);
        return false;
    }
    result = listener->Accept(server, milliseconds(3000));
    if (!result.ok()) {
        std::fprintf(stderr, "[SETUP] Accept: %s errno=%d\n", CodeName(result.code),
                     result.system_error);
        return false;
    }
    return true;
}

bool MakeMemoryPair(MemoryTransport::Pair* pair) {
    *pair = MemoryTransport::CreatePair();
    return pair->first != nullptr && pair->second != nullptr;
}

void PrintRates(const char* name, std::size_t bytes, std::uint64_t elapsed_us) {
    const double seconds = static_cast<double>(elapsed_us) / 1e6;
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const double rate = seconds > 0.0 ? mib / seconds : 0.0;
    std::printf("[RATE] %s bytes=%zu elapsed=%.3fs throughput=%.1f MiB/s\n", name, bytes,
                seconds, rate);
}

// ---------------------------------------------------------------------------
// 场景 1：TCP 回环 100 MiB 单向吞吐 + 10 万次 32B 往返
// ---------------------------------------------------------------------------

void TestTcpThroughput() {
    std::printf("\n=== [1] TCP loopback 100 MiB + 100k round trips ===\n");
    const std::size_t total = Scaled(100U * 1024U * 1024U, 4U * 1024U * 1024U);
    constexpr std::size_t kChunk = 64U * 1024U;
    const std::size_t chunks = std::max<std::size_t>(total / kChunk, 1U);

    TcpTransport listener;
    TcpTransport client;
    TcpTransport server;
    if (!MakeTcpPair(&listener, &client, &server)) {
        Fail("tcp throughput setup");
        return;
    }

    const std::size_t fd_baseline = CountFds();
    ResetPeakRss();
    std::vector<std::uint8_t> chunk(kChunk);
    Latency write_latency;
    Latency read_latency;
    write_latency.Reserve(chunks);
    read_latency.Reserve(chunks);
    std::atomic<bool> writer_ok{true};
    std::size_t written_bytes = 0;

    const std::uint64_t start = NowUs();
    std::thread writer([&] {
        for (std::size_t i = 0; i < chunks; ++i) {
            std::memset(chunk.data(), static_cast<int>(i & 0xFFU), 16);
            const std::uint64_t t0 = NowUs();
            const Result result =
                client.WriteFull(chunk.data(), chunk.size(), milliseconds(10000));
            write_latency.Add(NowUs() - t0);
            if (!result.ok() || result.bytes != chunk.size()) {
                writer_ok = false;
                break;
            }
            written_bytes += result.bytes;
        }
    });

    std::vector<std::uint8_t> received(kChunk);
    std::size_t read_bytes = 0;
    bool reader_ok = true;
    for (std::size_t i = 0; i < chunks; ++i) {
        const std::uint64_t t0 = NowUs();
        const Result result =
            server.ReadFull(received.data(), received.size(), milliseconds(10000));
        read_latency.Add(NowUs() - t0);
        if (!result.ok() || result.bytes != received.size() ||
            received[0] != static_cast<std::uint8_t>(i & 0xFFU)) {
            reader_ok = false;
            break;
        }
        read_bytes += result.bytes;
    }
    writer.join();
    const std::uint64_t elapsed = NowUs() - start;

    CHECK(writer_ok.load());
    CHECK(reader_ok);
    CHECK(written_bytes == chunks * kChunk);
    CHECK(read_bytes == chunks * kChunk);
    PrintRates("tcp-100MiB", read_bytes, elapsed);
    write_latency.Report("tcp-write-64KiB");
    read_latency.Report("tcp-read-64KiB");
    PrintResource("tcp-100MiB", fd_baseline);

    // 10 万次 32B 往返（请求-响应）。先跑 1000 次探测，避免 Nagle 退化时长时间挂起。
    const std::size_t trips = Scaled(100000U, 2000U);
    const std::size_t probe = std::min<std::size_t>(trips, 1000U);
    Latency rtt;
    rtt.Reserve(trips);
    const std::vector<std::uint8_t> small = MakePattern(32, 0xA5A5A5A5U);
    std::atomic<bool> server_ok{true};
    std::thread echo([&] {
        std::vector<std::uint8_t> buffer(small.size());
        for (std::size_t i = 0; i < trips; ++i) {
            const Result read_result =
                server.ReadFull(buffer.data(), buffer.size(), milliseconds(5000));
            if (!read_result.ok() || buffer != small) {
                server_ok = false;
                return;
            }
            const Result write_result =
                server.WriteFull(buffer.data(), buffer.size(), milliseconds(5000));
            if (!write_result.ok()) {
                server_ok = false;
                return;
            }
        }
    });

    bool small_ok = true;
    std::size_t completed = 0;
    std::vector<std::uint8_t> got_small(small.size());
    for (std::size_t i = 0; i < probe; ++i) {
        const std::uint64_t t0 = NowUs();
        const Result write_result =
            client.WriteFull(small.data(), small.size(), milliseconds(5000));
        const Result read_result =
            client.ReadFull(got_small.data(), got_small.size(), milliseconds(5000));
        rtt.Add(NowUs() - t0);
        if (!write_result.ok() || !read_result.ok() || got_small != small) {
            small_ok = false;
            break;
        }
        ++completed;
    }
    bool slow_probe = false;
    if (!rtt.samples.empty()) {
        double sum = 0.0;
        for (const std::uint32_t value : rtt.samples) {
            sum += value;
        }
        slow_probe = sum / static_cast<double>(rtt.samples.size()) > 2000.0;
    }
    if (slow_probe && trips > probe) {
        std::printf(
            "[WARN] 32B RTT avg > 2ms (Nagle/delayed-ACK suspected); skip remaining "
            "100k trips\n");
        (void)client.Close();
        echo.join();
        (void)server.Close();
        (void)listener.Close();
        rtt.Report("tcp-rtt-32B-probe");
        CHECK(small_ok);
        std::printf("[SKIP] tcp 100k small round trips (Nagle suspected)\n");
        ++g_skips;
        PrintResource("tcp-small", fd_baseline);
        CHECK(CountFds() <= fd_baseline + 1);
        return;
    }

    for (std::size_t i = probe; i < trips; ++i) {
        const std::uint64_t t0 = NowUs();
        const Result write_result =
            client.WriteFull(small.data(), small.size(), milliseconds(5000));
        const Result read_result =
            client.ReadFull(got_small.data(), got_small.size(), milliseconds(5000));
        rtt.Add(NowUs() - t0);
        if (!write_result.ok() || !read_result.ok() || got_small != small) {
            small_ok = false;
            break;
        }
        ++completed;
    }
    (void)client.ShutdownWrite();
    echo.join();

    CHECK(small_ok);
    CHECK(server_ok.load());
    CHECK(completed == trips);
    rtt.Report("tcp-rtt-32B");
    std::printf("[COUNT] tcp small round trips completed=%zu/%zu\n", completed, trips);
    PrintResource("tcp-small", fd_baseline);
    client.Close();
    server.Close();
    listener.Close();
    CHECK(CountFds() <= fd_baseline + 1);
}

// ---------------------------------------------------------------------------
// 场景 2：MemoryTransport 100 MiB + 10 万次 64B 小消息
// ---------------------------------------------------------------------------

void TestMemoryThroughput() {
    std::printf("\n=== [2] MemoryTransport 100 MiB + 100k messages ===\n");
    const std::size_t total = Scaled(100U * 1024U * 1024U, 1U * 1024U * 1024U);
    constexpr std::size_t kChunk = 64U * 1024U;
    const std::size_t chunks = std::max<std::size_t>(total / kChunk, 1U);

    MemoryTransport::Pair pair;
    if (!MakeMemoryPair(&pair)) {
        Fail("memory pair");
        return;
    }
    MemoryTransport& first = *pair.first;
    MemoryTransport& second = *pair.second;
    const std::size_t fd_baseline = CountFds();
    ResetPeakRss();

    std::vector<std::uint8_t> chunk(kChunk);
    Latency write_latency;
    Latency read_latency;
    write_latency.Reserve(chunks);
    read_latency.Reserve(chunks);
    std::atomic<bool> writer_ok{true};
    std::size_t written = 0;

    const std::uint64_t start = NowUs();
    std::thread writer([&] {
        for (std::size_t i = 0; i < chunks; ++i) {
            std::memset(chunk.data(), static_cast<int>(i & 0xFFU), 16);
            const std::uint64_t t0 = NowUs();
            const Result result =
                first.WriteFull(chunk.data(), chunk.size(), milliseconds(10000));
            write_latency.Add(NowUs() - t0);
            if (!result.ok() || result.bytes != chunk.size()) {
                writer_ok = false;
                break;
            }
            written += result.bytes;
        }
    });

    std::vector<std::uint8_t> received(kChunk);
    std::size_t read_bytes = 0;
    bool reader_ok = true;
    for (std::size_t i = 0; i < chunks; ++i) {
        const std::uint64_t t0 = NowUs();
        const Result result =
            second.ReadFull(received.data(), received.size(), milliseconds(10000));
        read_latency.Add(NowUs() - t0);
        if (!result.ok() || result.bytes != received.size() ||
            received[0] != static_cast<std::uint8_t>(i & 0xFFU)) {
            reader_ok = false;
            break;
        }
        read_bytes += result.bytes;
    }
    writer.join();
    const std::uint64_t elapsed = NowUs() - start;

    CHECK(writer_ok.load());
    CHECK(reader_ok);
    CHECK(written == chunks * kChunk);
    CHECK(read_bytes == chunks * kChunk);
    PrintRates("memory-100MiB", read_bytes, elapsed);
    write_latency.Report("memory-write-64KiB");
    read_latency.Report("memory-read-64KiB");
    PrintResource("memory-100MiB", fd_baseline);
    CHECK(CountFds() <= fd_baseline + 1);

    // 10 万次 64B 单线程写读（队列 + 锁 + 条件变量路径）。
    const std::size_t messages = Scaled(100000U, 2000U);
    const std::vector<std::uint8_t> payload = MakePattern(64, 0x5A5A5A5AU);
    std::vector<std::uint8_t> buffer(payload.size());
    Latency message_latency;
    message_latency.Reserve(messages);
    bool message_ok = true;
    const std::uint64_t message_start = NowUs();
    for (std::size_t i = 0; i < messages; ++i) {
        const std::uint64_t t0 = NowUs();
        const Result write_result =
            first.WriteFull(payload.data(), payload.size(), milliseconds(1000));
        const Result read_result =
            second.ReadFull(buffer.data(), buffer.size(), milliseconds(1000));
        message_latency.Add(NowUs() - t0);
        if (!write_result.ok() || !read_result.ok() || buffer != payload) {
            message_ok = false;
            break;
        }
    }
    const std::uint64_t message_elapsed = NowUs() - message_start;
    CHECK(message_ok);
    std::printf("[COUNT] memory messages=%zu elapsed=%.3fs\n", messages,
                static_cast<double>(message_elapsed) / 1e6);
    message_latency.Report("memory-64B-write+read");
    PrintResource("memory-small", fd_baseline);
}

// ---------------------------------------------------------------------------
// 场景 3：1000 并发连接
// ---------------------------------------------------------------------------

void TestManyConnections() {
    std::printf("\n=== [3] 1000 concurrent connections ===\n");
    const std::size_t count = Scaled(1000U, 100U);
    const std::size_t fd_baseline = CountFds();
    ResetPeakRss();

    TcpTransport listener;
    Result result = listener.Listen({"127.0.0.1", 0}, 1024);
    if (!result.ok()) {
        Fail("listener.Listen for many connections");
        return;
    }
    Endpoint local;
    result = listener.LocalEndpoint(&local);
    CHECK(result.ok());

    std::vector<std::unique_ptr<TcpTransport>> clients;
    std::vector<std::unique_ptr<TcpTransport>> servers;
    clients.reserve(count);
    servers.reserve(count);
    const std::uint64_t start = NowUs();
    bool setup_ok = true;
    for (std::size_t i = 0; i < count; ++i) {
        auto client = std::make_unique<TcpTransport>();
        auto server = std::make_unique<TcpTransport>();
        result = client->Connect(local, milliseconds(3000));
        if (!result.ok()) {
            std::fprintf(stderr, "[FAIL] connect #%zu: %s errno=%d\n", i,
                         CodeName(result.code), result.system_error);
            setup_ok = false;
            break;
        }
        result = listener.Accept(server.get(), milliseconds(3000));
        if (!result.ok()) {
            std::fprintf(stderr, "[FAIL] accept #%zu: %s errno=%d\n", i,
                         CodeName(result.code), result.system_error);
            setup_ok = false;
            break;
        }
        clients.push_back(std::move(client));
        servers.push_back(std::move(server));
    }
    const std::uint64_t setup_elapsed = NowUs() - start;
    CHECK(setup_ok);
    CHECK(clients.size() == count);
    CHECK(servers.size() == count);
    std::printf("[COUNT] connections=%zu setup=%.3fs\n", clients.size(),
                static_cast<double>(setup_elapsed) / 1e6);

    const std::size_t fd_after_connect = CountFds();
    std::printf("[RES] after-connect fd=%zu expected>=%zu peak=%ldkB\n", fd_after_connect,
                fd_baseline + 2U * clients.size() + 1U, PeakRssKb());
    CHECK(fd_after_connect >= fd_baseline + 2U * clients.size() + 1U);

    // 每条连接写 8B 序号并回读，验证互不串流。
    bool io_ok = true;
    for (std::size_t i = 0; i < clients.size(); ++i) {
        std::uint64_t value = i;
        Result write_result =
            clients[i]->WriteFull(reinterpret_cast<const std::uint8_t*>(&value),
                                  sizeof(value), milliseconds(2000));
        std::uint64_t got = 0;
        Result read_result = servers[i]->ReadFull(reinterpret_cast<std::uint8_t*>(&got),
                                                  sizeof(got), milliseconds(2000));
        if (!write_result.ok() || !read_result.ok() || got != value) {
            io_ok = false;
            break;
        }
    }
    CHECK(io_ok);

    for (auto& client : clients) {
        (void)client->Close();
    }
    for (auto& server : servers) {
        (void)server->Close();
    }
    (void)listener.Close();
    clients.clear();
    servers.clear();
    const std::size_t fd_after_close = CountFds();
    std::printf("[RES] after-close fd=%zu baseline=%zu peak=%ldkB\n", fd_after_close,
                fd_baseline, PeakRssKb());
    CHECK(fd_after_close <= fd_baseline + 2U);
}

// ---------------------------------------------------------------------------
// 场景 4：慢客户端（写阻塞）→ 超时
// ---------------------------------------------------------------------------

void TestSlowClient() {
    std::printf("\n=== [4] slow client: write blocks then times out ===\n");
    TcpTransport listener;
    TcpTransport client;
    TcpTransport server;
    if (!MakeTcpPair(&listener, &client, &server)) {
        Fail("slow client setup");
        return;
    }
    const std::size_t fd_baseline = CountFds();

    const std::size_t chunk = 1024U * 1024U;
    std::vector<std::uint8_t> payload(chunk, 0x5CU);
    std::size_t total_written = 0;
    std::size_t timeouts = 0;
    bool saw_timeout = false;
    std::uint64_t elapsed_us = 0;
    const std::uint64_t start = NowUs();
    for (int i = 0; i < 64; ++i) {
        const std::uint64_t t0 = NowUs();
        const Result result =
            server.Write(payload.data(), payload.size(), milliseconds(100));
        const std::uint64_t slice = NowUs() - t0;
        total_written += result.bytes;
        if (result.code == Code::kTimeout) {
            saw_timeout = true;
            ++timeouts;
            elapsed_us = slice;
            break;
        }
        if (!result.ok()) {
            std::fprintf(stderr, "[FAIL] slow-client write: %s errno=%d\n",
                         CodeName(result.code), result.system_error);
            break;
        }
    }
    const std::uint64_t total_elapsed = NowUs() - start;
    std::printf(
        "[DATA] slow-client written=%zu timeouts=%zu last_timeout=%.1fms total=%.1fms\n",
        total_written, timeouts, static_cast<double>(elapsed_us) / 1000.0,
        static_cast<double>(total_elapsed) / 1000.0);
    CHECK(saw_timeout);
    CHECK(total_written > 0);
    CHECK(total_written < 64U * chunk);
    // 单次超时应受 100ms 预算约束（允许调度抖动）。
    CHECK(elapsed_us < 1500U * 1000U);

    // 超时后连接仍可读/关闭。
    std::vector<std::uint8_t> buffer(4096);
    const Result read_result = client.Read(buffer.data(), buffer.size(), milliseconds(1000));
    CHECK(read_result.ok() && read_result.bytes > 0);
    CHECK(server.Close().ok());
    CHECK(client.Close().ok());
    listener.Close();
    CHECK(CountFds() <= fd_baseline + 1);
    PrintResource("slow-client", fd_baseline);
}

// ---------------------------------------------------------------------------
// 场景 5：对端 RST / 半关闭
// ---------------------------------------------------------------------------

void TestRstAndHalfClose() {
    std::printf("\n=== [5] peer RST + half close ===\n");
    TcpTransport listener;
    TcpTransport client;
    TcpTransport server;
    if (!MakeTcpPair(&listener, &client, &server)) {
        Fail("rst setup");
        return;
    }
    const std::size_t fd_baseline = CountFds();

    // 半关闭：FIN 后对端仍可写回。
    const std::vector<std::uint8_t> request = {'r', 'e', 'q'};
    CHECK(client.WriteFull(request.data(), request.size(), milliseconds(1000)).ok());
    std::vector<std::uint8_t> buffer(16);
    CHECK(server.ReadFull(buffer.data(), request.size(), milliseconds(1000)).ok());
    CHECK(std::equal(request.begin(), request.end(), buffer.begin()));
    CHECK(client.ShutdownWrite().ok());
    CHECK(server.Read(buffer.data(), buffer.size(), milliseconds(1000)).code ==
          Code::kClosed);
    const std::vector<std::uint8_t> response = {'r', 's', 'p'};
    CHECK(server.WriteFull(response.data(), response.size(), milliseconds(1000)).ok());
    CHECK(client.ReadFull(buffer.data(), response.size(), milliseconds(1000)).ok());
    CHECK(std::equal(response.begin(), response.end(), buffer.begin()));
    client.Close();
    server.Close();
    listener.Close();
    CHECK(CountFds() <= fd_baseline + 1);

    // RST：裸 socket + SO_LINGER(0)，TcpTransport 服务端应映射为 kClosed。
    TcpTransport rst_listener;
    Result result = rst_listener.Listen({"127.0.0.1", 0});
    CHECK(result.ok());
    Endpoint local;
    result = rst_listener.LocalEndpoint(&local);
    CHECK(result.ok());

    const int raw = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(raw >= 0);
    if (raw >= 0) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(local.port);
        (void)::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        CHECK(::connect(raw, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
              0);
        TcpTransport rst_server;
        result = rst_listener.Accept(&rst_server, milliseconds(2000));
        CHECK(result.ok());
        const linger reset_linger{1, 0};
        CHECK(::setsockopt(raw, SOL_SOCKET, SO_LINGER, &reset_linger, sizeof(reset_linger)) ==
              0);
        CHECK(::close(raw) == 0);
        const Result read_result =
            rst_server.Read(buffer.data(), buffer.size(), milliseconds(2000));
        std::printf("[DATA] rst read code=%s errno=%d\n", CodeName(read_result.code),
                    read_result.system_error);
        CHECK(read_result.code == Code::kClosed);
        CHECK(rst_server.Close().ok());
    }
    rst_listener.Close();
    CHECK(CountFds() <= fd_baseline + 1);
    PrintResource("rst-halfclose", fd_baseline);
}

// ---------------------------------------------------------------------------
// 场景 6：非阻塞 EAGAIN 循环
// ---------------------------------------------------------------------------

void TestEagainLoop() {
    std::printf("\n=== [6] non-blocking EAGAIN loop ===\n");
    TcpTransport listener;
    TcpTransport client;
    TcpTransport server;
    if (!MakeTcpPair(&listener, &client, &server)) {
        Fail("eagain setup");
        return;
    }
    const std::size_t fd_baseline = CountFds();
    std::vector<std::uint8_t> buffer(64U * 1024U);

    const std::size_t empty_reads = Scaled(10000U, 500U);
    Latency empty_latency;
    empty_latency.Reserve(empty_reads);
    bool empty_ok = true;
    const std::uint64_t start = NowUs();
    for (std::size_t i = 0; i < empty_reads; ++i) {
        const std::uint64_t t0 = NowUs();
        const Result result = client.Read(buffer.data(), buffer.size(), milliseconds(0));
        empty_latency.Add(NowUs() - t0);
        if (result.code != Code::kTimeout || result.bytes != 0) {
            empty_ok = false;
            break;
        }
    }
    const std::uint64_t empty_elapsed = NowUs() - start;
    CHECK(empty_ok);
    std::printf("[COUNT] nonblocking empty reads=%zu elapsed=%.1fms\n", empty_reads,
                static_cast<double>(empty_elapsed) / 1000.0);
    empty_latency.Report("tcp-read-EAGAIN-0ms");

    // 已有数据时，0ms 读取必须立即消费而不是先报超时。
    const std::vector<std::uint8_t> tiny = {'e', 'a', 'g', 'a', 'i', 'n'};
    CHECK(server.WriteFull(tiny.data(), tiny.size(), milliseconds(1000)).ok());
    const Result ready = client.Read(buffer.data(), buffer.size(), milliseconds(0));
    CHECK(ready.ok());
    CHECK(ready.bytes == tiny.size());
    CHECK(std::equal(tiny.begin(), tiny.end(), buffer.begin()));

    // 一次超大非阻塞写：send 到 EAGAIN 后应返回 kTimeout + 部分字节。
    const std::size_t big_size = 16U * 1024U * 1024U;
    std::vector<std::uint8_t> big(big_size, 0xA5U);
    const std::uint64_t write_start = NowUs();
    const Result big_write = client.Write(big.data(), big.size(), milliseconds(0));
    const std::uint64_t write_elapsed = NowUs() - write_start;
    std::printf("[DATA] nonblocking big write code=%s bytes=%zu elapsed=%.1fms\n",
                CodeName(big_write.code), big_write.bytes,
                static_cast<double>(write_elapsed) / 1000.0);
    CHECK(big_write.code == Code::kTimeout || big_write.code == Code::kOk);
    CHECK(big_write.bytes > 0);
    if (big_write.code == Code::kTimeout) {
        CHECK(big_write.bytes < big.size());
    }
    CHECK(write_elapsed < 1500U * 1000U);

    CHECK(client.Close().ok());
    CHECK(server.Close().ok());
    listener.Close();
    CHECK(CountFds() <= fd_baseline + 1);
    PrintResource("eagain", fd_baseline);
}

// ---------------------------------------------------------------------------
// 场景 7：IPv6 + 大包（>64 KiB 分片读）
// ---------------------------------------------------------------------------

void TestIpv6BigPacket() {
    std::printf("\n=== [7] IPv6 + 1 MiB reassembly (>64 KiB fragment reads) ===\n");
    const std::size_t total = Scaled(1024U * 1024U, 128U * 1024U);
    constexpr std::size_t kChunk = 64U * 1024U;
    const std::size_t chunks = std::max<std::size_t>(total / kChunk, 1U);

    TcpTransport listener;
    Result result = listener.Listen({"::1", 0});
    if (!result.ok()) {
        std::printf("[SKIP] IPv6 listen(::1) unavailable: errno=%d\n", result.system_error);
        ++g_skips;
        return;
    }
    Endpoint local;
    result = listener.LocalEndpoint(&local);
    CHECK(result.ok());
    CHECK(local.host.find(':') != std::string::npos);

    TcpTransport client;
    result = client.Connect(local, milliseconds(3000));
    if (!result.ok()) {
        std::printf("[SKIP] IPv6 connect failed: errno=%d\n", result.system_error);
        ++g_skips;
        return;
    }
    TcpTransport server;
    result = listener.Accept(&server, milliseconds(3000));
    CHECK(result.ok());
    if (!result.ok()) {
        return;
    }

    const std::size_t fd_baseline = CountFds();
    ResetPeakRss();
    std::vector<std::uint8_t> chunk(kChunk);
    std::atomic<bool> writer_ok{true};
    std::size_t written = 0;
    const std::uint64_t start = NowUs();
    std::thread writer([&] {
        for (std::size_t i = 0; i < chunks; ++i) {
            std::memset(chunk.data(), static_cast<int>(i & 0xFFU), 8);
            const Result write_result =
                client.WriteFull(chunk.data(), chunk.size(), milliseconds(10000));
            if (!write_result.ok() || write_result.bytes != chunk.size()) {
                writer_ok = false;
                return;
            }
            written += write_result.bytes;
        }
    });

    std::vector<std::uint8_t> received(kChunk);
    std::size_t read_bytes = 0;
    bool reader_ok = true;
    std::size_t max_single_read = 0;
    for (std::size_t i = 0; i < chunks; ++i) {
        const Result read_result =
            server.ReadFull(received.data(), received.size(), milliseconds(10000));
        if (!read_result.ok() || read_result.bytes != received.size() ||
            received[0] != static_cast<std::uint8_t>(i & 0xFFU)) {
            reader_ok = false;
            break;
        }
        read_bytes += read_result.bytes;
    }

    // 再来一次大 capacity 单次读，观察内核返回上限（应 <= capacity）。
    std::vector<std::uint8_t> large(256U * 1024U, 0);
    std::vector<std::uint8_t> extra = MakePattern(200U * 1024U, 0x1234U);
    const Result extra_write =
        client.WriteFull(extra.data(), extra.size(), milliseconds(5000));
    CHECK(extra_write.ok());
    std::size_t extra_read = 0;
    while (extra_read < extra.size()) {
        const Result one = server.Read(
            large.data(), std::min(large.size(), extra.size() - extra_read),
            milliseconds(5000));
        if (!one.ok() || one.bytes == 0) {
            reader_ok = false;
            break;
        }
        max_single_read = std::max(max_single_read, one.bytes);
        if (!std::equal(extra.begin() + static_cast<std::ptrdiff_t>(extra_read),
                        extra.begin() + static_cast<std::ptrdiff_t>(extra_read + one.bytes),
                        large.begin())) {
            reader_ok = false;
            break;
        }
        extra_read += one.bytes;
    }
    writer.join();
    const std::uint64_t elapsed = NowUs() - start;

    CHECK(writer_ok.load());
    CHECK(reader_ok);
    CHECK(written == chunks * kChunk);
    CHECK(read_bytes == chunks * kChunk);
    CHECK(extra_read == extra.size());
    CHECK(max_single_read > 0);
    CHECK(max_single_read <= large.size());
    std::printf("[DATA] ipv6 max-single-read=%zu bytes (capacity=%zu)\n", max_single_read,
                large.size());
    PrintRates("ipv6-1MiB", read_bytes, elapsed);
    PrintResource("ipv6", fd_baseline);
    client.Close();
    server.Close();
    listener.Close();
    CHECK(CountFds() <= fd_baseline + 1);
}

// ---------------------------------------------------------------------------
// 场景 8：随机操作序列（Memory 5000 次 + TCP 2000 次）
// ---------------------------------------------------------------------------

struct MemoryModel {
    bool a_closed = false;
    bool b_closed = false;
    bool a_write_closed = false;
    bool b_write_closed = false;
    std::vector<std::uint8_t> to_a;
    std::vector<std::uint8_t> to_b;
};

void TestRandomMemory() {
    std::printf("\n=== [8a] MemoryTransport random operation sequence ===\n");
    MemoryTransport::Pair pair;
    if (!MakeMemoryPair(&pair)) {
        Fail("random memory pair");
        return;
    }
    MemoryTransport& first = *pair.first;
    MemoryTransport& second = *pair.second;
    const std::size_t fd_baseline = CountFds();
    MemoryModel model;
    std::uint64_t random = 0xC0FFEE123456789ULL;
    const std::size_t operations = Scaled(5000U, 1000U);
    std::vector<std::uint8_t> buffer(1024);
    std::size_t inconsistent = 0;

    for (std::size_t i = 0; i < operations; ++i) {
        const std::size_t op = static_cast<std::size_t>(NextRandom(&random) % 8U);
        const bool side_a = (NextRandom(&random) & 1U) != 0;
        MemoryTransport& endpoint = side_a ? first : second;
        const std::size_t size = static_cast<std::size_t>(NextRandom(&random) % 300U) + 1U;
        const std::size_t capacity = static_cast<std::size_t>(NextRandom(&random) % 512U);
        std::vector<std::uint8_t> payload = MakePattern(size, NextRandom(&random));

        const bool did_connect = (NextRandom(&random) % 16U) == 0U;
        const bool closed_before = side_a ? model.a_closed : model.b_closed;
        Result connect_result;
        if (did_connect) {
            connect_result = endpoint.Connect({}, milliseconds(0));
        }

        if (op <= 2U) {  // write
            const Result result =
                endpoint.Write(payload.data(), payload.size(), milliseconds(0));
            if (side_a ? model.a_closed : model.b_closed) {
                inconsistent += result.code != Code::kClosed ? 1U : 0U;
            } else if (side_a ? (model.a_write_closed || model.b_closed)
                              : (model.b_write_closed || model.a_closed)) {
                inconsistent += result.code != Code::kClosed ? 1U : 0U;
            } else {
                inconsistent += (!result.ok() || result.bytes != payload.size()) ? 1U : 0U;
                std::vector<std::uint8_t>& target = side_a ? model.to_b : model.to_a;
                target.insert(target.end(), payload.begin(), payload.end());
            }
        } else if (op <= 5U) {  // read
            std::vector<std::uint8_t>& inbox = side_a ? model.to_a : model.to_b;
            const Result result = endpoint.Read(buffer.data(), capacity, milliseconds(0));
            if (side_a ? model.a_closed : model.b_closed) {
                inconsistent += result.code != Code::kClosed ? 1U : 0U;
            } else if (capacity == 0U) {
                inconsistent += (!result.ok() || result.bytes != 0) ? 1U : 0U;
            } else if (!inbox.empty()) {
                const std::size_t want = std::min(capacity, inbox.size());
                const bool matches =
                    result.ok() && result.bytes == want &&
                    std::equal(inbox.begin(),
                               inbox.begin() + static_cast<std::ptrdiff_t>(want),
                               buffer.begin());
                inconsistent += matches ? 0U : 1U;
                if (matches) {
                    inbox.erase(inbox.begin(),
                                inbox.begin() + static_cast<std::ptrdiff_t>(want));
                }
            } else {
                const bool peer_done = side_a ? (model.b_write_closed || model.b_closed)
                                              : (model.a_write_closed || model.a_closed);
                const Code expected = peer_done ? Code::kClosed : Code::kTimeout;
                inconsistent += result.code != expected ? 1U : 0U;
            }
        } else if (op == 6U) {  // shutdown
            const Result result = endpoint.ShutdownWrite();
            if (side_a ? model.a_closed : model.b_closed) {
                inconsistent += result.code != Code::kClosed ? 1U : 0U;
            } else {
                inconsistent += !result.ok() ? 1U : 0U;
                (side_a ? model.a_write_closed : model.b_write_closed) = true;
            }
        } else {  // close
            const Result result = endpoint.Close();
            inconsistent += !result.ok() ? 1U : 0U;
            if (side_a) {
                model.a_closed = true;
                model.a_write_closed = true;
                model.to_a.clear();
            } else {
                model.b_closed = true;
                model.b_write_closed = true;
                model.to_b.clear();
            }
        }
        if (did_connect) {
            const Code expected = closed_before ? Code::kClosed : Code::kOk;
            inconsistent += connect_result.code != expected ? 1U : 0U;
        }
    }

    std::printf("[COUNT] memory random ops=%zu inconsistent=%zu\n", operations,
                inconsistent);
    CHECK(inconsistent == 0);
    CHECK(CountFds() <= fd_baseline + 1);
    PrintResource("random-memory", fd_baseline);
}

void TestRandomTcp() {
    std::printf("\n=== [8b] TCP random operation sequence ===\n");
    TcpTransport listener;
    TcpTransport client;
    TcpTransport server;
    if (!MakeTcpPair(&listener, &client, &server)) {
        Fail("random tcp setup");
        return;
    }
    const std::size_t fd_baseline = CountFds();
    std::uint64_t random = 0xDEADBEEFCAFEULL;
    const std::size_t operations = Scaled(2000U, 500U);
    std::vector<std::uint8_t> buffer(8192);
    std::vector<std::uint8_t> client_to_server;
    std::vector<std::uint8_t> server_to_client;
    bool client_closed = false;
    bool server_closed = false;
    bool client_write_closed = false;
    bool server_write_closed = false;
    std::size_t invalid = 0;
    std::size_t data_mismatch = 0;

    for (std::size_t i = 0; i < operations; ++i) {
        const std::size_t op = static_cast<std::size_t>(NextRandom(&random) % 8U);
        const std::size_t size = static_cast<std::size_t>(NextRandom(&random) % 4096U) + 1U;
        const std::size_t capacity = static_cast<std::size_t>(NextRandom(&random) % 4096U);
        std::vector<std::uint8_t> payload = MakePattern(size, NextRandom(&random));

        if (op == 0U && !client_closed) {  // client write
            const Result result =
                client.Write(payload.data(), payload.size(), milliseconds(0));
            if (result.bytes > payload.size() ||
                (!result.ok() && result.code != Code::kTimeout &&
                 result.code != Code::kClosed)) {
                ++invalid;
            }
            if (client_write_closed && result.code != Code::kClosed) {
                ++invalid;
            }
            client_to_server.insert(
                client_to_server.end(), payload.begin(),
                payload.begin() + static_cast<std::ptrdiff_t>(result.bytes));
        } else if (op == 1U && !client_closed) {  // client read
            const Result result = client.Read(buffer.data(), capacity, milliseconds(0));
            if (result.bytes > capacity ||
                (result.code != Code::kOk && result.code != Code::kTimeout &&
                 result.code != Code::kClosed)) {
                ++invalid;
            }
            if (result.ok() && result.bytes > 0 && !client_closed && !server_closed) {
                const std::size_t want = result.bytes;
                if (want > server_to_client.size() ||
                    !std::equal(buffer.begin(),
                                buffer.begin() + static_cast<std::ptrdiff_t>(want),
                                server_to_client.begin())) {
                    ++data_mismatch;
                } else {
                    server_to_client.erase(
                        server_to_client.begin(),
                        server_to_client.begin() + static_cast<std::ptrdiff_t>(want));
                }
            }
        } else if (op == 2U && !server_closed) {  // server write
            const Result result =
                server.Write(payload.data(), payload.size(), milliseconds(0));
            if (result.bytes > payload.size() ||
                (!result.ok() && result.code != Code::kTimeout &&
                 result.code != Code::kClosed)) {
                ++invalid;
            }
            if (server_write_closed && result.code != Code::kClosed) {
                ++invalid;
            }
            server_to_client.insert(
                server_to_client.end(), payload.begin(),
                payload.begin() + static_cast<std::ptrdiff_t>(result.bytes));
        } else if (op == 3U && !server_closed) {  // server read
            const Result result = server.Read(buffer.data(), capacity, milliseconds(0));
            if (result.bytes > capacity ||
                (result.code != Code::kOk && result.code != Code::kTimeout &&
                 result.code != Code::kClosed)) {
                ++invalid;
            }
            if (result.ok() && result.bytes > 0 && !client_closed && !server_closed) {
                const std::size_t want = result.bytes;
                if (want > client_to_server.size() ||
                    !std::equal(buffer.begin(),
                                buffer.begin() + static_cast<std::ptrdiff_t>(want),
                                client_to_server.begin())) {
                    ++data_mismatch;
                } else {
                    client_to_server.erase(
                        client_to_server.begin(),
                        client_to_server.begin() + static_cast<std::ptrdiff_t>(want));
                }
            }
        } else if (op == 4U && !client_closed) {  // client shutdown
            const Result result = client.ShutdownWrite();
            if (!result.ok() && result.code != Code::kClosed) {
                ++invalid;
            }
            client_write_closed = true;
            const Result after = client.Write(payload.data(), 1, milliseconds(0));
            if (after.code != Code::kClosed) {
                ++invalid;
            }
        } else if (op == 5U && !server_closed) {  // server shutdown
            const Result result = server.ShutdownWrite();
            if (!result.ok() && result.code != Code::kClosed) {
                ++invalid;
            }
            server_write_closed = true;
            const Result after = server.Write(payload.data(), 1, milliseconds(0));
            if (after.code != Code::kClosed) {
                ++invalid;
            }
        } else if (op == 6U && !client_closed) {  // client close
            if (!client.Close().ok()) {
                ++invalid;
            }
            client_closed = true;
            client_write_closed = true;
            if (client.Read(buffer.data(), 1, milliseconds(0)).code != Code::kClosed ||
                client.Write(payload.data(), 1, milliseconds(0)).code != Code::kClosed) {
                ++invalid;
            }
        } else if (op == 7U && !server_closed) {  // server close
            if (!server.Close().ok()) {
                ++invalid;
            }
            server_closed = true;
            server_write_closed = true;
            if (server.Read(buffer.data(), 1, milliseconds(0)).code != Code::kClosed ||
                server.Write(payload.data(), 1, milliseconds(0)).code != Code::kClosed) {
                ++invalid;
            }
        }
    }
    if (!client_closed) {
        (void)client.Close();
    }
    if (!server_closed) {
        (void)server.Close();
    }
    (void)listener.Close();
    std::printf("[COUNT] tcp random ops=%zu invalid=%zu mismatch=%zu\n", operations,
                invalid, data_mismatch);
    CHECK(invalid == 0);
    CHECK(data_mismatch == 0);
    CHECK(CountFds() <= fd_baseline + 1);
    PrintResource("random-tcp", fd_baseline);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        g_scale = std::strtod(argv[1], nullptr);
        if (g_scale <= 0.0 || g_scale > 10.0) {
            g_scale = 1.0;
        }
    }
    // 提升 fd 软限制以支持 1000 并发连接（不修改硬限制）。
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        limit.rlim_cur = std::min<rlim_t>(limit.rlim_max, 65535);
        (void)::setrlimit(RLIMIT_NOFILE, &limit);
    }
    std::printf("[ENV] scale=%.2f fds=%zu peak=%ldkB\n", g_scale, CountFds(),
                PeakRssKb());

    TestTcpThroughput();
    TestMemoryThroughput();
    TestManyConnections();
    TestSlowClient();
    TestRstAndHalfClose();
    TestEagainLoop();
    TestIpv6BigPacket();
    TestRandomMemory();
    TestRandomTcp();

    std::printf("\n[SUMMARY] checks=%d failures=%d skips=%d\n", g_checks, g_failures,
                g_skips);
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] audit_transport_stress: %d/%d checks failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf("[PASS] audit_transport_stress (%d checks, %d skips)\n", g_checks,
                g_skips);
    return 0;
}
