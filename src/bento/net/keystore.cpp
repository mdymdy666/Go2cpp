/**
 * @file keystore.cpp
 * @brief KeyStore 实现：hex/base64 解码、文件权限告警、轮换与常量时间比较。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/keystore.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace go2cpp::bento {
namespace {

/// 单调毫秒时钟：轮换窗口不受系统时间回拨影响。
std::uint64_t NowMs() noexcept {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

bool IsKeyLength(std::size_t bytes) noexcept {
    return bytes == kKeyBytes128 || bytes == kKeyBytes256;
}

char ToLowerAscii(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (ToLowerAscii(text[i]) != ToLowerAscii(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool IsAsciiWhitespace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' ||
           value == '\v';
}

std::string StripWhitespace(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (const char value : text) {
        if (!IsAsciiWhitespace(value)) {
            stripped.push_back(value);
        }
    }
    return stripped;
}

int HexValue(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

/// 仅当串非空、偶数长度且全部是 hex 字符时返回 true。
bool IsHexText(std::string_view text) noexcept {
    if (text.empty() || (text.size() % 2U) != 0) {
        return false;
    }
    for (const char value : text) {
        if (HexValue(value) < 0) {
            return false;
        }
    }
    return true;
}

int Base64Value(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

error::ErrorPtr DecodeHex(std::string_view text, std::vector<std::uint8_t>* out) {
    if (text.empty()) {
        return error::NewError("keystore: hex key is empty", error::ErrorCode::kInvalidArgument);
    }
    if ((text.size() % 2U) != 0) {
        return error::NewError("keystore: hex key has an odd number of characters",
                               error::ErrorCode::kInvalidArgument);
    }
    std::vector<std::uint8_t> bytes(text.size() / 2U);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const int high = HexValue(text[i * 2U]);
        const int low = HexValue(text[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return error::NewError("keystore: hex key contains a non-hex character",
                                   error::ErrorCode::kInvalidArgument);
        }
        bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    *out = std::move(bytes);
    return nullptr;
}

error::ErrorPtr DecodeBase64(std::string_view text, std::vector<std::uint8_t>* out) {
    if (text.empty()) {
        return error::NewError("keystore: base64 key is empty", error::ErrorCode::kInvalidArgument);
    }
    std::size_t data_bytes = text.size();
    std::size_t padding = 0;
    while (data_bytes > 0 && text[data_bytes - 1] == '=') {
        ++padding;
        --data_bytes;
    }
    if (padding > 2) {
        return error::NewError("keystore: base64 key has too much padding",
                               error::ErrorCode::kInvalidArgument);
    }
    if (data_bytes == 0 || (data_bytes % 4U) == 1U) {
        return error::NewError("keystore: base64 key has an invalid length",
                               error::ErrorCode::kInvalidArgument);
    }
    if (padding != 0 && ((data_bytes + padding) % 4U) != 0) {
        return error::NewError("keystore: base64 key has inconsistent padding",
                               error::ErrorCode::kInvalidArgument);
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve((data_bytes * 3U) / 4U + 1U);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (std::size_t i = 0; i < data_bytes; ++i) {
        const int value = Base64Value(text[i]);
        if (value < 0) {
            return error::NewError("keystore: base64 key contains an invalid character",
                                   error::ErrorCode::kInvalidArgument);
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFFU));
        }
    }
    *out = std::move(bytes);
    return nullptr;
}

/// 解析一段编码文本；错误信息只描述格式，不回显输入。
error::ErrorPtr ParseKeyText(std::string_view raw, std::size_t max_bytes, KeyStore::Key* out) {
    if (raw.size() > max_bytes) {
        return error::NewError("keystore: encoded key exceeds the input limit",
                               error::ErrorCode::kResourceExhausted);
    }
    const std::string text = StripWhitespace(raw);
    if (text.empty()) {
        return error::NewError("keystore: empty key input", error::ErrorCode::kInvalidArgument);
    }

    bool forced_hex = false;
    bool forced_base64 = false;
    std::string_view body(text);
    if (StartsWithIgnoreCase(text, "hex:")) {
        forced_hex = true;
        body.remove_prefix(4);
    } else if (StartsWithIgnoreCase(text, "base64:")) {
        forced_base64 = true;
        body.remove_prefix(7);
    }
    const std::string normalized = StripWhitespace(body);
    if (normalized.empty()) {
        return error::NewError("keystore: key body is empty", error::ErrorCode::kInvalidArgument);
    }

    std::vector<std::uint8_t> bytes;
    error::ErrorPtr decode_error;
    if (forced_hex) {
        decode_error = DecodeHex(normalized, &bytes);
    } else if (forced_base64) {
        decode_error = DecodeBase64(normalized, &bytes);
    } else if (IsHexText(normalized)) {
        decode_error = DecodeHex(normalized, &bytes);
    } else {
        decode_error = DecodeBase64(normalized, &bytes);
    }
    if (decode_error) {
        return decode_error;
    }
    if (!IsKeyLength(bytes.size())) {
        std::string message = "keystore: key length ";
        message += std::to_string(bytes.size());
        message += " bytes is not 16 or 32";
        return error::NewError(std::move(message), error::ErrorCode::kInvalidArgument);
    }
    *out = KeyStore::Key::FromBytes(bytes.data(), bytes.size());
    if (!out->valid()) {
        return error::NewError("keystore: failed to construct key", error::ErrorCode::kInternal);
    }
    return nullptr;
}

std::string SystemErrorText(int code) {
    return std::error_code(code, std::generic_category()).message();
}

/// 打开失败时把 errno 映射成稳定的 error 语义。
error::ErrorCode OpenErrorCode(int code) noexcept {
    switch (code) {
        case EACCES:
        case EPERM:
            return error::ErrorCode::kPermissionDenied;
        case ENOENT:
        case ENOTDIR:
            return error::ErrorCode::kNotFound;
        default:
            return error::ErrorCode::kUnavailable;
    }
}

}  // namespace

KeyStore::Key KeyStore::Key::FromBytes(const std::uint8_t* data, std::size_t bytes) {
    Key key;
    if (data == nullptr || !IsKeyLength(bytes)) {
        return key;
    }
    std::memcpy(key.m_bytes.data(), data, bytes);
    key.m_bytes_used = bytes;
    return key;
}

bool KeyStore::Key::valid() const noexcept {
    return IsKeyLength(m_bytes_used);
}

std::size_t KeyStore::Key::size() const noexcept {
    return m_bytes_used;
}

const std::uint8_t* KeyStore::Key::data() const noexcept {
    return m_bytes.data();
}

std::string KeyStore::Key::ToHex() const {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string hex(m_bytes_used * 2U, '\0');
    for (std::size_t i = 0; i < m_bytes_used; ++i) {
        hex[i * 2U] = kHexDigits[m_bytes[i] >> 4U];
        hex[i * 2U + 1U] = kHexDigits[m_bytes[i] & 0x0FU];
    }
    return hex;
}

std::string KeyStore::Key::Masked() const {
    if (!valid()) {
        return "<empty>";
    }
    const std::string hex = ToHex();
    if (hex.size() <= 8U) {
        // 16/32B 的十六进制串至少 32 字符，这里只是防御未来长度策略变化。
        return std::string(hex.size(), '*');
    }
    std::string masked;
    masked.reserve(hex.size());
    masked.append(hex, 0, 4);
    masked.append(hex.size() - 8U, '*');
    masked.append(hex, hex.size() - 4U, 4);
    return masked;
}

KeyStore::KeyStore() : KeyStore(Config{}) {}

KeyStore::KeyStore(Config config) : m_config(config) {
    if (m_config.max_input_bytes == 0) {
        m_config.max_input_bytes = 4096;
    }
}

error::ErrorPtr KeyStore::LoadFromString(std::string_view text) {
    return LoadDecoded(text, "string");
}

error::ErrorPtr KeyStore::LoadFromEnv(const std::string& variable) {
    const char* value = std::getenv(variable.c_str());
    if (value == nullptr) {
        return error::NewError("keystore: environment variable '" + variable + "' is not set",
                               error::ErrorCode::kNotFound);
    }
    if (*value == '\0') {
        return error::NewError("keystore: environment variable '" + variable + "' is empty",
                               error::ErrorCode::kInvalidArgument);
    }
    return LoadDecoded(value, "environment");
}

error::ErrorPtr KeyStore::LoadFromFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const int saved_errno = errno;
        std::string message = "keystore: cannot open key file '";
        message += path;
        message += "': ";
        message += SystemErrorText(saved_errno);
        return error::NewError(std::move(message), OpenErrorCode(saved_errno));
    }

    // RAII：任何返回路径都关闭 fd，避免加载失败时泄漏描述符。
    struct FdGuard final {
        int fd = -1;
        ~FdGuard() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    } guard{fd};

    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        const int saved_errno = errno;
        std::string message = "keystore: cannot stat key file '";
        message += path;
        message += "': ";
        message += SystemErrorText(saved_errno);
        return error::NewError(std::move(message), error::ErrorCode::kInternal);
    }
    if (!S_ISREG(info.st_mode)) {
        return error::NewError("keystore: key file is not a regular file: '" + path + "'",
                               error::ErrorCode::kInvalidArgument);
    }
    if (info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > m_config.max_input_bytes) {
        std::string message = "keystore: key file exceeds the input limit of ";
        message += std::to_string(m_config.max_input_bytes);
        message += " bytes: '";
        message += path;
        message += "'";
        return error::NewError(std::move(message), error::ErrorCode::kResourceExhausted);
    }
    if (m_config.check_file_permissions && (info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        std::ostringstream stream;
        stream << "keystore: warning: key file '" << path << "' mode " << std::oct
               << std::setw(4) << std::setfill('0')
               << static_cast<unsigned int>(info.st_mode & 07777U)
               << " is more permissive than 0600";
        AddWarning(stream.str());
    }

    const std::size_t content_size = static_cast<std::size_t>(info.st_size);
    std::string content(content_size, '\0');
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t read_bytes = ::read(fd, content.data() + offset, content.size() - offset);
        if (read_bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int saved_errno = errno;
            std::string message = "keystore: failed to read key file '";
            message += path;
            message += "': ";
            message += SystemErrorText(saved_errno);
            return error::NewError(std::move(message), error::ErrorCode::kDataLoss);
        }
        if (read_bytes == 0) {
            break;
        }
        offset += static_cast<std::size_t>(read_bytes);
    }
    if (offset != content.size()) {
        return error::NewError("keystore: key file changed while reading: '" + path + "'",
                               error::ErrorCode::kDataLoss);
    }
    if (content.empty()) {
        return error::NewError("keystore: key file is empty: '" + path + "'",
                               error::ErrorCode::kInvalidArgument);
    }
    return LoadDecoded(content, "file");
}

bool KeyStore::loaded() const noexcept {
    return m_current.valid();
}

const KeyStore::Key& KeyStore::Current() const noexcept {
    return m_current;
}

const KeyStore::Key& KeyStore::Previous() const noexcept {
    if (HasUsablePrevious()) {
        return m_previous;
    }
    static const Key kEmptyKey;
    return kEmptyKey;
}

bool KeyStore::HasUsablePrevious() const noexcept {
    if (!m_previous.valid()) {
        return false;
    }
    const std::uint64_t now = NowMs();
    if (now < m_rotated_at_ms) {
        return false;
    }
    return (now - m_rotated_at_ms) < m_config.rotation_retention_ms;
}

bool KeyStore::Accepts(const Key& candidate) const noexcept {
    if (!candidate.valid()) {
        return false;
    }
    const bool current_match = ConstantTimeEquals(m_current, candidate);
    // 窗口状态与候选内容无关；窗口内总是比较上一代，避免按内容跳过。
    bool previous_match = false;
    if (HasUsablePrevious()) {
        previous_match = ConstantTimeEquals(m_previous, candidate);
    }
    return current_match || previous_match;
}

bool KeyStore::Rotate(const Key& new_key) {
    if (!new_key.valid()) {
        Emit("keystore: rotate rejected: invalid key");
        return false;
    }
    const Key old = m_current;
    m_current = new_key;
    if (old.valid()) {
        m_previous = old;
        m_rotated_at_ms = NowMs();
    } else {
        m_previous = Key{};
        m_rotated_at_ms = 0;
    }

    std::string message = "keystore: rotated; current=";
    message += m_current.Masked();
    message += " previous=";
    message += m_previous.valid() ? m_previous.Masked() : "<none>";
    message += " retention_ms=";
    message += std::to_string(m_config.rotation_retention_ms);
    Emit(message);
    return true;
}

void KeyStore::SetRotationRetentionMs(std::uint64_t value) noexcept {
    m_config.rotation_retention_ms = value;
}

std::uint64_t KeyStore::rotation_retention_ms() const noexcept {
    return m_config.rotation_retention_ms;
}

void KeyStore::SetLogSink(LogSink sink) {
    m_log_sink = std::move(sink);
}

const std::vector<std::string>& KeyStore::warnings() const noexcept {
    return m_warnings;
}

void KeyStore::ClearWarnings() noexcept {
    m_warnings.clear();
}

bool KeyStore::ConstantTimeEquals(const Key& a, const Key& b) noexcept {
    return ConstantTimeEquals(a.data(), a.size(), b.data(), b.size());
}

bool KeyStore::ConstantTimeEquals(const std::uint8_t* a, std::size_t a_bytes,
                                  const std::uint8_t* b, std::size_t b_bytes) noexcept {
    if (a == nullptr || b == nullptr) {
        return a_bytes == 0 && b_bytes == 0;
    }
    const std::size_t max_bytes = std::max(a_bytes, b_bytes);
    unsigned int difference = (a_bytes == b_bytes) ? 0U : 1U;
    for (std::size_t i = 0; i < max_bytes; ++i) {
        const unsigned int left = (i < a_bytes) ? a[i] : 0U;
        const unsigned int right = (i < b_bytes) ? b[i] : 0U;
        difference |= (left ^ right);
    }
    return difference == 0;
}

error::ErrorPtr KeyStore::LoadDecoded(std::string_view text, const char* source) {
    Key key;
    error::ErrorPtr parse_error = ParseKeyText(text, m_config.max_input_bytes, &key);
    if (parse_error) {
        std::string message = "keystore: load from ";
        message += source;
        message += " failed: ";
        message += error::ErrorMessage(parse_error);
        Emit(message);
        return error::Wrap(std::move(parse_error),
                           std::string("keystore: load from ") + source + " failed");
    }

    m_current = key;
    m_previous = Key{};
    m_rotated_at_ms = 0;

    std::string message = "keystore: loaded ";
    message += std::to_string(m_current.size());
    message += "-byte key from ";
    message += source;
    message += "; masked=";
    message += m_current.Masked();
    Emit(message);
    return nullptr;
}

void KeyStore::AddWarning(std::string message) {
    m_warnings.push_back(message);
    Emit(m_warnings.back());
}

void KeyStore::Emit(std::string_view message) const {
    if (!m_log_sink) {
        return;
    }
    try {
        m_log_sink(message);
    } catch (...) {
        // 日志是旁路：回调异常不能把「密钥已成功加载」变成失败。
        ++m_sink_failures;
    }
}

}  // namespace go2cpp::bento
