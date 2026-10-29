/**
 * @file test_fast_retransmit.cpp
 * @brief 3 次重复 ACK / SACK 空洞触发快速重传，且严格早于 PTO。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/bento/net/relation.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

struct SentFrame {
    std::uint32_t seq = 0;
    std::uint8_t flags = 0;
};

struct Harness {
    Relation relation;
    std::uint64_t now = 1000;
    std::uint64_t start = 1000;
    std::vector<SentFrame> sent;
    std::unique_ptr<ReliableChannel> channel;

    explicit Harness(ReliableChannel::Config config) {
        channel = std::make_unique<ReliableChannel>(
            0, &relation, [](std::uint32_t, const std::uint8_t*, std::size_t) {}, config,
            [this]() { return now; });
        channel->SetFrameSink([this](const std::uint8_t* data, std::size_t bytes) {
            FrameView view;
            BENTO_CHECK(ParseFrame(data, bytes, &view));
            sent.push_back(SentFrame{view.header.seq, view.header.flags});
            return true;
        });
    }

    void SendMessages(int count) {
        for (int i = 0; i < count; ++i) {
            const std::uint8_t payload[8] = {static_cast<std::uint8_t>(i), 1, 2, 3, 4, 5, 6, 7};
            BENTO_CHECK(channel->Send(payload, sizeof(payload)));
        }
    }

    void Ack(std::uint32_t ack_seq, std::uint32_t bits) {
        AckState ack;
        ack.ack_seq = ack_seq;
        ack.ack_bits = bits;
        channel->OnAck(ack);
    }

    std::size_t RetransmitCount() const {
        std::size_t count = 0;
        for (const SentFrame& frame : sent) {
            if ((frame.flags & kFlagRetransmit) != 0) {
                ++count;
            }
        }
        return count;
    }
};

std::uint32_t LastRetransmitSeq(const Harness& harness) {
    std::uint32_t seq = 0;
    for (const SentFrame& frame : harness.sent) {
        if ((frame.flags & kFlagRetransmit) != 0) {
            seq = frame.seq;
        }
    }
    return seq;
}

void TestDuplicateAckThreshold() {
    ReliableChannel::Config config;
    Harness harness(config);
    harness.SendMessages(4);
    BENTO_CHECK_EQ(harness.sent.size(), 4U);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 0U);

    // 第一个 ACK 建立基准；随后 3 个「不推进累积确认、无新 SACK」的 ACK
    // 才是重复 ACK。第 2、3 个不够触发，第 4 个（累计 3 次重复）触发。
    harness.now = 1050;
    harness.Ack(1, 0);
    harness.now = 1060;
    harness.Ack(1, 0);
    harness.now = 1070;
    harness.Ack(1, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 0U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 0U);

    harness.now = 1080;
    harness.Ack(1, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 1U);
    BENTO_CHECK_EQ(LastRetransmitSeq(harness), 1U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 1U);
    BENTO_CHECK_EQ(harness.channel->stats().retransmits, 1U);
    // 快速路径必须早于 PTO（初始 1022ms），这里只过了 80ms。
    BENTO_CHECK(harness.now - harness.start < harness.channel->base_pto_ms());

    // 恢复期内再次重复 ACK 不重复重传（避免重传风暴）。
    harness.now = 1090;
    harness.Ack(1, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 1U);

    // 累积确认前进后恢复，下一轮 3 个重复 ACK 触发 seq=2。
    harness.now = 1200;
    harness.Ack(2, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 1U);
    harness.now = 1210;
    harness.Ack(2, 0);
    harness.now = 1220;
    harness.Ack(2, 0);
    harness.now = 1230;
    harness.Ack(2, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 2U);
    BENTO_CHECK_EQ(LastRetransmitSeq(harness), 2U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 2U);
}

void TestTwoDuplicatesNotEnough() {
    ReliableChannel::Config config;
    Harness harness(config);
    harness.SendMessages(3);
    harness.Ack(1, 0);
    harness.Ack(1, 0);
    harness.Ack(1, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 0U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 0U);
}

void TestSackGapTriggersImmediately() {
    ReliableChannel::Config config;
    Harness harness(config);
    harness.SendMessages(4);
    // ack_seq=1 表示 seq1 缺失；bit1..3 表示 2/3/4 已收到 -> SACK 空洞。
    harness.now = 1050;
    harness.Ack(1, 0x0000000EU);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 1U);
    BENTO_CHECK_EQ(LastRetransmitSeq(harness), 1U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 1U);
    BENTO_CHECK(harness.now - harness.start < harness.channel->base_pto_ms());
}

void TestSackMultipleHoles() {
    ReliableChannel::Config config;
    Harness harness(config);
    harness.SendMessages(6);
    // ack_seq=1；SACK 只确认 2/4/6（bit1/3/5），1/3/5 是三个已知空洞。
    // 一次 ACK 应并行补发三条，而不是等三个 PTO。
    harness.now = 1050;
    harness.Ack(1, 0x0000002AU);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 3U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 3U);

    bool saw_one = false;
    bool saw_three = false;
    bool saw_five = false;
    for (const SentFrame& frame : harness.sent) {
        if ((frame.flags & kFlagRetransmit) == 0) {
            continue;
        }
        saw_one = saw_one || frame.seq == 1U;
        saw_three = saw_three || frame.seq == 3U;
        saw_five = saw_five || frame.seq == 5U;
    }
    BENTO_CHECK(saw_one);
    BENTO_CHECK(saw_three);
    BENTO_CHECK(saw_five);
}

void TestFastRetransmitDisabled() {
    ReliableChannel::Config config;
    config.fast_retransmit = false;
    Harness harness(config);
    harness.SendMessages(3);
    harness.Ack(1, 0);
    harness.Ack(1, 0);
    harness.Ack(1, 0);
    harness.Ack(1, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 0U);
    BENTO_CHECK_EQ(harness.channel->stats().retransmits, 0U);
}


// ---------------------------------------------------------------------------
// SACK 多块扩展：协议编解码、兼容性与通道接入
// ---------------------------------------------------------------------------

void StoreU16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

void StoreU32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

bool SameBlocks(const AckState& lhs, const AckState& rhs) {
    if (lhs.sack_block_count != rhs.sack_block_count) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.sack_block_count; ++i) {
        if (lhs.sack_blocks[i].start_offset != rhs.sack_blocks[i].start_offset ||
            lhs.sack_blocks[i].length != rhs.sack_blocks[i].length) {
            return false;
        }
    }
    return true;
}

std::uint32_t NextRandom(std::uint32_t* state) {
    *state = (*state) * 1664525U + 1013904223U;
    return *state;
}

void TestSackExtCodecRoundTrip() {
    AckState ack;
    ack.ack_seq = 0x12345678U;
    ack.ack_bits = 0xDEADBEEFU;
    ack.sack_block_count = 3;
    ack.sack_blocks[0] = SackBlock{32, 8};
    ack.sack_blocks[1] = SackBlock{64, 1};
    ack.sack_blocks[2] = SackBlock{300, 200};
    std::uint8_t buffer[kAckPayloadMaxBytes] = {};
    const std::size_t expected = kAckExtHeaderBytes + 3 * kSackBlockBytes;
    BENTO_CHECK_EQ(AckPayloadBytes(ack), expected);
    BENTO_CHECK_EQ(EncodeAckPayload(ack, buffer, sizeof(buffer)), expected);
    BENTO_CHECK_EQ(static_cast<unsigned>(buffer[8]), 3U);
    AckState decoded;
    BENTO_CHECK(DecodeAckPayload(buffer, expected, &decoded));
    BENTO_CHECK_EQ(decoded.ack_seq, ack.ack_seq);
    BENTO_CHECK_EQ(decoded.ack_bits, ack.ack_bits);
    BENTO_CHECK(SameBlocks(ack, decoded));

    // 8 块满载：载荷 41B，仍是旧端可忽略的纯 ACK 尾部。
    AckState full;
    full.ack_seq = 1;
    full.sack_block_count = static_cast<std::uint8_t>(kMaxSackBlocks);
    for (std::size_t i = 0; i < kMaxSackBlocks; ++i) {
        full.sack_blocks[i].start_offset = static_cast<std::uint16_t>(40 + i * 8);
        full.sack_blocks[i].length = 4;
    }
    std::uint8_t full_buffer[kAckPayloadMaxBytes] = {};
    BENTO_CHECK_EQ(EncodeAckPayload(full, full_buffer, sizeof(full_buffer)), kAckPayloadMaxBytes);
    AckState full_decoded;
    BENTO_CHECK(DecodeAckPayload(full_buffer, kAckPayloadMaxBytes, &full_decoded));
    BENTO_CHECK(SameBlocks(full, full_decoded));

    // 无块 / 容量不足 / 越界块数：降级为 8B 旧格式，绝不产生半截扩展。
    AckState plain;
    plain.ack_seq = 9;
    plain.ack_bits = 1;
    std::uint8_t small[kAckPayloadMaxBytes] = {};
    BENTO_CHECK_EQ(EncodeAckPayload(plain, small, sizeof(small)), kAckBytes);
    BENTO_CHECK_EQ(EncodeAckPayload(ack, small, kAckBytes), kAckBytes);
    BENTO_CHECK_EQ(EncodeAckPayload(ack, small, kAckBytes - 1), 0U);
    BENTO_CHECK_EQ(AckPayloadBytes(plain), kAckBytes);
    AckState too_many = ack;
    too_many.sack_block_count = static_cast<std::uint8_t>(kMaxSackBlocks + 1);
    BENTO_CHECK_EQ(AckPayloadBytes(too_many), kAckBytes);
}

void TestSackExtValidation() {
    BENTO_CHECK(IsValidAckPayloadSize(kAckBytes));
    BENTO_CHECK(!IsValidAckPayloadSize(kAckBytes - 1));
    BENTO_CHECK(!IsValidAckPayloadSize(kAckExtHeaderBytes));                   // 9B = 0 块
    BENTO_CHECK(!IsValidAckPayloadSize(kAckExtHeaderBytes + 2));               // 不对齐
    BENTO_CHECK(IsValidAckPayloadSize(kAckExtHeaderBytes + kSackBlockBytes));  // 13B = 1 块
    BENTO_CHECK(IsValidAckPayloadSize(kAckPayloadMaxBytes));
    BENTO_CHECK(!IsValidAckPayloadSize(kAckPayloadMaxBytes + kSackBlockBytes));

    // 发送端生成非法块时降级 8B：宁可少报，也不能让对端把整帧丢弃。
    AckState ack;
    ack.ack_seq = 1;
    ack.sack_block_count = 1;
    std::uint8_t buffer[kAckPayloadMaxBytes] = {};
    ack.sack_blocks[0] = SackBlock{0, 4};
    BENTO_CHECK_EQ(EncodeAckPayload(ack, buffer, sizeof(buffer)), kAckBytes);
    ack.sack_blocks[0] = SackBlock{4, 0};
    BENTO_CHECK_EQ(EncodeAckPayload(ack, buffer, sizeof(buffer)), kAckBytes);
    ack.sack_block_count = 2;
    ack.sack_blocks[0] = SackBlock{40, 8};
    ack.sack_blocks[1] = SackBlock{44, 8};  // 重叠
    BENTO_CHECK_EQ(EncodeAckPayload(ack, buffer, sizeof(buffer)), kAckBytes);
    ack.sack_blocks[0] = SackBlock{8, 4};
    ack.sack_blocks[1] = SackBlock{4, 4};  // 乱序
    BENTO_CHECK_EQ(EncodeAckPayload(ack, buffer, sizeof(buffer)), kAckBytes);
    ack.sack_block_count = 1;
    ack.sack_blocks[0] = SackBlock{0xFFFFU, 2};  // start+len 越过 u16 语义边界
    BENTO_CHECK_EQ(EncodeAckPayload(ack, buffer, sizeof(buffer)), kAckBytes);

    // 解码端拒绝一切非法扩展，避免把坏块当成「已收到」错误确认。
    const auto make_payload = [](const SackBlock* blocks, std::size_t count, std::uint8_t* out) {
        StoreU32(out, 5);
        StoreU32(out + 4, 0);
        out[8] = static_cast<std::uint8_t>(count);
        for (std::size_t i = 0; i < count; ++i) {
            StoreU16(out + kAckExtHeaderBytes + i * kSackBlockBytes, blocks[i].start_offset);
            StoreU16(out + kAckExtHeaderBytes + i * kSackBlockBytes + 2, blocks[i].length);
        }
        return kAckExtHeaderBytes + count * kSackBlockBytes;
    };
    std::uint8_t payload[kAckPayloadMaxBytes] = {};
    AckState decoded;
    const SackBlock zero_start[1] = {SackBlock{0, 4}};
    BENTO_CHECK(!DecodeAckPayload(payload, make_payload(zero_start, 1, payload), &decoded));
    const SackBlock zero_length[1] = {SackBlock{4, 0}};
    BENTO_CHECK(!DecodeAckPayload(payload, make_payload(zero_length, 1, payload), &decoded));
    const SackBlock swapped[2] = {SackBlock{8, 4}, SackBlock{4, 4}};
    BENTO_CHECK(!DecodeAckPayload(payload, make_payload(swapped, 2, payload), &decoded));
    const SackBlock overlap[2] = {SackBlock{40, 8}, SackBlock{44, 8}};
    BENTO_CHECK(!DecodeAckPayload(payload, make_payload(overlap, 2, payload), &decoded));
    const SackBlock valid[1] = {SackBlock{4, 4}};
    const std::size_t one_block = make_payload(valid, 1, payload);
    BENTO_CHECK(DecodeAckPayload(payload, one_block, &decoded));
    // 长度与块数不符：声明 2 块但只给 1 块字节。
    payload[8] = 2;
    BENTO_CHECK(!DecodeAckPayload(payload, one_block, &decoded));
    BENTO_CHECK(!DecodeAckPayload(nullptr, one_block, &decoded));
    BENTO_CHECK(!DecodeAckPayload(payload, one_block, nullptr));
}

void TestSackExtFrameParse() {
    AckState ack;
    ack.ack_seq = 7;
    ack.ack_bits = 0x2;
    ack.sack_block_count = 1;
    ack.sack_blocks[0] = SackBlock{40, 4};
    std::uint8_t payload[kAckPayloadMaxBytes] = {};
    const std::size_t bytes = EncodeAckPayload(ack, payload, sizeof(payload));
    FrameHeader header;
    header.type = FrameType::kAck;
    header.channel = 0;
    const std::vector<std::uint8_t> frame = BuildFrame(header, nullptr, payload, bytes);
    FrameView view;
    BENTO_CHECK(ParseFrame(frame.data(), frame.size(), &view));
    BENTO_CHECK_EQ(static_cast<unsigned>(view.header.type), static_cast<unsigned>(FrameType::kAck));
    BENTO_CHECK_EQ(view.ack.ack_seq, 7U);
    BENTO_CHECK_EQ(static_cast<std::size_t>(view.ack.sack_block_count), 1U);
    BENTO_CHECK_EQ(view.ack.sack_blocks[0].start_offset, 40U);
    BENTO_CHECK_EQ(view.ack.sack_blocks[0].length, 4U);

    // 旧 8B 纯 ACK 仍可解析（旧端与降级路径）。
    const std::vector<std::uint8_t> legacy_frame =
        BuildFrame(header, nullptr, payload, kAckBytes);
    FrameView legacy_view;
    BENTO_CHECK(ParseFrame(legacy_frame.data(), legacy_frame.size(), &legacy_view));
    BENTO_CHECK_EQ(static_cast<std::size_t>(legacy_view.ack.sack_block_count), 0U);
    BENTO_CHECK_EQ(legacy_view.ack.ack_seq, 7U);

    // 非法扩展（声明 9 块）在帧解析层被拒绝，计入 invalid_frames。
    std::uint8_t bad[kAckPayloadMaxBytes] = {};
    StoreU32(bad, 1);
    StoreU32(bad + 4, 0);
    bad[8] = static_cast<std::uint8_t>(kMaxSackBlocks + 1);
    const std::vector<std::uint8_t> bad_frame =
        BuildFrame(header, nullptr, bad, kAckPayloadMaxBytes);
    FrameView bad_view;
    BENTO_CHECK(!ParseFrame(bad_frame.data(), bad_frame.size(), &bad_view));
}

void TestSackExtRandomRoundTrip() {
    // 固定 LCG：1000 组随机 (ack_seq, bits, 块数, 不重叠块) 编解码往返，
    // 并逐字节核对前 8B 与旧编码一致（线格式兼容的唯一硬约束）。
    std::uint32_t state = 0x5EED1234U;
    std::uint8_t buffer[kAckPayloadMaxBytes] = {};
    for (int iteration = 0; iteration < 1000; ++iteration) {
        AckState ack;
        ack.ack_seq = NextRandom(&state) & 0x7FFFFFFFU;
        if (ack.ack_seq == 0) {
            ack.ack_seq = 1;
        }
        ack.ack_bits = NextRandom(&state);
        const std::size_t count =
            static_cast<std::size_t>(NextRandom(&state) % (kMaxSackBlocks + 1));
        std::uint32_t cursor = 1 + NextRandom(&state) % 16;
        for (std::size_t i = 0; i < count; ++i) {
            cursor += NextRandom(&state) % 24;
            const std::uint32_t length = 1 + NextRandom(&state) % 128;
            ack.sack_blocks[i].start_offset = static_cast<std::uint16_t>(cursor);
            ack.sack_blocks[i].length = static_cast<std::uint16_t>(length);
            cursor += length;
        }
        ack.sack_block_count = static_cast<std::uint8_t>(count);
        const std::size_t bytes = EncodeAckPayload(ack, buffer, sizeof(buffer));
        BENTO_CHECK_EQ(bytes, AckPayloadBytes(ack));
        AckState decoded;
        BENTO_CHECK(DecodeAckPayload(buffer, bytes, &decoded));
        BENTO_CHECK_EQ(decoded.ack_seq, ack.ack_seq);
        BENTO_CHECK_EQ(decoded.ack_bits, ack.ack_bits);
        BENTO_CHECK(SameBlocks(ack, decoded));
        std::uint8_t legacy[kAckBytes] = {};
        EncodeAck(ack, legacy);
        for (std::size_t i = 0; i < kAckBytes; ++i) {
            BENTO_CHECK_EQ(buffer[i], legacy[i]);
        }
    }
}

std::unique_ptr<ReliableChannel> MakeReceiver(Relation* relation, std::uint64_t* now) {
    ReliableChannel::Config config;
    return std::make_unique<ReliableChannel>(
        0, relation, [](std::uint32_t, const std::uint8_t*, std::size_t) {}, config,
        [now]() { return *now; });
}

void DeliverStreamFrame(ReliableChannel* receiver, std::uint32_t seq) {
    const std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = 0;
    header.seq = seq;
    const std::vector<std::uint8_t> frame = BuildFrame(header, nullptr, payload, sizeof(payload));
    FrameView view;
    BENTO_CHECK(ParseFrame(frame.data(), frame.size(), &view));
    receiver->OnFrame(view.header, view.payload, view.payload_bytes, nullptr);
}

void TestFillAckSackBlocks() {
    Relation relation;
    std::uint64_t now = 1000;
    auto receiver = MakeReceiver(&relation, &now);
    // seq1 丢失；seq2..48 到达。基础位图覆盖 2..32，扩展块负责 33..48。
    for (std::uint32_t seq = 2; seq <= 48; ++seq) {
        DeliverStreamFrame(receiver.get(), seq);
    }
    AckState ack;
    BENTO_CHECK(receiver->FillAck(&ack));
    BENTO_CHECK_EQ(ack.ack_seq, 1U);
    BENTO_CHECK_EQ(ack.ack_bits, 0xFFFFFFFEU);
    BENTO_CHECK_EQ(static_cast<std::size_t>(ack.sack_block_count), 1U);
    BENTO_CHECK_EQ(ack.sack_blocks[0].start_offset, 32U);
    BENTO_CHECK_EQ(ack.sack_blocks[0].length, 16U);

    std::uint8_t wire[kAckPayloadMaxBytes] = {};
    const std::size_t wire_bytes = EncodeAckPayload(ack, wire, sizeof(wire));
    BENTO_CHECK_EQ(wire_bytes, kAckExtHeaderBytes + kSackBlockBytes);
    AckState decoded;
    BENTO_CHECK(DecodeAckPayload(wire, wire_bytes, &decoded));
    BENTO_CHECK(SameBlocks(ack, decoded));
}

void TestFillAckBlockSelection() {
    Relation relation;
    std::uint64_t now = 1000;
    auto receiver = MakeReceiver(&relation, &now);
    // 只收到偶数 seq（1 与所有奇数丢失），位图之外有 24 个单包 run。
    for (std::uint32_t seq = 2; seq <= 80; seq += 2) {
        DeliverStreamFrame(receiver.get(), seq);
    }
    AckState ack;
    BENTO_CHECK(receiver->FillAck(&ack));
    BENTO_CHECK_EQ(ack.ack_seq, 1U);
    BENTO_CHECK_EQ(static_cast<std::size_t>(ack.sack_block_count), kMaxSackBlocks);
    // 块必须升序、不重叠（解析端按此校验，否则整帧被拒）。
    std::uint32_t previous_end = 0;
    for (std::size_t i = 0; i < ack.sack_block_count; ++i) {
        const std::uint32_t start = ack.sack_blocks[i].start_offset;
        const std::uint32_t length = ack.sack_blocks[i].length;
        BENTO_CHECK(start >= previous_end);
        BENTO_CHECK(length > 0);
        previous_end = start + length;
    }
    // 近端 15 块 + 最远 1 块：既让发送端判定 HOL 空洞，也把「最远已收」
    // 推进到 80，避免远端已收项被 PTO 整批重传。
    BENTO_CHECK_EQ(ack.sack_blocks[0].start_offset, 33U);   // seq34
    BENTO_CHECK_EQ(ack.sack_blocks[14].start_offset, 61U);  // seq62
    BENTO_CHECK_EQ(ack.sack_blocks[15].start_offset, 79U);  // seq80
    BENTO_CHECK_EQ(ack.sack_blocks[15].length, 1U);
}

void TestExtendedSackRemovesInflight() {
    ReliableChannel::Config config;
    // 对照：只有累积确认（无 SACK 块）时，40 条全部留在 inflight。
    Harness plain(config);
    plain.SendMessages(40);
    AckState plain_ack;
    plain_ack.ack_seq = 1;
    plain_ack.ack_bits = 0;
    plain.channel->OnAck(plain_ack);
    BENTO_CHECK_EQ(plain.channel->stats().inflight, 40U);

    // 扩展块覆盖 seq33..40：这 8 条立即移出 inflight，不再参与 PTO。
    Harness ext(config);
    ext.SendMessages(40);
    AckState ack;
    ack.ack_seq = 1;
    ack.ack_bits = 0;
    ack.sack_block_count = 1;
    ack.sack_blocks[0] = SackBlock{32, 8};
    ext.now = 1050;
    ext.channel->OnAck(ack);
    BENTO_CHECK_EQ(ext.channel->stats().inflight, 32U);
    BENTO_CHECK_EQ(ext.channel->stats().sack_acks_with_blocks, 1U);
    BENTO_CHECK_EQ(ext.channel->stats().sack_seqs_covered, 8U);

    // 推进到 PTO：兜底集合已不含 33..40，重传只发生在已知空洞 1..32。
    ext.now += 2000;
    ext.channel->OnTimer(ext.now);
    BENTO_CHECK(ext.channel->stats().pto_retransmits > 0U);
    for (const SentFrame& frame : ext.sent) {
        if ((frame.flags & kFlagRetransmit) != 0) {
            BENTO_CHECK(frame.seq <= 32U);
        }
    }
}

}  // namespace

int main() {
    TestDuplicateAckThreshold();
    TestTwoDuplicatesNotEnough();
    TestSackGapTriggersImmediately();
    TestSackMultipleHoles();
    TestFastRetransmitDisabled();
    TestSackExtCodecRoundTrip();
    TestSackExtValidation();
    TestSackExtFrameParse();
    TestSackExtRandomRoundTrip();
    TestFillAckSackBlocks();
    TestFillAckBlockSelection();
    TestExtendedSackRemovesInflight();
    return bento_test::Finish("test_fast_retransmit");
}
