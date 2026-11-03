/**
 * @file realtime_room.cpp
 * @brief 房间广播实现：每连接一个发送线程 + 有界队列，队列满 / 发送失败则摘除。
 *
 * @details 锁的层次只有两级，且**永远只按 hub → conn 的顺序获取**：
 *
 *          1. `RoomHub::Impl::mutex` 保护房间表与 id 索引；
 *          2. `Conn::m_mutex` 保护单条连接的队列。
 *
 *          `Broadcast()` 在 hub 锁内逐条调用 `Conn::Enqueue()`（O(1) 入队，不碰
 *          业务回调），随后在锁外释放已退出的连接——释放会析构 `Conn` 并 join
 *          发送线程，这一步绝不能持 hub 锁。
 *
 * @author go2cpp contributors
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "go2cpp/app/realtime_room.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace go2cpp::app::realtime {

namespace {

using ConnId = RoomHub::ConnId;
using SendFn = RoomHub::SendFn;

/// @brief 一条连接：有界发送队列 + 一个发送线程。
///
/// 发送线程只捕获 `this`，连接的生命周期由 `RoomHub` 的 `shared_ptr` 掌管；
/// 因此 `Conn` 绝不会在自己的发送线程上析构（析构会 join 该线程）。
class Conn {
   public:
    Conn(ConnId id, std::string room, SendFn send, std::size_t cap)
        : m_id(id),
          m_room(std::move(room)),
          m_send(std::move(send)),
          m_cap(cap == 0 ? 1 : cap) {}

    ~Conn() { Stop(); }

    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    /// @brief 启动发送线程（Join 时调用一次）；已请求停止则不再启动。
    void Start() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping.load(std::memory_order_acquire) || m_thread.joinable()) {
            return;
        }
        m_thread = std::thread([this] { SendLoop(); });
    }

    /// @brief 入队一条消息；队列满则标记踢出并返回 false。
    bool Enqueue(const std::string& message) {
        bool overflow = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping.load(std::memory_order_acquire)) {
                return false;
            }
            if (m_queue.size() >= m_cap) {
                // 慢消费者：清空积压并请求停止，发送线程最多再发完手上一条。
                m_overflow.store(true, std::memory_order_release);
                m_stopping.store(true, std::memory_order_release);
                m_queue.clear();
                overflow = true;
            } else {
                m_queue.push_back(message);
            }
        }
        if (overflow) {
            m_cv.notify_all();
            return false;
        }
        m_cv.notify_one();
        return true;
    }

    /// @brief 请求停止并 join 发送线程（幂等），返回时 `SendFn` 不会再被调用。
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping.store(true, std::memory_order_release);
            m_queue.clear();
        }
        m_cv.notify_all();
        if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) {
            m_thread.join();
        }
    }

    ConnId id() const noexcept { return m_id; }

    /// @brief 是否已请求停止（队列满 / 发送失败 / Leave 之后为 true）。
    bool stopping() const noexcept { return m_stopping.load(std::memory_order_acquire); }

    /// @brief 发送线程是否已返回；为 true 时可以安全摘除并析构。
    bool finished() const noexcept { return m_finished.load(std::memory_order_acquire); }

    /// @brief 是否因积压被踢（诊断用）。
    bool overflow() const noexcept { return m_overflow.load(std::memory_order_acquire); }

    /// @brief 已成功交给 `SendFn` 的消息条数（诊断用）。
    std::uint64_t sent() const noexcept { return m_sent.load(std::memory_order_relaxed); }

   private:
    void SendLoop() {
        for (;;) {
            std::string message;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] {
                    return m_stopping.load(std::memory_order_acquire) || !m_queue.empty();
                });
                if (m_stopping.load(std::memory_order_acquire)) {
                    m_finished.store(true, std::memory_order_release);
                    return;
                }
                message = std::move(m_queue.front());
                m_queue.pop_front();
            }
            bool ok = false;
            try {
                ok = m_send && m_send(message);
            } catch (...) {
                ok = false;  // 业务回调抛异常同样视为连接已坏。
            }
            if (!ok) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stopping.store(true, std::memory_order_release);
                    m_queue.clear();
                }
                m_cv.notify_all();
                m_finished.store(true, std::memory_order_release);
                return;
            }
            m_sent.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const ConnId m_id;
    const std::string m_room;
    const SendFn m_send;
    const std::size_t m_cap;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_queue;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_overflow{false};
    std::atomic<bool> m_finished{false};
    std::atomic<std::uint64_t> m_sent{0};
    std::thread m_thread;
};

}  // namespace

/// @brief `RoomHub` 的实现细节：房间表、id → 房间索引、id 分配器。
struct RoomHub::Impl {
    explicit Impl(RoomConfig cfg) : send_queue_cap(cfg.send_queue_cap == 0 ? 1 : cfg.send_queue_cap) {}

    /// @brief 摘除发送线程已退出的连接；返回值必须在锁外释放（析构会 join）。
    std::vector<std::shared_ptr<Conn>> TakeFinishedLocked() {
        std::vector<std::shared_ptr<Conn>> finished;
        for (auto room_it = rooms.begin(); room_it != rooms.end();) {
            auto& conns = room_it->second;
            for (auto conn_it = conns.begin(); conn_it != conns.end();) {
                if (conn_it->second->finished()) {
                    room_of.erase(conn_it->first);
                    finished.push_back(std::move(conn_it->second));
                    conn_it = conns.erase(conn_it);
                } else {
                    ++conn_it;
                }
            }
            if (conns.empty()) {
                room_it = rooms.erase(room_it);
            } else {
                ++room_it;
            }
        }
        return finished;
    }

    mutable std::mutex mutex;
    std::map<std::string, std::map<ConnId, std::shared_ptr<Conn>>, std::less<>> rooms;
    std::map<ConnId, std::string> room_of;
    ConnId next_id{1};
    std::size_t send_queue_cap{256};
};

RoomHub::RoomHub(RoomConfig cfg) : m_impl(std::make_unique<Impl>(cfg)) {}

RoomHub::~RoomHub() { CloseAll(); }

RoomHub::ConnId RoomHub::Join(std::string_view room, SendFn send) {
    if (!send) {
        return 0;
    }
    std::shared_ptr<Conn> conn;
    ConnId id = 0;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        id = m_impl->next_id++;
        auto room_key = std::string(room);
        conn = std::make_shared<Conn>(id, room_key, std::move(send), m_impl->send_queue_cap);
        m_impl->rooms[room_key][id] = conn;
        m_impl->room_of[id] = std::move(room_key);
    }
    conn->Start();  // 锁外启动，避免线程启动期间被 hub 锁串行化。
    return id;
}

void RoomHub::Leave(ConnId id) {
    std::shared_ptr<Conn> removed;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto index = m_impl->room_of.find(id);
        if (index == m_impl->room_of.end()) {
            return;  // 幂等：不存在（或已离开）直接返回。
        }
        const std::string room = index->second;
        const auto room_it = m_impl->rooms.find(room);
        if (room_it != m_impl->rooms.end()) {
            auto& conns = room_it->second;
            const auto conn_it = conns.find(id);
            if (conn_it != conns.end()) {
                removed = std::move(conn_it->second);
                conns.erase(conn_it);
            }
            if (conns.empty()) {
                m_impl->rooms.erase(room_it);
            }
        }
        m_impl->room_of.erase(index);
    }
    if (removed) {
        removed->Stop();  // 锁外 join：不拖住其它房间的广播/加入。
    }
}

std::size_t RoomHub::Broadcast(std::string_view room, std::string_view message) {
    std::size_t queued = 0;
    std::vector<std::shared_ptr<Conn>> finished;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto room_it = m_impl->rooms.find(room);
        if (room_it != m_impl->rooms.end()) {
            const std::string payload(message);
            for (const auto& entry : room_it->second) {
                if (entry.second->Enqueue(payload)) {
                    ++queued;
                }
            }
        }
        finished = m_impl->TakeFinishedLocked();
    }
    finished.clear();  // 锁外释放：析构会 join 已退出的发送线程。
    return queued;
}

std::size_t RoomHub::Online(std::string_view room) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto room_it = m_impl->rooms.find(room);
    if (room_it == m_impl->rooms.end()) {
        return 0;
    }
    std::size_t online = 0;
    for (const auto& entry : room_it->second) {
        if (!entry.second->stopping()) {
            ++online;
        }
    }
    return online;
}

std::size_t RoomHub::TotalOnline() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    std::size_t online = 0;
    for (const auto& room_entry : m_impl->rooms) {
        for (const auto& conn_entry : room_entry.second) {
            if (!conn_entry.second->stopping()) {
                ++online;
            }
        }
    }
    return online;
}

void RoomHub::CloseAll() {
    std::vector<std::shared_ptr<Conn>> all;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        all.reserve(m_impl->room_of.size());
        for (auto& room_entry : m_impl->rooms) {
            for (auto& conn_entry : room_entry.second) {
                all.push_back(std::move(conn_entry.second));
            }
        }
        m_impl->rooms.clear();
        m_impl->room_of.clear();
    }
    for (const auto& conn : all) {
        if (conn) {
            conn->Stop();
        }
    }
    all.clear();
}

}  // namespace go2cpp::app::realtime
