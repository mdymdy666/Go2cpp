# 跨平台（Windows）严格测试流程

> 用户明确要求：**不是「编译通过就行」**。
> 平台之间的**系统函数契约不同** → 同生态位的功能**行为不同**。
> 所以顺序是：**① 系统调用差异调查（最重要）→ ② 开发 + 三件套 → ③ 重复验收 + 压测**。

---

## 一、系统调用差异调查（第一步，最重要）

**目标**：把项目用到的**每一个**系统调用，逐个列出 Linux 与 Windows 的**契约差异**（不是名字差异，是**语义差异**）。

### 1.1 调查表模板（每个调用一行）

| 项 | 内容 |
|---|---|
| 调用 | 例：`epoll_wait` |
| Linux 契约 | 语义 / 返回值 / 错误码 / 边界 |
| Windows 对应 | 例：`GetQueuedCompletionStatus`（IOCP） |
| **契约差异** | 语义不同点（阻塞/超时/错误/顺序/所有权） |
| **对本项目的影响** | 哪些代码路径会行为不同 |
| 结论 | 需适配 / 无需适配（并写理由） |

### 1.2 必查清单（按模块）

#### 网络（最关键）
| Linux | Windows | 已知差异 |
|---|---|---|
| `epoll_create1/epoll_ctl/epoll_wait` | `CreateIoCompletionPort/GetQueuedCompletionStatus` | **IOCP 是完成端口，不是就绪通知**——语义根本不同（需要重写事件循环） |
| `poll/select` | `WSAPoll/select` | `WSAPoll` 有已知 bug（连接失败不报）；`select` 的 `nfds` 忽略 |
| `socket()` | `socket()` | 返回 `SOCKET`（`UINT_PTR`）不是 `int`；`INVALID_SOCKET` vs `-1` |
| `close(fd)` | `closesocket(s)` | Windows 必须 `closesocket`，`close` 对 socket 无效 |
| `errno` | `WSAGetLastError()` | **错误码空间完全不同** |
| `SO_REUSEADDR` | `SO_REUSEADDR` | **语义不同**：Windows 允许绑定已用端口（危险），Linux 只允许 TIME_WAIT |
| `SO_REUSEPORT` | ❌ 无 | 需模拟（多进程监听同端口） |
| `TCP_NODELAY` | `TCP_NODELAY` | 一致 |
| `shutdown(SHUT_WR)` | `shutdown(SD_SEND)` | 常量不同 |
| `recv` 返回 0 | `recv` 返回 0 | 一致（对端关闭） |
| `EPOLLET`（边缘触发） | ❌ 无 | 需用 IOCP 或 `WSAPoll` + 手动状态机 |
| `sendfile/splice` | `TransmitFile` | 语义不同 |

#### 线程 / 同步
| Linux | Windows | 已知差异 |
|---|---|---|
| `pthread_create` | `CreateThread` | 返回 `HANDLE` 需 `CloseHandle` |
| `pthread_mutex` | `SRWLOCK`/`CRITICAL_SECTION` | **`CRITICAL_SECTION` 不可跨进程**；`SRWLOCK` 不可重入 |
| `futex` | `WaitOnAddress`（Win8+） | API 形态不同但语义接近 |
| `pthread_cond_wait` | `CONDITION_VARIABLE` | **`SleepConditionVariableSRW` 语义接近**，但**无虚假唤醒保证差异** |
| `pthread_key` | `TlsAlloc` | 一致 |
| `sched_setaffinity` | `SetThreadAffinityMask` | 一致 |
| `pthread_cancel` | ❌ 无（`TerminateThread` 危险） | 需用协作式取消 |

#### 进程
| Linux | Windows | 已知差异 |
|---|---|---|
| `fork` | ❌ 无（`CreateProcess`） | **COW 语义无法模拟**——所有 `fork` 用法都要重写 |
| `execve` | `CreateProcess` | 参数传递方式不同（数组 vs 命令行字符串） |
| `waitpid` | `WaitForSingleObject` | 退出码/信号语义不同 |
| `kill(pid, SIGTERM)` | `TerminateProcess` | **无法优雅关闭**（需 `GenerateConsoleCtrlEvent` 或自定义 IPC） |
| `getpid` | `GetCurrentProcessId` | 一致 |

#### 内存
| Linux | Windows | 已知差异 |
|---|---|---|
| `mmap/munmap` | `VirtualAlloc/VirtualFree` | 一致（匿名映射） |
| `mprotect` | `VirtualProtect` | 一致 |
| `madvise(MADV_DONTNEED)` | `VirtualUncommit` | 语义接近 |
| `mmap(MAP_SHARED)` | `CreateFileMapping` | 需命名对象 |
| `mlock` | `VirtualLock` | 一致 |

#### 时间
| Linux | Windows | 已知差异 |
|---|---|---|
| `clock_gettime(CLOCK_MONOTONIC)` | `QueryPerformanceCounter` | 一致（高精度） |
| `nanosleep` | `Sleep`/`SleepConditionVariable` | `Sleep` 精度 ~15.6ms（需 `timeBeginPeriod`） |
| `timerfd` | `CreateWaitableTimer` | 一致 |
| `gettimeofday` | `GetSystemTimeAsFileTime` | 一致 |

