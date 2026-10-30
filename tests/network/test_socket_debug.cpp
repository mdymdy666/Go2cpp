/**
 * @file test_socket_debug.cpp
 * @brief SocketDebug 最小实现的单元 + 真实场景测试。
 *
 * ## 验证策略
 * 1. **编译期契约**：OFF 构建断言 `Available()==false`、记录接口空转、
 *    dump 标注 unavailable；ON 构建断言可用性与开关语义；
 * 2. **计数口径**：send/recv 调用数、字节数、errors/timeouts 细分、last_errno；
 * 3. **事件环**：固定 256 条、覆盖最旧、`Events(fd, n)` 取最近 n 条且按
 *    seq 升序、seq 在单 fd 内连续不重复；
 * 4. **关闭归档**：close 后 Snapshot 仍可读（closed=true）、事件仍可查、
 *    fd 复用后新账本从 0 开始且归档不被误读；
 * 5. **并发**：8 线程写同一 fd，字节/调用数守恒，环形内 seq 无重复；
 * 6. **开关开销**：Disable 时记录被完全阻断，并打印 Enable/Disable 两种
 *    状态下 20 万次调用的 ns/op（只做量级参考，不做严格性能断言）；
 * 7. **真实场景（6 个，覆盖全部功能）**：TCP echo、多客户端、UDP 数据报、
 *    接收超时、connect 被拒、并发发送 + 关闭后快照。
 *
 * 该文件是独立可执行文件（自带 main），不并入 tests/test_main.cpp；
 * CMake 在两种构建（GO2CPP_DEBUG_SOCKET=ON/OFF）下都会注册它。
 */

#include "go2cpp/network/address.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/socket_debug.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::net::IPv4Address;
using go2cpp::net::Socket;
using go2cpp::net::SocketDebug;
using go2cpp::net::SocketEvent;
using go2cpp::net::SocketEventKind;
using go2cpp::net::SocketStats;

#if GO2CPP_DEBUG_SOCKET

/// @brief 每个用例开始前清空账本并打开记录，避免用例间互相污染。
void ResetAndEnable() {
    SocketDebug::Reset();
    (void)SocketDebug::Enable();
}

/// @brief 事件集合里是否存在指定类型。
bool HasKind(const std::vector<SocketEvent>& events, SocketEventKind kind) {
    return std::any_of(events.begin(), events.end(),
                       [kind](const SocketEvent& event) {
                           return event.kind == kind;
                       });
}

/// @brief 一对已建立连接的 TCP socket（用于真实场景）。
struct TcpPair {
    Socket::Ptr listener;
    Socket::Ptr client;
    Socket::Ptr server;
    bool ok() const { return listener && client && server; }
};

TcpPair MakeTcpPair(std::chrono::milliseconds timeout = 2000ms) {
    TcpPair pair;
    pair.listener = Socket::CreateTCP(AF_INET);
    if (!pair.listener) return pair;
    auto bind_address = IPv4Address::Create("127.0.0.1", 0);
    if (!bind_address || !pair.listener->bind(*bind_address) ||
        !pair.listener->listen(16)) {
        return pair;
    }
    auto address = std::dynamic_pointer_cast<IPv4Address>(
        pair.listener->local_address());
    if (!address) return pair;
    pair.client = Socket::CreateTCP(AF_INET);
    if (!pair.client || !pair.client->connect(*address, timeout)) return pair;
    pair.server = pair.listener->accept(timeout);
    return pair;
}

#endif  // GO2CPP_DEBUG_SOCKET

