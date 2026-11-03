/**
 * @file douyin_auth.cpp
 * @brief 密码散列与 token 签发/校验实现（SHA-256 / HMAC-SHA256 / base64url）。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_auth.hpp"

#include "go2cpp/encoding/base64.hpp"
#include "go2cpp/hash/hash.hpp"
#include "go2cpp/hash/sha256.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace douyin {
namespace {

/// @brief 缺省 demo 密钥（生产应由 DOUYIN_SECRET 覆盖）。
constexpr const char* kDefaultSecret = "douyin-demo-secret-v1";

/// @brief HMAC 的分组长度（SHA-256 块 64 字节）。
constexpr std::size_t kHmacBlockBytes = 64;

/// @brief 字节序列 → 小写 hex。
std::string ToHex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kHexDigits[data[i] >> 4];
        out[i * 2 + 1] = kHexDigits[data[i] & 0x0f];
    }
    return out;
}

/// @brief 常量时间比较（长度不同直接失败；长度相同按字节 XOR 累加）。
bool ConstantTimeEqual(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff = static_cast<unsigned char>(
            diff | static_cast<unsigned char>(left[i] ^ right[i]));
    }
    return diff == 0;
}

/// @brief 一次性 SHA-256（32 字节）。
std::array<std::uint8_t, go2cpp::hash::Sha256::kSize> Digest(std::string_view data) {
    return go2cpp::hash::Sha256::Digest(go2cpp::hash::AsBytes(data));
}

/// @brief HMAC-SHA256（RFC 2104）：ipad/opad 两层哈希。
std::array<std::uint8_t, go2cpp::hash::Sha256::kSize> Hmac(std::string_view key,
                                                           std::string_view data) {
    std::array<std::uint8_t, kHmacBlockBytes> key_block{};
    if (key.size() > kHmacBlockBytes) {
        const auto digest = Digest(key);
        std::memcpy(key_block.data(), digest.data(), digest.size());
    } else if (!key.empty()) {
        std::memcpy(key_block.data(), key.data(), key.size());
    }

    std::string inner;
    inner.reserve(kHmacBlockBytes + data.size());
    for (const std::uint8_t byte : key_block) {
        inner.push_back(static_cast<char>(byte ^ 0x36U));
    }
    inner.append(data.data(), data.size());
    const auto inner_digest = Digest(inner);

    std::string outer;
    outer.reserve(kHmacBlockBytes + inner_digest.size());
    for (const std::uint8_t byte : key_block) {
        outer.push_back(static_cast<char>(byte ^ 0x5cU));
    }
    outer.append(reinterpret_cast<const char*>(inner_digest.data()),
                 inner_digest.size());
    return Digest(outer);
}

/// @brief 严格解析十进制 int64（整段必须被消费）。
bool ParseInt64(std::string_view text, std::int64_t* out) {
    if (text.empty()) {
        return false;
    }
    std::int64_t value = 0;
    const char* const begin = text.data();
    const char* const end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc() || parsed.ptr != end) {
        return false;
    }
    *out = value;
    return true;
}

}  // namespace

std::string TokenSecret() {
    const char* secret = std::getenv("DOUYIN_SECRET");
    if (secret == nullptr || *secret == '\0') {
        return std::string(kDefaultSecret);
    }
    return std::string(secret);
}

std::string RandomSaltHex(std::size_t bytes) {
    std::vector<std::uint8_t> buffer(bytes);
    std::random_device device;
    for (std::uint8_t& byte : buffer) {
        byte = static_cast<std::uint8_t>(device() & 0xffU);
    }
    return ToHex(buffer.data(), buffer.size());
}

std::string HashPassword(const std::string& password, const std::string& salt) {
    const std::string material = salt + password;
    const auto digest = Digest(material);
    return ToHex(digest.data(), digest.size());
}

bool VerifyPassword(const std::string& password, const std::string& salt,
                    const std::string& expected_hash) {
    return ConstantTimeEqual(HashPassword(password, salt), expected_hash);
}

std::string HmacSha256Hex(const std::string& key, const std::string& data) {
    const auto digest = Hmac(key, data);
    return ToHex(digest.data(), digest.size());
}

std::string IssueToken(std::int64_t user_id, std::int64_t now) {
    const std::int64_t expires = now + kTokenTtlSeconds;
    const std::string payload = std::to_string(user_id) + "." + std::to_string(expires);
    const std::string encoded =
        go2cpp::encoding::RawURLEncoding.EncodeToString(std::string_view(payload));
    return encoded + "." + HmacSha256Hex(TokenSecret(), payload);
}

bool VerifyToken(const std::string& token, std::int64_t now, TokenInfo* out,
                 std::string* error) {
    const std::size_t dot = token.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= token.size()) {
        if (error != nullptr) {
            *error = "token 格式非法";
        }
        return false;
    }
    const std::string encoded = token.substr(0, dot);
    const std::string signature = token.substr(dot + 1);

    const auto decoded =
        go2cpp::encoding::RawURLEncoding.DecodeString(std::string_view(encoded));
    if (!decoded || decoded.data.empty()) {
        if (error != nullptr) {
            *error = "token 编码非法";
        }
        return false;
    }
    const std::string& payload = decoded.data;
    const std::string expected = HmacSha256Hex(TokenSecret(), payload);
    if (!ConstantTimeEqual(signature, expected)) {
        if (error != nullptr) {
            *error = "token 签名非法";
        }
        return false;
    }

    const std::size_t separator = payload.find('.');
    if (separator == std::string::npos) {
        if (error != nullptr) {
            *error = "token 载荷非法";
        }
        return false;
    }
    TokenInfo info;
    if (!ParseInt64(std::string_view(payload).substr(0, separator), &info.user_id) ||
        !ParseInt64(std::string_view(payload).substr(separator + 1), &info.expires)) {
        if (error != nullptr) {
            *error = "token 载荷非法";
        }
        return false;
    }
    if (info.expires <= now) {
        if (error != nullptr) {
            *error = "token 已过期";
        }
        return false;
    }
    if (out != nullptr) {
        *out = info;
    }
    return true;
}

}  // namespace douyin
