/**
 * @file test_robot_control.cpp
 * @brief ROS 2 工具组端到端测试：用临时 PATH 上的假 `ros2` 脚本验证
 *        启动/输出捕获/超时/非零退出码/命令不存在/白名单与变更保护。
 */

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <unistd.h>

#include "test_util.hpp"

#include "mcp/tools/builtin_tools.hpp"

namespace {

using SteadyClock = std::chrono::steady_clock;

std::pair<bool, std::string> CallTool(const go2cpp::mcp::Registry& registry, const std::string& name,
                                      const go2cpp::mcp::json& arguments) {
    go2cpp::mcp::ToolContext context;
    context.arguments = arguments;
    const auto result = registry.InvokeTool(name, context);
    if (!result) {
        return {true, "__missing__"};
    }
    return {result->is_error, result->content.empty() ? "" : result->content[0].text};
}

go2cpp::mcp::json ParsePayload(const std::string& text) {
    try {
        return go2cpp::mcp::json::parse(text);
    } catch (const std::exception&) {
        return go2cpp::mcp::json::object();
    }
}

/// PATH 前插/替换，析构时恢复原值。
class ScopedPath final {
public:
    explicit ScopedPath(const std::string& prefix) {
        const char* current = std::getenv("PATH");
        m_saved = current != nullptr ? current : "";
        Set(prefix + ":" + m_saved);
    }

    ~ScopedPath() { Restore(); }

    ScopedPath(const ScopedPath&) = delete;
    ScopedPath& operator=(const ScopedPath&) = delete;

    void Set(const std::string& value) { (void)::setenv("PATH", value.c_str(), 1); }
    void Restore() { (void)::setenv("PATH", m_saved.c_str(), 1); }

private:
    std::string m_saved;
};

/// 测试结束删除临时目录。
class TempDirGuard final {
public:
    explicit TempDirGuard(std::filesystem::path path) : m_path(std::move(path)) {}
    ~TempDirGuard() {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;

private:
    std::filesystem::path m_path;
};

}  // namespace

int main() {
    namespace fs = std::filesystem;

    const fs::path directory =
        fs::temp_directory_path() / ("mcp-ros2-fake-" + std::to_string(::getpid()));
    std::error_code cleanup_error;
    fs::remove_all(directory, cleanup_error);
    fs::create_directories(directory);
    TempDirGuard directory_guard(directory);

    const fs::path script = directory / "ros2";
    {
        std::ofstream stream(script, std::ios::binary | std::ios::trunc);
        stream << R"SH(#!/bin/sh
case "$1 $2" in
  "node list")
    echo "node_alpha"
    echo "node_beta"
    exit 0
    ;;
  "topic list")
    printf 'starting\n'
    exec sleep 30
    ;;
  "service list")
    echo 'permission denied' >&2
    exit 7
    ;;
  *)
    echo "unexpected:$*"
    exit 2
    ;;
esac
)SH";
    }
    fs::permissions(script, fs::perms::owner_all, fs::perm_options::replace);

    ScopedPath path_guard(directory.string());

    go2cpp::mcp::Registry registry;
    go2cpp::mcp::tools::RegisterRobotControlTools(registry);

    // 1. 启动并捕获 stdout，退出码 0。
    {
        const auto result = CallTool(registry, "ros2_node_list", {{"timeout_ms", 3000}});
        CHECK(!result.first);
        const go2cpp::mcp::json payload = ParsePayload(result.second);
        CHECK(payload.value("exitCode", -999) == 0);
        CHECK(payload.value("timedOut", true) == false);
        CHECK(payload.value("command", std::string()) == "ros2 node list");
        CHECK(payload.value("output", std::string()).find("node_alpha") != std::string::npos);
        CHECK(payload.value("output", std::string()).find("node_beta") != std::string::npos);
    }

    // 2. 超时：timedOut=true、exitCode=-1、已有输出保留。
    {
        const auto begin = SteadyClock::now();
        const auto result = CallTool(registry, "ros2_topic_list", {{"timeout_ms", 300}});
        const long long elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - begin)
                .count();
        CHECK(result.first);
        const go2cpp::mcp::json payload = ParsePayload(result.second);
        CHECK(payload.value("timedOut", false) == true);
        CHECK(payload.value("exitCode", 0) == -1);
        CHECK(payload.value("output", std::string()).find("starting") != std::string::npos);
        CHECK(elapsed < 3000);
    }

    // 3. 非零退出码与 stderr 合并。
    {
        const auto result = CallTool(registry, "ros2_service_list", {{"timeout_ms", 3000}});
        CHECK(result.first);
        const go2cpp::mcp::json payload = ParsePayload(result.second);
        CHECK(payload.value("timedOut", true) == false);
        CHECK(payload.value("exitCode", 0) == 7);
        CHECK(payload.value("output", std::string()).find("permission denied") !=
              std::string::npos);
    }

    // 4. 变更保护：run/launch/set 未显式授权时在启动前拒绝。
    {
        go2cpp::mcp::json arguments = go2cpp::mcp::json::object();
        arguments["args"] = go2cpp::mcp::json::array({"run", "demo", "node"});
        const auto result = CallTool(registry, "ros2_command", arguments);
        CHECK(result.first);
        CHECK(result.second.find("allow_mutating") != std::string::npos);
    }

    // 5. 白名单内的查询命令可通过 ros2_command 执行。
    {
        go2cpp::mcp::json arguments = go2cpp::mcp::json::object();
        arguments["args"] = go2cpp::mcp::json::array({"node", "list"});
        arguments["timeout_ms"] = 3000;
        const auto result = CallTool(registry, "ros2_command", arguments);
        CHECK(!result.first);
        const go2cpp::mcp::json payload = ParsePayload(result.second);
        CHECK(payload.value("exitCode", -1) == 0);
        CHECK(payload.value("output", std::string()).find("node_alpha") != std::string::npos);
    }

    // 6. 命令不存在：exitCode=127，不崩溃。放在最后，避免影响上面的假 ros2。
    {
        const fs::path empty = directory / "empty";
        fs::create_directories(empty);
        path_guard.Set(empty.string());
        const auto result = CallTool(registry, "ros2_node_list", {{"timeout_ms", 1000}});
        CHECK(result.first);
        const go2cpp::mcp::json payload = ParsePayload(result.second);
        CHECK(payload.value("exitCode", 0) == 127);
        CHECK(payload.value("output", std::string()).find("exec failed") != std::string::npos);
    }

    return go2cpp::mcp::test::Summary("robot_control");
}
