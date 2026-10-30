/**
 * @file test_socket_tools.cpp
 * @brief `socket_*` 工具端到端：真 TCP/UDP/Unix socket → list/stats/events/close。
 *
 * 覆盖：
 * 1. 四个工具注册、schema 与协议层参数校验（fd/n/reason 边界）；
 * 2. 真 TCP 环回连接：list 显示 fd/协议/地址/状态，未登记项只读；
 * 3. 真流量：TCP_INFO 接收/发送字节、错误/超时埋点进入 stats；
 * 4. 事件顺序、截断与默认 n，TCP 采样事件；
 * 5. socket_close：fd 真正关闭、审计记录、重复关闭拒绝、关闭事件可查；
 * 6. UDP 与 Unix socketpair 的协议识别、无 TCP_INFO 时的诚实降级。
 */

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "socket_test_util.hpp"
#include "test_util.hpp"

#include "mcp/tools/socket_registry.hpp"
#include "mcp/tools/socket_tools.hpp"
#include "mcp/validation/validators.hpp"

namespace {

using go2cpp::mcp::Registry;
using go2cpp::mcp::json;
using go2cpp::mcp::test::CallTool;
using go2cpp::mcp::test::Fd;
using go2cpp::mcp::test::HasTool;
using go2cpp::mcp::test::MakeTcpPair;
using go2cpp::mcp::test::MakeUdpSocket;
using go2cpp::mcp::test::MakeUnixPair;
using go2cpp::mcp::test::ParseJson;
using go2cpp::mcp::test::TcpPair;
using go2cpp::mcp::tools::MemorySocketAuditSink;
using go2cpp::mcp::tools::SocketEventKind;
using go2cpp::mcp::tools::SocketProtocol;
using go2cpp::mcp::tools::SocketRegistry;
using go2cpp::mcp::tools::SocketState;
using go2cpp::mcp::tools::SocketStats;

/// 每个用例开始前清空单例，避免用例间串数据。
void ResetAll() { SocketRegistry::Instance().Reset(); }

/// 注册四个工具并返回内存审计汇。
std::shared_ptr<MemorySocketAuditSink> SetupTools(Registry* registry) {
    auto audit = std::make_shared<MemorySocketAuditSink>();
    go2cpp::mcp::tools::RegisterSocketTools(*registry, audit);
    return audit;
}

/// 工具注册、schema、参数校验。
void TestToolsAndSchema() {
    ResetAll();
    Registry registry;
    (void)SetupTools(&registry);
    CHECK(HasTool(registry, "socket_list"));
    CHECK(HasTool(registry, "socket_stats"));
    CHECK(HasTool(registry, "socket_events"));
    CHECK(HasTool(registry, "socket_close"));
    CHECK(registry.tool_count() == 4);

    const auto list_schema = registry.ToolSchema("socket_list");
    const auto stats_schema = registry.ToolSchema("socket_stats");
    const auto events_schema = registry.ToolSchema("socket_events");
    const auto close_schema = registry.ToolSchema("socket_close");
    CHECK(list_schema.has_value());
    CHECK(stats_schema.has_value());
    CHECK(events_schema.has_value());
    CHECK(close_schema.has_value());
    if (!list_schema || !stats_schema || !events_schema || !close_schema) {
        return;
    }

    using go2cpp::mcp::validation::ValidateToolArguments;
    CHECK(ValidateToolArguments(*list_schema, json::object()).ok);
    CHECK(ValidateToolArguments(*list_schema,
                                {{"include_unobserved", false}})
              .ok);
    CHECK(!ValidateToolArguments(*list_schema, {{"include_unobserved", "yes"}}).ok);
    CHECK(!ValidateToolArguments(*list_schema, {{"unknown", 1}}).ok);

    CHECK(ValidateToolArguments(*stats_schema, {{"fd", 3}}).ok);
    CHECK(!ValidateToolArguments(*stats_schema, json::object()).ok);
    CHECK(!ValidateToolArguments(*stats_schema, {{"fd", -1}}).ok);
    CHECK(!ValidateToolArguments(*stats_schema, {{"fd", "3"}}).ok);
    CHECK(!ValidateToolArguments(*stats_schema, {{"fd", 1.5}}).ok);
    CHECK(!ValidateToolArguments(*stats_schema, {{"fd", 3}, {"x", 1}}).ok);

    CHECK(ValidateToolArguments(*events_schema, {{"fd", 3}}).ok);
    CHECK(ValidateToolArguments(*events_schema, {{"fd", 3}, {"n", 1}}).ok);
    CHECK(ValidateToolArguments(*events_schema, {{"fd", 3}, {"n", 256}}).ok);
    CHECK(!ValidateToolArguments(*events_schema, {{"fd", 3}, {"n", 0}}).ok);
    CHECK(!ValidateToolArguments(*events_schema, {{"fd", 3}, {"n", 257}}).ok);
    CHECK(!ValidateToolArguments(*events_schema, {{"n", 3}}).ok);

    CHECK(ValidateToolArguments(*close_schema, {{"fd", 3}}).ok);
    CHECK(ValidateToolArguments(*close_schema,
                                {{"fd", 3}, {"reason", "test"}})
              .ok);
    CHECK(!ValidateToolArguments(
               *close_schema, {{"fd", 3}, {"reason", std::string(201, 'a')}})
               .ok);
    CHECK(!ValidateToolArguments(*close_schema, {{"reason", "x"}}).ok);
}

/// list：真实 TCP 连接、登记/未登记、工具输出。
void TestListRealSockets() {
    ResetAll();
    TcpPair pair = MakeTcpPair();
    CHECK(pair.valid());
    if (!pair.valid()) {
        return;
    }
    Registry registry;
    (void)SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;
    CHECK(socket_registry.Register(pair.server.get(), "server", &error));
    CHECK(error.empty());

    const auto snapshots = socket_registry.List(true, &error);
    CHECK(error.empty());
    bool server_found = false;
    bool client_found = false;
    for (const auto& snapshot : snapshots) {
        if (snapshot.fd == pair.server.get()) {
            server_found = true;
            CHECK(snapshot.protocol == SocketProtocol::kTcp);
            CHECK(snapshot.state == SocketState::kConnected);
            CHECK(snapshot.registered);
            CHECK(snapshot.closable);
            CHECK(!snapshot.local_address.empty());
            CHECK(!snapshot.remote_address.empty());
            CHECK_EQ(snapshot.label, "server");
        }
        if (snapshot.fd == pair.client.get()) {
            client_found = true;
            CHECK(!snapshot.registered);
            CHECK(!snapshot.closable);
        }
    }
    CHECK(server_found);
    CHECK(client_found);

    const auto [is_error, text] = CallTool(registry, "socket_list", json::object());
    CHECK(!is_error);
    const json out = ParseJson(text);
    CHECK(out.is_object());
    CHECK(out["count"].is_number_unsigned());
    CHECK(out["count"].get<std::size_t>() >= 2);
    CHECK(out["sockets"].is_array());
    CHECK(out["sockets"].size() == out["count"].get<std::size_t>());

    const auto [filtered_error, filtered_text] =
        CallTool(registry, "socket_list", {{"include_unobserved", false}});
    CHECK(!filtered_error);
    const json filtered = ParseJson(filtered_text);
    CHECK(filtered["count"] == 1);
    CHECK(filtered["sockets"][0]["fd"].get<int>() == pair.server.get());
}

/// stats：真实 TCP 流量、TCP_INFO、应用层错误/超时。
void TestStatsRealTraffic() {
    ResetAll();
    TcpPair pair = MakeTcpPair();
    CHECK(pair.valid());
    if (!pair.valid()) {
        return;
    }
    Registry registry;
    (void)SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;
    CHECK(socket_registry.Register(pair.server.get(), "stats", &error));

    const std::string payload(4096, 'x');
    CHECK(::send(pair.client.get(), payload.data(), payload.size(), 0) ==
          static_cast<ssize_t>(payload.size()));
    std::string received(payload.size(), '\0');
    CHECK(::recv(pair.server.get(), received.data(), received.size(), MSG_WAITALL) ==
          static_cast<ssize_t>(payload.size()));

    SocketStats stats;
    bool ready = false;
    for (int attempt = 0; attempt < 100 && !ready; ++attempt) {
        const auto value = socket_registry.Stats(pair.server.get(), &error);
        if (value.has_value() && value->tcp.available &&
            value->tcp.received_bytes >= payload.size()) {
            stats = *value;
            ready = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    CHECK(ready);
    CHECK(stats.tcp.available);
    CHECK(stats.tcp.received_bytes >= payload.size());
    CHECK(stats.tcp.received_bytes >= payload.size());
    CHECK_EQ(stats.tcp.state, "established");

    // 反方向：服务端发送，客户端确认，内核 sent 字节应增长。
    CHECK(::send(pair.server.get(), payload.data(), 1024, 0) == 1024);
    CHECK(::recv(pair.client.get(), received.data(), 1024, MSG_WAITALL) == 1024);
    bool sent_ready = false;
    for (int attempt = 0; attempt < 100 && !sent_ready; ++attempt) {
        const auto value = socket_registry.Stats(pair.server.get(), &error);
        if (value.has_value() && value->tcp.available &&
            value->tcp.sent_bytes >= 1024) {
            stats = *value;
            sent_ready = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    CHECK(sent_ready);
    CHECK(stats.tcp.sent_bytes >= 1024);
    CHECK(stats.tcp.sent_bytes >= 1024);

    const auto [is_error, text] =
        CallTool(registry, "socket_stats", {{"fd", pair.server.get()}});
    CHECK(!is_error);
    const json out = ParseJson(text);
    CHECK(out["fd"].get<int>() == pair.server.get());
    CHECK(out["tcp"]["available"] == true);
    CHECK(out["tcp"]["received_bytes"].get<std::uint64_t>() >= payload.size());
    CHECK(out["recv_bytes"].get<std::uint64_t>() >= payload.size());
    CHECK(out["registered"] == true);
}

/// events：应用埋点、顺序、截断、工具默认 n。
void TestEvents() {
    ResetAll();
    TcpPair pair = MakeTcpPair();
    CHECK(pair.valid());
    if (!pair.valid()) {
        return;
    }
    Registry registry;
    (void)SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;
    CHECK(socket_registry.Register(pair.server.get(), "events", &error));

    socket_registry.RecordRead(pair.server.get(), 100);
    socket_registry.RecordWrite(pair.server.get(), 200);
    socket_registry.RecordError(pair.server.get(), ECONNRESET);
    socket_registry.RecordTimeout(pair.server.get());

    bool registered = false;
    bool found = false;
    const auto tail = socket_registry.Events(pair.server.get(), 2, &registered,
                                             &found, &error);
    CHECK(error.empty());
    CHECK(found);
    CHECK(registered);
    CHECK(tail.size() == 2);
    if (tail.size() == 2) {
        CHECK(tail[0].kind == SocketEventKind::kError);
        CHECK(tail[1].kind == SocketEventKind::kTimeout);
    }

    const auto all = socket_registry.Events(pair.server.get(), 256, &registered,
                                            &found, &error);
    bool has_registered = false;
    bool has_connected = false;
    bool has_read = false;
    bool has_write = false;
    bool has_error = false;
    bool has_timeout = false;
    for (const auto& event : all) {
        switch (event.kind) {
            case SocketEventKind::kRegistered:
                has_registered = true;
                break;
            case SocketEventKind::kConnected:
                has_connected = true;
                break;
            case SocketEventKind::kRead:
                has_read = true;
                break;
            case SocketEventKind::kWrite:
                has_write = true;
                break;
            case SocketEventKind::kError:
                has_error = true;
                break;
            case SocketEventKind::kTimeout:
                has_timeout = true;
                break;
            case SocketEventKind::kCloseRequested:
            case SocketEventKind::kClosed:
                break;
        }
    }
    CHECK(has_registered);
    CHECK(has_connected);
    CHECK(has_read);
    CHECK(has_write);
    CHECK(has_error);
    CHECK(has_timeout);

    const auto [is_error, text] = CallTool(
        registry, "socket_events",
        {{"fd", pair.server.get()}, {"n", 3}});
    CHECK(!is_error);
    const json out = ParseJson(text);
    CHECK(out["count"] == 3);
    CHECK(out["events"].size() == 3);
    CHECK(out["events"][2]["kind"] == "timeout");

    const auto [default_error, default_text] =
        CallTool(registry, "socket_events", {{"fd", pair.server.get()}});
    CHECK(!default_error);
    const json default_out = ParseJson(default_text);
    CHECK(default_out["count"].get<std::size_t>() >= 6);

    const auto stats = socket_registry.Stats(pair.server.get(), &error);
    CHECK(stats.has_value());
    if (stats.has_value()) {
        CHECK(stats->app.errors == 1);
        CHECK(stats->app.timeouts == 1);
        CHECK(stats->app.recv_bytes == 100);
        CHECK(stats->app.sent_bytes == 200);
    }
}

/// close：真正关闭、审计、重复关闭、关闭事件。
void TestCloseAudit() {
    ResetAll();
    TcpPair pair = MakeTcpPair();
    CHECK(pair.valid());
    if (!pair.valid()) {
        return;
    }
    Registry registry;
    auto audit = SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;
    CHECK(socket_registry.Register(pair.server.get(), "close", &error));

    const int server_fd = pair.server.get();
    const auto [is_error, text] = CallTool(
        registry, "socket_close", {{"fd", server_fd}, {"reason", "e2e"}});
    CHECK(!is_error);
    const json out = ParseJson(text);
    CHECK(out["closed"] == true);
    CHECK(out["audit"]["id"].is_string());
    CHECK(out["audit"]["id"].get<std::string>().size() > 0);
    CHECK(out["audit"]["sink"] == "memory");
    CHECK(::fcntl(server_fd, F_GETFD) == -1);
    pair.server.Release();

    CHECK(audit->size() == 1);
    const auto records = audit->Records();
    CHECK(records[0]["action"] == "socket_close");
    CHECK(records[0]["result"] == "ok");
    CHECK(records[0]["fd"].get<int>() == server_fd);
    CHECK(records[0]["reason"] == "e2e");
    CHECK(records[0]["pid"].get<int>() > 0);
    CHECK(records[0]["ts"].is_string());

    bool still_listed = false;
    for (const auto& snapshot : socket_registry.List(true, &error)) {
        if (snapshot.fd == server_fd) {
            still_listed = true;
        }
    }
    CHECK(!still_listed);

    bool registered = true;
    bool found = false;
    const auto events = socket_registry.Events(server_fd, 256, &registered, &found,
                                               &error);
    CHECK(error.empty());
    CHECK(found);
    CHECK(!registered);
    bool has_closed = false;
    for (const auto& event : events) {
        if (event.kind == SocketEventKind::kClosed) {
            has_closed = true;
        }
    }
    CHECK(has_closed);

    const auto [second_error, second_text] =
        CallTool(registry, "socket_close", {{"fd", server_fd}});
    CHECK(second_error);
    const json second = ParseJson(second_text);
    CHECK(second["closed"] == false);
    CHECK(second["error"].is_string());
    CHECK(audit->size() == 2);
    CHECK(audit->Records()[1]["result"] == "denied");
}

/// UDP / Unix：协议识别与无 TCP_INFO 的降级。
void TestUdpUnix() {
    ResetAll();
    Registry registry;
    (void)SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;

    Fd udp = MakeUdpSocket();
    CHECK(udp.valid());
    if (udp.valid()) {
        CHECK(socket_registry.Register(udp.get(), "udp", &error));
        const auto snapshots = socket_registry.List(true, &error);
        bool found = false;
        for (const auto& snapshot : snapshots) {
            if (snapshot.fd == udp.get()) {
                found = true;
                CHECK(snapshot.protocol == SocketProtocol::kUdp);
                CHECK(snapshot.state == SocketState::kUnconnected);
                CHECK(snapshot.registered);
            }
        }
        CHECK(found);
        const auto stats = socket_registry.Stats(udp.get(), &error);
        CHECK(stats.has_value());
        if (stats.has_value()) {
            CHECK(!stats->tcp.available);
        }
        const auto [is_error, text] =
            CallTool(registry, "socket_close", {{"fd", udp.get()}});
        CHECK(!is_error);
        CHECK(::fcntl(udp.get(), F_GETFD) == -1);
        udp.Release();
    }

    auto pair = MakeUnixPair();
    CHECK(pair.first.valid());
    CHECK(pair.second.valid());
    if (!pair.first.valid() || !pair.second.valid()) {
        return;
    }
    CHECK(socket_registry.Register(pair.first.get(), "unix", &error));
    const auto snapshots = socket_registry.List(true, &error);
    bool found = false;
    for (const auto& snapshot : snapshots) {
        if (snapshot.fd == pair.first.get()) {
            found = true;
            CHECK(snapshot.protocol == SocketProtocol::kUnixStream);
            CHECK(snapshot.state == SocketState::kConnected);
        }
    }
    CHECK(found);
    const auto stats = socket_registry.Stats(pair.first.get(), &error);
    CHECK(stats.has_value());
    if (stats.has_value()) {
        CHECK(!stats->tcp.available);
    }
    // 关闭前双向可用。
    const char byte = 'x';
    char received = '\0';
    CHECK(::send(pair.second.get(), &byte, 1, 0) == 1);
    CHECK(::recv(pair.first.get(), &received, 1, 0) == 1);
    CHECK(received == 'x');

    const auto [is_error, text] =
        CallTool(registry, "socket_close", {{"fd", pair.first.get()}});
    CHECK(!is_error);
    pair.first.Release();

    // 对端仍存活：关闭一端后 recv 返回 0（EOF），而不是向已关端 send。
    CHECK(::recv(pair.second.get(), &received, 1, 0) == 0);
}

/// 随机参数 fuzz：类型错误/越界/随机组合下不崩溃，成功结果必为合法 JSON。
void TestRandomArguments() {
    ResetAll();
    TcpPair pair = MakeTcpPair();
    CHECK(pair.valid());
    if (!pair.valid()) {
        return;
    }
    Registry registry;
    (void)SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;
    CHECK(socket_registry.Register(pair.server.get(), "fuzz", &error));

    const char* const tools[4] = {"socket_list", "socket_stats", "socket_events",
                                  "socket_close"};
    // 故意不放真实登记的 fd：fuzz 若真把它关掉，用例析构时的 RAII 会二次 close
    // 一个已被内核复用的 fd，从而误伤同进程其它测试。
    const long long fd_pool[] = {-2, -1, 0, 1, 2, 512, 999999,
                                 std::numeric_limits<int>::max()};
    const long long n_pool[] = {-1, 0, 1, 2, 20, 255, 256, 257, 10000};
    const std::string reason_pool[] = {"", "fuzz", std::string(400, 'x'),
                                       "关闭原因：随机 fuzz 测试"};

    std::mt19937 engine(20261030U);  // 固定种子：失败可复现
    std::uniform_int_distribution<std::size_t> fd_pick(0, 7);
    std::uniform_int_distribution<std::size_t> n_pick(0, 8);
    std::uniform_int_distribution<std::size_t> reason_pick(0, 3);
    std::uniform_int_distribution<int> shape(0, 9);

    int accepted = 0;
    int rejected = 0;
    for (int iteration = 0; iteration < 1000; ++iteration) {
        const int variant = shape(engine);
        const std::string tool = tools[static_cast<std::size_t>(variant) % 4U];
        json arguments = json::object();
        switch (variant) {
            case 0:
                arguments["fd"] = fd_pool[fd_pick(engine)];
                break;
            case 1:
                arguments["fd"] = n_pool[n_pick(engine)];
                break;
            case 2:
                arguments["fd"] = std::string("3");
                break;
            case 3:
                arguments["fd"] = 1.5;
                break;
            case 4:
                arguments["fd"] = true;
                break;
            case 5:
                arguments["fd"] = json::array({1});
                break;
            case 6:
                arguments["n"] = n_pool[n_pick(engine)];
                break;
            case 7:
                arguments["fd"] = fd_pool[fd_pick(engine)];
                arguments["n"] = n_pool[n_pick(engine)];
                break;
            case 8:
                arguments["fd"] = fd_pool[fd_pick(engine)];
                arguments["reason"] = reason_pool[reason_pick(engine)];
                break;
            default:
                arguments["include_unobserved"] = (iteration % 2) == 0;
                break;
        }
        const auto [is_error, text] = CallTool(registry, tool, arguments);
        CHECK(!text.empty());
        if (is_error) {
            ++rejected;
            continue;
        }
        ++accepted;
        CHECK_MSG(ParseJson(text).is_object(),
                  std::string("成功调用应返回 JSON 对象：") + tool);
    }
    std::printf("[socket_tools] 随机 fuzz 1000 组：接受 %d，拒绝 %d\n", accepted,
                rejected);
    // 两侧都必须出现，否则说明生成器退化成了单一路径。
    CHECK(accepted > 0);
    CHECK(rejected > 0);
    CHECK(::fcntl(pair.server.get(), F_GETFD) != -1);
}

/// 压测：真实连接上的混合查询吞吐与延迟分位（只做宽松下限断言，避免 CI 抖动）。
void TestThroughput() {
    ResetAll();
    Registry registry;
    (void)SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;

    std::vector<TcpPair> pairs;
    pairs.reserve(4);
    for (int index = 0; index < 4; ++index) {
        TcpPair pair = MakeTcpPair();
        CHECK(pair.valid());
        if (!pair.valid()) {
            return;
        }
        CHECK(socket_registry.Register(pair.server.get(), "bench", &error));
        pairs.push_back(std::move(pair));
    }

    constexpr int kIterations = 2000;
    std::vector<double> latency_us;
    latency_us.reserve(kIterations);
    int failures = 0;
    const auto benchmark_start = std::chrono::steady_clock::now();
    for (int index = 0; index < kIterations; ++index) {
        const int fd =
            pairs[static_cast<std::size_t>(index) % pairs.size()].server.get();
        const auto start = std::chrono::steady_clock::now();
        std::pair<bool, std::string> outcome;
        switch (index % 3) {
            case 0:
                outcome = CallTool(registry, "socket_list", json::object());
                break;
            case 1:
                outcome = CallTool(registry, "socket_stats", {{"fd", fd}});
                break;
            default:
                outcome = CallTool(registry, "socket_events",
                                   {{"fd", fd}, {"n", 20}});
                break;
        }
        latency_us.push_back(std::chrono::duration<double, std::micro>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
        if (outcome.first || outcome.second.empty()) {
            ++failures;
        }
    }
    const double elapsed_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      benchmark_start)
            .count();

    std::sort(latency_us.begin(), latency_us.end());
    const auto percentile = [&latency_us](double ratio) {
        const std::size_t index = static_cast<std::size_t>(
            ratio * static_cast<double>(latency_us.size() - 1));
        return latency_us[index];
    };
    std::printf(
        "[socket_tools] 压测 %d 次混合调用（4 个真实 TCP 连接）：%.0f ops/s，"
        "P50=%.1fus P95=%.1fus P99=%.1fus，失败 %d\n",
        kIterations, static_cast<double>(kIterations) / elapsed_s,
        percentile(0.50), percentile(0.95), percentile(0.99), failures);
    CHECK(failures == 0);
    CHECK(elapsed_s > 0.0);
    CHECK(static_cast<double>(kIterations) / elapsed_s > 100.0);
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    TestToolsAndSchema();
    TestListRealSockets();
    TestStatsRealTraffic();
    TestEvents();
    TestCloseAudit();
    TestUdpUnix();
    TestRandomArguments();
    TestThroughput();
    return go2cpp::mcp::test::Summary("socket_tools");
}
