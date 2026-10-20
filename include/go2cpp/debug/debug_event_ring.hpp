#pragma once

/**
 * @file debug_event_ring.hpp
 * @brief 调度事件环：park / wake / runqueue / sync / channel 的固定字段序列（可导出文件）。
 *
 * ## 定位
 *
 * `scheduler` 的高频事件在 `SchedulerObserver` 之外还有一层**细粒度**记录：
 * 任务级事件（kTaskSuspended 等）看不到「谁在什么时刻 park、被谁唤醒、
 * 入队/出队是否成功」。本模块按 `/tmp/fix_wake_loss.md` §1.3 的格式提供
 * 每线程固定容量环形缓冲：
 *
 * ```text
 * seq ts_ns tid code gid arg2 pid
 * ```
 *
 * ## 字段语义（本轮新增 pid；旧字段位置不变，pid 追加在行尾）
 *
 * - `seq`：**进程内全局单调**序号（1 起，原子递增）。旧版是「每线程独立递增」，
 *   跨线程排序无意义；本轮改为全局唯一，`Snapshot()` 的排序与订阅游标才真正
 *   可用。代价是每次记录一次 `fetch_add`（仅 `GO2CPP_DEBUG_DUMP=ON` 构建）。
 * - `ts_ns`：steady_clock 纳秒，仅同机可比。
 * - `tid`：线程 id 的 32 位哈希。
 * - `pid`：事件发生时**当前线程绑定的 P**（`Scheduler::current_processor_id()`
 *   的发布值，由调度器在 worker 绑定/解绑时写入）。未绑定 P 的线程（IO 轮询
 *   线程、普通线程、测试线程）记录为 `kInvalidProcessorId`（`0xFFFFFFFF`）。
 * - `code`：稳定的 `SchedEventCode` 枚举（**数值不复用**）。
 * - `gid`：Task/G 编号（sync/channel 事件在无法廉价取得时写 0）。
 * - `arg2`：事件相关的附加量（park 原因、唤醒结果、目标 P、队列长度等）。
 *
 * ## 关键事件覆盖策略（本轮）
 *
 * 每线程的环形缓冲拆成**两段**：
 *
 * - **普通区**（`kCapacity`，默认 256）：park / runqueue push/pop / claim ok 等
 *   高频事件。写满后覆盖最旧的一条。
 * - **关键区**（`kCriticalCapacity`，默认 64）：wake 系列、enqueue 拒绝、
 *   claim 失败、park 被拒、诊断事件。普通事件**永远不会**写入这一段，因此
 *   高频普通事件写满 256 条后，关键事件仍然保留在关键区里。
 *
 * 判定函数是 `SchedEventCodeIsCritical()`；容量通过编译期常量配置：
 * `GO2CPP_DEBUG_EVENT_CAPACITY` / `GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY`
 * （CMake 侧对应同名缓存变量，见 `cmake/Go2CppOptions.cmake`）。
 *
 * 注意：关键区本身仍是环形缓冲 —— 关键事件超过 `kCriticalCapacity` 条时，
 * 更旧的关键事件会被覆盖（这是「不丢失全部」而不是「绝对不丢」）。dump 文本
 * 里的 `critical_event_count` / `seq_at_start` 让消费方可以判断是否发生过截断。
 *
 * ## 编译开关（零开销）
 *
 * 只有 `GO2CPP_DEBUG_DUMP=ON`（CMake option）构建才真正记录。OFF 构建中
 * `GO2CPP_DEBUG_EVENT` 宏展开为 `((void)0)`，热路径**不产生任何代码**；
 * `EventRing` 的查询接口仍可用，但明确返回 `enabled=false`。
 *
 * ## 只读与一致性
 *
 * `Snapshot()/DumpText()/DumpJson()/ReadSince()` 只读取环形缓冲，不调用调度器、
 * 不改状态。读取是 **best-effort**：与正在写入的线程并发时，单条事件字段可能被
 * 覆盖（用全局唯一的 `seq` 二次校验丢弃撕裂记录）；这适合诊断与重放，不构成
 * 精确的 happens-before 记录。
 *
 * 导出文本/JSON 自带 `seq_at_start` / `seq_at_end`：两者相同表示快照期间没有
 * 新事件发布；不同则说明存在并发写入（见 debug_dump.hpp 的一致性标记约定）。
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifndef GO2CPP_DEBUG_DUMP
#define GO2CPP_DEBUG_DUMP 0
#endif

/// 每线程普通环形区容量（编译期常量；可被 CMake 缓存变量覆盖）。
#ifndef GO2CPP_DEBUG_EVENT_CAPACITY
#define GO2CPP_DEBUG_EVENT_CAPACITY 256
#endif

/// 每线程关键环形区容量（编译期常量；可被 CMake 缓存变量覆盖）。
#ifndef GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY
#define GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY 64
#endif

namespace go2cpp::debug {

/// 未绑定 P 时记录的 pid 值（与合法 PId 不冲突）。
inline constexpr std::uint32_t kInvalidProcessorId = 0xFFFFFFFFU;

/// 「来源为 incoming/global 队列」时 `arg2` 的约定值（沿用 v1 语义）。
inline constexpr std::uint64_t kGlobalQueueTag = 0xFFFFFFFFULL;

/**
 * @brief 调度事件码（稳定；新增只能追加，不能复用旧值）。
 *
 * 数值分组：1~9 park，10~19 wake，20~29 入队/重入队，30~39 runqueue
 * 出队/认领，40~49 io，50~59 线程/生命周期，60~69 Doctor，70~79 sync，
 * 80~89 channel。
 */
