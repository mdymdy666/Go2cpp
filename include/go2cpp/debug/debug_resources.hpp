#pragma once

/**
 * @file debug_resources.hpp
 * @brief 资源级历史：资源注册表、线程栈样本、Fiber 调用链样本与资源事件。
 *
 * ## 定位
 *
 * `debug_event_ring.hpp` 的调度事件回答「谁 park / 谁被唤醒」，本文件补齐
 * **资源维度**：一把 `sync::Mutex`、一个 `ParkingCondition`、一个
 * `Channel<T>` 在什么时刻被谁加锁/等待/通知/收发，以及每次操作时的
 * **调用线程栈**与**Fiber 父链**。视图层据此输出
 * `--view history --resource <id>` 与聚合视图 `locks/channels/resources`。
 *
 * ## 三类数据
 *
 * 1. **资源注册表**（`RegisterResource` / `FindResourceByAddress`）：
 *    原语构造时注册、析构时注销；`ResourceId` 进程内单调、不复用（0 无效）。
 *    debug 关闭时注册接口仍可调用，但一律返回 0 且不分配内存。
 * 2. **线程栈样本表**（`CaptureStackSample`）：`backtrace()` 取 8~16 帧，
 *    按帧地址序列哈希去重 + 引用计数。**不在采集点做符号解析**；视图输出
 *    时才用 `dladdr`（可选 addr2line）解析，避免热路径付 `backtrace_symbols`
 *    的代价。
 * 3. **Fiber 链样本表**（`CaptureFiberChain`，实现见 `fiber.cpp`）：记录
 *    从 main_fiber 到当前 Fiber 的整条父链，每节点含 gid/状态/任务函数名/
 *    栈范围；同样去重 + 引用计数，事件只存 `chain_id`。
 *
 * ## 采样（debug 开启时）
 *
 * - 线程栈：默认 1/16（`GO2CPP_DEBUG_STACK_SAMPLE`，运行时环境变量可覆盖）；
 * - Fiber 链：默认 1/8（`GO2CPP_DEBUG_FIBER_CHAIN_SAMPLE`）；
 * - `critical=true`（park / 阻塞 / 唤醒 / 超时）时**全量**捕获，不采样。
 *
 * ## 零开销约束
 *
 * 所有插桩点必须通过本文件的宏调用：`GO2CPP_DEBUG_DUMP=OFF` 时宏展开为
 * `((void)0)` / `0`，不产生任何符号引用、不构造样本、不读环境变量。
 *
 * ## 边界（如实声明）
 *
 * - 栈样本是**采样**：未采样的事件 `stack_id=0`，视图不得伪造成“无栈”；
 * - 样本表有容量上限（默认 4096 条栈 / 2048 条链），满后丢弃新样本并计数；
 * - `dladdr` 只解析动态符号表；静态函数显示为 `module+offset`，demo 用
 *   `-rdynamic` 链接可得到可读符号；
 * - 资源对象析构后其 `ResourceId` 仍可查询（墓碑），用于回看历史；
 * - 注册表按地址查找依赖对象地址唯一；对象析构后地址可能被新对象复用，
 *   因此注销时按「地址 + 当前存活条目」匹配，不复用旧 id。
 */

#include "go2cpp/debug/debug_event_ring.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#ifndef GO2CPP_DEBUG_DUMP
#define GO2CPP_DEBUG_DUMP 0
#endif

