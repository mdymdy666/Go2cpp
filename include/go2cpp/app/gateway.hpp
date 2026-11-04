#pragma once

/**
 * @file gateway.hpp
 * @brief 通用 HTTP 网关组件：注册式路由 + 中间件链 + 可插拔上游解析 + JSON 配置。
 *
 * @details 本组件只依赖 `go2cpp::http`（请求/响应模型、Parser、Client）与
 *          `go2cpp::net`（Socket），**不认识任何业务协议**（不引用任何业务
 *          协议/消息符号，不了解抖音/用户/视频等业务字段）。业务通过四类注入点接入：
 *
 *          1. **路由注册**：`AddRoute(RouteSpec)` 在运行期添加静态/参数/通配路由；
 *             `AddRoute(matcher, handler)` 允许业务完全自定义匹配与处理。
 *          2. **中间件**：`Use(Middleware)` 按调用顺序组成洋葱链；内置
 *             `AuthMiddleware` / `RateLimitMiddleware` / `AccessLogMiddleware`
 *             均为可选，业务可插入自己的鉴权、租户、灰度、重试中间件。
 *          3. **上游解析**：`SetResolver(Resolver)` 替换「服务名 → 地址」、
 *             多实例轮询/一致性哈希、配置中心拉取等策略；默认为
 *             `RouteSpec::upstream` 直连。
 *          4. **配置注入**：`LoadConfig(json)` / `LoadConfigFile(path)` 解析
 *             routes 数组并逐条 `AddRoute`；可重复调用做增量热更新
 *             （旧路由保留，同 method + pattern 覆盖）。
 *
 * ## 复用与自研决策
 * - 复用 `http::Parser`（请求解析、半包/chunked）、`http::Client`（上游转发，
 *   全局 deadline 覆盖 connect/send/recv）与 `net::Socket`（accept/recv/send）；
 * - **不复用** `bento::HttpServer`：它依赖 bento/io/sse/scheduler 整条链，
 *   且其响应序列化经 `http::Response::to_string`，对 429 等不在 `http::Status`
 *   枚举内的状态码会输出空 reason phrase，本库自己的 Parser 会判定
 *   `invalid start line`；网关必须能回 429；
 * - **不复用** `app/douyin/include/douyin_http.hpp`：那是抖音 demo 的最小
 *   解析器（只按 Content-Length 读 body、无参数路由、无超时），不适合通用组件；
 * - 因此连接循环与响应序列化在本组件内实现（约 60 行），换来任意状态码透传
 *   与零 bento 依赖。连接循环支持**同包流水线**：一次 `recv` 里的多个请求
 *   按序全部处理（不再只回第一个），缓冲耗尽后关闭连接；**不做跨包空闲
 *   keep-alive 等待**——空闲连接若继续占住 worker，会被 slowloris 放大成
 *   全站 DoS。每个请求另有 `Config::request_timeout` 的读取总时限。
 * - 协议边界：`Expect: 100-continue` 在请求头收齐后、读 body 前先回
 *   `100 Continue`；请求解析失败回 `400`；待处理连接队列满回 `503`；三者
 *   都以 `Connection: close` 收尾，客户端不会只看到静默 reset。
 *   **不做 101 Switching Protocols 透传**：WebSocket/HTTP Upgrade 需要 raw
 *   双向复制，超出本组件的 HTTP 代理路径；需要时由业务在自定义 Handler 中
 *   接管底层连接（本组件不暴露 Socket，因此当前版本不支持）。
 *
 * ## 路由匹配
 * - `path_pattern` 三种形态：
 *   - 静态：`/api/user/list`（完全相等）；
 *   - 参数：`/api/video/:id`（按 `/` 分段，`:` 段捕获到
 *     `Request::path_values`，键为参数名）；
 *   - 通配：以 `/api/` 为前缀再加 `**`（匹配 `/api` 本身与 `/api/` 前缀下的全部路径，
 *     剩余部分写入 `path_values["**"]`，不含前导 `/`）。
 * - 优先级：自定义 `Matcher`（业务显式接管）> 静态 > 参数 > 通配；
 *   同一类内按 `priority` 降序、再按注册顺序升序。
 * - 参数写入 `http::Request::path_values`（本组件对「path_params」的落地字段，
 *   `Request::path_value("id")` 可直接读取）。
 * - 方法匹配用位掩码（`RouteSpec::method_mask`）；`HEAD` 不会自动回退到
 *   `GET`，需要时显式把 HEAD 位加入掩码。
 *
 * ## 转发语义（内置路由）
 * - **路径保留**：默认原样透传 `Request::target`（含 path 与 query），
 *   不剥离 `path_pattern` 前缀；若需要给上游加公共前缀，用
 *   `Upstream::base_path`（追加到 target 之前）。
 * - 方法、body、query、`Authorization` 等端到端头原样透传；剔除
 *   hop-by-hop 头（`Host`/`Connection`/`Content-Length`/`Transfer-Encoding`/
 *   `TE`/`Trailer`/`Upgrade`/`Keep-Alive`/`Proxy-*`），由 HTTP 客户端重新生成
 *   framing。
 * - 响应状态码、响应体、端到端响应头原样回写；`Set-Cookie` 等重复头保留。
 * - 超时：`Upstream::timeout`（0 表示用路由超时）覆盖 connect/send/recv
 *   的**全局 deadline**；超时返回 504，连接失败返回 502。
 * - 错误格式统一为 `{"ok":false,"error":"...","code":<int>}`；内建
 *   `GET /health` 返回自身，`GET /api/health/all` 聚合所有已注册上游的
 *   `/health`（失败项标 `ok:false`）。
 *
 * ## 鉴权注入点
 * - `RouteSpec::require_auth` 为真时，网关先写入内部标记头
 *   `X-Go2Cpp-Gateway-Auth: 1`（客户端传入的同名头会被清除，防止伪造）；
 * - `AuthMiddleware` 只对带标记的请求执行注入的 `AuthFn`；
 * - `AuthFn` 成功时把 `user_id` 写入转发头 `X-User-Id`（覆盖客户端传值），
 *   失败时用返回的 `err` 作为 401 body：以 `{` 开头视为完整 JSON 原样透传，
 *   否则包装成统一错误格式。
 *
 * ## 如何扩展（示例）
 * @code
 * using namespace go2cpp::app::gateway;
 *
 * Config config;
 * config.port = 28080;
 * config.auth = [](const http::Request& req, std::string* user_id, std::string* err) {
 *     if (req.header.get("Authorization") != "Bearer demo") {
 *         *err = "invalid token";
 *         return false;
 *     }
 *     *user_id = "42";
 *     return true;
 * };
 *
 * auto created = Gateway::Create(config);
 * if (!created.ok()) return 1;
 * auto gateway = std::move(created.value());
 *
 * // 1) 注册式路由（运行期可加，无需重编译）
 * gateway->AddRoute(RouteSpec{method_mask::kGet, "/api/user/list",
 *                             "127.0.0.1:29101", false, std::chrono::milliseconds(500), 0});
 * gateway->AddRoute(RouteSpec{method_mask::kAll, "/api/video/:id",
 *                             "127.0.0.1:29102", true});
 *
 * // 2) 中间件：按 Use 顺序由外到内（洋葱）
 * gateway->Use(AccessLogMiddleware([](const http::Request& req,
 *                                     const http::Response& resp,
 *                                     std::chrono::microseconds cost) {
 *     std::fprintf(stderr, "%s -> %d (%lldus)\n", req.path().c_str(),
 *                  static_cast<int>(resp.status), static_cast<long long>(cost.count()));
 * }));
 *
 * // 3) 自定义 Matcher + Handler：按租户头路由，完全不写 path 规则
 * class TenantMatcher final : public Matcher {
 * public:
 *     bool Match(const http::Request& req, PathParams* params) const override {
 *         const std::string tenant = req.header.get("X-Tenant");
 *         if (tenant.empty()) return false;
 *         (*params)["tenant"] = tenant;
 *         return true;
 *     }
 * };
 * gateway->AddRoute(std::make_shared<TenantMatcher>(),
 *                   std::make_shared<Handler>([](http::Request&, http::Response& resp) {
 *                       resp.status = http::Status::OK;
 *                       resp.body = "tenant route";
 *                       return Result<void>::Ok();
 *                   }));
 *
 * // 4) 可插拔上游：同一 path 不改路由即可切换后端
 * gateway->SetResolver([](const http::Request& req) -> Result<Upstream> {
 *     if (req.path().rfind("/api/video/", 0) == 0) return Result<Upstream>::Ok({"127.0.0.1", 29112, "", {}});
 *     return Result<Upstream>::Err("no upstream");
 * });
 *
 * // 5) 配置热更新（旧路由保留，同 method + pattern 覆盖）
 * gateway->LoadConfig(R"({"routes":[{"path":"/api/feed/all","upstream":"127.0.0.1:29103","method":"GET"}]})");
 *
 * std::thread runner([&] { gateway->Run(); });
 * // ...
 * gateway->Stop();
 * runner.join();
 * @endcode
 *
 * @note 中间件在**路由选定之后**执行；想在路由前改写路径，请用自定义
 *       `Matcher`（它看到的是原始请求）或在 Resolver 中改写上游。
 * @note `Gateway::Run()` 阻塞当前线程；`Stop()` 可从任意线程调用，
 *       `Run()` 返回时 worker 已全部退出。
 *
 * @author go2cpp contributors
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "go2cpp/network/http.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::app::gateway {

/// @brief 本组件使用的请求/响应类型别名（来自 `go2cpp::http`）。
using Request = http::Request;
using Response = http::Response;

/// @brief 方法位掩码（bit = `http::Method` 的枚举值）。
using MethodMask = std::uint32_t;

/// @brief 方法掩码常量与构造辅助。
namespace method_mask {
/// @brief 零掩码（不匹配任何方法）。
inline constexpr MethodMask kNone = 0;
/// @brief 任意方法。
inline constexpr MethodMask kAll = 0xFFFFFFFFu;
inline constexpr MethodMask kGet = 1u << static_cast<unsigned>(http::Method::Get);
inline constexpr MethodMask kHead = 1u << static_cast<unsigned>(http::Method::Head);
inline constexpr MethodMask kPost = 1u << static_cast<unsigned>(http::Method::Post);
inline constexpr MethodMask kPut = 1u << static_cast<unsigned>(http::Method::Put);
inline constexpr MethodMask kDelete = 1u << static_cast<unsigned>(http::Method::Delete);
inline constexpr MethodMask kPatch = 1u << static_cast<unsigned>(http::Method::Patch);
inline constexpr MethodMask kOptions = 1u << static_cast<unsigned>(http::Method::Options);

/// @brief 取单个方法的掩码位。
inline constexpr MethodMask Of(http::Method method) noexcept {
    return 1u << static_cast<unsigned>(method);
}
}  // namespace method_mask

/**
 * @brief 不抛异常的结果载体（替代 C++23 `std::expected`，保持 C++17）。
 * @tparam T 成功值类型；`T` 必须可默认构造。
 */
