/**
 * @file bili_danmaku_room.cpp
 * @brief 房间广播实现：每条连接一个发送线程 + 有界队列，队列满则踢；
 *        广播带单调 seq 并写入环形缓冲，重连时按 last_seq 补发。
 *
 * @details 锁序固定为 hub → conn：`Broadcast()` / `JoinWithResume()` 在 hub
 *          锁内完成「分配 seq → 写缓冲 → 逐连接 Enqueue」，`Enqueue()` 只
 *          入队、不做任何 socket I/O（慢连接由发送线程醒来后自行关闭），
 *          因此持 hub 锁调用它是安全的，也保证补发一定先于后续实时消息。
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
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping || m_closed.load(std::memory_order_acquire)) {
        return false;
    }
    if (m_queue.size() >= m_max_queue) {
        // 慢消费者保护：标记踢出并清空积压。这里**不**做 shutdown/Close，
        // 因为 Enqueue 会被 RoomHub 在持 hub 锁时调用；I/O 由发送线程退出前
        // 统一执行（见 SendLoop 末尾的 Close）。
        m_slow.store(true, std::memory_order_release);
        m_stopping = true;
        m_queue.clear();
        m_cv.notify_all();
        return false;
    }
    m_queue.push_back(std::move(message));
    m_cv.notify_one();
    return true;
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
                break;
            }
            message = std::move(m_queue.front());
            m_queue.pop_front();
        }
        if (!m_websocket) {
            break;
        }
        const auto result = m_websocket->send_text(message);
        if (!result.ok()) {
            break;
        }
        m_sent.fetch_add(1, std::memory_order_relaxed);
    }
    // 统一出口：队列满被踢、主动 Close、发送失败都在这里关 socket。此时本函数
    // 已释放 m_mutex，Close() 的 shutdown 不会与 hub 锁形成嵌套。
    Close();
}

RoomHub::RoomHub(std::size_t journal_capacity)
    : m_journal_capacity(journal_capacity == 0 ? 1 : journal_capacity) {}

go2cpp::app::realtime::SeqJournal& RoomHub::JournalLocked(std::int64_t video_id) {
    const auto it = m_journals.find(video_id);
    if (it != m_journals.end()) {
        return it->second;
    }
    // journal 必须按配置的容量创建；map::operator[] 会走默认容量，故用 emplace。
    return m_journals.emplace(video_id, go2cpp::app::realtime::SeqJournal(m_journal_capacity))
        .first->second;
}

bool RoomHub::Join(std::int64_t video_id, RoomConnection::Ptr connection) {
    return JoinWithResume(video_id, std::move(connection), false, 0, nullptr);
}

bool RoomHub::JoinWithResume(std::int64_t video_id, RoomConnection::Ptr connection,
                             bool resume, std::uint64_t last_seq,
                             ResumeReport* report) {
    if (!connection) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& room = m_rooms[video_id];
    if (std::find(room.begin(), room.end(), connection) != room.end()) {
        return true;  // 幂等：已在该房间，不重复补发。
    }
    auto& journal = JournalLocked(video_id);
    if (report != nullptr) {
        report->resumed = resume;
        report->gap = false;
        report->earliest = journal.earliest();
        report->latest = journal.latest();
        report->replayed = 0;
    }
    if (!resume) {
        room.push_back(std::move(connection));
        return true;
    }

    const auto replay = journal.Since(last_seq);
    if (report != nullptr) {
        report->gap = replay.gap;
        report->earliest = replay.earliest;
        report->latest = replay.latest;
    }
    if (replay.gap) {
        // 窗口已滚出或序号回退：不补发，服务层回 resume_gap 让客户端全量刷新。
        room.push_back(std::move(connection));
        return true;
    }
    // 补发必须先于登记完成：此刻连接不在房间里，并发 Broadcast 拿不到 hub 锁，
    // 因此不可能把实时消息插到补发前面。
    std::size_t replayed = 0;
    for (const auto& message : replay.messages) {
        if (!connection->Enqueue(message)) {
            break;  // 队列满：连接已被标记踢出，后续实时消息也送不到。
        }
        ++replayed;
    }
    if (report != nullptr) {
        report->replayed = replayed;
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
    // journal 有意保留：房间暂空时客户端仍可能带 last_seq 重连，删掉会让序号
    // 回退成 1，旧客户端只能收到 resume_gap。demo 规模下按 video_id 常驻可接受；
    // 生产落地需给 journal 加 TTL/容量上限（见 AI_COLLAB.md 的后续项）。
}

std::size_t RoomHub::Broadcast(std::int64_t video_id, std::string_view payload) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& journal = JournalLocked(video_id);
    std::uint64_t seq = 0;
    const std::string message = journal.Assign(payload, &seq);
    const auto it = m_rooms.find(video_id);
    if (it == m_rooms.end()) {
        // 无人房间也分配序号并留缓冲：下一次有人重连时序号仍然连续。
        return 0;
    }
    std::size_t delivered = 0;
    for (const auto& connection : it->second) {
        if (connection && connection->Enqueue(message)) {
            ++delivered;
        }
    }
    return delivered;
}

std::size_t RoomHub::BroadcastEphemeral(std::int64_t video_id,
                                            std::string_view payload) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_rooms.find(video_id);
    if (it == m_rooms.end()) {
        return 0;
    }
    const std::string message(payload);
    std::size_t delivered = 0;
    for (const auto& connection : it->second) {
        if (connection && connection->Enqueue(message)) {
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

std::uint64_t RoomHub::LatestSeq(std::int64_t video_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_journals.find(video_id);
    return it == m_journals.end() ? 0 : it->second.latest();
}

}  // namespace bili
