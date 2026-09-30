#include "go2cpp/fiber.hpp"

#include "go2cpp/detail/context_backend.hpp"
#include "go2cpp/fiber_local.hpp"

#if !defined(GO2CPP_USE_NATIVE_CONTEXT)
#include <boost/context/detail/fcontext.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>
#include <boost/context/stack_traits.hpp>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

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
// 没有运行真实 Fiber 时，每个 OS 线程仍有一个不可 resume 的根上下文。
// 调度器在 worker 进入/离开 G 时更新这份元数据，Fiber 首次进入时继承它。
thread_local FiberExecutionBinding s_main_execution{};

// resume_locked 可能沿父链循环多次，任意失败/取消返回都必须恢复进入
// resume 前的直接调用者，避免 TLS 残留成已完成或错误的 Fiber。
struct FiberTlsRestore final {
    explicit FiberTlsRestore(Fiber* previous) noexcept : m_previous(previous) {}
    ~FiberTlsRestore() noexcept { s_current_fiber = m_previous; }

    FiberTlsRestore(const FiberTlsRestore&) = delete;
    FiberTlsRestore& operator=(const FiberTlsRestore&) = delete;

    Fiber* m_previous;
};

std::atomic<std::uint64_t> s_next_fiber_id{1};

// 受保护栈的映射/解除映射成本远高于一次 Fiber 上下文切换。栈不能在
// Fiber 仍可恢复时复用，因此只在 Fiber 完成、且其 fcontext 已经失效后
// 放回缓存。先使用当前 M 的 TLS 缓存，跨 M 销毁时再进入有界全局缓存。
struct CachedStack {
    std::size_t size{0};
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
    void* mapping{nullptr};
    std::size_t mapping_size{0};
#else
    boost::context::stack_context context{};
#endif
};

struct StackCache {
    ~StackCache() noexcept {
        for (auto& entry : entries) {
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
            if (entry.mapping != nullptr) {
                ::munmap(entry.mapping, entry.mapping_size);
            }
#else
            if (entry.context.sp != nullptr) {
                boost::context::protected_fixedsize_stack allocator(entry.size);
                allocator.deallocate(entry.context);
            }
#endif
        }
    }

    std::vector<CachedStack> entries;
};

std::mutex s_stack_pool_mutex;
StackCache s_stack_pool;
thread_local StackCache s_thread_stack_pool;

FiberContextFrame main_context_frame(
    const FiberExecutionBinding& binding) noexcept {
    FiberContextFrame frame;
    frame.main_fiber = true;
    frame.alive = true;
    frame.active = true;
    frame.depth = 0;
    frame.state = FiberState::Running;
    frame.last_thread = std::this_thread::get_id();
    frame.execution = binding;
    return frame;
}

// Keep TLS address lookup outside the function that suspends. A compiler may
// otherwise cache __errno_location() across a cross-thread context switch.
GO2CPP_FIBER_NOINLINE int load_errno() noexcept { return errno; }

GO2CPP_FIBER_NOINLINE void store_errno(int value) noexcept { errno = value; }

std::size_t normalize_stack_size(std::size_t requested) noexcept {
    if (requested == 0) {
        requested = Fiber::DefaultStackSize();
    }
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
    const auto minimum = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    const auto page_size = minimum;
    return std::max(requested, minimum * 2U) +
           ((page_size - (std::max(requested, minimum * 2U) % page_size)) %
            page_size);
#else
    const auto minimum = boost::context::stack_traits::minimum_size();
    const auto page_size = boost::context::stack_traits::page_size();
    const auto safe_maximum =
        std::numeric_limits<std::size_t>::max() - 2U * page_size;
    std::size_t result = std::clamp(requested, minimum, safe_maximum);
    if (!boost::context::stack_traits::is_unbounded()) {
        result = std::min(result, boost::context::stack_traits::maximum_size());
    }
    return result;
#endif
}

struct FiberStack {
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
    explicit FiberStack(std::size_t size)
        : m_requested_size(size), m_page_size(page_size()), m_mapping_size(
              m_requested_size + m_page_size),
          m_mapping(acquire(m_requested_size, m_page_size, m_mapping_size)) {
        if (m_mapping == MAP_FAILED) {
            m_mapping = nullptr;
            throw std::bad_alloc();
        }
    }

