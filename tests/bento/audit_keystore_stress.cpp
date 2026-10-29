/**
 * @file audit_keystore_stress.cpp
 * @brief keystore 模块审查用压测/随机/真实场景/并发/泄漏程序（不注册 CTest）。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 与 `tests/bento/test_keystore.cpp` 的分工：
 *          - `test_keystore.cpp` 是注册进 CTest 的确定性回归测试；
 *          - 本程序是**审查压测工具**，覆盖百万级 LoadFromString /
 *            ConstantTimeEquals / Masked / Accepts、5000 组随机输入对拍
 *            （独立参考模型）、5 个真实场景（加载/脱敏/常量时间/轮换/并发读）、
 *            分配计数与 RSS 长稳。
 *
 * 用法：
 * @code
 *   audit_keystore_stress 1.0                     # 全量（约 3-6s）
 *   audit_keystore_stress 1.0 --mode=bench        # 只跑吞吐/内存
 *   audit_keystore_stress 1.0 --mode=random       # 随机对拍
 *   audit_keystore_stress 1.0 --mode=scenarios    # 五个真实场景
 *   audit_keystore_stress 0.02 --mode=all         # ASan/UBSan 小规模
 *   audit_keystore_stress 0.005 --mode=leaks      # valgrind 小规模
 * @endcode
 *
 * 退出码：0 = 全部断言通过；1 = 有失败（失败点打印 CHECK 位置）。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/keystore.hpp"

// ---------------------------------------------------------------------------
// 分配计数：默认开启；ASan/UBSan 构建用 -DAUDIT_NO_ALLOC_COUNTER 关闭，
// 避免与 sanitizer 的 operator new 拦截互相干扰。
// ---------------------------------------------------------------------------
#ifndef AUDIT_NO_ALLOC_COUNTER
namespace audit_alloc {
std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_bytes{0};
std::atomic<std::uint64_t> g_frees{0};
} // namespace audit_alloc

void* operator new(std::size_t bytes) {
    audit_alloc::g_calls.fetch_add(1, std::memory_order_relaxed);
    audit_alloc::g_bytes.fetch_add(bytes, std::memory_order_relaxed);
    void* pointer = std::malloc(bytes == 0U ? 1U : bytes);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    audit_alloc::g_calls.fetch_add(1, std::memory_order_relaxed);
    audit_alloc::g_bytes.fetch_add(bytes, std::memory_order_relaxed);
    return std::malloc(bytes == 0U ? 1U : bytes);
}

void* operator new[](std::size_t bytes, const std::nothrow_t& tag) noexcept {
    return ::operator new(bytes, tag);
}

void operator delete(void* pointer) noexcept {
    if (pointer != nullptr) {
        audit_alloc::g_frees.fetch_add(1, std::memory_order_relaxed);
        std::free(pointer);
    }
}

void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    ::operator delete(pointer);
}
#endif // AUDIT_NO_ALLOC_COUNTER

namespace {

using go2cpp::bento::KeyStore;
namespace error = go2cpp::bento::error;
using go2cpp::bento::error::ErrorCode;
using go2cpp::bento::error::ErrorPtr;

using Clock = std::chrono::steady_clock;

int g_checks = 0;
int g_failures = 0;
std::uint64_t g_sink = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition);            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!((actual) == (expected))) {                                                           \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual,          \
                         #expected);                                                               \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

std::uint64_t RssBytes() {
    std::ifstream statm("/proc/self/statm");
    std::uint64_t total_pages = 0;
    std::uint64_t resident_pages = 0;
    statm >> total_pages >> resident_pages;
    const long page_size = ::sysconf(_SC_PAGESIZE);
    return resident_pages * static_cast<std::uint64_t>(page_size > 0 ? page_size : 4096);
}

std::uint64_t AllocCalls() {
#ifndef AUDIT_NO_ALLOC_COUNTER
    return audit_alloc::g_calls.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

std::uint64_t FreeCalls() {
#ifndef AUDIT_NO_ALLOC_COUNTER
    return audit_alloc::g_frees.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

std::uint64_t AllocBytes() {
#ifndef AUDIT_NO_ALLOC_COUNTER
    return audit_alloc::g_bytes.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

std::uint64_t Percentile(std::vector<std::uint64_t> samples, double ratio) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double index = ratio * static_cast<double>(samples.size() - 1);
    const std::size_t offset = static_cast<std::size_t>(index + 0.5);
    return samples[std::min(offset, samples.size() - 1)];
}

// ---------------------------------------------------------------------------
// 编码辅助（测试侧独立实现，不复用被测代码）
// ---------------------------------------------------------------------------

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

bool IsAsciiWhitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' ||
           value == '\v';
}

std::string StripWhitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char value : text) {
        if (!IsAsciiWhitespace(value)) {
            out.push_back(value);
        }
    }
    return out;
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const char left =
            (text[i] >= 'A' && text[i] <= 'Z') ? static_cast<char>(text[i] - 'A' + 'a') : text[i];
        if (left != prefix[i]) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 独立参考模型：按公开语义预测「成功 / 错误码 / 解码字节」。
// ---------------------------------------------------------------------------

struct RefResult {
    bool ok = false;
    ErrorCode code = ErrorCode::kOk;
    std::vector<std::uint8_t> bytes;
};

int RefHexValue(char value) {
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

int RefBase64Value(char value) {
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

bool RefIsHexText(std::string_view text) {
    if (text.empty() || (text.size() % 2U) != 0U) {
        return false;
    }
    for (const char value : text) {
        if (RefHexValue(value) < 0) {
            return false;
        }
    }
    return true;
}

RefResult RefParse(std::string_view raw, std::size_t max_input) {
    RefResult result;
    if (raw.size() > max_input) {
        result.code = ErrorCode::kResourceExhausted;
        return result;
    }
    const std::string text = StripWhitespace(raw);
    if (text.empty()) {
        result.code = ErrorCode::kInvalidArgument;
        return result;
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
        result.code = ErrorCode::kInvalidArgument;
        return result;
    }

    if (forced_hex || (!forced_base64 && RefIsHexText(body))) {
        if (body.empty() || (body.size() % 2U) != 0U) {
            result.code = ErrorCode::kInvalidArgument;
            return result;
        }
        std::vector<std::uint8_t> bytes(body.size() / 2U, 0);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const int high = RefHexValue(body[i * 2U]);
            const int low = RefHexValue(body[i * 2U + 1U]);
            if (high < 0 || low < 0) {
                result.code = ErrorCode::kInvalidArgument;
                return result;
            }
            bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
        }
        if (bytes.size() != 16U && bytes.size() != 32U) {
            result.code = ErrorCode::kInvalidArgument;
            return result;
        }
        result.ok = true;
        result.bytes = std::move(bytes);
        return result;
    }

    // base64：独立按 4 字符块解码。
    std::size_t data_bytes = body.size();
    std::size_t padding = 0;
    while (data_bytes > 0U && body[data_bytes - 1U] == '=') {
        ++padding;
        --data_bytes;
    }
    if (padding > 2U || data_bytes == 0U || (data_bytes % 4U) == 1U ||
        (padding != 0U && ((data_bytes + padding) % 4U) != 0U)) {
        result.code = ErrorCode::kInvalidArgument;
        return result;
    }
    for (std::size_t i = 0; i < data_bytes; ++i) {
        if (RefBase64Value(body[i]) < 0) {
            result.code = ErrorCode::kInvalidArgument;
            return result;
        }
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve((data_bytes * 3U) / 4U + 1U);
    std::size_t index = 0;
    while (index + 4U <= data_bytes) {
        const std::uint32_t a = static_cast<std::uint32_t>(RefBase64Value(body[index]));
        const std::uint32_t b = static_cast<std::uint32_t>(RefBase64Value(body[index + 1U]));
        const std::uint32_t c = static_cast<std::uint32_t>(RefBase64Value(body[index + 2U]));
        const std::uint32_t d = static_cast<std::uint32_t>(RefBase64Value(body[index + 3U]));
        const std::uint32_t triple = (a << 18U) | (b << 12U) | (c << 6U) | d;
        bytes.push_back(static_cast<std::uint8_t>((triple >> 16U) & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>((triple >> 8U) & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>(triple & 0xFFU));
        index += 4U;
    }
    const std::size_t rest = data_bytes - index;
    if (rest == 2U) {
        const std::uint32_t a = static_cast<std::uint32_t>(RefBase64Value(body[index]));
        const std::uint32_t b = static_cast<std::uint32_t>(RefBase64Value(body[index + 1U]));
        bytes.push_back(static_cast<std::uint8_t>(((a << 2U) | (b >> 4U)) & 0xFFU));
    } else if (rest == 3U) {
        const std::uint32_t a = static_cast<std::uint32_t>(RefBase64Value(body[index]));
        const std::uint32_t b = static_cast<std::uint32_t>(RefBase64Value(body[index + 1U]));
        const std::uint32_t c = static_cast<std::uint32_t>(RefBase64Value(body[index + 2U]));
        bytes.push_back(static_cast<std::uint8_t>(((a << 2U) | (b >> 4U)) & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>(((b << 4U) | (c >> 2U)) & 0xFFU));
    } else if (rest != 0U) {
        result.code = ErrorCode::kInvalidArgument;
        return result;
    }
    if (bytes.size() != 16U && bytes.size() != 32U) {
        result.code = ErrorCode::kInvalidArgument;
        return result;
    }
    result.ok = true;
    result.bytes = std::move(bytes);
    return result;
}

// ---------------------------------------------------------------------------
// 临时文件
// ---------------------------------------------------------------------------

class TempFile final {
  public:
    explicit TempFile(std::string_view content, mode_t mode) {
        std::string pattern = "/tmp/bento_keystore_stress_XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const int fd = ::mkstemp(buffer.data());
        if (fd < 0) {
            return;
        }
        m_fd = fd;
        m_path = buffer.data();
        std::size_t offset = 0;
        while (offset < content.size()) {
            const ssize_t written = ::write(m_fd, content.data() + offset, content.size() - offset);
            if (written <= 0) {
                break;
            }
            offset += static_cast<std::size_t>(written);
        }
        (void)::fchmod(m_fd, mode);
        ::close(m_fd);
        m_fd = -1;
    }

    ~TempFile() {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        if (!m_path.empty()) {
            ::unlink(m_path.c_str());
        }
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    bool valid() const noexcept { return !m_path.empty(); }
    const std::string& path() const noexcept { return m_path; }

  private:
    int m_fd = -1;
    std::string m_path;
};

// ---------------------------------------------------------------------------
// 压测
// ---------------------------------------------------------------------------

struct BenchResult {
    std::uint64_t iterations = 0;
    std::uint64_t elapsed_ns = 0;
    double ops_per_sec = 0.0;
    double ns_per_op = 0.0;
};

template <typename Fn>
BenchResult RunBench(const char* name, std::uint64_t iterations, Fn&& operation,
                     std::uint64_t sample_count = 0) {
    for (int i = 0; i < 1000; ++i) {
        operation();
    }
    const std::uint64_t start = NowNs();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        operation();
    }
    const std::uint64_t end = NowNs();
    BenchResult result;
    result.iterations = iterations;
    result.elapsed_ns = end - start;
    result.ops_per_sec = static_cast<double>(iterations) * 1e9 /
                         static_cast<double>(std::max<std::uint64_t>(result.elapsed_ns, 1U));
    result.ns_per_op = static_cast<double>(result.elapsed_ns) /
                       static_cast<double>(std::max<std::uint64_t>(iterations, 1U));
    std::printf("[BENCH] %-28s %10llu ops %8.3f ms  %10.2f M ops/s  %8.2f ns/op", name,
                static_cast<unsigned long long>(iterations),
                static_cast<double>(result.elapsed_ns) / 1e6, result.ops_per_sec / 1e6,
                result.ns_per_op);

    if (sample_count > 0U) {
        const std::uint64_t samples_to_take = std::min(sample_count, iterations);
        std::vector<std::uint64_t> samples;
        samples.reserve(static_cast<std::size_t>(samples_to_take));
        for (std::uint64_t i = 0; i < samples_to_take; ++i) {
            const std::uint64_t before = NowNs();
            operation();
            const std::uint64_t after = NowNs();
            samples.push_back(after - before);
        }
        const std::uint64_t p50 = Percentile(samples, 0.50);
        const std::uint64_t p95 = Percentile(samples, 0.95);
        const std::uint64_t p99 = Percentile(samples, 0.99);
        std::printf("  P50=%llu P95=%llu P99=%llu ns (含计时开销)",
                    static_cast<unsigned long long>(p50), static_cast<unsigned long long>(p95),
                    static_cast<unsigned long long>(p99));
    }
    std::printf("\n");
    return result;
}

void RunBenchmarks(double scale) {
    const std::uint64_t load_iterations = static_cast<std::uint64_t>(1000000.0 * scale);
    const std::uint64_t compare_iterations = static_cast<std::uint64_t>(20000000.0 * scale);
    const std::uint64_t masked_iterations = static_cast<std::uint64_t>(1000000.0 * scale);
    const std::uint64_t accepts_iterations = static_cast<std::uint64_t>(1000000.0 * scale);
    const std::uint64_t mixed_iterations = static_cast<std::uint64_t>(1000000.0 * scale);

    const std::vector<std::uint8_t> raw32 = MakeBytes(0x11, 32);
    const std::string hex32 = HexEncode(raw32);
    const std::string base64_32 = Base64Encode(raw32);
    const KeyStore::Key key_a = MakeKey(0x21, 32);
    const KeyStore::Key key_b = MakeKey(0x22, 32);
    const KeyStore::Key key_c = MakeKey(0x23, 32);

    std::printf("\n[MODE] bench scale=%.3f\n", scale);
    const std::uint64_t rss_before = RssBytes();
    const std::uint64_t alloc_before = AllocCalls();
    const std::uint64_t free_before = FreeCalls();
    const std::uint64_t bytes_before = AllocBytes();

    {
        KeyStore store;
        std::uint64_t failures = 0;
        CHECK(store.LoadFromString(hex32) == nullptr);
        RunBench(
            "LoadFromString hex 32B", load_iterations,
            [&store, &hex32, &failures]() {
                if (store.LoadFromString(hex32) != nullptr) {
                    ++failures;
                }
                g_sink += store.Current().size();
            },
            200000U);
        CHECK_EQ(failures, 0U);
        CHECK_EQ(store.Current().size(), 32U);
    }

    {
        KeyStore store;
        std::uint64_t failures = 0;
        CHECK(store.LoadFromString("base64:" + base64_32) == nullptr);
        RunBench(
            "LoadFromString base64 32B", load_iterations,
            [&store, &base64_32, &failures]() {
                if (store.LoadFromString("base64:" + base64_32) != nullptr) {
                    ++failures;
                }
                g_sink += store.Current().size();
            },
            200000U);
        CHECK_EQ(failures, 0U);
    }

    {
        std::uint64_t matches = 0;
        const std::uint64_t iterations = compare_iterations;
        RunBench(
            "ConstantTimeEquals 32B", iterations,
            [&key_a, &key_b, &matches]() {
                matches += KeyStore::ConstantTimeEquals(key_a, key_b) ? 0U : 1U;
            },
            200000U);
        CHECK(!KeyStore::ConstantTimeEquals(key_a, key_b));
    }

    {
        std::uint64_t total_bytes = 0;
        const KeyStore::Key key = MakeKey(0x31, 32);
        RunBench(
            "Masked 32B", masked_iterations,
            [&key, &total_bytes]() {
                const std::string masked = key.Masked();
                total_bytes += masked.size();
                g_sink += static_cast<std::uint64_t>(masked[0]);
            },
            200000U);
        CHECK_EQ(key.Masked().size(), 64U);
    }

    {
        KeyStore store;
        CHECK(store.Rotate(key_a));
        CHECK(store.Rotate(key_b));
        std::uint64_t accepted = 0;
        RunBench(
            "Accepts（窗口内 A/B）", accepts_iterations,
            [&store, &key_a, &key_b, &key_c, &accepted]() {
                if (store.Accepts(key_a) && store.Accepts(key_b) && !store.Accepts(key_c)) {
                    ++accepted;
                }
            },
            100000U);
        CHECK(store.Accepts(key_a) && store.Accepts(key_b) && !store.Accepts(key_c));
    }

    {
        KeyStore store;
        CHECK(store.Rotate(key_a));
        CHECK(store.Rotate(key_b));
        std::uint64_t ok = 0;
        RunBench(
            "混合 Load/Accepts/Masked", mixed_iterations,
            [&store, &hex32, &ok]() {
                if (store.LoadFromString(hex32) == nullptr && store.Accepts(store.Current()) &&
                    !store.Current().Masked().empty()) {
                    ++ok;
                }
            },
            100000U);
        CHECK(store.Accepts(store.Current()));
    }

    {
        // 长稳：反复创建/加载/轮换/脱敏/析构，检查 RSS 与净分配不增长。
        const std::uint64_t cycles = static_cast<std::uint64_t>(200000.0 * scale);
        const std::uint64_t rss_before_long = RssBytes();
        for (std::uint64_t i = 0; i < cycles; ++i) {
            KeyStore store;
            if (store.LoadFromString(hex32) != nullptr ||
                !store.Rotate(MakeKey(static_cast<std::uint8_t>(i & 0xFFU), 32))) {
                ++g_failures;
            }
            g_sink += store.Current().Masked().size();
        }
        const std::uint64_t rss_after_long = RssBytes();
        std::printf("[MEM] 长稳 %llu 轮 Load/Rotate/Masked/析构: RSS %llu -> %llu KiB "
                    "(+%lld)\n",
                    static_cast<unsigned long long>(cycles),
                    static_cast<unsigned long long>(rss_before_long / 1024U),
                    static_cast<unsigned long long>(rss_after_long / 1024U),
                    static_cast<long long>((rss_after_long - rss_before_long) / 1024));
    }

    const std::uint64_t rss_after = RssBytes();
    const std::uint64_t alloc_after = AllocCalls();
    const std::uint64_t free_after = FreeCalls();
    const std::uint64_t bytes_after = AllocBytes();
    const std::int64_t net_alloc =
        static_cast<std::int64_t>(alloc_after) - static_cast<std::int64_t>(free_after);
    std::printf("[MEM] RSS %llu -> %llu KiB (+%lld); 分配 %llu 次 / 释放 %llu 次 "
                "/ 净 %+lld; "
                "累计分配 %llu MiB\n",
                static_cast<unsigned long long>(rss_before / 1024U),
                static_cast<unsigned long long>(rss_after / 1024U),
                static_cast<long long>((rss_after - rss_before) / 1024),
                static_cast<unsigned long long>(alloc_after - alloc_before),
                static_cast<unsigned long long>(free_after - free_before),
                static_cast<long long>(net_alloc),
                static_cast<unsigned long long>((bytes_after - bytes_before) / (1024U * 1024U)));
}

// ---------------------------------------------------------------------------
// 随机对拍
// ---------------------------------------------------------------------------

std::string RandomizeHexCase(std::mt19937& random, const std::string& hex) {
    std::string out = hex;
    std::uniform_int_distribution<int> coin(0, 2);
    for (char& value : out) {
        if (value >= 'a' && value <= 'f' && coin(random) != 0) {
            value = static_cast<char>(value - 'a' + 'A');
        }
    }
    return out;
}

std::string InsertWhitespace(std::mt19937& random, const std::string& text) {
    static constexpr char kWhitespace[] = " \t\r\n\f\v";
    std::uniform_int_distribution<std::size_t> position(0, text.size());
    std::uniform_int_distribution<int> count(1, 3);
    std::uniform_int_distribution<int> which(0, 5);
    std::string out = text;
    for (int i = 0; i < count(random); ++i) {
        const std::size_t where = position(random);
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(where), kWhitespace[which(random)]);
    }
    return out;
}

std::string RandomJunk(std::mt19937& random) {
    static constexpr char kAlphabet[] = "0123456789abcdefABCDEFGHIJKLMNOPQRSTUVWXYZ+/=:$%"
                                        " \t\n\r\f\v\x7f\x01\x02";
    std::uniform_int_distribution<int> length(0, 220);
    std::uniform_int_distribution<std::size_t> pick(0, sizeof(kAlphabet) - 2U);
    const int size = length(random);
    std::string out(static_cast<std::size_t>(size), '\0');
    for (char& value : out) {
        value = kAlphabet[pick(random)];
    }
    return out;
}

struct RandomStats {
    std::uint64_t total = 0;
    std::uint64_t ok = 0;
    std::uint64_t invalid = 0;
    std::uint64_t exhausted = 0;
    std::uint64_t other_error = 0;
};

void CheckAgainstReference(KeyStore& store, const std::string& input, RandomStats* stats) {
    const RefResult expected = RefParse(input, 4096U);
    const ErrorPtr err = store.LoadFromString(input);
    ++stats->total;
    if (expected.ok) {
        CHECK(err == nullptr);
        if (err == nullptr) {
            ++stats->ok;
            CHECK_EQ(store.Current().size(), expected.bytes.size());
            const std::string actual_hex = store.Current().ToHex();
            const std::string expected_hex = HexEncode(expected.bytes);
            CHECK_EQ(actual_hex, expected_hex);
        } else {
            ++stats->other_error;
        }
        return;
    }

    CHECK(err != nullptr);
    if (err == nullptr) {
        ++stats->other_error;
        return;
    }
    if (error::Has(err, expected.code)) {
        if (expected.code == ErrorCode::kResourceExhausted) {
            ++stats->exhausted;
        } else {
            ++stats->invalid;
        }
    } else {
        ++stats->other_error;
    }
    const std::string message = error::ErrorMessage(err);
    if (input.size() >= 16U) {
        CHECK(!Contains(message, input));
    }
}

void RunRandom(double scale, std::uint32_t seed) {
    const std::uint64_t iterations = static_cast<std::uint64_t>(5000.0 * scale);
    std::printf("\n[MODE] random iterations=%llu seed=%u\n",
                static_cast<unsigned long long>(iterations), seed);
    std::mt19937 random(seed);
    std::uniform_int_distribution<int> mode(0, 9);

    KeyStore store;
    RandomStats stats;
    std::vector<std::string> logs;
    store.SetLogSink([&logs](std::string_view line) { logs.emplace_back(line); });

    std::uint64_t log_leaks = 0;
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        std::string input;
        const int kind = mode(random);
        if (kind <= 2) {
            // 合法 16/32B 的 hex，随机大小写/空白/前缀，50% 注入一处破坏。
            const std::size_t bytes = (random() % 2U == 0U) ? 16U : 32U;
            std::string text = HexEncode(MakeBytes(static_cast<std::uint8_t>(random()), bytes));
            if (kind == 0 && random() % 2U == 0U) {
                text = "hex:" + RandomizeHexCase(random, text);
            }
            if (random() % 2U == 0U) {
                text = InsertWhitespace(random, text);
            }
            if (random() % 2U == 0U) {
                const std::size_t where = random() % text.size();
                text[where] = static_cast<char>('g' + static_cast<int>(random() % 6U));
            }
            input = std::move(text);
        } else if (kind <= 4) {
            // 合法 16/32B 的 base64，随机 padding/前缀，50% 注入非法字符。
            const std::size_t bytes = (random() % 2U == 0U) ? 16U : 32U;
            std::string text = Base64Encode(MakeBytes(static_cast<std::uint8_t>(random()), bytes));
            if (random() % 2U == 0U) {
                while (!text.empty() && text.back() == '=') {
                    text.pop_back();
                }
            }
            if (kind == 3) {
                text = "base64:" + text;
            }
            if (random() % 2U == 0U) {
                const std::size_t where = random() % text.size();
                text[where] = (random() % 2U == 0U) ? '$' : '#';
            }
            if (random() % 4U == 0U) {
                text = InsertWhitespace(random, text);
            }
            input = std::move(text);
        } else if (kind == 5) {
            // 任意长度随机字节。
            input = RandomJunk(random);
        } else if (kind == 6) {
            // 编码长度临近 16/32 边界的 hex（15/17/31/33 字节）。
            static constexpr std::size_t kLengths[] = {0U, 1U, 8U, 15U, 17U, 31U, 33U, 64U};
            const std::size_t bytes = kLengths[random() % 8U];
            input = HexEncode(MakeBytes(static_cast<std::uint8_t>(random()), bytes));
        } else if (kind == 7) {
            // 带前缀但空体 / 仅空白 / 只有前缀。
            static constexpr const char* kCandidates[] = {
                "",        " ",    "hex:",   "base64:",     "  hex:  ",
                "BASE64:", "\n\t", "hex:zz", "base64:====", "hex:0"};
            input = kCandidates[random() % 10U];
        } else if (kind == 8) {
            // 超长输入（>4096），必须 kResourceExhausted。
            const std::size_t bytes = 4096U + (random() % 100U);
            input.assign(bytes, (random() % 2U == 0U) ? 'a' : '0');
        } else {
            // 合法 hex 但中间夹 NUL/非 ASCII。
            std::string text = HexEncode(MakeBytes(static_cast<std::uint8_t>(random()), 32));
            const std::size_t where = random() % text.size();
            text[where] = (random() % 2U == 0U) ? '\0' : static_cast<char>(0xFF);
            input = std::move(text);
        }

        logs.clear();
        CheckAgainstReference(store, input, &stats);
        if (input.size() >= 16U) {
            for (const std::string& line : logs) {
                if (Contains(line, input)) {
                    ++log_leaks;
                }
            }
        }
        if (iteration % 64U == 0U) {
            logs.clear();
        }
    }
    CHECK_EQ(log_leaks, 0U);
    std::printf("[RANDOM] total=%llu ok=%llu invalid=%llu exhausted=%llu "
                "other_error=%llu\n",
                static_cast<unsigned long long>(stats.total),
                static_cast<unsigned long long>(stats.ok),
                static_cast<unsigned long long>(stats.invalid),
                static_cast<unsigned long long>(stats.exhausted),
                static_cast<unsigned long long>(stats.other_error));
    CHECK_EQ(stats.total, iterations);
    CHECK_EQ(stats.other_error, 0U);
}

// ---------------------------------------------------------------------------
// 五个真实场景
// ---------------------------------------------------------------------------

void ScenarioLoadSources() {
    std::printf("[SCENARIO] 1/5 多来源加载（String/File/Env/前缀/0600 告警）\n");
    const std::vector<std::uint8_t> raw = MakeBytes(0x41, 32);
    const std::string hex = HexEncode(raw);
    const std::string base64 = Base64Encode(raw);

    KeyStore store;
    CHECK(store.LoadFromString(hex) == nullptr);
    CHECK_EQ(store.Current().ToHex(), hex);
    CHECK(store.LoadFromString("hex:" + hex) == nullptr);
    CHECK(store.LoadFromString("  HEX:" + hex + "\n") == nullptr);
    CHECK(store.LoadFromString("BASE64:" + base64) == nullptr);
    CHECK_EQ(store.Current().ToHex(), hex);
    store.ClearWarnings();

    // 0600 文件：无告警；0644：有告警且仍加载；0400：无告警。
    {
        TempFile secure(hex, 0600);
        CHECK(secure.valid());
        store.ClearWarnings();
        CHECK(store.LoadFromFile(secure.path()) == nullptr);
        CHECK_EQ(store.Current().ToHex(), hex);
        CHECK(store.warnings().empty());
    }
    {
        TempFile permissive(" \n" + hex + "\t\n", 0644);
        CHECK(permissive.valid());
        store.ClearWarnings();
        std::vector<std::string> logs;
        store.SetLogSink([&logs](std::string_view line) { logs.emplace_back(line); });
        CHECK(store.LoadFromFile(permissive.path()) == nullptr);
        CHECK_EQ(store.Current().ToHex(), hex);
        CHECK_EQ(store.warnings().size(), 1U);
        if (!store.warnings().empty()) {
            CHECK(Contains(store.warnings()[0], "0644"));
            CHECK(Contains(store.warnings()[0], "0600"));
            CHECK(Contains(store.warnings()[0], permissive.path()));
            CHECK(!Contains(store.warnings()[0], hex));
        }
        bool saw_warning = false;
        for (const std::string& line : logs) {
            saw_warning = saw_warning || Contains(line, "more permissive than 0600");
            CHECK(!Contains(line, hex));
        }
        CHECK(saw_warning);
        store.SetLogSink(nullptr);
    }
    {
        TempFile readonly(hex, 0400);
        CHECK(readonly.valid());
        store.ClearWarnings();
        CHECK(store.LoadFromFile(readonly.path()) == nullptr);
        CHECK(store.warnings().empty());
    }

    // 环境变量：未设置 / 空 / 合法 / 超长。
    const std::string name = "BENTO_KEYSTORE_AUDIT_ENV_" + std::to_string(::getpid());
    ::unsetenv(name.c_str());
    ErrorPtr err = store.LoadFromEnv(name);
    CHECK(err != nullptr);
    CHECK(error::Has(err, ErrorCode::kNotFound));
    CHECK(::setenv(name.c_str(), "", 1) == 0);
    err = store.LoadFromEnv(name);
    CHECK(err != nullptr);
    CHECK(error::Has(err, ErrorCode::kInvalidArgument));
    CHECK(::setenv(name.c_str(), hex.c_str(), 1) == 0);
    CHECK(store.LoadFromEnv(name) == nullptr);
    CHECK_EQ(store.Current().ToHex(), hex);
    const std::string oversized(5000U, 'a');
    CHECK(::setenv(name.c_str(), oversized.c_str(), 1) == 0);
    err = store.LoadFromEnv(name);
    CHECK(err != nullptr);
    CHECK(error::Has(err, ErrorCode::kResourceExhausted));
    ::unsetenv(name.c_str());

    // 错误来源：缺失文件 / 目录 / 空文件 / 超大文件 / 无权限文件。
    err = store.LoadFromFile("/tmp/bento_keystore_audit_missing.key");
    CHECK(err != nullptr);
    CHECK(error::Has(err, ErrorCode::kNotFound));
    err = store.LoadFromFile("/tmp");
    CHECK(err != nullptr);
    CHECK(error::Has(err, ErrorCode::kInvalidArgument));
    {
        TempFile empty("", 0600);
        err = store.LoadFromFile(empty.path());
        CHECK(err != nullptr);
        CHECK(error::Has(err, ErrorCode::kInvalidArgument));
    }
    {
        TempFile large(std::string(200U, 'a'), 0600);
        KeyStore::Config config;
        config.max_input_bytes = 64;
        KeyStore limited(config);
        err = limited.LoadFromFile(large.path());
        CHECK(err != nullptr);
        CHECK(error::Has(err, ErrorCode::kResourceExhausted));
    }
    if (::geteuid() != 0) {
        TempFile denied(hex, 0000);
        err = store.LoadFromFile(denied.path());
        CHECK(err != nullptr);
        CHECK(error::Has(err, ErrorCode::kPermissionDenied));
    }
}

void ScenarioMasking() {
    std::printf("[SCENARIO] 2/5 脱敏（前 4 后 4 / 日志不泄漏）\n");
    for (const std::size_t bytes : {static_cast<std::size_t>(16), static_cast<std::size_t>(32)}) {
        const KeyStore::Key key = MakeKey(0x51, bytes);
        const std::string hex = key.ToHex();
        const std::string masked = key.Masked();
        CHECK_EQ(masked.size(), hex.size());
        CHECK_EQ(masked.substr(0, 4), hex.substr(0, 4));
        CHECK_EQ(masked.substr(masked.size() - 4), hex.substr(hex.size() - 4));
        CHECK(!Contains(masked, hex));
        bool middle_is_stars = true;
        for (std::size_t i = 4; i + 4 < masked.size(); ++i) {
            middle_is_stars = middle_is_stars && masked[i] == '*';
        }
        CHECK(middle_is_stars);
        CHECK(Contains(masked, "****"));
    }
    const KeyStore::Key empty;
    CHECK_EQ(empty.Masked(), std::string("<empty>"));
    CHECK(empty.ToHex().empty());

    // 日志中的成功/轮换/失败行都不得出现完整明文。
    const std::vector<std::uint8_t> raw = MakeBytes(0x52, 32);
    const std::string hex = HexEncode(raw);
    const std::string base64 = Base64Encode(raw);
    KeyStore store;
    std::vector<std::string> logs;
    store.SetLogSink([&logs](std::string_view line) { logs.emplace_back(line); });
    CHECK(store.LoadFromString(hex) == nullptr);
    CHECK(store.LoadFromString("base64:" + base64) == nullptr);
    CHECK(store.Rotate(MakeKey(0x53, 32)));
    std::string broken = hex;
    broken[9] = 'z';
    const ErrorPtr err = store.LoadFromString("hex:" + broken);
    CHECK(err != nullptr);
    CHECK(!Contains(error::ErrorMessage(err), broken));
    CHECK(!Contains(error::ErrorMessage(err), hex));
    CHECK(!logs.empty());
    bool saw_masked = false;
    for (const std::string& line : logs) {
        CHECK(!Contains(line, hex));
        CHECK(!Contains(line, base64));
        CHECK(!Contains(line, broken));
        saw_masked = saw_masked || Contains(line, "****");
    }
    CHECK(saw_masked);
}

void ScenarioConstantTime(double scale) {
    std::printf("[SCENARIO] 3/5 常量时间比较（相同/不同/长度不同/空）\n");
    const KeyStore::Key first = MakeKey(0x61, 32);
    const KeyStore::Key same = MakeKey(0x61, 32);
    const KeyStore::Key other = MakeKey(0x62, 32);
    const KeyStore::Key short_key = MakeKey(0x61, 16);
    CHECK(KeyStore::ConstantTimeEquals(first, same));
    CHECK(!KeyStore::ConstantTimeEquals(first, other));
    CHECK(!KeyStore::ConstantTimeEquals(first, short_key));
    CHECK(!KeyStore::ConstantTimeEquals(short_key, first));
    CHECK(KeyStore::ConstantTimeEquals(KeyStore::Key{}, KeyStore::Key{}));
    CHECK(!KeyStore::ConstantTimeEquals(KeyStore::Key{}, first));
    CHECK(KeyStore::ConstantTimeEquals(nullptr, 0, nullptr, 0));
    CHECK(!KeyStore::ConstantTimeEquals(nullptr, 4, first.data(), 4));

    // 差异位置不影响结果：首字节/中间/末字节。
    std::vector<std::uint8_t> bytes(first.data(), first.data() + first.size());
    for (const std::size_t index : {std::size_t{0}, std::size_t{15}, std::size_t{31}}) {
        std::vector<std::uint8_t> changed = bytes;
        changed[index] ^= 0x80U;
        CHECK(!KeyStore::ConstantTimeEquals(
            first, KeyStore::Key::FromBytes(changed.data(), changed.size())));
    }

    // 百万次正确性 + 计时（记录，不做严格时序断言）。
    const std::uint64_t kIterations = static_cast<std::uint64_t>(1000000.0 * scale);
    std::uint64_t matches = 0;
    const std::uint64_t start = NowNs();
    for (std::uint64_t i = 0; i < kIterations; ++i) {
        matches += KeyStore::ConstantTimeEquals(first, same) ? 1U : 0U;
    }
    const std::uint64_t same_ns = NowNs() - start;
    std::uint64_t mismatches = 0;
    const std::uint64_t start_other = NowNs();
    for (std::uint64_t i = 0; i < kIterations; ++i) {
        mismatches += KeyStore::ConstantTimeEquals(first, other) ? 1U : 0U;
    }
    const std::uint64_t other_ns = NowNs() - start_other;
    CHECK_EQ(matches, kIterations);
    CHECK_EQ(mismatches, 0U);
    std::printf("[TIMING] ConstantTimeEquals 32B: same=%.2f ns/op other=%.2f ns/op\n",
                static_cast<double>(same_ns) / static_cast<double>(kIterations),
                static_cast<double>(other_ns) / static_cast<double>(kIterations));
}

void ScenarioRotation() {
    std::printf("[SCENARIO] 4/5 轮换（Current/Previous/Accepts/窗口 0/5min）\n");
    const KeyStore::Key key_a = MakeKey(0x71, 32);
    const KeyStore::Key key_b = MakeKey(0x72, 32);
    const KeyStore::Key key_c = MakeKey(0x73, 32);

    KeyStore store;
    CHECK_EQ(store.rotation_retention_ms(), 5U * 60U * 1000U);
    CHECK(!store.loaded());
    CHECK(!store.Previous().valid());
    CHECK(!store.Accepts(key_a));
    CHECK(store.Rotate(key_a));
    CHECK(store.loaded());
    CHECK(!store.HasUsablePrevious());
    CHECK(store.Accepts(key_a));
    CHECK(store.Rotate(key_b));
    CHECK_EQ(store.Current().ToHex(), key_b.ToHex());
    CHECK(store.HasUsablePrevious());
    CHECK_EQ(store.Previous().ToHex(), key_a.ToHex());
    CHECK(store.Accepts(key_a));
    CHECK(store.Accepts(key_b));
    CHECK(!store.Accepts(key_c));
    CHECK(store.Rotate(key_c));
    CHECK(!store.Accepts(key_a));
    CHECK(store.Accepts(key_b));
    CHECK(store.Accepts(key_c));
    CHECK(!store.Rotate(KeyStore::Key{}));
    CHECK_EQ(store.Current().ToHex(), key_c.ToHex());

    // 窗口 0：立即失效。
    store.SetRotationRetentionMs(0);
    CHECK(!store.HasUsablePrevious());
    CHECK(!store.Previous().valid());
    CHECK(!store.Accepts(key_b));
    store.SetRotationRetentionMs(5U * 60U * 1000U);
    CHECK(store.HasUsablePrevious());
    CHECK(store.Accepts(key_b));

    // 30ms 窗口过期，再调大恢复（轮换时间点未变）。
    store.SetRotationRetentionMs(30);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(!store.HasUsablePrevious());
    CHECK(!store.Previous().valid());
    CHECK(!store.Accepts(key_b));
    CHECK(store.Accepts(key_c));
    store.SetRotationRetentionMs(5U * 60U * 1000U);
    CHECK(store.HasUsablePrevious());
    CHECK(store.Accepts(key_b));

    // 相同密钥轮换：记录当前语义（刷新窗口，Previous 与 Current 相同）。
    CHECK(store.Rotate(key_c));
    CHECK(store.Accepts(key_c));
    CHECK(store.HasUsablePrevious());
    CHECK_EQ(store.Previous().ToHex(), key_c.ToHex());
    std::printf("[NOTE] Rotate 相同密钥：Previous 与 Current 相同、窗口刷新（记录）\n");

    // 日志含 masked、不含明文。
    std::vector<std::string> logs;
    store.SetLogSink([&logs](std::string_view line) { logs.emplace_back(line); });
    CHECK(store.Rotate(key_a));
    CHECK(!logs.empty());
    for (const std::string& line : logs) {
        CHECK(!Contains(line, key_c.ToHex()));
        CHECK(!Contains(line, key_a.ToHex()));
    }
}

void ScenarioConcurrent(double scale) {
    std::printf("[SCENARIO] 5/5 并发读（8 线程共享只读 Accepts / 每线程独立 Load）\n");
    const KeyStore::Key key_a = MakeKey(0x81, 32);
    const KeyStore::Key key_b = MakeKey(0x82, 32);
    const KeyStore::Key key_c = MakeKey(0x83, 32);
    KeyStore shared;
    CHECK(shared.Rotate(key_a));
    CHECK(shared.Rotate(key_b));
    CHECK(shared.HasUsablePrevious());

    constexpr int kThreads = 8;
    const std::uint64_t kReadIterations = static_cast<std::uint64_t>(200000.0 * scale);
    std::atomic<std::uint64_t> bad_reads{0};
    std::atomic<std::uint64_t> good_reads{0};
    std::vector<std::thread> readers;
    readers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        readers.emplace_back(
            [&shared, &key_a, &key_b, &key_c, &bad_reads, &good_reads, &kReadIterations]() {
                for (std::uint64_t i = 0; i < kReadIterations; ++i) {
                    if (shared.Accepts(key_a) && shared.Accepts(key_b) && !shared.Accepts(key_c) &&
                        shared.Current().size() == 32U && shared.Previous().valid()) {
                        good_reads.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        bad_reads.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }
    for (std::thread& reader : readers) {
        reader.join();
    }
    CHECK_EQ(bad_reads.load(), 0U);
    CHECK_EQ(good_reads.load(), static_cast<std::uint64_t>(kThreads) * kReadIterations);

    // 每线程独立 KeyStore：Load + Accepts 不共享状态。
    const std::string hex = HexEncode(MakeBytes(0x84, 32));
    const std::uint64_t kLocalIterations = static_cast<std::uint64_t>(50000.0 * scale);
    std::atomic<std::uint64_t> local_bad{0};
    std::vector<std::thread> loaders;
    loaders.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        loaders.emplace_back([&hex, &key_c, &local_bad, &kLocalIterations]() {
            KeyStore local;
            for (std::uint64_t i = 0; i < kLocalIterations; ++i) {
                if (local.LoadFromString(hex) != nullptr || !local.Accepts(local.Current()) ||
                    local.Accepts(key_c)) {
                    local_bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& loader : loaders) {
        loader.join();
    }
    CHECK_EQ(local_bad.load(), 0U);
    std::printf(
        "[CONCURRENT] shared-read:%llu ops（0 错误） local-load:%llu "
        "ops（0 错误）\n",
        static_cast<unsigned long long>(static_cast<std::uint64_t>(kThreads) * kReadIterations),
        static_cast<unsigned long long>(static_cast<std::uint64_t>(kThreads) * kLocalIterations));
}

// ---------------------------------------------------------------------------
// 泄漏检查（供 valgrind 小规模使用）
// ---------------------------------------------------------------------------

void RunLeaks(double scale) {
    const std::uint64_t cycles = static_cast<std::uint64_t>(100000.0 * scale);
    const std::string hex = HexEncode(MakeBytes(0x91, 32));
    std::printf("\n[MODE] leaks cycles=%llu\n", static_cast<unsigned long long>(cycles));
    const std::uint64_t rss_before = RssBytes();
    const std::uint64_t alloc_before = AllocCalls();
    const std::uint64_t free_before = FreeCalls();
    for (std::uint64_t i = 0; i < cycles; ++i) {
        KeyStore store;
        CHECK(store.LoadFromString(hex) == nullptr);
        CHECK(store.Rotate(MakeKey(static_cast<std::uint8_t>(i & 0xFFU), 16)));
        g_sink += store.Current().Masked().size();
        g_sink += store.Previous().Masked().size();
    }
    const std::uint64_t rss_after = RssBytes();
    const std::uint64_t allocs = AllocCalls() - alloc_before;
    const std::uint64_t frees = FreeCalls() - free_before;
    std::printf("[LEAK] RSS %llu -> %llu KiB (+%lld) alloc=%llu free=%llu net=%+lld\n",
                static_cast<unsigned long long>(rss_before / 1024U),
                static_cast<unsigned long long>(rss_after / 1024U),
                static_cast<long long>((rss_after - rss_before) / 1024),
                static_cast<unsigned long long>(allocs), static_cast<unsigned long long>(frees),
                static_cast<long long>(static_cast<std::int64_t>(allocs) -
                                       static_cast<std::int64_t>(frees)));
}

} // namespace

int main(int argc, char** argv) {
    double scale = 1.0;
    std::uint32_t seed = 20261029U;
    std::string mode = "all";
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument.rfind("--mode=", 0) == 0) {
            mode = std::string(argument.substr(7));
        } else if (argument.rfind("--seed=", 0) == 0) {
            seed = static_cast<std::uint32_t>(
                std::strtoul(std::string(argument.substr(7)).c_str(), nullptr, 10));
        } else {
            const double parsed = std::strtod(std::string(argument).c_str(), nullptr);
            if (parsed > 0.0) {
                scale = parsed;
            }
        }
    }

    std::printf("[keystore audit] scale=%.3f seed=%u mode=%s\n", scale, seed, mode.c_str());
    const std::uint64_t rss_start = RssBytes();
    const bool run_all = (mode == "all");
    if (run_all || mode == "bench") {
        RunBenchmarks(scale);
    }
    if (run_all || mode == "random") {
        RunRandom(scale, seed);
    }
    if (run_all || mode == "scenarios") {
        std::printf("\n[MODE] scenarios\n");
        ScenarioLoadSources();
        ScenarioMasking();
        ScenarioConstantTime(scale);
        ScenarioRotation();
        ScenarioConcurrent(scale);
    }
    if (mode == "leaks") {
        RunLeaks(scale);
    }

    const std::uint64_t rss_end = RssBytes();
    std::printf("\n[RESULT] checks=%d failures=%d RSS %llu -> %llu KiB sink=%llu\n", g_checks,
                g_failures, static_cast<unsigned long long>(rss_start / 1024U),
                static_cast<unsigned long long>(rss_end / 1024U),
                static_cast<unsigned long long>(g_sink));
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] audit_keystore: %d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("[PASS] audit_keystore\n");
    return 0;
}
