#pragma once

/**
 * @file client.h
 * @brief chat_server_full 的测试客户端：PeerHub + stdin 交互 + 自动 PONG。
 * @details 客户端与服务端同仓构建，便于 README 里的命令直接自测：
 *          - 服务端每 app_ping_interval 发 PING，客户端自动回 PONG；
 *          - 收到 TOKEN 时打印 `[token] <token>`，测试脚本据此验证重连；
 *          - stdin EOF 或 /quit 后停止接收并关闭 Hub。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "go2cpp/bento/net/peer_hub.hpp"

namespace chat {

class ChatClient final {
   public:
    struct Options {
        std::string host{"127.0.0.1"};
        std::uint16_t port{9200};
        std::string name{"guest"};
        std::string token;  ///< 非空时发送 HELLO 恢复会话，不发送 JOIN。
        bool auto_join{true};  ///< false 时只连接不 JOIN/HELLO（测试错误路径用）。
    };

    explicit ChatClient(Options options);
    ~ChatClient();

    ChatClient(const ChatClient&) = delete;
    ChatClient& operator=(const ChatClient&) = delete;

    /// @brief 连接、等待握手、发送 JOIN/HELLO；失败写 error。
    bool Start(std::string* error);
    /// @brief 交互循环：stdin 一行一条命令，直到 EOF 或 /quit。
    void Run();

    /// @brief 最近一次从服务端收到的 token（测试/脚本使用）。
    std::string token() const;

   private:
    bool Send(const std::string& text);
    void OnMessage(const std::uint8_t* data, std::size_t bytes);
    void SetToken(const std::string& token);

    go2cpp::bento::PeerHub m_hub;
    Options m_options;
    mutable std::mutex m_mutex;       ///< 保护 m_token（回调线程写、主线程读）。
    std::string m_token;
    std::thread m_pump;               ///< RunOnce 接收线程。
    std::atomic<bool> m_running{true};
};

}  // namespace chat
