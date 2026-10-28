# 模块文档：os（多进程 / IPC）

> **适用 HEAD**：`1b46de8`（2026-10-26，「MCP 框架 + os 多进程/IPC + eBPF 监控内置」）。
> **构建**：`/UserData/build/go2cpp-mcp-integrated`（`-DGO2CPP_WITH_MCP=ON`，Linux/WSL2 x86_64）。
> **状态**：现行。本文只描述**当前树内**的 `include/go2cpp/os/**` 与 `src/os/**`；
> 监控（`os/monitor`）只在边界处提及。
>
> **合并说明**：本文合并迁移前的 `docs/os-final-report.md`（2026-10-24，位于旧工作区
> `/UserData/CodexWorkSpace/Go2Cpp-mcp-old-20261025/docs/`，**不在本树**，原文保留未删）
> 的 R1–R31 完成度、压测数据与修复记录；同时吸收 `docs/module-audit-2026-10-20.md`
> 第 57 行的 OS 实测、`docs/modules.md` 第 67–87 行的接入状态。旧报告顶部的
> 「最终报告」定位不再适用：其中的测试装置（`tests/os/test_os_proc_lifecycle.cpp` 等）
> **没有随代码迁入本树**，数据按「迁移前基线」标注。

---

## 1. 概述

`go2cpp::os` 是最低层的 POSIX 能力封装，分两块：

1. **文件 / 目录**：`File`（普通文件 RAII 句柄）、`ReadFile`/`WriteFile`、环境变量、
   `ReadDir`/`MkdirAll`/`RemoveAll`/`Readlink`/`Symlink`/`SetTimes`/`StatPath`、
   `Args`/`Exit`/`SaveDefaultConfig`。
2. **多进程 / IPC**：`Process`/`Spawn`/`Run`、`Pipe`/`SocketPair`、`MessageChannel`
   与 G2CF 帧、`SharedMemory`/`RingBuffer`/`ShmChannel`、`MessageQueue`/`MqChannel`、
   `Pty`、`Supervisor`、`ProcessPool`、`IoCapability`/`AsyncIoBinding`。

### 1.1 设计前提：不参与协程调度

普通文件 IO 在内核里同步完成、不会 `EAGAIN`；把它接入 `IOManager` 只会白付一次
park/wake。因此本模块**全部 fd 级读写走裸 `syscall(SYS_*)`，不经过 `go2cpp_hook`**，
行为与 Go 的 `os.File` 一致（`file.hpp:1-115`、`proc.hpp:12-28`）。直接后果：

- **在 Fiber 里调用本模块的阻塞 API 会阻塞它所在的 worker 线程（M）**；
- 需要协程友好的等待，必须显式调用 `EnableAsync(IOManager*)`（仅对 pollable fd 有效），
  或把调用放到独立线程；
- `File` **只接受普通文件**：`Adopt` socket/FIFO/eventfd 会返回 `kIsSocket`/
  `kNotRegularFile`，因为绕过 Hook 的 `read` 会把 M 卡死（`file.cpp:254-270`、`:323-337`）。

### 1.2 最小示例

```cpp
#include "go2cpp/os/proc.hpp"

using namespace go2cpp::os;

IgnoreSigPipe();                       // 写已关闭管道时返回 EPIPE，而不是被杀
ProcessOptions options;
options.argv = {"/bin/sh", "-c", "echo hi; echo err >&2; exit 3"};
auto run = Run(options);               // 收集 stdout/stderr + 超时阶梯
// run.value().standard_output == "hi\n"；exit_code == 3
```

```cpp
auto pipe = Pipe::Create();            // 默认 O_CLOEXEC
int write_fd = pipe.value().ReleaseWrite();
```

---

## 2. 分层与文件映射

| 子层 | 公开头文件 | 实现 | 职责 | 不负责 |
| --- | --- | --- | --- | --- |
| 进程 | `include/go2cpp/os/proc.hpp` | `src/os/proc.cpp` | fork/exec、`wait4`、信号、`/proc` 查询、`Run` 输出泵、Supervisor/Pool 的底座 | 协程调度、Hook |
| 字节通道 | `channel.hpp` | `channel.cpp`、`raw_io.cpp` | fd 通道、G2CF 帧、Pipe/SocketPair、背压、后端注册表 | 帧的语义、协议解析 |
| 共享内存 | `shm.hpp` | `shm.cpp` | 命名共享内存、SPSC 环、跨进程环通道 | 事件通知（读方轮询） |
| 消息队列 | `mq.hpp` | `mq.cpp` | POSIX mq：优先级、批量、ENOBUFS、epoll 异步 | 大块流（用 shm） |
| PTY | `pty.hpp` | `pty.cpp` | `posix_openpt`、窗口大小、`EIO→EOF`、异步 | 终端行规程实现 |
| 监督器 | `supervisor.hpp` | `supervisor.cpp` | 单线程 `ppoll` 循环、超时阶梯、条件、配额、输出泵 | 跨线程共享可变状态 |
| 进程池 | `pool.hpp` | `pool.cpp` | 有界队列、拒绝、缩扩容、异常补位 | 任务业务语义 |
| IO 能力 | `io_capability.hpp` | `io_capability.cpp` | fd 分类、`DetectIoCapability`、`AsyncIoBinding` | 具体后端实现 |
| 文件/目录 | `file.hpp`、`dir.hpp`、`os.hpp` | `file.cpp`、`dir.cpp`、`os.cpp` | 普通文件、路径操作、环境变量/Args/Exit | socket/pollable fd |

