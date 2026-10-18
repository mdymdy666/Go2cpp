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

    /// SNI（TLS 里的服务器名）。
    std::string sni{"localhost"};

    /// 是否验证对端证书。
    ///
    /// **默认 `false`** —— 这与安全直觉相反，理由：本项目目前的验证场景都是
    /// **本地自签名证书**，严格验证会直接握手失败。生产环境必须设为 `true`
    /// 并配置根证书。这个默认值是**测试便利**，不是安全默认。
    bool verify_peer{false};

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
 * 被 QUIC/HTTP-3 的示例与测试使用（`example/network/quic_*`、`tests/network/quic_*`），
 * 字节通道由调用方以 `net::http2::Transport*` 注入。与 `http2::Connection` 并列：
 * 后者自己实现协议，前者把协议全部交给 picoquic，只做 Fiber 粘合。
 *
 * ## 不变量
 * - 所有方法必须在同一线程/Fiber 序列调用（非线程安全）；
 * - `HandshakeComplete()` 为真之前 `WriteStream()` 不可用；
 * - `ReadStream()` 返回 0 表示「该流暂时没有数据」，不是 EOF；
 * - 对象不拥有 `Transport` 的生命周期；析构只释放 picoquic 上下文（`picoquic_free` 会连带释放 cnx）。
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
     * @brief 推进一次协议状态机。
     *
     * 做三件事：
     * 1. 调 `picoquic_prepare_next_packet` 循环，把**所有待发**的包写到 `transport`；
     * 2. 从 `transport` 读**一个** datagram，喂给 `picoquic_incoming_packet`；
     * 3. 处理定时器（重传、idle 超时）。
     *
     * @return 连接仍存活返回 true；已关闭/出错返回 false 并填 `error`。
     *
     * @note 第 2 步的 `transport->Read` 会**阻塞**（在 Fiber 里就是 park 当前
     *       Fiber，不阻塞 worker），所以 `Poll` 是"驱动一次收发"的语义，
     *       不是忙轮询。
     */
    bool Poll(std::string* error);

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

    /// 已收到数据的流号列表（便于调用方知道哪些流有数据）。
    void ReceivedStreamIds(std::uint64_t* out, std::size_t capacity,
                           std::size_t* count) const;

    /// 最近一次错误码（picoquic 的 application/transport error code）。
    std::uint64_t LastErrorCode() const noexcept;

    /// 底层 picoquic 连接句柄（`picoquic_cnx_t*`），仅供调试/扩展用。
    void* NativeHandle() const noexcept;

private:
    struct Impl;
    explicit PicoquicConnection(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::net::quic
