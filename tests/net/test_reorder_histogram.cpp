/**
 * @file tests/net/test_reorder_histogram.cpp
 * @brief `go2cpp_reorder_histogram` 的正确性 / 场景 / 随机 / 对照测试。
 *
 * 覆盖：
 *   1. 纯乱序（0,2,1,3,5,4…）→ 直方图零误重传；
 *   2. 真丢包（缺 2/6）→ 缺口全部判 lost；
 *   3. 混合（乱序 + 丢包）→ 只判真丢的；
 *   4. 学习收敛（首次 gap=2 误判 1 次，之后不再误判）；
 *   5. 停流超时兜底（stale_timeout_ms）；
 *   6. 配置/边界（分位、夹紧、位图滚动、重复/重传包）；
 *   7. 6 个真实网络画像 + 100 万随机包，朴素 vs 直方图对照。
 *
 * 判据模型（与真实可靠层一致）：维护「已发现但未收到的缺口」集合 pending；
 * 每到达一个包就重估 pending，判 true 即发起一次重传并销账。这样
 * `decisions` 就等于重传次数，`false_positives` 就等于误重传的包数。
 */
#include "go2cpp/net/reorder_histogram.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>

namespace {

using go2cpp::net::PacketResult;
using go2cpp::net::ReorderHistogram;
using go2cpp::net::ReorderHistogramConfig;

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("[FAIL] line %d: %s\n", line, what);
    }
}

void CheckU64(std::uint64_t got, std::uint64_t want, const char* what, int line) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("[FAIL] line %d: %s (got %llu, want %llu)\n", line, what,
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(want));
    }
}

#define CHECK(cond) Check((cond), #cond, __LINE__)
#define CHECK_U64(got, want) CheckU64((got), (want), #got " == " #want, __LINE__)

// ---------------------------------------------------------------------------
// 场景执行器：同一份到达序列分别用「朴素判据」与「直方图判据」跑一遍。
// ---------------------------------------------------------------------------

struct ScenarioResult {
    std::uint64_t decisions = 0;        ///< 判丢（发起重传）的次数。
    std::uint64_t false_positives = 0;  ///< 判丢、但该包最终到达过的包数。
    std::uint64_t missed = 0;           ///< 最终没到、却从未判丢的包数。
    std::uint64_t arrived = 0;          ///< 实际到达过的不同序号数。
    std::set<std::uint64_t> flagged;    ///< 判丢过（重传过）的序号集合。
};

/**
 * @brief 用指定判据跑一遍到达序列。
 *
 * @param arrivals [in] 按到达先后排列的序号；可重复（模拟重传）。
 * @param naive [in] true = 朴素判据（任何 < max_seq_seen 且未收到的序号都判丢）；
 *        false = 直方图判据。
 * @param cfg [in] 直方图参数（naive 时忽略）。
 * @return 重传决策 / 误判 / 漏判统计。
 */