依赖方向：`supervisor/pool → proc → channel/pty/mq/shm → io_capability → file`；
`file`/`dir` 不依赖任何 IPC 子层。CMake 目标为 `go2cpp_os`（`cmake/Go2CppModules.cmake`），
由 `go2cpp::runtime` umbrella 导出（`docs/modules.md:87`）。

---

## 3. API 一览

所有类型位于 `namespace go2cpp::os`，错误统一走 `Result<T>`/`ProcError`（`proc.hpp:118-249`）。

### 3.1 进程：`Process` / `Spawn` / `Run`

| API | 位置 | 说明 |
| --- | --- | --- |
| `struct ProcessOptions` | `proc.hpp:411` | argv 数组（不经 shell）、env 白名单/覆盖、cwd、nice/亲和性/sched、umask、uid/gid、rlimits、OOM、stdio 模式、`use_pty`、`max_output_bytes` |
| `enum class StdioMode` | `proc.hpp:376` | `kPipe`/`kInherit`/`kNull`/`kFile`/`kPty` |
| `struct ExitStatus` | `proc.hpp:312` | `reaped/exited/signaled/core_dumped/timed_out` + `exit_code/signal/wall_ms` |
| `struct ProcessInfo` | `proc.hpp:285` | `/proc/<pid>/stat` 快照：state/RSS/vsize/CPU/线程/启动时间 |
| `enum class LifecycleState` | `proc.hpp:253` | `kCreated→kRunning→kTerminating→kKilled→kReaped`（失败 `kFailed`） |
| `class Process` | `proc.hpp:573` | RAII 子进程句柄；`Spawn`/`Wait`/`TryWait`/`Kill`/`Terminate`/`KillTree`/`Info`/`Alive`/`SetNice`/`SetAffinity`/`SetScheduling`/`SetRlimit`/`SetOomScore`/`stdin_channel`/`stdout_channel`/`stderr_channel`/`native_fd`/`CloseChannels`/`Usage` |
| `Result<Process> Spawn(...)` | `proc.hpp:684` | 便捷形式 |
| `struct RunResult` | `proc.hpp:692` | `status` + stdout/stderr + 截断计数 |
| `Result<RunResult> Run(...)` | `proc.hpp:719` | 写 stdin、并行排空两路输出、超时 TERM→KILL、回收 |
| `ProcStatsSnapshot StatsSnapshot()` | `proc.hpp:782` | 累计 spawned/exited/timed_out/killed/reaped/live/open_fds |
| `void IgnoreSigPipe()` | `proc.hpp:807` | 显式忽略 SIGPIPE（本模块不擅自改全局信号处理） |
| `struct ProcessDefaults` / `LoadProcessDefaults` | `proc.hpp:822`、`:836` | 读 `os.proc.*`（`src/os/config.cpp:61-80`） |

超时语义全文统一（`proc.hpp:29-35`）：`0` 立即返回、`<0`（`kWaitForever`）永久、
`>0` 最多等待并返回 `kTimeout`。

### 3.2 字节通道：`MessageChannel` / `Pipe` / `SocketPair`

| API | 位置 | 说明 |
| --- | --- | --- |
| `struct Frame` / `enum Framing` | `channel.hpp:126`、`:133` | `kRaw`/`kLine`/`kLengthPrefixed`（G2CF） |
| `struct ChannelOptions` | `channel.hpp:140` | `max_frame_bytes`（默认 8 MiB，上限 64 MiB）、`read_chunk_bytes` |
| `class MessageChannel` | `channel.hpp:179` | `Read`/`Write`/`WriteAll`/`ReadLine`/`WriteLine`/`ReadFrame`/`WriteFrame`/`ReadToEnd`/`Close`/`native_fd`/`stats`/`Capability`/`SetNonBlocking`/`EnableAsync`/`DisableAsync` |
| `class FdChannel` | `channel.hpp:298` | pipe/socketpair 后端；继承 `MessageChannel` |
| `class Pipe` | `channel.hpp:344` | `Create`/`ReleaseRead`/`ReleaseWrite`/`CloseRead`/`CloseWrite`/`Capability`/`SetNonBlocking` |
| `class SocketPair` | `channel.hpp:381` | `Create(type=SOCK_STREAM)`、`ReleaseFirst`/`ReleaseSecond` |
| `class FlowBuffer` | `channel.hpp:444` | 高/低水位 + `kBlock`/`kTimeout`/`kDrop` 背压 |
| `class ChannelRegistry` | `channel.hpp:518` | 内置 `pipe`/`socketpair`/`shm`/`mq`，可运行时 `Register` 新后端 |

