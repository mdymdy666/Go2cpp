/**
 * @file net/fec_controller.hpp
 * @brief 自适应 FEC 冗余度控制器：按「滤波丢包率」闭环调冗余，保护开销硬上限 50%。
 *
 * @details## 背景
 * `bento::FecEncoder` / `bento::FecDecoder` 的冗余度是**配置期固定**的
 * （`FecConfig::parity_shards / data_shards`，默认 1/8 = 12.5%）。内网或丢包
 * 恒定的链路上这没问题，但公网丢包率会在 0%~30% 之间漂：
 *
 * ```
 *   好网（0% 丢包）  ：固定 20% 冗余 → 每秒白传 20% 带宽
 *   差网（25% 丢包） ：固定 20% 冗余 → 组内只能补 16.7%，其余全靠 RTO 兜底
 * ```
 *
 * 所以要「丢包高 → 冗余高、丢包低 → 冗余低」的闭环。WebRTC 的做法见
 * `modules/video_coding/fec_controller_default.cc:UpdateFecRates()`：拿**滤波后
 * 的丢包率**（max filter）、RTT、目标码率，经 `VCMLossProtectionLogic` 算
 * `fec_rate`，并把 NACK+FEC 开销压在 50% 以内。
 *
 * ## 做法
 *
 * ```
 *   每个保护周期（建议 1s）统计一次丢包率 loss_i
 *        │
 *        ▼
 *   EWMA: f_i = α·loss_i + (1−α)·f_{i−1}            ← 吃掉瞬时抖动
 *        │
 *        ▼
 *   稳态映射 target = piecewise_linear(f_i)          ← 丢包率 → 冗余比例
 *        │
 *        ▼
 *   滞后三闸门：幅度(up/down margin) + 连续确认(down_confirm) + 驻留(dwell)
 *        │
 *        ▼
 *   RecommendedRedundancy() ∈ [min_redundancy, 50%]  ← 硬上限
 * ```
 *
 * ## 定位
 * **纯计算**：只依赖 C++ 标准库，不依赖 fiber / scheduler / 时钟 / IO，也
 * 不持有任何编解码状态。它是「FEC 形状选择器」：上层每周期喂一次丢包率，
 * 拿 `RecommendedRedundancy()`（或 `RecommendedParityShards(k)`）去改
 * `FecConfig::parity_shards`。错了的代价不对称——**给低了丢包补不回来，
 * 延迟被 RTO 拉长；给高了挤占有效带宽，反而推高排队延迟**（见
 * `docs/modules/fec_controller.md` §6）。
 *
 * ## 线程模型
 * **非线程安全**。每通道/每方向一个实例，由同一执行流（通常是统计定时器）
 * 串行调用；内部无锁、无原子、无分配。
 *
 * @author go2cpp contributors
 * @date 2026-11-08
 * @copyright MIT License
 */
#pragma once

#include <cstdint>

namespace go2cpp {
namespace net {

/**
 * @brief `FecController` 的可调参数。
 *
 * @details 默认值对照 WebRTC `fec_controller_default.cc` 的「滤波丢包率 →
 *          fec_rate → 50% 开销」三段式，并把「按帧类型/码率算 fec_rate」
 *          简化为「按滤波丢包率算冗余比例」（差异见
 *          `docs/modules/fec_controller.md` §4）。
 *
 *          所有非法值（NaN / 越界 / 上下限颠倒）在构造时被**就地修正**，
 *          修正后的值由 `config()` 读回。
 */
struct FecControllerConfig {
    /// EWMA 系数 α ∈ (0,1]。越大越信当前采样（响应快、抖动大），越小越平滑。
    /// 默认 0.25 等价于「约 4 个采样的时间常数」（1Hz 采样 ≈ 4s）。
    double ewma_alpha = 0.25;

    /// 冗余下限，即「好网也发多少」。默认 0.02 = 每 50 个媒体包多 1 个校验包，
    /// 作为突发丢包的底噪保护。设 0 表示好网完全不发冗余（最省带宽）。
    double min_redundancy = 0.02;

    /// 冗余上限。**硬上限为 `kMaxTotalProtectionOverhead`（50%）**：传更大的
    /// 值会被夹到 50%，配置无法突破（对齐 WebRTC 的 50% 开销上限）。
    double max_redundancy = 0.50;

    /// 死区上沿：滤波丢包率 ≤ 该值时输出下限（不为一两个丢包加冗余）。
    double loss_low = 0.02;

    /// 饱和点：滤波丢包率 ≥ 该值时输出上限。默认 0.30——与「50% 冗余的组
    /// 只能补 1/3 丢包」的 MDS 性质对齐（见文档 §6）。
    double loss_high = 0.30;

    /// 升档门限：`target ≥ 当前冗余 + up_margin` 才升。小于 down_margin 表示
    /// 「升得快、降得慢」。
    double up_margin = 0.02;