enum class SchedEventCode : std::uint16_t {
    kUnknown = 0,

    kParkEnter = 1,      ///< 进入 park：gid=G，arg2=SuspendReason 数值
    kParkReturn = 2,     ///< park 返回（恢复执行）：gid=G，arg2=1 正常/0 取消
    kParkRejected = 3,   ///< park 被拒：gid=G，arg2=1 非当前 G/2 draining/3 状态机拒绝

    kWakeRequest = 10,   ///< 唤醒请求：gid=G
    kWakeEnqueued = 11,  ///< 唤醒成功入队：gid=G，arg2=入队后 runnable 计数
    kWakePending = 12,   ///< 唤醒转为 pending 令牌：gid=G（调用方不需要再入队）
    kWakeRejected = 13,  ///< 唤醒被拒：gid=G，arg2=1 未注册/2 非 kWaiting/3 入队失败
    kWakeBatch = 14,     ///< 批量唤醒：gid=0，arg2=批大小

    kEnqueue = 20,           ///< Scheduler::enqueue 入口（请求，非结果）：gid=G
    kEnqueueRejected = 21,   ///< Scheduler::enqueue 拒绝：gid=G，arg2=原因码（见实现）
    kRequeueWorker = 22,     ///< worker 收尾重入队：gid=G，arg2=1 成功
    kRequeueProcessor = 23,  ///< 定向到最近 P：gid=G，arg2=P id

    kRunQueuePush = 30,      ///< 队列节点发布：gid=G，arg2=目标 P（kGlobalQueueTag=incoming/global）
    kRunQueuePop = 31,       ///< 队列节点取出：gid=G，arg2=来源 P（kGlobalQueueTag=incoming/global）
    kRunQueueClaimOk = 32,   ///< try_mark_running 成功：gid=G
    kRunQueueClaimFail = 33, ///< try_mark_running 失败（残留节点）：gid=G，arg2=来源 P

    kIoWait = 40,            ///< IOManager 注册等待：gid=G，arg2=fd
    kIoWake = 41,            ///< IOManager 唤醒：gid=G，arg2=fd
    kIoWakeBatch = 42,       ///< IOManager 批量唤醒：arg2=批大小