    ~FiberStack() noexcept {
        if (m_mapping != nullptr) {
            release(m_requested_size, m_page_size, m_mapping_size, m_mapping);
        }
    }

    FiberStack(const FiberStack&) = delete;
    FiberStack& operator=(const FiberStack&) = delete;

    std::size_t usable_size() const noexcept { return m_requested_size; }
    const void* bottom() const noexcept {
        return static_cast<const char*>(m_mapping) + m_page_size;
    }
    void* stack_pointer() const noexcept {
        return static_cast<char*>(m_mapping) + m_mapping_size;
    }

private:
    static std::size_t page_size() noexcept {
        const auto value = ::sysconf(_SC_PAGESIZE);
        return value > 0 ? static_cast<std::size_t>(value) : 4096U;
    }

    static void* acquire(std::size_t size, std::size_t page,
                         std::size_t mapping_size) {
        for (auto it = s_thread_stack_pool.entries.rbegin();
             it != s_thread_stack_pool.entries.rend(); ++it) {
            if (it->size == size && it->mapping != nullptr) {
                void* mapping = it->mapping;
                s_thread_stack_pool.entries.erase(std::next(it).base());
                if (::mprotect(mapping, page, PROT_NONE) != 0) {
                    ::munmap(mapping, mapping_size);
                    throw std::system_error(errno, std::generic_category(),
                                             "mprotect fiber guard page");
                }
                return mapping;
            }
        }
        {
            std::lock_guard<std::mutex> lock(s_stack_pool_mutex);
            for (auto it = s_stack_pool.entries.rbegin();
                 it != s_stack_pool.entries.rend(); ++it) {
                if (it->size == size && it->mapping != nullptr) {
                    void* mapping = it->mapping;
                    s_stack_pool.entries.erase(std::next(it).base());
                    if (::mprotect(mapping, page, PROT_NONE) != 0) {
                        ::munmap(mapping, mapping_size);
                        throw std::system_error(
                            errno, std::generic_category(),
                            "mprotect fiber guard page");
                    }
                    return mapping;
                }
            }
        }
        void* mapping = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            return MAP_FAILED;
        }
        if (::mprotect(mapping, page, PROT_NONE) != 0) {
            ::munmap(mapping, mapping_size);
            throw std::system_error(errno, std::generic_category(),
                                     "mprotect fiber guard page");
        }
        return mapping;
    }

    static void release(std::size_t size, std::size_t page,
                        std::size_t mapping_size, void* mapping) noexcept {
        (void)page;
        try {
            if (s_thread_stack_pool.entries.size() < 8U) {
                s_thread_stack_pool.entries.push_back(
                    CachedStack{size, mapping, mapping_size});
                return;
            }
            std::lock_guard<std::mutex> lock(s_stack_pool_mutex);
            if (s_stack_pool.entries.size() < 128U) {
                s_stack_pool.entries.push_back(
                    CachedStack{size, mapping, mapping_size});
                return;
            }
        } catch (...) {
        }
        ::munmap(mapping, mapping_size);
    }

    std::size_t m_requested_size;
    std::size_t m_page_size;
    std::size_t m_mapping_size;
    void* m_mapping;
#else
    explicit FiberStack(std::size_t size)
        : m_requested_size(size),
          m_allocator(size),
          m_context(acquire(m_allocator, m_requested_size)) {}

    ~FiberStack() noexcept { release(m_allocator, m_requested_size, m_context); }

    FiberStack(const FiberStack&) = delete;
    FiberStack& operator=(const FiberStack&) = delete;

    std::size_t usable_size() const noexcept {
        return m_context.size - boost::context::stack_traits::page_size();
    }

    const void* bottom() const noexcept {
        return static_cast<const char*>(m_context.sp) - usable_size();
    }

    void* stack_pointer() const noexcept { return m_context.sp; }

