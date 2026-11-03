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
#include <vector>

namespace rd::net {
namespace {

using go2cpp::net::Address;
using go2cpp::net::AddressPtr;
using go2cpp::net::Socket;

/// @brief 单条 Send 隐式切段时栈上数组上限（显式批量入口用公开的 kMaxBatchEntries）。
constexpr std::size_t kSingleSendMaxSegments = 64;

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

/// @brief 计算「已写入 bytes 字节」完整覆盖的前缀段数（size==0 的段视为已提交）。
std::size_t CompletePrefixSegments(const go2cpp::bufio::BatchEntry* entries,
                                   std::size_t limit, std::size_t bytes) {
    std::size_t prefix = 0;
    std::size_t cumulative = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (entries[i].size == 0) {
            ++prefix;
            continue;
        }
        if (entries[i].size > bytes - cumulative) {
            break;
        }
        cumulative += entries[i].size;
        ++prefix;
    }
    return prefix;
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
      enable_sendmmsg_(other.enable_sendmmsg_),
      last_batch_submitted_(other.last_batch_submitted_),
      batch_target_(std::move(other.batch_target_)) {
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
        last_batch_submitted_ = other.last_batch_submitted_;
        batch_target_ = std::move(other.batch_target_);
        other.fd_ = -1;
        other.next_token_ = 0;
        other.last_error_ = 0;
        other.batch_bytes_ = 0;
        other.batch_entries_ = 0;
        other.enable_sendmmsg_ = false;
        other.last_batch_submitted_ = 0;
        other.batch_target_ = Endpoint{};
    }
    return *this;
}

/**
 * @brief 创建并绑定一个 UDP 数据报 socket，把选项与批量发送参数固化成 DatagramSocket。
 *
 * @param local [in] 本地绑定端点——借用，仅在本调用内读取；端口 0 表示由内核分配。
 * @param options [in] socket 选项——借用；reuse_address/broadcast 为真时才设对应 SO_*；缓冲大小为 0 表示不设。
 *
 * @return
 * - Result::Ok(DatagramSocket)：已创建并 bind 成功，fd 所有权移交返回对象——调用方**必须**负责 Close；
 * - Result::Err(kInvalid)：端点解析失败或地址构造失败——调用方**不要**重试同一端点，先修正地址文本/家族；
 * - Result::Err(kIoError)：socket/bind/选项失败，error 为 errno——调用方按 errno 决定是否换端口重试。
 *
 * ## 定位
 * UDP 数据面唯一的建链入口；fd 生命周期从这里开始，参数写死会影响该 socket 后续所有发送路径的语义。
 *
 * ## 步骤
 * step1. 把 Endpoint 解析成 sockaddr（失败即 kInvalid），并包装成 Address；
 * step2. 按地址族创建 UDP socket，失败返回 kIoError；
 * step3. 依序应用 REUSEADDR/BROADCAST/收发缓冲选项，任一失败立即失败（避免半配置的 socket 外泄）；
 * step4. bind 到解析出的本地地址，失败返回 kIoError；
 * step5. 组装 DatagramSocket：移交 socket、记录 fd/local 与批量发送参数，返回 Ok。
 *
 * ## 并发/失败语义
 * - 调用约束：建链期单线程使用；返回前对象未被发布，无并发读者。
 * - 原子性：任一步失败都会销毁已建 socket（RAII），不会留下未绑定的 fd。
 * - 失败语义：不抛异常；Err 时返回对象未构造，fd 已被析构路径关闭。
 * - 不变量：返回 Ok 时 valid() 为真、local() 是实际绑定地址；返回 Err 时无 fd 泄漏。
 */
Result<DatagramSocket> DatagramSocket::Open(const Endpoint& local,
                                            const SocketOptions& options) {
    // step1: 解析绑定端点（端口 0 由内核分配）；失败按 kInvalid 返回，不建 socket。
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
    // step2: 按地址族创建 UDP socket；失败按 kIoError 返回 errno。
    Socket::Ptr socket = Socket::CreateUDP(addr.ss_family);
    if (!socket) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, errno);
    }
    // step3: 依序应用 REUSEADDR/BROADCAST/收发缓冲；任一失败立即返回，避免半配置的 socket 外泄。
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
    // step4: bind 到解析出的本地地址；失败按 kIoError 返回，fd 由 RAII 关闭。
    if (!socket->bind(*bind_address)) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, errno);
    }
    DatagramSocket sock;
    sock.socket_ = std::move(socket);
    sock.fd_ = sock.socket_->fd();
    if (sock.fd_ < 0) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, EBADF);
    }
    // step5: 组装 DatagramSocket，记录实际绑定地址与批量发送参数后返回 Ok（fd 所有权移交返回对象）。
    sock.local_ = EndpointFromAddress(sock.socket_->local_address());
    sock.batch_bytes_ = options.batch_bytes;
    sock.batch_entries_ = options.batch_entries;
    sock.enable_sendmmsg_ = options.enable_sendmmsg;
    return Result<DatagramSocket>::Ok(std::move(sock));
}

