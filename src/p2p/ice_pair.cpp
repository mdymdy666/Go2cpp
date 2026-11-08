/**
 * @file ice_pair.cpp
 * @brief 候选对状态机与热切换决策的实现。
 * @author go2cpp contributors
 * @date 2026-11-08
 * @copyright MIT License
 */

#include "go2cpp/p2p/ice_pair.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace go2cpp::p2p {
namespace {

std::int64_t ToNanos(std::chrono::milliseconds value) noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
}

}  // namespace

const char* IcePairStateName(IcePairState state) noexcept {
    switch (state) {
        case IcePairState::kWaiting:
            return "waiting";
        case IcePairState::kInProgress:
            return "in-progress";
        case IcePairState::kSucceeded:
            return "succeeded";
        case IcePairState::kFailed:
            return "failed";
    }
    return "unknown";
}

const char* IceSwitchDecisionName(IceSwitchDecision decision) noexcept {
    switch (decision) {
        case IceSwitchDecision::kNone:
            return "none";
        case IceSwitchDecision::kSwitch:
            return "switch";
        case IceSwitchDecision::kSwitchBack:
            return "switch-back";
        case IceSwitchDecision::kCooldownBlocked:
            return "cooldown-blocked";
        case IceSwitchDecision::kNoCandidate:
            return "no-candidate";
    }
    return "unknown";
}

IcePairController::IcePairController(IceSwitchoverConfig config) : m_config(std::move(config)) {
    // 防御性归一化：负值/0 在语义上有专门含义，但负的冷却期与探测间隔没有，
    // 这里统一夹到 0，避免调用方传 -1 时出现「永不过期」这种反直觉行为。
    if (m_config.switch_cooldown.count() < 0) {
        m_config.switch_cooldown = std::chrono::milliseconds(0);
    }
    if (m_config.probe_interval.count() < 0) {
        m_config.probe_interval = std::chrono::milliseconds(0);
    }
    if (m_config.failed_probe_backoff == 0) {
        m_config.failed_probe_backoff = 1;
    }
    if (m_config.max_pairs == 0) {
        m_config.max_pairs = 1;
    }
}

bool IcePairController::AddPair(const Endpoint& local, const Endpoint& remote,
                                std::uint64_t priority) {
    if (!local.valid() || !remote.valid()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const PairRecord& record : m_pairs) {
        if (record.local == local && record.remote == remote) {
            return false;
        }
    }
    if (m_pairs.size() >= m_config.max_pairs) {
        return false;
    }
    PairRecord record;
    record.local = local;
    record.remote = remote;
    record.priority = priority;
    m_pairs.push_back(record);
    return true;
}

void IcePairController::Clear() noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pairs.clear();
    m_selected = kNoSelection;
    m_last_switch_ns = 0;
    // generation 不重置：它是「本连接内切换过几次」的单调计数，上层用它做
    // 「流没重建」的对照，重置会让观测方误判为重新建链。
}

bool IcePairController::Select(const Endpoint& remote, std::int64_t now_ns) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t index = FindLocked(remote);
    if (index == kNoSelection) {
        return false;
    }
    m_pairs[index].selected_since_ns = now_ns;
    m_selected = index;
    return true;
}

void IcePairController::OnPacketReceived(const Endpoint& remote, std::int64_t now_ns) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t index = FindLocked(remote);
    if (index == kNoSelection) {
        return;
    }
    PairRecord& record = m_pairs[index];
    record.last_receive_ns = std::max(record.last_receive_ns, now_ns);
}

void IcePairController::OnCheckSent(const Endpoint& remote, std::int64_t now_ns) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t index = FindLocked(remote);
    if (index == kNoSelection) {
        return;
    }
    PairRecord& record = m_pairs[index];
    record.last_probe_ns = now_ns;
    ++record.checks_sent;
    if (record.state != IcePairState::kSucceeded) {
        record.state = IcePairState::kInProgress;
    }
}

void IcePairController::OnCheckSucceeded(const Endpoint& remote, std::int64_t now_ns,
                                         double rtt_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t index = FindLocked(remote);
    if (index == kNoSelection) {
        return;
    }
    PairRecord& record = m_pairs[index];
    record.state = IcePairState::kSucceeded;
    record.writable = true;
    ++record.checks_succeeded;
    record.last_receive_ns = std::max(record.last_receive_ns, now_ns);
    if (rtt_ms > 0.0) {
        // 与 P2PChannel 的 rtt_avg 一致：一次响应就是一次样本，用 EWMA 抑制
        // 单次抖动；首个样本直接取原值，避免从 0 慢慢爬上来。
        record.rtt_ms = record.rtt_ms > 0.0 ? record.rtt_ms * 0.8 + rtt_ms * 0.2 : rtt_ms;
    }
}

