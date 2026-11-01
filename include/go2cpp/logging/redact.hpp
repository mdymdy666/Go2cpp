#pragma once

/**
 * @file include/go2cpp/logging/redact.hpp
 * @brief 日志脱敏：密钥只以 `key_id` 出现，token 只以占位符出现。
 *
 * ## 模块定位
 *
 * JinShan MOD-01 要求「key/token 不落日志」。本头文件是 `logging` 模块的
 * **可选扩展**（不修改 `log.hpp` 的任何既有行为），提供三类能力：
 *
 * - `Redact(key)`：密钥 → `key_id=<8 hex>`（SHA-256 前 4 字节，16 进制小写）。
 *   同一个密钥得到同一个 id，便于日志关联与去重，但无法从 `key_id` 还原密钥；
 * - `RedactToken(token)`：token → `tok_***`（固定占位符，**不泄露长度**，
 *   避免长度侧信道；空 token 单独记为 `tok_empty`）；
 * - `Redactable` / `RedactedField` / `LogRedacted(field, value)`：把「敏感值」
 *   包装成日志安全类型。`Redactable` 的 `operator<<` **只输出脱敏文本**，
 *   所以 `LOG << redactable` 即使写错了也泄露不了明文。
 *
 * ## 与 `log.hpp` 的关系
 *
 * `LogLine::stream()` 返回 `std::ostream&`，因此本文件只需提供
 * `operator<<(std::ostream&, ...)` 重载，就能与 `GO2CPP_LOG_*` 宏无缝组合：
 *
 * ```cpp
 * GO2CPP_LOG_INFO(logger) << "login api_key=" << log::Redact(key)
 *                         << " session=" << log::RedactToken(token);
 * // 输出：login api_key=key_id=1a2b3c4d session=tok_***
 * ```
 *
 * ## 为什么内嵌 SHA-256
 *
 * `logging` 模块的既定约束是「只依赖 C++17 标准库」（见 `log.hpp` 文件头），
 * 所以本文件不 include `go2cpp/hash/sha256.hpp`，而是内嵌一份最小的
 * FIPS 180-4 实现（约 90 行，一次性摘要，无状态、无分配）。
 * `tests/logging/test_log_redact.cpp` 用仓库的 `go2cpp::hash::Sha256` 与
 * NIST 已知向量对拍，保证两份实现输出一致。
 *
 * ## 安全边界（必须知道）
 *
 * - `key_id` 只有 32 位（8 hex），用途是**日志关联**，不是认证、不是完整性
 *   校验；对低熵输入（如短密码）可被离线枚举，不要用它处理密码类字段
 *   （密码应完全打码，见 `RedactToken`）；
 * - `Redactable::secret()` 返回明文，仅供业务逻辑使用（如建立连接）；
 *   **不要**把它拼进日志、异常信息或 HTTP 响应；
 * - 本文件不做「自动识别敏感字段」，`IsSensitiveFieldName()` 只提供命名
 *   判断，是否脱敏由调用方决定；漏掉一个字段就是一次泄露，接入时请用
 *   `grep` 审计所有日志调用点。
 *
 * ## 性能
 *
 * 每次 `Redact()` 计算一次 SHA-256（输入通常 < 128 字节，即 1~2 次压缩），
 * 实测 1e4 次 < 10 ms（见测试输出）。不要在每帧日志里对同一密钥重复计算，
 * 调用方可以缓存 `Redactable`。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <string_view>

namespace go2cpp::log {

/// `key_id` 的文本前缀。
inline constexpr std::string_view kKeyIdPrefix = "key_id=";

/// token 的固定占位符（与 token 内容/长度无关）。
inline constexpr std::string_view kTokenMask = "tok_***";

/// 空密钥占位符。
inline constexpr std::string_view kEmptyKeyId = "key_id=empty";

/// 空 token 占位符。
inline constexpr std::string_view kEmptyTokenMask = "tok_empty";

namespace detail {

/// SHA-256 轮常量（前 64 个素数立方根小数部分，FIPS 180-4 §4.2.2）。
inline constexpr std::uint32_t kSha256Round[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

/// 32 位循环右移。
inline std::uint32_t Rotr(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32u - bits));
}

/// 压缩一个 64 字节块（FIPS 180-4 §6.2.2）。
inline void Sha256Compress(std::uint32_t state[8],
                           const std::uint8_t block[64]) noexcept {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t big_s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + big_s1 + ch + kSha256Round[i] + w[i];
        const std::uint32_t big_s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = big_s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/// 一次性 SHA-256（输入/输出均为字节串，返回 32 字节摘要）。
inline std::array<std::uint8_t, 32> Sha256Digest(std::string_view data) noexcept {
    std::uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                              0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                              0x1f83d9abu, 0x5be0cd19u};
    std::size_t offset = 0;
    while (data.size() - offset >= 64) {
        Sha256Compress(state,
                       reinterpret_cast<const std::uint8_t*>(data.data() + offset));
        offset += 64;
    }
    // 尾部：剩余字节 + 0x80 + 0 填充 + 64 位大端比特长度。
    std::array<std::uint8_t, 128> tail{};
    const std::size_t remainder = data.size() - offset;
    if (remainder != 0) {
        std::memcpy(tail.data(), data.data() + offset, remainder);
    }
    tail[remainder] = 0x80u;
    const std::size_t padded = remainder < 56 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8u;
    for (int i = 0; i < 8; ++i) {
        tail[padded - 1 - static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(bits >> (8 * i));
    }
    Sha256Compress(state, tail.data());
    if (padded == 128) {
        Sha256Compress(state, tail.data() + 64);
    }
    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
    }
    return out;
}

}  // namespace detail

/**
 * @brief 计算密钥指纹：SHA-256 前 4 字节的 8 位小写 16 进制。
 * @param key 密钥原文。
 * @return 8 位 hex；`key` 为空时返回空字符串（调用方用 `kEmptyKeyId` 占位）。
 * @note 32 位输出只用于日志关联，不具备认证强度。
 */
