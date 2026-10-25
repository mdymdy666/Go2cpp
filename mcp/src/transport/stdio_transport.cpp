/**
 * @file stdio_transport.cpp
 * @brief stdin/stdout 传输实现。
 */

#include "stdio_transport.hpp"

#include <cstdio>
#include <iostream>

namespace go2cpp::mcp {

bool StdioTransport::Start() {
    m_stopped.store(false);
    return true;
}

void StdioTransport::Stop() {
    m_stopped.store(true);
}

std::optional<std::string> StdioTransport::ReadMessage() {
    std::string line;
    while (!m_stopped.load()) {
        if (!std::getline(std::cin, line)) {
            return std::nullopt;  // EOF：客户端关闭了管道
        }
        // 容忍空行（部分客户端在握手前后会多发换行）。
        if (!line.empty()) {
            return line;
        }
    }
    return std::nullopt;
}

bool StdioTransport::WriteMessage(std::string_view message) {
    std::lock_guard<std::mutex> lock(m_write_mutex);
    if (m_stopped.load()) {
        return false;
    }
    const std::size_t written = std::fwrite(message.data(), 1, message.size(), stdout);
    const int newline = std::fputc('\n', stdout);
    const int flushed = std::fflush(stdout);
    return written == message.size() && newline != EOF && flushed == 0;
}

}  // namespace go2cpp::mcp
