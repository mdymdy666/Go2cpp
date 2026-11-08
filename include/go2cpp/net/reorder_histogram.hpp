/**
 * @file net/reorder_histogram.hpp
 * @brief 乱序距离直方图：用 p 分位区分「公网乱序」与「真丢包」，抑制误重传。
 *
 * @details## 背景
 * 可靠传输层普遍用「收到的 seq 跳变 = 丢包」触发重传。内网丢包≈0、乱序≈0
 * 时这个判据几乎不出错；但公网（尤其多路径 / 无线 / QUIC 共存场景）乱序率
 * 可达 1%~20%，**迟到的包会被当成丢包**，触发重传风暴：
 *
 * ```
 *   真丢包 → 重传 → 重传包占满带宽 → 后续包更晚到 → 被误判成丢包 → 更多重传
 * ```
 *
 * ## 做法
 * 维护「乱序距离 = max_seq_seen − 到达 seq」的滑动直方图：
 *
 * ```
 *   到达: 0 2 1 3 5 4 ...
 *   乱序:    1   1     1        // 样本 1,1,1
 *   p50  → 1（分位）→ tolerance = 2（等 2 个序号仍没到才算丢）
 * ```
 *
 * `IsLikelyLost(seq)` 的判据是 `gap > tolerance()`：gap 落在历史 p 分位内
 * 认为「还会到」，**不重传**；超过才判丢（可重传）。这就是 WebRTC
 * `nack_requester.cc:WaitNumberOfPackets(0.5)` 的同款思想。
 *
 * ## 依赖与定位
 * **纯计算**：只依赖 C++ 标准库，不依赖 fiber / scheduler / 任何 IO。
 * 因此可被 L2（无 IO 的可靠层）直接复用，也能裁剪进最小构建。
 *
 * 它是「重传触发器」前的**决策过滤器**：不排队、不定时、不构造报文。
 * 误判的代价不对称——**误判 → 重传风暴**（带宽浪费 + 接收端重复帧 +
 * 拥塞窗口抖动）；漏判 → 该丢的包等超时兜底，RTT 被拉长。因此判据默认
 * 保守（宁晚勿滥）。
 *
 * ## 线程模型
 * **非线程安全**。所有方法必须由同一执行流调用（通常是接收循环）。内部
 * 无锁、无原子；构造后不再分配内存。
 *
 * @author go2cpp contributors
 * @date 2026-11-08
 * @copyright MIT License
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace go2cpp {
namespace net {

/**
 * @brief `ReorderHistogram` 的可调参数。
 *
 * @details 默认值全部对照 WebRTC `modules/video_coding/nack_requester.cc`
 *          的构造参数（10 桶、128 样本窗口、50% 分位）。差异见
 *          `docs/modules/reorder_histogram.md` 第 4 节。
 */
struct ReorderHistogramConfig {
    /// 桶数；乱序距离 >= 该值时归入末桶（WebRTC `kNumReorderingBuckets = 10`）。
    std::size_t num_buckets = 10;

    /// 滑动窗口样本容量；只保留最近 N 个乱序样本（WebRTC 用 128 作
    /// `Histogram` 的 max_num_values）。
    std::size_t window = 128;

    /// 分位，默认 0.5（WebRTC `WaitNumberOfPackets(0.5)`）。越界会被夹到 [0,1]。
    double percentile = 0.5;

    /// 容忍度下限（「等几个序号」）。冷启动、样本不足、分位为 0 时使用。
    /// 取 1 表示「至少给乱序一个序号的余地」，避免第一个乱序就误判。
    std::uint64_t min_tolerance = 1;

    /// 容忍度上限；0 表示「用 num_buckets」（对齐 WebRTC：最多等 10 个序号）。
    std::uint64_t max_tolerance = 0;

    /// 样本数少于此值时不用分位、直接返回 min_tolerance（防小样本噪声）。
    /// 默认 1：有 1 个样本就开始学习；设 0 等价于 1。
    std::size_t min_samples = 1;

    /// 停流超时兜底（毫秒）：流静止超过该时长后，`IsLikelyLost(seq, now_ms)`
    /// 会把「gap 在容忍度内」的缺口也判丢（PTO 兜底）。0 = 关闭（默认）。
    std::uint64_t stale_timeout_ms = 0;

