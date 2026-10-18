/**
 * @file control_flow.hpp
 * @brief `defer` / `panic` / `recover` 三个显式控制流原语。
 *
 * ## 模块定位
 *
 * 为 Fiber 任务提供「不靠异常跳栈」的清理与错误传播手段：`defer` 负责
 * 作用域退出时的清理，`panic` 只发布状态、不打断 C++ 栈，`recover` 在
 * defer 回调内消费该状态。上游是业务任务与 `scheduler` 的 Fiber 入口，
 * 下游只依赖标准库。
 *
 * ## 本文件定义什么
 *
 * - `PanicInfo`：一次 panic 的快照（文本、错误码、任意负载）；
 * - `defer`（别名 `Defer`）：RAII 清理，支持取消与立即执行；
 * - `panic`（别名 `Panic`）：可随 Fiber 迁移的 panic 状态；
 * - `recover`（别名 `Recover`）：defer 回调内的恢复句柄；
 * - `LastDeferException` / `ClearDeferException`：defer 回调异常记录。
 *
 * ## 阅读建议
 *
 * 先读 `defer` 的「构造保存、析构执行一次」模型，再读 `panic`/`recover`
 * 的 Idle -> Active -> Recovered 状态机（见 `docs/state-machines/Panic.md`）。
 *
 * ## 边界与陷阱
 *
 * `panic::call()` 不会跳转 C++ 栈，必须配合 `defer` + `recover` 才能形成
 * Go 风格的恢复边界；`recover::take()` 在非 defer 回调期间恒失败。
 */
#pragma once

#include <any>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace go2cpp {

/**
 * @brief Fiber 控制流中的 panic 描述。
 * @details 记录 panic 是否产生、错误码、文本信息以及可选业务负载。
 *          该对象只保存状态，不负责跨线程传播。
 *
 * ## 定位
 * `panic` 状态与 `recover` 读取结果的值类型；`recover::take()`/`peek()` 返回它。
 *
 * ## 不变量
 * - `valid()` 等价于 `raised`；
 * - `raised == false` 时 `message`/`payload` 不参与语义（可能已被 clear 清空）。
 */
struct PanicInfo {
    bool raised{false};     ///< 是否发生过 panic；call()/call_info() 置真，clear()/take() 复位
    std::string message;    ///< panic 文本；call(string) 写，recover 读取
    int code{0};            ///< 业务错误码；call() 的可选参数，recover 读取
    std::any payload;       ///< 任意业务负载；call(T) 写入，take()/operator() 消费或清空

    /** @brief 返回当前描述是否包含有效的 panic。 */
    bool valid() const noexcept { return raised; }
};

namespace detail {
struct PanicState;

bool InDeferCallback() noexcept;
void RecordDeferException(std::exception_ptr exception) noexcept;
std::exception_ptr LastDeferException() noexcept;
void ClearDeferException() noexcept;
class DeferCallbackScope;
}  // namespace detail

// 作用域退出时执行一次回调。C++ 对象的析构顺序天然提供 LIFO 顺序；
// 参数通过构造函数传入，构造时就会按值保存。
/**
 * @brief 作用域退出时执行一次回调的 RAII defer。
 * @details 构造时保存回调和参数的值，析构时按 C++ 作用域逆序执行，
 *          覆盖正常返回、提前 return 和异常展开路径。
 *
 * ## 定位
 * 业务任务与 `panic`/`recover` 的清理原语：在 Fiber 任务的退出路径上释放
 * 资源、恢复 panic，或在测试里断言清理顺序。
 *
 * ## 生命周期与线程模型
 * 构造时捕获回调与参数（按值），析构或 `run_now()` 时执行一次；不可拷贝/
 * 不可移动，保证「一个作用域一个 defer」。回调异常不会逃出析构，而是记录到
 * 当前线程的 `LastDeferException()`。
 *
 * ## 不变量
 * - `m_active` 为真时 `m_callable` 非空；
 * - `run_now()` 返回后 `m_active` 必为假；
 * - 回调最多执行一次（`run_now()` 与析构互斥于该标志）。
 */
class defer final {
public:
    /**
     * @brief 注册无参数回调。
     * @param function 可调用对象；注册时复制或移动其状态。
     */
    template <typename F,
              typename = std::enable_if_t<std::is_invocable_v<
                  std::decay_t<F>&>>>
    explicit defer(F&& function)
        : m_callable(MakeCallable(std::forward<F>(function))) {}