template <typename T>
class Result final {
public:
    /// @brief 构造失败结果（默认错误信息为空）。
    Result() = default;
    /// @brief 构造成功结果。
    static Result Ok(T value);
    /// @brief 构造失败结果。
    static Result Err(std::string message);

    bool ok() const noexcept { return m_ok; }
    const T& value() const noexcept { return m_value; }
    T& value() noexcept { return m_value; }
    const std::string& error() const noexcept { return m_error; }

private:
    /// 成功标记；`Ok`/`Err` 写入，`ok()` 只读。
    bool m_ok{false};
    /// 成功值；仅 `ok()` 为真时有效。
    T m_value{};
    /// 失败原因；仅 `ok()` 为假时有效。
    std::string m_error;
};

template <typename T>
Result<T> Result<T>::Ok(T value) {
    Result<T> result;
    result.m_ok = true;
    result.m_value = std::move(value);
    return result;
}

template <typename T>
Result<T> Result<T>::Err(std::string message) {
    Result<T> result;
    result.m_ok = false;
    result.m_error = std::move(message);
    return result;
}

/// @brief `void` 特化：只有成功/失败与错误文本。
template <>
class Result<void> final {
public:
    /// @brief 默认构造为成功。
    Result() = default;
    static Result Ok() { return Result{}; }
    static Result Err(std::string message) {
        Result result;
        result.m_ok = false;
        result.m_error = std::move(message);
        return result;
    }

