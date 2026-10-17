/**
 * @file test_quic_packet.cpp
 * @brief QUIC 地基测试：varint（RFC 9000 §16）+ 包头（§17）。
 *
 * ## 覆盖
 *
 * varint：
 * - 4 个长度档的下边界与上边界精确往返（63/64、16383/16384、2^30-1/2^30、2^62-1）
 * - 编码长度选择、超上限拒绝、截断输入返回 kNeedMore
 * - **非最短编码可解码**（RFC 不要求发送方用最短，接收方必须宽容）
 * - 随机往返 200,000 次
 *
 * 包头：
 * - Initial / Handshake / 0-RTT / Retry / Version Negotiation / 1-RTT 六种形态
 * - DCID/SCID 边界（0 字节、20 字节上限）
 * - 畸形输入（fixed bit 为 0、DCID 超长、非法版本列表长度）
 * - 截断输入在任意前缀都不得崩溃
 * - 随机 fuzz：10 万次位翻转/截断，不得崩溃
 *
 * ## 明确不测的
 *
 * **packet number 的真实值与长度** —— 它被 header protection 加密（RFC 9001 §5.4），
 * 没有密钥就无法得到。本测试只验证 `packet_number_offset` 指向正确位置。
 */
#include "go2cpp/network/quic/packet.hpp"
#include "go2cpp/network/quic/varint.hpp"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::net::quic;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[quic] FAILED: %s\n", what);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// varint
// ---------------------------------------------------------------------------
void RoundTripVarInt(std::uint64_t value, const char* label) {
    std::uint8_t buffer[8] = {};
    const auto enc = EncodeVarInt(value, buffer);
    if (enc.status != VarIntStatus::kOk) {
        std::fprintf(stderr, "[quic] encode failed: %s value=%llu\n", label,
                     static_cast<unsigned long long>(value));
        ++g_failures;
        return;
    }
    const auto dec = DecodeVarInt(buffer, enc.size);
    if (dec.status != VarIntStatus::kOk || dec.value != value || dec.consumed != enc.size) {
        std::fprintf(stderr, "[quic] varint round-trip mismatch: %s value=%llu\n", label,
                     static_cast<unsigned long long>(value));
        ++g_failures;
    }
    if (enc.size != VarIntEncodedSize(value)) {
        std::fprintf(stderr, "[quic] size prediction wrong: %s\n", label);
        ++g_failures;
    }
}

void ScenarioVarInt() {
    std::printf("[quic][场景1] varint 边界\n");
    // 1 字节档
    RoundTripVarInt(0, "min");
    RoundTripVarInt(63, "max-1byte");
    // 2 字节档
    RoundTripVarInt(64, "min-2byte");
    RoundTripVarInt(16383, "max-2byte");
    // 4 字节档
    RoundTripVarInt(16384, "min-4byte");
    RoundTripVarInt(1073741823ULL, "max-4byte");
    // 8 字节档
    RoundTripVarInt(1073741824ULL, "min-8byte");
    RoundTripVarInt(kVarIntMax, "max-8byte");

    // 编码长度选择
    Check(VarIntEncodedSize(0) == 1, "size(0)");
    Check(VarIntEncodedSize(63) == 1, "size(63)");
    Check(VarIntEncodedSize(64) == 2, "size(64)");
    Check(VarIntEncodedSize(16383) == 2, "size(16383)");
    Check(VarIntEncodedSize(16384) == 4, "size(16384)");
    Check(VarIntEncodedSize(1073741823ULL) == 4, "size(2^30-1)");
    Check(VarIntEncodedSize(1073741824ULL) == 8, "size(2^30)");
    Check(VarIntEncodedSize(kVarIntMax) == 8, "size(max)");

    // 超上限拒绝
    std::uint8_t buffer[8] = {};
    Check(EncodeVarInt(kVarIntMax + 1, buffer).status == VarIntStatus::kTooLarge,
          "max+1 rejected");
    Check(EncodeVarInt(~0ULL, buffer).status == VarIntStatus::kTooLarge, "UINT64_MAX rejected");

    // 截断
    Check(EncodeVarInt(1000000ULL, buffer).size == 4, "1000000 uses 4 bytes");
    for (std::size_t avail = 0; avail < 4; ++avail) {
        const auto dec = DecodeVarInt(buffer, avail);
        Check(dec.status == VarIntStatus::kNeedMore, "truncated -> kNeedMore");
        Check(dec.consumed == 0, "truncated consumes 0");
    }

    // 非最短编码：值 5 用 4 字节表示
    const std::uint8_t wide[4] = {0x80, 0x00, 0x00, 0x05};
    const auto wide_dec = DecodeVarInt(wide, 4);
    Check(wide_dec.status == VarIntStatus::kOk && wide_dec.value == 5,
          "non-minimal encoding decodes");
    Check(wide_dec.consumed == 4, "non-minimal consumed");
}

