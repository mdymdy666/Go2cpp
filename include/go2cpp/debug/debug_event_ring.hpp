#pragma once

/**
 * @file debug_event_ring.hpp
 * @brief 调度事件环：park / wake / runqueue / sync / channel / timer / G 生命周期的固定字段序列。
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
 * - `arg3` / `arg4`：按事件码复用的语义槽。timer 事件是 kind/source_gid；
 *   park 事件是「原语原因码」/上下文；wake 事件是唤醒原因码。其它事件为 0。
 *
 * ## 关键事件覆盖策略
 *
 * 每线程的环形缓冲拆成**两段**：
 *
 * - **普通区**（`kCapacity`，默认 256）：park / runqueue push/pop / claim ok 等
 *   高频事件。写满后覆盖最旧的一条。
 * - **关键区**（`kCriticalCapacity`，默认 64）：wake 系列、enqueue 拒绝、
 *   claim 失败、park 被拒、G 终态、诊断事件。普通事件**永远不会**写入这一段，
 *   因此高频普通事件写满 256 条后，关键事件仍然保留在关键区里。
 *
 * 判定函数是 `SchedEventCodeIsCritical()`；容量通过编译期常量配置：
 * `GO2CPP_DEBUG_EVENT_CAPACITY` / `GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY`
 * （CMake 侧对应同名缓存变量，见 `cmake/Go2CppOptions.cmake`）。
 *
 * 注意：关键区本身仍是环形缓冲 —— 关键事件超过 `kCriticalCapacity` 条时，
 * 更旧的关键事件会被覆盖（这是「不丢失全部」而不是「绝对不丢」）。dump 文本
 * 里的 `critical_event_count` / `seq_at_start` 让消费方可以判断是否发生过截断。
 *
 * ## 全局累计计数（本轮）
 *
 * 环形缓冲只保留**最近**若干条事件，按窗口统计会低估长跑场景的真实次数。
 * `TotalCount(code)` 提供进程启动以来的累计发布数（atomic relaxed 递增）：
 * 覆盖率报告用它算「park 总数 / wake 总数 / enqueue 总数」，环形窗口只用于
 * 逐条配对与时间线。
 *
 * ## 原语提示（park/wake 原因）
 *
 * `PrimitiveHintScope` 是 RAII 的 **thread_local** 标记：channel/mutex/select/
 * ParkingCondition/timer 在进入阻塞或唤醒调用前压入原因码，事件记录时把它
 * 写进 `arg3`。作用域是栈式的，外层优先：channel 内部的 ParkingCondition
 * 等待会保留外层 `kChannelRecv`，而不是被改写成 `kParkingCondition`。
 * 该标记只影响诊断字段，不改变任何调度/同步语义。
 *
 * ## 编译开关（零开销）
 *
 * 只有 `GO2CPP_DEBUG_DUMP=ON`（CMake option）构建才真正记录。OFF 构建中
 * `GO2CPP_DEBUG_EVENT` / `GO2CPP_DEBUG_PARK_HINT` / `GO2CPP_DEBUG_EVENT_SAMPLED`
 * 等宏展开为 `((void)0)`，热路径**不产生任何代码**；`EventRing` 的查询接口
 * 仍可用，但明确返回 `enabled=false`、累计计数恒为 0。
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

/// select 未选中任何 case 时的 `arg2` 约定值（= uint64 最大值）。
inline constexpr std::uint64_t kNoSelectCase = ~std::uint64_t{0};

/**
 * @brief 并发原语原因码（park/wake 的语义标签；稳定，只能追加）。
 *
 * 写入 park 事件的 `arg3` 与 wake 事件的 `arg3`。与 `SuspendReason`
 * （调度器内部原因，写入 park 事件的 `arg2`）是两层信息：SuspendReason
 * 说明「调度器认为这是什么挂起」，本枚举说明「哪个原语触发的」。
 */
enum class PrimitiveReasonCode : std::uint16_t {
    kUnknown = 0,
    kYield = 1,
    kChannelSend = 2,
    kChannelRecv = 3,
    kSelect = 4,
    kMutex = 5,
    kParkingCondition = 6,
    kIo = 7,
    kTimer = 8,
    kFuture = 9,
    kScheduler = 10,
    kWait = 11,
    kShutdown = 12,
};

/// 原因码的稳定名字；未知值返回 "kUnknown"。
const char* PrimitiveReasonName(PrimitiveReasonCode reason) noexcept;

