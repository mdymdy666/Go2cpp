#include "go2cpp/context.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp {

struct DoneSignal::State {
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    bool done{false};
    CallbackId next_id{1};
    std::unordered_map<CallbackId, std::function<void()>> callbacks;
};

DoneSignal::DoneSignal() : m_state(std::make_shared<State>()) {}
DoneSignal::~DoneSignal() = default;

void DoneSignal::Wait() const {
    std::unique_lock<std::mutex> lock(m_state->mutex);
    m_state->cv.wait(lock, [this] { return m_state->done; });
}

bool DoneSignal::WaitFor(ContextDuration timeout) const {
    std::unique_lock<std::mutex> lock(m_state->mutex);
    return m_state->cv.wait_for(lock, timeout, [this] { return m_state->done; });
}

bool DoneSignal::WaitUntil(ContextTimePoint deadline) const {
    std::unique_lock<std::mutex> lock(m_state->mutex);
    return m_state->cv.wait_until(lock, deadline, [this] { return m_state->done; });
}

bool DoneSignal::IsDone() const noexcept {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->done;
}

DoneSignal::CallbackId DoneSignal::AddCallback(
    std::function<void()> callback) const {
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
            m_state->callbacks.emplace(id, std::move(callback));
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
}

void DoneSignal::Signal() const noexcept {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->done) {
            return;
        }
        m_state->done = true;
        callbacks.reserve(m_state->callbacks.size());
        for (auto& entry : m_state->callbacks) {
            callbacks.push_back(std::move(entry.second));
        }
        m_state->callbacks.clear();
    }
    m_state->cv.notify_all();
    for (auto& callback : callbacks) {
        try {
            callback();
        } catch (...) {
        }
    }
}

namespace {

std::mutex s_clock_mutex;
Context::NowFunction s_now_function;

class TimerService {
public:
    using Id = std::uint64_t;

    static TimerService& Instance() {
        static TimerService service;
        return service;
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

    ~TimerService() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) {
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

    ~State();

    struct CancellationWork {
        std::shared_ptr<State> state;
        ErrorPtr error;
        ErrorPtr cause;
        bool deadline_error{false};
    };

    static void DrainCancellation(std::vector<CancellationWork> work) {
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
            item.state->done.Signal();
            for (auto& child : descendants) {
                work.push_back(
                    {std::move(child), local_error, local_cause, false});
            }
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
                        deadline_error});
        DrainCancellation(std::move(work));
    }

    void CancelFromParent(ErrorPtr inherited_error, ErrorPtr inherited_cause) {
        std::vector<CancellationWork> work;
        work.push_back({shared_from_this(), std::move(inherited_error),
                        std::move(inherited_cause), false});
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
    QueueStateParent(std::move(retained_parent));
}

namespace {

TimerService::Id TimerService::Add(const std::shared_ptr<Context::State>& state,
                                   ContextTimePoint deadline) {
    const Id id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    const Id actual_id = id == 0 ? m_next_id.fetch_add(1, std::memory_order_relaxed)
                                 : id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stopping) {
            m_entries.emplace(deadline, Entry{deadline, actual_id, state});
        }
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
    {
        std::lock_guard<std::mutex> lock(child->m_state->mutex);
        if (token != nullptr) {
            child->m_state->typed_values[token] = std::move(value);
            child->m_state->typed_key_anchors[token] = std::move(token_anchor);
        } else {
            child->m_state->named_values[name] = std::move(value);
        }
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
                const auto id = TimerService::Instance().Add(child->m_state,
                                                             requested_deadline);
                std::lock_guard<std::mutex> lock(child->m_state->mutex);
                if (!child->m_state->error) {
                    child->m_state->timer_id = id;
                } else {
                    TimerService::Instance().Remove(id);
                }
            }
        }
    }
    const auto state = child->m_state;
    return {std::move(child), [state] { state->Cancel({}); }};
}

std::pair<ContextPtr, CancelFunc> Context::WithTimeout(
    const ContextPtr& parent, ContextDuration timeout) {
    return WithDeadline(parent, Now() + timeout);
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
