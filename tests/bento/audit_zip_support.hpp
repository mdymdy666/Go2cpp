/**
 * @file audit_zip_support.hpp
 * @brief zip/backup 审计程序共用的资源观测、随机变异与多线程 HTTP 测试服务器。
 *
 * @details 与 `test_backup_support.hpp`（单测用）分开，是因为审计程序需要：
 *          - 进程级资源观测：RSS / fd 数 / 子进程数 / 磁盘占用；
 *          - 可复现的随机数发生器（独立于 libstdc++ 实现）；
 *          - 多线程 HTTP 服务器：1000 并发上传时单线程 accept 循环会成为瓶颈；
 *          - 直接篡改 zip 字节的辅助函数（构造炸弹、Zip Slip、截断）。
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#pragma once

#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace audit {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// 资源观测
// ---------------------------------------------------------------------------

/// 当前进程 RSS（KiB）；读取失败返回 -1。
inline long RssKb() {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::strtol(line.c_str() + 6, nullptr, 10);
        }
    }
    return -1;
}

/// 当前进程打开的 fd 数（含 0/1/2）。
inline int FdCount() {
    std::error_code ec;
    int count = 0;
    for (fs::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end;
         it.increment(ec)) {
        ++count;
    }
    return count;
}

/// 把已释放的堆页还给 OS，让 RSS 读数不包含历史峰值。
inline void TrimMemory() { (void)::malloc_trim(0); }

/// 递归统计目录树占用的字节数；传入普通文件时直接返回其大小。
inline std::uint64_t DiskBytes(const fs::path& root) {
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        const std::uintmax_t size = fs::file_size(root, ec);
        return ec ? 0 : static_cast<std::uint64_t>(size);
    }
    std::uint64_t total = 0;
    fs::recursive_directory_iterator it(root, ec);
    if (ec) {
        return 0;
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code size_ec;
        if (it->is_regular_file(size_ec)) {
            total += it->file_size(size_ec);
        }
    }
    return total;
}

/// 系统当前进程数（观测 fork 压力）。
inline int ProcessCount() {
    std::error_code ec;
    int count = 0;
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().find_first_not_of("0123456789") ==
            std::string::npos) {
            ++count;
        }
    }
    return count;
}

/// 毫秒计时器。
class Stopwatch final {
public:
    Stopwatch() : m_begin(std::chrono::steady_clock::now()) {}
    void Restart() { m_begin = std::chrono::steady_clock::now(); }
    double Seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_begin)
            .count();
    }
    double Ms() const { return Seconds() * 1000.0; }

private:
    std::chrono::steady_clock::time_point m_begin;
};

// ---------------------------------------------------------------------------
// 断言与输出
// ---------------------------------------------------------------------------

inline int g_checks = 0;
inline int g_failures = 0;

/// 审计断言：失败不中断，最后统一报告。
#define AUDIT_CHECK(condition)                                                              \
    do {                                                                                    \
        ++audit::g_checks;                                                                  \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            ++audit::g_failures;                                                            \
        }                                                                                   \
    } while (0)

/// 打印汇总并返回进程退出码。
inline int Finish(const char* name) {
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] %s: %d/%d checks failed\n", name, g_failures, g_checks);
        return 1;
    }
    std::printf("[PASS] %s (%d checks)\n", name, g_checks);
    return 0;
}

// ---------------------------------------------------------------------------
// 可复现随机数（xorshift64*）
// ---------------------------------------------------------------------------

class Rng final {
public:
    explicit Rng(std::uint64_t seed) : m_state(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {}

    std::uint64_t Next() {
        m_state ^= m_state >> 12;
        m_state ^= m_state << 25;
        m_state ^= m_state >> 27;
        return m_state * 0x2545f4914f6cdd1dull;
    }
    /// [0, bound)
    std::uint64_t Range(std::uint64_t bound) {
        return bound == 0 ? 0 : Next() % bound;
    }
    bool Chance(std::uint32_t percent) { return Range(100) < percent; }

private:
    std::uint64_t m_state;
};

// ---------------------------------------------------------------------------
// zip 字节篡改辅助
// ---------------------------------------------------------------------------

/// 小端写 u16/u32 到字节数组。
inline void PutU16At(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint16_t value) {
    if (offset + 2 > bytes->size()) {
        return;
    }
    (*bytes)[offset] = static_cast<std::uint8_t>(value & 0xffu);
    (*bytes)[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
}

inline void PutU32At(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
    if (offset + 4 > bytes->size()) {
        return;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        (*bytes)[offset + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
    }
}

inline std::uint16_t GetU16At(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

inline std::uint32_t GetU32At(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

/// 找到 EOCD 的绝对偏移；找不到返回 npos。
inline std::size_t FindEocd(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 22) {
        return std::string::npos;
    }
    for (std::size_t position = bytes.size() - 22;; --position) {
        if (GetU32At(bytes, position) == 0x06054b50u) {
            return position;
        }
        if (position == 0) {
            break;
        }
    }
    return std::string::npos;
}

/// 写入整个文件。
inline bool WriteBytes(const fs::path& path, const std::vector<std::uint8_t>& data) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

/// 读取整个文件。
inline std::vector<std::uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> data;
    if (!in) {
        return data;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size > 0) {
        data.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (!in) {
            data.clear();
        }
    }
    return data;
}

// ---------------------------------------------------------------------------
// 多线程 HTTP/1.1 测试服务器
// ---------------------------------------------------------------------------

struct HttpRequest final {
    std::string method;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int index{0}; ///< 全局递增的请求序号（用于按次数返回不同响应）。

    /// 大小写不敏感取 header；不存在返回空串。
    std::string Header(const std::string& name) const {
        for (const auto& entry : headers) {
            if (entry.first.size() != name.size()) {
                continue;
            }
            bool same = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(entry.first[i])) !=
                    std::tolower(static_cast<unsigned char>(name[i]))) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return entry.second;
            }
        }
        return {};
    }
};

struct HttpReply final {
    int status{200};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    /// 发出响应前的等待（毫秒），用于构造慢服务器/超时场景。
    int delay_ms{0};
};

using HttpHandler = std::function<HttpReply(const HttpRequest&)>;

/**
 * @brief 极简多线程 HTTP/1.1 服务器（每个连接一个 detached 线程）。
 *
 * @details 只用于本地审计：一次连接一个请求（Connection: close），支持
 *          `Expect: 100-continue`、按请求序号返回不同响应、可配置延迟。
 *          请求体完整读取后再回调，避免半包导致 handler 看到截断数据。
 */
