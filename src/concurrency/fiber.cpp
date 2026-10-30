/**
 * @file fiber.cpp
 * @brief Fiber 生命周期、栈管理与 FiberBin 池化。
 *
 * ## 这个文件实现什么
 *
 * - Fiber 的创建/启动/挂起/恢复/销毁；
 * - 独立栈的分配与 `FiberBin` 池化复用（避免反复 mmap/munmap）；
 * - `FiberLocal` 的底层存储（`ThreadValues()` 特意定义在本文件，保证全进程
 *   只有一份 TLS 映射 —— 函数局部 static 在每个 DSO 里会各有一份）；
 * - Fiber trampoline：Fiber 入口的包装层，负责在 Fiber 结束时清理局部存储。
 *
 * ## 与 coost 的取舍差异
 *
 * 本项目用**每 Fiber 独立栈 + 池化**；coost 用**每调度器共享栈**。
 * 独立栈切换更快（不用拷栈），代价是内存占用更大；池化把建栈开销摊掉。
 * 这两个方向没有对错，取决于"并发 Fiber 数量"与"切换频率"的比值。
 *
 * ## 换栈在哪
 *
 * 本文件只调用 `context_backend` 的两个原语，具体寄存器操作在
 * `context_x64.S`。阅读时可以把"换栈"当成一个原子动作，不必跟进汇编。
 */
#include "go2cpp/fiber.hpp"

#include "go2cpp/debug/debug_resources.hpp"

#include "go2cpp/detail/context_backend.hpp"
#include "go2cpp/fiber_local.hpp"

// 只有 Linux x86_64 + 项目自带的汇编上下文后端才启用按需增长。
// 其它平台/后端保留固定栈语义，不会安装 SIGSEGV handler。
#if defined(GO2CPP_USE_NATIVE_CONTEXT) && defined(__linux__) && defined(__x86_64__)
#define GO2CPP_FIBER_GROWABLE_STACK 1
#include <ucontext.h>
#endif

#if !defined(GO2CPP_USE_NATIVE_CONTEXT)
#include <boost/context/detail/fcontext.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>
#include <boost/context/stack_traits.hpp>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define GO2CPP_FIBER_HAVE_VALGRIND 1
#endif
#endif
#endif

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <list>
#include <stdexcept>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
// 跨进程迁移实现复用 tools/migration 的 v3 快照格式与符号哈希。
#include "migration/format.hpp"
#include "migration/symbol_table.hpp"
#include <elf.h>
#include <link.h>
#include <climits>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#endif

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

// TLS 模型：共享库默认的 general-dynamic 模型会让每次 thread_local 访问
// 都调用 __tls_get_addr（实测每次约 5–8 ns）。一次 Fiber 切换往返要访问
// 「当前 Fiber / 当前可增长栈」等 TLS 变量 6–10 次，是本路径最大的固定
// 开销之一。本库由 DT_NEEDED 在进程启动时加载，initial-exec 是安全的；
// 若未来需要 dlopen 本库，必须回退 general-dynamic（与 channel.cpp 中
// Select 游标的取舍一致）。
#if defined(__GNUC__)
#define GO2CPP_FIBER_TLS __attribute__((tls_model("initial-exec")))
#else
#define GO2CPP_FIBER_TLS
#endif

namespace go2cpp {
namespace {

#if defined(GO2CPP_USE_NATIVE_CONTEXT)
// 当前线程正在运行的 Fiber 栈。只有 growable 栈会写入非空值；SIGSEGV
// handler 据此判断故障地址是否落在某个可变长栈的未提交区。
struct FiberStack;
GO2CPP_FIBER_TLS thread_local FiberStack* t_current_growable_stack = nullptr;
#endif

GO2CPP_FIBER_TLS thread_local Fiber* s_current_fiber = nullptr;
// 没有运行真实 Fiber 时，每个 OS 线程仍有一个不可 resume 的根上下文。
// 调度器在 worker 进入/离开 G 时更新这份元数据，Fiber 首次进入时继承它。
GO2CPP_FIBER_TLS thread_local FiberExecutionBinding s_main_execution{};

// resume_locked 可能沿父链循环多次，任意失败/取消返回都必须恢复进入
// resume 前的直接调用者，避免 TLS 残留成已完成或错误的 Fiber。
struct FiberTlsRestore final {
    explicit FiberTlsRestore(Fiber* previous) noexcept : m_previous(previous) {}
    ~FiberTlsRestore() noexcept { s_current_fiber = m_previous; }

    FiberTlsRestore(const FiberTlsRestore&) = delete;
    FiberTlsRestore& operator=(const FiberTlsRestore&) = delete;

    Fiber* m_previous;
};

#if defined(GO2CPP_USE_NATIVE_CONTEXT)
// resume_locked 可能沿父链循环多次，每次 jump 前把 TLS 指向目标栈，返回
// 后必须恢复到进入 resume 时的栈；否则 handler 会拿着已释放/错误的栈边界
// 判断故障。析构函数保证所有 return 路径都恢复。
struct GrowableStackTlsRestore final {
    explicit GrowableStackTlsRestore(FiberStack* previous) noexcept
        : m_previous(previous) {}
    ~GrowableStackTlsRestore() noexcept {
        t_current_growable_stack = m_previous;
    }

    GrowableStackTlsRestore(const GrowableStackTlsRestore&) = delete;
    GrowableStackTlsRestore& operator=(const GrowableStackTlsRestore&) = delete;

    FiberStack* m_previous;
};
#endif

std::atomic<std::uint64_t> s_next_fiber_id{1};

// 析构等待 resume 权超时的累计次数（FIBER-02 诊断 API 的数据源）。
// 正常路径第一次 CAS 就成功，不会触碰该计数。
std::atomic<std::uint64_t> s_destroy_claim_timeouts{0};

// 受保护栈的映射/解除映射成本远高于一次 Fiber 上下文切换。栈不能在
// Fiber 仍可恢复时复用，因此只在 Fiber 完成、且其 fcontext 已经失效后
// 放回缓存。先使用当前 M 的 TLS 缓存，跨 M 销毁时再进入有界全局缓存。
struct CachedStack {
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
    std::size_t usable{0};
    // 0 表示固定栈布局（usable + 1 页 guard）；非 0 表示预留虚拟地址大小。
    // 缓存键是 (usable, reserve)：两种布局的提交方式不同，不能混用。
    std::size_t reserve{0};
    void* mapping{nullptr};
    std::size_t mapping_size{0};
#else
    std::size_t size{0};
    boost::context::stack_context context{};
#endif
};

/**
 * @brief 受保护栈的缓存（TLS 一份 + 进程级一份兜底）。
 *
 * ## 定位
 *
 * `FiberStack` 的工厂与回收站。每次 `go()` 都要 mmap 一段带 guard page 的栈
 * （`mprotect` 的成本远高于一次上下文切换 —— 实测去掉两次冗余调用后
 * `go()` 17.6µs → 14.5µs），所以 Fiber 结束后把映射放回缓存，下一次直接复用。
 *
 * ## 两层缓存
 *
 * | 缓存 | 位置 | 无锁？ | 用途 |
 * |---|---|---|---|
 * | `s_thread_stack_pool` | thread_local | 是 | 同 M 上的 Fiber 复用（绝大多数） |
 * | `s_stack_pool` | 全局 + `s_stack_pool_mutex` | 否 | 跨 M 销毁时的兜底，有界 |
 *
 * ## 为什么必须等 fcontext 失效
 *
 * 栈在 Fiber **仍可恢复时绝不能复用** —— 会把旧栈帧的数据暴露给新 Fiber。
 * 只有「Fiber 已完成、`reset_for_scheduler` 已清空 m_context/m_stack」的栈
 * 才进缓存。
 *
 * ## 析构
 *
 * 把缓存里所有映射 `munmap`（或交还 boost 的 allocator），不泄漏。
 */
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
GO2CPP_FIBER_TLS thread_local StackCache s_thread_stack_pool;

// 仅由 Scheduler 内部使用的 FiberBin。一个 M 只访问自己的 TLS 池，
// 因而不会把仍可能被其他 M resume 的 Fiber 放进共享容器。完成 Fiber
// 的上下文已经失效，放回这里的对象只会在下一次 AcquireForScheduler
// 时重新绑定任务；普通用户创建的 Fiber 不经过此路径。
struct FiberBin {
    std::vector<std::unique_ptr<Fiber>> entries;
};

GO2CPP_FIBER_TLS thread_local FiberBin s_thread_fiber_bin;
constexpr std::size_t kDefaultFiberBinCapacity = 32U;
// 防止误配把每个 M 变成无界对象保留池。FiberBin 只应缓存短期突发，
// 更大的容量不会带来线性收益，且会长期占用 Fiber/闭包对象内存。
constexpr std::size_t kMaxFiberBinCapacity = 4096U;

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

// errno 是每线程 TLS；`errno` 宏展开为 `(*__errno_location())`。
//
// 旧实现每次访问都调用一次 noinline 的 load/store：noinline 是为了阻止
// 编译器把 `__errno_location()` 的结果跨上下文切换缓存到寄存器（Fiber
// 迁移到别的线程后会写错线程的 errno）。但一次切换往返要访问 errno 3 次，
// 每次 3–4ns 的调用开销很可观。
//
// 改为「每线程缓存一次地址」：TLS 指针在**当前线程**上永远指向当前线程的
// errno 槽，Fiber 迁移后读到的自然是新线程的 TLS 缓存，语义与每次重新调用
// __errno_location 完全一致；此后每次访问只需一次 TLS 指针读 + 解引用。
GO2CPP_FIBER_TLS thread_local int* t_errno_slot = nullptr;

inline int* errno_slot() noexcept {
    int* slot = t_errno_slot;
    if (slot == nullptr) {
        slot = &errno;
        t_errno_slot = slot;
    }
    return slot;
}

inline int load_errno() noexcept { return *errno_slot(); }

inline int swap_errno(int value) noexcept {
    int* const slot = errno_slot();
    const int previous = *slot;
    *slot = value;
    return previous;
}

inline void store_errno(int value) noexcept { *errno_slot() = value; }

#if defined(GO2CPP_USE_NATIVE_CONTEXT)
// 查询系统页大小。两个细节：
// 1. `sysconf` 失败时可能**覆盖 errno**，所以进入前保存、返回前恢复，
//    否则会把无关的 errno 泄漏给调用方；
// 2. 结果要检查能否放进 `size_t`（32 位平台上 long 可能更宽）。
std::size_t native_page_size() {
    const int saved_errno = errno;
    const long raw_page_size = ::sysconf(_SC_PAGESIZE);
    if (raw_page_size <= 0) {
        const int error = errno == 0 ? EINVAL : errno;
        errno = saved_errno;
        throw std::system_error(error, std::generic_category(),
                                 "query fiber page size");
    }
    errno = saved_errno;
    const auto page_size = static_cast<unsigned long long>(raw_page_size);
    if (page_size > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("fiber page size is not representable");
    }
    return static_cast<std::size_t>(page_size);
}
#endif

#if defined(GO2CPP_FIBER_GROWABLE_STACK)
// 一次 fault 至少提交 64KiB（16 页），避免深递归时每页一次信号。
constexpr std::size_t kGrowableStackGrowthGranularity = 64U * 1024U;
// 故障地址与 rsp 的最大允许距离。函数序言会先 sub rsp 再写局部变量，
// 因此 rsp 可能已经越过 committed；大帧（大数组/内置 alloca）也可能让
// 两者相距数十 KiB。超过窗口按野指针/超大帧处理，直接 fatal。
constexpr std::size_t kGrowableStackFaultWindow = 256U * 1024U;
constexpr std::size_t kGrowableStackAltStackSize = 256U * 1024U;

// handler 内不允许拿锁/分配；这里依赖 x86_64 上指针与 64 位计数器的
// 无锁原子实现（C++17 保证 is_always_lock_free 可用于编译期断言）。
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free,
              "growable stack handler requires lock-free pointer atomics");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "growable stack growth counter must be lock-free");

std::atomic<std::uint64_t> s_growable_stack_growth_count{0};
std::atomic<int> s_growable_handler_state{0};  // 0 未安装 1 已安装 2 失败
std::mutex s_growable_handler_mutex;
GO2CPP_FIBER_TLS thread_local bool t_growable_altstack_ready = false;
GO2CPP_FIBER_TLS thread_local bool t_growable_altstack_attempted = false;
GO2CPP_FIBER_TLS thread_local volatile sig_atomic_t t_growable_in_handler = 0;
// 每线程一次性结果缓存：0=未检查，1=可用，2=不可用。resume 热路径只需
// 一次 TLS 字节读，不必每轮都调 ensure_growable_thread_altstack()（两次
// TLS 读）与 install_growable_stack_handler()（一次 acquire 原子读）。
GO2CPP_FIBER_TLS thread_local std::uint8_t t_growable_support_state = 0;

[[noreturn]] void growable_stack_fatal(int sig) noexcept {
    // 恢复内核默认动作、解除屏蔽后重新投递，保留 core dump 语义。
    // sigaction/sigprocmask/raise/_exit 都是 async-signal-safe 的。
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)::sigaction(sig, &action, nullptr);
    sigset_t empty;
    sigemptyset(&empty);
    (void)::sigprocmask(SIG_SETMASK, &empty, nullptr);
    (void)::raise(sig);
    ::_exit(128 + sig);
}

// 定义在 FiberStack 之后：handler 需要完整的栈边界访问器。
void growable_stack_segv_handler(int sig, siginfo_t* info,
                                 void* raw_context) noexcept;

bool growable_stack_env_enabled() noexcept {
    const char* value = std::getenv("GO2CPP_GROWABLE_STACK");
    if (value == nullptr || *value == '\0') {
        return true;
    }
    return value[0] != '0' && std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "off") != 0;
}

bool ensure_growable_thread_altstack() noexcept {
    if (t_growable_altstack_ready) {
        return true;
    }
    if (t_growable_altstack_attempted) {
        return false;
    }
    t_growable_altstack_attempted = true;
    // 必须用 mmap 而不是 malloc：栈溢出时堆元数据可能已被踩坏，备用栈
    // 必须是独立的匿名映射。线程退出时不主动 munmap，避免信号处理期间
    // 释放正在使用的备用栈；进程退出统一回收。
    void* memory = ::mmap(nullptr, kGrowableStackAltStackSize,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        return false;
    }
    stack_t alt{};
    alt.ss_sp = memory;
    alt.ss_size = kGrowableStackAltStackSize;
    alt.ss_flags = 0;
    if (::sigaltstack(&alt, nullptr) != 0) {
        ::munmap(memory, kGrowableStackAltStackSize);
        return false;
    }
    t_growable_altstack_ready = true;
    return true;
}

