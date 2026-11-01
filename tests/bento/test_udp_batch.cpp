/**
 * @file test_udp_batch.cpp
 * @brief 批量/多队列 UDP 单测：recvmmsg/sendmmsg 批量收发、SO_REUSEPORT 多队列、
 *        批量上限、截断与超时语义。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "go2cpp/bento/net/udp_batch.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::net::Endpoint;
using go2cpp::bento::net::UdpBatchSocket;

std::vector<std::uint8_t> MakePacket(std::size_t bytes, std::uint8_t tag) {
    std::vector<std::uint8_t> data(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 37U + tag) & 0xFFU);
    }
    return data;
}

void TestSingleQueueBatch() {
    UdpBatchSocket socket;
    std::string error;
    UdpBatchSocket::Config config;
    config.queues = 1;
    config.recv_batch = 32;
    config.send_batch = 16;
    BENTO_CHECK(socket.Open(Endpoint{"127.0.0.1", 0}, config, &error));
    BENTO_CHECK(socket.IsOpen());
    BENTO_CHECK_EQ(socket.queue_count(), 1U);
    BENTO_CHECK(socket.local_port() != 0);

    const Endpoint peer{"127.0.0.1", socket.local_port()};
    std::vector<std::vector<std::uint8_t>> packets;
    for (std::uint8_t i = 0; i < 5; ++i) {
        packets.push_back(MakePacket(32 + i * 17, i));
    }
    std::size_t sent = 0;
    BENTO_CHECK(socket.SendBatch(peer, packets, &sent, &error));
    BENTO_CHECK_EQ(sent, 5U);
    BENTO_CHECK_EQ(socket.stats().tx_batches, 1U);
    BENTO_CHECK_EQ(socket.stats().tx_datagrams, 5U);

    std::vector<UdpBatchSocket::Datagram> received;
    // 回环数据报可能在 poll 前尚未入队，允许重试几轮。
    for (int attempt = 0; attempt < 50 && received.size() < packets.size(); ++attempt) {
        socket.RecvBatch(&received, 20, &error);
    }
    BENTO_CHECK_EQ(received.size(), packets.size());
    // UDP 不保证数据报顺序：每条报文首字节是构造时的 tag（0..4），据此
    // 匹配回原始报文，验证内容与长度都没被截断或错位。
    for (const auto& datagram : received) {
        BENTO_CHECK(!datagram.data.empty());
        BENTO_CHECK_EQ(datagram.peer.port, socket.local_port());
        if (!datagram.data.empty() && datagram.data[0] < packets.size()) {
            BENTO_CHECK(datagram.data == packets[datagram.data[0]]);
        } else {
            BENTO_CHECK(false);
        }
    }
    BENTO_CHECK(socket.stats().rx_datagrams >= 5U);
    BENTO_CHECK(socket.stats().rx_batches >= 1U);

    // 非阻塞接收：没有新数据时立即返回 0。
    std::vector<UdpBatchSocket::Datagram> empty;
    BENTO_CHECK_EQ(socket.RecvBatch(&empty, 0, &error), 0U);
    socket.Close();
    BENTO_CHECK(!socket.IsOpen());
}

void TestMultiQueueReusePort() {
    UdpBatchSocket socket;
    std::string error;
    UdpBatchSocket::Config config;
    config.queues = 4;
    config.reuse_port = true;
    BENTO_CHECK(socket.Open(Endpoint{"127.0.0.1", 0}, config, &error));
    // 内核不支持 SO_REUSEPORT 时自动回退单队列，不能视为失败。
    BENTO_CHECK(socket.queue_count() >= 1U);
    BENTO_CHECK(socket.queue_count() <= 4U);
    if (socket.stats().reuse_port) {
        BENTO_CHECK_EQ(socket.queue_count(), 4U);
    }
    const Endpoint peer{"127.0.0.1", socket.local_port()};
    std::vector<std::vector<std::uint8_t>> packets;
    for (std::uint8_t i = 0; i < 40; ++i) {
        packets.push_back(MakePacket(48, i));
    }
    std::size_t sent = 0;
    BENTO_CHECK(socket.SendBatch(peer, packets, &sent, &error));
    BENTO_CHECK_EQ(sent, 40U);
    // 40 条 > send_batch 16 → 至少 3 次 sendmmsg。
    BENTO_CHECK(socket.stats().tx_batches >= 3U);

    std::vector<UdpBatchSocket::Datagram> received;
    for (int attempt = 0; attempt < 100 && received.size() < 40; ++attempt) {
        socket.RecvBatch(&received, 20, &error);
    }
    BENTO_CHECK_EQ(received.size(), 40U);
    std::printf("[INFO] queues=%zu reuse_port=%d tx_batches=%llu rx_batches=%llu\n",
                socket.queue_count(), static_cast<int>(socket.stats().reuse_port),
                static_cast<unsigned long long>(socket.stats().tx_batches),
                static_cast<unsigned long long>(socket.stats().rx_batches));
    socket.Close();
}

void TestQueueClampAndBatchClamp() {
    UdpBatchSocket socket;
    std::string error;
    UdpBatchSocket::Config config;
    config.queues = 100;      // 超过 kMaxQueues=64
    config.recv_batch = 100;  // 超过 kMaxRecvBatch=32
    config.send_batch = 100;  // 超过 kMaxSendBatch=16
    BENTO_CHECK(socket.Open(Endpoint{"127.0.0.1", 0}, config, &error));
    BENTO_CHECK(socket.queue_count() <= UdpBatchSocket::kMaxQueues);
    BENTO_CHECK_EQ(socket.config().recv_batch, UdpBatchSocket::kMaxRecvBatch);
    BENTO_CHECK_EQ(socket.config().send_batch, UdpBatchSocket::kMaxSendBatch);
    BENTO_CHECK(socket.queue_count() <= 64U);
    socket.Close();
}

void TestTruncation() {
    UdpBatchSocket socket;
    std::string error;
    UdpBatchSocket::Config config;
    config.queues = 1;
    config.slot_bytes = 64;  // 故意小于数据报
    BENTO_CHECK(socket.Open(Endpoint{"127.0.0.1", 0}, config, &error));
    const Endpoint peer{"127.0.0.1", socket.local_port()};
    std::vector<std::vector<std::uint8_t>> packets;
    packets.push_back(MakePacket(200, 7));
    std::size_t sent = 0;
    BENTO_CHECK(socket.SendBatch(peer, packets, &sent, &error));
    BENTO_CHECK_EQ(sent, 1U);
    std::vector<UdpBatchSocket::Datagram> received;
    for (int attempt = 0; attempt < 50 && received.empty(); ++attempt) {
        socket.RecvBatch(&received, 20, &error);
    }
    BENTO_CHECK_EQ(received.size(), 1U);
    if (!received.empty()) {
        BENTO_CHECK_EQ(received[0].data.size(), 64U);  // 截断到槽大小
    }
    socket.Close();
}

void TestInvalidUsage() {
    UdpBatchSocket socket;
    std::string error;
    std::vector<UdpBatchSocket::Datagram> received;
    BENTO_CHECK(!socket.SendBatch(Endpoint{"127.0.0.1", 1}, {}, nullptr, &error));
    BENTO_CHECK_EQ(socket.RecvBatch(&received, 0, &error), 0U);
    // 关闭后重复 Close 幂等；再次 Open 可以恢复。
    socket.Close();
    BENTO_CHECK(socket.Open(Endpoint{"127.0.0.1", 0}, UdpBatchSocket::Config{}, &error));
    BENTO_CHECK(socket.IsOpen());
    socket.Close();
}

}  // namespace

int main() {
    TestSingleQueueBatch();
    TestMultiQueueReusePort();
    TestQueueClampAndBatchClamp();
    TestTruncation();
    TestInvalidUsage();
    return bento_test::Finish("test_udp_batch");
}
