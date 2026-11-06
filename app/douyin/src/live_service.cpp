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
 *          - GET  /api/live/room/:id/messages   聊天增量（after 游标）
 *          - POST /api/live/room/:id/messages   发聊天（需登录、拉黑过滤）
 *          - GET  /api/live/room/:id/ranking    送礼榜（GROUP BY 单条 SQL）
 *          - POST /api/live/room/:id/gift       送礼（事务扣余额）
 *          - GET  /api/live/gifts               礼物价目
 *          - GET  /api/live/wallet              当前用户余额（需登录）
 *          - 以上互动路由同时接受 /api/live/<id>/... 短形式
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
    kMessages,    ///< `/api/live/<id>/messages`（GET 增量 / POST 发送）。
    kGift,        ///< `/api/live/<id>/gift`（POST 送礼）。
    kRanking,     ///< `/api/live/<id>/ranking`（GET 贡献榜）。
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
    if (action == "messages") {
        return RoomAction::kMessages;
    }
    if (action == "gift") {
        return RoomAction::kGift;
    }
    if (action == "ranking") {
        return RoomAction::kRanking;
    }
    return RoomAction::kNone;
}

/// @brief 解析短形式 `/api/live/<id>/<action>`（互动三件套）。
/// @param path [in] 请求路径（不含 query）。
/// @param id [out] 成功时写房间 id；失败保持原值。
/// @param action [out] 成功时写动作；失败保持原值。
/// @return 命中 `messages` / `gift` / `ranking` 返回 true。
bool ParseLiveScopedPath(const std::string& path, std::int64_t* id,
                         RoomAction* action) {
    constexpr std::string_view kBase = "/api/live/";
    if (path.size() <= kBase.size() + 2) {
        return false;
    }
    if (path.compare(0, kBase.size(), kBase.data(), kBase.size()) != 0) {
        return false;
    }
    const std::string_view rest(path.data() + kBase.size(),
                                path.size() - kBase.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    const std::string_view digits = rest.substr(0, slash);
    if (digits.empty() || digits.size() > 18) {
        return false;
    }
    std::int64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value <= 0) {
        return false;
    }
    const std::string_view tail = rest.substr(slash + 1);
    if (tail == "messages") {
        *action = RoomAction::kMessages;
    } else if (tail == "gift") {
        *action = RoomAction::kGift;
    } else if (tail == "ranking") {
        *action = RoomAction::kRanking;
    } else {
        return false;
    }
    *id = value;
    return true;
}


/// @brief UTF-8 码点个数（按首字节计；非法字节各算一个）。
std::size_t Utf8Length(const std::string& text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

/// @brief 是否全为空白（只发空格/制表/换行按空内容处理）。
bool IsBlank(const std::string& text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return false;
        }
    }
    return true;
}

/// @brief 从 JSON 文本取整数字段（只支持本项目固定结构）。
bool JsonIntField(std::string_view body, std::string_view key,
                  std::int64_t* out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos = body.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return false;
    }
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) {
        ++pos;
    }
    if (pos >= body.size()) {
        return false;
    }
    const std::string rest(body.substr(pos));
    char* end = nullptr;
    const long long value = std::strtoll(rest.c_str(), &end, 10);
    if (end == rest.c_str()) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

/// @brief 读一个非负整数 query 参数；缺省返回 fallback，非法返回 false。
bool ReadQueryInt(const douyin::HttpRequest& request, const char* name,
                  std::int64_t fallback, std::int64_t* out) {
    const std::string text = douyin::QueryParam(request, name);
    if (text.empty()) {
        *out = fallback;
        return true;
    }
    if (text.size() > 18) {
        return false;
    }
    std::int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    *out = value;
    return true;
}

/// @brief 聊天/礼物消息的 JSON（列顺序与验收脚本解析一致）。
std::string MessageJson(const douyin::StoredLiveMessage& message) {
    return "{\"id\":" + std::to_string(message.id) +
           ",\"room_id\":" + std::to_string(message.room_id) +
           ",\"user_id\":" + std::to_string(message.user_id) +
           ",\"user_name\":\"" + douyin::JsonEscape(message.user_name) + "\"" +
           ",\"content\":\"" + douyin::JsonEscape(message.content) + "\"" +
           ",\"kind\":\"" + douyin::JsonEscape(message.kind) + "\"" +
           ",\"created_at\":" + std::to_string(message.created_at) + "}";
}

/// @brief 礼物对象的 JSON。
std::string GiftJson(const douyin::StoredGift& gift) {
    return "{\"id\":" + std::to_string(gift.id) + ",\"name\":\"" +
           douyin::JsonEscape(gift.name) +
           "\",\"price_coins\":" + std::to_string(gift.price_coins) +
           ",\"icon\":\"" + douyin::JsonEscape(gift.icon) + "\"}";
}

