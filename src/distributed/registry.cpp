/**
 * @file registry.cpp
 * @brief `ServiceRegistry` / `Registration` 的实现：租约 + 自动心跳的薄封装。
 */

#include "go2cpp/distributed/registry.hpp"

#include "go2cpp/distributed/lease_session.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace go2cpp::distributed {

namespace {

ServiceInstance ToInstance(const LeaseView &view) {
    ServiceInstance instance;
    instance.service = view.name;
    instance.instance = view.instance;
    instance.endpoint = view.endpoint;
    instance.owner = view.owner;
    instance.lease = view.id;
    instance.ttl = view.ttl;
    instance.remaining = view.remaining;
    return instance;
}

} // namespace

struct Registration::Impl {
    ICoordinator *coordinator{nullptr};
    std::string owner;
    LeaseId lease{kInvalidLease};

    std::mutex mutex;
    ServiceInstance snapshot;
    bool stopped{false};

    std::unique_ptr<detail::LeaseSession> session;
};

Registration::Registration() noexcept = default;

Registration::Registration(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

Registration::~Registration() { Stop(); }

Registration::Registration(Registration &&other) noexcept : m_impl(std::move(other.m_impl)) {}

Registration &Registration::operator=(Registration &&other) noexcept {
    if (this != &other) {
        // 旧句柄必须先注销再被覆盖：直接 move 赋值会让旧 Impl 在析构时只停
        // 心跳、不发 ReleaseLease，注册表里留下一个直到 TTL 才消失的幽灵。
        Stop();
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

bool Registration::valid() const {
    if (!m_impl) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->stopped) {
            return false;
        }
    }
    return m_impl->session != nullptr && m_impl->session->alive();
}

bool Registration::lost() const {
    if (m_impl == nullptr || m_impl->session == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->stopped) {
            return false;
        }
    }
    // 手动续约模式没有后台 Lease 线程，本地 deadline 到期也必须报 lost；
    // 主动 Stop() 不算失效（上面已经排除）。
    return m_impl->session->lost() ||
           (m_impl->session->started() && !m_impl->session->alive());
}

LeaseId Registration::lease() const { return m_impl != nullptr ? m_impl->lease : kInvalidLease; }

ServiceInstance Registration::instance() const {
    if (!m_impl) {
        return {};
    }
    ServiceInstance snapshot;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        snapshot = m_impl->snapshot;
    }
    if (m_impl->session != nullptr) {
        snapshot.remaining = m_impl->session->remaining();
    }
    return snapshot;
}

Outcome Registration::Renew() {
    if (!m_impl) {
        return Outcome::Failure(Status::kClosed, "empty registration");
    }
    const Result<LeaseView> result = m_impl->session->RenewNow();
    if (!result.ok()) {
        return Outcome::Failure(result.status, result.message);
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->stopped) {
            return Outcome::Failure(Status::kClosed, "registration stopped");
        }
        m_impl->snapshot.endpoint = result.value.endpoint;
        m_impl->snapshot.ttl = result.value.ttl;
        m_impl->snapshot.remaining = result.value.remaining;
    }
    return Outcome::Success();
}

void Registration::Stop() {
    if (!m_impl) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->stopped) {
            return;
        }
        m_impl->stopped = true;
    }
    // 先停后台续约再注销：相反的顺序会留下一个「刚注销又被心跳续上」的
    // 竞态窗口，恰好是滚动发布时最不想看到的。
    m_impl->session->Stop();
    m_impl->coordinator->ReleaseLease(m_impl->lease, m_impl->owner);
}

void Registration::OnLost(std::function<void()> callback) {
    if (m_impl) {
        m_impl->session->SetLostCallback(std::move(callback));
    }
}

ServiceRegistry::ServiceRegistry(ICoordinator &coordinator, ServiceRegistryOptions options)
    : m_coordinator(coordinator), m_options(std::move(options)) {}

ServiceRegistry::~ServiceRegistry() = default;

Result<Registration> ServiceRegistry::Register(std::string service, std::string instance,
                                               std::string endpoint, Duration ttl) {
    ServiceRegistration registration;
    registration.service = std::move(service);
    registration.instance = std::move(instance);
    registration.endpoint = std::move(endpoint);
    // owner 与实例 id 分开：同一实例可能由不同进程重新注册，owner 用于让
    // 协调者判断「这是原来那个注册者还是抢占者」。
    registration.owner = registration.instance;
    registration.ttl = ttl > Duration::zero() ? ttl : m_options.default_ttl;

    const Result<LeaseView> registered = m_coordinator.RegisterService(registration);
    if (!registered.ok()) {
        return Result<Registration>::Failure(registered.status, registered.message);
    }

    auto impl = std::make_unique<Registration::Impl>();
    impl->coordinator = &m_coordinator;
    impl->owner = registered.value.owner;
    impl->lease = registered.value.id;
    impl->snapshot = ToInstance(registered.value);

    const LeaseId lease = impl->lease;
    const std::string owner = impl->owner;
    ICoordinator *coordinator = &m_coordinator;
    auto session = std::make_unique<detail::LeaseSession>(
        registered.value.ttl, m_options.renew_interval,
        [coordinator, lease, owner] { return coordinator->RenewLease(lease, owner); },
        nullptr);
    session->Start(m_options.auto_renew);
    impl->session = std::move(session);

    return Result<Registration>::Success(Registration(std::move(impl)));
}

std::vector<ServiceInstance> ServiceRegistry::Lookup(std::string_view service) const {
    return m_coordinator.LookupService(service);
}

std::vector<std::string> ServiceRegistry::Services() const { return m_coordinator.ListServices(); }

std::optional<ServiceInstance> ServiceRegistry::Get(std::string_view service,
                                                    std::string_view instance) const {
    for (ServiceInstance &entry : m_coordinator.LookupService(service)) {
        if (entry.instance == instance) {
            return entry;
        }
    }
    return std::nullopt;
}

} // namespace go2cpp::distributed
