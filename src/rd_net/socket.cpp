#include "go2cpp/rd_net/socket.hpp"

/// @file socket.cpp
/// @brief REQ-006 的平台实现：绑定 go2cpp::net::Socket + 非阻塞四态错误映射。
///
/// @details 生命周期/地址/bind/connect/close 全部走仓库现有的
///          `go2cpp::net::Socket`；收发用它的 `fd()` 做 MSG_DONTWAIT 系统调用，
///          从而同时保留内核级四态（kClosed/kReset/kTruncated/kWouldBlock）。
///          批量写（`batch_bytes > 0`）复用 `SocketBatchWriter`：
///          TCP → `writev`，UDP → `sendmmsg`。

#if !defined(_WIN32)

#include "go2cpp/network/address.hpp"
#include "go2cpp/network/bufio_socket.hpp"
#include "go2cpp/network/socket.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace rd::net {
namespace {

using go2cpp::net::Address;
using go2cpp::net::AddressPtr;
using go2cpp::net::Socket;

/// @brief 单次批量提交允许的最大段数（栈上数组上限）。
constexpr std::size_t kMaxBatchEntries = 64;

/// @brief 解析 IP 字面量（快速路径，避免热路径反复 getaddrinfo）。
bool ParseIpLiteral(const Endpoint& ep, int sock_type, sockaddr_storage* out,
                    socklen_t* out_len) {
    if (ep.host.empty()) {
        return false;
    }
    sockaddr_in v4{};
    if (::inet_pton(AF_INET, ep.host.c_str(), &v4.sin_addr) == 1) {
        v4.sin_family = AF_INET;
        v4.sin_port = htons(ep.port);
        std::memcpy(out, &v4, sizeof(v4));
        *out_len = static_cast<socklen_t>(sizeof(v4));
        (void)sock_type;
        return true;
    }
    sockaddr_in6 v6{};
    if (::inet_pton(AF_INET6, ep.host.c_str(), &v6.sin6_addr) == 1) {
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(ep.port);
        std::memcpy(out, &v6, sizeof(v6));
        *out_len = static_cast<socklen_t>(sizeof(v6));
        return true;
    }
    return false;
}

/// @brief 把 Endpoint 解析成 sockaddr；passive=true 用于 bind。
bool ResolveEndpoint(const Endpoint& ep, int sock_type, bool passive,
                     sockaddr_storage* out, socklen_t* out_len, int* error) {
    if (ParseIpLiteral(ep, sock_type, out, out_len)) {
        return true;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = sock_type;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    char service[16];
    std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(ep.port));
    const char* node = ep.host.empty() ? nullptr : ep.host.c_str();
    addrinfo* list = nullptr;
    const int rc = ::getaddrinfo(node, service, &hints, &list);
    if (rc != 0) {
        *error = rc;
        return false;
    }
    bool copied = false;
    for (const addrinfo* it = list; it != nullptr; it = it->ai_next) {
        if (it->ai_addrlen <= sizeof(sockaddr_storage)) {
            std::memcpy(out, it->ai_addr, it->ai_addrlen);
            *out_len = static_cast<socklen_t>(it->ai_addrlen);
            copied = true;
            break;
        }
    }
    ::freeaddrinfo(list);
    if (!copied) {
        *error = EAI_FAMILY;
    }
    return copied;
}

/// @brief sockaddr → go2cpp 的地址类型（IPv4/IPv6/Unix/Unknown 由它分派）。
AddressPtr MakeAddress(const sockaddr_storage& storage, socklen_t length) {
    return Address::Create(reinterpret_cast<const sockaddr*>(&storage), length);
}

/// @brief go2cpp 地址类型 → rd::net::Endpoint（走 addr()/addr_len() 访问器）。
Endpoint EndpointFromSockaddr(const sockaddr* sa, socklen_t len) {
    Endpoint ep;
    if (sa == nullptr) {
        return ep;
    }
    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    if (::getnameinfo(sa, len, host, sizeof(host), service, sizeof(service),
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return ep;
    }
    ep.host = host;
    ep.port = static_cast<std::uint16_t>(std::strtoul(service, nullptr, 10));
    return ep;
}

Endpoint EndpointFromAddress(const AddressPtr& address) {
    if (!address) {
        return Endpoint{};
    }
    return EndpointFromSockaddr(address->addr(), address->addr_len());
}

/// @brief 设置整数 socket 选项（走 go2cpp Socket，失败时 errno 由内核保留）。
bool SetIntOption(Socket& socket, int level, int name, int value) {
    return socket.set_option(level, name, &value, sizeof(value));
}

/// @brief 非阻塞可写探测：用于在批量聚合前把 kWouldBlock 透出。
/// @return 1=可写，0=不可写（EAGAIN），-1=错误（errno 有效）。
int ProbeWritable(int fd) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int rc = ::poll(&pfd, 1, 0);
    if (rc > 0) {
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            // 先让调用方按需触发一次 sendto/send 取得精确 errno，这里不算错误。
            return 1;
        }
        return (pfd.revents & POLLOUT) != 0 ? 1 : 0;
    }
    return rc == 0 ? 0 : -1;
}

