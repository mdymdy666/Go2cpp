#pragma once

/**
 * @file config.h
 * @brief chat_server_full 的配置模型与 INI 加载入口。
 * @details 配置在启动时快照，运行期只读；配置项覆盖链路参数、应用层心跳、
 *          重连宽限、消息上限、持久化文件与日志。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace chat {

struct Config {
    std::string host{"127.0.0.1"};                                  ///< 监听地址。
    std::uint16_t port{9200};                                       ///< 监听端口。
    std::size_t max_peers{64};                                      ///< LinkServer 连接上限。
    std::uint32_t heartbeat_ms{500};                                ///< 链路心跳间隔。
    std::uint32_t peer_timeout_ms{10000};                           ///< 链路 peer 超时。
    std::uint32_t app_ping_interval_ms{5000};                       ///< 应用层 PING 间隔。
    std::uint32_t app_session_timeout_ms{15000};                    ///< 应用层会话超时。
    std::uint32_t reconnect_grace_ms{60000};                        ///< 断线后 token 保留时长。
    std::size_t max_message_bytes{1024};                            ///< 单条消息上限。
    std::size_t replay_lines{20};                                   ///< 加入房间时回放条数。
    std::string history_file{"/tmp/go2cpp_chat_history.log"};       ///< 消息持久化文件。
    std::string log_file{"/tmp/go2cpp_chat_server_full.log"};       ///< 日志文件。
    std::string log_level{"info"};                                  ///< debug/info/warn/error。

    static Config Default();
    /// @brief 从 INI 读取 [chat]/[logging]；任一值非法则不提交任何字段。
    bool LoadFromFile(const std::string& path, std::string* error);
    std::string Describe() const;
};

}  // namespace chat
