/**
 * @file memory_transport.cpp
 * @brief `MemoryTransport` 实现：分块队列 + mutex + condition_variable 的进程内管道。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/memory_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace go2cpp::bento::net {
namespace {

using Clock = std::chrono::steady_clock;

/// 队列分块大小：写入按块聚合，读取按块 memcpy，避免逐字节 deque 的开销。
constexpr std::size_t kChunkBytes = 64U * 1024U;

/// 负超时按 0 处理。
std::chrono::milliseconds NormalizeTimeout(std::chrono::milliseconds timeout) noexcept {
    return timeout.count() < 0 ? std::chrono::milliseconds(0) : timeout;
}

/// 一块连续的待读字节；`read_offset` 之前的字节已被消费。
struct Chunk {
    std::vector<std::uint8_t> bytes;
    std::size_t read_offset = 0;

    std::size_t Available() const noexcept {
        return bytes.size() - read_offset;
    }
};

/**
 * @brief 把 `data` 追加到队列尾部。
 *
 * @details 优先复用尾块剩余容量：小消息（几十字节）会合并进同一块，避免每条
 *          消息一次堆分配；尾块写满后新开一块。尾块前缀已读超过一半时先做一次
 *          压缩，避免「读得慢、写得快」时已读字节长期占用内存。
 */
void AppendBytes(std::deque<Chunk>& queue, const std::uint8_t* data, std::size_t size) {
    while (size > 0) {
        if (!queue.empty()) {
            Chunk& tail = queue.back();
            if (tail.read_offset > 0 && tail.read_offset * 2U >= tail.bytes.size()) {
                tail.bytes.erase(
                    tail.bytes.begin(),
                    tail.bytes.begin() + static_cast<std::ptrdiff_t>(tail.read_offset));
                tail.read_offset = 0;
            }
            const std::size_t room = tail.bytes.capacity() - tail.bytes.size();
            if (room > 0) {
                const std::size_t take = std::min(room, size);
                tail.bytes.insert(tail.bytes.end(), data, data + take);
                data += take;
                size -= take;
                continue;
            }
        }
        Chunk chunk;
        chunk.bytes.reserve(std::max(kChunkBytes, size));
        queue.push_back(std::move(chunk));
    }
}

/// 从队列头部取至多 `capacity` 字节；返回实际拷贝量。
std::size_t ReadBytes(std::deque<Chunk>& queue, std::uint8_t* out, std::size_t capacity) {
    std::size_t copied = 0;
    while (copied < capacity && !queue.empty()) {
        Chunk& front = queue.front();
        const std::size_t available = front.Available();
        const std::size_t take = std::min(capacity - copied, available);
        std::memcpy(out + copied, front.bytes.data() + front.read_offset, take);
        front.read_offset += take;
        copied += take;
        if (front.read_offset == front.bytes.size()) {
            queue.pop_front();
        }
    }
    return copied;
}

}  // namespace

/**
 * @brief 两个端点共享的连接状态。
 *
 * @details 每个方向一个分块队列：`to_a` 是「B 写给 A」的字节，`to_b` 是
 *          「A 写给 B」的字节。`*_write_closed` 是半关闭标志（只影响对应
 *          方向），`*_closed` 是完全关闭标志。所有字段由 `mutex` 保护。
 */
struct MemoryTransport::SharedState {
    std::mutex mutex;                    ///< 保护下面所有字段。
    std::condition_variable readable;    ///< 有数据到达或关闭时广播。

    std::deque<Chunk> to_a;              ///< B → A 的待读字节。
    std::deque<Chunk> to_b;              ///< A → B 的待读字节。

    bool a_write_closed = false;         ///< A 已半关闭写方向。
    bool b_write_closed = false;         ///< B 已半关闭写方向。
    bool a_closed = false;               ///< A 已完全关闭。
    bool b_closed = false;               ///< B 已完全关闭。

    /// 本端的收件箱（对端写进来的字节）。
    std::deque<Chunk>& Inbox(bool side_a) noexcept {
        return side_a ? to_a : to_b;
    }

    /// 对端的收件箱（本端写出去的字节）。
    std::deque<Chunk>& PeerInbox(bool side_a) noexcept {
        return side_a ? to_b : to_a;
    }

    bool& SelfWriteClosed(bool side_a) noexcept {
        return side_a ? a_write_closed : b_write_closed;
    }

    bool& PeerWriteClosed(bool side_a) noexcept {
        return side_a ? b_write_closed : a_write_closed;
    }

    bool& SelfClosed(bool side_a) noexcept {
        return side_a ? a_closed : b_closed;
    }