G2CF 帧（`channel.hpp:19-34`）：`magic "G2CF"` + `version=1` + `type` + `flags` +
`payload_size` + payload + CRC32（覆盖 version..payload）；实现见 `channel.cpp:44`
（CRC32）、`:196`（`WriteFrame`）、`:221`（`ReadFrame`）；单帧上限
`kMaxFramePayloadLimit = 64 MiB`（`channel.hpp:123`）。

### 3.3 共享内存与 SPSC 环

| API | 位置 | 说明 |
| --- | --- | --- |
| `enum class ShmAccess` | `shm.hpp:75` | `kReadOnly`/`kReadWrite` |
| `class SharedMemory` | `shm.hpp:89` | `Create`/`Open`/`Unlink`/`Resize`/`data`/`size`/`fd`；析构只 munmap+close，不 unlink |
| `class RingBuffer` | `shm.hpp:161` | 在外部内存上 `Init`/`Attach`，`Write`/`Read` 无锁，`Readable`/`Writable`/`dropped_bytes` |
| `class SharedRingBuffer` | `shm.hpp:206` | 命名共享内存 + 已初始化环 |
| `class ShmChannel` | `shm.hpp:256` | 把环适配成 `MessageChannel`；无内核事件，读方短轮询（空读后退避 50µs，`shm.cpp:531`） |

`RingBuffer` 的 header 固定 64 字节、64 字节对齐（`shm.cpp:222-230`），字段为
`magic/capacity/flags/write_pos/read_pos/dropped`；`Init`/`Attach`/`Write`/`Read`
分别在 `shm.cpp:245`/`:270`/`:292`/`:348`。

### 3.4 消息队列

| API | 位置 | 说明 |
| --- | --- | --- |
| `enum class QueueOverflowPolicy` | `mq.hpp:77` | `kBlock`/`kTimeout`/`kDropNewest` |
| `struct QueueMessage` | `mq.hpp:84` | payload + priority |
| `class MessageQueue` | `mq.hpp:95` | `Create`/`Open`/`Unlink`/`Send`/`Receive`/`SendBatch`/`ReceiveBatch`/`Capability`/`SetNonBlocking`/`EnableAsync`/`DisableAsync` |
| `class MqChannel` | `mq.hpp:211` | `MessageChannel` 的 mq 后端；`Write` 一条消息 = 一条 mq 消息 |

实现：`mq.cpp:145`（`Create`，`O_CREAT|O_EXCL`）、`:237`（`Send`，`mq_timedsend` +
相对→绝对时间换算）、`:325`（`Receive`，`mq_timedreceive`）、`:475`（`EnableAsync`，
`AsyncIoBinding::Enable` 走 epoll，不用 `mq_notify`）。`MqChannel` 的默认队列参数是
10 条 × 8192 字节（`mq.cpp:512-516`），受 `/proc/sys/fs/mqueue/*` 限制。

### 3.5 PTY

| API | 位置 | 说明 |
| --- | --- | --- |
| `class Pty` | `pty.hpp:60` | `Open(columns, rows, echo)`/`master_fd`/`slave_fd`/`slave_name`/`Resize`/`SetEcho`/`ReleaseMaster`/`ReleaseSlave`/`Read`/`Write`/`Capability`/`SetNonBlocking`/`EnableAsync`/`DisableAsync` |

`Open` 在 `pty.cpp:104`（`posix_openpt`+`grantpt`+`unlockpt`+`ptsname_r`），
`Resize` 在 `:150`（`TIOCSWINSZ`，内核发 `SIGWINCH`），`Read` 在 `:297`
（从端全关时 `EIO` 映射成 `kClosed`，`:327`/`:335`）。默认关闭回显，避免
「父进程写进去的命令被自己读回」。

### 3.6 Supervisor

| API | 位置 | 说明 |
| --- | --- | --- |
| `struct SupervisorOptions` | `supervisor.hpp:153` | `max_output_bytes`（默认 1 MiB）、`forward_stderr`、`tick`、`default_grace`、默认配额、可注入 `Clock`/`ProcessBackend` |
| `struct ManagedSpec` | `supervisor.hpp:177` | `ProcessOptions` + `deadline`/`grace`/条件/配额/`on_exit` |
| `struct ManagedSnapshot` | `supervisor.hpp:194` | 状态、触发原因、退出状态、用量、deadline |
| `class Supervisor` | `supervisor.hpp:236` | `Spawn`/`Add`/`NotifyHeartbeat`/`Stop`/`PollOnce`/`Run`/`Wait`/`Snapshot`/`CollectedStdout`/`CollectedStderr`/`stats` |
| `class Condition` + 四个工厂 | `supervisor.hpp:95-151` | 心跳丢失、RSS 超限、连续探测失败、输出匹配 |

### 3.7 ProcessPool

