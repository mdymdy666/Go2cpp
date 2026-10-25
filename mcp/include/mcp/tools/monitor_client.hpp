/**
 * @file monitor_client.hpp
 * @brief 监控工具依赖的 `MonitorClient` 抽象，与 HTTP/测试替身实现。
 *
 * @details 工具层只认这个接口：`os_monitor_*` 的 handler 通过构造注入的
 * `MonitorClient` 查询数据，不 include 任何网络头文件、不拼 URL。这样：
 *
 * - 生产装配注入 `HttpMonitorClient`（配置 `mcp.monitor.base_url`）；
 * - 单测注入 `MockMonitorClient`，完全离线、可编程数据与错误；
 * - 未来换 gRPC/Unix socket/本地缓存，只需要新增一个实现类。
 *
 * 接口方法接收 `MonitorQuery`：`limit` / `timeout_ms` / 可选的 `base_url`
 * 覆盖（对应工具参数 `manager_url`）。失败用 `Result<json>` 的 error 表达，
 * 不抛异常——Manager 未启动是部署状态，不是程序错误。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mcp/protocol/types.hpp"

namespace go2cpp::mcp::tools {

/** @brief 监控查询失败的分类，供工具层/调用方判断是否可重试。 */
enum class MonitorErrorKind : std::uint8_t {
    kNone = 0,
    kInvalidEndpoint = 1,  ///< 地址为空/非 http/端口非法（配置错误，不重试）
    kConnect = 2,          ///< 连接/解析主机失败（Manager 未启动）
    kTimeout = 3,          ///< 请求超时（可增大 timeout_ms）
    kParse = 4,            ///< 响应不是合法 JSON / 不是 Manager 响应
    kHttpStatus = 5,       ///< 非 2xx 状态码
    kResponseFormat = 6,   ///< JSON 结构不符合工具预期（工具层设置）
};

/** @brief 一次失败的结构化信息。 */
struct MonitorError {
    MonitorErrorKind kind{MonitorErrorKind::kNone};
    /// 已包含 URL、HTTP 状态与排查提示的中文诊断，可直接作为工具错误文本。
    std::string message;
    /// 非 2xx 时的 HTTP 状态码；其它错误为 0。
    int http_status{0};
};

/** @brief 轻量结果类型：成功带值，失败带 `MonitorError`。 */
template <typename T>
struct Result {
    bool ok{false};
    T value{};
    MonitorError error;

    /** @brief 构造成功结果。 */
    static Result Ok(T value) {
        Result result;
        result.ok = true;
        result.value = std::move(value);
        return result;
    }

    /** @brief 构造失败结果。 */
    static Result Fail(MonitorErrorKind kind, std::string message,
                       int http_status = 0) {
        Result result;
        result.ok = false;
        result.error.kind = kind;
        result.error.message = std::move(message);
        result.error.http_status = http_status;
        return result;
    }
};

/** @brief 一次查询的参数。 */
struct MonitorQuery {
    /// 返回条数（latest）或主机数（stats/kernel）；health 忽略。
    int limit{10};
    /// 本次查询超时（毫秒）。
    int timeout_ms{3000};
    /// 覆盖客户端默认地址；空表示用客户端配置。对应工具参数 manager_url。
    std::string base_url;
};

/**
 * @brief 监控数据查询接口（工具层唯一依赖）。
 *
 * 四个方法一一对应四个 MCP 工具；实现必须线程安全（工具可能并发调用）。
 */
class MonitorClient {
public:
    MonitorClient() = default;
    virtual ~MonitorClient() = default;

    MonitorClient(const MonitorClient&) = delete;
    MonitorClient& operator=(const MonitorClient&) = delete;
    MonitorClient(MonitorClient&&) = delete;
    MonitorClient& operator=(MonitorClient&&) = delete;

    /** @brief `os_monitor_latest`：最近 limit 条快照（JSON 数组）。 */
    virtual Result<json> Latest(const MonitorQuery& query) = 0;
    /** @brief `os_monitor_stats`：每主机汇总（JSON 数组）。 */
    virtual Result<json> Stats(const MonitorQuery& query) = 0;
    /** @brief `os_monitor_kernel`：每主机内核摘要（JSON 数组）。 */
    virtual Result<json> Kernel(const MonitorQuery& query) = 0;
    /** @brief `os_monitor_health`：Manager 健康（JSON 对象）。 */
    virtual Result<json> Health(const MonitorQuery& query) = 0;

