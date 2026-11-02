#pragma once

/**
 * @file app_server.h
 * @brief chat_server_full 的组装层：LinkServer 回调 -> 会话/房间/持久化 -> 下行。
 * @details 线程模型：
 *          - LinkServer 的 On* 回调在它自己的接收线程执行；
 *          - 应用层心跳 PingLoop 在 Scheduler Fiber 执行；
 *          - 两者都通过 SessionTable/MessageStore 的内部锁共享状态，
 *            Send/Broadcast 本身线程安全，因此不需要额外的全局锁。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "config.h"
#include "go2cpp/bento/net/link_server.hpp"
#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/scheduler.hpp"
#include "protocol.h"
#include "session.h"
#include "store.h"

namespace chat {

/// @brief 聊天服务端；生命周期由 main 控制。
class ChatServer final {
   public:
    explicit ChatServer(Config config);
    ~ChatServer();

    ChatServer(const ChatServer&) = delete;
    ChatServer& operator=(const ChatServer&) = delete;

    bool Start(std::string* error);
    /// @brief 停心跳 Fiber、停 LinkServer（会触发断开回调）；幂等。
    void Stop();

    std::uint16_t port() const;
    std::size_t online_count() const;

   private:
    void OnConnected(PeerId peer);
    void OnDisconnected(PeerId peer, const char* reason);
    void OnMessage(PeerId peer, std::uint8_t channel, const std::uint8_t* data, std::size_t bytes);
    void Handle(PeerId peer, const Command& command);

    void HandleJoin(PeerId peer, const std::string& arguments);
    void HandleHello(PeerId peer, const std::string& arguments);
    void HandleRoom(PeerId peer, const std::string& arguments);
    void HandleSay(PeerId peer, const std::string& arguments);
    void HandleDirect(PeerId peer, const std::string& arguments);
    void HandleWho(PeerId peer);
    void HandleRooms(PeerId peer);
    void HandleQuit(PeerId peer);

    void SendTo(PeerId peer, const std::string& text);
    void BroadcastRoom(const std::string& room, const std::string& text);
    void Replay(PeerId peer, const std::string& room);
    void Record(const std::string& room, const std::string& from, const std::string& text);
    void PingLoop();

    Config m_config;                                   ///< 启动快照。
    go2cpp::bento::LinkServer m_server;                ///< UDP 多 peer 链路。
    SessionTable m_sessions;                           ///< 在线/离线会话。
    MessageStore m_store;                              ///< 消息持久化。
    go2cpp::Scheduler m_scheduler{2};                  ///< 承载心跳 Fiber。
    go2cpp::ChannelPtr<int> m_wake;                    ///< Stop 唤醒 PingLoop。
    std::shared_ptr<go2cpp::Task> m_pinger;            ///< 心跳 Fiber 句柄。
    std::atomic<bool> m_running{false};                ///< 运行状态。
};

}  // namespace chat
