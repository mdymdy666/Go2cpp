#pragma once

/**
 * @file jitter_buffer.hpp
 * @brief 视频包的抖动缓冲：乱序重排、去重、丢包检测、自适应深度、迟到处理。
 * @details
 * ## 为什么需要
 * 媒体链路（`transport.hpp`）把一帧切成 `<=1200B` 的分片走 UDP。弱网下同一个
 * 帧的分片会**乱序、重复、丢失**，到达间隔也会抖动（实测 ±50ms 量级）。
 * `VideoChannel` 的重组层只要缺一片就整帧丢弃，而差分编码（`kDeltaI420`）
 * 依赖参考帧，丢一帧会让后续差分帧全部无法解码——所以必须在「重组」之前
 * 对**包级序号**做重排与等待，把抖动吸收在一个有界的窗口里。
 *
 * ## 排序键的取舍：为什么用 seq 而不是 timestamp
 * RTP 的同一帧往往拆成多个包，这些包共享同一个 `timestamp`；用 timestamp 排序
 * 会产生大量并列，无法定序。因此本实现的排序键是**单调递增的包序号 `seq`**，
 * `timestamp` 只用于按 RFC 3550 估计抖动（`J += (|D| - J) / 16`，
 * `D = 到达间隔 - 媒体时间戳间隔`）。这样「重排」与「抖动度量」两件事解耦。
 *
 * ## 容器选择：环形数组，而不是优先队列 / 有序表
 * 参考的三类工业实现：
 * - **libwebrtc** `modules/video_coding/jitter_buffer.cc`（BSD-3）：帧级等待 +
 *   `std::map`/`std::list`，`O(log n)` 但每帧多次堆分配，且强依赖
 *   `RtpFrameObject`/`Clock`/`VCMEncodedFrame` 一整套 RTC 生态，移植成本过高；
 * - **pion/interceptor** `pkg/jitterbuffer`（MIT）：有序切片 + `sort.Search`，
 *   Pop `O(1)` 但 Push 插入需要 `memmove`，最坏 `O(n)`，且 Go 的接口无法直接复用；
 * - **GStreamer** `rtpjitterbuffer`（LGPL-2.1）：许可证与本项目 MIT 不兼容。
 *
 * 本项目的包序号在窗口内连续、窗口上限固定，于是用**环形数组**
 * `slot = seq & (capacity - 1)`：Push/Pop 都是均摊 `O(1)`，`2` 次数组寻址 +
 * `1` 次 `memcpy`，热路径**零堆分配**（构造期一次性分配元数据与 payload 池），
 * 且元数据数组（默认 512 x 32B = 16KB）常驻 L1。
 *
 * ## 深度自适应
 * 目标等待深度 `target = clamp(base_delay + J * jitter_factor, min, max)`。
 * `J` 是抖动的 EWMA；抖动变大 → 等待窗口变长（少丢包、多延迟），抖动收敛 →
 * 窗口自动收回（低延迟）。这是「延迟」与「完整率」之间唯一的可调旋钮，
 * 固定深度在弱网下要么大量丢包、要么永久高延迟。
 *
 * ## 迟到帧
 * `seq < next_seq` 的包已经错过了交付点。默认**丢弃**（视频解码器依赖参考帧
 * 顺序，乱序补发只会让解码器重置）；配置 `deliver_late = true` 时进入一个
 * 8 槽的补发队列，由 `Pop` 以 `kLateDelivered` 优先吐出，用于「音频/可丢弃
 * 参考帧」这类能容忍补发的负载。
 *
 * ## 线程模型
 * 单线程使用（Push/Pop/Reset 都不得并发调用）。内部**无锁、无原子操作**，
 * 这是热路径能到 ns 级的前提；需要跨线程时由调用方在外部串行化。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <memory>

namespace go2cpp::media {

/// @brief 环形槽数上限（2 的幂；4096 x 1536B ≈ 6MB 已是单个缓冲的合理上界）。
inline constexpr std::uint32_t kMaxJitterCapacity = 4096;

/// @brief 单包负载上限（字节）。
inline constexpr std::size_t kMaxJitterPayloadBytes = 65536;

/// @brief 迟到补发队列的槽数（固定，不随配置变化）。
inline constexpr std::uint32_t kJitterLateSlots = 8;

/// @brief Push 的返回码；每个值都对应一条可观测的丢包原因（不吞错误）。
enum class PushResult : std::uint8_t {
    kAccepted = 0,       ///< 进入缓冲，等待按序交付。
    kDuplicate = 1,      ///< 同 `seq` 重复到达，幂等丢弃。
    kLateQueued = 2,     ///< 迟到但在容忍窗口内，进入补发队列（`deliver_late`）。
    kDroppedLate = 3,    ///< 迟到且超出容忍窗口（或补发队列满），丢弃。
    kDroppedInvalid = 4, ///< 负载为空 / 超过 `max_payload_bytes`，丢弃。
    kDiscontinuity = 5,  ///< `seq` 跳变超过整个窗口：判定为流重启，重置后接受本包。
};

/// @brief Pop 的返回码。
enum class PopResult : std::uint8_t {
    kOk = 0,            ///< 交付了一个按序包（`out` 有效）。
    kEmpty = 1,         ///< 暂无包可交付（缓冲空，或空洞仍在等待窗口内）。
    kGapSkipped = 2,    ///< 跳过了缺失的 `seq`（计为丢包），但本次没有交付任何包。
    kLateDelivered = 3, ///< 交付了一个迟到补发包（`out->late == true`）。
};

/// @brief 缓冲配置。所有时间单位都是纳秒（`steady_clock` 口径）。
struct JitterBufferConfig {
    /// 环形槽数；会被规范化为 2 的幂并 clamp 到 `[8, kMaxJitterCapacity]`。
    std::uint32_t capacity = 512;

    /// 单包负载上限；会被向上对齐到 64B（`memcpy` 与缓存行友好）。
    std::size_t max_payload_bytes = 1500;

    /// 媒体时间戳频率（Hz）。RTP 视频惯例 90000；0 会被规范化为 90000。
    std::uint32_t timestamp_hz = 90000;

    /// 基础等待（无抖动时的排队目标）。
    std::int64_t base_delay_ns = 20'000'000;

    /// 等待深度下限（抖动极小时也不会低于它）。
    std::int64_t min_delay_ns = 10'000'000;

    /// 等待深度上限（弱网保护，避免无限追高）。
    std::int64_t max_delay_ns = 400'000'000;

    /// 抖动倍数（Q8 定点：256 = 1.0x，512 = 2.0x）。默认 2.0x 是
    /// 「覆盖约 2 倍 EWMA 抖动」的经验值；调大更抗抖但延迟更高。
    std::uint32_t jitter_factor_q8 = 512;

    /// true = 深度随抖动自适应；false = 固定 `base_delay_ns`。
    bool adaptive = true;

    /// true = 首帧交付前先攒够 `target_delay`（吸收启动期的乱序与突发）。
    bool startup_buffering = true;

    /// true = 迟到帧进入补发队列；false = 直接丢弃。
    bool deliver_late = false;

    /// 迟到容忍窗口（单位：包序号差）。超过则无论 `deliver_late` 都丢弃。
    std::uint32_t late_tolerance = 64;
};

/// @brief 零拷贝交付视图。
/// @warning `data` 指向缓冲内部池，**在下一次 Push 到同一槽位之前有效**
///          （最坏情况是 `capacity` 个包之后）。调用方必须在下一次 Push 前消费。
struct JitterPacketView {
    std::uint32_t seq = 0;              ///< 包序号。
    std::uint32_t timestamp = 0;        ///< 媒体时间戳（原样回传）。
    const std::uint8_t* data = nullptr; ///< 负载指针（缓冲内，零拷贝）。
    std::size_t bytes = 0;              ///< 负载字节数。
    std::int64_t arrival_ns = 0;        ///< 入队时刻。
    std::int64_t held_ns = 0;           ///< 在缓冲中停留的时间（`now - arrival`）。
    bool late = false;                  ///< true = 迟到补发包。
};

/// @brief 可观测统计快照（拷贝，非热路径）。
struct JitterStats {
    std::uint64_t received = 0;        ///< Push 调用次数（含重复/非法）。
    std::uint64_t accepted = 0;        ///< 真正入队的包数。
    std::uint64_t delivered = 0;       ///< 按序交付的包数。
    std::uint64_t duplicates = 0;      ///< 重复包数。
    std::uint64_t dropped_late = 0;    ///< 迟到丢弃数。
    std::uint64_t late_delivered = 0;  ///< 迟到补发交付数。
    std::uint64_t dropped_invalid = 0; ///< 非法负载数。
    std::uint64_t overwrites = 0;      ///< 同槽被新包覆盖（窗口滑过的旧包）。
    std::uint64_t reordered = 0;       ///< 乱序到达但被正确重排的包数。
    std::uint64_t gap_events = 0;      ///< 空洞跳过事件数。
    std::uint64_t lost_packets = 0;    ///< 判定丢失的包数（空洞长度累计）。
    std::uint64_t discontinuities = 0; ///< 序号跳变（流重启）次数。
    std::uint64_t max_buffered = 0;    ///< 同时占用的槽数峰值。
    std::uint32_t capacity = 0;        ///< 生效容量。
    std::uint32_t buffered = 0;        ///< 当前占用的槽数。
    std::int64_t target_delay_ns = 0;  ///< 当前目标等待深度。
    std::int64_t jitter_ns = 0;        ///< 当前抖动估计（RFC 3550 EWMA）。
};

/// @brief 包级抖动缓冲（单线程、有界内存、零拷贝交付）。
class JitterBuffer {
   public:
    /// @brief 按配置构造；构造期一次性分配元数据与 payload 池。
    explicit JitterBuffer(const JitterBufferConfig& config = JitterBufferConfig());
    ~JitterBuffer();

    JitterBuffer(const JitterBuffer&) = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;
    JitterBuffer(JitterBuffer&&) = delete;
    JitterBuffer& operator=(JitterBuffer&&) = delete;

    /// @brief 入队一个包。
    /// @param seq 单调包序号（可回绕，内部用 `int32_t` 差值判新旧）。
    /// @param timestamp 媒体时间戳（单位见 `timestamp_hz`），用于抖动估计。
    /// @param payload 负载，会被拷贝进内部池。
    /// @param bytes 负载字节数，必须 `<= max_payload_bytes`。
    /// @param arrival_ns 到达时刻（纳秒，调用方时钟）。
    /// @return 见 `PushResult`；热路径（`kAccepted`）不分配内存。
    PushResult Push(std::uint32_t seq, std::uint32_t timestamp, const std::uint8_t* payload,
                    std::size_t bytes, std::int64_t arrival_ns) noexcept;

    /// @brief 按序取下一个包。
    /// @param out 交付视图；仅在返回 `kOk` / `kLateDelivered` 时被填写。
    /// @param now_ns 当前时刻（纳秒）。空洞超过 `target_delay` 才判定丢包。
    PopResult Pop(JitterPacketView* out, std::int64_t now_ns) noexcept;

    /// @brief 排空模式：忽略等待窗口，立即交付/跳过（用于断流或降级切档）。
    PopResult PopFlush(JitterPacketView* out, std::int64_t now_ns) noexcept;

    /// @brief 清空缓冲并复位全部状态（等价于重建，但保留已分配内存）。
    void Reset() noexcept;

    /// @brief 下一个期望交付的序号。
    std::uint32_t next_seq() const noexcept {
        return m_next_seq;
    }

    /// @brief 已见过的最大序号。
    std::uint32_t highest_seq() const noexcept {
        return m_highest_seq;
    }

    /// @brief 当前占用的槽数。
    std::uint32_t buffered() const noexcept {
        return m_buffered;
    }

    /// @brief 生效容量（2 的幂）。
    std::uint32_t capacity() const noexcept {
        return m_capacity;
    }

    /// @brief 当前目标等待深度（纳秒）。
    std::int64_t target_delay_ns() const noexcept {
        return m_target_delay_ns;
    }

    /// @brief 当前抖动估计（纳秒）。
    std::int64_t jitter_ns() const noexcept {
        return m_jitter_ns;
    }

    /// @brief 缓冲占用的总字节数（与流量无关的常量，证明内存有界）。
    std::size_t allocated_bytes() const noexcept;

    /// @brief 统计快照。
    JitterStats Snapshot() const noexcept;

   private:
    /// @brief 一个环形槽的元数据；payload 在同下标的池里。
    /// @details 刻意**不使用默认成员初始化器**：这样 `SlotMeta` 是平凡类型，
    ///          `new SlotMeta[n]()`（值初始化）与 `memset` 都合法，且
    ///          `-Wclass-memaccess` 不会把清零操作判为未定义行为。
    struct SlotMeta {
        std::uint32_t seq;       ///< 槽内包的序号。
        std::uint32_t timestamp; ///< 媒体时间戳。
        std::uint32_t bytes;     ///< 负载字节数。
        std::uint8_t occupied;   ///< 0 = 空，1 = 有效。
        std::uint8_t pad[3];     ///< 显式填充，保持 24B 布局。
        std::int64_t arrival_ns; ///< 入队时刻。
    };

    /// @brief 迟到补发队列的一项（payload 在 `m_late_pool`）；同样是平凡类型。
    struct LateMeta {
        std::uint32_t seq;
        std::uint32_t timestamp;
        std::uint32_t bytes;
        std::uint32_t pad;
        std::int64_t arrival_ns;
    };

    /// @brief 抖动估计 + 深度更新（RFC 3550 风格，整数运算）。
    void UpdateJitter(std::uint32_t timestamp, std::int64_t arrival_ns) noexcept;

    /// @brief 由 `m_jitter_ns` 重算目标深度。
    void UpdateDepth() noexcept;

    /// @brief 流重启：清空窗口并把锚点移到 `seq`。
    void ResetTo(std::uint32_t seq) noexcept;

    /// @brief 迟到包入补发队列。
    void PushLate(std::uint32_t seq, std::uint32_t timestamp, const std::uint8_t* payload,
                  std::size_t bytes, std::int64_t arrival_ns) noexcept;

    /// @brief Pop/PopFlush 的共同实现。
    PopResult PopInternal(JitterPacketView* out, std::int64_t now_ns, bool flush) noexcept;

    JitterBufferConfig m_config;                 ///< 规范化后的配置。
    std::unique_ptr<SlotMeta[]> m_meta;          ///< 环形元数据（capacity 项）。
    std::unique_ptr<std::uint8_t[]> m_pool;      ///< payload 池（capacity x slot_bytes）。
    std::unique_ptr<LateMeta[]> m_late_meta;     ///< 迟到队列元数据（仅 deliver_late）。
    std::unique_ptr<std::uint8_t[]> m_late_pool; ///< 迟到 payload 池（仅 deliver_late）。

    std::uint32_t m_capacity = 0;   ///< 生效槽数（2 的幂）。
    std::uint32_t m_mask = 0;       ///< capacity - 1。
    std::size_t m_slot_bytes = 0;   ///< 每槽 payload 容量（64B 对齐）。

    std::uint32_t m_next_seq = 0;    ///< 期望交付的下一个序号。
    std::uint32_t m_highest_seq = 0; ///< 已见过的最大序号。
    std::uint32_t m_buffered = 0;    ///< 占用槽数。
    std::uint32_t m_hole_seq = 0;    ///< 当前空洞计时对应的序号。
    std::uint32_t m_late_head = 0;   ///< 迟到队列头。
    std::uint32_t m_late_count = 0;  ///< 迟到队列长度。

    std::int64_t m_target_delay_ns = 0;  ///< 当前目标深度。
    std::int64_t m_jitter_ns = 0;        ///< 抖动 EWMA。
    std::int64_t m_prev_arrival_ns = 0;  ///< 上一包到达时刻。
    std::int64_t m_hole_deadline_ns = 0; ///< 当前空洞的判定时刻。
    std::int64_t m_first_arrival_ns = 0; ///< 首包到达时刻。
    /// @brief 时间戳 -> 纳秒的定点比例（Q16）。构造期算好 `1e9/hz`，
    ///        热路径只需一次 64 位乘法 + 右移，避免运行时 64 位除法（~20-30 周期）。
    std::int64_t m_ts_scale_q16 = 0;

    std::uint32_t m_prev_timestamp = 0; ///< 上一包媒体时间戳。
    bool m_anchored = false;            ///< 是否已锚定首包。
    bool m_has_first_arrival = false;   ///< `m_first_arrival_ns` 是否有效。
    bool m_has_timing = false;          ///< `m_prev_*` 是否有效。
    bool m_delivered_any = false;       ///< 是否已交付过任何包。

    JitterStats m_stats; ///< 累计统计（热路径自增）。
};

}  // namespace go2cpp::media