bool install_growable_stack_handler() noexcept {
    int state = s_growable_handler_state.load(std::memory_order_acquire);
    if (state == 1) {
        return true;
    }
    if (state == 2) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(s_growable_handler_mutex);
        state = s_growable_handler_state.load(std::memory_order_relaxed);
        if (state == 1) {
            return true;
        }
        if (state == 2) {
            return false;
        }
        struct sigaction action {};
        action.sa_sigaction = &growable_stack_segv_handler;
        sigemptyset(&action.sa_mask);
        // 不加 SA_NODEFER：handler 执行期间屏蔽 SIGSEGV，配合 TLS 重入标志
        // 把「handler 内再次 fault」变成明确的 fatal，而不是无限递归。
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        if (::sigaction(SIGSEGV, &action, nullptr) != 0) {
            s_growable_handler_state.store(2, std::memory_order_release);
            return false;
        }
        s_growable_handler_state.store(1, std::memory_order_release);
        return true;
    } catch (...) {
        s_growable_handler_state.store(2, std::memory_order_release);
        return false;
    }
}

bool ensure_growable_thread_support() noexcept {
    // 每线程结果只算一次：失败也要缓存，避免每一轮 resume 都重试
    // mmap/sigaction（两者都比一次切换贵几个数量级）。
    const std::uint8_t cached = t_growable_support_state;
    if (cached != 0) {
        return cached == 1;
    }
    // 先装备用栈，再装进程级 handler：handler 一装好，任何线程上的
    // SIGSEGV 都会进入它，因此当前线程必须已经能安全接收信号。
    const bool supported = ensure_growable_thread_altstack() &&
                           install_growable_stack_handler();
    t_growable_support_state = supported ? 1 : 2;
    return supported;
}
#endif

std::size_t checked_stack_mapping_size(std::size_t usable_size,
                                       std::size_t page_size) {
    if (page_size == 0 || usable_size >
                             std::numeric_limits<std::size_t>::max() -
                                 page_size) {
        throw std::length_error("fiber stack mapping size overflow");
    }
    return usable_size + page_size;
}

#if defined(GO2CPP_USE_NATIVE_CONTEXT)
// 把栈大小向上对齐到页边界。`mmap`/`mprotect` 要求页对齐，
// 不对齐会让 guard page 落不到正确位置。
// 加法前检查溢出 —— 接近 `SIZE_MAX` 的请求直接抛 length_error 而不是回绕成小值。
std::size_t align_stack_size(std::size_t requested, std::size_t page_size) {
    const auto remainder = requested % page_size;
    if (remainder == 0) {
        return requested;
    }
    const auto padding = page_size - remainder;
    if (requested > std::numeric_limits<std::size_t>::max() - padding) {
        throw std::length_error("fiber stack size alignment overflow");
    }
    return requested + padding;
}
#endif

// 归一化初始提交量：0 用 Fiber::DefaultStackSize()（直接构造 Fiber 的
// 默认值 128KiB）；页对齐；下限 1 页。注意调度器配置的默认请求是 2048，
// 页对齐后是 4096（1 页），符合 Go 的 2KB 初始栈在 4KB 页上的归一化规则：
//   initial_usable = max(page_align_up(fiber_stack_size), 1 * page)
// 固定栈回退仍需要额外的 guard page，所以上限检查保留 checked_mapping_size。
std::size_t normalize_stack_size(std::size_t requested) {
    if (requested == 0) {
        requested = Fiber::DefaultStackSize();
    }
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
    const auto page_size = native_page_size();
    const auto maximum_usable =
        std::numeric_limits<std::size_t>::max() - page_size;
    const auto at_least_minimum = std::max(requested, page_size);
    if (at_least_minimum > maximum_usable) {
        throw std::length_error("fiber stack size is too large");
    }
    const auto aligned = align_stack_size(at_least_minimum, page_size);
    if (aligned > maximum_usable) {
        throw std::length_error("fiber aligned stack size is too large");
    }
    // 提前检查固定栈回退的 guard page 加法，确保尚未 resume 的 Fiber
    // 也不会保存一个随后会回绕的映射尺寸。
    (void)checked_stack_mapping_size(aligned, page_size);
    return aligned;
#else
    const auto minimum = boost::context::stack_traits::minimum_size();
    const auto page_size = boost::context::stack_traits::page_size();
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (page_size == 0 || page_size > maximum / 2U) {
        throw std::length_error("fiber page size is invalid");
    }
    const auto safe_maximum = maximum - 2U * page_size;
    if (minimum > safe_maximum) {
        throw std::length_error("fiber minimum stack size is invalid");
    }
    std::size_t result = std::min(std::max(requested, minimum), safe_maximum);
    if (!boost::context::stack_traits::is_unbounded()) {
        const auto configured_maximum =
            boost::context::stack_traits::maximum_size();
        if (configured_maximum < minimum) {
            throw std::length_error("fiber maximum stack size is invalid");
        }
        result = std::min(result, configured_maximum);
    }
    return result;
#endif
}

#if defined(GO2CPP_FIBER_GROWABLE_STACK)
// 归一化预留区：0 → 默认 8MiB；页对齐；至少覆盖初始提交量。
// 只有 reserve > usable 时才可能启用增长；具体退化在 FiberStack 构造中。
std::size_t normalize_stack_reserve_size(std::size_t requested,
                                          std::size_t usable,
                                          std::size_t page_size) {
    if (requested == 0) {
        requested = Fiber::DefaultStackReserveSize();
    }
    if (requested < usable) {
        requested = usable;
    }
    return align_stack_size(requested, page_size);
}
#endif

/**
 * @brief 一个 Fiber 的独立栈：大预留 + 顶部初始提交 + 可选按需增长。
 *
 * ## 两种布局
 *
 * - growable（Linux/x86_64 + 原生上下文 + 环境变量允许）：
 *     base = mmap(reserve, PROT_NONE, MAP_NORESERVE)
 *     top  = base + reserve
 *     committed = top - initial_usable
 *     mprotect(committed, initial_usable, RW)
 *   未提交区 [base, committed) 天然是 guard；栈地址（top/base）不变，
 *   增长只是 committed 向下移动，因此栈上的局部变量指针始终有效。
 * - 固定栈（关闭增长/reserve 不足/非原生后端）：
 *   保留旧布局，usable + 1 页 PROT_NONE guard，避免退化路径改变语义。
 *
 * ## 增长
 *
 * SIGSEGV handler 在故障地址落在 [base, committed)、rsp 落在预留区且两者
 * 距离不超过 256KiB 时，以 64KiB 为单位向下 mprotect，然后直接 return。
 * handler 只调用 async-signal-safe 函数，事件计数用原子变量，不打印日志。
 *
 * ## 池化
 *
 * 缓存键是 (initial_usable, reserve)；固定栈的 reserve 为 0。复用 growable
 * 栈前必须把增长过的区间重新 mprotect(PROT_NONE) 并把 committed 重置回
 * top - initial_usable —— 只重置指针会让后续溢出静默写坏已提交内存。
 *
 * ## 生命周期
 *
 * 构造时映射；析构时交给 `StackCache` 而不是直接 munmap（除非缓存满）。
 * Valgrind 构建会注册/注销栈边界，普通构建被客户端宏消除、不影响热路径。
 */
struct FiberStack {
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
    explicit FiberStack(std::size_t usable, std::size_t requested_reserve,
                        bool requested_growable)
        : m_initial_usable(usable),
          m_page_size(native_page_size()),
          m_reserve(resolve_reserve(m_initial_usable, requested_reserve,
                                    requested_growable, m_page_size)),
          m_mapping_size(m_reserve != 0
                             ? m_reserve
                             : checked_stack_mapping_size(m_initial_usable,
                                                          m_page_size)),
          m_mapping(acquire_stack_mapping(m_initial_usable, m_reserve,
                                          m_page_size, m_mapping_size)) {
        const auto base = reinterpret_cast<std::uintptr_t>(m_mapping);
        m_committed.store(
            m_reserve != 0 ? base + m_mapping_size - m_initial_usable
                           : base + m_page_size,
            std::memory_order_relaxed);
#if defined(GO2CPP_FIBER_HAVE_VALGRIND)
        // 自定义 context 汇编不会自动告诉 Memcheck 新栈边界。注册只在
        // Memcheck 运行时生效，普通构建会被 Valgrind 客户端宏消除。
        m_valgrind_stack_id = VALGRIND_STACK_REGISTER(bottom(), stack_pointer());
#endif
    }

    ~FiberStack() noexcept {
#if defined(GO2CPP_FIBER_HAVE_VALGRIND)
        if (m_valgrind_stack_id != 0U) {
            VALGRIND_STACK_DEREGISTER(m_valgrind_stack_id);
            m_valgrind_stack_id = 0U;
        }
#endif
        if (m_mapping != nullptr) {
            release_to_cache();
        }
    }

    FiberStack(const FiberStack&) = delete;
    FiberStack& operator=(const FiberStack&) = delete;

    bool growable() const noexcept { return m_reserve != 0; }

    // 诊断用：实际生效的预留大小与映射大小。构造参数 reserve 为 0 时
    // 也会解析出默认预留（8MiB）或固定栈映射，因此快照必须报告这里的
    // 实际值，而不是构造参数。
    std::size_t reserve_size() const noexcept { return m_reserve; }
    std::size_t mapping_size() const noexcept { return m_mapping_size; }

    // 当前已提交区的大小。growable 栈增长后 bottom() 会下移，MakeContext
    // 在首次 resume 前调用，因此这里返回的就是初始提交量；复用前池化路径
    // 已把 committed 重置回初始位置。
    std::size_t usable_size() const noexcept {
        const auto top = reinterpret_cast<std::uintptr_t>(stack_pointer());
        return top - reinterpret_cast<std::uintptr_t>(bottom());
    }

    // 当前已提交区的底部。MakeContext 用它作为新上下文的 rsp 起点；
    // 增长只改变 committed，不改变 top，所以栈上的地址/指针始终有效。
    const void* bottom() const noexcept {
        if (m_reserve != 0) {
            return reinterpret_cast<const void*>(
                m_committed.load(std::memory_order_relaxed));
        }
        return static_cast<const char*>(m_mapping) + m_page_size;
    }

    void* stack_pointer() const noexcept {
        return static_cast<char*>(m_mapping) + m_mapping_size;
    }

    // ---- SIGSEGV handler 访问器（async-signal-safe）----------------------
    char* growth_base() const noexcept { return static_cast<char*>(m_mapping); }
    char* growth_top() const noexcept {
        return static_cast<char*>(m_mapping) + m_mapping_size;
    }
    std::uintptr_t growth_committed() const noexcept {
        return m_committed.load(std::memory_order_relaxed);
    }
    void growth_commit(std::uintptr_t value) noexcept {
        m_committed.store(value, std::memory_order_relaxed);
    }
    std::size_t growth_page_size() const noexcept { return m_page_size; }

private:
    // 决定是否真正启用增长。返回 0 表示固定栈布局：
    //   - 调用方关闭 growable；
    //   - GO2CPP_GROWABLE_STACK=0；
    //   - 备用栈/handler 安装失败；
    //   - reserve 不足以再提交一个 64KiB 增长块。
    static std::size_t resolve_reserve(std::size_t usable,
                                       std::size_t requested_reserve,
                                       bool requested_growable,
                                       std::size_t page) {
#if defined(GO2CPP_FIBER_ASAN) || defined(GO2CPP_FIBER_TSAN)
        // ASAN/TSAN 自己管理栈切换与栈边界；按设计文档回退固定栈，
        // 避免 sanitizer 把增长后的页误判为越界/泄漏。
        (void)usable;
        (void)requested_reserve;
        (void)requested_growable;
        (void)page;
        return 0;
#elif defined(GO2CPP_FIBER_GROWABLE_STACK)
        if (!requested_growable || !growable_stack_env_enabled()) {
            return 0;
        }
        const auto reserve =
            normalize_stack_reserve_size(requested_reserve, usable, page);
        // reserve == initial_usable（或只多出不足一个增长块）按固定栈处理。
        // 固定栈路径不安装 SIGSEGV handler，也不分配 sigaltstack。
        if (reserve <= usable ||
            reserve - usable < kGrowableStackGrowthGranularity) {
            return 0;
        }
        if (!ensure_growable_thread_support()) {
            return 0;
        }
        return reserve;
#else
        (void)requested_reserve;
        (void)requested_growable;
        (void)page;
        return 0;
#endif
    }

    // 从池中取一段已经处于「初始提交」状态的映射；没有命中则新建。
    // 池化键是 (initial_usable, reserve)，固定栈的 reserve 恒为 0。
    static void* acquire_stack_mapping(std::size_t usable,
                                       std::size_t reserve,
                                       std::size_t page,
                                       std::size_t mapping_size) {
        for (auto it = s_thread_stack_pool.entries.rbegin();
             it != s_thread_stack_pool.entries.rend(); ++it) {
            if (it->usable == usable && it->reserve == reserve &&
                it->mapping != nullptr) {
                void* mapping = it->mapping;
                s_thread_stack_pool.entries.erase(std::next(it).base());
                // 池里的 growable 栈在 release_to_cache() 时已经把增长过的
                // 区间重新 mprotect(PROT_NONE)，committed 也重置回
                // top - initial_usable；这里不需要再次改动页保护。
                return mapping;
            }
        }
        {
            std::lock_guard<std::mutex> lock(s_stack_pool_mutex);
            for (auto it = s_stack_pool.entries.rbegin();
                 it != s_stack_pool.entries.rend(); ++it) {
                if (it->usable == usable && it->reserve == reserve &&
                    it->mapping != nullptr) {
                    void* mapping = it->mapping;
                    s_stack_pool.entries.erase(std::next(it).base());
                    return mapping;
                }
            }
        }

        void* mapping = nullptr;
        if (reserve != 0) {
            // 整段预留 PROT_NONE + MAP_NORESERVE；只把顶部初始可用量
            // 改成 RW。未提交区天然就是 guard page，无需再移动 guard。
            mapping = ::mmap(nullptr, reserve, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                             -1, 0);
            if (mapping == MAP_FAILED) {
                throw std::bad_alloc();
            }
            if (::mprotect(static_cast<char*>(mapping) + reserve - usable,
                           usable, PROT_READ | PROT_WRITE) != 0) {
                const int error = errno;
                ::munmap(mapping, reserve);
                throw std::system_error(error, std::generic_category(),
                                        "mprotect fiber initial stack");
            }
        } else {
            // 固定栈：保留旧行为，映射末尾额外一页 PROT_NONE guard。
            mapping = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapping == MAP_FAILED) {
                throw std::bad_alloc();
            }
            if (::mprotect(mapping, page, PROT_NONE) != 0) {
                const int error = errno;
                ::munmap(mapping, mapping_size);
                throw std::system_error(error, std::generic_category(),
                                        "mprotect fiber guard page");
            }
        }
        return mapping;
    }

    // 回收前把 growable 栈的增长区重新设为 PROT_NONE，并重置 committed。
    // 这是保证「复用后的 Fiber 仍从初始 1 页开始」且未提交区继续充当
    // guard 的必要步骤；只重置指针而保留 RW 会让后续溢出静默写坏内存。
    // mprotect 失败时退回 munmap，绝不把状态不明的页放回池中。
    void release_to_cache() noexcept {
        bool cacheable = true;
        if (m_reserve != 0) {
            const auto top = reinterpret_cast<std::uintptr_t>(stack_pointer());
            const auto initial = top - m_initial_usable;
            const auto committed = m_committed.load(std::memory_order_relaxed);
            if (committed < initial) {
                if (::mprotect(reinterpret_cast<void*>(committed),
                               initial - committed, PROT_NONE) != 0) {
                    cacheable = false;
                } else {
                    m_committed.store(initial, std::memory_order_relaxed);
                }
            }
        }
        if (cacheable) {
            try {
                if (s_thread_stack_pool.entries.size() < 32U) {
                    s_thread_stack_pool.entries.push_back(
                        CachedStack{m_initial_usable, m_reserve, m_mapping,
                                    m_mapping_size});
                    m_mapping = nullptr;
                    return;
                }
                std::lock_guard<std::mutex> lock(s_stack_pool_mutex);
                if (s_stack_pool.entries.size() < 512U) {
                    s_stack_pool.entries.push_back(
                        CachedStack{m_initial_usable, m_reserve, m_mapping,
                                    m_mapping_size});
                    m_mapping = nullptr;
                    return;
                }
            } catch (...) {
            }
        }
        ::munmap(m_mapping, m_mapping_size);
        m_mapping = nullptr;
    }

    std::size_t m_initial_usable;
    std::size_t m_page_size;
    std::size_t m_reserve;
    std::size_t m_mapping_size;
    void* m_mapping;
    std::atomic<std::uintptr_t> m_committed{0};
