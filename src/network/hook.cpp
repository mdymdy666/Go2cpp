/**
 * @file network/hook.cpp
 * @brief libc IO 函数的 interposition 实现：GMP「同步写法、异步执行」的落地层。
 *
 * ## 模块定位
 *
 * 本文件把 `read/write/recv/send/connect/accept/poll/sleep…` 截下来，替调用方
 * 决定「这次调用该 park 还是该让 worker 真阻塞」，从而让业务代码里看起来是
 * 同步阻塞的写法（`socket.recv()`）在受管 G 中自动让出 M。
 *
 * 上下游：
 *
 * - 上游是**用户的同步代码**与 `net::Socket`（`Socket::recv/send/accept` 最终
 *   落到这里）；
 * - 下游是 `io::IOManager`（`WaitFor` + epoll）与 `Scheduler`（park/wake）；
 * - 旁路是**未接管 fd**：走 `BlockingRegion` + sysmon 申请替代 M（见
 *   `docs/hook-blocking-policy.md` 的 C 类）。
 *
 * ## 三条处置路径（判定准则只有一条：能不能归约为「等某个 fd 就绪」）
 *
 * | 路径 | 判定 | 行为 |
 * |---|---|---|
 * | **park** | socket / FIFO 上的 read/write/connect/accept | fd 切 `O_NONBLOCK`，先试一次，`EAGAIN` 才注册 epoll 并 park |
 * | **BlockingRegion** | 非 fd 就绪可等（`poll/select/epoll_wait` 本身、未接管 fd、`fsync`、`getaddrinfo`…） | 标记 M 阻塞，sysmon 检测后申请替代 M |
 * | **直调** | 普通文件（`S_ISREG`，内核同步完成）与 `*_f` | 直接调 libc，不 park、不标记 |
 *
 * ## fd 类型识别（只能白名单）
 *
 * `anon_inodefs`（eventfd/timerfd/signalfd）**不给 `st_mode` 类型位**
 * （`S_IFMT == 0`），任何黑名单都会漏掉它们；因此先 `getsockopt(SO_TYPE)`
 * 判 socket，再用 `fstat + S_ISFIFO` 判 FIFO，其余一律不接管。
 *
 * ## `*_f` 系列（22 个）
 *
 * `read_f/write_f/recv_f/send_f/connect_f/accept_f/…` 是**显式 raw 调用**：
 * 直调 libc、不 park、会真阻塞整个 worker。它们不在 `go2cpp::hook` 命名空间里
 * （全局 `extern "C"` 符号）。**`_f` 不代表更快**，只代表「我知道自己在阻塞」。
 *
 * ## 关键不变量
 *
 * - 接管 socket/FIFO 后必须把 fd 切 `O_NONBLOCK`，并通过 `fcntl` hook 对用户
 *   隐藏该位（`OpenDescription::m_system_nonblocking` vs `m_user_nonblocking`）；
 * - 每个 hook 调用期间持有 `DescriptorGuard`（fd 条带锁）：保证 close/dup2 不会
 *   在系统调用中途抽走 fd；**绝不能跨 park 持有**；
 * - `hooks_enabled()` 为假或符号没解析出来时，所有 hook 必须退化为直调 libc ——
 *   这是「运行时关闭 hook 仍然正确」的唯一保证。
 *
 * ## 阅读建议
 *
 * 先看 `Originals`（符号表）→ `OpenDescription`/`Descriptor`/`DescriptorStripe`
 * （fd 元数据与分条）→ `inspect_socket`/`inspect_fifo`/`can_wait`（判定链）→
 * `cooperative_io`（park 主循环）→ 最后看各 hook 函数（它们几乎都是薄壳）。
 *
 * ## 相关文档
 *
 * - `docs/hook-blocking-policy.md`：判定准则与 FIFO park 的实测数据；
 * - `docs/concurrency-park-points.md`：哪些函数会 park、哪些阻塞 M、哪些是纯计算。
 */
#include "go2cpp/hook.hpp"
#include "go2cpp/config/config.hpp"

#ifndef __linux__
#error "go2cpp hook interposition currently requires Linux"
#endif

#include "go2cpp/fiber.hpp"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <string>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {


using go2cpp::IOEvent;
using go2cpp::IOManager;
using go2cpp::IOWaitStatus;
using Duration = IOManager::Duration;
using go2cpp::io::DescriptorGuard;
using go2cpp::io::DescriptorTokenPtr;

using SleepFn = unsigned int (*)(unsigned int);
using UsleepFn = int (*)(useconds_t);
using NanosleepFn = int (*)(const timespec*, timespec*);
// clock_nanosleep 的返回值语义与 nanosleep 不同：成功返回 0，
// 失败返回 **errno 本身**（不是 -1 + errno）。
using ClockNanosleepFn = int (*)(clockid_t, int, const timespec*, timespec*);
using SocketFn = int (*)(int, int, int);
using SocketPairFn = int (*)(int, int, int, int*);
using ConnectFn = int (*)(int, const sockaddr*, socklen_t);
using AcceptFn = int (*)(int, sockaddr*, socklen_t*);
using Accept4Fn = int (*)(int, sockaddr*, socklen_t*, int);
using ReadFn = ssize_t (*)(int, void*, size_t);
using ReadvFn = ssize_t (*)(int, const iovec*, int);
using RecvFn = ssize_t (*)(int, void*, size_t, int);
using RecvFromFn = ssize_t (*)(int, void*, size_t, int, sockaddr*, socklen_t*);
using RecvMsgFn = ssize_t (*)(int, msghdr*, int);
using WriteFn = ssize_t (*)(int, const void*, size_t);
using WritevFn = ssize_t (*)(int, const iovec*, int);
using SendFn = ssize_t (*)(int, const void*, size_t, int);
using SendToFn = ssize_t (*)(int, const void*, size_t, int,
                             const sockaddr*, socklen_t);
using SendMsgFn = ssize_t (*)(int, const msghdr*, int);
using CloseFn = int (*)(int);
using DupFn = int (*)(int);
using Dup2Fn = int (*)(int, int);
using Dup3Fn = int (*)(int, int, int);
using FcntlFn = int (*)(int, int, ...);
using IoctlFn = int (*)(int, unsigned long, ...);
using GetSockOptFn = int (*)(int, int, int, void*, socklen_t*);
using SetSockOptFn = int (*)(int, int, int, const void*, socklen_t);
using PollFn = int (*)(struct pollfd*, nfds_t, int);
using PpollFn = int (*)(struct pollfd*, nfds_t, const timespec*,
                        const sigset_t*);
using SelectFn = int (*)(int, fd_set*, fd_set*, fd_set*, timeval*);
using PselectFn = int (*)(int, fd_set*, fd_set*, fd_set*, const timespec*,
                          const sigset_t*);
using EpollWaitFn = int (*)(int, epoll_event*, int, int);
using EpollPwaitFn = int (*)(int, epoll_event*, int, int, const sigset_t*);

/**
 * @brief libc 原始函数的符号表（`dlsym(RTLD_NEXT, ...)` 的结果）。
 *
 * ## 定位
 *
 * 每个 hook 在「决定不接管」时必须调用原始 libc 实现。这里缓存全部 33 个符号，
 * 避免每次调用都 `dlsym`（`dlsym` 会取加载器锁，在 recv/send 热路径上不可接受）。
 *
 * ## 为什么用 `std::call_once` 惰性解析
 *
 * `initialize_originals()` 在**第一次 hook 调用时**执行，而不是静态初始化：
 * - hook 符号可能在别的 DSO 的静态初始化阶段就被调用，那时本文件的全局对象
 *   还没构造，不能依赖静态初始化顺序；
 * - `dlsym` 自身可能再次进入被 hook 的函数，所以解析期间置 `s_resolving`，
 *   hook 看到它就直接走 raw syscall（见 `read`/`write` 的实现）。
 *
 * ## 调用约定
 *
 * 所有成员只在 `call_once` 内写一次，之后只读 ⇒ 读取不需要锁。
 * `RealCallGuard` 在调用原始函数期间递增 `s_real_call_depth`，让内层 hook 判断
 * 「当前是在替 libc 干活」而不是用户调用。
 *
 * ## 成员分组（顺序与下方声明一致）
 *
 * - sleep 族：`m_sleep/m_usleep/m_nanosleep/m_clock_nanosleep`；
 * - socket 创建与连接：`m_socket/m_socketpair/m_connect/m_accept/m_accept4`；
 * - read 族：`m_read/m_readv/m_recv/m_recvfrom/m_recvmsg`；
 * - write 族：`m_write/m_writev/m_send/m_sendto/m_sendmsg`；
 * - fd 生命周期：`m_close/m_dup/m_dup2/m_dup3`；
 * - fd 属性：`m_fcntl/m_ioctl/m_getsockopt/m_setsockopt`；
 * - 事件循环入口：`m_poll/m_ppoll/m_select/m_pselect/m_epoll_wait/m_epoll_pwait`
 *   （这些**本身不 park**，只包 BlockingRegion 后直调）。
 */
struct Originals {
    /// @name sleep 族
    /// @{
    SleepFn m_sleep{nullptr};
    UsleepFn m_usleep{nullptr};
    NanosleepFn m_nanosleep{nullptr};
    ClockNanosleepFn m_clock_nanosleep{nullptr};
    /// @}
    /// @name socket 创建与连接
    /// @{
    SocketFn m_socket{nullptr};
    SocketPairFn m_socketpair{nullptr};
    ConnectFn m_connect{nullptr};
    AcceptFn m_accept{nullptr};
    Accept4Fn m_accept4{nullptr};
    /// @}
    /// @name read 族
    /// @{
    ReadFn m_read{nullptr};
    ReadvFn m_readv{nullptr};
    RecvFn m_recv{nullptr};
    RecvFromFn m_recvfrom{nullptr};
    RecvMsgFn m_recvmsg{nullptr};
    /// @}
    /// @name write 族
    /// @{
    WriteFn m_write{nullptr};
    WritevFn m_writev{nullptr};
    SendFn m_send{nullptr};
    SendToFn m_sendto{nullptr};
    SendMsgFn m_sendmsg{nullptr};
    /// @}
    /// @name fd 生命周期
    /// @{
    CloseFn m_close{nullptr};
    DupFn m_dup{nullptr};
    Dup2Fn m_dup2{nullptr};
    Dup3Fn m_dup3{nullptr};
    /// @}
    /// @name fd 属性
    /// @{
    FcntlFn m_fcntl{nullptr};
    IoctlFn m_ioctl{nullptr};
    GetSockOptFn m_getsockopt{nullptr};
    SetSockOptFn m_setsockopt{nullptr};
    /// @}
    /// @name 事件循环入口（不 park，只包 BlockingRegion）
    /// @{
    PollFn m_poll{nullptr};
    PpollFn m_ppoll{nullptr};
    SelectFn m_select{nullptr};
    PselectFn m_pselect{nullptr};
    EpollWaitFn m_epoll_wait{nullptr};
    EpollPwaitFn m_epoll_pwait{nullptr};
    /// @}
};

Originals s_originals;
std::once_flag s_originals_once;
thread_local unsigned int s_real_call_depth = 0;
thread_local bool s_resolving = false;

/**
 * @brief 在调用原始 libc 函数期间递增 `s_real_call_depth` 的 RAII 守卫。
 *
 * ## 为什么需要深度计数
 *
 * hook 函数要区分「用户调用」与「我们替 libc 干活」：深度 > 0 说明当前调用是
 * hook 内部转发给原始函数的（`invoke_real`），此时**不能再 park、不能再接管**，
 * 只能直调 —— 否则会无限递归（hook 调 hook）。
 *
 * ## 为什么是 thread_local
 *
 * 深度按线程计数：一个线程上同一时刻只有一条调用链，用全局原子会造成
 * 无关线程之间的误判（别的线程正在转发原始调用，不代表本线程也在）。
 */
class RealCallGuard final {
public:
    RealCallGuard() noexcept { ++s_real_call_depth; }
    ~RealCallGuard() { --s_real_call_depth; }
};

template <typename Function>
Function load_symbol(const char* name) noexcept {
    void* symbol = ::dlsym(RTLD_NEXT, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(symbol),
                  "function and object pointers must have equal ABI size");
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}

void initialize_originals() noexcept {
    if (s_resolving) {
        return;
    }
    std::call_once(s_originals_once, [] {
        s_resolving = true;
        s_originals.m_sleep = load_symbol<SleepFn>("sleep");
        s_originals.m_usleep = load_symbol<UsleepFn>("usleep");
        s_originals.m_nanosleep = load_symbol<NanosleepFn>("nanosleep");
        s_originals.m_clock_nanosleep =
            load_symbol<ClockNanosleepFn>("clock_nanosleep");
        s_originals.m_socket = load_symbol<SocketFn>("socket");
        s_originals.m_socketpair = load_symbol<SocketPairFn>("socketpair");
        s_originals.m_connect = load_symbol<ConnectFn>("connect");
        s_originals.m_accept = load_symbol<AcceptFn>("accept");
        s_originals.m_accept4 = load_symbol<Accept4Fn>("accept4");
        s_originals.m_read = load_symbol<ReadFn>("read");
        s_originals.m_readv = load_symbol<ReadvFn>("readv");
        s_originals.m_recv = load_symbol<RecvFn>("recv");
        s_originals.m_recvfrom = load_symbol<RecvFromFn>("recvfrom");
        s_originals.m_recvmsg = load_symbol<RecvMsgFn>("recvmsg");
        s_originals.m_write = load_symbol<WriteFn>("write");
        s_originals.m_writev = load_symbol<WritevFn>("writev");
        s_originals.m_send = load_symbol<SendFn>("send");
        s_originals.m_sendto = load_symbol<SendToFn>("sendto");
        s_originals.m_sendmsg = load_symbol<SendMsgFn>("sendmsg");
        s_originals.m_close = load_symbol<CloseFn>("close");
        s_originals.m_dup = load_symbol<DupFn>("dup");
        s_originals.m_dup2 = load_symbol<Dup2Fn>("dup2");
        s_originals.m_dup3 = load_symbol<Dup3Fn>("dup3");
        s_originals.m_fcntl = load_symbol<FcntlFn>("fcntl");
        s_originals.m_ioctl = load_symbol<IoctlFn>("ioctl");
        s_originals.m_getsockopt =
            load_symbol<GetSockOptFn>("getsockopt");
        s_originals.m_setsockopt =
            load_symbol<SetSockOptFn>("setsockopt");
        s_originals.m_poll = load_symbol<PollFn>("poll");
        s_originals.m_ppoll = load_symbol<PpollFn>("ppoll");
        s_originals.m_select = load_symbol<SelectFn>("select");
        s_originals.m_pselect = load_symbol<PselectFn>("pselect");
        s_originals.m_epoll_wait = load_symbol<EpollWaitFn>("epoll_wait");
        s_originals.m_epoll_pwait =
            load_symbol<EpollPwaitFn>("epoll_pwait");
        s_resolving = false;
    });
}

