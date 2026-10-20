#pragma once

/**
 * @file replay_engine.hpp
 * @brief 离线重放：文件加载、静态一致性校验、单线程按记录交错重跑。
 *
 * ## 两种重放
 *
 * 1. **静态重演**（`ReplayFile`）：不需要原程序。
 *    按事件流重建每个 gid 的操作/挂起/唤醒关系，校验：
 *    - 每个 `kPark` 是否有 `kWake`（**丢失唤醒 → stuck**）；
 *    - 每个 `blocked=1` 的操作是否有对应 park；
 *    - `xfer_id` 是否成对（send/recv 同 id、同 channel）；
 *    - `kExit` 计数与操作计数是否一致；文件是否以 `kEnd` 收尾；
 *    - `kSelect` 的 `chosen`/case 表是否自洽。
 *    CLI 用这个入口做时间线/统计/卡死判定。
 *
 * 2. **完整重放**（`ReplayFileWith` / `ReplayFileAuto`）：需要程序提供每个
 *    gid 的 fiber 入口（显式 factory，或 `RECORD_FIBER_ENTRY` 编译期注册）。
 *    在**单线程 ucontext 调度器**上按记录的 `kSpawn/kPark/kWake` 顺序恢复
 *    fiber，channel/select/clock/context 的结果全部来自记录，因此能校验
 *    「重跑产生的消息/select 决策序列与原记录一致」，也能在注入丢唤醒时
 *    真正重演卡死。
 *
 * ## 只重放到出事点
 *
 * `ReplayOptions::stop_at_seq > 0` 时只处理 `seq <= stop_at_seq` 的事件：
 * 静态分析给出 `kPartial`，完整重放保留停在检查点时的 fiber 状态而不判
 * stuck。配合 `checkpoint_every_events` / `checkpoint_every_ns` 输出检查点。
 */

#include "replay/format.hpp"
#include "replay/replay_source.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace go2cpp::replay {

/// 重放选项。
struct ReplayOptions {
    /// 是否保留逐条时间线（CLI 打印用；大文件可关掉省内存）。
    bool keep_timeline{false};
    /// 故障注入：丢弃第 N 次 `kWake`（N>=1；0 表示关闭）。
    std::uint32_t drop_wake_at{0};
    /// 遇到第一个错误/不一致就停止（默认 true）。
    bool stop_on_first_error{true};
    /// 只重放到 `seq <= stop_at_seq`（0 表示不限制）。
    std::uint64_t stop_at_seq{0};
    /// 每 N 条事件记录一个检查点（0 表示关闭）。
    std::uint32_t checkpoint_every_events{0};
    /// 每 M 纳秒（虚拟时间）记录一个检查点（0 表示关闭）。
    std::uint64_t checkpoint_every_ns{0};
};

/// 重放结论。
enum class ReplayStatus {
    kOk = 0,           ///< 记录自洽，重放完成且结果一致
    kStuck = 1,        ///< 检测到丢失唤醒/永久阻塞
    kFormatError = 2,  ///< 文件损坏/版本不符/CRC 失败
    kMismatch = 3,     ///< 记录与程序不一致（操作数、类型、配对）
    kIoError = 4,      ///< 读文件失败
    kNotRun = 5,
    kPartial = 6,      ///< 按 stop_at_seq 截断（只重放到出事点）
};

/// 结论的稳定文本名。
const char* ReplayStatusName(ReplayStatus status) noexcept;

/// 单个 gid 的统计。
struct GidStats {
    std::uint32_t gid{0};
    std::uint64_t sends{0};
    std::uint64_t recvs{0};
    std::uint64_t parks{0};
    std::uint64_t wakes{0};
    std::uint64_t selects{0};
    bool exited{false};
    bool started{false};
};

/// 时间线条目（与 `Event` 一一对应，但裁剪为 CLI 关心的字段）。
struct TimelineEntry {
    std::uint64_t seq{0};
    std::uint64_t ts_ns{0};
    std::uint64_t vtime_ns{0};
    EventType type{EventType::kNote};
    std::uint16_t flags{0};
    std::uint32_t gid{0};
    std::uint32_t channel{kNoChannel};
    std::uint32_t op_index{0};
    std::int64_t value{0};
    std::string data;  ///< bytes 消息值（hex 由 JSON 层编码）
    Decision decision{Decision::kOk};
    std::uint8_t blocked{0};
    ParkReason reason{ParkReason::kOther};
    std::uint16_t chosen{0};
};

/// 检查点（分片/二分定位「出事点」用）。
struct CheckpointInfo {
    std::uint64_t seq{0};         ///< 该检查点覆盖的最后一条事件 seq
    std::uint64_t event_index{0}; ///< 在事件数组里的下标（含）
    std::uint64_t vtime_ns{0};    ///< 虚拟时间
    std::uint64_t ts_ns{0};       ///< 录制时 steady_clock
};

