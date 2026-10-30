#pragma once

/**
 * @file socks5.hpp
 * @brief SOCKS5（RFC 1928）最小骨架：无认证握手、CONNECT 请求与双向中继。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details 本文件是 Bento 网络层的 SOCKS5 服务端骨架，**不含第三方库**，
 *          只依赖 POSIX socket 与标准库。它刻意分成两层：
 *
 *          - **协议层**（`ParseSocks5Greeting` / `ParseSocks5Request` /
 *            `EncodeSocks5Reply`）：纯字节编解码，不碰 fd、不分配连接、
 *            不做 DNS。测试可以直接喂任意字节序列，覆盖半包、非法命令、
 *            未知 ATYP 等边界；
 *          - **服务端**（`Socks5Server`）：监听 → accept → 握手 → CONNECT
 *            拨号 → 双向中继，单线程 `poll` 驱动，Open/Poll/RunFor/Close
 *            与 `TcpProxy` 同风格。需要后台线程时用 Start/Stop。
 *
 * ## 协议覆盖范围（RFC 1928）
 *
 *          - 握手：`VER(0x05) NMETHODS METHODS...` → 选了 0x00（无认证）时回
 *            `05 00`；客户端未提供 0x00 时回 `05 FF` 并关闭；
 *          - 请求：`VER CMD RSV ATYP DST.ADDR DST.PORT`，ATYP 支持 IPv4
 *            (0x01)、域名 (0x03)、IPv6 (0x04)；
 *          - 响应：成功时 `BND.ADDR/BND.PORT` 填上游 socket 的本地地址
 *            （`getsockname`），失败时固定 `0.0.0.0:0` 并带 RFC 错误码。
 *
 *          未实现（骨架边界）：用户名/密码与 GSSAPI 认证（RFC 1929/1961）、
 *          BIND 与 UDP ASSOCIATE（回 0x07）、UDP 中继、TLS、ACL/规则集
 *          （0x02 预留但当前没有规则来源）。
 *
 * ## 为什么是单线程 poll 而不是每连接一线程
 *
 *          SOCKS5 服务端天然是「大量短连接 + 长寿命中继」的混合负载。每连接
 *          起两个线程做双向拷贝的写法简单，但连接数一上去线程栈与上下文
 *          切换就不划算；单线程 poll 与 `TcpProxy`（MOD-05 场景）保持一致，
 *          没有锁、没有 join 竞态，Stop 的语义也简单。
 *          代价是**域名解析仍在驱动线程里同步做 `getaddrinfo`**：DNS 慢时
 *          会阻塞其他连接。这是骨架的已知限制，生产实现应换成异步解析或
 *          解析线程池。
 *
 * ## 端口与测试
 *
 *          `listen_port = 0` 时由内核分配，`local_port()` 返回实际端口；
 *          测试因此可以并行跑多个实例，不依赖固定端口。
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace go2cpp::bento::socks5 {

/// @brief RFC 1928 协议版本；只有 0x05 合法。
inline constexpr std::uint8_t kProtocolVersion = 0x05;

/**
 * @brief 认证方法编号（RFC 1928 §3）。
 *
 * @details 服务端只支持 0x00（无认证）；其余编号仅用于解析与日志，
 *          不会协商成功。
 */
enum class Socks5Method : std::uint8_t {
    kNoAuthentication = 0x00,  ///< 无认证（本骨架唯一支持的方法）。
    kGssApi = 0x01,            ///< GSSAPI（未实现）。
    kUsernamePassword = 0x02,  ///< 用户名/密码（RFC 1929，未实现）。
    /// 服务端回给客户端的「没有可接受的方法」（只在响应里出现）。
    kNoAcceptable = 0xFF,
};

/// @brief 请求命令（RFC 1928 §4）：本骨架只实现 CONNECT。
enum class Socks5Command : std::uint8_t {
    kConnect = 0x01,       ///< 建立到目标的 TCP 连接。
    kBind = 0x02,          ///< 反向绑定（未实现，回 0x07）。
    kUdpAssociate = 0x03,  ///< UDP 关联（未实现，回 0x07）。
};