    kSchedulerStart = 50,    ///< 调度器启动：arg2=P 数
    kSchedulerStop = 51,     ///< 调度器停止
    kThreadStart = 52,       ///< worker 线程启动：gid=M
    kThreadStop = 53,        ///< worker 线程退出：gid=M

    // 60~69：Doctor（在线诊断）。只在 GO2CPP_DIAGNOSTIC=ON 时由诊断模块记录；
    // 追加新值，不复用旧值。
    kDiagnosticSuspected = 60,   ///< 无进展超过阈值：arg2=无进展毫秒数
    kDiagnosticForceWake = 61,   ///< 执行了受控唤醒：gid=0，arg2=唤醒的等待者数
    kDiagnosticMutexUnlock = 62, ///< 执行了 Mutex 级解锁：gid=owner，arg2=waiters

    // 70~79：sync 原语。只在**慢路径**记录（无竞争的 Lock/Unlock 不产生事件），
    // 因此对快路径零开销。
    kMutexLock = 70,   ///< Mutex 争用：gid=等待的 G，arg2=当前等待者数（近似）
    kMutexUnlock = 71, ///< Mutex 解锁并唤醒等待者：gid=解锁的 G，arg2=1 唤醒了一个

    // 80~89：channel。只在**阻塞**路径记录（立即完成/缓冲命中不产生事件）。
    kChannelSend = 80, ///< 发送方即将 park：gid=0，arg2=capacity
    kChannelRecv = 81, ///< 接收方即将 park：gid=0，arg2=capacity
    kChannelWake = 82, ///< 唤醒一个已 park 的等待者：gid=0，arg2=1 sender/2 receiver
};

/// 事件码的稳定名字；未知值返回 "kUnknown"。
const char* SchedEventCodeName(SchedEventCode code) noexcept;

/**
 * @brief 该事件是否写入**关键区**（不会被普通事件覆盖）。
 * @details 关键集合 = wake 系列 + enqueue 拒绝 + claim 失败 + park 拒绝 + 诊断事件。
 *          普通高频事件（park 进入、runqueue push/pop、claim ok、io）走普通区。
 */
bool SchedEventCodeIsCritical(SchedEventCode code) noexcept;

/// 一条事件记录（固定字段，导出格式与 §1.3 对齐 + v2 的 pid）。
struct EventRecord {
    std::uint64_t seq{0};   ///< 全局单调序号（v2 起跨线程唯一）
    std::uint64_t ts_ns{0}; ///< steady_clock 纳秒（仅同机可比）
    std::uint64_t gid{0};   ///< Task/G 编号；M 事件时为 M 编号
    std::uint64_t arg2{0};  ///< 事件附加量（原因/结果/目标 P/队列长度）
    std::uint32_t code{0};  ///< SchedEventCode 数值
    std::uint32_t tid{0};   ///< 线程 id 的 32 位哈希
    std::uint32_t pid{0};   ///< 当前线程绑定的 P；未绑定为 kInvalidProcessorId
    std::uint32_t reserved{0}; ///< 保留（显式零，保持 8 字节对齐与未来扩展）
};

/**
 * @brief 线程局部事件环的全局访问入口。
 *
 * 每个线程一个「普通区 + 关键区」的双段环形缓冲；线程退出时自动从全局表摘除。
 * `DroppedThreads()` 统计因线程数超过上限而未注册的线程数。
 */
class EventRing final {
public:
    /// 每线程**普通区**容量（沿用旧版 `kCapacity` 语义）。
    static constexpr std::size_t kCapacity = GO2CPP_DEBUG_EVENT_CAPACITY;
    /// 每线程**关键区**容量；关键事件写满后覆盖最旧的关键事件。
    static constexpr std::size_t kCriticalCapacity =
        GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY;
    /// 每线程总容量（两段之和）。
    static constexpr std::size_t kTotalCapacity = kCapacity + kCriticalCapacity;
    /// 全局最多跟踪的线程数；超出部分不记录（计入 dropped_threads）。
    static constexpr std::size_t kMaxThreads = 128;

