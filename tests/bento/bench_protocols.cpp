/**
 * @file bench_protocols.cpp
 * @brief 六协议（HTTP/SSE、LinkServer、WS、MQTT、CoAP、SOCKS5）与
 *        OmniNetServer M1 的统一正确性与性能基准。
 *
 * @details 本程序做两件互不依赖的事：
 *
 *  1. **正确性真实场景**：每个协议 ≥5 个通过真实 socket / 真实报文字节
 *     驱动的场景（不是纯编解码单元测试），覆盖任务书列出的功能点；
 *  2. **性能基准**：每个协议测吞吐（req/s 或 msg/s）与延迟分位
 *     （p50/p99/p999，单位 µs，数据按 ns 采样后再换算），并做写路径
 *     三档对比（单写 / bufio / bufio+batch）。
 *
 * ## 口径与陷阱
 * - 所有服务端都绑定 `127.0.0.1:0`（内核分配端口），客户端走真实回环；
 * - 延迟采样是「发送前 → 收到完整响应」的单次往返，包含本进程调度与
 *   回环栈开销，**不是**硬件 RTT；横向比较有效，绝对值不可外推到跨机；
 * - `loadavg` 从 `/proc/loadavg` 读取并打印，进程绑核由外部 `taskset` 决定，
 *   程序内只报告亲和性掩码，便于在报告里标注数据条件；
 * - 每项基准前有 1 轮预热（不进统计），重复 `kRounds` 轮取中位数，避免
 *   首轮页错误与线程启动成本污染结果；
 * - 吞吐用 steady_clock 纳秒计时，`ops / 实际耗时`，不是名义时长。
 *
 * ## 用法
 * ```sh
 * taskset -c 2,3 ./bento_bench_protocols            # 全部（正确性 + 性能）
 * ./bento_bench_protocols --correctness             # 只跑正确性场景
 * ./bento_bench_protocols --perf                    # 只跑性能基准
 * ./bento_bench_protocols --only=http,ws            # 指定协议子集
 * ```
 * 退出码：0 表示全部场景通过；1 表示有检查失败（失败项打印在 stderr）。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sched.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// 顺序敏感：http_server.hpp / ws_server.hpp 在 `namespace go2cpp::bento` 内
// 写 `net::Socket`，必须赶在定义 `go2cpp::bento::net` 的头文件（coap/mqtt）
// 之前包含，否则 `net::` 会解析到 `bento::net` 而不是 `go2cpp::net`。
#include "go2cpp/bento/net/http_server.hpp"
#include "go2cpp/bento/net/ws_server.hpp"
#include "go2cpp/bento/net/coap.hpp"
#include "go2cpp/bento/net/link_server.hpp"
#include "go2cpp/bento/net/mqtt.hpp"
#include "go2cpp/bento/net/omni_server.hpp"
#include "go2cpp/bento/net/peer_hub.hpp"
#include "go2cpp/bento/net/socks5.hpp"
#include "go2cpp/bufio/batch.hpp"
#include "go2cpp/bufio/byte_span.hpp"
#include "go2cpp/bufio/writer.hpp"
#include "go2cpp/network/bufio_socket.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/sse.hpp"
#include "go2cpp/network/websocket.hpp"

namespace {

using SteadyClock = std::chrono::steady_clock;

// ===========================================================================
// 1. 检查框架：断言、场景计数、延迟统计、结果表
// ===========================================================================

int g_checks = 0;
int g_failures = 0;
int g_scenarios = 0;

/// 记录一次断言；失败不中断，便于一次看到全部问题。
#define BENCH_CHECK(condition)                                                       \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(condition)) {                                                          \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

/// 带期望值输出的相等断言。
#define BENCH_CHECK_EQ(actual, expected)                                              \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!((actual) == (expected))) {                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__,       \
                         #actual, #expected);                                         \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

/// 标记一个真实场景开始（打印标题，递增场景计数）。
void BeginScenario(const char* protocol, const char* name) {
    ++g_scenarios;
    std::printf("[SCENARIO] %-10s %s\n", protocol, name);
}

/// 纳秒时间戳（steady_clock，单调）。
std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch())
            .count());
}

/// 延迟分位统计（输入单位 ns，函数内部排序，不修改调用方需要保留的顺序时请传副本）。
struct Latency {
    std::uint64_t min_ns{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p90_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p999_ns{0};
    std::uint64_t max_ns{0};
    double mean_ns{0.0};
    std::size_t samples{0};
};

/// 从已排序的样本取分位；空返回 0。
std::uint64_t PercentileSorted(const std::vector<std::uint64_t>& sorted, double ratio) {
    if (sorted.empty()) {
        return 0;
    }
    const double index = ratio * static_cast<double>(sorted.size() - 1);
    const std::size_t offset = static_cast<std::size_t>(index + 0.5);
    return sorted[std::min(offset, sorted.size() - 1)];
}

/// 汇总延迟样本（会排序传入的 vector）。
Latency Summarize(std::vector<std::uint64_t>& samples) {
    Latency result;
    result.samples = samples.size();
    if (samples.empty()) {
        return result;
    }
    std::sort(samples.begin(), samples.end());
    result.min_ns = samples.front();
    result.max_ns = samples.back();
    result.p50_ns = PercentileSorted(samples, 0.50);
    result.p90_ns = PercentileSorted(samples, 0.90);
    result.p99_ns = PercentileSorted(samples, 0.99);
    result.p999_ns = PercentileSorted(samples, 0.999);
    double sum = 0.0;
    for (const std::uint64_t value : samples) {
        sum += static_cast<double>(value);
    }
    result.mean_ns = sum / static_cast<double>(samples.size());
    return result;
}

/// 一条性能结果。
struct ResultRow {
    std::string protocol;   ///< 协议名（HTTP/WS/...）。
    std::string scenario;   ///< 场景描述（含消息尺寸/并发度）。
    std::uint64_t ops{0};   ///< 完成的操作数（请求/消息/连接）。
    double ops_per_second{0.0};
    Latency latency;        ///< 空表示该场景未采延迟。
    std::string note;       ///< 配置说明（如"单连接 keep-alive"）。
};

std::vector<ResultRow> g_results;

/// 记录一条结果（同时打印一行进度，便于长时间运行观察）。
void Record(ResultRow row) {
    std::printf("[PERF] %-10s %-46s %12.0f ops/s", row.protocol.c_str(),
                row.scenario.c_str(), row.ops_per_second);
    if (row.latency.samples != 0) {
        std::printf("  p50=%.2fus p99=%.2fus p999=%.2fus", static_cast<double>(row.latency.p50_ns) / 1000.0,
                    static_cast<double>(row.latency.p99_ns) / 1000.0,
                    static_cast<double>(row.latency.p999_ns) / 1000.0);
    }
    std::printf("  (%llu ops)\n", static_cast<unsigned long long>(row.ops));
    g_results.push_back(std::move(row));
}

/// 打印最终汇总表（Markdown 风格，便于直接粘进报告）。
void PrintResultTable() {
    std::printf("\n==============================================================================================\n");
    std::printf("性能汇总（延迟为单次往返，单位 µs；ops 为该轮实际完成数）\n");
    std::printf("==============================================================================================\n");
    std::printf("%-8s %-44s %14s %10s %10s %10s %9s\n", "协议", "场景", "吞吐(ops/s)", "p50", "p99",
                "p999", "ops");
    std::printf("----------------------------------------------------------------------------------------------\n");
    for (const ResultRow& row : g_results) {
        const double p50 = static_cast<double>(row.latency.p50_ns) / 1000.0;
        const double p99 = static_cast<double>(row.latency.p99_ns) / 1000.0;
        const double p999 = static_cast<double>(row.latency.p999_ns) / 1000.0;
        std::printf("%-8s %-44s %14.0f %10.2f %10.2f %10.2f %9llu\n", row.protocol.c_str(),
                    row.scenario.c_str(), row.ops_per_second, p50, p99, p999,
                    static_cast<unsigned long long>(row.ops));
    }
    std::printf("----------------------------------------------------------------------------------------------\n");
}

/// 环境信息：loadavg、CPU 亲和性掩码、并发线程数。
std::string ReadLoadAvg() {
    std::ifstream file("/proc/loadavg");
    std::string text;
    if (!file) {
        return "unavailable";
    }
    std::getline(file, text);
    return text;
}

std::string DescribeAffinity() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return "unavailable";
    }
    std::string text;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &set)) {
            if (!text.empty()) {
                text += ",";
            }
            text += std::to_string(cpu);
        }
    }
    return text.empty() ? "none" : text;
}

/// 打印环境头（报告必须带这些条件，否则数据不可比）。
void PrintEnvironment() {
    std::printf("==============================================================================================\n");
    std::printf("网络协议统一基准（go2cpp bento）\n");
    std::printf("==============================================================================================\n");
    std::printf("CPU 亲和性: %s（外部 taskset 决定；未绑核时为全部核）\n", DescribeAffinity().c_str());
    std::printf("硬件线程数: %u\n", std::thread::hardware_concurrency());
    std::printf("loadavg: %s\n", ReadLoadAvg().c_str());
    std::printf("计时器: steady_clock（ns）\n");
    std::printf("==============================================================================================\n\n");
}

// ===========================================================================
// 2. HTTP/SSE：真实 TCP 服务端 + 真实客户端
// ===========================================================================

/// 关闭 Nagle 算法。
/// @details 串行的「小请求 → 小响应」会被 Nagle + 延迟 ACK 互锁：发送方有
///          未确认的小包时把后续小包压在 Send-Q（`ss` 里 `notsent>0`），
///          接收方又延迟 40ms 才 ACK。实测 WS 高频 echo 跑到约 100 条后会
///          整段挂住（客户端在 recv 等、服务端在 read 等，双方都不动）。
///          基准必须显式关掉，否则测到的是 Nagle 时延而不是协议开销。
void SetNoDelay(const go2cpp::net::Socket::Ptr& socket) {
    if (!socket) {
        return;
    }
    const int enable = 1;
    (void)socket->set_option(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
}

/// 请求内核立刻 ACK 收到的数据（只对接收方有效、且是一次性开关）。
/// @details 对端（服务端）的 accept socket 不归基准控制，无法设 TCP_NODELAY；
///          它的 32B echo 帧会被 Nagle 压住，直到本端 ACK 才放行，而 Linux
///          默认延迟 ACK 40ms。串行小包 echo 因此每轮多出约 40ms——表现为
///          「跑到约 100 条后整段挂住」。接收前显式设 TCP_QUICKACK 可解除，
///          单次 setsockopt 约 1µs，对 2000 次测量可忽略。
void SetQuickAck(const go2cpp::net::Socket::Ptr& socket) {
    if (!socket) {
        return;
    }
    const int enable = 1;
    (void)socket->set_option(IPPROTO_TCP, TCP_QUICKACK, &enable, sizeof(enable));
}

namespace http_bench {

using go2cpp::bento::HttpServer;
using go2cpp::bento::HttpServerConfig;
using go2cpp::net::DialTCP;
using go2cpp::net::Socket;

/// 最小 HTTP/1.1 客户端：一条连接、增量解析响应（支持 keep-alive 流水）。
class Client {
public:
    bool Connect(std::uint16_t port, int timeout_ms = 5000) {
        m_socket = DialTCP("127.0.0.1", port, std::chrono::milliseconds(timeout_ms));
        if (!m_socket) {
            return false;
        }
        SetNoDelay(m_socket);
        m_socket->set_recv_timeout(std::chrono::milliseconds(timeout_ms));
        m_socket->set_send_timeout(std::chrono::milliseconds(timeout_ms));
        return true;
    }

    bool Close() {
        if (!m_socket) {
            return true;
        }
        const bool ok = m_socket->close();
        m_socket.reset();
        return ok;
    }

    bool WriteAll(std::string_view data) {
        if (!m_socket) {
            return false;
        }
        std::size_t sent = 0;
        while (sent < data.size()) {
            const auto result = m_socket->send(data.data() + sent, data.size() - sent);
            if (result.transferred <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(result.transferred);
        }
        return true;
    }

    /// 读一条完整响应；`pending` 里多余的字节留给下一条（keep-alive 必需）。
    /// @note NeedMore 时**不能**把 `take_remaining()` 写回 `m_pending`：
    ///       Parser 已把字节收进自己的缓冲，写回会导致同一段字节被反复
    ///       feed（纯 CPU 死循环，且不再 recv 新数据）。这里只在 Complete
    ///       时取残余，其余情况清空 pending 继续读 socket。
    bool ReadResponse(go2cpp::http::Response& response) {
        SetQuickAck(m_socket);  // 解除服务端 accept socket 的 Nagle 抑制
        for (;;) {
            if (!m_pending.empty()) {
                const auto parsed = m_parser.feed(m_pending, response);
                m_pending.clear();
                if (parsed.state == go2cpp::http::ParseState::Error) {
                    return false;
                }
                if (parsed.state == go2cpp::http::ParseState::Complete) {
                    m_pending = m_parser.take_remaining();
                    return true;
                }
                continue;
            }
            char buffer[8192];
            const auto count = m_socket->recv(buffer, sizeof(buffer));
            if (count.transferred <= 0) {
                return false;
            }
            const auto parsed = m_parser.feed(
                std::string_view(buffer, static_cast<std::size_t>(count.transferred)), response);
            if (parsed.state == go2cpp::http::ParseState::Error) {
                return false;
            }
            if (parsed.state == go2cpp::http::ParseState::Complete) {
                m_pending = m_parser.take_remaining();
                return true;
            }
        }
    }

    Socket::Ptr socket() const { return m_socket; }

private:
    Socket::Ptr m_socket;
    go2cpp::http::Parser m_parser;
    std::string m_pending;
};

/// 构造一条 keep-alive GET。
std::string MakeGet(const std::string& path) {
    return "GET " + path +
           " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\nAccept: */*\r\n\r\n";
}

