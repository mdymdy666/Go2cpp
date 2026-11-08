/**
 * @file test_ice_switchover.cpp
 * @brief 候选对热切换（连接迁移）测试：切换、回切、冷却期与在线验证。
 * @details 场景覆盖（纯逻辑层用注入时钟驱动，纳秒精度、无 flake）：
 *          1. 当前选定对停止收包 → 切到最优可写对（断言选定对变了、代数 +1）；
 *          2. 原对恢复 → 默认**不回切**（避免抖动）；
 *          3. 显式打开 `allow_switch_back` → 回切到优先级更高的旧对；
 *          4. 切换冷却期 → 冷期内只报告 `kCooldownBlocked`，不反复切；
 *          5. 没有可写备用对 → `kNoCandidate`，保持当前对不断链；
 *          6. 探测调度：间隔与失败对降频；
 *          7. 快照/查询 API（writable、receiving、排序、RTT）；
 *          8. 真实双通道在线验证：切断当前路径的入向收包 → 触发热切换，
 *             断言 `SessionId()` 不变、`state()` 仍是 kConnected、出向仍可达。
 *
 *          运行：`.build-p2p-ice/p2p_test_ice_switchover`
 * @author go2cpp contributors
 * @date 2026-11-08
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/ice_pair.hpp"
#include "go2cpp/p2p/p2p_channel.hpp"
#include "go2cpp/p2p/signaling.hpp"
#include "go2cpp/p2p/transport.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;

using go2cpp::p2p::Candidate;
using go2cpp::p2p::DatagramTransport;
using go2cpp::p2p::Endpoint;
using go2cpp::p2p::IcePairController;
using go2cpp::p2p::IcePairSnapshot;
using go2cpp::p2p::IcePairState;
using go2cpp::p2p::IceSwitchDecision;
using go2cpp::p2p::IceSwitchDecisionName;
using go2cpp::p2p::IceSwitchEvent;
using go2cpp::p2p::IceSwitchoverConfig;
using go2cpp::p2p::LoopbackSignaling;
using go2cpp::p2p::P2PChannel;
using go2cpp::p2p::P2PConfig;
using go2cpp::p2p::P2PState;
using go2cpp::p2p::UdpTransport;
using go2cpp::p2p::UdpTransportConfig;
using Clock = std::chrono::steady_clock;

/// 1 毫秒对应的纳秒数（测试里的时间轴单位）。
constexpr std::int64_t kMs = 1000 * 1000;
/// 时间轴起点：0 在控制器里是「从未」哨兵，测试必须避开。
constexpr std::int64_t kT0 = 1 * kMs;

Endpoint MakeEndpoint(std::uint16_t port) {
    const auto parsed = Endpoint::Parse("127.0.0.1:" + std::to_string(port));
    return parsed.has_value() ? *parsed : Endpoint();
}

/// 选定对远端；未选定时返回无效端点（让断言自然失败而不是解空 optional）。
Endpoint SelectedRemote(const IcePairController& ice, std::int64_t now_ns) {
    const auto selected = ice.Selected(now_ns);
    return selected.has_value() ? selected->remote : Endpoint();
}

/// 某条对的快照；不存在时返回默认快照（writable/receiving 均为 false）。
IcePairSnapshot SnapshotOf(const IcePairController& ice, const Endpoint& remote,
                           std::int64_t now_ns) {
    const auto pair = ice.Pair(remote, now_ns);
    return pair.has_value() ? *pair : IcePairSnapshot();
}

/// 填充「三对都可写、high 被选定」的控制器状态（控制器不可拷贝，故用输出参数）。
void FillThreePairController(IcePairController* ice, Endpoint* high, Endpoint* middle,
                             Endpoint* low) {
    *high = MakeEndpoint(10001);
    *middle = MakeEndpoint(10002);
    *low = MakeEndpoint(10003);
    const Endpoint local = MakeEndpoint(9000);
    (void)ice->AddPair(local, *high, 900);
    (void)ice->AddPair(local, *middle, 800);
    (void)ice->AddPair(local, *low, 700);
    ice->OnCheckSucceeded(*high, kT0);
    ice->OnCheckSucceeded(*middle, kT0);
    ice->OnCheckSucceeded(*low, kT0);
    (void)ice->Select(*high, kT0);
}

std::string DecisionText(const IceSwitchEvent& event) {
    return std::string(IceSwitchDecisionName(event.decision)) + "（" + event.reason + "）";
}

/// 场景 1：当前对停止收包超过阈值 → 切到最优可写对。
void TestSwitchOnReceiveTimeout() {
    IceSwitchoverConfig config;  // no_receive_timeout=1s，默认不回切
    Endpoint high;
    Endpoint middle;
    Endpoint low;
    IcePairController ice(config);
    FillThreePairController(&ice, &high, &middle, &low);

    P2P_CHECK(ice.size() == 3);
    P2P_CHECK(ice.generation() == 0);

    // 重复注册同一 (local, remote) 必须是幂等失败。
    P2P_CHECK(!ice.AddPair(MakeEndpoint(9000), high, 900));
    P2P_CHECK(ice.size() == 3);

    const IcePairSnapshot selected = SnapshotOf(ice, high, kT0);
    P2P_CHECK(selected.remote == high);
    P2P_CHECK(selected.writable);
    P2P_CHECK(selected.receiving);
    P2P_CHECK(selected.selected);
    P2P_CHECK(SelectedRemote(ice, kT0) == high);

    // 阈值内：判定仍在收，不切换。
    IceSwitchEvent event = ice.Evaluate(kT0 + 500 * kMs);
    P2P_CHECK(event.decision == IceSwitchDecision::kNone);
    P2P_CHECK(ice.generation() == 0);
    P2P_CHECK(SnapshotOf(ice, high, kT0 + 500 * kMs).receiving);

    // 超过 1s 没有任何包 → 切到优先级最高的可写对（middle）。
    P2P_CHECK(!SnapshotOf(ice, high, kT0 + 1500 * kMs).receiving);
    event = ice.Evaluate(kT0 + 1500 * kMs);
    P2P_CHECK_MSG(event.decision == IceSwitchDecision::kSwitch,
                  ("期望 switch，实际 " + DecisionText(event)).c_str());
    P2P_CHECK(event.from == high);
    P2P_CHECK(event.to == middle);
    P2P_CHECK(event.generation == 1);
    P2P_CHECK(ice.generation() == 1);
    P2P_CHECK(SelectedRemote(ice, kT0 + 1500 * kMs) == middle);
    P2P_CHECK(SnapshotOf(ice, middle, kT0 + 1500 * kMs).selected);
    P2P_CHECK(!SnapshotOf(ice, high, kT0 + 1500 * kMs).selected);

    // 新对有宽限期：紧接着再评估不能立刻又切走。
    const IceSwitchEvent again = ice.Evaluate(kT0 + 1600 * kMs);
    P2P_CHECK(again.decision == IceSwitchDecision::kNone);
    P2P_CHECK(ice.generation() == 1);
}

/// 场景 2：原对恢复后，默认不回切。
void TestNoSwitchBackByDefault() {
    IceSwitchoverConfig config;
    P2P_CHECK_MSG(!config.allow_switch_back, "默认必须是「不回切」");
    Endpoint high;
    Endpoint middle;
    Endpoint low;
    IcePairController ice(config);
    FillThreePairController(&ice, &high, &middle, &low);

    const std::int64_t switch_at = kT0 + 1500 * kMs;
    const IceSwitchEvent switched = ice.Evaluate(switch_at);
    P2P_CHECK(switched.decision == IceSwitchDecision::kSwitch);
    P2P_CHECK(switched.to == middle);

    // 原对（high，优先级更高）恢复：既 writable 又 receiving。
    const std::int64_t recovered_at = switch_at + 4100 * kMs;
    ice.OnCheckSucceeded(high, recovered_at);
    ice.OnPacketReceived(middle, recovered_at);  // 当前对仍然健康
    P2P_CHECK(SnapshotOf(ice, high, recovered_at + 100 * kMs).writable);
    P2P_CHECK(SnapshotOf(ice, high, recovered_at + 100 * kMs).receiving);

    // 冷却期早已过去（4.1s > 2s），但默认不允许回切 → 必须保持 middle。
    const IceSwitchEvent event = ice.Evaluate(recovered_at + 100 * kMs);
    P2P_CHECK_MSG(event.decision == IceSwitchDecision::kNone,
                  ("默认不回切，实际 " + DecisionText(event)).c_str());
    P2P_CHECK(ice.generation() == 1);
    P2P_CHECK(SelectedRemote(ice, recovered_at + 100 * kMs) == middle);
}

/// 场景 3：显式允许回切 → 原对恢复且更优时切回去。
void TestSwitchBackWhenEnabled() {
    IceSwitchoverConfig config;
    config.allow_switch_back = true;
    Endpoint high;
    Endpoint middle;
    Endpoint low;
    IcePairController ice(config);
    FillThreePairController(&ice, &high, &middle, &low);

    const std::int64_t switch_at = kT0 + 1500 * kMs;
    P2P_CHECK(ice.Evaluate(switch_at).decision == IceSwitchDecision::kSwitch);
    P2P_CHECK(ice.generation() == 1);

    const std::int64_t recovered_at = switch_at + 4100 * kMs;
    ice.OnCheckSucceeded(high, recovered_at);
    ice.OnPacketReceived(middle, recovered_at);
    const IceSwitchEvent event = ice.Evaluate(recovered_at + 100 * kMs);
    P2P_CHECK_MSG(event.decision == IceSwitchDecision::kSwitchBack,
                  ("期望 switch-back，实际 " + DecisionText(event)).c_str());
    P2P_CHECK(event.to == high);
    P2P_CHECK(event.generation == 2);
    P2P_CHECK(SelectedRemote(ice, recovered_at + 100 * kMs) == high);
    P2P_CHECK(ice.generation() == 2);
}

/// 场景 4：冷却期内不反复切换。
void TestSwitchCooldown() {
    IceSwitchoverConfig config;
    config.no_receive_timeout = 100ms;
    config.switch_cooldown = 5000ms;
    Endpoint high;
    Endpoint middle;
    Endpoint low;
    IcePairController ice(config);
    FillThreePairController(&ice, &high, &middle, &low);

    // 第一次切换：high 超过 100ms 没收到包。
    const std::int64_t switch_at = kT0 + 200 * kMs;
    const IceSwitchEvent first = ice.Evaluate(switch_at);
    P2P_CHECK(first.decision == IceSwitchDecision::kSwitch);
    P2P_CHECK(first.to == middle);
    P2P_CHECK(ice.generation() == 1);

    // 切换瞬间新对还没收过包，但宽限期保证不会立刻再切。
    P2P_CHECK(ice.Evaluate(switch_at + 50 * kMs).decision == IceSwitchDecision::kNone);
    P2P_CHECK(ice.generation() == 1);

    // 冷期内且当前对也失效 → 只能报告冷却期阻塞，选定对不许变。
    const IceSwitchEvent blocked = ice.Evaluate(switch_at + 200 * kMs);
    P2P_CHECK_MSG(blocked.decision == IceSwitchDecision::kCooldownBlocked,
                  ("期望 cooldown-blocked，实际 " + DecisionText(blocked)).c_str());
    P2P_CHECK(SelectedRemote(ice, switch_at + 200 * kMs) == middle);
    P2P_CHECK(ice.generation() == 1);

    // 同一冷期内反复评估：仍然不切（防抖动）。
    for (int round = 1; round <= 5; ++round) {
        const IceSwitchEvent repeated = ice.Evaluate(switch_at + 200 * kMs * round);
        P2P_CHECK(repeated.decision == IceSwitchDecision::kCooldownBlocked);
        P2P_CHECK(ice.generation() == 1);
    }

    // 冷期（5s）过后才允许切回 high。
    const IceSwitchEvent after_cooldown = ice.Evaluate(switch_at + 5100 * kMs);
    P2P_CHECK(after_cooldown.decision == IceSwitchDecision::kSwitch);
    P2P_CHECK(after_cooldown.to == high);
    P2P_CHECK(ice.generation() == 2);
}

/// 场景 5：当前对已死但没有可写备用对 → 保持不动（不断链）。
void TestNoCandidateKeepsCurrent() {
    IceSwitchoverConfig config;
    config.no_receive_timeout = 100ms;
    IcePairController ice(config);
    const Endpoint local = MakeEndpoint(9000);
    const Endpoint only = MakeEndpoint(10001);
    (void)ice.AddPair(local, only, 900);
    ice.OnCheckSucceeded(only, kT0);
    (void)ice.Select(only, kT0);

    const IceSwitchEvent event = ice.Evaluate(kT0 + 500 * kMs);
    P2P_CHECK(event.decision == IceSwitchDecision::kNoCandidate);
    P2P_CHECK(SelectedRemote(ice, kT0 + 500 * kMs) == only);
    P2P_CHECK(ice.generation() == 0);

    // 关闭功能时任何评估都不产生动作。
    IceSwitchoverConfig disabled;
    disabled.enabled = false;
    IcePairController off(disabled);
    (void)off.AddPair(local, only, 900);
    off.OnCheckSucceeded(only, kT0);
    (void)off.Select(only, kT0);
    const IceSwitchEvent none = off.Evaluate(kT0 + 100 * kMs);
    P2P_CHECK(none.decision == IceSwitchDecision::kNone);
    P2P_CHECK(off.generation() == 0);
}

/// 场景 6：探测调度（间隔 + 失败对降频）。
void TestProbeScheduling() {
    IceSwitchoverConfig config;
    config.probe_interval = 500ms;
    config.max_check_failures = 3;
    config.failed_probe_backoff = 2;
    IcePairController ice(config);
    const Endpoint local = MakeEndpoint(9000);
    const Endpoint high = MakeEndpoint(10001);
    const Endpoint middle = MakeEndpoint(10002);
    const Endpoint low = MakeEndpoint(10003);
    (void)ice.AddPair(local, high, 900);
    (void)ice.AddPair(local, middle, 800);
    (void)ice.AddPair(local, low, 700);
    // 只有 high/middle 成功过；low 从未成功，用来验证「连续失败 → 降频」。
    // 成功过的对不会因为单次失败降级（见 OnCheckFailed 的注释），所以这里
    // 不能复用「三对都成功」的脚手架。
    ice.OnCheckSucceeded(high, kT0);
    ice.OnCheckSucceeded(middle, kT0);
    (void)ice.Select(high, kT0);

    // 从未探测过 → 立刻全部放行（连接后第一轮就刷新所有对）。
    std::vector<Endpoint> due = ice.DueProbeTargets(kT0);
    P2P_CHECK(due.size() == 3);
    P2P_CHECK(due.front() == high);  // 按优先级降序
    for (const Endpoint& target : due) {
        ice.OnCheckSent(target, kT0);
    }
    P2P_CHECK(ice.DueProbeTargets(kT0 + 400 * kMs).empty());
    P2P_CHECK(ice.DueProbeTargets(kT0 + 500 * kMs).size() == 3);

    // low 连续失败 3 次 → failed → 探测间隔翻倍。
    for (int round = 0; round < 3; ++round) {
        ice.OnCheckFailed(low, kT0 + 500 * kMs);
    }
    P2P_CHECK(SnapshotOf(ice, low, kT0 + 500 * kMs).state == IcePairState::kFailed);
    P2P_CHECK(SnapshotOf(ice, low, kT0 + 500 * kMs).checks_failed == 3);
    P2P_CHECK(!SnapshotOf(ice, low, kT0 + 500 * kMs).writable);
    // 500ms 时 high/middle 到期（500ms 间隔），failed 的 low 未到期（1000ms），
    // 这就是「失败对降频」的直接证据。
    const std::vector<Endpoint> at_500 = ice.DueProbeTargets(kT0 + 500 * kMs);
    P2P_CHECK(at_500.size() == 2);
    P2P_CHECK(std::find(at_500.begin(), at_500.end(), low) == at_500.end());
    for (const Endpoint& target : at_500) {
        ice.OnCheckSent(target, kT0 + 500 * kMs);
    }
    // 1000ms 时 low 的降频窗口也到了，重新进入探测集合。
    const std::vector<Endpoint> at_1000 = ice.DueProbeTargets(kT0 + 1000 * kMs);
    P2P_CHECK(at_1000.size() == 3);
    P2P_CHECK(std::find(at_1000.begin(), at_1000.end(), low) != at_1000.end());
}

/// 场景 7：快照与查询 API。
void TestSnapshotAndQueries() {
    IceSwitchoverConfig config;
    Endpoint high;
    Endpoint middle;
    Endpoint low;
    IcePairController ice(config);
    FillThreePairController(&ice, &high, &middle, &low);

    const std::vector<IcePairSnapshot> snapshot = ice.Snapshot(kT0);
    P2P_CHECK(snapshot.size() == 3);
    P2P_CHECK(snapshot[0].remote == high);
    P2P_CHECK(snapshot[1].remote == middle);
    P2P_CHECK(snapshot[2].remote == low);
    P2P_CHECK(snapshot[0].priority > snapshot[1].priority);
    P2P_CHECK(ice.IsWritable(high));
    P2P_CHECK(!ice.IsWritable(MakeEndpoint(12345)));
    P2P_CHECK(ice.IsKnownPair(high));
    P2P_CHECK(!ice.IsKnownPair(MakeEndpoint(12345)));
    P2P_CHECK(ice.HasPair(low));
    P2P_CHECK(!ice.HasPair(MakeEndpoint(12345)));
    P2P_CHECK(SnapshotOf(ice, high, kT0).checks_succeeded == 1);
    P2P_CHECK(ice.Pair(MakeEndpoint(12345), kT0) == std::nullopt);
    P2P_CHECK(ice.config().switch_cooldown == config.switch_cooldown);
    P2P_CHECK(ice.config().no_receive_timeout == config.no_receive_timeout);

    // 收包会刷新 receiving；带 RTT 的成功检查走 EWMA。
    P2P_CHECK(SnapshotOf(ice, low, kT0 + 900 * kMs).receiving);
    ice.OnCheckSucceeded(middle, kT0 + 100 * kMs, 20.0);
    P2P_CHECK(SnapshotOf(ice, middle, kT0 + 100 * kMs).rtt_ms == 20.0);
    ice.OnCheckSucceeded(middle, kT0 + 200 * kMs, 30.0);
    const double expected = 20.0 * 0.8 + 30.0 * 0.2;
    const double actual = SnapshotOf(ice, middle, kT0 + 200 * kMs).rtt_ms;
    P2P_CHECK(actual > expected - 0.001 && actual < expected + 0.001);

    // Clear 后不再有选定对，评估恒为 none。
    ice.Clear();
    P2P_CHECK(ice.size() == 0);
    P2P_CHECK(ice.Pair(high, kT0) == std::nullopt);
    P2P_CHECK(ice.DueProbeTargets(kT0).empty());
    P2P_CHECK(ice.Evaluate(kT0).decision == IceSwitchDecision::kNone);
    // generation 不因 Clear 归零（它是本连接内的单调计数）。
    P2P_CHECK(ice.generation() == 0);
}

/**
 * @brief 可按源地址丢弃入站包的传输包装（模拟「当前路径停止收包」）。
 * @details 只影响 `RecvFrom`：`SendTo` 原样转发。丢弃后用 0 超时立刻读下一个，
 *          避免「一次超时被吞掉一批包再返回」把调用方的超时预算翻倍。
 */