/// @brief 贡献榜一行的 JSON。
std::string RankJson(const douyin::StoredGiftRank& rank) {
    return "{\"user_id\":" + std::to_string(rank.user_id) +
           ",\"user_name\":\"" + douyin::JsonEscape(rank.user_name) +
           "\",\"coins\":" + std::to_string(rank.coins) +
           ",\"gift_count\":" + std::to_string(rank.gift_count) + "}";
}

/// @brief 双向拉黑校验：被房主拉黑或自己拉黑房主都不能发消息/送礼。
/// @return 可以继续返回 true；已写 403/500 响应返回 false。
bool CheckNotBlocked(douyin::Store& store, std::int64_t owner_id,
                     std::int64_t actor_id, douyin::HttpResponse* response) {
    bool blocked = false;
    std::string error;
    if (!store.IsBlocked(owner_id, actor_id, &blocked, &error)) {
        std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                     error.c_str());
        Fail(response, 500, "数据库错误");
        return false;
    }
    if (blocked) {
        Fail(response, 403, "对方已把你拉黑");
        return false;
    }
    if (!store.IsBlocked(actor_id, owner_id, &blocked, &error)) {
        std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                     error.c_str());
        Fail(response, 500, "数据库错误");
        return false;
    }
    if (blocked) {
        Fail(response, 403, "你已拉黑对方，请先取消拉黑");
        return false;
    }
    return true;
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

