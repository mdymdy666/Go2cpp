#pragma once

/// @file socket.hpp
/// @brief JinShan REQ-006：UDP/TCP 统一 Socket 抽象与四态错误模型（rd::net）。
///
/// @details 本层只做「轻量 rd::net 封装」：
///          - 底层**复用仓库现有的 `go2cpp::net::Socket`**（生命周期、fd、
///            bind/connect/listen/close 与地址类型全部走它），不在本层新建
///            POSIX 实现；收发仍用 `fd()` 拿到的 fd 做非阻塞系统调用，
///            **不建线程**；
///          - 把内核返回值映射成显式状态：kClosed（FIN）/ kReset（RST）/
///            kTruncated（UDP 报文大于缓冲）/ kWouldBlock（非阻塞重试）四态可分；
///          - 不解析任何协议语义，不接管事件循环，不持有缓冲区所有权。
///
///          缓冲区所有权：ByteView / MutableByteView 是**调用方借用的视图**，
///          本层不做拷贝、不延长其生命周期；缓冲区由调用方保证在调用期间有效。
///
///          平台：Linux/POSIX 为验收实现；_WIN32 分支只保留接口占位，
///          不引入任何 Windows 头，由 JinShan 侧适配 WSA 错误码。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace go2cpp::net {
class Socket;
}  // namespace go2cpp::net

namespace rd::net {

/// @brief 一次 I/O 操作的状态（四态模型 + 通用错误）。
enum class IoStatus : std::uint8_t {
    kOk = 0,        ///< 成功（Recv 时也可能是「收到 0 字节」，见 kClosed）。
    kWouldBlock = 1,///< 非阻塞下当前不可读写，稍后重试。
    kTimeout = 2,   ///< 带超时的操作（如 connect）到期。
    kClosed = 3,    ///< 对端有序关闭（TCP FIN），流上不会再有任何字节。
    kReset = 4,     ///< 连接被重置（TCP RST / 对端 SO_LINGER=0 后 close）。
    kTruncated = 5, ///< 接收缓冲小于数据报，报文被内核截断（UDP）。
    kIoError = 6,   ///< 其它系统调用错误，errno 见 last_system_error()。
    kInvalid = 7,   ///< 参数非法或对象未打开。
};

/// @brief 返回状态的可读名称，用于日志与测试失败信息；未知值返回 "Unknown"。
const char* IoStatusName(IoStatus status) noexcept;

/// @brief 发送结果。token 是本 socket 上单调递增的发送序号（从 1 开始），
///        无论成功、kWouldBlock 还是错误都会分配，便于上层关联完成事件与重试。
struct SendResult {
    IoStatus status{IoStatus::kOk};
    std::uint64_t token{0};
};

/// @brief 网络端点（主机名或 IP 字面量 + 主机字节序端口）。
/// @note host 为空表示通配地址（仅绑定时有意义）；port 为 0 表示由内核分配。
struct Endpoint {
    std::string host;
    std::uint16_t port{0};

    bool empty() const noexcept { return host.empty() && port == 0; }
};

inline bool operator==(const Endpoint& a, const Endpoint& b) noexcept {
    return a.port == b.port && a.host == b.host;
}
inline bool operator!=(const Endpoint& a, const Endpoint& b) noexcept {
    return !(a == b);
}

/// @brief 只读字节视图（借用，不拥有）。
struct ByteView {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
};

/// @brief 可写字节视图（借用，不拥有）。
struct MutableByteView {
    std::uint8_t* data{nullptr};
    std::size_t size{0};
};

/// @brief UDP 打开选项。
struct SocketOptions {
    std::uint32_t recv_buffer_bytes{0};  ///< SO_RCVBUF，0 = 用系统默认。
    std::uint32_t send_buffer_bytes{0};  ///< SO_SNDBUF，0 = 用系统默认。
    bool reuse_address{false};           ///< SO_REUSEADDR。
    bool broadcast{false};               ///< SO_BROADCAST。

    // ---- 批量写（复用 go2cpp 的 bufio 批量能力；默认全关 = 单次 send）----
    /// @brief 聚合阈值（字节）。0 = 关闭批量路径，走单次 send/sendto。
    /// @note 开启后单次 SendTo/Send 会按 `batch_entries` 切成多段，
    ///       由 `SocketBatchWriter` 用一次 `writev`/`sendmmsg` 提交。
    std::uint32_t batch_bytes{0};
    /// @brief 单次批量提交的段数上限（iovec / sendmsg 数组长度），0 视为 1。
    std::uint32_t batch_entries{0};
    /// @brief TCP 走 `writev(2)` 批量提交（仅在 batch_bytes > 0 时生效）。
    bool enable_writev{false};
    /// @brief UDP 走 `sendmmsg(2)` 批量提交（仅在 batch_bytes > 0 时生效）。
    bool enable_sendmmsg{false};
};

/// @brief TCP 拨号选项。
struct DialOptions {
    std::chrono::milliseconds connect_timeout{std::chrono::milliseconds(3000)};
    bool tcp_no_delay{true};             ///< TCP_NODELAY。
    std::uint32_t recv_buffer_bytes{0};  ///< SO_RCVBUF，0 = 用系统默认。
    std::uint32_t send_buffer_bytes{0};  ///< SO_SNDBUF，0 = 用系统默认。

