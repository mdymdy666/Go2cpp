/**
 * @file fetch.cpp
 * @brief MOD-27：HTTP(S) 下载、路径沙箱、摘要校验、重试与超时实现。
 * @details 设计取舍（与头文件中的接口契约对应）：
 *
 *          - **两条传输路径**：http 用本文件内的最小 HTTP/1.1 客户端
 *            （POSIX socket + poll），https 交给系统 curl。原因是仓库不引入
 *            第三方 TLS 库，而 HTTP 自研可以精确控制超时/上限并让测试不依赖
 *            curl 的行为；两条路径共享外层重试、沙箱、摘要与原子落盘。
 *          - **流式写盘**：响应体边读边写临时文件并同步更新 SHA-256，不在
 *            内存里攒整个文件；`Content-Length` 先预检，chunked 与 EOF
 *            模式按累计字节数检查上限。
 *          - **临时文件安全**：http 路径用 `O_CREAT|O_EXCL|O_NOFOLLOW` 写
 *            名字带 pid 与单调序号的临时文件；https 路径让 curl 写进
 *            `mkdtemp` 创建的 0700 私有目录，再由本进程用 `O_NOFOLLOW` 校验
 *            类型/大小/摘要后 `rename`。两条路径都不跟随符号链接，失败路径
 *            统一清理临时文件与目录。
 *          - **错误分类**：DNS/连接/超时/协议/HTTP/摘要各自独立，便于调用方
 *            决定是否重试；重试决策只看分类与状态码，不看人类可读消息。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/app/fetch.hpp"

#include "go2cpp/bento/app/detail/retry.hpp"

#include "go2cpp/hash/sha256.hpp"
#include "go2cpp/os/proc.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#if defined(__linux__)
#include <linux/fs.h>  // RENAME_NOREPLACE：排他落盘标志
#endif
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace go2cpp::bento::app {
namespace {

using Clock = std::chrono::steady_clock;

/// 临时文件序号；只在同一进程内保证唯一。
std::atomic<std::uint64_t> g_temp_seq{0};

// ---------------------------------------------------------------------------
// 通用小工具
// ---------------------------------------------------------------------------

/// 是否 ASCII 控制字符（含 NUL，不含 DEL 之外的可打印字节）。
bool IsControlByte(char ch) noexcept {
    const auto byte = static_cast<unsigned char>(ch);
    return byte < 0x20 || byte == 0x7f;
}

/// ASCII 小写化（只影响 A-Z）。
std::string ToLowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

/// 去掉首尾 ASCII 空白（空格、\t、\r、\n）。
std::string TrimAscii(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

/// 大小写不敏感的前缀判断（用于 scheme）。
bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const char left = text[i];
        const char right = prefix[i];
        const char lower_left =
            (left >= 'A' && left <= 'Z') ? static_cast<char>(left - 'A' + 'a') : left;
        const char lower_right =
            (right >= 'A' && right <= 'Z') ? static_cast<char>(right - 'A' + 'a') : right;
        if (lower_left != lower_right) {
            return false;
        }
    }
    return true;
}

/// 十六进制字符值；非法返回 -1。
int HexValue(char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/// 32 字节摘要转 64 位小写十六进制。
std::string DigestToHex(const std::uint8_t* digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (std::size_t i = 0; i < 32; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return out;
}

/// 从已打开的 fd 流式计算 SHA-256 与字节数（不关闭 fd）。
/// @details HTTPS 路径先用 `O_NOFOLLOW` 打开并 `fstat` 校验类型，再在同一个
///          fd 上算摘要，避免「校验的是 A 文件、读的是 B 文件」。
bool HashFd(int fd, std::string* hex, std::uint64_t* bytes) {
    if (fd < 0) {
        return false;
    }
    go2cpp::hash::Sha256 hasher;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        const ssize_t got = ::read(fd, buffer.data(), buffer.size());
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        hasher.Write(go2cpp::hash::AsBytes(buffer.data(), static_cast<std::size_t>(got)));
        total += static_cast<std::uint64_t>(got);
    }
    const std::vector<std::uint8_t> digest = hasher.Sum();
    if (digest.size() < 32) {
        return false;
    }
    *hex = DigestToHex(digest.data());
    *bytes = total;
    return true;
}

/// 把毫秒时长格式化为 curl 需要的秒（含三位小数）。
std::string FormatSeconds(std::uint32_t milliseconds) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.3f",
                  static_cast<double>(milliseconds) / 1000.0);
    return std::string(buffer);
}

/// 距离 `until` 还剩多少毫秒（已过期返回 0，过大截到 int 上限）。
int RemainingMs(Clock::time_point until) noexcept {
    const auto now = Clock::now();
    if (until <= now) {
        return 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
    if (remaining > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(remaining);
}

// ---------------------------------------------------------------------------
// URL 解析
// ---------------------------------------------------------------------------

/// 解析后的 URL 组件；`path` 已含查询串，至少是 `/`。
struct UrlParts final {
    bool https{false};
    std::string host;  ///< 不含 IPv6 方括号
    std::string port;  ///< 字符串形式，默认 80/443
    std::string path;  ///< origin-form（以 `/` 开头，可带 `?query`）
};

/// 主机名带方括号（IPv6）与端口（非默认端口才带）。
std::string HostHeader(const UrlParts& url) {
    std::string host =
        (url.host.find(':') != std::string::npos) ? "[" + url.host + "]" : url.host;
    const std::string default_port = url.https ? "443" : "80";
    if (url.port != default_port) {
        host += ":" + url.port;
    }
    return host;
}

/// `scheme://authority`，用于拼接重定向目标与 curl 参数。
std::string Origin(const UrlParts& url) {
    return std::string(url.https ? "https://" : "http://") + HostHeader(url);
}

/// `scheme://authority/path?query`。
std::string BuildUrlText(const UrlParts& url) { return Origin(url) + url.path; }

/// 校验端口：纯数字且 1..65535。
bool IsValidPort(std::string_view port) noexcept {
    if (port.empty() || port.size() > 5) {
        return false;
    }
    unsigned value = 0;
    for (char ch : port) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(ch - '0');
    }
    return value >= 1 && value <= 65535;
}

/**
 * @brief 解析 http(s) URL。
 * @details 只接受绝对 URL；拒绝 userinfo、裸 IPv6（必须用 `[]`）、空白与
 *          控制字符。fragment 被丢弃（不发送给服务器）。
 */
