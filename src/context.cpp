#include "go2cpp/context.hpp"
#include "go2cpp/core/parking_condition.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <map>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp {

struct DoneSignal::State {
    mutable std::mutex mutex;
    mutable core::ParkingCondition cv;
    bool done{false};
    CallbackId next_id{1};
    // 内部回调（例如 ContextRollback）必须先于用户观察回调执行。
    // 两组回调仍然都在 Signal() 的锁外调用，避免回调重入信号对象。
    std::unordered_map<CallbackId, std::function<void()>> internal_callbacks;
    std::unordered_map<CallbackId, std::function<void()>> callbacks;
};

DoneSignal::DoneSignal() : m_state(std::make_shared<State>()) {}
DoneSignal::~DoneSignal() = default;

void DoneSignal::Wait() const {
    const auto state = m_state;
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [state] { return state->done; });
}

bool DoneSignal::WaitFor(ContextDuration timeout) const {
    const auto state = m_state;
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->cv.wait_for(lock, timeout, [state] { return state->done; });
}

bool DoneSignal::WaitUntil(ContextTimePoint deadline) const {
    const auto state = m_state;
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->cv.wait_until(lock, deadline, [state] { return state->done; });
}

bool DoneSignal::IsDone() const noexcept {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->done;
}

DoneSignal::CallbackId DoneSignal::AddCallback(
    std::function<void()> callback) const {
    return AddCallbackImpl(std::move(callback), false);
}

DoneSignal::CallbackId DoneSignal::AddCallbackImpl(
    std::function<void()> callback, bool internal) const {
    if (!callback) {
        return 0;
    }
    CallbackId id = 0;
    bool invoke_now = false;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->done) {
            invoke_now = true;
        } else {
            id = m_state->next_id++;
            if (id == 0) {
                id = m_state->next_id++;
            }
            auto& callbacks = internal ? m_state->internal_callbacks
                                       : m_state->callbacks;
            callbacks.emplace(id, std::move(callback));
        }
    }
    if (invoke_now) {
        // A notification callback is advisory. Cancellation itself has
        // already been linearized, so a faulty observer cannot undo it.
        try {
            callback();
        } catch (...) {
        }
    }
    return id;
}

void DoneSignal::RemoveCallback(CallbackId id) const {
    if (id == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->callbacks.erase(id);
    m_state->internal_callbacks.erase(id);
}

void DoneSignal::Signal() const noexcept {
    std::vector<std::function<void()>> internal_callbacks;
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->done) {
            return;
        }
        m_state->done = true;
        internal_callbacks.reserve(m_state->internal_callbacks.size());
        for (auto& entry : m_state->internal_callbacks) {
            internal_callbacks.push_back(std::move(entry.second));
        }
        m_state->internal_callbacks.clear();
        callbacks.reserve(m_state->callbacks.size());
        for (auto& entry : m_state->callbacks) {
            callbacks.push_back(std::move(entry.second));
        }
        m_state->callbacks.clear();
    }
    m_state->cv.notify_all();
    for (auto& callback : internal_callbacks) {
        try {
            callback();
        } catch (...) {
        }
    }
    for (auto& callback : callbacks) {
        try {
            callback();
        } catch (...) {
        }
    }
}

namespace detail {

struct DoneSignalAccess {
    static void Signal(DoneSignal& signal) noexcept { signal.Signal(); }

    static DoneSignal::CallbackId AddInternalCallback(
        const DoneSignal& signal, std::function<void()> callback) {
        return signal.AddCallbackImpl(std::move(callback), true);
    }
};

}  // namespace detail

namespace {

std::mutex s_clock_mutex;
Context::NowFunction s_now_function;

ContextTimePoint SaturatingDeadline(ContextTimePoint now,
                                    ContextDuration timeout) noexcept {
    // Keep the addition in the time-point domain. In particular, a
    // ContextDuration::max() timeout must not wrap into the past and become
    // an accidental immediate cancellation.
    using Rep = ContextDuration::rep;
    if (timeout > ContextDuration::zero() &&
        now > ContextTimePoint::max() - timeout) {
        return ContextTimePoint::max();
    }
    if (timeout < ContextDuration::zero()) {
        // Negating duration::min() would overflow. It is already farther
        // below any representable time point, so handle it explicitly before
        // using the otherwise safe subtraction below.
        if constexpr (std::numeric_limits<Rep>::is_signed) {
            if (timeout.count() == std::numeric_limits<Rep>::min() ||
                now < ContextTimePoint::min() - timeout) {
                return ContextTimePoint::min();
            }
        }
    }
    return now + timeout;
}

class TimerService {
public:
    using Id = std::uint64_t;

