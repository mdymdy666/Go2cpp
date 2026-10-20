#pragma once

/**
 * @file recorder.hpp
 * @brief 录制端：把决定点事件流式写成一棵稳定格式的文件。
 *
 * ## 线程模型
 *
 * `Write()` 可从任意 fiber/worker 线程调用；内部用一把互斥锁串行化文件
 * 写入，用原子量分配 `seq`。**调用约定**：产生决定点（交接、park、wake）
 * 的调用方必须持有产生该决定点的锁再调用 `Write()`，这样 `seq` 的顺序
 * 才等于真实线性化顺序（见 `format.hpp` 顶部说明）。
 *
 * ## 虚拟时钟
 *
 * `RecordOptions::clock_mode` 为 `kClockVirtual` 时，所有事件的 `vtime_ns`
 * 都取「相对录制起点」的 steady_clock 纳秒。重放端只信这个值，因此
 * `ReplayClock::SleepFor()` 可以在毫秒级内重演录制时数十秒的等待。
 *
 * ## 丢唤醒注入
 *
 * `ShouldDropWake()` 是**故障注入**入口：第 N 次唤醒返回 true 时，channel
 * 不再调用 `notify_one()`，于是等待者真的永远不被唤醒，进程卡死。记录里
 * 不会出现对应的 `kWake`，离线重放因此能判定「记录本身不完整 → 该 gid
 * 在记录结束仍处于 blocked」，即复现卡死。
 *
 * ## 流式与收尾
 *
 * 打开即写文件头与 `kMeta`；每条事件 append；`Finalize()` 写 `kEnd`（带
 * 总条数、丢弃数与整文件滚动 CRC）并 flush。`Finalize()` 可重复调用。
 */

#include "replay/format.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace go2cpp::replay {

/**
 * @brief 录制配置。
 *
 * `gid_count`/`channel_count` 只用于 `kMeta` 的统计与 CLI 展示；真实 id
 * 由调用方分配。
 */
struct RecordOptions {
    std::string path;             ///< 输出文件路径
    std::string demo_name;        ///< 写入 kMeta，便于识别录制场景
    std::uint32_t gid_count{0};
    std::uint32_t channel_count{0};
    std::uint32_t ctx_count{0};
    std::int64_t value_kind{0};   ///< `ValueKind`：0=i64、1=bytes、2=mixed
    std::uint32_t clock_mode{kClockVirtual};  ///< `kClockReal`/`kClockVirtual`
    std::uint32_t drop_wake_at{0};///< >0 时丢弃第 N 次唤醒（故障注入）
};

/// 构造一条操作结果事件（send/recv/close 共用字段布局）。
Event MakeOpEvent(EventType type, std::uint32_t gid, std::uint32_t channel,
                  std::uint32_t op_index, Decision decision, std::uint8_t blocked,
                  std::int64_t value, std::uint32_t xfer_id,
                  const std::string& data = {});

/// 构造一条 select 决策事件。
Event MakeSelectEvent(std::uint32_t gid, std::uint32_t channel,
                      std::uint32_t op_index, Decision decision,
                      std::uint8_t blocked, std::uint16_t chosen,
                      std::int64_t value, const std::string& data,
                      std::vector<SelectCaseRecord> cases,
                      std::uint32_t xfer_id);

/// 构造一条 park/wake 事件。
Event MakeParkEvent(EventType type, std::uint32_t gid, std::uint32_t channel,
                    ParkReason reason, std::uint32_t op_index,
                    std::uint32_t xfer_id);

/// 构造一条虚拟时钟事件（sleep / deadline / timeout）。
Event MakeTimerEvent(std::uint32_t gid, TimerKind kind, std::uint64_t timer_ns,
                     std::uint32_t op_index, std::string name = {});

/**
 * @brief 构造一条 TimerService 生命周期事件（v3）。
 * @param timer_id 录制端分配的定时器标识（1 起）。
 * @param kind kServiceArm/kServiceFire/kServiceCancel/kServiceReschedule。
 * @param deadline_ns 绝对的虚拟截止时间（相对录制起点）。
 * @param fired_at_ns 回调开始执行时的虚拟时间（仅 fire 有意义）。
 * @param callback_result 回调返回值（仅 fire 有意义）。
 * @param timer_source `TimerSource` 数值（0=unknown ... 7=user）。
 * @param source_gid 发起注册/取消的逻辑 fiber id（0=未知）。
 */
Event MakeTimerServiceEvent(std::uint32_t timer_id, TimerKind kind,
                            std::uint64_t deadline_ns,
                            std::uint64_t fired_at_ns,
                            std::int64_t callback_result,
                            std::uint32_t timer_source,
                            std::uint32_t source_gid);

/// 构造一条 Context 事件（cancel / deadline set / deadline fired）。
Event MakeCtxEvent(std::uint32_t ctx_id, ContextEventKind kind,
                   std::uint32_t op_index);

