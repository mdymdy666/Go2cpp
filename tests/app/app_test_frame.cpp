/**
 * @file app_test_frame.cpp
 * @brief 帧编解码 / 流式解码器 / KV 文本与二进制 / Bento 帧桥测试。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "app_test_support.hpp"
#include "go2cpp/app/frame.hpp"
#include "go2cpp/app/kv.hpp"

#if defined(GO2CPP_APP_HAS_BENTO)
#include "go2cpp/app/bento_bridge.hpp"
#include "go2cpp/bento/net/frame.hpp"
#endif

namespace {

using namespace go2cpp::app;

void FrameRoundTrip() {
    APP_SCENARIO("frame: encode/decode round trip for every type");
    const std::vector<FrameType> types = {
        FrameType::kData,       FrameType::kHeaders,      FrameType::kPriority,
        FrameType::kRstStream,  FrameType::kSettings,     FrameType::kPing,
        FrameType::kGoAway,     FrameType::kWindowUpdate, FrameType::kResourceIndex,
    };
    for (FrameType type : types) {
        Frame frame;
        frame.type = type;
        frame.stream_id = (type == FrameType::kSettings || type == FrameType::kPing ||
                           type == FrameType::kGoAway)
                              ? 0
                              : 7;
        frame.flags = kFlagEndStream;
        frame.payload = app_test::MakePattern(37, static_cast<std::uint32_t>(type) + 1);
        const std::vector<std::uint8_t> encoded = EncodeFrame(frame);
        APP_REQUIRE(!encoded.empty());
        APP_CHECK(encoded.size() == kFrameHeaderBytes + frame.payload.size());
        Frame decoded;
        std::size_t consumed = 0;
        APP_CHECK(DecodeFrame(encoded.data(), encoded.size(), &decoded, &consumed) ==
                  FrameStatus::kOk);
        APP_CHECK(consumed == encoded.size());
        APP_CHECK(decoded.type == type);
        APP_CHECK(decoded.stream_id == frame.stream_id);
        APP_CHECK(decoded.flags == frame.flags);
        APP_CHECK(decoded.payload == frame.payload);
    }
}

void FrameDecoderStickyAndHalfPacket() {
    APP_SCENARIO("frame: streaming decoder handles sticky/half packets");
    Frame first;
    first.type = FrameType::kHeaders;
    first.stream_id = 3;
    first.flags = kFlagEndHeaders;
    first.payload = {1, 2, 3, 4, 5};
    Frame second;
    second.type = FrameType::kData;
    second.stream_id = 3;
    second.payload = app_test::MakePattern(1000, 99);
    std::vector<std::uint8_t> stream = EncodeFrame(first);
    const std::vector<std::uint8_t> second_bytes = EncodeFrame(second);
    stream.insert(stream.end(), second_bytes.begin(), second_bytes.end());

    FrameDecoder decoder;
    std::size_t decoded = 0;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        decoder.Append(&stream[i], 1);
        Frame frame;
        while (decoder.Next(&frame) == FrameStatus::kOk) {
            if (decoded == 0) {
                APP_CHECK(frame.type == FrameType::kHeaders);
                APP_CHECK(frame.payload.size() == 5);
            } else {
                APP_CHECK(frame.type == FrameType::kData);
                APP_CHECK(frame.payload == second.payload);
            }
            ++decoded;
        }
    }
    APP_CHECK(decoded == 2);
    APP_CHECK(decoder.PendingBytes() == 0);

    // 一次性塞入两帧（粘包）。
    FrameDecoder bulk;
    bulk.Append(stream.data(), stream.size());
    Frame frame;
    APP_CHECK(bulk.Next(&frame) == FrameStatus::kOk);
    APP_CHECK(bulk.Next(&frame) == FrameStatus::kOk);
    APP_CHECK(bulk.Next(&frame) == FrameStatus::kNeedMoreData);
}

void FrameRejectsInvalidInput() {
    APP_SCENARIO("frame: rejects R-bit/length/unknown-type violations");
    // R 位非 0。
    std::vector<std::uint8_t> bad(9, 0);
    bad[3] = 0;
    bad[5] = 0x80;
    Frame frame;
    APP_CHECK(DecodeFrame(bad.data(), bad.size(), &frame) ==
              FrameStatus::kInvalidStreamId);
    // 未知帧类型。
    std::vector<std::uint8_t> unknown(9, 0);
    unknown[3] = 0x7f;
    APP_CHECK(DecodeFrame(unknown.data(), unknown.size(), &frame) ==
              FrameStatus::kInvalidType);
    // 声明长度超过上限。
    std::vector<std::uint8_t> oversized(9, 0);
    oversized[0] = 0x01;
    oversized[1] = 0x00;
    oversized[2] = 0x01;  // 65537
    APP_CHECK(DecodeFrame(oversized.data(), oversized.size(), 65536, &frame) ==
              FrameStatus::kLengthExceeded);
    // 半包。
    Frame valid;
    valid.type = FrameType::kData;
    valid.payload = app_test::MakePattern(100, 3);
    const std::vector<std::uint8_t> bytes = EncodeFrame(valid);
    APP_CHECK(DecodeFrame(bytes.data(), bytes.size() - 1, &frame) ==
              FrameStatus::kNeedMoreData);
    // 编码拒绝非法 stream id / 超长载荷。
    Frame invalid = valid;
    invalid.stream_id = kMaxStreamId + 1;
    APP_CHECK(EncodeFrame(invalid).empty());
    APP_CHECK(DecodeFrame(nullptr, 0, &frame) == FrameStatus::kNeedMoreData);
    APP_CHECK(DecodeFrame(bytes.data(), bytes.size(), nullptr) ==
              FrameStatus::kInvalidArgument);
}

void StandardPayloadRoundTrips() {
    APP_SCENARIO("frame: standard payload codecs round trip");
    std::vector<SettingsEntry> entries = {
        {SettingId::kMaxConcurrentStreams, 1000},
        {SettingId::kInitialWindowSize, 65535},
        {SettingId::kMaxFrameSize, 262144},
        {SettingId::kSmallKvThreshold, 4096},
    };
    const std::vector<std::uint8_t> settings = EncodeSettings(entries);
    std::vector<SettingsEntry> decoded_entries;
    APP_CHECK(DecodeSettings(settings.data(), settings.size(), &decoded_entries));
    APP_CHECK(decoded_entries.size() == entries.size());
    APP_CHECK(decoded_entries[1].id == SettingId::kInitialWindowSize);
    APP_CHECK(decoded_entries[1].value == 65535);

    const std::vector<std::uint8_t> goaway = EncodeGoAway(123, ErrorCode::kCancel, "bye");
    std::uint32_t last = 0;
    ErrorCode code = ErrorCode::kNoError;
    std::string debug;
    APP_CHECK(DecodeGoAway(goaway.data(), goaway.size(), &last, &code, &debug));
    APP_CHECK(last == 123);
    APP_CHECK(code == ErrorCode::kCancel);
    APP_CHECK(debug == "bye");

    const std::vector<std::uint8_t> rst = EncodeRstStream(ErrorCode::kChecksumError);
    ErrorCode decoded_code = ErrorCode::kNoError;
    APP_CHECK(DecodeRstStream(rst.data(), rst.size(), &decoded_code));
    APP_CHECK(decoded_code == ErrorCode::kChecksumError);

    const std::vector<std::uint8_t> update = EncodeWindowUpdate(4096);
    std::uint32_t increment = 0;
    APP_CHECK(DecodeWindowUpdate(update.data(), update.size(), &increment));
    APP_CHECK(increment == 4096);
    const std::vector<std::uint8_t> zero_update = EncodeWindowUpdate(0);
    APP_CHECK(!DecodeWindowUpdate(zero_update.data(), zero_update.size(), &increment));

    const std::vector<std::uint8_t> priority = EncodePriority(9, 200);
    std::uint32_t parent = 0;
    std::uint16_t weight = 0;
    APP_CHECK(DecodePriority(priority.data(), priority.size(), &parent, &weight));
    APP_CHECK(parent == 9 && weight == 200);

    std::array<std::uint8_t, 8> opaque = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::uint8_t> ping = EncodePing(opaque);
    std::array<std::uint8_t, 8> decoded_ping{};
    APP_CHECK(DecodePing(ping.data(), ping.size(), &decoded_ping));
    APP_CHECK(decoded_ping == opaque);
    APP_CHECK(!DecodePing(ping.data(), 7, &decoded_ping));

    // 大载荷（1 MiB 上限内）。
    Frame large;
    large.type = FrameType::kData;
    large.payload = app_test::MakePattern(1024 * 1024, 7);
    const std::vector<std::uint8_t> encoded = EncodeFrame(large);
    APP_CHECK(encoded.size() == kFrameHeaderBytes + large.payload.size());
}

void KvTextParsing() {
    APP_SCENARIO("kv: text parsing/escaping/empty values");
    const std::string text = "k1=v1;k2=v2;k3=v3;k4=v4;k5=v5";
    KvTable table;
    std::string error;
    APP_REQUIRE(KvTable::ParseText(text, &table, &error));
    APP_CHECK(table.size() == 5);
    APP_CHECK(table.Get("k1") == "v1");
    APP_CHECK(table.Get("k5") == "v5");
    APP_CHECK(table.ToText() == text);

    // 转义：分号、等号、反斜杠。
    KvTable escaped;
    escaped.Set("a;b", "c=d\\e");
    const std::string encoded = escaped.ToText();
    KvTable reparsed;
    APP_REQUIRE(KvTable::ParseText(encoded, &reparsed, &error));
    APP_CHECK(reparsed == escaped);

    // 空值 / 无等号 / 空串 / 末尾分号。
    KvTable edge;
    APP_CHECK(KvTable::ParseText("a=;b;c=;", &edge, &error));
    APP_CHECK(edge.size() == 3);
    APP_CHECK(edge.Get("a").empty());
    APP_CHECK(edge.Get("b").empty());
    APP_CHECK(edge.Get("c").empty());
    KvTable empty;
    APP_CHECK(KvTable::ParseText("", &empty, &error));
    APP_CHECK(empty.empty());
    // 空键非法。
    KvTable bad;
    APP_CHECK(!KvTable::ParseText("=v", &bad, &error));
    APP_CHECK(!KvTable::ParseText(";=v", &bad, &error));
    // 重复键后者覆盖且保持位置。
    KvTable duplicate;
    APP_REQUIRE(KvTable::ParseText("a=1;b=2;a=3", &duplicate, &error));
    APP_CHECK(duplicate.size() == 2);
    APP_CHECK(duplicate.entries()[0].first == "a");
    APP_CHECK(duplicate.Get("a") == "3");
    // 空白不裁剪。
    KvTable spaces;
    APP_REQUIRE(KvTable::ParseText(" a = b ;c=d", &spaces, &error));
    APP_CHECK(spaces.Get(" a ") == " b ");
    APP_CHECK(spaces.Get("c") == "d");
    // 非法 UTF-8 字节也能往返（按字节保存）。
    KvTable binary;
    binary.Set("bin", std::string("\x00\x01\xff", 3));
    KvTable binary_back;
    APP_REQUIRE(KvTable::ParseText(binary.ToText(), &binary_back, &error));
    APP_CHECK(binary_back.Get("bin") == std::string("\x00\x01\xff", 3));
}

void KvBinaryRoundTripAndRandom() {
    APP_SCENARIO("kv: binary round trip + 2000 random tables + fuzz");
    std::mt19937_64 rng(20261031);
    std::uniform_int_distribution<int> count_dist(0, 40);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (int round = 0; round < 2000; ++round) {
        KvTable table;
        const int count = count_dist(rng);
        for (int i = 0; i < count; ++i) {
            std::string key = "k" + std::to_string(round) + "_" + std::to_string(i);
            std::string value;
            const int length = static_cast<int>(rng() % 64);
            for (int j = 0; j < length; ++j) {
                value.push_back(static_cast<char>(byte_dist(rng)));
            }
            table.Set(std::move(key), std::move(value));
        }
        const std::vector<std::uint8_t> binary = table.ToBinary();
        KvTable decoded;
        std::string error;
        if (!KvTable::ParseBinary(binary.data(), binary.size(), &decoded, &error)) {
            APP_CHECK(false);
            continue;
        }
        APP_CHECK(decoded == table);
    }
    // 随机字节 fuzz：不崩溃、不成功时不改输出。
    for (int round = 0; round < 2000; ++round) {
        std::vector<std::uint8_t> garbage(static_cast<std::size_t>(rng() % 128));
        for (auto& byte : garbage) {
            byte = static_cast<std::uint8_t>(byte_dist(rng));
        }
        KvTable table;
        std::string error;
        KvTable::ParseBinary(garbage.data(), garbage.size(), &table, &error);
        KvTable text;
        KvTable::ParseText(
            std::string(reinterpret_cast<const char*>(garbage.data()), garbage.size()),
            &text, &error);
    }
}

#if defined(GO2CPP_APP_HAS_BENTO)
void BentoBridgeRoundTrip() {
    APP_SCENARIO("bento bridge: app frame <-> 20B Bento frame");
    const std::vector<FrameType> types = {
        FrameType::kData,      FrameType::kHeaders,     FrameType::kPriority,
        FrameType::kRstStream, FrameType::kSettings,    FrameType::kPing,
        FrameType::kGoAway,    FrameType::kWindowUpdate, FrameType::kResourceIndex,
    };
    for (int i = 0; i < 16; ++i) {
        Frame frame;
        frame.type = types[static_cast<std::size_t>(i) % types.size()];
        frame.stream_id = static_cast<std::uint32_t>(i);
        frame.flags = static_cast<std::uint8_t>(i & 0x0f);
        frame.payload = app_test::MakePattern(static_cast<std::size_t>(i * 37), i + 1);
        const std::vector<std::uint8_t> encoded = go2cpp::app::bento_bridge::Encode(frame);
        APP_REQUIRE(!encoded.empty());
        // Bento 默认帧解码器能识别（magic/CRC 正确）。
        go2cpp::bento::net::Frame bento;
        APP_CHECK(go2cpp::bento::net::Decode(encoded.data(), encoded.size(), &bento) ==
                  go2cpp::bento::net::FrameStatus::kOk);
        APP_CHECK(bento.type == go2cpp::app::bento_bridge::kAppFrameType);
        Frame decoded;
        std::size_t consumed = 0;
        APP_CHECK(go2cpp::app::bento_bridge::Decode(encoded.data(), encoded.size(),
                                                    &decoded, &consumed) ==
                  go2cpp::app::bento_bridge::Status::kOk);
        APP_CHECK(consumed == encoded.size());
        APP_CHECK(decoded.type == frame.type);
        APP_CHECK(decoded.stream_id == frame.stream_id);
        APP_CHECK(decoded.flags == frame.flags);
        APP_CHECK(decoded.payload == frame.payload);
    }
    // 半包 + 粘包。
    Frame a;
    a.type = FrameType::kHeaders;
    a.stream_id = 1;
    a.payload = {1, 2, 3};
    Frame b;
    b.type = FrameType::kData;
    b.stream_id = 1;
    b.payload = app_test::MakePattern(500, 2);
    const std::vector<std::uint8_t> first = go2cpp::app::bento_bridge::Encode(a);
    const std::vector<std::uint8_t> second = go2cpp::app::bento_bridge::Encode(b);
    std::vector<std::uint8_t> combined = first;
    combined.insert(combined.end(), second.begin(), second.end());
    go2cpp::app::bento_bridge::Decoder decoder;
    std::size_t count = 0;
    for (std::size_t i = 0; i < combined.size(); i += 3) {
        const std::size_t chunk = std::min<std::size_t>(3, combined.size() - i);
        decoder.Append(combined.data() + i, chunk);
        Frame frame;
        while (decoder.Next(&frame) == go2cpp::app::bento_bridge::Status::kOk) {
            ++count;
        }
    }
    APP_CHECK(count == 2);
}
#endif

}  // namespace

int main() {
    FrameRoundTrip();
    FrameDecoderStickyAndHalfPacket();
    FrameRejectsInvalidInput();
    StandardPayloadRoundTrips();
    KvTextParsing();
    KvBinaryRoundTripAndRandom();
#if defined(GO2CPP_APP_HAS_BENTO)
    BentoBridgeRoundTrip();
#endif
    return app_test::Finish("app_test_frame");
}
