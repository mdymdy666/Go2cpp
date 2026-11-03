/**
 * @file douyin_auth.hpp
 * @brief 抖音 P0 鉴权：密码散列 + Bearer token 签发/校验。
 *
 * @details 密码：`SHA-256(salt + password)` 的 hex（不落明文）。token：
 *          `base64url("<user_id>.<expires>")." + hex(HMAC-SHA256(secret, payload))`，
 *          secret 从环境变量 `DOUYIN_SECRET` 读取，缺省用固定 demo 值。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace douyin {

/// @brief token 有效期（秒）：7 天。
inline constexpr std::int64_t kTokenTtlSeconds = 7 * 24 * 60 * 60;

/// @brief 每用户盐长度（字节）。
inline constexpr std::size_t kSaltBytes = 16;

/// @brief 取签名密钥（环境变量 `DOUYIN_SECRET`，缺省固定 demo 值）。
std::string TokenSecret();

/// @brief 生成 `bytes` 字节随机盐并 hex 编码；随机源失败时退回时间/计数器。
std::string RandomSaltHex(std::size_t bytes = kSaltBytes);

/// @brief `SHA-256(salt + password)` 的 hex。
std::string HashPassword(const std::string& password, const std::string& salt);

/// @brief 校验密码（常量时间比较 hex 串）。
bool VerifyPassword(const std::string& password, const std::string& salt,
                    const std::string& expected_hash);

/// @brief HMAC-SHA256 的 hex（公开给测试与网关复用）。
std::string HmacSha256Hex(const std::string& key, const std::string& data);

/// @brief 签发 token；`now` 为 Unix 秒，过期时间 = now + kTokenTtlSeconds。
std::string IssueToken(std::int64_t user_id, std::int64_t now);

/// @brief token 解析结果。
struct TokenInfo {
    std::int64_t user_id{0};  ///< 用户 id。
    std::int64_t expires{0};  ///< 过期时间（Unix 秒）。
};

/**
 * @brief 校验 token：签名正确且未过期才返回 true。
 * @param token 客户端传来的 token。
 * @param now 当前 Unix 秒。
 * @param out 成功时填 user_id / expires（可为空）。
 * @param error 失败原因（签名非法 / 已过期 / 格式错误）。
 */
bool VerifyToken(const std::string& token, std::int64_t now, TokenInfo* out,
                 std::string* error);

}  // namespace douyin
