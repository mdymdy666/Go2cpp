/**
 * @file bench_tls.cpp
 * @brief TLS 传输层三组基准：握手延迟 / 32KB 写吞吐 / 64B 小包写延迟。
 * @details 手工基准（不进 ctest）。本地回环 + 进程内自建 OpenSSL 服务端
 *          （自签 P-256 证书，verify_peer=false 仅用于本地测试），客户端两套实现：
 *
 *          - `TlsTransport`：`CreateClient` + 非阻塞 fd + `poll` 状态机；
 *            每次连接新建 `SSL_CTX`（库的固有语义）。
 *          - 裸 OpenSSL：共享 `SSL_CTX` / 每次新建 `SSL_CTX` 两种，
 *            同样非阻塞 fd + `poll` 状态机，等待策略与 `TlsTransport` 对齐。
 *
 *          三轮取中位；延迟比值 = TlsTransport / 裸 OpenSSL，吞吐比值 =
 *          裸 OpenSSL / TlsTransport（>1 表示封装更慢）。
 *
 *          构建（需 GO2CPP_WITH_TLS=ON 的 go2cpp_network）：
 *            g++ -O2 -DNDEBUG -std=c++17 -I include tests/network/bench_tls.cpp \
 *                -L/tmp/build-tls-bench -lgo2cpp_network -lgo2cpp_scheduler \
 *                -Wl,-rpath,/tmp/build-tls-bench -lssl -lcrypto -lpthread \
 *                -o /tmp/build-tls-bench/bench_tls
 *
 *          运行：taskset -c 2,3 /tmp/build-tls-bench/bench_tls
 *
 * @author go2cpp contributors
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "go2cpp/network/http2/tls_transport.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/scheduler.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::net::IPv4Address;
using go2cpp::net::Socket;
using go2cpp::net::http2::TlsTransport;

constexpr int kHandshakeIters = 300;
constexpr std::size_t kWriteChunk = 32 * 1024;
constexpr std::size_t kWriteChunks = 8192;  // 32KB × 8192 = 256 MiB
constexpr std::size_t kSmallBytes = 64;
constexpr std::size_t kSmallOps = 50000;
constexpr int kRounds = 3;

double NsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
}

std::string OpensslError(const char* what) {
    char buffer[256] = {};
    const unsigned long code = ERR_get_error();
    if (code == 0) {
        return std::string(what);
    }
    ERR_error_string_n(code, buffer, sizeof(buffer));
    return std::string(what) + ": " + buffer;
}

// ---------------------------------------------------------------- 证书 / 服务端

bool MakeSelfSigned(SSL_CTX* ctx, std::string* error) {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (pkey == nullptr) {
        *error = OpensslError("EVP_EC_gen");
        return false;
    }
    X509* cert = X509_new();
    if (cert == nullptr) {
        EVP_PKEY_free(pkey);
        *error = OpensslError("X509_new");
        return false;
    }
    bool ok = X509_set_version(cert, 2) == 1;
    ok = ok && ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1;
    ok = ok && X509_gmtime_adj(X509_getm_notBefore(cert), 0) != nullptr;
    ok = ok && X509_gmtime_adj(X509_getm_notAfter(cert), 24 * 3600) != nullptr;
    ok = ok && X509_set_pubkey(cert, pkey) == 1;
    X509_NAME* name = X509_get_subject_name(cert);
    ok = ok && name != nullptr;
    if (ok) {
        ok = X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                        reinterpret_cast<const unsigned char*>("localhost"), -1,
                                        -1, 0) == 1;
    }
    ok = ok && X509_set_issuer_name(cert, name) == 1;
    if (ok) {
        X509V3_CTX v3;
        X509V3_set_ctx_nodb(&v3);
        X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
        X509_EXTENSION* san = X509V3_EXT_conf_nid(
            nullptr, &v3, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
        if (san != nullptr) {
            ok = X509_add_ext(cert, san, -1) == 1;
            X509_EXTENSION_free(san);
        } else {
            ok = false;
        }
    }
    ok = ok && X509_sign(cert, pkey, EVP_sha256()) > 0;
    ok = ok && SSL_CTX_use_certificate(ctx, cert) == 1;
    ok = ok && SSL_CTX_use_PrivateKey(ctx, pkey) == 1;
    if (!ok) {
        *error = OpensslError("build self-signed certificate");
    }
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return ok;
}

/// 进程内 TLS 服务端：loopback，自签证书，串行处理连接。
class TlsServer final {
public:
    bool Start(std::string* error) {
        ctx_ = SSL_CTX_new(TLS_server_method());
        if (ctx_ == nullptr) {
            *error = OpensslError("SSL_CTX_new(server)");
            return false;
        }
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        if (!MakeSelfSigned(ctx_, error)) {
            return false;
        }
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) {
            *error = std::string("socket: ") + std::strerror(errno);
            return false;
        }
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            *error = std::string("bind: ") + std::strerror(errno);
            return false;
        }
        if (::listen(listen_fd_, 128) != 0) {
            *error = std::string("listen: ") + std::strerror(errno);
            return false;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            *error = std::string("getsockname: ") + std::strerror(errno);
            return false;
        }
        port_ = ntohs(addr.sin_port);
        const int flags = ::fcntl(listen_fd_, F_GETFL, 0);
        ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
        thread_ = std::thread([this] { Loop(); });
        return true;
    }

    std::uint16_t port() const noexcept { return port_; }

    void Stop() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

    ~TlsServer() { Stop(); }

private:
    void ServeConnection(int fd) {
        SSL* ssl = SSL_new(ctx_);
        if (ssl == nullptr) {
            ::close(fd);
            return;
        }
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) != 1) {
            ERR_clear_error();
            SSL_free(ssl);
            ::close(fd);
            return;
        }
        std::vector<std::uint8_t> buffer(64 * 1024);
        for (;;) {
            const int got = SSL_read(ssl, buffer.data(), static_cast<int>(buffer.size()));
            if (got <= 0) {
                break;
            }
            ++served_bytes_;
        }
        ERR_clear_error();
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(fd);
    }

    void Loop() {
        while (!stop_.load(std::memory_order_acquire)) {
            pollfd pfd{listen_fd_, POLLIN, 0};
            const int ready = ::poll(&pfd, 1, 100);
            if (ready <= 0) {
                continue;
            }
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (fd < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                break;
            }
            ServeConnection(fd);
        }
    }

    int listen_fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> served_bytes_{0};
    std::thread thread_;
};

// ---------------------------------------------------------------- 裸 OpenSSL 客户端

bool WaitFd(int fd, short events, int timeout_ms) {
    pollfd pfd{fd, events, 0};
    int ready = 0;
    do {
        ready = ::poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

/// 裸 OpenSSL 客户端：非阻塞 fd + poll 状态机（与 TlsTransport 等待策略对齐）。
class RawTlsClient final {
public:
    ~RawTlsClient() { Close(); }

    static SSL_CTX* NewClientContext() {
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr) {
            return nullptr;
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        return ctx;
    }

    bool Connect(SSL_CTX* ctx, std::uint16_t port, std::string* error) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            *error = std::string("socket: ") + std::strerror(errno);
            return false;
        }
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        int rc = ::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (rc != 0 && errno != EINPROGRESS) {
            *error = std::string("connect: ") + std::strerror(errno);
            return false;
        }
        if (rc != 0) {
            if (!WaitFd(fd_, POLLOUT, 5000)) {
                *error = "connect timed out";
                return false;
            }
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error != 0) {
                *error = std::string("connect: ") + std::strerror(so_error);
                return false;
            }
        }
        ssl_ = SSL_new(ctx);
        if (ssl_ == nullptr) {
            *error = OpensslError("SSL_new");
            return false;
        }
        SSL_set_fd(ssl_, fd_);
        SSL_set_connect_state(ssl_);
        return true;
    }

    bool Handshake(std::string* error) {
        for (;;) {
            const int rc = SSL_connect(ssl_);
            if (rc == 1) {
                return true;
            }
            const int reason = SSL_get_error(ssl_, rc);
            short events = 0;
            if (reason == SSL_ERROR_WANT_READ) {
                events = POLLIN;
            } else if (reason == SSL_ERROR_WANT_WRITE) {
                events = POLLOUT;
            } else {
                *error = OpensslError("SSL_connect");
                return false;
            }
            if (!WaitFd(fd_, events, 100)) {
                *error = "SSL_connect timed out";
                return false;
            }
        }
    }

    bool WriteAll(const std::uint8_t* data, std::size_t length) {
        const std::uint8_t* cursor = data;
        std::size_t remaining = length;
        while (remaining > 0) {
            const int written = SSL_write(ssl_, cursor, static_cast<int>(remaining));
            if (written > 0) {
                cursor += written;
                remaining -= static_cast<std::size_t>(written);
                continue;
            }
            const int reason = SSL_get_error(ssl_, written);
            short events = 0;
            if (reason == SSL_ERROR_WANT_READ) {
                events = POLLIN;
            } else if (reason == SSL_ERROR_WANT_WRITE) {
                events = POLLOUT;
            } else {
                return false;
            }
            if (!WaitFd(fd_, events, 1000)) {
                return false;
            }
        }
        return true;
    }

    const char* Version() const { return ssl_ != nullptr ? SSL_get_version(ssl_) : ""; }

    void Close() {
        if (ssl_ != nullptr) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    SSL* ssl_ = nullptr;
};

// ---------------------------------------------------------------- 统计与场景

struct Latency {
    std::vector<double> ns;

    void Add(double value) { ns.push_back(value); }

    double Mean() const {
        double sum = 0;
        for (double v : ns) {
            sum += v;
        }
        return ns.empty() ? 0.0 : sum / static_cast<double>(ns.size());
    }
    double P50() const {
        if (ns.empty()) {
            return 0.0;
        }
        std::vector<double> sorted = ns;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2];
    }
    double P99() const {
        if (ns.empty()) {
            return 0.0;
        }
        std::vector<double> sorted = ns;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t index =
            std::min(sorted.size() - 1, sorted.size() * 99 / 100);
        return sorted[index];
    }
};

struct RoundResult {
    Latency hs_tt;
    Latency hs_raw_shared;
    Latency hs_raw_fresh;
    double write_tt_mibps = 0;
    double write_raw_mibps = 0;
    double small_tt_ns = 0;
    double small_raw_ns = 0;
    std::string tls_version;
};

/// 握手一次（含 TCP 建连），返回耗时 ns。TlsTransport 侧每次新建 SSL_CTX。
bool HandshakeTlsTransport(std::uint16_t port, std::string* error, double* ns,
                           std::string* version) {
    auto address = IPv4Address::Create("127.0.0.1", port);
    if (!address) {
        *error = "IPv4Address::Create failed";
        return false;
    }
    auto socket = Socket::CreateTCP(AF_INET);
    if (!socket) {
        *error = "Socket::CreateTCP failed";
        return false;
    }
    if (!socket->connect(*address, std::chrono::milliseconds(5000))) {
        *error = "TCP connect failed";
        return false;
    }
    const Clock::time_point t0 = Clock::now();
    auto transport = TlsTransport::CreateClient(socket, "localhost", false,
                                                std::chrono::milliseconds(10000));
    bool ok = transport != nullptr;
    if (!ok) {
        *error = "TlsTransport::CreateClient failed";
    } else {
        std::string handshake_error;
        ok = transport->Handshake(&handshake_error);
        if (!ok) {
            *error = handshake_error.empty() ? "TLS handshake failed" : handshake_error;
        } else if (version->empty()) {
            *version = transport->TlsVersion();
        }
    }
    *ns = NsSince(t0);
    if (transport) {
        transport->Close();
    }
    return ok;
}

bool HandshakeRaw(SSL_CTX* ctx, std::uint16_t port, std::string* error, double* ns,
                  std::string* version) {
    RawTlsClient client;
    const Clock::time_point t0 = Clock::now();
    if (!client.Connect(ctx, port, error) || !client.Handshake(error)) {
        *ns = NsSince(t0);
        return false;
    }
    *ns = NsSince(t0);
    if (version->empty()) {
        *version = client.Version();
    }
    client.Close();
    return true;
}

bool BenchWriteTlsTransport(std::uint16_t port, double* mibps, std::string* error) {
    auto address = IPv4Address::Create("127.0.0.1", port);
    auto socket = Socket::CreateTCP(AF_INET);
    if (!address || !socket ||
        !socket->connect(*address, std::chrono::milliseconds(5000))) {
        *error = "TlsTransport write: TCP connect failed";
        return false;
    }
    auto transport = TlsTransport::CreateClient(socket, "localhost", false,
                                                std::chrono::milliseconds(10000));
    if (!transport || !transport->Handshake(error)) {
        *error = error->empty() ? "TlsTransport write: handshake failed" : *error;
        return false;
    }
    std::vector<std::uint8_t> chunk(kWriteChunk, 0x5A);
    const Clock::time_point t0 = Clock::now();
    for (std::size_t i = 0; i < kWriteChunks; ++i) {
        if (transport->Write(chunk.data(), chunk.size()) != chunk.size()) {
            *error = "TlsTransport::Write short write";
            return false;
        }
    }
    const double seconds = NsSince(t0) / 1e9;
    const double bytes = static_cast<double>(kWriteChunk * kWriteChunks);
    *mibps = bytes / (1024.0 * 1024.0) / seconds;
    transport->Close();
    return true;
}

bool BenchWriteRaw(SSL_CTX* ctx, std::uint16_t port, double* mibps, std::string* error) {
    RawTlsClient client;
    if (!client.Connect(ctx, port, error) || !client.Handshake(error)) {
        return false;
    }
    std::vector<std::uint8_t> chunk(kWriteChunk, 0x5A);
    const Clock::time_point t0 = Clock::now();
    for (std::size_t i = 0; i < kWriteChunks; ++i) {
        if (!client.WriteAll(chunk.data(), chunk.size())) {
            *error = "raw SSL_write failed";
            return false;
        }
    }
    const double seconds = NsSince(t0) / 1e9;
    const double bytes = static_cast<double>(kWriteChunk * kWriteChunks);
    *mibps = bytes / (1024.0 * 1024.0) / seconds;
    client.Close();
    return true;
}

bool BenchSmallTlsTransport(std::uint16_t port, double* ns_per_op, std::string* error) {
    auto address = IPv4Address::Create("127.0.0.1", port);
    auto socket = Socket::CreateTCP(AF_INET);
    if (!address || !socket ||
        !socket->connect(*address, std::chrono::milliseconds(5000))) {
        *error = "TlsTransport small: TCP connect failed";
        return false;
    }
    auto transport = TlsTransport::CreateClient(socket, "localhost", false,
                                                std::chrono::milliseconds(10000));
    if (!transport || !transport->Handshake(error)) {
        *error = error->empty() ? "TlsTransport small: handshake failed" : *error;
        return false;
    }
    std::vector<std::uint8_t> packet(kSmallBytes, 0x33);
    const Clock::time_point t0 = Clock::now();
    for (std::size_t i = 0; i < kSmallOps; ++i) {
        if (transport->Write(packet.data(), packet.size()) != packet.size()) {
            *error = "TlsTransport::Write short write (small)";
            return false;
        }
    }
    *ns_per_op = NsSince(t0) / static_cast<double>(kSmallOps);
    transport->Close();
    return true;
}

bool BenchSmallRaw(SSL_CTX* ctx, std::uint16_t port, double* ns_per_op, std::string* error) {
    RawTlsClient client;
    if (!client.Connect(ctx, port, error) || !client.Handshake(error)) {
        return false;
    }
    std::vector<std::uint8_t> packet(kSmallBytes, 0x33);
    const Clock::time_point t0 = Clock::now();
    for (std::size_t i = 0; i < kSmallOps; ++i) {
        if (!client.WriteAll(packet.data(), packet.size())) {
            *error = "raw SSL_write failed (small)";
            return false;
        }
    }
    *ns_per_op = NsSince(t0) / static_cast<double>(kSmallOps);
    client.Close();
    return true;
}

bool RunRound(int round, std::uint16_t port, SSL_CTX* raw_shared, RoundResult* out,
              std::string* error) {
    std::printf("[bench-tls] ---- round %d/%d ----\n", round + 1, kRounds);
    std::fflush(stdout);

    for (int i = 0; i < kHandshakeIters; ++i) {
        double ns = 0;
        if (!HandshakeTlsTransport(port, error, &ns, &out->tls_version)) {
            return false;
        }
        out->hs_tt.Add(ns);
    }
    for (int i = 0; i < kHandshakeIters; ++i) {
        double ns = 0;
        std::string version;
        if (!HandshakeRaw(raw_shared, port, error, &ns, &version)) {
            return false;
        }
        out->hs_raw_shared.Add(ns);
        if (out->tls_version.empty()) {
            out->tls_version = version;
        }
    }
    for (int i = 0; i < kHandshakeIters; ++i) {
        SSL_CTX* fresh = RawTlsClient::NewClientContext();
        if (fresh == nullptr) {
            *error = "SSL_CTX_new(client) failed";
            return false;
        }
        double ns = 0;
        std::string version;
        const bool ok = HandshakeRaw(fresh, port, error, &ns, &version);
        SSL_CTX_free(fresh);
        if (!ok) {
            return false;
        }
        out->hs_raw_fresh.Add(ns);
    }
    std::printf("[bench-tls] handshake n=%d tls=%s\n", kHandshakeIters,
                out->tls_version.c_str());
    std::printf(
        "[bench-tls]   tt_us mean=%.1f p50=%.1f p99=%.1f | raw_shared mean=%.1f p50=%.1f "
        "p99=%.1f | raw_fresh mean=%.1f p50=%.1f p99=%.1f\n",
        out->hs_tt.Mean() / 1e3, out->hs_tt.P50() / 1e3, out->hs_tt.P99() / 1e3,
        out->hs_raw_shared.Mean() / 1e3, out->hs_raw_shared.P50() / 1e3,
        out->hs_raw_shared.P99() / 1e3, out->hs_raw_fresh.Mean() / 1e3,
        out->hs_raw_fresh.P50() / 1e3, out->hs_raw_fresh.P99() / 1e3);
    std::fflush(stdout);

    if (!BenchWriteTlsTransport(port, &out->write_tt_mibps, error)) {
        return false;
    }
    if (!BenchWriteRaw(raw_shared, port, &out->write_raw_mibps, error)) {
        return false;
    }
    std::printf("[bench-tls] write 32KBx8192=256MiB tt_mibps=%.1f raw_mibps=%.1f\n",
                out->write_tt_mibps, out->write_raw_mibps);
    std::fflush(stdout);

    if (!BenchSmallTlsTransport(port, &out->small_tt_ns, error)) {
        return false;
    }
    if (!BenchSmallRaw(raw_shared, port, &out->small_raw_ns, error)) {
        return false;
    }
    std::printf("[bench-tls] small 64Bx50000 tt_ns=%.1f raw_ns=%.1f\n", out->small_tt_ns,
                out->small_raw_ns);
    std::fflush(stdout);
    return true;
}

double Median(double a, double b, double c) {
    double v[3] = {a, b, c};
    std::sort(v, v + 3);
    return v[1];
}

std::string AffinityText() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return "unknown";
    }
    std::string out;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &set)) {
            if (!out.empty()) {
                out += ",";
            }
            out += std::to_string(i);
        }
    }
    return out.empty() ? "empty" : out;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // 服务端在对端关闭后仍会尝试写 close_notify，默认 SIGPIPE 会杀掉整个进程。
    std::signal(SIGPIPE, SIG_IGN);
    if (OPENSSL_init_ssl(0, nullptr) != 1) {
        std::fprintf(stderr, "[bench-tls] OPENSSL_init_ssl failed\n");
        return 1;
    }
    std::printf("[bench-tls] openssl=%s cpus=%s\n", OpenSSL_version(OPENSSL_VERSION),
                AffinityText().c_str());

    TlsServer server;
    std::string error;
    if (!server.Start(&error)) {
        std::fprintf(stderr, "[bench-tls] server start failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("[bench-tls] server port=%u\n", static_cast<unsigned>(server.port()));

    SSL_CTX* raw_shared = RawTlsClient::NewClientContext();
    if (raw_shared == nullptr) {
        std::fprintf(stderr, "[bench-tls] SSL_CTX_new(client) failed\n");
        server.Stop();
        return 1;
    }

    go2cpp::scheduler::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    config.collect_metrics = false;
    go2cpp::scheduler::Scheduler scheduler(config);
    scheduler.start();

    RoundResult rounds[kRounds];
    std::atomic<bool> done{false};
    bool failed = false;
    auto task = scheduler.spawn([&] {
        for (int r = 0; r < kRounds && !failed; ++r) {
            if (!RunRound(r, server.port(), raw_shared, &rounds[r], &error)) {
                failed = true;
            }
        }
        done.store(true, std::memory_order_release);
    });
    (void)task->wait_for(std::chrono::seconds(300));
    (void)scheduler.shutdown_for(std::chrono::seconds(10));
    server.Stop();
    SSL_CTX_free(raw_shared);

    if (failed || !done.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "[bench-tls] FAILED: %s\n",
                     error.empty() ? "round timeout" : error.c_str());
        return 1;
    }

    const double hs_tt_mean = Median(rounds[0].hs_tt.Mean(), rounds[1].hs_tt.Mean(),
                                     rounds[2].hs_tt.Mean());
    const double hs_tt_p50 =
        Median(rounds[0].hs_tt.P50(), rounds[1].hs_tt.P50(), rounds[2].hs_tt.P50());
    const double hs_tt_p99 =
        Median(rounds[0].hs_tt.P99(), rounds[1].hs_tt.P99(), rounds[2].hs_tt.P99());
    const double hs_rs_mean = Median(rounds[0].hs_raw_shared.Mean(), rounds[1].hs_raw_shared.Mean(),
                                     rounds[2].hs_raw_shared.Mean());
    const double hs_rs_p50 = Median(rounds[0].hs_raw_shared.P50(), rounds[1].hs_raw_shared.P50(),
                                    rounds[2].hs_raw_shared.P50());
    const double hs_rs_p99 = Median(rounds[0].hs_raw_shared.P99(), rounds[1].hs_raw_shared.P99(),
                                    rounds[2].hs_raw_shared.P99());
    const double hs_rf_mean = Median(rounds[0].hs_raw_fresh.Mean(), rounds[1].hs_raw_fresh.Mean(),
                                     rounds[2].hs_raw_fresh.Mean());
    const double hs_rf_p50 = Median(rounds[0].hs_raw_fresh.P50(), rounds[1].hs_raw_fresh.P50(),
                                    rounds[2].hs_raw_fresh.P50());
    const double hs_rf_p99 = Median(rounds[0].hs_raw_fresh.P99(), rounds[1].hs_raw_fresh.P99(),
                                    rounds[2].hs_raw_fresh.P99());
    const double wr_tt = Median(rounds[0].write_tt_mibps, rounds[1].write_tt_mibps,
                                rounds[2].write_tt_mibps);
    const double wr_raw = Median(rounds[0].write_raw_mibps, rounds[1].write_raw_mibps,
                                 rounds[2].write_raw_mibps);
    const double sm_tt =
        Median(rounds[0].small_tt_ns, rounds[1].small_tt_ns, rounds[2].small_tt_ns);
    const double sm_raw =
        Median(rounds[0].small_raw_ns, rounds[1].small_raw_ns, rounds[2].small_raw_ns);

    std::printf("\n[bench-tls] ===== median of %d rounds =====\n", kRounds);
    std::printf("[bench-tls] handshake(us) tt mean=%.1f p50=%.1f p99=%.1f\n", hs_tt_mean / 1e3,
                hs_tt_p50 / 1e3, hs_tt_p99 / 1e3);
    std::printf("[bench-tls] handshake(us) raw_shared mean=%.1f p50=%.1f p99=%.1f\n",
                hs_rs_mean / 1e3, hs_rs_p50 / 1e3, hs_rs_p99 / 1e3);
    std::printf("[bench-tls] handshake(us) raw_fresh mean=%.1f p50=%.1f p99=%.1f\n",
                hs_rf_mean / 1e3, hs_rf_p50 / 1e3, hs_rf_p99 / 1e3);
    std::printf("[bench-tls] handshake ratio tt/raw_shared mean=%.3f p50=%.3f p99=%.3f\n",
                hs_tt_mean / hs_rs_mean, hs_tt_p50 / hs_rs_p50, hs_tt_p99 / hs_rs_p99);
    std::printf("[bench-tls] handshake ratio tt/raw_fresh mean=%.3f p50=%.3f p99=%.3f\n",
                hs_tt_mean / hs_rf_mean, hs_tt_p50 / hs_rf_p50, hs_tt_p99 / hs_rf_p99);
    std::printf("[bench-tls] write 32KBx8192 tt_mibps=%.1f raw_mibps=%.1f ratio raw/tt=%.3f\n",
                wr_tt, wr_raw, wr_raw / wr_tt);
    std::printf("[bench-tls] small 64Bx50000 tt_ns=%.1f raw_ns=%.1f ratio tt/raw=%.3f\n", sm_tt,
                sm_raw, sm_tt / sm_raw);
    std::printf("[bench-tls] tls_version=%s\n", rounds[0].tls_version.c_str());
    return 0;
}
