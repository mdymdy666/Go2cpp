/**
 * @file net/reorder_histogram.cpp
 * @brief `ReorderHistogram` 的实现：环形样本直方图 + 已收位图 + 分位判丢。
 *
 * 所有状态都在对象内、固定容量，构造后不再分配；无锁、无 IO、无时钟调用。
 */
#include "go2cpp/net/reorder_histogram.hpp"

#include <algorithm>
#include <cmath>

namespace go2cpp {
namespace net {

// ---------------------------------------------------------------------------
// 构造：只做配置修正与固定容量分配（一次分配，之后热路径零分配）。
// ---------------------------------------------------------------------------
ReorderHistogram::ReorderHistogram(ReorderHistogramConfig config) : config_(config) {
    // step1: 容量类参数至少为 1，避免取模/除零与空桶。
    if (config_.num_buckets == 0) {
        config_.num_buckets = 1;
    }
    if (config_.window == 0) {
        config_.window = 1;
    }
    if (config_.received_window == 0) {
        config_.received_window = 1;
    }
    if (config_.min_samples == 0) {
        config_.min_samples = 1;
    }
    // step2: 分位夹到 [0,1]；用 !(x >= 0) 同时吃掉 NaN。
    if (!(config_.percentile >= 0.0)) {
        config_.percentile = 0.0;
    }
    if (config_.percentile > 1.0) {
        config_.percentile = 1.0;
    }
    // step3: max_tolerance=0 表示「用桶数」，对齐 WebRTC 最多等 10 个序号；
    //        再保证上限不低于下限（否则 clamp 语义自相矛盾）。
    if (config_.max_tolerance == 0) {
        config_.max_tolerance = config_.num_buckets;
    }
    if (config_.max_tolerance < config_.min_tolerance) {
        config_.max_tolerance = config_.min_tolerance;
    }
    // step4: 固定容量分配。
    buckets_.assign(config_.num_buckets, std::uint32_t{0});
    samples_.assign(config_.window, std::uint32_t{0});
    received_.assign(config_.received_window, std::uint8_t{0});
}

PacketResult ReorderHistogram::OnPacket(std::uint64_t seq, std::uint64_t arrival_time_ms,
                                        bool is_retransmitted) {
    ++stats_.packets;

    // step1: 首包既不乱序也不丢，只负责建立基准（max_seq_seen / 到达时刻）。
    if (!has_packets_) {
        has_packets_ = true;
        max_seq_seen_ = seq;
        max_seq_seen_time_ms_ = arrival_time_ms;
        SetSeq(seq);
        ++stats_.in_order;
        return PacketResult::kInOrder;
    }

    // step2: 推进最大序号。必须先滚动位图，否则新 seq 的 bit 与滑出的旧 seq
    //        共用同一槽位，会把「新 seq 未收到」误读成「已收到」。
    if (seq > max_seq_seen_) {
        AdvanceWindow(max_seq_seen_, seq);
        max_seq_seen_ = seq;
        max_seq_seen_time_ms_ = arrival_time_ms;
        SetSeq(seq);
        ++stats_.in_order;
        return PacketResult::kInOrder;
    }

    // step3: 重复包与窗口外的老包都不产生新的乱序证据。
    if (has_seq(seq)) {
        ++stats_.duplicate;
        return PacketResult::kDuplicate;
    }
    const std::uint64_t gap = max_seq_seen_ - seq;
    if (gap >= config_.received_window) {
        ++stats_.too_old;
        return PacketResult::kTooOld;
    }

    // step4: 窗口内缺口 → 真正迟到。先登记已收，避免同一 seq 被重复计数。
    SetSeq(seq);
    ++stats_.out_of_order;
    if (is_retransmitted && !config_.count_retransmitted) {
        // 重传包是我们上次判丢后补发的，把它当乱序样本会让分位自我强化
        // （越重传、容忍度越高、越晚判丢），故默认不采样。
        return PacketResult::kOutOfOrder;
    }

    // step5: 入直方图：超过桶数上限的乱序距离归入末桶（WebRTC Histogram 同款）。
    const std::size_t bucket = gap >= config_.num_buckets
                                   ? config_.num_buckets - 1
                                   : static_cast<std::size_t>(gap);
    AddSample(bucket);
    ++stats_.reorder_samples;
    if (gap > stats_.max_reorder_distance) {
        stats_.max_reorder_distance = gap;
    }
    return PacketResult::kOutOfOrder;
}

bool ReorderHistogram::IsLikelyLost(std::uint64_t seq) const {
    // step1: 没有基准、或 seq 尚未进入视野 → 不可能丢。
    if (!has_packets_ || seq > max_seq_seen_) {
        return false;
    }
    // step2: 已收到（max_seq_seen 本身必然已收）→ 不丢。
    if (seq == max_seq_seen_ || has_seq(seq)) {
        return false;
    }
    // step3: 比位图窗口还老：无法判断是否收到，保守地不判丢（宁可漏判）。
    const std::uint64_t gap = max_seq_seen_ - seq;
    if (gap >= config_.received_window) {
        return false;
    }
    // step4: 核心判据——乱序距离是否已超出历史 p 分位容忍度。
    return gap > tolerance();
}

bool ReorderHistogram::IsLikelyLost(std::uint64_t seq, std::uint64_t now_ms) const {
    if (IsLikelyLost(seq)) {
        return true;
    }
    // step1: 未启用停流兜底 / 无基准 / 已收 / 超窗口 → 无额外结论。
    if (config_.stale_timeout_ms == 0 || !has_packets_) {
        return false;
    }
    if (seq >= max_seq_seen_ || has_seq(seq)) {
        return false;
    }
    if (max_seq_seen_ - seq >= config_.received_window) {
        return false;
    }
    // step2: 时钟回退时不猜；否则「流已静止足够久」视为丢（PTO 语义）。
    if (now_ms < max_seq_seen_time_ms_) {
        return false;
    }
    return now_ms - max_seq_seen_time_ms_ >= config_.stale_timeout_ms;
}

std::uint64_t ReorderHistogram::ReorderPercentile(double p) const {
    if (sample_size_ == 0) {
        return 0;
    }
    // step1: 分位夹紧（NaN 归 0）。
    double clamped = p;
    if (!(clamped >= 0.0)) {
        clamped = 0.0;
    }
    if (clamped > 1.0) {
        clamped = 1.0;
    }
    // step2: 最近秩法：threshold = ceil(p * n)，p=0 时 threshold=0（最小桶）。
    const double raw = std::ceil(clamped * static_cast<double>(sample_size_));
    const std::size_t threshold = static_cast<std::size_t>(raw);
    // step3: 从最小距离桶开始累加，首次达到 threshold 的桶号即分位。
    std::size_t accumulated = 0;
    for (std::size_t bucket = 0; bucket < buckets_.size(); ++bucket) {
        accumulated += buckets_[bucket];
        if (accumulated >= threshold) {
            return bucket;
        }
    }
    return buckets_.size() - 1;
}

std::uint64_t ReorderHistogram::ReorderPercentile() const {
    return ReorderPercentile(config_.percentile);
}

std::uint64_t ReorderHistogram::tolerance() const {
    // 样本太少时分位噪声大，直接用保守下限。
    if (sample_size_ < config_.min_samples) {
        return config_.min_tolerance;
    }
    // +1 把 0-based 距离换成「再等几个序号」，与 WebRTC 1-based 返回值对齐。
    std::uint64_t value = ReorderPercentile(config_.percentile) + 1;
    if (value < config_.min_tolerance) {
        value = config_.min_tolerance;
    }
    if (value > config_.max_tolerance) {
        value = config_.max_tolerance;
    }
    return value;
}

bool ReorderHistogram::has_seq(std::uint64_t seq) const noexcept {
    if (!has_packets_ || seq > max_seq_seen_) {
        return false;
    }
    if (max_seq_seen_ - seq >= config_.received_window) {
        return false;
    }
    return received_[static_cast<std::size_t>(seq % config_.received_window)] != 0;
}

std::size_t ReorderHistogram::bucket_count(std::size_t index) const noexcept {
    if (index >= buckets_.size()) {
        return 0;
    }
    return buckets_[index];
}

void ReorderHistogram::Reset() {
    std::fill(buckets_.begin(), buckets_.end(), std::uint32_t{0});
    std::fill(samples_.begin(), samples_.end(), std::uint32_t{0});
    std::fill(received_.begin(), received_.end(), std::uint8_t{0});
    sample_pos_ = 0;
    sample_size_ = 0;
    has_packets_ = false;
    max_seq_seen_ = 0;
    max_seq_seen_time_ms_ = 0;
    stats_ = ReorderStats{};
}

void ReorderHistogram::AdvanceWindow(std::uint64_t old_max, std::uint64_t new_max) {
    const std::uint64_t delta = new_max - old_max;  // 调用方保证 new_max > old_max
    // step1: 一次跳跃跨越整个窗口 → 旧位全部失效，直接清零更省。
    if (delta >= config_.received_window) {
        std::fill(received_.begin(), received_.end(), std::uint8_t{0});
        return;
    }
    // step2: 尚未绕第一圈：这些槽位从未被写过，没有残留位需要清。
    if (old_max < config_.received_window - 1) {
        return;
    }
    // step3: 滑出的正好是新旧有效窗口的差集，共 delta 个序号。
    const std::uint64_t first = old_max - (config_.received_window - 1);
    for (std::uint64_t k = 0; k < delta; ++k) {
        const std::uint64_t seq = first + k;
        received_[static_cast<std::size_t>(seq % config_.received_window)] = 0;
    }
}

void ReorderHistogram::AddSample(std::size_t bucket) {
    if (sample_size_ == config_.window) {
        // 窗口已满：环形覆盖，先扣掉被覆盖样本的桶计数。
        --buckets_[samples_[sample_pos_]];
    } else {
        ++sample_size_;
    }
    samples_[sample_pos_] = static_cast<std::uint32_t>(bucket);
    ++buckets_[bucket];
    sample_pos_ = (sample_pos_ + 1) % config_.window;
}

void ReorderHistogram::SetSeq(std::uint64_t seq) {
    received_[static_cast<std::size_t>(seq % config_.received_window)] = 1;
}

}  // namespace net
}  // namespace go2cpp
