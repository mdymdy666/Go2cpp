# rd_net 平台差异：Linux/POSIX 与 Windows（MSVC/Winsock）

- 状态：Windows 分支已实现（`src/rd_net/socket.cpp`、`src/rd_net/event_loop.cpp`
  的 `#if defined(_WIN32)` 段），**已于 2026-11-04 用真 MSVC 2022 编译并运行四个
  测试（406/44/44/64 全 0 失败）**；9 项测试期平台差异逐条登记在第 9 节。
- 范围：REQ-006 四态 Socket、REQ-007 事件循环；REQ-008/009 的 protocol.cpp
  只依赖本层接口，未新增平台分支。
- 原则：Windows 头（`winsock2.h` / `ws2tcpip.h` / `windows.h`）只出现在
  `src/rd_net/*.cpp` 的 `_WIN32` 段内，**公共头不进入 Windows include 链**；
  SOCKET 用 `std::uintptr_t` 保存（`UINT_PTR` 宽度），由
  `native_handle()` 暴露给事件循环。

本文件按「Linux 行为 / Windows 行为 / 我们怎么处理 / 风险」四栏，逐项列出
两套平台在 socket、IO 多路复用、定时器、线程、文件、时间上的差异，并在
第 7 节列出 Windows 上**不可用或降级**的功能。

## 1. Socket（WSA vs POSIX）

### 1.1 库初始化与关闭

| 项 | 内容 |
|---|---|
| Linux 行为 | 不需要初始化；`socket()` 直接用。进程退出自动回收。 |
| Windows 行为 | 必须先 `WSAStartup(MAKEWORD(2,2), &WSADATA)`，否则所有 socket 调用返回 `WSANOTINITIALISED`；`WSACleanup` 按引用计数成对出现。 |
| 我们怎么处理 | `EnsureWinsock()` 用函数内 `static` 做进程级一次性初始化（C++11 magic static，线程安全）；返回 0 或 WSA 错误码，失败时 `Open/Create` 返回 `kIoError`。**不调用 `WSACleanup`**：库不能替宿主决定何时销毁 Winsock，进程退出由 OS 回收；代价是引用计数不归零。 |
| 风险 | 宿主若在 `main` 返回前自己调用 `WSACleanup`，残留的 rd_net 对象析构时 `closesocket` 会失败（返回 `WSANOTINITIALISED`）。文档要求：`WSACleanup` 只能在所有 rd_net 对象销毁后调用。 |

### 1.2 句柄类型与所有权

| 项 | 内容 |
|---|---|
| Linux 行为 | fd 是 `int`，`-1` 表示无效。 |
| Windows 行为 | `SOCKET` 是 `UINT_PTR`（64 位构建下 8 字节），`INVALID_SOCKET = (SOCKET)(~0)`；句柄值不保证能用 `int` 表示。 |
| 我们怎么处理 | 公共头新增 `std::uintptr_t native_handle()`；Windows 成员 `native_socket_`（`uintptr_t`）保存真实句柄，`fd_` 只作诊断镜像（`static_cast<int>`，理论上可能截断/为负）。`valid()` 在 Windows 走 `native_socket_ != kInvalidNativeHandle`。事件循环必须用 `native_handle()`，**不得**用 `fd()`。 |
| 风险 | 任何外部代码若用 `fd()` 在 Windows 上做 `select`/`closesocket`，句柄值可能被截断。这是刻意的 API 边界：`fd()` 在 Windows 上仅诊断。 |

### 1.3 错误码体系

| 项 | 内容 |
|---|---|
| Linux 行为 | `errno`（`EAGAIN/EWOULDBLOCK/ECONNRESET/EPIPE/EMSGSIZE/ETIMEDOUT`），线程局部。 |
| Windows 行为 | `WSAGetLastError()` 返回 `WSA*`（`WSAEWOULDBLOCK/WSAECONNRESET/WSAEMSGSIZE/WSAETIMEDOUT/WSAENOBUFS/WSAEDISCON` 等），**与 `errno` 数值不同**，且不是线程局部 `errno`。 |
| 我们怎么处理 | 新增 `MapSendError/MapRecvError/MapConnectError` 三个映射函数：`WSAEWOULDBLOCK/WSAENOBUFS→kWouldBlock`；`WSAECONNRESET/WSAECONNABORTED/WSAECONNREFUSED→kReset`；`WSAEMSGSIZE→kTruncated`；`WSAETIMEDOUT→kTimeout`；`WSAENOTCONN/WSAESHUTDOWN/WSAEDISCON→kClosed`；其余 `kIoError`。`last_system_error()` 在 Windows 返回 WSA 码。 |
| 风险 | 上层若把 `last_system_error()` 当 `errno` 判断（例如与 `EAGAIN` 比较），Windows 上会误判。已有测试只断言 `IoStatus`，不依赖具体错误码；新代码请只比较 `IoStatus`。 |