void ScenarioVarIntStress() {
    std::printf("[quic][场景2] varint 随机往返 200000 次\n");
    std::mt19937_64 rng(20261017);
    std::uniform_int_distribution<std::uint64_t> dist(0, kVarIntMax);
    std::uint8_t buffer[8] = {};
    constexpr int kRounds = 200000;
    for (int i = 0; i < kRounds; ++i) {
        const auto value = dist(rng);
        const auto enc = EncodeVarInt(value, buffer);
        if (enc.status != VarIntStatus::kOk) {
            Check(false, "random encode failed");
            break;
        }
        const auto dec = DecodeVarInt(buffer, enc.size);
        if (dec.status != VarIntStatus::kOk || dec.value != value) {
            std::fprintf(stderr, "[quic] random varint mismatch at i=%d\n", i);
            Check(false, "random varint mismatch");
            break;
        }
    }
    std::printf("[quic] varint 随机往返 %d 次完成\n", kRounds);
}

// ---------------------------------------------------------------------------
// 包头
// ---------------------------------------------------------------------------
const std::vector<std::uint8_t> kDcid = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
const std::vector<std::uint8_t> kScid = {0xAA, 0xBB, 0xCC, 0xDD};

void ScenarioInitial() {
    std::printf("[quic][场景3] Initial 包（含 token）\n");
    LongHeaderSpec spec;
    spec.type = PacketType::kInitial;
    spec.dcid = kDcid;
    spec.scid = kScid;
    spec.token = {0xDE, 0xAD, 0xBE, 0xEF};
    spec.payload_length = 1200;
    const auto bytes = EncodeLongHeader(spec);
    Check(!bytes.empty(), "encode initial failed");

    const auto decoded = DecodePacketHeader(bytes.data(), bytes.size());
    Check(decoded.status == HeaderStatus::kOk, "decode initial failed");
    Check(decoded.header.type == PacketType::kInitial, "initial type");
    Check(decoded.header.version == kVersion1, "initial version");
    Check(decoded.header.dcid == kDcid, "initial dcid");
    Check(decoded.header.scid == kScid, "initial scid");
    Check(decoded.header.token == spec.token, "initial token");
    Check(decoded.header.payload_length == 1200, "initial length");
    Check(decoded.consumed == bytes.size(), "initial consumed == full header");
    Check(decoded.header.packet_number_offset == bytes.size(),
          "initial pn offset at end of header");
}

void ScenarioHandshakeAndZeroRtt() {
    std::printf("[quic][场景4] Handshake / 0-RTT / Retry\n");
    // Handshake
    {
        LongHeaderSpec spec;
        spec.type = PacketType::kHandshake;
        spec.dcid = kDcid;
        spec.scid = kScid;
        spec.payload_length = 64;
        const auto bytes = EncodeLongHeader(spec);
        const auto decoded = DecodePacketHeader(bytes.data(), bytes.size());
        Check(decoded.status == HeaderStatus::kOk, "handshake decode");
        Check(decoded.header.type == PacketType::kHandshake, "handshake type");
        Check(decoded.header.token.empty(), "handshake has no token");
        Check(decoded.header.payload_length == 64, "handshake length");
    }
    // 0-RTT
    {
        LongHeaderSpec spec;
        spec.type = PacketType::kZeroRtt;
        spec.dcid = kDcid;
        spec.scid = kScid;
        spec.payload_length = 300;
        const auto bytes = EncodeLongHeader(spec);
        const auto decoded = DecodePacketHeader(bytes.data(), bytes.size());
        Check(decoded.status == HeaderStatus::kOk, "0-rtt decode");
        Check(decoded.header.type == PacketType::kZeroRtt, "0-rtt type");
    }
    // Retry（token + 16 字节完整性标签）
    {
        LongHeaderSpec spec;
        spec.type = PacketType::kRetry;
        spec.dcid = kDcid;
        spec.scid = kScid;
        spec.token = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        spec.retry_integrity_tag.assign(16, 0x5A);
        const auto bytes = EncodeLongHeader(spec);
        const auto decoded = DecodePacketHeader(bytes.data(), bytes.size());
        Check(decoded.status == HeaderStatus::kOk, "retry decode");
        Check(decoded.header.type == PacketType::kRetry, "retry type");
        Check(decoded.header.token == spec.token, "retry token");
        Check(decoded.header.retry_integrity_tag.size() == kRetryIntegrityTagLength,
              "retry integrity tag length");
        Check(decoded.consumed == bytes.size(), "retry consumed");
    }
}

