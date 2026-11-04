# network 模块心智负担审查

- 审查对象：`/UserData/CodexWorkSpace/Go2cpp`（WSL 真树）的 `include/go2cpp/network/` + `src/network/`
- 审查方式：**只读**静态审查（未改任何代码、未 commit），逐条给 `file:line`
- 审查视角：用户在测试里能过，但**实际开发会踩**的默认值/隐式行为；目标是「心智负担最小」
- 时间盒：35 分钟（实际约 32 分钟）

---

## 0. 结论速览

**Socket 是否阻塞 M？——分三种情况，默认情况不阻塞，但退化路径会阻塞且不可见：**

| 执行上下文 | 判定依据 | `Socket::recv/send` 的行为 | 是否阻塞 M |
|---|---|---|---|
| IOManager 的受管 Fiber | `io::IOManager::current() != nullptr`（socket.cpp:765） | `wait_for` → **park Fiber** | 否 |
| 裸 `Scheduler` 的 Fiber（全局单例 go()，未挂 IOManager） | `Scheduler::current_scheduler()` 有值但 `io_manager()` 为空（io.cpp:3283-3291） | 走 `poll_wait`（socket.cpp:799-814）→ **阻塞整个 OS 线程** | **是** |
| 普通 std::thread | 同上，`current()` 返回空 | `poll_wait` | **是** |

关键：上表第二行是**最容易踩**的路径——用户按「Scheduler 默认全局单例」写代码（正确用法），但只有 `IOManager` 才提供 epoll 等待；此时 Socket 沉默退化为 `poll()`，而且**没有包 `BlockingRegion`**，sysmon 看不见，不会补 M（对比 hook 路径 hook.cpp:1176-1179 会显式包）。默认 `timeout = Duration::max()`（socket.hpp:135-158），于是 `poll(-1)` 无限期挂住该 worker 上的**所有** Fiber。

> 一句话：**「Socket 默认非阻塞」只在 IOManager 内成立；它默认的是「无限等待」，而且等法随上下文静默切换。**

---

## 1. 危险点清单（按严重度排序）

> 每条：级别 / 位置 / 症状 / 为什么会踩 / 改造建议

### S 级 · 高

**H-01 [高] Socket 在「裸 Scheduler Fiber / 普通线程」中默认无限期阻塞 M，且 sysmon 不可见**
- 位置：`src/network/socket.cpp:765`、`src/network/socket.cpp:799-814`、`src/network/io.cpp:3283-3291`、`include/go2cpp/network/socket.hpp:135-158`
- 症状：在全局 Scheduler 的 `go()` 里写 `sock->recv(buf, n)`，该 worker 上几十个 Fiber 全部停摆；没有超时、没有日志、gdb 里只看到 `poll`。
- 为什么会踩：等待策略由「当前有没有 IOManager」决定，而不是由「当前是否在 Fiber 中」决定。用户以为自己用了 Fiber 就该 park；实际上裸 Scheduler 没有 I/O 后端。并且退化路径缺少 `Scheduler::BlockingRegion`，调度器无法识别「这个 M 被系统调用占住了」（hook 的同类路径是有的：hook.cpp:1176-1179、1192-1199）。
- 改造建议：`wait_ready()` 的 poll 分支加 `go2cpp::BlockingRegion`（一行即可让 sysmon 补 M）；退化时打一次性 WARN；长期方案见 §3 的「显式后端」改造。

**H-02 [高] 超时参数 `0` 的语义是「立即超时」，不是「不限/清除」**
- 位置：`src/network/socket.cpp:820`、`src/network/socket.cpp:789`、`src/network/socket.cpp:844`、`src/network/socket.cpp:869`
- 症状：为了「清除超时」写 `sock->set_recv_timeout(0ms)` 或 `recv(..., 0ms)`，结果每次读立刻返回 `ETIMEDOUT`，连接看起来「秒断」。
- 为什么会踩：与两个最常被类比的系统相反——POSIX `SO_RCVTIMEO = {0,0}` 表示**永不超时**；Go 的 `SetReadDeadline(time.Time{})` 也是**清除**。我们的 `0` 是「预算已耗尽」。而且 `Duration::max()` 才表示无限，这个三态（0 / 有限 / max）没有类型保护。
- 改造建议：`set_recv_timeout` 增加 `ClearTimeout()` 或接受 `std::optional<Duration>`（`nullopt` = 无限）；`0` 改为非法值返回 false；文档在头文件显式写「0 = 立即超时」。