/**
 * @brief 向指定端点发送一个不可切分的 UDP 数据报，返回四态结果。
 *
 * @param data [in] 载荷视图——借用；data 为 nullptr 时 size 必须为 0；size 超过 ssize_t 上限返回 kInvalid。
 * @param to [in] 目标端点——借用；每次调用都重新解析，并记入 batch_target_ 供 SendBatch 复用。
 * @param out [out] 发送结果——可为 nullptr；成功/失败都写入（含 token 与 status）；失败时 token 仍是本次分配值。
 *
 * @return
 * - IoStatus::kOk：整条数据报已提交给内核——调用方**必须**认为该报文已发出（UDP 无重传保证）；
 * - IoStatus::kWouldBlock：内核缓冲暂满——调用方**不要**忙等，应让出并按退避策略重试；
 * - IoStatus::kIoError/kInvalid：系统错误/参数错误——调用方**不要**原样重试，按 last_error() 处理。
 *
 * ## 定位
 * 单播数据面的唯一提交点，也是四态（Ok/WouldBlock/IoError/Invalid）映射的基准实现。
 * 写错会导致「部分发送被当成成功」或「EAGAIN 被吞成错误」，前者静默丢包、后者误杀连接。
 *
 * ## 步骤
 * step1. 初始化 SendResult 并分配 token，先把结果写回 out，保证任何早退都有可读状态；
 * step2. 校验 fd 有效性与载荷参数，非法直接 kInvalid；
 * step3. 解析目标端点；失败 kInvalid。解析成功后记录 batch_target_，供后续批量入口复用；
 * step4. 大包且启用 sendmmsg 时走批量适配器（count=1，timeout=0 不重试）：先探测可写性，把 EAGAIN 原样透出为 kWouldBlock；
 * step5. 普通路径循环 sendto(MSG_DONTWAIT|MSG_NOSIGNAL)：EINTR 重试，短写按 kIoError 上报（UDP 不允许部分发送），其余错误经 MapSendErrno 映射。
 *
 * ## 并发/失败语义
 * - 调用约束：同一 DatagramSocket 不保证并发安全（next_token_/batch_target_/last_error_ 无锁），需外部串行化。
 * - 原子性：单次 sendto 是一个数据报的原子提交；不存在半条报文可见的情况。
 * - 失败语义：不抛异常；失败时 last_error_ 为 errno（kWouldBlock 时清 0），socket 状态不变。
 * - 不变量：返回 kOk 时内核已接收整条数据报；返回 kWouldBlock 时一个字节都没发出。
 */
