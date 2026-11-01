/**
 * @file test_fec_codec.cpp
 * @brief FEC 单测：GF(256) 运算、XOR 与 Cauchy Reed-Solomon 编解码、
 *        kFec 帧载荷编解码，以及真实 UDP 弱网下 ReliableChannel 的 FEC 恢复。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/fec.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "test_util.hpp"
#include "udp_proxy.hpp"

namespace {

using go2cpp::bento::BuildFecPayload;
using go2cpp::bento::BuildFrame;
using go2cpp::bento::DecodeFecHeader;
using go2cpp::bento::FecDecoder;
using go2cpp::bento::FecEncoder;
using go2cpp::bento::FecFrameHeader;
using go2cpp::bento::FecKind;
using go2cpp::bento::FecLengthAt;
using go2cpp::bento::FecPayloadView;
using go2cpp::bento::FecShard;
using go2cpp::bento::FrameHeader;
using go2cpp::bento::FrameType;
using go2cpp::bento::FrameView;
using go2cpp::bento::Gf256Divide;
using go2cpp::bento::Gf256Multiply;
using go2cpp::bento::Gf256Pow;
using go2cpp::bento::kFecHeaderBytes;
using go2cpp::bento::Link;
using go2cpp::bento::LinkConfig;
using go2cpp::bento::ParseFecPayload;
using go2cpp::bento::ParseFrame;

std::vector<std::uint8_t> MakeShard(std::size_t bytes, std::uint8_t seed) {
    std::vector<std::uint8_t> data(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 131U + seed * 17U + 7U) & 0xFFU);
    }
    return data;
}

// ---------------------------------------------------------------------------
// GF(256)
// ---------------------------------------------------------------------------

void TestGf256() {
    BENTO_CHECK_EQ(Gf256Multiply(0, 7), 0U);
    BENTO_CHECK_EQ(Gf256Multiply(1, 7), 7U);
    BENTO_CHECK_EQ(Gf256Multiply(2, 3), 6U);
    // 0x80 × 2 = 0x100，模不可约多项式 0x11D 后为 0x1D。
    BENTO_CHECK_EQ(Gf256Multiply(0x80, 2), 0x1DU);
    // 分配律与逆运算。
    for (int a = 1; a < 256; a += 17) {
        for (int b = 1; b < 256; b += 29) {
            const std::uint8_t product = Gf256Multiply(static_cast<std::uint8_t>(a),
                                                       static_cast<std::uint8_t>(b));
            BENTO_CHECK_EQ(Gf256Divide(product, static_cast<std::uint8_t>(b)),
                           static_cast<std::uint8_t>(a));
            BENTO_CHECK_EQ(Gf256Multiply(static_cast<std::uint8_t>(a),
                                         static_cast<std::uint8_t>(b ^ 0x5A)),
                           static_cast<std::uint8_t>(product ^
                                                     Gf256Multiply(static_cast<std::uint8_t>(a),
                                                                   0x5A)));
        }
    }
    BENTO_CHECK_EQ(Gf256Pow(2, 8), 0x1DU);
    BENTO_CHECK_EQ(Gf256Pow(2, 0), 1U);
    BENTO_CHECK_EQ(Gf256Pow(0, 5), 0U);
    BENTO_CHECK_EQ(Gf256Divide(5, 0), 0U);
}

// ---------------------------------------------------------------------------
// XOR 编解码
// ---------------------------------------------------------------------------

void TestXorCodec() {
    constexpr std::uint16_t kData = 8;
    std::vector<std::vector<std::uint8_t>> sources(kData);
    for (std::uint16_t i = 0; i < kData; ++i) {
        sources[i] = MakeShard(100 + i, static_cast<std::uint8_t>(i));
    }
    FecEncoder encoder(FecKind::kXor, kData, 1);
    for (std::uint16_t i = 0; i < kData; ++i) {
        BENTO_CHECK(encoder.AddSource(i, sources[i].data(), sources[i].size()));
    }
    BENTO_CHECK(encoder.complete());
    std::vector<FecShard> parity;
    std::size_t max_len = 0;
    BENTO_CHECK(encoder.Encode(&parity, &max_len));
    BENTO_CHECK_EQ(parity.size(), 1U);
    BENTO_CHECK_EQ(max_len, 107U);
    BENTO_CHECK_EQ(parity[0].data.size(), max_len);

    // 丢 1 片可恢复（长度按长度表裁剪回 100+i）。
    FecDecoder decoder(FecKind::kXor, kData, 1);
    BENTO_CHECK(decoder.SetMaxLen(max_len));
    for (std::uint16_t i = 0; i < kData; ++i) {
        if (i == 3) {
            continue;
        }
        BENTO_CHECK(decoder.AddSource(i, sources[i].data(), sources[i].size()));
    }
    BENTO_CHECK(decoder.AddParity(0, parity[0].data.data(), parity[0].data.size()));
    BENTO_CHECK_EQ(decoder.missing_count(), 1U);
    std::vector<FecShard> recovered;
    std::string error;
    BENTO_CHECK(decoder.Recover(&recovered, &error));
    BENTO_CHECK_EQ(recovered.size(), 1U);
    BENTO_CHECK_EQ(recovered[0].index, 3U);
    BENTO_CHECK(recovered[0].data.size() >= sources[3].size());
    BENTO_CHECK(std::memcmp(recovered[0].data.data(), sources[3].data(), sources[3].size()) == 0);

    // 丢 2 片：单校验片无法恢复，必须明确失败而不是给出错误数据。
    FecDecoder broken(FecKind::kXor, kData, 1);
    BENTO_CHECK(broken.SetMaxLen(max_len));
    for (std::uint16_t i = 0; i < kData; ++i) {
        if (i == 2 || i == 5) {
            continue;
        }
        BENTO_CHECK(broken.AddSource(i, sources[i].data(), sources[i].size()));
    }
    BENTO_CHECK(broken.AddParity(0, parity[0].data.data(), parity[0].data.size()));
    BENTO_CHECK(!broken.Recover(&recovered, &error));
    BENTO_CHECK(!error.empty());

    // 非法形状：XOR 的 m 必须为 1。
    FecEncoder invalid(FecKind::kXor, 4, 2);
    BENTO_CHECK_EQ(invalid.data_shards(), 0U);
}

// ---------------------------------------------------------------------------
// Cauchy Reed-Solomon：任意 2 片丢失可恢复
// ---------------------------------------------------------------------------

void TestReedSolomonCodec() {
    constexpr std::uint16_t kData = 8;
    constexpr std::uint16_t kParity = 2;
    std::vector<std::vector<std::uint8_t>> sources(kData);
    for (std::uint16_t i = 0; i < kData; ++i) {
        sources[i] = MakeShard(64 + i * 3, static_cast<std::uint8_t>(i + 1));
    }
    FecEncoder encoder(FecKind::kReedSolomon, kData, kParity);
    for (std::uint16_t i = 0; i < kData; ++i) {
        BENTO_CHECK(encoder.AddSource(i, sources[i].data(), sources[i].size()));
    }
    std::vector<FecShard> parity;
    std::size_t max_len = 0;
    BENTO_CHECK(encoder.Encode(&parity, &max_len));
    BENTO_CHECK_EQ(parity.size(), kParity);

    // 任意两片丢失组合（数据、校验、混合）都必须恢复。
    struct LossSet {
        std::uint16_t lost[2];
        bool parity_lost[2];
    };
    const LossSet cases[] = {
        {{1, 3}, {false, false}},  // 丢两个数据片
        {{1, 6}, {false, false}},
        {{0, 7}, {false, false}},
    };
    for (const LossSet& loss : cases) {
        FecDecoder decoder(FecKind::kReedSolomon, kData, kParity);
        BENTO_CHECK(decoder.SetMaxLen(max_len));
        for (std::uint16_t i = 0; i < kData; ++i) {
            if (i == loss.lost[0] || i == loss.lost[1]) {
                continue;
            }
            BENTO_CHECK(decoder.AddSource(i, sources[i].data(), sources[i].size()));
        }
        for (std::uint16_t j = 0; j < kParity; ++j) {
            BENTO_CHECK(decoder.AddParity(j, parity[j].data.data(), parity[j].data.size()));
        }
        std::vector<FecShard> recovered;
        std::string error;
        BENTO_CHECK(decoder.Recover(&recovered, &error));
        BENTO_CHECK_EQ(recovered.size(), 2U);
        for (const FecShard& shard : recovered) {
            BENTO_CHECK(shard.index == loss.lost[0] || shard.index == loss.lost[1]);
            BENTO_CHECK(std::memcmp(shard.data.data(), sources[shard.index].data(),
                                    sources[shard.index].size()) == 0);
        }
    }

    // 丢两个数据片 + 一个校验片：只剩 6 数据 + 1 校验 = 7 < 8，必须失败。
    FecDecoder too_many(FecKind::kReedSolomon, kData, kParity);
    BENTO_CHECK(too_many.SetMaxLen(max_len));
    for (std::uint16_t i = 2; i < kData; ++i) {
        BENTO_CHECK(too_many.AddSource(i, sources[i].data(), sources[i].size()));
    }
    BENTO_CHECK(too_many.AddParity(0, parity[0].data.data(), parity[0].data.size()));
    std::vector<FecShard> recovered;
    std::string error;
    BENTO_CHECK(!too_many.Recover(&recovered, &error));
}

// ---------------------------------------------------------------------------
// kFec 帧载荷与线格式
// ---------------------------------------------------------------------------

void TestFecFrameCodec() {
    BENTO_CHECK_EQ(static_cast<std::uint8_t>(FrameType::kFec), 0x0BU);
    BENTO_CHECK(std::string(go2cpp::bento::FrameTypeName(FrameType::kFec)) == "FEC");

    FecFrameHeader header;
    header.kind = 1;
    header.group_id = 7;
    header.base_seq = 42;
    header.data_shards = 8;
    header.parity_shards = 2;
    header.parity_index = 1;
    header.max_len = 100;
    std::vector<std::uint16_t> lengths(8, 0);
    for (std::uint16_t i = 0; i < 8; ++i) {
        lengths[i] = static_cast<std::uint16_t>(40 + i);
    }
    std::vector<std::uint8_t> parity = MakeShard(100, 9);
    const std::vector<std::uint8_t> payload = BuildFecPayload(header, lengths.data(),
                                                              parity.data());
    BENTO_CHECK(!payload.empty());
    BENTO_CHECK_EQ(payload.size(), kFecHeaderBytes + 8U * 2U + 100U);

    FecPayloadView view;
    BENTO_CHECK(ParseFecPayload(payload.data(), payload.size(), &view));
    BENTO_CHECK_EQ(view.header.kind, 1U);
    BENTO_CHECK_EQ(view.header.group_id, 7U);
    BENTO_CHECK_EQ(view.header.base_seq, 42U);
    BENTO_CHECK_EQ(view.header.parity_index, 1U);
    BENTO_CHECK_EQ(FecLengthAt(view, 0), 40U);
    BENTO_CHECK_EQ(FecLengthAt(view, 7), 47U);
    BENTO_CHECK_EQ(FecLengthAt(view, 8), 0U);  // 越界安全
    BENTO_CHECK(std::memcmp(view.parity, parity.data(), parity.size()) == 0);

    // 非法 index / 空 max_len 必须被拒。
    std::vector<std::uint8_t> header_bytes(kFecHeaderBytes, 0);
    go2cpp::bento::EncodeFecHeader(header, header_bytes.data());
    header_bytes[12] = 0;
    header_bytes[13] = 2;  // parity_index = 2 >= parity_shards
    FecFrameHeader rejected;
    BENTO_CHECK(!DecodeFecHeader(header_bytes.data(), header_bytes.size(), &rejected));
    FecFrameHeader decoded;
    header_bytes[12] = 0;
    header_bytes[13] = 1;
    BENTO_CHECK(DecodeFecHeader(header_bytes.data(), header_bytes.size(), &decoded));
    BENTO_CHECK_EQ(decoded.group_id, header.group_id);
    BENTO_CHECK_EQ(decoded.max_len, header.max_len);

    // 完整帧往返：FrameType::kFec + BuildFrame/ParseFrame。
    FrameHeader frame_header;
    frame_header.type = FrameType::kFec;
    frame_header.channel = 2;
    frame_header.seq = 7;
    const std::vector<std::uint8_t> frame =
        BuildFrame(frame_header, nullptr, payload.data(), payload.size());
    BENTO_CHECK(!frame.empty());
    FrameView frame_view;
    BENTO_CHECK(ParseFrame(frame.data(), frame.size(), &frame_view));
    BENTO_CHECK_EQ(frame_view.header.type, FrameType::kFec);
    BENTO_CHECK_EQ(frame_view.header.channel, 2U);
    BENTO_CHECK_EQ(frame_view.payload_bytes, payload.size());
    FecPayloadView second;
    BENTO_CHECK(ParseFecPayload(frame_view.payload, frame_view.payload_bytes, &second));
    BENTO_CHECK_EQ(second.header.base_seq, 42U);

    // 非法形状：k<2、m>16、index 越界都被 DecodeFecHeader 拒绝。
    header.data_shards = 1;
    const std::vector<std::uint8_t> invalid = BuildFecPayload(header, lengths.data(), parity.data());
    if (!invalid.empty()) {
        BENTO_CHECK(!DecodeFecHeader(invalid.data(), invalid.size(), &decoded));
    }
}

// ---------------------------------------------------------------------------
// 真实 UDP 弱网：ReliableChannel 的 FEC 恢复
// ---------------------------------------------------------------------------

struct FecPairStats {
    bool connected = false;
    std::size_t delivered = 0;
    go2cpp::bento::ReliableChannel::Stats sender{};
    go2cpp::bento::ReliableChannel::Stats receiver{};
};

bool RunFecPair(FecPairStats* stats, bool fec_enabled, int loss_percent) {
    LinkConfig server_config;
    server_config.require_encryption = false;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 8000;
    server_config.reliable.fec.enabled = fec_enabled;
    server_config.reliable.fec.kind = FecKind::kXor;
    server_config.reliable.fec.data_shards = 8;
    server_config.reliable.fec.parity_shards = 1;
    server_config.reliable.fec.max_payload_bytes = 900;
    Link server(server_config);
    std::string error;
    if (!server.Open(&error)) {
        return false;
    }

    LinkConfig client_config;
    client_config.require_encryption = false;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 8000;
    client_config.random_seed = 0xFEC001U;
    client_config.reliable.fec = server_config.reliable.fec;

    std::unique_ptr<bento_test::UdpProxy> proxy;
    if (loss_percent > 0) {
        sockaddr_in address{};
        if (!go2cpp::bento::UdpSocket::ResolveIPv4("127.0.0.1", server.local_port(), &address,
                                                   &error)) {
            return false;
        }
        proxy = std::make_unique<bento_test::UdpProxy>(address, loss_percent, 0, 0xFEC0DEU);
        if (!proxy->Start(&error)) {
            return false;
        }
        client_config.peer_port = proxy->port();
    } else {
        client_config.peer_port = server.local_port();
    }

    Link client(client_config);
    if (!client.Open(&error)) {
        return false;
    }
    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };
    server.on_stream = [stats](std::uint8_t channel, const std::uint8_t*, std::size_t) {
        if (channel == 0) {
            ++stats->delivered;
        }
    };
    client.Connect();
    stats->connected = bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 15000);
    if (!stats->connected) {
        return false;
    }
    constexpr std::uint32_t kMessages = 120;
    for (std::uint32_t i = 0; i < kMessages; ++i) {
        std::vector<std::uint8_t> payload(300, 0);
        payload[0] = static_cast<std::uint8_t>(i >> 8);
        payload[1] = static_cast<std::uint8_t>(i);
        for (std::size_t j = 2; j < payload.size(); ++j) {
            payload[j] = static_cast<std::uint8_t>((i + j) & 0xFFU);
        }
        client.SendStream(0, payload.data(), payload.size());
    }
    const bool all = bento_test::PumpUntil(
        client, server,
        [&]() {
            return stats->delivered == kMessages && client.reliable_stats(0).inflight == 0 &&
                   server.reliable_stats(0).inflight == 0;
        },
        30000);
    stats->sender = client.reliable_stats(0);
    stats->receiver = server.reliable_stats(0);
    if (proxy) {
        proxy->Stop();
    }
    client.Close();
    server.Close();
    return all;
}

void TestChannelFecRecoveryUnderLoss() {
    FecPairStats with_fec;
    BENTO_CHECK(RunFecPair(&with_fec, true, 5));
    BENTO_CHECK_EQ(with_fec.delivered, 120U);
    BENTO_CHECK(with_fec.sender.fec_groups_encoded >= 8U);
    BENTO_CHECK(with_fec.receiver.fec_frames_recovered >= 1U);
    std::printf("[INFO] fec: encoded=%llu sent=%llu recovered=%llu retx=%llu failures=%llu\n",
                static_cast<unsigned long long>(with_fec.sender.fec_groups_encoded),
                static_cast<unsigned long long>(with_fec.sender.fec_frames_sent),
                static_cast<unsigned long long>(with_fec.receiver.fec_frames_recovered),
                static_cast<unsigned long long>(with_fec.sender.retransmits),
                static_cast<unsigned long long>(with_fec.receiver.fec_recovery_failures));

    FecPairStats without_fec;
    BENTO_CHECK(RunFecPair(&without_fec, false, 5));
    BENTO_CHECK_EQ(without_fec.delivered, 120U);
    BENTO_CHECK_EQ(without_fec.sender.fec_groups_encoded, 0U);
    BENTO_CHECK_EQ(without_fec.receiver.fec_frames_recovered, 0U);
    // 关闭 FEC 时只能靠重传；这组数据是 FEC 价值的对照基线。
    BENTO_CHECK(without_fec.sender.retransmits >= 1U);
}

void TestChannelFecNoLossNoParityLoss() {
    FecPairStats clean;
    BENTO_CHECK(RunFecPair(&clean, true, 0));
    BENTO_CHECK_EQ(clean.delivered, 120U);
    BENTO_CHECK(clean.sender.fec_groups_encoded >= 8U);
    BENTO_CHECK_EQ(clean.sender.retransmits, 0U);
    BENTO_CHECK_EQ(clean.receiver.fec_frames_recovered, 0U);
}

}  // namespace

int main() {
    TestGf256();
    TestXorCodec();
    TestReedSolomonCodec();
    TestFecFrameCodec();
    TestChannelFecRecoveryUnderLoss();
    TestChannelFecNoLossNoParityLoss();
    return bento_test::Finish("test_fec_codec");
}
