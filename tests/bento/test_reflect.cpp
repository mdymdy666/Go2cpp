/**
 * @file test_reflect.cpp
 * @brief bento::reflect 验收测试：类型描述、严格 caster、Bind 与随机往返。
 *
 * @details 覆盖：
 * - `TypeTraits`/`Describe`/`NameOf`/`SizeOf` 及 cv/引用归一化；
 * - int/double/bool/string 的合法值、边界值、非法输入与随机往返；
 * - `FieldList` + `BENTO_BIND_FIELD` 的完整/部分/失败/未知/缺失绑定语义；
 * - 失败时对象不被部分修改（强保证）。
 *
 * 运行方式：构建后执行 `./tests/bento/bento_test_reflect`（或 ctest -R bento_reflect）。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "go2cpp/bento/reflect/bind.hpp"
#include "go2cpp/bento/reflect/caster.hpp"
#include "go2cpp/bento/reflect/reflect.hpp"
#include "test_error_support.hpp"

namespace {

using namespace go2cpp::bento::reflect;

/// @brief 绑定测试用配置结构体。
struct Config {
    std::string host = "127.0.0.1";
    int port = 8080;
    double ratio = 0.5;
    bool secure = false;
    std::string note = "default";
};

/// @brief 确定性伪随机源（splitmix64），测试不读系统熵。
class Rng {
 public:
    explicit Rng(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t Next() noexcept {
        m_state += 0x9e3779b97f4a7c15ULL;
        std::uint64_t value = m_state;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

 private:
    std::uint64_t m_state;
};

} // namespace

namespace go2cpp::bento::reflect {

/// @brief Config 的字段列表特化（宏只声明字段，不重复写类型转换）。
template <>
struct FieldList<Config> {
    static std::vector<FieldBinder<Config>> Fields() {
        return {
            BENTO_BIND_FIELD(Config, host),   BENTO_BIND_FIELD(Config, port),
            BENTO_BIND_FIELD(Config, ratio),  BENTO_BIND_FIELD(Config, secure),
            BENTO_BIND_FIELD(Config, note),
        };
    }
};

} // namespace go2cpp::bento::reflect

namespace {

// ---------------------------------------------------------------------------
// 类型描述
// ---------------------------------------------------------------------------

void TestTypeDescriptors() {
    const TypeDescriptor int_desc = Describe<int>();
    BENTO_ERROR_CHECK_EQ(std::string_view(int_desc.name), std::string_view("int"));
    BENTO_ERROR_CHECK_EQ(int_desc.size, sizeof(int));
    BENTO_ERROR_CHECK(int_desc.is_numeric);
    BENTO_ERROR_CHECK_EQ(int_desc.kind, FieldKind::kInt);

    const TypeDescriptor double_desc = Describe<double>();
    BENTO_ERROR_CHECK_EQ(std::string_view(double_desc.name), std::string_view("double"));
    BENTO_ERROR_CHECK(double_desc.is_numeric);
    BENTO_ERROR_CHECK_EQ(double_desc.kind, FieldKind::kDouble);

    const TypeDescriptor bool_desc = Describe<bool>();
    BENTO_ERROR_CHECK_EQ(std::string_view(bool_desc.name), std::string_view("bool"));
    BENTO_ERROR_CHECK(!bool_desc.is_numeric);
    BENTO_ERROR_CHECK_EQ(bool_desc.kind, FieldKind::kBool);

    const TypeDescriptor string_desc = Describe<std::string>();
    BENTO_ERROR_CHECK_EQ(std::string_view(string_desc.name), std::string_view("string"));
    BENTO_ERROR_CHECK_EQ(string_desc.size, sizeof(std::string));
    BENTO_ERROR_CHECK_EQ(string_desc.kind, FieldKind::kString);

    BENTO_ERROR_CHECK_EQ(std::string(NameOf<int>()), std::string("int"));
    BENTO_ERROR_CHECK_EQ(std::string(NameOf<std::string>()), std::string("string"));
    BENTO_ERROR_CHECK_EQ(SizeOf<double>(), sizeof(double));
    BENTO_ERROR_CHECK_EQ(SizeOf<std::string>(), sizeof(std::string));

    BENTO_ERROR_CHECK_EQ(std::string(NameOf<const int>()), std::string("int"));
    BENTO_ERROR_CHECK_EQ(Describe<const double&>().kind, FieldKind::kDouble);
    BENTO_ERROR_CHECK_EQ(SizeOf<volatile bool>(), sizeof(bool));

    BENTO_ERROR_CHECK_EQ(static_cast<int>(FieldKind::kString), 0);
    BENTO_ERROR_CHECK_EQ(static_cast<int>(FieldKind::kInt), 1);
    BENTO_ERROR_CHECK_EQ(static_cast<int>(FieldKind::kDouble), 2);
    BENTO_ERROR_CHECK_EQ(static_cast<int>(FieldKind::kBool), 3);
}

// ---------------------------------------------------------------------------
// 严格 caster
// ---------------------------------------------------------------------------

void TestIntCaster() {
    const caster::StringToInt to_int;
    int value = 0;
    BENTO_ERROR_CHECK(to_int("42", value));
    BENTO_ERROR_CHECK_EQ(value, 42);
    BENTO_ERROR_CHECK(to_int("-42", value));
    BENTO_ERROR_CHECK_EQ(value, -42);
    BENTO_ERROR_CHECK(to_int("+7", value));
    BENTO_ERROR_CHECK_EQ(value, 7);
    BENTO_ERROR_CHECK(to_int("0", value));
    BENTO_ERROR_CHECK_EQ(value, 0);
    BENTO_ERROR_CHECK(to_int("-0", value));
    BENTO_ERROR_CHECK_EQ(value, 0);
    BENTO_ERROR_CHECK(to_int("0007", value));
    BENTO_ERROR_CHECK_EQ(value, 7);
    const std::string max_text = std::to_string(std::numeric_limits<int>::max());
    const std::string min_text = std::to_string(std::numeric_limits<int>::min());
    const std::string overflow_text =
        std::to_string(static_cast<long long>(std::numeric_limits<int>::max()) + 1LL);
    const std::string underflow_text =
        std::to_string(static_cast<long long>(std::numeric_limits<int>::min()) - 1LL);
    BENTO_ERROR_CHECK(to_int(max_text, value));
    BENTO_ERROR_CHECK_EQ(value, std::numeric_limits<int>::max());
    BENTO_ERROR_CHECK(to_int(min_text, value));
    BENTO_ERROR_CHECK_EQ(value, std::numeric_limits<int>::min());

    const char* const invalid[] = {
        "",     " ",      " 1",    "1 ",   "+",     "-",      "1.0",  "abc",
        "1e3",  "++1",    "--1",   "1\n",  "\t1",
    };
    for (const char* text : invalid) {
        value = 12345;
        BENTO_ERROR_CHECK(!to_int(text, value));
    }
    BENTO_ERROR_CHECK(!to_int(overflow_text, value));
    BENTO_ERROR_CHECK(!to_int(underflow_text, value));
}

void TestDoubleCaster() {
    const caster::StringToDouble to_double;
    double value = 0.0;
    BENTO_ERROR_CHECK(to_double("0", value));
    BENTO_ERROR_CHECK_EQ(value, 0.0);
    BENTO_ERROR_CHECK(to_double("-0.5", value));
    BENTO_ERROR_CHECK_EQ(value, -0.5);
    BENTO_ERROR_CHECK(to_double("+2.", value));
    BENTO_ERROR_CHECK_EQ(value, 2.0);
    BENTO_ERROR_CHECK(to_double(".5", value));
    BENTO_ERROR_CHECK_EQ(value, 0.5);
    BENTO_ERROR_CHECK(to_double("1e3", value));
    BENTO_ERROR_CHECK_EQ(value, 1000.0);
    BENTO_ERROR_CHECK(to_double("1E-3", value));
    BENTO_ERROR_CHECK(std::fabs(value - 0.001) < 1e-15);
    BENTO_ERROR_CHECK(to_double("-123.456", value));
    BENTO_ERROR_CHECK(std::fabs(value + 123.456) < 1e-12);

    const char* const invalid[] = {
        "",     " ",     "1,5",  "inf",   "nan",   "0x10",  "1.2.3", "1e",
        "e3",   "1e309", "-1e309", "1e-400", "+.", ".",   "1 ",    " 1",
    };
    for (const char* text : invalid) {
        value = 42.0;
        BENTO_ERROR_CHECK(!to_double(text, value));
    }
}

void TestBoolCaster() {
    const caster::StringToBool to_bool;
    bool value = false;
    const char* const true_values[] = {"true", "TRUE", "True", "1", "yes", "YES"};
    for (const char* text : true_values) {
        BENTO_ERROR_CHECK(to_bool(text, value));
        BENTO_ERROR_CHECK(value);
    }
    const char* const false_values[] = {"false", "FALSE", "False", "0", "no", "NO"};
    for (const char* text : false_values) {
        value = true;
        BENTO_ERROR_CHECK(to_bool(text, value));
        BENTO_ERROR_CHECK(!value);
    }
    const char* const invalid[] = {"", "2", " true", "true ", "y", "n", "truth", "00"};
    for (const char* text : invalid) {
        BENTO_ERROR_CHECK(!to_bool(text, value));
    }
}

void TestStringCaster() {
    const caster::StringToString to_string;
    std::string value = "old";
    BENTO_ERROR_CHECK(to_string("", value));
    BENTO_ERROR_CHECK_EQ(value, std::string(""));
    BENTO_ERROR_CHECK(to_string("  spaces  ", value));
    BENTO_ERROR_CHECK_EQ(value, std::string("  spaces  "));
    BENTO_ERROR_CHECK(to_string("中文, \"quoted\"\nline", value));
    BENTO_ERROR_CHECK_EQ(value, std::string("中文, \"quoted\"\nline"));
}

void TestRandomIntRoundTrip() {
    Rng rng(0x123456789abcdef0ULL);
    const caster::StringToInt to_int;
    for (int iteration = 0; iteration < 5000; ++iteration) {
        const int expected =
            static_cast<int>(rng.Next() % 2000000001ULL) - 1000000000;
        const std::string text = std::to_string(expected);
        int parsed = 0;
        BENTO_ERROR_CHECK(to_int(text, parsed));
        BENTO_ERROR_CHECK_EQ(parsed, expected);
    }
}

void TestRandomDoubleRoundTrip() {
    Rng rng(0x0f0e0d0c0b0a0908ULL);
    const caster::StringToDouble to_double;
    int checked = 0;
    int attempts = 0;
    while (checked < 2000 && attempts < 20000) {
        ++attempts;
        const std::uint64_t raw = rng.Next();
        double expected = 0.0;
        std::memcpy(&expected, &raw, sizeof(expected));
        if (!std::isfinite(expected)) {
            continue;
        }
        char buffer[64];
        const std::to_chars_result written = std::to_chars(
            buffer, buffer + sizeof(buffer), expected, std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (written.ec != std::errc()) {
            continue;
        }
        const std::string_view text(buffer, static_cast<std::size_t>(written.ptr - buffer));
        double parsed = 0.0;
        BENTO_ERROR_CHECK(to_double(text, parsed));
        BENTO_ERROR_CHECK_EQ(parsed, expected);
        BENTO_ERROR_CHECK_EQ(std::signbit(parsed), std::signbit(expected));
        ++checked;
    }
    BENTO_ERROR_CHECK_EQ(checked, 2000);
}

void TestRandomGarbageNeverCrashes() {
    Rng rng(0xfeedfacecafebeefULL);
    const caster::StringToInt to_int;
    const caster::StringToDouble to_double;
    const caster::StringToBool to_bool;
    constexpr char kAlphabet[] =
        "0123456789+-.eE abcXYZ\t\n'\"\\,;:{}[]()_/|*&^%$#@!~`";

    for (int iteration = 0; iteration < 3000; ++iteration) {
        const std::size_t length = static_cast<std::size_t>(rng.Next() % 13ULL);
        std::string text;
        text.reserve(length);
        for (std::size_t index = 0; index < length; ++index) {
            const std::size_t offset =
                static_cast<std::size_t>(rng.Next() % (sizeof(kAlphabet) - 1U));
            text.push_back(kAlphabet[offset]);
        }
        int int_value = 0;
        double double_value = 0.0;
        bool bool_value = false;
        const bool int_ok = to_int(text, int_value);
        const bool double_ok = to_double(text, double_value);
        const bool bool_ok = to_bool(text, bool_value);
        if (int_ok) {
            int again = 0;
            BENTO_ERROR_CHECK(to_int(text, again));
            BENTO_ERROR_CHECK_EQ(again, int_value);
        }
        if (double_ok) {
            BENTO_ERROR_CHECK(std::isfinite(double_value));
        }
        if (bool_ok) {
            bool again = false;
            BENTO_ERROR_CHECK(to_bool(text, again));
            BENTO_ERROR_CHECK_EQ(again, bool_value);
        }
    }
}

// ---------------------------------------------------------------------------
// Bind
// ---------------------------------------------------------------------------

void TestBindFull() {
    const std::map<std::string, std::string> values{
        {"host", "example.com"},
        {"port", "9090"},
        {"ratio", "0.25"},
        {"secure", "true"},
        {"note", "hello world"},
    };
    Config config;
    BENTO_ERROR_CHECK(Bind(config, values));
    BENTO_ERROR_CHECK_EQ(config.host, std::string("example.com"));
    BENTO_ERROR_CHECK_EQ(config.port, 9090);
    BENTO_ERROR_CHECK_EQ(config.ratio, 0.25);
    BENTO_ERROR_CHECK(config.secure);
    BENTO_ERROR_CHECK_EQ(config.note, std::string("hello world"));
}

void TestBindPartialPreservesDefaults() {
    Config config;
    const std::map<std::string, std::string> values{{"port", "1234"}};
    BENTO_ERROR_CHECK(Bind(config, values));
    BENTO_ERROR_CHECK_EQ(config.port, 1234);
    BENTO_ERROR_CHECK_EQ(config.host, std::string("127.0.0.1"));
    BENTO_ERROR_CHECK_EQ(config.ratio, 0.5);
    BENTO_ERROR_CHECK(!config.secure);
    BENTO_ERROR_CHECK_EQ(config.note, std::string("default"));
}

void TestBindFailureKeepsObject() {
    Config config;
    config.host = "original";
    config.port = 1;
    config.ratio = 2.0;
    config.secure = true;
    config.note = "keep";
    BindError error;
    const std::map<std::string, std::string> values{
        {"host", "changed"},
        {"port", "not-a-number"},
    };
    BENTO_ERROR_CHECK(!Bind(config, values, BindOptions{}, &error));
    BENTO_ERROR_CHECK_EQ(error.kind, BindErrorKind::kWrongType);
    BENTO_ERROR_CHECK_EQ(error.field, std::string("port"));
    BENTO_ERROR_CHECK_EQ(error.value, std::string("not-a-number"));

    BindError short_error;
    BENTO_ERROR_CHECK(!Bind(config, values, &short_error));
    BENTO_ERROR_CHECK_EQ(short_error.kind, BindErrorKind::kWrongType);
    BENTO_ERROR_CHECK_EQ(short_error.field, std::string("port"));

    BENTO_ERROR_CHECK_EQ(config.host, std::string("original"));
    BENTO_ERROR_CHECK_EQ(config.port, 1);
    BENTO_ERROR_CHECK_EQ(config.ratio, 2.0);
    BENTO_ERROR_CHECK(config.secure);
    BENTO_ERROR_CHECK_EQ(config.note, std::string("keep"));
}

void TestBindUnknownFieldPolicy() {
    Config config;
    const std::map<std::string, std::string> values{{"unknown", "x"}, {"port", "80"}};
    BENTO_ERROR_CHECK(Bind(config, values));
    BENTO_ERROR_CHECK_EQ(config.port, 80);

    BindOptions options;
    options.reject_unknown = true;
    BindError error;
    BENTO_ERROR_CHECK(!Bind(config, values, options, &error));
    BENTO_ERROR_CHECK_EQ(error.kind, BindErrorKind::kUnknownField);
    BENTO_ERROR_CHECK_EQ(error.field, std::string("unknown"));
    BENTO_ERROR_CHECK_EQ(config.port, 80);
}

void TestBindRequireAll() {
    Config config;
    const std::map<std::string, std::string> partial{{"host", "h"}};
    BindOptions options;
    options.require_all = true;
    BindError error;
    BENTO_ERROR_CHECK(!Bind(config, partial, options, &error));
    BENTO_ERROR_CHECK_EQ(error.kind, BindErrorKind::kMissingField);
    BENTO_ERROR_CHECK(!error.field.empty());
    BENTO_ERROR_CHECK_EQ(config.host, std::string("127.0.0.1"));

    const std::map<std::string, std::string> full{
        {"host", "h"}, {"port", "1"}, {"ratio", "1.5"}, {"secure", "no"}, {"note", ""}};
    BENTO_ERROR_CHECK(Bind(config, full, options));
    BENTO_ERROR_CHECK_EQ(config.host, std::string("h"));
    BENTO_ERROR_CHECK_EQ(config.port, 1);
    BENTO_ERROR_CHECK_EQ(config.ratio, 1.5);
    BENTO_ERROR_CHECK(!config.secure);
    BENTO_ERROR_CHECK_EQ(config.note, std::string(""));
}

void TestBindOptionalForm() {
    const std::map<std::string, std::string> values{{"port", "65535"}, {"ratio", "-2.5"}};
    const std::optional<Config> parsed = Bind<Config>(values);
    BENTO_ERROR_CHECK(parsed.has_value());
    if (parsed) {
        BENTO_ERROR_CHECK_EQ(parsed->port, 65535);
        BENTO_ERROR_CHECK_EQ(parsed->ratio, -2.5);
    }

    const std::map<std::string, std::string> invalid{{"port", "70000x"}};
    const std::optional<Config> failed = Bind<Config>(invalid);
    BENTO_ERROR_CHECK(!failed.has_value());
}

void TestBindEdgeValues() {
    Config config;
    const std::map<std::string, std::string> values{
        {"port", "-2147483648"},
        {"ratio", "-0.0"},
        {"secure", "NO"},
        {"note", "中文, \"quoted\"\nline"},
    };
    BENTO_ERROR_CHECK(Bind(config, values));
    BENTO_ERROR_CHECK_EQ(config.port, std::numeric_limits<int>::min());
    BENTO_ERROR_CHECK_EQ(config.ratio, -0.0);
    BENTO_ERROR_CHECK(std::signbit(config.ratio));
    BENTO_ERROR_CHECK(!config.secure);
    BENTO_ERROR_CHECK_EQ(config.note, std::string("中文, \"quoted\"\nline"));

    const std::map<std::string, std::string> max_values{{"port", "2147483647"}};
    BENTO_ERROR_CHECK(Bind(config, max_values));
    BENTO_ERROR_CHECK_EQ(config.port, std::numeric_limits<int>::max());
}

void TestBindCaseSensitiveAndEmpty() {
    Config config;
    const std::map<std::string, std::string> wrong_case{{"Port", "1"}};
    BENTO_ERROR_CHECK(Bind(config, wrong_case));
    BENTO_ERROR_CHECK_EQ(config.port, 8080);

    BindOptions options;
    options.reject_unknown = true;
    BENTO_ERROR_CHECK(!Bind(config, wrong_case, options));

    BENTO_ERROR_CHECK(Bind(config, std::map<std::string, std::string>{}));
    BENTO_ERROR_CHECK_EQ(config.port, 8080);
}

void TestBindStringPreserved() {
    Config config;
    const std::map<std::string, std::string> values{
        {"note", "a,b;c=d e f 中文 \t tab"}};
    BENTO_ERROR_CHECK(Bind(config, values));
    BENTO_ERROR_CHECK_EQ(config.note, std::string("a,b;c=d e f 中文 \t tab"));
}

void TestBindUnorderedMap() {
    const std::unordered_map<std::string, std::string> values{
        {"host", "unordered.example"}, {"secure", "1"}, {"port", "443"}};
    Config config;
    BENTO_ERROR_CHECK(Bind(config, values));
    BENTO_ERROR_CHECK_EQ(config.host, std::string("unordered.example"));
    BENTO_ERROR_CHECK_EQ(config.port, 443);
    BENTO_ERROR_CHECK(config.secure);

    const std::optional<Config> parsed = Bind<Config>(values);
    BENTO_ERROR_CHECK(parsed.has_value());
    if (parsed) {
        BENTO_ERROR_CHECK_EQ(parsed->host, std::string("unordered.example"));
        BENTO_ERROR_CHECK(parsed->secure);
    }
}

} // namespace

int main() {
    TestTypeDescriptors();
    TestIntCaster();
    TestDoubleCaster();
    TestBoolCaster();
    TestStringCaster();
    TestRandomIntRoundTrip();
    TestRandomDoubleRoundTrip();
    TestRandomGarbageNeverCrashes();
    TestBindFull();
    TestBindPartialPreservesDefaults();
    TestBindFailureKeepsObject();
    TestBindUnknownFieldPolicy();
    TestBindRequireAll();
    TestBindOptionalForm();
    TestBindEdgeValues();
    TestBindCaseSensitiveAndEmpty();
    TestBindStringPreserved();
    TestBindUnorderedMap();
    return bento_error_test::Finish("bento_test_reflect");
}
