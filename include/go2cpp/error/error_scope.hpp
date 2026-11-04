#pragma once

/**
 * @file error_scope.hpp
 * @brief `ErrorScope`：Fiber 链级别的多重错误收集器，以及 Fiber 退出时的自动抽干。
 *
 * @details 一个 Fiber（或普通线程）内可能**连续发生多个错误**，而调用方只关心
 *          最终是否失败、失败时要把哪些错误一起上报。为此本模块提供：
 *
 * - `ErrorScope::Current()`：当前执行上下文的收集器；基于 `ErrorLocal`，Fiber
 *   内共享同一份、普通线程退化为线程局部；
 * - `Add(error[, context])`：把错误追加进收集器（**不去重**，同一指针可重复收集）；
 * - `TakeAll()`：一次性取走全部条目并清空，调用方负责聚合上报；
 * - Fiber trampoline 钩子：Fiber 正常结束时若收集器非空，逐条以 WARN 级打印
 *   （错误文本 / 码 / 类型 / 上下文），随后清空并析构错误对象。
 *
 * ## 为什么挂在 Fiber 上而不是线程上
 *
 * G 会在 M 之间迁移。若用 `thread_local` 收集，一个 G 在 M1 上收集的错误，在
 * M2 上取不到；Fiber 退出时也可能在第三个 M 上。`ErrorLocal` + `error_fiber.hpp`
 * 的后端正好提供「跟着 G 走、G 结束回收」的语义，本模块直接复用，不重复造轮子。
 *
 * ## 自动抽干的开关
 *
 * - 编译期：定义 `GO2CPP_ERROR_SCOPE_NO_AUTO_DRAIN` 后默认关闭；
 * - 运行期：`SetErrorScopeAutoDrainEnabled(false)` 可随时关闭/恢复。
 *
 * 两者都关闭时，未 `TakeAll()` 的错误会在 Fiber 结束时静默析构（不打印）。
 *
 * ## 静态析构期的安全
 *
 * 抽干会调用 `go2cpp::logging`。日志单例按需构造，若在它析构之后再打日志就是
 * 未定义行为。`detail::LoggingUsable()` 在**首次使用前**先强制构造单例、再登记
 * 一个析构守卫；守卫在单例析构之前把可用标志置 false，之后抽干只析构错误、
 * 不再触碰日志单例。因此全局对象析构期触发的 Fiber 退出不会崩溃。
 *
 * ## 并发约束
 *
 * `ErrorScope` 必须在**所属 Fiber/线程内单线程访问**：同一 G 不会在两个 M 上
 * 同时执行，因此不需要锁。跨 Fiber 不共享（每个 G 一份），普通线程之间也不
 * 共享。错误对象本身不可变，可以安全地跨上下文传递 `ErrorPtr`。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "go2cpp/error/error.hpp"
#include "go2cpp/error/error_local.hpp"

#include "go2cpp/logging/log.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::error {

/// @brief 自动抽干的编译期默认值；定义 `GO2CPP_ERROR_SCOPE_NO_AUTO_DRAIN` 可关闭。
#if defined(GO2CPP_ERROR_SCOPE_NO_AUTO_DRAIN)
inline constexpr bool kErrorScopeAutoDrainByDefault = false;
#else
inline constexpr bool kErrorScopeAutoDrainByDefault = true;
#endif

class ErrorScope;

namespace detail {

/// @brief 当前执行上下文的 `ErrorScope` 槽；`Current()` 与 `TryCurrent()` 共用。
inline ErrorLocal<ErrorScope>& ScopeSlot() noexcept {
    static ErrorLocal<ErrorScope> slot;
    return slot;
}

/// @brief 进程级自动抽干开关；读写必须共用同一个实例。
inline std::atomic<bool>& AutoDrainFlag() noexcept {
    static std::atomic<bool> enabled{kErrorScopeAutoDrainByDefault};
    return enabled;
}

} // namespace detail

/**
 * @brief Fiber/线程级别的多重错误收集器。
 *
 * @details 值本身由 `ErrorLocal` 持有：Fiber 内所有代码看到同一个实例，Fiber
 *          迁移到别的 M 后实例跟随；普通线程上是线程局部。实例没有锁，访问
 *          约束见文件头。
 *
 * ## 典型用法
 * @code
 * auto& scope = go2cpp::error::ErrorScope::Current();
 * scope.Add(Wrap(MakeError<IOError>("read failed", path), "load config"));
 * scope.Add(MakeError<TimeoutError>(...), "connect");
 * if (!scope.empty()) {
 *     for (auto& entry : scope.TakeAll()) {
 *         Report(entry.context, entry.error);
 *     }
 * }
 * @endcode
 */