**H-03 [高] `Duration::max()` 不是「无限」，会被 Socket 级配置静默覆盖**
- 位置：`src/network/socket.cpp:888-890`、`src/network/socket.cpp:906-908`、`include/go2cpp/network/socket.hpp:266-269`
- 症状：`recv(buf, n)`（默认 max）本来想无限等，但之前某处调过 `set_recv_timeout(5s)`，于是 5 秒后 `ETIMEDOUT`；调用点看不出来。
- 为什么会踩：「默认参数 = 无限」是大多数人的第一直觉，但实现里 `max` 是**哨兵**，表示「回落到 socket 配置」，只有 socket 配置也是 max 才是无限。这层回落不可见，且 `set_recv_timeout` 是**持久状态**，一次设置影响之后所有调用。
- 改造建议：去掉回落，让 `max` 真正等于无限；要配置默认超时就放进 `SocketConfig`（见 §3），调用点显式传。或者在 API 上区分 `recv()` 与 `recv_with_timeout()`。

**H-04 [高] `Address::LookupAny` 的阻塞 DNS 会卡住整个 M**
- 位置：`src/network/address.cpp:134-136`（`::getaddrinfo` 直接调用）、`src/network/address.cpp:153-156`、`src/network/hook.cpp:183-240`（`Originals` 里**没有** `getaddrinfo`）
- 症状：在 Fiber 里 `DialTCP("api.example.com", 443)`，DNS 慢时整个 worker 卡 5–20 秒（`/etc/resolv.conf` 默认 5s × 2 次 × 2 个 server），期间该 M 上所有 Fiber 停摆。
- 为什么会踩：hook 只拦截 fd 就绪类调用，`getaddrinfo` 是**非 fd 阻塞**，既不在 `Originals` 表里，也没有被 `BlockingRegion` 包裹（hook.cpp:24 的注释把它列为 BlockingRegion 场景，但**代码里没有对应入口**）。因此 sysmon 完全看不见。
- 改造建议：`Lookup`/`LookupAny` 入口包 `BlockingRegion`（最小改动）；中期把 DNS 放到专用线程池 + `ParkingCondition` 异步化；`LookupAny` 增加 `timeout` 参数。

**H-05 [高] `HttpServer::serve_connection` 不设任何超时，慢连接永久占用**
- 位置：`include/go2cpp/network/http.hpp:570-577`（头文件自己写明「不设置 recv/write 超时（走无限等待）」）、`include/go2cpp/network/socket.hpp:147-150`
- 症状：一个只发半个请求头的客户端可以让 handler 永久挂住；1000 个慢连接 = 1000 个 Fiber 常驻 + 1000 个 socket；压测时表现为「连接数只涨不降」。
- 为什么会踩：`Client` 有 30s 默认超时（http.hpp:358），`Server` 没有——同一模块内默认值不一致，用户按 Client 的经验推理 Server 必然错。
- 改造建议：`Server` 增加 `ServerOptions{ read_timeout, write_timeout, idle_timeout, max_requests }`，默认 `read_timeout = 30s`、`idle = 60s`；`serve_connection` 内用绝对 deadline 而不是每次调用重置。

**H-06 [高] `Socket::send/recv` 是「单次系统调用」语义，部分成功即返回**
- 位置：`src/network/socket.cpp:835-836`、`src/network/socket.cpp:860-861`、`include/go2cpp/network/socket.hpp:147-158`
- 症状：`sock->send(buf, 1MB)` 返回 `transferred = 219KB`，`ok()` 仍为 true（socket.hpp:37）；调用方以为整块发完，对端只收到 219KB，协议错位。
- 为什么会踩：Go 的 `net.Conn.Write` 契约是「写满或报错」，用户从 Go 迁过来会默认同样的语义。我们的 `ok()` 只判断 `transferred >= 0`，把「部分写」和「写完」混为一谈。
- 改造建议：新增 `SendAll/RecvAll`（内部循环到满足长度）作为默认推荐；把 `ok()` 改名/拆成 `success()` 与 `complete(expected)`；`SocketStream::write_all` 已有正确实现（socket.hpp:309），把它提升为首选 API。

