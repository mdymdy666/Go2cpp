/**
 * @file test_unreliable_dropstale.cpp
 * @brief 乱序/旧 seq 注入：0 采纳、0 乱序；并验证 dep_seq 因果与 BARRIER。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

std::vector<std::uint8_t> PayloadOf(std::uint32_t seq) {
    std::vector<std::uint8_t> payload(16, 0);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((seq * 13U + i) & 0xFFU);
    }
    return payload;
}

void InjectDatagram(UnreliableChannel& channel, std::uint32_t seq, std::uint32_t dep_seq,
                    const std::vector<std::uint8_t>& payload) {
    FrameHeader header;
    header.type = FrameType::kDatagram;
    header.channel = channel.channel_id();
    header.seq = seq;
    header.dep_seq = dep_seq;
    const std::vector<std::uint8_t> bytes =
        BuildFrame(header, nullptr, payload.data(), payload.size());
    BENTO_CHECK(!bytes.empty());
    FrameView view;
    BENTO_CHECK(ParseFrame(bytes.data(), bytes.size(), &view));
    channel.OnFrame(view.header, view.payload, view.payload_bytes);
}

void InjectStream(ReliableChannel& channel, std::uint32_t seq, std::uint32_t dep_seq,
                  const std::vector<std::uint8_t>& payload) {
    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = channel.channel_id();
    header.seq = seq;
    header.dep_seq = dep_seq;
    const std::vector<std::uint8_t> bytes =
        BuildFrame(header, nullptr, payload.data(), payload.size());
    BENTO_CHECK(!bytes.empty());
    FrameView view;
    BENTO_CHECK(ParseFrame(bytes.data(), bytes.size(), &view));
    channel.OnFrame(view.header, view.payload, view.payload_bytes,
                    view.has_piggyback_ack ? &view.ack : nullptr);
}

bool StrictlyIncreasing(const std::vector<std::uint32_t>& values) {
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (!SeqGreater(values[i], values[i - 1])) {
            return false;
        }
    }
    return true;
}

void TestFrameCodec() {
    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = 0;
    header.seq = 42;
    header.dep_seq = 7;
    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    AckState ack;
    ack.ack_seq = 9;
    ack.ack_bits = 0x5;
    const std::vector<std::uint8_t> frame =
        BuildFrame(header, &ack, payload.data(), payload.size());
    BENTO_CHECK_EQ(frame.size(), kFrameHeaderBytes + kAckBytes + payload.size());

    FrameView view;
    BENTO_CHECK(ParseFrame(frame.data(), frame.size(), &view));
    BENTO_CHECK_EQ(static_cast<int>(view.header.type), static_cast<int>(FrameType::kStream));
    BENTO_CHECK_EQ(view.header.seq, 42U);
    BENTO_CHECK_EQ(view.header.dep_seq, 7U);
    BENTO_CHECK(view.has_piggyback_ack);
    BENTO_CHECK_EQ(view.ack.ack_seq, 9U);
    BENTO_CHECK_EQ(view.ack.ack_bits, 0x5U);
    BENTO_CHECK_EQ(view.payload_bytes, payload.size());
    BENTO_CHECK(view.payload[4] == 5);

    // 截断、坏版本、未知类型都必须被拒绝。
    BENTO_CHECK(!ParseFrame(frame.data(), kFrameHeaderBytes - 1, &view));
    std::vector<std::uint8_t> bad_version = frame;
    bad_version[0] = 2;
    BENTO_CHECK(!ParseFrame(bad_version.data(), bad_version.size(), &view));
    std::vector<std::uint8_t> bad_type = frame;
    bad_type[1] = 0x7F;
    BENTO_CHECK(!ParseFrame(bad_type.data(), bad_type.size(), &view));

    // HAS_ACK 但缺少完整 8B ACK 段。
    std::vector<std::uint8_t> truncated_ack(frame.begin(), frame.begin() + kFrameHeaderBytes + 4);
    BENTO_CHECK(!ParseFrame(truncated_ack.data(), truncated_ack.size(), &view));

    // 纯 ACK 帧的载荷必须恰好 8B。
    FrameHeader ack_header;
    ack_header.type = FrameType::kAck;
    BENTO_CHECK(BuildFrame(ack_header, nullptr, nullptr, 1).empty());
    const std::vector<std::uint8_t> short_ack_frame =
        BuildFrame(ack_header, nullptr, payload.data(), 7);
    BENTO_CHECK(short_ack_frame.empty());

    // BARRIER 无载荷；VIDEO 至少 8B 子头。
    FrameHeader barrier_header;
    barrier_header.type = FrameType::kBarrier;
    BENTO_CHECK(BuildFrame(barrier_header, nullptr, payload.data(), 1).empty());
    FrameHeader video_header;
    video_header.type = FrameType::kVideo;
    BENTO_CHECK(BuildFrame(video_header, nullptr, payload.data(), 7).empty());
    BENTO_CHECK(!BuildFrame(video_header, nullptr, payload.data(), 8).empty());
}

void TestDropStale() {
    Relation relation;
    std::vector<std::uint32_t> delivered;
    UnreliableChannel channel(0, &relation,
                              [&delivered](std::uint32_t seq, const std::uint8_t*, std::size_t) {
                                  delivered.push_back(seq);
                              });
    channel.SetFrameSink([](const std::uint8_t*, std::size_t) { return true; });

    InjectDatagram(channel, 5, 0, PayloadOf(5));
    BENTO_CHECK_EQ(delivered.size(), 1U);
    InjectDatagram(channel, 3, 0, PayloadOf(3));
    InjectDatagram(channel, 4, 0, PayloadOf(4));
    InjectDatagram(channel, 2, 0, PayloadOf(2));
    InjectDatagram(channel, 1, 0, PayloadOf(1));
    BENTO_CHECK_EQ(delivered.size(), 1U);
    InjectDatagram(channel, 6, 0, PayloadOf(6));
    BENTO_CHECK_EQ(delivered.size(), 2U);
    BENTO_CHECK_EQ(channel.stats().stale_dropped, 4U);

    // 采纳 100 后乱序注入 1..99：全部是旧包，必须 0 采纳。
    InjectDatagram(channel, 100, 0, PayloadOf(100));
    std::vector<std::uint32_t> stale_sequences(99);
    std::iota(stale_sequences.begin(), stale_sequences.end(), 1U);
    std::mt19937 random(20261026U);
    std::shuffle(stale_sequences.begin(), stale_sequences.end(), random);
    for (const std::uint32_t seq : stale_sequences) {
        InjectDatagram(channel, seq, 0, PayloadOf(seq));
    }
    InjectDatagram(channel, 100, 0, PayloadOf(100));
    BENTO_CHECK_EQ(delivered.size(), 3U);
    BENTO_CHECK(StrictlyIncreasing(delivered));
    BENTO_CHECK_EQ(delivered[0], 5U);
    BENTO_CHECK_EQ(delivered[1], 6U);
    BENTO_CHECK_EQ(delivered[2], 100U);
    BENTO_CHECK_EQ(channel.stats().stale_dropped, 104U);
    BENTO_CHECK_EQ(channel.stats().messages_delivered, 3U);
    BENTO_CHECK_EQ(channel.applied_seq(), 100U);
    BENTO_CHECK_EQ(relation.watermark(), 100U);

    // 长度为 0 的载荷也是合法 DATAGRAM。
    InjectDatagram(channel, 101, 0, {});
    BENTO_CHECK_EQ(delivered.size(), 4U);
    BENTO_CHECK_EQ(delivered[3], 101U);
    BENTO_CHECK(StrictlyIncreasing(delivered));
}

void TestCausalDependency() {
    Relation relation;
    std::vector<std::uint32_t> delivered;
    UnreliableChannel channel(0, &relation,
                              [&delivered](std::uint32_t seq, const std::uint8_t*, std::size_t) {
                                  delivered.push_back(seq);
                              });
    channel.SetFrameSink([](const std::uint8_t*, std::size_t) { return true; });

    InjectDatagram(channel, 10, 0, PayloadOf(10));
    InjectDatagram(channel, 20, 12, PayloadOf(20));
    BENTO_CHECK_EQ(delivered.size(), 1U);
    BENTO_CHECK_EQ(channel.stats().dependency_deferred, 1U);
    BENTO_CHECK_EQ(relation.pending(), 1U);

    InjectDatagram(channel, 11, 0, PayloadOf(11));
    InjectDatagram(channel, 12, 0, PayloadOf(12));
    BENTO_CHECK_EQ(delivered.size(), 4U);
    BENTO_CHECK(StrictlyIncreasing(delivered));
    BENTO_CHECK_EQ(delivered[0], 10U);
    BENTO_CHECK_EQ(delivered[1], 11U);
    BENTO_CHECK_EQ(delivered[2], 12U);
    BENTO_CHECK_EQ(delivered[3], 20U);
    BENTO_CHECK_EQ(relation.pending(), 0U);

    // 依赖被后续帧越过：释放时按 DropStale 丢弃，不产生乱序交付。
    InjectDatagram(channel, 30, 40, PayloadOf(30));
    BENTO_CHECK_EQ(relation.pending(), 1U);
    InjectDatagram(channel, 50, 0, PayloadOf(50));
    BENTO_CHECK_EQ(relation.pending(), 0U);
    BENTO_CHECK(StrictlyIncreasing(delivered));
    BENTO_CHECK(std::find(delivered.begin(), delivered.end(), 30U) == delivered.end());

    // BARRIER：先把 applied 抬到 150，再释放依赖 150 的 200。
    InjectDatagram(channel, 100, 0, PayloadOf(100));
    InjectDatagram(channel, 200, 150, PayloadOf(200));
    BENTO_CHECK_EQ(relation.pending(), 1U);
    channel.OnBarrier(150);
    relation.OnBarrier(150);
    BENTO_CHECK_EQ(relation.pending(), 0U);
    BENTO_CHECK(StrictlyIncreasing(delivered));
    BENTO_CHECK(std::find(delivered.begin(), delivered.end(), 200U) != delivered.end());
    BENTO_CHECK_EQ(channel.applied_seq(), 200U);
}

void TestReliableCausalityAndOrder() {
    Relation relation;
    relation.AdvanceUnreliable(10);
    std::vector<std::uint32_t> delivered;
    ReliableChannel channel(0, &relation,
                            [&delivered](std::uint32_t seq, const std::uint8_t*, std::size_t) {
                                delivered.push_back(seq);
                            });
    channel.SetFrameSink([](const std::uint8_t*, std::size_t) { return true; });

    InjectStream(channel, 1, 12, PayloadOf(1));
    BENTO_CHECK_EQ(delivered.size(), 0U);
    BENTO_CHECK_EQ(channel.stats().dependency_deferred, 1U);
    BENTO_CHECK(!channel.HasPendingAck());

    relation.OnBarrier(12);
    InjectStream(channel, 1, 12, PayloadOf(1));
    BENTO_CHECK_EQ(delivered.size(), 1U);
    BENTO_CHECK_EQ(delivered[0], 1U);
    BENTO_CHECK(channel.HasPendingAck());

    // 乱序缓存 + 有序交付 + 重复/超窗丢弃。
    Relation relation2;
    std::vector<std::uint32_t> ordered;
    ReliableChannel reliable(0, &relation2,
                             [&ordered](std::uint32_t seq, const std::uint8_t*, std::size_t) {
                                 ordered.push_back(seq);
                             });
    reliable.SetFrameSink([](const std::uint8_t*, std::size_t) { return true; });
    InjectStream(reliable, 3, 0, PayloadOf(3));
    InjectStream(reliable, 3, 0, PayloadOf(3));
    InjectStream(reliable, 1, 0, PayloadOf(1));
    InjectStream(reliable, 2, 0, PayloadOf(2));
    BENTO_CHECK_EQ(ordered.size(), 3U);
    BENTO_CHECK_EQ(ordered[0], 1U);
    BENTO_CHECK_EQ(ordered[1], 2U);
    BENTO_CHECK_EQ(ordered[2], 3U);
    InjectStream(reliable, 2, 0, PayloadOf(2));
    BENTO_CHECK_EQ(ordered.size(), 3U);
    BENTO_CHECK_EQ(reliable.stats().duplicates_dropped, 1U);
    InjectStream(reliable, 0, 0, PayloadOf(0));
    BENTO_CHECK_EQ(reliable.stats().invalid_frames, 1U);
    InjectStream(reliable, 5000, 0, PayloadOf(5000));
    BENTO_CHECK_EQ(ordered.size(), 3U);
    BENTO_CHECK_EQ(reliable.stats().stale_dropped, 2U);
    BENTO_CHECK_EQ(reliable.next_rx_seq(), 4U);
}

void TestLinkDatagramLoopback() {
    LinkConfig server_config;
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 5000;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    LinkConfig client_config;
    client_config.peer_address = "127.0.0.1";
    client_config.peer_port = server.local_port();
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 5000;
    client_config.random_seed = 0xB0B0B0B0U;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));

    std::vector<std::uint32_t> received;
    bool both_connected = false;
    server.on_datagram = [&received](std::uint8_t channel, const std::uint8_t* data,
                                     std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 2U);
        BENTO_CHECK(bytes >= 4);
        if (bytes >= 4) {
            received.push_back((static_cast<std::uint32_t>(data[0]) << 24) |
                               (static_cast<std::uint32_t>(data[1]) << 16) |
                               (static_cast<std::uint32_t>(data[2]) << 8) |
                               static_cast<std::uint32_t>(data[3]));
        }
    };
    client.on_connected = [&both_connected]() { both_connected = true; };
    bool server_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.Connect();
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return server_connected && both_connected; }, 5000));

    for (std::uint32_t i = 0; i < 50; ++i) {
        const std::vector<std::uint8_t> payload = PayloadOf(i);
        BENTO_CHECK(client.SendDatagram(2, payload.data(), payload.size()));
    }
    BENTO_CHECK(
        bento_test::PumpUntil(client, server, [&]() { return received.size() == 50; }, 5000));
    BENTO_CHECK_EQ(received.size(), 50U);
    BENTO_CHECK(StrictlyIncreasing(received));
    BENTO_CHECK_EQ(server.unreliable_stats(2).stale_dropped, 0U);

    client.Close();
    server.Close();
}

}  // namespace

int main() {
    TestFrameCodec();
    TestDropStale();
    TestCausalDependency();
    TestReliableCausalityAndOrder();
    TestLinkDatagramLoopback();
    return bento_test::Finish("test_unreliable_dropstale");
}
