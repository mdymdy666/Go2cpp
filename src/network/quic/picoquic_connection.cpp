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
 *
 * ## 模块定位
 * `picoquic_connection.hpp` 的唯一实现文件，也是唯一 `#include <picoquic.h>` 的
 * 翻译单元；picoquic 类型通过 pimpl（`struct Impl`）完全隔离在 .cpp 内。
 *
 * ## 本文件定义什么
 * - `Impl`：持有 quic/cnx 句柄、握手/关闭标记、按流接收缓冲；
 * - `Impl::StreamCallback`：把 picoquic 回调翻译成「收到数据 / 关闭」状态；
 * - `Connect()` / `Poll()` / `WriteStream()` / `ReadStream()` 的全部实现。
 *
 * ## 阅读建议
 * 先看文件头的三个实现坑，再看 `Connect()` 的建上下文顺序，最后看 `Poll()` 的
 * 「先把所有待发包写完、再读一个 datagram」循环。
 *
 * ## 边界与陷阱
 * - 本文件只在 `GO2CPP_WITH_PICOQUIC=ON` 时参与构建（默认 OFF）；
 * - `picoquic_set_null_verifier()` 是自签名测试能通的**前提**：
 *   `picoquic_create_client_cnx()` 在严格验证下会直接拒绝自签名链；
 * - `picoquic_create_client_cnx()` 内部已经 start，外面**不要**再调
 *   `picoquic_start_client_cnx()`（会返回 -1）；
 * - `Read()` 的 datagram 语义由调用方传入的 `Transport` 保证，本文件只按包喂给
 *   picoquic；占位 `sockaddr` 仅用于满足 API 签名。
 */

#include "go2cpp/network/quic/picoquic_connection.hpp"

#include "go2cpp/network/http2/transport.hpp"

#include <picoquic.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

namespace go2cpp::net::quic {
namespace {

using Clock = std::chrono::steady_clock;

/// `Poll()` 单次调用的默认等待上限（毫秒）：调用方至少每秒能拿回控制权。
constexpr int kDefaultPollWaitMs = 1000;
/// 单次 `Read` 的等待切片上限（毫秒）：保证 picoquic 的 PTO/idle 定时器按时驱动。
constexpr int kMaxWaitSliceMs = 1000;
/// 非阻塞 Transport 连续空转时的退避上限（毫秒），避免 100% CPU（审计 D07）。
constexpr int kIdleBackoffMaxMs = 20;

/// 距 deadline 的剩余毫秒数（已过期返回 0）。
std::int64_t RemainingMs(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return left.count() < 0 ? 0 : left.count();
}

void SleepMs(int ms) {
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

/// 探测系统默认 CA bundle（verify_peer 且未显式配置根证书时使用）。
std::string DefaultCertRootFile() {
    static const char* const kCandidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
        "/usr/local/etc/openssl/cert.pem",
    };
    for (const char* path : kCandidates) {
        if (::access(path, R_OK) == 0) {
            return std::string(path);
        }
    }
    return std::string();
}

/// 取关闭码：对端 application > 本地 application > 对端 transport > 本地 transport。
///
/// 注意 `picoquic_get_application_error()` 只返回**对端**的 application error；
/// 本地 `picoquic_close(cnx, 42)` 的码要用 `picoquic_get_close_reasons()` 取。
std::uint64_t CloseErrorCode(picoquic_cnx_t* cnx) {
    if (cnx == nullptr) {
        return 0;
    }
    std::uint64_t local_transport = 0;
    std::uint64_t remote_transport = 0;
    std::uint64_t local_application = 0;
    std::uint64_t remote_application = 0;
    picoquic_get_close_reasons(cnx, &local_transport, &remote_transport, &local_application,
                               &remote_application);
    if (remote_application != 0) {
        return remote_application;
    }
    if (local_application != 0) {
        return local_application;
    }
    if (remote_transport != 0) {
        return remote_transport;
    }
    return local_transport;
}

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
    /// 是否已经发起过本地 close（保证 `picoquic_close` 只调一次）。
    bool close_requested{false};
    std::uint64_t last_error{0};
    /// 非阻塞 Transport 空转时的退避步长（毫秒）。
    int idle_backoff_ms{1};

    /// 按流号收集收到的字节。薄封装只做分拣，不做 HTTP/3 解析。
    std::map<std::uint64_t, std::vector<std::uint8_t>> received;
    /// 每个流的读偏移（避免每次 ReadStream 都 erase 前面的数据）。
    std::map<std::uint64_t, std::size_t> read_offset;
    /// 每个流是否已收到 FIN（区分「暂时没有数据」与「流已结束」）。
    std::map<std::uint64_t, bool> finished;
    /// 每个流最近一次 stream_reset 的对端错误码。
    std::map<std::uint64_t, std::uint64_t> stream_error;

