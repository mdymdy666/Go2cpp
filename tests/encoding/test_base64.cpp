/**
 * @file test_base64.cpp
 * @brief `encoding::Base64` 的单元测试：四种编码、填充语义、严格校验、边界。
 *
 * 覆盖：
 *   - RFC 4648 官方测试向量（含填充三种情况 `len % 3 == 0/1/2`）；
 *   - Std / URL / RawStd / RawURL 四种实例；
 *   - `EncodedLen` / `DecodedLen` / 追加语义 / 失败不改输出；
 *   - 非法字符、长度不合法、填充错位、填充后继续有数据；
 *   - URL 字母表与标准字母表的差异（`+/` vs `-_`）。
 */

#include "encoding_test_support.hpp"

#include "go2cpp/encoding/base64.hpp"
#include "go2cpp/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace e = go2cpp::encoding;

using go2cpp::ErrorMessage;
using go2cpp::Is;

namespace {

/// 断言解码失败且错误身份为 `Base64CorruptInput()`。
void ExpectCorrupt(const e::Base64Encoding& encoding, std::string_view input,
                   const char* what) {
    std::string dst = "unchanged";
    const auto error = encoding.Decode(dst, input);
    ENCODING_CHECK(error != nullptr);
    ENCODING_CHECK(Is(error, e::Base64CorruptInput()));
    ENCODING_CHECK(dst == "unchanged");
    const auto result = encoding.DecodeString(input);
    ENCODING_CHECK(result.error != nullptr);
    std::printf("    rejected %-14s <- \"%s\" (%s)\n", what,
                std::string(input).c_str(), ErrorMessage(error).c_str());
}

}  // namespace

