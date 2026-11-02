/**
 * @file client.cpp
 * @brief `TunnelClient` 实现：注册、重连退避、每流对接本地 TCP。
 *
 * @details 状态机（由 `RunLoop` 驱动）：
 *
 * ```
 *   [未连接] --dial--> [握手] --ACK ok--> [在线] --断线--> [退避] --> [未连接]
 *                        |                  |
 *                        +--ACK 拒绝--> 停止（不再重试）
 * ```
 *
 * 注册被**明确拒绝**（版本不符 / token 错误 / 子域名非法）时不重试——重试
 * 也只会得到同样结果，会把日志刷爆。只有网络类失败才退避重连。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/tunnel/client.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace go2cpp {
namespace tunnel {
namespace {

/// 生成给浏览器的错误响应（本地服务连不上时隧道里直接回一个 502）。
std::string LocalErrorResponse(int code, const char* reason,
                               const std::string& detail) {
    const std::string body = std::string("tunnel: ") + detail + "\n";
    return "HTTP/1.1 " + std::to_string(code) + " " + reason +
           "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

}  // namespace

TunnelClient::TunnelClient(TunnelClientConfig config)
    : m_config(std::move(config)) {
    if (!m_config.token.empty()) {
        m_psk = DerivePsk(m_config.token);
        m_auth_required = true;
    }
    go2cpp::metrics::Registry& registry = go2cpp::metrics::Registry::Default();
    m_metric_reconnects = &registry.GetCounter(
        "go2cpp_tunnel_client_reconnects_total", "Client reconnect attempts");
    m_metric_streams = &registry.GetCounter(
        "go2cpp_tunnel_client_streams_total", "Local streams relayed by client");
    m_metric_online = &registry.GetGauge("go2cpp_tunnel_client_online",
                                         "Whether this client is online");
}

TunnelClient::~TunnelClient() { Stop(); }

bool TunnelClient::Start(ErrorPtr* err) {
    if (m_running.exchange(true)) {
        if (err != nullptr) {
            *err = NewError("tunnel: client already started");
        }
        return false;
    }
    try {
        m_run_thread = std::thread([this] { RunLoop(); });
    } catch (const std::exception& error) {
        m_running.store(false);
        if (err != nullptr) {
            *err = NewError(std::string("tunnel: start thread: ") + error.what());
        }
        return false;
    }
    return true;
}

void TunnelClient::Stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    m_state_cv.notify_all();
    if (m_run_thread.joinable()) {
        if (m_run_thread.get_id() == std::this_thread::get_id()) {
            m_run_thread.detach();
        } else {
            m_run_thread.join();
        }
    }
    for (std::thread& thread : m_data_threads) {
        if (thread.joinable()) {
            if (thread.get_id() == std::this_thread::get_id()) {
                thread.detach();
            } else {
                thread.join();
            }
        }
    }
    m_data_threads.clear();
    {
        std::unique_lock<std::mutex> lock(m_worker_mutex);
        m_worker_cv.wait_for(lock, std::chrono::milliseconds(10000),
                             [this] { return m_workers == 0; });
    }
    SetState(false, {}, {});
    if (m_metric_online != nullptr) {
        m_metric_online->Set(0);
    }
}

bool TunnelClient::WaitOnline(int timeout_ms) {
    std::unique_lock<std::mutex> lock(m_state_mutex);
    return m_state_cv.wait_for(
        lock, std::chrono::milliseconds(std::max(0, timeout_ms)),
        [this] { return m_online.load(std::memory_order_acquire) || !m_running.load(); });
}

std::string TunnelClient::public_url() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_public_url;
}

std::string TunnelClient::subdomain() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_subdomain;
}

TunnelClientStats TunnelClient::Stats() const {
    TunnelClientStats stats;
    stats.online = m_online.load(std::memory_order_acquire);
    stats.reconnects = m_reconnects.load(std::memory_order_relaxed);
    stats.streams_total = m_streams_total.load(std::memory_order_relaxed);
    stats.bytes_in = m_bytes_in.load(std::memory_order_relaxed);
    stats.bytes_out = m_bytes_out.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        stats.subdomain = m_subdomain;
        stats.public_url = m_public_url;
    }
    return stats;
}

void TunnelClient::set_event_handler(EventHandler handler) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    m_event_handler = std::move(handler);
}

void TunnelClient::Emit(const std::string& message) const {
    EventHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_event_mutex);
        handler = m_event_handler;
    }
    if (handler) {
        handler(message);
    }
}

void TunnelClient::SetState(bool online_now, const std::string& subdomain,
                            const std::string& url) {
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_subdomain = subdomain;
        m_public_url = url;
        m_online.store(online_now, std::memory_order_release);
    }
    m_state_cv.notify_all();
}

bento::Backoff TunnelClient::MakeBackoff() const {
    bento::Backoff::Config config;
    config.base_ms = static_cast<std::uint32_t>(
        std::max(100, m_config.reconnect_min_ms));
    config.max_ms = static_cast<std::uint32_t>(
        std::max(m_config.reconnect_min_ms, m_config.reconnect_max_ms));
    config.jitter_ratio = m_config.reconnect_jitter;
    return bento::Backoff(config);
}

bool TunnelClient::ConnectAndRegister(bool data_role, MuxSession::Ptr* session_out,
                                      HelloAck* ack_out, std::string* err) {
    std::string dial_error;
    TcpConn::Ptr conn = TcpConn::Dial(
        m_config.server_host, m_config.server_port,
        std::max(100, m_config.dial_timeout_ms), &dial_error);
    if (!conn) {
        if (err != nullptr) {
            *err = "dial " + m_config.server_host + ":" +
                   std::to_string(m_config.server_port) + ": " + dial_error;
        }
        return false;
    }

    // 1) 服务端总是先发 16B 挑战：全零 = 不鉴权；否则用 PSK 回 AEAD tag。
    FrameReader reader(conn, m_config.with_checksum);
    frame::Frame frame;
    std::string read_error;
    if (!reader.Next(5000, &frame, &read_error) ||
        static_cast<FrameType>(frame.type) != FrameType::kChallenge) {
        if (err != nullptr) {
            *err = "challenge: " + read_error;
        }
        conn->Shutdown();
        return false;
    }
    std::uint8_t challenge[kChallengeBytes] = {};
    if (!DecodeChallenge(frame.payload.data(), frame.payload.size(), challenge)) {
        if (err != nullptr) {
            *err = "malformed challenge";
        }
        conn->Shutdown();
        return false;
    }
    bool requires_auth = false;
    for (std::size_t index = 0; index < kChallengeBytes; ++index) {
        if (challenge[index] != 0) {
            requires_auth = true;
            break;
        }
    }
    std::uint8_t tag[kAuthTagBytes] = {};
    if (requires_auth) {
        if (!m_auth_required) {
            if (err != nullptr) {
                *err = "server requires --token";
            }
            conn->Shutdown();
            return false;
        }
        if (!ComputeAuthTag(m_psk, challenge, tag)) {
            if (err != nullptr) {
                *err = "compute auth tag failed";
            }
            conn->Shutdown();
            return false;
        }
    }
    if (!WriteFrame(conn, m_config.with_checksum, FrameType::kAuth,
                    EncodeAuth(tag))) {
        if (err != nullptr) {
            *err = "send auth failed";
        }
        conn->Shutdown();
        return false;
    }

    // 2) HELLO（数据连接额外置 kHelloFlagDataConnection）。
    MuxConfig mux_config;
    mux_config.recv_queue_cap = m_config.recv_queue_cap;
    mux_config.idle_timeout_ms = m_config.idle_timeout_ms;
    mux_config.ping_interval_ms = m_config.ping_interval_ms;
    mux_config.with_checksum = m_config.with_checksum;
    MuxSession::Ptr session = MuxSession::Create(conn, mux_config);
    if (!session) {
        conn->Shutdown();
        return false;
    }
    Hello hello;
    hello.version = kProtocolVersion;
    hello.flags = data_role ? kHelloFlagDataConnection : 0U;
    const std::string active_subdomain = subdomain();
    hello.subdomain = active_subdomain.empty() ? m_config.subdomain
                                              : active_subdomain;
    hello.local_port = m_config.local_port;
    hello.token = m_config.token;
    std::vector<std::uint8_t> payload;
    ErrorPtr encode_error;
    if (!EncodeHello(hello, &payload, &encode_error) ||
        !session->SendPayload(FrameType::kHello, payload)) {
        if (err != nullptr) {
            *err = "send HELLO failed: " + ErrorMessage(encode_error);
        }
        session->Stop();
        return false;
    }
    ErrorPtr start_error;
    if (!session->Start(
            [this](MuxStream::Ptr stream) { HandleOpen(stream); },
            &start_error)) {
        if (err != nullptr) {
            *err = "start session failed: " + ErrorMessage(start_error);
        }
        session->Stop();
        return false;
    }
    HelloAck ack;
    if (!session->WaitHelloAck(m_config.hello_timeout_ms, &ack)) {
        if (err != nullptr) {
            *err = "wait HELLO_ACK failed";
        }
        session->Stop();
        return false;
    }
    if (ack_out != nullptr) {
        *ack_out = ack;
    }
    if (ack.status != Status::kOk) {
        if (err != nullptr) {
            *err = std::string("rejected: ") + StatusName(ack.status) + " " +
                   ack.message;
        }
        const bool fatal = ack.status == Status::kUnauthorized ||
                           ack.status == Status::kBadVersion;
        if (fatal) {
            Emit("[tunnel] 服务端拒绝注册（" +
                 std::string(StatusName(ack.status)) + "）: " + ack.message);
            m_running.store(false, std::memory_order_release);
        }
        session->Stop();
        return false;
    }
    *session_out = session;
    return true;
}

void TunnelClient::StartDataConnections() {
    if (m_config.data_conns <= 0) {
        return;
    }
    bool expected = false;
    if (!m_data_started.compare_exchange_strong(expected, true)) {
        return;
    }
    for (int index = 0; index < m_config.data_conns; ++index) {
        try {
            m_data_threads.emplace_back([this] { DataLoop(); });
        } catch (...) {
            Emit("[tunnel] 数据连接线程启动失败");
        }
    }
    Emit("[tunnel] 数据连接数: " + std::to_string(m_config.data_conns));
}

void TunnelClient::DataLoop() {
    bento::Backoff backoff = MakeBackoff();
    while (m_running.load(std::memory_order_acquire)) {
        MuxSession::Ptr session;
        HelloAck ack;
        std::string error;
        if (!ConnectAndRegister(true, &session, &ack, &error)) {
            if (!m_running.load()) {
                return;
            }
            Emit("[tunnel] 数据连接重试: " + error);
            std::unique_lock<std::mutex> lock(m_state_mutex);
            m_state_cv.wait_for(lock,
                                std::chrono::milliseconds(backoff.Next()),
                                [this] { return !m_running.load(); });
            continue;
        }
        backoff.Reset();
        const std::uint64_t generation = m_generation.load();
        while (session->alive() && m_running.load() &&
               m_generation.load() == generation) {
            std::unique_lock<std::mutex> lock(m_state_mutex);
            m_state_cv.wait_for(lock, std::chrono::milliseconds(200),
                                [this] { return !m_running.load(); });
        }
        session->Shutdown("data connection rebind");
        session->Stop();
        // 控制连接换代后给新隧道注册留一点时间，避免立刻打回 unknown tunnel。
        std::unique_lock<std::mutex> lock(m_state_mutex);
        m_state_cv.wait_for(lock, std::chrono::milliseconds(300),
                            [this] { return !m_running.load(); });
    }
}

void TunnelClient::RunLoop() {
    bento::Backoff backoff = MakeBackoff();
    bool ever_online = false;
    while (m_running.load(std::memory_order_acquire)) {
        const bool registered = RunOnce();
        m_generation.fetch_add(1, std::memory_order_relaxed);
        if (!m_running.load(std::memory_order_acquire)) {
            break;
        }
        if (server_draining()) {
            Emit("[tunnel] 服务端 GOAWAY，停止重连");
            break;
        }
        std::uint32_t delay_ms = 0;
        if (registered) {
            ever_online = true;
            backoff.Reset();
            delay_ms = 200;
            if (m_metric_reconnects != nullptr) {
                m_metric_reconnects->Inc();
            }
            Emit("[tunnel] 控制连接断开，准备重连");
        } else {
            delay_ms = backoff.Next();
            if (ever_online) {
                if (m_metric_reconnects != nullptr) {
                    m_metric_reconnects->Inc();
                }
                Emit("[tunnel] 重连失败，退避 " + std::to_string(delay_ms) +
                     " ms");
            } else {
                Emit("[tunnel] 连接失败，退避 " + std::to_string(delay_ms) +
                     " ms");
            }
        }
        std::unique_lock<std::mutex> lock(m_state_mutex);
        m_state_cv.wait_for(lock, std::chrono::milliseconds(delay_ms),
                            [this] { return !m_running.load(); });
    }
    SetState(false, {}, {});
}

bool TunnelClient::RunOnce() {
    MuxSession::Ptr session;
    HelloAck ack;
    std::string error;
    if (!ConnectAndRegister(false, &session, &ack, &error)) {
        Emit("[tunnel] 连接失败: " + error);
        return false;
    }
    SetState(true, ack.subdomain, ack.public_url);
    if (m_metric_online != nullptr) {
        m_metric_online->Set(1);
    }
    Emit("[tunnel] 已上线: " + ack.public_url + "  →  " + m_config.local_host +
         ":" + std::to_string(m_config.local_port));
    StartDataConnections();

    while (session->alive() && m_running.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(m_state_mutex);
        m_state_cv.wait_for(lock, std::chrono::milliseconds(200),
                            [this] { return !m_running.load(); });
        if (session->draining()) {
            m_server_draining.store(true, std::memory_order_release);
        }
    }
    if (session->alive()) {
        session->Shutdown("client stopping");
    }
    m_bytes_in.fetch_add(session->bytes_in(), std::memory_order_relaxed);
    m_bytes_out.fetch_add(session->bytes_out(), std::memory_order_relaxed);
    session->Stop();
    if (m_metric_online != nullptr) {
        m_metric_online->Set(0);
    }
    SetState(false, {}, {});
    return true;
}

void TunnelClient::HandleOpen(const MuxStream::Ptr& stream) {
    if (!stream) {
        return;
    }
    std::size_t active = 0;
    {
        std::lock_guard<std::mutex> lock(m_worker_mutex);
        active = static_cast<std::size_t>(m_workers);
    }
    if (active >= m_config.max_local_streams) {
        const std::string response =
            LocalErrorResponse(503, "Service Unavailable",
                               "too many concurrent streams");
        (void)stream->Write(response.data(), response.size());
        (void)stream->Close();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_worker_mutex);
        ++m_workers;
    }
    m_streams_total.fetch_add(1, std::memory_order_relaxed);
    if (m_metric_streams != nullptr) {
        m_metric_streams->Inc();
    }
    try {
        std::thread([this, stream] {
            RelayLocal(stream);
            {
                std::lock_guard<std::mutex> lock(m_worker_mutex);
                --m_workers;
            }
            m_worker_cv.notify_all();
        }).detach();
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(m_worker_mutex);
            --m_workers;
        }
        m_worker_cv.notify_all();
        (void)stream->Close();
    }
}

void TunnelClient::RelayLocal(const MuxStream::Ptr& stream) {
    std::string error;
    TcpConn::Ptr local = TcpConn::Dial(
        m_config.local_host, m_config.local_port,
        std::max(100, m_config.dial_timeout_ms), &error);
    if (!local) {
        const std::string response = LocalErrorResponse(
            502, "Bad Gateway", "local service unreachable: " + error);
        (void)stream->Write(response.data(), response.size());
        (void)stream->Close();
        return;
    }
    // 注意方向语义：本地服务发完响应就会 FIN（HTTP/1.0 或 Connection: close），
    // 但此时响应字节可能还排在 RelayPipe 的队列里。若在「读到本地 EOF」时立刻
    // 关闭隧道流，CLOSE 帧会先于剩余 DATA 到达服务端，响应体被截断。因此
    // **本地侧 EOF 不传播**：让服务端在公网侧读到 EOF（浏览器收到完整响应后
    // 关闭连接）再关闭隧道流，本端随隧道流 EOF 一起收尾。
    SocketStream local_stream(local, 1000);
    EofClosingStream tunnel_side(stream.get(), &local_stream);
    relay::PipeConfig pipe_config;
    pipe_config.copy_buf = 32U * 1024U;
    pipe_config.queue_cap = 256U * 1024U;
    pipe_config.write_timeout = std::chrono::milliseconds(30000);
    relay::RelayPipe pipe(&local_stream, &tunnel_side, pipe_config);
    ErrorPtr pipe_error;
    (void)pipe.Run(nullptr, &pipe_error);
    (void)stream->Close();
    local->Shutdown();
}

}  // namespace tunnel
}  // namespace go2cpp
