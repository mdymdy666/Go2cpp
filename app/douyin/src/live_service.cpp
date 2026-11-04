/**
 * @file live_service.cpp
 * @brief 抖音 C7 live 服务（29105）：建房 / 开播 / 关播 / 列表 / 进出房。
 *
 * @details 路由（除 /health、/metrics 外都要 Bearer）：
 *          - POST /api/live/room                 创建直播间（status=0）
 *          - POST /api/live/room/:id/start       开播（仅房主；非房主 403、
 *                                                不存在 404）
 *          - POST /api/live/room/:id/stop        关播（权限同 start）
 *          - GET  /api/live/rooms?status=        列表（status=1 只列直播中）
 *          - POST /api/live/room/:id/enter       进房（幂等，观众数 +1）
 *          - POST /api/live/room/:id/leave       出房（幂等，观众数 -1，不为负）
 *          - GET  /health, /metrics
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_http.hpp"
#include "douyin_auth.hpp"
#include "douyin_store.hpp"
#include "go2cpp/storage/db.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29105;
constexpr const char* kServiceName = "live";
constexpr std::size_t kMaxTitleBytes = 256;

/// @brief 单线程事件循环上的计数器（无需原子）。
struct ServiceMetrics {
    std::uint64_t requests{0};
    std::uint64_t errors{0};
};

ServiceMetrics g_metrics;

/// @brief 当前 Unix 秒。
std::int64_t NowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// @brief 统一错误响应。
bool Fail(douyin::HttpResponse* response, int status, std::string_view message) {
    response->status = status;
    response->body = douyin::ErrorBody(status, message);
    ++g_metrics.errors;
    return true;
}

/// @brief 解析并校验 Bearer token。
bool RequireToken(const douyin::HttpRequest& request, douyin::HttpResponse* response,
                  std::int64_t* user_id) {
    const std::string token = douyin::BearerToken(request);
    if (token.empty()) {
        Fail(response, 401, "缺少 Bearer token");
        return false;
    }
    douyin::TokenInfo info;
    std::string error;
    if (!douyin::VerifyToken(token, NowSeconds(), &info, &error)) {
        Fail(response, 401, error.empty() ? "token 无效" : error);
        return false;
    }
    *user_id = info.user_id;
    return true;
}

/// @brief 房间对象的 JSON（列顺序与存储行一致，便于测试解析）。
std::string RoomJson(const douyin::StoredRoom& room) {
    return "{\"id\":" + std::to_string(room.id) +
           ",\"user_id\":" + std::to_string(room.user_id) +
           ",\"title\":\"" + douyin::JsonEscape(room.title) + "\"" +
           ",\"status\":" + std::to_string(room.status) +
           ",\"viewer_count\":" + std::to_string(room.viewer_count) +
           ",\"created_at\":" + std::to_string(room.created_at) + "}";
}

/// @brief `/api/live/room[/<id>[/<action>]]` 的解析结果。
enum class RoomAction {
    kNone,        ///< 不匹配。
    kCollection,  ///< `/api/live/room`（只有 POST）。
    kStart,       ///< `/api/live/room/<id>/start`。
    kStop,        ///< `/api/live/room/<id>/stop`。
    kEnter,       ///< `/api/live/room/<id>/enter`。
    kLeave,       ///< `/api/live/room/<id>/leave`。
};

/// @brief 解析房间路径；成功时填 id（仅 :id 形式）。
RoomAction ParseRoomPath(const std::string& path, std::int64_t* id) {
    constexpr std::string_view kBase = "/api/live/room";
    if (path == kBase) {
        return RoomAction::kCollection;
    }
    if (path.size() <= kBase.size() + 1 ||
        path.compare(0, kBase.size(), kBase.data(), kBase.size()) != 0 ||
        path[kBase.size()] != '/') {
        return RoomAction::kNone;
    }
    const std::string_view rest(path.data() + kBase.size() + 1,
                                path.size() - kBase.size() - 1);
    const std::size_t slash = rest.find('/');
    const std::string_view digits = rest.substr(0, slash);
    if (digits.empty() || digits.size() > 18) {
        return RoomAction::kNone;
    }
    std::int64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return RoomAction::kNone;
        }
        value = value * 10 + (c - '0');
    }
    if (value <= 0) {
        return RoomAction::kNone;
    }
    *id = value;
    if (slash == std::string_view::npos) {
        return RoomAction::kNone;
    }
    const std::string_view action = rest.substr(slash + 1);
    if (action == "start") {
        return RoomAction::kStart;
    }
    if (action == "stop") {
        return RoomAction::kStop;
    }
    if (action == "enter") {
        return RoomAction::kEnter;
    }
    if (action == "leave") {
        return RoomAction::kLeave;
    }
    return RoomAction::kNone;
}

/// @brief 创建直播间：status=0、viewer_count=0。
bool HandleCreate(douyin::Store& store, const douyin::HttpRequest& request,
                  douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::string title;
    if (!douyin::JsonStringField(request.body, "title", &title) ||
        title.empty()) {
        return Fail(response, 400, "title 不能为空");
    }
    if (title.size() > kMaxTitleBytes) {
        return Fail(response, 400, "title 过长");
    }

    std::string error;
    const std::int64_t id = store.CreateRoom(actor, title, &error);
    if (id <= 0) {
        std::fprintf(stderr, "[%s] 创建房间失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    douyin::StoredRoom room;
    room.id = id;
    room.user_id = actor;
    room.title = title;
    room.status = 0;
    room.viewer_count = 0;
    room.created_at = NowSeconds();
    response->status = 200;
    response->body = "{\"ok\":true,\"room\":" + RoomJson(room) + "}";
    return true;
}

/// @brief 开播 / 关播：房间必须存在，且只有房主能改。
bool HandleStatus(douyin::Store& store, const douyin::HttpRequest& request,
                  std::int64_t id, int status, douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    douyin::StoredRoom room;
    std::string error;
    if (!store.FindRoom(id, &room, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询房间失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "房间不存在");
    }
    if (room.user_id != actor) {
        return Fail(response, 403, "只有房主可以开播/关播");
    }
    if (store.UpdateRoomStatus(id, actor, status, &error) < 0) {
        std::fprintf(stderr, "[%s] 更新房间状态失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // 重新读一次，拿到最新 viewer_count（避免与并发进出房不一致）。
    douyin::StoredRoom latest;
    if (store.FindRoom(id, &latest, &error)) {
        room = latest;
    }
    response->status = 200;
    response->body = "{\"ok\":true,\"room\":" + RoomJson(room) + "}";
    return true;
}

/// @brief 进房 / 出房：幂等，返回本次是否真的改变了观众数。
bool HandlePresence(douyin::Store& store, const douyin::HttpRequest& request,
                    std::int64_t id, bool enter,
                    douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    douyin::StoredRoom room;
    std::string error;
    if (!store.FindRoom(id, &room, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询房间失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "房间不存在");
    }

    const int changed = enter ? store.EnterRoom(id, actor, &error)
                              : store.LeaveRoom(id, actor, &error);
    if (changed < 0) {
        std::fprintf(stderr, "[%s] %s失败: %s\n", kServiceName,
                     enter ? "进房" : "出房", error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    douyin::StoredRoom latest;
    if (store.FindRoom(id, &latest, &error)) {
        room = latest;
    }
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"room_id\":") +
                     std::to_string(id) +
                     ",\"changed\":" + (changed == 1 ? "true" : "false") +
                     ",\"viewer_count\":" + std::to_string(room.viewer_count) +
                     "}";
    return true;
}

/// @brief 房间列表；`status` 缺省不限，0/1 过滤。
bool HandleList(douyin::Store& store, const douyin::HttpRequest& request,
                douyin::HttpResponse* response) {
    int status = -1;
    const std::string text = douyin::QueryParam(request, "status");
    if (!text.empty()) {
        if (text == "0") {
            status = 0;
        } else if (text == "1") {
            status = 1;
        } else {
            return Fail(response, 400, "status 只能是 0 或 1");
        }
    }

    std::vector<douyin::StoredRoom> rooms;
    std::string error;
    if (!store.ListRooms(status, &rooms, &error)) {
        std::fprintf(stderr, "[%s] 查询房间列表失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    std::string body = "{\"ok\":true,\"count\":" + std::to_string(rooms.size()) +
                       ",\"items\":[";
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"room\":" + RoomJson(rooms[i]) + "}";
    }
    body += "]}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief 总路由。
bool Handle(douyin::Store& store, const douyin::HttpRequest& request,
            douyin::HttpResponse* response) {
    ++g_metrics.requests;
    if (request.method == "GET" && request.path == "/health") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") + kServiceName + "\"}";
        return true;
    }
    if (request.method == "GET" && request.path == "/metrics") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") + kServiceName +
                         "\",\"requests\":" + std::to_string(g_metrics.requests) +
                         ",\"errors\":" + std::to_string(g_metrics.errors) + "}";
        return true;
    }

    if (request.path == "/api/live/rooms") {
        if (request.method == "GET") {
            return HandleList(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    std::int64_t id = 0;
    const RoomAction action = ParseRoomPath(request.path, &id);
    if (action == RoomAction::kNone) {
        return false;
    }
    if (action == RoomAction::kCollection) {
        if (request.method == "POST") {
            return HandleCreate(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.method != "POST") {
        return Fail(response, 405, "方法不允许");
    }
    if (action == RoomAction::kStart) {
        return HandleStatus(store, request, id, 1, response);
    }
    if (action == RoomAction::kStop) {
        return HandleStatus(store, request, id, 0, response);
    }
    if (action == RoomAction::kEnter) {
        return HandlePresence(store, request, id, true, response);
    }
    if (action == RoomAction::kLeave) {
        return HandlePresence(store, request, id, false, response);
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = kDefaultPort;
    if (argc > 1) {
        port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    }

    std::string db_path = douyin::Store::kDefaultPath;
    if (const char* env = std::getenv("DOUYIN_DB"); env != nullptr && *env != '\0') {
        db_path = env;
    }
    {
        const std::filesystem::path parent = std::filesystem::path(db_path).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
    }

    douyin::Store store;
    std::string error;
    if (!store.Open(db_path, &error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }
    if (!store.EnsureSchema(&error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }

    douyin::HttpServer server(kServiceName, port);
    if (!server.Start(&error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }
    // 就绪探针：核心表可读才算 ready；只读，不改任何数据。
    // 表被删/连接失效时返回 false，`/health/ready` 回 503。
    server.SetReadinessProbe([&store](std::string* detail) {
        if (!store.Ok() || store.Database() == nullptr) {
            if (detail != nullptr) {
                *detail = "db not open";
            }
            return false;
        }
        auto rows = store.Database()->Query("SELECT 1 FROM rooms LIMIT 1");
        if (!rows) {
            if (detail != nullptr) {
                *detail = "db prepare failed";
            }
            return false;
        }
        (void)rows.Value().Next();
        if (rows.Value().Err()) {
            if (detail != nullptr) {
                *detail = "db query failed";
            }
            return false;
        }
        return true;
    });
    std::printf("[%s] 监听 0.0.0.0:%u db=%s\n", kServiceName,
                static_cast<unsigned>(port), db_path.c_str());
    std::fflush(stdout);

    server.ServeForever([&store](const douyin::HttpRequest& request,
                                 douyin::HttpResponse* response) {
        return Handle(store, request, response);
    });
    return 0;
}
