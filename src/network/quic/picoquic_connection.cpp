/**
 * @file picoquic_connection.cpp
 * @brief `PicoquicConnection` 的实现 —— 唯一 include picoquic 头的地方。
 *
 * ## 三个必须注意的实现细节
 *
 * ### 1. `picoquic_create_client_cnx` 内部**已经调用了** `picoquic_start_client_cnx`
 *
 * 见 picoquic `quicctx.c:4108`。所以**不要**在外面再调一次 —— 再调会得到 -1
 * （错误信息是 "picoquic_start_client_cnx called twice."）。我第一版就是这么错的。
 *
 * ### 2. 必须调 `picoquic_set_null_verifier`
 *
 * 如果不传根证书，`picoquic_start_client_cnx` 会在 `is_cert_verification_strict`
 * 为真时直接拒绝连接。如果传了自签名证书当 root，OpenSSL 的严格验证仍然会
 * 在握手中失败（自签名证书链不完备）。
 *
 * **实测**：传 `certs/cert.pem` 当 root 时握手必失败（连上后立刻
 * `connection closed`）；改成 `picoquic_set_null_verifier` 后立刻成功。
 *
 * ### 3. `sockaddr` 是 picoquic 的 API 要求，不是真实网络地址
 *
 * `picoquic_prepare_next_packet` / `picoquic_incoming_packet` 的签名要求
 * `sockaddr_storage`。但我们走 `Transport` 抽象，真实地址由 Transport 负责。
 * 所以这里用一个**占位地址**（127.0.0.1:0）满足签名即可 —— 因为一条
 * `PicoquicConnection` 只对应一个 `picoquic_cnx_t`，地址不参与多连接区分。
 */

#include "go2cpp/network/quic/picoquic_connection.hpp"

#include "go2cpp/network/http2/transport.hpp"

#include <picoquic.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

namespace go2cpp::net::quic {
namespace {

/// 占位地址：满足 picoquic API 的 `sockaddr` 要求，不参与真实路由。
void FillPlaceholderAddress(struct sockaddr_storage* addr) {
    std::memset(addr, 0, sizeof(*addr));
    auto* in4 = reinterpret_cast<struct sockaddr_in*>(addr);
    in4->sin_family = AF_INET;
    in4->sin_port = htons(0);
    inet_pton(AF_INET, "127.0.0.1", &in4->sin_addr);
}

}  // namespace

struct PicoquicConnection::Impl {
    net::http2::Transport* transport{nullptr};
    picoquic_quic_t* quic{nullptr};
    picoquic_cnx_t* cnx{nullptr};

    bool handshake_done{false};
    bool closed{false};
    std::uint64_t last_error{0};

    /// 按流号收集收到的字节。薄封装只做分拣，不做 HTTP/3 解析。
    std::map<std::uint64_t, std::vector<std::uint8_t>> received;
    /// 每个流的读偏移（避免每次 ReadStream 都 erase 前面的数据）。
    std::map<std::uint64_t, std::size_t> read_offset;

    std::string poll_error;

