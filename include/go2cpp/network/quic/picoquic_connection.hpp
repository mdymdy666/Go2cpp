/**
 * @file picoquic_connection.hpp
 * @brief 基于 picoquic 的 QUIC 薄封装（客户端）。
 *
 * ## 为什么是「薄封装」而不是自己实现
 *
 * QUIC 涉及 TLS 1.3 握手、密钥派生、Header Protection、丢包检测/PTO、
 * 拥塞控制、流状态机、QPACK 等，成熟实现是数万行、多年迭代的结果。
 * 本项目只做**协议层与 Fiber 的粘合**，协议逻辑全部交给 picoquic。
 *
 * ## 为什么选 picoquic
 *
 * 候选库的 TLS 依赖（实测）：
 *
 * | 库 | TLS 依赖 | 本机 OpenSSL 3.0.13 够用？ |
 * |---|---|---|
 * | **picoquic** | **picotls（自带 TLS 1.3）** | **是** |
 * | ngtcp2 | 必须 OpenSSL 3.2+（`SSL_set_quic_tls_cbs`） | 否 |
 * | msquic | 默认 quictls | 否 |
 * | lsquic | BoringSSL | 否 |
 * | quiche | Rust + BoringSSL | 否 |
 *
 * picoquic 是唯一**不需要更换系统 TLS**的：picotls 自己实现 TLS 1.3，
 * 系统 OpenSSL 只用于证书解析与哈希。
 *
 * ## 设计：只借用协议层，IO 完全由本项目控制
 *
 * picoquic 自带 socket 层与事件循环（`picosock`），本封装**不使用它们**。
 * 只用协议层：
 *
 * - `picoquic_prepare_next_packet` → 取出"想发的字节"
 * - `picoquic_incoming_packet` → 喂"收到的字节"
 *
 * 于是 IO 抽象成项目的 `Transport`（`Read`/`Write`），可以：
 * - 用**内存 Transport** 在单线程里测试（不需要真实网络）
 * - 用 **Fiber 版 Transport** 让 `Read` park 当前 Fiber（不阻塞 worker）
 *
 * **不引入第二个线程** —— 这是与 picoquic 自带事件循环最大的区别，
 * 也是它能与本项目 `Scheduler` 共存的前提。
 *
 * ## `Transport` 必须是 datagram 语义
 *
 * QUIC 是**基于 UDP** 的协议，每一"包"必须保持边界。所以传入的
 * `Transport::Read` **必须一次返回一个完整 datagram**，不能像 TCP 那样
 * 把多段数据合并。用 TCP 风格的 Transport 会导致 QUIC 解析失败。
 *
 * ## 头文件不依赖 picoquic
 *
 * 用 pimpl（`struct Impl` + `unique_ptr<Impl>`）把 picoquic 类型完全藏在
 * 实现文件里。好处：
 * - 使用本头文件的项目代码**不需要** picoquic 的头文件路径
 * - picoquic 头里的宏（如 `PICOQUIC_VERSION`）不会污染使用者
 * - picoquic 升级时，只有 `picoquic_connection.cpp` 需要重编
 *
 * ## 本文件定义什么
 * - `PicoquicConfig`：ALPN / SNI / 证书校验 / idle 超时；
 * - `PicoquicConnection`：客户端连接的创建、`Poll()` 驱动、按流读写与诊断接口。
 *
 * ## 阅读建议
 * 先看文件头的「设计：只借用协议层」与「Transport 必须是 datagram 语义」，
 * 再看 `Poll()` 的三步，最后看 `picoquic_connection.cpp` 头部列出的三个实现坑。
 *
 * ## 边界与陷阱
 * - `GO2CPP_WITH_PICOQUIC` **默认 OFF**（见 `cmake/Go2CppModules.cmake`）：
 *   不打开时 `picoquic_connection.cpp` 不参与构建，头文件仍可被包含，但链接期
 *   会缺符号；打开时还需同时给出 `PICOQUIC_ROOT` 与 `PICOTLS_ROOT`；
 * - 本地自签名证书必须走 `picoquic_set_null_verifier()`（本封装在
 *   `verify_peer == false` 时代为调用），否则严格验证必然握手失败；
 * - `picoquic_create_client_cnx()` 内部已经调用了 `picoquic_start_client_cnx()`，
 *   再调一次会返回 -1（错误信息 "called twice"）；
 * - `Transport` 必须是 datagram 语义（一次 `Read` 一个完整包），TCP 风格实现
 *   会让 QUIC 解析失败。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace go2cpp::net::http2 {
/// 前置声明：字节通道接口（定义在 http2/transport.hpp）。
class Transport;
}

namespace go2cpp::net::quic {

/**
 * @brief 客户端连接参数。
 */
