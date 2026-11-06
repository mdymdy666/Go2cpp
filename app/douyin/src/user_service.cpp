/**
 * @file user_service.cpp
 * @brief 抖音 P0 user 服务（29101）：注册 / 登录 / 资料 / 关注 / 粉丝列表。
 *
 * @details 路由：
 *          - POST   /api/user/register         注册（重复 email → 409）
 *          - POST   /api/user/login            登录（错误 → 401）
 *          - GET    /api/user/:id              资料（不含 password_hash/salt）
 *          - GET    /api/user/me               本人资料（Bearer）
 *          - PATCH  /api/user/me               改昵称/简介/头像（Bearer）
 *          - POST   /api/user/:id/follow       关注（需 Bearer token，幂等）
 *          - DELETE /api/user/:id/follow       取关（需 Bearer token，幂等）
 *          - GET    /api/user/:id/following    关注列表（分页）
 *          - GET    /api/user/:id/followers    粉丝列表（分页）
 *          - GET    /api/user/:id/videos       主页「作品」tab（分页）
 *          - GET    /api/user/:id/liked        主页「喜欢」tab（仅本人可见）
 *          - GET    /api/user/:id/favorites    主页「收藏」tab（仅本人可见）
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
#include <unordered_map>
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
    user.bio = stored.bio;
    user.avatar_url = stored.avatar_url;
    return user;
}

/// @brief UTF-8 码点个数（续接字节 10xxxxxx 不计；非法字节各算一个）。
std::size_t Utf8Length(std::string_view text) {
    std::size_t count = 0;
    for (const char ch : text) {
        if ((static_cast<unsigned char>(ch) & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

/// @brief 昵称是否含禁用字符：`@` 与任意空白（含全角空格 U+3000）。
bool NameHasForbiddenChar(const std::string& name) {
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '@' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '\v' || c == '\f') {
            return true;
        }
        // U+3000（全角空格）在 UTF-8 里是 E3 80 80。
        if (c == '\xE3' && i + 2 < name.size() && name[i + 1] == '\x80' &&
            name[i + 2] == '\x80') {
            return true;
        }
    }
    return false;
}

/// @brief 头像地址白名单：http(s) 或 `videos/` 下的单段图片文件名。
/// @note 与封面 `ValidCoverUrl`（video_service.cpp）同一套规则：只允许
///       `[A-Za-z0-9_.-]` 组成的 `.jpg/.png` 单段名，挡住目录穿越与
///       二次编码；两处规则必须保持同步。
bool ValidAvatarUrl(const std::string& url) {
    if (url.empty()) {
        return true;
    }
    if (url.size() > 512) {
        return false;
    }
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
        return true;
    }
    constexpr std::string_view kPrefix = "videos/";
    if (url.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    const std::string name = url.substr(kPrefix.size());
    if (name.size() < 5 || name.size() > 128) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    const std::string ext = name.substr(name.size() - 4);
    return ext == ".jpg" || ext == ".png";
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

/// @brief 改写本人资料：`PATCH /api/user/me`（Bearer）。
/// @param store [in,out] 已打开的存储——只写 users 一行。
/// @param request [in] 原始请求——读 body（name/bio/avatar_url 均可选）。
/// @param response [out] 200 + 更新后的 user；400/401/409/500。
/// @return 总是 true。
///
/// ## 步骤
/// step1. 鉴权并读当前资料（缺失字段沿用原值，支持「只改简介」）；
/// step2. 逐字段校验：昵称 2~20 码点、无 @/空白、全局唯一；简介 ≤80；
///        头像走 http(s)/videos 白名单；
/// step3. 单条 UPDATE（唯一提交点），失败 500；
/// step4. 回读最新行返回给前端，避免前端自己拼状态。
///
/// ## 并发/失败语义
/// - 调用约束：单线程事件循环内调用；昵称唯一性先查后写，极端并发下
///   仍可能两个请求同时通过检查（演示规模可接受，DB 无唯一索引兜底）；
/// - 不变量：返回 200 时 users 行已更新，且响应 user 与库内一致。
bool HandleUpdateMe(douyin::Store& store, const douyin::HttpRequest& request,
                    douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    douyin::StoredUser current;
    std::string error;
    if (!store.FindUserById(actor, &current, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询本人失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "用户不存在");
    }
    // step1: 三个字段都可选，未出现的沿用原值。
    std::string name = current.name;
    std::string bio = current.bio;
    std::string avatar_url = current.avatar_url;
    const bool has_name = douyin::JsonStringField(request.body, "name", &name);
    const bool has_bio = douyin::JsonStringField(request.body, "bio", &bio);
    const bool has_avatar =
        douyin::JsonStringField(request.body, "avatar_url", &avatar_url);
    if (!has_name && !has_bio && !has_avatar) {
        return Fail(response, 400, "至少提供 name/bio/avatar_url 之一");
    }
    // step2: 校验。昵称按码点计数，避免中文被字节数误判。
    if (has_name) {
        const std::size_t chars = Utf8Length(name);
        if (chars < 2 || chars > 20) {
            return Fail(response, 400, "昵称需 2~20 个字");
        }
        if (NameHasForbiddenChar(name)) {
            return Fail(response, 400, "昵称不能包含 @ 或空白");
        }
        std::string name_error;
        const bool taken = store.NameTaken(name, actor, &name_error);
        if (!name_error.empty()) {
            std::fprintf(stderr, "[%s] 查询昵称冲突失败: %s\n", kServiceName,
                         name_error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (taken) {
            return Fail(response, 409, "昵称已被占用");
        }
    }
    if (has_bio && Utf8Length(bio) > 80) {
        return Fail(response, 400, "简介不能超过 80 个字");
    }
    if (has_avatar && !ValidAvatarUrl(avatar_url)) {
        return Fail(response, 400, "头像地址不合法");
    }
    // step3: 写前清 ORM 读游标（FindUserById 可能留下未 finalize 的语句）。
    store.ResetStatements();
    const int changed =
        store.UpdateUserProfile(actor, name, bio, avatar_url, &error);
    if (changed < 0) {
        std::fprintf(stderr, "[%s] 更新资料失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (changed == 0) {
        return Fail(response, 404, "用户不存在");
    }
    // step4: 回读权威行。
    douyin::StoredUser updated;
    if (!store.FindUserById(actor, &updated, &error)) {
        std::fprintf(stderr, "[%s] 回读资料失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = "{\"ok\":true,\"user\":" +
                     douyin::ToJson(PublicUser(updated)) + "}";
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

/// @brief 个人主页的三个 tab。
enum class ProfileTab {
    kWorks = 0,     ///< 作品：所有人可见。
    kLiked = 1,     ///< 喜欢：默认仅本人可见。
    kFavorite = 2,  ///< 收藏：默认仅本人可见。
};

/// @brief tab 的稳定名字（响应里回给前端，便于断言与排错）。
const char* ProfileTabName(ProfileTab tab) {
    switch (tab) {
        case ProfileTab::kWorks:
            return "works";
        case ProfileTab::kLiked:
            return "liked";
        case ProfileTab::kFavorite:
            return "favorites";
    }
    return "works";
}

/// @brief 该 tab 是否属于「默认私密」：他人只能拿到空列表 + 提示。
bool IsPrivateTab(ProfileTab tab) {
    return tab == ProfileTab::kLiked || tab == ProfileTab::kFavorite;
}

/// @brief 存储行 → 对外 Video（与 video 服务的 PublicVideo 字段一致）。
douyin::Video PublicVideo(const douyin::StoredVideo& stored) {
    douyin::Video video;
    video.id = stored.id;
    video.user_id = stored.user_id;
    video.title = stored.title;
    video.url = stored.url;
    video.like_count = stored.like_count;
    video.comment_count = stored.comment_count;
    video.created_at = stored.created_at;
    video.cover_url = stored.cover_url;
    video.clip_start_ms = stored.clip_start_ms;
    video.clip_end_ms = stored.clip_end_ms;
    return video;
}

/// @brief JSON 字符串转义（作者昵称可能含引号/反斜杠/换行）。
std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

/// @brief 调存储层拉一页：三个 tab 共用同一分页语义，只是数据源不同。
bool ListTabPage(douyin::Store& store, ProfileTab tab, std::int64_t owner_id,
                 std::int64_t viewer_id, const std::string& cursor,
                 std::int64_t limit, std::vector<douyin::StoredVideo>* out,
                 std::string* next_cursor, std::int64_t* total,
                 std::string* error) {
    switch (tab) {
        case ProfileTab::kWorks:
            return store.ListUserVideos(owner_id, viewer_id, cursor, limit, out,
                                        next_cursor, total, error);
        case ProfileTab::kLiked:
            return store.ListLikedVideos(owner_id, viewer_id, cursor, limit, out,
                                         next_cursor, total, error);
        case ProfileTab::kFavorite:
            return store.ListFavoriteVideos(owner_id, viewer_id, cursor, limit,
                                            out, next_cursor, total, error);
    }
    return false;
}

/// @brief 空列表响应：私密 / 被拉黑 / 真空 都走同一形态，前端只需看标记位。
std::string EmptyTabBody(ProfileTab tab, std::int64_t owner_id,
                         std::int64_t viewer_id, bool private_tab, bool blocked,
                         const std::string& hint) {
    return std::string("{\"ok\":true,\"tab\":\"") + ProfileTabName(tab) +
           "\",\"private\":" + (private_tab ? "true" : "false") +
           ",\"blocked\":" + (blocked ? "true" : "false") +
           ",\"owner_id\":" + std::to_string(owner_id) +
           ",\"viewer_id\":" + std::to_string(viewer_id) +
           ",\"total\":0,\"next_cursor\":\"\",\"items\":[]"
           ",\"hint\":\"" + JsonEscape(hint) + "\"}";
}

/**
 * @brief 个人主页三 tab 的统一处理器：作品 / 喜欢 / 收藏。
 *
 * @param store [in,out] 已打开的存储；借用，不接管生命周期。
 * @param request [in] 原始请求；`cursor`/`limit` 为可选查询参数，Bearer 可选
 *        （匿名也能看公开 tab）。
 * @param owner_id [in] 主页主人 id（>0），由 `ParseUserPath` 解析。
 * @param tab [in] 要拉取的 tab。
 * @param response [out] 成功写 `{ok,tab,private,blocked,owner_id,viewer_id,
 *        total,next_cursor,items}`；失败写统一错误体。
 * @return 恒为 true（已处理）；无法识别的方法由调用方先拦成 405。
 *
 * ## 定位
 * 主页数据的唯一出口，同时是**隐私边界**：喜欢/收藏只对本人返回内容，
 * 他人与匿名一律空列表 + `private:true`（不泄露「有没有内容」）。错了会怎样：
 * 别人能翻到你点赞/收藏过什么，或本人被误判成他人、自己的列表打不开。
 *
 * ## 并发/失败语义
 * - 调用约束：单请求内串行；游标不跨请求缓存。
 * - 失败语义：用户不存在 404；cursor 非法 400；数据库错误 500。
 * - 不变量：`private:true` 时 `items` 必为空、`total` 必为 0。
 */
