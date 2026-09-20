#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>

namespace go2cpp {

enum class FiberState : std::uint8_t {
    Ready,
    Running,
    Suspended,
    Completed,
    Failed,
};

enum class SuspendReason : std::uint8_t {
    None,
    Yield,
    Park,
    Io,
    Timer,
    Synchronization,
};

// A movable execution stack with a protected guard page. A Fiber can migrate
// between OS threads, provided resume() calls are sequential. Boost.Context
// remains an implementation detail and is not exposed by this interface.
class Fiber {
public:
    using Function = std::function<void()>;

    static constexpr std::size_t DefaultStackSize() noexcept {
        return 128U * 1024U;
    }

    explicit Fiber(Function function,
                   std::size_t stack_size = DefaultStackSize());
    // Destruction is a cancellation request followed by join. A Ready Fiber
    // skips its body; a Suspended Fiber resumes until it returns naturally.
    // The owner must outlive resume(), and destruction must not occur from
    // the Fiber itself. A body that ignores cancellation can block this join.
    ~Fiber();
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;

    // Transfers control to this Fiber. Returns false for a running, completed,
    // failed, or concurrently resumed Fiber. User exceptions never escape;
    // they are retained by failure() and set state() to Failed.
    bool resume() noexcept;

    // Suspends the currently running Fiber. Returns false outside a Fiber or
    // when reason is None. Execution continues here after the next resume().
    static bool Suspend(SuspendReason reason = SuspendReason::Yield) noexcept;
    static Fiber* Current() noexcept;
    static bool CancellationRequested() noexcept;
    // Scheduler/task cancellation is cooperative. This sets the Fiber-local
    // flag observed by CancellationRequested() before the next resume.
    void RequestCancellation() noexcept;

    FiberState state() const noexcept;
    SuspendReason suspend_reason() const noexcept;
    std::exception_ptr failure() const;
    std::size_t stack_size() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp
