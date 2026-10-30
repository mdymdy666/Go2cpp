/**
 * @file tls_transport.cpp
 * @brief OpenSSL 版 Transport 实现（客户端 + ALPN）。
 */
#include "go2cpp/network/http2/tls_transport.hpp"

// TLS 是可选的：GO2CPP_WITH_TLS=OFF 时本文件只编译下面的 stub，不 include
// 任何 OpenSSL 头、也不要求链接 libssl/libcrypto。未定义宏时默认保持旧行为。
#if !defined(GO2CPP_WITH_TLS) || GO2CPP_WITH_TLS

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
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

/// 在作用域内屏蔽 SIGPIPE。
/// @details OpenSSL 的 socket BIO 用 write()，对端关闭后写会触发 SIGPIPE，
///          默认动作是**终止进程**。屏蔽后 write 返回 EPIPE，由调用方转成
///          `SSL_ERROR_SYSCALL` 处理。握手写 ClientHello、Write 写应用数据、
///          关闭写 close_notify 都必须带上它，否则「正常关闭」反而可能杀死
///          进程（实测对端先 close 时客户端析构触发 SIGPIPE）。用线程级
///          pthread_sigmask 而不是全局 signal()，不改变进程其它线程的行为。
class ScopedSigpipeBlock {
public:
    ScopedSigpipeBlock() {
        ::sigemptyset(&m_blocked);
        ::sigaddset(&m_blocked, SIGPIPE);
        m_active = ::pthread_sigmask(SIG_BLOCK, &m_blocked, &m_previous) == 0;
    }
    ~ScopedSigpipeBlock() {
        if (!m_active) {
            return;
        }
        // 关键：只恢复掩码还不够。写失败时内核把 SIGPIPE 记为「本线程
        // pending」，恢复掩码的一瞬间会按默认动作投递并终止进程（实测
        // 对端先 close 时客户端析构仍然 SIGPIPE）。恢复前必须先把本线程
        // pending 的 SIGPIPE 取走消费掉。
        sigset_t pending;
        if (::sigpending(&pending) == 0 && ::sigismember(&pending, SIGPIPE) == 1) {
            struct timespec zero {};
            siginfo_t info {};
            // SIGPIPE 是标准信号不排队；循环仅防御性处理多次 pending。
            while (::sigtimedwait(&m_blocked, &info, &zero) == SIGPIPE) {
            }
        }
        ::pthread_sigmask(SIG_SETMASK, &m_previous, nullptr);
    }
    ScopedSigpipeBlock(const ScopedSigpipeBlock&) = delete;
    ScopedSigpipeBlock& operator=(const ScopedSigpipeBlock&) = delete;

private:
    sigset_t m_previous{};
    sigset_t m_blocked{};
    bool m_active{false};
};

