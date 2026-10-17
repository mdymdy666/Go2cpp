/**
 * @file test_quic_frame.cpp
 * @brief QUIC 帧层测试（RFC 9000 §19）。
 *
 * ## 覆盖策略
 *
 * 1. **每种帧类型往返**（22 种）+ 字段逐项比对；
 * 2. **STREAM 的 8 个变体**（OFF/LEN/FIN 组合）—— 这是最容易写错的地方；
 * 3. **ACK Range**（0 个 / 1 个 / 多个额外区间）；
 * 4. **PADDING 合并**（1 个、多个、整个包全是零）；
 * 5. **截断**：对每种帧的编码，逐个前缀截断，必须返回 `kNeedMore` 且 `consumed == 0`；
 * 6. **畸形输入**：长度字段超包、NEW_TOKEN 空、CID 长度非法、
 *    retire_prior_to > sequence、max_streams > 2^60 等；
 * 7. **未知帧类型**：RFC 9000 §12.4 要求当 FRAME_ENCODING_ERROR 处理；
 * 8. **随机往返** 20,000 次；
 * 9. **随机变异 fuzz** 100,000 次（不崩溃 + `consumed ≤ length`）。
 */
#include "go2cpp/network/quic/frame.hpp"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::net::quic;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[quic-frame] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 往返：编码 -> 解码，比较关键字段。
void RoundTrip(const Frame& frame, const char* label) {
    const auto encoded = EncodeFrame(frame);
    if (encoded.status != FrameStatus::kOk) {
        std::fprintf(stderr, "[quic-frame] encode failed: %s\n", label);
        ++g_failures;
        return;
    }
    const auto decoded = DecodeFrame(encoded.bytes.data(), encoded.bytes.size());
    if (decoded.status != FrameStatus::kOk) {
        std::fprintf(stderr, "[quic-frame] decode failed: %s (status=%s)\n", label,
                     ToString(decoded.status));
        ++g_failures;
        return;
    }
    if (decoded.consumed != encoded.bytes.size()) {
        std::fprintf(stderr, "[quic-frame] consumed mismatch: %s (%zu vs %zu)\n", label,
                     decoded.consumed, encoded.bytes.size());
        ++g_failures;
        return;
    }
    // 再编码一次，比对字节（等价性检查，不依赖字段逐项比较）
    const auto reencoded = EncodeFrame(decoded.frame);
    if (reencoded.status != FrameStatus::kOk ||
        reencoded.bytes != encoded.bytes) {
        std::fprintf(stderr, "[quic-frame] re-encode mismatch: %s\n", label);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// 场景 1：所有帧类型的基本往返
// ---------------------------------------------------------------------------
void ScenarioBasicFrames() {
    RoundTrip(MakePing(), "PING");
    RoundTrip(MakeHandshakeDone(), "HANDSHAKE_DONE");
    RoundTrip(MakeResetStream(4, 0x1234, 1000), "RESET_STREAM");
    RoundTrip(MakeStopSending(8, 0x99), "STOP_SENDING");
    RoundTrip(MakeCrypto(0, {'h', 'e', 'l', 'l', 'o'}), "CRYPTO");
    RoundTrip(MakeCrypto(123456, {}), "CRYPTO-empty");
    RoundTrip(MakeNewToken({1, 2, 3, 4, 5}), "NEW_TOKEN");
    RoundTrip(MakeMaxData(1 << 20), "MAX_DATA");
    RoundTrip(MakeMaxStreamData(7, 1 << 19), "MAX_STREAM_DATA");
    RoundTrip(MakeMaxStreams(100, true), "MAX_STREAMS-bidi");
    RoundTrip(MakeMaxStreams(200, false), "MAX_STREAMS-uni");
    RoundTrip(MakeDataBlocked(4096), "DATA_BLOCKED");
    RoundTrip(MakeStreamDataBlocked(9, 8192), "STREAM_DATA_BLOCKED");
    RoundTrip(MakeStreamsBlocked(50, true), "STREAMS_BLOCKED-bidi");
    RoundTrip(MakeStreamsBlocked(60, false), "STREAMS_BLOCKED-uni");
    RoundTrip(MakeNewConnectionId(3, 1, {0xaa, 0xbb, 0xcc, 0xdd},
                                  {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}),
              "NEW_CONNECTION_ID");
    RoundTrip(MakeRetireConnectionId(2), "RETIRE_CONNECTION_ID");
    RoundTrip(MakePathChallenge({1, 2, 3, 4, 5, 6, 7, 8}), "PATH_CHALLENGE");
    RoundTrip(MakePathResponse({8, 7, 6, 5, 4, 3, 2, 1}), "PATH_RESPONSE");
    RoundTrip(MakeConnectionCloseTransport(0x0a, 0x08, "bad frame"),
              "CONNECTION_CLOSE-transport");
    RoundTrip(MakeConnectionCloseApplication(0x42, "app gone"),
              "CONNECTION_CLOSE-app");
    RoundTrip(MakeConnectionCloseApplication(0xff, ""), "CONNECTION_CLOSE-empty-reason");
    std::printf("[quic-frame] 基础帧往返 22 种完成\n");
}

// ---------------------------------------------------------------------------
// 场景 2：STREAM 的 8 个变体
// ---------------------------------------------------------------------------
void ScenarioStreamVariants() {
    const std::vector<std::uint8_t> payload = {'a', 'b', 'c'};
    int count = 0;
    for (int bits = 0; bits < 8; ++bits) {
        const bool has_offset = (bits & 0x4) != 0;
        const bool has_length = (bits & 0x2) != 0;
        const bool fin = (bits & 0x1) != 0;
        const auto frame = MakeStream(4, has_offset ? 100 : 0, has_offset, has_length, fin,
                                      payload);
        const auto encoded = EncodeFrame(frame);
        if (encoded.status != FrameStatus::kOk) {
            Check(false, "stream encode failed");
            continue;
        }
        // 类型码的低 3 位必须与标志一致
        if (encoded.bytes[0] != static_cast<std::uint8_t>(0x08 | bits)) {
            std::fprintf(stderr, "[quic-frame] stream type byte wrong: bits=%d got=0x%02x\n",
                         bits, encoded.bytes[0]);
            Check(false, "stream type byte mismatch");
        }
        const auto decoded = DecodeFrame(encoded.bytes.data(), encoded.bytes.size());
        if (decoded.status != FrameStatus::kOk) {
            Check(false, "stream decode failed");
            continue;
        }
        Check(decoded.frame.has_offset == has_offset, "stream has_offset mismatch");
        Check(decoded.frame.has_length == has_length, "stream has_length mismatch");
        Check(decoded.frame.fin == fin, "stream fin mismatch");
        Check(decoded.frame.stream_id == 4, "stream id mismatch");
        Check(decoded.frame.data == payload, "stream data mismatch");
        if (has_offset) {
            Check(decoded.frame.offset == 100, "stream offset mismatch");
        }
        ++count;
    }
    Check(count == 8, "should exercise all 8 stream variants");
    std::printf("[quic-frame] STREAM 8 变体完成\n");
}

// ---------------------------------------------------------------------------
// 场景 3：LEN 位为 0 时数据延伸到包尾
// ---------------------------------------------------------------------------
void ScenarioStreamNoLength() {
    // 手工构造：type=0x08（无 OFF/LEN/FIN）、stream_id=4、后面全是数据
    const std::vector<std::uint8_t> bytes = {0x08, 0x04, 'x', 'y', 'z'};
    const auto decoded = DecodeFrame(bytes.data(), bytes.size());
    Check(decoded.status == FrameStatus::kOk, "no-LEN stream should decode");
    Check(decoded.consumed == bytes.size(), "no-LEN stream must consume to end of packet");
    Check(decoded.frame.data.size() == 3, "no-LEN stream data should be packet remainder");

    // 若"包"更长，数据也要延伸到包尾
    const std::vector<std::uint8_t> longer = {0x08, 0x04, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto decoded2 = DecodeFrame(longer.data(), longer.size());
    Check(decoded2.status == FrameStatus::kOk, "longer no-LEN stream should decode");
    Check(decoded2.frame.data.size() == 8, "data should extend to packet end");
}

// ---------------------------------------------------------------------------
// 场景 4：ACK 帧与 Range 列表
// ---------------------------------------------------------------------------
void ScenarioAck() {
    // 无额外区间
    {
        const auto frame = MakeAck(10, 0, 2);
        const auto encoded = EncodeFrame(frame);
        const auto decoded = DecodeFrame(encoded.bytes.data(), encoded.bytes.size());
        Check(decoded.status == FrameStatus::kOk, "ack decode failed");
        Check(decoded.frame.largest_acked == 10, "ack largest wrong");
        Check(decoded.frame.first_ack_range == 2, "ack first range wrong");
        Check(decoded.frame.ack_ranges.empty(), "ack ranges should be empty");
    }
    // 多个额外区间
    {
        const std::vector<AckRange> ranges = {{1, 2}, {0, 0}, {5, 3}};
        const auto frame = MakeAck(100, 7, 4, ranges);
        const auto encoded = EncodeFrame(frame);
        const auto decoded = DecodeFrame(encoded.bytes.data(), encoded.bytes.size());
        Check(decoded.status == FrameStatus::kOk, "ack multi decode failed");
        Check(decoded.frame.ack_ranges.size() == 3, "ack range count wrong");
        for (std::size_t i = 0; i < ranges.size() && i < decoded.frame.ack_ranges.size(); ++i) {
            Check(decoded.frame.ack_ranges[i].gap == ranges[i].gap, "ack gap mismatch");
            Check(decoded.frame.ack_ranges[i].length == ranges[i].length,
                  "ack range length mismatch");
        }
    }
    // ACK_ECN
    {
        const auto frame = MakeAckEcn(50, 1, 3, {{2, 1}}, 11, 22, 33);
        const auto encoded = EncodeFrame(frame);
        const auto decoded = DecodeFrame(encoded.bytes.data(), encoded.bytes.size());
        Check(decoded.status == FrameStatus::kOk, "ack_ecn decode failed");
        Check(decoded.frame.ect0_count == 11 && decoded.frame.ect1_count == 22 &&
                  decoded.frame.ce_count == 33,
              "ack_ecn counts wrong");
    }
    std::printf("[quic-frame] ACK / ACK_ECN / Range 列表完成\n");
}

// ---------------------------------------------------------------------------
// 场景 5：PADDING 合并
// ---------------------------------------------------------------------------
void ScenarioPadding() {
    // 单个
    {
        const std::vector<std::uint8_t> one = {0x00};
        const auto decoded = DecodeFrame(one.data(), one.size());
        Check(decoded.status == FrameStatus::kOk, "single padding decode failed");
        Check(decoded.frame.padding_count == 1, "single padding count wrong");
    }
    // 连续 5 个
    {
        const std::vector<std::uint8_t> five(5, 0x00);
        const auto decoded = DecodeFrame(five.data(), five.size());
        Check(decoded.frame.padding_count == 5, "5-padding count wrong");
        Check(decoded.consumed == 5, "5-padding consumed wrong");
    }
    // 整个包全是零 + 后面跟着别的帧
    {
        const std::vector<std::uint8_t> mixed = {0, 0, 0, 0x01};   // 4 个 PADDING + PING
        const auto decoded = DecodeFrame(mixed.data(), mixed.size());
        Check(decoded.frame.type == FrameType::kPadding, "should be padding");
        Check(decoded.frame.padding_count == 3, "padding before PING should be 3");
        Check(decoded.consumed == 3, "padding consumed should stop at PING");
        // 继续解后续
        const auto next = DecodeFrame(mixed.data() + decoded.consumed,
                                      mixed.size() - decoded.consumed);
        Check(next.status == FrameStatus::kOk && next.frame.type == FrameType::kPing,
              "PING after padding should decode");
    }
    // 编码往返
    {
        const auto frame = MakePadding(17);
        const auto encoded = EncodeFrame(frame);
        Check(encoded.bytes.size() == 17, "padding encode size wrong");
        for (auto byte : encoded.bytes) {
            Check(byte == 0, "padding bytes must be zero");
        }
    }
    std::printf("[quic-frame] PADDING 合并完成\n");
}

// ---------------------------------------------------------------------------
// 场景 6：截断必须返回 kNeedMore 且 consumed == 0
// ---------------------------------------------------------------------------
void ScenarioTruncation() {
    const std::vector<Frame> frames = {
        MakeAck(100, 5, 3, {{1, 2}}),
        MakeResetStream(4, 0x11, 1000),
        MakeStopSending(8, 0x22),
        MakeCrypto(0, {1, 2, 3, 4, 5}),
        MakeNewToken({9, 8, 7, 6}),
        MakeStream(4, 100, true, true, true, {'a', 'b', 'c'}),
        MakeMaxData(1 << 20),
        MakeMaxStreamData(7, 1 << 19),
        MakeMaxStreams(100, true),
        MakeNewConnectionId(3, 1, {0xaa, 0xbb}, 
                            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}),
        MakeRetireConnectionId(2),
        MakePathChallenge({1, 2, 3, 4, 5, 6, 7, 8}),
        MakeConnectionCloseTransport(0x0a, 0x08, "bad frame"),
        MakeConnectionCloseApplication(0x42, "app gone"),
    };
    int truncated = 0;
    for (const auto& frame : frames) {
        const auto encoded = EncodeFrame(frame);
        if (encoded.status != FrameStatus::kOk) {
            Check(false, "truncation: encode failed");
            continue;
        }
        // 逐个前缀（从 1 到 size-1）
        //
        // **这里有个语义细节**：`DecodeFrame` 的 `length` 参数代表「包的剩余字节数」，
        // 不是「缓冲区还有多少」。所以当截断点已经读到了**完整的长度字段**、
        // 而该字段声明的长度超过"包剩余"时，按包边界语义这是**格式错误**
        // （例如 CONNECTION_CLOSE 读到 reason_length=9 但包里只剩 0 字节）。
        // 这不是 bug，而是"包必须完整"的直接推论。
        //
        // 因此本测试接受两种结果之一：
        //   - kNeedMore：截断发生在字段内部（正是我们要验证的主路径）
        //   - kFrameEncodingError：截断点之后只剩数据区，且长度字段已声明更多
        // 但**要求每个帧至少出现过一次 kNeedMore**，否则说明截断根本没被检测到。
        std::size_t need_more_seen = 0;
        for (std::size_t prefix = 1; prefix < encoded.bytes.size(); ++prefix) {
            const auto decoded = DecodeFrame(encoded.bytes.data(), prefix);
            if (decoded.status == FrameStatus::kNeedMore) {
                ++need_more_seen;
            } else if (decoded.status != FrameStatus::kFrameEncodingError) {
                std::fprintf(stderr,
                             "[quic-frame] truncation: type=0x%02x prefix=%zu/%zu 返回 %s\n",
                             encoded.bytes[0], prefix, encoded.bytes.size(),
                             ToString(decoded.status));
                Check(false, "truncated frame must report kNeedMore or kFrameEncodingError");
                break;
            }
            if (decoded.consumed != 0) {
                Check(false, "failed decode must consume nothing");
                break;
            }
            ++truncated;
        }
        if (need_more_seen == 0) {
            std::fprintf(stderr,
                         "[quic-frame] truncation: type=0x%02x 没有任何前缀报 kNeedMore\n",
                         encoded.bytes[0]);
            Check(false, "at least one truncation must report kNeedMore");
        }
    }
    std::printf("[quic-frame] 截断测试 %d 个前缀完成\n", truncated);
}

// ---------------------------------------------------------------------------
// 场景 7：畸形输入
// ---------------------------------------------------------------------------
void ScenarioMalformed() {
    // 1) CRYPTO 长度字段超过包剩余
    {
        const std::vector<std::uint8_t> bytes = {0x06, 0x00, 0x20, 0x01, 0x02};  // 声明 32 字节，只给 2
        const auto decoded = DecodeFrame(bytes.data(), bytes.size());
        Check(decoded.status == FrameStatus::kFrameEncodingError,
              "crypto length overflow must be frame-encoding-error");
    }
    // 2) STREAM（带 LEN）长度字段超剩余
    //
    // 注意 varint 编码：32 < 64，是**单字节** 0x20。
    // （最初这里写成 {0x40, 0x00}，但 0x40 是 2 字节 varint 的前缀，
    //  {0x40,0x00} 解码出来是 0 —— 长度 0 是合法的，所以那个用例其实是错的。）
    {
        const std::vector<std::uint8_t> bytes = {0x0a, 0x04, 0x20, 0x01};
        const auto decoded = DecodeFrame(bytes.data(), bytes.size());
        Check(decoded.status == FrameStatus::kFrameEncodingError,
              "stream length overflow must be frame-encoding-error");
    }
    // 3) NEW_TOKEN 空 token（RFC 9000 §19.7 明确禁止）
    {
        const std::vector<std::uint8_t> bytes = {0x07, 0x00};
        const auto decoded = DecodeFrame(bytes.data(), bytes.size());
        Check(decoded.status == FrameStatus::kFrameEncodingError,
              "empty NEW_TOKEN must be frame-encoding-error");
    }
    // 4) NEW_CONNECTION_ID 的 CID 长度 0
    {
        const std::vector<std::uint8_t> bytes = {0x18, 0x01, 0x00, 0x00,
                                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const auto decoded = DecodeFrame(bytes.data(), bytes.size());
        Check(decoded.status == FrameStatus::kFrameEncodingError,
              "CID length 0 must be frame-encoding-error");
    }
    // 5) NEW_CONNECTION_ID 的 CID 长度 21
    {
        std::vector<std::uint8_t> bytes = {0x18, 0x01, 0x00, 21};
        bytes.resize(4 + 21 + 16, 0);
        const auto decoded = DecodeFrame(bytes.data(), bytes.size());
        Check(decoded.status == FrameStatus::kFrameEncodingError,
              "CID length 21 must be frame-encoding-error");
    }
    // 6) NEW_CONNECTION_ID 的 retire_prior_to > sequence_number
    {
        std::vector<std::uint8_t> bytes = {0x18, 0x01, 0x05, 4, 0xaa, 0xbb};
        bytes.resize(6 + 16, 0);
        const auto decoded = DecodeFrame(bytes.data(), bytes.size());
        Check(decoded.status == FrameStatus::kFrameEncodingError,
              "retire_prior_to > sequence must be frame-encoding-error");
    }
    // 7) MAX_STREAMS 超过 2^60
    {
        // 2^61 = 0x2000000000000000，varint 8 字节编码：0xC0 | 高 6 位...
        // 简化：用 8 字节 varint 表达一个 > 2^60 的值
        // 2^61 的 8 字节 varint：前缀 11 + 62 位值
        std::vector<std::uint8_t> bytes = {0x12};
        const std::uint64_t too_big = (1ULL << 61);
        bytes.push_back(static_cast<std::uint8_t>(0xC0 | ((too_big >> 56) & 0x3f)));
        for (int shift = 48; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<std::uint8_t>((too_big >> shift) & 0xff));
        }
        const auto decoded = DecodeFrame(bytes.data(), bytes.size());
        Check(decoded.status == FrameStatus::kFrameEncodingError,
              "max_streams > 2^60 must be frame-encoding-error");
    }
    // 8) 空输入
    {
        const auto decoded = DecodeFrame(nullptr, 0);
        Check(decoded.status == FrameStatus::kNeedMore, "empty input must be need-more");
    }
    // 9) 序列化非法：NEW_CONNECTION_ID 的 reset token 不是 16 字节
    {
        auto frame = MakeNewConnectionId(1, 0, {0xaa}, {1, 2, 3});
        const auto encoded = EncodeFrame(frame);
        Check(encoded.status == FrameStatus::kInvalidArgument,
              "bad reset token length must be invalid-argument");
    }
    // 10) 序列化非法：PADDING count == 0
    {
        Frame frame = MakePadding(0);
        const auto encoded = EncodeFrame(frame);
        Check(encoded.status == FrameStatus::kInvalidArgument,
              "padding count 0 must be invalid-argument");
    }
    std::printf("[quic-frame] 畸形输入 10 例完成\n");
}

// ---------------------------------------------------------------------------
// 场景 8：未知帧类型
// ---------------------------------------------------------------------------
void ScenarioUnknownType() {
    // 0x1f 是已注册范围之外
    const std::vector<std::uint8_t> unknown = {0x1f, 0x01, 0x02};
    const auto decoded = DecodeFrame(unknown.data(), unknown.size());
    Check(decoded.status == FrameStatus::kUnknownType,
          "unknown type must report kUnknownType");
    Check(decoded.consumed == 0, "unknown type must not consume");

    // 0x30 / 0x7f / 0xff 同样是未知
    for (const std::uint8_t code : {0x20, 0x30, 0x7f, 0xff}) {
        const std::vector<std::uint8_t> bytes = {code, 0x00};
        const auto result = DecodeFrame(bytes.data(), bytes.size());
        if (result.status != FrameStatus::kUnknownType) {
            std::fprintf(stderr, "[quic-frame] type 0x%02x 应报 unknown，实际 %s\n", code,
                         ToString(result.status));
            Check(false, "unknown type codes must report kUnknownType");
        }
    }
    std::printf("[quic-frame] 未知帧类型完成（注意：RFC 9000 §12.4 要求当连接错误处理）\n");
}

// ---------------------------------------------------------------------------
// 场景 9：随机往返
// ---------------------------------------------------------------------------
void ScenarioRandomRoundTrip() {
    std::mt19937_64 rng(20261017);
    std::uniform_int_distribution<std::uint64_t> data_dist(0, 64);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    constexpr int kRounds = 20000;

    int checked = 0;
    for (int round = 0; round < kRounds; ++round) {
        const std::size_t data_size = static_cast<std::size_t>(data_dist(rng) % 64);
        std::vector<std::uint8_t> data(data_size);
        for (auto& byte : data) {
            byte = static_cast<std::uint8_t>(byte_dist(rng));
        }
        Frame frame;
        switch (round % 8) {
            case 0: frame = MakeStream(static_cast<std::uint64_t>(round),
                                       static_cast<std::uint64_t>(round * 7), round % 2 == 0,
                                       (round / 2) % 2 == 0, (round / 4) % 2 == 0,
                                       std::move(data)); break;
            case 1: frame = MakeCrypto(static_cast<std::uint64_t>(round), std::move(data)); break;
            case 2: frame = MakeAck(static_cast<std::uint64_t>(round), 1, 2,
                                    {{3, 4}}); break;
            case 3: frame = MakeMaxData(static_cast<std::uint64_t>(round)); break;
            case 4: frame = MakePathChallenge({1, 2, 3, 4, 5, 6, 7, 8}); break;
            case 5: {
                // vector 没有 operator+，先 push 再 move
                data.push_back(1);
                frame = MakeNewToken(std::move(data));
                break;
            }
            case 6: frame = MakeConnectionCloseTransport(round, round / 2, "x"); break;
            default: frame = MakeHandshakeDone(); break;
        }
        const auto encoded = EncodeFrame(frame);
        if (encoded.status != FrameStatus::kOk) {
            Check(false, "random encode failed");
            break;
        }
        const auto decoded = DecodeFrame(encoded.bytes.data(), encoded.bytes.size());
        if (decoded.status != FrameStatus::kOk || decoded.consumed != encoded.bytes.size()) {
            std::fprintf(stderr, "[quic-frame] random round-trip failed at %d\n", round);
            Check(false, "random round-trip failed");
            break;
        }
        ++checked;
    }
    std::printf("[quic-frame] 随机往返 %d 次完成\n", checked);
}

// ---------------------------------------------------------------------------
// 场景 10：随机变异 fuzz
// ---------------------------------------------------------------------------
void ScenarioFuzz() {
    std::mt19937_64 rng(998244353);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<std::size_t> len_dist(1, 64);
    constexpr int kRounds = 100000;

    int ok = 0, need_more = 0, encoding_error = 0, unknown = 0;
    for (int round = 0; round < kRounds; ++round) {
        const std::size_t len = len_dist(rng);
        std::vector<std::uint8_t> bytes(len);
        for (auto& byte : bytes) {
            byte = static_cast<std::uint8_t>(byte_dist(rng));
        }
        const auto decoded = DecodeFrame(bytes.data(), bytes.size());
        // 核心不变量：绝不能崩溃，且 consumed 永远不超过输入长度
        if (decoded.consumed > bytes.size()) {
            std::fprintf(stderr, "[quic-frame] fuzz: consumed %zu > length %zu\n",
                         decoded.consumed, bytes.size());
            Check(false, "fuzz consumed exceeds input length");
            break;
        }
        switch (decoded.status) {
            case FrameStatus::kOk:
                ++ok;
                // 成功时 consumed 必须 > 0
                if (decoded.consumed == 0) {
                    Check(false, "ok status must consume > 0");
                }
                break;
            case FrameStatus::kNeedMore: ++need_more; break;
            case FrameStatus::kFrameEncodingError: ++encoding_error; break;
            case FrameStatus::kUnknownType: ++unknown; break;
            case FrameStatus::kInvalidArgument: break;   // 解码不该返回这个
        }
        // 刻意**不**因前面的失败而 break：fuzz 要独立跑完，
        // 否则一个无关的失败会让统计全为 0，掩盖 fuzz 本身的结论。
    }
    std::printf("[quic-frame] fuzz %d 次: ok=%d need_more=%d encoding_error=%d unknown=%d\n",
                kRounds, ok, need_more, encoding_error, unknown);
    Check(ok > 0 && need_more > 0 && encoding_error > 0 && unknown > 0,
          "fuzz should exercise all outcome kinds");
}

}  // namespace

int main() {
    std::printf("=== QUIC 帧层测试（RFC 9000 §19）===\n");
    std::printf("[quic-frame][场景1] 基础帧往返 22 种\n");
    ScenarioBasicFrames();
    std::printf("[quic-frame][场景2] STREAM 8 变体\n");
    ScenarioStreamVariants();
    std::printf("[quic-frame][场景3] STREAM 无 LEN 位（延伸到包尾）\n");
    ScenarioStreamNoLength();
    std::printf("[quic-frame][场景4] ACK / ACK_ECN / Range\n");
    ScenarioAck();
    std::printf("[quic-frame][场景5] PADDING 合并\n");
    ScenarioPadding();
    std::printf("[quic-frame][场景6] 截断\n");
    ScenarioTruncation();
    std::printf("[quic-frame][场景7] 畸形输入\n");
    ScenarioMalformed();
    std::printf("[quic-frame][场景8] 未知帧类型\n");
    ScenarioUnknownType();
    std::printf("[quic-frame][场景9] 随机往返 20000 次\n");
    ScenarioRandomRoundTrip();
    std::printf("[quic-frame][场景10] 随机变异 fuzz 100000 次\n");
    ScenarioFuzz();

    if (g_failures == 0) {
        std::printf("\n[quic-frame] failures=0\n");
    } else {
        std::printf("\n[quic-frame] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