    static int StreamCallback(picoquic_cnx_t* cnx, std::uint64_t stream_id,
                              std::uint8_t* bytes, std::size_t length,
                              picoquic_call_back_event_t event, void* callback_ctx,
                              void* stream_ctx);
};

int PicoquicConnection::Impl::StreamCallback(picoquic_cnx_t* /*cnx*/, std::uint64_t stream_id,
                                             std::uint8_t* bytes, std::size_t length,
                                             picoquic_call_back_event_t event,
                                             void* callback_ctx, void* /*stream_ctx*/) {
    auto* self = static_cast<Impl*>(callback_ctx);
    if (self == nullptr) {
        return 0;
    }
    switch (event) {
        case picoquic_callback_ready:
            self->handshake_done = true;
            break;
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin:
            if (bytes != nullptr && length > 0) {
                auto& buf = self->received[stream_id];
                buf.insert(buf.end(), bytes, bytes + length);
            }
            break;
        case picoquic_callback_close: {
            self->closed = true;
            // close 事件里 bytes 承载错误码（大端），长度可能为 0。
            if (bytes != nullptr && length > 0) {
                std::uint64_t code = 0;
                const std::size_t n = std::min<std::size_t>(length, 8);
                for (std::size_t i = 0; i < n; ++i) {
                    code = (code << 8) | bytes[i];
                }
                self->last_error = code;
            }
            break;
        }
        default:
            break;
    }
    return 0;
}

PicoquicConnection::PicoquicConnection(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

PicoquicConnection::~PicoquicConnection() {
    if (m_impl != nullptr && m_impl->quic != nullptr) {
        // picoquic_free 会连带释放 cnx，不需要单独 delete cnx。
        picoquic_free(m_impl->quic);
        m_impl->quic = nullptr;
        m_impl->cnx = nullptr;
    }
}

std::unique_ptr<PicoquicConnection> PicoquicConnection::Connect(
    net::http2::Transport* transport, const PicoquicConfig& config, std::string* error) {
    if (transport == nullptr) {
        if (error != nullptr) {
            *error = "transport is null";
        }
        return nullptr;
    }
    if (config.alpn.empty()) {
        if (error != nullptr) {
            *error = "alpn must not be empty";
        }
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->transport = transport;

    // 1) 建 quic 上下文。注意第 4 个参数（cert_root_file_name）传 nullptr，
    //    配合下面的 set_null_verifier；见文件头「必须调 set_null_verifier」。
    impl->quic = picoquic_create(
        /*max_nb_connections=*/1, /*cert_file_name=*/nullptr, /*key_file_name=*/nullptr,
        /*cert_root_file_name=*/nullptr, config.alpn.c_str(), &Impl::StreamCallback,
        impl.get(), /*cnx_id_callback=*/nullptr, /*cnx_id_callback_data=*/nullptr,
        /*reset_seed=*/nullptr, picoquic_current_time(), /*p_simulated_time=*/nullptr,
        /*ticket_file_name=*/nullptr, /*ticket_encryption_key=*/nullptr,
        /*ticket_encryption_key_length=*/0);
    if (impl->quic == nullptr) {
        if (error != nullptr) {
            *error = "picoquic_create failed";
        }
        return nullptr;
    }

    if (!config.verify_peer) {
        // 见文件头细节 2：不这样做的话自签名证书握手必失败。
        picoquic_set_null_verifier(impl->quic);
    }

    if (config.idle_timeout_ms != 0) {
        picoquic_set_default_idle_timeout(impl->quic, config.idle_timeout_ms);
    }

    // 2) 建客户端连接。**注意**：它内部已经调用了 picoquic_start_client_cnx，
    //    所以这里不要再调（见文件头细节 1）。
    struct sockaddr_storage addr;
    FillPlaceholderAddress(&addr);
    impl->cnx = picoquic_create_client_cnx(
        impl->quic, reinterpret_cast<struct sockaddr*>(&addr), picoquic_current_time(),
        /*preferred_version=*/0, config.sni.c_str(), config.alpn.c_str(),
        &Impl::StreamCallback, impl.get());
    if (impl->cnx == nullptr) {
        if (error != nullptr) {
            *error = "picoquic_create_client_cnx failed";
        }
        picoquic_free(impl->quic);
        return nullptr;
    }

    return std::unique_ptr<PicoquicConnection>(new PicoquicConnection(std::move(impl)));
}

bool PicoquicConnection::Poll(std::string* error) {
    Impl& s = *m_impl;
    if (s.closed) {
        if (error != nullptr) {
            *error = s.poll_error.empty() ? "connection closed" : s.poll_error;
        }
        return false;
    }

    struct sockaddr_storage peer;
    FillPlaceholderAddress(&peer);

    std::uint8_t packet[2048];

    // 1) 把 picoquic 所有待发的包写出去。
    for (;;) {
        std::size_t send_length = 0;
        struct sockaddr_storage addr_to;
        struct sockaddr_storage addr_from;
        int if_index = 0;
        picoquic_connection_id_t log_cid;
        picoquic_cnx_t* last_cnx = nullptr;

        const int rc = picoquic_prepare_next_packet(
            s.quic, picoquic_current_time(), packet, sizeof(packet), &send_length, &addr_to,
            &addr_from, &if_index, &log_cid, &last_cnx);
        if (rc != 0 || send_length == 0) {
            break;
        }
        const std::size_t written = s.transport->Write(packet, send_length);
        if (written != send_length) {
            s.poll_error = "transport write failed";
            s.closed = true;
            if (error != nullptr) {
                *error = s.poll_error;
            }
            return false;
        }
    }

    // 2) 读一个 datagram 喂给 picoquic。
    const std::size_t got = s.transport->Read(packet, sizeof(packet));
    if (got == 0) {
        if (s.transport->Closed()) {
            s.poll_error = "transport closed by peer";
            s.closed = true;
            if (error != nullptr) {
                *error = s.poll_error;
            }
            return false;
        }
        // 没数据但没关闭：上层可以重试。
        return true;
    }
    if (picoquic_incoming_packet(s.quic, packet, got,
                                 reinterpret_cast<struct sockaddr*>(&peer),
                                 reinterpret_cast<struct sockaddr*>(&peer),
                                 /*if_index_to=*/0, /*received_ecn=*/0,
                                 picoquic_current_time()) != 0) {
        s.poll_error = "picoquic_incoming_packet failed";
        if (error != nullptr) {
            *error = s.poll_error;
        }
        return false;
    }

    if (s.closed) {
        if (error != nullptr) {
            *error = s.poll_error.empty() ? "connection closed" : s.poll_error;
        }
        return false;
    }
    return true;
}

bool PicoquicConnection::HandshakeComplete() const noexcept {
    return m_impl != nullptr && m_impl->handshake_done;
}

bool PicoquicConnection::Closed() const noexcept {
    return m_impl == nullptr || m_impl->closed;
}

std::size_t PicoquicConnection::WriteStream(std::uint64_t stream_id,
                                            const std::uint8_t* data, std::size_t length,
                                            bool fin, std::string* error) {
    Impl& s = *m_impl;
    if (s.cnx == nullptr || s.closed) {
        if (error != nullptr) {
            *error = "connection not usable";
        }
        return 0;
    }
    const int rc = picoquic_add_to_stream(s.cnx, stream_id, data, length, fin ? 1 : 0);
    if (rc != 0) {
        if (error != nullptr) {
            *error = "picoquic_add_to_stream failed";
        }
        return 0;
    }
    return length;
}

std::size_t PicoquicConnection::ReadStream(std::uint64_t stream_id, std::uint8_t* buffer,
                                           std::size_t capacity) {
    Impl& s = *m_impl;
    auto it = s.received.find(stream_id);
    if (it == s.received.end()) {
        return 0;
    }
    std::size_t& offset = s.read_offset[stream_id];
    const std::vector<std::uint8_t>& buf = it->second;
    if (offset >= buf.size()) {
        return 0;
    }
    const std::size_t available = buf.size() - offset;
    const std::size_t n = std::min(available, capacity);
    std::memcpy(buffer, buf.data() + offset, n);
    offset += n;
    return n;
}

void PicoquicConnection::ReceivedStreamIds(std::uint64_t* out, std::size_t capacity,
                                           std::size_t* count) const {
    if (count != nullptr) {
        *count = 0;
    }
    if (out == nullptr || count == nullptr || m_impl == nullptr) {
        return;
    }
    std::size_t n = 0;
    for (const auto& kv : m_impl->received) {
        if (n >= capacity) {
            break;
        }
        out[n++] = kv.first;
    }
    *count = n;
}

std::uint64_t PicoquicConnection::LastErrorCode() const noexcept {
    return m_impl == nullptr ? 0 : m_impl->last_error;
}

void* PicoquicConnection::NativeHandle() const noexcept {
    return m_impl == nullptr ? nullptr : static_cast<void*>(m_impl->cnx);
}

}  // namespace go2cpp::net::quic