/// @brief 收发 errno → IoStatus（EINTR 已在调用点重试）。
IoStatus MapSendErrno(int err) {
    switch (err) {
        case EAGAIN:
            return IoStatus::kWouldBlock;
        case EPIPE:
            return IoStatus::kClosed;
        case ECONNRESET:
        case ECONNREFUSED:
            return IoStatus::kReset;
        case ETIMEDOUT:
            return IoStatus::kTimeout;
        default:
            return IoStatus::kIoError;
    }
}

IoStatus MapRecvErrno(int err) {
    switch (err) {
        case EAGAIN:
            return IoStatus::kWouldBlock;
        case ECONNRESET:
        case ECONNREFUSED:
            return IoStatus::kReset;
        case ETIMEDOUT:
            return IoStatus::kTimeout;
        default:
            return IoStatus::kIoError;
    }
}

IoStatus MapConnectErrno(int err) {
    switch (err) {
        case ECONNREFUSED:
        case ECONNRESET:
            return IoStatus::kReset;
        case ETIMEDOUT:
            return IoStatus::kTimeout;
        default:
            return IoStatus::kIoError;
    }
}

}  // namespace

const char* IoStatusName(IoStatus status) noexcept {
    switch (status) {
        case IoStatus::kOk:
            return "kOk";
        case IoStatus::kWouldBlock:
            return "kWouldBlock";
        case IoStatus::kTimeout:
            return "kTimeout";
        case IoStatus::kClosed:
            return "kClosed";
        case IoStatus::kReset:
            return "kReset";
        case IoStatus::kTruncated:
            return "kTruncated";
        case IoStatus::kIoError:
            return "kIoError";
        case IoStatus::kInvalid:
            return "kInvalid";
    }
    return "Unknown";
}

// ---------------------------------------------------------------- DatagramSocket

DatagramSocket::~DatagramSocket() { Close(); }

DatagramSocket::DatagramSocket(DatagramSocket&& other) noexcept
    : fd_(other.fd_),
      local_(std::move(other.local_)),
      next_token_(other.next_token_),
      last_error_(other.last_error_),
      socket_(std::move(other.socket_)),
      batch_bytes_(other.batch_bytes_),
      batch_entries_(other.batch_entries_),
      enable_sendmmsg_(other.enable_sendmmsg_) {
    other.fd_ = -1;
    other.next_token_ = 0;
    other.last_error_ = 0;
    other.batch_bytes_ = 0;
    other.batch_entries_ = 0;
    other.enable_sendmmsg_ = false;
}

DatagramSocket& DatagramSocket::operator=(DatagramSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        local_ = std::move(other.local_);
        next_token_ = other.next_token_;
        last_error_ = other.last_error_;
        socket_ = std::move(other.socket_);
        batch_bytes_ = other.batch_bytes_;
        batch_entries_ = other.batch_entries_;
        enable_sendmmsg_ = other.enable_sendmmsg_;
        other.fd_ = -1;
        other.next_token_ = 0;
        other.last_error_ = 0;
        other.batch_bytes_ = 0;
        other.batch_entries_ = 0;
        other.enable_sendmmsg_ = false;
    }
    return *this;
}