/// @brief 目标地址类型（RFC 1928 §4）。
enum class Socks5AddressType : std::uint8_t {
    kIpv4 = 0x01,        ///< 4 字节 IPv4 地址。
    kDomainName = 0x03,  ///< 1 字节长度 + 域名（不以 NUL 结尾）。
    kIpv6 = 0x04,        ///< 16 字节 IPv6 地址。
};

/**
 * @brief 响应码（RFC 1928 §6）。数值即线格式字节，不要改。
 *
 * @note 0x02（规则不允许）本骨架保留但当前没有规则来源，不会主动发送。
 *       0x09–0xFF 未分配，解析时按一般故障处理。
 */
enum class Socks5ReplyCode : std::uint8_t {
    kSucceeded = 0x00,                ///< 成功，随后进入中继。
    kGeneralFailure = 0x01,           ///< 一般性服务端故障。
    kNotAllowed = 0x02,               ///< 规则集不允许（当前无规则来源）。
    kNetworkUnreachable = 0x03,       ///< 网络不可达（如 ENETUNREACH）。
    kHostUnreachable = 0x04,          ///< 主机不可达（DNS 失败/EHOSTUNREACH）。
    kConnectionRefused = 0x05,        ///< 目标拒绝连接（ECONNREFUSED）。
    kTtlExpired = 0x06,               ///< TTL 过期/连接超时。
    kCommandNotSupported = 0x07,      ///< CMD 不是 CONNECT。
    kAddressTypeNotSupported = 0x08,  ///< ATYP 未知。
};

/**
 * @brief 增量解析状态。
 *
 * @details `kIncomplete` 是**正常**的中间态：调用方保留已读字节，等更多
 *          数据再调一次；`kError` 表示协议违规，调用方应回错误响应（若
 *          还能回）并关闭连接。
 */
enum class Socks5ParseState : std::uint8_t {
    kIncomplete = 0,  ///< 数据不足，继续读。
    kComplete = 1,    ///< 已完整解析。
    kError = 2,       ///< 协议违规。
};

/// @brief 客户端问候（VER/NMETHODS/METHODS）。
struct Socks5Greeting {
    /// METHOD 列表，按线上顺序保留，便于日志与「客户端支持哪些方法」诊断。
    std::vector<std::uint8_t> methods;

    /// @brief 客户端是否提供了 0x00（无认证）。
    bool OffersNoAuthentication() const noexcept {
        for (const std::uint8_t method : methods) {
            if (method == static_cast<std::uint8_t>(Socks5Method::kNoAuthentication)) {
                return true;
            }
        }
        return false;
    }
};

/// @brief `ParseSocks5Greeting` 的结果。
struct Socks5GreetingResult {
    /// 解析状态；`kIncomplete` 时 `consumed` 无意义。
    Socks5ParseState state{Socks5ParseState::kIncomplete};
    /// 线上 VER 字节；`kError` 时用于日志（非 0x05）。
    std::uint8_t version{0};
    /// `kComplete` 时消耗的字节数（2 + NMETHODS）。
    std::size_t consumed{0};
};

/**
 * @brief 解析客户端问候。
 *
 * @param data 已读到的字节；可以包含问候之后的多余数据（半包/粘包均支持）。
 * @param size `data` 的有效长度。
 * @param greeting 输出：METHOD 列表。
 * @return 解析状态、版本与消耗字节数。
 *
 * @details 约束：VER 必须为 0x05、NMETHODS 必须 >= 1（RFC 1928 §3 要求
 *          至少一个方法，0 个方法无法协商）。数据不足时返回 `kIncomplete`
 *          且不修改 `greeting`。
 */
Socks5GreetingResult ParseSocks5Greeting(const std::uint8_t* data, std::size_t size,
                                         Socks5Greeting& greeting);