    /// 降档门限：`target ≤ 当前冗余 − down_margin` 才降。默认 0.05 = 5 个百分点，
    /// 远大于 `up_margin`，这是滞后的第一道闸门。
    double down_margin = 0.05;

    /// 降档连续确认采样数：降档信号要连续出现 N 次才生效（防单次异常采样
    /// 把冗余打下去）。0 与 1 等价。
    std::uint32_t down_confirm_samples = 3;

    /// 两次冗余变更之间的最小采样间隔（驻留时间）。防「升一点、降一点」爬行。
    /// 0 = 不限制（不推荐）。
    std::uint32_t dwell_samples = 2;

    /// 冷启动/采样数少时是否允许调整：至少喂 N 个采样才允许改冗余。默认 1
    /// （第一个采样就生效，快速收敛）；0 与 1 等价。
    std::uint32_t min_samples_before_adapt = 1;
};

/**
 * @brief 丢包率 → 冗余比例的闭环控制器。
 *
 * @details 生命周期：构造 → 每周期 `OnLossSample(loss)` → 读
 *          `RecommendedRedundancy()` → 通道结束析构。无全局状态。
 *
 *          输出是**冗余比例**（相对媒体字节）：0.1 表示每 100 字节媒体多传
 *          10 字节校验，即 `parity_shards / data_shards = 0.1`。这个口径与
 *          现有 `FecConfig` 的 `parity_shards / data_shards` 完全一致，可直接
 *          接线；与 WebRTC `fec_rate` 的口径也一致。
 */
class FecController {
public:
    /// **总保护开销硬上限**（相对媒体字节）= 50%，对齐 WebRTC
    /// `fec_controller_default.cc` 的 NACK+FEC 开销约束。`max_redundancy`
    /// 传再大也会被夹到该值。
    static constexpr double kMaxTotalProtectionOverhead = 0.50;

    /**
     * @brief 用默认参数构造。
     *
     * @return 无。
     *
     * ## 定位
     * 默认值即推荐值（文档 §3）；需要特殊链路（卫星/专线）时用带配置的构造。
     *
     * ## 并发/失败语义
     * 不抛异常（noexcept 隐含）；构造后对象即可用。
     */
    FecController() : FecController(FecControllerConfig{}) {}

    /**
     * @brief 用给定配置构造，并修正非法参数。
     *
     * @param config [in] 参数快照；所有权不转移，内部复制一份并就地修正
     *        （NaN / 越界 / 上下限颠倒见 `## 步骤`），修正结果由 `config()` 读回。
     *
     * @return 无。
     *
     * ## 定位
     * 唯一的参数校验点。修正规则错了会让映射整体偏移或退化成「永远不变」，
     * 因此这里逐项夹紧而不是信任调用方。
     *
     * ## 步骤
     * step1. `ewma_alpha` 夹到 (0,1]——0 会让滤波器永不更新，>1 会发散；
     * step2. `min_redundancy` / `max_redundancy` 夹到 [0, 50%]，并保证
     *        `max >= min`，否则输出区间为空；
     * step3. `loss_low` / `loss_high` 夹到 [0,1]，并保证 `loss_high > loss_low`
     *        （相等会让分段线性退化成阶跃并除零）；
     * step4. `down_confirm_samples` / `min_samples_before_adapt` 至少为 1；
     * step5. 初始冗余 = 下限，驻留计数 = `dwell_samples`，使第一个采样可立即生效。
     *
     * ## 并发/失败语义
     * - 调用约束：构造期单线程；构造后不再修改 `config_`。
     * - 失败语义：不抛异常（成员均为标量）。
     * - 不变量：`min_redundancy <= max_redundancy <= 50%`；
     *   `redundancy_ ∈ [min_redundancy, max_redundancy]`。
     */
    explicit FecController(FecControllerConfig config);

    /**
     * @brief 喂一个保护周期的丢包率采样，推进闭环并返回新的推荐冗余。
     *
     * @param loss_rate [in] 本周期**原始**丢包率（丢包数 / 期望到达数），
     *        取值 [0,1]；NaN、负数按 0 处理，>1 按 1 处理。**不要预先滤波**：
     *        本方法内部做 EWMA，重复滤波会让响应变钝。
     *
     * @return 本次采样后的推荐冗余比例 ∈ [min_redundancy, 50%]；
     *         调用方**可以**直接用于改 `parity_shards`，也可以只在周期末读
     *         `RecommendedRedundancy()`。
     *
     * ## 定位
     * 唯一的状态推进点。滤波器、映射、滞后三件事都在这里串起来；错了会让
     * 冗余跟着瞬时丢包抖动（升来降去），直接把带宽浪费掉。
     *
     * ## 步骤
     * step1. 夹紧采样到 [0,1]（NaN 当 0）——上位统计器的除零/负值不该污染状态；
     * step2. 一阶 EWMA：首个采样直接作为初值（不从 0 爬），之后
     *         `f = α·loss + (1−α)·f`；
     * step3. 按 `f` 算稳态目标 `target = RedundancyForLoss(f)`（分段线性，
     *         死区内为下限、饱和点以上为上限）；
     * step4. 幅度闸门：`target ≥ 当前 + up_margin` 才考虑升，`target ≤ 当前 −
     *         down_margin` 才考虑降；其余情况清零降档确认计数（滞后第一道）；
     * step5. 驻留闸门：距上次变更不足 `dwell_samples` 个采样则本周期不变（第二道）；
     * step6. 降档确认闸门：降档信号需连续 `down_confirm_samples` 次才生效（第三道）；
     * step7. 真正变更时把输出置为 `target`（夹紧），清零驻留与确认计数，累计统计。
     *
     * ## 并发/失败语义
     * - 调用约束：同一实例必须串行调用（通常单个统计定时器）；频率建议 1Hz。
     * - 原子性：无锁；单线程下每步都看到一致的前一状态。
     * - 失败语义：不抛异常、不分配内存；任意输入（含 NaN/Inf）都保持不变量。
     * - 不变量：返回值恒在 [min_redundancy, max_redundancy]；`samples()` 单调递增。
     */
    double OnLossSample(double loss_rate);

