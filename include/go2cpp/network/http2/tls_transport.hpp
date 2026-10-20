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
 * ## 明确未实现
 *
 * - **证书校验默认关闭**（`verify_peer = false`）。生产必须打开并配置 CA
 *   路径，否则等于不做身份验证 —— 这里为了让自签名证书的本地测试能跑通
 *   才默认关闭，属于**测试便利**而非安全默认；
 * - `verify_peer = true` 时：优先加载显式 `ca_file`/`ca_path`，否则加载系统
 *   默认信任库（`SSL_CTX_set_default_verify_paths`，尊重 `SSL_CERT_FILE` /
 *   `SSL_CERT_DIR`）；`server_name` 非空时校验证书 SAN/CN（IP 字面量走
 *   `X509_VERIFY_PARAM_set1_ip_asc`）；
 * - **服务端 TLS**（只有客户端）；
 * - **会话复用 / 0-RTT**。
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
 * - 证书校验默认关闭（`verify_peer = false`），生产必须打开并配置 CA；
 * - `Read`/`Write` 把超时、EOF、错误都收敛为「返回 0」，细节在
 *   `LastErrorString()`/`last_error` 路径里；
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
 * - 非线程安全：与 `Connection` 一样只由一个 Fiber 驱动。
 */
class TlsTransport final : public Transport {
public:
    using Duration = std::chrono::milliseconds;

    /**
     * @brief 创建一个客户端 TLS 传输。
     * @param socket 已连接（TCP 握手完成）的 socket，所有权转入。
     * @param server_name SNI 主机名；可为空字符串。
     * @param verify_peer 是否校验证书链与主机名；测试自签名证书时传 false。
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
        Socket::Ptr socket, const std::string& server_name, bool verify_peer = false,
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
               m_cancelled.load(std::memory_order_acquire);
    }

private:
    TlsTransport(Socket::Ptr socket, Duration timeout);

    /// 本次操作的总 deadline；`Duration::max()` 在 CreateClient 已被拒绝。
    std::chrono::steady_clock::time_point Deadline() const;
    /// 等待 fd 就绪；按 100ms 分片检查取消/关闭/总 deadline。true 表示可以重试。
    bool WaitFor(bool want_read, std::chrono::steady_clock::time_point deadline);
    /// 把 OpenSSL 错误转成可读文本。
    std::string LastErrorString() const;

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
    /// socket fd 副本；构造时取，WaitFor() 与 SSL_set_fd() 使用。
    int m_fd{-1};
};

}  // namespace go2cpp::net::http2
