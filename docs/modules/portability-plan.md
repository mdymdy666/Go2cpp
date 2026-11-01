# Go2Cpp 跨平台适配方案（Windows x64 优先 / macOS 与 ARM64 次之）

> 状态：**只读调研 + 方案（未实施）**。本文不含任何实现代码，只给结论、接口草案与分阶段计划。
> 基线：`/UserData/CodexWorkSpace/Go2cpp` @ `1dcd4bd`（2026-11-01），`src/` + `include/` 共 **196,118 行**。
> 前置阅读：`docs/modules/portability.md`（既有标注清单，2026-10-30）。
> 同源参考：`Go2Cpp-product/docs/cross-platform-plan.md`（2026-10-26，基线 `603c51c`）与其 `cross-platform-status.md`。

---

## 一、结论摘要

1. **当前是 Linux/x86_64 单平台，且门禁在 CMake 配置阶段硬拒**：`cmake/Go2CppModules.cmake:6-8` 对非 Linux / 非 x86_64 直接 `FATAL_ERROR`，`CMakeLists.txt:19-21` 只在 Linux x86_64 下 `enable_language(ASM)`。门禁不解除，任何 Windows/macOS/ARM64 尝试连配置都过不去。
2. **平台相关面分布极不均匀，但比预想的集中**：真正不可移植的是三块——**Fiber 上下文（汇编 + mmap + SIGSEGV 增长）**、**IO 多路复用后端（epoll）**、**Hook interposer（Linux 专属，无 Windows 等价物）**。其余大量文件只是调用 POSIX 通用 API（`socket/read/write/pthread`），属于「换个后端就行」。
3. **IO 抽象已经就绪，是本次最大利好**：`io::IOBackend`（`include/go2cpp/network/io_backend.hpp`）已是完整的虚接口 + 工厂注册表，且 `src/network/io.cpp` **已 100% 经由 `m_backend->` 调用，全文没有一处直接 `::epoll_*`**。所以「epoll → IOCP」不是重写 IOManager，而是**新增一个后端实现**——但完成模型与就绪模型的语义鸿沟仍是本项目最大风险（见 §六）。
4. **并发同步已有非 Linux 回退，且已被 MSVC 编译验证过**：`futex_mutex.hpp` 有 `std::mutex + condition_variable` 回退（并有 Windows 单测 2000 轮通过的先例）；`sync.cpp:653-666`、`parking_condition.cpp:511-526`、`channel.cpp:126-128` 都有 `#else` 分支。产品分支的 `go2cpp_windows_sync_compile` OBJECT target 证明这段能在 MSVC 下编译。**这意味着阶段 2 的 futex 工作不是「从零实现」，而是「把 CV 回退升级为 WaitOnAddress 快路径」**。
5. **Fiber 是唯一「零回退」的模块**：`fiber.cpp:39` 无条件 `#include <sys/mman.h>`，`:436/:686` 直接用 `mmap/mprotect`，`:469` 用 `sigaction(SIGSEGV)` 做栈增长，`:34-37` 的 growable stack 只在 Linux x86_64 启用，Boost.Context 后备**已被删除**（`Go2CppModules.cmake:8` 的注释明说）。Windows/macOS 上必须新写栈分配后端，或先关 growable 只保留固定栈。
6. **命名有冲突，必须先决策**：`include/go2cpp/os/**` 现有 22 个公共头是 **Go `os` 包的镜像**（`os.hpp` 头部明写「**不**做平台抽象（POSIX only）」）。用户要求把 OSAL 放 `include/go2cpp/os/**` 会与既有语义正面冲突。本文推荐 `include/go2cpp/platform/**`（与产品分支既有资产对齐，可省 8-12 人天），备选方案见 §四.1。
7. **工作量：62-89 人天（约 13-18 周单人）**，关键路径是「门禁 → OSAL 接口 → Windows IOCP 后端」。ARM64 汇编与 Windows 后端可并行（互不阻塞）。
8. **最大教训来自产品分支**：`Go2Cpp-product` 已经做过一次 M0-M3，产出了完整的 `go2cpp_platform` 库（Linux 0 warning、Windows 单测 1/1 通过），**但它没有被 runtime 使用**——P3-P6 未做，主工作树的 `go2cpp_runtime` 至今仍是 POSIX-only。本次方案必须把「接入并被真实模块使用」写进验收，否则会重演「新增一个没人用的库」。

---

## 二、现状盘点

### 2.1 硬门禁（不解除则一切免谈）

| 位置 | 内容 | 影响 |
|---|---|---|
| `cmake/Go2CppModules.cmake:6-8` | `if (NOT CMAKE_SYSTEM_NAME STREQUAL Linux OR ...)` → `FATAL_ERROR "Go2Cpp fiber backend requires Linux x86_64 (context_x64.S)"` | 非 Linux x86_64 配置阶段直接失败 |
| `CMakeLists.txt:19-21` | 仅 Linux x86_64 下 `enable_language(ASM)` | 其它平台不启用汇编语言，`.S` 无法编译 |
| `cmake/Go2CppModules.cmake:35` | `target_sources(go2cpp_fiber PRIVATE .../context_x64.S)` **无条件** | Windows 上即使解禁也会因无法汇编而失败 |
| `cmake/Go2CppModules.cmake:168-191` | `-ftls-model=initial-exec` 整 TU（GCC/Clang 专属） | MSVC 无此选项，需换成 `__declspec(thread)` 语义 |
| `cmake/Go2CppMonitor.cmake:24` | monitor 模块限定 `x86_64\|amd64` | P2 辅助模块，可最后处理 |

### 2.2 平台依赖量化（扫描口径：`src/` + `include/`，含注释/文档串的文本命中）

