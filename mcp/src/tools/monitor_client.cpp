#include "mcp/tools/monitor_client.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "go2cpp/network/http.hpp"

namespace go2cpp::mcp::tools {
namespace {

constexpr int kMinTimeoutMs = 100;
constexpr int kMaxTimeoutMs = 60000;
constexpr int kDefaultTimeoutMs = 3000;

/// 截断过长的响应片段，避免把整页 HTML 塞进工具结果。
std::string Truncate(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    return text.substr(0, limit) + "...(已截断)";
}

/**
 * @brief 规范化 Manager 基地址：只接受 `http://`，去掉尾部斜杠。
 * @param[out] normalized 规范化后的地址
 * @param[out] error 失败时的中文诊断
 */
bool NormalizeBaseUrl(const std::string& input, std::string& normalized,
                      std::string& error) {
    constexpr std::size_t kSchemeSize = 7;  // "http://"
    if (input.empty()) {
        error = "Manager 地址为空：请配置 mcp.monitor.base_url 或传 manager_url 参数";
        return false;
    }
    if (input.compare(0, kSchemeSize, "http://") != 0) {
        error = "不支持的 Manager 地址: " + input +
                "（仅支持 http://host:port；Go2Cpp 没有可用的 TLS 客户端）";
        return false;
    }
    std::string base = input;
    while (base.size() > kSchemeSize && base.back() == '/') {
        base.pop_back();
    }
    const std::size_t slash = base.find('/', kSchemeSize);
    const std::string authority =
        base.substr(kSchemeSize, slash == std::string::npos ? std::string::npos
                                                            : slash - kSchemeSize);
    if (authority.empty()) {
        error = "Manager 地址缺少主机名: " + input +
                "（应形如 http://127.0.0.1:9101）";
        return false;
    }
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        const std::string host = authority.substr(0, colon);
        const std::string port_text = authority.substr(colon + 1);
        if (host.empty() || port_text.empty()) {
            error = "Manager 地址格式错误: " + input +
                    "（应形如 http://127.0.0.1:9101）";
            return false;
        }
        try {
            const long port = std::stol(port_text);
            if (port < 1 || port > 65535) {
                error = "Manager 端口超出范围: " + port_text +
                        "（有效范围 1-65535）";
                return false;
            }
        } catch (const std::exception&) {
            error = "Manager 端口不是数字: " + port_text;
            return false;
        }
    }
    normalized = std::move(base);
    return true;
}

int ClampTimeout(int value) {
    if (value <= 0) {
        value = kDefaultTimeoutMs;
    }
    return std::clamp(value, kMinTimeoutMs, kMaxTimeoutMs);
}

// ---------------------------------------------------------------------------
// Mock 默认数据：字段与 Manager 的四个查询接口保持同一形状。
// ---------------------------------------------------------------------------

json MockLatestPayload() {
    return json::parse(R"json([
        {
            "host": "mock-host-a",
            "timestamp_ms": 1700000000000,
            "host_info": {"hostname": "mock-host-a", "os": "linux",
                          "kernel": "6.6-mock", "arch": "x86_64"},
            "load": {"load1": 1.5, "load5": 1.2, "load15": 0.9},
            "cpus": [{"name": "cpu0", "usage_percent": 20.0},
                     {"name": "cpu1", "usage_percent": 30.0}],
            "mem": {"total_kb": 1048576, "used_percent": 25.0},
            "disks": [{"name": "sda", "util_percent": 12.5}],
            "nets": [{"name": "eth0", "rcv_rate": 1024.0,
                      "snd_rate": 2048.0}],
            "ebpf_active": false,
            "ebpf_reason": "mock: no eBPF"
        },
        {
            "host": "mock-host-b",
            "timestamp_ms": 1700000001000,
            "host_info": {"hostname": "mock-host-b", "os": "linux",
                          "kernel": "6.6-mock", "arch": "x86_64"},
            "load": {"load1": 3.0, "load5": 2.5, "load15": 2.0},
            "cpus": [{"name": "cpu0", "usage_percent": 60.0}],
            "mem": {"total_kb": 2097152, "used_percent": 75.0},
            "disks": [{"name": "sdb", "util_percent": 40.0}],
            "nets": [{"name": "eth0", "rcv_rate": 2048.0,
                      "snd_rate": 4096.0}],
            "ebpf_active": false,
            "ebpf_reason": "mock: no eBPF"
        }
    ])json");
}

json MockStatsPayload() {
    return json::parse(R"json([
        {"host": "mock-host-a", "timestamp_ms": 1700000000000, "score": 92.5,
         "cpu_percent": 25.0, "mem_used_percent": 25.0,
         "disk_util_percent": 12.5, "load1": 1.5,
         "net_rcv_rate": 1024.0, "net_snd_rate": 2048.0,
         "ebpf_active": false},
        {"host": "mock-host-b", "timestamp_ms": 1700000001000, "score": 61.0,
         "cpu_percent": 60.0, "mem_used_percent": 75.0,
         "disk_util_percent": 40.0, "load1": 3.0,
         "net_rcv_rate": 2048.0, "net_snd_rate": 4096.0,
         "ebpf_active": false}
    ])json");
}