    std::string poll_error;
    /// 收发共用 datagram 缓冲：65535 覆盖 UDP 最大报文，避免 2048 截断（D13）。
    std::vector<std::uint8_t> packet = std::vector<std::uint8_t>(65535);

    /// 把 picoquic 当前所有待发包写到 transport（不读、不阻塞）。
    bool PrepareAndSend(std::string* error);

    static int StreamCallback(picoquic_cnx_t* cnx, std::uint64_t stream_id,
                              std::uint8_t* bytes, std::size_t length,
                              picoquic_call_back_event_t event, void* callback_ctx,
                              void* stream_ctx);
};

int PicoquicConnection::Impl::StreamCallback(picoquic_cnx_t* cnx, std::uint64_t stream_id,
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
            if (bytes != nullptr && length > 0) {
                auto& buf = self->received[stream_id];
                buf.insert(buf.end(), bytes, bytes + length);
            }
            break;
        case picoquic_callback_stream_fin:
            if (bytes != nullptr && length > 0) {
                auto& buf = self->received[stream_id];
                buf.insert(buf.end(), bytes, bytes + length);
            }
            self->finished[stream_id] = true;
            break;
        case picoquic_callback_stream_reset:
            self->stream_error[stream_id] = picoquic_get_remote_stream_error(cnx, stream_id);
            break;
        case picoquic_callback_stop_sending:
            // 对端要求我们停止发送：记录该流不可再写（错误码无意义，记 0）。
            self->stream_error.emplace(stream_id, 0);
            break;
        // D05/D09：picoquic 的关闭类事件 bytes/len 恒为 NULL/0，关闭码必须从
        // picoquic_get_*_error() 取，不能解 bytes。
        case picoquic_callback_application_close:
            self->closed = true;
            self->last_error = CloseErrorCode(cnx);
            if (self->poll_error.empty()) {
                self->poll_error = "connection closed by application";
            }
            break;
        case picoquic_callback_stateless_reset:
            self->closed = true;
            self->last_error = CloseErrorCode(cnx);
            if (self->poll_error.empty()) {
                self->poll_error = "connection reset (stateless reset)";
            }
            break;
        case picoquic_callback_close:
            self->closed = true;
            self->last_error = CloseErrorCode(cnx);
            break;
        default:
            break;
    }
    return 0;
}

