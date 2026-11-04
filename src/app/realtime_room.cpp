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

#include "go2cpp/logging/log.hpp"

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
                // 业务回调抛异常被当作「连接已坏」并停止推送；吞掉异常但留下证据。
                GO2CPP_LOG_WARN(GO2CPP_LOG_NAME("app"))
                    << "realtime_room: send callback threw, stopping room";
                ok = false;
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

/**
 * @brief 新建一个连接对象加入房间，并在锁外启动其发送线程，返回连接 id。
 * @param room [in] 房间名——借用，函数内拷贝为 key；空串视为合法房间。
 * @param send [in] 发送函数——所有权移交给连接对象（用于投递消息）；不可为空，空则直接返回 0。
 * @return
 * - >0：新连接 id——调用方**必须**保存并在断开时调用 Leave(id)，否则发送线程泄漏；
 * - 0：send 为空，未加入——调用方**不要**把 0 当作有效 id 使用。
 * ## 定位
 * 房间成员的唯一创建点。写错会让发送线程在 hub 锁内启动，导致 Join 串行化甚至死锁；
 * 或漏登记 room_of，使 Leave 找不到连接、留下僵尸发送线程。
 * ## 步骤
 * step1. 拒绝空发送函数，避免创建永远收不到消息的连接；
 * step2. 持 hub 锁分配 id、构造 Conn，并同时写入 rooms[room][id] 与 room_of[id]；
 * step3. 释放锁后再 Start 发送线程，避免线程启动被 hub 锁串行化；
 * step4. 返回 id。
 * ## 并发/失败语义
 * - 调用约束：任意线程可调用；conn->Start 在锁外执行。
 * - 原子性：id 分配、两条索引登记在同一把锁内完成；失败只可能是 send 为空，不会半登记。
 * - 失败语义：不抛异常；创建失败仅出现在 send 为空的分支。
 * - 不变量：返回 id>0 后，rooms 与 room_of 都能查到该连接，且发送线程已启动。
 */
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

/**
 * @brief 幂等地把一个连接移出房间：先从索引摘除，再在锁外 stop/join 其发送线程。
 * @param id [in] Join 返回的连接 id——0 或未知 id 视为已离开，直接返回。
 * @return 无返回值；无论 id 是否存在都保证返回后 hub 中不再有该连接。
 * ## 定位
 * 成员移除的唯一入口（析构走 CloseAll）。写错会在持 hub 锁时 join 发送线程：
 * 发送线程若正在回调中再访问 hub 就会死锁；顺序颠倒还会让广播拿到已停的连接。
 * ## 步骤
 * step1. 锁内查 room_of，查不到直接返回（幂等）；
 * step2. 从 rooms 与 room_of 同时摘除，房间空了顺手删房间；
 * step3. 锁外 Stop()（join 发送线程）并让 shared_ptr 析构。
 * ## 并发/失败语义
 * - 调用约束：可在任意线程调用；Stop 必须在锁外。
 * - 原子性：两条索引的摘除在同一临界区内完成，广播不会看到半移除状态。
 * - 失败语义：不抛异常；重复 Leave 是安全的空操作。
 * - 不变量：返回后 hub 中不存在该 id，且其发送线程已 join 完毕。
 */
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

/**
 * @brief 把一条消息入队到房间内所有连接（非阻塞），并顺带清理已退出的连接。
 * @param room [in] 房间名——借用，查找用的 key；不存在的房间返回 0。
 * @param message [in] 消息内容——借用，锁内拷贝一份供所有连接共享，返回后调用方可复用。
 * @return 成功入队的连接数——**不代表已送达**；慢连接队列满时不计入。
 * ## 定位
 * 广播的唯一入口：所有房间消息都从这里进入各连接的发送队列。
 * 写错会在锁内做 I/O 或 join，造成广播延迟随连接数线性恶化；漏清理则房间表无限增长。
 * ## 步骤
 * step1. 持 hub 锁查房间，不存在直接返回 0；
 * step2. 拷贝一次 payload，逐连接 Enqueue（队列满则丢，不阻塞广播）；
 * step3. 统计入队成功数；
 * step4. 顺手 TakeFinishedLocked 摘除发送线程已退出的连接；
 * step5. 锁外释放 finished，让 Conn 析构时的 join 不占 hub 锁。
 * ## 并发/失败语义
 * - 调用约束：任意线程可调用；与 Join/Leave 通过同一把 hub 锁互斥。
 * - 原子性：一次广播在锁内对房间成员快照式入队，期间不会看到 Join/Leave 的半状态。
 * - 失败语义：不抛异常；单连接队列满只减少返回值，不影响其它连接。
 * - 不变量：返回计数 <= 广播时刻房间内未被停止的连接总数。
 */
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