    // ---- 批量写（同 SocketOptions，默认全关）----
    /// @brief 聚合阈值（字节）。0 = 关闭批量路径，走单次 send。
    std::uint32_t batch_bytes{0};
    /// @brief 单次批量提交的段数上限（iovec 长度），0 视为 1。
    std::uint32_t batch_entries{0};
    /// @brief 走 `writev(2)` 批量提交（仅在 batch_bytes > 0 时生效）。
    bool enable_writev{false};
};

/// @brief 打开结果：成功时持有可移动的对象，失败时只带状态与 errno。
/// @tparam T 需要可移动；失败分支不构造 T。
template <typename T>
class Result;

/// @brief UDP 套接字：非阻塞、无连接（SendTo 显式给目标），可绑定本地端点。
class DatagramSocket {
public:
    DatagramSocket() noexcept = default;
    ~DatagramSocket();

    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;
    DatagramSocket(DatagramSocket&& other) noexcept;
    DatagramSocket& operator=(DatagramSocket&& other) noexcept;

    /// @brief 打开并绑定本地端点（port=0 由内核分配）。
    /// @param local 本地绑定地址；host 为空表示通配。
    static Result<DatagramSocket> Open(const Endpoint& local,
                                       const SocketOptions& options = {});

    /// @brief 发送一个数据报。UDP 语义为「全有或全无」，不产生部分发送。
    IoStatus SendTo(ByteView data, const Endpoint& to, SendResult* out);

    /// @brief 接收一个数据报。缓冲不足时返回 kTruncated，*received 给出**实际报文长度**。
    IoStatus RecvFrom(MutableByteView buf, Endpoint* from, std::size_t* received);

    /// @brief 关闭并释放 fd；幂等。
    void Close() noexcept;

    bool valid() const noexcept { return fd_ >= 0; }
    int fd() const noexcept { return fd_; }
    Endpoint local_endpoint() const noexcept { return local_; }
    int last_system_error() const noexcept { return last_error_; }

private:
    int fd_{-1};
    Endpoint local_{};
    std::uint64_t next_token_{0};
    int last_error_{0};
    /// @brief 底层 go2cpp Socket（拥有 fd；Close 走它的 close）。
    std::shared_ptr<go2cpp::net::Socket> socket_;
    /// @brief 批量配置快照（Open 时从 SocketOptions 拷入）。
    std::uint32_t batch_bytes_{0};
    std::uint32_t batch_entries_{0};
    bool enable_sendmmsg_{false};
};

/// @brief TCP 流套接字：非阻塞连接 + 非阻塞收发。
class StreamSocket {
public:
    StreamSocket() noexcept = default;
    ~StreamSocket();

    StreamSocket(const StreamSocket&) = delete;
    StreamSocket& operator=(const StreamSocket&) = delete;
    StreamSocket(StreamSocket&& other) noexcept;
    StreamSocket& operator=(StreamSocket&& other) noexcept;

    /// @brief 非阻塞连接到远端（带 connect_timeout），成功后保持非阻塞。
    static Result<StreamSocket> Open(const Endpoint& remote,
                                     const DialOptions& options = {});

    /// @brief 发送。可能部分发送：kOk 时用 last_transferred() 取得实发字节数，
    ///        调用方应推进视图后继续发送剩余部分。
    IoStatus Send(ByteView data, SendResult* out);

    /// @brief 接收。返回 0 字节且无错误 → kClosed（FIN）；ECONNRESET → kReset。
    IoStatus Recv(MutableByteView buf, std::size_t* received);

    /// @brief 关闭并释放 fd；幂等。
    void Close() noexcept;

    bool valid() const noexcept { return fd_ >= 0; }
    int fd() const noexcept { return fd_; }
    Endpoint peer_endpoint() const noexcept { return peer_; }
    Endpoint local_endpoint() const noexcept { return local_; }
    std::size_t last_transferred() const noexcept { return last_transferred_; }
    int last_system_error() const noexcept { return last_error_; }

private:
    int fd_{-1};
    Endpoint peer_{};
    Endpoint local_{};
    std::uint64_t next_token_{0};
    std::size_t last_transferred_{0};
    int last_error_{0};
    /// @brief 底层 go2cpp Socket（拥有 fd；Close 走它的 close）。
    std::shared_ptr<go2cpp::net::Socket> socket_;
    /// @brief 批量配置快照（Open 时从 DialOptions 拷入）。
    std::uint32_t batch_bytes_{0};
    std::uint32_t batch_entries_{0};
    bool enable_writev_{false};
};

/// @brief 最小 Result：成功时持有 T，失败时带 IoStatus + errno。
/// @note 仅允许在 T 完整类型可见后使用（.cpp 与测试中）。
template <typename T>
class Result {
public:
    static Result Ok(T value) {
        Result r;
        r.status_ = IoStatus::kOk;
        r.value_.emplace(std::move(value));
        return r;
    }
    static Result Err(IoStatus status, int system_error = 0) {
        Result r;
        r.status_ = status;
        r.system_error_ = system_error;
        return r;
    }

    bool ok() const noexcept { return status_ == IoStatus::kOk && value_.has_value(); }
    IoStatus status() const noexcept { return status_; }
    int system_error() const noexcept { return system_error_; }

    T& value() & { return value_.value(); }
    const T& value() const& { return value_.value(); }
    T&& value() && { return std::move(value_.value()); }
    T* operator->() { return &value_.value(); }
    const T* operator->() const { return &value_.value(); }
    T& operator*() { return value_.value(); }
    const T& operator*() const { return value_.value(); }

private:
    Result() = default;

    IoStatus status_{IoStatus::kInvalid};
    int system_error_{0};
    std::optional<T> value_;
};

}  // namespace rd::net