ScenarioResult RunScenario(const std::vector<std::uint64_t>& arrivals, bool naive,
                           const ReorderHistogramConfig& cfg = ReorderHistogramConfig{}) {
    ScenarioResult result;
    ReorderHistogram hist(cfg);
    std::set<std::uint64_t> received;
    std::set<std::uint64_t> pending;
    std::uint64_t max_seen = 0;
    bool has_packets = false;

    for (std::size_t i = 0; i < arrivals.size(); ++i) {
        const std::uint64_t seq = arrivals[i];
        const std::uint64_t now_ms = static_cast<std::uint64_t>(i);
        const bool first = !has_packets;
        const bool advances = first || seq > max_seen;
        const std::uint64_t old_max = max_seen;

        // 直方图与「已收」集合同步推进；决策必须用推进后的 max_seq_seen，
        // 这样缺口的乱序距离 = 新 max − 缺口，与 WebRTC 的等待语义一致。
        if (!naive) {
            (void)hist.OnPacket(seq, now_ms);
        }
        if (!has_packets) {
            has_packets = true;
            max_seen = seq;
        } else if (seq > max_seen) {
            max_seen = seq;
        }
        received.insert(seq);
        pending.erase(seq);

        // 新出现的缺口：本次推进跳过的序号。首包之前的序号不属于本流。
        if (advances && !first) {
            for (std::uint64_t gap_seq = old_max + 1; gap_seq < seq; ++gap_seq) {
                pending.insert(gap_seq);
            }
        }

        // 每个到达包后重估所有未决缺口；判丢即重传并销账。
        for (auto it = pending.begin(); it != pending.end();) {
            const bool lost = naive ? true : hist.IsLikelyLost(*it);
            if (lost) {
                ++result.decisions;
                result.flagged.insert(*it);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
    }

    result.arrived = received.size();
    for (std::uint64_t seq : result.flagged) {
        if (received.count(seq) != 0) {
            ++result.false_positives;
        }
    }
    // 漏判：序列覆盖范围内最终没到、且从未判丢的序号。
    if (!received.empty()) {
        const std::uint64_t limit = *received.rbegin();
        for (std::uint64_t seq = 0; seq <= limit; ++seq) {
            if (received.count(seq) == 0 && result.flagged.count(seq) == 0) {
                ++result.missed;
            }
        }
    }
    return result;
}

std::vector<std::uint64_t> SeqList(std::initializer_list<std::uint64_t> values) {
    return std::vector<std::uint64_t>(values);
}

void PrintScenario(const char* name, const std::vector<std::uint64_t>& arrivals,
                   const ScenarioResult& naive, const ScenarioResult& hist) {
    std::printf("[scenario] %s（%zu 包）\n", name, arrivals.size());
    std::printf("  朴素(seq>expected): 重传决策=%llu 误重传=%llu 漏判=%llu\n",
                static_cast<unsigned long long>(naive.decisions),
                static_cast<unsigned long long>(naive.false_positives),
                static_cast<unsigned long long>(naive.missed));
    std::printf("  直方图(p50):        重传决策=%llu 误重传=%llu 漏判=%llu\n",
                static_cast<unsigned long long>(hist.decisions),
                static_cast<unsigned long long>(hist.false_positives),
                static_cast<unsigned long long>(hist.missed));
}

// ---------------------------------------------------------------------------
// 1. 纯乱序：相邻交换（gap 恒为 1）。
// ---------------------------------------------------------------------------
void TestPureReordering() {
    std::printf("\n=== 1. 纯乱序场景 ===\n");
    const std::vector<std::uint64_t> arrivals =
        SeqList({0, 2, 1, 3, 5, 4, 6, 8, 7, 9, 11, 10, 12, 14, 13, 15, 17, 16, 18, 20, 19, 21});
    const ScenarioResult naive = RunScenario(arrivals, true);
    const ScenarioResult hist = RunScenario(arrivals, false);
    PrintScenario("纯乱序（7 次相邻交换）", arrivals, naive, hist);

    // 核心契约：直方图在纯乱序下一次都不该判丢。
    CHECK_U64(hist.decisions, 0);
    CHECK_U64(hist.false_positives, 0);
    CHECK_U64(hist.missed, 0);
    // 对照：朴素判据每个交换误重传一次。
    CHECK_U64(naive.decisions, 7);
    CHECK_U64(naive.false_positives, 7);

    // 逐包验证：每个乱序包到达前，直方图都不判它丢。
    ReorderHistogram hist2;
    const std::uint64_t pairs[][2] = {{2, 1}, {5, 4}, {8, 7}};
    for (const auto& pair : pairs) {
        const std::uint64_t big = pair[0];
        const std::uint64_t small = pair[1];
        (void)hist2.OnPacket(big, big);
        CHECK(!hist2.IsLikelyLost(small));
        (void)hist2.OnPacket(small, small + 1);
        CHECK(hist2.has_seq(small));
    }
    // 直方图应学到「乱序距离 1」这个样本。
    CHECK_U64(hist2.sample_count(), 3);
    CHECK_U64(hist2.ReorderPercentile(), 1);
    CHECK_U64(hist2.tolerance(), 2);
}

// ---------------------------------------------------------------------------
// 2. 真丢包：缺 2 与 6。
// ---------------------------------------------------------------------------
void TestRealLoss() {
    std::printf("\n=== 2. 真丢包场景 ===\n");
    const std::vector<std::uint64_t> arrivals = SeqList({0, 1, 3, 4, 5, 7, 8, 9, 10, 11});
    const ScenarioResult naive = RunScenario(arrivals, true);
    const ScenarioResult hist = RunScenario(arrivals, false);
    PrintScenario("真丢包（缺 2、6）", arrivals, naive, hist);

    CHECK_U64(hist.false_positives, 0);
    CHECK_U64(hist.missed, 0);
    CHECK_U64(hist.decisions, 2);
    CHECK(hist.flagged.count(2) == 1);
    CHECK(hist.flagged.count(6) == 1);
    CHECK_U64(naive.missed, 0);

    // 时序断言：2 在收到 4 后判丢（gap=2），6 在收到 8 后判丢。
    ReorderHistogram h;
    for (std::uint64_t seq : arrivals) {
        (void)h.OnPacket(seq, seq);
    }
    CHECK(h.IsLikelyLost(2));
    CHECK(h.IsLikelyLost(6));
    CHECK(!h.IsLikelyLost(3));
    CHECK(!h.IsLikelyLost(4));
}

// ---------------------------------------------------------------------------
// 3. 混合：乱序 + 真丢包，只判真丢。
// ---------------------------------------------------------------------------
void TestMixed() {
    std::printf("\n=== 3. 混合场景（乱序 + 丢包）===\n");
    const std::vector<std::uint64_t> arrivals =
        SeqList({0, 1, 3, 5, 4, 7, 8, 10, 9, 11, 13, 12, 14, 17, 15, 16, 18});
    const ScenarioResult naive = RunScenario(arrivals, true);
    const ScenarioResult hist = RunScenario(arrivals, false);
    PrintScenario("混合（真丢 2/6，乱序 4/9/12/15/16）", arrivals, naive, hist);

    CHECK_U64(hist.decisions, 2);
    CHECK_U64(hist.false_positives, 0);
    CHECK_U64(hist.missed, 0);
    CHECK(hist.flagged.count(2) == 1);
    CHECK(hist.flagged.count(6) == 1);
    // 所有乱序但最终到达的包都不该被判丢。
    for (std::uint64_t seq : {4u, 9u, 12u, 15u, 16u}) {
        CHECK(hist.flagged.count(seq) == 0);
    }
    // 朴素判据在同样的乱序上会误判。
    CHECK(naive.false_positives > 0);
    CHECK(naive.false_positives > hist.false_positives);
}

// ---------------------------------------------------------------------------
// 4. 学习收敛：首次 gap=2 误判一次，之后同距离不再误判。
// ---------------------------------------------------------------------------
void TestLearningConvergence() {
    std::printf("\n=== 4. 学习收敛场景 ===\n");
    const std::vector<std::uint64_t> arrivals =
        SeqList({0, 3, 2, 1, 4, 6, 5, 7, 10, 8, 9, 11});
    const ScenarioResult naive = RunScenario(arrivals, true);
    const ScenarioResult hist = RunScenario(arrivals, false);
    PrintScenario("首次 gap=2 → 学习后 gap<=2 安全", arrivals, naive, hist);

    // 唯一误判是 1（收 3 时直方图还没有样本，tolerance=1，gap=2 超出）。
    CHECK_U64(hist.decisions, 1);
    CHECK_U64(hist.false_positives, 1);
    CHECK(hist.flagged.count(1) == 1);
    CHECK_U64(hist.missed, 0);

    // 收 10 时 8/9 都是缺口，gap 分别为 2 和 1；此时已学过乱序距离 1 和 2，
    // 都不该判丢。
    ReorderHistogram h;
    for (std::uint64_t seq : {0u, 3u, 2u, 1u, 4u, 6u, 5u, 7u, 10u}) {
        (void)h.OnPacket(seq, seq);
    }
    CHECK(!h.IsLikelyLost(8));
    CHECK(!h.IsLikelyLost(9));
    CHECK_U64(h.ReorderPercentile(), 1);
    CHECK_U64(h.tolerance(), 2);

    // 对照配置：把 min_tolerance 提到 2，首次 gap=2 也不误判。
    ReorderHistogramConfig cfg;
    cfg.min_tolerance = 2;
    const ScenarioResult conservative = RunScenario(arrivals, false, cfg);
    CHECK_U64(conservative.decisions, 0);
    CHECK_U64(conservative.false_positives, 0);
}

// ---------------------------------------------------------------------------
// 5. 停流超时兜底：最后一个缺口没有后继包推进时靠时间确认。
// ---------------------------------------------------------------------------
void TestStaleTimeout() {
    std::printf("\n=== 5. 停流超时兜底 ===\n");
    ReorderHistogramConfig cfg;
    cfg.stale_timeout_ms = 200;
    ReorderHistogram h(cfg);
    (void)h.OnPacket(0, 0);
    (void)h.OnPacket(1, 10);
    CHECK(h.OnPacket(3, 20) == PacketResult::kInOrder);

    // 收 3 时 2 的 gap=1，仍在容忍度内：纯 gap 判据不判丢。
    CHECK(!h.IsLikelyLost(2));
    // 100ms 未静止够久 → 仍不判丢。
    CHECK(!h.IsLikelyLost(2, 100));
    // 20ms + 200ms = 220ms → 兜底判丢。
    CHECK(h.IsLikelyLost(2, 220));
    // 时钟回退不做判断。
    CHECK(!h.IsLikelyLost(2, 5));
    // 已收到的包、比 max 大的包永远不判丢。
    CHECK(!h.IsLikelyLost(3, 1000));
    CHECK(!h.IsLikelyLost(99, 1000));
    // stale_timeout_ms 关闭时，纯 gap 判据维持 false。
    ReorderHistogram off;
    (void)off.OnPacket(0, 0);
    (void)off.OnPacket(3, 10);
    CHECK(!off.IsLikelyLost(2, 100000));
}

// ---------------------------------------------------------------------------
// 6. 配置 / 边界 / 分位语义。
// ---------------------------------------------------------------------------
void TestConfigAndEdges() {
    std::printf("\n=== 6. 配置与边界 ===\n");

    // 空直方图：无样本 → 分位 0，容忍度取下限。
    ReorderHistogram empty;
    CHECK(!empty.has_packets());
    CHECK_U64(empty.ReorderPercentile(), 0);
    CHECK_U64(empty.tolerance(), 1);
    CHECK(!empty.IsLikelyLost(0));
    CHECK_U64(empty.sample_count(), 0);

    // 非法配置被就地修正。
    ReorderHistogramConfig bad;
    bad.num_buckets = 0;
    bad.window = 0;
    bad.received_window = 0;
    bad.min_samples = 0;
    bad.percentile = -3.0;
    bad.max_tolerance = 0;
    ReorderHistogram fixed(bad);
    CHECK_U64(fixed.Config().num_buckets, 1);
    CHECK_U64(fixed.Config().window, 1);
    CHECK_U64(fixed.Config().received_window, 1);
    CHECK_U64(fixed.Config().min_samples, 1);
    CHECK(fixed.Config().percentile == 0.0);
    CHECK_U64(fixed.Config().max_tolerance, 1);

    ReorderHistogramConfig high;
    high.percentile = 5.0;
    ReorderHistogram clamp_high(high);
    CHECK(clamp_high.Config().percentile == 1.0);
    // 上限低于下限时取上限 = 下限。
    ReorderHistogramConfig inverted;
    inverted.min_tolerance = 5;
    inverted.max_tolerance = 2;
    ReorderHistogram inverted_hist(inverted);
    CHECK_U64(inverted_hist.Config().max_tolerance, 5);

    // 已知样本 {1, 2}：p50 → 1，p90 → 2。
    ReorderHistogram h;
    (void)h.OnPacket(0, 0);
    (void)h.OnPacket(3, 1);  // 按序推进到 3，无样本
    (void)h.OnPacket(2, 2);  // 样本: gap=1（seq 2）
    (void)h.OnPacket(1, 3);  // 样本: gap=2（seq 1）
    CHECK_U64(h.sample_count(), 2);
    CHECK_U64(h.bucket_count(1), 1);
    CHECK_U64(h.bucket_count(2), 1);
    CHECK_U64(h.bucket_count(0), 0);
    CHECK_U64(h.ReorderPercentile(0.0), 0);  // ceil(0)=0 → 最小桶
    CHECK_U64(h.ReorderPercentile(0.5), 1);  // ceil(1.0)=1 → 桶 1
    CHECK_U64(h.ReorderPercentile(1.0), 2);  // ceil(2.0)=2 → 桶 2
    CHECK_U64(h.tolerance(), 2);
    CHECK_U64(h.max_seq_seen(), 3);
    CHECK_U64(h.Stats().max_reorder_distance, 2);

    // 重复包不产生新样本。
    const std::size_t before = h.sample_count();
    CHECK(h.OnPacket(1, 4) == PacketResult::kDuplicate);
    CHECK_U64(h.sample_count(), before);

    // 重传包默认不采样；显式开启后才采样。
    ReorderHistogram h2;
    (void)h2.OnPacket(0, 0);
    (void)h2.OnPacket(5, 1);
    CHECK(h2.OnPacket(4, 2, true) == PacketResult::kOutOfOrder);
    CHECK_U64(h2.sample_count(), 0);
    ReorderHistogramConfig cfg;
    cfg.count_retransmitted = true;
    ReorderHistogram h3(cfg);
    (void)h3.OnPacket(0, 0);
    (void)h3.OnPacket(5, 1);
    CHECK(h3.OnPacket(4, 2, true) == PacketResult::kOutOfOrder);
    CHECK_U64(h3.sample_count(), 1);

    // 位图滚动：默认窗口 1024，按序走 2000 个包后老序号已滚出。
    ReorderHistogram rolling;
    for (std::uint64_t seq = 0; seq < 2000; ++seq) {
        (void)rolling.OnPacket(seq, seq);
    }
    CHECK(rolling.has_seq(1999));
    CHECK(rolling.has_seq(1000));  // 1999 - 1000 = 999 < 1024，仍在窗口内
    CHECK(!rolling.has_seq(900));  // 1999 - 900 = 1099 >= 1024，已滚出
    CHECK(!rolling.IsLikelyLost(900));  // 信息不足 → 保守不判丢
    CHECK_U64(rolling.sample_count(), 0);  // 全按序，没有乱序样本

    // 大跳跃（delta >= 窗口）会整体清零位图，新 max 必须仍标记为已收。
    ReorderHistogram jump;
    (void)jump.OnPacket(0, 0);
    (void)jump.OnPacket(50000, 1);
    CHECK(jump.has_seq(50000));
    CHECK(!jump.has_seq(0));
    CHECK_U64(jump.max_seq_seen(), 50000);

    // Reset 后回到初始状态但保留配置。
    ReorderHistogram resets;
    (void)resets.OnPacket(0, 0);
    (void)resets.OnPacket(2, 1);
    resets.Reset();
    CHECK(!resets.has_packets());
    CHECK_U64(resets.sample_count(), 0);
    CHECK_U64(resets.max_seq_seen(), 0);
    CHECK_U64(resets.Stats().packets, 0);
    CHECK_U64(resets.tolerance(), 1);

    // 桶计数之和恒等于样本数。
    CHECK_U64(h.bucket_count(0) + h.bucket_count(1) + h.bucket_count(2) +
                  h.bucket_count(3) + h.bucket_count(4) + h.bucket_count(5) +
                  h.bucket_count(6) + h.bucket_count(7) + h.bucket_count(8) +
                  h.bucket_count(9),
              h.sample_count());
}

// ---------------------------------------------------------------------------
// 7. 真实网络画像 + 100 万随机包，朴素 vs 直方图。
// ---------------------------------------------------------------------------

struct XorShift64 {
    std::uint64_t state;

    explicit XorShift64(std::uint64_t seed)
        : state(seed != 0 ? seed : 0x9e3779b97f4a7c15ULL) {}

    std::uint64_t Next() {
        std::uint64_t x = state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state = x;
        return x;
    }

    std::uint64_t Below(std::uint64_t bound) { return bound == 0 ? 0 : Next() % bound; }
};

/**
 * @brief 乱序延迟分布模型。
 *
 * - `kLongTail`：真实公网画像——绝大多数乱序距离 1~3，长尾到 max_delay
 *   （70% / 20% / 7% / 3%，与 RFC 4737 类测量到的长尾形态一致）；
 * - `kUniform`：**最坏情况**压力模型（1..max_delay 均匀），用于给出上界，
 *   不代表真实网络，不参与 <5% 断言。
 */
enum class DelayModel { kLongTail, kUniform };

std::uint64_t PickDelay(XorShift64& rng, std::uint64_t max_delay, DelayModel model) {
    if (max_delay == 0) {
        return 0;
    }
    if (model == DelayModel::kUniform) {
        return 1 + rng.Below(max_delay);
    }
    const std::uint64_t roll = rng.Below(1000);
    if (roll < 700 || max_delay == 1) {
        return 1;
    }
    if (roll < 900 || max_delay == 2) {
        return 2;
    }
    if (roll < 970 || max_delay == 3) {
        return 3;
    }
    return 4 + rng.Below(max_delay - 3);
}

/**
 * @brief 生成「按到达先后」的序号序列：每包独立决定丢包，未丢的包按概率
 *        延迟若干个序号（模拟乱序），最后按到达时刻排序。
 *
 * @param count [in] 原始发送包数（序号 0..count-1）。
 * @param rng [in,out] 随机源。
 * @param drop_permille [in] 丢包率（千分比，0..1000）。
 * @param reorder_permille [in] 未丢包中发生乱序的比例（千分比）。
 * @param max_delay [in] 最大乱序延迟（序号个数）；0 表示不乱序。
 * @param model [in] 延迟分布模型。
 * @return 到达序列（长度 <= count）。
 */
std::vector<std::uint64_t> MakeArrivals(std::uint64_t count, XorShift64& rng,
                                        std::uint64_t drop_permille,
                                        std::uint64_t reorder_permille,
                                        std::uint64_t max_delay, DelayModel model) {
    struct Item {
        std::uint64_t due;
        std::uint64_t seq;
    };
    std::vector<Item> items;
    items.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t seq = 0; seq < count; ++seq) {
        if (rng.Below(1000) < drop_permille) {
            continue;
        }
        std::uint64_t delay = 0;
        if (max_delay > 0 && rng.Below(1000) < reorder_permille) {
            delay = PickDelay(rng, max_delay, model);
        }
        items.push_back(Item{seq + delay, seq});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.due != b.due) {
            return a.due < b.due;
        }
        return a.seq < b.seq;
    });
    std::vector<std::uint64_t> arrivals;
    arrivals.reserve(items.size());
    for (const Item& item : items) {
        arrivals.push_back(item.seq);
    }
    return arrivals;
}

