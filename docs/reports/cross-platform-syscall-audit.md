# 跨平台系统调用契约审计报告

> 项目：Go2Cpp（WSL 真树 `/UserData/CodexWorkSpace/Go2cpp`）
> 范围：`src/` + `include/`，276851 行 C/C++；`grep -rn` 逐项定位。
> 结论统计：**48 类**系统调用/内核接口 → **40 类需适配、8 类无需适配**（无「待定」）。
> 配套明细表：`docs/plans/cross-platform-plan.md` 的「### 1.3 本项目调查结果」。
> 日期：2026-11-07。

## 一、扫描结果（按模块）

| 模块 | 类数 | 需适配 | 无需适配 | 硬阻断 |
|---|---:|---:|---:|---|
| 网络 | 14 | 13 | 1（TCP_NODELAY） | `hook.cpp:64` `#error`；无非 Linux IO 后端 |
| 线程/内存/时间 | 9 | 7 | 2 | `sync.cpp` 仅有 Linux futex 快路径 |
| 进程 | 7 | 6 | 1（getpid） | `fork` 无对应，`os/proc` 整体重写 |
| 文件 | 7 | 6 | 1（路径已适配） | 目录 fsync / unlink 语义弱化 |
| 诊断/系统信息 | 6 | 5 | 1（eBPF 不构建） | `/proc` 全域失效 |
| 杂项 | 5 | 3 | 2（timerfd、gettimeofday 未使用） | — |

## 二、TOP10 风险

### 1. LD_PRELOAD 透明 IO 拦截在 Windows 不存在（架构级）
- 代码位置：`src/network/hook.cpp:64`（`#error "requires Linux"`）、`:267`（`dlsym(RTLD_NEXT)`）、`:2144`（nanosleep）、`:2287`（select）
- 影响：整个「阻塞调用 → fiber park → epoll 唤醒」的协程化前提消失；业务代码用 `std::socket` 不再免费获得异步语义；`sleep`/`poll` 会真阻塞 worker。
- 适配方向：Windows 放弃透明拦截，业务侧统一走 `go2cpp::network` 显式 park；IAT/inline hook 只作为可选兼容层。**必须先定「fiber 化白名单」，这是能力契约变更。**

### 2. `epoll` → IOCP：就绪 vs 完成，语义根本不同
- 代码位置：`src/network/io_backend.cpp:48/53/93/105/117`；`src/network/io.cpp:2150`（唯一等待点）、`:697`（初始化）；`io_backend.cpp:218-222`（非 Linux 返回空后端名）
- 影响：`IOBackend::add_or_modify(interest)+wait()` 是就绪形状，IOCP 无法实现；Windows 上 `IOManager` 直接起不来 → 所有网络路径不可用。
- 适配方向：短期补 `WSAPoll/select` 后端保住 reactor；长期为 socket 增 completion 后端（投递 `WSARecv/WSASend`、完成包唤醒 fiber），`interest` 降级为能力位。

### 3. `eventfd` 无跨平台等价物
- 代码位置：`src/network/io_backend.cpp:50/129-137`；`bento/net/link.cpp:936/1013/1183`；`rd_net/event_loop.cpp:94`；`os/supervisor.cpp:284`
- 影响：跨线程唤醒是 reactor 的命门，缺失会导致 worker 睡死（丢唤醒）。
- 适配方向：抽 `WakeEvent`——reactor 用 loopback socketpair（可 `WSAPoll`），completion 用 `PostQueuedCompletionStatus` 伪造完成包。

### 4. `fork`/`execve`/`waitpid`/`kill` 进程模型
- 代码位置：`src/os/proc.cpp:1100`（fork）、`:815`（execve）、`:1173/:1263`（waitpid）、`:1335`（kill）；`os/pool.cpp:419`；`media/audio.cpp:512`
- 影响：`os` 模块的进程子层整体不可移植；`kill(SIGTERM)` 的优雅关闭在 Windows 无对应。
- 适配方向：`CreateProcess` + `STARTUPINFOEX` + 命令行转义；SIGTERM → `CTRL_BREAK`/IPC 退出请求，超时才 `TerminateProcess`；`setsid/setrlimit/setuid` 一律显式「不支持」。

