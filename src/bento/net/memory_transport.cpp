/**
 * @file memory_transport.cpp
 * @brief `MemoryTransport` 实现：共享 deque + mutex + condition_variable 的进程内管道。
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
#include <deque>
#include <memory>
#include <mutex>

namespace go2cpp::bento::net {
namespace {

using Clock = std::chrono::steady_clock;

/// 负超时按 0 处理。
std::chrono::milliseconds NormalizeTimeout(std::chrono::milliseconds timeout) noexcept {
    return timeout.count() < 0 ? std::chrono::milliseconds(0) : timeout;
}

}  // namespace

/**
 * @brief 两个端点共享的连接状态。
 *
 * @details 每个方向一个 deque：`to_a` 是「B 写给 A」的字节，`to_b` 是
 *          「A 写给 B」的字节。`*_write_closed` 是半关闭标志（只影响对应
 *          方向），`*_closed` 是完全关闭标志。所有字段由 `mutex` 保护。
 */
struct MemoryTransport::SharedState {
    std::mutex mutex;                    ///< 保护下面所有字段。
    std::condition_variable readable;    ///< 有数据到达或关闭时广播。

    std::deque<std::uint8_t> to_a;       ///< B → A 的待读字节。
    std::deque<std::uint8_t> to_b;       ///< A → B 的待读字节。

    bool a_write_closed = false;         ///< A 已半关闭写方向。
    bool b_write_closed = false;         ///< B 已半关闭写方向。
    bool a_closed = false;               ///< A 已完全关闭。
    bool b_closed = false;               ///< B 已完全关闭。

    /// 本端的收件箱（对端写进来的字节）。
    std::deque<std::uint8_t>& Inbox(bool side_a) noexcept {
        return side_a ? to_a : to_b;
    }

    /// 对端的收件箱（本端写出去的字节）。
    std::deque<std::uint8_t>& PeerInbox(bool side_a) noexcept {
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
        std::deque<std::uint8_t>& inbox = state->Inbox(side_a);
        if (!inbox.empty()) {
            const std::size_t count = std::min(capacity, inbox.size());
            for (std::size_t i = 0; i < count; ++i) {
                buffer[i] = inbox.front();
                inbox.pop_front();
            }
            return Result{Code::kOk, 0, count};
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

    std::deque<std::uint8_t>& outbox = state->PeerInbox(side_a);
    outbox.insert(outbox.end(), data, data + size);
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
    std::deque<std::uint8_t>().swap(state->Inbox(side_a));
    state->readable.notify_all();
    return Result{Code::kOk, 0, 0};
}

}  // namespace go2cpp::bento::net
