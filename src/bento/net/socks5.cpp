/**
 * @file socks5.cpp
 * @brief SOCKS5（RFC 1928）骨架实现：协议编解码 + 单线程 poll 服务端。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details 实现分成两块：
 *          1. 协议编解码：只做字节级解析/生成，不碰 fd、不加锁，可在任意
 *             线程调用，便于单元测试与复用；
 *          2. 服务端：一个 poll 事件循环驱动 N 条连接的状态机，连接状态
 *             只属于驱动线程，网络 IO 全部非阻塞。
 *
 * ## 为什么每个方向各一条 pending 缓冲
 *
 *          `to_client`/`to_upstream` 分别保存「已读到但还没写出」的字节，不跨
 *          方向混用。缓冲非空时停止从该方向继续读，形成背压：慢的一侧不会
 *          让服务端把源侧数据吸进内存。这与 `TcpProxy`/`Relay` 的模型一致，
 *          排查问题时语义相同。
 *
 * ## 半关闭
 *
 *          客户端发 FIN 后仍可能要读上游响应，所以读到 EOF 只标记该方向结束，
 *          等该方向 pending 排空后再对下游 `shutdown(SHUT_WR)`；两个方向都
 *          EOF 且缓冲排空才真正关闭。若一方一直不发 FIN，由空闲超时兜底。
 *
 * ## 已知限制
 *
 *          - 域名解析在驱动线程同步 `getaddrinfo`，DNS 慢会阻塞其他连接；
 *          - 域名同时有 A/AAAA 记录时优先尝试 IPv4（见 `ResolveTarget`），
 *            骨架不做多地址轮询；
 *          - 只用 `poll`，没有 epoll；连接数上千时应换事件驱动实现。
 */

#include "go2cpp/bento/net/socks5.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::bento::socks5 {
namespace {

using Clock = std::chrono::steady_clock;

/// 握手/请求阶段的输入上限：合法请求最长 4 + 1 + 255 + 2 = 262 字节，
/// 留足余量后仍能挡住「只发垃圾不发明文长度」的客户端。
constexpr std::size_t kMaxHandshakeBytes = 1024;

/// 单条连接的状态机阶段。
enum class Phase : std::uint8_t {
    kGreeting,    ///< 等客户端问候（VER/NMETHODS/METHODS）。
    kRequest,     ///< 等 CONNECT 请求（CMD/ATYP/地址/端口）。
    kConnecting,  ///< 上游非阻塞 connect 已发起，等可写。
    kRelay,       ///< 双向中继。
    kClosing,     ///< 已拒绝/已结束，只把待发响应写出后关闭。
};

/// 当前稳态时间（毫秒，单调时钟）。
std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
}

/// 设置非阻塞；失败返回 false（调用方应关闭 fd）。
bool SetNonBlocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

