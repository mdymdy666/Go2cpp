/**
 * @file test_monitor_args_fuzz.cpp
 * @brief `os_monitor_*` 参数边界矩阵 + 随机 fuzz + handler 侧 clamp。
 *
 * 覆盖：
 * 1. limit 边界矩阵 0 / 1 / 256 / 257：1 与 256 接受，0 与 257 拒绝；
 * 2. timeout_ms 边界矩阵 0 / 100 / 60000 / 60001：100 与 60000 接受，0 与
 *    60001 拒绝；
 * 3. health 不接受 limit（schema 无该字段），四个工具都接受 timeout_ms；
 * 4. 类型错误（字符串/浮点/布尔/数组/对象/null）与未知字段一律拒绝；
 * 5. 1000 组随机参数 fuzz：校验器不崩溃，拒绝时每条 issue 都有路径与诊断；
 * 6. 合法组合直接调用工具（绕过协议层校验）时 handler 必须 clamp 到
 *    [1,256] / [100,60000]，不能把越界值透传给 Manager；
 * 7. 统计接受/拒绝分布，作为随机测试数据写进报告。
 */

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "test_util.hpp"

#include "mcp/server/registry.hpp"
#include "mcp/tools/monitor_client.hpp"
#include "mcp/tools/monitor_tools.hpp"
#include "mcp/validation/validators.hpp"

