/**
 * @file test_net_batch.cpp
 * @brief 批量系统调用（recvmmsg/sendmmsg）与零拷贝（sendfile）测试。
 * @details 覆盖：参数校验、UDP 批量收发、部分完成、超时、跨线程取消、
 *          连接/非连接两种发送模式、sendfile 的 offset/EOF/慢读/对端关闭、
 *          Fiber 内使用、300 轮随机批大小与包长，以及 5 个真实场景。
 *          本文件自带 main，可独立构建运行（不依赖 go2cpp_tests 框架）。
 * @note 构建：g++ -std=c++20 -O2 -Iinclude tests/network/test_net_batch.cpp \
 *             -o /tmp/test_net_batch -L<build> -lgo2cpp_network -lgo2cpp_io ...
 */

#include "go2cpp/network/socket.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::net::IPv4Address;
using go2cpp::net::RecvBatchEntry;
using go2cpp::net::SendBatchEntry;
using go2cpp::net::Socket;
using go2cpp::net::SocketResult;

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::milliseconds;

constexpr Duration kShortTimeout{50};
constexpr Duration kWaitTimeout{3000};

int g_checks = 0;
int g_failures = 0;

void ReportFailure(const char* file, int line, const char* text) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, text);
}

#define CHECK(condition)                                                  \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(condition)) ReportFailure(__FILE__, __LINE__, #condition);  \
    } while (0)

#define REQUIRE(condition)                                                \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(condition)) {                                               \
            ReportFailure(__FILE__, __LINE__, #condition);                \
            return;                                                       \
        }                                                                 \
    } while (0)

/// UDP 收发对：接收端已 bind，发送端可选 connect。
struct UdpPair {
    Socket::Ptr receiver;
    Socket::Ptr sender;
    go2cpp::net::AddressPtr receiver_address;
};

bool MakeUdpPair(UdpPair* pair, bool connected, Duration recv_timeout = kWaitTimeout) {
    pair->receiver = Socket::CreateUDP(AF_INET);
    if (!pair->receiver) return false;
    const auto bind_address = IPv4Address::Create("127.0.0.1", 0);
    if (!bind_address || !pair->receiver->bind(*bind_address)) return false;
    pair->receiver->set_recv_timeout(recv_timeout);
    pair->receiver_address = pair->receiver->local_address();
    if (!pair->receiver_address) return false;
    pair->sender = Socket::CreateUDP(AF_INET);
    if (!pair->sender) return false;
    pair->sender->set_send_timeout(kWaitTimeout);
    if (connected && !pair->sender->connect(*pair->receiver_address, kWaitTimeout)) {
        return false;
    }
    return true;
}

/// 生成 count 个数据报；前 4 字节是全局序号，后续字节由序号派生。
std::vector<std::vector<std::uint8_t>> MakePayloads(std::size_t count, std::size_t size,
                                                   std::uint32_t first_sequence = 0) {
    if (size < sizeof(std::uint32_t)) size = sizeof(std::uint32_t);
    std::vector<std::vector<std::uint8_t>> payloads(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t sequence = first_sequence + static_cast<std::uint32_t>(index);
        payloads[index].resize(size);
        std::memcpy(payloads[index].data(), &sequence, sizeof(sequence));
        for (std::size_t offset = sizeof(sequence); offset < size; ++offset) {
            payloads[index][offset] =
                static_cast<std::uint8_t>((sequence * 131U + offset) & 0xFFU);
        }
    }
    return payloads;
}

/// 批量发送；chunk==1 时也走 send_batch（对照单包 API 的基准在 bench 文件）。
bool SendPayloads(Socket& sender, const go2cpp::net::AddressPtr& destination,
                  const std::vector<std::vector<std::uint8_t>>& payloads,
                  std::size_t chunk) {
    std::vector<SendBatchEntry> entries(chunk);
    for (std::size_t offset = 0; offset < payloads.size(); offset += chunk) {
        const std::size_t count = std::min(chunk, payloads.size() - offset);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& payload = payloads[offset + index];
            entries[index].data = payload.data();
            entries[index].length = payload.size();
            entries[index].to = destination ? destination->addr() : nullptr;
            entries[index].to_length = destination ? destination->addr_len() : 0;
        }
        std::size_t sent = 0;
        while (sent < count) {
            const auto result = sender.send_batch(entries.data() + sent, count - sent, 0,
                                                  kWaitTimeout);
            if (!result.ok() || result.transferred == 0) {
                std::fprintf(stderr, "send_batch error=%d transferred=%zd\n", result.error,
                             result.transferred);
                return false;
            }
            sent += static_cast<std::size_t>(result.transferred);
        }
    }
    return true;
}

