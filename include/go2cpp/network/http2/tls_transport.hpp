/**
 * @file tls_transport.hpp
 * @brief 用 OpenSSL 给 `Transport` 加 TLS + ALPN，使 HTTP/2 能跑在 TLS 上。
 *
 * ## 为什么需要它
 *
 * 到这一层为止，HTTP/2 只在 h2c（明文）上验证过。但**真实互联网的 HTTP/2
 * 几乎全部跑在 TLS 上**，且必须通过 ALPN 协商出 `h2` —— 否则对端会当作
 * HTTP/1.1 处理。缺这一块，HTTP/2 实现就无法与真实站点对话。
 *
 * ## 并发模型
 *
 * 底层 socket 设为**非阻塞**，`SSL_connect`/`SSL_read`/`SSL_write` 返回
 * `SSL_ERROR_WANT_READ` / `SSL_ERROR_WANT_WRITE` 时，用 `poll` 等待 fd 就绪
 * 后重试。本项目的 hook 会拦截 `poll`，所以在 Fiber 里等待会把协程挂起而不是
 * 卡住线程 —— 与 `FiberTransport` 的行为一致。
 *
 * **不能**用阻塞 fd：那会让 SSL 内部阻塞整个 worker 线程。
 *
 * ## 安全默认
 *
 * - **默认校验对端证书**（`verify_peer = true`）：优先加载显式
 *   `ca_file`/`ca_path`，否则系统默认信任库（`SSL_CTX_set_default_verify_paths`，
 *   尊重 `SSL_CERT_FILE` / `SSL_CERT_DIR`）；`server_name` 非空时校验证书
 *   SAN/CN（IP 字面量走 `X509_VERIFY_PARAM_set1_ip_asc`）；
 * - 自签名/本地测试必须**显式**传 `verify_peer = false`，该开关只用于测试；
 *   默认值早年是 false（测试便利），审计发现「不安全默认」后改为 true。
 *   依赖旧默认的调用方会因证书不受信而握手失败，这是 fail-closed 的预期
 *   行为 —— 正确做法是配置 CA，而不是关闭校验；
 * - **服务端 TLS**（只有客户端）；
 * - **会话复用 / 0-RTT**。
 *
 * ## 构建开关（GO2CPP_WITH_TLS）
 *
 * 默认 ON，需要 OpenSSL。`-DGO2CPP_WITH_TLS=OFF` 时不链接
 * libssl/libcrypto：本类仍保留定义（保证既有目标可链接），但
 * `CreateClient()` 一律返回 `nullptr`、`Handshake()` 返回 false、
 * `Read()`/`Write()` 返回 0，**不会**静默降级成明文连接。
 *
 * ## 本文件定义什么
 * - `TlsTransport`：`Transport` 的 TLS 实现；`CreateClient()` 建上下文，
 *   `Handshake()` 走 TLS 握手并读取 ALPN 结果；
 * - 前置声明 `SSL_CTX` / `SSL`，把 OpenSSL 完全挡在头文件之外。
 *
 * ## 阅读建议
 * 先看文件头的「并发模型」（非阻塞 fd + poll），再看 `Read`/`Write` 的
 * WANT_READ/WANT_WRITE 重试，最后看 `Closed()` 的语义。
 *
 * ## 边界与陷阱
 * - 客户端 socket 会被设成**非阻塞**：阻塞 fd 会让 SSL 卡住整个 worker 线程；
 * - 证书校验默认**开启**（`verify_peer = true`，校验证书链 + 主机名）；
 *   确需跳过时显式 `verify_peer = false`（仅测试）；
 * - `Read`/`Write` 把超时、EOF、错误都收敛为「返回 0」，细节在公开的
 *   `LastError()`/`LastErrorString()` 里；
 * - `Close()`/析构会发 close_notify（只发一次，不等对端回包），保证对端
 *   能区分「正常结束」与「TCP 被截断」；
 * - 只实现客户端，没有服务端 TLS 与会话复用。
 */
#pragma once

#include "go2cpp/network/http2/transport.hpp"
#include "go2cpp/network/socket.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace go2cpp::net::http2 {

