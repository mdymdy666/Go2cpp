/**
 * @file audit_session_control_stress.cpp
 * @brief session_control 模块审查用压测/随机/场景程序（不注册进 CTest）。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 与 `tests/bento/test_session_control.cpp` 的分工：
 *          - `test_session_control.cpp` 是注册进 CTest 的回归测试，只放确定性、
 *            秒级用例（含 10 万级压测与 1000 序列随机测试的缩减版）；
 *          - 本程序是**审查压测工具**，覆盖百万级 Grant/Revoke/Request/Tick、
 *            1000 个随机序列的影子模型比对、16 线程并发压测、5 个复杂场景、
 *            RSS/长稳与 valgrind 泄漏模式。
 *
 *          刻意**不** include `test_util.hpp`：该头文件会拖入 `link.hpp`，
 *          使本程序无法证明 session_control 只依赖标准库。这里只用
 *          `session_control.hpp` + 标准库，链接 `session_control.cpp` 与
 *          `protocol.cpp`（提供 `NowMs`）即可构建。
 *
 * 用法：
 * @code
 *   audit_session_control_stress --mode=all --scale=1.0        # 全量（约 60s）
 *   audit_session_control_stress --mode=bench --scale=1.0      # 只跑吞吐
 *   audit_session_control_stress --mode=random --scale=1.0     # 影子模型随机
 *   audit_session_control_stress --mode=scenarios              # 5 个复杂场景
 *   audit_session_control_stress --mode=threads                # 16 线程 fuzz
 *   audit_session_control_stress --mode=soak --seconds=30      # 长稳
 *   audit_session_control_stress --mode=all --scale=0.02       # ASan/UBSan
 * @endcode
 *
 * 退出码：0 = 全部断言通过；1 = 有失败。
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/session_control.hpp"

namespace {

using go2cpp::bento::ControlChange;
using go2cpp::bento::ControlChangeReason;
using go2cpp::bento::ControlMode;
using go2cpp::bento::ControlPolicy;
using go2cpp::bento::ControlSnapshot;
using go2cpp::bento::SessionControl;
using go2cpp::bento::SessionControlConfig;
using go2cpp::bento::SessionControlStats;

int g_failures = 0;
long g_checks = 0;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(condition)) {                                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

#define CHECK_EQ(actual, expected)                                                        \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!((actual) == (expected))) {                                                  \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual, \
                         #expected);                                                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

using Clock = std::chrono::steady_clock;

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

/// 读取本进程 RSS（KiB）；非 Linux 返回 -1。
long ReadRssKb() {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::strtol(line.c_str() + 6, nullptr, 10);
        }
    }
#endif
    return -1;
}

std::uint64_t Percentile(std::vector<std::uint64_t> samples, double ratio) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double index = ratio * static_cast<double>(samples.size() - 1);
    const std::size_t offset = static_cast<std::size_t>(index + 0.5);
    return samples[std::min(offset, samples.size() - 1)];
}

/// 规模缩放：`percent` 为百分比（100 = 全量），至少 1 次。
long Scaled(long base, long percent) {
    const long long value = static_cast<long long>(base) * percent / 100;
    return value < 1 ? 1 : static_cast<long>(value);
}

/// 统计当前 kInteract 数量。
int CountInteract(SessionControl& control) {
    int interact = 0;
    for (const std::uint32_t peer_id : control.participants()) {
        if (control.mode(peer_id) == ControlMode::kInteract) {
            ++interact;
        }
    }
    return interact;
}

/// 唯一控制器不变式：最多一个 Interact；controller 与表内模式一致。
void CheckInvariants(SessionControl& control, const char* where) {
    const int interact = CountInteract(control);
    CHECK(interact <= 1);
    const std::uint32_t controller = control.controller();
    if (controller != 0) {
        CHECK_EQ(control.mode(controller), ControlMode::kInteract);
        CHECK_EQ(interact, 1);
        // 被撤销者永远不能是控制器（Grant 解除后才可能重新请求）。
        CHECK(!control.revoked(controller));
    } else {
        CHECK_EQ(interact, 0);
    }
    if (interact > 1) {
        std::fprintf(stderr, "[FAIL] invariant broken at %s: interact=%d\n", where, interact);
    }
}

struct BenchResult {
    std::string name;
    long long total_ops = 0;
    double wall_ns = 0.0;
    double ns_per_op = 0.0;
    double ops_per_sec = 0.0;
    std::uint64_t p50_ns = 0;
    std::uint64_t p95_ns = 0;
    std::uint64_t p99_ns = 0;
    long rss_before_kb = 0;
    long rss_after_kb = 0;
};

/// 多线程计时：每线程执行 iters 次 `fn(thread_index, iter)`；
/// 只抽样 1/64 的延迟，避免计时器本身盖过被测开销。
template <typename Fn>
BenchResult RunBench(const std::string& name, int threads, long iters, Fn fn) {
    BenchResult result;
    result.name = name;
    result.total_ops = static_cast<long long>(threads) * iters;
    result.rss_before_kb = ReadRssKb();
    std::vector<std::vector<std::uint64_t>> latencies(static_cast<std::size_t>(threads));
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int thread = 0; thread < threads; ++thread) {
        workers.emplace_back([&, thread]() {
            latencies[static_cast<std::size_t>(thread)].reserve(
                static_cast<std::size_t>(iters / 64 + 1));
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (long iter = 0; iter < iters; ++iter) {
                const std::uint64_t begin = NowNs();
                fn(thread, iter);
                if ((iter & 63) == 0) {
                    latencies[static_cast<std::size_t>(thread)].push_back(NowNs() - begin);
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < threads) {
        std::this_thread::yield();
    }
    const std::uint64_t begin = NowNs();
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }
    const std::uint64_t end = NowNs();
    result.wall_ns = static_cast<double>(end - begin);
    result.ns_per_op = result.wall_ns / static_cast<double>(result.total_ops);
    result.ops_per_sec = 1e9 / std::max(result.ns_per_op, 0.001);
    result.rss_after_kb = ReadRssKb();
    std::vector<std::uint64_t> all;
    for (const auto& per_thread : latencies) {
        all.insert(all.end(), per_thread.begin(), per_thread.end());
    }
    result.p50_ns = Percentile(all, 0.50);
    result.p95_ns = Percentile(all, 0.95);
    result.p99_ns = Percentile(all, 0.99);
    std::printf(
        "[BENCH] %-42s ops=%9lld  %8.1f ns/op  %7.2f Mops/s  "
        "p50=%llu p95=%llu p99=%llu ns  rss=%ld->%ld KiB\n",
        result.name.c_str(), result.total_ops, result.ns_per_op, result.ops_per_sec / 1e6,
        static_cast<unsigned long long>(result.p50_ns),
        static_cast<unsigned long long>(result.p95_ns),
        static_cast<unsigned long long>(result.p99_ns), result.rss_before_kb,
        result.rss_after_kb);
    return result;
}

// ---------------------------------------------------------------------------
// 1. 吞吐压测
// ---------------------------------------------------------------------------

void BenchGrantViewNoop(long scale) {
    // 已存在且已是 kView：最轻写路径（无事件、无审计、无回调）。
    SessionControl control;
    for (std::uint32_t peer_id = 1; peer_id <= 8; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    const long iters = Scaled(2000000, scale);
    RunBench("Grant(view) no-op", 1, iters, [&](int, long iter) {
        (void)control.Grant(1 + static_cast<std::uint32_t>(iter % 8));
    });
}

void BenchGrantInteractRotation(long scale) {
    // 每次换一个 peer：8 次里 7 次「抢占 + 2 条事件 + 审计」，最重路径。
    SessionControl control;
    for (std::uint32_t peer_id = 1; peer_id <= 8; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    const long iters = Scaled(2000000, scale);
    RunBench("Grant(interact) rotation (preempt)", 1, iters, [&](int, long iter) {
        (void)control.Grant(1 + static_cast<std::uint32_t>(iter % 8), ControlMode::kInteract);
    });
}

void BenchRequestRotation(long scale) {
    SessionControlConfig config;
    config.policy = ControlPolicy::kPreempt;
    SessionControl control(config);
    for (std::uint32_t peer_id = 1; peer_id <= 8; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    const long iters = Scaled(2000000, scale);
    RunBench("RequestControl rotation (preempt)", 1, iters, [&](int, long iter) {
        (void)control.RequestControl(1 + static_cast<std::uint32_t>(iter % 8));
    });
}

void BenchRevokeRotation(long scale) {
    SessionControl control;
    for (std::uint32_t peer_id = 1; peer_id <= 8; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    const long iters = Scaled(2000000, scale);
    RunBench("Revoke(peer) rotation", 1, iters, [&](int, long iter) {
        const std::uint32_t peer_id = 1 + static_cast<std::uint32_t>(iter % 8);
        (void)control.Revoke(peer_id);
        (void)control.Grant(peer_id);
    });
}

void BenchTickIdle(long scale) {
    SessionControlConfig config;
    config.lease_ms = 30000;
    SessionControl control(config);
    CHECK(control.Join(1));
    CHECK(control.RequestControl(1));
    const long iters = Scaled(5000000, scale);
    RunBench("Tick(now) idle with controller", 1, iters, [&](int, long) {
        (void)control.Tick();
    });
}

void BenchReads(long scale) {
    SessionControl control;
    for (std::uint32_t peer_id = 1; peer_id <= 64; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    CHECK(control.RequestControl(1));
    const long iters = Scaled(2000000, scale);
    RunBench("mode()/is_controller() reads", 1, iters, [&](int, long iter) {
        const std::uint32_t peer_id = 1 + static_cast<std::uint32_t>(iter % 64);
        (void)control.mode(peer_id);
        (void)control.is_controller(peer_id);
    });
}

void BenchSnapshot(long scale) {
    SessionControl control;
    for (std::uint32_t peer_id = 1; peer_id <= 64; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    CHECK(control.RequestControl(1));
    const long iters = Scaled(200000, scale);
    RunBench("Snapshot() 64 participants + audit", 1, iters, [&](int, long) {
        const ControlSnapshot snapshot = control.Snapshot();
        if (snapshot.participants.size() != 64) {
            std::fprintf(stderr, "[FAIL] snapshot participants\n");
            ++g_failures;
        }
    });
}

void BenchAuditCost(long scale) {
    const long iters = Scaled(1000000, scale);
    for (std::size_t capacity : {std::size_t{0}, std::size_t{64}, std::size_t{4096}}) {
        SessionControlConfig config;
        config.audit_capacity = capacity;
        config.lease_ms = 0;
        SessionControl control(config);
        for (std::uint32_t peer_id = 1; peer_id <= 8; ++peer_id) {
            CHECK(control.Join(peer_id));
        }
        const std::string name = "Grant(interact) audit_capacity=" + std::to_string(capacity);
        RunBench(name, 1, iters, [&](int, long iter) {
            (void)control.Grant(1 + static_cast<std::uint32_t>(iter % 8),
                                ControlMode::kInteract);
        });
    }
}

void BenchThreads(long scale) {
    // 16 线程：Grant(interact) 与 RequestControl 混合，检查唯一控制器与统计守恒。
    SessionControlConfig config;
    config.policy = ControlPolicy::kPreempt;
    config.lease_ms = 0;
    config.audit_capacity = 0;  // 压锁竞争，审计另测
    SessionControl control(config);
    for (std::uint32_t peer_id = 1; peer_id <= 16; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    const long iters = Scaled(250000, scale);
    (void)RunBench("16T Grant(interact) mix", 16, iters, [&](int thread, long) {
        (void)control.Grant(1 + static_cast<std::uint32_t>(thread), ControlMode::kInteract);
    });
    (void)RunBench("16T RequestControl mix", 16, iters, [&](int thread, long) {
        (void)control.RequestControl(1 + static_cast<std::uint32_t>(thread));
    });
    CHECK_EQ(CountInteract(control), 1);
    CHECK_EQ(control.generation(), control.stats().changes);
    CheckInvariants(control, "bench-threads");
}

void RunBenchAll(long scale) {
    std::printf("== 吞吐压测（scale=%ld%%）==\n", scale);
    BenchGrantViewNoop(scale);
    BenchGrantInteractRotation(scale);
    BenchRequestRotation(scale);
    BenchRevokeRotation(scale);
    BenchTickIdle(scale);
    BenchReads(scale);
    BenchSnapshot(scale);
    BenchAuditCost(scale);
    BenchThreads(scale);
}

// ---------------------------------------------------------------------------
// 2. 随机测试：1000 序列，每步与影子模型逐项比对
// ---------------------------------------------------------------------------

struct ShadowState {
    std::map<std::uint32_t, ControlMode> modes;
    std::set<std::uint32_t> revoked;
    std::uint32_t controller = 0;
    std::uint64_t generation = 0;
    std::uint64_t requests = 0;
    std::uint64_t grants = 0;
    std::uint64_t denials = 0;
    std::uint64_t revocations = 0;
    std::uint64_t preemptions = 0;
    std::uint64_t lease_expirations = 0;
    std::uint64_t disconnects = 0;
};

/// 独立影子模型：按 API 契约重写一遍状态机，用来逐项比对。
/// @note Tick 只模拟「注入极值时钟」：`Tick(0)` 必然未过期、`Tick(UINT64_MAX)`
///       必然过期（真实 deadline 由 NowMs() 生成，不可能达到 u64 上限）。
///       精确的 deadline 边界由确定性场景覆盖。
class ShadowModel {
   public:
    explicit ShadowModel(const SessionControlConfig& config) : m_config(config) {
        if (m_config.max_participants == 0) {
            m_config.max_participants = 1;
        }
        if (m_config.default_mode == ControlMode::kNone) {
            m_config.default_mode = ControlMode::kView;
        }
    }

    bool Join(std::uint32_t peer_id) {
        if (peer_id == 0 || m_state.modes.count(peer_id) != 0 ||
            m_state.modes.size() >= m_config.max_participants) {
            return false;
        }
        ControlMode assigned = m_config.default_mode;
        const bool is_revoked = m_state.revoked.count(peer_id) != 0;
        if (assigned == ControlMode::kInteract && (is_revoked || m_state.controller != 0)) {
            assigned = ControlMode::kView;
        }
        m_state.modes[peer_id] = assigned;
        if (assigned == ControlMode::kInteract) {
            m_state.controller = peer_id;
        }
        ++m_state.generation;
        return true;
    }

    bool Leave(std::uint32_t peer_id) {
        const auto found = m_state.modes.find(peer_id);
        if (found == m_state.modes.end()) {
            return false;
        }
        if (m_state.controller == peer_id) {
            m_state.controller = 0;
        }
        m_state.modes.erase(found);
        ++m_state.generation;
        return true;
    }

    bool Disconnect(std::uint32_t peer_id) {
        if (!Leave(peer_id)) {
            return false;
        }
        ++m_state.disconnects;
        return true;
    }

    bool Grant(std::uint32_t peer_id, ControlMode mode) {
        if (peer_id == 0) {
            return false;
        }
        auto found = m_state.modes.find(peer_id);
        if (found == m_state.modes.end()) {
            if (m_state.modes.size() >= m_config.max_participants) {
                return false;
            }
            found = m_state.modes.emplace(peer_id, ControlMode::kNone).first;
        }
        const ControlMode from = found->second;
        m_state.revoked.erase(peer_id);
        if (mode == ControlMode::kInteract) {
            if (m_state.controller != 0 && m_state.controller != peer_id) {
                const auto previous = m_state.modes.find(m_state.controller);
                if (previous != m_state.modes.end()) {
                    const ControlMode previous_mode = previous->second;
                    previous->second = ControlMode::kView;
                    if (previous_mode != ControlMode::kView) {
                        ++m_state.generation;
                    }
                    ++m_state.preemptions;
                }
            }
            found->second = ControlMode::kInteract;
            m_state.controller = peer_id;
            if (from != ControlMode::kInteract) {
                ++m_state.generation;
            }
        } else {
            if (m_state.controller == peer_id) {
                m_state.controller = 0;
            }
            found->second = mode;
            if (from != mode) {
                ++m_state.generation;
            }
        }
        ++m_state.grants;
        return true;
    }

    bool Revoke(std::uint32_t peer_id) {
        if (peer_id == 0) {
            return false;
        }
        const auto found = m_state.modes.find(peer_id);
        if (found == m_state.modes.end()) {
            return false;
        }
        m_state.revoked.insert(peer_id);
        if (m_state.controller == peer_id) {
            found->second = ControlMode::kView;
            m_state.controller = 0;
            ++m_state.revocations;
            ++m_state.generation;
        }
        return true;
    }

    bool RevokeController() {
        if (m_state.controller == 0) {
            return false;
        }
        const std::uint32_t target = m_state.controller;
        const auto found = m_state.modes.find(target);
        if (found == m_state.modes.end()) {
            return false;
        }
        found->second = ControlMode::kView;
        m_state.revoked.insert(target);
        m_state.controller = 0;
        ++m_state.revocations;
        ++m_state.generation;
        return true;
    }

    bool Request(std::uint32_t peer_id) {
        if (peer_id == 0) {
            return false;
        }
        ++m_state.requests;
        const auto found = m_state.modes.find(peer_id);
        if (found == m_state.modes.end() || found->second == ControlMode::kNone ||
            m_state.revoked.count(peer_id) != 0) {
            ++m_state.denials;
            return false;
        }
        if (m_state.controller == peer_id) {
            return true;
        }
        if (m_state.controller == 0) {
            found->second = ControlMode::kInteract;
            m_state.controller = peer_id;
            ++m_state.generation;
            return true;
        }
        if (m_config.policy == ControlPolicy::kPreempt) {
            const auto previous = m_state.modes.find(m_state.controller);
            if (previous != m_state.modes.end()) {
                const ControlMode previous_mode = previous->second;
                previous->second = ControlMode::kView;
                if (previous_mode != ControlMode::kView) {
                    ++m_state.generation;
                }
                ++m_state.preemptions;
            }
            found->second = ControlMode::kInteract;
            m_state.controller = peer_id;
            ++m_state.generation;
            return true;
        }
        ++m_state.denials;
        return false;
    }

    bool Renew(std::uint32_t peer_id) {
        return m_state.controller != 0 && m_state.controller == peer_id &&
               m_config.lease_ms != 0;
    }

    /// @param expire true 用 UINT64_MAX（必过期），false 用 0（必不过期）。
    bool Tick(bool expire) {
        if (m_state.controller == 0 || m_config.lease_ms == 0) {
            return false;
        }
        if (!expire) {
            return false;
        }
        const auto found = m_state.modes.find(m_state.controller);
        if (found != m_state.modes.end()) {
            found->second = ControlMode::kView;
            ++m_state.lease_expirations;
            ++m_state.generation;
        }
        m_state.controller = 0;
        return true;
    }

    const ShadowState& state() const { return m_state; }

   private:
    SessionControlConfig m_config;
    ShadowState m_state;
};

/// 每一步都与库状态逐项比对（模式表、控制器、撤销集合、代数、统计）。
void CompareShadow(SessionControl& control, const ShadowModel& shadow, long step) {
    const ShadowState& expected = shadow.state();
    CHECK_EQ(control.generation(), expected.generation);
    CHECK_EQ(control.controller(), expected.controller);
    CHECK_EQ(control.has_controller(), expected.controller != 0);
    const std::vector<std::uint32_t> participants = control.participants();
    CHECK_EQ(participants.size(), expected.modes.size());
    std::size_t index = 0;
    for (const auto& entry : expected.modes) {
        if (index < participants.size()) {
            CHECK_EQ(participants[index], entry.first);
            CHECK_EQ(control.mode(entry.first), entry.second);
            CHECK_EQ(control.revoked(entry.first), expected.revoked.count(entry.first) != 0);
        }
        ++index;
    }
    CHECK_EQ(control.mode(0xFFFFFFFFU), ControlMode::kNone);
    const SessionControlStats stats = control.stats();
    CHECK_EQ(stats.changes, expected.generation);
    CHECK_EQ(stats.requests, expected.requests);
    CHECK_EQ(stats.grants, expected.grants);
    CHECK_EQ(stats.denials, expected.denials);
    CHECK_EQ(stats.revocations, expected.revocations);
    CHECK_EQ(stats.preemptions, expected.preemptions);
    CHECK_EQ(stats.lease_expirations, expected.lease_expirations);
    CHECK_EQ(stats.disconnects, expected.disconnects);
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] shadow mismatch at sequence step %ld\n", step);
    }
}

/// 一个随机序列：随机配置 + 随机操作，逐项比对影子模型。
void RunRandomSequence(std::uint32_t seed, int steps) {
    std::mt19937 rng(seed);
    SessionControlConfig config;
    static constexpr std::array<std::uint32_t, 8> kLeases = {0, 1, 10, 30, 100, 1000, 30000,
                                                             60000};
    static constexpr std::array<std::size_t, 6> kAudits = {0, 1, 2, 8, 64, 1024};
    static constexpr std::array<std::size_t, 4> kMaxPeers = {1, 2, 8, 64};
    config.lease_ms = kLeases[rng() % kLeases.size()];
    config.audit_capacity = kAudits[rng() % kAudits.size()];
    config.max_participants = kMaxPeers[rng() % kMaxPeers.size()];
    config.policy = (rng() % 2U) == 0 ? ControlPolicy::kFirstCome : ControlPolicy::kPreempt;
    config.default_mode = (rng() % 2U) == 0 ? ControlMode::kView : ControlMode::kInteract;
    config.session_id = rng();
    SessionControl control(config);
    ShadowModel shadow(config);
    std::vector<ControlChange> events;
    control.on_change = [&](const ControlChange& change) { events.push_back(change); };

    // 初始：最多 4 个参与者。
    const std::uint32_t initial = rng() % 5U;
    for (std::uint32_t peer_id = 1; peer_id <= initial; ++peer_id) {
        CHECK_EQ(control.Join(peer_id), shadow.Join(peer_id));
    }
    CompareShadow(control, shadow, 0);

    for (int step = 1; step <= steps; ++step) {
        const std::uint32_t peer_id = 1 + (rng() % 6U);
        const std::uint32_t op = rng() % 12U;
        bool actual = false;
        bool expected = false;
        switch (op) {
            case 0:
            case 1: {
                actual = control.Join(peer_id);
                expected = shadow.Join(peer_id);
                break;
            }
            case 2: {
                actual = control.Leave(peer_id);
                expected = shadow.Leave(peer_id);
                break;
            }
            case 3: {
                actual = control.OnPeerDisconnected(peer_id);
                expected = shadow.Disconnect(peer_id);
                break;
            }
            case 4:
            case 5: {
                actual = control.RequestControl(peer_id);
                expected = shadow.Request(peer_id);
                break;
            }
            case 6: {
                actual = control.Revoke(peer_id);
                expected = shadow.Revoke(peer_id);
                break;
            }
            case 7: {
                actual = control.Revoke();
                expected = shadow.RevokeController();
                break;
            }
            case 8: {
                actual = control.Renew(peer_id);
                expected = shadow.Renew(peer_id);
                break;
            }
            case 9: {
                const ControlMode mode =
                    (rng() % 3U) == 0 ? ControlMode::kNone
                                      : ((rng() % 2U) == 0 ? ControlMode::kView
                                                           : ControlMode::kInteract);
                actual = control.Grant(peer_id, mode);
                expected = shadow.Grant(peer_id, mode);
                break;
            }
            case 10: {
                const bool expire = (rng() % 2U) == 0;
                actual = control.Tick(expire ? std::numeric_limits<std::uint64_t>::max() : 0);
                expected = shadow.Tick(expire);
                break;
            }
            default: {
                actual = control.Grant(peer_id);
                expected = shadow.Grant(peer_id, ControlMode::kView);
                break;
            }
        }
        if (actual != expected) {
            std::fprintf(stderr, "[FAIL] seed=%u step=%d op=%u peer=%u actual=%d expected=%d\n",
                         seed, step, op, peer_id, static_cast<int>(actual),
                         static_cast<int>(expected));
            ++g_failures;
        }
        ++g_checks;
        // 回调事件的 generation 严格递增（同一操作的多个事件也不允许相等）。
        for (std::size_t i = 1; i < events.size(); ++i) {
            CHECK(events[i].generation > events[i - 1].generation);
        }
        events.clear();
        CompareShadow(control, shadow, step);
        if (g_failures != 0) {
            std::fprintf(stderr, "[FAIL] abort sequence seed=%u at step=%d\n", seed, step);
            return;
        }
    }
    CheckInvariants(control, "random-sequence");
}

void RunRandomTests(long scale) {
    const int sequences = static_cast<int>(Scaled(1000, scale));
    const int steps = 200;
    std::printf("== 随机测试：%d 序列 × %d 步，影子模型逐步比对 ==\n", sequences, steps);
    for (int sequence = 0; sequence < sequences; ++sequence) {
        RunRandomSequence(20261029U + static_cast<std::uint32_t>(sequence), steps);
        if (g_failures != 0) {
            return;
        }
    }
    std::printf("[RANDOM] sequences=%d steps=%d checks=%ld failures=%d\n", sequences,
                sequences * steps, g_checks, g_failures);
}

// ---------------------------------------------------------------------------
// 3. 五个复杂场景
// ---------------------------------------------------------------------------

/// 场景 1：抢占（Preempt）与先到先得（FirstCome）在同一负载下的对比。
void ScenarioPreemptVsFirstCome() {
    std::printf("-- 场景 1：抢占 vs 先到先得 --\n");
    for (const ControlPolicy policy : {ControlPolicy::kFirstCome, ControlPolicy::kPreempt}) {
        SessionControlConfig config;
        config.policy = policy;
        config.lease_ms = 0;
        SessionControl control(config);
        for (std::uint32_t peer_id = 1; peer_id <= 16; ++peer_id) {
            CHECK(control.Join(peer_id));
        }
        std::uint32_t winners = 0;
        for (std::uint32_t peer_id = 1; peer_id <= 16; ++peer_id) {
            if (control.RequestControl(peer_id)) {
                ++winners;
            }
        }
        CHECK_EQ(CountInteract(control), 1);
        CHECK_EQ(control.controller(), policy == ControlPolicy::kFirstCome ? 1U : 16U);
        const SessionControlStats stats = control.stats();
        if (policy == ControlPolicy::kFirstCome) {
            CHECK_EQ(winners, 1U);
            CHECK_EQ(stats.denials, 15U);
            CHECK_EQ(stats.preemptions, 0U);
        } else {
            CHECK_EQ(winners, 16U);
            CHECK_EQ(stats.denials, 0U);
            CHECK_EQ(stats.preemptions, 15U);
        }
        // 抢占事件字段：先 kPreempted(旧) 再 kRequested(新)，两者 controller
        // 都必须是「变化后」的新控制器。
        if (policy == ControlPolicy::kPreempt) {
            std::vector<ControlChange> events;
            control.on_change = [&](const ControlChange& change) { events.push_back(change); };
            CHECK(control.RequestControl(1));
            CHECK_EQ(events.size(), static_cast<std::size_t>(2));
            if (events.size() == 2) {
                CHECK_EQ(events[0].reason, ControlChangeReason::kPreempted);
                CHECK_EQ(events[0].peer_id, 16U);
                CHECK_EQ(events[0].controller, 1U);
                CHECK(events[0].generation < events[1].generation);
                CHECK_EQ(events[1].reason, ControlChangeReason::kRequested);
                CHECK_EQ(events[1].controller, 1U);
            }
        }
        CheckInvariants(control, "scenario-preempt");
    }
}

/// 场景 2：租约边界（假时钟）——极早不回收、极晚回收、Renew 生效、
/// 重复 Tick 不重复计数、租约 0 永不过期。
void ScenarioLeaseBoundary() {
    std::printf("-- 场景 2：租约边界与 Renew（假时钟） --\n");
    SessionControlConfig config;
    config.lease_ms = 1000;
    SessionControl control(config);
    CHECK(control.Join(1));
    CHECK(control.Join(2));
    CHECK(control.RequestControl(1));
    CHECK(!control.Tick(0));
    CHECK_EQ(control.controller(), 1U);
    std::vector<ControlChange> events;
    control.on_change = [&](const ControlChange& change) { events.push_back(change); };
    CHECK(control.Tick(std::numeric_limits<std::uint64_t>::max()));
    CHECK_EQ(control.controller(), 0U);
    CHECK_EQ(control.mode(1), ControlMode::kView);
    CHECK_EQ(events.size(), static_cast<std::size_t>(1));
    if (events.size() == 1) {
        CHECK_EQ(events[0].reason, ControlChangeReason::kLeaseExpired);
        CHECK_EQ(events[0].controller, 0U);  // 变化后的控制器必须为 0
        CHECK_EQ(events[0].from, ControlMode::kInteract);
        CHECK_EQ(events[0].to, ControlMode::kView);
    }
    CHECK(!control.Tick(std::numeric_limits<std::uint64_t>::max()));
    CHECK_EQ(control.stats().lease_expirations, 1U);

    // Renew：控制器可续租；非控制器、无控制器时返回 false。
    CHECK(control.RequestControl(2));
    CHECK(control.Renew(2));
    CHECK(!control.Renew(1));
    CHECK_EQ(control.controller(), 2U);

    // 租约 0：永不过期，Renew 恒 false。
    SessionControlConfig forever;
    forever.lease_ms = 0;
    SessionControl permanent(forever);
    CHECK(permanent.Join(1));
    CHECK(permanent.RequestControl(1));
    CHECK(!permanent.Tick(std::numeric_limits<std::uint64_t>::max()));
    CHECK_EQ(permanent.controller(), 1U);
    CHECK(!permanent.Renew(1));
    CheckInvariants(control, "scenario-lease");
}

/// 场景 3：撤销持久——Revoke → Leave → Join → 仍拒绝；Grant 才解除；
/// 撤销集合不随参与者表收缩而丢失。
void ScenarioRevokePersistence() {
    std::printf("-- 场景 3：撤销持久（Revoke → Leave → Join → 拒绝） --\n");
    SessionControl control;
    CHECK(control.Join(1));
    CHECK(control.Join(2));
    CHECK(control.RequestControl(1));
    CHECK(control.Revoke(1));
    for (int round = 0; round < 5; ++round) {
        CHECK(control.revoked(1));
        CHECK(control.Leave(1));
        CHECK(control.revoked(1));
        CHECK(control.Join(1));
        CHECK(control.revoked(1));
        CHECK_EQ(control.mode(1), ControlMode::kView);
        CHECK(!control.RequestControl(1));
    }
    CHECK(control.Grant(1));
    CHECK(!control.revoked(1));
    CHECK(control.RequestControl(1));

    // 撤销非控制器：只标记禁入，不改变其模式，不产生事件。
    CHECK(control.Join(3));
    std::vector<ControlChange> events;
    control.on_change = [&](const ControlChange& change) { events.push_back(change); };
    const std::uint64_t generation_before = control.generation();
    CHECK(control.Revoke(3));
    CHECK_EQ(control.generation(), generation_before);
    CHECK(events.empty());
    CHECK(control.revoked(3));
    CHECK_EQ(control.mode(3), ControlMode::kView);

    // 断开后撤销标记仍在；重连仍被拒。
    CHECK(control.OnPeerDisconnected(3));
    CHECK(control.revoked(3));
    CHECK(control.Join(3));
    CHECK(!control.RequestControl(3));
    CHECK(control.Grant(3));
    CHECK(control.Revoke());  // 先释放当前控制器，3 才能走「无控制器」路径
    CHECK(control.RequestControl(3));
    CHECK_EQ(control.controller(), 3U);
    CheckInvariants(control, "scenario-revoke");
}

/// 场景 4：断开回收——模拟 PeerHub::on_peer_disconnected 的 lambda 适配、
/// 控制者断开后他人立即接手、非控制者断开不误伤、统计守恒。
void ScenarioDisconnectReclaim() {
    std::printf("-- 场景 4：断开回收（on_peer_disconnected 适配） --\n");
    SessionControl control;
    for (std::uint32_t peer_id = 1; peer_id <= 4; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    CHECK(control.RequestControl(1));
    std::vector<ControlChange> events;
    control.on_change = [&](const ControlChange& change) { events.push_back(change); };
    // 模拟 PeerHub 的 two-argument 回调签名。
    const auto hub_disconnect = [&control](std::uint32_t peer_id, const char* reason) {
        (void)reason;
        return control.OnPeerDisconnected(peer_id);
    };
    CHECK(hub_disconnect(1, "tcp closed"));
    CHECK_EQ(control.controller(), 0U);
    CHECK_EQ(events.size(), static_cast<std::size_t>(1));
    if (events.size() == 1) {
        CHECK_EQ(events[0].reason, ControlChangeReason::kDisconnected);
        CHECK_EQ(events[0].actor, 0U);
        CHECK_EQ(events[0].controller, 0U);
    }
    CHECK_EQ(control.stats().disconnects, 1U);
    // 断开非控制器：不回收控制权，只移除参与者。
    CHECK(control.RequestControl(2));
    events.clear();
    CHECK(hub_disconnect(3, "timeout"));
    CHECK_EQ(control.controller(), 2U);
    CHECK_EQ(control.mode(3), ControlMode::kNone);
    CHECK_EQ(control.stats().disconnects, 2U);
    // 重复断开同一 peer 返回 false，不重复计数。
    CHECK(!hub_disconnect(3, "timeout"));
    CHECK_EQ(control.stats().disconnects, 2U);
    // 重连后可以正常参与仲裁：先释放 2 的控制权。
    CHECK(control.Join(3));
    CHECK(control.Revoke(2));
    CHECK(control.RequestControl(3));
    CHECK_EQ(control.controller(), 3U);
    CheckInvariants(control, "scenario-disconnect");
}

/// 场景 5：16 线程并发 Grant/Request——唯一 Interact、wins 与状态一致、
/// 统计守恒、无崩溃。
void ScenarioConcurrentGrantRequest() {
    std::printf("-- 场景 5：16 线程并发 Grant/Request --\n");
    SessionControlConfig config;
    config.policy = ControlPolicy::kPreempt;
    config.lease_ms = 0;
    SessionControl control(config);
    constexpr std::uint32_t kThreads = 16;
    for (std::uint32_t peer_id = 1; peer_id <= kThreads; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<long> grant_wins{0};
    std::atomic<long> request_wins{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::uint32_t thread = 0; thread < kThreads; ++thread) {
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
    while (ready.load(std::memory_order_acquire) < static_cast<int>(kThreads)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK_EQ(grant_wins.load(), static_cast<long>(kThreads) * 250);
    CHECK_EQ(request_wins.load(), static_cast<long>(kThreads) * 250);
    CHECK_EQ(CountInteract(control), 1);
    CHECK(control.controller() != 0);
    const SessionControlStats stats = control.stats();
    CHECK_EQ(stats.grants, static_cast<std::uint64_t>(grant_wins.load()));
    CHECK_EQ(stats.requests, static_cast<std::uint64_t>(request_wins.load()));
    CHECK_EQ(stats.changes, control.generation());
    CheckInvariants(control, "scenario-concurrent-grant");
}

void RunScenarios() {
    ScenarioPreemptVsFirstCome();
    ScenarioLeaseBoundary();
    ScenarioRevokePersistence();
    ScenarioDisconnectReclaim();
    ScenarioConcurrentGrantRequest();
    std::printf("[SCENARIOS] checks=%ld failures=%d\n", g_checks, g_failures);
}

// ---------------------------------------------------------------------------
// 4. 16 线程随机 fuzz + 长稳
// ---------------------------------------------------------------------------

void RunThreadedFuzz(long scale) {
    std::printf("== 16 线程随机 fuzz ==\n");
    SessionControlConfig config;
    config.policy = ControlPolicy::kPreempt;
    config.lease_ms = 0;
    config.audit_capacity = 0;
    SessionControl control(config);
    constexpr std::uint32_t kThreads = 16;
    for (std::uint32_t peer_id = 1; peer_id <= 64; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<long> ops{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::uint32_t thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&, thread]() {
            std::mt19937 rng(0xC0FFEEU + thread);
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const int iterations = static_cast<int>(Scaled(20000, scale));
            for (int iter = 0; iter < iterations; ++iter) {
                const std::uint32_t peer_id = 1 + (rng() % 64U);
                switch (rng() % 8U) {
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
                        (void)control.Grant(peer_id, ControlMode::kInteract);
                        break;
                    case 6:
                        (void)control.Renew(peer_id);
                        break;
                    default:
                        (void)control.Tick();
                        break;
                }
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < static_cast<int>(kThreads)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    std::printf("[FUZZ] threads=%u ops=%ld\n", kThreads, ops.load());
    CHECK_EQ(CountInteract(control), 1);
    CHECK_EQ(control.generation(), control.stats().changes);
    CHECK(control.participants().size() <= 64);
    CheckInvariants(control, "threaded-fuzz");
}

void RunSoak(int seconds) {
    std::printf("== 长稳：%d s 混合负载 ==\n", seconds);
    SessionControlConfig config;
    config.policy = ControlPolicy::kPreempt;
    config.lease_ms = 1000;
    config.audit_capacity = 64;
    SessionControl control(config);
    for (std::uint32_t peer_id = 1; peer_id <= 64; ++peer_id) {
        CHECK(control.Join(peer_id));
    }
    std::atomic<bool> stop{false};
    std::atomic<long> ops{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; ++thread) {
        threads.emplace_back([&, thread]() {
            std::mt19937 rng(0xBEEFU + thread);
            while (!stop.load(std::memory_order_relaxed)) {
                const std::uint32_t peer_id = 1 + (rng() % 64U);
                switch (rng() % 4U) {
                    case 0:
                        (void)control.RequestControl(peer_id);
                        break;
                    case 1:
                        (void)control.Grant(peer_id, ControlMode::kInteract);
                        break;
                    case 2:
                        (void)control.Renew(peer_id);
                        break;
                    default:
                        (void)control.Tick();
                        break;
                }
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    const long rss_start = ReadRssKb();
    long rss_peak = rss_start;
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        rss_peak = std::max(rss_peak, ReadRssKb());
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& thread : threads) {
        thread.join();
    }
    const long rss_end = ReadRssKb();
    std::printf("[SOAK] ops=%ld rss=%ld -> peak %ld -> %ld KiB\n", ops.load(), rss_start,
                rss_peak, rss_end);
    CHECK_EQ(CountInteract(control), 1);
    CHECK_EQ(control.generation(), control.stats().changes);
    // ASan 的影子内存与隔离区会放大 RSS，阈值只在常规构建生效；
    // 泄漏本身由 ASan/valgrind 的分配器检查负责。
#if !defined(__SANITIZE_ADDRESS__)
    CHECK(rss_end - rss_start < 32 * 1024);  // 增长 < 32 MiB
#endif
    CheckInvariants(control, "soak");
}

// ---------------------------------------------------------------------------
// 5. 参数解析与入口
// ---------------------------------------------------------------------------

long ParseScale(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--scale=", 8) == 0) {
            const double value = std::atof(argv[i] + 8);
            if (value <= 0.0) {
                return 0;
            }
            return static_cast<long>(value * 100.0);
        }
    }
    return 100;
}

int ParseSeconds(int argc, char** argv, int fallback) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--seconds=", 10) == 0) {
            return std::atoi(argv[i] + 10);
        }
    }
    return fallback;
}

bool HasMode(int argc, char** argv, const char* mode) {
    const std::string needle = std::string("--mode=") + mode;
    for (int i = 1; i < argc; ++i) {
        if (needle == argv[i]) {
            return true;
        }
    }
    return false;
}

std::string ModeName(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--mode=", 7) == 0) {
            return argv[i] + 7;
        }
    }
    return "all";
}

}  // namespace

int main(int argc, char** argv) {
    const long scale = ParseScale(argc, argv);
    if (scale <= 0) {
        std::fprintf(stderr, "invalid --scale\n");
        return 2;
    }
    const bool all = argc == 1 || HasMode(argc, argv, "all");
    std::printf("session_control audit stress: scale=%ld%% mode=%s\n", scale,
                ModeName(argc, argv).c_str());
    if (all || HasMode(argc, argv, "bench")) {
        RunBenchAll(scale);
    }
    if (all || HasMode(argc, argv, "random")) {
        RunRandomTests(scale);
    }
    if (all || HasMode(argc, argv, "scenarios")) {
        RunScenarios();
    }
    if (all || HasMode(argc, argv, "threads")) {
        RunThreadedFuzz(scale);
    }
    if (HasMode(argc, argv, "soak") || (all && scale >= 100)) {
        RunSoak(ParseSeconds(argc, argv, 10));
    }
    std::printf("[%s] checks=%ld failures=%d\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
