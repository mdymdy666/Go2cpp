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

namespace go2cpp::bufio {
struct BatchEntry;
}  // namespace go2cpp::bufio

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

/// @brief SendBatch 单次调用允许提交的最大段数（与 go2cpp::net::Socket::kMaxBatch
///        及内核 IOV_MAX 对齐）。
/// @note SendBatch 是显式批量入口，**不受** SocketOptions/DialOptions 里
///       batch_entries 的限制（该配置只影响单条 SendTo/Send 的隐式切段）。
inline constexpr std::size_t kMaxBatchEntries = 1024;

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

    /// @brief 设置 SendBatch 的缺省目标端点（只解析地址，不发送数据报）。
    /// @return kOk；kInvalid 参数非法或对象未打开；errno 见 last_system_error()。
    IoStatus SetBatchTarget(const Endpoint& to, SendResult* out = nullptr);

    /// @brief 一次 `sendmmsg(2)` 提交多条数据报（显式批量入口，不阻塞）。
    ///
    /// @details 目标地址来自最近一次 `SetBatchTarget()` 或 `SendTo()` 解析出的
    ///          端点；从未设置过目标时返回 `kInvalid`（errno=EDESTADDRREQ）。
    ///          每条 `entries[i]` 就是**一条独立数据报**，按数组顺序提交；
    ///          size==0 且 data 非空的段提交为空数据报。
    ///          始终走 `SocketBatchWriter::SendToBatch`（一次 `sendmmsg`），
    ///          **不受** `SocketOptions::batch_bytes/batch_entries/
    ///          enable_sendmmsg` 影响——后者只控制单条 `SendTo` 的隐式切段。
    ///
    /// @par 上限与截断
    ///      单次最多提交 `min(count, kMaxBatchEntries)` 段（kMaxBatchEntries=1024）。
    ///      超出部分**本次不提交**：返回 `kOk` 时 `last_batch_submitted()` 为
    ///      1024，调用方应从第 1024 段继续提交。
    ///
    /// @par 部分提交规则
    ///      内核发送缓冲不足时 `sendmmsg` 只提交前 K 条：本函数返回
    ///      `kWouldBlock`，`last_batch_submitted() == K`（0 < K < 提交上限），
    ///      表示 `entries[0..K)` 已整条进入内核、`entries[K..)` 均未提交，
    ///      调用方从第 K 条重试即可（UDP 不会产生半条数据报）。token 仍是
    ///      本次调用的发送序号，**不**编码段数。
    ///
    /// @par 返回值
    ///      - kOk：提交上限内的段全部提交（`last_batch_submitted()==limit`）；
    ///      - kWouldBlock：一条都没提交（K==0）或只提交了前 K 条；
    ///      - kInvalid/kIoError/kReset/kClosed/kTimeout：参数非法或系统错误，
    ///        errno 见 last_system_error()；非阻塞入口内部不等待，EAGAIN 一律
    ///        映射成 kWouldBlock。
    /// @note `count == 0` 是合法 no-op：返回 kOk，不产生系统调用，
    ///       `last_batch_submitted()==0`，token 仍按调用分配。
    /// @note entries 只在调用期间借用，本函数不保存指针。
    IoStatus SendBatch(const go2cpp::bufio::BatchEntry* entries, std::size_t count,
                       SendResult* out);

    /// @brief 最近一次 SendBatch 已**完整提交**的前缀段数 K。
    std::size_t last_batch_submitted() const noexcept { return last_batch_submitted_; }

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
    /// @brief SendBatch 已完整提交的前缀段数（最近一次调用）。
    std::size_t last_batch_submitted_{0};
    /// @brief SendBatch 的缺省目标（SetBatchTarget 或最近一次 SendTo 写入）。
    Endpoint batch_target_{};
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

    /// @brief 一次 `writev(2)` 提交全部段（显式批量入口，不阻塞）。
    ///
    /// @details 所有段按数组顺序写入同一字节流，等价于把各段拼成一块后一次
    ///          `write`，但保留段边界、不需要调用方先拷贝成大缓冲。始终走
    ///          `SocketBatchWriter::WriteBatch`，**不受** `DialOptions::batch_*`
    ///          与 `enable_writev` 影响——后者只控制单条 `Send` 的隐式切段。
    ///
    /// @par 上限与截断
    ///      单次最多提交 `min(count, kMaxBatchEntries)` 段（1024）；超出部分
    ///      本次不提交，返回 `kOk` 且 `last_batch_submitted()==1024`。
    ///
    /// @par 部分提交规则
    ///      对端读得慢、发送缓冲放不下全部段时，`writev` 只写入前若干字节：
    ///      - 返回 `kWouldBlock`；
    ///      - `last_batch_submitted() == K`：`entries[0..K)` 已**整段**写入；
    ///      - `last_transferred()` 为实际写入的总字节数，满足
    ///        `prefix(K) <= last_transferred() < prefix(K+1)`，其中
    ///        `prefix(K)` 是前 K 段字节和；
    ///      - 调用方应从第 K 段继续，并把该段起点前移
    ///        `last_transferred() - prefix(K)` 字节（该段可能已被部分写入）。
    ///      `kOk` 表示提交上限内的段全部写入（`last_batch_submitted()==limit`）。
    ///
    /// @par 其它
    ///      - `count == 0`：kOk no-op，不产生系统调用；
    ///      - 内部 timeout=0，不等待、不重试；EAGAIN 映射成 kWouldBlock；
    ///      - entries 仅借用，不保存指针。
    IoStatus SendBatch(const go2cpp::bufio::BatchEntry* entries, std::size_t count,
                       SendResult* out);

    /// @brief 最近一次 SendBatch 已**整段提交**的前缀段数 K。
    std::size_t last_batch_submitted() const noexcept { return last_batch_submitted_; }

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
    /// @brief SendBatch 已整段提交的前缀段数（最近一次调用）。
    std::size_t last_batch_submitted_{0};
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
