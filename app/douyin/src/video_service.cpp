/**
 * @file video_service.cpp
 * @brief 抖音 P0 video 服务（29102）：发布 / 详情 / 我的视频 / 删除。
 *
 * @details 路由：
 *          - POST   /api/video            发布（Bearer；缺 title/url → 400）
 *          - GET    /api/video/:id        详情（含 author 名 / like_count /
 *                                         comment_count；不存在 → 404）
 *          - GET    /api/video/mine       我的视频（Bearer；cursor 分页、
 *                                         热度序；响应
 *                                         `{"ok":true,"items":[{"video":...}],
 *                                         "next_cursor":"..."}`）
 *          - POST   /api/video/:id/favorite 收藏 / 取消收藏（Bearer；切换语义，
 *                                         响应 `{"ok":true,"favorited":bool}`）
 *          - GET    /api/video/favorites   我的收藏（Bearer；按收藏时间倒序，
 *                                         响应 `{"ok":true,"items":[{"video":...}],
 *                                         "total":N}`）
 *          - DELETE /api/video/:id        删除（Bearer；非作者 403、不存在 404）
 *          - POST   /api/video/upload/init        开启分片上传（Bearer；返回 upload_id）
 *          - POST   /api/video/upload/chunk       追加分片（Bearer；Content-Range）
 *          - POST   /api/video/upload/finish      校验并转正式文件 + 建记录（Bearer）
 *          - GET    /api/video/upload/status/:id  断点进度（Bearer；仅本人）
 *          - GET    /api/tag/list       热门话题（按视频数降序；limit 默认 20）
 *          - GET    /api/tag/<tag>       话题下的视频（page/page_size 分页）
 *          - GET    /health, /metrics
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_auth.hpp"
#include "douyin_http.hpp"
#include "douyin_proto.hpp"
#include "douyin_share_page.hpp"
#include "douyin_store.hpp"
#include "douyin_tag.hpp"
#include "go2cpp/encoding/base64.hpp"
#include "go2cpp/storage/db.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29102;
constexpr const char* kServiceName = "video";
constexpr std::int64_t kDefaultPageSize = 20;
constexpr std::int64_t kMaxPageSize = 100;
/// @brief 上传幂等缓存：Idempotency-Key → (video_id, 过期时间)。
/// @note 只在本进程内有效（Demo 单实例）；重启丢失不影响正确性，只会让
///       重试再多建一条视频，由前端按钮禁用兜底。
struct UploadIdemEntry {
    std::int64_t video_id{0};
    std::time_t expires_at{0};
};
std::mutex g_upload_idem_mutex;
std::map<std::string, UploadIdemEntry> g_upload_idem;
constexpr std::size_t kMaxIdemEntries = 4096;
constexpr std::int64_t kIdemTtlSeconds = 24 * 60 * 60;

constexpr std::size_t kMaxTitleBytes = 256;
constexpr std::size_t kMaxUrlBytes = 2048;
/// @brief 上传体积上限（32 MiB，与 douyin_http 的 body 上限一致）。
constexpr std::size_t kMaxUploadBytes = 32 * 1024 * 1024;
/// @brief 上传标题的最大字符数（按 UTF-8 字符计，不按字节）。
constexpr std::size_t kMaxTitleChars = 80;

/// @brief Web 根目录兜底值：编译期未注入 -DDOUYIN_WEB_DIR 时使用真树路径。
#ifndef DOUYIN_WEB_DIR
#define DOUYIN_WEB_DIR "/UserData/CodexWorkSpace/Go2cpp/app/douyin/web"
#endif

/// @brief Web 根目录：环境变量 DOUYIN_WEB_DIR 优先，其次编译期注入值。
std::string WebDir() {
    const char* env = std::getenv("DOUYIN_WEB_DIR");
    if (env != nullptr && *env != '\0') {
        return env;
    }
    return DOUYIN_WEB_DIR;
}

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

/// @brief 当前 Unix 毫秒（上传文件名用，避免同秒覆盖）。
std::int64_t NowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// @brief 统计 UTF-8 字符数（续接字节 10xxxxxx 不计入）。
std::size_t Utf8Length(std::string_view text) {
    std::size_t count = 0;
    for (const char ch : text) {
        if ((static_cast<unsigned char>(ch) & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

/// @brief 解码 query 中的 `%XX` 与 `+`；非法百分号序列原样保留。
std::string UrlDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '+') {
            out += ' ';
            continue;
        }
        if (ch == '%' && i + 2 < text.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(text[i + 1]);
            const int lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += ch;
    }
    return out;
}

/// @brief 判断请求体前 12 字节内是否含 MP4 的 `ftyp` 盒标记。
bool LooksLikeMp4(std::string_view body) {
    const std::string_view head = body.substr(0, std::min<std::size_t>(body.size(), 12));
    return head.find("ftyp") != std::string_view::npos;
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

/**
 * @brief 尝试解析可选的 Bearer token，用于「当前查看者」拉黑过滤。
 * @param request [in] 原始请求；Authorization 头可为空。
 * @return 有效 token 的 user_id；无 token / 非法 / 过期一律 0（匿名）。
 * @note 视频详情/话题页是公开接口，token 无效不应 401；匿名请求无法
 *       应用个人黑名单。
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

/// @brief 生成 UUID v4 文本（36 字符，小写）；上传会话 id 用它防止被枚举。
std::string NewUuidV4() {
    static std::mt19937_64 engine([] {
        std::random_device random;
        std::seed_seq seed{random(), random(), random(), random()};
        return std::mt19937_64(seed);
    }());
    std::uint64_t high = engine();
    std::uint64_t low = engine();
    // 版本位固定 4；变体位固定 10xx（RFC 4122 §4.4），其余位保持随机。
    high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    low = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char text[37];
    std::snprintf(text, sizeof(text), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(high >> 32),
                  static_cast<unsigned>((high >> 16) & 0xFFFFU),
                  static_cast<unsigned>(high & 0xFFFFU),
                  static_cast<unsigned>((low >> 48) & 0xFFFFU),
                  static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFULL));
    return std::string(text);
}

/// @brief 是否是合法的 UUID v4 文本（36 字符小写；版本 4、变体 8/9/a/b）。
bool IsUuidV4(std::string_view text) {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') {
                return false;
            }
            continue;
        }
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return text[14] == '4' &&
           (text[19] == '8' || text[19] == '9' || text[19] == 'a' ||
            text[19] == 'b');
}

/// @brief 严格解析 `Content-Range: bytes <first>-<last>/<total>`。
bool ParseContentRange(std::string_view text, std::int64_t* first,
                       std::int64_t* last, std::int64_t* total) {
    constexpr std::string_view kPrefix = "bytes ";
    if (text.size() <= kPrefix.size() ||
        text.compare(0, kPrefix.size(), kPrefix.data(), kPrefix.size()) != 0) {
        return false;
    }
    text.remove_prefix(kPrefix.size());
    const std::size_t dash = text.find('-');
    const std::size_t slash = text.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos ||
        dash > slash) {
        return false;
    }
    const auto parse = [](std::string_view digits, std::int64_t* out) {
        if (digits.empty()) {
            return false;
        }
        std::int64_t value = 0;
        const auto result = std::from_chars(digits.data(),
                                            digits.data() + digits.size(), value, 10);
        if (result.ec != std::errc() ||
            result.ptr != digits.data() + digits.size()) {
            return false;
        }
        *out = value;
        return true;
    };
    std::int64_t begin_value = 0;
    std::int64_t end_value = 0;
    std::int64_t total_value = 0;
    if (!parse(text.substr(0, dash), &begin_value) ||
        !parse(text.substr(dash + 1, slash - dash - 1), &end_value) ||
        !parse(text.substr(slash + 1), &total_value)) {
        return false;
    }
    // 不变量：0 <= first <= last < total；越界一律非法，避免写出超长前缀。
    if (begin_value < 0 || end_value < begin_value || total_value <= 0 ||
        end_value >= total_value) {
        return false;
    }
    *first = begin_value;
    *last = end_value;
    *total = total_value;
    return true;
}

/// @brief 从扁平 JSON 里取整数字段（只服务本项目固定结构；失败返回 false）。
bool JsonIntField(std::string_view body, std::string_view key, std::int64_t* out) {
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
    std::int64_t value = 0;
    const char* const begin = body.data() + pos;
    const char* const end = body.data() + body.size();
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec != std::errc() || result.ptr == begin) {
        return false;
    }
    *out = value;
    return true;
}

/// @brief 上传暂存文件路径：`<web>/videos/.upload_<uuid>.part`（只由服务端拼）。
std::filesystem::path UploadPartPath(const std::string& upload_id) {
    return std::filesystem::path(WebDir()) / "videos" /
           (".upload_" + upload_id + ".part");
}

/// @brief 上传会话的对外 JSON：不含服务端路径。
/// @note `ranges` 用半开区间 `[start, end)` 描述已收分片；协议只允许顺序
///       追加，因此已收部分必然是单段连续前缀，`end` 即已收字节数。
std::string UploadJson(const douyin::StoredUpload& session) {
    std::string body = "{\"ok\":true,\"upload_id\":\"" +
                       douyin::JsonEscape(session.upload_id) +
                       "\",\"received\":" +
                       std::to_string(session.received_bytes) + ",\"total\":" +
                       std::to_string(session.total) + ",\"ranges\":[";
    if (session.received_bytes > 0) {
        body += "{\"start\":0,\"end\":" +
                std::to_string(session.received_bytes) + "}";
    }
    body += "]}";
    return body;
}

/**
 * @brief 取上传会话并校验归属；不存在与不属于本人返回同一个 400。
 *
 * @param store [in,out] 已打开的存储。
 * @param upload_id [in] 客户端提交的会话 id——必须已是 UUID v4 形态。
 * @param actor [in] 当前 Bearer token 对应的用户 id。
 * @param session [out] 查到且归属正确时写入；失败保持原值。
 * @param response [out] 失败时写入 400/500 错误体。
 * @return 查到且归属正确返回 true；否则已写好响应并返回 false。
 *
 * ## 定位
 * 断点续传的唯一鉴权收口：upload_id 是 UUID 不可枚举，但「知道 id」不等于
 * 「有权限」，跨用户必须在这里被挡下；若漏判，任何人可用别人的 upload_id
 * 覆盖/收尾别人的视频。
 */