/**
 * @brief 调度事件码（稳定；新增只能追加，不能复用旧值）。
 *
 * 数值分组：1~9 park，10~19 wake，20~29 入队/重入队，30~39 runqueue
 * 出队/认领，40~49 io，50~59 线程/生命周期，60~69 Doctor，70~79 sync，
 * 80~89 channel，90~99 timer，100~109 G 生命周期，110~119 调度决策补充，
 * 120~129 并发原语，130~139 timer 回调投递。
 */
enum class SchedEventCode : std::uint16_t {
    kUnknown = 0,

    kParkEnter = 1,      ///< 进入 park：gid=G，arg2=SuspendReason 数值，arg3=原语原因
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

    // 90~99：timer（TimerService 调试事件）。只在 GO2CPP_DEBUG_DUMP=ON
    // 构建记录；固定字段映射见 EventRecord 的 arg3/arg4 注释。
    // 追加新值，不复用旧值。
    kTimerArm = 90,        ///< 注册：gid=timer_id，arg2=deadline_ns，arg3=kind，arg4=source_gid
    kTimerFire = 91,       ///< 触发：字段同上（deadline_ns 为计划时间）
    kTimerCancel = 92,     ///< 取消：字段同上
    kTimerReschedule = 93, ///< 重排：字段同上（deadline_ns 为新的截止时间）

    // 100~109：G（Task）生命周期。G 的编号与 Fiber 编号不同（Fiber 是执行栈，
    // 一个 G 在移交给另一个 M 时仍复用同一个 Fiber），因此这些事件一律用
    // Task::id()。arg2 的语义见各条注释。
    kGCreated = 100,   ///< 新 G 创建（spawn 返回前）：gid=G
    kGStarted = 101,   ///< 首次 resume 到 Fiber 栈：gid=G，arg2=M id
    kGResumed = 102,   ///< 非首次 resume：gid=G，arg2=M id（pid 字段为 P）
    kGCompleted = 103, ///< 正常结束：gid=G，arg2=GState::kDead
    kGFailed = 104,    ///< 异常结束：gid=G，arg2=GState::kFailed
    kGCancelled = 105, ///< 被取消结束：gid=G，arg2=GState::kCancelled

    // 110~119：调度决策补充（原有 20~39 已覆盖入队/出队/认领）。
    kStealOk = 110,      ///< 成功从其它 P 窃取：gid=G，arg2=victim P
    kStealFail = 111,    ///< 本轮窃取未取到：arg2=扫描的 P 数（采样）
    kWorkerPark = 112,   ///< M 进入空闲等待：gid=M，arg2=P（采样）
    kWorkerUnpark = 113, ///< M 被唤醒：gid=M，arg2=1 有工作/2 stopping/3 超时（采样）
    kSysmonPass = 114,   ///< sysmon 一轮检查：gid=0，arg2=累计 pass 数（采样）
    kTimerEnqueue = 115, ///< 定时器回调投递出的 G 已入队：gid=新 G，arg2=timer_id

    // 120~129：并发原语（快路径采样，慢路径全量）。
    kMutexFastLock = 120,      ///< Mutex 无竞争 CAS 成功：gid=G（采样）
    kChannelSendOk = 121,      ///< channel 发送立即完成：gid=G，arg2=capacity（采样）
    kChannelRecvOk = 122,      ///< channel 接收立即完成：gid=G，arg2=capacity（采样）
    kChannelSelectEnter = 123, ///< 进入 select：gid=G，arg2=case 数
    kChannelSelect = 124,      ///< select 决策：gid=G，arg2=选中下标（kNoSelectCase=无）
    kPCWait = 125,             ///< ParkingCondition 受管等待：gid=G（采样）
    kPCNotify = 126,           ///< notify_one 唤醒：gid=被唤醒 G（0=native）
    kPCNotifyAll = 127,        ///< notify_all：arg2=唤醒节点数

    // 130~139：timer 回调投递。kTimerFire 是服务端触发；本组记录「回调被交给
    // 执行器、进而变成可执行 G」的那一步。
    kTimerCallback = 130, ///< 回调交给执行器：gid=source_gid，arg2=timer_id
};

/// 事件码的稳定名字；未知值返回 "kUnknown"。
const char* SchedEventCodeName(SchedEventCode code) noexcept;

