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
 * - **服务端 TLS**（只有客户端）；
 * - **会话复用 / 0-RTT**。
 */
#pragma once

#include "go2cpp/network/http2/transport.hpp"
#include "go2cpp/network/socket.hpp"

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
 */
class TlsTransport final : public Transport {
public:
    using Duration = std::chrono::milliseconds;

    /**
     * @brief 创建一个客户端 TLS 传输。
     * @param socket 已连接（TCP 握手完成）的 socket，所有权转入。
     * @param server_name SNI 主机名；可为空字符串。
     * @param verify_peer 是否校验证书链；测试自签名证书时传 false。
     * @param timeout 单次读写的等待超时。
     * @return 成功返回对象；OpenSSL 初始化失败返回 nullptr。
     */
    static std::unique_ptr<TlsTransport> CreateClient(Socket::Ptr socket,
                                                      const std::string& server_name,
                                                      bool verify_peer = false,
                                                      Duration timeout = Duration{10000});

    ~TlsTransport() override;
    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;

    /**
     * @brief 执行 TLS 握手，并读取 ALPN 协商结果。
     * @param error 失败时写入中文诊断。
     * @return 是否成功。
     */
    bool Handshake(std::string* error = nullptr);

    /** @brief ALPN 协商出的协议名（如 `"h2"`）；未协商或失败时为空。 */
    const std::string& NegotiatedProtocol() const noexcept { return m_alpn; }

    /** @brief TLS 版本字符串（诊断用）。 */
    std::string TlsVersion() const;

    std::size_t Read(std::uint8_t* buffer, std::size_t capacity) override;
    std::size_t Write(const std::uint8_t* data, std::size_t length) override;
    bool Closed() const noexcept override { return m_ssl == nullptr || m_eof; }

private:
    TlsTransport(Socket::Ptr socket, Duration timeout);

    /// 等待 fd 就绪；返回 true 表示可以重试。
    bool WaitFor(bool want_read);
    /// 把 OpenSSL 错误转成可读文本。
    std::string LastErrorString() const;

    Socket::Ptr m_socket;
    SSL_CTX* m_ctx{nullptr};
    SSL* m_ssl{nullptr};
    Duration m_timeout;
    std::string m_alpn;
    bool m_eof{false};
    int m_fd{-1};
};

}  // namespace go2cpp::net::http2