bool HandleProfileTab(douyin::Store& store, const douyin::HttpRequest& request,
                      std::int64_t owner_id, ProfileTab tab,
                      douyin::HttpResponse* response) {
    // step1: 主人必须存在，否则 404（与 /api/user/:id 一致）。
    douyin::StoredUser owner;
    std::string error;
    const bool found = store.FindUserById(owner_id, &owner, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "[%s] 查询主页用户失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (!found) {
        return Fail(response, 404, "用户不存在");
    }

    // step2: 隐私判断放在任何查询之前——他人视角连 COUNT 都不做。
    const std::int64_t viewer = ViewerId(request);
    if (IsPrivateTab(tab) && viewer != owner_id) {
        response->status = 200;
        response->body = EmptyTabBody(
            tab, owner_id, viewer, true, false,
            tab == ProfileTab::kLiked ? "喜欢列表仅本人可见"
                                      : "收藏列表仅本人可见");
        return true;
    }

    // step3: 双向拉黑：只要有一方拉黑另一方，作品 tab 直接空列表（前端据此
    //        显示「已拉黑」）；喜欢/收藏本人视角不会命中这里。
    bool blocked = false;
    if (viewer > 0 && viewer != owner_id) {
        bool viewer_blocks = false;
        bool owner_blocks = false;
        std::string block_error;
        if (!store.IsBlocked(viewer, owner_id, &viewer_blocks, &block_error) ||
            !store.IsBlocked(owner_id, viewer, &owner_blocks, &block_error)) {
            std::fprintf(stderr, "[%s] 查询主页拉黑状态失败: %s\n", kServiceName,
                         block_error.c_str());
            // 与资料页一致：拉黑状态查不到时按「未拉黑」处理，但列表层
            // 仍会再做一次 SQL 过滤，不会因此泄露数据。
            viewer_blocks = false;
            owner_blocks = false;
        }
        blocked = viewer_blocks || owner_blocks;
        if (blocked) {
            response->status = 200;
            response->body = EmptyTabBody(tab, owner_id, viewer, false, true,
                                          "由于拉黑设置，该内容不会展示");
            return true;
        }
    }

    // step4: 分页参数：limit 夹到 [1, 100]，缺省 20；cursor 交给存储层校验。
    std::int64_t limit = kDefaultPageSize;
    if (const std::string text = douyin::QueryParam(request, "limit");
        !text.empty()) {
        limit = std::atoll(text.c_str());
    }
    if (limit <= 0) {
        limit = kDefaultPageSize;
    }
    if (limit > kMaxPageSize) {
        limit = kMaxPageSize;
    }
    const std::string cursor = douyin::QueryParam(request, "cursor");

    std::vector<douyin::StoredVideo> videos;
    std::string next_cursor;
    std::int64_t total = 0;
    if (!ListTabPage(store, tab, owner_id, viewer, cursor, limit, &videos,
                     &next_cursor, &total, &error)) {
        if (error.rfind("cursor 非法", 0) == 0) {
            return Fail(response, 400, "cursor 非法");
        }
        std::fprintf(stderr, "[%s] 查询主页 %s 失败: %s\n", kServiceName,
                     ProfileTabName(tab), error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    // step5: 作者名按页去重回填（liked/favorites 的作者可能不止一个）；
    //        查不到时退化成 user<id>，与前端 `authorName` 的兜底一致。
    std::unordered_map<std::int64_t, std::string> names;
    for (const douyin::StoredVideo& video : videos) {
        if (names.find(video.user_id) != names.end()) {
            continue;
        }
        douyin::StoredUser author;
        std::string name_error;
        if (store.FindUserById(video.user_id, &author, &name_error) &&
            name_error.empty()) {
            names[video.user_id] = author.name;
        } else {
            names[video.user_id] = "user" + std::to_string(video.user_id);
        }
    }

    // step6: 序列化；items 与 feed 同构（video + author），前端可复用卡片。
    std::string body = std::string("{\"ok\":true,\"tab\":\"") +
                       ProfileTabName(tab) + "\",\"private\":false" +
                       ",\"blocked\":false,\"owner_id\":" +
                       std::to_string(owner_id) + ",\"viewer_id\":" +
                       std::to_string(viewer) + ",\"total\":" +
                       std::to_string(total) + ",\"next_cursor\":\"" +
                       JsonEscape(next_cursor) + "\",\"items\":[";
    for (std::size_t i = 0; i < videos.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"video\":" + douyin::ToJson(PublicVideo(videos[i])) +
                ",\"author\":\"" + JsonEscape(names[videos[i].user_id]) +
                "\"}";
    }
    body += "]}";
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

    // 当前用户资料：`me` 不是数字 id，必须在 ParseUserPath 之前拦截。
    if (request.path == "/api/user/me") {
        if (request.method == "PATCH") {
            return HandleUpdateMe(store, request, response);
        }
        if (request.method == "GET") {
            std::int64_t actor = 0;
            if (!RequireToken(request, response, &actor)) {
                return true;
            }
            return HandleProfile(store, request, actor, response);
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
    if (suffix == "videos") {
        if (request.method == "GET") {
            return HandleProfileTab(store, request, id, ProfileTab::kWorks,
                                    response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (suffix == "liked") {
        if (request.method == "GET") {
            return HandleProfileTab(store, request, id, ProfileTab::kLiked,
                                    response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (suffix == "favorites") {
        if (request.method == "GET") {
            return HandleProfileTab(store, request, id, ProfileTab::kFavorite,
                                    response);
        }
        return Fail(response, 405, "方法不允许");
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
