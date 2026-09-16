#include "go2cpp/panic_defer.hpp"

#include <mutex>
#include <utility>

namespace go2cpp::panic_defer {
namespace {

struct ExecutionState {
    Frame* current_frame{nullptr};
    Frame* active_defer{nullptr};
    PanicValue panic_value;
    UnhandledPanicHandler handler;
    bool panicking{false};
    bool unwinding{false};
};

thread_local ExecutionState t_state;
std::mutex s_handler_mutex;
UnhandledPanicHandler s_default_handler;

UnhandledPanicHandler copy_default_handler() {
    std::lock_guard<std::mutex> lock(s_handler_mutex);
    return s_default_handler;
}

}  // namespace

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
    : m_parent(parent != nullptr ? parent : t_state.current_frame),
      m_previous_active_defer(t_state.active_defer) {
    t_state.current_frame = this;
}

Frame::~Frame() {
    if (!m_finished && !m_unwound) {
        if (t_state.panicking) {
            unwind();
        } else {
            finish();
        }
    }
    if (t_state.current_frame == this) {
        t_state.current_frame = m_parent;
        t_state.active_defer = m_previous_active_defer;
    }
}

void Frame::defer_call_impl(std::function<void()> callback) {
    if ((!m_processing && (m_finished || m_unwound)) || !callback) {
        return;
    }
    m_defers.emplace_back(std::move(callback));
}

void Frame::run_deferred(bool panic_path) noexcept {
    const bool previous_unwinding = t_state.unwinding;
    Frame* const previous_active_defer = t_state.active_defer;
    t_state.unwinding = panic_path;
    m_processing = true;
    while (!m_defers.empty()) {
        std::function<void()> callback = std::move(m_defers.back());
        m_defers.pop_back();
        t_state.active_defer = this;
        try {
            callback();
        } catch (...) {
            // User defer callbacks are not allowed to use C++ exceptions as
            // runtime control flow. Contain an accidental throw as a fresh
            // panic so later defers can still observe/recover it.
            panic(PanicValue::text("C++ exception in defer callback"));
        }
        t_state.active_defer = nullptr;
        // A defer may itself request a panic while finishing normally. The
        // remaining defers must then observe an active unwind and may recover.
        if (t_state.panicking) {
            t_state.unwinding = true;
        }
    }
    m_processing = false;
    t_state.active_defer = previous_active_defer;
    t_state.unwinding = previous_unwinding;
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
    t_state.panic_value = std::move(value);
    t_state.panicking = true;
    if (t_state.active_defer != nullptr) {
        t_state.unwinding = true;
    }
}

void panic_nil() { panic(PanicValue::nil()); }

bool panicking() noexcept { return t_state.panicking; }

Frame* Frame::Current() noexcept { return t_state.current_frame; }

PanicValue recover() noexcept {
    if (!t_state.panicking || !t_state.unwinding ||
        t_state.active_defer == nullptr ||
        t_state.active_defer != t_state.current_frame) {
        return PanicValue{};
    }
    PanicValue result = t_state.panic_value;
    t_state.panic_value = PanicValue{};
    t_state.panicking = false;
    return result;
}

PanicValue current_panic() noexcept {
    return t_state.panicking ? t_state.panic_value : PanicValue{};
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
        // C++ exceptions do not cross the runtime boundary.  Translate an
        // accidental callback throw into an ordinary unhandled panic so the
        // frame still executes its registered defers.
        panic(PanicValue::text("C++ exception in goroutine body"));
    }
    if (t_state.panicking) {
        frame.unwind();
    } else {
        frame.finish();
    }
    const bool completed = !t_state.panicking;
    if (t_state.panicking) {
        UnhandledPanicHandler handler = t_state.handler;
        if (!handler) {
            handler = copy_default_handler();
        }
        if (handler) {
            try {
                handler(t_state.panic_value);
            } catch (...) {
                // An observer cannot replace the terminal runtime result or
                // take down a worker with an accidental C++ exception.
            }
        }
        t_state.panic_value = PanicValue{};
        t_state.panicking = false;
    }
    return completed;
}

GoroutineScope::GoroutineScope() noexcept
    : m_previous_frame(t_state.current_frame),
      m_previous_panic(t_state.panic_value),
      m_previous_handler(t_state.handler),
      m_previous_active(t_state.panicking),
      m_previous_unwinding(t_state.unwinding),
      m_previous_defer(t_state.active_defer) {
    t_state.current_frame = nullptr;
    t_state.active_defer = nullptr;
    t_state.panic_value = PanicValue{};
    t_state.panicking = false;
    t_state.unwinding = false;
    t_state.handler = copy_default_handler();
}

GoroutineScope::~GoroutineScope() noexcept {
    t_state.current_frame = m_previous_frame;
    t_state.active_defer = m_previous_defer;
    t_state.panic_value = m_previous_panic;
    t_state.panicking = m_previous_active;
    t_state.unwinding = m_previous_unwinding;
    t_state.handler = std::move(m_previous_handler);
}

}  // namespace go2cpp::panic_defer