class DropSourceTransport final : public DatagramTransport {
public:
    explicit DropSourceTransport(std::shared_ptr<DatagramTransport> inner)
        : m_inner(std::move(inner)) {}

    bool SendTo(const Endpoint& destination, const void* data, std::size_t length) override {
        return m_inner->SendTo(destination, data, length);
    }

    int RecvFrom(Endpoint* source, void* buffer, std::size_t capacity,
                 std::chrono::milliseconds timeout) override {
        bool first = true;
        for (int attempt = 0; attempt < 256; ++attempt) {
            const std::chrono::milliseconds wait =
                first ? timeout : std::chrono::milliseconds(0);
            first = false;
            const int received = m_inner->RecvFrom(source, buffer, capacity, wait);
            if (received <= 0) {
                return received;
            }
            if (source == nullptr || !IsBlocked(*source)) {
                return received;
            }
        }
        return 0;
    }

    Endpoint LocalEndpoint() const override { return m_inner->LocalEndpoint(); }

    bool SetReceiveBufferSize(std::size_t bytes) override {
        return m_inner->SetReceiveBufferSize(bytes);
    }

    void Block(const Endpoint& source) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_blocked.push_back(source);
    }

private:
    bool IsBlocked(const Endpoint& source) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::find(m_blocked.begin(), m_blocked.end(), source) != m_blocked.end();
    }

    std::shared_ptr<DatagramTransport> m_inner;
    std::mutex m_mutex;
    std::vector<Endpoint> m_blocked;
};

