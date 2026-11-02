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
 * ## 为什么复用 `hash` 模块
 *
 * 本文件原先内嵌一份最小的 FIPS 180-4 实现（约 90 行），理由是 `logging`
 * 只依赖标准库。2026-11-02 统一：仓库的 SHA-256 已收敛为
 * `go2cpp::hash::Sha256` 单一实现（标量 + SHA-NI 运行时分派），三份副本
 * 的漂移风险大于这点编译耦合。`redact.hpp` 是 `logging` 的**可选扩展**
 * （`log.hpp` 本身仍只依赖标准库），因此这里直接 include
 * `go2cpp/hash/sha256.hpp`，并调用一次性入口 `Sha256::Digest`（无对象
 * 拷贝、无分配）。
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
 * 实测约 221 ns/次（Release，1e5 次；SHA-NI 机器上更低）。不要在每帧日志
 * 里对同一密钥重复计算，调用方可以缓存 `Redactable`。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/encoding/hex_core.hpp"
#include "go2cpp/hash/hash.hpp"
#include "go2cpp/hash/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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
    const std::array<std::uint8_t, go2cpp::hash::Sha256::kSize> digest =
        go2cpp::hash::Sha256::Digest(go2cpp::hash::AsBytes(key));
    // 只取摘要前 4 字节；编码走零依赖的 hex_core（不引入 error/链接依赖）。
    return encoding::hexcore::Encode(
        std::string_view(reinterpret_cast<const char*>(digest.data()), 4));
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
