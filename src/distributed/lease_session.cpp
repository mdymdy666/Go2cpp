/**
 * @file lease_session.cpp
 * @brief `LeaseSession` 的实现：Heartbeat 周期续约 + 本地 Lease 兜底。
 */

#include "go2cpp/distributed/lease_session.hpp"

#include "go2cpp/distributed/metrics.hpp"

#include "go2cpp/relay/lease.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace go2cpp::distributed::detail {

struct LeaseSession::State {
    std::mutex mutex;
    Duration ttl{0};
    Duration interval{0};
    TimePoint deadline{};
    bool started{false};
    bool stopped{false};
    bool lost{false};
    std::string lost_reason;
    RenewFn renew;
    LostFn on_lost;
    std::unique_ptr<relay::Heartbeat> heartbeat;
    std::unique_ptr<relay::Lease> lease;
};

LeaseSession::LeaseSession(Duration ttl, Duration renew_interval, RenewFn renew, LostFn on_lost)
    : m_state(std::make_shared<State>()) {
    m_state->ttl = ttl > Duration::zero() ? ttl : Duration(1);
    // 默认 ttl/3：三次续约机会里成功一次就能保活；再短会放大调度抖动，
    // 再长（例如 ttl/2）则一次丢包就可能过期。
    m_state->interval = renew_interval > Duration::zero()
                            ? renew_interval
                            : std::max<Duration>(m_state->ttl / 3, Duration(1));
    m_state->renew = std::move(renew);
    m_state->on_lost = std::move(on_lost);
}

LeaseSession::~LeaseSession() { Stop(); }

void LeaseSession::Start(bool background) {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->started || m_state->stopped || m_state->lost) {
        return;
    }
    m_state->started = true;
    m_state->deadline = Clock::now() + m_state->ttl;
    if (!background) {
        // 手动模式：不建任何线程。句柄的 local deadline 仍然生效（调用方
        // 必须自己续约），RenewNow/Stop 走的路径与后台模式完全一致。
        return;
    }

    // 只捕获 weak_ptr：回调可能晚于 LeaseSession 析构执行（relay 的计时
    // 线程被调度延迟），强引用会把整个 State 拖到回调之后才释放。
    const std::weak_ptr<State> weak = m_state;
    m_state->heartbeat = std::make_unique<relay::Heartbeat>(m_state->interval);
    m_state->heartbeat->Start(
        nullptr,
        [weak] {
            const auto state = weak.lock();
            return state != nullptr && RenewFromHeartbeat(state);
        },
        [weak] {
            if (const auto state = weak.lock()) {
                MarkLostState(state, "heartbeat reported failure");
            }
        });
    // 本地兜底：一个完整 TTL 内没有成功续约就判定失效。它比协调者的判定
    // 只会更保守（本地时钟在最后一次成功续约后才开始计时）。
    m_state->lease = std::make_unique<relay::Lease>(
        m_state->ttl, [weak] {
            if (const auto state = weak.lock()) {
                MarkLostState(state, "local lease expired without renewal");
            }
        });
}

void LeaseSession::Stop() {
    std::unique_ptr<relay::Heartbeat> heartbeat;
    std::unique_ptr<relay::Lease> lease;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->stopped) {
            return;
        }
        m_state->stopped = true;
        // 先把两个后台对象搬出临界区，再 join：join 期间心跳线程可能正
        // 尝试拿 m_state->mutex，持锁 join 会直接死锁。
        heartbeat = std::move(m_state->heartbeat);
        lease = std::move(m_state->lease);
    }
    if (heartbeat != nullptr) {
        heartbeat->Stop();
    }
    if (lease != nullptr) {
        lease->Stop();
    }
}

bool LeaseSession::started() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->started;
}

bool LeaseSession::lost() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->lost;
}

bool LeaseSession::stopped() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->stopped;
}

bool LeaseSession::alive() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->started && !m_state->stopped && !m_state->lost &&
           Clock::now() < m_state->deadline;
}

Duration LeaseSession::ttl() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->ttl;
}

Duration LeaseSession::remaining() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (!m_state->started || m_state->stopped || m_state->lost) {
        return Duration::zero();
    }
    return RemainingUntil(m_state->deadline, Clock::now());
}

Result<LeaseView> LeaseSession::RenewNow() {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->stopped || m_state->lost) {
            return Result<LeaseView>::Failure(Status::kClosed, "lease session is not active");
        }
    }
    // 续约函数可能走网络（远程协调者），不能持内部锁调用：否则 Stop()
    // 的 join 会等一次 RTT，最长可到调用方的 I/O 超时。
    Result<LeaseView> result = m_state->renew();
    if (!result.ok()) {
        MarkLostState(m_state, result.message.empty() ? StatusName(result.status)
                                                      : result.message);
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->stopped) {
            return Result<LeaseView>::Failure(Status::kClosed, "lease session stopped");
        }
        if (!m_state->lost) {
            metrics::LeaseRenew().Inc();
            m_state->deadline = Clock::now() + m_state->ttl;
            if (m_state->lease != nullptr) {
                m_state->lease->Renew();
            }
        }
    }
    return result;
}

void LeaseSession::MarkLost(std::string reason) { MarkLostState(m_state, std::move(reason)); }

void LeaseSession::SetLostCallback(LostFn callback) {
    LostFn immediate;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->on_lost = std::move(callback);
        // 已经失效但还没 Stop：调用方刚拿到句柄就发现丢了，必须让它知道
        // （否则要等下一次轮询，可能已经是两个 TTL 之后）。
        if (m_state->lost && !m_state->stopped) {
            immediate = m_state->on_lost;
        }
    }
    if (immediate) {
        immediate();
    }
}

bool LeaseSession::RenewFromHeartbeat(const std::shared_ptr<State> &state) {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->started || state->stopped || state->lost || !state->renew) {
            return false;
        }
    }
    Result<LeaseView> result = state->renew();
    if (!result.ok()) {
        MarkLostState(state, result.message.empty() ? StatusName(result.status)
                                                    : result.message);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopped || state->lost) {
            return false;
        }
        metrics::LeaseRenew().Inc();
        state->deadline = Clock::now() + state->ttl;
        if (state->lease != nullptr) {
            state->lease->Renew();
        }
    }
    return true;
}

void LeaseSession::MarkLostState(const std::shared_ptr<State> &state, std::string reason) {
    LostFn callback;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->lost || state->stopped) {
            return;
        }
        state->lost = true;
        state->lost_reason = std::move(reason);
        metrics::LeaseLost().Inc();
        callback = state->on_lost;
    }
    if (callback) {
        callback();
    }
}

} // namespace go2cpp::distributed::detail