std::shared_ptr<UdpTransport> MakeSocket(std::string* error) {
    UdpTransportConfig udp;
    // 绑定通配地址才能枚举出多个本地 host 候选（多路径的前提）。
    udp.bind_endpoint = Endpoint::Wildcard(0);
    auto socket = std::make_shared<UdpTransport>(udp);
    if (!socket->Start(error)) {
        return nullptr;
    }
    return socket;
}

P2PConfig MakeLiveConfig(LoopbackSignaling* signaling,
                         const std::shared_ptr<DatagramTransport>& transport) {
    P2PConfig config;
    config.signaling = signaling;
    config.transport = transport;
    config.gather_host_candidates = true;
    config.gather_loopback_candidates = true;
    config.enable_relay = false;
    config.auto_exchange_candidates = true;
    config.connect_timeout = 5000ms;
    config.gather_timeout = 500ms;
    config.check_interval = 50ms;
    config.keepalive_interval = 100ms;
    config.peer_timeout = 30000ms;
    config.ice_switchover.no_receive_timeout = 500ms;
    config.ice_switchover.switch_cooldown = 1000ms;
    config.ice_switchover.probe_interval = 100ms;
    return config;
}

bool SendReceive(P2PChannel* from, P2PChannel* to, const char* text, int attempts,
                 std::chrono::milliseconds poll) {
    const std::size_t length = std::strlen(text);
    char buffer[256] = {};
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (!from->Send(text, length)) {
            continue;
        }
        for (int round = 0; round < 10; ++round) {
            const std::size_t got = to->Recv(buffer, sizeof(buffer), poll);
            if (got == length && std::memcmp(buffer, text, length) == 0) {
                return true;
            }
        }
    }
    return false;
}