Result<DatagramSocket> DatagramSocket::Open(const Endpoint& local,
                                            const SocketOptions& options) {
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    int resolve_err = 0;
    if (!ResolveEndpoint(local, SOCK_DGRAM, true, &addr, &addr_len, &resolve_err)) {
        return Result<DatagramSocket>::Err(IoStatus::kInvalid, resolve_err);
    }
    AddressPtr bind_address = MakeAddress(addr, addr_len);
    if (!bind_address) {
        return Result<DatagramSocket>::Err(IoStatus::kInvalid, EINVAL);
    }
    Socket::Ptr socket = Socket::CreateUDP(addr.ss_family);
    if (!socket) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, errno);
    }
    if (options.reuse_address && !SetIntOption(*socket, SOL_SOCKET, SO_REUSEADDR, 1)) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, errno);
    }
    if (options.broadcast && !SetIntOption(*socket, SOL_SOCKET, SO_BROADCAST, 1)) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, errno);
    }
    if (options.recv_buffer_bytes != 0 &&
        !SetIntOption(*socket, SOL_SOCKET, SO_RCVBUF,
                      static_cast<int>(options.recv_buffer_bytes))) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, errno);
    }
    if (options.send_buffer_bytes != 0 &&
        !SetIntOption(*socket, SOL_SOCKET, SO_SNDBUF,
                      static_cast<int>(options.send_buffer_bytes))) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, errno);
    }
    if (!socket->bind(*bind_address)) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, errno);
    }
    DatagramSocket sock;
    sock.socket_ = std::move(socket);
    sock.fd_ = sock.socket_->fd();
    if (sock.fd_ < 0) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, EBADF);
    }
    sock.local_ = EndpointFromAddress(sock.socket_->local_address());
    sock.batch_bytes_ = options.batch_bytes;
    sock.batch_entries_ = options.batch_entries;
    sock.enable_sendmmsg_ = options.enable_sendmmsg;
    return Result<DatagramSocket>::Ok(std::move(sock));
}

IoStatus DatagramSocket::SendTo(ByteView data, const Endpoint& to, SendResult* out) {
    SendResult result;
    result.token = ++next_token_;
    result.status = IoStatus::kInvalid;
    last_error_ = 0;
    if (out != nullptr) {
        *out = result;
    }
    if (!valid() || (data.data == nullptr && data.size != 0) ||
        data.size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        last_error_ = EINVAL;
        if (out != nullptr) {
            out->status = IoStatus::kInvalid;
        }
        return IoStatus::kInvalid;
    }
    sockaddr_storage dst{};
    socklen_t dst_len = 0;
    int resolve_err = 0;
    if (!ResolveEndpoint(to, SOCK_DGRAM, false, &dst, &dst_len, &resolve_err)) {
        last_error_ = resolve_err;
        if (out != nullptr) {
            out->status = IoStatus::kInvalid;
        }
        return IoStatus::kInvalid;
    }
    const int fd = socket_ ? socket_->fd() : -1;
    if (fd < 0) {
        last_error_ = EBADF;
        return IoStatus::kInvalid;
    }
    // ---- 批量路径：复用 go2cpp 的 SocketBatchWriter（sendmmsg）----
    // UDP 的「一条数据报」不可切分，否则接收端会看到多条报文、破坏
    // 「全有或全无」语义；因此这里用 sendmmsg 提交**单条**（count=1），
    // 由批量适配器统一超时/错误处理。可写性仍先由内核 poll 探测，
    // 保证 EAGAIN 能原样透出为 kWouldBlock，而不是被内部重试吞掉。
    if (batch_bytes_ > 0 && enable_sendmmsg_ && data.size >= batch_bytes_) {
        const int writable = ProbeWritable(fd);
        if (writable == 0) {
            last_error_ = 0;
            result.status = IoStatus::kWouldBlock;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kWouldBlock;
        }
        if (writable < 0) {
            last_error_ = errno;
            result.status = IoStatus::kIoError;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kIoError;
        }
        go2cpp::net::SendBatchEntry entry{};
        entry.data = data.data;
        entry.length = data.size;
        entry.to = reinterpret_cast<const sockaddr*>(&dst);
        entry.to_length = dst_len;
        // timeout=0：不允许内部 park/重试，与四态的非阻塞语义一致。
        go2cpp::net::SocketBatchWriter writer(socket_,
                                              std::chrono::milliseconds(0), true);
        const go2cpp::net::SocketResult sent = writer.SendToBatch(&entry, 1);
        if (sent.transferred == 1) {
            result.status = IoStatus::kOk;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kOk;
        }
        const int err = sent.error != 0 ? sent.error : EAGAIN;
        last_error_ = err;
        result.status = MapSendErrno(err);
        if (out != nullptr) {
            *out = result;
        }
        return result.status;
    }
    for (;;) {
        const ssize_t n =
            ::sendto(fd, data.data, data.size, MSG_DONTWAIT | MSG_NOSIGNAL,
                     reinterpret_cast<const sockaddr*>(&dst), dst_len);
        if (n >= 0) {
            if (static_cast<std::size_t>(n) != data.size) {
                // UDP 不产生部分发送；若内核仍返回短写，按错误上报，避免静默丢包。
                last_error_ = 0;
                if (out != nullptr) {
                    out->status = IoStatus::kIoError;
                }
                return IoStatus::kIoError;
            }
            result.status = IoStatus::kOk;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kOk;
        }
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        last_error_ = err;
        result.status = MapSendErrno(err);
        if (out != nullptr) {
            *out = result;
        }
        return result.status;
    }
}

