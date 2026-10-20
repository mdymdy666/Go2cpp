#pragma once

/**
 * @file debug_config.hpp
 * @brief 调试事件的**粒度配置**：按行为/组件单独开关，外加采样位移。
 *
 * ## 为什么需要
 *
 * `GO2CPP_DEBUG_DUMP=ON` 是「全开」：调度、park、原语、timer、IO 的事件全部
 * 记录。排查单个原语（例如只看 Mutex 争用）时，调度器事件占满了 256 条的
 * 普通环形区，把真正关心的事件挤掉。本配置把事件码划成 6 个类别，允许
 * 运行时只打开其中几类。
 *
 * ## 类别划分（`DebugCategoryOf(SchedEventCode)`）
 *
 * | 类别 | 覆盖事件码 | 含义 |
 * |---|---|---|
 * | `kLifecycle` | 50~53、100~105 | 调度器/线程启动停止、G 创建/启动/完成/失败/取消 |
 * | `kSched` | 20~23、30~33、110~114 | 入队/重入队、runqueue push/pop/claim、steal、worker park、sysmon |
 * | `kPark` | 1~3、10~14、136 | park/wake 全系列（含原因与 park 捕获） |
 * | `kPrimitives` | 70~79、80~82、120~129、137~138 | mutex/rwlock/CV/PC/channel/select/waitgroup |
 * | `kTimer` | 90~93、115、130 | timer arm/fire/cancel/reschedule/回调投递/入队 |
 * | `kIo` | 40~42 | IO 等待/唤醒 |
 *
 * 未归类的码（`kUnknown` 与 Doctor 诊断事件）**不受**类别开关控制：
 * `Enabled(kNone)` 恒为 true，避免把 Doctor 的受控干预记录也关掉。
 *
 * ## 环境变量
 *
 * ```text
 * GO2CPP_DEBUG_CATEGORIES=lifecycle,sched,park,mutex,channel,timer
 * ```
 *
 * - 逗号分隔；大小写不敏感；首尾空白忽略；空串 / `all` = 全开。
 * - `none` = 全关（连 stacks/chains 也关）。
 * - 除 `none` 外，**未列出的类别一律关闭**（白名单语义）。
 * - `mutex` / `channel` / `pc` / `rwmutex` / `cv` / `waitgroup` 是
 *   `primitives` 的别名（当前实现里它们打开同一个位）。
 * - `stacks` / `chains` 控制线程栈与 Fiber 链样本捕获。
 * - 非法 token：该 token 被忽略并返回 false（`LoadFromEnv` 会打印一次警告，
 *   并保持其它合法 token 的解析结果）。
 *
 * `GO2CPP_DEBUG_SAMPLE_SHIFT=N` 设置全局采样位移（0~16）。
 *
 * ## 零开销
 *
 * `GO2CPP_DEBUG_DUMP=OFF` 的构建里 `GO2CPP_DEBUG_EVENT` 等宏仍展开为
 * `((void)0)`，本文件的 `Enabled()` 不会被任何热路径引用；配置实现虽然
 * 参与编译（供测试与工具直接调用），但不进入调度/同步的调用点。
 */

#include <atomic>
#include <cstdint>