#if defined(GO2CPP_FIBER_HAVE_VALGRIND)
    unsigned m_valgrind_stack_id{0U};
#endif
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
            if (s_thread_stack_pool.entries.size() < 32U) {
                s_thread_stack_pool.entries.push_back(CachedStack{size, context});
                context = {};
                return;
            }
            std::lock_guard<std::mutex> lock(s_stack_pool_mutex);
            if (s_stack_pool.entries.size() < 512U) {
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

#if defined(GO2CPP_FIBER_GROWABLE_STACK)
void growable_stack_segv_handler(int sig, siginfo_t* info,
                                 void* raw_context) noexcept {
    // handler 内部再次 fault（mprotect 失败、栈边界判断错误、备用栈也
    // 不可用）时不能递归，直接交给默认动作产生 core。
    if (t_growable_in_handler != 0) {
        growable_stack_fatal(sig);
    }
    t_growable_in_handler = 1;
    const int saved_errno = errno;

    FiberStack* stack = t_current_growable_stack;
    if (sig != SIGSEGV || stack == nullptr || info == nullptr ||
        raw_context == nullptr || !stack->growable()) {
        growable_stack_fatal(sig);
    }

    const auto* context = static_cast<const ucontext_t*>(raw_context);
    // 取指故障（x86 error code bit 4）不是栈增长；野指针跳到未映射区时
    // 也走这里，直接恢复默认动作。
    if ((context->uc_mcontext.gregs[REG_ERR] & 0x10) != 0) {
        growable_stack_fatal(sig);
    }
    const char* fault = static_cast<const char*>(info->si_addr);
    const char* rsp = reinterpret_cast<const char*>(
        context->uc_mcontext.gregs[REG_RSP]);
    const auto committed = stack->growth_committed();
    const auto base_value =
        reinterpret_cast<std::uintptr_t>(stack->growth_base());
    const auto top_value =
        reinterpret_cast<std::uintptr_t>(stack->growth_top());
    const auto fault_value = reinterpret_cast<std::uintptr_t>(fault);
    const auto rsp_value = reinterpret_cast<std::uintptr_t>(rsp);

    // 故障必须落在未提交区 [base, committed)。已提交区内的 fault 不是
    // 栈增长（可能是破坏性写入命中了仍可写的页），交回默认动作。
    if (fault == nullptr || fault_value < base_value ||
        fault_value >= committed) {
        growable_stack_fatal(sig);
    }
    // rsp 必须在预留区内，但不能要求 rsp >= committed：函数序言会先
    // sub rsp 再写局部变量，实测 rsp 可以比 committed 低几十字节
    // （probe: fault=committed-8, rsp=committed-0x50）。只要求它和故障
    // 地址处于同一个 256KiB 窗口内。
    if (rsp_value < base_value || rsp_value >= top_value) {
        growable_stack_fatal(sig);
    }
    const auto distance = fault_value >= rsp_value
                              ? fault_value - rsp_value
                              : rsp_value - fault_value;
    if (distance > kGrowableStackFaultWindow) {
        growable_stack_fatal(sig);
    }
    // 预留区底部不足一个增长粒度时无法再提交；committed 向下移动，
    // committed - base 递减到 0 就是“到达上限”的唯一判定点。
    if (committed - base_value < kGrowableStackGrowthGranularity) {
        growable_stack_fatal(sig);
    }
    const auto page = static_cast<std::uintptr_t>(stack->growth_page_size());
    // 从 min(fault, rsp) 向下提交：大帧的局部变量可能位于 rsp 上方，
    // 只按 fault 提交会漏掉 rsp 以下的帧；按两者较小值提交才能让重试
    // 指令一次成功（前提是二者距离在窗口内）。
    auto new_low = fault_value < rsp_value ? fault_value : rsp_value;
    new_low &= ~(page - 1U);
    new_low = new_low >= kGrowableStackGrowthGranularity
                  ? new_low - kGrowableStackGrowthGranularity
                  : base_value;
    if (new_low < base_value) {
        new_low = base_value;
    }
    if (new_low >= committed) {
        growable_stack_fatal(sig);
    }
    if (::mprotect(reinterpret_cast<void*>(new_low), committed - new_low,
                   PROT_READ | PROT_WRITE) != 0) {
        // mprotect 失败（ENOMEM 等）不能重试，否则会无限 SIGSEGV。
        growable_stack_fatal(sig);
    }
    stack->growth_commit(new_low);
    s_growable_stack_growth_count.fetch_add(1, std::memory_order_relaxed);
    t_growable_in_handler = 0;
    // mprotect 成功时一般不动 errno，但实现允许；恢复被中断代码的值，
    // 避免一次栈增长污染用户可见的 errno。
    errno = saved_errno;
    // 直接 return：内核重新执行触发 fault 的指令，此时页已可写。
    // 不 longjmp、不换上下文，栈地址与所有局部变量指针保持有效。
}
#endif

// 父链只保存这份独立元数据，不保存父 Fiber 栈或 owner 的所有权。子 Fiber
// 持有父记录，因此父对象结束后仍可生成一致的墓碑帧、传播取消状态；
// 真正的 fcontext 恢复仍要求父 Fiber 对象存活。错误生命周期会进入 Failed
// 放弃路径，不能把已经失效的父栈当作可恢复上下文。
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
/// 删除某个 Fiber 的调试元数据（定义见 FiberMetaMap 之后）。
void EraseFiberDebugMeta(std::uint64_t fiber_id) noexcept;
#endif

struct FiberRecord {
    explicit FiberRecord(std::uint64_t fiber_id) : id(fiber_id) {}
    ~FiberRecord() {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        // record 是 Fiber 调试元数据的最后归属：最后一个 shared_ptr 释放
        // 时同步删除 FiberMetaMap 条目，避免每个 fiber id 永久滞留。
        EraseFiberDebugMeta(id);
#endif
    }

    // 原地复用：reset_for_scheduler() 更新 id 而不是替换 record 对象，
    // 因此这里不能是 const；所有并发读点都在 mutex 内（或持有 resume 权）。
    std::uint64_t id;
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
    /// 以下仅为调试元数据（GO2CPP_DEBUG_DUMP=ON 时填充）：
    /// 任务实际函数名（typeid demangle / GO2CPP_FIBER_NAME）与栈地址范围。
    std::string function_name;
    std::uintptr_t stack_low{0};
    std::uintptr_t stack_high{0};
    bool stack_known{false};
    /// 构造/reset 时发布的栈请求参数。debug_stack 在 record->mutex 内
    /// 读取它们，避免并发读 Impl 的非原子字段（FIBER-01）。
    std::size_t requested_stack_bytes{0};
    std::size_t reserve_stack_bytes{0};
};

/// 任务函数名：优先 GO2CPP_FIBER_NAME 的显式名字，其次 typeid 的
/// target_type 名（std::function 的 target_type 才能拿到真实 lambda/函数
/// 指针类型；直接 typeid(Function) 只会得到 std::function<void()>）。
std::string DescribeFiberFunction(const Fiber::Function& function) {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    const char* explicit_name = go2cpp::debug::TakeNextFiberName();
    if (explicit_name != nullptr && *explicit_name != '\0') {
        return explicit_name;
    }
    const std::type_info& type = function.target_type();
    const char* raw = type.name();
    if (raw == nullptr) {
        return "<unknown>";
    }
#if defined(__GNUG__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
        std::string name(demangled);
        std::free(demangled);
        return name;
    }
    std::free(demangled);
#endif
    return raw;
#else
    (void)function;
    return {};
#endif
}

#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
/// Fiber 调试元数据发布表：父链样本需要父 Fiber 的函数名与栈范围，
/// 而 free function 无法访问 Fiber 的私有 Impl；`context_snapshot()`
/// 遍历父链时顺带发布，供 CaptureFiberChain 查询。
struct FiberDebugMeta {
    std::string function;
    std::uintptr_t stack_low{0};
    std::uintptr_t stack_high{0};
    bool stack_known{false};
};

/// 元数据表容量上限。正常路径由 `~FiberRecord()` / `reset_for_scheduler()`
/// 即时删除；上限只是兜底，防止任何遗漏路径让表随历史 fiber 数增长。
constexpr std::size_t kMaxFiberMetaEntries = 4096;
/// 触顶时一次淘汰的最旧条目数（批量删除，避免每个新 fiber 都淘汰一次）。
constexpr std::size_t kFiberMetaEvictBatch = 512;

/// 元数据表：entries 是查询表；order/order_pos 维护插入顺序，容量兜底时
/// 按「最旧优先」批量淘汰。三者由 FiberMetaMutex 共同保护。
struct FiberMetaTable {
    std::unordered_map<std::uint64_t, FiberDebugMeta> entries;
    std::list<std::uint64_t> order;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator>
        order_pos;
};

std::mutex& FiberMetaMutex() {
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}

FiberMetaTable& FiberMetaTableRef() {
    static auto* table = new FiberMetaTable();
    return *table;
}

std::unordered_map<std::uint64_t, FiberDebugMeta>& FiberMetaMap() {
    return FiberMetaTableRef().entries;
}

/// 删除某个 fiber id 的元数据（O(1)）；重复调用安全。
void EraseFiberDebugMeta(std::uint64_t fiber_id) noexcept {
    if (fiber_id == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(FiberMetaMutex());
    FiberMetaTable& table = FiberMetaTableRef();
    const auto found = table.entries.find(fiber_id);
    if (found == table.entries.end()) {
        return;
    }
    const auto position = table.order_pos.find(fiber_id);
    if (position != table.order_pos.end()) {
        table.order.erase(position->second);
        table.order_pos.erase(position);
    }
    table.entries.erase(found);
}

/// 读取 record 元数据并发布（record 由父链共享，墓碑也可读）。
void PublishFiberDebugMeta(FiberRecord& record) {
    FiberDebugMeta meta;
    std::uint64_t fiber_id = 0;
    {
        std::lock_guard<std::mutex> lock(record.mutex);
        // id 会被 reset_for_scheduler() 原地更新，必须在同一把锁内取快照。
        fiber_id = record.id;
        meta.function = record.function_name;
        meta.stack_low = record.stack_low;
        meta.stack_high = record.stack_high;
        meta.stack_known = record.stack_known;
    }
    std::lock_guard<std::mutex> lock(FiberMetaMutex());
    FiberMetaTable& table = FiberMetaTableRef();
    const auto found = table.entries.find(fiber_id);
    if (found != table.entries.end()) {
        found->second = std::move(meta);
        return;
    }
    if (table.entries.size() >= kMaxFiberMetaEntries) {
        // 兜底淘汰：按插入顺序删除最旧的一批。只清理元数据，不影响
        // FiberRecord / 链样本本身（诊断采样本来就是 best-effort）。
        for (std::size_t index = 0;
             index < kFiberMetaEvictBatch && !table.order.empty(); ++index) {
            const std::uint64_t victim = table.order.front();
            table.order.pop_front();
            table.order_pos.erase(victim);
            table.entries.erase(victim);
        }
    }
    const auto inserted = table.entries.emplace(fiber_id, std::move(meta));
    if (!inserted.second) {
        return;
    }
    table.order.push_back(fiber_id);
    try {
        table.order_pos.emplace(fiber_id, std::prev(table.order.end()));
    } catch (...) {
        // 位置索引只是兜底淘汰的优化；失败时该条目仍由 ~FiberRecord
        // 删除，不会永久驻留。
    }
}
#endif  // GO2CPP_DEBUG_DUMP

}  // namespace

// Impl 只在 fiber.cpp 内使用；保留默认可见性（GCC 对类内定义的成员函数
// 仍生成 weak 符号，pragma visibility 在这里不改变绑定方式，见 2026-10 实测）。
struct Fiber::Impl {
    explicit Impl(Fiber* owner, Function function, std::size_t stack_size,
                  std::size_t stack_reserve_size, bool stack_growable)
        : m_owner(owner),
          m_id(s_next_fiber_id.fetch_add(1, std::memory_order_relaxed)),
          m_record(std::make_shared<FiberRecord>(m_id)),
          m_function(std::move(function)),
          m_stack_size(normalize_stack_size(stack_size)),
          m_stack_reserve_size(stack_reserve_size),
          m_stack_growable(stack_growable) {
        // 构造期还没有并发观察者；直接发布栈请求参数。debug_stack 以后
        // 只从 record 读这两个值，不再碰 Impl 的非原子字段（FIBER-01）。
        m_record->requested_stack_bytes = m_stack_size;
        m_record->reserve_stack_bytes = m_stack_reserve_size;
#if defined(GO2CPP_FIBER_TSAN)
        m_tsan_fiber = __tsan_create_fiber(0);
#endif
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        m_record->function_name = DescribeFiberFunction(m_function);
#endif
    }