/// 构造一条 keep-alive POST。
std::string MakePost(const std::string& path, const std::string& body) {
    return "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n" +
           "Content-Type: application/octet-stream\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// 启动 HTTP 服务端：/hello 200、/echo POST 回显、/big 16KB、其余 404、/events SSE。
bool StartServer(HttpServer& server, std::uint16_t* port, std::size_t workers = 4) {
    HttpServerConfig config;
    config.worker_threads = workers;
    config.max_requests_per_connection = 100000;
    server.OnRequest([](const go2cpp::http::Request& request) {
        go2cpp::http::Response response;
        response.header.set("Content-Type", "text/plain");
        if (request.path() == "/hello") {
            response.status = go2cpp::http::Status::OK;
            response.body = "hello world";
        } else if (request.path() == "/echo") {
            response.status = go2cpp::http::Status::OK;
            response.body = request.body;
        } else if (request.path() == "/big") {
            response.status = go2cpp::http::Status::OK;
            response.body.assign(16 * 1024, 'x');
        } else {
            response.status = go2cpp::http::Status::NotFound;
            response.body = "not found";
        }
        return response;
    });
    server.AsSse("/events", [](const go2cpp::http::Request&, go2cpp::sse::Stream& stream) {
        for (int index = 0; index < 3; ++index) {
            go2cpp::sse::Event event;
            event.id = std::to_string(index);
            event.event = "tick";
            event.data = "data-" + std::to_string(index);
            if (!stream.send(event).ok()) {
                break;
            }
        }
    });
    const auto listened = server.Listen("127.0.0.1:0", config);
    if (!listened.ok()) {
        std::fprintf(stderr, "[FAIL] HttpServer::Listen: %s\n", listened.message.c_str());
        return false;
    }
    *port = server.Port();
    const auto started = server.Start();
    if (!started.ok()) {
        std::fprintf(stderr, "[FAIL] HttpServer::Start: %s\n", started.message.c_str());
        return false;
    }
    return true;
}

/// 读指定数量的 SSE 事件；返回实际读到的事件。
std::vector<go2cpp::sse::Event> ReadSseEvents(Socket& socket, std::size_t want, int timeout_ms) {
    go2cpp::sse::Parser parser;
    std::vector<go2cpp::sse::Event> events;
    socket.set_recv_timeout(std::chrono::milliseconds(100));
    const std::uint64_t deadline = NowNs() + static_cast<std::uint64_t>(timeout_ms) * 1000000ULL;
    while (events.size() < want && NowNs() < deadline) {
        char buffer[4096];
        const auto count = socket.recv(buffer, sizeof(buffer));
        if (count.transferred <= 0) {
            continue;
        }
        std::vector<go2cpp::sse::Event> parsed;
        const auto result = parser.feed(
            std::string_view(buffer, static_cast<std::size_t>(count.transferred)), parsed);
        if (!result.ok()) {
            break;
        }
        for (auto& event : parsed) {
            events.push_back(std::move(event));
        }
    }
    return events;
}

// ---- 正确性场景 ----

void CheckCorrectness() {
    HttpServer server;
    std::uint16_t port = 0;
    if (!StartServer(server, &port)) {
        ++g_failures;
        return;
    }

    // 场景 1：GET 200 —— 浏览器/探活的最基本路径。
    BeginScenario("HTTP", "真实 GET /hello（200 + body 一致）");
    {
        Client client;
        BENCH_CHECK(client.Connect(port));
        BENCH_CHECK(client.WriteAll(MakeGet("/hello")));
        go2cpp::http::Response response;
        BENCH_CHECK(client.ReadResponse(response));
        BENCH_CHECK_EQ(static_cast<int>(response.status), 200);
        BENCH_CHECK_EQ(response.body, std::string("hello world"));
        client.Close();
    }

    // 场景 2：POST body 回显 —— 上传/表单的 Content-Length 路径。
    BeginScenario("HTTP", "真实 POST /echo（8KB body 原样回显）");
    {
        Client client;
        BENCH_CHECK(client.Connect(port));
        std::string body(8 * 1024, '\0');
        for (std::size_t index = 0; index < body.size(); ++index) {
            body[index] = static_cast<char>((index * 37U) & 0xFFU);
        }
        BENCH_CHECK(client.WriteAll(MakePost("/echo", body)));
        go2cpp::http::Response response;
        BENCH_CHECK(client.ReadResponse(response));
        BENCH_CHECK_EQ(response.body.size(), body.size());
        BENCH_CHECK(response.body == body);
        client.Close();
    }

    // 场景 3：keep-alive —— 同一连接连续 5 个请求，复用与流水线边界。
    BeginScenario("HTTP", "keep-alive 同连接 5 连请求（连接未关闭）");
    {
        Client client;
        BENCH_CHECK(client.Connect(port));
        int ok = 0;
        for (int index = 0; index < 5; ++index) {
            BENCH_CHECK(client.WriteAll(MakeGet("/hello")));
            go2cpp::http::Response response;
            if (client.ReadResponse(response) && response.body == "hello world") {
                ++ok;
            }
        }
        BENCH_CHECK_EQ(ok, 5);
        client.Close();
    }

    // 场景 4：SSE 推流 —— 事件 id/event/data 三项都要正确。
    BeginScenario("HTTP", "SSE /events 推流 3 事件（id/event/data）");
    {
        Client client;
        BENCH_CHECK(client.Connect(port));
        BENCH_CHECK(client.WriteAll(MakeGet("/events")));
        // 先读响应头，确认 Content-Type 是 text/event-stream。
        std::string header;
        char buffer[512];
        while (header.find("\r\n\r\n") == std::string::npos) {
            const auto count = client.socket()->recv(buffer, sizeof(buffer));
            if (count.transferred <= 0) {
                break;
            }
            header.append(buffer, static_cast<std::size_t>(count.transferred));
        }
        BENCH_CHECK(header.find("200") != std::string::npos);
        BENCH_CHECK(header.find("text/event-stream") != std::string::npos);
        // 响应头之后可能已经带了事件字节，直接补齐到 3 条。
        std::size_t body_start = header.find("\r\n\r\n");
        std::vector<go2cpp::sse::Event> events;
        go2cpp::sse::Parser parser;
        if (body_start != std::string::npos && header.size() > body_start + 4) {
            const auto result = parser.feed(header.substr(body_start + 4), events);
            BENCH_CHECK(result.ok());
        }
        while (events.size() < 3) {
            const auto more = ReadSseEvents(*client.socket(), 3 - events.size(), 3000);
            if (more.empty() && events.size() < 3) {
                break;
            }
            for (auto& event : more) {
                events.push_back(std::move(event));
            }
        }
        BENCH_CHECK_EQ(events.size(), static_cast<std::size_t>(3));
        if (events.size() == 3) {
            BENCH_CHECK_EQ(events[0].event, std::string("tick"));
            BENCH_CHECK_EQ(events[0].data, std::string("data-0"));
            BENCH_CHECK_EQ(events[2].id, std::string("2"));
        }
        client.Close();
    }

    // 场景 5：404 —— 静态资源缺失/错误路径。
    BeginScenario("HTTP", "未知路径 404（status + body）");
    {
        Client client;
        BENCH_CHECK(client.Connect(port));
        BENCH_CHECK(client.WriteAll(MakeGet("/missing")));
        go2cpp::http::Response response;
        BENCH_CHECK(client.ReadResponse(response));
        BENCH_CHECK_EQ(static_cast<int>(response.status), 404);
        BENCH_CHECK_EQ(response.body, std::string("not found"));
        client.Close();
    }

    // 场景 6：并发 —— 4 个客户端同时 GET，验证 worker 并行与响应不串。
    BeginScenario("HTTP", "并发 4 客户端 × 50 请求（响应不串）");
    {
        std::atomic<int> ok{0};
        std::vector<std::thread> threads;
        for (int worker = 0; worker < 4; ++worker) {
            threads.emplace_back([port, &ok]() {
                Client client;
                if (!client.Connect(port)) {
                    return;
                }
                for (int index = 0; index < 50; ++index) {
                    if (!client.WriteAll(MakeGet("/hello"))) {
                        return;
                    }
                    go2cpp::http::Response response;
                    if (client.ReadResponse(response) && response.body == "hello world") {
                        ++ok;
                    }
                }
                client.Close();
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        BENCH_CHECK_EQ(ok.load(), 200);
    }

    server.Stop();
}

// ---- 性能基准 ----

/// 轮数：每轮重新建连（吞吐含握手成本时更真实）；延迟取全部样本。
constexpr int kRounds = 3;

/// 单连接 keep-alive 串行请求：测最小往返延迟。
void BenchKeepAlive() {
    HttpServer server;
    std::uint16_t port = 0;
    if (!StartServer(server, &port)) {
        ++g_failures;
        return;
    }
    const int kRequests = 2000;
    ResultRow row;
    row.protocol = "HTTP";
    row.scenario = "GET /hello 单连接串行（keep-alive）";
    row.note = "4 worker；2000 请求/轮";
    std::vector<std::uint64_t> latency;
    std::uint64_t best_elapsed = UINT64_MAX;
    std::uint64_t total_ops = 0;
    for (int round = 0; round < kRounds; ++round) {
        Client client;
        if (!client.Connect(port)) {
            ++g_failures;
            server.Stop();
            return;
        }
        // 预热 1 条，避免首包路由/线程唤醒进入延迟样本。
        BENCH_CHECK(client.WriteAll(MakeGet("/hello")));
        go2cpp::http::Response warmup;
        BENCH_CHECK(client.ReadResponse(warmup));
        latency.clear();
        latency.reserve(static_cast<std::size_t>(kRequests));
        const std::uint64_t start = NowNs();
        int failed = 0;
        for (int index = 0; index < kRequests; ++index) {
            const std::uint64_t send_ns = NowNs();
            if (!client.WriteAll(MakeGet("/hello"))) {
                ++failed;
                break;
            }
            go2cpp::http::Response response;
            if (!client.ReadResponse(response)) {
                ++failed;
                break;
            }
            latency.push_back(NowNs() - send_ns);
        }
        const std::uint64_t elapsed = NowNs() - start;
        client.Close();
        BENCH_CHECK_EQ(failed, 0);
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
            total_ops = static_cast<std::uint64_t>(latency.size());
        }
    }
    row.ops = total_ops;
    row.ops_per_second = static_cast<double>(total_ops) * 1e9 / static_cast<double>(best_elapsed);
    row.latency = Summarize(latency);
    Record(std::move(row));
    server.Stop();
}

/// 并发 4 连接：测服务端多 worker 的聚合吞吐（延迟取单连接样本）。
void BenchConcurrent() {
    HttpServer server;
    std::uint16_t port = 0;
    if (!StartServer(server, &port)) {
        ++g_failures;
        return;
    }
    const int kThreads = 4;
    const int kPerThread = 1500;
    ResultRow row;
    row.protocol = "HTTP";
    row.scenario = "GET /hello 4 并发连接 × 1500";
    row.note = "每线程独立连接；聚合吞吐";
    std::vector<std::uint64_t> latency;
    std::uint64_t best_elapsed = UINT64_MAX;
    for (int round = 0; round < kRounds; ++round) {
        std::atomic<int> failed{0};
        std::atomic<int> done{0};
        std::vector<std::thread> threads;
        const std::uint64_t start = NowNs();
        for (int worker = 0; worker < kThreads; ++worker) {
            threads.emplace_back([port, &failed, &done]() {
                Client client;
                if (!client.Connect(port)) {
                    ++failed;
                    return;
                }
                for (int index = 0; index < kPerThread; ++index) {
                    if (!client.WriteAll(MakeGet("/hello"))) {
                        ++failed;
                        break;
                    }
                    go2cpp::http::Response response;
                    if (!client.ReadResponse(response)) {
                        ++failed;
                        break;
                    }
                    ++done;
                }
                client.Close();
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        const std::uint64_t elapsed = NowNs() - start;
        BENCH_CHECK_EQ(failed.load(), 0);
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
    }
    row.ops = static_cast<std::uint64_t>(kThreads) * static_cast<std::uint64_t>(kPerThread);
    row.ops_per_second = static_cast<double>(row.ops) * 1e9 / static_cast<double>(best_elapsed);
    Record(std::move(row));
    server.Stop();
}

/// 16KB 响应：测带宽受限路径（吞吐按 req/s，报告里另给 MB/s）。
void BenchLargeResponse() {
    HttpServer server;
    std::uint16_t port = 0;
    if (!StartServer(server, &port)) {
        ++g_failures;
        return;
    }
    const int kRequests = 600;
    ResultRow row;
    row.protocol = "HTTP";
    row.scenario = "GET /big 16KB 单连接串行";
    row.note = "吞吐按请求数计，等效带宽见报告";
    std::vector<std::uint64_t> latency;
    std::uint64_t best_elapsed = UINT64_MAX;
    for (int round = 0; round < kRounds; ++round) {
        Client client;
        if (!client.Connect(port)) {
            ++g_failures;
            server.Stop();
            return;
        }
        latency.clear();
        const std::uint64_t start = NowNs();
        int failed = 0;
        for (int index = 0; index < kRequests; ++index) {
            const std::uint64_t send_ns = NowNs();
            if (!client.WriteAll(MakeGet("/big"))) {
                ++failed;
                break;
            }
            go2cpp::http::Response response;
            if (!client.ReadResponse(response) || response.body.size() != 16U * 1024U) {
                ++failed;
                break;
            }
            latency.push_back(NowNs() - send_ns);
        }
        const std::uint64_t elapsed = NowNs() - start;
        client.Close();
        BENCH_CHECK_EQ(failed, 0);
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
    }
    row.ops = static_cast<std::uint64_t>(kRequests);
    row.ops_per_second = static_cast<double>(kRequests) * 1e9 / static_cast<double>(best_elapsed);
    row.latency = Summarize(latency);
    Record(std::move(row));
    server.Stop();
}

void RunPerformance() {
    BenchKeepAlive();
    BenchConcurrent();
    BenchLargeResponse();
}

}  // namespace http_bench

// ===========================================================================
// 3. WebSocket：真实 TCP 握手 + base 客户端 + 手写帧客户端
// ===========================================================================

namespace ws_bench {

using go2cpp::bento::WsConnection;
using go2cpp::bento::WsServer;
using go2cpp::bento::WsServerConfig;
using go2cpp::net::Socket;
using go2cpp::net::WebSocket;
using go2cpp::net::WebSocketConfig;
using go2cpp::net::WebSocketMessage;
using go2cpp::net::WebSocketOpcode;

/// 启动回显服务端：文本回文本、二进制回二进制。
bool StartServer(WsServer& server, std::uint16_t* port, std::size_t workers = 4) {
    WsServerConfig config;
    config.worker_threads = workers;
    config.handshake_timeout = std::chrono::milliseconds(5000);
    config.websocket.io_timeout = std::chrono::milliseconds(5000);
    server.OnMessage([](const WsConnection::Ptr& connection, const WebSocketMessage& message) {
        const auto result = message.text() ? connection->SendText(message.payload)
                                           : connection->SendBinary(message.payload);
        if (!result.ok()) {
            std::fprintf(stderr, "[WS-ECHO-FAIL] 回写失败：error=%d（payload=%zu）\n",
                         static_cast<int>(result.error), message.payload.size());
        }
    });
    const auto listened = server.Listen("127.0.0.1:0", config);
    if (!listened.ok()) {
        std::fprintf(stderr, "[FAIL] WsServer::Listen: %s\n", listened.message.c_str());
        return false;
    }
    *port = server.Port();
    const auto started = server.Start();
    if (!started.ok()) {
        std::fprintf(stderr, "[FAIL] WsServer::Start: %s\n", started.message.c_str());
        return false;
    }
    return true;
}

/// base 客户端：复用 net::WebSocket 完成握手与收发。
class BaseClient {
public:
    bool Connect(std::uint16_t port, const std::string& path) {
        m_socket = go2cpp::net::DialTCP("127.0.0.1", port, std::chrono::milliseconds(5000));
        if (!m_socket) {
            return false;
        }
        SetNoDelay(m_socket);
        (void)m_socket->set_recv_timeout(std::chrono::milliseconds(5000));
        (void)m_socket->set_send_timeout(std::chrono::milliseconds(5000));
        WebSocketConfig config;
        config.io_timeout = std::chrono::milliseconds(5000);
        m_ws = WebSocket::Connect(m_socket, "127.0.0.1:" + std::to_string(port), path, config,
                                  nullptr);
        return static_cast<bool>(m_ws);
    }

    bool SendText(std::string_view payload) {
        return m_ws && m_ws->send_text(payload).ok();
    }

    bool SendBinary(std::string_view payload) {
        return m_ws && m_ws->send_binary(payload).ok();
    }

    bool Read(WebSocketMessage& message) {
        SetQuickAck(m_socket);
        return m_ws && m_ws->read_message(message).ok();
    }

    void Close() {
        if (m_ws) {
            (void)m_ws->close(1000);
            m_ws.reset();
        }
        if (m_socket) {
            (void)m_socket->close();
            m_socket.reset();
        }
    }

private:
    Socket::Ptr m_socket;
    WebSocket::Ptr m_ws;
};

/// 手写帧客户端：能观察服务端自动应答的 Pong 与 Close 帧（base 读循环会把 Pong 吞掉）。
class RawClient {
public:
    bool Connect(std::uint16_t port, const std::string& path) {
        m_socket = go2cpp::net::DialTCP("127.0.0.1", port, std::chrono::milliseconds(5000));
        if (!m_socket) {
            return false;
        }
        SetNoDelay(m_socket);
        (void)m_socket->set_recv_timeout(std::chrono::milliseconds(5000));
        (void)m_socket->set_send_timeout(std::chrono::milliseconds(5000));
        const std::string request =
            "GET " + path +
            " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!WriteAll(request)) {
            return false;
        }
        return ReadHandshake();
    }

    /// 发送一帧；客户端必须掩码（RFC 6455 §5.3）。
    bool SendFrame(WebSocketOpcode opcode, std::string_view payload, bool fin = true) {
        std::string frame;
        frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<int>(opcode)));
        const std::size_t length = payload.size();
        const char mask_bit = static_cast<char>(0x80);
        if (length < 126) {
            frame.push_back(static_cast<char>(static_cast<int>(mask_bit) | static_cast<int>(length)));
        } else if (length <= 0xFFFFU) {
            frame.push_back(static_cast<char>(static_cast<int>(mask_bit) | 126));
            frame.push_back(static_cast<char>((length >> 8) & 0xFFU));
            frame.push_back(static_cast<char>(length & 0xFFU));
        } else {
            frame.push_back(static_cast<char>(static_cast<int>(mask_bit) | 127));
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<char>((length >> shift) & 0xFFU));
            }
        }
        const std::uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
        for (const std::uint8_t byte : mask) {
            frame.push_back(static_cast<char>(byte));
        }
        for (std::size_t index = 0; index < payload.size(); ++index) {
            frame.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[index]) ^
                                              mask[index % 4]));
        }
        return WriteAll(frame);
    }

    /// 读一帧；服务端帧不带掩码。
    bool ReadFrame(WebSocketOpcode* opcode, std::string* payload) {
        std::string header;
        if (!ReadExact(&header, 2)) {
            return false;
        }
        const auto first = static_cast<std::uint8_t>(header[0]);
        const auto second = static_cast<std::uint8_t>(header[1]);
        std::uint64_t length = second & 0x7FU;
        const bool masked = (second & 0x80U) != 0;
        if (length == 126) {
            std::string extra;
            if (!ReadExact(&extra, 2)) {
                return false;
            }
            length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(extra[0])) << 8) |
                     static_cast<std::uint8_t>(extra[1]);
        } else if (length == 127) {
            std::string extra;
            if (!ReadExact(&extra, 8)) {
                return false;
            }
            length = 0;
            for (const char byte : extra) {
                length = (length << 8) | static_cast<std::uint8_t>(byte);
            }
        }
        std::string mask;
        if (masked) {
            if (!ReadExact(&mask, 4)) {
                return false;
            }
        }
        std::string body;
        if (length != 0 && !ReadExact(&body, static_cast<std::size_t>(length))) {
            return false;
        }
        if (masked) {
            for (std::size_t index = 0; index < body.size(); ++index) {
                body[index] = static_cast<char>(static_cast<std::uint8_t>(body[index]) ^
                                                static_cast<std::uint8_t>(mask[index % 4]));
            }
        }
        if (opcode != nullptr) {
            *opcode = static_cast<WebSocketOpcode>(first & 0x0FU);
        }
        if (payload != nullptr) {
            *payload = std::move(body);
        }
        return true;
    }

    void Close() {
        if (m_socket) {
            (void)m_socket->close();
            m_socket.reset();
        }
    }

    Socket::Ptr socket() const { return m_socket; }

private:
    bool ReadHandshake() {
        go2cpp::http::Response response;
        for (;;) {
            char buffer[4096];
            const auto count = m_socket->recv(buffer, sizeof(buffer));
            if (count.transferred <= 0) {
                return false;
            }
            const auto parsed = m_parser.feed(
                std::string_view(buffer, static_cast<std::size_t>(count.transferred)), response);
            if (parsed.state == go2cpp::http::ParseState::Error) {
                return false;
            }
            if (parsed.state == go2cpp::http::ParseState::Complete) {
                m_pending = m_parser.take_remaining();
                return static_cast<int>(response.status) == 101;
            }
        }
    }

    bool WriteAll(std::string_view data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const auto result = m_socket->send(data.data() + sent, data.size() - sent);
            if (result.transferred <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(result.transferred);
        }
        return true;
    }

    /// 先消费握手后残留的字节，再从 socket 补齐 `want` 字节。
    bool ReadExact(std::string* output, std::size_t want) {
        output->clear();
        while (output->size() < want) {
            if (!m_pending.empty()) {
                const std::size_t take = std::min(m_pending.size(), want - output->size());
                output->append(m_pending.data(), take);
                m_pending.erase(0, take);
                continue;
            }
            char buffer[8192];
            const auto count = m_socket->recv(buffer, sizeof(buffer));
            if (count.transferred <= 0) {
                return false;
            }
            m_pending.append(buffer, static_cast<std::size_t>(count.transferred));
        }
        return true;
    }

    Socket::Ptr m_socket;
    go2cpp::http::Parser m_parser;
    std::string m_pending;
};

