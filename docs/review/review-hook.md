# hook 模块审查报告

- 审查对象：`src/network/hook.cpp`（2832 行）、`include/go2cpp/network/hook.hpp`（355 行）
- 基线：`a2fa0df`；本模块文件 md5 与审查快照一致（同期 Bento 工作区有并发改动，不影响本模块行号）
- 审查方式：只读快照 + 逐函数阅读；**未运行**编译/测试（本模块需要 LD_PRELOAD 宿主进程，仓内无独立 hook 测试）
- 统计：**P0×3 / P1×4 / P2×5，共 12 条**

## 1. 问题清单

| ID | 级别 | 位置 | 问题 | 证据 | 建议修复 |
|---|---|---|---|---|---|
| HOOK-01 | P0 | `src/network/hook.cpp:956-970,1009,1153-1159` | 套接字超时被映射为 `ETIMEDOUT`，违反 Linux `SO_RCVTIMEO/SO_SNDTIMEO` 契约（应为 `EAGAIN/EWOULDBLOCK`） | `case IOWaitStatus::kTimeout: return ETIMEDOUT;`（960-961）→ `errno = error; return -1`（1157-1159）；`native_wait` 到期同样 `return ETIMEDOUT`（1009） | read/write/recv/send 的 `kTimeout` 映射为 `EAGAIN`；仅 connect 保留 `ETIMEDOUT`；补一条与原生 `SO_RCVTIMEO` 对照测试 |
| HOOK-02 | P0 | `src/network/hook.cpp:2517-2529`（白名单 1449-1478） | 非白名单 `ioctl` 一律 `ENOTSUP`，宿主进程的终端/网卡/文件系统 ioctl 全线失败；不检查返回值的调用方会读未初始化 `winsize` | `if (!ioctl_has_pointer_argument(request)) { ... errno = ENOTSUP; return -1; }`（2523-2529）；白名单仅 `FIONBIO/FIONREAD/TIOCOUTQ/FIOCLEX/FIONCLEX/TIOCEXCL/TIOCNXCL` | 只对「无参且不解析」的命令转发，其余原样透传内核；或默认 passthrough，`ENOTSUP` 仅用于显式诊断模式 |
| HOOK-03 | P0 | `src/network/hook.cpp:1138-1140,1217-1224` | `EINTR` 被静默重试且无次数/退避上限，信号对调用方不可见；connect 的 `do-while` 无条件重试，Linux 上 connect 的 EINTR 表示连接已中止，重试只会得到 `EALREADY` | `if (result == -1 && errno == EINTR && manager) continue;`；`} while (result < 0 && errno == EINTR && manager);`；`continue` 位于 blocking 判断之前，`MSG_DONTWAIT` 分支也被重试 | 默认原样返回 `EINTR`；确需重试时限次并检查 deadline/取消；connect 遇 EINTR 直接返回 |
| HOOK-04 | P1 | `src/network/hook.cpp:2186-2196` | `accept4` 完全没有 `OpEnabled(kAccept)` 检查：关闭 `enable_accept` 后 accept 走原生，accept4 仍 park 并注册新 fd，配置开关静默失效 | accept 分支有 `if (!OpEnabled(HookOp::kAccept))`（2170-2176），accept4 直接 `cooperative_io<int>(fd, s_originals.m_accept4, ...)`（2188-2191） | accept4 入口补同样检查，关闭时直调 `m_accept4` 且不注册新 fd |
| HOOK-05 | P1 | `src/network/hook.cpp:1103-1120`（adopt 链 546-681） | 未接管的 fd 没有「不可接管」负缓存：普通文件/字符设备/anon_inode 每次 read/write 都重新 `getsockopt(SO_TYPE)+fstat`，Fiber 内写日志文件每次 write 额外 2 次 syscall + 2 次 registry 条带锁 | `descriptor_for(fd)` 为空 → `try_adopt_socket` → `try_adopt_fifo` → 仍空 → `invoke_real`（1103-1120）；`adopt_socket` 每次先 `DescriptorGuard lifecycle(fd)`（557） | 增加每 fd/每线程负结果缓存（close 时失效），或只对疑似 socket/FIFO 的 fd 探测；文档「普通文件直调」改为「首次探测后直调」 |
| HOOK-06 | P1 | `src/network/hook.cpp:1173-1184,504-510,347` | connect 超时被全局默认值覆盖：`SO_SNDTIMEO={0,0}`（显式无限）被映射为 `nullopt`，回落默认 5000ms；未设超时也一律 5s；在途非阻塞连接超时后不会取消 | `if (value.tv_sec == 0 && value.tv_usec == 0) return std::nullopt;`（508-510）；`if (milliseconds < 0) return nullopt; ...`（1180-1183） | 用三态表示「未设置/0=无限/具体值」；默认 connect 超时配置化并文档化；超时后 shutdown/放弃在途连接 |
| HOOK-07 | P1 | `src/network/hook.cpp:1312-1344,1941-1989` | 协作 sleep 没有信号唤醒路径：`nanosleep/sleep/usleep/clock_nanosleep` 只等 `manager->WaitFor(fd, kRead, duration)` 超时，不会被信号打断；`while (nanosleep(...) == -1 && errno == EINTR)` 在 Fiber 里永不退出，SIGTERM 优雅退出失效 | `const auto result = manager->WaitFor(fd, IOEvent::kRead, duration);`（1337）；Interrupted 分支（1972-1983）只在 WaitFor 返回非 Timeout 才执行 | 增加可被信号唤醒的等待源（self-pipe/signalfd），或在 hook.hpp 明确写出「sleep 不响应信号」的 ABI 限制 |
| HOOK-08 | P2 | `src/network/hook.cpp:2598-2627` | `setsockopt` 在原始系统调用成功返回后再次解引用调用方 `timeval*`（2619），跨线程释放/复用时是 TOCTOU/UAF 读 | `invoke_real(s_originals.m_setsockopt, ...)`（2606）→ `timeval_duration(*static_cast<const timeval*>(value))`（2619） | 调用前把 timeval 复制到本地，再基于副本更新元数据；或改读内核状态（FIONBIO 后 `F_GETFL` 的做法） |
| HOOK-09 | P2 | `src/network/hook.cpp:2063-2128,2286-2305,2358-2362` | 开关覆盖缺口：`hook.enable_poll/select/epoll_wait` 从不被读取（六个入口直接 `invoke_native_blocking`，死配置）；`recvmsg/sendmsg` 无独立 HookOp 也不检查 `kRecv/kSend` | poll/select/epoll_wait 的 `return invoke_native_blocking(...)`（2069/2080/2091/2103/2115/2126）无 OpEnabled；`recvmsg` 直接 `cooperative_io(...)`（2303），sendmsg 同（2360） | 删除死开关或让其真正生效；recvmsg/sendmsg 复用 kRecv/kSend 检查 |
| HOOK-10 | P2 | `src/network/hook.cpp:1291,1322-1329` | 每线程 `t_sleep_eventfd` 创建后永不 close，线程退出泄漏一个 fd；短命线程 + Fiber sleep 会累积到 `RLIMIT_NOFILE` | `thread_local int t_sleep_eventfd = -1;`（1291）仅在 fd<0 时 `eventfd2`（1324-1329），全文件无 close | thread_local RAII 句柄析构 close，或线程池退出路径统一回收 |
| HOOK-11 | P2 | `src/network/hook.cpp:2364-2371`（notify 809-811） | close 在持有 `DescriptorGuard` 时调用 `IOManager::NotifyCloseAll`，把外部组件放进 fd 生命周期临界区，违反 `docs/modules/io.md`「Guard 不能跨阻塞/park」的约束；被唤醒 Fiber 可能等待整个通知过程（需人工确认 NotifyCloseAll 是否同步进入 hook） | `DescriptorGuard lifecycle(fd); plan = prepare_close(fd); notify_before_close(fd);`（2368-2370）→ `IOManager::NotifyCloseAll(fd);`（809-811） | `prepare_close` 已置 `m_closed`，先释放 Guard 再 notify，保持「真实 close 之前通知」语义 |
| HOOK-12 | P2 | `src/network/hook.cpp:274-319,485-494` | fork 安全无防护：子进程继承其他线程持有的 `std::once_flag` 与 64 个 `DescriptorStripe` mutex，首次 hook 可能永久 futex 等待；无 `pthread_atfork` 重置（需人工确认子进程是否会调用 hook） | `std::call_once(s_originals_once, [] {...});`（278）；`static std::array<DescriptorStripe,64> stripes`（492） | 注册 `pthread_atfork` 子进程回调（重置 once/锁或置 fork 标记走裸 syscall）；至少在文档标注「fork 后子进程仅允许裸 syscall」 |

