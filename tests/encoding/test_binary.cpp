/**
 * @file test_binary.cpp
 * @brief `encoding::Binary` 的单元测试：大小端定长读写、短输入边界、LEB128 边界。
 *
 * 覆盖：
 *   - `Uint16/32/64`、`Int16/32/64`、`Float32/64` 的大端/小端已知向量；
 *   - 每个读取函数在 `0 .. width-1` 字节输入下都返回 `ErrShortBuffer()` 且不越界；
 *   - 写入函数的追加语义与返回长度；
 *   - `Uvarint`/`Varint` 的 Go 已知编码、10 字节边界、溢出与截断；
 *   - 浮点 NaN 位模式原样往返。
 */

#include "encoding_test_support.hpp"

#include "go2cpp/encoding/binary.hpp"
#include "go2cpp/error.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

namespace e = go2cpp::encoding;

using go2cpp::ErrorMessage;
using go2cpp::Is;
using Order = e::Binary::Order;

namespace {

std::string Bytes(std::initializer_list<unsigned int> values) {
    std::string out;
    out.reserve(values.size());
    for (const unsigned int value : values) {
        out.push_back(static_cast<char>(value & 0xff));
    }
    return out;
}

/// 把测试里用十六进制书写的期望值转成实际字节串。
std::string ExpectedBytes(const char* hex) {
    std::string out;
    ENCODING_REQUIRE(go2cpp_encoding_test::FromHex(hex, out));
    return out;
}

std::uint64_t DoubleBits(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void ExpectShort(std::string_view input, const char* what) {
    const std::size_t size = input.size();
    const auto check = [size, what](const go2cpp::ErrorPtr& error,
                                    std::size_t width) {
        if (size < width) {
            if (error == nullptr || !Is(error, e::ErrShortBuffer())) {
                ENCODING_CHECK(false);
                std::fprintf(stderr, "    short input %s width %zu: %s\n", what,
                             width,
                             error == nullptr ? "missing error"
                                              : ErrorMessage(error).c_str());
            }
        } else {
            ENCODING_CHECK(error == nullptr);
        }
    };
    check(e::Binary::Uint16(input).error, 2);
    check(e::Binary::Uint32(input).error, 4);
    check(e::Binary::Uint64(input).error, 8);
    check(e::Binary::Int16(input).error, 2);
    check(e::Binary::Int32(input).error, 4);
    check(e::Binary::Int64(input).error, 8);
    check(e::Binary::Float32(input).error, 4);
    check(e::Binary::Float64(input).error, 8);
}

void ExpectUvarintOverflow(std::string_view input, const char* what) {
    const auto result = e::Binary::Uvarint(input);
    ENCODING_CHECK(result.error != nullptr);
    ENCODING_CHECK(Is(result.error, e::ErrUvarintOverflow()));
    ENCODING_CHECK(result.value == 0);
    ENCODING_CHECK(result.bytes_read == 0);
    std::printf("    rejected %-24s (%s)\n", what,
                ErrorMessage(result.error).c_str());
}

void ExpectUvarintTruncated(std::string_view input, const char* what) {
    const auto result = e::Binary::Uvarint(input);
    ENCODING_CHECK(result.error != nullptr);
    ENCODING_CHECK(Is(result.error, e::ErrUvarintTruncated()));
    ENCODING_CHECK(result.value == 0);
    ENCODING_CHECK(result.bytes_read == 0);
    std::printf("    rejected %-24s (%s)\n", what,
                ErrorMessage(result.error).c_str());
}

}  // namespace

void RunBinaryTests() {
    go2cpp_encoding_test::Announce("binary: big/little endian known vectors");

    {
        const std::string big16 = Bytes({0x12, 0x34});
        const std::string little16 = Bytes({0x34, 0x12});
        ENCODING_CHECK(e::Binary::Uint16(big16).value == 0x1234);
        ENCODING_CHECK(e::Binary::Uint16(little16, Order::kLittleEndian).value ==
                       0x1234);
        // 同一份字节在另一种字节序下必然得到交换后的值。
        ENCODING_CHECK(e::Binary::Uint16(big16, Order::kLittleEndian).value ==
                       0x3412);

        std::string out;
        ENCODING_CHECK(e::Binary::PutUint16(out, 0x1234) == 2);
        ENCODING_CHECK_BYTES(out, big16);
        out.clear();
        ENCODING_CHECK(e::Binary::PutUint16(out, 0x1234, Order::kLittleEndian) == 2);
        ENCODING_CHECK_BYTES(out, little16);
    }

    {
        const std::string big32 = Bytes({0x01, 0x02, 0x03, 0x04});
        const std::string little32 = Bytes({0x04, 0x03, 0x02, 0x01});
        ENCODING_CHECK(e::Binary::Uint32(big32).value == 0x01020304u);
        ENCODING_CHECK(
            e::Binary::Uint32(little32, Order::kLittleEndian).value ==
            0x01020304u);
        std::string out;
        ENCODING_CHECK(e::Binary::PutUint32(out, 0x01020304u) == 4);
        ENCODING_CHECK_BYTES(out, big32);
        out.clear();
        ENCODING_CHECK(
            e::Binary::PutUint32(out, 0x01020304u, Order::kLittleEndian) == 4);
        ENCODING_CHECK_BYTES(out, little32);
    }

    {
        const std::string big64 =
            Bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
        const std::string little64 =
            Bytes({0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01});
        ENCODING_CHECK(e::Binary::Uint64(big64).value ==
                       0x0102030405060708ull);
        ENCODING_CHECK(e::Binary::Uint64(little64, Order::kLittleEndian).value ==
                       0x0102030405060708ull);
        std::string out;
        ENCODING_CHECK(e::Binary::PutUint64(out, 0x0102030405060708ull) == 8);
        ENCODING_CHECK_BYTES(out, big64);
        out.clear();
        ENCODING_CHECK(e::Binary::PutUint64(out, 0x0102030405060708ull,
                                            Order::kLittleEndian) == 8);
        ENCODING_CHECK_BYTES(out, little64);
        // 默认参数就是大端。
        ENCODING_CHECK(e::Binary::Uint64(big64, e::Binary::BigEndian).value ==
                       e::Binary::Uint64(big64).value);
    }

    go2cpp_encoding_test::Announce("binary: signed integers (two's complement)");

    {
        const std::string minus_one16 = Bytes({0xff, 0xff});
        ENCODING_CHECK(e::Binary::Int16(minus_one16).value == -1);
        const std::string minus_two16 = Bytes({0xff, 0xfe});
        ENCODING_CHECK(e::Binary::Int16(minus_two16).value == -2);
        const std::string min16 = Bytes({0x80, 0x00});
        ENCODING_CHECK(e::Binary::Int16(min16).value ==
                       std::numeric_limits<std::int16_t>::min());

        const std::string min32 = Bytes({0x80, 0x00, 0x00, 0x00});
        ENCODING_CHECK(e::Binary::Int32(min32).value ==
                       std::numeric_limits<std::int32_t>::min());
        const std::string max32 = Bytes({0x7f, 0xff, 0xff, 0xff});
        ENCODING_CHECK(e::Binary::Int32(max32).value ==
                       std::numeric_limits<std::int32_t>::max());

        const std::string min64 =
            Bytes({0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        ENCODING_CHECK(e::Binary::Int64(min64).value ==
                       std::numeric_limits<std::int64_t>::min());

        std::string out;
        ENCODING_CHECK(e::Binary::PutInt16(out, -1) == 2);
        ENCODING_CHECK_BYTES(out, minus_one16);
        out.clear();
        ENCODING_CHECK(e::Binary::PutInt32(
                           out, std::numeric_limits<std::int32_t>::min()) == 4);
        ENCODING_CHECK_BYTES(out, min32);
        out.clear();
        ENCODING_CHECK(e::Binary::PutInt64(
                           out, std::numeric_limits<std::int64_t>::min()) == 8);
        ENCODING_CHECK_BYTES(out, min64);
    }

    go2cpp_encoding_test::Announce("binary: IEEE-754 floats");

    {
        const std::string one32 = Bytes({0x3f, 0x80, 0x00, 0x00});
        ENCODING_CHECK(e::Binary::Float32(one32).value == 1.0f);
        const std::string minus_two_point_five =
            Bytes({0xc0, 0x20, 0x00, 0x00});
        ENCODING_CHECK(e::Binary::Float32(minus_two_point_five).value == -2.5f);

        std::string out;
        ENCODING_CHECK(e::Binary::PutFloat32(out, 1.0f) == 4);
        ENCODING_CHECK_BYTES(out, one32);
        out.clear();
        ENCODING_CHECK(e::Binary::PutFloat32(out, -2.5f) == 4);
        ENCODING_CHECK_BYTES(out, minus_two_point_five);

        const std::string one64 =
            Bytes({0x3f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        ENCODING_CHECK(e::Binary::Float64(one64).value == 1.0);
        const std::string minus_zero64 =
            Bytes({0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        const double minus_zero = e::Binary::Float64(minus_zero64).value;
        ENCODING_CHECK(minus_zero == 0.0);
        ENCODING_CHECK(DoubleBits(minus_zero) == 0x8000000000000000ull);

        // NaN payload 必须原样保留（不能归一化成 quiet NaN 的固定 payload）。
        const std::string nan64 =
            Bytes({0x7f, 0xf8, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc});
        const double nan_value = e::Binary::Float64(nan64).value;
        ENCODING_CHECK(nan_value != nan_value);
        ENCODING_CHECK(DoubleBits(nan_value) == 0x7ff8123456789abcull);

        out.clear();
        ENCODING_CHECK(e::Binary::PutFloat64(out, nan_value) == 8);
        ENCODING_CHECK_BYTES(out, nan64);
    }

    go2cpp_encoding_test::Announce("binary: short input never reads out of bounds");

    ExpectShort("", "empty");
    ExpectShort(Bytes({0x01}), "1 byte");
    ExpectShort(Bytes({0x01, 0x02}), "2 bytes");
    ExpectShort(Bytes({0x01, 0x02, 0x03}), "3 bytes");
    ExpectShort(Bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}), "7 bytes");
    {
        const auto error = e::Binary::Uint64(Bytes({1, 2, 3})).error;
        ENCODING_REQUIRE(error != nullptr);
        ENCODING_CHECK(ErrorMessage(error).find("need 8 bytes") !=
                       std::string::npos);
        ENCODING_CHECK(ErrorMessage(error).find("got 3") != std::string::npos);
    }

    go2cpp_encoding_test::Announce("binary: uvarint known vectors");

    struct VarintVector {
        std::uint64_t value;
        const char* encoded;  // 以十六进制书写，便于肉眼核对
    };
    const VarintVector uvarint_vectors[] = {
        {0, "00"},
        {1, "01"},
        {127, "7f"},
        {128, "8001"},
        {300, "ac02"},
        {16383, "ff7f"},
        {16384, "808001"},
        {0xffffffffull, "ffffffff0f"},
        {0xffffffffffffffffull, "ffffffffffffffffff01"},
    };
    for (const VarintVector& vector : uvarint_vectors) {
        std::string encoded;
        const std::size_t written = e::Binary::PutUvarint(encoded, vector.value);
        ENCODING_CHECK_BYTES(encoded, ExpectedBytes(vector.encoded));
        ENCODING_CHECK(written == encoded.size());

        const auto decoded = e::Binary::Uvarint(encoded);
        ENCODING_REQUIRE(decoded);
        ENCODING_CHECK(decoded.value == vector.value);
        ENCODING_CHECK(decoded.bytes_read == encoded.size());

        // 尾部多余字节不应影响解码（Go 返回消耗的字节数）。
        std::string with_tail = encoded + "junk";
        const auto tail_decoded = e::Binary::Uvarint(with_tail);
        ENCODING_REQUIRE(tail_decoded);
        ENCODING_CHECK(tail_decoded.value == vector.value);
        ENCODING_CHECK(tail_decoded.bytes_read == encoded.size());
    }
    // 最大 uint64 必须是 10 字节。
    {
        std::string encoded;
        e::Binary::PutUvarint(encoded,
                              std::numeric_limits<std::uint64_t>::max());
        ENCODING_CHECK(encoded.size() == 10);
        ENCODING_CHECK(encoded.back() == '\x01');
    }

    go2cpp_encoding_test::Announce("binary: varint zig-zag vectors");

    const VarintVector varint_vectors[] = {
        {0, "00"},
        {1, "02"},
        {2, "04"},
        {63, "7e"},
        {64, "8001"},
        {0x7fffffffffffffffull, "feffffffffffffffff01"},
    };
    for (const VarintVector& vector : varint_vectors) {
        const std::int64_t value = static_cast<std::int64_t>(vector.value);
        std::string encoded;
        const std::size_t written = e::Binary::PutVarint(encoded, value);
        ENCODING_CHECK_BYTES(encoded, ExpectedBytes(vector.encoded));
        ENCODING_CHECK(written == encoded.size());
        const auto decoded = e::Binary::Varint(encoded);
        ENCODING_REQUIRE(decoded);
        ENCODING_CHECK(decoded.value == value);
        ENCODING_CHECK(decoded.bytes_read == encoded.size());
    }
    // 负数向量单独列出（无符号聚合初始化不适合负数）。
    const std::int64_t negative_values[] = {-1, -2, -63, -64, -65,
                                            std::numeric_limits<std::int64_t>::min()};
    const char* negative_encoded[] = {"01", "03", "7d", "7f", "8101",
                                      "ffffffffffffffffff01"};
    for (std::size_t i = 0; i < sizeof(negative_values) / sizeof(negative_values[0]);
         ++i) {
        std::string encoded;
        e::Binary::PutVarint(encoded, negative_values[i]);
        ENCODING_CHECK_BYTES(encoded, ExpectedBytes(negative_encoded[i]));
        const auto decoded = e::Binary::Varint(encoded);
        ENCODING_REQUIRE(decoded);
        ENCODING_CHECK(decoded.value == negative_values[i]);
    }

    go2cpp_encoding_test::Announce("binary: uvarint overflow / truncation");

    // 第 10 个字节只能 <= 1；0x02 表示超过 64 位。
    ExpectUvarintOverflow(
        Bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02}),
        "10th byte = 0x02");
    // 第 11 个字节出现即溢出（Go 的 MaxVarintLen64 检查）。
    ExpectUvarintOverflow(
        Bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
               0x00}),
        "11 continuation bytes");
    // 恰好 10 个续位字节：Go 视为缓冲区太小，而不是溢出。
    ExpectUvarintTruncated(
        Bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
        "10 continuation bytes");
    ExpectUvarintTruncated("", "empty");
    ExpectUvarintTruncated(Bytes({0x80}), "single continuation");
    ExpectUvarintTruncated(Bytes({0xff, 0xff}), "two continuations");

    // 最大值的第 10 字节是 0x01，必须接受。
    {
        const auto max = e::Binary::Uvarint(
            Bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                   0x01}));
        ENCODING_REQUIRE(max);
        ENCODING_CHECK(max.value == std::numeric_limits<std::uint64_t>::max());
        ENCODING_CHECK(max.bytes_read == 10);
    }
    // Varint 的溢出/截断沿用 Uvarint 的错误。
    {
        const auto overflow = e::Binary::Varint(
            Bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                   0x02}));
        ENCODING_CHECK(overflow.error != nullptr);
        ENCODING_CHECK(Is(overflow.error, e::ErrUvarintOverflow()));
        const auto truncated = e::Binary::Varint(Bytes({0x80}));
        ENCODING_CHECK(truncated.error != nullptr);
        ENCODING_CHECK(Is(truncated.error, e::ErrUvarintTruncated()));
    }

    go2cpp_encoding_test::Announce("binary: append semantics");

    std::string target = "head";
    ENCODING_CHECK(e::Binary::PutUint16(target, 0x0102) == 2);
    ENCODING_CHECK(e::Binary::PutUint32(target, 0x03040506) == 4);
    ENCODING_CHECK(e::Binary::PutUvarint(target, 300) == 2);
    ENCODING_CHECK(target.size() == 4 + 2 + 4 + 2);
    ENCODING_CHECK_BYTES(target.substr(4, 2), Bytes({0x01, 0x02}));
    ENCODING_CHECK_BYTES(target.substr(6, 4), Bytes({0x03, 0x04, 0x05, 0x06}));
    ENCODING_CHECK_BYTES(target.substr(10, 2), Bytes({0xac, 0x02}));
}
