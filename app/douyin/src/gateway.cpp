/**
 * @file gateway.cpp
 * @brief 抖音 C8：基于 go2cpp::app::gateway 的统一 HTTP 网关（28080）。
 *
 * @details 通用 Gateway 组件负责路由匹配、中间件链、上游转发、健康聚合；
 *          本文件只做抖音业务接线：
 *          - 六个业务前缀 → 六个上游服务（29101~29106）；
 *          - video / like / comment / live 的写方法（POST/DELETE/PATCH）注册
 *            require_auth 路由，由组件内置 AuthMiddleware 调用
 *            douyin_auth::VerifyToken 校验 Bearer token，失败回 401；
 *          - 读方法（GET/HEAD）为公开读（详情、feed、评论列表、房间列表、
 *            搜索），不要求网关鉴权；
 *          - /health、/api/health/all 由组件内建，无需注册；
 *          - GET /videos/<name> 由本地 handler 直接返回 web/videos 下的 mp4。
 *
 * @note 路由按「业务前缀」组织；读/写语义不同的前缀按方法掩码拆成两条
 *       RouteSpec（同一上游），避免把公开读误挡在 401 之后。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_auth.hpp"
#include "douyin_home_page.hpp"
#include "go2cpp/app/gateway.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

using go2cpp::app::gateway::Config;
using go2cpp::app::gateway::Gateway;
using go2cpp::app::gateway::Handler;
using go2cpp::app::gateway::Matcher;
using go2cpp::app::gateway::MethodMask;
using go2cpp::app::gateway::Next;
using go2cpp::app::gateway::PathParams;
using go2cpp::app::gateway::Request;
using go2cpp::app::gateway::Response;
using go2cpp::app::gateway::Result;
using go2cpp::app::gateway::RouteSpec;
using go2cpp::http::Method;
using go2cpp::http::Status;
namespace method_mask = go2cpp::app::gateway::method_mask;

/// @brief 网关默认监听端口。
constexpr std::uint16_t kDefaultPort = 28080;
/// @brief user 服务端口。
constexpr std::uint16_t kUserPort = 29101;
/// @brief video 服务端口。
constexpr std::uint16_t kVideoPort = 29102;
/// @brief feed 服务端口。
constexpr std::uint16_t kFeedPort = 29103;
/// @brief interaction 服务端口（like + comment）。
constexpr std::uint16_t kInteractionPort = 29104;
/// @brief live 服务端口。
constexpr std::uint16_t kLivePort = 29105;
/// @brief search 服务端口。
constexpr std::uint16_t kSearchPort = 29106;
/// @brief 服务名（日志用）。
constexpr const char* kServiceName = "gateway";
/// @brief 静态资源根目录：build.sh 用 -DDOUYIN_WEB_DIR 注入绝对路径，
///        未定义时退回相对工作目录的 `web`（仅本地手工运行时的兜底）。
#ifndef DOUYIN_WEB_DIR
#define DOUYIN_WEB_DIR "web"
#endif
constexpr const char* kWebDir = DOUYIN_WEB_DIR;
/// @brief 单个静态视频的大小上限（64 MiB），防止误放超大文件撑爆内存。
constexpr std::uint64_t kMaxVideoBytes = 64ULL * 1024 * 1024;
/// @brief 读方法掩码：GET / HEAD。
constexpr MethodMask kReadMask = method_mask::kGet | method_mask::kHead;
/// @brief 写方法掩码：POST / DELETE / PATCH（需要 Bearer 鉴权）。
constexpr MethodMask kWriteMask =
    method_mask::kPost | method_mask::kDelete | method_mask::kPatch;

/// @brief 信号处理只置位；由监控线程在普通上下文调用 Gateway::Stop()。
volatile std::sig_atomic_t g_stop_requested = 0;

/// @brief 记录停止请求（async-signal-safe）。
void RequestStop(int /*signal*/) { g_stop_requested = 1; }

