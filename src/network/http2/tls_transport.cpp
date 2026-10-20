/**
 * @file tls_transport.cpp
 * @brief OpenSSL 版 Transport 实现（客户端 + ALPN）。
 */
#include "go2cpp/network/http2/tls_transport.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstring>

namespace go2cpp::net::http2 {

namespace {

/// 客户端想协商的协议，按优先级排列。
const unsigned char kAlpnProtos[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

std::string DrainOpenSslErrors() {
    std::string out;
    char buffer[256] = {};
    unsigned long code = 0;
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buffer, sizeof(buffer));
        if (!out.empty()) {
            out += "; ";
        }
        out += buffer;
    }
    return out;
}

}  // namespace

TlsTransport::TlsTransport(Socket::Ptr socket, Duration timeout)
    : m_socket(std::move(socket)), m_timeout(timeout) {
    if (m_socket) {
        m_fd = m_socket->fd();
    }
}

std::unique_ptr<TlsTransport> TlsTransport::CreateClient(
    Socket::Ptr socket, const std::string& server_name, bool verify_peer,
    Duration timeout, const std::string& ca_file, const std::string& ca_path) {
    if (!socket || timeout.count() < 0 || timeout == Duration::max()) {
        // Duration::max() 以前会被 static_cast<int> 溢出成 -1，令 poll(-1)
        // 永久挂起；这里直接拒绝，调用方必须给有限的握手/读写 deadline。
        return nullptr;
    }
    auto transport = std::unique_ptr<TlsTransport>(new TlsTransport(std::move(socket), timeout));

    // OpenSSL 3.x 初始化是幂等的
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                         nullptr) == 0) {
        return nullptr;
    }
    transport->m_ctx = SSL_CTX_new(TLS_client_method());
    if (transport->m_ctx == nullptr) {
        return nullptr;
    }
    SSL_CTX_set_min_proto_version(transport->m_ctx, TLS1_2_VERSION);
    if (verify_peer) {
        SSL_CTX_set_verify(transport->m_ctx, SSL_VERIFY_PEER, nullptr);
        // 信任锚优先级：显式 ca_file/ca_path > 系统默认信任库。
        // 默认库会读取 SSL_CERT_FILE / SSL_CERT_DIR，便于容器与测试注入 CA。
        int loaded = 0;
        if (!ca_file.empty() || !ca_path.empty()) {
            loaded = SSL_CTX_load_verify_locations(
                transport->m_ctx, ca_file.empty() ? nullptr : ca_file.c_str(),
                ca_path.empty() ? nullptr : ca_path.c_str());
        } else {
            loaded = SSL_CTX_set_default_verify_paths(transport->m_ctx);
        }
        if (loaded != 1) {
            return nullptr;
        }
    } else {
        // 测试便利：不校验证书链。生产必须打开。
        SSL_CTX_set_verify(transport->m_ctx, SSL_VERIFY_NONE, nullptr);
    }

    transport->m_ssl = SSL_new(transport->m_ctx);
    if (transport->m_ssl == nullptr) {
        return nullptr;
    }
    // 关键：把 socket 设为非阻塞，避免 SSL 内部阻塞整个 worker 线程。
    // 本项目的 hook 会拦截 poll，让它变成 Fiber 友好的等待。
    ::fcntl(transport->m_fd, F_SETFL, ::fcntl(transport->m_fd, F_GETFL, 0) | O_NONBLOCK);
    if (SSL_set_fd(transport->m_ssl, transport->m_fd) != 1) {
        return nullptr;
    }
    if (!server_name.empty()) {
        if (SSL_set_tlsext_host_name(transport->m_ssl, server_name.c_str()) != 1) {
            return nullptr;
        }
    }
    // 主机名校验：verify_peer=true 且给了名字时，证书必须匹配 SAN/CN。
    // IP 字面量（例如 "127.0.0.1"）走 set1_ip_asc，DNS 走 SSL_set1_host。
    if (verify_peer && !server_name.empty()) {
        X509_VERIFY_PARAM* param = SSL_get0_param(transport->m_ssl);
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        unsigned char address[sizeof(struct in6_addr)] = {};
        const bool is_ip = ::inet_pton(AF_INET, server_name.c_str(), address) == 1 ||
                           ::inet_pton(AF_INET6, server_name.c_str(), address) == 1;
        const int verified = is_ip
                                 ? X509_VERIFY_PARAM_set1_ip_asc(param, server_name.c_str())
                                 : SSL_set1_host(transport->m_ssl, server_name.c_str());
        if (verified != 1) {
            return nullptr;
        }
    }
    if (SSL_set_alpn_protos(transport->m_ssl, kAlpnProtos, sizeof(kAlpnProtos)) != 0) {
        return nullptr;
    }
    return transport;
}

