/**
 * @file bili_danmaku_room.cpp
 * @brief 房间广播实现：每条连接一个发送线程 + 有界队列，队列满则踢。
 *
 * @author bili demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "bili_danmaku_room.hpp"

#include <algorithm>
#include <utility>

namespace bili {

namespace {
/// @brief 连接 id 分配器；进程内单调递增。
std::atomic<std::uint64_t> g_next_connection_id{1};
}  // namespace

RoomConnection::RoomConnection(go2cpp::net::WebSocket::Ptr websocket, std::string peer,
                               std::size_t max_queue)
    : m_websocket(std::move(websocket)),
      m_peer(std::move(peer)),
      m_max_queue(max_queue == 0 ? 1 : max_queue),
      m_id(g_next_connection_id.fetch_add(1, std::memory_order_relaxed)) {}

RoomConnection::~RoomConnection() {
    Close();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void RoomConnection::Start() {
    if (m_thread.joinable()) {
        return;
    }
    m_thread = std::thread([this] { SendLoop(); });
}

bool RoomConnection::Enqueue(std::string message) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping || m_closed.load(std::memory_order_acquire)) {
            return false;
        }
        if (m_queue.size() >= m_max_queue) {
            m_slow.store(true, std::memory_order_release);
            // 释放锁后再 Close，避免在持锁路径里调用 socket/shutdown。
        } else {
            m_queue.push_back(std::move(message));
            m_cv.notify_one();
            return true;
        }
    }
    Close();
    return false;
}

void RoomConnection::Close() {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        m_queue.clear();
    }
    m_cv.notify_all();
    if (m_websocket) {
        // shutdown 唤醒可能阻塞在 read/write 上的对端；Close 帧随后尽力发出。
        if (auto socket = m_websocket->socket()) {
            socket->shutdown(SHUT_RDWR);
        }
        m_websocket->close(1000, "server closed");
    }
}

void RoomConnection::SendLoop() {
    for (;;) {
        std::string message;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_stopping) {
                return;
            }
            message = std::move(m_queue.front());
            m_queue.pop_front();
        }
        if (!m_websocket) {
            return;
        }
        const auto result = m_websocket->send_text(message);
        if (!result.ok()) {
            m_closed.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stopping = true;
                m_queue.clear();
            }
            m_cv.notify_all();
            return;
        }
        m_sent.fetch_add(1, std::memory_order_relaxed);
    }
}

bool RoomHub::Join(std::int64_t video_id, RoomConnection::Ptr connection) {
    if (!connection) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& room = m_rooms[video_id];
    if (std::find(room.begin(), room.end(), connection) != room.end()) {
        return true;
    }
    room.push_back(std::move(connection));
    return true;
}

void RoomHub::Leave(std::int64_t video_id, const RoomConnection::Ptr& connection) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_rooms.find(video_id);
    if (it == m_rooms.end()) {
        return;
    }
    auto& room = it->second;
    room.erase(std::remove(room.begin(), room.end(), connection), room.end());
    if (room.empty()) {
        m_rooms.erase(it);
    }
}

std::size_t RoomHub::Broadcast(std::int64_t video_id, std::string_view payload) {
    std::vector<RoomConnection::Ptr> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_rooms.find(video_id);
        if (it == m_rooms.end()) {
            return 0;
        }
        targets = it->second;
    }
    std::size_t delivered = 0;
    for (const auto& connection : targets) {
        if (connection && connection->Enqueue(std::string(payload))) {
            ++delivered;
        }
    }
    return delivered;
}

std::size_t RoomHub::OnlineCount(std::int64_t video_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_rooms.find(video_id);
    return it == m_rooms.end() ? 0 : it->second.size();
}

std::size_t RoomHub::OnlineTotal() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t total = 0;
    for (const auto& entry : m_rooms) {
        total += entry.second.size();
    }
    return total;
}

}  // namespace bili
