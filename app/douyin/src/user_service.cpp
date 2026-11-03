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

/// @brief 取资料。
bool HandleProfile(douyin::Store& store, std::int64_t id,
                   douyin::HttpResponse* response) {
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
    response->status = 200;
    response->body = "{\"ok\":true,\"user\":" + douyin::ToJson(PublicUser(stored)) + "}";
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
    // 写前清空 ORM 语句缓存：FindUserById 提前结束会残留读游标，
    // 多服务共享 WAL 库时会让本进程的写触发 SQLITE_BUSY_SNAPSHOT。
    store.ResetStatements();
    const bool ok = follow ? store.Follow(actor, target_id, &error)
                           : store.Unfollow(actor, target_id, &error);
    if (!ok) {
        std::fprintf(stderr, "[%s] 关注操作失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = follow ? "{\"ok\":true,\"following\":true}"
                            : "{\"ok\":true,\"following\":false}";
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
            return HandleProfile(store, id, response);
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
