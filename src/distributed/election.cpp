/**
 * @file election.cpp
 * @brief `LeaderElection` / `Candidate` 的实现。
 */

#include "go2cpp/distributed/election.hpp"

#include "go2cpp/distributed/lease_session.hpp"

#include <mutex>
#include <utility>

namespace go2cpp::distributed {

struct Candidate::Impl {
    ICoordinator *coordinator{nullptr};
    std::string election;
    std::string node;
    LeaseId lease{kInvalidLease};

    std::mutex mutex;
    bool left{false};
    std::string leader_id;
    std::uint64_t epoch{0};
    Duration leader_remaining{0};
    std::function<void(const std::optional<LeaderInfo> &)> on_leader_change;

    std::unique_ptr<detail::LeaseSession> session;

    /// 更新缓存；leader 节点或 epoch 变化时在锁外调用一次回调。
    /// 作为 `Impl` 的静态成员而不是自由函数：`Candidate::Impl` 是私有嵌套
    /// 类型，命名空间作用域的自由函数无法命名它。
    static void UpdateCache(Impl *impl, const std::string &leader, std::uint64_t epoch,
                           Duration remaining);
};

void Candidate::Impl::UpdateCache(Impl *impl, const std::string &leader, std::uint64_t epoch,
                                  Duration remaining) {
    if (impl == nullptr) {
        return;
    }
    std::optional<LeaderInfo> change;
    std::function<void(const std::optional<LeaderInfo> &)> callback;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->left) {
            return;
        }
        const bool changed = leader != impl->leader_id || epoch != impl->epoch;
        impl->leader_id = leader;
        impl->epoch = epoch;
        impl->leader_remaining = remaining;
        if (changed) {
            if (!leader.empty()) {
                change = LeaderInfo{leader, epoch, remaining};
            }
            callback = impl->on_leader_change;
        }
    }
    if (callback) {
        callback(change);
    }
}

Candidate::Candidate() noexcept = default;

Candidate::Candidate(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

Candidate::~Candidate() { Leave(); }

Candidate::Candidate(Candidate &&other) noexcept : m_impl(std::move(other.m_impl)) {}

Candidate &Candidate::operator=(Candidate &&other) noexcept {
    if (this != &other) {
        Leave();
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

bool Candidate::valid() const {
    if (!m_impl) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->left) {
            return false;
        }
    }
    return m_impl->session != nullptr && m_impl->session->alive();
}

bool Candidate::lost() const {
    if (!m_impl) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->left) {
            return false;
        }
    }
    if (m_impl->session == nullptr) {
        return true;
    }
    return m_impl->session->lost() ||
           (m_impl->session->started() && !m_impl->session->alive());
}

bool Candidate::is_leader() const {
    if (!m_impl) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return !m_impl->left && !m_impl->leader_id.empty() && m_impl->leader_id == m_impl->node;
}

std::string Candidate::leader_id() const {
    if (!m_impl) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->leader_id;
}

std::uint64_t Candidate::epoch() const {
    if (!m_impl) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->epoch;
}

const std::string &Candidate::node() const {
    static const std::string kEmpty;
    return m_impl != nullptr ? m_impl->node : kEmpty;
}

const std::string &Candidate::election() const {
    static const std::string kEmpty;
    return m_impl != nullptr ? m_impl->election : kEmpty;
}

LeaseId Candidate::lease() const { return m_impl != nullptr ? m_impl->lease : kInvalidLease; }

Duration Candidate::remaining() const {
    return m_impl != nullptr && m_impl->session != nullptr ? m_impl->session->remaining()
                                                          : Duration::zero();
}

Outcome Candidate::Refresh() {
    if (!m_impl) {
        return Outcome::Failure(Status::kClosed, "empty candidate");
    }
    const ElectionState state = m_impl->coordinator->QueryElection(m_impl->election);
    Duration remaining = Duration::zero();
    for (const CandidateInfo &candidate : state.candidates) {
        if (candidate.node == state.leader) {
            remaining = candidate.remaining;
            break;
        }
    }
    Candidate::Impl::UpdateCache(m_impl.get(), state.leader, state.epoch, remaining);
    return Outcome::Success();
}

Outcome Candidate::Renew() {
    if (!m_impl) {
        return Outcome::Failure(Status::kClosed, "empty candidate");
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->left) {
            return Outcome::Failure(Status::kClosed, "candidate already left");
        }
    }
    const Result<LeaseView> result = m_impl->session->RenewNow();
    if (!result.ok()) {
        return Outcome::Failure(result.status, result.message);
    }
    Candidate::Impl::UpdateCache(m_impl.get(), result.value.leader_id, result.value.epoch,
                      result.value.remaining);
    return Outcome::Success();
}

void Candidate::Leave() {
    if (!m_impl) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->left) {
            return;
        }
        m_impl->left = true;
    }
    m_impl->session->Stop();
    m_impl->coordinator->ReleaseLease(m_impl->lease, m_impl->node);
}

void Candidate::OnLeaderChange(
    std::function<void(const std::optional<LeaderInfo> &)> callback) {
    if (m_impl) {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->on_leader_change = std::move(callback);
    }
}

LeaderElection::LeaderElection(ICoordinator &coordinator) noexcept : m_coordinator(coordinator) {}

Result<Candidate> LeaderElection::Join(std::string election, std::string node, Duration ttl,
                                       CandidateOptions options) {
    const Result<LeaseView> joined = m_coordinator.JoinElection(election, node, ttl);
    if (!joined.ok()) {
        return Result<Candidate>::Failure(joined.status, joined.message);
    }

    auto impl = std::make_unique<Candidate::Impl>();
    impl->coordinator = &m_coordinator;
    impl->election = std::move(election);
    impl->node = std::move(node);
    impl->lease = joined.value.id;
    impl->leader_id = joined.value.leader_id;
    impl->epoch = joined.value.epoch;
    impl->leader_remaining = joined.value.remaining;

    Candidate::Impl *impl_ptr = impl.get();
    const LeaseId lease = impl->lease;
    const std::string candidate = impl->node;
    ICoordinator *coordinator = &m_coordinator;
    auto session = std::make_unique<detail::LeaseSession>(
        joined.value.ttl, options.renew_interval,
        [coordinator, lease, candidate, impl_ptr] {
            Result<LeaseView> renewed = coordinator->RenewLease(lease, candidate);
            if (renewed.ok()) {
                Candidate::Impl::UpdateCache(impl_ptr, renewed.value.leader_id, renewed.value.epoch,
                                  renewed.value.remaining);
            }
            return renewed;
        },
        nullptr);
    session->Start(options.auto_renew);
    impl->session = std::move(session);

    return Result<Candidate>::Success(Candidate(std::move(impl)));
}

ElectionState LeaderElection::State(std::string_view election) const {
    return m_coordinator.QueryElection(election);
}

std::optional<LeaderInfo> LeaderElection::Leader(std::string_view election) const {
    const ElectionState state = m_coordinator.QueryElection(election);
    if (state.leader.empty()) {
        return std::nullopt;
    }
    Duration remaining = Duration::zero();
    for (const CandidateInfo &candidate : state.candidates) {
        if (candidate.node == state.leader) {
            remaining = candidate.remaining;
            break;
        }
    }
    return LeaderInfo{state.leader, state.epoch, remaining};
}

std::vector<std::string> LeaderElection::Elections() const {
    return m_coordinator.ListElections();
}

} // namespace go2cpp::distributed