### 1.4 非阻塞与关闭

| 项 | 内容 |
|---|---|
| Linux 行为 | `fcntl(fd, F_SETFL, O_NONBLOCK)`；`close(fd)`；`MSG_DONTWAIT` 逐次非阻塞。 |
| Windows 行为 | `ioctlsocket(s, FIONBIO, &mode)`；`closesocket(s)`；没有 `MSG_DONTWAIT`（socket 本身非阻塞即可）。`WSAEventSelect` 会**自动**把 socket 设为非阻塞。 |
| 我们怎么处理 | `Open` 中统一 `ioctlsocket(FIONBIO)`；收发一律 `WSASend/WSASendTo/WSARecv/WSARecvFrom`（无 `MSG_DONTWAIT`，依赖 socket 非阻塞属性）。`Close()` 幂等，`closesocket` 后句柄置无效。 |
| 风险 | 若外部把同一个 socket 又设回阻塞模式（或另一次 `WSAEventSelect` 改标志），非阻塞语义会被破坏；本层不感知。 |

### 1.5 发送与部分发送

| 项 | 内容 |
|---|---|
| Linux 行为 | `send/sendto` + `MSG_NOSIGNAL`（避免 SIGPIPE）；UDP 一次一条、不可切分；TCP 可比 `writev` 批量。 |
| Windows 行为 | `WSASend/WSASendTo`；**没有 SIGPIPE**，对端关闭时错误为 `WSAECONNRESET/WSAENOTCONN/WSAESHUTDOWN`，不需要 `MSG_NOSIGNAL`；`WSASend` 支持 `WSABUF` 数组（scatter/gather，等价 `writev`）；**UDP 没有 `sendmmsg`**。 |
| 我们怎么处理 | `StreamSocket::SendBatch` 一次 `WSASend` 提交最多 1024 段（`WSABUF` 数组），保持与 Linux `writev` 相同的「部分写 + 完整前缀段数」语义；`DatagramSocket::SendBatch` 逐条 `WSASendTo` 循环，遇到 `kWouldBlock` 立刻停止并记录 `last_batch_submitted_`，调用方从第 K 条重试。单条 `SendTo` 的隐式批量配置（`batch_bytes/enable_sendmmsg`）在 Windows 上**无效**（没有 sendmmsg 可退化为一条）。 |
| 风险 | UDP 批量从 1 次系统调用退化为 N 次，高 PPS 场景下 syscall 开销上升（N 次 vs 1 次）；语义（全有或全无、前缀提交）不变。`DatagramSocket::SendBatch` 不是原子的，与 Linux 一致仍需按 `last_batch_submitted()` 重试。 |

### 1.6 接收与截断

| 项 | 内容 |
|---|---|
| Linux 行为 | `recvmsg(..., MSG_TRUNC)` 返回值是**真实报文长度**，可判断 `kTruncated` 并拿到真实长度。 |
| Windows 行为 | `WSARecvFrom` 在缓冲不足时返回 `SOCKET_ERROR + WSAEMSGSIZE`，**不返回真实报文长度**（数据已被丢弃）；没有 `MSG_TRUNC` 等价物。 |
| 我们怎么处理 | `RecvDatagramRaw` 在 `WSAEMSGSIZE` 时返回 `IoStatus::kTruncated`，`*received` 记为**缓冲大小**（不是真实长度）。事件循环只会消费 `min(received, scratch)` 字节。 |
| 风险 | 上层若依赖 `*received` 区分「刚好填满」与「被截断」，Windows 上无法区分真实长度。文档要求：Windows 上收到 `kTruncated` 一律按「报文已丢失，需扩容重收」处理，不要用 `*received` 做协议判断。 |

