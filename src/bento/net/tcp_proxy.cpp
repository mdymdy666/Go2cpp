/**
 * @file tcp_proxy.cpp
 * @brief 通用 TCP 回环代理实现：poll 事件循环、非阻塞 connect、双向半关闭转发。
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include "go2cpp/bento/net/tcp_proxy.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "go2cpp/bento/net/protocol.hpp"

namespace go2cpp::bento {
namespace {

/// 关闭 Nagle：与 `network::Socket::SetTcpNoDelay()` 同语义。
/// @note `go2cpp_bento` 是 core 协议库，不链接 `network` 模块（否则会把
///       HTTP/WS/TLS 整条依赖链拖进来），因此在这里直接做一次 setsockopt。
bool SetTcpNoDelayCompat(int fd) noexcept {
    const int enabled = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0;
}

/// 解析地址（SOCK_STREAM）。返回第一条可用地址；失败时写 error。
bool ResolveStream(const std::string& host, std::uint16_t port, sockaddr_storage* out,
                   socklen_t* out_len, std::string* error) {
    if (host.empty() || port == 0) {
        if (error != nullptr) {
            *error = "address and port are required";
        }
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        if (error != nullptr) {
            *error = std::string("getaddrinfo(") + host + "): " + ::gai_strerror(rc);
        }
        if (result != nullptr) {
            ::freeaddrinfo(result);
        }
        return false;
    }
    bool ok = false;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        if (it->ai_addrlen <= sizeof(sockaddr_storage)) {
            std::memcpy(out, it->ai_addr, it->ai_addrlen);
            *out_len = static_cast<socklen_t>(it->ai_addrlen);
            ok = true;
            break;
        }
    }
    ::freeaddrinfo(result);
    if (!ok && error != nullptr) {
        *error = "no usable address for " + host;
    }
    return ok;
}

/// 关闭 fd；-1 安全。
void CloseFd(int* fd) noexcept {
    if (*fd >= 0) {
        ::close(*fd);
        *fd = -1;
    }
}

/// 宽裕地计算截止时间；timeout 为 0 时返回 now（由调用方决定是否忽略）。
std::uint64_t Deadline(std::uint64_t now_ms, std::uint32_t timeout_ms) noexcept {
    return now_ms + static_cast<std::uint64_t>(timeout_ms);
}

}  // namespace

/// @brief 一条双向管道。两个方向各自持有「读到但未写完」的有界缓冲。
struct TcpProxy::Connection {
    std::uint64_t id = 0;
    int client_fd = -1;
    int target_fd = -1;

    /// true 表示 target_fd 的 connect 尚未完成（等 POLLOUT 后看 SO_ERROR）。
    bool connecting = false;
    std::uint64_t connect_deadline_ms = 0;
    std::uint64_t last_activity_ms = 0;

    // ---- 方向 A：client → target ----
    std::vector<std::uint8_t> a_buffer;
    std::size_t a_len = 0;
    std::size_t a_off = 0;
    bool a_eof = false;      ///< 已从 client 读到 EOF。
    bool a_shutdown = false; ///< 已对 target 执行 SHUT_WR。

    // ---- 方向 B：target → client ----
    std::vector<std::uint8_t> b_buffer;
    std::size_t b_len = 0;
    std::size_t b_off = 0;
    bool b_eof = false;
    bool b_shutdown = false;

    /// 首次进入半关闭的时间；0 表示尚未半关闭。
    std::uint64_t half_close_at_ms = 0;

    bool PendingA() const noexcept {
        return a_len > a_off;
    }
    bool PendingB() const noexcept {
        return b_len > b_off;
    }
    bool CanReadClient() const noexcept {
        return !a_eof && !PendingA();
    }
    bool CanReadTarget() const noexcept {
        return !b_eof && !PendingB();
    }
    /// 两个方向都已 EOF 且缓冲排空：可以正常关闭。
    bool Finished() const noexcept {
        return a_eof && b_eof && !PendingA() && !PendingB();
    }
};

TcpProxy::TcpProxy(TcpProxyConfig config) : m_config(std::move(config)) {
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

TcpProxy::~TcpProxy() {
    Close();
}

bool TcpProxy::SetNonBlocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    const int fd_flags = ::fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) {
        (void)::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
    return true;
}

bool TcpProxy::Open(std::string* error) {
    if (m_listen_fd >= 0) {
        return true;
    }
    if (m_config.target_port == 0) {
        if (error != nullptr) {
            *error = "target_port must be non-zero";
        }
        return false;
    }
    // 目标地址在 Open 时解析一次：DNS 失败属于配置错误，应当立即暴露，
    // 而不是等每个连接 accept 后再失败（JinShan AX-017 的教训）。
    std::string local_error;
    if (!ResolveStream(m_config.target_address, m_config.target_port, &m_target_addr,
                       &m_target_addr_len, &local_error)) {
        if (error != nullptr) {
            *error = local_error;
        }
        return false;
    }

    // listen_port 允许为 0（内核分配），而 getaddrinfo 需要非零 service 串，
    // 所以先用占位端口 1 解析出地址与 family，再把端口字段改成配置值。
    sockaddr_storage listen_addr{};
    socklen_t listen_len = 0;
    if (!ResolveStream(m_config.listen_address, 1, &listen_addr, &listen_len, &local_error)) {
        if (error != nullptr) {
            *error = local_error;
        }
        return false;
    }
    if (listen_addr.ss_family == AF_INET) {
        reinterpret_cast<sockaddr_in*>(&listen_addr)->sin_port = htons(m_config.listen_port);
    } else if (listen_addr.ss_family == AF_INET6) {
        reinterpret_cast<sockaddr_in6*>(&listen_addr)->sin6_port = htons(m_config.listen_port);
    }

    const int listen_fd = ::socket(listen_addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        if (error != nullptr) {
            *error = std::string("socket: ") + std::strerror(errno);
        }
        return false;
    }
    int reuse = 1;
    (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (listen_addr.ss_family == AF_INET6) {
        // 双栈监听：允许 IPv4 客户端连 :: 端口（测试与真实部署都常需要）。
        int v6only = 0;
        (void)::setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    }
    if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&listen_addr), listen_len) != 0) {
        if (error != nullptr) {
            *error = std::string("bind: ") + std::strerror(errno);
        }
        (void)::close(listen_fd);
        return false;
    }
    if (::listen(listen_fd, m_config.backlog) != 0) {
        if (error != nullptr) {
            *error = std::string("listen: ") + std::strerror(errno);
        }
        (void)::close(listen_fd);
        return false;
    }
    if (!SetNonBlocking(listen_fd)) {
        if (error != nullptr) {
            *error = std::string("fcntl: ") + std::strerror(errno);
        }
        (void)::close(listen_fd);
        return false;
    }

    sockaddr_storage actual{};
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
        if (actual.ss_family == AF_INET) {
            m_local_port = ntohs(reinterpret_cast<sockaddr_in*>(&actual)->sin_port);
        } else if (actual.ss_family == AF_INET6) {
            m_local_port = ntohs(reinterpret_cast<sockaddr_in6*>(&actual)->sin6_port);
        }
    }
    m_listen_fd = listen_fd;
    return true;
}

void TcpProxy::Close() {
    for (std::size_t i = m_connections.size(); i > 0; --i) {
        CloseConnection(i - 1, "eof");
    }
    m_connections.clear();
    CloseFd(&m_listen_fd);
    m_local_port = 0;
}

TcpProxyStats TcpProxy::Stats() const {
    TcpProxyStats snapshot = m_stats;
    snapshot.active = 0;
    for (const auto& connection : m_connections) {
        if (!connection->connecting) {
            ++snapshot.active;
        }
    }
    return snapshot;
}

void TcpProxy::CloseConnection(std::size_t index, const char* reason) {
    if (index >= m_connections.size()) {
        return;
    }
    std::unique_ptr<Connection> connection = std::move(m_connections[index]);
    m_connections.erase(m_connections.begin() + static_cast<std::ptrdiff_t>(index));
    CloseFd(&connection->client_fd);
    CloseFd(&connection->target_fd);
    ++m_stats.closed;
    if (std::strcmp(reason, "idle") == 0) {
        ++m_stats.idle_timeouts;
    } else if (std::strcmp(reason, "linger") == 0) {
        ++m_stats.linger_timeouts;
    } else if (std::strcmp(reason, "reset") == 0) {
        ++m_stats.resets;
    } else if (std::strcmp(reason, "dial") == 0) {
        ++m_stats.dial_failed;
    }
    if (on_closed) {
        on_closed(connection->id, reason);
    }
}

void TcpProxy::AcceptReady() {
    for (;;) {
        sockaddr_storage peer{};
        socklen_t peer_len = sizeof(peer);
        const int client_fd = ::accept(m_listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            return;
        }
        ++m_stats.accepted;
        if (m_connections.size() >= m_config.max_connections) {
            // 超出上限：立即关闭，避免 fd 留在半开状态（JinShan AX-023）。
            ++m_stats.rejected;
            (void)::close(client_fd);
            continue;
        }
        if (!SetNonBlocking(client_fd)) {
            ++m_stats.rejected;
            (void)::close(client_fd);
            continue;
        }
        // 代理承载的多是握手/控制帧等小包；不关 Nagle 会被延迟 ACK 压成
        // 每轮约 40ms。与 `Socks5Server::AcceptReady()` 的 accept 路径一致，
        // 失败只计数不拒绝连接（B-02）。
        if (m_config.tcp_nodelay) {
            if (SetTcpNoDelayCompat(client_fd)) {
                ++m_stats.nodelay_applied;
            } else {
                ++m_stats.nodelay_failed;
            }
        }
        const int target_fd = ::socket(m_target_addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (target_fd < 0) {
            ++m_stats.dial_failed;
            (void)::close(client_fd);
            continue;
        }
        if (!SetNonBlocking(target_fd)) {
            ++m_stats.dial_failed;
            (void)::close(target_fd);
            (void)::close(client_fd);
            continue;
        }
        const int rc = ::connect(target_fd, reinterpret_cast<const sockaddr*>(&m_target_addr),
                                 m_target_addr_len);
        if (rc != 0 && errno != EINPROGRESS) {
            // 立即失败（如 ECONNREFUSED）：与异步失败走同一条清理路径。
            ++m_stats.dial_failed;
            (void)::close(target_fd);
            (void)::close(client_fd);
            continue;
        }
        auto connection = std::make_unique<Connection>();
        connection->id = m_next_connection_id++;
        connection->client_fd = client_fd;
        connection->target_fd = target_fd;
        connection->a_buffer.resize(m_config.buffer_bytes);
        connection->b_buffer.resize(m_config.buffer_bytes);
        const std::uint64_t now = NowMs();
        connection->last_activity_ms = now;
        connection->connecting = (rc != 0);
        if (connection->connecting) {
            connection->connect_deadline_ms = Deadline(now, m_config.connect_timeout_ms);
        } else {
            ++m_stats.dial_ok;
        }
        const std::uint64_t id = connection->id;
        const bool connected_now = !connection->connecting;
        m_connections.push_back(std::move(connection));
        if (on_accept) {
            on_accept(id);
        }
        if (connected_now && on_connected) {
            on_connected(id);
        }
    }
}

int TcpProxy::PumpDirection(int src_fd, int dst_fd, std::uint8_t* buffer, std::size_t capacity,
                            std::size_t* pending_bytes, std::size_t* pending_offset, bool* eof) {
    int moved = 0;
    // 1) 先续写上次没写完的数据；写不动（EAGAIN）就保持 pending，等下一轮。
    if (*pending_bytes > *pending_offset) {
        const ssize_t written =
            ::write(dst_fd, buffer + *pending_offset, *pending_bytes - *pending_offset);
        if (written > 0) {
            *pending_offset += static_cast<std::size_t>(written);
            moved += static_cast<int>(written);
            if (*pending_offset >= *pending_bytes) {
                *pending_bytes = 0;
                *pending_offset = 0;
            }
        } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
    }
    // 2) 缓冲排空后才继续读，形成背压（慢对端不会把内存吃光）。
    if (*pending_bytes == 0 && !*eof) {
        const ssize_t received = ::read(src_fd, buffer, capacity);
        if (received > 0) {
            *pending_bytes = static_cast<std::size_t>(received);
            *pending_offset = 0;
            const ssize_t written = ::write(dst_fd, buffer, *pending_bytes);
            if (written > 0) {
                *pending_offset = static_cast<std::size_t>(written);
                moved += static_cast<int>(written);
                if (*pending_offset >= *pending_bytes) {
                    *pending_bytes = 0;
                    *pending_offset = 0;
                }
            } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                return -1;
            }
        } else if (received == 0) {
            *eof = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
    }
    return moved;
}

int TcpProxy::ServiceConnection(std::size_t index, std::uint64_t now_ms) {
    Connection& connection = *m_connections[index];
    int moved = 0;

    if (connection.connecting) {
        // 非阻塞 connect 完成判定：可写 + SO_ERROR == 0。
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(connection.target_fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
            socket_error = errno;
        }
        if (socket_error == 0) {
            connection.connecting = false;
            ++m_stats.dial_ok;
            connection.last_activity_ms = now_ms;
            if (on_connected) {
                on_connected(connection.id);
            }
        } else if (socket_error == EINPROGRESS || socket_error == EALREADY) {
            if (m_config.connect_timeout_ms != 0 && now_ms >= connection.connect_deadline_ms) {
                ++m_stats.connect_timeouts;
                CloseConnection(index, "dial");
                return 0;
            }
            return 0;
        } else {
            CloseConnection(index, "dial");
            return 0;
        }
    }

    // A: client → target
    const int moved_a = PumpDirection(connection.client_fd, connection.target_fd,
                                      connection.a_buffer.data(), connection.a_buffer.size(),
                                      &connection.a_len, &connection.a_off, &connection.a_eof);
    if (moved_a < 0) {
        CloseConnection(index, "reset");
        return 0;
    }
    // B: target → client
    const int moved_b = PumpDirection(connection.target_fd, connection.client_fd,
                                      connection.b_buffer.data(), connection.b_buffer.size(),
                                      &connection.b_len, &connection.b_off, &connection.b_eof);
    if (moved_b < 0) {
        CloseConnection(index, "reset");
        return 0;
    }
    moved = moved_a + moved_b;
    if (moved_a > 0) {
        m_stats.bytes_to_target += static_cast<std::uint64_t>(moved_a);
    }
    if (moved_b > 0) {
        m_stats.bytes_to_client += static_cast<std::uint64_t>(moved_b);
    }
    if (moved > 0) {
        connection.last_activity_ms = now_ms;
    }

    // 半关闭传播：本方向 EOF 且缓冲已排空 → 对下游 SHUT_WR。
    // 这里不能直接 close：请求-响应协议的对端往往还要回一大段数据。
    if (connection.a_eof && !connection.a_shutdown && !connection.PendingA()) {
        (void)::shutdown(connection.target_fd, SHUT_WR);
        connection.a_shutdown = true;
        ++m_stats.half_closes;
        if (connection.half_close_at_ms == 0) {
            connection.half_close_at_ms = now_ms;
        }
    }
    if (connection.b_eof && !connection.b_shutdown && !connection.PendingB()) {
        (void)::shutdown(connection.client_fd, SHUT_WR);
        connection.b_shutdown = true;
        ++m_stats.half_closes;
        if (connection.half_close_at_ms == 0) {
            connection.half_close_at_ms = now_ms;
        }
    }

    if (connection.Finished()) {
        CloseConnection(index, "eof");
        return moved;
    }
    // 半关闭后对端迟迟不结束：强关，避免 fd 泄漏（JinShan AX-020）。
    if (connection.half_close_at_ms != 0 && m_config.half_close_timeout_ms != 0 &&
        now_ms - connection.half_close_at_ms >= m_config.half_close_timeout_ms) {
        CloseConnection(index, "linger");
        return moved;
    }
    if (m_config.idle_timeout_ms != 0 &&
        now_ms - connection.last_activity_ms >= m_config.idle_timeout_ms) {
        CloseConnection(index, "idle");
        return moved;
    }
    return moved;
}

int TcpProxy::Poll(int timeout_ms) {
    if (m_listen_fd < 0) {
        return 0;
    }
    std::vector<pollfd> fds;
    fds.reserve(1 + m_connections.size() * 2);
    pollfd listen_entry{};
    listen_entry.fd = m_listen_fd;
    listen_entry.events = POLLIN;
    fds.push_back(listen_entry);

    for (const auto& connection : m_connections) {
        pollfd client_entry{};
        client_entry.fd = connection->client_fd;
        client_entry.events = 0;
        if (connection->CanReadClient()) {
            client_entry.events |= POLLIN;
        }
        if (connection->PendingB()) {
            client_entry.events |= POLLOUT;
        }
        fds.push_back(client_entry);

        pollfd target_entry{};
        target_entry.fd = connection->target_fd;
        target_entry.events = 0;
        if (connection->connecting) {
            target_entry.events |= POLLOUT;
        } else if (connection->CanReadTarget()) {
            target_entry.events |= POLLIN;
        }
        if (connection->PendingA()) {
            target_entry.events |= POLLOUT;
        }
        fds.push_back(target_entry);
    }

    const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), std::max(timeout_ms, 0));
    if (ready < 0 && errno != EINTR) {
        return 0;
    }
    const std::uint64_t now = NowMs();
    if (ready > 0 && (fds[0].revents & POLLIN) != 0) {
        AcceptReady();
    }
    // 倒序遍历：CloseConnection 会 erase，倒序不会打乱未处理项的下标。
    // 注意 AcceptReady 可能往尾部追加连接，这些新连接在 fds 里没有槽位，
    // 用 askip 统一按「无事件」处理，下一轮 Poll 才正式参与。
    const std::size_t polled = (fds.size() - 1) / 2;
    int moved = 0;
    for (std::size_t i = m_connections.size(); i > 0; --i) {
        const std::size_t index = i - 1;
        if (index >= polled) {
            continue;
        }
        const std::size_t client_slot = 1 + index * 2;
        const short client_events = fds[client_slot].revents;
        const short target_events = fds[client_slot + 1].revents;
        const Connection& snapshot = *m_connections[index];
        const bool interesting = client_events != 0 || target_events != 0 || snapshot.PendingA() ||
                                 snapshot.PendingB() || snapshot.connecting;
        if (interesting) {
            moved += ServiceConnection(index, now);
            continue;
        }
        // 没有事件也要走超时检查：idle/linger 与流量无关。
        if (m_config.idle_timeout_ms != 0 &&
            now - snapshot.last_activity_ms >= m_config.idle_timeout_ms) {
            CloseConnection(index, "idle");
        } else if (snapshot.half_close_at_ms != 0 && m_config.half_close_timeout_ms != 0 &&
                   now - snapshot.half_close_at_ms >= m_config.half_close_timeout_ms) {
            CloseConnection(index, "linger");
        }
    }
    return moved;
}

void TcpProxy::RunFor(int duration_ms) {
    const int clamped = std::max(duration_ms, 0);
    const std::uint64_t start = NowMs();
    for (;;) {
        const std::uint64_t elapsed = NowMs() - start;
        if (elapsed >= static_cast<std::uint64_t>(clamped)) {
            break;
        }
        const int remaining = clamped - static_cast<int>(elapsed);
        (void)Poll(std::min(remaining, 10));
    }
}

}  // namespace go2cpp::bento
