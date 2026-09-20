#pragma once

#include <any>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace go2cpp::panic_defer {

// A panic payload is deliberately type-erased.  panic(nil) is represented by
// a non-empty value with nil_payload() == true, so it cannot be confused with
// the absence of a panic.
class PanicValue {
public:
    PanicValue() = default;

    template <typename T>
    static PanicValue from(T value) {
        PanicValue result;
        result.m_payload = std::make_shared<std::any>(std::move(value));
        return result;
    }

    static PanicValue nil();
    static PanicValue text(std::string message);

    bool valid() const noexcept { return static_cast<bool>(m_payload); }
    bool nil_payload() const noexcept { return m_nil_payload; }
    const std::any* type_erased() const noexcept { return m_payload.get(); }

    template <typename T>
    const T* as() const noexcept {
        if (!m_payload) {
            return nullptr;
        }
        return std::any_cast<T>(m_payload.get());
    }

    const std::string* as_text() const noexcept { return as<std::string>(); }

private:
    std::shared_ptr<const std::any> m_payload;
    bool m_nil_payload{false};
};

class Frame;

// Owns the panic/defer state of one logical execution stream. Binding the
// context lets a stackful Fiber carry this state when it migrates between OS
// threads. Calls made without a Binding continue to use a thread-local
// fallback context.
class ExecutionContext {
public:
    ExecutionContext();
    ~ExecutionContext();
    ExecutionContext(const ExecutionContext&) = delete;
    ExecutionContext& operator=(const ExecutionContext&) = delete;
    ExecutionContext(ExecutionContext&&) = delete;
    ExecutionContext& operator=(ExecutionContext&&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend class Binding;
};

// Installs an ExecutionContext for the lifetime of this guard and restores
// the previous context on destruction. Bindings may be nested, but a single
// ExecutionContext must not be bound concurrently by multiple threads.
class Binding {
public:
    explicit Binding(ExecutionContext& context) noexcept;
    ~Binding() noexcept;
    Binding(const Binding&) = delete;
    Binding& operator=(const Binding&) = delete;
    Binding(Binding&&) = delete;
    Binding& operator=(Binding&&) = delete;

private:
    // Opaque pointer keeps the execution-state representation private while
    // making this guard allocation-free on every Fiber context switch.
    void* m_previous{nullptr};
};

// A goroutine-local execution boundary.  Constructing a boundary installs a
// frame on the current thread.  It is safe to call finish() or unwind() more
// than once; the first terminal operation owns the defer stack.
class Frame {
public:
    explicit Frame(Frame* parent = nullptr) noexcept;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&) = delete;
    Frame& operator=(Frame&&) = delete;
    ~Frame() noexcept;

    template <typename F>
    void defer_call(F&& callback) {
        defer_call_impl(std::function<void()>(std::forward<F>(callback)));
    }

    // Captures arguments now and invokes callback(args...) at unwind time.
    // This is the explicit equivalent of Go's registration-time argument
    // evaluation rule.
    template <typename F, typename... Args>
    void defer_call(F&& callback, Args&&... args) {
        auto captured = std::make_tuple(std::forward<Args>(args)...);
        defer_call_impl([fn = std::forward<F>(callback),
                         values = std::move(captured)]() mutable {
            std::apply(fn, std::move(values));
        });
    }

    void finish() noexcept;
    void unwind() noexcept;
    bool finished() const noexcept { return m_finished; }
    bool unwound() const noexcept { return m_unwound; }
    Frame* parent() const noexcept { return m_parent; }
    static Frame* Current() noexcept;

    template <typename F>
    void Defer(F&& callback) {
        defer_call(std::forward<F>(callback));
    }

    template <typename F, typename... Args>
    void Defer(F&& callback, Args&&... args) {
        defer_call(std::forward<F>(callback), std::forward<Args>(args)...);
    }

private:
    void defer_call_impl(std::function<void()> callback);
    void run_deferred(bool panic_path) noexcept;

    Frame* m_parent{nullptr};
    Frame* m_previous_active_defer{nullptr};
    std::vector<std::function<void()>> m_defers;
    bool m_finished{false};
    bool m_unwound{false};
    bool m_processing{false};
};

// Explicit runtime operations.  panic() only records state; a caller reaches
// the next Frame boundary by returning normally and letting Frame::unwind()
// run.  No C++ exception or longjmp is involved.
void panic(PanicValue value);
void panic_nil();
bool panicking() noexcept;
PanicValue recover() noexcept;
PanicValue current_panic() noexcept;

using UnhandledPanicHandler = std::function<void(const PanicValue&)>;
void set_unhandled_panic_handler(UnhandledPanicHandler handler);

// Runs one cooperative goroutine body.  The returned value is true when the
// body completed without an unhandled panic.  A recovered panic is considered
// normal completion, matching Go's control-flow boundary.
bool run(std::function<void()> body);

// Scheduler workers can use this guard to ensure panic state never leaks from
// one G task to another on a reused M thread.
class GoroutineScope {
public:
    GoroutineScope();
    GoroutineScope(const GoroutineScope&) = delete;
    GoroutineScope& operator=(const GoroutineScope&) = delete;
    ~GoroutineScope() noexcept;

private:
    Frame* m_previous_frame{nullptr};
    PanicValue m_previous_panic;
    UnhandledPanicHandler m_previous_handler;
    bool m_previous_active{false};
    bool m_previous_unwinding{false};
    Frame* m_previous_defer{nullptr};
};

}  // namespace go2cpp::panic_defer
