/**
 * @file test_random.cpp
 * @brief `encoding` 模块的随机测试与 Python 对拍。
 *
 * 覆盖（固定种子，可复现）：
 *   - 随机字节（0 ~ 1 MiB）在四种 base64 编码下的往返；
 *   - 随机字节的 hex 往返（含大写解码）；
 *   - `tests/encoding/testdata/python_vectors.txt`：由 Python
 *     `base64.b64encode` / `binascii.hexlify` 生成的期望值，逐条对拍；
 *   - Binary 定长/变长随机往返（含任意浮点位模式）；
 *   - CSV 随机字段（逗号、引号、换行、空白、多字节）Writer → Reader 往返；
 *   - 随机非法输入的健壮性（只允许返回错误锚点，不允许崩溃）。
 */

#include "encoding_test_support.hpp"

#include "go2cpp/encoding/base64.hpp"
#include "go2cpp/encoding/binary.hpp"
#include "go2cpp/encoding/csv.hpp"
#include "go2cpp/encoding/hex.hpp"
#include "go2cpp/error.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace e = go2cpp::encoding;

using go2cpp::ErrorMessage;
using go2cpp::Is;

namespace {

const e::Base64Encoding* const kEncodings[] = {
    &e::StdEncoding, &e::URLEncoding, &e::RawStdEncoding, &e::RawURLEncoding,
};
const char* const kEncodingNames[] = {"StdEncoding", "URLEncoding",
                                      "RawStdEncoding", "RawURLEncoding"};

/// 把标准 base64 输出翻译成 URL 安全字母表（仅差两个字符）。
std::string ToURLAlphabet(std::string text) {
    for (char& c : text) {
        if (c == '+') {
            c = '-';
        } else if (c == '/') {
            c = '_';
        }
    }
    return text;
}

/// 把 URL 安全 base64 输出翻译回标准字母表。
std::string FromURLAlphabet(std::string text) {
    for (char& c : text) {
        if (c == '-') {
            c = '+';
        } else if (c == '_') {
            c = '/';
        }
    }
    return text;
}

std::uint64_t DoubleBits(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint32_t FloatBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// 按 `\t` 切分，保留空字段。
std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return parts;
}

void RunPythonVectors() {
    go2cpp_encoding_test::Announce("random: Python reference vectors");

    const std::string path =
        go2cpp_encoding_test::TestdataPath("python_vectors.txt");
    std::ifstream input(path);
    ENCODING_REQUIRE(input.good());
    std::printf("    reading %s\n", path.c_str());

    std::size_t lines = 0;
    std::size_t cases = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        ++lines;
        const std::vector<std::string> parts = SplitTabs(line);
        ENCODING_REQUIRE(parts.size() == 3);
        const std::string& kind = parts[0];
        std::string bytes;
        ENCODING_REQUIRE(go2cpp_encoding_test::FromHex(parts[1], bytes));
        const std::string& expected = parts[2];

        bool matched = false;
        if (kind == "b64std") {
            ENCODING_CHECK_BYTES(e::StdEncoding.EncodeToString(bytes), expected);
            const auto decoded = e::StdEncoding.DecodeString(expected);
            ENCODING_REQUIRE(decoded);
            ENCODING_CHECK_BYTES(decoded.data, bytes);
            matched = true;
        } else if (kind == "b64url") {
            ENCODING_CHECK_BYTES(e::URLEncoding.EncodeToString(bytes), expected);
            const auto decoded = e::URLEncoding.DecodeString(expected);
            ENCODING_REQUIRE(decoded);
            ENCODING_CHECK_BYTES(decoded.data, bytes);
            matched = true;
        } else if (kind == "b64rawstd") {
            ENCODING_CHECK_BYTES(e::RawStdEncoding.EncodeToString(bytes), expected);
            const auto decoded = e::RawStdEncoding.DecodeString(expected);
            ENCODING_REQUIRE(decoded);
            ENCODING_CHECK_BYTES(decoded.data, bytes);
            matched = true;
        } else if (kind == "b64rawurl") {
            ENCODING_CHECK_BYTES(e::RawURLEncoding.EncodeToString(bytes), expected);
            const auto decoded = e::RawURLEncoding.DecodeString(expected);
            ENCODING_REQUIRE(decoded);
            ENCODING_CHECK_BYTES(decoded.data, bytes);
            matched = true;
        } else if (kind == "hex") {
            ENCODING_CHECK_BYTES(e::Hex::EncodeToString(bytes), expected);
            const auto decoded = e::Hex::DecodeString(expected);
            ENCODING_REQUIRE(decoded);
            ENCODING_CHECK_BYTES(decoded.data, bytes);
            matched = true;
        }
        ENCODING_CHECK(matched);
        if (!matched) {
            std::fprintf(stderr, "    unknown vector kind: %s\n", kind.c_str());
        }
        ++cases;
    }
    ENCODING_REQUIRE(lines > 0);
    std::printf("    verified %zu Python vectors over %zu lines\n", cases, lines);
}

