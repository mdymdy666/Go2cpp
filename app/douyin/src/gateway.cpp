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
 *          - /health、/api/health/all 由组件内建，无需注册。
 *
 * @note 路由按「业务前缀」组织；读/写语义不同的前缀按方法掩码拆成两条
 *       RouteSpec（同一上游），避免把公开读误挡在 401 之后。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_auth.hpp"
#include "go2cpp/app/gateway.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

/// @brief 根路径页面（本地渲染，不转发任何上游服务）。
constexpr const char* kHomePageHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Go2Cpp 抖音 Demo</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 40px 20px; background: #0f1115; color: #e6e8ee;
         font: 15px/1.6 -apple-system, "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif; }
  main { max-width: 880px; margin: 0 auto; }
  h1 { font-size: 27px; margin: 0 0 6px; }
  h2 { font-size: 17px; margin: 30px 0 10px; color: #c7cedd; }
  .sub { color: #8b93a7; margin: 0; }
  table { width: 100%; border-collapse: collapse; background: #161a22;
          border: 1px solid #242a36; border-radius: 10px; overflow: hidden; }
  th, td { text-align: left; padding: 9px 14px; border-bottom: 1px solid #242a36; }
  th { color: #8b93a7; font-size: 13px; font-weight: 600; }
  tr:last-child td { border-bottom: 0; }
  code { background: #1d232e; border: 1px solid #2b3240; border-radius: 5px;
         padding: 1px 6px; font: 13px/1.5 "JetBrains Mono", Consolas, monospace; color: #9fd2ff; }
  pre { background: #161a22; border: 1px solid #242a36; border-radius: 10px;
        padding: 14px 16px; overflow-x: auto; }
  pre code { background: none; border: 0; padding: 0; color: #d6e2f5; }
  a { color: #6cb6ff; }
  .ok { color: #67d68b; }
</style>
</head>
<body>
<main>
  <h1>Go2Cpp 抖音 Demo</h1>
  <p class="sub">网关根路径 · 本地页面（不转发上游）· 监听端口 <code>28080</code></p>

  <h2>服务状态</h2>
  <table>
    <tr><th>服务</th><th>端口</th><th>状态</th><th>职责</th></tr>
    <tr><td>user</td><td><code>29101</code></td><td class="ok">已注册</td><td>注册 / 登录 / 资料 / 关注</td></tr>
    <tr><td>video</td><td><code>29102</code></td><td class="ok">已注册</td><td>视频发布 / 详情 / 删除</td></tr>
    <tr><td>feed</td><td><code>29103</code></td><td class="ok">已注册</td><td>推荐流 / 关注流</td></tr>
    <tr><td>interaction</td><td><code>29104</code></td><td class="ok">已注册</td><td>点赞 / 评论</td></tr>
    <tr><td>live</td><td><code>29105</code></td><td class="ok">已注册</td><td>直播间创建 / 列表</td></tr>
    <tr><td>search</td><td><code>29106</code></td><td class="ok">已注册</td><td>视频搜索</td></tr>
  </table>

  <h2>可用接口</h2>
  <table>
    <tr><th>方法</th><th>路径</th><th>说明</th></tr>
    <tr><td>GET</td><td><code>/api/feed?limit=3</code></td><td>推荐流（公开读）</td></tr>
    <tr><td>GET</td><td><code>/api/video/1</code></td><td>视频详情（公开读）</td></tr>
    <tr><td>GET</td><td><code>/api/health/all</code></td><td>六个上游的聚合健康</td></tr>
    <tr><td>POST</td><td><code>/api/user/register</code></td><td>注册（JSON：name / email / password）</td></tr>
  </table>

  <h2>试一下</h2>
  <p><a href="api/feed?limit=3">打开 /api/feed?limit=3</a>（相对路径：本机 / 与公网 /douyin/ 前缀下都可直接点开）</p>
  <pre><code>curl -s -X POST http://82.157.147.127:8443/douyin/api/user/register \
  -H 'Content-Type: application/json' \
  -d '{"name":"alice","email":"alice@example.com","password":"s3cret"}'</code></pre>
  <p class="sub">/api/** 请求由网关转发到对应上游；写操作需 <code>Authorization: Bearer &lt;token&gt;</code>。</p>
</main>
</body>
</html>)HTML";

/**
 * @brief 首页匹配器：只接管 GET / HEAD 的 `/` 与 `/index.html`。
 *
 * ## 定位
 * 网关 Dispatch 先查内建 `/health`，再按「自定义 Matcher > 静态 > 参数 > 通配」
 * 选路由；本类作为自定义 Matcher 注册，保证根路径不会被上游通配路由吞掉。
 *
 * ## 并发/失败语义
 * - 调用约束：worker 线程并发调用，本类无状态，可安全共享；
 * - 不变量：仅当路径完全等于 `/` 或 `/index.html` 且方法为 GET/HEAD 时返回 true。
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
        return path == "/" || path == "/index.html";
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
    response.body = kHomePageHtml;
    return Result<void>::Ok();
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

/// @brief 注册一条路由；失败时打印并返回 false。
bool AddRoute(Gateway* gateway, MethodMask methods, const char* pattern,
              std::uint16_t upstream_port, bool require_auth, int priority,
              const char* name) {
    RouteSpec spec;
    spec.method_mask = methods;
    spec.path_pattern = pattern;
    spec.upstream = Upstream(upstream_port);
    spec.require_auth = require_auth;
    spec.timeout = std::chrono::milliseconds(1000);
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

    if (!AddHomeRoute(gateway.get())) {
        return 1;
    }

    const bool routes_ok =
        AddRoute(gateway.get(), method_mask::kAll, "/api/user/**", kUserPort, false,
                 0, "user") &&
        AddRoute(gateway.get(), kReadMask, "/api/video/**", kVideoPort, false, 0,
                 "video-read") &&
        AddRoute(gateway.get(), kWriteMask, "/api/video/**", kVideoPort, true, 10,
                 "video-write") &&
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