template <typename Function, typename... Args>
auto invoke_real(Function function, Args&&... args)
    -> decltype(function(std::forward<Args>(args)...)) {
    RealCallGuard guard;
    return function(std::forward<Args>(args)...);
}

// 被禁用或不可用的 IOManager 仍需记录 managed G 发起的原生调用。普通线程
// 以及已经处于另一个声明区域内的 worker 会走空操作。该区域让 Scheduler 在
// 系统调用休眠前发布 M::Blocking，并接纳替代的 M。
template <typename Function, typename... Args>
auto invoke_native_blocking(Function function, Args&&... args)
    -> decltype(function(std::forward<Args>(args)...)) {
    std::optional<go2cpp::BlockingRegion> blocking_region;
    if (go2cpp::Scheduler::current_task() != nullptr) {
        blocking_region.emplace();
    }
    return invoke_real(function, std::forward<Args>(args)...);
}

int raw_close_fallback(int fd) noexcept {
    return static_cast<int>(::syscall(SYS_close, fd));
}

std::atomic<bool> s_enabled{true};
std::atomic<std::uint64_t> s_scoped_enable_count{0};
std::atomic<std::int64_t> s_connect_timeout_ms{5000};
thread_local IOManager* s_bound_manager = nullptr;

bool hooks_enabled() noexcept {
    const bool process_enabled =
        s_enabled.load(std::memory_order_acquire) ||
        s_scoped_enable_count.load(std::memory_order_acquire) != 0;
    return go2cpp::thread_policy::detail::HookAllowed(process_enabled);
}

IOManager* current_manager() noexcept {
    if (IOManager* manager = IOManager::Current()) {
        return manager;
    }
    return s_bound_manager;
}

IOManager* cooperative_manager() noexcept {
    if (!hooks_enabled() || s_real_call_depth != 0 ||
        go2cpp::Fiber::Current() == nullptr ||
        !go2cpp::Scheduler::current_task()) {
        return nullptr;
    }
    return current_manager();
}

/**
 * @brief 一个「打开描述符」的共享元数据（dup/dup2 产生的副本共享同一份）。
 *
 * ## 定位
 *
 * `Descriptor`（句柄）通过 `m_open` 指向它；`dup/dup2/dup3` 只复制句柄、**不复制本结构**，
 * 所以「同一个 open file description 的多个 fd」共享 nonblocking 与超时元数据 ——
 * 这正是 POSIX 语义，也是 `clone_descriptor` 必须支持的原因（否则下一代会把 hook
 * 自己设置的 `O_NONBLOCK` 误判成用户意图而失去 park 能力）。
 *
 * ## 三个布尔位的组合决定处置路径（见 inspect_socket/inspect_fifo/can_wait）
 *
 * | m_pollable | m_system_nonblocking | m_user_nonblocking | 行为 |
 * |---|---|---|---|
 * | true | true | false | **park**：先试一次 syscall，EAGAIN 才注册 epoll |
 * | true | true | true | 用户显式要求非阻塞：保持 EAGAIN 语义，**不 park** |
 * | false | — | — | 走 `BlockingRegion` + sysmon（park 会永久挂起） |
 *
 * ## 并发
 *
 * 所有字段由 `m_mutex` 保护。`DescriptorGuard` 只锁 fd 所在的注册表条带，
 * **不代替本锁** —— 两者保护的对象不同（前者是 fd 代际，后者是元数据）。
 */
struct OpenDescription {
    /// @brief 保护本结构全部字段；fcntl/setsockopt hook 与 can_wait 竞争它。
    mutable std::mutex m_mutex;
    // 来源类型，仅用于诊断：socket 与 FIFO 走同一条 park 路径。
    /// @brief 来源标记（只用于诊断）：true=socket、false=FIFO。
    bool m_socket{true};
    // 该 fd 的阻塞原因能否归约为「epoll 能报的 fd 就绪」。
    // 只有 pollable 的 fd 才能 park Fiber；false 必须走 BlockingRegion，
    // 否则 park 之后没有任何东西能唤醒它（变成 Fiber 永久挂起）。
    /// @brief 能否 park（判定准则的唯一一条）；false 必须走 BlockingRegion。
    bool m_pollable{true};
    /// @brief 用户通过 fcntl 设置的 `O_NONBLOCK`（对用户可见的那一层）。
    bool m_user_nonblocking{false};
    /// @brief hook 自己设置的 `O_NONBLOCK`（对用户隐藏，park 的前提）。
    bool m_system_nonblocking{false};
    /// @brief `SO_RCVTIMEO` 映射的接收超时；`nullopt` 表示无限等待。
    std::optional<Duration> m_receive_timeout;
    /// @brief `SO_SNDTIMEO` 映射的发送超时；`nullopt` 表示无限等待。
    std::optional<Duration> m_send_timeout;
};

/**
 * @brief 单个 fd 的句柄记录（hook 的 fd → 元数据映射里的值）。
 *
 * ## 定位
 *
 * `DescriptorStripe::descriptors[fd]` 存的就是它，把「fd 的属性」与「fd 的代际令牌」
 * 绑在一起：
 * - `m_open`：共享的打开描述元数据；`dup` 出来的副本共享同一份（POSIX 语义）；
 * - `m_token`：`io::DescriptorToken`，代际号用于拒绝 close/dup2 之后迟到的 epoll
 *   事件 —— `IOManager` 的 `FdSlot` 用同一个令牌做二次校验。
 *
 * ## 生命周期
 *
 * 由 `DescriptorStripe::descriptors` 的 `shared_ptr` 持有；hook 每次调用通过
 * `descriptor_for(fd)` 取一份临时强引用，保证调用期间元数据不被释放。
 * `m_closed` 一旦置真，后续 hook 直接把该 fd 当未接管处理（走 native 路径）。
 */
struct Descriptor {
    Descriptor(std::shared_ptr<OpenDescription> description,
               DescriptorTokenPtr descriptor_token)
        : m_open(std::move(description)),
          m_token(std::move(descriptor_token)) {}

    /// @brief 共享的打开描述元数据；dup 副本共享同一份（POSIX open file description）。
    std::shared_ptr<OpenDescription> m_open;
    /// @brief 代际令牌；close/dup2 之后的迟到事件靠它被拒绝（与 IOManager 的 FdSlot 共用）。
    DescriptorTokenPtr m_token;
    /// @brief 保护 m_closed；close 路径置位，descriptor_closed() 读取。
    mutable std::mutex m_mutex;
    /// @brief 已关闭标记；置真后 hook 把该 fd 当未接管（走 native 路径）。
    bool m_closed{false};
};

// 描述符元数据表按 fd 分条带。
//
// 为什么分片：每次 recv/send/read/write/fcntl/setsockopt 进入 Hook 都要先在
// 这里查一次元数据。原先这把全局锁让 12 个 M 在所有连接上互相排队——万连接
// 场景下 12 个 worker 只有约 2 个核在做有效工作，其余全部阻塞在这把锁上。
//
// 为什么安全：分片键是 fd 数值本身（fd & (N-1)），因此同一个 fd 永远落在
// 同一条带，同一 fd 的 adopt/close/dup2/在途 syscall 之间的互斥关系与分片
// 前完全一致；不同 fd 落在不同条带，互不串行。这与 DescriptorRegistryState
// （令牌 + 代际）使用同一套分片键，两层门的相对顺序不变：
//
//   DescriptorRegistryState 条带锁（fd 代际门） -> 描述符表条带锁 -> Descriptor::m_mutex
//
// 不得出现反向嵌套（例如持描述符表条带锁再调用 DescriptorGuard::Capture），
// 否则两条同 fd 路径会互相等待。
constexpr std::size_t kDescriptorStripeCount = 64;

/**
 * @brief fd 注册表的一个条带（按 fd 哈希分片，减少 hook 热路径的锁争用）。
 *
 * ## 为什么分条
 *
 * hook 的每个 recv/send/fcntl/setsockopt 都要查一次 fd 元数据；单一全局锁在 12 个
 * worker 下会把它们串行化（WSL2 上一次竞争约 55~75µs，短连接 accept 路径上的两次
 * setsockopt 实测 168µs）。分条后只有**同一个 fd** 的调用互相阻塞，不同 fd 互不影响，
 * 而「同一 fd 的 Capture/Invalidate/在途 syscall 互斥」这个不变式仍然成立。
 *
 * ## 不变式
 *
 * - 同一 fd 永远落在同一条带（`fd % kStripeCount`），所以「查/改/删」天然互斥；
 * - 条带锁只在查询/更新元数据时短暂持有，**绝不跨系统调用或 park 持有**；
 * - `descriptors` 里没有条目 ⇒ 该 fd 未被接管，hook 走 native 路径。
 */
struct DescriptorStripe {
    /// @brief 保护 descriptors；descriptor_for/adopt_socket/adopt_fifo/close 路径竞争它。
    std::mutex mutex;
    /// @brief fd → 句柄记录；没有条目表示该 fd 未被 hook 接管。
    std::unordered_map<int, std::shared_ptr<Descriptor>> descriptors;
};

// 函数内 static：避免与其它翻译单元之间的静态初始化顺序依赖。
std::array<DescriptorStripe, kDescriptorStripeCount>& descriptor_stripes() {
    static std::array<DescriptorStripe, kDescriptorStripeCount> stripes;
    return stripes;
}

/// 返回 fd 所属的描述符表条带。负数 fd 会落到某个条带但必然查不到，
/// 调用方在此之前都已经做过 fd < 0 判断。
DescriptorStripe& descriptor_stripe_for(int fd) noexcept {
    return descriptor_stripes()[static_cast<std::size_t>(
                                    static_cast<unsigned int>(fd)) &
                                (kDescriptorStripeCount - 1)];
}

std::optional<Duration> timeval_duration(const timeval& value) noexcept {
    if (value.tv_sec < 0 || value.tv_usec < 0 || value.tv_usec >= 1000000) {
        return std::nullopt;
    }
    if (value.tv_sec == 0 && value.tv_usec == 0) {
        return std::nullopt;
    }
    using Microseconds = std::chrono::microseconds;
    constexpr auto maximum =
        std::chrono::duration_cast<Microseconds>(Duration::max()).count();
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    const auto microseconds = static_cast<std::uint64_t>(value.tv_usec);
    const auto max_count = static_cast<std::uint64_t>(maximum);
    if (seconds > max_count / 1000000ULL ||
        seconds * 1000000ULL > max_count - microseconds) {
        return Duration::max();
    }
    return std::chrono::duration_cast<Duration>(
        Microseconds(seconds * 1000000ULL + microseconds));
}

std::optional<Duration> socket_timeout(int fd, int option) noexcept {
    if (!s_originals.m_getsockopt) {
        return std::nullopt;
    }
    timeval value{};
    socklen_t size = sizeof(value);
    if (invoke_real(s_originals.m_getsockopt, fd, SOL_SOCKET, option, &value,
                    &size) != 0 ||
        size < sizeof(value)) {
        return std::nullopt;
    }
    return timeval_duration(value);
}

std::shared_ptr<Descriptor> descriptor_for(int fd) {
    DescriptorStripe& stripe = descriptor_stripe_for(fd);
    std::lock_guard<std::mutex> lock(stripe.mutex);
    const auto found = stripe.descriptors.find(fd);
    return found == stripe.descriptors.end() ? nullptr : found->second;
}

bool inspect_socket(int fd) noexcept {
    if (!s_originals.m_getsockopt) {
        return false;
    }
    int type = 0;
    socklen_t size = sizeof(type);
    return invoke_real(s_originals.m_getsockopt, fd, SOL_SOCKET, SO_TYPE, &type,
                       &size) == 0;
}

std::shared_ptr<Descriptor> adopt_socket(int fd) {
    DescriptorGuard lifecycle(fd);
    if (fd < 0 || !s_originals.m_fcntl || !inspect_socket(fd)) {
        return {};
    }

    // 串行化第一次接管。否则一个接管者可能观察到另一个接管者设置的
    // O_NONBLOCK 位，并错误地将其识别为用户请求。按 fd 分片后串行化只覆盖
    // 同一个数字 fd（这正是语义要求），不同 fd 的首次接管可以并行。
    DescriptorStripe& stripe = descriptor_stripe_for(fd);
    std::lock_guard<std::mutex> lock(stripe.mutex);
    const auto found = stripe.descriptors.find(fd);
    if (found != stripe.descriptors.end()) {
        return found->second;
    }

    const int flags = invoke_real(s_originals.m_fcntl, fd, F_GETFL);
    if (flags < 0) {
        return {};
    }

    const bool originally_nonblocking = (flags & O_NONBLOCK) != 0;
    bool changed_system_nonblocking = false;
    auto open = std::make_shared<OpenDescription>();
    open->m_user_nonblocking = originally_nonblocking;
    open->m_system_nonblocking = originally_nonblocking;
    open->m_receive_timeout = socket_timeout(fd, SO_RCVTIMEO);
    open->m_send_timeout = socket_timeout(fd, SO_SNDTIMEO);
    if (!open->m_system_nonblocking &&
        invoke_real(s_originals.m_fcntl, fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        open->m_system_nonblocking = true;
        changed_system_nonblocking = true;
    }
    try {
        auto descriptor = std::make_shared<Descriptor>(
            std::move(open), DescriptorGuard::Capture(fd));
        stripe.descriptors.emplace(fd, descriptor);
        return descriptor;
    } catch (...) {
        if (changed_system_nonblocking) {
            (void)invoke_real(s_originals.m_fcntl, fd, F_SETFL, flags);
        }
        throw;
    }
}

std::shared_ptr<Descriptor> try_adopt_socket(int fd) noexcept {
    try {
        return adopt_socket(fd);
    } catch (...) {
        // 元数据只是围绕真实系统调用的优化。分配失败时保持 fd 可用，让调用者
        // 回退到 libc。
        return {};
    }
}

// FIFO/pipe 的阻塞原因同样是「fd 可读/可写」，epoll 能报，因此可以 park。
// getsockopt(SO_TYPE) 对它们必然返回 ENOTSOCK，只能用 fstat 的类型位识别。
bool inspect_fifo(int fd) noexcept {
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
        return false;
    }
    return S_ISFIFO(status.st_mode) != 0;
}