bool LoadOwnedUpload(douyin::Store& store, const std::string& upload_id,
                     std::int64_t actor, douyin::StoredUpload* session,
                     douyin::HttpResponse* response) {
    // 注意：Fail() 返回 true（供 handler 直接 `return Fail(...)` 链式返回），
    // 而本函数的约定是「false = 已写响应、调用方必须立刻 return true」，
    // 所以每个失败分支都要先 Fail(...) 再显式 return false。
    // 先校验格式：既挡路径注入，也避免拿任意串去查库。
    if (!IsUuidV4(upload_id)) {
        Fail(response, 400, "upload_id 非法");
        return false;
    }
    std::string error;
    if (!store.FindUploadSession(upload_id, session, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询上传会话失败: %s\n", kServiceName,
                         error.c_str());
            Fail(response, 500, "数据库错误");
            return false;
        }
        Fail(response, 400, "上传会话不存在或不属于当前用户");
        return false;
    }
    if (session->user_id != actor) {
        Fail(response, 400, "上传会话不存在或不属于当前用户");
        return false;
    }
    return true;
}

/**
 * @brief 用 `.part` 实际大小校准 DB 里的 received_bytes（崩溃窗口自愈）。
 *
 * @param store [in,out] 已打开的存储。
 * @param session [in,out] 会话——调用前 received_bytes 来自 DB，返回后对齐磁盘。
 * @param response [out] 失败时写 500 错误体。
 * @return 校准成功返回 true。
 *
 * ## 定位
 * 「追加写盘 → 更新 DB」不是原子的：两者之间掉电/被杀，磁盘比 DB 多。这里
 * 以磁盘为准补齐 DB，避免客户端从旧偏移重传导致数据重复；反过来磁盘比 DB
 * 少说明暂存文件被外部截断，宁可 500 也不静默丢字节。
 */
bool SyncUploadProgress(douyin::Store& store, douyin::StoredUpload* session,
                        douyin::HttpResponse* response) {
    // 约定同 LoadOwnedUpload：false = 已写响应，调用方必须立刻返回。
    std::error_code ec;
    const auto size = std::filesystem::file_size(session->path, ec);
    if (ec) {
        std::fprintf(stderr, "[%s] 读取暂存文件大小失败: %s\n", kServiceName,
                     ec.message().c_str());
        Fail(response, 500, "上传暂存文件不可用");
        return false;
    }
    const std::int64_t actual = static_cast<std::int64_t>(size);
    if (actual == session->received_bytes) {
        return true;
    }
    if (actual < session->received_bytes) {
        std::fprintf(stderr, "[%s] 暂存文件被截断: disk=%lld db=%lld\n",
                     kServiceName, static_cast<long long>(actual),
                     static_cast<long long>(session->received_bytes));
        Fail(response, 500, "上传进度与暂存文件不一致");
        return false;
    }
    std::string error;
    if (!store.UpdateUploadProgress(session->upload_id, actual, &error)) {
        std::fprintf(stderr, "[%s] 校准上传进度失败: %s\n", kServiceName,
                     error.c_str());
        Fail(response, 500, "数据库错误");
        return false;
    }
    session->received_bytes = actual;
    return true;
}

/// @brief 解析十进制整数文本（全部字符必须是数字）；失败返回 false。
bool ParseInt64Text(const std::string& text, std::int64_t* out) {
    if (text.empty() || text.size() > 18) {
        return false;
    }
    std::int64_t value = 0;
    const char* const begin = text.data();
    const char* const end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }
    *out = value;
    return true;
}

/// @brief 封面地址白名单：空 / http(s) / `videos/<安全文件名>.(jpg|png)`。
bool ValidCoverUrl(const std::string& url) {
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

/// @brief 裁剪区间合法：0/0=不裁剪；否则 0 <= start < end <= 24h（end=0 到片尾）。
bool ValidClip(std::int64_t start_ms, std::int64_t end_ms) {
    constexpr std::int64_t kMaxClipMs = 24LL * 3600 * 1000;
    if (start_ms < 0 || end_ms < 0 || start_ms > kMaxClipMs || end_ms > kMaxClipMs) {
        return false;
    }
    if (end_ms == 0) {
        return true;
    }
    return start_ms < end_ms;
}

/// @brief 从 query 读可选封面/裁剪（整包上传用，body 是视频字节）。
bool ReadClipFromQuery(const douyin::HttpRequest& request, std::string* cover_url,
                       std::int64_t* clip_start_ms, std::int64_t* clip_end_ms,
                       douyin::HttpResponse* response) {
    *cover_url = UrlDecode(douyin::QueryParam(request, "cover_url"));
    const std::string start = douyin::QueryParam(request, "clip_start_ms");
    const std::string end = douyin::QueryParam(request, "clip_end_ms");
    *clip_start_ms = 0;
    *clip_end_ms = 0;
    if (!start.empty() && !ParseInt64Text(start, clip_start_ms)) {
        Fail(response, 400, "clip_start_ms 非法");
        return false;
    }
    if (!end.empty() && !ParseInt64Text(end, clip_end_ms)) {
        Fail(response, 400, "clip_end_ms 非法");
        return false;
    }
    if (!ValidCoverUrl(*cover_url) || !ValidClip(*clip_start_ms, *clip_end_ms)) {
        Fail(response, 400, "cover_url/裁剪区间非法");
        return false;
    }
    return true;
}

/// @brief 从 JSON body 读可选封面/裁剪（publish 与分片 finish 用）。
bool ReadClipFromJson(const std::string& body, std::string* cover_url,
                      std::int64_t* clip_start_ms, std::int64_t* clip_end_ms,
                      douyin::HttpResponse* response) {
    *cover_url = "";
    *clip_start_ms = 0;
    *clip_end_ms = 0;
    (void)douyin::JsonStringField(body, "cover_url", cover_url);
    if (!JsonIntField(body, "clip_start_ms", clip_start_ms)) {
        *clip_start_ms = 0;
    }
    if (!JsonIntField(body, "clip_end_ms", clip_end_ms)) {
        *clip_end_ms = 0;
    }
    if (!ValidCoverUrl(*cover_url) || !ValidClip(*clip_start_ms, *clip_end_ms)) {
        Fail(response, 400, "cover_url/裁剪区间非法");
        return false;
    }
    return true;
}

/// @brief 把存储行转成对外 Video。
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
    video.tags = douyin::ExtractTags(stored.title);
    // `liked` 由调用方按查看者回填（详情 HasLiked、列表批量
    // FillVideoLikes）；回填前保持 false（匿名语义）。不在这里发 SQL，
    // 否则每个列表元素一次查询，退化成 N+1。
    video.liked = stored.liked;
    return video;
}

/// @brief 解析 `limit`：缺省 20，夹紧到 [1,100]；非数字按缺省处理。
std::int64_t ParseLimit(const douyin::HttpRequest& request) {
    const std::string text = douyin::QueryParam(request, "limit");
    if (text.empty()) {
        return kDefaultPageSize;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str()) {
        return kDefaultPageSize;
    }
    if (value <= 0) {
        return kDefaultPageSize;
    }
    if (value > kMaxPageSize) {
        return kMaxPageSize;
    }
    return static_cast<std::int64_t>(value);
}

/// @brief 读取 `cursor`；空串合法（第一页），格式非法返回 false。
bool ReadCursor(const douyin::HttpRequest& request, std::string* cursor) {
    *cursor = douyin::QueryParam(request, "cursor");
    if (cursor->empty()) {
        return true;
    }
    std::int64_t created_at = 0;
    std::int64_t id = 0;
    return douyin::DecodeVideoCursor(*cursor, &created_at, &id);
}

/// @brief 从 `/api/video/<id>` 解析 id；`/api/video/mine` 等非数字路径返回 false。
bool ParseVideoPath(const std::string& path, std::int64_t* id) {
    constexpr std::string_view kPrefix = "/api/video/";
    if (path.size() <= kPrefix.size() ||
        path.compare(0, kPrefix.size(), kPrefix.data(), kPrefix.size()) != 0) {
        return false;
    }
    const std::string rest = path.substr(kPrefix.size());
    if (rest.empty() || rest.size() > 18) {
        return false;
    }
    for (const char c : rest) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    *id = std::atoll(rest.c_str());
    return *id > 0;
}