class ErrorScope final {
 public:
    /// @brief 一条被收集的错误及其来源上下文。
    struct Entry {
        ErrorPtr error;                              ///< 不可变错误对象；非空（`Add(nullptr)` 被忽略）
        std::string context;                         ///< 来源标签，例如 `"load config"`；未提供时为空
        std::chrono::system_clock::time_point at;    ///< 收集时刻（系统时钟，用于诊断/排序）
    };

    ErrorScope() = default;
    ~ErrorScope() = default;
    ErrorScope(const ErrorScope&) = delete;
    ErrorScope& operator=(const ErrorScope&) = delete;
    ErrorScope(ErrorScope&&) = delete;
    ErrorScope& operator=(ErrorScope&&) = delete;

    /**
     * @brief 收集一条错误，不附带上下文标签。
     * @param err [in] 错误指针；**所有权不移交**（shared_ptr 复制），空指针被忽略。
     * @note 不去重：同一 `ErrorPtr` 连续 Add 两次会产生两条记录。
     */
    void Add(ErrorPtr err) { Add(std::move(err), std::string_view{}); }

    /**
     * @brief 收集一条错误并记录来源上下文。
     * @param err [in] 错误指针；所有权不移交（shared_ptr 复制），空指针被忽略。
     * @param context [in] 来源标签的借用视图；**在调用期间有效即可**，内部复制。
     *        典型值是阶段名（`"dial"` / `"handshake"`），不要放长文本。
     * @note 不去重；上下文按原样复制，不做截断。
     */
    void Add(ErrorPtr err, std::string_view context) {
        if (!err) {
            return;
        }
        Entry entry;
        entry.error = std::move(err);
        entry.context.assign(context.data(), context.size());
        entry.at = std::chrono::system_clock::now();
        m_entries.push_back(std::move(entry));
    }

    /// @brief 当前收集条数。@return 条目数；`0` 表示没有待处理错误。
    std::size_t size() const noexcept { return m_entries.size(); }

    /// @brief 是否没有待处理错误。@return 空返回 true。
    bool empty() const noexcept { return m_entries.empty(); }

    /**
     * @brief 取出全部条目并清空收集器。
     * @return 按 Add 顺序排列的条目副本；调用方取得所有权，负责销毁。
     * @note `noexcept`：只做 vector 交换，不分配、不析构错误对象。
     *       返回后 `size() == 0`，再次 Add 从空开始。
     */
    std::vector<Entry> TakeAll() noexcept {
        std::vector<Entry> taken;
        taken.swap(m_entries);
        return taken;
    }

    /// @brief 丢弃全部条目（析构其中的错误对象），不打印。
    void Clear() noexcept { m_entries.clear(); }

    /**
     * @brief 返回当前执行上下文的收集器；不存在时创建。
     * @return 引用；生命周期到 Fiber 结束或线程退出，调用方**不要**保存到
     *         其他上下文（跨 Fiber 传递该引用是错误用法）。
     */
    static ErrorScope& Current() { return detail::ScopeSlot().GetOrCreate(); }

    /**
     * @brief 只读取当前上下文的收集器，不创建。
     * @return 已存在返回指针；从未 Add 过（或上下文已结束）返回 nullptr。
     * @note 指针在本次访问内有效；`Reset()`/Fiber 结束后失效。
     */
    static ErrorScope* TryCurrent() noexcept { return detail::ScopeSlot().TryGet(); }

 private:
    /// 收集顺序即 Add 顺序；不做去重、不排序。
    std::vector<Entry> m_entries;
};

/**
 * @brief 读取自动抽干开关。
 * @return 开启返回 true（默认，除非编译期宏关闭）。
 */
inline bool ErrorScopeAutoDrainEnabled() noexcept {
    return detail::AutoDrainFlag().load(std::memory_order_relaxed);
}

/**
 * @brief 运行期开关自动抽干。
 * @param enabled [in] true 恢复自动打印；false 关闭（Fiber 结束时静默析构）。
 * @note 进程级生效，影响之后结束的所有 Fiber；不改变已收集的条目。
 */
inline void SetErrorScopeAutoDrainEnabled(bool enabled) noexcept {
    detail::AutoDrainFlag().store(enabled, std::memory_order_relaxed);
}