private:
    static boost::context::stack_context acquire(
        boost::context::protected_fixedsize_stack& allocator,
        std::size_t size) {
        for (auto it = s_thread_stack_pool.entries.rbegin();
             it != s_thread_stack_pool.entries.rend(); ++it) {
            if (it->size == size) {
                const auto context = it->context;
                s_thread_stack_pool.entries.erase(std::next(it).base());
                return context;
            }
        }
        {
            std::lock_guard<std::mutex> lock(s_stack_pool_mutex);
            for (auto it = s_stack_pool.entries.rbegin();
                 it != s_stack_pool.entries.rend(); ++it) {
                if (it->size == size) {
                    const auto context = it->context;
                    s_stack_pool.entries.erase(std::next(it).base());
                    return context;
                }
            }
        }
        return allocator.allocate();
    }

    static void release(boost::context::protected_fixedsize_stack& allocator,
                        std::size_t size,
                        boost::context::stack_context& context) noexcept {
        if (context.sp == nullptr) {
            return;
        }
        try {
            if (s_thread_stack_pool.entries.size() < 8U) {
                s_thread_stack_pool.entries.push_back(CachedStack{size, context});
                context = {};
                return;
            }
            std::lock_guard<std::mutex> lock(s_stack_pool_mutex);
            if (s_stack_pool.entries.size() < 128U) {
                s_stack_pool.entries.push_back(CachedStack{size, context});
                context = {};
                return;
            }
        } catch (...) {
            // 缓存只用于性能优化；容量不足或分配失败时回收到系统。
        }
        allocator.deallocate(context);
        context = {};
    }

    std::size_t m_requested_size;
    boost::context::protected_fixedsize_stack m_allocator;
    boost::context::stack_context m_context;
#endif
};

// 父链只保存这份独立元数据，不保存父 Fiber 栈或 owner 的所有权。子 Fiber
// 持有父记录，因此父对象结束后仍可生成一致的墓碑帧、传播取消状态；
// 真正的 fcontext 恢复仍要求父 Fiber 对象存活。错误生命周期会进入 Failed
// 放弃路径，不能把已经失效的父栈当作可恢复上下文。
struct FiberRecord {
    explicit FiberRecord(std::uint64_t fiber_id) : id(fiber_id) {}

    const std::uint64_t id;
    std::shared_ptr<FiberRecord> parent;
    std::atomic<bool> alive{true};
    std::atomic<FiberState> state{FiberState::Ready};
    std::atomic<SuspendReason> reason{SuspendReason::None};
    std::atomic<bool> cancellation_requested{false};
    mutable std::mutex mutex;
    std::uint64_t active_parent_id{0};
    std::size_t depth{0};
    std::thread::id last_thread{};
    FiberExecutionBinding execution{};
};

}  // namespace

struct Fiber::Impl {
    explicit Impl(Fiber* owner, Function function, std::size_t stack_size)
        : m_owner(owner),
          m_id(s_next_fiber_id.fetch_add(1, std::memory_order_relaxed)),
          m_record(std::make_shared<FiberRecord>(m_id)),
          m_function(std::move(function)),
          m_stack_size(normalize_stack_size(stack_size)) {
#if defined(GO2CPP_FIBER_TSAN)
        m_tsan_fiber = __tsan_create_fiber(0);
#endif
    }

    void abandon_suspended(const char* message) noexcept {
        // 挂起的 fcontext 没有正在执行的指令，释放其栈是安全的；但栈上
        // 的 C++ 局部变量无法再展开。因此只能显式标记 Failed，并清理
        // FiberLocal 值，不能伪造 Completed。这个路径只允许在已经取得
        // m_resume_claim 后执行，避免与另一个 resume 并发访问上下文。
        try {
            const auto error = std::make_exception_ptr(std::runtime_error(message));
            std::lock_guard<std::mutex> lock(m_failure_mutex);
            if (!m_failure) {
                m_failure = error;
            }
        } catch (...) {
            // 诊断对象分配失败时仍必须完成上下文隔离。
        }
        m_function = {};
        m_caller = nullptr;
        m_context = nullptr;
        m_record->reason.store(SuspendReason::None, std::memory_order_release);
        m_scheduler_propagate.store(false, std::memory_order_release);
        m_record->state.store(FiberState::Failed, std::memory_order_release);
        fiber_local::detail::Cleanup(m_owner);
    }

