# G5 · IO 后端：`io` / `io_backend` / `fd_manager` / `hook`

> 依赖：`io` → `Scheduler`（park/wake）、`Context`（取消）、`Timer`（deadline）、`DescriptorRegistry`（fd 代际）；
> `hook` → `io` + `thread_policy`（线程级 Hook 模式）；`fd_manager` 几乎不依赖任何东西（只依赖 libc 类型）。
> 规模（实测）：`network/io.hpp` 314 行 + `src/network/io.cpp` 2642 行；
> `network/io_backend.hpp` 87 + `src/network/io_backend.cpp` 228；
> `network/hook.hpp` 102 + `src/network/hook.cpp` 2161；
> `network/fd_manager.hpp` 69 + `src/network/fd_manager.cpp` 72。

---

## 0. 一句话定位

把「阻塞式 socket API」变成「协程式 API」需要四件东西：
**事件循环**（`IOManager` 的 poller 线程）、**可替换的多路复用后端**（`IOBackend`）、
**fd 元数据**（`fd_manager`）、以及**拦截层**（`hook`，用 `dlsym(RTLD_NEXT)` 包装 31 个 libc 函数）。
`IOManager` 自己拥有一个 `Scheduler`，所以它也提供 `go()`。

---

## 1. `IOManager`（`io`）

### 1.1 公开 API（实读签名）

```cpp
class IOManager final {
  explicit IOManager(SchedulerConfig config = {}, IOBackendPtr backend = {});
  ~IOManager();                                    // 不可拷贝
  bool start();  void shutdown();  bool is_running() const noexcept;
  std::shared_ptr<Task> go(Task::Function);

  WaitResult wait(int fd, IOEvent event, std::optional<TimePoint> deadline = std::nullopt,
                  ContextPtr context = {}, DescriptorTokenPtr expected_descriptor = {});
  WaitResult wait_for(int fd, IOEvent event, Duration timeout, ContextPtr context = {});

  WaitAnyResult  wait_any (const std::vector<WaitRequest>&, std::optional<TimePoint> = {}, ContextPtr = {});
  WaitAnyResult  wait_any_for (const std::vector<WaitRequest>&, Duration, ContextPtr = {});
  WaitManyResult wait_many(const std::vector<WaitRequest>&, std::optional<TimePoint> = {}, ContextPtr = {});
  WaitManyResult wait_many_for(const std::vector<WaitRequest>&, Duration, ContextPtr = {});

  bool cancel(int fd, IOEvent event);  bool cancel_all(int fd);
  bool notify_close(int fd);           static void NotifyCloseAll(int fd) noexcept;
  Scheduler& scheduler() noexcept;
  static IOManager* current() noexcept;            // 拥有 current_scheduler() 的那个
};
// 结果类型
enum class IOEvent : uint8_t { kRead = 1, kWrite = 2 };
enum class WaitStatus : uint8_t { kReady = 1, kTimeout, kCancelled, kClosed, kError };
struct WaitResult     { WaitStatus status; int system_error; bool ready(); };
struct WaitRequest    { int fd{-1}; IOEvent event{kRead}; DescriptorTokenPtr expected_descriptor; };
struct WaitAnyResult  { WaitStatus status; int system_error; std::size_t index{kNoIndex}; int fd; IOEvent event; };
struct WaitManyResult { WaitStatus status; int system_error; std::vector<std::size_t> ready_indices; };
```

`WaitAnyResult::kNoIndex = static_cast<std::size_t>(-1)` 是「无就绪项」哨兵。

### 1.2 线程结构（关键事实）

- `IOManager` 内部持有一个 `Scheduler m_scheduler` + `shared_ptr<State> m_state`。
- `start()`：先 `m_scheduler.start()`，再由 `State::start()` 创建**专用事件循环线程**
  `m_poller = std::thread([self]{ self->poll_loop(); })`。
