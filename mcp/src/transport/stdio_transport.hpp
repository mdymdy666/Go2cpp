/**
 * @file stdio_transport.hpp
 * @brief stdin/stdout 传输：最常用的 MCP 传输方式（客户端把服务端当子进程）。
 *
 * @details
 * 消息分隔符是换行（MCP stdio 规范：一条 JSON-RPC 消息一行，内容里不得出现
 * 裸换行——JSON 编码器保证这一点）。**stdout 只写协议帧**，日志一律走 stderr，
 * 否则客户端解析会被日志污染。
 */

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "mcp/transport/transport.hpp"

namespace go2cpp::mcp {

class StdioTransport final : public ITransport {
public:
    bool Start() override;
    void Stop() override;
    std::optional<std::string> ReadMessage() override;
    bool WriteMessage(std::string_view message) override;
    std::string Name() const override { return "stdio"; }

private:
    std::atomic<bool> m_stopped{false};
    std::mutex m_write_mutex;
};

}  // namespace go2cpp::mcp