## 2. 重点问题说明

### HOOK-01（P0）超时 errno 语义
`recv/send` 在 `SO_RCVTIMEO/SO_SNDTIMEO` 到期时必须返回 `-1` 且 `errno=EAGAIN`（Linux man 7 socket）。hook 把内部 `IOWaitStatus::kTimeout` 统一翻译成 `ETIMEDOUT`，会让 OpenSSL、gRPC、Netty 之类「EAGAIN 即重试、ETIMEDOUT 即断链」的库在 hook 下行为突变。修复成本低（一处映射），影响面大。

### HOOK-02（P0）ioctl 白名单
`LD_PRELOAD` 场景下 hook 会接管宿主所有 ioctl；当前实现对白名单外的「无指针参数」命令直接 `ENOTSUP`。终端 `TIOCGWINSZ`、串口 `TCGETS`、网卡 `SIOCGIFADDR`、文件系统 `FS_IOC_*` 都属于这一类。宿主代码通常不检查 ioctl 返回值，随后读取未初始化的 `winsize/termios`，属于典型的「hook 静默破坏宿主」问题。建议默认透传，只对需要特殊语义的命令做处理。

### HOOK-03（P0）EINTR 静默重试
hook 在 `cooperative_io` 的循环里无条件 `continue`，会让「带超时/可取消的阻塞调用」无法被信号打断；connect 的 EINTR 语义尤其危险（Linux 上连接已中止）。建议遵循 libc：默认返回 EINTR，把重试策略交给调用方。

