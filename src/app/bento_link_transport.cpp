/**
 * @file bento_link_transport.cpp
 * @brief Bento Link STREAM 通道 → ITransport 适配器实现。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/bento_link_transport.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace go2cpp::app {

BentoLinkTransport::BentoLinkTransport(go2cpp::bento::Link* link, std::uint8_t channel,
                                       std::size_t max_message_bytes,
                                       std::uint32_t send_timeout_ms)
    : m_link(link),
      m_channel(channel),
      m_max_message(max_message_bytes == 0 ? 32 * 1024 : max_message_bytes),
      m_send_timeout_ms(send_timeout_ms) {}

BentoLinkTransport::~BentoLinkTransport() { Close(); }

bool BentoLinkTransport::Install() {
    if (m_link == nullptr || m_installed) {
        return false;
    }
    m_installed = true;
    m_previous = m_link->on_stream;
    m_link->on_stream = [this](std::uint8_t channel, const std::uint8_t* data,
                               std::size_t size) {
        if (channel == m_channel) {
            OnLinkData(data, size);
        }
        if (m_previous) {
            m_previous(channel, data, size);
        }
    };
    return true;
}

void BentoLinkTransport::OnLinkData(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return;
        }
        m_rx.insert(m_rx.end(), data, data + size);
    }
    m_cv.notify_all();
}

bool BentoLinkTransport::WriteAll(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return !closed();
    }
    if (data == nullptr || m_link == nullptr || closed()) {
        return false;
    }
    const std::uint64_t deadline =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count()) +
        m_send_timeout_ms;
    std::size_t offset = 0;
    while (offset < size) {
        if (closed()) {
            return false;
        }
        const std::size_t chunk = std::min(m_max_message, size - offset);
        const go2cpp::bento::SendStatus status =
            m_link->TrySendStream(m_channel, data + offset, chunk);
        if (status == go2cpp::bento::SendStatus::kOk) {
            offset += chunk;
            continue;
        }
        if (status == go2cpp::bento::SendStatus::kWouldBlock) {
            const std::uint64_t now = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            if (now >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return false;
    }
    return true;
}

int BentoLinkTransport::Read(std::uint8_t* data, std::size_t capacity,
                             std::uint32_t timeout_ms) {
    if (data == nullptr && capacity != 0) {
        return -1;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_rx.empty()) {
        if (m_closed) {
            return -1;
        }
        if (timeout_ms == 0) {
            return 0;
        }
        m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return !m_rx.empty() || m_closed; });
        if (m_rx.empty()) {
            return m_closed ? -1 : 0;
        }
    }
    const std::size_t take = std::min(capacity, m_rx.size());
    std::copy(m_rx.begin(), m_rx.begin() + static_cast<std::ptrdiff_t>(take), data);
    m_rx.erase(m_rx.begin(), m_rx.begin() + static_cast<std::ptrdiff_t>(take));
    return static_cast<int>(take);
}

void BentoLinkTransport::Close() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }
    m_cv.notify_all();
}

bool BentoLinkTransport::closed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

std::size_t BentoLinkTransport::pending() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rx.size();
}

std::string BentoLinkTransport::Describe() const {
    return "bento-link-chan" + std::to_string(m_channel);
}

}  // namespace go2cpp::app