PicoquicConnection::PicoquicConnection(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

PicoquicConnection::~PicoquicConnection() {
    if (m_impl != nullptr && m_impl->quic != nullptr) {
        // D01：析构必须把 CONNECTION_CLOSE 发出去，否则服务端连接槽一直占着
        // （picoquicdemo 默认 -x 256，第 257 个连接会被拒）。best-effort：
        // 这里不报告错误，显式关闭请调 Close()。
        (void)Close(/*application_error_code=*/0, /*error=*/nullptr);
        // picoquic_free 会连带释放 cnx，不需要单独 delete cnx。
        picoquic_free(m_impl->quic);
        m_impl->quic = nullptr;
        m_impl->cnx = nullptr;
    }
}

/**
 * @brief 建立客户端连接（只发 ClientHello，握手在后续 Poll 中推进）。
 *
 * ## 步骤
 * step1. 校验 transport 与 alpn；
 * step2. `picoquic_create()` 建上下文（不传根证书，配合 null verifier）；
 * step3. `verify_peer == false` 时调 `picoquic_set_null_verifier()`；
 * step4. 设置 idle 超时，用占位地址 `picoquic_create_client_cnx()`（内部已 start）；
 * step5. 失败时释放已建资源，成功则移交 `unique_ptr`。
 *
 * ## 失败语义
 * 任一步失败返回 nullptr 并填 `error`；不泄漏 quic 上下文。
 */
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

    // D04：verify_peer 必须真的有信任根；未显式给 CA 时探测系统信任库，
    // 探测不到就 fail-closed（否则就是「假开关」）。
    std::string cert_root;
    if (config.verify_peer) {
        cert_root = config.cert_root_file_name;
        if (cert_root.empty()) {
            cert_root = DefaultCertRootFile();
        }
        if (cert_root.empty()) {
            if (error != nullptr) {
                *error =
                    "verify_peer is enabled but no CA bundle was found; "
                    "set PicoquicConfig::cert_root_file_name";
            }
            return nullptr;
        }
        if (::access(cert_root.c_str(), R_OK) != 0) {
            if (error != nullptr) {
                *error = "cannot read CA bundle '" + cert_root + "'";
            }
            return nullptr;
        }
    }

    // step2: 建 picoquic 上下文并登记回调上下文。
    auto impl = std::make_unique<Impl>();
    impl->transport = transport;

    // 1) 建 quic 上下文：verify_peer 时把 CA bundle 交给 picoquic 安装
    //    OpenSSL 校验器；否则传 nullptr + null verifier（本地自签场景）。
    impl->quic = picoquic_create(
        /*max_nb_connections=*/1, /*cert_file_name=*/nullptr, /*key_file_name=*/nullptr,
        /*cert_root_file_name=*/config.verify_peer ? cert_root.c_str() : nullptr,
        config.alpn.c_str(), &Impl::StreamCallback,
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

    // step3: D04。verify_peer=true 时开启严格策略（无信任根则拒绝建连，而不是
    // fail-open）；false 时安装 null verifier 让自签名本地测试可通。
    if (config.verify_peer) {
        picoquic_set_client_cert_verification_policy(impl->quic, 1);
    } else {
        picoquic_set_null_verifier(impl->quic);
    }

    if (config.idle_timeout_ms != 0) {
        picoquic_set_default_idle_timeout(impl->quic, config.idle_timeout_ms);
    }

    // 2) 建客户端连接。**注意**：它内部已经调用了 picoquic_start_client_cnx，
    //    所以这里不要再调（见文件头细节 1）。
    struct sockaddr_storage addr;
    FillPlaceholderAddress(&addr);
    // step4: 注意内部已 start，不要重复调用。
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

/**
 * @brief 推进一次协议状态机（单次最多等待 `kDefaultPollWaitMs` 毫秒）。
 *
 * 旧实现的 `Poll` 会阻塞在 `Transport::Read()` 上：既会击穿 `Send`/`Receive`
 * 的 deadline（审计 D02：5s 超时实测 8s），也会让 picoquic 的 PTO/idle 定时器
 * 没有机会被驱动（D06）。现在改为：
 * 1. 先把所有待发包写完；
 * 2. 用 `picoquic_get_next_wake_time()` 截短等待上限，再调
 *    `Transport::ReadWithTimeout()` 读一个 datagram；
 * 3. 空转时退避，避免非阻塞 Transport 下 100% CPU（D07）。
 */
bool PicoquicConnection::Poll(std::string* error) {
    return PollFor(error, kDefaultPollWaitMs);
}

bool PicoquicConnection::PollFor(std::string* error, int timeout_ms) {
    Impl& s = *m_impl;
    if (s.closed) {
        if (error != nullptr) {
            *error = s.poll_error.empty() ? "connection closed" : s.poll_error;
        }
        return false;
    }

    const bool bounded = (timeout_ms >= 0);
    const auto deadline = Clock::now() + std::chrono::milliseconds(bounded ? timeout_ms : 0);
    bool first_wait = true;
    int zero_wait_spins = 0;

    for (;;) {
        if (!s.PrepareAndSend(error)) {
            return false;
        }
        if (s.closed) {
            if (error != nullptr) {
                *error = s.poll_error.empty() ? "connection closed" : s.poll_error;
            }
            return false;
        }

        std::int64_t wait_ms = kMaxWaitSliceMs;
        if (bounded) {
            const std::int64_t remaining = RemainingMs(deadline);
            if (remaining <= 0 && !first_wait) {
                return true;  // 等待预算用完：连接仍存活，deadline 由上层判断
            }
            wait_ms = std::min<std::int64_t>(wait_ms, remaining < 0 ? 0 : remaining);
        }

        // 用 picoquic 的下一个定时器（PTO / idle / ACK delay）截短等待。
        const std::uint64_t now_us = picoquic_get_quic_time(s.quic);
        const std::uint64_t wake_us = picoquic_get_next_wake_time(s.quic, now_us);
        if (wake_us != UINT64_MAX) {
            if (wake_us <= now_us) {
                wait_ms = 0;
            } else {
                const std::int64_t delta_ms =
                    static_cast<std::int64_t>((wake_us - now_us + 999) / 1000);
                wait_ms = std::min<std::int64_t>(wait_ms, delta_ms);
            }
        }
        // 防御：定时器一直「已到期」但 prepare 没有推进时，避免零等待自旋。
        if (wait_ms == 0 && ++zero_wait_spins > 8) {
            wait_ms = 1;
            zero_wait_spins = 0;
        } else if (wait_ms > 0) {
            zero_wait_spins = 0;
        }

        const auto before = Clock::now();
        const std::size_t got = s.transport->ReadWithTimeout(
            s.packet.data(), s.packet.size(), static_cast<int>(wait_ms));
        first_wait = false;

        if (got == 0) {
            if (s.transport->Closed()) {
                s.poll_error = "transport closed by peer";
                s.closed = true;
                if (error != nullptr) {
                    *error = s.poll_error;
                }
                return false;
            }
            if (!bounded) {
                // 显式要求「等到有数据或关闭」：退避后继续，可由 Close() 取消。
                SleepMs(s.idle_backoff_ms);
                s.idle_backoff_ms = std::min(s.idle_backoff_ms * 2, kIdleBackoffMaxMs);
                continue;
            }
            if (RemainingMs(deadline) <= 0) {
                return true;
            }
            // Transport 提前返回 0（非阻塞实现）：退避，避免忙转（D07）。
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   Clock::now() - before)
                                   .count();
            if (spent + 1 < wait_ms) {
                SleepMs(s.idle_backoff_ms);
                s.idle_backoff_ms = std::min(s.idle_backoff_ms * 2, kIdleBackoffMaxMs);
            }
            continue;
        }

        s.idle_backoff_ms = 1;
        struct sockaddr_storage peer;
        FillPlaceholderAddress(&peer);
        if (picoquic_incoming_packet(s.quic, s.packet.data(), got,
                                     reinterpret_cast<struct sockaddr*>(&peer),
                                     reinterpret_cast<struct sockaddr*>(&peer),
                                     /*if_index_to=*/0, /*received_ecn=*/0,
                                     picoquic_current_time()) != 0) {
            s.poll_error = "picoquic_incoming_packet failed";
            s.closed = true;  // D12：出错后不再继续使用该连接
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
        return true;  // 一次调用最多处理一个 datagram
    }
}

bool PicoquicConnection::Impl::PrepareAndSend(std::string* error) {
    for (;;) {
        std::size_t send_length = 0;
        struct sockaddr_storage addr_to;
        struct sockaddr_storage addr_from;
        int if_index = 0;
        picoquic_connection_id_t log_cid;
        picoquic_cnx_t* last_cnx = nullptr;

        const int rc = picoquic_prepare_next_packet(
            quic, picoquic_current_time(), packet.data(), packet.size(), &send_length, &addr_to,
            &addr_from, &if_index, &log_cid, &last_cnx);
        if (rc < 0) {
            // D11：负错误是真实故障（缓冲不足/内存错误），不能当成「没包可发」。
            closed = true;
            poll_error = "picoquic_prepare_next_packet failed (" + std::to_string(rc) + ")";
            if (error != nullptr) {
                *error = poll_error;
            }
            return false;
        }
        if (send_length == 0) {
            return true;
        }
        if (transport->Write(packet.data(), send_length) != send_length) {
            closed = true;
            poll_error = "transport write failed";
            if (error != nullptr) {
                *error = poll_error;
            }
            return false;
        }
    }
}

bool PicoquicConnection::Close(std::uint64_t application_error_code, std::string* error) {
    if (m_impl == nullptr) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    Impl& s = *m_impl;
    if (s.quic == nullptr) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    bool ok = true;
    if (!s.close_requested) {
        s.close_requested = true;
        if (s.cnx != nullptr) {
            const int rc = picoquic_close(s.cnx, application_error_code);
            // rc != 0 表示连接已处于 closing/draining 状态，不需要再发关闭帧。
            if (rc == 0 && application_error_code != 0 && s.last_error == 0) {
                s.last_error = application_error_code;
            }
            std::string flush_error;
            if (!s.PrepareAndSend(&flush_error)) {
                ok = false;
                if (error != nullptr) {
                    *error = flush_error;
                }
            }
        }
        s.closed = true;
    }
    if (ok && error != nullptr) {
        error->clear();
    }
    return ok;
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
    // D03：picoquic_add_to_stream 内部会 memcpy(data, length)，空指针直接 SEGV。
    if (length > 0 && data == nullptr) {
        if (error != nullptr) {
            *error = "data is null";
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
    // D03：buffer=nullptr + capacity>0 时 memcpy 会直接 SEGV，按「没有数据」返回 0。
    if (buffer == nullptr || capacity == 0) {
        return 0;
    }
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

bool PicoquicConnection::StreamFinished(std::uint64_t stream_id) const noexcept {
    if (m_impl == nullptr) {
        return false;
    }
    auto it = m_impl->finished.find(stream_id);
    return it != m_impl->finished.end() && it->second;
}

std::uint64_t PicoquicConnection::StreamErrorCode(std::uint64_t stream_id) const noexcept {
    if (m_impl == nullptr) {
        return 0;
    }
    auto it = m_impl->stream_error.find(stream_id);
    return it == m_impl->stream_error.end() ? 0 : it->second;
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