    template <typename F, typename... Args,
              typename = std::enable_if_t<std::is_invocable_v<
                  std::decay_t<F>&, std::decay_t<Args>...>>>
    /**
     * @brief 注册带参数回调。
     * @param function 可调用对象。
     * @param args 回调参数；参数值在构造时保存。
     */
    defer(F&& function, Args&&... args)
        : m_callable(MakeCallableWithArgs(std::forward<F>(function),
                                          std::forward<Args>(args)...)) {}

    /** @brief 析构时执行尚未取消的回调。 */
    ~defer() noexcept;

    defer(const defer&) = delete;
    defer& operator=(const defer&) = delete;
    defer(defer&&) = delete;
    defer& operator=(defer&&) = delete;

    /** @brief 取消本次 defer，之后析构不再执行回调。 */
    void dismiss() noexcept { m_active = false; }
    /** @brief 立即执行回调，并将对象置为非活动状态。 */
    void run_now() noexcept;
    /** @brief 返回回调是否仍等待执行。 */
    bool active() const noexcept { return m_active; }

private:
    /**
     * @brief 类型擦除后的回调基类（defer 的内部多态载体）。
     *
     * ## 定位
     * `defer` 用 `unique_ptr<Callable>` 保存任意可调用对象；具体逻辑在
     * `CallableModel<F>`。不对外暴露。
     *
     * ## 不变量
     * `run()` 必须 noexcept 且自行吞掉异常（记录到 `LastDeferException`）。
     */
    struct Callable {
        virtual ~Callable() = default;
        virtual void run() noexcept = 0;
    };

    /**
     * @brief `Callable` 的模板实现，持有用户回调。
     *
     * ## 定位
     * `defer` 构造时把 lambda/函数对象包成它；析构/`run_now()` 通过基类调用。
     *
     * ## 不变量
     * - `m_function` 在构造后不再改变（回调可能被移动进来）；
     * - `run()` 最多执行一次（由 `defer::m_active` 保证），异常被捕获并记录。
     */
    template <typename F>
    struct CallableModel final : Callable {
        explicit CallableModel(F function) : m_function(std::move(function)) {}

        void run() noexcept override {
            try {
                std::invoke(m_function);
            } catch (...) {
                detail::RecordDeferException(std::current_exception());
            }
        }

        F m_function; ///< 用户回调；CallableModel 构造时保存，run() 调用
    };

    template <typename F>
    static std::unique_ptr<Callable> MakeCallable(F&& function) {
        using Stored = std::decay_t<F>;
        return std::make_unique<CallableModel<Stored>>(
            std::forward<F>(function));
    }

    template <typename F, typename... Args>
    static std::unique_ptr<Callable> MakeCallableWithArgs(F&& function,
                                                          Args&&... args) {
        using StoredFunction = std::decay_t<F>;
        using StoredArgs = std::tuple<std::decay_t<Args>...>;
        auto stored_function = StoredFunction(std::forward<F>(function));
        auto stored_args = StoredArgs(std::forward<Args>(args)...);
        return MakeCallable(
            [function = std::move(stored_function),
             args = std::move(stored_args)]() mutable {
                std::apply(
                    [&function](auto&&... values) {
                        std::invoke(function,
                                    std::forward<decltype(values)>(values)...);
                    },
                    std::move(args));
            });
    }

    std::unique_ptr<Callable> m_callable; ///< 类型擦除后的回调；构造时设置，run_now() 调用一次
    bool m_active{true};                  ///< 是否仍待执行；dismiss()/run_now() 置假
};

using Defer = defer;

// 一个显式的、可跨 Fiber 迁移的 panic 状态。call() 只发布状态，不跳转
// C++ 栈；调用者通过 defer 回调中的 recover 消费它。
/**
 * @brief 可随 Fiber 迁移的显式 panic 状态。
 * @details call() 只记录状态，不使用 longjmp 或破坏栈的跳转；用户应在
 *          defer 回调中通过 recover 读取和处理状态。
 *
 * ## 定位
 * `defer` + `recover` 模式的状态载体：业务代码 `call()` 发布 panic，
 * Fiber 复用/迁移后仍能通过共享状态读取；`recover` 负责消费。
 *
 * ## 生命周期与线程模型
 * 构造时分配 `detail::PanicState`；拷贝共享同一状态。内部用 mutex 保护
 * `info`/`active`/`recovered`，查询可从任意线程发起；但 `take()` 只允许在
 * 当前线程的 defer 回调内成功。
 *
 * ## 状态机
 * `Idle -(call)-> Active -(recover::take/operator())-> Recovered -(clear)-> Idle`，
 * 详见 `docs/state-machines/Panic.md`。
 *
 * ## 不变量
 * - `active == info.raised`；
 * - `recovered` 为真时 `active` 必为假；
 * - `m_state` 在构造后非空（`call_info()` 还会对拷贝后的空状态兜底创建）。
 */