/**
 * @brief 生成方法选择响应（2 字节）。
 * @param accept_no_auth true → `05 00`；false → `05 FF`（随后应关闭）。
 */
std::vector<std::uint8_t> EncodeSocks5MethodSelection(bool accept_no_auth);

/// @brief 已解析的 CONNECT 请求。
struct Socks5Request {
    Socks5Command command{Socks5Command::kConnect};
    Socks5AddressType address_type{Socks5AddressType::kIpv4};
    /// IPv4 点分文本、IPv6 冒号文本，或域名原文（不做 IDNA/大小写转换）。
    std::string host;
    /// 目标端口（网络字节序已在解析时转成主机序）。
    std::uint16_t port{0};

    /// @brief 便于日志的 `host:port`；IPv6 用方括号包裹。
    std::string Target() const {
        const bool ipv6 = address_type == Socks5AddressType::kIpv6;
        return (ipv6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
    }
};

/// @brief `ParseSocks5Request` 的结果。
struct Socks5RequestResult {
    Socks5ParseState state{Socks5ParseState::kIncomplete};
    /// `kError` 时建议回给客户端的错误码（已按 RFC 归类）。
    Socks5ReplyCode error{Socks5ReplyCode::kGeneralFailure};
    /// `kComplete` 时消耗的字节数；`kError` 时为已检查到的字节数（仅诊断用）。
    std::size_t consumed{0};
};

/**
 * @brief 解析 CONNECT 请求。
 *
 * @param data 已读到的字节。
 * @param size `data` 的有效长度。
 * @param request 输出：仅在返回 `kComplete` 时有效。
 * @return 解析状态、建议错误码与消耗字节数。
 *
 * @details 错误归类（用于回包，不抛异常）：
 *          - VER != 0x05 或 RSV != 0 → `kGeneralFailure`；
 *          - CMD 不是 CONNECT → `kCommandNotSupported`（不等地址部分）；
 *          - ATYP 未知 → `kAddressTypeNotSupported`（不等地址部分）；
 *          - 域名长度 0 → `kHostUnreachable`（畸形目标）；
 *          - 端口缺失 → `kIncomplete`，等待更多数据。
 *          注意域名**允许**非 ASCII 字节：RFC 1928 不规定编码，本层保持
 *          原始字节，由服务端交给 `getaddrinfo` 解析。
 */
Socks5RequestResult ParseSocks5Request(const std::uint8_t* data, std::size_t size,
                                       Socks5Request& request);

/**
 * @brief 生成响应报文。
 *
 * @param code 响应码。
 * @param address_type `BND.ADDR` 的类型；成功时只接受 IPv4/IPv6。
 * @param address `BND.ADDR` 原始字节（4 或 16 字节）；可为 nullptr。
 * @param address_length `address` 的字节数。
 * @param port `BND.PORT` 主机序端口。
 * @return 完整响应字节（10 或 22 字节）。
 *
 * @details 失败响应（`code != kSucceeded`）固定写 `ATYP=0x01`、`0.0.0.0:0`，
 *          这是 RFC 1928 允许的最保守形式，也避免把内部地址泄漏给客户端。
 *          成功但参数不自洽（类型/长度不匹配）时降级为 `0.0.0.0:0`，仍保留
 *          成功码——调用方应保证成功路径的参数来自 `getsockname`。
 */
std::vector<std::uint8_t> EncodeSocks5Reply(Socks5ReplyCode code,
                                            Socks5AddressType address_type,
                                            const std::uint8_t* address,
                                            std::size_t address_length,
                                            std::uint16_t port);

/// @brief 响应码的可读名称，用于日志与测试失败信息；未知值返回 "Unknown"。
const char* Socks5ReplyCodeName(Socks5ReplyCode code) noexcept;

/// @brief 命令的可读名称；未知值返回 "Unknown"。
const char* Socks5CommandName(Socks5Command command) noexcept;

/// @brief 地址类型的可读名称；未知值返回 "Unknown"。
const char* Socks5AddressTypeName(Socks5AddressType type) noexcept;

/// @brief 服务端配置；`Open()` 时快照，运行期不再改变。
struct Socks5ServerConfig {
    /// 监听地址（IPv4/IPv6 字面量或主机名）；空字符串按 "0.0.0.0" 处理。
    std::string listen_address = "127.0.0.1";
    /// 监听端口；0 = 内核分配（测试/多实例推荐）。
    std::uint16_t listen_port = 0;
    /// 同时存在的连接上限（含握手中与中继中）；超过后新连接立即关闭。
    std::size_t max_connections = 128;
    /// 从 accept 到 CONNECT 请求解析完成的超时；0 = 不限制。
    std::uint32_t handshake_timeout_ms = 10000;
    /// 上游 connect 的超时；0 = 不限制。
    std::uint32_t connect_timeout_ms = 5000;
    /// 中继期间双向都没有字节流动的空闲超时；0 = 不限制。
    std::uint32_t idle_timeout_ms = 60000;
    /// 已决定拒绝/关闭后，把错误响应写出去的宽限时间；0 = 不限制。
    /// 客户端不读响应时不能永久占着连接，到点直接关闭。
    std::uint32_t close_flush_timeout_ms = 2000;
    /// 每个方向 pending 缓冲的容量上限（字节），也是单次 read 的上限。
    std::size_t buffer_bytes = 64 * 1024;
    /// listen(2) 的 backlog。
    int backlog = 64;
};

/// @brief 服务端计数器。每个拒绝/超时都有独立计数，便于上层告警。
struct Socks5Stats {
    std::uint64_t accepted = 0;           ///< accept 成功（含随后握手失败）。
    std::uint64_t rejected = 0;           ///< 超过 max_connections 被拒。
    std::uint64_t handshake_ok = 0;       ///< 协商出无认证并进入请求阶段。
    std::uint64_t auth_rejected = 0;      ///< 客户端未提供 0x00，回 05 FF。
    std::uint64_t handshake_failed = 0;   ///< 握手阶段版本/长度非法。
    std::uint64_t handshake_timeouts = 0; ///< 握手超时关闭。
    std::uint64_t requests = 0;           ///< 完整解析出的 CONNECT 请求。
    std::uint64_t request_errors = 0;     ///< 请求阶段协议错误（含 CMD/ATYP）。
    std::uint64_t connect_ok = 0;         ///< 上游连接成功。
    std::uint64_t connect_failed = 0;     ///< 上游连接失败（含 DNS/超时）。
    std::uint64_t bytes_to_upstream = 0;  ///< 客户端 → 上游累计字节。
    std::uint64_t bytes_to_client = 0;    ///< 上游 → 客户端累计字节。
    std::uint64_t idle_timeouts = 0;      ///< 中继空闲超时关闭。
    std::uint64_t closed = 0;             ///< 已关闭连接总数（正常 + 异常）。
    std::uint64_t resets = 0;             ///< 读写错误导致的中断。
};

/**
 * @brief SOCKS5 服务端（单线程 poll 驱动，非阻塞）。
 *
 * @note 线程模型：除 `Start()`/`Stop()` 外，所有成员函数必须在**同一个
 *       驱动线程**调用（`Poll`/`RunFor` 或持有连接表的线程）。`Stats()`
 *       与 `local_port()` 只是读快照，仍建议在停止后读取，避免与驱动
 *       线程的计数器更新竞争。`Start()` 后不要在外部再调 `Poll()`。
 */
class Socks5Server final {
   public:
    explicit Socks5Server(Socks5ServerConfig config = {});
    ~Socks5Server();