    bool ok() const noexcept { return m_ok; }
    const std::string& error() const noexcept { return m_error; }

private:
    /// 成功标记；默认成功（`Ok` 语义），`Err` 清零。
    bool m_ok{true};
    /// 失败原因。
    std::string m_error;
};

/**
 * @brief 解析后的上游地址；0 超时表示沿用路由超时。
 */
struct Upstream {
    /// 上游主机（IPv4 / IPv6 / 主机名）。
    std::string host{"127.0.0.1"};
    /// 上游端口；0 视为配置错误（Resolver 返回时应填有效端口）。
    std::uint16_t port{0};
    /// 追加在请求 target 之前的公共路径前缀（空表示原样透传）。
    std::string base_path;
    /// 上游超时；0 表示使用路由（或 Resolver 调用方）的默认超时。
    std::chrono::milliseconds timeout{0};
};

/// @brief 路由参数表：参数名 → 段原文（不做百分号解码）。
using PathParams = std::map<std::string, std::string>;

/**
 * @brief 业务自定义路由匹配器。
 * @details `Match` 只应读取请求（路径、头、方法等），命中时写 params 并返回 true。
 *          网关按「自定义 Matcher > 静态 > 参数 > 通配」的顺序调用；同一层内按
 *          priority 与注册顺序排序。
 */