namespace go2cpp::debug {

/// 资源类别（稳定；新增只能追加）。
enum class ResourceKind : std::uint8_t {
    kUnknown = 0,
    kMutex = 1,
    kRwMutex = 2,
    kRecursiveMutex = 3,
    kParkingCondition = 4,
    kChannel = 5,
    kWaitGroup = 6,
    kConditionVariable = 7,
    kTimedMutex = 8,
    kSpinMutex = 9,
    kFutexMutex = 10,
};

/// 资源类别名；未知值返回 "kUnknown"。
const char* ResourceKindName(ResourceKind kind) noexcept;
/// 从文本解析资源类别（大小写敏感；失败返回 kUnknown）。
ResourceKind ParseResourceKind(const std::string& text) noexcept;

/// 资源编号：进程内单调、不复用；0 表示「无资源」。
using ResourceId = std::uint64_t;
/// 无效资源编号。
inline constexpr ResourceId kInvalidResourceId = 0;

/// 资源注册表中的一个条目（快照语义：拷贝后与注册表解耦）。
struct ResourceInfo {
    ResourceId id{0};
    ResourceKind kind{ResourceKind::kUnknown};
    const void* address{nullptr};
    std::string name;             ///< 注册时提供的名字；空则自动生成
    std::uint64_t created_seq{0}; ///< 注册时的全局事件序号
    std::uint64_t created_ts_ns{0};
    std::uint64_t destroyed_seq{0}; ///< 注销时的全局序号；0=仍存活
    std::uint64_t destroyed_ts_ns{0};
    bool alive{false};
};

/**
 * @brief 注册一个资源，返回稳定编号。
 * @param kind 资源类别。
 * @param address 资源对象地址（用作查找键；不得为空）。
 * @param name 可读名字；为空时自动生成 `<kind>#<id>`。
 * @return 新编号；`GO2CPP_DEBUG_DUMP=OFF` 或参数非法时返回 0。
 * @note 线程安全；只应在原语构造/析构时调用（低频）。
 */
ResourceId RegisterResource(ResourceKind kind, const void* address,
                            const char* name) noexcept;

/// 按编号注销（幂等）。
void UnregisterResource(ResourceId id) noexcept;
/// 按地址注销当前存活条目（幂等；用于无法保存 id 的类型）。
void UnregisterResourceByAddress(const void* address) noexcept;

/// 按编号查询；找到时写入 *out（含墓碑）并返回 true。
bool LookupResource(ResourceId id, ResourceInfo* out);
/// 按地址查询**存活**条目；未找到返回 0。
ResourceId FindResourceByAddress(const void* address) noexcept;
/// 全部条目快照（按 id 升序）。
std::vector<ResourceInfo> SnapshotResources();
/// 当前存活条目数。
std::size_t ResourceAliveCount() noexcept;
/// 清空注册表（测试/会话切换用；仅无并发使用时调用）。
void ResetResourceRegistry() noexcept;

// ---------------------------------------------------------------------------
// 线程栈样本
// ---------------------------------------------------------------------------

/// 单条栈样本的最大帧数（固定 16，与 backtrace 的调用方缓冲一致）。
inline constexpr std::size_t kMaxStackFrames = 16;
/// 栈样本表容量（去重后）。
inline constexpr std::size_t kMaxStackSamples = 4096;

using StackSampleId = std::uint32_t;

/// 一帧的解析结果（视图输出用）。
struct StackFrameInfo {
    std::uintptr_t address{0};
    std::string symbol;       ///< 可读符号；解析失败为空
    std::string module;       ///< 所属模块（可执行文件/so 路径；未知为空）
    std::uintptr_t offset{0}; ///< 模块内偏移
    bool resolved{false};
};

/// 一条线程栈样本。
struct StackSample {
    StackSampleId id{0};
    std::uint32_t thread_id{0}; ///< std::thread::id 哈希的低 32 位
    std::uint64_t gid{0};       ///< 捕获时的受管 G；0=非受管线程
    std::uint64_t ts_ns{0};     ///< 首次捕获时间
    std::uint32_t frame_count{0};
    std::uint32_t refs{0}; ///< 引用计数（相同栈被复用时递增）
    std::array<std::uintptr_t, kMaxStackFrames> frames{};
};

/**
 * @brief 捕获当前线程的调用栈并登记为样本。
 * @param gid 当前受管 G（0=非受管线程）。
 * @param critical true 时**不采样**，总是捕获；false 时按 1/16 采样。
 * @return 样本编号；采样命中失败或表满返回 0。
 * @note 同线程重复捕获相同栈只增加引用计数，不新建样本。
 */
StackSampleId CaptureStackSample(std::uint64_t gid, bool critical) noexcept;

/// 查询样本（拷贝）；未找到返回 false。
bool LookupStackSample(StackSampleId id, StackSample* out);
/// 解析样本的各帧（dladdr；必要时 addr2line）。只读、可能较慢。
std::vector<StackFrameInfo> ResolveStackSample(StackSampleId id);
/// 已登记样本数。
std::size_t StackSampleCount() noexcept;
/// 因容量上限丢弃的样本数。
std::uint64_t StackSampleDropped() noexcept;
/// 清空样本表（测试用）。
void ResetStackSamples() noexcept;

// ---------------------------------------------------------------------------
// Fiber 链样本
// ---------------------------------------------------------------------------

/// 链样本的最大节点数（main_fiber + 8 层 Fiber）。
inline constexpr std::size_t kMaxFiberChainNodes = 9;
/// 链样本表容量。
inline constexpr std::size_t kMaxFiberChains = 2048;

using FiberChainId = std::uint32_t;

/// 链上一个节点的只读信息。
struct FiberChainNode {
    std::uint64_t fiber_id{0}; ///< Fiber 编号；0 = 线程 main_fiber
    std::uint64_t gid{0};      ///< 该 Fiber 最近绑定的受管 G；0=未知
    std::uint8_t state{0};     ///< FiberState 数值
    std::uint8_t suspend_reason{0};
    std::uint32_t depth{0};
    bool alive{false};
    bool main_fiber{false};
    std::string function; ///< 任务实际函数（typeid 名称或 GO2CPP_FIBER_NAME）
    std::uintptr_t stack_low{0};
    std::uintptr_t stack_high{0};
    bool stack_known{false};
};

/// 一条 Fiber 父链样本（从 main_fiber 到当前 Fiber，按深度升序）。
struct FiberChainSample {
    FiberChainId id{0};
    std::uint32_t thread_id{0};
    std::uint64_t gid{0}; ///< 捕获时当前受管 G
    std::uint64_t ts_ns{0};
    std::uint32_t refs{0};
    std::vector<FiberChainNode> nodes;
};

/**
 * @brief 捕获当前 Fiber 父链并登记为样本（实现位于 fiber.cpp）。
 * @param gid 当前受管 G；0=非受管。
 * @param critical true 时不采样，总是捕获。
 * @return 样本编号；采样未命中/无 Fiber/表满返回 0。
 */
FiberChainId CaptureFiberChain(std::uint64_t gid, bool critical) noexcept;

/// 查询链样本（拷贝）；未找到返回 false。
bool LookupFiberChain(FiberChainId id, FiberChainSample* out);
/// 已登记链样本数。
std::size_t FiberChainCount() noexcept;
/// 因容量上限丢弃的链样本数。
std::uint64_t FiberChainDropped() noexcept;
/// 清空链样本表（测试用）。
void ResetFiberChains() noexcept;

/// FiberState 稳定名字（供视图使用；未知返回 "kUnknown"）。
const char* FiberStateNameForDebug(std::uint8_t state) noexcept;

/**
 * @brief 设置「下一个创建的 Fiber」的可读名字（thread_local，消费一次）。
 *
 * 用法（仅 debug 构建有意义）：
 * ```cpp
 * GO2CPP_DEBUG_FIBER_NAME("worker-1");
 * auto fiber = Fiber::AcquireForScheduler(...);
 * ```
 * debug 关闭时宏空展开，字符串不参与编译。
 */
void SetNextFiberName(const char* name) noexcept;
/// 取出并清空「下一个 Fiber 名字」；未设置返回 nullptr。
const char* TakeNextFiberName() noexcept;

// ---------------------------------------------------------------------------
// 资源事件
// ---------------------------------------------------------------------------

/// 操作结果（事件字段；稳定，只能追加）。
enum class ResourceResult : std::uint8_t {
    kOk = 0,
    kBlocked = 1, ///< 本次操作进入阻塞/排队
    kWake = 2,    ///< 唤醒了一个等待者
    kTimeout = 3,
    kCancelled = 4,
    kWouldBlock = 5, ///< 非阻塞尝试失败
    kClosed = 6,
    kFailed = 7,
};

/// 结果名；未知返回 "kUnknown"。
const char* ResourceResultName(ResourceResult result) noexcept;

/// 资源事件标志位。
enum ResourceEventFlags : std::uint32_t {
    kResourceFlagValueKnown = 1U << 0,   ///< value 字段有效
    kResourceFlagTargetNative = 1U << 1, ///< target 是普通线程（无 gid）
    kResourceFlagStackSampled = 1U << 2, ///< stack_id 来自采样（非关键事件）
    kResourceFlagChainSampled = 1U << 3, ///< chain_id 来自采样
    kResourceFlagTry = 1U << 4,          ///< 非阻塞尝试（TryLock/TrySend...）
};

/**
 * @brief 一条资源事件（RecordResourceEvent 的输入）。
 *
 * 字段映射：
 * - `code`：复用/新增的 `SchedEventCode`（Mutex/RW/PC/Channel/Park）；
 * - `resource_id`：资源编号（park 无资源时为 0）；
 * - `actor_gid`：执行本次操作的 G；
 * - `target_gid`：被唤醒/被交接的 G（0=无）；
 * - `value`：channel 值的摘要（整型/指针直接取值，其它类型 0+未标记）；
 * - `result`：ok/blocked/wake/timeout/...；
 * - `waiters`：操作发生后的队列深度（近似）；
 * - `arg2`：事件码原有的附加量（capacity/等待者数等）；
 * - `flags`：ResourceEventFlags 位；
 * - `chain_id`：Fiber 链样本；0=未捕获。
 */
struct ResourceEventRecord {
    SchedEventCode code{SchedEventCode::kUnknown};
    ResourceId resource_id{0};
    std::uint64_t actor_gid{0};
    std::uint64_t target_gid{0};
    std::uint64_t value{0};
    std::uint64_t arg2{0};
    ResourceResult result{ResourceResult::kOk};
    std::uint32_t waiters{0};
    std::uint32_t flags{0};
    FiberChainId chain_id{0};
};

/**
 * @brief 记录一条资源事件（线程栈样本在此内部捕获）。
 * @note OFF 构建为空操作；调用方优先使用 `GO2CPP_DEBUG_RESOURCE_EVENT`。
 */
void RecordResourceEvent(const ResourceEventRecord& event) noexcept;

/// 计算 channel 值的摘要（整型/枚举/指针可直接比较；其它类型返回 0）。
template <typename T>
inline std::uint64_t ResourceValueDigest(const T& value) noexcept {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_integral_v<U> || std::is_enum_v<U>) {
        return static_cast<std::uint64_t>(value);
    } else if constexpr (std::is_pointer_v<U>) {
        return static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(value));
    } else {
        (void)value;
        return 0;
    }
}

