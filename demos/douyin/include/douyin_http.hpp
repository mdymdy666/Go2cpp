/**
 * @file douyin_http.hpp
 * @brief 骨架用的最小 HTTP/1.1 服务器：基于 go2cpp::net::Socket，无第三方库。
 *
 * @details 只支持「读一次请求头 + 写一个响应 + 关闭连接」，够 /health 用。
 *          业务实现阶段应替换为 go2cpp::bento::HttpServer。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#pragma once

#include "go2cpp/network/socket.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <sys/socket.h>

namespace douyin {

/// @brief 解析后的请求（只关心请求行）。
struct HttpRequest {
    std::string method;
    std::string target;
    std::string version;
};

/// @brief 待发送的响应。
struct HttpResponse {
    int status{200};
    std::string content_type{"application/json; charset=utf-8"};
    std::string body;
};

/// @brief 命中路由返回 true 并填好 response；返回 false 由服务器回 404。
using HttpHandler = std::function<bool(const HttpRequest&, HttpResponse*)>;

/// @brief 状态码 → reason phrase。
inline const char* StatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        default: return "OK";
    }
}

/// @brief 序列化成完整 HTTP 响应字节流。
inline std::string Serialize(const HttpResponse& response) {
    std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " +
                       StatusText(response.status) + "\r\n";
    head += "Content-Type: " + response.content_type + "\r\n";
    head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";
    return head + response.body;
}

/// @brief 从缓冲区首行解析 method / target / version。
inline bool ParseRequestLine(const std::string& buffer, HttpRequest* request) {
    const std::size_t line_end = buffer.find('\n');
    if (line_end == std::string::npos) {
        return false;
    }
    std::string line = buffer.substr(0, line_end);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const std::size_t first = line.find(' ');
    if (first == std::string::npos) {
        return false;
    }
    const std::size_t second = line.find(' ', first + 1);
    if (second == std::string::npos) {
        return false;
    }
    request->method = line.substr(0, first);
    request->target = line.substr(first + 1, second - first - 1);
    request->version = line.substr(second + 1);
    return !request->method.empty() && !request->target.empty();
}

/// @brief 单连接处理：读请求头 → 交给 handler → 写响应。
inline void ServeConnection(go2cpp::net::Socket& connection, const HttpHandler& handler) {
    std::string buffer;
    char chunk[4096];
    while (buffer.size() < 64 * 1024) {
        const auto result = connection.recv(chunk, sizeof(chunk), 0, std::chrono::milliseconds(2000));
        if (!result.ok() || result.transferred <= 0) {
            break;
        }
        buffer.append(chunk, static_cast<std::size_t>(result.transferred));
        if (buffer.find("\r\n\r\n") != std::string::npos ||
            buffer.find("\n\n") != std::string::npos) {
            break;
        }
    }

    HttpRequest request;
    HttpResponse response;
    bool handled = false;
    if (ParseRequestLine(buffer, &request)) {
        handled = handler(request, &response);
    }
    if (!handled) {
        response.status = 404;
        response.body = "{\"ok\":false,\"error\":\"not found\"}";
    }

    const std::string payload = Serialize(response);
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const auto result =
            connection.send(payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (!result.ok() || result.transferred <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(result.transferred);
    }
}

/// @brief 监听端口并按请求顺序处理（骨架级：单线程 accept 循环）。
class HttpServer {
   public:
    HttpServer(std::string name, std::uint16_t port)
        : m_name(std::move(name)), m_port(port) {}

    /// @brief 绑定并监听 0.0.0.0:port。
    bool Start(std::string* error) {
        m_listener = go2cpp::net::ListenTCP("0.0.0.0", m_port);
        if (!m_listener) {
            if (error != nullptr) {
                *error = m_name + " 监听 0.0.0.0:" + std::to_string(m_port) + " 失败";
            }
            return false;
        }
        return true;
    }

    /// @brief 永久接受连接；进程被信号终止时退出。
    void ServeForever(const HttpHandler& handler) {
        for (;;) {
            auto connection = m_listener->accept();
            if (!connection) {
                continue;
            }
            ServeConnection(*connection, handler);
            connection->close();
        }
    }

    std::uint16_t Port() const noexcept { return m_port; }

   private:
    std::string m_name;
    std::uint16_t m_port;
    go2cpp::net::Socket::Ptr m_listener;
};

}  // namespace douyin
