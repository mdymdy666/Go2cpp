/**
 * @file test_keystore.cpp
 * @brief MOD-05 密钥管理：加载/编码/脱敏/常量时间比较/轮换窗口/权限告警/
 *        日志不泄漏，以及 PeerHub 与 Link 的 PSK 集成。
 * @details 覆盖矩阵：
 *          - 加载：hex（大小写/空白/前缀）、base64（带/不带 padding/前缀）、
 *            文件（trim/权限/空/超大/不存在/目录）、环境变量（成功/缺失/空）。
 *          - 拒绝路径：非法 hex、非法 base64、16/32 之外的长度；失败不破坏
 *            已加载密钥。
 *          - 脱敏：前 4 后 4、长度、空 Key；Masked 结果本身可安全记录。
 *          - 常量时间比较：相同/首字节不同/末字节不同/长度不同/空指针。
 *          - 轮换：Current/Previous、Accepts 过渡、窗口置 0 立即失效、
 *            连续轮换只保留一代、真实时间窗口过期。
 *          - 泄漏审计：注入 LogSink 捕获成功/失败/权限/轮换日志，断言完整
 *            hex/base64 明文均不出现；错误文本同样不含输入。
 *          - 集成：ApplyPreSharedKey 模板对 PeerHub/Link 生效，跑真实 UDP
 *            回环加密握手与消息收发。
 *          - 随机：500 组随机字符串不崩溃/不泄漏；200 组密钥 hex+base64 往返。
 *          - 压测：20 万次常量时间比较、2 万次加载，输出 ops/s 作为量化指标。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/keystore.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/peer_hub.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

std::vector<std::uint8_t> MakeBytes(std::uint8_t seed, std::size_t bytes) {
    std::vector<std::uint8_t> out(bytes, 0);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((seed * 31U + i * 7U + 0x5AU) & 0xFFU);
    }
    return out;
}

std::string HexEncode(const std::uint8_t* data, std::size_t bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(bytes * 2U, '\0');
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i * 2U] = kDigits[data[i] >> 4U];
        out[i * 2U + 1U] = kDigits[data[i] & 0x0FU];
    }
    return out;
}

std::string HexEncode(const std::vector<std::uint8_t>& bytes) {
    return HexEncode(bytes.data(), bytes.size());
}

/// 测试内独立实现的 base64 编码，避免用被测代码证明自己。
std::string Base64Encode(const std::uint8_t* data, std::size_t bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes + 2U) / 3U) * 4U);
    for (std::size_t i = 0; i < bytes; i += 3U) {
        const std::uint32_t first = data[i];
        const std::uint32_t second = (i + 1U < bytes) ? data[i + 1U] : 0U;
        const std::uint32_t third = (i + 2U < bytes) ? data[i + 2U] : 0U;
        const std::uint32_t triple = (first << 16U) | (second << 8U) | third;
        out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        out.push_back((i + 1U < bytes) ? kAlphabet[(triple >> 6U) & 0x3FU] : '=');
        out.push_back((i + 2U < bytes) ? kAlphabet[triple & 0x3FU] : '=');
    }
    return out;
}

std::string Base64Encode(const std::vector<std::uint8_t>& bytes) {
    return Base64Encode(bytes.data(), bytes.size());
}

KeyStore::Key MakeKey(std::uint8_t seed, std::size_t bytes) {
    const std::vector<std::uint8_t> raw = MakeBytes(seed, bytes);
    return KeyStore::Key::FromBytes(raw.data(), raw.size());
}

bool Contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

bool IsAllStars(std::string_view text) {
    for (const char value : text) {
        if (value != '*') {
            return false;
        }
    }
    return true;
}

/// mkstemp 临时文件；析构自动关闭并 unlink。
class TempFile final {
   public:
    TempFile() {
        std::string pattern = "/tmp/bento_keystore_XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const int fd = ::mkstemp(buffer.data());
        if (fd >= 0) {
            m_fd = fd;
            m_path = buffer.data();
        }
    }
    ~TempFile() {
        Close();
        if (!m_path.empty()) {
            ::unlink(m_path.c_str());
        }
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    bool valid() const noexcept {
        return m_fd >= 0;
    }
    const std::string& path() const noexcept {
        return m_path;
    }

    bool Write(std::string_view content) {
        std::size_t offset = 0;
        while (offset < content.size()) {
            const ssize_t written =
                ::write(m_fd, content.data() + offset, content.size() - offset);
            if (written <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    bool SetMode(mode_t mode) {
        return m_fd >= 0 && ::fchmod(m_fd, mode) == 0;
    }

    void Close() noexcept {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

   private:
    int m_fd = -1;
    std::string m_path;
};

void Run(const char* name, void (*test)()) {
    std::printf("[CASE] %s\n", name);
    test();
}

// ---------------------------------------------------------------------------
// 加载
// ---------------------------------------------------------------------------

void TestLoadHexFromString() {
    const auto raw = MakeBytes(0x11, 32);
    const std::string hex = HexEncode(raw);

    KeyStore store;
    BENTO_CHECK(store.LoadFromString(hex) == nullptr);
    BENTO_CHECK(store.loaded());
    BENTO_CHECK(store.Current().valid());
    BENTO_CHECK_EQ(store.Current().size(), 32U);
    BENTO_CHECK_EQ(store.Current().ToHex(), hex);

    // 大写 + 空白 + hex: 前缀都应接受。
    std::string upper = hex;
    for (char& value : upper) {
        if (value >= 'a' && value <= 'f') {
            value = static_cast<char>(value - 'a' + 'A');
        }
    }
    const std::string spaced = "  " + upper.substr(0, 20) + "\n\t" + upper.substr(20) + " \r\n";
    BENTO_CHECK(store.LoadFromString(std::string("hex:") + spaced) == nullptr);
    BENTO_CHECK_EQ(store.Current().ToHex(), hex);

    const auto raw16 = MakeBytes(0x22, 16);
    BENTO_CHECK(store.LoadFromString(HexEncode(raw16)) == nullptr);
    BENTO_CHECK_EQ(store.Current().size(), 16U);
    BENTO_CHECK_EQ(store.Current().ToHex(), HexEncode(raw16));
}

void TestLoadBase64FromString() {
    const auto raw32 = MakeBytes(0x33, 32);
    const std::string base64_32 = Base64Encode(raw32);

    KeyStore store;
    BENTO_CHECK(store.LoadFromString("base64:" + base64_32) == nullptr);
    BENTO_CHECK_EQ(store.Current().size(), 32U);
    BENTO_CHECK_EQ(store.Current().ToHex(), HexEncode(raw32));

    // 前缀大小写不敏感。
    BENTO_CHECK(store.LoadFromString("BASE64:" + base64_32) == nullptr);
    BENTO_CHECK_EQ(store.Current().ToHex(), HexEncode(raw32));

    // 无 padding 的 base64 也应接受。
    std::string no_padding = base64_32;
    while (!no_padding.empty() && no_padding.back() == '=') {
        no_padding.pop_back();
    }
    BENTO_CHECK(store.LoadFromString("base64:" + no_padding) == nullptr);
    BENTO_CHECK_EQ(store.Current().ToHex(), HexEncode(raw32));

    const auto raw16 = MakeBytes(0x44, 16);
    BENTO_CHECK(store.LoadFromString(Base64Encode(raw16)) == nullptr);
    BENTO_CHECK_EQ(store.Current().ToHex(), HexEncode(raw16));
}

void TestRejectInvalidEncodings() {
    KeyStore store;
    const auto good = HexEncode(MakeBytes(0x55, 32));
    BENTO_CHECK(store.LoadFromString(good) == nullptr);

    const std::array<const char*, 7> invalid{
        "hex:abc",          // 奇数长度
        "hex:zz00",         // 非 hex 字符
        "hex:",             // 空前缀体
        "base64:AAAAA",     // 长度 %4 == 1
        "base64:AB$C",      // 非法字符
        "base64:A===",      // padding 过多
        "base64:",          // 空
    };
    for (const char* text : invalid) {
        const error::ErrorPtr err = store.LoadFromString(text);
        BENTO_CHECK(err != nullptr);
        BENTO_CHECK(error::Has(err, error::ErrorCode::kInvalidArgument));
        // 失败不破坏已成功加载的密钥。
        BENTO_CHECK_EQ(store.Current().ToHex(), good);
    }
}

void TestRejectWrongLength() {
    KeyStore store;
    const std::string good = HexEncode(MakeBytes(0x66, 16));
    BENTO_CHECK(store.LoadFromString(good) == nullptr);

    const std::array<std::size_t, 4> lengths{8, 20, 24, 33};
    for (const std::size_t bytes : lengths) {
        const auto raw = MakeBytes(0x77, bytes);
        const error::ErrorPtr hex_error = store.LoadFromString(HexEncode(raw));
        BENTO_CHECK(hex_error != nullptr);
        BENTO_CHECK(error::Has(hex_error, error::ErrorCode::kInvalidArgument));
        BENTO_CHECK(Contains(error::ErrorMessage(hex_error), "not 16 or 32"));

        const error::ErrorPtr base64_error =
            store.LoadFromString("base64:" + Base64Encode(raw));
        BENTO_CHECK(base64_error != nullptr);
        BENTO_CHECK(error::Has(base64_error, error::ErrorCode::kInvalidArgument));
        BENTO_CHECK_EQ(store.Current().ToHex(), good);
    }
}

void TestLoadFromEnv() {
    const std::string name = "BENTO_KEYSTORE_TEST_ENV_" + std::to_string(::getpid());
    ::unsetenv(name.c_str());

    KeyStore store;
    error::ErrorPtr err = store.LoadFromEnv(name);
    BENTO_CHECK(err != nullptr);
    BENTO_CHECK(error::Has(err, error::ErrorCode::kNotFound));

    const auto raw = MakeBytes(0x88, 32);
    BENTO_CHECK(::setenv(name.c_str(), HexEncode(raw).c_str(), 1) == 0);
    err = store.LoadFromEnv(name);
    BENTO_CHECK(err == nullptr);
    BENTO_CHECK_EQ(store.Current().ToHex(), HexEncode(raw));

    BENTO_CHECK(::setenv(name.c_str(), "", 1) == 0);
    err = store.LoadFromEnv(name);
    BENTO_CHECK(err != nullptr);
    BENTO_CHECK(error::Has(err, error::ErrorCode::kInvalidArgument));

    ::unsetenv(name.c_str());
}

void TestLoadFromFileAndTrim() {
    const auto raw = MakeBytes(0x99, 32);
    const std::string hex = HexEncode(raw);

    TempFile file;
    BENTO_CHECK(file.valid());
    BENTO_CHECK(file.Write(" \n" + hex.substr(0, 30) + "\n" + hex.substr(30) + "\t\n"));
    BENTO_CHECK(file.SetMode(0600));
    file.Close();

    KeyStore store;
    BENTO_CHECK(store.LoadFromFile(file.path()) == nullptr);
    BENTO_CHECK_EQ(store.Current().ToHex(), hex);
    BENTO_CHECK_EQ(store.Current().size(), 32U);
    BENTO_CHECK(store.warnings().empty());
}

void TestFilePermissionWarning() {
    const std::string hex = HexEncode(MakeBytes(0xAA, 16));

    TempFile file;
    BENTO_CHECK(file.valid());
    BENTO_CHECK(file.Write(hex));
    BENTO_CHECK(file.SetMode(0644));
    file.Close();

    KeyStore store;
    std::vector<std::string> logs;
    store.SetLogSink([&logs](std::string_view line) { logs.emplace_back(line); });

    BENTO_CHECK(store.LoadFromFile(file.path()) == nullptr);
    BENTO_CHECK_EQ(store.warnings().size(), 1U);
    if (!store.warnings().empty()) {
        BENTO_CHECK(Contains(store.warnings()[0], "0644"));
        BENTO_CHECK(Contains(store.warnings()[0], "0600"));
        BENTO_CHECK(Contains(store.warnings()[0], file.path()));
        BENTO_CHECK(!Contains(store.warnings()[0], hex));
    }
    BENTO_CHECK(!logs.empty());
    bool saw_permission_warning = false;
    for (const std::string& line : logs) {
        if (Contains(line, "more permissive than 0600")) {
            saw_permission_warning = true;
        }
        BENTO_CHECK(!Contains(line, hex));
    }
    BENTO_CHECK(saw_permission_warning);

    // 收紧到 0600 后不再告警。
    BENTO_CHECK(::chmod(file.path().c_str(), 0600) == 0);
    store.ClearWarnings();
    BENTO_CHECK(store.LoadFromFile(file.path()) == nullptr);
    BENTO_CHECK(store.warnings().empty());
}

void TestFileErrors() {
    KeyStore store;

    const std::string missing =
        "/tmp/bento_keystore_missing_" + std::to_string(::getpid()) + ".key";
    error::ErrorPtr err = store.LoadFromFile(missing);
    BENTO_CHECK(err != nullptr);
    BENTO_CHECK(error::Has(err, error::ErrorCode::kNotFound));

    // 目录不是普通文件。
    err = store.LoadFromFile("/tmp");
    BENTO_CHECK(err != nullptr);
    BENTO_CHECK(error::Has(err, error::ErrorCode::kInvalidArgument));

    TempFile empty;
    BENTO_CHECK(empty.valid());
    empty.Close();
    err = store.LoadFromFile(empty.path());
    BENTO_CHECK(err != nullptr);
    BENTO_CHECK(error::Has(err, error::ErrorCode::kInvalidArgument));

    // 超过 max_input_bytes 时在解析前拒绝。
    KeyStore::Config config;
    config.max_input_bytes = 64;
    KeyStore limited(config);
    TempFile large;
    BENTO_CHECK(large.valid());
    BENTO_CHECK(large.Write(std::string(200, 'a')));
    large.Close();
    err = limited.LoadFromFile(large.path());
    BENTO_CHECK(err != nullptr);
    BENTO_CHECK(error::Has(err, error::ErrorCode::kResourceExhausted));
}

// ---------------------------------------------------------------------------
// 脱敏 / 常量时间 / 轮换
// ---------------------------------------------------------------------------

void TestMasked() {
    for (const std::size_t bytes : {static_cast<std::size_t>(16), static_cast<std::size_t>(32)}) {
        const KeyStore::Key key = MakeKey(0xBB, bytes);
        const std::string hex = key.ToHex();
        const std::string masked = key.Masked();

        BENTO_CHECK_EQ(masked.size(), hex.size());
        BENTO_CHECK_EQ(masked.substr(0, 4), hex.substr(0, 4));
        BENTO_CHECK_EQ(masked.substr(masked.size() - 4), hex.substr(hex.size() - 4));
        BENTO_CHECK(IsAllStars(masked.substr(4, masked.size() - 8)));
        BENTO_CHECK(!Contains(masked, hex));
        BENTO_CHECK(Contains(masked, "****"));
    }

    const KeyStore::Key empty;
    BENTO_CHECK_EQ(empty.Masked(), std::string("<empty>"));
    BENTO_CHECK(empty.ToHex().empty());
    BENTO_CHECK(!empty.valid());
    BENTO_CHECK_EQ(empty.size(), 0U);
}

void TestConstantTimeEquals() {
    const KeyStore::Key first = MakeKey(0xCC, 32);
    const KeyStore::Key same = MakeKey(0xCC, 32);
    const KeyStore::Key other = MakeKey(0xCD, 32);
    const KeyStore::Key short_key = MakeKey(0xCC, 16);

    BENTO_CHECK(KeyStore::ConstantTimeEquals(first, same));
    BENTO_CHECK(!KeyStore::ConstantTimeEquals(first, other));
    BENTO_CHECK(!KeyStore::ConstantTimeEquals(first, short_key));
    BENTO_CHECK(!KeyStore::ConstantTimeEquals(short_key, first));

    std::vector<std::uint8_t> bytes(first.data(), first.data() + first.size());
    bytes[0] ^= 0x80U;
    const KeyStore::Key first_byte_changed = KeyStore::Key::FromBytes(bytes.data(), bytes.size());
    BENTO_CHECK(!KeyStore::ConstantTimeEquals(first, first_byte_changed));

    bytes.assign(first.data(), first.data() + first.size());
    bytes.back() ^= 0x01U;
    const KeyStore::Key last_byte_changed = KeyStore::Key::FromBytes(bytes.data(), bytes.size());
    BENTO_CHECK(!KeyStore::ConstantTimeEquals(first, last_byte_changed));

    BENTO_CHECK(KeyStore::ConstantTimeEquals(nullptr, 0, nullptr, 0));
    BENTO_CHECK(KeyStore::ConstantTimeEquals(nullptr, 0, first.data(), 0));
    BENTO_CHECK(!KeyStore::ConstantTimeEquals(nullptr, 4, first.data(), 4));
    BENTO_CHECK(KeyStore::ConstantTimeEquals(KeyStore::Key{}, KeyStore::Key{}));
    BENTO_CHECK(!KeyStore::ConstantTimeEquals(KeyStore::Key{}, first));
}

void TestRotation() {
    const KeyStore::Key key_a = MakeKey(0x01, 32);
    const KeyStore::Key key_b = MakeKey(0x02, 32);
    const KeyStore::Key key_c = MakeKey(0x03, 32);

    KeyStore store;
    BENTO_CHECK(store.Rotate(key_a));
    BENTO_CHECK(store.loaded());
    BENTO_CHECK_EQ(store.Current().ToHex(), key_a.ToHex());
    BENTO_CHECK(!store.Previous().valid());
    BENTO_CHECK(!store.HasUsablePrevious());

    BENTO_CHECK(store.Rotate(key_b));
    BENTO_CHECK_EQ(store.Current().ToHex(), key_b.ToHex());
    BENTO_CHECK(store.HasUsablePrevious());
    BENTO_CHECK_EQ(store.Previous().ToHex(), key_a.ToHex());
    BENTO_CHECK(store.Accepts(key_a));
    BENTO_CHECK(store.Accepts(key_b));
    BENTO_CHECK(!store.Accepts(key_c));

    // 连续轮换只保留一代：A 不再被接受。
    BENTO_CHECK(store.Rotate(key_c));
    BENTO_CHECK_EQ(store.Previous().ToHex(), key_b.ToHex());
    BENTO_CHECK(!store.Accepts(key_a));
    BENTO_CHECK(store.Accepts(key_b));
    BENTO_CHECK(store.Accepts(key_c));

    // 窗口 0 = 旧密钥立即失效。
    store.SetRotationRetentionMs(0);
    BENTO_CHECK(!store.HasUsablePrevious());
    BENTO_CHECK(!store.Previous().valid());
    BENTO_CHECK(!store.Accepts(key_b));
    BENTO_CHECK(store.Accepts(key_c));

    // 重新打开窗口，上一代恢复可用。
    store.SetRotationRetentionMs(60000);
    BENTO_CHECK(store.HasUsablePrevious());
    BENTO_CHECK(store.Accepts(key_b));

    // 无效 Key 不改变状态。
    BENTO_CHECK(!store.Rotate(KeyStore::Key{}));
    BENTO_CHECK_EQ(store.Current().ToHex(), key_c.ToHex());
}

void TestRotationWindowExpiry() {
    KeyStore::Config config;
    config.rotation_retention_ms = 30;
    KeyStore store(config);
    BENTO_CHECK(store.Rotate(MakeKey(0x0A, 16)));
    BENTO_CHECK(store.Rotate(MakeKey(0x0B, 16)));
    BENTO_CHECK(store.HasUsablePrevious());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    BENTO_CHECK(!store.HasUsablePrevious());
    BENTO_CHECK(!store.Previous().valid());
    BENTO_CHECK(!store.Accepts(MakeKey(0x0A, 16)));
    BENTO_CHECK(store.Accepts(MakeKey(0x0B, 16)));
}

void TestLoadDoesNotLeakPrevious() {
    KeyStore store;
    BENTO_CHECK(store.LoadFromString(HexEncode(MakeBytes(0x10, 32))) == nullptr);
    BENTO_CHECK(store.LoadFromString(HexEncode(MakeBytes(0x20, 32))) == nullptr);
    BENTO_CHECK(!store.Previous().valid());
    BENTO_CHECK(!store.Accepts(MakeKey(0x10, 32)));
}

// ---------------------------------------------------------------------------
// 日志不泄漏
// ---------------------------------------------------------------------------

void TestNoPlaintextInLogsOrErrors() {
    const std::string hex = HexEncode(MakeBytes(0xD1, 32));
    const std::string base64 = Base64Encode(MakeBytes(0xD2, 32));

    KeyStore store;
    std::vector<std::string> logs;
    store.SetLogSink([&logs](std::string_view line) { logs.emplace_back(line); });

    BENTO_CHECK(store.LoadFromString(hex) == nullptr);
    BENTO_CHECK(store.LoadFromString("base64:" + base64) == nullptr);

    // 失败输入里带完整密钥，错误与日志都不得回显。
    std::string invalid_hex = hex;
    invalid_hex[7] = 'z';
    const error::ErrorPtr hex_error = store.LoadFromString("hex:" + invalid_hex);
    BENTO_CHECK(hex_error != nullptr);
    const std::string hex_error_text = error::ErrorMessage(hex_error);
    BENTO_CHECK(!Contains(hex_error_text, invalid_hex));
    BENTO_CHECK(!Contains(hex_error_text, hex));
    BENTO_CHECK(Contains(hex_error_text, "non-hex character"));

    std::string invalid_base64 = base64;
    invalid_base64[5] = '$';
    const error::ErrorPtr base64_error = store.LoadFromString("base64:" + invalid_base64);
    BENTO_CHECK(base64_error != nullptr);
    const std::string base64_error_text = error::ErrorMessage(base64_error);
    BENTO_CHECK(!Contains(base64_error_text, invalid_base64));
    BENTO_CHECK(!Contains(base64_error_text, base64));
    BENTO_CHECK(Contains(base64_error_text, "invalid character"));

    BENTO_CHECK(store.Rotate(MakeKey(0xD3, 32)));
    BENTO_CHECK(store.Rotate(MakeKey(0xD4, 16)));

    BENTO_CHECK(!logs.empty());
    bool saw_masked = false;
    for (const std::string& line : logs) {
        BENTO_CHECK(!Contains(line, hex));
        BENTO_CHECK(!Contains(line, base64));
        BENTO_CHECK(!Contains(line, invalid_hex));
        BENTO_CHECK(!Contains(line, invalid_base64));
        if (Contains(line, "****")) {
            saw_masked = true;
        }
    }
    BENTO_CHECK(saw_masked);
}

// ---------------------------------------------------------------------------
// 集成
// ---------------------------------------------------------------------------

/// 记录调用参数的假对象，用于单独验证 ApplyPreSharedKey 的转换语义。
struct FakeSecureTarget {
    bool called = false;
    std::size_t bytes = 0;

    bool SetPreSharedKey(const std::uint8_t* key, std::size_t size) {
        called = true;
        bytes = size;
        return key != nullptr && size == 32U;
    }
};

void TestApplyPreSharedKeyHelper() {
    FakeSecureTarget target;
    BENTO_CHECK(ApplyPreSharedKey(target, MakeKey(0xE1, 32)));
    BENTO_CHECK(target.called);
    BENTO_CHECK_EQ(target.bytes, 32U);

    FakeSecureTarget short_target;
    BENTO_CHECK(!ApplyPreSharedKey(short_target, MakeKey(0xE2, 16)));
    BENTO_CHECK(short_target.called);
    BENTO_CHECK_EQ(short_target.bytes, 16U);

    FakeSecureTarget empty_target;
    BENTO_CHECK(!ApplyPreSharedKey(empty_target, KeyStore::Key{}));
    BENTO_CHECK(!empty_target.called);
}

template <typename Predicate>
bool PumpHubs(PeerHub& first, PeerHub& second, Predicate done, int timeout_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (NowMs() < deadline) {
        first.RunOnce(1);
        second.RunOnce(1);
        if (done()) {
            return true;
        }
    }
    return done();
}

void TestPeerHubIntegration() {
    const std::string hex = HexEncode(MakeBytes(0xE3, 32));
    KeyStore store;
    BENTO_CHECK(store.LoadFromString(hex) == nullptr);

    PeerHubConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 5000;
    PeerHub server(server_config);
    std::string open_error;
    BENTO_CHECK(server.Open(&open_error));
    BENTO_CHECK(ApplyPreSharedKey(server, store.Current()));
    BENTO_CHECK(server.secure());

    PeerHubConfig client_config;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 5000;
    PeerHub client(client_config);
    BENTO_CHECK(client.Open(&open_error));
    BENTO_CHECK(ApplyPreSharedKey(client, store.Current()));
    BENTO_CHECK(client.secure());
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &open_error));

    bool server_connected = false;
    bool client_connected = false;
    server.on_peer_connected = [&server_connected](std::uint32_t) { server_connected = true; };
    client.on_peer_connected = [&client_connected](std::uint32_t) { client_connected = true; };

    const bool connected = PumpHubs(
        client, server, [&]() { return server_connected && client_connected; }, 5000);
    BENTO_CHECK(connected);
    BENTO_CHECK(client.peer_secure_ready(1));

    std::vector<std::uint8_t> received;
    server.on_stream = [&received](std::uint32_t peer_id, std::uint8_t channel,
                                   const std::uint8_t* data, std::size_t bytes) {
        BENTO_CHECK(peer_id != 0U);
        BENTO_CHECK_EQ(channel, 3U);
        received.assign(data, data + bytes);
    };

    const char payload[] = "keystore peerhub integration";
    BENTO_CHECK(client.SendStream(1, 3, payload, sizeof(payload)));
    const bool received_ok = PumpHubs(client, server, [&]() { return !received.empty(); }, 5000);
    BENTO_CHECK(received_ok);
    BENTO_CHECK_EQ(received.size(), sizeof(payload));
    BENTO_CHECK(std::memcmp(received.data(), payload, sizeof(payload)) == 0);
    BENTO_CHECK(server.secure_stats().handshakes >= 1U);
}

void TestLinkIntegration() {
    const std::string hex = HexEncode(MakeBytes(0xE4, 32));
    KeyStore store;
    BENTO_CHECK(store.LoadFromString("base64:" + Base64Encode(MakeBytes(0xE4, 32))) == nullptr);
    BENTO_CHECK_EQ(store.Current().ToHex(), hex);

    LinkConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 5000;
    Link server(server_config);
    BENTO_CHECK(ApplyPreSharedKey(server, store.Current()));
    std::string open_error;
    BENTO_CHECK(server.Open(&open_error));
    BENTO_CHECK(server.secure());

    LinkConfig client_config;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.peer_port = server.local_port();
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 5000;
    client_config.handshake_timeout_ms = 3000;
    Link client(client_config);
    BENTO_CHECK(ApplyPreSharedKey(client, store.Current()));
    BENTO_CHECK(client.Open(&open_error));
    BENTO_CHECK(client.secure());
    client.Connect();

    const bool connected = bento_test::PumpUntil(
        client, server, [&]() { return client.connected() && server.connected(); }, 5000);
    BENTO_CHECK(connected);
    BENTO_CHECK(client.session_established());
    BENTO_CHECK(server.session_established());

    std::vector<std::uint8_t> received;
    server.on_stream = [&received](std::uint8_t channel, const std::uint8_t* data,
                                   std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 2U);
        received.assign(data, data + bytes);
    };
    const std::string payload = "keystore link integration";
    BENTO_CHECK(client.SendStream(2, payload.data(), payload.size()));
    const bool received_ok = bento_test::PumpUntil(
        client, server, [&]() { return !received.empty(); }, 5000);
    BENTO_CHECK(received_ok);
    BENTO_CHECK(received.size() == payload.size());
    BENTO_CHECK(std::memcmp(received.data(), payload.data(), payload.size()) == 0);
    // 服务端解密了客户端发来的加密 STREAM；客户端侧 ACK 可能尚未处理。
    BENTO_CHECK(server.secure_stats().packets_opened >= 1U);
}

// ---------------------------------------------------------------------------
// 随机 / 压测
// ---------------------------------------------------------------------------

void TestRandomInputs() {
    std::mt19937 random(0x5EED2026U);
    std::uniform_int_distribution<int> length_distribution(0, 96);
    std::uniform_int_distribution<int> character_distribution(0x20, 0x7E);

    for (int iteration = 0; iteration < 500; ++iteration) {
        const int length = length_distribution(random);
        std::string text(static_cast<std::size_t>(length), '\0');
        for (char& value : text) {
            value = static_cast<char>(character_distribution(random));
        }

        KeyStore store;
        const error::ErrorPtr err = store.LoadFromString(text);
        if (err == nullptr) {
            BENTO_CHECK(store.loaded());
            BENTO_CHECK(store.Current().size() == 16U || store.Current().size() == 32U);
        } else {
            BENTO_CHECK(!store.loaded());
            const std::string message = error::ErrorMessage(err);
            BENTO_CHECK(!message.empty());
            if (text.size() >= 8U) {
                BENTO_CHECK(!Contains(message, text));
            }
        }
    }
}

void TestRandomRoundTrip() {
    std::mt19937 random(0xC0FFEEU);
    for (int iteration = 0; iteration < 200; ++iteration) {
        const std::size_t bytes = (random() % 2U == 0U) ? 16U : 32U;
        std::vector<std::uint8_t> raw(bytes, 0);
        for (std::uint8_t& value : raw) {
            value = static_cast<std::uint8_t>(random() & 0xFFU);
        }
        const KeyStore::Key expected = KeyStore::Key::FromBytes(raw.data(), raw.size());
        BENTO_CHECK(expected.valid());

        KeyStore hex_store;
        BENTO_CHECK(hex_store.LoadFromString(HexEncode(raw)) == nullptr);
        BENTO_CHECK(KeyStore::ConstantTimeEquals(hex_store.Current(), expected));

        KeyStore base64_store;
        BENTO_CHECK(base64_store.LoadFromString("base64:" + Base64Encode(raw)) == nullptr);
        BENTO_CHECK(KeyStore::ConstantTimeEquals(base64_store.Current(), expected));
    }
}

void TestBenchmarks() {
    const KeyStore::Key first = MakeKey(0xF1, 32);
    const KeyStore::Key second = MakeKey(0xF2, 32);
    constexpr int kCompareIterations = 200000;
    std::uint64_t matches = 0;

    const auto compare_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kCompareIterations; ++i) {
        matches += KeyStore::ConstantTimeEquals(first, second) ? 0U : 1U;
    }
    const auto compare_end = std::chrono::steady_clock::now();
    const auto compare_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                compare_end - compare_start)
                                .count();
    const double compare_ops =
        static_cast<double>(kCompareIterations) * 1e6 /
        static_cast<double>(std::max<std::int64_t>(compare_us, 1));
    std::printf("[BENCH] ConstantTimeEquals 32B: %d iterations, %lld us, %.0f ops/s\n",
                kCompareIterations, static_cast<long long>(compare_us), compare_ops);
    BENTO_CHECK_EQ(matches, static_cast<std::uint64_t>(kCompareIterations));

    const std::string hex = HexEncode(MakeBytes(0xF3, 32));
    constexpr int kLoadIterations = 20000;
    std::uint64_t loaded = 0;
    const auto load_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kLoadIterations; ++i) {
        KeyStore store;
        if (store.LoadFromString(hex) == nullptr && store.Current().size() == 32U) {
            ++loaded;
        }
    }
    const auto load_end = std::chrono::steady_clock::now();
    const auto load_us =
        std::chrono::duration_cast<std::chrono::microseconds>(load_end - load_start).count();
    const double load_ops = static_cast<double>(kLoadIterations) * 1e6 /
                            static_cast<double>(std::max<std::int64_t>(load_us, 1));
    std::printf("[BENCH] LoadFromString hex 32B: %d iterations, %lld us, %.0f ops/s\n",
                kLoadIterations, static_cast<long long>(load_us), load_ops);
    BENTO_CHECK_EQ(loaded, static_cast<std::uint64_t>(kLoadIterations));
}

}  // namespace

int main() {
    Run("load_hex_string", TestLoadHexFromString);
    Run("load_base64_string", TestLoadBase64FromString);
    Run("reject_invalid_encodings", TestRejectInvalidEncodings);
    Run("reject_wrong_length", TestRejectWrongLength);
    Run("load_env", TestLoadFromEnv);
    Run("load_file_and_trim", TestLoadFromFileAndTrim);
    Run("file_permission_warning", TestFilePermissionWarning);
    Run("file_errors", TestFileErrors);
    Run("masked", TestMasked);
    Run("constant_time_equals", TestConstantTimeEquals);
    Run("rotation", TestRotation);
    Run("rotation_window_expiry", TestRotationWindowExpiry);
    Run("load_does_not_leak_previous", TestLoadDoesNotLeakPrevious);
    Run("no_plaintext_in_logs", TestNoPlaintextInLogsOrErrors);
    Run("apply_psk_helper", TestApplyPreSharedKeyHelper);
    Run("peer_hub_integration", TestPeerHubIntegration);
    Run("link_integration", TestLinkIntegration);
    Run("random_inputs", TestRandomInputs);
    Run("random_round_trip", TestRandomRoundTrip);
    Run("benchmarks", TestBenchmarks);
    return bento_test::Finish("test_keystore");
}