namespace go2cpp::debug {

/// 调度事件码（完整定义见 debug_event_ring.hpp；此处前置声明避免循环包含）。
enum class SchedEventCode : std::uint16_t;

/**
 * @brief 事件类别位（可组合；`kNone` 表示「不受开关控制」）。
 *
 * `kStacks` / `kChains` 不是事件码类别，而是**样本捕获**开关：
 * 关闭时 `CaptureStackSample()` / `CaptureFiberChain()` 立即返回 0，
 * 不做 backtrace、不加锁、不建样本。
 */
enum class DebugCategory : std::uint32_t {
    kNone = 0U,
    kLifecycle = 1U << 0U,
    kSched = 1U << 1U,
    kPark = 1U << 2U,
    kPrimitives = 1U << 3U,
    kTimer = 1U << 4U,
    kIo = 1U << 5U,
    kStacks = 1U << 6U,
    kChains = 1U << 7U,
    /// 全部类别（含样本捕获）。
    kAll = (1U << 8U) - 1U,
};

/// 类别名（"lifecycle"/"sched"/...；kNone 返回 "none"，kAll 返回 "all"）。
const char* DebugCategoryName(DebugCategory category) noexcept;

/**
 * @brief 事件码 → 类别。
 * @return `kNone` 表示该码不受类别开关控制（未知码、Doctor 诊断事件）。
 * @note 定义在 `debug_event_ring.hpp`（需要完整的 `SchedEventCode` 枚举）。
 */
DebugCategory DebugCategoryOf(SchedEventCode code) noexcept;

/**
 * @brief 粒度配置（值语义；`Set/Get` 通过原子掩码交换）。
 *
 * 字段与任务约定的六个行为类别一一对应；`stacks`/`chains` 控制样本捕获；
 * `sample_shift` 是**额外**降采样位移：0 保持现有采样周期不变，
 * `n>0` 把每个采样周期再乘以 `2^n`（关键事件仍全量）。
 */
struct DebugConfig {
    bool lifecycle{true};  ///< G 生命周期
    bool sched{true};      ///< enqueue/requeue/steal/claim/worker park/sysmon
    bool park{true};       ///< park/wake（含原因）
    bool primitives{true}; ///< mutex/channel/pc/rwlock/cv/waitgroup
    bool timer{true};      ///< timer arm/fire/cancel/reschedule/callback
    bool io{true};         ///< io wait/wake
    bool stacks{true};     ///< 线程栈样本
    bool chains{true};     ///< Fiber 链样本
    std::uint32_t sample_shift{0}; ///< 采样位移（0=不额外降采样，3=1/8）

    /**
     * @brief 该类别当前是否启用（原子读；热路径入口）。
     * @note `kNone` 恒为 true：未归类的事件不受开关控制。
     *       首次调用会惰性读取环境变量（只发生一次）。
     */
    static bool Enabled(DebugCategory category) noexcept;

    /// 当前配置快照。
    static DebugConfig Get() noexcept;
    /// 覆盖当前配置（线程安全；会抑制环境变量的后续惰性解析）。
    static void Set(const DebugConfig& config) noexcept;
    /// 恢复「全开、不额外降采样」，并抑制环境变量解析。
    static void Reset() noexcept;

    /// 当前全局采样位移。
    static std::uint32_t SampleShift() noexcept;

    /**
     * @brief 显式解析类别列表（不读环境变量）。
     * @param text 逗号分隔的类别名/别名；nullptr 或空串表示全开。
     * @param out 解析结果（未列出的类别关闭）。
     * @return 全部 token 都识别返回 true；出现非法 token 返回 false
     *         （合法 token 仍然生效）。
     */
    static bool ParseCategories(const char* text, DebugConfig* out) noexcept;

    /**
     * @brief 从 `GO2CPP_DEBUG_CATEGORIES` / `GO2CPP_DEBUG_SAMPLE_SHIFT` 加载。
     * @return 环境变量缺失或全部合法返回 true；非法值返回 false 并打印警告。
     * @note 幂等：第一次调用后结果被缓存，后续调用直接返回 true。
     */
    static bool LoadFromEnv() noexcept;
};

namespace detail {

/// 类别掩码（原子；由 DebugConfig::Set/LoadFromEnv 写入）。
extern std::atomic<std::uint32_t> g_debug_category_mask;
/// 全局采样位移（原子）。
extern std::atomic<std::uint32_t> g_debug_sample_shift;
/// 环境变量是否已解析（acquire/release 配对）。
extern std::atomic<bool> g_debug_env_ready;

/// 惰性解析环境变量（幂等；首次调用可能阻塞在内部互斥锁上）。
void EnsureDebugConfigEnv() noexcept;

}  // namespace detail

inline bool DebugConfig::Enabled(DebugCategory category) noexcept {
    // kNone 表示「不受开关控制」：Doctor 诊断事件与未知码永远记录。
    if (category == DebugCategory::kNone) {
        return true;
    }
    if (!detail::g_debug_env_ready.load(std::memory_order_acquire)) {
        detail::EnsureDebugConfigEnv();
    }
    const std::uint32_t mask =
        detail::g_debug_category_mask.load(std::memory_order_relaxed);
    return (mask & static_cast<std::uint32_t>(category)) != 0U;
}

inline std::uint32_t DebugConfig::SampleShift() noexcept {
    return detail::g_debug_sample_shift.load(std::memory_order_relaxed);
}

}  // namespace go2cpp::debug