**H-07 [高·待确认] hook 的 `connect` 默认 5s 会截断 `Socket::connect` 的显式超时**
- 位置：`src/network/hook.cpp:348`（`s_connect_timeout_ms{5000}`）、`include/go2cpp/network/hook.hpp:83-86`、`src/network/socket.cpp:620`
- 症状：`sock->connect(addr, 60s)` 在慢网络下 5 秒就返回失败；或者反过来，`connect(addr)`（默认无限）被 5 秒截断，用户找不到配置来源。
- 为什么会踩：`::connect` 被 hook 接管后走 `cooperative_connect`，它用**进程级** hook 配置，而 `Socket` 自己另有一套 `Duration` 预算。两套超时叠加，谁生效取决于 fd 是否已被 adopt，属于隐式状态机。
- 改造建议：`Socket::connect` 用 `connect_f`（原始版本，hook.hpp:369-370）绕开 hook，只由 Socket 的超时控制；或让 hook 读 `FdManager` 的 timeout 而不是全局 5s。**本条需运行时验证 hook 的实际优先级。**

**H-08 [高·待确认] hook 接管后可能 park，导致 `Socket` 的 `Duration` 超时完全失效**
- 位置：`src/network/socket.cpp:860`（`::recvmsg` **不带** `MSG_DONTWAIT`）、`src/network/hook.cpp:386-390`（`can_wait` 三元表）、`src/network/socket.cpp:378`
- 症状：显式 `recv(buf, n, 0, 2s)` 在 hook 启用时可能永远不返回——hook 在 `can_wait=true` 时直接 park，而不是把 `EAGAIN` 还给 Socket；Socket 的 `wait_ready` 根本没机会执行。
- 为什么会踩：`Socket` 依赖「fd 是 O_NONBLOCK，所以 syscall 立刻返回 EAGAIN」来接管等待；但 hook 把 `O_NONBLOCK` 解释为自己的「system nonblocking」并 park。两套等待机制叠加，胜负由 adopt 时机与 `m_user_nonblocking` 归因决定，调用方完全不可见。
- 改造建议：`send_impl/recv_impl` 显式加 `MSG_DONTWAIT`（内核层面强制非阻塞，hook 的 `per_call_nonblocking` 会尊重它，hook.cpp:1204），从根上消除两种等待的竞争；同时加一条集成测试「hook 开启 + 2s 超时必须 2s 返回」。**需运行时确认当前实际行为。**

### A 级 · 中

**M-01 [中] WebSocket 的 `io_timeout` 默认无限，半开连接永久挂住**
- 位置：`include/go2cpp/network/websocket.hpp:90`、`src/network/websocket.cpp:585-593`
- 症状：对端掉线（无 FIN）时 `ReadMessage()` 永远不返回，Fiber 与 16KiB 缓冲永久占用。
- 为什么会踩：`io_timeout` 是 `Socket::Duration::max()`；而 WebSocket 场景「对端不客气地消失」是常态，必须有读超时或心跳。默认无限 = 默认不可用。
- 改造建议：默认 30s；同时默认启用 ping/pong 心跳（现在 `auto_pong=true` 只被动回，不主动发）。

**M-02 [中] WebSocket 预取缓冲 16KiB/连接，与「刚优化到 1KiB」的认知不一致**
- 位置：`include/go2cpp/network/websocket.hpp:99-103`（实际值 `prefetch_bytes{16 * 1024}`）、`src/network/websocket.cpp:572-593`
- 症状：10 万长连接常驻约 1.6GB 读缓冲；如果目标是小消息/心跳场景，这是纯浪费。
- 为什么会踩：注释自己写「小消息/心跳为主的连接 4KiB 足够」，但默认给了 16KiB；用户记忆里的 1KiB 与代码不符。默认值按「大消息优化」而不是按「常见场景」。
- 改造建议：默认降到 4KiB（或 1KiB，与用户记忆对齐）；按 `max_frame_payload` 自适应；在 `Connect()` 时打印一次实际值。**「1KiB」是否曾存在过需查 git 历史（待确认）。**