    void reset_for_scheduler(Function function, std::size_t stack_size,
                             std::size_t stack_reserve_size,
                             bool stack_growable) {
        // 只有 entry 已经返回 caller，且 resume_locked 已将 m_context 和
        // m_stack 清空后才能重绑定。Scheduler 只在 terminal Fiber 上调用
        // 该方法；若后端约束被破坏，宁可抛出也不能复用活动栈。
        const auto state = m_record->state.load(std::memory_order_acquire);
        if ((state != FiberState::Completed && state != FiberState::Failed) ||
            m_context != nullptr || m_stack != nullptr ||
            m_resume_claim.load(std::memory_order_acquire)) {
            throw std::logic_error("cannot rebind an active Fiber");
        }

        const auto normalized_stack_size = normalize_stack_size(stack_size);
        const auto next_id =
            s_next_fiber_id.fetch_add(1, std::memory_order_relaxed);

#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        // 旧 fiber id 不再对应任何可观测执行流；立即删除，而不是等
        // Fiber 对象析构或 bin 淘汰（每个任务复用 Fiber 时会走这里）。
        EraseFiberDebugMeta(m_record->id);
#endif
        m_function = std::move(function);
        m_id = next_id;
        {
            // 原地重置同一个 FiberRecord（FIBER-03）：不再替换 shared_ptr。
            // 所有外部观察者持有的 record 指针因此始终有效，读 m_record
            // 与重置之间不存在指针竞争。调用方（RecycleForScheduler）已
            // 保证 use_count()==1，重置不会覆盖别的 Fiber 的父链墓碑。
            std::lock_guard<std::mutex> lock(m_record->mutex);
            m_record->id = next_id;
            m_record->parent.reset();
            m_record->alive.store(true, std::memory_order_release);
            m_record->state.store(FiberState::Ready,
                                  std::memory_order_release);
            m_record->reason.store(SuspendReason::None,
                                   std::memory_order_release);
            m_record->cancellation_requested.store(false,
                                                   std::memory_order_release);
            m_record->active_parent_id = 0;
            m_record->depth = 0;
            m_record->last_thread = {};
            m_record->execution = {};
            m_record->requested_stack_bytes = normalized_stack_size;
            m_record->reserve_stack_bytes = stack_reserve_size;
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
            m_record->function_name = DescribeFiberFunction(m_function);
            m_record->stack_low = 0;
            m_record->stack_high = 0;
            m_record->stack_known = false;
#endif
        }
        m_stack_size = normalized_stack_size;
        m_stack_reserve_size = stack_reserve_size;
        m_stack_growable = stack_growable;
        m_caller = nullptr;
        m_scheduler_propagate.store(false, std::memory_order_relaxed);
        m_scheduler_parent_bound = false;
        m_resume_claim.store(false, std::memory_order_relaxed);
        m_last_resume_thread_hash.store(0, std::memory_order_relaxed);
        m_parent.store(nullptr, std::memory_order_relaxed);
        m_parent_bound.store(false, std::memory_order_relaxed);
        m_saved_errno = 0;
        {
            std::lock_guard<std::mutex> lock(m_failure_mutex);
            m_failure = {};
        }
#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
        m_migration_enabled.store(false, std::memory_order_relaxed);
        m_migration_wait_active.store(false, std::memory_order_relaxed);
        m_migration_channel = nullptr;
        m_migration_node = nullptr;
        m_migration_pending_slot = nullptr;
        m_migration_kind = 0;
#endif
#if defined(GO2CPP_FIBER_TSAN)
        if (m_tsan_fiber != nullptr) {
            __tsan_destroy_fiber(m_tsan_fiber);
        }
        m_tsan_fiber = __tsan_create_fiber(0);
        m_tsan_caller = nullptr;
#endif
#if defined(GO2CPP_FIBER_ASAN)
        m_asan_fake_stack = nullptr;
        m_asan_caller_bottom = nullptr;
        m_asan_caller_size = 0;
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
#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
        m_migration_wait_active.store(false, std::memory_order_release);
        m_migration_channel = nullptr;
        m_migration_node = nullptr;
        m_migration_pending_slot = nullptr;
        m_migration_kind = 0;
#endif
        m_record->reason.store(SuspendReason::None, std::memory_order_release);
        m_scheduler_propagate.store(false, std::memory_order_release);
        m_record->state.store(FiberState::Failed, std::memory_order_release);
        fiber_local::detail::Cleanup(m_owner);
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        // 放弃路径不会复用该 record；清掉元数据避免长期滞留。
        EraseFiberDebugMeta(m_record->id);
#endif
    }

    // 析构等待 resume 权的预算。正常路径第一次 CAS 就成功（另一个线程
    // 只在 resume() 期间短暂持有 claim），因此热路径零额外开销；只有
    // 「另一个线程仍在 resume 时析构」这种契约违例才会进入退避循环。
    static constexpr std::size_t kDestroyClaimSpins = 64;
    static constexpr std::chrono::microseconds kDestroyClaimBackoffMin{20};
    static constexpr std::chrono::microseconds kDestroyClaimBackoffMax{2000};
    static constexpr std::chrono::microseconds kDestroyClaimBudget{200000};