| 依赖 | 命中文件 | 命中次数 | 主要位置 | 跨平台替代 |
|---|---:|---:|---|---|
| `__linux__` 宏 | 14 | 49 | `sync.cpp`(10)、`parking_condition.cpp`(8)、`debug_event_ring.cpp`(5)、`futex_mutex.hpp`(5) | 平台宏收口到 OSAL |
| 裸 `syscall(SYS_*)` | 12 | **108** | `proc.cpp`(35)、`file.cpp`(15)、`raw_io.cpp`(13)、`hook.cpp`(10) | 平台封装，Windows 用 Win32 |
| `eventfd` | — | 64 | `io_backend.cpp`、`scheduler.cpp`、`timer.cpp` | Windows：`PostQueuedCompletionStatus` / 手动重置事件 |
| `timerfd` | — | 21 | 定时器兜底路径 | Windows：`CreateWaitableTimer` 或纯用户态 deadline 队列 |
| `signalfd` | — | 14 | 信号转 fd | Windows 无等价，禁用 |
| `io_uring` | — | 16 | 注释中的预留后端 | 非本期目标 |
| `epoll_pwait` / `accept4` / `pipe2` / `memfd` | — | 9 / 16 / 1 / 3 | `io.cpp`、`socket.cpp`、`os/` | 平台层逐一替换 |
| `sendmmsg` / `recvmmsg` | 4 | 见 §2.4 | `socket.cpp:889/958`、`peer_hub.cpp` | `WSASendMsg` / `WSARecvMsg` |
| `sendfile` | 2 | — | `socket.cpp:18/1003` | `TransmitFile`（Win）/ `sendfile`（mac，签名不同） |
| `mmap/mprotect/munmap` | 2 | 20+ | `fiber.cpp`、`os/shm.cpp` | `VirtualAlloc`/`VirtualProtect`（Win） |
| `pthread_setaffinity_np` | 1 | 1 | `scheduler.cpp:5261` | `SetThreadAffinityMask` 或 no-op |
| `_WIN32` 已有分支 | 2 | 约 50 | `logging/log.cpp`、`path/filepath.cpp`（**完整盘符/UNC 实现**） | 可直接复用 |
| `__APPLE__`/`__MACH__` | 3 | 少量 | debug/cli 分支 | 参考 |

**公共头 POSIX 污染（阶段 2 必须先收口，否则 `#ifdef` 会扩散到业务层）**：

```
include/go2cpp/network/address.hpp   // sys/socket.h, netinet/in.h, sys/un.h
include/go2cpp/network/socket.hpp    // sys/socket.h:24, sys/types.h:25, sys/uio.h:26
include/go2cpp/network/hook.hpp
include/go2cpp/network/byte_array.hpp
include/go2cpp/bento/net/peer_hub.hpp
include/go2cpp/bento/net/link.hpp
include/go2cpp/bento/net/tcp_proxy.hpp
include/go2cpp/p2p/endpoint.hpp
```

共 8 个。`socket.hpp` 里连 `enum SocketType { Tcp = SOCK_STREAM, ... }` 都直接用了 POSIX 常量，`SocketResult::transferred` 是 `ssize_t`——这两个类型在 Windows 上都不存在。

### 2.3 逐项明细

#### A. 上下文切换（唯一零回退项）

| 文件 | 行 | 事实 |
|---|---|---|
| `src/concurrency/context_x64.S` | 全文 83 行 | x86_64 SysV：保存 `rbp/rbx/r12-r15`，栈上布局 56 字节；`go2cpp_context_make` 按 SysV 布置参数寄存器 |
| `src/concurrency/context_x64.S:12` | `#if defined(__linux__) && defined(__x86_64__)` | 非目标平台编译为空文件 |
| `src/concurrency/context_backend.cpp:28-58` | `MakeContext`/`JumpContext` | 非 Linux x64 返回 `nullptr` / `{}`，Fiber 创建「安全失败」 |
| `src/concurrency/fiber.cpp:34-37` | `GO2CPP_FIBER_GROWABLE_STACK` | 仅 Linux x86_64 启用，其它平台固定栈 |
| `src/concurrency/fiber.cpp:436` | `mmap(PROT_READ\|PROT_WRITE, MAP_PRIVATE\|MAP_ANONYMOUS)` | 信号备用栈 |
| `src/concurrency/fiber.cpp:686` | `mmap(PROT_NONE, MAP_NORESERVE)` | 栈槽 arena 预留（8 MiB/槽，一次映射 N 槽） |
| `src/concurrency/fiber.cpp:469-475` | `sigaction(SIGSEGV, ...)` | 栈增长靠 SIGSEGV handler + `mprotect` 提交顶部窗口 |

**判断**：x64 汇编**不能跨 ABI 复用**——Win64 有 32 字节 shadow space、非易失寄存器集合不同（`rbx/rbp/rdi/rsi/rsp/r12-r15` + `xmm6-xmm15`），SysV 版本漏存 `rdi/rsi` 和 `xmm6-15`。但**寄存器保存/恢复的骨架可以照抄**，改的是寄存器清单与调用约定。

#### B. 事件循环

| 文件 | 行 | 事实 |
|---|---|---|
| `include/go2cpp/network/io_backend.hpp` | 26-70 | `IOBackend`：`initialize/supports_zero_interest/supports_one_shot/add_or_modify/remove/wait/wake/drain_wake/shutdown` |
| `src/network/io_backend.cpp` | 40-156 | `EpollBackend`：`epoll_create1` + `eventfd` 唤醒（token=0 保留）+ `EPOLLET` 边缘触发 |
| `src/network/io_backend.cpp` | 218-224 | `DefaultBackendName()` 在非 Linux 返回空串 |
| `src/network/io.cpp` | 686/1740/1900 | 只调 `m_backend->initialize / wait / wake / drain_wake`，**无直接 epoll** |
| `src/network/io.cpp` | 1305/1348/1376 | `remove`、`supports_zero_interest`、`add_or_modify` |
| `src/concurrency/timer.cpp` | 全文 | 后台线程 + `std::multimap` + `std::condition_variable`，**不依赖 timerfd**；IOManager 的 deadline 队列才复用 `epoll_wait` 超时 |

**判断**：IOManager 的状态机（fd 槽位、代际 token、interest=0 停用注册、ONESHOT 重布防）全部与后端解耦，Windows 后端只要满足「token 原样回传 + wait 返回就绪位 + shutdown 后不再唤醒」三条契约即可复用。`timer.cpp` 天然可移植。

#### C. 线程阻塞 / futex

| 文件 | 行 | 事实 |
|---|---|---|
| `src/concurrency/sync.cpp:90/100` | `WaitFutex`/`WakeFutex` | 裸 `syscall(SYS_futex, FUTEX_WAIT_PRIVATE/FUTEX_WAKE_PRIVATE)`，两个函数是全部 futex 调用的收口 |
| `src/concurrency/sync.cpp:653-666` | `#else` 分支 | `std::unique_lock<std::mutex>` + `m_native_condition.wait(pred)` |
| `src/concurrency/sync.cpp:761-763` | 成员 | `#if !defined(__linux__)` 时每 WaitNode 多 88 字节的 `std::mutex` + `condition_variable` |
| `src/concurrency/parking_condition.cpp:71-183` | `futex_waitv` | Linux 5.16+ 才有的双字等待，缺失时逐节点 `FUTEX_WAKE` |
| `src/concurrency/parking_condition.cpp:511-526` | `#else` | `FutexMutex` + `m_native_cv.wait_until` |
| `src/concurrency/channel.cpp:151/164` | futex 封装 | 与 sync 同构 |
| `include/go2cpp/concurrency/core/futex_mutex.hpp:117-121, 220-224` | 回退实现 | `std::mutex` 保护状态 + `condition_variable` 阻塞；已有 Windows 单测先例 |

