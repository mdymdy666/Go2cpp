# 跨平台支持调研与标注清单

> 状态：**只标注，不实现**（Go2Cpp 当前仅 Linux/x86_64；跨平台适配列入 backlog）。
> 参考：JinShanProject 已做部分跨平台（Windows 主控/被控，Win32 API + 条件编译）。

## 一、平台依赖分类

| 类别 | 依赖 | 影响范围 | 跨平台替代（Linux 以外） |
|---|---|---|---|
| **汇编上下文** | `src/concurrency/context_x64.S`（x86_64） | Fiber 切换（核心） | ARM64/RISC-V 需新汇编；Windows x64 可用同一调用约定（ABI 不同需改） |
| **事件循环** | `epoll_create1/epoll_ctl/epoll_wait` | io/IOManager/hook | Windows：IOCP；macOS：kqueue |
| **线程阻塞** | `futex`（FUTEX_WAIT/WAKE） | Mutex/CV/Channel/WaitNode/parking | Windows：`WaitOnAddress`/`WakeByAddress`；macOS：`__ulock_wait/__ulock_wake` |
| **内存映射** | `mmap/mprotect/munmap` | Fiber 栈（arena/guard page） | Windows：`VirtualAlloc`/`VirtualProtect`；macOS：`mmap`（同 POSIX） |
| **批量 IO** | `sendmmsg/recvmmsg` | socket/bufio/peer_hub | Windows：`WSASendMsg`/`WSARecvMsg` 或循环；macOS：`sendmsg` 循环 |
| **零拷贝** | `sendfile` | socket/bufio | Windows：`TransmitFile`；macOS：`sendfile`（不同签名） |
| **时间** | `clock_gettime(CLOCK_MONOTONIC)` | timer/fiber/scheduler | Windows：`QueryPerformanceCounter`、macOS：`mach_absolute_time` |
| **进程/信号** | `pthread_sigmask`、`SIGPIPE` | socket send | Windows：无 SIGPIPE（用 MSG_NOSIGNAL 替代） |
| **线程局部** | `thread_local`（已 initial-exec） | 全部 | Windows：`__declspec(thread)`；macOS：`thread_local`（支持） |
| **路径** | `/proc/self/...`、`/tmp` | os/monitor/debug | Windows：`GetProcessInformation`、`GetTempPath` |

## 二、文件级清单（按优先级）

### P0（核心，不移植等于不可用）

| 文件 | 依赖 | 说明 |
|---|---|---|
| `src/concurrency/context_x64.S` | x86_64 汇编 | **ARM64/RISC-V 必须新写**（Fiber 切换） |
| `src/concurrency/fiber.cpp` | mmap/mprotect + __linux__ 宏 | 栈 arena/guard page |
| `src/concurrency/scheduler.cpp` | futex/mmap/线程 | 调度器 park/wake |
| `src/concurrency/sync.cpp` | futex | Mutex/CV/WaitGroup |
| `src/concurrency/parking_condition.cpp` | futex | WaitNode |
| `src/concurrency/channel.cpp` | futex/mmap | Channel 等待 |
| `src/network/io.cpp` | epoll/eventfd | IOManager |
| `src/network/socket.cpp` | sendmmsg/recvmmsg/sendfile/epoll | Socket |

### P1（功能，缺了可用但性能降）

| 文件 | 依赖 | 说明 |
|---|---|---|
| `src/concurrency/timer.cpp` | epoll/timerfd（或 timer wheel） | 定时器 |
| `src/network/hook.cpp` | epoll + syscall hook | 网络 hook |
| `src/bento/net/peer_hub.cpp` | sendmmsg | 批量发送 |
| `include/go2cpp/network/bufio_socket.hpp` | sendmmsg/sendfile | 适配层 |

### P2（辅助）

| 文件 | 依赖 | 说明 |
|---|---|---|
| `src/logging/log.cpp` | 已有 `_WIN32` 分支 | 日志（部分已跨平台） |
| `src/os/monitor/**` | /proc + syscall 名 | 监控（Linux 专用） |
| `src/debug/**` | /proc/self/maps | 调试（Linux 专用） |

## 三、关键难点

| 难点 | 说明 | 工作量 |
|---|---|---|
| **x64 汇编** | ARM64/RISC-V 需新写上下文切换（寄存器保存/恢复/栈切换） | 中（每架构 ~200 行） |
| **epoll → IOCP** | 异步模型不同（IOCP 是完成端口，epoll 是就绪） | **大**（IOManager 重写） |
| **futex → WaitOnAddress** | 语义相似但 API 不同 | 中（封装层） |
| **mmap → VirtualAlloc** | 栈 arena/guard page | 小（封装） |
| **sendmmsg → WSASendMsg** | 批量语义不同（循环/消息数组） | 中 |

## 四、JinShanProject 的参考

```
JinShan 已做：
  - Windows 主控/被控（Win32 API：窗口/对话框/COM/IRDPSRAPI）
  - 条件编译（_WIN32 分支）
  - 少依赖（无 Qt）
可借鉴：
  - Win32 API 封装层（跨平台抽象）
  - 条件编译组织（每平台一个 .cpp）
  - 构建（CMake 平台判断）
未覆盖：
  - 并发/网络（JinShan 的 Go 中转是 Go 语言——天然跨平台）
```

## 五、建议路径（若做跨平台）

```
阶段 1（Linux 内多架构）：ARM64/RISC-V 汇编 + 交叉编译（1-2 周）
阶段 2（Windows）：IOCP + WaitOnAddress + VirtualAlloc 封装（1-2 月）
阶段 3（macOS）：kqueue + ulock（1 月）
```

## 六、标注约定

代码内统一前缀（便于 grep）：

```cpp
// [PORTABILITY] Linux-only: epoll_create1
// [PORTABILITY] x86_64-only: 上下文切换
// TODO(portability): Windows 用 IOCP 替代
```

## 七、现状结论

> **Go2Cpp 当前 = Linux/x86_64 专用**（并发/IO 全依赖 epoll/futex/mmap/x64 汇编）。
> **跨平台不是「改几个宏」**——需要「IOCP/WAITONADDRESS/VIRTUALALLOC 封装层 + ARM64/RISC-V 汇编」。
> **JinShan 的跨平台经验（Win32 封装 + 条件编译）可借鉴**，但它的并发/网络在 Go 侧（天然跨平台），不直接适用于 C++ 基座。