/// 设置 close-on-exec；失败不致命（fd 只在 fork+exec 场景泄漏）。
void SetCloExec(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

/// 关闭 fd 并置 -1；-1 安全。
void CloseFd(int* fd) noexcept {
    if (*fd >= 0) {
        ::close(*fd);
        *fd = -1;
    }
}

/// 按 `address_type` 把 4/16 字节地址格式化成文本。
std::string FormatAddress(const std::uint8_t* bytes, std::size_t length, int family) {
    char text[INET6_ADDRSTRLEN] = {};
    if (::inet_ntop(family, bytes, text, sizeof(text)) == nullptr) {
        return {};
    }
    (void)length;
    return std::string(text);
}

/// 地址类型转 sockaddr 族。
int FamilyOf(Socks5AddressType type) noexcept {
    switch (type) {
        case Socks5AddressType::kIpv4:
            return AF_INET;
        case Socks5AddressType::kIpv6:
            return AF_INET6;
        case Socks5AddressType::kDomainName:
            return AF_UNSPEC;
    }
    return AF_UNSPEC;
}

/// 目标地址解析结果（只保留一条，骨架不做多地址轮询）。
struct ResolvedTarget {
    sockaddr_storage address{};
    socklen_t length = 0;
    int family = AF_UNSPEC;
};

/**
 * @brief 解析目标为一条 sockaddr。
 *
 * @details 统一走 `getaddrinfo`：IPv4/IPv6 字面量也能被它识别，避免手写
 *          `inet_pton` 分支。域名同时有 A/AAAA 时**优先 AF_INET**：骨架只
 *          保留一条地址，优先 IPv4 能让「localhost + 只监听 127.0.0.1 的
 *          上游」这种最常见的测试/内网部署直接可用；纯 IPv6 目标在
 *          `getaddrinfo` 只返回 AAAA 时自然走 IPv6。
 *          端口为 0 或主机为空时直接失败（不查 DNS）。
 */
bool ResolveTarget(const Socks5Request& request, ResolvedTarget& resolved) {
    if (request.host.empty() || request.port == 0) {
        return false;
    }
    addrinfo hints{};
    hints.ai_family = FamilyOf(request.address_type);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string service = std::to_string(request.port);
    const int rc = ::getaddrinfo(request.host.c_str(), service.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        if (result != nullptr) {
            ::freeaddrinfo(result);
        }
        return false;
    }

    const auto usable = [](const addrinfo* entry) {
        return entry->ai_addr != nullptr && entry->ai_addrlen <= sizeof(sockaddr_storage) &&
               (entry->ai_family == AF_INET || entry->ai_family == AF_INET6);
    };
    const addrinfo* chosen = nullptr;
    for (const addrinfo* it = result; it != nullptr; it = it->ai_next) {
        if (it->ai_family == AF_INET && usable(it)) {
            chosen = it;
            break;
        }
    }
    if (chosen == nullptr) {
        for (const addrinfo* it = result; it != nullptr; it = it->ai_next) {
            if (usable(it)) {
                chosen = it;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        ::freeaddrinfo(result);
        return false;
    }
    std::memcpy(&resolved.address, chosen->ai_addr, chosen->ai_addrlen);
    resolved.length = static_cast<socklen_t>(chosen->ai_addrlen);
    resolved.family = chosen->ai_family;
    ::freeaddrinfo(result);
    return true;
}

/// connect(2) 的 errno 映射为 RFC 1928 响应码。
Socks5ReplyCode MapConnectError(int error) noexcept {
    switch (error) {
        case 0:
            return Socks5ReplyCode::kSucceeded;
        case ECONNREFUSED:
            return Socks5ReplyCode::kConnectionRefused;
        case ENETUNREACH:
            return Socks5ReplyCode::kNetworkUnreachable;
        case EHOSTUNREACH:
            return Socks5ReplyCode::kHostUnreachable;
        case ETIMEDOUT:
            return Socks5ReplyCode::kTtlExpired;
        case EACCES:
        case EPERM:
            return Socks5ReplyCode::kNotAllowed;
        default:
            return Socks5ReplyCode::kGeneralFailure;
    }
}

/// 写缓冲状态。
enum class IoStatus : std::uint8_t {
    kOk,         ///< 全部写完（缓冲已清空）。
    kWouldBlock, ///< 暂时写不动，保留 pending 等下一轮。
    kError,      ///< 不可恢复的写错误。
};

/// 把 `buffer[offset..]` 尽量写到 fd（非阻塞，循环处理 EINTR）。
IoStatus FlushTo(int fd, std::vector<std::uint8_t>& buffer, std::size_t& offset) {
    while (offset < buffer.size()) {
        const std::size_t remaining = buffer.size() - offset;
        const ssize_t written = ::send(fd, buffer.data() + offset, remaining, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return IoStatus::kWouldBlock;
        }
        return IoStatus::kError;
    }
    buffer.clear();
    offset = 0;
    return IoStatus::kOk;
}

/// 读状态。
enum class ReadStatus : std::uint8_t {
    kOk,         ///< 读到 >=1 字节。
    kEof,        ///< 对端关闭写方向。
    kWouldBlock, ///< 暂无数据（含 EINTR，下一轮重试）。
    kError,      ///< 不可恢复的读错误。
};

/// 从 fd 读一块到 buffer 末尾；`chunk` 同时是单次上限。
ReadStatus ReadInto(int fd, std::vector<std::uint8_t>& buffer, std::size_t chunk) {
    if (chunk == 0) {
        chunk = 4096;
    }
    const std::size_t old_size = buffer.size();
    buffer.resize(old_size + chunk);
    const ssize_t count = ::recv(fd, buffer.data() + old_size, chunk, 0);
    if (count > 0) {
        buffer.resize(old_size + static_cast<std::size_t>(count));
        return ReadStatus::kOk;
    }
    buffer.resize(old_size);
    if (count == 0) {
        return ReadStatus::kEof;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return ReadStatus::kWouldBlock;
    }
    return ReadStatus::kError;
}

/// 非阻塞 connect 是否已有结论；返回 false 表示仍在进行。
bool ConnectFinished(int fd, int& error) {
    pollfd entry{};
    entry.fd = fd;
    entry.events = POLLOUT;
    const int rc = ::poll(&entry, 1, 0);
    if (rc < 0) {
        error = (errno == EINTR) ? EINPROGRESS : errno;
        return error != EINPROGRESS;
    }
    if (rc == 0) {
        return false;
    }
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
        error = errno;
        return true;
    }
    error = socket_error;
    return true;
}

}  // namespace

/// @brief 一条连接的完整状态。只在驱动线程访问。
struct Socks5Server::Connection {
    std::uint64_t id = 0;
    int client_fd = -1;
    int upstream_fd = -1;
    Phase phase = Phase::kGreeting;

    /// 握手/请求阶段累积的客户端输入；进入中继后清空（剩余字节转入 to_upstream）。
    std::vector<std::uint8_t> client_input;
    /// 发往客户端的数据（响应或上游回包）。
    std::vector<std::uint8_t> to_client;
    std::size_t to_client_off = 0;
    /// 发往上游的数据（客户端请求体）。
    std::vector<std::uint8_t> to_upstream;
    std::size_t to_upstream_off = 0;

    bool client_eof = false;
    bool upstream_eof = false;
    bool client_shutdown = false;
    bool upstream_shutdown = false;
    bool dead = false;

    /// 当前阶段 deadline（0 = 不限制）；进入中继后不再使用。
    std::uint64_t deadline_ms = 0;
    /// 最近一次有字节流动的时间（中继空闲超时用）。
    std::uint64_t last_activity_ms = 0;

    Socks5Request request;

    bool ToClientPending() const noexcept { return to_client_off < to_client.size(); }
    bool ToUpstreamPending() const noexcept { return to_upstream_off < to_upstream.size(); }
};

// ---------------------------------------------------------------------------
// 协议层
// ---------------------------------------------------------------------------

Socks5GreetingResult ParseSocks5Greeting(const std::uint8_t* data, std::size_t size,
                                         Socks5Greeting& greeting) {
    Socks5GreetingResult result;
    if (data == nullptr || size < 1) {
        return result;
    }
    result.version = data[0];
    if (result.version != kProtocolVersion) {
        result.state = Socks5ParseState::kError;
        return result;
    }
    if (size < 2) {
        return result;
    }
    const std::size_t method_count = data[1];
    if (method_count == 0) {
        result.state = Socks5ParseState::kError;
        return result;
    }
    if (size < 2 + method_count) {
        return result;
    }
    greeting.methods.assign(data + 2, data + 2 + method_count);
    result.state = Socks5ParseState::kComplete;
    result.consumed = 2 + method_count;
    return result;
}

std::vector<std::uint8_t> EncodeSocks5MethodSelection(bool accept_no_auth) {
    const auto method = accept_no_auth
                            ? static_cast<std::uint8_t>(Socks5Method::kNoAuthentication)
                            : static_cast<std::uint8_t>(Socks5Method::kNoAcceptable);
    return {kProtocolVersion, method};
}

Socks5RequestResult ParseSocks5Request(const std::uint8_t* data, std::size_t size,
                                       Socks5Request& request) {
    Socks5RequestResult result;
    if (data == nullptr || size < 1) {
        return result;
    }
    if (data[0] != kProtocolVersion) {
        result.state = Socks5ParseState::kError;
        result.error = Socks5ReplyCode::kGeneralFailure;
        result.consumed = 1;
        return result;
    }
    if (size < 4) {
        return result;
    }
    const auto command = static_cast<Socks5Command>(data[1]);
    if (command != Socks5Command::kConnect) {
        // BIND/UDP ASSOCIATE 以及未分配命令都不支持；不必等地址部分。
        result.state = Socks5ParseState::kError;
        result.error = Socks5ReplyCode::kCommandNotSupported;
        result.consumed = 2;
        return result;
    }
    if (data[2] != 0x00) {
        result.state = Socks5ParseState::kError;
        result.error = Socks5ReplyCode::kGeneralFailure;
        result.consumed = 3;
        return result;
    }
    const auto address_type = static_cast<Socks5AddressType>(data[3]);
    std::size_t offset = 4;
    std::string host;
    switch (address_type) {
        case Socks5AddressType::kIpv4: {
            if (size < offset + 4) {
                return result;
            }
            host = FormatAddress(data + offset, 4, AF_INET);
            offset += 4;
            break;
        }
        case Socks5AddressType::kDomainName: {
            if (size < offset + 1) {
                return result;
            }
            const std::size_t name_length = data[offset];
            ++offset;
            if (name_length == 0) {
                // 空域名无法解析，按主机不可达回包；不等后续字节。
                result.state = Socks5ParseState::kError;
                result.error = Socks5ReplyCode::kHostUnreachable;
                result.consumed = offset;
                return result;
            }
            if (size < offset + name_length) {
                return result;
            }
            host.assign(reinterpret_cast<const char*>(data + offset), name_length);
            offset += name_length;
            break;
        }
        case Socks5AddressType::kIpv6: {
            if (size < offset + 16) {
                return result;
            }
            host = FormatAddress(data + offset, 16, AF_INET6);
            offset += 16;
            break;
        }
        default:
            result.state = Socks5ParseState::kError;
            result.error = Socks5ReplyCode::kAddressTypeNotSupported;
            result.consumed = offset;
            return result;
    }
    if (host.empty()) {
        result.state = Socks5ParseState::kError;
        result.error = Socks5ReplyCode::kGeneralFailure;
        result.consumed = offset;
        return result;
    }
    if (size < offset + 2) {
        return result;
    }
    const std::uint16_t port =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8U) |
                                   static_cast<std::uint16_t>(data[offset + 1]));
    offset += 2;
    request.command = command;
    request.address_type = address_type;
    request.host = std::move(host);
    request.port = port;
    result.state = Socks5ParseState::kComplete;
    result.consumed = offset;
    return result;
}