struct PicoquicConfig {
    /// ALPN。`hq-interop` 是 picoquic 自带的 HTTP/0.9 测试协议（最容易验证）；
    /// 真正的 HTTP/3 用 `h3`，但那需要额外的 QPACK 解码层（本封装不做）。
    std::string alpn{"hq-interop"};

    /// SNI（TLS 里的服务器名）。HTTP/3 的 `:authority` 由上层单独构造，
    /// 两者不要混用（同一字段既当 SNI 又当 authority 是审计 D15 的缺陷）。
    std::string sni{"localhost"};

    /// 是否验证对端证书。
    ///
    /// **默认 `false`** —— 这与安全直觉相反，理由：本项目目前的验证场景都是
    /// **本地自签名证书**，严格验证会直接握手失败。生产环境必须设为 `true`
    /// 并配置根证书。这个默认值是**测试便利**，不是安全默认。
    bool verify_peer{false};

    /// 根证书（CA bundle）PEM 文件路径。
    ///
    /// - `verify_peer == true` 时使用；留空则自动探测系统信任库
    ///   （`/etc/ssl/certs/ca-certificates.crt`、`/etc/pki/tls/certs/ca-bundle.crt` 等）；
    /// - `verify_peer == false` 时忽略（改为安装 null verifier，保持本地自签场景可用）。
    std::string cert_root_file_name;

    /// 空闲超时（毫秒）。0 表示用 picoquic 默认值。
    std::uint32_t idle_timeout_ms{30000};
};

/**
 * @brief 一条 QUIC 客户端连接。
 *
 * 生命周期：`Connect` 之后反复调 `Poll` 驱动，直到 `HandshakeComplete()`
 * 为真后可以 `WriteStream` / `ReadStream`。
 *
 * **非线程安全** —— 所有方法必须在同一个线程（或同一个 Fiber 序列）里调用。
 *
 * ## 定位
 * 被 `example/network/picoquic_hq_client.cpp` 与
 * `src/network/http3/client.cpp`（HTTP/3 客户端）使用；字节通道由调用方以
 * `net::http2::Transport*` 注入。与 `http2::Connection` 并列：后者自己实现
 * 协议，前者把协议全部交给 picoquic，只做 Fiber 粘合。
 * 仓库内**没有** `tests/network/quic_*` 测试（审计 D19：HTTP/3 测试缺口）。
 *
 * ## 不变量
 * - 所有方法必须在同一线程/Fiber 序列调用（非线程安全）；
 * - `HandshakeComplete()` 为真之前 `WriteStream()` 不可用；
 * - `ReadStream()` 返回 0 表示「该流暂时没有数据」，不是 EOF；
 *   判断结束要用 `StreamFinished(stream_id)`（审计 D17）；
 * - 回调内发生的 C++ 异常会被捕获并置 `closed`（审计 D18），
 *   `Poll()` 返回 false 且错误为 "exception escaped stream callback"；
 * - 对象不拥有 `Transport` 的生命周期：`Transport` 必须比本对象**活得久**，
 *   因为析构会尽力发送 CONNECTION_CLOSE（见 `Close()`）后才释放 picoquic 上下文；
 * - 析构是 best-effort 关闭：显式 `Close()` 才能拿到失败原因。
 */
class PicoquicConnection {
public:
    /**
     * @brief 建立连接（只创建对象并发出 ClientHello，握手在后续 `Poll` 中推进）。
     *
     * @param transport 字节通道。**必须是 datagram 语义**（见文件头说明）。
     *                  对象不接管所有权，调用方保证其生命周期长于本对象。
     * @param config 连接参数。
     * @param error 失败时写入原因；可为 nullptr。
     * @return 成功返回对象，失败返回 nullptr 并填 `error`。
     */
    static std::unique_ptr<PicoquicConnection> Connect(
        net::http2::Transport* transport, const PicoquicConfig& config, std::string* error);

    ~PicoquicConnection();
    PicoquicConnection(const PicoquicConnection&) = delete;
    PicoquicConnection& operator=(const PicoquicConnection&) = delete;

    /**
     * @brief 推进一次协议状态机（单次最多等待 `kDefaultPollWaitMs` = 1000 ms）。
     *
     * 做三件事：
     * 1. 调 `picoquic_prepare_next_packet` 循环，把**所有待发**的包写到 `transport`；
     * 2. 按 `picoquic_get_next_wake_time()` 截短等待，用
     *    `Transport::ReadWithTimeout()` 读**一个** datagram 喂给
     *    `picoquic_incoming_packet`（定时器到点会先重传再继续等）；
     * 3. 回调置 closed 或出错时返回 false。
     *
     * @return 连接仍存活返回 true（**包括等待超时**）；已关闭/出错返回 false 并填 `error`。
     *
     * @note 单次调用有 1s 上限，调用方不会被无界阻塞；需要精确控制等待用
     *       `PollFor()`，需要从别的 Fiber/线程取消用 `Close()`。
     */
    bool Poll(std::string* error);