| API | 位置 | 说明 |
| --- | --- | --- |
| `struct PoolOptions` | `pool.hpp:62` | `workers`、`queue_capacity`、`block_when_full`/`submit_timeout`、`default_timeout`、`grace`、`max_output_bytes` |
| `struct PoolTask` / `PoolTaskResult` / `PoolStats` | `pool.hpp:84-118` | 任务输入/输出与统计（submitted/completed/rejected/cancelled/failed/timed_out） |
| `class ProcessPool` | `pool.hpp:120` | `Start`/`Submit`/`SubmitAndWait`/`Resize`/`Shutdown`/`Stats` |

### 3.8 IO 能力与显式异步

| API | 位置 | 说明 |
| --- | --- | --- |
| `enum class IoKind` | `io_capability.hpp:34` | Regular/Directory/FIFO/Socket/Char/Block/AnonInode/Unknown |
| `struct IoCapability` | `io_capability.hpp:90` | `pollable/can_nonblocking/native_blocking/async_supported` + 人话 `reason` |
| `IoCapability DetectIoCapability(int fd)` | `io_capability.cpp:46` | `fstat` 分类 + `isatty`；普通文件恒不可 poll/异步 |
| `class AsyncIoBinding` | `io_capability.hpp:131` | `Enable`（记录原阻塞状态、设 `O_NONBLOCK`、保存 manager）、`WaitReadable`/`WaitWritable`、`Disable` |

### 3.9 文件与目录

| API | 位置 | 说明 |
| --- | --- | --- |
| `enum class OsError` / `struct FileStatus` / `enum class OpenFlag` | `file.hpp:146`/`:197`/`:207` | 稳定错误码（含 `kIsSocket`/`kNotRegularFile`）、errno 结果、语义化 open 标志 |
| `class File` | `file.hpp:253` | `Open`/`Create`/`Adopt`/`Read`/`ReadAt`/`Write`/`WriteAt`/`WriteAll`/`Seek`/`Tell`/`Stat`/`Size`/`Sync`/`Truncate`/`Close`/`Capability`/`SetNonBlocking`/`EnableAsync` |
| `ReadFile` / `WriteFile` | `file.hpp:434`、`:440` | 整文件读写（`WriteAll` 保证写全） |
| `Getenv`/`Setenv`/`Unsetenv`/`Environ` | `file.hpp:460-490` | 一律返回值拷贝，避免 `getenv` 指针失效 |
| `struct DirEntry` | `dir.hpp:99` | name + `is_dir`/`is_regular`/`is_symlink`/`type_unknown` |
| `ReadDir`/`MkdirAll`/`RemoveAll` | `dir.hpp:120`/`:131`/`:150` | 不递归的目录列举、`mkdir -p`、两阶段删除（`lstat`，不跟随链接） |
| `Readlink`/`Symlink`/`SetTimes`/`StatPath` | `dir.hpp:160`/`:170`/`:192`/`:202` | 链接与元数据 |

`File` 的 socket 白名单校验在 `file.cpp:254-270`（`Open`）与 `:323-337`（`Adopt`）；
所有按路径入口对内嵌 NUL 返回 `kInvalidArgument`（`dir.hpp:94-95` 的约定，
实现见 `dir.cpp:118` 等）。

---

## 4. 设计

### 4.1 fork / exec：错误管道 + 裸 syscall

`RealBackend::Spawn`（`proc.cpp:809`）的步骤：

1. 父进程先把 stdio 模式、cwd、uid/gid、rlimits、亲和性等整理成 `ChildPlan`
   （`proc.cpp:900-1010`）；
2. 创建一条 `O_CLOEXEC` 的**错误管道**（`proc.cpp:964-969`）；
3. `fork`（`proc.cpp:1019`）；子进程只调用 `ChildRun`（`proc.cpp:604`），其中
   `dup2/dup3`、`setsid`、`setuid/setgid`、`setrlimit`、`execve` 全部用
   `::syscall(SYS_*)`——`fork` 之后不得分配内存/拿锁（`proc.cpp:6-20`）；
4. `execve` 成功后内核自动关闭错误管道写端，父进程读到 EOF 即成功；若读到
   4 字节 errno，父进程 `waitpid` 回收并返回 `kExecFailed`（`proc.cpp:1080-1100`），
   `ProcessOptions` 里任何一项设置失败都**不会静默忽略**（`proc.hpp:404-410`）；
5. 父侧 fd 设为非阻塞后交给 `FdChannel`（`proc.cpp:1102-1112`）。

线程注意：`Process` 非线程安全；`Wait` 与 `Kill` 可从不同线程并发（POSIX 语义）
但本对象不保证状态记录一致（`proc.hpp:563-574`）。

### 4.2 Run：两个死锁的消除

`Run`（`proc.cpp:1903`）把 stdin 写端、stdout/stderr 读端一起放进 `poll` 状态机：

- 「父进程写满 stdin、子进程不读」与「子进程写满 stdout、父进程等 wait」都不会死锁：
  每轮先排空两路输出，再用 `WNOHANG` 探测退出，退出后继续读到 EOF（`proc.hpp:700-718`）；