### 1.7 连接（非阻塞 connect）

| 项 | 内容 |
|---|---|
| Linux 行为 | 非阻塞 `connect` 返回 `EINPROGRESS`，`poll(POLLOUT)` 后 `getsockopt(SO_ERROR)`。 |
| Windows 行为 | 非阻塞 `connect` 返回 `SOCKET_ERROR + WSAEWOULDBLOCK`，`select` 等可写后用 `getsockopt(SO_ERROR)`。`select` 在 Windows 上 `nfds` 参数被忽略。 |
| 我们怎么处理 | `StreamSocket::Open`：`FIONBIO` → `connect` → `WSAEWOULDBLOCK` 时 `select(0, nullptr, &write_set, &error_set, &tv)`，超时按 `connect_timeout` 返回 `kTimeout`，`SO_ERROR != 0` 按 `kReset/kIoError` 映射。连接成功后 socket 保持非阻塞。 |
| 风险 | `select` 的 `fd_set` 是 Windows 的固定数组结构，`FD_SET` 超 `FD_SETSIZE`（默认 64）会越界写。这里只放 1 个 socket，安全；但**不要**在本层用 `select` 做多 socket 等待。 |

### 1.8 地址解析

| 项 | 内容 |
|---|---|
| Linux 行为 | `inet_pton/getaddrinfo/getnameinfo/freeaddrinfo`（glibc）。 |
| Windows 行为 | 同名 API（`ws2tcpip.h`，`_WIN32_WINNT >= 0x0600` 才有 `inet_pton`）；`getaddrinfo` 失败返回的 `EAI_*` 取值与 POSIX 不同。 |
| 我们怎么处理 | 同一套调用；`ResolveEndpoint` 失败时把 `getaddrinfo` 的返回值直接写进 `last_error_`（Windows 上是 WSA/EAI 码）。为兼容 Windows 7，CMake 定义 `_WIN32_WINNT=0x0601`。 |
| 风险 | 错误码跨平台不可比；上层只应看 `IoStatus::kInvalid`。 |

## 2. IO 多路复用

### 2.1 为什么不用 WSAPoll

| 项 | 内容 |
|---|---|
| Linux 行为 | `poll(2)`：`pollfd` 数组 + 毫秒超时；`eventfd` 做跨线程唤醒。 |
| Windows 行为 | `WSAPoll` 是 poll 的近似物，但没有 `eventfd` 等价物（只能等 SOCKET，不能等 HANDLE）；且对非阻塞 `connect` 完成、UDP 的 `POLLOUT/POLLHUP` 语义与 `poll(2)` 不一致（curl/libuv 都记录过该实现问题）。 |
| 我们怎么处理 | 改用 **WSAEventSelect + WaitForMultipleObjectsEx**：每个已挂载 socket 一个 `WSAEVENT`（`WSACreateEvent`），注册 `FD_READ`（TCP 额外 `FD_CLOSE`）；`Wake()` 用 `CreateEvent` 的**自动重置**事件，`WaitForMultipleObjectsEx` 同时等唤醒事件与所有 socket 事件；`WSAEnumNetworkEvents` 取事件并重置。定时器直接复用等待调用的毫秒超时。 |
| 风险 | 每个 socket 多一个内核事件对象（句柄），挂载/摘除有额外系统调用；`WSAEventSelect` 只支持 `FD_*` 网络事件（没有 `POLLERR` 的直接等价物），错误通过 `recv/send` 的 WSA 错误体现。 |

### 2.2 64 句柄上限（**Windows 特有约束**）

| 项 | 内容 |
|---|---|
| Linux 行为 | `poll` 无固定上限（受 `RLIMIT_NOFILE` 约束），`epoll` 可到数万 fd。 |
| Windows 行为 | `WaitForMultipleObjects(Ex)` 的 `nCount` 上限是 `MAXIMUM_WAIT_OBJECTS = 64`。 |
| 我们怎么处理 | 单个事件循环最多挂 **63 个 socket**（1 个句柄留给唤醒事件）。`AttachDatagram/AttachStream` 在配额满时返回 `false`；`Create()` 不受影响。`kMaxWaitHandles = 64` 在 `event_loop.cpp` 内定义并注释。 |
| 风险 | 单 loop 超过 63 个 socket 的连接网关在 Windows 上无法直接使用，必须拆成多个 `EventLoop`（每线程一个）或改用 IOCP（本层未实现）。这是**明确的可用性上限**，不是 bug。 |