/// @brief 上游地址文本 `127.0.0.1:<port>`。
std::string Upstream(std::uint16_t port) {
    return "127.0.0.1:" + std::to_string(port);
}

/**
 * @brief 校验 `Authorization: Bearer <token>`。
 * @details 成功时把 token 中的 user_id 写入 `X-User-Id`（组件负责注入转发头）；
 *          失败时写 error，组件据此返回 401。
 */
bool VerifyBearer(const Request& request, std::string* user_id, std::string* error) {
    constexpr std::string_view kPrefix = "Bearer ";
    const std::string authorization = request.header.get("Authorization");
    if (authorization.size() <= kPrefix.size() ||
        authorization.compare(0, kPrefix.size(), kPrefix.data(), kPrefix.size()) != 0) {
        *error = "missing bearer token";
        return false;
    }
    const std::string token = authorization.substr(kPrefix.size());
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    douyin::TokenInfo info;
    std::string reason;
    if (!douyin::VerifyToken(token, static_cast<std::int64_t>(now), &info, &reason)) {
        *error = reason.empty() ? "invalid token" : reason;
        return false;
    }
    *user_id = std::to_string(info.user_id);
    return true;
}


/**
 * @brief 首页匹配器：只接管 GET / HEAD 的 `/` 与 `/index.html`。
 *
 * ## 定位
 * 网关 Dispatch 先查内建 `/health`，再按「自定义 Matcher > 静态 > 参数 > 通配」
 * 选路由；本类作为自定义 Matcher 注册，保证根路径不会被上游通配路由吞掉。
 *
 * ## 并发/失败语义
 * - 调用约束：worker 线程并发调用，本类无状态，可安全共享；
 * - 不变量：路径为 `/`、`/index.html` 或 `/v/<数字>`（可带尾斜杠）、
 *   且方法为 GET/HEAD 时返回 true。
 */
class HomeMatcher final : public Matcher {
public:
    /// @brief 命中首页路径；本路由无参数，不写入 params。
    /// @param request [in] 入站请求——借用，调用期间有效；读取 method 与 path_view()。
    /// @param params [out] 参数表——本路由不写入（保持调用方给定内容）。
    /// @return true 表示由首页 handler 接管；false 交给后续路由继续匹配。
    bool Match(const Request& request, PathParams* /*params*/) const override {
        // step1: 只有 GET/HEAD 需要页面；其余方法回落 404，避免误导。
        if (request.method != Method::Get && request.method != Method::Head) {
            return false;
        }
        // step2: 完全相等匹配——`/` 与 `/index.html` 两种入口都指向同一页面。
        const std::string_view path = request.path_view();
        if (path == "/" || path == "/index.html") {
            return true;
        }
        // step3: 短视频直达 `/v/<数字>`（允许一个尾斜杠），页面按 pathname 解析 id。
        if (path.size() > 3 && path.compare(0, 3, "/v/") == 0) {
            std::size_t i = 3;
            while (i < path.size() && path[i] >= '0' && path[i] <= '9') {
                ++i;
            }
            if (i > 3 && (i == path.size() ||
                          (i + 1 == path.size() && path[i] == '/'))) {
                return true;
            }
        }
        return false;
    }
};

/// @brief 首页 handler：本地填写 HTML，不产生上游请求。
/// @param request [in] 入站请求——借用；本页面与请求内容无关，未使用。
/// @param response [out] 出站响应——成功时被整体覆盖为 200 + HTML。
/// @return 总是 Ok；响应内容由 response 携带。
Result<void> ServeHome(Request& /*request*/, Response& response) {
    response.status = Status::OK;
    response.header.set("Content-Type", "text/html; charset=utf-8");
    response.header.set("Cache-Control", "no-store");
    response.body = douyin::kHomePageHtml;
    return Result<void>::Ok();
}

/// @brief 上传体积上限（32 MiB），与 video_service 的 kMaxUploadBytes 保持一致。
constexpr std::size_t kMaxUploadBytes = 32 * 1024 * 1024;