namespace {

using go2cpp::mcp::json;
using go2cpp::mcp::Registry;
using go2cpp::mcp::ToolContext;
using go2cpp::mcp::tools::MockMonitorClient;
using go2cpp::mcp::tools::MonitorQuery;
using go2cpp::mcp::validation::Result;
using go2cpp::mcp::validation::ValidateToolArguments;

const char* const kTools[4] = {"os_monitor_latest", "os_monitor_stats",
                               "os_monitor_kernel", "os_monitor_health"};

std::pair<bool, std::string> CallTool(const Registry& registry,
                                      const std::string& name,
                                      const json& arguments) {
    ToolContext context;
    context.arguments = arguments;
    const auto result = registry.InvokeTool(name, context);
    if (!result) {
        return {true, "__missing__"};
    }
    return {result->is_error,
            result->content.empty() ? "" : result->content[0].text};
}

json ParseResult(const std::string& text) {
    try {
        return json::parse(text);
    } catch (const std::exception&) {
        return json(nullptr);
    }
}

bool Valid(const Registry& registry, const std::string& tool,
           const json& arguments) {
    const auto schema = registry.ToolSchema(tool);
    if (!schema) {
        return false;
    }
    return ValidateToolArguments(*schema, arguments).ok;
}

/// 断言非法参数被拒，并把校验摘要打印出来以便诊断。
void CheckRejected(const Registry& registry, const std::string& tool,
                   const json& arguments, const std::string& label) {
    const auto schema = registry.ToolSchema(tool);
    CHECK_MSG(schema.has_value(), label + "：工具 schema 缺失");
    if (!schema) {
        return;
    }
    const Result result = ValidateToolArguments(*schema, arguments);
    CHECK_MSG(!result.ok, label + "：应被拒绝，实际接受；参数=" +
                              arguments.dump());
    if (!result.ok) {
        CHECK_MSG(!result.issues.empty(), label + "：拒绝时必须给出 issue");
        for (const auto& issue : result.issues) {
            CHECK_MSG(!issue.path.empty() && !issue.message.empty(),
                      label + "：issue 必须带路径与诊断，实际 path=「" +
                          issue.path + "」message=「" + issue.message + "」");
        }
    }
}

void CheckAccepted(const Registry& registry, const std::string& tool,
                   const json& arguments, const std::string& label) {
    const auto schema = registry.ToolSchema(tool);
    CHECK_MSG(schema.has_value(), label + "：工具 schema 缺失");
    if (!schema) {
        return;
    }
    const Result result = ValidateToolArguments(*schema, arguments);
    CHECK_MSG(result.ok, label + "：应被接受，实际拒绝：" + result.Summary() +
                             "；参数=" + arguments.dump());
}

/// 1) limit / timeout 边界矩阵。
void TestBoundaryMatrix() {
    std::shared_ptr<MockMonitorClient> client = std::make_shared<MockMonitorClient>();
    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(registry, client);

    const int limits[] = {0, 1, 256, 257};
    const bool limit_expected[] = {false, true, true, false};
    const int timeouts[] = {0, 100, 60000, 60001};
    const bool timeout_expected[] = {false, true, true, false};

    for (const char* tool : {"os_monitor_latest", "os_monitor_stats",
                             "os_monitor_kernel"}) {
        for (std::size_t index = 0; index < 4; ++index) {
            const json arguments = {{"limit", limits[index]}};
            const std::string label =
                std::string(tool) + "/limit=" + std::to_string(limits[index]);
            if (limit_expected[index]) {
                CheckAccepted(registry, tool, arguments, label);
            } else {
                CheckRejected(registry, tool, arguments, label);
            }
            // 边界值 + 合法 timeout 的组合也必须保持同一结论。
            const json combined = {{"limit", limits[index]},
                                   {"timeout_ms", 3000}};
            if (limit_expected[index]) {
                CheckAccepted(registry, tool, combined, label + "+timeout");
            } else {
                CheckRejected(registry, tool, combined, label + "+timeout");
            }
        }
        for (std::size_t index = 0; index < 4; ++index) {
            const json arguments = {{"timeout_ms", timeouts[index]}};
            const std::string label =
                std::string(tool) + "/timeout=" + std::to_string(timeouts[index]);
            if (timeout_expected[index]) {
                CheckAccepted(registry, tool, arguments, label);
            } else {
                CheckRejected(registry, tool, arguments, label);
            }
        }
    }

    // health：timeout 边界相同，但没有 limit 字段。
    for (std::size_t index = 0; index < 4; ++index) {
        const json arguments = {{"timeout_ms", timeouts[index]}};
        const std::string label =
            "os_monitor_health/timeout=" + std::to_string(timeouts[index]);
        if (timeout_expected[index]) {
            CheckAccepted(registry, "os_monitor_health", arguments, label);
        } else {
            CheckRejected(registry, "os_monitor_health", arguments, label);
        }
    }
    for (int limit : limits) {
        CheckRejected(registry, "os_monitor_health", {{"limit", limit}},
                      "health 不接受 limit=" + std::to_string(limit));
    }
}

/// 2) 类型错误 / 未知字段 / 空参数。
void TestTypeAndUnknownFields() {
    std::shared_ptr<MockMonitorClient> client = std::make_shared<MockMonitorClient>();
    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(registry, client);
    const char* tool = "os_monitor_latest";

    CheckAccepted(registry, tool, json::object(), "空参数");
    CheckAccepted(registry, tool, {{"limit", 1}, {"timeout_ms", 100}},
                  "最小合法组合");
    CheckAccepted(registry, tool, {{"limit", 256}, {"timeout_ms", 60000}},
                  "最大合法组合");
    CheckAccepted(registry, tool,
                  {{"manager_url", "http://127.0.0.1:9101"},
                   {"limit", 10}, {"timeout_ms", 3000}},
                  "完整合法组合");
    // schema 的 pattern 只限制前缀，https 在协议层通过；工具 handler 再给出
    // 「仅支持 http://」的具体诊断（这是文档化的限制）。
    CheckAccepted(registry, tool, {{"manager_url", "https://127.0.0.1:9101"}},
                  "https 前缀 schema 通过");
    CheckRejected(registry, tool, {{"manager_url", "ftp://127.0.0.1:9101"}},
                  "非 http(s) scheme");
    CheckRejected(registry, tool, {{"manager_url", 9101}}, "manager_url 数字");
    CheckRejected(registry, tool, {{"manager_url", nullptr}},
                  "manager_url null");
    CheckRejected(registry, tool, {{"manager_url", json::array({"http://a"})}},
                  "manager_url 数组");
    CheckRejected(registry, tool, {{"manager_url", json::object()}},
                  "manager_url 对象");
    CheckRejected(registry, tool, {{"limit", "10"}}, "limit 字符串");
    CheckRejected(registry, tool, {{"limit", 10.5}}, "limit 浮点");
    CheckRejected(registry, tool, {{"limit", true}}, "limit 布尔");
    CheckRejected(registry, tool, {{"limit", nullptr}}, "limit null");
    CheckRejected(registry, tool, {{"timeout_ms", "3000"}},
                  "timeout 字符串");
    CheckRejected(registry, tool, {{"timeout_ms", 3000.5}},
                  "timeout 浮点");
    CheckRejected(registry, tool, {{"unexpected", 1}}, "未知字段");
    CheckRejected(registry, tool,
                  {{"limit", 10}, {"unknown", "x"}, {"timeout_ms", 1000}},
                  "合法字段中混入未知字段");

    // null arguments：协议层一定有对象；直接喂 null 不允许崩溃。
    const auto schema = registry.ToolSchema(tool);
    CHECK(schema.has_value());
    if (schema) {
        bool threw = false;
        try {
            const Result result = ValidateToolArguments(*schema, json(nullptr));
            CHECK_MSG(!result.ok || result.issues.empty(),
                      "null arguments 的校验结果必须自洽");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK_MSG(!threw, "null arguments 不应让校验器抛异常");
    }
}

/// 3) 随机 fuzz：1000 组参数，校验器不崩溃、issue 结构完整。
void TestRandomFuzz() {
    std::shared_ptr<MockMonitorClient> client = std::make_shared<MockMonitorClient>();
    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(registry, client);

    std::mt19937 rng(20261025u);
    std::uniform_int_distribution<int> tool_pick(0, 3);
    std::uniform_int_distribution<int> kind_pick(0, 9);
    std::uniform_int_distribution<int> int_pick(-100, 70000);
    int accepted = 0;
    int rejected = 0;
    int unexpected_errors = 0;

    for (int round = 0; round < 1000; ++round) {
        const char* tool = kTools[tool_pick(rng)];
        json arguments = json::object();
        // 一半用例从合法骨架出发，再按概率注入一个非法点；另一半纯随机。
        const bool structured = (rng() % 2) == 0;
        if (structured) {
            if (rng() % 2 == 0) {
                arguments["limit"] = 1 + static_cast<int>(rng() % 256);
            }
            if (rng() % 2 == 0) {
                arguments["timeout_ms"] = 100 + static_cast<int>(rng() % 59901);
            }
            if (rng() % 3 == 0) {
                arguments["manager_url"] = "http://127.0.0.1:9101";
            }
            switch (rng() % 6) {
                case 0:
                    arguments["limit"] = 0;  // 越界下沿
                    break;
                case 1:
                    arguments["limit"] = 257;  // 越界上沿
                    break;
                case 2:
                    arguments["timeout_ms"] = 0;
                    break;
                case 3:
                    arguments["timeout_ms"] = 60001;
                    break;
                case 4:
                    arguments["unknown_key"] = int_pick(rng);
                    break;
                default:
                    break;
            }
        } else {
            const int kind = kind_pick(rng);
            if (kind == 0) {
                arguments["limit"] = nullptr;
            } else if (kind == 1) {
                arguments["limit"] = "ten";
            } else if (kind == 2) {
                arguments["limit"] = int_pick(rng);
            } else if (kind == 3) {
                arguments["timeout_ms"] = "fast";
            } else if (kind == 4) {
                arguments["timeout_ms"] = int_pick(rng);
            } else if (kind == 5) {
                arguments["manager_url"] = int_pick(rng);
            } else if (kind == 6) {
                arguments["manager_url"] = json::array({1, 2, 3});
            } else if (kind == 7) {
                arguments["random_field"] = json::object({{"nested", true}});
            } else if (kind == 8) {
                arguments = json::array({1, 2, 3});
            } else {
                arguments = json(nullptr);
            }
        }

        const auto schema = registry.ToolSchema(tool);
        if (!schema) {
            ++unexpected_errors;
            continue;
        }
        try {
            const Result result = ValidateToolArguments(*schema, arguments);
            if (result.ok) {
                ++accepted;
            } else {
                ++rejected;
                for (const auto& issue : result.issues) {
                    if (issue.path.empty() || issue.message.empty()) {
                        ++unexpected_errors;
                    }
                }
            }
        } catch (const std::exception&) {
            ++unexpected_errors;
        }

        // 合法参数再走一次 handler，验证随机输入不会让工具崩溃。
        if (Valid(registry, tool, arguments)) {
            const auto invocation = CallTool(registry, tool, arguments);
            if (invocation.second == "__missing__") {
                ++unexpected_errors;
            } else if (!invocation.first) {
                const json payload = ParseResult(invocation.second);
                if (payload.is_null()) {
                    ++unexpected_errors;
                }
            }
        }
    }

    CHECK_MSG(unexpected_errors == 0,
              "随机 fuzz 不应出现结构异常/崩溃，异常数 " +
                  std::to_string(unexpected_errors));
    CHECK_MSG(accepted > 0 && rejected > 0,
              "随机 fuzz 应同时覆盖接受与拒绝，accepted=" +
                  std::to_string(accepted) + " rejected=" +
                  std::to_string(rejected));
    CHECK_MSG(accepted + rejected == 1000,
              "1000 组随机参数都应得到明确结论，实际 " +
                  std::to_string(accepted + rejected));
    std::printf("[args_fuzz] 1000 组随机参数：接受 %d，拒绝 %d\n", accepted,
                rejected);
}

/// 4) handler 侧 clamp：绕过 schema 直接调用时不能把越界值透传。
void TestHandlerClamp() {
    auto client = std::make_shared<MockMonitorClient>();
    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(registry, client);

    struct Case {
        int limit;
        int timeout_ms;
        int expected_limit;
        int expected_timeout;
    };
    const std::vector<Case> cases = {
        {0, 0, 1, 100},
        {1, 100, 1, 100},
        {256, 60000, 256, 60000},
        {257, 60001, 256, 60000},
        {-5, -5, 1, 100},
        {1000000, 1000000, 256, 60000},
    };
    for (const auto& item : cases) {
        client->ClearCalls();
        const auto result = CallTool(
            registry, "os_monitor_latest",
            {{"limit", item.limit}, {"timeout_ms", item.timeout_ms}});
        const std::string label = "clamp limit=" + std::to_string(item.limit) +
                                  " timeout=" + std::to_string(item.timeout_ms);
        CHECK_MSG(!result.first, label + "：绕过 schema 后 handler 不应报错");
        CHECK_MSG(client->CallCount(MockMonitorClient::Endpoint::kLatest) == 1,
                  label + "：应恰好调用一次 Latest");
        if (client->calls().empty()) {
            continue;
        }
        const MonitorQuery& query = client->calls().back().query;
        CHECK_MSG(query.limit == item.expected_limit,
                  label + "：limit 应 clamp 到 " +
                      std::to_string(item.expected_limit) + "，实际 " +
                      std::to_string(query.limit));
        CHECK_MSG(query.timeout_ms == item.expected_timeout,
                  label + "：timeout 应 clamp 到 " +
                      std::to_string(item.expected_timeout) + "，实际 " +
                      std::to_string(query.timeout_ms));
    }

    // stats/kernel 的 limit 也走同一 clamp 路径。
    client->ClearCalls();
    (void)CallTool(registry, "os_monitor_stats", {{"limit", 999}});
    (void)CallTool(registry, "os_monitor_kernel", {{"limit", -1}});
    CHECK(client->CallCount(MockMonitorClient::Endpoint::kStats) == 1);
    CHECK(client->CallCount(MockMonitorClient::Endpoint::kKernel) == 1);
    if (client->calls().size() == 2) {
        CHECK_MSG(client->calls()[0].query.limit == 256,
                  "stats limit=999 应 clamp 到 256");
        CHECK_MSG(client->calls()[1].query.limit == 1,
                  "kernel limit=-1 应 clamp 到 1");
    }
}

}  // namespace

int main() {
    TestBoundaryMatrix();
    TestTypeAndUnknownFields();
    TestRandomFuzz();
    TestHandlerClamp();
    return go2cpp::mcp::test::Summary("monitor_args_fuzz");
}