    static TimerService& Instance() {
        // Context states can be destroyed during arbitrary static teardown.
        // Keep the service and its synchronization primitives alive until the
        // process exits, so State::~State() can always remove a pending timer
        // without touching a destroyed singleton. The exit hook stops the
        // worker first, avoiding a live-thread Memcheck report.
        static TimerService* service = [] {
            auto* value = new TimerService();
            std::atexit(&TimerService::ShutdownAtExit);
            return value;
        }();
        return *service;
    }

    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    Id Add(const std::shared_ptr<Context::State>& state,
           ContextTimePoint deadline);
    void Remove(Id id);

private:
    struct Entry {
        ContextTimePoint deadline;
        Id id;
        std::weak_ptr<Context::State> state;
    };

    TimerService() : m_thread([this] { Run(); }) {}

    ~TimerService() { Shutdown(); }

    static void ShutdownAtExit() noexcept { Instance().Shutdown(); }

    void Shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            m_entries.clear();
        }
        m_cv.notify_all();
        if (m_thread.joinable() &&
            m_thread.get_id() != std::this_thread::get_id()) {
            m_thread.join();
        }
    }

    void Run();

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::multimap<ContextTimePoint, Entry> m_entries;
    std::atomic<Id> m_next_id{1};
    bool m_stopping{false};
    std::thread m_thread;
};

}  // namespace

struct Context::State : std::enable_shared_from_this<Context::State> {
    mutable std::mutex mutex;
    std::shared_ptr<State> parent;
    std::vector<std::weak_ptr<State>> children;
    std::unordered_map<const void*, std::any> typed_values;
    // Keep the allocation that identifies each typed key alive for as long as
    // its value is reachable.  Looking up by a raw pointer without this owner
    // would allow an allocator reuse to alias a later, unrelated key.
    std::unordered_map<const void*, std::shared_ptr<const std::uint64_t>>
        typed_key_anchors;
    std::unordered_map<std::string, std::any> named_values;
    DoneSignal done;
    ErrorPtr error;
    ErrorPtr cause;
    std::optional<ContextTimePoint> deadline;
    TimerService::Id timer_id{0};
    bool root{false};
    DoneSignal::CallbackId next_before_done_id{1};
    std::unordered_map<DoneSignal::CallbackId, std::function<void()>>
        before_done_callbacks;

    ~State();

    struct CancellationWork {
        std::shared_ptr<State> state;
        ErrorPtr error;
        ErrorPtr cause;
        bool deadline_error{false};
        bool remove_from_parent{false};
    };

