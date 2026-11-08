/**
 * @file tests/net/test_fec_controller.cpp
 * @brief `go2cpp_fec_controller` 的契约 / 场景 / 对照测试。
 *
 * 覆盖（对应任务验收项）：
 *   1. 0% 丢包            → 冗余趋近下限（不浪费带宽）；
 *   2. 10% 丢包           → 冗余升到 15.7%（落在 10%~20%）；
 *   3. 50% / 100% 丢包    → 冗余到 50% 硬上限（不无限增）；
 *   4. 1%↔10% 反复抖动    → 冗余只变 2~3 次（滞后生效），并给出
 *                          「瞬时映射 vs 本控制器」的量化对照；
 *   5. 滞后三闸门各自生效（驻留 dwell / 降档连续确认 / 升降门限不同）；
 *   6. 闭环双向：先升后降，丢包恢复后冗余必须降回下限；
 *   7. 配置边界（50% 硬上限不可突破、NaN/越界修正、Reset、分片换算）；
 *   8. 6 个网络画像 + 100 万随机采样压力（不变量 + 耗时）；
 *   9. **固定 20% vs 自适应** 的带宽浪费对照（核心数据）。
 */
#include "go2cpp/net/fec_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using go2cpp::net::FecController;
using go2cpp::net::FecControllerConfig;

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("[FAIL] line %d: %s\n", line, what);
    }
}

void CheckNear(double got, double want, double tol, const char* what, int line) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failures;
        std::printf("[FAIL] line %d: %s (got %.6f, want %.6f ± %.6f)\n", line, what, got, want,
                    tol);
    }
}

void CheckInRange(double got, double lo, double hi, const char* what, int line) {
    ++g_checks;
    if (!(got >= lo && got <= hi)) {
        ++g_failures;
        std::printf("[FAIL] line %d: %s (got %.6f, want [%.4f, %.4f])\n", line, what, got, lo,
                    hi);
    }
}

#define CHECK(cond) Check((cond), #cond, __LINE__)
#define CHECK_NEAR(got, want, tol) CheckNear((got), (want), (tol), #got " ≈ " #want, __LINE__)
#define CHECK_IN(got, lo, hi) CheckInRange((got), (lo), (hi), #got " ∈ [" #lo ", " #hi "]", __LINE__)

// ---------------------------------------------------------------------------
// 轨迹统计：极差 / 相邻跳变次数 / 平均绝对步长。
// 这是「抖动」的可量化定义——不看单个值，看序列有多闹。
// ---------------------------------------------------------------------------

struct TraceStats {
    double min_value = 0.0;
    double max_value = 0.0;
    double range = 0.0;        ///< 极差 = max − min。
    double mean = 0.0;         ///< 算术平均。
    int big_steps = 0;         ///< |Δ| > threshold 的次数（越少越稳）。
    double max_step = 0.0;     ///< 单次最大跳变。
};

TraceStats Summarize(const std::vector<double>& values, double threshold) {
    TraceStats stats;
    if (values.empty()) {
        return stats;
    }
    stats.min_value = values.front();
    stats.max_value = values.front();
    double sum = 0.0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double v = values[i];
        sum += v;
        if (v < stats.min_value) {
            stats.min_value = v;
        }
        if (v > stats.max_value) {
            stats.max_value = v;
        }
        if (i > 0) {
            const double step = std::fabs(v - values[i - 1]);
            if (step > threshold) {
                ++stats.big_steps;
            }
            if (step > stats.max_step) {
                stats.max_step = step;
            }
        }
    }
    stats.mean = sum / static_cast<double>(values.size());
    stats.range = stats.max_value - stats.min_value;
    return stats;
}

/// XorShift64：测试只需要可复现的均匀随机，不引入 <random> 的依赖与开销。
class XorShift64 {
public:
    explicit XorShift64(std::uint64_t seed) : state_(seed == 0 ? 0x9e3779b97f4a7c15ULL : seed) {}

    std::uint64_t Next() {
        std::uint64_t x = state_;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state_ = x;
        return x;
    }

    /// [0,1) 均匀分布。
    double NextDouble() {
        return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0);
    }

    /// [lo, hi] 均匀分布。
    double Uniform(double lo, double hi) {
        return lo + (hi - lo) * NextDouble();
    }

private:
    std::uint64_t state_;
};

/// 模拟「媒体字节数」：packets 个包，每包 packet_bytes 字节。
double MediaBytes(std::uint64_t packets, std::uint64_t packet_bytes) {
    return static_cast<double>(packets) * static_cast<double>(packet_bytes);
}

// ---------------------------------------------------------------------------
// 1. 0% 丢包：冗余停在下限，且一个校验包都不多发。
// ---------------------------------------------------------------------------