IoStatus DatagramSocket::SendTo(ByteView data, const Endpoint& to, SendResult* out) {
    SendResult result;
    // step1: 初始化 SendResult 并分配 token；先写回 out，保证任何早退都有可读状态。
    result.token = ++next_token_;
    result.status = IoStatus::kInvalid;
    last_error_ = 0;
    if (out != nullptr) {
        *out = result;
    }
    // step2: 校验 fd 与载荷参数（空指针只允许长度为 0），非法直接 kInvalid。
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
    // step3: 解析目标端点；失败 kInvalid。成功后记录 batch_target_ 供批量入口复用。
    if (!ResolveEndpoint(to, SOCK_DGRAM, false, &dst, &dst_len, &resolve_err)) {
        last_error_ = resolve_err;
        if (out != nullptr) {
            out->status = IoStatus::kInvalid;
        }
        return IoStatus::kInvalid;
    }
    // 记录最近一次解析出的目标，供 SendBatch 复用（不要求本次发送有结果）。
    batch_target_ = to;
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
    // step4: 大包走 sendmmsg 适配器（count=1、timeout=0）：先探测可写，EAGAIN 原样透出为 kWouldBlock。
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
            // step5: 普通路径循环 sendto；EINTR 重试，短写按 kIoError 上报（UDP 不允许部分发送），其余经 MapSendErrno 映射。
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

IoStatus DatagramSocket::SetBatchTarget(const Endpoint& to, SendResult* out) {
    last_error_ = 0;
    if (out != nullptr) {
        *out = SendResult{};
        out->status = IoStatus::kInvalid;
    }
    if (!valid() || to.host.empty() || to.port == 0) {
        last_error_ = EINVAL;
        return IoStatus::kInvalid;
    }
    sockaddr_storage dst{};
    socklen_t dst_len = 0;
    int resolve_err = 0;
    if (!ResolveEndpoint(to, SOCK_DGRAM, false, &dst, &dst_len, &resolve_err)) {
        last_error_ = resolve_err;
        return IoStatus::kInvalid;
    }
    batch_target_ = to;
    if (out != nullptr) {
        out->status = IoStatus::kOk;
    }
    return IoStatus::kOk;
}

/**
 * @brief 用 sendmmsg 把一个批次的 UDP 报文发往最近一次 SendTo/SetBatchTarget 记录的目标。
 *
 * @param entries [in] 批次数组——借用；长度由 count 给出，每项 data 非空；超过 kMaxBatchEntries 的部分被静默截断。
 * @param count [in] 条目数——0 是合法 no-op（仍分配 token）；>0 时 entries 不可为 nullptr。
 * @param out [out] 发送结果——可为 nullptr；写入 token/status，成功时 last_batch_submitted_ 记录实际提交条数。
 *
 * @return
 * - IoStatus::kOk：整批都已提交——调用方**必须**视为全部发出；
 * - IoStatus::kWouldBlock：部分或零条提交（见 last_batch_submitted_）——调用方**必须**从该下标起重试剩余部分；
 * - IoStatus::kInvalid：参数非法/无目标（EDESTADDRREQ）——调用方**不要**重试，先修正批次或先调 SetBatchTarget。
 *
 * ## 定位
 * 批量数据面的唯一提交点；截断与部分提交语义直接决定上层是否丢包。
 * 写错会让调用方把「只发了 3 条」当成「64 条都发了」，是最隐蔽的丢包来源。
 *
 * ## 步骤
 * step1. 初始化 token 与计数，count==0 直接成功返回（不产生系统调用）；
 * step2. 校验 valid/entries/每条 data 非空，非法即 kInvalid；
 * step3. 校验 batch_target_ 已就绪，否则 EDESTADDRREQ；
 * step4. 解析目标端点并构造 SendBatchEntry 数组（每条都指向同一目标）；
 * step5. 以 timeout=0 调 SocketBatchWriter：EAGAIN/ETIMEDOUT 映射 kWouldBlock，其余按 MapSendErrno；成功则按提交条数决定 Ok/WouldBlock。
 *
 * ## 并发/失败语义
 * - 调用约束：同 DatagramSocket 需外部串行化；batch_target_ 与 last_batch_submitted_ 无锁。
 * - 原子性：sendmmsg 逐条提交，整批不是原子的——部分成功必须通过 last_batch_submitted_ 观察。
 * - 失败语义：不抛异常；kWouldBlock 时 last_error_ 清 0，kInvalid 时写入具体 errno。
 * - 不变量：返回 kOk 时 last_batch_submitted_ == limit；返回 kWouldBlock 时它 < limit 且等于内核实际接受条数。
 */
IoStatus DatagramSocket::SendBatch(const go2cpp::bufio::BatchEntry* entries,
                                   std::size_t count, SendResult* out) {
    SendResult result;
    result.token = ++next_token_;
    result.status = IoStatus::kOk;
    last_batch_submitted_ = 0;
    last_error_ = 0;
    if (out != nullptr) {
        *out = result;
    }
    // step1: count==0 是合法 no-op：不产生系统调用，但仍分配 token 保持调用计数语义。
    if (count == 0) {
        // 合法 no-op：不产生系统调用，token 仍按调用分配。
        return IoStatus::kOk;
    }
    if (!valid() || entries == nullptr) {
        last_error_ = EINVAL;
        result.status = IoStatus::kInvalid;
        if (out != nullptr) {
            *out = result;
        }
        return IoStatus::kInvalid;
    }
    // step2: 校验 entries 与每条 data 非空，并把条目数截断到 kMaxBatchEntries。
    const std::size_t limit = std::min(count, kMaxBatchEntries);
    for (std::size_t i = 0; i < limit; ++i) {
        if (entries[i].data == nullptr) {
            last_error_ = EINVAL;
            result.status = IoStatus::kInvalid;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kInvalid;
        }
    }
    // step3: 批量入口没有目标参数，必须先 SetBatchTarget 或至少 SendTo 过一次，否则 EDESTADDRREQ。
    if (batch_target_.empty()) {
        // 批量入口没有目标参数：必须先 SetBatchTarget 或至少调用过一次 SendTo。
        last_error_ = EDESTADDRREQ;
        result.status = IoStatus::kInvalid;
        if (out != nullptr) {
            *out = result;
        }
        return IoStatus::kInvalid;
    }
    sockaddr_storage dst{};
    socklen_t dst_len = 0;
    int resolve_err = 0;
    if (!ResolveEndpoint(batch_target_, SOCK_DGRAM, false, &dst, &dst_len,
                         &resolve_err)) {
        last_error_ = resolve_err;
        result.status = IoStatus::kInvalid;
        if (out != nullptr) {
            *out = result;
        }
        return IoStatus::kInvalid;
    }
    // step4: 解析目标端点并构造 SendBatchEntry 数组，所有条目指向同一目标。
    std::vector<go2cpp::net::SendBatchEntry> batch;
    batch.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        go2cpp::net::SendBatchEntry entry{};
        entry.data = entries[i].data;
        entry.length = entries[i].size;
        entry.to = reinterpret_cast<const sockaddr*>(&dst);
        entry.to_length = dst_len;
        batch.push_back(entry);
    }
    // timeout=0：不等待、不重试，EAGAIN 原样透出（映射成 kWouldBlock）。
    go2cpp::net::SocketBatchWriter writer(socket_, std::chrono::milliseconds(0), true);
    // step5: timeout=0 调批量写入器：EAGAIN/ETIMEDOUT 映射 kWouldBlock，其余按 MapSendErrno；成功按提交条数定 Ok/WouldBlock。
    const go2cpp::net::SocketResult sent = writer.SendToBatch(batch.data(), batch.size());
    if (sent.error != 0 || sent.transferred < 0) {
        const int err = sent.error != 0 ? sent.error : EAGAIN;
        if (err == ETIMEDOUT || err == EAGAIN) {
            // 非阻塞入口：超时预算为 0 等价于「当前不可写」。
            last_error_ = 0;
            result.status = IoStatus::kWouldBlock;
        } else {
            last_error_ = err;
            result.status = MapSendErrno(err);
        }
        if (out != nullptr) {
            *out = result;
        }
        return result.status;
    }
    last_batch_submitted_ = static_cast<std::size_t>(sent.transferred);
    result.status = last_batch_submitted_ >= limit ? IoStatus::kOk
                                                   : IoStatus::kWouldBlock;
    if (out != nullptr) {
        *out = result;
    }
    return result.status;
}

