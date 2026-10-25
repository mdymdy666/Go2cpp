/**
 * @file http_transport.cpp
 * @brief HTTP / SSE 前端实现：多客户端会话路由、通知定向投递、生命周期清理。
 */

#include "http_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/scheduler/scheduler.hpp"
#include "go2cpp/tools/go.hpp"

namespace go2cpp::mcp {

namespace {

using SocketPtr = go2cpp::Socket::Ptr;
using go2cpp::http::ParseState;
using go2cpp::http::Status;

/// 把 Socket 写满（处理部分写）。
bool WriteAll(go2cpp::Socket& socket, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto result = socket.send(data.data() + offset, data.size() - offset);
        if (!result.ok() || result.transferred == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(result.transferred);
    }
    return true;
}

/// 单调时钟毫秒值；只用于比较，不表示墙上时间。
long long NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int HexValue(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

/// 查询串百分号解码；sessionId 本身是十六进制，这里主要兼容手工测试。
std::string PercentDecode(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char current = text[index];
        if (current == '+') {
            decoded.push_back(' ');
            continue;
        }
        if (current == '%' && index + 2 < text.size()) {
            const int high = HexValue(text[index + 1]);
            const int low = HexValue(text[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>(high * 16 + low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(current);
    }
    return decoded;
}

/// 从 `a=1&sessionId=x&b=2` 中取指定参数；缺失返回空串。
std::string QueryParam(std::string_view query, std::string_view name) {
    std::size_t position = 0;
    while (position <= query.size()) {
        const std::size_t separator = query.find('&', position);
        const std::size_t end = separator == std::string_view::npos ? query.size() : separator;
        const std::string_view pair = query.substr(position, end - position);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == name) {
            return PercentDecode(pair.substr(equals + 1));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        position = separator + 1;
    }
    return {};
}

bool IsSessionMessagePath(const std::string& path) {
    return path == "/message" || path == "/messages";
}

/// HTTP 层错误也返回 JSON-RPC 形状，客户端可统一解析。
std::string JsonRpcErrorBody(int code, const std::string& message) {
    json body = json::object();
    body["jsonrpc"] = "2.0";
    body["id"] = nullptr;
    json error = json::object();
    error["code"] = code;
    error["message"] = message;
    body["error"] = std::move(error);
    return body.dump();
}

bool WriteSimple(go2cpp::Socket& socket, Status status, const char* content_type,
                 const std::string& body, bool close) {
    go2cpp::http::Response response;
    response.status = status;
    response.header.set("Content-Type", content_type);
    response.body = body;
    response.close = close;
    return WriteAll(socket, response.to_string());
}

bool WriteHttpError(go2cpp::Socket& socket, Status status, int code,
                    const std::string& message, bool close) {
    return WriteSimple(socket, status, "application/json",
                       JsonRpcErrorBody(code, message), close);
}

/**
 * @brief 非阻塞探测 SSE 对端是否已关闭。
 *
 * 用 `MSG_PEEK|MSG_DONTWAIT` + 0 超时，避免占用 worker：
 * - 返回 0 字节：FIN，已断开；
 * - 返回 >0：客户端发了意外数据，读掉 1 字节后视为仍在线；
 * - EAGAIN/EWOULDBLOCK/ETIMEDOUT：暂无数据，仍在线；
 * - 其它错误（ECONNRESET 等）：已断开。
 */
bool PeerClosed(go2cpp::Socket& socket) {
    char byte = 0;
    const auto result = socket.recv(&byte, 1, MSG_PEEK | MSG_DONTWAIT,
                                    std::chrono::milliseconds(0));
    if (result.ok()) {
        if (result.transferred == 0) {
            return true;
        }
        char discarded = 0;
        (void)socket.recv(&discarded, 1, MSG_DONTWAIT, std::chrono::milliseconds(0));
        return false;
    }
    return result.error != EAGAIN && result.error != EWOULDBLOCK &&
           result.error != ETIMEDOUT && result.error != EINTR;
}

/// 读取正整数环境变量；非法值回退默认并告警（用于测试缩短超时）。
int EnvironmentInt(const char* name, int fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value < 0 || value > 86400000L) {
        std::fprintf(stderr, "[http] 忽略非法环境变量 %s=%s\n", name, raw);
        return fallback;
    }
    return static_cast<int>(value);
}

/// 只在当前 Fiber 内绑定 sessionId，析构时清掉，避免后续请求读到旧值。
class ScopedSessionBinding {
public:
    ScopedSessionBinding(go2cpp::FiberLocal<std::string>& slot, std::string id)
        : m_slot(slot) {
        m_slot.GetOrCreate() = std::move(id);
    }

    ~ScopedSessionBinding() { m_slot.Reset(); }

    ScopedSessionBinding(const ScopedSessionBinding&) = delete;
    ScopedSessionBinding& operator=(const ScopedSessionBinding&) = delete;

private:
    go2cpp::FiberLocal<std::string>& m_slot;
};

}  // namespace

HttpTransport::HttpTransport(Server& server, Options options)
    : m_server(server), m_options(std::move(options)) {}

HttpTransport::~HttpTransport() {
    DetachNotificationSinks();
    m_notification_target.reset();
}

void HttpTransport::DetachNotificationSinks() {
    if (!m_notification_target) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_notification_target->mutex);
        m_notification_target->transport = nullptr;
    }
    m_server.SetNotificationSink({});
    m_server.SetBroadcastSink({});
}

bool HttpTransport::Start(std::uint16_t& actual_port, std::string& error) {
    m_options.heartbeat_interval_ms =
        EnvironmentInt("MCP_SSE_HEARTBEAT_MS", m_options.heartbeat_interval_ms);
    m_options.poll_interval_ms = EnvironmentInt("MCP_SSE_POLL_MS", m_options.poll_interval_ms);
    m_options.session_idle_timeout_ms =
        EnvironmentInt("MCP_SSE_IDLE_TIMEOUT_MS", m_options.session_idle_timeout_ms);

    m_listener = go2cpp::net::ListenTCP(m_options.host, m_options.port);
    if (!m_listener) {
        error = "监听失败: " + m_options.host + ":" + std::to_string(m_options.port);
        return false;
    }
    actual_port = m_options.port;
    if (actual_port == 0) {
        const auto local =
            std::dynamic_pointer_cast<go2cpp::net::IPAddress>(m_listener->local_address());
        if (local) {
            actual_port = local->port();
        }
    }

    // 通知出口：请求 Fiber 内投递到绑定的 SSE 会话；无绑定的服务级通知广播。
    // 用 shared_ptr 间接层：析构时置空 transport，插件线程不会回调悬空 this。
    m_notification_target = std::make_shared<NotificationTarget>();
    m_notification_target->transport = this;
    const auto target = m_notification_target;
    m_server.SetNotificationSink([target](const std::string& payload) {
        std::lock_guard<std::mutex> lock(target->mutex);
        if (target->transport != nullptr) {
            target->transport->RouteNotification(payload);
        }
    });
    // 广播出口：插件主动通知 / SendBroadcastNotification，投递所有活跃会话。
    m_server.SetBroadcastSink([target](const std::string& payload) {
        std::lock_guard<std::mutex> lock(target->mutex);
        if (target->transport != nullptr) {
            target->transport->BroadcastNotification(payload);
        }
    });
    return true;
}

void HttpTransport::Stop() {
    m_stopped.store(true);
    if (m_listener) {
        m_listener->close();
    }
    DetachNotificationSinks();

    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(m_sessions_mutex);
        sessions.reserve(m_sessions.size());
        for (auto& entry : m_sessions) {
            entry.second->closed.store(true);
            sessions.push_back(entry.second);
        }
        m_sessions.clear();
    }
    for (auto& session : sessions) {
        session->queue->Close();
    }
}

void HttpTransport::Run() {
    const int workers = std::max(1, m_options.workers);
    go2cpp::Scheduler scheduler(static_cast<std::size_t>(workers));
    scheduler.start();

    while (!m_stopped.load()) {
        auto connection = m_listener->accept(std::chrono::milliseconds(200));
        if (!connection) {
            continue;
        }
        go2cpp::go(scheduler, [this, connection]() mutable {
            HandleConnection(std::move(connection));
        });
    }
    scheduler.shutdown();
}

void HttpTransport::HandleConnection(SocketPtr connection) {
    // 半开/慢读连接的最长阻塞时间：POST 读写与 SSE 推送共用，避免 Fiber 永久挂起。
    (void)connection->set_recv_timeout(std::chrono::seconds(30));
    (void)connection->set_send_timeout(std::chrono::seconds(30));

    go2cpp::http::Parser parser;
    std::string buffered;

    // 单连接最多复用 32 次，防止恶意长连接长期占用 Fiber。
    for (int served = 0; served < 32 && !m_stopped.load(); ++served) {
        go2cpp::http::Request request;
        bool complete = false;

        while (!complete) {
            go2cpp::http::ParseResult result;
            if (!buffered.empty()) {
                // 上一条请求之后残留的字节（keep-alive / 流水线），先喂给解析器。
                std::string chunk;
                chunk.swap(buffered);
                result = parser.feed(chunk, request);
            } else {
                char buffer[8192];
                const auto received = connection->recv(buffer, sizeof(buffer));
                if (!received.ok() || received.transferred == 0) {
                    connection->close();
                    return;
                }
                result = parser.feed(
                    std::string_view(buffer, static_cast<std::size_t>(received.transferred)),
                    request);
            }

            if (result.state == ParseState::Complete) {
                complete = true;
            } else if (result.state == ParseState::Error) {
                WriteHttpError(*connection, Status::BadRequest, -32700,
                               "invalid HTTP request", true);
                connection->close();
                return;
            }
        }

        // 解析器内部缓冲可能还含有下一条请求；取出后下一轮继续解析。
        buffered = parser.take_remaining();

        const std::string path = request.path();
        const std::string query = request.query();

        if (request.method == go2cpp::http::Method::Get && path == "/sse") {
            (void)ServeSse(std::move(connection));
            return;  // SSE 独占连接
        }

        if (request.method == go2cpp::http::Method::Post && path == "/mcp") {
            if (!ServePost(connection, request)) {
                return;
            }
        } else if (request.method == go2cpp::http::Method::Post &&
                   IsSessionMessagePath(path)) {
            if (!ServeSessionPost(connection, request, query)) {
                return;
            }
        } else {
            WriteSimple(*connection, Status::NotFound, "text/plain", "not found", true);
            connection->close();
            return;
        }

        if (request.close) {
            connection->close();
            return;
        }
    }
    connection->close();
}

bool HttpTransport::ServePost(SocketPtr connection, const go2cpp::http::Request& request) {
    const auto response_text = m_server.HandleMessage(request.body);

    go2cpp::http::Response response;
    response.close = request.close;
    if (response_text) {
        response.status = Status::OK;
        response.header.set("Content-Type", "application/json");
        response.body = *response_text;
    } else {
        response.status = Status::NoContent;
    }
    return WriteAll(*connection, response.to_string());
}

bool HttpTransport::ServeSessionPost(SocketPtr connection,
                                     const go2cpp::http::Request& request,
                                     const std::string& query) {
    const std::string session_id = QueryParam(query, "sessionId");
    if (session_id.empty()) {
        return WriteHttpError(*connection, Status::BadRequest, -32600,
                              "missing sessionId query parameter", request.close);
    }

    auto session = FindSession(session_id);
    if (!session) {
        return WriteHttpError(*connection, Status::NotFound, -32000,
                              "unknown or expired sessionId", request.close);
    }
    session->last_active_ms.store(NowMillis(), std::memory_order_relaxed);

    std::optional<std::string> response_text;
    {
        // 通知 sink 读取 FiberLocal，把工具执行期间的通知投递回同一会话。
        ScopedSessionBinding binding(m_bound_session, session->id);
        response_text = m_server.HandleMessage(request.body);
    }

    if (response_text) {
        // 响应必须有界等待：队列满说明客户端读得太慢，给 5s 背压而不是静默丢失。
        if (!PushToSession(session->id, *response_text, true)) {
            return WriteHttpError(*connection, Status::NotFound, -32000,
                                  "session closed while handling request", request.close);
        }
        session->last_active_ms.store(NowMillis(), std::memory_order_relaxed);
    }

    go2cpp::http::Response accepted;
    accepted.status = Status::OK;  // Go2Cpp Status 无 202，用 200 + "accepted"
    accepted.header.set("Content-Type", "text/plain");
    accepted.body = "accepted";
    accepted.close = request.close;
    return WriteAll(*connection, accepted.to_string());
}

bool HttpTransport::ServeSse(SocketPtr connection) {
    static const std::string kSseHeader =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n";
    if (!WriteAll(*connection, kSseHeader)) {
        connection->close();
        return false;
    }

    auto session = std::make_shared<Session>();
    session->id = NewSessionId();
    session->queue = go2cpp::MakeChannel<std::string>(m_options.session_queue_capacity);
    session->last_active_ms.store(NowMillis(), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_sessions_mutex);
        m_sessions[session->id] = session;
    }