void TestZeroLoss() {
    std::printf("\n=== 1. 0%% 丢包：冗余趋近下限（不浪费）===\n");
    FecController ctl;
    std::vector<double> trace;
    for (int i = 0; i < 60; ++i) {
        ctl.OnLossSample(0.0);
        trace.push_back(ctl.RecommendedRedundancy());
    }
    const TraceStats stats = Summarize(trace, 0.005);
    std::printf("  最终冗余=%.4f  变更次数=%u  极差=%.4f\n", ctl.RecommendedRedundancy(),
                static_cast<unsigned>(ctl.changes()), stats.range);

    CHECK_NEAR(ctl.RecommendedRedundancy(), ctl.config().min_redundancy, 1e-12);
    CHECK_IN(ctl.RecommendedRedundancy(), 0.0, 0.03);
    CHECK(ctl.changes() == 0);        // 全程一次都不动
    CHECK(stats.range == 0.0);        // 输出完全平直
    CHECK(ctl.up_changes() == 0);
    CHECK(ctl.down_changes() == 0);

    // 默认 min_redundancy=0.02、k=8 → round(0.16)=0：好网连 1 个校验包都不发。
    CHECK(ctl.RecommendedParityShards(8) == 0);
    std::printf("  k=8 → parity=%u（0.02×8=0.16，四舍五入为 0，不浪费一个校验包）\n",
                static_cast<unsigned>(ctl.RecommendedParityShards(8)));

    // 即使把下限设成 0，也不应低于 0（合法区间断言）。
    FecControllerConfig cfg;
    cfg.min_redundancy = 0.0;
    FecController zero_floor(cfg);
    for (int i = 0; i < 30; ++i) {
        zero_floor.OnLossSample(0.0);
    }
    CHECK_NEAR(zero_floor.RecommendedRedundancy(), 0.0, 1e-12);
    CHECK(zero_floor.RecommendedParityShards(8) == 0);
}

// ---------------------------------------------------------------------------
// 2. 10% 丢包：冗余升到 10%~20% 区间。
// ---------------------------------------------------------------------------

void TestTenPercentLoss() {
    std::printf("\n=== 2. 10%% 丢包：冗余升到 15.7%%（10%%~20%% 区间）===\n");
    FecController ctl;
    for (int i = 0; i < 40; ++i) {
        ctl.OnLossSample(0.10);
    }
    const double r = ctl.RecommendedRedundancy();
    std::printf("  冗余=%.4f（%.1f%%）  稳态映射=%.4f  变更=%u↑/%u↓  分片 m(k=8)=%u\n", r,
                r * 100.0, ctl.RedundancyForLoss(0.10), static_cast<unsigned>(ctl.up_changes()),
                static_cast<unsigned>(ctl.down_changes()),
                static_cast<unsigned>(ctl.RecommendedParityShards(8)));

    CHECK_IN(r, 0.10, 0.20);
    CHECK_NEAR(r, 0.1571, 5e-4);
    CHECK(ctl.up_changes() == 1);      // 一次升到位，没有反复
    CHECK(ctl.RecommendedParityShards(8) == 1);   // 0.157×8=1.26 → 1
    CHECK(ctl.RecommendedParityShards(16) == 3);  // 0.157×16=2.51 → 3（2.5 四舍五入）

    // 同一个丢包率，若走「瞬时映射」结果一致——说明稳态映射本身没有滞后，
    // 滞后只发生在 OnLossSample 的决策层。
    CHECK_NEAR(ctl.RedundancyForLoss(0.10), r, 1e-12);
}

// ---------------------------------------------------------------------------
// 3. 50% / 100% 丢包：封顶 50%，配置也无法突破。
// ---------------------------------------------------------------------------

void TestCapAtFiftyPercent() {
    std::printf("\n=== 3. 50%% / 100%% 丢包：冗余封顶 50%% ===\n");
    FecController ctl;
    for (int i = 0; i < 40; ++i) {
        ctl.OnLossSample(0.50);
    }
    std::printf("  50%% 丢包 → 冗余=%.4f（上限常量 %.2f）\n", ctl.RecommendedRedundancy(),
                FecController::kMaxTotalProtectionOverhead);
    CHECK_NEAR(ctl.RecommendedRedundancy(), 0.50, 1e-12);
    CHECK(ctl.RecommendedRedundancy() <= FecController::kMaxTotalProtectionOverhead);

    for (int i = 0; i < 40; ++i) {
        ctl.OnLossSample(1.0);
    }
    std::printf("  100%% 丢包 → 冗余=%.4f（不无限增）\n", ctl.RecommendedRedundancy());
    CHECK_NEAR(ctl.RecommendedRedundancy(), 0.50, 1e-12);

    // 配置想超过 50% 也不行：硬上限优先于可配置性。
    FecControllerConfig cfg;
    cfg.max_redundancy = 0.90;
    FecController over(cfg);
    CHECK_NEAR(over.config().max_redundancy, 0.50, 1e-12);
    for (int i = 0; i < 20; ++i) {
        over.OnLossSample(1.0);
    }
    CHECK(over.RecommendedRedundancy() <= 0.50);
    std::printf("  配置 max_redundancy=0.90 → 实际 %.2f（被夹到硬上限）\n",
                over.config().max_redundancy);

    // 组内理论可恢复比例 = r/(1+r)：50% 冗余最多补 33.3% 丢包。
    const double recoverable = 0.50 / (1.0 + 0.50);
    std::printf("  50%% 冗余的 MDS 可恢复组内丢包率 = %.4f（= r/(1+r)）\n", recoverable);
    CHECK_NEAR(recoverable, 1.0 / 3.0, 1e-12);
}

