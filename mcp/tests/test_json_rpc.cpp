/**
 * @file test_json_rpc.cpp
 * @brief JSON-RPC 解析与编码的单元测试。
 */

#include <string>

#include "test_util.hpp"

#include "mcp/protocol/json_rpc.hpp"

int main() {
    using namespace go2cpp::mcp;

    // 正常请求
    {
        const auto outcome = ParseRequest(json::parse(R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})"));
        CHECK(outcome.ok);
        CHECK(!outcome.request.notification);
        CHECK_EQ(outcome.request.method, "tools/list");
        CHECK(outcome.request.params.is_object());
        CHECK(outcome.request.params.empty());
    }

    // 字符串 id 原样保留
    {
        const auto outcome = ParseRequest(
            json::parse(R"({"jsonrpc":"2.0","id":"abc","method":"ping","params":{}})"));
        CHECK(outcome.ok);
        CHECK(outcome.request.id.is_string());
        CHECK_EQ(outcome.request.id.get<std::string>(), "abc");
    }

    // 无 id => 通知
    {
        const auto outcome = ParseRequest(json::parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
        CHECK(outcome.ok);
        CHECK(outcome.request.notification);
        CHECK(!outcome.request.ExpectsResponse());
    }

    // 版本错误
    {
        const auto outcome = ParseRequest(json::parse(R"({"jsonrpc":"1.0","id":1,"method":"ping"})"));
        CHECK(!outcome.ok);
        CHECK_EQ(outcome.error["error"]["message"].get<std::string>(), "jsonrpc must be \"2.0\"");
        // 设计选择：能确定 id 时回填，客户端才能把错误和请求对上；规范允许
        // 在无法判定 id 时才用 null。
        CHECK_EQ(outcome.error["id"].dump(), "1");
    }

    // 非对象
    {
        const auto outcome = ParseRequest(json::parse(R"([1,2,3])"));
        CHECK(!outcome.ok);
        CHECK_EQ(outcome.error["error"]["code"].dump(), "-32700");
    }

    // 缺 method
    {
        const auto outcome = ParseRequest(json::parse(R"({"jsonrpc":"2.0","id":1})"));
        CHECK(!outcome.ok);
        CHECK_EQ(outcome.error["error"]["code"].dump(), "-32600");
    }

    // params 类型非法
    {
        const auto outcome = ParseRequest(json::parse(R"({"jsonrpc":"2.0","id":1,"method":"x","params":"str"})"));
        CHECK(!outcome.ok);
        CHECK_EQ(outcome.error["error"]["code"].dump(), "-32602");
    }

    // MakeResult 结构
    {
        const json response = MakeResult(3, json{{"ok", true}});
        CHECK_EQ(response["jsonrpc"].get<std::string>(), "2.0");
        CHECK_EQ(response["id"].dump(), "3");
        CHECK(response["result"]["ok"].get<bool>());
    }

    // MakeError 结构
    {
        const json response = MakeError("x", RpcError::kMethodNotFound, "nope");
        CHECK_EQ(response["jsonrpc"].get<std::string>(), "2.0");
        CHECK_EQ(response["error"]["code"].dump(), "-32601");
        CHECK_EQ(response["error"]["message"].get<std::string>(), "nope");
        CHECK(!response["error"].contains("data"));
    }

    // MakeNotification 无 id
    {
        const json message = MakeNotification("tools/list_changed");
        CHECK(!message.contains("id"));
        CHECK_EQ(message["method"].get<std::string>(), "tools/list_changed");
    }

    return go2cpp::mcp::test::Summary("json_rpc");
}
