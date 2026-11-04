/**
 * @file coordinator.cpp
 * @brief `Coordinator` 的实现：单锁保护的租约状态机 + 后台过期清扫。
 *
 * 设计要点（为什么这样写）：
 *
 * 1. **所有状态变更都在一把互斥锁内完成**。服务发现、锁、选举的读写频率
 *    在毫秒~秒级，临界区只做哈希表操作，单锁足够；换来的是「授予唯一」
 *    这一最容易出错的不变量可以被逐行检查。
 * 2. **过期记录是惰性的**。查询路径自己按 `now` 过滤，清扫只负责回收内存
 *    与推进选举任期。因此即使清扫线程被调度延迟，外部也不可能拿到过期
 *    资源；反过来，测试可以关掉清扫线程，用同步 `SweepExpired()` 精确控制。
 * 3. **fencing token 与选举任期永不清零**。锁 key 释放、election 无候选者
 *    之后，历史计数仍然保留在 `m_fencing` / `ElectionRecord::epoch` 里。
 *    这是「不丢锁/不双主」的最后一道防线：任何新持有者拿到的 token 都严格
 *    大于前任，下游资源可以据此拒绝过期持有者的写入。
 */

#include "go2cpp/distributed/coordinator.hpp"

#include "go2cpp/distributed/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace go2cpp::distributed {

const char *StatusName(Status status) noexcept {
    switch (status) {
    case Status::kOk:
        return "ok";
    case Status::kInvalidArgument:
        return "invalid_argument";
    case Status::kNotFound:
        return "not_found";
    case Status::kConflict:
        return "conflict";
    case Status::kExpired:
        return "expired";
    case Status::kNotOwner:
        return "not_owner";
    case Status::kUnavailable:
        return "unavailable";
    case Status::kProtocolError:
        return "protocol_error";
    case Status::kClosed:
        return "closed";
    }
    return "unknown";
}

Coordinator::Coordinator(CoordinatorOptions options) : m_options(std::move(options)) {
    // P1: 先恢复快照再启动清扫线程——避免清扫线程在恢复过程中读半初始化状态。
    // 加载失败必须 fail-stop：healthy() == false 且所有写操作返回 kUnavailable，
    // 绝不能静默退化成「fencing 从 0 开始」。
    if (!m_options.snapshot_path.empty()) {
        const Outcome restored = Restore();
        if (!restored.ok()) {
            m_degraded.store(true, std::memory_order_release);
            m_load_error = restored.message;
        }
    }
    // 合并落盘窗口从启动时刻起算：首个窗口内即使有清扫也不额外写盘。
    m_last_sweep_persist = Clock::now();
    if (m_options.start_sweeper) {
        Start();
    }
}

Coordinator::~Coordinator() {
    Stop();
    // 析构时尽力落盘一次：调用方忘了显式 Snapshot() 也不会丢掉最后的 fencing。
    // 降级状态不写（不能覆盖那份损坏的快照，保留现场）；异常不得逸出析构。
    if (!m_options.snapshot_path.empty() && !m_degraded) {
        try {
            (void)Snapshot();
        } catch (...) {
            // 快照失败已在 persist_error() 中记录，析构不抛。
        }
    }
}

void Coordinator::Start() {
    std::lock_guard<std::mutex> lock(m_sweep_mutex);
    if (m_sweeper_started) {
        return;
    }
    m_stop_sweeper = false;
    m_sweeper_started = true;
    m_sweeper = std::thread([this] { SweepLoop(); });
}

void Coordinator::Stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(m_sweep_mutex);
        if (!m_sweeper_started) {
            return;
        }
        m_stop_sweeper = true;
        worker = std::move(m_sweeper);
        m_sweeper_started = false;
    }
    // 先置位再唤醒，避免清扫线程在谓词检查与 wait 之间丢通知。
    m_sweep_cv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

bool Coordinator::running() const {
    std::lock_guard<std::mutex> lock(m_sweep_mutex);
    return m_sweeper_started;
}