### 2.3 就绪派发

| 项 | 内容 |
|---|---|
| Linux 行为 | 一次 `poll` 返回多个 `revents`，逐个派发。 |
| Windows 行为 | `WaitForMultipleObjectsEx` 一次只返回**一个**被信号唤醒的句柄。 |
| 我们怎么处理 | 每轮只派发一个句柄，其余仍处于 signaled 状态的 `WSAEVENT` 会让下一次等待立即返回，因此不会饿死或丢事件；每 tick 的单 socket 派发预算仍是 64 个报文/读事件。 |
| 风险 | 高并发就绪时每轮一个句柄，tick 的轮转次数变多（仍受「每 tick 64 包」限制）；极端情况下单 tick 吞吐略低于 Linux 的批量 `revents` 派发。 |

## 3. 定时器

| 项 | 内容 |
|---|---|
| Linux 行为 | `poll` 的 `timeout` 是毫秒，`eventfd` 可被 `ScheduleAfter` 唤醒；clock 用 `steady_clock`。 |
| Windows 行为 | `WaitForMultipleObjectsEx` 的 `dwMilliseconds` 同样是毫秒；系统定时器分辨率默认约 15.6ms（`timeBeginPeriod` 可提高到 1ms，但会全局影响功耗，本层不调用）。 |
| 我们怎么处理 | `ComputePollTimeoutMs` 与 Linux 完全同构：最近 deadline 的剩余微秒向上取整到毫秒，上限 60000ms；`ScheduleAfter` 成为堆顶时 `SetEvent` 唤醒等待以重算超时。不建线程、不用 `CreateWaitableTimer`（会多一个句柄并破坏「等待 + IO 同一次阻塞」的结构）。 |
| 风险 | 未调用 `timeBeginPeriod` 时，实际粒度约 15.6ms，**定时器误差可能到 ~16ms**，与文档声称的「目标 ≤5ms」在 Windows 上不成立；测试/上层不要按 1ms 精度断言。需要 1ms 精度时由应用层自行 `timeBeginPeriod(1)`。 |

## 4. 线程

| 项 | 内容 |
|---|---|
| Linux 行为 | `std::thread`（pthread）；`Post` 用 `eventfd` 唤醒 `poll`；`ScheduleAfter` 可从其它线程调用，加锁 + `Wake`。 |
| Windows 行为 | `std::thread`（Win32 线程）语义一致；但**没有** `eventfd`/`pipe2`，跨线程唤醒用 `SetEvent`。 |
| 我们怎么处理 | `Post/Stop/ScheduleAfter/CancelTimer` 的线程约束、`post_mutex` 保护、`Wake()` 调用点与 Linux 完全相同（`SetEvent` 自动重置事件，重复 Set 只保留一次信号；Post 队列整批 swap，不会丢任务）。`Run()` 仍然只能单线程调用。 |
| 风险 | `SetEvent` 的自动重置事件只对「一个等待者」有效；本层是单消费者，安全。若将来改成多线程 `Run`，唤醒语义需重做。 |

## 5. 文件

| 项 | 内容 |
|---|---|
| Linux 行为 | `open/read/write/close`，`int` fd。 |
| Windows 行为 | `_open/_read/_write/_close`（`<io.h>`），或 Win32 `CreateFile`；fd 与 `SOCKET` 是**两套**句柄空间，`closesocket` 不能关文件 fd。 |
| 我们怎么处理 | rd_net **不做文件 IO**（socket.cpp/event_loop.cpp 不包含 `<fcntl.h>`/`<unistd.h>` 的 Windows 等价物），因此没有差异面。上层如需文件能力，请走 `go2cpp::os` 的封装。 |
| 风险 | 无（本层不涉及）。 |

## 6. 时间

