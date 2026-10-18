/**
 * @file test_scenarios.cpp
 * @brief `encoding` 模块的 6 个真实场景模拟。
 *
 * 场景（覆盖四个子模块的全部功能点）：
 *   1. **JWT 解码**：base64url（无填充）解 header/payload/signature，使用 jwt.io
 *      的公开示例 token，签名 hex 作为独立 oracle；
 *   2. **hex dump**：二进制文件（512 字节）写入临时文件 → 读回 → 带偏移输出，
 *      校验偏移、ASCII 区、行数与文件清理；
 *   3. **二进制协议**：magic + version + type + flags + length + payload 的自定义
 *      包头，大端读写、多帧连续解析、截断检测；
 *   4. **CSV 导出**：含逗号 / 引号 / 换行 / 前导空格的字段写文件 → 回读解析；
 *   5. **base64 图片**：1 MiB PNG-like 数据 → 标准 base64 → data URL → 解码往返；
 *   6. **LEB128 序列化**：uint32 id + zig-zag varint 增量 + 长度前缀字符串，
 *      模拟时序数据/协议字段的紧凑编码与顺序解析。
 */

#include "encoding_test_support.hpp"

#include "go2cpp/encoding/base64.hpp"
#include "go2cpp/encoding/binary.hpp"
#include "go2cpp/encoding/csv.hpp"
#include "go2cpp/encoding/hex.hpp"
#include "go2cpp/error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace e = go2cpp::encoding;

using go2cpp::ErrorMessage;
using go2cpp::Is;

namespace {

std::string TempPath(const char* tag) {
    return "/tmp/go2cpp_encoding_" + std::to_string(::getpid()) + "_" + tag;
}

bool WriteFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// 生成带偏移与 ASCII 区的 hex dump；字节部分复用 `Hex::Encode`。
std::string HexDump(std::string_view data, std::size_t width = 16) {
    std::string out;
    for (std::size_t offset = 0; offset < data.size(); offset += width) {
        char prefix[32] = {};
        std::snprintf(prefix, sizeof(prefix), "%08zx", offset);
        out += prefix;
        out += "  ";
        const std::size_t line_bytes =
            std::min(width, data.size() - offset);
        const std::string hex = e::Hex::EncodeToString(
            data.substr(offset, line_bytes));
        for (std::size_t i = 0; i < width; ++i) {
            if (i < line_bytes) {
                out.push_back(hex[i * 2]);
                out.push_back(hex[i * 2 + 1]);
                out.push_back(' ');
            } else {
                out += "   ";
            }
        }
        out += " |";
        for (std::size_t i = 0; i < line_bytes; ++i) {
            const unsigned char byte =
                static_cast<unsigned char>(data[offset + i]);
            out.push_back(byte >= 0x20 && byte < 0x7f
                              ? static_cast<char>(byte)
                              : '.');
        }
        out += "|\n";
    }
    return out;
}

void ScenarioJwt() {
    go2cpp_encoding_test::Announce("scenario 1: JWT decode (base64url raw)");

    const std::string token =
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2"
        "MjM5MDIyfQ."
        "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";

    std::vector<std::string> segments;
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = token.find('.', start);
        if (dot == std::string::npos) {
            segments.push_back(token.substr(start));
            break;
        }
        segments.push_back(token.substr(start, dot - start));
        start = dot + 1;
    }
    ENCODING_REQUIRE(segments.size() == 3);
    for (const std::string& segment : segments) {
        ENCODING_CHECK(segment.find('=') == std::string::npos);
    }

    const auto header = e::RawURLEncoding.DecodeString(segments[0]);
    ENCODING_REQUIRE(header);
    ENCODING_CHECK_BYTES(header.data, "{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
    const auto payload = e::RawURLEncoding.DecodeString(segments[1]);
    ENCODING_REQUIRE(payload);
    ENCODING_CHECK_BYTES(
        payload.data,
        "{\"sub\":\"1234567890\",\"name\":\"John Doe\",\"iat\":1516239022}");
    const auto signature = e::RawURLEncoding.DecodeString(segments[2]);
    ENCODING_REQUIRE(signature);
    ENCODING_CHECK(signature.data.size() == 32);
    ENCODING_CHECK_BYTES(
        e::Hex::EncodeToString(signature.data),
        "49f94ac7044948c78a285d904f87f0a4c7897f7e8f3a4eb2255fda750b2cc397");

    // 重新编码必须得到原 token 段（验证解码没有丢失尾部比特）。
    ENCODING_CHECK_BYTES(e::RawURLEncoding.EncodeToString(header.data),
                         segments[0]);
    ENCODING_CHECK_BYTES(e::RawURLEncoding.EncodeToString(payload.data),
                         segments[1]);
    ENCODING_CHECK_BYTES(e::RawURLEncoding.EncodeToString(signature.data),
                         segments[2]);

    // JWT 段是 base64url：标准字母表应拒绝含 `-`/`_` 的签名段。
    const auto wrong_alphabet = e::RawStdEncoding.DecodeString(segments[2]);
    ENCODING_CHECK(wrong_alphabet.error != nullptr);
    ENCODING_CHECK(Is(wrong_alphabet.error, e::Base64CorruptInput()));
}

void ScenarioHexDump() {
    go2cpp_encoding_test::Announce("scenario 2: hex dump of a binary file");

    std::string payload;
    payload.resize(512);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(i);
    }
    payload[0] = '\x7f';
    payload[1] = '\x00';
    payload[2] = 'G';
    payload[3] = 'o';

