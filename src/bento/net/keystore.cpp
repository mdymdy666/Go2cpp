/**
 * @file keystore.cpp
 * @brief KeyStore 实现：hex/base64 解码、文件权限告警、轮换与常量时间比较。
 * @details 性能与安全要点：
 *          - 成功加载路径尽量零堆分配：输入无空白时直接在调用方缓冲上解析，
 *            解码写入 32B 栈缓冲，不再经 `std::vector`；
 *          - 未安装 `LogSink` 时不构造日志字符串（`Masked()`/`to_string`），
 *            热加载路径因此不再产生日志临时分配；
 *          - 密钥缓冲、文件内容与解码中间缓冲在离开作用域前用
 *            `explicit_bzero` 清零（编译器不会把它优化成死存储）。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/keystore.hpp"

#include <algorithm>
#include <array>
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

/// 保证不被优化掉的内存清零（glibc `explicit_bzero`，按字写 + 编译器屏障）。
void SecureClear(void* data, std::size_t bytes) noexcept {
    if (data == nullptr || bytes == 0U) {
        return;
    }
    ::explicit_bzero(data, bytes);
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

bool ContainsAsciiWhitespace(std::string_view text) noexcept {
    for (const char value : text) {
        if (IsAsciiWhitespace(value)) {
            return true;
        }
    }
    return false;
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

/// 统一「长度不是 16/32」错误；错误文本只含长度，不回显输入。
error::ErrorPtr MakeLengthError(std::size_t bytes) {
    std::string message = "keystore: key length ";
    message += std::to_string(bytes);
    message += " bytes is not 16 or 32";
    return error::NewError(std::move(message), error::ErrorCode::kInvalidArgument);
}

/// 解码 hex 到 `out`（容量至少 kKeyMaxBytes）。
/// @details 先完整校验字符，再检查长度，最后写入；错误顺序与旧实现一致
///          （非 hex 字符优先于长度错误）。`out_bytes` 仅在成功时写入。
error::ErrorPtr DecodeHex(std::string_view text, std::uint8_t* out, std::size_t* out_bytes) {
    if (text.empty()) {
        return error::NewError("keystore: hex key is empty", error::ErrorCode::kInvalidArgument);
    }
    if ((text.size() % 2U) != 0) {
        return error::NewError("keystore: hex key has an odd number of characters",
                               error::ErrorCode::kInvalidArgument);
    }
    const std::size_t bytes = text.size() / 2U;
    for (std::size_t i = 0; i < bytes; ++i) {
        if (HexValue(text[i * 2U]) < 0 || HexValue(text[i * 2U + 1U]) < 0) {
            return error::NewError("keystore: hex key contains a non-hex character",
                                   error::ErrorCode::kInvalidArgument);
        }
    }
    if (!IsKeyLength(bytes)) {
        return MakeLengthError(bytes);
    }
    for (std::size_t i = 0; i < bytes; ++i) {
        const int high = HexValue(text[i * 2U]);
        const int low = HexValue(text[i * 2U + 1U]);
        out[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    *out_bytes = bytes;
    return nullptr;
}

/// 解码 base64 到 `out`（容量至少 kKeyMaxBytes）。
/// @details 先校验 padding/长度/字符，再按解码后的字节数做 16/32 检查；
///          与旧实现一样接受无 padding 与非规范尾部填充位。
error::ErrorPtr DecodeBase64(std::string_view text, std::uint8_t* out, std::size_t* out_bytes) {
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
    for (std::size_t i = 0; i < data_bytes; ++i) {
        if (Base64Value(text[i]) < 0) {
            return error::NewError("keystore: base64 key contains an invalid character",
                                   error::ErrorCode::kInvalidArgument);
        }
    }

    // 每 6 bit 一个字符，向下取整即解码字节数（尾部 2/3 字符分别产 1/2 字节）。
    const std::size_t decoded_bytes = (data_bytes * 6U) / 8U;
    if (!IsKeyLength(decoded_bytes)) {
        return MakeLengthError(decoded_bytes);
    }

    std::uint32_t accumulator = 0;
    int bits = 0;
    std::size_t written = 0;
    for (std::size_t i = 0; i < data_bytes; ++i) {
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(Base64Value(text[i]));
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[written] = static_cast<std::uint8_t>((accumulator >> bits) & 0xFFU);
            ++written;
        }
    }
    *out_bytes = written;
    return nullptr;
}

/// 解析一段编码文本；错误信息只描述格式，不回显输入。
/// @details 无空白时零拷贝解析调用方输入；有空白时只复制一份去空白文本。
error::ErrorPtr ParseKeyText(std::string_view raw, std::size_t max_bytes, KeyStore::Key* out) {
    if (raw.size() > max_bytes) {
        return error::NewError("keystore: encoded key exceeds the input limit",
                               error::ErrorCode::kResourceExhausted);
    }

    // 只在确实含空白时复制；绝大多数调用（配置文件里单行 hex）走零拷贝路径。
    std::string stripped;
    std::string_view text = raw;
    if (ContainsAsciiWhitespace(raw)) {
        stripped = StripWhitespace(raw);
        text = stripped;
    }
    // RAII：函数任何返回路径都把去空白副本清零（它等价于密钥）。
    struct StrippedGuard final {
        std::string& text;
        ~StrippedGuard() { SecureClear(text.data(), text.size()); }
    } stripped_guard{stripped};

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
    if (body.empty()) {
        return error::NewError("keystore: key body is empty", error::ErrorCode::kInvalidArgument);
    }

    std::array<std::uint8_t, kKeyMaxBytes> bytes{};
    struct BytesGuard final {
        std::uint8_t* data;
        std::size_t size;
        ~BytesGuard() { SecureClear(data, size); }
    } bytes_guard{bytes.data(), bytes.size()};

    std::size_t decoded_bytes = 0;
    error::ErrorPtr decode_error;
    if (forced_hex) {
        decode_error = DecodeHex(body, bytes.data(), &decoded_bytes);
    } else if (forced_base64) {
        decode_error = DecodeBase64(body, bytes.data(), &decoded_bytes);
    } else if (IsHexText(body)) {
        decode_error = DecodeHex(body, bytes.data(), &decoded_bytes);
    } else {
        decode_error = DecodeBase64(body, bytes.data(), &decoded_bytes);
    }
    if (decode_error) {
        return decode_error;
    }
    *out = KeyStore::Key::FromBytes(bytes.data(), decoded_bytes);
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

} // namespace

// ---- Key 生命周期 ----

KeyStore::Key::~Key() { SecureClear(m_bytes.data(), m_bytes.size()); }

KeyStore::Key& KeyStore::Key::operator=(const Key& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // 先清零再拷贝：32B → 16B 的替换不会在后面留下旧密钥的高 16 字节。
    SecureClear(m_bytes.data(), m_bytes.size());
    m_bytes = other.m_bytes;
    m_bytes_used = other.m_bytes_used;
    return *this;
}

KeyStore::Key& KeyStore::Key::operator=(Key&& other) noexcept { return *this = other; }

KeyStore::Key KeyStore::Key::FromBytes(const std::uint8_t* data, std::size_t bytes) {
    Key key;
    if (data == nullptr || !IsKeyLength(bytes)) {
        return key;
    }
    std::memcpy(key.m_bytes.data(), data, bytes);
    key.m_bytes_used = bytes;
    return key;
}

bool KeyStore::Key::valid() const noexcept { return IsKeyLength(m_bytes_used); }

std::size_t KeyStore::Key::size() const noexcept { return m_bytes_used; }

const std::uint8_t* KeyStore::Key::data() const noexcept { return m_bytes.data(); }

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
    static constexpr char kHexDigits[] = "0123456789abcdef";
    const std::size_t hex_chars = m_bytes_used * 2U;
    std::string masked(hex_chars, '*');
    if (hex_chars <= 8U) {
        // 16/32B 的十六进制串至少 32 字符，这里只是防御未来长度策略变化。
        return masked;
    }
    // 直接写前 4 / 后 4 个十六进制字符，不生成完整明文临时串。
    masked[0] = kHexDigits[m_bytes[0] >> 4U];
    masked[1] = kHexDigits[m_bytes[0] & 0x0FU];
    masked[2] = kHexDigits[m_bytes[1] >> 4U];
    masked[3] = kHexDigits[m_bytes[1] & 0x0FU];
    masked[hex_chars - 4U] = kHexDigits[m_bytes[m_bytes_used - 2U] >> 4U];
    masked[hex_chars - 3U] = kHexDigits[m_bytes[m_bytes_used - 2U] & 0x0FU];
    masked[hex_chars - 2U] = kHexDigits[m_bytes[m_bytes_used - 1U] >> 4U];
    masked[hex_chars - 1U] = kHexDigits[m_bytes[m_bytes_used - 1U] & 0x0FU];
    return masked;
}

// ---- KeyStore ----

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
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > m_config.max_input_bytes) {
        std::string message = "keystore: key file exceeds the input limit of ";
        message += std::to_string(m_config.max_input_bytes);
        message += " bytes: '";
        message += path;
        message += "'";
        return error::NewError(std::move(message), error::ErrorCode::kResourceExhausted);
    }
    if (m_config.check_file_permissions && (info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        std::ostringstream stream;
        stream << "keystore: warning: key file '" << path << "' mode " << std::oct << std::setw(4)
               << std::setfill('0') << static_cast<unsigned int>(info.st_mode & 07777U)
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

    // 文件内容是编码后的密钥，解析完成后立即清零，避免留在字符串堆缓冲里。
    struct ContentGuard final {
        std::string& text;
        ~ContentGuard() { SecureClear(text.data(), text.size()); }
    } content_guard{content};
    return LoadDecoded(content, "file");
}

bool KeyStore::loaded() const noexcept { return m_current.valid(); }

const KeyStore::Key& KeyStore::Current() const noexcept { return m_current; }

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

    if (m_log_sink) {
        std::string message = "keystore: rotated; current=";
        message += m_current.Masked();
        message += " previous=";
        message += m_previous.valid() ? m_previous.Masked() : "<none>";
        message += " retention_ms=";
        message += std::to_string(m_config.rotation_retention_ms);
        Emit(message);
    }
    return true;
}

void KeyStore::SetRotationRetentionMs(std::uint64_t value) noexcept {
    m_config.rotation_retention_ms = value;
}

std::uint64_t KeyStore::rotation_retention_ms() const noexcept {
    return m_config.rotation_retention_ms;
}

void KeyStore::SetLogSink(LogSink sink) { m_log_sink = std::move(sink); }

const std::vector<std::string>& KeyStore::warnings() const noexcept { return m_warnings; }

void KeyStore::ClearWarnings() noexcept { m_warnings.clear(); }

bool KeyStore::ConstantTimeEquals(const Key& a, const Key& b) noexcept {
    return ConstantTimeEquals(a.data(), a.size(), b.data(), b.size());
}

bool KeyStore::ConstantTimeEquals(const std::uint8_t* a, std::size_t a_bytes, const std::uint8_t* b,
                                  std::size_t b_bytes) noexcept {
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
        // 日志是旁路：没有 sink 时不构造日志字符串（热加载失败也不额外分配）。
        if (m_log_sink) {
            std::string message = "keystore: load from ";
            message += source;
            message += " failed: ";
            message += error::ErrorMessage(parse_error);
            Emit(message);
        }
        return error::Wrap(std::move(parse_error),
                           std::string("keystore: load from ") + source + " failed");
    }

    m_current = key;
    m_previous = Key{};
    m_rotated_at_ms = 0;

    if (m_log_sink) {
        std::string message = "keystore: loaded ";
        message += std::to_string(m_current.size());
        message += "-byte key from ";
        message += source;
        message += "; masked=";
        message += m_current.Masked();
        Emit(message);
    }
    return nullptr;
}

void KeyStore::AddWarning(std::string message) {
    // 热加载同一权限宽松的文件时避免 warnings 无界增长；重复告警不再刷日志。
    if (std::find(m_warnings.begin(), m_warnings.end(), message) != m_warnings.end()) {
        return;
    }
    m_warnings.push_back(std::move(message));
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

} // namespace go2cpp::bento