void Coordinator::SweepLoop() {
    std::unique_lock<std::mutex> lock(m_sweep_mutex);
    const Duration interval =
        m_options.sweep_interval > Duration::zero() ? m_options.sweep_interval : Duration(1);
    while (!m_stop_sweeper) {
        m_sweep_cv.wait_for(lock, interval, [this] { return m_stop_sweeper; });
        if (m_stop_sweeper) {
            break;
        }
        lock.unlock();
        SweepExpired();
        lock.lock();
    }
}

bool Coordinator::ValidTtl(Duration ttl) const noexcept {
    return ttl >= m_options.min_ttl && ttl <= m_options.max_ttl;
}

std::uint64_t Coordinator::EpochSumLocked() const noexcept {
    std::uint64_t sum = 0;
    for (const auto &[name, election] : m_elections) {
        (void)name;
        sum += election.epoch;
    }
    return sum;
}

std::size_t Coordinator::SweepExpired() {
    std::unique_lock<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();
    std::vector<LeaseId> expired;
    expired.reserve(m_leases.size() / 8 + 1);
    for (const auto &[id, record] : m_leases) {
        if (record.deadline <= now) {
            expired.push_back(id);
        }
    }
    const std::uint64_t epoch_before = EpochSumLocked();
    for (const LeaseId id : expired) {
        EraseLeaseLocked(id, now);
    }
    // 删除候选者可能让 leader 换代（epoch 递增）：任期是下游拒绝旧主的
    // 依据，必须立即落盘，不能进合并窗口。
    const bool epoch_changed = EpochSumLocked() != epoch_before;
    ++m_sweeps;
    m_expired_total += expired.size();
    if (!expired.empty()) {
        metrics::LeasesExpired().Add(expired.size());
        m_sweep_persist_pending = true;
    }
    // 批量落盘：清扫每 10ms 一轮、租约表可达上千条，逐轮全量写盘会把 IO
    // 放大成 O(n)/10ms。窗口内只累积 dirty，到期后写一次；删除租约本身
    // 不改变 fencing，延迟提交是安全的（epoch 换代除外，见上）。
    const bool interval_elapsed =
        m_options.sweep_persist_interval <= Duration::zero() ||
        now - m_last_sweep_persist >= m_options.sweep_persist_interval;
    const bool flush =
        epoch_changed || (interval_elapsed && (m_sweep_persist_pending || m_dirty));
    if (flush) {
        m_sweep_persist_pending = false;
        m_last_sweep_persist = now;
    }
    const std::size_t swept = expired.size();
    lock.unlock();
    if (flush) {
        (void)PersistAfterWrite();
    }
    return swept;
}

bool Coordinator::EraseLeaseLocked(LeaseId id, TimePoint now) {
    const auto it = m_leases.find(id);
    if (it == m_leases.end()) {
        return false;
    }
    LeaseRecord record = std::move(it->second);
    m_leases.erase(it);
    // 租约表变化（含 lease-id 释放）需要反映到快照：重启后 m_next_lease 不会
    // 复用已删除的编号。
    m_dirty = true;

    switch (record.kind) {
    case LeaseKind::kService: {
        auto service_it = m_services.find(record.name);
        if (service_it != m_services.end()) {
            const auto instance_it = service_it->second.find(record.instance);
            // 索引可能已经被同名实例的新租约覆盖：只删除指向本租约的条目。
            if (instance_it != service_it->second.end() && instance_it->second == id) {
                service_it->second.erase(instance_it);
            }
            if (service_it->second.empty()) {
                m_services.erase(service_it);
            }
        }
        break;
    }
    case LeaseKind::kLock: {
        const auto lock_it = m_locks.find(record.name);
        if (lock_it != m_locks.end() && lock_it->second == id) {
            m_locks.erase(lock_it);
        }
        break;
    }
    case LeaseKind::kElection: {
        const auto election_it = m_elections.find(record.name);
        if (election_it != m_elections.end()) {
            const auto candidate_it = election_it->second.candidates.find(record.owner);
            if (candidate_it != election_it->second.candidates.end() &&
                candidate_it->second == id) {
                election_it->second.candidates.erase(candidate_it);
            }
            RecomputeLeaderLocked(election_it->second, now);
        }
        break;
    }
    }
    return true;
}