**M-03 [中] HTTP body 上限 64MiB / header 64KiB，超限后的响应语义不明确**
- 位置：`include/go2cpp/network/http.hpp:206-211`、`src/network/http.cpp:407`、`450-452`、`486-489`、`641-642`
- 症状：客户端发 100MiB body，解析器返回 `Error "body too large"`；server 是否回 413、是否关闭连接、连接能否复用，取决于 `serve_connection` 的错误分支（未逐行确认）。
- 为什么会踩：64MiB 是「防恶意」上限，但业务传文件很容易超；超限后没有明确的 `413` 保证，用户只能看到连接被断。
- 改造建议：`ParserLimits` 进 `ServerOptions` 并暴露；超限时**明确**回 `413 Payload Too Large` + `Connection: close`；错误字符串常量化便于匹配。

**M-04 [中] HTTP Client 每次请求新建连接，没有连接池**
- 位置：`include/go2cpp/network/http.hpp:344-356`（注释：`每次 do_request() 自行建连（不复用连接池）`）、`http.hpp:358`
- 症状：QPS 一高就 TIME_WAIT 堆积 / 端口耗尽；每次请求多一次 TCP + 一次 DNS（叠加 H-04）。
- 为什么会踩：Go 的 `http.Client` 默认复用连接，用户按 Go 经验写循环请求必然踩。
- 改造建议：`Client` 内部加 per-host 连接池（`keep-alive` 已经支持，只差池化）；或至少在类名/文档里写「短连接客户端」。

**M-05 [中] hook 有一批「静默不生效」的场景，且没有统一诊断**
- 位置：`src/network/hook.cpp:366-372`（`cooperative_manager()` 在无 Fiber/无 task/hook 关闭时返回 nullptr → 原生阻塞）、`hook.cpp:183-240`（`Originals` 只覆盖 read/write/recv/send/connect/accept 等）、`include/go2cpp/network/hook.hpp:344-349`
- 症状：以下情况「看起来 hook 开了，实际走原生阻塞」：①静态链接（`dlsym(RTLD_NEXT)` 拿不到）；②直接 `syscall(SYS_read)`；③io_uring / 第三方自带 epoll 的库；④`getaddrinfo`/`fsync` 等非 fd 调用；⑤线程被 `thread_policy` 禁止 hook（hook.cpp:351-356）；⑥`*_f` 接口；⑦`SetOpEnabled(false)`。
- 为什么会踩：hook 是「进程级 + 运行时」的，编译期完全不可见；失效时没有日志、没有计数器，只表现为「偶尔卡一下」。
- 改造建议：提供 `hook::Diagnose()` 返回「本进程接管率 / 最近 N 次未接管原因」；对 `getaddrinfo`、`fsync` 这类高频阻塞调用至少加一次性 WARN。

**M-06 [中] `SetOpEnabled` 是进程级全局开关，改一处影响所有线程**
- 位置：`include/go2cpp/network/hook.hpp:210-235`（文档自己标了 `@warning`）
- 症状：为了「这条日志不 hook」调用 `SetOpEnabled(kWrite, false)`，忘了恢复，之后所有 socket 写都变阻塞；或者只关 read 不关 write，同一连接一半 park 一半真阻塞。
- 为什么会踩：`boolean` 全局开关没有 RAII/作用域；而且「已 park 的 Fiber 不受影响」的语义很反直觉（hook.hpp:164-171）。
- 改造建议：删掉或 `[[deprecated]]`；只保留 `*_f` 与作用域守卫 `ScopedHookOp`（现有 `ScopedEnable` 只做总闸，hook.hpp:257-271）。

**M-07 [中] `ResponseWriter` 默认写超时无限**
- 位置：`include/go2cpp/network/http.hpp:398-402`
- 症状：客户端不读响应（只发请求就挂起），server 的 `write()` 永久阻塞在 send buffer 满。
- 为什么会踩：与 `Client` 的 30s 默认不一致；SSE 场景下更是常态（`sse.hpp` 复用它）。
- 改造建议：默认 30s，SSE 显式覆盖为 max。

