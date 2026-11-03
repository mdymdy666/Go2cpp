/**
 * @file user_service.cpp
 * @brief 抖音 P0 骨架：user 服务，只实现 GET /health。
 *
 * @details 业务接口（DB / RPC）留到下一步，本文件不访问存储。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "douyin_http.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr std::uint16_t kDefaultPort = 29101;
constexpr const char* kServiceName = "user";

/// @brief 骨架路由：只有 /health。
bool Handle(const douyin::HttpRequest& request, douyin::HttpResponse* response) {
    if (request.method == "GET" && request.target == "/health") {
        response->status = 200;
        response->body = std::string("{\"ok\":true,\"service\":\"") + kServiceName + "\"}";
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = kDefaultPort;
    if (argc > 1) {
        port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    }

    douyin::HttpServer server(kServiceName, port);
    std::string error;
    if (!server.Start(&error)) {
        std::fprintf(stderr, "[%s] %s\n", kServiceName, error.c_str());
        return 1;
    }
    std::printf("[%s] 监听 0.0.0.0:%u\n", kServiceName, static_cast<unsigned>(port));
    std::fflush(stdout);

    server.ServeForever(Handle);
    return 0;
}