// 与 adopt_socket 同构，差异只有两点：
//   1. 不读 SO_RCVTIMEO/SO_SNDTIMEO —— FIFO 没有 socket 超时语义，
//      保持 nullopt（等到数据就绪或对端关闭）；
//   2. m_socket=false，只标记来源。
// 同样把 fd 切到 O_NONBLOCK：这是 park 的前提（先试一次 syscall，
// EAGAIN 才去 epoll 等），并且 fcntl hook 会对用户隐藏这个内部位。
std::shared_ptr<Descriptor> adopt_fifo(int fd) {
    DescriptorGuard lifecycle(fd);
    if (fd < 0 || !s_originals.m_fcntl || !inspect_fifo(fd)) {
        return {};
    }

    // 与 socket 接管使用同一套条带锁：并发接管时不能把内部设置的
    // O_NONBLOCK 误判成用户请求。同一个 fd 落在同一条带，语义与分片前一致。
    DescriptorStripe& stripe = descriptor_stripe_for(fd);
    std::lock_guard<std::mutex> lock(stripe.mutex);
    const auto found = stripe.descriptors.find(fd);
    if (found != stripe.descriptors.end()) {
        return found->second;
    }

    const int flags = invoke_real(s_originals.m_fcntl, fd, F_GETFL);
    if (flags < 0) {
        return {};
    }

    const bool originally_nonblocking = (flags & O_NONBLOCK) != 0;
    bool changed_system_nonblocking = false;
    auto open = std::make_shared<OpenDescription>();
    open->m_socket = false;
    open->m_pollable = true;
    open->m_user_nonblocking = originally_nonblocking;
    open->m_system_nonblocking = originally_nonblocking;
    if (!open->m_system_nonblocking &&
        invoke_real(s_originals.m_fcntl, fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        open->m_system_nonblocking = true;
        changed_system_nonblocking = true;
    }
    try {
        auto descriptor = std::make_shared<Descriptor>(
            std::move(open), DescriptorGuard::Capture(fd));
        stripe.descriptors.emplace(fd, descriptor);
        return descriptor;
    } catch (...) {
        if (changed_system_nonblocking) {
            (void)invoke_real(s_originals.m_fcntl, fd, F_SETFL, flags);
        }
        throw;
    }
}

std::shared_ptr<Descriptor> try_adopt_fifo(int fd) noexcept {
    try {
        return adopt_fifo(fd);
    } catch (...) {
        // 元数据分配失败不能影响 fd 本身：回退到 libc 阻塞语义，
        // 由 BlockingRegion/sysmon 兜底。
        return {};
    }
}

void register_new_socket(int fd, bool user_nonblocking) {
    DescriptorGuard lifecycle(fd);
    if (fd < 0 || !s_originals.m_fcntl) {
        return;
    }
    const int flags = invoke_real(s_originals.m_fcntl, fd, F_GETFL);
    if (flags < 0) {
        return;
    }
    const bool originally_nonblocking = (flags & O_NONBLOCK) != 0;
    bool changed_system_nonblocking = false;
    auto open = std::make_shared<OpenDescription>();
    open->m_user_nonblocking = user_nonblocking || originally_nonblocking;
    open->m_system_nonblocking = originally_nonblocking;
    if (!open->m_system_nonblocking &&
        invoke_real(s_originals.m_fcntl, fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        open->m_system_nonblocking = true;
        changed_system_nonblocking = true;
    }
    try {
        auto descriptor = std::make_shared<Descriptor>(
            std::move(open), DescriptorGuard::Capture(fd));
        DescriptorStripe& stripe = descriptor_stripe_for(fd);
        std::lock_guard<std::mutex> lock(stripe.mutex);
        stripe.descriptors[fd] = std::move(descriptor);
    } catch (...) {
        if (changed_system_nonblocking) {
            (void)invoke_real(s_originals.m_fcntl, fd, F_SETFL, flags);
        }
        throw;
    }
}

void try_register_new_socket(int fd, bool user_nonblocking) noexcept {
    try {
        register_new_socket(fd, user_nonblocking);
    } catch (...) {
        // socket 系统调用已经成功。未跟踪的描述符可以在后续 managed 操作中
        // 延迟接管。
    }
}

/**
 * @brief 一个 fd 句柄的一致快照（令牌 + 关闭标记）。
 *
 * ## 为什么不在调用现场直接读
 *
 * hook 热路径需要「判断 fd 是否仍有效」而**不持有条带锁**：`snapshot_descriptor`
 * 在锁内取一份快照（复制 `shared_ptr`），锁外再判 `m_closed` / `token->valid()`。
 * 复制令牌保证判断期间它不会被回收。
 *
 * ## `m_closed` 的三重含义
 *
 * 只要 `m_closed` 为真、**或**令牌为空、**或**令牌 `valid()==false`，都视为已关闭 ——
 * 后两者对应「close/dup2 已使令牌失效、但描述符还没从条带表里清掉」的窗口。
 */
struct DescriptorSnapshot {
    /// @brief 代际令牌（强引用，保证判断期间不被回收）。
    DescriptorTokenPtr m_token;
    /// @brief 是否已关闭（含令牌为空/失效两种情形）。
    bool m_closed{true};
};

DescriptorSnapshot snapshot_descriptor(
    const std::shared_ptr<Descriptor>& descriptor) noexcept {
    DescriptorSnapshot snapshot;
    if (!descriptor) {
        return snapshot;
    }
    std::lock_guard<std::mutex> lock(descriptor->m_mutex);
    snapshot.m_token = descriptor->m_token;
    snapshot.m_closed = descriptor->m_closed || !snapshot.m_token ||
                        !snapshot.m_token->valid();
    return snapshot;
}

bool descriptor_closed(const std::shared_ptr<Descriptor>& descriptor) noexcept {
    return snapshot_descriptor(descriptor).m_closed;
}

/**
 * @brief `close` 的两阶段计划：先摘出句柄并标记关闭，再在锁外收尾。
 *
 * ## 为什么分两阶段
 *
 * `close(fd)` 要做四件事：① 从条带表摘出 `Descriptor`（持条带锁）；② 标记
 * `m_closed`；③ 调原始 `close`；④ 通知 `IOManager` 清理槽位。它们涉及不同的锁，
 * 混在一起会违反锁层次（hook 条带锁 → IOManager 条带锁）。
 *
 * 因此 `prepare_close` 只做 ①②，`finish_close` 在**锁外**做 ③④。
 */
struct ClosePlan {
    /// @brief 摘出的句柄；为空表示该 fd 未被接管（close 直接透传给 libc）。
    std::shared_ptr<Descriptor> m_descriptor;
};

ClosePlan prepare_close(int fd) {
    ClosePlan plan;
    {
        DescriptorStripe& stripe = descriptor_stripe_for(fd);
        std::lock_guard<std::mutex> lock(stripe.mutex);
        const auto found = stripe.descriptors.find(fd);
        if (found != stripe.descriptors.end()) {
            plan.m_descriptor = found->second;
        }
    }

    if (plan.m_descriptor) {
        std::lock_guard<std::mutex> lock(plan.m_descriptor->m_mutex);
        plan.m_descriptor->m_closed = true;
    }
    return plan;
}

void finish_close(int fd, const std::shared_ptr<Descriptor>& descriptor) {
    if (!descriptor) {
        return;
    }
    DescriptorStripe& stripe = descriptor_stripe_for(fd);
    std::lock_guard<std::mutex> lock(stripe.mutex);
    const auto found = stripe.descriptors.find(fd);
    if (found != stripe.descriptors.end() && found->second == descriptor) {
        stripe.descriptors.erase(found);
    }
}

void notify_before_close(int fd) {
    IOManager::NotifyCloseAll(fd);
}

void clone_descriptor(int old_fd, int new_fd) {
    auto source = descriptor_for(old_fd);
    if (!source && cooperative_manager()) {
        source = adopt_socket(old_fd);
        if (!source) {
            // dup 出来的 FIFO 副本共享同一 open file description；必须沿用
            // FIFO 接管规则，否则下一代会把运行时的 O_NONBLOCK 当成用户意图，
            // 从而静默失去 park 能力。
            source = adopt_fifo(old_fd);
        }
    }
    if (source && snapshot_descriptor(source).m_closed) {
        source.reset();
    }
    // Capture 必须在描述符表条带锁之外完成：Capture 内部要拿
    // DescriptorRegistryState 的 fd 条带锁，而 adopt_socket 等路径是
    // 「先拿注册表条带锁、再拿描述符表条带锁」。若这里反过来嵌套，
    // 同一个 fd 上两条路径会互相等待（死锁）。
    DescriptorTokenPtr new_token;
    if (source) {
        new_token = DescriptorGuard::Capture(new_fd);
    }
    DescriptorStripe& stripe = descriptor_stripe_for(new_fd);
    std::lock_guard<std::mutex> lock(stripe.mutex);
    if (source) {
        stripe.descriptors[new_fd] = std::make_shared<Descriptor>(
            source->m_open, std::move(new_token));
    } else {
        stripe.descriptors.erase(new_fd);
    }
}

void try_clone_descriptor(int old_fd, int new_fd) noexcept {
    try {
        clone_descriptor(old_fd, new_fd);
    } catch (...) {
        // 不要让 C++ 分配失败越过 C ABI。移除过期的目标条目；下一次 managed
        // 调用可以重新接管该描述符。
        DescriptorStripe& stripe = descriptor_stripe_for(new_fd);
        std::lock_guard<std::mutex> lock(stripe.mutex);
        stripe.descriptors.erase(new_fd);
    }
}

// dup2/dup3 会把关闭目标作为内核操作的一部分。Hook 必须在进入系统调用前
// 发布关闭，防止旧 open-file description 的 epoll 事件赢得一次性等待。若
// Linux 内核拒绝本来有效的替换，目标仍保持打开；重新建立描述符 token，
// 让后续 managed 调用可以再次接管。预检关闭已经唤醒的 waiter 保留其终态结果。
void restore_failed_replacement(
    int fd, const std::shared_ptr<Descriptor>& descriptor) noexcept {
    if (!descriptor) {
        return;
    }
    DescriptorTokenPtr token;
    try {
        token = DescriptorGuard::Capture(fd);
    } catch (...) {
        // 如果恢复过程本身内存不足，丢弃过期条目。后续 managed 操作可以接管
        // 仍存活的描述符，而不会被错误的 closed 标志永久阻塞。
    }
    if (token) {
        std::lock_guard<std::mutex> lock(descriptor->m_mutex);
        descriptor->m_token = std::move(token);
        descriptor->m_closed = false;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(descriptor->m_mutex);
        descriptor->m_token.reset();
        descriptor->m_closed = true;
    }
    DescriptorStripe& stripe = descriptor_stripe_for(fd);
    std::lock_guard<std::mutex> lock(stripe.mutex);
    const auto found = stripe.descriptors.find(fd);
    if (found != stripe.descriptors.end() && found->second == descriptor) {
        stripe.descriptors.erase(found);
    }
}

bool source_fd_is_valid(int fd) noexcept {
    if (fd < 0 || !s_originals.m_fcntl) {
        return false;
    }
    return invoke_real(s_originals.m_fcntl, fd, F_GETFD) >= 0;
}

std::optional<Duration> operation_timeout(
    const std::shared_ptr<Descriptor>& descriptor, IOEvent event) {
    std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
    return event == IOEvent::kRead ? descriptor->m_open->m_receive_timeout
                                   : descriptor->m_open->m_send_timeout;
}

/**
 * @brief 判定「这个 fd 上的调用可以 park 吗」—— 整个 hook 层唯一的决策点。
 *
 * ## 判定链（三步，从粗到细）
 *
 * step1. `inspect_socket(fd)`：`getsockopt(SOL_SOCKET, SO_TYPE)` 成功 ⇒ socket；
 * step2. 失败则 `inspect_fifo(fd)`：`fstat` + `S_ISFIFO` ⇒ FIFO/pipe；
 *        两者都不成立 ⇒ 不接管，走 `BlockingRegion` 或直调；
 * step3. 本函数：接管成功的 fd 还要同时满足三个布尔位才 park。
 *
 * ## 为什么只能白名单
 *
 * `anon_inodefs`（`eventfd`/`timerfd`/`signalfd`）**不给 `st_mode` 类型位**
 * （`S_IFMT == 0`，`ISREG/ISFIFO/ISSOCK/ISCHR` 全为 0），任何黑名单都必然漏掉
 * 它们 —— 而它们恰好都会阻塞。所以只能「先认 socket、再认 FIFO，其余一律不碰」。
 *
 * ## 三个条件的含义
 *
 * | 条件 | 为什么必须 |
 * |---|---|
 * | `m_pollable` | 阻塞原因必须能归约为「某个 fd 就绪」；否则 park 之后没有任何东西能唤醒它（Fiber 永久挂起，比 worker 被阻塞更难查） |
 * | `m_system_nonblocking` | hook 必须已经把 fd 切成 `O_NONBLOCK`，否则「先试一次」会直接阻塞 M |
 * | `!m_user_nonblocking` | 用户显式要非阻塞时保持 `EAGAIN` 语义，**替他 park 会违反他的契约** |
 */
bool can_wait(const std::shared_ptr<Descriptor>& descriptor) noexcept {
    std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
    return descriptor->m_open->m_pollable &&
           descriptor->m_open->m_system_nonblocking &&
           !descriptor->m_open->m_user_nonblocking;
}

int wait_error(const go2cpp::IOWaitResult& result) noexcept {
    switch (result.status) {
        case IOWaitStatus::kReady:
            return 0;
        case IOWaitStatus::kTimeout:
            return ETIMEDOUT;
        case IOWaitStatus::kCancelled:
            return ECANCELED;
        case IOWaitStatus::kClosed:
            return EBADF;
        case IOWaitStatus::kError:
            return result.system_error == 0 ? EIO : result.system_error;
    }
    return EIO;
}

go2cpp::IOWaitResult wait_for_io(
    IOManager& manager, int fd, IOEvent event,
    const std::optional<IOManager::TimePoint>& deadline,
    DescriptorTokenPtr token) {
    return manager.wait(fd, event, deadline, {}, std::move(token));
}

std::optional<IOManager::TimePoint> deadline_from_timeout(
    const std::optional<Duration>& timeout) {
    if (!timeout.has_value()) {
        return std::nullopt;
    }
    const auto now = IOManager::Clock::now();
    if (*timeout >= Duration::max() - now.time_since_epoch()) {
        return IOManager::TimePoint::max();
    }
    return now + *timeout;
}

int native_wait(int fd, IOEvent event,
                const std::optional<IOManager::TimePoint>& deadline,
                const std::shared_ptr<Descriptor>& descriptor) {
    // 另一个线程中的 poll 不一定会被 close 可靠中断。有限时长的 poll 切片让
    // 保留的代次检测 close，同时不持有 IOManager，也不会在复用的数字描述符
    // 上永久等待。
    for (;;) {
        {
            DescriptorGuard lifecycle(fd);
            if (descriptor_closed(descriptor)) {
                return EBADF;
            }
        }
        int milliseconds = 10;
        if (deadline) {
            const auto now = IOManager::Clock::now();
            if (*deadline <= now) {
                // 原始阻塞 socket 契约报告 socket 超时，而不是内部非阻塞探测结果。
                return ETIMEDOUT;
            }
            const auto remaining = *deadline - now;
            // 用短切片轮询，及时观察 close 或 fd 复用。转换前先限制范围：
            // TimePoint::max() 可能超出毫秒 duration 的可表示范围。
            const auto slice = std::min(
                remaining,
                std::chrono::duration_cast<IOManager::Clock::duration>(
                    std::chrono::milliseconds(10)));
            auto rounded = std::chrono::duration_cast<std::chrono::milliseconds>(
                slice);
            if (rounded < slice) {
                rounded += std::chrono::milliseconds(1);
            }
            milliseconds = static_cast<int>(std::min<std::int64_t>(
                milliseconds, std::max<std::int64_t>(1, rounded.count())));
        }
        pollfd poll_descriptor{};
        poll_descriptor.fd = fd;
        poll_descriptor.events = event == IOEvent::kRead ? POLLIN : POLLOUT;
        const long result = ::syscall(SYS_poll, &poll_descriptor, 1,
                                      milliseconds);
        if (result < 0) {
            return errno;
        }
        if (result > 0) {
            // poll 返回就绪后再次通过生命周期门检查。这样 close/dup2
            // Hook 在就绪通知与真正读写之间不能把旧 generation 伪装成
            // 新的同号 fd；真正的 syscall 仍在 cooperative_io 的门内执行。
            DescriptorGuard lifecycle(fd);
            if (descriptor_closed(descriptor)) {
                return EBADF;
            }
            return (poll_descriptor.revents & POLLNVAL) != 0 ? EBADF : 0;
        }
    }
}

/**
 * @brief 所有可 park 的 hook 共用的主循环：「先真调一次，EAGAIN 才 park」。
 *
 * ## 定位
 *
 * `read/write/recv/send/recvfrom/sendto/recvmsg/sendmsg/connect/accept/accept4`
 * 的 hook 全是薄壳，最终都汇聚到这里。它负责三件事：① 判定该 fd 走 park 还是
 * native；② 执行真实 syscall；③ 在 `EAGAIN` 时注册到 `IOManager` 并 park 当前 G。
 *
 * ## 步骤
 *
 * step1. 取 `cooperative_manager()`；若没有 manager 但当前是 managed G，说明 hook
 *        已启用却没有 IOManager —— 退化为 `BlockingRegion` + 原生调用；
 * step2. `descriptor_for(fd)` 查元数据；未接管时先试 `try_adopt_socket`
 *        （`getsockopt(SO_TYPE)`）、再试 `try_adopt_fifo`（`fstat + S_ISFIFO`）；
 *        都失败则原生调用，并**显式发布 `M::Blocking`**（否则 sysmon 看不见这个长调用）；
 * step3. `can_wait()` 决定本次是否可 park（用户显式非阻塞时 `blocking=false`）；
 * step4. 循环体内持 `DescriptorGuard`（fd 条带锁）做一次真实 syscall ——
 *        持门保证 close/dup2 不会在调用中途抽走 fd，**但绝不跨 park 持有**；
 * step5. 成功、或错误不是 `EAGAIN/EWOULDBLOCK` ⇒ 直接返回；
 * step6. `EAGAIN` ⇒ 注册 epoll 等待并 park；被唤醒后回到 step4 **重试** ——
 *        数据可能已被别的读者取走，所以不能假设「唤醒 = 有数据」。
 *
 * ## 为什么第一次一定要真调
 *
 * 「先试后等」省掉「数据已在缓冲里却还去 park」的往返；它也是 `O_NONBLOCK`
 * 语义的来源 —— 用户看到的是 `EAGAIN`，我们拿到的是 park 点。
 *
 * ## 参数
 *
 * @param per_call_nonblocking 本次调用是否由 `SOCK_NONBLOCK`/`MSG_DONTWAIT` 等
 *        单次标志要求非阻塞（此时即使 `can_wait` 为真也不 park）；
 * @param args 透传给原始 libc 函数。
 */
template <typename Result, typename Function, typename... Args>
Result cooperative_io(int fd, Function function, IOEvent event,
                      bool per_call_nonblocking, Args... args) {
    if (!function) {
        errno = ENOSYS;
        return static_cast<Result>(-1);
    }
    IOManager* manager = cooperative_manager();
    const bool managed_hook_admission =
        manager != nullptr ||
        (hooks_enabled() && s_real_call_depth == 0 &&
         go2cpp::Scheduler::current_task() != nullptr &&
         go2cpp::Fiber::Current() != nullptr);
    // 启用 Hook 时，没有 IOManager 的 managed Fiber 使用相同的描述符元数据和
    // 原生 poll 回退。禁用 Hook 时保留调用者的原生阻塞选择，但仍在下方记录 M。
    std::optional<go2cpp::BlockingRegion> blocking_region;
    if (manager == nullptr && go2cpp::Scheduler::current_task() != nullptr) {
        blocking_region.emplace();
    }
    auto descriptor = descriptor_for(fd);
    if (!descriptor && managed_hook_admission) {
        descriptor = try_adopt_socket(fd);
        if (!descriptor) {
            // FIFO/pipe 同样是「等 fd 就绪」：getsockopt 认不出它们，
            // 但 fstat 能识别 S_ISFIFO。接管后 read/write 会 park，
            // worker 立即空出，而不是靠 sysmon 补一个线程。
            descriptor = try_adopt_fifo(fd);
        }
    }
    if (!descriptor) {
        // IOManager 正常路径把 socket 设为 nonblocking 并 park Fiber；
        // 未跟踪/懒采用失败的 fd 会回到 libc，必须显式发布 M::Blocking，
        // 否则 sysmon 无法为这个长系统调用申请替代 M。
        if (!blocking_region && go2cpp::Scheduler::current_task() != nullptr) {
            blocking_region.emplace();
        }
        return invoke_real(function, fd, args...);
    }

    // step3: 只有 can_wait 且本次没有单次非阻塞标志，才允许 park。
    const bool blocking = !per_call_nonblocking && can_wait(descriptor);
    const auto deadline =
        deadline_from_timeout(operation_timeout(descriptor, event));
    for (;;) {
        Result result;
        {
            // step4: 持条带锁做真实 syscall；锁只覆盖这一次调用，不跨 park。
            DescriptorGuard lifecycle(fd);
            if (snapshot_descriptor(descriptor).m_closed) {
                errno = EBADF;
                return static_cast<Result>(-1);
            }
            result = invoke_real(function, fd, args...);
        }
        if (result == static_cast<Result>(-1) && errno == EINTR && manager) {
            continue;
        }
        if (!blocking) {
            return result;
        }
        if (result != static_cast<Result>(-1) ||
            (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return result;
        }

        const auto token = snapshot_descriptor(descriptor).m_token;
        const int error = manager
                              ? wait_error(wait_for_io(*manager, fd, event,
                                                       deadline, token))
                              : native_wait(fd, event, deadline, descriptor);
        if (error != 0) {
            errno = error;
            return static_cast<Result>(-1);
        }
    }
}

bool has_dontwait(int flags) noexcept {
#ifdef MSG_DONTWAIT
    return (flags & MSG_DONTWAIT) != 0;
#else
    (void)flags;
    return false;
#endif
}

std::optional<Duration> connect_wait_timeout(
    const std::shared_ptr<Descriptor>& descriptor) {
    if (auto timeout = operation_timeout(descriptor, IOEvent::kWrite)) {
        return timeout;
    }
    const auto milliseconds =
        s_connect_timeout_ms.load(std::memory_order_acquire);
    if (milliseconds < 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds(milliseconds);
}

int cooperative_connect(int fd, const sockaddr* address, socklen_t length) {
    if (!s_originals.m_connect) {
        errno = ENOSYS;
        return -1;
    }
    IOManager* manager = cooperative_manager();
    const bool managed_hook_admission =
        manager != nullptr ||
        (hooks_enabled() && s_real_call_depth == 0 &&
         go2cpp::Scheduler::current_task() != nullptr &&
         go2cpp::Fiber::Current() != nullptr);
    // 启用 Hook 时，没有 IOManager 的 managed Fiber 仍可使用有限时长的原生
    // poll 回退。禁用 Hook 时保留原生语义，只发布 M::Blocking。
    std::optional<go2cpp::BlockingRegion> blocking_region;
    if (manager == nullptr && go2cpp::Scheduler::current_task() != nullptr) {
        blocking_region.emplace();
    }
    auto descriptor = descriptor_for(fd);
    if (!descriptor && managed_hook_admission) {
        descriptor = try_adopt_socket(fd);
    }
    if (!descriptor) {
        // connect 的未跟踪 fd 同样会落回可能阻塞的 libc 调用；在
        // managed G 中补齐 BlockingRegion，普通线程保持原生语义。
        if (!blocking_region && go2cpp::Scheduler::current_task() != nullptr) {
            blocking_region.emplace();
        }
        return invoke_real(s_originals.m_connect, fd, address, length);
    }

    int result;
    do {
        DescriptorGuard lifecycle(fd);
        if (snapshot_descriptor(descriptor).m_closed) {
            errno = EBADF;
            return -1;
        }
        result = invoke_real(s_originals.m_connect, fd, address, length);
    } while (result < 0 && errno == EINTR && manager);
    if (!can_wait(descriptor)) {
        return result;
    }
    if (result == 0) {
        return 0;
    }
    if (result != -1 ||
        (errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN)) {
        return result;
    }

    // 配置的 connect 超时同样适用于 managed 延迟接管后的原生 poll 回退。若
    // 元数据分配失败或显式禁用 Hook，原始 libc 调用仍是文档规定的原生阻塞边界。
    const auto deadline =
        deadline_from_timeout(connect_wait_timeout(descriptor));
    const auto token = snapshot_descriptor(descriptor).m_token;
    const int wait_errno =
        manager ? wait_error(wait_for_io(*manager, fd, IOEvent::kWrite,
                                         deadline, token))
                : native_wait(fd, IOEvent::kWrite, deadline, descriptor);
    if (wait_errno != 0) {
        errno = wait_errno;
        return -1;
    }
    DescriptorGuard lifecycle(fd);
    if (snapshot_descriptor(descriptor).m_closed) {
        errno = EBADF;
        return -1;
    }

    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (!s_originals.m_getsockopt ||
        invoke_real(s_originals.m_getsockopt, fd, SOL_SOCKET, SO_ERROR,
                    &socket_error, &error_size) != 0) {
        return -1;
    }
    if (socket_error != 0) {
        errno = socket_error;
        return -1;
    }
    return 0;
}

enum class SleepOutcome { Unavailable, Completed, Interrupted };

/// 本线程复用的睡眠 eventfd（-1 表示尚未创建）。
///
/// ## 为什么要复用
///
/// 旧实现每次 `usleep` 都 `eventfd2` + `WaitFor` + `NotifyClose` + `close`：
/// 实测 s4（5ms 注入）每请求 **284µs sys**，其中两次 syscall（创建/销毁）
/// 与一次 epoll 注册占了绝大部分。
///
/// 复用后每线程只创建一次，后续睡眠只付「注册 + park + 唤醒」。
///
/// ## 为什么是 thread_local 而不是全局
///
/// 全局单例会变成所有线程的争用点（之前试过「全局 eventfd 复用」，
/// s4 反而 −7.8%）。thread_local 无锁，且每个 M 只会有一个 Fiber 在睡眠
/// （同一 M 上的其它 Fiber 在 park 时不占用它）。
///
/// ## 计数语义
///
/// eventfd 是可累加的：如果被外部 `write` 唤醒过，计数会残留，下一次
/// `WaitFor` 会立即返回（假唤醒）。所以复用前必须先 `read` 清空。
thread_local int t_sleep_eventfd = -1;

SleepOutcome cooperative_sleep_for(Duration duration) {
    IOManager* manager = cooperative_manager();
    if (duration <= Duration::zero()) {
        return SleepOutcome::Completed;
    }
    if (!manager) {
        return SleepOutcome::Unavailable;
    }

    int fd = t_sleep_eventfd;
    if (fd < 0) {
        fd = static_cast<int>(
            ::syscall(SYS_eventfd2, 0, EFD_NONBLOCK | EFD_CLOEXEC));
        if (fd < 0) {
            return SleepOutcome::Unavailable;
        }
        t_sleep_eventfd = fd;
    } else {
        // 清空可能残留的计数：正常超时路径计数为 0，这里通常是一次失败的
        // read（EAGAIN），成本可忽略；但必须做，否则会假唤醒。
        std::uint64_t pending = 0;
        (void)::read(fd, &pending, sizeof(pending));
    }

    const auto result = manager->WaitFor(fd, IOEvent::kRead, duration);
    if (result.status == IOWaitStatus::kTimeout ||
        result.status == IOWaitStatus::kReady) {
        return SleepOutcome::Completed;
    }
    errno = wait_error(result);
    return SleepOutcome::Interrupted;
}

/// `Duration` -> `timespec`，供 `nanosleep` 的 `remaining` 输出使用。
///
/// 为什么需要它：`cooperative_sleep_for` 在被唤醒时不返回「已等待多久」，
/// 而 POSIX 要求 `nanosleep` 被打断时把**剩余时间**写回 `rem`。所以调用方
/// 自己记起止时刻，用这个函数把差值转回去。
timespec duration_to_timespec(Duration value) noexcept {
    if (value < Duration::zero()) {
        value = Duration::zero();
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(value - seconds);
    timespec result{};
    result.tv_sec = static_cast<time_t>(seconds.count());
    result.tv_nsec = static_cast<long>(nanos.count());
    return result;
}

Duration saturating_duration(const timespec& value) noexcept {
    using Nanoseconds = std::chrono::nanoseconds;
    const auto max_ns =
        std::chrono::duration_cast<Nanoseconds>(Duration::max()).count();
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    const auto nanoseconds = static_cast<std::uint64_t>(value.tv_nsec);
    const auto maximum = static_cast<std::uint64_t>(max_ns);
    if (seconds > maximum / 1000000000ULL ||
        seconds * 1000000000ULL > maximum - nanoseconds) {
        return Duration::max();
    }
    return std::chrono::duration_cast<Duration>(
        Nanoseconds(seconds * 1000000000ULL + nanoseconds));
}

enum class FcntlArgument { None, Integer, Pointer, Unknown };

FcntlArgument fcntl_argument(int command) noexcept {
    switch (command) {
        case F_GETFD:
        case F_GETFL:
        case F_GETOWN:
#ifdef F_GETSIG
        case F_GETSIG:
#endif
#ifdef F_GETLEASE
        case F_GETLEASE:
#endif
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
#ifdef F_GET_SEALS
        case F_GET_SEALS:
#endif
            return FcntlArgument::None;

        case F_DUPFD:
#ifdef F_DUPFD_CLOEXEC
        case F_DUPFD_CLOEXEC:
#endif
        case F_SETFD:
        case F_SETFL:
        case F_SETOWN:
#ifdef F_SETSIG
        case F_SETSIG:
#endif
#ifdef F_SETLEASE
        case F_SETLEASE:
#endif
#ifdef F_NOTIFY
        case F_NOTIFY:
#endif
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
#ifdef F_ADD_SEALS
        case F_ADD_SEALS:
#endif
            return FcntlArgument::Integer;

        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
#ifdef F_OFD_GETLK
        case F_OFD_GETLK:
        case F_OFD_SETLK:
        case F_OFD_SETLKW:
#endif
#ifdef F_GETOWN_EX
        case F_GETOWN_EX:
        case F_SETOWN_EX:
#endif
#ifdef F_GET_RW_HINT
        case F_GET_RW_HINT:
        case F_SET_RW_HINT:
#endif
#ifdef F_GET_FILE_RW_HINT
        case F_GET_FILE_RW_HINT:
        case F_SET_FILE_RW_HINT:
#endif
            return FcntlArgument::Pointer;
        default:
            return FcntlArgument::Unknown;
    }
}

bool ioctl_has_pointer_argument(unsigned long request) noexcept {
    if (request == FIONBIO || request == FIONREAD) {
        return true;
    }
#ifdef TIOCOUTQ
    if (request == TIOCOUTQ) {
        return true;
    }
#endif
    return false;
}

bool ioctl_has_no_argument(unsigned long request) noexcept {
#ifdef FIOCLEX
    if (request == FIOCLEX) {
        return true;
    }
#endif
#ifdef FIONCLEX
    if (request == FIONCLEX) {
        return true;
    }
#endif
#ifdef TIOCEXCL
    if (request == TIOCEXCL || request == TIOCNXCL) {
        return true;
    }
#endif
    return false;
}

}  // namespace

namespace go2cpp::hook {

void set_enabled(bool enabled_value) noexcept {
    s_enabled.store(enabled_value, std::memory_order_release);
}

bool enabled() noexcept { return hooks_enabled(); }

void bind_io_manager(IOManager* manager) noexcept {
    s_bound_manager = manager;
}

IOManager* bound_io_manager() noexcept { return s_bound_manager; }

void set_connect_timeout(std::chrono::milliseconds timeout) noexcept {
    s_connect_timeout_ms.store(timeout == std::chrono::milliseconds::max()
                                   ? -1
                                   : std::max<std::int64_t>(0, timeout.count()),
                               std::memory_order_release);
}

std::chrono::milliseconds connect_timeout() noexcept {
    const auto value = s_connect_timeout_ms.load(std::memory_order_acquire);
    return value < 0 ? std::chrono::milliseconds::max()
                     : std::chrono::milliseconds(value);
}

ScopedEnable::ScopedEnable() noexcept {
    s_scoped_enable_count.fetch_add(1, std::memory_order_acq_rel);
}

ScopedEnable::~ScopedEnable() {
    if (m_active) {
        s_scoped_enable_count.fetch_sub(1, std::memory_order_acq_rel);
    }
}

ScopedEnable::ScopedEnable(ScopedEnable&& other) noexcept
    : m_active(other.m_active) {
    other.m_active = false;
}

ScopedIOManagerBinding::ScopedIOManagerBinding(IOManager* manager) noexcept
    : m_previous(s_bound_manager) {
    s_bound_manager = manager;
}

ScopedIOManagerBinding::~ScopedIOManagerBinding() {
    s_bound_manager = m_previous;
}


// ---------------------------------------------------------------------------
// 每函数动态开关
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kHookOpCount = static_cast<std::size_t>(HookOp::kCount);
static_assert(kHookOpCount <= 32,
              "HookOp 位图用 uint32_t 承载，最多 32 个操作；超了要换 uint64_t");

/// 默认**全开**：保持与引入开关之前完全一致的行为。
constexpr std::uint32_t kAllHookOpsMask =
    (kHookOpCount >= 32) ? 0xFFFFFFFFu : ((1u << kHookOpCount) - 1u);

std::atomic<std::uint32_t> g_hook_op_mask{kAllHookOpsMask};

constexpr const char* kHookOpNames[kHookOpCount] = {
    "read", "readv", "recv", "recvfrom",
    "write", "writev", "send", "sendto",
    "connect", "accept", "sleep", "nanosleep", "usleep", "clock_nanosleep",
    "poll", "select", "epoll_wait",
    "close", "shutdown", "fcntl", "dup", "dup2",
};

}  // namespace

const char* ToString(HookOp op) noexcept {
    const auto index = static_cast<std::size_t>(op);
    return index < kHookOpCount ? kHookOpNames[index] : "unknown";
}

bool OpFromString(const std::string& name, HookOp* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < kHookOpCount; ++i) {
        if (name == kHookOpNames[i]) {
            *out = static_cast<HookOp>(i);
            return true;
        }
    }
    return false;
}

bool OpEnabled(HookOp op) noexcept {
    const auto bit = static_cast<std::uint32_t>(op);
    if (bit >= kHookOpCount) {
        // 越界视为启用：宁可多 Hook 也不要因为无效枚举把 IO 路径静默关掉。
        return true;
    }
    return (g_hook_op_mask.load(std::memory_order_relaxed) >> bit) & 1u;
}

namespace {

/**
 * @brief 该操作是否**不允许被关闭**。
 * @details 两类：
 *  1. `close` / `shutdown` / `dup` / `dup2` —— 关闭后 fd 元数据与内核状态脱节，
 *     fd 复用时 Hook 拿过期 descriptor 判断类型，行为不可预测；
 *  2. `fcntl` —— 它承载 `F_SETFL`/`F_DUPFD` 等对 fd 标志与描述符表的修改，
 *     关闭后 `FdContext{m_user_nonblock, m_system_nonblock}` 会与实际状态脱节，
 *     后续 `close` 的还原逻辑会写错标志。
 * 这些操作**没有安全的关闭场景**，所以硬拒绝比留一个能开的洞好。
 */
constexpr bool IsNonDisableable(HookOp op) noexcept {
    switch (op) {
        case HookOp::kClose:
        case HookOp::kShutdown:
        case HookOp::kFcntl:
        case HookOp::kDup:
        case HookOp::kDup2:
            return true;
        default:
            return false;
    }
}

/// 不可关闭操作对应位的掩码（用于 SetAllOpsEnabled 保留它们）。
constexpr std::uint32_t NonDisableableMask() noexcept {
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < kHookOpCount; ++i) {
        if (IsNonDisableable(static_cast<HookOp>(i))) {
            mask |= (1u << i);
        }
    }
    return mask;
}

}  // namespace

bool SetOpEnabled(HookOp op, bool enabled) noexcept {
    const auto bit = static_cast<std::uint32_t>(op);
    if (bit >= kHookOpCount) {
        return false;
    }
    if (!enabled && IsNonDisableable(op)) {
        // 硬拒绝：不改变位图，保持启用。调用方可以从返回值知道请求被拒。
        return false;
    }
    if (enabled) {
        g_hook_op_mask.fetch_or(1u << bit, std::memory_order_relaxed);
    } else {
        g_hook_op_mask.fetch_and(~(1u << bit), std::memory_order_relaxed);
    }
    return true;
}

void SetAllOpsEnabled(bool enabled) noexcept {
    if (enabled) {
        g_hook_op_mask.store(kAllHookOpsMask, std::memory_order_relaxed);
        return;
    }
    // 关闭全部时**保留不可关闭操作的位** —— 否则一个 SetAllOpsEnabled(false)
    // 就能绕过硬拒绝。
    g_hook_op_mask.store(NonDisableableMask(), std::memory_order_relaxed);
}

std::uint32_t OpMask() noexcept {
    return g_hook_op_mask.load(std::memory_order_relaxed);
}


// ---------------------------------------------------------------------------
// 配置接入
// ---------------------------------------------------------------------------

// 每个开关的醒目警告。**这个开关是为了诊断，不是为了优化** ——
// 关掉不会让任何东西变快，只会让协程退化成阻塞线程。
namespace {

/**
 * @brief 一个 hook 开关的文档条目（`Config::DumpIni` 用它生成 `#` 注释）。
 *
 * ## 定位
 *
 * `kHookOpDocs` 的数组元素，与 `kHookOpCount` 一一对应。每个条目同时服务两件事：
 * **运行时开关**（按 `op_name` 查）与**配置文档**（`DumpIni` 把 `description`
 * 按 `\n` 拆成多条 `#` 注释写进 ini）。
 *
 * ## 为什么 description 是多行且带警告
 *
 * 配置里必须给出**关闭后果**，而不是一句「是否启用」—— 关掉 `read` 的 hook
 * 会让 Fiber 退化成阻塞线程、整个 worker 停摆。这些警告是排查 hook 问题的
 * 第一手材料（`kHookOpDocs` 的每条都是这个格式）。
 */
struct HookOpDoc final {
    /// @brief 操作名（`"read"`/`"write"`/`"recv"`…），也是 ini 里的键名。
    const char* op_name;
    /// @brief 多行说明；`\n` 分隔，`DumpIni` 按行转成 `#` 注释。
    const char* description;
};

constexpr HookOpDoc kHookOpDocs[kHookOpCount] = {
    {"read",
     "!!! 危险：不建议关闭 read 的 Hook !!!\n"
     "关闭后 read 不再 park Fiber，而是真的阻塞调用它的线程。\n"
     "具体后果：\n"
     "  1. 若在 Fiber 内调用：整个 worker 线程被阻塞，该线程上其余 Fiber 全部停摆；\n"
     "  2. 与 write 开关不对称时：同一连接的收发路径一侧 park、一侧阻塞，吞吐骤降甚至卡住；\n"
     "  3. 已 park 在 read 上的 Fiber 不会被这个开关影响（唤醒路径不查它），\n"
     "     但新发起的 read 会走原生阻塞。\n"
     "只在确认调用方是普通线程、或纯诊断场景下关闭。默认 true 是安全配置。"},
    {"readv",
     "!!! 危险：不建议关闭 readv 的 Hook !!!\n"
     "后果同 read：关闭后在 Fiber 内会阻塞整个 worker 线程；\n"
     "与 write/writev 不对称时可能导致收发路径行为不一致。\n"
     "默认 true 是安全配置。"},
    {"recv",
     "!!! 危险：不建议关闭 recv 的 Hook !!!\n"
     "关闭后 recv 不再 park Fiber，在协程内调用会阻塞整个 worker 线程。\n"
     "注意：recv 常用于 socket，与 send 必须保持对称开关。\n"
     "默认 true 是安全配置。"},
    {"recvfrom",
     "!!! 危险：不建议关闭 recvfrom 的 Hook !!!\n"
     "后果同 recv：在 Fiber 内会阻塞 worker 线程。\n"
     "与 sendto 必须保持对称开关。默认 true 是安全配置。"},
    {"write",
     "!!! 危险：不建议关闭 write 的 Hook !!!\n"
     "关闭后 write 不再 park Fiber，在协程内调用会阻塞整个 worker 线程。\n"
     "具体后果：\n"
     "  1. 对端不读、发送缓冲区满时，write 会卡住整个 worker，而非只是这条流；\n"
     "  2. 与 read 不对称时收发路径行为不一致。\n"
     "默认 true 是安全配置。"},
    {"writev",
     "!!! 危险：不建议关闭 writev 的 Hook !!!\n"
     "后果同 write：在 Fiber 内会阻塞 worker 线程。\n"
     "与 readv 必须保持对称开关。默认 true 是安全配置。"},
    {"send",
     "!!! 危险：不建议关闭 send 的 Hook !!!\n"
     "关闭后 send 不再 park Fiber，在协程内调用会阻塞整个 worker 线程。\n"
     "与 recv 必须保持对称开关。默认 true 是安全配置。"},
    {"sendto",
     "!!! 危险：不建议关闭 sendto 的 Hook !!!\n"
     "后果同 send：在 Fiber 内会阻塞 worker 线程。\n"
     "与 recvfrom 必须保持对称开关。默认 true 是安全配置。"},
    {"connect",
     "!!! 危险：不建议关闭 connect 的 Hook !!!\n"
     "关闭后 connect 不再 park Fiber。\n"
     "具体后果：连接一个慢速/不可达的对端时，整个 worker 线程会被阻塞，\n"
     "直到内核超时（可能数十秒），期间该 P 上的所有 Fiber 停摆。\n"
     "默认 true 是安全配置。"},
    {"accept",
     "!!! 危险：不建议关闭 accept 的 Hook !!!\n"
     "关闭后在无新连接时，accept 会阻塞整个 worker 线程。\n"
     "典型症状：整个调度器看起来「卡住」，实际是一个 worker 卡在 accept 上。\n"
     "默认 true 是安全配置。"},
    {"sleep",
     "!!! 危险：不建议关闭 sleep 的 Hook !!!\n"
     "关闭后 sleep_for / nanosleep 会真实阻塞 worker 线程。\n"
     "具体后果：一个 Fiber 里 sleep 1 秒，该 worker 上的所有 Fiber 都停 1 秒。\n"
     "这是最能暴露「关掉 Hook 的代价」的一个开关，仅供诊断对比使用。\n"
     "默认 true 是安全配置。"},
    {"nanosleep",
     "!!! 危险：不建议关闭 nanosleep 的 Hook !!!\n"
     "关闭后在 Fiber 内调用会真的阻塞整个 worker 线程（该 P 上其余 Fiber 停摆）。\n"
     "与 sleep 同源：开启时走 `cooperative_sleep_for`（eventfd + WaitFor 带超时），\n"
     "不需要 timerfd 桥接。\n"
     "默认 true 是安全配置。"},
    {"usleep",
     "!!! 危险：不建议关闭 usleep 的 Hook !!!\n"
     "后果同 nanosleep。注意 `usleep` 是 POSIX.1-2001 废弃接口，新代码请用 nanosleep。\n"
     "默认 true 是安全配置。"},
    {"clock_nanosleep",
     "!!! 危险：不建议关闭 clock_nanosleep 的 Hook !!!\n"
     "关闭后在 Fiber 内调用会真的阻塞整个 worker 线程。\n"
     "支持 CLOCK_REALTIME / CLOCK_MONOTONIC，含 TIMER_ABSTIME 语义。\n"
     "默认 true 是安全配置。"},
    {"poll",
     "!!! 危险：不建议关闭 poll 的 Hook !!!\n"
     "关闭后 poll 会阻塞 worker 线程直到有事件或超时。\n"
     "典型场景：自带事件循环的第三方库只希望 poll 不被接管 —— 那种情况建议\n"
     "用 BlockingRegion 明确标注，而不是关闭这个开关。\n"
     "与 select / epoll_wait 应保持一致。默认 true 是安全配置。"},
    {"select",
     "!!! 危险：不建议关闭 select 的 Hook !!!\n"
     "后果同 poll：会阻塞 worker 线程。\n"
     "与 poll / epoll_wait 应保持一致。默认 true 是安全配置。"},
    {"epoll_wait",
     "!!! 危险：不建议关闭 epoll_wait 的 Hook !!!\n"
     "关闭后 epoll_wait 会阻塞 worker 线程。\n"
     "注意：IOManager 自身的等待也走 epoll —— 关掉它会让依赖 IOManager 的\n"
     "所有 Fiber 无法被正常调度。**这一项几乎总是应该保持开启**。\n"
     "默认 true 是安全配置。"},
    {"close",
     "!!! 不应关闭 close 的 Hook !!!\n"
     "关闭后关闭的 fd 不会从 FdManager 元数据里摘除。\n"
     "具体后果：fd 号被复用时，Hook 会拿**过期**的 descriptor 做判断\n"
     "（可能把普通文件当 socket，或把新 socket 当已关闭），行为不可预测。\n"
     "**这一项没有安全的关闭场景**，请保持 true。"},
    {"shutdown",
     "!!! 不应关闭 shutdown 的 Hook !!!\n"
     "同 close：跳过 Hook 会让连接状态与元数据脱节，后续读写路径可能误判。\n"
     "**没有安全的关闭场景**，请保持 true。"},
    {"fcntl",
     "!!! 危险：不建议关闭 fcntl 的 Hook !!!\n"
     "关闭后 fd 的非阻塞标志与 Hook 元数据（m_system_nonblock / m_user_nonblock）\n"
     "不再同步。后果：Hook 以为自己设了非阻塞，实际被外部改回阻塞。\n"
     "默认 true 是安全配置。"},
    {"dup",
     "!!! 不应关闭 dup 的 Hook !!!\n"
     "关闭后新 fd 不会继承 descriptor 元数据（token、事件订阅、超时配置）。\n"
     "后果：新 fd 上的读写走不到正确的 park 路径。**请保持 true**。"},
    {"dup2",
     "!!! 不应关闭 dup2 的 Hook !!!\n"
     "同 dup：元数据不继承，且目标 fd 若已被跟踪会造成元数据错位。\n"
     "**请保持 true**。"},
};

}  // namespace

void RegisterHookConfig(::go2cpp::config::Config& config) {
    constexpr const char* kPrefix = "hook.enable_";
    for (std::size_t i = 0; i < kHookOpCount; ++i) {
        const auto op = static_cast<HookOp>(i);
        const std::string key = std::string(kPrefix) + kHookOpDocs[i].op_name;
        // 默认 true = 全开，与引入开关前的行为完全一致。
        auto variable = config.Lookup<bool>(key, true, kHookOpDocs[i].description);
        if (variable == nullptr) {
            continue;   // 同名不同类型冲突：跳过，不改变现状
        }
        // 双向绑定：先按配置当前值设一次，再挂监听器保持后续同步。
        //
        // 注意 `SetOpEnabled` 可能**拒绝**（close/shutdown/fcntl/dup/dup2
        // 不允许关闭）。被拒时必须把配置项改回 true，否则配置文件会显示
        // false 而实际仍在 Hook —— 那种不一致比没有开关更危险。
        if (!SetOpEnabled(op, variable->GetValue())) {
            variable->SetValue(true);
        }
        // 用 weak_ptr 捕获，避免 ConfigVar -> listener -> ConfigVar 的循环引用。
        std::weak_ptr<::go2cpp::config::ConfigVar<bool>> weak_variable = variable;
        variable->AddListener([op, weak_variable](const bool& /*old_value*/,
                                                  const bool& new_value) {
            if (!SetOpEnabled(op, new_value)) {
                // 回滚。递归一次后终止：启用总是被允许，所以第二次不会再回滚。
                if (auto variable_locked = weak_variable.lock()) {
                    variable_locked->SetValue(true);
                }
            }
        });
    }
}

}  // namespace go2cpp::hook

extern "C" {
using go2cpp::hook::HookOp;
using go2cpp::hook::OpEnabled;

void go2cpp_hook_set_enabled(int enabled) noexcept {
    go2cpp::hook::set_enabled(enabled != 0);
}

int go2cpp_hook_is_enabled() noexcept {
    return go2cpp::hook::enabled() ? 1 : 0;
}

void go2cpp_hook_bind_io_manager(void* manager) noexcept {
    go2cpp::hook::bind_io_manager(static_cast<IOManager*>(manager));
}

unsigned int sleep(unsigned int seconds) {
    initialize_originals();
    if (!OpEnabled(HookOp::kSleep)) {
        // 关闭后真的阻塞整个 worker 线程 —— 这正是这个开关要暴露的代价。
        if (s_originals.m_sleep) return s_originals.m_sleep(seconds);
        const timespec request{static_cast<time_t>(seconds), 0};
        timespec remaining{};
        ::nanosleep(&request, &remaining);
        return static_cast<unsigned int>(remaining.tv_sec);
    }
    const SleepOutcome outcome =
        cooperative_sleep_for(std::chrono::seconds(seconds));
    if (outcome == SleepOutcome::Completed) {
        return 0;
    }
    if (outcome == SleepOutcome::Interrupted) {
        return seconds;
    }
    if (!s_originals.m_sleep) {
        errno = ENOSYS;
        return seconds;
    }
    return invoke_native_blocking(s_originals.m_sleep, seconds);
}

int usleep(useconds_t microseconds) {
    initialize_originals();
    if (!OpEnabled(HookOp::kUsleep)) {
        // 关闭后真的阻塞整个 worker 线程 —— 这正是这个开关要暴露的代价。
        if (s_originals.m_usleep) return s_originals.m_usleep(microseconds);
        const timespec request{static_cast<time_t>(microseconds / 1000000),
                               static_cast<long>((microseconds % 1000000) * 1000)};
        ::nanosleep(&request, nullptr);
        return 0;
    }
    const SleepOutcome outcome =
        cooperative_sleep_for(std::chrono::microseconds(microseconds));
    if (outcome == SleepOutcome::Completed) {
        return 0;
    }
    if (outcome == SleepOutcome::Interrupted) {
        // POSIX：被信号打断时返回 -1 且 errno == EINTR（旧实现漏了这一句，
        // 会让调用方拿到 -1 但 errno 是上一次的值，无法区分真错误与打断）。
        errno = EINTR;
        return -1;
    }
    if (!s_originals.m_usleep) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_usleep, microseconds);
}

int nanosleep(const timespec* request, timespec* remaining) {
    initialize_originals();
    if (!request || request->tv_sec < 0 || request->tv_nsec < 0 ||
        request->tv_nsec >= 1000000000L) {
        if (!s_originals.m_nanosleep) {
            errno = EINVAL;
            return -1;
        }
        return invoke_real(s_originals.m_nanosleep, request, remaining);
    }
    if (!OpEnabled(HookOp::kNanosleep)) {
        // 关闭后真的阻塞整个 worker 线程。
        if (s_originals.m_nanosleep) {
            return s_originals.m_nanosleep(request, remaining);
        }
        struct timespec native_remaining{};
        ::nanosleep(request, &native_remaining);
        if (remaining) {
            *remaining = native_remaining;
        }
        return 0;
    }
    const Duration requested = saturating_duration(*request);
    const auto started = std::chrono::steady_clock::now();
    const SleepOutcome outcome = cooperative_sleep_for(requested);
    if (outcome == SleepOutcome::Completed) {
        if (remaining) {
            *remaining = timespec{};
        }
        return 0;
    }
    if (outcome == SleepOutcome::Interrupted) {
        // POSIX：被打断时返回 -1、errno == EINTR，并把**剩余时间**写回 rem。
        // `cooperative_sleep_for` 不返回「已等待多久」，所以这里自己计时算差值。
        errno = EINTR;
        if (remaining) {
            const auto elapsed = std::chrono::duration_cast<Duration>(
                std::chrono::steady_clock::now() - started);
            *remaining = duration_to_timespec(requested > elapsed ? requested - elapsed
                                                                  : Duration::zero());
        }
        return -1;
    }
    if (!s_originals.m_nanosleep) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_nanosleep, request, remaining);
}