    // 规范：先推 endpoint 事件，客户端再向该 URI POST。
    const std::string endpoint =
        "event: endpoint\ndata: /message?sessionId=" + session->id + "\n\n";
    if (!WriteAll(*connection, endpoint)) {
        CloseSession(session);
        connection->close();
        return false;
    }

    const int heartbeat_ms = std::max(100, m_options.heartbeat_interval_ms);
    const int configured_poll_ms = m_options.poll_interval_ms > 0 ? m_options.poll_interval_ms : 500;
    const int poll_ms = std::max(50, std::min(configured_poll_ms, heartbeat_ms));
    const auto heartbeat = std::chrono::milliseconds(heartbeat_ms);
    const auto poll = std::chrono::milliseconds(poll_ms);
    auto next_heartbeat = std::chrono::steady_clock::now() + heartbeat;

    while (!m_stopped.load() && !session->closed.load()) {
        const auto now = std::chrono::steady_clock::now();
        auto wait = poll;
        if (now + wait > next_heartbeat) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(next_heartbeat - now);
            wait = remaining.count() > 0 ? remaining : std::chrono::milliseconds(0);
        }

        const auto selected = go2cpp::Select({go2cpp::RecvCase(session->queue)}, {}, wait);
        if (m_stopped.load() || session->closed.load()) {
            break;
        }