// ---------------------------------------------------------------------------
// 4. 抖动：1%↔10% 反复交替，滞后必须把输出钉住。
//    这是本模块存在的理由——瞬时映射会跟着抖，控制器不该跟着抖。
// ---------------------------------------------------------------------------

void TestJitterHysteresis() {
    std::printf("\n=== 4. 1%%↔10%% 抖动（200 个采样交替）===\n");
    FecController ctl;
    std::vector<double> controlled;
    std::vector<double> instantaneous;
    for (int i = 0; i < 200; ++i) {
        const double loss = (i % 2 == 0) ? 0.01 : 0.10;
        ctl.OnLossSample(loss);
        controlled.push_back(ctl.RecommendedRedundancy());
        // 对照：不滤波、不滞后，直接把瞬时丢包率喂给同一条映射。
        instantaneous.push_back(ctl.RedundancyForLoss(loss));
    }
    const TraceStats ctrl_stats = Summarize(controlled, 0.005);
    const TraceStats inst_stats = Summarize(instantaneous, 0.005);

    std::printf("  瞬时映射：极差=%.4f 大跳=%d 次 最大单步=%.4f 均值=%.4f\n", inst_stats.range,
                inst_stats.big_steps, inst_stats.max_step, inst_stats.mean);
    std::printf("  控制器  ：极差=%.4f 大跳=%d 次 最大单步=%.4f 均值=%.4f 变更=%u\n",
                ctrl_stats.range, ctrl_stats.big_steps, ctrl_stats.max_step, ctrl_stats.mean,
                static_cast<unsigned>(ctl.changes()));
    std::printf("  滤波器   ：滤波后丢包率=%.4f（真实均值 0.0550）\n", ctl.FilteredLossRate());

    // 核心断言：控制器在 200 个采样里最多动 3 次（瞬时映射动 199 次）。
    CHECK(ctl.changes() <= 3);
    CHECK(ctrl_stats.big_steps <= 4);
    CHECK(inst_stats.big_steps >= 190);
    CHECK(ctrl_stats.range < inst_stats.range * 0.7);
    CHECK(ctl.RecommendedRedundancy() <= ctl.config().max_redundancy);
    CHECK_NEAR(ctl.FilteredLossRate(), 0.055, 0.02);   // EWMA 收敛到真实均值附近

    // 更严格的「不抖动」判据：前几次是冷启动爬升，**稳态段必须完全不动**。
    // 后 100 个采样（=100 秒）输出极差为 0，而瞬时映射在这 100 秒里跳了 99 次。
    std::vector<double> tail_controlled(controlled.begin() + 100, controlled.end());
    std::vector<double> tail_instant(instantaneous.begin() + 100, instantaneous.end());
    const TraceStats tail_ctrl = Summarize(tail_controlled, 0.005);
    const TraceStats tail_inst = Summarize(tail_instant, 0.005);
    std::printf("  稳态段（后 100 采样）：控制器极差=%.6f 大跳=%d 次 ｜ 瞬时极差=%.4f 大跳=%d 次\n",
                tail_ctrl.range, tail_ctrl.big_steps, tail_inst.range, tail_inst.big_steps);
    CHECK(tail_ctrl.range < 1e-12);
    CHECK(tail_ctrl.big_steps == 0);
    CHECK(tail_inst.big_steps >= 90);

    // 滞后生效的第二个证据：即使把「抖动」换成更大的幅度（1%↔20%），
    // 变更次数仍是小常数——说明抑制来自机制而不是阈值恰巧合适。
    FecController wide;
    for (int i = 0; i < 200; ++i) {
        wide.OnLossSample((i % 2 == 0) ? 0.01 : 0.20);
    }
    std::printf("  1%%↔20%% 抖动：变更=%u 次 最终冗余=%.4f\n",
                static_cast<unsigned>(wide.changes()), wide.RecommendedRedundancy());
    CHECK(wide.changes() <= 4);
}

// ---------------------------------------------------------------------------
// 5. 滞后三闸门：驻留 / 降档连续确认 / 升降门限不同。
//    用 α=1.0（无滤波）把「滞后」单独隔离出来测。
// ---------------------------------------------------------------------------