    DoneSignal::CallbackId AddBeforeDoneCallback(
        std::function<void()> callback) {
        if (!callback) {
            return 0;
        }
        DoneSignal::CallbackId id = 0;
        bool invoke_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (error) {
                invoke_now = true;
            } else {
                id = next_before_done_id++;
                if (id == 0) {
                    id = next_before_done_id++;
                }
                before_done_callbacks.emplace(id, std::move(callback));
            }
        }
        if (invoke_now) {
            try {
                callback();
            } catch (...) {
                // 取消前置钩子只用于改变内部状态，不能让 Cancel 抛出。
            }
        }
        return id;
    }

    void RemoveBeforeDoneCallback(DoneSignal::CallbackId id) noexcept {
        if (id == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        before_done_callbacks.erase(id);
    }

    bool IsCanceled() const noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        return error != nullptr;
    }

    void RemoveChildRaw(const State* child) {
        std::lock_guard<std::mutex> lock(mutex);
        children.erase(
            std::remove_if(children.begin(), children.end(),
                           [child](const std::weak_ptr<State>& weak_child) {
                               const auto current = weak_child.lock();
                               return !current || current.get() == child;
                           }),
            children.end());
    }

    void RemoveChild(const std::shared_ptr<State>& child) {
        if (child) {
            RemoveChildRaw(child.get());
        }
    }

    static void DrainCancellation(std::vector<CancellationWork> work) {
        // Mark the complete subtree before invoking any user callback. Done
        // callbacks are allowed to observe/wait on descendants; signaling a
        // parent callback first would otherwise deadlock a synchronous
        // parent->child wait during cancellation propagation.
        std::vector<std::shared_ptr<State>> to_signal;
        std::vector<std::function<void()>> before_done_callbacks;
        while (!work.empty()) {
            CancellationWork item = std::move(work.back());
            work.pop_back();
            if (!item.state) {
                continue;
            }

            std::vector<std::shared_ptr<State>> descendants;
            ErrorPtr local_error = item.error;
            ErrorPtr local_cause = item.cause;
            TimerService::Id old_timer = 0;
            std::shared_ptr<State> parent_to_remove;
            std::vector<std::function<void()>> local_before_done_callbacks;
            {
                std::lock_guard<std::mutex> lock(item.state->mutex);
                if (item.state->root || item.state->error) {
                    continue;
                }
                if (!local_error) {
                    local_error = item.deadline_error ? DeadlineExceededError()
                                                      : CanceledError();
                }
                if (!local_cause) {
                    local_cause = local_error;
                }
                item.state->error = local_error;
                item.state->cause = local_cause;
                old_timer = item.state->timer_id;
                item.state->timer_id = 0;
                local_before_done_callbacks.reserve(
                    item.state->before_done_callbacks.size());
                for (auto& entry : item.state->before_done_callbacks) {
                    local_before_done_callbacks.push_back(
                        std::move(entry.second));
                }
                item.state->before_done_callbacks.clear();
                if (item.remove_from_parent) {
                    parent_to_remove = item.state->parent;
                }
                for (const auto& weak_child : item.state->children) {
                    if (auto child = weak_child.lock()) {
                        descendants.emplace_back(std::move(child));
                    }
                }
                item.state->children.clear();
            }

            if (old_timer != 0) {
                TimerService::Instance().Remove(old_timer);
            }
            if (parent_to_remove) {
                parent_to_remove->RemoveChild(item.state);
            }
            for (auto& callback : local_before_done_callbacks) {
                before_done_callbacks.emplace_back(std::move(callback));
            }
            to_signal.emplace_back(item.state);
            for (auto& child : descendants) {
                work.push_back(
                    {std::move(child), local_error, local_cause, false});
            }
        }
        // 所有 Context 状态都已先标记取消，再让 rollback scope 冻结其
        // undo。这样 Done 的观察者、RecordUndo 和 Commit 不会在取消窗口
        // 中错误地把新动作当成可提交状态。真正的补偿由 DoneSignal 的
        // 内部回调在 done=true、普通用户回调之前执行。
        for (auto it = before_done_callbacks.rbegin();
             it != before_done_callbacks.rend(); ++it) {
            try {
                (*it)();
            } catch (...) {
            }
        }
        for (auto it = to_signal.rbegin(); it != to_signal.rend(); ++it) {
            (*it)->done.Signal();
        }
    }

    void AddChild(const std::shared_ptr<State>& child) {
        ErrorPtr inherited_error;
        ErrorPtr inherited_cause;
        {
            std::lock_guard<std::mutex> lock(mutex);
            children.erase(std::remove_if(children.begin(), children.end(),
                                          [](const auto& item) {
                                              return item.expired();
                                          }),
                           children.end());
            if (!error) {
                children.emplace_back(child);
                return;
            }
            inherited_error = error;
            inherited_cause = cause;
        }
        child->CancelFromParent(std::move(inherited_error),
                                std::move(inherited_cause));
    }

    void Cancel(ErrorPtr requested_cause, bool deadline_error = false) {
        std::vector<CancellationWork> work;
        work.push_back({shared_from_this(), {}, std::move(requested_cause),
                        deadline_error, true});
        DrainCancellation(std::move(work));
    }

    void CancelFromParent(ErrorPtr inherited_error, ErrorPtr inherited_cause) {
        std::vector<CancellationWork> work;
        work.push_back({shared_from_this(), std::move(inherited_error),
                        std::move(inherited_cause), false, false});
        DrainCancellation(std::move(work));
    }

    std::any Lookup(const void* token, const std::string& name) const {
        auto owner = const_cast<State*>(this)->shared_from_this();
        while (owner) {
            std::shared_ptr<State> parent_owner;
            {
                std::lock_guard<std::mutex> lock(owner->mutex);
                if (token != nullptr) {
                    const auto found = owner->typed_values.find(token);
                    if (found != owner->typed_values.end()) {
                        return found->second;
                    }
                }
                if (token == nullptr) {
                    const auto found = owner->named_values.find(name);
                    if (found != owner->named_values.end()) {
                        return found->second;
                    }
                }
                parent_owner = owner->parent;
            }
            owner = std::move(parent_owner);
        }
        return std::any{};
    }
};

namespace {

// A child retains its parent state so cancellation and values remain valid
// after the caller drops the parent handle.  Releasing a long parent chain via
// ordinary shared_ptr destruction is recursive, though.  Move each parent
// into a thread-local release queue and drain it iteratively from the custom
// deleter so deep context trees cannot overflow the native stack.
thread_local bool t_draining_state_releases = false;
thread_local std::vector<std::shared_ptr<Context::State>>
    t_state_release_queue;

void QueueStateParent(std::shared_ptr<Context::State> parent) noexcept {
    if (parent) {
        t_state_release_queue.emplace_back(std::move(parent));
    }
}

struct StateDeleter {
    void operator()(Context::State* state) const noexcept {
        delete state;
        if (t_draining_state_releases) {
            return;
        }
        t_draining_state_releases = true;
        while (!t_state_release_queue.empty()) {
            auto next = std::move(t_state_release_queue.back());
            t_state_release_queue.pop_back();
            next.reset();
        }
        t_draining_state_releases = false;
    }
};

std::shared_ptr<Context::State> MakeState() {
    return std::shared_ptr<Context::State>(new Context::State(),
                                           StateDeleter{});
}

}  // namespace