/// @brief 发布视频。
bool HandleCreate(douyin::Store& store, const douyin::HttpRequest& request,
                  douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::string title;
    std::string url;
    if (!douyin::JsonStringField(request.body, "title", &title) ||
        !douyin::JsonStringField(request.body, "url", &url) || title.empty() ||
        url.empty()) {
        return Fail(response, 400, "title/url 不能为空");
    }
    if (title.size() > kMaxTitleBytes || url.size() > kMaxUrlBytes) {
        return Fail(response, 400, "字段过长");
    }

    std::string cover_url;
    std::int64_t clip_start_ms = 0;
    std::int64_t clip_end_ms = 0;
    if (!ReadClipFromJson(request.body, &cover_url, &clip_start_ms, &clip_end_ms,
                          response)) {
        return true;
    }

    std::string error;
    const std::int64_t id = store.CreateVideo(
        actor, title, url, cover_url, clip_start_ms, clip_end_ms, &error);
    if (id <= 0) {
        std::fprintf(stderr, "[%s] 发布视频失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    douyin::StoredVideo stored;
    stored.id = id;
    stored.user_id = actor;
    stored.title = title;
    stored.url = url;
    stored.created_at = NowSeconds();
    stored.cover_url = cover_url;
    stored.clip_start_ms = clip_start_ms;
    stored.clip_end_ms = clip_end_ms;
    response->status = 200;
    response->body = "{\"ok\":true,\"video\":" +
                     douyin::ToJson(PublicVideo(stored)) + "}";
    return true;
}


/**
 * @brief 接收客户端上传的 mp4：校验 → 落盘 → 建视频记录 → 返回新视频。
 *
 * @param store [in,out] 已打开的存储——成功时写入 videos 行；失败时保持原状。
 * @param request [in] HTTP 请求——借用；query 需含 `title`，body 为原始 mp4 字节，
 *        头需带 `Authorization: Bearer <token>`。
 * @param response [out] 出站响应——成功 200 + `{"ok":true,"video":{...}}`；
 *        鉴权失败 401；title/体积/魔数非法 400；落盘或建记录失败 500。
 * @return 总是 true（错误通过 response 表达）；调用方**不要**再回 404。
 *
 * ## 定位
 * 视频字节的唯一写入点：文件名、目录与大小上限都在这里收口；一旦校验缺失，
 * 任意用户都能写满磁盘或伪造非视频内容，因此所有检查都在写盘之前完成。
 *
 * ## 步骤
 * step1. 鉴权——上传必须绑定具体用户，缺 token/无效 token 直接 401；
 * step2. 标题——query 先 URL 解码，再按 UTF-8 字符数限制 1–80；
 * step3. 体积——空 body、Content-Length 超限、实际 body 超限都在写盘前拒绝；
 * step4. 魔数——前 12 字节必须含 ftyp，挡掉文本/图片误传；
 * step5. 生成 `upload_<uid>_<ms>.mp4` 并写盘（同毫秒重名顺延），失败 500；
 * step6. 建视频记录；DB 失败时删除已落盘文件，避免孤儿文件。
 *
 * ## 并发/失败语义
 * - 调用约束：单线程事件循环内调用，store 无需额外加锁；
 * - 失败语义：不抛异常；所有错误路径都返回 JSON 错误体；
 * - 不变量：返回 200 时磁盘文件完整、videos 行存在、url 指向该文件。
 */
bool HandleUpload(douyin::Store& store, const douyin::HttpRequest& request,
                  douyin::HttpResponse* response) {
    // step1: 鉴权——未登录/过期 token 一律 401，不泄露后续校验细节。
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    // step2: 标题解码后再校验字符数，避免 %E4... 被当成可见字符计数。
    // 幂等：同一 Idempotency-Key 的重试直接回放第一次的视频，不重复建行。
    const std::string idem_key = request.Header("idempotency-key");
    if (!idem_key.empty() && idem_key.size() <= 200) {
        std::lock_guard<std::mutex> lock(g_upload_idem_mutex);
        const auto it = g_upload_idem.find(idem_key);
        if (it != g_upload_idem.end() &&
            it->second.expires_at > std::time(nullptr)) {
            douyin::StoredVideo replay;
            std::string replay_error;
            if (store.FindVideo(it->second.video_id, &replay, &replay_error)) {
                response->status = 200;
                response->headers["Idempotency-Replayed"] = "true";
                response->body = "{\"ok\":true,\"video\":" +
                                 douyin::ToJson(PublicVideo(replay)) + "}";
                return true;
            }
        }
    }
    const std::string title = UrlDecode(douyin::QueryParam(request, "title"));
    if (title.empty() || Utf8Length(title) > kMaxTitleChars) {
        return Fail(response, 400, "title 长度需为 1-80 字");
    }
    std::string cover_url;
    std::int64_t clip_start_ms = 0;
    std::int64_t clip_end_ms = 0;
    if (!ReadClipFromQuery(request, &cover_url, &clip_start_ms, &clip_end_ms,
                           response)) {
        return true;
    }
    // step3: 体积与完整性检查。读取层负责按 Content-Length 读满；这里再比对一次
    // 声明长度与实际 body，防止短 body 被当成完整视频落盘（读取层换实现时的兜底）。
    if (request.body.empty()) {
        return Fail(response, 400, "请求体为空");
    }
    const std::string length_text = request.Header("content-length");
    if (!length_text.empty()) {
        std::size_t declared = 0;
        if (!douyin::ParseContentLength(length_text, &declared)) {
            return Fail(response, 400, "Content-Length 非法");
        }
        if (declared > kMaxUploadBytes) {
            return Fail(response, 400, "视频不能超过 32 MiB");
        }
        if (declared != request.body.size()) {
            return Fail(response, 400, "请求体不完整");
        }
    }
    if (request.body.size() > kMaxUploadBytes) {
        return Fail(response, 400, "视频不能超过 32 MiB");
    }
    // step4: MP4 魔数——标准文件偏移 4 处是 ftyp，放宽到前 12 字节内查找。
    if (!LooksLikeMp4(request.body)) {
        return Fail(response, 400, "不是合法的 mp4（缺少 ftyp 魔数）");
    }
    // step5: 目录按需创建；文件名固定 upload_<uid>_<ms>.mp4，同毫秒重名顺延。
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(WebDir()) / "videos";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "[%s] 创建上传目录失败: %s\n", kServiceName,
                     ec.message().c_str());
        return Fail(response, 500, "存储目录不可用");
    }
    std::int64_t stamp = NowMilliseconds();
    std::filesystem::path path =
        dir / ("upload_" + std::to_string(actor) + "_" + std::to_string(stamp) + ".mp4");
    for (int i = 0; i < 1000 && std::filesystem::exists(path); ++i) {
        ++stamp;
        path = dir / ("upload_" + std::to_string(actor) + "_" +
                      std::to_string(stamp) + ".mp4");
    }
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            std::fprintf(stderr, "[%s] 打开上传文件失败: %s\n", kServiceName,
                         path.c_str());
            return Fail(response, 500, "写入失败");
        }
        output.write(request.body.data(),
                     static_cast<std::streamsize>(request.body.size()));
        if (!output.good()) {
            output.close();
            std::filesystem::remove(path, ec);
            std::fprintf(stderr, "[%s] 写入上传文件失败: %s\n", kServiceName,
                         path.c_str());
            return Fail(response, 500, "写入失败");
        }
    }
    // step6: 先落盘再建记录；建记录失败要回滚文件，避免磁盘出现无主视频。
    const std::string url = "videos/" + path.filename().string();
    std::string error;
    const std::int64_t id = store.CreateVideo(
        actor, title, url, cover_url, clip_start_ms, clip_end_ms, &error);
    if (id <= 0) {
        std::filesystem::remove(path, ec);
        std::fprintf(stderr, "[%s] 建视频记录失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // 记住这个 key：同 key 的重试（网络抖动/用户连点）回放同一条视频。
    if (!idem_key.empty() && idem_key.size() <= 200) {
        std::lock_guard<std::mutex> lock(g_upload_idem_mutex);
        if (g_upload_idem.size() >= kMaxIdemEntries) {
            g_upload_idem.clear();
        }
        g_upload_idem[idem_key] =
            UploadIdemEntry{id, std::time(nullptr) + kIdemTtlSeconds};
    }
    douyin::StoredVideo stored;
    stored.id = id;
    stored.user_id = actor;
    stored.title = title;
    stored.url = url;
    stored.created_at = NowSeconds();
    stored.cover_url = cover_url;
    stored.clip_start_ms = clip_start_ms;
    stored.clip_end_ms = clip_end_ms;
    response->status = 200;
    response->body = "{\"ok\":true,\"video\":" +
                     douyin::ToJson(PublicVideo(stored)) + "}";
    return true;
}

/**
 * @brief 上传封面：base64 data URL → `videos/cover_<uid>_<ms>.(jpg|png)`。
 *
 * @param store [in,out] 只用来确认存储可用；封面不落 DB，发布时按路径引用。
 * @param request [in] JSON `{"data_url":"data:image/jpeg;base64,..."}`；
 *        Bearer 必需。
 * @param response [out] 成功 200 + `{"ok":true,"cover_url":"videos/..."}`；
 *        格式/体积/base64 非法 400；鉴权 401；写盘失败 500。
 * @return 总是 true。
 *
 * ## 定位
 * 封面字节的唯一写入点：前端抽帧后先传封面拿短路径，再带 `cover_url`
 * 发布视频，避免把几十 KB 的 data URL 塞进发布 query 或数据库行。
 *
 * ## 步骤
 * step1. 鉴权；step2. 校验 data URL 前缀与体积上限；step3. base64 解码；
 * step4. 校验 JPEG/PNG 魔数；step5. 时间戳命名落盘并返回相对路径。
 */
bool HandleCoverUpload(douyin::Store& store, const douyin::HttpRequest& request,
                       douyin::HttpResponse* response) {
    (void)store;
    // step1: 封面必须绑定用户，便于配额与审计。
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    // step2: 只接受 jpeg/png；base64 文本先限长，避免先解码再拒绝。
    std::string data_url;
    if (!douyin::JsonStringField(request.body, "data_url", &data_url) ||
        data_url.empty()) {
        return Fail(response, 400, "缺少 data_url");
    }
    constexpr std::string_view kJpeg = "data:image/jpeg;base64,";
    constexpr std::string_view kPng = "data:image/png;base64,";
    bool png = false;
    if (data_url.rfind(kJpeg, 0) == 0) {
        data_url.erase(0, kJpeg.size());
    } else if (data_url.rfind(kPng, 0) == 0) {
        png = true;
        data_url.erase(0, kPng.size());
    } else {
        return Fail(response, 400, "只支持 jpeg/png data URL");
    }
    if (data_url.size() > 420 * 1024) {
        return Fail(response, 400, "封面不能超过 300 KiB");
    }
    // step3: 解码失败一律 400，不把半截字节写盘。
    auto decoded = go2cpp::encoding::StdEncoding.DecodeString(data_url);
    if (!decoded || decoded.data.size() < 8 ||
        decoded.data.size() > 300 * 1024) {
        return Fail(response, 400, "base64 解码失败或封面超过 300 KiB");
    }
    const std::string& bytes = decoded.data;
    // step4: 魔数校验，挡住「base64 里塞任意文件」。
    const unsigned char* head =
        reinterpret_cast<const unsigned char*>(bytes.data());
    const bool is_jpeg = head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF;
    const bool is_png = head[0] == 0x89 && head[1] == 0x50 && head[2] == 0x4E &&
                        head[3] == 0x47;
    if (png ? !is_png : !is_jpeg) {
        return Fail(response, 400, "封面内容与声明格式不符");
    }
    // step5: 目录按需创建；时间戳命名，同毫秒重名顺延。
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(WebDir()) / "videos";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return Fail(response, 500, "存储目录不可用");
    }
    const std::string ext = png ? ".png" : ".jpg";
    std::int64_t stamp = NowMilliseconds();
    std::filesystem::path path = dir / ("cover_" + std::to_string(actor) + "_" +
                                        std::to_string(stamp) + ext);
    for (int i = 0; i < 1000 && std::filesystem::exists(path); ++i) {
        ++stamp;
        path = dir / ("cover_" + std::to_string(actor) + "_" +
                      std::to_string(stamp) + ext);
    }
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            return Fail(response, 500, "写入失败");
        }
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output.good()) {
            output.close();
            std::filesystem::remove(path, ec);
            return Fail(response, 500, "写入失败");
        }
    }
    response->status = 200;
    response->body = "{\"ok\":true,\"cover_url\":\"videos/" +
                     path.filename().string() + "\",\"bytes\":" +
                     std::to_string(bytes.size()) + "}";
    return true;
}