    /// 「已收到」位图的窗口（序号个数）。比它更老的 seq 因信息不足而
    /// 保守地不判丢。默认 1024 覆盖 128 样本窗口 + 最大容忍度一个数量级以上。
    std::size_t received_window = 1024;

    /// 重传包是否计入乱序统计。默认 false：重传是「已判丢后的补发」，
    /// 把它当乱序样本会污染分位（与 WebRTC 的 `is_retransmitted` 意图一致）。
    bool count_retransmitted = false;
};

/// `OnPacket` 对单个到达包的分类。
enum class PacketResult : std::uint8_t {
    kInOrder = 0,   ///< 推进了 max_seq_seen（含首包）。
    kOutOfOrder = 1, ///< 迟到的乱序包（seq < max_seq_seen 且未收过）。
    kDuplicate = 2,  ///< 重复包（该 seq 已收过）。
    kTooOld = 3,     ///< 早于位图窗口，无法判定；不参与任何统计。
};

/// 诊断计数器；只读累计，`Reset()` 清零。
struct ReorderStats {
    std::uint64_t packets = 0;            ///< OnPacket 调用总数。
    std::uint64_t in_order = 0;           ///< 按序到达（推进 max_seq_seen）。
    std::uint64_t out_of_order = 0;       ///< 乱序到达（含被忽略的重传包）。
    std::uint64_t duplicate = 0;          ///< 重复包。
    std::uint64_t too_old = 0;            ///< 超出位图窗口而忽略的包。
    std::uint64_t reorder_samples = 0;    ///< 真正进入直方图的乱序样本数。
    std::uint64_t max_reorder_distance = 0; ///< 观测到的最大乱序距离。
};

/**
 * @brief 乱序距离直方图 + 「是否可能已丢」判定。
 *
 * @details 生命周期：构造 → 对每个到达包调 `OnPacket` → 对缺口调
 *          `IsLikelyLost` 决定是否重传 → 连接结束析构。无全局状态。
 */
class ReorderHistogram {
public:
    /**
     * @brief 构造直方图并按配置分配固定容量（之后不再分配）。
     *
     * @param config [in] 参数；非法值会被就地修正（见 `Config()` 返回值），
     *        所有权不转移，内部复制一份。
     *
     * @return 无。
     *
     * ## 定位
     * 唯一初始化点；配置修正规则同时决定分位/容忍度的语义，错了会让
     * 判据整体偏保守或偏激进。
     *
     * ## 步骤
     * step1. 夹紧 num_buckets / window / received_window 到 >= 1；
     * step2. 夹紧 percentile 到 [0,1]；
     * step3. max_tolerance==0 → num_buckets；再保证 max_tolerance >= min_tolerance；
     * step4. 按最终容量分配桶计数、环形样本数组、已收位图。
     *
     * ## 并发/失败语义
     * - 调用约束：单线程构造，构造后配置只读（`Config()` 返回 const 引用）。
     * - 失败语义：不抛业务异常（仅 `std::bad_alloc`）；无 IO。
     * - 不变量：`bucket_count()` 之和 == `sample_count()`。
     */
    explicit ReorderHistogram(ReorderHistogramConfig config = ReorderHistogramConfig{});

