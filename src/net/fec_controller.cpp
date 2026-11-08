/**
 * @file net/fec_controller.cpp
 * @brief `FecController` 的实现：EWMA 滤波 + 分段线性映射 + 三道滞后闸门。
 *
 * 所有状态都在对象内、全部是标量；无锁、无 IO、无时钟调用、无堆分配。
 * 设计取舍见 `docs/modules/fec_controller.md`。
 */
#include "go2cpp/net/fec_controller.hpp"

#include <cmath>
#include <limits>

namespace go2cpp {
namespace net {

namespace {

/// 把 x 夹到 [lo, hi]。NaN 会因为两次比较都为假而落到 lo——
/// 这正是我们想要的「坏采样按无丢包处理」，不必额外判 isnan。
double Clamp(double x, double lo, double hi) {
    if (!(x >= lo)) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

/// 计数饱和自增：长时间运行（1Hz、数月）不担心 uint32 回绕。
void SaturatingIncrement(std::uint32_t* value) {
    constexpr std::uint32_t kMax = std::numeric_limits<std::uint32_t>::max();
    if (*value < kMax) {
        ++*value;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造：先修正配置，再把状态置成「好网 + 允许第一个采样立即生效」。
// ---------------------------------------------------------------------------
FecController::FecController(FecControllerConfig config) : config_(config) {
    NormalizeConfig();
    redundancy_ = config_.min_redundancy;
    // 冷启动时驻留闸门不能挡住第一次调整，否则要等 dwell 个周期才响应真实丢包。
    since_change_ = config_.dwell_samples;
}

void FecController::NormalizeConfig() {
    // step1: α 必须在 (0,1]；0 会让 EWMA 永不更新，>1 / NaN 会发散。
    if (!(config_.ewma_alpha > 0.0) || config_.ewma_alpha > 1.0) {
        config_.ewma_alpha = 0.25;
    }

    // step2: 冗余区间夹到 [0, 50%]，并保证 max >= min（颠倒时退化为恒定下限）。
    config_.min_redundancy =
        Clamp(config_.min_redundancy, 0.0, kMaxTotalProtectionOverhead);
    config_.max_redundancy =
        Clamp(config_.max_redundancy, config_.min_redundancy, kMaxTotalProtectionOverhead);

    // step3: 丢包率区间夹到 [0,1]；high 必须严格大于 low，否则分段线性退化成
    //        阶跃并在 span 上除零。high 无法再加时让它等于 low，剩下靠
    //        RedundancyForLoss 的边界分支兜住。
    config_.loss_low = Clamp(config_.loss_low, 0.0, 1.0);
    config_.loss_high = Clamp(config_.loss_high, 0.0, 1.0);
    if (!(config_.loss_high > config_.loss_low)) {
        constexpr double kEpsilon = 1e-3;
        config_.loss_high =
            (config_.loss_low < 1.0) ? config_.loss_low + kEpsilon : config_.loss_low;
    }

    // step4: 确认/预热采样数至少为 1（0 与 1 语义等价）；门限不能为负。
    if (config_.down_confirm_samples == 0) {
        config_.down_confirm_samples = 1;
    }
    if (config_.min_samples_before_adapt == 0) {
        config_.min_samples_before_adapt = 1;
    }
    config_.up_margin = Clamp(config_.up_margin, 0.0, kMaxTotalProtectionOverhead);
    config_.down_margin = Clamp(config_.down_margin, 0.0, kMaxTotalProtectionOverhead);
}

double FecController::RedundancyForLoss(double loss_rate) const noexcept {
    const double loss = Clamp(loss_rate, 0.0, 1.0);

    // 死区内（含 0% 与 NaN）取上限；这里不为一两个丢包付冗余。
    if (!(loss > config_.loss_low)) {
        return config_.min_redundancy;
    }
    // 饱和点及以上取上限（50%），丢包再高也不无限加冗余。
    if (loss >= config_.loss_high) {
        return config_.max_redundancy;
    }

    const double span = config_.loss_high - config_.loss_low;
    const double t = (loss - config_.loss_low) / span;
    const double mapped =
        config_.min_redundancy + t * (config_.max_redundancy - config_.min_redundancy);
    return Clamp(mapped, config_.min_redundancy, config_.max_redundancy);
}

double FecController::OnLossSample(double loss_rate) {
    // step1: 夹紧采样。NaN / 负数按无丢包，>1 按 100%——坏统计不该污染状态。
    const double loss = Clamp(loss_rate, 0.0, 1.0);

    // step2: 一阶 EWMA。首个采样直接作为初值，避免从 0 慢慢爬到真实值
    //        （那会让冷启动的前几个周期保护不足）。
    if (!has_sample_) {
        filtered_loss_ = loss;
        has_sample_ = true;
    } else {
        filtered_loss_ = config_.ewma_alpha * loss + (1.0 - config_.ewma_alpha) * filtered_loss_;
    }
    SaturatingIncrement(&samples_);

    // step3: 由滤波后的丢包率算稳态目标（不含滞后）。
    const double target = RedundancyForLoss(filtered_loss_);

    // step4: 驻留计数每个采样都推进，作为「两次变更之间至少隔多久」的闸门。
    SaturatingIncrement(&since_change_);

    if (samples_ >= config_.min_samples_before_adapt) {
        if (target >= redundancy_ + config_.up_margin) {
            // 升档信号：幅度够大就排队；但必须等够驻留，否则 1%↔10% 的
            // 交替采样会把输出拖成锯齿。
            down_confirm_ = 0;
            if (since_change_ >= config_.dwell_samples && target > redundancy_) {
                redundancy_ =
                    Clamp(target, config_.min_redundancy, config_.max_redundancy);
                SaturatingIncrement(&changes_);
                SaturatingIncrement(&up_changes_);
                since_change_ = 0;
            }
        } else if (target <= redundancy_ - config_.down_margin ||
                   target <= config_.min_redundancy) {
            // 降档信号：幅度够大也要连续确认 N 次——单次异常采样
            // （例如一次统计窗口的除零或突发抖动）不该把保护拆掉。
            //
            // 特例：`target == min_redundancy` 表示丢包已回到死区（≈无丢包），
            // 这时**忽略 down_margin 直接允许降回下限**。否则绝对门限会形成
            // 「降到下限 + 5 个百分点就停住」的死区，好网永远清不掉残余冗余
            // （单测 TestClosedLoopRecovery 覆盖）。
            SaturatingIncrement(&down_confirm_);
            if (down_confirm_ >= config_.down_confirm_samples &&
                since_change_ >= config_.dwell_samples && target < redundancy_) {
                redundancy_ =
                    Clamp(target, config_.min_redundancy, config_.max_redundancy);
                SaturatingIncrement(&changes_);
                SaturatingIncrement(&down_changes_);
                since_change_ = 0;
                down_confirm_ = 0;
            }
        } else {
            // 落在滞回带内（不升也不降）：任何未完成的降档确认作废，
            // 这样「降一半又回来」的抖动不会累积成一次误降。
            down_confirm_ = 0;
        }
    }

    return redundancy_;
}

std::uint16_t FecController::RecommendedParityShards(std::uint16_t data_shards,
                                                     std::uint16_t max_parity) const noexcept {
    if (data_shards == 0 || max_parity == 0) {
        return 0;
    }
    // 四舍五入：m = round(r × k)。r 已含滞后，所以这里就是「当前该发几个校验包」。
    const double exact = redundancy_ * static_cast<double>(data_shards);
    const double rounded = std::floor(exact + 0.5);
    if (!(rounded > 0.0)) {
        return 0;
    }
    if (rounded > static_cast<double>(max_parity)) {
        return max_parity;
    }
    return static_cast<std::uint16_t>(rounded);
}

void FecController::Reset() {
    filtered_loss_ = 0.0;
    redundancy_ = config_.min_redundancy;
    samples_ = 0;
    since_change_ = config_.dwell_samples;
    down_confirm_ = 0;
    changes_ = 0;
    up_changes_ = 0;
    down_changes_ = 0;
    has_sample_ = false;
}

}  // namespace net
}  // namespace go2cpp