void RunBase64Tests() {
    go2cpp_encoding_test::Announce("base64: RFC 4648 vectors");

    struct Vector {
        const char* plain;
        const char* padded;
        const char* raw;
    };
    const Vector vectors[] = {
        {"", "", ""},
        {"f", "Zg==", "Zg"},
        {"fo", "Zm8=", "Zm8"},
        {"foo", "Zm9v", "Zm9v"},
        {"foob", "Zm9vYg==", "Zm9vYg"},
        {"fooba", "Zm9vYmE=", "Zm9vYmE"},
        {"foobar", "Zm9vYmFy", "Zm9vYmFy"},
    };
    for (const Vector& vector : vectors) {
        const std::string plain = vector.plain;
        ENCODING_CHECK_BYTES(e::StdEncoding.EncodeToString(plain), vector.padded);
        ENCODING_CHECK_BYTES(e::RawStdEncoding.EncodeToString(plain), vector.raw);
        const auto padded = e::StdEncoding.DecodeString(vector.padded);
        ENCODING_REQUIRE(padded);
        ENCODING_CHECK_BYTES(padded.data, plain);
        const auto raw = e::RawStdEncoding.DecodeString(vector.raw);
        ENCODING_REQUIRE(raw);
        ENCODING_CHECK_BYTES(raw.data, plain);
    }

    go2cpp_encoding_test::Announce("base64: URL alphabet");

    // 0xfb 0xff 在标准字母表下是 "+/8="，URL 字母表下是 "-_8="。
    const std::string high_bytes = std::string("\xfb\xff", 2);
    ENCODING_CHECK_BYTES(e::StdEncoding.EncodeToString(high_bytes), "+/8=");
    ENCODING_CHECK_BYTES(e::URLEncoding.EncodeToString(high_bytes), "-_8=");
    ENCODING_CHECK_BYTES(e::RawURLEncoding.EncodeToString(high_bytes), "-_8");
    ENCODING_CHECK_BYTES(e::RawStdEncoding.EncodeToString(high_bytes), "+/8");

    const auto url_decoded = e::URLEncoding.DecodeString("-_8=");
    ENCODING_REQUIRE(url_decoded);
    ENCODING_CHECK_BYTES(url_decoded.data, high_bytes);
    const auto raw_url_decoded = e::RawURLEncoding.DecodeString("-_8");
    ENCODING_REQUIRE(raw_url_decoded);
    ENCODING_CHECK_BYTES(raw_url_decoded.data, high_bytes);

    // 两个字母表互不兼容：标准字母表不接受 `-`/`_`，URL 字母表不接受 `+`/`/`。
    ExpectCorrupt(e::StdEncoding, "-_8=", "std rejects url");
    ExpectCorrupt(e::URLEncoding, "+/8=", "url rejects std");

    go2cpp_encoding_test::Announce("base64: padding variants");

    // len % 3 == 0/1/2 三种填充情况。
    for (std::size_t length = 0; length <= 12; ++length) {
        const std::string input(length, 'A');
        const std::string encoded = e::StdEncoding.EncodeToString(input);
        ENCODING_CHECK(encoded.size() == e::StdEncoding.EncodedLen(length));
        ENCODING_CHECK(encoded.size() % 4 == 0);
        switch (length % 3) {
            case 0:
                ENCODING_CHECK(encoded.find('=') == std::string::npos);
                break;
            case 1:
                ENCODING_CHECK(encoded.size() >= 2);
                ENCODING_CHECK(encoded[encoded.size() - 1] == '=');
                ENCODING_CHECK(encoded[encoded.size() - 2] == '=');
                break;
            default:
                ENCODING_CHECK(encoded[encoded.size() - 1] == '=');
                ENCODING_CHECK(encoded[encoded.size() - 2] != '=');
                break;
        }
        const auto decoded = e::StdEncoding.DecodeString(encoded);
        ENCODING_REQUIRE(decoded);
        ENCODING_CHECK_BYTES(decoded.data, input);

        // Raw 编码大小公式：len/4*4 + (len%3==0 ? 0 : len%3+1)
        const std::string raw = e::RawStdEncoding.EncodeToString(input);
        ENCODING_CHECK(raw.size() == e::RawStdEncoding.EncodedLen(length));
        ENCODING_CHECK(raw.find('=') == std::string::npos);
        const auto raw_decoded = e::RawStdEncoding.DecodeString(raw);
        ENCODING_REQUIRE(raw_decoded);
        ENCODING_CHECK_BYTES(raw_decoded.data, input);
    }

    go2cpp_encoding_test::Announce("base64: append semantics");

    std::string target = "prefix:";
    const std::size_t appended = e::StdEncoding.Encode(target, "foo");
    ENCODING_CHECK(appended == 4);
    ENCODING_CHECK(target == "prefix:Zm9v");
    const auto append_error = e::StdEncoding.Decode(target, "YmFy");
    ENCODING_CHECK(append_error == nullptr);
    ENCODING_CHECK(target == "prefix:Zm9vbar");

    std::string untouched = "keep-me";
    const auto bad = e::StdEncoding.Decode(untouched, "!!!!");
    ENCODING_CHECK(bad != nullptr);
    ENCODING_CHECK(untouched == "keep-me");

    go2cpp_encoding_test::Announce("base64: configuration accessors");

    ENCODING_CHECK(e::StdEncoding.Alphabet().size() == 64);
    ENCODING_CHECK(e::URLEncoding.Alphabet().size() == 64);
    ENCODING_CHECK(e::StdEncoding.Padding() == '=');
    ENCODING_CHECK(e::URLEncoding.Padding() == '=');
    ENCODING_CHECK(!e::StdEncoding.IsRaw());
    ENCODING_CHECK(!e::URLEncoding.IsRaw());
    ENCODING_CHECK(e::RawStdEncoding.IsRaw());
    ENCODING_CHECK(e::RawURLEncoding.IsRaw());
    ENCODING_CHECK(e::RawStdEncoding.Padding() == '\0');
    ENCODING_CHECK(e::RawURLEncoding.Padding() == '\0');
    ENCODING_CHECK(e::StdEncoding.Alphabet().find('+') != std::string_view::npos);
    ENCODING_CHECK(e::URLEncoding.Alphabet().find('-') != std::string_view::npos);
    ENCODING_CHECK(e::URLEncoding.Alphabet().find('+') == std::string_view::npos);

    // 长度公式：DecodedLen 为合法输入的解码长度。
    ENCODING_CHECK(e::StdEncoding.DecodedLen(0) == 0);
    ENCODING_CHECK(e::StdEncoding.DecodedLen(4) == 3);
    ENCODING_CHECK(e::StdEncoding.DecodedLen(8) == 6);
    ENCODING_CHECK(e::RawStdEncoding.DecodedLen(2) == 1);
    ENCODING_CHECK(e::RawStdEncoding.DecodedLen(3) == 2);
    ENCODING_CHECK(e::RawStdEncoding.DecodedLen(4) == 3);

    go2cpp_encoding_test::Announce("base64: strict validation");

    // 非法字符
    ExpectCorrupt(e::StdEncoding, "Zm9v!", "illegal char");
    ExpectCorrupt(e::StdEncoding, "Zm9 v", "space");
    ExpectCorrupt(e::StdEncoding, "Zm9v\n", "newline");
    ExpectCorrupt(e::StdEncoding, "Zm9v\r\n", "crlf");
    // 长度不合法（带填充必须是 4 的倍数）
    ExpectCorrupt(e::StdEncoding, "Z", "length 1");
    ExpectCorrupt(e::StdEncoding, "Zg", "length 2");
    ExpectCorrupt(e::StdEncoding, "Zg=", "length 3");
    ExpectCorrupt(e::StdEncoding, "Zg===", "length 5");
    // 填充错位
    ExpectCorrupt(e::StdEncoding, "=Zg=", "pad first");
    ExpectCorrupt(e::StdEncoding, "Z=g=", "pad second");
    ExpectCorrupt(e::StdEncoding, "Zg==Zg==", "data after pad");
    // 无填充编码
    ExpectCorrupt(e::RawStdEncoding, "Zg==", "raw rejects pad");
    ExpectCorrupt(e::RawStdEncoding, "=", "raw rejects pad only");
    ExpectCorrupt(e::RawStdEncoding, "Z", "raw length 1 mod 4");
    ExpectCorrupt(e::RawStdEncoding, "Zm9vZ", "raw length 5");
    // 错误位置信息（对齐 Go 的 CorruptInputError 文本）
    {
        const auto error = e::StdEncoding.DecodeString("Zm9v!").error;
        ENCODING_CHECK(error != nullptr);
        ENCODING_CHECK(ErrorMessage(error).find("input byte 4") !=
                       std::string::npos);
    }

    // 空输入合法。
    {
        std::string empty_target = "x";
        ENCODING_CHECK(e::StdEncoding.Decode(empty_target, "") == nullptr);
        ENCODING_CHECK(empty_target == "x");
        ENCODING_CHECK_BYTES(e::StdEncoding.DecodeString("").data, "");
    }

    // 每个字节值都能往返（覆盖字母表全部 64 个字符）。
    {
        std::string all;
        all.resize(256);
        for (std::size_t i = 0; i < 256; ++i) {
            all[i] = static_cast<char>(i);
        }
        for (const e::Base64Encoding* encoding :
             {&e::StdEncoding, &e::URLEncoding, &e::RawStdEncoding,
              &e::RawURLEncoding}) {
            const std::string encoded = encoding->EncodeToString(all);
            const auto decoded = encoding->DecodeString(encoded);
            ENCODING_REQUIRE(decoded);
            ENCODING_CHECK_BYTES(decoded.data, all);
        }
    }

    // ------------------------------------------------------------------
    // Strict()：尾部填充位非零必须拒绝（Go 默认接受，Strict 才拒绝）。
    // ------------------------------------------------------------------
    go2cpp_encoding_test::Announce("base64: Strict() padding-bit checks");
    const e::Base64Encoding strict = e::StdEncoding.Strict();
    const e::Base64Encoding strict_raw = e::RawStdEncoding.Strict();
    ENCODING_CHECK(strict.IsStrict());
    ENCODING_CHECK(strict_raw.IsStrict());
    ENCODING_CHECK(!e::StdEncoding.IsStrict());
    ENCODING_CHECK(!e::RawStdEncoding.IsStrict());

    // 默认仍接受（与 Go 默认一致），Strict 拒绝。
    {
        const auto lenient = e::StdEncoding.DecodeString("Zh==");
        ENCODING_REQUIRE(lenient);
        ENCODING_CHECK_BYTES(lenient.data, "f");
        const auto strict_result = strict.DecodeString("Zh==");
        ENCODING_CHECK(strict_result.error != nullptr);
        ENCODING_CHECK(Is(strict_result.error, e::Base64CorruptInput()));
        ExpectCorrupt(strict, "Zm9=", "strict: low 2 bits non-zero");
        ExpectCorrupt(strict_raw, "Zh", "raw strict: low 4 bits non-zero");
        ExpectCorrupt(strict_raw, "Zm9", "raw strict: low 2 bits non-zero");
    }
    // 合法输入在 Strict 下仍然通过。
    {
        struct StrictCase {
            const char* encoded;
            const char* plain;
        };
        const StrictCase cases[] = {{"", ""},   {"Zg==", "f"},
                                    {"Zm8=", "fo"}, {"Zm9v", "foo"},
                                    {"Zm9vYg==", "foob"}};
        for (const StrictCase& item : cases) {
            const auto result = strict.DecodeString(item.encoded);
            ENCODING_REQUIRE(result);
            ENCODING_CHECK_BYTES(result.data, item.plain);
        }
        const auto raw_ok = strict_raw.DecodeString("Zm9vYg");
        ENCODING_REQUIRE(raw_ok);
        ENCODING_CHECK_BYTES(raw_ok.data, "foob");
    }
    // Strict() 是副本：原实例不受影响，且可以继续链式派生出不同字母表的副本。
    {
        const e::Base64Encoding url_strict = e::URLEncoding.Strict();
        ENCODING_CHECK(url_strict.IsStrict());
        ENCODING_CHECK(!e::URLEncoding.IsStrict());
        ENCODING_CHECK(url_strict.Padding() == '=');
        ENCODING_CHECK(url_strict.Alphabet() == e::URLEncoding.Alphabet());
    }
}
