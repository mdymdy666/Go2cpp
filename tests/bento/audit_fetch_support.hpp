#pragma once

/**
 * @file audit_fetch_support.hpp
 * @brief fetch 审计（压测/随机/场景）用的测试支撑：断言、可脚本化 HTTP
 *        服务器、可随机访问的确定性数据流、资源观测与目录残留检查。
 * @details 与 `tests/bento/test_util.hpp` 的差异：
 *          - **不引入 link.hpp**：fetch 审计要验证「app 层不依赖 link」，
 *            支撑层自己也不能把 link 拉进编译依赖；
 *          - **流式 body**：百 MB 级响应不能先在服务端内存里攒出来，这里用
 *            可随机访问的生成器边生成边发送；
 *          - **异常形态可脚本化**：静默不响应、慢滴流、接受即关闭、任意原始
 *            字节（非法头/截断/chunk 语法错误），供随机测试与场景测试复用。
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/app/fetch.hpp"
#include "go2cpp/hash/sha256.hpp"

namespace audit {

namespace fs = std::filesystem;

using go2cpp::bento::app::FetchOptions;
using go2cpp::bento::app::FetchResult;
using go2cpp::bento::app::FetchStatus;
using go2cpp::bento::app::kFetchDefaultMaxBytes;
using go2cpp::bento::app::kFetchMaxBytesCeiling;
using go2cpp::bento::app::kFetchMaxHeaderBytes;

// ---------------------------------------------------------------------------
// 断言与统计
// ---------------------------------------------------------------------------

inline int g_checks = 0;
inline int g_failures = 0;
inline int g_skips = 0;
/// CTest 原生 SKIP 语义：环境不满足时整份测试以该码退出。
inline constexpr int kSkipReturnCode = 77;

/// 标记环境不满足（不满足时不能用 PASS 掩盖）。
inline void Skip(const char* reason) {
    std::printf("[SKIP] %s\n", reason);
    ++g_skips;
}

/// 统一输出结果；失败优先于跳过。
inline int Finish(const char* name) {
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] %s: %d/%d checks failed\n", name, g_failures, g_checks);
        return 1;
    }
    if (g_skips != 0) {
        std::printf("[SKIP] %s: %d checks, %d skipped\n", name, g_checks, g_skips);
        return kSkipReturnCode;
    }
    std::printf("[PASS] %s (%d checks)\n", name, g_checks);
    return 0;
}

/// 把 glibc 的空闲 arena 尽量归还给系统，缩小跨测量之间的 RSS 噪声。
inline void TrimMemory() {
    std::vector<char> ballast(16 * 1024 * 1024, 0);
    ballast.clear();
    ballast.shrink_to_fit();
}

}  // namespace audit

// 断言宏：记录检查数，失败不中断，便于一次看到全部问题。
#define AUDIT_CHECK(condition)                                                          \
    do {                                                                                \
        ++audit::g_checks;                                                              \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++audit::g_failures;                                                        \
        }                                                                               \
    } while (0)

#define AUDIT_CHECK_EQ(actual, expected)                                                   \
    do {                                                                                   \
        ++audit::g_checks;                                                                 \
        if (!((actual) == (expected))) {                                                   \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual,  \
                         #expected);                                                       \
            ++audit::g_failures;                                                           \
        }                                                                                  \
    } while (0)

/// 带上下文的断言（message 为可拼接表达式，拼成 std::string）。
#define AUDIT_CHECK_MSG(condition, message)                                             \
    do {                                                                                \
        ++audit::g_checks;                                                              \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "[FAIL] %s:%d: %s | %s\n", __FILE__, __LINE__,         \
                         #condition, std::string(message).c_str());                     \
            ++audit::g_failures;                                                        \
        }                                                                               \
    } while (0)

/// 状态断言：失败时打印实际状态名与诊断，比 == 更容易定位。
#define AUDIT_CHECK_STATUS(result, expected, context)                                        \
    do {                                                                                     \
        ++audit::g_checks;                                                                   \
        if ((result).status != (expected)) {                                                 \
            std::fprintf(stderr, "[FAIL] %s:%d: %s | %s: 期望 %s，实际 %s（%s）\n",         \
                         __FILE__, __LINE__, #result, std::string(context).c_str(),         \
                         go2cpp::bento::app::FetchStatusToString(expected),                  \
                         go2cpp::bento::app::FetchStatusToString((result).status),           \
                         (result).message.c_str());                                          \
            ++audit::g_failures;                                                             \
        }                                                                                    \
    } while (0)

namespace audit {

// ---------------------------------------------------------------------------
// 确定性数据流
// ---------------------------------------------------------------------------

/// 生成块大小；发送端与校验端必须用同一块对齐规则。
inline constexpr std::size_t kPayloadBlock = 64 * 1024;

/// splitmix64 的 32 位混合（从 (seed, block) 派生块内 LCG 种子）。
inline std::uint32_t Mix32(std::uint64_t value, std::uint64_t salt) noexcept {
    value ^= salt + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return static_cast<std::uint32_t>(value >> 32);
}

/// 填充一个块；`offset` 必须是 `kPayloadBlock` 的整数倍。
inline void FillPayloadBlock(std::uint32_t seed, std::uint64_t offset, char* out,
                             std::size_t size) noexcept {
    std::uint32_t state = Mix32(seed, offset / kPayloadBlock) | 1u;
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        out[i] = static_cast<char>(state >> 24);
    }
}

/// 可随机访问的确定性字节流（顺序读，块对齐生成）。
class PayloadReader final {
   public:
    PayloadReader(std::uint32_t seed, std::uint64_t total) : m_seed(seed), m_total(total) {}

    /// 读至多 `want` 字节；返回 0 表示已到末尾。
    std::size_t Read(char* out, std::size_t want) noexcept {
        if (m_offset >= m_total) {
            return 0;
        }
        if (m_block_pos == m_block_len) {
            const std::uint64_t remaining = m_total - m_block_begin;
            const std::size_t block =
                static_cast<std::size_t>(std::min<std::uint64_t>(kPayloadBlock, remaining));
            FillPayloadBlock(m_seed, m_block_begin, m_block.data(), block);
            m_block_begin += block;
            m_block_len = block;
            m_block_pos = 0;
        }
        const std::size_t take = std::min(want, m_block_len - m_block_pos);
        std::memcpy(out, m_block.data() + m_block_pos, take);
        m_block_pos += take;
        m_offset += take;
        return take;
    }

   private:
    std::uint32_t m_seed{0};
    std::uint64_t m_total{0};
    std::uint64_t m_offset{0};
    std::uint64_t m_block_begin{0};
    std::array<char, kPayloadBlock> m_block{};
    std::size_t m_block_len{0};
    std::size_t m_block_pos{0};
};

/// 一次性计算「同一条生成流」的 SHA-256（十六进制小写）。
inline std::string PayloadSha256(std::uint32_t seed, std::uint64_t total) {
    go2cpp::hash::Sha256 hasher;
    PayloadReader reader(seed, total);
    std::array<char, kPayloadBlock> buffer{};
    for (;;) {
        const std::size_t got = reader.Read(buffer.data(), buffer.size());
        if (got == 0) {
            break;
        }
        hasher.Write(std::string_view(buffer.data(), got));
    }
    const std::vector<std::uint8_t> digest = hasher.Sum();
    if (digest.size() < 32) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (std::size_t i = 0; i < 32; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return out;
}

/// 「重复同一块」流的 SHA-256；对应 `ResponseSpec::repeat_block`。
/// @details 用于纯吞吐压测：服务端不再花时间生成数据，瓶颈落回传输/落盘侧。
inline std::string PayloadSha256Repeated(std::uint32_t seed, std::uint64_t total) {
    go2cpp::hash::Sha256 hasher;
    std::array<char, kPayloadBlock> block{};
    const std::size_t block_size =
        static_cast<std::size_t>(std::min<std::uint64_t>(kPayloadBlock, total));
    if (block_size == 0) {
        return PayloadSha256(seed, 0);
    }
    FillPayloadBlock(seed, 0, block.data(), block_size);
    std::uint64_t sent = 0;
    while (sent < total) {
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(block_size, total - sent));
        hasher.Write(std::string_view(block.data(), take));
        sent += take;
    }
    const std::vector<std::uint8_t> digest = hasher.Sum();
    if (digest.size() < 32) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (std::size_t i = 0; i < 32; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return out;
}

// ---------------------------------------------------------------------------
// 文件与资源观测
// ---------------------------------------------------------------------------

/// 递归删除（失败忽略）。
inline void RemoveTree(const fs::path& path) noexcept {
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// 唯一的临时工作目录（mkdtemp，0700）；析构自动删除。
class TempDir final {
   public:
    explicit TempDir(const char* tag) {
        std::string pattern =
            (fs::temp_directory_path() / (std::string("audit_fetch_") + tag + "_XXXXXX")).string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (::mkdtemp(buffer.data()) != nullptr) {
            m_path = buffer.data();
        }
    }
    ~TempDir() { RemoveTree(m_path); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    bool ok() const noexcept { return !m_path.empty(); }
    const fs::path& path() const noexcept { return m_path; }
    std::string str() const { return m_path.string(); }

   private:
    fs::path m_path;
};

/// 目录内条目数（含隐藏文件）。
inline std::size_t CountEntries(const fs::path& dir) {
    std::error_code ec;
    std::size_t count = 0;
    fs::directory_iterator iterator(dir, ec);
    const fs::directory_iterator end;
    while (!ec && iterator != end) {
        ++count;
        iterator.increment(ec);
    }
    return count;
}

/// 目录内是否残留本模块的临时产物（`.go2cpp-part.<pid>.<seq>[.XXXXXX]`）。
inline bool HasTempArtifacts(const fs::path& dir) {
    std::error_code ec;
    fs::directory_iterator iterator(dir, ec);
    const fs::directory_iterator end;
    while (!ec && iterator != end) {
        const std::string name = iterator->path().filename().string();
        if (name.find(".go2cpp-part.") != std::string::npos) {
            return true;
        }
        iterator.increment(ec);
    }
    return false;
}

/// 目录内所有普通文件的字节总和。
inline std::uint64_t DirectoryBytes(const fs::path& dir) {
    std::error_code ec;
    std::uint64_t total = 0;
    fs::directory_iterator iterator(dir, ec);
    const fs::directory_iterator end;
    while (!ec && iterator != end) {
        std::error_code size_ec;
        const auto size = fs::file_size(iterator->path(), size_ec);
        if (!size_ec) {
            total += static_cast<std::uint64_t>(size);
        }
        iterator.increment(ec);
    }
    return total;
}

/// 读 /proc/self/status 的某个 Vm* 字段（KB）；失败返回 0。
inline std::uint64_t ReadVmKb(const char* field) {
    std::ifstream input("/proc/self/status");
    if (!input) {
        return 0;
    }
    const std::size_t field_len = std::strlen(field);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind(field, 0) != 0) {
            continue;
        }
        const char* cursor = line.c_str() + field_len;
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        return std::strtoull(cursor, nullptr, 10);
    }
    return 0;
}

/// 当前 RSS（字节）。
inline std::uint64_t CurrentRss() { return ReadVmKb("VmRSS:") * 1024; }
/// 进程生命周期内的 RSS 峰值（字节）。
inline std::uint64_t PeakRss() { return ReadVmKb("VmHWM:") * 1024; }

/// 当前进程打开的 fd 数。
inline std::size_t OpenFdCount() {
    std::error_code ec;
    std::size_t count = 0;
    fs::directory_iterator iterator("/proc/self/fd", ec);
    const fs::directory_iterator end;
    while (!ec && iterator != end) {
        ++count;
        iterator.increment(ec);
    }
    // 目录迭代自身占用一个 fd，扣掉。
    return count > 0 ? count - 1 : 0;
}

/// 后台资源采样器：每 2ms 记录一次 RSS/fd 峰值。
class ResourceSampler final {
   public:
    ~ResourceSampler() { Stop(); }

    void Start() {
        m_peak_fd.store(OpenFdCount());
        m_peak_rss.store(CurrentRss());
        m_running.store(true);
        m_thread = std::thread([this]() { Loop(); });
    }

    void Stop() {
        if (!m_running.exchange(false)) {
            return;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    std::size_t peak_fd() const noexcept { return m_peak_fd.load(); }
    std::uint64_t peak_rss() const noexcept { return m_peak_rss.load(); }

   private:
    void Loop() {
        while (m_running.load()) {
            const std::size_t fd = OpenFdCount();
            std::size_t previous_fd = m_peak_fd.load();
            while (fd > previous_fd && !m_peak_fd.compare_exchange_weak(previous_fd, fd)) {
            }
            const std::uint64_t rss = CurrentRss();
            std::uint64_t previous_rss = m_peak_rss.load();
            while (rss > previous_rss && !m_peak_rss.compare_exchange_weak(previous_rss, rss)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    std::atomic<bool> m_running{false};
    std::atomic<std::size_t> m_peak_fd{0};
    std::atomic<std::uint64_t> m_peak_rss{0};
    std::thread m_thread;
};

// ---------------------------------------------------------------------------
// 可脚本化 HTTP 服务器
// ---------------------------------------------------------------------------

/// 响应形态。
enum class ResponseKind : std::uint8_t {
    kRaw,       ///< 原样发送 `raw` 后关闭（非法头/截断/语法错误都走这里）。
    kBody,      ///< 先发 `head`，再流式发 `body_bytes` 个生成字节（可选 chunked）。
    kSilent,    ///< 读完请求后不发送任何字节，保持连接（触发读空闲超时）。
    kSlowDrip,  ///< 每 `drip_ms` 发送 1 字节，共 `body_bytes` 字节（触发总超时）。
    kCloseNow,  ///< 接受后立即关闭（连接被重置）。
};

/// 单条脚本化响应。
struct ResponseSpec final {
    ResponseKind kind{ResponseKind::kRaw};
    /// kRaw：完整原始字节（可包含非法状态行/头）。
    std::string raw;
    /// kBody/kSlowDrip：响应头（含 CRLFCRLF）。
    std::string head;
    /// kBody/kSlowDrip：响应体字节数（由确定性生成器产生）。
    std::uint64_t body_bytes{0};
    /// 生成器种子。
    std::uint32_t seed{0};
    /// >0 时用 chunked 编码，每个 chunk 最多这么多字节。
    std::uint64_t chunk_bytes{0};
    /// true 时重复发送同一个块（纯吞吐压测，服务端零生成开销）。
    bool repeat_block{false};
    /// 发送前的延迟（毫秒）。
    std::uint32_t delay_ms{0};
    /// kSlowDrip：每字节间隔（毫秒）。
    std::uint32_t drip_ms{0};
};

/**
 * @brief 可脚本化 HTTP/1.1 测试服务器。
 *
 * @details 每个路径 `/r/<index>` 映射到 `responses[index]`。多个 acceptor 线程
 *          共享监听 fd，每个连接在 acceptor 线程内同步处理（响应都很小，无需
 *          每连接建线程）。因此 `acceptors` 就是最大并发处理数。
 */