void TestHysteresisGates() {
    std::printf("\n=== 5. 滞后三闸门（隔离测试，α=1.0 关掉滤波）===\n");

    // ---- 5.1 驻留闸门：dwell=3 时，连续 4 个递增采样只允许变 2 次 ----
    {
        FecControllerConfig cfg;
        cfg.ewma_alpha = 1.0;
        cfg.loss_low = 0.0;
        cfg.loss_high = 0.30;
        cfg.min_redundancy = 0.0;
        cfg.max_redundancy = 0.50;
        cfg.up_margin = 0.01;
        cfg.down_margin = 0.01;
        cfg.down_confirm_samples = 1;
        cfg.dwell_samples = 3;
        FecController ctl(cfg);
        const double losses[] = {0.10, 0.20, 0.25, 0.30};
        for (double loss : losses) {
            ctl.OnLossSample(loss);
        }
        std::printf("  驻留 dwell=3：4 个递增采样 → 变更=%u 次（不设闸门会是 4 次）冗余=%.4f\n",
                    static_cast<unsigned>(ctl.changes()), ctl.RecommendedRedundancy());
        // 第 1 个采样升到 0.1667；随后 3 个采样被驻留挡住，直到第 4 个才
        // 一次性升到当时的 target（0.30 → 上限 0.50），共 2 次变更。
        CHECK(ctl.changes() == 2);
        CHECK_NEAR(ctl.RecommendedRedundancy(), ctl.RedundancyForLoss(0.30), 1e-12);

        // 对照：dwell=0 时同样的输入会变 3 次（0.10→0.20→0.25→0.30 中每次
        // 只升到 target，第 4 次才到上限；这里断言「明显更多」即可）。
        FecControllerConfig fast_cfg = cfg;
        fast_cfg.dwell_samples = 0;
        FecController fast(fast_cfg);
        for (double loss : losses) {
            fast.OnLossSample(loss);
        }
        std::printf("  对照 dwell=0：变更=%u 次 冗余=%.4f\n", static_cast<unsigned>(fast.changes()),
                    fast.RecommendedRedundancy());
        CHECK(fast.changes() > ctl.changes());
    }

    // ---- 5.2 降档连续确认：单个 0% 采样不足以拆掉保护 ----
    {
        FecControllerConfig cfg;
        cfg.ewma_alpha = 1.0;
        cfg.loss_low = 0.0;
        cfg.loss_high = 0.30;
        cfg.min_redundancy = 0.0;
        cfg.max_redundancy = 0.50;
        cfg.up_margin = 0.01;
        cfg.down_margin = 0.05;
        cfg.down_confirm_samples = 3;
        cfg.dwell_samples = 0;
        FecController ctl(cfg);
        ctl.OnLossSample(0.30);            // 升到 0.50
        CHECK_NEAR(ctl.RecommendedRedundancy(), 0.50, 1e-12);
        ctl.OnLossSample(0.0);             // 降档确认 1/3
        ctl.OnLossSample(0.0);             // 2/3
        std::printf("  降档确认 down_confirm=3：2 个 0%% 采样后冗余=%.4f（仍未降）\n",
                    ctl.RecommendedRedundancy());
        CHECK_NEAR(ctl.RecommendedRedundancy(), 0.50, 1e-12);
        ctl.OnLossSample(0.0);             // 3/3 → 生效
        std::printf("  第 3 个 0%% 采样后冗余=%.4f（降档生效）down_changes=%u\n",
                    ctl.RecommendedRedundancy(),
                    static_cast<unsigned>(ctl.down_changes()));
        CHECK_NEAR(ctl.RecommendedRedundancy(), 0.0, 1e-12);
        CHECK(ctl.down_changes() == 1);
        CHECK(ctl.changes() == 2);          // 一升一降，没有多余动作
    }

    // ---- 5.3 升降门限不同：上升 2 个百分点就动，下降要 5 个百分点 ----
    {
        FecController ctl;                  // 默认 up=0.02 / down=0.05
        CHECK_NEAR(ctl.config().up_margin, 0.02, 1e-12);
        CHECK_NEAR(ctl.config().down_margin, 0.05, 1e-12);
        CHECK(ctl.config().up_margin < ctl.config().down_margin);
        std::printf("  升降门限：up=%.2f < down=%.2f（升快降慢）\n", ctl.config().up_margin,
                    ctl.config().down_margin);

        // 小幅下降（目标只低 3 个百分点，< 5 个百分点）不应触发降档。
        ctl.OnLossSample(0.10);             // 升到 ~0.157
        const double before = ctl.RecommendedRedundancy();
        const std::uint32_t down_before = ctl.down_changes();
        for (int i = 0; i < 20; ++i) {
            ctl.OnLossSample(0.085);        // 目标约 0.131，差距 2.6 个百分点
        }
        std::printf("  目标下降 2.6 个百分点（< down_margin 5）→ 冗余 %.4f → %.4f，未降\n",
                    before, ctl.RecommendedRedundancy());
        CHECK(ctl.down_changes() == down_before);
        CHECK_NEAR(ctl.RecommendedRedundancy(), before, 1e-12);
    }
}