| 项 | 内容 |
|---|---|
| Linux 行为 | `std::chrono::steady_clock` 基于 `CLOCK_MONOTONIC`，不受系统时间调整影响。 |
| Windows 行为 | MSVC 的 `steady_clock` 基于 `QueryPerformanceCounter`，同样单调；精度通常 <1µs，但**等待**的精度受系统定时器分辨率限制（见第 3 节）。 |
| 我们怎么处理 | `EventClock = std::chrono::steady_clock` 不变；deadline 计算、`FireDueTimers` 的 cutoff 判定、`ComputePollTimeoutMs` 的剩余时间换算与 Linux 同一份逻辑。 |
| 风险 | 只有「等待精度」有差异，时间源本身一致；不要混用 `system_clock` 做 deadline。 |

## 7. Windows 上不可用 / 降级的功能（明确清单）

1. **单事件循环 63 个 socket 上限**：`WaitForMultipleObjects` 的 64 句柄硬上限
   减去 1 个唤醒事件。超过时 `Attach*` 返回 `false`，不静默截断。
2. **UDP 批量发送降级**：没有 `sendmmsg`，`DatagramSocket::SendBatch` 退化为
   逐条 `WSASendTo`；语义保持（前缀提交 + `last_batch_submitted()`），但
   syscall 数 = 条数，高 PPS 下吞吐低于 Linux。
3. **UDP 截断长度不可得**：`kTruncated` 时 `*received` 是缓冲大小而不是真实
   报文长度（Windows 没有 `MSG_TRUNC`）。
4. **单条 `SendTo` 的隐式批量配置无效**：`SocketOptions::batch_bytes/
   enable_sendmmsg` 在 Windows 上没有 sendmmsg 可退化，单条路径就是一次
   `WSASendTo`；`Send` 的 `enable_writev` 仍有效（`WSASend` scatter/gather）。
5. **定时器精度约 15.6ms**：未调用 `timeBeginPeriod`，与 Linux 的 1ms 粒度
   不同。
6. **`fd()` 仅诊断**：Windows 上不能用它做 `select`/`closesocket`；必须用
   `native_handle()`。
7. **`last_system_error()` 是 WSA 码**：不是 errno，不能与 `EAGAIN` 等比较。
8. **hook / eBPF / bento / relay / tunnel 等模块**：与本层无关，Windows
   构建中保持关闭（`GO2CPP_BUILD_HOOK=OFF`、`GO2CPP_ENABLE_EBPF=OFF`）。
9. **`tests/rd_net/*.cpp` 不可在 Windows 编译**（2026-11-04 已推翻：适配后经 MSVC 编译通过，见第 9 节）：依赖 `dirent.h`、`poll.h`、
   `fork`、`socketpair` 等 POSIX 设施；Windows 侧只能做「编译库」门禁，
   运行期测试待移植（这是当前最大的验证缺口）。

## 8. 验证状态（如实）

| 验证项 | 状态 | 依据 |
|---|---|---|
| Linux 分支回归 | ✅ 已做 | WSL `g++ -std=c++17 -Iinclude -c src/rd_net/{socket,event_loop,protocol}.cpp -Wall -Wextra -Wpedantic` 三个编译单元零告警、零错误 |
| Windows 分支语法/语义 | ✅ 已做（2026-11-04） | 真 MSVC 2022 x64（`vcvars64.bat`）以 `/W4 /WX` 编译四个测试 rc=0；此前只能静态审查，因为 WSL 无 mingw-w64/wine |
| Windows 真实构建 | ⏳ 待 CI | `.github/workflows/ci.yml` 的 `windows-configure` job 升级为 `cmake --build --target go2cpp_rd_net`；首次运行前保留 `continue-on-error` |
| Windows 运行期行为 | ✅ 已做（2026-11-04） | 真 MSVC 2022 x64 编译并运行四个测试：406/44/44/64 全 0 失败，9 项差异见第 9 节 |
| Windows cmake/CI 路径 | ⏳ 待首次运行 | 本机用 `cl` 直接编译（`E:\tmp\msvc-build\*.bat`），仓库内 `build_and_run_windows.bat` / CMake 未走通（acceptance:370 遗留） |