bool ParseUrl(std::string_view text, UrlParts* out, std::string* error) {
    const auto fail = [error](const char* why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (out == nullptr) {
        return fail("输出为空");
    }
    if (text.empty() || text.size() > kFetchMaxUrlBytes) {
        return fail("URL 为空或超过长度上限");
    }
    for (char ch : text) {
        if (IsControlByte(ch) || ch == ' ') {
            return fail("URL 含控制字符或空格");
        }
    }

    std::string_view rest = text;
    bool https = false;
    if (StartsWithIgnoreCase(rest, "http://")) {
        rest.remove_prefix(7);
    } else if (StartsWithIgnoreCase(rest, "https://")) {
        https = true;
        rest.remove_prefix(8);
    } else {
        return fail("仅支持 http/https");
    }
    if (const std::size_t fragment = rest.find('#'); fragment != std::string_view::npos) {
        rest = rest.substr(0, fragment);
    }
    const std::size_t authority_end = rest.find_first_of("/?");
    const std::string_view authority =
        authority_end == std::string_view::npos ? rest : rest.substr(0, authority_end);
    const std::string_view tail =
        authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);
    if (authority.empty()) {
        return fail("URL 缺少主机名");
    }
    if (authority.find('@') != std::string_view::npos) {
        return fail("不支持 userinfo");
    }

    std::string host;
    std::string port;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos || close == 1) {
            return fail("IPv6 地址缺少右方括号");
        }
        host = std::string(authority.substr(1, close - 1));
        if (host.find(':') == std::string::npos) {
            return fail("方括号内不是 IPv6 地址");
        }
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return fail("IPv6 地址后的字符非法");
            }
            port = std::string(authority.substr(close + 2));
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            host = std::string(authority.substr(0, colon));
            port = std::string(authority.substr(colon + 1));
        } else {
            host = std::string(authority);
        }
        if (host.empty()) {
            return fail("URL 缺少主机名");
        }
        if (host.find(':') != std::string::npos) {
            return fail("IPv6 地址必须使用方括号");
        }
    }
    if (host.empty()) {
        return fail("URL 缺少主机名");
    }
    if (port.empty()) {
        port = https ? "443" : "80";
    }
    if (!IsValidPort(port)) {
        return fail("端口非法");
    }

    std::string path;
    if (tail.empty()) {
        path = "/";
    } else if (tail.front() == '?') {
        path = "/" + std::string(tail);
    } else {
        path = std::string(tail);
    }

    out->https = https;
    out->host = std::move(host);
    out->port = std::move(port);
    out->path = std::move(path);
    return true;
}

/**
 * @brief 规范化 URL path 中的 `.`/`..` 段。
 * @details 只处理路径本身，`..` 不会越过根；查询串原样保留。
 */
std::string NormalizeUrlPath(const std::string& path) {
    std::string raw = path;
    std::string query;
    if (const std::size_t mark = raw.find('?'); mark != std::string::npos) {
        query = raw.substr(mark);
        raw.resize(mark);
    }
    std::vector<std::string> segments;
    std::size_t cursor = 0;
    while (cursor <= raw.size()) {
        const std::size_t slash = raw.find('/', cursor);
        const std::size_t end = slash == std::string::npos ? raw.size() : slash;
        std::string segment = raw.substr(cursor, end - cursor);
        if (segment == "..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
        } else if (!segment.empty() && segment != ".") {
            segments.push_back(std::move(segment));
        }
        if (slash == std::string::npos) {
            break;
        }
        cursor = slash + 1;
    }
    std::string normalized = "/";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) {
            normalized.push_back('/');
        }
        normalized += segments[i];
    }
    normalized += query;
    return normalized;
}

/**
 * @brief 把 Location 解析成新的绝对 URL。
 * @return 失败时填 `error`（空 Location、非法 URL 等）。
 */
bool ResolveRedirect(const UrlParts& base, std::string_view location, UrlParts* out,
                     std::string* error) {
    std::string target = TrimAscii(location);
    if (target.empty()) {
        if (error != nullptr) {
            *error = "重定向缺少 Location";
        }
        return false;
    }
    if (const std::size_t fragment = target.find('#'); fragment != std::string::npos) {
        target.resize(fragment);
    }
    std::string absolute;
    if (StartsWithIgnoreCase(target, "http://") || StartsWithIgnoreCase(target, "https://")) {
        absolute = target;
    } else if (target.rfind("//", 0) == 0) {
        absolute = std::string(base.https ? "https:" : "http:") + target;
    } else if (target.front() == '/') {
        absolute = Origin(base) + target;
    } else if (target.front() == '?') {
        std::string path = base.path;
        if (const std::size_t mark = path.find('?'); mark != std::string::npos) {
            path.resize(mark);
        }
        absolute = Origin(base) + path + target;
    } else {
        std::string path = base.path;
        if (const std::size_t mark = path.find('?'); mark != std::string::npos) {
            path.resize(mark);
        }
        const std::size_t slash = path.rfind('/');
        path = (slash == std::string::npos) ? std::string("/") : path.substr(0, slash + 1);
        absolute = Origin(base) + path + target;
    }
    if (!ParseUrl(absolute, out, error)) {
        return false;
    }
    out->path = NormalizeUrlPath(out->path);
    return true;
}

/// 百分号解码；遇到非法 `%xx` 返回 false。
std::string PercentDecode(std::string_view text, bool* ok) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }
        if (i + 2 >= text.size()) {
            *ok = false;
            return {};
        }
        const int high = HexValue(text[i + 1]);
        const int low = HexValue(text[i + 2]);
        if (high < 0 || low < 0) {
            *ok = false;
            return {};
        }
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return out;
}

/// 从 URL 推导单段文件名；失败时 `error` 非空。
std::string FilenameFromUrl(const UrlParts& url, std::string* error) {
    std::string path = url.path;
    if (const std::size_t mark = path.find('?'); mark != std::string::npos) {
        path.resize(mark);
    }
    const std::size_t slash = path.rfind('/');
    std::string last = slash == std::string::npos ? path : path.substr(slash + 1);
    bool ok = true;
    last = PercentDecode(last, &ok);
    if (!ok) {
        *error = "URL 文件名包含非法百分号编码";
        return {};
    }
    if (last.empty()) {
        last = "index.html";
    } else if (last == "." || last == "..") {
        *error = "URL 文件名非法";
        return {};
    }
    return last;
}

// ---------------------------------------------------------------------------
// 路径沙箱
// ---------------------------------------------------------------------------

/// 目标文件名是否为单段、无危险字符（`ResolveFetchPath` 的公共校验）。
bool IsSafeSingleSegment(const std::string& name, std::size_t max_bytes) {
    if (name.empty() || name.size() > max_bytes || name.size() > kFetchMaxFilenameBytes) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    for (char ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || IsControlByte(ch)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 网络 IO
// ---------------------------------------------------------------------------

/// 关闭 fd（忽略 EINTR）。
void CloseFd(int* fd) noexcept {
    if (fd != nullptr && *fd >= 0) {
        int ignored = 0;
        while (::close(*fd) != 0 && errno == EINTR) {
            ignored = 1;
        }
        (void)ignored;
        *fd = -1;
    }
}

/**
 * @brief 建立 TCP 连接（非阻塞 connect + poll，支持 IPv4/IPv6）。
 * @param deadline 整次尝试的总截止时间。
 * @param connect_timeout_ms 单次连接超时。
 */
FetchStatus ConnectTcp(const std::string& host, const std::string& port,
                       Clock::time_point deadline, std::uint32_t connect_timeout_ms,
                       int* out_fd, std::string* message) {
    if (out_fd == nullptr) {
        return FetchStatus::kInvalidArgument;
    }
    *out_fd = -1;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const int gai = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
    if (gai != 0) {
        *message = std::string("解析主机失败: ") + ::gai_strerror(gai);
        return FetchStatus::kDnsError;
    }

    const Clock::time_point connect_deadline =
        std::min(deadline, Clock::now() + std::chrono::milliseconds(connect_timeout_ms));
    bool saw_timeout = false;
    int last_errno = 0;
    FetchStatus status = FetchStatus::kConnectFailed;

    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        if (Clock::now() >= connect_deadline || Clock::now() >= deadline) {
            saw_timeout = true;
            break;
        }
        const int fd = ::socket(entry->ai_family,
                                entry->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                entry->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        if (::connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
            *out_fd = fd;
            ::freeaddrinfo(results);
            return FetchStatus::kOk;
        }
        if (errno != EINPROGRESS) {
            last_errno = errno;
            ::close(fd);
            continue;
        }

        const int wait_ms = RemainingMs(std::min(connect_deadline, deadline));
        if (wait_ms <= 0) {
            saw_timeout = true;
            ::close(fd);
            continue;
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int poll_result = 0;
        do {
            poll_result = ::poll(&pfd, 1, wait_ms);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result == 0) {
            saw_timeout = true;
            ::close(fd);
            continue;
        }
        if (poll_result < 0) {
            last_errno = errno;
            ::close(fd);
            continue;
        }
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
            last_errno = errno;
            ::close(fd);
            continue;
        }
        if (socket_error != 0) {
            last_errno = socket_error;
            ::close(fd);
            continue;
        }
        *out_fd = fd;
        ::freeaddrinfo(results);
        return FetchStatus::kOk;
    }
    ::freeaddrinfo(results);

    if (saw_timeout) {
        *message = "连接超时";
        status = FetchStatus::kTimeout;
    } else {
        *message = std::string("连接失败") +
                   (last_errno != 0 ? std::string(": ") + std::strerror(last_errno) : "");
        status = FetchStatus::kConnectFailed;
    }
    return status;
}

/// 发送一段数据直到写完（poll POLLOUT 处理 EAGAIN）。
FetchStatus SendAll(int fd, std::string_view data, Clock::time_point deadline,
                    std::uint32_t idle_timeout_ms, std::string* message) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const Clock::time_point wait_until =
                std::min(deadline, Clock::now() + std::chrono::milliseconds(idle_timeout_ms));
            const int wait_ms = RemainingMs(wait_until);
            if (wait_ms <= 0) {
                *message = "发送超时";
                return FetchStatus::kTimeout;
            }
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int poll_result = 0;
            do {
                poll_result = ::poll(&pfd, 1, wait_ms);
            } while (poll_result < 0 && errno == EINTR);
            if (poll_result == 0) {
                *message = "发送超时";
                return FetchStatus::kTimeout;
            }
            if (poll_result > 0) {
                continue;
            }
        }
        *message = std::string("发送失败: ") + std::strerror(errno);
        return FetchStatus::kIoError;
    }
    return FetchStatus::kOk;
}

