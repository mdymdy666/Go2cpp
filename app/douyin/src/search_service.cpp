/**
 * @file search_service.cpp
 * @brief 抖音 P0 search 服务（29106）：综合搜索（user/video/live）。
 *
 * @details 路由：
 *          - GET /api/search?q=&type=&limit=&cursor=
 *
 *          `q` 非空（空/全空白 → 400）；`type` ∈ user/video/live/all
 *          （缺省 all）；`limit` 缺省 20、上限 100。匹配规则：`name`/`title`
 *          包含 q（`lower(...) LIKE lower('%q%')`，大小写不敏感；`%`/`_`
 *          按字面量转义）。排序按 id 倒序；`cursor` 是十进制 offset（空串表示
 *          第一页），`next_cursor` 只在还有下一页时非空。
 *
 *          live 表由 C7 创建，本阶段没有房间数据：`type=live` 与 `type=all`
 *          的 live 部分都返回空列表，不查询数据库、也不报错。
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
#include "douyin_tag.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29106;
constexpr const char* kServiceName = "search";
constexpr std::int64_t kDefaultPageSize = 20;
constexpr std::int64_t kMaxPageSize = 100;

/// @brief 单线程事件循环上的计数器（无需原子）。
struct ServiceMetrics {
    std::uint64_t requests{0};
    std::uint64_t errors{0};
};

ServiceMetrics g_metrics;

/// @brief 当前 Unix 秒（token 过期校验用）。
std::int64_t NowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

/**
 * @brief 尝试解析可选的 Bearer token，用于「当前查看者」拉黑过滤。
 * @param request [in] 原始请求；Authorization 头可为空。
 * @return 有效 token 的 user_id；无 token / 非法 / 过期一律 0（匿名）。
 * @note 搜索是公开接口，token 无效不应 401；匿名请求无法应用个人黑名单。
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

/// @brief 统一错误响应。
bool Fail(douyin::HttpResponse* response, int status, std::string_view message) {
    response->status = status;
    response->body = douyin::ErrorBody(status, message);
    ++g_metrics.errors;
    return true;
}

/// @brief 十六进制数字 → 0~15；非法返回 -1。
int HexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// @brief 最小 URL 解码（`%XX` 与 `+`）；非法序列按字面量保留。
std::string UrlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
            continue;
        }
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = HexDigit(text[i + 1]);
            const int low = HexDigit(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out += static_cast<char>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

/// @brief 解析 `limit`：缺省 20，夹紧到 [1,100]；非数字按缺省处理。
std::int64_t ParseLimit(const douyin::HttpRequest& request) {
    const std::string text = douyin::QueryParam(request, "limit");
    if (text.empty()) {
        return kDefaultPageSize;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || value <= 0) {
        return kDefaultPageSize;
    }
    if (value > kMaxPageSize) {
        return kMaxPageSize;
    }
    return static_cast<std::int64_t>(value);
}

/// @brief 读取 offset 游标；空串 → 0，非法（非纯数字/溢出）→ false。
bool ReadOffset(const douyin::HttpRequest& request, std::int64_t* offset) {
    const std::string text = douyin::QueryParam(request, "cursor");
    *offset = 0;
    if (text.empty()) {
        return true;
    }
    if (text.size() > 18) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    *offset = std::atoll(text.c_str());
    // 防御超大 offset：demo 数据量很小，超过 10 万没有实际意义。
    return *offset <= 100000;
}

/// @brief 搜索词是否为空或全空白。
bool IsBlank(const std::string& text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return false;
        }
    }
    return true;
}

/// @brief 一条结果项。
struct ResultItem {
    std::string type;
    std::int64_t id{0};
    std::string title;
};

/// @brief 结果项 → JSON（复用共享 IDL 的 SearchResult 序列化）。
std::string ToItemJson(const ResultItem& item) {
    douyin::SearchResult result;
    result.type = item.type;
    result.id = item.id;
    result.title = item.title;
    return douyin::ToJson(result);
}


/// @brief 话题结果项：name 为话题名，count 为命中视频数。
struct TagItem {
    std::string name;
    std::int64_t count{0};
};

/// @brief 转小写（按字节，ASCII 足够 demo 使用）。
std::string Lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

/// @brief 大小写不敏感的子串包含判断。
bool ContainsIgnoreCase(const std::string& haystack, const std::string& needle) {
    return Lower(haystack).find(Lower(needle)) != std::string::npos;
}

/// @brief 累加话题计数：同名条目 count + 1，否则追加一条。
void AddTag(const std::string& name, std::vector<TagItem>* out) {
    for (TagItem& item : *out) {
        if (item.name == name) {
            ++item.count;
            return;
        }
    }
    out->push_back(TagItem{name, 1});
}

/// @brief 按命中视频数降序（稳定排序，同数保持首次出现顺序）。
void SortTags(std::vector<TagItem>* tags) {
    std::stable_sort(tags->begin(), tags->end(),
                     [](const TagItem& a, const TagItem& b) {
                         return a.count > b.count;
                     });
}

/// @brief 搜索路由。
bool HandleSearch(douyin::Store& store, const douyin::HttpRequest& request,
                  douyin::HttpResponse* response) {
    const std::string query = UrlDecode(douyin::QueryParam(request, "q"));
    if (IsBlank(query)) {
        return Fail(response, 400, "q 不能为空");
    }

    std::string type = douyin::QueryParam(request, "type");
    if (type.empty()) {
        type = "all";
    }
    if (type != "user" && type != "video" && type != "live" && type != "tag" &&
        type != "all") {
        return Fail(response, 400, "type 非法（user/video/tag/live/all）");
    }

    std::int64_t offset = 0;
    if (!ReadOffset(request, &offset)) {
        return Fail(response, 400, "cursor 非法");
    }
    const std::int64_t limit = ParseLimit(request);
    // 拉黑过滤：搜索用户/视频都要剔除被拉黑者（带 token 时）。
    const std::int64_t viewer = ViewerId(request);

    std::string error;
    std::vector<ResultItem> items;
    bool has_more = false;

    if (type == "live") {
        // C7 之前没有 rooms 表，固定空列表。
    } else if (type == "user" || type == "video") {
        const std::int64_t want = limit + 1;
        if (type == "user") {
            std::vector<douyin::StoredUser> users;
            if (!store.SearchUsers(query, want, offset, &users, &error,
                                   viewer)) {
                std::fprintf(stderr, "[%s] 搜索用户失败: %s\n", kServiceName,
                             error.c_str());
                return Fail(response, 500, "数据库错误");
            }
            if (static_cast<std::int64_t>(users.size()) > limit) {
                has_more = true;
                users.resize(static_cast<std::size_t>(limit));
            }
            for (const douyin::StoredUser& user : users) {
                items.push_back(ResultItem{"user", user.id, user.name});
            }
        } else {
            std::vector<douyin::StoredVideo> videos;
            if (!store.SearchVideos(query, want, offset, &videos, &error,
                                    viewer)) {
                std::fprintf(stderr, "[%s] 搜索视频失败: %s\n", kServiceName,
                             error.c_str());
                return Fail(response, 500, "数据库错误");
            }
            if (static_cast<std::int64_t>(videos.size()) > limit) {
                has_more = true;
                videos.resize(static_cast<std::size_t>(limit));
            }
            for (const douyin::StoredVideo& video : videos) {
                items.push_back(ResultItem{"video", video.id, video.title});
            }
        }
    } else if (type == "all") {
        // all：用户在前、视频在后（固定顺序），live 为空。
        const std::int64_t span = offset + limit + 1;
        std::vector<douyin::StoredUser> users;
        std::vector<douyin::StoredVideo> videos;
        if (!store.SearchUsers(query, span, 0, &users, &error, viewer)) {
            std::fprintf(stderr, "[%s] 搜索用户失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (!store.SearchVideos(query, span, 0, &videos, &error, viewer)) {
            std::fprintf(stderr, "[%s] 搜索视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        std::vector<ResultItem> combined;
        combined.reserve(users.size() + videos.size());
        for (const douyin::StoredUser& user : users) {
            combined.push_back(ResultItem{"user", user.id, user.name});
        }
        for (const douyin::StoredVideo& video : videos) {
            combined.push_back(ResultItem{"video", video.id, video.title});
        }
        const std::size_t start = static_cast<std::size_t>(offset);
        if (start < combined.size()) {
            const std::size_t remaining = combined.size() - start;
            const std::size_t take =
                remaining < static_cast<std::size_t>(limit)
                    ? remaining
                    : static_cast<std::size_t>(limit);
            for (std::size_t i = 0; i < take; ++i) {
                items.push_back(combined[start + i]);
            }
            has_more = start + take < combined.size();
        }
    }

    // 聚合面板：users / videos / tags 各自最多 10 条（与 items 分页无关）。
    constexpr std::int64_t kFacetLimit = 10;
    std::vector<douyin::StoredUser> facet_users;
    std::vector<douyin::StoredVideo> facet_videos;
    std::vector<TagItem> facet_tags;
    if (type == "user" || type == "all") {
        if (!store.SearchUsers(query, kFacetLimit, 0, &facet_users, &error,
                               viewer)) {
            std::fprintf(stderr, "[%s] 搜索用户失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
    }
    if (type == "video" || type == "all") {
        if (!store.SearchVideos(query, kFacetLimit, 0, &facet_videos, &error,
                                viewer)) {
            std::fprintf(stderr, "[%s] 搜索视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
    }
    if (type == "tag" || type == "all") {
        // 话题没有独立表：demo 视频量级是几十条，全量扫描 + 标题提取即可。
        // 命中规则：视频标题含 q（视频相关）或话题名含 q（话题直接命中）。
        std::vector<douyin::StoredVideo> all_videos;
        if (!store.ListAllVideos(&all_videos, &error)) {
            std::fprintf(stderr, "[%s] 枚举视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        for (const douyin::StoredVideo& video : all_videos) {
            const bool title_hit = ContainsIgnoreCase(video.title, query);
            for (const std::string& tag : douyin::ExtractTags(video.title)) {
                if (title_hit || ContainsIgnoreCase(tag, query)) {
                    AddTag(tag, &facet_tags);
                }
            }
        }
        SortTags(&facet_tags);
        if (static_cast<std::int64_t>(facet_tags.size()) > kFacetLimit) {
            facet_tags.resize(static_cast<std::size_t>(kFacetLimit));
        }
    }

    std::string body = "{\"ok\":true,\"users\":[";
    for (std::size_t i = 0; i < facet_users.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"id\":" + std::to_string(facet_users[i].id) +
                ",\"name\":\"" + douyin::JsonEscape(facet_users[i].name) + "\"}";
    }
    body += "],\"videos\":[";
    const std::size_t video_count =
        static_cast<std::size_t>(kFacetLimit) < facet_videos.size()
            ? static_cast<std::size_t>(kFacetLimit)
            : facet_videos.size();
    for (std::size_t i = 0; i < video_count; ++i) {
        if (i != 0) {
            body += ',';
        }
        const douyin::StoredVideo& video = facet_videos[i];
        body += "{\"id\":" + std::to_string(video.id) + ",\"user_id\":" +
                std::to_string(video.user_id) + ",\"title\":\"" +
                douyin::JsonEscape(video.title) + "\",\"url\":\"" +
                douyin::JsonEscape(video.url) + "\",\"like_count\":" +
                std::to_string(video.like_count) + ",\"comment_count\":" +
                std::to_string(video.comment_count) + "}";
    }
    body += "],\"tags\":[";
    for (std::size_t i = 0; i < facet_tags.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"name\":\"" + douyin::JsonEscape(facet_tags[i].name) +
                "\",\"count\":" + std::to_string(facet_tags[i].count) + "}";
    }
    body += "],\"items\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += ToItemJson(items[i]);
    }
    body += "],\"next_cursor\":\"";
    if (has_more) {
        body += std::to_string(offset + limit);
    }
    body += "\"}";
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
        response->body = std::string("{\"ok\":true,\"service\":\"") +
                         kServiceName + "\"}";
        return true;
    }
    if (request.method == "GET" && request.path == "/metrics") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") +
                         kServiceName +
                         "\",\"requests\":" + std::to_string(g_metrics.requests) +
                         ",\"errors\":" + std::to_string(g_metrics.errors) + "}";
        return true;
    }
    if (request.path == "/api/search") {
        if (request.method == "GET") {
            return HandleSearch(store, request, response);
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
        const std::filesystem::path parent =
            std::filesystem::path(db_path).parent_path();
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
        auto rows = store.Database()->Query("SELECT 1 FROM videos LIMIT 1");
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