**判断**：futex 的**语义**（比较后睡眠、跨线程唤醒、代际字、等待者计数）可以 1:1 映射到 Windows `WaitOnAddress/WakeByAddressSingle/All`——参数模型几乎相同（地址 + 比较值 + 超时）。`futex_waitv`（双字等待）**没有对应 API**，Windows 上退化为「广播唤醒 + 重判」，会在高竞争下多唤几次，属性能退化不属正确性缺失（`parking_condition.cpp` 本身就有「编译期无 `futex_waitv` 时逐节点唤醒」的降级路径，可复用）。

#### D. Socket / 批量 IO / 零拷贝

| 文件 | 行 | 事实 |
|---|---|---|
| `src/network/socket.cpp:18` | `#include <sys/sendfile.h>` | 零拷贝发送 |
| `src/network/socket.cpp:702` | `::sendmsg(fd, msg, flags \| MSG_NOSIGNAL)` | 分散写 |
| `src/network/socket.cpp:889` | `::recvmmsg(...)` | 批量收 |
| `src/network/socket.cpp:958` | `::sendmmsg(...)` | 批量发 |
| `src/network/socket.cpp:1003` | `::sendfile(fd, in_fd, offset, count - total)` | 文件 → socket |
| `src/network/socket.cpp:41-43`（注释） | `sendfile` 无 `MSG_NOSIGNAL` | 对端关闭后继续发会收到 SIGPIPE |

**判断**：Windows 的 `WSASendMsg/WSARecvMsg` 需要 `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER, WSAID_WSASENDMSG)` 取函数指针（Vista+），语义是「一次一条消息」——**没有 recvmmsg 的批量语义**，只能用「循环 + 非阻塞直到 EWOULDBLOCK」模拟。macOS 只有 `sendmsg` 循环。`sendfile` 在 Windows 是 `TransmitFile`（一次一整文件 + 可选 header/footer），在 macOS 是 `sendfile(fd, in_fd, offset, &len, hdtr, flags)`（签名不同、len 是 in/out）。

#### E. 文件 / 进程 / 调试

| 区域 | 事实 |
|---|---|
| `src/os/file.cpp` | 15 处裸 `syscall(SYS_openat/read/write/pread/pwrite/fsync/ftruncate/fstat/lseek)` |
| `src/os/proc.cpp` | **35 处**裸 syscall（`/proc` 解析、`getpid` 族） |
| `src/os/raw_io.cpp` | 13 处裸 syscall（绕过 hook 的直调层） |
| `src/os/monitor/**` | `/proc` + eBPF（`kernel_ebpf.cpp`），Linux 专属 |
| `src/debug/**` | `/proc/self/maps`、`/proc/self/fd` |
| `src/network/hook.cpp` | **2979 行**，`:63` 直接 `#error "go2cpp hook interposition currently requires Linux"`；依赖 `dlsym` + 符号 interposition |

**判断**：`os` 文件层是「能用 Win32 重写但量大」的部分（`CreateFileW` 不提供 `pread/pwrite`，只能用 `OVERLAPPED` + `ReadFile/WriteFile` 模拟）。`hook` 在 Windows **无等价机制**（无 `LD_PRELOAD`），只能明确列为非目标。

#### F. TLS

| 位置 | 事实 |
|---|---|
| `cmake/Go2CppModules.cmake:168-191` | 对 concurrency/io/network/bufio/p2p 整 TU 加 `-ftls-model=initial-exec` |
| `src/concurrency/fiber.cpp:129-133`、`scheduler.cpp:213-219`、`sync.cpp:774-797`、`channel.cpp:79-85` | 变量级 `__attribute__((tls_model("initial-exec")))` |
| `sync.cpp:789-794`（注释） | 明确记录 initial-exec 与 `dlopen` 互斥，以及 64KB 静态 TLS 预算风险 |

**判断**：MSVC 对应物是 `__declspec(thread)`（默认即为静态 TLS 模型），在 **DLL 中动态加载**时同样有「静态 TLS 块已固定」的限制——与 Linux 的 initial-exec 限制同构，迁移后风险等级不变，但要在文档里显式声明「本库不支持 dlopen 热加载」这一既有限制在 Windows 同样成立。

### 2.4 已有可复用资产（这是本次工作量的最大折扣项）

| 资产 | 位置 | 复用价值 |
|---|---|---|
| `IOBackend` 虚接口 + 工厂 | `include/go2cpp/network/io_backend.hpp:26-88` | **直接复用**，Windows 后端实现同一接口即可，IOManager 零改动 |
| `EpollBackend` | `src/network/io_backend.cpp:40-156` | 作为 Windows 后端的**结构模板**（唤醒 fd / token=0 / 批量事件） |
| `futex_mutex` 非 Linux 回退 | `include/go2cpp/concurrency/core/futex_mutex.hpp:117-224` | 可直接编译，阶段 2 再升级为 WaitOnAddress |
| `sync.cpp` 非 Linux 分支 | `src/concurrency/sync.cpp:653-666, 761-763` | MSVC 编译已验证（产品分支 `go2cpp_windows_sync_compile`） |
| `parking_condition` 降级路径 | `src/concurrency/parking_condition.cpp:178-183` | 无 `futex_waitv` 时的逐节点唤醒，Windows 可直接走这条 |
| `path/filepath.cpp` Windows 分支 | `:57-200, 622-816` | 完整的盘符/UNC/反斜杠实现，可直接用 |
| `logging/log.cpp` `_WIN32` 分支 | `:102, :308` | `localtime_s` 差异已处理 |
| **产品分支 `go2cpp_platform` 全套** | `Go2Cpp-product/src/platform/{linux,windows}/**` + `cmake/Go2CppPlatform.cmake` | Linux 0 warning、Windows 单测 1/1 通过；含 `error/time/thread/socket/io_poller/file/fiber` 七个后端，**可整体回流** |

### 2.5 关键量化