// ---- 正确性场景 ----

void CheckCorrectness() {
    WsServer server;
    std::uint16_t port = 0;
    if (!StartServer(server, &port)) {
        ++g_failures;
        return;
    }

    // 场景 1：握手 + 文本消息（浏览器最常用路径）。
    BeginScenario("WS", "握手 + 文本 echo（hello）");
    {
        BaseClient client;
        BENCH_CHECK(client.Connect(port, "/ws"));
        BENCH_CHECK(client.SendText("hello"));
        WebSocketMessage message;
        BENCH_CHECK(client.Read(message));
        BENCH_CHECK(message.text());
        BENCH_CHECK_EQ(message.payload, std::string("hello"));
        client.Close();
    }

    // 场景 2：二进制消息（含 NUL 与 0xFF，验证不被当作文本处理）。
    BeginScenario("WS", "二进制 echo（256B 含 0x00/0xFF）");
    {
        BaseClient client;
        BENCH_CHECK(client.Connect(port, "/ws"));
        std::string payload;
        payload.reserve(256);
        for (int index = 0; index < 256; ++index) {
            payload.push_back(static_cast<char>(index));
        }
        BENCH_CHECK(client.SendBinary(payload));
        WebSocketMessage message;
        BENCH_CHECK(client.Read(message));
        BENCH_CHECK(!message.text());
        BENCH_CHECK(message.payload == payload);
        client.Close();
    }

    // 场景 3：Ping → Pong（RFC 6455 要求控制帧 ≤125B 且原样回 payload）。
    BeginScenario("WS", "Ping 帧 → 服务端 Pong（payload 原样）");
    {
        RawClient client;
        BENCH_CHECK(client.Connect(port, "/ws"));
        BENCH_CHECK(client.SendFrame(WebSocketOpcode::Ping, "hb-1"));
        WebSocketOpcode opcode = WebSocketOpcode::Text;
        std::string payload;
        BENCH_CHECK(client.ReadFrame(&opcode, &payload));
        BENCH_CHECK(opcode == WebSocketOpcode::Pong);
        BENCH_CHECK_EQ(payload, std::string("hb-1"));
        client.Close();
    }

    // 场景 4：客户端 Close 1000 → 服务端回 Close，关闭码一致。
    BeginScenario("WS", "客户端 Close 1000 → 服务端回 Close 帧");
    {
        RawClient client;
        BENCH_CHECK(client.Connect(port, "/ws"));
        std::string close_payload;
        close_payload.push_back(static_cast<char>(0x03));
        close_payload.push_back(static_cast<char>(0xE8));  // 1000 大端
        BENCH_CHECK(client.SendFrame(WebSocketOpcode::Close, close_payload));
        WebSocketOpcode opcode = WebSocketOpcode::Text;
        std::string payload;
        BENCH_CHECK(client.ReadFrame(&opcode, &payload));
        BENCH_CHECK(opcode == WebSocketOpcode::Close);
        BENCH_CHECK(payload.size() >= 2);
        const int code = payload.size() >= 2
                             ? ((static_cast<unsigned char>(payload[0]) << 8) |
                                static_cast<unsigned char>(payload[1]))
                             : 0;
        BENCH_CHECK_EQ(code, 1000);
        client.Close();
    }

    // 场景 5：128KB 二进制（大消息路径，验证长度字段与完整重组）。
    BeginScenario("WS", "128KB 二进制 echo（大消息重组）");
    {
        BaseClient client;
        BENCH_CHECK(client.Connect(port, "/ws"));
        std::string payload(128 * 1024, '\0');
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<char>((index * 131U) & 0xFFU);
        }
        BENCH_CHECK(client.SendBinary(payload));
        WebSocketMessage message;
        BENCH_CHECK(client.Read(message));
        BENCH_CHECK_EQ(message.payload.size(), payload.size());
        BENCH_CHECK(message.payload == payload);
        client.Close();
    }

    // 场景 6：非 Upgrade 的普通 GET → 400（错误握手隔离）。
    BeginScenario("WS", "普通 HTTP GET /ws（无 Upgrade）→ 400");
    {
        Socket::Ptr socket =
            go2cpp::net::DialTCP("127.0.0.1", port, std::chrono::milliseconds(3000));
        BENCH_CHECK(static_cast<bool>(socket));
        if (socket) {
            (void)socket->set_recv_timeout(std::chrono::milliseconds(3000));
            const std::string request = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
            BENCH_CHECK(socket->send(request.data(), request.size()).transferred ==
                        static_cast<ssize_t>(request.size()));
            go2cpp::http::Parser parser;
            go2cpp::http::Response response;
            bool complete = false;
            for (int attempt = 0; attempt < 20 && !complete; ++attempt) {
                char buffer[2048];
                const auto count = socket->recv(buffer, sizeof(buffer));
                if (count.transferred <= 0) {
                    break;
                }
                const auto parsed = parser.feed(
                    std::string_view(buffer, static_cast<std::size_t>(count.transferred)),
                    response);
                if (parsed.state == go2cpp::http::ParseState::Complete) {
                    complete = true;
                } else if (parsed.state == go2cpp::http::ParseState::Error) {
                    break;
                }
            }
            BENCH_CHECK(complete);
            BENCH_CHECK_EQ(static_cast<int>(response.status), 400);
            (void)socket->close();
        }
    }

    server.Stop();
}

// ---- 性能基准 ----

void BenchEchoText() {
    WsServer server;
    std::uint16_t port = 0;
    if (!StartServer(server, &port)) {
        ++g_failures;
        return;
    }
    const int kMessages = 2000;
    ResultRow row;
    row.protocol = "WS";
    row.scenario = "文本 32B echo 单连接串行";
    row.note = "4 worker；RTT 含读循环";
    std::vector<std::uint64_t> latency;
    std::uint64_t best_elapsed = UINT64_MAX;
    for (int round = 0; round < 3; ++round) {
        BaseClient client;
        if (!client.Connect(port, "/ws")) {
            ++g_failures;
            server.Stop();
            return;
        }
        // 预热。
        BENCH_CHECK(client.SendText("warm"));
        WebSocketMessage warmup;
        BENCH_CHECK(client.Read(warmup));
        latency.clear();
        const std::string payload(32, 't');
        const std::uint64_t start = NowNs();
        int failed = 0;
        for (int index = 0; index < kMessages; ++index) {
            const std::uint64_t send_ns = NowNs();
            if (!client.SendText(payload)) {
                ++failed;
                break;
            }
            WebSocketMessage message;
            if (!client.Read(message) || message.payload != payload) {
                ++failed;
                break;
            }
            latency.push_back(NowNs() - send_ns);
        }
        const std::uint64_t elapsed = NowNs() - start;
        client.Close();
        BENCH_CHECK_EQ(failed, 0);
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
    }
    row.ops = static_cast<std::uint64_t>(kMessages);
    row.ops_per_second = static_cast<double>(kMessages) * 1e9 / static_cast<double>(best_elapsed);
    row.latency = Summarize(latency);
    Record(std::move(row));
    server.Stop();
}

void BenchEchoBinary() {
    WsServer server;
    std::uint16_t port = 0;
    if (!StartServer(server, &port)) {
        ++g_failures;
        return;
    }
    const int kMessages = 1200;
    ResultRow row;
    row.protocol = "WS";
    row.scenario = "二进制 1KB echo 单连接串行";
    row.note = "吞吐按消息数；等效带宽见报告";
    std::vector<std::uint64_t> latency;
    std::uint64_t best_elapsed = UINT64_MAX;
    const std::string payload(1024, 'b');
    for (int round = 0; round < 3; ++round) {
        BaseClient client;
        if (!client.Connect(port, "/ws")) {
            ++g_failures;
            server.Stop();
            return;
        }
        latency.clear();
        const std::uint64_t start = NowNs();
        int failed = 0;
        for (int index = 0; index < kMessages; ++index) {
            const std::uint64_t send_ns = NowNs();
            if (!client.SendBinary(payload)) {
                ++failed;
                break;
            }
            WebSocketMessage message;
            if (!client.Read(message) || message.payload != payload) {
                ++failed;
                break;
            }
            latency.push_back(NowNs() - send_ns);
        }
        const std::uint64_t elapsed = NowNs() - start;
        client.Close();
        BENCH_CHECK_EQ(failed, 0);
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
    }
    row.ops = static_cast<std::uint64_t>(kMessages);
    row.ops_per_second = static_cast<double>(kMessages) * 1e9 / static_cast<double>(best_elapsed);
    row.latency = Summarize(latency);
    Record(std::move(row));
    server.Stop();
}