void ScenarioVersionNegotiation() {
    std::printf("[quic][场景5] Version Negotiation\n");
    const std::vector<std::uint32_t> versions = {0x00000001, 0xFF00001D, 0xFF00001C};
    const auto bytes = EncodeVersionNegotiation(kDcid, kScid, versions);
    Check(!bytes.empty(), "encode vn failed");

    const auto decoded = DecodePacketHeader(bytes.data(), bytes.size());
    Check(decoded.status == HeaderStatus::kOk, "vn decode");
    Check(decoded.header.type == PacketType::kVersionNegotiation, "vn type");
    Check(decoded.header.version == kVersionNegotiation, "vn version is 0");
    Check(decoded.header.supported_versions == versions, "vn version list");

    // 非 4 倍数长度的版本列表 -> 非法
    auto bad = bytes;
    bad.push_back(0xFF);
    const auto bad_decoded = DecodePacketHeader(bad.data(), bad.size());
    Check(bad_decoded.status == HeaderStatus::kInvalid,
          "vn with non-multiple-of-4 trailing bytes is invalid");
}

void ScenarioShortHeader() {
    std::printf("[quic][场景6] Short header（1-RTT）\n");
    for (std::size_t dcid_len : {std::size_t{0}, std::size_t{4}, std::size_t{8},
                                 std::size_t{20}}) {
        std::vector<std::uint8_t> dcid(dcid_len);
        for (std::size_t i = 0; i < dcid_len; ++i) {
            dcid[i] = static_cast<std::uint8_t>(i + 1);
        }
        const auto bytes = EncodeShortHeader(dcid, 0x1234, 2);
        Check(!bytes.empty(), "encode short failed");
        // 注意：解析 short header 必须由调用方提供 DCID 长度
        const auto decoded = DecodePacketHeader(bytes.data(), bytes.size(), dcid_len);
        Check(decoded.status == HeaderStatus::kOk, "short decode");
        Check(!decoded.header.is_long, "short is_long false");
        Check(decoded.header.type == PacketType::kOneRtt, "short type");
        Check(decoded.header.dcid == dcid, "short dcid");
        Check(decoded.header.packet_number_offset == 1 + dcid_len, "short pn offset");
    }
    // dcid_len == 0 **合法**（RFC 9000 §17.3 只规定"短包头不含长度字段"，
    // 长度由连接状态给出，没有下界；实践中罕见但协议允许）。
    // 只有超过 20 字节上限才非法。
    const auto bytes = EncodeShortHeader(kDcid, 1, 1);
    Check(DecodePacketHeader(bytes.data(), bytes.size(), 0).status == HeaderStatus::kOk,
          "short with dcid_len 0 is legal");
    Check(DecodePacketHeader(bytes.data(), bytes.size(), 21).status == HeaderStatus::kInvalid,
          "short with dcid_len 21 is illegal");
}

void ScenarioMalformed() {
    std::printf("[quic][场景7] 畸形输入与截断\n");
    // fixed bit 为 0 的 long header -> 非法
    {
        LongHeaderSpec spec;
        spec.type = PacketType::kInitial;
        spec.dcid = kDcid;
        spec.scid = kScid;
        spec.payload_length = 10;
        auto bytes = EncodeLongHeader(spec);
        bytes[0] = static_cast<std::uint8_t>(bytes[0] & static_cast<std::uint8_t>(~0x40U));
        Check(DecodePacketHeader(bytes.data(), bytes.size()).status == HeaderStatus::kInvalid,
              "long with fixed bit 0 is invalid");
    }
    // DCID 长度声明 21（超上限）-> 非法
    {
        LongHeaderSpec spec;
        spec.type = PacketType::kHandshake;
        spec.dcid = kDcid;
        spec.scid = kScid;
        spec.payload_length = 10;
        auto bytes = EncodeLongHeader(spec);
        bytes[5] = 21;   // DCID length 字段
        Check(DecodePacketHeader(bytes.data(), bytes.size()).status == HeaderStatus::kInvalid,
              "dcid length 21 is invalid");
    }
    // 任意前缀都不得崩溃，且必须是 kNeedMore
    {
        LongHeaderSpec spec;
        spec.type = PacketType::kInitial;
        spec.dcid = kDcid;
        spec.scid = kScid;
        spec.token = {1, 2, 3};
        spec.payload_length = 100;
        const auto bytes = EncodeLongHeader(spec);
        for (std::size_t avail = 0; avail < bytes.size(); ++avail) {
            const auto decoded = DecodePacketHeader(bytes.data(), avail);
            Check(decoded.status == HeaderStatus::kNeedMore,
                  "truncated long header must be kNeedMore");
        }
        // 完整后可解析
        Check(DecodePacketHeader(bytes.data(), bytes.size()).status == HeaderStatus::kOk,
              "full header decodes");
    }
    // 空输入
    Check(DecodePacketHeader(nullptr, 0).status == HeaderStatus::kNeedMore,
          "null input -> kNeedMore");
}

