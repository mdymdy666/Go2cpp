/**
 * @file signaling.cpp
 * @brief 进程内信令通道实现。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/signaling.hpp"

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace go2cpp::p2p {
namespace detail {

class SignalingMailbox {
public:
    void Send(int sender_side, std::string payload) {
        const int receiver_side = sender_side == 0 ? 1 : 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queues[static_cast<std::size_t>(receiver_side)].push_back(std::move(payload));
        }
        m_condition.notify_all();
    }

    int Recv(int side, std::string* payload, std::chrono::milliseconds timeout) {
        if (payload == nullptr) {
            return -1;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool ready = m_condition.wait_for(lock, timeout, [this, side]() {
            return !m_queues[static_cast<std::size_t>(side)].empty();
        });
        if (!ready) {
            return 0;
        }
        *payload = std::move(m_queues[static_cast<std::size_t>(side)].front());
        m_queues[static_cast<std::size_t>(side)].pop_front();
        return static_cast<int>(payload->size());
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::array<std::deque<std::string>, 2> m_queues;
};

}  // namespace detail

LoopbackSignaling::LoopbackSignaling(std::shared_ptr<detail::SignalingMailbox> mailbox, int side)
    : m_mailbox(std::move(mailbox)), m_side(side) {}

std::pair<std::unique_ptr<LoopbackSignaling>, std::unique_ptr<LoopbackSignaling>>
LoopbackSignaling::CreatePair() {
    auto mailbox = std::make_shared<detail::SignalingMailbox>();
    auto side_a = std::make_unique<LoopbackSignaling>(mailbox, 0);
    auto side_b = std::make_unique<LoopbackSignaling>(mailbox, 1);
    return {std::move(side_a), std::move(side_b)};
}

bool LoopbackSignaling::Send(const std::string& payload) {
    if (!m_mailbox) {
        return false;
    }
    m_mailbox->Send(m_side, payload);
    return true;
}

int LoopbackSignaling::Recv(std::string* payload, std::chrono::milliseconds timeout) {
    if (!m_mailbox) {
        return -1;
    }
    return m_mailbox->Recv(m_side, payload, timeout);
}

}  // namespace go2cpp::p2p
