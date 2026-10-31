/**
 * @file transport.cpp
 * @brief 进程内配对传输实现（有界队列 + 条件变量）。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/transport.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace go2cpp::app {
namespace {

constexpr std::size_t kDefaultCapacity = 8u * 1024u * 1024u;
constexpr std::size_t kWriteChunk = 64u * 1024u;

}  // namespace

struct MemoryTransport::Shared {
    mutable std::mutex mutex;              ///< 保护队列。
    std::condition_variable cv;            ///< 读写等待。
    std::deque<std::vector<std::uint8_t>> chunks;  ///< 数据块。
    std::size_t head_offset = 0;           ///< 首块已读偏移。
    std::size_t bytes = 0;                 ///< 队列中的总字节数。
    std::size_t capacity = kDefaultCapacity;  ///< 容量。
    bool closed = false;                   ///< 写端是否已关闭。
};

MemoryTransport::MemoryTransport(std::shared_ptr<Shared> inbox,
                                 std::shared_ptr<Shared> outbox, bool /*owner*/)
    : m_inbox(std::move(inbox)), m_outbox(std::move(outbox)) {}

MemoryTransport::~MemoryTransport() { Close(); }

MemoryTransport::Pair MemoryTransport::CreatePair(std::size_t capacity) {
    const std::size_t limit = capacity == 0 ? kDefaultCapacity : capacity;
    auto a_to_b = std::make_shared<Shared>();
    auto b_to_a = std::make_shared<Shared>();
    a_to_b->capacity = limit;
    b_to_a->capacity = limit;
    auto a = std::unique_ptr<MemoryTransport>(
        new MemoryTransport(b_to_a, a_to_b, true));
    auto b = std::unique_ptr<MemoryTransport>(
        new MemoryTransport(a_to_b, b_to_a, true));
    return {std::move(a), std::move(b)};
}

bool MemoryTransport::WriteAll(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return !closed();
    }
    if (data == nullptr) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < size) {
        if (closed()) {
            return false;
        }
        std::unique_lock<std::mutex> lock(m_outbox->mutex);
        if (m_outbox->closed) {
            return false;
        }
        const std::size_t space = m_outbox->capacity - m_outbox->bytes;
        if (space == 0) {
            // 背压：等待读端消费或关闭；100ms 后重试以观察本端关闭。
            m_outbox->cv.wait_for(lock, std::chrono::milliseconds(100));
            continue;
        }
        const std::size_t chunk = std::min({space, size - offset, kWriteChunk});
        m_outbox->chunks.emplace_back(data + offset, data + offset + chunk);
        m_outbox->bytes += chunk;
        offset += chunk;
        m_outbox->cv.notify_all();
    }
    return true;
}

int MemoryTransport::Read(std::uint8_t* data, std::size_t capacity,
                          std::uint32_t timeout_ms) {
    if (data == nullptr && capacity != 0) {
        return -1;
    }
    std::unique_lock<std::mutex> lock(m_inbox->mutex);
    auto has_data = [this] { return m_inbox->bytes > 0; };
    if (!has_data()) {
        if (m_inbox->closed || closed()) {
            return -1;
        }
        if (timeout_ms == 0) {
            return 0;
        }
        m_inbox->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this] {
                                 return m_inbox->bytes > 0 || m_inbox->closed ||
                                        closed();
                             });
        if (!has_data()) {
            return (m_inbox->closed || closed()) ? -1 : 0;
        }
    }
    std::size_t copied = 0;
    while (copied < capacity && m_inbox->bytes > 0) {
        std::vector<std::uint8_t>& chunk = m_inbox->chunks.front();
        const std::size_t available = chunk.size() - m_inbox->head_offset;
        const std::size_t take = std::min(available, capacity - copied);
        std::copy(chunk.begin() + static_cast<std::ptrdiff_t>(m_inbox->head_offset),
                  chunk.begin() + static_cast<std::ptrdiff_t>(m_inbox->head_offset + take),
                  data + copied);
        copied += take;
        m_inbox->head_offset += take;
        m_inbox->bytes -= take;
        if (m_inbox->head_offset == chunk.size()) {
            m_inbox->chunks.pop_front();
            m_inbox->head_offset = 0;
        }
    }
    // 唤醒可能因背压等待的写端。
    m_inbox->cv.notify_all();
    lock.unlock();
    return static_cast<int>(copied);
}

void MemoryTransport::Close() {
    if (m_closed.exchange(true)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_outbox->mutex);
        m_outbox->closed = true;
        m_outbox->cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(m_inbox->mutex);
        m_inbox->cv.notify_all();
    }
}

bool MemoryTransport::closed() const { return m_closed.load(); }

std::string MemoryTransport::Describe() const {
    return "memory-transport";
}

std::size_t MemoryTransport::pending() const {
    std::lock_guard<std::mutex> lock(m_inbox->mutex);
    return m_inbox->bytes;
}

bool WriteChunked(ITransport& transport, const std::uint8_t* data, std::size_t size,
                  std::size_t chunk_size) {
    if (size == 0) {
        return true;
    }
    if (data == nullptr || chunk_size == 0) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t chunk = std::min(chunk_size, size - offset);
        if (!transport.WriteAll(data + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

}  // namespace go2cpp::app
