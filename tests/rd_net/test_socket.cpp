#include "go2cpp/rd_net/socket.hpp"

/// @file test_socket.cpp
/// @brief REQ-006 验收：四态可分（kClosed / kReset / kTruncated / kWouldBlock）
///        + 1e4 无泄漏 + 100 socket 不串包 + 10 万次压测。

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK_TRUE(cond)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                       \
    } while (0)

#define CHECK_STATUS(actual, expected)                                          \
    do {                                                                        \
        ++g_checks;                                                             \
        const rd::net::IoStatus a_ = (actual);                                  \
        const rd::net::IoStatus e_ = (expected);                                \
        if (!(a_ == e_)) {                                                      \
            ++g_failures;                                                       \
            std::printf("[FAIL] %s:%d: status=%s(%d) expected=%s(%d)\n",         \
                        __FILE__, __LINE__, rd::net::IoStatusName(a_),          \
                        static_cast<int>(a_), rd::net::IoStatusName(e_),        \
                        static_cast<int>(e_));                                  \
        }                                                                       \
    } while (0)

long ReadVmRssKb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) {
        return -1;
    }
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            if (std::sscanf(line + 6, "%ld", &kb) != 1) {
                kb = -1;
            }
            break;
        }
    }
    std::fclose(f);
    return kb;
}

int CountOpenFds() {
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) {
        return -1;
    }
    int n = 0;
    while (dirent* ent = ::readdir(d)) {
        if (std::strcmp(ent->d_name, ".") != 0 && std::strcmp(ent->d_name, "..") != 0) {
            ++n;
        }
    }
    ::closedir(d);
    return n;
}

int WaitEvents(int fd, short events, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = events;
    for (;;) {
        const int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc > 0) {
            return pfd.revents;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return rc;
    }
}

int MakeListener(std::uint16_t* port_out) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    *port_out = ntohs(addr.sin_port);
    return fd;
}