/**
 * @brief 基于 OpenSSL 的 Transport，支持 ALPN。
 * @note 非线程安全：与 `Connection` 一样，只由一个 Fiber 驱动。
 *
 * ## 定位
 * 被 `http2::Connection` 以 `Transport*` 注入，用于与真实站点（h2 over TLS）
 * 对话；由上层客户端/示例在 Fiber 中创建。与 `FiberTransport` 并列，区别是
 * 读写经过 OpenSSL 而非裸 socket。
 *
 * ## 不变量
 * - `m_ssl == nullptr` 时 `Closed()` 恒真，所有读写返回 0；
 * - `m_fd` 与 `m_socket->fd()` 一致，且被设为非阻塞；
 * - `m_eof` 一旦置位就不再尝试 SSL_read/SSL_write；
 * - 非线程安全：与 `Connection` 一样只由一个 Fiber 驱动；只有
 *   `LastError()`/`LastErrorString()` 允许从其它线程查询；
 * - `Close()` 之后 `Closed()` 为 true，且 close_notify 至多发送一次。
 */
class TlsTransport final : public Transport {
public:
    using Duration = std::chrono::milliseconds;

    /**
     * @brief 创建一个客户端 TLS 传输。
     * @param socket 已连接（TCP 握手完成）的 socket，所有权转入。
     * @param server_name SNI 主机名；可为空字符串。
     * @param verify_peer 是否校验证书链与主机名，**默认 true（安全默认）**；
     *        只有自签名证书的本地测试才显式传 false，生产禁用。
     * @param timeout 握手或**单次** Read/Write 的总 deadline（不是每轮
     *        WANT_READ/WANT_WRITE 重新计时）；必须是有限值。`Duration::max()`
     *        会被拒绝并返回 nullptr，避免 `poll(-1)` 永久挂起。
     * @param ca_file verify_peer=true 时显式 CA 文件（PEM）；为空则用 ca_path
     *        或系统默认信任库。
     * @param ca_path verify_peer=true 时显式 CA 目录（openssl rehash 过的目录）。
     * @return 成功返回对象；参数非法、OpenSSL 初始化或信任库加载失败返回 nullptr。
     *
     * ## 步骤
     * step1. `OPENSSL_init_ssl()`（3.x 幂等）；
     * step2. 建 `SSL_CTX`，设最低 TLS1.2 与校验模式（verify_peer 决定）；
     *        verify_peer=true 时加载 ca_file/ca_path 或默认信任库；
     * step3. `SSL_new`，把 fd 设为非阻塞（关键，见文件头）；
     * step4. 设置 SNI、主机名校验（`SSL_set1_host` / IP 参数）与 ALPN 列表
     *        （`h2` 优先、`http/1.1` 兜底）。
     */
    static std::unique_ptr<TlsTransport> CreateClient(
        Socket::Ptr socket, const std::string& server_name, bool verify_peer = true,
        Duration timeout = Duration{10000}, const std::string& ca_file = {},
        const std::string& ca_path = {});

    ~TlsTransport() override;
    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;

    /**
     * @brief 执行 TLS 握手，并读取 ALPN 协商结果。
     * @param error 失败时写入中文诊断。
     * @return 是否成功。
     *
     * ## 步骤
     * step1. 循环 `SSL_connect()`；
     * step2. `WANT_READ`/`WANT_WRITE` 时 `WaitFor()`（项目 hook 拦截 poll，Fiber 挂起）；
     *        每片最多 100ms 并共享同一个总 deadline，可被 `Cancel()` 打断；
     * step3. 成功后 `SSL_get0_alpn_selected()` 记录协商出的协议名。
     */
    bool Handshake(std::string* error = nullptr);

    /**
     * @brief 取消正在进行的握手/读写等待（线程安全）。
     * @details 置位取消标记；等待中的 `poll` 最多 100ms 后返回，后续
     *          `Read`/`Write` 返回 0、`Closed()` 变为 true。取消不可复位。
     */
    void Cancel() noexcept { m_cancelled.store(true, std::memory_order_release); }

    /** @brief ALPN 协商出的协议名（如 `"h2"`）；未协商或失败时为空。 */
    const std::string& NegotiatedProtocol() const noexcept { return m_alpn; }

    /** @brief TLS 版本字符串（诊断用）。 */
    std::string TlsVersion() const;

    /**
     * @brief 发送 close_notify 并结束 TLS 会话（可重复调用，只发一次）。
     * @details 析构会自动调用。只执行一次 `SSL_shutdown` 语义的关闭：把
     *          close_notify 写进 socket 即返回，不等待对端回包；写缓冲满时
     *          最多等待 `min(m_timeout, 500ms)` 处理 WANT_WRITE，避免析构
     *          在坏网络下无限挂起。调用后 `Closed()` 为 true。
     */
    void Close();