    /// @brief 当前推荐冗余比例（相对媒体字节，∈ [下限, 50%]）。
    double RecommendedRedundancy() const noexcept {
        return redundancy_;
    }

    /**
     * @brief 把当前冗余比例换算成 FEC 的校验分片数 m。
     *
     * @param data_shards [in] 组内数据分片数 k（现有 `FecConfig::data_shards`），
     *        0 返回 0（空组无意义）。
     * @param max_parity [in] m 的上限（现有协议校验为 16，XOR 场景传 1），
     *        0 返回 0。
     *
     * @return m = round(redundancy × k)，夹到 [0, max_parity]；
     *         小 k + 下限冗余（如 k=8、2%）可能返回 0——那表示「该组不值得
     *         加校验」，不是错误。
     *
     * ## 定位
     * 接线用的唯一换算点：`FecConfig::parity_shards = ctl.RecommendedParityShards(k)`。
     * 换算式与 `parity_shards/data_shards` 的冗余定义严格互逆。
     *
     * ## 并发/失败语义
     * 只读；不抛异常；对任意 k/max_parity 输入都返回可用的 m。
     */
    std::uint16_t RecommendedParityShards(std::uint16_t data_shards,
                                          std::uint16_t max_parity = 16) const noexcept;

    /// @brief 稳态映射（不改状态）：给定滤波后丢包率时的冗余比例。
    ///        诊断/文档/单测用，正式闭环请走 `OnLossSample`。
    /// @param loss_rate [in] 丢包率；NaN/负数按 0，>1 按 1。
    /// @return ∈ [min_redundancy, max_redundancy] 的冗余比例。
    double RedundancyForLoss(double loss_rate) const noexcept;

    /// @brief 当前 EWMA 滤波后的丢包率（闭环的可见中间量，诊断用）。
    double FilteredLossRate() const noexcept {
        return filtered_loss_;
    }

    /// @brief 已喂入的采样数。
    std::uint32_t samples() const noexcept {
        return samples_;
    }

    /// @brief 冗余变更总次数（抖动测试的核心指标：越少越稳）。
    std::uint32_t changes() const noexcept {
        return changes_;
    }

    /// @brief 冗余上调次数。
    std::uint32_t up_changes() const noexcept {
        return up_changes_;
    }

    /// @brief 冗余下调次数。
    std::uint32_t down_changes() const noexcept {
        return down_changes_;
    }

    /// @brief 修正后的配置（读回非法值的实际取值）。
    const FecControllerConfig& config() const noexcept {
        return config_;
    }

    /**
     * @brief 清空全部状态，等价于「刚构造」。
     *
     * @return 无。
     *
     * ## 定位
     * 连接重建/统计窗口重开时用；不清配置。
     *
     * ## 并发/失败语义
     * 只重置标量；不抛异常；重置后不变量与构造时一致。
     */
    void Reset();

private:
    /// 把 `config_` 修正到合法域（见构造函数的 `## 步骤`）。
    void NormalizeConfig();

    FecControllerConfig config_;
    double filtered_loss_ = 0.0;   ///< EWMA 后的丢包率。
    double redundancy_ = 0.0;      ///< 当前推荐冗余（已含滞后）。
    std::uint32_t samples_ = 0;    ///< 已喂采样数。
    std::uint32_t since_change_ = 0;  ///< 距上次变更的采样数（驻留闸门）。
    std::uint32_t down_confirm_ = 0;  ///< 连续降档确认计数。
    std::uint32_t changes_ = 0;    ///< 变更总次数。
    std::uint32_t up_changes_ = 0;
    std::uint32_t down_changes_ = 0;
    bool has_sample_ = false;      ///< 是否已收到首个采样（决定 EWMA 初值）。
};

}  // namespace net
}  // namespace go2cpp