/**
 * @brief 上传体中间件：`POST /api/video/upload` 超过 32 MiB 在网关直接 400。
 *
 * @param request [in,out] 入站请求——借用；只读 method / path / body。
 * @param response [out] 出站响应——超限时写 400 JSON；放行时保持原值。
 * @param next [in] 中间件链的下一环——**必须**在放行时调用且只调用一次。
 * @return 总是 Ok；超限时链被截断，响应已填好。
 *
 * ## 定位
 * 网关是视频字节的第一道闸门：请求体已被网关完整读入，若超限仍转发，
 * 上游 video_service 会在读满上限后直接回包并关闭连接，客户端剩余字节
 * 触发 RST，最终表现为 502 而不是 400。这里提前拒绝保证语义稳定。
 */
Result<void> LimitUploadSize(Request& request, Response& response,
                             const Next& next) {
    // step1: 只拦上传路径，其余请求原样交给后续路由。
    if (request.method != Method::Post ||
        request.path_view() != "/api/video/upload") {
        return next();
    }
    // step2: 超限直接 400，不再产生上游连接。
    if (request.body.size() > kMaxUploadBytes) {
        response.status = Status::BadRequest;
        response.header.set("Content-Type", "application/json; charset=utf-8");
        response.body =
            "{\"ok\":false,\"error\":\"视频不能超过 32 MiB\",\"code\":400}";
        return Result<void>::Ok();
    }
    return next();
}