- 代码量：`src/` + `include/` = **196,118 行**；`hook.cpp` 单文件 2,979 行。
- 测试：`cmake/Go2CppTests.cmake` 共 **77 处 `add_test`**，实际 ctest 用例约 117-118（含 `SKIP_RETURN_CODE=77` 的 3 个 tools 用例）。
- 构建基线：`.build-bento-wsl` 为 Release；WSL Ubuntu 24.04、g++ 13.3。
- 热路径敏感度（来自代码注释的实测数据）：WSL2 一次 `FUTEX_WAKE` 实测 **19-20µs**；`mmap(8MiB)` ≈1.1µs、`mprotect` ≈1.9-7µs、`munmap` ≈2.7-8µs；Fiber 切换为纳秒级。**这决定了抽象层必须是零成本内联，不能引入虚调用或函数指针**。

---

## 三、目标平台与能力映射

| 能力 | Linux x86_64（现状） | Windows x64（目标） | macOS arm64（可选） | ARM64/RISC-V Linux（边缘） |
|---|---|---|---|---|
| 上下文切换 | 自研 `context_x64.S` | 新写 `context_win64`（MASM 或 clang `.S`） | 新写 `context_aarch64`（Mach-O） | 新写 `context_aarch64.S` / `context_riscv64.S` |
| Fiber 栈 | `mmap` + `mprotect` + SIGSEGV | `VirtualAlloc(MEM_RESERVE/COMMIT)` + `PAGE_GUARD`/VEH | `mmap`（同 POSIX） | `mmap`（同 POSIX） |
| 线程阻塞 | `futex` | `WaitOnAddress/WakeByAddress*` | `__ulock_wait/__ulock_wake`（私有）或 `pthread_cond` | `futex`（同 Linux） |
| 事件循环 | `epoll` + `eventfd` | **IOCP**（终态）/ `WSAPoll`（过渡） | `kqueue` | `epoll` |
| 批量 IO | `sendmmsg/recvmmsg` | `WSASendMsg/WSARecvMsg`（需取函数指针） | `sendmsg` 循环 | 同 Linux |
| 零拷贝 | `sendfile` | `TransmitFile` | `sendfile`（签名不同） | 同 Linux |
| 单调时钟 | `clock_gettime(CLOCK_MONOTONIC)` | `QueryPerformanceCounter` | `mach_absolute_time` | `clock_gettime` |
| 文件/目录 | 裸 syscall + `dirent` | `CreateFileW` + `FindFirstFileW` | POSIX | POSIX |
| 进程/信号 | `/proc`、`SIGPIPE`、`pthread_sigmask` | 无 SIGPIPE（`MSG_NOSIGNAL` 不存在，用 `SO_NOSIGPIPE` 思路改为忽略错误） | 同 POSIX | 同 Linux |
| Hook | `LD_PRELOAD` interposer | **无等价，非目标** | 无等价，非目标 | 同 Linux |
| TLS | `initial-exec` | `__declspec(thread)` | `thread_local` | `initial-exec` |

---

## 四、OSAL 设计

### 4.1 命名决策（必须先拍板）

**冲突事实**：`include/go2cpp/os/**` 现有 22 个公共头，是 **Go `os` 包的镜像**（`os/file/dir/proc/pty/channel/mq/shm/pool/supervisor/monitor`），`os.hpp` 头部原文：「它**不**做平台抽象（POSIX only）」。

| 方案 | 做法 | 代价 | 评价 |
|---|---|---|---|
| **A（推荐）** | OSAL 放 `include/go2cpp/platform/**` + `src/platform/{linux,windows,macos}/**` | 与用户原话的目录名不一致 | 语义准确；**可直接复用产品分支 `62dfe77`+`864394b` 的实现**，省 8-12 人天；与既有 `os` 模块零冲突 |
| B | OSAL 放 `include/go2cpp/os/port/**` + `src/os/port/{linux,windows}/**` | 无迁移成本，但目录语义变模糊（`os/` 下同时有 Go API 与平台抽象） | 满足字面要求，可作为 A 的折中命名 |
| C | 把现有 `os` 模块改名 `go2cpp/sys/`，`os/` 让给 OSAL | 22 个公共头 + 60+ 处 include + 全部测试/示例 | **不建议**：纯改名工作量 3-5 人天，且破坏所有下游 include |

**建议采用 A**，若必须贴合用户原话则用 B。两者接口完全一致，只是目录前缀不同，可实现期一次性决定。

### 4.2 分层

```
include/go2cpp/platform/          # 接口层：只依赖 C++17 标准库，不泄漏 <windows.h>/<sys/socket.h>
  platform.hpp                    #   平台枚举、版本、编译期能力位（constexpr）
  error.hpp                       #   errno / WSAGetLastError / GetLastError → 统一 PlatformError
  time.hpp                        #   Now() / MonotonicNanos() / SleepFor()
  thread.hpp                      #   CurrentThreadId / SetThreadName / Yield / Affinity（可失败）
  sync.hpp                        #   Futex（wait/wake，内联）+ Mutex/CV 别名
  io_poller.hpp                   #   Poller 接口（= 现有 IOBackend 的 OSAL 镜像）
  socket.hpp                      #   NativeSocket（int / SOCKET 别名）、Endpoint、Resolve、SocketHandle
  memory.hpp                      #   Reserve/Commit/Protect/Release（mmap ↔ VirtualAlloc）
  fiber.hpp                       #   FiberBackend 枚举与选择结果
  file.hpp                        #   FileHandle / Stat / ReadDir / UTF-8↔UTF-16

src/platform/linux/**             # Linux 后端：包装现有 POSIX 调用，行为逐字节等价
src/platform/windows/**           # Windows 后端：Winsock2 / QPC / WaitOnAddress / VirtualAlloc / IOCP
src/platform/macos/**             # macOS 后端：kqueue / ulock / mmap
src/platform/common/**            # 平台无关：错误文本、路径小工具
```

**四条硬原则**：
1. 公共头**零平台头泄漏**——`NativeSocket` 用整型别名收口，`iovec`/`WSABUF` 差异用 OSAL 自己的 `Slice` 结构屏蔽。
2. 平台代码只出现在 `src/platform/{os}/` 与 CMake；**业务模块不得新增 `#ifdef`**。现有 14 个文件的 `__linux__` 逐步替换为 `platform::kHasFutex` 这类 `constexpr` 能力位。
3. Linux 后端**先包装、不重写**：同一套 POSIX 调用，保证 `objdump` 对比无回归。
4. 接口**能内联就内联**：热路径（futex、poller 的 add/modify）必须是 `inline` + `constexpr` 分派，禁止虚函数/`std::function`。

### 4.3 接口清单（草案，仅示意签名）

