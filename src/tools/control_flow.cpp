#include "go2cpp/control_flow.hpp"

#include <mutex>

namespace go2cpp {
namespace detail {

struct PanicState {
    mutable std::mutex mutex;
    PanicInfo info;
    bool active{false};
    bool recovered{false};
};

thread_local unsigned int s_defer_depth = 0;
thread_local std::exception_ptr s_defer_exception;

class DeferCallbackScope final {
public:
    DeferCallbackScope() noexcept : m_previous(s_defer_depth) {
        ++s_defer_depth;
    }

    ~DeferCallbackScope() noexcept { s_defer_depth = m_previous; }

private:
    unsigned int m_previous;
};

bool InDeferCallback() noexcept { return s_defer_depth != 0; }

void RecordDeferException(std::exception_ptr exception) noexcept {
    s_defer_exception = std::move(exception);
}

std::exception_ptr LastDeferException() noexcept {
    return s_defer_exception;
}

void ClearDeferException() noexcept { s_defer_exception = {}; }

}  // namespace detail

defer::~defer() noexcept { run_now(); }

void defer::run_now() noexcept {
    if (!m_active) {
        return;
    }
    m_active = false;
    if (!m_callable) {
        return;
    }
    detail::DeferCallbackScope scope;
    m_callable->run();
}

panic::panic() : m_state(std::make_shared<detail::PanicState>()) {}

panic::~panic() = default;

void panic::call(std::string message, int code) {
    PanicInfo info;
    info.raised = true;
    info.message = std::move(message);
    info.code = code;
    call_info(std::move(info));
}

void panic::call(const char* message, int code) {
    call(message == nullptr ? std::string{} : std::string(message), code);
}

void panic::call_info(PanicInfo info) {
    if (!m_state) {
        m_state = std::make_shared<detail::PanicState>();
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->info = std::move(info);
    m_state->active = true;
    m_state->recovered = false;
}

bool panic::active() const noexcept {
    if (!m_state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->active;
}

bool panic::recovered() const noexcept {
    if (!m_state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->recovered;
}

PanicInfo panic::info() const {
    if (!m_state) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->info;
}

void panic::clear() noexcept {
    if (!m_state) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->info.payload.reset();
    m_state->info.message.clear();
    m_state->info.code = 0;
    m_state->info.raised = false;
    m_state->active = false;
    m_state->recovered = false;
}

bool recover::bind(panic& source) noexcept {
    m_state = source.m_state;
    return !m_state.expired();
}

bool recover::active() const noexcept {
    const auto state = m_state.lock();
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->active;
}

std::optional<PanicInfo> recover::take() const {
    if (!detail::InDeferCallback()) {
        return std::nullopt;
    }
    const auto state = m_state.lock();
    if (!state) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active) {
        return std::nullopt;
    }
    PanicInfo result = std::move(state->info);
    state->info = {};
    state->active = false;
    state->recovered = true;
    return result;
}

bool recover::operator()() const noexcept {
    if (!detail::InDeferCallback()) {
        return false;
    }
    const auto state = m_state.lock();
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active) {
        return false;
    }
    state->info.payload.reset();
    state->info.message.clear();
    state->info.code = 0;
    state->info.raised = false;
    state->active = false;
    state->recovered = true;
    return true;
}

PanicInfo recover::peek() const {
    const auto state = m_state.lock();
    if (!state) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->info;
}

std::exception_ptr LastDeferException() noexcept {
    return detail::LastDeferException();
}

void ClearDeferException() noexcept {
    detail::ClearDeferException();
}

}  // namespace go2cpp