    /** @brief 默认地址，用于工具结果展示；未知实现可返回空串。 */
    virtual std::string BaseUrl() const { return {}; }

    /** @brief 未传工具参数时使用的默认查询。 */
    virtual MonitorQuery DefaultQuery() const { return {}; }
};

/** @brief `mcp.monitor.*` 配置（HTTP 实现的构造参数）。 */
struct MonitorOptions {
    /// Manager 基地址，形如 `http://127.0.0.1:9101`；不要带查询串。
    std::string base_url = "http://127.0.0.1:9101";
    /// 单次查询超时（毫秒）；调用方可用工具参数 `timeout_ms` 临时覆盖。
    int timeout_ms = 3000;
};

/**
 * @brief 通过 HTTP GET 查询 `go2cpp::os::monitor::Manager`。
 *
 * 只支持 `http://`（Go2Cpp 没有 TLS 客户端，Manager 默认监听本地回环）。
 * 负责 URL 规范化、HTTP 状态/JSON 解析与中文诊断；工具层不感知这些细节。
 */
class HttpMonitorClient final : public MonitorClient {
public:
    explicit HttpMonitorClient(MonitorOptions options = {});

    Result<json> Latest(const MonitorQuery& query) override;
    Result<json> Stats(const MonitorQuery& query) override;
    Result<json> Kernel(const MonitorQuery& query) override;
    Result<json> Health(const MonitorQuery& query) override;

    std::string BaseUrl() const override;
    MonitorQuery DefaultQuery() const override;

    /** @brief 当前配置。 */
    const MonitorOptions& options() const noexcept { return options_; }

private:
    /// 规范化地址后 GET `path`，把网络/HTTP/JSON 失败收敛成 MonitorError。
    Result<json> Get(const MonitorQuery& query, const std::string& path);

    MonitorOptions options_;
};

/** @brief 生产装配用的工厂：按配置创建 HTTP 客户端。 */
std::shared_ptr<MonitorClient> MakeHttpMonitorClient(
    MonitorOptions options = {});

/**
 * @brief 测试替身：固定/可编程数据，不发起任何网络请求。
 *
 * 默认给出一套结构完整的样例数据（latest 2 条、stats/kernel 2 台主机、
 * health 计数），覆盖四个工具的 happy path；`SetResponse` 替换数据，
 * `SetFailure` 注入错误，`calls()` 记录每次查询的参数（验证透传）。
 */
class MockMonitorClient final : public MonitorClient {
public:
    /** @brief 可编程的四个端点。 */
    enum class Endpoint : std::uint8_t {
        kLatest = 0,
        kStats = 1,
        kKernel = 2,
        kHealth = 3,
    };

    /** @brief 一次调用记录（端点为被调用的工具数据源）。 */
    struct Call {
        Endpoint endpoint{Endpoint::kLatest};
        MonitorQuery query;
    };

    MockMonitorClient();
    explicit MockMonitorClient(std::string endpoint);

    // ---- 可编程数据 ----
    void SetLatest(json payload);
    void SetStats(json payload);
    void SetKernel(json payload);
    void SetHealth(json payload);
    /** @brief 按端点设置返回数据（等价于上面的专用方法）。 */
    void SetResponse(Endpoint endpoint, json payload);

    /** @brief 让某个端点返回失败；message 原样作为工具错误文本。 */
    void SetFailure(Endpoint endpoint, MonitorError error);
    void SetFailure(Endpoint endpoint, std::string message,
                    MonitorErrorKind kind = MonitorErrorKind::kConnect,
                    int http_status = 0);
    void ClearFailure(Endpoint endpoint);

    /** @brief 当前数据（拷贝）。 */
    json Response(Endpoint endpoint) const;

    // ---- 观测 ----
    const std::vector<Call>& calls() const noexcept { return calls_; }
    std::size_t CallCount(Endpoint endpoint) const;
    void ClearCalls();

    std::string BaseUrl() const override;
    MonitorQuery DefaultQuery() const override;

    Result<json> Latest(const MonitorQuery& query) override;
    Result<json> Stats(const MonitorQuery& query) override;
    Result<json> Kernel(const MonitorQuery& query) override;
    Result<json> Health(const MonitorQuery& query) override;

private:
    Result<json> Dispatch(Endpoint endpoint, const MonitorQuery& query);

    std::string endpoint_;
    json latest_;
    json stats_;
    json kernel_;
    json health_;
    bool failed_[4]{false, false, false, false};
    MonitorError errors_[4];
    std::vector<Call> calls_;
};

}  // namespace go2cpp::mcp::tools
