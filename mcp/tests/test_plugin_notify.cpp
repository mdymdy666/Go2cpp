/**
 * @file test_plugin_notify.cpp
 * @brief 插件主动通知的单测：注入、协议转发、拒绝非法帧、无客户端丢弃、卸载后不回调、并发。
 *
 * 用法: test_plugin_notify <插件目录>
 *
 * 依赖 `fake_plugin/notify_plugin.cpp` 编译出的 .so（CMake 会复制到
 * `<build>/tests/test_plugins/`）。覆盖的真实语义见 `NotificationHub` 文档。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mcp/server/registry.hpp"
#include "mcp/server/server.hpp"
#include "plugin/plugin_loader.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::mcp::json;
using go2cpp::mcp::plugin::NotificationHub;

/// 记录 sink 收到的通知（sink 可能被插件工作线程调用，所以内部加锁）。
struct Capture {
    std::mutex mutex;
    std::vector<std::string> plugins;
    std::vector<std::string> methods;
    std::vector<json> params;

    NotificationHub::Sink Sink() {
        return [this](const std::string& plugin, const std::string& method, const json& arguments) {
            std::lock_guard<std::mutex> lock(mutex);
            plugins.push_back(plugin);
            methods.push_back(method);
            params.push_back(arguments);
        };
    }

    std::size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return methods.size();
    }
};

/// 调工具（插件工具只认 name，arguments 用空对象即可）。
go2cpp::mcp::ToolResult Call(go2cpp::mcp::Registry& registry, const std::string& name) {
    go2cpp::mcp::ToolContext context;
    context.arguments = json::object();
    const auto result = registry.InvokeTool(name, context);
    if (!result) {
        return go2cpp::mcp::ToolResult::Text("missing tool: " + name, true);
    }
    return *result;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法: %s <插件目录>\n", argv[0]);
        return 2;
    }
    const std::string plugins_dir = argv[1];

    // ---- 1) 加载并注入：插件工具能调用宿主 sink ----
    go2cpp::mcp::Registry registry;
    auto loader = std::make_unique<go2cpp::mcp::plugin::PluginLoader>();
    Capture capture;
    loader->SetNotificationSink(capture.Sink());

    std::string error;
    CHECK(loader->LoadDirectory(plugins_dir, registry, error));
    CHECK_EQ(error, std::string());
    CHECK(loader->count() == 1);

    const auto hub = loader->notification_hub();
    CHECK(hub != nullptr);

    const auto first = Call(registry, "notify_test");
    CHECK(!first.is_error);
    CHECK(hub->forwarded() == 2);
    CHECK(hub->dropped() == 0);
    CHECK(hub->rejected() == 0);
    CHECK(capture.size() == 2);

    // ---- 2) 非法 JSON 与越权 method 被拒绝，不进入 sink ----
    const auto bad_json = Call(registry, "notify_bad_json");
    CHECK(!bad_json.is_error);
    const auto forbidden = Call(registry, "notify_forbidden");
    CHECK(!forbidden.is_error);
    CHECK(hub->rejected() == 2);
    CHECK(hub->forwarded() == 2);
    CHECK(capture.size() == 2);

    // ---- 3) 工作线程发通知：宿主串行化，不丢帧 ----
    const auto worker = Call(registry, "notify_worker");
    CHECK(!worker.is_error);
    CHECK(hub->forwarded() == 3);
    CHECK(capture.size() == 3);

    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        CHECK_EQ(capture.plugins.at(0), std::string("fake-notify"));
        CHECK_EQ(capture.methods.at(0), std::string("notifications/message"));
        CHECK_EQ(capture.params.at(0).value("level", std::string()), std::string("notice"));
        CHECK_EQ(capture.params.at(0).value("data", std::string()), std::string("plugin says hi"));
        CHECK_EQ(capture.methods.at(1), std::string("notifications/progress"));
        CHECK_EQ(capture.params.at(1).value("progressToken", std::string()), std::string("tok-1"));
        CHECK_EQ(capture.params.at(2).value("data", std::string()),
                 std::string("from worker thread"));
    }

    // ---- 4) 并发投递 8×500：Hub 的计数与 sink 调用次数必须一致 ----
    {
        std::atomic<int> delivered{0};
        NotificationHub concurrent([&delivered](const std::string& plugin,
                                                const std::string& method, const json&) {
            (void)plugin;
            if (method == "notifications/message") {
                delivered.fetch_add(1, std::memory_order_relaxed);
            }
        });
        constexpr int kThreads = 8;
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int index = 0; index < kThreads; ++index) {
            threads.emplace_back([&concurrent] {
                constexpr const char* kPayload =
                    R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":"x"}})";
                for (int i = 0; i < 500; ++i) {
                    concurrent.Deliver("fake-notify", kPayload);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        const std::size_t expected = static_cast<std::size_t>(kThreads) * 500;
        CHECK(concurrent.forwarded() == expected);
        CHECK(concurrent.rejected() == 0);
        CHECK(concurrent.dropped() == 0);
        CHECK(delivered.load(std::memory_order_relaxed) == static_cast<int>(expected));
    }

    // ---- 4.5) 轻量吞吐：单线程 10000 条（sink 为空，测解析+校验+计数开销） ----
    {
        NotificationHub bench([](const std::string& plugin, const std::string& method,
                                 const json& params) {
            (void)plugin;
            (void)method;
            (void)params;
        });
        constexpr const char* kPayload =
            R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":"bench"}})";
        constexpr int kIterations = 10000;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kIterations; ++i) {
            bench.Deliver("fake-notify", kPayload);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        const double microseconds = static_cast<double>(elapsed.count());
        const double denominator = microseconds > 0.0 ? microseconds : 1.0;
        std::printf("[plugin_notify] 单线程 10000 条: %.0f 条/s (%.2f us/条)\n",
                    kIterations * 1e6 / denominator, denominator / kIterations);
        CHECK(bench.forwarded() == static_cast<std::size_t>(kIterations));
    }

    // ---- 5) 没有 sink：丢弃并计数（不缓存） ----
    {
        NotificationHub detached;
        detached.Deliver("fake-notify",
                         R"({"jsonrpc":"2.0","method":"notifications/message","params":{"data":"x"}})");
        CHECK(detached.forwarded() == 0);
        CHECK(detached.dropped() == 1);
    }

    // ---- 6) 无客户端连接：Server 丢弃并计数，不崩溃 ----
    {
        go2cpp::mcp::ServerOptions options;
        go2cpp::mcp::Server server(options, registry);
        NotificationHub server_hub([&server](const std::string& plugin, const std::string& method,
                                             const json& params) {
            (void)plugin;
            server.SendNotification(method, params);
        });
        server_hub.Deliver(
            "fake-notify",
            R"({"jsonrpc":"2.0","method":"notifications/message","params":{"data":"no client"}})");
        CHECK(server_hub.forwarded() == 1);
        CHECK(server.notifications_dropped() == 1);
    }

    // ---- 6.5) 广播出口优先；传输未提供广播能力时退回定向出口 ----
    {
        go2cpp::mcp::ServerOptions options;
        go2cpp::mcp::Server server(options, registry);
        std::vector<std::string> directed;
        std::vector<std::string> broadcast;
        server.SetNotificationSink(
            [&directed](const std::string& payload) { directed.push_back(payload); });
        server.SetBroadcastSink(
            [&broadcast](const std::string& payload) { broadcast.push_back(payload); });

        server.SendNotification("notifications/message", json{{"data", "directed"}});
        server.SendBroadcastNotification("notifications/message", json{{"data", "broadcast"}});
        CHECK(directed.size() == 1);
        CHECK(broadcast.size() == 1);
        CHECK(broadcast.at(0).find("\"data\":\"broadcast\"") != std::string::npos);
    }
    {
        go2cpp::mcp::ServerOptions options;
        go2cpp::mcp::Server server(options, registry);
        std::vector<std::string> directed;
        server.SetNotificationSink(
            [&directed](const std::string& payload) { directed.push_back(payload); });
        server.SendBroadcastNotification("notifications/message", json{{"data", "fallback"}});
        CHECK(directed.size() == 1);  // 没有广播出口 → 退回定向
    }

    // ---- 7) 卸载：Shutdown 里的通知只计数；卸载后不再转发 ----
    const std::size_t forwarded_before_unload = hub->forwarded();
    const std::size_t captures_before_unload = capture.size();

    loader.reset();  // Detach → Shutdown → 注销投递表 → DestroyPlugin → dlclose

    CHECK(hub->forwarded() == forwarded_before_unload);
    CHECK(hub->dropped() >= 1);  // Shutdown 里发的那条被丢弃
    CHECK(capture.size() == captures_before_unload);

    // 卸载完成后（投递表项已删、sink 已 Detach）再次投递：不转发、不崩溃。
    hub->Deliver("fake-notify",
                 R"({"jsonrpc":"2.0","method":"notifications/message","params":{"data":"late"}})");
    CHECK(hub->forwarded() == forwarded_before_unload);
    CHECK(hub->dropped() >= 2);

    return go2cpp::mcp::test::Summary("plugin_notify");
}
