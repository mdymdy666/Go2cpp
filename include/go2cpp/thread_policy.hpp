#pragma once

#include <cstdint>

namespace go2cpp {

namespace scheduler { class Scheduler; }

enum class ThreadParticipationMode : std::uint8_t {
    kUnmanaged,
    kGmpEligible,

    Unmanaged = kUnmanaged,
    GmpEligible = kGmpEligible,
};

enum class ThreadHookMode : std::uint8_t {
    kInherit,
    kDisabled,
    kEnabled,

    Inherit = kInherit,
    Disabled = kDisabled,
    Enabled = kEnabled,
};

// A snapshot is observational. The scheduler pointer is non-owning and is only valid
// while the associated Scheduler remains alive; querying the pointer does not
// attach this thread to a worker or reserve a P.
struct ThreadPolicySnapshot {
    scheduler::Scheduler* scheduler{nullptr};
    ThreadParticipationMode participation{ThreadParticipationMode::kUnmanaged};
    ThreadHookMode hook{ThreadHookMode::kInherit};
    bool runtime_worker{false};

    bool participates_in_gmp() const noexcept {
        return scheduler != nullptr &&
               participation == ThreadParticipationMode::kGmpEligible;
    }
};

ThreadPolicySnapshot CurrentThreadPolicy() noexcept;
inline ThreadPolicySnapshot current_thread_policy() noexcept {
    return CurrentThreadPolicy();
}

bool ThreadParticipatesInGMP() noexcept;
inline bool thread_participates_in_gmp() noexcept {
    return ThreadParticipatesInGMP();
}

// Marks the current OS thread as eligible for a scheduler policy for one scope.
// This does not create an M, bind a P, install Scheduler::current_scheduler(),
// or execute queued work. Move/destruction must stay on this same OS thread
// because the state being restored is thread-local.
class ScopedThreadParticipation final {
public:
    explicit ScopedThreadParticipation(
        scheduler::Scheduler* scheduler,
        ThreadParticipationMode mode = ThreadParticipationMode::kGmpEligible)
        noexcept;
    explicit ScopedThreadParticipation(
        scheduler::Scheduler& scheduler,
        ThreadParticipationMode mode = ThreadParticipationMode::kGmpEligible)
        noexcept;
    ~ScopedThreadParticipation();

    ScopedThreadParticipation(const ScopedThreadParticipation&) = delete;
    ScopedThreadParticipation& operator=(const ScopedThreadParticipation&) = delete;
    // TLS restoration is bound to the creating OS thread; do not move the
    // scope into another thread or Fiber.
    ScopedThreadParticipation(ScopedThreadParticipation&&) = delete;
    ScopedThreadParticipation& operator=(ScopedThreadParticipation&&) = delete;

private:
    ThreadPolicySnapshot m_previous{};
    bool m_active{false};
};

ThreadHookMode CurrentThreadHookMode() noexcept;
void SetCurrentThreadHookMode(ThreadHookMode mode) noexcept;

// Per-thread hook override. kInherit follows the process-wide hook switch;
// kDisabled suppresses new cooperative admissions on this thread, while
// kEnabled opts this thread into the hook path. Move/destruction must stay on
// the same OS thread because the state being restored is thread-local.
class ScopedThreadHookMode final {
public:
    explicit ScopedThreadHookMode(ThreadHookMode mode) noexcept;
    ~ScopedThreadHookMode();

    ScopedThreadHookMode(const ScopedThreadHookMode&) = delete;
    ScopedThreadHookMode& operator=(const ScopedThreadHookMode&) = delete;
    // TLS restoration is bound to the creating OS thread; do not move the
    // scope into another thread or Fiber.
    ScopedThreadHookMode(ScopedThreadHookMode&&) = delete;
    ScopedThreadHookMode& operator=(ScopedThreadHookMode&&) = delete;

private:
    ThreadHookMode m_previous{ThreadHookMode::kInherit};
    bool m_active{false};
};

namespace thread_policy::detail {

// Called by scheduler worker entry/exit. This is a source-level hook, not an
// ABI promise; external threads should use ScopedThreadParticipation and must
// explicitly execute scheduler work through a future backend API.
void EnterRuntimeWorker(scheduler::Scheduler* scheduler) noexcept;
void LeaveRuntimeWorker() noexcept;

// Combines the process-wide hook switch with the current thread override.
bool HookAllowed(bool process_enabled) noexcept;

}  // namespace thread_policy::detail

}  // namespace go2cpp