std::vector<std::uint8_t> EncodeSocks5Reply(Socks5ReplyCode code,
                                            Socks5AddressType address_type,
                                            const std::uint8_t* address,
                                            std::size_t address_length,
                                            std::uint16_t port) {
    const bool success = code == Socks5ReplyCode::kSucceeded;
    const bool valid_address =
        address != nullptr &&
        ((address_type == Socks5AddressType::kIpv4 && address_length == 4) ||
         (address_type == Socks5AddressType::kIpv6 && address_length == 16));

    std::vector<std::uint8_t> reply;
    reply.reserve(4 + 16 + 2);
    reply.push_back(kProtocolVersion);
    reply.push_back(static_cast<std::uint8_t>(code));
    reply.push_back(0x00);
    if (success && valid_address) {
        reply.push_back(static_cast<std::uint8_t>(address_type));
        reply.insert(reply.end(), address, address + address_length);
        reply.push_back(static_cast<std::uint8_t>((port >> 8U) & 0xFFU));
        reply.push_back(static_cast<std::uint8_t>(port & 0xFFU));
    } else {
        // 失败响应统一 0.0.0.0:0：不泄漏内部地址，也不依赖上游地址族。
        reply.push_back(static_cast<std::uint8_t>(Socks5AddressType::kIpv4));
        reply.insert(reply.end(), 4, 0);
        reply.push_back(0x00);
        reply.push_back(0x00);
    }
    return reply;
}