/// `clock_nanosleep` 与 `nanosleep` 的两处语义差异（容易写错）：
///
/// 1. **返回值**：成功返回 0，失败返回 **errno 本身**（不是 -1 + errno）。
///    所以下面所有错误分支都 `return EINVAL;` 而不是 `errno = EINVAL; return -1;`
/// 2. **`TIMER_ABSTIME`**：`request` 是**绝对时刻**。此时：
///    - 目标时刻已过 → **立即返回 0**（不 park、不睡）
///    - `rem` **不使用**（POSIX 明确要求）
///    非绝对时间才把 `rem` 当作剩余时间输出。
int clock_nanosleep(clockid_t clock_id, int flags, const timespec* request,
                    timespec* remaining) {
    initialize_originals();
    if (!request || request->tv_nsec < 0 || request->tv_nsec >= 1000000000L) {
        return EINVAL;
    }
    // 只支持这两个时钟；其它（如 CLOCK_PROCESS_CPUTIME_ID）本层不接管。
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) {
        return EINVAL;
    }
    if (!OpEnabled(HookOp::kClockNanosleep)) {
        if (s_originals.m_clock_nanosleep) {
            return s_originals.m_clock_nanosleep(clock_id, flags, request, remaining);
        }
        return ENOSYS;
    }

    const bool absolute = (flags & TIMER_ABSTIME) != 0;
    Duration wait_for = Duration::zero();
    if (absolute) {
        // 绝对时刻减去「现在」才是要等多久；已经过了就立即返回 0。
        timespec now{};
        if (::clock_gettime(clock_id, &now) != 0) {
            return errno;
        }
        const Duration target = saturating_duration(*request);
        const Duration current = saturating_duration(now);
        if (target <= current) {
            return 0;
        }
        wait_for = target - current;
    } else {
        if (request->tv_sec < 0) {
            return EINVAL;
        }
        wait_for = saturating_duration(*request);
    }

    const auto started = std::chrono::steady_clock::now();
    const SleepOutcome outcome = cooperative_sleep_for(wait_for);
    if (outcome == SleepOutcome::Completed) {
        return 0;
    }
    if (outcome == SleepOutcome::Interrupted) {
        if (!absolute && remaining) {
            const auto elapsed = std::chrono::duration_cast<Duration>(
                std::chrono::steady_clock::now() - started);
            *remaining = duration_to_timespec(wait_for > elapsed ? wait_for - elapsed
                                                                 : Duration::zero());
        }
        return EINTR;
    }
    // 退化：没有 IOManager 时走原生（原生的返回值语义就是 errno）。
    if (!s_originals.m_clock_nanosleep) {
        return ENOSYS;
    }
    return s_originals.m_clock_nanosleep(clock_id, flags, request, remaining);
}