    static_assert(kCapacity > 0, "GO2CPP_DEBUG_EVENT_CAPACITY must be > 0");
    static_assert(kCriticalCapacity > 0,
                  "GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY must be > 0");

    /**
     * @brief 记录一条事件（当前线程的 ring）。
     * @note OFF 构建中为空操作；调用方应优先使用 `GO2CPP_DEBUG_EVENT` 宏，
     *       让编译器在 OFF 时完全消除参数求值。
     */
    static void Record(SchedEventCode code, std::uint64_t gid,
                       std::uint64_t arg2) noexcept;

    /**
     * @brief 发布当前线程绑定的 P（调度器在 worker 绑定 P 后调用）。
     * @details 记录事件时读取该值写入 `pid` 字段。OFF 构建为空操作。
     */
    static void BindProcessor(std::uint32_t pid) noexcept;
    /// 清除当前线程的 P 绑定（worker 退出时调用）；此后 pid 记为 kInvalidProcessorId。
    static void UnbindProcessor() noexcept;
    /// 当前线程已发布的 P；未绑定返回 kInvalidProcessorId。
    static std::uint32_t BoundProcessor() noexcept;

    /// 当前已注册（仍存活）的线程 ring 数。
    static std::size_t ThreadCount() noexcept;
    /// 未注册的线程数（线程数超过 kMaxThreads 时累加）。
    static std::uint64_t DroppedThreads() noexcept;
    /// 已发布事件总数（全局单调游标；OFF 构建恒为 0）。
    static std::uint64_t PublishSeq() noexcept;
    /// 采集全部线程环的事件，按 seq 升序返回最近 `max` 条（0=全部）。
    static std::vector<EventRecord> Snapshot(std::size_t max = 0);
    /**
     * @brief 返回 seq > cursor 的事件（按 seq 升序，最多 `max` 条；0=不限）。
     * @details 优先返回**最旧**的事件：若积压超过 `max`，本次只交付最旧的
     *          `max` 条，剩余留到下一次，调用方用最后一条的 seq 推进游标。
     *          用于 DebugServer 的 `SUBSCRIBE events` 增量推送。
     */
    static std::vector<EventRecord> ReadSince(std::uint64_t cursor,
                                              std::size_t max = 0);
    /// 清空所有已注册 ring 的内容（测试与重放会话切换用；**仅**用于无并发写入时）。
    static void Reset() noexcept;
    /// 本构建是否记录事件（GO2CPP_DEBUG_DUMP=ON）。
    static bool Enabled() noexcept;

    /// text dump：固定字段，一行一条事件。
    static std::string DumpText(std::size_t max = 0);
    /// JSON dump：`{"schema":"go2cpp.dump.events/1", ...}`。
    static std::string DumpJson(std::size_t max = 0);
    /// 导出到文件；`json=false` 写 text。失败时填充 error 并返回 false。
    static bool WriteToFile(const std::string& path, bool json,
                            std::string* error, std::size_t max = 0);
};

}  // namespace go2cpp::debug

/// 记录一条调度事件；OFF 构建展开为零。
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
#define GO2CPP_DEBUG_EVENT(code, gid, arg2)                              \
    ::go2cpp::debug::EventRing::Record(                                  \
        ::go2cpp::debug::SchedEventCode::code, static_cast<std::uint64_t>(gid), \
        static_cast<std::uint64_t>(arg2))
#define GO2CPP_DEBUG_BIND_PROCESSOR(pid) \
    ::go2cpp::debug::EventRing::BindProcessor(static_cast<std::uint32_t>(pid))
#define GO2CPP_DEBUG_UNBIND_PROCESSOR() \
    ::go2cpp::debug::EventRing::UnbindProcessor()
#else
#define GO2CPP_DEBUG_EVENT(code, gid, arg2) ((void)0)
#define GO2CPP_DEBUG_BIND_PROCESSOR(pid) ((void)0)
#define GO2CPP_DEBUG_UNBIND_PROCESSOR() ((void)0)
#endif