    /**
     * @brief 记录一个到达包的序号与到达时刻。
     *
     * @param seq [in] 该包的序号；0 是合法值，不与「未初始化」混淆（内部有
     *        `has_packets` 标志）。回绕由调用方负责（本模块按单调递增处理）。
     * @param arrival_time_ms [in] 到达时刻（毫秒，调用方时钟，可为
     *        steady_clock 的毫秒数）；仅用于停流超时判定，可传 0。
     * @param is_retransmitted [in] 该包是否为重传包；true 且
     *        `config.count_retransmitted == false` 时不进直方图，只更新
     *        「已收」位图（避免重复触发）。
     *
     * @return
     * - `kInOrder`：推进了 max_seq_seen——调用方**必须**用它清理/推进重传队列；
     * - `kOutOfOrder`：迟到包——调用方**不要**把它当丢包证据，最多用于撤销
     *   已排队的重传；
     * - `kDuplicate`：重复包——调用方**不要**再次交付给上层；
     * - `kTooOld`：窗口外——调用方**不要**据此判丢（信息不足）。
     *
     * ## 定位
     * 唯一的状态推进入口；误用（如把重传包当普通乱序样本）会污染分位。
     *
     * ## 步骤
     * step1. 首包：初始化 max_seq_seen 与时刻，标记已收；
     * step2. seq > max_seq_seen：先滚动位图清掉滑出的旧位，再推进 max 与时刻；
     * step3. seq <= max_seq_seen：重复包直接返回，窗口外的返回 kTooOld；
     * step4. 窗口内缺口：置已收位；重传包（按配置）只计数不进直方图；
     * step5. 普通乱序：gap 夹到末桶后入直方图，更新统计。
     *
     * ## 并发/失败语义
     * - 调用约束：与读接口同一执行流；不可与 `IsLikelyLost` 并发。
     * - 原子性：单次调用内的状态更新是整体生效的（无中间可见态）。
     * - 不变量：`max_seq_seen` 单调不减；`has_seq(max_seq_seen)` 恒为 true。
     */
    PacketResult OnPacket(std::uint64_t seq, std::uint64_t arrival_time_ms,
                          bool is_retransmitted = false);

    /**
     * @brief 判断「序号 seq 至今未到，是否已大概率丢失」。
     *
     * @param seq [in] 待判定的序号；可已收到（返回 false）、可大于
     *        max_seq_seen（返回 false，还没轮到它）。
     *
     * @return
     * - true：gap 已超过 p 分位容忍度 → 调用方**可以**发起重传；
     * - false：在正常乱序范围内 / 已收到 / 信息不足 → 调用方**必须**继续等，
     *   **不要**重传（这里正是误重传抑制点）。
     *
     * ## 定位
     * 重传触发前的唯一判据；返回 true 的次数直接等于「重传决策数」。
     *
     * ## 步骤
     * step1. 没见过任何包、或 seq 超过 max_seq_seen → 不可能丢，false；
     * step2. 已收到（含 seq == max_seq_seen）→ false；
     * step3. gap >= received_window → 位图信息不足，保守 false；
     * step4. 否则返回 `gap > tolerance()`。
     *
     * ## 并发/失败语义
     * - 调用约束：只读，无副作用，可与同线程其它只读查询任意顺序调用。
     * - 不变量：对同一状态重复调用结果相同（纯函数）；不分配。
     */
    bool IsLikelyLost(std::uint64_t seq) const;

    /**
     * @brief 带停流兜底的判定：在 `IsLikelyLost(seq)` 基础上追加时间维度。
     *
     * @param seq [in] 待判定序号，语义同上。
     * @param now_ms [in] 当前时刻（毫秒，与 `OnPacket` 的 arrival_time_ms
     *        同一时钟）；小于 max_seq_seen 的到达时刻视为时钟回退，不做兜底。
     *
     * @return
     * - true：超出乱序容忍度，**或**流已静止 `stale_timeout_ms`；
     * - false：仍在等，**不要**重传。
     *
     * ## 定位
     * 给「丢的是最后一个可见缺口、之后再无新包推进」的场景兜底（PTO 语义）。
     * `stale_timeout_ms == 0`（默认）时等价于 `IsLikelyLost(seq)`。
     *
     * ## 步骤
     * step1. 先走无时间判据，true 直接返回；
     * step2. 未启用兜底 / 无包 / 已收 / 超窗口 → false；
     * step3. 时钟回退 → false；否则 `now - max_seq_seen_time >= stale`。
     *
     * ## 并发/失败语义
     * - 调用约束：只读；`now_ms` 单调性由调用方保证。
     * - 不变量：不改变任何状态；不分配。
     */
    bool IsLikelyLost(std::uint64_t seq, std::uint64_t now_ms) const;