- 因此 epoll 等待**既不在 M（worker 线程）也不在 sysmon**：M 只跑 Fiber，poller 只做
  readiness 分发与 `expire_timers()`。

### 1.3 等待的固定顺序（`docs/design.md` 的硬规则）

```text
① 先注册 readiness（IOManager 侧登记 WaitNode + 后端 add_or_modify）
② 再发布 deadline（TimerService，绑定到同一个 WaitNode）
③ 再 park Fiber（Scheduler::park_io）
唤醒来源（readiness / timeout / cancel / close）通过**一个原子 outcome claim** 决出唯一赢家，
之后以相同的绝对 deadline 重试 syscall（EAGAIN 才回到 ①）。
```

### 1.4 fd 代际（防 close/reuse ABA）

- `DescriptorToken{fd, generation, atomic<bool> m_valid, weak_ptr<DescriptorRegistryState>}`；
  `DescriptorTokenPtr = shared_ptr<const DescriptorToken>`。
- `DescriptorGuard`（RAII，持 `recursive_mutex`）：
  `Capture(fd)` 复用有效 token，否则 `++m_next_generation` 新建；
  `Invalidate(fd)` 置 `token->m_valid=false` 并检出表。
- **绝不能跨 park 或阻塞系统调用持有 `DescriptorGuard`**（头文件明确要求）；
  Hook 在失效标记与 close/dup2 期间持它，IOManager 在验证 token 并发布 interest 期间持它。
- 数字 fd 被复用后，旧 token 的 `valid()` 为 false → 旧等待不会再被误唤醒。
- `notify_close(fd)` / `NotifyCloseAll(fd)`：**Hook 必须在真实 `close` 之前调用**；
  当前等待者收到 `kClosed`/`EBADF`，旧 epoll 载荷同时失效。

### 1.5 `wait` / `wait_any` / `wait_many` 的边界（★ 容易误读）

| 接口 | 谁可以调用 | 语义 |
|---|---|---|
| `wait` / `wait_for` | 头文件 `@details` 明确「普通线程也可通过 wait 接口安全使用」 | 单 fd 就绪；`ready/timeout/cancelled/closed/error` |
| `wait_any(_for)` | **仅当前 IOManager 的 managed Fiber**；普通线程返回 `kError`/`EPERM` | 返回**第一个获胜项**（含 index/fd/event） |
| `wait_many(_for)` | 同上 | 返回**本轮已完成的所有队头项** `ready_indices`（按注册顺序） |

- `wait_many` **不是屏障**（头文件原话：「它不是『等待所有请求都完成』的屏障；需要屏障时应使用多个任务或 `WaitGroup`」）。
- **同一集合内不得重复提交相同 fd/方向**（重复返回 `EINVAL`）；补采样只完成该 fd/方向等待队列的**队头节点**，以保持多个 Fiber 之间的 FIFO。
- 注册过程中遇到无效 fd 会取消已注册节点并返回整体错误。
- 批量唤醒用 `Scheduler::wake_io_batch`（唯一调用点是 `State::wake_nodes` 的批量分支），
  它**跳过 `owned_by` 校验**，只接受本 Scheduler 已注册的 G（见 G2 的坑点 3）。

### 1.6 `State` 内部机制（供架构图标注）

- `make_registration_token(fd, sequence)`：把 fd 与注册序号编进 epoll token；fd 仍在 mutex 下重查（防悬空）。
- `m_slots`：`FdSlot{readers, writers, descriptor, generation}`（每 fd 的读/写等待表）。
- `m_deadline_index` / `m_deadline_order`：deadline 索引与顺序（`expire_timers` 使用）。
- `process_events_batch`：批量处理 epoll 事件；`wake_nodes(WakeList)`：批量唤醒。
- `tickle()`：`m_tickle_pending` 合并 eventfd 写（避免每个唤醒一次系统调用）。
- `State::s_node_cache`：`inline static thread_local std::vector<pair<State*, NodePtr>>`（上限 4096）
  用于 WaitNode 复用；以裸 `State*` 为键，`~State()` **不清理**自己的条目
  （台账 X-15：有界滞留 + 地址复用命中，取出即 `reset()`，实测无害）。

