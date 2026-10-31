#pragma once

/**
 * @file config.h
 * @brief web_server_full 的配置模型与 INI 加载入口。
 * @details 配置在启动时读取一次，之后只读；请求路径不做热更新，
 *          因此 HttpServer 的多个 worker 线程无需为配置加锁。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace app {

/// @brief 服务端配置；所有字段在 Application::Start 之后只读。
struct Config {
    std::string host{"127.0.0.1"};                                  ///< 监听地址。
    std::uint16_t port{8081};                                       ///< 监听端口。
    std::size_t worker_threads{4};                                  ///< HttpServer worker 数。
    std::string web_root{"example/app/web_server_full/www"};        ///< 静态文件根目录。
    std::string log_file{"/tmp/go2cpp_web_server_full.log"};        ///< 日志文件。
    std::string log_level{"info"};                                  ///< debug/info/warn/error。
    std::uint32_t sse_interval_ms{1000};                            ///< SSE 推送间隔。

    /// @brief 内置默认配置，不依赖任何文件。
    static Config Default();

    /// @brief 从 INI 读取 [server] / [logging]；文件无效时返回 false 并写错误。
    bool LoadFromFile(const std::string& path, std::string* error);

    /// @brief 单行摘要，启动日志打印，便于复现测试条件。
    std::string Describe() const;
};

}  // namespace app