// ---------------------------------------------------------------------------
// 6. 闭环双向：丢包恢复后冗余必须降回下限。
// ---------------------------------------------------------------------------

void TestClosedLoopRecovery() {
    std::printf("\n=== 6. 闭环双向：10%% → 0%% 恢复后冗余降回下限 ===\n");
    FecController ctl;
    for (int i = 0; i < 30; ++i) {
        ctl.OnLossSample(0.10);
    }
    const double high = ctl.RecommendedRedundancy();
    std::printf("  10%% 丢包段：冗余=%.4f（%.1f%%）\n", high, high * 100.0);
    CHECK_IN(high, 0.10, 0.20);

    int recovered_at = -1;
    for (int i = 0; i < 100; ++i) {
        ctl.OnLossSample(0.0);
        if (recovered_at < 0 && std::fabs(ctl.RecommendedRedundancy() - ctl.config().min_redundancy) <
                                    1e-12) {
            recovered_at = i + 1;
        }
    }
    std::printf("  0%% 恢复段：冗余=%.4f，在第 %d 个采样回到下限（共 100 个），down_changes=%u\n",
                ctl.RecommendedRedundancy(), recovered_at,
                static_cast<unsigned>(ctl.down_changes()));
    CHECK_NEAR(ctl.RecommendedRedundancy(), ctl.config().min_redundancy, 1e-12);
    CHECK(recovered_at > 0);
    CHECK(recovered_at <= 20);          // EWMA 6~7 个采样 + 3 次降档确认
    CHECK(ctl.down_changes() >= 1);
    CHECK(ctl.up_changes() == 1);
}

// ---------------------------------------------------------------------------
// 7. 配置边界与不变量。
// ---------------------------------------------------------------------------