const char* Socks5ReplyCodeName(Socks5ReplyCode code) noexcept {
    switch (code) {
        case Socks5ReplyCode::kSucceeded:
            return "succeeded";
        case Socks5ReplyCode::kGeneralFailure:
            return "general failure";
        case Socks5ReplyCode::kNotAllowed:
            return "not allowed";
        case Socks5ReplyCode::kNetworkUnreachable:
            return "network unreachable";
        case Socks5ReplyCode::kHostUnreachable:
            return "host unreachable";
        case Socks5ReplyCode::kConnectionRefused:
            return "connection refused";
        case Socks5ReplyCode::kTtlExpired:
            return "ttl expired";
        case Socks5ReplyCode::kCommandNotSupported:
            return "command not supported";
        case Socks5ReplyCode::kAddressTypeNotSupported:
            return "address type not supported";
    }
    return "Unknown";
}

const char* Socks5CommandName(Socks5Command command) noexcept {
    switch (command) {
        case Socks5Command::kConnect:
            return "CONNECT";
        case Socks5Command::kBind:
            return "BIND";
        case Socks5Command::kUdpAssociate:
            return "UDP ASSOCIATE";
    }
    return "Unknown";
}

const char* Socks5AddressTypeName(Socks5AddressType type) noexcept {
    switch (type) {
        case Socks5AddressType::kIpv4:
            return "IPv4";
        case Socks5AddressType::kDomainName:
            return "domain";
        case Socks5AddressType::kIpv6:
            return "IPv6";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// 服务端
// ---------------------------------------------------------------------------

Socks5Server::Socks5Server(Socks5ServerConfig config) : m_config(std::move(config)) {
    if (m_config.listen_address.empty()) {
        m_config.listen_address = "0.0.0.0";
    }
    if (m_config.buffer_bytes < 4096) {
        m_config.buffer_bytes = 4096;
    }
    if (m_config.max_connections == 0) {
        m_config.max_connections = 1;
    }
    if (m_config.backlog <= 0) {
        m_config.backlog = 64;
    }
}

Socks5Server::~Socks5Server() {
    Stop();
    Close();
}

bool Socks5Server::Open(std::string* error) {
    if (m_listen_fd >= 0) {
        if (error != nullptr) {
            *error = "socks5 server is already open";
        }
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(m_config.listen_port);
    const int rc = ::getaddrinfo(m_config.listen_address.c_str(), service.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        if (result != nullptr) {
            ::freeaddrinfo(result);
        }
        if (error != nullptr) {
            *error = std::string("getaddrinfo(") + m_config.listen_address +
                     "): " + ::gai_strerror(rc);
        }
        return false;
    }

    int listen_fd = -1;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        if (it->ai_family != AF_INET && it->ai_family != AF_INET6) {
            continue;
        }
        const int fd = ::socket(it->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            continue;
        }
        const int yes = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (::bind(fd, it->ai_addr, it->ai_addrlen) == 0 &&
            ::listen(fd, m_config.backlog) == 0) {
            listen_fd = fd;
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(result);
    if (listen_fd < 0) {
        if (error != nullptr) {
            *error = "bind/listen failed for " + m_config.listen_address + ":" +
                     std::to_string(m_config.listen_port) + ": " + std::strerror(errno);
        }
        return false;
    }
    if (!SetNonBlocking(listen_fd)) {
        const int saved_errno = errno;
        ::close(listen_fd);
        if (error != nullptr) {
            *error = std::string("set non-blocking failed: ") + std::strerror(saved_errno);
        }
        return false;
    }
    SetCloExec(listen_fd);
    m_listen_fd = listen_fd;

    sockaddr_storage bound{};
    socklen_t bound_length = sizeof(bound);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
        if (bound.ss_family == AF_INET) {
            m_local_port = ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
        } else if (bound.ss_family == AF_INET6) {
            m_local_port = ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
        }
    }
    return true;
}

bool Socks5Server::Start(std::string* error) {
    if (m_listen_fd < 0) {
        if (error != nullptr) {
            *error = "socks5 server is not open";
        }
        return false;
    }
    if (m_running) {
        if (error != nullptr) {
            *error = "socks5 server is already running";
        }
        return false;
    }
    try {
        m_stop.store(false);
        m_thread = std::thread([this]() {
            while (!m_stop.load()) {
                (void)Poll(50);
            }
        });
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("failed to start poll thread: ") + exception.what();
        }
        return false;
    }
    m_running = true;
    return true;
}

void Socks5Server::Stop() {
    m_stop.store(true);
    if (m_thread.joinable()) {
        if (m_thread.get_id() == std::this_thread::get_id()) {
            // 在驱动线程内调用 Stop（例如回调里）：不能 join 自己，detach 后
            // 由线程自行结束；对象析构前调用方应确认线程已退出。
            m_thread.detach();
        } else {
            m_thread.join();
        }
    }
    m_running = false;
}

void Socks5Server::Close() {
    for (auto& connection : m_connections) {
        if (connection->dead) {
            continue;
        }
        CloseFd(&connection->client_fd);
        CloseFd(&connection->upstream_fd);
        connection->dead = true;
        ++m_stats.closed;
    }
    m_connections.clear();
    CloseFd(&m_listen_fd);
    m_local_port = 0;
}

void Socks5Server::CloseConnection(Connection& connection, const char* reason) {
    (void)reason;  // 保留参数便于调试；统计已按阶段分开计数。
    if (connection.dead) {
        return;
    }
    connection.dead = true;
    CloseFd(&connection.client_fd);
    CloseFd(&connection.upstream_fd);
    ++m_stats.closed;
}

void Socks5Server::ReapDead() {
    m_connections.erase(
        std::remove_if(m_connections.begin(), m_connections.end(),
                       [](const std::unique_ptr<Connection>& connection) {
                           return connection->dead;
                       }),
        m_connections.end());
}

void Socks5Server::RejectRequest(Connection& connection, Socks5ReplyCode code,
                                 std::uint64_t now_ms) {
    const auto reply = EncodeSocks5Reply(code, Socks5AddressType::kIpv4, nullptr, 0, 0);
    connection.to_client.assign(reply.begin(), reply.end());
    connection.to_client_off = 0;
    connection.phase = Phase::kClosing;
    connection.deadline_ms = m_config.close_flush_timeout_ms == 0
                                 ? 0
                                 : now_ms + m_config.close_flush_timeout_ms;
}

void Socks5Server::AcceptReady() {
    for (;;) {
        const int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;  // EAGAIN/EWOULDBLOCK：队列已空。
        }
        if (m_connections.size() >= m_config.max_connections) {
            ::close(client_fd);
            ++m_stats.rejected;
            continue;
        }
        if (!SetNonBlocking(client_fd)) {
            ::close(client_fd);
            ++m_stats.rejected;
            continue;
        }
        SetCloExec(client_fd);

        auto connection = std::make_unique<Connection>();
        connection->id = m_next_connection_id++;
        connection->client_fd = client_fd;
        connection->last_activity_ms = NowMs();
        connection->deadline_ms = m_config.handshake_timeout_ms == 0
                                      ? 0
                                      : connection->last_activity_ms + m_config.handshake_timeout_ms;
        m_connections.push_back(std::move(connection));
        ++m_stats.accepted;
    }
}

int Socks5Server::ServiceGreeting(Connection& connection, std::uint64_t now_ms) {
    Socks5Greeting greeting;
    auto parsed =
        ParseSocks5Greeting(connection.client_input.data(), connection.client_input.size(), greeting);
    if (parsed.state == Socks5ParseState::kIncomplete) {
        const auto status = ReadInto(connection.client_fd, connection.client_input,
                                     m_config.buffer_bytes);
        if (status == ReadStatus::kError) {
            ++m_stats.handshake_failed;
            CloseConnection(connection, "handshake-read-error");
            return 0;
        }
        if (status == ReadStatus::kEof) {
            ++m_stats.handshake_failed;
            CloseConnection(connection, "handshake-eof");
            return 0;
        }
        if (status == ReadStatus::kWouldBlock) {
            return 0;
        }
        parsed = ParseSocks5Greeting(connection.client_input.data(),
                                     connection.client_input.size(), greeting);
    }
    if (parsed.state == Socks5ParseState::kIncomplete) {
        if (connection.client_input.size() > kMaxHandshakeBytes) {
            ++m_stats.handshake_failed;
            CloseConnection(connection, "handshake-too-large");
        }
        return 0;
    }
    if (parsed.state == Socks5ParseState::kError) {
        ++m_stats.handshake_failed;
        CloseConnection(connection, "handshake-invalid");
        return 0;
    }

    connection.client_input.erase(
        connection.client_input.begin(),
        connection.client_input.begin() + static_cast<std::ptrdiff_t>(parsed.consumed));
    if (!greeting.OffersNoAuthentication()) {
        // RFC 1928 §3：没有可接受的方法时回 05 FF，随后关闭。
        ++m_stats.auth_rejected;
        connection.to_client = EncodeSocks5MethodSelection(false);
        connection.to_client_off = 0;
        connection.phase = Phase::kClosing;
        connection.deadline_ms =
            m_config.close_flush_timeout_ms == 0
                ? 0
                : now_ms + m_config.close_flush_timeout_ms;
        return ServiceClosing(connection);
    }

    ++m_stats.handshake_ok;
    connection.to_client = EncodeSocks5MethodSelection(true);
    connection.to_client_off = 0;
    connection.phase = Phase::kRequest;
    // 同一轮继续解析请求：支持客户端把问候和请求一起发（pipeline）。
    return ServiceRequest(connection, now_ms);
}

int Socks5Server::ServiceRequest(Connection& connection, std::uint64_t now_ms) {
    Socks5Request request;
    auto parsed = ParseSocks5Request(connection.client_input.data(),
                                     connection.client_input.size(), request);
    if (parsed.state == Socks5ParseState::kIncomplete) {
        const auto status = ReadInto(connection.client_fd, connection.client_input,
                                     m_config.buffer_bytes);
        if (status == ReadStatus::kError) {
            ++m_stats.request_errors;
            CloseConnection(connection, "request-read-error");
            return 0;
        }
        if (status == ReadStatus::kEof) {
            ++m_stats.request_errors;
            CloseConnection(connection, "request-eof");
            return 0;
        }
        if (status == ReadStatus::kWouldBlock) {
            return 0;
        }
        parsed = ParseSocks5Request(connection.client_input.data(),
                                    connection.client_input.size(), request);
    }
    if (parsed.state == Socks5ParseState::kIncomplete) {
        if (connection.client_input.size() > kMaxHandshakeBytes) {
            ++m_stats.request_errors;
            CloseConnection(connection, "request-too-large");
        }
        return 0;
    }
    if (parsed.state == Socks5ParseState::kError) {
        ++m_stats.request_errors;
        RejectRequest(connection, parsed.error, now_ms);
        return ServiceClosing(connection);
    }

    connection.client_input.erase(
        connection.client_input.begin(),
        connection.client_input.begin() + static_cast<std::ptrdiff_t>(parsed.consumed));
    connection.request = std::move(request);
    ++m_stats.requests;
    if (!StartConnect(connection, now_ms)) {
        return 0;
    }
    if (connection.phase == Phase::kRelay) {
        return ServiceRelay(connection, now_ms);
    }
    return 0;
}

bool Socks5Server::StartConnect(Connection& connection, std::uint64_t now_ms) {
    if (connection.request.port == 0) {
        ++m_stats.connect_failed;
        RejectRequest(connection, Socks5ReplyCode::kGeneralFailure, now_ms);
        (void)ServiceClosing(connection);
        return false;
    }
    ResolvedTarget target;
    if (!ResolveTarget(connection.request, target)) {
        ++m_stats.connect_failed;
        RejectRequest(connection, Socks5ReplyCode::kHostUnreachable, now_ms);
        (void)ServiceClosing(connection);
        return false;
    }

    const int upstream_fd = ::socket(target.family, SOCK_STREAM, IPPROTO_TCP);
    if (upstream_fd < 0) {
        ++m_stats.connect_failed;
        RejectRequest(connection, Socks5ReplyCode::kGeneralFailure, now_ms);
        (void)ServiceClosing(connection);
        return false;
    }
    if (!SetNonBlocking(upstream_fd)) {
        const int saved_errno = errno;
        ::close(upstream_fd);
        ++m_stats.connect_failed;
        RejectRequest(connection, MapConnectError(saved_errno), now_ms);
        (void)ServiceClosing(connection);
        return false;
    }
    SetCloExec(upstream_fd);
    connection.upstream_fd = upstream_fd;

    const int rc = ::connect(upstream_fd, reinterpret_cast<const sockaddr*>(&target.address),
                             target.length);
    if (rc == 0) {
        BeginRelay(connection, now_ms);
        return true;
    }
    if (errno == EINPROGRESS || errno == EINTR) {
        connection.phase = Phase::kConnecting;
        connection.deadline_ms = m_config.connect_timeout_ms == 0
                                     ? 0
                                     : now_ms + m_config.connect_timeout_ms;
        return true;
    }
    const int saved_errno = errno;
    ++m_stats.connect_failed;
    RejectRequest(connection, MapConnectError(saved_errno), now_ms);
    (void)ServiceClosing(connection);
    return false;
}

int Socks5Server::ServiceConnecting(Connection& connection, std::uint64_t now_ms) {
    int error = 0;
    if (!ConnectFinished(connection.upstream_fd, error)) {
        return 0;
    }
    if (error == 0) {
        BeginRelay(connection, now_ms);
        return ServiceRelay(connection, now_ms);
    }
    ++m_stats.connect_failed;
    RejectRequest(connection, MapConnectError(error), now_ms);
    return ServiceClosing(connection);
}

void Socks5Server::BeginRelay(Connection& connection, std::uint64_t now_ms) {
    ++m_stats.connect_ok;

    // BND.ADDR/BND.PORT 填上游 socket 的本地地址：客户端若要回连/诊断，
    // 看到的是真实出口，而不是 0.0.0.0:0。
    sockaddr_storage local{};
    socklen_t local_length = sizeof(local);
    Socks5AddressType address_type = Socks5AddressType::kIpv4;
    std::uint8_t address_bytes[16] = {};
    std::size_t address_length = 0;
    std::uint16_t port = 0;
    if (::getsockname(connection.upstream_fd, reinterpret_cast<sockaddr*>(&local),
                      &local_length) == 0) {
        if (local.ss_family == AF_INET) {
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&local);
            std::memcpy(address_bytes, &ipv4->sin_addr, 4);
            address_length = 4;
            port = ntohs(ipv4->sin_port);
        } else if (local.ss_family == AF_INET6) {
            const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&local);
            std::memcpy(address_bytes, &ipv6->sin6_addr, 16);
            address_type = Socks5AddressType::kIpv6;
            address_length = 16;
            port = ntohs(ipv6->sin6_port);
        }
    }
    const auto reply =
        EncodeSocks5Reply(Socks5ReplyCode::kSucceeded, address_type, address_bytes,
                          address_length, port);
    connection.to_client.insert(connection.to_client.end(), reply.begin(), reply.end());

    // 握手期间可能已经把请求体一起读了进来，全部转给上游，不能丢。
    if (!connection.client_input.empty()) {
        connection.to_upstream.insert(connection.to_upstream.end(), connection.client_input.begin(),
                                      connection.client_input.end());
        m_stats.bytes_to_upstream += connection.client_input.size();
        connection.client_input.clear();
    }
    connection.client_input.shrink_to_fit();
    connection.phase = Phase::kRelay;
    connection.deadline_ms = 0;
    connection.last_activity_ms = now_ms;
}