## 3. 亮点

- Descriptor 生命周期设计有三重门：`shared_ptr` 保活 + token/generation 二次校验 + `DescriptorGuard`，未发现确定的 close/poll UAF；`prepare_close/finish_close` 两阶段语义正确。
- 内部转发统一走 `RealCallGuard` 防递归，避免 hook 内部再次被 hook。
- socket→FIFO 的 adopt 白名单与 `descriptor_for` 分派结构清晰，注释解释了每个分支的动机。
- `native_wait` 的 10ms 切片、`ScopedEnable` 进程级计数等实现有明确注释（尽管存在文档漂移，见 HOOK-09）。

## 4. 维度结论

| 维度 | 结论 |
|---|---|
| 正确性 | 部分读写的短读语义正确；EINTR/超时/sleep 三处破坏 libc 契约；fork 未防护 |
| 并发 | 未发现确定的 close/poll UAF；close 持 Guard 调 NotifyCloseAll 违反既有约束；fork 后锁状态未重置 |
| 资源与错误 | 每线程 eventfd 泄漏；connect 超时后在途连接未取消；`ENOTSUP/ETIMEDOUT` 与 libc/Go 语义不一致 |
| 风格与可维护性 | 注释质量总体高但存在漂移（`ScopedEnable` 线程/进程语义、poll 类死开关、shutdown 自承未 hook）；10ms 切片为硬编码魔法数（1004、1014-1024） |

## 5. 未覆盖与限制

- 快照未包含 `include/go2cpp/network/io.hpp`、`src/network/io.cpp`、`src/concurrency/thread_policy.cpp`；`DescriptorGuard` 的「按 fd 分条带递归锁」语义取自 `docs/modules/io.md`，若源码不同，dup/dup2 的 Capture 重入结论需重估。
- 未做运行时验证（需要构造 LD_PRELOAD 宿主进程），因此 P0 的触发条件按代码路径推断，均给出精确行号供复核。