    Socks5Server(const Socks5Server&) = delete;
    Socks5Server& operator=(const Socks5Server&) = delete;

    /**
     * @brief 创建监听 socket（只 bind + listen，不拨号）。
     * @param error 失败原因（可为 nullptr）。
     * @return 成功与否；失败后对象仍可重试（不会残留半开的 listen fd）。
     */
    bool Open(std::string* error = nullptr);

    /**
     * @brief 在后台线程循环 `Poll()`，直到 `Stop()`。
     * @param error 失败原因：未 Open、已在运行、线程创建失败。
     * @note 线程创建失败时不会留下运行标记；调用方可以再次 Start。
     */
    bool Start(std::string* error = nullptr);

    /// @brief 通知后台线程退出并 join（幂等）。不会关闭监听 socket。
    void Stop();

    /// @brief 关闭监听与全部连接（幂等）；可在 `Stop()` 之后或之前调用。
    void Close();

    /**
     * @brief 处理一轮 accept/握手/拨号/中继/超时检查。
     * @param timeout_ms `poll` 的等待上限；0 = 只轮询不等待。
     * @return 本轮 accept + 中继搬运的字节数，便于测试判断是否有进展。
     */
    int Poll(int timeout_ms);

    /// @brief 循环 `Poll` 指定时长（阻塞当前线程）。
    void RunFor(int duration_ms);