void ScenarioFuzz() {
    std::printf("[quic][场景8] 随机变异 fuzz\n");
    std::mt19937 rng(20261017);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(0, 80);
    constexpr int kRounds = 100000;
    int ok = 0, need_more = 0, invalid = 0, unsupported = 0;
    for (int i = 0; i < kRounds; ++i) {
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(len_dist(rng)));
        for (auto& b : buf) {
            b = static_cast<std::uint8_t>(byte_dist(rng));
        }
        const auto decoded = DecodePacketHeader(buf.data(), buf.size(), 8);
        switch (decoded.status) {
            case HeaderStatus::kOk: {
                ++ok;
                // 不变量：consumed 不得超过输入长度
                Check(decoded.consumed <= buf.size(), "consumed must not exceed input");
                // 不变量：DCID/SCID 长度不得超过上限
                Check(decoded.header.dcid.size() <= kMaxConnectionIdLength,
                      "dcid within limit");
                Check(decoded.header.scid.size() <= kMaxConnectionIdLength,
                      "scid within limit");
                break;
            }
            case HeaderStatus::kNeedMore: ++need_more; break;
            case HeaderStatus::kInvalid: ++invalid; break;
            case HeaderStatus::kUnsupportedVersion: ++unsupported; break;
        }
    }
    std::printf("[quic] fuzz %d 次: ok=%d need_more=%d invalid=%d unsupported=%d\n",
                kRounds, ok, need_more, invalid, unsupported);
    Check(ok + need_more + invalid + unsupported == kRounds, "fuzz outcome accounting");
}

void ScenarioHeaderRoundTripStress() {
    std::printf("[quic][场景9] 包头随机往返 20000 次\n");
    std::mt19937 rng(20261017);
    std::uniform_int_distribution<int> cid_len(0, 20);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> type_dist(0, 3);
    constexpr int kRounds = 20000;
    const PacketType types[] = {PacketType::kInitial, PacketType::kZeroRtt,
                                PacketType::kHandshake, PacketType::kRetry};
    for (int i = 0; i < kRounds; ++i) {
        LongHeaderSpec spec;
        spec.type = types[type_dist(rng)];
        spec.dcid.resize(static_cast<std::size_t>(cid_len(rng)));
        spec.scid.resize(static_cast<std::size_t>(cid_len(rng)));
        for (auto& b : spec.dcid) b = static_cast<std::uint8_t>(byte_dist(rng));
        for (auto& b : spec.scid) b = static_cast<std::uint8_t>(byte_dist(rng));
        if (spec.type == PacketType::kInitial) {
            spec.token.resize(static_cast<std::size_t>(cid_len(rng)));
            for (auto& b : spec.token) b = static_cast<std::uint8_t>(byte_dist(rng));
        }
        if (spec.type == PacketType::kRetry) {
            spec.token.resize(static_cast<std::size_t>(cid_len(rng)) + 1);
            for (auto& b : spec.token) b = static_cast<std::uint8_t>(byte_dist(rng));
            spec.retry_integrity_tag.assign(16, 0x11);
        } else {
            spec.payload_length = static_cast<std::uint64_t>(i % 100000);
        }
        const auto bytes = EncodeLongHeader(spec);
        if (bytes.empty()) {
            Check(false, "stress encode failed");
            break;
        }
        const auto decoded = DecodePacketHeader(bytes.data(), bytes.size());
        if (decoded.status != HeaderStatus::kOk) {
            std::fprintf(stderr, "[quic] stress decode failed at %d type=%s\n", i,
                         ToString(spec.type));
            Check(false, "stress decode failed");
            break;
        }
        if (decoded.header.type != spec.type || decoded.header.dcid != spec.dcid ||
            decoded.header.scid != spec.scid) {
            Check(false, "stress field mismatch");
            break;
        }
        if (spec.type != PacketType::kRetry && decoded.header.token != spec.token) {
            Check(false, "stress token mismatch");
            break;
        }
        if (spec.type != PacketType::kRetry &&
            decoded.header.payload_length != spec.payload_length) {
            Check(false, "stress length mismatch");
            break;
        }
    }
    std::printf("[quic] 包头随机往返 %d 次完成\n", kRounds);
}

}  // namespace

int main() {
    std::printf("=== QUIC 地基测试（RFC 9000 §16 / §17）===\n");
    ScenarioVarInt();
    ScenarioVarIntStress();
    ScenarioInitial();
    ScenarioHandshakeAndZeroRtt();
    ScenarioVersionNegotiation();
    ScenarioShortHeader();
    ScenarioMalformed();
    ScenarioFuzz();
    ScenarioHeaderRoundTripStress();

    if (g_failures == 0) {
        std::printf("\n[quic] failures=0\n");
    } else {
        std::printf("\n[quic] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
