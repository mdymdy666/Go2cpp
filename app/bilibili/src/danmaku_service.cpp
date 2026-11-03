/**
 * @file danmaku_service.cpp
 * @brief B 站弹幕服务（端口 29203）：REST 历史/在线数 + WebSocket 实时广播。
 *
 * @details 单进程、每连接一个处理线程：
 *          - REST：`GET /api/danmaku/:video_id?from=&to=`、`GET /api/danmaku/:video_id/online`；
 *          - WS：`/ws/danmaku?video_id=1`，文本帧 `{"type":"send",...}`；
 *          - 校验：1–100 字符、屏蔽词、5 条/10s/用户（游客按连接计）；
 *          - 入库后广播 `{"type":"danmaku",...}`，在线数变化广播 `{"type":"online",...}`；
 *          - 非法 JSON 回 `{"type":"error","code":400,...}`，不断连。
 *
 * @author bili demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "bili_danmaku_room.hpp"
#include "bili_http.hpp"
#include "bili_store.hpp"

#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/websocket.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

using go2cpp::net::Socket;

/// @brief 服务端配置（命令行解析结果）。
struct ServerOptions {
    std::uint16_t port{29203};
    std::string db_path{bili::DanmakuStore::kDefaultPath};
    std::size_t rate_limit{5};
    std::int64_t rate_window_ms{10000};
    std::size_t max_queue{256};
    bool verbose{false};
};

/// @brief 时间戳 `HH:MM:SS.mmm`（本地时间）。
std::string NowStamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t seconds = system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&seconds, &local);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", local.tm_hour,
                  local.tm_min, local.tm_sec, static_cast<int>(millis.count()));
    return std::string(buffer);
}

/// @brief 带时间戳的 stdout 日志。
void Log(const std::string& message) {
    std::fprintf(stdout, "[%s] %s\n", NowStamp().c_str(), message.c_str());
    std::fflush(stdout);
}

/// @brief 限流器：滑动窗口内每 key 最多 limit 次；limit=0 表示关闭。
class RateLimiter {
   public:
    RateLimiter(std::size_t limit, std::int64_t window_ms)
        : m_limit(limit), m_window_ms(window_ms) {}

    bool Allow(const std::string& key) {
        if (m_limit == 0) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto cutoff = now - std::chrono::milliseconds(m_window_ms);
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& stamps = m_stamps[key];
        while (!stamps.empty() && stamps.front() < cutoff) {
            stamps.pop_front();
        }
        if (stamps.size() >= m_limit) {
            return false;
        }
        stamps.push_back(now);
        return true;
    }

   private:
    std::size_t m_limit;
    std::int64_t m_window_ms;
    std::mutex m_mutex;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> m_stamps;
};

/// @brief 服务端共享状态（进程内单例，main 持有）。
struct ServerState {
    explicit ServerState(const ServerOptions& options)
        : limiter(options.rate_limit, options.rate_window_ms),
          max_queue(options.max_queue),
          verbose(options.verbose) {}

    bili::DanmakuStore store;
    bili::RoomHub hub;
    RateLimiter limiter;
    std::size_t max_queue;
    bool verbose;
};

/// @brief JSON 字段解析结果：缺失 / 合法 / 类型非法。
enum class JsonFieldState { kMissing, kOk, kInvalid };

/// @brief 按 UTF-8 计字符数（非续字节计数；非法字节也算一个字符）。
std::size_t Utf8Length(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char byte : text) {
        if ((byte & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

/// @brief 取 JSON 字符串字段；区分「键不存在」与「值不是字符串」。
JsonFieldState JsonString(std::string_view json, std::string_view key,
                          std::string* out) {
    if (bili::JsonStringField(json, key, out)) {
        return JsonFieldState::kOk;
    }
    const std::string needle = "\"" + std::string(key) + "\"";
    return json.find(needle) == std::string_view::npos ? JsonFieldState::kMissing
                                                       : JsonFieldState::kInvalid;
}

/// @brief 取 JSON 整数字段；只接受十进制整数（可带负号），不做浮点。
JsonFieldState JsonInt(std::string_view json, std::string_view key,
                       std::int64_t* out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return JsonFieldState::kMissing;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return JsonFieldState::kInvalid;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    bool negative = false;
    if (pos < json.size() && json[pos] == '-') {
        negative = true;
        ++pos;
    }
    if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') {
        return JsonFieldState::kInvalid;
    }
    std::int64_t value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        const int digit = json[pos] - '0';
        if (value > (INT64_MAX - digit) / 10) {
            return JsonFieldState::kInvalid;
        }
        value = value * 10 + digit;
        ++pos;
    }
    *out = negative ? -value : value;
    return JsonFieldState::kOk;
}

/// @brief 解析 query 里的正整数 id；非法返回 -1。
std::int64_t ParseId(std::string_view text) {
    if (text.empty()) {
        return -1;
    }
    std::int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return -1;
        }
        if (value > (INT64_MAX - (c - '0')) / 10) {
            return -1;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

/// @brief 从 token query 解析用户 id：纯数字或 `u<id>` 视为登录用户，其余=游客 0。
std::int64_t ParseUserToken(std::string_view token) {
    if (token.empty()) {
        return 0;
    }
    if (token.front() == 'u' || token.front() == 'U') {
        token.remove_prefix(1);
    }
    const std::int64_t id = ParseId(token);
    return id < 0 ? 0 : id;
}

/// @brief 读请求头直到空行；body 起点写入 `body_begin`。
bool ReadHttpHead(Socket& socket, std::string* buffer, std::size_t* body_begin) {
    char chunk[4096];
    for (;;) {
        if (bili::FindHeaderEnd(*buffer, body_begin)) {
            return true;
        }
        if (buffer->size() >= bili::kMaxHeaderBytes) {
            return false;
        }
        const auto received =
            socket.recv(chunk, sizeof(chunk), 0, std::chrono::milliseconds(5000));
        if (!received.ok() || received.transferred <= 0) {
            return false;
        }
        buffer->append(chunk, static_cast<std::size_t>(received.transferred));
    }
}

/// @brief 写完整 HTTP 响应。
void SendHttp(Socket& socket, const bili::HttpResponse& response) {
    const std::string wire = bili::Serialize(response);
    std::size_t sent = 0;
    while (sent < wire.size()) {
        const auto result = socket.send(wire.data() + sent, wire.size() - sent,
                                        MSG_NOSIGNAL);
        if (!result.ok() || result.transferred <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(result.transferred);
    }
}

/// @brief 回一个 JSON 错误响应。
void SendHttpError(Socket& socket, int status, std::string_view message) {
    bili::HttpResponse response;
    response.status = status;
    response.body = bili::ErrorBody(status, message);
    SendHttp(socket, response);
}

/// @brief 房间在线数变化广播。
void BroadcastOnline(ServerState& state, std::int64_t video_id) {
    const std::size_t count = state.hub.OnlineCount(video_id);
    const std::string payload =
        "{\"type\":\"online\",\"count\":" + std::to_string(count) + "}";
    state.hub.Broadcast(video_id, payload);
    Log("online video_id=" + std::to_string(video_id) +
        " count=" + std::to_string(count));
}

/// @brief 给单条连接回 error 帧（不断连）。
void SendWsError(const bili::RoomConnection::Ptr& connection, int code,
                 std::string_view message) {
    const std::string payload = "{\"type\":\"error\",\"code\":" +
                                std::to_string(code) + ",\"message\":\"" +
                                bili::JsonEscape(message) + "\"}";
    connection->Enqueue(payload);
    Log("ws error conn=" + std::to_string(connection->id()) +
        " code=" + std::to_string(code) + " message=" + std::string(message));
}

/// @brief 处理一条 `send` 文本帧：校验 → 限流 → 入库 → 广播。
void HandleSend(ServerState& state, const bili::RoomConnection::Ptr& connection,
                std::int64_t video_id, std::int64_t user_id,
                const std::string& payload) {
    const std::string json = bili::Trim(payload);
    if (json.size() < 2 || json.front() != '{' || json.back() != '}') {
        SendWsError(connection, 400, "非法 JSON");
        return;
    }
    std::string type;
    if (JsonString(json, "type", &type) != JsonFieldState::kOk) {
        SendWsError(connection, 400, "缺少 type 字段");
        return;
    }
    if (type != "send") {
        SendWsError(connection, 400, "未知消息类型");
        return;
    }
    std::string text;
    if (JsonString(json, "text", &text) != JsonFieldState::kOk) {
        SendWsError(connection, 400, "缺少 text 字段");
        return;
    }
    const std::size_t characters = Utf8Length(text);
    if (characters < 1 || characters > 100) {
        SendWsError(connection, 400, "弹幕长度需为 1-100 字符");
        return;
    }
    for (const std::string_view blocked : {"广告", "加群"}) {
        if (text.find(blocked) != std::string::npos) {
            SendWsError(connection, 400, "包含屏蔽词");
            return;
        }
    }
    std::int64_t time_ms = 0;
    const JsonFieldState time_state = JsonInt(json, "time_ms", &time_ms);
    if (time_state == JsonFieldState::kInvalid || time_ms < 0) {
        SendWsError(connection, 400, "time_ms 非法");
        return;
    }
    std::int64_t color = 16777215;
    const JsonFieldState color_state = JsonInt(json, "color", &color);
    if (color_state == JsonFieldState::kInvalid || color < 0 || color > 16777215) {
        SendWsError(connection, 400, "color 非法");
        return;
    }

    const std::string rate_key =
        user_id > 0 ? "u:" + std::to_string(user_id)
                    : "c:" + std::to_string(connection->id());
    if (!state.limiter.Allow(rate_key)) {
        SendWsError(connection, 429, "发送过于频繁");
        return;
    }

    bili::DanmakuRow row;
    row.video_id = video_id;
    row.user_id = user_id;
    row.text = text;
    row.time_ms = time_ms;
    row.color = color;
    bili::DanmakuRow stored;
    std::string error;
    if (!state.store.InsertDanmaku(row, &stored, &error)) {
        SendWsError(connection, 500, "写入失败");
        Log("insert failed: " + error);
        return;
    }

    const std::string broadcast =
        "{\"type\":\"danmaku\",\"id\":" + std::to_string(stored.id) +
        ",\"video_id\":" + std::to_string(stored.video_id) +
        ",\"user_id\":" + std::to_string(stored.user_id) +
        ",\"text\":\"" + bili::JsonEscape(stored.text) + "\"" +
        ",\"time_ms\":" + std::to_string(stored.time_ms) +
        ",\"color\":" + std::to_string(stored.color) +
        ",\"created_at\":" + std::to_string(stored.created_at) + "}";
    const std::size_t delivered = state.hub.Broadcast(video_id, broadcast);
    if (state.verbose) {
        Log("danmaku id=" + std::to_string(stored.id) + " video_id=" +
            std::to_string(video_id) + " delivered=" + std::to_string(delivered) +
            " text=" + stored.text);
    }
}

/// @brief 把 bili::HttpRequest 转成 WebSocket 握手用的 go2cpp HTTP 请求。
go2cpp::http::Request BuildUpgradeRequest(const bili::HttpRequest& parsed) {
    go2cpp::http::Request request;
    request.method = go2cpp::http::parse_method(parsed.method);
    request.target = parsed.target;
    request.version = parsed.version;
    for (const auto& entry : parsed.headers) {
        request.header.add(entry.first, entry.second);
    }
    return request;
}

/// @brief 处理 WS 会话：握手、加入房间、读循环、离开。
void HandleWebSocket(Socket::Ptr socket, const bili::HttpRequest& parsed,
                     std::string pending, ServerState& state) {
    auto request = BuildUpgradeRequest(parsed);
    std::string key;
    std::string reason;
    if (!go2cpp::net::WebSocket::ValidateUpgradeRequest(request, key, &reason)) {
        SendHttpError(*socket, 400, reason.empty() ? "非法 WebSocket 握手" : reason);
        return;
    }
    go2cpp::net::WebSocketResult accept_result;
    auto websocket = go2cpp::net::WebSocket::AcceptWithPending(
        socket, request, std::move(pending), go2cpp::net::WebSocketConfig{},
        &accept_result);
    if (!websocket) {
        Log("websocket accept failed: " + accept_result.message);
        return;
    }

    const std::int64_t video_id =
        ParseId(bili::QueryParam(parsed, "video_id"));
    if (video_id < 0) {
        Log("websocket rejected: video_id 非法");
        websocket->close(1008, "video_id 非法");
        return;
    }
    const std::int64_t user_id =
        ParseUserToken(bili::QueryParam(parsed, "token"));
    const std::string peer = "?";

    auto connection = std::make_shared<bili::RoomConnection>(websocket, peer,
                                                             state.max_queue);
    connection->Start();
    state.hub.Join(video_id, connection);
    Log("ws join conn=" + std::to_string(connection->id()) + " video_id=" +
        std::to_string(video_id) + " user_id=" + std::to_string(user_id) +
        " peer=" + peer);
    BroadcastOnline(state, video_id);

    for (;;) {
        if (connection->closed()) {
            break;
        }
        go2cpp::net::WebSocketMessage message;
        const auto read = websocket->read_message(message);
        if (!read.ok()) {
            break;
        }
        if (!message.text()) {
            continue;
        }
        HandleSend(state, connection, video_id, user_id, message.payload);
    }

    state.hub.Leave(video_id, connection);
    connection->Close();
    Log("ws leave conn=" + std::to_string(connection->id()) + " video_id=" +
        std::to_string(video_id) + " sent=" +
        std::to_string(connection->sent()));
    BroadcastOnline(state, video_id);
}

/// @brief 处理 REST：历史弹幕 / 在线数。
void HandleRest(Socket& socket, const bili::HttpRequest& parsed,
                ServerState& state) {
    if (parsed.method != "GET") {
        SendHttpError(socket, 405, "method not allowed");
        return;
    }
    constexpr std::string_view kPrefix = "/api/danmaku/";
    if (parsed.path.size() <= kPrefix.size() ||
        parsed.path.compare(0, kPrefix.size(), kPrefix.data(), kPrefix.size()) !=
            0) {
        SendHttpError(socket, 404, "not found");
        return;
    }
    std::string rest = parsed.path.substr(kPrefix.size());
    bool online = false;
    constexpr std::string_view kOnlineSuffix = "/online";
    if (rest.size() > kOnlineSuffix.size() &&
        rest.compare(rest.size() - kOnlineSuffix.size(), kOnlineSuffix.size(),
                     kOnlineSuffix.data(), kOnlineSuffix.size()) == 0) {
        online = true;
        rest.resize(rest.size() - kOnlineSuffix.size());
    }
    const std::int64_t video_id = ParseId(rest);
    if (video_id < 0) {
        SendHttpError(socket, 400, "video_id 非法");
        return;
    }

    bili::HttpResponse response;
    if (online) {
        const std::size_t count = state.hub.OnlineCount(video_id);
        response.body = "{\"ok\":true,\"video_id\":" + std::to_string(video_id) +
                        ",\"online\":" + std::to_string(count) +
                        ",\"total\":" + std::to_string(state.hub.OnlineTotal()) +
                        "}";
        SendHttp(socket, response);
        return;
    }

    std::int64_t from_ms = -1;
    std::int64_t to_ms = -1;
    const std::string from_text = bili::QueryParam(parsed, "from");
    const std::string to_text = bili::QueryParam(parsed, "to");
    if (!from_text.empty()) {
        from_ms = ParseId(from_text);
        if (from_ms < 0) {
            SendHttpError(socket, 400, "from 非法");
            return;
        }
    }
    if (!to_text.empty()) {
        to_ms = ParseId(to_text);
        if (to_ms < 0) {
            SendHttpError(socket, 400, "to 非法");
            return;
        }
    }
    std::size_t limit = 0;
    const std::string limit_text = bili::QueryParam(parsed, "limit");
    if (!limit_text.empty()) {
        const std::int64_t parsed_limit = ParseId(limit_text);
        if (parsed_limit < 0) {
            SendHttpError(socket, 400, "limit 非法");
            return;
        }
        limit = static_cast<std::size_t>(parsed_limit);
    }

    std::vector<bili::DanmakuRow> rows;
    std::string error;
    if (!state.store.ListDanmaku(video_id, from_ms, to_ms, limit, &rows, &error)) {
        SendHttpError(socket, 500, error.empty() ? "查询失败" : error);
        return;
    }
    std::string body = "{\"ok\":true,\"video_id\":" + std::to_string(video_id) +
                       ",\"count\":" + std::to_string(rows.size()) +
                       ",\"danmaku\":[";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const bili::DanmakuRow& row = rows[i];
        if (i != 0) {
            body += ',';
        }
        body += "{\"id\":" + std::to_string(row.id) +
                ",\"video_id\":" + std::to_string(row.video_id) +
                ",\"user_id\":" + std::to_string(row.user_id) +
                ",\"text\":\"" + bili::JsonEscape(row.text) + "\"" +
                ",\"time_ms\":" + std::to_string(row.time_ms) +
                ",\"color\":" + std::to_string(row.color) +
                ",\"created_at\":" + std::to_string(row.created_at) + "}";
    }
    body += "]}";
    response.body = std::move(body);
    SendHttp(socket, response);
}

/// @brief 单连接入口：读头 → 分派 WS / REST。
void HandleConnection(Socket::Ptr socket, ServerState& state) {
    std::string buffer;
    std::size_t body_begin = 0;
    if (!ReadHttpHead(*socket, &buffer, &body_begin)) {
        Log("connection dropped: 读请求头失败/超时");
        return;
    }
    bili::HttpRequest parsed;
    if (!bili::ParseHttpRequest(buffer, body_begin, &parsed)) {
        SendHttpError(*socket, 400, "非法 HTTP 请求");
        return;
    }
    std::string upgrade = parsed.Header("upgrade");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (upgrade == "websocket") {
        HandleWebSocket(std::move(socket), parsed, buffer.substr(body_begin), state);
        return;
    }
    HandleRest(*socket, parsed, state);
}

/// @brief 命令行帮助。
void PrintUsage(const char* program) {
    std::fprintf(stderr,
                 "用法: %s [--port N] [--db PATH] [--rate-limit N] "
                 "[--rate-window-ms N] [--max-queue N] [--verbose]\n",
                 program);
}

}  // namespace

int main(int argc, char** argv) {
    ServerOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %s 缺少值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--port") {
            const long port = std::strtol(value("--port").c_str(), nullptr, 10);
            if (port <= 0 || port > 65535) {
                std::fprintf(stderr, "端口非法: %ld\n", port);
                return 2;
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (arg == "--db") {
            options.db_path = value("--db");
        } else if (arg == "--rate-limit") {
            const long limit = std::strtol(value("--rate-limit").c_str(), nullptr, 10);
            if (limit < 0) {
                std::fprintf(stderr, "rate-limit 非法: %ld\n", limit);
                return 2;
            }
            options.rate_limit = static_cast<std::size_t>(limit);
        } else if (arg == "--rate-window-ms") {
            const long window =
                std::strtol(value("--rate-window-ms").c_str(), nullptr, 10);
            if (window <= 0) {
                std::fprintf(stderr, "rate-window-ms 非法: %ld\n", window);
                return 2;
            }
            options.rate_window_ms = window;
        } else if (arg == "--max-queue") {
            const long queue = std::strtol(value("--max-queue").c_str(), nullptr, 10);
            if (queue <= 0) {
                std::fprintf(stderr, "max-queue 非法: %ld\n", queue);
                return 2;
            }
            options.max_queue = static_cast<std::size_t>(queue);
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    ServerState state(options);
    std::string error;
    if (!state.store.Open(options.db_path, &error)) {
        std::fprintf(stderr, "打开数据库失败: %s\n", error.c_str());
        return 1;
    }
    if (!state.store.EnsureSchema(&error)) {
        std::fprintf(stderr, "建表失败: %s\n", error.c_str());
        return 1;
    }

    auto listener = go2cpp::net::ListenTCP("0.0.0.0", options.port);
    if (!listener) {
        std::fprintf(stderr, "监听 0.0.0.0:%u 失败\n",
                     static_cast<unsigned>(options.port));
        return 1;
    }
    Log("danmaku_service listening on 0.0.0.0:" +
        std::to_string(options.port) + " db=" + options.db_path +
        " rate_limit=" + std::to_string(options.rate_limit) + "/" +
        std::to_string(options.rate_window_ms) + "ms");

    for (;;) {
        auto socket = listener->accept(std::chrono::milliseconds(500));
        if (!socket) {
            continue;
        }
        std::thread([socket, &state] { HandleConnection(socket, state); }).detach();
    }
    return 0;
}
