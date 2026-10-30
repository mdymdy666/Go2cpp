/**
 * @file test_reflect_codec.cpp
 * @brief Reflect 模块验收测试：类型注册、序列化往返、压缩、文本格式化、帧对接。
 *
 * @details 覆盖：
 * - TypeId/TypeTraits/TypeRegistry（内置固定 id、动态注册、固定 id 注册、并发）；
 * - Buffer varint/zigzag/字符串/截断与越界；
 * - 内置类型与嵌套结构体的二进制往返（含边界值、字节级精度）；
 * - JSON-like 文本格式化（可读性、转义、往返）；
 * - RLE 压缩（压缩比、不膨胀、损坏流拒绝、压缩炸弹拒绝）；
 * - 与 Bento 通用帧对接（MakeFrame/EncodeFrame/DecodeFrame/粘包/半包/CRC）；
 * - 错误类型、未知类型、未知字段、缺失字段、坏 magic/version/文本；
 * - 随机往返与 fuzz（畸形输入不崩溃）、吞吐压测、6 个真实场景。
 *
 * 运行方式：构建后执行 `go2cpp_reflect_codec_test`（或 ctest -R reflect_codec）。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/frame.hpp"
#include "go2cpp/reflect/frame_codec.hpp"
#include "go2cpp/reflect/reflect.hpp"

namespace testmsg {

/// @brief 嵌套配置（用于验证嵌套结构体字段）。
struct Limits {
    std::uint32_t max_connections = 0;
    std::uint32_t max_message_bytes = 0;
    double ratio = 0.0;
};

/// @brief 登录请求。
struct LoginRequest {
    std::string user;
    std::string token;
    std::int64_t timestamp = 0;
    bool remember = false;
};

/// @brief 玩家状态同步。
struct PlayerState {
    std::uint64_t player_id = 0;
    double x = 0.0;
    double y = 0.0;
    std::int32_t hp = 0;
    std::string name;
    std::uint32_t flags = 0;
};

/// @brief 聊天消息。
struct ChatMessage {
    std::string room;
    std::string sender;
    std::string text;
    std::int64_t sent_at = 0;
};

/// @brief 服务端配置下发（含嵌套结构体）。
struct ServerConfig {
    std::uint32_t version = 0;
    Limits limits;
    std::string motd;
};

/// @brief 与 LoginRequest 不同的类型（用于类型不匹配测试）。
struct Wrong {
    std::int32_t id = 0;
    std::string name;
};

inline bool operator==(const Limits& lhs, const Limits& rhs) {
    return lhs.max_connections == rhs.max_connections &&
           lhs.max_message_bytes == rhs.max_message_bytes && lhs.ratio == rhs.ratio;
}

inline bool operator==(const LoginRequest& lhs, const LoginRequest& rhs) {
    return lhs.user == rhs.user && lhs.token == rhs.token && lhs.timestamp == rhs.timestamp &&
           lhs.remember == rhs.remember;
}

inline bool operator==(const PlayerState& lhs, const PlayerState& rhs) {
    return lhs.player_id == rhs.player_id && lhs.x == rhs.x && lhs.y == rhs.y && lhs.hp == rhs.hp &&
           lhs.name == rhs.name && lhs.flags == rhs.flags;
}

inline bool operator==(const ChatMessage& lhs, const ChatMessage& rhs) {
    return lhs.room == rhs.room && lhs.sender == rhs.sender && lhs.text == rhs.text &&
           lhs.sent_at == rhs.sent_at;
}

inline bool operator==(const ServerConfig& lhs, const ServerConfig& rhs) {
    return lhs.version == rhs.version && lhs.limits == rhs.limits && lhs.motd == rhs.motd;
}

inline bool operator==(const Wrong& lhs, const Wrong& rhs) {
    return lhs.id == rhs.id && lhs.name == rhs.name;
}

}  // namespace testmsg

namespace go2cpp::reflect {

/// @brief 测试用固定类型 id（协议双方约定的 id 表）。
inline constexpr TypeId kLoginType = 100;
inline constexpr TypeId kPlayerType = 101;
inline constexpr TypeId kChatType = 102;
inline constexpr TypeId kLimitsType = 103;
inline constexpr TypeId kConfigType = 104;
inline constexpr TypeId kWrongType = 105;

template <>
struct FieldList<testmsg::Limits> {
    static constexpr TypeId kId = kLimitsType;
    static constexpr const char* kName = "Limits";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(testmsg::Limits, max_connections),
            GO2CPP_REFLECT_FIELD(testmsg::Limits, max_message_bytes),
            GO2CPP_REFLECT_FIELD(testmsg::Limits, ratio),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<testmsg::LoginRequest> {
    static constexpr TypeId kId = kLoginType;
    static constexpr const char* kName = "LoginRequest";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(testmsg::LoginRequest, user),
            GO2CPP_REFLECT_FIELD(testmsg::LoginRequest, token),
            GO2CPP_REFLECT_FIELD(testmsg::LoginRequest, timestamp),
            GO2CPP_REFLECT_FIELD(testmsg::LoginRequest, remember),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<testmsg::PlayerState> {
    static constexpr TypeId kId = kPlayerType;
    static constexpr const char* kName = "PlayerState";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(testmsg::PlayerState, player_id),
            GO2CPP_REFLECT_FIELD(testmsg::PlayerState, x),
            GO2CPP_REFLECT_FIELD(testmsg::PlayerState, y),
            GO2CPP_REFLECT_FIELD(testmsg::PlayerState, hp),
            GO2CPP_REFLECT_FIELD(testmsg::PlayerState, name),
            GO2CPP_REFLECT_FIELD(testmsg::PlayerState, flags),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<testmsg::ChatMessage> {
    static constexpr TypeId kId = kChatType;
    static constexpr const char* kName = "ChatMessage";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(testmsg::ChatMessage, room),
            GO2CPP_REFLECT_FIELD(testmsg::ChatMessage, sender),
            GO2CPP_REFLECT_FIELD(testmsg::ChatMessage, text),
            GO2CPP_REFLECT_FIELD(testmsg::ChatMessage, sent_at),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<testmsg::ServerConfig> {
    static constexpr TypeId kId = kConfigType;
    static constexpr const char* kName = "ServerConfig";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(testmsg::ServerConfig, version),
            GO2CPP_REFLECT_FIELD(testmsg::ServerConfig, limits),
            GO2CPP_REFLECT_FIELD(testmsg::ServerConfig, motd),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<testmsg::Wrong> {
    static constexpr TypeId kId = kWrongType;
    static constexpr const char* kName = "Wrong";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(testmsg::Wrong, id),
            GO2CPP_REFLECT_FIELD(testmsg::Wrong, name),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

}  // namespace go2cpp::reflect

namespace {

using namespace go2cpp::reflect;

int g_failures = 0;
int g_checks = 0;
std::string g_current_test;

void ReportFailure(const char* file, int line, const std::string& message) {
    std::cerr << "[FAIL] " << g_current_test << " (" << file << ":" << line << "): " << message
              << "\n";
    ++g_failures;
}

#define CHECK_TRUE(condition)                              \
    do {                                                   \
        ++g_checks;                                        \
        if (!(condition)) {                                \
            ReportFailure(__FILE__, __LINE__, #condition); \
        }                                                  \
    } while (false)

#define CHECK_EQ(actual, expected)                                                  \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!((actual) == (expected))) {                                            \
            ReportFailure(__FILE__, __LINE__,                                       \
                          std::string("expected ") + #actual + " == " + #expected); \
        }                                                                           \
    } while (false)

/// @brief 确定性伪随机源（splitmix64），测试不依赖系统熵。
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

    std::size_t Range(std::size_t bound) noexcept {
        return bound == 0 ? 0 : static_cast<std::size_t>(Next() % bound);
    }

   private:
    std::uint64_t m_state;
};

std::size_t CountOccurrences(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = haystack.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::string ToText(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

/// @brief 字符串 -> 字节数组。
/// @note 不用 `vector(text.begin(), text.end())`：GCC 13 在 -O3 下会把
///       `_M_range_initialize` 误判为 `-Wfree-nonheap-object` 并让 -Werror
///       构建失败（编译器已知误报）；显式循环避开该模式。
std::vector<std::uint8_t> ToBytes(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size());
    for (const char value : text) {
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

template <typename T>
std::vector<std::uint8_t> EncodeBytes(const T& value, Encoding encoding = Encoding::kBinary,
                                      Compression compression = Compression::kNone) {
    std::vector<std::uint8_t> bytes;
    const CodecStatus status = ReflectCodec::Encode(value, bytes, encoding, compression);
    CHECK_TRUE(status == CodecStatus::kOk);
    return bytes;
}

template <typename T>
T BinaryRoundTrip(const T& value, Compression compression = Compression::kNone) {
    const std::vector<std::uint8_t> bytes = EncodeBytes(value, Encoding::kBinary, compression);
    T restored{};
    const CodecStatus status = ReflectCodec::Decode(bytes, restored);
    CHECK_TRUE(status == CodecStatus::kOk);
    return restored;
}

template <typename T>
T TextRoundTrip(const T& value) {
    const std::vector<std::uint8_t> bytes = EncodeBytes(value, Encoding::kText);
    T restored{};
    const CodecStatus status = ReflectCodec::Decode(bytes, restored);
    CHECK_TRUE(status == CodecStatus::kOk);
    return restored;
}

template <typename T>
void CheckBuiltinRoundTrip(const T& value) {
    Buffer buffer;
    CHECK_TRUE(Serialize(value, buffer));
    T restored{};
    CHECK_TRUE(Deserialize(buffer, restored));
    CHECK_EQ(buffer.Remaining(), std::size_t{0});
    CHECK_TRUE(restored == value);
}

/// @brief 手工拼一条二进制 Reflect 消息（用于构造异常/未知类型输入）。
std::vector<std::uint8_t> MakeBinaryEnvelope(TypeId type_id, std::string_view type_name,
                                             const std::vector<std::uint8_t>& payload,
                                             bool compressed = false) {
    Buffer buffer;
    CHECK_TRUE(buffer.PutBytes("G2RF", 4));
    CHECK_TRUE(buffer.PutU8(1));
    CHECK_TRUE(buffer.PutU8(compressed ? 1u : 0u));
    CHECK_TRUE(buffer.PutVarint(type_id));
    CHECK_TRUE(buffer.PutString(type_name));
    CHECK_TRUE(buffer.PutVarint(static_cast<std::uint64_t>(payload.size())));
    CHECK_TRUE(buffer.PutBytes(payload.data(), payload.size()));
    return buffer.data();
}

std::string RandomText(Rng& rng, std::size_t max_length) {
    static constexpr char kChars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-.:/\\\"'{}[]";
    const std::size_t length = rng.Range(max_length + 1);
    std::string text;
    text.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        text.push_back(kChars[rng.Range(sizeof(kChars) - 1)]);
    }
    return text;
}

double RandomFiniteDouble(Rng& rng) {
    const auto raw = static_cast<std::int64_t>(rng.Next() % 2000001ULL) - 1000000;
    return static_cast<double>(raw) / 1000.0;
}

testmsg::LoginRequest MakeLogin() {
    testmsg::LoginRequest message;
    message.user = "alice";
    message.token = "dG9rZW4tZm9yLXRlc3Q=";
    message.timestamp = 1730000000123LL;
    message.remember = true;
    return message;
}

testmsg::PlayerState MakePlayer() {
    testmsg::PlayerState state;
    state.player_id = 9007199254740993ULL;
    state.x = 1234.5;
    state.y = -9876.25;
    state.hp = 87;
    state.name = "玩家一号";
    state.flags = 0x80000001u;
    return state;
}

testmsg::ChatMessage MakeChat() {
    testmsg::ChatMessage message;
    message.room = "world-1";
    message.sender = "player-42";
    message.text = "你好，世界！\"quoted\" \\ slash\nnew line\ttab";
    message.sent_at = -1;
    return message;
}

testmsg::ServerConfig MakeConfig() {
    testmsg::ServerConfig config;
    config.version = 3;
    config.limits.max_connections = 5000;
    config.limits.max_message_bytes = 1300;
    config.limits.ratio = 0.75;
    config.motd = "欢迎来到服务器";
    return config;
}

// ---------------------------------------------------------------------------
// 1. 类型身份
// ---------------------------------------------------------------------------

void TestTypeIdentity() {
    CHECK_TRUE(TypeIdOf<bool>() == kBoolType);
    CHECK_TRUE(TypeIdOf<int>() == kInt32Type);
    CHECK_TRUE(TypeIdOf<short>() == kInt32Type);
    CHECK_TRUE(TypeIdOf<std::int64_t>() == kInt64Type);
    CHECK_TRUE(TypeIdOf<unsigned int>() == kUint32Type);
    CHECK_TRUE(TypeIdOf<std::uint64_t>() == kUint64Type);
    CHECK_TRUE(TypeIdOf<double>() == kDoubleType);
    CHECK_TRUE(TypeIdOf<std::string>() == kStringType);
    CHECK_TRUE(TypeIdOf<std::vector<std::uint8_t>>() == kBytesType);
    CHECK_TRUE(TypeIdOf<testmsg::LoginRequest>() == kLoginType);
    CHECK_TRUE(std::string(TypeNameOf<testmsg::LoginRequest>()) == "LoginRequest");
    CHECK_TRUE(std::string(TypeNameOf<std::string>()) == "string");
    CHECK_TRUE(TypeIdOf<const double&>() == kDoubleType);
    CHECK_TRUE(TypeIdOf<volatile std::int64_t>() == kInt64Type);
    CHECK_TRUE(TypeIdOf<float>() == kUnknownType);
    CHECK_TRUE(std::string(TypeNameOf<float>()) == "unknown");
    static_assert(TypeIdOf<int>() == kInt32Type, "int maps to int32 on this platform");
    static_assert(IsBuiltinType<std::string>(), "string is builtin");
    static_assert(!IsBuiltinType<testmsg::LoginRequest>(), "struct is not builtin");
}

// ---------------------------------------------------------------------------
// 2. 类型注册表
// ---------------------------------------------------------------------------

void TestTypeRegistry() {
    TypeRegistry& registry = TypeRegistry::Instance();
    CHECK_TRUE(registry.Size() >= 8);

    TypeId found = kUnknownType;
    CHECK_TRUE(registry.Find("bool", found));
    CHECK_TRUE(found == kBoolType);
    CHECK_TRUE(registry.Find("string", found));
    CHECK_TRUE(found == kStringType);
    CHECK_TRUE(!registry.Find("no.such.type", found));

    std::string_view name;
    CHECK_TRUE(registry.Name(kInt32Type, name));
    CHECK_TRUE(name == "int32");
    CHECK_TRUE(!registry.Name(kUnknownType, name));

    TypeInfo info;
    CHECK_TRUE(registry.Describe(kStringType, info));
    CHECK_TRUE(info.name == "string");
    CHECK_EQ(info.size, sizeof(std::string));
    CHECK_TRUE(info.builtin);
    CHECK_TRUE(!registry.Describe(kUnknownType, info));

    const TypeId dynamic_id = registry.Register("test.dynamic", 24);
    CHECK_TRUE(dynamic_id != kUnknownType);
    CHECK_TRUE(dynamic_id >= kFirstDynamicType);
    CHECK_TRUE(registry.Register("test.dynamic", 24) == dynamic_id);
    CHECK_TRUE(registry.Register("test.dynamic", 99) == dynamic_id);
    CHECK_TRUE(registry.Find("test.dynamic", found));
    CHECK_TRUE(found == dynamic_id);
    CHECK_TRUE(registry.Describe(dynamic_id, info));
    CHECK_EQ(info.size, std::size_t{24});
    CHECK_TRUE(!info.builtin);

    CHECK_TRUE(registry.Register("", 4) == kUnknownType);
    CHECK_TRUE(registry.Register("x", 0) == kUnknownType);
    CHECK_TRUE(registry.Register(std::string(kMaxTypeNameBytes + 1, 'a'), 4) == kUnknownType);
    CHECK_TRUE(!registry.Contains(kUnknownType));

    // 固定 id 注册（协议约定 id 表）。
    CHECK_TRUE(registry.RegisterWithId(kLoginType, "LoginRequest", sizeof(testmsg::LoginRequest)) ==
               kLoginType);
    CHECK_TRUE(registry.RegisterWithId(kLoginType, "LoginRequest", 1) == kLoginType);
    CHECK_TRUE(registry.RegisterWithId(kPlayerType, "PlayerState", sizeof(testmsg::PlayerState)) ==
               kPlayerType);
    CHECK_TRUE(registry.RegisterWithId(kChatType, "ChatMessage", sizeof(testmsg::ChatMessage)) ==
               kChatType);
    CHECK_TRUE(registry.RegisterWithId(kLimitsType, "Limits", sizeof(testmsg::Limits)) ==
               kLimitsType);
    CHECK_TRUE(registry.RegisterWithId(kConfigType, "ServerConfig",
                                       sizeof(testmsg::ServerConfig)) == kConfigType);
    CHECK_TRUE(registry.RegisterWithId(kWrongType, "Wrong", sizeof(testmsg::Wrong)) == kWrongType);
    // 同 id 不同名字必须被拒绝，不能覆盖已有映射。
    CHECK_TRUE(registry.RegisterWithId(kLoginType, "AnotherName", 8) == kUnknownType);
    CHECK_TRUE(registry.Contains(kLoginType));

    // 并发注册同名类型：所有线程必须拿到同一个 id。
    constexpr std::size_t kThreads = 8;
    std::vector<TypeId> results(kThreads, kUnknownType);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t index = 0; index < kThreads; ++index) {
        threads.emplace_back([index, &results] {
            results[index] = TypeRegistry::Instance().Register("test.concurrent", 8);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK_TRUE(results.front() != kUnknownType);
    for (const TypeId id : results) {
        CHECK_TRUE(id == results.front());
    }
}

// ---------------------------------------------------------------------------
// 3. Buffer
// ---------------------------------------------------------------------------

void TestBufferVarint() {
    Buffer buffer;
    CHECK_TRUE(buffer.PutVarint(0));
    CHECK_TRUE(buffer.PutVarint(1));
    CHECK_TRUE(buffer.PutVarint(127));
    CHECK_TRUE(buffer.PutVarint(128));
    CHECK_TRUE(buffer.PutVarint(16384));
    CHECK_TRUE(buffer.PutVarint(std::numeric_limits<std::uint64_t>::max()));
    CHECK_TRUE(buffer.PutSignedVarint(0));
    CHECK_TRUE(buffer.PutSignedVarint(-1));
    CHECK_TRUE(buffer.PutSignedVarint(1));
    CHECK_TRUE(buffer.PutSignedVarint(std::numeric_limits<std::int64_t>::min()));
    CHECK_TRUE(buffer.PutSignedVarint(std::numeric_limits<std::int64_t>::max()));

    std::uint64_t unsigned_value = 0;
    std::int64_t signed_value = 0;
    CHECK_TRUE(buffer.GetVarint(unsigned_value));
    CHECK_TRUE(unsigned_value == 0);
    CHECK_TRUE(buffer.GetVarint(unsigned_value));
    CHECK_TRUE(unsigned_value == 1);
    CHECK_TRUE(buffer.GetVarint(unsigned_value));
    CHECK_TRUE(unsigned_value == 127);
    CHECK_TRUE(buffer.GetVarint(unsigned_value));
    CHECK_TRUE(unsigned_value == 128);
    CHECK_TRUE(buffer.GetVarint(unsigned_value));
    CHECK_TRUE(unsigned_value == 16384);
    CHECK_TRUE(buffer.GetVarint(unsigned_value));
    CHECK_TRUE(unsigned_value == std::numeric_limits<std::uint64_t>::max());
    CHECK_TRUE(buffer.GetSignedVarint(signed_value));
    CHECK_TRUE(signed_value == 0);
    CHECK_TRUE(buffer.GetSignedVarint(signed_value));
    CHECK_TRUE(signed_value == -1);
    CHECK_TRUE(buffer.GetSignedVarint(signed_value));
    CHECK_TRUE(signed_value == 1);
    CHECK_TRUE(buffer.GetSignedVarint(signed_value));
    CHECK_TRUE(signed_value == std::numeric_limits<std::int64_t>::min());
    CHECK_TRUE(buffer.GetSignedVarint(signed_value));
    CHECK_TRUE(signed_value == std::numeric_limits<std::int64_t>::max());
    CHECK_TRUE(buffer.Remaining() == 0);
    CHECK_TRUE(buffer.ok());

    Buffer truncated;
    CHECK_TRUE(truncated.PutU8(0x80));
    std::uint64_t raw = 0;
    CHECK_TRUE(!truncated.GetVarint(raw));

    Buffer overflow;
    for (int index = 0; index < 9; ++index) {
        CHECK_TRUE(overflow.PutU8(0x80));
    }
    CHECK_TRUE(overflow.PutU8(0x02));
    CHECK_TRUE(!overflow.GetVarint(raw));

    Buffer limited(4);
    CHECK_TRUE(!limited.PutString("12345"));

    Buffer short_string;
    CHECK_TRUE(short_string.PutVarint(100));
    std::string text;
    CHECK_TRUE(!short_string.GetString(text));

    Buffer strings;
    const std::string with_nul("a\0b", 3);
    CHECK_TRUE(strings.PutString(with_nul));
    CHECK_TRUE(strings.PutString("你好"));
    std::string first;
    std::string second;
    CHECK_TRUE(strings.GetString(first));
    CHECK_TRUE(strings.GetString(second));
    CHECK_TRUE(first == with_nul);
    CHECK_TRUE(second == "你好");
}

// ---------------------------------------------------------------------------
// 4. 内置类型二进制往返
// ---------------------------------------------------------------------------

void TestBuiltinBinaryRoundTrip() {
    CheckBuiltinRoundTrip(false);
    CheckBuiltinRoundTrip(true);
    CheckBuiltinRoundTrip<std::int8_t>(-128);
    CheckBuiltinRoundTrip<std::int16_t>(std::numeric_limits<std::int16_t>::min());
    CheckBuiltinRoundTrip(std::numeric_limits<std::int32_t>::min());
    CheckBuiltinRoundTrip(std::numeric_limits<std::int32_t>::max());
    CheckBuiltinRoundTrip(std::numeric_limits<std::int64_t>::min());
    CheckBuiltinRoundTrip(std::numeric_limits<std::int64_t>::max());
    CheckBuiltinRoundTrip(std::numeric_limits<std::uint32_t>::max());
    CheckBuiltinRoundTrip(std::numeric_limits<std::uint64_t>::max());
    CheckBuiltinRoundTrip(0.0);
    CheckBuiltinRoundTrip(1.5);
    CheckBuiltinRoundTrip(-123456.789);
    CheckBuiltinRoundTrip(std::numeric_limits<double>::min());
    CheckBuiltinRoundTrip(std::numeric_limits<double>::max());
    CheckBuiltinRoundTrip(std::string());
    CheckBuiltinRoundTrip(std::string("plain ascii"));
    CheckBuiltinRoundTrip(std::string("你好，世界"));
    CheckBuiltinRoundTrip(std::string("a\0b", 3));
    CheckBuiltinRoundTrip(std::vector<std::uint8_t>{});
    CheckBuiltinRoundTrip(std::vector<std::uint8_t>{0, 1, 127, 128, 255});

    // 浮点位级往返（-0.0 / NaN 无法用 == 区分）。
    const auto check_bits = [](double original) {
        Buffer buffer;
        CHECK_TRUE(Serialize(original, buffer));
        double restored = 1.0;
        CHECK_TRUE(Deserialize(buffer, restored));
        std::uint64_t original_bits = 0;
        std::uint64_t restored_bits = 0;
        std::memcpy(&original_bits, &original, sizeof(original_bits));
        std::memcpy(&restored_bits, &restored, sizeof(restored_bits));
        CHECK_TRUE(original_bits == restored_bits);
    };
    check_bits(-0.0);
    check_bits(std::numeric_limits<double>::quiet_NaN());
    check_bits(std::numeric_limits<double>::infinity());

    // 整型越界必须被拒绝（把 int64 值解到 int32）。
    Buffer big;
    CHECK_TRUE(Serialize(static_cast<std::int64_t>(1) << 40, big));
    std::int32_t narrowed = 7;
    CHECK_TRUE(!Deserialize(big, narrowed));
    CHECK_TRUE(narrowed == 7);
}

// ---------------------------------------------------------------------------
// 5. 结构体二进制往返 + 字段名去重
// ---------------------------------------------------------------------------

void TestStructBinaryRoundTrip() {
    const testmsg::LoginRequest login = MakeLogin();
    CHECK_TRUE(BinaryRoundTrip(login) == login);

    const testmsg::PlayerState player = MakePlayer();
    CHECK_TRUE(BinaryRoundTrip(player) == player);

    const testmsg::ChatMessage chat = MakeChat();
    CHECK_TRUE(BinaryRoundTrip(chat) == chat);

    const testmsg::ServerConfig config = MakeConfig();
    CHECK_TRUE(BinaryRoundTrip(config) == config);
    CHECK_TRUE(BinaryRoundTrip(config, Compression::kRle) == config);

    // 字段名在整条消息里只应出现一次（字段表），不会每个字段重复。
    const std::vector<std::uint8_t> bytes = EncodeBytes(login, Encoding::kBinary);
    const std::string raw = ToText(bytes);
    CHECK_EQ(CountOccurrences(raw, "user"), std::size_t{1});
    CHECK_EQ(CountOccurrences(raw, "token"), std::size_t{1});
    CHECK_EQ(CountOccurrences(raw, "timestamp"), std::size_t{1});
    CHECK_EQ(CountOccurrences(raw, "remember"), std::size_t{1});

    // 失败时目标对象保持不变（强保证）。
    testmsg::Wrong wrong;
    wrong.id = 7;
    wrong.name = "before";
    CHECK_TRUE(ReflectCodec::Decode(bytes, wrong) == CodecStatus::kTypeMismatch);
    CHECK_TRUE((wrong == testmsg::Wrong{7, "before"}));

    testmsg::LoginRequest target = MakeLogin();
    target.user = "keep";
    Buffer empty;
    CHECK_TRUE(!Deserialize(empty, target));
    CHECK_TRUE(target.user == "keep");
}

// ---------------------------------------------------------------------------
// 6. 文本格式
// ---------------------------------------------------------------------------

void TestTextFieldFormat() {
    const testmsg::LoginRequest login = MakeLogin();
    const std::string text = ToText(EncodeBytes(login, Encoding::kText));
    CHECK_TRUE(text.find("\"type\":\"LoginRequest\"") != std::string::npos);
    CHECK_TRUE(text.find("\"payload\":{") != std::string::npos);
    CHECK_TRUE(text.find("\"user\":\"alice\"") != std::string::npos);
    CHECK_TRUE(text.find("\"remember\":true") != std::string::npos);
    CHECK_TRUE(text.find('\n') == std::string::npos);
    CHECK_TRUE(TextRoundTrip(login) == login);

    const testmsg::ChatMessage chat = MakeChat();
    const std::string chat_text = ToText(EncodeBytes(chat, Encoding::kText));
    CHECK_TRUE(chat_text.find("\\n") != std::string::npos);
    CHECK_TRUE(chat_text.find("\\t") != std::string::npos);
    CHECK_TRUE(chat_text.find("\\\"") != std::string::npos);
    CHECK_TRUE(TextRoundTrip(chat) == chat);

    const testmsg::ServerConfig config = MakeConfig();
    const std::string config_text = ToText(EncodeBytes(config, Encoding::kText));
    CHECK_TRUE(config_text.find("\"limits\":{") != std::string::npos);
    CHECK_TRUE(config_text.find("\"max_connections\":5000") != std::string::npos);
    CHECK_TRUE(TextRoundTrip(config) == config);

    // 文本模式的压缩参数被忽略（仍是可读 JSON）。
    const std::string compressed_text =
        ToText(EncodeBytes(chat, Encoding::kText, Compression::kRle));
    CHECK_TRUE(compressed_text.find("\"type\":\"ChatMessage\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 7. 字段描述与按名称访问
// ---------------------------------------------------------------------------

void TestFieldDescriptorAccess() {
    CHECK_EQ(FieldCount<testmsg::LoginRequest>(), std::size_t{4});
    CHECK_EQ(FieldCount<testmsg::Limits>(), std::size_t{3});
    CHECK_EQ(FieldCount<int>(), std::size_t{0});

    testmsg::LoginRequest request = MakeLogin();
    std::size_t index = 99;
    const FieldDescriptor* field = FindField<testmsg::LoginRequest>("token", &index);
    CHECK_TRUE(field != nullptr);
    CHECK_EQ(index, std::size_t{1});
    CHECK_TRUE(field->type_id == kStringType);
    CHECK_EQ(field->size, sizeof(std::string));
    const std::ptrdiff_t expected_offset =
        reinterpret_cast<const char*>(&request.token) - reinterpret_cast<const char*>(&request);
    CHECK_EQ(field->offset, static_cast<std::size_t>(expected_offset));
    const auto* token_pointer = reinterpret_cast<const std::string*>(
        reinterpret_cast<const char*>(&request) + field->offset);
    CHECK_TRUE(*token_pointer == request.token);

    CHECK_TRUE(FindField<testmsg::LoginRequest>("no_such_field") == nullptr);
    CHECK_TRUE(FindField<testmsg::LoginRequest>("timestamp")->type_id == kInt64Type);
    CHECK_TRUE(FindField<testmsg::LoginRequest>("remember")->type_id == kBoolType);
    CHECK_TRUE(FindField<testmsg::PlayerState>("x")->type_id == kDoubleType);
    CHECK_TRUE(FindField<testmsg::ServerConfig>("limits")->type_id == kLimitsType);
}

// ---------------------------------------------------------------------------
// 8. RLE 压缩
// ---------------------------------------------------------------------------

void TestRleCompression() {
    const std::vector<std::uint8_t> repeated(1000, 0x41);
    std::vector<std::uint8_t> compressed;
    CHECK_TRUE(RleCompress(repeated.data(), repeated.size(), compressed));
    CHECK_TRUE(compressed.size() < 32);
    std::vector<std::uint8_t> restored;
    CHECK_TRUE(RleDecompress(compressed.data(), compressed.size(), restored));
    CHECK_TRUE(restored == repeated);

    const std::vector<std::uint8_t> empty;
    CHECK_TRUE(RleCompress(nullptr, 0, compressed));
    CHECK_TRUE(RleDecompress(compressed.data(), compressed.size(), restored));
    CHECK_TRUE(restored.empty());

    std::vector<std::uint8_t> pattern;
    for (std::size_t index = 0; index < 2000; ++index) {
        pattern.push_back(static_cast<std::uint8_t>(index % 251));
    }
    CHECK_TRUE(RleCompress(pattern.data(), pattern.size(), compressed));
    CHECK_TRUE(RleDecompress(compressed.data(), compressed.size(), restored));
    CHECK_TRUE(restored == pattern);

    // 截断/篡改的压缩流必须被拒绝，而不是解出半截数据。
    std::vector<std::uint8_t> truncated(compressed.begin(), compressed.end() - 1);
    CHECK_TRUE(!RleDecompress(truncated.data(), truncated.size(), restored));
    CHECK_TRUE(restored.empty());

    Buffer bomb;
    CHECK_TRUE(bomb.PutVarint(100u * 1024u * 1024u));
    CHECK_TRUE(bomb.PutU8(0x7f));
    CHECK_TRUE(bomb.PutU8(0));
    CHECK_TRUE(!RleDecompress(bomb.data().data(), bomb.size(), restored, 1024));

    // Codec 层：长重复文本压缩后明显变小，且能解回。
    testmsg::ChatMessage chat = MakeChat();
    chat.text.assign(4000, 'x');
    const std::vector<std::uint8_t> plain = EncodeBytes(chat, Encoding::kBinary);
    const std::vector<std::uint8_t> packed =
        EncodeBytes(chat, Encoding::kBinary, Compression::kRle);
    CHECK_TRUE(packed.size() < plain.size());
    CHECK_TRUE(BinaryRoundTrip(chat, Compression::kRle) == chat);

    Envelope envelope;
    CHECK_TRUE(ReflectCodec::Peek(packed.data(), packed.size(), envelope) == CodecStatus::kOk);
    CHECK_TRUE(envelope.compressed);
    CHECK_TRUE(ReflectCodec::Peek(plain.data(), plain.size(), envelope) == CodecStatus::kOk);
    CHECK_TRUE(!envelope.compressed);

    // 不可压缩的数据不设置压缩标志（不会因为压缩反而变大）。
    testmsg::ChatMessage random_chat = MakeChat();
    Rng rng(42);
    random_chat.text.clear();
    for (std::size_t index = 0; index < 3000; ++index) {
        random_chat.text.push_back(static_cast<char>(rng.Range(256)));
    }
    const std::vector<std::uint8_t> random_packed =
        EncodeBytes(random_chat, Encoding::kBinary, Compression::kRle);
    CHECK_TRUE(ReflectCodec::Peek(random_packed.data(), random_packed.size(), envelope) ==
               CodecStatus::kOk);
    CHECK_TRUE(!envelope.compressed);
    CHECK_TRUE(BinaryRoundTrip(random_chat, Compression::kRle) == random_chat);
}

// ---------------------------------------------------------------------------
// 9. Bento 帧对接
// ---------------------------------------------------------------------------

void TestFrameIntegration() {
    const testmsg::LoginRequest login = MakeLogin();

    net::Frame frame = ReflectFrameCodec::MakeFrame(login, 7, Encoding::kBinary);
    CHECK_EQ(frame.type, std::uint16_t{7});
    CHECK_EQ(frame.flags, std::uint16_t{0});
    CHECK_TRUE(!frame.payload.empty());

    const std::vector<std::uint8_t> wire = net::Encode(frame);
    CHECK_TRUE(!wire.empty());

    net::Frame decoded_frame;
    CHECK_TRUE(net::Decode(wire.data(), wire.size(), &decoded_frame) == net::FrameStatus::kOk);
    CHECK_EQ(decoded_frame.type, std::uint16_t{7});
    testmsg::LoginRequest restored;
    CHECK_TRUE(ReflectFrameCodec::DecodeFrame(decoded_frame, restored) == CodecStatus::kOk);
    CHECK_TRUE(restored == login);

    net::FrameStatus frame_status = net::FrameStatus::kOk;
    std::size_t consumed = 0;
    testmsg::LoginRequest direct;
    CHECK_TRUE(ReflectFrameCodec::DecodeFrame(wire.data(), wire.size(), direct, &frame_status,
                                              &consumed) == CodecStatus::kOk);
    CHECK_TRUE(frame_status == net::FrameStatus::kOk);
    CHECK_EQ(consumed, wire.size());
    CHECK_TRUE(direct == login);

    // 文本帧 + 自定义 flags。
    const net::Frame text_frame =
        ReflectFrameCodec::MakeFrame(login, 9, Encoding::kText, Compression::kNone, 0x1234);
    CHECK_EQ(text_frame.flags, std::uint16_t{0x1234});
    const std::vector<std::uint8_t> text_wire = net::Encode(text_frame);
    CHECK_TRUE(ReflectFrameCodec::DecodeFrame(text_wire.data(), text_wire.size(), direct) ==
               CodecStatus::kOk);
    CHECK_TRUE(direct == login);

    // 压缩帧。
    testmsg::ChatMessage chat = MakeChat();
    chat.text.assign(3000, 'z');
    const std::vector<std::uint8_t> packed_wire =
        ReflectFrameCodec::EncodeFrame(chat, 10, Encoding::kBinary, Compression::kRle);
    testmsg::ChatMessage chat_out;
    CHECK_TRUE(ReflectFrameCodec::DecodeFrame(packed_wire.data(), packed_wire.size(), chat_out) ==
               CodecStatus::kOk);
    CHECK_TRUE(chat_out == chat);

    // 粘包：两个帧连续到达，按 consumed 逐帧解析。
    const std::vector<std::uint8_t> first = ReflectFrameCodec::EncodeFrame(login, 1);
    const std::vector<std::uint8_t> second = ReflectFrameCodec::EncodeFrame(MakePlayer(), 2);
    std::vector<std::uint8_t> stream = first;
    stream.insert(stream.end(), second.begin(), second.end());
    std::size_t offset = 0;
    testmsg::LoginRequest login_out;
    CHECK_TRUE(ReflectFrameCodec::DecodeFrame(stream.data() + offset, stream.size() - offset,
                                              login_out, &frame_status,
                                              &consumed) == CodecStatus::kOk);
    offset += consumed;
    testmsg::PlayerState player_out;
    CHECK_TRUE(ReflectFrameCodec::DecodeFrame(stream.data() + offset, stream.size() - offset,
                                              player_out, &frame_status,
                                              &consumed) == CodecStatus::kOk);
    CHECK_TRUE(player_out == MakePlayer());
    CHECK_EQ(offset + consumed, stream.size());

    // 半包：少一个字节必须报 need_more_data，而不是解析出半帧。
    CHECK_TRUE(ReflectFrameCodec::DecodeFrame(wire.data(), wire.size() - 1, direct,
                                              &frame_status) == CodecStatus::kFrameError);
    CHECK_TRUE(frame_status == net::FrameStatus::kNeedMoreData);

    // CRC 损坏。
    std::vector<std::uint8_t> corrupted = wire;
    corrupted.back() = static_cast<std::uint8_t>(corrupted.back() ^ 0xffu);
    CHECK_TRUE(ReflectFrameCodec::DecodeFrame(corrupted.data(), corrupted.size(), direct,
                                              &frame_status) == CodecStatus::kFrameError);
    CHECK_TRUE(frame_status == net::FrameStatus::kChecksumMismatch);
}

// ---------------------------------------------------------------------------
// 10. 错误类型（类型不匹配）
// ---------------------------------------------------------------------------

void TestWrongType() {
    const testmsg::LoginRequest login = MakeLogin();
    const std::vector<std::uint8_t> binary = EncodeBytes(login, Encoding::kBinary);
    const std::vector<std::uint8_t> text = EncodeBytes(login, Encoding::kText);

    testmsg::Wrong wrong;
    wrong.id = 11;
    wrong.name = "unchanged";
    CHECK_TRUE(ReflectCodec::Decode(binary, wrong) == CodecStatus::kTypeMismatch);
    CHECK_TRUE((wrong == testmsg::Wrong{11, "unchanged"}));
    CHECK_TRUE(ReflectCodec::Decode(text, wrong) == CodecStatus::kTypeMismatch);
    CHECK_TRUE((wrong == testmsg::Wrong{11, "unchanged"}));

    testmsg::PlayerState player;
    CHECK_TRUE(ReflectCodec::Decode(binary, player) == CodecStatus::kTypeMismatch);

    // 类型信息正确时仍可解回。
    testmsg::LoginRequest restored;
    CHECK_TRUE(ReflectCodec::Decode(binary, restored) == CodecStatus::kOk);
    CHECK_TRUE(restored == login);
}

// ---------------------------------------------------------------------------
// 11. 未知类型
// ---------------------------------------------------------------------------

void TestUnknownType() {
    const std::vector<std::uint8_t> payload{0x00};  // 空字段表
    const std::vector<std::uint8_t> ghost = MakeBinaryEnvelope(9999, "Ghost", payload);
    testmsg::LoginRequest login;
    CHECK_TRUE(ReflectCodec::Decode(ghost, login) == CodecStatus::kUnknownType);

    TypeId type_id = kUnknownType;
    std::string type_name;
    CHECK_TRUE(ReflectCodec::PeekType(ghost.data(), ghost.size(), type_id, type_name) ==
               CodecStatus::kOk);
    CHECK_TRUE(type_id == 9999);
    CHECK_TRUE(type_name == "Ghost");

    const std::string text = R"({"v":1,"id":9999,"type":"Ghost","payload":{}})";
    const std::vector<std::uint8_t> text_bytes = ToBytes(text);
    CHECK_TRUE(ReflectCodec::Decode(text_bytes, login) == CodecStatus::kUnknownType);

    // 名字一致时即使 id 不同也接受（动态注册顺序不同的真实场景）；
    // 空字段表意味着所有字段缺省，保留目标对象的默认值。
    const std::vector<std::uint8_t> renamed = MakeBinaryEnvelope(12345, "LoginRequest", payload);
    testmsg::LoginRequest renamed_out;
    CHECK_TRUE(ReflectCodec::Decode(renamed, renamed_out) == CodecStatus::kOk);
    CHECK_TRUE(renamed_out.user.empty());
    CHECK_TRUE(renamed_out.token.empty());
}

// ---------------------------------------------------------------------------
// 12. 畸形输入
// ---------------------------------------------------------------------------

void TestMalformedInput() {
    testmsg::LoginRequest login;
    CHECK_TRUE(ReflectCodec::Decode(nullptr, 0, login) == CodecStatus::kInvalidArgument);
    CHECK_TRUE(ReflectCodec::Decode(std::vector<std::uint8_t>{}, login) == CodecStatus::kTruncated);

    std::vector<std::uint8_t> bad_magic = EncodeBytes(MakeLogin());
    bad_magic[0] = 'X';
    CHECK_TRUE(ReflectCodec::Decode(bad_magic, login) == CodecStatus::kBadMagic);

    std::vector<std::uint8_t> bad_version = EncodeBytes(MakeLogin());
    bad_version[4] = 2;
    CHECK_TRUE(ReflectCodec::Decode(bad_version, login) == CodecStatus::kBadVersion);

    std::vector<std::uint8_t> bad_flags = EncodeBytes(MakeLogin());
    bad_flags[5] = 0x80;
    CHECK_TRUE(ReflectCodec::Decode(bad_flags, login) == CodecStatus::kBadVersion);

    const std::vector<std::uint8_t> full = EncodeBytes(MakeLogin());
    for (std::size_t cut = 1; cut < full.size(); cut += 7) {
        const std::vector<std::uint8_t> truncated(full.begin(), full.begin() + cut);
        const CodecStatus status = ReflectCodec::Decode(truncated, login);
        CHECK_TRUE(status != CodecStatus::kOk);
    }

    const std::string bad_json = R"({"v":1,"id":100,"type":"LoginRequest","payload":{)";
    CHECK_TRUE(ReflectCodec::Decode(ToBytes(bad_json), login) == CodecStatus::kBadText);

    // 载荷内尾随字节：字段表之后多 1 字节，解码必须拒绝。
    Buffer trailing_payload;
    CHECK_TRUE(Serialize(MakeLogin(), trailing_payload));
    CHECK_TRUE(trailing_payload.PutU8(0xaa));
    const std::vector<std::uint8_t> trailing =
        MakeBinaryEnvelope(kLoginType, "LoginRequest", trailing_payload.data());
    CHECK_TRUE(ReflectCodec::Decode(trailing, login) == CodecStatus::kBadPayload);

    // 非对象的 JSON 根。
    const std::string array_text = "[1,2,3]";
    CHECK_TRUE(ReflectCodec::Decode(ToBytes(array_text), login) == CodecStatus::kBadMagic);
}

// ---------------------------------------------------------------------------
// 13. 未知字段 / 类型不符字段 / 缺失字段
// ---------------------------------------------------------------------------

void TestUnknownAndMissingFields() {
    testmsg::LoginRequest login;
    Buffer unknown_payload;
    CHECK_TRUE(unknown_payload.PutVarint(1));
    CHECK_TRUE(unknown_payload.PutString("bogus"));
    CHECK_TRUE(unknown_payload.PutVarint(kInt32Type));
    CHECK_TRUE(unknown_payload.PutSignedVarint(42));
    const std::vector<std::uint8_t> unknown =
        MakeBinaryEnvelope(kLoginType, "LoginRequest", unknown_payload.data());
    CHECK_TRUE(ReflectCodec::Decode(unknown, login) == CodecStatus::kUnknownField);

    Buffer mismatched_payload;
    CHECK_TRUE(mismatched_payload.PutVarint(1));
    CHECK_TRUE(mismatched_payload.PutString("user"));
    CHECK_TRUE(mismatched_payload.PutVarint(kInt32Type));
    CHECK_TRUE(mismatched_payload.PutSignedVarint(42));
    const std::vector<std::uint8_t> mismatched =
        MakeBinaryEnvelope(kLoginType, "LoginRequest", mismatched_payload.data());
    CHECK_TRUE(ReflectCodec::Decode(mismatched, login) == CodecStatus::kTypeMismatch);

    // 文本未知字段拒绝；缺字段保留默认值。
    const std::string unknown_text =
        R"({"v":1,"id":100,"type":"LoginRequest","payload":{"user":"a","bogus":1}})";
    CHECK_TRUE(ReflectCodec::Decode(ToBytes(unknown_text), login) == CodecStatus::kBadPayload);

    const std::string partial_text =
        R"({"v":1,"id":100,"type":"LoginRequest","payload":{"user":"a"}})";
    testmsg::LoginRequest partial;
    partial.token = "keep";
    CHECK_TRUE(ReflectCodec::Decode(ToBytes(partial_text), partial) == CodecStatus::kOk);
    CHECK_TRUE(partial.user == "a");
    // 缺字段回到默认构造值，而不是沿用调用方旧值：解码总是从全新的临时对象
    // 开始，避免复用对象时的字段串扰。
    CHECK_TRUE(partial.token.empty());
    CHECK_EQ(partial.timestamp, std::int64_t{0});
    CHECK_TRUE(!partial.remember);
}

// ---------------------------------------------------------------------------
// 14. 随机往返 + fuzz
// ---------------------------------------------------------------------------

void TestRandomRoundTrip() {
    Rng rng(0x5eed1234ULL);
    for (int round = 0; round < 400; ++round) {
        testmsg::LoginRequest login;
        login.user = RandomText(rng, 40);
        login.token = RandomText(rng, 80);
        login.timestamp = static_cast<std::int64_t>(rng.Next());
        login.remember = (rng.Next() & 1u) != 0;

        testmsg::PlayerState player;
        player.player_id = rng.Next();
        player.x = RandomFiniteDouble(rng);
        player.y = RandomFiniteDouble(rng);
        player.hp = static_cast<std::int32_t>(rng.Next());
        player.name = RandomText(rng, 30);
        player.flags = static_cast<std::uint32_t>(rng.Next());

        testmsg::ChatMessage chat;
        chat.room = RandomText(rng, 10);
        chat.sender = RandomText(rng, 20);
        chat.text = RandomText(rng, 300);
        chat.sent_at = static_cast<std::int64_t>(rng.Next());

        testmsg::ServerConfig config;
        config.version = static_cast<std::uint32_t>(rng.Next());
        config.limits.max_connections = static_cast<std::uint32_t>(rng.Next());
        config.limits.max_message_bytes = static_cast<std::uint32_t>(rng.Next());
        config.limits.ratio = RandomFiniteDouble(rng);
        config.motd = RandomText(rng, 60);

        CHECK_TRUE(BinaryRoundTrip(login) == login);
        CHECK_TRUE(BinaryRoundTrip(player, Compression::kRle) == player);
        CHECK_TRUE(TextRoundTrip(chat) == chat);
        CHECK_TRUE(BinaryRoundTrip(config) == config);
        CHECK_TRUE(TextRoundTrip(config) == config);
    }

    // fuzz：任意字节都不能让解码崩溃；成功时必须只消费一条完整消息。
    Rng fuzz_rng(0xdeadbeefULL);
    for (int round = 0; round < 3000; ++round) {
        const std::size_t length = fuzz_rng.Range(256);
        std::vector<std::uint8_t> noise(length);
        for (auto& byte : noise) {
            byte = static_cast<std::uint8_t>(fuzz_rng.Next());
        }
        testmsg::LoginRequest out;
        const CodecStatus status = ReflectCodec::Decode(noise, out);
        (void)status;  // 只要求不崩溃、不越界；返回值可以是任意错误。
        ++g_checks;
    }

    // RLE 随机往返。
    for (int round = 0; round < 500; ++round) {
        const std::size_t length = fuzz_rng.Range(2048);
        std::vector<std::uint8_t> input(length);
        for (auto& byte : input) {
            byte = static_cast<std::uint8_t>(fuzz_rng.Range(4) + 'a');
        }
        std::vector<std::uint8_t> compressed;
        CHECK_TRUE(RleCompress(input.data(), input.size(), compressed));
        std::vector<std::uint8_t> restored;
        CHECK_TRUE(RleDecompress(compressed.data(), compressed.size(), restored));
        CHECK_TRUE(restored == input);
    }
}

// ---------------------------------------------------------------------------
// 15. 吞吐压测
// ---------------------------------------------------------------------------

void TestThroughput() {
    const testmsg::LoginRequest message = MakeLogin();
    constexpr int kIterations = 200000;
    std::uint64_t checksum = 0;

    // ASan/UBSan 会放大每次内存操作的开销，吞吐下限按 sanitizer 构建放宽；
    // 这里只验证「没有数量级异常」的回归，具体数字在报告里给出。
#if defined(__SANITIZE_ADDRESS__)
    constexpr double kBinaryMinOps = 5000.0;
    constexpr double kTextMinOps = 2000.0;
#else
    constexpr double kBinaryMinOps = 100000.0;
    constexpr double kTextMinOps = 10000.0;
#endif

    const auto binary_start = std::chrono::steady_clock::now();
    for (int index = 0; index < kIterations; ++index) {
        std::vector<std::uint8_t> bytes;
        if (ReflectCodec::Encode(message, bytes, Encoding::kBinary) != CodecStatus::kOk) {
            CHECK_TRUE(false);
            break;
        }
        testmsg::LoginRequest out;
        if (ReflectCodec::Decode(bytes, out) != CodecStatus::kOk) {
            CHECK_TRUE(false);
            break;
        }
        checksum += static_cast<std::uint64_t>(out.timestamp) + out.user.size() + out.token.size();
    }
    const auto binary_end = std::chrono::steady_clock::now();
    const double binary_seconds = std::chrono::duration<double>(binary_end - binary_start).count();
    const double binary_ops = kIterations / binary_seconds;
    std::cout << "[INFO] binary encode+decode: " << static_cast<std::uint64_t>(binary_ops)
              << " ops/s (" << binary_seconds * 1e6 / kIterations << " us/op)\n";
    CHECK_TRUE(binary_ops > kBinaryMinOps);
    CHECK_TRUE(checksum != 0);

    constexpr int kTextIterations = 20000;
    const auto text_start = std::chrono::steady_clock::now();
    for (int index = 0; index < kTextIterations; ++index) {
        std::vector<std::uint8_t> bytes;
        if (ReflectCodec::Encode(message, bytes, Encoding::kText) != CodecStatus::kOk) {
            CHECK_TRUE(false);
            break;
        }
        testmsg::LoginRequest out;
        if (ReflectCodec::Decode(bytes, out) != CodecStatus::kOk) {
            CHECK_TRUE(false);
            break;
        }
        checksum += out.user.size();
    }
    const auto text_end = std::chrono::steady_clock::now();
    const double text_seconds = std::chrono::duration<double>(text_end - text_start).count();
    const double text_ops = kTextIterations / text_seconds;
    std::cout << "[INFO] text encode+decode: " << static_cast<std::uint64_t>(text_ops) << " ops/s ("
              << text_seconds * 1e6 / kTextIterations << " us/op)\n";
    CHECK_TRUE(text_ops > kTextMinOps);
    CHECK_TRUE(checksum != 0);
}

// ---------------------------------------------------------------------------
// 16. 真实场景
// ---------------------------------------------------------------------------

void TestRealScenarios() {
    // S1：登录流程——客户端文本请求、服务端二进制响应、全程帧。
    {
        testmsg::LoginRequest request = MakeLogin();
        request.user = "newbie";
        request.token = "otp-123456";
        request.remember = false;
        const std::vector<std::uint8_t> wire =
            ReflectFrameCodec::EncodeFrame(request, 1, Encoding::kText);
        testmsg::LoginRequest received;
        net::FrameStatus frame_status = net::FrameStatus::kOk;
        CHECK_TRUE(ReflectFrameCodec::DecodeFrame(wire.data(), wire.size(), received,
                                                  &frame_status) == CodecStatus::kOk);
        CHECK_TRUE(frame_status == net::FrameStatus::kOk);
        CHECK_TRUE(received == request);
    }

    // S2：玩家状态高频同步——1 万次随机帧编解码全部一致。
    {
        Rng rng(20261030);
        constexpr int kSamples = 10000;
        int mismatches = 0;
        for (int index = 0; index < kSamples; ++index) {
            testmsg::PlayerState state;
            state.player_id = rng.Next();
            state.x = RandomFiniteDouble(rng);
            state.y = RandomFiniteDouble(rng);
            state.hp = static_cast<std::int32_t>(rng.Range(2000));
            state.name = RandomText(rng, 12);
            state.flags = static_cast<std::uint32_t>(rng.Next());
            const std::vector<std::uint8_t> wire =
                ReflectFrameCodec::EncodeFrame(state, 2, Encoding::kBinary, Compression::kRle);
            testmsg::PlayerState out;
            if (ReflectFrameCodec::DecodeFrame(wire.data(), wire.size(), out) != CodecStatus::kOk ||
                !(out == state)) {
                ++mismatches;
            }
        }
        CHECK_EQ(mismatches, 0);
    }

    // S3：聊天长文本——压缩比 + 往返。
    {
        testmsg::ChatMessage chat;
        chat.room = "world-1";
        chat.sender = "system";
        chat.text.assign(4096, '!');
        chat.sent_at = 1730000000;
        const std::vector<std::uint8_t> plain = EncodeBytes(chat, Encoding::kBinary);
        const std::vector<std::uint8_t> packed =
            EncodeBytes(chat, Encoding::kBinary, Compression::kRle);
        CHECK_TRUE(packed.size() * 10 < plain.size());
        const std::string text = ToText(EncodeBytes(chat, Encoding::kText));
        CHECK_TRUE(text.size() > 4096);
        CHECK_TRUE(BinaryRoundTrip(chat, Compression::kRle) == chat);
        CHECK_TRUE(TextRoundTrip(chat) == chat);
    }

    // S4：配置下发——嵌套结构体 + 帧 + 压缩 + 文本调试视图。
    {
        const testmsg::ServerConfig config = MakeConfig();
        const std::vector<std::uint8_t> wire =
            ReflectFrameCodec::EncodeFrame(config, 4, Encoding::kBinary, Compression::kRle);
        testmsg::ServerConfig out;
        CHECK_TRUE(ReflectFrameCodec::DecodeFrame(wire.data(), wire.size(), out) ==
                   CodecStatus::kOk);
        CHECK_TRUE(out == config);
        const std::string debug_view = ToText(EncodeBytes(config, Encoding::kText));
        CHECK_TRUE(debug_view.find("\"max_message_bytes\":1300") != std::string::npos);
        CHECK_TRUE(debug_view.find("欢迎来到服务器") != std::string::npos);
    }

    // S5：未知消息路由——旧客户端收到新消息时能拿到类型名上报，而不是乱解析。
    {
        const std::vector<std::uint8_t> future_message =
            MakeBinaryEnvelope(5001, "NewFeatureMessage", {0x00});
        testmsg::PlayerState ignored;
        CHECK_TRUE(ReflectCodec::Decode(future_message, ignored) == CodecStatus::kUnknownType);
        TypeId id = kUnknownType;
        std::string name;
        CHECK_TRUE(ReflectCodec::PeekType(future_message.data(), future_message.size(), id, name) ==
                   CodecStatus::kOk);
        CHECK_TRUE(id == 5001);
        CHECK_TRUE(name == "NewFeatureMessage");
    }

    // S6：帧流粘包/半包——每次 7 字节喂给 FrameDecoder，跨包重组。
    {
        std::vector<std::uint8_t> stream;
        std::vector<testmsg::PlayerState> sent;
        for (int index = 0; index < 5; ++index) {
            testmsg::PlayerState state = MakePlayer();
            state.player_id = static_cast<std::uint64_t>(index + 1);
            state.hp = index * 10;
            sent.push_back(state);
            const std::vector<std::uint8_t> wire =
                ReflectFrameCodec::EncodeFrame(state, 2, Encoding::kBinary);
            stream.insert(stream.end(), wire.begin(), wire.end());
        }
        net::FrameDecoder decoder;
        std::vector<testmsg::PlayerState> received;
        std::size_t offset = 0;
        while (offset < stream.size()) {
            const std::size_t chunk = std::min<std::size_t>(7, stream.size() - offset);
            decoder.Append(stream.data() + offset, chunk);
            offset += chunk;
            net::Frame frame;
            while (decoder.Next(&frame) == net::FrameStatus::kOk) {
                testmsg::PlayerState state;
                if (ReflectFrameCodec::DecodeFrame(frame, state) == CodecStatus::kOk) {
                    received.push_back(state);
                }
            }
        }
        CHECK_EQ(received.size(), sent.size());
        CHECK_TRUE(received == sent);
    }

    // S7：错误恢复——坏帧被丢弃后，后续帧仍能正常解析。
    {
        const std::vector<std::uint8_t> good = ReflectFrameCodec::EncodeFrame(MakeLogin(), 1);
        std::vector<std::uint8_t> corrupted = good;
        corrupted.back() = static_cast<std::uint8_t>(corrupted.back() ^ 0x5a);
        net::FrameDecoder decoder;
        decoder.Append(corrupted.data(), corrupted.size());
        net::Frame frame;
        CHECK_TRUE(decoder.Next(&frame) == net::FrameStatus::kChecksumMismatch);
        // 解码器在错误后清空缓冲（帧边界不可信），重新喂入好帧必须成功。
        decoder.Append(good.data(), good.size());
        CHECK_TRUE(decoder.Next(&frame) == net::FrameStatus::kOk);
        testmsg::LoginRequest login;
        CHECK_TRUE(ReflectFrameCodec::DecodeFrame(frame, login) == CodecStatus::kOk);
        CHECK_TRUE(login == MakeLogin());
    }
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

void RunTest(const char* name, void (*function)()) {
    g_current_test = name;
    const int before = g_failures;
    function();
    if (g_failures == before) {
        std::cout << "[PASS] " << name << "\n";
    } else {
        std::cout << "[FAIL] " << name << " (" << (g_failures - before) << " failures)\n";
    }
}

}  // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"TypeIdentity", TestTypeIdentity},
        {"TypeRegistry", TestTypeRegistry},
        {"BufferVarint", TestBufferVarint},
        {"BuiltinBinaryRoundTrip", TestBuiltinBinaryRoundTrip},
        {"StructBinaryRoundTrip", TestStructBinaryRoundTrip},
        {"TextFieldFormat", TestTextFieldFormat},
        {"FieldDescriptorAccess", TestFieldDescriptorAccess},
        {"RleCompression", TestRleCompression},
        {"FrameIntegration", TestFrameIntegration},
        {"WrongType", TestWrongType},
        {"UnknownType", TestUnknownType},
        {"MalformedInput", TestMalformedInput},
        {"UnknownAndMissingFields", TestUnknownAndMissingFields},
        {"RandomRoundTrip", TestRandomRoundTrip},
        {"Throughput", TestThroughput},
        {"RealScenarios", TestRealScenarios},
    };
    for (const auto& test : tests) {
        RunTest(test.first, test.second);
    }
    std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
    return g_failures == 0 ? 0 : 1;
}