    /// 有界取得 resume 权（FIBER-02）。成功返回 true 且 m_resume_claim
    /// 已置 true；超时返回 false 且不修改任何字段。
    /// @note 先做固定次数的 yield（覆盖「resume 正在收尾」的常见窗口），
    ///       再用指数退避 sleep，总预算 kDestroyClaimBudget，避免烧核。
    bool try_claim_for_destroy() noexcept {
        bool expected = false;
        if (m_resume_claim.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + kDestroyClaimBudget;
        auto backoff = kDestroyClaimBackoffMin;
        for (std::size_t spin = 0;; ++spin) {
            if (spin < kDestroyClaimSpins) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(backoff);
                backoff = std::min(backoff * 2, kDestroyClaimBackoffMax);
            }
            expected = false;
            if (m_resume_claim.compare_exchange_weak(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
        }
    }

    /// 超时灾难路径：另一个线程仍持有 resume 权，绝不能销毁 Impl。
    /// 只写原子墓碑；Running 时主体仍在执行（闭包与栈都在使用中），
    /// 不做任何上下文清理，等它自然结束。非 Running 时尽力放弃上下文。
    void on_destroy_timeout() noexcept {
        m_record->alive.store(false, std::memory_order_release);
        m_record->cancellation_requested.store(true,
                                               std::memory_order_release);
        if (m_record->state.load(std::memory_order_acquire) ==
            FiberState::Running) {
            return;
        }
        abandon_suspended(
            "Fiber destroyed while another thread held the resume claim");
    }

    /// 把 Impl 移入进程级隔离区：进程退出前不析构，避免仍持有 resume 权
    /// 的线程随后访问已释放对象。只在超时灾难路径调用，不进入热路径。
    static void quarantine(std::unique_ptr<Impl> impl) noexcept {
        try {
            std::lock_guard<std::mutex> lock(QuarantineMutex());
            QuarantineList().push_back(std::move(impl));
        } catch (...) {
            // 隔离区分配失败时只能泄漏；析构路径不允许抛异常。
            (void)impl.release();
        }
    }

    static std::mutex& QuarantineMutex() noexcept {
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

    static std::vector<std::unique_ptr<Impl>>& QuarantineList() {
        static auto* list = new std::vector<std::unique_ptr<Impl>>();
        return *list;
    }

    ~Impl() noexcept {
        // 析构只允许在 ~Fiber 已经（有界地）取得 resume 权后进入。
        // 若此时没有 claim，说明另一个线程正在 resume 并使用上下文，
        // 继续析构就是 UAF；fail-fast 比静默破坏更安全。
        if (!m_resume_claim.load(std::memory_order_acquire)) {
            std::terminate();
        }

        // 不能在 Fiber 自身栈上销毁自身；此约束保留为明确的 API 前置条件，
        // 否则没有任何安全位置可以释放当前正在使用的栈。
        if (s_current_fiber == m_owner) {
            std::terminate();
        }

        // claim 已由 ~Fiber 有界取得（FIBER-02），可以直接观察终态。
        // entry() 会先发布 Completed/Failed，后由 resume() 释放 claim；
        // 若只检查 state，析构可能在这个窗口释放 fcontext/保护栈，导致
        // resume_locked() 继续访问已释放对象。

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
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        // 提前清理调试元数据；若仍有父链样本持有该 record，下一次
        // context_snapshot() 会按需重新发布。
        EraseFiberDebugMeta(m_record->id);
#endif
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
            // 这里只隔离用户代码意外抛出的异常。Fiber 挂起、取消和清理流程
            // 从不使用异常。
            std::lock_guard<std::mutex> lock(self->m_failure_mutex);
            self->m_failure = std::current_exception();
            terminal_state = FiberState::Failed;
        }
        self->m_function = {};
        // Fiber-local 值属于逻辑 G，而不是 worker M。主体栈展开后、把控制权
        // 交回调用者前，执行这些值的析构函数。
        fiber_local::detail::Cleanup(self->m_owner);
        self->m_record->reason.store(SuspendReason::None, std::memory_order_release);
        self->m_record->state.store(terminal_state, std::memory_order_release);
        self->m_saved_errno = load_errno();

#if defined(GO2CPP_FIBER_ASAN)
        // 已完成的执行栈不会再次恢复；在调用者释放受保护分配前先丢弃 fake
        // stack。
        __sanitizer_start_switch_fiber(nullptr, self->m_asan_caller_bottom,
                                      self->m_asan_caller_size);
#endif
#if defined(GO2CPP_FIBER_TSAN)
        __tsan_switch_to_fiber(self->m_tsan_caller, 0);
#endif
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
        // 栈即将交还给调用者；在真正跳转前清掉，避免窗口期内 handler
        // 把调用者栈上的 fault 误判成这个已完成的 Fiber 栈。
        t_current_growable_stack = nullptr;
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

        // 快速路径（同一父级的后续 resume）：父链在首次绑定时固定，之后
        // 不再变化，因此不必每次 resume 都重复「两次 mutex + shared_ptr
        // 原子引用计数 + 父记录拷贝」。用户态 `Fiber::resume()` 的每一次
        // 恢复都会走到这里，这是它与调度器热路径之间最大的差距。
        //
        // 可见性：写侧在 resume claim 与 m_metadata_mutex 保护下完成，
        // 并以 release 发布 m_parent_bound；这里用 acquire 读 bound，
        // 保证读到 true 时 m_parent 已是最终值。
        if (m_parent_bound.load(std::memory_order_acquire)) {
            return m_parent.load(std::memory_order_relaxed) == caller;
        }

        std::shared_ptr<FiberRecord> parent_record;
        {
            std::lock_guard<std::mutex> lock(m_metadata_mutex);
            if (!m_parent_bound.load(std::memory_order_relaxed)) {
                m_parent.store(caller, std::memory_order_relaxed);
                m_parent_bound.store(true, std::memory_order_release);
            } else if (m_parent.load(std::memory_order_relaxed) != caller) {
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
        if (m_parent.load(std::memory_order_relaxed) != caller) {
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

    /**
     * @brief 切换到本 Fiber 执行，直到它挂起/结束再返回调用者。
     * @param validate_caller 是否校验「调用者确实是本 Fiber 的父级」（调度器
     *        内部恢复已建立父子关系时可传 false 省掉校验）；
     * @param scheduler_fast 调度器热路径模式：跳过跨线程记录（`last_thread`
     *        的哈希比较与写回），只保留必要的上下文切换；
     * @return true 表示成功切换过；false 表示状态不允许或 context 初始化失败。
     *
     * ## 定位
     *
     * 这是**整个 Fiber 机制的唯一切换点**：`Fiber::Resume`、`Task::run` 的
     * trampoline、`Scheduler` 的 G 执行都收敛到这里。它下面是
     * `jump_fcontext`（或原生 asm 版本），上面是所有并发原语。
     *
     * ## 步骤
     *
     * step1. 可选地校验调用者：`bind_caller()` 会在父子关系不符时返回 false，
     *        防止「A 的父级被写成 B」这类栈链损坏；
     * step2. 保存最初的调用者并登记 `FiberTlsRestore` —— 本函数内部可能沿
     *        父链**循环多轮**，每轮都临时改 TLS，退出时必须恢复到最初的调用者；
     * step3. 进入 `for (;;)`：校验状态必须是 `Ready`（首次）或 `Suspended`
     *        （再次），`Ready` 且无 context 时先 `initialize_context()`；
     * step4. 把状态置为 `Running`、清 `reason` 与 `m_scheduler_propagate`；
     * step5. 保存当前 `errno`、把 TLS 当前 Fiber 切成本 Fiber、非 fast 模式
     *        下记录 `last_thread`，然后 `store_errno(m_saved_errno)`；
     * step6. 执行真正的上下文切换（含 ASAN/TSAN 的 fiber 切换钩子）；
     *        返回时说明本 Fiber 已挂起或结束，恢复 TLS 与 errno 后回到循环
     *        （若是 Scheduler 级挂起，会继续沿父链处理）。
     *
     * ## 并发/失败语义
     *
     * - `noexcept`：切换路径不允许抛异常（栈已经换过，异常无法安全穿越）；
     * - 状态不是 `Ready`/`Suspended` 时返回 false，**不改变任何状态**；
     * - `m_context == nullptr` 且初始化失败时返回 false（例如栈分配失败）；
     * - **errno 必须按 Fiber 隔离**：进入前保存调用者的 errno，切到目标 Fiber
     *   时恢复它自己保存的 errno，返回时再恢复调用者的——否则一个 Fiber 的
     *   系统调用会污染另一个 Fiber 的 errno；
     * - ASAN/TSAN 钩子成对出现（`start_switch` / `finish_switch`），遗漏任一侧
     *   会让 sanitizer 把跨 Fiber 的栈访问误报为 use-after-return。
     */
    bool resume_locked(bool validate_caller, bool scheduler_fast = false) noexcept {
        // step1: 可选地校验调用者确实是本 Fiber 的父级。
        if (validate_caller && !bind_caller(s_current_fiber)) {
            return false;
        }
        // Scheduler 级挂起可能在本函数内部沿父链循环多次。每一轮跳转
        // 都暂时把 TLS 当前 Fiber 设为本对象，但完成后必须恢复最初的
        // 调用者；否则父函数在同一栈帧里创建下一个子 Fiber 时，会把
        // 已完成的前一个子 Fiber 错当成新子 Fiber 的父级。
        // step2: 记住最初的调用者；本函数可能沿父链循环多轮，退出时必须恢复。
        Fiber* const original_caller = s_current_fiber;
        const FiberTlsRestore tls_restore(original_caller);
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
        // handler 只应看到当前正在执行的 Fiber 栈；进入 resume 时保存
        // 调用者的栈，jump 返回后恢复（嵌套 Fiber 时是父 Fiber 的栈）。
        FiberStack* const previous_growable_stack = t_current_growable_stack;
        const GrowableStackTlsRestore stack_tls_restore(previous_growable_stack);
#endif
        // step3: 每轮都重新校验状态（上一轮切换后状态可能已变）。
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
#if defined(GO2CPP_FIBER_GROWABLE_STACK)
            // 构造期只在创建栈的线程安装过 altstack；Fiber 可能迁移到
            // 另一个 M，必须在真正 jump 前确认当前线程也有备用栈。
            // 首次之后只是一次 TLS bool + 原子状态读。
            if (m_stack_growable_active && !ensure_growable_thread_support()) {
                m_record->state.store(FiberState::Failed,
                                      std::memory_order_release);
                return false;
            }
#endif
            // step4: 进入运行态，清掉上一次挂起的原因与调度器传播标记。
            m_record->state.store(FiberState::Running, std::memory_order_release);
            m_record->reason.store(SuspendReason::None, std::memory_order_release);
            m_scheduler_propagate.store(false, std::memory_order_release);

            // step5: errno 按 Fiber 隔离——保存调用者的，切到目标 Fiber 前恢复
            // 它自己保存的值。swap_errno 一次调用同时完成读旧值与写目标值。
            const int caller_errno = swap_errno(m_saved_errno);
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
                t_current_growable_stack =
                    m_stack_growable_active ? m_stack.get() : nullptr;
                transfer = detail::JumpContext(m_context, this);
                // 切回调用者后立即恢复；随后 m_stack 可能被 reset 释放。
                t_current_growable_stack = previous_growable_stack;
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
                    // entry() 返回调用者前已经展开全部用户栈帧。现在可以立即
                    // 把受保护栈放回线程级或全局缓存，避免突发负载中每个排队
                    // Fiber 都重新建立一次映射。
                    m_stack.reset();
                    m_stack_growable_active = false;
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
            const FiberState after_jump =
                m_record->state.load(std::memory_order_acquire);
            Fiber* const parent_fiber = m_parent.load(std::memory_order_acquire);
            if (after_jump == FiberState::Suspended &&
                m_scheduler_propagate.load(std::memory_order_acquire) &&
                parent_fiber != nullptr) {
                const SuspendReason reason =
                    m_record->reason.load(std::memory_order_acquire);
                const auto parent_record = m_record->parent;
                if (!parent_record ||
                    !parent_record->alive.load(std::memory_order_acquire) ||
                    !parent_fiber->m_impl->suspend(reason, true)) {
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

#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        // park 是死锁分析的关键锚点：全量捕获线程栈与 Fiber 父链。
        // 事件只写入事件环，不改变挂起/恢复语义；任何异常都被吞掉，
        // 不能让诊断代码破坏 noexcept 的上下文切换。
        try {
            const std::uint64_t actor =
                m_debug_gid.load(std::memory_order_relaxed);
            go2cpp::debug::ResourceEventRecord event;
            event.code = go2cpp::debug::SchedEventCode::kParkCaptured;
            event.actor_gid = actor;
            event.arg2 = static_cast<std::uint64_t>(reason);
            event.result = go2cpp::debug::ResourceResult::kBlocked;
            event.chain_id = go2cpp::debug::CaptureFiberChainForResource(actor, true);
            go2cpp::debug::RecordResourceEvent(event);
        } catch (...) {
        }
#endif

#if defined(GO2CPP_FIBER_ASAN)
        __sanitizer_start_switch_fiber(&m_asan_fake_stack, m_asan_caller_bottom,
                                      m_asan_caller_size);
#endif
#if defined(GO2CPP_FIBER_TSAN)
        __tsan_switch_to_fiber(m_tsan_caller, 0);
#endif
            #if defined(GO2CPP_USE_NATIVE_CONTEXT)
            // 挂起后执行流回到调用者的 resume_locked；在跳转前清 TLS，
            // 避免调用者栈上的 fault 被误判为本 Fiber 的栈增长。
            t_current_growable_stack = nullptr;
            const detail::Transfer transfer = detail::JumpContext(m_caller, this);
            #else
            const boost::context::detail::transfer_t transfer =
                boost::context::detail::jump_fcontext(m_caller, this);
            #endif
        finish_switch_to_fiber();
        m_caller = transfer.context;
        // 这里不再重复写 reason/propagate/state/s_current_fiber：恢复本
        // Fiber 的 resume_locked 在跳转前已经把这四项设置成完全相同的值
        // （Running / None / false / m_owner），中间没有用户代码能改它们。
        // 每往返省 4 次原子/ TLS 写。
        return true;
    }


    void finish_switch_to_fiber() noexcept {
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
        // 迁移到新 M 后，resume_locked 在跳转前已设置过；这里再设置一次，
        // 覆盖 entry() 首次进入和 suspend() 返回后的两条路径。
        t_current_growable_stack =
            (m_stack && m_stack_growable_active) ? m_stack.get() : nullptr;
#endif
#if defined(GO2CPP_FIBER_ASAN)
        // 迁移后这些输出描述的是当前恢复 Fiber 的新 OS 线程，而不是上次
        // 挂起该 Fiber 的线程。
        __sanitizer_finish_switch_fiber(m_asan_fake_stack, &m_asan_caller_bottom,
                                       &m_asan_caller_size);
#endif
    }

    bool initialize_context() noexcept {
        try {
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
            m_stack = std::make_unique<FiberStack>(
                m_stack_size, m_stack_reserve_size, m_stack_growable);
#else
            m_stack = std::make_unique<FiberStack>(m_stack_size);
#endif
            #if defined(GO2CPP_USE_NATIVE_CONTEXT)
            m_context = detail::MakeContext(const_cast<void*>(m_stack->bottom()),
                                            m_stack->usable_size(), &Impl::entry);
            #else
            m_context = boost::context::detail::make_fcontext(
                m_stack->stack_pointer(), m_stack->usable_size(), &Impl::entry);
            #endif
            // 固化一次「实际是否按需增长」；resume/suspend 热路径不再解引用
            // unique_ptr 调用 growable()。
            m_stack_growable_active = m_stack->growable();
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
            {
                std::lock_guard<std::mutex> lock(m_record->mutex);
                m_record->stack_low = reinterpret_cast<std::uintptr_t>(
                    m_stack->growth_base());
                m_record->stack_high = reinterpret_cast<std::uintptr_t>(
                    m_stack->growth_top());
                m_record->stack_known = true;
            }
#endif
            return true;
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_failure_mutex);
            m_failure = std::current_exception();
            m_record->state.store(FiberState::Failed, std::memory_order_release);
            return false;
        }
    }

    /// @brief 反向指针（Impl 属于 Fiber）；trampoline 靠它从栈入口找回句柄。
    Fiber* m_owner;
    /// @brief 全局单调 Fiber Id；供 FiberRecord 与诊断使用，reset 时换成新 Id。
    std::uint64_t m_id;
    /// @brief 与外部观察者共享的元数据（状态/父链/时间戳）；快照跨线程读它。
    std::shared_ptr<FiberRecord> m_record;
    /// @brief 最近一次执行绑定的 G 编号（仅调试事件用；无锁快照）。
    std::atomic<std::uint64_t> m_debug_gid{0};
    /// @brief Fiber 主体；`reset_for_scheduler` 会替换成新提交的可调用对象。
    Function m_function;
    /// @brief 规范化后的初始提交量（页对齐，见 normalize_stack_size）。
    std::size_t m_stack_size;
    /// @brief 预留虚拟地址大小；0 表示使用 Fiber::DefaultStackReserveSize()。
    std::size_t m_stack_reserve_size{0};
    /// @brief 是否请求按需增长；实际是否增长还受环境变量/后端能力约束。
    bool m_stack_growable{true};
    /// @brief 本次栈实际是否按需增长（initialize_context 时从 FiberStack
    ///        固化）。热路径据此判断是否需要检查备用信号栈，避免每次切换
    ///        都做一次 unique_ptr 解引用 + growable() 调用。
    bool m_stack_growable_active{false};
    /// @brief 独立栈（FiberStack 内部走池化）；Fiber 结束后由 resume_locked 清空，
    ///        非空表示仍在活动 —— reset_for_scheduler 会据此拒绝重绑定。
    std::unique_ptr<FiberStack> m_stack;
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
    /// @brief 当前 Fiber 的上下文；首次 resume 前为 nullptr，结束后清空。
    detail::Context m_context{nullptr};
    /// @brief 切换前的调用方上下文；Suspend 时切回它。
    detail::Context m_caller{nullptr};
#else
    /// @brief 当前 Fiber 的 fcontext；首次 resume 前为 nullptr，结束后清空。
    boost::context::detail::fcontext_t m_context{nullptr};
    /// @brief 切换前的调用方 fcontext；Suspend 时切回它。
    boost::context::detail::fcontext_t m_caller{nullptr};
#endif
    /// @brief 是否把 SuspendReason 向上传播给父 Fiber（嵌套 Fiber 的取消传播）。
    std::atomic<bool> m_scheduler_propagate{false};
    /// @brief Scheduler 父链是否已绑定；绑定后不允许跨父级恢复。
    bool m_scheduler_parent_bound{false};
    /// @brief 恢复权门的原子 claim；防止两个线程同时 resume 同一个 Fiber。
    std::atomic<bool> m_resume_claim{false};
    /// @brief 保护元数据拷贝（快照/父链读写）；与 m_failure_mutex 分开，避免锁嵌套。
    mutable std::mutex m_metadata_mutex;
    /// @brief 上次 resume 所在线程的哈希；用于检测跨线程恢复（诊断与断言）。
    std::atomic<std::size_t> m_last_resume_thread_hash{0};
    /// @brief 嵌套 Fiber 的父指针（手工父子链）；顶层为 nullptr。
    /// @details 只在首次 resume 时由 `bind_caller` 写入（写侧仍由
    ///          `m_metadata_mutex` 与 resume claim 串行化），热路径用
    ///          acquire 读它跳过重复的加锁父链刷新。
    std::atomic<Fiber*> m_parent{nullptr};
    /// @brief 父链是否已绑定；未绑定时的挂起会被标记 Failed（防跳错栈）。
    std::atomic<bool> m_parent_bound{false};
    /// @brief 保护 m_failure；initialize_context/entry 捕获异常时写入。
    mutable std::mutex m_failure_mutex;
    /// @brief Fiber 内抛出的异常；由 resume 重新抛给调用方。
    std::exception_ptr m_failure;
    /// @brief 进入 Fiber 时保存的 errno；切回调用方时恢复，避免污染对方。
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
#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
    /// @brief 是否显式开启迁移等待路径（Channel 阻塞收发据此分流）。
    std::atomic<bool> m_migration_enabled{false};
    /// @brief 当前是否登记了一次可迁移的通道等待。
    std::atomic<bool> m_migration_wait_active{false};
    /// @brief 等待中的通道（非拥有指针，仅在挂起期间有效）。
    ChannelMigrationBase* m_migration_channel{nullptr};
    /// @brief 等待节点地址（PendingRecv*，仅用于导出校验）。
    void* m_migration_node{nullptr};
    /// @brief 等待方栈上的 shared_ptr 槽地址（导出为栈内偏移）。
    const void* m_migration_pending_slot{nullptr};
    /// @brief 等待方向：1=recv 2=send。
    std::uint32_t m_migration_kind{0};
#endif
};

Fiber::Fiber(Function function, std::size_t stack_size,
             std::size_t stack_reserve_size, bool stack_growable)
    : m_impl(std::make_unique<Impl>(this, std::move(function), stack_size,
                                   stack_reserve_size, stack_growable)) {}

std::unique_ptr<Fiber> Fiber::AcquireForScheduler(
    Function function, std::size_t stack_size, std::size_t bin_capacity,
    std::size_t stack_reserve_size, bool stack_growable) {
    const auto capacity = std::min(
        kMaxFiberBinCapacity,
        bin_capacity == 0 ? kDefaultFiberBinCapacity : bin_capacity);
    if (capacity != 0 && !s_thread_fiber_bin.entries.empty()) {
        auto fiber = std::move(s_thread_fiber_bin.entries.back());
        s_thread_fiber_bin.entries.pop_back();
        try {
            fiber->m_impl->reset_for_scheduler(
                std::move(function), stack_size, stack_reserve_size,
                stack_growable);
            return fiber;
        } catch (...) {
            // 重绑定失败时销毁该对象并走普通构造路径。失败不会把
            // 一个状态不明的 Fiber 再次放回池中。
        }
    }
    return std::make_unique<Fiber>(std::move(function), stack_size,
                                   stack_reserve_size, stack_growable);
}

void Fiber::RecycleForScheduler(std::unique_ptr<Fiber> fiber,
                                std::size_t bin_capacity) noexcept {
    if (!fiber) {
        return;
    }
    const auto capacity = std::min(
        kMaxFiberBinCapacity,
        bin_capacity == 0 ? kDefaultFiberBinCapacity : bin_capacity);
    const auto state = fiber->state();
    if (capacity == 0 ||
        (state != FiberState::Completed && state != FiberState::Failed) ||
        s_current_fiber == fiber.get() ||
        // 嵌套 Fiber 的子记录会持有 parent shared_ptr。只要仍有外部
        // 持有者，就不能把这个 Fiber 的父记录替换成下一项任务，避免
        // 子 Fiber 之后恢复时观察到被复用的父对象。
        !fiber->m_impl || fiber->m_impl->m_record.use_count() != 1) {
        return;
    }
    try {
        if (s_thread_fiber_bin.entries.size() < capacity) {
            s_thread_fiber_bin.entries.emplace_back(std::move(fiber));
        }
    } catch (...) {
        // FiberBin 只是分配优化；容量不足或 OOM 时由 unique_ptr 正常
        // 销毁对象，FiberStack 仍会回收到原有栈缓存。
    }
}

Fiber::~Fiber() {
    if (!m_impl) {
        return;
    }
    // 不能在 Fiber 自身栈上销毁自身；此约束保留为明确的 API 前置条件。
    if (s_current_fiber == m_impl->m_owner) {
        std::terminate();
    }
    if (m_impl->try_claim_for_destroy()) {
        // 已独占 resume 权：~Impl 直接收尾，不再等待。
        // 注意不能先 m_impl.reset()：~Impl 可能恢复挂起的 Fiber 执行流，
        // 它在自己的栈上通过 Current()->m_impl 回读本对象（例如
        // CancellationRequested）。析构期间 m_impl 必须仍指向有效 Impl，
        // 因此先 delete、再 release 避免 unique_ptr 二次删除。
        Impl* const impl = m_impl.get();
        delete impl;
        (void)m_impl.release();
        return;
    }
    // 另一个线程在整个预算内都没有释放 resume 权（违反「owner 必须长于
    // 所有 resume()」契约）。继续自旋会烧满一个核，继续析构会让那个线程
    // 随后访问已释放内存。这里放弃所有权：写原子墓碑、把 Impl 移入
    // 进程级隔离区，并递增 DestroyTimeoutCount()（FIBER-02）。
    s_destroy_claim_timeouts.fetch_add(1, std::memory_order_relaxed);
    m_impl->on_destroy_timeout();
    Impl::quarantine(std::move(m_impl));
}

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
    // 直接读 TLS：Current() 是 noinline 的导出符号，调用要经 PLT；挂起热
    // 路径上这次多出来的调用没有语义收益（initial-exec 下就是一条 fs 段读）。
    Fiber* const current = s_current_fiber;
    return current != nullptr && current->m_impl->suspend(reason, false);
}

bool Fiber::SuspendForScheduler(SuspendReason reason) noexcept {
    Fiber* const current = s_current_fiber;
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

std::uint64_t Fiber::DestroyTimeoutCount() noexcept {
    return s_destroy_claim_timeouts.load(std::memory_order_relaxed);
}

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
    // record 不再被替换（FIBER-03），指针比较即可识别「本 Fiber 的记录」，
    // 无需在锁外读会被 reset 更新的 id。
    const FiberRecord* const self_record = m_impl->m_record.get();
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
            frame.active = cursor.get() == self_record && Current() == this;
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
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        PublishFiberDebugMeta(*cursor);
#endif
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
    m_impl->m_debug_gid.store(binding.task_id, std::memory_order_relaxed);
}

FiberStackDebugSnapshot Fiber::debug_stack() const {
    FiberStackDebugSnapshot snapshot;
    const auto record = m_impl->m_record;
    {
        std::lock_guard<std::mutex> lock(record->mutex);
        snapshot.id = record->id;
        snapshot.state = record->state.load(std::memory_order_acquire);
        snapshot.suspend_reason = record->reason.load(std::memory_order_acquire);
        snapshot.parent_id = record->parent == nullptr ? 0 : record->parent->id;
        snapshot.active_parent_id = record->active_parent_id;
        snapshot.nesting_depth = record->depth;
        // 栈请求参数在 record 里复制过，锁内读取不需要碰 Impl。
        snapshot.requested_bytes = record->requested_stack_bytes;
        snapshot.reserve_bytes = record->reserve_stack_bytes;
    }
    // 只有执行本 Fiber 的线程能保证栈在其脚下存活：完成路径（resume_locked）
    // 会在切回调用者后 reset m_stack，另一个线程先取裸指针再解引用就是
    // use-after-free（FIBER-01）。因此非当前 Fiber 一律不读栈字段。
    if (Current() != this) {
        return snapshot;
    }
    FiberStack* stack = m_impl->m_stack.get();
    if (stack != nullptr) {
        snapshot.has_stack = true;
        snapshot.growable = stack->growable();
        snapshot.reserve_bytes = stack->reserve_size();
        snapshot.usable_bytes = stack->usable_size();
        snapshot.mapping_base =
            reinterpret_cast<std::uintptr_t>(stack->growth_base());
        snapshot.mapping_top =
            reinterpret_cast<std::uintptr_t>(stack->growth_top());
        snapshot.committed_begin = stack->growth_committed();
        if (s_current_fiber == this) {
            // 当前线程正在执行的 Fiber 才能读到有意义的栈帧地址；
            // 其它 Fiber 返回 0，不伪造 sp。
            snapshot.current_sp =
                reinterpret_cast<std::uintptr_t>(&snapshot);
        }
    }
    return snapshot;
}

std::size_t Fiber::nesting_depth() const noexcept {
    std::lock_guard<std::mutex> lock(m_impl->m_record->mutex);
    return m_impl->m_record->depth;
}

#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
// ===========================================================================
// 跨进程迁移：Fiber 导出 / 导入
//
// 设计要点（与 docs/fiber-migration.md §11 对应）：
//   1. 复用 tools/migration 的 G2CMIG03（v3）快照格式、CRC 与 HMAC-SHA256；
//   2. 栈内指针分三类重定位：栈内偏移 / 代码地址（模块 + 模块内偏移）/
//      已登记堆对象（Fiber、Impl、FiberRecord、Channel、等待节点与控制块）；
//   3. 任何未登记的堆/映射指针都在导出时拒绝（安全边界，不猜测）；
//   4. 代码地址不用裸地址也不用符号表名，而是「模块真实路径 + 模块内偏移
//      + 模块 build-id」。同 build-id 是恢复的前置条件，跨版本明确拒绝。
//
// 恢复点位于 `Impl::suspend()` 内 `JumpContext` 返回处：导入时把保存区
// 放到新栈的同一相对位置，7 个 callee-saved 寄存器与返回地址都按上述
// 规则重写；随后 `resume()` 直接跳回该点，`suspend()` 的收尾代码会把
// `m_caller` 刷新成本进程的调用者上下文。
// ===========================================================================
namespace {

/// 导出时登记的可重定位对象类别。
enum class MigrationObjectKind : std::uint32_t {
    kFiber = 1,
    kImpl = 2,
    kRecord = 3,
    kChannel = 4,
    kWaitNode = 5,
    kWaitControl = 6,
    kStaleCaller = 7,
    kWaitSlot = 8,
};

constexpr char kMigrationHandleFiber[] = "go2cpp::Fiber";
constexpr char kMigrationHandleImpl[] = "go2cpp::Fiber::Impl";
constexpr char kMigrationHandleRecord[] = "go2cpp::FiberRecord";
constexpr char kMigrationHandleChannel[] = "go2cpp::Channel";
constexpr char kMigrationHandleWaitNode[] = "go2cpp::ChannelWaitNode";
constexpr char kMigrationHandleWaitControl[] = "go2cpp::ChannelWaitControl";
constexpr char kMigrationHandleStaleCaller[] = "go2cpp::StaleCaller";
constexpr char kMigrationHandleWaitSlot[] = "go2cpp::ChannelWaitSlot";
constexpr char kMigrationHandleCleared[] = "go2cpp::ClearedHeapSlot";
constexpr char kMigrationModulePrefix[] = "module:";

/// 一个已加载模块的可执行段与 build-id。
struct MigrationModuleSpan {
    std::string key;          ///< `module:<realpath>`，跨进程稳定键
    std::string path;         ///< 模块文件路径（读取 build-id 用）
    std::uintptr_t base{0};   ///< 模块加载基址（ASLR 后的 dlpi_addr）
    /// 所有 PT_LOAD 段的 [begin, end)。代码与数据都按「模块 + 偏移」重定位：
    /// 栈上出现 libc 的 `stdout`/locale 等数据段指针时，同 build-id 的新进程
    /// 里同一偏移仍然指向同一个对象，不需要拒绝导出。
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> load_ranges;
};

/// 导出时登记的一个堆对象（用于栈槽匹配与句柄生成）。
struct MigrationObjectSpan {
    std::uint32_t handle_id{0};
    MigrationObjectKind kind{MigrationObjectKind::kFiber};
    std::uint64_t start{0};
    std::uint64_t size{0};
};

std::string MigrationReadSelfExecutablePath() {
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    buffer[length] = '\0';
    return std::string(buffer, static_cast<std::size_t>(length));
}

std::string MigrationNormalizeModulePath(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    char resolved[PATH_MAX];
    if (::realpath(path.c_str(), resolved) != nullptr) {
        return std::string(resolved);
    }
    return path;
}

int MigrationCollectModule(struct dl_phdr_info* info, std::size_t, void* data) {
    auto* spans = static_cast<std::vector<MigrationModuleSpan>*>(data);
    if (info == nullptr || spans == nullptr) {
        return 0;
    }
    std::string path =
        info->dlpi_name != nullptr ? std::string(info->dlpi_name) : std::string();
    if (path.empty()) {
        path = MigrationReadSelfExecutablePath();
    }
    path = MigrationNormalizeModulePath(path);
    if (path.empty()) {
        return 0;
    }
    MigrationModuleSpan span;
    span.key = std::string(kMigrationModulePrefix) + path;
    span.path = path;
    span.base = static_cast<std::uintptr_t>(info->dlpi_addr);
    bool has_load = false;
    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const Elf64_Phdr& header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || header.p_memsz == 0) {
            continue;
        }
        const auto begin = span.base + static_cast<std::uintptr_t>(header.p_vaddr);
        const auto end = begin + static_cast<std::uintptr_t>(header.p_memsz);
        span.load_ranges.emplace_back(begin, end);
        has_load = true;
    }
    if (has_load) {
        spans->push_back(std::move(span));
    }
    return 0;
}

std::vector<MigrationModuleSpan> MigrationEnumerateModules() {
    std::vector<MigrationModuleSpan> spans;
    ::dl_iterate_phdr(&MigrationCollectModule, &spans);
    return spans;
}

const MigrationModuleSpan* MigrationFindModuleForAddress(
    const std::vector<MigrationModuleSpan>& modules, std::uint64_t address) {
    for (const auto& module : modules) {
        for (const auto& range : module.load_ranges) {
            if (address >= range.first && address < range.second) {
                return &module;
            }
        }
    }
    return nullptr;
}

std::uint64_t MigrationFnv1a(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/// 读取 ELF 文件的 GNU build-id 并哈希成 u64；失败返回 false。
bool MigrationReadBuildIdHash(const std::string& path, std::uint64_t* hash) {
    if (hash == nullptr) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    Elf64_Ehdr header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phoff == 0) {
        return false;
    }
    for (std::uint16_t index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr program{};
        file.seekg(static_cast<std::streamoff>(
            header.e_phoff + static_cast<std::uint64_t>(index) * sizeof(Elf64_Phdr)));
        file.read(reinterpret_cast<char*>(&program), sizeof(program));
        if (!file || program.p_type != PT_NOTE || program.p_filesz == 0) {
            continue;
        }
        std::vector<char> notes(static_cast<std::size_t>(program.p_filesz));
        file.seekg(static_cast<std::streamoff>(program.p_offset));
        file.read(notes.data(), static_cast<std::streamsize>(notes.size()));
        if (!file) {
            continue;
        }
        std::size_t offset = 0;
        while (offset + sizeof(Elf64_Nhdr) <= notes.size()) {
            Elf64_Nhdr note{};
            std::memcpy(&note, notes.data() + offset, sizeof(note));
            offset += sizeof(note);
            const std::size_t name_size = (note.n_namesz + 3U) & ~static_cast<std::size_t>(3U);
            const std::size_t desc_size = (note.n_descsz + 3U) & ~static_cast<std::size_t>(3U);
            if (offset + name_size + desc_size > notes.size()) {
                break;
            }
            const char* name = notes.data() + offset;
            const auto* desc = reinterpret_cast<const std::uint8_t*>(
                notes.data() + offset + name_size);
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                std::memcmp(name, "GNU", 4) == 0) {
                *hash = MigrationFnv1a(desc, note.n_descsz);
                return true;
            }
            offset += name_size + desc_size;
        }
    }
    return false;
}

bool MigrationReadMappedRanges(
    std::vector<std::pair<std::uint64_t, std::uint64_t>>* ranges) {
    std::ifstream maps("/proc/self/maps");
    if (!maps || ranges == nullptr) {
        return false;
    }
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long begin = 0;
        unsigned long long end = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx", &begin, &end) == 2 && end > begin) {
            ranges->emplace_back(begin, end);
        }
    }
    return true;
}

bool MigrationInMappedRanges(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
    std::uint64_t value) {
    for (const auto& range : ranges) {
        if (value >= range.first && value < range.second) {
            return true;
        }
    }
    return false;
}

std::string MigrationHexU64(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

bool MigrationParseHexU64(const std::string& text, std::uint64_t* value) {
    if (text.empty() || value == nullptr) {
        return false;
    }
    std::uint64_t result = 0;
    for (char character : text) {
        std::uint32_t digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<std::uint32_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<std::uint32_t>(character - 'a') + 10U;
        } else if (character >= 'A' && character <= 'F') {
            digit = static_cast<std::uint32_t>(character - 'A') + 10U;
        } else {
            return false;
        }
        result = (result << 4U) | digit;
    }
    *value = result;
    return true;
}

std::vector<std::uint8_t> MigrationEncodeU64(std::uint64_t value) {
    std::vector<std::uint8_t> payload(sizeof(value));
    std::memcpy(payload.data(), &value, sizeof(value));
    return payload;
}

std::uint64_t MigrationDecodeU64(const std::uint8_t* data, std::size_t size,
                                 std::size_t offset) {
    if (data == nullptr || offset + sizeof(std::uint64_t) > size) {
        return 0;
    }
    std::uint64_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

const char* MigrationHandleType(MigrationObjectKind kind) {
    switch (kind) {
    case MigrationObjectKind::kFiber:
        return kMigrationHandleFiber;
    case MigrationObjectKind::kImpl:
        return kMigrationHandleImpl;
    case MigrationObjectKind::kRecord:
        return kMigrationHandleRecord;
    case MigrationObjectKind::kChannel:
        return kMigrationHandleChannel;
    case MigrationObjectKind::kWaitNode:
        return kMigrationHandleWaitNode;
    case MigrationObjectKind::kWaitControl:
        return kMigrationHandleWaitControl;
    case MigrationObjectKind::kStaleCaller:
        return kMigrationHandleStaleCaller;
    case MigrationObjectKind::kWaitSlot:
        return kMigrationHandleWaitSlot;
    }
    return "go2cpp::Unknown";
}

std::size_t MigrationRoundUpStackSize(std::size_t size) {
    const long page = ::sysconf(_SC_PAGESIZE);
    const auto page_size = page > 0 ? static_cast<std::size_t>(page) : 4096U;
    const std::size_t aligned = (size + page_size - 1U) & ~(page_size - 1U);
    return aligned == 0 ? page_size : aligned;
}

}  // namespace

bool Fiber::EnableMigration() noexcept {
    if (!m_impl) {
        return false;
    }
    m_impl->m_migration_enabled.store(true, std::memory_order_release);
    return true;
}

bool Fiber::migration_enabled() const noexcept {
    return m_impl && m_impl->m_migration_enabled.load(std::memory_order_acquire);
}

bool Fiber::BeginMigrationWait(ChannelMigrationBase* channel, void* node,
                               const void* pending_slot,
                               std::uint32_t kind) noexcept {
    if (!m_impl || channel == nullptr || node == nullptr) {
        return false;
    }
    m_impl->m_migration_channel = channel;
    m_impl->m_migration_node = node;
    m_impl->m_migration_pending_slot = pending_slot;
    m_impl->m_migration_kind = kind;
    m_impl->m_migration_wait_active.store(true, std::memory_order_release);
    return true;
}

void Fiber::EndMigrationWait() noexcept {
    if (!m_impl) {
        return;
    }
    m_impl->m_migration_wait_active.store(false, std::memory_order_release);
    m_impl->m_migration_channel = nullptr;
    m_impl->m_migration_node = nullptr;
    m_impl->m_migration_pending_slot = nullptr;
    m_impl->m_migration_kind = 0;
}

bool Fiber::ExportMigrationImage(const std::string& path,
                                 const FiberMigrationOptions& options,
                                 std::string* error) const {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (!m_impl) {
        return fail("Fiber 对象无效");
    }
    if (!m_impl->m_migration_enabled.load(std::memory_order_acquire)) {
        return fail("Fiber 未开启迁移路径：请先在 Fiber 内调用 EnableMigration()");
    }
    if (!m_impl->m_migration_wait_active.load(std::memory_order_acquire)) {
        return fail("Fiber 当前不在可迁移的通道等待中（只支持等待 channel 的挂起状态）");
    }
    if (m_impl->m_migration_kind != 1U) {
        return fail("当前只支持接收等待（<-ch）的迁移，发送等待明确拒绝");
    }
    if (m_impl->m_record->state.load(std::memory_order_acquire) !=
        FiberState::Suspended) {
        return fail("Fiber 不是挂起状态，不能导出迁移镜像");
    }
    ChannelMigrationBase* const channel = m_impl->m_migration_channel;
    FiberStack* const stack = m_impl->m_stack.get();
    const auto context_address =
        reinterpret_cast<std::uint64_t>(m_impl->m_context);
    if (channel == nullptr || stack == nullptr || context_address == 0) {
        return fail("Fiber 缺少迁移所需的通道、栈或上下文");
    }
    const auto bottom = reinterpret_cast<std::uint64_t>(stack->bottom());
    const auto top = reinterpret_cast<std::uint64_t>(stack->stack_pointer());
    const auto usable = top - bottom;
    if (context_address < bottom || context_address + 8U * 7U > top) {
        return fail("上下文保存区不在 Fiber 栈内（Fiber 可能已经结束）");
    }

    ChannelWaitState wait;
    std::string wait_error;
    if (!channel->ExportWaitState(&wait, &wait_error)) {
        return fail("通道等待状态导出失败：" + wait_error);
    }
    wait.pending_slot =
        reinterpret_cast<std::uint64_t>(m_impl->m_migration_pending_slot);
    if (wait.node_address == 0 ||
        wait.node_address !=
            reinterpret_cast<std::uint64_t>(m_impl->m_migration_node)) {
        return fail("通道等待节点与 Fiber 迁移登记不一致");
    }
    if (wait.pending_slot == 0 ||
        wait.pending_slot < bottom ||
        wait.pending_slot + 2U * sizeof(std::uint64_t) > top) {
        return fail("等待方栈上的 shared_ptr 槽不在 Fiber 栈内");
    }

    std::vector<migration::HandleRecord> handles;
    const auto add_handle = [&handles](const char* type, std::uint64_t old_address,
                                       std::uint64_t object_size,
                                       std::vector<std::uint8_t> payload) {
        migration::HandleRecord record{};
        record.id = static_cast<std::uint32_t>(handles.size()) + 1U;
        record.old_address = old_address;
        record.object_size = object_size;
        record.type = type;
        record.payload = std::move(payload);
        handles.push_back(std::move(record));
        return handles.back().id;
    };

    std::vector<MigrationObjectSpan> objects;
    const auto add_object = [&handles, &objects, &add_handle](
                                MigrationObjectKind kind, std::uint64_t start,
                                std::uint64_t size,
                                std::vector<std::uint8_t> payload) {
        MigrationObjectSpan span;
        span.kind = kind;
        span.start = start;
        span.size = size;
        span.handle_id =
            add_handle(MigrationHandleType(kind), start, size, std::move(payload));
        objects.push_back(span);
    };

    add_object(MigrationObjectKind::kFiber, reinterpret_cast<std::uint64_t>(this),
               sizeof(Fiber), {});
    add_object(MigrationObjectKind::kImpl,
               reinterpret_cast<std::uint64_t>(m_impl.get()), sizeof(Impl), {});
    add_object(MigrationObjectKind::kRecord,
               reinterpret_cast<std::uint64_t>(m_impl->m_record.get()),
               sizeof(FiberRecord), {});
    {
        std::vector<std::uint8_t> payload(3U * sizeof(std::uint64_t));
        const std::uint64_t object_size = channel->MigrationObjectSize();
        const std::uint64_t node_size = channel->MigrationNodeSize();
        const std::uint64_t value_bytes = wait.value_bytes;
        std::memcpy(payload.data(), &object_size, sizeof(object_size));
        std::memcpy(payload.data() + sizeof(std::uint64_t), &node_size,
                    sizeof(node_size));
        std::memcpy(payload.data() + 2U * sizeof(std::uint64_t), &value_bytes,
                    sizeof(value_bytes));
        // 用 Channel<T> 本体的地址（快照里的栈槽保存的是 Channel<T>*），
        // 不能用 ChannelMigrationBase* 子对象地址。
        add_object(MigrationObjectKind::kChannel, wait.channel_address,
                   object_size, std::move(payload));
    }
    add_object(MigrationObjectKind::kWaitNode, wait.node_address,
               channel->MigrationNodeSize(), {});
    add_object(MigrationObjectKind::kWaitControl, wait.control_block, 64U, {});
    if (m_impl->m_caller != nullptr) {
        add_object(MigrationObjectKind::kStaleCaller,
                   reinterpret_cast<std::uint64_t>(m_impl->m_caller), 8U, {});
    }
    add_object(MigrationObjectKind::kWaitSlot, wait.pending_slot,
               sizeof(void*), MigrationEncodeU64(wait.pending_slot - context_address));

    const auto modules = MigrationEnumerateModules();
    std::vector<migration::SymbolRecord> symbols;
    std::map<std::string, std::uint32_t> module_ids;
    const auto intern_module = [&modules, &symbols, &module_ids](
                                   const MigrationModuleSpan& module) {
        const auto found = module_ids.find(module.key);
        if (found != module_ids.end()) {
            return found->second;
        }
        std::uint64_t build_id = 0;
        if (!MigrationReadBuildIdHash(module.path, &build_id)) {
            build_id = 0;
        }
        migration::SymbolRecord record{};
        record.id = static_cast<std::uint32_t>(symbols.size()) + 1U;
        record.kind = migration::kSymbolData;
        record.size = 0;
        record.name = module.key;
        record.display = MigrationHexU64(build_id);
        symbols.push_back(record);
        module_ids[module.key] = record.id;
        return record.id;
    };

    std::vector<std::pair<std::uint64_t, std::uint64_t>> mapped_ranges;
    if (!MigrationReadMappedRanges(&mapped_ranges)) {
        return fail("无法读取 /proc/self/maps，指针审计不可用");
    }

    std::vector<migration::Relocation> relocations;
    migration::PointerAudit audit{};
    for (std::uint64_t slot = context_address; slot + 8U <= top; slot += 8U) {
        std::uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(slot), sizeof(value));
        ++audit.slots;
        if (value == 0) {
            continue;
        }
        migration::Relocation relocation{};
        relocation.slot_offset = slot - context_address;
        if (value >= bottom && value < top) {
            relocation.kind = migration::kRelocStack;
            relocation.addend = static_cast<std::int64_t>(value - context_address);
            ++audit.stack_ptrs;
            relocations.push_back(relocation);
            continue;
        }
        const MigrationObjectSpan* matched = nullptr;
        for (const auto& object : objects) {
            if (value >= object.start && value < object.start + object.size) {
                matched = &object;
                break;
            }
        }
        if (matched != nullptr) {
            relocation.kind = migration::kRelocHandle;
            relocation.target_id = matched->handle_id;
            relocation.addend =
                static_cast<std::int64_t>(value - matched->start);
            ++audit.handle_ptrs;
            relocations.push_back(relocation);
            continue;
        }
        const MigrationModuleSpan* const module =
            MigrationFindModuleForAddress(modules, value);
        if (module != nullptr) {
            relocation.kind = migration::kRelocSymbol;
            relocation.target_id = intern_module(*module);
            relocation.addend =
                static_cast<std::int64_t>(value - module->base);
            ++audit.symbol_ptrs;
            ++audit.code_ptrs;
            relocations.push_back(relocation);
            continue;
        }
        if (MigrationInMappedRanges(mapped_ranges, value)) {
            ++audit.heap_ptrs;
            if (options.allow_dead_heap_slots) {
                // 受控模式：登记为「清零槽」。恢复后该槽为 0；代码若真的
                // 使用它会在解引用处立即崩溃，而不是访问旧进程的地址。
                const std::uint32_t handle_id = add_handle(
                    kMigrationHandleCleared, value, sizeof(std::uint64_t),
                    MigrationEncodeU64(value));
                relocation.kind = migration::kRelocHandle;
                relocation.target_id = handle_id;
                relocation.addend = 0;
                relocations.push_back(relocation);
                continue;
            }
            std::ostringstream message;
            message << "栈上存在未登记的堆/映射指针 0x" << std::hex << value
                    << std::dec << "（槽位相对上下文 +0x" << std::hex
                    << (slot - context_address) << std::dec
                    << "）：拒绝导出，避免恢复侧执行悬空指针。"
                       "若该槽确认是容器内部的死值，可在导出选项里显式打开 "
                       "allow_dead_heap_slots（恢复后该槽为 0）。";
            return fail(message.str());
        }
        ++audit.other_ptrs;
    }

    migration::Snapshot snapshot;
    snapshot.fiber_id = m_impl->m_id;
    snapshot.stack_base_va = bottom;
    snapshot.stack_va = top;
    snapshot.context_va = context_address;
    snapshot.stack_size = usable;
    snapshot.stack_bytes = top - context_address;
    snapshot.stack.resize(static_cast<std::size_t>(snapshot.stack_bytes));
    std::memcpy(snapshot.stack.data(),
                reinterpret_cast<const void*>(context_address),
                snapshot.stack.size());
    std::uint64_t registers[7] = {0, 0, 0, 0, 0, 0, 0};
    std::memcpy(registers, reinterpret_cast<const void*>(context_address),
                sizeof(registers));
    snapshot.registers.r12 = registers[0];
    snapshot.registers.r13 = registers[1];
    snapshot.registers.r14 = registers[2];
    snapshot.registers.r15 = registers[3];
    snapshot.registers.rbx = registers[4];
    snapshot.registers.rbp = registers[5];
    snapshot.registers.rip = registers[6];

    snapshot.channel.va = wait.channel_address;
    snapshot.channel.capacity = 0;
    snapshot.channel.count = 0;
    snapshot.channel.closed = 0;
    snapshot.channel.head_offset = 0;
    snapshot.channel.tail_offset = 0;
    snapshot.channel_va = wait.channel_address;
    snapshot.waiter_va = wait.node_address;
    snapshot.wait_kind = migration::kWaitChannelRecv;

    snapshot.fiber.ctx = context_address;
    snapshot.fiber.caller = reinterpret_cast<std::uint64_t>(m_impl->m_caller);
    snapshot.fiber.stack_base = bottom;
    snapshot.fiber.stack_size = usable;
    snapshot.fiber.top = top;
    snapshot.fiber.fiber_id = m_impl->m_id;
    snapshot.fiber.state = 2;  // Parked：ReadSnapshotFile 强制要求
    snapshot.fiber.fn = 0;
    snapshot.fiber.channel = wait.channel_address;
    snapshot.fiber.waiter = wait.node_address;
    snapshot.fiber.ctx_offset = context_address - bottom;
    snapshot.fiber.waiter_offset = 0;
    snapshot.fiber.safepoint_id = 0;
    snapshot.fiber.fn_symbol_id = 0;
    snapshot.fiber.channel_symbol_id = 0;

    snapshot.audit = audit;
    snapshot.relocations = std::move(relocations);
    snapshot.handles = std::move(handles);
    snapshot.symbols = std::move(symbols);
    snapshot.symbol_epoch = migration::kSymbolEpoch;
    snapshot.build_id_hash = migration::SelfBuildIdHash();
    snapshot.safepoint_slots.clear();
    snapshot.frames.clear();
    {
        std::vector<std::string> names;
        names.reserve(snapshot.symbols.size());
        for (const auto& symbol : snapshot.symbols) {
            names.push_back(symbol.name);
        }
        snapshot.symbol_hash = migration::SymbolTable::HashNames(names);
    }
    {
        std::vector<std::string> manifest;
        manifest.reserve(modules.size());
        for (const auto& module : modules) {
            manifest.push_back(module.key);
        }
        snapshot.symbol_manifest = manifest;
        snapshot.symbol_table_hash =
            migration::SymbolTable::HashNames(manifest);
    }
    snapshot.flags = migration::kFlagCrossProcessReady |
                     migration::kFlagSymbolized;
    if (audit.code_ptrs != 0) {
        snapshot.flags |= migration::kFlagHasCodePointers;
    }
    if (audit.stack_ptrs != 0) {
        snapshot.flags |= migration::kFlagHasStackPointers;
    }
    if (!snapshot.handles.empty()) {
        snapshot.flags |= migration::kFlagHasHandles;
    }
    {
        std::ostringstream summary;
        summary << "go2cpp Fiber 迁移镜像 v3：fiber#" << m_impl->m_id
                << "，栈 " << snapshot.stack_bytes << " 字节，"
                << snapshot.symbols.size() << " 个模块、"
                << snapshot.relocations.size() << " 条重定位、"
                << snapshot.handles.size() << " 个句柄；指针审计 stack="
                << audit.stack_ptrs << " symbol=" << audit.symbol_ptrs
                << " code=" << audit.code_ptrs << " heap=" << audit.heap_ptrs
                << " handle=" << audit.handle_ptrs
                << " other=" << audit.other_ptrs;
        snapshot.summary = summary.str();
    }

    std::string write_error;
    if (!migration::WriteSnapshotFile(path, snapshot, &write_error)) {
        return fail("写入迁移镜像失败：" + write_error);
    }
    return true;
}

std::unique_ptr<Fiber> Fiber::ImportMigrationImage(
    const std::string& path, const FiberMigrationOptions& options,
    std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return std::unique_ptr<Fiber>();
    };

