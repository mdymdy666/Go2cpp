/**
 * @file test_session_control.cpp
 * @brief 控制权仲裁测试：并发请求、唯一控制器、断开/租约回收、Revoke、策略注入与审计。
 * @details 覆盖场景矩阵 3.4 / 7.1–7.3（FR-03 / FR-06 / RISK-024 / AX-036）：
 *          - 场景 1：16 个 peer 并发 RequestControl，只有 1 个拿到 kInteract。
 *          - 场景 2：控制器断开（OnPeerDisconnected）自动回收，其他人可接手。
 *          - 场景 3：Revoke 后该 peer 请求失败，Grant 解除后才恢复。
 *          - 场景 4：租约过期自动回收。
 *          - 场景 5：可抢占策略下后来者抢占，原控制器降为 kView。
 *          - 场景 6：变更回调顺序/代数、Snapshot JSON、统计计数。
 *          - 场景 7：参数与边界（0、重复 Join、上限、Grant 自动创建）。
 *          - 场景 8：Grant(kNone) 收回权限后 RequestControl 必须被拒（P1-1）。
 *          - 场景 9：测试内自定义 IControlPolicy 注入生效，内置策略按枚举回退（P1-2）。
 *          - 场景 10：审计字段（seq/actor/timestamp/session_id）与 Snapshot().audit（P1-3）。
 *          - 场景 11：随机操作序列 4000 步，验证「最多一个 Interact」不变式。
 *          - 场景 12：Revoke 状态跨 Leave/断开/重连 Join 保持（SESS-01）。
 *          - 场景 13：注入时钟的租约等号边界（deadline-1 不回收、deadline 回收），
 *            并在同一毫秒内断言 `lease_remaining_ms` 的精确值。
 *          - 场景 14：`ControlChange::controller` 契约——所有事件都携带
 *            「变化后的唯一控制器」（抢占/撤销/断开/租约过期）。
 *          - 场景 15：`Renew` 顺延租约——旧 deadline 不再回收、新 deadline 回收。
 *          - 场景 16：16 线程并发 Grant(kInteract)/RequestControl，唯一控制器、
 *            成功计数与统计守恒。
 *          - 场景 17：10 万次混合操作压测（Grant/Revoke/Request/Tick/Leave+Join），
 *            吞吐打印 + 代数/统计/上限守恒。
 *          - 场景 18：1000 条随机序列（60 步/序列）覆盖随机配置与操作，逐步验证
 *            唯一控制器、参与者上限与 generation/changes 守恒。
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/session_control.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::ControlChange;
using go2cpp::bento::ControlChangeReason;
using go2cpp::bento::ControlDecision;
using go2cpp::bento::ControlMode;
using go2cpp::bento::ControlPolicy;
using go2cpp::bento::ControlRequestContext;
using go2cpp::bento::ControlSnapshot;
using go2cpp::bento::IControlPolicy;
using go2cpp::bento::SessionControl;
using go2cpp::bento::SessionControlConfig;
using go2cpp::bento::SessionControlStats;

/// @brief 租约 deadline 精确已知的仲裁器（供注入时钟的边界用例使用）。
struct KnownLeaseControl {
    std::unique_ptr<SessionControl> control;
    std::uint64_t deadline = 0;
    ControlSnapshot initial_snapshot;
};

/// @brief 构造 deadline 精确可算的 SessionControl（deadline = now + lease_ms）。
/// @details 实现内部用 `NowMs()` 记录 deadline，因此只有 RequestControl 与
///          紧随其后的 Snapshot 都落在同一毫秒，deadline 才是确定的整数毫秒。
///          失败时重试（不 sleep），避免偶发跨毫秒让边界断言变成 flaky。
KnownLeaseControl MakeLeaseControl(std::uint32_t lease_ms) {
    KnownLeaseControl result;
    for (int attempt = 0; attempt < 500; ++attempt) {
        SessionControlConfig config;
        config.lease_ms = lease_ms;
        auto control = std::make_unique<SessionControl>(config);
        if (!control->Join(1) || !control->Join(2)) {
            continue;
        }
        const std::uint64_t before_request = go2cpp::bento::NowMs();
        const bool granted = control->RequestControl(1);
        const std::uint64_t after_request = go2cpp::bento::NowMs();
        const std::uint64_t before_snapshot = go2cpp::bento::NowMs();
        const ControlSnapshot snapshot = control->Snapshot();
        const std::uint64_t after_snapshot = go2cpp::bento::NowMs();
        if (!granted || before_request != after_request || before_request != before_snapshot ||
            before_snapshot != after_snapshot) {
            continue;
        }
        result.control = std::move(control);
        result.deadline = before_request + lease_ms;
        result.initial_snapshot = snapshot;
        return result;
    }
    return result;
}

/// 统计当前 kInteract 的数量，并断言它与 controller() 一致。
int CountInteract(const SessionControl& control) {
    int interact = 0;
    for (const std::uint32_t peer_id : control.participants()) {
        if (control.mode(peer_id) == ControlMode::kInteract) {
            ++interact;
        }
    }
    return interact;
}

/// 不变式：最多一个 Interact；有 controller 时它必须是 Interact。
void CheckInvariants(const SessionControl& control, const char* where) {
    const int interact = CountInteract(control);
    BENTO_CHECK(interact <= 1);
    const std::uint32_t controller = control.controller();
    if (controller != 0) {
        BENTO_CHECK_EQ(control.mode(controller), ControlMode::kInteract);
        BENTO_CHECK_EQ(interact, 1);
        // 被撤销者永远不能是控制器（Revoke 会立刻回收，Grant 解除后才会清除）。
        BENTO_CHECK(!control.revoked(controller));
    } else {
        BENTO_CHECK_EQ(interact, 0);
    }
    if (interact > 1) {
        std::fprintf(stderr, "[FAIL] invariant broken at %s: interact=%d\n", where, interact);
    }
}

/// 场景 9 用的自定义策略：只批准偶数 peer 抢占，奇数一律拒绝。
/// 定义在测试文件内，证明「新增策略不改库源码」。
class EvenPeerPreemptPolicy final : public IControlPolicy {
   public:
    const char* name() const noexcept override { return "test_even_preempt"; }

    ControlDecision Decide(const ControlRequestContext& context) const override {
        ++calls_;
        last_controller_ = context.controller;
        last_requests_ = context.requests;
        last_participants_ = context.participants;
        return (context.peer_id % 2U) == 0 ? ControlDecision::kGrant : ControlDecision::kDeny;
    }

    int calls() const { return calls_; }
    std::uint32_t last_controller() const { return last_controller_; }
    std::uint64_t last_requests() const { return last_requests_; }
    std::size_t last_participants() const { return last_participants_; }

   private:
    mutable int calls_ = 0;
    mutable std::uint32_t last_controller_ = 0;
    mutable std::uint64_t last_requests_ = 0;
    mutable std::size_t last_participants_ = 0;
};

/// 场景 1：16 个 peer 并发请求，只有 1 个成为控制器，其余保持 View。
void TestConcurrentRequests() {
    constexpr std::uint32_t kPeers = 16;
    SessionControl control;
    for (std::uint32_t peer_id = 1; peer_id <= kPeers; ++peer_id) {
        BENTO_CHECK(control.Join(peer_id));
    }
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> wins{0};
    std::vector<std::thread> threads;
    threads.reserve(kPeers);
    for (std::uint32_t peer_id = 1; peer_id <= kPeers; ++peer_id) {
        threads.emplace_back([&, peer_id]() {
            ready.fetch_add(1);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (control.RequestControl(peer_id)) {
                wins.fetch_add(1);
            }
        });
    }
    while (ready.load() < static_cast<int>(kPeers)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    BENTO_CHECK_EQ(wins.load(), 1);
    BENTO_CHECK_EQ(CountInteract(control), 1);
    BENTO_CHECK(control.controller() != 0);
    CheckInvariants(control, "concurrent");
    const auto stats = control.stats();
    BENTO_CHECK_EQ(stats.requests, kPeers);
    BENTO_CHECK_EQ(stats.denials, kPeers - 1);
    std::printf("[INFO] concurrent: peers=%u winners=%d controller=%u denials=%llu\n", kPeers,
                wins.load(), control.controller(),
                static_cast<unsigned long long>(stats.denials));
}

/// 场景 2：控制器断开后自动回收，其他 peer 可立即接手。
void TestDisconnectReclaim() {
    SessionControl control;
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(control.Join(2));
    BENTO_CHECK(control.Join(3));
    BENTO_CHECK(control.RequestControl(1));
    BENTO_CHECK_EQ(control.controller(), 1U);

    std::vector<ControlChange> changes;
    control.on_change = [&](const ControlChange& change) { changes.push_back(change); };

    BENTO_CHECK(control.OnPeerDisconnected(1));
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kNone);
    BENTO_CHECK_EQ(control.participants().size(), static_cast<std::size_t>(2));
    BENTO_CHECK_EQ(changes.size(), static_cast<std::size_t>(1));
    BENTO_CHECK_EQ(changes.front().reason, ControlChangeReason::kDisconnected);
    BENTO_CHECK_EQ(changes.front().from, ControlMode::kInteract);
    BENTO_CHECK_EQ(changes.front().to, ControlMode::kNone);
    // 断开是被动事件：审计 actor 记为 0。
    BENTO_CHECK_EQ(changes.front().actor, 0U);

    // 剩余 peer 可以接手，且断开者不再参与仲裁。
    BENTO_CHECK(control.RequestControl(2));
    BENTO_CHECK_EQ(control.controller(), 2U);
    BENTO_CHECK_EQ(control.stats().disconnects, 1U);
    CheckInvariants(control, "disconnect");
}

/// 场景 3：Revoke 后请求失败；Grant 解除禁入后才可重新请求。
void TestRevokeBlocksRequest() {
    SessionControl control;
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(control.Join(2));
    BENTO_CHECK(control.RequestControl(1));
    BENTO_CHECK_EQ(control.controller(), 1U);

    BENTO_CHECK(control.Revoke(1));
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kView);
    BENTO_CHECK(control.revoked(1));
    // 被撤销者不能直接重新请求。
    BENTO_CHECK(!control.RequestControl(1));
    BENTO_CHECK(control.stats().denials >= 1);
    // 另一个 peer 不受影响。
    BENTO_CHECK(control.RequestControl(2));
    BENTO_CHECK_EQ(control.controller(), 2U);
    // 撤销 2 后，1 仍需 Grant 解除。
    BENTO_CHECK(control.Revoke(2));
    BENTO_CHECK(!control.RequestControl(1));
    BENTO_CHECK(control.Grant(1));
    BENTO_CHECK(!control.revoked(1));
    BENTO_CHECK(control.RequestControl(1));
    BENTO_CHECK_EQ(control.controller(), 1U);

    // 无参 Revoke 撤销当前控制器，并同时禁止其再次请求。
    BENTO_CHECK(control.Revoke());
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK(control.revoked(1));
    BENTO_CHECK(!control.RequestControl(1));
    BENTO_CHECK(!control.Revoke());  // 已无控制器
    CheckInvariants(control, "revoke");
}

/// 场景 4：租约过期后自动回收为 View（注入时钟，不依赖 sleep）。
void TestLeaseExpiry() {
    KnownLeaseControl lease = MakeLeaseControl(100);
    BENTO_CHECK(lease.control != nullptr);
    if (lease.control == nullptr) {
        return;
    }
    SessionControl& control = *lease.control;
    BENTO_CHECK_EQ(control.controller(), 1U);
    // 未过期时 Tick 不回收。
    BENTO_CHECK(!control.Tick(lease.deadline - 1));
    BENTO_CHECK_EQ(control.controller(), 1U);
    BENTO_CHECK(control.Tick(lease.deadline));
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kView);
    BENTO_CHECK_EQ(control.stats().lease_expirations, 1U);
    // 过期后其他 peer 可请求。
    BENTO_CHECK(control.RequestControl(2));
    BENTO_CHECK_EQ(control.controller(), 2U);
    CheckInvariants(control, "lease");
}

/// 场景 13：租约等号边界——`Tick(deadline-1)` 不回收、`Tick(deadline)`
/// 恰好回收；事件字段、代数与统计守恒；`lease_remaining_ms` 在同一毫秒内精确。
void TestLeaseBoundaryInjectedClock() {
    KnownLeaseControl lease = MakeLeaseControl(1000);
    BENTO_CHECK(lease.control != nullptr);
    if (lease.control == nullptr) {
        return;
    }
    SessionControl& control = *lease.control;
    std::vector<ControlChange> changes;
    control.on_change = [&](const ControlChange& change) { changes.push_back(change); };
    const std::uint64_t generation_at_grant = control.generation();
    const std::uint64_t deadline = lease.deadline;

    // 初始快照与 deadline 同一毫秒：剩余量精确等于租约长度。
    BENTO_CHECK_EQ(lease.initial_snapshot.controller, 1U);
    BENTO_CHECK_EQ(lease.initial_snapshot.lease_remaining_ms, 1000U);

    // deadline-10 / deadline-1：都未到期，不回收、不产生事件。
    BENTO_CHECK(!control.Tick(deadline - 10));
    BENTO_CHECK(!control.Tick(deadline - 1));
    BENTO_CHECK_EQ(control.controller(), 1U);
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kInteract);
    BENTO_CHECK(control.is_controller(1));
    BENTO_CHECK(control.has_controller());
    BENTO_CHECK(changes.empty());
    BENTO_CHECK_EQ(control.stats().lease_expirations, 0U);

    // 恰好 deadline：必须回收（实现是 now < deadline 才续期）。
    BENTO_CHECK(control.Tick(deadline));
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK(!control.has_controller());
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kView);
    BENTO_CHECK_EQ(control.stats().lease_expirations, 1U);
    BENTO_CHECK_EQ(control.generation(), generation_at_grant + 1);
    BENTO_CHECK_EQ(control.stats().changes, generation_at_grant + 1);
    BENTO_CHECK_EQ(changes.size(), static_cast<std::size_t>(1));
    if (changes.size() == 1) {
        BENTO_CHECK_EQ(changes[0].peer_id, 1U);
        BENTO_CHECK_EQ(changes[0].reason, ControlChangeReason::kLeaseExpired);
        BENTO_CHECK_EQ(changes[0].from, ControlMode::kInteract);
        BENTO_CHECK_EQ(changes[0].to, ControlMode::kView);
        // 事件契约：controller 是「变化后的唯一控制器」，租约过期后必须为 0。
        BENTO_CHECK_EQ(changes[0].controller, 0U);
        BENTO_CHECK_EQ(changes[0].generation, generation_at_grant + 1);
    }
    const ControlSnapshot expired = control.Snapshot();
    BENTO_CHECK_EQ(expired.controller, 0U);
    BENTO_CHECK_EQ(expired.lease_remaining_ms, 0U);
    BENTO_CHECK_EQ(expired.generation, generation_at_grant + 1);
    CheckInvariants(control, "lease-boundary");

    // 同一个 deadline 再次 Tick、以及更晚的 Tick：不重复回收、不重复计数。
    BENTO_CHECK(!control.Tick(deadline));
    BENTO_CHECK(!control.Tick(deadline + 5000));
    BENTO_CHECK_EQ(control.stats().lease_expirations, 1U);
    BENTO_CHECK_EQ(changes.size(), static_cast<std::size_t>(1));
    BENTO_CHECK_EQ(control.generation(), generation_at_grant + 1);

    // 精确剩余量：10ms 租约下 RequestControl 所在毫秒即 deadline-10，
    // Snapshot 必须精确报告 10ms（不 sleep），且该时刻 Tick 仍不回收。
    KnownLeaseControl short_lease = MakeLeaseControl(10);
    BENTO_CHECK(short_lease.control != nullptr);
    if (short_lease.control == nullptr) {
        return;
    }
    SessionControl& short_control = *short_lease.control;
    BENTO_CHECK_EQ(short_lease.initial_snapshot.lease_remaining_ms, 10U);
    BENTO_CHECK(!short_control.Tick(short_lease.deadline - 10));
    BENTO_CHECK_EQ(short_control.controller(), 1U);
    BENTO_CHECK(short_control.Tick(short_lease.deadline));
    BENTO_CHECK_EQ(short_control.controller(), 0U);
    BENTO_CHECK_EQ(short_control.stats().lease_expirations, 1U);
    BENTO_CHECK_EQ(short_control.Snapshot().lease_remaining_ms, 0U);
}

/// 场景 5：可抢占策略。
void TestPreemptPolicy() {
    SessionControlConfig config;
    config.policy = ControlPolicy::kPreempt;
    SessionControl control(config);
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(control.Join(2));
    std::vector<ControlChange> changes;
    control.on_change = [&](const ControlChange& change) { changes.push_back(change); };
    BENTO_CHECK(control.RequestControl(1));
    changes.clear();
    BENTO_CHECK(control.RequestControl(2));
    BENTO_CHECK_EQ(control.controller(), 2U);
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kView);
    BENTO_CHECK_EQ(control.mode(2), ControlMode::kInteract);
    BENTO_CHECK_EQ(control.stats().preemptions, 1U);
    // 先 kPreempted(1) 后 kRequested(2)，generation 严格递增。
    BENTO_CHECK_EQ(changes.size(), static_cast<std::size_t>(2));
    if (changes.size() == 2) {
        BENTO_CHECK_EQ(changes[0].peer_id, 1U);
        BENTO_CHECK_EQ(changes[0].reason, ControlChangeReason::kPreempted);
        BENTO_CHECK_EQ(changes[1].peer_id, 2U);
        BENTO_CHECK_EQ(changes[1].reason, ControlChangeReason::kRequested);
        BENTO_CHECK(changes[1].generation > changes[0].generation);
        BENTO_CHECK_EQ(changes[1].controller, 2U);
        // 抢占事件同样携带「变化后」的新控制器（修复前这里是旧控制器 1）。
        BENTO_CHECK_EQ(changes[0].controller, 2U);
        // 抢占事件的 actor 记录发起抢占的请求者。
        BENTO_CHECK_EQ(changes[0].actor, 2U);
        BENTO_CHECK_EQ(changes[1].actor, 2U);
    }
    CheckInvariants(control, "preempt");
}

/// 场景 6：回调/快照/统计。
void TestCallbacksAndSnapshot() {
    SessionControl control;
    std::vector<ControlChange> changes;
    control.on_change = [&](const ControlChange& change) { changes.push_back(change); };
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(control.RequestControl(1));
    BENTO_CHECK(control.Revoke(1));
    BENTO_CHECK_EQ(changes.size(), static_cast<std::size_t>(3));
    if (changes.size() == 3) {
        BENTO_CHECK_EQ(changes[0].reason, ControlChangeReason::kJoined);
        BENTO_CHECK_EQ(changes[0].from, ControlMode::kNone);
        BENTO_CHECK_EQ(changes[0].to, ControlMode::kView);
        BENTO_CHECK_EQ(changes[1].reason, ControlChangeReason::kRequested);
        BENTO_CHECK_EQ(changes[1].to, ControlMode::kInteract);
        BENTO_CHECK_EQ(changes[2].reason, ControlChangeReason::kRevoked);
        BENTO_CHECK_EQ(changes[2].to, ControlMode::kView);
    }
    const ControlSnapshot snapshot = control.Snapshot();
    BENTO_CHECK_EQ(snapshot.controller, 0U);
    BENTO_CHECK_EQ(snapshot.generation, 3U);
    BENTO_CHECK_EQ(snapshot.participants.size(), static_cast<std::size_t>(1));
    if (!snapshot.participants.empty()) {
        BENTO_CHECK_EQ(snapshot.participants.front().peer_id, 1U);
        BENTO_CHECK_EQ(snapshot.participants.front().mode, ControlMode::kView);
        BENTO_CHECK(snapshot.participants.front().revoked);
        BENTO_CHECK(!snapshot.participants.front().controller);
    }
    const std::string json = snapshot.ToJson();
    BENTO_CHECK(json.find("\"controller\":0") != std::string::npos);
    BENTO_CHECK(json.find("\"generation\":3") != std::string::npos);
    BENTO_CHECK(json.find("\"revoked\":true") != std::string::npos);
    BENTO_CHECK(json.find("\"mode\":\"view\"") != std::string::npos);

    const auto stats = control.stats();
    BENTO_CHECK_EQ(stats.changes, 3U);
    BENTO_CHECK_EQ(stats.requests, 1U);
    BENTO_CHECK_EQ(stats.revocations, 1U);
    BENTO_CHECK_EQ(stats.grants, 0U);
    BENTO_CHECK_EQ(control.generation(), 3U);

    // 回调在锁外执行：回调里查询 Snapshot 不会死锁。
    bool reentered = false;
    control.on_change = [&](const ControlChange&) {
        reentered = true;
        (void)control.Snapshot();
        (void)control.controller();
    };
    BENTO_CHECK(control.Join(2));
    BENTO_CHECK(reentered);
}

/// 场景 7：边界与配置。
void TestValidationAndConfig() {
    SessionControl control;
    BENTO_CHECK(!control.Join(0));
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(!control.Join(1));  // 重复加入
    BENTO_CHECK_EQ(control.mode(99), ControlMode::kNone);
    BENTO_CHECK(!control.is_controller(99));
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK(!control.Leave(99));
    BENTO_CHECK(!control.OnPeerDisconnected(99));
    BENTO_CHECK(!control.Revoke(99));
    BENTO_CHECK(!control.RequestControl(0));
    BENTO_CHECK(!control.RequestControl(99));  // 未加入
    BENTO_CHECK(!control.Renew(1));            // 不是控制器
    // 0 号 peer 在所有入口都被拒绝（0 保留表示「无控制器/系统」）。
    BENTO_CHECK(!control.Grant(0));
    BENTO_CHECK(!control.Grant(0, ControlMode::kInteract));
    BENTO_CHECK(!control.Revoke(0));
    BENTO_CHECK(!control.Leave(0));
    BENTO_CHECK(!control.OnPeerDisconnected(0));
    BENTO_CHECK(!control.Renew(0));

    // Grant 可自动创建参与者；Grant(kNone) 收回权限但保留记录。
    BENTO_CHECK(control.Grant(42));
    BENTO_CHECK_EQ(control.mode(42), ControlMode::kView);
    BENTO_CHECK(control.Grant(42, ControlMode::kInteract));
    BENTO_CHECK_EQ(control.controller(), 42U);
    BENTO_CHECK(control.Renew(42));
    BENTO_CHECK(control.Grant(42, ControlMode::kNone));
    BENTO_CHECK_EQ(control.mode(42), ControlMode::kNone);
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK(control.Grant(42));

    // 上限。
    SessionControlConfig limited;
    limited.max_participants = 2;
    SessionControl small(limited);
    BENTO_CHECK(small.Join(1));
    BENTO_CHECK(small.Join(2));
    BENTO_CHECK(!small.Join(3));
    BENTO_CHECK(!small.Grant(3));

    // default_mode = kInteract：首个加入者直接成为控制器，后续为 View。
    SessionControlConfig eager;
    eager.default_mode = ControlMode::kInteract;
    SessionControl direct(eager);
    BENTO_CHECK(direct.Join(1));
    BENTO_CHECK_EQ(direct.controller(), 1U);
    BENTO_CHECK(direct.Join(2));
    BENTO_CHECK_EQ(direct.mode(2), ControlMode::kView);
    CheckInvariants(direct, "config");

    // lease_ms = 0：永不过期。
    SessionControlConfig forever;
    forever.lease_ms = 0;
    SessionControl permanent(forever);
    BENTO_CHECK(permanent.Join(1));
    BENTO_CHECK(permanent.RequestControl(1));
    BENTO_CHECK(!permanent.Tick());
    BENTO_CHECK_EQ(permanent.controller(), 1U);

    // 模式/原因名（全部枚举成员，指标标签依赖这些稳定字符串）。
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlModeName(ControlMode::kNone)), "none");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlModeName(ControlMode::kView)), "view");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlModeName(ControlMode::kInteract)),
                   "interact");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlChangeReasonName(
                        ControlChangeReason::kJoined)),
                   "joined");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlChangeReasonName(
                        ControlChangeReason::kLeft)),
                   "left");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlChangeReasonName(
                        ControlChangeReason::kGranted)),
                   "granted");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlChangeReasonName(
                        ControlChangeReason::kRequested)),
                   "requested");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlChangeReasonName(
                        ControlChangeReason::kPreempted)),
                   "preempted");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlChangeReasonName(
                        ControlChangeReason::kRevoked)),
                   "revoked");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlChangeReasonName(
                        ControlChangeReason::kDisconnected)),
                   "disconnected");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlChangeReasonName(
                        ControlChangeReason::kLeaseExpired)),
                   "lease_expired");

    // 空快照：ToJson 必须给出空参与者数组且 controller 为 0。
    SessionControl empty;
    const std::string empty_json = empty.Snapshot().ToJson();
    BENTO_CHECK(empty_json.find("\"controller\":0") != std::string::npos);
    BENTO_CHECK(empty_json.find("\"participants\":[]") != std::string::npos);
    BENTO_CHECK_EQ(empty.controller(), 0U);
    BENTO_CHECK(!empty.has_controller());
}

/// 场景 8（P1-1）：Grant(kNone) 收回权限后，RequestControl 必须一律被拒。
/// 用 kPreempt 配置证明「权限检查先于策略」，策略无法绕过 kNone。
void TestGrantNoneBlocksRequest() {
    SessionControlConfig config;
    config.policy = ControlPolicy::kPreempt;
    config.lease_ms = 0;
    SessionControl control(config);
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(control.Join(2));
    BENTO_CHECK(control.Join(3));
    BENTO_CHECK(control.RequestControl(1));
    BENTO_CHECK_EQ(control.controller(), 1U);

    // Grant(kNone)：收回全部权限但保留参与者记录（并解除 revoke 标记）。
    BENTO_CHECK(control.Grant(2, ControlMode::kNone));
    BENTO_CHECK_EQ(control.mode(2), ControlMode::kNone);
    BENTO_CHECK(!control.revoked(2));

    const SessionControlStats before = control.stats();
    const std::uint64_t generation_before = control.generation();
    // 有控制器时：kNone 请求被拒，且不产生任何变更事件。
    BENTO_CHECK(!control.RequestControl(2));
    BENTO_CHECK_EQ(control.controller(), 1U);
    BENTO_CHECK_EQ(control.mode(2), ControlMode::kNone);
    BENTO_CHECK_EQ(control.generation(), generation_before);
    BENTO_CHECK_EQ(control.stats().denials, before.denials + 1);
    BENTO_CHECK_EQ(control.stats().requests, before.requests + 1);

    // 无控制器时同样被拒：kNone 不是「可自行恢复的降级」。
    BENTO_CHECK(control.Revoke(1));
    BENTO_CHECK_EQ(control.controller(), 0U);
    const std::uint64_t generation_after_revoke = control.generation();
    BENTO_CHECK(!control.RequestControl(2));
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK_EQ(control.generation(), generation_after_revoke);

    // 显式 Grant 恢复权限后才可请求。
    BENTO_CHECK(control.Grant(2));
    BENTO_CHECK_EQ(control.mode(2), ControlMode::kView);
    BENTO_CHECK(control.RequestControl(2));
    BENTO_CHECK_EQ(control.controller(), 2U);

    // Grant(kNone) 作用于当前控制器时立即回收控制权。
    BENTO_CHECK(control.Grant(2, ControlMode::kNone));
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK_EQ(control.mode(2), ControlMode::kNone);
    BENTO_CHECK(!control.RequestControl(2));

    // 已存在但未加入的 peer 被 Grant(kNone) 后同样不可请求。
    BENTO_CHECK(control.Grant(99, ControlMode::kNone));
    BENTO_CHECK_EQ(control.mode(99), ControlMode::kNone);
    BENTO_CHECK(!control.RequestControl(99));

    // 管理路径不受影响：Grant(kInteract) 可直接抢占，并记录 actor。
    BENTO_CHECK(control.Grant(3, ControlMode::kInteract, 77));
    BENTO_CHECK_EQ(control.controller(), 3U);
    const ControlSnapshot snapshot = control.Snapshot();
    BENTO_CHECK(!snapshot.audit.empty());
    if (!snapshot.audit.empty()) {
        BENTO_CHECK_EQ(snapshot.audit.back().reason, ControlChangeReason::kGranted);
        BENTO_CHECK_EQ(snapshot.audit.back().actor, 77U);
        BENTO_CHECK_EQ(snapshot.audit.back().to, ControlMode::kInteract);
    }
    CheckInvariants(control, "grant_none");
}

/// 场景 9（P1-2）：测试内自定义策略经构造函数注入即生效，不改库源码。
void TestCustomPolicyInjection() {
    // 注入策略优先于 config.policy。
    SessionControlConfig config;
    config.policy = ControlPolicy::kFirstCome;
    config.lease_ms = 0;
    auto custom = std::make_shared<EvenPeerPreemptPolicy>();
    SessionControl control(config, custom);
    BENTO_CHECK_EQ(std::string(control.policy_name()), "test_even_preempt");
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(control.Join(2));
    BENTO_CHECK(control.Join(3));

    // 无控制器：策略不参与，直接成功。
    BENTO_CHECK(control.RequestControl(1));
    BENTO_CHECK_EQ(custom->calls(), 0);

    // 奇数请求：自定义策略拒绝。
    BENTO_CHECK(!control.RequestControl(3));
    BENTO_CHECK_EQ(control.controller(), 1U);
    BENTO_CHECK_EQ(custom->calls(), 1);
    BENTO_CHECK_EQ(custom->last_controller(), 1U);
    BENTO_CHECK_EQ(custom->last_participants(), static_cast<std::size_t>(3));

    // 偶数请求：自定义策略批准并抢占（原控制器降 kView）。
    BENTO_CHECK(control.RequestControl(2));
    BENTO_CHECK_EQ(control.controller(), 2U);
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kView);
    BENTO_CHECK_EQ(control.mode(2), ControlMode::kInteract);
    BENTO_CHECK_EQ(custom->calls(), 2);
    BENTO_CHECK_EQ(custom->last_requests(), 3U);  // 累计请求数（含本次）
    BENTO_CHECK_EQ(control.stats().preemptions, 1U);

    // 已是控制器时的幂等请求（续租）不再进入策略。
    BENTO_CHECK(control.RequestControl(2));
    BENTO_CHECK_EQ(custom->calls(), 2);

    // 未注入时按 config.policy 选择内置策略。
    SessionControlConfig first_come;
    first_come.lease_ms = 0;
    SessionControl defaulted(first_come);
    BENTO_CHECK_EQ(std::string(defaulted.policy_name()), "first_come");
    BENTO_CHECK(defaulted.Join(1));
    BENTO_CHECK(defaulted.Join(2));
    BENTO_CHECK(defaulted.RequestControl(1));
    BENTO_CHECK(!defaulted.RequestControl(2));

    SessionControlConfig preempt_config;
    preempt_config.policy = ControlPolicy::kPreempt;
    preempt_config.lease_ms = 0;
    SessionControl preempt(preempt_config);
    BENTO_CHECK_EQ(std::string(preempt.policy_name()), "preempt");
    BENTO_CHECK(preempt.Join(1));
    BENTO_CHECK(preempt.Join(2));
    BENTO_CHECK(preempt.RequestControl(1));
    BENTO_CHECK(preempt.RequestControl(2));
    BENTO_CHECK_EQ(preempt.controller(), 2U);

    // 显式注入 nullptr：回退到 config.policy 的内置策略。
    SessionControl fallback(preempt_config, nullptr);
    BENTO_CHECK_EQ(std::string(fallback.policy_name()), "preempt");

    // 内置策略工厂与接口一致。
    BENTO_CHECK_EQ(std::string(go2cpp::bento::MakeControlPolicy(ControlPolicy::kFirstCome)->name()),
                   "first_come");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::MakeControlPolicy(ControlPolicy::kPreempt)->name()),
                   "preempt");
    CheckInvariants(control, "custom_policy");
}

/// 场景 10（P1-3）：审计字段与 Snapshot().audit 最近 N 条变更历史。
void TestAuditTrail() {
    SessionControlConfig config;
    config.session_id = 4242;
    config.audit_capacity = 3;
    config.lease_ms = 0;
    SessionControl control(config);
    std::vector<ControlChange> callbacks;
    control.on_change = [&](const ControlChange& change) { callbacks.push_back(change); };

    BENTO_CHECK(control.Join(1));                              // seq 1, actor 1
    BENTO_CHECK(control.Join(2));                              // seq 2, actor 2
    BENTO_CHECK(control.RequestControl(1));                    // seq 3, actor 1
    BENTO_CHECK(control.Revoke(1, 88));                        // seq 4, actor 88
    BENTO_CHECK(control.Grant(2, ControlMode::kInteract, 99)); // seq 5, actor 99

    BENTO_CHECK_EQ(control.generation(), 5U);
    BENTO_CHECK_EQ(callbacks.size(), static_cast<std::size_t>(5));
    for (std::size_t i = 0; i < callbacks.size(); ++i) {
        BENTO_CHECK_EQ(callbacks[i].seq, static_cast<std::uint64_t>(i + 1));
        BENTO_CHECK_EQ(callbacks[i].generation, static_cast<std::uint64_t>(i + 1));
        BENTO_CHECK_EQ(callbacks[i].session_id, 4242U);
        BENTO_CHECK(callbacks[i].timestamp_ms > 0);
        if (i != 0) {
            BENTO_CHECK(callbacks[i].timestamp_ms >= callbacks[i - 1].timestamp_ms);
        }
    }
    BENTO_CHECK_EQ(callbacks[0].actor, 1U);
    BENTO_CHECK_EQ(callbacks[1].actor, 2U);
    BENTO_CHECK_EQ(callbacks[2].actor, 1U);
    BENTO_CHECK_EQ(callbacks[3].actor, 88U);
    BENTO_CHECK_EQ(callbacks[4].actor, 99U);

    // 容量 3：只保留最近 3 条（seq 3/4/5，旧→新）。
    const ControlSnapshot snapshot = control.Snapshot();
    BENTO_CHECK_EQ(snapshot.audit.size(), static_cast<std::size_t>(3));
    if (snapshot.audit.size() == 3) {
        BENTO_CHECK_EQ(snapshot.audit[0].seq, 3U);
        BENTO_CHECK_EQ(snapshot.audit[0].reason, ControlChangeReason::kRequested);
        BENTO_CHECK_EQ(snapshot.audit[0].to, ControlMode::kInteract);
        BENTO_CHECK_EQ(snapshot.audit[1].seq, 4U);
        BENTO_CHECK_EQ(snapshot.audit[1].reason, ControlChangeReason::kRevoked);
        BENTO_CHECK_EQ(snapshot.audit[1].actor, 88U);
        BENTO_CHECK_EQ(snapshot.audit[2].seq, 5U);
        BENTO_CHECK_EQ(snapshot.audit[2].reason, ControlChangeReason::kGranted);
        BENTO_CHECK_EQ(snapshot.audit[2].actor, 99U);
        BENTO_CHECK_EQ(snapshot.audit[2].controller, 2U);
        BENTO_CHECK_EQ(snapshot.audit.back().seq, callbacks.back().seq);
    }
    // ToJson 保持广播精简：不含审计历史。
    BENTO_CHECK(control.Snapshot().ToJson().find("\"audit\"") == std::string::npos);

    // audit_capacity = 0：完全关闭审计。
    SessionControlConfig no_audit;
    no_audit.audit_capacity = 0;
    no_audit.lease_ms = 0;
    SessionControl silent(no_audit);
    BENTO_CHECK(silent.Join(1));
    BENTO_CHECK(silent.Snapshot().audit.empty());

    // 容量与全局代数解耦：容量 2 时仍保留最近两条（seq 2/3）。
    SessionControlConfig capped;
    capped.audit_capacity = 2;
    capped.lease_ms = 0;
    SessionControl small(capped);
    BENTO_CHECK(small.Join(1));
    BENTO_CHECK(small.Join(2));
    BENTO_CHECK(small.Join(3));
    const ControlSnapshot small_snapshot = small.Snapshot();
    BENTO_CHECK_EQ(small_snapshot.audit.size(), static_cast<std::size_t>(2));
    if (small_snapshot.audit.size() == 2) {
        BENTO_CHECK_EQ(small_snapshot.audit[0].seq, 2U);
        BENTO_CHECK_EQ(small_snapshot.audit[1].seq, 3U);
        // 未配置 session_id：审计字段保持默认 0。
        BENTO_CHECK_EQ(small_snapshot.audit.back().session_id, 0U);
    }
    CheckInvariants(control, "audit");
}

/// 场景 12（SESS-01）：撤销状态按 peer 身份持久化，Leave/断开/重连均不清除。
void TestRevokeSurvivesReconnect() {
    SessionControl control;
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(control.Join(2));
    BENTO_CHECK(control.RequestControl(1));
    BENTO_CHECK(control.Revoke(1));
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK(control.revoked(1));

    // 主动 Leave 后撤销记录仍在（参与者已不在表中，查询仍为 true）。
    BENTO_CHECK(control.Leave(1));
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kNone);
    BENTO_CHECK(control.revoked(1));

    // 重连 Join 不得清除撤销：强制 kView 且请求被拒。
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(control.revoked(1));
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kView);
    BENTO_CHECK(!control.RequestControl(1));
    BENTO_CHECK_EQ(control.controller(), 0U);

    // 断开重连同样保持。
    BENTO_CHECK(control.OnPeerDisconnected(1));
    BENTO_CHECK(control.revoked(1));
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(!control.RequestControl(1));

    // 只有显式 Grant 解除后才可重新请求。
    BENTO_CHECK(control.Grant(1));
    BENTO_CHECK(!control.revoked(1));
    BENTO_CHECK(control.RequestControl(1));
    BENTO_CHECK_EQ(control.controller(), 1U);

    // default_mode = kInteract：被撤销者重连也不得直接成为控制器。
    SessionControlConfig eager;
    eager.default_mode = ControlMode::kInteract;
    eager.lease_ms = 0;
    SessionControl direct(eager);
    BENTO_CHECK(direct.Join(5));
    BENTO_CHECK_EQ(direct.controller(), 5U);
    BENTO_CHECK(direct.Revoke(5));
    BENTO_CHECK(direct.Leave(5));
    BENTO_CHECK(direct.Join(5));  // 重连
    BENTO_CHECK_EQ(direct.controller(), 0U);
    BENTO_CHECK_EQ(direct.mode(5), ControlMode::kView);
    BENTO_CHECK(direct.revoked(5));
    BENTO_CHECK(!direct.RequestControl(5));

    // Revoke + Grant(kNone)：显式授权解除撤销，但权限仍是 kNone → 请求继续被拒。
    SessionControl combined;
    BENTO_CHECK(combined.Join(1));
    BENTO_CHECK(combined.Revoke(1));  // 非控制器：只标记禁入
    BENTO_CHECK(combined.revoked(1));
    BENTO_CHECK(combined.Grant(1, ControlMode::kNone));
    BENTO_CHECK(!combined.revoked(1));
    BENTO_CHECK_EQ(combined.mode(1), ControlMode::kNone);
    BENTO_CHECK(!combined.RequestControl(1));
    BENTO_CHECK(combined.Grant(1));
    BENTO_CHECK(combined.RequestControl(1));

    // Snapshot 反映身份级撤销：重连后的参与者带 revoked 标志。
    SessionControl view;
    BENTO_CHECK(view.Join(7));
    BENTO_CHECK(view.Revoke(7));  // 非控制器：仅标记
    BENTO_CHECK(view.Leave(7));
    BENTO_CHECK(view.Join(7));
    const ControlSnapshot snapshot = view.Snapshot();
    BENTO_CHECK_EQ(snapshot.participants.size(), static_cast<std::size_t>(1));
    if (!snapshot.participants.empty()) {
        BENTO_CHECK(snapshot.participants.front().revoked);
    }
    CheckInvariants(control, "revoke_reconnect");
}

/// 场景 11：随机操作序列，持续验证唯一控制器不变式。
void TestRandomizedOperations() {
    SessionControlConfig config;
    config.lease_ms = 30;
    config.policy = ControlPolicy::kFirstCome;
    SessionControl control(config);
    for (std::uint32_t peer_id = 1; peer_id <= 8; ++peer_id) {
        BENTO_CHECK(control.Join(peer_id));
    }
    std::mt19937 rng(20261027);
    std::uint64_t grant_count = 0;
    std::uint64_t request_count = 0;
    for (int step = 0; step < 4000; ++step) {
        const std::uint32_t peer_id = 1 + (rng() % 8U);
        switch (rng() % 8U) {
            case 0: {
                (void)control.Join(peer_id);
                break;
            }
            case 1: {
                (void)control.Leave(peer_id);
                break;
            }
            case 2: {
                (void)control.OnPeerDisconnected(peer_id);
                break;
            }
            case 3: {
                if (control.RequestControl(peer_id)) {
                    ++request_count;
                }
                break;
            }
            case 4: {
                (void)control.Revoke(peer_id);
                break;
            }
            case 5: {
                if (control.Grant(peer_id, (rng() % 2U) == 0 ? ControlMode::kView
                                                             : ControlMode::kInteract)) {
                    ++grant_count;
                }
                break;
            }
            case 6: {
                (void)control.Renew(peer_id);
                break;
            }
            default: {
                (void)control.Tick();
                break;
            }
        }
        CheckInvariants(control, "random");
        // 参与者表不会超过上限，且不变量在每一步都成立。
        BENTO_CHECK(control.participants().size() <= 8);
        // 审计序号与代数、统计保持守恒。
        BENTO_CHECK_EQ(control.generation(), control.stats().changes);
    }
    BENTO_CHECK(grant_count > 0);
    BENTO_CHECK(request_count > 0);
    CheckInvariants(control, "random-final");
    std::printf("[INFO] randomized: steps=4000 grants=%llu requests=%llu controller=%u\n",
                static_cast<unsigned long long>(grant_count),
                static_cast<unsigned long long>(request_count), control.controller());
}

/// 场景 14：`ControlChange::controller` 契约——所有事件都携带「变化后的唯一控制器」。
void TestEventControllerContract() {
    // 1) Grant(kInteract) 管理路径抢占：两条事件都指向新控制器。
    {
        SessionControl control;
        BENTO_CHECK(control.Join(1));
        BENTO_CHECK(control.Join(2));
        BENTO_CHECK(control.RequestControl(1));
        std::vector<ControlChange> events;
        control.on_change = [&](const ControlChange& change) { events.push_back(change); };
        BENTO_CHECK(control.Grant(2, ControlMode::kInteract));
        BENTO_CHECK_EQ(events.size(), static_cast<std::size_t>(2));
        if (events.size() == 2) {
            BENTO_CHECK_EQ(events[0].reason, ControlChangeReason::kPreempted);
            BENTO_CHECK_EQ(events[0].peer_id, 1U);
            BENTO_CHECK_EQ(events[0].controller, 2U);
            BENTO_CHECK_EQ(events[1].reason, ControlChangeReason::kGranted);
            BENTO_CHECK_EQ(events[1].peer_id, 2U);
            BENTO_CHECK_EQ(events[1].controller, 2U);
            BENTO_CHECK(events[1].generation > events[0].generation);
        }
    }
    // 2) Revoke(peer) 撤销控制器：事件 controller 必须为 0。
    {
        SessionControl control;
        BENTO_CHECK(control.Join(1));
        BENTO_CHECK(control.RequestControl(1));
        std::vector<ControlChange> events;
        control.on_change = [&](const ControlChange& change) { events.push_back(change); };
        BENTO_CHECK(control.Revoke(1));
        BENTO_CHECK_EQ(events.size(), static_cast<std::size_t>(1));
        if (events.size() == 1) {
            BENTO_CHECK_EQ(events[0].reason, ControlChangeReason::kRevoked);
            BENTO_CHECK_EQ(events[0].controller, 0U);
        }
    }
    // 3) 控制器 Leave / 断开：事件 controller 为 0。
    {
        SessionControl control;
        BENTO_CHECK(control.Join(1));
        BENTO_CHECK(control.RequestControl(1));
        std::vector<ControlChange> events;
        control.on_change = [&](const ControlChange& change) { events.push_back(change); };
        BENTO_CHECK(control.OnPeerDisconnected(1));
        BENTO_CHECK_EQ(events.size(), static_cast<std::size_t>(1));
        if (events.size() == 1) {
            BENTO_CHECK_EQ(events[0].reason, ControlChangeReason::kDisconnected);
            BENTO_CHECK_EQ(events[0].controller, 0U);
        }
        BENTO_CHECK(control.Join(2));
        BENTO_CHECK(control.RequestControl(2));
        events.clear();
        BENTO_CHECK(control.Leave(2));
        BENTO_CHECK_EQ(events.size(), static_cast<std::size_t>(1));
        if (events.size() == 1) {
            BENTO_CHECK_EQ(events[0].reason, ControlChangeReason::kLeft);
            BENTO_CHECK_EQ(events[0].controller, 0U);
        }
    }
}

/// 场景 15：`Renew` 顺延租约——旧 deadline 不回收、新 deadline 恰好回收。
void TestRenewExtendsLease() {
    KnownLeaseControl lease = MakeLeaseControl(500);
    BENTO_CHECK(lease.control != nullptr);
    if (lease.control == nullptr) {
        return;
    }
    SessionControl& control = *lease.control;
    const std::uint64_t granted_at = lease.deadline - 500;
    std::uint64_t renewed_at = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        renewed_at = go2cpp::bento::NowMs();
        if (renewed_at > granted_at) {
            break;
        }
        // 等待跨毫秒：Renew 的 deadline 是 now + lease，必须严格晚于旧 deadline
        // 才能断言「旧 deadline 不回收」。
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // 必须等到时钟前进，才能保证 Renew 后的 deadline 严格更晚。
    BENTO_CHECK(renewed_at > granted_at);
    if (renewed_at <= granted_at) {
        return;
    }
    BENTO_CHECK(control.Renew(1));
    // 旧 deadline：新 deadline = renewed_at + 500 > 旧 deadline，不得回收。
    BENTO_CHECK(!control.Tick(lease.deadline));
    BENTO_CHECK_EQ(control.controller(), 1U);
    BENTO_CHECK_EQ(control.stats().lease_expirations, 0U);
    // 新 deadline 恰好回收（now >= deadline）。
    BENTO_CHECK(control.Tick(renewed_at + 500));
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kView);
    BENTO_CHECK_EQ(control.stats().lease_expirations, 1U);
    CheckInvariants(control, "renew");
}

/// 场景 16：16 线程并发 Grant(kInteract)/RequestControl——唯一控制器、
/// 成功计数与统计守恒。
void TestConcurrentGrantAndRequest() {
    SessionControlConfig config;
    config.policy = ControlPolicy::kPreempt;
    config.lease_ms = 0;
    SessionControl control(config);
    constexpr std::uint32_t kPeers = 16;
    for (std::uint32_t peer_id = 1; peer_id <= kPeers; ++peer_id) {
        BENTO_CHECK(control.Join(peer_id));
    }
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<long> grant_wins{0};
    std::atomic<long> request_wins{0};
    std::vector<std::thread> threads;
    threads.reserve(kPeers);
    for (std::uint32_t thread = 0; thread < kPeers; ++thread) {
        threads.emplace_back([&, thread]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const std::uint32_t peer_id = thread + 1;
            for (int round = 0; round < 500; ++round) {
                if ((round & 1) == 0) {
                    if (control.Grant(peer_id, ControlMode::kInteract)) {
                        grant_wins.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (control.RequestControl(peer_id)) {
                    request_wins.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < static_cast<int>(kPeers)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    // 管理路径不受策略限制，Preempt 策略下所有请求也应成功。
    BENTO_CHECK_EQ(grant_wins.load(), static_cast<long>(kPeers) * 250);
    BENTO_CHECK_EQ(request_wins.load(), static_cast<long>(kPeers) * 250);
    BENTO_CHECK_EQ(CountInteract(control), 1);
    BENTO_CHECK(control.controller() != 0);
    const SessionControlStats stats = control.stats();
    BENTO_CHECK_EQ(stats.grants, static_cast<std::uint64_t>(grant_wins.load()));
    BENTO_CHECK_EQ(stats.requests, static_cast<std::uint64_t>(request_wins.load()));
    BENTO_CHECK_EQ(stats.changes, control.generation());
    CheckInvariants(control, "concurrent-grant");
    std::printf("[INFO] concurrent grant/request: %ld grants, %ld requests, controller=%u\n",
                grant_wins.load(), request_wins.load(), control.controller());
}

/// 场景 17：10 万次混合操作压测（Grant/Revoke/Request/Tick/Leave+Join）。
void TestMixedStress100k() {
    SessionControlConfig config;
    config.lease_ms = 0;
    config.policy = ControlPolicy::kPreempt;
    config.audit_capacity = 64;
    SessionControl control(config);
    for (std::uint32_t peer_id = 1; peer_id <= 16; ++peer_id) {
        BENTO_CHECK(control.Join(peer_id));
    }
    constexpr int kOps = 100000;
    std::mt19937 rng(20261029);
    const auto begin = std::chrono::steady_clock::now();
    for (int op = 0; op < kOps; ++op) {
        const std::uint32_t peer_id = 1 + (rng() % 16U);
        switch (rng() % 6U) {
            case 0:
                (void)control.Grant(peer_id, ControlMode::kView);
                break;
            case 1:
                (void)control.Grant(peer_id, ControlMode::kInteract);
                break;
            case 2:
                (void)control.Revoke(peer_id);
                break;
            case 3:
                (void)control.RequestControl(peer_id);
                break;
            case 4:
                (void)control.Leave(peer_id);
                (void)control.Join(peer_id);
                break;
            default:
                (void)control.Tick();
                break;
        }
        if ((op % 1000) == 0) {
            CheckInvariants(control, "stress-100k");
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - begin).count();
    BENTO_CHECK_EQ(control.generation(), control.stats().changes);
    BENTO_CHECK(control.participants().size() <= 16);
    CheckInvariants(control, "stress-100k-final");
    std::printf("[INFO] stress: ops=%d elapsed=%.1fms throughput=%.2fM ops/s\n", kOps,
                elapsed_ms, static_cast<double>(kOps) / elapsed_ms / 1000.0);
}

/// 场景 18：1000 条随机序列（60 步/序列），随机配置 + 随机操作，
/// 每 10 步验证唯一控制器、参与者上限与 generation/changes 守恒。
void TestRandomSequences1000() {
    static constexpr int kSequences = 1000;
    static constexpr int kSteps = 60;
    static constexpr std::array<std::uint32_t, 3> kLeases = {0, 30, 1000};
    long long total_steps = 0;
    for (int sequence = 0; sequence < kSequences; ++sequence) {
        std::mt19937 rng(20261029U + static_cast<std::uint32_t>(sequence));
        SessionControlConfig config;
        config.lease_ms = kLeases[rng() % kLeases.size()];
        config.audit_capacity = (rng() % 2U) == 0 ? 0 : 8;
        config.max_participants = (rng() % 2U) == 0 ? 8 : 64;
        config.policy = (rng() % 2U) == 0 ? ControlPolicy::kFirstCome : ControlPolicy::kPreempt;
        config.default_mode =
            (rng() % 2U) == 0 ? ControlMode::kView : ControlMode::kInteract;
        SessionControl control(config);
        const std::uint32_t initial = 1 + (rng() % 6U);
        for (std::uint32_t peer_id = 1; peer_id <= initial; ++peer_id) {
            (void)control.Join(peer_id);
        }
        for (int step = 0; step < kSteps; ++step) {
            const std::uint32_t peer_id = 1 + (rng() % 8U);
            switch (rng() % 9U) {
                case 0:
                    (void)control.Join(peer_id);
                    break;
                case 1:
                    (void)control.Leave(peer_id);
                    break;
                case 2:
                    (void)control.OnPeerDisconnected(peer_id);
                    break;
                case 3:
                    (void)control.RequestControl(peer_id);
                    break;
                case 4:
                    (void)control.Revoke(peer_id);
                    break;
                case 5:
                    (void)control.Revoke();
                    break;
                case 6: {
                    const ControlMode mode = (rng() % 3U) == 0 ? ControlMode::kNone
                                                               : ControlMode::kInteract;
                    (void)control.Grant(peer_id, mode);
                    break;
                }
                case 7:
                    (void)control.Renew(peer_id);
                    break;
                default:
                    (void)control.Tick((rng() % 2U) == 0 ? 0 : UINT64_MAX);
                    break;
            }
            if ((step % 10) == 0) {
                CheckInvariants(control, "random-1000");
                BENTO_CHECK(control.participants().size() <= config.max_participants);
                BENTO_CHECK_EQ(control.generation(), control.stats().changes);
            }
            ++total_steps;
        }
        CheckInvariants(control, "random-1000-final");
    }
    BENTO_CHECK_EQ(total_steps, static_cast<long long>(kSequences) * kSteps);
    std::printf("[INFO] random sequences: %d sequences x %d steps, total=%lld\n", kSequences,
                kSteps, total_steps);
}

}  // namespace

int main() {
    TestConcurrentRequests();
    TestDisconnectReclaim();
    TestRevokeBlocksRequest();
    TestLeaseExpiry();
    TestLeaseBoundaryInjectedClock();
    TestPreemptPolicy();
    TestCallbacksAndSnapshot();
    TestValidationAndConfig();
    TestGrantNoneBlocksRequest();
    TestCustomPolicyInjection();
    TestAuditTrail();
    TestRevokeSurvivesReconnect();
    TestRandomizedOperations();
    TestEventControllerContract();
    TestRenewExtendsLease();
    TestConcurrentGrantAndRequest();
    TestMixedStress100k();
    TestRandomSequences1000();
    return bento_test::Finish("test_session_control");
}