    /**
     * @brief 带等待预算的 `Poll`。
     *
     * @param timeout_ms 等待上限（毫秒）：`0` = 只发待发包并做一次非阻塞读；
     *                   `>0` = 最多等这么久；`<0` = 等到有数据或连接关闭
     *                   （可由 `Close()` 从外部置状态取消）。
     * @return 连接仍存活（含等待超时）返回 true；已关闭/出错返回 false。
     *
     * 每次内部等待还会被 picoquic 的下一个定时器（PTO / idle / ACK delay）
     * 截短，保证重传与 idle 超时能被及时驱动；Transport 若立即返回 0，
     * 本方法会做 1ms→20ms 退避，避免忙转占满 CPU。
     */
    bool PollFor(std::string* error, int timeout_ms);

    /**
     * @brief 主动关闭连接，并把 picoquic 生成的 CONNECTION_CLOSE 立即写出去。
     *
     * 只调 `picoquic_close()` 只是把连接置为 disconnecting，关闭帧要等下一次
     * `prepare_next_packet` 才发送；本方法在内部完成这一步（不需要再 `Poll`）。
     * 析构函数复用同一路径，因此对象销毁时对端能收到关闭帧并释放连接槽
     * （审计 D01：不关闭时服务端 256 个连接槽耗尽后拒绝新连接）。
     *
     * @param application_error_code 应用层关闭码（0 表示正常关闭）。
     * @param error 写失败时填原因；可为 nullptr。
     * @return 关闭帧是否成功写出（连接已关闭/已释放时返回 true）。
     *
     * @note 幂等；调用后 `Closed()` 为 true，`LastErrorCode()` 返回关闭码。
     *       `transport` 必须仍然可用。
     */
    bool Close(std::uint64_t application_error_code = 0, std::string* error = nullptr);

    /// 握手（TLS 1.3）是否已完成。完成后才能收发应用数据。
    bool HandshakeComplete() const noexcept;

    /// 连接是否已关闭。
    bool Closed() const noexcept;

    /**
     * @brief 向流 `stream_id` 写数据。
     *
     * @param stream_id 流号。客户端发起的双向流从 0 开始（0, 4, 8, ...）。
     * @param data 数据。
     * @param length 长度。
     * @param fin 是否在写完这批数据后关闭发送方向。
     * @param error 失败原因。
     * @return 实际排入的数据字节数；0 表示失败。
     */
    std::size_t WriteStream(std::uint64_t stream_id, const std::uint8_t* data,
                            std::size_t length, bool fin, std::string* error);

    /**
     * @brief 从接收缓冲里取出流 `stream_id` 的数据。
     *
     * @return 取到的字节数；0 表示该流暂时没有数据（**不是**错误，也不是 EOF）。
     *
     * @note 本封装只做**按流分拣**，把每个流收到的字节留在内部缓冲里。
     *       真正的 HTTP/3 解析（QPACK + 帧）不在这里做。
     */
    std::size_t ReadStream(std::uint64_t stream_id, std::uint8_t* buffer,
                           std::size_t capacity);

    /// 流是否已收到对端 FIN（`true` 表示不会再有新数据）。
    bool StreamFinished(std::uint64_t stream_id) const noexcept;

    /// 流最近一次 reset 的对端错误码；未 reset 返回 0。
    ///
    /// 与 `StreamFinished()` 配合使用：FIN 之后不会再有数据；reset 之后
    /// 该流的数据应被丢弃（错误码来自 `picoquic_get_remote_stream_error`）。
    std::uint64_t StreamErrorCode(std::uint64_t stream_id) const noexcept;

    /// 已收到数据的流号列表（便于调用方知道哪些流有数据）。
    void ReceivedStreamIds(std::uint64_t* out, std::size_t capacity,
                           std::size_t* count) const;

    /// 最近一次错误码（picoquic 的 application/transport error code）。
    ///
    /// 关闭类事件触发后为关闭码（对端 application > 本地 application >
    /// 对端 transport > 本地 transport）；`Close(code)` 后为 `code`。
    /// 回调内发生 C++ 异常时为 0x0102（HTTP/3 INTERNAL_ERROR）。
    std::uint64_t LastErrorCode() const noexcept;

    /// 底层 picoquic 连接句柄（`picoquic_cnx_t*`），仅供调试/扩展用。
    void* NativeHandle() const noexcept;

private:
    /// pimpl：picoquic 类型完全藏在 .cpp 内，头文件零依赖。
    struct Impl;
    explicit PicoquicConnection(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::net::quic