    migration::Snapshot snapshot;
    std::string read_error;
    if (!migration::ReadSnapshotFile(path, &snapshot, &read_error)) {
        return fail("读取迁移镜像失败：" + read_error);
    }
    (void)options.verify_signature;
    if (options.verify_build_id) {
        // 读取层已经做过一次；这里再显式检查一次，保证错误信息明确指向
        // 「可执行文件不同」而不是后续的模块表缺失。
        const std::uint64_t self_build = migration::SelfBuildIdHash();
        if (snapshot.build_id_hash != 0 && self_build != 0 &&
            snapshot.build_id_hash != self_build) {
            return fail("快照的可执行文件 build-id 与当前进程不匹配"
                        "（代码布局不同，拒绝恢复）");
        }
    }
    if (snapshot.wait_kind != migration::kWaitNone &&
        snapshot.wait_kind != migration::kWaitChannelRecv) {
        return fail("当前只支持接收等待（<-ch）的迁移镜像");
    }
    if (snapshot.stack_bytes == 0 || snapshot.stack.empty()) {
        return fail("迁移镜像没有 Fiber 栈内容");
    }
    if (snapshot.stack_bytes > snapshot.stack_size) {
        return fail("迁移镜像的栈使用量超过栈大小");
    }
    if (snapshot.wait_kind != migration::kWaitNone && options.channel == nullptr) {
        return fail("迁移镜像包含通道等待状态，导入选项必须提供 channel");
    }