/// 把 `SSL_get_error()` 的常量映射成可读名字（日志/诊断用）。
const char* SslErrorName(int ssl_error) {
    switch (ssl_error) {
        case SSL_ERROR_NONE: return "SSL_ERROR_NONE";
        case SSL_ERROR_SSL: return "SSL_ERROR_SSL";
        case SSL_ERROR_WANT_READ: return "SSL_ERROR_WANT_READ";
        case SSL_ERROR_WANT_WRITE: return "SSL_ERROR_WANT_WRITE";
        case SSL_ERROR_WANT_X509_LOOKUP: return "SSL_ERROR_WANT_X509_LOOKUP";
        case SSL_ERROR_SYSCALL: return "SSL_ERROR_SYSCALL";
        case SSL_ERROR_ZERO_RETURN: return "SSL_ERROR_ZERO_RETURN";
        case SSL_ERROR_WANT_CONNECT: return "SSL_ERROR_WANT_CONNECT";
        case SSL_ERROR_WANT_ACCEPT: return "SSL_ERROR_WANT_ACCEPT";
#ifdef SSL_ERROR_WANT_ASYNC
        case SSL_ERROR_WANT_ASYNC: return "SSL_ERROR_WANT_ASYNC";
#endif
#ifdef SSL_ERROR_WANT_ASYNC_JOB
        case SSL_ERROR_WANT_ASYNC_JOB: return "SSL_ERROR_WANT_ASYNC_JOB";
#endif
#ifdef SSL_ERROR_WANT_CLIENT_HELLO_CB
        case SSL_ERROR_WANT_CLIENT_HELLO_CB: return "SSL_ERROR_WANT_CLIENT_HELLO_CB";
#endif
#ifdef SSL_ERROR_WANT_RETRY_VERIFY
        case SSL_ERROR_WANT_RETRY_VERIFY: return "SSL_ERROR_WANT_RETRY_VERIFY";
#endif
        default: break;
    }
    return "SSL_ERROR_UNKNOWN";
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
    // 先发 close_notify 再释放 SSL：否则对端只会看到 TCP FIN，无法区分
    // 「正常结束」与「响应被截断」，HTTP/2 层可能把半截响应当成完整响应。
    Close();
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

void TlsTransport::RecordSslError(int ssl_error) {
    m_last_ssl_error.store(ssl_error, std::memory_order_relaxed);
    std::string detail = SslErrorName(ssl_error);
    const std::string queue = DrainOpenSslErrors();
    if (!queue.empty()) {
        detail += ": ";
        detail += queue;
    }
    // OpenSSL 3 的 ERR 队列对证书类失败只给笼统的 "certificate verify
    // failed"；把 verify result 一并带上，调用方才分得清自签名、过期、
    // 主机名不匹配。只有开启了对端校验的会话才附加：verify_peer=false 时
    // SSL_get_verify_result 可能残留自签名等结果，附在无关的 EOF 错误
    // 后面会误导排错。
    const bool verify_enabled =
        m_ssl != nullptr && (SSL_get_verify_mode(m_ssl) & SSL_VERIFY_PEER) != 0;
    if (verify_enabled) {
        const long verify_result = SSL_get_verify_result(m_ssl);
        if (verify_result != X509_V_OK) {
            detail += "; verify: ";
            detail += X509_verify_cert_error_string(verify_result);
        }
    }
    std::lock_guard<std::mutex> lock(m_error_mutex);
    m_last_error = std::move(detail);
}

int TlsTransport::LastError() const noexcept {
    return m_last_ssl_error.load(std::memory_order_relaxed);
}

std::string TlsTransport::LastErrorString() const {
    {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        if (!m_last_error.empty()) {
            return m_last_error;
        }
    }
    // 还没有记录过错误：返回实时队列（保留旧的排查路径）；队列也为空
    // 说明确实没有错误，返回空串（与公开接口文档一致）。
    return DrainOpenSslErrors();
}

void TlsTransport::Close() {
    if (m_ssl == nullptr) {
        return;
    }
    if (m_shutdown_sent.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // 关闭是「尽力而为」：把 close_notify 写出去就返回，不等待对端回包
    // （那会把析构拖到对端的超时）。WANT_WRITE 说明 close_notify 还没
    // 写进 socket，必须重试；预算取 min(m_timeout, 500ms)，避免坏网络下
    // 析构长时间挂起。WANT_READ 只可能出现在等待对端 close_notify 阶段，
    // 此时本端通知已经发出，直接结束。
    // 对端可能已经先关闭：向死连接写 close_notify 会触发 SIGPIPE，必须
    // 屏蔽，让 SSL_shutdown 返回错误而不是终止进程。
    [[maybe_unused]] ScopedSigpipeBlock sigpipe_block;
    const auto budget = std::min(m_timeout, Duration{500});
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (int attempt = 0; attempt < 8; ++attempt) {
        ERR_clear_error();
        const int result = SSL_shutdown(m_ssl);
        if (result >= 0) {
            // 1 = 双向关闭完成；0 = close_notify 已写出、正在等对端回包。
            // 两种情况下本端 close_notify 都已经交给底层 socket。
            m_eof = true;
            return;
        }
        const int ssl_error = SSL_get_error(m_ssl, result);
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!WaitFor(false, deadline)) {
                return;  // 预算耗尽：不再等待，socket 关闭时会发 FIN
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_READ) {
            m_eof = true;
            return;
        }
        RecordSslError(ssl_error);
        m_eof = true;
        return;
    }
}

bool TlsTransport::Handshake(std::string* error) {
    if (m_ssl == nullptr) {
        if (error != nullptr) *error = "TLS 尚未初始化";
        return false;
    }
    // 对端可能在握手期间关闭；ClientHello 写失败不能把进程 SIGPIPE 掉。
    [[maybe_unused]] ScopedSigpipeBlock sigpipe_block;
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
        RecordSslError(ssl_error);
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
            // 对端正常关闭（收到 close_notify）：不是错误，不覆盖 LastError。
            m_eof = true;
            return 0;
        }
        if (ssl_error == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) {
            // 对端直接关闭 TCP，没有 close_notify。
            RecordSslError(ssl_error);
            m_eof = true;
            return 0;
        }
        RecordSslError(ssl_error);
        m_eof = true;
        return 0;
    }
}

