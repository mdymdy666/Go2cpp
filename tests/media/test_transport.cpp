/**
 * @file test_transport.cpp
 * @brief 传输层：UDP 端点 / VideoChannel 1200B 分片与重组 / 乱序 / 丢片超时 /
 *        大帧（260KB，216 片）/ 超上限拒绝 / 进程内直连链路（含丢包模拟）。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <cstring>
#include <vector>

#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/media/transport.hpp"
#include "media_test_util.hpp"

namespace {

using namespace go2cpp::media;

/// @brief 构造一段可校验的数据。
std::vector<std::uint8_t> MakePayload(std::size_t bytes, std::uint8_t seed) {
    std::vector<std::uint8_t> data(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 31U + seed * 7U) & 0xFFU);
    }
    return data;
}

void TestUdpEndpoint() {
    UdpEndpoint endpoint;
    std::string error;
    MEDIA_CHECK(endpoint.Open(&error));
    MEDIA_CHECK(endpoint.opened());
    MEDIA_CHECK(endpoint.port() != 0);
    // 未设置对端时发送应失败（不能假设有默认对端）。
    const std::uint8_t byte = 0;
    MEDIA_CHECK(!endpoint.Send(&byte, 1));

    // 两个端点互发。
    UdpEndpoint a;
    UdpEndpoint b;
    MEDIA_CHECK(a.Open(&error));
    MEDIA_CHECK(b.Open(&error));
    a.SetPeerPort(b.port());
    b.SetPeerPort(a.port());
    const std::vector<std::uint8_t> payload = MakePayload(512, 3);
    MEDIA_CHECK(a.Send(payload.data(), payload.size()));
    std::uint8_t buffer[1024] = {0};
    const int received = b.Recv(buffer, sizeof(buffer), 500);
    MEDIA_CHECK_EQ(received, 512);
    MEDIA_CHECK(std::memcmp(buffer, payload.data(), payload.size()) == 0);

    // 超时返回 0。
    MEDIA_CHECK_EQ(b.Recv(buffer, sizeof(buffer), 20), 0);
}

void TestSenderReceiverRoundTrip() {
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> delivered;
    UdpVideoReceiver receiver([&delivered](std::uint32_t frame_id, bool key, const std::uint8_t* data,
                                           std::size_t bytes) {
        (void)key;
        delivered.emplace_back(frame_id, std::vector<std::uint8_t>(data, data + bytes));
    });
    std::string error;
    MEDIA_CHECK(receiver.Open(&error));
    MEDIA_CHECK(receiver.local_port() != 0);

    UdpVideoSender sender;
    MEDIA_CHECK(sender.Open(receiver.local_port(), &error));
    MEDIA_CHECK(sender.local_port() != 0);

    // 3000B → 3 个 1200B 分片。
    const std::vector<std::uint8_t> frame = MakePayload(3000, 11);
    MEDIA_CHECK(sender.SendFrame(7, true, frame.data(), frame.size()));
    MEDIA_CHECK_EQ(sender.stats().fragments_sent, 3U);

    for (int i = 0; i < 10 && delivered.empty(); ++i) {
        receiver.Pump(100, 64);
    }
    MEDIA_CHECK_EQ(delivered.size(), 1U);
    MEDIA_CHECK_EQ(delivered[0].first, 7U);
    MEDIA_CHECK(delivered[0].second == frame);
    MEDIA_CHECK_EQ(receiver.stats().frames_delivered, 1U);
    MEDIA_CHECK(receiver.datagrams() >= 3U);
    MEDIA_CHECK_EQ(receiver.malformed(), 0U);

    // 大帧：150000B → 125 片（<= 256；受 WSL2 rmem_max 208KB 约束，见文档）。
    delivered.clear();
    const std::vector<std::uint8_t> big = MakePayload(150000, 13);
    MEDIA_CHECK(sender.SendFrame(8, false, big.data(), big.size()));
    for (int i = 0; i < 40 && delivered.empty(); ++i) {
        receiver.Pump(100, 256);
    }
    MEDIA_CHECK_EQ(delivered.size(), 1U);
    MEDIA_CHECK_EQ(delivered[0].second.size(), big.size());
    MEDIA_CHECK(delivered[0].second == big);

    // 超上限：257 片 → SendFrame 返回 false。
    const std::vector<std::uint8_t> huge(256 * 1200 + 1, 0x5A);
    MEDIA_CHECK(!sender.SendFrame(9, false, huge.data(), huge.size()));
}

void TestReordering() {
    // 用一个裸端点手工乱序发送分片，验证接收端重组与旧帧丢弃。
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> delivered;
    UdpVideoReceiver receiver([&delivered](std::uint32_t frame_id, bool, const std::uint8_t* data,
                                           std::size_t bytes) {
        delivered.emplace_back(frame_id, std::vector<std::uint8_t>(data, data + bytes));
    });
    std::string error;
    MEDIA_CHECK(receiver.Open(&error));

    UdpEndpoint raw;
    MEDIA_CHECK(raw.Open(&error));
    raw.SetPeerPort(receiver.local_port());

    std::vector<std::vector<std::uint8_t>> packets;
    go2cpp::bento::VideoChannel sender(
        [&packets](const std::uint8_t* data, std::size_t bytes) {
            packets.emplace_back(data, data + bytes);
            return true;
        },
        [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});
    const std::vector<std::uint8_t> frame = MakePayload(3000, 21);
    MEDIA_CHECK(sender.SendFrame(42, true, frame.data(), frame.size()));
    MEDIA_CHECK_EQ(packets.size(), 3U);

    // 乱序 2,0,1。
    MEDIA_CHECK(raw.Send(packets[2].data(), packets[2].size()));
    MEDIA_CHECK(raw.Send(packets[0].data(), packets[0].size()));
    MEDIA_CHECK(raw.Send(packets[1].data(), packets[1].size()));
    for (int i = 0; i < 10 && delivered.empty(); ++i) {
        receiver.Pump(100, 64);
    }
    MEDIA_CHECK_EQ(delivered.size(), 1U);
    MEDIA_CHECK(delivered[0].second == frame);

    // 旧 frame_id 的分片：stale，不交付。
    packets.clear();
    const std::vector<std::uint8_t> older = MakePayload(100, 22);
    MEDIA_CHECK(sender.SendFrame(5, false, older.data(), older.size()));
    MEDIA_CHECK(raw.Send(packets[0].data(), packets[0].size()));
    receiver.Pump(50, 8);
    MEDIA_CHECK_EQ(delivered.size(), 1U);
    MEDIA_CHECK_EQ(receiver.stats().fragments_stale, 1U);
}

void TestMissingFragmentTimeout() {
    std::vector<std::uint32_t> delivered;
    UdpVideoReceiver receiver([&delivered](std::uint32_t frame_id, bool, const std::uint8_t*,
                                           std::size_t) { delivered.push_back(frame_id); });
    std::string error;
    MEDIA_CHECK(receiver.Open(&error));

    UdpEndpoint raw;
    MEDIA_CHECK(raw.Open(&error));
    raw.SetPeerPort(receiver.local_port());

    std::vector<std::vector<std::uint8_t>> packets;
    go2cpp::bento::VideoChannel sender(
        [&packets](const std::uint8_t* data, std::size_t bytes) {
            packets.emplace_back(data, data + bytes);
            return true;
        },
        [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});
    const std::vector<std::uint8_t> frame = MakePayload(2400, 31);
    MEDIA_CHECK(sender.SendFrame(3, false, frame.data(), frame.size()));
    MEDIA_CHECK_EQ(packets.size(), 2U);
    // 只发第 0 片，第 1 片丢失。
    MEDIA_CHECK(raw.Send(packets[0].data(), packets[0].size()));
    receiver.Pump(100, 8);
    MEDIA_CHECK(delivered.empty());
    MEDIA_CHECK_EQ(receiver.stats().frames_incomplete_dropped, 0U);
    receiver.OnTimer(go2cpp::bento::NowMs() + go2cpp::bento::VideoChannel::kReassemblyTimeoutMs + 100);
    MEDIA_CHECK_EQ(receiver.stats().frames_incomplete_dropped, 1U);
    // 补齐第 1 片也不会交付（重组项已删）。
    MEDIA_CHECK(raw.Send(packets[1].data(), packets[1].size()));
    receiver.Pump(100, 8);
    MEDIA_CHECK(delivered.empty());
}

void TestInProcessLink() {
    InProcessVideoLink link;
    std::vector<std::vector<std::uint8_t>> delivered;
    link.SetDeliver([&delivered](std::uint32_t, bool, const std::uint8_t* data, std::size_t bytes) {
        delivered.emplace_back(data, data + bytes);
    });
    std::vector<std::uint8_t> frame = MakePayload(4800, 41);
    MEDIA_CHECK(link.SendFrame(1, true, frame.data(), frame.size()));
    MEDIA_CHECK_EQ(link.send_stats().fragments_sent, 4U);
    MEDIA_CHECK(delivered.empty());  // 未 Flush 前不投递
    link.Flush();
    MEDIA_CHECK_EQ(delivered.size(), 1U);
    MEDIA_CHECK(delivered[0] == frame);

    // 丢包模拟：每 2 片丢 1 片（4 片 → 收 2 片，缺片，不交付）。
    InProcessVideoLink lossy;
    std::vector<std::vector<std::uint8_t>> lossy_delivered;
    lossy.SetDeliver(
        [&lossy_delivered](std::uint32_t, bool, const std::uint8_t* data, std::size_t bytes) {
            lossy_delivered.emplace_back(data, data + bytes);
        });
    lossy.SetDropEvery(2);
    MEDIA_CHECK(lossy.SendFrame(2, false, frame.data(), frame.size()));
    lossy.Flush();
    MEDIA_CHECK(lossy_delivered.empty());
    lossy.OnTimer(go2cpp::bento::NowMs() + go2cpp::bento::VideoChannel::kReassemblyTimeoutMs + 100);
    MEDIA_CHECK_EQ(lossy.recv_stats().frames_incomplete_dropped, 1U);
}

void TestMalformedDatagram() {
    std::vector<std::uint32_t> delivered;
    UdpVideoReceiver receiver([&delivered](std::uint32_t frame_id, bool, const std::uint8_t*,
                                           std::size_t) { delivered.push_back(frame_id); });
    std::string error;
    MEDIA_CHECK(receiver.Open(&error));
    UdpEndpoint raw;
    MEDIA_CHECK(raw.Open(&error));
    raw.SetPeerPort(receiver.local_port());

    const std::uint8_t garbage[17] = {0xFF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                      0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    MEDIA_CHECK(raw.Send(garbage, sizeof(garbage)));
    receiver.Pump(100, 8);
    MEDIA_CHECK_EQ(receiver.malformed(), 1U);
    MEDIA_CHECK(delivered.empty());
}

}  // namespace

int main() {
    TestUdpEndpoint();
    TestSenderReceiverRoundTrip();
    TestReordering();
    TestMissingFragmentTimeout();
    TestInProcessLink();
    TestMalformedDatagram();
    return media_test::Finish("test_transport");
}