/// 带空闲超时与总截止时间的行/字节读取器。
class SocketReader final {
public:
    SocketReader(int fd, std::uint32_t idle_timeout_ms, Clock::time_point deadline)
        : m_fd(fd), m_idle_timeout_ms(idle_timeout_ms), m_deadline(deadline) {}

    /// 读一行（去掉 CRLF），`line` 不含 `\r\n`。
    FetchStatus ReadLine(std::string* line, std::string* message) {
        for (;;) {
            const std::size_t newline = m_buffer.find('\n');
            if (newline != std::string::npos) {
                line->assign(m_buffer, 0, newline);
                if (!line->empty() && line->back() == '\r') {
                    line->pop_back();
                }
                m_buffer.erase(0, newline + 1);
                return FetchStatus::kOk;
            }
            if (m_buffer.size() > kFetchMaxHeaderBytes) {
                *message = "响应头单行超限";
                return FetchStatus::kProtocolError;
            }
            if (m_eof) {
                *message = "连接在行结束前关闭";
                return FetchStatus::kProtocolError;
            }
            const FetchStatus status = Fill(message);
            if (status != FetchStatus::kOk) {
                return status;
            }
        }
    }

    /// 读一段字节；`got == 0` 表示对端 EOF。
    /// @details 大块读取（>= `kDirectReadBytes`）直接把字节 `recv` 进调用方
    ///          缓冲，省掉「内核 → 内部缓冲 → 调用方缓冲」的两次内存拷贝；
    ///          小块（头部残留/尾部零头）仍走内部缓冲，语义不变。
    FetchStatus ReadSome(char* output, std::size_t capacity, std::size_t* got,
                         std::string* message) {
        if (capacity == 0) {
            *got = 0;
            return FetchStatus::kOk;
        }
        if (!m_buffer.empty()) {
            const std::size_t take = std::min(capacity, m_buffer.size());
            std::memcpy(output, m_buffer.data(), take);
            m_buffer.erase(0, take);
            *got = take;
            return FetchStatus::kOk;
        }
        if (m_eof) {
            *got = 0;
            return FetchStatus::kOk;
        }
        if (capacity >= kDirectReadBytes) {
            return ReadDirect(output, capacity, got, message);
        }
        const FetchStatus status = Fill(message);
        if (status != FetchStatus::kOk) {
            return status;
        }
        const std::size_t take = std::min(capacity, m_buffer.size());
        if (take > 0) {
            std::memcpy(output, m_buffer.data(), take);
            m_buffer.erase(0, take);
        }
        *got = take;
        return FetchStatus::kOk;
    }

private:
    /// 等待 socket 可读；同时受单次空闲超时与本次尝试的总截止时间约束。
    FetchStatus WaitReadable(std::string* message) {
        for (;;) {
            const Clock::time_point wait_until =
                std::min(m_deadline, Clock::now() + std::chrono::milliseconds(m_idle_timeout_ms));
            const int wait_ms = RemainingMs(wait_until);
            if (wait_ms <= 0) {
                *message = "读取超时";
                return FetchStatus::kTimeout;
            }
            pollfd pfd{};
            pfd.fd = m_fd;
            pfd.events = POLLIN;
            const int poll_result = ::poll(&pfd, 1, wait_ms);
            if (poll_result == 0) {
                *message = "读取超时";
                return FetchStatus::kTimeout;
            }
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                *message = std::string("poll 失败: ") + std::strerror(errno);
                return FetchStatus::kIoError;
            }
            return FetchStatus::kOk;
        }
    }

    /// 直接把字节读进调用方缓冲（大块 body 的零拷贝路径）。
    FetchStatus ReadDirect(char* output, std::size_t capacity, std::size_t* got,
                           std::string* message) {
        for (;;) {
            const FetchStatus ready = WaitReadable(message);
            if (ready != FetchStatus::kOk) {
                return ready;
            }
            const ssize_t result = ::recv(m_fd, output, capacity, 0);
            if (result > 0) {
                *got = static_cast<std::size_t>(result);
                return FetchStatus::kOk;
            }
            if (result == 0) {
                m_eof = true;
                *got = 0;
                return FetchStatus::kOk;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            *message = std::string("读取失败: ") + std::strerror(errno);
            return FetchStatus::kIoError;
        }
    }

    /// 从 socket 读一次到内部缓冲；超时/错误写 `message`。
    FetchStatus Fill(std::string* message) {
        for (;;) {
            const FetchStatus ready = WaitReadable(message);
            if (ready != FetchStatus::kOk) {
                return ready;
            }
            const ssize_t got = ::recv(m_fd, m_fill.data(), m_fill.size(), 0);
            if (got > 0) {
                m_buffer.append(m_fill.data(), static_cast<std::size_t>(got));
                return FetchStatus::kOk;
            }
            if (got == 0) {
                m_eof = true;
                return FetchStatus::kOk;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            *message = std::string("读取失败: ") + std::strerror(errno);
            return FetchStatus::kIoError;
        }
    }

    /// 至少这么大块时才走「直接读进调用方缓冲」的快路径。
    static constexpr std::size_t kDirectReadBytes = 16 * 1024;

    int m_fd{-1};
    std::uint32_t m_idle_timeout_ms{0};
    Clock::time_point m_deadline{};
    std::string m_buffer;
    std::array<char, 64 * 1024> m_fill{};
    bool m_eof{false};
};

// ---------------------------------------------------------------------------
// HTTP 响应
// ---------------------------------------------------------------------------

/// 响应头中本模块关心的字段。
struct HttpResponseHead final {
    int status{0};
    bool has_content_length{false};
    std::uint64_t content_length{0};
    bool chunked{false};
    std::string location;
    std::string content_encoding;
};

/// 解析状态行中的三位状态码；非法返回 -1。
int ParseStatusCode(std::string_view line) noexcept {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    if (line.size() < 12 || line.rfind("HTTP/", 0) != 0) {
        return -1;
    }
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos || space + 4 > line.size()) {
        return -1;
    }
    int code = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const char ch = line[space + 1 + i];
        if (ch < '0' || ch > '9') {
            return -1;
        }
        code = code * 10 + (ch - '0');
    }
    if (space + 4 < line.size() && line[space + 4] != ' ') {
        return -1;
    }
    return code;
}

