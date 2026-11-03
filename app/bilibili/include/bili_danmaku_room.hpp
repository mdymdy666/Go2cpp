/**
 * @file bili_danmaku_room.hpp
 * @brief 弹幕房间：按 video_id 聚合 WebSocket 连接，异步有界广播。
 *
 * @details 每条连接一个发送线程 + 有界队列（默认 256 条）。`Broadcast()`
 *          只做入队，不碰 socket，因此一个慢连接不会拖住房间内其它连接；
 *          队列满时由 `RoomConnection::Enqueue` 主动踢出该连接。
 *
 * @author bili demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "go2cpp/network/websocket.hpp"

namespace bili {

/// @brief 一条已升级的 WS 连接：发送线程 + 有界发送队列。
class RoomConnection {
   public:
    using Ptr = std::shared_ptr<RoomConnection>;

    /// @brief 构造；`max_queue` 为积压上限，超过即踢出。
    RoomConnection(go2cpp::net::WebSocket::Ptr websocket, std::string peer,
                   std::size_t max_queue = 256);
    ~RoomConnection();
    RoomConnection(const RoomConnection&) = delete;
    RoomConnection& operator=(const RoomConnection&) = delete;

    /// @brief 启动发送线程（Join 房间前调用一次）。
    void Start();

    /// @brief 入队一条完整 JSON 文本；队列满返回 false 并踢出本连接。
    bool Enqueue(std::string message);

    /// @brief 主动关闭（幂等）：置停止位、唤醒发送线程、发 Close 帧。
    void Close();

    /// @brief 连接是否已关闭。
    bool closed() const noexcept { return m_closed.load(std::memory_order_acquire); }

    /// @brief 是否因塞满队列被踢。
    bool slow_consumer() const noexcept {
        return m_slow.load(std::memory_order_acquire);
    }

    /// @brief 唯一连接 id（限流/日志用）。
    std::uint64_t id() const noexcept { return m_id; }

    /// @brief 对端地址（日志用）。
    const std::string& peer() const noexcept { return m_peer; }

    /// @brief 已成功发出的消息条数。
    std::uint64_t sent() const noexcept { return m_sent.load(std::memory_order_relaxed); }

   private:
    void SendLoop();

    go2cpp::net::WebSocket::Ptr m_websocket;
    std::string m_peer;
    std::size_t m_max_queue;
    std::uint64_t m_id;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_queue;
    bool m_stopping{false};

    std::thread m_thread;
    std::atomic<bool> m_closed{false};
    std::atomic<bool> m_slow{false};
    std::atomic<std::uint64_t> m_sent{0};
};

/// @brief video_id → 连接集合；所有方法线程安全。
class RoomHub {
   public:
    /// @brief 加入房间；连接为空返回 false。
    bool Join(std::int64_t video_id, RoomConnection::Ptr connection);

    /// @brief 离开房间（幂等）。
    void Leave(std::int64_t video_id, const RoomConnection::Ptr& connection);

    /// @brief 向房间内所有连接入队；返回入队成功的连接数。
    std::size_t Broadcast(std::int64_t video_id, std::string_view payload);

    /// @brief 房间在线数。
    std::size_t OnlineCount(std::int64_t video_id) const;

    /// @brief 所有房间在线总数。
    std::size_t OnlineTotal() const;

   private:
    mutable std::mutex m_mutex;
    std::map<std::int64_t, std::vector<RoomConnection::Ptr>> m_rooms;
};

}  // namespace bili