Context::State::~State() {
    TimerService::Id old_timer = 0;
    std::shared_ptr<State> retained_parent;
    {
        std::lock_guard<std::mutex> lock(mutex);
        old_timer = timer_id;
        timer_id = 0;
        children.clear();
        retained_parent = std::move(parent);
    }
    if (old_timer != 0) {
        TimerService::Instance().Remove(old_timer);
    }
    if (retained_parent) {
        // 未取消的短命 child 也要及时移除 parent 的 weak 登记，避免长期
        // 只创建、不再创建新 child 时登记表单调增长。
        retained_parent->RemoveChildRaw(this);
    }
    QueueStateParent(std::move(retained_parent));
}

struct ContextRollback::State : std::enable_shared_from_this<State> {
    mutable std::mutex mutex;
    std::weak_ptr<Context::State> context_state;
    std::vector<UndoAction> actions;
    Status status{Status::kActive};
    std::uint64_t generation{1};
    bool rollback_on_cancel{true};
    bool abort_requested{false};
    bool context_cancel_claimed{false};
    bool manual_rollback_claimed{false};
    bool had_failure{false};
    std::exception_ptr first_failure;
    std::vector<UndoAction> pending_context_actions;
    DoneSignal completion;

    bool ContextCanceled() const noexcept {
        const auto context = context_state.lock();
        return context && context->IsCanceled();
    }

    bool Record(UndoAction action) {
        if (!action) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (status != Status::kActive) {
            return false;
        }
        if (rollback_on_cancel && ContextCanceled()) {
            status = Status::kRollingBack;
            pending_context_actions.swap(actions);
            context_cancel_claimed = true;
            return false;
        }
        actions.emplace_back(std::move(action));
        return true;
    }

    ContextRollback::Savepoint Mark() const noexcept {
        ContextRollback::Savepoint mark;
        std::lock_guard<std::mutex> lock(mutex);
        if (status == Status::kActive &&
            (!rollback_on_cancel || !ContextCanceled())) {
            mark.m_state = const_cast<State*>(this)->shared_from_this();
            mark.m_depth = actions.size();
            mark.m_generation = generation;
        }
        return mark;
    }

    bool BeginPartial(const ContextRollback::Savepoint& mark,
                      std::vector<UndoAction>& extracted) {
        std::lock_guard<std::mutex> lock(mutex);
        if (status == Status::kRollingBack) {
            abort_requested = true;
            return false;
        }
        if (status != Status::kActive || mark.m_generation != generation ||
            mark.m_depth > actions.size()) {
            return false;
        }
        if (rollback_on_cancel && ContextCanceled()) {
            status = Status::kRollingBack;
            pending_context_actions.swap(actions);
            context_cancel_claimed = true;
            return false;
        }
        extracted.reserve(actions.size() - mark.m_depth);
        status = Status::kRollingBack;
        while (actions.size() > mark.m_depth) {
            extracted.emplace_back(std::move(actions.back()));
            actions.pop_back();
        }
        std::reverse(extracted.begin(), extracted.end());
        return true;
    }

    void RecordFailure(std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (status == Status::kCommitted || status == Status::kRolledBack ||
            status == Status::kFailed) {
            return;
        }
        had_failure = true;
        if (!first_failure) {
            first_failure = std::move(failure);
        }
    }

    void Execute(std::vector<UndoAction>& extracted) noexcept {
        for (auto it = extracted.rbegin(); it != extracted.rend(); ++it) {
            if (!*it) {
                continue;
            }
            try {
                (*it)();
            } catch (...) {
                RecordFailure(std::current_exception());
            }
        }
        extracted.clear();
    }

