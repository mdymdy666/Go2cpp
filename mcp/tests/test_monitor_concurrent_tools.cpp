/**
 * @file test_monitor_concurrent_tools.cpp
 * @brief `os_monitor_*` 工具并发压测：100 并发调用 + 响应隔离 + 混合工具。
 *
 * 覆盖：
 * 1. 100 个线程同时调用 `os_monitor_latest`（Mock 客户端），每个线程带唯一
 *    `manager_url` / `limit` / `timeout_ms`，断言返回 JSON 三个字段与请求
 *    一一对应（无跨线程串扰）；
 * 2. 100 并发混合调用四个工具（各 25 次），断言每个工具只命中自己的数据源、
 *    计数精确、结果可解析；
 * 3. 并发下注入失败（每 3 个请求失败一次）→ 失败请求得到 isError 且错误文本
 *    对得上，成功请求不受影响；
 * 4. 并发调用与注册/注销写线程同时进行，工具表最终状态正确；
 * 5. 打印 100/200 并发的总耗时、吞吐与 P50/P95 延迟，供报告引用。
 *
 * 客户端替身只使用原子计数，不共享可变状态，因此本测试可直接在 TSan 下运行。
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_util.hpp"

#include "mcp/server/registry.hpp"
#include "mcp/tools/monitor_client.hpp"
#include "mcp/tools/monitor_tools.hpp"

namespace {

using go2cpp::mcp::json;
using go2cpp::mcp::Registry;
using go2cpp::mcp::ToolContext;
using go2cpp::mcp::tools::MonitorClient;
using go2cpp::mcp::tools::MonitorErrorKind;
using go2cpp::mcp::tools::MonitorQuery;
using go2cpp::mcp::tools::RegisterMonitorTools;
using go2cpp::mcp::tools::Result;

/// 线程安全的可编程客户端：每个请求把查询参数原样回显，便于检测串扰。
class ThreadSafeFakeClient final : public MonitorClient {
public:
    enum class Endpoint : int { kLatest = 0, kStats = 1, kKernel = 2, kHealth = 3 };

    ThreadSafeFakeClient() {
        for (auto& counter : calls_) {
            counter.store(0);
        }
    }

    Result<json> Latest(const MonitorQuery& query) override {
        return Dispatch(Endpoint::kLatest, query);
    }
    Result<json> Stats(const MonitorQuery& query) override {
        return Dispatch(Endpoint::kStats, query);
    }
    Result<json> Kernel(const MonitorQuery& query) override {
        return Dispatch(Endpoint::kKernel, query);
    }
    Result<json> Health(const MonitorQuery& query) override {
        return Dispatch(Endpoint::kHealth, query);
    }

    std::string BaseUrl() const override { return "fake://monitor"; }
    MonitorQuery DefaultQuery() const override {
        MonitorQuery query;
        query.limit = 10;
        query.timeout_ms = 3000;
        return query;
    }

    std::size_t CallCount(Endpoint endpoint) const {
        return calls_[static_cast<int>(endpoint)].load();
    }
    std::size_t TotalCalls() const {
        std::size_t total = 0;
        for (const auto& counter : calls_) {
            total += counter.load();
        }
        return total;
    }

    /// 每 N 个请求注入一次失败；0 表示不注入。
    void FailEvery(std::size_t n) { fail_every_.store(n); }
    /// 每个请求的人为延迟（微秒），用于拉长并发窗口。
    void SetDelayUs(int delay_us) { delay_us_.store(delay_us); }

private:
    Result<json> Dispatch(Endpoint endpoint, const MonitorQuery& query) {
        const std::size_t fail_every = fail_every_.load();
        const std::size_t index =
            ++sequence_ % (fail_every == 0 ? 1 : fail_every);
        const std::size_t invocation =
            calls_[static_cast<int>(endpoint)].fetch_add(1) + 1;
        const int delay_us = delay_us_.load();
        if (delay_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
        }
        if (fail_every != 0 && index == 0) {
            // 失败文本带调用序号，客户端必须原样透传。
            return Result<json>::Fail(
                MonitorErrorKind::kConnect,
                "fake: injected failure " + std::to_string(invocation), 0);
        }
        json item = json::object();
        item["tool"] = static_cast<int>(endpoint);
        item["url"] = query.base_url;
        item["limit"] = query.limit;
        item["timeout_ms"] = query.timeout_ms;
        item["invocation"] = invocation;

        switch (endpoint) {
            case Endpoint::kLatest:
                // 工具层要求数组；数组元素原样进入 snapshots。
                return Result<json>::Ok(json::array({item}));
            case Endpoint::kStats:
                item["host"] = "fake-host";
                item["cpu_percent"] = 10.0;
                item["mem_used_percent"] = 20.0;
                item["score"] = 90.0;
                return Result<json>::Ok(json::array({item}));
            case Endpoint::kKernel:
                item["host"] = "fake-host";
                item["syscall"] = json::object();
                item["sched"] = json::object();
                item["futex"] = json::object();
                item["tcp"] = json::object();
                item["fileio"] = json::object();
                return Result<json>::Ok(json::array({item}));
            case Endpoint::kHealth:
                item["status"] = "ok";
                return Result<json>::Ok(item);
        }
        return Result<json>::Fail(MonitorErrorKind::kResponseFormat,
                                  "fake: unknown endpoint", 0);
    }

    std::atomic<std::size_t> calls_[4];
    std::atomic<std::size_t> sequence_{0};
    std::atomic<std::size_t> fail_every_{0};
    std::atomic<int> delay_us_{0};
};

struct Invocation {
    bool is_error{true};
    std::string text;
    long long elapsed_us{0};
};

Invocation CallTool(const Registry& registry, const std::string& name,
                    const json& arguments) {
    ToolContext context;
    context.arguments = arguments;
    Invocation invocation;
    const auto started = std::chrono::steady_clock::now();
    const auto result = registry.InvokeTool(name, context);
    invocation.elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count();
    if (!result) {
        invocation.is_error = true;
        invocation.text = "__missing__";
        return invocation;
    }
    invocation.is_error = result->is_error;
    invocation.text = result->content.empty() ? "" : result->content[0].text;
    return invocation;
}

json ParseResult(const std::string& text) {
    try {
        return json::parse(text);
    } catch (const std::exception&) {
        return json(nullptr);
    }
}

void PrintPercentiles(const char* label, std::vector<long long> values) {
    if (values.empty()) {
        return;
    }
    std::sort(values.begin(), values.end());
    const auto at = [&values](double percentile) {
        std::size_t index = static_cast<std::size_t>(
            percentile * static_cast<double>(values.size() - 1));
        return values[index];
    };
    std::printf("[concurrent] %s n=%zu P50=%lldus P95=%lldus P99=%lldus max=%lldus\n",
                label, values.size(), at(0.50), at(0.95), at(0.99),
                values.back());
}

/// 1) 100 并发 single-tool：响应必须逐字段回显自己的参数。
void TestHundredLatestCalls() {
    auto client = std::make_shared<ThreadSafeFakeClient>();
    client->SetDelayUs(200);  // 200µs/请求，确保 100 个线程真正重叠
    Registry registry;
    RegisterMonitorTools(registry, client);

    constexpr int kThreads = 100;
    std::vector<Invocation> results(kThreads);
    std::vector<std::thread> threads;
    std::atomic<bool> start{false};
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const std::string url =
                "http://thread-" + std::to_string(index) + ".example";
            const int limit = 1 + (index % 256);
            const int timeout_ms = 100 + index;
            results[static_cast<std::size_t>(index)] = CallTool(
                registry, "os_monitor_latest",
                {{"manager_url", url}, {"limit", limit},
                 {"timeout_ms", timeout_ms}});
        });
    }
    const auto started = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();

    std::vector<long long> latencies;
    latencies.reserve(kThreads);
    int failures = 0;
    for (int index = 0; index < kThreads; ++index) {
        const Invocation& invocation = results[static_cast<std::size_t>(index)];
        latencies.push_back(invocation.elapsed_us);
        if (invocation.is_error) {
            ++failures;
            CHECK_MSG(false, "线程 " + std::to_string(index) +
                                 " 调用失败: " + invocation.text);
            continue;
        }
        const json payload = ParseResult(invocation.text);
        if (!payload.is_object() || !payload["snapshots"].is_array() ||
            payload["snapshots"].empty()) {
            ++failures;
            CHECK_MSG(false, "线程 " + std::to_string(index) +
                                 " 返回结构异常: " + invocation.text);
            continue;
        }
        const json& item = payload["snapshots"][0];
        const std::string expected_url =
            "http://thread-" + std::to_string(index) + ".example";
        CHECK_MSG(item.value("url", std::string{}) == expected_url,
                  "线程 " + std::to_string(index) + " 的响应串扰：期望 " +
                      expected_url + "，实际 " +
                      item.value("url", std::string{}));
        CHECK_MSG(item.value("limit", -1) == 1 + (index % 256),
                  "线程 " + std::to_string(index) + " limit 串扰");
        CHECK_MSG(item.value("timeout_ms", -1) == 100 + index,
                  "线程 " + std::to_string(index) + " timeout_ms 串扰");
    }
    CHECK_MSG(failures == 0,
              "100 并发应全部成功，失败 " + std::to_string(failures));
    CHECK_MSG(client->CallCount(ThreadSafeFakeClient::Endpoint::kLatest) ==
                  static_cast<std::size_t>(kThreads),
              "Latest 调用计数应为 100，实际 " +
                  std::to_string(client->CallCount(
                      ThreadSafeFakeClient::Endpoint::kLatest)));
    CHECK_MSG(client->TotalCalls() == static_cast<std::size_t>(kThreads),
              "其它端点不应被调用，总调用数 " +
                  std::to_string(client->TotalCalls()));
    PrintPercentiles("latest×100", latencies);
    std::printf("[concurrent] 100 并发 total=%lldus throughput=%.0f req/s\n",
                static_cast<long long>(elapsed_us),
                elapsed_us > 0 ? kThreads * 1e6 / elapsed_us : 0.0);
}

/// 2) 100 并发混合四工具：各 25 次，端点计数精确。
void TestMixedTools() {
    auto client = std::make_shared<ThreadSafeFakeClient>();
    Registry registry;
    RegisterMonitorTools(registry, client);
    const char* names[4] = {"os_monitor_latest", "os_monitor_stats",
                            "os_monitor_kernel", "os_monitor_health"};
    constexpr int kPerTool = 25;
    constexpr int kThreads = 4 * kPerTool;

    std::vector<Invocation> results(kThreads);
    std::vector<std::thread> threads;
    std::atomic<bool> start{false};
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const int tool = index % 4;
            results[static_cast<std::size_t>(index)] =
                CallTool(registry, names[tool],
                         {{"manager_url",
                           "http://mixed-" + std::to_string(index) + ".example"}});
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    int failures = 0;
    for (int index = 0; index < kThreads; ++index) {
        const Invocation& invocation = results[static_cast<std::size_t>(index)];
        const int tool = index % 4;
        if (invocation.is_error) {
            ++failures;
            CHECK_MSG(false, "混合并发线程 " + std::to_string(index) +
                                 " 失败: " + invocation.text);
            continue;
        }
        const json payload = ParseResult(invocation.text);
        CHECK_MSG(payload.is_object(),
                  "混合并发 " + std::to_string(index) + " 返回非 JSON 对象");
        if (!payload.is_object()) {
            ++failures;
            continue;
        }
        const json* item = nullptr;
        if (tool == 0) {
            if (payload["snapshots"].is_array() &&
                !payload["snapshots"].empty()) {
                item = &payload["snapshots"][0];
            }
        } else if (tool == 1 || tool == 2) {
            if (payload["hosts"].is_array() && !payload["hosts"].empty()) {
                item = &payload["hosts"][0];
            }
        } else {
            if (payload["health"].is_object()) {
                item = &payload["health"];
            }
        }
        if (item == nullptr) {
            ++failures;
            CHECK_MSG(false, "混合并发 " + std::to_string(index) +
                                 " 缺少数据字段: " + invocation.text);
            continue;
        }
        CHECK_MSG(item->value("tool", -1) == tool,
                  "混合并发 " + std::to_string(index) +
                      " 命中了错误端点，期望 " + std::to_string(tool) +
                      "，实际 " + std::to_string(item->value("tool", -1)));
        CHECK_MSG(item->value("url", std::string{}) ==
                      "http://mixed-" + std::to_string(index) + ".example",
                  "混合并发 " + std::to_string(index) + " URL 串扰");
    }
    CHECK_MSG(failures == 0, "混合并发应全部成功");
    CHECK_MSG(client->CallCount(ThreadSafeFakeClient::Endpoint::kLatest) ==
                  static_cast<std::size_t>(kPerTool),
              "混合并发 latest 计数应为 25");
    CHECK_MSG(client->CallCount(ThreadSafeFakeClient::Endpoint::kStats) ==
                  static_cast<std::size_t>(kPerTool),
              "混合并发 stats 计数应为 25");
    CHECK_MSG(client->CallCount(ThreadSafeFakeClient::Endpoint::kKernel) ==
                  static_cast<std::size_t>(kPerTool),
              "混合并发 kernel 计数应为 25");
    CHECK_MSG(client->CallCount(ThreadSafeFakeClient::Endpoint::kHealth) ==
                  static_cast<std::size_t>(kPerTool),
              "混合并发 health 计数应为 25");
    CHECK_MSG(client->TotalCalls() == static_cast<std::size_t>(kThreads),
              "混合并发总调用数应为 100");
}

/// 3) 并发 + 失败注入：失败只影响对应请求，错误可诊断。
void TestConcurrentFailures() {
    auto client = std::make_shared<ThreadSafeFakeClient>();
    client->FailEvery(3);  // 每 3 个请求失败 1 个
    Registry registry;
    RegisterMonitorTools(registry, client);
    constexpr int kThreads = 90;
    std::vector<Invocation> results(kThreads);
    std::vector<std::thread> threads;
    std::atomic<bool> start{false};
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results[static_cast<std::size_t>(index)] = CallTool(
                registry, "os_monitor_latest", {{"limit", 1 + index % 10}});
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    int errors = 0;
    int successes = 0;
    for (int index = 0; index < kThreads; ++index) {
        const Invocation& invocation = results[static_cast<std::size_t>(index)];
        if (invocation.is_error) {
            ++errors;
            CHECK_MSG(invocation.text.find("fake: injected failure") !=
                          std::string::npos,
                      "注入失败应原样返回诊断文本，实际: " + invocation.text);
        } else {
            ++successes;
            const json payload = ParseResult(invocation.text);
            CHECK_MSG(payload.is_object(),
                      "成功请求应返回可解析 JSON: " + invocation.text);
        }
    }
    CHECK_MSG(errors + successes == kThreads, "失败+成功应等于总请求数");
    CHECK_MSG(errors > 0 && successes > 0,
              "失败注入下应同时存在成功与失败请求，实际 errors=" +
                  std::to_string(errors) + " successes=" +
                  std::to_string(successes));
    CHECK_MSG(static_cast<int>(
                  client->CallCount(ThreadSafeFakeClient::Endpoint::kLatest)) ==
                  kThreads,
              "失败注入下调用计数仍应为 90");
}

/// 4) 并发调用 + 注册/注销写线程：最终工具表与调用都不被破坏。
void TestConcurrentMutation() {
    auto client = std::make_shared<ThreadSafeFakeClient>();
    Registry registry;
    RegisterMonitorTools(registry, client);

    std::atomic<bool> stop{false};
    std::atomic<int> mutation_failures{0};
    std::vector<std::thread> writers;
    for (int writer = 0; writer < 2; ++writer) {
        writers.emplace_back([&, writer] {
            for (int round = 0; round < 100 && !stop.load(); ++round) {
                const std::string name =
                    "mutation-" + std::to_string(writer) + "-" +
                    std::to_string(round);
                try {
                    registry.RegisterTool(
                        go2cpp::mcp::Tool{name, "并发写线程注册的工具",
                                          json{{"type", "object"}},
                                          [](const ToolContext&) {
                                              return go2cpp::mcp::ToolResult::Text(
                                                  "ok");
                                          }});
                    (void)registry.UnregisterTool(name);
                } catch (...) {
                    mutation_failures.fetch_add(1);
                }
            }
        });
    }

    constexpr int kReaders = 100;
    std::vector<Invocation> results(kReaders);
    std::vector<std::thread> readers;
    for (int index = 0; index < kReaders; ++index) {
        readers.emplace_back([&, index] {
            results[static_cast<std::size_t>(index)] =
                CallTool(registry, "os_monitor_latest",
                         {{"manager_url",
                           "http://mutate-" + std::to_string(index) + ".example"}});
        });
    }
    for (auto& reader : readers) {
        reader.join();
    }
    stop.store(true);
    for (auto& writer : writers) {
        writer.join();
    }

    int failures = 0;
    for (const auto& invocation : results) {
        if (invocation.is_error) {
            ++failures;
        }
    }
    CHECK_MSG(failures == 0,
              "写线程干扰下 100 次工具调用应全部成功，失败 " +
                  std::to_string(failures));
    CHECK_MSG(mutation_failures.load() == 0,
              "并发注册/注销不应抛异常");
    CHECK_MSG(registry.tool_count() == 4,
              "写线程结束后工具表应恢复为 4 个监控工具，实际 " +
                  std::to_string(registry.tool_count()));
}

}  // namespace

int main() {
    TestHundredLatestCalls();
    TestMixedTools();
    TestConcurrentFailures();
    TestConcurrentMutation();
    return go2cpp::mcp::test::Summary("monitor_concurrent_tools");
}
