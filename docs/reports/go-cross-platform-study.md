# Go 跨平台实现调研（源码级）

> 目的：回应「跨平台其实挺难，看看 Go 怎么处理的」。
> 源码：`/usr/local/go/src`（本机已装）。
> 对照本项目：48 类系统调用、40 需适配（`docs/reports/cross-platform-syscall-audit.md`）。

---

## §1 Go 的分层（平台差异放在哪）

| 层 | 职责 | 平台差异怎么处理 |
|---|---|---|
| **`runtime/`** | 线程、时间、事件、netpoll 接口 | `os_windows.go` / `os_linux.go` **各自实现同一组函数** |
| **`internal/poll`** | fd 的等待/就绪抽象 | `fd_windows.go`（IOCP）/ `fd_unix.go`（epoll） |
| **`syscall/`** | 系统调用薄封装 + 错误码 | `syscall_windows.go`（`Errno` = `GetLastError`）|
| **`os/`** | 文件/进程/环境的高层封装 | `file_windows.go`（rename/unlink 约束） |

**关键**：**接口在顶层，实现在底部**——上层代码**完全不知道**下面是 epoll 还是 IOCP。

---

## §2 六个问题（源码位置 + 对 Go2Cpp 的启示）

### Q1：Go 在 Windows 上怎么实现「同步代码 + 高并发 IO」？

**Go 不需要 hook**——它的 goroutine 是**编译器插桩**：

| 机制 | 位置 | 作用 |
|---|---|---|
| `morestack` | `runtime/asm_amd64.s` | 每个函数入口检查栈空间，不够就扩容 |
| 抢占点 | `runtime/proc.go`（`gosched`/`asyncPreempt`） | 10ms 级抢占，不让一个 goroutine 独占 |
| `entersyscall` / `exitsyscall` | `runtime/proc.go` | 系统调用阻塞时**把 P 让出**给其他 goroutine |

**关键结论**：
> Go 的「同步代码自动并发」是**语言运行时 + 编译器**共同实现的，**不需要拦截系统调用**。
> **Go2Cpp 用 `LD_PRELOAD` hook 在用户层模拟这个能力**——这在 Linux 可行，**Windows 没有 `LD_PRELOAD` 等价物**（最接近的是 API hooking / Detours，但需要**每进程注入 + 函数级 trampoline**，工程量和稳定性都不可接受）。

**启示**：**Windows 上必须放弃 hook 方案**，改为「显式异步 API 或白名单」。

### Q2：Go 的 netpoll 怎么统一 Linux/Windows？

**接口只有 5 个函数**（`runtime/netpoll.go:19-39`）：

```go
netpollinit()                    // 初始化
netpollopen(fd, pd) int32        // 注册 fd
netpollclose(fd) int32           // 注销
netpoll(delta) (gList, int32)    // 等待就绪，返回可运行的 goroutine 列表
netpollBreak()                   // 打断等待（用于唤醒）
```

**Linux 实现**：`epoll_create1/epoll_ctl/epoll_wait`
**Windows 实现**（`netpoll_windows.go:102-203`）：
- `CreateIoCompletionPort`（第 102 行）建完成端口；
- `netpollopen` 用 `CreateIoCompletionPort(fd, iocphandle, key, 0)` **把 socket 绑定到完成端口**；
- `netpoll` 用 `GetQueuedCompletionStatusEx`（第 197 行）**批量取完成事件**。

**关键洞察**：
> **IOCP 的「完成语义」被适配成「就绪语义」**——`netpoll` 的**契约是「返回哪些 fd 可读/可写」**，Windows 实现里通过 `overlapped` 结构**把完成事件映射回 fd**，上层无感知。
> **所以：接口设计成「就绪形状」是可以的**，关键是**Windows 实现内部要做映射**（而不是让上层感知 IOCP）。

**启示**：Go2Cpp 的 `IOBackend` 接口**保持就绪形态**即可，Windows 后端**内部**用 IOCP（或短期 `WSAPoll`）实现，**上层不用改**。

### Q3：Go 怎么处理 `fork` 缺失？

- `os.StartProcess`（`src/os/exec.go`）→ Linux 用 `fork+exec`，**Windows 用 `CreateProcess`**（`src/syscall/exec_windows.go`）；
- **`syscall.ForkExec` 只在 unix 有**（`exec_unix.go`），Windows 上不存在。

**启示**：Go2Cpp 如果有 `fork` 用法（`os::pool` 的进程池？）→ **Windows 上用 `CreateProcess`**；语义差异（COW、fd 继承）**无法模拟**，必须改设计。

### Q4：Go 怎么替代 `/proc`？