/// 该类型是否支持值摘要（供调用方设置 kResourceFlagValueKnown）。
template <typename T>
inline constexpr bool ResourceValueDigestible =
    std::is_integral_v<std::remove_cv_t<std::remove_reference_t<T>>> ||
    std::is_enum_v<std::remove_cv_t<std::remove_reference_t<T>>> ||
    std::is_pointer_v<std::remove_cv_t<std::remove_reference_t<T>>>;

// ---------------------------------------------------------------------------
// 调试捕获采样比例（仅 ON 构建；环境变量可覆盖）
// ---------------------------------------------------------------------------

/// 线程栈采样周期（1/N；1=每次）。首次调用时读取环境变量。
std::uint32_t StackSampleEvery() noexcept;
/// Fiber 链采样周期（1/N）。
std::uint32_t FiberChainSampleEvery() noexcept;
/// 重设采样周期（测试用；0 表示恢复默认/环境变量）。
void SetDebugCaptureSampleEvery(std::uint32_t stack_every,
                                std::uint32_t chain_every) noexcept;

}  // namespace go2cpp::debug

#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP

/// 注册资源；`name` 可为 nullptr。
#define GO2CPP_DEBUG_RESOURCE_REGISTER(kind, address, name)                \
    ::go2cpp::debug::RegisterResource(::go2cpp::debug::ResourceKind::kind, \
                                      (address), (name))