    bool FinishPartial(std::vector<UndoAction>& remaining) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (abort_requested) {
            abort_requested = false;
            remaining.swap(actions);
            return true;
        }
        status = Status::kActive;
        // savepoint 是一次性令牌。即使 vector 深度随后恢复到原值，
        // 旧令牌也不能重新解释新登记的动作，避免 ABA。
        if (++generation == 0) {
            generation = 1;
        }
        return false;
    }

    void FinishFull() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            status = had_failure ? Status::kFailed : Status::kRolledBack;
            if (++generation == 0) {
                generation = 1;
            }
            abort_requested = false;
        }
        detail::DoneSignalAccess::Signal(completion);
    }

    void ClaimContextCancellation() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (!rollback_on_cancel || status == Status::kCommitted ||
            status == Status::kRolledBack || status == Status::kFailed) {
            return;
        }
        if (status == Status::kRollingBack) {
            if (!manual_rollback_claimed) {
                abort_requested = true;
            }
            return;
        }
        status = Status::kRollingBack;
        pending_context_actions.swap(actions);
        context_cancel_claimed = true;
    }

    // 取消前置钩子只负责冻结动作日志。真正的补偿在 DoneSignal 的
    // 内部回调中执行：此时 Done 已经线性化并唤醒等待者，但普通用户
    // 回调尚未开始，因此等待 RollbackDone 不会和内部回调互相阻塞。
    void ClaimContextCancellationOnly() noexcept { ClaimContextCancellation(); }

    bool BeginManualRollback() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (status == Status::kCommitted || status == Status::kRolledBack ||
            status == Status::kFailed) {
            return false;
        }
        if (status == Status::kRollingBack) {
            abort_requested = true;
            return false;
        }
        status = Status::kRollingBack;
        pending_context_actions.swap(actions);
        manual_rollback_claimed = true;
        return true;
    }

    void OnContextDone() noexcept {
        std::vector<UndoAction> extracted;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (status == Status::kCommitted || status == Status::kRolledBack ||
                status == Status::kFailed) {
                return;
            }
            if (manual_rollback_claimed) {
                manual_rollback_claimed = false;
                extracted.swap(pending_context_actions);
            } else if (context_cancel_claimed) {
                if (!rollback_on_cancel) {
                    return;
                }
                context_cancel_claimed = false;
                extracted.swap(pending_context_actions);
            } else if (status == Status::kRollingBack) {
                if (!rollback_on_cancel) {
                    return;
                }
                abort_requested = true;
                return;
            } else {
                if (!rollback_on_cancel) {
                    return;
                }
                status = Status::kRollingBack;
                extracted.swap(actions);
            }
        }
        Execute(extracted);
        FinishFull();
    }
};

// ContextRollback 的公开方法定义放在 Context::State 完整定义之后，
// 这样回调只依赖自己的共享状态，不需要暴露 Context 的内部节点。
ContextRollback::ContextRollback(ContextPtr parent, bool rollback_on_cancel)
    : m_state(std::make_shared<State>()) {
    m_state->rollback_on_cancel = rollback_on_cancel;
    auto child_pair = Context::WithCancel(parent);
    m_context = std::move(child_pair.first);
    m_state->context_state = m_context->m_state;

    const std::weak_ptr<State> weak_state(m_state);
    try {
        m_before_callback_id = m_context->AddBeforeDoneCallback([weak_state] {
            if (const auto state = weak_state.lock()) {
                state->ClaimContextCancellationOnly();
            }
        });
        m_callback_id = detail::DoneSignalAccess::AddInternalCallback(
            m_context->Done(), [weak_state] {
            if (const auto state = weak_state.lock()) {
                state->OnContextDone();
            }
            });
    } catch (...) {
        DisarmCallback();
        try {
            m_context->Cancel();
        } catch (...) {
        }
        throw;
    }
}

ContextRollback::~ContextRollback() noexcept {
    if (m_state) {
        (void)Rollback();
    }
}

ContextRollback::ContextRollback(ContextRollback&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.m_callback_mutex);
    m_state = std::move(other.m_state);
    m_context = std::move(other.m_context);
    m_callback_id = other.m_callback_id;
    m_before_callback_id = other.m_before_callback_id;
    other.m_callback_id = 0;
    other.m_before_callback_id = 0;
}

ContextRollback& ContextRollback::operator=(ContextRollback&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    DisarmCallback();
    if (m_state) {
        (void)Rollback();
    }
    {
        std::scoped_lock lock(m_callback_mutex, other.m_callback_mutex);
        m_state = std::move(other.m_state);
        m_context = std::move(other.m_context);
        m_callback_id = other.m_callback_id;
        m_before_callback_id = other.m_before_callback_id;
        other.m_callback_id = 0;
        other.m_before_callback_id = 0;
    }
    return *this;
}

void ContextRollback::DisarmCallback() noexcept {
    ContextPtr context;
    DoneSignal::CallbackId callback_id = 0;
    DoneSignal::CallbackId before_callback_id = 0;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        callback_id = m_callback_id;
        before_callback_id = m_before_callback_id;
        m_callback_id = 0;
        m_before_callback_id = 0;
        context = m_context;
    }
    if (context && before_callback_id != 0) {
        context->RemoveBeforeDoneCallback(before_callback_id);
    }
    if (context && callback_id != 0) {
        context->Done().RemoveCallback(callback_id);
    }
}

bool ContextRollback::RecordUndo(UndoAction action) {
    return m_state && m_state->Record(std::move(action));
}

