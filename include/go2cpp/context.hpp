#pragma once

#include "go2cpp/error.hpp"

#include <any>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp {

class Context;
using ContextPtr = std::shared_ptr<Context>;
using ContextTimePoint = std::chrono::steady_clock::time_point;
using ContextDuration = std::chrono::steady_clock::duration;
using CancelFunc = std::function<void()>;
using CancelCauseFunc = std::function<void(ErrorPtr)>;

namespace detail {
struct DoneSignalAccess;
}

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

    CallbackId AddCallbackImpl(std::function<void()> callback,
                               bool internal) const;
    void Signal() const noexcept;
    friend class Context;
    friend struct detail::DoneSignalAccess;
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
    DoneSignal::CallbackId AddBeforeDoneCallback(
        std::function<void()> callback);
    void RemoveBeforeDoneCallback(DoneSignal::CallbackId id) const noexcept;

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
    friend class ContextRollback;
};

// ContextRollback 把一个临时子 Context 和一组局部补偿动作绑定在一起。
// 它只回滚尚未提交的本地状态，不恢复父 Context，也不跳过 C++ 析构函数。
class ContextRollback final {
    struct State;

public:
    enum class Status {
        kActive,
        kRollingBack,
        kCommitted,
        kRolledBack,
        kFailed,
    };

    using UndoAction = std::function<void()>;

    class Savepoint final {
    public:
        Savepoint() = default;

        bool valid() const noexcept;

    private:
        std::weak_ptr<State> m_state;
        std::size_t m_depth{0};
        std::uint64_t m_generation{0};

        friend class ContextRollback;
    };

    // rollback_on_cancel 为 true 时，child Context 被父级取消或 deadline
    // 取消后，尚未提交的 undo 会在 child Done 已线性化后由触发取消的
    // 线程执行；普通 Done 观察回调会在内部 undo 之后运行。
    explicit ContextRollback(ContextPtr parent,
                             bool rollback_on_cancel = true);
    ~ContextRollback() noexcept;

    ContextRollback(const ContextRollback&) = delete;
    ContextRollback& operator=(const ContextRollback&) = delete;
    ContextRollback(ContextRollback&& other) noexcept;
    ContextRollback& operator=(ContextRollback&& other) noexcept;

    ContextPtr context() const noexcept { return m_context; }

    // 成功返回 true。动作应幂等、短小、非阻塞，最好声明为 noexcept。
    bool RecordUndo(UndoAction action);
    bool record_undo(UndoAction action) { return RecordUndo(std::move(action)); }

    Savepoint Mark() const noexcept;
    Savepoint savepoint() const noexcept { return Mark(); }

    // 只撤销 mark 之后登记的动作，事务通常仍保持 active；并发完整回滚
    // 或取消可能把请求升级为完整回滚。一次成功的局部回滚会使已有
    // Savepoint 全部失效，调用方应重新创建令牌。
    bool RollbackTo(const Savepoint& mark) noexcept;
    bool rollback_to(const Savepoint& mark) noexcept {
        return RollbackTo(mark);
    }

    // 显式回滚会取消 child Context，并按 LIFO 执行全部剩余动作。
    // 已经完成回滚时重复调用仍返回 true；已提交则返回 false。
    bool Rollback(ErrorPtr cause = {}) noexcept;
    bool rollback(ErrorPtr cause = {}) noexcept {
        return Rollback(std::move(cause));
    }

    // 提交只丢弃 undo，不取消 child Context；调用方可以继续把 context()
    // 交给下游。提交和回滚竞争时，以先完成状态转换者为准。
    bool Commit() noexcept;
    bool commit() noexcept { return Commit(); }

    Status status() const noexcept;
    bool active() const noexcept;
    bool committed() const noexcept;
    bool rolled_back() const noexcept;
    bool had_failure() const noexcept;
    std::exception_ptr failure() const noexcept;

    // 与 child Context 的 Done 分离：该信号表示 commit 或完整 rollback
    // 的补偿动作已经执行完毕。需要等待 undo 完成时使用它。undo 不得
    // 等待自身的 RollbackDone，也不应在回调中阻塞或再次取得外部锁。
    DoneSignal RollbackDone() const;
    DoneSignal rollback_done() const { return RollbackDone(); }

private:
    std::shared_ptr<State> m_state;
    ContextPtr m_context;
    mutable std::mutex m_callback_mutex;
    DoneSignal::CallbackId m_callback_id{0};
    DoneSignal::CallbackId m_before_callback_id{0};

    void DisarmCallback() noexcept;
};

using RollbackScope = ContextRollback;

ContextRollback WithRollback(const ContextPtr& parent,
                             bool rollback_on_cancel = true);

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