    ~Impl() noexcept {
        // 不能在 Fiber 自身栈上销毁自身；此约束保留为明确的 API 前置条件，
        // 否则没有任何安全位置可以释放当前正在使用的栈。
        if (s_current_fiber == m_owner) {
            std::terminate();
        }

        // 先取得执行 claim，再观察终态。entry() 会先发布 Completed/Failed，
        // 后由 resume() 释放 claim；若只检查 state，析构可能在这个窗口释放
        // fcontext/保护栈，导致 resume_locked() 继续访问已释放对象。
        bool expected_claim = false;
        while (!m_resume_claim.compare_exchange_weak(
            expected_claim, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
            expected_claim = false;
            std::this_thread::yield();
        }

        // 先发布墓碑，再请求协作式收尾。父链记录不会悬空；析构线程
        // 只有在能够回到原父 Fiber 的情况下才允许恢复挂起栈。
        m_record->alive.store(false, std::memory_order_release);
        m_record->cancellation_requested.store(true,
                                                std::memory_order_release);

        // Ready Fiber 尚未进入用户栈，没有需要展开的局部变量；可以在
        // 任意调用方安全跳过主体。
        if (m_record->state.load(std::memory_order_acquire) ==
            FiberState::Ready) {
            m_function = {};
            m_record->reason.store(SuspendReason::None,
                                   std::memory_order_release);
            m_record->state.store(FiberState::Completed,
                                  std::memory_order_release);
        }

        while (true) {
            const FiberState state =
                m_record->state.load(std::memory_order_acquire);
            if (state == FiberState::Completed || state == FiberState::Failed) {
                break;
            }
            if (state == FiberState::Running) {
                // 取得 claim 后不应再观察到 Running；该分支只保护未来
                // 后端扩展，不能释放仍在执行的栈。
                std::terminate();
            }
            if (state != FiberState::Suspended) {
                abandon_suspended("Fiber destroyed in an invalid state");
                break;
            }

            const bool resumed = resume_locked(true);
            if (!resumed) {
                // 当前析构调用方不是固定父 Fiber，无法安全跳转到挂起栈。
                // 记录失败并释放上下文，而不是让进程被 terminate。
                abandon_suspended("Fiber parent is not resumable during destruction");
                break;
            }
        }

        const auto final_state = m_record->state.load(std::memory_order_acquire);
        if (final_state == FiberState::Ready ||
            final_state == FiberState::Suspended ||
            final_state == FiberState::Running) {
            // 取得独占 claim 后理论上不可达；保守地 fail-fast，避免 UAF。
            std::terminate();
        }
#if defined(GO2CPP_FIBER_TSAN)
        __tsan_destroy_fiber(m_tsan_fiber);
#endif
        // m_resume_claim 故意保持为 true，Impl 即将释放，任何后续 resume
        // 都违反 owner 生命周期契约；在本析构期间不会再有并发上下文访问。
    }

    static void entry(
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
        detail::Transfer transfer
#else
        boost::context::detail::transfer_t transfer
#endif
    ) noexcept {
        auto* const self = static_cast<Impl*>(const_cast<void*>(transfer.data));
        self->finish_switch_to_fiber();
        self->m_caller = transfer.context;
        store_errno(self->m_saved_errno);

        FiberState terminal_state = FiberState::Completed;
        try {
            if (!self->m_record->cancellation_requested.load(std::memory_order_acquire) &&
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
        // Run their destructors after the body has unwound and before handing
        // control back to the caller.
        fiber_local::detail::Cleanup(self->m_owner);
        self->m_record->reason.store(SuspendReason::None, std::memory_order_release);
        self->m_record->state.store(terminal_state, std::memory_order_release);
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
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
        detail::JumpContext(self->m_caller, self);
#else
        boost::context::detail::jump_fcontext(self->m_caller, self);
#endif
        std::terminate();
    }

    bool resume() noexcept {
        bool expected = false;
        if (!m_resume_claim.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }
        const bool result = resume_locked(true);
        m_resume_claim.store(false, std::memory_order_release);
        return result;
    }

    bool resume_from_scheduler() noexcept {
        bool expected = false;
        if (!m_resume_claim.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }
        if (!m_scheduler_parent_bound) {
            if (!bind_caller(s_current_fiber)) {
                m_resume_claim.store(false, std::memory_order_release);
                return false;
            }
            m_scheduler_parent_bound = true;
        }
        // 调度器已经通过 Task 的 execution claim 串行化恢复，并在迁移
        // 到新 M 时显式发布执行绑定；因此恢复热路径不再重复更新
        // FiberRecord 的 last_thread 元数据。
        const bool result = resume_locked(false, true);
        m_resume_claim.store(false, std::memory_order_release);
        return result;
    }

    // 首次进入时固定直接父 Fiber；之后只能由同一个父级恢复。
    // 父链的可观测信息放在共享记录中，真正恢复仍验证父对象存活。
    // 先复制父记录，再写入本 Fiber，避免同时锁两个 Fiber 的记录而形成
    // 反向锁序；这样错误的并发 resume 会返回失败而不是制造死锁。
    bool bind_caller(Fiber* caller) noexcept {
        if (caller == m_owner) {
            return false;
        }

        std::shared_ptr<FiberRecord> parent_record;
        {
            std::lock_guard<std::mutex> lock(m_metadata_mutex);
            if (!m_parent_bound) {
                m_parent = caller;
                m_parent_bound = true;
            } else if (m_parent != caller) {
                return false;
            }
            parent_record = caller == nullptr ? nullptr : caller->m_impl->m_record;
        }

        std::size_t depth = 1U;
        std::uint64_t parent_id = 0;
        FiberExecutionBinding execution{};
        if (parent_record != nullptr) {
            std::lock_guard<std::mutex> parent_lock(parent_record->mutex);
            if (!parent_record->alive.load(std::memory_order_acquire)) {
                return false;
            }
            depth = parent_record->depth + 1U;
            parent_id = parent_record->id;
            execution = parent_record->execution;
        } else {
            std::lock_guard<std::mutex> record_lock(m_record->mutex);
            execution = m_record->execution;
            if (!execution.managed) {
                execution = s_main_execution;
            }
        }

        std::lock_guard<std::mutex> lock(m_metadata_mutex);
        if (m_parent != caller) {
            return false;
        }
        std::lock_guard<std::mutex> record_lock(m_record->mutex);
        m_record->parent = std::move(parent_record);
        m_record->depth = depth;
        m_record->active_parent_id = parent_id;
        m_record->execution = execution;
        m_record->last_thread = std::this_thread::get_id();
        return true;
    }

    bool resume_locked(bool validate_caller, bool scheduler_fast = false) noexcept {
        if (validate_caller && !bind_caller(s_current_fiber)) {
            return false;
        }
        // Scheduler 级挂起可能在本函数内部沿父链循环多次。每一轮跳转
        // 都暂时把 TLS 当前 Fiber 设为本对象，但完成后必须恢复最初的
        // 调用者；否则父函数在同一栈帧里创建下一个子 Fiber 时，会把
        // 已完成的前一个子 Fiber 错当成新子 Fiber 的父级。
        Fiber* const original_caller = s_current_fiber;
        const FiberTlsRestore tls_restore(original_caller);
        for (;;) {
            const FiberState current = m_record->state.load(std::memory_order_acquire);
            if (current != FiberState::Ready && current != FiberState::Suspended) {
                return false;
            }
            if (current == FiberState::Ready && m_context == nullptr) {
                if (!initialize_context()) {
                    return false;
                }
            }
            if (m_context == nullptr) {
                return false;
            }
            m_record->state.store(FiberState::Running, std::memory_order_release);
            m_record->reason.store(SuspendReason::None, std::memory_order_release);
            m_scheduler_propagate.store(false, std::memory_order_release);

            const int caller_errno = load_errno();
            Fiber* const previous_fiber = s_current_fiber;
            s_current_fiber = m_owner;
            if (!scheduler_fast) {
                const auto thread_hash = std::hash<std::thread::id>{}(
                    std::this_thread::get_id());
                if (m_last_resume_thread_hash.load(std::memory_order_relaxed) !=
                    thread_hash) {
                    std::lock_guard<std::mutex> lock(m_metadata_mutex);
                    m_record->last_thread = std::this_thread::get_id();
                    m_last_resume_thread_hash.store(thread_hash,
                                                    std::memory_order_relaxed);
                }
            }
            store_errno(m_saved_errno);

            #if defined(GO2CPP_USE_NATIVE_CONTEXT)
            detail::Transfer transfer{};
            #else
            boost::context::detail::transfer_t transfer{};
            #endif
            {
#if defined(GO2CPP_FIBER_ASAN)
                void* caller_fake_stack = nullptr;
                __sanitizer_start_switch_fiber(&caller_fake_stack,
                                              m_stack->bottom(),
                                              m_stack->usable_size());
#endif
#if defined(GO2CPP_FIBER_TSAN)
                m_tsan_caller = __tsan_get_current_fiber();
                __tsan_switch_to_fiber(m_tsan_fiber, 0);
#endif
                #if defined(GO2CPP_USE_NATIVE_CONTEXT)
                transfer = detail::JumpContext(m_context, this);
                #else
                transfer = boost::context::detail::jump_fcontext(m_context, this);
                #endif
#if defined(GO2CPP_FIBER_ASAN)
                __sanitizer_finish_switch_fiber(caller_fake_stack, nullptr,
                                                nullptr);
#endif
                const auto next = m_record->state.load(std::memory_order_acquire);
                m_context = next == FiberState::Completed ||
                                    next == FiberState::Failed
                                ? nullptr
                                : transfer.context;
                if (m_context == nullptr) {
                    // entry() has unwound all user frames before returning to
                    // the caller. The protected stack can now be returned to
                    // the per-thread/global cache immediately, avoiding one
                    // mapping per queued Fiber in burst workloads.
                    m_stack.reset();
                }
            }

            // jump 返回有两种语义：如果当前 Fiber 仍在 Running，说明
            // 是它在自己的栈上恢复了嵌套子 Fiber，执行流仍属于当前
            // owner；只有挂起/完成/失败并回到 caller 时，才恢复旧 TLS。
            // 无条件恢复 previous_fiber 会让父 Fiber 后续创建的子 Fiber
            // 错绑到根上下文，破坏父链并可能触发 fail-fast。
            const FiberState returned_state =
                m_record->state.load(std::memory_order_acquire);
            s_current_fiber = returned_state == FiberState::Running
                                  ? m_owner
                                  : previous_fiber;
            store_errno(caller_errno);

            // 调度器挂起沿直接父链传播；普通 Suspend 不进入这个分支，
            // 因而 fiber2 的普通 go_back 仍然只回到 fiber1。
            if (m_record->state.load(std::memory_order_acquire) ==
                    FiberState::Suspended &&
                m_scheduler_propagate.load(std::memory_order_acquire) &&
                m_parent != nullptr) {
                const SuspendReason reason =
                    m_record->reason.load(std::memory_order_acquire);
                const auto parent_record = m_record->parent;
                if (!parent_record ||
                    !parent_record->alive.load(std::memory_order_acquire) ||
                    m_parent == nullptr ||
                    !m_parent->m_impl->suspend(reason, true)) {
                    // 不能把仍挂起的子栈当作完成任务交给调度器。父级
                    // 已失效时将当前 Fiber 标记失败并清理其局部值，让
                    // Scheduler 走普通失败收尾；不再从这里 terminate。
                    abandon_suspended("nested Fiber parent is not resumable");
                    s_current_fiber = original_caller;
                    return false;
                }
                s_current_fiber = m_owner;
                continue;
            }
            s_current_fiber = original_caller;
            return true;
        }
    }

    bool suspend(SuspendReason reason, bool propagate) noexcept {
        if (reason == SuspendReason::None ||
            m_record->state.load(std::memory_order_acquire) != FiberState::Running) {
            return false;
        }
        m_saved_errno = load_errno();
        m_record->reason.store(reason, std::memory_order_release);
        m_scheduler_propagate.store(propagate, std::memory_order_release);
        m_record->state.store(FiberState::Suspended, std::memory_order_release);

#if defined(GO2CPP_FIBER_ASAN)
        __sanitizer_start_switch_fiber(&m_asan_fake_stack, m_asan_caller_bottom,
                                      m_asan_caller_size);
#endif
#if defined(GO2CPP_FIBER_TSAN)
        __tsan_switch_to_fiber(m_tsan_caller, 0);
#endif
            #if defined(GO2CPP_USE_NATIVE_CONTEXT)
            const detail::Transfer transfer = detail::JumpContext(m_caller, this);
            #else
            const boost::context::detail::transfer_t transfer =
                boost::context::detail::jump_fcontext(m_caller, this);
            #endif
        finish_switch_to_fiber();
        m_caller = transfer.context;
        store_errno(m_saved_errno);
        m_record->reason.store(SuspendReason::None, std::memory_order_release);
        m_scheduler_propagate.store(false, std::memory_order_release);
        m_record->state.store(FiberState::Running, std::memory_order_release);
        s_current_fiber = m_owner;
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

    bool initialize_context() noexcept {
        try {
            m_stack = std::make_unique<FiberStack>(m_stack_size);
            #if defined(GO2CPP_USE_NATIVE_CONTEXT)
            m_context = detail::MakeContext(const_cast<void*>(m_stack->bottom()),
                                            m_stack->usable_size(), &Impl::entry);
            #else
            m_context = boost::context::detail::make_fcontext(
                m_stack->stack_pointer(), m_stack->usable_size(), &Impl::entry);
            #endif
            return true;
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_failure_mutex);
            m_failure = std::current_exception();
            m_record->state.store(FiberState::Failed, std::memory_order_release);
            return false;
        }
    }

    Fiber* m_owner;
    const std::uint64_t m_id;
    std::shared_ptr<FiberRecord> m_record;
    Function m_function;
    const std::size_t m_stack_size;
    std::unique_ptr<FiberStack> m_stack;
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
    detail::Context m_context{nullptr};
    detail::Context m_caller{nullptr};
#else
    boost::context::detail::fcontext_t m_context{nullptr};
    boost::context::detail::fcontext_t m_caller{nullptr};
#endif
    std::atomic<bool> m_scheduler_propagate{false};
    bool m_scheduler_parent_bound{false};
    std::atomic<bool> m_resume_claim{false};
    mutable std::mutex m_metadata_mutex;
    std::atomic<std::size_t> m_last_resume_thread_hash{0};
    Fiber* m_parent{nullptr};
    bool m_parent_bound{false};
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

bool Fiber::resume_from_scheduler() noexcept {
    return m_impl->resume_from_scheduler();
}

FiberResumeResult Fiber::resume_result() noexcept {
    FiberResumeResult result;
    result.accepted = resume();
    result.state = state();
    result.failure = failure();
    try {
        result.context_snapshot = context_snapshot();
    } catch (...) {
        // 快照只是诊断信息，不能因为 OOM 让 noexcept API terminate；
        // state/failure 仍然有效，调用方可稍后再次请求快照。
        result.context_snapshot.clear();
    }
    return result;
}

bool Fiber::Suspend(SuspendReason reason) noexcept {
    Fiber* const current = Current();
    return current != nullptr && current->m_impl->suspend(reason, false);
}

bool Fiber::SuspendForScheduler(SuspendReason reason) noexcept {
    Fiber* const current = Current();
    return current != nullptr && current->m_impl->suspend(reason, true);
}


GO2CPP_FIBER_NOINLINE Fiber* Fiber::Current() noexcept { return s_current_fiber; }

bool Fiber::CancellationRequested() noexcept {
    Fiber* current = Current();
    if (current == nullptr) {
        return false;
    }
    std::shared_ptr<FiberRecord> record = current->m_impl->m_record;
    while (record != nullptr) {
        if (record->cancellation_requested.load(std::memory_order_acquire)) {
            return true;
        }
        std::lock_guard<std::mutex> lock(record->mutex);
        record = record->parent;
    }
    return false;
}

FiberContextFrame Fiber::CurrentContext() {
    if (Fiber* const current = Current()) {
        return current->debug_info();
    }
    return main_context_frame(s_main_execution);
}

FiberContextSnapshot Fiber::CurrentContextSnapshot() {
    if (Fiber* const current = Current()) {
        return current->context_snapshot();
    }
    return {main_context_frame(s_main_execution)};
}

void Fiber::BindCurrentExecution(FiberExecutionBinding binding) noexcept {
    s_main_execution = binding;
}

void Fiber::RequestCancellation() noexcept {
    if (m_impl) {
        m_impl->m_record->cancellation_requested.store(true, std::memory_order_release);
    }
}

FiberState Fiber::state() const noexcept {
    return m_impl->m_record->state.load(std::memory_order_acquire);
}

SuspendReason Fiber::suspend_reason() const noexcept {
    return m_impl->m_record->reason.load(std::memory_order_acquire);
}

std::exception_ptr Fiber::failure() const {
    std::lock_guard<std::mutex> lock(m_impl->m_failure_mutex);
    return m_impl->m_failure;
}

std::size_t Fiber::stack_size() const noexcept { return m_impl->m_stack_size; }

std::uint64_t Fiber::id() const noexcept { return m_impl->m_id; }

FiberContextFrame Fiber::debug_info() const {
    FiberContextFrame frame;
    const auto record = m_impl->m_record;
    std::lock_guard<std::mutex> lock(record->mutex);
    frame.id = record->id;
    frame.parent_id = record->parent == nullptr ? 0 : record->parent->id;
    frame.active_parent_id = record->active_parent_id;
    frame.depth = record->depth;
    frame.alive = record->alive.load(std::memory_order_acquire);
    frame.active = Current() == this;
    frame.cancellation_requested =
        record->cancellation_requested.load(std::memory_order_acquire);
    frame.state = record->state.load(std::memory_order_acquire);
    frame.suspend_reason = record->reason.load(std::memory_order_acquire);
    frame.last_thread = record->last_thread;
    frame.execution = record->execution;
    return frame;
}

FiberContextSnapshot Fiber::context_snapshot() const {
    FiberContextSnapshot result;
    std::shared_ptr<FiberRecord> cursor = m_impl->m_record;
    FiberExecutionBinding root_binding{};
    const auto current_id = m_impl->m_record->id;
    while (cursor != nullptr) {
        FiberContextFrame frame;
        std::shared_ptr<FiberRecord> parent;
        {
            std::lock_guard<std::mutex> lock(cursor->mutex);
            frame.id = cursor->id;
            frame.parent_id = cursor->parent == nullptr ? 0 : cursor->parent->id;
            frame.active_parent_id = cursor->active_parent_id;
            frame.depth = cursor->depth;
            frame.alive = cursor->alive.load(std::memory_order_acquire);
            frame.active = cursor->id == current_id && Current() == this;
            frame.cancellation_requested =
                cursor->cancellation_requested.load(std::memory_order_acquire);
            frame.state = cursor->state.load(std::memory_order_acquire);
            frame.suspend_reason = cursor->reason.load(std::memory_order_acquire);
            frame.last_thread = cursor->last_thread;
            frame.execution = cursor->execution;
            if (cursor->parent == nullptr) {
                root_binding = cursor->execution;
            }
            parent = cursor->parent;
        }
        result.push_back(std::move(frame));
        cursor = std::move(parent);
    }
    std::reverse(result.begin(), result.end());
    // 这是从 main_fiber 进入目标 Fiber 后的调用链快照；main_fiber
    // 只是根帧，不应与链中当前/目标 Fiber 同时标记 active。没有
    // Fiber 时 CurrentContextSnapshot() 仍单独返回 active 的 main_fiber。
    auto main_frame = main_context_frame(root_binding);
    main_frame.active = false;
    result.insert(result.begin(), std::move(main_frame));
    return result;
}

FiberExecutionBinding Fiber::execution_binding() const noexcept {
    std::lock_guard<std::mutex> lock(m_impl->m_record->mutex);
    return m_impl->m_record->execution;
}

void Fiber::bind_execution(FiberExecutionBinding binding) noexcept {
    std::lock_guard<std::mutex> lock(m_impl->m_record->mutex);
    m_impl->m_record->execution = binding;
    m_impl->m_record->last_thread = std::this_thread::get_id();
}

std::size_t Fiber::nesting_depth() const noexcept {
    std::lock_guard<std::mutex> lock(m_impl->m_record->mutex);
    return m_impl->m_record->depth;
}

}  // namespace go2cpp
