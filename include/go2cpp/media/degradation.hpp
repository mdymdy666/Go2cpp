#pragma once

/**
 * @file degradation.hpp
 * @brief 弱网降级策略：低分辨率、低帧率、码率自适应（与 BBR 联动）、断线重连。
 * @details
 * ## 为什么需要
 * 媒体链路在弱网下只有两条出路：**降质保住实时性**，或**硬扛导致卡顿/断流**。
 * 本文件把「降质」拆成三个可独立验证的旋钮 + 一个独立的链路恢复状态机：
 *
 * | 旋钮 | 实现 | 观测量 |
 * |---|---|---|
 * | 分辨率 | `320x240 → 160x120`（阶梯第 2/3 级）| `target_width/height` |
 * | 帧率 | `30 → 15 → 10`（每一级固定）| `ShouldDeliverFrame` 的实际通过率 |
 * | 码率 | 档位码率 + 档内按可用带宽线性缩放 | `adaptive_bitrate_bps()` |
 *
 * ## 与 BBR 的联动
 * Bento 的 `BandwidthEstimator`（RFC 9002 delivery-rate 简化）输出
 * `delivery_rate_bps` 与 `loss_rate`；`NetworkSample::FromBbr()` 把这两个字段
 * 取出来喂给控制器。本模块**不反向修改 BBR**，只消费它的估计——
 * 这样拥塞控制与质量决策的职责边界清晰，也便于单独回归。
 *
 * ## 滞回（hysteresis）：为什么降级快、升级慢
 * - 降级：条件持续 `downgrade_hold_ms`（默认 200ms）即执行，最多一次降
 *   `max_downgrade_step` 级——弱网恶化时犹豫的代价是卡顿；
 * - 升级：条件必须持续 `upgrade_hold_ms`（默认 3s），且要求可用带宽超过
 *   上一级的 `min_bandwidth_bps * upgrade_margin`——带宽抖动时来回切档会让
 *   画面反复缩放/变速，比稳定在低一档更糟。
 *
 * ## 断线重连
 * 独立的 `ReconnectSupervisor` 状态机：
 * `kConnected → kSuspect → kDisconnected → kReconnecting → kConnected`。
 * 退避为 `initial << n`（封顶 `max_backoff_ms`）+ 确定性伪随机抖动
 * （±`jitter_percent`%，种子可配，测试可复现）。重连成功后置位
 * `ConsumeKeyframeRequest()`，让编码器立刻发关键帧重建参考链——
 * 这是「重连」与「重连后画面能恢复」之间的关键一步，缺了它解码器会一直
 * 缺参考帧。
 *
 * ## 分辨率落地
 * `Downscale2xBgra()` 提供 2:1 盒式下采样（4 像素平均，整数运算），
 * 让 `320x240 → 160x120` 不只是一个配置项，而是可验证的像素操作。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>

// 只前向声明：头文件不拖入 Bento 的完整依赖，`FromBbr` 的实现在 .cpp 里。
namespace go2cpp::bento {
struct BandwidthStats;
}  // namespace go2cpp::bento

namespace go2cpp::media {

/// @brief 质量档位（严格单调：级数越大画质越低）。
enum class QualityLevel : std::uint8_t {
    kFull = 0,    ///< 320x240 @ 30fps（满档）。
    kReduced = 1, ///< 320x240 @ 15fps（先降帧率：分辨率不变，编码与呈现代价最低）。
    kLow = 2,     ///< 160x120 @ 15fps（再降分辨率：单帧字节数降到 1/4）。
    kMinimal = 3, ///< 160x120 @ 10fps（保底档：能让画面继续动）。
};

/// @brief 档位数量（与 `QualityLevel` 同步，编译期断言保证）。
inline constexpr std::size_t kQualityLevelCount = 4;

/// @brief 档位转可读名字（"full" / "reduced" / "low" / "minimal"）。
const char* QualityLevelName(QualityLevel level) noexcept;

/// @brief 一个档位的编码参数。
struct QualityParams {
    int width = 0;                       ///< 目标宽（像素）。
    int height = 0;                      ///< 目标高（像素）。
    int fps = 0;                         ///< 目标帧率。
    std::uint32_t bitrate_bps = 0;       ///< 编码器标称码率（bit/s）。
    std::uint32_t min_bandwidth_bps = 0; ///< 维持该档所需的最低可用带宽（bit/s）。
};

/// @brief 默认四档阶梯（分辨率 320x240→160x120，帧率 30→15→10）。
std::array<QualityParams, kQualityLevelCount> DefaultQualityLadder() noexcept;

/// @brief 降级控制器配置。
struct DegradationConfig {
    /// 档位阶梯，索引 = `static_cast<std::size_t>(QualityLevel)`。
    std::array<QualityParams, kQualityLevelCount> ladder = DefaultQualityLadder();

    /// 可用带宽的安全系数（只用 90%，给 ACK/重传/突发留余量）。
    double safety_factor = 0.90;

    /// 升级额外余量：可用带宽要超过上一级需求的 1.15 倍才升。
    double upgrade_margin = 1.15;

    /// 降级条件需持续多久（毫秒）。
    std::uint32_t downgrade_hold_ms = 200;

    /// 升级条件需持续多久（毫秒）。
    std::uint32_t upgrade_hold_ms = 3000;

    /// 重连成功后的升级静默期（毫秒），避免刚恢复就冲高档再次拥塞。
    std::uint32_t reconnect_guard_ms = 500;

    /// 单次最多降几级（防一刀切到最低）。
    std::uint32_t max_downgrade_step = 2;

    /// 丢包率阈值：>= 它开始降级。
    double loss_downgrade = 0.05;

    /// 丢包率阈值：>= 它直接降到保底档。
    double loss_severe = 0.12;

    /// 丢包率阈值：低于它才认为可以恢复。
    double loss_recover = 0.02;

    /// 带宽 EWMA 的移位量（alpha = 1 / (1 << shift)）。
    std::uint32_t bandwidth_ewma_shift = 2;
};

/// @brief 一次网络状况采样。
struct NetworkSample {
    std::uint64_t available_bps = 0; ///< 可用带宽（bit/s），通常取 BBR 的 delivery_rate。
    double loss_rate = 0.0;          ///< 丢包率（0.0..1.0）。
    std::int64_t rtt_ms = 0;         ///< 平滑 RTT（毫秒），仅记录与诊断。
    bool valid = true;               ///< false = 估计器未收敛，跳过本次决策。

    /// @brief 从 Bento 的带宽估计快照构造采样（实现见 .cpp）。
    static NetworkSample FromBbr(const go2cpp::bento::BandwidthStats& stats) noexcept;
};

/// @brief 降级/恢复的原因。
enum class DegradationReason : std::uint8_t {
    kNone = 0,     ///< 无变化。
    kBandwidth = 1,///< 可用带宽不足。
    kLoss = 2,     ///< 丢包率过高。
    kRecovery = 3, ///< 带宽恢复。
    kReconnect = 4,///< 断线重连后重建。
    kExternal = 5, ///< 外部强制切档。
};

/// @brief 原因转可读名字。
const char* DegradationReasonName(DegradationReason reason) noexcept;

/// @brief 一次档位变化事件。
struct DegradationEvent {
    bool changed = false;                              ///< 档位是否变化。
    QualityLevel from = QualityLevel::kFull;           ///< 变化前档位。
    QualityLevel to = QualityLevel::kFull;             ///< 变化后档位。
    DegradationReason reason = DegradationReason::kNone;///< 触发原因。
};

/// @brief 降级控制器统计。
struct DegradationStats {
    std::uint64_t samples = 0;       ///< Update 调用次数。
    std::uint64_t downgrades = 0;    ///< 降级次数。
    std::uint64_t upgrades = 0;      ///< 升级次数。
    std::uint64_t forced = 0;        ///< 外部强制切档次数。
    std::uint64_t reconnects = 0;    ///< 重连通知次数。
    std::uint64_t frames_passed = 0; ///< 帧率控制放行的帧数。
    std::uint64_t frames_dropped = 0;///< 帧率控制丢弃的帧数。
    std::uint64_t last_available_bps = 0; ///< 最近一次采样的可用带宽。
    std::uint64_t smoothed_bps = 0;  ///< EWMA 平滑后的可用带宽。
    double last_loss_rate = 0.0;     ///< 最近一次采样的丢包率。
    std::int64_t last_update_ms = 0; ///< 最近一次 Update 的时刻。
};

/// @brief 质量档位控制器（单线程；`Update` 非热路径，`ShouldDeliverFrame` 是热路径）。
class DegradationController {
   public:
    explicit DegradationController(const DegradationConfig& config = DegradationConfig()) noexcept;

    /// @brief 喂一次网络采样，可能触发升/降级。
    DegradationEvent Update(std::int64_t now_ms, const NetworkSample& sample) noexcept;

    /// @brief 外部强制切档（例如 UI 手动选择、或重连失败后的兜底）。
    DegradationEvent ForceLevel(std::int64_t now_ms, QualityLevel level,
                                DegradationReason reason) noexcept;

    /// @brief 断线重连成功：请求关键帧、进入升级静默期。
    DegradationEvent OnReconnected(std::int64_t now_ms) noexcept;

    /// @brief 当前档位。
    QualityLevel level() const noexcept {
        return m_level;
    }

    /// @brief 当前档位参数。
    const QualityParams& current() const noexcept {
        return m_config.ladder[static_cast<std::size_t>(m_level)];
    }

    /// @brief 目标宽 / 高 / 帧率（转发 `current()`）。
    int target_width() const noexcept {
        return current().width;
    }
    int target_height() const noexcept {
        return current().height;
    }
    int target_fps() const noexcept {
        return current().fps;
    }

    /// @brief 档位标称码率（bit/s）。
    std::uint32_t target_bitrate_bps() const noexcept {
        return current().bitrate_bps;
    }

    /// @brief 档内自适应码率：`clamp(平滑带宽 x safety, 标称/4, 标称)`。
    /// @details 档位切换是离散的，档内连续调节才能跟上带宽的缓慢变化；
    ///          下限取标称的 1/4，避免带宽估计短暂低谷时把码率压到 0。
    std::uint32_t adaptive_bitrate_bps() const noexcept;

    /// @brief 平滑后的可用带宽（bit/s）。
    std::uint64_t smoothed_bandwidth_bps() const noexcept;

    /// @brief 帧率控制：源端每产出一帧调用一次，返回是否应投递。
    /// @details 用「相位累积」而不是简单间隔判断：目标 15fps、输入 30fps 时
    ///          稳定放行一半，且不会因为一次延迟就长期漂移。
    bool ShouldDeliverFrame(std::int64_t now_ms) noexcept;

    /// @brief 取走「需要关键帧」标志（重连后重建参考链）。
    bool ConsumeKeyframeRequest() noexcept;

    /// @brief 配置（只读）。
    const DegradationConfig& config() const noexcept {
        return m_config;
    }

    /// @brief 统计快照。
    DegradationStats Snapshot() const noexcept;

   private:
    /// @brief 待确认的档位变化方向（滞回状态）。
    enum class Pending : std::uint8_t { kNone = 0, kUp = 1, kDown = 2 };

    /// @brief 应用档位（重置帧率相位）。
    void ApplyLevel(QualityLevel level) noexcept;

    /// @brief 判断 `level` 档在 `usable_bps` 下是否可维持。
    bool Fits(QualityLevel level, std::uint64_t usable_bps) const noexcept;

    DegradationConfig m_config;
    QualityLevel m_level = QualityLevel::kFull;
    Pending m_pending = Pending::kNone;
    std::int64_t m_pending_since_ms = 0;
    std::int64_t m_upgrade_blocked_until_ms = 0;
    std::int64_t m_next_due_ms = 0;
    std::int64_t m_smoothed_bps = 0;
    bool m_has_sample = false;
    bool m_paced = false;
    bool m_keyframe_request = false;
    DegradationStats m_stats;
};

/// @brief 链路状态（断线检测与重连）。
enum class LinkState : std::uint8_t {
    kConnected = 0,    ///< 正常收包。
    kSuspect = 1,      ///< 超过 `suspect_after_ms` 没包，但还没判定断线。
    kDisconnected = 2, ///< 超过 `disconnect_after_ms`，开始重连。
    kReconnecting = 3, ///< 已发出重连尝试，等结果。
    kFailed = 4,       ///< 超过 `max_attempts`，进入失败态（需外部重启或降级兜底）。
};

/// @brief 状态转可读名字。
const char* LinkStateName(LinkState state) noexcept;

/// @brief `Tick` 的返回动作。
enum class ReconnectAction : std::uint8_t {
    kNone = 0,     ///< 什么都不用做。
    kProbe = 1,    ///< 发起一次重连（重建链路 / 重新握手）。
    kExhausted = 2,///< 重试次数用尽，需要外部介入。
};

/// @brief 重连配置。
struct ReconnectConfig {
    std::uint32_t suspect_after_ms = 1000;  ///< 无包多久进入可疑态。
    std::uint32_t disconnect_after_ms = 3000;///< 无包多久判定断线。
    std::uint32_t initial_backoff_ms = 100; ///< 首次重连间隔。
    std::uint32_t max_backoff_ms = 5000;    ///< 退避上限。
    std::uint32_t max_attempts = 8;         ///< 最大尝试次数。
    std::uint32_t jitter_percent = 20;      ///< 退避抖动幅度（±%）。
    std::uint32_t seed = 0x9E3779B9u;       ///< 伪随机种子（测试可固定）。
};

/// @brief 重连统计。
struct ReconnectStats {
    std::uint64_t packets = 0;     ///< 收到的包数（`OnPacket` 次数）。
    std::uint64_t disconnects = 0; ///< 判定断线次数。
    std::uint64_t attempts = 0;    ///< 发起重连次数。
    std::uint64_t recoveries = 0;  ///< 恢复次数。
    std::uint64_t exhausted = 0;   ///< 进入失败态次数。
};

/// @brief 断线检测 + 指数退避重连状态机（单线程）。
class ReconnectSupervisor {
   public:
    explicit ReconnectSupervisor(const ReconnectConfig& config = ReconnectConfig()) noexcept;

    /// @brief 收到任意包：刷新存活时间；若之前在断线态则直接恢复。
    void OnPacket(std::int64_t now_ms) noexcept;

    /// @brief 推进状态机；需要发起重连时返回 `kProbe`（调用方执行一次重连）。
    ReconnectAction Tick(std::int64_t now_ms) noexcept;

    /// @brief 上报一次重连尝试的结果。
    void OnAttemptResult(std::int64_t now_ms, bool ok) noexcept;

    /// @brief 当前状态。
    LinkState state() const noexcept {
        return m_state;
    }

    /// @brief 已发起的重连次数。
    std::uint32_t attempts() const noexcept {
        return m_attempts;
    }

    /// @brief 下一次重连的时刻（毫秒）；无计划返回 0。
    std::int64_t next_attempt_at_ms() const noexcept {
        return m_next_attempt_ms;
    }

    /// @brief 取走「需要关键帧」标志。
    bool ConsumeKeyframeRequest() noexcept;

    /// @brief 复位到初始连接态（保留统计）。
    void Reset() noexcept;

    /// @brief 统计快照。
    ReconnectStats Snapshot() const noexcept;

    /// @brief 当前应使用的退避间隔（毫秒），供诊断/测试断言。
    std::uint32_t current_backoff_ms() const noexcept {
        return m_last_backoff_ms;
    }

   private:
    /// @brief 第 `attempt` 次重连的退避（含确定性抖动）。
    std::uint32_t ComputeBackoffMs(std::uint32_t attempt) noexcept;

    /// @brief xorshift32（确定性，测试可复现）。
    std::uint32_t NextRandom() noexcept;

    ReconnectConfig m_config;
    LinkState m_state = LinkState::kConnected;
    std::int64_t m_last_packet_ms = 0;
    std::int64_t m_next_attempt_ms = 0;
    std::int64_t m_disconnected_at_ms = 0;
    std::uint32_t m_attempts = 0;
    std::uint32_t m_last_backoff_ms = 0;
    std::uint32_t m_rng = 0;
    bool m_has_packet = false;
    bool m_keyframe_request = false;
    ReconnectStats m_stats;
};

/// @brief 2:1 盒式下采样（BGRA）。`src_width/src_height` 必须为偶数且 > 0。
/// @param src 源像素（`src_stride` 字节/行）。
/// @param dst 目标像素（`dst_stride` 字节/行，宽度为 `src_width / 2`）。
/// @details 4 像素整数平均（不做浮点），用于 `320x240 → 160x120` 的降级落地。
void Downscale2xBgra(const std::uint8_t* src, int src_width, int src_height, int src_stride,
                     std::uint8_t* dst, int dst_stride) noexcept;

}  // namespace go2cpp::media
