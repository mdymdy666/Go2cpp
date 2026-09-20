#include "go2cpp/fiber.hpp"

#include "go2cpp/fiber_local.hpp"

#include "go2cpp/panic_defer.hpp"

#include <boost/context/detail/fcontext.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>
#include <boost/context/stack_traits.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <mutex>
#include <utility>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GO2CPP_FIBER_ASAN 1
#endif
#if __has_feature(thread_sanitizer)
#define GO2CPP_FIBER_TSAN 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__) && !defined(GO2CPP_FIBER_ASAN)
#define GO2CPP_FIBER_ASAN 1
#endif
#if defined(__SANITIZE_THREAD__) && !defined(GO2CPP_FIBER_TSAN)
#define GO2CPP_FIBER_TSAN 1
#endif

#if defined(GO2CPP_FIBER_ASAN)
#include <sanitizer/common_interface_defs.h>
#endif
#if defined(GO2CPP_FIBER_TSAN)
#include <sanitizer/tsan_interface.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define GO2CPP_FIBER_NOINLINE __attribute__((noinline))
#else
#define GO2CPP_FIBER_NOINLINE
#endif

namespace go2cpp {
namespace {

thread_local Fiber* s_current_fiber = nullptr;

// Keep TLS address lookup outside the function that suspends. A compiler may
// otherwise cache __errno_location() across a cross-thread context switch.
GO2CPP_FIBER_NOINLINE int load_errno() noexcept { return errno; }

GO2CPP_FIBER_NOINLINE void store_errno(int value) noexcept { errno = value; }

std::size_t normalize_stack_size(std::size_t requested) noexcept {
    if (requested == 0) {
        requested = Fiber::DefaultStackSize();
    }
    const auto minimum = boost::context::stack_traits::minimum_size();
    const auto page_size = boost::context::stack_traits::page_size();
    const auto safe_maximum =
        std::numeric_limits<std::size_t>::max() - 2U * page_size;
    std::size_t result = std::clamp(requested, minimum, safe_maximum);
    if (!boost::context::stack_traits::is_unbounded()) {
        result = std::min(result, boost::context::stack_traits::maximum_size());
    }
    return result;
}

struct FiberStack {
    explicit FiberStack(std::size_t size)
        : m_allocator(size), m_context(m_allocator.allocate()) {}

    ~FiberStack() noexcept { m_allocator.deallocate(m_context); }

    FiberStack(const FiberStack&) = delete;
    FiberStack& operator=(const FiberStack&) = delete;

    std::size_t usable_size() const noexcept {
        return m_context.size - boost::context::stack_traits::page_size();
    }

    const void* bottom() const noexcept {
        return static_cast<const char*>(m_context.sp) - usable_size();
    }

    boost::context::protected_fixedsize_stack m_allocator;
    boost::context::stack_context m_context;
};

}  // namespace

struct Fiber::Impl {
    explicit Impl(Fiber* owner, Function function, std::size_t stack_size)
        : m_owner(owner),
          m_function(std::move(function)),
          m_stack_size(normalize_stack_size(stack_size)),
          m_stack(m_stack_size) {
        m_context = boost::context::detail::make_fcontext(
            m_stack.m_context.sp, m_stack.usable_size(), &Impl::entry);
#if defined(GO2CPP_FIBER_TSAN)
        m_tsan_fiber = __tsan_create_fiber(0);
#endif
    }

    ~Impl() noexcept {
        m_cancel_requested.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> resume_lock(m_resume_mutex);
        while (m_state.load(std::memory_order_acquire) == FiberState::Ready ||
               m_state.load(std::memory_order_acquire) == FiberState::Suspended) {
            (void)resume_locked();
        }
#if defined(GO2CPP_FIBER_TSAN)
        __tsan_destroy_fiber(m_tsan_fiber);
#endif
    }

    static void entry(boost::context::detail::transfer_t transfer) noexcept {
        auto* const self = static_cast<Impl*>(transfer.data);
        self->finish_switch_to_fiber();
        self->m_caller = transfer.fctx;
        store_errno(self->m_saved_errno);

        FiberState terminal_state = FiberState::Completed;
        try {
            if (!self->m_cancel_requested.load(std::memory_order_acquire) &&
                self->m_function) {
                self->m_function();
            }
        } catch (...) {
            // This boundary contains accidental user throws only. Fiber
            // suspension, cancellation and cleanup never use exceptions.
            std::lock_guard<std::mutex> lock(self->m_failure_mutex);
            self->m_failure = std::current_exception();
            terminal_state = FiberState::Failed;
        }
        self->m_function = {};
        // Fiber-local values belong to the logical G, not to the worker M.
        // Run their destructors after the body/defer stack has unwound and
        // before handing control back to the caller.
        fiber_local::detail::Cleanup(self->m_owner);
        self->m_reason.store(SuspendReason::None, std::memory_order_release);
        self->m_state.store(terminal_state, std::memory_order_release);
        self->m_saved_errno = load_errno();

#if defined(GO2CPP_FIBER_ASAN)
        // A completed execution stack is never resumed; discard its fake
        // stack before the caller releases the protected allocation.
        __sanitizer_start_switch_fiber(nullptr, self->m_asan_caller_bottom,
                                      self->m_asan_caller_size);
#endif
#if defined(GO2CPP_FIBER_TSAN)
        __tsan_switch_to_fiber(self->m_tsan_caller, 0);
#endif
        boost::context::detail::jump_fcontext(self->m_caller, self);
        std::terminate();
    }

