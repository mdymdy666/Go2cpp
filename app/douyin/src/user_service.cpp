/**
 * @file user_service.cpp
 * @brief 抖音 P0 user 服务（29101）：注册 / 登录 / 资料 / 关注 / 粉丝列表。
 *
 * @details 路由：
 *          - POST   /api/user/register         注册（重复 email → 409）
 *          - POST   /api/user/login            登录（错误 → 401）
 *          - GET    /api/user/:id              资料（不含 password_hash/salt）
 *          - POST   /api/user/:id/follow       关注（需 Bearer token，幂等）
 *          - DELETE /api/user/:id/follow       取关（需 Bearer token，幂等）
 *          - GET    /api/user/:id/following    关注列表（分页）
 *          - GET    /api/user/:id/followers    粉丝列表（分页）
 *          - GET    /health, /metrics
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_auth.hpp"
#include "douyin_http.hpp"
#include "douyin_proto.hpp"
#include "douyin_store.hpp"
#include "go2cpp/storage/db.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29101;
constexpr const char* kServiceName = "user";
constexpr std::size_t kMaxNameBytes = 64;
constexpr std::size_t kMaxEmailBytes = 128;
constexpr std::size_t kMaxPasswordBytes = 128;
constexpr std::int64_t kDefaultPageSize = 20;
constexpr std::int64_t kMaxPageSize = 100;

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

/// @brief 把 StoredUser 转成对外 User（不含密码散列/盐）。
douyin::User PublicUser(const douyin::StoredUser& stored) {
    douyin::User user;
    user.id = stored.id;
    user.name = stored.name;
    user.email = stored.email;
    user.created_at = stored.created_at;
    return user;
}

/// @brief 注册处理。
bool HandleRegister(douyin::Store& store, const douyin::HttpRequest& request,
                    douyin::HttpResponse* response) {
    std::string name;
    std::string email;
    std::string password;
    if (!douyin::JsonStringField(request.body, "name", &name) ||
        !douyin::JsonStringField(request.body, "email", &email) ||
        !douyin::JsonStringField(request.body, "password", &password)) {
        return Fail(response, 400, "name/email/password 不能为空");
    }
    if (name.empty() || email.empty() || password.empty()) {
        return Fail(response, 400, "name/email/password 不能为空");
    }
    if (name.size() > kMaxNameBytes || email.size() > kMaxEmailBytes ||
        password.size() > kMaxPasswordBytes) {
        return Fail(response, 400, "字段过长");
    }
    const bool email_shape_ok =
        email.find('@') != std::string::npos && email.find(' ') == std::string::npos;
    if (!email_shape_ok) {
        return Fail(response, 400, "email 格式非法");
    }

    douyin::StoredUser existing;
    std::string error;
    if (store.FindUserByEmail(email, &existing, &error)) {
        return Fail(response, 409, "email 已注册");
    }
    if (!error.empty()) {
        std::fprintf(stderr, "[%s] 查询 email 失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    const std::string salt = douyin::RandomSaltHex();
    const std::string hash = douyin::HashPassword(password, salt);
    douyin::StoredUser created;
    // 写前清空 ORM 语句缓存：FindUserByEmail 提前结束会残留读游标，
    // 多服务共享 WAL 库时会让本进程的写触发 SQLITE_BUSY_SNAPSHOT。
    store.ResetStatements();
    const douyin::CreateUserResult result =
        store.CreateUser(name, email, hash, salt, &created, &error);
    if (result == douyin::CreateUserResult::kConflict) {
        return Fail(response, 409, "email 已注册");
    }
    if (result != douyin::CreateUserResult::kOk) {
        std::fprintf(stderr, "[%s] 创建用户失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    const std::string token = douyin::IssueToken(created.id, NowSeconds());
    response->status = 200;
    response->body = "{\"ok\":true,\"user\":" + douyin::ToJson(PublicUser(created)) +
                     ",\"token\":\"" + token + "\"}";
    return true;
}

/// @brief 登录处理。
bool HandleLogin(douyin::Store& store, const douyin::HttpRequest& request,
                 douyin::HttpResponse* response) {
    std::string email;
    std::string password;
    if (!douyin::JsonStringField(request.body, "email", &email) ||
        !douyin::JsonStringField(request.body, "password", &password) ||
        email.empty() || password.empty()) {
        return Fail(response, 400, "email/password 不能为空");
    }

    douyin::StoredUser stored;
    std::string error;
    const bool found = store.FindUserByEmail(email, &stored, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "[%s] 查询用户失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (!found || !douyin::VerifyPassword(password, stored.salt, stored.password_hash)) {
        return Fail(response, 401, "email 或密码错误");
    }

    const std::string token = douyin::IssueToken(stored.id, NowSeconds());
    response->status = 200;
    response->body = "{\"ok\":true,\"token\":\"" + token +
                     "\",\"user\":" + douyin::ToJson(PublicUser(stored)) + "}";
    return true;
}

/**
 * @brief 尝试解析可选的 Bearer token，用于「当前查看者」相关字段。
 * @param request [in] 原始请求；Authorization 头可为空。
 * @return 有效 token 的 user_id；无 token / 签名非法 / 过期一律返回 0（匿名）。
 * @note 资料页是公开接口，token 无效**不应**回 401；这里只影响 is_blocked
 *       等「查看者视角」字段。
 */
