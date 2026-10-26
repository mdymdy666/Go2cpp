/**
 * @file test_stream_fragment.cpp
 * @brief STREAM >1300B 分片：5000B 重装、乱序容忍、缺片超时、非法子头。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

std::vector<std::uint8_t> MakeMessage(std::size_t bytes, std::uint8_t seed) {
    std::vector<std::uint8_t> message(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        message[i] = static_cast<std::uint8_t>((i * 37U + seed) & 0xFFU);
    }
    return message;
}

std::vector<std::vector<std::uint8_t>> MakeFragments(std::uint32_t frame_id,
                                                      const std::vector<std::uint8_t>& message) {
    const std::size_t count =
        message.empty() ? 1 : (message.size() + kStreamFragmentBytes - 1) / kStreamFragmentBytes;
    std::vector<std::vector<std::uint8_t>> fragments;
    fragments.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t offset = index * kStreamFragmentBytes;
        const std::size_t chunk =
            message.empty() ? 0 : std::min(kStreamFragmentBytes, message.size() - offset);
        std::vector<std::uint8_t> payload(kStreamFragmentHeaderBytes + chunk, 0);
        StreamFragmentHeader header;
        header.frame_id = frame_id;
        header.frag_index = static_cast<std::uint16_t>(index);
        header.frag_count = static_cast<std::uint16_t>(count);
        EncodeStreamFragmentHeader(header, payload.data());
        if (chunk > 0) {
            std::memcpy(payload.data() + kStreamFragmentHeaderBytes, message.data() + offset, chunk);
        }
        fragments.push_back(std::move(payload));
    }
    return fragments;
}

StreamReassembler::Result Feed(StreamReassembler& reassembler,
                               const std::vector<std::uint8_t>& payload, std::uint64_t now_ms) {
    StreamFragmentHeader header;
    if (payload.size() < kStreamFragmentHeaderBytes ||
        !DecodeStreamFragmentHeader(payload.data(), payload.size(), &header)) {
        return StreamReassembler::Result::kInvalid;
    }
    return reassembler.AddFragment(header.frame_id, header.frag_index, header.frag_count,
                                   payload.data() + kStreamFragmentHeaderBytes,
                                   payload.size() - kStreamFragmentHeaderBytes, now_ms);
}

struct Delivery {
    std::uint32_t frame_id = 0;
    std::vector<std::uint8_t> data;
};

void TestReassemblerOutOfOrder() {
    StreamReassembler reassembler;
    std::vector<Delivery> delivered;
    reassembler.SetDeliver([&delivered](std::uint32_t frame_id, const std::uint8_t* data,
                                        std::size_t bytes) {
        delivered.push_back(Delivery{frame_id, std::vector<std::uint8_t>(data, data + bytes)});
    });

    const std::vector<std::uint8_t> message = MakeMessage(5000, 7);
    const std::vector<std::vector<std::uint8_t>> fragments = MakeFragments(7, message);
    BENTO_CHECK_EQ(fragments.size(), 4U);

    // 3,1,0,2 乱序注入：最后一片到达前不得交付。
    BENTO_CHECK(Feed(reassembler, fragments[3], 1000) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, fragments[1], 1001) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, fragments[0], 1002) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK_EQ(delivered.size(), 0U);
    BENTO_CHECK(Feed(reassembler, fragments[2], 1003) ==
                StreamReassembler::Result::kDelivered);
    BENTO_CHECK_EQ(delivered.size(), 1U);
    BENTO_CHECK_EQ(delivered[0].frame_id, 7U);
    BENTO_CHECK(delivered[0].data == message);
    BENTO_CHECK_EQ(reassembler.stats().frames_delivered, 1U);
    BENTO_CHECK_EQ(reassembler.stats().fragments_received, 4U);
    BENTO_CHECK_EQ(reassembler.active(), 0U);
}

void TestReassemblerDuplicateAndInterleave() {
    StreamReassembler reassembler;
    std::vector<Delivery> delivered;
    reassembler.SetDeliver([&delivered](std::uint32_t frame_id, const std::uint8_t* data,
                                        std::size_t bytes) {
        delivered.push_back(Delivery{frame_id, std::vector<std::uint8_t>(data, data + bytes)});
    });

    const std::vector<std::uint8_t> message = MakeMessage(3000, 5);
    const std::vector<std::vector<std::uint8_t>> fragments = MakeFragments(5, message);
    BENTO_CHECK_EQ(fragments.size(), 3U);
    BENTO_CHECK(Feed(reassembler, fragments[0], 2000) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, fragments[0], 2001) == StreamReassembler::Result::kDuplicate);
    BENTO_CHECK_EQ(reassembler.stats().fragments_duplicate, 1U);
    BENTO_CHECK(Feed(reassembler, fragments[2], 2002) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, fragments[1], 2003) == StreamReassembler::Result::kDelivered);
    BENTO_CHECK_EQ(delivered.size(), 1U);
    BENTO_CHECK(delivered[0].data == message);

    // 两个 frame_id 的分片交错到达，各自独立重组。
    const std::vector<std::vector<std::uint8_t>> a = MakeFragments(20, MakeMessage(3000, 20));
    const std::vector<std::vector<std::uint8_t>> b = MakeFragments(21, MakeMessage(3000, 21));
    BENTO_CHECK(Feed(reassembler, a[0], 3000) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, b[0], 3001) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, b[1], 3002) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, a[1], 3003) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, a[2], 3004) == StreamReassembler::Result::kDelivered);
    BENTO_CHECK(Feed(reassembler, b[2], 3005) == StreamReassembler::Result::kDelivered);
    BENTO_CHECK_EQ(delivered.size(), 3U);
    BENTO_CHECK_EQ(delivered[1].frame_id, 20U);
    BENTO_CHECK_EQ(delivered[2].frame_id, 21U);
    BENTO_CHECK(delivered[1].data == MakeMessage(3000, 20));
    BENTO_CHECK(delivered[2].data == MakeMessage(3000, 21));
}

void TestReassemblerMissingTimeout() {
    StreamReassembler reassembler;
    std::vector<Delivery> delivered;
    reassembler.SetDeliver([&delivered](std::uint32_t frame_id, const std::uint8_t* data,
                                        std::size_t bytes) {
        delivered.push_back(Delivery{frame_id, std::vector<std::uint8_t>(data, data + bytes)});
    });

    const std::vector<std::vector<std::uint8_t>> fragments =
        MakeFragments(8, MakeMessage(4000, 8));
    BENTO_CHECK_EQ(fragments.size(), 4U);
    BENTO_CHECK(Feed(reassembler, fragments[0], 1000) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, fragments[2], 1001) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK_EQ(reassembler.active(), 1U);

    const std::uint64_t timeout = StreamReassembler::kDefaultTimeoutMs;
    BENTO_CHECK_EQ(reassembler.OnTimer(1001 + timeout), 1U);
    BENTO_CHECK_EQ(reassembler.active(), 0U);
    BENTO_CHECK_EQ(reassembler.stats().frames_timed_out, 1U);

    // 缺片超时后补来的分片只能形成新的半成品，永远无法交付（消息已丢弃）：
    // 这是「无重传的不可靠分片」语义；可靠 STREAM 由 seq 重传兜底，不会走到这里。
    BENTO_CHECK(Feed(reassembler, fragments[1], 1002) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, fragments[3], 1003) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK_EQ(delivered.size(), 0U);
    BENTO_CHECK_EQ(reassembler.OnTimer(1003 + timeout), 1U);
    BENTO_CHECK_EQ(reassembler.stats().frames_timed_out, 2U);
}

void TestReassemblerInvalid() {
    StreamReassembler reassembler;
    const std::uint8_t data[kStreamFragmentBytes] = {0};

    BENTO_CHECK(reassembler.AddFragment(10, 0, 0, data, 1, 1000) ==
                StreamReassembler::Result::kInvalid);
    BENTO_CHECK(reassembler.AddFragment(10, 2, 2, data, 1, 1000) ==
                StreamReassembler::Result::kInvalid);
    BENTO_CHECK(reassembler.AddFragment(10, 0, 2, data, 100, 1000) ==
                StreamReassembler::Result::kInvalid);  // 非末片必须满片
    BENTO_CHECK(reassembler.AddFragment(10, 0, 1, data,
                                        kStreamFragmentBytes + 1, 1000) ==
                StreamReassembler::Result::kInvalid);
    BENTO_CHECK(reassembler.AddFragment(10, 0, 1, nullptr, 4, 1000) ==
                StreamReassembler::Result::kInvalid);
    BENTO_CHECK(reassembler.AddFragment(10, 0, static_cast<std::uint16_t>(
                                                   kMaxStreamFragments + 1),
                                        data, 1, 1000) ==
                StreamReassembler::Result::kInvalid);
    BENTO_CHECK_EQ(reassembler.stats().fragments_invalid, 6U);
    BENTO_CHECK_EQ(reassembler.active(), 0U);
}

void TestReassemblerEvictionAndReset() {
    StreamReassembler::Config config;
    config.max_active = 1;
    StreamReassembler reassembler(config);
    std::vector<Delivery> delivered;
    reassembler.SetDeliver([&delivered](std::uint32_t frame_id, const std::uint8_t* data,
                                        std::size_t bytes) {
        delivered.push_back(Delivery{frame_id, std::vector<std::uint8_t>(data, data + bytes)});
    });

    const std::vector<std::vector<std::uint8_t>> a = MakeFragments(30, MakeMessage(3000, 30));
    const std::vector<std::vector<std::uint8_t>> b = MakeFragments(31, MakeMessage(3000, 31));
    BENTO_CHECK(Feed(reassembler, a[0], 1000) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, b[0], 1001) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK_EQ(reassembler.stats().frames_evicted, 1U);
    BENTO_CHECK(Feed(reassembler, b[1], 1002) == StreamReassembler::Result::kBuffered);
    BENTO_CHECK(Feed(reassembler, b[2], 1003) == StreamReassembler::Result::kDelivered);
    BENTO_CHECK_EQ(delivered.size(), 1U);
    BENTO_CHECK_EQ(delivered[0].frame_id, 31U);

    reassembler.Reset();
    BENTO_CHECK_EQ(reassembler.active(), 0U);
    BENTO_CHECK_EQ(reassembler.stats().frames_delivered, 0U);
}

void TestLinkLoopback5000() {
    LinkConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 5000;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    LinkConfig client_config;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.peer_port = server.local_port();
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 5000;
    client_config.random_seed = 0x46524147U;  // "FRAG"
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));

    std::vector<std::vector<std::uint8_t>> received;
    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };
    server.on_stream = [&received](std::uint8_t channel, const std::uint8_t* data,
                                   std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 0U);
        received.emplace_back(data, data + bytes);
    };

    client.Connect();
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000));

    const std::vector<std::uint8_t> payload5000 = MakeMessage(5000, 0x5A);
    const std::vector<std::uint8_t> payload1301 = MakeMessage(1301, 0x5B);
    const std::vector<std::uint8_t> payload1300 = MakeMessage(1300, 0x5C);
    const std::uint8_t dummy = 0;

    BENTO_CHECK(client.SendStream(0, payload5000.data(), payload5000.size()));
    BENTO_CHECK(client.SendStream(0, payload1301.data(), payload1301.size()));
    BENTO_CHECK(client.SendStream(0, payload1300.data(), payload1300.size()));
    BENTO_CHECK(client.SendStream(0, &dummy, 0));

    // 等到 4 条消息交付且发送端 inflight 清空，统计才是最终值。
    BENTO_CHECK(bento_test::PumpUntil(
        client, server,
        [&]() {
            return received.size() == 4 && client.reliable_stats(0).inflight == 0;
        },
        10000));
    BENTO_CHECK_EQ(received.size(), 4U);
    if (received.size() == 4) {
        BENTO_CHECK(received[0] == payload5000);
        BENTO_CHECK(received[1] == payload1301);
        BENTO_CHECK(received[2] == payload1300);
        BENTO_CHECK_EQ(received[3].size(), 0U);
    }

    const auto client_stats = client.reliable_stats(0);
    const auto server_stats = server.reliable_stats(0);
    BENTO_CHECK_EQ(client_stats.messages_sent, 4U);
    BENTO_CHECK_EQ(client_stats.fragments_sent, 8U);  // 4 + 2 + 1 + 1
    BENTO_CHECK_EQ(server_stats.messages_delivered, 4U);
    BENTO_CHECK_EQ(server_stats.messages_reassembled, 2U);
    BENTO_CHECK_EQ(server_stats.fragments_received, 6U);  // 仅分片帧：4+2
    BENTO_CHECK_EQ(client_stats.retransmits, 0U);
    BENTO_CHECK_EQ(client_stats.pto_ms, 200U);  // 1ms 样本 + 25ms 后仍被 200ms 下限夹住

    const std::vector<std::uint8_t> too_large(kMaxStreamMessageBytes + 1, 0x7F);
    BENTO_CHECK(!client.SendStream(0, too_large.data(), too_large.size()));

    client.Close();
    server.Close();
}

}  // namespace

int main() {
    TestReassemblerOutOfOrder();
    TestReassemblerDuplicateAndInterleave();
    TestReassemblerMissingTimeout();
    TestReassemblerInvalid();
    TestReassemblerEvictionAndReset();
    TestLinkLoopback5000();
    return bento_test::Finish("test_stream_fragment");
}