// ---------------------------------------------------------------------------
// 1. 编译期契约：两种构建都必须可执行
// ---------------------------------------------------------------------------
void CheckCompileTimeContract() {
    go2cpp_tests::announce("socket_debug compile-time contract");
#if GO2CPP_DEBUG_SOCKET
    GO2CPP_CHECK(SocketDebug::Available());
    SocketDebug::Disable();
    GO2CPP_CHECK(!SocketDebug::Enabled());
    // 重复 Enable 必须保持幂等：第二次仍返回 true 且开关仍为打开。
    GO2CPP_CHECK(SocketDebug::Enable());
    GO2CPP_CHECK(SocketDebug::Enable());
    GO2CPP_CHECK(SocketDebug::Enabled());
#else
    GO2CPP_CHECK(!SocketDebug::Available());
    GO2CPP_CHECK(!SocketDebug::Enable());
    SocketDebug::Disable();
    GO2CPP_CHECK(!SocketDebug::Enabled());
    // OFF 时所有记录接口都是 inline 空实现：不产生条目、不分配。
    const int fd = 99101;
    SocketDebug::RecordWrite(fd, 4096, 0);
    SocketDebug::RecordRead(fd, 4096, ETIMEDOUT);
    SocketDebug::RecordAccept(fd);
    SocketDebug::RecordConnect(fd, ECONNREFUSED, 1000);
    SocketDebug::RecordClose(fd);
    const SocketStats stats = SocketDebug::Snapshot(fd);
    GO2CPP_CHECK(!stats.tracked);
    GO2CPP_CHECK(stats.fd == -1);
    GO2CPP_CHECK(SocketDebug::Events(fd, 0).empty());
    GO2CPP_CHECK(SocketDebug::TrackedCount() == 0);
    GO2CPP_CHECK(SocketDebug::DumpToString().find("unavailable") !=
                 std::string::npos);
#endif
}

#if GO2CPP_DEBUG_SOCKET

// ---------------------------------------------------------------------------
// 2. 计数口径
// ---------------------------------------------------------------------------
void CheckCounters() {
    go2cpp_tests::announce("socket_debug counters");
    ResetAndEnable();
    const int fd = 99201;
    SocketDebug::RecordWrite(fd, 100, 0);
    SocketDebug::RecordWrite(fd, 200, 0);
    SocketDebug::RecordWrite(fd, 0, EPIPE);
    SocketDebug::RecordRead(fd, 500, 0);
    SocketDebug::RecordRead(fd, 0, ETIMEDOUT);
    const SocketStats stats = SocketDebug::Snapshot(fd);
    GO2CPP_CHECK(stats.tracked);
    GO2CPP_CHECK(!stats.closed);
    GO2CPP_CHECK(stats.fd == fd);
    GO2CPP_CHECK(stats.send_calls == 3);
    GO2CPP_CHECK(stats.bytes_sent == 300);
    GO2CPP_CHECK(stats.recv_calls == 2);
    GO2CPP_CHECK(stats.bytes_recv == 500);
    GO2CPP_CHECK(stats.errors == 2);
    GO2CPP_CHECK(stats.timeouts == 1);
    GO2CPP_CHECK(stats.events == 5);
    GO2CPP_CHECK(stats.last_errno == ETIMEDOUT);
    GO2CPP_CHECK(stats.last_activity_ns > 0);
    // 事件顺序必须是调用顺序：write/write/error/read/timeout。
    const auto events = SocketDebug::Events(fd, 0);
    GO2CPP_CHECK(events.size() == 5);
    GO2CPP_CHECK(events[0].kind == SocketEventKind::kWrite);
    GO2CPP_CHECK(events[0].bytes == 100);
    GO2CPP_CHECK(events[2].kind == SocketEventKind::kError);
    GO2CPP_CHECK(events[2].error == EPIPE);
    GO2CPP_CHECK(events[4].kind == SocketEventKind::kTimeout);
}

// ---------------------------------------------------------------------------
// 3. 事件环容量与覆盖
// ---------------------------------------------------------------------------
void CheckRingCapacity() {
    go2cpp_tests::announce("socket_debug ring capacity 256");
    ResetAndEnable();
    const int fd = 99202;
    constexpr std::uint64_t kTotal = 300;
    for (std::uint64_t index = 0; index < kTotal; ++index) {
        SocketDebug::RecordWrite(fd, 1, 0);
    }
    const SocketStats stats = SocketDebug::Snapshot(fd);
    GO2CPP_CHECK(stats.send_calls == kTotal);
    GO2CPP_CHECK(stats.bytes_sent == kTotal);
    GO2CPP_CHECK(stats.events == kTotal);

    const auto all = SocketDebug::Events(fd, 0);
    GO2CPP_CHECK(all.size() == SocketDebug::kEventRingCapacity);
    // 写满后保留最近 256 条：seq 从 300-256+1 = 45 开始。
    GO2CPP_CHECK(all.front().seq == kTotal - 256 + 1);
    GO2CPP_CHECK(all.back().seq == kTotal);
    for (std::size_t index = 1; index < all.size(); ++index) {
        GO2CPP_CHECK(all[index].seq == all[index - 1].seq + 1);
    }

    const auto tail = SocketDebug::Events(fd, 10);
    GO2CPP_CHECK(tail.size() == 10);
    GO2CPP_CHECK(tail.front().seq == kTotal - 10 + 1);
    GO2CPP_CHECK(tail.back().seq == kTotal);
}