void RunBase64Random() {
    go2cpp_encoding_test::Announce("random: base64 round-trip");

    std::mt19937_64 rng(20261018);
    const std::size_t sizes[] = {0,    1,    2,    3,     4,     5,
                                 6,    7,    8,    9,     15,    16,
                                 17,   31,   32,   33,    63,    64,
                                 65,   127,  128,  129,   255,   256,
                                 257,  1023, 1024, 1025,  65535, 100000};
    for (const std::size_t size : sizes) {
        const std::string bytes = go2cpp_encoding_test::RandomBytes(rng, size);
        for (std::size_t i = 0; i < 4; ++i) {
            const e::Base64Encoding* encoding = kEncodings[i];
            const std::string encoded = encoding->EncodeToString(bytes);
            ENCODING_CHECK(encoded.size() == encoding->EncodedLen(bytes.size()));
            if (encoding->IsRaw()) {
                ENCODING_CHECK(encoded.find('=') == std::string::npos);
            }
            const auto decoded = encoding->DecodeString(encoded);
            ENCODING_REQUIRE(decoded);
            if (decoded.data != bytes) {
                ENCODING_CHECK(false);
                std::fprintf(stderr, "    %s failed at size %zu\n",
                             kEncodingNames[i], size);
            }
        }
        // 标准与 URL 字母表只差 `+/` ↔ `-_`，填充完全相同。
        const std::string std_encoded = e::StdEncoding.EncodeToString(bytes);
        const std::string url_encoded = e::URLEncoding.EncodeToString(bytes);
        ENCODING_CHECK_BYTES(ToURLAlphabet(std_encoded), url_encoded);
        ENCODING_CHECK_BYTES(FromURLAlphabet(url_encoded), std_encoded);
        ENCODING_CHECK(std_encoded.size() == url_encoded.size());
    }

    // 1 MiB 随机往返（三种长度余数都覆盖）。
    for (const std::size_t size : {std::size_t{1024 * 1024}, std::size_t{1024 * 1024 + 1},
                                   std::size_t{1024 * 1024 + 2}}) {
        const std::string bytes = go2cpp_encoding_test::RandomBytes(rng, size);
        const auto decoded = e::StdEncoding.DecodeString(
            e::StdEncoding.EncodeToString(bytes));
        ENCODING_REQUIRE(decoded);
        ENCODING_CHECK_BYTES(decoded.data, bytes);
    }
}