void TestConfigAndInvariants() {
    std::printf("\n=== 7. 配置边界 / 不变量 / Reset ===\n");

    // 7.1 非法配置被修正，且不会破坏区间关系。
    {
        FecControllerConfig cfg;
        cfg.ewma_alpha = 0.0;              // 会让 EWMA 永不更新
        cfg.loss_low = 0.9;
        cfg.loss_high = 0.1;               // 上下限颠倒
        cfg.min_redundancy = 0.8;
        cfg.max_redundancy = 0.1;          // 倒挂
        FecController ctl(cfg);
        std::printf("  α=0→%.2f  low=0.9/high=0.1→[%.3f,%.3f]  min/max 倒挂→[%.3f,%.3f]\n",
                    ctl.config().ewma_alpha, ctl.config().loss_low, ctl.config().loss_high,
                    ctl.config().min_redundancy, ctl.config().max_redundancy);
        CHECK_NEAR(ctl.config().ewma_alpha, 0.25, 1e-12);
        CHECK(ctl.config().loss_high > ctl.config().loss_low);
        CHECK(ctl.config().min_redundancy <= ctl.config().max_redundancy);
        CHECK(ctl.config().max_redundancy <= FecController::kMaxTotalProtectionOverhead);
        CHECK_NEAR(ctl.config().min_redundancy, 0.50, 1e-12);   // 0.8 被夹到 50%
        std::printf("  min 0.8 被夹到 %.2f（50%% 硬上限）\n", ctl.config().min_redundancy);
        for (int i = 0; i < 10; ++i) {
            ctl.OnLossSample(0.5);
        }
        CHECK_IN(ctl.RecommendedRedundancy(), ctl.config().min_redundancy,
                 ctl.config().max_redundancy);
    }

    // 7.2 坏采样不破坏不变量：NaN/负数按「无丢包」，>1 按「100% 丢包」。
    {
        FecController zero_bad;
        const double zeros[] = {-1.0, -0.0, std::nan("")};
        for (double loss : zeros) {
            zero_bad.OnLossSample(loss);
        }
        const double r0 = zero_bad.RecommendedRedundancy();
        std::printf("  NaN/负数采样后：冗余=%.4f（按 0 丢包处理）samples=%u\n", r0,
                    static_cast<unsigned>(zero_bad.samples()));
        CHECK(!std::isnan(r0));
        CHECK_IN(r0, zero_bad.config().min_redundancy, zero_bad.config().max_redundancy);
        CHECK(zero_bad.samples() == 3);
        CHECK(r0 <= 0.03);

        FecController high_bad;
        const double highs[] = {2.0, 1e9};
        for (double loss : highs) {
            high_bad.OnLossSample(loss);
        }
        const double r1 = high_bad.RecommendedRedundancy();
        std::printf("  >1 采样后：冗余=%.4f（按 100%% 丢包处理，夹到上限）samples=%u\n", r1,
                    static_cast<unsigned>(high_bad.samples()));
        CHECK(!std::isnan(r1));
        CHECK_IN(r1, high_bad.config().min_redundancy, high_bad.config().max_redundancy);
        CHECK(high_bad.samples() == 2);
        CHECK_NEAR(r1, 0.50, 1e-12);
    }

    // 7.3 分片换算边界。
    {
        FecController ctl;
        for (int i = 0; i < 20; ++i) {
            ctl.OnLossSample(0.10);
        }
        CHECK(ctl.RecommendedParityShards(0) == 0);
        CHECK(ctl.RecommendedParityShards(8, 0) == 0);
        CHECK(ctl.RecommendedParityShards(8, 1) == 1);      // 上限夹紧
        CHECK(ctl.RecommendedParityShards(64) == 10);       // 0.157×64=10.06 → 10
        std::printf("  parity 换算：k=0→%u，max=0→%u，k=8/max=1→%u，k=64→%u\n",
                    static_cast<unsigned>(ctl.RecommendedParityShards(0)),
                    static_cast<unsigned>(ctl.RecommendedParityShards(8, 0)),
                    static_cast<unsigned>(ctl.RecommendedParityShards(8, 1)),
                    static_cast<unsigned>(ctl.RecommendedParityShards(64)));
    }

    // 7.4 Reset 回到构造态（配置保留）。
    {
        FecController ctl;
        for (int i = 0; i < 20; ++i) {
            ctl.OnLossSample(0.30);
        }
        CHECK(ctl.changes() > 0);
        ctl.Reset();
        std::printf("  Reset：冗余=%.4f samples=%u changes=%u（配置 min=%.2f 保留）\n",
                    ctl.RecommendedRedundancy(), static_cast<unsigned>(ctl.samples()),
                    static_cast<unsigned>(ctl.changes()), ctl.config().min_redundancy);
        CHECK_NEAR(ctl.RecommendedRedundancy(), ctl.config().min_redundancy, 1e-12);
        CHECK(ctl.samples() == 0);
        CHECK(ctl.changes() == 0);
        CHECK(ctl.up_changes() == 0);
        CHECK(ctl.down_changes() == 0);
        CHECK(!ctl.FilteredLossRate());
        // Reset 后第一个采样仍能立即生效（驻留闸门不挡冷启动）。
        ctl.OnLossSample(0.50);
        CHECK_NEAR(ctl.RecommendedRedundancy(), 0.50, 1e-12);
    }

    // 7.5 默认构造与显式默认配置等价。
    {
        FecController a;
        FecController b(FecControllerConfig{});
        for (int i = 0; i < 10; ++i) {
            a.OnLossSample(0.07);
            b.OnLossSample(0.07);
        }
        CHECK_NEAR(a.RecommendedRedundancy(), b.RecommendedRedundancy(), 1e-12);
        CHECK(a.changes() == b.changes());
    }
}

// ---------------------------------------------------------------------------
// 8. 6 个网络画像：真实丢包序列（带噪声）下的行为。
// ---------------------------------------------------------------------------

struct ProfileResult {
    const char* name = "";
    double final_redundancy = 0.0;
    double mean_redundancy = 0.0;
    std::uint32_t changes = 0;
    std::uint32_t up = 0;
    std::uint32_t down = 0;
    double filtered_loss = 0.0;
};

ProfileResult RunProfile(const char* name, const std::vector<double>& losses) {
    FecController ctl;
    std::vector<double> trace;
    for (double loss : losses) {
        ctl.OnLossSample(loss);
        trace.push_back(ctl.RecommendedRedundancy());
    }
    ProfileResult result;
    result.name = name;
    result.final_redundancy = ctl.RecommendedRedundancy();
    result.mean_redundancy = Summarize(trace, 0.005).mean;
    result.changes = ctl.changes();
    result.up = ctl.up_changes();
    result.down = ctl.down_changes();
    result.filtered_loss = ctl.FilteredLossRate();
    std::printf("  %-22s 滤波丢包=%6.2f%%  末态冗余=%5.1f%%  均值冗余=%5.1f%%  变更=%u(%u↑/%u↓)\n",
                result.name, result.filtered_loss * 100.0, result.final_redundancy * 100.0,
                result.mean_redundancy * 100.0, static_cast<unsigned>(result.changes),
                static_cast<unsigned>(result.up), static_cast<unsigned>(result.down));
    return result;
}