    const auto modules = MigrationEnumerateModules();
    std::map<std::string, const MigrationModuleSpan*> module_by_key;
    for (const auto& module : modules) {
        module_by_key[module.key] = &module;
    }
    std::map<std::uint32_t, std::uint64_t> module_bases;
    for (const auto& symbol : snapshot.symbols) {
        if (symbol.name.rfind(kMigrationModulePrefix, 0) != 0) {
            continue;
        }
        const auto found = module_by_key.find(symbol.name);
        if (found == module_by_key.end()) {
            return fail("迁移镜像引用的模块未加载：" + symbol.name);
        }
        if (!symbol.display.empty()) {
            std::uint64_t expected = 0;
            std::uint64_t actual = 0;
            if (!MigrationParseHexU64(symbol.display, &expected)) {
                return fail("迁移镜像的模块 build-id 字段损坏：" + symbol.name);
            }
            if (expected != 0) {
                if (!MigrationReadBuildIdHash(found->second->path, &actual) ||
                    actual != expected) {
                    return fail("模块 build-id 不匹配（版本漂移，拒绝恢复）：" +
                                symbol.name);
                }
            }
        }
        module_bases[symbol.id] = found->second->base;
    }

    const std::size_t usable = MigrationRoundUpStackSize(
        std::max<std::size_t>(Fiber::DefaultStackSize(),
                              static_cast<std::size_t>(snapshot.stack_bytes)));
#if defined(GO2CPP_USE_NATIVE_CONTEXT)
    auto stack = std::make_unique<FiberStack>(usable, 0U, false);
#else
    auto stack = std::make_unique<FiberStack>(usable);
#endif
    const auto new_top = reinterpret_cast<std::uint64_t>(stack->stack_pointer());
    const auto new_bottom = reinterpret_cast<std::uint64_t>(stack->bottom());
    if (new_top - new_bottom < snapshot.stack_bytes) {
        return fail("新栈容量不足");
    }
    const std::uint64_t new_context = new_top - snapshot.stack_bytes;
    std::memcpy(reinterpret_cast<void*>(new_context), snapshot.stack.data(),
                snapshot.stack.size());