- 超时：`SIGTERM` → 等 `options.grace` → `SIGKILL`，回收后 `status.timed_out=true`，
  已收集输出照常返回；
- `max_output_bytes` 控制保留上限，超出部分丢弃并计数（`RunResult.dropped_output_bytes`）。

### 4.3 字节通道与 G2CF 帧

`MessageChannel` 把 framing 收进基类：子类只实现 `Pull`/`Write`，`Read`/`ReadFrame`/
`ReadLine` 共用接收缓冲（`channel.hpp:1-12`、`:179`）。`FdChannel::Pull`
（`channel.cpp:511`）用 `poll`+裸 `read`，`Write`（`:586`）处理 EINTR/部分写；
`Pipe::Create`（`:725`）与 `SocketPair::Create`（`:803`）默认 `O_CLOEXEC`。
帧写入/解析见 `channel.cpp:196-270`：CRC 覆盖 `version..payload`，magic 只用于
快速定位；坏 CRC 丢弃该帧而不是崩溃。

### 4.4 SPSC 环：两个原子与覆盖模式

`RingBuffer` 只依赖 `write_pos`/`read_pos` 两个 64 位原子（`shm.hpp:21-35`）：

- 生产者：写数据 → `write_pos.store(release)`；
- 消费者：`write_pos.load(acquire)` → 读数据 → `read_pos.store(release)`；
- 生产者复用空间前 `read_pos.load(acquire)`。

计数器单调递增、下标取模，`2^64` 回绕仍成立。默认满时「能写多少写多少」；
`overwrite=true` 时写方推进 `read_pos` 丢最旧数据，读方每次检查
`write_pos-read_pos > capacity` 并跳到有效窗口（`shm.cpp:292-347`、`:348-386`）。
覆盖模式是 **best-effort**：读方 memcpy 可能被覆盖，完整性靠上层 G2CF 的 CRC
（`shm.hpp:37-46`）。

### 4.5 消息队列：优先级与 epoll

- `Send` 用 `mq_timedsend`（相对毫秒转 `CLOCK_REALTIME` 绝对时间；`timeout==0`
  传当前时间，`kWaitForever` 传 `nullptr`，`mq.cpp:255-320`）；
- 队列满按策略：`kBlock` 等、`kTimeout` 立刻 `kQueueFull`、`kDropNewest` 丢新消息
  并计数（`mq.hpp:27-40`）；
- 异步：`mqd_t` 是 fd，可 `epoll_ctl`；`EnableAsync` 后空队列 `WaitReadable`、
  满队列 `WaitWritable`（`mq.cpp:475-495`、`io_capability.cpp:223`）。**不用
  `mq_notify`**：SIGEV_THREAD 建内核线程、每队列单次通知，不适合多 Fiber 等待。

### 4.6 PTY：EIO 与窗口变化

PTY 的 master 是双向流，`Process` 在 `use_pty=true` 时把 0/1/2 都接到从端
（`proc.hpp:469-472`）。从端全部关闭后 master `read` 返回 `EIO`，`Pty::Read`
把它映射成 `kClosed`（`pty.cpp:327`/`:335`），因此上层不会把正常退出当错误。
`Resize` 改 `TIOCSWINSZ`，内核向从端前台进程组发 `SIGWINCH`（`pty.cpp:150-160`）。

### 4.7 Supervisor：单线程 ppoll 事件循环

`Supervisor::Impl::PollOnce`（`supervisor.cpp:792`）每轮固定顺序：
**DrainEventFd → ApplyHeartbeats → DrainOutput → 查退出（TryWait）→ 判定
deadline/条件/配额 → 发 TERM/KILL → WaitForEvents（ppoll）**。
唤醒源是 `ppoll` 的 fd 集合：`timerfd`（最近 deadline/条件 tick）、`eventfd`
（`Stop()` 跨线程请求）、子进程 stdout/stderr（`supervisor.cpp:283`、`:780-790`）。
`Run()` 循环 `PollOnce(100ms)` 直到无活跃进程（`:859`）。

条件与配额在 `kRunning` 阶段求值（`:833-846`）；`Terminate` 幂等，重复 Stop
只在 grace 到期后补一次 SIGKILL。输出持续排空，避免子进程写满管道。

### 4.8 ProcessPool：缩扩容与异常补位

`WorkerLoop`（`pool.cpp:250`）的唤醒条件三选一：`stopping`、队列非空、**队列空且
本 worker 的 index 已超出目标**。缩容只更新 `target_workers`，多余 worker 在队列空
时自行退出并登记到 `retired`，由下一次 `Resize`/`Shutdown` 在锁外 join
（`pool.cpp:250-300`、`:479-500`）。扩容逐槽补线程，不按 `alive_workers` 补
（避免刚退出线程留下的空槽被漏掉，`pool.cpp:330-360`）。任务执行走 `Run`
（`pool.cpp:210`），spawn 失败/子进程被信号杀死只影响该任务，worker 继续接单
（R22 的「异常补位」）。