void TestNetworkProfiles() {
    std::printf("\n=== 8. 6 个网络画像（每个 120 个 1Hz 采样）===\n");
    XorShift64 rng(0x5eedfec0deULL);
    const std::size_t kSamples = 120;

    // 8.1 内网：0% 丢包，每 50 秒偶发一个 0.5% 采样。
    std::vector<double> intranet(kSamples, 0.0);
    for (std::size_t i = 0; i < kSamples; i += 50) {
        intranet[i] = 0.005;
    }
    // 8.2 稳定 3%，±1% 噪声。
    std::vector<double> stable3;
    for (std::size_t i = 0; i < kSamples; ++i) {
        stable3.push_back(std::max(0.0, 0.03 + rng.Uniform(-0.01, 0.01)));
    }
    // 8.3 稳定 8%，±3% 噪声。
    std::vector<double> stable8;
    for (std::size_t i = 0; i < kSamples; ++i) {
        stable8.push_back(std::max(0.0, 0.08 + rng.Uniform(-0.03, 0.03)));
    }
    // 8.4 稳定 20%，±5% 噪声。
    std::vector<double> stable20;
    for (std::size_t i = 0; i < kSamples; ++i) {
        stable20.push_back(std::max(0.0, 0.20 + rng.Uniform(-0.05, 0.05)));
    }
    // 8.5 突发：前 60 秒 0%，后 60 秒 25%（模拟基站切换 / 跨洋拥塞）。
    std::vector<double> burst(kSamples, 0.0);
    for (std::size_t i = 60; i < kSamples; ++i) {
        burst[i] = std::max(0.0, 0.25 + rng.Uniform(-0.05, 0.05));
    }
    // 8.6 高频抖动：1%↔12% 交替。
    std::vector<double> jitter;
    for (std::size_t i = 0; i < kSamples; ++i) {
        jitter.push_back((i % 2 == 0) ? 0.01 : 0.12);
    }

    const ProfileResult intranet_r = RunProfile("内网 0%", intranet);
    const ProfileResult stable3_r = RunProfile("稳定 3%", stable3);
    const ProfileResult stable8_r = RunProfile("稳定 8%", stable8);
    const ProfileResult stable20_r = RunProfile("稳定 20%", stable20);
    const ProfileResult burst_r = RunProfile("突发 0%→25%", burst);
    const ProfileResult jitter_r = RunProfile("抖动 1%↔12%", jitter);

    CHECK(intranet_r.final_redundancy <= 0.03);
    CHECK(intranet_r.changes <= 1);
    CHECK_IN(stable3_r.final_redundancy, 0.02, 0.09);
    CHECK_IN(stable8_r.final_redundancy, 0.08, 0.20);
    CHECK_IN(stable20_r.final_redundancy, 0.25, 0.50);
    CHECK(burst_r.final_redundancy >= 0.35);
    CHECK(burst_r.changes >= 1);
    CHECK(jitter_r.changes <= 4);
    CHECK(jitter_r.final_redundancy <= 0.20);

    // 单调性：丢包越高的画像，冗余不应更低（同一套参数下）。
    CHECK(stable3_r.final_redundancy <= stable8_r.final_redundancy);
    CHECK(stable8_r.final_redundancy <= stable20_r.final_redundancy);
    std::printf("  单调性：3%%(%.3f) ≤ 8%%(%.3f) ≤ 20%%(%.3f)  ✓\n", stable3_r.final_redundancy,
                stable8_r.final_redundancy, stable20_r.final_redundancy);
}

// ---------------------------------------------------------------------------
// 9. 100 万随机采样压力：不变量 + 纯计算开销。
// ---------------------------------------------------------------------------

