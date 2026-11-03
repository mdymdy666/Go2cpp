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
#include <string>
#include <string_view>
#include <thread>

namespace {

using go2cpp::app::gateway::Config;
using go2cpp::app::gateway::Gateway;
using go2cpp::app::gateway::MethodMask;
using go2cpp::app::gateway::Request;
using go2cpp::app::gateway::RouteSpec;
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