bool ContextRollback::Savepoint::valid() const noexcept {
    const auto state = m_state.lock();
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->status == Status::kActive &&
           state->generation == m_generation && m_depth <= state->actions.size() &&
           (!state->rollback_on_cancel || !state->ContextCanceled());
}

ContextRollback::Savepoint ContextRollback::Mark() const noexcept {
    return m_state ? m_state->Mark() : Savepoint{};
}

bool ContextRollback::RollbackTo(const Savepoint& mark) noexcept {
    if (!m_state || mark.m_state.lock() != m_state) {
        return false;
    }
    std::vector<UndoAction> extracted;
    try {
        if (!m_state->BeginPartial(mark, extracted)) {
            return false;
        }
    } catch (...) {
        m_state->RecordFailure(std::current_exception());
        return false;
    }
    m_state->Execute(extracted);

    std::vector<UndoAction> remaining;
    if (m_state->FinishPartial(remaining)) {
        m_state->Execute(remaining);
        m_state->FinishFull();
        DisarmCallback();
    }
    return true;
}

bool ContextRollback::Rollback(ErrorPtr cause) noexcept {
    if (!m_state) {
        return false;
    }
    const bool owner = m_state->BeginManualRollback();
    if (!owner) {
        Status current;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            current = m_state->status;
        }
        if (current == Status::kCommitted) {
            return false;
        }
        bool cancellation_failed = false;
        if (m_context) {
            try {
                m_context->Cancel(std::move(cause));
            } catch (...) {
                m_state->RecordFailure(std::current_exception());
                cancellation_failed = true;
            }
        }
        // 无论最初读取状态时是否已经看到 claim，取消和 claim 可能在
        // 此期间交错；等待 Done 后再尝试领取 pending actions，避免随后
        // 注销回调把唯一的执行路径移除后留下永久等待。若另一个 owner
        // 正在执行，OnContextDone 只会设置 abort_requested 或直接返回。
        if (m_context && !cancellation_failed && !m_context->Done().IsDone()) {
            // 另一个取消拥有者可能已经写入 Err 但尚未发布 Done；先等
            // 取消线性化，再领取 pending undo，保持统一的观察顺序。
            m_context->Done().Wait();
        }
        m_state->OnContextDone();
        DisarmCallback();
        return true;
    }

    bool cancellation_failed = false;
    if (m_context) {
        try {
            m_context->Cancel(std::move(cause));
        } catch (...) {
            m_state->RecordFailure(std::current_exception());
            cancellation_failed = true;
        }
    }
    // 正常路径由 DoneSignal 的内部回调领取并执行 pending actions；
    // Cancel 失败、Context 已经在回调注销窗口完成等异常交错，则由
    // 当前线程兜底。OnContextDone 对重复调用是幂等的。
    if (m_context && !cancellation_failed && !m_context->Done().IsDone()) {
        // 另一个线程可能已经写入 Err，但还没有完成 Done.Signal。等待
        // 取消线性化，避免在 Done 之前直接执行可能观察 Done 的 undo。
        m_context->Done().Wait();
    }
    m_state->OnContextDone();
    DisarmCallback();
    return true;
}

bool ContextRollback::Commit() noexcept {
    if (!m_state) {
        return false;
    }
    bool committed = false;
    bool context_claimed = false;
    std::vector<UndoAction> discarded;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->status == Status::kActive && !m_state->had_failure) {
            if (m_state->rollback_on_cancel && m_state->ContextCanceled()) {
                m_state->status = Status::kRollingBack;
                m_state->pending_context_actions.swap(m_state->actions);
                m_state->context_cancel_claimed = true;
                context_claimed = true;
            } else {
                discarded.swap(m_state->actions);
                m_state->status = Status::kCommitted;
                if (++m_state->generation == 0) {
                    m_state->generation = 1;
                }
                committed = true;
            }
        } else if (m_state->status == Status::kCommitted) {
            committed = true;
        }
    }
    if (context_claimed) {
        // 如果 Done 尚未发布，保留内部回调；它会在取消线程完成
        // 线性化后执行 pending undo。Done 已发布时才需要当前线程兜底。
        if (m_context && m_context->Done().IsDone()) {
            m_state->OnContextDone();
            DisarmCallback();
        }
        return false;
    }
    if (committed) {
        DisarmCallback();
        // 丢弃动作对象本身可能释放临时资源；在通知等待者前完成析构，
        // 让 RollbackDone 覆盖整个提交清理过程，而不只是状态切换。
        discarded.clear();
        detail::DoneSignalAccess::Signal(m_state->completion);
    }
    return committed;
}