/**
 * @brief 该事件是否写入**关键区**（不会被普通事件覆盖）。
 * @details 关键集合 = wake 系列 + enqueue 拒绝 + claim 失败 + park 拒绝 +
 *          G 终态（完成/失败/取消）+ 诊断事件。
 *          普通高频事件（park 进入、runqueue push/pop、claim ok、io、
 *          G 创建/启动/恢复）走普通区。
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
    std::uint64_t arg3{0};  ///< timer kind / park-wake 原语原因码
    std::uint64_t arg4{0};  ///< timer 来源 gid / park 上下文
};

/**
 * @brief 原语提示作用域（RAII；只影响诊断字段）。
 *
 * 构造时若当前线程的对应槽位为空，则写入原因码；析构时恢复先前值。
 * 因此「外层优先」：Channel 内部调用 ParkingCondition 时，park 事件保留
 * `kChannelRecv` 而不是被改写成 `kParkingCondition`。
 *
 * @note 所有函数在 OFF 构建中是空实现（宏不展开调用，但类接口仍可用）。
 */
class PrimitiveHintScope final {
public:
    PrimitiveHintScope(bool wake_hint, PrimitiveReasonCode reason,
                       std::uint64_t context = 0) noexcept;
    ~PrimitiveHintScope();
    PrimitiveHintScope(const PrimitiveHintScope&) = delete;
    PrimitiveHintScope& operator=(const PrimitiveHintScope&) = delete;

private:
    bool m_wake_hint;
    bool m_active{false};
    PrimitiveReasonCode m_prior_reason{PrimitiveReasonCode::kUnknown};
    std::uint64_t m_prior_context{0};
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
     * @brief 采样记录：每 `every` 次调用实际记录一条。
     * @param every 采样周期；0 与 1 等价于每次都记录。
     * @note 用于 Mutex 快路径、worker park/unpark、sysmon 等极高频事件：
     *       它们必须保留可观测性，但不能用噪声填满环形缓冲。
     */
    static void RecordSampled(SchedEventCode code, std::uint64_t gid,
                              std::uint64_t arg2, std::uint32_t every) noexcept;

    /**
     * @brief 记录一条 timer 事件（字段语义见 EventRecord 注释）。
     * @param code 必须是 kTimerArm/kTimerFire/kTimerCancel/kTimerReschedule。
     * @param timer_id 定时器的调试标识（1 起；与句柄生命周期无关）。
     * @param deadline_ns 计划截止时间（steady_clock 纳秒，同机可比）。
     * @param kind 0=kOnce 1=kFixedDelay 2=kFixedRate 3=executor。
     * @param source_gid 来源逻辑 fiber id（0=未知）。
     * @note OFF 构建为空操作；不分配内存、不加锁。
     */
    static void RecordTimer(SchedEventCode code, std::uint64_t timer_id,
                            std::uint64_t deadline_ns, std::uint8_t kind,
                            std::uint32_t source_gid) noexcept;

    /**
     * @brief 发布当前线程绑定的 P（调度器在 worker 绑定 P 后调用）。
     * @details 记录事件时读取该值写入 `pid` 字段。OFF 构建为空操作。
     */
    static void BindProcessor(std::uint32_t pid) noexcept;
    /// 清除当前线程的 P 绑定（worker 退出时调用）；此后 pid 记为 kInvalidProcessorId。
    static void UnbindProcessor() noexcept;
    /// 当前线程已发布的 P；未绑定返回 kInvalidProcessorId。
    static std::uint32_t BoundProcessor() noexcept;

    /**
     * @brief 发布当前线程正在执行的 G（调度器在 M 执行 G 前调用）。
     * @details 供 channel/mutex 等模块在无法直接拿到 Scheduler 时给事件填 gid。
     *          0 表示未绑定。OFF 构建为空操作 / 返回 0。
     */
    static void BindTask(std::uint64_t gid) noexcept;
    /// 清除当前线程的 G 绑定（G 返回后调用）。
    static void UnbindTask() noexcept;
    /// 当前线程正在执行的 G；未绑定返回 0。
    static std::uint64_t BoundTask() noexcept;