```cpp
// platform/platform.hpp —— 编译期能力位，驱动 #if 收敛
namespace go2cpp::platform {
inline constexpr bool kHasFutex   = /* linux or win or mac */;
inline constexpr bool kHasEpoll   = /* linux */;
inline constexpr bool kHasIocp    = /* win */;
inline constexpr bool kHasKqueue  = /* mac */;
inline constexpr bool kHasSendmmsg = /* linux */;
enum class FiberBackend { kNativeAsm, kThreadFallback, kUnsupported };
constexpr FiberBackend SelectedFiberBackend() noexcept;
}

// platform/sync.hpp —— futex 的零成本抽象（热路径核心）
namespace go2cpp::platform {
// 返回值语义与 FUTEX_WAIT 对齐：0=被唤醒，EAGAIN=值已变，ETIMEDOUT=超时
int FutexWait(std::atomic<std::uint32_t>* word, std::uint32_t expected,
              const timespec* relative_timeout) noexcept;   // inline：Linux 走 syscall，Win 走 WaitOnAddress
int FutexWake(std::atomic<std::uint32_t>* word, int count) noexcept;
// 双字等待：Windows 无等价，退化为 Wake(all) + 重判
int FutexWaitTwo(std::atomic<std::uint32_t>* a, std::uint32_t av,
                 std::atomic<std::uint32_t>* b, std::uint32_t bv,
                 const timespec* deadline) noexcept;
}

// platform/memory.hpp —— 栈 arena 的分配原语
namespace go2cpp::platform {
void* Reserve(std::size_t bytes);                  // mmap(PROT_NONE) / VirtualAlloc(MEM_RESERVE)
bool  Commit(void* base, std::size_t offset, std::size_t bytes);  // mprotect / VirtualAlloc(MEM_COMMIT)
bool  Protect(void* base, std::size_t bytes, int prot);           // mprotect / VirtualProtect
void  Release(void* base, std::size_t bytes);      // munmap / VirtualFree
}

// platform/io_poller.hpp —— 与现有 IOBackend 同构，Windows 后端直接实现
//   Initialize / Add / Modify / Remove / Wait / Wake / DrainWake / Shutdown
//   语义契约：token 原样回传；Wait 返回就绪位；Shutdown 后不再唤醒。

// platform/file.hpp
//   Open(path_utf8, mode) / Read / Write / Pread / Pwrite / Fsync / Truncate / Stat / ReadDir
```

### 4.4 性能约束（硬性）

| 约束 | 具体要求 | 验证方法 |
|---|---|---|
| 热路径零虚调用 | futex 包装必须 `inline`，编译后与现有 `syscall` 指令序列一致 | `objdump -d` 对比 `sync.o` 的 `syscall` 条数与位置 |
| 无额外 TLS 访问 | OSAL 不得引入新的 `thread_local` | 检查 `.tbss` 段大小变化 |
| 无分支放大 | 平台判断用 `if constexpr`/`constexpr bool`，确保死代码消除 | `-O2` 反汇编中不得出现运行时分支 |
| 系统调用次数不变 | 阶段 1 前后 `strace -c` 的 `futex/epoll_wait` 调用数一致（±0） | 同一基准程序对比 |
| 基准波动 ≤3% | `docs/performance-baseline-2026-10-19.md` 的 sync/io 指标 | 现有 benchmark 复跑 |

### 4.5 与既有抽象的关系（不要重复造）

| 既有设施 | OSAL 策略 |
|---|---|
| `io::IOBackend`（虚接口 + 工厂） | **直接复用**，Windows/macOS 后端注册为 `"iocp"` / `"kqueue"`，`DefaultBackendName()` 按平台返回；`IOManager` 一行不改 |
| `EpollBackend` | Linux 后端的实现**就是它**，不搬家、不重写 |
| `core::FutexMutex` | 内部 `#if defined(__linux__)` 换成 `platform::kHasFutex` + `platform::FutexWait/Wake` |
| `detail::MakeContext/JumpContext` | 不动；平台层只负责「选哪个后端」与「汇编文件选哪个」 |
| `os`（Go API 层） | 不动，后续可选地把 `file.cpp` 的裸 syscall 换成 `platform::file` |
| CMake `GO2CPP_NATIVE_CONTEXT` | 保留；新增 `GO2CPP_PLATFORM` 变量（linux/windows/macos）驱动源文件选择 |

### 4.6 就绪 vs 完成：IOCP 桥接设计（本项目最大技术难点）

`IOBackend` 的契约是**就绪模型**：`add_or_modify(fd, interest, token)` 声明关心什么 → `wait()` 返回「谁就绪了」。IOCP 是**完成模型**：提交一个操作 → 完成包告诉你「哪个操作完成了」。

桥接方案对比：

| 方案 | 做法 | 优点 | 缺点 | 结论 |
|---|---|---|---|---|
| **IOCP + 0 字节探测** | 每次 arm 读兴趣时投递 `WSARecv(len=0)`；完成即「可读」。写兴趣投递 `WSASend(len=0)` | 真正的完成端口，万级连接可扩展；与 libuv 的 Windows 实现同构 | 每次 re-arm 一个系统调用；UDP 的 0 字节发送**会真的发出空包**；取消/回收时序复杂 | **终态方案** |
| WSAPoll 过渡 | `WSAPoll` 轮询就绪 | 语义 1:1，实现最简单（产品分支已做过 select 版） | O(n) 扫描；`WSAPoll` 有「连接失败不报错」的已知缺陷；万级连接不可用 | **阶段 2 过渡**，先让功能跑通 |
| AFD poll 组 | `\Device\Afd` 的 `IOCTL_AFD_POLL` | 真就绪语义、可扩展 | 未文档化接口，跨 Windows 版本易碎 | **不采用** |
| select | 产品分支现状 | 现成 | `FD_SETSIZE=64` | 仅作冒烟 |

**IOCP 方案的关键设计点**（实施时必须逐条落实）：

1. **token 代际**：`IOBackend` 的 token 已经是「fd + 注册序号」编码（`io.cpp:298-301`），完成包可能晚于 `remove()`/`close()` 到达，必须用 token 代际丢弃过期完成。
2. **取消与回收**：`remove()` 要 `CancelIoEx(fd, &ov)` 并**等完成包回收**后才能释放 overlapped 结构，否则 use-after-free。
3. **唤醒**：`wake()` → `PostQueuedCompletionStatus(iocp, 0, kSpecialKey, nullptr)`，`wait()` 识别特殊 key 后调 `drain_wake()`（与现有 token=0 约定一致）。
4. **一次性语义**：完成包天然一次性，`supports_one_shot()` 返回 **true**，让 IOManager 走「消费后重布防」路径（`io.cpp:1327/1486` 已支持）。
5. **零字节探测的协议差异**：TCP 可用；UDP 的「可写」探测改为「总是可写」（UDP send 基本不阻塞）+ 可读用 `WSARecvFrom(len=0)`（不消费数据）；`SO_OOBINLINE`/`MSG_PEEK` 场景需单独验证。