IoStatus DatagramSocket::RecvFrom(MutableByteView buf, Endpoint* from,
                                  std::size_t* received) {
    if (received != nullptr) {
        *received = 0;
    }
    last_error_ = 0;
    const int fd = socket_ ? socket_->fd() : -1;
    if (!valid() || fd < 0 || buf.data == nullptr || buf.size == 0) {
        last_error_ = EINVAL;
        return IoStatus::kInvalid;
    }
    iovec iov{};
    iov.iov_base = buf.data;
    iov.iov_len = buf.size;
    sockaddr_storage src{};
    msghdr msg{};
    msg.msg_name = &src;
    msg.msg_namelen = static_cast<socklen_t>(sizeof(src));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    for (;;) {
        // 传入 MSG_TRUNC：报文大于缓冲时返回值是**实际报文长度**，
        // 同时 msg_flags 置 MSG_TRUNC。这样「截断」与「成功」可区分。
        const ssize_t n = ::recvmsg(fd, &msg, MSG_DONTWAIT | MSG_TRUNC);
        if (n >= 0) {
            const std::size_t actual = static_cast<std::size_t>(n);
            if (from != nullptr) {
                *from = EndpointFromSockaddr(reinterpret_cast<const sockaddr*>(&src),
                                             msg.msg_namelen);
            }
            if (received != nullptr) {
                *received = actual;
            }
            if ((msg.msg_flags & MSG_TRUNC) != 0 || actual > buf.size) {
                return IoStatus::kTruncated;
            }
            return IoStatus::kOk;
        }
        const int err = errno;
        if (err == EINTR) {
            msg.msg_namelen = static_cast<socklen_t>(sizeof(src));
            msg.msg_flags = 0;
            continue;
        }
        last_error_ = err;
        if (err == EAGAIN) {
            last_error_ = 0;
        }
        return MapRecvErrno(err);
    }
}

void DatagramSocket::Close() noexcept {
    if (socket_) {
        (void)socket_->close();
        socket_.reset();
    }
    fd_ = -1;
    next_token_ = 0;
}

// ---------------------------------------------------------------- StreamSocket

StreamSocket::~StreamSocket() { Close(); }

StreamSocket::StreamSocket(StreamSocket&& other) noexcept
    : fd_(other.fd_),
      peer_(std::move(other.peer_)),
      local_(std::move(other.local_)),
      next_token_(other.next_token_),
      last_transferred_(other.last_transferred_),
      last_error_(other.last_error_),
      socket_(std::move(other.socket_)),
      batch_bytes_(other.batch_bytes_),
      batch_entries_(other.batch_entries_),
      enable_writev_(other.enable_writev_) {
    other.fd_ = -1;
    other.next_token_ = 0;
    other.last_transferred_ = 0;
    other.last_error_ = 0;
    other.batch_bytes_ = 0;
    other.batch_entries_ = 0;
    other.enable_writev_ = false;
}

StreamSocket& StreamSocket::operator=(StreamSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        peer_ = std::move(other.peer_);
        local_ = std::move(other.local_);
        next_token_ = other.next_token_;
        last_transferred_ = other.last_transferred_;
        last_error_ = other.last_error_;
        socket_ = std::move(other.socket_);
        batch_bytes_ = other.batch_bytes_;
        batch_entries_ = other.batch_entries_;
        enable_writev_ = other.enable_writev_;
        other.fd_ = -1;
        other.next_token_ = 0;
        other.last_transferred_ = 0;
        other.last_error_ = 0;
        other.batch_bytes_ = 0;
        other.batch_entries_ = 0;
        other.enable_writev_ = false;
    }
    return *this;
}