`Shutdown`：停止接单 → 唤醒全部 worker → 最多等 `grace` → 对未结束进程 `SIGKILL`
→ join（含 retired）→ 未开始任务计入 `cancelled`，`Stats().workers` 归零
（`pool.hpp:31-44`）。

### 4.9 IO 能力与 futex 的位置

`IoCapability` 回答三个问题：能否 `epoll_ctl`、`O_NONBLOCK` 是否有语义、默认是否
阻塞 M（`io_capability.hpp:1-60`）。默认策略是**原生阻塞**，`EnableAsync` 必须由
调用方显式发起；不支持时返回 `kUnsupported` + `reason`，**不静默降级**。
普通文件的能力在 `io_capability.cpp:58-68` 固定为不可 poll/不可异步。

`AsyncIoBinding::WaitReadable/WaitWritable` 只是把等待交给
`IOManager::wait_for(fd, ...)`（`io_capability.cpp:195-230`）；**Fiber 的 park/wake
与 futex 不在本模块**——那是 `sync`（`FutexMutex`/`ParkingCondition`）与
`IOManager` 的职责。`os` 里唯一出现 futex 的地方是监控子系统的 eBPF 采集器
（`src/os/monitor/futex_monitor.cpp`），它观察**其他进程**的 futex syscall，
与 IPC 通道实现无关。

### 4.10 热插拔：通道注册表与后端工厂

`ChannelRegistry`（`channel.hpp:518`、`channel.cpp:992`）在启动时注册四个内置后端
`pipe`/`socketpair`/`shm`/`mq`，对外只暴露 `Register`/`Unregister`/`Has`/`Backends`/
`Create`。上层持有 `ChannelSpec` + `MessageChannel`，**运行时可替换后端而不改调用方**
（R23）；注册/查询加锁，工厂在调用线程执行。这是本模块的「热插拔」；进程池的
`Resize` 是另一处运行时调整（§4.8）。监控子系统在 `Stop()` 后重建 eBPF 采集器
（`src/os/monitor/aggregator.cpp:414`）属于监控热恢复，不在本文范围。

---

## 5. 边界与陷阱

### 5.1 需要 root 的 eBPF

`os::monitor` 的 6 个 eBPF 采集器（net/syscall/sched/futex/tcp/fileio）需要 root
与内核 BTF；非 root 时降级为 `/proc` 的 4 个采集器（CPU/内存/磁盘/网络），
`ctest` 中 `go2cpp_os_monitor_ebpf_root` 等用例在非 root 下 SKIP。
`os` 的进程/IPC 子层**不需要 root**，除非调用方显式设置 uid/gid/亲和性/`oom_score_adj`
（`kPermissionDenied`）。

### 5.2 SIGPIPE

`Pipe`/`SocketPair`/`FdChannel` 写已关闭对端时底层是 `EPIPE`；若进程未忽略 SIGPIPE，
默认动作是**终止进程**。本模块不修改全局信号处理，因此**每个会写管道的程序都应在
`main` 开头调用 `IgnoreSigPipe()`**（`proc.hpp:800-807`）。

### 5.3 Fiber 里调用会阻塞 M

- `Process::Wait`/`Run`/`Supervisor::Run`/`ProcessPool::SubmitAndWait` 都是阻塞调用，
  **不要放进 Fiber**；放进去了整个 worker 线程停住（`proc.hpp:12-28`）。
- `File` 的 `read/write` 走裸 syscall，慢文件系统（NFS/FUSE）会卡 M；热路径应包
  `Scheduler::BlockingRegion`（`file.hpp:53-60` 的论证）。
- 要协程化：`FdChannel`/`Pty`/`MessageQueue` 支持 `EnableAsync(manager)`，但只能在
  该 manager 管理的 Fiber 中等待；普通线程调用返回 `kUnsupported`，**不会退回阻塞
  poll**（`io_capability.hpp:104-118`）。

### 5.4 普通文件不能异步

普通文件没有 `EAGAIN`，`epoll_ctl` 注册会 `EPERM`；`File::EnableAsync` 恒返回
`kUnsupported`（`file.hpp:390-405`、`io_capability.cpp:58-68`）。这不是缺陷，是
POSIX 语义。需要「读文件不阻塞 M」时用独立线程，不要给普通文件设 `O_NONBLOCK`
冒充异步。

### 5.5 共享内存没有事件通知

`ShmChannel` 读方是「短轮询 + 50µs 退避」（`shm.cpp:531-560`），不要放在低延迟
控制路径；控制信令用 pipe/mq，数据面用 shm（`shm.hpp:14-20`）。一端的 `unlink`
不会释放另一端已映射的内存；名字残留需显式 `Unlink`。

### 5.6 消息队列的内核上限

`max_messages` 受 `/proc/sys/fs/mqueue/msg_max` 限制、单条大小受 `msgsize_max`
限制；WSL 默认 10 条 / 8192 字节（`mq.hpp:42-46`）。进程退出后名字会留在
`/dev/mqueue`，测试/示例必须显式 `Unlink`。`kDropNewest` 只丢新消息，不会删除
队列里旧消息（多消费者场景不能替别人消费）。

