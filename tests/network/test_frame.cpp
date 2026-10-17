/**
 * @file test_frame.cpp
 * @brief HTTP/2 帧层测试（RFC 9113 §4 / §6）。
 *
 * 重点覆盖三类最容易出错的地方：
 *
 * 1. **增量解析**：帧可能分多次到达，必须返回 `kNeedMore` 而不是报错；
 * 2. **stream id 规则**：SETTINGS/PING/GOAWAY 必须为 0，DATA/HEADERS 等不能为 0；
 * 3. **podding 与固定长度校验**：PADDED 的 pad length、SETTINGS 6 的倍数、
 *    PING 恰好 8 字节等。
 */
#include "go2cpp/network/http2/frame.hpp"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::net::http2;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[frame] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 把帧序列化成字节流（帧头 + payload）。
std::vector<std::uint8_t> Serialize(const Frame& frame) {
    std::vector<std::uint8_t> out(kFrameHeaderSize + frame.payload.size());
    EncodeFrameHeader(frame.header, out.data());
    if (!frame.payload.empty()) {
        std::copy(frame.payload.begin(), frame.payload.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 场景 1：帧头往返与增量解析
// ---------------------------------------------------------------------------
void ScenarioHeader() {
    FrameHeader header;
    header.length = 0x010203;
    header.type = FrameType::kHeaders;
    header.flags = 0x5;
    header.stream_id = 0x1234567;

    std::uint8_t buffer[kFrameHeaderSize] = {};
    EncodeFrameHeader(header, buffer);
    // 长度 24 位大端
    Check(buffer[0] == 0x01 && buffer[1] == 0x02 && buffer[2] == 0x03, "length not big-endian");
    Check(buffer[3] == 0x01, "type byte wrong");
    Check(buffer[4] == 0x05, "flags byte wrong");
    // stream id 31 位
    Check(buffer[5] == 0x01 && buffer[6] == 0x23 && buffer[7] == 0x45 && buffer[8] == 0x67,
          "stream id not big-endian");

    FrameHeader decoded;
    Check(DecodeFrameHeader(buffer, kFrameHeaderSize, &decoded) == FrameStatus::kOk,
          "decode header failed");
    Check(decoded.length == header.length && decoded.type == header.type &&
              decoded.flags == header.flags && decoded.stream_id == header.stream_id,
          "header round-trip mismatch");

    // 不足 9 字节 -> kNeedMore
    for (std::size_t available = 0; available < kFrameHeaderSize; ++available) {
        Check(DecodeFrameHeader(buffer, available, &decoded) == FrameStatus::kNeedMore,
              "partial header must be kNeedMore");
    }

    // 最高位（保留位）必须被忽略
    std::uint8_t reserved[kFrameHeaderSize] = {0, 0, 0, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    Check(DecodeFrameHeader(reserved, kFrameHeaderSize, &decoded) == FrameStatus::kOk,
          "reserved bit header decode failed");
    Check(decoded.stream_id == 0x7FFFFFFF, "reserved bit must be masked off");
}

// ---------------------------------------------------------------------------
// 场景 2：帧长超限与帧不完整
// ---------------------------------------------------------------------------
void ScenarioSizeLimits() {
    Frame out;
    std::size_t consumed = 0;

    // 声明长度 100，但只给 10 字节 -> kNeedMore
    std::vector<std::uint8_t> stream(kFrameHeaderSize + 10, 0);
    stream[0] = 0; stream[1] = 0; stream[2] = 100;   // length = 100
    stream[3] = 0;                                     // DATA
    stream[8] = 1;                                     // stream 1
    Check(DecodeFrame(stream.data(), stream.size(), &out, &consumed) == FrameStatus::kNeedMore,
          "incomplete frame must be kNeedMore");

    // 声明长度超过 max_frame_size -> kFrameSizeError（不等 payload）
    std::vector<std::uint8_t> big(kFrameHeaderSize, 0);
    big[0] = 0x00; big[1] = 0x40; big[2] = 0x01;   // length = 16385
    big[3] = 0;                                     // DATA
    big[8] = 1;
    Check(DecodeFrame(big.data(), big.size(), &out, &consumed,
                      kDefaultMaxFrameSize) == FrameStatus::kFrameSizeError,
          "oversized frame must be kFrameSizeError");

    // 恰好等于上限应当允许
    std::vector<std::uint8_t> exact(kFrameHeaderSize + kDefaultMaxFrameSize, 0);
    exact[0] = 0x00; exact[1] = 0x40; exact[2] = 0x00;   // 16384
    exact[3] = 0;
    exact[8] = 1;
    Check(DecodeFrame(exact.data(), exact.size(), &out, &consumed) == FrameStatus::kOk,
          "frame at max size should be accepted");
    Check(consumed == kFrameHeaderSize + kDefaultMaxFrameSize, "consumed wrong for max frame");
}

// ---------------------------------------------------------------------------
// 场景 3：stream id 规则
// ---------------------------------------------------------------------------
void ScenarioStreamIdRules() {
    Frame out;
    std::size_t consumed = 0;

    // SETTINGS 带非 0 stream id -> 协议错误
    {
        auto bytes = Serialize(MakeSettings({}));
        bytes[8] = 1;   // stream id = 1
        Check(DecodeFrame(bytes.data(), bytes.size(), &out, &consumed) ==
                  FrameStatus::kProtocolError,
              "SETTINGS with non-zero stream must be protocol error");
    }
    // DATA 带 stream 0 -> 协议错误
    {
        auto bytes = Serialize(MakeData(0, false, {1, 2, 3}));
        Check(DecodeFrame(bytes.data(), bytes.size(), &out, &consumed) ==
                  FrameStatus::kProtocolError,
              "DATA with stream 0 must be protocol error");
    }
    // GOAWAY 带非 0 stream id -> 协议错误
    {
        auto bytes = Serialize(MakeGoAway(1, ErrorCode::kNoError));
        bytes[8] = 3;
        Check(DecodeFrame(bytes.data(), bytes.size(), &out, &consumed) ==
                  FrameStatus::kProtocolError,
              "GOAWAY with non-zero stream must be protocol error");
    }
    // WINDOW_UPDATE 允许 stream 0（连接级）
    {
        auto bytes = Serialize(MakeWindowUpdate(0, 100));
        Check(DecodeFrame(bytes.data(), bytes.size(), &out, &consumed) == FrameStatus::kOk,
              "WINDOW_UPDATE with stream 0 must be allowed");
    }
}

// ---------------------------------------------------------------------------
// 场景 4：SETTINGS
// ---------------------------------------------------------------------------
void ScenarioSettings() {
    const std::vector<SettingsEntry> entries = {
        {static_cast<std::uint16_t>(SettingsId::kHeaderTableSize), 4096},
        {static_cast<std::uint16_t>(SettingsId::kEnablePush), 0},
        {static_cast<std::uint16_t>(SettingsId::kMaxFrameSize), 32768},
        {static_cast<std::uint16_t>(SettingsId::kInitialWindowSize), 65535},
    };
    const auto frame = MakeSettings(entries);
    Check(frame.payload.size() == entries.size() * 6, "settings payload length wrong");

    SettingsFrame parsed;
    std::string error;
    Check(ParseSettings(frame, &parsed, &error) == FrameStatus::kOk, "parse settings failed");
    Check(!parsed.ack, "settings should not be ack");
    Check(parsed.entries.size() == entries.size(), "settings entry count wrong");
    for (std::size_t i = 0; i < entries.size() && i < parsed.entries.size(); ++i) {
        Check(parsed.entries[i].id == entries[i].id && parsed.entries[i].value == entries[i].value,
              "settings entry mismatch");
    }

    // ACK：空 payload、flags = ACK
    const auto ack = MakeSettingsAck();
    Check(ack.payload.empty(), "settings ack payload must be empty");
    Check((ack.header.flags & FrameFlags::kAck) != 0, "settings ack flag missing");
    SettingsFrame parsed_ack;
    Check(ParseSettings(ack, &parsed_ack, &error) == FrameStatus::kOk, "parse settings ack failed");
    Check(parsed_ack.ack && parsed_ack.entries.empty(), "settings ack semantics wrong");

    // ACK 带 payload -> 协议错误
    {
        Frame bad = ack;
        bad.payload = {1, 2, 3};
        bad.header.length = 3;
        SettingsFrame tmp;
        Check(ParseSettings(bad, &tmp, &error) == FrameStatus::kProtocolError,
              "settings ack with payload must be rejected");
    }
    // 长度非 6 的倍数 -> 协议错误
    {
        Frame bad = MakeSettings({{1, 1}});
        bad.payload.push_back(0);
        bad.header.length += 1;
        SettingsFrame tmp;
        Check(ParseSettings(bad, &tmp, &error) == FrameStatus::kProtocolError,
              "settings with non-multiple-of-6 length must be rejected");
    }
}

// ---------------------------------------------------------------------------
// 场景 5：PING
// ---------------------------------------------------------------------------
void ScenarioPing() {
    const auto frame = MakePing(0x1122334455667788ULL, false);
    Check(frame.payload.size() == 8, "ping payload must be 8 bytes");

    PingFrame parsed;
    std::string error;
    Check(ParsePing(frame, &parsed, &error) == FrameStatus::kOk, "parse ping failed");
    Check(!parsed.ack, "ping should not be ack");
    Check(parsed.opaque_data == 0x1122334455667788ULL, "ping opaque data mismatch");

    const auto ack = MakePing(0xAABBCCDDULL, true);
    PingFrame parsed_ack;
    Check(ParsePing(ack, &parsed_ack, &error) == FrameStatus::kOk, "parse ping ack failed");
    Check(parsed_ack.ack && parsed_ack.opaque_data == 0xAABBCCDDULL, "ping ack mismatch");

    // 长度非 8 -> 协议错误
    Frame bad = frame;
    bad.payload.pop_back();
    bad.header.length = 7;
    PingFrame tmp;
    Check(ParsePing(bad, &tmp, &error) == FrameStatus::kProtocolError,
          "ping with 7 bytes must be rejected");
}

// ---------------------------------------------------------------------------
// 场景 6：DATA（含 padding）
// ---------------------------------------------------------------------------
void ScenarioData() {
    const std::vector<std::uint8_t> payload = {'h', 'e', 'l', 'l', 'o'};
    auto frame = MakeData(5, true, payload);
    DataFrame parsed;
    std::string error;
    Check(ParseData(frame, &parsed, &error) == FrameStatus::kOk, "parse data failed");
    Check(parsed.stream_id == 5 && parsed.end_stream, "data header fields wrong");
    Check(parsed.data == payload, "data payload mismatch");

    // PADDED：pad length = 3，数据 "hi"，总 payload = 1 + 2 + 3 = 6
    Frame padded;
    padded.header.type = FrameType::kData;
    padded.header.stream_id = 7;
    padded.header.flags = FrameFlags::kPadded | FrameFlags::kEndStream;
    padded.payload = {3, 'h', 'i', 0, 0, 0};
    padded.header.length = static_cast<std::uint32_t>(padded.payload.size());
    DataFrame parsed_padded;
    Check(ParseData(padded, &parsed_padded, &error) == FrameStatus::kOk,
          "parse padded data failed");
    Check(parsed_padded.data.size() == 2 && parsed_padded.data[0] == 'h' &&
              parsed_padded.data[1] == 'i',
          "padded data payload wrong");
    Check(parsed_padded.end_stream, "padded data end_stream wrong");

    // pad length 超出 payload -> 协议错误
    Frame bad;
    bad.header.type = FrameType::kData;
    bad.header.stream_id = 1;
    bad.header.flags = FrameFlags::kPadded;
    bad.payload = {9, 'x'};
    bad.header.length = 2;
    DataFrame tmp;
    Check(ParseData(bad, &tmp, &error) == FrameStatus::kProtocolError,
          "oversized pad length must be rejected");
}

// ---------------------------------------------------------------------------
// 场景 7：HEADERS / CONTINUATION
// ---------------------------------------------------------------------------
void ScenarioHeaders() {
    const std::vector<std::uint8_t> block = {0x82, 0x86, 0x84};
    auto frame = MakeHeaders(3, true, false, block);
    HeadersFrame parsed;
    std::string error;
    Check(ParseHeaders(frame, &parsed, &error) == FrameStatus::kOk, "parse headers failed");
    Check(parsed.stream_id == 3 && parsed.end_stream && !parsed.end_headers,
          "headers flags wrong");
    Check(parsed.header_block == block, "headers block mismatch");

    // 带 PRIORITY 字段（5 字节）应当被跳过
    Frame with_priority;
    with_priority.header.type = FrameType::kHeaders;
    with_priority.header.stream_id = 9;
    with_priority.header.flags = FrameFlags::kPriority | FrameFlags::kEndHeaders;
    with_priority.payload = {0, 0, 0, 1, 15, 0x82, 0x86};   // 5 字节优先级 + 2 字节块
    with_priority.header.length = static_cast<std::uint32_t>(with_priority.payload.size());
    HeadersFrame parsed_priority;
    Check(ParseHeaders(with_priority, &parsed_priority, &error) == FrameStatus::kOk,
          "parse headers with priority failed");
    Check(parsed_priority.header_block.size() == 2 &&
              parsed_priority.header_block[0] == 0x82,
          "priority field should be skipped");

    // CONTINUATION
    auto cont = MakeContinuation(3, true, {0x01, 0x02});
    HeadersFrame parsed_cont;
    Check(ParseContinuation(cont, &parsed_cont, &error) == FrameStatus::kOk,
          "parse continuation failed");
    Check(parsed_cont.stream_id == 3 && parsed_cont.end_headers, "continuation flags wrong");
    Check(parsed_cont.header_block.size() == 2, "continuation block size wrong");
}

// ---------------------------------------------------------------------------
// 场景 8：RST_STREAM / GOAWAY / WINDOW_UPDATE / PRIORITY
// ---------------------------------------------------------------------------
void ScenarioOtherFrames() {
    std::string error;
    {
        auto frame = MakeRstStream(11, ErrorCode::kCancel);
        RstStreamFrame parsed;
        Check(ParseRstStream(frame, &parsed, &error) == FrameStatus::kOk, "parse rst failed");
        Check(parsed.stream_id == 11 && parsed.error_code == ErrorCode::kCancel,
              "rst fields wrong");
        // 长度非 4
        Frame bad = frame;
        bad.payload.pop_back();
        bad.header.length = 3;
        RstStreamFrame tmp;
        Check(ParseRstStream(bad, &tmp, &error) == FrameStatus::kProtocolError,
              "rst with 3 bytes must be rejected");
    }
    {
        const std::vector<std::uint8_t> debug = {'o', 'o', 'p', 's'};
        auto frame = MakeGoAway(17, ErrorCode::kProtocolError, debug);
        GoAwayFrame parsed;
        Check(ParseGoAway(frame, &parsed, &error) == FrameStatus::kOk, "parse goaway failed");
        Check(parsed.last_stream_id == 17 &&
                  parsed.error_code == ErrorCode::kProtocolError,
              "goaway fields wrong");
        Check(parsed.debug_data == debug, "goaway debug data mismatch");
    }
    {
        auto frame = MakeWindowUpdate(0, 65535);
        WindowUpdateFrame parsed;
        Check(ParseWindowUpdate(frame, &parsed, &error) == FrameStatus::kOk,
              "parse window update failed");
        Check(parsed.stream_id == 0 && parsed.increment == 65535, "window update fields wrong");
        // 增量 0 -> 协议错误
        Frame bad = MakeWindowUpdate(1, 0);
        WindowUpdateFrame tmp;
        Check(ParseWindowUpdate(bad, &tmp, &error) == FrameStatus::kProtocolError,
              "window update with zero increment must be rejected");
    }
    {
        auto frame = MakePriority(21, 5, true, 200);
        PriorityFrame parsed;
        Check(ParsePriority(frame, &parsed, &error) == FrameStatus::kOk, "parse priority failed");
        Check(parsed.stream_id == 21 && parsed.depends_on == 5 && parsed.exclusive &&
                  parsed.weight == 200,
              "priority fields wrong");
    }
}

// ---------------------------------------------------------------------------
// 场景 9：增量解析 + 粘包
// ---------------------------------------------------------------------------
void ScenarioIncrementalAndCoalesced() {
    // 构造两个连续帧
    const auto f1 = MakeData(1, false, {'a', 'b'});
    const auto f2 = MakePing(42, false);
    auto bytes = Serialize(f1);
    const auto b2 = Serialize(f2);
    bytes.insert(bytes.end(), b2.begin(), b2.end());

    // 逐字节喂：直到收全第一个帧才返回成功
    Frame out;
    std::size_t consumed = 0;
    const std::size_t first_size = kFrameHeaderSize + f1.payload.size();
    bool first_ok_at = false;
    for (std::size_t available = 1; available < first_size; ++available) {
        const auto st = DecodeFrame(bytes.data(), available, &out, &consumed);
        Check(st == FrameStatus::kNeedMore, "partial first frame must be kNeedMore");
    }
    Check(DecodeFrame(bytes.data(), first_size, &out, &consumed) == FrameStatus::kOk,
          "first frame should decode at exact size");
    Check(consumed == first_size, "first frame consumed wrong");
    first_ok_at = true;
    Check(first_ok_at, "control");

    // 粘包：一次给完整两个帧，第一个 consumed 后还剩第二个
    Check(DecodeFrame(bytes.data(), bytes.size(), &out, &consumed) == FrameStatus::kOk,
          "coalesced first frame failed");
    Check(DecodeFrame(bytes.data() + consumed, bytes.size() - consumed, &out, &consumed) ==
              FrameStatus::kOk,
          "coalesced second frame failed");
    Check(out.header.type == FrameType::kPing, "second frame should be PING");
}

// ---------------------------------------------------------------------------
// 场景 10：随机往返压测
// ---------------------------------------------------------------------------
void ScenarioRandomStress() {
    std::mt19937 rng(20261017);
    std::uniform_int_distribution<int> sid_dist(1, 1000);
    std::uniform_int_distribution<int> len_dist(0, 64);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    constexpr int kRounds = 3000;

    Frame out;
    std::size_t consumed = 0;
    for (int round = 0; round < kRounds; ++round) {
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(len_dist(rng)));
        for (auto& b : payload) {
            b = static_cast<std::uint8_t>(byte_dist(rng));
        }
        const std::uint32_t sid = static_cast<std::uint32_t>(sid_dist(rng));
        const auto frame = MakeData(sid, (round % 2) == 0, payload);
        const auto bytes = Serialize(frame);
        if (DecodeFrame(bytes.data(), bytes.size(), &out, &consumed) != FrameStatus::kOk) {
            Check(false, "random frame decode failed");
            break;
        }
        DataFrame parsed;
        std::string error;
        if (ParseData(out, &parsed, &error) != FrameStatus::kOk) {
            Check(false, "random frame data parse failed");
            break;
        }
        if (parsed.stream_id != sid || parsed.data != payload) {
            Check(false, "random frame round-trip mismatch");
            break;
        }
    }
    std::printf("[frame] 随机往返 %d 轮完成\n", kRounds);
}

}  // namespace

int main() {
    std::printf("=== HTTP/2 帧层测试（RFC 9113）===\n");
    std::printf("[frame][场景1] 帧头往返与增量解析\n");
    ScenarioHeader();
    std::printf("[frame][场景2] 帧长上限\n");
    ScenarioSizeLimits();
    std::printf("[frame][场景3] stream id 规则\n");
    ScenarioStreamIdRules();
    std::printf("[frame][场景4] SETTINGS\n");
    ScenarioSettings();
    std::printf("[frame][场景5] PING\n");
    ScenarioPing();
    std::printf("[frame][场景6] DATA（含 padding）\n");
    ScenarioData();
    std::printf("[frame][场景7] HEADERS / CONTINUATION\n");
    ScenarioHeaders();
    std::printf("[frame][场景8] RST_STREAM / GOAWAY / WINDOW_UPDATE / PRIORITY\n");
    ScenarioOtherFrames();
    std::printf("[frame][场景9] 增量解析与粘包\n");
    ScenarioIncrementalAndCoalesced();
    std::printf("[frame][场景10] 随机往返 3000 轮\n");
    ScenarioRandomStress();

    if (g_failures == 0) {
        std::printf("\n[frame] failures=0\n");
    } else {
        std::printf("\n[frame] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