// poll/select/epoll_wait Hook 保持 libc ABI 和 timeout、EINTR、信号掩码、
// revents 语义，不把一次多路等待偷偷拆成多个顺序等待。需要在 Fiber 中
// 通过 epoll 注册多个 fd 并由任一事件唤醒时，显式调用 IOManager::WaitAny/
// WaitMany；普通线程继续使用原生 poll/select，长时间 native 调用仍由
// BlockingRegion/sysmon 记录其 M 状态。
int poll(struct pollfd* descriptors, nfds_t count, int timeout) {
    initialize_originals();
    if (!s_originals.m_poll) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_poll, descriptors, count,
                                  timeout);
}

int ppoll(struct pollfd* descriptors, nfds_t count, const timespec* timeout,
          const sigset_t* signal_mask) {
    initialize_originals();
    if (!s_originals.m_ppoll) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_ppoll, descriptors, count,
                                  timeout, signal_mask);
}

int select(int descriptor_count, fd_set* read_set, fd_set* write_set,
           fd_set* exception_set, timeval* timeout) {
    initialize_originals();
    if (!s_originals.m_select) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_select, descriptor_count,
                                  read_set, write_set, exception_set, timeout);
}

int pselect(int descriptor_count, fd_set* read_set, fd_set* write_set,
            fd_set* exception_set, const timespec* timeout,
            const sigset_t* signal_mask) {
    initialize_originals();
    if (!s_originals.m_pselect) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_pselect, descriptor_count,
                                  read_set, write_set, exception_set, timeout,
                                  signal_mask);
}