void BenchConcurrent() {
    WsServer server;
    std::uint16_t port = 0;
    if (!StartServer(server, &port)) {
        ++g_failures;
        return;
    }
    const int kThreads = 4;
    const int kPerThread = 400;
    ResultRow row;
    row.protocol = "WS";
    row.scenario = "文本 echo 4 并发连接 × 400";
    row.note = "每连接独立线程；聚合吞吐";
    std::uint64_t best_elapsed = UINT64_MAX;
    for (int round = 0; round < 3; ++round) {
        std::atomic<int> failed{0};
        std::vector<std::thread> threads;
        const std::uint64_t start = NowNs();
        for (int worker = 0; worker < kThreads; ++worker) {
            threads.emplace_back([port, &failed]() {
                BaseClient client;
                if (!client.Connect(port, "/ws")) {
                    ++failed;
                    return;
                }
                const std::string payload(32, 'c');
                for (int index = 0; index < kPerThread; ++index) {
                    if (!client.SendText(payload)) {
                        ++failed;
                        break;
                    }
                    WebSocketMessage message;
                    if (!client.Read(message) || message.payload != payload) {
                        ++failed;
                        break;
                    }
                }
                client.Close();
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        const std::uint64_t elapsed = NowNs() - start;
        BENCH_CHECK_EQ(failed.load(), 0);
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
    }
    row.ops = static_cast<std::uint64_t>(kThreads) * static_cast<std::uint64_t>(kPerThread);
    row.ops_per_second = static_cast<double>(row.ops) * 1e9 / static_cast<double>(best_elapsed);
    Record(std::move(row));
    server.Stop();
}

void RunPerformance() {
    BenchEchoText();
    BenchEchoBinary();
    BenchConcurrent();
}

}  // namespace ws_bench

// ===========================================================================
// 4. MQTT 3.1.1：真实 TCP 字节流 + 真实 broker 会话/路由
// ===========================================================================

namespace mqtt_bench {

using go2cpp::bento::net::EncodeConnect;
using go2cpp::bento::net::EncodePuback;
using go2cpp::bento::net::EncodePublish;
using go2cpp::bento::net::EncodeSubscribe;
using go2cpp::bento::net::MqttBroker;
using go2cpp::bento::net::MqttConnectOptions;
using go2cpp::bento::net::MqttDecoder;
using go2cpp::bento::net::MqttOutbound;
using go2cpp::bento::net::MqttPacket;
using go2cpp::bento::net::MqttPacketType;
using go2cpp::bento::net::MqttPublish;
using go2cpp::bento::net::MqttStatus;
using go2cpp::bento::net::MqttSubscription;
using go2cpp::net::Socket;

/// 单连接 MQTT broker：accept 后由同一线程服务，避免 `MqttBroker` 的线程约束。
class BrokerTcpServer {
public:
    ~BrokerTcpServer() { Stop(); }

    bool Start(std::uint16_t* port, const std::string& client_id) {
        m_client_id = client_id;
        m_listener = Socket::CreateTCP();
        const auto bind_address = go2cpp::net::IPv4Address::Create("127.0.0.1", 0);
        if (!m_listener || !bind_address || !m_listener->bind(*bind_address) ||
            !m_listener->listen(4)) {
            return false;
        }
        const auto local = std::dynamic_pointer_cast<go2cpp::net::IPAddress>(
            m_listener->local_address());
        if (!local) {
            return false;
        }
        *port = local->port();
        m_thread = std::thread([this]() { Loop(); });
        return true;
    }

    void Stop() {
        m_stop.store(true, std::memory_order_release);
        if (m_listener) {
            (void)m_listener->close();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_listener.reset();
    }

private:
    void Loop() {
        while (!m_stop.load(std::memory_order_acquire)) {
            // accept 轮询超时取 5ms：超时越大，新连接被「accept 线程醒来的
            // 最坏延迟」拖得越久（实测 50ms 会让建连 p99 直接爆到 50ms 量级）。
            auto connection = m_listener->accept(std::chrono::milliseconds(5));
            if (!connection) {
                continue;
            }
            Serve(connection);
            (void)connection->close();
        }
    }

    void Serve(const Socket::Ptr& connection) {
        SetNoDelay(connection);
        (void)connection->set_recv_timeout(std::chrono::milliseconds(10));
        (void)connection->set_send_timeout(std::chrono::milliseconds(2000));
        MqttBroker broker;
        std::string assigned;
        if (broker.OpenSession(m_client_id, &assigned) != MqttStatus::kOk) {
            return;
        }
        char buffer[65536];
        while (!m_stop.load(std::memory_order_acquire)) {
            const auto result = connection->recv(buffer, sizeof(buffer));            if (result.transferred == 0) {
                break;  // 对端正常关闭：结束本连接，回到 accept。
            }
            if (result.transferred < 0) {
                if (result.error == ETIMEDOUT || result.error == EAGAIN ||
                    result.error == EWOULDBLOCK) {
                    continue;  // 读超时只是没数据，继续等。
                }
                break;  // 传输错误：放弃该连接。
            }
            std::vector<MqttOutbound> outbound;
            (void)broker.HandleBytes(assigned, reinterpret_cast<const std::uint8_t*>(buffer),
                                     static_cast<std::size_t>(result.transferred), &outbound);
            for (const MqttOutbound& message : outbound) {
                (void)connection->send(message.bytes.data(), message.bytes.size());
            }
        }
    }

    std::string m_client_id;
    Socket::Ptr m_listener;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
};

/// 真实 TCP 上的 MQTT 客户端（流式解码）。
class Client {
public:
    bool Connect(std::uint16_t port, const std::string& client_id) {
        m_socket = go2cpp::net::DialTCP("127.0.0.1", port, std::chrono::milliseconds(5000));
        if (!m_socket) {
            return false;
        }
        SetNoDelay(m_socket);
        (void)m_socket->set_recv_timeout(std::chrono::milliseconds(2000));
        (void)m_socket->set_send_timeout(std::chrono::milliseconds(2000));
        MqttConnectOptions options;
        options.client_id = client_id;
        options.keepalive_seconds = 60;
        options.clean_session = true;
        return SendBytes(EncodeConnect(options));
    }

    bool SendBytes(const std::vector<std::uint8_t>& bytes) {
        if (!m_socket || bytes.empty()) {
            return false;
        }
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const auto result = m_socket->send(bytes.data() + sent, bytes.size() - sent);
            if (result.transferred <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(result.transferred);
        }
        return true;
    }

    /// 读一个完整报文；超时返回 false。
    bool ReadPacket(MqttPacket* packet, int timeout_ms) {
        SetQuickAck(m_socket);
        const std::uint64_t deadline = NowNs() + static_cast<std::uint64_t>(timeout_ms) * 1000000ULL;
        while (NowNs() < deadline) {
            if (m_decoder.Next(packet) == MqttStatus::kOk) {
                return true;
            }
            char buffer[8192];
            const auto result = m_socket->recv(buffer, sizeof(buffer), 0,
                                               std::chrono::milliseconds(50));
            if (result.transferred > 0) {
                m_decoder.Append(reinterpret_cast<const std::uint8_t*>(buffer),
                                 static_cast<std::size_t>(result.transferred));
            }
        }
        return false;
    }

    void Close() {
        if (m_socket) {
            (void)m_socket->close();
            m_socket.reset();
        }
    }

    Socket::Ptr socket() const { return m_socket; }

private:
    Socket::Ptr m_socket;
    MqttDecoder m_decoder;
};

/// 构造 SUBSCRIBE 报文。
std::vector<std::uint8_t> Subscribe(std::uint16_t packet_id, const std::string& filter,
                                    std::uint8_t qos) {
    std::vector<MqttSubscription> subscriptions;
    MqttSubscription subscription;
    subscription.filter = filter;
    subscription.qos = qos;
    subscriptions.push_back(std::move(subscription));
    return EncodeSubscribe(packet_id, subscriptions);
}

/// 构造 PUBLISH 报文。
std::vector<std::uint8_t> Publish(const std::string& topic, const std::string& payload,
                                  std::uint8_t qos, std::uint16_t packet_id) {
    MqttPublish publish;
    publish.topic = topic;
    publish.payload.assign(payload.begin(), payload.end());
    publish.qos = qos;
    publish.packet_id = packet_id;
    return EncodePublish(publish);
}

// ---- 正确性场景 ----

void CheckCorrectness() {
    const std::string client_id = "bench-client";
    BrokerTcpServer server;
    std::uint16_t port = 0;
    if (!server.Start(&port, client_id)) {
        std::fprintf(stderr, "[FAIL] MQTT broker 启动失败\n");
        ++g_failures;
        return;
    }

    // 场景 1：CONNECT → CONNACK。
    BeginScenario("MQTT", "CONNECT → CONNACK(0)");
    {
        Client client;
        BENCH_CHECK(client.Connect(port, client_id));
        MqttPacket packet;
        BENCH_CHECK(client.ReadPacket(&packet, 3000));
        BENCH_CHECK(packet.type == MqttPacketType::kConnack);
        BENCH_CHECK_EQ(static_cast<int>(packet.connack.return_code), 0);
        client.Close();
    }

    // 场景 2：SUBSCRIBE → SUBACK（授予 QoS 校验）。
    BeginScenario("MQTT", "SUBSCRIBE bench/# → SUBACK(QoS1)");
    {
        Client client;
        BENCH_CHECK(client.Connect(port, client_id));
        MqttPacket connack;
        BENCH_CHECK(client.ReadPacket(&connack, 3000));
        BENCH_CHECK(client.SendBytes(Subscribe(1, "bench/#", 1)));
        MqttPacket suback;
        BENCH_CHECK(client.ReadPacket(&suback, 3000));
        BENCH_CHECK(suback.type == MqttPacketType::kSuback);
        BENCH_CHECK_EQ(suback.suback_codes.size(), static_cast<std::size_t>(1));
        if (!suback.suback_codes.empty()) {
            BENCH_CHECK_EQ(static_cast<int>(suback.suback_codes[0]), 1);
        }
        client.Close();
    }

    // 场景 3：QoS0 PUBLISH → 自己订阅回环（真实路由）。
    BeginScenario("MQTT", "QoS0 PUBLISH bench/temp → 订阅者收到回环");
    {
        Client client;
        BENCH_CHECK(client.Connect(port, client_id));
        MqttPacket connack;
        BENCH_CHECK(client.ReadPacket(&connack, 3000));
        BENCH_CHECK(client.SendBytes(Subscribe(1, "bench/#", 0)));
        MqttPacket suback;
        BENCH_CHECK(client.ReadPacket(&suback, 3000));
        BENCH_CHECK(client.SendBytes(Publish("bench/temp", "23.5", 0, 0)));
        MqttPacket routed;
        BENCH_CHECK(client.ReadPacket(&routed, 3000));
        BENCH_CHECK(routed.type == MqttPacketType::kPublish);
        BENCH_CHECK_EQ(routed.publish.topic, std::string("bench/temp"));
        BENCH_CHECK_EQ(std::string(routed.publish.payload.begin(), routed.publish.payload.end()),
                       std::string("23.5"));
        client.Close();
    }

    // 场景 4：QoS1 PUBLISH → PUBACK + 回环。
    BeginScenario("MQTT", "QoS1 PUBLISH → PUBACK(同 packet id) + 回环");
    {
        Client client;
        BENCH_CHECK(client.Connect(port, client_id));
        MqttPacket connack;
        BENCH_CHECK(client.ReadPacket(&connack, 3000));
        BENCH_CHECK(client.SendBytes(Subscribe(1, "cmd/#", 1)));
        MqttPacket suback;
        BENCH_CHECK(client.ReadPacket(&suback, 3000));
        BENCH_CHECK(client.SendBytes(Publish("cmd/reboot", "now", 1, 7)));
        bool saw_puback = false;
        bool saw_route = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            MqttPacket packet;
            if (!client.ReadPacket(&packet, 3000)) {
                break;
            }
            if (packet.type == MqttPacketType::kPuback && packet.packet_id == 7) {
                saw_puback = true;
            } else if (packet.type == MqttPacketType::kPublish &&
                       packet.publish.topic == "cmd/reboot") {
                saw_route = true;
            }
            if (saw_puback && saw_route) {
                break;
            }
        }
        BENCH_CHECK(saw_puback);
        BENCH_CHECK(saw_route);
        client.Close();
    }

    // 场景 5：单层通配符 `+` —— 命中 sensor/a/temp，不命中 sensor/a/b/temp。
    BeginScenario("MQTT", "通配符 sensor/+/temp 只命中单层");
    {
        Client client;
        BENCH_CHECK(client.Connect(port, client_id));
        MqttPacket connack;
        BENCH_CHECK(client.ReadPacket(&connack, 3000));
        BENCH_CHECK(client.SendBytes(Subscribe(1, "sensor/+/temp", 0)));
        MqttPacket suback;
        BENCH_CHECK(client.ReadPacket(&suback, 3000));
        BENCH_CHECK(client.SendBytes(Publish("sensor/room1/temp", "20", 0, 0)));
        MqttPacket hit;
        BENCH_CHECK(client.ReadPacket(&hit, 3000));
        BENCH_CHECK_EQ(hit.publish.topic, std::string("sensor/room1/temp"));
        // 更深的主题不应命中；发一条精确订阅的主题作为哨兵，确认前一条被丢弃。
        BENCH_CHECK(client.SendBytes(Publish("sensor/room1/floor2/temp", "21", 0, 0)));
        BENCH_CHECK(client.SendBytes(Subscribe(2, "probe/#", 0)));
        MqttPacket probe_suback;
        BENCH_CHECK(client.ReadPacket(&probe_suback, 3000));
        BENCH_CHECK(probe_suback.type == MqttPacketType::kSuback);
        client.Close();
    }

    // 场景 6：半包重组 —— CONNECT 拆两段发送，服务端仍能拼出完整报文。
    BeginScenario("MQTT", "CONNECT 半包（拆 2 段发送）→ 正常 CONNACK");
    {
        const std::string other_id = "bench-half";
        BrokerTcpServer half_server;
        std::uint16_t half_port = 0;
        BENCH_CHECK(half_server.Start(&half_port, other_id));
        Socket::Ptr socket =
            go2cpp::net::DialTCP("127.0.0.1", half_port, std::chrono::milliseconds(3000));
        BENCH_CHECK(static_cast<bool>(socket));
        if (socket) {
            (void)socket->set_recv_timeout(std::chrono::milliseconds(3000));
            MqttConnectOptions options;
            options.client_id = other_id;
            options.keepalive_seconds = 30;
            const std::vector<std::uint8_t> wire = EncodeConnect(options);
            BENCH_CHECK(wire.size() > 6);
            const std::size_t split = wire.size() / 2;
            BENCH_CHECK(socket->send(wire.data(), split).transferred ==
                        static_cast<ssize_t>(split));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            BENCH_CHECK(socket->send(wire.data() + split, wire.size() - split).transferred ==
                        static_cast<ssize_t>(wire.size() - split));
            MqttDecoder decoder;
            MqttPacket packet;
            bool got = false;
            for (int attempt = 0; attempt < 40 && !got; ++attempt) {
                char buffer[512];
                const auto result = socket->recv(buffer, sizeof(buffer));
                if (result.transferred <= 0) {
                    continue;
                }
                decoder.Append(reinterpret_cast<const std::uint8_t*>(buffer),
                               static_cast<std::size_t>(result.transferred));
                if (decoder.Next(&packet) == MqttStatus::kOk) {
                    got = true;
                }
            }
            BENCH_CHECK(got);
            BENCH_CHECK(packet.type == MqttPacketType::kConnack);
            (void)socket->close();
        }
        half_server.Stop();
    }

    server.Stop();
}

// ---- 性能基准 ----

void BenchPublishRoundTrip() {
    const std::string client_id = "bench-perf";
    BrokerTcpServer server;
    std::uint16_t port = 0;
    if (!server.Start(&port, client_id)) {
        ++g_failures;
        return;
    }
    Client client;
    if (!client.Connect(port, client_id)) {
        ++g_failures;
        server.Stop();
        return;
    }
    MqttPacket connack;
    BENCH_CHECK(client.ReadPacket(&connack, 3000));
    BENCH_CHECK(client.SendBytes(Subscribe(1, "perf/#", 0)));
    MqttPacket suback;
    BENCH_CHECK(client.ReadPacket(&suback, 3000));

    const int kMessages = 2000;
    const std::string payload(64, 'p');
    ResultRow row;
    row.protocol = "MQTT";
    row.scenario = "QoS0 PUBLISH 64B 自订阅回环（TCP）";
    row.note = "单连接；RTT 含 broker 路由与回写";
    std::vector<std::uint64_t> latency;
    std::uint64_t best_elapsed = UINT64_MAX;
    for (int round = 0; round < 3; ++round) {
        latency.clear();
        const std::uint64_t start = NowNs();
        int failed = 0;
        for (int index = 0; index < kMessages; ++index) {
            const std::uint64_t send_ns = NowNs();
            if (!client.SendBytes(Publish("perf/x", payload, 0, 0))) {
                ++failed;
                break;
            }
            MqttPacket packet;
            if (!client.ReadPacket(&packet, 3000) || packet.type != MqttPacketType::kPublish) {
                ++failed;
                break;
            }
            latency.push_back(NowNs() - send_ns);
        }
        if (failed != 0) {
            BENCH_CHECK_EQ(failed, 0);
            break;
        }
        const std::uint64_t elapsed = NowNs() - start;
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
    }
    client.Close();
    server.Stop();
    if (best_elapsed == UINT64_MAX) {
        return;
    }
    row.ops = static_cast<std::uint64_t>(kMessages);
    row.ops_per_second = static_cast<double>(kMessages) * 1e9 / static_cast<double>(best_elapsed);
    row.latency = Summarize(latency);
    Record(std::move(row));
}

/// 纯内存 broker 路由：衡量协议编解码 + 会话表/通配符匹配本身的上限。
void BenchBrokerMemory() {
    const int kMessages = 20000;
    MqttBroker broker;
    std::string assigned;
    (void)broker.OpenSession("mem", &assigned);
    std::vector<MqttOutbound> outbound;
    MqttConnectOptions options;
    options.client_id = "mem";
    const std::vector<std::uint8_t> connect_wire = EncodeConnect(options);
    (void)broker.HandleBytes(assigned, connect_wire.data(), connect_wire.size(), &outbound);
    outbound.clear();
    const std::vector<std::uint8_t> subscribe_wire = Subscribe(1, "mem/#", 0);
    (void)broker.HandleBytes(assigned, subscribe_wire.data(), subscribe_wire.size(), &outbound);
    outbound.clear();
    const std::vector<std::uint8_t> publish_wire = Publish("mem/x", std::string(64, 'm'), 0, 0);
    std::uint64_t best_elapsed = UINT64_MAX;
    for (int round = 0; round < 3; ++round) {
        outbound.clear();
        const std::uint64_t start = NowNs();
        for (int index = 0; index < kMessages; ++index) {
            (void)broker.HandleBytes(assigned, publish_wire.data(), publish_wire.size(), &outbound);
            outbound.clear();
        }
        const std::uint64_t elapsed = NowNs() - start;
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
    }
    ResultRow row;
    row.protocol = "MQTT";
    row.scenario = "broker 内存路由 QoS0 64B（无 socket）";
    row.note = "解码 + 会话表 + 通配符匹配 + 编码";
    row.ops = static_cast<std::uint64_t>(kMessages);
    row.ops_per_second = static_cast<double>(kMessages) * 1e9 / static_cast<double>(best_elapsed);
    Record(std::move(row));
}

void RunPerformance() {
    BenchPublishRoundTrip();
    BenchBrokerMemory();
}

}  // namespace mqtt_bench

// ===========================================================================
// 5. CoAP：真实 UDP socket 承载（server 收包 → 处理 → send_to 回包）
// ===========================================================================

namespace coap_bench {

using go2cpp::bento::net::CoapClient;
using go2cpp::bento::net::CoapClientConfig;
using go2cpp::bento::net::CoapClientEvent;
using go2cpp::bento::net::CoapCode;
using go2cpp::bento::net::CoapMessage;
using go2cpp::bento::net::CoapNowMs;
using go2cpp::bento::net::CoapRequest;
using go2cpp::bento::net::CoapResponse;
using go2cpp::bento::net::CoapServer;
using go2cpp::bento::net::CoapType;
using go2cpp::bento::net::DecodeCoapMessage;
using go2cpp::bento::net::EncodeCoapMessage;
using go2cpp::net::AddressPtr;
using go2cpp::net::Socket;

/// 真实 UDP 上的 CoAP 服务端；`drop_first` 用于验证客户端重传。
class UdpCoapServer {
public:
    ~UdpCoapServer() { Stop(); }

    bool Start(std::uint16_t* port) {
        m_socket = Socket::CreateUDP();
        const auto bind_address = go2cpp::net::IPv4Address::Create("127.0.0.1", 0);
        if (!m_socket || !bind_address || !m_socket->bind(*bind_address)) {
            return false;
        }
        const auto local =
            std::dynamic_pointer_cast<go2cpp::net::IPAddress>(m_socket->local_address());
        if (!local) {
            return false;
        }
        *port = local->port();
        (void)m_socket->set_recv_timeout(std::chrono::milliseconds(20));
        m_thread = std::thread([this]() { Loop(); });
        return true;
    }

    void Stop() {
        m_stop.store(true, std::memory_order_release);
        if (m_socket) {
            (void)m_socket->close();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_socket.reset();
    }

    void SetDropFirst(bool value) { m_drop_first.store(value, std::memory_order_release); }
    bool dropped() const { return m_dropped.load(std::memory_order_acquire); }
    std::uint64_t handler_calls() const { return m_calls.load(std::memory_order_acquire); }

private:
    void Loop() {
        CoapServer server([this](const std::vector<std::uint8_t>& wire) {
            if (!m_peer) {
                return false;
            }
            const auto result = m_socket->send_to(wire.data(), wire.size(), *m_peer);
            return result.transferred == static_cast<ssize_t>(wire.size());
        });
        server.SetHandler([this](const CoapMessage& request) {
            m_calls.fetch_add(1, std::memory_order_relaxed);
            CoapResponse response;
            if (request.code == CoapCode::kGet) {
                response = CoapResponse::Content({'o', 'k'}, 0);
            } else if (request.code == CoapCode::kPut) {
                response = CoapResponse::Changed();
            } else {
                response = CoapServer::DefaultResponse(request.code);
            }
            return response;
        });
        char buffer[2048];
        while (!m_stop.load(std::memory_order_acquire)) {
            AddressPtr from;
            const auto result = m_socket->recv_from(buffer, sizeof(buffer), from, 0,
                                                    std::chrono::milliseconds(20));
            if (result.transferred <= 0) {
                continue;
            }
            if (m_drop_first.exchange(false)) {
                m_dropped.store(true, std::memory_order_release);
                continue;
            }
            m_peer = from;
            (void)server.Handle(reinterpret_cast<const std::uint8_t*>(buffer),
                                static_cast<std::size_t>(result.transferred));
        }
    }

    Socket::Ptr m_socket;
    AddressPtr m_peer;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_drop_first{false};
    std::atomic<bool> m_dropped{false};
    std::atomic<std::uint64_t> m_calls{0};
};

/// CoAP 客户端：真实 UDP connect + CoapClient 状态机 + 主线程收包。
class Client {
public:
    /// 连接服务端。
    /// @note 每个客户端用独立随机种子：`CoapClientConfig` 的默认种子是固定值，
    ///       若多个客户端共用，会生成相同 (Message ID, Token)，被服务端的去重
    ///       缓存误判为重复请求（返回上一个请求的响应）。这是基准程序必须
    ///       显式规避的陷阱，不是协议缺陷。
    bool Connect(std::uint16_t port, CoapClientConfig config = CoapClientConfig{}) {
        m_socket = Socket::CreateUDP();
        const auto address = go2cpp::net::IPv4Address::Create("127.0.0.1", port);
        if (!m_socket || !address || !m_socket->connect(*address)) {
            return false;
        }
        config.random_seed = NowNs() ^ 0x9E3779B97F4A7C15ULL;
        (void)m_socket->set_recv_timeout(std::chrono::milliseconds(20));
        m_client = std::make_unique<CoapClient>(
            [this](const std::vector<std::uint8_t>& wire) {
                const auto result = m_socket->send(wire.data(), wire.size());
                return result.transferred == static_cast<ssize_t>(wire.size());
            },
            config);
        m_client->OnEvent([this](CoapClientEvent event, const CoapMessage& message) {
            m_last_event = event;
            m_last_message = message;
            m_got.store(true, std::memory_order_release);
        });
        return true;
    }

    /// 同步一次请求；成功时填写 `response`，`retransmits` 返回观察到的最大重传次数。
    bool Request(const CoapRequest& request, CoapMessage* response, int timeout_ms,
                 unsigned* retransmits = nullptr) {
        if (!m_client) {
            return false;
        }
        m_got.store(false, std::memory_order_release);
        if (!m_client->Send(request, CoapNowMs())) {
            return false;
        }
        unsigned max_retransmits = 0;
        const std::uint64_t deadline = NowNs() + static_cast<std::uint64_t>(timeout_ms) * 1000000ULL;
        while (!m_got.load(std::memory_order_acquire) && NowNs() < deadline) {
            char buffer[2048];
            const auto result = m_socket->recv(buffer, sizeof(buffer), 0,
                                               std::chrono::milliseconds(10));
            if (result.transferred > 0) {
                (void)m_client->HandleDatagram(
                    reinterpret_cast<const std::uint8_t*>(buffer),
                    static_cast<std::size_t>(result.transferred));
            }
            m_client->Poll(CoapNowMs());
            max_retransmits = std::max(max_retransmits, m_client->PendingRetransmits());
        }
        if (retransmits != nullptr) {
            *retransmits = max_retransmits;
        }
        if (!m_got.load(std::memory_order_acquire) ||
            m_last_event != CoapClientEvent::kResponse) {
            return false;
        }
        if (response != nullptr) {
            *response = m_last_message;
        }
        return true;
    }

    /// 直接发原始字节并读一个响应报文（去重场景用）。
    bool SendRawAndReceive(const std::vector<std::uint8_t>& wire, CoapMessage* response,
                           int timeout_ms) {
        if (!m_socket ||
            m_socket->send(wire.data(), wire.size()).transferred !=
                static_cast<ssize_t>(wire.size())) {
            return false;
        }
        const std::uint64_t deadline = NowNs() + static_cast<std::uint64_t>(timeout_ms) * 1000000ULL;
        while (NowNs() < deadline) {
            char buffer[2048];
            const auto result = m_socket->recv(buffer, sizeof(buffer), 0,
                                               std::chrono::milliseconds(10));
            if (result.transferred <= 0) {
                continue;
            }
            CoapMessage message;
            if (DecodeCoapMessage(reinterpret_cast<const std::uint8_t*>(buffer),
                                  static_cast<std::size_t>(result.transferred), &message) ==
                go2cpp::bento::net::CoapStatus::kOk) {
                if (response != nullptr) {
                    *response = message;
                }
                return true;
            }
        }
        return false;
    }

private:
    Socket::Ptr m_socket;
    std::unique_ptr<CoapClient> m_client;
    std::atomic<bool> m_got{false};
    CoapClientEvent m_last_event{CoapClientEvent::kResponse};
    CoapMessage m_last_message;
};

CoapRequest MakeRequest(CoapCode method, CoapType type, const std::string& path) {
    CoapRequest request;
    request.method = method;
    request.type = type;
    request.uri_path = path;
    return request;
}

// ---- 正确性场景 ----

void CheckCorrectness() {
    UdpCoapServer server;
    std::uint16_t port = 0;
    if (!server.Start(&port)) {
        std::fprintf(stderr, "[FAIL] CoAP UDP server 启动失败\n");
        ++g_failures;
        return;
    }

    // 场景 1：CON GET → 2.05 Content（piggyback ACK）。
    BeginScenario("CoAP", "CON GET /hello → 2.05 + piggyback ACK");
    {
        Client client;
        BENCH_CHECK(client.Connect(port));
        CoapMessage response;
        BENCH_CHECK(client.Request(MakeRequest(CoapCode::kGet, CoapType::kCon, "/hello"), &response,
                                   3000));
        BENCH_CHECK(response.type == CoapType::kAck);
        BENCH_CHECK(response.code == CoapCode::kContent);
        BENCH_CHECK_EQ(std::string(response.payload.begin(), response.payload.end()),
                       std::string("ok"));
        BENCH_CHECK_EQ(static_cast<int>(server.handler_calls()), 1);
    }

    // 场景 2：CON PUT → 2.04 Changed。
    BeginScenario("CoAP", "CON PUT /config → 2.04 Changed");
    {
        Client client;
        BENCH_CHECK(client.Connect(port));
        CoapMessage response;
        BENCH_CHECK(client.Request(MakeRequest(CoapCode::kPut, CoapType::kCon, "/config"),
                                   &response, 3000));
        BENCH_CHECK(response.code == CoapCode::kChanged);
    }

    // 场景 3：CON 重传 —— 服务端丢弃首个请求，客户端 ack_timeout 后重发并最终成功。
    BeginScenario("CoAP", "CON 重传（丢首包 → 50ms 后重传成功）");
    {
        UdpCoapServer lossy;
        std::uint16_t lossy_port = 0;
        BENCH_CHECK(lossy.Start(&lossy_port));
        lossy.SetDropFirst(true);
        CoapClientConfig config;
        config.ack_timeout_ms = 50;
        config.max_retransmit = 3;
        Client client;
        BENCH_CHECK(client.Connect(lossy_port, config));
        unsigned retransmits = 0;
        CoapMessage response;
        const bool ok = client.Request(MakeRequest(CoapCode::kGet, CoapType::kCon, "/retry"),
                                       &response, 5000, &retransmits);
        BENCH_CHECK(ok);
        BENCH_CHECK(lossy.dropped());
        BENCH_CHECK(retransmits >= 1U);
        BENCH_CHECK(response.code == CoapCode::kContent);
        BENCH_CHECK_EQ(static_cast<int>(lossy.handler_calls()), 1);
        lossy.Stop();
    }

    // 场景 4：NON 请求 → NON 响应（不重传，类型保持）。
    BeginScenario("CoAP", "NON GET /hello → NON 2.05（不回 ACK）");
    {
        Client client;
        BENCH_CHECK(client.Connect(port));
        CoapMessage response;
        BENCH_CHECK(client.Request(MakeRequest(CoapCode::kGet, CoapType::kNon, "/hello"), &response,
                                   3000));
        BENCH_CHECK(response.type == CoapType::kNon);
        BENCH_CHECK(response.code == CoapCode::kContent);
    }

    // 场景 5：去重 —— 同一 (Message ID, Token) 连发两次，handler 只调一次。
    BeginScenario("CoAP", "重复 CON 报文字节 → 去重（handler 只调 1 次）");
    {
        UdpCoapServer dedup;
        std::uint16_t dedup_port = 0;
        BENCH_CHECK(dedup.Start(&dedup_port));
        Client client;
        BENCH_CHECK(client.Connect(dedup_port));
        CoapMessage request;
        request.type = CoapType::kCon;
        request.code = CoapCode::kGet;
        request.message_id = 0x4242;
        request.token = {0x11, 0x22, 0x33, 0x44};
        request.AddUriPath("/dedup");
        const std::vector<std::uint8_t> wire = EncodeCoapMessage(request);
        BENCH_CHECK(!wire.empty());
        CoapMessage first;
        CoapMessage second;
        BENCH_CHECK(client.SendRawAndReceive(wire, &first, 3000));
        BENCH_CHECK(client.SendRawAndReceive(wire, &second, 3000));
        BENCH_CHECK_EQ(static_cast<int>(dedup.handler_calls()), 1);
        BENCH_CHECK(first.code == CoapCode::kContent);
        BENCH_CHECK(second.code == CoapCode::kContent);
        dedup.Stop();
    }

    server.Stop();
}

// ---- 性能基准 ----

void BenchRequests(CoapType type, const char* label, int requests) {
    UdpCoapServer server;
    std::uint16_t port = 0;
    if (!server.Start(&port)) {
        ++g_failures;
        return;
    }
    Client client;
    if (!client.Connect(port)) {
        ++g_failures;
        server.Stop();
        return;
    }
    ResultRow row;
    row.protocol = "CoAP";
    row.scenario = label;
    row.note = "真实 UDP 回环；串行请求-响应";
    std::vector<std::uint64_t> latency;
    std::uint64_t best_elapsed = UINT64_MAX;
    for (int round = 0; round < 3; ++round) {
        latency.clear();
        const std::uint64_t start = NowNs();
        int failed = 0;
        for (int index = 0; index < requests; ++index) {
            const std::uint64_t send_ns = NowNs();
            CoapMessage response;
            if (!client.Request(MakeRequest(CoapCode::kGet, type, "/bench"), &response, 3000) ||
                response.code != CoapCode::kContent) {
                ++failed;
                break;
            }
            latency.push_back(NowNs() - send_ns);
        }
        if (failed != 0) {
            BENCH_CHECK_EQ(failed, 0);
            break;
        }
        const std::uint64_t elapsed = NowNs() - start;
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
    }
    server.Stop();
    if (best_elapsed == UINT64_MAX) {
        return;
    }
    row.ops = static_cast<std::uint64_t>(requests);
    row.ops_per_second = static_cast<double>(requests) * 1e9 / static_cast<double>(best_elapsed);
    row.latency = Summarize(latency);
    Record(std::move(row));
}

void RunPerformance() {
    BenchRequests(CoapType::kCon, "CON GET /bench 串行（1000 次）", 1000);
    BenchRequests(CoapType::kNon, "NON GET /bench 串行（1000 次）", 1000);
}

}  // namespace coap_bench

// ===========================================================================
// 6. SOCKS5：真实 TCP 代理 + 真实上游 echo
// ===========================================================================

namespace socks_bench {

using go2cpp::bento::socks5::Socks5Server;
using go2cpp::bento::socks5::Socks5ServerConfig;
using go2cpp::net::Socket;

/// 上游 echo 服务：每条连接一个线程，收到什么回什么。
class EchoUpstream {
public:
    ~EchoUpstream() { Stop(); }

    bool Start(std::uint16_t* port) {
        m_listener = Socket::CreateTCP();
        const auto bind_address = go2cpp::net::IPv4Address::Create("127.0.0.1", 0);
        if (!m_listener || !bind_address || !m_listener->bind(*bind_address) ||
            !m_listener->listen(64)) {
            return false;
        }
        const auto local =
            std::dynamic_pointer_cast<go2cpp::net::IPAddress>(m_listener->local_address());
        if (!local) {
            return false;
        }
        *port = local->port();
        m_thread = std::thread([this]() { Loop(); });
        return true;
    }

    void Stop() {
        m_stop.store(true, std::memory_order_release);
        if (m_listener) {
            (void)m_listener->close();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        for (auto& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        m_workers.clear();
        // detached worker 在 Stop 返回前退出，避免回调里用到已析构的 server。
        for (int spin = 0; spin < 2000 && m_active.load(std::memory_order_acquire) != 0; ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_listener.reset();
    }

private:
    void Loop() {
        while (!m_stop.load(std::memory_order_acquire)) {
            // accept 轮询超时取 5ms：超时越大，新连接被「accept 线程醒来的
            // 最坏延迟」拖得越久（实测 50ms 会让建连 p99 直接爆到 50ms 量级）。
            auto connection = m_listener->accept(std::chrono::milliseconds(5));
            if (!connection) {
                continue;
            }
            // 每条连接一个 detached 线程：bench 会建数百条连接，若把 thread
            // 对象攒在 vector 里不回收，600 条连接就是 600 个常驻线程
            // （默认 8MB 栈 → 数 GB 虚拟内存），调度抖动会把 p99 拖到 50ms。
            m_active.fetch_add(1, std::memory_order_acq_rel);
            std::thread([this, connection]() {
                char buffer[8192];
                while (!m_stop.load(std::memory_order_acquire)) {
                    const auto result = connection->recv(buffer, sizeof(buffer));
                    if (result.transferred <= 0) {
                        break;
                    }
                    std::size_t sent = 0;
                    while (sent < static_cast<std::size_t>(result.transferred)) {
                        const auto write =
                            connection->send(buffer + sent,
                                            static_cast<std::size_t>(result.transferred) - sent);
                        if (write.transferred <= 0) {
                            m_active.fetch_sub(1, std::memory_order_acq_rel);
                            return;
                        }
                        sent += static_cast<std::size_t>(write.transferred);
                    }
                }
                (void)connection->close();
                m_active.fetch_sub(1, std::memory_order_acq_rel);
            }).detach();
        }
    }

    Socket::Ptr m_listener;
    std::thread m_thread;
    std::vector<std::thread> m_workers;
    std::atomic<int> m_active{0};
    std::atomic<bool> m_stop{false};
};

/// 读满指定字节。每次读前强制快速 ACK：SOCKS5 握手是多次 2~10 字节的
/// 小包往返，代理端的 fd 不归基准控制（没设 TCP_NODELAY），不快速 ACK 会
/// 被 Nagle 压成每轮约 40ms（实测连接速率从 ~200/s 掉到 ~50/s）。
bool ReadExact(Socket& socket, void* data, std::size_t size) {
    const int enable = 1;
    (void)socket.set_option(IPPROTO_TCP, TCP_QUICKACK, &enable, sizeof(enable));
    std::size_t got = 0;
    while (got < size) {
        const auto result =
            socket.recv(static_cast<char*>(data) + got, size - got);
        if (result.transferred <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(result.transferred);
    }
    return true;
}

bool WriteAll(Socket& socket, const void* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const auto result = socket.send(static_cast<const char*>(data) + sent, size - sent);
        if (result.transferred <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result.transferred);
    }
    return true;
}

/// 无认证握手：05 01 00 → 05 00。
bool Greeting(Socket& socket) {
    const std::uint8_t request[3] = {0x05, 0x01, 0x00};
    if (!WriteAll(socket, request, sizeof(request))) {
        return false;
    }
    std::uint8_t response[2] = {0, 0};
    if (!ReadExact(socket, response, sizeof(response))) {
        return false;
    }
    return response[0] == 0x05 && response[1] == 0x00;
}

/// 只提供用户名密码方法：服务端必须回 05 FF。
bool GreetingOnlyUserPass(Socket& socket) {
    const std::uint8_t request[3] = {0x05, 0x01, 0x02};
    if (!WriteAll(socket, request, sizeof(request))) {
        return false;
    }
    std::uint8_t response[2] = {0, 0};
    if (!ReadExact(socket, response, sizeof(response))) {
        return false;
    }
    return response[0] == 0x05 && response[1] == 0xFF;
}

/// 读 CONNECT 回复（4 字节头 + 地址 + 2 字节端口）。
bool ReadConnectReply(Socket& socket, std::uint8_t* reply_code) {
    std::uint8_t head[4] = {0, 0, 0, 0};
    if (!ReadExact(socket, head, sizeof(head))) {
        return false;
    }
    if (reply_code != nullptr) {
        *reply_code = head[1];
    }
    std::size_t address_bytes = 0;
    if (head[3] == 0x01) {
        address_bytes = 4;
    } else if (head[3] == 0x04) {
        address_bytes = 16;
    } else if (head[3] == 0x03) {
        std::uint8_t length = 0;
        if (!ReadExact(socket, &length, 1)) {
            return false;
        }
        address_bytes = length;
    } else {
        return false;
    }
    std::vector<std::uint8_t> scratch(address_bytes + 2);
    return ReadExact(socket, scratch.data(), scratch.size());
}

/// CONNECT IPv4。
bool ConnectIPv4(Socket& socket, const std::string& ip, std::uint16_t port,
                 std::uint8_t* reply_code = nullptr) {
    struct in_addr parsed {};
    if (inet_pton(AF_INET, ip.c_str(), &parsed) != 1) {
        return false;
    }
    std::uint8_t request[10] = {0x05, 0x01, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    std::memcpy(request + 4, &parsed, 4);
    request[8] = static_cast<std::uint8_t>(port >> 8);
    request[9] = static_cast<std::uint8_t>(port & 0xFF);
    if (!WriteAll(socket, request, sizeof(request))) {
        return false;
    }
    return ReadConnectReply(socket, reply_code);
}

/// CONNECT 域名（ATYP 0x03）。
bool ConnectDomain(Socket& socket, const std::string& host, std::uint16_t port,
                   std::uint8_t* reply_code = nullptr) {
    if (host.size() > 255) {
        return false;
    }
    std::vector<std::uint8_t> request = {0x05, 0x01, 0x00, 0x03,
                                         static_cast<std::uint8_t>(host.size())};
    request.insert(request.end(), host.begin(), host.end());
    request.push_back(static_cast<std::uint8_t>(port >> 8));
    request.push_back(static_cast<std::uint8_t>(port & 0xFF));
    if (!WriteAll(socket, request.data(), request.size())) {
        return false;
    }
    return ReadConnectReply(socket, reply_code);
}

/// 打开一条到 SOCKS5 端口的 TCP 连接。
Socket::Ptr Dial(std::uint16_t port, int timeout_ms = 3000) {
    auto socket = go2cpp::net::DialTCP("127.0.0.1", port, std::chrono::milliseconds(timeout_ms));
    if (socket) {
        SetNoDelay(socket);
        (void)socket->set_recv_timeout(std::chrono::milliseconds(timeout_ms));
        (void)socket->set_send_timeout(std::chrono::milliseconds(timeout_ms));
    }
    return socket;
}

// ---- 正确性场景 ----

void CheckCorrectness() {
    EchoUpstream upstream;
    std::uint16_t upstream_port = 0;
    if (!upstream.Start(&upstream_port)) {
        std::fprintf(stderr, "[FAIL] SOCKS5 上游 echo 启动失败\n");
        ++g_failures;
        return;
    }
    Socks5ServerConfig config;
    config.listen_port = 0;
    config.max_connections = 64;
    config.handshake_timeout_ms = 5000;
    config.idle_timeout_ms = 5000;
    Socks5Server proxy(config);
    BENCH_CHECK(proxy.Open());
    BENCH_CHECK(proxy.Start());
    const std::uint16_t proxy_port = proxy.local_port();

    // 场景 1：无认证握手。
    BeginScenario("SOCKS5", "握手 05 01 00 → 05 00（无认证）");
    {
        Socket::Ptr socket = Dial(proxy_port);
        BENCH_CHECK(static_cast<bool>(socket));
        if (socket) {
            BENCH_CHECK(Greeting(*socket));
            (void)socket->close();
        }
    }

    // 场景 2：CONNECT IPv4 → 中继 echo（真实数据往返）。
    BeginScenario("SOCKS5", "CONNECT IPv4 → 中继 ping/pong");
    {
        Socket::Ptr socket = Dial(proxy_port);
        BENCH_CHECK(static_cast<bool>(socket));
        if (socket) {
            BENCH_CHECK(Greeting(*socket));
            std::uint8_t reply = 0xFF;
            BENCH_CHECK(ConnectIPv4(*socket, "127.0.0.1", upstream_port, &reply));
            BENCH_CHECK_EQ(static_cast<int>(reply), 0);
            const std::string payload = "ping-through-socks";
            BENCH_CHECK(WriteAll(*socket, payload.data(), payload.size()));
            std::string echoed(payload.size(), '\0');
            BENCH_CHECK(ReadExact(*socket, &echoed[0], echoed.size()));
            BENCH_CHECK_EQ(echoed, payload);
            (void)socket->close();
        }
    }

    // 场景 3：CONNECT 域名（ATYP 0x03，localhost 解析）。
    BeginScenario("SOCKS5", "CONNECT 域名 localhost → 中继");
    {
        Socket::Ptr socket = Dial(proxy_port);
        BENCH_CHECK(static_cast<bool>(socket));
        if (socket) {
            BENCH_CHECK(Greeting(*socket));
            std::uint8_t reply = 0xFF;
            BENCH_CHECK(ConnectDomain(*socket, "localhost", upstream_port, &reply));
            BENCH_CHECK_EQ(static_cast<int>(reply), 0);
            const std::string payload = "domain-ok";
            BENCH_CHECK(WriteAll(*socket, payload.data(), payload.size()));
            std::string echoed(payload.size(), '\0');
            BENCH_CHECK(ReadExact(*socket, &echoed[0], echoed.size()));
            BENCH_CHECK_EQ(echoed, payload);
            (void)socket->close();
        }
    }

    // 场景 4：CONNECT 到未监听端口 → 非 0 回复码。
    BeginScenario("SOCKS5", "CONNECT 失败端口 → 非 0 回复码");
    {
        Socket::Ptr socket = Dial(proxy_port);
        BENCH_CHECK(static_cast<bool>(socket));
        if (socket) {
            BENCH_CHECK(Greeting(*socket));
            std::uint8_t reply = 0;
            (void)ConnectIPv4(*socket, "127.0.0.1", 1, &reply);
            BENCH_CHECK(reply != 0);
            (void)socket->close();
        }
    }

    // 场景 5：客户端只提供用户名密码方法 → 05 FF（明确拒绝）。
    BeginScenario("SOCKS5", "只提供 0x02 认证 → 05 FF 拒绝");
    {
        Socket::Ptr socket = Dial(proxy_port);
        BENCH_CHECK(static_cast<bool>(socket));
        if (socket) {
            BENCH_CHECK(GreetingOnlyUserPass(*socket));
            (void)socket->close();
        }
    }

    proxy.Stop();
    proxy.Close();
    upstream.Stop();
}

// ---- 性能基准 ----

void BenchConnectRate() {
    EchoUpstream upstream;
    std::uint16_t upstream_port = 0;
    if (!upstream.Start(&upstream_port)) {
        ++g_failures;
        return;
    }
    Socks5ServerConfig config;
    config.listen_port = 0;
    config.max_connections = 64;
    config.handshake_timeout_ms = 3000;
    config.idle_timeout_ms = 3000;
    Socks5Server proxy(config);
    if (!proxy.Open() || !proxy.Start()) {
        ++g_failures;
        upstream.Stop();
        return;
    }
    const std::uint16_t proxy_port = proxy.local_port();
    const int kConnections = 200;
    ResultRow row;
    row.protocol = "SOCKS5";
    row.scenario = "握手 + CONNECT IPv4 + 关闭（200 次）";
    row.note = "连接建立速率；含代理 poll 循环";
    std::uint64_t best_elapsed = UINT64_MAX;
    int failures = 0;
    std::vector<std::uint64_t> latencies;
    for (int round = 0; round < 3; ++round) {
        const std::uint64_t start = NowNs();
        for (int index = 0; index < kConnections; ++index) {
            const std::uint64_t connect_ns = NowNs();
            Socket::Ptr socket = Dial(proxy_port);
            if (!socket || !Greeting(*socket)) {
                ++failures;
                break;
            }
            std::uint8_t reply = 0xFF;
            if (!ConnectIPv4(*socket, "127.0.0.1", upstream_port, &reply) || reply != 0) {
                ++failures;
                break;
            }
            latencies.push_back(NowNs() - connect_ns);
            (void)socket->close();
        }
        const std::uint64_t elapsed = NowNs() - start;
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
        if (failures != 0) {
            break;
        }
    }
    proxy.Stop();
    proxy.Close();
    upstream.Stop();
    BENCH_CHECK_EQ(failures, 0);
    if (best_elapsed == UINT64_MAX) {
        return;
    }
    row.ops = static_cast<std::uint64_t>(kConnections);
    row.ops_per_second =
        static_cast<double>(kConnections) * 1e9 / static_cast<double>(best_elapsed);
    row.latency = Summarize(latencies);
    Record(std::move(row));
}

void BenchRelayThroughput() {
    EchoUpstream upstream;
    std::uint16_t upstream_port = 0;
    if (!upstream.Start(&upstream_port)) {
        ++g_failures;
        return;
    }
    Socks5ServerConfig config;
    config.listen_port = 0;
    config.max_connections = 16;
    config.handshake_timeout_ms = 3000;
    config.idle_timeout_ms = 10000;
    config.buffer_bytes = 256 * 1024;
    Socks5Server proxy(config);
    if (!proxy.Open() || !proxy.Start()) {
        ++g_failures;
        upstream.Stop();
        return;
    }
    const std::uint16_t proxy_port = proxy.local_port();
    const std::size_t kBytes = 4 * 1024 * 1024;
    const std::size_t kChunk = 64 * 1024;
    std::vector<char> chunk(kChunk, 'r');
    std::uint64_t best_elapsed = UINT64_MAX;
    int failures = 0;
    for (int round = 0; round < 2; ++round) {
        Socket::Ptr socket = Dial(proxy_port);
        if (!socket || !Greeting(*socket)) {
            ++failures;
            break;
        }
        std::uint8_t reply = 0xFF;
        if (!ConnectIPv4(*socket, "127.0.0.1", upstream_port, &reply) || reply != 0) {
            ++failures;
            break;
        }
        std::size_t echoed = 0;
        const std::uint64_t start = NowNs();
        std::thread reader([&]() {
            std::vector<char> buffer(64 * 1024);
            while (echoed < kBytes) {
                const auto result = socket->recv(buffer.data(), buffer.size());
                if (result.transferred <= 0) {
                    break;
                }
                echoed += static_cast<std::size_t>(result.transferred);
            }
        });
        std::size_t sent = 0;
        while (sent < kBytes) {
            const std::size_t step = std::min(kChunk, kBytes - sent);
            if (!WriteAll(*socket, chunk.data(), step)) {
                break;
            }
            sent += step;
        }
        reader.join();
        const std::uint64_t elapsed = NowNs() - start;
        (void)socket->close();
        if (sent != kBytes || echoed != kBytes) {
            ++failures;
            break;
        }
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
    }
    proxy.Stop();
    proxy.Close();
    upstream.Stop();
    BENCH_CHECK_EQ(failures, 0);
    if (best_elapsed == UINT64_MAX) {
        return;
    }
    ResultRow row;
    row.protocol = "SOCKS5";
    row.scenario = "中继双向 4MB（64KB 块）";
    row.note = "单连接经代理回环；MB/s 见报告";
    // 吞吐按「单方向 MB/s」记：4MB 去 + 4MB 回，两个方向各自计时。
    row.ops = static_cast<std::uint64_t>(kBytes / kChunk);
    row.ops_per_second = static_cast<double>(kBytes) * 1e9 /
                         static_cast<double>(best_elapsed) / (1024.0 * 1024.0);
    Record(std::move(row));
    std::printf("[NOTE] SOCKS5 中继: %.1f MB/s（单方向，4MB 回环）\n",
                row.ops_per_second);
}

void RunPerformance() {
    BenchConnectRate();
    BenchRelayThroughput();
}

}  // namespace socks_bench

// ===========================================================================
// 7. LinkServer：真实 UDP（PeerHub 客户端）+ 可靠/不可靠/广播/断线
// ===========================================================================

namespace link_bench {

using go2cpp::bento::LinkPeerId;
using go2cpp::bento::LinkReliability;
using go2cpp::bento::LinkSendResult;
using go2cpp::bento::LinkServer;
using go2cpp::bento::LinkServerConfig;
using go2cpp::bento::NowMs;
using go2cpp::bento::PeerHub;
using go2cpp::bento::PeerHubConfig;
using go2cpp::bento::SendStatus;

constexpr std::uint8_t kStreamChannel = 1;
constexpr std::uint8_t kDatagramChannel = 2;

PeerHubConfig MakeClientConfig() {
    PeerHubConfig config;
    config.require_encryption = false;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = 5000;
    config.handshake_timeout_ms = 3000;
    config.hello_initial_pto_ms = 50;
    config.hello_max_pto_ms = 500;
    config.hello_max_attempts = 16;
    return config;
}

LinkServerConfig MakeServerConfig(std::size_t max_peers = 16) {
    LinkServerConfig config;
    config.hub.bind_address = "127.0.0.1";
    config.hub.bind_port = 0;
    config.hub.heartbeat_interval_ms = 100;
    config.hub.peer_timeout_ms = 3000;
    config.hub.handshake_timeout_ms = 3000;
    config.hub.hello_initial_pto_ms = 50;
    config.hub.hello_max_pto_ms = 500;
    config.hub.hello_max_attempts = 16;
    config.hub.max_peers = max_peers;
    config.hub.require_encryption = false;
    config.poll_timeout_ms = 2;
    return config;
}

/// 客户端：PeerHub 主动端 + 主线程驱动（RunOnce）。
class Client {
public:
    Client() : hub(MakeClientConfig()) {}

    bool Open() {
        std::string error;
        if (!hub.Open(&error)) {
            std::fprintf(stderr, "[FAIL] PeerHub::Open: %s\n", error.c_str());
            return false;
        }
        hub.on_peer_connected = [this](std::uint32_t) { connected = true; };
        hub.on_stream = [this](std::uint32_t, std::uint8_t channel, const std::uint8_t* data,
                               std::size_t bytes) {
            Received message;
            message.payload.assign(data, data + bytes);
            message.channel = channel;
            streams.push_back(std::move(message));
        };
        hub.on_datagram = [this](std::uint32_t, std::uint8_t channel, const std::uint8_t* data,
                                 std::size_t bytes) {
            Received message;
            message.payload.assign(data, data + bytes);
            message.channel = channel;
            datagrams.push_back(std::move(message));
        };
        return true;
    }

    bool Connect(std::uint16_t port, std::uint32_t id) {
        peer_id = id;
        std::string error;
        if (!hub.AddPeer(peer_id, "127.0.0.1", port, &error)) {
            std::fprintf(stderr, "[FAIL] PeerHub::AddPeer: %s\n", error.c_str());
            return false;
        }
        return PumpUntil([this]() { return connected; }, 5000);
    }

    template <typename Predicate>
    bool PumpUntil(Predicate done, int timeout_ms) {
        const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout_ms);
        while (NowMs() < deadline) {
            hub.RunOnce(1);
            if (done()) {
                return true;
            }
        }
        return done();
    }

    void PumpFor(int duration_ms) {
        const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(duration_ms);
        while (NowMs() < deadline) {
            hub.RunOnce(1);
        }
    }

    bool SendReliable(std::uint8_t channel, const std::vector<std::uint8_t>& payload,
                      int timeout_ms = 5000) {
        const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout_ms);
        while (NowMs() < deadline) {
            const SendStatus status =
                hub.TrySendStream(peer_id, channel, payload.data(), payload.size());
            if (status == SendStatus::kOk) {
                return true;
            }
            if (status == SendStatus::kError) {
                return false;
            }
            hub.RunOnce(1);
        }
        return false;
    }

    bool SendUnreliable(std::uint8_t channel, const std::vector<std::uint8_t>& payload) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (hub.SendDatagram(peer_id, channel, payload.data(), payload.size())) {
                return true;
            }
            hub.RunOnce(1);
        }
        return false;
    }

    struct Received {
        std::vector<std::uint8_t> payload;
        std::uint8_t channel{0};
    };

    PeerHub hub;
    std::uint32_t peer_id{1};
    bool connected{false};
    std::vector<Received> streams;
    std::vector<Received> datagrams;
};

/// 服务端事件探针（接收线程写、主线程读，统一加锁）。
class ServerProbe {
public:
    /// @param parse_latency true 时把 payload 前 8 字节当发送时间戳，记录单向延迟
    ///        （性能基准用；正确性场景的 payload 前 4 字节是序号，不能解析）。
    void Install(LinkServer& server, bool parse_latency = false) {
        server.OnPeerConnected([this](LinkPeerId peer) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_connected.push_back(peer);
        });
        server.OnPeerDisconnected([this](LinkPeerId peer, const char* reason) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_disconnected.push_back(peer);
            m_last_reason = reason == nullptr ? std::string() : std::string(reason);
        });
        server.OnStream([this, parse_latency](LinkPeerId peer, std::uint8_t channel,
                                              const std::uint8_t* data, std::size_t bytes) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (parse_latency && bytes >= 8) {
                std::uint64_t send_ns = 0;
                std::memcpy(&send_ns, data, sizeof(send_ns));
                const std::uint64_t now = NowNs();
                if (now > send_ns) {
                    m_latencies.push_back(now - send_ns);
                }
            }
            m_streams.push_back(Received{peer, channel, std::vector<std::uint8_t>(data, data + bytes)});
        });
        server.OnDatagram([this, parse_latency](LinkPeerId peer, std::uint8_t channel,
                                                const std::uint8_t* data, std::size_t bytes) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (parse_latency && bytes >= 8) {
                std::uint64_t send_ns = 0;
                std::memcpy(&send_ns, data, sizeof(send_ns));
                const std::uint64_t now = NowNs();
                if (now > send_ns) {
                    m_latencies.push_back(now - send_ns);
                }
            }
            m_datagrams.push_back(
                Received{peer, channel, std::vector<std::uint8_t>(data, data + bytes)});
        });
    }

    struct Received {
        std::uint32_t peer{0};
        std::uint8_t channel{0};
        std::vector<std::uint8_t> payload;
    };

    std::vector<std::uint64_t> Latencies() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_latencies;
    }

    std::size_t ConnectedCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_connected.size();
    }

    /// 按连接顺序返回服务端视角的 peer id。
    std::vector<std::uint32_t> Connected() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_connected;
    }

    std::size_t DisconnectedCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_disconnected.size();
    }

    std::size_t StreamCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_streams.size();
    }

    std::size_t DatagramCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_datagrams.size();
    }

    std::vector<Received> Streams() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_streams;
    }

    std::vector<Received> Datagrams() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_datagrams;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connected.clear();
        m_disconnected.clear();
        m_streams.clear();
        m_datagrams.clear();
        m_latencies.clear();
    }

    std::string LastDisconnectReason() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_reason;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::uint32_t> m_connected;
    std::vector<std::uint32_t> m_disconnected;
    std::vector<Received> m_streams;
    std::vector<Received> m_datagrams;
    std::vector<std::uint64_t> m_latencies;
    std::string m_last_reason;
};