---

## 五、分阶段方案

> 用户要求的四阶段，前置一个「阶段 0」——因为门禁不解除，阶段 1 连编译都过不去。

### 阶段 0：解禁门禁 + 基线固化（前置，2-3 人天）

- **范围**：把 `Go2CppModules.cmake:6-8` 的 `FATAL_ERROR` 改为「按平台选择后端 + 不支持时给清晰错误」；`CMakeLists.txt:19-21` 改为按 `CMAKE_SYSTEM_PROCESSOR` 启用 ASM；`target_sources(context_x64.S)` 加平台条件；新增 `GO2CPP_PLATFORM` 变量。
- **交付**：CMake 在 Linux 上**行为完全不变**（同一套源文件、同一套编译选项），非 Linux 上能进入配置阶段并给出「缺哪个后端」的可读错误。
- **验收**：Linux 全量 ctest 117/118 与基线逐项一致；`cmake --build` 产物字节级对比（除时间戳）。
- **回退**：单文件改动，`git revert` 即可。

### 阶段 1：OSAL 接口 + Linux 后端（等价重构，10-14 人天）

- **范围**：
  1. 建 `include/go2cpp/platform/**` 九个头（大部分可从产品分支回流）；
  2. 建 `src/platform/linux/**` 七个后端（**包装现有调用，不重写**）；
  3. 迁移调用点：`futex_mutex.hpp`、`sync.cpp`、`parking_condition.cpp`、`channel.cpp` 的 `#if defined(__linux__)` → `platform::kHasFutex` + `platform::Futex*`；
  4. `io_backend.cpp` 的 `EpollBackend` 原地保留，只把 `DefaultBackendName()` 改为查 OSAL。
- **不做**：不碰 `socket.cpp`/`os`/`hook`（留到阶段 2/非目标）。
- **验收**：
  - `objdump` 对比 `sync.o`/`parking_condition.o` 的 `syscall` 指令条数与偏移**完全一致**；
  - `strace -c` 跑现有基准，`futex` 调用数一致；
  - `docs/performance-baseline-2026-10-19.md` 指标波动 ≤3%；
  - ctest 全绿，用例数与阶段 0 相同。
- **回退**：新增库 + 头，`#if` 替换点逐文件可独立 revert。

### 阶段 2：Windows 后端（28-40 人天，最大块）

拆成五个可独立验收的包：

| 包 | 内容 | 人天 | 验收 |
|---|---|---:|---|
| 2a | `platform/{error,time,thread}` + Winsock 错误码映射 + `/utf-8` | 3-4 | 平台单测全绿（产品分支已有 1/1 基线可对照） |
| 2b | `platform/sync`：`WaitOnAddress/WakeByAddress*` 替换 CV 回退；`futex_waitv` 降级为广播 | 4-6 | `FutexMutex` 2000 轮 + 竞态压测；等待延迟不高于 CV 版 |
| 2c | `platform/memory` + Fiber 栈：`VirtualAlloc` 预留/提交；先只支持**固定栈**，growable 关闭 | 6-9 | Fiber 创建/切换/销毁压测；栈溢出走 `PAGE_GUARD` + VEH |
| 2d | **IOCP 后端**（或先 WSAPoll 过渡） | 10-14 | `IOManager` 在 Windows 跑通 echo 压测（≥1k 连接）；`wait` 延迟与 Linux 同量级 |
| 2e | `socket.hpp`/`address.hpp` 去 POSIX 化 + `WSASendMsg`/`TransmitFile` | 5-7 | 网络子集测试（connect/accept/send/recv/批量发/文件发送） |

- **关键决策点**：2d 先做 `WSAPoll` 让功能闭环（1-2 人天），再换 IOCP。**不要一上来写 IOCP**——先用最笨的后端把「IOManager 状态机在 Windows 正确」验证掉，再优化。
- **验收（阶段 2 整体）**：Windows 上 `go2cpp_platform_tests` + 网络子集 + Fiber 基础测试全绿；`hook` 与 `monitor` 明确排除并在 CMake 中关掉。

### 阶段 3：ARM64 / RISC-V 汇编 + 交叉编译（10-14 人天，可与阶段 2 并行）

- **范围**：
  1. `context_aarch64.S`：保存 `x19-x28, x29(fp), x30(lr), sp, d8-d15`；`context_riscv64.S`：`s0-s11, ra, sp, fs0-fs11`；
  2. CMake 按 `CMAKE_SYSTEM_PROCESSOR` 选汇编文件；
  3. `aarch64-linux-gnu-g++` / `riscv64-linux-gnu-g++` 交叉编译工具链文件；
  4. QEMU 用户态跑单元测试（`qemu-aarch64 -L /usr/aarch64-linux-gnu`）。
- **验收**：ARM64 上 Fiber 创建/切换/销毁 10 万次无崩溃；上下文切换基准不高于 x64 的 1.5 倍。
- **注意**：ARM64 的 `sp` 必须 16 字节对齐；若开启 BTI，函数入口需 `bti c`；RISC-V 需要处理 `gp/tp` 不被破坏。

### 阶段 4：macOS（可选，12-18 人天）

- **范围**：`kqueue` 后端（`EVFILT_READ/WRITE` + `EVFILT_USER` 唤醒）；`mmap` 后端直接复用 Linux 的 `memory` 实现；futex 用 `__ulock_wait/__ulock_wake`（私有 API，需评估）或退回 `pthread_cond`；Mach-O 汇编（符号下划线、`__TEXT,__text`）。
- **风险**：`__ulock_*` 是 libplatform 私有符号，App Store 分发有风险；建议默认用 `pthread_cond`（性能损失可接受），把 ulock 作为编译期可选优化。
- **验收**：macOS arm64 上同一套 ctest 子集全绿。

### 阶段依赖图

```
阶段 0 ──► 阶段 1 ──► 阶段 2（Windows）
                 └──► 阶段 3（ARM64，可与阶段 2 并行）
                              └──► 阶段 4（macOS，可选）
```

---

## 六、风险与缓解