class Matcher {
public:
    virtual ~Matcher() = default;
    /// @brief 判断请求是否命中；命中时写入参数。
    virtual bool Match(const Request& request, PathParams* params) const = 0;
};

/// @brief 路由处理器：业务在自定义路由中直接填写 Response。
using Handler = std::function<Result<void>(Request&, Response&)>;

/// @brief 中间件的「下一环」：无参调用，继续执行剩余链。
using Next = std::function<Result<void>()>;

/// @brief 中间件：调用 next 之前/之后都可以读写 request/response（洋葱模型）。
using Middleware = std::function<Result<void>(Request&, Response&, const Next&)>;

/// @brief 鉴权回调：成功写 user_id，失败写 error（作为 401 body）。
using AuthFn =
    std::function<bool(const Request&, std::string* user_id, std::string* error)>;

/// @brief 限流回调：返回 false 时网关直接返回 429。
using RateLimitFn = std::function<bool(const Request&)>;

/// @brief 访问日志回调：请求完成（含错误响应）后调用。
using AccessLogFn = std::function<void(const Request&, const Response&,
                                       std::chrono::microseconds)>;

/// @brief 响应装饰器：在最终写回前追加/修改响应头。
using DecorateFn = std::function<void(const Request&, Response*)>;

/// @brief 上游解析器：按请求决定实际上游；返回 Err 时网关回 502。
using Resolver = std::function<Result<Upstream>(const Request&)>;

/**
 * @brief 一条注册路由的声明（值对象，`AddRoute` 时快照）。
 */
struct RouteSpec {
    /// 方法掩码；默认 kAll。
    MethodMask method_mask{method_mask::kAll};
    /// 路径模式：静态 / `:name` 参数 / `/prefix/**` 通配（`**` 只能在末尾）。
    std::string path_pattern;
    /// 默认上游 `host:port`（Resolver 为空时使用；Resolver 非空时仅用于健康聚合）。
    std::string upstream;
    /// 是否要求鉴权（由 AuthMiddleware 消费）。
    bool require_auth{false};
    /// 上游超时（毫秒）；覆盖 connect/send/recv 的全局 deadline。
    std::chrono::milliseconds timeout{500};
    /// 同类路由内的优先级（数值越大越优先）；默认 0。
    int priority{0};
    /// 可读名称（日志/健康聚合展示用，可空）。
    std::string name;
};

/**
 * @brief 网关配置；`Create` 时快照，运行期不再改变（Resolver 可用 SetResolver 替换）。
 */
struct Config {
    /// 监听地址；空表示 0.0.0.0。
    std::string listen_host{"0.0.0.0"};
    /// 监听端口；0 由内核分配，实际端口用 `Gateway::port()` 查询。
    std::uint16_t port{28080};
    /// 连接 worker 线程数；0 按 4 处理。
    std::size_t worker_threads{4};
    /// 待处理连接队列上限；超过时先回 503 + `Connection: close` 再关闭新连接（背压）。
    std::size_t max_queued_connections{256};
    /// accept 轮询超时（决定 Stop 的响应速度）。
    std::chrono::milliseconds accept_timeout{100};
    /// 单个请求的读取总时限（毫秒，从本次请求第一次 recv 前起算，覆盖头部+body）。
    /// 到期直接关闭连接，替代「每次 recv 30s」——后者可被 slowloris 无限续期。
    /// 0 按 10000ms 处理。
    std::chrono::milliseconds request_timeout{10000};
    /// 鉴权回调；非空时自动安装 `AuthMiddleware`。
    AuthFn auth;
    /// 限流回调；非空时自动安装 `RateLimitMiddleware`。
    RateLimitFn rate_limit;
    /// 是否自动安装 `AccessLogMiddleware`（默认 sink 输出到 stderr）。
    bool access_log{false};
    /// 最终响应装饰器（所有响应，包括错误响应）。
    DecorateFn decorate;
    /// 初始上游解析器（等价 `SetResolver`）。
    Resolver resolver;
    /// `/api/health/all` 聚合健康时单个上游的超时；0 按 300ms 处理。
    std::chrono::milliseconds health_timeout{300};
};