    bool& PeerClosed(bool side_a) noexcept {
        return side_a ? b_closed : a_closed;
    }
};

MemoryTransport::MemoryTransport(std::shared_ptr<SharedState> state, bool side_a) noexcept
    : m_state(std::move(state)), m_side_a(side_a) {}

MemoryTransport::Pair MemoryTransport::CreatePair() {
    auto state = std::make_shared<SharedState>();
    // 构造函数私有，make_unique 无法在 std 的上下文里访问；用 unique_ptr
    // 直接接管 new 的结果，所有权在表达式结束时就已明确，不泄露。
    Pair pair;
    pair.first = std::unique_ptr<MemoryTransport>(new MemoryTransport(state, true));
    pair.second = std::unique_ptr<MemoryTransport>(new MemoryTransport(state, false));
    return pair;
}

MemoryTransport::~MemoryTransport() {
    (void)Close();
}

Result MemoryTransport::Connect(const Endpoint&, std::chrono::milliseconds) {
    const std::shared_ptr<SharedState> state = m_state;
    if (!state) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->SelfClosed(m_side_a)) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    return Result{Code::kOk, 0, 0};
}

Result MemoryTransport::Read(std::uint8_t* buffer, std::size_t capacity,
                             std::chrono::milliseconds timeout) {
    const std::shared_ptr<SharedState> state = m_state;
    if (!state) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    const bool side_a = m_side_a;
    if (state->SelfClosed(side_a)) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    if (capacity == 0) {
        return Result{Code::kOk, 0, 0};
    }
    if (buffer == nullptr) {
        return Result{Code::kError, EINVAL, 0};
    }

    const Clock::time_point deadline = Clock::now() + NormalizeTimeout(timeout);
    for (;;) {
        std::deque<Chunk>& inbox = state->Inbox(side_a);
        if (!inbox.empty()) {
            const std::size_t count = ReadBytes(inbox, buffer, capacity);
            if (count > 0) {
                return Result{Code::kOk, 0, count};
            }
        }

        // 数据先于 EOF：对端半关闭/关闭前写入但尚未读走的字节仍然可读。
        if (state->PeerWriteClosed(side_a) || state->PeerClosed(side_a)) {
            return Result{Code::kClosed, 0, 0};
        }
        if (state->SelfClosed(side_a)) {
            return Result{Code::kClosed, ENOTCONN, 0};
        }
        if (Clock::now() >= deadline) {
            return Result{Code::kTimeout, ETIMEDOUT, 0};
        }

        // wait_until 可能虚假唤醒；循环顶部重新检查数据与关闭标志。
        state->readable.wait_until(lock, deadline);
    }
}

Result MemoryTransport::Write(const std::uint8_t* data, std::size_t size,
                              std::chrono::milliseconds) {
    const std::shared_ptr<SharedState> state = m_state;
    if (!state) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    const bool side_a = m_side_a;
    if (state->SelfClosed(side_a)) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    if (state->SelfWriteClosed(side_a) || state->PeerClosed(side_a)) {
        return Result{Code::kClosed, EPIPE, 0};
    }
    if (size == 0) {
        return Result{Code::kOk, 0, 0};
    }
    if (data == nullptr) {
        return Result{Code::kError, EINVAL, 0};
    }

    AppendBytes(state->PeerInbox(side_a), data, size);
    state->readable.notify_all();
    return Result{Code::kOk, 0, size};
}

Result MemoryTransport::ShutdownWrite() {
    const std::shared_ptr<SharedState> state = m_state;
    if (!state) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    const bool side_a = m_side_a;
    if (state->SelfClosed(side_a)) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    if (state->SelfWriteClosed(side_a)) {
        return Result{Code::kOk, 0, 0};
    }

    state->SelfWriteClosed(side_a) = true;
    state->readable.notify_all();
    return Result{Code::kOk, 0, 0};
}

Result MemoryTransport::Close() {
    const std::shared_ptr<SharedState> state = m_state;
    if (!state) {
        return Result{Code::kOk, 0, 0};
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    const bool side_a = m_side_a;
    if (state->SelfClosed(side_a)) {
        return Result{Code::kOk, 0, 0};
    }

    state->SelfClosed(side_a) = true;
    state->SelfWriteClosed(side_a) = true;
    // 本端不再读，丢弃对端已写但未读的字节，避免 shared_ptr 释放前占用内存。
    // 本端已写给对端的数据不清空：对端仍应能读到 close 之前的完整字节流。
    std::deque<Chunk>().swap(state->Inbox(side_a));
    state->readable.notify_all();
    return Result{Code::kOk, 0, 0};
}

}  // namespace go2cpp::bento::net