struct ReceivedPacket {
    std::vector<std::uint8_t> data;
    sockaddr_storage from{};
    socklen_t from_length{0};
};

/// 批量接收 total 个包，分 ceil(total/chunk) 轮 recv_batch。
bool ReceivePayloads(Socket& receiver, std::size_t total, std::size_t chunk,
                     std::vector<ReceivedPacket>* out) {
    std::vector<RecvBatchEntry> entries(chunk);
    std::vector<std::vector<std::uint8_t>> buffers(chunk);
    while (out->size() < total) {
        const std::size_t want = std::min(chunk, total - out->size());
        for (std::size_t index = 0; index < want; ++index) {
            buffers[index].resize(2048);
            entries[index] = RecvBatchEntry{};
            entries[index].data = buffers[index].data();
            entries[index].capacity = buffers[index].size();
        }
        const auto result = receiver.recv_batch(entries.data(), want, 0, kWaitTimeout);
        if (!result.ok() || result.transferred <= 0) {
            std::fprintf(stderr, "recv_batch error=%d transferred=%zd\n", result.error,
                         result.transferred);
            return false;
        }
        for (ssize_t index = 0; index < result.transferred; ++index) {
            ReceivedPacket packet;
            packet.data.assign(buffers[static_cast<std::size_t>(index)].data(),
                               buffers[static_cast<std::size_t>(index)].data() +
                                   entries[static_cast<std::size_t>(index)].received);
            packet.from = entries[static_cast<std::size_t>(index)].from;
            packet.from_length = entries[static_cast<std::size_t>(index)].from_length;
            out->push_back(std::move(packet));
        }
    }
    return true;
}

/// 校验：数量一致、序号无重复无缺失、内容逐字节一致、来源地址有效。
bool VerifyPackets(const std::vector<ReceivedPacket>& packets,
                   const std::vector<std::vector<std::uint8_t>>& payloads) {
    if (packets.size() != payloads.size()) return false;
    std::vector<bool> seen(payloads.size(), false);
    for (const auto& packet : packets) {
        if (packet.data.size() < sizeof(std::uint32_t)) return false;
        if (packet.from_length == 0 ||
            packet.from.ss_family != static_cast<sa_family_t>(AF_INET)) {
            return false;
        }
        std::uint32_t sequence = 0;
        std::memcpy(&sequence, packet.data.data(), sizeof(sequence));
        if (sequence >= payloads.size() || seen[sequence]) return false;
        seen[sequence] = true;
        if (packet.data != payloads[sequence]) return false;
    }
    for (bool value : seen) {
        if (!value) return false;
    }
    return true;
}

struct TcpPair {
    Socket::Ptr listener;
    Socket::Ptr client;
    Socket::Ptr server;
};

bool MakeTcpPair(TcpPair* pair) {
    pair->listener = Socket::CreateTCP(AF_INET);
    if (!pair->listener) return false;
    const auto bind_address = IPv4Address::Create("127.0.0.1", 0);
    if (!bind_address || !pair->listener->bind(*bind_address)) return false;
    if (!pair->listener->listen(4)) return false;
    const auto listen_address = pair->listener->local_address();
    if (!listen_address) return false;
    pair->client = Socket::CreateTCP(AF_INET);
    if (!pair->client || !pair->client->connect(*listen_address, kWaitTimeout)) return false;
    pair->server = pair->listener->accept(kWaitTimeout);
    if (!pair->server) return false;
    pair->client->set_send_timeout(Duration::max());
    pair->server->set_recv_timeout(kWaitTimeout);
    return true;
}

int CreateMemfdWithData(const std::vector<std::uint8_t>& data) {
    const int fd = ::memfd_create("go2cpp-net-batch", MFD_CLOEXEC);
    if (fd < 0) return -1;
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = ::write(fd, data.data() + written, data.size() - written);
        if (count <= 0) {
            ::close(fd);
            return -1;
        }
        written += static_cast<std::size_t>(count);
    }
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::vector<std::uint8_t> MakeRandomData(std::size_t size, std::uint32_t seed) {
    std::mt19937 generator(seed);
    std::vector<std::uint8_t> data(size);
    for (auto& byte : data) byte = static_cast<std::uint8_t>(generator() & 0xFFU);
    return data;
}