#### 文件
| Linux | Windows | 已知差异 |
|---|---|---|
| `open/read/write` | `CreateFile/ReadFile/WriteFile` | 句柄是 `HANDLE`；**文本模式自动 CRLF 转换**（需 `_O_BINARY`） |
| `fsync` | `FlushFileBuffers` | 一致 |
| `ftruncate` | `SetEndOfFile` | 一致 |
| `rename`（原子替换） | `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` | **Windows 上 rename 不能覆盖已存在文件**（除非用 `MoveFileEx`） |
| `unlink`（打开时删除） | ❌ 默认不可（需 `FILE_SHARE_DELETE`） | **Windows 打开的文件不能删/改名**——影响日志轮转、临时文件 |
| `opendir/readdir` | `FindFirstFile/FindNextFile` | 一致 |
| 路径分隔符 | `\` vs `/` | 需统一（项目内用 `/`，Windows API 接受 `/` 但某些场景不接受） |
| 文件名大小写 | 敏感 vs **不敏感** | Windows 上 `a.txt` 与 `A.txt` 同名 |
| 路径长度 | 无限制（相对） | **260 字符（MAX_PATH）**，需 `\\?\` 前缀 |

#### 信号
| Linux | Windows | 已知差异 |
|---|---|---|
| `SIGTERM/SIGINT` | `SetConsoleCtrlHandler` | **只有 Ctrl+C/Break/Close/Logoff/Shutdown**，无任意信号 |
| `SIGSEGV` 等 | 结构化异常（SEH） | 处理机制完全不同 |
| `raise` | `raise` | 一致（但只有少数信号） |

#### 杂项
| Linux | Windows | 已知差异 |
|---|---|---|
| `dlopen/dlsym` | `LoadLibrary/GetProcAddress` | 一致 |
| `dlerror` | `GetLastError` | 一致 |
| `/proc/*` | ❌ 无（需 `NtQuerySystemInformation` 或 WMI） | **本项目大量用 `/proc`**（`/proc/stat`、`/proc/self/status`、`/proc/<pid>/fd`）——**必须全部适配** |
| `sysconf(_SC_NPROCESSORS_ONLN)` | `GetSystemInfo` | 一致 |
| `getenv/setenv` | `GetEnvironmentVariable/SetEnvironmentVariable` | 一致 |
| `umask/permissions` | ACL | 完全不同 |

### 1.3 本项目调查结果

> 扫描口径：`grep -rn` 覆盖 `src/` + `include/`（276851 行 C/C++ 源码）。
> 共梳理 **48 类**系统调用/内核接口 → **40 类需适配、8 类无需适配**，全部给出 `file:line`。
> 结论只允许两种：**需适配（怎么做）** / **无需适配（为什么）**，无「待定」。

#### 1.3.0 先看两个硬阻断（不是"差异"，是"编译不过/启动不了"）

| # | 事实 | 位置 |
|---|---|---|
| B-1 | Windows 上 `hook.cpp` 直接 `#error`：透明 IO 拦截层根本不存在 | `src/network/hook.cpp:64` |
| B-2 | 非 Linux 平台 `DefaultBackendName()` 返回空串 → `CreateBackend()` 拿不到后端 → `IOManager` 初始化失败 | `src/network/io_backend.cpp:218-222`、`src/network/io.cpp:697` |

这两条决定了 Windows 适配**不是"打补丁"**，而是先补两个"平台层缺口"。

#### 1.3.1 网络（14 类）

| 调用 | Linux 契约(语义/返回/错误码/边界) | Windows 对应 | **契约差异** | 对本项目影响(file:line) | 结论 |
|---|---|---|---|---|---|
| `epoll_create1/epoll_ctl/epoll_wait` | **就绪通知**：注册 fd+兴趣位，`wait` 返回「哪些 fd 现在可读/可写」；水平或边缘触发；`EPOLLONESHOT` 可一次性布防；`epoll_wait` 返回事件个数，`-1`+`errno`（`EINTR` 可重试），`timeout` 毫秒（-1 无限） | `CreateIoCompletionPort` + `GetQueuedCompletionStatus(Ex)` | **完成 vs 就绪**：IOCP 没有"fd 可读"这个概念，只有"我投递的那次 `WSARecv/WSASend` 完成了"；每个操作要自带缓冲区与 `OVERLAPPED`；没有兴趣位掩码、没有 `EPOLLONESHOT`（要靠每次重新投递模拟）；`dwMilliseconds` 也是毫秒，但**超时精度受系统定时器影响** | `src/network/io_backend.cpp:48`（create）、`:53/:93/:98`（ctl）、`:105`（DEL）、`:117`（wait 主循环）；`src/network/io.cpp:2150`（IOManager 唯一等待点）、`:697`（初始化） | **需适配**。`IOBackend` 接口（`add_or_modify(interest)` + `wait(timeout)`）是**就绪形状**，IOCP 无法直接实现。两条路线：① **短期**加一个 `WSAPoll`/`select` 后端保住 reactor，上层零改动（代价：单实例上限、WSAPoll 已知缺陷要兜底）；② **长期**给 socket 路径新增 completion 后端（先投递 `WSARecv/WSASend`，完成包唤醒 fiber），把 `interest` 降级为"仅 reactor 后端支持"，用能力位（类似已有 `supports_one_shot()`）区分 |
| `eventfd` | 内核计数器：`write` 8 字节**累加**、`read` 清零；可被 `poll/epoll` 等待；`EFD_NONBLOCK` 满则 `EAGAIN` | 无直接对应 | **不是可等待句柄**：IOCP 完成包不是 fd；`WaitForSingleObject` 能等 Event 但不能注册进 `WSAPoll`；语义是"计数合并"而非"事件" | `src/network/io_backend.cpp:50`（创建）、`:129-137`（`wake()/drain_wake()` 用 `SYS_write` 直写）；`src/bento/net/link.cpp:936`、`:1013`、`:1183`；`src/rd_net/event_loop.cpp:94`；`src/os/supervisor.cpp:284` | **需适配**。抽 `WakeEvent`：reactor 后端用 loopback socketpair（Windows 下 `WSAPoll` 可等待）；completion 后端用 `PostQueuedCompletionStatus` 伪造完成包，`drain_wake` 变成"弹出并丢弃" |
| `poll/ppoll` | 就绪列表；`timeout` 毫秒；`EINTR` 重试；可等待任意 fd（管道/eventfd/终端） | `WSAPoll`（Vista+） | `WSAPoll` **只支持 socket**，不支持管道/eventfd；**已知缺陷**：连接失败时不一定报 `POLLOUT/POLLERR`、被删除的 socket 不通知；`WSAPOLLFD` 字段名与 `pollfd` 不同 | `src/tunnel/net.cpp:158`、`:182`（connect/读写等待）；`src/network/http2/tls_transport.cpp:407`（TLS 握手等待） | **需适配**。包装 `PollSockets()`：Windows 分支对每个 `POLLOUT` 的 connect **必须**补 `getsockopt(SO_ERROR)` 判定；非 socket 句柄在 Windows 上不得走 poll |
| `select` | 第一个参数 `nfds` 被忽略；`fd_set` 位图，`FD_SETSIZE=1024`；`EINTR` | `select`（仅 socket） | `fd_set` 是 **SOCKET 数组**不是位图，默认 `FD_SETSIZE=64`；句柄类型 `SOCKET`；只支持 socket | `src/network/hook.cpp:2287`（hook 拦截，Linux only）；`src/rd_net/socket.cpp:1927`（connect 完成判定） | **需适配**。connect 完成判定改 `getsockopt(SO_ERROR)`（不依赖 `exceptfds`）；内部禁止用 `select` 等待超过 64 个 socket |
| `socket()` / `close()` | 返回 `int fd`，失败 `-1`+`errno`；与普通文件同一 fd 空间；`close()` 通用 | `socket()` 返回 `SOCKET`（`UINT_PTR`）；失败 `INVALID_SOCKET`；必须 `closesocket()` | 句柄宽度/无效值/关闭函数全不同；**socket 不能交给 `close()`，也不能和 CRT fd 混用** | `include/go2cpp/network/socket.hpp:371`（`int m_fd{-1}`）；`src/network/socket.cpp:689`（`::close(fd)`）；`src/tunnel/net.cpp:204`、`:521` | **需适配**。`Socket` 内部句柄改 `std::intptr_t`，关闭统一走 `CloseSocket()`；`os::File` 与 socket 句柄类型分离 |
| `SO_REUSEADDR` | **只**允许复用 `TIME_WAIT`；与已 `LISTEN` 的同一 `addr:port` **不能**共存（要 `SO_REUSEPORT` 才行） | `SO_REUSEADDR` | **语义相反**：Windows 文档明示"允许绑定到已被使用的地址"，可**静默抢占**正在监听的端口 | 21 处引用（11 处直接 `setsockopt`，其余经 `Socket::set_option`），关键：`src/network/socket.cpp:1352`、`src/rd_net/socket.cpp:337`、`:1507`、`src/bento/net/link.cpp:312`、`src/tunnel/net.cpp:529`、`src/rpc/transport.cpp:358`、`src/debug/debug_server.cpp:1569`、`src/p2p/transport.cpp:76` | **需适配**。Windows 上监听 socket **额外设 `SO_EXCLUSIVEADDRUSE`**，并在 `Listen` 前显式探测端口占用；否则两个实例可同时监听同一端口，出现"幽灵连接"（测试/线上都会静默错） |
| `SO_REUSEPORT` | 多 socket 绑同一端口，**内核按流哈希分发** | **无** | 无内核分发；只能应用层自己派发 | `src/bento/net/link.cpp:316`、`src/bento/net/udp_batch.cpp:202`（多队列 UDP） | **需适配**。退化为 `SO_REUSEADDR` + 用户态分发（单接收线程按 flow hash 派发），文档标注吞吐/延迟与 Linux 不可比 |
| `TCP_NODELAY` | 关闭 Nagle | `TCP_NODELAY` | 无差异（常量名与值一致） | `src/tunnel/net.cpp:121`、`src/distributed/transport.cpp:832` | **无需适配**：`setsockopt` 参数完全一致，Windows 同样支持 |
| `shutdown(SHUT_*)` | `SHUT_RD/WR/RDWR` = 0/1/2 | `SD_RECEIVE/SD_SEND/SD_BOTH` = 0/1/2 | 名字不同，**值恰好相同** | `src/tunnel/net.cpp:484`、`:600` | **需适配**（一行宏映射即可；不要假设名字可移植） |
| `accept4(SOCK_NONBLOCK\|SOCK_CLOEXEC)` | **原子**地 accept + 设非阻塞 + CLOEXEC | `accept()`（无 flags） | 不原子：只能 accept 后再 `ioctlsocket(FIONBIO)`，中间有窗口；句柄不可继承由 `CreateProcess` 继承表控制 | `src/network/socket.cpp:509`、`src/tunnel/net.cpp:580` | **需适配**。封装 `AcceptSocket()`：accept → 立刻设非阻塞；文档标注"accept 与设非阻塞之间可能被调度一次" |
| `sendfile` / `splice` | 零拷贝；`sendfile` 仅"文件→socket"；`splice` 走管道、可 socket↔socket | `TransmitFile`（文件→socket，支持重叠 IO）；**无** socket↔socket 零拷贝 | 只覆盖一半；管道模型不存在 | `src/os/file.cpp:947`（`SYS_sendfile`）；`src/relay/splice_pipe.cpp:233`（`pipe2`）、`:248`、`:277`（`::splice`） | **需适配**。文件→socket 用 `TransmitFile`；relay 的 splice 在 Windows 退回用户态拷贝（用内存带宽换 CPU，需重测吞吐） |
| `MSG_NOSIGNAL` | `send` 不产生 `SIGPIPE` | 无此 flag（Windows 无 `SIGPIPE`） | 传未知 flag 会 `WSAEINVAL` | 22 行 send 调用带该 flag（`src/network/socket.cpp` 发送路径等） | **需适配**。定义 `kNoSignalFlag`：Linux=`MSG_NOSIGNAL`，Windows=`0` |
| `errno` | 线程局部 `int`；`EAGAIN==EWOULDBLOCK`；`EINTR` 普遍 | `WSAGetLastError()`（WSA*）与 `GetLastError()`（系统）**两套错误域** | 错误码空间完全不同；Windows 无 `EINTR`（"被信号打断"不存在），重试循环退化为直通 | 807 行涉及 `errno`（`src/` + `include/`）；`EINTR`/`EAGAIN` 遍布重试循环 | **需适配**。新增 `platform::LastError()` 统一映射到项目错误类型（沿用 `os::ProcError` 模式）；socket 路径单独做 WSA→errno 映射表；重试循环保留但注明 Windows 分支永不命中 `EINTR` |
| `LD_PRELOAD` + `dlsym(RTLD_NEXT)`（透明 IO 拦截） | 接管 libc 的 `read/write/recv/send/poll/nanosleep/select`，把"阻塞调用"变成"fiber park + epoll 唤醒"，业务代码 `std::socket` 零改动就获得协程化 | **无 `LD_PRELOAD`**；只能 IAT/inline hook（Detours 类）或改调用方 | **架构级差异**：Windows 没有"进程级符号拦截"；IAT hook 只能覆盖静态导入、可被安全软件拦截、对 `GetProcAddress` 动态解析无效 | `src/network/hook.cpp:64`（`#error`）、`:267`（`dlsym(RTLD_NEXT)`）、`:2144`（`nanosleep` hook）、`:2287`（`select` hook）、`:3001`（`epoll_wait` 直通） | **需适配（最重）**。Windows 上必须**放弃"透明 hook"**：业务侧只走 `go2cpp::network` 自有 API，由该层显式 park；或在支持场景用 IAT hook（仅静态链接、可关闭）。同时要重新定义"哪些阻塞调用会被 fiber 化"的白名单——这是能力契约变更，不是实现细节 |

#### 1.3.2 线程 / 内存 / 时间（9 类）

| 调用 | Linux 契约(语义/返回/错误码/边界) | Windows 对应 | **契约差异** | 对本项目影响(file:line) | 结论 |
|---|---|---|---|---|---|
| `futex(FUTEX_WAIT_PRIVATE/WAKE_PRIVATE)` | 原子比较 `*word==expected` 后睡；不相等立即 `EAGAIN`；超时是相对时间；`WAKE` 返回实际唤醒数；`PRIVATE` 不走跨进程哈希表 | `WaitOnAddress` / `WakeByAddressSingle`（Win8+） | 无 `PRIVATE` 变体；无 `FUTEX_REQUEUE`（本项目 `Channel` 的唤醒链若用到需重写）；超时是**相对毫秒**且精度受系统定时器（默认 15.6ms）影响；无"返回值=唤醒数" | `src/concurrency/sync.cpp:91`、`:101`（`WaitFutex/WakeFutex` 定义）、`:670`（等待点）；`src/concurrency/parking_condition.cpp:170`（`CLOCK_MONOTONIC`+等待） | **需适配**。封装 `platform/futex.hpp`：Windows 用 `WaitOnAddress`；超时统一走高精度定时器（`timeBeginPeriod(1)`）。`sync.cpp` 已有 `#if defined(__linux__) … #else` 回退（`:824` 起），**只能保证正确性、不保证性能**——必须重跑 park/unpark 基准 |
| `pthread_create` / `std::thread` | 返回 pthread_t；join/detach | `CreateThread`，返回 `HANDLE` 需 `CloseHandle`；C++ 的 `std::thread` 已封装 | 本项目用 `std::thread`，接口层无差异；**资源层**：默认栈 1MB、TLS 槽位数量、线程数上限不同 | `src/concurrency/scheduler.cpp:2313`（worker 线程）、`:3343`（sysmon 线程） | **无需适配**：`std::thread` 已跨平台；但 Windows 线程栈/TLS 上限需压测（万线程场景） |
| `pthread_setaffinity_np` | `cpu_set_t` 任意核绑定 | `SetThreadAffinityMask`（≤64 核）；`SetThreadGroupAffinity`（>64 核，需分组） | 掩码宽度 64 位；超过 64 逻辑核必须用 processor group | `src/concurrency/scheduler.cpp:5684`（`pin_workers_to_cpu`） | **需适配**（可选优化）。Windows 用 `SetThreadGroupAffinity`；跨 group 的 P 需重新分配。绑核失败按现有语义"继续运行" |
| `sigaction(SIGSEGV)` + `sigaltstack` | 栈溢出时内核切到 altstack 执行 handler，可扩栈续跑 | SEH；`AddVectoredExceptionHandler` 可捕获 `EXCEPTION_STACK_OVERFLOW` | 异常分发模型不同；Vectored handler 运行时栈已耗尽，扩栈要非常小心；`sigaltstack` 无对应 | `src/concurrency/fiber.cpp:409`、`:495`（growable stack 的 SIGSEGV 处理器） | **需适配**。Windows 用 `AddVectoredExceptionHandler` 处理 `EXCEPTION_STACK_OVERFLOW`；做不到就在 Windows 关闭 growable stack 并文档化（不能静默退化） |
| `SIGPIPE` | 写已关闭 socket 默认杀进程，必须忽略 | **无此信号** | 写已关闭 socket 返回 `WSAECONNRESET`，不会杀进程 | `src/tunnel/client_main.cpp:251`、`server_main.cpp:221`、`src/network/http2/tls_transport.cpp:165`、`src/media/audio.cpp:332`、`src/os/proc.cpp:2355` | **无需适配**：Windows 无 `SIGPIPE`，`signal(SIGPIPE, SIG_IGN)` 放进 `#ifndef _WIN32` 即可，语义反而更简单 |
| `mmap/mprotect`（guard page + 按需提交） | `PROT_NONE` 保留 + `mprotect` 提交；页 4KB；`MAP_NORESERVE` | `VirtualAlloc(MEM_RESERVE)` + `VirtualAlloc(MEM_COMMIT)` + `VirtualProtect(PAGE_NOACCESS)` | **提交粒度 64KB**（不是 4KB）；reserve/commit 是两段式，没有 `mprotect` 的"部分提交"；`MEM_RELEASE` 必须整段释放 | `src/concurrency/fiber.cpp:459`（altstack）、`:728`（slab）、`:1185`（reserve）、`:1191`/`:1205`/`:1232`/`:1375`（提交/扩栈） | **需适配**。抽 `StackMapping`（reserve/commit/protect/release 四操作），按 64KB 对齐提交；fiber 栈大小需向上取整（`fiber_stack_size` 归一化逻辑要改） |
| `sysconf(_SC_PAGESIZE/_SC_OPEN_MAX)` | 页大小 4096；fd 上限 | `GetSystemInfo(dwPageSize)`；无 `OPEN_MAX` 概念 | 页大小通常 4096 但**不保证**；无"文件描述符上限" | `src/concurrency/fiber.cpp:356`、`:3315`（页大小）；`src/os/proc.cpp:1090`（`_SC_OPEN_MAX`，用于子进程关 fd） | **需适配**。页大小走 `GetSystemInfo`；`OPEN_MAX` 在 Windows 上改为固定保守值（如 2048）并注释 |
| `clock_gettime(CLOCK_MONOTONIC)` | 纳秒、单调、vDSO 免陷入 | `QueryPerformanceCounter`（频率由 `QueryPerformanceFrequency` 给出，通常 10MHz） | 单位不是纳秒，要做**乘除换算**（注意 64 位溢出）；`CLOCK_REALTIME` 对应 `GetSystemTimePreciseAsFileTime` | `src/concurrency/parking_condition.cpp:170`、`:187`；`src/os/mq.cpp:45`、`:61` | **需适配**。统一 `SteadyNow()`（QPC 换算到 ns）与 `WallNow()`；所有"纳秒绝对值"的等待超时要重新标定 |
| `nanosleep` → `Sleep` | 高精度（hrtimer），`EINTR` 时回写剩余时间 | `Sleep(ms)` / `SleepConditionVariableSRW` | **默认精度 15.6ms**（`timeBeginPeriod` 可到 1ms，Win10 2004 起是 per-process）；毫秒截断；无"剩余时间回写" | `src/network/hook.cpp:2144`（hook 实现）、`:2098`/`:2123`（内部真实 `nanosleep`）；30 处 `std::this_thread::sleep_for`（退避/重试）；定时器由 epoll 超时驱动 `src/concurrency/timer.cpp:283` | **需适配**。启动时 `timeBeginPeriod(1)`（并配对 `timeEndPeriod`）；sub-ms 等待改 `CreateWaitableTimerEx(HIGH_RESOLUTION)` + 自旋补偿；**Windows 上"sleep 被 fiber 化"随 hook 一起消失**（用户代码 `Sleep` 会真阻塞 worker） |

#### 1.3.3 进程（7 类）

| 调用 | Linux 契约(语义/返回/错误码/边界) | Windows 对应 | **契约差异** | 对本项目影响(file:line) | 结论 |
|---|---|---|---|---|---|
| `fork` | COW 复制整个地址空间；子进程继续执行 C++ 代码；失败 `-1`+`errno` | **无**（`CreateProcess` 从零创建） | 无法在子进程里"先改 fd/rlimit/uid/setsid 再 exec"；COW 语义不可模拟 | `src/os/proc.cpp:1100`（`Spawn` 的 `::fork`）、`:1106-1120`（子进程分支）、`ChildRun` 里 `setsid/setrlimit/setuid/execve` | **需适配（重写）**。`CreateProcess` + `STARTUPINFOEX` 句柄继承表；fork 后的设置项在 Windows 无对应 → 显式返回"不支持"（不能静默忽略）；stdin/stdout/stderr 重定向走 `STARTUPINFO` 的 STD 句柄 |
| `execve` | argv **数组**原样传递、无 shell；exec 失败通过 `O_CLOEXEC` 错误管道回传 `errno` | `CreateProcess` 单个命令行**字符串**，需按 MSVCRT 规则转义；失败靠 `GetLastError` | 参数模型不同；引号/反斜杠转义规则易错；无 CLOEXEC 管道回传机制 | `src/os/proc.cpp:815`（`SYS_execve`）；`src/bento/net/command.cpp:11`（承诺"argv 原样、不经 shell"） | **需适配**。实现 Windows 命令行转义（每个参数按 `CommandLineToArgvW` 反向规则加引号）；`CreateProcess` 本身不经 `cmd.exe`，"不经 shell"的承诺可保留；失败用 `GetLastError` 直接映射 |
| `waitpid/wait4` | 任意子进程、`WNOHANG`、退出状态 `WIFEXITED` 编码 | `WaitForSingleObject` + `GetExitCodeProcess` | 无信号退出概念；退出码 32 位直传；不能"等任意子进程"（只能按 HANDLE 等） | `src/os/proc.cpp:1173`（阻塞回收）、`:1263`（`WNOHANG` 轮询） | **需适配**。`Process` 持 `HANDLE`；退出码直传；"被信号杀死"用 `128+signum` 约定并在文档写清 |
| `kill(pid, SIGTERM/SIGKILL)` | 信号可捕获/可忽略，可优雅关闭 | `TerminateProcess`（不可捕获） | **没有优雅关闭手段**；`SIGTERM` 无对应 | `src/os/proc.cpp:1335`（`::kill`）、`src/os/pool.cpp:419`（`SIGKILL`）、`src/media/audio.cpp:512`（停 ffmpeg） | **需适配**。`SIGTERM` → `GenerateConsoleCtrlEvent(CTRL_BREAK)` 或自定义 IPC 退出请求，超时才 `TerminateProcess`；`SIGKILL` → 直接 `TerminateProcess` |
| `pidfd_open` | 用 fd 等待进程退出，可 epoll | 无（用进程 HANDLE） | 句柄语义不同但能力等价 | `src/os/proc.cpp:1258`（`SYS_pidfd_open`） | **需适配**（非必须）。Windows 分支不实现 pidfd，直接用 `HANDLE` 等待；pidfd 代码整体放 `#ifdef SYS_pidfd_open` |
| `setsid/setuid/setgid/setgroups/setrlimit/setpriority/sched_setscheduler` | 子进程降权/资源限制/会话分离 | 无对应（需 token/Job Object） | 权限模型是 ACL+Token，不是 uid/gid；`rlimit` 对应 Job Object 但语义不同 | `src/os/proc.cpp:1090-1100`（`ChildRun` 设置段）、`:793`（`oom_score_adj`） | **需适配（能力降级）**。Windows 上这些选项一律返回"不支持"并写日志，**不要假装生效**；确需资源限制另做 Job Object 路径 |
| `getpid` | 进程号 | `GetCurrentProcessId` | 基本一致（pid 复用时机不同） | `src/bento/net/peer_hub.cpp:361`、`src/bento/net/link.cpp:761`、`src/bento/net/coap.cpp:556`、`src/debug/debug_dump.cpp:773` | **无需适配**：只用于唯一后缀/日志，pid 复用差异不影响正确性；包一层 `CurrentPid()` 即可 |

#### 1.3.4 文件（7 类）

| 调用 | Linux 契约(语义/返回/错误码/边界) | Windows 对应 | **契约差异** | 对本项目影响(file:line) | 结论 |
|---|---|---|---|---|---|
| `open/openat` | `O_CLOEXEC`/`O_NOFOLLOW`/`O_DIRECTORY`/`O_APPEND` 语义齐全；路径分隔符 `/` | `CreateFile` | 句柄是 `HANDLE`；继承需 `SECURITY_ATTRIBUTES`；**无 `O_NOFOLLOW`**（用 `FILE_FLAG_OPEN_REPARSE_POINT` + 校验 reparse tag）；目录需 `FILE_FLAG_BACKUP_SEMANTICS`；CRT 文本模式会自动 CRLF（必须二进制或直接用 `CreateFile`） | `src/os/file.cpp:68`（`SYS_openat`）、`src/bento/app/file_browser.cpp:249`（`O_NOFOLLOW`）、`:542`（`O_DIRECTORY`）、`src/bento/app/fetch.cpp:1100` | **需适配**。文件层统一 `OpenMode` → `CreateFile` 映射；`O_NOFOLLOW` 用 reparse point 校验；`O_DIRECTORY` 用 `BACKUP_SEMANTICS`；**禁止 CRT 文本模式** |
| `read/write` | `ssize_t`；部分写；`EINTR` | `ReadFile/WriteFile` | `DWORD` 长度（单次 ≤4GB）；同步句柄也可用；错误 `GetLastError`；无 `EINTR` | `src/os/file.cpp:369`（`SYS_read`）、`:430`（`SYS_write`）、`:694`/`:719`（`readv/writev`）、`src/distributed/metadata_store.cpp:96`（WAL 写） | **需适配**。封装 `ReadFull/WriteFull`（部分写循环已存在，保留）；单次 >2GB 分块；`readv/writev` 在 Windows 退化为循环 |
| `fsync` + 目录 `fsync` | 文件 `fsync` 持久；**目录 `fsync` 保证 rename 持久** | `FlushFileBuffers` | 文件有效；**目录句柄 `FlushFileBuffers` 失败（`ERROR_ACCESS_DENIED`）**，目录持久化无法等价 | `src/distributed/coordinator_snapshot.cpp:174`（文件 fsync）、`:192`（目录打开）、`:199`（目录 fsync）；`src/storage/wal.cpp:296`；`src/storage/sstable.cpp` | **需适配**。目录 fsync 在 Windows 上 no-op + 注释说明"崩溃后 rename 可能丢失"；或临时文件用 `FILE_FLAG_WRITE_THROUGH` 缩小窗口。**这是持久性契约的实质弱化，必须写进文档** |
| `rename`（原子覆盖） | **原子替换**已存在的目标；目标不存在也可 | `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` | CRT `rename()` 目标存在则失败（`EEXIST`）；`MoveFileEx` 同卷近似原子，**跨卷是复制（非原子）** | `src/distributed/coordinator_snapshot.cpp:185`、`src/storage/sstable.cpp:306`、`src/bento/app/file_browser.cpp:297`、`src/bento/app/fetch.cpp:1418`、`src/bento/app/backup.cpp:957`、`src/logging/log.cpp:661` | **需适配**。统一 `AtomicReplace(src,dst)`：Windows 用 `MoveFileEx(REPLACE_EXISTING\|WRITE_THROUGH)` 并校验同卷；失败保留 tmp 供恢复（现有 tmp 清理逻辑保留） |
| `unlink`（打开中的文件） | 立即删目录项，**打开者 fd 继续有效**（日志轮转/临时文件依赖此语义） | `DeleteFile` | 默认共享模式**不允许删除**；需 `FILE_SHARE_DELETE`，且删除只是"标记删除"，最后一个句柄关闭才真正删 | `src/bento/app/file_browser.cpp:301/314/320/324/354/362`、`src/bento/app/fetch.cpp:1203/1376/1605`、`src/distributed/coordinator_snapshot.cpp:169`、`src/storage/sstable.cpp:150` | **需适配**。所有文件打开加 `FILE_SHARE_DELETE`；"删除打开中的文件"改为"关闭后删除"或延迟删除；**日志轮转必须走"关闭→rename→重开"**，不能依赖 unlink 语义 |
| `opendir/readdir` | 返回 `.`/`..`；顺序不保证但稳定 | `FindFirstFile/FindNextFile` | 不返回 `.`/`..`（需自己补）；通配符 `dir\*`；顺序不同 | `src/bento/app/capability.cpp:305`、`src/bento/app/file_browser.cpp:546`（`fdopendir`） | **需适配**。目录遍历封装；检查现有代码是否依赖顺序（不依赖则只需换实现） |
| 路径分隔符 / 大小写 / `MAX_PATH` | `/`；大小写敏感；长度基本无限制 | `\`（多数 API 也接受 `/`）；**大小写不敏感**；260 字符（需 `\\?\` 前缀） | 大小写不敏感会让 `a.txt`/`A.txt` 冲突；超长路径必须加前缀 | 已有 Windows 分支：`src/path/filepath.cpp:57`、`:204`、`:622`、`:690` 等；`include/go2cpp/path/filepath.hpp:97` | **无需适配（代码已存在）**：`filepath` 已有完整 `_WIN32` 词法实现；但需实测大小写冲突与 260 字符边界（现有注释也承认"Windows 未真机验证"） |

#### 1.3.5 诊断 / 系统信息（6 类）

| 调用 | Linux 契约(语义/返回/错误码/边界) | Windows 对应 | **契约差异** | 对本项目影响(file:line) | 结论 |
|---|---|---|---|---|---|
| `/proc/*`（stat/status/fd/maps/net/dev/diskstats/loadavg/oom_score_adj） | 文本伪文件，字段稳定、可直接解析；进程视图/内核计数都在这里 | **无**（需 `NtQuerySystemInformation`/PDH/`GetProcessMemoryInfo`/`GetAdaptersAddresses`） | 没有等价统一接口；每个指标要单独找 API，且语义/单位不同（jiffies vs 100ns） | `src/os/proc.cpp:345`（`/proc/stat`）、`:385`（`/proc/<pid>/stat`）、`:531`（`/proc/<pid>/fd`）、`:1943`（`oom_score_adj`）、`:2347`（`/proc/self/fd`）；`src/debug/debug_server.cpp:1422`、`:1442`；`src/os/monitor/cpu_monitor.cpp:12`、`net_monitor.cpp:78`、`fileio_monitor.cpp:101/109/159`；`src/concurrency/fiber.cpp:3089`、`:3221` | **需适配（重写采集层）**。抽 `SystemProbe`：Windows 用 `GetProcessMemoryInfo`/`GetProcessIoCounters`/`GetSystemTimes`/`GetAdaptersAddresses`/PDH；**不可得指标返回"不支持"而不是 0**（否则监控图会撒谎） |
| `readlink("/proc/self/exe")` | 返回可执行文件绝对路径 | `GetModuleFileNameW` | 编码（UTF-16 vs UTF-8）、路径分隔符 | `src/concurrency/fiber.cpp:3089`、`src/debug/diagnostic_agent.cpp:323` | **需适配**（简单）。封装 `ExecutablePath()`，统一转 UTF-8 |
| `/proc/self/maps`（指针审计） | 可枚举全部映射（含匿名、权限、offset） | `VirtualQuery` + `EnumProcessModules` | 只能枚举"已提交区间"和模块；匿名映射来源不可得 | `src/concurrency/fiber.cpp:3221`、`:3503`；`src/debug/diagnostic_agent.cpp:325` | **需适配（功能降级）**。`VirtualQuery` 遍历 + 模块枚举；文档明确"匿名映射无法归属" |
| eBPF（`bpf/` + `src/os/monitor`） | 内核态追踪 syscall/sched/net/futex 延迟 | **无** | 完全没有对应能力 | `bpf/*.bpf.c`（6 个程序）、`cmake/Go2CppMonitor.cmake` | **无需适配**：Windows 不构建该模块，整体功能关闭并文档标注（`cmake/Go2CppMonitor.cmake` 已有"仅 /proc 采集"的降级路径） |
| PTY（`posix_openpt`/`grantpt`/`unlockpt`/`ptsname_r`/`ioctl(TIOCSWINSZ)`） | 伪终端主从设备 | `ConPTY`（`CreatePseudoConsole`，Win10 1809+） | 完全不同的一套 API；没有"master fd"可 `read/write` | `src/os/pty.cpp:113`（`posix_openpt`）、`:117`、`:123`、`:161`（`TIOCSWINSZ`）；`src/network/hook.cpp:2729`（`TIOCGWINSZ` 拦截） | **需适配**。`pty` 模块 Windows 重写为 ConPTY；不做则整块功能关闭（不要留半成品） |
| `shm_open`/`mq_open` | POSIX 共享内存/消息队列，命名空间 `/name` | `CreateFileMapping` / 命名管道或 `CreateMailSlot` | 句柄语义、命名空间（`Global\`/`Local\`）、权限模型都不同 | `src/os/shm.cpp:95`、`:107`、`:136`；`src/os/mq.cpp:164`、`:203`、`:292`、`:338` | **需适配**。重写句柄与命名空间；Windows 全局命名加 `Global\` 前缀；消息队列用命名管道模拟（注意消息边界语义） |

#### 1.3.6 杂项（5 类）

| 调用 | Linux 契约(语义/返回/错误码/边界) | Windows 对应 | **契约差异** | 对本项目影响(file:line) | 结论 |
|---|---|---|---|---|---|
| `dlopen/dlsym` | `.so`；`RTLD_NOW/LOCAL`；`dlerror` | `LoadLibrary/GetProcAddress`；`.dll` | 扩展名/搜索路径不同；`LoadLibrary` 默认会把依赖 DLL 的目录加入搜索；错误用 `GetLastError` | `src/media/audio.cpp:602`（Opus 动态加载）；`src/config/so_reloader.cpp:298`（热加载） | **需适配**。封装 `DynamicLibrary`；`so_manifest` 的 ELF 哈希校验改为 PE 哈希/签名校验 |
| `inotify`（配置热加载） | 内核事件：`IN_MODIFY/IN_MOVED_TO` 等 | `ReadDirectoryChangesW` | 事件模型不同（重命名/覆盖的事件序列不同）；只能监控目录 | `include/go2cpp/config/watch.hpp:1522`（`inotify_init1`）、`:1534`、`:1552` | **需适配**。Windows 用 `ReadDirectoryChangesW`；**已有轮询回退**（`watch.hpp:1524` 注释"回退轮询"），Windows 先走回退也可接受 |
| `timerfd` | 定时器作为 fd 可 poll | `CreateWaitableTimer` | 本项目定时器由 epoll 超时驱动 | **0 处实际调用**（只在 `src/os/monitor/syscall_names.inc:287` 作为 syscall 名字出现） | **无需适配**：未使用；见 `src/concurrency/timer.cpp:283`（deadline 队列与 `epoll_wait` 同一次返回） |
| `gettimeofday` | 墙钟微秒 | `GetSystemTimeAsFileTime` | 本项目未使用 | **0 处实际调用**（`grep -rn "gettimeofday" src/ include/` 仅命中注释） | **无需适配**：未使用；墙钟统一走 `clock_gettime(CLOCK_REALTIME)`（`src/os/mq.cpp:45`） |
| `umask` / 权限位 | `0666 & ~umask` 语义 | ACL | 完全不同；`0644` 在 Windows 上会被忽略 | `src/os/file.cpp` 的 `mode` 参数、各 `::open(..., 0644)` 调用点 | **需适配**（轻）。Windows 上 `mode` 参数忽略并注释；敏感文件（WAL、密钥）改用 ACL 收紧，不能假装 0600 生效 |


---

## 二、开发 + 三件套（第二步，在调查完成后）

| 项 | 内容 |
|---|---|
| **开发** | 按调查结论逐个适配（**每个差异点一个 commit**） |
| **三件套** | 每个适配点必须有：① 代码 ② 单测 ③ 文档（`docs/modules/<module>.md` 加「平台差异」节） |
| **平台抽象层** | 强制走 `#if defined(_WIN32)` 分支，**不允许散落的平台判断** |

---

## 三、重复验收 + 压测（第三步）

| 项 | 内容 |
|---|---|
| **单元测试** | Windows 上全量跑（核心子集必跑） |
| **E2E** | douyin 最小链路（登录→feed→发布→播放）在 Windows 上跑通 |
| **压测** | 同 Linux 的口径（`taskset` 等价 → `SetThreadAffinityMask`；100ms–1s 窗口 + 5 轮 + CV<5%） |
| **重复** | 每个适配点改完都要重跑（**不是最后跑一次**） |
| **回归** | Linux 侧必须同时保持绿（**不能为了 Windows 弄坏 Linux**） |

---

## 四、验收标准（不可绕过）

- [ ] 调查表覆盖**本项目用到的全部系统调用**（`grep -rn` 全部 syscall 包装）
- [ ] 每个差异点有「对本项目的影响」结论（不是照抄文档）
- [ ] 每个适配点有三件套（代码/单测/文档）
- [ ] Windows 上核心测试全绿
- [ ] Windows 上 douyin 最小链路跑通
- [ ] **Linux 侧无回归**（同 commit 跑一遍 Linux）
- [ ] 压测数据与 Linux 可比（同口径）