### 5.7 进程池的两条并发约束

- `Start`/`Shutdown` 不应与自身并发；`Resize` 可与其他 `Submit`/`Stats` 并发，
  缩容**异步生效**，调用方轮询 `Stats().workers`（`pool.hpp:100-118`）；
- `Shutdown` 的阻塞时间上界是 `grace` + 一次系统调用；`grace` 内不退出的进程
  会被 `SIGKILL`，任务结果标记 `timed_out`/失败。

### 5.8 当前树的测试缺口

迁移时只带入了 `tests/os/monitor/`（9 个）与 `test_os_dir.cpp`/`test_os_file.cpp`/
`test_os_hook_conflict.cpp`；**R1–R31 的 IPC 测试装置（`test_os_proc_lifecycle`、
`test_os_channel`、`test_os_shm_mq`、`test_os_supervisor`、`test_os_pool`、
`test_os_pty`、`test_os_async` 等）没有进入本树**，因此当前构建的 `ctest` 只有
14 个（12 个 monitor + 2 个 bufio），不含 os IPC 专项。本文的 R1–R31 数据与
压测数字均标注为「迁移前基线」。

---

## 6. 测试

### 6.1 R1–R31 覆盖（迁移前基线）

来源：旧工作区 `/UserData/CodexWorkSpace/Go2Cpp-mcp-old-20261025/docs/os-final-report.md`
（2026-10-24，原文保留）。测试状态为当时的 17 个 os 专项，**当前树未携带这些装置**。

| 需求 | 状态 | 关键接口/测试 |
| --- | --- | --- |
| R1 进程对象/启动 | ✅ | `ProcessOptions`/`Process::Spawn`（argv 数组 exec、错误管道 errno） |
| R2 信息调整 | ✅ | nice/亲和性/sched/rlimit/OOM/cwd/env/umask |
| R3 状态查询 | ✅ | `/proc/<pid>/stat`（状态/RSS/CPU/启动时间/线程） |
| R4 等待/回收 | ✅ | `Wait/TryWait`（pidfd+EINTR）、析构兜底 |
| R5 信号 | ✅ | `Kill/Terminate/KillTree/Alive` |
| R6 stdio 三管道 | ✅ | 三条 pipe + `Run` + EOF/大输出 |
| R7 pipe/socketpair | ✅ | `Pipe/SocketPair` + 所有权 + CLOEXEC |
| R8 共享内存 | ✅ | `SharedMemory` + SPSC `RingBuffer` |
| R9 消息队列 | ✅ | `MessageQueue`（优先级/批量/ENOBUFS） |
| R10 帧抽象 | ✅ | `MessageChannel` + G2CF + 四后端 |
| R11 PTY | ✅ | `Pty` + use_pty + EIO→EOF + Resize |
| R12 背压 | ✅ | `FlowBuffer` 水位 + block/timeout/drop |
| R13 超时销毁 | ✅ | TERM→grace→KILL；timed_out + 输出 |
| R14 条件触发 | ✅ | 心跳/RSS/探测/输出匹配 |
| R15 Supervisor | ✅ | 单线程 ppoll+eventfd、Stop 回收全部 |
| R16 优雅停机 | ✅ | 状态机 + 幂等 |
| R17 统一错误 | ✅ | `ProcError` + `Result` |
| R18 日志/观测 | ✅ | `ProcEventSink`/`StderrLineSink` + 统计 |
| R19 RAII | ✅ | fd 关闭/析构兜底/Cleanup/fd 计数 |
| R20 调度器边界 | ✅ | 文档 + `native_fd` + EnableAsync + BlockingRegion |
| R21 可测试性 | ✅ | 注入 Backend/Clock + fake |
| R22 进程池 | ✅ | 有界队列/拒绝/缩扩容/补位 |
| R23 后端可扩展 | ✅ | `ChannelRegistry` |
| R24 安全 | ✅ | argv 数组/白名单/降权/CLOEXEC/close_range |
| R25 配额 | ✅ | `QuotaLimits` + `QuotaAction` |
| R26 文档/示例 | ✅ | `docs/os-process.md`、`os-io.md`（旧仓库） |
| R27 能力描述 | ✅ | `IoCapability` + `DetectIoCapability` |
| R28 显式切换 | ✅ | `EnableAsync/DisableAsync/SetNonBlocking` |
| R29 默认阻塞 | ✅ | 所有对象默认 `native_blocking=true` |
| R30 失败语义 | ✅ | `kUnsupported` + reason（regular file） |
| R31 TTY/mq 异步 | ✅ | Pty master / mq fd 接 epoll |

### 6.2 迁移前压测与正确性数据