    /// 某事件码的累计发布数（进程启动以来；OFF 构建恒 0）。
    static std::uint64_t TotalCount(SchedEventCode code) noexcept;
    /// 全部事件码的累计发布数（OFF 构建恒 0）。
    static std::uint64_t TotalCountAll() noexcept;
    /// 清零累计计数（测试与会话切换用；仅无并发写入时调用）。
    static void ResetTotals() noexcept;

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

#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP

#define GO2CPP_DEBUG_EVENT(code, gid, arg2)                              \
    ::go2cpp::debug::EventRing::Record(                                  \
        ::go2cpp::debug::SchedEventCode::code, static_cast<std::uint64_t>(gid), \
        static_cast<std::uint64_t>(arg2))
#define GO2CPP_DEBUG_EVENT_SAMPLED(code, gid, arg2, every)               \
    ::go2cpp::debug::EventRing::RecordSampled(                           \
        ::go2cpp::debug::SchedEventCode::code, static_cast<std::uint64_t>(gid), \
        static_cast<std::uint64_t>(arg2), static_cast<std::uint32_t>(every))
#define GO2CPP_DEBUG_BIND_PROCESSOR(pid) \
    ::go2cpp::debug::EventRing::BindProcessor(static_cast<std::uint32_t>(pid))
#define GO2CPP_DEBUG_UNBIND_PROCESSOR() \
    ::go2cpp::debug::EventRing::UnbindProcessor()
#define GO2CPP_DEBUG_BIND_TASK(gid) \
    ::go2cpp::debug::EventRing::BindTask(static_cast<std::uint64_t>(gid))
#define GO2CPP_DEBUG_UNBIND_TASK() \
    ::go2cpp::debug::EventRing::UnbindTask()
#define GO2CPP_DEBUG_BOUND_TASK() \
    ::go2cpp::debug::EventRing::BoundTask()
#define GO2CPP_DEBUG_TIMER_EVENT(code, timer_id, deadline_ns, kind, source_gid) \
    ::go2cpp::debug::EventRing::RecordTimer(                                 \
        (code), static_cast<std::uint64_t>(timer_id),                        \
        static_cast<std::uint64_t>(deadline_ns),                             \
        static_cast<std::uint8_t>(kind),                                     \
        static_cast<std::uint32_t>(source_gid))

#define GO2CPP_DEBUG_CONCAT_INNER(a, b) a##b
#define GO2CPP_DEBUG_CONCAT(a, b) GO2CPP_DEBUG_CONCAT_INNER(a, b)
/// 声明一个 park 原因作用域；调用处必须加分号。
#define GO2CPP_DEBUG_PARK_HINT(name)                                       \
    ::go2cpp::debug::PrimitiveHintScope GO2CPP_DEBUG_CONCAT(               \
        _go2cpp_park_hint_, __LINE__)(                                     \
        false, ::go2cpp::debug::PrimitiveReasonCode::name, 0)
#define GO2CPP_DEBUG_PARK_HINT_CTX(name, context)                           \
    ::go2cpp::debug::PrimitiveHintScope GO2CPP_DEBUG_CONCAT(               \
        _go2cpp_park_hint_, __LINE__)(                                     \
        false, ::go2cpp::debug::PrimitiveReasonCode::name,                 \
        static_cast<std::uint64_t>(context))
/// 声明一个 wake 原因作用域；调用处必须加分号。
#define GO2CPP_DEBUG_WAKE_HINT(name)                                        \
    ::go2cpp::debug::PrimitiveHintScope GO2CPP_DEBUG_CONCAT(               \
        _go2cpp_wake_hint_, __LINE__)(                                     \
        true, ::go2cpp::debug::PrimitiveReasonCode::name, 0)

#else  // GO2CPP_DEBUG_DUMP

#define GO2CPP_DEBUG_EVENT(code, gid, arg2) ((void)0)
#define GO2CPP_DEBUG_EVENT_SAMPLED(code, gid, arg2, every) ((void)0)
#define GO2CPP_DEBUG_TIMER_EVENT(code, timer_id, deadline_ns, kind, source_gid) \
    do {                                                                     \
        (void)(code);                                                        \
        (void)(timer_id);                                                    \
        (void)(deadline_ns);                                                 \
        (void)(kind);                                                        \
        (void)(source_gid);                                                  \
    } while (0)
#define GO2CPP_DEBUG_BIND_PROCESSOR(pid) ((void)0)
#define GO2CPP_DEBUG_UNBIND_PROCESSOR() ((void)0)
#define GO2CPP_DEBUG_BIND_TASK(gid) ((void)0)
#define GO2CPP_DEBUG_UNBIND_TASK() ((void)0)
#define GO2CPP_DEBUG_BOUND_TASK() (std::uint64_t{0})
#define GO2CPP_DEBUG_PARK_HINT(name) ((void)0)
#define GO2CPP_DEBUG_PARK_HINT_CTX(name, context) ((void)0)
#define GO2CPP_DEBUG_WAKE_HINT(name) ((void)0)

#endif  // GO2CPP_DEBUG_DUMP