int Socks5Server::ServiceRelay(Connection& connection, std::uint64_t now_ms) {
    int moved = 0;

    // 1. 先把两个方向的 pending 尽量写出去（背压的另一半）。
    if (connection.ToClientPending()) {
        const auto status =
            FlushTo(connection.client_fd, connection.to_client, connection.to_client_off);
        if (status == IoStatus::kError) {
            ++m_stats.resets;
            CloseConnection(connection, "client-write-error");
            return moved;
        }
    }
    if (connection.ToUpstreamPending()) {
        const auto status =
            FlushTo(connection.upstream_fd, connection.to_upstream, connection.to_upstream_off);
        if (status == IoStatus::kError) {
            ++m_stats.resets;
            CloseConnection(connection, "upstream-write-error");
            return moved;
        }
    }

    // 2. 客户端 → 上游：上游缓冲为空时才继续读，形成背压。
    if (!connection.client_eof && !connection.ToUpstreamPending()) {
        const std::size_t before = connection.to_upstream.size();
        const auto status =
            ReadInto(connection.client_fd, connection.to_upstream, m_config.buffer_bytes);
        if (status == ReadStatus::kEof) {
            connection.client_eof = true;
        } else if (status == ReadStatus::kError) {
            ++m_stats.resets;
            CloseConnection(connection, "client-read-error");
            return moved;
        } else if (status == ReadStatus::kOk) {
            const std::size_t read_bytes = connection.to_upstream.size() - before;
            m_stats.bytes_to_upstream += read_bytes;
            moved += static_cast<int>(read_bytes);
            connection.last_activity_ms = now_ms;
            const auto flush_status = FlushTo(connection.upstream_fd, connection.to_upstream,
                                              connection.to_upstream_off);
            if (flush_status == IoStatus::kError) {
                ++m_stats.resets;
                CloseConnection(connection, "upstream-write-error");
                return moved;
            }
        }
    }

    // 3. 上游 → 客户端。
    if (!connection.upstream_eof && !connection.ToClientPending()) {
        const std::size_t before = connection.to_client.size();
        const auto status =
            ReadInto(connection.upstream_fd, connection.to_client, m_config.buffer_bytes);
        if (status == ReadStatus::kEof) {
            connection.upstream_eof = true;
        } else if (status == ReadStatus::kError) {
            ++m_stats.resets;
            CloseConnection(connection, "upstream-read-error");
            return moved;
        } else if (status == ReadStatus::kOk) {
            const std::size_t read_bytes = connection.to_client.size() - before;
            m_stats.bytes_to_client += read_bytes;
            moved += static_cast<int>(read_bytes);
            connection.last_activity_ms = now_ms;
            const auto flush_status =
                FlushTo(connection.client_fd, connection.to_client, connection.to_client_off);
            if (flush_status == IoStatus::kError) {
                ++m_stats.resets;
                CloseConnection(connection, "client-write-error");
                return moved;
            }
        }
    }

    // 4. 半关闭传播：只有该方向 pending 排空后才发 FIN，避免截断响应。
    if (connection.client_eof && !connection.ToUpstreamPending() &&
        !connection.upstream_shutdown) {
        (void)::shutdown(connection.upstream_fd, SHUT_WR);
        connection.upstream_shutdown = true;
    }
    if (connection.upstream_eof && !connection.ToClientPending() &&
        !connection.client_shutdown) {
        (void)::shutdown(connection.client_fd, SHUT_WR);
        connection.client_shutdown = true;
    }

    // 5. 双向 EOF 且缓冲排空：正常结束。
    if (connection.client_eof && connection.upstream_eof && !connection.ToClientPending() &&
        !connection.ToUpstreamPending()) {
        CloseConnection(connection, "relay-complete");
    }
    return moved;
}