    bool resume() noexcept {
        std::unique_lock<std::mutex> resume_lock(m_resume_mutex,
                                                std::try_to_lock);
        return resume_lock.owns_lock() && resume_locked();
    }

    bool resume_locked() noexcept {
        const FiberState current = m_state.load(std::memory_order_acquire);
        if (current != FiberState::Ready && current != FiberState::Suspended) {
            return false;
        }
        m_state.store(FiberState::Running, std::memory_order_release);
        m_reason.store(SuspendReason::None, std::memory_order_release);

        const int caller_errno = load_errno();
        Fiber* const previous_fiber = s_current_fiber;
        s_current_fiber = m_owner;
        store_errno(m_saved_errno);

        {
            panic_defer::Binding binding(m_execution_context);
#if defined(GO2CPP_FIBER_ASAN)
            void* caller_fake_stack = nullptr;
            __sanitizer_start_switch_fiber(&caller_fake_stack, m_stack.bottom(),
                                          m_stack.usable_size());
#endif
#if defined(GO2CPP_FIBER_TSAN)
            m_tsan_caller = __tsan_get_current_fiber();
            __tsan_switch_to_fiber(m_tsan_fiber, 0);
#endif
            const boost::context::detail::transfer_t transfer =
                boost::context::detail::jump_fcontext(m_context, this);
#if defined(GO2CPP_FIBER_ASAN)
            __sanitizer_finish_switch_fiber(caller_fake_stack, nullptr, nullptr);
#endif
            const auto next = m_state.load(std::memory_order_acquire);
            m_context = next == FiberState::Completed || next == FiberState::Failed
                            ? nullptr
                            : transfer.fctx;
        }

        s_current_fiber = previous_fiber;
        store_errno(caller_errno);
        return true;
    }

    bool suspend(SuspendReason reason) noexcept {
        if (reason == SuspendReason::None ||
            m_state.load(std::memory_order_acquire) != FiberState::Running) {
            return false;
        }
        m_saved_errno = load_errno();
        m_reason.store(reason, std::memory_order_release);
        m_state.store(FiberState::Suspended, std::memory_order_release);

#if defined(GO2CPP_FIBER_ASAN)
        __sanitizer_start_switch_fiber(&m_asan_fake_stack, m_asan_caller_bottom,
                                      m_asan_caller_size);
#endif
#if defined(GO2CPP_FIBER_TSAN)
        __tsan_switch_to_fiber(m_tsan_caller, 0);
#endif
        const boost::context::detail::transfer_t transfer =
            boost::context::detail::jump_fcontext(m_caller, this);
        finish_switch_to_fiber();
        m_caller = transfer.fctx;
        store_errno(m_saved_errno);
        m_reason.store(SuspendReason::None, std::memory_order_release);
        m_state.store(FiberState::Running, std::memory_order_release);
        return true;
    }

    void finish_switch_to_fiber() noexcept {
#if defined(GO2CPP_FIBER_ASAN)
        // On migration these outputs describe the newly resuming OS thread,
        // not the thread that last suspended this Fiber.
        __sanitizer_finish_switch_fiber(m_asan_fake_stack, &m_asan_caller_bottom,
                                       &m_asan_caller_size);
#endif
    }

    Fiber* m_owner;
    Function m_function;
    const std::size_t m_stack_size;
    FiberStack m_stack;
    panic_defer::ExecutionContext m_execution_context;
    boost::context::detail::fcontext_t m_context{nullptr};
    boost::context::detail::fcontext_t m_caller{nullptr};
    std::atomic<FiberState> m_state{FiberState::Ready};
    std::atomic<SuspendReason> m_reason{SuspendReason::None};
    std::atomic<bool> m_cancel_requested{false};
    std::mutex m_resume_mutex;
    mutable std::mutex m_failure_mutex;
    std::exception_ptr m_failure;
    int m_saved_errno{0};
#if defined(GO2CPP_FIBER_ASAN)
    void* m_asan_fake_stack{nullptr};
    const void* m_asan_caller_bottom{nullptr};
    std::size_t m_asan_caller_size{0};
#endif
#if defined(GO2CPP_FIBER_TSAN)
    void* m_tsan_fiber{nullptr};
    void* m_tsan_caller{nullptr};
#endif
};

Fiber::Fiber(Function function, std::size_t stack_size)
    : m_impl(std::make_unique<Impl>(this, std::move(function), stack_size)) {}

Fiber::~Fiber() = default;

bool Fiber::resume() noexcept { return m_impl->resume(); }

bool Fiber::Suspend(SuspendReason reason) noexcept {
    Fiber* const current = Current();
    return current != nullptr && current->m_impl->suspend(reason);
}

GO2CPP_FIBER_NOINLINE Fiber* Fiber::Current() noexcept { return s_current_fiber; }

bool Fiber::CancellationRequested() noexcept {
    Fiber* const current = Current();
    return current != nullptr &&
           current->m_impl->m_cancel_requested.load(std::memory_order_acquire);
}

void Fiber::RequestCancellation() noexcept {
    if (m_impl) {
        m_impl->m_cancel_requested.store(true, std::memory_order_release);
    }
}

FiberState Fiber::state() const noexcept {
    return m_impl->m_state.load(std::memory_order_acquire);
}

SuspendReason Fiber::suspend_reason() const noexcept {
    return m_impl->m_reason.load(std::memory_order_acquire);
}

std::exception_ptr Fiber::failure() const {
    std::lock_guard<std::mutex> lock(m_impl->m_failure_mutex);
    return m_impl->m_failure;
}

std::size_t Fiber::stack_size() const noexcept { return m_impl->m_stack_size; }

}  // namespace go2cpp