    const std::string path = TempPath("dump.bin");
    ENCODING_REQUIRE(WriteFile(path, payload));

    const std::string on_disk = ReadFile(path);
    ENCODING_CHECK_BYTES(on_disk, payload);
    ENCODING_CHECK(on_disk.size() == 512);

    const std::string dump = HexDump(on_disk);
    std::size_t line_count = 0;
    std::size_t start = 0;
    while (start < dump.size()) {
        const std::size_t newline = dump.find('\n', start);
        ENCODING_REQUIRE(newline != std::string::npos);
        const std::string line = dump.substr(start, newline - start);
        if (line_count == 0) {
            ENCODING_CHECK(line.rfind("00000000  ", 0) == 0);
            ENCODING_CHECK(line.find("7f 00 47 6f") != std::string::npos);
            ENCODING_CHECK(line.find("|") != std::string::npos);
        }
        if (line_count == 1) {
            ENCODING_CHECK(line.rfind("00000010  ", 0) == 0);
        }
        ++line_count;
        start = newline + 1;
    }
    ENCODING_CHECK(line_count == 32);
    ENCODING_CHECK(dump.find("000001f0  ") != std::string::npos);

    // 第 0 行 ASCII 区：0x7f 与 0x00 是 '.'，'G'/'o' 原样。
    const std::size_t first_newline = dump.find('\n');
    const std::string first_line = dump.substr(0, first_newline);
    const std::size_t bar = first_line.find('|');
    ENCODING_REQUIRE(bar != std::string::npos);
    const std::string ascii = first_line.substr(bar + 1, 16);
    ENCODING_CHECK(ascii.size() == 16);
    ENCODING_CHECK(ascii.rfind(".", 0) == 0);
    ENCODING_CHECK(ascii.find("Go") == 2);

    ENCODING_CHECK(std::remove(path.c_str()) == 0);
    std::ifstream check(path);
    ENCODING_CHECK(!check.good());
}

/// 解析一条协议帧；返回错误（`ErrShortBuffer` 表示截断）。
go2cpp::ErrorPtr ParseFrame(std::string_view wire, std::size_t& offset,
                            std::uint16_t& type, std::uint16_t& flags,
                            std::string& payload) {
    const auto magic = e::Binary::Uint32(wire.substr(offset));
    if (!magic) return magic.error;
    if (magic.value != 0x47324350u) {
        return go2cpp::NewError("protocol: bad magic");
    }
    offset += 4;
    const auto version = e::Binary::Uint16(wire.substr(offset));
    if (!version) return version.error;
    if (version.value != 1) {
        return go2cpp::NewError("protocol: unsupported version");
    }
    offset += 2;
    const auto type_value = e::Binary::Uint16(wire.substr(offset));
    if (!type_value) return type_value.error;
    offset += 2;
    const auto flags_value = e::Binary::Uint16(wire.substr(offset));
    if (!flags_value) return flags_value.error;
    offset += 2;
    const auto length = e::Binary::Uint32(wire.substr(offset));
    if (!length) return length.error;
    offset += 4;
    if (length.value > wire.size() - offset) {
        return go2cpp::Wrap(e::ErrShortBuffer(),
                            "protocol: truncated payload");
    }
    type = type_value.value;
    flags = flags_value.value;
    payload.assign(wire.substr(offset, length.value));
    offset += length.value;
    return nullptr;
}