/// 从 socket 读满 size 字节；返回 false 表示 EOF/错误/超时。
bool ReceiveExact(Socket& socket, void* buffer, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    std::size_t received = 0;
    while (received < size) {
        const auto result = socket.recv(bytes + received, size - received, 0, kWaitTimeout);
        if (!result.ok() || result.transferred == 0) return false;
        received += static_cast<std::size_t>(result.transferred);
    }
    return true;
}

std::size_t CountOpenFds() {
    DIR* directory = ::opendir("/proc/self/fd");
    if (!directory) return 0;
    std::size_t count = 0;
    while (dirent* entry = ::readdir(directory)) {
        if (entry->d_name[0] != '.') ++count;
    }
    ::closedir(directory);
    return count;
}

// ---------------------------------------------------------------------------
// 1. 参数校验
// ---------------------------------------------------------------------------
void TestValidation() {
    std::printf("[test] validation\n");
    auto socket = Socket::CreateUDP(AF_INET);
    REQUIRE(socket != nullptr);
    std::uint8_t buffer[8]{};
    RecvBatchEntry entry{};
    entry.data = buffer;
    entry.capacity = sizeof(buffer);
    CHECK(socket->recv_batch(nullptr, 1).error == EINVAL);
    CHECK(socket->recv_batch(&entry, 0).error == EINVAL);
    CHECK(socket->recv_batch(&entry, Socket::kMaxBatch + 1).error == EINVAL);
    RecvBatchEntry missing_buffer{};
    CHECK(socket->recv_batch(&missing_buffer, 1).error == EINVAL);
    missing_buffer.data = buffer;
    CHECK(socket->recv_batch(&missing_buffer, 1).error == EINVAL);  // capacity 0
    SendBatchEntry send{};
    CHECK(socket->send_batch(nullptr, 1).error == EINVAL);
    CHECK(socket->send_batch(&send, 1).error == EINVAL);
    CHECK(socket->send_batch(&send, 0).error == EINVAL);
    CHECK(socket->send_file(-1, nullptr, 16).error == EBADF);
    CHECK(socket->send_file(0, nullptr, 0).ok());
    // 未 bind 的 UDP socket 上批量收包：recvmmsg 会 EAGAIN 然后等待，这里先关闭。
    CHECK(socket->close());
}

// ---------------------------------------------------------------------------
// 2. UDP 批量收发（连接 / 非连接）
// ---------------------------------------------------------------------------
void TestUdpBatchRoundTrip() {
    std::printf("[test] udp batch round trip\n");
    for (bool connected : {false, true}) {
        UdpPair pair;
        REQUIRE(MakeUdpPair(&pair, connected));
        constexpr std::size_t kCount = 32;
        constexpr std::size_t kSize = 64;
        const auto payloads = MakePayloads(kCount, kSize);
        const auto destination = connected ? nullptr : pair.receiver_address;
        REQUIRE(SendPayloads(*pair.sender, destination, payloads, kCount));
        std::vector<ReceivedPacket> packets;
        REQUIRE(ReceivePayloads(*pair.receiver, kCount, kCount, &packets));
        CHECK(packets.size() == kCount);
        CHECK(VerifyPackets(packets, payloads));
        CHECK(pair.sender->close());
        CHECK(pair.receiver->close());
    }
}

// ---------------------------------------------------------------------------
// 3. 部分完成与超时
// ---------------------------------------------------------------------------
void TestPartialAndTimeout() {
    std::printf("[test] partial receive and timeout\n");
    UdpPair pair;
    REQUIRE(MakeUdpPair(&pair, false, kShortTimeout));
    const auto payloads = MakePayloads(3, 64);
    REQUIRE(SendPayloads(*pair.sender, pair.receiver_address, payloads, 3));

    std::vector<RecvBatchEntry> entries(8);
    std::vector<std::vector<std::uint8_t>> buffers(8, std::vector<std::uint8_t>(128));
    for (std::size_t index = 0; index < entries.size(); ++index) {
        entries[index].data = buffers[index].data();
        entries[index].capacity = buffers[index].size();
    }
    // 只到达 3 个包，但要求 8 个槽位：一次 recvmmsg 收走已有的 3 个。
    auto result = pair.receiver->recv_batch(entries.data(), entries.size(), 0, kWaitTimeout);
    CHECK(result.ok() && result.transferred == 3);
    CHECK(entries[0].received == 64);
    CHECK(entries[0].from_length > 0);

    // 队列为空：等待 50ms 后超时，且不返回 0。
    const auto started = Clock::now();
    result = pair.receiver->recv_batch(entries.data(), entries.size(), 0, kShortTimeout);
    const auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - started).count();
    CHECK(!result.ok() && result.error == ETIMEDOUT);
    CHECK(elapsed >= 40 && elapsed < 2000);
}

