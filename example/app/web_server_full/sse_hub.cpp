/**
 * @file sse_hub.cpp
 * @brief SseHub 的实现：订阅生命周期、Channel 背压与心跳。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "sse_hub.h"

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "logger.h"

namespace app {
namespace {

/// @brief 订阅者队列容量：满了丢事件，避免慢客户端拖垮广播方。
constexpr std::size_t kQueueDepth = 16;
/// @brief 单次 SSE 写事件上限（sse::Stream 的默认值也是 1 MiB）。
constexpr std::size_t kMaxEventBytes = 256 * 1024;

}  // namespace

SseHub::SseHub(go2cpp::bento::HttpServer& server, go2cpp::Scheduler& scheduler,
               std::chrono::milliseconds interval)
    : m_server(server), m_scheduler(scheduler), m_interval(interval),
      m_wake(go2cpp::MakeChannel<int>(1)) {}

SseHub::~SseHub() { Stop(); }

void SseHub::Start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    m_server.AsSse("/events", [this](const go2cpp::http::Request& request,
                                     go2cpp::sse::Stream& stream) { Serve(request, stream); });
    m_ticker = m_scheduler.spawn([this] { TickLoop(); });
    LogInfo("SSE hub 已启动，interval=" + std::to_string(m_interval.count()) + "ms");
}

void SseHub::Stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)m_wake->TrySend(1);
    if (m_ticker) {
        (void)m_ticker->wait_for(std::chrono::seconds(2));
        m_ticker.reset();
    }
    std::vector<Queue> queues;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queues.reserve(m_subscribers.size());
        for (const auto& entry : m_subscribers) {
            queues.push_back(entry.second);
        }
    }
    for (const auto& queue : queues) {
        (void)queue->Close();
    }
    LogInfo("SSE hub 已停止");
}

std::size_t SseHub::subscriber_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_subscribers.size();
}

void SseHub::Publish(const std::string& event, const std::string& data) {
    std::vector<Queue> queues;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queues.reserve(m_subscribers.size());
        for (const auto& entry : m_subscribers) {
            queues.push_back(entry.second);
        }
    }
    std::size_t dropped = 0;
    for (const auto& queue : queues) {
        const auto sent = queue->TrySend(SseEvent{event, data});
        if (!sent.Ok()) {
            ++dropped;
        }
    }
    if (dropped != 0) {
        LogWarn("SSE 广播丢弃 " + std::to_string(dropped) + " 个慢订阅者事件");
    }
}

void SseHub::Serve(const go2cpp::http::Request&, go2cpp::sse::Stream& stream) {
    const auto id = m_next_id.fetch_add(1, std::memory_order_relaxed) + 1;
    auto queue = go2cpp::MakeChannel<SseEvent>(kQueueDepth);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribers.emplace(id, queue);
    }
    LogInfo("SSE 订阅 #" + std::to_string(id) + "，当前 " +
            std::to_string(subscriber_count()) + " 个连接");

    go2cpp::sse::Event welcome;
    welcome.event = "welcome";
    welcome.id = std::to_string(id);
    welcome.data = "{\"message\":\"connected\",\"subscriber\":" + std::to_string(id) + "}";
    (void)stream.send(welcome);

    while (m_running.load(std::memory_order_acquire) && !m_server.IsStopping()) {
        const auto received = queue->RecvFor(m_interval);
        if (received.Ok()) {
            const SseEvent event = received.ValueOrDefault();
            go2cpp::sse::Event outgoing;
            outgoing.event = event.event;
            outgoing.data = event.data;
            if (!stream.send(outgoing).ok()) {
                break;
            }
            continue;
        }
        if (received.status == go2cpp::ChannelStatus::kTimedOut) {
            if (!stream.comment("heartbeat").ok()) {
                break;
            }
            continue;
        }
        break;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribers.erase(id);
    }
    LogInfo("SSE 订阅 #" + std::to_string(id) + " 已断开，剩余 " +
            std::to_string(subscriber_count()) + " 个连接");
}

void SseHub::TickLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        const auto woken = m_wake->RecvFor(m_interval);
        if (!m_running.load(std::memory_order_acquire)) {
            break;
        }
        if (woken.Ok()) {
            continue;
        }
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        Publish("tick", "{\"epoch_ms\":" + std::to_string(now_ms) + "}");
    }
}

}  // namespace app