std::int64_t ViewerId(const douyin::HttpRequest& request) {
    const std::string token = douyin::BearerToken(request);
    if (token.empty()) {
        return 0;
    }
    douyin::TokenInfo info;
    std::string error;
    if (!douyin::VerifyToken(token, NowSeconds(), &info, &error)) {
        return 0;
    }
    return info.user_id;
}

/// @brief 取资料（含关注数/粉丝数/当前登录者的关注状态）。
bool HandleProfile(douyin::Store& store, const douyin::HttpRequest& request,
                   std::int64_t id, douyin::HttpResponse* response) {
    douyin::StoredUser stored;
    std::string error;
    const bool found = store.FindUserById(id, &stored, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "[%s] 查询用户失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (!found) {
        return Fail(response, 404, "用户不存在");
    }
    std::int64_t followers = 0;
    std::int64_t following = 0;
    if (!store.FollowCounts(id, &followers, &following, &error)) {
        std::fprintf(stderr, "[%s] 统计关注失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // 未登录也能看资料：token 缺失或非法时 is_following 一律 false。
    bool is_following = false;
    const std::string token = douyin::BearerToken(request);
    if (!token.empty()) {
        douyin::TokenInfo info;
        std::string token_error;
        if (douyin::VerifyToken(token, NowSeconds(), &info, &token_error) &&
            info.user_id != id &&
            !store.IsFollowing(info.user_id, id, &is_following, &error)) {
            std::fprintf(stderr, "[%s] 查询关注关系失败: %s\n", kServiceName,
                         error.c_str());
            is_following = false;
        }
    }
    // 拉黑状态：只有登录用户看别人主页时才有意义；自己/匿名恒 false。
    // 前端据此显示「已拉黑」并隐藏作品，避免拉黑后还能从主页绕进去。
    bool is_blocked = false;
    const std::int64_t viewer = ViewerId(request);
    if (viewer > 0 && viewer != id) {
        if (!store.IsBlocked(viewer, id, &is_blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            is_blocked = false;
        }
    }
    std::string user_json = douyin::ToJson(PublicUser(stored));
    user_json.pop_back();  // 去掉结尾 '}'，追加计数字段
    user_json += ",\"followers\":" + std::to_string(followers) +
                 ",\"following\":" + std::to_string(following) +
                 ",\"is_blocked\":" + (is_blocked ? "true" : "false") + "}";
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"is_following\":") +
                     (is_following ? "true" : "false") + ",\"user\":" +
                     user_json + "}";
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

/// @brief 关注 / 取关。
bool HandleFollow(douyin::Store& store, const douyin::HttpRequest& request,
                  std::int64_t target_id, bool follow, douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    if (actor == target_id) {
        return Fail(response, 400, "不能关注自己");
    }
    douyin::StoredUser target;
    std::string error;
    const bool found = store.FindUserById(target_id, &target, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "[%s] 查询用户失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (!found) {
        return Fail(response, 404, "用户不存在");
    }
    // 防骚扰：被对方拉黑后不能再关注（否则拉黑只是单方面看不见，对方仍
    // 会收到关注通知）。
    if (follow) {
        bool blocked = false;
        if (!store.IsBlocked(target_id, actor, &blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (blocked) {
            return Fail(response, 403, "对方已把你拉黑");
        }
        if (!store.IsBlocked(actor, target_id, &blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (blocked) {
            return Fail(response, 403, "你已拉黑对方，请先取消拉黑");
        }
    }
    // 写前清空 ORM 语句缓存：FindUserById 提前结束会残留读游标，
    // 多服务共享 WAL 库时会让本进程的写触发 SQLITE_BUSY_SNAPSHOT。
    store.ResetStatements();
    // 关注通知：先查旧状态，只有「从未关注 → 关注」这一跳才写一条；
    // 重复关注不重复提醒（Follow 本身是 INSERT OR IGNORE）。
    bool was_following = false;
    bool status_known = true;
    if (follow && !store.IsFollowing(actor, target_id, &was_following, &error)) {
        status_known = false;
        std::fprintf(stderr, "[%s] 查询关注状态失败: %s\n", kServiceName,
                     error.c_str());
    }
    const bool ok = follow ? store.Follow(actor, target_id, &error)
                           : store.Unfollow(actor, target_id, &error);
    if (!ok) {
        std::fprintf(stderr, "[%s] 关注操作失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (follow && status_known && !was_following) {
        std::string notify_error;
        if (!store.AddNotification(target_id, actor, "follow", "user", target_id,
                                   std::string(), &notify_error)) {
            // 通知是附属动作：写失败只记日志，不能把已经成功的关注改成 500。
            std::fprintf(stderr, "[%s] 写关注通知失败: %s\n", kServiceName,
                         notify_error.c_str());
        }
    }
    // 响应带上目标用户的最新粉丝数，前端可直接刷新按钮旁的数字。
    std::int64_t followers = 0;
    std::int64_t following_count = 0;
    std::string count_error;
    if (!store.FollowCounts(target_id, &followers, &following_count,
                            &count_error)) {
        followers = 0;
    }
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"following\":") +
                     (follow ? "true" : "false") +
                     ",\"followers\":" + std::to_string(followers) + "}";
    return true;
}

/// @brief 关注 / 粉丝列表（分页）。
bool HandleFollowList(douyin::Store& store, const douyin::HttpRequest& request,
                      std::int64_t id, bool following,
                      douyin::HttpResponse* response) {
    douyin::StoredUser target;
    std::string error;
    const bool found = store.FindUserById(id, &target, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "[%s] 查询用户失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (!found) {
        return Fail(response, 404, "用户不存在");
    }

    std::int64_t limit = kDefaultPageSize;
    std::int64_t offset = 0;
    if (const std::string text = douyin::QueryParam(request, "limit"); !text.empty()) {
        limit = std::atoll(text.c_str());
    }
    if (const std::string text = douyin::QueryParam(request, "offset"); !text.empty()) {
        offset = std::atoll(text.c_str());
    }
    if (limit <= 0) {
        limit = kDefaultPageSize;
    }
    if (limit > kMaxPageSize) {
        limit = kMaxPageSize;
    }
    if (offset < 0) {
        offset = 0;
    }

    std::vector<std::int64_t> ids;
    const bool ok = following ? store.Following(id, &ids, &error)
                              : store.Followers(id, &ids, &error);
    if (!ok) {
        std::fprintf(stderr, "[%s] 查询列表失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    std::vector<std::int64_t> page;
    const std::int64_t total = static_cast<std::int64_t>(ids.size());
    for (std::int64_t i = offset; i < total && static_cast<std::int64_t>(page.size()) < limit;
         ++i) {
        page.push_back(ids[static_cast<std::size_t>(i)]);
    }

    std::string body = "{\"ok\":true,\"ids\":[";
    for (std::size_t i = 0; i < page.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += std::to_string(page[i]);
    }
    body += "],\"total\":" + std::to_string(total) + "}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/**
 * @brief 拉黑 / 取消拉黑：`POST|DELETE /api/user/:id/block`。
 *
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 原始请求；必须带有效 Bearer token。
 * @param target_id [in] 被拉黑者 id（>0），由 `ParseUserPath` 解析。
 * @param block [in] true=拉黑，false=取消。
 * @param response [out] 成功写 `{ok,blocked,already}`；失败写统一错误体。
 * @return 恒为 true（已处理）。
 *
 * ## 定位
 * 拉黑关系的唯一写入口。错了会怎样：重复行让黑名单出现同一个人两次，
 * 或取消拉黑后 feed 仍过滤。
 *
 * ## 步骤
 * step1. 鉴权（拉黑是登录操作，匿名 401）。
 * step2. 拒绝拉黑自己（400），确认目标用户存在（404）。
 * step3. 写/删 blocks（唯一键 + INSERT OR IGNORE 保证幂等）。
 * step4. 输出当前状态；重复拉黑 `already=true`，未拉黑时取消也成功。
 */
bool HandleBlock(douyin::Store& store, const douyin::HttpRequest& request,
                 std::int64_t target_id, bool block,
                 douyin::HttpResponse* response) {
    // step1: 鉴权。
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    // step2: 目标校验。
    if (actor == target_id) {
        return Fail(response, 400, "不能拉黑自己");
    }
    douyin::StoredUser target;
    std::string error;
    if (!store.FindUserById(target_id, &target, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询用户失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "用户不存在");
    }
    // step3: 写库。写前清缓存，避免读游标残留触发 BUSY_SNAPSHOT。
    store.ResetStatements();
    bool already = false;
    if (block) {
        const int inserted = store.BlockUser(actor, target_id, &error);
        if (inserted < 0) {
            std::fprintf(stderr, "[%s] 拉黑失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        already = inserted == 0;
    } else if (!store.UnblockUser(actor, target_id, &error)) {
        std::fprintf(stderr, "[%s] 取消拉黑失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // step4: 输出当前状态。
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"blocked\":") +
                     (block ? "true" : "false") + ",\"already\":" +
                     (already ? "true" : "false") + ",\"user_id\":" +
                     std::to_string(target_id) + "}";
    return true;
}

/**
 * @brief 查询是否已拉黑：`GET /api/user/:id/block`。
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 原始请求；必须带有效 Bearer token。
 * @param target_id [in] 被拉黑者 id（>0）。
 * @param response [out] 成功写 `{ok,blocked}`；失败写统一错误体。
 * @return 恒为 true。
 */
bool HandleBlockStatus(douyin::Store& store,
                       const douyin::HttpRequest& request,
                       std::int64_t target_id,
                       douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    bool blocked = false;
    std::string error;
    if (!store.IsBlocked(actor, target_id, &blocked, &error)) {
        std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"blocked\":") +
                     (blocked ? "true" : "false") + ",\"user_id\":" +
                     std::to_string(target_id) + "}";
    return true;
}

/**
 * @brief 黑名单列表：`GET /api/user/blocks`。
 * @param store [in,out] 已打开的存储；借用。
 * @param request [in] 原始请求；必须带有效 Bearer token。
 * @param response [out] 成功写 `{ok,items,total}`；失败写统一错误体。
 * @return 恒为 true。
 *
 * ## 定位
 * 黑名单管理页的唯一数据源（一次 JOIN 回填昵称，不逐条查）。
 */
bool HandleBlockList(douyin::Store& store, const douyin::HttpRequest& request,
                     douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::vector<douyin::StoredBlock> items;
    std::string error;
    if (!store.ListBlocks(actor, &items, &error)) {
        std::fprintf(stderr, "[%s] 查询黑名单失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"id\":" + std::to_string(items[i].id) +
                ",\"user_id\":" + std::to_string(items[i].user_id) +
                ",\"blocked_id\":" + std::to_string(items[i].blocked_id) +
                ",\"blocked_name\":\"" +
                douyin::JsonEscape(items[i].blocked_name) +
                "\",\"created_at\":" +
                std::to_string(items[i].created_at) + "}";
    }
    body += "],\"total\":" + std::to_string(items.size()) + "}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief 从 `/api/user/<id>` 或 `/api/user/<id>/<suffix>` 解析 id 与后缀。
bool ParseUserPath(const std::string& path, std::int64_t* id, std::string* suffix) {
    constexpr std::string_view kPrefix = "/api/user/";
    if (path.size() <= kPrefix.size() ||
        path.compare(0, kPrefix.size(), kPrefix.data(), kPrefix.size()) != 0) {
        return false;
    }
    const std::string rest = path.substr(kPrefix.size());
    const std::size_t slash = rest.find('/');
    const std::string id_text = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (id_text.empty() || id_text.size() > 18) {
        return false;
    }
    for (const char c : id_text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    *id = std::atoll(id_text.c_str());
    *suffix = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    return *id > 0;
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

    // 黑名单列表是固定路径，必须放在 `/api/user/<id>` 解析之前——否则
    // `blocks` 会被当成非数字 id 直接落到 404。
    if (request.path == "/api/user/blocks") {
        if (request.method == "GET") {
            return HandleBlockList(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }

    constexpr std::string_view kRegister = "/api/user/register";
    constexpr std::string_view kLogin = "/api/user/login";
    if (request.method == "POST" && request.path == kRegister) {
        return HandleRegister(store, request, response);
    }
    if (request.method == "POST" && request.path == kLogin) {
        return HandleLogin(store, request, response);
    }

    std::int64_t id = 0;
    std::string suffix;
    if (!ParseUserPath(request.path, &id, &suffix)) {
        return false;
    }
    if (suffix.empty()) {
        if (request.method == "GET") {
            return HandleProfile(store, request, id, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (suffix == "follow") {
        if (request.method == "POST") {
            return HandleFollow(store, request, id, true, response);
        }
        if (request.method == "DELETE") {
            return HandleFollow(store, request, id, false, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (suffix == "block") {
        if (request.method == "POST") {
            return HandleBlock(store, request, id, true, response);
        }
        if (request.method == "DELETE") {
            return HandleBlock(store, request, id, false, response);
        }
        if (request.method == "GET") {
            return HandleBlockStatus(store, request, id, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (suffix == "following" && request.method == "GET") {
        return HandleFollowList(store, request, id, true, response);
    }
    if (suffix == "followers" && request.method == "GET") {
        return HandleFollowList(store, request, id, false, response);
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
    std::int64_t users = store.CountUsers(&error);
    if (users < 0) {
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
        auto rows = store.Database()->Query("SELECT 1 FROM users LIMIT 1");
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
    std::printf("[%s] 监听 0.0.0.0:%u db=%s users=%lld\n", kServiceName,
                static_cast<unsigned>(port), db_path.c_str(),
                static_cast<long long>(users));
    std::fflush(stdout);

    server.ServeForever([&store](const douyin::HttpRequest& request,
                                 douyin::HttpResponse* response) {
        return Handle(store, request, response);
    });
    return 0;
}