void IcePairController::OnCheckFailed(const Endpoint& remote, std::int64_t now_ns) {
    (void)now_ns;
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t index = FindLocked(remote);
    if (index == kNoSelection) {
        return;
    }
    PairRecord& record = m_pairs[index];
    ++record.checks_failed;
    if (record.writable) {
        // 成功过的对不降级：失败可能只是单个包丢了，判死会让切换阈值失真。
        return;
    }
    if (m_config.max_check_failures > 0 &&
        record.checks_failed >= static_cast<std::uint64_t>(m_config.max_check_failures)) {
        record.state = IcePairState::kFailed;
    } else if (record.checks_sent > 0) {
        record.state = IcePairState::kInProgress;
    }
}

IceSwitchEvent IcePairController::Evaluate(std::int64_t now_ns) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return EvaluateLocked(now_ns);
}

std::optional<IcePairSnapshot> IcePairController::Selected(std::int64_t now_ns) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_selected == kNoSelection) {
        return std::nullopt;
    }
    return MakeSnapshotLocked(m_pairs[m_selected], m_selected, now_ns);
}

bool IcePairController::HasPair(const Endpoint& remote) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return FindLocked(remote) != kNoSelection;
}

bool IcePairController::IsWritable(const Endpoint& remote) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t index = FindLocked(remote);
    return index != kNoSelection && m_pairs[index].writable;
}

std::optional<IcePairSnapshot> IcePairController::Pair(const Endpoint& remote,
                                                       std::int64_t now_ns) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t index = FindLocked(remote);
    if (index == kNoSelection) {
        return std::nullopt;
    }
    return MakeSnapshotLocked(m_pairs[index], index, now_ns);
}

std::vector<IcePairSnapshot> IcePairController::Snapshot(std::int64_t now_ns) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<IcePairSnapshot> snapshot;
    snapshot.reserve(m_pairs.size());
    for (std::size_t index = 0; index < m_pairs.size(); ++index) {
        snapshot.push_back(MakeSnapshotLocked(m_pairs[index], index, now_ns));
    }
    std::stable_sort(snapshot.begin(), snapshot.end(),
                     [](const IcePairSnapshot& left, const IcePairSnapshot& right) {
                         return left.priority > right.priority;
                     });
    return snapshot;
}

std::vector<Endpoint> IcePairController::DueProbeTargets(std::int64_t now_ns) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Endpoint> targets;
    const std::int64_t interval =
        ToNanos(std::max(m_config.probe_interval, std::chrono::milliseconds(1)));
    for (const PairRecord& record : m_pairs) {
        std::int64_t threshold = interval;
        if (record.state == IcePairState::kFailed && !record.writable) {
            threshold = interval * static_cast<std::int64_t>(m_config.failed_probe_backoff);
        }
        // last_probe_ns == 0 表示「从未探测」：立刻放行，连接建立后第一时间
        // 把所有对刷成已知状态，否则要等一个 interval 才可能发现备用路径。
        if (record.last_probe_ns != 0 && now_ns - record.last_probe_ns < threshold) {
            continue;
        }
        targets.push_back(record.remote);
    }
    std::stable_sort(targets.begin(), targets.end(),
                     [this](const Endpoint& left, const Endpoint& right) {
                         const std::size_t left_index = FindLocked(left);
                         const std::size_t right_index = FindLocked(right);
                         if (left_index == kNoSelection || right_index == kNoSelection) {
                             return false;
                         }
                         return m_pairs[left_index].priority > m_pairs[right_index].priority;
                     });
    return targets;
}

std::size_t IcePairController::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pairs.size();
}

std::uint64_t IcePairController::generation() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_generation;
}

IceSwitchoverConfig IcePairController::config() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

std::size_t IcePairController::FindLocked(const Endpoint& remote) const noexcept {
    for (std::size_t index = 0; index < m_pairs.size(); ++index) {
        if (m_pairs[index].remote == remote) {
            return index;
        }
    }
    return kNoSelection;
}

bool IcePairController::IsReceivingLocked(const PairRecord& record,
                                          std::int64_t now_ns) const noexcept {
    if (m_config.no_receive_timeout.count() <= 0) {
        // 0 表示关闭判定：永远视为在收，即永不因为「没包」而切换。
        return true;
    }
    // 起点取「最近收包」与「被选定」的较大者：刚切换过去的新对还没有收包样本，
    // 用 selected_since 兜底一个宽限期，否则会立刻被判死、来回切换。
    const std::int64_t base = std::max(record.last_receive_ns, record.selected_since_ns);
    if (base <= 0) {
        return false;
    }
    return now_ns - base <= ToNanos(m_config.no_receive_timeout);
}