class AuditServer final {
   public:
    AuditServer() = default;
    ~AuditServer() { Stop(); }

    AuditServer(const AuditServer&) = delete;
    AuditServer& operator=(const AuditServer&) = delete;

    void SetResponses(std::vector<ResponseSpec> responses) { m_responses = std::move(responses); }

    /// 启动；`acceptors` 个处理线程，监听 backlog 固定 4096（支持千级并发）。
    bool Start(int acceptors = 4) {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_listen_fd < 0) {
            return false;
        }
        int reuse = 1;
        ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(m_listen_fd, 4096) != 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return false;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return false;
        }
        m_port = ntohs(address.sin_port);
        m_running.store(true);
        m_workers.reserve(static_cast<std::size_t>(acceptors));
        for (int index = 0; index < acceptors; ++index) {
            m_workers.emplace_back([this]() { AcceptLoop(); });
        }
        return true;
    }

    void Stop() {
        if (!m_running.exchange(false)) {
            return;
        }
        if (m_listen_fd >= 0) {
            ::shutdown(m_listen_fd, SHUT_RDWR);
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
        for (std::thread& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        m_workers.clear();
    }

    /// `http://127.0.0.1:<port>`。
    std::string BaseUrl() const {
        return "http://127.0.0.1:" + std::to_string(static_cast<unsigned>(m_port));
    }

    /// 第 `index` 条响应对应的 URL。
    std::string Url(std::size_t index) const {
        return BaseUrl() + "/r/" + std::to_string(index);
    }

    /// 已处理连接数。
    int requests() const noexcept { return m_requests.load(); }
    /// 已发送的响应体字节数。
    std::uint64_t bytes_sent() const noexcept { return m_bytes_sent.load(); }

   private:
    void AcceptLoop() {
        while (m_running.load()) {
            const int client = ::accept(m_listen_fd, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            // 防止对端不读导致 send 永久阻塞（压测里客户端总是读，这里只是兜底）。
            timeval timeout{};
            timeout.tv_sec = 5;
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            // 连接一被接住就计数：静默/慢响应用例也要算一次请求。
            m_requests.fetch_add(1, std::memory_order_relaxed);
            HandleConnection(client);
            ::close(client);
        }
    }

    static bool SendAll(int fd, std::string_view data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t result =
                ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (result > 0) {
                sent += static_cast<std::size_t>(result);
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
    }

    /// 把生成的 body 流式发出去（chunked 或直发）；返回发送字节数。
    std::uint64_t SendBody(int fd, const ResponseSpec& spec) {
        std::array<char, kPayloadBlock> buffer{};
        std::uint64_t sent_total = 0;
        if (spec.repeat_block) {
            const std::size_t block_size =
                static_cast<std::size_t>(std::min<std::uint64_t>(kPayloadBlock, spec.body_bytes));
            if (block_size == 0) {
                return 0;
            }
            FillPayloadBlock(spec.seed, 0, buffer.data(), block_size);
            const bool chunked = spec.chunk_bytes != 0;
            const std::size_t chunk =
                chunked ? static_cast<std::size_t>(
                              std::min<std::uint64_t>(spec.chunk_bytes, block_size))
                        : block_size;
            while (sent_total < spec.body_bytes) {
                const std::size_t take = static_cast<std::size_t>(
                    std::min<std::uint64_t>(chunk, spec.body_bytes - sent_total));
                if (chunked) {
                    char size_line[32] = {};
                    std::snprintf(size_line, sizeof(size_line), "%zx\r\n", take);
                    if (!SendAll(fd, size_line)) {
                        return sent_total;
                    }
                }
                if (!SendAll(fd, std::string_view(buffer.data(), take))) {
                    return sent_total;
                }
                if (chunked && !SendAll(fd, "\r\n")) {
                    return sent_total;
                }
                sent_total += take;
            }
            if (chunked) {
                SendAll(fd, "0\r\n\r\n");
            }
            return sent_total;
        }
        PayloadReader reader(spec.seed, spec.body_bytes);
        if (spec.chunk_bytes == 0) {
            for (;;) {
                const std::size_t got = reader.Read(buffer.data(), buffer.size());
                if (got == 0) {
                    break;
                }
                if (!SendAll(fd, std::string_view(buffer.data(), got))) {
                    break;
                }
                sent_total += got;
            }
            return sent_total;
        }
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(spec.chunk_bytes, kPayloadBlock));
        for (;;) {
            const std::size_t got = reader.Read(buffer.data(), chunk);
            if (got == 0) {
                break;
            }
            char size_line[32] = {};
            std::snprintf(size_line, sizeof(size_line), "%zx\r\n", got);
            if (!SendAll(fd, size_line) ||
                !SendAll(fd, std::string_view(buffer.data(), got)) || !SendAll(fd, "\r\n")) {
                return sent_total;
            }
            sent_total += got;
        }
        SendAll(fd, "0\r\n\r\n");
        return sent_total;
    }

    /// 读取请求头（最多 8 KiB）；返回请求路径，失败返回空。
    std::string ReadRequestPath(int fd) {
        std::string request;
        char buffer[2048];
        for (;;) {
            if (request.find("\r\n\r\n") != std::string::npos || request.size() > 8 * 1024) {
                break;
            }
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 200) <= 0) {
                return {};
            }
            const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                return {};
            }
            request.append(buffer, static_cast<std::size_t>(got));
        }
        const std::size_t first_space = request.find(' ');
        if (first_space == std::string::npos) {
            return {};
        }
        const std::size_t second_space = request.find(' ', first_space + 1);
        if (second_space == std::string::npos) {
            return {};
        }
        return request.substr(first_space + 1, second_space - first_space - 1);
    }

    /// 解析 `/r/<index>`；越界返回 `m_responses.size()`。
    std::size_t ParseIndex(std::string_view path) const {
        constexpr std::string_view kPrefix = "/r/";
        if (path.rfind(kPrefix, 0) != 0) {
            return m_responses.size();
        }
        std::size_t index = 0;
        for (char ch : path.substr(kPrefix.size())) {
            if (ch < '0' || ch > '9') {
                return m_responses.size();
            }
            index = index * 10 + static_cast<std::size_t>(ch - '0');
            if (index > 100000) {
                return m_responses.size();
            }
        }
        return index < m_responses.size() ? index : m_responses.size();
    }

    void HandleConnection(int fd) {
        const std::string path = ReadRequestPath(fd);
        const std::size_t index = ParseIndex(path);
        if (index >= m_responses.size()) {
            SendAll(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
        const ResponseSpec& spec = m_responses[index];
        if (spec.delay_ms != 0) {
            SleepSliced(spec.delay_ms);
        }
        switch (spec.kind) {
            case ResponseKind::kCloseNow:
                return;
            case ResponseKind::kSilent:
                WaitUntilStopped();
                return;
            case ResponseKind::kRaw:
                SendAll(fd, spec.raw);
                m_bytes_sent.fetch_add(spec.raw.size(), std::memory_order_relaxed);
                return;
            case ResponseKind::kSlowDrip: {
                SendAll(fd, spec.head);
                PayloadReader reader(spec.seed, spec.body_bytes);
                char byte = 0;
                std::uint64_t sent = 0;
                while (reader.Read(&byte, 1) == 1) {
                    if (!SendAll(fd, std::string_view(&byte, 1))) {
                        break;
                    }
                    ++sent;
                    std::this_thread::sleep_for(std::chrono::milliseconds(spec.drip_ms));
                }
                m_bytes_sent.fetch_add(sent, std::memory_order_relaxed);
                return;
            }
            case ResponseKind::kBody:
                SendAll(fd, spec.head);
                m_bytes_sent.fetch_add(SendBody(fd, spec), std::memory_order_relaxed);
                return;
        }
    }

    /// 分片睡眠，保证 Stop() 不会被长睡眠拖延。
    void SleepSliced(std::uint32_t total_ms) {
        for (std::uint32_t waited = 0; waited < total_ms && m_running.load(); waited += 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    /// 保持连接直到服务器停止（对端关闭时 send 会失败，这里不关心）。
    void WaitUntilStopped() {
        while (m_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    int m_listen_fd{-1};
    std::uint16_t m_port{0};
    std::vector<std::thread> m_workers;
    std::vector<ResponseSpec> m_responses;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_requests{0};
    std::atomic<std::uint64_t> m_bytes_sent{0};
};

// ---------------------------------------------------------------------------
// 并发与工具
// ---------------------------------------------------------------------------

/**
 * @brief 用 pthread 创建固定栈线程，避免 1000 并发时每线程 8 MiB 虚拟栈。
 */
class ThreadGroup final {
   public:
    ThreadGroup() = default;
    ~ThreadGroup() { JoinAll(); }

    ThreadGroup(const ThreadGroup&) = delete;
    ThreadGroup& operator=(const ThreadGroup&) = delete;

    /// 启动一个任务；返回 false 表示创建失败（调用方负责计数）。
    bool Spawn(std::function<void()> task, std::size_t stack_bytes = 512 * 1024) {
        auto* holder = new std::function<void()>(std::move(task));
        pthread_attr_t attributes;
        if (pthread_attr_init(&attributes) != 0) {
            delete holder;
            return false;
        }
        const std::size_t minimum = static_cast<std::size_t>(PTHREAD_STACK_MIN);
        if (stack_bytes < minimum) {
            stack_bytes = minimum;
        }
        pthread_attr_setstacksize(&attributes, stack_bytes);
        pthread_t thread;
        const int created = pthread_create(
            &thread, &attributes,
            [](void* raw) -> void* {
                auto* task_holder = static_cast<std::function<void()>*>(raw);
                (*task_holder)();
                delete task_holder;
                return nullptr;
            },
            holder);
        pthread_attr_destroy(&attributes);
        if (created != 0) {
            delete holder;
            return false;
        }
        m_threads.push_back(thread);
        return true;
    }

    std::size_t size() const noexcept { return m_threads.size(); }

    void JoinAll() {
        for (pthread_t thread : m_threads) {
            pthread_join(thread, nullptr);
        }
        m_threads.clear();
    }

   private:
    std::vector<pthread_t> m_threads;
};

/// 单调时钟毫秒。
inline double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

/// 打印一条测量记录（供报告引用）。
inline void Report(const std::string& name, std::uint64_t bytes, double milliseconds,
                   std::uint64_t peak_rss, std::size_t peak_fd) {
    const double seconds = milliseconds / 1000.0;
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const double throughput = seconds > 0.0 ? mb / seconds : 0.0;
    std::printf("[DATA] %-30s bytes=%-10llu time=%8.1fms throughput=%7.1fMiB/s "
                "peak_rss=%5.1fMiB peak_fd=%zu\n",
                name.c_str(), static_cast<unsigned long long>(bytes), milliseconds, throughput,
                static_cast<double>(peak_rss) / (1024.0 * 1024.0), peak_fd);
}

/// 检查一个失败结果必须满足的不变量：无路径、无临时残留。
inline void CheckFailureCleanup(const FetchResult& result, const fs::path& dir,
                                const std::string& context) {
    AUDIT_CHECK_MSG(result.path.empty(), context + ": 失败时 path 应为空");
    AUDIT_CHECK_MSG(!HasTempArtifacts(dir), context + ": 失败后不应残留 .go2cpp-part.* 临时文件");
}

}  // namespace audit
