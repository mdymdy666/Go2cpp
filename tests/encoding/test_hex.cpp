/**
 * @file test_hex.cpp
 * @brief `encoding::Hex` 的单元测试：小写输出、大小写不敏感解码、错误与边界。
 */

#include "encoding_test_support.hpp"

#include "go2cpp/encoding/hex.hpp"
#include "go2cpp/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace e = go2cpp::encoding;

using go2cpp::ErrorMessage;
using go2cpp::Is;

namespace {

void ExpectHexError(std::string_view input, const go2cpp::ErrorPtr& anchor,
                    const char* what) {
    std::string dst = "keep";
    const auto error = e::Hex::Decode(dst, input);
    ENCODING_CHECK(error != nullptr);
    ENCODING_CHECK(Is(error, anchor));
    ENCODING_CHECK(dst == "keep");
    std::printf("    rejected %-16s <- \"%s\" (%s)\n", what,
                std::string(input).c_str(), ErrorMessage(error).c_str());
}

}  // namespace

void RunHexTests() {
    go2cpp_encoding_test::Announce("hex: known vectors");

    ENCODING_CHECK_BYTES(e::Hex::EncodeToString(""), "");
    ENCODING_CHECK_BYTES(e::Hex::EncodeToString("abc"), "616263");
    ENCODING_CHECK_BYTES(e::Hex::EncodeToString(std::string("\x00\xff\x10", 3)),
                         "00ff10");
    // 输出必须是小写。
    ENCODING_CHECK_BYTES(e::Hex::EncodeToString(std::string("\xab\xcd", 2)),
                         "abcd");
    ENCODING_CHECK_BYTES(e::Hex::EncodeToString(std::string("\xde\xad\xbe\xef", 4)),
                         "deadbeef");
    // NUL 不截断。
    ENCODING_CHECK(e::Hex::EncodeToString(std::string("\x00\x00", 2)).size() == 4);

    ENCODING_CHECK(e::Hex::EncodedLen(0) == 0);
    ENCODING_CHECK(e::Hex::EncodedLen(1) == 2);
    ENCODING_CHECK(e::Hex::EncodedLen(7) == 14);
    ENCODING_CHECK(e::Hex::DecodedLen(0) == 0);
    ENCODING_CHECK(e::Hex::DecodedLen(2) == 1);
    ENCODING_CHECK(e::Hex::DecodedLen(14) == 7);

    go2cpp_encoding_test::Announce("hex: decode accepts both cases");

    {
        const auto lower = e::Hex::DecodeString("deadbeef");
        ENCODING_REQUIRE(lower);
        ENCODING_CHECK_BYTES(lower.data, std::string("\xde\xad\xbe\xef", 4));

        const auto upper = e::Hex::DecodeString("DEADBEEF");
        ENCODING_REQUIRE(upper);
        ENCODING_CHECK_BYTES(upper.data, lower.data);

        const auto mixed = e::Hex::DecodeString("DeAdBeEf");
        ENCODING_REQUIRE(mixed);
        ENCODING_CHECK_BYTES(mixed.data, lower.data);

        const auto empty = e::Hex::DecodeString("");
        ENCODING_REQUIRE(empty);
        ENCODING_CHECK(empty.data.empty());
    }

    go2cpp_encoding_test::Announce("hex: errors");

    ExpectHexError("a", e::HexOddLength(), "odd length");
    ExpectHexError("abc", e::HexOddLength(), "odd length 3");
    ExpectHexError("zz", e::HexInvalidByte(), "invalid first");
    ExpectHexError("0g", e::HexInvalidByte(), "invalid second");
    // 奇数长度优先于字符扫描（与 Go 的 Decode 一致）。
    ExpectHexError("ab cd", e::HexOddLength(), "space");
    ExpectHexError("ab\n", e::HexOddLength(), "newline");
    {
        const auto error = e::Hex::DecodeString("0g").error;
        ENCODING_REQUIRE(error != nullptr);
        ENCODING_CHECK(ErrorMessage(error).find("invalid byte") !=
                       std::string::npos);
        ENCODING_CHECK(ErrorMessage(error).find("U+0067") != std::string::npos);
    }

    go2cpp_encoding_test::Announce("hex: append semantics");

    std::string target = "x:";
    ENCODING_CHECK(e::Hex::Encode(target, "AB") == 4);
    ENCODING_CHECK(target == "x:4142");
    ENCODING_CHECK(e::Hex::Decode(target, "4142") == nullptr);
    ENCODING_CHECK(target == "x:4142AB");
    std::string untouched = "keep";
    ENCODING_CHECK(e::Hex::Decode(untouched, "zz") != nullptr);
    ENCODING_CHECK(untouched == "keep");

    go2cpp_encoding_test::Announce("hex: all byte values round-trip");

    std::string all;
    all.resize(256);
    for (std::size_t i = 0; i < 256; ++i) {
        all[i] = static_cast<char>(i);
    }
    const std::string encoded = e::Hex::EncodeToString(all);
    ENCODING_CHECK(encoded.size() == 512);
    ENCODING_CHECK(encoded.find_first_of("ABCDEF") == std::string::npos);
    const auto decoded = e::Hex::DecodeString(encoded);
    ENCODING_REQUIRE(decoded);
    ENCODING_CHECK_BYTES(decoded.data, all);

    // 大写输入同样得到全部字节。
    std::string upper = encoded;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const auto upper_decoded = e::Hex::DecodeString(upper);
    ENCODING_REQUIRE(upper_decoded);
    ENCODING_CHECK_BYTES(upper_decoded.data, all);
}