// ---------------------------------------------------------------------------
// 4. 事件类型映射
// ---------------------------------------------------------------------------
void CheckEventKinds() {
    go2cpp_tests::announce("socket_debug event kinds");
    ResetAndEnable();
    const int fd = 99203;
    SocketDebug::RecordConnect(fd, 0, 1500);
    SocketDebug::RecordConnect(fd, ETIMEDOUT, 9000);
    SocketDebug::RecordAccept(fd);
    SocketDebug::RecordRead(fd, 0, ECONNRESET);
    SocketDebug::RecordClose(fd);
    const auto events = SocketDebug::Events(fd, 0);
    GO2CPP_CHECK(events.size() == 5);
    GO2CPP_CHECK(events[0].kind == SocketEventKind::kConnect);
    GO2CPP_CHECK(events[0].elapsed_ns == 1500);
    GO2CPP_CHECK(events[1].kind == SocketEventKind::kTimeout);
    GO2CPP_CHECK(events[1].elapsed_ns == 9000);
    GO2CPP_CHECK(events[2].kind == SocketEventKind::kAccept);
    GO2CPP_CHECK(events[3].kind == SocketEventKind::kError);
    GO2CPP_CHECK(events[4].kind == SocketEventKind::kClose);
}

// ---------------------------------------------------------------------------
// 5. 关闭归档 + fd 复用
// ---------------------------------------------------------------------------
void CheckCloseAndArchive() {
    go2cpp_tests::announce("socket_debug close archive and fd reuse");
    ResetAndEnable();
    const int fd = 99204;
    SocketDebug::RecordWrite(fd, 64, 0);
    SocketDebug::RecordClose(fd);
    SocketStats closed = SocketDebug::Snapshot(fd);
    GO2CPP_CHECK(closed.tracked);
    GO2CPP_CHECK(closed.closed);
    GO2CPP_CHECK(closed.bytes_sent == 64);
    GO2CPP_CHECK(closed.closes == 1);
    GO2CPP_CHECK(SocketDebug::TrackedCount() == 0);
    const auto archived = SocketDebug::Events(fd, 0);
    GO2CPP_CHECK(archived.size() == 2);
    GO2CPP_CHECK(archived.back().kind == SocketEventKind::kClose);
    // 重复 close 不得重复计数。
    SocketDebug::RecordClose(fd);
    GO2CPP_CHECK(SocketDebug::Snapshot(fd).closes == 1);

    // fd 号复用：新账本从 0 开始，归档里的旧条目被移除。
    SocketDebug::RecordWrite(fd, 8, 0);
    const SocketStats reused = SocketDebug::Snapshot(fd);
    GO2CPP_CHECK(!reused.closed);
    GO2CPP_CHECK(reused.bytes_sent == 8);
    GO2CPP_CHECK(reused.send_calls == 1);
    GO2CPP_CHECK(reused.closes == 0);
    // 只有 2 条事件（旧归档被移除，新条目重新计数）。
    GO2CPP_CHECK(SocketDebug::Events(fd, 0).size() == 1);

    // 从未跟踪的 fd 调用 close：静默无操作。
    SocketDebug::RecordClose(99999);
    GO2CPP_CHECK(!SocketDebug::Snapshot(99999).tracked);
}

// ---------------------------------------------------------------------------
// 6. 未知 fd
// ---------------------------------------------------------------------------
void CheckUnknownFd() {
    go2cpp_tests::announce("socket_debug unknown fd");
    ResetAndEnable();
    const SocketStats stats = SocketDebug::Snapshot(99888);
    GO2CPP_CHECK(!stats.tracked);
    GO2CPP_CHECK(stats.fd == -1);
    GO2CPP_CHECK(stats.bytes_sent == 0);
    GO2CPP_CHECK(SocketDebug::Events(99888, 0).empty());
    GO2CPP_CHECK(SocketDebug::Events(-1, 4).empty());
    GO2CPP_CHECK(SocketDebug::TrackedCount() == 0);
}