void Coordinator::RecomputeLeaderLocked(ElectionRecord &election, TimePoint now) {
    std::string best_node;
    LeaseId best_lease = kInvalidLease;
    for (const auto &[node, lease] : election.candidates) {
        const auto record_it = m_leases.find(lease);
        if (record_it == m_leases.end() || record_it->second.deadline <= now) {
            continue;
        }
        // 「租约最短者为主」的稳定落地：比较**租约编号**而不是剩余时间。
        // 剩余时间会因续约时序抖动（谁先续谁剩余更长），按它排序会让 leader
        // 在两台机器之间来回漂移；编号只在加入时分配，先加入者永远最小。
        if (best_lease == kInvalidLease || lease < best_lease) {
            best_lease = lease;
            best_node = node;
        }
    }
    if (best_node == election.leader) {
        return;
    }
    election.leader = best_node;
    // 每次 leader 实际变化都开启新任期（包括「有主 → 无主」）。其余节点用
    // epoch 判断自己看到的 leader 是否已经过时，epoch 不增就说明还是同一任。
    ++election.epoch;
    // 任期必须持久化：重启后新任期只能在旧任期之上递增（否则旧 leader 的
    // epoch 会被复用，客户端无法用它区分新旧主）。
    m_dirty = true;
}

LeaseView Coordinator::MakeViewLocked(const LeaseRecord &record, TimePoint now) const {
    LeaseView view;
    view.id = record.id;
    view.kind = record.kind;
    view.name = record.name;
    view.instance = record.instance;
    view.owner = record.owner;
    view.endpoint = record.endpoint;
    view.ttl = record.ttl;
    view.remaining = RemainingUntil(record.deadline, now);
    view.fencing = record.fencing;
    if (record.kind == LeaseKind::kElection) {
        const auto election_it = m_elections.find(record.name);
        if (election_it != m_elections.end()) {
            const ElectionRecord &election = election_it->second;
            view.epoch = election.epoch;
            view.fencing = election.epoch;
            view.leader_id = election.leader;
            view.leader = !election.leader.empty() && election.leader == record.owner;
            // leader 的租约可能已经过期而清扫还没跑：此时对外不能报告一个
            // 已经失效的 leader，否则客户端会继续向旧主发送写请求。
            const auto leader_it = election.candidates.find(election.leader);
            if (leader_it == election.candidates.end()) {
                view.leader_id.clear();
                view.leader = false;
            } else {
                const auto leader_lease = m_leases.find(leader_it->second);
                if (leader_lease == m_leases.end() ||
                    leader_lease->second.deadline <= now) {
                    view.leader_id.clear();
                    view.leader = false;
                }
            }
        }
    }
    return view;
}

Status Coordinator::RenewRecordLocked(LeaseRecord &record, TimePoint now,
                                      std::string *message) {
    if (record.deadline <= now) {
        if (message != nullptr) {
            *message = "lease expired before renew";
        }
        return Status::kExpired;
    }
    record.deadline = now + record.ttl;
    ++record.revision;
    return Status::kOk;
}

