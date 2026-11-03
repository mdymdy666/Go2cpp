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
 *          - DELETE /api/video/:id        删除（Bearer；非作者 403、不存在 404）
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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 29102;
constexpr const char* kServiceName = "video";
constexpr std::int64_t kDefaultPageSize = 20;
constexpr std::int64_t kMaxPageSize = 100;
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

    std::string error;
    const std::int64_t id = store.CreateVideo(actor, title, url, &error);
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
    const std::string title = UrlDecode(douyin::QueryParam(request, "title"));
    if (title.empty() || Utf8Length(title) > kMaxTitleChars) {
        return Fail(response, 400, "title 长度需为 1-80 字");
    }
    // step3: 体积检查。读取层超过上限会截断，Content-Length 是原始大小的唯一信号。
    if (request.body.empty()) {
        return Fail(response, 400, "请求体为空");
    }
    const std::string length_text = request.Header("content-length");
    if (!length_text.empty() &&
        std::strtoull(length_text.c_str(), nullptr, 10) > kMaxUploadBytes) {
        return Fail(response, 400, "视频不能超过 32 MiB");
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
    const std::int64_t id = store.CreateVideo(actor, title, url, &error);
    if (id <= 0) {
        std::filesystem::remove(path, ec);
        std::fprintf(stderr, "[%s] 建视频记录失败: %s\n", kServiceName,
                     error.c_str());
        return Fail(response, 500, "数据库错误");
    }
    douyin::StoredVideo stored;
    stored.id = id;
    stored.user_id = actor;
    stored.title = title;
    stored.url = url;
    stored.created_at = NowSeconds();
    response->status = 200;
    response->body = "{\"ok\":true,\"video\":" +
                     douyin::ToJson(PublicVideo(stored)) + "}";
    return true;
}

/// @brief 视频详情（附作者名）。
bool HandleDetail(douyin::Store& store, std::int64_t id,
                  douyin::HttpResponse* response) {
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

/// @brief 删除视频（仅作者）。
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
    response->status = 200;
    response->body = "{\"ok\":true,\"deleted\":true,\"id\":" +
                     std::to_string(id) + "}";
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
        return HandleDetail(store, id, response);
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