结论：第 1–7 节是实现取舍与降级清单（仍适用于实现层）；**Windows 运行期已实测**，
证据见第 9 节。Linux 侧回归以 `tests/rd_net/build_and_run_*.sh` 为准；cmake/CI 的
Windows job 尚未首次运行（acceptance:370 记为遗留）。

---

## 9. rd_net 测试平台差异（REQ-010，2026-11-04）

> 背景：2026-11-04 用**真 MSVC 2022 BuildTools x64** 编译并运行 `tests/rd_net/`
> 四个测试（`test_socket` 406、`test_event_loop` 44、`test_protocol` 44、
> `test_reliability` 64，全 0 失败），见 `collab/acceptance.md:346-370`。为消除
> MSVC 下的编译错误与运行期断言差异，共处理 **9 项**平台差异；本节逐项登记
> 「Linux 行为 / Windows 行为 / 处理 / 等价性 / 验证位置」，并把 acceptance 的
> 5 行归因拆成 9 个可独立核查的点（拆分映射见 9.1 末注）。

### 9.1 差异清单（9/9）

| # | 差异项 | Linux 行为 | Windows 行为 | 处理（实现/断言） | 等价性 | 验证位置（file:line） |
|---|---|---|---|---|---|---|
| 1 | UDP 截断长度 | `recvmsg(MSG_TRUNC)` 返回**真实报文长度** 65507，断言 `got==65507` | `WSARecvFrom` 只报 `WSAEMSGSIZE`，内核不交真实长度；实现只能把 `*received` 记为缓冲大小 | `_WIN32` 断言 `got == sizeof(small)`（=1024）；POSIX 分支保留 65507。实现侧 `RecvDatagramRaw` 在 `WSAEMSGSIZE` 时 `*received = buf.size` | 截断态 `received` 必须**恰等于**调用方缓冲：不谎报 0（不丢「有数据」信号）、不超过缓冲（不被当完整报文）；首尾字节仍与实发 payload 比对 | `tests/rd_net/test_socket.cpp:564-574`（断言 :571）；实现 `src/rd_net/socket.cpp:1378-1380` |
| 2 | TCP 批量提交：`kOk` 态 | `sendmsg`/`writev` 一次提交受 `SO_SNDBUF` 硬约束，对端不读时必为部分提交 → 只出现 `kWouldBlock` | `WSASend` 把 `SO_SNDBUF` 当**提示**，AFD 可整吞一次 64KB gather 写 → 额外出现 `kOk` | 允许两态；`kOk` 断言 `submitted==64 && transferred==65536`，随后按 `transferred` 逐字节回读比对；**不伪造部分提交**（内核已全收时谎报会导致重发重复） | `kOk` 是「全部 64 段已提交」的合法实现结果，与 Linux 的「全量提交」同一语义；字节内容比对不放宽 | `test_socket.cpp:1627-1632`（`kOk` 分支）；实现 `src/rd_net/socket.cpp:2038` |
| 3 | TCP 批量提交：`kWouldBlock` 态 | 前缀提交：`1<=K<64` 段完整，`transferred` 落在第 K 段内 | 同样可能发生（内核先收一部分再阻塞） | 保留原断言：`submitted>0 && submitted<64`、`transferred∈[K*seg,(K+1)*seg)`，逐字节回读前缀 | 与 Linux 完全同一组断言，未放宽；段边界（写入原子性）仍被检查 | `test_socket.cpp:1633-1643`（`kWouldBlock` 分支） |
| 4 | 事件派发时序：Test1 同 tick | `poll` 一次返回全部就绪 fd，Post/定时器/IO **同 tick**（`io_tick==1`） | `WSAEventSelect`+`WaitForMultipleObjectsEx` 一次只报索引最小的句柄；唤醒事件在 `handles[0]`，IO 顺延下一 tick | 断言 `handler.io_tick == post_tick + 1`（严格晚一 tick、不能更晚）；顺序 `0→1→2` 不变 | 「Post 先于定时器先于 IO」不变，只允许 IO 晚**一个** tick；多晚一个 tick 即失败，不是恒真 | `tests/rd_net/test_event_loop.cpp:247-256`（断言 :253）；实现 `src/rd_net/event_loop.cpp:1122-1137` |
| 5 | 事件派发时序：Test5 first_tick | A/B 同 tick 派发，`B.first_tick == A.first_tick` | 一次只派发一个句柄；A（索引小）先按 64/tick 排空 1000 包（16 tick），B 的 1 包之后才派发 | 断言 `B.first_tick > A.first_tick`；其余不变：`A.first_tick_count==64`、`A.max_per_tick<=64`、`B.count==1`、`A.count>=64` | 两 socket 都**没有被饿死**（B 仍恰好派发 1 次、A 仍收满且每 tick ≤64）；只改「同 tick」为「严格晚」，不放宽单 tick 预算 | `test_event_loop.cpp:565-573`（断言 :570） |
| 6 | Stop 条件根因 | A 收满即 `Stop()` 安全——同一 tick 内 B 也已派发 | 同 tick 只派发 A，A 收满即停会让 B 的 1 包**永远没有机会**（表现：`b_rx=0`/`timed_out`） | 改为两 socket 都收满才停：`handler_a.on_packet` 增加 `handler_b.count>=1` 条件，`handler_b.on_packet` 在 A 满时停 | Stop 条件从「A 完成」改为「A+B 都完成」，不放宽任何计数断言；`B.count==1`、`timed_out==false` 仍检查 | `test_event_loop.cpp:531-544`（注释 :531-533；acceptance 记作 `:517/:518`，行号随提交漂移） |
| 7 | 定时精度 | `poll` 毫秒超时 + 1ms 内核粒度，1000 定时器 `Max()<5000µs` | 系统时钟中断默认 15.625ms，`WaitForMultipleObjectsEx` 最坏晚一个时钟周期（未调 `timeBeginPeriod`，避免全局副作用） | 仅 `_WIN32` 阈值放宽到 `Max()<20000`；保留 `elapsed∈[990,1100]ms` | 误差上界从 5ms 变为 2 个时钟周期；「1000 个定时器整体在 1s 附近跑完」不变（退化成秒级立即失败）；Test3 的 `stats.Max()<5000` **未放宽**作对照 | `test_event_loop.cpp:301-310`（断言 :307）；对照 `:424` |
| 8 | UDP 吞吐 | 环回 500ms 窗口内可发 >10 万包，断言 `sent>=100000` | 环回每包一次 `sendto` + 发送线程与 loop 线程抢核，实测 3~8 万包 | 阈值降为 `sent>=30000`（仍 ≥60 包/ms） | 仍能抓「发送线程没起来 / 出错提前退出 / 卡死」：这些情况 `sent` 停在原位，30000 也过不去 | `test_event_loop.cpp:425-433`（断言 :430）；实测区间依据 commit `4cc501a` 注释 |
| 9 | UDP 发送临时错误 | 缓冲满时 `sendto` 接受并丢弃，只有 `EWOULDBLOCK` 重试；其他错误 `break` | 环回 UDP 缓冲满可能返回 `WSAENOBUFS` 等非 `kOk`；原逻辑 `break` 会让发送线程提前退出、`sent` 断言失真 | `_WIN32` 下非 `kOk` 一律 `yield`+`continue` 重试（窗口由 `stop_sender` 截止）；POSIX 分支保持原逻辑 | 用「窗口内持续成功发送 ≥30000」兜底；若 socket 真损坏，`sent` 停住仍失败，**不会静默放过** | `test_event_loop.cpp:362-380`（重试 :368-371） |