Result<LeaseView> Coordinator::RegisterService(const ServiceRegistration &registration) {
    if (registration.service.empty() || registration.instance.empty() ||
        registration.owner.empty()) {
        return Result<LeaseView>::Failure(Status::kInvalidArgument,
                                          "service, instance and owner must be non-empty");
    }
    if (!ValidTtl(registration.ttl)) {
        return Result<LeaseView>::Failure(Status::kInvalidArgument,
                                          "ttl out of supported range");
    }
    if (Outcome writable = EnsureWritable(); !writable.ok()) {
        return Result<LeaseView>::Failure(writable.status, writable.message);
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();

    // 不能长期持有 `m_services[service]` 的引用：回收旧租约时如果它是该
    // service 的最后一个实例，EraseLeaseLocked 会把整个 service 键删掉，
    // 引用随即悬空。这里先复制出旧 lease id，再做替换。
    LeaseId existing_lease = kInvalidLease;
    const auto service_it = m_services.find(registration.service);
    if (service_it != m_services.end()) {
        const auto instance_it = service_it->second.find(registration.instance);
        if (instance_it != service_it->second.end()) {
            existing_lease = instance_it->second;
        }
    }
    if (existing_lease != kInvalidLease) {
        const auto record_it = m_leases.find(existing_lease);
        if (record_it != m_leases.end() && record_it->second.deadline > now &&
            record_it->second.owner == registration.owner) {
            // 同一 owner 重复注册：幂等续约，保留租约编号（服务发现语义里
            // 实例 id 的「年龄」是稳定的，不能因为一次重复上报就被重置）。
            LeaseRecord &record = record_it->second;
            record.endpoint = registration.endpoint;
            record.ttl = registration.ttl;
            record.deadline = now + registration.ttl;
            ++record.revision;
            metrics::LeasesGranted().Inc();
            Result<LeaseView> result = Result<LeaseView>::Success(MakeViewLocked(record, now));
            lock.unlock();
            if (Outcome persisted = PersistAfterWrite(); !persisted.ok()) {
                return Result<LeaseView>::Failure(persisted.status, persisted.message);
            }
            return result;
        }
        // 不同 owner 或已过期：旧实例被替换，回收后重新分配（编号递增）。
        EraseLeaseLocked(existing_lease, now);
    }

    LeaseRecord record;
    record.id = m_next_lease++;
    m_dirty = true;  // lease-id 水位必须持久化：重启后编号不复用。
    record.kind = LeaseKind::kService;
    record.name = registration.service;
    record.instance = registration.instance;
    record.owner = registration.owner;
    record.endpoint = registration.endpoint;
    record.deadline = now + registration.ttl;
    record.ttl = registration.ttl;
    record.revision = 1;
    const LeaseId id = record.id;
    // 先登记索引再插入租约：两步之间不释放锁，不会出现「索引指向不存在
    // 的租约」的窗口。
    m_services[record.name][record.instance] = id;
    m_leases.emplace(id, std::move(record));
    metrics::LeasesGranted().Inc();
    Result<LeaseView> result = Result<LeaseView>::Success(MakeViewLocked(m_leases.at(id), now));
    lock.unlock();
    if (Outcome persisted = PersistAfterWrite(); !persisted.ok()) {
        return Result<LeaseView>::Failure(persisted.status, persisted.message);
    }
    return result;
}

Result<LeaseView> Coordinator::RenewLease(LeaseId lease, std::string_view owner) {
    if (Outcome writable = EnsureWritable(); !writable.ok()) {
        return Result<LeaseView>::Failure(writable.status, writable.message);
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto it = m_leases.find(lease);
    if (it == m_leases.end()) {
        return Result<LeaseView>::Failure(Status::kNotFound, "unknown lease");
    }
    LeaseRecord &record = it->second;
    if (record.owner != owner) {
        return Result<LeaseView>::Failure(Status::kNotOwner, "owner does not match lease");
    }
    const TimePoint now = Clock::now();
    std::string message;
    const Status status = RenewRecordLocked(record, now, &message);
    if (status != Status::kOk) {
        // 过期租约在返回前直接回收：让紧随其后的抢锁请求立即成功，而不是
        // 等下一次清扫（10ms 的窗口在测试里会被放大成可见的「锁丢了但抢不到」）。
        EraseLeaseLocked(lease, now);
        lock.unlock();
        (void)PersistAfterWrite();
        return Result<LeaseView>::Failure(status, std::move(message));
    }
    return Result<LeaseView>::Success(MakeViewLocked(record, now));
}

Outcome Coordinator::ReleaseLease(LeaseId lease, std::string_view owner) {
    if (Outcome writable = EnsureWritable(); !writable.ok()) {
        return writable;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto it = m_leases.find(lease);
    if (it == m_leases.end()) {
        return Outcome::Failure(Status::kNotFound, "unknown lease");
    }
    if (it->second.owner != owner) {
        return Outcome::Failure(Status::kNotOwner, "owner does not match lease");
    }
    EraseLeaseLocked(lease, Clock::now());
    lock.unlock();
    return PersistAfterWrite();
}

std::vector<ServiceInstance> Coordinator::LookupService(std::string_view service) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();
    std::vector<ServiceInstance> result;
    const auto service_it = m_services.find(std::string(service));
    if (service_it == m_services.end()) {
        return result;
    }
    result.reserve(service_it->second.size());
    for (const auto &[instance, lease] : service_it->second) {
        const auto record_it = m_leases.find(lease);
        if (record_it == m_leases.end() || record_it->second.deadline <= now) {
            continue;
        }
        const LeaseRecord &record = record_it->second;
        ServiceInstance entry;
        entry.service = record.name;
        entry.instance = record.instance;
        entry.endpoint = record.endpoint;
        entry.owner = record.owner;
        entry.lease = record.id;
        entry.ttl = record.ttl;
        entry.remaining = RemainingUntil(record.deadline, now);
        result.push_back(std::move(entry));
    }
    std::sort(result.begin(), result.end(),
              [](const ServiceInstance &left, const ServiceInstance &right) {
                  return left.instance < right.instance;
              });
    return result;
}

std::vector<std::string> Coordinator::ListServices() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();
    std::vector<std::string> result;
    result.reserve(m_services.size());
    for (const auto &[service, instances] : m_services) {
        const bool alive = std::any_of(instances.begin(), instances.end(),
                                       [&](const auto &entry) {
                                           const auto it = m_leases.find(entry.second);
                                           return it != m_leases.end() &&
                                                  it->second.deadline > now;
                                       });
        if (alive) {
            result.push_back(service);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

Result<LeaseView> Coordinator::AcquireLock(std::string_view key, std::string_view owner,
                                           Duration ttl) {
    if (key.empty() || owner.empty()) {
        return Result<LeaseView>::Failure(Status::kInvalidArgument,
                                          "key and owner must be non-empty");
    }
    if (!ValidTtl(ttl)) {
        return Result<LeaseView>::Failure(Status::kInvalidArgument,
                                          "ttl out of supported range");
    }
    if (Outcome writable = EnsureWritable(); !writable.ok()) {
        return Result<LeaseView>::Failure(writable.status, writable.message);
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();

    const auto lock_it = m_locks.find(std::string(key));
    if (lock_it != m_locks.end()) {
        const auto record_it = m_leases.find(lock_it->second);
        if (record_it != m_leases.end() && record_it->second.deadline > now) {
            return Result<LeaseView>::Failure(
                Status::kConflict, "lock held by " + record_it->second.owner);
        }
        // 已过期但尚未清扫：先回收，保证下面分配的新 token 一定大于前任。
        EraseLeaseLocked(lock_it->second, now);
    }

    LeaseRecord record;
    record.id = m_next_lease++;
    m_dirty = true;  // lease-id 水位必须持久化：重启后编号不复用。
    record.kind = LeaseKind::kLock;
    record.name = std::string(key);
    record.owner = std::string(owner);
    record.deadline = now + ttl;
    record.ttl = ttl;
    record.revision = 1;
    record.fencing = ++m_fencing[record.name];
    m_dirty = true;
    metrics::FencingAdvanced().Inc();
    const LeaseId id = record.id;
    m_locks[record.name] = id;
    m_leases.emplace(id, std::move(record));
    metrics::LeasesGranted().Inc();
    Result<LeaseView> result = Result<LeaseView>::Success(MakeViewLocked(m_leases.at(id), now));
    lock.unlock();
    // fencing token 必须在返回给调用方**之前**落盘：否则返回成功、重启回退，
    // 下游会同时接受两个持有者的写。
    if (Outcome persisted = PersistAfterWrite(); !persisted.ok()) {
        // 落盘失败必须**回滚内存授予**：否则调用方拿到 kUnavailable 后重试，
        // 会被自己刚授予的锁挡住一个 TTL（kConflict），表现为「报错却永久
        // 占着锁」。fencing 计数不回滚——token 只升不降，跳号是安全的。
        {
            std::lock_guard<std::mutex> rollback(m_mutex);
            const auto held = m_locks.find(std::string(key));
            if (held != m_locks.end() && held->second == id) {
                m_locks.erase(held);
            }
            m_leases.erase(id);
            // fencing 已前进，仍需要在下一次成功快照里落盘。
            m_dirty = true;
            m_sweep_persist_pending = true;
        }
        return Result<LeaseView>::Failure(persisted.status, persisted.message);
    }
    return result;
}

std::optional<LockInfo> Coordinator::QueryLock(std::string_view key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();
    const auto lock_it = m_locks.find(std::string(key));
    if (lock_it == m_locks.end()) {
        return std::nullopt;
    }
    const auto record_it = m_leases.find(lock_it->second);
    if (record_it == m_leases.end() || record_it->second.deadline <= now) {
        return std::nullopt;
    }
    const LeaseRecord &record = record_it->second;
    LockInfo info;
    info.key = record.name;
    info.owner = record.owner;
    info.lease = record.id;
    info.fencing = record.fencing;
    info.ttl = record.ttl;
    info.remaining = RemainingUntil(record.deadline, now);
    info.held = true;
    return info;
}

std::vector<LockInfo> Coordinator::ListLocks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();
    std::vector<LockInfo> result;
    result.reserve(m_locks.size());
    for (const auto &[key, lease] : m_locks) {
        const auto record_it = m_leases.find(lease);
        if (record_it == m_leases.end() || record_it->second.deadline <= now) {
            continue;
        }
        const LeaseRecord &record = record_it->second;
        LockInfo info;
        info.key = key;
        info.owner = record.owner;
        info.lease = record.id;
        info.fencing = record.fencing;
        info.ttl = record.ttl;
        info.remaining = RemainingUntil(record.deadline, now);
        info.held = true;
        result.push_back(std::move(info));
    }
    std::sort(result.begin(), result.end(),
              [](const LockInfo &left, const LockInfo &right) { return left.key < right.key; });
    return result;
}

Result<LeaseView> Coordinator::JoinElection(std::string_view election_name,
                                            std::string_view candidate, Duration ttl) {
    if (election_name.empty() || candidate.empty()) {
        return Result<LeaseView>::Failure(Status::kInvalidArgument,
                                          "election and candidate must be non-empty");
    }
    if (!ValidTtl(ttl)) {
        return Result<LeaseView>::Failure(Status::kInvalidArgument,
                                          "ttl out of supported range");
    }
    if (Outcome writable = EnsureWritable(); !writable.ok()) {
        return Result<LeaseView>::Failure(writable.status, writable.message);
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();
    ElectionRecord &election = m_elections[std::string(election_name)];

    const auto existing = election.candidates.find(std::string(candidate));
    if (existing != election.candidates.end()) {
        const auto record_it = m_leases.find(existing->second);
        if (record_it != m_leases.end() && record_it->second.deadline > now) {
            // 重复加入：原地续约，保留租约编号（也就保留了排队位置）。
            LeaseRecord &record = record_it->second;
            record.ttl = ttl;
            record.deadline = now + ttl;
            ++record.revision;
            RecomputeLeaderLocked(election, now);
            metrics::LeasesGranted().Inc();
            Result<LeaseView> result =
                Result<LeaseView>::Success(MakeViewLocked(record, now));
            lock.unlock();
            if (Outcome persisted = PersistAfterWrite(); !persisted.ok()) {
                return Result<LeaseView>::Failure(persisted.status, persisted.message);
            }
            return result;
        }
        // 过期记录还在索引里：先回收，再当作新候选者重新排队。
        EraseLeaseLocked(existing->second, now);
    }

    LeaseRecord record;
    record.id = m_next_lease++;
    m_dirty = true;  // lease-id 水位必须持久化：重启后编号不复用。
    record.kind = LeaseKind::kElection;
    record.name = std::string(election_name);
    record.owner = std::string(candidate);
    record.deadline = now + ttl;
    record.ttl = ttl;
    record.revision = 1;
    const LeaseId id = record.id;
    election.candidates[record.owner] = id;
    m_leases.emplace(id, std::move(record));
    RecomputeLeaderLocked(election, now);
    metrics::LeasesGranted().Inc();
    Result<LeaseView> result = Result<LeaseView>::Success(MakeViewLocked(m_leases.at(id), now));
    lock.unlock();
    if (Outcome persisted = PersistAfterWrite(); !persisted.ok()) {
        return Result<LeaseView>::Failure(persisted.status, persisted.message);
    }
    return result;
}

ElectionState Coordinator::QueryElection(std::string_view election_name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();
    ElectionState state;
    state.election = std::string(election_name);
    const auto election_it = m_elections.find(std::string(election_name));
    if (election_it == m_elections.end()) {
        return state;
    }
    const ElectionRecord &election = election_it->second;
    state.epoch = election.epoch;
    state.candidates.reserve(election.candidates.size());
    bool leader_alive = false;
    for (const auto &[node, lease] : election.candidates) {
        const auto record_it = m_leases.find(lease);
        if (record_it == m_leases.end() || record_it->second.deadline <= now) {
            continue;
        }
        CandidateInfo info;
        info.node = node;
        info.lease = lease;
        info.remaining = RemainingUntil(record_it->second.deadline, now);
        info.leader = node == election.leader;
        leader_alive = leader_alive || info.leader;
        state.candidates.push_back(std::move(info));
    }
    state.leader = leader_alive ? election.leader : std::string();
    std::sort(state.candidates.begin(), state.candidates.end(),
              [](const CandidateInfo &left, const CandidateInfo &right) {
                  return left.lease < right.lease;
              });
    return state;
}

std::vector<std::string> Coordinator::ListElections() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> result;
    result.reserve(m_elections.size());
    for (const auto &[name, election] : m_elections) {
        (void)election;
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

CoordinatorStats Coordinator::Stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const TimePoint now = Clock::now();
    CoordinatorStats stats;
    stats.active_leases = static_cast<std::size_t>(std::count_if(
        m_leases.begin(), m_leases.end(),
        [&](const auto &entry) { return entry.second.deadline > now; }));
    stats.services = static_cast<std::size_t>(std::count_if(
        m_services.begin(), m_services.end(), [&](const auto &entry) {
            return std::any_of(entry.second.begin(), entry.second.end(),
                               [&](const auto &instance) {
                                   const auto lease = m_leases.find(instance.second);
                                   return lease != m_leases.end() &&
                                          lease->second.deadline > now;
                               });
        }));
    for (const auto &[service, instances] : m_services) {
        (void)service;
        for (const auto &[instance, lease] : instances) {
            (void)instance;
            const auto record = m_leases.find(lease);
            if (record != m_leases.end() && record->second.deadline > now) {
                ++stats.instances;
            }
        }
    }
    for (const auto &[key, lease] : m_locks) {
        (void)key;
        const auto record = m_leases.find(lease);
        if (record != m_leases.end() && record->second.deadline > now) {
            ++stats.locks;
            ++stats.held_locks;
        }
    }
    stats.elections = m_elections.size();
    for (const auto &[name, election] : m_elections) {
        (void)name;
        for (const auto &[node, lease] : election.candidates) {
            (void)node;
            const auto record = m_leases.find(lease);
            if (record != m_leases.end() && record->second.deadline > now) {
                ++stats.candidates;
            }
        }
    }
    stats.expired_total = m_expired_total;
    stats.sweeps = m_sweeps;
    return stats;
}

} // namespace go2cpp::distributed