**M-08 [中] `SocketResult::ok()` 把「部分成功」判为成功**
- 位置：`include/go2cpp/network/socket.hpp:34-38`
- 症状：同 H-06，`if (result.ok())` 分支里直接推进缓冲区指针，实际只写了一部分。
- 为什么会踩：`ok()` 的名字暗示「完全成功」，实现只判断 `transferred >= 0 && error == 0`。
- 改造建议：`ok()` 语义收窄为「没有错误」并改名 `no_error()`；另提供 `complete(size_t expected)`。

**M-09 [中] `Socket::close()` 可能返回 false 并转为延迟关闭**
- 位置：`include/go2cpp/network/socket.hpp:138-143`、`src/network/socket.cpp:390-400`
- 症状：`if (!sock->close()) { /* 以为失败了 */ }`，实际 fd 会在最后一个在途操作结束时关闭；调用方若此时复用 fd 号会踩 ABA。
- 为什么会踩：`bool` 返回值把「已关闭」和「无法立即 park」混在一起；文档只有一句注释。
- 改造建议：返回三态 `enum class CloseResult { Closed, Deferred, Failed }`；`Deferred` 时禁止调用方继续使用对象（现有 `m_closing_flag` 已做到，但 API 没表达）。

### B 级 · 低

**L-01 [低] `Socket::fd()` 是瞬时值，跨操作持有会 ABA**
- 位置：`include/go2cpp/network/socket.hpp:110-117`
- 症状：缓存 `fd` 后做 `::write(fd, ...)`，期间 socket 被 close + fd 号复用，写到别的连接上。
- 为什么会踩：`fd()` 太像普通 getter；注释里写了「不应跨操作持有」，但类型上没有约束。
- 改造建议：返回一个带代际的 `FdSnapshot`，或只允许在 `begin_operation` 作用域内使用。

**L-02 [低] `AI_ADDRCONFIG` 会按本机网卡过滤地址族**
- 位置：`src/network/address.cpp:129`
- 症状：CI 容器里没有 IPv6 网卡，`Lookup("::1")` 返回空，测试在本地过、在容器挂。
- 为什么会踩：这是 getaddrinfo 的隐藏行为，与「纯 DNS 查询」不同（address.cpp:21-23 注释已提）。
- 改造建议：`Lookup` 增加 `flags` 参数；默认保留，测试里显式关掉。

**L-03 [低] DNS 失败只打 WARN，错误码丢失**
- 位置：`src/network/address.cpp:137-143`
- 症状：`DialTCP` 失败只返回空指针，调用方无法区分「域名不存在」「超时」「无网络」。
- 为什么会踩：`Lookup` 返回 `bool`，`gai_strerror` 只进了日志。
- 改造建议：增加 `Result<AddressPtr>` 或 out 参数 `int* gai_error`。

**L-04 [低] `EINTR` 重试上限 3 次，超限后把 EINTR 暴露给调用方**
- 位置：`include/go2cpp/network/hook.hpp:100`、`hook.hpp:119-123`
- 症状：高频信号（profiler、GC 线程）下偶发 `EINTR` 穿透到业务，按错误处理导致误判连接断开。
- 为什么会踩：上限是硬编码 3；大多数业务代码没写 `EINTR` 分支。
- 改造建议：数据面调用改为无限重试（只在 `connect` 保留上限）；或提供 `errno == EINTR` 的统一重试宏。

**L-05 [低] `shutdown` 从未被 hook，但配置表里有 `hook.enable_shutdown`**
- 位置：`include/go2cpp/network/hook.hpp:344-349`
- 症状：改这个配置没有任何效果，排查时浪费时间。
- 改造建议：删除配置项，或实现 hook。

**L-06 [低] `FdContext` 读取无同步，并发写构成数据竞争（UB）**
- 位置：`include/go2cpp/network/fd_manager.hpp:18-25`（`@warning` 自己写明）
- 症状：一个线程 `set_timeout`，另一个线程 `recv` 读 `m_recv_timeout`，TSan 报 race；理论上可读到撕裂值。
- 改造建议：字段改 `std::atomic`（bool/ms 都是平凡类型），成本极低。