TlsTransport::~TlsTransport() {
    if (m_ssl != nullptr) {
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
    if (m_ctx != nullptr) {
        SSL_CTX_free(m_ctx);
        m_ctx = nullptr;
    }
}

std::chrono::steady_clock::time_point TlsTransport::Deadline() const {
    if (m_timeout == Duration::max()) {
        return std::chrono::steady_clock::time_point::max();
    }
    return std::chrono::steady_clock::now() + m_timeout;
}

bool TlsTransport::WaitFor(bool want_read,
                           std::chrono::steady_clock::time_point deadline) {
    // 100ms 分片：既让 Duration::max() 不再溢出成 poll(-1)，也让 Cancel()
    // 与 socket 关闭能在有限时间内打断等待；剩余时间按总 deadline 计算，
    // 不会像旧实现那样每次 WANT_READ/WANT_WRITE 都重新获得完整超时。
    constexpr Duration kSlice{100};
    for (;;) {
        if (m_cancelled.load(std::memory_order_acquire)) {
            return false;
        }
        if (!m_socket || !m_socket->valid()) {
            return false;
        }
        Duration slice = kSlice;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            const auto remaining =
                std::chrono::duration_cast<Duration>(deadline - now);
            if (remaining.count() <= 0) {
                return false;
            }
            if (remaining < slice) {
                slice = remaining;
            }
        }
        if (slice.count() <= 0) {
            slice = Duration{1};
        }
        struct pollfd pfd;
        pfd.fd = m_fd;
        pfd.events = want_read ? POLLIN : POLLOUT;
        pfd.revents = 0;
        const int result = ::poll(&pfd, 1, static_cast<int>(slice.count()));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            continue;  // 本片超时：回到循环顶部重查取消/关闭/总 deadline
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            // 交给 SSL_* 读取真实错误（EOF/EPIPE/ECONNRESET 等）。
            return true;
        }
        if ((pfd.revents & (want_read ? POLLIN : POLLOUT)) != 0) {
            return true;
        }
        return false;
    }
}

std::string TlsTransport::LastErrorString() const {
    std::string detail = DrainOpenSslErrors();
    if (detail.empty()) {
        detail = "无 OpenSSL 错误详情";
    }
    return detail;
}

bool TlsTransport::Handshake(std::string* error) {
    if (m_ssl == nullptr) {
        if (error != nullptr) *error = "TLS 尚未初始化";
        return false;
    }
    const auto deadline = Deadline();
    for (;;) {
        if (m_cancelled.load(std::memory_order_acquire)) {
            if (error != nullptr) *error = "TLS 握手已取消";
            return false;
        }
        ERR_clear_error();
        const int result = SSL_connect(m_ssl);
        if (result == 1) {
            break;
        }
        const int ssl_error = SSL_get_error(m_ssl, result);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!WaitFor(true, deadline)) {
                if (error != nullptr) {
                    *error = m_cancelled.load(std::memory_order_acquire)
                                 ? "TLS 握手已取消"
                                 : "TLS 握手等待可读超时";
                }
                return false;
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!WaitFor(false, deadline)) {
                if (error != nullptr) {
                    *error = m_cancelled.load(std::memory_order_acquire)
                                 ? "TLS 握手已取消"
                                 : "TLS 握手等待可写超时";
                }
                return false;
            }
            continue;
        }
        if (error != nullptr) {
            std::string detail = LastErrorString();
            // OpenSSL 3 的 ERR 队列对证书类失败只给笼统的
            // "certificate verify failed"；把 verify result 一并带上，
            // 调用方才分得清自签名、过期、主机名不匹配。
            const long verify_result = SSL_get_verify_result(m_ssl);
            if (verify_result != X509_V_OK) {
                detail += "; verify: ";
                detail += X509_verify_cert_error_string(verify_result);
            }
            *error = "TLS 握手失败: " + detail;
        }
        return false;
    }

    // 读取 ALPN 结果
    const unsigned char* proto = nullptr;
    unsigned int proto_len = 0;
    SSL_get0_alpn_selected(m_ssl, &proto, &proto_len);
    if (proto != nullptr && proto_len > 0) {
        m_alpn.assign(reinterpret_cast<const char*>(proto), proto_len);
    }
    return true;
}

std::string TlsTransport::TlsVersion() const {
    if (m_ssl == nullptr) {
        return {};
    }
    const char* version = SSL_get_version(m_ssl);
    return version != nullptr ? version : "";
}

std::size_t TlsTransport::Read(std::uint8_t* buffer, std::size_t capacity) {
    if (m_ssl == nullptr || buffer == nullptr || capacity == 0 || m_eof ||
        m_cancelled.load(std::memory_order_acquire)) {
        return 0;
    }
    const auto deadline = Deadline();
    // SSL_read 的 length 是 int：>2GiB 的容量按 INT_MAX 截断，不做隐式溢出。
    const int request = capacity > static_cast<std::size_t>(INT_MAX)
                            ? INT_MAX
                            : static_cast<int>(capacity);
    for (;;) {
        ERR_clear_error();
        const int result = SSL_read(m_ssl, buffer, request);
        if (result > 0) {
            return static_cast<std::size_t>(result);
        }
        const int ssl_error = SSL_get_error(m_ssl, result);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!WaitFor(true, deadline)) return 0;
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!WaitFor(false, deadline)) return 0;
            continue;
        }
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            m_eof = true;
            return 0;
        }
        if (ssl_error == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) {
            // 对端直接关闭 TCP，没有 close_notify
            m_eof = true;
            return 0;
        }
        m_eof = true;
        return 0;
    }
}

std::size_t TlsTransport::Write(const std::uint8_t* data, std::size_t length) {
    if (m_ssl == nullptr || data == nullptr || length == 0 || m_eof ||
        m_cancelled.load(std::memory_order_acquire)) {
        return 0;
    }
    const auto deadline = Deadline();
    std::size_t total = 0;
    while (total < length) {
        ERR_clear_error();
        const std::size_t remaining = length - total;
        const int chunk = remaining > static_cast<std::size_t>(INT_MAX)
                              ? INT_MAX
                              : static_cast<int>(remaining);
        const int result = SSL_write(m_ssl, data + total, chunk);
        if (result > 0) {
            total += static_cast<std::size_t>(result);
            continue;
        }
        const int ssl_error = SSL_get_error(m_ssl, result);
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!WaitFor(false, deadline)) break;
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!WaitFor(true, deadline)) break;
            continue;
        }
        m_eof = true;
        break;
    }
    return total;
}

}  // namespace go2cpp::net::http2