| # | 风险 | 等级 | 说明 | 缓解 |
|---|---|---|---|---|
| R1 | **IOCP 与 epoll 模型差异** | **最高** | 就绪 vs 完成；`IOBackend` 的 `add_or_modify` 语义在 IOCP 上没有直接对应，必须用「每次 arm 投递探测」模拟，带来额外系统调用与取消/回收时序问题 | 先 WSAPoll 闭环功能，再换 IOCP；token 代际丢弃过期完成；`remove` 必须等回收；准备「若 IOCP 不可行，Windows 长期用 WSAPoll + 连接数上限」的回退 |
| R2 | Fiber 栈在 Windows 的语义差异 | 高 | `mmap(PROT_NONE)` + SIGSEGV 增长 ↔ `MEM_RESERVE` + `PAGE_GUARD` + VEH；Windows 的栈溢出异常处理比 SIGSEGV 复杂（需要 `_resetstkoflw` 类逻辑） | 阶段 2 先关 growable stack（固定栈），growable 单列后续任务；固定栈下只需 `VirtualAlloc` 预留/提交 |
| R3 | 汇编 ABI 不可复用 | 高 | Win64 有 shadow space、非易失寄存器含 `rdi/rsi/xmm6-15`；ARM64 是完全不同的指令集 | Win64 单独写（骨架照抄 x64 SysV）；ARM64/RISC-V 各自新写，用「保存全部 callee-saved」的保守策略 |
| R4 | initial-exec TLS 与 `dlopen` | 中 | Linux 的 initial-exec 与 dlopen 互斥；Windows 的 `__declspec(thread)` 在 DLL 动态加载时同样受限 | 文档明确「本库不支持 dlopen 热加载」为既有限制；CMake 保留 `GO2CPP_TLS_INITIAL_EXEC=OFF` 开关 |
| R5 | Hook 无 Windows 等价 | 中 | `hook.cpp` 2979 行、`#error` 硬拒；Windows 无 `LD_PRELOAD` | 明确非目标；Windows 侧只用显式异步 API（`Socket::recv` 已支持非阻塞路径） |
| R6 | 公共头 POSIX 污染 | 中 | 8 个公共头直接 include POSIX socket 头，`SocketType` 用 `SOCK_STREAM` 常量、`SocketResult` 用 `ssize_t` | 阶段 2e 先收口到 `platform/socket.hpp`，再改 `net` 公共头；期间用 shim 头过渡 |
| R7 | 平台层「只编译不接入」 | **高（历史教训）** | 产品分支的 `go2cpp_platform` 已编译通过但**没被 runtime 使用**，P3-P6 未做 | 每个阶段的验收必须包含「被真实模块调用」的证据（链接依赖图 + 运行测试），禁止以「新库 0 warning」作为完成标准 |
| R8 | macOS 私有 API | 中 | `__ulock_*` 非公开；App Store 审核风险 | 默认 `pthread_cond`，ulock 作为可选优化 |
| R9 | 测试覆盖下降被误读为「全绿」 | 中 | Windows 侧必然有一批 Linux 专属测试跑不了（hook/epoll/monitor） | 状态文档逐项列明「未覆盖」而非只报通过数 |
| R10 | 现有 117 个测试的回归成本 | 中 | 任何 OSAL 接入都可能扰动热路径 | 阶段 1 用 objdump + strace + 基准三重对比，不只依赖 ctest |

---

## 七、工作量估算

| 阶段 | 内容 | 人天（乐观-悲观） | 说明 |
|---|---|---:|---|
| 0 | 门禁解禁 + 基线固化 | 2-3 | 单文件级改动，但需要完整回归 |
| 1 | OSAL 接口 + Linux 后端 + 热路径迁移 | 10-14 | 接口 ~600 行；若直接回流产品分支的 platform 层，可压到 6-8 |
| 2a | Windows 基础（error/time/thread/winsock 错误码） | 3-4 | 产品分支已有实现，主要是回流 + 补测 |
| 2b | WaitOnAddress 同步后端 | 4-6 | 有 CV 回退可对照，正确性风险低于性能风险 |
| 2c | VirtualAlloc + Fiber 固定栈 | 6-9 | growable 单列，本期不做 |
| 2d | IOCP（或 WSAPoll 过渡）后端 | 10-14 | **最大不确定性**；WSAPoll 版 1-2 人天，IOCP 版 8-12 |
| 2e | socket/address 平台化 + 批量 IO | 5-7 | 8 个公共头收口 + `WSASendMsg` |
| 3 | ARM64/RISC-V 汇编 + 交叉编译 | 10-14 | 两份汇编各 3-5 天，工具链 2-3 天 |
| 4 | macOS（可选） | 12-18 | kqueue + ulock 评估 + Mach-O 汇编 |
| **合计** | 不含阶段 4 | **50-71** | 约 10-14 周单人；含 macOS 62-89 人天（13-18 周） |

**关键路径**：阶段 0 → 阶段 1 → 阶段 2d（IOCP）。阶段 3 与阶段 2 可并行（不同人）。

**可压缩项**：
- 直接 `cherry-pick` 产品分支 `62dfe77 + 864394b` 的平台层，阶段 1 与 2a 合计可省 **8-12 人天**；
- 阶段 2d 若接受「Windows 长期用 WSAPoll + 连接数上限」，可省 8-10 人天，代价是放弃万级连接能力。

**不可压缩项**：阶段 2c（Windows 栈语义）、阶段 3（汇编）没有捷径，必须实机验证。

---

## 八、参考 JinShanProject

### 8.1 它的实际做法（事实）

| 项 | 事实 |
|---|---|
| 定位 | **纯 Windows 产品**（Win7 SP1+ x86/x64），Win32 原生 UI + `IRDPSRAPI*` 媒体层；`docs/03-architecture/architecture.md` 明确「不交付 macOS/Linux 桌面端」 |
| 与 Go2Cpp 的关系 | `third_party/go2cpp/` **只读设计参考，不参与 MSVC 构建**（`ADR-04-base-freeze-migration.md`）；它没有把 Go2Cpp 整个搬上 Windows |
| 它真正做的跨平台 | `TASK-001-xplat`（M0-M3）：建了 `go2cpp_platform` 静态库（Linux/Windows 双后端，七个后端文件），`go2cpp_platform_tests`（Windows 1/1 通过），`go2cpp_windows_futex_tests`（FutexMutex 2000 轮），`go2cpp_windows_sync_compile`（OBJECT，证明 `sync.cpp` 非 Linux 分支可在 MSVC 编译） |
| 覆盖到哪一步 | **只到平台层**；`go2cpp_runtime`/`network`/`os` 仍是 POSIX-only（其 status 文档 §8.3 自述）；M4 未开始 |
| 工具链 | VS 2022 v143 + `_WIN32_WINNT=0x0601` + `/MT` 静态 CRT + `/utf-8`；禁用 VS 2026（取消 Win7 target）；SDK 在 `D:\Windows Kits\10` |
| 踩过的坑 | ① MSVC 默认 GBK 代码页把中文注释误读成语法错误 → 必须 `/utf-8`；② `select` 唤醒用阻塞 UDP socket，排空后第二次 `recv` 永久阻塞 → 必须 `FIONBIO`；③ `std::filesystem::u8path` 在受限令牌下 `ACCESS_DENIED` → 改 `CreateDirectoryW`；④ 移除不存在的 `WSAENOENT/WSAENOTDIR/WSAEISDIR` |