/// 场景 8：真实双通道验证「切断当前路径 → 热切换 → 会话/流不变」。
void TestLiveSwitchoverKeepsStream() {
    std::string error;
    auto socket_a = MakeSocket(&error);
    auto socket_b = MakeSocket(&error);
    if (socket_a == nullptr || socket_b == nullptr) {
        p2p_test::Skip("UDP socket 创建失败: " + error);
        return;
    }
    auto filter_a = std::make_shared<DropSourceTransport>(socket_a);

    auto signaling = LoopbackSignaling::CreatePair();
    P2PConfig config_a = MakeLiveConfig(signaling.first.get(), filter_a);
    P2PConfig config_b = MakeLiveConfig(signaling.second.get(), socket_b);
    P2PChannel a(std::move(config_a));
    P2PChannel b(std::move(config_b));

    const std::vector<Candidate> local_a = a.GatherCandidates();
    const std::vector<Candidate> local_b = b.GatherCandidates();
    if (local_a.size() < 2 || local_b.size() < 2) {
        p2p_test::Skip("本机可用单播地址不足 2 个，无法构造多路径");
        return;
    }

    std::atomic<bool> ok_a{false};
    std::atomic<bool> ok_b{false};
    std::thread connect_a([&]() { ok_a.store(a.Connect(5000ms)); });
    std::thread connect_b([&]() { ok_b.store(b.Connect(5000ms)); });
    connect_a.join();
    connect_b.join();
    P2P_CHECK_MSG(ok_a.load() && ok_b.load(),
                  ("双端连接失败: A=" + a.LastError() + " B=" + b.LastError()).c_str());
    if (!ok_a.load() || !ok_b.load()) {
        return;
    }

    const std::uint64_t session_before = a.SessionId();
    const Endpoint selected_before = a.SelectedPairRemote();
    const std::vector<IcePairSnapshot> pairs_before = a.CandidatePairs();
    std::printf("[INFO] 在线前：session=%llu selected=%s 候选对=%zu 本地候选 A=%zu B=%zu\n",
                static_cast<unsigned long long>(session_before),
                selected_before.ToString().c_str(), pairs_before.size(), local_a.size(),
                local_b.size());
    P2P_CHECK(session_before != 0);
    P2P_CHECK(selected_before.valid());
    P2P_CHECK(a.state() == P2PState::kConnected);
    P2P_CHECK(a.SwitchGeneration() == 0);
    if (pairs_before.size() < 2) {
        p2p_test::Skip("只有 1 条候选对（单路径），无法验证切换");
        return;
    }

    // 切换前先确认数据面通畅（同一会话内的基线）。
    const bool before_ok = SendReceive(&a, &b, "before-switch", 20, 20ms);
    P2P_CHECK_MSG(before_ok, "切换前数据面就不通畅，测试前提不成立");
    if (!before_ok) {
        return;
    }

    // 切断 A 的当前路径：来自选定对端的包全部丢弃（模拟 WiFi→4G 旧路径失效）。
    filter_a->Block(selected_before);
    char buffer[256] = {};
    const auto deadline = Clock::now() + 8000ms;
    while (Clock::now() < deadline && a.SwitchGeneration() == 0) {
        a.Recv(buffer, sizeof(buffer), 20ms);
        b.Recv(buffer, sizeof(buffer), 20ms);
    }

    P2P_CHECK_MSG(a.SwitchGeneration() >= 1, "当前路径停止收包后没有触发热切换");
    P2P_CHECK_MSG(a.SessionId() == session_before, "热切换不应改变会话 ID（上层流被重建）");
    P2P_CHECK_MSG(a.state() == P2PState::kConnected, "热切换后状态应保持 kConnected");
    const Endpoint selected_after = a.SelectedPairRemote();
    std::printf("[INFO] 在线后：session=%llu selected=%s 代数=%llu 切换次数=%llu 探测=%llu\n",
                static_cast<unsigned long long>(a.SessionId()),
                selected_after.ToString().c_str(),
                static_cast<unsigned long long>(a.SwitchGeneration()),
                static_cast<unsigned long long>(a.Stats().ice_pair_switches),
                static_cast<unsigned long long>(a.Stats().ice_probes_sent));
    P2P_CHECK_MSG(selected_after != selected_before, "切换后选定对没有变化");
    P2P_CHECK(selected_after.valid());
    P2P_CHECK(a.PeerEndpoint() == selected_after);
    P2P_CHECK(a.Stats().ice_probes_sent > 0);

    // 切换后出向仍可达（A 的入向被测试人为切断，这里只验证出向与同一通道）。
    P2P_CHECK_MSG(SendReceive(&a, &b, "after-switch", 40, 20ms),
                  "热切换后数据面不通（出向不可达）");

    a.Close();
    b.Close();
}

}  // namespace

int main() {
    TestSwitchOnReceiveTimeout();
    TestNoSwitchBackByDefault();
    TestSwitchBackWhenEnabled();
    TestSwitchCooldown();
    TestNoCandidateKeepsCurrent();
    TestProbeScheduling();
    TestSnapshotAndQueries();
    TestLiveSwitchoverKeepsStream();
    return p2p_test::Summary("ice_switchover");
}
