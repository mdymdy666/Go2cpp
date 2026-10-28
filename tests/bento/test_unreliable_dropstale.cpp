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
    bad_version[0] = static_cast<std::uint8_t>(kProtocolVersion + 1);
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
    UnreliableChannel::Config config;
    // 128 覆盖 11..100 的整段乱序（89 帧）；小窗口会在注入中途触发容量
    // 淘汰，反而测不到「乱序补齐后按序交付」。
    config.reorder_window = 128;
    config.reorder_timeout_ms = 100;
    UnreliableChannel channel(
        0, &relation,
        [&delivered](std::uint32_t seq, const std::uint8_t*, std::size_t) {
            delivered.push_back(seq);
        },
        config);
    channel.SetFrameSink([](const std::uint8_t*, std::size_t) { return true; });

    // 乱序注入 1..8：全部进乱序缓存，缺口补齐后按序交付（应用层 0 回退）。
    const std::vector<std::uint32_t> shuffled{5, 3, 1, 7, 2, 8, 4, 6};
    for (const std::uint32_t seq : shuffled) {
        InjectDatagram(channel, seq, 0, PayloadOf(seq));
    }
    BENTO_CHECK_EQ(delivered.size(), 8U);
    BENTO_CHECK(StrictlyIncreasing(delivered));
    BENTO_CHECK_EQ(channel.applied_seq(), 8U);
    BENTO_CHECK_EQ(channel.reorder_pending(), 0U);
    BENTO_CHECK_EQ(channel.stats().stale_dropped, 0U);
    // 5/3/7/8 进缓存；1/2/4/6 到达时与水位连续，直接交付并冲刷后续。
    BENTO_CHECK(channel.stats().reorder_buffered >= 4U);

    // 已交付之后到达的旧包/重复包：真·过期，绝不采纳（0 回退）。
    InjectDatagram(channel, 4, 0, PayloadOf(4));
    InjectDatagram(channel, 8, 0, PayloadOf(8));
    BENTO_CHECK_EQ(delivered.size(), 8U);
    BENTO_CHECK_EQ(channel.stats().stale_dropped, 2U);

    // 缺口 9 永久丢失：10 先到进缓存，超时后跳过缺口交付（实时性兜底）。
    InjectDatagram(channel, 10, 0, PayloadOf(10));
    BENTO_CHECK_EQ(delivered.size(), 8U);
    channel.OnTimer(NowMs() + config.reorder_timeout_ms + 10);
    BENTO_CHECK_EQ(delivered.size(), 9U);
    BENTO_CHECK_EQ(delivered.back(), 10U);
    BENTO_CHECK_EQ(channel.applied_seq(), 10U);
    BENTO_CHECK_EQ(channel.stats().reorder_skipped, 1U);

    // 越过 10 之后旧包 9 才到：不再采纳（过期不接受）。
    InjectDatagram(channel, 9, 0, PayloadOf(9));
    BENTO_CHECK_EQ(delivered.size(), 9U);
    BENTO_CHECK_EQ(channel.stats().stale_dropped, 3U);

    // 100 先到、随后 11..99 乱序补齐：全部缓存后按序交付，100 也必须
    // 排在 99 之后（乱序容忍不等于乱序交付）。
    InjectDatagram(channel, 100, 0, PayloadOf(100));
    std::vector<std::uint32_t> middle(89);
    std::iota(middle.begin(), middle.end(), 11U);
    std::mt19937 random(20261026U);
    std::shuffle(middle.begin(), middle.end(), random);
    for (const std::uint32_t seq : middle) {
        InjectDatagram(channel, seq, 0, PayloadOf(seq));
    }
    BENTO_CHECK_EQ(delivered.size(), 99U);  // 1..8 + 10 + 11..100（缺 9）。
    BENTO_CHECK(StrictlyIncreasing(delivered));
    BENTO_CHECK_EQ(delivered.back(), 100U);
    BENTO_CHECK(std::find(delivered.begin(), delivered.end(), 9U) == delivered.end());
    BENTO_CHECK_EQ(channel.applied_seq(), 100U);
    BENTO_CHECK_EQ(relation.watermark(), 100U);
    BENTO_CHECK_EQ(channel.stats().stale_dropped, 3U);

    // applied=100 之后注入 1..100：全部是旧包，0 采纳。
    for (std::uint32_t seq = 1; seq <= 100; ++seq) {
        InjectDatagram(channel, seq, 0, PayloadOf(seq));
    }
    BENTO_CHECK_EQ(delivered.size(), 99U);
    BENTO_CHECK_EQ(channel.stats().stale_dropped, 103U);

    // 长度为 0 的载荷也是合法 DATAGRAM。
    InjectDatagram(channel, 101, 0, {});
    BENTO_CHECK_EQ(delivered.size(), 100U);
    BENTO_CHECK_EQ(delivered.back(), 101U);
    BENTO_CHECK(StrictlyIncreasing(delivered));
}

void TestCausalDependency() {
    Relation relation;
    std::vector<std::uint32_t> delivered;
    UnreliableChannel::Config config;
    config.reorder_window = 16;
    config.reorder_timeout_ms = 100;
    UnreliableChannel channel(
        0, &relation,
        [&delivered](std::uint32_t seq, const std::uint8_t*, std::size_t) {
            delivered.push_back(seq);
        },
        config);
    channel.SetFrameSink([](const std::uint8_t*, std::size_t) { return true; });

    // 先建立连续基线 1..10。
    for (std::uint32_t seq = 1; seq <= 10; ++seq) {
        InjectDatagram(channel, seq, 0, PayloadOf(seq));
    }
    BENTO_CHECK_EQ(delivered.size(), 10U);
    BENTO_CHECK(StrictlyIncreasing(delivered));

    // dep 未满足：20 挂起，不交付也不推进 applied。
    InjectDatagram(channel, 20, 12, PayloadOf(20));
    BENTO_CHECK_EQ(delivered.size(), 10U);
    BENTO_CHECK_EQ(channel.stats().dependency_deferred, 1U);
    BENTO_CHECK_EQ(relation.pending(), 1U);

    // 11、12 到达 → 水位推进 → 释放 20（因果等待后直接交付，不再进缓存）。
    InjectDatagram(channel, 11, 0, PayloadOf(11));
    InjectDatagram(channel, 12, 0, PayloadOf(12));
    BENTO_CHECK_EQ(delivered.size(), 13U);
    BENTO_CHECK(StrictlyIncreasing(delivered));
    BENTO_CHECK_EQ(delivered.back(), 20U);
    BENTO_CHECK_EQ(relation.pending(), 0U);

    // 依赖被后续水位越过：30(dep 40) 挂起；50 到达后超时跳过缺口交付，
    // 水位越过 30，30 按 DropStale 丢弃（不产生回退交付）。
    InjectDatagram(channel, 30, 40, PayloadOf(30));
    BENTO_CHECK_EQ(relation.pending(), 1U);
    InjectDatagram(channel, 50, 0, PayloadOf(50));
    BENTO_CHECK_EQ(delivered.size(), 13U);  // 50 在缓存等缺口 21..49。
    channel.OnTimer(NowMs() + config.reorder_timeout_ms + 10);
    BENTO_CHECK_EQ(delivered.back(), 50U);
    BENTO_CHECK_EQ(relation.pending(), 0U);
    BENTO_CHECK(std::find(delivered.begin(), delivered.end(), 30U) == delivered.end());
    BENTO_CHECK(StrictlyIncreasing(delivered));

    // BARRIER：先把 applied 抬到 150，再释放 dep=150 的 200。
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