/// @brief POST /api/live/<id>/messages：发一条聊天（1~200 字）。
bool HandleSendMessage(douyin::Store& store, const douyin::HttpRequest& request,
                       std::int64_t id, douyin::HttpResponse* response) {
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
    std::string content;
    if (!douyin::JsonStringField(request.body, "content", &content) ||
        content.empty() || IsBlank(content)) {
        return Fail(response, 400, "消息内容不能为空");
    }
    if (Utf8Length(content) > 200) {
        return Fail(response, 400, "消息不能超过 200 字");
    }
    if (!CheckNotBlocked(store, room.user_id, actor, response)) {
        return true;
    }
    const std::int64_t mid = store.AppendLiveMessage(id, actor, content, "chat",
                                                     &error);
    if (mid <= 0) {
        std::fprintf(stderr, "[%s] 写直播消息失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    douyin::StoredUser user;
    if (!store.FindUserById(actor, &user, &error)) {
        std::fprintf(stderr, "[%s] 查询用户失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    douyin::StoredLiveMessage message;
    message.id = mid;
    message.room_id = id;
    message.user_id = actor;
    message.user_name = user.name;
    message.content = content;
    message.kind = "chat";
    message.created_at = NowSeconds();
    response->status = 200;
    response->body = "{\"ok\":true,\"message\":" + MessageJson(message) + "}";
    return true;
}

/// @brief GET /api/live/<id>/messages?after=&limit=：增量拉取（升序）。
bool HandleListMessages(douyin::Store& store, const douyin::HttpRequest& request,
                        std::int64_t id, douyin::HttpResponse* response) {
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
    std::int64_t after = 0;
    if (!ReadQueryInt(request, "after", 0, &after) || after < 0) {
        return Fail(response, 400, "after 非法");
    }
    std::int64_t limit = 50;
    if (!ReadQueryInt(request, "limit", 50, &limit) || limit < 1 || limit > 100) {
        return Fail(response, 400, "limit 必须是 1~100");
    }
    std::vector<douyin::StoredLiveMessage> messages;
    if (!store.ListLiveMessages(id, after, limit, room.user_id, &messages,
                                &error)) {
        std::fprintf(stderr, "[%s] 查询直播消息失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    std::int64_t last_id = after;
    std::string body = "{\"ok\":true,\"count\":" +
                       std::to_string(messages.size()) +
                       ",\"items\":[";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += MessageJson(messages[i]);
        last_id = messages[i].id;
    }
    body += "],\"last_id\":" + std::to_string(last_id) + "}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief GET /api/live/gifts：礼物价目表（公开）。
bool HandleGifts(douyin::Store& store, douyin::HttpResponse* response) {
    std::vector<douyin::StoredGift> gifts;
    std::string error;
    if (!store.ListGifts(&gifts, &error)) {
        std::fprintf(stderr, "[%s] 查询礼物失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    std::string body = "{\"ok\":true,\"count\":" + std::to_string(gifts.size()) +
                       ",\"items\":[";
    for (std::size_t i = 0; i < gifts.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += GiftJson(gifts[i]);
    }
    body += "]}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief GET /api/live/wallet：当前用户金币余额（需登录）。
bool HandleWallet(douyin::Store& store, const douyin::HttpRequest& request,
                  douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::int64_t coins = 0;
    std::string error;
    if (!store.FindUserCoins(actor, &coins, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询余额失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "用户不存在");
    }
    response->status = 200;
    response->body = "{\"ok\":true,\"coins\":" + std::to_string(coins) + "}";
    return true;
}

/// @brief POST /api/live/<id>/gift：事务扣余额 + 写记录 + 聊天区特效消息。
bool HandleGift(douyin::Store& store, const douyin::HttpRequest& request,
                std::int64_t id, douyin::HttpResponse* response) {
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
    std::int64_t gift_id = 0;
    if (!JsonIntField(request.body, "gift_id", &gift_id) || gift_id <= 0) {
        return Fail(response, 400, "gift_id 非法");
    }
    std::int64_t count = 1;
    if (request.body.find("\"count\"") != std::string::npos) {
        if (!JsonIntField(request.body, "count", &count) || count < 1 ||
            count > 99) {
            return Fail(response, 400, "count 必须是 1~99");
        }
    }
    douyin::StoredGift gift;
    if (!store.FindGift(gift_id, &gift, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询礼物失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "礼物不存在");
    }
    if (!CheckNotBlocked(store, room.user_id, actor, response)) {
        return true;
    }
    douyin::StoredUser user;
    if (!store.FindUserById(actor, &user, &error)) {
        std::fprintf(stderr, "[%s] 查询用户失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    const std::string content = "送出 " + gift.name + " ×" +
                                std::to_string(count);
    std::int64_t coins = 0;
    std::int64_t message_id = 0;
    const int result = store.SendGift(id, actor, gift, count, content, &coins,
                                      &message_id, &error);
    if (result == 0) {
        return Fail(response, 400, "余额不足");
    }
    if (result < 0) {
        std::fprintf(stderr, "[%s] 送礼失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    douyin::StoredLiveMessage message;
    message.id = message_id;
    message.room_id = id;
    message.user_id = actor;
    message.user_name = user.name;
    message.content = content;
    message.kind = "gift";
    message.created_at = NowSeconds();
    response->status = 200;
    response->body = "{\"ok\":true,\"gift\":" + GiftJson(gift) +
                     ",\"count\":" + std::to_string(count) +
                     ",\"coins\":" + std::to_string(coins) +
                     ",\"message\":" + MessageJson(message) + "}";
    return true;
}

/// @brief GET /api/live/<id>/ranking?limit=：贡献榜（单条 SQL 聚合）。
bool HandleRanking(douyin::Store& store, const douyin::HttpRequest& request,
                   std::int64_t id, douyin::HttpResponse* response) {
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
    std::int64_t limit = 10;
    if (!ReadQueryInt(request, "limit", 10, &limit) || limit < 1 || limit > 50) {
        return Fail(response, 400, "limit 必须是 1~50");
    }
    std::vector<douyin::StoredGiftRank> ranks;
    if (!store.ListGiftRanking(id, limit, &ranks, &error)) {
        std::fprintf(stderr, "[%s] 查询贡献榜失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    std::string body = "{\"ok\":true,\"count\":" + std::to_string(ranks.size()) +
                       ",\"items\":[";
    for (std::size_t i = 0; i < ranks.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += RankJson(ranks[i]);
    }
    body += "]}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief 总路由。
bool Handle(douyin::Store& store, const douyin::HttpRequest& request,
            douyin::HttpResponse* response) {
    // 每个请求开始前清 ORM 读游标：ORM 语句缓存会把连接钉在上一个请求的
    // 读快照上，导致跨进程拉黑/取消拉黑后判定延后一拍（403/200 反转），
    // 写操作还会报 SQLITE_BUSY_SNAPSHOT。live 是单线程事件循环，代价可忽略。
    store.ResetStatements();
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
    if (request.path == "/api/live/gifts") {
        if (request.method == "GET") {
            return HandleGifts(store, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/live/wallet") {
        if (request.method == "GET") {
            return HandleWallet(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    std::int64_t id = 0;
    RoomAction action = ParseRoomPath(request.path, &id);
    if (action == RoomAction::kNone &&
        !ParseLiveScopedPath(request.path, &id, &action)) {
        return false;
    }
    if (action == RoomAction::kCollection) {
        if (request.method == "POST") {
            return HandleCreate(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (action == RoomAction::kMessages) {
        if (request.method == "POST") {
            return HandleSendMessage(store, request, id, response);
        }
        if (request.method == "GET") {
            return HandleListMessages(store, request, id, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (action == RoomAction::kGift) {
        if (request.method == "POST") {
            return HandleGift(store, request, id, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (action == RoomAction::kRanking) {
        if (request.method == "GET") {
            return HandleRanking(store, request, id, response);
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