**L-07 [低] 100ms 分片等待：空闲长连接每 100ms 被唤醒一次**
- 位置：`src/network/socket.cpp:768-771`、`src/network/socket.cpp:801-803`
- 症状：10 万空闲连接 = 每秒 100 万次定时器唤醒，CPU 空转；profile 里全是 `wait_for` 超时返回。
- 为什么会踩：分片是为了让 `close()` 能打断无限等待，但代价是「无限等待变成 100ms 轮询」。
- 改造建议：`close()` 走现有 `cancel_read/write`（socket.hpp:212-215）+ `ParkingCondition` 唤醒，等待本身用真正的无限 deadline。

---

## 2. Go 对照

| 能力 | Go 的默认语义 | 我们的默认语义 | 差异风险 |
|---|---|---|---|
| 阻塞读 | `net.Conn.Read` 阻塞当前 goroutine，runtime 把 M 交出去 | IOManager Fiber park；**裸 Scheduler/普通线程退化为 poll 阻塞 M** | 高（H-01） |
| 超时 | `SetDeadline(绝对时间)`，零值 = 清除 | `Duration` 相对值；`0` = 立即超时；`max` = 回落配置 | 高（H-02/H-03） |
| 写完整性 | `Write` 写满或返回错误 | 单次 `sendmsg`，部分成功即返回 | 高（H-06） |
| DNS | `net.DefaultResolver` 在独立 goroutine，不阻塞 M | `getaddrinfo` 直接调用，阻塞 M | 高（H-04） |
| HTTP Client 超时 | 默认 0（无限） | 默认 30s | 中（方向相反：我们更安全，但 Go 用户会以为无限） |
| HTTP Server 超时 | `ReadTimeout/WriteTimeout/IdleTimeout` 默认 0（无限） | 同样无限，且明确不设 | 高（H-05） |
| WebSocket | `gorilla/websocket` 无默认帧限制，读超时需显式设 | 16MiB 帧 / 64MiB 消息 / 读超时无限 | 中（M-01/M-02） |
| 错误检查 | `error` 必须显式处理（可被 `_` 丢弃） | `SocketResult` 可被忽略，`ok()` 语义宽 | 中（M-08） |

**核心差异**：Go 用「goroutine + netpoll」把「阻塞语义」和「不阻塞 M」统一了，调用方不需要知道底层；我们用「Socket 自己做等待 + hook 再做一次等待」，两条路径的判定依据不同（IOManager 是否存在 / fd 是否被 adopt），所以**同一个 `recv()` 在不同上下文行为不同**。这就是心智负担的根源。

---

## 3. 「心智负担最小」改造建议

### 3.1 默认值

| 项 | 现状 | 建议默认 | 理由 |
|---|---|---|---|
| Socket 读/写超时 | `max`（无限） | `30s`（可 `ClearTimeout()`） | 默认无限 = 默认会挂死；Go 的「默认无限」有 runtime 兜底，我们没有 |
| `0` 的语义 | 立即超时 | **非法**（返回 false） | 消除三态歧义 |
| `max` 的语义 | 回落 Socket 配置 | **真正的无限** | 哨兵语义不可见 |
| HttpServer 读/空闲超时 | 无限 | `30s / 60s` | 慢连接防护 |
| WebSocket `io_timeout` | 无限 | `30s` + 主动心跳 | 半开连接 |
| WebSocket `prefetch_bytes` | 16KiB | 4KiB（或 1KiB） | 长连接内存主项 |
| hook connect 超时 | 5000ms（全局） | 由 `Socket` 传入，hook 不设默认 | 两套超时叠加 |
| DNS | 无超时 | `5s` + 异步 | 卡 M |

### 3.2 API 形状

1. **`SocketConfig` 结构体**（用户明确点名）：
   ```cpp
   struct SocketConfig {
       bool blocking = false;              // 默认非阻塞，Go 语义
       std::optional<Duration> recv_timeout;  // nullopt = 无限
       std::optional<Duration> send_timeout;
       bool tcp_nodelay = true;
       std::optional<Duration> connect_timeout;
   };
   Socket::Ptr CreateTCP(const SocketConfig& cfg = {});
   ```
   `blocking = true` 时必须显式声明，且只在「确定不在 Fiber 中」时允许（运行期断言 `Scheduler::current_task() == nullptr`）。