std::size_t TlsTransport::Write(const std::uint8_t* data, std::size_t length) {
    if (m_ssl == nullptr || data == nullptr || length == 0 || m_eof ||
        m_cancelled.load(std::memory_order_acquire)) {
        return 0;
    }
    // 对端可能已经关闭；应用数据写失败同样不能把进程 SIGPIPE 掉。
    [[maybe_unused]] ScopedSigpipeBlock sigpipe_block;
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
        RecordSslError(ssl_error);
        m_eof = true;
        break;
    }
    return total;
}

}  // namespace go2cpp::net::http2

#else  // GO2CPP_WITH_TLS == 0

#include <string>
#include <utility>

namespace go2cpp::net::http2 {

namespace {

/// 编译期关闭 TLS 时的统一诊断文本。
constexpr const char* kTlsDisabledMessage =
    "TLS 支持未编译：请在 CMake 中设置 -DGO2CPP_WITH_TLS=ON 并安装 OpenSSL";

}  // namespace

TlsTransport::TlsTransport(Socket::Ptr socket, Duration timeout)
    : m_socket(std::move(socket)), m_timeout(timeout) {}

TlsTransport::~TlsTransport() = default;

std::unique_ptr<TlsTransport> TlsTransport::CreateClient(
    Socket::Ptr socket, const std::string& server_name, bool verify_peer,
    Duration timeout, const std::string& ca_file, const std::string& ca_path) {
    (void)socket;
    (void)server_name;
    (void)verify_peer;
    (void)timeout;
    (void)ca_file;
    (void)ca_path;
    return nullptr;
}

std::chrono::steady_clock::time_point TlsTransport::Deadline() const {
    return std::chrono::steady_clock::now();
}

bool TlsTransport::WaitFor(bool want_read,
                           std::chrono::steady_clock::time_point deadline) {
    (void)want_read;
    (void)deadline;
    return false;
}

void TlsTransport::RecordSslError(int ssl_error) {
    m_last_ssl_error.store(ssl_error, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_error_mutex);
    m_last_error = kTlsDisabledMessage;
}

int TlsTransport::LastError() const noexcept {
    return m_last_ssl_error.load(std::memory_order_acquire);
}

std::string TlsTransport::LastErrorString() const {
    std::lock_guard<std::mutex> lock(m_error_mutex);
    return m_last_error.empty() ? std::string(kTlsDisabledMessage) : m_last_error;
}

void TlsTransport::Close() {
    m_shutdown_sent.store(true, std::memory_order_release);
    m_eof = true;
}

bool TlsTransport::Handshake(std::string* error) {
    if (error != nullptr) {
        *error = kTlsDisabledMessage;
    }
    m_eof = true;
    return false;
}

std::string TlsTransport::TlsVersion() const {
    return std::string();
}

std::size_t TlsTransport::Read(std::uint8_t* buffer, std::size_t capacity) {
    (void)buffer;
    (void)capacity;
    m_eof = true;
    return 0;
}

std::size_t TlsTransport::Write(const std::uint8_t* data, std::size_t length) {
    (void)data;
    (void)length;
    m_eof = true;
    return 0;
}

}  // namespace go2cpp::net::http2

#endif  // GO2CPP_WITH_TLS
