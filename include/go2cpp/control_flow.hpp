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

struct PanicInfo {
    bool raised{false};
    std::string message;
    int code{0};
    std::any payload;

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
class defer final {
public:
    template <typename F,
              typename = std::enable_if_t<std::is_invocable_v<
                  std::decay_t<F>&>>>
    explicit defer(F&& function)
        : m_callable(MakeCallable(std::forward<F>(function))) {}

    template <typename F, typename... Args,
              typename = std::enable_if_t<std::is_invocable_v<
                  std::decay_t<F>&, std::decay_t<Args>...>>>
    defer(F&& function, Args&&... args)
        : m_callable(MakeCallableWithArgs(std::forward<F>(function),
                                          std::forward<Args>(args)...)) {}

    ~defer() noexcept;

    defer(const defer&) = delete;
    defer& operator=(const defer&) = delete;
    defer(defer&&) = delete;
    defer& operator=(defer&&) = delete;

    void dismiss() noexcept { m_active = false; }
    void run_now() noexcept;
    bool active() const noexcept { return m_active; }

private:
    struct Callable {
        virtual ~Callable() = default;
        virtual void run() noexcept = 0;
    };

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

        F m_function;
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

    std::unique_ptr<Callable> m_callable;
    bool m_active{true};
};

using Defer = defer;

// 一个显式的、可跨 Fiber 迁移的 panic 状态。call() 只发布状态，不跳转
// C++ 栈；调用者通过 defer 回调中的 recover 消费它。
class panic final {
public:
    panic();
    ~panic();

    panic(const panic&) = default;
    panic& operator=(const panic&) = default;
    panic(panic&&) noexcept = default;
    panic& operator=(panic&&) noexcept = default;

    void call(std::string message, int code = 0);
    void call(const char* message, int code = 0);

    template <typename T>
    void call(T payload, int code = 0) {
        PanicInfo info;
        info.raised = true;
        info.code = code;
        info.payload = std::move(payload);
        call_info(std::move(info));
    }

    bool active() const noexcept;
    bool recovered() const noexcept;
    PanicInfo info() const;
    void clear() noexcept;

private:
    void call_info(PanicInfo info);
    std::shared_ptr<detail::PanicState> m_state;

    friend class recover;
};

using Panic = panic;

// recover 绑定一个 panic 状态。take() 只有在 defer 回调期间才会成功，
// 以保留最重要的恢复边界；它不会捕获普通 C++ 异常。
class recover final {
public:
    recover() = default;
    explicit recover(panic& source) noexcept { bind(source); }
    ~recover() = default;

    recover(const recover&) = default;
    recover& operator=(const recover&) = default;
    recover(recover&&) noexcept = default;
    recover& operator=(recover&&) noexcept = default;

    bool bind(panic& source) noexcept;
    void reset() noexcept { m_state.reset(); }
    bool bound() const noexcept { return !m_state.expired(); }
    bool active() const noexcept;

    std::optional<PanicInfo> take() const;
    bool operator()() const noexcept;
    PanicInfo peek() const;

private:
    std::weak_ptr<detail::PanicState> m_state;
};

using Recover = recover;

std::exception_ptr LastDeferException() noexcept;
void ClearDeferException() noexcept;

}  // namespace go2cpp