/// 构造一条带序号与校验填充的载荷。
std::vector<std::uint8_t> MakePayload(std::uint32_t seq, std::size_t bytes) {
    std::vector<std::uint8_t> payload(std::max<std::size_t>(bytes, 8), 0);
    payload[0] = static_cast<std::uint8_t>(seq >> 24);
    payload[1] = static_cast<std::uint8_t>(seq >> 16);
    payload[2] = static_cast<std::uint8_t>(seq >> 8);
    payload[3] = static_cast<std::uint8_t>(seq);
    payload[4] = 0x5A;
    for (std::size_t index = 5; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>((seq * 31U + index * 7U) & 0xFFU);
    }
    return payload;
}

/// 校验载荷并返回序号；不匹配返回 0xFFFFFFFF。
std::uint32_t VerifyPayload(const std::uint8_t* data, std::size_t bytes) {
    if (bytes < 8 || data[4] != 0x5A) {
        return 0xFFFFFFFFU;
    }
    const std::uint32_t seq = (static_cast<std::uint32_t>(data[0]) << 24) |
                              (static_cast<std::uint32_t>(data[1]) << 16) |
                              (static_cast<std::uint32_t>(data[2]) << 8) |
                              static_cast<std::uint32_t>(data[3]);
    for (std::size_t index = 5; index < bytes; ++index) {
        if (data[index] != static_cast<std::uint8_t>((seq * 31U + index * 7U) & 0xFFU)) {
            return 0xFFFFFFFFU;
        }
    }
    return seq;
}

