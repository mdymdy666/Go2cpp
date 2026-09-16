#pragma once

#include "go2cpp/error.hpp"

#include <any>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace go2cpp {

class Context;
using ContextPtr = std::shared_ptr<Context>;
using ContextTimePoint = std::chrono::steady_clock::time_point;
using ContextDuration = std::chrono::steady_clock::duration;
using CancelFunc = std::function<void()>;
using CancelCauseFunc = std::function<void(ErrorPtr)>;

/** A close-only, copyable notification corresponding to Context.Done(). */
class DoneSignal {
public:
    using CallbackId = std::uint64_t;

    DoneSignal();
    DoneSignal(const DoneSignal&) = default;
    DoneSignal& operator=(const DoneSignal&) = default;
    DoneSignal(DoneSignal&&) noexcept = default;
    DoneSignal& operator=(DoneSignal&&) noexcept = default;
    ~DoneSignal();

    void Wait() const;
    bool WaitFor(ContextDuration timeout) const;
    bool WaitUntil(ContextTimePoint deadline) const;
    bool IsDone() const noexcept;

    bool wait_for(ContextDuration timeout) const { return WaitFor(timeout); }
    bool wait_until(ContextTimePoint deadline) const { return WaitUntil(deadline); }
    bool is_done() const noexcept { return IsDone(); }

    // Callbacks are invoked at most once and always outside the signal lock.
    // Call RemoveCallback before releasing the last owner when a callback
    // captures that owner; the signal cannot infer callback ownership.
    CallbackId AddCallback(std::function<void()> callback) const;
    void RemoveCallback(CallbackId id) const;

private:
    struct State;
    std::shared_ptr<State> m_state;

    void Signal() const noexcept;
    friend class Context;
};

/** A typed identity key. Copies preserve identity; separate keys never collide. */
template <typename T>
class ContextKey {
public:
    ContextKey();
    explicit ContextKey(std::string name);

    const std::string& Name() const noexcept { return m_name; }
    const void* Identity() const noexcept { return m_token.get(); }
    // The context node retains this anchor while a typed value is present.
    // That prevents allocator address reuse from making a later key collide.
    std::shared_ptr<const std::uint64_t> Anchor() const noexcept {
        return m_token;
    }

private:
    std::shared_ptr<const std::uint64_t> m_token;
    std::string m_name;
};

class Context final {
public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::function<ContextTimePoint()>;
    struct State;

    static ContextPtr Background();
    static ContextPtr TODO();

    static std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr& parent);
    static std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(
        const ContextPtr& parent);
    static std::pair<ContextPtr, CancelFunc> WithDeadline(
        const ContextPtr& parent, ContextTimePoint deadline);
    static std::pair<ContextPtr, CancelFunc> WithTimeout(
        const ContextPtr& parent, ContextDuration timeout);

    template <typename T>
    static ContextPtr WithValue(const ContextPtr& parent,
                                const ContextKey<T>& key, T value) {
        return MakeValueContext(parent, key.Identity(), key.Name(),
                                key.Anchor(),
                                std::any(std::move(value)));
    }

    // String keys are provided for dynamically translated code. Typed keys
    // are preferred because their identity cannot collide accidentally.
    static ContextPtr WithValue(const ContextPtr& parent, std::string key,
                                std::any value);

    const DoneSignal& Done() const noexcept { return m_done; }
    DoneSignal& Done() noexcept { return m_done; }
    bool IsDone() const noexcept;
    ErrorPtr Err() const;
    ErrorPtr Cause() const;
    std::optional<ContextTimePoint> Deadline() const;
    bool HasDeadline() const;

    std::any Value(const std::string& key) const;

    template <typename T>
    std::optional<T> Value(const ContextKey<T>& key) const {
        // Typed lookup uses identity only; a string name is metadata and must
        // not let unrelated keys collide across a parent chain.
        const std::any value = LookupValue(key.Identity(), {});
        if (!value.has_value()) {
            return std::nullopt;
        }
        const auto* converted = std::any_cast<T>(&value);
        if (converted == nullptr) {
            return std::nullopt;
        }
        return *converted;
    }

    // Cancellation is idempotent. A null cause maps to CanceledError().
    void Cancel(ErrorPtr cause = {});
    void CancelDeadline();

    // Tests and embedders may inject a monotonic clock for timeout creation.
    // Existing timers retain their already calculated deadline.
    static void SetNowFunctionForTesting(NowFunction now);
    static void ResetNowFunctionForTesting();
    static ContextTimePoint Now();

private:
    explicit Context(std::shared_ptr<State> state);
    static ContextPtr MakeChild(const ContextPtr& parent);
    static ContextPtr MakeValueContext(const ContextPtr& parent,
                                       const void* token,
                                       const std::string& name,
                                       std::shared_ptr<const std::uint64_t>
                                           token_anchor,
                                       std::any value);
    std::any LookupValue(const void* token, const std::string& name) const;

    std::shared_ptr<State> m_state;
    DoneSignal m_done;

    friend ContextPtr Background();
    friend ContextPtr TODO();
    friend std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr&);
    friend std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(
        const ContextPtr&);
    friend std::pair<ContextPtr, CancelFunc> WithDeadline(
        const ContextPtr&, ContextTimePoint);
    friend std::pair<ContextPtr, CancelFunc> WithTimeout(const ContextPtr&,
                                                         ContextDuration);
};

ContextPtr Background();
ContextPtr TODO();
std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr& parent);
std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(
    const ContextPtr& parent);
std::pair<ContextPtr, CancelFunc> WithDeadline(const ContextPtr& parent,
                                               ContextTimePoint deadline);
std::pair<ContextPtr, CancelFunc> WithTimeout(const ContextPtr& parent,
                                             ContextDuration timeout);

template <typename T>
ContextPtr WithValue(const ContextPtr& parent, const ContextKey<T>& key, T value) {
    return Context::WithValue(parent, key, std::move(value));
}

ContextPtr WithValue(const ContextPtr& parent, std::string key, std::any value);

ErrorPtr CanceledError();
ErrorPtr DeadlineExceededError();
inline ErrorPtr ErrCanceled() { return CanceledError(); }
inline ErrorPtr ErrDeadlineExceeded() { return DeadlineExceededError(); }
inline ErrorPtr Canceled() { return CanceledError(); }
inline ErrorPtr DeadlineExceeded() { return DeadlineExceededError(); }

template <typename T>
ContextKey<T>::ContextKey()
    : m_token(std::make_shared<const std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(this))) {}

template <typename T>
ContextKey<T>::ContextKey(std::string name)
    : m_token(std::make_shared<const std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(this))),
      m_name(std::move(name)) {}

}  // namespace go2cpp