class HttpServer final {
public:
    explicit HttpServer(HttpHandler handler) : m_handler(std::move(handler)) {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen_fd < 0) {
            return;
        }
        int one = 1;
        ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(m_listen_fd, 4096) != 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return;
        }
        m_port = ntohs(address.sin_port);
        m_accept_thread = std::thread([this] { AcceptLoop(); });
    }

    ~HttpServer() {
        m_stopping = true;
        if (m_listen_fd >= 0) {
            ::shutdown(m_listen_fd, SHUT_RDWR);
        }
        if (m_accept_thread.joinable()) {
            m_accept_thread.join();
        }
        if (m_listen_fd >= 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
        // 等待所有 detach 的连接线程退出，避免使用已销毁的 handler。
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_active == 0; });
    }

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool valid() const noexcept { return m_port > 0; }
    int port() const noexcept { return m_port; }
    std::string Url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(m_port) + path;
    }
    /// 已处理的请求总数。
    int Requests() const { return m_request_count.load(); }
    /// 当前活跃连接线程数。
    int Active() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_active;
    }

private:
    void AcceptLoop() {
        while (!m_stopping) {
            sockaddr_in peer{};
            socklen_t peer_length = sizeof(peer);
            const int fd = ::accept(m_listen_fd, reinterpret_cast<sockaddr*>(&peer),
                                    &peer_length);
            if (fd < 0) {
                if (m_stopping) {
                    break;
                }
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping) {
                    ::close(fd);
                    break;
                }
                ++m_active;
            }
            std::thread([this, fd] {
                Handle(fd);
                ::close(fd);
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_active;
                m_cv.notify_all();
            }).detach();
        }
    }

    static bool SendAll(int fd, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n =
                ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    static std::string TrimCopy(const std::string& text) {
        std::size_t begin = 0;
        std::size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
            ++begin;
        }
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
            --end;
        }
        return text.substr(begin, end - begin);
    }

    void Handle(int fd) {
        timeval timeout{10, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::string buffer;
        char chunk[16384];
        std::size_t marker = std::string::npos;
        while (buffer.size() < 256 * 1024) {
            marker = buffer.find("\r\n\r\n");
            if (marker != std::string::npos) {
                break;
            }
            const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
            if (got <= 0) {
                return;
            }
            buffer.append(chunk, static_cast<std::size_t>(got));
        }
        if (marker == std::string::npos) {
            return;
        }
        HttpRequest request;
        const std::string header_block = buffer.substr(0, marker);
        const std::size_t line_end = header_block.find("\r\n");
        {
            const std::string request_line = header_block.substr(0, line_end);
            std::size_t first = request_line.find(' ');
            std::size_t second = request_line.rfind(' ');
            if (first == std::string::npos || second == std::string::npos || second <= first) {
                return;
            }
            request.method = request_line.substr(0, first);
            request.path = request_line.substr(first + 1, second - first - 1);
        }
        std::size_t position = line_end == std::string::npos ? header_block.size() : line_end + 2;
        while (position < header_block.size()) {
            const std::size_t end = header_block.find("\r\n", position);
            const std::string line = header_block.substr(
                position, end == std::string::npos ? std::string::npos : end - position);
            position = end == std::string::npos ? header_block.size() : end + 2;
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            request.headers.emplace_back(TrimCopy(line.substr(0, colon)),
                                         TrimCopy(line.substr(colon + 1)));
        }
        request.index = m_request_count.fetch_add(1);
        const std::string expect = request.Header("Expect");
        if (!expect.empty() && expect.find("100-continue") != std::string::npos) {
            if (!SendAll(fd, "HTTP/1.1 100 Continue\r\n\r\n")) {
                return;
            }
        }
        std::size_t content_length = 0;
        const std::string length_header = request.Header("Content-Length");
        if (!length_header.empty()) {
            content_length = static_cast<std::size_t>(std::strtoull(length_header.c_str(),
                                                                    nullptr, 10));
        }
        request.body = buffer.substr(marker + 4);
        while (request.body.size() < content_length) {
            const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
            if (got <= 0) {
                break;
            }
            request.body.append(chunk, static_cast<std::size_t>(got));
        }
        if (request.body.size() > content_length) {
            request.body.resize(content_length);
        }
        const HttpReply reply = m_handler ? m_handler(request) : HttpReply{};
        if (reply.delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(reply.delay_ms));
        }
        std::string text = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\n";
        bool has_length = false;
        for (const auto& header : reply.headers) {
            if (header.first.size() == 14) {
                std::string lower = header.first;
                for (char& ch : lower) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (lower == "content-length") {
                    has_length = true;
                }
            }
            text += header.first + ": " + header.second + "\r\n";
        }
        if (!has_length) {
            text += "Content-Length: " + std::to_string(reply.body.size()) + "\r\n";
        }
        text += "Connection: close\r\n\r\n";
        if (request.method != "HEAD") {
            text += reply.body;
        }
        (void)SendAll(fd, text);
    }

    HttpHandler m_handler;
    int m_listen_fd{-1};
    int m_port{0};
    std::thread m_accept_thread;
    std::atomic<bool> m_stopping{false};
    std::atomic<int> m_request_count{0};
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    int m_active{0};
};

}  // namespace audit