/// 十进制 u64 解析（拒绝空串、符号、溢出）。
bool ParseU64Decimal(std::string_view text, std::uint64_t* out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

/// 十六进制 u64 解析（chunk 大小，允许空串以外的数字）。
bool ParseHexU64(std::string_view text, std::uint64_t* out) noexcept {
    if (text.empty() || text.size() > 16) {
        return false;
    }
    std::uint64_t value = 0;
    for (char ch : text) {
        const int digit = HexValue(ch);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    *out = value;
    return true;
}

/// 读取并解析响应头；跳过 1xx 响应。
FetchStatus ReadHeaders(SocketReader& reader, HttpResponseHead* head, std::string* message) {
    std::size_t header_bytes = 0;
    for (;;) {
        std::string line;
        FetchStatus status = reader.ReadLine(&line, message);
        if (status != FetchStatus::kOk) {
            return status;
        }
        header_bytes += line.size() + 2;
        if (header_bytes > kFetchMaxHeaderBytes) {
            *message = "响应头超过大小上限";
            return FetchStatus::kProtocolError;
        }
        if (line.empty()) {
            continue;
        }
        const int code = ParseStatusCode(line);
        if (code < 100 || code > 599) {
            *message = "HTTP 状态行非法";
            return FetchStatus::kProtocolError;
        }

        HttpResponseHead parsed;
        parsed.status = code;
        bool content_length_seen = false;
        bool chunked_seen = false;
        for (;;) {
            std::string header;
            status = reader.ReadLine(&header, message);
            if (status != FetchStatus::kOk) {
                return status;
            }
            header_bytes += header.size() + 2;
            if (header_bytes > kFetchMaxHeaderBytes) {
                *message = "响应头超过大小上限";
                return FetchStatus::kProtocolError;
            }
            if (header.empty()) {
                break;
            }
            const std::size_t colon = header.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::string key = ToLowerAscii(TrimAscii(std::string_view(header).substr(0, colon)));
            const std::string value = TrimAscii(std::string_view(header).substr(colon + 1));
            if (key == "content-length") {
                std::uint64_t length = 0;
                if (!ParseU64Decimal(value, &length)) {
                    *message = "Content-Length 非法";
                    return FetchStatus::kProtocolError;
                }
                if (content_length_seen && length != parsed.content_length) {
                    *message = "Content-Length 重复且不一致";
                    return FetchStatus::kProtocolError;
                }
                parsed.has_content_length = true;
                parsed.content_length = length;
                content_length_seen = true;
            } else if (key == "transfer-encoding") {
                if (ToLowerAscii(value).find("chunked") != std::string::npos) {
                    chunked_seen = true;
                }
            } else if (key == "location") {
                parsed.location = value;
            } else if (key == "content-encoding") {
                parsed.content_encoding = ToLowerAscii(value);
            }
        }
        if (code >= 100 && code < 200) {
            continue;
        }
        if (chunked_seen) {
            parsed.chunked = true;
            parsed.has_content_length = false;
        }
        if (!parsed.content_encoding.empty() && parsed.content_encoding != "identity") {
            *message = "响应使用了不支持的 Content-Encoding: " + parsed.content_encoding;
            return FetchStatus::kProtocolError;
        }
        *head = std::move(parsed);
        return FetchStatus::kOk;
    }
}

/// 构造请求（HTTP/1.1、Connection: close、不发 Accept-Encoding）。
std::string BuildRequest(const UrlParts& url) {
    std::string request;
    request.reserve(256);
    request += "GET " + url.path + " HTTP/1.1\r\n";
    request += "Host: " + HostHeader(url) + "\r\n";
    request += "User-Agent: go2cpp-fetch/1.0\r\n";
    request += "Accept: */*\r\n";
    request += "Connection: close\r\n";
    request += "\r\n";
    return request;
}

// ---------------------------------------------------------------------------
// 响应体落盘
// ---------------------------------------------------------------------------

/// 逐段写临时文件并同步更新 SHA-256。
class BodySink final {
public:
    BodySink(int fd, std::uint64_t max_bytes) : m_fd(fd), m_max_bytes(max_bytes) {}

    /// 追加一段 body；失败时把 errno 翻译成可读诊断（`message` 可为空）。
    FetchStatus Append(const char* data, std::size_t size, std::string* message) {
        if (size > m_max_bytes - m_bytes) {
            return FetchStatus::kTooLarge;
        }
        std::size_t written = 0;
        while (written < size) {
            const ssize_t result = ::write(m_fd, data + written, size - written);
            if (result > 0) {
                written += static_cast<std::size_t>(result);
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (message != nullptr) {
                *message = std::string("写入临时文件失败: ") + std::strerror(errno);
            }
            return FetchStatus::kIoError;
        }
        m_hasher.Write(std::string_view(data, size));
        m_bytes += static_cast<std::uint64_t>(size);
        return FetchStatus::kOk;
    }

    std::uint64_t bytes() const noexcept { return m_bytes; }

    std::string Hex() const {
        const std::vector<std::uint8_t> digest = m_hasher.Sum();
        if (digest.size() < 32) {
            return {};
        }
        return DigestToHex(digest.data());
    }

private:
    int m_fd{-1};
    std::uint64_t m_max_bytes{0};
    std::uint64_t m_bytes{0};
    go2cpp::hash::Sha256 m_hasher;
};

/// 精确读取 `count` 字节写入 sink。
FetchStatus ReadExact(SocketReader& reader, BodySink& sink, std::uint64_t count,
                      std::string* message) {
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t remaining = count;
    while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
        std::size_t got = 0;
        const FetchStatus status = reader.ReadSome(buffer.data(), want, &got, message);
        if (status != FetchStatus::kOk) {
            return status;
        }
        if (got == 0) {
            *message = "响应体提前结束";
            return FetchStatus::kTruncated;
        }
        const FetchStatus append = sink.Append(buffer.data(), got, message);
        if (append != FetchStatus::kOk) {
            return append;
        }
        remaining -= static_cast<std::uint64_t>(got);
    }
    return FetchStatus::kOk;
}

/// 读到 EOF（无 Content-Length 且非 chunked 的响应）。
FetchStatus ReadUntilEof(SocketReader& reader, BodySink& sink, std::string* message) {
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        std::size_t got = 0;
        const FetchStatus status = reader.ReadSome(buffer.data(), buffer.size(), &got, message);
        if (status != FetchStatus::kOk) {
            return status;
        }
        if (got == 0) {
            return FetchStatus::kOk;
        }
        const FetchStatus append = sink.Append(buffer.data(), got, message);
        if (append != FetchStatus::kOk) {
            return append;
        }
    }
}

/// chunked 传输解码。
FetchStatus ReadChunked(SocketReader& reader, BodySink& sink, std::string* message) {
    for (;;) {
        std::string line;
        FetchStatus status = reader.ReadLine(&line, message);
        if (status != FetchStatus::kOk) {
            return status;
        }
        if (const std::size_t extension = line.find(';'); extension != std::string::npos) {
            line.resize(extension);
        }
        line = TrimAscii(line);
        std::uint64_t chunk_size = 0;
        if (!ParseHexU64(line, &chunk_size)) {
            *message = "chunk 大小非法";
            return FetchStatus::kProtocolError;
        }
        if (chunk_size == 0) {
            for (;;) {
                std::string trailer;
                status = reader.ReadLine(&trailer, message);
                if (status != FetchStatus::kOk) {
                    return status;
                }
                if (trailer.empty()) {
                    return FetchStatus::kOk;
                }
            }
        }
        status = ReadExact(reader, sink, chunk_size, message);
        if (status != FetchStatus::kOk) {
            return status;
        }
        std::string terminator;
        status = reader.ReadLine(&terminator, message);
        if (status != FetchStatus::kOk) {
            return status;
        }
        if (!terminator.empty()) {
            *message = "chunk 数据后缺少 CRLF";
            return FetchStatus::kProtocolError;
        }
    }
}

// ---------------------------------------------------------------------------
// 临时文件
// ---------------------------------------------------------------------------

/// 以 O_EXCL|O_NOFOLLOW 创建临时文件。
FetchStatus OpenTempFile(const std::string& path, int* fd, std::string* message) {
    const int opened =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (opened < 0) {
        *message = std::string("创建临时文件失败: ") + std::strerror(errno);
        return FetchStatus::kIoError;
    }
    *fd = opened;
    return FetchStatus::kOk;
}

/// 生成与目标同目录的临时文件名（同进程内单调递增）。
std::string MakeTempPath(const std::string& target) {
    const std::uint64_t sequence = g_temp_seq.fetch_add(1, std::memory_order_relaxed);
    return target + ".go2cpp-part." + std::to_string(static_cast<long long>(::getpid())) + "." +
           std::to_string(static_cast<unsigned long long>(sequence));
}

// ---------------------------------------------------------------------------
// HTTP 下载
// ---------------------------------------------------------------------------

// 前置声明：http 重定向到 https 时需要切换到 curl 路径。
FetchStatus PerformHttpsAttempt(const UrlParts& url, const FetchOptions& options,
                                const std::string& temp_path, FetchResult* result);

/**
 * @brief 内置 HTTP 客户端的一次完整下载（含重定向）。
 * @param fd_io 已打开的临时文件 fd；若重定向到 https 会关闭它并置 -1，
 *              由 curl 路径继续。
 */
FetchStatus HttpGetToFile(const UrlParts& start, const FetchOptions& options,
                          const std::string& temp_path, int* fd_io, FetchResult* result) {
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(options.total_timeout_ms);
    UrlParts current = start;
    std::size_t redirects = 0;

    for (;;) {
        int socket_fd = -1;
        std::string message;
        FetchStatus status = ConnectTcp(current.host, current.port, deadline,
                                        options.connect_timeout_ms, &socket_fd, &message);
        if (status != FetchStatus::kOk) {
            result->message = message;
            return status;
        }
        status = SendAll(socket_fd, BuildRequest(current), deadline, options.read_timeout_ms,
                         &message);
        if (status != FetchStatus::kOk) {
            CloseFd(&socket_fd);
            result->message = message;
            return status;
        }

        SocketReader reader(socket_fd, options.read_timeout_ms, deadline);
        HttpResponseHead head;
        status = ReadHeaders(reader, &head, &message);
        if (status != FetchStatus::kOk) {
            CloseFd(&socket_fd);
            result->message = message;
            return status;
        }
        result->http_status = head.status;

        // 204/205/304 按 RFC 语义**没有响应体**：既不能按 Content-Length 读，
        // 也不能读到 EOF（keep-alive 连接不会半关闭，会一直等到读超时）。
        // 直接按「成功且 0 字节」完成（FETCH-02）。
        if (head.status == 204 || head.status == 205 || head.status == 304) {
            CloseFd(&socket_fd);
            BodySink empty_sink(*fd_io, options.max_bytes);
            result->bytes = 0;
            result->sha256 = empty_sink.Hex();
            if (result->sha256.empty()) {
                result->message = "计算 SHA-256 失败";
                return FetchStatus::kIoError;
            }
            return FetchStatus::kOk;
        }

        const bool is_redirect =
            head.status == 301 || head.status == 302 || head.status == 303 ||
            head.status == 307 || head.status == 308;
        if (is_redirect) {
            CloseFd(&socket_fd);
            if (!options.allow_redirects) {
                result->message = "服务器返回重定向，但调用方禁止跟随";
                return FetchStatus::kHttpError;
            }
            if (redirects >= options.max_redirects) {
                result->message = "重定向次数超过上限";
                return FetchStatus::kHttpError;
            }
            UrlParts next;
            if (!ResolveRedirect(current, head.location, &next, &message)) {
                // 3xx 缺少/带非法 Location 属于 HTTP 响应问题：按头文件
                // 契约归 kHttpError（而不是 kProtocolError），便于调用方
                // 统一按「服务器返回了非预期响应」处理。
                result->message = message;
                return FetchStatus::kHttpError;
            }
            ++redirects;
            if (next.https) {
                // TLS 交给 curl：先撤销临时文件，让 curl 以同样路径重新创建。
                CloseFd(fd_io);
                ::unlink(temp_path.c_str());
                return PerformHttpsAttempt(next, options, temp_path, result);
            }
            current = std::move(next);
            continue;
        }
        if (head.status < 200 || head.status >= 300) {
            CloseFd(&socket_fd);
            result->message = "HTTP 状态码 " + std::to_string(head.status);
            return FetchStatus::kHttpError;
        }

        if (head.has_content_length && head.content_length > options.max_bytes) {
            CloseFd(&socket_fd);
            result->message = "响应体超过大小上限";
            return FetchStatus::kTooLarge;
        }

        BodySink sink(*fd_io, options.max_bytes);
        if (head.chunked) {
            status = ReadChunked(reader, sink, &message);
        } else if (head.has_content_length) {
            status = ReadExact(reader, sink, head.content_length, &message);
        } else {
            status = ReadUntilEof(reader, sink, &message);
        }
        CloseFd(&socket_fd);
        if (status != FetchStatus::kOk) {
            result->message = message;
            return status;
        }
        result->bytes = sink.bytes();
        result->sha256 = sink.Hex();
        if (result->sha256.empty()) {
            result->message = "计算 SHA-256 失败";
            return FetchStatus::kIoError;
        }
        return FetchStatus::kOk;
    }
}

// ---------------------------------------------------------------------------
// curl（HTTPS）
// ---------------------------------------------------------------------------

/// 从 curl 的 `--write-out` 输出尾部解析 HTTP 状态码。
int ParseTrailingHttpCode(std::string_view output) {
    std::size_t end = output.size();
    while (end > 0 && (output[end - 1] == ' ' || output[end - 1] == '\n' ||
                       output[end - 1] == '\r' || output[end - 1] == '\t')) {
        --end;
    }
    std::size_t begin = end;
    while (begin > 0 && output[begin - 1] >= '0' && output[begin - 1] <= '9') {
        --begin;
    }
    if (begin == end || end - begin > 3) {
        return 0;
    }
    int code = 0;
    for (std::size_t i = begin; i < end; ++i) {
        code = code * 10 + (output[i] - '0');
    }
    return code >= 100 && code <= 599 ? code : 0;
}

/// 转义 curl 配置文件里的双引号字符串（`\\` 与 `"` 需要反斜杠转义）。
bool EscapeCurlConfigValue(std::string_view value, std::string* out) {
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte == 0 || byte == '\r' || byte == '\n') {
            return false;
        }
    }
    out->clear();
    out->reserve(value.size() + 2);
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') {
            out->push_back('\\');
        }
        out->push_back(ch);
    }
    return true;
}