        if (selected.selected) {
            const auto payload = selected.Value<std::string>();
            if (!payload) {
                break;  // 队列关闭
            }
            const std::string frame = "event: message\ndata: " + *payload + "\n\n";
            if (!WriteAll(*connection, frame)) {
                break;
            }
            session->last_active_ms.store(NowMillis(), std::memory_order_relaxed);
            continue;
        }

        const auto after = std::chrono::steady_clock::now();
        if (after >= next_heartbeat) {
            if (!WriteAll(*connection, ": ping\n\n")) {
                break;
            }
            next_heartbeat = after + heartbeat;
        }

        if (m_options.session_idle_timeout_ms > 0) {
            const long long idle =
                NowMillis() - session->last_active_ms.load(std::memory_order_relaxed);
            if (idle >= m_options.session_idle_timeout_ms) {
                break;
            }
        }
        if (PeerClosed(*connection)) {
            break;
        }
    }

    CloseSession(session);
    connection->close();
    return true;
}

std::shared_ptr<HttpTransport::Session> HttpTransport::FindSession(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_sessions_mutex);
    const auto found = m_sessions.find(id);
    if (found == m_sessions.end() || found->second->closed.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return found->second;
}

void HttpTransport::CloseSession(const std::shared_ptr<Session>& session) {
    {
        std::lock_guard<std::mutex> lock(m_sessions_mutex);
        session->closed.store(true, std::memory_order_release);
        const auto found = m_sessions.find(session->id);
        if (found != m_sessions.end() && found->second == session) {
            m_sessions.erase(found);
        }
    }
    session->queue->Close();
}