void ScenarioBinaryProtocol() {
    go2cpp_encoding_test::Announce("scenario 3: binary protocol frames");

    struct Frame {
        std::uint16_t type;
        std::uint16_t flags;
        std::string payload;
    };
    const std::vector<Frame> frames = {
        {1, 0, "hello"},
        {2, 0x8000, std::string("\x00\x01\x02", 3)},
        {0x1234, 0x0001, std::string(300, 'x')},
    };

    std::string wire;
    for (const Frame& frame : frames) {
        ENCODING_CHECK(e::Binary::PutUint32(wire, 0x47324350u) == 4);
        ENCODING_CHECK(e::Binary::PutUint16(wire, 1) == 2);
        ENCODING_CHECK(e::Binary::PutUint16(wire, frame.type) == 2);
        ENCODING_CHECK(e::Binary::PutUint16(wire, frame.flags) == 2);
        ENCODING_CHECK(
            e::Binary::PutUint32(wire, static_cast<std::uint32_t>(
                                           frame.payload.size())) == 4);
        wire += frame.payload;
    }

    std::size_t offset = 0;
    for (const Frame& frame : frames) {
        std::uint16_t type = 0;
        std::uint16_t flags = 0;
        std::string payload;
        const auto error = ParseFrame(wire, offset, type, flags, payload);
        ENCODING_CHECK(error == nullptr);
        ENCODING_CHECK(type == frame.type);
        ENCODING_CHECK(flags == frame.flags);
        ENCODING_CHECK_BYTES(payload, frame.payload);
    }
    ENCODING_CHECK(offset == wire.size());

    // 截断在 header 中间：必须是 ErrShortBuffer，而不是越界读取。
    {
        std::size_t cut_offset = 0;
        std::uint16_t type = 0;
        std::uint16_t flags = 0;
        std::string payload;
        const std::string cut = wire.substr(0, 9);
        const auto error = ParseFrame(cut, cut_offset, type, flags, payload);
        ENCODING_CHECK(error != nullptr);
        ENCODING_CHECK(Is(error, e::ErrShortBuffer()));
    }
    // 截断 payload：长度字段声称 300 字节但只给 10 字节。
    {
        std::string truncated;
        e::Binary::PutUint32(truncated, 0x47324350u);
        e::Binary::PutUint16(truncated, 1);
        e::Binary::PutUint16(truncated, 0x1234);
        e::Binary::PutUint16(truncated, 1);
        e::Binary::PutUint32(truncated, 300);
        truncated.append(10, 'x');
        std::size_t cut_offset = 0;
        std::uint16_t type = 0;
        std::uint16_t flags = 0;
        std::string payload;
        const auto error =
            ParseFrame(truncated, cut_offset, type, flags, payload);
        ENCODING_CHECK(error != nullptr);
        ENCODING_CHECK(Is(error, e::ErrShortBuffer()));
    }
    // 错误 magic 必须被拒绝。
    {
        std::string bad;
        e::Binary::PutUint32(bad, 0xdeadbeefu);
        e::Binary::PutUint16(bad, 1);
        std::size_t cut_offset = 0;
        std::uint16_t type = 0;
        std::uint16_t flags = 0;
        std::string payload;
        const auto error = ParseFrame(bad, cut_offset, type, flags, payload);
        ENCODING_CHECK(error != nullptr);
        ENCODING_CHECK(!Is(error, e::ErrShortBuffer()));
    }
}

void ScenarioCsvExport() {
    go2cpp_encoding_test::Announce("scenario 4: CSV export with hard fields");

    const std::vector<std::vector<std::string>> rows = {
        {"id", "product", "note", "price"},
        {"1", "Widget, large", "He said \"hi\"", "9.99"},
        {"2", "Multi\nline", "line1\nline2", "0.50"},
        {"3", " padded", "", "1.00"},
        {"4", "plain", "semi;colon", "0.01"},
    };

    e::CsvWriter writer;
    for (const auto& row : rows) {
        ENCODING_CHECK(writer.Write(row) == nullptr);
    }
    const std::string csv = writer.Str();
    ENCODING_CHECK(csv.find("id,product,note,price\n") == 0);
    ENCODING_CHECK(csv.find("\"Widget, large\"") != std::string::npos);
    ENCODING_CHECK(csv.find("\"He said \"\"hi\"\"\"") != std::string::npos);
    ENCODING_CHECK(csv.find("\"Multi\nline\"") != std::string::npos);
    ENCODING_CHECK(csv.find("\" padded\"") != std::string::npos);

    const std::string path = TempPath("export.csv");
    ENCODING_REQUIRE(WriteFile(path, csv));
    const std::string on_disk = ReadFile(path);
    ENCODING_CHECK_BYTES(on_disk, csv);

    // 回读并按 Go 默认的 FieldsPerRecord 推断（首条记录 4 列）校验。
    e::CsvReader reader(on_disk);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto result = reader.Read();
        ENCODING_REQUIRE(!result.eof);
        ENCODING_REQUIRE(result.error == nullptr);
        ENCODING_CHECK(result.record == rows[i]);
    }
    const auto end = reader.Read();
    ENCODING_CHECK(end.eof);
    ENCODING_CHECK(reader.FieldsPerRecord() == 4);

    ENCODING_CHECK(std::remove(path.c_str()) == 0);
    std::ifstream check(path);
    ENCODING_CHECK(!check.good());
}

