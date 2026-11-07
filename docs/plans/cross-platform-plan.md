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