    /**
     * @brief 最近一次 `SSL_get_error()` 的结果（OpenSSL 的 `SSL_ERROR_*` 值）。
     * @return `0`（`SSL_ERROR_NONE`）表示尚未发生错误；`SSL_ERROR_SSL` 表示
     *         协议/证书错误，`SSL_ERROR_SYSCALL` 表示 TCP 层错误。
     */
    int LastError() const noexcept;

    /**
     * @brief 最近一次 TLS 错误的可读描述。
     * @details 包含 `SSL_ERROR_*` 名称、`ERR_get_error()` 队列文本，以及证书
     *          校验失败时的 `X509_verify_cert_error_string()` 原因（自签名、
     *          过期、主机名不匹配等）。未发生错误时返回空字符串。
     */
    std::string LastErrorString() const;

    /**
     * @brief 读最多 capacity 字节（非阻塞 fd + poll 重试）。
     *
     * ## 步骤
     * step1. `SSL_read()`；正数直接返回；
     * step2. WANT_READ/WANT_WRITE -> `WaitFor()` 后重试（共享本次调用的总 deadline，
     *        分片 100ms，可被 Cancel() 打断）；
     * step3. ZERO_RETURN / 无 close_notify 的 EOF -> 置 `m_eof` 并返回 0。
     */
    std::size_t Read(std::uint8_t* buffer, std::size_t capacity) override;
    /**
     * @brief 写全部 length 字节（循环处理 WANT_WRITE）。
     *
     * ## 步骤
     * step1. 循环 `SSL_write()` 累计已写字节；
     * step2. WANT_WRITE/WANT_READ -> `WaitFor()` 后重试（共享本次调用的总 deadline，
     *        分片 100ms，可被 Cancel() 打断）；
     * step3. 其它错误置 `m_eof` 并返回已写字节数（小于 length 即失败）。
     */
    std::size_t Write(const std::uint8_t* data, std::size_t length) override;
    bool Closed() const noexcept override {
        return m_ssl == nullptr || m_eof ||
               m_shutdown_sent.load(std::memory_order_acquire) ||
               m_cancelled.load(std::memory_order_acquire);
    }

private:
    TlsTransport(Socket::Ptr socket, Duration timeout);

    /// 本次操作的总 deadline；`Duration::max()` 在 CreateClient 已被拒绝。
    std::chrono::steady_clock::time_point Deadline() const;
    /// 等待 fd 就绪；按 100ms 分片检查取消/关闭/总 deadline。true 表示可以重试。
    bool WaitFor(bool want_read, std::chrono::steady_clock::time_point deadline);
    /// 记录最近一次 SSL_get_error 与 ERR 队列，供公开查询接口使用。
    void RecordSslError(int ssl_error);

    /// 底层 TCP socket（所有权在本类）；构造函数取 fd，Closed() 检查它。
    Socket::Ptr m_socket;
    /// SSL 上下文；CreateClient() 创建，析构释放。
    SSL_CTX* m_ctx{nullptr};
    /// SSL 会话；CreateClient() 创建，Handshake/Read/Write 使用，析构释放。
    SSL* m_ssl{nullptr};
    /// 握手或单次 Read/Write 的总 deadline 时长；WaitFor() 分片消费它。
    Duration m_timeout;
    /// ALPN 协商结果；Handshake() 写入，NegotiatedProtocol() 只读。
    std::string m_alpn;
    /// 读侧 EOF/不可用标记；Read/Write 置位，Closed() 读取。
    bool m_eof{false};
    /// 取消标记；Cancel() 置位，WaitFor()/Closed() 读取（只从 false 变 true）。
    std::atomic<bool> m_cancelled{false};
    /// close_notify 是否已发出；Close()/析构据此保证只发一次，Closed() 读取。
    std::atomic<bool> m_shutdown_sent{false};
    /// 最近一次 SSL_get_error 的结果；0 = SSL_ERROR_NONE，可跨线程查询。
    std::atomic<int> m_last_ssl_error{0};
    /// 最近一次错误的可读文本；由 m_error_mutex 保护。
    std::string m_last_error;
    /// 保护 m_last_error；RecordSslError() 写、LastErrorString() 读。
    mutable std::mutex m_error_mutex;
    /// socket fd 副本；构造时取，WaitFor() 与 SSL_set_fd() 使用。
    int m_fd{-1};
};

}  // namespace go2cpp::net::http2
