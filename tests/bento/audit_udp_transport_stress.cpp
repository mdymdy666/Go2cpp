/**
 * @file audit_udp_transport_stress.cpp
 * @brief `UdpTransport` 独立审计程序：压测 / 随机 / 5 场景 / 并发 / fd / CPU。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 与 `audit_*_stress.cpp` 一致，本程序**不注册进 CTest**，用
 *          `.build-bento-wsl` 之外的单文件编译命令构建（见审计文档）。
 *
 *          模式：
 *            bench    100k+ 数据报的单向洪泛与请求-响应 RTT、getaddrinfo 成本
 *            random   2000 轮随机大小/缓冲/超时/地址，断言边界与截断标记
 *            scenario 5 个真实场景（回环回显、截断/大包、超时、并发发送、IPv6）
 *            resolve  getaddrinfo vs inet_pton 的解析成本微基准
 *            all      以上全部
 *
 *          退出码：0 全部通过；1 有断言失败；2 参数错误。
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "go2cpp/bento/net/udp_transport.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using go2cpp::bento::net::Code;
using go2cpp::bento::net::Endpoint;
using go2cpp::bento::net::Result;
using go2cpp::bento::net::UdpTransport;

int g_checks = 0;
int g_failures = 0;
int g_skips = 0;

#define AUDIT_CHECK(condition)                                                          \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                               \
        }                                                                               \
    } while (0)

void CheckMsg(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "[FAIL] %s\n", what);
        ++g_failures;
    }
}

void Skip(const char* reason) {
    std::printf("[SKIP] %s\n", reason);
    ++g_skips;
}

double Seconds(Clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

std::uint64_t NowMicros() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<microseconds>(Clock::now().time_since_epoch()).count());
}

std::uint64_t NextRandom(std::uint64_t* state) noexcept {
    std::uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

/// /proc/self/fd 条目数（opendir 自身占一个，扣掉）。
int CountFds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (const dirent* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    ::closedir(dir);
    return count - 1;
}

/// 读 /proc/self/status 里的 VmRSS / VmHWM（KB）。
long ProcStatusKb(const char* key) {
    FILE* file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return -1;
    }
    char line[256];
    long value = -1;
    const std::size_t key_length = std::strlen(key);
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, key, key_length) == 0) {
            if (std::sscanf(line + key_length, " %ld", &value) != 1) {
                value = -1;
            }
            break;
        }
    }
    std::fclose(file);
    return value;
}

struct CpuTimes {
    double user = 0.0;
    double sys = 0.0;

    double total() const {
        return user + sys;
    }
};

CpuTimes ReadCpu() {
    rusage usage{};
    (void)::getrusage(RUSAGE_SELF, &usage);
    CpuTimes times;
    times.user = static_cast<double>(usage.ru_utime.tv_sec) +
                 static_cast<double>(usage.ru_utime.tv_usec) / 1e6;
    times.sys = static_cast<double>(usage.ru_stime.tv_sec) +
                static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
    return times;
}

struct UdpStats {
    std::uint64_t in_datagrams = 0;
    std::uint64_t out_datagrams = 0;
    std::uint64_t in_errors = 0;
    std::uint64_t rcvbuf_errors = 0;
    std::uint64_t sndbuf_errors = 0;
};

/// 读 /proc/net/snmp 的 Udp 计数，用于证明丢包发生在内核接收缓冲。
bool ReadUdpStats(UdpStats* out) {
    FILE* file = std::fopen("/proc/net/snmp", "r");
    if (file == nullptr) {
        return false;
    }
    char line[1024];
    bool header_seen = false;
    bool parsed = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "Udp:", 4) != 0) {
            continue;
        }
        if (!header_seen) {
            header_seen = true;
            continue;
        }
        unsigned long long in_datagrams = 0;
        unsigned long long no_ports = 0;
        unsigned long long in_errors = 0;
        unsigned long long out_datagrams = 0;
        unsigned long long rcvbuf_errors = 0;
        unsigned long long sndbuf_errors = 0;
        unsigned long long in_csum_errors = 0;
        unsigned long long ignored_multi = 0;
        const int fields = std::sscanf(line, "Udp: %llu %llu %llu %llu %llu %llu %llu %llu",
                                       &in_datagrams, &no_ports, &in_errors, &out_datagrams,
                                       &rcvbuf_errors, &sndbuf_errors, &in_csum_errors,
                                       &ignored_multi);
        if (fields == 8) {
            out->in_datagrams = in_datagrams;
            out->out_datagrams = out_datagrams;
            out->in_errors = in_errors;
            out->rcvbuf_errors = rcvbuf_errors;
            out->sndbuf_errors = sndbuf_errors;
            parsed = true;
        }
        break;
    }
    std::fclose(file);
    return parsed;
}

struct UdpStatsDelta {
    long long in_datagrams = 0;
    long long out_datagrams = 0;
    long long in_errors = 0;
    long long rcvbuf_errors = 0;
    long long sndbuf_errors = 0;
};

UdpStatsDelta Delta(const UdpStats& before, const UdpStats& after) {
    UdpStatsDelta delta;
    delta.in_datagrams = static_cast<long long>(after.in_datagrams - before.in_datagrams);
    delta.out_datagrams = static_cast<long long>(after.out_datagrams - before.out_datagrams);
    delta.in_errors = static_cast<long long>(after.in_errors - before.in_errors);
    delta.rcvbuf_errors = static_cast<long long>(after.rcvbuf_errors - before.rcvbuf_errors);
    delta.sndbuf_errors = static_cast<long long>(after.sndbuf_errors - before.sndbuf_errors);
    return delta;
}

// ---------------------------------------------------------------------------
// 数据报格式与校验
// ---------------------------------------------------------------------------

constexpr std::uint32_t kMagic = 0x55445031U;  // "UDP1"
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kMaxPayload = 65507;  // IPv4 UDP 上限

struct PacketView {
    std::uint32_t magic = 0;
    std::uint32_t sequence = 0;
    std::uint16_t writer = 0;
    std::uint16_t total_size = 0;
    std::uint32_t body_sum = 0;
};

void PutU16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

void PutU32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
}

std::uint16_t GetU16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(in[0] | (static_cast<std::uint16_t>(in[1]) << 8));
}

std::uint32_t GetU32(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) |
           (static_cast<std::uint32_t>(in[3]) << 24);
}

std::uint8_t BodyByte(std::uint32_t sequence, std::size_t index) {
    return static_cast<std::uint8_t>((sequence * 131U + static_cast<std::uint32_t>(index) * 7U +
                                      static_cast<std::uint32_t>(index >> 3)) &
                                     0xFFU);
}

/// 校验和只覆盖前 256 字节：既能让 64KB 压测不把 CPU 花在哈希上，又能发现错位/串包。
constexpr std::size_t kSumBytes = 256;

std::uint32_t BodySum(std::uint32_t sequence, std::size_t body_bytes) {
    std::uint32_t sum = 0;
    const std::size_t hashed = std::min(body_bytes, kSumBytes);
    for (std::size_t i = 0; i < hashed; ++i) {
        sum = sum * 33U + BodyByte(sequence, i);
    }
    return sum;
}

void FillPacket(std::vector<std::uint8_t>* buffer, std::size_t size, std::uint32_t sequence,
                std::uint16_t writer) {
    buffer->resize(size);
    std::uint8_t* data = buffer->data();
    if (size < kHeaderBytes) {
        for (std::size_t i = 0; i < size; ++i) {
            data[i] = BodyByte(sequence, i);
        }
        return;
    }
    const std::size_t body = size - kHeaderBytes;
    PutU32(data, kMagic);
    PutU32(data + 4, sequence);
    PutU16(data + 8, writer);
    PutU16(data + 10, static_cast<std::uint16_t>(size));
    PutU32(data + 12, BodySum(sequence, body));
    for (std::size_t i = 0; i < body; ++i) {
        data[kHeaderBytes + i] = BodyByte(sequence, i);
    }
}

bool ParsePacket(const std::uint8_t* data, std::size_t size, PacketView* view) {
    if (size < kHeaderBytes) {
        return false;
    }
    view->magic = GetU32(data);
    view->sequence = GetU32(data + 4);
    view->writer = GetU16(data + 8);
    view->total_size = GetU16(data + 10);
    view->body_sum = GetU32(data + 12);
    return view->magic == kMagic && static_cast<std::size_t>(view->total_size) == size;
}

bool VerifyBody(const std::uint8_t* data, std::size_t size, const PacketView& view) {
    if (size < kHeaderBytes) {
        return false;
    }
    const std::size_t body = size - kHeaderBytes;
    if (view.body_sum != BodySum(view.sequence, body)) {
        return false;
    }
    const std::size_t hashed = std::min(body, kSumBytes);
    for (std::size_t i = 0; i < hashed; ++i) {
        if (data[kHeaderBytes + i] != BodyByte(view.sequence, i)) {
            return false;
        }
    }
    // 大包再抽查尾部 64 字节，防止尾部错位。
    const std::size_t tail = std::min<std::size_t>(body, 64);
    for (std::size_t i = body - tail; i < body; ++i) {
        if (data[kHeaderBytes + i] != BodyByte(view.sequence, i)) {
            return false;
        }
    }
    return true;
}

bool BindLoopback(UdpTransport* transport, Endpoint* local, const char* host = "127.0.0.1") {
    const Result result = transport->Bind({host, 0});
    if (!result.ok()) {
        std::fprintf(stderr, "[FAIL] Bind(%s) errno=%d\n", host, result.system_error);
        ++g_failures;
        return false;
    }
    const Result local_result = transport->LocalEndpoint(local);
    if (!local_result.ok()) {
        std::fprintf(stderr, "[FAIL] LocalEndpoint errno=%d\n", local_result.system_error);
        ++g_failures;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 压测
// ---------------------------------------------------------------------------

struct FloodResult {
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    std::uint64_t corrupt = 0;
    std::uint64_t send_fail = 0;
    double seconds = 0.0;
    double cpu_seconds = 0.0;
    UdpStatsDelta udp;
    int fd_delta = 0;
};

double Pps(const FloodResult& result) {
    return result.seconds > 0.0 ? static_cast<double>(result.received) / result.seconds : 0.0;
}

double MibPerSecond(const FloodResult& result, std::size_t size) {
    return result.seconds > 0.0
               ? static_cast<double>(result.received) * static_cast<double>(size) /
                     (1024.0 * 1024.0) / result.seconds
               : 0.0;
}

/// 单向洪泛：独立发送线程 + 主线程收包；统计丢包（回环内核缓冲溢出）。
FloodResult BenchFlood(std::size_t size, std::uint64_t packets, bool paced) {
    FloodResult result;
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    if (!BindLoopback(&receiver, &receiver_local) || !BindLoopback(&sender, &sender_local)) {
        return result;
    }

    const int fd_before = CountFds();
    UdpStats stats_before;
    (void)ReadUdpStats(&stats_before);
    const CpuTimes cpu_before = ReadCpu();

    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> send_fail{0};
    std::atomic<bool> sender_done{false};
    const Endpoint target = receiver_local;

    const Clock::time_point started = Clock::now();
    std::thread sender_thread([&]() {
        std::vector<std::uint8_t> payload;
        FillPacket(&payload, size, 0, 0);
        for (std::uint64_t i = 0; i < packets; ++i) {
            FillPacket(&payload, size, static_cast<std::uint32_t>(i), 0);
            const Result written = sender.SendTo(target, payload.data(), payload.size(),
                                                 milliseconds(2000));
            if (written.ok()) {
                sent.fetch_add(1, std::memory_order_relaxed);
            } else {
                send_fail.fetch_add(1, std::memory_order_relaxed);
            }
            if (paced && (i % 32U) == 31U) {
                std::this_thread::sleep_for(microseconds(20));
            }
        }
        sender_done.store(true, std::memory_order_release);
    });

    std::vector<std::uint8_t> buffer(65536);
    std::uint64_t received = 0;
    std::uint64_t corrupt = 0;
    int idle_timeouts = 0;
    const Clock::time_point deadline = started + std::chrono::seconds(30);
    while (received + corrupt < packets && Clock::now() < deadline) {
        Endpoint peer;
        UdpTransport::RecvInfo info;
        const Result read_result = receiver.RecvFrom(buffer.data(), buffer.size(),
                                                     milliseconds(200), &peer, &info);
        if (!read_result.ok()) {
            if (read_result.code == Code::kTimeout) {
                if (sender_done.load(std::memory_order_acquire)) {
                    ++idle_timeouts;
                    if (idle_timeouts >= 3) {
                        break;
                    }
                }
                continue;
            }
            ++corrupt;
            continue;
        }
        idle_timeouts = 0;
        PacketView view;
        if (!ParsePacket(buffer.data(), read_result.bytes, &view) ||
            !VerifyBody(buffer.data(), read_result.bytes, view) ||
            static_cast<std::size_t>(view.total_size) != size) {
            ++corrupt;
            continue;
        }
        ++received;
    }
    sender_thread.join();

    const double seconds = Seconds(Clock::now() - started);
    const CpuTimes cpu_after = ReadCpu();
    UdpStats stats_after;
    (void)ReadUdpStats(&stats_after);

    result.sent = sent.load();
    result.received = received;
    result.corrupt = corrupt;
    result.send_fail = send_fail.load();
    result.seconds = seconds;
    result.cpu_seconds = cpu_after.total() - cpu_before.total();
    result.udp = Delta(stats_before, stats_after);
    result.fd_delta = CountFds() - fd_before;
    return result;
}

struct LatencySummary {
    std::uint64_t rounds = 0;
    std::uint64_t failures = 0;
    std::uint64_t p50_us = 0;
    std::uint64_t p95_us = 0;
    std::uint64_t p99_us = 0;
    std::uint64_t max_us = 0;
    double seconds = 0.0;
};

std::uint64_t Percentile(std::vector<std::uint64_t> samples, double ratio) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double index = ratio * static_cast<double>(samples.size() - 1);
    const std::size_t offset = static_cast<std::size_t>(index + 0.5);
    return samples[std::min(offset, samples.size() - 1)];
}

/// 请求-响应 RTT：客户端 SendTo → 服务端 RecvFrom → SendTo 回显 → 客户端 RecvFrom。
LatencySummary BenchPingPong(std::size_t size, std::uint64_t rounds) {
    LatencySummary summary;
    UdpTransport server;
    UdpTransport client;
    Endpoint server_local;
    Endpoint client_local;
    if (!BindLoopback(&server, &server_local) || !BindLoopback(&client, &client_local)) {
        return summary;
    }

    std::vector<std::uint64_t> samples;
    samples.reserve(static_cast<std::size_t>(rounds));
    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> reply(size);
    std::vector<std::uint8_t> server_buffer(65536);
    const Clock::time_point started = Clock::now();
    for (std::uint64_t i = 0; i < rounds; ++i) {
        FillPacket(&request, size, static_cast<std::uint32_t>(i), 1);
        const std::uint64_t begin = NowMicros();
        if (!client.SendTo(server_local, request.data(), request.size(), milliseconds(2000)).ok()) {
            ++summary.failures;
            continue;
        }
        Endpoint peer;
        UdpTransport::RecvInfo info;
        const Result at_server = server.RecvFrom(server_buffer.data(), server_buffer.size(),
                                                 milliseconds(2000), &peer, &info);
        if (!at_server.ok() || info.datagram_bytes != size || peer.port != client_local.port) {
            ++summary.failures;
            continue;
        }
        if (!server.SendTo(peer, server_buffer.data(), at_server.bytes, milliseconds(2000)).ok()) {
            ++summary.failures;
            continue;
        }
        const Result at_client = client.RecvFrom(reply.data(), reply.size(), milliseconds(2000),
                                                 &peer, &info);
        if (!at_client.ok() || at_client.bytes != size || peer.port != server_local.port) {
            ++summary.failures;
            continue;
        }
        samples.push_back(NowMicros() - begin);
    }
    summary.seconds = Seconds(Clock::now() - started);
    summary.rounds = static_cast<std::uint64_t>(samples.size());
    summary.p50_us = Percentile(samples, 0.50);
    summary.p95_us = Percentile(samples, 0.95);
    summary.p99_us = Percentile(samples, 0.99);
    summary.max_us = samples.empty() ? 0 : *std::max_element(samples.begin(), samples.end());
    return summary;
}

/// 裸 socket 对照：同样的 sendto → poll → recvfrom 路径，衡量 UdpTransport 的每包额外开销。
LatencySummary BenchRawPingPong(std::size_t size, std::uint64_t rounds) {
    LatencySummary summary;
    const int server = ::socket(AF_INET, SOCK_DGRAM, 0);
    const int client = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (server < 0 || client < 0) {
        if (server >= 0) {
            ::close(server);
        }
        if (client >= 0) {
            ::close(client);
        }
        return summary;
    }
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(0x7F000001U);
    server_address.sin_port = 0;
    (void)::bind(server, reinterpret_cast<const sockaddr*>(&server_address), sizeof(server_address));
    socklen_t server_length = sizeof(server_address);
    (void)::getsockname(server, reinterpret_cast<sockaddr*>(&server_address), &server_length);
    sockaddr_in client_address = server_address;
    client_address.sin_port = 0;
    (void)::bind(client, reinterpret_cast<const sockaddr*>(&client_address), sizeof(client_address));

    std::vector<std::uint8_t> payload(size, 0x5A);
    std::vector<std::uint8_t> buffer(65536);
    std::vector<std::uint64_t> samples;
    samples.reserve(static_cast<std::size_t>(rounds));
    const Clock::time_point started = Clock::now();
    for (std::uint64_t i = 0; i < rounds; ++i) {
        const std::uint64_t begin = NowMicros();
        if (::sendto(client, payload.data(), payload.size(), 0,
                     reinterpret_cast<const sockaddr*>(&server_address), server_length) < 0) {
            ++summary.failures;
            continue;
        }
        pollfd server_poll{};
        server_poll.fd = server;
        server_poll.events = POLLIN;
        (void)::poll(&server_poll, 1, 2000);
        sockaddr_storage from{};
        socklen_t from_length = sizeof(from);
        const ssize_t received =
            ::recvfrom(server, buffer.data(), buffer.size(), 0,
                       reinterpret_cast<sockaddr*>(&from), &from_length);
        if (received < 0) {
            ++summary.failures;
            continue;
        }
        if (::sendto(server, buffer.data(), static_cast<std::size_t>(received), 0,
                     reinterpret_cast<const sockaddr*>(&from), from_length) < 0) {
            ++summary.failures;
            continue;
        }
        pollfd client_poll{};
        client_poll.fd = client;
        client_poll.events = POLLIN;
        (void)::poll(&client_poll, 1, 2000);
        if (::recv(client, buffer.data(), buffer.size(), 0) < 0) {
            ++summary.failures;
            continue;
        }
        samples.push_back(NowMicros() - begin);
    }
    ::close(server);
    ::close(client);
    summary.seconds = Seconds(Clock::now() - started);
    summary.rounds = static_cast<std::uint64_t>(samples.size());
    summary.p50_us = Percentile(samples, 0.50);
    summary.p95_us = Percentile(samples, 0.95);
    summary.p99_us = Percentile(samples, 0.99);
    summary.max_us = samples.empty() ? 0 : *std::max_element(samples.begin(), samples.end());
    return summary;
}

void RunBench(double scale) {
    std::printf("\n=== 压测（回环，默认内核缓冲，未调 SO_RCVBUF）===\n");
    struct Case {
        std::size_t size;
        std::uint64_t packets;
    };
    const Case cases[] = {{64, static_cast<std::uint64_t>(100000 * scale)},
                          {1400, static_cast<std::uint64_t>(100000 * scale)},
                          {65507, static_cast<std::uint64_t>(10000 * scale)}};
    for (const Case& item : cases) {
        for (int paced_index = 0; paced_index < 2; ++paced_index) {
            const bool paced = paced_index == 1;
            const FloodResult result = BenchFlood(item.size, item.packets, paced);
            const std::uint64_t lost = result.sent - result.received;
            const double loss = result.sent == 0 ? 0.0
                                                 : 100.0 * static_cast<double>(lost) /
                                                       static_cast<double>(result.sent);
            const double cpu_ratio =
                result.seconds > 0.0 ? result.cpu_seconds / result.seconds * 100.0 : 0.0;
            std::printf(
                "[bench] size=%zu packets=%" PRIu64 " mode=%s recv=%" PRIu64
                " loss=%.3f%% corrupt=%" PRIu64 " send_fail=%" PRIu64
                " elapsed=%.3fs pps=%.0f MiB/s=%.1f cpu=%.2fs(%.0f%%) fd=%+d "
                "udp_rcvbuf_err=%+lld udp_in_err=%+lld\n",
                item.size, item.packets, paced ? "paced" : "flood", result.received, loss,
                result.corrupt, result.send_fail, result.seconds, Pps(result),
                MibPerSecond(result, item.size), result.cpu_seconds, cpu_ratio, result.fd_delta,
                result.udp.rcvbuf_errors, result.udp.in_errors);
            AUDIT_CHECK(result.corrupt == 0);
            AUDIT_CHECK(result.send_fail == 0);
            AUDIT_CHECK(result.fd_delta == 0);
        }
    }

    const LatencySummary small = BenchPingPong(64, static_cast<std::uint64_t>(20000 * scale));
    std::printf(
        "[rtt] size=64 rounds=%" PRIu64 " fail=%" PRIu64
        " p50=%" PRIu64 "us p95=%" PRIu64 "us p99=%" PRIu64 "us max=%" PRIu64
        "us elapsed=%.3fs\n",
        small.rounds, small.failures, small.p50_us, small.p95_us, small.p99_us, small.max_us,
        small.seconds);
    AUDIT_CHECK(small.failures == 0);
    const LatencySummary big = BenchPingPong(1400, static_cast<std::uint64_t>(10000 * scale));
    std::printf(
        "[rtt] size=1400 rounds=%" PRIu64 " fail=%" PRIu64
        " p50=%" PRIu64 "us p95=%" PRIu64 "us p99=%" PRIu64 "us max=%" PRIu64
        "us elapsed=%.3fs\n",
        big.rounds, big.failures, big.p50_us, big.p95_us, big.p99_us, big.max_us, big.seconds);
    AUDIT_CHECK(big.failures == 0);

    const LatencySummary raw_small = BenchRawPingPong(64, static_cast<std::uint64_t>(20000 * scale));
    std::printf(
        "[rtt-raw] size=64 rounds=%" PRIu64 " fail=%" PRIu64
        " p50=%" PRIu64 "us p95=%" PRIu64 "us p99=%" PRIu64 "us（裸 socket 对照）\n",
        raw_small.rounds, raw_small.failures, raw_small.p50_us, raw_small.p95_us,
        raw_small.p99_us);
    AUDIT_CHECK(raw_small.failures == 0);
}

// ---------------------------------------------------------------------------
// 解析成本微基准（sendto 热路径瓶颈证据）
// ---------------------------------------------------------------------------

double CostGetAddrInfoNs(const char* host, std::uint16_t port, int iterations) {
    const std::string service = std::to_string(port);
    const Clock::time_point started = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_flags = AI_NUMERICSERV;
        addrinfo* raw = nullptr;
        const int rc = ::getaddrinfo(host, service.c_str(), &hints, &raw);
        if (rc != 0) {
            return -1.0;
        }
        ::freeaddrinfo(raw);
    }
    return Seconds(Clock::now() - started) * 1e9 / static_cast<double>(iterations);
}

double CostInetPtonNs(bool ipv6, int iterations) {
    std::uint8_t storage[16] = {};
    const Clock::time_point started = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        const int rc = ::inet_pton(ipv6 ? AF_INET6 : AF_INET, ipv6 ? "::1" : "127.0.0.1", storage);
        if (rc != 1) {
            return -1.0;
        }
    }
    return Seconds(Clock::now() - started) * 1e9 / static_cast<double>(iterations);
}

/// RecvFrom 每包把 sockaddr 转成字符串：getnameinfo(NI_NUMERICHOST) vs inet_ntop。
double CostGetNameInfoNs(int iterations) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(34567);
    address.sin_addr.s_addr = htonl(0x7F000001U);
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];
    const Clock::time_point started = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        const int rc = ::getnameinfo(reinterpret_cast<const sockaddr*>(&address),
                                     sizeof(address), host, sizeof(host), service,
                                     sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
        if (rc != 0) {
            return -1.0;
        }
    }
    const double elapsed = Seconds(Clock::now() - started);
    if (host[0] == '\0' || service[0] == '\0') {
        return -1.0;
    }
    return elapsed * 1e9 / static_cast<double>(iterations);
}

double CostInetNtopNs(int iterations) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(34567);
    address.sin_addr.s_addr = htonl(0x7F000001U);
    char host[INET_ADDRSTRLEN];
    const Clock::time_point started = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        const char* rc = ::inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
        if (rc == nullptr) {
            return -1.0;
        }
    }
    return Seconds(Clock::now() - started) * 1e9 / static_cast<double>(iterations);
}

void RunResolve(double scale) {
    const int iterations = static_cast<int>(200000 * scale);
    std::printf("\n=== 地址解析成本（每次调用 ns）===\n");
    const double v4 = CostGetAddrInfoNs("127.0.0.1", 12345, iterations);
    const double v6 = CostGetAddrInfoNs("::1", 12345, iterations);
    const double localhost = CostGetAddrInfoNs("localhost", 12345, iterations);
    const double pton4 = CostInetPtonNs(false, iterations);
    const double pton6 = CostInetPtonNs(true, iterations);
    const double nameinfo = CostGetNameInfoNs(iterations);
    const double ntop = CostInetNtopNs(iterations);
    std::printf("[resolve] getaddrinfo 127.0.0.1=%.1f ns  ::1=%.1f ns  localhost=%.1f ns\n", v4,
                v6, localhost);
    std::printf("[resolve] inet_pton     v4=%.1f ns  v6=%.1f ns  (iterations=%d)\n", pton4, pton6,
                iterations);
    std::printf("[resolve] getnameinfo=%.1f ns  inet_ntop=%.1f ns（RecvFrom 每包路径）\n",
                nameinfo, ntop);
    AUDIT_CHECK(v4 > 0.0);
    AUDIT_CHECK(v6 > 0.0);
    AUDIT_CHECK(pton4 > 0.0);
}

// ---------------------------------------------------------------------------
// 随机测试
// ---------------------------------------------------------------------------

std::size_t PickSize(std::uint64_t* state) {
    static const std::size_t kSizes[] = {0,  1,   2,    7,    8,    15,   16,   63,   64,
                                         65, 100, 512,  1400, 1472, 4096, 8192, 16384, 32768,
                                         65507};
    if ((NextRandom(state) & 3U) != 0) {
        return kSizes[NextRandom(state) % (sizeof(kSizes) / sizeof(kSizes[0]))];
    }
    return static_cast<std::size_t>(NextRandom(state) % (kMaxPayload + 1));
}

std::size_t PickCapacity(std::uint64_t* state) {
    static const std::size_t kCapacities[] = {0, 1, 8, 64, 100, 1000, 1400, 4096, 16384, 65536};
    if ((NextRandom(state) & 3U) != 0) {
        return kCapacities[NextRandom(state) % (sizeof(kCapacities) / sizeof(kCapacities[0]))];
    }
    return static_cast<std::size_t>(NextRandom(state) % 65537);
}

long long PickTimeoutMs(std::uint64_t* state) {
    static const long long kTimeouts[] = {-100, -1, 0, 0, 0, 1, 2, 5, 10, 50, 200, 2000};
    return kTimeouts[NextRandom(state) % (sizeof(kTimeouts) / sizeof(kTimeouts[0]))];
}

struct RandomResult {
    std::uint64_t iterations = 0;
    std::uint64_t failures = 0;
};

RandomResult RunRandom(std::uint64_t seed, int iterations) {
    RandomResult result;
    std::uint64_t state = seed | 1U;
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    if (!BindLoopback(&receiver, &receiver_local) || !BindLoopback(&sender, &sender_local)) {
        ++result.failures;
        return result;
    }
    const int fd_before = CountFds();
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> buffer(65536 + 64);
    int print_budget = 10;

    for (int index = 0; index < iterations; ++index) {
        ++result.iterations;
        const std::size_t size = PickSize(&state);
        const std::size_t capacity = PickCapacity(&state);
        const long long timeout_ms = PickTimeoutMs(&state);

        // 每次迭代前清空可能残留的数据报，避免跨迭代堆积。
        for (int drain = 0; drain < 4; ++drain) {
            UdpTransport::RecvInfo drain_info;
            if (!receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(0), nullptr,
                                   &drain_info)
                     .ok()) {
                break;
            }
        }

        FillPacket(&payload, size, static_cast<std::uint32_t>(index), 3);
        const Result sent = sender.SendTo(receiver_local, payload.data(), payload.size(),
                                          milliseconds(timeout_ms));
        if (!sent.ok() || sent.bytes != size) {
            if (print_budget > 0) {
                std::fprintf(stderr, "[random] send fail idx=%d size=%zu errno=%d\n", index, size,
                             sent.system_error);
                --print_budget;
            }
            ++result.failures;
            ++g_failures;
            ++g_checks;
            continue;
        }

        UdpTransport::RecvInfo info;
        Endpoint peer;
        const Result received = receiver.RecvFrom(buffer.data(), capacity,
                                                  milliseconds(timeout_ms), &peer, &info);
        ++g_checks;
        const std::size_t expected_bytes = std::min(size, capacity);
        const bool valid = received.ok() && received.bytes == expected_bytes &&
                           info.bytes == expected_bytes && info.datagram_bytes == size &&
                           info.truncated == (size > capacity) &&
                           peer.port == sender_local.port &&
                           std::equal(payload.begin(), payload.begin() + expected_bytes,
                                      buffer.begin());
        if (!valid) {
            ++result.failures;
            ++g_failures;
            if (print_budget > 0) {
                std::fprintf(stderr,
                             "[random] recv mismatch idx=%d size=%zu cap=%zu code=%d "
                             "bytes=%zu info_bytes=%zu dg=%zu trunc=%d peer_port=%u\n",
                             index, size, capacity, static_cast<int>(received.code),
                             received.bytes, info.bytes, info.datagram_bytes,
                             static_cast<int>(info.truncated), peer.port);
                --print_budget;
            }
        }

        // 每 97 轮做一次关闭/重开，验证 fd 不泄漏。
        if ((index % 97) == 96) {
            receiver.Close();
            sender.Close();
            if (!BindLoopback(&receiver, &receiver_local) || !BindLoopback(&sender, &sender_local)) {
                ++result.failures;
                ++g_failures;
                break;
            }
        }
    }
    const int fd_delta = CountFds() - fd_before;
    AUDIT_CHECK(fd_delta == 0);
    return result;
}

// ---------------------------------------------------------------------------
// 场景 1：回环回显（请求-响应，含来源地址校验）
// ---------------------------------------------------------------------------

void ScenarioEcho() {
    std::printf("\n=== 场景 1：回环回显 10000 次（SendTo/RecvFrom + 来源地址）===\n");
    UdpTransport server;
    UdpTransport client;
    Endpoint server_local;
    Endpoint client_local;
    if (!BindLoopback(&server, &server_local) || !BindLoopback(&client, &client_local)) {
        ++g_failures;
        return;
    }
    const int fd_before = CountFds();
    const int rounds = 10000;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> buffer(2048);
    int failures = 0;
    for (int i = 0; i < rounds; ++i) {
        FillPacket(&payload, 64, static_cast<std::uint32_t>(i), 4);
        const Result sent = client.SendTo(server_local, payload.data(), payload.size(),
                                          milliseconds(2000));
        Endpoint peer;
        UdpTransport::RecvInfo info;
        const Result at_server =
            server.RecvFrom(buffer.data(), buffer.size(), milliseconds(2000), &peer, &info);
        const bool peer_ok = peer.port == client_local.port && peer.host == "127.0.0.1";
        const Result echoed =
            server.SendTo(peer, buffer.data(), at_server.bytes, milliseconds(2000));
        const Result at_client =
            client.RecvFrom(buffer.data(), buffer.size(), milliseconds(2000), &peer, &info);
        PacketView view;
        if (!sent.ok() || !at_server.ok() || !peer_ok || !echoed.ok() || !at_client.ok() ||
            at_client.bytes != 64 || !ParsePacket(buffer.data(), at_client.bytes, &view) ||
            view.sequence != static_cast<std::uint32_t>(i) || peer.port != server_local.port) {
            ++failures;
        }
    }
    std::printf("[scenario-echo] rounds=%d failures=%d fd=%+d\n", rounds, failures,
                CountFds() - fd_before);
    AUDIT_CHECK(failures == 0);
    AUDIT_CHECK(CountFds() - fd_before == 0);
}

// ---------------------------------------------------------------------------
// 场景 2：大包 / 截断（MSG_TRUNC）与 connected Read 的静默截断
// ---------------------------------------------------------------------------

void ScenarioTruncation() {
    std::printf("\n=== 场景 2：大包与截断（MSG_TRUNC / 零缓冲 / 65507 上限）===\n");
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    if (!BindLoopback(&receiver, &receiver_local) || !BindLoopback(&sender, &sender_local)) {
        ++g_failures;
        return;
    }
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> buffer(65536);

    FillPacket(&payload, 4096, 7, 5);
    AUDIT_CHECK(sender
                    .SendTo(receiver_local, payload.data(), payload.size(), milliseconds(2000))
                    .ok());
    std::vector<std::uint8_t> tiny(64);
    UdpTransport::RecvInfo info;
    Result read_result =
        receiver.RecvFrom(tiny.data(), tiny.size(), milliseconds(2000), nullptr, &info);
    AUDIT_CHECK(read_result.ok());
    AUDIT_CHECK(read_result.bytes == 64);
    AUDIT_CHECK(info.datagram_bytes == 4096);
    AUDIT_CHECK(info.truncated);
    AUDIT_CHECK(std::equal(tiny.begin(), tiny.end(), payload.begin()));
    std::printf("[scenario-trunc] 4096->64 bytes=%zu datagram=%zu truncated=%d\n",
                read_result.bytes, info.datagram_bytes, static_cast<int>(info.truncated));

    // 零缓冲：探测长度并丢弃数据报。
    AUDIT_CHECK(sender
                    .SendTo(receiver_local, payload.data(), payload.size(), milliseconds(2000))
                    .ok());
    read_result = receiver.RecvFrom(nullptr, 0, milliseconds(2000), nullptr, &info);
    AUDIT_CHECK(read_result.ok());
    AUDIT_CHECK(read_result.bytes == 0);
    AUDIT_CHECK(info.datagram_bytes == 4096);
    AUDIT_CHECK(info.truncated);

    // 65507 上限：完整接收。
    FillPacket(&payload, kMaxPayload, 8, 5);
    const Result sent =
        sender.SendTo(receiver_local, payload.data(), payload.size(), milliseconds(2000));
    read_result = receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(2000), nullptr,
                                    &info);
    AUDIT_CHECK(sent.ok());
    AUDIT_CHECK(read_result.ok());
    AUDIT_CHECK(read_result.bytes == kMaxPayload);
    AUDIT_CHECK(info.datagram_bytes == kMaxPayload);
    AUDIT_CHECK(!info.truncated);
    AUDIT_CHECK(std::equal(payload.begin(), payload.end(), buffer.begin()));
    std::printf("[scenario-trunc] 65507 完整接收 bytes=%zu truncated=%d\n", read_result.bytes,
                static_cast<int>(info.truncated));

    // connected Read：接口没有截断输出，静默截断（文档已声明）。
    UdpTransport connected_receiver;
    UdpTransport connected_sender;
    Endpoint connected_local;
    Endpoint connected_sender_local;
    if (!BindLoopback(&connected_receiver, &connected_local) ||
        !BindLoopback(&connected_sender, &connected_sender_local)) {
        ++g_failures;
        return;
    }
    const Result connected =
        connected_sender.Connect(connected_local, milliseconds(2000));
    AUDIT_CHECK(connected.ok());
    AUDIT_CHECK(connected_receiver.Connect(connected_sender_local, milliseconds(2000)).ok());
    std::vector<std::uint8_t> big(4096, 0x5A);
    AUDIT_CHECK(connected_sender.Write(big.data(), big.size(), milliseconds(2000)).ok());
    std::vector<std::uint8_t> small(64);
    const Result truncated_read =
        connected_receiver.Read(small.data(), small.size(), milliseconds(2000));
    AUDIT_CHECK(truncated_read.ok());
    AUDIT_CHECK(truncated_read.bytes == 64);
    const Result extra =
        connected_receiver.Read(small.data(), small.size(), milliseconds(20));
    AUDIT_CHECK(extra.code == Code::kTimeout);
    std::printf("[scenario-trunc] connected Read 4096->64 静默截断 bytes=%zu（后续读超时）\n",
                truncated_read.bytes);
}

// ---------------------------------------------------------------------------
// 场景 3：超时语义（0 / 负 / 精确 / 极值 / 数据迟到）
// ---------------------------------------------------------------------------

void ScenarioTimeout() {
    std::printf("\n=== 场景 3：超时（0 / 负 / 1ms / 数据迟到 / 极大值）===\n");
    UdpTransport receiver;
    Endpoint local;
    if (!BindLoopback(&receiver, &local)) {
        ++g_failures;
        return;
    }
    std::vector<std::uint8_t> buffer(64);

    // 空 socket + 1ms：应返回 kTimeout 且耗时在合理区间。
    Clock::time_point begin = Clock::now();
    Result result = receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(1), nullptr,
                                      nullptr);
    double elapsed = Seconds(Clock::now() - begin) * 1000.0;
    AUDIT_CHECK(result.code == Code::kTimeout);
    AUDIT_CHECK(elapsed >= 0.5 && elapsed < 100.0);
    std::printf("[scenario-timeout] 1ms 实耗=%.2fms code=kTimeout\n", elapsed);

    // 负超时按 0 处理：立即返回，不能挂死。
    begin = Clock::now();
    result = receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(-100), nullptr, nullptr);
    elapsed = Seconds(Clock::now() - begin) * 1000.0;
    AUDIT_CHECK(result.code == Code::kTimeout);
    AUDIT_CHECK(elapsed < 50.0);
    std::printf("[scenario-timeout] -100ms 实耗=%.2fms code=kTimeout（按 0 处理）\n", elapsed);

    // 数据迟到 30ms：RecvFrom 应在 30ms 后拿到数据，而不是提前超时。
    std::thread delayed_sender([&]() {
        std::this_thread::sleep_for(milliseconds(30));
        UdpTransport sender;
        sender.Bind({"127.0.0.1", 0});
        const std::uint8_t payload[4] = {1, 2, 3, 4};
        (void)sender.SendTo(local, payload, sizeof(payload), milliseconds(2000));
    });
    begin = Clock::now();
    result = receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(2000), nullptr, nullptr);
    elapsed = Seconds(Clock::now() - begin) * 1000.0;
    delayed_sender.join();
    AUDIT_CHECK(result.ok());
    AUDIT_CHECK(result.bytes == 4);
    AUDIT_CHECK(elapsed >= 25.0);
    std::printf("[scenario-timeout] 迟到 30ms 数据：实耗=%.2fms bytes=%zu\n", elapsed,
                result.bytes);

    // 极大超时（milliseconds::max）：不得因 deadline 溢出而立即超时。
    // 期望行为：阻塞等待，30ms 后数据到达即返回；修复前会 0ms 返回 kTimeout。
    std::thread delayed_sender2([&]() {
        std::this_thread::sleep_for(milliseconds(30));
        UdpTransport sender;
        sender.Bind({"127.0.0.1", 0});
        const std::uint8_t payload[4] = {9, 8, 7, 6};
        (void)sender.SendTo(local, payload, sizeof(payload), milliseconds(2000));
    });
    begin = Clock::now();
    result = receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds::max(), nullptr,
                               nullptr);
    elapsed = Seconds(Clock::now() - begin) * 1000.0;
    delayed_sender2.join();
    const bool huge_ok = result.ok() && result.bytes == 4 && elapsed >= 25.0;
    std::printf("[scenario-timeout] milliseconds::max：实耗=%.2fms code=%s bytes=%zu -> %s\n",
                elapsed, go2cpp::bento::net::CodeName(result.code), result.bytes,
                huge_ok ? "OK" : "立即超时(违反语义)");
    CheckMsg(huge_ok, "milliseconds::max() 超时应被钳制而不是立即超时");

    // 零超时在空 socket 上的成本（非阻塞路径）。
    begin = Clock::now();
    int timeouts = 0;
    for (int i = 0; i < 10000; ++i) {
        if (receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(0), nullptr, nullptr)
                .code == Code::kTimeout) {
            ++timeouts;
        }
    }
    elapsed = Seconds(Clock::now() - begin) * 1000.0;
    AUDIT_CHECK(timeouts == 10000);
    std::printf("[scenario-timeout] 10000 次 0ms RecvFrom：%.2fms（%.2fus/次）\n", elapsed,
                elapsed * 1000.0 / 10000.0);
}

// ---------------------------------------------------------------------------
// 场景 4：并发发送（同一 socket 多线程 + 多 socket 汇聚）
// ---------------------------------------------------------------------------

void ScenarioConcurrent() {
    std::printf("\n=== 场景 4：并发发送（4 线程 × 5000 包，共享 socket）===\n");
    UdpTransport server;
    UdpTransport shared_sender;
    Endpoint server_local;
    Endpoint sender_local;
    if (!BindLoopback(&server, &server_local) || !BindLoopback(&shared_sender, &sender_local)) {
        ++g_failures;
        return;
    }
    const int fd_before = CountFds();
    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;
    std::atomic<int> failed_sends{0};
    std::atomic<bool> sender_done{false};
    std::vector<std::thread> threads;
    const Clock::time_point started = Clock::now();
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&, thread_index]() {
            std::vector<std::uint8_t> payload;
            FillPacket(&payload, 64, 0, static_cast<std::uint16_t>(thread_index));
            for (int i = 0; i < kPerThread; ++i) {
                FillPacket(&payload, 64, static_cast<std::uint32_t>(i),
                           static_cast<std::uint16_t>(thread_index));
                if (!shared_sender
                         .SendTo(server_local, payload.data(), payload.size(),
                                 milliseconds(2000))
                         .ok()) {
                    failed_sends.fetch_add(1);
                }
                // 发送端限速：4 线程全速会把默认 rcvbuf 打爆（实测丢 62%），
                // 那样测到的是内核丢包而不是并发正确性。
                if ((i % 4) == 3) {
                    std::this_thread::sleep_for(microseconds(20));
                }
            }
        });
    }
    std::thread done_thread([&]() {
        for (std::thread& thread : threads) {
            thread.join();
        }
        sender_done.store(true);
    });

    std::vector<std::uint8_t> buffer(2048);
    std::uint64_t received = 0;
    std::uint64_t corrupt = 0;
    std::vector<std::vector<bool>> seen(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        seen[static_cast<std::size_t>(t)].assign(kPerThread, false);
    }
    std::uint64_t duplicates = 0;
    const Clock::time_point deadline = started + std::chrono::seconds(30);
    int idle_timeouts = 0;
    while (received + corrupt <
               static_cast<std::uint64_t>(kThreads) * kPerThread &&
           Clock::now() < deadline) {
        Endpoint peer;
        UdpTransport::RecvInfo info;
        const Result result = server.RecvFrom(buffer.data(), buffer.size(), milliseconds(100),
                                              &peer, &info);
        if (!result.ok()) {
            if (result.code == Code::kTimeout) {
                if (sender_done.load()) {
                    if (++idle_timeouts >= 5) {
                        break;
                    }
                }
                continue;
            }
            ++corrupt;
            continue;
        }
        idle_timeouts = 0;
        PacketView view;
        if (result.bytes != 64 || !ParsePacket(buffer.data(), result.bytes, &view) ||
            view.writer >= kThreads || view.sequence >= static_cast<std::uint32_t>(kPerThread) ||
            !VerifyBody(buffer.data(), result.bytes, view)) {
            ++corrupt;
            continue;
        }
        const std::size_t writer = view.writer;
        if (seen[writer][view.sequence]) {
            ++duplicates;
        } else {
            seen[writer][view.sequence] = true;
            ++received;
        }
    }
    done_thread.join();
    const double seconds = Seconds(Clock::now() - started);
    std::uint64_t missing = static_cast<std::uint64_t>(kThreads) * kPerThread - received;
    std::printf(
        "[scenario-concurrent] threads=%d per_thread=%d recv=%" PRIu64
        " missing=%" PRIu64 " corrupt=%" PRIu64 " duplicates=%" PRIu64
        " send_fail=%d elapsed=%.3fs pps=%.0f fd=%+d\n",
        kThreads, kPerThread, received, missing, corrupt, duplicates, failed_sends.load(),
        seconds, static_cast<double>(received) / seconds, CountFds() - fd_before);
    AUDIT_CHECK(failed_sends.load() == 0);
    AUDIT_CHECK(corrupt == 0);
    AUDIT_CHECK(duplicates == 0);
    AUDIT_CHECK(CountFds() - fd_before == 0);
    // 回环默认缓冲下允许内核丢包，但丢失率必须低于 1%，且不能有撕裂/串包。
    AUDIT_CHECK(static_cast<double>(missing) <
                static_cast<double>(kThreads) * kPerThread * 0.01);
}

// ---------------------------------------------------------------------------
// 场景 5：IPv6 ::1（环境不支持时 SKIP）
// ---------------------------------------------------------------------------

void ScenarioIpv6() {
    std::printf("\n=== 场景 5：IPv6 ::1 回显 + 截断 ===\n");
    UdpTransport receiver;
    UdpTransport sender;
    if (!receiver.Bind({"::1", 0}).ok()) {
        Skip("IPv6 ::1 unavailable");
        return;
    }
    Endpoint receiver_local;
    Endpoint sender_local;
    AUDIT_CHECK(receiver.LocalEndpoint(&receiver_local).ok());
    if (!sender.Bind({"::1", 0}).ok()) {
        ++g_failures;
        return;
    }
    AUDIT_CHECK(sender.LocalEndpoint(&sender_local).ok());
    AUDIT_CHECK(receiver_local.host.find("::1") != std::string::npos);

    // 回显 1000 次。
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> buffer(1024);
    int failures = 0;
    for (int i = 0; i < 1000; ++i) {
        FillPacket(&payload, 300, static_cast<std::uint32_t>(i), 6);
        Endpoint peer;
        UdpTransport::RecvInfo info;
        const Result sent = sender.SendTo(receiver_local, payload.data(), payload.size(),
                                          milliseconds(2000));
        const Result got = receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(2000),
                                             &peer, &info);
        if (!sent.ok() || !got.ok() || got.bytes != 300 || peer.port != sender_local.port ||
            !std::equal(payload.begin(), payload.end(), buffer.begin())) {
            ++failures;
        }
    }
    AUDIT_CHECK(failures == 0);

    // 截断（IPv6 上限 65527）。
    FillPacket(&payload, 4096, 99, 6);
    AUDIT_CHECK(sender
                    .SendTo(receiver_local, payload.data(), payload.size(), milliseconds(2000))
                    .ok());
    std::vector<std::uint8_t> tiny(128);
    Endpoint peer;
    UdpTransport::RecvInfo info;
    const Result truncated =
        receiver.RecvFrom(tiny.data(), tiny.size(), milliseconds(2000), &peer, &info);
    AUDIT_CHECK(truncated.ok());
    AUDIT_CHECK(truncated.bytes == 128);
    AUDIT_CHECK(info.datagram_bytes == 4096);
    AUDIT_CHECK(info.truncated);
    std::printf("[scenario-ipv6] local=%s:%u 回显 1000 次 failures=%d；4096->128 截断=%d\n",
                receiver_local.host.c_str(), receiver_local.port, failures,
                static_cast<int>(info.truncated));
}

// ---------------------------------------------------------------------------
// 附加场景：connected UDP 的 ICMP 端口不可达（ECONNREFUSED）
// ---------------------------------------------------------------------------

void ScenarioIcmpUnreachable() {
    std::printf("\n=== 场景 6（附加）：connected UDP 的 ICMP 端口不可达 ===\n");
    UdpTransport client;
    const Result connected = client.Connect({"127.0.0.1", 9}, milliseconds(2000));
    AUDIT_CHECK(connected.ok());
    const std::uint8_t payload[4] = {1, 2, 3, 4};
    const Result written = client.Write(payload, sizeof(payload), milliseconds(2000));
    AUDIT_CHECK(written.ok());
    std::vector<std::uint8_t> buffer(64);
    const Result read_result = client.Read(buffer.data(), buffer.size(), milliseconds(500));
    const bool refused = read_result.code == Code::kError &&
                         read_result.system_error == ECONNREFUSED;
    std::printf("[scenario-icmp] code=%s errno=%d -> %s\n",
                go2cpp::bento::net::CodeName(read_result.code), read_result.system_error,
                refused ? "ECONNREFUSED（符合文档）" : "非 ECONNREFUSED");
    CheckMsg(refused, "connected UDP 收到 ICMP 后 Read 应返回 kError+ECONNREFUSED");
}

// ---------------------------------------------------------------------------
// 热路径微基准：数据已就绪时每次 RecvFrom 的成本（poll 是否可省的直接证据）
// ---------------------------------------------------------------------------

struct MicroResult {
    double ns_per_call = 0.0;
    std::uint64_t calls = 0;
    std::uint64_t failures = 0;
    double wall_seconds = 0.0;
};

/// 先灌满一批数据报（都在内核接收队列里），再计时连续 RecvFrom；末尾的耗时
/// 就是「数据已就绪」路径的每包成本，不受线程调度与丢包影响。
MicroResult MicroRecvHot(std::size_t size, int batches, int batch_size) {
    MicroResult result;
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    if (!BindLoopback(&receiver, &receiver_local) || !BindLoopback(&sender, &sender_local)) {
        ++result.failures;
        return result;
    }
    std::vector<std::uint8_t> payload;
    FillPacket(&payload, size, 0, 7);
    std::vector<std::uint8_t> buffer(65536);
    std::uint64_t elapsed_ns = 0;
    for (int batch = 0; batch < batches; ++batch) {
        for (int i = 0; i < batch_size; ++i) {
            FillPacket(&payload, size, static_cast<std::uint32_t>(i), 7);
            if (!sender.SendTo(receiver_local, payload.data(), payload.size(),
                               milliseconds(2000))
                     .ok()) {
                ++result.failures;
                return result;
            }
        }
        const Clock::time_point started = Clock::now();
        for (int i = 0; i < batch_size; ++i) {
            UdpTransport::RecvInfo info;
            const Result received = receiver.RecvFrom(buffer.data(), buffer.size(),
                                                      milliseconds(2000), nullptr, &info);
            if (!received.ok() || received.bytes != size) {
                ++result.failures;
            }
        }
        elapsed_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    }
    result.calls = static_cast<std::uint64_t>(batches) * static_cast<std::uint64_t>(batch_size);
    result.ns_per_call = static_cast<double>(elapsed_ns) / static_cast<double>(result.calls);
    return result;
}

/// 空 socket + timeout=0：空闲轮询路径不能因为优化而变慢。
MicroResult MicroRecvIdleZero(int iterations) {
    MicroResult result;
    UdpTransport receiver;
    Endpoint local;
    if (!BindLoopback(&receiver, &local)) {
        ++result.failures;
        return result;
    }
    std::vector<std::uint8_t> buffer(64);
    const Clock::time_point started = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(0), nullptr, nullptr)
                .code != Code::kTimeout) {
            ++result.failures;
        }
    }
    result.wall_seconds = Seconds(Clock::now() - started);
    result.calls = static_cast<std::uint64_t>(iterations);
    result.ns_per_call = result.wall_seconds * 1e9 / static_cast<double>(iterations);
    return result;
}

/// connected 模式 Read 的热路径成本（数据已就绪）。
MicroResult MicroReadHot(std::size_t size, int batches, int batch_size) {
    MicroResult result;
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    if (!BindLoopback(&receiver, &receiver_local) || !BindLoopback(&sender, &sender_local)) {
        ++result.failures;
        return result;
    }
    if (!sender.Connect(receiver_local, milliseconds(2000)).ok() ||
        !receiver.Connect(sender_local, milliseconds(2000)).ok()) {
        ++result.failures;
        return result;
    }
    std::vector<std::uint8_t> payload(size, 0x5A);
    std::vector<std::uint8_t> buffer(65536);
    std::uint64_t elapsed_ns = 0;
    for (int batch = 0; batch < batches; ++batch) {
        for (int i = 0; i < batch_size; ++i) {
            if (!sender.Write(payload.data(), payload.size(), milliseconds(2000)).ok()) {
                ++result.failures;
                return result;
            }
        }
        const Clock::time_point started = Clock::now();
        for (int i = 0; i < batch_size; ++i) {
            const Result received =
                receiver.Read(buffer.data(), buffer.size(), milliseconds(2000));
            if (!received.ok() || received.bytes != size) {
                ++result.failures;
            }
        }
        elapsed_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    }
    result.calls = static_cast<std::uint64_t>(batches) * static_cast<std::uint64_t>(batch_size);
    result.ns_per_call = static_cast<double>(elapsed_ns) / static_cast<double>(result.calls);
    return result;
}

void RunMicro(double scale) {
    const int batches = static_cast<int>(200 * scale);
    const int batch_size = 128;
    std::printf("\n=== 热路径微基准（数据已就绪，单线程）===\n");
    const MicroResult recv_hot = MicroRecvHot(64, batches, batch_size);
    const MicroResult read_hot = MicroReadHot(64, batches, batch_size);
    const MicroResult idle = MicroRecvIdleZero(static_cast<int>(20000 * scale));
    std::printf("[micro] RecvFrom(64B, 已就绪)=%.0f ns/call calls=%" PRIu64 " fail=%" PRIu64 "\n",
                recv_hot.ns_per_call, recv_hot.calls, recv_hot.failures);
    std::printf("[micro] Read(64B, 已就绪)    =%.0f ns/call calls=%" PRIu64 " fail=%" PRIu64 "\n",
                read_hot.ns_per_call, read_hot.calls, read_hot.failures);
    std::printf("[micro] RecvFrom(0ms, 空闲)  =%.0f ns/call calls=%" PRIu64 " fail=%" PRIu64 "\n",
                idle.ns_per_call, idle.calls, idle.failures);
    AUDIT_CHECK(recv_hot.failures == 0);
    AUDIT_CHECK(read_hot.failures == 0);
    AUDIT_CHECK(idle.failures == 0);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

bool StartsWith(const char* text, const char* prefix) {
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = "all";
    double scale = 1.0;
    std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
    for (int i = 1; i < argc; ++i) {
        if (StartsWith(argv[i], "--mode=")) {
            mode = argv[i] + 7;
        } else if (StartsWith(argv[i], "--scale=")) {
            scale = std::strtod(argv[i] + 8, nullptr);
        } else if (StartsWith(argv[i], "--seed=")) {
            seed = std::strtoull(argv[i] + 7, nullptr, 0);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (scale <= 0.0) {
        std::fprintf(stderr, "invalid --scale\n");
        return 2;
    }

    const int fd_start = CountFds();
    std::printf("=== audit_udp_transport stress (mode=%s scale=%.2f seed=0x%" PRIx64 ") ===\n",
                mode, scale, seed);
    std::printf("[env] cpus=%ld rss=%ldKB hwm=%ldKB fd=%d\n", ::sysconf(_SC_NPROCESSORS_ONLN),
                ProcStatusKb("VmRSS:"), ProcStatusKb("VmHWM:"), fd_start);

    const bool run_all = std::strcmp(mode, "all") == 0;
    if (run_all || std::strcmp(mode, "bench") == 0) {
        RunBench(scale);
    }
    if (run_all || std::strcmp(mode, "resolve") == 0) {
        RunResolve(scale);
    }
    if (run_all || std::strcmp(mode, "random") == 0) {
        std::printf("\n=== 随机测试（2000 轮）===\n");
        const RandomResult random =
            RunRandom(seed, static_cast<int>(2000 * scale));
        std::printf("[random] iterations=%" PRIu64 " failures=%" PRIu64 "\n", random.iterations,
                    random.failures);
        AUDIT_CHECK(random.failures == 0);
    }
    if (run_all || std::strcmp(mode, "micro") == 0) {
        RunMicro(scale);
    }
    if (run_all || std::strcmp(mode, "scenario") == 0) {
        ScenarioEcho();
        ScenarioTruncation();
        ScenarioTimeout();
        ScenarioConcurrent();
        ScenarioIpv6();
        ScenarioIcmpUnreachable();
    }

    const int fd_delta = CountFds() - fd_start;
    std::printf("\n[summary] checks=%d failures=%d skips=%d fd_delta=%+d rss=%ldKB hwm=%ldKB\n",
                g_checks, g_failures, g_skips, fd_delta, ProcStatusKb("VmRSS:"),
                ProcStatusKb("VmHWM:"));
    if (fd_delta != 0) {
        std::fprintf(stderr, "[FAIL] fd leak: %+d\n", fd_delta);
        ++g_failures;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] audit_udp_transport: %d/%d checks failed\n", g_failures,
                     g_checks);
        return 1;
    }
    std::printf("[PASS] audit_udp_transport (%d checks, %d skips)\n", g_checks, g_skips);
    return 0;
}