    auto fiber = std::unique_ptr<Fiber>(new Fiber(Function{}));
    Impl* const impl = fiber->m_impl.get();

    std::map<std::uint32_t, std::uint64_t> handle_addresses;
    std::uint64_t pending_slot = 0;
    const migration::HandleRecord* node_handle = nullptr;
    const migration::HandleRecord* control_handle = nullptr;
    std::uint64_t channel_value_bytes = 0;
    for (const auto& handle : snapshot.handles) {
        if (handle.type == kMigrationHandleFiber) {
            handle_addresses[handle.id] =
                reinterpret_cast<std::uint64_t>(fiber.get());
        } else if (handle.type == kMigrationHandleImpl) {
            handle_addresses[handle.id] = reinterpret_cast<std::uint64_t>(impl);
        } else if (handle.type == kMigrationHandleRecord) {
            handle_addresses[handle.id] =
                reinterpret_cast<std::uint64_t>(impl->m_record.get());
        } else if (handle.type == kMigrationHandleChannel) {
            if (options.channel == nullptr) {
                return fail("迁移镜像包含通道等待状态，导入选项必须提供 channel");
            }
            if (handle.payload.size() < 3U * sizeof(std::uint64_t)) {
                return fail("通道句柄内容损坏");
            }
            const std::uint64_t expected_size =
                MigrationDecodeU64(handle.payload.data(), handle.payload.size(), 0);
            if (expected_size != options.channel->MigrationObjectSize()) {
                return fail("通道对象大小不匹配（等待值类型或通道实现不同）");
            }
            channel_value_bytes = MigrationDecodeU64(
                handle.payload.data(), handle.payload.size(), 2U * sizeof(std::uint64_t));
            handle_addresses[handle.id] =
                options.channel->MigrationObjectAddress();
        } else if (handle.type == kMigrationHandleStaleCaller ||
                   handle.type == kMigrationHandleCleared) {
            handle_addresses[handle.id] = 0;
        } else if (handle.type == kMigrationHandleWaitSlot) {
            if (handle.payload.size() < sizeof(std::uint64_t)) {
                return fail("等待槽句柄内容损坏");
            }
            const std::uint64_t offset =
                MigrationDecodeU64(handle.payload.data(), handle.payload.size(), 0);
            if (offset + 2U * sizeof(std::uint64_t) > snapshot.stack_bytes) {
                return fail("等待槽偏移超出栈范围");
            }
            pending_slot = new_context + offset;
        } else if (handle.type == kMigrationHandleWaitNode) {
            node_handle = &handle;
        } else if (handle.type == kMigrationHandleWaitControl) {
            control_handle = &handle;
        } else {
            return fail("未知的迁移句柄类型：" + handle.type);
        }
    }

    if (snapshot.wait_kind != migration::kWaitNone) {
        if (node_handle == nullptr || control_handle == nullptr) {
            return fail("迁移镜像缺少等待节点/控制块句柄");
        }
        if (options.channel == nullptr) {
            return fail("迁移镜像包含通道等待状态，导入选项必须提供 channel");
        }
        ChannelWaitState state;
        state.kind = 1;
        state.channel_address = snapshot.channel_va;
        state.node_address = node_handle->old_address;
        state.control_block = control_handle->old_address;
        state.pending_slot = pending_slot;
        state.value_bytes = channel_value_bytes;
        ChannelWaitState adopted;
        std::string adopt_error;
        if (!options.channel->AdoptWaitState(state, &adopted, &adopt_error)) {
            return fail("把等待者挂回通道失败：" + adopt_error);
        }
        handle_addresses[node_handle->id] = adopted.node_address;
        handle_addresses[control_handle->id] = adopted.control_block;
        options.channel->ReleaseMigrationHold();
    }

    for (const auto& relocation : snapshot.relocations) {
        if (relocation.slot_offset + sizeof(std::uint64_t) > snapshot.stack_bytes) {
            return fail("重定位槽偏移超出栈范围");
        }
        std::uint64_t value = 0;
        if (relocation.kind == migration::kRelocStack) {
            value = new_context + static_cast<std::uint64_t>(relocation.addend);
        } else if (relocation.kind == migration::kRelocSymbol) {
            const auto found = module_bases.find(relocation.target_id);
            if (found == module_bases.end()) {
                return fail("重定位引用了未解析的模块 id " +
                            std::to_string(relocation.target_id));
            }
            value = found->second + static_cast<std::uint64_t>(relocation.addend);
        } else if (relocation.kind == migration::kRelocHandle) {
            const auto found = handle_addresses.find(relocation.target_id);
            if (found == handle_addresses.end()) {
                return fail("重定位引用了未重建的句柄 id " +
                            std::to_string(relocation.target_id));
            }
            if (relocation.addend < 0) {
                return fail("句柄重定位的偏移为负");
            }
            value = found->second + static_cast<std::uint64_t>(relocation.addend);
        } else {
            return fail("未知的重定位类型 " + std::to_string(relocation.kind));
        }
        std::memcpy(reinterpret_cast<void*>(new_context + relocation.slot_offset),
                    &value, sizeof(value));
    }

    impl->m_stack = std::move(stack);
    // 迁移导入的栈同样需要固化实际增长属性；否则 resume 热路径会认为
    // 这不是可增长栈，SIGSEGV 处理器拿不到正确的栈边界。
    impl->m_stack_growable_active = impl->m_stack->growable();
    impl->m_stack_size = usable;
    impl->m_context = reinterpret_cast<void*>(static_cast<std::uintptr_t>(new_context));
    impl->m_caller = nullptr;
    impl->m_migration_enabled.store(true, std::memory_order_release);
    impl->m_migration_wait_active.store(false, std::memory_order_release);
    impl->m_migration_channel = nullptr;
    impl->m_migration_node = nullptr;
    impl->m_migration_pending_slot = nullptr;
    impl->m_migration_kind = 0;
    impl->m_record->state.store(FiberState::Suspended, std::memory_order_release);
    impl->m_record->reason.store(SuspendReason::Park, std::memory_order_release);
    impl->m_record->alive.store(true, std::memory_order_release);
    return fiber;
}

#endif  // GO2CPP_MIGRATION_API

// ===========================================================================
// Fiber 链样本（debug）：父链节点 = gid / 状态 / 任务函数 / 栈范围
//
// 放在 fiber.cpp 的原因：只有这里能读到 FiberRecord 的内部元数据；样本表
// 也随 go2cpp_fiber 一起导出，debug_dump 视图通过 LookupFiberChain 读取。
// ===========================================================================
namespace debug {
namespace {

constexpr std::uint32_t kDefaultChainEvery = 8;

struct ChainTable {
    std::mutex mutex;
    std::vector<FiberChainSample> samples; ///< index = id - 1
    std::unordered_map<std::uint64_t, FiberChainId> by_hash;
    std::atomic<std::uint64_t> dropped{0};
};

ChainTable& ChainSamples() {
    static ChainTable* table = new ChainTable();
    return *table;
}

std::uint64_t HashChain(const FiberChainSample& sample) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& node : sample.nodes) {
        mix(node.fiber_id);
        mix(node.gid);
        mix(node.state);
        mix(node.depth);
        mix(node.stack_low);
        mix(node.stack_high);
        for (const char ch : node.function) {
            hash ^= static_cast<unsigned char>(ch);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

std::uint64_t SteadyNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uint32_t ThreadHash() noexcept {
    return static_cast<std::uint32_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

}  // namespace

const char* FiberStateNameForDebug(std::uint8_t state) noexcept {
    switch (static_cast<FiberState>(state)) {
        case FiberState::Ready: return "Ready";
        case FiberState::Running: return "Running";
        case FiberState::Suspended: return "Suspended";
        case FiberState::Completed: return "Completed";
        case FiberState::Failed: return "Failed";
    }
    return "kUnknown";
}

namespace {

/// 采样周期按 2^shift 放大（v7：链捕获也受全局采样位移约束）。
std::uint32_t ScaleChainEvery(std::uint32_t every,
                              std::uint32_t shift) noexcept {
    if (every <= 1U) {
        every = 1U;
    }
    if (shift == 0U) {
        return every;
    }
    const std::uint32_t cap = 0xFFFFFFFFU >> shift;
    return every > cap ? 0xFFFFFFFFU : (every << shift);
}

/// 链捕获核心（采样判定已在入口完成）。
FiberChainId CaptureFiberChainCore(std::uint64_t gid) noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    try {
        Fiber* const current = Fiber::Current();
        FiberContextSnapshot frames = current != nullptr
                                          ? current->context_snapshot()
                                          : Fiber::CurrentContextSnapshot();
        if (frames.size() > kMaxFiberChainNodes) {
            frames.erase(frames.begin(),
                         frames.end() - static_cast<std::ptrdiff_t>(
                                            kMaxFiberChainNodes));
        }
        FiberChainSample sample;
        sample.gid = gid;
        sample.thread_id = ThreadHash();
        sample.ts_ns = SteadyNs();
        sample.nodes.reserve(frames.size());
        for (const auto& frame : frames) {
            FiberChainNode node;
            node.fiber_id = frame.id;
            node.gid = frame.execution.task_id != 0
                           ? frame.execution.task_id
                           : (frame.main_fiber ? 0U : gid);
            node.state = static_cast<std::uint8_t>(frame.state);
            node.suspend_reason =
                static_cast<std::uint8_t>(frame.suspend_reason);
            node.depth = static_cast<std::uint32_t>(frame.depth);
            node.alive = frame.alive;
            node.main_fiber = frame.main_fiber;
            if (frame.main_fiber) {
                node.function = "<main>";
            } else {
                std::lock_guard<std::mutex> lock(FiberMetaMutex());
                const auto it = FiberMetaMap().find(frame.id);
                if (it != FiberMetaMap().end()) {
                    node.function = it->second.function;
                    node.stack_low = it->second.stack_low;
                    node.stack_high = it->second.stack_high;
                    node.stack_known = it->second.stack_known;
                }
            }
            sample.nodes.push_back(std::move(node));
        }
        const std::uint64_t hash = HashChain(sample);
        ChainTable& table = ChainSamples();
        std::lock_guard<std::mutex> lock(table.mutex);
        const auto found = table.by_hash.find(hash);
        if (found != table.by_hash.end()) {
            FiberChainSample& existing = table.samples[found->second - 1U];
            if (existing.nodes.size() == sample.nodes.size()) {
                bool same = true;
                for (std::size_t i = 0; i < sample.nodes.size(); ++i) {
                    if (existing.nodes[i].fiber_id != sample.nodes[i].fiber_id ||
                        existing.nodes[i].function != sample.nodes[i].function) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    ++existing.refs;
                    return existing.id;
                }
            }
        }
        if (table.samples.size() >= kMaxFiberChains) {
            table.dropped.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        sample.id = static_cast<FiberChainId>(table.samples.size() + 1U);
        sample.refs = 1;
        table.samples.push_back(std::move(sample));
        table.by_hash[hash] = table.samples.back().id;
        return table.samples.back().id;
    } catch (...) {
        return 0;
    }
#else
    (void)gid;
    return 0;
#endif
}

}  // namespace

FiberChainId CaptureFiberChain(std::uint64_t gid, bool critical) noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    static thread_local std::uint32_t counter = 0;
    ++counter;
    std::uint32_t every = FiberChainSampleEvery();
    if (!critical) {
        every = ScaleChainEvery(every, DebugConfig::SampleShift());
        if (every > 1U && (counter % every) != 0U) {
            return 0;
        }
    }
    return CaptureFiberChainCore(gid);
#else
    (void)gid;
    (void)critical;
    return 0;
#endif
}

FiberChainId CaptureFiberChainForResource(std::uint64_t gid,
                                          bool critical) noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    // v7：资源事件走独立周期，critical（park/阻塞/唤醒）同样受约束。
    // 与 CaptureStackSampleForResource 配对，共同把 FULL 下的捕获成本
    // 从「每条事件」降到「每 16 条事件」再乘采样位移。
    (void)critical;
    static thread_local std::uint32_t counter = 0;
    ++counter;
    const std::uint32_t every = ScaleChainEvery(
        ResourceChainSampleEvery(), DebugConfig::SampleShift());
    if (every > 1U && (counter % every) != 1U) {
        return 0;
    }
    return CaptureFiberChainCore(gid);
#else
    (void)gid;
    (void)critical;
    return 0;
#endif
}

bool LookupFiberChain(FiberChainId id, FiberChainSample* out) {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    if (out == nullptr || id == 0) {
        return false;
    }
    ChainTable& table = ChainSamples();
    std::lock_guard<std::mutex> lock(table.mutex);
    if (id > table.samples.size()) {
        return false;
    }
    *out = table.samples[id - 1U];
    return true;
#else
    (void)id;
    (void)out;
    return false;
#endif
}

std::size_t FiberChainCount() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    ChainTable& table = ChainSamples();
    std::lock_guard<std::mutex> lock(table.mutex);
    return table.samples.size();
#else
    return 0;
#endif
}

std::uint64_t FiberChainDropped() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    return ChainSamples().dropped.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

void ResetFiberChains() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    ChainTable& table = ChainSamples();
    std::lock_guard<std::mutex> lock(table.mutex);
    table.samples.clear();
    table.by_hash.clear();
    table.dropped.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> meta_lock(FiberMetaMutex());
        FiberMetaTable& table = FiberMetaTableRef();
        table.entries.clear();
        table.order.clear();
        table.order_pos.clear();
    }
#endif
}

#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
namespace {
thread_local std::string t_next_fiber_name;
}  // namespace
#endif

void SetNextFiberName(const char* name) noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    try {
        if (name != nullptr) {
            t_next_fiber_name.assign(name);
        } else {
            t_next_fiber_name.clear();
        }
    } catch (...) {
        t_next_fiber_name.clear();
    }
#else
    (void)name;
#endif
}

const char* TakeNextFiberName() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    if (t_next_fiber_name.empty()) {
        return nullptr;
    }
    static thread_local std::string taken;
    try {
        taken = t_next_fiber_name;
    } catch (...) {
        return nullptr;
    }
    t_next_fiber_name.clear();
    return taken.c_str();
#else
    return nullptr;
#endif
}

}  // namespace debug


}  // namespace go2cpp