    bool is_open() const noexcept { return m_listen_fd >= 0; }

    /// @brief 实际监听端口（`listen_port = 0` 时由内核分配）。
    std::uint16_t local_port() const noexcept { return m_local_port; }

    /// @brief 当前连接数（含握手中与中继中）。
    std::size_t connection_count() const noexcept { return m_connections.size(); }

    /// @brief 计数器快照（见类注释的线程约定）。
    Socks5Stats Stats() const noexcept { return m_stats; }

   private:
    /// @brief 一条客户端连接的全部状态（定义在 .cpp，避免头文件暴露 fd 细节）。
    struct Connection;

    /// @brief 接受所有待处理连接（非阻塞），超过上限的直接关闭并计数。
    void AcceptReady();

    /// @brief 推进一条连接的状态机；返回本轮收发字节数。
    int ServiceConnection(std::size_t index, std::uint64_t now_ms);

    /// @brief 握手阶段：读 VER/NMETHODS/METHODS 并回方法选择。
    int ServiceGreeting(Connection& connection, std::uint64_t now_ms);

    /// @brief 请求阶段：解析 CMD/ATYP/地址/端口。
    int ServiceRequest(Connection& connection, std::uint64_t now_ms);

    /// @brief 拨号阶段：检查非阻塞 connect 是否完成。
    int ServiceConnecting(Connection& connection, std::uint64_t now_ms);

    /// @brief 中继阶段：双向搬运 + 半关闭传播。
    int ServiceRelay(Connection& connection, std::uint64_t now_ms);

    /// @brief 收尾阶段：把错误响应写出去后关闭（受 close_flush_timeout_ms 约束）。
    int ServiceClosing(Connection& connection);

    /// @brief 解析目标地址并发起非阻塞 connect；失败时已写好错误响应。
    /// @return true 表示已进入拨号或中继；false 表示已拒绝（进入 kClosing）。
    bool StartConnect(Connection& connection, std::uint64_t now_ms);

    /// @brief 上游连上后写成功响应，并把握手期间多读的首包转给上游。
    void BeginRelay(Connection& connection, std::uint64_t now_ms);

    /// @brief 追加一条错误响应并进入 kClosing。
    void RejectRequest(Connection& connection, Socks5ReplyCode code, std::uint64_t now_ms);

    /// @brief 关闭并回收已标记 `dead` 的连接。
    void ReapDead();

    /// @brief 检查握手/拨号/空闲超时并标记关闭。
    void CheckTimeouts(std::uint64_t now_ms);

    /// @brief 关闭一条连接的两个 fd 并计数（不删除，等待 ReapDead）。
    void CloseConnection(Connection& connection, const char* reason);

    Socks5ServerConfig m_config;
    int m_listen_fd = -1;
    std::uint16_t m_local_port = 0;
    std::uint64_t m_next_connection_id = 1;
    std::vector<std::unique_ptr<Connection>> m_connections;
    Socks5Stats m_stats;

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    bool m_running = false;
};

}  // namespace go2cpp::bento::socks5