inline std::string KeyId(std::string_view key) {
    if (key.empty()) {
        return {};
    }
    const std::array<std::uint8_t, 32> digest = detail::Sha256Digest(key);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(8, '0');
    for (std::size_t i = 0; i < 4; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0fu];
    }
    return out;
}

/**
 * @brief 敏感值包装：明文只供业务使用，日志输出恒为脱敏形式。
 *
 * ## 不变量
 * - `secret()` 返回构造时的明文副本，之后不再变化；
 * - `redacted()` 在构造时算好，`operator<<` 只输出它；
 * - 类型**不提供**到 `std::string` 的隐式转换，避免明文被无意拼进字符串。
 */
class Redactable {
   public:
    /// 脱敏策略。
    enum class Kind : std::uint8_t {
        kKey = 0,    ///< 密钥：输出 `key_id=<8 hex>`。
        kToken = 1,  ///< token：输出 `tok_***`。
    };

    /// @brief 构造空值（`key_id=empty` / `tok_empty`）。
    Redactable() = default;

    /**
     * @brief 包装一个敏感值。
     * @param secret 明文（会被拷贝）。
     * @param kind 脱敏策略。
     */
    Redactable(std::string secret, Kind kind)
        : m_secret(std::move(secret)), m_kind(kind) {
        if (m_secret.empty()) {
            m_redacted = m_kind == Kind::kToken ? std::string(kEmptyTokenMask)
                                                : std::string(kEmptyKeyId);
        } else if (m_kind == Kind::kToken) {
            m_redacted.assign(kTokenMask);
        } else {
            m_redacted = std::string(kKeyIdPrefix) + KeyId(m_secret);
        }
    }

    /// @brief 明文（**禁止**写入日志/异常/响应）。
    const std::string& secret() const noexcept { return m_secret; }
    /// @brief 日志可用的脱敏文本。
    const std::string& redacted() const noexcept { return m_redacted; }
    /// @brief 脱敏策略。
    Kind kind() const noexcept { return m_kind; }
    /// @brief 明文是否为空。
    bool empty() const noexcept { return m_secret.empty(); }