/// 统计汇总（CLI 的 `--stats` 直接映射这些字段）。
struct ReplayStats {
    std::uint64_t file_events{0};    ///< 文件中的事件条数（含 kEnd）
    std::uint64_t sends{0};
    std::uint64_t recvs{0};
    std::uint64_t parks{0};
    std::uint64_t wakes{0};
    std::uint64_t closes{0};
    std::uint64_t selects{0};
    std::uint64_t timers{0};
    // v3：TimerService 生命周期细分与 fire 序列校验和。
    std::uint64_t timer_arms{0};
    std::uint64_t timer_fires{0};
    std::uint64_t timer_cancels{0};
    std::uint64_t timer_reschedules{0};
    std::uint64_t timer_checksum{0};
    std::uint64_t replayed_timer_checksum{0};
    std::vector<std::uint32_t> timer_fire_ids;
    std::vector<std::uint32_t> replayed_timer_fire_ids;
    std::uint64_t ctx_events{0};
    std::uint64_t blocked_ops{0};    ///< blocked=1 的操作数
    std::uint64_t messages{0};       ///< 成功接收的消息数（含 select 的 recv）
    std::uint64_t bytes_messages{0}; ///< 走 bytes 编码的接收消息数
    std::uint64_t dropped_wakes{0};  ///< 注入丢弃的唤醒数
    std::uint64_t synthetic_events{0};
    /// 记录中的全局接收序列校验和（FNV-1a 64，按事件 seq 顺序）。
    std::uint64_t message_checksum{0};
    /// 重放实际产生的接收序列校验和（按 fiber 执行顺序）。
    std::uint64_t replayed_message_checksum{0};
    /// select 决策序列（记录 vs 重放）。
    std::vector<std::uint32_t> select_choices;
    std::vector<std::uint32_t> replayed_select_choices;
    std::uint64_t select_checksum{0};
    std::uint64_t replayed_select_checksum{0};
    std::vector<GidStats> per_gid;   ///< 按 gid 升序
    /// 记录中的全局接收顺序（`seq` 顺序）。
    std::vector<std::int64_t> global_recv_sequence;
    /// 重放实际产生的接收顺序。
    std::vector<std::int64_t> replayed_recv_sequence;
    std::vector<std::vector<std::int64_t>> recv_by_gid;  ///< 每 gid 接收序列
};

/**
 * @brief 重放报告。
 *
 * `timeline` 仅在 `ReplayOptions::keep_timeline` 时填充。
 */
struct ReplayReport {
    ReplayStatus status{ReplayStatus::kNotRun};
    std::string error;
    std::string path;
    std::string demo_name;
    std::uint64_t file_size{0};
    std::uint32_t file_crc{0};
    bool checksum_ok{false};
    bool complete{false};   ///< 文件以 kEnd 正常收尾
    bool stuck{false};
    std::uint32_t stuck_gid{kNoChannel};
    std::uint32_t stuck_channel{kNoChannel};
    std::uint32_t stuck_op_index{0};
    std::string stuck_reason;
    bool replay_executed{false};  ///< 是否真的跑了程序（完整重放）
    bool stopped_early{false};    ///< 是否因 stop_at_seq 截断
    std::uint64_t stopped_at_seq{0};
    bool auto_entry{false};       ///< 是否使用编译期注册的入口
    std::string entry_name;
    std::uint64_t virtual_ns{0};  ///< 重放结束时的虚拟时间
    std::vector<CheckpointInfo> checkpoints;
    ReplayStats stats;
    std::vector<TimelineEntry> timeline;

    bool ok() const noexcept { return status == ReplayStatus::kOk; }
    /// 一行摘要（人类可读；JSON 见 `ReplayReportJson`）。
    std::string Summary() const;
};

/**
 * @brief 每个 gid 的 fiber 入口工厂。
 *
 * replay 模式的 channel 需要 `ReplaySource&` 才能构造，因此工厂同时拿到
 * 引擎实例；返回的函数在该 fiber 的 ucontext 栈上执行。
 */
using FiberEntryFactory =
    std::function<std::function<void()>(std::uint32_t gid, ReplaySource& source)>;

/**
 * @brief 静态重演：不需要原程序。
 *
 * 适合统一 CLI：读文件 → 校验 → 统计 → 卡死判定 → 时间线。
 */
ReplayReport ReplayFile(const std::string& path, const ReplayOptions& options = {});

/**
 * @brief 完整重放：在单线程 ucontext 上按记录交错重跑程序。
 *
 * @param factory 每个 gid 的 fiber 入口；必须用 `SyncChannel` 的 replay 模式。
 * @note 需要 `path` 的第一个事件是 `kMeta`，且 fiber 数与记录一致。
 */
ReplayReport ReplayFileWith(const std::string& path, const FiberEntryFactory& factory,
                            const ReplayOptions& options = {});

/// 报告序列化为 JSON（单行可解析，字段名稳定，供 CLI 消费）。
std::string ReplayReportJson(const ReplayReport& report);

/// 事件类型/决策/挂起原因的稳定文本名。
const char* DecisionName(Decision decision) noexcept;
const char* ParkReasonName(ParkReason reason) noexcept;

}  // namespace go2cpp::replay
