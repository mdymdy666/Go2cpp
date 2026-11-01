/**
 * @file lock.cpp
 * @brief `DistributedLock` / `LockHandle` 的实现。
 */

#include "go2cpp/distributed/lock.hpp"

#include "go2cpp/distributed/lease_session.hpp"

#include <mutex>
#include <utility>

namespace go2cpp::distributed {

struct LockHandle::Impl {
    ICoordinator *coordinator{nullptr};
    std::string key;
    std::string owner;
    LeaseId lease{kInvalidLease};
    std::uint64_t fencing{0};
    Duration ttl{0};

    std::mutex mutex;
    bool released{false};

    std::unique_ptr<detail::LeaseSession> session;
};

LockHandle::LockHandle() noexcept = default;

LockHandle::LockHandle(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

LockHandle::~LockHandle() { Release(); }

LockHandle::LockHandle(LockHandle &&other) noexcept : m_impl(std::move(other.m_impl)) {}

LockHandle &LockHandle::operator=(LockHandle &&other) noexcept {
    if (this != &other) {
        // 先释放旧锁再接管：否则旧 Impl 析构只会停心跳，锁要等 TTL 才释放。
        Release();
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

bool LockHandle::valid() const {
    if (!m_impl) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->released) {
            return false;
        }
    }
    return m_impl->session != nullptr && m_impl->session->alive();
}

bool LockHandle::lost() const {
    if (!m_impl) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->released) {
            return false;
        }
    }
    if (m_impl->session == nullptr) {
        return true;
    }
    // 手动续约模式下没有后台 Lease 线程，本地 deadline 到期也要报 lost：
    // 否则调用方会一直以为自己还持有锁。
    return m_impl->session->lost() ||
           (m_impl->session->started() && !m_impl->session->alive());
}

LeaseId LockHandle::lease() const { return m_impl != nullptr ? m_impl->lease : kInvalidLease; }

std::uint64_t LockHandle::fencing_token() const {
    return m_impl != nullptr ? m_impl->fencing : 0;
}

const std::string &LockHandle::key() const {
    static const std::string kEmpty;
    return m_impl != nullptr ? m_impl->key : kEmpty;
}

const std::string &LockHandle::owner() const {
    static const std::string kEmpty;
    return m_impl != nullptr ? m_impl->owner : kEmpty;
}

Duration LockHandle::remaining() const {
    return m_impl != nullptr && m_impl->session != nullptr ? m_impl->session->remaining()
                                                          : Duration::zero();
}

LockInfo LockHandle::info() const {
    LockInfo info;
    if (!m_impl) {
        return info;
    }
    info.key = m_impl->key;
    info.owner = m_impl->owner;
    info.lease = m_impl->lease;
    info.fencing = m_impl->fencing;
    info.ttl = m_impl->ttl;
    info.remaining = remaining();
    info.held = valid();
    return info;
}

Outcome LockHandle::Renew() {
    if (!m_impl) {
        return Outcome::Failure(Status::kClosed, "empty lock handle");
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->released) {
            return Outcome::Failure(Status::kClosed, "lock already released");
        }
    }
    const Result<LeaseView> result = m_impl->session->RenewNow();
    if (!result.ok()) {
        return Outcome::Failure(result.status, result.message);
    }
    return Outcome::Success();
}

void LockHandle::Release() {
    if (!m_impl) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->released) {
            return;
        }
        m_impl->released = true;
    }
    m_impl->session->Stop();
    // 已过期/已被清扫时协调者返回 not_found；释放语义是幂等的，直接忽略。
    m_impl->coordinator->ReleaseLease(m_impl->lease, m_impl->owner);
}

void LockHandle::OnLost(std::function<void()> callback) {
    if (m_impl) {
        m_impl->session->SetLostCallback(std::move(callback));
    }
}

DistributedLock::DistributedLock(ICoordinator &coordinator) noexcept
    : m_coordinator(coordinator) {}

Result<LockHandle> DistributedLock::Acquire(std::string key, std::string owner, Duration ttl,
                                            LockOptions options) {
    const Result<LeaseView> granted = m_coordinator.AcquireLock(key, owner, ttl);
    if (!granted.ok()) {
        return Result<LockHandle>::Failure(granted.status, granted.message);
    }

    auto impl = std::make_unique<LockHandle::Impl>();
    impl->coordinator = &m_coordinator;
    impl->key = std::move(key);
    impl->owner = std::move(owner);
    impl->lease = granted.value.id;
    impl->fencing = granted.value.fencing;
    impl->ttl = granted.value.ttl;

    const LeaseId lease = impl->lease;
    const std::string lease_owner = impl->owner;
    ICoordinator *coordinator = &m_coordinator;
    auto session = std::make_unique<detail::LeaseSession>(
        granted.value.ttl, options.renew_interval,
        [coordinator, lease, lease_owner] {
            return coordinator->RenewLease(lease, lease_owner);
        },
        nullptr);
    session->Start(options.auto_renew);
    impl->session = std::move(session);

    return Result<LockHandle>::Success(LockHandle(std::move(impl)));
}

std::optional<LockInfo> DistributedLock::Query(std::string_view key) const {
    return m_coordinator.QueryLock(key);
}

std::vector<LockInfo> DistributedLock::Locks() const { return m_coordinator.ListLocks(); }

} // namespace go2cpp::distributed
