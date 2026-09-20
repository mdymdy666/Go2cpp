#include "go2cpp/panic_defer.hpp"

#include <mutex>
#include <utility>

namespace go2cpp::panic_defer {
namespace {

struct ExecutionState {
    Frame* m_current_frame{nullptr};
    Frame* m_active_defer{nullptr};
    PanicValue m_panic_value;
    UnhandledPanicHandler m_handler;
    bool m_panicking{false};
    bool m_unwinding{false};
};

thread_local ExecutionState s_fallback_state;
thread_local ExecutionState* s_current_state = &s_fallback_state;
std::mutex s_handler_mutex;
UnhandledPanicHandler s_default_handler;

ExecutionState& current_state() noexcept { return *s_current_state; }

UnhandledPanicHandler copy_default_handler() {
    std::lock_guard<std::mutex> lock(s_handler_mutex);
    return s_default_handler;
}

}  // namespace

struct ExecutionContext::Impl {
    ExecutionState m_state;
};

ExecutionContext::ExecutionContext() : m_impl(std::make_unique<Impl>()) {}

ExecutionContext::~ExecutionContext() = default;

Binding::Binding(ExecutionContext& context) noexcept
    : m_previous(s_current_state) {
    s_current_state = &context.m_impl->m_state;
}

Binding::~Binding() noexcept {
    if (m_previous != nullptr) {
        s_current_state = static_cast<ExecutionState*>(m_previous);
    }
}

PanicValue PanicValue::nil() {
    PanicValue result;
    result.m_payload = std::make_shared<std::any>(std::nullptr_t{nullptr});
    result.m_nil_payload = true;
    return result;
}

PanicValue PanicValue::text(std::string message) {
    return from<std::string>(std::move(message));
}

Frame::Frame(Frame* parent) noexcept
    : m_parent(parent != nullptr ? parent : current_state().m_current_frame),
      m_previous_active_defer(current_state().m_active_defer) {
    current_state().m_current_frame = this;
}

Frame::~Frame() noexcept {
    ExecutionState& state = current_state();
    if (!m_finished && !m_unwound) {
        if (state.m_panicking) {
            unwind();
        } else {
            finish();
        }
    }
    if (state.m_current_frame == this) {
        state.m_current_frame = m_parent;
        state.m_active_defer = m_previous_active_defer;
    }
}

void Frame::defer_call_impl(std::function<void()> callback) {
    if ((!m_processing && (m_finished || m_unwound)) || !callback) {
        return;
    }
    m_defers.emplace_back(std::move(callback));
}

void Frame::run_deferred(bool panic_path) noexcept {
    ExecutionState& state = current_state();
    const bool previous_unwinding = state.m_unwinding;
    Frame* const previous_active_defer = state.m_active_defer;
    state.m_unwinding = panic_path;
    m_processing = true;
    while (!m_defers.empty()) {
        std::function<void()> callback = std::move(m_defers.back());
        m_defers.pop_back();
        state.m_active_defer = this;
        try {
            callback();
        } catch (...) {
            // User defer callbacks are not allowed to use C++ exceptions as
            // runtime control flow. Contain an accidental throw as a fresh
            // panic so later defers can still observe/recover it.
            panic(PanicValue::text("C++ exception in defer callback"));
        }
        state.m_active_defer = nullptr;
        // A defer may itself request a panic while finishing normally. The
        // remaining defers must then observe an active unwind and may recover.
        if (state.m_panicking) {
            state.m_unwinding = true;
        }
    }
    m_processing = false;
    state.m_active_defer = previous_active_defer;
    state.m_unwinding = previous_unwinding;
}

void Frame::finish() noexcept {
    if (m_finished || m_unwound) {
        return;
    }
    m_finished = true;
    run_deferred(false);
}

void Frame::unwind() noexcept {
    if (m_finished || m_unwound) {
        return;
    }
    m_unwound = true;
    run_deferred(true);
}

void panic(PanicValue value) {
    if (!value.valid()) {
        value = PanicValue::nil();
    }
    ExecutionState& state = current_state();
    state.m_panic_value = std::move(value);
    state.m_panicking = true;
    if (state.m_active_defer != nullptr) {
        state.m_unwinding = true;
    }
}

void panic_nil() { panic(PanicValue::nil()); }

bool panicking() noexcept { return current_state().m_panicking; }

Frame* Frame::Current() noexcept { return current_state().m_current_frame; }

PanicValue recover() noexcept {
    ExecutionState& state = current_state();
    if (!state.m_panicking || !state.m_unwinding ||
        state.m_active_defer == nullptr ||
        state.m_active_defer != state.m_current_frame) {
        return PanicValue{};
    }
    PanicValue result = state.m_panic_value;
    state.m_panic_value = PanicValue{};
    state.m_panicking = false;
    return result;
}

PanicValue current_panic() noexcept {
    ExecutionState& state = current_state();
    return state.m_panicking ? state.m_panic_value : PanicValue{};
}

void set_unhandled_panic_handler(UnhandledPanicHandler handler) {
    std::lock_guard<std::mutex> lock(s_handler_mutex);
    s_default_handler = std::move(handler);
}

bool run(std::function<void()> body) {
    GoroutineScope scope;
    Frame frame;
    try {
        if (body) {
            body();
        }
    } catch (...) {
        // C++ exceptions do not cross the runtime boundary. Translate an
        // accidental callback throw into an ordinary unhandled panic so the
        // frame still executes its registered defers.
        panic(PanicValue::text("C++ exception in goroutine body"));
    }
    ExecutionState& state = current_state();
    if (state.m_panicking) {
        frame.unwind();
    } else {
        frame.finish();
    }
    const bool completed = !state.m_panicking;
    if (state.m_panicking) {
        UnhandledPanicHandler handler = state.m_handler;
        if (!handler) {
            handler = copy_default_handler();
        }
        if (handler) {
            try {
                handler(state.m_panic_value);
            } catch (...) {
                // An observer cannot replace the terminal runtime result or
                // take down a worker with an accidental C++ exception.
            }
        }
        state.m_panic_value = PanicValue{};
        state.m_panicking = false;
    }
    return completed;
}

GoroutineScope::GoroutineScope()
    : m_previous_frame(current_state().m_current_frame),
      m_previous_panic(current_state().m_panic_value),
      m_previous_handler(current_state().m_handler),
      m_previous_active(current_state().m_panicking),
      m_previous_unwinding(current_state().m_unwinding),
      m_previous_defer(current_state().m_active_defer) {
    UnhandledPanicHandler handler = copy_default_handler();
    ExecutionState& state = current_state();
    state.m_current_frame = nullptr;
    state.m_active_defer = nullptr;
    state.m_panic_value = PanicValue{};
    state.m_panicking = false;
    state.m_unwinding = false;
    state.m_handler = std::move(handler);
}

GoroutineScope::~GoroutineScope() noexcept {
    ExecutionState& state = current_state();
    state.m_current_frame = m_previous_frame;
    state.m_active_defer = m_previous_defer;
    state.m_panic_value = m_previous_panic;
    state.m_panicking = m_previous_active;
    state.m_unwinding = m_previous_unwinding;
    state.m_handler = std::move(m_previous_handler);
}

}  // namespace go2cpp::panic_defer