### 5. `/proc` 全域依赖（诊断/监控/自省）
- 代码位置：`src/os/proc.cpp:345/385/531/1943/2347`；`debug/debug_server.cpp:1422/1442`；`os/monitor/cpu_monitor.cpp:12`、`net_monitor.cpp:78`、`fileio_monitor.cpp:101/109/159`；`concurrency/fiber.cpp:3089/3221`
- 影响：进程指标、fd 审计、指针审计、容器诊断全部失效；若静默返回 0，监控图会「撒谎」。
- 适配方向：抽 `SystemProbe`（`GetProcessMemoryInfo`/`GetSystemTimes`/`GetAdaptersAddresses`/PDH/`VirtualQuery`）；不可得指标返回「不支持」。

### 6. `rename` 覆盖 / `unlink` 打开中的文件（WAL、SST、日志轮转）
- 代码位置：`distributed/coordinator_snapshot.cpp:185/199`（rename+目录 fsync）；`storage/sstable.cpp:306`；`bento/app/file_browser.cpp:297/301`；`fetch.cpp:1418/1203`；`logging/log.cpp:661`
- 影响：Windows 上 `rename()` 目标存在即失败 → WAL/SST 的原子替换失败；打开中的文件不能删 → 日志轮转与临时文件清理失败。
- 适配方向：统一 `AtomicReplace()`（`MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH)` + 同卷校验）；所有打开加 `FILE_SHARE_DELETE`；轮转改「关闭→改名→重开」；目录 fsync 退化并文档化。

### 7. `futex` → `WaitOnAddress`
- 代码位置：`src/concurrency/sync.cpp:91/101/670`；`concurrency/parking_condition.cpp:170`
- 影响：无 `PRIVATE`/`REQUEUE`，超时按毫秒且受系统定时器影响 → park/unpark 延迟可能从 µs 级退化；`#else` 回退只保证正确性。
- 适配方向：封装 `platform/futex.hpp`；Windows 用 `WaitOnAddress` + `timeBeginPeriod(1)`；必须重跑 park/unpark 基准（对照 `docs/performance.md`）。

### 8. `nanosleep` → `Sleep`：15.6ms 精度
- 代码位置：`src/network/hook.cpp:2144/2098/2123`；30 处 `std::this_thread::sleep_for`；定时器 `concurrency/timer.cpp:283`（由 epoll 超时驱动）
- 影响：所有 sub-ms 退避/轮询节拍被抬到 15.6ms；scheduler/timer 的时延粒度退化。
- 适配方向：`timeBeginPeriod(1)` + `CreateWaitableTimerEx(HIGH_RESOLUTION)` + 自旋补偿；文档标注 Windows 上 sleep 不再 fiber 化。

### 9. `SO_REUSEADDR` 语义反转（端口可被静默抢占）
- 代码位置：21 处，关键 `network/socket.cpp:1352`、`rd_net/socket.cpp:337/1507`、`bento/net/link.cpp:312`、`tunnel/net.cpp:529`、`rpc/transport.cpp:358`、`debug/debug_server.cpp:1569`
- 影响：Linux 上第二个实例会 `EADDRINUSE` 失败；Windows 上会**成功**，两个进程同时监听 → 请求被随机分流，测试与线上都极难排查。
- 适配方向：监听 socket 额外设 `SO_EXCLUSIVEADDRUSE`；`Listen` 前显式探测端口占用。

### 10. `errno` vs `WSAGetLastError`/`GetLastError`
- 代码位置：807 行涉及 `errno`（`src/`+`include/`）；`EINTR`/`EAGAIN` 遍布重试循环
- 影响：错误码空间不同，`EINTR` 重试在 Windows 永不命中；错误分支判断会静默走错路径。
- 适配方向：`platform::LastError()` 统一映射；socket 路径单独 WSA→errno 表；重试循环保留但注明平台差异。

## 三、结论

- **不能先写代码再补平台层**：B-1（hook `#error`）与 B-2（无非 Linux IO 后端）决定 Windows 版本当前**编译不过/起不来**，须先补 `platform/` 三个基础件：`WakeEvent`、`LastError`、`SteadyClock`。
- 优先级：P0 = hook 白名单 + IOBackend 后端 + 进程子层；P1 = 文件原子替换/删除语义 + `/proc` 采集层；P2 = futex/定时器精度、pty、shm/mq、inotify。
- 每项适配都要带「Linux 无回归」验证：同一 commit 两侧跑（见计划第三节）。