/// 按编号注销。
#define GO2CPP_DEBUG_RESOURCE_UNREGISTER(id) \
    ::go2cpp::debug::UnregisterResource(id)
/// 按地址注销存活条目。
#define GO2CPP_DEBUG_RESOURCE_UNREGISTER_ADDR(address) \
    ::go2cpp::debug::UnregisterResourceByAddress(address)
/// 按地址查编号（0=未注册）。
#define GO2CPP_DEBUG_RESOURCE_ID(address) \
    ::go2cpp::debug::FindResourceByAddress(address)
/// 设置下一个 Fiber 的名字（thread_local，消费一次）。
#define GO2CPP_DEBUG_FIBER_NAME(name) ::go2cpp::debug::SetNextFiberName(name)

/**
 * @brief 记录一条资源事件。
 * @param code 事件码（不带 SchedEventCode:: 前缀，宏内补）。
 * @param resource 资源编号表达式。
 * @param actor 操作者 gid。
 * @param target 被唤醒者 gid（0=无）。
 * @param value channel 值摘要（其它原语传 0）。
 * @param result ResourceResult::xxx。
 * @param waiters 队列深度。
 * @param critical 关键事件（栈/链全量捕获）。
 */
#define GO2CPP_DEBUG_RESOURCE_EVENT(ev_code, ev_resource, ev_actor,          \
                                    ev_target, ev_value, ev_result,          \
                                    ev_waiters, ev_critical)                 \
    GO2CPP_DEBUG_RESOURCE_EVENT_EX(ev_code, ev_resource, ev_actor, ev_target,\
                                   ev_value, ev_result, ev_waiters,          \
                                   ev_critical, 0U)