Result<StreamSocket> StreamSocket::Open(const Endpoint& remote,
                                        const DialOptions& options) {
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    int resolve_err = 0;
    if (!ResolveEndpoint(remote, SOCK_STREAM, false, &addr, &addr_len, &resolve_err)) {
        return Result<StreamSocket>::Err(IoStatus::kInvalid, resolve_err);
    }
    AddressPtr remote_address = MakeAddress(addr, addr_len);
    if (!remote_address) {
        return Result<StreamSocket>::Err(IoStatus::kInvalid, EINVAL);
    }
    Socket::Ptr socket = Socket::CreateTCP(addr.ss_family);
    if (!socket) {
        return Result<StreamSocket>::Err(IoStatus::kIoError, errno);
    }
    if (options.tcp_no_delay &&
        !SetIntOption(*socket, IPPROTO_TCP, TCP_NODELAY, 1)) {
        return Result<StreamSocket>::Err(IoStatus::kIoError, errno);
    }
    if (options.recv_buffer_bytes != 0 &&
        !SetIntOption(*socket, SOL_SOCKET, SO_RCVBUF,
                      static_cast<int>(options.recv_buffer_bytes))) {
        return Result<StreamSocket>::Err(IoStatus::kIoError, errno);
    }
    if (options.send_buffer_bytes != 0 &&
        !SetIntOption(*socket, SOL_SOCKET, SO_SNDBUF,
                      static_cast<int>(options.send_buffer_bytes))) {
        return Result<StreamSocket>::Err(IoStatus::kIoError, errno);
    }
    std::chrono::milliseconds budget = options.connect_timeout;
    if (budget.count() <= 0) {
        budget = std::chrono::milliseconds(1);
    }
    if (!socket->connect(*remote_address, budget)) {
        const int err = errno;
        return Result<StreamSocket>::Err(MapConnectErrno(err), err);
    }
    StreamSocket sock;
    sock.socket_ = std::move(socket);
    sock.fd_ = sock.socket_->fd();
    if (sock.fd_ < 0) {
        return Result<StreamSocket>::Err(IoStatus::kIoError, EBADF);
    }
    sock.peer_ = EndpointFromAddress(sock.socket_->remote_address());
    sock.local_ = EndpointFromAddress(sock.socket_->local_address());
    sock.batch_bytes_ = options.batch_bytes;
    sock.batch_entries_ = options.batch_entries;
    sock.enable_writev_ = options.enable_writev;
    return Result<StreamSocket>::Ok(std::move(sock));
}

IoStatus StreamSocket::Send(ByteView data, SendResult* out) {
    SendResult result;
    result.token = ++next_token_;
    result.status = IoStatus::kInvalid;
    last_transferred_ = 0;
    last_error_ = 0;
    if (out != nullptr) {
        *out = result;
    }
    if (!valid() || (data.data == nullptr && data.size != 0) ||
        data.size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        last_error_ = EINVAL;
        if (out != nullptr) {
            out->status = IoStatus::kInvalid;
        }
        return IoStatus::kInvalid;
    }
    const int fd = socket_ ? socket_->fd() : -1;
    if (fd < 0) {
        last_error_ = EBADF;
        return IoStatus::kInvalid;
    }
    // ---- 批量路径：复用 go2cpp 的 SocketBatchWriter（writev）----
    // 把本次负载按 batch_bytes 均分成 ≤ batch_entries 个 iovec，用**一次**
    // writev 提交；TCP 是字节流，切段不改变语义，partial write 仍由
    // last_transferred() 如实上报。可写性先由 poll 探测，保证 EAGAIN
    // 原样透出为 kWouldBlock，不会被内部重试吞掉。
    if (batch_bytes_ > 0 && enable_writev_ && data.size >= batch_bytes_) {
        const int writable = ProbeWritable(fd);
        if (writable == 0) {
            last_error_ = 0;
            result.status = IoStatus::kWouldBlock;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kWouldBlock;
        }
        if (writable < 0) {
            last_error_ = errno;
            result.status = IoStatus::kIoError;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kIoError;
        }
        std::size_t count = (data.size + batch_bytes_ - 1) / batch_bytes_;
        const std::size_t limit =
            batch_entries_ > 0 ? std::min<std::size_t>(batch_entries_, kMaxBatchEntries)
                               : 1;
        if (count > limit) {
            count = limit;
        }
        if (count == 0) {
            count = 1;
        }
        go2cpp::bufio::BatchEntry entries[kMaxBatchEntries];
        const std::size_t seg = (data.size + count - 1) / count;
        std::size_t used = 0;
        for (std::size_t i = 0; i < count && used < kMaxBatchEntries; ++i) {
            const std::size_t offset = i * seg;
            if (offset >= data.size) {
                break;
            }
            const std::size_t length = std::min(seg, data.size - offset);
            entries[used].data = reinterpret_cast<const char*>(data.data + offset);
            entries[used].size = length;
            ++used;
        }
        go2cpp::net::SocketBatchWriter writer(socket_,
                                              std::chrono::milliseconds(0), false);
        const go2cpp::streams::WriteResult written = writer.WriteBatch(entries, used);
        if (written.error == nullptr) {
            last_transferred_ = written.count;
            result.status = IoStatus::kOk;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kOk;
        }
        last_transferred_ = written.count;
        const int err = writer.last_system_error() != 0 ? writer.last_system_error()
                                                        : EIO;
        last_error_ = err;
        result.status = MapSendErrno(err);
        if (out != nullptr) {
            *out = result;
        }
        return result.status;
    }
    for (;;) {
        const ssize_t n = ::send(fd, data.data, data.size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n >= 0) {
            last_transferred_ = static_cast<std::size_t>(n);
            result.status = IoStatus::kOk;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kOk;
        }
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        last_error_ = err;
        result.status = MapSendErrno(err);
        if (out != nullptr) {
            *out = result;
        }
        return result.status;
    }
}