- 进程信息：`NtQuerySystemInformation`（`src/internal/syscall/windows/`）；
- 进程句柄：`OpenProcess`；
- 启动时间/CPU：`GetProcessTimes`。

**启示**：Go2Cpp 的 `os::monitor` / `debug` 大量依赖 `/proc`（`/proc/stat`、`/proc/<pid>/status`、`/proc/<pid>/fd`）→ **Windows 上要么用 `NtQuerySystemInformation` 重写，要么标记 Linux-only**。

### Q5：Go 怎么处理 `rename` 覆盖 / `unlink` 打开中文件？

| 操作 | Go 的实现 | Windows 约束 |
|---|---|---|
| `os.Rename` | `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` | **必须用 `MoveFileEx`**，`MoveFile` 不能覆盖 |
| `os.Remove` | `DeleteFile` | **打开中的文件删不掉**——Go 打开文件时带 `FILE_SHARE_DELETE` 才允许 |

**启示**：
- WAL/SST 的「写临时文件 → rename 覆盖」→ **用 `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`**；
- 日志轮转的「unlink 旧文件」→ **打开时加 `FILE_SHARE_DELETE`**，否则删不掉。

### Q6：平台差异放在哪一层？

**三层分工**（不是一层全包）：
1. **`runtime`**：线程/时间/事件/netpoll（最底层）；
2. **`internal/poll`**：fd 等待（epoll/IOCP）；
3. **`syscall`**：错误码/系统调用（`Errno` = `GetLastError`）；
4. **`os`**：高层封装（文件/进程，处理平台约束）。

**启示**：Go2Cpp 应该建 **`include/go2cpp/platform/` + `src/platform/`**（3–4 个文件），**禁止散落的 `#if defined(_WIN32)`**。

---

## §3 Go2Cpp 的 hook 层在 Windows 上的 3 个候选方案

| 方案 | 内容 | 改动量 | 风险 | 功能完整度 |
|---|---|---|---|---|
| **A. hook 白名单 + 显式异步** | 只 hook **可安全模拟**的（`read`/`write`/`poll`/`nanosleep`）；关键路径提供 `AsyncRead`/`AsyncWrite`；`fork`/`/proc`/`mmap` **不 hook** | 中 | 中 | 中（覆盖 80% 场景） |
| **B. 编译期插桩** | 自己实现 `morestack` 式检查点（Clang plugin / 代码生成） | **大** | **高** | 高（但工程风险大） |
| **C. 只支持异步 API** | Windows 上**不模拟同步阻塞**，用户必须用 `co_await`/回调 | 小 | 低 | 低（所有代码要重写） |

**代价对比**：
- A：改 **hook 层 + 若干 API**，**用户代码基本不动**（阻塞调用仍能工作，只是 Windows 上部分场景退化）；
- B：改 **编译流程**，**用户代码不动**，但**构建工具链要重做**（且 MSVC 的插桩能力弱于 Clang）；
- C：**用户代码全部重写**，但**运行时最简单**。

---

## §4 建议（我的判断）

**A 为主 + 降级兜底**：

1. **hook 白名单**（Windows）：
   - ✅ hook：`read`/`write`/`recv`/`send`/`poll`/`select`/`nanosleep`（这些**能安全用 `WSAPoll` + 事件模拟**）；
   - ❌ 不 hook：`fork`/`/proc`/`mmap`/`signal`（**语义无法模拟**，直接报错或降级）；
2. **显式异步 API**：给关键路径加 `AsyncRead`/`AsyncWrite`（Windows 上用 IOCP，Linux 上用 epoll）；
3. **平台层**：建 `platform/` 三件（`WakeEvent` / `LastError` / `SteadyClock`）——**所有适配共用的地基**；
4. **不追求 100% 对等**：`os::monitor`/`debug` 的 `/proc` 功能在 Windows 上**标记 Linux-only**（在文档里写清楚）。

**为什么不选 B**：编译器插桩需要改构建工具链（Clang plugin），**工程风险 > 收益**——Go2Cpp 的目标是「可移植的 Go 风格并发」，不是「100% 复刻 Go 运行时」。

**为什么不选 C**：用户代码要全重写，**等于放弃现有生态**。

---

## §5 结论（一句话）

> **Go 的核心优势是「编译器 + 运行时」一体化，Go2Cpp 的 hook 是在用户层模拟——所以 Windows 上注定做不到 100% 对等。正确策略是：保留就绪型 IOBackend 接口（学 Go 的 netpoll 抽象）、用白名单 hook 覆盖主流场景、把无法模拟的能力明确标记为 Linux-only。**