int epoll_wait(int epoll_descriptor, epoll_event* events, int max_events,
               int timeout) {
    initialize_originals();
    if (!s_originals.m_epoll_wait) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_epoll_wait, epoll_descriptor,
                                  events, max_events, timeout);
}

int epoll_pwait(int epoll_descriptor, epoll_event* events, int max_events,
                int timeout, const sigset_t* signal_mask) {
    initialize_originals();
    if (!s_originals.m_epoll_pwait) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_epoll_pwait, epoll_descriptor,
                                  events, max_events, timeout, signal_mask);
}

int socket(int domain, int type, int protocol) {
    initialize_originals();
    if (!s_originals.m_socket) {
        errno = ENOSYS;
        return -1;
    }
    const int fd = invoke_real(s_originals.m_socket, domain, type, protocol);
    if (fd >= 0 && cooperative_manager()) {
        try_register_new_socket(fd, (type & SOCK_NONBLOCK) != 0);
    }
    return fd;
}

int socketpair(int domain, int type, int protocol, int descriptors[2]) {
    initialize_originals();
    if (!s_originals.m_socketpair) {
        errno = ENOSYS;
        return -1;
    }
    const int result = invoke_real(s_originals.m_socketpair, domain, type,
                                   protocol, descriptors);
    if (result == 0 && descriptors && cooperative_manager()) {
        const bool user_nonblocking = (type & SOCK_NONBLOCK) != 0;
        try_register_new_socket(descriptors[0], user_nonblocking);
        try_register_new_socket(descriptors[1], user_nonblocking);
    }
    return result;
}