/// @brief 网关计数器快照。
struct Stats {
    std::uint64_t requests{0};        ///< 收到的请求总数。
    std::uint64_t forwarded{0};       ///< 成功转发到上游的请求数。
    std::uint64_t unauthorized{0};    ///< 401 响应数。
    std::uint64_t rate_limited{0};    ///< 429 响应数。
    std::uint64_t not_found{0};       ///< 404 响应数。
    std::uint64_t upstream_errors{0}; ///< 502（连接失败/解析失败）数。
    std::uint64_t timeouts{0};        ///< 504（上游超时）数。
    std::uint64_t health_checks{0};   ///< 执行的聚合健康检查次数。
    std::uint64_t request_timeouts{0}; ///< 请求读取总时限到期而关闭的连接数。
    std::uint64_t internal_errors{0};  ///< handler/中间件抛异常被兜底成 500 的次数。
};

/**
 * @brief 通用网关实例。
 *
 * 生命周期：`Create` → `AddRoute` / `Use` / `SetResolver` / `LoadConfig`
 * → `Run`（阻塞）→ `Stop`。`Run` 之前注册的路由与中间件对全部连接生效；
 * 运行期 `AddRoute` / `Use` 也允许（内部用读写锁保护，匹配/执行时取快照）。
 */
class Gateway final {
public:
    /// @brief 创建网关并绑定监听端口；失败时返回 Err（端口占用/地址非法等）。
    static Result<std::unique_ptr<Gateway>> Create(Config config);
    ~Gateway();

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;
    Gateway(Gateway&&) = delete;
    Gateway& operator=(Gateway&&) = delete;

    /**
     * @brief 注册一条内置路由（静态/参数/通配）。
     * @return 模式或 upstream 非法时返回 Err；同 method_mask + path_pattern
     *         再次注册会覆盖旧路由（热更新）。
     */
    Result<void> AddRoute(RouteSpec spec);

    /**
     * @brief 注册一条完全自定义的路由：Matcher 决定命中，Handler 直接处理。
     * @param matcher 匹配器；空指针返回 Err。
     * @param handler 处理器；空指针返回 Err。
     */
    Result<void> AddRoute(std::shared_ptr<Matcher> matcher,
                          std::shared_ptr<Handler> handler);

    /// @brief 追加一个中间件；按调用顺序由外到内执行。
    void Use(Middleware middleware);

    /// @brief 替换上游解析器；传空函数对象恢复「按 RouteSpec::upstream 直连」。
    void SetResolver(Resolver resolver);

    /**
     * @brief 从 JSON 加载 routes 并逐条 AddRoute（增量；同 method + pattern 覆盖）。
     *
     * 支持的结构：
     * @code
     * {"routes":[
     *   {"method":"GET","path":"/api/user/list","upstream":"127.0.0.1:29101",
     *    "require_auth":false,"timeout_ms":500,"priority":0,"name":"user"}
     * ]}
     * @endcode
     * `method` 可为字符串、字符串数组或 `"*"`；缺省 `"*"`。
     * 任一条非法时停止并返回 Err（已成功的条目保留，符合增量热更新语义）。
     */
    Result<void> LoadConfig(std::string_view json);

    /// @brief 读取文件后调用 `LoadConfig`。
    Result<void> LoadConfigFile(const std::string& path);

    /// @brief 阻塞接受连接，直到 `Stop()` 被调用；返回时 worker 已全部退出。
    void Run();

    /// @brief 请求停止：关闭监听、唤醒空闲连接、通知 worker 退出；幂等。
    void Stop();

    /// @brief 计数器快照。
    Stats stats() const;

    /// @brief 实际监听端口；`Create` 之后有效。
    std::uint16_t port() const noexcept;

    /// @brief 是否已进入 `Run` 且尚未停止。
    bool running() const noexcept;

private:
    struct Impl;
    explicit Gateway(std::unique_ptr<Impl> impl);
    /// 全部实现细节（路由表/线程池/统计）都藏在 cpp 中。
    std::unique_ptr<Impl> m_impl;
};

/// @brief 内置鉴权中间件：只处理带内部鉴权标记的路由，成功注入 `X-User-Id`。
Middleware AuthMiddleware(AuthFn auth);

/// @brief 内置限流中间件：`limiter` 返回 false 时回 429。
Middleware RateLimitMiddleware(RateLimitFn limiter);

/// @brief 内置访问日志中间件；sink 为空时输出到 stderr。
Middleware AccessLogMiddleware(AccessLogFn sink = {});

}  // namespace go2cpp::app::gateway