| 项 | 结果（旧报告，2026-10-24） |
| --- | --- |
| 构建 | 0 warning / 0 error（`-Wall -Wextra -Wpedantic`） |
| ctest | 20/20（高负载 20 轮全绿；无负载 3 轮全绿） |
| os 专项 | 17 个（lifecycle/channel/shm_mq/pty/supervisor/pool/io_capability/async/stress/scenarios） |
| SPSC 环 | **29.5 GiB/s**（要求 ≥1 GiB/s） |
| 消息队列 | **935,166 msg/s**（要求 ≥1 万 msg/s） |
| spawn+reap | **1,059–1,068/s**（要求 ≥200/s） |
| 异步 socketpair | **~1.0 GiB/s** |
| TSan | os 全模块 0 报告（修复 `File::m_last_error`、mq/pty `m_async` 两处真 race） |

> 数字口径：单机 WSL2、Release、同一份 2026-10-24 构建；任务验收阈值写在
> 旧报告的括号内。SPSC 29.5 GiB/s 与 mq 935k msg/s 是**迁移前环境**的实测，
> 当前树无法复跑（缺测试装置），引用时需带这条限定。

### 6.3 当前树可运行的测试

```bash
# 集成构建（MCP + os monitor）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /UserData/build/go2cpp-mcp-integrated \
      -DGO2CPP_WITH_MCP=ON -DGO2CPP_BUILD_TESTS=OFF
cmake --build /UserData/build/go2cpp-mcp-integrated -j8
ctest --test-dir /UserData/build/go2cpp-mcp-integrated -N    # 14 个
ctest --test-dir /UserData/build/go2cpp-mcp-integrated --output-on-failure
```

14 个用例：`go2cpp_os_monitor_collectors`、`go2cpp_os_monitor_interfaces`、
`go2cpp_os_monitor_ebpf`、`go2cpp_os_monitor_e2e`、`go2cpp_os_monitor_stress`、
`go2cpp_os_kernel_monitor`、`go2cpp_os_tcp_io_monitor`、
`go2cpp_os_monitor_ebpf_nonroot`、`go2cpp_os_monitor_overhead`、
`go2cpp_os_monitor_ebpf_root`、`go2cpp_os_kernel_monitor_root`、
`go2cpp_os_tcp_io_monitor_root`、`go2cpp_bufio_test`、`go2cpp_bufio_stress`
（`*_root` 用例在非 root 下 SKIP）。

**本次构建阻塞（如实记录）**：在当前 HEAD 上重新配置后 `cmake --build` 失败，
`src/os/dir.cpp` 的 7 处 `detail::ContainsNul(...)` 未声明
（`dir.cpp:118`、`:196`、`:267`、`:376`、`:412`、`:432`、`:463`），
另有 `libgo2cpp_network` 未定义 `go2cpp::hook::mark_system_nonblocking` 的链接错误。
这是 HEAD 的既有缺陷，**不是本文改动引入**；按「不改代码」约束，文档按源码现状
描述，测试数据引用迁移前基线。

### 6.4 复现单项

```bash
# 文件/目录（如果单独构建了 runtime 测试）
GO2CPP_TEST_FILTER=os ./build/go2cpp_tests
# 监控：任一用例
ctest --test-dir /UserData/build/go2cpp-mcp-integrated -R go2cpp_os_monitor_e2e -V
```

---

## 7. 旧文档合并与相关文档

| 旧文档 | 处理 | 说明 |
| --- | --- | --- |
| `/UserData/CodexWorkSpace/Go2Cpp-mcp-old-20261025/docs/os-final-report.md` | 有效内容并入本文 §1.1、§5.8、§6.1、§6.2；原文保留（不在本树） | R1–R31、压测数据、4 个 bug 修复、TSan 结论 |
| `docs/module-audit-2026-10-20.md:57` | 数字并入 §6.2 | OS 1 GiB 写 473.6 MiB/s、读 6868 MiB/s；10 万文件 create 3.4s/list 28ms；并发 O_APPEND 4000 条无撕裂 |
| `docs/modules.md:67-87` | 接入状态并入 §2 | `go2cpp_os` 已在 `GO2CPP_MODULE_TARGETS` 与 `go2cpp::runtime` |
| `docs/os-process.md`、`docs/os-io.md` | **本树不存在**（旧仓库文档）；相关语义已并入 §4.1–§4.6、§5.2–§5.5 | 头文件注释仍引用这两个名字，阅读时以本文为准 |

相关文档：

- [docs/modules.md](../modules.md)：模块目录 → CMake target → 依赖边界（os 的接入状态）；
- [docs/module-audit-2026-10-20.md](../module-audit-2026-10-20.md)：OS 模块的实测与 D9/D3 审计项；
- [docs/boundary_audit.md](../boundary_audit.md)：并发/生命周期边界；
- `src/os/config.cpp:60-80`：`os.proc.*` 配置项（grace/timeout/pool 等）。

---

## 8. 变更记录

| 日期 | 变更 |
| --- | --- |
| 2026-10-26 | 首版：合并旧 `os-final-report.md` 的 R1–R31 与压测数据，补全 `include/go2cpp/os/**`/`src/os/**` 的 API 与设计（带 `file:line`）；记录当前树 IPC 测试缺失与构建阻塞 |