// ---- 正确性场景 ----

/// 每个场景独立起一个 LinkServer：避免上一场景的 peer 还在超时窗口里，
/// 干扰「广播投递数 / 多 peer 隔离」这类依赖 peer 集合的断言。
struct ServerFixture {
    LinkServer server;
    ServerProbe probe;
    std::uint16_t port{0};

    bool Start(std::size_t max_peers = 16) {
        probe.Install(server);
        std::string error;
        if (!server.Listen("127.0.0.1", 0, MakeServerConfig(max_peers), &error)) {
            std::fprintf(stderr, "[FAIL] LinkServer::Listen: %s\n", error.c_str());
            return false;
        }
        port = server.local_port();
        return server.Start();
    }

    void Stop() { server.Stop(); }
};

void CheckCorrectness() {
    // 场景 1：可靠 STREAM 双向（200B 含序号校验）。
    BeginScenario("Link", "可靠 STREAM 双向 200B");
    {
        ServerFixture fixture;
        BENCH_CHECK(fixture.Start());
        Client client;
        BENCH_CHECK(client.Open());
        BENCH_CHECK(client.Connect(fixture.port, 1));
        const std::vector<std::uint8_t> outbound = MakePayload(7, 200);
        BENCH_CHECK(client.SendReliable(kStreamChannel, outbound));
        BENCH_CHECK(client.PumpUntil([&fixture]() { return fixture.probe.StreamCount() >= 1; },
                                     3000));
        const auto received = fixture.probe.Streams();
        BENCH_CHECK_EQ(received.size(), static_cast<std::size_t>(1));
        if (!received.empty()) {
            BENCH_CHECK_EQ(VerifyPayload(received[0].payload.data(), received[0].payload.size()),
                           7U);
        }
        // 服务端回发，客户端收。peer id 以服务端 `Peers()` 为准：客户端
        // `AddPeer(id)` 的 id 只是本地句柄，服务端会分配自己的 peer 编号。
        const std::vector<std::uint32_t> peers = fixture.server.Peers();
        BENCH_CHECK_EQ(peers.size(), static_cast<std::size_t>(1));
        const std::vector<std::uint8_t> reply = MakePayload(8, 64);
        BENCH_CHECK_EQ(static_cast<int>(fixture.server.Send(
                           peers.empty() ? 0U : peers[0], kStreamChannel, reply.data(),
                           reply.size(), LinkReliability::kReliable)),
                       static_cast<int>(LinkSendResult::kOk));
        BENCH_CHECK(client.PumpUntil([&client]() { return client.streams.size() >= 1; }, 3000));
        BENCH_CHECK_EQ(client.streams.size(), static_cast<std::size_t>(1));
        if (!client.streams.empty()) {
            BENCH_CHECK_EQ(VerifyPayload(client.streams[0].payload.data(),
                                         client.streams[0].payload.size()),
                           8U);
        }
        client.PumpFor(50);
        fixture.Stop();
    }

    // 场景 2：不可靠 DATAGRAM 双向。
    BeginScenario("Link", "不可靠 DATAGRAM 双向 100B");
    {
        ServerFixture fixture;
        BENCH_CHECK(fixture.Start());
        Client client;
        BENCH_CHECK(client.Open());
        BENCH_CHECK(client.Connect(fixture.port, 2));
        const std::vector<std::uint8_t> outbound = MakePayload(21, 100);
        BENCH_CHECK(client.SendUnreliable(kDatagramChannel, outbound));
        BENCH_CHECK(client.PumpUntil([&fixture]() { return fixture.probe.DatagramCount() >= 1; },
                                     3000));
        const auto datagrams = fixture.probe.Datagrams();
        BENCH_CHECK_EQ(datagrams.size(), static_cast<std::size_t>(1));
        if (!datagrams.empty()) {
            BENCH_CHECK_EQ(datagrams[0].channel, kDatagramChannel);
            BENCH_CHECK_EQ(VerifyPayload(datagrams[0].payload.data(), datagrams[0].payload.size()),
                           21U);
        }
        client.PumpFor(50);
        fixture.Stop();
    }

    // 场景 3：广播 —— 2 个 peer 都收到。
    BeginScenario("Link", "Broadcast 到 2 peers");
    {
        ServerFixture fixture;
        BENCH_CHECK(fixture.Start());
        Client first;
        Client second;
        BENCH_CHECK(first.Open());
        BENCH_CHECK(second.Open());
        BENCH_CHECK(first.Connect(fixture.port, 3));
        BENCH_CHECK(second.Connect(fixture.port, 4));
        BENCH_CHECK(second.PumpUntil([&fixture]() { return fixture.probe.ConnectedCount() >= 2; },
                                     5000));
        const std::vector<std::uint8_t> payload = MakePayload(31, 48);
        const std::size_t delivered = fixture.server.Broadcast(
            kStreamChannel, payload.data(), payload.size(), LinkReliability::kReliable);
        BENCH_CHECK_EQ(delivered, static_cast<std::size_t>(2));
        BENCH_CHECK(first.PumpUntil([&first]() { return first.streams.size() >= 1; }, 3000));
        BENCH_CHECK(second.PumpUntil([&second]() { return second.streams.size() >= 1; }, 3000));
        BENCH_CHECK_EQ(first.streams.size(), static_cast<std::size_t>(1));
        BENCH_CHECK_EQ(second.streams.size(), static_cast<std::size_t>(1));
        first.PumpFor(50);
        second.PumpFor(50);
        fixture.Stop();
    }

    // 场景 4：多 peer 单播隔离 —— 只发给 peer 5，peer 6 不应收到。
    BeginScenario("Link", "多 peer 单播隔离（A 收、B 不收）");
    {
        ServerFixture fixture;
        BENCH_CHECK(fixture.Start());
        Client first;
        Client second;
        BENCH_CHECK(first.Open());
        BENCH_CHECK(second.Open());
        BENCH_CHECK(first.Connect(fixture.port, 5));
        // 先等第一个 peer 出现在服务端集合里，才能确定它的服务端 peer id
        // 属于 first（而不是 second）。
        BENCH_CHECK(first.PumpUntil([&fixture]() { return fixture.probe.ConnectedCount() >= 1; },
                                    3000));
        const std::vector<std::uint32_t> first_peers = fixture.probe.Connected();
        BENCH_CHECK_EQ(first_peers.size(), static_cast<std::size_t>(1));
        const std::uint32_t first_peer = first_peers.empty() ? 0U : first_peers[0];
        BENCH_CHECK(second.Connect(fixture.port, 6));
        BENCH_CHECK(second.PumpUntil([&fixture]() { return fixture.probe.ConnectedCount() >= 2; },
                                     5000));
        first.streams.clear();
        second.streams.clear();
        const std::vector<std::uint8_t> payload = MakePayload(51, 32);
        // 可靠通道的发送窗口可能暂时满（kWouldBlock）：重试同一条消息即可，
        // 顺序不会被破坏。
        LinkSendResult send_result = LinkSendResult::kWouldBlock;
        for (int attempt = 0; attempt < 200 && send_result != LinkSendResult::kOk; ++attempt) {
            send_result = fixture.server.Send(first_peer, kStreamChannel, payload.data(),
                                              payload.size(), LinkReliability::kReliable);
            if (send_result != LinkSendResult::kOk) {
                first.hub.RunOnce(1);
            }
        }
        BENCH_CHECK_EQ(static_cast<int>(send_result), static_cast<int>(LinkSendResult::kOk));
        BENCH_CHECK(first.PumpUntil([&first]() { return first.streams.size() >= 1; }, 3000));
        second.PumpFor(300);
        BENCH_CHECK_EQ(first.streams.size(), static_cast<std::size_t>(1));
        BENCH_CHECK_EQ(second.streams.size(), static_cast<std::size_t>(0));
        first.PumpFor(50);
        fixture.Stop();
    }

    // 场景 5：断线 —— 客户端静默超过 peer_timeout，服务端触发断开回调。
    BeginScenario("Link", "客户端静默 → 服务端 peer 超时断开回调");
    {
        ServerFixture fixture;
        BENCH_CHECK(fixture.Start());
        {
            Client client;
            BENCH_CHECK(client.Open());
            BENCH_CHECK(client.Connect(fixture.port, 7));
            // 不跑 RunOnce、不发心跳，等服务端超时（3s）。
        }
        const bool disconnected = [&fixture]() {
            const std::uint64_t deadline = NowMs() + 8000;
            while (NowMs() < deadline) {
                if (fixture.probe.DisconnectedCount() > 0) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return fixture.probe.DisconnectedCount() > 0;
        }();
        BENCH_CHECK(disconnected);
        BENCH_CHECK(!fixture.probe.LastDisconnectReason().empty());
        fixture.Stop();
    }
}

// ---- 性能基准 ----

void BenchReliable() {
    LinkServer server;
    ServerProbe probe;
    probe.Install(server, true);
    std::string error;
    if (!server.Listen("127.0.0.1", 0, MakeServerConfig(), &error) || !server.Start()) {
        ++g_failures;
        return;
    }
    const std::uint16_t port = server.local_port();
    Client client;
    if (!client.Open() || !client.Connect(port, 11)) {
        ++g_failures;
        server.Stop();
        return;
    }
    const int kMessages = 2000;
    std::vector<std::uint8_t> payload(64, 0x5A);
    std::uint64_t best_elapsed = UINT64_MAX;
    std::vector<std::uint64_t> latencies;
    for (int round = 0; round < 3; ++round) {
        probe.Clear();
        const std::size_t target = static_cast<std::size_t>(kMessages);
        const std::uint64_t start = NowNs();
        int sent = 0;
        while (sent < kMessages) {
            const std::uint64_t send_ns = NowNs();
            std::memcpy(payload.data(), &send_ns, sizeof(send_ns));
            const SendStatus status =
                client.hub.TrySendStream(client.peer_id, kStreamChannel, payload.data(),
                                         payload.size());
            if (status == SendStatus::kOk) {
                ++sent;
                continue;
            }
            if (status == SendStatus::kError) {
                break;
            }
            client.hub.RunOnce(0);
        }
        // 排空接收：直到服务端计数达到 target 或超时。
        const std::uint64_t deadline = NowNs() + 5000000000ULL;
        while (probe.StreamCount() < target && NowNs() < deadline) {
            client.hub.RunOnce(1);
        }
        const std::uint64_t elapsed = NowNs() - start;
        BENCH_CHECK_EQ(probe.StreamCount(), target);
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
        // 单向延迟单独采样：串行 30 条（每条等对端回调后再发下一条）。
        // 若直接用吞吐阶段的样本，测到的是「发送队列排队时间」（流水线
        // 2000 条时 p50 可达数十毫秒），不是网络/协议延迟。
        for (int index = 0; index < 30; ++index) {
            const std::size_t before = probe.StreamCount();
            const std::uint64_t send_ns = NowNs();
            std::memcpy(payload.data(), &send_ns, sizeof(send_ns));
            const std::uint64_t send_deadline = NowNs() + 100000000ULL;
            while (client.hub.TrySendStream(client.peer_id, kStreamChannel, payload.data(),
                                            payload.size()) != SendStatus::kOk &&
                   NowNs() < send_deadline) {
                client.hub.RunOnce(1);
            }
            const std::uint64_t deadline = NowNs() + 200000000ULL;
            while (probe.StreamCount() == before && NowNs() < deadline) {
                client.hub.RunOnce(1);
            }
        }
        {
            const std::vector<std::uint64_t> all = probe.Latencies();
            latencies.assign(all.size() > 30 ? all.end() - 30 : all.begin(), all.end());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    server.Stop();
    if (best_elapsed == UINT64_MAX) {
        return;
    }
    ResultRow row;
    row.protocol = "Link";
    row.scenario = "可靠 STREAM 64B 单 peer 单向";
    row.note = "UDP 回环；含 ACK/重传与拥塞控制";
    row.ops = static_cast<std::uint64_t>(kMessages);
    row.ops_per_second = static_cast<double>(kMessages) * 1e9 / static_cast<double>(best_elapsed);
    row.latency = Summarize(latencies);
    Record(std::move(row));
}

void BenchDatagram() {
    LinkServer server;
    ServerProbe probe;
    probe.Install(server, true);
    std::string error;
    if (!server.Listen("127.0.0.1", 0, MakeServerConfig(), &error) || !server.Start()) {
        ++g_failures;
        return;
    }
    const std::uint16_t port = server.local_port();
    Client client;
    if (!client.Open() || !client.Connect(port, 12)) {
        ++g_failures;
        server.Stop();
        return;
    }
    const int kMessages = 2000;
    std::vector<std::uint8_t> payload(64, 0x5A);
    std::uint64_t best_elapsed = UINT64_MAX;
    std::size_t best_received = 0;
    std::vector<std::uint64_t> latencies;
    for (int round = 0; round < 3; ++round) {
        probe.Clear();
        const std::uint64_t start = NowNs();
        int sent = 0;
        for (int index = 0; index < kMessages; ++index) {
            const std::uint64_t send_ns = NowNs();
            std::memcpy(payload.data(), &send_ns, sizeof(send_ns));
            if (client.hub.SendDatagram(client.peer_id, kDatagramChannel, payload.data(),
                                        payload.size())) {
                ++sent;
            }
            client.hub.RunOnce(0);
            // 不可靠通道直写 UDP：不节流时会撑爆内核接收缓冲，到达率跌到
            // 个位数百分比，测出来的是「丢包率」而不是吞吐。每 16 条让出
            // 100µs，把发送速率压到回环栈能稳定吸收的量级。
            if ((index % 16) == 15) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        const std::uint64_t deadline = NowNs() + 3000000000ULL;
        while (probe.DatagramCount() < static_cast<std::size_t>(sent) && NowNs() < deadline) {
            client.hub.RunOnce(1);
        }
        const std::uint64_t elapsed = NowNs() - start;
        const std::size_t received = probe.DatagramCount();
        // 不可靠通道允许丢包：要求 ≥80% 到达，否则数据说明不了吞吐。
        BENCH_CHECK(received >= static_cast<std::size_t>(sent) * 80 / 100);
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
            best_received = received;
            latencies = probe.Latencies();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    server.Stop();
    if (best_elapsed == UINT64_MAX) {
        return;
    }
    ResultRow row;
    row.protocol = "Link";
    row.scenario = "不可靠 DATAGRAM 64B 单 peer 单向（节流）";
    row.note = "UDP 回环；不重传；到达率 " +
               std::to_string(static_cast<int>(best_received * 100 / kMessages)) + "%";
    row.ops = static_cast<std::uint64_t>(kMessages);
    row.ops_per_second = static_cast<double>(kMessages) * 1e9 / static_cast<double>(best_elapsed);
    row.latency = Summarize(latencies);
    Record(std::move(row));
}

void RunPerformance() {
    BenchReliable();
    BenchDatagram();
}

}  // namespace link_bench

// ===========================================================================
// 8. OmniNetServer M1：AsLinkServer（真实 UDP）/ AsHttpServer（真实 TCP+SSE）
// ===========================================================================

namespace omni_bench {

using go2cpp::bento::omni::BindEndpoint;
using go2cpp::bento::omni::ConcurrencyKind;
using go2cpp::bento::omni::IoMode;
using go2cpp::bento::omni::NetServer;
using go2cpp::bento::omni::OmniNetConfig;
using go2cpp::bento::omni::ProtocolKind;
using go2cpp::bento::omni::TransportKind;
using go2cpp::bento::LinkPeerId;

OmniNetConfig UdpLinkConfig() {
    OmniNetConfig config;
    config.mode = IoMode::kNonBlocking;
    config.transport = TransportKind::kUdp;
    config.protocols = {ProtocolKind::kLink};
    config.concurrency = ConcurrencyKind::kGmp;
    config.bind = BindEndpoint{"127.0.0.1", 0};
    return config;
}

OmniNetConfig TcpHttpConfig() {
    OmniNetConfig config;
    config.mode = IoMode::kNonBlocking;
    config.transport = TransportKind::kTcp;
    config.protocols = {ProtocolKind::kHttp};
    config.concurrency = ConcurrencyKind::kGmp;
    config.bind = BindEndpoint{"127.0.0.1", 0};
    // keep-alive 单连接会连续发上千个请求；`Limits` 默认 100 会让服务端
    // 主动关连接，吞吐测试就变成「连接重建速率」。基准显式放宽。
    config.limits.max_requests_per_connection = 100000;
    return config;
}

// ---- 正确性场景 ----

void CheckCorrectness() {
    // 场景 1 + 2：UDP 配置下 AsLinkServer 真实收发，且 HTTP 视图无效。
    BeginScenario("OMNI", "UDP: AsLinkServer 真实收发 + 视图交叉校验");
    {
        std::string error;
        std::unique_ptr<NetServer> server = NetServer::Create(UdpLinkConfig(), &error);
        BENCH_CHECK(server != nullptr);
        if (!server) {
            std::fprintf(stderr, "[FAIL] NetServer::Create(UDP): %s\n", error.c_str());
        } else {
            BENCH_CHECK(server->AsLinkServer().valid());
            BENCH_CHECK(!server->AsHttpServer().valid());
            std::atomic<int> received{0};
            std::atomic<int> bytes{0};
            std::atomic<std::uint32_t> server_peer{0};
            server->AsLinkServer().OnPeerConnected(
                [&server_peer](LinkPeerId peer) { server_peer.store(peer); });
            server->AsLinkServer().OnStream(
                [&received, &bytes](LinkPeerId, std::uint8_t, const std::uint8_t*,
                                    std::size_t size) {
                    received.fetch_add(1, std::memory_order_relaxed);
                    bytes.store(static_cast<int>(size), std::memory_order_relaxed);
                });
            BENCH_CHECK(server->Start(&error));
            const std::uint16_t port = server->Port();
            BENCH_CHECK(port != 0);
            link_bench::Client client;
            BENCH_CHECK(client.Open());
            BENCH_CHECK(client.Connect(port, 21));
            const std::vector<std::uint8_t> payload = link_bench::MakePayload(91, 128);
            BENCH_CHECK(client.SendReliable(1, payload));
            BENCH_CHECK(client.PumpUntil([&received]() { return received.load() >= 1; }, 3000));
            BENCH_CHECK_EQ(bytes.load(), 128);
            // 服务端回发：peer id 以服务端 OnPeerConnected 回调给出的为准
            // （客户端 AddPeer 的 id 必须与之一致，但基准不假设这一点）。
            BENCH_CHECK(server_peer.load() != 0);
            bool delivered = false;
            for (int attempt = 0; attempt < 50 && !delivered; ++attempt) {
                delivered =
                    server->AsLinkServer().Send(server_peer.load(), 1, payload.data(),
                                                payload.size()) ==
                    go2cpp::bento::LinkSendResult::kOk;
                client.hub.RunOnce(1);
            }
            BENCH_CHECK(delivered);
            BENCH_CHECK(client.PumpUntil([&client]() { return client.streams.size() >= 1; },
                                         3000));
            server->Stop();
        }
    }

    // 场景 3：TCP 配置下 AsHttpServer 真实 GET。
    BeginScenario("OMNI", "TCP: AsHttpServer 真实 GET /hello");
    {
        std::string error;
        std::unique_ptr<NetServer> server = NetServer::Create(TcpHttpConfig(), &error);
        BENCH_CHECK(server != nullptr);
        if (!server) {
            std::fprintf(stderr, "[FAIL] NetServer::Create(TCP): %s\n", error.c_str());
        } else {
            BENCH_CHECK(server->AsHttpServer().valid());
            BENCH_CHECK(!server->AsLinkServer().valid());
            server->AsHttpServer().OnRequest([](const go2cpp::http::Request& request) {
                go2cpp::http::Response response;
                response.header.set("Content-Type", "text/plain");
                response.body = "omni:" + request.path();
                return response;
            });
            std::atomic<int> sse_events{0};
            server->AsHttpServer().AsSse(
                "/events", [&sse_events](const go2cpp::http::Request&, go2cpp::sse::Stream& stream) {
                    for (int index = 0; index < 3; ++index) {
                        go2cpp::sse::Event event;
                        event.event = "tick";
                        event.data = "omni-" + std::to_string(index);
                        if (!stream.send(event).ok()) {
                            break;
                        }
                        sse_events.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            BENCH_CHECK(server->Start(&error));
            const std::uint16_t port = server->Port();
            http_bench::Client client;
            BENCH_CHECK(client.Connect(port));
            BENCH_CHECK(client.WriteAll(http_bench::MakeGet("/hello")));
            go2cpp::http::Response response;
            BENCH_CHECK(client.ReadResponse(response));
            BENCH_CHECK_EQ(static_cast<int>(response.status), 200);
            BENCH_CHECK_EQ(response.body, std::string("omni:/hello"));
            client.Close();

            // 场景 4：TCP 配置下 SSE 推流。
            BeginScenario("OMNI", "TCP: AsHttpServer 真实 SSE 推流 3 事件");
            http_bench::Client sse_client;
            BENCH_CHECK(sse_client.Connect(port));
            BENCH_CHECK(sse_client.WriteAll(http_bench::MakeGet("/events")));
            std::string header;
            char buffer[512];
            while (header.find("\r\n\r\n") == std::string::npos) {
                const auto count = sse_client.socket()->recv(buffer, sizeof(buffer));
                if (count.transferred <= 0) {
                    break;
                }
                header.append(buffer, static_cast<std::size_t>(count.transferred));
            }
            BENCH_CHECK(header.find("text/event-stream") != std::string::npos);
            std::vector<go2cpp::sse::Event> events;
            go2cpp::sse::Parser parser;
            const std::size_t body_start = header.find("\r\n\r\n");
            if (body_start != std::string::npos && header.size() > body_start + 4) {
                BENCH_CHECK(parser.feed(header.substr(body_start + 4), events).ok());
            }
            while (events.size() < 3) {
                auto more = http_bench::ReadSseEvents(*sse_client.socket(), 3 - events.size(), 3000);
                if (more.empty()) {
                    break;
                }
                for (auto& event : more) {
                    events.push_back(std::move(event));
                }
            }
            BENCH_CHECK_EQ(events.size(), static_cast<std::size_t>(3));
            sse_client.Close();
            server->Stop();
        }
    }

    // 场景 5：Stop 后端口释放，新实例可重新绑定同一端口。
    BeginScenario("OMNI", "Stop 后端口释放（同端口重绑）");
    {
        std::string error;
        std::unique_ptr<NetServer> first = NetServer::Create(TcpHttpConfig(), &error);
        BENCH_CHECK(first != nullptr);
        if (first) {
            BENCH_CHECK(first->Start(&error));
            const std::uint16_t port = first->Port();
            first->Stop();
            OmniNetConfig config = TcpHttpConfig();
            config.bind.port = port;
            std::unique_ptr<NetServer> second = NetServer::Create(config, &error);
            BENCH_CHECK(second != nullptr);
            if (second) {
                BENCH_CHECK(second->Start(&error));
                BENCH_CHECK_EQ(second->Port(), port);
                second->Stop();
            }
        }
    }
}

// ---- 性能基准 ----

void BenchLinkThroughput() {
    std::string error;
    std::unique_ptr<NetServer> server = NetServer::Create(UdpLinkConfig(), &error);
    if (!server) {
        ++g_failures;
        return;
    }
    std::atomic<int> received{0};
    std::mutex latency_mutex;
    std::vector<std::uint64_t> latencies;
    // 回调必须在 Start() 之前注册：Start 会立刻起接收线程，晚注册会漏掉
    // 最前面的帧（基准里表现为「已经发出但计数永远差几条」）。
    server->AsLinkServer().OnStream(
        [&received, &latency_mutex, &latencies](LinkPeerId, std::uint8_t, const std::uint8_t* data,
                                                std::size_t size) {
            const int count = received.fetch_add(1, std::memory_order_relaxed);
            // 只采样 1/4，减少锁对吞吐的影响；延迟样本仍足够算分位。
            if (size >= 8 && (count % 4) == 0) {
                std::uint64_t send_ns = 0;
                std::memcpy(&send_ns, data, sizeof(send_ns));
                const std::uint64_t now = NowNs();
                if (now > send_ns) {
                    std::lock_guard<std::mutex> lock(latency_mutex);
                    latencies.push_back(now - send_ns);
                }
            }
        });
    if (!server->Start(&error)) {
        ++g_failures;
        return;
    }
    const std::uint16_t port = server->Port();
    link_bench::Client client;
    if (!client.Open() || !client.Connect(port, 31)) {
        ++g_failures;
        server->Stop();
        return;
    }
    const int kMessages = 2000;
    std::vector<std::uint8_t> payload(64, 0x5A);
    std::uint64_t best_elapsed = UINT64_MAX;
    for (int round = 0; round < 3; ++round) {
        received.store(0);
        const std::uint64_t start = NowNs();
        int sent = 0;
        while (sent < kMessages) {
            const std::uint64_t send_ns = NowNs();
            std::memcpy(payload.data(), &send_ns, sizeof(send_ns));
            const auto status = client.hub.TrySendStream(client.peer_id, 1, payload.data(),
                                                         payload.size());
            if (status == go2cpp::bento::SendStatus::kOk) {
                ++sent;
                continue;
            }
            if (status == go2cpp::bento::SendStatus::kError) {
                break;
            }
            client.hub.RunOnce(0);
        }
        const std::uint64_t deadline = NowNs() + 5000000000ULL;
        while (received.load() < kMessages && NowNs() < deadline) {
            client.hub.RunOnce(1);
        }
        const std::uint64_t elapsed = NowNs() - start;
        BENCH_CHECK_EQ(received.load(), kMessages);
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    server->Stop();
    if (best_elapsed == UINT64_MAX) {
        return;
    }
    ResultRow row;
    row.protocol = "OMNI";
    row.scenario = "AsLinkServer 可靠 64B 单向（真实 UDP）";
    row.note = "OmniNetServer M1 UDP 路径";
    row.ops = static_cast<std::uint64_t>(kMessages);
    row.ops_per_second = static_cast<double>(kMessages) * 1e9 / static_cast<double>(best_elapsed);
    {
        std::lock_guard<std::mutex> lock(latency_mutex);
        row.latency = Summarize(latencies);
    }
    Record(std::move(row));
}

void BenchHttpThroughput() {
    std::string error;
    std::unique_ptr<NetServer> server = NetServer::Create(TcpHttpConfig(), &error);
    if (!server) {
        ++g_failures;
        return;
    }
    server->AsHttpServer().OnRequest([](const go2cpp::http::Request&) {
        go2cpp::http::Response response;
        response.body = "omni-ok";
        return response;
    });
    if (!server->Start(&error)) {
        ++g_failures;
        return;
    }
    const std::uint16_t port = server->Port();
    const int kRequests = 1500;
    std::uint64_t best_elapsed = UINT64_MAX;
    std::vector<std::uint64_t> latency;
    for (int round = 0; round < 3; ++round) {
        http_bench::Client client;
        if (!client.Connect(port)) {
            ++g_failures;
            break;
        }
        latency.clear();
        const std::uint64_t start = NowNs();
        int failed = 0;
        for (int index = 0; index < kRequests; ++index) {
            const std::uint64_t send_ns = NowNs();
            if (!client.WriteAll(http_bench::MakeGet("/hello"))) {
                ++failed;
                break;
            }
            go2cpp::http::Response response;
            if (!client.ReadResponse(response) || response.body != "omni-ok") {
                ++failed;
                break;
            }
            latency.push_back(NowNs() - send_ns);
        }
        const std::uint64_t elapsed = NowNs() - start;
        client.Close();
        BENCH_CHECK_EQ(failed, 0);
        if (elapsed < best_elapsed) {
            best_elapsed = elapsed;
        }
    }
    server->Stop();
    if (best_elapsed == UINT64_MAX) {
        return;
    }
    ResultRow row;
    row.protocol = "OMNI";
    row.scenario = "AsHttpServer GET 单连接串行（真实 TCP）";
    row.note = "OmniNetServer M1 TCP 路径";
    row.ops = static_cast<std::uint64_t>(kRequests);
    row.ops_per_second =
        static_cast<double>(kRequests) * 1e9 / static_cast<double>(best_elapsed);
    row.latency = Summarize(latency);
    Record(std::move(row));
}

void RunPerformance() {
    BenchLinkThroughput();
    BenchHttpThroughput();
}

}  // namespace omni_bench

// ===========================================================================
// 3. 写路径三档对比：单写 / bufio / bufio+batch
// ===========================================================================

namespace write_path {

using go2cpp::bufio::AsBytes;
using go2cpp::bufio::BatchConfig;
using go2cpp::bufio::FlushPolicy;
using go2cpp::bufio::Writer;
using go2cpp::net::DialTCP;
using go2cpp::net::Socket;
using go2cpp::net::SocketBatchWriter;

/// 一对已连接的 TCP 回环 socket：client 发送，drain 线程排空 server 侧。
struct TcpPair {
    Socket::Ptr client;
    Socket::Ptr server;
    std::thread drain;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> received{0};

    bool Open() {
        Socket::Ptr listener = Socket::CreateTCP();
        const auto bind_address = go2cpp::net::IPv4Address::Create("127.0.0.1", 0);
        if (!listener || !bind_address || !listener->bind(*bind_address) || !listener->listen(4)) {
            return false;
        }
        const auto local = listener->local_address();
        const auto local_ip = std::dynamic_pointer_cast<go2cpp::net::IPAddress>(local);
        if (!local_ip) {
            return false;
        }
        client = DialTCP("127.0.0.1", local_ip->port(), std::chrono::milliseconds(3000));
        if (!client) {
            return false;
        }
        server = listener->accept(std::chrono::milliseconds(3000));
        if (!server) {
            return false;
        }
        SetNoDelay(client);
        SetNoDelay(server);
        client->set_send_timeout(std::chrono::milliseconds(5000));
        server->set_recv_timeout(std::chrono::milliseconds(5));
        // 大 SO_SNDBUF/RCVBUF 降低回环背压对测量结果的干扰。
        int buffer_bytes = 4 * 1024 * 1024;
        (void)client->set_option(SOL_SOCKET, SO_SNDBUF, &buffer_bytes, sizeof(buffer_bytes));
        (void)server->set_option(SOL_SOCKET, SO_RCVBUF, &buffer_bytes, sizeof(buffer_bytes));
        drain = std::thread([this]() {
            char buffer[256 * 1024];
            while (!stop.load(std::memory_order_acquire)) {
                const auto result = server->recv(buffer, sizeof(buffer));
                if (result.transferred > 0) {
                    received.fetch_add(static_cast<std::uint64_t>(result.transferred),
                                       std::memory_order_relaxed);
                }
            }
        });
        return true;
    }

    void Close() {
        stop.store(true, std::memory_order_release);
        if (client) {
            (void)client->close();
        }
        if (drain.joinable()) {
            drain.join();
        }
        if (server) {
            (void)server->close();
        }
        client.reset();
        server.reset();
    }
};

/// 三档结果（每档：耗时 ns、系统调用次数）。
struct WriteTier {
    std::uint64_t elapsed_ns{0};
    std::uint64_t syscalls{0};
    std::uint64_t failed{0};
};

/// 档 A：每条消息一次 `send(2)`；部分写循环补齐（4096B 大消息会触发）。
WriteTier SingleWrite(Socket::Ptr socket, const std::string& frame, int messages) {
    WriteTier tier;
    const std::uint64_t start = NowNs();
    std::uint64_t calls = 0;
    for (int index = 0; index < messages; ++index) {
        std::size_t sent = 0;
        while (sent < frame.size()) {
            const auto result = socket->send(frame.data() + sent, frame.size() - sent);
            ++calls;
            if (result.transferred <= 0) {
                ++tier.failed;
                break;
            }
            sent += static_cast<std::size_t>(result.transferred);
        }
        if (tier.failed != 0) {
            break;
        }
    }
    tier.elapsed_ns = NowNs() - start;
    tier.syscalls = calls;
    return tier;
}

/// 档 B：`bufio::Writer`（kHybrid 默认策略）+ `SocketBatchWriter`，靠缓冲区合并。
WriteTier BufioWrite(Socket::Ptr socket, const std::string& source, int messages) {
    WriteTier tier;
    std::string frame = source;
    SocketBatchWriter destination(socket, std::chrono::milliseconds(5000));
    Writer<SocketBatchWriter> writer(destination, 4096);
    const std::uint64_t start = NowNs();
    for (int index = 0; index < messages; ++index) {
        std::size_t offset = 0;
        while (offset < frame.size()) {
            const std::size_t accepted = writer.Write(
                AsBytes(frame.data() + offset, frame.size() - offset));
            if (accepted == 0) {
                ++tier.failed;
                break;
            }
            offset += accepted;
        }
        if (tier.failed != 0) {
            break;
        }
    }
    if (!writer.Flush().ok()) {
        ++tier.failed;
    }
    tier.elapsed_ns = NowNs() - start;
    tier.syscalls = destination.send_calls() + destination.writev_calls();
    return tier;
}

/// 档 C：`bufio::Writer` + `kOnExplicit` + 每 64 条 `FlushBatch()`（writev 合并段）。
WriteTier BufioBatchWrite(Socket::Ptr socket, const std::string& source, int messages) {
    WriteTier tier;
    std::string frame = source;
    SocketBatchWriter destination(socket, std::chrono::milliseconds(5000));
    Writer<SocketBatchWriter> writer(destination, 4096);
    BatchConfig config;
    config.policy = FlushPolicy::kOnExplicit;
    config.max_batch = 64;
    config.threshold = 4096;
    writer.SetBatchConfig(config);
    const std::uint64_t start = NowNs();
    int pending = 0;
    for (int index = 0; index < messages; ++index) {
        std::size_t offset = 0;
        while (offset < frame.size()) {
            const std::size_t accepted = writer.Write(
                AsBytes(frame.data() + offset, frame.size() - offset));
            if (accepted == 0) {
                ++tier.failed;
                break;
            }
            offset += accepted;
        }
        if (tier.failed != 0) {
            break;
        }
        if (++pending == 64) {
            if (!writer.FlushBatch().ok()) {
                ++tier.failed;
                break;
            }
            pending = 0;
        }
    }
    if (pending != 0 || writer.Buffered() != 0) {
        (void)writer.FlushBatch();
    }
    tier.elapsed_ns = NowNs() - start;
    tier.syscalls = destination.send_calls() + destination.writev_calls();
    return tier;
}

/// 对给定帧尺寸跑三档，输出三行结果（吞吐 + 系统调用数）。
void RunSize(const std::string& label, std::size_t frame_bytes, int messages) {
    std::string frame(frame_bytes, 'x');
    for (std::size_t index = 0; index < frame.size(); ++index) {
        frame[index] = static_cast<char>('a' + (index % 26));
    }
    struct Tier {
        const char* name;
        WriteTier (*run)(Socket::Ptr, const std::string&, int);
    };
    const Tier tiers[3] = {{"单写 send", SingleWrite},
                           {"bufio(4KB,hybrid)", BufioWrite},
                           {"bufio+batch(64,writev)", BufioBatchWrite}};
    for (const Tier& tier : tiers) {
        TcpPair pair;
        if (!pair.Open()) {
            std::fprintf(stderr, "[FAIL] write_path: 回环 socket 建立失败\n");
            ++g_failures;
            return;
        }
        // 预热 200 条，让接收线程与内核窗口进入稳态。
        (void)tier.run(pair.client, frame, 200);
        WriteTier best;
        best.elapsed_ns = UINT64_MAX;
        for (int round = 0; round < 2; ++round) {
            const WriteTier current = tier.run(pair.client, frame, messages);
            if (current.elapsed_ns < best.elapsed_ns) {
                best = current;
            }
        }
        pair.Close();
        BENCH_CHECK_EQ(best.failed, 0U);
        ResultRow row;
        row.protocol = "WRITE";
        row.scenario = label + " " + tier.name;
        row.note = "loopback TCP；系统调用数/消息数=" +
                   std::to_string(static_cast<double>(best.syscalls) / static_cast<double>(messages));
        row.ops = static_cast<std::uint64_t>(messages);
        row.ops_per_second =
            static_cast<double>(messages) * 1e9 / static_cast<double>(best.elapsed_ns);
        Record(std::move(row));
        std::printf("[WRITE] %-28s %-26s syscalls=%-8llu (%.3f/msg)\n", label.c_str(), tier.name,
                    static_cast<unsigned long long>(best.syscalls),
                    static_cast<double>(best.syscalls) / static_cast<double>(messages));
    }
}

void Run() {
    std::printf("\n--- 写路径三档对比（单写 / bufio / bufio+batch，回环 TCP）---\n");
    RunSize("HTTP 响应 120B", 120, 20000);
    RunSize("WS 帧 64B", 64, 20000);
    RunSize("MQTT PUBLISH 96B", 96, 20000);
    RunSize("CoAP 响应 64B", 64, 20000);
    RunSize("大消息 4096B", 4096, 4000);
}

}  // namespace write_path

// ===========================================================================
// 4. 入口：参数解析与调度
// ===========================================================================

struct Options {
    bool correctness{true};
    bool perf{true};
    std::vector<std::string> only;  ///< 空表示全部
};

bool Wanted(const Options& options, const std::string& protocol) {
    if (options.only.empty()) {
        return true;
    }
    for (const auto& item : options.only) {
        if (item == protocol) {
            return true;
        }
    }
    return false;
}

Options ParseArgs(int argc, char** argv) {
    Options options;
    bool saw_mode = false;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--correctness") {
            if (!saw_mode) {
                options.perf = false;
                saw_mode = true;
            }
        } else if (arg == "--perf") {
            if (!saw_mode) {
                options.correctness = false;
                saw_mode = true;
            }
        } else if (arg.rfind("--only=", 0) == 0) {
            std::string list = arg.substr(7);
            std::size_t start = 0;
            while (start <= list.size()) {
                const std::size_t comma = list.find(',', start);
                const std::string item =
                    list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!item.empty()) {
                    options.only.push_back(item);
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法: %s [--correctness|--perf] [--only=http,ws,...]\n", argv[0]);
            std::exit(0);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const Options options = ParseArgs(argc, argv);
    PrintEnvironment();

    const bool want_http = Wanted(options, "http");
    const bool want_ws = Wanted(options, "ws");
    const bool want_mqtt = Wanted(options, "mqtt");
    const bool want_coap = Wanted(options, "coap");
    const bool want_socks = Wanted(options, "socks5");
    const bool want_link = Wanted(options, "link");
    const bool want_omni = Wanted(options, "omni");
    const bool want_write = Wanted(options, "write");

    if (options.correctness) {
        if (want_http) {
            http_bench::CheckCorrectness();
        }
        if (want_ws) {
            ws_bench::CheckCorrectness();
        }
        if (want_mqtt) {
            mqtt_bench::CheckCorrectness();
        }
        if (want_coap) {
            coap_bench::CheckCorrectness();
        }
        if (want_socks) {
            socks_bench::CheckCorrectness();
        }
        if (want_link) {
            link_bench::CheckCorrectness();
        }
        if (want_omni) {
            omni_bench::CheckCorrectness();
        }
    }
    if (options.perf) {
        if (want_http) {
            http_bench::RunPerformance();
        }
        if (want_ws) {
            ws_bench::RunPerformance();
        }
        if (want_mqtt) {
            mqtt_bench::RunPerformance();
        }
        if (want_coap) {
            coap_bench::RunPerformance();
        }
        if (want_socks) {
            socks_bench::RunPerformance();
        }
        if (want_link) {
            link_bench::RunPerformance();
        }
        if (want_omni) {
            omni_bench::RunPerformance();
        }
        if (want_write) {
            write_path::Run();
        }
    }

    if (options.perf) {
        PrintResultTable();
    }
    std::printf("\n[SUMMARY] 场景 %d 个，断言 %d 条，失败 %d 条\n", g_scenarios, g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