int connect(int fd, const sockaddr* address, socklen_t length) {
    initialize_originals();
    if (!OpEnabled(HookOp::kConnect)) {
        if (s_originals.m_connect) return s_originals.m_connect(fd, address, length);
        return static_cast<int>(::syscall(SYS_connect, fd, address, length));
    }
    return cooperative_connect(fd, address, length);
}

int accept(int fd, sockaddr* address, socklen_t* length) {
    initialize_originals();
    if (!OpEnabled(HookOp::kAccept)) {
        // 关闭后直接走原生 accept：**不** try_register_new_socket ——
        // 新 fd 不进 FdManager，后续读写也会落到原生路径，保持一致。
        if (s_originals.m_accept) return s_originals.m_accept(fd, address, length);
        errno = ENOSYS;
        return -1;
    }
    const int accepted = cooperative_io<int>(fd, s_originals.m_accept,
                                              IOEvent::kRead, false, address,
                                              length);
    if (accepted >= 0 && cooperative_manager()) {
        try_register_new_socket(accepted, false);
    }
    return accepted;
}

int accept4(int fd, sockaddr* address, socklen_t* length, int flags) {
    initialize_originals();
    const int accepted = cooperative_io<int>(
        fd, s_originals.m_accept4, IOEvent::kRead,
        (flags & SOCK_NONBLOCK) != 0, address, length,
        flags);
    if (accepted >= 0 && cooperative_manager()) {
        try_register_new_socket(accepted, (flags & SOCK_NONBLOCK) != 0);
    }
    return accepted;
}

ssize_t read(int fd, void* buffer, size_t count) {
    initialize_originals();
    if (!OpEnabled(HookOp::kRead)) {
        if (s_originals.m_read) return s_originals.m_read(fd, buffer, count);
        return static_cast<ssize_t>(::syscall(SYS_read, fd, buffer, count));
    }
    if (s_resolving && !s_originals.m_read) {
        return static_cast<ssize_t>(::syscall(SYS_read, fd, buffer, count));
    }
    return cooperative_io<ssize_t>(fd, s_originals.m_read, IOEvent::kRead,
                                    false, buffer, count);
}

ssize_t readv(int fd, const iovec* vectors, int count) {
    initialize_originals();
    if (!OpEnabled(HookOp::kReadv)) {
        if (s_originals.m_readv) return s_originals.m_readv(fd, vectors, count);
        errno = ENOSYS;
        return static_cast<ssize_t>(-1);
    }
    return cooperative_io<ssize_t>(fd, s_originals.m_readv, IOEvent::kRead,
                                    false, vectors, count);
}

ssize_t recv(int fd, void* buffer, size_t length, int flags) {
    initialize_originals();
    if (!OpEnabled(HookOp::kRecv)) {
        if (s_originals.m_recv) return s_originals.m_recv(fd, buffer, length, flags);
        return static_cast<ssize_t>(::syscall(SYS_recvfrom, fd, buffer, length, flags, nullptr, nullptr));
    }
    // MSG_WAITALL 需要跨多次读取累计字节数；有限 Hook 不模拟该契约。保留
    // libc 的确切行为，不要在一次唤醒后静默返回短读。
#ifdef MSG_WAITALL
    if ((flags & MSG_WAITALL) != 0 && s_originals.m_recv) {
        if (cooperative_manager() != nullptr) {
            errno = ENOTSUP;
            return static_cast<ssize_t>(-1);
        }
        return invoke_native_blocking(s_originals.m_recv, fd, buffer, length, flags);
    }
#endif
#ifdef MSG_OOB
    // EPOLLPRI 有意不属于当前有限的就绪能力。不要让 managed G 等待 poller
    // 无法报告的紧急数据事件；普通线程保留 libc 的原生语义。
    if ((flags & MSG_OOB) != 0 && cooperative_manager() != nullptr) {
        errno = ENOTSUP;
        return static_cast<ssize_t>(-1);
    }
#endif
    return cooperative_io<ssize_t>(fd, s_originals.m_recv, IOEvent::kRead,
                                    has_dontwait(flags), buffer, length,
                                    flags);
}

ssize_t recvfrom(int fd, void* buffer, size_t length, int flags,
                 sockaddr* source, socklen_t* source_length) {
    initialize_originals();
    if (!OpEnabled(HookOp::kRecvfrom)) {
        // 关闭后连 MSG_WAITALL / MSG_OOB 的特殊分支也一并跳过 ——
        // 它们存在的意义是"在协程里正确处理这些标志"，不走 hook 就不需要。
        if (s_originals.m_recvfrom) {
            return s_originals.m_recvfrom(fd, buffer, length, flags, source,
                                          source_length);
        }
        errno = ENOSYS;
        return static_cast<ssize_t>(-1);
    }
#ifdef MSG_WAITALL
    if ((flags & MSG_WAITALL) != 0 && s_originals.m_recvfrom) {
        if (cooperative_manager() != nullptr) {
            errno = ENOTSUP;
            return static_cast<ssize_t>(-1);
        }
        return invoke_native_blocking(s_originals.m_recvfrom, fd, buffer, length, flags,
                                      source, source_length);
    }
#endif
#ifdef MSG_OOB
    if ((flags & MSG_OOB) != 0 && cooperative_manager() != nullptr) {
        errno = ENOTSUP;
        return static_cast<ssize_t>(-1);
    }
#endif
    return cooperative_io<ssize_t>(
        fd, s_originals.m_recvfrom, IOEvent::kRead, has_dontwait(flags), buffer,
        length, flags, source, source_length);
}

ssize_t recvmsg(int fd, msghdr* message, int flags) {
    initialize_originals();
#ifdef MSG_WAITALL
    if ((flags & MSG_WAITALL) != 0 && s_originals.m_recvmsg) {
        if (cooperative_manager() != nullptr) {
            errno = ENOTSUP;
            return static_cast<ssize_t>(-1);
        }
        return invoke_native_blocking(s_originals.m_recvmsg, fd, message, flags);
    }
#endif
#ifdef MSG_OOB
    if ((flags & MSG_OOB) != 0 && cooperative_manager() != nullptr) {
        errno = ENOTSUP;
        return static_cast<ssize_t>(-1);
    }
#endif
    return cooperative_io<ssize_t>(fd, s_originals.m_recvmsg, IOEvent::kRead,
                                    has_dontwait(flags), message, flags);
}

ssize_t write(int fd, const void* buffer, size_t count) {
    initialize_originals();
    if (!OpEnabled(HookOp::kWrite)) {
        if (s_originals.m_write) return s_originals.m_write(fd, buffer, count);
        return static_cast<ssize_t>(::syscall(SYS_write, fd, buffer, count));
    }
    if (s_resolving && !s_originals.m_write) {
        return static_cast<ssize_t>(::syscall(SYS_write, fd, buffer, count));
    }
    return cooperative_io<ssize_t>(fd, s_originals.m_write, IOEvent::kWrite,
                                    false, buffer, count);
}

ssize_t writev(int fd, const iovec* vectors, int count) {
    initialize_originals();
    if (!OpEnabled(HookOp::kWritev)) {
        if (s_originals.m_writev) return s_originals.m_writev(fd, vectors, count);
        errno = ENOSYS;
        return static_cast<ssize_t>(-1);
    }
    return cooperative_io<ssize_t>(fd, s_originals.m_writev, IOEvent::kWrite,
                                    false, vectors, count);
}

ssize_t send(int fd, const void* buffer, size_t length, int flags) {
    initialize_originals();
    if (!OpEnabled(HookOp::kSend)) {
        if (s_originals.m_send) return s_originals.m_send(fd, buffer, length, flags);
        return static_cast<ssize_t>(::syscall(SYS_sendto, fd, buffer, length, flags, nullptr, 0));
    }
    return cooperative_io<ssize_t>(fd, s_originals.m_send, IOEvent::kWrite,
                                    has_dontwait(flags), buffer, length,
                                    flags);
}

ssize_t sendto(int fd, const void* buffer, size_t length, int flags,
               const sockaddr* destination, socklen_t destination_length) {
    initialize_originals();
    if (!OpEnabled(HookOp::kSendto)) {
        if (s_originals.m_sendto) {
            return s_originals.m_sendto(fd, buffer, length, flags, destination,
                                         destination_length);
        }
        errno = ENOSYS;
        return static_cast<ssize_t>(-1);
    }
    return cooperative_io<ssize_t>(
        fd, s_originals.m_sendto, IOEvent::kWrite, has_dontwait(flags), buffer,
        length, flags, destination, destination_length);
}

ssize_t sendmsg(int fd, const msghdr* message, int flags) {
    initialize_originals();
    return cooperative_io<ssize_t>(fd, s_originals.m_sendmsg, IOEvent::kWrite,
                                    has_dontwait(flags), message, flags);
}

int close(int fd) {
    initialize_originals();
    ClosePlan plan;
    {
        DescriptorGuard lifecycle(fd);
        plan = prepare_close(fd);
        notify_before_close(fd);
    }
    // 不要在真实 close 期间持有进程级描述符生命周期门；SO_LINGER 和文件系统
    // 描述符可能在此处阻塞。
    const int result = s_originals.m_close
                           ? invoke_native_blocking(s_originals.m_close, fd)
                           : invoke_native_blocking(&raw_close_fallback, fd);
    {
        DescriptorGuard lifecycle(fd);
        finish_close(fd, plan.m_descriptor);
    }
    return result;
}

int dup(int old_fd) {
    initialize_originals();
    if (!s_originals.m_dup) {
        errno = ENOSYS;
        return -1;
    }
    DescriptorGuard lifecycle(old_fd);
    const int new_fd = invoke_real(s_originals.m_dup, old_fd);
    if (new_fd >= 0) {
        try_clone_descriptor(old_fd, new_fd);
    }
    return new_fd;
}

int dup2(int old_fd, int new_fd) {
    initialize_originals();
    if (!s_originals.m_dup2) {
        errno = ENOSYS;
        return -1;
    }
    DescriptorGuard lifecycle(new_fd);
    // dup2(old, old) 按文档是空操作。此时不要使描述符 token 失效，也不要
    // 唤醒无关的 waiter。
    if (old_fd == new_fd || new_fd < 0 || !source_fd_is_valid(old_fd)) {
        return invoke_real(s_originals.m_dup2, old_fd, new_fd);
    }

    // 目标由内核作为 dup2 的一部分关闭。在持有描述符生命周期门时发布关闭，
    // 之后替换系统调用才不会为旧描述产生过期就绪事件。
    const ClosePlan plan = prepare_close(new_fd);
    notify_before_close(new_fd);
    const int result = invoke_real(s_originals.m_dup2, old_fd, new_fd);
    if (result >= 0) {
        finish_close(new_fd, plan.m_descriptor);
        try_clone_descriptor(old_fd, result);
    } else {
        // 有效源和有效目标通常使 Linux 上的 dup2 不会失败。若内核仍拒绝操作，
        // 保留元数据。
        restore_failed_replacement(new_fd, plan.m_descriptor);
    }
    return result;
}