ContextRollback::Status ContextRollback::status() const noexcept {
    if (!m_state) {
        return Status::kRolledBack;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->status;
}

bool ContextRollback::active() const noexcept {
    return status() == Status::kActive;
}

bool ContextRollback::committed() const noexcept {
    return status() == Status::kCommitted;
}

bool ContextRollback::rolled_back() const noexcept {
    const auto current = status();
    return current == Status::kRolledBack || current == Status::kFailed;
}

bool ContextRollback::had_failure() const noexcept {
    if (!m_state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->had_failure;
}

std::exception_ptr ContextRollback::failure() const noexcept {
    if (!m_state) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->first_failure;
}

DoneSignal ContextRollback::RollbackDone() const {
    if (m_state) {
        return m_state->completion;
    }

    // 移动后的空对象没有自己的事务状态；返回一个已经完成的信号，
    // 这样通用清理代码等待它时不会永久阻塞。
    static const DoneSignal completed = [] {
        DoneSignal signal;
        detail::DoneSignalAccess::Signal(signal);
        return signal;
    }();
    return completed;
}

namespace {

TimerService::Id TimerService::Add(const std::shared_ptr<Context::State>& state,
                                   ContextTimePoint deadline) {
    const Id id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    const Id actual_id = id == 0 ? m_next_id.fetch_add(1, std::memory_order_relaxed)
                                 : id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return 0;
        }
        m_entries.emplace(deadline, Entry{deadline, actual_id, state});
    }
    m_cv.notify_all();
    return actual_id;
}

void TimerService::Remove(Id id) {
    if (id == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->second.id == id) {
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_cv.notify_all();
}

void TimerService::Run() {
    for (;;) {
        std::vector<std::weak_ptr<Context::State>> expired;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }
            if (m_entries.empty()) {
                m_cv.wait(lock, [this] { return m_stopping || !m_entries.empty(); });
                continue;
            }
            const auto deadline = m_entries.begin()->first;
            if (std::chrono::steady_clock::now() < deadline) {
                m_cv.wait_until(lock, deadline);
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            while (!m_entries.empty() && m_entries.begin()->first <= now) {
                expired.emplace_back(m_entries.begin()->second.state);
                m_entries.erase(m_entries.begin());
            }
        }
        for (auto& weak_state : expired) {
            if (auto state = weak_state.lock()) {
                state->Cancel({}, true);
            }
        }
    }
}

}  // namespace

Context::Context(std::shared_ptr<State> state)
    : m_state(std::move(state)), m_done(m_state->done) {}

DoneSignal::CallbackId Context::AddBeforeDoneCallback(
    std::function<void()> callback) {
    return m_state->AddBeforeDoneCallback(std::move(callback));
}

void Context::RemoveBeforeDoneCallback(DoneSignal::CallbackId id) const noexcept {
    m_state->RemoveBeforeDoneCallback(id);
}

ContextPtr Context::Background() {
    static const ContextPtr root = [] {
        auto state = MakeState();
        state->root = true;
        return ContextPtr(new Context(std::move(state)));
    }();
    return root;
}

ContextPtr Context::TODO() {
    static const ContextPtr root = [] {
        auto state = MakeState();
        state->root = true;
        return ContextPtr(new Context(std::move(state)));
    }();
    return root;
}

ContextPtr Context::MakeChild(const ContextPtr& parent) {
    // Go rejects a nil parent; translated C++ code receives an explicit root
    // instead so the operation remains memory-safe and non-throwing.
    const auto actual_parent = parent ? parent : Background();
    auto state = MakeState();
    state->parent = actual_parent->m_state;  // child retains its parent anchor
    {
        std::lock_guard<std::mutex> lock(actual_parent->m_state->mutex);
        state->deadline = actual_parent->m_state->deadline;
    }
    auto child = ContextPtr(new Context(state));
    actual_parent->m_state->AddChild(state);
    return child;
}

ContextPtr Context::MakeValueContext(const ContextPtr& parent, const void* token,
                                     const std::string& name,
                                     std::shared_ptr<const std::uint64_t>
                                         token_anchor,
                                     std::any value) {
    auto child = MakeChild(parent);
    try {
        std::lock_guard<std::mutex> lock(child->m_state->mutex);
        if (token != nullptr) {
            child->m_state->typed_values[token] = std::move(value);
            child->m_state->typed_key_anchors[token] = std::move(token_anchor);
        } else {
            child->m_state->named_values[name] = std::move(value);
        }
    } catch (...) {
        try {
            child->Cancel();
        } catch (...) {
        }
        throw;
    }
    return child;
}

std::pair<ContextPtr, CancelFunc> Context::WithCancel(const ContextPtr& parent) {
    auto child = MakeChild(parent);
    const auto state = child->m_state;
    return {std::move(child), [state] { state->Cancel({}); }};
}