/**
 * @brief 开启分片上传：分配 UUID、建空 `.part`、把状态写进 SQLite。
 *
 * @param store [in,out] 存储——成功时插入 upload_sessions 行。
 * @param request [in] HTTP 请求——Bearer 必需；body 为 JSON
 *        `{"total": <总字节>, "title": "<可选标题>"}`。
 * @param response [out] 成功 200 + `{"ok":true,"upload_id":...,"received":0,...}`；
 *        参数非法 400；鉴权失败 401；落盘/建行失败 500。
 * @return 总是 true。
 *
 * ## 定位
 * 上传会话的唯一创建点：upload_id 在这里生成，客户端拿不到服务端路径；
 * 一旦允许客户端指定路径或复用 id，就会变成覆盖任意文件的入口。
 *
 * ## 步骤
 * step1. 鉴权——上传必须绑定具体用户；
 * step2. total——正整数且不超过 32 MiB（与整包上传上限一致）；
 * step3. title——可选，出现时按 UTF-8 字符数校验 1–80；
 * step4. 建目录与空 `.part`，再插 upload_sessions；插入失败回滚文件。
 */
bool HandleUploadInit(douyin::Store& store, const douyin::HttpRequest& request,
                      douyin::HttpResponse* response) {
    // step1: 先鉴权，未登录不产生任何磁盘/DB 副作用。
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    // step2: total 是后续所有 Content-Range 的基准，必须一次定死。
    std::int64_t total = 0;
    if (!JsonIntField(request.body, "total", &total) || total <= 0) {
        return Fail(response, 400, "total 必须是正整数");
    }
    if (total > static_cast<std::int64_t>(kMaxUploadBytes)) {
        return Fail(response, 400, "视频不能超过 32 MiB");
    }
    // step3: title 可选；给了就必须可解析且合法（finish 时还会再传一次）。
    std::string title;
    if (request.body.find("\"title\"") != std::string::npos &&
        !douyin::JsonStringField(request.body, "title", &title)) {
        return Fail(response, 400, "title 字段非法");
    }
    if (!title.empty() && Utf8Length(title) > kMaxTitleChars) {
        return Fail(response, 400, "title 长度需为 1-80 字");
    }
    // step4: 先落文件再写 DB；DB 失败要删掉空文件，避免留下无主 .part。
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(WebDir()) / "videos";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "[%s] 创建上传目录失败: %s\n", kServiceName,
                     ec.message().c_str());
        return Fail(response, 500, "存储目录不可用");
    }
    douyin::StoredUpload session;
    session.upload_id = NewUuidV4();
    session.user_id = actor;
    session.path = UploadPartPath(session.upload_id).string();
    session.title = title;
    session.total = total;
    session.created_at = NowSeconds();
    {
        std::ofstream output(session.path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            std::fprintf(stderr, "[%s] 创建暂存文件失败: %s\n", kServiceName,
                         session.path.c_str());
            return Fail(response, 500, "写入失败");
        }
    }
    std::string error;
    if (!store.CreateUploadSession(session, &error)) {
        std::filesystem::remove(session.path, ec);
        std::fprintf(stderr, "[%s] 建上传会话失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = UploadJson(session);
    return true;
}

/**
 * @brief 追加一个分片：校验 `Content-Range` 与顺序 → 追加 `.part` → 更新进度。
 *
 * @param store [in,out] 存储——成功时把 received_bytes 前移。
 * @param request [in] HTTP 请求——Bearer 必需；`?upload_id=`（或 `X-Upload-Id`
 *        头）指定会话；`Content-Range: bytes a-b/N`；body 为该片原始字节。
 * @param response [out] 成功 200 + 最新进度；乱序/重叠/长度不符 400；
 *        鉴权 401；落盘或 DB 失败 500。
 * @return 总是 true。
 *
 * ## 定位
 * 字节进入 `.part` 的唯一入口：顺序校验与长度校验都在写盘之前，否则乱序
 * 追加会静默产生错位文件，finish 时大小恰好相等却内容是坏的。
 *
 * ## 步骤
 * step1. 鉴权 + 取 upload_id；
 * step2. 严格解析 Content-Range，body 长度必须等于 b-a+1；
 * step3. 查会话 + 归属校验（跨用户按 400 处理，不泄露会话是否存在）；
 * step4. 校准磁盘进度后判断：完全落在已收前缀内 = 幂等重传；a > received = 乱序；
 * step5. 追加写盘，成功后再更新 DB；DB 失败回滚文件长度，保证两边一致。
 */
bool HandleUploadChunk(douyin::Store& store, const douyin::HttpRequest& request,
                       douyin::HttpResponse* response) {
    // step1: upload_id 允许放 query 或头，body 是纯二进制，不能塞 JSON。
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::string upload_id = douyin::QueryParam(request, "upload_id");
    if (upload_id.empty()) {
        upload_id = request.Header("x-upload-id");
    }
    if (upload_id.empty()) {
        return Fail(response, 400, "缺少 upload_id");
    }
    // step2: 范围与长度必须自洽，且总大小不能超过上传上限。
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::int64_t total = 0;
    if (!ParseContentRange(request.Header("content-range"), &first, &last, &total)) {
        return Fail(response, 400, "Content-Range 非法（需 bytes a-b/N）");
    }
    if (request.body.empty()) {
        return Fail(response, 400, "分片为空");
    }
    if (static_cast<std::int64_t>(request.body.size()) != last - first + 1) {
        return Fail(response, 400, "分片长度与 Content-Range 不符");
    }
    if (total > static_cast<std::int64_t>(kMaxUploadBytes)) {
        return Fail(response, 400, "视频不能超过 32 MiB");
    }
    // step3: 归属校验；跨用户与不存在同一个错误，避免会话探测。
    douyin::StoredUpload session;
    if (!LoadOwnedUpload(store, upload_id, actor, &session, response)) {
        return true;
    }
    if (total != session.total) {
        return Fail(response, 400, "总大小与 init 不一致");
    }
    if (!SyncUploadProgress(store, &session, response)) {
        return true;
    }
    const std::int64_t received = session.received_bytes;
    // step4: 已收前缀内的重放视为幂等（响应丢失后客户端重试同一片）。
    if (last < received) {
        response->status = 200;
        response->body = UploadJson(session);
        return true;
    }
    if (first != received) {
        return Fail(response, 400,
                    "分片乱序：期望从 " + std::to_string(received) + " 开始");
    }
    // step5: 追加写盘——先写字节，再更新 DB；DB 失败把文件截回旧长度。
    {
        std::ofstream output(session.path, std::ios::binary | std::ios::app);
        if (!output.is_open()) {
            std::fprintf(stderr, "[%s] 打开暂存文件失败: %s\n", kServiceName,
                         session.path.c_str());
            return Fail(response, 500, "写入失败");
        }
        output.write(request.body.data(),
                     static_cast<std::streamsize>(request.body.size()));
        output.flush();
        if (!output.good()) {
            output.close();
            std::error_code rollback;
            std::filesystem::resize_file(
                session.path, static_cast<std::uintmax_t>(received), rollback);
            std::fprintf(stderr, "[%s] 写入分片失败: %s\n", kServiceName,
                         session.path.c_str());
            return Fail(response, 500, "写入失败");
        }
    }
    std::string error;
    if (!store.UpdateUploadProgress(session.upload_id, last + 1, &error)) {
        std::error_code rollback;
        std::filesystem::resize_file(
            session.path, static_cast<std::uintmax_t>(received), rollback);
        std::fprintf(stderr, "[%s] 更新上传进度失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    session.received_bytes = last + 1;
    response->status = 200;
    response->body = UploadJson(session);
    return true;
}

/**
 * @brief 收尾：校验总大小与 MP4 魔数 → `.part` 改名为正式文件 → 建视频记录。
 *
 * @param store [in,out] 存储——成功时插入 videos 行并删除 upload_sessions 行。
 * @param request [in] HTTP 请求——Bearer 必需；body 为 JSON
 *        `{"upload_id":"<uuid>","title":"<1-80 字>"}`。
 * @param response [out] 成功 200 + `{"ok":true,"video":{...}}`；未传完/魔数错
 *        400；鉴权 401；改名或建记录失败 500。
 * @return 总是 true。
 *
 * ## 定位
 * 分片态到正式视频的唯一提交点：只有这里把 `.part` 变成 `videos/upload_*`，
 * 并保证「文件名/DB 行/url」三者一致；任何一步失败都要回滚，否则会出现
 * 磁盘有文件但没人能访问（或反过来）的半提交。
 *
 * ## 步骤
 * step1. 鉴权 + 解析 upload_id/title；
 * step2. 查会话、校验归属，并用磁盘大小校准进度；
 * step3. 已收字节必须等于 total，否则 400（不自动 finish）；
 * step4. 读文件头验证 ftyp 魔数（防止把任意字节拼成「视频」）；
 * step5. 同毫秒重名顺延生成正式文件名并 rename；
 * step6. 建 videos 行；失败则把文件改回 `.part`，保留续传能力；
 * step7. 删除会话行（失败只记日志，视频已可用）。
 */
bool HandleUploadFinish(douyin::Store& store, const douyin::HttpRequest& request,
                        douyin::HttpResponse* response) {
    // step1: 先鉴权；finish 必须带 title（init 的 title 只作展示兜底）。
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::string upload_id;
    if (!douyin::JsonStringField(request.body, "upload_id", &upload_id) ||
        upload_id.empty()) {
        return Fail(response, 400, "缺少 upload_id");
    }
    std::string title;
    if (!douyin::JsonStringField(request.body, "title", &title) || title.empty() ||
        Utf8Length(title) > kMaxTitleChars) {
        return Fail(response, 400, "title 长度需为 1-80 字");
    }
    std::string cover_url;
    std::int64_t clip_start_ms = 0;
    std::int64_t clip_end_ms = 0;
    if (!ReadClipFromJson(request.body, &cover_url, &clip_start_ms, &clip_end_ms,
                          response)) {
        return true;
    }
    // step2: 归属校验 + 用磁盘真实大小校准（崩溃窗口里 DB 可能落后）。
    douyin::StoredUpload session;
    if (!LoadOwnedUpload(store, upload_id, actor, &session, response)) {
        return true;
    }
    if (!SyncUploadProgress(store, &session, response)) {
        return true;
    }
    // step3: 大小不符直接 400，绝不把半截文件当视频发布。
    if (session.received_bytes != session.total) {
        return Fail(response, 400,
                    "上传未完成：已收 " + std::to_string(session.received_bytes) +
                        " / 总 " + std::to_string(session.total));
    }
    // step4: 魔数检查——只读前 12 字节，不把整包重新读进内存。
    std::string head(12, '\0');
    {
        std::ifstream input(session.path, std::ios::binary);
        if (!input.is_open()) {
            std::fprintf(stderr, "[%s] 打开暂存文件失败: %s\n", kServiceName,
                         session.path.c_str());
            return Fail(response, 500, "上传暂存文件不可用");
        }
        input.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(input.gcount()));
    }
    if (!LooksLikeMp4(head)) {
        return Fail(response, 400, "不是合法的 mp4（缺少 ftyp 魔数）");
    }
    // step5: 正式文件名与整包上传保持一致；同毫秒重名顺延。
    const std::filesystem::path part(session.path);
    const std::filesystem::path dir = part.parent_path();
    std::error_code ec;
    std::int64_t stamp = NowMilliseconds();
    std::filesystem::path final_path =
        dir / ("upload_" + std::to_string(actor) + "_" + std::to_string(stamp) +
               ".mp4");
    for (int i = 0; i < 1000 && std::filesystem::exists(final_path); ++i) {
        ++stamp;
        final_path = dir / ("upload_" + std::to_string(actor) + "_" +
                            std::to_string(stamp) + ".mp4");
    }
    std::filesystem::rename(part, final_path, ec);
    if (ec) {
        std::fprintf(stderr, "[%s] 暂存文件改名失败: %s\n", kServiceName,
                     ec.message().c_str());
        return Fail(response, 500, "重命名失败");
    }
    // step6: rename 后再建记录；建记录失败把文件改回 `.part`，客户端可重试 finish。
    const std::string url = "videos/" + final_path.filename().string();
    std::string error;
    const std::int64_t id = store.CreateVideo(
        actor, title, url, cover_url, clip_start_ms, clip_end_ms, &error);
    if (id <= 0) {
        std::error_code restore;
        std::filesystem::rename(final_path, part, restore);
        std::fprintf(stderr, "[%s] 建视频记录失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // step7: 会话行删除失败不影响用户；残留行由下次同名 id 覆盖（UUID 不复用）。
    if (!store.DeleteUploadSession(upload_id, &error)) {
        std::fprintf(stderr, "[%s] 删除上传会话失败: %s\n", kServiceName,
                     error.c_str());
    }
    douyin::StoredVideo stored;
    stored.id = id;
    stored.user_id = actor;
    stored.title = title;
    stored.url = url;
    stored.created_at = NowSeconds();
    stored.cover_url = cover_url;
    stored.clip_start_ms = clip_start_ms;
    stored.clip_end_ms = clip_end_ms;
    response->status = 200;
    response->body = "{\"ok\":true,\"video\":" +
                     douyin::ToJson(PublicVideo(stored)) + "}";
    return true;
}

/**
 * @brief 查断点进度：返回已收字节与已收区间，客户端据此决定从哪片续传。
 *
 * @param store [in,out] 只读使用。
 * @param request [in] HTTP 请求——Bearer 必需（防止他人窥探上传进度）。
 * @param upload_id [in] 从 `/api/video/upload/status/<upload_id>` 取出的 id。
 * @param response [out] 成功 200 + `{"ok":true,"received":N,"total":M,...}`；
 *        非法/跨用户 400；鉴权 401。
 * @return 总是 true。
 */
bool HandleUploadStatus(douyin::Store& store, const douyin::HttpRequest& request,
                        const std::string& upload_id,
                        douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    douyin::StoredUpload session;
    if (!LoadOwnedUpload(store, upload_id, actor, &session, response)) {
        return true;
    }
    if (!SyncUploadProgress(store, &session, response)) {
        return true;
    }
    response->status = 200;
    response->body = UploadJson(session);
    return true;
}

/// @brief 视频详情（附作者名）；被作者拉黑时按 404 隐藏。
bool HandleDetail(douyin::Store& store, const douyin::HttpRequest& request,
                  std::int64_t id, douyin::HttpResponse* response) {
    douyin::StoredVideo stored;
    std::string error;
    if (!store.FindVideo(id, &stored, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "视频不存在");
    }
    // 拉黑过滤（第 3 轮）：拉黑区里的视频详情一律 404，不暴露「被屏蔽」，
    // 与 feed/评论列表保持一致。
    const std::int64_t viewer = ViewerId(request);
    if (viewer > 0) {
        bool blocked = false;
        // 双向：viewer 拉黑作者（内容过滤）或作者拉黑 viewer（防骚扰），
        // 两种情况下视频都对 viewer 不可见。
        if (!store.IsBlocked(viewer, stored.user_id, &blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (!blocked &&
            !store.IsBlocked(stored.user_id, viewer, &blocked, &error)) {
            std::fprintf(stderr, "[%s] 查询拉黑状态失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (blocked) {
            return Fail(response, 404, "视频不存在");
        }
        // 详情页刷新后要恢复 ♥：登录态用单条 HasLiked（主键查询），
        // 查不到/查询失败按未赞处理，不阻断详情。
        std::string like_error;
        stored.liked = store.HasLiked(id, viewer, &like_error);
        if (!like_error.empty()) {
            std::fprintf(stderr, "[%s] 查询详情点赞态失败: %s\n", kServiceName,
                         like_error.c_str());
        }
    }

    std::string author;
    if (!store.FindUserName(stored.user_id, &author, &error) && !error.empty()) {
        std::fprintf(stderr, "[%s] 查询作者失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    response->status = 200;
    response->body = "{\"ok\":true,\"video\":" +
                     douyin::ToJson(PublicVideo(stored)) + ",\"author\":\"" +
                     douyin::JsonEscape(author) + "\"}";
    return true;
}


/// @brief 从 `<前缀>/v/<id>` 解析视频 id 与站点前缀。
///
/// @param path [in] 请求路径，如 `/v/12`、`/douyin/v/12`（允许一个尾斜杠）；
///        借用，函数内不保留引用。
/// @param id [out] 解析成功时写视频 id（>0）；失败时保持原值不变。
/// @param base_path [out] 成功时写站点前缀（`""` 或 `/douyin`，无尾斜杠）；
///        失败时保持原值不变。
/// @return 路径合法返回 true；否则返回 false，调用方回 404 落地页。
///
/// ## 定位
/// 落地页路由的唯一路径解析点。错了会怎样：把 `/v/abc` 当成合法 id 去查库
/// （浪费一次查询），或把公网前缀 `/douyin` 丢掉导致页面内视频/接口 404。
bool ParseSharePath(std::string_view path, std::int64_t* id,
                    std::string* base_path) {
    constexpr std::string_view kMarker = "/v/";
    const std::size_t marker = path.rfind(kMarker);
    if (marker == std::string_view::npos) {
        return false;
    }
    std::string_view rest = path.substr(marker + kMarker.size());
    if (!rest.empty() && rest.back() == '/') {
        rest.remove_suffix(1);
    }
    // 18 位十进制足以覆盖 int64，同时挡掉超长数字串造成的溢出。
    if (rest.empty() || rest.size() > 18) {
        return false;
    }
    std::int64_t value = 0;
    for (const char c : rest) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value <= 0) {
        return false;
    }
    std::string prefix(path.substr(0, marker));
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    *id = value;
    *base_path = std::move(prefix);
    return true;
}

/// @brief 站点源（`http://host[:port]`）；拿不到合法 Host 时返回空串。
///
/// @param request [in] 入站请求；只读 `X-Forwarded-Host` / `Host` /
///        `X-Forwarded-Proto`。借用。
/// @return 绝对地址前缀；空串表示未知（渲染层退回相对 URL）。
///
/// ## 定位
/// OG 标签（og:url / og:image / og:video）需要绝对地址，微信/QQ 的抓取器
/// 不执行 JS、也不补全相对路径。网关转发时会擦掉 Host 头，因此由网关中间件
/// 注入 `X-Forwarded-Host`；本函数只信任该头与入站 Host，并过滤控制字符，
/// 避免把攻击者构造的头拼进 HTML 属性。
std::string ShareOrigin(const douyin::HttpRequest& request) {
    std::string host = request.Header("X-Forwarded-Host");
    if (host.empty()) {
        host = request.Header("Host");
    }
    // 多级代理会追加 `,` 分隔的多个 host，只取第一个。
    const std::size_t comma = host.find(',');
    if (comma != std::string::npos) {
        host.resize(comma);
    }
    for (const char c : host) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F || c == '<' || c == '>') {
            return std::string();
        }
    }
    if (host.empty()) {
        return std::string();
    }
    std::string proto = request.Header("X-Forwarded-Proto");
    if (proto != "https") {
        proto = "http";
    }
    return proto + "://" + host;
}

/// @brief DB 里的 url → 落地页可播放地址（带站点前缀）。
///
/// @param prefix [in] 站点前缀（`"/"` 或 `"/douyin/"`）；借用。
/// @param url [in] videos.url 列；借用。可为绝对 http(s)、`videos/x.mp4`、
///        裸文件名或空串。
/// @param id [in] 视频 id；url 为空时用它选 demo 兜底片（1~5）。
/// @return 可直接写进 `<video src>` 的地址；调用方负责 HTML 转义。
std::string ShareVideoUrl(const std::string& prefix, const std::string& url,
                          std::int64_t id) {
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
        return url;
    }
    if (url.rfind("videos/", 0) == 0) {
        return prefix + url;
    }
    if (url.rfind('/', 0) == 0) {
        return url;
    }
    if (url.empty()) {
        return prefix + "videos/demo" + std::to_string((id % 5) + 1) + ".mp4";
    }
    return prefix + "videos/" + url;
}

/// @brief DB 里的 url → 封面地址（同名 jpg 存在才用，否则用 demo 封面兜底）。
///
/// @param prefix [in] 站点前缀（`"/"` 或 `"/douyin/"`）；借用。
/// @param url [in] videos.url 列；借用。
/// @param id [in] 视频 id；兜底封面按 `(id % 5) + 1` 选。
/// @return 封面地址；文件是否真的存在由本函数用 std::filesystem 确认，
///         避免 OG 图 404（聊天窗口里表现为空白预览）。
std::string SharePosterUrl(const std::string& prefix, const std::string& url,
                           const std::string& cover_url, std::int64_t id) {
    // 用户选的封面优先：绝对地址直接用，`videos/x.jpg` 需文件存在才落 URL。
    if (cover_url.rfind("http://", 0) == 0 ||
        cover_url.rfind("https://", 0) == 0) {
        return cover_url;
    }
    if (cover_url.rfind("videos/", 0) == 0) {
        std::error_code cover_ec;
        if (std::filesystem::exists(
                std::filesystem::path(WebDir()) / cover_url.substr(7), cover_ec) &&
            !cover_ec) {
            return prefix + cover_url;
        }
    }
    std::string name = url;
    const std::size_t slash = name.rfind('/');
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    const std::size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    if (!name.empty()) {
        std::error_code ec;
        const std::filesystem::path poster =
            std::filesystem::path(WebDir()) / "videos" / (name + ".jpg");
        if (std::filesystem::exists(poster, ec) && !ec) {
            return prefix + "videos/" + name + ".jpg";
        }
    }
    return prefix + "videos/demo" + std::to_string((id % 5) + 1) + ".jpg";
}

/**
 * @brief 渲染分享落地页 `GET /v/:id`（服务端直出完整 HTML）。
 *
 * @param store [in,out] 已打开的存储——本函数只读；内部可能触发 ORM 语句
 *        缓存，多进程 WAL 下由调用方保证请求边界 `ResetStatements()`。
 * @param request [in] 入站请求——借用；读 Authorization（可选身份）、
 *        Host / X-Forwarded-* 头；不读 body。
 * @param path [in] 请求路径（`/v/12` 或 `/douyin/v/12`）；借用，生命周期
 *        与 request 相同。
 * @param response [out] 出站响应——成功 200 + HTML；视频不存在 / 已删除 /
 *        被拉黑 404 + HTML；数据库错误 500 + JSON（极端路径，浏览器看到
 *        错误文本也可以接受）。不可为 nullptr。
 * @return 恒为 true（表示本路由已处理）；调用方**不要**再走 `/api/video`
 *         的 404 兜底。
 *
 * ## 定位
 * 分享链接的唯一落地点：微信/QQ/复制链接打开的 `/v/:id` 都由这里直出页面。
 * 错了会怎样：分享卡片没有标题/封面（OG 缺失）、被拉黑作者重新可见（过滤
 * 漏掉）、或已删除视频仍返回 200（爬虫缓存死链）。因此 404 与拉黑都必须
 * 在**渲染之前**判定。
 *
 * ## 步骤
 * step1. 解析 id 与站点前缀；失败直接 404 页（不查库）；
 * step2. 查视频行；不存在/已删除 → 404 页，数据库错误 → 500；
 * step3. 登录者做拉黑双向检查，命中 → 404 页（不暴露「被屏蔽」）；
 * step4. 组装页面数据：作者、点赞/评论计数、播放/封面地址；
 * step5. 查第一页评论并批量回填点赞；失败只降级为空评论，不影响整页；
 * step6. 输出 HTML，`Cache-Control: no-store`（计数随互动变化）。
 *
 * ## 并发/失败语义
 * - 调用约束：服务单线程事件循环内调用；store 非线程安全；
 * - 原子性：只读，无写入；评论查询失败与点赞回填失败都不会让页面 500；
 * - 失败语义：404 用 HTML 页表达（人可读），500 用既有 JSON 错误体；
 * - 不变量：返回 200 时页面里的 og:title/og:image/og:video 一定非空。
 */
bool HandleSharePage(douyin::Store& store, const douyin::HttpRequest& request,
                     std::string_view path, douyin::HttpResponse* response) {
    // step1: 路径解析；404 页同样需要 base_path / origin 才能「回首页」。
    std::int64_t video_id = 0;
    std::string base_path;
    const bool parsed = ParseSharePath(path, &video_id, &base_path);
    const std::string origin = ShareOrigin(request);
    auto not_found = [response, &origin, &base_path]() {
        response->status = 404;
        response->content_type = "text/html; charset=utf-8";
        response->headers["Cache-Control"] = "no-store";
        response->headers["X-Content-Type-Options"] = "nosniff";
        response->body = douyin::BuildShareNotFound(base_path, origin);
        return true;
    };
    if (!parsed) {
        return not_found();
    }
    // step2: 视频行；软删/真删在存储层都表现为「查不到」，统一 404。
    douyin::StoredVideo stored;
    std::string error;
    if (!store.FindVideo(video_id, &stored, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询落地页视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return not_found();
    }
    // step3: 拉黑双向过滤——viewer 拉黑作者，或作者拉黑 viewer，都按 404 隐藏；
    // 匿名请求没有身份，不做个人黑名单（与 feed/详情一致）。
    const std::int64_t viewer = ViewerId(request);
    if (viewer > 0) {
        bool blocked = false;
        if (!store.IsBlocked(viewer, stored.user_id, &blocked, &error) ||
            (!blocked &&
             !store.IsBlocked(stored.user_id, viewer, &blocked, &error))) {
            std::fprintf(stderr, "[%s] 查询落地页拉黑状态失败: %s\n",
                         kServiceName, error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        if (blocked) {
            return not_found();
        }
    }
    // step4: 页面数据；作者名查不到只显示「用户 #id」，不阻断页面。
    douyin::SharePageData data;
    data.video_id = video_id;
    data.title = stored.title;
    data.author_id = stored.user_id;
    std::string author;
    if (!store.FindUserName(stored.user_id, &author, &error)) {
        author.clear();
    }
    data.author_name = std::move(author);
    data.like_count = stored.like_count;
    data.comment_count = stored.comment_count;
    data.created_at = stored.created_at;
    data.base_path = base_path;
    data.origin = origin;
    const std::string prefix = base_path + "/";
    data.video_url = ShareVideoUrl(prefix, stored.url, video_id);
    data.poster_url =
        SharePosterUrl(prefix, stored.url, stored.cover_url, video_id);
    // OG 图片尺寸取封面真实像素（读 JPEG SOF 段）；读不到就留 0，
    // 页面不输出 width/height——输出错误尺寸比不输出更糟。
    if (data.poster_url.rfind(prefix + "videos/", 0) == 0) {
        const std::string rel =
            data.poster_url.substr(prefix.size() + std::string("videos/").size());
        douyin::ReadJpegSize(WebDir() + "/videos/" + rel, &data.poster_width,
                             &data.poster_height);
    }
    if (viewer > 0) {
        bool following = false;
        std::string status_error;
        data.liked = store.HasLiked(video_id, viewer, &status_error);
        if (store.IsFollowing(viewer, stored.user_id, &following,
                              &status_error)) {
            data.following = following;
        }
    }
    // step5: 评论区第一页（置顶在前，再按时间 20 条）；失败只降级空评论。
    std::vector<douyin::StoredComment> comments;
    std::string next_cursor;
    std::vector<douyin::StoredComment> pinned;
    if (!store.ListPinnedComments(video_id, viewer, &pinned, &error)) {
        std::fprintf(stderr, "[%s] 查询落地页置顶评论失败: %s\n",
                     kServiceName, error.c_str());
        pinned.clear();
    }
    if (!store.ListComments(video_id, "", 20, "new", &comments, &next_cursor,
                            &error, viewer)) {
        std::fprintf(stderr, "[%s] 查询落地页评论失败: %s\n", kServiceName,
                     error.c_str());
        comments.clear();
    } else {
        if (!pinned.empty()) {
            comments.insert(comments.begin(), pinned.begin(), pinned.end());
        }
        if (!store.FillCommentLikes(viewer, &comments, &error)) {
            std::fprintf(stderr, "[%s] 回填落地页评论点赞失败: %s\n",
                         kServiceName, error.c_str());
        }
        data.comments.reserve(comments.size());
        // 评论者昵称：原先只显示「用户 #id」，同页里作者有昵称、评论者没有，
        // 用户分不清谁是谁。按 user_id 去重查询，一页最多 20 次。
        std::map<std::int64_t, std::string> comment_names;
        for (const auto& comment : comments) {
            if (comment_names.find(comment.user_id) != comment_names.end()) {
                continue;
            }
            std::string name;
            if (!store.FindUserName(comment.user_id, &name, &error)) {
                name.clear();
            }
            comment_names[comment.user_id] = std::move(name);
        }
        for (const auto& comment : comments) {
            douyin::ShareComment item;
            item.id = comment.id;
            item.user_id = comment.user_id;
            item.author_name = comment_names[comment.user_id];
            item.content = comment.content;
            item.created_at = comment.created_at;
            item.like_count = comment.like_count;
            item.liked = comment.liked;
            item.pinned = comment.pinned_at > 0;
            data.comments.push_back(std::move(item));
        }
        data.next_cursor = std::move(next_cursor);
    }
    // step6: 输出；no-store 防止分享页点赞/评论数被缓存成旧值。
    response->status = 200;
    response->content_type = "text/html; charset=utf-8";
    response->headers["Cache-Control"] = "no-store";
    response->headers["X-Content-Type-Options"] = "nosniff";
    response->body = douyin::BuildSharePage(data);
    return true;
}

/// @brief 我的视频（热度序 + cursor 分页）。
bool HandleMine(douyin::Store& store, const douyin::HttpRequest& request,
                douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    std::string cursor;
    if (!ReadCursor(request, &cursor)) {
        return Fail(response, 400, "cursor 非法");
    }
    const std::int64_t limit = ParseLimit(request);

    std::vector<douyin::StoredVideo> videos;
    std::string next_cursor;
    std::string error;
    if (!store.ListByUser(actor, cursor, limit, &videos, &next_cursor, &error)) {
        std::fprintf(stderr, "[%s] 查询我的视频失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // 一页一条 SQL 回填 liked，避免逐条查询。
    if (!store.FillVideoLikes(actor, &videos, &error)) {
        std::fprintf(stderr, "[%s] 回填我的视频点赞态失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }

    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < videos.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"video\":" + douyin::ToJson(PublicVideo(videos[i])) + "}";
    }
    body += "],\"next_cursor\":\"" + douyin::JsonEscape(next_cursor) + "\"}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/// @brief 从 `/api/video/<id>/favorite` 解析 id；其它路径返回 false。
bool ParseFavoritePath(std::string_view path, std::int64_t* id) {
    constexpr std::string_view kPrefix = "/api/video/";
    constexpr std::string_view kSuffix = "/favorite";
    if (path.size() <= kPrefix.size() + kSuffix.size() ||
        path.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    const std::size_t suffix_at = path.size() - kSuffix.size();
    if (suffix_at <= kPrefix.size() ||
        path.compare(suffix_at, kSuffix.size(), kSuffix) != 0) {
        return false;
    }
    const std::string_view number =
        path.substr(kPrefix.size(), suffix_at - kPrefix.size());
    if (number.empty() || number.size() > 18) {
        return false;
    }
    std::int64_t value = 0;
    const auto parsed =
        std::from_chars(number.data(), number.data() + number.size(), value);
    if (parsed.ec != std::errc() ||
        parsed.ptr != number.data() + number.size() || value <= 0) {
        return false;
    }
    *id = value;
    return true;
}

/// @brief `POST /api/video/<id>/favorite`：切换当前用户对该视频的收藏状态。
bool HandleFavorite(std::int64_t video_id, douyin::Store& store,
                    const douyin::HttpRequest& request,
                    douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    // 先确认视频存在：否则收藏表里会积累指向已删除视频的孤儿行。
    douyin::StoredVideo video;
    std::string error;
    if (!store.FindVideo(video_id, &video, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询收藏目标失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "视频不存在");
    }
    const int changed = store.ToggleFavorite(video_id, actor, &error);
    if (changed < 0) {
        std::fprintf(stderr, "[%s] 切换收藏失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    response->status = 200;
    response->body = std::string("{\"ok\":true,\"favorited\":") +
                     (changed > 0 ? "true" : "false") + "}";
    return true;
}

/// @brief `GET /api/video/favorites`：当前用户的收藏列表（按收藏时间倒序）。
bool HandleFavorites(douyin::Store& store, const douyin::HttpRequest& request,
                     douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    const std::int64_t limit = ParseLimit(request);
    std::vector<douyin::StoredVideo> videos;
    std::string error;
    if (!store.ListFavorites(actor, limit, &videos, &error)) {
        std::fprintf(stderr, "[%s] 查询收藏列表失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (!store.FillVideoLikes(actor, &videos, &error)) {
        std::fprintf(stderr, "[%s] 回填收藏列表点赞态失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    std::string body = "{\"ok\":true,\"items\":[";
    for (std::size_t i = 0; i < videos.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"video\":" + douyin::ToJson(PublicVideo(videos[i])) + "}";
    }
    body += "],\"total\":" + std::to_string(videos.size()) + "}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/**
 * @brief 软删除视频：`DELETE /api/video/:id`（仅作者）。
 *
 * @param store [in,out] 已打开的存储——写 videos.deleted_at。
 * @param request [in] HTTP 请求——借用；只读 Authorization。
 * @param id [in] 视频 id；数据库按 id + user_id 软删。
 * @param response [out] 出站响应——401/403/404/500，成功 200。
 * @return 总是 true；调用方**不要**再回 404。
 *
 * ## 步骤
 * step1. 鉴权；step2. 查未删除记录并校验作者（非作者 403）；
 * step3. 写 deleted_at（唯一提交点，重复删除返回 0 → 404，幂等）；
 * step4. 返回 200。
 *
 * ## 并发/失败语义
 * - 调用约束：单线程事件循环内调用；
 * - 不变量：返回 200 时该视频对所有读接口不可见（详情/feed/主页/搜索 404）；
 * - 文件保留：软删除不删 mp4/封面，便于恢复与分享链接短时兜底。
 */
bool HandleDelete(douyin::Store& store, const douyin::HttpRequest& request,
                  std::int64_t id, douyin::HttpResponse* response) {
    std::int64_t actor = 0;
    if (!RequireToken(request, response, &actor)) {
        return true;
    }
    douyin::StoredVideo stored;
    std::string error;
    if (!store.FindVideo(id, &stored, &error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "[%s] 查询视频失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        return Fail(response, 404, "视频不存在");
    }
    if (stored.user_id != actor) {
        return Fail(response, 403, "只有作者可以删除");
    }
    const int changed = store.DeleteVideo(id, actor, &error);
    if (changed < 0) {
        std::fprintf(stderr, "[%s] 删除视频失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    if (changed == 0) {
        return Fail(response, 404, "视频不存在");
    }
    // 软删除保留 mp4 与封面文件：撤下内容不等于立刻销毁资产，
    // 恢复/审计都还需要；物理清理交给离线任务。
    response->status = 200;
    response->body = "{\"ok\":true,\"deleted\":true,\"id\":" +
                     std::to_string(id) + "}";
    return true;
}


/// @brief 一个话题的聚合统计（话题名 + 视频数 + 去重作者）。
struct TagSummary {
    std::string tag;
    std::int64_t video_count{0};
    std::vector<std::int64_t> users;
};

/**
 * @brief 把 percent-encoded 的路径片段还原成 UTF-8 文本。
 *
 * @param text [in] 原始路径片段（如 `%E7%88%AC%E5%B1%B1`）——借用。
 * @return 解码结果；非法 `%XX` 原样保留，不抛异常。
 *
 * ## 定位
 * 浏览器对 `/api/tag/爬山` 会发 `%E7%88%AC...`；不解码的话话题永远
 * 匹配不上。错了会怎样：中文话题一律 0 条视频。
 */
std::string PercentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            int hi = -1;
            int lo = -1;
            const char a = text[i + 1];
            const char b = text[i + 2];
            if (a >= '0' && a <= '9') {
                hi = a - '0';
            } else if (a >= 'a' && a <= 'f') {
                hi = a - 'a' + 10;
            } else if (a >= 'A' && a <= 'F') {
                hi = a - 'A' + 10;
            }
            if (b >= '0' && b <= '9') {
                lo = b - '0';
            } else if (b >= 'a' && b <= 'f') {
                lo = b - 'a' + 10;
            } else if (b >= 'A' && b <= 'F') {
                lo = b - 'A' + 10;
            }
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        if (text[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

/// @brief 解析正整数 query 参数；缺省或非法回退 `fallback`，再夹紧到区间。
std::int64_t QueryInt(const douyin::HttpRequest& request, std::string_view name,
                      std::int64_t fallback, std::int64_t min_value,
                      std::int64_t max_value) {
    const std::string text = douyin::QueryParam(request, name);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str()) {
        return fallback;
    }
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return static_cast<std::int64_t>(value);
}

/**
 * @brief 扫全库视频，按话题聚合成 `tag -> TagSummary`。
 *
 * @param store [in,out] 存储；只读查询。
 * @param stats [out] 先清空再填。
 * @param error [out] 失败时写错误文本。
 * @return 成功返回 true。
 *
 * ## 步骤
 * step1. 取全部视频（demo 几十条，全表扫描可接受）；
 * step2. 每条视频调 `ExtractTags`（与卡片展示同一函数，保证口径一致）；
 * step3. 按 tag 累计视频数并去重作者 id。
 *
 * ## 并发/失败语义
 * - 调用约束：单线程事件循环内调用；
 * - 失败语义：数据库错误返回 false，调用方回 500。
 */
bool LoadTagStats(douyin::Store& store,
                  std::map<std::string, TagSummary>* stats,
                  std::string* error) {
    stats->clear();
    std::vector<douyin::StoredVideo> videos;
    if (!store.ListAllVideos(&videos, error)) {
        return false;
    }
    for (const douyin::StoredVideo& video : videos) {
        for (const std::string& tag : douyin::ExtractTags(video.title)) {
            TagSummary& summary = (*stats)[tag];
            summary.tag = tag;
            ++summary.video_count;
            if (std::find(summary.users.begin(), summary.users.end(),
                          video.user_id) == summary.users.end()) {
                summary.users.push_back(video.user_id);
            }
        }
    }
    return true;
}

/**
 * @brief `GET /api/tag/list?limit=20`：热门话题（按视频数降序）。
 *
 * @param store [in,out] 存储。
 * @param request [in] HTTP 请求；只读 query `limit`（默认 20，夹紧 1~100）。
 * @param response [out] 200 + `{"ok":true,"tags":[...]}`。
 * @return 总是 true。
 */
bool HandleTagList(douyin::Store& store, const douyin::HttpRequest& request,
                   douyin::HttpResponse* response) {
    std::map<std::string, TagSummary> stats;
    std::string error;
    if (!LoadTagStats(store, &stats, &error)) {
        std::fprintf(stderr, "[%s] 话题聚合失败: %s\n", kServiceName, error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    std::vector<TagSummary> list;
    list.reserve(stats.size());
    for (const auto& entry : stats) {
        list.push_back(entry.second);
    }
    std::sort(list.begin(), list.end(),
              [](const TagSummary& a, const TagSummary& b) {
                  if (a.video_count != b.video_count) {
                      return a.video_count > b.video_count;
                  }
                  return a.tag < b.tag;
              });
    const std::int64_t limit = QueryInt(request, "limit", 20, 1, 100);
    std::string body = "{\"ok\":true,\"tags\":[";
    const std::size_t count = static_cast<std::size_t>(limit) < list.size()
                                  ? static_cast<std::size_t>(limit)
                                  : list.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            body += ',';
        }
        body += "{\"tag\":\"" + douyin::JsonEscape(list[i].tag) +
                "\",\"video_count\":" + std::to_string(list[i].video_count) +
                ",\"participants\":" + std::to_string(list[i].users.size()) + "}";
    }
    body += "]}";
    response->status = 200;
    response->body = std::move(body);
    return true;
}

/**
 * @brief `GET /api/tag/<tag>?page=1&page_size=20`：话题下的视频分页。
 *
 * @param store [in,out] 存储。
 * @param raw_tag [in] 路径里的原始话题（percent-encoded）——借用。
 * @param request [in] HTTP 请求；只读 query `page` / `page_size`。
 * @param response [out] 200 + 话题信息与 `items`；空话题返回 0 条而非 404。
 * @return 总是 true。
 *
 * ## 步骤
 * step1. 解码并校验话题名（空 -> 400）；
 * step2. 全库过滤 `ExtractTags` 命中该话题的视频；
 * step3. 切片分页并序列化（视频结构复用 video JSON，含 tags）。
 *
 * ## 失败语义
 * - 数据库错误回 500；不存在的 tag 是合法空结果（200）。
 */
bool HandleTagDetail(douyin::Store& store, std::string_view raw_tag,
                     const douyin::HttpRequest& request,
                     douyin::HttpResponse* response) {
    const std::string tag = PercentDecode(raw_tag);
    if (tag.empty() || tag.size() > 60) {
        return Fail(response, 400, "话题名非法");
    }
    std::vector<douyin::StoredVideo> videos;
    std::string error;
    if (!store.ListAllVideos(&videos, &error)) {
        std::fprintf(stderr, "[%s] 查询话题视频失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    // 拉黑过滤：话题页也是内容列表，被拉黑作者的视频同样不出现。
    std::vector<std::int64_t> blocked_ids;
    const std::int64_t viewer = ViewerId(request);
    if (viewer > 0) {
        std::vector<douyin::StoredBlock> blocks;
        if (!store.ListBlocks(viewer, &blocks, &error)) {
            std::fprintf(stderr, "[%s] 查询黑名单失败: %s\n", kServiceName,
                         error.c_str());
            return Fail(response, 500, "数据库错误");
        }
        for (const douyin::StoredBlock& block : blocks) {
            blocked_ids.push_back(block.blocked_id);
        }
    }
    std::vector<douyin::StoredVideo> matched;
    std::vector<std::int64_t> users;
    for (const douyin::StoredVideo& video : videos) {
        if (std::find(blocked_ids.begin(), blocked_ids.end(), video.user_id) !=
            blocked_ids.end()) {
            continue;
        }
        const std::vector<std::string> tags = douyin::ExtractTags(video.title);
        if (std::find(tags.begin(), tags.end(), tag) == tags.end()) {
            continue;
        }
        matched.push_back(video);
        if (std::find(users.begin(), users.end(), video.user_id) == users.end()) {
            users.push_back(video.user_id);
        }
    }
    const std::int64_t page = QueryInt(request, "page", 1, 1, 100000);
    const std::int64_t page_size = QueryInt(request, "page_size", 20, 1, 50);
    const std::size_t total = matched.size();
    const std::size_t begin = static_cast<std::size_t>((page - 1) * page_size);
    std::string body = "{\"ok\":true,\"tag\":\"" + douyin::JsonEscape(tag) +
                       "\",\"video_count\":" + std::to_string(total) +
                       ",\"participants\":" + std::to_string(users.size()) +
                       ",\"page\":" + std::to_string(page) +
                       ",\"page_size\":" + std::to_string(page_size) +
                       ",\"total\":" + std::to_string(total) + ",\"items\":[";
    if (begin < total) {
        const std::size_t end = std::min(total, begin + static_cast<std::size_t>(page_size));
        for (std::size_t i = begin; i < end; ++i) {
            if (i != begin) {
                body += ',';
            }
            std::string author;
            if (!store.FindUserName(matched[i].user_id, &author, &error) &&
                !error.empty()) {
                std::fprintf(stderr, "[%s] 查询作者失败: %s\n", kServiceName,
                             error.c_str());
                return Fail(response, 500, "数据库错误");
            }
            body += "{\"video\":" +
                    douyin::ToJson(PublicVideo(matched[i])) +
                    ",\"author\":\"" + douyin::JsonEscape(author) + "\"}";
        }
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
    // 分享落地页：`/v/<id>`（公网前缀下是 `/<prefix>/v/<id>`）直出独立 HTML。
    // 必须放在 `/api/...` 各分支之前？不：`/api/video/12` 不含 `/v/` 子串，
    // 这里只按 `/v/` 判定，不会截走 API 路由。
    if ((request.method == "GET" || request.method == "HEAD") &&
        request.path.find("/v/") != std::string::npos) {
        return HandleSharePage(store, request, request.path, response);
    }

    if (request.path == "/api/tag/list") {
        if (request.method == "GET") {
            return HandleTagList(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    {
        constexpr std::string_view kTagPrefix = "/api/tag/";
        if (request.path.size() > kTagPrefix.size() &&
            request.path.compare(0, kTagPrefix.size(), kTagPrefix) == 0) {
            if (request.method != "GET") {
                return Fail(response, 405, "方法不允许");
            }
            return HandleTagDetail(
                store,
                std::string_view(request.path).substr(kTagPrefix.size()),
                request, response);
        }
    }
    if (request.path == "/api/video") {
        if (request.method == "POST") {
            return HandleCreate(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/video/mine") {
        if (request.method == "GET") {
            return HandleMine(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }

    if (request.path == "/api/video/favorites") {
        if (request.method == "GET") {
            return HandleFavorites(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    {
        std::int64_t favorite_id = 0;
        if (ParseFavoritePath(request.path, &favorite_id)) {
            if (request.method == "POST") {
                return HandleFavorite(favorite_id, store, request, response);
            }
            return Fail(response, 405, "方法不允许");
        }
    }

    if (request.path == "/api/video/upload/init") {
        if (request.method == "POST") {
            return HandleUploadInit(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/video/upload/chunk") {
        if (request.method == "POST") {
            return HandleUploadChunk(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    if (request.path == "/api/video/upload/finish") {
        if (request.method == "POST") {
            return HandleUploadFinish(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }
    {
        constexpr std::string_view kStatusPrefix = "/api/video/upload/status/";
        if (request.path.size() > kStatusPrefix.size() &&
            request.path.compare(0, kStatusPrefix.size(), kStatusPrefix.data(),
                                 kStatusPrefix.size()) == 0) {
            if (request.method == "GET") {
                return HandleUploadStatus(
                    store, request, request.path.substr(kStatusPrefix.size()),
                    response);
            }
            return Fail(response, 405, "方法不允许");
        }
    }

    if (request.path == "/api/video/cover") {
        if (request.method == "POST") {
            return HandleCoverUpload(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }

    if (request.path == "/api/video/upload") {
        if (request.method == "POST") {
            return HandleUpload(store, request, response);
        }
        return Fail(response, 405, "方法不允许");
    }

    std::int64_t id = 0;
    if (!ParseVideoPath(request.path, &id)) {
        return false;
    }
    if (request.method == "GET") {
        return HandleDetail(store, request, id, response);
    }
    if (request.method == "DELETE") {
        return HandleDelete(store, request, id, response);
    }
    return Fail(response, 405, "方法不允许");
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
    // 就绪探针：SQLite 连接可读（`SELECT 1`）才算 ready；只读，不改任何数据。
    // 表被删/连接失效时返回 false，`/health/ready` 回 503。
    server.SetReadinessProbe([&store](std::string* detail) {
        if (!store.Ok() || store.Database() == nullptr) {
            if (detail != nullptr) {
                *detail = "db not open";
            }
            return false;
        }
        // 只读查询核心表并真正执行一次（`Query` 只 prepare，`Next` 才 step）：
        // 连接失效或 videos 表被删都会在 Err() 中体现 → `/health/ready` 503。
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
    const std::string web_dir = WebDir();
    std::printf("[%s] 监听 0.0.0.0:%u db=%s web=%s\n", kServiceName,
                static_cast<unsigned>(port), db_path.c_str(), web_dir.c_str());
    std::fflush(stdout);

    server.ServeForever([&store](const douyin::HttpRequest& request,
                                 douyin::HttpResponse* response) {
        return Handle(store, request, response);
    });
    return 0;
}