---

## 2. `IOBackend`（可替换多路复用协议）

```cpp
enum class BackendEventFlag : std::uint32_t { kReadable = 0x01, kWritable = 0x02, kError = 0x04, kHangup = 0x08 };
struct BackendEvent final { std::uint64_t token{0}; std::uint32_t events{0}; };   // token=0 保留给唤醒

class IOBackend {
  virtual bool initialize(int* error = nullptr) noexcept = 0;
  virtual bool supports_zero_interest() const noexcept { return false; }
  virtual bool supports_one_shot() const noexcept { return false; }
  virtual int  add_or_modify(int fd, std::uint32_t interest, std::uint64_t token, bool modify) noexcept = 0;
  virtual int  remove(int fd) noexcept = 0;
  virtual int  wait(BackendEvent* output, std::size_t capacity, int timeout_ms) noexcept = 0;  // 超时 0，失败负 errno
  virtual void wake() noexcept = 0;        virtual void drain_wake() noexcept = 0;   // drain 只由 poller 线程调
  virtual void shutdown() noexcept = 0;
};
bool RegisterBackend(std::string name, IOBackendFactory);   // 同名注册失败
bool UnregisterBackend(const std::string& name) noexcept;   // ★ 平台默认后端不可注销
IOBackendPtr CreateBackend(const std::string& name = {});   // 空名称 = 默认后端
std::string DefaultBackendName();                            // Linux：epoll
```

**契约**：后端只处理 readiness 注册/等待/跨线程唤醒/关闭，**不接触 Fiber、Task、Context 或超时状态**；
一次等待的最终结果仍由 IOManager 的 WaitNode 原子 claim 决定。一个后端实例只能绑定一个 IOManager。

`EpollBackend` 实现要点（`io_backend.cpp`）：

- `epoll_create1(EPOLL_CLOEXEC)` + `eventfd`（token=0 是唤醒事件）；
- `supports_zero_interest() = true`：interest=0 时用 `EPOLL_CTL_MOD` 停用而不是 DEL/ADD；
- `supports_one_shot() = true`：用 `EPOLLONESHOT`，被消费后由 IOManager 重新 arm；
- ADD/MOD 遇 `EEXIST`/`ENOENT` 时**互换重试**（容忍注册状态漂移）；
- 释放 fd 用 `syscall(SYS_close)` **绕开自身 hook**（否则 hook 会再拦截一次）。

---

## 3. `hook`（Linux syscall 拦截层）

### 3.1 层与开关

```cpp
namespace go2cpp::hook {
  void set_enabled(bool) noexcept;   bool enabled() noexcept;
  void bind_io_manager(IOManager*) noexcept;  IOManager* bound_io_manager() noexcept;
  void set_connect_timeout(std::chrono::milliseconds) noexcept;
  class ScopedEnable;                 // 线程作用域临时启用
  class ScopedIOManagerBinding;       // 线程作用域临时绑定 IOManager
  using ThreadHookMode = ::go2cpp::ThreadHookMode;
  using ScopedThreadHookMode = ::go2cpp::ScopedThreadHookMode;
}
extern "C" {                          // 刻意用 C ABI，便于 preload/interposer 宿主使用
  void go2cpp_hook_set_enabled(int) noexcept;
  int  go2cpp_hook_is_enabled() noexcept;
  void go2cpp_hook_bind_io_manager(void*) noexcept;
}
```

Hook 默认在**进程范围**启用；`GO2CPP_BUILD_HOOK=OFF` 或静态构建时必须关闭
（否则进程内会有两份 FD/TLS 注册表）。