struct Profile {
    const char* name;
    std::uint64_t packet_count;
    std::uint64_t drop_permille;
    std::uint64_t reorder_permille;
    std::uint64_t max_delay;
    DelayModel model;
};

void TestRealProfiles() {
    std::printf("\n=== 7. 真实网络画像（长尾乱序；朴素 vs 直方图）===\n");
    std::printf("%-30s %8s %8s %12s %12s %10s %10s\n", "场景", "包数", "丢包率",
                "朴素误重传", "直方图误重传", "直方图漏判", "直方图误重率");
    const Profile profiles[] = {
        {"内网（无丢包/无乱序）", 200000, 0, 0, 0, DelayModel::kLongTail},
        {"有线公网（1%丢/1%乱序）", 200000, 10, 10, 2, DelayModel::kLongTail},
        {"4G 移动（5%丢/10%乱序）", 200000, 50, 100, 8, DelayModel::kLongTail},
        {"WiFi 切换（10%丢/20%乱序）", 200000, 100, 200, 16, DelayModel::kLongTail},
        {"多路径（0%丢/40%乱序）", 200000, 0, 400, 4, DelayModel::kLongTail},
        {"恶劣链路（20%丢/20%乱序）", 200000, 200, 200, 8, DelayModel::kLongTail},
    };
    std::uint64_t total_packets = 0;
    for (const Profile& profile : profiles) {
        XorShift64 rng(0x1234567890abcdefULL + profile.drop_permille +
                       profile.reorder_permille * 17);
        const std::vector<std::uint64_t> arrivals =
            MakeArrivals(profile.packet_count, rng, profile.drop_permille,
                         profile.reorder_permille, profile.max_delay, profile.model);
        const ScenarioResult naive = RunScenario(arrivals, true);
        const ScenarioResult hist = RunScenario(arrivals, false);
        total_packets += profile.packet_count;
        const double fp_rate = 100.0 * static_cast<double>(hist.false_positives) /
                               static_cast<double>(profile.packet_count);
        std::printf("%-30s %8llu %7llu%% %12llu %12llu %10llu %9.2f%%\n",
                    profile.name,
                    static_cast<unsigned long long>(profile.packet_count),
                    static_cast<unsigned long long>(profile.drop_permille / 10),
                    static_cast<unsigned long long>(naive.false_positives),
                    static_cast<unsigned long long>(hist.false_positives),
                    static_cast<unsigned long long>(hist.missed), fp_rate);
        // P0-2 验收：乱序场景误重传率（误重传包数 / 总包数）< 5%。
        CHECK(fp_rate < 5.0);
        // 只要乱序存在，直方图误重传必须严格少于朴素判据。
        if (profile.reorder_permille > 0) {
            CHECK(hist.false_positives < naive.false_positives);
        }
        // 内网/好网不应有误判。
        if (profile.reorder_permille == 0 && profile.drop_permille == 0) {
            CHECK_U64(hist.false_positives, 0);
        }
        // 直方图漏判应显著少于丢包数（末尾停流缺口除外）。
        const std::uint64_t dropped =
            profile.packet_count - static_cast<std::uint64_t>(arrivals.size());
        if (dropped > 0) {
            CHECK(hist.missed * 10 < dropped);
        }
    }
    // 6 个画像合计 120 万包（>= 100 万随机数据的要求）。
    CHECK_U64(total_packets, 1200000);

    // 极端模型（均匀 1..16 乱序距离）：只作上界参考，不断言 <5%。
    std::printf("\n=== 7b. 最坏情况上界（均匀延迟 1..16，仅参考）===\n");
    {
        XorShift64 rng(0xfeedfacecafebeefULL);
        const std::vector<std::uint64_t> arrivals =
            MakeArrivals(200000, rng, 100, 200, 16, DelayModel::kUniform);
        const ScenarioResult naive = RunScenario(arrivals, true);
        const ScenarioResult hist = RunScenario(arrivals, false);
        const double rate = 100.0 * static_cast<double>(hist.false_positives) / 200000.0;
        std::printf("  WiFi+均匀：朴素误重传=%llu，直方图误重传=%llu（%.2f%%），漏判=%llu\n",
                    static_cast<unsigned long long>(naive.false_positives),
                    static_cast<unsigned long long>(hist.false_positives), rate,
                    static_cast<unsigned long long>(hist.missed));
        // 即使最坏模型，直方图也必须显著优于朴素。
        CHECK(hist.false_positives * 2 < naive.false_positives);
    }

    // 额外一次纯随机 100 万包压力：丢包 3%、乱序 8%、长尾延迟 1..12。
    std::printf("\n=== 8. 随机压力（100 万包：3%%丢包 / 8%%乱序 / 长尾）===\n");
    XorShift64 rng(0xdeadbeefcafef00dULL);
    const std::vector<std::uint64_t> stress =
        MakeArrivals(1000000, rng, 30, 80, 12, DelayModel::kLongTail);
    const ScenarioResult naive = RunScenario(stress, true);
    const ScenarioResult hist = RunScenario(stress, false);
    const double naive_fp_rate = 100.0 * static_cast<double>(naive.false_positives) /
                                 1000000.0;
    const double hist_fp_rate = 100.0 * static_cast<double>(hist.false_positives) /
                                1000000.0;
    const double naive_dec_rate = 100.0 * static_cast<double>(naive.false_positives) /
                                  static_cast<double>(naive.decisions + 1);
    const double hist_dec_rate = 100.0 * static_cast<double>(hist.false_positives) /
                                 static_cast<double>(hist.decisions + 1);
    std::printf("  朴素：决策=%llu 误重传=%llu（占总包 %.2f%%，占决策 %.2f%%）\n",
                static_cast<unsigned long long>(naive.decisions),
                static_cast<unsigned long long>(naive.false_positives), naive_fp_rate,
                naive_dec_rate);
    std::printf("  直方图：决策=%llu 误重传=%llu（占总包 %.2f%%，占决策 %.2f%%）漏判=%llu\n",
                static_cast<unsigned long long>(hist.decisions),
                static_cast<unsigned long long>(hist.false_positives), hist_fp_rate,
                hist_dec_rate, static_cast<unsigned long long>(hist.missed));
    // 验收指标：误重传包数 / 总包数 < 5%。
    CHECK(hist_fp_rate < 5.0);
    CHECK(hist.false_positives < naive.false_positives);
    std::printf("  误重传率：朴素 %.2f%% → 直方图 %.2f%%\n", naive_fp_rate, hist_fp_rate);
}

}  // namespace

int main() {
    std::printf("[reorder_histogram] === 乱序直方图测试 ===\n");
    TestPureReordering();
    TestRealLoss();
    TestMixed();
    TestLearningConvergence();
    TestStaleTimeout();
    TestConfigAndEdges();
    TestRealProfiles();

    std::printf("\n[reorder_histogram] checks=%d failures=%d\n", g_checks, g_failures);
    if (g_failures != 0) {
        return 1;
    }
    std::printf("[reorder_histogram] all checks passed\n");
    return 0;
}