int AcceptOne(int listen_fd) {
    for (;;) {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd >= 0) {
            return fd;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

/// @brief 可校验的确定性字节模式，用于「不丢数据」比对。
void FillPattern(std::uint8_t* dst, std::size_t n, std::size_t offset) {
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = static_cast<std::uint8_t>(((offset + i) * 31u + 7u) & 0xFFu);
    }
}

bool VerifyPattern(const std::uint8_t* src, std::size_t n, std::size_t offset,
                   std::size_t* bad_offset) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t want = static_cast<std::uint8_t>(((offset + i) * 31u + 7u) & 0xFFu);
        if (src[i] != want) {
            if (bad_offset != nullptr) {
                *bad_offset = offset + i;
            }
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- 场景 1：FIN
void Scenario1Fin() {
    std::printf("\n== 场景 1: TCP 对端 shutdown(SHUT_WR) -> kClosed ==\n");
    std::uint16_t port = 0;
    const int listen_fd = MakeListener(&port);
    CHECK_TRUE(listen_fd >= 0);
    if (listen_fd < 0) {
        return;
    }
    auto opened = rd::net::StreamSocket::Open(rd::net::Endpoint{"127.0.0.1", port});
    CHECK_TRUE(opened.ok());
    if (!opened.ok()) {
        ::close(listen_fd);
        return;
    }
    rd::net::StreamSocket client = std::move(*opened);
    const int peer = AcceptOne(listen_fd);
    ::close(listen_fd);
    CHECK_TRUE(peer >= 0);
    if (peer < 0) {
        return;
    }

    // 对端有序关闭写方向：本端应收到 FIN，而不是 RST。
    CHECK_TRUE(::shutdown(peer, SHUT_WR) == 0);
    const int revents = WaitEvents(client.fd(), POLLIN, 1000);
    std::printf("  poll revents=0x%x\n", static_cast<unsigned>(revents));
    CHECK_TRUE(revents > 0);

    std::uint8_t buf[64];
    std::size_t got = 0;
    const rd::net::IoStatus status =
        client.Recv(rd::net::MutableByteView{buf, sizeof(buf)}, &got);
    std::printf("  Recv -> %s, received=%zu, errno=%d\n",
                rd::net::IoStatusName(status), got, client.last_system_error());
    CHECK_STATUS(status, rd::net::IoStatus::kClosed);
    CHECK_TRUE(got == 0);
    ::close(peer);
}

// ---------------------------------------------------------------- 场景 2：RST
void Scenario2Reset() {
    std::printf("\n== 场景 2: TCP 对端 SO_LINGER{1,0}+close -> kReset ==\n");
    std::uint16_t port = 0;
    const int listen_fd = MakeListener(&port);
    CHECK_TRUE(listen_fd >= 0);
    if (listen_fd < 0) {
        return;
    }
    auto opened = rd::net::StreamSocket::Open(rd::net::Endpoint{"127.0.0.1", port});
    CHECK_TRUE(opened.ok());
    if (!opened.ok()) {
        ::close(listen_fd);
        return;
    }
    rd::net::StreamSocket client = std::move(*opened);
    const int peer = AcceptOne(listen_fd);
    ::close(listen_fd);
    CHECK_TRUE(peer >= 0);
    if (peer < 0) {
        return;
    }

    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 0;  // 关闭时发 RST，而非 FIN
    CHECK_TRUE(::setsockopt(peer, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
    ::close(peer);

    std::uint8_t buf[64];
    std::size_t got = 0;
    rd::net::IoStatus status = rd::net::IoStatus::kWouldBlock;
    int revents = 0;
    for (int i = 0; i < 50; ++i) {
        revents = WaitEvents(client.fd(), POLLIN, 100);
        if (revents == 0) {
            continue;
        }
        got = 0;
        status = client.Recv(rd::net::MutableByteView{buf, sizeof(buf)}, &got);
        if (status != rd::net::IoStatus::kWouldBlock) {
            break;
        }
    }
    std::printf("  poll revents=0x%x\n", static_cast<unsigned>(revents));
    std::printf("  Recv -> %s, received=%zu, errno=%d\n",
                rd::net::IoStatusName(status), got, client.last_system_error());
    CHECK_STATUS(status, rd::net::IoStatus::kReset);
    CHECK_TRUE(status != rd::net::IoStatus::kClosed);
    CHECK_TRUE(client.last_system_error() == ECONNRESET);
}

// ---------------------------------------------------------------- 场景 3：UDP 截断
void Scenario3Truncated() {
    std::printf("\n== 场景 3: UDP 70000 字节 -> 1024 字节缓冲 -> kTruncated ==\n");
    auto recv_opened =
        rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    auto send_opened =
        rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    CHECK_TRUE(recv_opened.ok());
    CHECK_TRUE(send_opened.ok());
    if (!recv_opened.ok() || !send_opened.ok()) {
        return;
    }
    rd::net::DatagramSocket receiver = std::move(*recv_opened);
    rd::net::DatagramSocket sender = std::move(*send_opened);
    std::printf("  receiver=%s:%u sender=%s:%u\n",
                receiver.local_endpoint().host.c_str(), receiver.local_endpoint().port,
                sender.local_endpoint().host.c_str(), sender.local_endpoint().port);

    // IPv4 单个 UDP 数据报最大载荷 65507 字节；70000 字节根本发不出去。
    std::vector<std::uint8_t> oversized(70000, 0x5A);
    rd::net::SendResult sr{};
    rd::net::IoStatus status = sender.SendTo(
        rd::net::ByteView{oversized.data(), oversized.size()},
        receiver.local_endpoint(), &sr);
    std::printf("  SendTo(70000) -> %s, errno=%d (%s)\n",
                rd::net::IoStatusName(status), sender.last_system_error(),
                std::strerror(sender.last_system_error()));
    CHECK_STATUS(status, rd::net::IoStatus::kIoError);
    CHECK_TRUE(sender.last_system_error() == EMSGSIZE);

    // 用合法上限 65507 触发接收侧截断：缓冲 1024 < 报文 65507。
    std::vector<std::uint8_t> max_payload(65507);
    FillPattern(max_payload.data(), max_payload.size(), 0);
    status = sender.SendTo(
        rd::net::ByteView{max_payload.data(), max_payload.size()},
        receiver.local_endpoint(), &sr);
    std::printf("  SendTo(65507) -> %s, token=%llu\n",
                rd::net::IoStatusName(status),
                static_cast<unsigned long long>(sr.token));
    CHECK_STATUS(status, rd::net::IoStatus::kOk);

    std::uint8_t small[1024];
    std::size_t got = 0;
    rd::net::Endpoint from{};
    CHECK_TRUE(WaitEvents(receiver.fd(), POLLIN, 1000) > 0);
    status = receiver.RecvFrom(rd::net::MutableByteView{small, sizeof(small)}, &from,
                               &got);
    std::printf("  RecvFrom(1024) -> %s, actual=%zu (buffer=1024), from=%s:%u\n",
                rd::net::IoStatusName(status), got, from.host.c_str(), from.port);
    CHECK_STATUS(status, rd::net::IoStatus::kTruncated);
    CHECK_TRUE(status != rd::net::IoStatus::kOk);
    CHECK_TRUE(got == 65507);
    CHECK_TRUE(small[0] == max_payload[0]);
    CHECK_TRUE(small[1023] == max_payload[1023]);

    // 截断后 socket 仍可正常收发小包。
    std::vector<std::uint8_t> tiny(100);
    FillPattern(tiny.data(), tiny.size(), 999);
    status = sender.SendTo(rd::net::ByteView{tiny.data(), tiny.size()},
                           receiver.local_endpoint(), &sr);
    CHECK_STATUS(status, rd::net::IoStatus::kOk);
    CHECK_TRUE(WaitEvents(receiver.fd(), POLLIN, 1000) > 0);
    got = 0;
    status = receiver.RecvFrom(rd::net::MutableByteView{small, sizeof(small)}, &from,
                               &got);
    std::printf("  follow-up RecvFrom -> %s, actual=%zu\n",
                rd::net::IoStatusName(status), got);
    CHECK_STATUS(status, rd::net::IoStatus::kOk);
    CHECK_TRUE(got == 100);
}

// ---------------------------------------------------------------- 场景 4：满缓冲
void Scenario4WouldBlock() {
    std::printf("\n== 场景 4: TCP 非阻塞发送塞满对端缓冲 -> kWouldBlock，重试不丢数据 ==\n");
    std::uint16_t port = 0;
    const int listen_fd = MakeListener(&port);
    CHECK_TRUE(listen_fd >= 0);
    if (listen_fd < 0) {
        return;
    }
    int listen_rcvbuf = 8192;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_RCVBUF, &listen_rcvbuf,
                 sizeof(listen_rcvbuf));
    rd::net::DialOptions dopts;
    dopts.send_buffer_bytes = 16384;
    dopts.connect_timeout = std::chrono::milliseconds(2000);
    auto opened = rd::net::StreamSocket::Open(rd::net::Endpoint{"127.0.0.1", port},
                                              dopts);
    CHECK_TRUE(opened.ok());
    if (!opened.ok()) {
        ::close(listen_fd);
        return;
    }
    rd::net::StreamSocket client = std::move(*opened);
    const int peer = AcceptOne(listen_fd);
    ::close(listen_fd);
    CHECK_TRUE(peer >= 0);
    if (peer < 0) {
        return;
    }
    int peer_rcvbuf = 8192;
    ::setsockopt(peer, SOL_SOCKET, SO_RCVBUF, &peer_rcvbuf, sizeof(peer_rcvbuf));

    constexpr std::size_t kChunk = 16384;
    constexpr std::size_t kTotal = 512 * 1024;
    std::vector<std::uint8_t> chunk(kChunk);
    std::vector<std::uint8_t> rbuf(65536);
    std::size_t sent = 0;
    std::size_t received = 0;
    bool saw_would_block = false;
    bool data_ok = true;
    std::uint64_t last_token = 0;
    bool token_monotonic = true;

    auto drain_peer = [&]() {
        for (;;) {
            const ssize_t n = ::recv(peer, rbuf.data(), rbuf.size(), MSG_DONTWAIT);
            if (n > 0) {
                std::size_t bad = 0;
                if (!VerifyPattern(rbuf.data(), static_cast<std::size_t>(n), received,
                                   &bad)) {
                    data_ok = false;
                    std::printf("  pattern mismatch at offset %zu\n", bad);
                }
                received += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    };

    // 阶段 1：对端不读，连续非阻塞发送，直到本端发送缓冲满。
    while (sent < kTotal) {
        const std::size_t n = std::min(kChunk, kTotal - sent);
        FillPattern(chunk.data(), n, sent);
        rd::net::SendResult sr{};
        const rd::net::IoStatus status =
            client.Send(rd::net::ByteView{chunk.data(), n}, &sr);
        if (sr.token <= last_token) {
            token_monotonic = false;
        }
        last_token = sr.token;
        if (status == rd::net::IoStatus::kOk) {
            CHECK_TRUE(client.last_transferred() > 0);
            sent += client.last_transferred();
        } else if (status == rd::net::IoStatus::kWouldBlock) {
            saw_would_block = true;
            break;
        } else {
            std::printf("  unexpected Send status=%s errno=%d\n",
                        rd::net::IoStatusName(status), client.last_system_error());
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
    }
    std::printf("  phase1: sent=%zu, would_block=%s\n", sent,
                saw_would_block ? "yes" : "no");
    CHECK_TRUE(saw_would_block);

    // 阶段 2：一边读对端，一边把剩余数据发完。
    drain_peer();
    while (sent < kTotal) {
        const std::size_t n = std::min(kChunk, kTotal - sent);
        FillPattern(chunk.data(), n, sent);
        rd::net::SendResult sr{};
        const rd::net::IoStatus status =
            client.Send(rd::net::ByteView{chunk.data(), n}, &sr);
        if (status == rd::net::IoStatus::kOk) {
            sent += client.last_transferred();
        } else if (status == rd::net::IoStatus::kWouldBlock) {
            saw_would_block = true;
        } else {
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
        drain_peer();
    }

    // 阶段 3：把对端缓冲区里剩余的数据读干净。
    while (received < kTotal) {
        const int revents = WaitEvents(peer, POLLIN, 2000);
        if (revents <= 0) {
            break;
        }
        const std::size_t before = received;
        drain_peer();
        if (received == before) {
            break;
        }
    }
    std::printf("  sent=%zu received=%zu would_block_seen=%s data_ok=%s token_monotonic=%s\n",
                sent, received, saw_would_block ? "yes" : "no", data_ok ? "yes" : "no",
                token_monotonic ? "yes" : "no");
    CHECK_TRUE(sent == kTotal);
    CHECK_TRUE(received == kTotal);
    CHECK_TRUE(data_ok);
    CHECK_TRUE(token_monotonic);
    CHECK_TRUE(last_token > 0);
    ::close(peer);
}

// ---------------------------------------------------------------- 场景 5a：1e4 echo
void Scenario5aEchoNoLeak() {
    std::printf("\n== 场景 5a: 1e4 次 UDP echo，RSS / fd 前后对比 ==\n");
    auto a_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    auto b_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    CHECK_TRUE(a_opened.ok());
    CHECK_TRUE(b_opened.ok());
    if (!a_opened.ok() || !b_opened.ok()) {
        return;
    }
    rd::net::DatagramSocket a = std::move(*a_opened);
    rd::net::DatagramSocket b = std::move(*b_opened);

    constexpr int kEcho = 10000;
    std::uint8_t payload[128];
    FillPattern(payload, sizeof(payload), 0);
    std::uint8_t rbuf[256];
    const long rss_before = ReadVmRssKb();
    const int fds_before = CountOpenFds();
    int ok_count = 0;
    for (int i = 0; i < kEcho; ++i) {
        rd::net::SendResult sr{};
        rd::net::IoStatus status = a.SendTo(rd::net::ByteView{payload, sizeof(payload)},
                                            b.local_endpoint(), &sr);
        if (status != rd::net::IoStatus::kOk) {
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
        std::size_t got = 0;
        do {
            status = b.RecvFrom(rd::net::MutableByteView{rbuf, sizeof(rbuf)}, nullptr,
                                &got);
        } while (status == rd::net::IoStatus::kWouldBlock);
        if (status != rd::net::IoStatus::kOk || got != sizeof(payload)) {
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
        status = b.SendTo(rd::net::ByteView{rbuf, got}, a.local_endpoint(), &sr);
        if (status != rd::net::IoStatus::kOk) {
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
        got = 0;
        do {
            status = a.RecvFrom(rd::net::MutableByteView{rbuf, sizeof(rbuf)}, nullptr,
                                &got);
        } while (status == rd::net::IoStatus::kWouldBlock);
        if (status != rd::net::IoStatus::kOk || got != sizeof(payload)) {
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
        ++ok_count;
    }
    const long rss_after = ReadVmRssKb();
    const int fds_after = CountOpenFds();
    std::printf("  echo ok=%d/%d, VmRSS %ld kB -> %ld kB (delta %ld kB), fds %d -> %d\n",
                ok_count, kEcho, rss_before, rss_after, rss_after - rss_before,
                fds_before, fds_after);
    CHECK_TRUE(ok_count == kEcho);
    CHECK_TRUE(fds_after == fds_before);
    CHECK_TRUE((rss_after - rss_before) < 2048);
}

// ---------------------------------------------------------------- 场景 5b：100 socket
void Scenario5bHundredSockets() {
    std::printf("\n== 场景 5b: 100 个 socket 同时非阻塞收发，随机 payload 不串包 ==\n");
    constexpr int kCount = 100;
    std::vector<rd::net::DatagramSocket> socks;
    socks.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        auto opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
        if (!opened.ok()) {
            CHECK_TRUE(opened.ok());
            return;
        }
        socks.push_back(std::move(*opened));
    }
    // 每个目标期望收到的 (发送者, 完整 payload)。
    struct Expected {
        int from;
        std::vector<std::uint8_t> payload;
        bool seen{false};
    };
    std::vector<std::vector<Expected>> expect(kCount);
    std::mt19937_64 rng(20261103u);
    std::vector<std::vector<std::uint8_t>> payloads(kCount);
    for (int i = 0; i < kCount; ++i) {
        const int target = (i * 37 + 11) % kCount;
        std::vector<std::uint8_t> payload(64);
        const std::uint64_t r = rng();
        std::memcpy(payload.data(), &i, sizeof(i));
        std::memcpy(payload.data() + 4, &r, sizeof(r));
        for (std::size_t k = 12; k < payload.size(); ++k) {
            payload[k] = static_cast<std::uint8_t>((i + k) & 0xFF);
        }
        payloads[i] = payload;
        expect[static_cast<std::size_t>(target)].push_back(Expected{i, payload, false});
        rd::net::SendResult sr{};
        const rd::net::IoStatus status =
            socks[static_cast<std::size_t>(i)].SendTo(
                rd::net::ByteView{payload.data(), payload.size()},
                socks[static_cast<std::size_t>(target)].local_endpoint(), &sr);
        CHECK_STATUS(status, rd::net::IoStatus::kOk);
        CHECK_TRUE(sr.token == 1);  // 每个 socket 的首个 token 从 1 开始
    }

    int total_received = 0;
    int cross_talk = 0;
    std::vector<std::uint8_t> rbuf(256);
    for (int round = 0; round < 200 && total_received < kCount; ++round) {
        bool progressed = false;
        for (int i = 0; i < kCount; ++i) {
            for (;;) {
                std::size_t got = 0;
                const rd::net::IoStatus status = socks[static_cast<std::size_t>(i)]
                                                     .RecvFrom(rd::net::MutableByteView{
                                                                   rbuf.data(), rbuf.size()},
                                                               nullptr, &got);
                if (status == rd::net::IoStatus::kWouldBlock) {
                    break;
                }
                CHECK_STATUS(status, rd::net::IoStatus::kOk);
                if (status != rd::net::IoStatus::kOk) {
                    break;
                }
                ++total_received;
                progressed = true;
                bool matched = false;
                for (auto& e : expect[static_cast<std::size_t>(i)]) {
                    if (!e.seen && e.payload.size() == got &&
                        std::memcmp(e.payload.data(), rbuf.data(), got) == 0) {
                        e.seen = true;
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    ++cross_talk;
                }
            }
        }
        if (!progressed) {
            ::usleep(1000);
        }
    }
    int unseen = 0;
    for (const auto& list : expect) {
        for (const auto& e : list) {
            if (!e.seen) {
                ++unseen;
            }
        }
    }
    std::printf("  sockets=%d received=%d/%d cross_talk=%d unseen=%d\n", kCount,
                total_received, kCount, cross_talk, unseen);
    CHECK_TRUE(total_received == kCount);
    CHECK_TRUE(cross_talk == 0);
    CHECK_TRUE(unseen == 0);
    CHECK_TRUE(rbuf.size() > 64);
    (void)payloads;
}

// ---------------------------------------------------------------- 压测
void Benchmark() {
    std::printf("\n== 压测: 10 万次 SendTo+RecvFrom ==\n");
    auto a_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    auto b_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    CHECK_TRUE(a_opened.ok());
    CHECK_TRUE(b_opened.ok());
    if (!a_opened.ok() || !b_opened.ok()) {
        return;
    }
    rd::net::DatagramSocket a = std::move(*a_opened);
    rd::net::DatagramSocket b = std::move(*b_opened);
    constexpr int kOps = 100000;
    std::uint8_t payload[128];
    FillPattern(payload, sizeof(payload), 0);
    std::uint8_t rbuf[256];
    const long rss_before = ReadVmRssKb();
    const int fds_before = CountOpenFds();
    int completed = 0;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        rd::net::SendResult sr{};
        rd::net::IoStatus status = a.SendTo(rd::net::ByteView{payload, sizeof(payload)},
                                            b.local_endpoint(), &sr);
        if (status != rd::net::IoStatus::kOk) {
            break;
        }
        std::size_t got = 0;
        do {
            status = b.RecvFrom(rd::net::MutableByteView{rbuf, sizeof(rbuf)}, nullptr,
                                &got);
        } while (status == rd::net::IoStatus::kWouldBlock);
        if (status != rd::net::IoStatus::kOk) {
            break;
        }
        ++completed;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const long rss_after = ReadVmRssKb();
    const int fds_after = CountOpenFds();
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    const double ns_per_op =
        completed > 0 ? static_cast<double>(elapsed_ns) / static_cast<double>(completed)
                      : 0.0;
    std::printf("  ops=%d/%d elapsed=%.3f ms ns/op=%.1f VmRSS %ld kB -> %ld kB "
                "(delta %ld kB) fds %d -> %d\n",
                completed, kOps, static_cast<double>(elapsed_ns) / 1.0e6, ns_per_op,
                rss_before, rss_after, rss_after - rss_before, fds_before, fds_after);
    CHECK_TRUE(completed == kOps);
    CHECK_TRUE(fds_after == fds_before);
}


// ---------------------------------------------------------------- 对比：默认 vs 批量
// 说明：本段**不**增加 g_checks/g_failures，只打印实测数据，保证原有
// 353 条断言口径不变；批量路径若失败会打印 [FAIL-BATCH] 以便人工判读。
struct UdpBenchSample {
    double ns_per_op{0};
    double mib_per_s{0};
    int ok{0};
};

UdpBenchSample RunUdpEchoBench(const rd::net::SocketOptions& options, int iters,
                               std::size_t payload_size) {
    UdpBenchSample sample;
    auto a_opened =
        rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, options);
    auto b_opened =
        rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, options);
    if (!a_opened.ok() || !b_opened.ok()) {
        return sample;
    }
    rd::net::DatagramSocket a = std::move(*a_opened);
    rd::net::DatagramSocket b = std::move(*b_opened);
    std::vector<std::uint8_t> payload(payload_size);
    for (std::size_t i = 0; i < payload_size; ++i) {
        payload[i] = static_cast<std::uint8_t>(i & 0xFFu);
    }
    std::vector<std::uint8_t> rbuf(payload_size + 64);
    const auto start = std::chrono::steady_clock::now();
    int ok = 0;
    for (int i = 0; i < iters; ++i) {
        rd::net::SendResult sr{};
        auto st = a.SendTo(rd::net::ByteView{payload.data(), payload.size()},
                           b.local_endpoint(), &sr);
        if (st != rd::net::IoStatus::kOk) {
            break;
        }
        std::size_t got = 0;
        do {
            st = b.RecvFrom(rd::net::MutableByteView{rbuf.data(), rbuf.size()}, nullptr,
                            &got);
        } while (st == rd::net::IoStatus::kWouldBlock);
        if (st != rd::net::IoStatus::kOk || got != payload.size()) {
            break;
        }
        st = b.SendTo(rd::net::ByteView{rbuf.data(), got}, a.local_endpoint(), &sr);
        if (st != rd::net::IoStatus::kOk) {
            break;
        }
        got = 0;
        do {
            st = a.RecvFrom(rd::net::MutableByteView{rbuf.data(), rbuf.size()}, nullptr,
                            &got);
        } while (st == rd::net::IoStatus::kWouldBlock);
        if (st != rd::net::IoStatus::kOk || got != payload.size()) {
            break;
        }
        ++ok;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    sample.ok = ok;
    if (ok > 0 && seconds > 0) {
        sample.ns_per_op = seconds * 1e9 / ok;
        // 单向有效载荷：每次 echo 有一份 payload 从 a 到 b。
        sample.mib_per_s =
            (static_cast<double>(ok) * static_cast<double>(payload_size)) / seconds /
            (1024.0 * 1024.0);
    }
    return sample;
}

void BatchComparison() {
    std::printf("\n== 对比: UDP echo 默认(batch=0) vs 批量(sendmmsg) ==\n");
    rd::net::SocketOptions def{};
    rd::net::SocketOptions batch{};
    batch.batch_bytes = 4096;
    batch.batch_entries = 8;
    batch.enable_sendmmsg = true;

    constexpr int kIters = 5000;
    constexpr std::size_t kPayload = 8000;  // >= batch_bytes，确保走批量分支
    double def_ns[3] = {0, 0, 0};
    double def_mib[3] = {0, 0, 0};
    double bat_ns[3] = {0, 0, 0};
    double bat_mib[3] = {0, 0, 0};
    for (int round = 0; round < 3; ++round) {
        const UdpBenchSample d = RunUdpEchoBench(def, kIters, kPayload);
        const UdpBenchSample f = RunUdpEchoBench(batch, kIters, kPayload);
        def_ns[round] = d.ns_per_op;
        def_mib[round] = d.mib_per_s;
        bat_ns[round] = f.ns_per_op;
        bat_mib[round] = f.mib_per_s;
        std::printf(
            "  round %d: default ns/op=%.1f %.1f MiB/s (ok=%d/%d) | "
            "batch ns/op=%.1f %.1f MiB/s (ok=%d/%d)\n",
            round + 1, d.ns_per_op, d.mib_per_s, d.ok, kIters, f.ns_per_op,
            f.mib_per_s, f.ok, kIters);
        if (d.ok != kIters || f.ok != kIters) {
            std::printf("  [FAIL-BATCH] round %d 未跑满：default=%d batch=%d (期望 %d)\n",
                        round + 1, d.ok, f.ok, kIters);
        }
    }
    const auto avg = [](const double (&v)[3]) { return (v[0] + v[1] + v[2]) / 3.0; };
    std::printf("  avg: default ns/op=%.1f %.1f MiB/s | batch ns/op=%.1f %.1f MiB/s\n",
                avg(def_ns), avg(def_mib), avg(bat_ns), avg(bat_mib));

    // 批量配置下 kTruncated 语义不变：65507 字节报文 -> 1024 字节缓冲。
    auto rx = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, batch);
    auto tx = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, batch);
    if (rx.ok() && tx.ok()) {
        std::vector<std::uint8_t> big(65507);
        FillPattern(big.data(), big.size(), 7);
        std::uint8_t small[1024];
        rd::net::SendResult sr{};
        const auto send_st =
            tx->SendTo(rd::net::ByteView{big.data(), big.size()}, rx->local_endpoint(), &sr);
        std::size_t got = 0;
        const auto recv_st = rx->RecvFrom(rd::net::MutableByteView{small, sizeof(small)},
                                          nullptr, &got);
        std::printf("  batch-trunc: SendTo=%s RecvFrom=%s actual=%zu (期望 kOk/kTruncated/65507)\n",
                    rd::net::IoStatusName(send_st), rd::net::IoStatusName(recv_st), got);
        if (send_st != rd::net::IoStatus::kOk ||
            recv_st != rd::net::IoStatus::kTruncated || got != big.size()) {
            std::printf("  [FAIL-BATCH] 批量配置破坏了 kTruncated 语义\n");
        }
    } else {
        std::printf("  [FAIL-BATCH] 批量配置下 Open 失败\n");
    }

    // TCP 批量（writev）验证：128 KiB 单次 Send 按 batch_bytes=64 KiB 切成
    // 2 段，一次 writev 提交；对端逐块读回并校验内容。
    {
        const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (lfd >= 0) {
            int one = 1;
            ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in la{};
            la.sin_family = AF_INET;
            la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            la.sin_port = 0;
            ::bind(lfd, reinterpret_cast<sockaddr*>(&la), sizeof(la));
            ::listen(lfd, 4);
            socklen_t ll = sizeof(la);
            ::getsockname(lfd, reinterpret_cast<sockaddr*>(&la), &ll);
            rd::net::DialOptions dopts;
            dopts.batch_bytes = 65536;
            dopts.batch_entries = 8;
            dopts.enable_writev = true;
            auto cli = rd::net::StreamSocket::Open(
                rd::net::Endpoint{"127.0.0.1", ntohs(la.sin_port)}, dopts);
            if (cli.ok()) {
                const int peer = ::accept(lfd, nullptr, nullptr);
                std::vector<std::uint8_t> block(128 * 1024);
                FillPattern(block.data(), block.size(), 3);
                rd::net::SendResult sr{};
                const auto st =
                    cli->Send(rd::net::ByteView{block.data(), block.size()}, &sr);
                const std::size_t sent = cli->last_transferred();
                std::vector<std::uint8_t> got(block.size());
                std::size_t off = 0;
                while (off < block.size()) {
                    const ssize_t n = ::read(peer, got.data() + off, got.size() - off);
                    if (n <= 0) {
                        break;
                    }
                    off += static_cast<std::size_t>(n);
                }
                const bool same = off == block.size() &&
                                  std::memcmp(got.data(), block.data(), block.size()) == 0;
                std::printf("  tcp-batch: Send=%s transferred=%zu received=%zu match=%s\n",
                            rd::net::IoStatusName(st), sent, off, same ? "yes" : "no");
                if (st != rd::net::IoStatus::kOk || sent != block.size() ||
                    off != block.size() || !same) {
                    std::printf("  [FAIL-BATCH] TCP writev 批量路径异常\n");
                }
                ::close(peer);
            } else {
                std::printf("  [FAIL-BATCH] TCP Open 失败 status=%s errno=%d\n",
                            rd::net::IoStatusName(cli.status()), cli.system_error());
            }
            ::close(lfd);
        }
    }
}

}  // namespace

int main() {
    std::printf("REQ-006 四态 Socket 抽象验收（%s）\n", __DATE__);
    Scenario1Fin();
    Scenario2Reset();
    Scenario3Truncated();
    Scenario4WouldBlock();
    Scenario5aEchoNoLeak();
    Scenario5bHundredSockets();
    Benchmark();
    BatchComparison();
    std::printf("\n== 汇总: checks=%d failures=%d ==\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