int dup3(int old_fd, int new_fd, int flags) {
    initialize_originals();
    if (!s_originals.m_dup3) {
        errno = ENOSYS;
        return -1;
    }
    DescriptorGuard lifecycle(new_fd);
    // dup3 会在不触碰目标的情况下拒绝相同描述符和未知标志。对这些确定性失败
    // 不要发出伪关闭通知；源校验也让 dup3(-1, target, ...) 保持透明。
    if (old_fd == new_fd || new_fd < 0 ||
        (flags & ~O_CLOEXEC) != 0 || !source_fd_is_valid(old_fd)) {
        return invoke_real(s_originals.m_dup3, old_fd, new_fd, flags);
    }

    const ClosePlan plan = prepare_close(new_fd);
    notify_before_close(new_fd);
    const int result = invoke_real(s_originals.m_dup3, old_fd, new_fd, flags);
    if (result >= 0) {
        finish_close(new_fd, plan.m_descriptor);
        try_clone_descriptor(old_fd, result);
    } else {
        restore_failed_replacement(new_fd, plan.m_descriptor);
    }
    return result;
}

int fcntl(int fd, int command, ...) {
    initialize_originals();
    if (!s_originals.m_fcntl) {
        errno = ENOSYS;
        return -1;
    }

    const FcntlArgument kind = fcntl_argument(command);
    if (kind == FcntlArgument::Unknown) {
        // 读取未知的可变参数属于未定义行为。需要额外命令的宿主必须补充其
        // 确切的参数契约。
        errno = ENOTSUP;
        return -1;
    }
    va_list arguments;
    va_start(arguments, command);
    if (kind == FcntlArgument::Integer) {
        const int argument = va_arg(arguments, int);
        va_end(arguments);
        DescriptorGuard lifecycle(fd);
        auto descriptor = descriptor_for(fd);
        if (command == F_SETFL && descriptor) {
            const bool user_nonblocking = (argument & O_NONBLOCK) != 0;
            const int result = invoke_real(s_originals.m_fcntl, fd, command,
                                           argument | O_NONBLOCK);
            if (result == 0) {
                std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
                descriptor->m_open->m_user_nonblocking = user_nonblocking;
                descriptor->m_open->m_system_nonblocking = true;
            }
            return result;
        }
        const int result =
            invoke_real(s_originals.m_fcntl, fd, command, argument);
        if (result >= 0 &&
            (command == F_DUPFD
#ifdef F_DUPFD_CLOEXEC
             || command == F_DUPFD_CLOEXEC
#endif
             )) {
            try_clone_descriptor(fd, result);
        }
        return result;
    }
    if (kind == FcntlArgument::Pointer) {
        void* argument = va_arg(arguments, void*);
        va_end(arguments);
        DescriptorGuard lifecycle(fd);
        return invoke_real(s_originals.m_fcntl, fd, command, argument);
    }
    va_end(arguments);
    DescriptorGuard lifecycle(fd);
    const int result = invoke_real(s_originals.m_fcntl, fd, command);
    if (command == F_GETFL && result >= 0) {
        if (auto descriptor = descriptor_for(fd)) {
            std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
            return descriptor->m_open->m_user_nonblocking
                       ? result | O_NONBLOCK
                       : result & ~O_NONBLOCK;
        }
    }
    return result;
}

int ioctl(int fd, unsigned long request, ...) {
    initialize_originals();
    if (!s_originals.m_ioctl) {
        errno = ENOSYS;
        return -1;
    }
    if (!ioctl_has_pointer_argument(request)) {
        DescriptorGuard lifecycle(fd);
        if (ioctl_has_no_argument(request)) {
            return invoke_real(s_originals.m_ioctl, fd, request);
        }
        errno = ENOTSUP;
        return -1;
    }

    va_list arguments;
    va_start(arguments, request);
    void* argument = va_arg(arguments, void*);
    va_end(arguments);
    if (request == FIONBIO) {
        DescriptorGuard lifecycle(fd);
        auto descriptor = descriptor_for(fd);
        if (descriptor && argument) {
            // 先让内核校验并读取调用者指针。Wrapper 侧解引用会把正常的 EFAULT
            // 契约变成无效用户地址导致的进程崩溃。
            const int result =
                invoke_real(s_originals.m_ioctl, fd, request, argument);
            if (result != 0) {
                return result;
            }

            // 通过内核可见标志恢复请求的状态，再恢复运行时的 O_NONBLOCK 位。
            // 这样既不触碰用户指针，又保留 cooperative_io() 使用的共享打开
            // 描述元数据。如果原始 fcntl 不可用或失败，最后尝试使用本地
            // FIONBIO 请求恢复；失败情况在下方按保守策略记录。
            bool user_nonblocking = false;
            bool system_nonblocking = false;
            if (s_originals.m_fcntl) {
                const int flags =
                    invoke_real(s_originals.m_fcntl, fd, F_GETFL);
                if (flags >= 0) {
                    user_nonblocking = (flags & O_NONBLOCK) != 0;
                    system_nonblocking = user_nonblocking;
                    if (!system_nonblocking &&
                        invoke_real(s_originals.m_fcntl, fd, F_SETFL,
                                    flags | O_NONBLOCK) == 0) {
                        system_nonblocking = true;
                    }
                }
            }
            if (!system_nonblocking) {
                int force_nonblocking = 1;
                system_nonblocking =
                    invoke_real(s_originals.m_ioctl, fd, FIONBIO,
                                &force_nonblocking) == 0;
            }
            {
                std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
                descriptor->m_open->m_user_nonblocking = user_nonblocking;
                descriptor->m_open->m_system_nonblocking =
                    system_nonblocking;
            }
            return result;
        }
    }
    DescriptorGuard lifecycle(fd);
    return invoke_real(s_originals.m_ioctl, fd, request, argument);
}

int getsockopt(int fd, int level, int option, void* value,
               socklen_t* length) {
    initialize_originals();
    if (!s_originals.m_getsockopt) {
        errno = ENOSYS;
        return -1;
    }
    DescriptorGuard lifecycle(fd);
    return invoke_real(s_originals.m_getsockopt, fd, level, option, value,
                       length);
}

int setsockopt(int fd, int level, int option, const void* value,
               socklen_t length) {
    initialize_originals();
    if (!s_originals.m_setsockopt) {
        errno = ENOSYS;
        return -1;
    }
    DescriptorGuard lifecycle(fd);
    const int result = invoke_real(s_originals.m_setsockopt, fd, level, option,
                                   value, length);
    if (result != 0 || level != SOL_SOCKET ||
        (option != SO_RCVTIMEO && option != SO_SNDTIMEO) || !value ||
        length < sizeof(timeval)) {
        return result;
    }

    auto descriptor = descriptor_for(fd);
    if (!descriptor && cooperative_manager()) {
        descriptor = try_adopt_socket(fd);
    }
    if (descriptor) {
        const auto timeout = timeval_duration(*static_cast<const timeval*>(value));
        std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
        if (option == SO_RCVTIMEO) {
            descriptor->m_open->m_receive_timeout = timeout;
        } else {
            descriptor->m_open->m_send_timeout = timeout;
        }
    }
    return result;
}

}  // extern "C"

// ============================================================================
// 原始调用 API（`*_f`）实现
//
// 每个 `_f` 直接调 `s_originals.m_xxx`（`dlsym(RTLD_NEXT)` 拿到的真实 libc），
// **绝不调同名 libc 函数** —— 那会再次进入 hook，造成无限递归。
//
// 它们不读 `hooks_enabled()`、不碰 IOManager、不 park Fiber，
// 所以在普通线程与 Fiber 里行为一致：都是 libc 的原始阻塞语义。
// ============================================================================

namespace {

/// 原始函数指针不可用（dlsym 失败）时统一报 ENOSYS。
bool RawUnavailable(const void* function) noexcept {
    if (function == nullptr) {
        errno = ENOSYS;
        return true;
    }
    return false;
}

}  // namespace

ssize_t read_f(int fd, void* buffer, std::size_t count) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_read))) return -1;
    return s_originals.m_read(fd, buffer, count);
}

ssize_t readv_f(int fd, const struct iovec* iov, int iovcount) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_readv))) return -1;
    return s_originals.m_readv(fd, iov, iovcount);
}

ssize_t recv_f(int fd, void* buffer, std::size_t length, int flags) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_recv))) return -1;
    return s_originals.m_recv(fd, buffer, length, flags);
}

ssize_t recvfrom_f(int fd, void* buffer, std::size_t length, int flags,
                   struct sockaddr* address, socklen_t* address_length) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_recvfrom))) return -1;
    return s_originals.m_recvfrom(fd, buffer, length, flags, address, address_length);
}

ssize_t write_f(int fd, const void* buffer, std::size_t count) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_write))) return -1;
    return s_originals.m_write(fd, buffer, count);
}

ssize_t writev_f(int fd, const struct iovec* iov, int iovcount) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_writev))) return -1;
    return s_originals.m_writev(fd, iov, iovcount);
}

ssize_t send_f(int fd, const void* buffer, std::size_t length, int flags) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_send))) return -1;
    return s_originals.m_send(fd, buffer, length, flags);
}

ssize_t sendto_f(int fd, const void* buffer, std::size_t length, int flags,
                 const struct sockaddr* destination, socklen_t address_length) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_sendto))) return -1;
    return s_originals.m_sendto(fd, buffer, length, flags, destination, address_length);
}

int connect_f(int fd, const struct sockaddr* address, socklen_t address_length) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_connect))) return -1;
    return s_originals.m_connect(fd, address, address_length);
}

int accept_f(int fd, struct sockaddr* address, socklen_t* address_length) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_accept))) return -1;
    // 注意：**不**调 try_register_new_socket —— 原始版本产生的 fd 不进 FdManager，
    // 后续读写也走原生路径，与「显式不 hook」的语义一致。
    return s_originals.m_accept(fd, address, address_length);
}

int nanosleep_f(const timespec* request, timespec* remaining) {
    initialize_originals();
    if (s_originals.m_nanosleep != nullptr) {
        return s_originals.m_nanosleep(request, remaining);
    }
    errno = ENOSYS;
    return -1;
}

int usleep_f(unsigned int microseconds) {
    initialize_originals();
    if (s_originals.m_usleep != nullptr) {
        return s_originals.m_usleep(microseconds);
    }
    errno = ENOSYS;
    return -1;
}

int clock_nanosleep_f(int clock_id, int flags, const timespec* request,
                      timespec* remaining) {
    initialize_originals();
    if (s_originals.m_clock_nanosleep != nullptr) {
        return s_originals.m_clock_nanosleep(clock_id, flags, request, remaining);
    }
    return ENOSYS;
}

unsigned int sleep_f(unsigned int seconds) {
    initialize_originals();
    if (s_originals.m_sleep != nullptr) {
        return s_originals.m_sleep(seconds);
    }
    // 退化路径：用 nanosleep 实现 sleep 的语义（返回未睡完的秒数）。
    const timespec request{static_cast<time_t>(seconds), 0};
    timespec remaining{};
    if (::nanosleep(&request, &remaining) == 0) {
        return 0;
    }
    return static_cast<unsigned int>(remaining.tv_sec);
}

int poll_f(struct pollfd* descriptors, nfds_t count, int timeout) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_poll))) return -1;
    return s_originals.m_poll(descriptors, count, timeout);
}

int select_f(int descriptor_count, fd_set* read_set, fd_set* write_set,
             fd_set* except_set, struct timeval* timeout) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_select))) return -1;
    return s_originals.m_select(descriptor_count, read_set, write_set, except_set,
                                timeout);
}

int epoll_wait_f(int epoll_fd, struct epoll_event* events, int max_events,
                 int timeout) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_epoll_wait))) return -1;
    return s_originals.m_epoll_wait(epoll_fd, events, max_events, timeout);
}

int close_f(int fd) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_close))) return -1;
    // 注意：**不**清理 FdManager 元数据 —— 那正是「显式不 hook」的代价。
    return s_originals.m_close(fd);
}

int shutdown_f(int fd, int how) {
    // `shutdown` 在本层**从未被 hook**（Originals 里没有对应项，只有
    // hook.enable_shutdown 这个配置项是配置表完整性遗留）。
    // 所以 ::shutdown 就是原始版本，直接调它即可。
    return ::shutdown(fd, how);
}

int fcntl_f(int fd, int command, ...) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_fcntl))) return -1;

    // 变参命令按与 hook 版本完全相同的契约解析；未知命令返回 ENOTSUP，
    // 不读取未知类型的参数（那是未定义行为）。
    const FcntlArgument kind = fcntl_argument(command);
    if (kind == FcntlArgument::Unknown) {
        errno = ENOTSUP;
        return -1;
    }

    va_list arguments;
    va_start(arguments, command);
    int result = -1;
    if (kind == FcntlArgument::Integer) {
        const int argument = va_arg(arguments, int);
        va_end(arguments);
        result = s_originals.m_fcntl(fd, command, argument);
    } else {
        void* argument = va_arg(arguments, void*);
        va_end(arguments);
        result = s_originals.m_fcntl(fd, command, argument);
    }
    return result;
}

int dup_f(int fd) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_dup))) return -1;
    return s_originals.m_dup(fd);
}

int dup2_f(int fd, int new_fd) {
    initialize_originals();
    if (RawUnavailable(reinterpret_cast<const void*>(s_originals.m_dup2))) return -1;
    return s_originals.m_dup2(fd, new_fd);
}