std::pair<ContextPtr, CancelCauseFunc> Context::WithCancelCause(
    const ContextPtr& parent) {
    auto child = MakeChild(parent);
    const auto state = child->m_state;
    return {std::move(child), [state](ErrorPtr cause) {
                state->Cancel(std::move(cause));
            }};
}

std::pair<ContextPtr, CancelFunc> Context::WithDeadline(
    const ContextPtr& parent, ContextTimePoint requested_deadline) {
    auto child = MakeChild(parent);
    const auto inherited = child->Deadline();
    if (!inherited.has_value() || requested_deadline < *inherited) {
        {
            std::lock_guard<std::mutex> lock(child->m_state->mutex);
            if (child->m_state->error) {
                // Parent won the cancellation race while the child was made.
                requested_deadline = ContextTimePoint::max();
            } else {
                child->m_state->deadline = requested_deadline;
            }
        }
        if (requested_deadline != ContextTimePoint::max()) {
            if (requested_deadline <= Now()) {
                // Go closes an already-expired context before returning it;
                // do this synchronously instead of relying on the timer
                // worker's next scheduling turn.
                child->m_state->Cancel({}, true);
            } else {
                TimerService::Id id = 0;
                try {
                    id = TimerService::Instance().Add(child->m_state,
                                                      requested_deadline);
                } catch (...) {
                    try {
                        child->Cancel();
                    } catch (...) {
                    }
                    throw;
                }
                if (id == 0) {
                    // TimerService 正在停止，不能留下一个永远不会触发的
                    // deadline child；把注册失败转换为显式 deadline 取消。
                    child->m_state->Cancel({}, true);
                } else {
                    std::lock_guard<std::mutex> lock(child->m_state->mutex);
                    if (!child->m_state->error) {
                        child->m_state->timer_id = id;
                    } else {
                        TimerService::Instance().Remove(id);
                    }
                }
            }
        }
    }
    const auto state = child->m_state;
    return {std::move(child), [state] { state->Cancel({}); }};
}

std::pair<ContextPtr, CancelFunc> Context::WithTimeout(
    const ContextPtr& parent, ContextDuration timeout) {
    return WithDeadline(parent, SaturatingDeadline(Now(), timeout));
}

ContextPtr Context::WithValue(const ContextPtr& parent, std::string key,
                             std::any value) {
    return MakeValueContext(parent, nullptr, key, {}, std::move(value));
}

bool Context::IsDone() const noexcept { return m_state->done.IsDone(); }

ErrorPtr Context::Err() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->error;
}

ErrorPtr Context::Cause() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->cause;
}

std::optional<ContextTimePoint> Context::Deadline() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->deadline;
}

bool Context::HasDeadline() const { return Deadline().has_value(); }

std::any Context::LookupValue(const void* token, const std::string& name) const {
    return m_state->Lookup(token, name);
}

std::any Context::Value(const std::string& key) const {
    return LookupValue(nullptr, key);
}

void Context::Cancel(ErrorPtr cause) { m_state->Cancel(std::move(cause)); }

void Context::CancelDeadline() { m_state->Cancel({}, true); }

void Context::SetNowFunctionForTesting(NowFunction now) {
    std::lock_guard<std::mutex> lock(s_clock_mutex);
    s_now_function = std::move(now);
}

void Context::ResetNowFunctionForTesting() {
    std::lock_guard<std::mutex> lock(s_clock_mutex);
    s_now_function = {};
}

ContextTimePoint Context::Now() {
    NowFunction now;
    {
        std::lock_guard<std::mutex> lock(s_clock_mutex);
        now = s_now_function;
    }
    return now ? now() : Clock::now();
}

ContextPtr Background() { return Context::Background(); }
ContextPtr TODO() { return Context::TODO(); }
std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr& parent) {
    return Context::WithCancel(parent);
}
std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(const ContextPtr& parent) {
    return Context::WithCancelCause(parent);
}
std::pair<ContextPtr, CancelFunc> WithDeadline(const ContextPtr& parent,
                                               ContextTimePoint deadline) {
    return Context::WithDeadline(parent, deadline);
}
std::pair<ContextPtr, CancelFunc> WithTimeout(const ContextPtr& parent,
                                             ContextDuration timeout) {
    return Context::WithTimeout(parent, timeout);
}
ContextRollback WithRollback(const ContextPtr& parent, bool rollback_on_cancel) {
    return ContextRollback(parent, rollback_on_cancel);
}
ContextPtr WithValue(const ContextPtr& parent, std::string key, std::any value) {
    return Context::WithValue(parent, std::move(key), std::move(value));
}

ErrorPtr CanceledError() {
    static const ErrorPtr error = NewError("context canceled");
    return error;
}

ErrorPtr DeadlineExceededError() {
    static const ErrorPtr error = NewError("context deadline exceeded");
    return error;
}

}  // namespace go2cpp