2. **`SendAll/RecvAll` 成为默认推荐**，`send/recv` 改名 `send_some/recv_some`，让部分成功在名字里可见。
3. **`Result<T>` 强制检查**：`[[nodiscard]]` 加在 `SocketResult`/`WebSocketResult`/`http::Result` 上；`ok()` 拆成 `no_error()` 与 `complete()`。
4. **`Address::Lookup` 返回 `Result<AddressPtr>`**，带 `gai_error`，不再只 WARN。
5. **`IOManager` 成为默认后端**：`Scheduler` 构造时若检测到 network 模块使用，自动挂一个内部 IOManager；或在 `Socket::wait_ready` 退化时**一次性 abort**（debug 构建），把 H-01 变成编译/启动期错误而不是线上卡死。

### 3.3 错误语义

1. **禁止吞错**：所有 `bool` 返回改为 `Result`/`enum`；`close()` 三态（M-09）。
2. **超时统一为 `ETIMEDOUT`**，取消统一 `ECANCELED`，关闭统一 `EBADF`（现在大体如此，但 `poll_wait` 的 `POLLHUP` 返回 0，见 socket.cpp:88，会被误判为「就绪」）。
3. **错误计数可观测**：`hook::error_counters()` 已有（hook.hpp:125-136），把 `Socket` 的「退化到 poll 次数」「超时次数」也加进去，默认在退出时打印一行。
4. **`errno` 跨 park 不可靠**（socket.cpp:839-842 注释已说明），所有 API 用返回值传错误，禁止依赖 `errno`。

### 3.4 生命周期

1. **`Socket` 的 `shared_from_this` + in-flight keepalive** 已经做对（socket.hpp:282-288），但 `close()` 的 `Deferred` 状态需要显式 API。
2. **`fd()` 返回值带代际**，或改成 `with_fd(callback)` 形式，从类型上禁止跨操作持有（L-01）。
3. **`FdContext` 字段原子化**（L-06），消除 UB。
4. **`WebSocket` 的预取缓冲按连接常驻**（websocket.cpp:572）——考虑改为「按需增长 + 上限」，小消息连接不付 16KiB。
5. **`HttpServer` 每连接一个 Fiber 串行**（http.hpp:553-556）是对的；补 `idle_timeout` 后即可安全支持长连接。

---

## 4. 优先级（按「最容易踩 × 后果最重」）

| 排名 | 编号 | 理由 |
|---|---|---|
| 1 | **H-01** | 默认用法（全局 Scheduler + Socket）就会卡死整个 M，且无日志、无 sysmon 兜底 |
| 2 | **H-04** | 任何域名访问都会触发；DNS 是外部依赖，本地测试永远不复现 |
| 3 | **H-02 / H-03** | 超时三态语义反直觉，直接决定「挂死 vs 秒断」，排查成本极高 |
| 4 | **H-06** | 部分写导致数据错位，属于「偶发、难复现、后果严重」 |
| 5 | **H-05** | 服务端无超时，线上被慢连接打满只是时间问题 |
| 6 | **H-08 / H-07** | 两套等待/超时叠加，行为依赖运行时状态（需先验证） |
| 7 | **M-01 / M-02** | WebSocket 长连接的默认值不符合常见场景 |
| 8 | **M-03 / M-04 / M-07** | HTTP 的边界与连接管理 |
| 9 | **M-05 / M-06** | hook 的可观测性与全局开关 |
| 10 | 其余 | 清理项 |

---

## 5. 待确认清单（未运行验证）

1. **H-07**：`cooperative_connect` 是否用 `s_connect_timeout_ms` 覆盖 `Socket::connect` 传入的 `Duration`（需读 hook.cpp connect 实现 + 运行时验证）。
2. **H-08**：hook 对 `Socket` 的 `recvmsg`（无 `MSG_DONTWAIT`）是否真的 park；`mark_system_nonblocking` 之后 `can_wait` 的实际值（hook.cpp:1659 附近）。
3. **M-02**：`prefetch_bytes` 是否曾经是 1KiB（用户记忆），当前代码是 16KiB，需查 git 历史。
4. **M-03**：`serve_connection` 收到 `ParseState::Error "body too large"` 时是否回 413（`src/network/http.cpp` 未逐行读完）。
5. **静态链接**下 hook 是否完全失效（`dlsym(RTLD_NEXT)` 语义），本报告按「是」推断，未实测。

---

*本报告为只读审查产物，未修改任何源码。*