/// 构造 `curl --config -` 的内容。
/// @details URL 里可能带预签名 token，输出路径也不该出现在进程参数里：
///          二者都写进经 stdin 传入的配置，与 backup 模块的做法一致，
///          同机用户无法从 /proc/<pid>/cmdline 读到凭证（D-04）。
bool BuildHttpsCurlConfig(const std::string& url_text, const std::string& output_path,
                          const FetchOptions& options, std::string* config,
                          std::string* error) {
    std::string url;
    std::string output;
    if (!EscapeCurlConfigValue(url_text, &url) ||
        !EscapeCurlConfigValue(output_path, &output)) {
        *error = "URL/输出路径含换行或 NUL";
        return false;
    }
    std::string text;
    text += "url = \"" + url + "\"\n";
    text += "output = \"" + output + "\"\n";
    // globoff：URL 里的 [] 等字符按字面处理，不做 curl 通配展开。
    text += "globoff\n";
    text += "silent\n";
    text += "show-error\n";
    text += "fail\n";
    if (options.allow_redirects) {
        text += "location\n";
        text += "max-redirs = \"" + std::to_string(options.max_redirects) + "\"\n";
    }
    // 只允许 https，且重定向后仍必须是 https（安全默认不变）。
    text += "proto = \"=https\"\n";
    text += "proto-redir = \"=https\"\n";
    text += "connect-timeout = \"" + FormatSeconds(options.connect_timeout_ms) + "\"\n";
    text += "max-time = \"" + FormatSeconds(options.total_timeout_ms) + "\"\n";
    text += "max-filesize = \"" + std::to_string(options.max_bytes) + "\"\n";
    // 空闲读超时：curl 没有独立的 read timeout，用 speed-limit/speed-time 近似
    // —— speed-time 秒窗口内平均速度低于 1 B/s 即中止（退出码 28，映射为 kTimeout）。
    // 正常下载速度远高于 1 B/s，只有真正卡死时才会触发。
    const std::uint32_t speed_time_seconds =
        std::max<std::uint32_t>(1, (options.read_timeout_ms + 999) / 1000);
    text += "speed-limit = \"1\"\n";
    text += "speed-time = \"" + std::to_string(speed_time_seconds) + "\"\n";
    text += "write-out = \"%{http_code}\"\n";
    *config = std::move(text);
    return true;
}

