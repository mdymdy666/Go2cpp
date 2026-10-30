/**
 * @file socket_test_util.hpp
 * @brief socket 工具测试共享辅助：真 socket 构造、fd RAII、工具调用与 JSON 解析。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "mcp/protocol/types.hpp"
#include "mcp/server/registry.hpp"

namespace go2cpp::mcp::test {

using go2cpp::mcp::Registry;
using go2cpp::mcp::ToolContext;
using go2cpp::mcp::json;

/// fd RAII：析构关闭，Release 交出所有权（用于被测代码已经关闭的场景）。
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : m_fd(fd) {}
    ~Fd() { Reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : m_fd(other.Release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    int get() const noexcept { return m_fd; }
    bool valid() const noexcept { return m_fd >= 0; }
    int Release() noexcept {
        const int fd = m_fd;
        m_fd = -1;
        return fd;
    }
    void Reset(int fd = -1) noexcept {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_fd = fd;
    }

private:
    int m_fd{-1};
};

/// 一条已建立的 TCP 环回连接：监听 fd、客户端 fd、服务端 fd。
struct TcpPair {
    Fd listener;
    Fd client;
    Fd server;

    bool valid() const noexcept {
        return listener.valid() && client.valid() && server.valid();
    }
};

/// 创建 TCP 环回连接（bind 端口 0，内核分配）。
inline TcpPair MakeTcpPair() {
    TcpPair pair;
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return pair;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        ::close(listener);
        return pair;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) !=
        0) {
        ::close(listener);
        return pair;
    }
    if (::listen(listener, 1) != 0) {
        ::close(listener);
        return pair;
    }
    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0) {
        ::close(listener);
        return pair;
    }
    if (::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        ::close(client);
        ::close(listener);
        return pair;
    }
    const int server = ::accept(listener, nullptr, nullptr);
    if (server < 0) {
        ::close(client);
        ::close(listener);
        return pair;
    }
    pair.listener.Reset(listener);
    pair.client.Reset(client);
    pair.server.Reset(server);
    return pair;
}

/// 绑定到环回随机端口的 UDP socket。
inline Fd MakeUdpSocket() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return {};
    }
    return Fd(fd);
}

/// 一对已连接的 AF_UNIX SOCK_STREAM fd。
inline std::pair<Fd, Fd> MakeUnixPair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return {};
    }
    return {Fd(fds[0]), Fd(fds[1])};
}

/// 调用工具并返回 (is_error, 第一个文本块)。
inline std::pair<bool, std::string> CallTool(const Registry& registry,
                                            const std::string& name,
                                            const json& arguments) {
    ToolContext context;
    context.arguments = arguments;
    const auto result = registry.InvokeTool(name, context);
    if (!result.has_value()) {
        return {true, "__missing__"};
    }
    return {result->is_error,
            result->content.empty() ? std::string{} : result->content[0].text};
}

/// 解析工具返回的 JSON 文本；失败返回 null。
inline json ParseJson(const std::string& text) {
    try {
        return json::parse(text);
    } catch (const std::exception&) {
        return json(nullptr);
    }
}

/// tools/list 中是否存在该名字。
inline bool HasTool(const Registry& registry, const std::string& name) {
    const json list = registry.ToolList();
    for (const auto& item : list["tools"]) {
        if (item.value("name", std::string{}) == name) {
            return true;
        }
    }
    return false;
}

}  // namespace go2cpp::mcp::test