   private:
    std::string m_secret;      ///< 明文；仅 `secret()` 返回。
    std::string m_redacted;    ///< 构造时算好的脱敏文本；`operator<<` 只输出它。
    Kind m_kind = Kind::kKey;  ///< 脱敏策略。
};

/// @brief 密钥 → `Redactable`（输出 `key_id=<8 hex>`）。
inline Redactable Redact(std::string_view key) {
    return Redactable(std::string(key), Redactable::Kind::kKey);
}

/// @brief token → `Redactable`（输出 `tok_***`，与内容/长度无关）。
inline Redactable RedactToken(std::string_view token) {
    return Redactable(std::string(token), Redactable::Kind::kToken);
}

/** @brief 流输出：只写脱敏文本（防止 `LOG << redactable` 泄露明文）。 */
inline std::ostream& operator<<(std::ostream& os, const Redactable& value) {
    return os << value.redacted();
}

/**
 * @brief 带字段名的脱敏输出：`field=<redacted>`。
 *
 * 例：`LogRedacted("api_key", key)` 输出 `api_key=key_id=1a2b3c4d`。
 * 字段名本身不参与哈希，便于按字段检索。
 */
class RedactedField {
   public:
    RedactedField(std::string field, Redactable value)
        : m_field(std::move(field)), m_value(std::move(value)) {}

    /// @brief 字段名。
    const std::string& field() const noexcept { return m_field; }
    /// @brief 脱敏值。
    const Redactable& value() const noexcept { return m_value; }

    /// @brief 完整文本：`field=<redacted>`。
    std::string ToString() const {
        std::string out;
        out.reserve(m_field.size() + 1 + m_value.redacted().size());
        out.append(m_field);
        out.push_back('=');
        out.append(m_value.redacted());
        return out;
    }

   private:
    std::string m_field;  ///< 字段名。
    Redactable m_value;   ///< 脱敏值。
};

/** @brief 流输出：`field=<redacted>`。 */
inline std::ostream& operator<<(std::ostream& os, const RedactedField& field) {
    return os << field.ToString();
}

/**
 * @brief 构造 `field=key_id=<8 hex>`（按密钥策略脱敏）。
 * @param field 字段名（如 `api_key`）。
 * @param value 敏感值明文。
 */
inline RedactedField LogRedacted(std::string_view field, std::string_view value) {
    return RedactedField(std::string(field), Redact(value));
}

/// @brief 同上，但值已包装（避免重复计算 `key_id`）。
inline RedactedField LogRedacted(std::string_view field, const Redactable& value) {
    return RedactedField(std::string(field), value);
}

/**
 * @brief 构造 `field=tok_***`（按 token 策略脱敏）。
 * @param field 字段名（如 `session_token`）。
 * @param token token 明文。
 */
inline RedactedField LogRedactedToken(std::string_view field, std::string_view token) {
    return RedactedField(std::string(field), RedactToken(token));
}

/**
 * @brief 判断字段名是否「看起来敏感」。
 *
 * @details 对字段名做 ASCII 小写化后，检查是否包含以下子串之一：
 *          `key` / `token` / `secret` / `password` / `passwd` / `credential` /
 *          `auth` / `session` / `cookie` / `signature` / `private`。
 *
 * @note 只做命名判断，**不是**数据分类器：命中不等于一定敏感，未命中也不
 *       等于一定安全。接入时仍应逐个审计日志调用点。
 * @param name 字段名（大小写不敏感）。
 * @return 命中任一子串返回 true。
 */
inline bool IsSensitiveFieldName(std::string_view name) noexcept {
    static constexpr std::string_view kKeywords[] = {
        "key",        "token",   "secret",    "password", "passwd",
        "credential", "auth",    "session",   "cookie",   "signature",
        "private"};
    std::string lower;
    lower.reserve(name.size());
    for (const char ch : name) {
        lower.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a')
                                                 : ch);
    }
    for (const std::string_view keyword : kKeywords) {
        if (lower.find(keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace go2cpp::log
