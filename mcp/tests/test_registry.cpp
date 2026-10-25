/**
 * @file test_registry.cpp
 * @brief 注册表单元测试：注册、覆盖、查找、并发。
 */

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "test_util.hpp"

#include "mcp/server/registry.hpp"

namespace {

go2cpp::mcp::Tool MakeTool(const std::string& name, int value) {
    go2cpp::mcp::Tool tool;
    tool.name = name;
    tool.description = "test tool";
    tool.handler = [value](const go2cpp::mcp::ToolContext&) {
        return go2cpp::mcp::ToolResult::Text(std::to_string(value));
    };
    return tool;
}

}  // namespace

int main() {
    using namespace go2cpp::mcp;

    // 注册与列表
    {
        Registry registry;
        registry.RegisterTool(MakeTool("a", 1));
        registry.RegisterTool(MakeTool("b", 2));
        CHECK(registry.tool_count() == 2);

        const json listed = registry.ToolList();
        CHECK(listed["tools"].size() == 2);
        CHECK_EQ(listed["tools"][0]["name"].get<std::string>(), "a");
    }

    // 同名覆盖
    {
        Registry registry;
        registry.RegisterTool(MakeTool("a", 1));
        registry.RegisterTool(MakeTool("a", 9));
        CHECK(registry.tool_count() == 1);

        ToolContext context;
        const auto result = registry.InvokeTool("a", context);
        CHECK(result.has_value());
        CHECK_EQ(result->content[0].text, "9");
    }

    // 不存在
    {
        Registry registry;
        ToolContext context;
        CHECK(!registry.InvokeTool("missing", context).has_value());
    }

    // 参数透传
    {
        Registry registry;
        Tool tool;
        tool.name = "echo_args";
        tool.handler = [](const ToolContext& context) {
            return ToolResult::Text(context.arguments.value("text", ""));
        };
        registry.RegisterTool(std::move(tool));

        ToolContext context;
        context.arguments = json{{"text", "hello"}};
        const auto result = registry.InvokeTool("echo_args", context);
        CHECK(result && result->content[0].text == "hello");
    }

    // 提示与资源
    {
        Registry registry;
        Prompt prompt;
        prompt.name = "p";
        prompt.handler = [](const json& arguments) {
            return json{{"echo", arguments.value("x", "")}};
        };
        registry.RegisterPrompt(std::move(prompt));

        Resource resource;
        resource.uri = "test:///r";
        resource.reader = [](const json&) { return json{{"contents", json::array()}}; };
        registry.RegisterResource(std::move(resource));

        CHECK(registry.prompt_count() == 1);
        CHECK(registry.resource_count() == 1);
        CHECK(registry.GetPrompt("p", json{{"x", "y"}}).value()["echo"] == "y");
        CHECK(registry.ReadResource("test:///r").has_value());
        CHECK(!registry.GetPrompt("missing", json::object()).has_value());
        CHECK(!registry.ReadResource("missing:///r").has_value());
    }

    // 并发注册 + 调用（验证锁与「锁外回调」不会自死锁）
    {
        Registry registry;
        std::atomic<int> completed{0};
        std::vector<std::thread> threads;
        for (int index = 0; index < 8; ++index) {
            threads.emplace_back([&registry, &completed, index] {
                for (int round = 0; round < 50; ++round) {
                    registry.RegisterTool(MakeTool("t" + std::to_string(index), round));
                    ToolContext context;
                    (void)registry.InvokeTool("t" + std::to_string(index), context);
                    (void)registry.ToolList();
                }
                completed.fetch_add(1);
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(completed.load() == 8);
        CHECK(registry.tool_count() == 8);
    }

    // 工具内再次注册不应死锁
    {
        Registry registry;
        Tool tool;
        tool.name = "reentrant";
        tool.handler = [&registry](const ToolContext&) {
            registry.RegisterTool(MakeTool("inner", 1));
            return ToolResult::Text("ok");
        };
        registry.RegisterTool(std::move(tool));

        ToolContext context;
        const auto result = registry.InvokeTool("reentrant", context);
        CHECK(result.has_value());
        CHECK(registry.tool_count() == 2);
    }

    // 注销：从注册表与 tools/list 中消失；重复注销返回 false。
    {
        Registry registry;
        registry.RegisterTool(MakeTool("a", 1));
        registry.RegisterTool(MakeTool("b", 2));
        CHECK(registry.UnregisterTool("a"));
        CHECK(!registry.UnregisterTool("a"));
        CHECK(registry.tool_count() == 1);
        ToolContext context;
        CHECK(!registry.InvokeTool("a", context).has_value());
        const json listed = registry.ToolList();
        CHECK(listed["tools"].size() == 1);
        CHECK_EQ(listed["tools"][0]["name"].get<std::string>(), "b");
    }

    // 变更事件：新增/覆盖/注销的顺序与类型；退订后不再收到。
    {
        Registry registry;
        std::vector<std::pair<int, std::string>> events;
        const std::size_t id = registry.AddToolListener(
            [&events](ToolChangeKind kind, const Tool& tool) {
                events.emplace_back(static_cast<int>(kind), tool.name);
            });
        CHECK(registry.tool_listener_count() == 1);

        registry.RegisterTool(MakeTool("a", 1));
        registry.RegisterTool(MakeTool("a", 2));
        CHECK(registry.UnregisterTool("a"));
        CHECK(events.size() == 3);
        if (events.size() == 3) {
            CHECK(events[0].first == static_cast<int>(ToolChangeKind::kAdded));
            CHECK(events[1].first == static_cast<int>(ToolChangeKind::kUpdated));
            CHECK(events[2].first == static_cast<int>(ToolChangeKind::kRemoved));
            CHECK_EQ(events[0].second, "a");
            CHECK_EQ(events[1].second, "a");
            CHECK_EQ(events[2].second, "a");
        }

        CHECK(registry.RemoveToolListener(id));
        CHECK(!registry.RemoveToolListener(id));
        CHECK(registry.tool_listener_count() == 0);
        registry.RegisterTool(MakeTool("c", 3));
        CHECK(events.size() == 3);
    }

    // 监听器在注册表锁外调用：回调里再次注册/注销不会自死锁。
    {
        Registry registry;
        std::atomic<int> fired{0};
        const std::size_t id = registry.AddToolListener(
            [&registry, &fired](ToolChangeKind kind, const Tool& tool) {
                if (kind == ToolChangeKind::kAdded && tool.name == "outer") {
                    registry.RegisterTool(MakeTool("inner", 1));
                    registry.UnregisterTool("inner");
                    fired.fetch_add(1);
                }
            });
        registry.RegisterTool(MakeTool("outer", 1));
        CHECK(fired.load() == 1);
        CHECK(registry.tool_count() == 1);
        CHECK(registry.RemoveToolListener(id));
    }

    // 并发注册/注销 + 事件计数：监听器快照不能丢事件或崩溃。
    {
        Registry registry;
        std::atomic<int> events{0};
        const std::size_t id = registry.AddToolListener(
            [&events](ToolChangeKind, const Tool&) { events.fetch_add(1); });
        std::vector<std::thread> threads;
        for (int index = 0; index < 8; ++index) {
            threads.emplace_back([&registry, index] {
                const std::string name = "c" + std::to_string(index);
                for (int round = 0; round < 50; ++round) {
                    registry.RegisterTool(MakeTool(name, round));
                    registry.UnregisterTool(name);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(registry.tool_count() == 0);
        CHECK(events.load() == 8 * 50 * 2);
        CHECK(registry.RemoveToolListener(id));
    }

    return go2cpp::mcp::test::Summary("registry");
}