void RunHexRandom() {
    go2cpp_encoding_test::Announce("random: hex round-trip");

    std::mt19937_64 rng(0x5eed2026);
    for (int iteration = 0; iteration < 128; ++iteration) {
        const std::size_t size = static_cast<std::size_t>(rng() % 4096);
        const std::string bytes = go2cpp_encoding_test::RandomBytes(rng, size);
        const std::string encoded = e::Hex::EncodeToString(bytes);
        ENCODING_CHECK(encoded.size() == size * 2);
        const auto decoded = e::Hex::DecodeString(encoded);
        ENCODING_REQUIRE(decoded);
        ENCODING_CHECK_BYTES(decoded.data, bytes);

        std::string upper = encoded;
        for (char& c : upper) {
            if (c >= 'a' && c <= 'f') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        const auto upper_decoded = e::Hex::DecodeString(upper);
        ENCODING_REQUIRE(upper_decoded);
        ENCODING_CHECK_BYTES(upper_decoded.data, bytes);
    }
}

void RunBinaryRandom() {
    go2cpp_encoding_test::Announce("random: binary round-trip");

    std::mt19937_64 rng(0xb16b00b5);
    const e::Binary::Order orders[] = {e::Binary::BigEndian,
                                       e::Binary::LittleEndian};
    for (int iteration = 0; iteration < 20000; ++iteration) {
        const std::uint64_t value = rng();
        for (const e::Binary::Order order : orders) {
            std::string buffer;
            ENCODING_CHECK(e::Binary::PutUint64(buffer, value, order) == 8);
            ENCODING_CHECK(e::Binary::Uint64(buffer, order).value == value);
            ENCODING_CHECK(e::Binary::PutUint32(
                               buffer, static_cast<std::uint32_t>(value), order) ==
                           4);
            ENCODING_CHECK(e::Binary::Uint32(buffer.substr(8), order).value ==
                           static_cast<std::uint32_t>(value));
            ENCODING_CHECK(e::Binary::PutUint16(
                               buffer, static_cast<std::uint16_t>(value), order) ==
                           2);
            ENCODING_CHECK(e::Binary::Uint16(buffer.substr(12), order).value ==
                           static_cast<std::uint16_t>(value));

            const std::int64_t signed_value =
                static_cast<std::int64_t>(value);  // 位模式保留
            buffer.clear();
            ENCODING_CHECK(e::Binary::PutInt64(buffer, signed_value, order) == 8);
            ENCODING_CHECK(e::Binary::Int64(buffer, order).value == signed_value);
            ENCODING_CHECK(e::Binary::PutInt32(
                               buffer,
                               static_cast<std::int32_t>(signed_value), order) ==
                           4);
            ENCODING_CHECK(e::Binary::Int32(buffer.substr(8), order).value ==
                           static_cast<std::int32_t>(signed_value));

            // 任意位模式（含 NaN / 次正规数）必须逐位往返。
            const std::uint32_t float_bits = static_cast<std::uint32_t>(value);
            float f = 0.0f;
            std::memcpy(&f, &float_bits, sizeof(f));
            buffer.clear();
            ENCODING_CHECK(e::Binary::PutFloat32(buffer, f, order) == 4);
            ENCODING_CHECK(FloatBits(e::Binary::Float32(buffer, order).value) ==
                           float_bits);

            double d = 0.0;
            std::memcpy(&d, &value, sizeof(d));
            buffer.clear();
            ENCODING_CHECK(e::Binary::PutFloat64(buffer, d, order) == 8);
            ENCODING_CHECK(DoubleBits(e::Binary::Float64(buffer, order).value) ==
                           value);
        }
    }

    go2cpp_encoding_test::Announce("random: varint round-trip");

    const std::uint64_t uvarint_edges[] = {
        0,          1,          2,          126,        127,        128,
        129,        16383,      16384,      16385,      0xffffffu,  0x1000000u,
        0xffffffffull, 0x100000000ull, 0x7fffffffffffffffull,
        0xffffffffffffffffull};
    for (const std::uint64_t value : uvarint_edges) {
        std::string encoded;
        e::Binary::PutUvarint(encoded, value);
        const auto decoded = e::Binary::Uvarint(encoded);
        ENCODING_REQUIRE(decoded);
        ENCODING_CHECK(decoded.value == value);
        ENCODING_CHECK(decoded.bytes_read == encoded.size());
        ENCODING_CHECK(encoded.size() <= 10);
    }
    const std::int64_t varint_edges[] = {
        0,          1,          -1,         62,        63,        64,
        -62,        -63,        -64,        -65,       0x7ffffff, -0x8000000,
        std::numeric_limits<std::int64_t>::max(),
        std::numeric_limits<std::int64_t>::min()};
    for (const std::int64_t value : varint_edges) {
        std::string encoded;
        e::Binary::PutVarint(encoded, value);
        const auto decoded = e::Binary::Varint(encoded);
        ENCODING_REQUIRE(decoded);
        ENCODING_CHECK(decoded.value == value);
        ENCODING_CHECK(decoded.bytes_read == encoded.size());
        ENCODING_CHECK(encoded.size() <= 10);
    }
    for (int iteration = 0; iteration < 20000; ++iteration) {
        const std::uint64_t value = rng();
        std::string encoded;
        e::Binary::PutUvarint(encoded, value);
        const auto decoded = e::Binary::Uvarint(encoded);
        ENCODING_REQUIRE(decoded);
        ENCODING_CHECK(decoded.value == value);
        ENCODING_CHECK(decoded.bytes_read == encoded.size());

        const std::int64_t signed_value = static_cast<std::int64_t>(value);
        std::string signed_encoded;
        e::Binary::PutVarint(signed_encoded, signed_value);
        const auto signed_decoded = e::Binary::Varint(signed_encoded);
        ENCODING_REQUIRE(signed_decoded);
        ENCODING_CHECK(signed_decoded.value == signed_value);
        ENCODING_CHECK(signed_decoded.bytes_read == signed_encoded.size());

        // 追加语义：在已有数据后面编码，再从偏移处解码。
        std::string buffer = "prefix";
        e::Binary::PutUvarint(buffer, value);
        const auto offset_decoded =
            e::Binary::Uvarint(std::string_view(buffer).substr(6));
        ENCODING_REQUIRE(offset_decoded);
        ENCODING_CHECK(offset_decoded.value == value);
    }
}

void RunCsvRandom() {
    go2cpp_encoding_test::Announce("random: csv round-trip");

    std::mt19937_64 rng(0xc5f2026);
    const char pool[] = {'a', 'b', 'c', '0', '1', ',', '"', '\n', ' ', '\t',
                         ';', '|', '\\', '.', '-', '\xc2', '\xa0'};
    for (int iteration = 0; iteration < 300; ++iteration) {
        // 字段数至少为 1：0 字段记录的文本是空行，Reader 会按 Go 语义跳过它。
        const std::size_t field_count = 1 + static_cast<std::size_t>(rng() % 8);
        std::vector<std::vector<std::string>> rows;
        const std::size_t row_count = 1 + static_cast<std::size_t>(rng() % 6);
        for (std::size_t row = 0; row < row_count; ++row) {
            std::vector<std::string> fields;
            for (std::size_t field = 0; field < field_count; ++field) {
                const std::size_t length = static_cast<std::size_t>(rng() % 12);
                std::string text;
                for (std::size_t k = 0; k < length; ++k) {
                    text.push_back(pool[rng() % sizeof(pool)]);
                }
                fields.push_back(std::move(text));
            }
            // Go 的 Writer 把「只有一个空字段」写成空行，而 Reader 会跳过空行；
            // 这是 Go 自身的语义（不是本模块差异），随机往返时避开该组合。
            if (fields.size() == 1 && fields[0].empty()) {
                fields[0] = "x";
            }
            rows.push_back(std::move(fields));
        }

        e::CsvWriter writer;
        for (const auto& row : rows) {
            ENCODING_CHECK(writer.Write(row) == nullptr);
        }
        const std::string text = writer.Str();

        e::CsvReader reader(text);
        ENCODING_REQUIRE(reader.SetFieldsPerRecord(-1));
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const auto result = reader.Read();
            ENCODING_REQUIRE(!result.eof);
            ENCODING_REQUIRE(result.error == nullptr);
            if (result.record != rows[row]) {
                ENCODING_CHECK(false);
                std::fprintf(stderr,
                             "    csv round-trip failed at iteration %d row %zu\n",
                             iteration, row);
            }
        }
        ENCODING_CHECK(reader.Read().eof);
    }

    // 单字段空串：Go 的 Writer 写成空行，而 Reader 跳过空行 —— 读回是 EOF。
    // 这是 Go 自身的语义（不是本模块差异），需要真正的单空字段时用 `""`。
    {
        e::CsvWriter writer;
        ENCODING_CHECK(writer.Write({""}) == nullptr);
        ENCODING_CHECK(writer.Str() == "\n");
        e::CsvReader reader(writer.Str());
        ENCODING_CHECK(reader.Read().eof);

        e::CsvReader quoted(std::string("\"\"\n"));
        const auto result = quoted.Read();
        ENCODING_REQUIRE(result);
        ENCODING_CHECK(result.record.size() == 1);
        ENCODING_CHECK(result.record[0].empty());
    }
}