/**
 * @brief 从一个 UDP socket 收取一条数据报，同时给出源端点和真实报文长度。
 *
 * @param buf [out] 接收缓冲——写入最多 buf.size 字节；data 为 nullptr 或 size==0 视为非法参数。
 * @param from [out] 源端点——可为 nullptr；成功时写入实际来源（含截断报文）。
 * @param received [out] 实际报文长度——可为 nullptr；成功与截断时都写入内核报告的真实长度。
 *
 * @return
 * - IoStatus::kOk：收到完整数据报——调用方**必须**只消费 received 字节；
 * - IoStatus::kTruncated：报文大于缓冲（MSG_TRUNC）——调用方**必须**按 received 扩容或丢弃，不要当成功处理；
 * - IoStatus::kWouldBlock：暂无数据——调用方**不要**忙等，交给事件循环/退避重试；
 * - IoStatus::kInvalid：参数或 fd 非法——调用方**不要**重试，修正用法。
 *
 * ## 定位
 * UDP 接收方向的唯一出口，也是「截断 vs 成功」判定的唯一位置。
 * 写错会把被截断的报文当完整报文交给上层，造成协议解析错位且无任何错误信号。
 *
 * ## 步骤
 * step1. 先把 received 置 0 并清 last_error_，再校验 fd 与缓冲参数，非法即 kInvalid；
 * step2. 组装 msghdr（单 iovec + 源地址缓冲），带 MSG_TRUNC 调 recvmsg，使返回值是真实报文长度；
 * step3. 成功时写入 from 与 received，并按 MSG_TRUNC/长度判断返回 kTruncated 或 kOk；
 * step4. EINTR 时复位 msg_namelen/msg_flags 后重试；其它错误经 MapRecvErrno 映射（EAGAIN 清 last_error_）。
 *
 * ## 并发/失败语义
 * - 调用约束：同 DatagramSocket 需外部串行化；buf 的有效期只需覆盖本次调用。
 * - 原子性：一次 recvmsg 取走整条数据报，不存在「半条报文留在内核缓冲」的情况。
 * - 失败语义：不抛异常；kWouldBlock 时 last_error_ 为 0，其它失败写入 errno。
 * - 不变量：返回 kTruncated 时 received 是真实报文长度；返回 kOk 时 received <= buf.size。
 */
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
    // step1: 先置 received=0 并清 last_error_，校验 fd/缓冲；随后组装单 iovec 的 msghdr。
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
        // step2: 带 MSG_TRUNC 收包，使返回值是真实报文长度，从而能区分「截断」与「成功」。
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
        // step3: EINTR 复位 msg 字段后重试；其它错误映射为四态（EAGAIN 清 last_error_ 后转 kWouldBlock）。
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
    last_batch_submitted_ = 0;
    batch_target_ = Endpoint{};
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
      enable_writev_(other.enable_writev_),
      last_batch_submitted_(other.last_batch_submitted_) {
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
        last_batch_submitted_ = other.last_batch_submitted_;
        other.fd_ = -1;
        other.next_token_ = 0;
        other.last_transferred_ = 0;
        other.last_error_ = 0;
        other.batch_bytes_ = 0;
        other.batch_entries_ = 0;
        other.enable_writev_ = false;
        other.last_batch_submitted_ = 0;
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
            batch_entries_ > 0 ? std::min<std::size_t>(batch_entries_, kSingleSendMaxSegments)
                               : 1;
        if (count > limit) {
            count = limit;
        }
        if (count == 0) {
            count = 1;
        }
        go2cpp::bufio::BatchEntry entries[kSingleSendMaxSegments];
        const std::size_t seg = (data.size + count - 1) / count;
        std::size_t used = 0;
        for (std::size_t i = 0; i < count && used < kSingleSendMaxSegments; ++i) {
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

IoStatus StreamSocket::SendBatch(const go2cpp::bufio::BatchEntry* entries,
                                 std::size_t count, SendResult* out) {
    SendResult result;
    result.token = ++next_token_;
    result.status = IoStatus::kOk;
    last_batch_submitted_ = 0;
    last_transferred_ = 0;
    last_error_ = 0;
    if (out != nullptr) {
        *out = result;
    }
    if (count == 0) {
        // 合法 no-op：不产生系统调用，token 仍按调用分配。
        return IoStatus::kOk;
    }
    if (!valid() || entries == nullptr) {
        last_error_ = EINVAL;
        result.status = IoStatus::kInvalid;
        if (out != nullptr) {
            *out = result;
        }
        return IoStatus::kInvalid;
    }
    const std::size_t limit = std::min(count, kMaxBatchEntries);
    std::size_t total = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (entries[i].data == nullptr && entries[i].size != 0) {
            last_error_ = EINVAL;
            result.status = IoStatus::kInvalid;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kInvalid;
        }
        total += entries[i].size;
    }
    // timeout=0：不等待、不重试；一次 writev 提交全部段。
    go2cpp::net::SocketBatchWriter writer(socket_, std::chrono::milliseconds(0), false);
    const go2cpp::streams::WriteResult written = writer.WriteBatch(entries, limit);
    const std::size_t transferred = written.count;
    last_transferred_ = transferred;
    last_batch_submitted_ = CompletePrefixSegments(entries, limit, transferred);
    if (written.error == nullptr) {
        // 部分写不是错误：已提交前缀段数由 last_batch_submitted() 给出。
        result.status = transferred >= total ? IoStatus::kOk : IoStatus::kWouldBlock;
        if (out != nullptr) {
            *out = result;
        }
        return result.status;
    }
    int err = writer.last_system_error();
    if (err == 0) {
        err = EIO;
    }
    if (err == ETIMEDOUT || err == EAGAIN) {
        last_error_ = 0;
        result.status = IoStatus::kWouldBlock;
    } else {
        last_error_ = err;
        result.status = MapSendErrno(err);
    }
    if (out != nullptr) {
        *out = result;
    }
    return result.status;
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
IoStatus DatagramSocket::SetBatchTarget(const Endpoint&, SendResult* out) {
    if (out != nullptr) {
        *out = SendResult{};
        out->status = IoStatus::kInvalid;
    }
    return IoStatus::kInvalid;
}
IoStatus DatagramSocket::SendBatch(const go2cpp::bufio::BatchEntry*, std::size_t,
                                   SendResult* out) {
    if (out != nullptr) {
        *out = SendResult{};
        out->status = IoStatus::kInvalid;
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
IoStatus StreamSocket::SendBatch(const go2cpp::bufio::BatchEntry*, std::size_t,
                                 SendResult* out) {
    if (out != nullptr) {
        *out = SendResult{};
        out->status = IoStatus::kInvalid;
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