/**
 * @brief curl 输出用的私有临时目录（`mkdtemp`，权限 0700）。
 *
 * @details HTTPS 路径无法像 HTTP 那样把已打开的 fd 直接交给子进程，因此改让
 *          curl 写进一个**只有本进程可写**的新目录，再由本进程用
 *          `O_NOFOLLOW` 打开目录内的 payload、校验类型/大小/摘要后 rename 到
 *          最终临时路径。目录是新建的 0700 目录，攻击者无法提前在里面预置
 *          符号链接，所以 HTTPS 与 HTTP 具有同等的符号链接防护。
 *          析构负责清理目录内容（幂等）。
 */
class TempOutputDir final {
   public:
    TempOutputDir() = default;
    ~TempOutputDir() { Remove(); }

    TempOutputDir(const TempOutputDir&) = delete;
    TempOutputDir& operator=(const TempOutputDir&) = delete;

    /// @brief 在 `target` 同目录下创建唯一临时目录；失败返回 false 并填 message。
    bool Create(const std::string& target, std::string* message) {
        const std::uint64_t sequence = g_temp_seq.fetch_add(1, std::memory_order_relaxed);
        std::string pattern = target + ".go2cpp-part." +
                              std::to_string(static_cast<long long>(::getpid())) + "." +
                              std::to_string(static_cast<unsigned long long>(sequence)) +
                              ".XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* created = ::mkdtemp(buffer.data());
        if (created == nullptr) {
            *message = std::string("创建临时目录失败: ") + std::strerror(errno);
            return false;
        }
        m_dir = created;
        return true;
    }

    /// curl 的输出路径（目录内固定单段名，不会与目标同 URL 段冲突）。
    std::string PayloadPath() const { return m_dir + "/payload"; }

    /// 删除目录内容并移除目录；重复调用无副作用。
    void Remove() noexcept {
        if (m_dir.empty()) {
            return;
        }
        ::unlink((m_dir + "/payload").c_str());
        ::rmdir(m_dir.c_str());
        m_dir.clear();
    }

   private:
    std::string m_dir;  ///< 临时目录路径；空表示未创建。
};

/// 校验 curl 输出并把它变成最终临时文件（外层 Fetch 再做目标 rename）。
FetchStatus PromoteHttpsOutput(const TempOutputDir& output, const std::string& temp_path,
                               const FetchOptions& options, FetchResult* result) {
    const std::string payload = output.PayloadPath();
    const int fd = ::open(payload.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        result->message = "curl 未生成可用的目标文件（拒绝符号链接或打开失败）";
        return FetchStatus::kIoError;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        result->message = "curl 输出不是普通文件";
        return FetchStatus::kIoError;
    }
    if (static_cast<std::uint64_t>(info.st_size) > options.max_bytes) {
        ::close(fd);
        result->message = "响应体超过大小上限";
        return FetchStatus::kTooLarge;
    }
    std::string hex;
    std::uint64_t bytes = 0;
    const bool hashed = HashFd(fd, &hex, &bytes);
    ::close(fd);
    if (!hashed) {
        result->message = "读取 curl 下载结果失败";
        return FetchStatus::kIoError;
    }
    if (bytes != static_cast<std::uint64_t>(info.st_size)) {
        result->message = "下载过程中文件大小发生变化";
        return FetchStatus::kIoError;
    }
    // rename 不跟随符号链接；配合上面的 O_NOFOLLOW，写入路径的链接攻击失效。
    if (::rename(payload.c_str(), temp_path.c_str()) != 0) {
        result->message = std::string("临时文件 rename 失败: ") + std::strerror(errno);
        return FetchStatus::kIoError;
    }
    result->bytes = bytes;
    result->sha256 = std::move(hex);
    return FetchStatus::kOk;
}