std::size_t IcePairController::BestWritableLocked(std::size_t exclude, bool require_receiving,
                                                  std::int64_t now_ns) const noexcept {
    std::size_t best = kNoSelection;
    for (std::size_t index = 0; index < m_pairs.size(); ++index) {
        if (index == exclude) {
            continue;
        }
        const PairRecord& record = m_pairs[index];
        if (!record.writable) {
            continue;
        }
        if (require_receiving && !IsReceivingLocked(record, now_ns)) {
            continue;
        }
        if (best == kNoSelection) {
            best = index;
            continue;
        }
        const PairRecord& incumbent = m_pairs[best];
        if (record.priority > incumbent.priority ||
            (record.priority == incumbent.priority &&
             record.last_receive_ns > incumbent.last_receive_ns)) {
            best = index;
        }
    }
    return best;
}

IcePairSnapshot IcePairController::MakeSnapshotLocked(const PairRecord& record, std::size_t index,
                                                      std::int64_t now_ns) const noexcept {
    IcePairSnapshot snapshot;
    snapshot.local = record.local;
    snapshot.remote = record.remote;
    snapshot.priority = record.priority;
    snapshot.state = record.state;
    snapshot.writable = record.writable;
    snapshot.receiving = IsReceivingLocked(record, now_ns);
    snapshot.selected = index == m_selected;
    snapshot.last_receive_ns = record.last_receive_ns;
    snapshot.selected_since_ns = record.selected_since_ns;
    snapshot.checks_sent = record.checks_sent;
    snapshot.checks_succeeded = record.checks_succeeded;
    snapshot.checks_failed = record.checks_failed;
    snapshot.rtt_ms = record.rtt_ms;
    return snapshot;
}

IceSwitchEvent IcePairController::EvaluateLocked(std::int64_t now_ns) {
    IceSwitchEvent event;
    event.generation = m_generation;

    // step1: 功能关闭或还没有选定对时，一切保持原样。
    if (!m_config.enabled) {
        event.reason = "disabled";
        return event;
    }
    if (m_selected == kNoSelection) {
        event.reason = "no-selected-pair";
        return event;
    }

    const PairRecord& current = m_pairs[m_selected];
    event.from = current.remote;

    // step2: 当前对是否还在收包（selected_since 提供切换后的宽限期）。
    const bool current_receiving = IsReceivingLocked(current, now_ns);
    const std::int64_t cooldown_ns = ToNanos(m_config.switch_cooldown);
    const bool cooldown_ok =
        m_last_switch_ns == 0 || cooldown_ns <= 0 || now_ns - m_last_switch_ns >= cooldown_ns;

    if (current_receiving) {
        // step3: 当前对健康 → 默认不动。只有显式允许回切、且存在「可写 + 在收包 +
        // 优先级严格更高」的旧对时才回切，避免同级对之间来回跳。
        if (!m_config.allow_switch_back) {
            event.reason = "current-receiving";
            return event;
        }
        const std::size_t best = BestWritableLocked(m_selected, true, now_ns);
        if (best == kNoSelection) {
            event.reason = "no-better-pair";
            return event;
        }
        if (m_pairs[best].priority <= current.priority) {
            event.reason = "current-is-best";
            return event;
        }
        if (!cooldown_ok) {
            event.decision = IceSwitchDecision::kCooldownBlocked;
            event.reason = "switch-back-cooldown";
            return event;
        }
        // step6（回切分支）: 与切换共用同一套状态更新。
        m_selected = best;
        m_last_switch_ns = now_ns;
        ++m_generation;
        m_pairs[best].selected_since_ns = now_ns;
        event.decision = IceSwitchDecision::kSwitchBack;
        event.to = m_pairs[best].remote;
        event.generation = m_generation;
        event.reason = "preferred-pair-recovered";
        return event;
    }

    // step4: 当前对已死（超时没收到任何包）→ 找优先级最高的可写对。
    // 这里**不**要求目标对已经 receiving：网络刚切过去时它只可能「可写」，
    // 强迫它先收包才允许切换等于永远不切换。
    const std::size_t best = BestWritableLocked(m_selected, false, now_ns);
    if (best == kNoSelection) {
        event.decision = IceSwitchDecision::kNoCandidate;
        event.reason = "no-writable-pair";
        return event;
    }
    // step5: 冷却期内只报告、不改选定对——这是防抖动的关键。
    if (!cooldown_ok) {
        event.decision = IceSwitchDecision::kCooldownBlocked;
        event.reason = "switch-cooldown";
        return event;
    }

    // step6: 提交切换（上层流/会话不变，只换远端地址）。
    m_selected = best;
    m_last_switch_ns = now_ns;
    ++m_generation;
    m_pairs[best].selected_since_ns = now_ns;
    event.decision = IceSwitchDecision::kSwitch;
    event.to = m_pairs[best].remote;
    event.generation = m_generation;
    event.reason = "selected-pair-not-receiving";
    return event;
}

}  // namespace go2cpp::p2p