void ScenarioBase64Image() {
    go2cpp_encoding_test::Announce("scenario 5: base64 image (1 MiB)");

    std::mt19937_64 rng(0x1a2b3c4dULL);
    std::string image = std::string("\x89PNG\r\n\x1a\n", 8);
    image += go2cpp_encoding_test::RandomBytes(rng, 1024 * 1024);
    ENCODING_CHECK(image.size() == 1024 * 1024 + 8);

    const std::string encoded = e::StdEncoding.EncodeToString(image);
    ENCODING_CHECK(encoded.size() == (image.size() + 2) / 3 * 4);
    ENCODING_CHECK(encoded.size() == e::StdEncoding.EncodedLen(image.size()));
    ENCODING_CHECK(encoded.find('\n') == std::string::npos);
    ENCODING_CHECK(encoded.find("data:") == std::string::npos);

    const std::string data_url = "data:image/png;base64," + encoded;
    const std::string prefix = "data:image/png;base64,";
    ENCODING_REQUIRE(data_url.compare(0, prefix.size(), prefix) == 0);
    const auto decoded = e::StdEncoding.DecodeString(
        std::string_view(data_url).substr(prefix.size()));
    ENCODING_REQUIRE(decoded);
    ENCODING_CHECK_BYTES(decoded.data, image);
    ENCODING_CHECK(decoded.data.size() == image.size());

    // URL 安全变体同样可往返，且不含 `+` / `/`。
    const std::string url_encoded = e::URLEncoding.EncodeToString(image);
    ENCODING_CHECK(url_encoded.find('+') == std::string::npos);
    ENCODING_CHECK(url_encoded.find('/') == std::string::npos);
    const auto url_decoded = e::URLEncoding.DecodeString(url_encoded);
    ENCODING_REQUIRE(url_decoded);
    ENCODING_CHECK_BYTES(url_decoded.data, image);

    std::printf("    raw=%zu bytes -> base64=%zu bytes (ratio %.3f)\n",
                image.size(), encoded.size(),
                static_cast<double>(encoded.size()) /
                    static_cast<double>(image.size()));
}

void ScenarioLeb128Stream() {
    go2cpp_encoding_test::Announce("scenario 6: LEB128 time-series stream");

    struct Sample {
        std::uint32_t id;
        std::int64_t delta;  // 有符号增量，可能为负
        std::string label;
    };
    std::mt19937_64 rng(0x1eb128);
    std::vector<Sample> samples;
    for (std::uint32_t i = 0; i < 200; ++i) {
        Sample sample;
        sample.id = i;
        sample.delta = static_cast<std::int64_t>(rng() % 2001) - 1000;
        if (i % 37 == 0) {
            sample.delta = -1;  // 边界：zig-zag 最小宽度
        }
        const std::size_t label_length = static_cast<std::size_t>(rng() % 24);
        sample.label = go2cpp_encoding_test::RandomBytes(rng, label_length);
        samples.push_back(std::move(sample));
    }

    std::string wire;
    for (const Sample& sample : samples) {
        e::Binary::PutUint32(wire, sample.id);
        e::Binary::PutVarint(wire, sample.delta);
        e::Binary::PutUvarint(wire, sample.label.size());
        wire += sample.label;
    }

    std::size_t offset = 0;
    for (const Sample& sample : samples) {
        const auto id = e::Binary::Uint32(wire.substr(offset));
        ENCODING_REQUIRE(id);
        offset += 4;
        const auto delta = e::Binary::Varint(wire.substr(offset));
        ENCODING_REQUIRE(delta);
        offset += delta.bytes_read;
        const auto length = e::Binary::Uvarint(wire.substr(offset));
        ENCODING_REQUIRE(length);
        offset += length.bytes_read;
        ENCODING_REQUIRE(offset + length.value <= wire.size());
        const std::string label = wire.substr(offset, length.value);
        offset += length.value;

        ENCODING_CHECK(id.value == sample.id);
        ENCODING_CHECK(delta.value == sample.delta);
        ENCODING_CHECK_BYTES(label, sample.label);
    }
    ENCODING_CHECK(offset == wire.size());

    // 截断在 varint 中间：报截断错误，且不越界。
    {
        std::string truncated;
        e::Binary::PutUint32(truncated, 7);
        truncated.push_back(static_cast<char>(0x80));  // 续位，缺少后续字节
        const auto delta = e::Binary::Varint(std::string_view(truncated).substr(4));
        ENCODING_CHECK(delta.error != nullptr);
        ENCODING_CHECK(Is(delta.error, e::ErrUvarintTruncated()));
    }

    std::printf("    %zu samples, %zu wire bytes (uvarint+varint+BE id)\n",
                samples.size(), wire.size());
}

}  // namespace

void RunScenarioTests() {
    ScenarioJwt();
    ScenarioHexDump();
    ScenarioBinaryProtocol();
    ScenarioCsvExport();
    ScenarioBase64Image();
    ScenarioLeb128Stream();
}