### 3.2 拦截清单（31 个原函数指针，`dlsym(RTLD_NEXT)` 填充 `Originals`）

| 类别 | 函数 |
|---|---|
| 睡眠 | `sleep`、`usleep`、`nanosleep`（→ `cooperative_sleep_for`） |
| socket 生命周期 | `socket`、`socketpair`、`connect`、`accept`、`accept4`、`close`、`dup`、`dup2`、`dup3` |
| 读 | `read`、`readv`、`recv`、`recvfrom`、`recvmsg` |
| 写 | `write`、`writev`、`send`、`sendto`、`sendmsg` |
| 控制 | `fcntl`、`ioctl`、`getsockopt`、`setsockopt` |
| **只包 `BlockingRegion`、不 park** | `poll`、`ppoll`、`select`、`pselect`、`epoll_wait`、`epoll_pwait` |

`MSG_WAITALL`、`MSG_OOB`、未知的可变参数 `fcntl`/`ioctl` 命令 → **`ENOTSUP`**
（不读未知类型参数、不转发无效 ABI 状态）。

### 3.3 统一入口 `cooperative_io<Result>(fd, fn, event, per_call_nonblocking, args...)`

```text
① descriptor_for(fd)：命中已跟踪元数据 → 直接尝试
   未跟踪且属 managed hook 场景 → try_adopt_socket → adopt_socket
     （真 fcntl(F_SETFL, O_NONBLOCK)，区分 m_system_nonblocking / m_user_nonblocking）
② 在 DescriptorGuard 门内执行真实 syscall
③ 若 EAGAIN/EWOULDBLOCK：
     can_wait() = m_socket && m_system_nonblocking && !m_user_nonblocking
       真 → wait_for_io(manager, …)  park 当前 Fiber（IOManager 的 epoll）
       假/无 IOManager → native_wait：有界原生 poll 回退（10 ms 切片）
```

- 「系统非阻塞」与「用户非阻塞」分开记账：用户显式设过 `O_NONBLOCK` 时**不能**替它 park
  （否则会改变用户可见语义）。
- `cooperative_connect`：处理 `EINPROGRESS`/`EALREADY`，使用 `s_connect_timeout_ms`（`set_connect_timeout`）。
- `accept`/`accept4` 成功后 `try_register_new_socket` 把新 fd 纳入跟踪。
- `adopt_socket`：持全局 `s_descriptors_mutex` 做首次接管（含 3~4 次真实系统调用），
  热路径（已接管 fd）命中 map 即返回，不受影响（台账 X-18：新 fd 密集场景下这是有意串行点）。

### 3.4 坑点

1. `DescriptorGuard` **禁止跨 park/阻塞系统调用持有**（会与其他接管者/close 互锁）。
2. `poll/select/epoll_wait` **不会 park Fiber**，只标 `BlockingRegion` —— 在受管 G 里直接调用它们
   会阻塞整个 M（应由 `IOManager::wait_any/wait_many` 代替）。
3. Hook 关闭后**不再接纳**新的 Fiber/epoll 等待；socket 上已有元数据在 close 前仍可能走原生后备。
4. 已接管 socket 的内核非阻塞标志会一直保留（`m_system_nonblocking`），
   因此即使 Hook 关闭，后续 `read` 也可能返回 `EAGAIN` —— 需要调用方处理。
5. `socket`/`connect` 等被拦截后，`Originals` 里的原函数是 `dlsym(RTLD_NEXT)` 得到的；
   与 `LD_PRELOAD` 顺序、`-Bsymbolic` 构建相关（已登记为未验证项）。
6. C ABI 入口（`go2cpp_hook_*`）的 `manager` 必须是调用方拥有且仍存活的 `IOManager`。

---

## 4. `fd_manager`（fd 元数据表）

