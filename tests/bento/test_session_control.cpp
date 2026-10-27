/**
 * @file test_session_control.cpp
 * @brief 控制权仲裁测试：并发请求、唯一控制器、断开/租约回收、Revoke 与随机压力。
 * @details 覆盖场景矩阵 3.4 / 7.1–7.3（FR-03 / FR-06 / RISK-024 / AX-036）：
 *          - 场景 1：16 个 peer 并发 RequestControl，只有 1 个拿到 kInteract。
 *          - 场景 2：控制器断开（OnPeerDisconnected）自动回收，其他人可接手。
 *          - 场景 3：Revoke 后该 peer 请求失败，Grant 解除后才恢复。
 *          - 场景 4：租约过期自动回收。
 *          - 场景 5：可抢占策略下后来者抢占，原控制器降为 kView。
 *          - 场景 6：变更回调顺序/代数、Snapshot JSON、统计计数。
 *          - 场景 7：参数与边界（0、重复 Join、上限、Grant 自动创建）。
 *          - 场景 8：随机操作序列 4000 步，验证「最多一个 Interact」不变式。
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/session_control.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::ControlChange;
using go2cpp::bento::ControlChangeReason;
using go2cpp::bento::ControlMode;
using go2cpp::bento::ControlPolicy;
using go2cpp::bento::ControlSnapshot;
using go2cpp::bento::SessionControl;
using go2cpp::bento::SessionControlConfig;

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
    } else {
        BENTO_CHECK_EQ(interact, 0);
    }
    if (interact > 1) {
        std::fprintf(stderr, "[FAIL] invariant broken at %s: interact=%d\n", where, interact);
    }
}

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

/// 场景 4：租约过期后自动回收为 View。
void TestLeaseExpiry() {
    SessionControlConfig config;
    config.lease_ms = 100;
    SessionControl control(config);
    BENTO_CHECK(control.Join(1));
    BENTO_CHECK(control.Join(2));
    BENTO_CHECK(control.RequestControl(1));
    BENTO_CHECK_EQ(control.controller(), 1U);
    // 未过期时 Tick 不回收。
    BENTO_CHECK(!control.Tick());
    BENTO_CHECK_EQ(control.controller(), 1U);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    BENTO_CHECK(control.Tick());
    BENTO_CHECK_EQ(control.controller(), 0U);
    BENTO_CHECK_EQ(control.mode(1), ControlMode::kView);
    BENTO_CHECK_EQ(control.stats().lease_expirations, 1U);
    // 过期后其他 peer 可请求。
    BENTO_CHECK(control.RequestControl(2));
    BENTO_CHECK_EQ(control.controller(), 2U);
    CheckInvariants(control, "lease");
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

    // 模式/原因名。
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlModeName(ControlMode::kView)), "view");
    BENTO_CHECK_EQ(std::string(go2cpp::bento::ControlChangeReasonName(
                        ControlChangeReason::kLeaseExpired)),
                   "lease_expired");
}

/// 场景 8：随机操作序列，持续验证唯一控制器不变式。
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
    }
    BENTO_CHECK(grant_count > 0);
    BENTO_CHECK(request_count > 0);
    CheckInvariants(control, "random-final");
    std::printf("[INFO] randomized: steps=4000 grants=%llu requests=%llu controller=%u\n",
                static_cast<unsigned long long>(grant_count),
                static_cast<unsigned long long>(request_count), control.controller());
}

}  // namespace

int main() {
    TestConcurrentRequests();
    TestDisconnectReclaim();
    TestRevokeBlocksRequest();
    TestLeaseExpiry();
    TestPreemptPolicy();
    TestCallbacksAndSnapshot();
    TestValidationAndConfig();
    TestRandomizedOperations();
    return bento_test::Finish("test_session_control");
}