// ---------------------------------------------------------------------------
// 7. DumpTo 文本
// ---------------------------------------------------------------------------
void CheckDump() {
    go2cpp_tests::announce("socket_debug dump text");
    ResetAndEnable();
    SocketDebug::RecordWrite(99301, 128, 0);
    SocketDebug::RecordClose(99301);
    SocketDebug::RecordRead(99302, 32, ETIMEDOUT);
    std::string text;
    SocketDebug::DumpTo(text);
    GO2CPP_CHECK(text.find("socket_debug: available=1") != std::string::npos);
    GO2CPP_CHECK(text.find("enabled=1") != std::string::npos);
    GO2CPP_CHECK(text.find("tracked=1") != std::string::npos);
    GO2CPP_CHECK(text.find("finished=1") != std::string::npos);
    GO2CPP_CHECK(text.find("fd=99301 state=closed") != std::string::npos);
    GO2CPP_CHECK(text.find("fd=99302 state=open") != std::string::npos);
    GO2CPP_CHECK(text.find("bytes_sent=128") != std::string::npos);
    GO2CPP_CHECK(text.find("timeouts=1") != std::string::npos);
    GO2CPP_CHECK(SocketDebug::DumpToString() == text);
}

// ---------------------------------------------------------------------------
// 8. 并发计数
// ---------------------------------------------------------------------------
void CheckConcurrentCounters() {
    go2cpp_tests::announce("socket_debug concurrent counters");
    ResetAndEnable();
    const int fd = 99401;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 5000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([fd] {
            for (int iteration = 0; iteration < kPerThread; ++iteration) {
                SocketDebug::RecordWrite(fd, 10, 0);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const SocketStats stats = SocketDebug::Snapshot(fd);
    GO2CPP_CHECK(stats.send_calls ==
                 static_cast<std::uint64_t>(kThreads * kPerThread));
    GO2CPP_CHECK(stats.bytes_sent ==
                 static_cast<std::uint64_t>(kThreads * kPerThread * 10));
    GO2CPP_CHECK(stats.events ==
                 static_cast<std::uint64_t>(kThreads * kPerThread));
    const auto events = SocketDebug::Events(fd, 0);
    GO2CPP_CHECK(events.size() == SocketDebug::kEventRingCapacity);
    std::vector<std::uint64_t> seqs;
    seqs.reserve(events.size());
    for (const auto& event : events) {
        seqs.push_back(event.seq);
    }
    std::sort(seqs.begin(), seqs.end());
    GO2CPP_CHECK(std::adjacent_find(seqs.begin(), seqs.end()) == seqs.end());
    // 并发下 seq 仍连续（每次写事件都在同一把 event_mutex 内取号）。
    GO2CPP_CHECK(seqs.back() - seqs.front() + 1 == seqs.size());
}

// ---------------------------------------------------------------------------
// 9. 开关语义 + 开销
// ---------------------------------------------------------------------------
void CheckDisableBlocksRecording() {
    go2cpp_tests::announce("socket_debug disable blocks recording");
    ResetAndEnable();
    const int fd = 99501;
    SocketDebug::RecordWrite(fd, 10, 0);
    SocketDebug::Disable();
    GO2CPP_CHECK(!SocketDebug::Enabled());
    SocketDebug::RecordWrite(fd, 10, 0);
    SocketDebug::RecordRead(fd, 10, 0);
    const SocketStats paused = SocketDebug::Snapshot(fd);
    GO2CPP_CHECK(paused.send_calls == 1);
    GO2CPP_CHECK(paused.bytes_sent == 10);
    GO2CPP_CHECK(paused.recv_calls == 0);
    // 重新打开后继续累计，不丢失既有账本。
    (void)SocketDebug::Enable();
    SocketDebug::RecordWrite(fd, 10, 0);
    GO2CPP_CHECK(SocketDebug::Snapshot(fd).send_calls == 2);
}

void CheckOverhead() {
    go2cpp_tests::announce("socket_debug enable/disable overhead");
    ResetAndEnable();
    const int fd = 99502;
    constexpr int kIterations = 200000;
    // 先热身，避免把首次分配算进 Disable 路径。
    for (int index = 0; index < 1000; ++index) {
        SocketDebug::RecordWrite(fd, 1, 0);
    }
    const auto measure = [&](bool enabled) {
        if (enabled) {
            (void)SocketDebug::Enable();
        } else {
            SocketDebug::Disable();
        }
        const auto started = std::chrono::steady_clock::now();
        for (int index = 0; index < kIterations; ++index) {
            // 模拟 socket.cpp 的接入方式：先判运行时开关再记录。
            if (SocketDebug::Enabled()) {
                SocketDebug::RecordWrite(fd, 1, 0);
            }
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - started)
            .count();
    };
    const auto disabled_ns = measure(false);
    const auto before = SocketDebug::Snapshot(fd).send_calls;
    const auto enabled_ns = measure(true);
    const auto after = SocketDebug::Snapshot(fd).send_calls;
    const double disabled_per_op =
        static_cast<double>(disabled_ns) / static_cast<double>(kIterations);
    const double enabled_per_op =
        static_cast<double>(enabled_ns) / static_cast<double>(kIterations);
    std::printf("[socket_debug] disable=%.2f ns/op enable=%.2f ns/op "
                "(n=%d)\n",
                disabled_per_op, enabled_per_op, kIterations);
    // Disable 路径必须完全不记账（硬断言），耗时只做量级断言（本机实测
    // 个位数 ns，阈值放到 1us 以兼容虚拟化/CI 抖动）。
    GO2CPP_CHECK(after - before == static_cast<std::uint64_t>(kIterations));
    GO2CPP_CHECK(disabled_per_op < 1000.0);
}

// ---------------------------------------------------------------------------
// 10. 真实场景 1：TCP echo 全链路
// ---------------------------------------------------------------------------
void ScenarioTcpEcho() {
    go2cpp_tests::announce("socket_debug scenario tcp echo");
    ResetAndEnable();
    TcpPair pair = MakeTcpPair();
    GO2CPP_REQUIRE(pair.ok());
    const int client_fd = pair.client->fd();
    const int server_fd = pair.server->fd();

    const char request[] = "hello";
    const auto sent = pair.client->send(request, 5);
    GO2CPP_REQUIRE(sent.ok() && sent.transferred == 5);
    char buffer[16]{};
    const auto received = pair.server->recv(buffer, sizeof(buffer));
    GO2CPP_REQUIRE(received.ok() && received.transferred == 5);
    GO2CPP_CHECK(std::memcmp(buffer, request, 5) == 0);

    const auto reply = pair.server->send(request, 5);
    GO2CPP_REQUIRE(reply.ok() && reply.transferred == 5);
    char reply_buffer[16]{};
    const auto reply_received =
        pair.client->recv(reply_buffer, sizeof(reply_buffer));
    GO2CPP_REQUIRE(reply_received.ok() && reply_received.transferred == 5);

    const SocketStats client_stats = SocketDebug::Snapshot(client_fd);
    GO2CPP_CHECK(client_stats.tracked);
    GO2CPP_CHECK(client_stats.connects == 1);
    GO2CPP_CHECK(client_stats.connect_ok == 1);
    GO2CPP_CHECK(client_stats.connect_time_ns > 0);
    GO2CPP_CHECK(client_stats.send_calls == 1);
    GO2CPP_CHECK(client_stats.bytes_sent == 5);
    GO2CPP_CHECK(client_stats.recv_calls == 1);
    GO2CPP_CHECK(client_stats.bytes_recv == 5);
    GO2CPP_CHECK(client_stats.errors == 0);
    GO2CPP_CHECK(client_stats.accepts == 0);

    const SocketStats server_stats = SocketDebug::Snapshot(server_fd);
    GO2CPP_CHECK(server_stats.accepts == 1);
    GO2CPP_CHECK(server_stats.bytes_recv == 5);
    GO2CPP_CHECK(!server_stats.closed);

    const auto client_events = SocketDebug::Events(client_fd, 0);
    GO2CPP_CHECK(HasKind(client_events, SocketEventKind::kConnect));
    GO2CPP_CHECK(HasKind(client_events, SocketEventKind::kWrite));
    GO2CPP_CHECK(HasKind(client_events, SocketEventKind::kRead));
    const auto server_events = SocketDebug::Events(server_fd, 0);
    GO2CPP_CHECK(HasKind(server_events, SocketEventKind::kAccept));

    GO2CPP_CHECK(pair.client->close());
    const SocketStats closed = SocketDebug::Snapshot(client_fd);
    GO2CPP_CHECK(closed.closed);
    GO2CPP_CHECK(closed.closes == 1);
    GO2CPP_CHECK(closed.bytes_sent == 5);
    GO2CPP_CHECK(HasKind(SocketDebug::Events(client_fd, 0),
                         SocketEventKind::kClose));
    GO2CPP_CHECK(pair.server->close());
    GO2CPP_CHECK(pair.listener->close());
}

// ---------------------------------------------------------------------------
// 11. 真实场景 2：多客户端各自独立账本
// ---------------------------------------------------------------------------
void ScenarioMultiClient() {
    go2cpp_tests::announce("socket_debug scenario multi client");
    ResetAndEnable();
    auto listener = Socket::CreateTCP(AF_INET);
    GO2CPP_REQUIRE(listener);
    auto bind_address = IPv4Address::Create("127.0.0.1", 0);
    GO2CPP_REQUIRE(listener->bind(*bind_address));
    GO2CPP_REQUIRE(listener->listen(16));
    auto address =
        std::dynamic_pointer_cast<IPv4Address>(listener->local_address());
    GO2CPP_REQUIRE(address != nullptr);

    constexpr int kClients = 8;
    std::vector<Socket::Ptr> clients;
    std::vector<Socket::Ptr> servers;
    std::vector<int> client_fds;
    std::vector<int> server_fds;
    for (int index = 0; index < kClients; ++index) {
        auto client = Socket::CreateTCP(AF_INET);
        GO2CPP_REQUIRE(client && client->connect(*address, 2000ms));
        auto server = listener->accept(2000ms);
        GO2CPP_REQUIRE(server);
        const char payload[16] = "payload";
        GO2CPP_REQUIRE(client->send(payload, 7).ok());
        char buffer[16]{};
        GO2CPP_REQUIRE(server->recv(buffer, sizeof(buffer)).ok());
        client_fds.push_back(client->fd());
        server_fds.push_back(server->fd());
        clients.push_back(std::move(client));
        servers.push_back(std::move(server));
    }
    for (int index = 0; index < kClients; ++index) {
        const SocketStats client_stats =
            SocketDebug::Snapshot(client_fds[static_cast<std::size_t>(index)]);
        const SocketStats server_stats =
            SocketDebug::Snapshot(server_fds[static_cast<std::size_t>(index)]);
        GO2CPP_CHECK(client_stats.connect_ok == 1);
        GO2CPP_CHECK(client_stats.bytes_sent == 7);
        GO2CPP_CHECK(server_stats.accepts == 1);
        GO2CPP_CHECK(server_stats.bytes_recv == 7);
        // 账本按 fd 隔离：每个客户端只看到自己的 7 字节。
        GO2CPP_CHECK(client_stats.bytes_recv == 0);
        GO2CPP_CHECK(server_stats.bytes_sent == 0);
    }
    for (auto& client : clients) {
        GO2CPP_CHECK(client->close());
    }
    for (auto& server : servers) {
        GO2CPP_CHECK(server->close());
    }
    GO2CPP_CHECK(listener->close());
}

// ---------------------------------------------------------------------------
// 12. 真实场景 3：UDP send_to / recv_from
// ---------------------------------------------------------------------------
void ScenarioUdp() {
    go2cpp_tests::announce("socket_debug scenario udp datagram");
    ResetAndEnable();
    auto server = Socket::CreateUDP(AF_INET);
    GO2CPP_REQUIRE(server);
    auto bind_address = IPv4Address::Create("127.0.0.1", 0);
    GO2CPP_REQUIRE(server->bind(*bind_address));
    auto address =
        std::dynamic_pointer_cast<IPv4Address>(server->local_address());
    GO2CPP_REQUIRE(address != nullptr);
    auto client = Socket::CreateUDP(AF_INET);
    GO2CPP_REQUIRE(client);

    const char payload[] = "ping";
    const auto sent = client->send_to(payload, 4, *address);
    GO2CPP_REQUIRE(sent.ok() && sent.transferred == 4);
    char buffer[16]{};
    go2cpp::net::AddressPtr from;
    const auto received = server->recv_from(buffer, sizeof(buffer), from);
    GO2CPP_REQUIRE(received.ok() && received.transferred == 4);
    GO2CPP_REQUIRE(from != nullptr);

    const SocketStats client_stats = SocketDebug::Snapshot(client->fd());
    GO2CPP_CHECK(client_stats.send_calls == 1);
    GO2CPP_CHECK(client_stats.bytes_sent == 4);
    const SocketStats server_stats = SocketDebug::Snapshot(server->fd());
    GO2CPP_CHECK(server_stats.recv_calls == 1);
    GO2CPP_CHECK(server_stats.bytes_recv == 4);
    GO2CPP_CHECK(client->close());
    GO2CPP_CHECK(server->close());
}

// ---------------------------------------------------------------------------
// 13. 真实场景 4：接收超时 -> timeouts 计数 + kTimeout 事件
// ---------------------------------------------------------------------------
void ScenarioRecvTimeout() {
    go2cpp_tests::announce("socket_debug scenario recv timeout");
    ResetAndEnable();
    TcpPair pair = MakeTcpPair();
    GO2CPP_REQUIRE(pair.ok());
    const int client_fd = pair.client->fd();
    char buffer[8]{};
    const auto result = pair.client->recv(buffer, sizeof(buffer), 0, 50ms);
    GO2CPP_CHECK(!result.ok());
    GO2CPP_CHECK(result.error == ETIMEDOUT);

    const SocketStats stats = SocketDebug::Snapshot(client_fd);
    GO2CPP_CHECK(stats.recv_calls == 1);
    GO2CPP_CHECK(stats.timeouts == 1);
    GO2CPP_CHECK(stats.errors == 1);
    GO2CPP_CHECK(stats.last_errno == ETIMEDOUT);
    const auto events = SocketDebug::Events(client_fd, 0);
    GO2CPP_CHECK(!events.empty());
    GO2CPP_CHECK(events.back().kind == SocketEventKind::kTimeout);
    GO2CPP_CHECK(events.back().error == ETIMEDOUT);
    GO2CPP_CHECK(pair.client->close());
    GO2CPP_CHECK(pair.server->close());
    GO2CPP_CHECK(pair.listener->close());
}

// ---------------------------------------------------------------------------
// 14. 真实场景 5：connect 被拒 -> errors 计数
// ---------------------------------------------------------------------------
void ScenarioConnectRefused() {
    go2cpp_tests::announce("socket_debug scenario connect refused");
    ResetAndEnable();
    // 先监听拿到端口，再关闭监听，保证该端口上没有人 accept。
    auto probe = Socket::CreateTCP(AF_INET);
    GO2CPP_REQUIRE(probe);
    auto bind_address = IPv4Address::Create("127.0.0.1", 0);
    GO2CPP_REQUIRE(probe->bind(*bind_address));
    GO2CPP_REQUIRE(probe->listen(1));
    auto address =
        std::dynamic_pointer_cast<IPv4Address>(probe->local_address());
    GO2CPP_REQUIRE(address != nullptr);
    const std::uint16_t port = address->port();
    GO2CPP_REQUIRE(probe->close());

    auto client = Socket::CreateTCP(AF_INET);
    GO2CPP_REQUIRE(client);
    auto target = IPv4Address::Create("127.0.0.1", port);
    GO2CPP_REQUIRE(target != nullptr);
    const bool connected = client->connect(*target, 500ms);
    GO2CPP_CHECK(!connected);
    const SocketStats stats = SocketDebug::Snapshot(client->fd());
    GO2CPP_CHECK(stats.connects == 1);
    GO2CPP_CHECK(stats.connect_ok == 0);
    GO2CPP_CHECK(stats.errors == 1);
    GO2CPP_CHECK(stats.last_errno == ECONNREFUSED ||
                 stats.last_errno == ETIMEDOUT);
    GO2CPP_CHECK(stats.connect_time_ns > 0);
    const auto events = SocketDebug::Events(client->fd(), 0);
    GO2CPP_CHECK(!events.empty());
    GO2CPP_CHECK(events.back().kind == SocketEventKind::kError ||
                 events.back().kind == SocketEventKind::kTimeout);
    GO2CPP_CHECK(client->close());
}

// ---------------------------------------------------------------------------
// 15. 真实场景 6：并发发送 + 关闭后快照
// ---------------------------------------------------------------------------
void ScenarioConcurrentSend() {
    go2cpp_tests::announce("socket_debug scenario concurrent send");
    ResetAndEnable();
    TcpPair pair = MakeTcpPair();
    GO2CPP_REQUIRE(pair.ok());
    const int client_fd = pair.client->fd();
    const int server_fd = pair.server->fd();
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;
    constexpr std::size_t kChunk = 32;
    const std::string chunk(kChunk, 'x');
    std::atomic<int> send_failures{0};
    std::vector<std::thread> senders;
    senders.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        senders.emplace_back([&pair, &chunk, &send_failures] {
            for (int iteration = 0; iteration < kPerThread; ++iteration) {
                const auto result = pair.client->send(chunk.data(), chunk.size());
                if (!result.ok()) {
                    send_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    std::size_t received_total = 0;
    std::vector<char> buffer(4096);
    while (received_total <
           static_cast<std::size_t>(kThreads * kPerThread) * kChunk) {
        const auto result = pair.server->recv(buffer.data(), buffer.size(), 0,
                                              5000ms);
        GO2CPP_REQUIRE(result.ok());
        received_total += static_cast<std::size_t>(result.transferred);
    }
    for (auto& sender : senders) {
        sender.join();
    }
    GO2CPP_CHECK(send_failures.load() == 0);
    GO2CPP_CHECK(received_total ==
                 static_cast<std::size_t>(kThreads * kPerThread) * kChunk);

    const SocketStats stats = SocketDebug::Snapshot(client_fd);
    GO2CPP_CHECK(stats.send_calls ==
                 static_cast<std::uint64_t>(kThreads * kPerThread));
    GO2CPP_CHECK(stats.bytes_sent ==
                 static_cast<std::uint64_t>(kThreads * kPerThread * kChunk));
    GO2CPP_CHECK(stats.events >=
                 static_cast<std::uint64_t>(kThreads * kPerThread));
    const auto events = SocketDebug::Events(client_fd, 0);
    GO2CPP_CHECK(events.size() == SocketDebug::kEventRingCapacity);
    const SocketStats server_stats = SocketDebug::Snapshot(server_fd);
    GO2CPP_CHECK(server_stats.bytes_recv ==
                 static_cast<std::uint64_t>(kThreads * kPerThread * kChunk));
    GO2CPP_CHECK(server_stats.accepts == 1);

    GO2CPP_CHECK(pair.client->close());
    const SocketStats closed = SocketDebug::Snapshot(client_fd);
    GO2CPP_CHECK(closed.closed);
    GO2CPP_CHECK(closed.bytes_sent ==
                 static_cast<std::uint64_t>(kThreads * kPerThread * kChunk));
    GO2CPP_CHECK(pair.server->close());
    GO2CPP_CHECK(pair.listener->close());
}

void RunOnBuildTests() {
    CheckCounters();
    CheckRingCapacity();
    CheckEventKinds();
    CheckCloseAndArchive();
    CheckUnknownFd();
    CheckDump();
    CheckConcurrentCounters();
    CheckDisableBlocksRecording();
    CheckOverhead();
    ScenarioTcpEcho();
    ScenarioMultiClient();
    ScenarioUdp();
    ScenarioRecvTimeout();
    ScenarioConnectRefused();
    ScenarioConcurrentSend();
}

#endif  // GO2CPP_DEBUG_SOCKET

}  // namespace

int main() {
    std::printf("[socket_debug] build=%s\n",
                GO2CPP_DEBUG_SOCKET ? "ON" : "OFF");
    CheckCompileTimeContract();
#if GO2CPP_DEBUG_SOCKET
    RunOnBuildTests();
#endif
    const int failures = go2cpp_tests::g_failures.load();
    if (failures == 0) {
        std::printf("[socket_debug] failures=0\n");
        return 0;
    }
    std::printf("[socket_debug] failures=%d\n", failures);
    return 1;
}