/// @brief 注册首页路由（自定义 Matcher + 本地 Handler）；失败时打印并返回 false。
bool AddHomeRoute(Gateway* gateway) {
    const auto result = gateway->AddRoute(
        std::make_shared<HomeMatcher>(), std::make_shared<Handler>(&ServeHome));
    if (!result.ok()) {
        std::fprintf(stderr, "[%s] 注册首页路由失败：%s\n", kServiceName,
                     result.error().c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 静态视频：GET/HEAD /videos/<name> → <web>/videos/<name>
// 与 HomeMatcher 相同，用自定义 Matcher 注册，避免被 /api/** 通配路由吞掉。
// ---------------------------------------------------------------------------

/// @brief 静态视频的 URL 前缀（tail 只允许单段文件名）。
constexpr std::string_view kVideoUrlPrefix = "/videos/";

/**
 * @brief 判断视频文件名是否只由安全字符组成（防目录穿越）。
 *
 * @param name [in] 去掉前缀后的路径尾段——借用，调用期间有效；不可为 nullptr 语义（值传递）。
 *        取值：非空、长度 ≤ 128 的字节序列；含 `/`、`\`、`%`、空白一律非法。
 * @return
 * - true：文件名安全，调用方**必须**再与固定目录拼接后打开；
 * - false：非法输入，调用方**不要**打开文件，应按 404 处理。
 *
 * ## 定位
 * 唯一的路径注入防线：一旦放过 `..` 或分隔符，`/videos/../../etc/passwd`
 * 会读到任意文件；因此这里做白名单而不是黑名单。
 *
 * ## 步骤
 * step1. 拒绝空名、超长名与 `.` / `..` 两个特殊段；
 * step2. 逐字节白名单 `[A-Za-z0-9_.-]`，任何分隔符/编码符直接判否。
 *
 * ## 并发/失败语义
 * - 调用约束：无状态纯函数，可在任意 worker 线程并发调用；
 * - 不变量：返回 true 时字符串中不含路径分隔符，拼接到固定目录不会逃逸。
 */
bool IsSafeVideoName(std::string_view name) {
    // step1: 空名/超长/特殊段直接拒绝（`..` 完全由白名单字符组成，必须单独挡）。
    if (name.empty() || name.size() > 128) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    // step2: 白名单校验，排除 `/`、`\`、`%` 等可构造穿越或二次解码的字符。
    for (const char ch : name) {
        const bool ascii_lower = ch >= 'a' && ch <= 'z';
        const bool ascii_upper = ch >= 'A' && ch <= 'Z';
        const bool ascii_digit = ch >= '0' && ch <= '9';
        if (!ascii_lower && !ascii_upper && !ascii_digit && ch != '_' && ch != '-' &&
            ch != '.') {
            return false;
        }
    }
    return true;
}

/**
 * @brief 从请求路径中提取 `/videos/<name>` 的文件名。
 *
 * @param path [in] 请求路径（不含 query）——借用，调用期间有效。
 * @param name [out] 成功时写入文件名视图；失败时保持原值，生命周期依附于 path。
 * @return
 * - true：调用方**可以**用该名字拼路径并打开文件；
 * - false：请求不是静态视频，调用方**不要**读写文件，交给后续路由或回 404。
 *
 * ## 定位
 * Matcher 与 Handler 共用同一个解析入口，保证「是否命中」与「读哪个文件」
 * 永远一致；两边各写一份解析迟早会出现匹配了却读错文件。
 *
 * ## 步骤
 * step1. 前缀必须严格等于 `/videos/`（大小写敏感，避免大小写绕过）；
 * step2. 尾段必须非空且通过白名单校验。
 *
 * ## 并发/失败语义
 * - 纯函数、无状态；返回 true 时 `*name` 只指向 path 的内部区间。
 */
bool ExtractVideoName(std::string_view path, std::string_view* name) {
    // step1: 前缀判断放在 size 检查之后，避免 substr 越界。
    if (path.size() <= kVideoUrlPrefix.size()) {
        return false;
    }
    if (path.compare(0, kVideoUrlPrefix.size(), kVideoUrlPrefix) != 0) {
        return false;
    }
    // step2: 尾段白名单（内部会拒绝空串与 `..`）。
    const std::string_view tail = path.substr(kVideoUrlPrefix.size());
    if (!IsSafeVideoName(tail)) {
        return false;
    }
    *name = tail;
    return true;
}

/// @brief 静态视频匹配器：只接管 GET / HEAD 的 `/videos/<安全文件名>`。
/**
 * ## 定位
 * 与 HomeMatcher 一样注册为自定义 Matcher，优先级高于静态/参数/通配路由，
 * 确保 `/videos/*` 不会被 `/api/**` 通配吞掉而误转发到上游。
 *
 * ## 并发/失败语义
 * - 无状态，worker 线程并发调用安全；
 * - 仅当方法为 GET/HEAD 且路径通过 ExtractVideoName 时返回 true。
 */
class VideoMatcher final : public Matcher {
public:
    /// @brief 命中静态视频路径。
    /// @param request [in] 入站请求——借用，调用期间有效；读取 method 与 path_view()。
    /// @param params [out] 不写入（文件名在 handler 内重新解析，避免跨层传递）。
    /// @return true 表示由 ServeVideo 处理；false 交给后续路由。
    bool Match(const Request& request, PathParams* /*params*/) const override {
        if (request.method != Method::Get && request.method != Method::Head) {
            return false;
        }
        std::string_view name;
        return ExtractVideoName(request.path_view(), &name);
    }
};

/**
 * @brief 静态视频 handler：从磁盘读取 mp4 并整块写入响应体。
 *
 * @param request [in] 入站请求——借用；只读 path_view()（HEAD 也走这里）。
 * @param response [out] 出站响应——成功为 200 + `video/mp4` + 完整字节；
 *        文件不存在为 404；读取失败为 500。失败时只写状态与短文本。
 * @return 总是 Ok（错误通过 response.status 表达）；网络错误由网关层处理。
 *
 * ## 定位
 * 唯一的视频字节出口：路径校验（ExtractVideoName）、大小上限与 Content-Length
 * 都在这里收口；一旦拼路径绕过校验就会变成任意文件读取。
 *
 * ## 步骤
 * step1. 重新解析并校验文件名，非法直接 404（Matcher 已挡一次，这里防御调用方变化）；
 * step2. 以 ate 模式打开并取文件大小，不存在 → 404，超限/负值 → 500；
 * step3. 预分配后整块 read，长度不符 → 500；
 * step4. 填 200、Content-Type、Content-Length、Cache-Control 与 body。
 *
 * ## 并发/失败语义
 * - 调用约束：worker 线程并发；每次请求独立 ifstream，无共享状态；
 * - 原子性：响应在内存中构造完整后由网关一次性写出；
 * - 失败语义：不抛异常；失败响应 body 非空、无文件句柄泄漏（RAII）；
 * - 不变量：返回 200 时 body.size() 与磁盘文件字节数完全一致。
 */
Result<void> ServeVideo(Request& request, Response& response) {
    // step1: 路径再校验；非法输入按「不存在」处理，不泄露目录结构。
    std::string_view name;
    if (!ExtractVideoName(request.path_view(), &name)) {
        response.status = Status::NotFound;
        response.header.set("Content-Type", "text/plain; charset=utf-8");
        response.body = "404 video not found\n";
        return Result<void>::Ok();
    }
    // step2: ate 模式把读指针放到末尾，tellg 即文件大小；失败即不存在/无权限。
    const std::string path = std::string(kWebDir) + "/videos/" + std::string(name);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        response.status = Status::NotFound;
        response.header.set("Content-Type", "text/plain; charset=utf-8");
        response.body = "404 video not found\n";
        return Result<void>::Ok();
    }
    const std::streamoff file_size = input.tellg();
    if (file_size < 0 || static_cast<std::uint64_t>(file_size) > kMaxVideoBytes) {
        response.status = Status::InternalError;
        response.header.set("Content-Type", "text/plain; charset=utf-8");
        response.body = "500 video unreadable\n";
        return Result<void>::Ok();
    }
    // step3: 预分配后一次读完，避免逐字符迭代器的额外拷贝。
    std::string content(static_cast<std::size_t>(file_size), '\0');
    input.seekg(0, std::ios::beg);
    if (file_size > 0) {
        input.read(content.data(), static_cast<std::streamsize>(file_size));
        if (input.gcount() != static_cast<std::streamsize>(file_size)) {
            response.status = Status::InternalError;
            response.header.set("Content-Type", "text/plain; charset=utf-8");
            response.body = "500 video unreadable\n";
            return Result<void>::Ok();
        }
    }
    // step4: 全量返回；HEAD 由网关序列化层去掉 body，但仍带正确的 Content-Length。
    response.status = Status::OK;
    response.header.set("Content-Type", "video/mp4");
    response.header.set("Content-Length", std::to_string(content.size()));
    response.header.set("Cache-Control", "public, max-age=3600");
    response.body = std::move(content);
    return Result<void>::Ok();
}

/// @brief 注册静态视频路由（自定义 Matcher + 本地 Handler）；失败时打印并返回 false。
bool AddVideoRoute(Gateway* gateway) {
    const auto result = gateway->AddRoute(
        std::make_shared<VideoMatcher>(), std::make_shared<Handler>(&ServeVideo));
    if (!result.ok()) {
        std::fprintf(stderr, "[%s] 注册静态视频路由失败：%s\n", kServiceName,
                     result.error().c_str());
        return false;
    }
    return true;
}

/// @brief 注册一条路由；失败时打印并返回 false。
bool AddRoute(Gateway* gateway, MethodMask methods, const char* pattern,
              std::uint16_t upstream_port, bool require_auth, int priority,
              const char* name,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
    RouteSpec spec;
    spec.method_mask = methods;
    spec.path_pattern = pattern;
    spec.upstream = Upstream(upstream_port);
    spec.require_auth = require_auth;
    spec.timeout = timeout;
    spec.priority = priority;
    spec.name = name;
    const auto result = gateway->AddRoute(spec);
    if (!result.ok()) {
        std::fprintf(stderr, "[%s] 注册路由 %s 失败：%s\n", kServiceName, pattern,
                     result.error().c_str());
        return false;
    }
    return true;
}

/// @brief 解析命令行端口（缺省 28080）；非法时退出。
std::uint16_t ParsePort(int argc, char** argv) {
    if (argc <= 1) {
        return kDefaultPort;
    }
    char* end = nullptr;
    const long value = std::strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || value <= 0 || value > 65535) {
        std::fprintf(stderr, "[%s] 非法端口：%s\n", kServiceName, argv[1]);
        std::exit(2);
    }
    return static_cast<std::uint16_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint16_t port = ParsePort(argc, argv);

    Config config;
    config.listen_host = "0.0.0.0";
    config.port = port;
    config.worker_threads = 8;
    config.max_queued_connections = 512;
    config.health_timeout = std::chrono::milliseconds(500);
    // 非空 auth 会让组件自动安装 AuthMiddleware。
    config.auth = VerifyBearer;
    config.access_log = false;

    auto created = Gateway::Create(config);
    if (!created.ok()) {
        std::fprintf(stderr, "[%s] 创建失败：%s\n", kServiceName,
                     created.error().c_str());
        return 1;
    }
    auto gateway = std::move(created.value());

    // 上传体积闸门：放在路由/转发之前，超 32 MiB 的请求不产生上游连接。
    gateway->Use(LimitUploadSize);

    if (!AddHomeRoute(gateway.get()) || !AddVideoRoute(gateway.get())) {
        return 1;
    }

    const bool routes_ok =
        AddRoute(gateway.get(), method_mask::kAll, "/api/user/**", kUserPort, false,
                 0, "user") &&
        AddRoute(gateway.get(), kReadMask, "/api/video/**", kVideoPort, false, 0,
                 "video-read") &&
        // 上传大文件需要更长超时（默认 1s 会让 10 MiB 上传在网关侧超时 -> 502）。
        AddRoute(gateway.get(), kWriteMask, "/api/video/**", kVideoPort, true, 10,
                 "video-write", std::chrono::milliseconds(120000)) &&
        AddRoute(gateway.get(), method_mask::kAll, "/api/feed/**", kFeedPort, false,
                 0, "feed") &&
        AddRoute(gateway.get(), kWriteMask, "/api/like/**", kInteractionPort, true,
                 10, "like") &&
        AddRoute(gateway.get(), kReadMask, "/api/comment/**", kInteractionPort,
                 false, 0, "comment-read") &&
        AddRoute(gateway.get(), kWriteMask, "/api/comment/**", kInteractionPort,
                 true, 10, "comment-write") &&
        AddRoute(gateway.get(), kReadMask, "/api/live/**", kLivePort, false, 0,
                 "live-read") &&
        AddRoute(gateway.get(), kWriteMask, "/api/live/**", kLivePort, true, 10,
                 "live-write") &&
        AddRoute(gateway.get(), method_mask::kAll, "/api/search/**", kSearchPort,
                 false, 0, "search");
    if (!routes_ok) {
        return 1;
    }

    std::printf(
        "[%s] 监听 0.0.0.0:%u，上游 user=%u video=%u feed=%u interaction=%u "
        "live=%u search=%u\n",
        kServiceName, static_cast<unsigned>(port), static_cast<unsigned>(kUserPort),
        static_cast<unsigned>(kVideoPort), static_cast<unsigned>(kFeedPort),
        static_cast<unsigned>(kInteractionPort), static_cast<unsigned>(kLivePort),
        static_cast<unsigned>(kSearchPort));
    std::fflush(stdout);

    std::signal(SIGINT, RequestStop);
    std::signal(SIGTERM, RequestStop);

    std::atomic<bool> finished{false};
    std::thread watcher([&gateway, &finished]() {
        while (!finished.load(std::memory_order_relaxed)) {
            if (g_stop_requested != 0) {
                gateway->Stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    gateway->Run();
    finished.store(true, std::memory_order_relaxed);
    watcher.join();

    std::printf("[%s] 已停止\n", kServiceName);
    std::fflush(stdout);
    return 0;
}