### 8.2 可借鉴

1. **平台层目录结构**：`platform/{error,time,thread,sync,socket,io_poller,file,fiber}.hpp` 九个头 + `src/platform/{linux,windows}/` 七个实现，与本方案的 OSAL 目录一一对应，可直接回流。
2. **错误码统一映射**：`PlatformError` + `FromSystemError`，把 `errno` / `WSAGetLastError` / `GetLastError` 收口，避免业务层判断平台。
3. **用 OBJECT target 做「编译证据」**：`go2cpp_windows_sync_compile` 不改逻辑、只证明非 Linux 分支不腐烂——这是个低成本、高价值的工程手法，本方案应在阶段 1 就引入对应 target。
4. **测试目录固定到构建目录**：避开受限令牌写 `%TEMP%` 被拒。
5. **`/utf-8` 与 `/W4 /permissive-`**：MSVC 下中文注释项目的必备项。

### 8.3 不可借鉴 / 教训

1. **「平台层编译通过」不等于「项目跨平台」**——产品分支的 `go2cpp_platform` 至今没有被 runtime 链接使用，主工作树仍是 Linux-only。**这是本方案最重要的前车之鉴**，R7 就是为它设的。
2. **JinShan 的并发/网络在 Go 侧（relay 是独立 Go 服务），天然跨平台**——它的 C++ 端不需要 Fiber/Scheduler，所以它的「跨平台经验」在并发基座上几乎不适用。不能拿它的 M3 通过来类比 Go2Cpp 的移植难度。
3. **它选择了「不复用 Go2Cpp 的并发基座」**（third_party 只读）——这本身是一个信号：如果 Windows 端只需要「同步阻塞 + 线程池」，绕过 Fiber/Scheduler 可能比移植更划算。方案评审时应把「Windows 端是否真的需要 GMP 语义」作为一个前置问题。

---

## 九、验收门禁

| 门禁 | 标准 | 适用阶段 |
|---|---|---|
| Linux 行为不变 | ctest 用例数与基线一致、全绿；`strace -c` 的 `futex/epoll_wait` 次数一致 | 0/1 |
| 热路径零成本 | `objdump` 的 `syscall` 指令条数与偏移一致；基准波动 ≤3% | 1 |
| 无新 TLS | `.tbss` 段大小不变 | 1 |
| Windows 可运行 | 平台单测 + 网络子集 + Fiber 基础测试全绿（**不是只编译**） | 2 |
| 接入证据 | `ldd`/链接图中真实模块依赖 OSAL；有运行期测试覆盖 | 2/3 |
| ARM64 实机 | Fiber 10 万次创建/切换无崩溃；切换基准 ≤x64 的 1.5 倍 | 3 |
| 未覆盖项显式声明 | 状态文档逐项列出跑不了的测试与原因，不以「全绿」掩盖 | 全部 |
| 原工作区 | 实现阶段必须在新 worktree 进行，主工作树零改动 | 全部 |

---

## 十、附录：扫描命令与证据

```sh
# 门禁
grep -n "FATAL_ERROR" cmake/Go2CppModules.cmake
grep -n "enable_language(ASM)\|context_x64.S" CMakeLists.txt cmake/Go2CppModules.cmake

# 平台宏
grep -rn "__linux__" src include | wc -l          # 49 处 / 14 文件
grep -rn "_WIN32\|__APPLE__" src include | wc -l   # 50 处

# 裸系统调用
grep -rn "syscall(SYS_" src include | wc -l        # 108 处
grep -rn "syscall(SYS_" src include | cut -d: -f1 | sort | uniq -c | sort -rn

# IO 后端（确认 io.cpp 无直接 epoll）
grep -n "::epoll_\|m_backend->" src/network/io.cpp

# 公共头污染
grep -rln "sys/socket.h\|netinet/\|arpa/inet\|sys/uio.h\|sys/mman.h\|sys/epoll.h\|pthread.h" include

# futex 收口点
grep -n "SYS_futex\|FUTEX_WAIT_PRIVATE\|FUTEX_WAKE_PRIVATE" src/concurrency/*.cpp include/go2cpp/concurrency/core/*.hpp

# 批量 IO
grep -n "sendmmsg\|recvmmsg\|sendfile" src/network/socket.cpp

# 产品分支既有资产
ls /mnt/e/CodexWorkspace/Go2Cpp-product/src/platform/{linux,windows}/
sed -n '1,60p' /mnt/e/CodexWorkspace/Go2Cpp-product/docs/cross-platform-status.md
```

**证据文件清单**：

| 文件 | 用途 |
|---|---|
| `docs/modules/portability.md` | 既有标注清单（本文的前置） |
| `cmake/Go2CppModules.cmake:6-8, 35, 168-191` | 门禁与 TLS |
| `src/network/io.cpp:686, 1305, 1327, 1348, 1740` | IOManager 与后端契约 |
| `src/network/io_backend.cpp:40-156, 218-224` | EpollBackend 与默认后端名 |
| `src/concurrency/sync.cpp:90-106, 653-666, 761-763` | futex 收口与非 Linux 回退 |
| `src/concurrency/fiber.cpp:34-39, 436, 469, 686` | 栈分配与 SIGSEGV 增长 |
| `src/concurrency/context_x64.S:12`、`context_backend.cpp:28-58` | 汇编平台门 |
| `src/network/socket.cpp:18, 702, 889, 958, 1003` | 批量 IO 与零拷贝 |
| `src/network/hook.cpp:63` | Hook 的 Linux 硬约束 |
| `Go2Cpp-product/docs/cross-platform-plan.md`、`cross-platform-status.md` | 已完成一次的 M0-M3 与教训 |

---

## 十一、变更记录

| 日期 | 变更 |
|---|---|
| 2026-11-01 | 创建：只读调研完成，产出 OSAL 设计、四阶段计划、风险与工作量估算 |