void TestMillionSamples() {
    std::printf("\n=== 9. 100 万随机采样（不变量 + 耗时）===\n");
    XorShift64 rng(0x1234567890abcdefULL);
    FecController ctl;
    bool invariant_ok = true;
    double max_seen = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000000; ++i) {
        // 分布刻意刁钻：90% 落在 0~5%，10% 落在 30%~100%（重尾 + 突发）。
        const double u = rng.NextDouble();
        const double loss = (u < 0.9) ? rng.Uniform(0.0, 0.05) : rng.Uniform(0.30, 1.0);
        const double r = ctl.OnLossSample(loss);
        if (std::isnan(r) || r < ctl.config().min_redundancy ||
            r > ctl.config().max_redundancy) {
            invariant_ok = false;
            break;
        }
        if (r > max_seen) {
            max_seen = r;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    std::printf("  100 万采样：不变量=%s 最大冗余=%.4f 耗时=%.3f ms（单次 %.1f ns）变更=%u\n",
                invariant_ok ? "OK" : "BROKEN", max_seen, elapsed_ms,
                elapsed_ms * 1e6 / 1000000.0, static_cast<unsigned>(ctl.changes()));

    CHECK(invariant_ok);
    CHECK(ctl.samples() == 1000000);
    CHECK(max_seen <= FecController::kMaxTotalProtectionOverhead);
    CHECK(elapsed_ms < 1000.0);   // 纯标量计算，1 秒内必须跑完（实际 ~10ms）
}

// ---------------------------------------------------------------------------
// 10. 核心数据：固定 20% vs 自适应，0% 丢包下的带宽浪费。
// ---------------------------------------------------------------------------

void TestBandwidthComparison() {
    std::printf("\n=== 10. 固定 20%% vs 自适应：带宽浪费对照（100 万包 × 1200B）===\n");
    const std::uint64_t kPackets = 1000000;
    const std::uint64_t kPacketBytes = 1200;
    const double media = MediaBytes(kPackets, kPacketBytes);

    // 自适应：0% 丢包稳定后的推荐冗余（跑 60 秒）。
    FecController ctl;
    for (int i = 0; i < 60; ++i) {
        ctl.OnLossSample(0.0);
    }
    const double adaptive_r = ctl.RecommendedRedundancy();
    const std::uint16_t adaptive_m = ctl.RecommendedParityShards(8);
    CHECK(adaptive_r <= 0.03);

    // 固定 20%：每 8 个数据包配 1.6 个校验包（固定配置常见值 20%）。
    const double fixed_r = 0.20;

    // 冗余字节 = 媒体字节 × 冗余比例。
    const double fixed_extra = media * fixed_r;
    const double adaptive_extra = media * adaptive_r;
    const double saved = fixed_extra - adaptive_extra;
    const double saved_percent_of_media = saved / media * 100.0;

    std::printf("  媒体字节          ：%.0f B（%.1f MiB）\n", media, media / (1024.0 * 1024.0));
    std::printf("  固定 20%%   多传   ：%.0f B（%.1f MiB，%llu 个校验包）\n", fixed_extra,
                fixed_extra / (1024.0 * 1024.0),
                static_cast<unsigned long long>(kPackets * 20 / 100));
    std::printf("  自适应 0%%  多传   ：%.0f B（%.1f MiB，k=8 时 parity=%u）\n", adaptive_extra,
                adaptive_extra / (1024.0 * 1024.0), static_cast<unsigned>(adaptive_m));
    std::printf("  **省下冗余带宽     ：%.0f B（%.1f MiB），占媒体 %.2f%%**\n", saved,
                saved / (1024.0 * 1024.0), saved_percent_of_media);
    std::printf("  冗余开销：固定 20.00%% → 自适应 %.2f%%（相对降低 %.1f%%）\n",
                adaptive_r * 100.0, (1.0 - adaptive_r / fixed_r) * 100.0);

    // 核心数字：0% 丢包时，固定 20% 白传 20%，自适应只传 2%，差 18 个百分点。
    CHECK_NEAR(fixed_extra, media * 0.20, 1.0);
    CHECK_NEAR(saved_percent_of_media, 18.0, 0.5);
    CHECK(saved > 0.0);
    // 每 100 万包省 180000 个校验包的带宽（按 1200B ≈ 205.99 MiB）。
    std::printf("  等价于每 100 万包少传 %llu 个校验包（1200B/包）\n",
                static_cast<unsigned long long>(kPackets * 18 / 100));

    // 反面对照：差网（30% 丢包）固定 20% 保护不足，自适应到上限 50%。
    FecController bad;
    for (int i = 0; i < 60; ++i) {
        bad.OnLossSample(0.30);
    }
    const double bad_r = bad.RecommendedRedundancy();
    const double fixed_recoverable = fixed_r / (1.0 + fixed_r);   // 16.7%
    const double adaptive_recoverable = bad_r / (1.0 + bad_r);    // 33.3%
    std::printf("  30%% 丢包：固定 20%% 组内可恢复 %.1f%%（不足）、自适应 %.1f%% → 可恢复 %.1f%%\n",
                fixed_recoverable * 100.0, bad_r * 100.0, adaptive_recoverable * 100.0);
    CHECK_NEAR(bad_r, 0.50, 1e-12);
    CHECK(adaptive_recoverable > fixed_recoverable);
    CHECK(fixed_recoverable < 0.30);      // 固定 20% 补不住 30% 丢包
}

}  // namespace

int main() {
    std::printf("[fec_controller] === 自适应 FEC 冗余度测试 ===\n");
    TestZeroLoss();
    TestTenPercentLoss();
    TestCapAtFiftyPercent();
    TestJitterHysteresis();
    TestHysteresisGates();
    TestClosedLoopRecovery();
    TestConfigAndInvariants();
    TestNetworkProfiles();
    TestMillionSamples();
    TestBandwidthComparison();

    std::printf("\n[fec_controller] checks=%d failures=%d\n", g_checks, g_failures);
    if (g_failures != 0) {
        return 1;
    }
    std::printf("[fec_controller] all checks passed\n");
    return 0;
}