/**
 * @brief 线程安全的录制器。
 *
 * 生命周期：构造成功后即可 `Write`；析构会调用 `Finalize()`。对象必须比
 * 所有 channel / fiber 活得久（demo 里由 main 持有）。
 */
class Recorder final {
public:
    explicit Recorder(RecordOptions options);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    /// 文件是否已成功打开。失败原因见 `error()`。
    bool ok() const noexcept { return m_file != nullptr; }
    /// 最近一次错误（空表示无错误）。
    std::string error() const;
    /// 已经写出的事件条数（不含文件头与 kEnd）。
    std::uint64_t Records() const noexcept { return m_records.load(); }
    /// 是否已经 Finalize。
    bool finalized() const noexcept { return m_finalized.load(); }
    /// 输出路径（原样返回构造参数）。
    const std::string& path() const noexcept { return m_options.path; }
    /// 配置（只读）。
    const RecordOptions& options() const noexcept { return m_options; }

    /// 分配下一个全局序号（原子）。调用方应持有决定点锁。
    std::uint64_t NextSeq() noexcept { return m_seq.fetch_add(1, std::memory_order_relaxed); }
    /// 分配下一个 timer 标识（原子；从 1 起）。
    std::uint32_t NextTimerId() noexcept {
        return m_timer_id.fetch_add(1, std::memory_order_relaxed) + 1U;
    }
    /// 分配下一个交接 id（原子）；send/recv 两侧共享同一个值。
    std::uint32_t NextXferId() noexcept {
        return m_xfer.fetch_add(1, std::memory_order_relaxed);
    }
    /// 取该 gid 的下一个 channel 操作序号（0 起）。gid 越界返回 0。
    std::uint32_t NextOpIndex(std::uint32_t gid) noexcept {
        if (gid >= m_op_index.size()) {
            return 0;
        }
        return m_op_index[gid].fetch_add(1, std::memory_order_relaxed);
    }
    /// 取该 gid 的下一个 sleep 序号（0 起；与 channel op_index 独立）。
    std::uint32_t NextSleepIndex(std::uint32_t gid) noexcept {
        if (gid >= m_sleep_index.size()) {
            return 0;
        }
        return m_sleep_index[gid].fetch_add(1, std::memory_order_relaxed);
    }
    /// 取该 ctx 的下一个事件序号（0 起）。
    std::uint32_t NextCtxIndex(std::uint32_t ctx_id) noexcept {
        if (ctx_id >= m_ctx_index.size()) {
            return 0;
        }
        return m_ctx_index[ctx_id].fetch_add(1, std::memory_order_relaxed);
    }
    /// 当前 steady_clock 纳秒（同机比较）。
    static std::uint64_t NowNs() noexcept;
    /// 相对录制起点的虚拟纳秒（录制端所有 `vtime_ns` 都取这个值）。
    std::uint64_t VirtualNowNs() const noexcept;

    /**
     * @brief 写入一条事件。
     *
     * `seq` 由本函数分配（覆盖调用方填的值），`vtime_ns` 为 0 时补当前
     * 虚拟时间，`ts_ns` 为 0 时补当前时间。
     * @return 写入成功返回 true；文件已关闭或 I/O 失败返回 false。
     */
    bool Write(Event event);

    /**
     * @brief 故障注入判定：是否丢弃本次唤醒。
     *
     * 命中时返回 true 并写一条 `kNote`（flag=`kFlagInjected`），调用方必须
     * **跳过** `notify` 与 `kWake` 事件。全局只命中一次。
     */
    bool ShouldDropWake(std::uint32_t gid, std::uint32_t channel);

    /// 写一条自由文本 note。
    void Note(std::uint32_t gid, std::uint32_t channel, std::string text,
              std::uint16_t flags = 0);

    /// 写 kEnd 并 flush。可重复调用；第二个参数是收尾原因（诊断用）。
    void Finalize(const std::string& reason = {});

private:
    bool WriteRaw(const std::uint8_t* data, std::size_t size);
    void WriteEndLocked(const std::string& reason);

    RecordOptions m_options;
    std::FILE* m_file{nullptr};
    mutable std::mutex m_mutex;
    std::atomic<std::uint64_t> m_seq{0};
    std::atomic<std::uint32_t> m_xfer{0};
    std::atomic<std::uint32_t> m_timer_id{0};
    std::atomic<std::uint64_t> m_records{0};
    std::atomic<bool> m_finalized{false};
    std::atomic<std::uint32_t> m_wake_seen{0};
    std::atomic<bool> m_wake_dropped{false};
    std::uint32_t m_file_crc{0};
    std::uint64_t m_start_ns{0};
    std::vector<std::atomic<std::uint32_t>> m_op_index;
    std::vector<std::atomic<std::uint32_t>> m_sleep_index;
    std::vector<std::atomic<std::uint32_t>> m_ctx_index;
    std::string m_error;
    std::vector<std::uint8_t> m_scratch;
};

}  // namespace go2cpp::replay