**拆分映射（acceptance 5 行 → 本节 9 项）**：`socket:564`→#1；`socket:1608/1610`
两态→#2+#3；`event_loop:247/517/518` 三件事→#4（同 tick 派发）+ #6（Stop 条件根因）
+ #5（first_tick 语义）；`event_loop:292` 定时精度→#7；`event_loop:395` UDP 吞吐→#8，
其隐含的发送错误处理→#9。acceptance 表格中的行号为提交时快照，当前 HEAD 见上表。

### 9.2 MSVC 编译期兼容修复（3 处，不属运行期语义差异）

| 错误 | 原因 | 修复 | 位置 |
|---|---|---|---|
| `C3493` | lambda 隐式捕获 `kTimerCount`；MSVC 拒绝对 constexpr 局部变量做隐式捕获 | 显式列入捕获列表 `[&stats, &fired, &loop, expected, kTimerCount]` | `test_event_loop.cpp:282` |
| `C2589/C2059` | `windows.h` 的 `min/max` 宏破坏 `std::min/std::max` | `NOMINMAX` 在包含 `windows.h` 前定义；两处 `std::min` 改 `(std::min)` 抑制宏展开 | `test_socket.cpp:40-44`、`:656`、`:685` |
| `C4244` | `SOCKET`（`UINT_PTR`，64 位）塞进 `int` | 测试侧统一 `tsock::Handle = std::uintptr_t`，`const int peer` → `const tsock::Handle peer` | `test_socket.cpp:96`、`:434`、`:474`、`:614` |