/// HTTPS 分支：调用系统 curl，外层仍负责重试、摘要与原子落盘。
FetchStatus PerformHttpsAttempt(const UrlParts& url, const FetchOptions& options,
                                const std::string& temp_path, FetchResult* result) {
    // 先建私有目录：curl 只能写目录内的 payload，预置符号链接无法劫持写入。
    TempOutputDir output;
    std::string message;
    if (!output.Create(temp_path, &message)) {
        result->message = message;
        return FetchStatus::kIoError;
    }
    std::string config;
    if (!BuildHttpsCurlConfig(BuildUrlText(url), output.PayloadPath(), options, &config,
                              &message)) {
        result->message = message;
        return FetchStatus::kInvalidArgument;
    }

    go2cpp::os::ProcessOptions process;
    process.argv = {"curl", "--config", "-"};
    process.use_path_search = true;
    process.timeout = std::chrono::milliseconds(
        static_cast<std::int64_t>(options.total_timeout_ms) + 1000);
    process.grace = std::chrono::milliseconds(500);
    process.max_output_bytes = 8 * 1024;

    const auto run = go2cpp::os::Run(process, config);
    if (!run.ok()) {
        const auto code = run.error().code;
        if (code == go2cpp::os::ProcErrorCode::kSpawnFailed ||
            code == go2cpp::os::ProcErrorCode::kExecFailed) {
            result->message = "https 需要系统 curl，但启动失败";
            return FetchStatus::kBackendUnavailable;
        }
        result->message = run.error().ToString();
        return FetchStatus::kIoError;
    }
    const go2cpp::os::RunResult& completed = run.value();
    result->http_status = ParseTrailingHttpCode(completed.standard_output);
    if (completed.status.timed_out) {
        result->message = "curl 总超时";
        return FetchStatus::kTimeout;
    }
    if (completed.status.signaled) {
        result->message = "curl 被信号终止";
        return FetchStatus::kIoError;
    }
    if (completed.status.exit_code != 0) {
        const std::string detail = TrimAscii(completed.standard_error);
        result->message = detail.empty()
                              ? "curl 退出码 " + std::to_string(completed.status.exit_code)
                              : detail;
        switch (completed.status.exit_code) {
            case 6:
                return FetchStatus::kDnsError;
            case 7:
                return FetchStatus::kConnectFailed;
            case 28:
                return FetchStatus::kTimeout;
            case 22:
                return FetchStatus::kHttpError;
            case 47:
                // --max-redirs 用尽：与内置 HTTP 客户端「重定向次数超过上限」同分类。
                return FetchStatus::kHttpError;
            case 63:
                return FetchStatus::kTooLarge;
            case 35:
            case 51:
            case 58:
            case 59:
            case 60:
            case 77:
            case 83:
            case 90:
            case 91:
                return FetchStatus::kTlsError;
            default:
                return FetchStatus::kConnectFailed;
        }
    }

    // curl 的 --fail 只对 >=400 生效：3xx（未跟随/缺 Location）与其它非 2xx
    // 仍会以 0 退出并留下空文件。这里按与内置 HTTP 客户端相同的契约判定；
    // 204/205/304 按「成功且无响应体」处理（与 HTTP 路径一致）。
    if (result->http_status != 0 && (result->http_status < 200 || result->http_status >= 300) &&
        result->http_status != 204 && result->http_status != 205 &&
        result->http_status != 304) {
        result->message = "HTTP 状态码 " + std::to_string(result->http_status);
        return FetchStatus::kHttpError;
    }
    if (result->http_status == 204 || result->http_status == 205 || result->http_status == 304) {
        // 无响应体：curl 不保证为 0 字节响应创建输出文件，这里补一个空文件，
        // 让 HTTPS 的 204/205/304 与 HTTP 路径一致地以「成功且 0 字节」结束。
        const std::string payload = output.PayloadPath();
        const int empty_fd =
            ::open(payload.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (empty_fd >= 0) {
            ::close(empty_fd);
        } else if (errno != EEXIST) {
            result->message = std::string("创建空输出文件失败: ") + std::strerror(errno);
            return FetchStatus::kIoError;
        }
    }
    return PromoteHttpsOutput(output, temp_path, options, result);
}

// ---------------------------------------------------------------------------
// 排他落盘
// ---------------------------------------------------------------------------

/**
 * @brief `overwrite=false` 时的排他落盘。
 * @details 走 `renameat2(RENAME_NOREPLACE)`：并发写同一目标时只有第一个 rename
 *          成功，其余得到 `EEXIST`，转成 `kTargetExists`，不会互相覆盖。
 *          `Fetch` 开头的「目标是否存在」检查只是快速拒绝，真正的排他性由这里
 *          保证（否则检查与 rename 之间的 TOCTOU 窗口会让多个请求都落盘）。
 * @return true 表示已落盘；false 表示未落盘——此时若 `result->status` 已被置为
 *         `kTargetExists` / `kIoError` 属于确定性失败，否则表示内核或文件系统
 *         不支持该标志，调用方应回退到普通 `rename`。
 */
bool CommitNoReplace(const std::string& temp_path, const std::string& target,
                     FetchResult* result) {
#if defined(__linux__) && defined(RENAME_NOREPLACE)
    if (::renameat2(AT_FDCWD, temp_path.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) ==
        0) {
        return true;
    }
    const int rename_errno = errno;
    if (rename_errno == EEXIST) {
        result->status = FetchStatus::kTargetExists;
        result->message = "目标已存在且未允许覆盖（并发落盘时后到者被拒绝）";
        return false;
    }
    if (rename_errno != ENOSYS && rename_errno != EINVAL && rename_errno != EOPNOTSUPP) {
        result->status = FetchStatus::kIoError;
        result->message = std::string("落盘 rename 失败: ") + std::strerror(rename_errno);
        return false;
    }
#else
    (void)temp_path;
    (void)target;
    (void)result;
#endif
    return false;
}

// ---------------------------------------------------------------------------
// 重试
// ---------------------------------------------------------------------------

/// 是否可重试（只看分类与 HTTP 状态）。
bool IsRetryable(FetchStatus status, int http_status) noexcept {
    switch (status) {
        case FetchStatus::kDnsError:
        case FetchStatus::kConnectFailed:
        case FetchStatus::kTimeout:
        case FetchStatus::kTruncated:
        case FetchStatus::kChecksumMismatch:
        case FetchStatus::kTlsError:
            return true;
        case FetchStatus::kHttpError:
            return http_status == 408 || http_status == 425 || http_status == 429 ||
                   http_status >= 500;
        default:
            return false;
    }
}


/// 一次尝试：按 scheme 选择实现，失败时清理临时文件。
FetchStatus PerformAttempt(const UrlParts& url, const FetchOptions& options,
                           const std::string& temp_path, FetchResult* result) {
    result->http_status = 0;
    result->bytes = 0;
    result->sha256.clear();
    result->message.clear();
    if (url.https) {
        const FetchStatus status = PerformHttpsAttempt(url, options, temp_path, result);
        if (status != FetchStatus::kOk) {
            ::unlink(temp_path.c_str());
        }
        return status;
    }
    int fd = -1;
    std::string message;
    const FetchStatus created = OpenTempFile(temp_path, &fd, &message);
    if (created != FetchStatus::kOk) {
        result->message = message;
        return created;
    }
    FetchStatus status = HttpGetToFile(url, options, temp_path, &fd, result);
    if (fd >= 0) {
        CloseFd(&fd);
    }
    if (status != FetchStatus::kOk) {
        ::unlink(temp_path.c_str());
    }
    return status;
}

}  // namespace

// ---------------------------------------------------------------------------
// 公共 API
// ---------------------------------------------------------------------------

const char* FetchStatusToString(FetchStatus status) noexcept {
    switch (status) {
        case FetchStatus::kOk:
            return "ok";
        case FetchStatus::kInvalidArgument:
            return "invalid_argument";
        case FetchStatus::kUnsupportedScheme:
            return "unsupported_scheme";
        case FetchStatus::kPathRejected:
            return "path_rejected";
        case FetchStatus::kTargetExists:
            return "target_exists";
        case FetchStatus::kDnsError:
            return "dns_error";
        case FetchStatus::kConnectFailed:
            return "connect_failed";
        case FetchStatus::kTimeout:
            return "timeout";
        case FetchStatus::kHttpError:
            return "http_error";
        case FetchStatus::kTooLarge:
            return "too_large";
        case FetchStatus::kIoError:
            return "io_error";
        case FetchStatus::kChecksumMismatch:
            return "checksum_mismatch";
        case FetchStatus::kTruncated:
            return "truncated";
        case FetchStatus::kProtocolError:
            return "protocol_error";
        case FetchStatus::kRetryExhausted:
            return "retry_exhausted";
        case FetchStatus::kTlsError:
            return "tls_error";
        case FetchStatus::kBackendUnavailable:
            return "backend_unavailable";
    }
    return "unknown";
}