IoStatus StreamSocket::Recv(MutableByteView buf, std::size_t* received) {
    if (received != nullptr) {
        *received = 0;
    }
    last_transferred_ = 0;
    last_error_ = 0;
    const int fd = socket_ ? socket_->fd() : -1;
    if (!valid() || fd < 0 || buf.data == nullptr || buf.size == 0) {
        last_error_ = EINVAL;
        return IoStatus::kInvalid;
    }
    for (;;) {
        const ssize_t n = ::recv(fd, buf.data, buf.size, MSG_DONTWAIT);
        if (n > 0) {
            last_transferred_ = static_cast<std::size_t>(n);
            if (received != nullptr) {
                *received = static_cast<std::size_t>(n);
            }
            return IoStatus::kOk;
        }
        if (n == 0) {
            // 有序关闭：对端发了 FIN，之后不会再有数据。
            return IoStatus::kClosed;
        }
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        last_error_ = err;
        if (err == EAGAIN) {
            last_error_ = 0;
        }
        return MapRecvErrno(err);
    }
}

void StreamSocket::Close() noexcept {
    if (socket_) {
        (void)socket_->close();
        socket_.reset();
    }
    fd_ = -1;
    next_token_ = 0;
    last_transferred_ = 0;
}

}  // namespace rd::net

#else  // _WIN32：只保留接口占位，不引入 Windows 头，由 JinShan 侧适配 WSA。

namespace rd::net {

const char* IoStatusName(IoStatus) noexcept { return "kInvalid"; }

DatagramSocket::~DatagramSocket() = default;
DatagramSocket::DatagramSocket(DatagramSocket&&) noexcept = default;
DatagramSocket& DatagramSocket::operator=(DatagramSocket&&) noexcept = default;
Result<DatagramSocket> DatagramSocket::Open(const Endpoint&, const SocketOptions&) {
    return Result<DatagramSocket>::Err(IoStatus::kInvalid, 0);
}
IoStatus DatagramSocket::SendTo(ByteView, const Endpoint&, SendResult* out) {
    if (out != nullptr) {
        *out = SendResult{};
    }
    return IoStatus::kInvalid;
}
IoStatus DatagramSocket::RecvFrom(MutableByteView, Endpoint*, std::size_t* received) {
    if (received != nullptr) {
        *received = 0;
    }
    return IoStatus::kInvalid;
}
void DatagramSocket::Close() noexcept {}

StreamSocket::~StreamSocket() = default;
StreamSocket::StreamSocket(StreamSocket&&) noexcept = default;
StreamSocket& StreamSocket::operator=(StreamSocket&&) noexcept = default;
Result<StreamSocket> StreamSocket::Open(const Endpoint&, const DialOptions&) {
    return Result<StreamSocket>::Err(IoStatus::kInvalid, 0);
}
IoStatus StreamSocket::Send(ByteView, SendResult* out) {
    if (out != nullptr) {
        *out = SendResult{};
    }
    return IoStatus::kInvalid;
}
IoStatus StreamSocket::Recv(MutableByteView, std::size_t* received) {
    if (received != nullptr) {
        *received = 0;
    }
    return IoStatus::kInvalid;
}
void StreamSocket::Close() noexcept {}

}  // namespace rd::net

#endif  // _WIN32
