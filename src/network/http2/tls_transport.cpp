/**
 * @file tls_transport.cpp
 * @brief OpenSSL 版 Transport 实现（客户端 + ALPN）。
 */
#include "go2cpp/network/http2/tls_transport.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

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

std::unique_ptr<TlsTransport> TlsTransport::CreateClient(Socket::Ptr socket,
                                                         const std::string& server_name,
                                                         bool verify_peer,
                                                         Duration timeout) {
    if (!socket) {
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
        SSL_set_tlsext_host_name(transport->m_ssl, server_name.c_str());
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

bool TlsTransport::WaitFor(bool want_read) {
    struct pollfd pfd;
    pfd.fd = m_fd;
    pfd.events = want_read ? POLLIN : POLLOUT;
    pfd.revents = 0;
    const int result = ::poll(&pfd, 1, static_cast<int>(m_timeout.count()));
    if (result <= 0) {
        return false;
    }
    return (pfd.revents & (want_read ? POLLIN : POLLOUT)) != 0;
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
    for (;;) {
        ERR_clear_error();
        const int result = SSL_connect(m_ssl);
        if (result == 1) {
            break;
        }
        const int ssl_error = SSL_get_error(m_ssl, result);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!WaitFor(true)) {
                if (error != nullptr) *error = "TLS 握手等待可读超时";
                return false;
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!WaitFor(false)) {
                if (error != nullptr) *error = "TLS 握手等待可写超时";
                return false;
            }
            continue;
        }
        if (error != nullptr) {
            *error = "TLS 握手失败: " + LastErrorString();
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
    if (m_ssl == nullptr || buffer == nullptr || capacity == 0 || m_eof) {
        return 0;
    }
    for (;;) {
        ERR_clear_error();
        const int result = SSL_read(m_ssl, buffer, static_cast<int>(capacity));
        if (result > 0) {
            return static_cast<std::size_t>(result);
        }
        const int ssl_error = SSL_get_error(m_ssl, result);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!WaitFor(true)) return 0;
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!WaitFor(false)) return 0;
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
    if (m_ssl == nullptr || data == nullptr || length == 0 || m_eof) {
        return 0;
    }
    std::size_t total = 0;
    while (total < length) {
        ERR_clear_error();
        const int result =
            SSL_write(m_ssl, data + total, static_cast<int>(length - total));
        if (result > 0) {
            total += static_cast<std::size_t>(result);
            continue;
        }
        const int ssl_error = SSL_get_error(m_ssl, result);
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!WaitFor(false)) break;
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!WaitFor(true)) break;
            continue;
        }
        m_eof = true;
        break;
    }
    return total;
}

}  // namespace go2cpp::net::http2