FetchStatus ResolveFetchPath(const std::string& dest_dir, const std::string& filename,
                             std::size_t max_path_bytes, std::string* resolved) {
    if (resolved == nullptr) {
        return FetchStatus::kInvalidArgument;
    }
    resolved->clear();
    if (dest_dir.empty() || max_path_bytes == 0 || max_path_bytes > kFetchMaxFilenameBytes) {
        return FetchStatus::kInvalidArgument;
    }
    if (!IsSafeSingleSegment(filename, max_path_bytes)) {
        return FetchStatus::kPathRejected;
    }
    std::error_code ec;
    const std::filesystem::path root(dest_dir);
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return FetchStatus::kPathRejected;
    }
    const std::filesystem::path root_canon = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return FetchStatus::kPathRejected;
    }
    const std::filesystem::path candidate = root_canon / filename;
    const std::filesystem::path candidate_canon = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        return FetchStatus::kPathRejected;
    }
    const std::filesystem::path relative = candidate_canon.lexically_relative(root_canon);
    if (relative.empty()) {
        return FetchStatus::kPathRejected;
    }
    const auto first = relative.begin();
    if (first == relative.end() || first->string() == "..") {
        return FetchStatus::kPathRejected;
    }
    if (std::distance(relative.begin(), relative.end()) != 1) {
        return FetchStatus::kPathRejected;
    }
    *resolved = candidate_canon.string();
    return FetchStatus::kOk;
}

bool ParseSha256Hex(std::string_view text, std::string* canonical_lower_hex) {
    if (canonical_lower_hex == nullptr) {
        return false;
    }
    const std::string trimmed = TrimAscii(text);
    if (trimmed.size() != 64) {
        return false;
    }
    std::string normalized(64, '0');
    for (std::size_t i = 0; i < 64; ++i) {
        const int value = HexValue(trimmed[i]);
        if (value < 0) {
            return false;
        }
        normalized[i] = "0123456789abcdef"[value];
    }
    *canonical_lower_hex = std::move(normalized);
    return true;
}

FetchResult Fetch(std::string_view url, const std::string& dest_dir,
                  const FetchOptions& options) {
    FetchResult result;

    const auto fail = [&result](FetchStatus status, std::string message) {
        result.status = status;
        result.message = std::move(message);
        return result;
    };

    // ---- 参数校验 ----
    if (url.empty() || url.size() > kFetchMaxUrlBytes) {
        return fail(FetchStatus::kInvalidArgument, "URL 为空或超过长度上限");
    }
    if (dest_dir.empty()) {
        return fail(FetchStatus::kInvalidArgument, "目标目录为空");
    }
    if (options.max_bytes == 0 || options.max_bytes > kFetchMaxBytesCeiling) {
        return fail(FetchStatus::kInvalidArgument, "max_bytes 越界");
    }
    if (options.connect_timeout_ms == 0 || options.connect_timeout_ms > kFetchMaxTimeoutMs ||
        options.read_timeout_ms == 0 || options.read_timeout_ms > kFetchMaxTimeoutMs ||
        options.total_timeout_ms == 0 || options.total_timeout_ms > kFetchMaxTimeoutMs) {
        return fail(FetchStatus::kInvalidArgument, "超时参数越界");
    }
    if (options.max_retries < 0 || options.max_retries > kFetchMaxRetries) {
        return fail(FetchStatus::kInvalidArgument, "max_retries 越界");
    }
    if (options.retry_backoff_ms > kFetchMaxBackoffMs) {
        return fail(FetchStatus::kInvalidArgument, "retry_backoff_ms 越界");
    }
    if (options.max_redirects > kFetchMaxRedirects) {
        return fail(FetchStatus::kInvalidArgument, "max_redirects 越界");
    }
    if (!StartsWithIgnoreCase(url, "http://") && !StartsWithIgnoreCase(url, "https://")) {
        return fail(FetchStatus::kUnsupportedScheme, "仅支持 http/https");
    }

    UrlParts parsed;
    std::string parse_error;
    if (!ParseUrl(url, &parsed, &parse_error)) {
        return fail(FetchStatus::kInvalidArgument, parse_error);
    }

    std::string expected_sha256;
    if (!options.expected_sha256.empty() &&
        !ParseSha256Hex(options.expected_sha256, &expected_sha256)) {
        return fail(FetchStatus::kInvalidArgument, "expected_sha256 必须是 64 位十六进制");
    }

    std::string filename = options.filename;
    if (filename.empty()) {
        std::string filename_error;
        filename = FilenameFromUrl(parsed, &filename_error);
        if (filename.empty()) {
            return fail(FetchStatus::kPathRejected, filename_error);
        }
    }
    std::string target;
    const FetchStatus resolved =
        ResolveFetchPath(dest_dir, filename, kFetchMaxFilenameBytes, &target);
    if (resolved != FetchStatus::kOk) {
        return fail(resolved, resolved == FetchStatus::kPathRejected
                                  ? "目标文件名未通过沙箱校验"
                                  : "目标目录/文件名参数非法");
    }

    std::error_code ec;
    const bool target_exists = std::filesystem::exists(target, ec);
    if (ec) {
        return fail(FetchStatus::kIoError, "检查目标文件状态失败");
    }
    if (target_exists) {
        if (std::filesystem::is_directory(target, ec) || ec) {
            return fail(FetchStatus::kTargetExists, "目标是一个已存在的目录");
        }
        if (!options.overwrite) {
            return fail(FetchStatus::kTargetExists, "目标已存在且未允许覆盖");
        }
    }

    const std::string temp_path = MakeTempPath(target);
    const int max_attempts = options.max_retries + 1;
    FetchStatus last_status = FetchStatus::kIoError;

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        result.attempts = attempt;
        ::unlink(temp_path.c_str());
        last_status = PerformAttempt(parsed, options, temp_path, &result);
        if (last_status == FetchStatus::kOk && !expected_sha256.empty() &&
            result.sha256 != expected_sha256) {
            last_status = FetchStatus::kChecksumMismatch;
            result.message = "SHA-256 不匹配（期望 " + expected_sha256 + "，实际 " +
                             result.sha256 + "）";
        }
        if (last_status == FetchStatus::kOk) {
            // overwrite=false 时用排他落盘：并发下只有一个请求能成功，其余
            // 返回 kTargetExists，而不是悄悄互相覆盖。
            bool committed = false;
            if (!options.overwrite) {
                committed = CommitNoReplace(temp_path, target, &result);
                if (result.status == FetchStatus::kTargetExists ||
                    result.status == FetchStatus::kIoError) {
                    ::unlink(temp_path.c_str());
                    return result;
                }
            }
            if (!committed) {
                std::error_code rename_ec;
                std::filesystem::rename(temp_path, target, rename_ec);
                if (rename_ec) {
                    ::unlink(temp_path.c_str());
                    return fail(FetchStatus::kIoError,
                                std::string("落盘 rename 失败: ") + rename_ec.message());
                }
            }
            result.status = FetchStatus::kOk;
            result.path = target;
            result.message.clear();
            return result;
        }

        ::unlink(temp_path.c_str());
        if (!IsRetryable(last_status, result.http_status) || attempt == max_attempts) {
            std::string message = result.message;
            if (message.empty()) {
                message = FetchStatusToString(last_status);
            }
            if (IsRetryable(last_status, result.http_status) && max_attempts > 1) {
                result.status = FetchStatus::kRetryExhausted;
                result.message = "重试 " + std::to_string(options.max_retries) +
                                 " 次后仍失败: " + message;
            } else {
                result.status = last_status;
                result.message = std::move(message);
            }
            result.path.clear();
            return result;
        }
        detail::SleepBeforeRetry(std::chrono::milliseconds(options.retry_backoff_ms), attempt,
                                 std::chrono::milliseconds(kFetchMaxBackoffMs));
    }

    // 逻辑上不可达：循环内必然 return。
    result.status = last_status;
    result.message = "下载失败";
    return result;
}

}  // namespace go2cpp::bento::app