json MockKernelPayload() {
    json host_a = json::parse(R"json({
        "host": "mock-host-a",
        "timestamp_ms": 1700000000000,
        "syscall": {"active": false, "total": 0, "p50_us": 0.0,
                    "p99_us": 0.0, "top": []},
        "sched": {"active": false, "switches": 0, "wait_count": 0,
                  "wait_p50_us": 0.0, "wait_p99_us": 0.0,
                  "wait_max_us": 0.0, "top": []},
        "futex": {"active": false, "wait_count": 0, "wake_count": 0,
                  "wait_wake_ratio": 0.0, "wait_p50_us": 0.0,
                  "wait_p99_us": 0.0, "wait_max_us": 0.0, "top": []},
        "tcp": {"active": false, "reason": "mock", "rtt_source": "mock",
                "retransmits": 0, "retransmits_per_sec": 0.0,
                "rtt_samples": 0, "rtt_p50_us": 0.0, "rtt_p99_us": 0.0,
                "rtt_avg_us": 0.0, "rtt_max_us": 0.0, "top": []},
        "fileio": {"active": false, "reason": "mock", "read_count": 0,
                   "write_count": 0, "read_p50_us": 0.0, "read_p99_us": 0.0,
                   "write_p50_us": 0.0, "write_p99_us": 0.0,
                   "read_iops": 0.0, "write_iops": 0.0, "top": []}
    })json");
    json host_b = host_a;
    host_b["host"] = "mock-host-b";
    host_b["timestamp_ms"] = 1700000001000;
    return json::array({host_a, host_b});
}

json MockHealthPayload() {
    return json::parse(R"json({
        "status": "ok", "received": 2, "rejected": 0, "stored": 2
    })json");
}

}  // namespace

// ---------------------------------------------------------------------------
// HttpMonitorClient
// ---------------------------------------------------------------------------

HttpMonitorClient::HttpMonitorClient(MonitorOptions options)
    : options_(std::move(options)) {
    options_.timeout_ms = ClampTimeout(options_.timeout_ms);
}

Result<json> HttpMonitorClient::Get(const MonitorQuery& query,
                                    const std::string& path) {
    std::string base;
    std::string error;
    const std::string requested =
        query.base_url.empty() ? options_.base_url : query.base_url;
    if (!NormalizeBaseUrl(requested, base, error)) {
        return Result<json>::Fail(MonitorErrorKind::kInvalidEndpoint,
                                  std::move(error));
    }
    const std::string url = base + path;
    const int timeout_ms = ClampTimeout(query.timeout_ms);
    const auto result =
        go2cpp::http::Get(url, std::chrono::milliseconds(timeout_ms));
    if (!result.ok() || !result.response) {
        MonitorErrorKind kind = MonitorErrorKind::kConnect;
        std::string message =
            "无法访问 Manager：" + result.message + "（URL: " + url + "）";
        if (result.error == go2cpp::http::Result::Error::Connect ||
            result.error == go2cpp::http::Result::Error::Resolve) {
            message +=
                "；请确认 go2cpp::os::monitor::Manager 已启动，并检查 "
                "mcp.monitor.base_url 或工具参数 manager_url";
        } else if (result.error == go2cpp::http::Result::Error::Timeout) {
            kind = MonitorErrorKind::kTimeout;
            message += "；Manager 在 " + std::to_string(timeout_ms) +
                       "ms 内未响应，可增大 timeout_ms";
        } else if (result.error == go2cpp::http::Result::Error::Parse) {
            kind = MonitorErrorKind::kParse;
            message +=
                "；响应无法解析，请确认该地址确实是 go2cpp::os::monitor 的 "
                "Manager（路径前缀/版本不匹配会得到非 Manager 的响应）";
        }
        return Result<json>::Fail(kind, std::move(message));
    }

    const int status = static_cast<int>(result.response->status);
    if (status < 200 || status >= 300) {
        std::string message = "Manager 返回 HTTP " + std::to_string(status) +
                              "（URL: " + url + "）";
        if (status == 404) {
            message += "；路径不存在，请确认 Manager 版本包含该查询接口";
        }
        if (!result.response->body.empty()) {
            message += "；响应片段: " + Truncate(result.response->body, 200);
        }
        return Result<json>::Fail(MonitorErrorKind::kHttpStatus,
                                  std::move(message), status);
    }

    json payload;
    try {
        payload = json::parse(result.response->body);
    } catch (const std::exception& parse_error) {
        return Result<json>::Fail(
            MonitorErrorKind::kParse,
            "Manager 响应不是合法 JSON（URL: " + url +
                "）: " + parse_error.what());
    }
    return Result<json>::Ok(std::move(payload));
}

Result<json> HttpMonitorClient::Latest(const MonitorQuery& query) {
    return Get(query, "/api/query/latest?n=" + std::to_string(query.limit));
}

Result<json> HttpMonitorClient::Stats(const MonitorQuery& query) {
    return Get(query, "/api/query/stats");
}

Result<json> HttpMonitorClient::Kernel(const MonitorQuery& query) {
    return Get(query, "/api/query/kernel");
}