    /**
     * @brief 乱序距离的 p 分位（0-based 距离，无样本返回 0）。
     *
     * @param p [in] 分位，[0,1]；越界夹紧。0 表示「最小桶」，1 表示「末桶」。
     *
     * @return 0-based 乱序距离（0..num_buckets-1）。调用方若要「等几个序号」，
     *         用 `tolerance()`（它做了 +1 与上下限夹紧，与 WebRTC 的
     *         1-based `WaitNumberOfPackets` 对齐）。
     *
     * ## 定位
     * 纯查询接口，供诊断/上报/自定义判据使用；不参与内部判决。
     *
     * ## 步骤
     * step1. 无样本 → 0；
     * step2. threshold = ceil(p * sample_count)，最近秩法；
     * step3. 从 0 号桶累加计数，首次累计 >= threshold 的桶号即为分位。
     *
     * ## 并发/失败语义
     * - 调用约束：只读，与 `OnPacket` 不能并发。
     * - 不变量：返回值 < num_buckets；不分配。
     */
    std::uint64_t ReorderPercentile(double p) const;

    /// 用 `config.percentile` 调 `ReorderPercentile(p)`。
    std::uint64_t ReorderPercentile() const;

    /**
     * @brief 当前容忍度 = 「再等几个序号仍没到才算丢」。
     *
     * @return 夹在 [`min_tolerance`, `max_tolerance`] 的整数；
     *         样本数 < `min_samples` 时返回 `min_tolerance`。
     *
     * @details 计算公式 `clamp(ReorderPercentile(percentile) + 1, min, max)`。
     *          +1 是为了对齐 WebRTC 的 1-based 返回（`InverseCdf` 先累加
     *          再自增桶号），语义上等于「允许的最大乱序距离 + 1」。
     *
     * ## 定位
     * 唯一的阈值出口；改判据只需改这里。
     *
     * ## 并发/失败语义
     * - 调用约束：只读；不分配；O(num_buckets)。
     */
    std::uint64_t tolerance() const;

    // ---- 只读查询（均 noexcept、不分配） ----

    /// 是否已收到过至少一个包（区分「seq 0 首包」与「未初始化」）。
    bool has_packets() const noexcept { return has_packets_; }

    /**
     * @brief 序号 seq 是否在「已收」位图的有效范围内且被标记为已收。
     *
     * @param seq [in] 任意序号。
     * @return 已收返回 true；比 `max_seq_seen - received_window` 更老的
     *         返回 false（**不代表没收到**，而是信息已滚出窗口）。
     */
    bool has_seq(std::uint64_t seq) const noexcept;

    /// 见过的最大序号；无包时返回 0（配合 `has_packets()` 使用）。
    std::uint64_t max_seq_seen() const noexcept { return max_seq_seen_; }

    /// 最大序号那次到达的时刻（毫秒）；无包时返回 0。
    std::uint64_t max_seq_seen_time_ms() const noexcept { return max_seq_seen_time_ms_; }

    /// 直方图当前样本数（<= config.window）。
    std::size_t sample_count() const noexcept { return sample_size_; }

    /// 第 index 个桶的样本计数；越界返回 0。
    std::size_t bucket_count(std::size_t index) const noexcept;

    /// 实际生效的配置（构造时已修正非法值）。
    const ReorderHistogramConfig& Config() const noexcept { return config_; }

    /// 累计统计（只读）。
    const ReorderStats& Stats() const noexcept { return stats_; }

    /// 清空所有状态（保留配置），用于连接复用。
    void Reset();

private:
    /// 清掉因 max_seq_seen 推进而滑出位图窗口的已收位（step2 的辅助）。
    void AdvanceWindow(std::uint64_t old_max, std::uint64_t new_max);

    /// 向环形样本窗口写入一个桶索引；满窗口时同步扣减被覆盖样本的计数。
    void AddSample(std::size_t bucket);

    /// 置「已收」位（调用方保证 seq 在有效窗口内）。
    void SetSeq(std::uint64_t seq);

    ReorderHistogramConfig config_;
    std::vector<std::uint32_t> buckets_;  ///< 各桶样本计数，长度 num_buckets。
    std::vector<std::uint32_t> samples_;  ///< 环形样本（桶索引），长度 window。
    std::vector<std::uint8_t> received_;  ///< 「已收」位图，长度 received_window。
    std::size_t sample_pos_ = 0;          ///< 环形写指针。
    std::size_t sample_size_ = 0;         ///< 当前样本数（<= window）。
    bool has_packets_ = false;
    std::uint64_t max_seq_seen_ = 0;
    std::uint64_t max_seq_seen_time_ms_ = 0;
    ReorderStats stats_;
};

}  // namespace net
}  // namespace go2cpp