// ---------------------------------------------------------------------------
// 4. 跨线程取消
// ---------------------------------------------------------------------------
void TestCancel() {
    std::printf("[test] cancel read while recv_batch waits\n");
    UdpPair pair;
    REQUIRE(MakeUdpPair(&pair, false, Duration::max()));
    std::atomic<bool> done{false};
    std::atomic<int> error{0};
    std::atomic<long long> elapsed_ms{0};
    std::thread waiter([&] {
        std::vector<std::vector<std::uint8_t>> buffers(4, std::vector<std::uint8_t>(64));
        std::vector<RecvBatchEntry> entries(4);
        for (std::size_t index = 0; index < entries.size(); ++index) {
            entries[index].data = buffers[index].data();
            entries[index].capacity = buffers[index].size();
        }
        const auto started = Clock::now();
        const auto result =
            pair.receiver->recv_batch(entries.data(), entries.size(), 0, std::chrono::seconds(5));
        error.store(result.error, std::memory_order_release);
        elapsed_ms.store(
            std::chrono::duration_cast<Duration>(Clock::now() - started).count(),
            std::memory_order_release);
        done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(pair.receiver->cancel_read());
    for (int round = 0; round < 300 && !done.load(std::memory_order_acquire); ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done.load(std::memory_order_acquire)) {
        pair.receiver->close();
    }
    waiter.join();
    CHECK(done.load(std::memory_order_acquire));
    CHECK(error.load(std::memory_order_acquire) == ECANCELED);
    CHECK(elapsed_ms.load(std::memory_order_acquire) < 2000);

    // 取消只影响在途调用：下一轮 recv_batch 必须恢复正常。
    const auto payloads = MakePayloads(1, 32);
    REQUIRE(SendPayloads(*pair.sender, pair.receiver_address, payloads, 1));
    std::vector<ReceivedPacket> packets;
    REQUIRE(ReceivePayloads(*pair.receiver, 1, 1, &packets));
    CHECK(VerifyPackets(packets, payloads));
}

// ---------------------------------------------------------------------------
// 5. send_batch 错误路径
// ---------------------------------------------------------------------------
void TestSendBatchErrors() {
    std::printf("[test] send_batch error paths\n");
    UdpPair pair;
    REQUIRE(MakeUdpPair(&pair, true));
    std::vector<std::uint8_t> huge(70000, 0x7F);
    SendBatchEntry oversized{};
    oversized.data = huge.data();
    oversized.length = huge.size();
    auto result = pair.sender->send_batch(&oversized, 1, 0, kWaitTimeout);
    CHECK(!result.ok() && result.error == EMSGSIZE);

    // 第一条合法、第二条超大：sendmmsg 逐条提交，返回已提交条数（1）或整体
    // 失败（不同内核版本可能先校验后发送）。两种结果都允许，但绝不能崩溃。
    const std::uint8_t small[64]{};
    SendBatchEntry mixed[2];
    mixed[0].data = small;
    mixed[0].length = sizeof(small);
    mixed[1].data = huge.data();
    mixed[1].length = huge.size();
    result = pair.sender->send_batch(mixed, 2, 0, kWaitTimeout);
    CHECK((result.transferred == 1 && result.ok()) ||
          (result.transferred == -1 && result.error == EMSGSIZE));

    // 关闭后调用：返回取消/坏 fd，而不是崩溃。
    CHECK(pair.sender->close());
    result = pair.sender->send_batch(mixed, 1, 0, kWaitTimeout);
    CHECK(result.transferred == -1 && (result.error == ECANCELED || result.error == EBADF));
}

// ---------------------------------------------------------------------------
// 6. sendfile：基本、offset、EOF、慢读、对端关闭
// ---------------------------------------------------------------------------
void TestSendFileBasic() {
    std::printf("[test] sendfile basic\n");
    const auto data = MakeRandomData(1U << 20, 0x12345678U);
    const int file_fd = CreateMemfdWithData(data);
    REQUIRE(file_fd >= 0);
    TcpPair pair;
    REQUIRE(MakeTcpPair(&pair));
    std::atomic<bool> server_ok{false};
    std::vector<std::uint8_t> received(data.size());
    std::thread server([&] {
        server_ok.store(ReceiveExact(*pair.server, received.data(), received.size()),
                        std::memory_order_release);
    });
    const auto result = pair.client->send_file(file_fd, nullptr, data.size());
    CHECK(result.ok() && result.transferred == static_cast<ssize_t>(data.size()));
    if (server.joinable()) server.join();
    CHECK(server_ok.load(std::memory_order_acquire));
    CHECK(received == data);
    ::close(file_fd);
    CHECK(pair.client->close());
    CHECK(pair.server->close());
    CHECK(pair.listener->close());
}

void TestSendFileOffsetAndEof() {
    std::printf("[test] sendfile offset and eof\n");
    const auto data = MakeRandomData(64U * 1024U, 0xABCDEF01U);
    const int file_fd = CreateMemfdWithData(data);
    REQUIRE(file_fd >= 0);
    TcpPair pair;
    REQUIRE(MakeTcpPair(&pair));
    std::atomic<bool> server_ok{false};
    std::vector<std::uint8_t> received(data.size());
    std::thread server([&] {
        server_ok.store(ReceiveExact(*pair.server, received.data(), received.size()),
                        std::memory_order_release);
    });
    off_t offset = 0;
    auto result = pair.client->send_file(file_fd, &offset, 4096);
    CHECK(result.ok() && result.transferred == 4096);
    CHECK(offset == 4096);
    result = pair.client->send_file(file_fd, &offset, data.size());
    CHECK(result.ok() && result.transferred == static_cast<ssize_t>(data.size() - 4096));
    CHECK(offset == static_cast<off_t>(data.size()));
    // 已到 EOF：返回 0，不是错误。
    result = pair.client->send_file(file_fd, &offset, 4096);
    CHECK(result.ok() && result.transferred == 0);
    if (server.joinable()) server.join();
    CHECK(server_ok.load(std::memory_order_acquire));
    CHECK(received == data);
    ::close(file_fd);
    pair.client->close();
    pair.server->close();
    pair.listener->close();
}

void TestSendFileSlowReader() {
    std::printf("[test] sendfile slow reader (EAGAIN path)\n");
    constexpr std::size_t kSize = 8U << 20;
    const auto data = MakeRandomData(kSize, 0x0BADF00DU);
    const int file_fd = CreateMemfdWithData(data);
    REQUIRE(file_fd >= 0);
    TcpPair pair;
    REQUIRE(MakeTcpPair(&pair));
    std::atomic<bool> server_ok{false};
    std::vector<std::uint8_t> received(kSize);
    std::thread server([&] {
        std::size_t offset = 0;
        while (offset < kSize) {
            const std::size_t chunk = std::min<std::size_t>(64U * 1024U, kSize - offset);
            const auto result = pair.server->recv(received.data() + offset, chunk, 0, kWaitTimeout);
            if (!result.ok() || result.transferred <= 0) {
                server_ok.store(false, std::memory_order_release);
                return;
            }
            offset += static_cast<std::size_t>(result.transferred);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        server_ok.store(true, std::memory_order_release);
    });
    const auto result = pair.client->send_file(file_fd, nullptr, kSize, std::chrono::seconds(30));
    CHECK(result.ok() && result.transferred == static_cast<ssize_t>(kSize));
    if (server.joinable()) server.join();
    CHECK(server_ok.load(std::memory_order_acquire));
    CHECK(received == data);
    ::close(file_fd);
    pair.client->close();
    pair.server->close();
    pair.listener->close();
}

void TestSendFilePeerClosed() {
    std::printf("[test] sendfile peer closed\n");
    const auto data = MakeRandomData(1U << 20, 0xFEEDFACEU);
    const int file_fd = CreateMemfdWithData(data);
    REQUIRE(file_fd >= 0);
    TcpPair pair;
    REQUIRE(MakeTcpPair(&pair));
    pair.client->set_send_timeout(std::chrono::milliseconds(500));
    // 对端先关闭：sendfile 最终返回 EPIPE/ECONNRESET；库内 SIGPIPE 屏蔽必须
    // 保证进程存活（本测试能跑到下一行就是证据）。
    CHECK(pair.server->close());
    std::size_t total = 0;
    SocketResult result{-1, 0};
    for (int round = 0; round < 16; ++round) {
        result = pair.client->send_file(file_fd, nullptr, data.size());
        if (!result.ok()) break;
        total += static_cast<std::size_t>(result.transferred);
        if (total >= data.size()) break;
    }
    if (!result.ok()) {
        CHECK(result.error == EPIPE || result.error == ECONNRESET ||
              result.error == ETIMEDOUT || result.error == ESHUTDOWN);
    }
    CHECK(total <= data.size());
    ::close(file_fd);
    pair.client->close();
    pair.listener->close();
}

// ---------------------------------------------------------------------------
// 7. Fiber 内批量收发
// ---------------------------------------------------------------------------
void TestFiberBatch() {
    std::printf("[test] fiber recv_batch/send_batch\n");
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    go2cpp::IOManager manager(config);
    REQUIRE(manager.Start());
    UdpPair pair;
    REQUIRE(MakeUdpPair(&pair, false, Duration::max()));
    constexpr std::size_t kCount = 16;
    const auto payloads = MakePayloads(kCount, 96);
    std::atomic<bool> received_done{false};
    std::atomic<int> received_count{-1};
    std::atomic<int> received_error{0};
    auto fiber = manager.Go([&] {
        std::vector<std::vector<std::uint8_t>> buffers(kCount,
                                                       std::vector<std::uint8_t>(256));
        std::vector<RecvBatchEntry> entries(kCount);
        for (std::size_t index = 0; index < kCount; ++index) {
            entries[index].data = buffers[index].data();
            entries[index].capacity = buffers[index].size();
        }
        const auto started = Clock::now();
        std::size_t total = 0;
        int error = 0;
        while (total < kCount) {
            const auto result = pair.receiver->recv_batch(
                entries.data(), kCount - total, 0, std::chrono::seconds(5));
            if (!result.ok() || result.transferred <= 0) {
                error = result.ok() ? EIO : result.error;
                break;
            }
            total += static_cast<std::size_t>(result.transferred);
            if (Clock::now() - started > std::chrono::seconds(10)) {
                error = ETIMEDOUT;
                break;
            }
        }
        received_error.store(error, std::memory_order_release);
        received_count.store(error == 0 ? static_cast<int>(total) : -1,
                             std::memory_order_release);
        received_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(SendPayloads(*pair.sender, pair.receiver_address, payloads, kCount));
    for (int round = 0; round < 300 && !received_done.load(std::memory_order_acquire); ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(received_done.load(std::memory_order_acquire));
    if (received_count.load(std::memory_order_acquire) != static_cast<int>(kCount)) {
        std::fprintf(stderr, "fiber recv_batch count=%d error=%d\n",
                     received_count.load(std::memory_order_acquire),
                     received_error.load(std::memory_order_acquire));
    }
    CHECK(received_count.load(std::memory_order_acquire) == static_cast<int>(kCount));
    CHECK(fiber->wait_for(std::chrono::seconds(2)));
    manager.Shutdown();
}

// ---------------------------------------------------------------------------
// 8. 随机测试：300 轮随机批大小 / 包长 / 发送顺序
// ---------------------------------------------------------------------------
void TestRandomized() {
    std::printf("[test] randomized 300 rounds\n");
    std::mt19937 generator(0xC0FFEEU);
    for (int round = 0; round < 300; ++round) {
        const bool connected = (generator() & 1U) != 0;
        UdpPair pair;
        REQUIRE(MakeUdpPair(&pair, connected, kShortTimeout));
        const std::size_t count = 1 + (generator() % 32);
        const std::size_t size = sizeof(std::uint32_t) + (generator() % 1397);
        const std::size_t send_chunk = 1 + (generator() % 32);
        const std::size_t recv_chunk = 1 + (generator() % 32);
        // expected 保持序号顺序（VerifyPackets 用前 4 字节的序号索引）；
        // 只打乱发送顺序，验证批量收发不依赖数据报顺序。
        const auto expected = MakePayloads(count, size);
        auto send_order = expected;
        std::shuffle(send_order.begin(), send_order.end(), generator);
        const auto destination = connected ? nullptr : pair.receiver_address;
        CHECK(SendPayloads(*pair.sender, destination, send_order, send_chunk));
        std::vector<ReceivedPacket> packets;
        CHECK(ReceivePayloads(*pair.receiver, count, recv_chunk, &packets));
        CHECK(VerifyPackets(packets, expected));
        pair.sender->close();
        pair.receiver->close();
    }
}

// ---------------------------------------------------------------------------
// 5 个真实场景
// ---------------------------------------------------------------------------

/// 场景 1：游戏服务器每 tick 向 4 个订阅端各广播 16 条玩家状态（64 包/tick）。
void ScenarioGameBroadcast() {
    std::printf("[scenario] game broadcast 64 datagrams x 500 ticks\n");
    constexpr int kReceivers = 4;
    constexpr int kPerReceiver = 16;
    constexpr int kTicks = 500;
    std::vector<Socket::Ptr> receivers(kReceivers);
    std::vector<go2cpp::net::AddressPtr> addresses(kReceivers);
    for (int index = 0; index < kReceivers; ++index) {
        receivers[index] = Socket::CreateUDP(AF_INET);
        REQUIRE(receivers[index] != nullptr);
        const auto bind_address = IPv4Address::Create("127.0.0.1", 0);
        REQUIRE(bind_address && receivers[index]->bind(*bind_address));
        receivers[index]->set_recv_timeout(kWaitTimeout);
        addresses[index] = receivers[index]->local_address();
        REQUIRE(addresses[index] != nullptr);
    }
    auto sender = Socket::CreateUDP(AF_INET);
    REQUIRE(sender != nullptr);
    std::uint32_t sequence = 0;
    for (int tick = 0; tick < kTicks; ++tick) {
        const auto payloads = MakePayloads(kReceivers * kPerReceiver, 120, sequence);
        sequence += kReceivers * kPerReceiver;
        std::vector<SendBatchEntry> entries(payloads.size());
        for (int index = 0; index < kReceivers * kPerReceiver; ++index) {
            const int receiver_index = index / kPerReceiver;
            entries[static_cast<std::size_t>(index)].data =
                payloads[static_cast<std::size_t>(index)].data();
            entries[static_cast<std::size_t>(index)].length =
                payloads[static_cast<std::size_t>(index)].size();
            entries[static_cast<std::size_t>(index)].to = addresses[receiver_index]->addr();
            entries[static_cast<std::size_t>(index)].to_length =
                addresses[receiver_index]->addr_len();
        }
        const auto sent = sender->send_batch(entries.data(), entries.size(), 0, kWaitTimeout);
        CHECK(sent.ok() && sent.transferred == static_cast<ssize_t>(entries.size()));
        for (int index = 0; index < kReceivers; ++index) {
            std::vector<ReceivedPacket> packets;
            CHECK(ReceivePayloads(*receivers[index], kPerReceiver, kPerReceiver, &packets));
            // 每个接收端拿到本 tick 的 16 条；序号范围可校验，内容按序号重排。
            std::vector<std::vector<std::uint8_t>> expected(kPerReceiver);
            for (int offset = 0; offset < kPerReceiver; ++offset) {
                const std::size_t source =
                    static_cast<std::size_t>(index * kPerReceiver + offset);
                expected[static_cast<std::size_t>(offset)] =
                    payloads[source];
            }
            // expected 的序号不是 0..15，VerifyPackets 会失败；改为直接比对集合。
            if (packets.size() != expected.size()) {
                CHECK(false);
                continue;
            }
            std::vector<bool> used(expected.size(), false);
            bool ok = true;
            for (const auto& packet : packets) {
                bool matched = false;
                for (std::size_t candidate = 0; candidate < expected.size(); ++candidate) {
                    if (!used[candidate] && packet.data == expected[candidate]) {
                        used[candidate] = true;
                        matched = true;
                        break;
                    }
                }
                if (!matched) ok = false;
            }
            CHECK(ok);
        }
    }
    CHECK(sender->close());
    for (auto& receiver : receivers) CHECK(receiver->close());
}

/// 场景 2：8 MiB 日志文件零拷贝传输（sendfile），接收端完整校验。
void ScenarioFileTransfer() {
    std::printf("[scenario] 8 MiB file transfer via sendfile\n");
    constexpr std::size_t kSize = 8U << 20;
    const auto data = MakeRandomData(kSize, 0x5EED1234U);
    const int file_fd = CreateMemfdWithData(data);
    REQUIRE(file_fd >= 0);
    TcpPair pair;
    REQUIRE(MakeTcpPair(&pair));
    std::atomic<bool> server_ok{false};
    std::vector<std::uint8_t> received(kSize);
    std::thread server([&] {
        server_ok.store(ReceiveExact(*pair.server, received.data(), received.size()),
                        std::memory_order_release);
    });
    const auto result = pair.client->send_file(file_fd, nullptr, kSize);
    CHECK(result.ok() && result.transferred == static_cast<ssize_t>(kSize));
    if (server.joinable()) server.join();
    CHECK(server_ok.load(std::memory_order_acquire));
    CHECK(received == data);
    ::close(file_fd);
    pair.client->close();
    pair.server->close();
    pair.listener->close();
}

/// 场景 3：遥测突发 10,000 包，每批 64 条。
void ScenarioTelemetryBurst() {
    std::printf("[scenario] telemetry burst 10000 datagrams, batch=64\n");
    constexpr std::size_t kTotal = 10000;
    constexpr std::size_t kBatch = 64;
    UdpPair pair;
    REQUIRE(MakeUdpPair(&pair, false));
    const auto payloads = MakePayloads(kTotal, 80);
    // 真实遥测是「边到边处理」：每批发送后立刻收走，避免内核接收队列
    // 因 10k 包全部堆积而溢出（默认 rmem 只有 212 KiB）。
    std::vector<bool> seen(kTotal, false);
    std::size_t sent_total = 0;
    while (sent_total < kTotal) {
        const std::size_t chunk = std::min(kBatch, kTotal - sent_total);
        const std::vector<std::vector<std::uint8_t>> batch(
            payloads.begin() + static_cast<std::ptrdiff_t>(sent_total),
            payloads.begin() + static_cast<std::ptrdiff_t>(sent_total + chunk));
        REQUIRE(SendPayloads(*pair.sender, pair.receiver_address, batch, chunk));
        std::vector<ReceivedPacket> packets;
        REQUIRE(ReceivePayloads(*pair.receiver, chunk, chunk, &packets));
        for (const auto& packet : packets) {
            REQUIRE(packet.data.size() >= sizeof(std::uint32_t));
            std::uint32_t sequence = 0;
            std::memcpy(&sequence, packet.data.data(), sizeof(sequence));
            REQUIRE(sequence < kTotal);
            CHECK(!seen[sequence]);
            CHECK(packet.data == payloads[sequence]);
            seen[sequence] = true;
        }
        sent_total += chunk;
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
    pair.sender->close();
    pair.receiver->close();
}

/// 场景 4：无数据超时 + 跨线程取消 + 取消后恢复。
void ScenarioTimeoutAndCancel() {
    std::printf("[scenario] timeout and cancellation\n");
    TestPartialAndTimeout();
    TestCancel();
}

/// 场景 5：异常路径（参数、超大包、对端关闭、fd 泄漏）。
void ScenarioFailurePaths() {
    std::printf("[scenario] failure paths and fd hygiene\n");
    const std::size_t before = CountOpenFds();
    TestValidation();
    TestSendBatchErrors();
    TestSendFilePeerClosed();
    const std::size_t after = CountOpenFds();
    CHECK(after <= before + 2);
}

}  // namespace

int main() {
    // sendfile 对端关闭可能投递 SIGPIPE；库内部已做线程级屏蔽，这里再兜底
    // 忽略，保证任何遗漏都不会让测试进程直接被杀。
    ::signal(SIGPIPE, SIG_IGN);
    const auto started = Clock::now();
    TestValidation();
    TestUdpBatchRoundTrip();
    TestPartialAndTimeout();
    TestCancel();
    TestSendBatchErrors();
    TestSendFileBasic();
    TestSendFileOffsetAndEof();
    TestSendFileSlowReader();
    TestSendFilePeerClosed();
    TestFiberBatch();
    TestRandomized();
    ScenarioGameBroadcast();
    ScenarioFileTransfer();
    ScenarioTelemetryBurst();
    ScenarioTimeoutAndCancel();
    ScenarioFailurePaths();
    const auto elapsed =
        std::chrono::duration_cast<Duration>(Clock::now() - started).count();
    std::printf("\nchecks=%d failures=%d elapsed=%lldms\n", g_checks, g_failures,
                static_cast<long long>(elapsed));
    return g_failures == 0 ? 0 : 1;
}