Result<json> HttpMonitorClient::Health(const MonitorQuery& query) {
    return Get(query, "/api/health");
}

std::string HttpMonitorClient::BaseUrl() const {
    return options_.base_url;
}

MonitorQuery HttpMonitorClient::DefaultQuery() const {
    MonitorQuery query;
    query.limit = 10;
    query.timeout_ms = options_.timeout_ms;
    query.base_url = options_.base_url;
    return query;
}

std::shared_ptr<MonitorClient> MakeHttpMonitorClient(MonitorOptions options) {
    return std::make_shared<HttpMonitorClient>(std::move(options));
}

// ---------------------------------------------------------------------------
// MockMonitorClient
// ---------------------------------------------------------------------------

MockMonitorClient::MockMonitorClient() : MockMonitorClient("mock://monitor") {}

MockMonitorClient::MockMonitorClient(std::string endpoint)
    : endpoint_(std::move(endpoint)),
      latest_(MockLatestPayload()),
      stats_(MockStatsPayload()),
      kernel_(MockKernelPayload()),
      health_(MockHealthPayload()) {}

void MockMonitorClient::SetLatest(json payload) {
    latest_ = std::move(payload);
}

void MockMonitorClient::SetStats(json payload) {
    stats_ = std::move(payload);
}

void MockMonitorClient::SetKernel(json payload) {
    kernel_ = std::move(payload);
}

void MockMonitorClient::SetHealth(json payload) {
    health_ = std::move(payload);
}

void MockMonitorClient::SetResponse(Endpoint endpoint, json payload) {
    switch (endpoint) {
        case Endpoint::kLatest:
            latest_ = std::move(payload);
            return;
        case Endpoint::kStats:
            stats_ = std::move(payload);
            return;
        case Endpoint::kKernel:
            kernel_ = std::move(payload);
            return;
        case Endpoint::kHealth:
            health_ = std::move(payload);
            return;
    }
}

void MockMonitorClient::SetFailure(Endpoint endpoint, MonitorError error) {
    const auto index = static_cast<std::size_t>(endpoint);
    failed_[index] = true;
    errors_[index] = std::move(error);
}

void MockMonitorClient::SetFailure(Endpoint endpoint, std::string message,
                                   MonitorErrorKind kind, int http_status) {
    MonitorError error;
    error.kind = kind;
    error.message = std::move(message);
    error.http_status = http_status;
    SetFailure(endpoint, std::move(error));
}

void MockMonitorClient::ClearFailure(Endpoint endpoint) {
    const auto index = static_cast<std::size_t>(endpoint);
    failed_[index] = false;
    errors_[index] = MonitorError{};
}

json MockMonitorClient::Response(Endpoint endpoint) const {
    switch (endpoint) {
        case Endpoint::kLatest:
            return latest_;
        case Endpoint::kStats:
            return stats_;
        case Endpoint::kKernel:
            return kernel_;
        case Endpoint::kHealth:
            return health_;
    }
    return json(nullptr);
}

std::size_t MockMonitorClient::CallCount(Endpoint endpoint) const {
    std::size_t count = 0;
    for (const auto& call : calls_) {
        if (call.endpoint == endpoint) {
            ++count;
        }
    }
    return count;
}

void MockMonitorClient::ClearCalls() {
    calls_.clear();
}

std::string MockMonitorClient::BaseUrl() const {
    return endpoint_;
}

MonitorQuery MockMonitorClient::DefaultQuery() const {
    MonitorQuery query;
    query.limit = 10;
    query.timeout_ms = 3000;
    query.base_url = endpoint_;
    return query;
}

Result<json> MockMonitorClient::Dispatch(Endpoint endpoint,
                                         const MonitorQuery& query) {
    calls_.push_back(Call{endpoint, query});
    const auto index = static_cast<std::size_t>(endpoint);
    if (failed_[index]) {
        const MonitorError error = errors_[index];
        return Result<json>::Fail(error.kind, error.message, error.http_status);
    }
    return Result<json>::Ok(Response(endpoint));
}

Result<json> MockMonitorClient::Latest(const MonitorQuery& query) {
    auto result = Dispatch(Endpoint::kLatest, query);
    // 模拟 Manager 的 ?n= 行为：latest 在服务端按 limit 截断。
    if (!result.ok || !result.value.is_array() || query.limit <= 0) {
        return result;
    }
    const auto limit = static_cast<std::size_t>(query.limit);
    if (result.value.size() <= limit) {
        return result;
    }
    json sliced = json::array();
    for (std::size_t i = 0; i < limit; ++i) {
        sliced.push_back(result.value[i]);
    }
    result.value = std::move(sliced);
    return result;
}

Result<json> MockMonitorClient::Stats(const MonitorQuery& query) {
    return Dispatch(Endpoint::kStats, query);
}

Result<json> MockMonitorClient::Kernel(const MonitorQuery& query) {
    return Dispatch(Endpoint::kKernel, query);
}

Result<json> MockMonitorClient::Health(const MonitorQuery& query) {
    return Dispatch(Endpoint::kHealth, query);
}

}  // namespace go2cpp::mcp::tools