bool HttpTransport::PushToSession(const std::string& id, const std::string& payload, bool wait) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(m_sessions_mutex);
        const auto found = m_sessions.find(id);
        if (found == m_sessions.end() ||
            found->second->closed.load(std::memory_order_acquire)) {
            return false;
        }
        session = found->second;
    }

    if (!wait) {
        // 通知不能阻塞：插件工作线程也可能触发 sink，TrySend 满则丢弃。
        return session->queue->TrySend(payload).Ok();
    }
    return session->queue->SendFor(payload, std::chrono::milliseconds(5000)).Ok();
}

void HttpTransport::RouteNotification(const std::string& payload) {
    const auto* bound = m_bound_session.TryGet();
    if (bound != nullptr && !bound->empty()) {
        (void)PushToSession(*bound, payload, false);
        return;
    }
    // 无会话绑定：插件工作线程或 POST /mcp。这类通知没有归属会话，
    // 按旧版语义作为服务级事件广播给所有活跃客户端。
    BroadcastNotification(payload);
}

void HttpTransport::BroadcastNotification(const std::string& payload) {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(m_sessions_mutex);
        sessions.reserve(m_sessions.size());
        for (const auto& entry : m_sessions) {
            sessions.push_back(entry.second);
        }
    }
    for (const auto& session : sessions) {
        (void)session->queue->TrySend(payload);
    }
}

std::string HttpTransport::NewSessionId() {
    const std::uint64_t sequence =
        m_next_session_id.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "s%llx-%llx",
                  static_cast<unsigned long long>(micros),
                  static_cast<unsigned long long>(sequence));
    return std::string(buffer);
}

}  // namespace go2cpp::mcp
