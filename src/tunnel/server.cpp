/**
 * @file server.cpp
 * @brief `TunnelServer` 实现：控制端口握手 + 公网 HTTP 路由 + RelayPipe 中继。
 *
 * @details 公网连接的完整路径：
 *
 * ```
 *   浏览器 TCP ──▶ ReadHttpHead（只读第一个请求头）
 *                 ├─ 命中隧道? ──否──▶ 目录页 / 404（直接写响应并关闭）
 *                 └─ 是 ──▶ MuxSession::OpenStream() ──▶ OPEN 帧 → 客户端
 *                            ├─ 请求行/头重写（路径剥前缀、X-Forwarded-*）
 *                            └─ PrefixedStream(重写头 + 已预读 body)
 *                               RelayPipe(公网 socket ⇄ MuxStream)
 * ```
 *
 * `RelayPipe` 复用 `include/go2cpp/relay/relay_pipe.hpp`：双向中继 + 有界队列
 * 背压 + 写 deadline。这里只负责把「两端 Stream」配对并起停。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/tunnel/server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <cstring>
#include <thread>
#include <utility>

namespace go2cpp {
namespace tunnel {
namespace {

/// 请求头上限；超过直接 400，避免恶意客户端撑爆内存。
constexpr std::size_t kMaxHttpHead = 64U * 1024U;
/// 请求头读取超时（毫秒）。
constexpr int kHeadTimeoutMs = 10000;
/// 单次 `recv` 缓冲。
constexpr std::size_t kReadBuffer = 16U * 1024U;

/// 小写化（ASCII）。
std::string ToLower(std::string text) {
    for (char& character : text) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return text;
}

/// 去掉首尾空白。
std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// 解析后的 HTTP 请求头。
struct HttpHead {
    std::string method;
    std::string target;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;
};

/// 取 header 值（大小写不敏感）；不存在返回空。
std::string GetHeader(const HttpHead& head, const std::string& name) {
    const std::string wanted = ToLower(name);
    for (const auto& header : head.headers) {
        if (ToLower(header.first) == wanted) {
            return header.second;
        }
    }
    return {};
}

/// 是否存在某个 header。
bool HasHeader(const HttpHead& head, const std::string& name) {
    return !GetHeader(head, name).empty();
}

/// 解析请求头；畸形返回 false。
bool ParseHttpHead(const std::string& raw, HttpHead* out) {
    HttpHead head;
    std::size_t offset = 0;
    bool first = true;
    while (offset < raw.size()) {
        const std::size_t line_end = raw.find("\r\n", offset);
        if (line_end == std::string::npos) {
            break;
        }
        const std::string line = raw.substr(offset, line_end - offset);
        offset = line_end + 2;
        if (line.empty()) {
            break;
        }
        if (first) {
            const std::size_t first_space = line.find(' ');
            const std::size_t second_space =
                first_space == std::string::npos
                    ? std::string::npos
                    : line.find(' ', first_space + 1);
            if (first_space == std::string::npos ||
                second_space == std::string::npos) {
                return false;
            }
            head.method = line.substr(0, first_space);
            head.target =
                line.substr(first_space + 1, second_space - first_space - 1);
            head.version = line.substr(second_space + 1);
            if (head.method.empty() || head.target.empty()) {
                return false;
            }
            first = false;
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            continue;  // 容错：忽略畸形行（例如 obs-fold 的续行）
        }
        head.headers.emplace_back(line.substr(0, colon),
                                  Trim(line.substr(colon + 1)));
    }
    if (first) {
        return false;
    }
    *out = std::move(head);
    return true;
}

/// Host 去端口并小写。
std::string NormalizeHost(const std::string& host) {
    std::string value = ToLower(Trim(host));
    if (value.empty()) {
        return value;
    }
    if (value.front() == '[') {  // [::1]:8443
        const std::size_t close = value.find(']');
        if (close != std::string::npos) {
            return value.substr(0, close + 1);
        }
        return value;
    }
    const std::size_t colon = value.rfind(':');
    if (colon != std::string::npos &&
        value.find(':') == colon) {  // 只有一个冒号 → host:port
        const bool digits =
            std::all_of(value.begin() + static_cast<std::ptrdiff_t>(colon) + 1,
                        value.end(),
                        [](char character) { return character >= '0' && character <= '9'; });
        if (digits) {
            value.resize(colon);
        }
    }
    return value;
}

/// 从 target 里抽出 path+query（去掉 absolute-form 的 scheme/host）。
std::string ExtractPath(const std::string& target) {
    std::string path = target;
    const std::size_t scheme = path.find("://");
    if (scheme != std::string::npos) {
        const std::size_t slash = path.find('/', scheme + 3);
        path = slash == std::string::npos ? "/" : path.substr(slash);
    }
    if (path.empty() || path.front() != '/') {
        path = "/" + path;
    }
    const std::size_t fragment = path.find('#');
    if (fragment != std::string::npos) {
        path.resize(fragment);
    }
    return path;
}

/// 构造一段 HTTP 文本响应。
std::string BuildResponse(int code, const char* reason,
                          const std::string& content_type,
                          const std::string& body) {
    std::string response = "HTTP/1.1 " + std::to_string(code) + " " + reason +
                           "\r\nContent-Type: " + content_type +
                           "\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n";
    response += body;
    return response;
}

/// HTML 转义（目录页里的子域名/地址都来自对端，必须转义）。
std::string HtmlEscape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char character : text) {
        switch (character) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&#39;";
                break;
            default:
                escaped += character;
                break;
        }
    }
    return escaped;
}

/// 毫秒时间戳（steady）。
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// 先把请求头读出来；`*extra` 是多读的 body 字节（不能丢）。
bool ReadHttpHead(TcpConn::Ptr conn, std::string* head, std::string* extra,
                  std::string* err) {
    std::string buffer;
    std::vector<char> chunk(kReadBuffer);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeadTimeoutMs);
    while (buffer.find("\r\n\r\n") == std::string::npos) {
        if (buffer.size() > kMaxHttpHead) {
            if (err != nullptr) {
                *err = "request head too large";
            }
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            if (err != nullptr) {
                *err = "request head timeout";
            }
            return false;
        }
        const IoResult result = conn->Read(chunk.data(), chunk.size(), 2000);
        if (result.ok()) {
            buffer.append(chunk.data(), result.count);
            continue;
        }
        if (result.eof) {
            if (err != nullptr) {
                *err = "connection closed before request head";
            }
            return false;
        }
        if (result.error == ETIMEDOUT) {
            continue;
        }
        if (err != nullptr) {
            *err = "read fail: " + std::to_string(result.error);
        }
        return false;
    }
    const std::size_t end = buffer.find("\r\n\r\n") + 4;
    *head = buffer.substr(0, end);
    *extra = buffer.substr(end);
    return true;
}

/// 生成重写后的请求头字节。
std::string RewriteRequestHead(const HttpHead& head, const std::string& path,
                               const std::string& peer_ip,
                               const std::string& prefix, bool force_close) {
    std::string out;
    out.reserve(head.target.size() + 512);
    out += head.method;
    out += ' ';
    out += path;
    out += ' ';
    out += head.version;
    out += "\r\n";
    const std::string host = GetHeader(head, "host");
    bool forwarded_for = false;
    for (const auto& header : head.headers) {
        const std::string lower = ToLower(header.first);
        if (lower == "connection" && force_close) {
            continue;  // 统一在末尾改写成 close
        }
        out += header.first;
        out += ": ";
        out += header.second;
        if (lower == "x-forwarded-for") {
            forwarded_for = true;
            if (!header.second.empty()) {
                out += ", ";
            }
            out += peer_ip;
        }
        out += "\r\n";
    }
    if (!forwarded_for) {
        out += "X-Forwarded-For: " + peer_ip + "\r\n";
    }
    if (!HasHeader(head, "x-forwarded-proto")) {
        out += "X-Forwarded-Proto: http\r\n";
    }
    if (!host.empty() && !HasHeader(head, "x-forwarded-host")) {
        out += "X-Forwarded-Host: " + host + "\r\n";
    }
    if (!prefix.empty()) {
        out += "X-Forwarded-Prefix: " + prefix + "\r\n";
    }
    if (force_close) {
        out += "Connection: close\r\n";
    }
    out += "\r\n";
    return out;
}

/// 向公网连接写一段响应并关闭。
void SendResponse(TcpConn::Ptr conn, int code, const char* reason,
                  const std::string& content_type, const std::string& body) {
    const std::string response = BuildResponse(code, reason, content_type, body);
    (void)conn->WriteAll(response.data(), response.size(), 10000, nullptr);
    conn->Shutdown();
}

/// 在「隧道 → 浏览器」的写路径上解析响应头，按 `Content-Length` 判定响应
/// 结束；结束后主动 shutdown 公网连接并关闭隧道流。
///
/// @details 为什么需要它：HTTP/1.0 客户端（以及只按 EOF 判断响应结束的
///          客户端）会一直等 FIN。若只依赖浏览器主动关连接，隧道流会挂到
///          心跳超时。这里在**写路径**上判断"数据已经全部写进 socket"再关闭，
///          既不会截断响应，也不依赖客户端行为。
class ResponseCompletionGuard final : public relay::Stream {
public:
    ResponseCompletionGuard(relay::Stream* inner, relay::Stream* peer,
                            bool enabled)
        : m_inner(inner), m_peer(peer), m_enabled(enabled) {}

    streams::ReadResult Read(char* data, std::size_t size) override {
        return m_inner->Read(data, size);
    }

    streams::WriteResult Write(const char* data, std::size_t size) override {
        const streams::WriteResult result = m_inner->Write(data, size);
        if (result.ok() && m_enabled && result.count != 0) {
            Feed(reinterpret_cast<const std::uint8_t*>(data), result.count);
        }
        return result;
    }

    ErrorPtr Close() override { return m_inner->Close(); }

private:
    void Feed(const std::uint8_t* data, std::size_t length) {
        if (m_complete || m_disabled) {
            return;
        }
        if (!m_header_done) {
            m_head.append(reinterpret_cast<const char*>(data), length);
            const std::size_t end = m_head.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (m_head.size() > 65536) {
                    m_disabled = true;
                }
                return;
            }
            m_header_done = true;
            const std::string head = ToLower(m_head.substr(0, end));
            if (head.find("transfer-encoding: chunked") != std::string::npos) {
                m_disabled = true;  // chunked 需要更完整的解析，交给客户端关闭
                return;
            }
            const std::string key = "content-length:";
            const std::size_t position = head.find(key);
            if (position == std::string::npos) {
                m_disabled = true;
                return;
            }
            std::size_t begin = position + key.size();
            while (begin < head.size() &&
                   (head[begin] == ' ' || head[begin] == '\t')) {
                ++begin;
            }
            const char* digits = head.c_str() + begin;
            char* stop = nullptr;
            m_remaining = std::strtoull(digits, &stop, 10);
            if (stop == digits) {
                m_disabled = true;
                return;
            }
            m_has_length = true;
            Consume(m_head.size() - (end + 4));
            return;
        }
        Consume(length);
    }

    void Consume(std::size_t length) {
        if (!m_has_length || m_complete) {
            return;
        }
        const std::size_t take = std::min(length, m_remaining);
        m_remaining -= take;
        if (m_remaining != 0) {
            return;
        }
        m_complete = true;
        // 数据已经写进 socket 发送缓冲；先关隧道流（让对端收尾），
        // 再 shutdown 公网连接（TCP 会先发完缓冲再 FIN，不会截断）。
        if (m_peer != nullptr) {
            (void)m_peer->Close();
        }
        (void)m_inner->Close();
    }

    relay::Stream* m_inner;
    relay::Stream* m_peer;
    bool m_enabled;
    bool m_header_done = false;
    bool m_has_length = false;
    bool m_disabled = false;
    bool m_complete = false;
    std::uint64_t m_remaining = 0;
    std::string m_head;
};

/// 路由结果。
struct RouteResult {
    bool matched = false;
    std::string subdomain;
    std::string path;
    std::string prefix;
    bool host_mode = false;
    /// 路径第一段（即使没命中隧道也保留，用于把 404 升级成 502）。
    std::string candidate;
};

/// 从 `ip:port` 里取出 IP（IPv6 原样返回）。
std::string HostOnly(const std::string& ip_port) {
    const std::size_t colon = ip_port.rfind(':');
    if (colon == std::string::npos) {
        return ip_port;
    }
    if (ip_port.find(':') != colon) {
        return ip_port;  // IPv6 字面量，保留完整文本
    }
    return ip_port.substr(0, colon);
}

/// JSON 字符串转义（只处理引号、反斜杠与控制字符）。
std::string JsonEscape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (char character : text) {
        if (character == '"') {
            escaped.push_back('\\');
            escaped.push_back('"');
        } else if (character == '\\') {
            escaped.push_back('\\');
            escaped.push_back('\\');
        } else if (static_cast<unsigned char>(character) < 0x20U) {
            escaped.push_back('?');
        } else {
            escaped.push_back(character);
        }
    }
    return escaped;
}
}  // namespace

// ============================================================================
// TunnelServer
// ============================================================================

TunnelServer::TunnelServer(TunnelServerConfig config)
    : m_config(std::move(config)) {
    if (!m_config.token.empty()) {
        m_psk = DerivePsk(m_config.token);
        m_auth_required = true;
    }
    InitMetrics();
}

TunnelServer::~TunnelServer() { Stop(); }

void TunnelServer::InitMetrics() {
    go2cpp::metrics::Registry& registry = go2cpp::metrics::Registry::Default();
    m_metric_connections = &registry.GetCounter(
        "go2cpp_tunnel_connections_total", "Public HTTP connections accepted");
    m_metric_streams = &registry.GetCounter(
        "go2cpp_tunnel_streams_total", "Tunnel streams opened");
    m_metric_auth_failures = &registry.GetCounter(
        "go2cpp_tunnel_auth_failures_total", "Rejected tunnel handshakes");
    m_metric_bytes_in = &registry.GetCounter(
        "go2cpp_tunnel_bytes_in_total", "Bytes public -> tunnel");
    m_metric_bytes_out = &registry.GetCounter(
        "go2cpp_tunnel_bytes_out_total", "Bytes tunnel -> public");
    m_metric_online = &registry.GetGauge("go2cpp_tunnel_online",
                                         "Currently online tunnels");
}

void TunnelServer::BeginDrain() {
    if (m_draining.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::vector<MuxSession::Ptr> sessions;
    {
        std::lock_guard<std::mutex> lock(m_tunnels_mutex);
        for (auto& item : m_tunnels) {
            item.second->draining.store(true, std::memory_order_release);
            if (item.second->session) {
                sessions.push_back(item.second->session);
            }
            std::lock_guard<std::mutex> entry_lock(item.second->mutex);
            for (const MuxSession::Ptr& data : item.second->data_sessions) {
                sessions.push_back(data);
            }
        }
    }
    for (const MuxSession::Ptr& session : sessions) {
        (void)session->SendGoAway(1, 0);
    }
    Emit("[tunnel] drain: 已发送 GOAWAY，等待 " +
         std::to_string(sessions.size()) + " 条会话的在途流排空");
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              std::max(0, m_config.drain_timeout_ms));
    while (std::chrono::steady_clock::now() < deadline) {
        std::size_t active = 0;
        {
            std::lock_guard<std::mutex> lock(m_tunnels_mutex);
            for (auto& item : m_tunnels) {
                if (item.second->session) {
                    active += item.second->session->streams_active();
                }
                std::lock_guard<std::mutex> entry_lock(item.second->mutex);
                for (const MuxSession::Ptr& data : item.second->data_sessions) {
                    active += data->streams_active();
                }
            }
        }
        if (active == 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

MuxSession::Ptr TunnelServer::PickSession(
    const std::shared_ptr<Entry>& entry) {
    if (!entry) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    for (auto it = entry->data_sessions.begin();
         it != entry->data_sessions.end();) {
        if (!*it || !(*it)->alive()) {
            it = entry->data_sessions.erase(it);
        } else {
            ++it;
        }
    }
    if (entry->data_sessions.empty()) {
        return entry->session;
    }
    const std::size_t index =
        entry->round_robin.fetch_add(1, std::memory_order_relaxed) %
        entry->data_sessions.size();
    return entry->data_sessions[index];
}

std::string TunnelServer::BuildStatusJson() const {
    std::string json = "{\"server\":\"go2cpp-tunnel\",\"version\":1,\"public_port\":";
    json += std::to_string(m_public_port);
    json += ",\"draining\":";
    json += draining() ? "true" : "false";
    json += ",\"connections_total\":";
    json += std::to_string(m_connections_total.load(std::memory_order_relaxed));
    json += ",\"tunnels\":[";
    const std::vector<TunnelSnapshot> list = tunnels();
    for (std::size_t index = 0; index < list.size(); ++index) {
        if (index != 0) {
            json += ',';
        }
        const TunnelSnapshot& item = list[index];
        json += "{\"subdomain\":\"" + JsonEscape(item.subdomain) + "\",";
        json += "\"url\":\"" + JsonEscape(item.public_url) + "\",";
        json += "\"peer\":\"" + JsonEscape(item.peer) + "\",";
        json += "\"local\":\"" + JsonEscape(item.local_target) + "\",";
        json += "\"streams\":" + std::to_string(item.streams_active) + ",";
        json += "\"streams_total\":" + std::to_string(item.streams_total) + ",";
        json += "\"data_connections\":" +
                std::to_string(item.data_connections) + ",";
        json += "\"bytes_in\":" + std::to_string(item.bytes_in) + ",";
        json += "\"bytes_out\":" + std::to_string(item.bytes_out) + ",";
        json += "\"online_ms\":" + std::to_string(item.online_ms) + ",";
        json += "\"draining\":";
        json += item.draining ? "true" : "false";
        json += '}';
    }
    json += "]}";
    return json;
}

std::string TunnelServer::DetectLocalAddress() {
    // UDP socket 的 connect 只选路、不发包；离线时失败，回落到回环。
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return "127.0.0.1";
    }
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    ::inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
    std::string result = "127.0.0.1";
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&remote),
                  sizeof(remote)) == 0) {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
            char text[INET_ADDRSTRLEN] = {};
            if (::inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) !=
                nullptr) {
                result = text;
            }
        }
    }
    ::close(fd);
    return result;
}

bool TunnelServer::Start(ErrorPtr* err) {
    if (m_running.exchange(true)) {
        if (err != nullptr) {
            *err = NewError("tunnel: server already started");
        }
        return false;
    }
    m_display_host = m_config.display_host.empty()
                         ? DetectLocalAddress()
                         : m_config.display_host;

    std::string listen_error;
    m_control_listener = TcpListener::Listen(
        m_config.control_host, m_config.control_port, &listen_error);
    if (!m_control_listener) {
        if (err != nullptr) {
            *err = NewError("tunnel: control listen: " + listen_error);
        }
        m_running.store(false);
        return false;
    }
    m_public_listener = TcpListener::Listen(
        m_config.public_host, m_config.public_port, &listen_error);
    if (!m_public_listener) {
        if (err != nullptr) {
            *err = NewError("tunnel: public listen: " + listen_error);
        }
        m_control_listener->Close();
        m_control_listener.reset();
        m_running.store(false);
        return false;
    }
    m_control_port = m_control_listener->BoundPort();
    m_public_port = m_public_listener->BoundPort();

    m_control_thread = std::thread([this] { ControlAcceptLoop(); });
    m_public_thread = std::thread([this] { PublicAcceptLoop(); });
    return true;
}

void TunnelServer::Stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_control_listener) {
        m_control_listener->Close();
    }
    if (m_public_listener) {
        m_public_listener->Close();
    }
    if (m_control_thread.joinable()) {
        m_control_thread.join();
    }
    if (m_public_thread.joinable()) {
        m_public_thread.join();
    }
    // 优雅关闭：先发 GOAWAY 并等在途流排空，再强制断开。
    BeginDrain();
    std::vector<MuxSession::Ptr> sessions;
    {
        std::lock_guard<std::mutex> lock(m_tunnels_mutex);
        for (auto& entry : m_tunnels) {
            if (entry.second->session) {
                sessions.push_back(entry.second->session);
            }
            std::lock_guard<std::mutex> entry_lock(entry.second->mutex);
            for (const MuxSession::Ptr& data : entry.second->data_sessions) {
                sessions.push_back(data);
            }
            entry.second->data_sessions.clear();
        }
        m_tunnels.clear();
    }
    for (const MuxSession::Ptr& session : sessions) {
        session->Stop();
    }
    if (m_metric_online != nullptr) {
        m_metric_online->Set(0);
    }
    m_control_listener.reset();
    m_public_listener.reset();
    (void)WaitWorkers(kHeadTimeoutMs + 5000);
    Emit("server stopped");
}

void TunnelServer::ControlAcceptLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        TcpConn::Ptr conn =
            m_control_listener->Accept(std::max(50, m_config.accept_timeout_ms));
        if (!conn) {
            continue;
        }
        Spawn([this, conn] { HandleControl(conn); });
    }
}

void TunnelServer::PublicAcceptLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        TcpConn::Ptr conn =
            m_public_listener->Accept(std::max(50, m_config.accept_timeout_ms));
        if (!conn) {
            continue;
        }
        m_connections_total.fetch_add(1, std::memory_order_relaxed);
        if (m_metric_connections != nullptr) {
            m_metric_connections->Inc();
        }
        Spawn([this, conn] { HandlePublic(conn); });
    }
}

void TunnelServer::HandleControl(TcpConn::Ptr conn) {
    FrameReader reader(conn, m_config.with_checksum);
    frame::Frame frame;
    std::string error;

    // 0) 鉴权协商：服务端**总是**先发 16B 挑战。
    //    - 配置了 token：随机挑战，客户端回 ChaCha20-Poly1305 tag
    //      （PSK = sha256(token)，复用 bento::net::crypto 的 AEAD）；
    //    - 没配置 token：全零挑战，客户端回全零 tag，不做校验。
    //    这样客户端不必"先猜服务端有没有鉴权"，也没有额外往返。
    std::vector<std::uint8_t> challenge(kChallengeBytes, 0);
    if (m_auth_required) {
        challenge = MakeChallenge();
        if (challenge.size() != kChallengeBytes) {
            Emit("control handshake failed: CSPRNG unavailable");
            conn->Shutdown();
            return;
        }
    }
    if (!WriteFrame(conn, m_config.with_checksum, FrameType::kChallenge,
                    challenge)) {
        Emit("control handshake failed: cannot send challenge");
        conn->Shutdown();
        return;
    }
    if (!reader.Next(10000, &frame, &error)) {
        Emit("control handshake failed: " + error);
        conn->Shutdown();
        return;
    }
    {
        std::uint8_t tag[kAuthTagBytes] = {};
        const bool well_formed =
            static_cast<FrameType>(frame.type) == FrameType::kAuth &&
            DecodeAuth(frame.payload.data(), frame.payload.size(), tag);
        const bool authenticated =
            well_formed &&
            (!m_auth_required || VerifyAuthTag(m_psk, challenge.data(), tag));
        if (!authenticated) {
            if (m_metric_auth_failures != nullptr) {
                m_metric_auth_failures->Inc();
            }
            HelloAck ack;
            ack.status = Status::kUnauthorized;
            ack.message = "invalid token";
            std::vector<std::uint8_t> payload;
            if (EncodeHelloAck(ack, &payload, nullptr)) {
                (void)WriteFrame(conn, m_config.with_checksum,
                                 FrameType::kHelloAck, payload);
            }
            Emit("control handshake failed: unauthorized");
            conn->Shutdown();
            return;
        }
    }

    // 1) 同步读 HELLO：客户端在收到 ACK 之前不会再发别的帧，读第一帧安全。
    if (!reader.Next(10000, &frame, &error)) {
        Emit("control handshake failed: " + error);
        conn->Shutdown();
        return;
    }
    if (static_cast<FrameType>(frame.type) != FrameType::kHello) {
        Emit("control handshake failed: first frame is not HELLO");
        conn->Shutdown();
        return;
    }
    Hello hello;
    if (!DecodeHello(frame.payload.data(), frame.payload.size(), &hello,
                     nullptr)) {
        Emit("control handshake failed: malformed HELLO");
        conn->Shutdown();
        return;
    }

    // 2) 版本 / 子域名校验（token 已在上一步用 PSK 验证，不再比对明文）。
    HelloAck ack;
    std::string subdomain = hello.subdomain;
    if (hello.version != kProtocolVersion) {
        ack.status = Status::kBadVersion;
        ack.message = "protocol version " + std::to_string(hello.version) +
                      " != " + std::to_string(kProtocolVersion);
    } else if (!subdomain.empty() && !IsValidSubdomain(subdomain)) {
        ack.status = Status::kRejected;
        ack.message = "invalid subdomain";
    }
    if (ack.status != Status::kOk) {
        ack.subdomain = subdomain;
        std::vector<std::uint8_t> payload;
        if (EncodeHelloAck(ack, &payload, nullptr)) {
            (void)WriteFrame(conn, m_config.with_checksum, FrameType::kHelloAck,
                             payload);
        }
        Emit("tunnel rejected (" + std::string(StatusName(ack.status)) + "): " +
             subdomain + " " + ack.message);
        conn->Shutdown();
        return;
    }
    if (subdomain.empty()) {
        for (int attempt = 0; attempt < 8 && subdomain.empty(); ++attempt) {
            const std::string candidate = MakeRandomSubdomain(6);
            if (!Lookup(candidate)) {
                subdomain = candidate;
            }
        }
        if (subdomain.empty()) {
            ack.status = Status::kRejected;
            ack.message = "cannot allocate subdomain";
            conn->Shutdown();
            return;
        }
    }

    // 3) 建会话。数据连接（frp work conn / rathole data channel 的等价物）
    //    只承载流，不注册隧道；控制连接负责注册与心跳。
    MuxConfig mux_config;
    mux_config.recv_queue_cap = m_config.recv_queue_cap;
    mux_config.idle_timeout_ms = m_config.idle_timeout_ms;
    mux_config.ping_interval_ms = m_config.ping_interval_ms;
    mux_config.with_checksum = m_config.with_checksum;
    MuxSession::Ptr session = MuxSession::Create(conn, mux_config);
    if (!session) {
        conn->Shutdown();
        return;
    }
    const std::string local_target =
        "127.0.0.1:" + std::to_string(hello.local_port);
    const bool is_data_connection =
        (hello.flags & kHelloFlagDataConnection) != 0;

    if (is_data_connection) {
        std::shared_ptr<Entry> entry = Lookup(subdomain);
        if (!entry || !entry->session || !entry->session->alive()) {
            ack.status = Status::kRejected;
            ack.subdomain = subdomain;
            ack.message = "unknown tunnel for data connection";
            std::vector<std::uint8_t> payload;
            if (EncodeHelloAck(ack, &payload, nullptr)) {
                (void)WriteFrame(conn, m_config.with_checksum,
                                 FrameType::kHelloAck, payload);
            }
            Emit("data connection rejected: unknown tunnel " + subdomain);
            session->Stop();
            return;
        }
        ack.status = Status::kOk;
        ack.subdomain = subdomain;
        ack.public_url = BuildPublicUrl(subdomain, entry->scheme);
        ack.message = "data";
        std::vector<std::uint8_t> payload;
        if (!EncodeHelloAck(ack, &payload, nullptr) ||
            !session->SendPayload(FrameType::kHelloAck, payload)) {
            session->Stop();
            return;
        }
        ErrorPtr start_error;
        if (!session->Start(MuxSession::OnOpen(), &start_error)) {
            Emit("data session start failed: " + ErrorMessage(start_error));
            session->Stop();
            return;
        }
        std::size_t data_count = 0;
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->data_sessions.push_back(session);
            data_count = entry->data_sessions.size();
        }
        Emit("[tunnel] " + subdomain + " 数据连接 +1（当前 " +
             std::to_string(data_count) + "）");
        while (session->alive() && m_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        session->Shutdown("data connection closing");
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            const auto found =
                std::find(entry->data_sessions.begin(),
                          entry->data_sessions.end(), session);
            if (found != entry->data_sessions.end()) {
                entry->data_sessions.erase(found);
            }
            data_count = entry->data_sessions.size();
        }
        session->Stop();
        Emit("[tunnel] " + subdomain + " 数据连接 -1（当前 " +
             std::to_string(data_count) + "）");
        return;
    }

    const std::string scheme = m_config.domain.empty() ? "path" : "host";
    ack.status = Status::kOk;
    ack.subdomain = subdomain;
    ack.public_url = BuildPublicUrl(subdomain, scheme);
    ack.message = local_target;
    std::vector<std::uint8_t> payload;
    if (!EncodeHelloAck(ack, &payload, nullptr) ||
        !session->SendPayload(FrameType::kHelloAck, payload)) {
        Emit("control handshake failed: cannot send ACK");
        session->Stop();
        return;
    }
    ErrorPtr start_error;
    if (!session->Start(MuxSession::OnOpen(), &start_error)) {
        Emit("control session start failed: " + ErrorMessage(start_error));
        session->Stop();
        return;
    }
    const std::string peer = conn->PeerText();
    (void)Register(subdomain, session, peer, local_target, scheme);
    Emit("[tunnel] " + subdomain + " 上线 ← " + peer + " (" + local_target +
         ")  " + ack.public_url);
    if (m_metric_online != nullptr) {
        m_metric_online->Inc();
    }

    while (session->alive() && m_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (session->alive()) {
        session->Shutdown("server stopping");
    }
    Unregister(subdomain, session.get());
    session->Stop();
    if (m_metric_online != nullptr) {
        m_metric_online->Dec();
    }
    Emit("[tunnel] " + subdomain + " 下线");
}

void TunnelServer::HandlePublic(TcpConn::Ptr conn) {
    std::string head_raw;
    std::string extra;
    std::string error;
    if (!ReadHttpHead(conn, &head_raw, &extra, &error)) {
        if (!error.empty() && error != "request head timeout" &&
            error != "connection closed before request head") {
            SendResponse(conn, 400, "Bad Request", "text/plain; charset=utf-8",
                         error + "\n");
        } else {
            conn->Shutdown();
        }
        return;
    }
    HttpHead head;
    if (!ParseHttpHead(head_raw, &head)) {
        SendResponse(conn, 400, "Bad Request", "text/plain; charset=utf-8",
                     "malformed request line\n");
        return;
    }
    if (head.method == "CONNECT") {
        SendResponse(conn, 405, "Method Not Allowed",
                     "text/plain; charset=utf-8",
                     "CONNECT is not supported by the HTTP tunnel\n");
        return;
    }

    // 可观测端点：/__tunnel__/status 返回 JSON（与 Prometheus 指标同源）。
    const std::string path = ExtractPath(head.target);
    if (path == "/__tunnel__/status") {
        SendResponse(conn, 200, "OK", "application/json; charset=utf-8",
                     BuildStatusJson());
        return;
    }

    // 路由：先路径前缀，再 Host 子域名。
    RouteResult route;
    {
        const std::size_t query = path.find('?');
        const std::size_t path_end =
            query == std::string::npos ? path.size() : query;
        const std::size_t segment_end = std::min(
            path_end == 0 ? path.size() : path.find('/', 1), path_end);
        const std::string first =
            path.size() > 1 ? path.substr(1, segment_end - 1) : std::string();
        if (!first.empty() && IsValidSubdomain(first)) {
            route.candidate = first;
        }
        if (!first.empty() && IsValidSubdomain(first) && Lookup(first)) {
            route.matched = true;
            route.subdomain = first;
            route.prefix = "/" + first;
            std::string rest = path.substr(segment_end);
            if (rest.empty()) {
                rest = "/";
            } else if (rest.front() == '?') {
                rest = "/" + rest;
            }
            route.path = m_config.strip_prefix ? rest : path;
            route.host_mode = false;
        }
    }
    if (!route.matched && !m_config.domain.empty()) {
        const std::string host = NormalizeHost(GetHeader(head, "host"));
        const std::string suffix = "." + ToLower(m_config.domain);
        if (host.size() > suffix.size() &&
            host.compare(host.size() - suffix.size(), suffix.size(), suffix) ==
                0) {
            const std::string candidate =
                host.substr(0, host.size() - suffix.size());
            if (candidate.find('.') == std::string::npos &&
                IsValidSubdomain(candidate) && Lookup(candidate)) {
                route.matched = true;
                route.subdomain = candidate;
                route.path = path;
                route.host_mode = true;
            }
        }
    }

    if (!route.matched && !route.candidate.empty() &&
        WasRecentlyOffline(route.candidate)) {
        SendResponse(conn, 502, "Bad Gateway", "text/plain; charset=utf-8",
                     "tunnel '" + route.candidate +
                         "' is offline (client disconnected)\n");
        return;
    }
    if (!route.matched) {
        if (path == "/" || path == "/index.html" || path.rfind("/?", 0) == 0) {
            std::vector<TunnelSnapshot> snapshots = tunnels();
            std::string body =
                "<!doctype html><html><head><meta charset=\"utf-8\">"
                "<title>Go2Cpp Tunnel</title><style>body{font-family:system-ui,"
                "sans-serif;margin:2rem;max-width:56rem}table{border-collapse:"
                "collapse;width:100%}th,td{border-bottom:1px solid #ddd;padding:"
                ".4rem .6rem;text-align:left;font-size:.95rem}code{background:"
                "#f4f4f4;padding:.1rem .3rem;border-radius:3px}</style></head>"
                "<body><h1>Go2Cpp Tunnel</h1>";
            if (snapshots.empty()) {
                body += "<p>当前没有在线隧道。在本地执行：<br><code>"
                        "tunnel-client --server &lt;vps&gt;:7443 --local 8080 "
                        "--subdomain shop</code></p>";
            } else {
                body += "<p>在线隧道：</p><table><tr><th>子域名</th><th>公网 "
                        "URL</th><th>本地目标</th><th>在线</th></tr>";
                for (const TunnelSnapshot& snapshot : snapshots) {
                    body += "<tr><td>" + HtmlEscape(snapshot.subdomain) +
                            "</td><td><a href=\"" +
                            HtmlEscape(snapshot.public_url) + "\">" +
                            HtmlEscape(snapshot.public_url) + "</a></td><td>" +
                            HtmlEscape(snapshot.local_target) + "</td><td>" +
                            std::to_string(snapshot.online_ms / 1000) +
                            " s</td></tr>";
                }
                body += "</table>";
            }
            body += "</body></html>";
            SendResponse(conn, 200, "OK", "text/html; charset=utf-8", body);
        } else {
            SendResponse(conn, 404, "Not Found", "text/plain; charset=utf-8",
                         "no tunnel for path " + path + "\n");
        }
        return;
    }

    std::shared_ptr<Entry> entry = Lookup(route.subdomain);
    if (!entry || !entry->session || !entry->session->alive()) {
        SendResponse(conn, 502, "Bad Gateway", "text/plain; charset=utf-8",
                     "tunnel '" + route.subdomain + "' is offline\n");
        return;
    }
    if (entry->session && entry->session->streams_active() >=
                               m_config.max_streams_per_tunnel) {
        SendResponse(conn, 503, "Service Unavailable",
                     "text/plain; charset=utf-8",
                     "tunnel '" + route.subdomain + "' is at stream limit\n");
        return;
    }
    const std::string peer_ip_only = HostOnly(conn->PeerText());
    MuxSession::Ptr session = PickSession(entry);
    if (!session || !session->alive()) {
        SendResponse(conn, 502, "Bad Gateway", "text/plain; charset=utf-8",
                     "tunnel '" + route.subdomain + "' has no live session\n");
        return;
    }
    MuxStream::Ptr stream = session->OpenStream(peer_ip_only, 0);
    if (!stream) {
        SendResponse(conn, 502, "Bad Gateway", "text/plain; charset=utf-8",
                     "cannot open tunnel stream\n");
        return;
    }

    // 把「重写后的头 + 已预读的 body」一起作为预置字节送进隧道。
    std::string prefix = RewriteRequestHead(
        head, route.path, peer_ip_only, route.prefix,
        m_config.strip_prefix && !route.host_mode);
    prefix += extra;

    SocketStream public_stream(conn, 1000);
    PrefixedStream public_with_prefix(&public_stream, std::move(prefix));
    // 只有「路径前缀 + 强制 close」模式才按 Content-Length 主动收尾：
    // 子域名模式下一个隧道流可能承载多个 keep-alive 响应，不能提前关。
    ResponseCompletionGuard public_guard(
        &public_with_prefix, stream.get(),
        m_config.strip_prefix && !route.host_mode);
    EofClosingStream public_side(&public_guard, stream.get());
    EofClosingStream tunnel_side(stream.get(), &public_with_prefix);
    if (m_metric_streams != nullptr) {
        m_metric_streams->Inc();
    }
    relay::PipeConfig pipe_config;
    pipe_config.copy_buf = 32U * 1024U;
    pipe_config.queue_cap = 256U * 1024U;
    pipe_config.write_timeout = std::chrono::milliseconds(30000);
    relay::RelayPipe pipe(&public_side, &tunnel_side, pipe_config);
    ErrorPtr pipe_error;
    (void)pipe.Run(nullptr, &pipe_error);
    stream->Close();
    conn->Shutdown();
}

void TunnelServer::Emit(const std::string& message) const {
    EventHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_event_mutex);
        handler = m_event_handler;
    }
    if (handler) {
        handler(message);
    }
}

void TunnelServer::set_event_handler(EventHandler handler) {
    std::lock_guard<std::mutex> lock(m_event_mutex);
    m_event_handler = std::move(handler);
}

std::string TunnelServer::BuildPublicUrl(const std::string& subdomain,
                                         const std::string& scheme) const {
    std::string url = "http://";
    if (!m_config.domain.empty() && scheme == "host") {
        url += subdomain + "." + m_config.domain;
        if (m_public_port != 80) {
            url += ":" + std::to_string(m_public_port);
        }
        url += "/";
        return url;
    }
    url += m_display_host + ":" + std::to_string(m_public_port) + "/" +
           subdomain + "/";
    return url;
}

std::shared_ptr<TunnelServer::Entry> TunnelServer::Lookup(
    const std::string& subdomain) const {
    std::lock_guard<std::mutex> lock(m_tunnels_mutex);
    const auto found = m_tunnels.find(subdomain);
    if (found == m_tunnels.end()) {
        return nullptr;
    }
    return found->second;
}

std::shared_ptr<TunnelServer::Entry> TunnelServer::Register(
    const std::string& subdomain, const MuxSession::Ptr& session,
    const std::string& peer, const std::string& local_target,
    const std::string& scheme) {
    auto entry = std::make_shared<Entry>();
    entry->subdomain = subdomain;
    entry->session = session;
    entry->peer = peer;
    entry->local_target = local_target;
    entry->scheme = scheme;
    entry->started_ms = NowMs();
    std::shared_ptr<Entry> previous;
    {
        std::lock_guard<std::mutex> lock(m_tunnels_mutex);
        const auto found = m_tunnels.find(subdomain);
        if (found != m_tunnels.end()) {
            previous = found->second;
        }
        m_tunnels[subdomain] = entry;
    }
    if (previous && previous->session &&
        previous->session.get() != session.get()) {
        // 同名注册以最新连接为准：先给旧会话发 GOAWAY（让在途流收尾），
        // 再断开；旧的数据连接一并回收。
        std::vector<MuxSession::Ptr> old_data;
        {
            std::lock_guard<std::mutex> lock(previous->mutex);
            old_data.swap(previous->data_sessions);
        }
        (void)previous->session->SendGoAway(2, 0);
        for (const MuxSession::Ptr& data : old_data) {
            if (data) {
                (void)data->SendGoAway(2, 0);
            }
        }
        previous->session->Shutdown("replaced by newer registration");
        for (const MuxSession::Ptr& data : old_data) {
            if (data) {
                data->Shutdown("replaced by newer registration");
            }
        }
    }
    return entry;
}

void TunnelServer::Unregister(const std::string& subdomain,
                              const MuxSession* session) {
    std::lock_guard<std::mutex> lock(m_tunnels_mutex);
    const auto found = m_tunnels.find(subdomain);
    if (found == m_tunnels.end()) {
        return;
    }
    if (session != nullptr && found->second->session.get() != session) {
        return;  // 已被更新的注册替换
    }
    m_tunnels.erase(found);
    const std::int64_t now = NowMs();
    m_offline[subdomain] = now;
    for (auto it = m_offline.begin(); it != m_offline.end();) {
        if (now - it->second > 300000) {
            it = m_offline.erase(it);
        } else {
            ++it;
        }
    }
}

bool TunnelServer::WasRecentlyOffline(const std::string& subdomain) const {
    std::lock_guard<std::mutex> lock(m_tunnels_mutex);
    const auto found = m_offline.find(subdomain);
    if (found == m_offline.end()) {
        return false;
    }
    return NowMs() - found->second < 300000;
}

std::vector<TunnelSnapshot> TunnelServer::tunnels() const {
    std::vector<std::shared_ptr<Entry>> entries;
    {
        std::lock_guard<std::mutex> lock(m_tunnels_mutex);
        entries.reserve(m_tunnels.size());
        for (const auto& item : m_tunnels) {
            entries.push_back(item.second);
        }
    }
    std::vector<TunnelSnapshot> snapshots;
    snapshots.reserve(entries.size());
    const std::int64_t now = NowMs();
    for (const std::shared_ptr<Entry>& entry : entries) {
        TunnelSnapshot snapshot;
        snapshot.subdomain = entry->subdomain;
        snapshot.public_url = BuildPublicUrl(entry->subdomain, entry->scheme);
        snapshot.peer = entry->peer;
        snapshot.local_target = entry->local_target;
        snapshot.scheme = entry->scheme;
        if (entry->session) {
            snapshot.streams_active = entry->session->streams_active();
            snapshot.streams_total = entry->session->streams_total();
            snapshot.bytes_in = entry->session->bytes_in();
            snapshot.bytes_out = entry->session->bytes_out();
        }
        snapshot.online_ms = now - entry->started_ms;
        snapshot.draining = entry->draining.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            snapshot.data_connections = entry->data_sessions.size();
        }
        snapshots.push_back(std::move(snapshot));
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const TunnelSnapshot& left, const TunnelSnapshot& right) {
                  return left.subdomain < right.subdomain;
              });
    return snapshots;
}

std::string TunnelServer::PublicUrlFor(const std::string& subdomain) const {
    const std::shared_ptr<Entry> entry = Lookup(subdomain);
    if (!entry) {
        return {};
    }
    return BuildPublicUrl(entry->subdomain, entry->scheme);
}

void TunnelServer::Spawn(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(m_worker_mutex);
        ++m_workers;
    }
    try {
        std::thread([this, task = std::move(task)]() mutable {
            task();
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
    }
}

bool TunnelServer::WaitWorkers(int timeout_ms) {
    std::unique_lock<std::mutex> lock(m_worker_mutex);
    return m_worker_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [this] { return m_workers == 0; });
}

}  // namespace tunnel
}  // namespace go2cpp