void RunFuzzRobustness() {
    go2cpp_encoding_test::Announce("random: malformed input robustness");

    std::mt19937_64 rng(0xdead2026);
    const std::string pool =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
        "+/=-_ \t\r\n!@#$%^&*()[]{}";
    for (int iteration = 0; iteration < 20000; ++iteration) {
        const std::size_t length = static_cast<std::size_t>(rng() % 40);
        std::string text;
        for (std::size_t k = 0; k < length; ++k) {
            text.push_back(pool[rng() % pool.size()]);
        }

        const auto b64 = e::StdEncoding.DecodeString(text);
        if (b64.error) {
            ENCODING_CHECK(Is(b64.error, e::Base64CorruptInput()));
            ENCODING_CHECK(b64.data.empty());
        }
        const auto b64raw = e::RawURLEncoding.DecodeString(text);
        if (b64raw.error) {
            ENCODING_CHECK(Is(b64raw.error, e::Base64CorruptInput()));
            ENCODING_CHECK(b64raw.data.empty());
        }

        const auto hex = e::Hex::DecodeString(text);
        if (hex.error) {
            ENCODING_CHECK(Is(hex.error, e::HexOddLength()) ||
                           Is(hex.error, e::HexInvalidByte()));
            ENCODING_CHECK(hex.data.empty());
        } else {
            ENCODING_CHECK(hex.data.size() == text.size() / 2);
        }

        const auto uvarint = e::Binary::Uvarint(text);
        if (uvarint.error) {
            ENCODING_CHECK(Is(uvarint.error, e::ErrUvarintOverflow()) ||
                           Is(uvarint.error, e::ErrUvarintTruncated()));
        } else {
            ENCODING_CHECK(uvarint.bytes_read >= 1);
            ENCODING_CHECK(uvarint.bytes_read <= text.size());
        }

        // CSV Reader 对任意文本都不得崩溃；成功 / EOF / 三类错误之一。
        e::CsvReader reader(text);
        ENCODING_REQUIRE(reader.SetFieldsPerRecord(-1));
        for (int record = 0; record < 8; ++record) {
            const auto parsed = reader.Read();
            if (parsed.eof) {
                break;
            }
            if (parsed.error) {
                ENCODING_CHECK(Is(parsed.error, e::CsvErrBareQuote()) ||
                               Is(parsed.error, e::CsvErrQuote()));
                break;
            }
        }
    }

    // 超长输入边界：1 MiB 非法字符只报错，不崩溃。
    {
        const std::string long_invalid(1024 * 1024, '!');
        const auto result = e::StdEncoding.DecodeString(long_invalid);
        ENCODING_CHECK(result.error != nullptr);
        ENCODING_CHECK(Is(result.error, e::Base64CorruptInput()));
        const auto hex_result = e::Hex::DecodeString(long_invalid);
        ENCODING_CHECK(hex_result.error != nullptr);
        ENCODING_CHECK(Is(hex_result.error, e::HexInvalidByte()));
    }
}

}  // namespace

void RunRandomTests() {
    RunPythonVectors();
    RunBase64Random();
    RunHexRandom();
    RunBinaryRandom();
    RunCsvRandom();
    RunFuzzRobustness();
}