int Socks5Server::ServiceClosing(Connection& connection) {
    if (connection.ToClientPending()) {
        const auto status =
            FlushTo(connection.client_fd, connection.to_client, connection.to_client_off);
        if (status == IoStatus::kError) {
            CloseConnection(connection, "closing-write-error");
            return 0;
        }
        if (connection.ToClientPending()) {
            return 0;  // 等下一轮继续写，超时由 CheckTimeouts 兜底。
        }
    }
    CloseConnection(connection, "closing");
    return 0;
}

int Socks5Server::ServiceConnection(std::size_t index, std::uint64_t now_ms) {
    if (index >= m_connections.size()) {
        return 0;
    }
    Connection& connection = *m_connections[index];
    if (connection.dead) {
        return 0;
    }

    // 任何阶段都尽量把已排队的响应写出去：握手响应不能一直压到中继阶段。
    if (connection.ToClientPending()) {
        const auto status =
            FlushTo(connection.client_fd, connection.to_client, connection.to_client_off);
        if (status == IoStatus::kError) {
            ++m_stats.resets;
            CloseConnection(connection, "client-write-error");
            return 0;
        }
    }

    switch (connection.phase) {
        case Phase::kGreeting:
            return ServiceGreeting(connection, now_ms);
        case Phase::kRequest:
            return ServiceRequest(connection, now_ms);
        case Phase::kConnecting:
            return ServiceConnecting(connection, now_ms);
        case Phase::kRelay:
            return ServiceRelay(connection, now_ms);
        case Phase::kClosing:
            return ServiceClosing(connection);
    }
    return 0;
}