class panic final {
public:
    /** @brief 创建一个未触发的 panic 状态。 */
    panic();
    ~panic();

    panic(const panic&) = default;
    panic& operator=(const panic&) = default;
    panic(panic&&) noexcept = default;
    panic& operator=(panic&&) noexcept = default;

    /**
     * @brief 以文本触发 panic。
     * @param message 错误文本。
     * @param code 可选业务错误码。
     */
    void call(std::string message, int code = 0);
    /** @brief 以 C 字符串触发 panic，参数含义同字符串重载。 */
    void call(const char* message, int code = 0);

    template <typename T>
    /** @brief 以任意可移动负载触发 panic。@param payload 业务负载。 */
    void call(T payload, int code = 0) {
        PanicInfo info;
        info.raised = true;
        info.code = code;
        info.payload = std::move(payload);
        call_info(std::move(info));
    }

    /** @brief 返回 panic 是否已经触发且尚未清除。 */
    bool active() const noexcept;
    /** @brief 返回 panic 是否已被 recover 标记为已处理。 */
    bool recovered() const noexcept;
    /** @brief 读取当前 panic 描述的快照。 */
    PanicInfo info() const;
    /** @brief 清除当前 panic，使其回到未触发状态。 */
    void clear() noexcept;

private:
    void call_info(PanicInfo info);
    std::shared_ptr<detail::PanicState> m_state; ///< 共享 panic 状态；构造时创建，call/clear/recover 通过它同步

    friend class recover;
};

using Panic = panic;

// recover 绑定一个 panic 状态。take() 只有在 defer 回调期间才会成功，
// 以保留最重要的恢复边界；它不会捕获普通 C++ 异常。
/**
 * @brief defer 回调内读取并确认 panic 的恢复句柄。
 * @details take() 只有在活动的 defer 回调期间才会返回状态；其他线程或
 *          Fiber 不能越过恢复边界捕获 panic。
 *
 * ## 定位
 * `panic` 的消费者：在 `defer` 回调里绑定并 `take()`，形成 Go 风格的
 * recover 边界。`active()`/`peek()` 提供只读观察。
 *
 * ## 生命周期与线程模型
 * 持有 `weak_ptr<PanicState>`，不延长 panic 的生命周期；`bind()`/`reset()`
 * 可重绑定。所有读取都在锁内完成；`take()`/`operator()` 额外检查当前线程
 * 是否处于 defer 回调。
 *
 * ## 不变量
 * - `bound()` 表示 `weak_ptr` 未过期；
 * - `take()` 成功后源 panic 变为 `recovered` 且 `info` 被清空；
 * - 非 defer 回调期间 `take()` 恒返回 `nullopt`。
 */
class recover final {
public:
    /** @brief 创建未绑定的恢复句柄。 */
    recover() = default;
    explicit recover(panic& source) noexcept { bind(source); }
    ~recover() = default;

    recover(const recover&) = default;
    recover& operator=(const recover&) = default;
    recover(recover&&) noexcept = default;
    recover& operator=(recover&&) noexcept = default;

    /** @brief 绑定指定 panic 状态，成功返回 true。 */
    bool bind(panic& source) noexcept;
    /** @brief 解除绑定，不再观察原 panic。 */
    void reset() noexcept { m_state.reset(); }
    /** @brief 返回句柄是否仍绑定有效状态。 */
    bool bound() const noexcept { return !m_state.expired(); }
    /** @brief 返回绑定的 panic 当前是否活动。 */
    bool active() const noexcept;

    /** @brief 在合法 defer 边界读取并标记恢复，失败返回空值。 */
    std::optional<PanicInfo> take() const;
    /** @brief 尝试恢复并返回是否成功。 */
    bool operator()() const noexcept;
    /** @brief 只读取当前状态，不改变 recovered 标记。 */
    PanicInfo peek() const;

private:
    std::weak_ptr<detail::PanicState> m_state; ///< 绑定但不拥有 panic 状态；bind() 设置，take/peek/active 加锁读取
};

using Recover = recover;

/** @brief 读取当前线程/Fiber 最近一次 defer 回调记录的异常。 */
std::exception_ptr LastDeferException() noexcept;
/** @brief 清除当前线程/Fiber 记录的 defer 异常。 */
void ClearDeferException() noexcept;

}  // namespace go2cpp