namespace detail {

/// @brief 日志单例是否仍可安全调用（静态析构开始后为 false）。
inline std::atomic<bool>& LoggingUsableFlag() noexcept {
    static std::atomic<bool> usable{true};
    return usable;
}

/**
 * @brief 进程退出守卫：在日志单例析构**之前**关闭可用标志。
 * @note 构造顺序保证：先构造 `LoggerManager`，再构造本守卫；析构逆序执行，
 *       因此守卫先运行，此后抽干只做错误析构，不再访问日志单例。
 */
struct LoggingLifetimeGuard {
    ~LoggingLifetimeGuard() noexcept {
        LoggingUsableFlag().store(false, std::memory_order_relaxed);
    }
};

/**
 * @brief 判断日志单例是否可安全使用；首次调用同时登记退出守卫。
 * @return 可用返回 true；进程静态析构已开始返回 false。
 * @note 函数内静态初始化由 C++ 运行时串行化，多线程首次调用安全。
 */
inline bool LoggingUsable() noexcept {
    static const bool initialized = [] {
        // 步骤 1：先强制构造日志单例（若尚未构造），锁定构造顺序。
        (void)::go2cpp::log::LoggerManager::Instance();
        // 步骤 2：再构造守卫；退出时守卫先析构，标志先于单例失效。
        static LoggingLifetimeGuard guard;
        (void)guard;
        return true;
    }();
    (void)initialized;
    return LoggingUsableFlag().load(std::memory_order_relaxed);
}

/**
 * @brief 打印一条被抽干的错误。
 * @param entry [in] 条目；`entry.error` 必须非空（来自 `ErrorScope::Add`）。
 * @param index [in] 从 0 开始的序号，仅用于日志可读性。
 * @param total [in] 本次抽干总条数。
 * @note `noexcept`；日志路径任何异常都在内部吞掉，不影响 Fiber 收尾。
 */
inline void LogScopeEntry(const ErrorScope::Entry& entry, std::size_t index,
                          std::size_t total) noexcept {
    try {
        const std::string_view context =
            entry.context.empty() ? std::string_view{"<none>"} : std::string_view{entry.context};
        GO2CPP_LOG_WARN(GO2CPP_LOG_NAME("go2cpp.error"))
            << "ErrorScope: fiber 退出时发现未处理错误 (" << (index + 1) << '/' << total
            << ") context=" << context
            << " code=" << ErrorCodeName(entry.error->Code())
            << " type=" << entry.error->TypeName()
            << " message=" << entry.error->Message();
    } catch (...) {
        // step: 日志失败不能改变 Fiber 的收尾语义（条目稍后照常析构）。
    }
}

/**
 * @brief Fiber trampoline 的退出钩子：把当前 G 收集的错误逐条打印后清空。
 *
 * ## 定位
 * Fiber 正常结束（含用户代码抛异常）的唯一收尾点，位于 `fiber_local::detail::
 * Cleanup()` **之前**。错误收集器本身存放在 FiberLocal 里，Cleanup 会销毁它；
 * 一旦顺序颠倒，错误就再也打不出来。这是本模块唯一的自动提交点。
 *
 * ## 步骤
 * step1. 开关关闭时立即返回：零分配、不创建收集器。
 * step2. 只查已存在的收集器（`TryCurrent`），从未 Add 过的 Fiber 不分配任何东西。
 * step3. 原子摘出全部条目；先摘再打印，保证日志内部抛异常也不会重复打印。
 * step4. 日志单例已进入析构期时直接返回，条目在栈上析构（只释放错误对象）。
 * step5. 逐条打印错误文本、错误码、类型名与上下文。
 * step6. 离开作用域，错误对象随 `entries` 析构。
 *
 * ## 并发/失败语义
 * - 调用约束：只在 Fiber 自己的栈上、trampoline 收尾阶段调用；此时该 G 不会
 *   被并发 resume，收集器也不需要加锁。
 * - 原子性：`TakeAll()` 的交换是「取走即清空」，打印期间不会有新条目进入
 *   （用户代码已结束）。
 * - 失败语义：`noexcept`；日志异常被吞掉，错误对象仍被析构，不会泄漏；
 *   日志单例构造失败同样只析构错误。
 * - 不变量：返回后当前 G 的收集器为空；`LoggingUsable()` 为 false 时不触碰
 *   日志单例。
 */
inline void DrainErrorScopeOnFiberExit() noexcept {
    // step1: 关掉开关时完全不碰收集器，热路径零成本。
    if (!ErrorScopeAutoDrainEnabled()) {
        return;
    }
    // step2: 没有收集过错误就不创建实例（Current 会分配）。
    ErrorScope* const scope = ErrorScope::TryCurrent();
    if (scope == nullptr || scope->empty()) {
        return;
    }
    // step3: 先取走并清空，后续无论打印是否失败都不会重复上报。
    std::vector<ErrorScope::Entry> entries = scope->TakeAll();
    // step4: 静态析构期：只析构错误，绝不再碰日志单例。
    // 日志单例构造本身可能失败（例如 OOM），同样按「不可用」处理。
    bool logging_usable = false;
    try {
        logging_usable = LoggingUsable();
    } catch (...) {
        return;
    }
    if (!logging_usable) {
        return;
    }
    // step5: 每条错误一行 WARN，带上下文、错误码与类型名。
    const std::size_t total = entries.size();
    for (std::size_t index = 0; index < total; ++index) {
        LogScopeEntry(entries[index], index, total);
    }
    // step6: entries 在此析构，错误对象随之释放。
}

} // namespace detail

} // namespace go2cpp::error
