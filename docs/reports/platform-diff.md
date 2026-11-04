# rd_net 平台差异：Linux/POSIX 与 Windows（MSVC/Winsock）

- 状态：Windows 分支已实现（`src/rd_net/socket.cpp`、`src/rd_net/event_loop.cpp`
  的 `#if defined(_WIN32)` 段），**尚未在 Windows 上实测编译**（见第 8 节）。
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
9. **`tests/rd_net/*.cpp` 不可在 Windows 编译**：依赖 `dirent.h`、`poll.h`、
   `fork`、`socketpair` 等 POSIX 设施；Windows 侧只能做「编译库」门禁，
   运行期测试待移植（这是当前最大的验证缺口）。

## 8. 验证状态（如实）

| 验证项 | 状态 | 依据 |
|---|---|---|
| Linux 分支回归 | ✅ 已做 | WSL `g++ -std=c++17 -Iinclude -c src/rd_net/{socket,event_loop,protocol}.cpp -Wall -Wextra -Wpedantic` 三个编译单元零告警、零错误 |
| Windows 分支语法/语义 | ⚠️ **未在本机编译** | WSL 无 mingw-w64、无 wine、无外网（装不了）；本机无法交叉编译 MSVC。只能靠静态审查 + CI |
| Windows 真实构建 | ⏳ 待 CI | `.github/workflows/ci.yml` 的 `windows-configure` job 升级为 `cmake --build --target go2cpp_rd_net`；首次运行前保留 `continue-on-error` |
| Windows 运行期行为 | ❌ 未覆盖 | 无 MSVC 运行环境、无 Windows 测试用例；`WaitForMultipleObjects` 路径的正确性依赖 CI 构建 + 后续补测试 |

结论：**不要**把本文件当作「Windows 已验证通过」的证据；它描述的是实现
与设计取舍。Windows 侧的验收需要 CI 构建通过后再补一套可移植的测试
（把 `tests/rd_net` 里依赖 POSIX 的部分改成条件编译或独立用例）。