/// 带 flags 的版本（value_known / target_native / try 等位）。
/// @note 形参一律加 ev_ 前缀：结构体成员名（code/value/result/...）与宏
///       形参同名时，预处理器会把 `_ev.code` 也替换掉（实测编译错误）。
#define GO2CPP_DEBUG_RESOURCE_EVENT_EX(                                      \
    ev_code, ev_resource, ev_actor, ev_target, ev_value, ev_result,          \
    ev_waiters, ev_critical, ev_flags)                                       \
    do {                                                                     \
        ::go2cpp::debug::ResourceEventRecord _go2cpp_res_ev;                 \
        _go2cpp_res_ev.code = ::go2cpp::debug::SchedEventCode::ev_code;      \
        _go2cpp_res_ev.resource_id =                                         \
            static_cast<std::uint64_t>(ev_resource);                         \
        _go2cpp_res_ev.actor_gid = static_cast<std::uint64_t>(ev_actor);     \
        _go2cpp_res_ev.target_gid = static_cast<std::uint64_t>(ev_target);   \
        _go2cpp_res_ev.value = static_cast<std::uint64_t>(ev_value);         \
        _go2cpp_res_ev.result = (ev_result);                                 \
        _go2cpp_res_ev.waiters = static_cast<std::uint32_t>(ev_waiters);     \
        _go2cpp_res_ev.flags = static_cast<std::uint32_t>(ev_flags);         \
        _go2cpp_res_ev.chain_id = ::go2cpp::debug::CaptureFiberChain(        \
            static_cast<std::uint64_t>(ev_actor),                            \
            static_cast<bool>(ev_critical));                                 \
        ::go2cpp::debug::RecordResourceEvent(_go2cpp_res_ev);                \
    } while (0)

#else  // GO2CPP_DEBUG_DUMP

#define GO2CPP_DEBUG_RESOURCE_REGISTER(kind, address, name) ((void)0)
#define GO2CPP_DEBUG_RESOURCE_UNREGISTER(id) ((void)0)
#define GO2CPP_DEBUG_RESOURCE_UNREGISTER_ADDR(address) ((void)0)
#define GO2CPP_DEBUG_RESOURCE_ID(address) (::go2cpp::debug::kInvalidResourceId)
#define GO2CPP_DEBUG_FIBER_NAME(name) ((void)0)
#define GO2CPP_DEBUG_RESOURCE_EVENT(ev_code, ev_resource, ev_actor,          \
                                    ev_target, ev_value, ev_result,          \
                                    ev_waiters, ev_critical)                 \
    ((void)0)
#define GO2CPP_DEBUG_RESOURCE_EVENT_EX(                                      \
    ev_code, ev_resource, ev_actor, ev_target, ev_value, ev_result,          \
    ev_waiters, ev_critical, ev_flags)                                       \
    ((void)0)

#endif  // GO2CPP_DEBUG_DUMP