void Socks5Server::CheckTimeouts(std::uint64_t now_ms) {
    for (auto& connection_ptr : m_connections) {
        Connection& connection = *connection_ptr;
        if (connection.dead) {
            continue;
        }
        if (connection.phase == Phase::kRelay) {
            if (m_config.idle_timeout_ms != 0 && now_ms >= connection.last_activity_ms &&
                now_ms - connection.last_activity_ms >= m_config.idle_timeout_ms) {
                ++m_stats.idle_timeouts;
                CloseConnection(connection, "idle-timeout");
            }
            continue;
        }
        if (connection.deadline_ms == 0 || now_ms < connection.deadline_ms) {
            continue;
        }
        if (connection.phase == Phase::kGreeting || connection.phase == Phase::kRequest) {
            ++m_stats.handshake_timeouts;
            CloseConnection(connection, "handshake-timeout");
        } else if (connection.phase == Phase::kConnecting) {
            ++m_stats.connect_failed;
            RejectRequest(connection, Socks5ReplyCode::kTtlExpired, now_ms);
            (void)ServiceClosing(connection);
        } else {
            CloseConnection(connection, "closing-timeout");
        }
    }
}

int Socks5Server::Poll(int timeout_ms) {
    if (m_listen_fd < 0) {
        return 0;
    }
    int transferred = 0;

    // poll 只负责等待；真正的推进由 ServiceConnection 的非阻塞 IO 完成，
    // 因此这里不需要把 revents 回传给每条连接（事件到达即可唤醒轮询）。
    std::vector<pollfd> fds;
    fds.reserve(1 + m_connections.size() * 2);
    pollfd listen_entry{};
    listen_entry.fd = m_listen_fd;
    listen_entry.events = POLLIN;
    fds.push_back(listen_entry);

    for (auto& connection_ptr : m_connections) {
        Connection& connection = *connection_ptr;
        if (connection.dead) {
            continue;
        }
        short client_events = 0;
        short upstream_events = 0;
        switch (connection.phase) {
            case Phase::kGreeting:
            case Phase::kRequest:
                client_events = POLLIN;
                break;
            case Phase::kConnecting:
                upstream_events = POLLOUT;
                break;
            case Phase::kRelay:
                if (!connection.client_eof && !connection.ToUpstreamPending()) {
                    client_events |= POLLIN;
                }
                if (connection.ToClientPending()) {
                    client_events |= POLLOUT;
                }
                if (!connection.upstream_eof && !connection.ToClientPending()) {
                    upstream_events |= POLLIN;
                }
                if (connection.ToUpstreamPending()) {
                    upstream_events |= POLLOUT;
                }
                break;
            case Phase::kClosing:
                if (connection.ToClientPending()) {
                    client_events = POLLOUT;
                }
                break;
        }
        if (client_events != 0) {
            pollfd entry{};
            entry.fd = connection.client_fd;
            entry.events = client_events;
            fds.push_back(entry);
        }
        if (upstream_events != 0) {
            pollfd entry{};
            entry.fd = connection.upstream_fd;
            entry.events = upstream_events;
            fds.push_back(entry);
        }
    }

    const int rc = ::poll(fds.data(), fds.size(), timeout_ms);
    if (rc < 0 && errno != EINTR) {
        return 0;
    }

    if ((fds[0].revents & POLLIN) != 0) {
        const std::size_t before = m_connections.size();
        AcceptReady();
        transferred += static_cast<int>(m_connections.size() - before);
    }

    const std::uint64_t service_now = NowMs();
    for (std::size_t index = 0; index < m_connections.size(); ++index) {
        transferred += ServiceConnection(index, service_now);
    }
    CheckTimeouts(service_now);
    ReapDead();
    return transferred;
}

void Socks5Server::RunFor(int duration_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(duration_ms, 0));
    while (Clock::now() < deadline) {
        (void)Poll(50);
    }
}

}  // namespace go2cpp::bento::socks5