### 9.3 MSVC 原始命令与输出

构建脚本在 `E:\tmp\msvc-build\`（**展平副本**：`include/`、`src/`、`tests/`，
源文件与仓库 HEAD 同源；用 `cl` 直接编译，**未走 cmake**）：

```bat
call "...\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /std:c++17 /W4 /WX /utf-8 /EHsc /D_WIN32_WINNT=0x0601 ^
   /D_CRT_SECURE_NO_WARNINGS /I include ^
   tests/test_socket.cpp src/socket.cpp ^
   /Fe:test_socket.exe /link ws2_32.lib psapi.lib

cl ... /I include tests/test_event_loop.cpp src/socket.cpp src/event_loop.cpp ^
   /Fe:test_event_loop.exe /link ws2_32.lib psapi.lib

cl ... /I include tests/test_protocol.cpp src/socket.cpp src/event_loop.cpp ^
   src/protocol.cpp /Fe:test_protocol.exe /link ws2_32.lib psapi.lib

cl ... /I include tests/test_reliability.cpp ^
   /Fe:test_reliability.exe /link ws2_32.lib psapi.lib
```

运行结果（原始记录见 `collab/acceptance.md:352-357`）：

```text
test_socket      编译 rc=0 → 运行 checks=406 failures=0
test_event_loop  编译 rc=0 → 运行 checks=44  failures=0
test_protocol    编译 rc=0 → 运行 checks=44  failures=0
test_reliability 编译 rc=0 → 运行 检查 64 项，失败 0 项
```

**证据边界（如实）**：`E:\tmp\msvc-build` 未保留 stdout 日志文件，只留下
`.bat` 与 `.obj/.exe` 产物（exe 时间戳 `2026-11-04 13:05:53`–`13:06:00`，与
提交 `4cc501a` 的 13:06:19 吻合）；上表运行行为验收记录，**不是**本节现场复跑。
`tests/rd_net/build_and_run_windows.bat` 与 `tests/rd_net/CMakeLists.txt` 的 cmake
路径未在本机走通（acceptance:370 遗留），CI 的 Windows job 尚未首次运行。

### 9.4 长期规则（自本节起强制）

1. **新增平台分支必须登记到本文件**：任何 `#if defined(_WIN32)` 新增/修改，
   都要在 9.1 表格加一行并给 `file:line`；未登记视为该改动未完成。
2. **断言只能改期望值/阈值，不能改语义**：禁止把「必须发生」退化为「可能发生」
   后不再验证另一半；允许两态（如 `kOk`/`kWouldBlock`）时必须**两态各自**给出
   断言，且两态都要做内容比对。
3. **不允许 skip**：不得用 `#if 0`、提前 `return`、空断言、恒真断言绕过平台差异；
   确实无法验证的项写「未确认」并给出替代断言，不得删除原断言。
4. **阈值放宽必须自证**：写清实测区间（如 `3~8 万包`）与放宽后仍能抓住的失败模式
   （如「线程未启动仍会让 `sent>=30000` 失败」）；只写「Windows 比较慢」不合格。
5. **行号不是契约**：acceptance/本文中的 `:行号` 会随提交漂移；引用时以
   `git log -S<断言文本>` 或提交 hash 定位，并同步更新本表。
6. **平台差异只允许出现在 `#if defined(_WIN32)` 块内**，块内注释必须写清
   「Linux 行为 / Windows 行为 / 等价性」三要素，否则 review 直接打回。