```cpp
class FdContext final {                      // ★ getter 无任何同步保护（头文件明确警告）
  int fd() const;  bool initialized() const;  bool closed() const;
  bool user_nonblock() const;  bool system_nonblock() const;
  std::chrono::milliseconds recv_timeout() const;  send_timeout() const;
};
class FdManager final {
  static FdManager& instance() noexcept;
  ContextPtr get(int fd, bool auto_create = true);
  void erase(int fd) noexcept;                  // 标记旧对象 closed
  ContextPtr duplicate(int old_fd, int new_fd); // 复制用户状态到新 fd
  bool set_nonblock(int fd, bool user, bool system);
  bool set_timeout(int fd, bool receive, std::chrono::milliseconds timeout);
};
```

- 该表**不拥有 fd**，只保存 hook / `Socket` / IOManager 共享的用户/系统非阻塞状态与超时；
  `close`/`dup2` 时必须显式 `erase`/`duplicate`。
- **读取侧无锁直读**是已知的数据竞争（头文件 `@warning` 明写）；调用方应只在确认无并发写入时读，
  或在更高层自行同步。

---

## 5. 与 Go 的对应与差异

| Go | 本项目 | 差异 |
|---|---|---|
| runtime netpoller（epoll 集成在 runtime 里） | `IOManager` + 独立 poller 线程 + `IOBackend` 抽象 | 后端可替换（kqueue/IOCP/io_uring 只实现协议）；poller 不跑用户代码 |
| 阻塞式 `net.Conn` 语义 | 暴露 readiness + `WaitStatus`/errno | 调用方要自己重试 syscall（EAGAIN 循环） |
| goroutine 自动感知阻塞 syscall | 需 Hook 或显式 `BlockingRegion` | 未 Hook 的阻塞调用**看不到**（会占住 M） |
| `netpoller` 内部 ABI | `DescriptorToken`/generation 显式防 ABA | 用户可见的 fd 代际概念 |
| 无 | `wait_any` / `wait_many` | Go 用 `select`+`SetDeadline`；这里是一次收集多个 fd 的 API |

---

## 6. 对架构图的贡献

**节点**

- `IOManager`（内含 `Scheduler` + `State` + **独立 poller 线程**）
- `DescriptorRegistry`（`DescriptorToken` generation + `DescriptorGuard` 短门）
- `IOBackend`（协议）→ `EpollBackend`（`epoll_create1` + `eventfd`，ONESHOT，zero-interest）
- `Hook`（`Originals` 31 个 + `cooperative_io` + `adopt_socket` + `native_wait` 回退）
- `FdManager`（全局 fd 元数据表；读侧无锁）

**连线**

- `Hook → IOManager`：「EAGAIN → wait_for_io → park Fiber（先注册 readiness，再发布 deadline）」
- `Hook → FdManager`：「用户/系统非阻塞、收发超时元数据」
- `Hook → DescriptorGuard`：「真实 close 前 NotifyClose/Invalidate（防 ABA）」
- `IOManager → Scheduler`：「park_io / wake_io / wake_io_batch」
- `IOManager → TimerService`：「deadline 到期 claim outcome」
- `IOManager → IOBackend`：「add_or_modify / wait / wake / drain_wake」
- `EpollBackend → 内核`：「epoll_wait + eventfd（token=0 保留）」

**图内文字**

> IO 路径：Hook 拦截 libc → `cooperative_io` 先试真调用 → `EAGAIN` 时把 fd 注册到 epoll
> （**先 readiness、再 deadline、最后 park**，三者通过一个原子 outcome claim 决出唯一赢家）→
> Fiber 挂起让出 M。epoll 循环跑在 **IOManager 自己的 poller 线程**，不在 M、也不在 sysmon。
> fd 用**代际 token** 防 close/reuse ABA：Hook 必须在真实 close 前 `NotifyClose`。
> 没有 IOManager 的受管 Fiber 退化为「有界原生 poll 切片」后备；`poll/select/epoll_wait` 本身不被 park。
