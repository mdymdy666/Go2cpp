# 网络（Socket / epoll / Hook / HTTP/1 / HTTP/2 / TLS / WS / QUIC）· 逐层深入

> 五层读法：第 1 层「为什么」，第 2 层「长什么样」，第 3 层「哪一行、怎么坏」，
> 第 4 层「真实场景与坑」，第 5 层「慢在哪、怎么调」。
> **行号纪律**：所有 `文件:行` 以 2026-11-04 的工作树
> （WSL `/UserData/CodexWorkSpace/Go2cpp`）为准，写作时逐条 `grep -n` 核对。
> **数字纪律**：性能数字只引用 `docs/reports/asm-http-e2e.md`、
> `docs/reports/asm-tls.md` 与 `docs/metrics-ledger.md`，并标明「报告值 / 本文未复跑」。
> 浅层 API 说明在 `user/docs/02-network.md`；本文只讲设计动机、源码细节与坑。

## 五层导航

| 层 | 主题 | 读完能回答 |
|---|---|---|
| 1 | What / Why | 为什么要自己管 fd 与等待，和 Go net/http 差在哪 |
| 2 | How | Socket/IO/协议层的字段级结构与主链 |
| 3 | 源码细节 | 逐函数 `file:line`、状态机、锁序、失败路径 |
| 4 | 实战与陷阱 | 超时 / RST / 关闭三个真场景 + 调试 |
| 5 | 性能与调优 | 已知瓶径、报告数字、调参 |

---

# 第一层 · What / Why

## 1.1 一句话定位

把「阻塞 socket → 协作式等待 → 协议解析」这条链，从「一连接一线程」
改成「一连接一 Fiber，等待即 park」，同时**不改变 POSIX socket 的语义**：
`read/write/accept/connect` 还是那些调用，只是 EAGAIN 时不再阻塞线程。

## 1.2 问题域

| Go 里免费的 | C++/POSIX 默认 | 本模块对应 | 不做的后果 |
|---|---|---|---|
| netpoller 自动挂起 goroutine | 阻塞 syscall 占死线程 | `IOManager` + `ParkingCondition` | 1 万连接 = 1 万线程 |
| `net.Conn` 的 deadline | 每次 `setsockopt` 改全局 | `Socket::wait_ready(deadline)` | 改超时影响同连接其它操作 |
| `http.Server` 并发模型 | 手写 accept 循环 | `Server::serve_connection` | 连接间互相饿死 |
| 连接关闭与在途请求的协调 | 裸 `close(fd)` 直接 UAF | `m_inflight` 计数 + keepalive | 关闭时踩已释放 fd |
| `net/http` 的 pipeline | 一条请求一次 write | bento 的 postpone/CORK/writev | 单连接吞吐上不去 |
| TLS 的 SIGPIPE 安全 | 写已关闭 socket 会杀进程 | `MSG_NOSIGNAL` BIO + 信号兜底 | 对端一 RST 进程就没了 |

## 1.3 边界：不做什么

| 不做 | 原因 | 替代 |
|---|---|---|
| 自研 QUIC | 工程量与正确性风险 | picoquic 薄封装（`src/network/quic/`） |
| TLS 服务端 | 只有客户端封装 | 用 nginx/OpenSSL 终止 TLS |
| HTTP/2 服务端全链路验证 | 缺对端工具 | 仓库自述未验证，别当已支持 |
| 每连接独立 Hook 开关 | Hook 是进程级位图 | 用 `*_f` 变体或关闭 Hook |
| MQTT/CoAP/SOCKS5 放在 network 命名空间 | 需要 bento 依赖 | `GO2CPP_WITH_BENTO=ON` 后 `bento::net` |

## 1.4 与 Go 的对照（迁移时最常踩的差异）

| 维度 | Go | 本模块 | 后果 |
|---|---|---|---|
| 等待方式 | netpoller 挂起 goroutine | `IOManager::wait` + park | 语义近，但手动 Fiber 只能 100 ms 分片轮询 |
| deadline | `SetDeadline` 一改全改 | 每次调用传 timeout | 更安全，但调用方必须逐次传 |
| 关闭 | `Close` 幂等、唤醒阻塞方 | `m_inflight` 归零才真 close | close 可能短暂阻塞 |
| 半关闭 | `CloseWrite` | `shutdown(how)` 直通 | 无协作等待 |
| 读事件 | 边缘/水平对用户透明 | ET+ONESHOT，必须读到 EAGAIN | 少读一次就永久静默 |
| 错误 | `error` | `SocketResult{transferred,error}` | 必须查 error |
| HTTP pipeline | 运行时自动 | 应用层 `take_remaining` + bento 批量写 | 写错会丢第二条请求 |
| TLS | 标准库 | 封装 OpenSSL，默认校验 | `verify_peer=false` 是自毁 |

## 1.5 一条设计主线：fd 是一个「有生命周期的资源」，不是一个整数

```
裸 fd 模型：          本模块：
  int fd = socket()     Socket::Ptr（shared_ptr）
  read(fd)               begin_operation() 固定 fd
  close(fd)              end_operation()
  └─ 另一个线程还在 read  └─ close() 等在途归零，再 ::close
```

围绕这条主线，模块被切成四层：

```
协议层（http/http2/ws/sse）  —— 只认 Socket::Ptr，不认 fd
  ↓
Socket（fd 所有权 + 在途计数 + deadline）
  ↓
IOManager（epoll 条带 + waiter + 定时器）
  ↓
Hook（可选：把 libc 阻塞调用接进 IOManager）
  ↓
Scheduler（park/wake）
```

## 1.6 模块规模与文件职责

| 层 | 文件 | 规模（行） | 职责 |
|---|---|---|---|
| Socket | `include/go2cpp/network/socket.hpp` / `src/network/socket.cpp` | 346 / 1188 | fd 生命周期、在途计数、accept/connect/send/recv/sendfile |
| IO | `include/go2cpp/network/io.hpp` / `src/network/io.cpp` | 360 / 2949 | epoll ET+ONESHOT、64 条带、deadline、批量唤醒 |
| Hook | `include/go2cpp/network/hook.hpp` / `src/network/hook.cpp` | 405 / 2979 | 22 个操作、`__wrap_*`、进程级位图 |
| HTTP/1 | `include/go2cpp/network/http.hpp` / `src/network/http.cpp` | 585 / 1498 | Parser/Writer/Router/Client/Server/StaticFiles |
| HTTP/2 | `src/network/http2/*.cpp` | 1444+651+786+267+753 | 帧、HPACK、流状态机、TLS 传输 |
| TLS | `src/network/http2/tls_transport.cpp` | 753 | OpenSSL 封装、NoSignal BIO |
| WS/SSE | `src/network/websocket.cpp` / `src/network/sse.cpp` | 1018 / 808 | RFC 6455 帧与消息；事件流 |
| QUIC/H3 | `src/network/quic/picoquic_connection.cpp` / `src/network/http3/client.cpp` | 698 / 1348 | picoquic 薄封装 + 客户端 |
| bento | `src/bento/net/http_server.cpp` / `{mqtt,coap,socks5}.cpp` | 755 / 1380+976+1280 | pipeline 三策略；三个应用层协议 |

**记住归属**：pipeline 的 postpone/CORK/writev 在 `bento::HttpServer`，
不在 `http::Server`；MQTT/CoAP/SOCKS5 在 `bento::net`，需要
`GO2CPP_WITH_BENTO=ON`。

## 1.7 术语表

| 词 | 含义 |
|---|---|
| inflight | Socket 上正在进行的操作数（原子计数） |
| slot | 一个 fd 在 IOManager 里的注册状态（`FdSlot`） |
| 条带 | 按 `fd & 63` 分片，降低锁竞争 |
| waiter | 一次等待的节点（`WaitNode`），可被事件/超时/取消三方认领 |
| arm | 把 fd 的 interest 注册进 epoll（ADD/MOD） |
| rearm | ONESHOT 事件后重新注册 |
| 迟到事件 | 注册 id 不匹配的旧事件，必须丢弃 |
| 半关闭 | `shutdown(SHUT_WR)`，本模块直通、不协作 |
| close-delimited | 无 Content-Length 的响应，以 EOF 定界 |

## 1.8 第一层小结 & 下一层看什么

**小结**：网络模块的价值不在「实现了 HTTP」，而在**把 fd 的生命周期
和等待的所有权管住了**。协议层只是消费者；真正的难点在 Socket 与
IOManager 的交界处。

**下一层看什么**：先看 `Socket` 的 5 个原子字段与 `FdSlot` 的 11 个字段，
再看 `accept→recv→EAGAIN→wait_ready→park→epoll→wake` 这条主链。

---

# 第二层 · How

## 2.1 对象图

```
┌──────────────────────────── 用户/协议层 ────────────────────────────┐
│ Server::serve_connection   Client::do_request   WebSocket   sse     │
└───────────────────────────────┬─────────────────────────────────────┘
                                │ Socket::Ptr
┌───────────────────────────────▼─────────────────────────────────────┐
│ Socket（Impl 内）                                                    │
│  m_inflight（原子计数）  m_closing_flag   m_inflight_keepalive        │
│  m_fd_fast（快照）       m_waiter（可注入）  m_mutex（关闭协调）      │
└───────────────┬─────────────────────────────────┬───────────────────┘
                │ EAGAIN                          │ close
┌───────────────▼─────────────────┐   ┌───────────▼───────────────────┐
│ IOManager::State                 │   │ 等在途归零 → ::close(fd)       │
│  64 条带（fd & 63）              │   └───────────────────────────────┘
│  FdSlot{readers,writers,armed,   │
│          interest,force_rearm,   │
│          pending_read/write}     │
│  deadline multimap（weak_ptr）    │
│  epoll(EPOLLET|EPOLLONESHOT)     │
└───────────────┬─────────────────┘
                │ 就绪
┌───────────────▼─────────────────┐
│ 空闲 worker poll_once → wake_io_batch → Scheduler park/wake          │
└──────────────────────────────────────────────────────────────────────┘
```

## 2.2 Socket 的字段级结构

| 字段 | 保护 | 作用 | 坑 |
|---|---|---|---|
| `m_inflight` | 原子 `fetch_add/sub` | 在途操作计数 | 归零前不能 `::close` |
| `m_closing_flag` | 原子 | 关闭已开始，新操作返回 EBADF | 与 inflight 的顺序不能反 |
| `m_inflight_keepalive` | `m_mutex` | 0→1 时装一次 `shared_from_this()` | 避免每包两跳引用计数 |
| `m_fd_fast` | 原子/快照 | `fd()` 的无锁读 | **不能跨操作持有** |
| `m_waiter` | `m_mutex` | 可注入等待器（bento） | 注入后等待路径变了 |
| `m_mutex` | 互斥 | close 与 keepalive 协调 | 只在冷路径 |

**begin_operation 的五步**（`src/network/socket.cpp:228`）：

```
1. m_inflight.fetch_add(1, acq_rel)
2. 已 closing → end_operation() 回滚，errno=EBADF，返回 false
3. prev==0 → InstallKeepalive()（失败回滚）
4. 取 fd 快照；<0 回滚
5. 把 fd 交给调用方
```

**end_operation 的顺序**（`socket.cpp:285`）：

```
1. fetch_sub(1)；非 0 直接返回（还有别的在途操作）
2. 归零：m_mutex 下移出 keepalive
3. 若 m_closing 且 inflight==0 → notify 关闭等待者
4. 锁外释放 keepalive（可能触发 Socket 析构）
```

第 3 步与第 4 步的顺序是 I2 不变量：**先 notify 再 finalize**，
否则关闭者永远看不到谓词成立。

## 2.3 IOManager 的字段级结构

`FdSlot`（`src/network/io.cpp:620` 附近）：

| 字段 | 作用 |
|---|---|
| `fd` / `generation` | 槽位归属 + 代际（拒绝迟到事件） |
| `descriptor` | 强引用 token，跨多轮等待复用 |
| `readers` / `writers` | FIFO 等待队列 |
| `registered` / `armed` | 内核是否注册 / ONESHOT 后是否仍在监听 |
| `interest` | 当前掩码；0 = 停用但保留注册 |
| `registration_id` | 编码 fd + 序号 |
| `force_rearm` | ET 下同 fd 还有等待者时必须 MOD |
| `pending_read` / `pending_write` | 事件到了但没 waiter，留给下次 |

**为什么保留已是空的 slot**（源码注释里的实测）：早期实现
「消费完最后一个等待者就删 slot」，导致 1 万空闲连接场景每请求
6.13 次 `fcntl` + 10.75 次 `epoll_ctl`；改为保留后降到「命中缓存 + MOD」，
s3 QPS 6,270 → 9,823（+56%）、P50 445 ms → 3.4 ms。这是**缓存友好性
胜过内存节省**的典型案例。

## 2.4 主链：一次 recv 的完整路径

```
1. Server::serve_connection（http.cpp:1598）调 connection->recv
2. Socket::recv（socket.cpp:895）→ recv_impl
3. begin_operation 固定 fd
4. ::recvmsg → EAGAIN
5. wait_ready（socket.cpp:743）：
     a. 取消/关闭先行检查
     b. 受管 Fiber → IOManager::wait_for（io.cpp:2699），单次 ≤100 ms
     c. 手动 Fiber → 100 ms 分片轮询
6. IOManager::wait（io.cpp:2545）：
     register_wait（io.cpp:840）→ 条带锁内入队 + update_interest_locked（:1374）
7. park（Scheduler）
8. 空闲 worker：poll_once（io.cpp:2122）→ epoll_wait
9. process_event（:1607）→ process_event_locked（:1532）摘队首 waiter
10. ET/ONESHOT：force_rearm + update_interest_locked 重新 arm
11. wake_io_batch → Scheduler wake → G 恢复
12. recv 重试，直到 EAGAIN 或读满
```

**100 ms 分片不是超时**：它只是「让 close() 有机会打断无限等待」。
真正的超时由 `remaining(timeout, start)` 计算，分片之间会重算剩余预算。

## 2.5 HTTP/1 解析器结构

| 字段 | 作用 |
|---|---|
| `m_buffer` | 累积字节；`feed_impl` 从头解析 |
| `m_limits.max_body_bytes` | body 上限（默认 64 MiB，`include/go2cpp/network/http.hpp:208`） |
| `m_expected_body` / `m_chunked` | 增量状态 |
| `m_chunked_body` / `m_chunk_scan_pos` | chunked 组装与扫描位置 |
| `m_response_close_delimited` | 无长度响应按 EOF 定界 |

**feed 的三条路径**（`http.cpp:447`）：

```
无 body（HEAD/204/304）→ 头部完成即 Complete
Content-Length        → 等到 body_start + length
chunked               → 逐 chunk 扫描，O(n) 增量（m_chunk_scan_pos）
```

**pipeline 的关键**：`consumed` 是「本次喂入字节数」，不是「已消费偏移」。
要把同一 `recv` 里的第二条请求拿出来，必须 `take_remaining()`
（`http.cpp` 的 `Parser::take_remaining`）而不是 `erase(0, consumed)`。

## 2.6 HTTP/2 的分层

```
帧层（frame.cpp）→ 流状态机（stream_state.cpp）→ HPACK（hpack.cpp）
                     ↓
              Connection（connection.cpp）
                     ↓
              TlsTransport（tls_transport.cpp）
```

| 组件 | 职责 | 关键约束 |
|---|---|---|
| Frame | 帧头/长度/类型 | 长度上限防内存放大 |
| StreamStateMachine | 七状态转移 | 非法转移不改字段 |
| HPACK | 动态表 + Huffman | 解码顺序不能乱（丢同步） |
| Connection | 多路复用 + 两道闸 | 超限是连接错误 |

## 2.7 TLS 写路径的三级快路径

```
ScopedSigpipeBlock(required):
  ① required=false（已装 MSG_NOSIGNAL BIO）→ 直接返回，0 次系统调用
  ② 进程已 SIG_IGN SIGPIPE → 直接返回
  ③ 兜底：sigemptyset/sigaddset/pthread_sigmask(SIG_BLOCK)
析构：
  仅 m_failed 才 sigpending + sigtimedwait 消费
  成功路径只恢复掩码
```

**为什么必须消费 pending**：写失败时内核把 SIGPIPE 记为「本线程 pending」，
恢复掩码的瞬间按默认动作投递 → 进程被杀。只做 ③ 不做消费是**不够的**，
这是 Q16 的标准错答。

## 2.8 HTTP/2 的帧与 HPACK（再深一层）

### 2.9.1 帧层

HTTP/2 的所有语义都压在 9 字节帧头上：长度(24) + 类型(8) + 标志(8) + 流号(31)。
本模块的帧处理有三条硬规则：

1. **长度必须与类型匹配**：SETTINGS 长度必须是 6 的倍数、PING 必须 8 字节、
   WINDOW_UPDATE 必须 4 字节；不匹配是连接错误（GOAWAY），不是流错误。
2. **MAX_FRAME_SIZE 是双向的**：本端通告的值约束对端发给我们的帧；
   对端通告的值约束我们发出去的帧。两者不能混用。
3. **未知帧类型必须忽略**（不是错误）——这是 HTTP/2 的扩展点，
   也是「实现只处理自己认识的类型」时最容易漏的一条。

```
+-----------------------------------------------+
| Length (24)                                   |
+---------------+---------------+---------------+
| Type (8)      | Flags (8)     | R + Stream (31)|
+---------------+---------------+---------------+
| Payload ...
+-----------------------------------------------+
```

### 2.9.2 HPACK 的两个方向

| 方向 | 操作 | 出错后果 |
|---|---|---|
| 解码（收） | 索引表查找 + Huffman 解压 + 动态表插入 | 动态表与对端不同步 → 后续所有 header 偏移 |
| 编码（发） | 静态表/动态表选择 + Huffman | 同上，且会污染对端 |

**关键顺序**：超限判定必须在 HPACK **解码完成之后**。
源码注释明确写了理由：先解完再回 `RST_STREAM`，动态表保持同步，
后续流才不会解析错乱。如果为了省内存「先拒绝再解码」，动态表就少了一项，
之后每个带索引的 header 都会错位。

### 2.9.3 两道闸的分工

| 闸 | 限制对象 | 防的攻击 | 检查点 |
|---|---|---|---|
| `max_header_block_size` | 压缩后的字节数 | CONTINUATION 泛滥 / 单片巨大 HEADERS | 拼接时累计 + 单片独立检查 |
| `max_header_list_size` | 解压后的 name+value+32 总和 | HPACK 解压炸弹 | 解码后逐项累加 |

**为什么单片 HEADERS 要单独检查**：CONTINUATION 路径会在
`AssembleHeaderBlock` 里边拼边累计，但单片 HEADERS 不走那个函数。
如果只在拼接函数里检查，一个 10 MB 的单帧 HEADERS 就能绕过第一道闸。

## 2.9 WebSocket 的帧状态机

WebSocket 的复杂度不在帧格式，而在**分片重组**与**控制帧插队**：

```
读路径：
  帧头 → 解析 FIN/opcode/mask/len
    ├─ 控制帧（opcode>=8）：FIN 必须置位、载荷 ≤125、不能分片
    │     ├─ PING → 自动 PONG（auto_pong=true）
    │     ├─ PONG → 更新存活时间
    │     └─ CLOSE → 回 CLOSE，进入 Closing
    └─ 数据帧（1/2/0）：
          opcode=1/2 且 FIN=1 → 单帧消息，直接交付
          opcode=1/2 且 FIN=0 → 开始分片，缓存 payload
          opcode=0（continuation）→ 追加，FIN=1 时交付
```

**三条容易写错的规则**：

1. **控制帧可以插在分片中间**，但不能再分片；
2. **消息总长上限**（`max_message_payload`）要在拼接过程中检查，
   不能等拼完再看——否则内存先爆；
3. **客户端发的帧必须 mask**，服务端发的不能 mask；反过来就是协议错误。

**所有权**：`WebSocket` 只持有 `Socket::Ptr`，**不拥有 fd**；
析构走 `abort_transport()`，只中止读写、唤醒阻塞方，不 close。
这条如果记错，fd 会一直泄漏到进程退出。

## 2.10 定时器与 deadline 表

IO 超时不走业务 `TimerService`，而是 IOManager 自己的 deadline 表：

```
register_wait(deadline) → 插入 multimap<TimePoint, TimerRecord>
                                   │
poll_once 计算 effective timeout ←──┘ 取最近 deadline 与调用方超时的较小值
                                   │
expire_timers（两阶段）：
  阶段 1：m_timer_mutex 内摘出所有到期记录
  阶段 2：释放锁，逐个 claim(kTimedOut) + 唤醒
```

**为什么两阶段**：如果持 `m_timer_mutex` 去拿条带锁，再在条带锁里
唤醒 waiter（可能又要调度器锁），就形成
`m_timer_mutex → 条带锁 → 调度器锁` 的长链；反过来
`register_wait` 是 `条带锁 → m_timer_mutex`。两阶段把方向统一，
避免 ABBA。

**`TimerRecord::node` 为什么是 weak_ptr**：等待节点可能在到期前
就被 IO 完成或取消（正常路径），强引用会把它的生命周期拖到超时，
造成「已完成的请求还占着内存」。weak_ptr 让 `expire_timers` 跳过失效项。

**容量**：每个在途等待一个记录，O(log n) 插入、O(1) 删除（迭代器）、
到期 O(k)。没有时间轮，因为连接级等待的数量级远小于业务定时器。

## 2.11 第二层小结 & 下一层看什么


**小结**：网络层的结构复杂度集中在两个「有状态资源」上——fd（在途计数）
与 epoll slot（注册状态）。协议层反而是纯函数式的字节处理。

**下一层看什么**：逐函数看失败路径——`accept` 的 EBADF、
`connect` 的 ECANCELED、`wait` 的 ETIMEDOUT、`poll_once` 的迟到事件。

---

# 第三层 · 源码细节

## 3.1 逐函数索引

| 函数 | 位置 | 职责 | 失败返回 |
|---|---|---|---|
| `Socket::accept` | `socket.cpp:496` | 固定 fd → accept4 → 纠 Hook 标志 → NODELAY | 空 Ptr |
| `Socket::connect` | `socket.cpp:577` | connect → EINPROGRESS → wait_ready → SO_ERROR | bool |
| `Socket::close` | `socket.cpp:652` | 置 closing → 等在途归零 → close | bool |
| `Socket::shutdown` | `socket.cpp:691` | 直通 `::shutdown` | bool |
| `Socket::wait_ready` | `socket.cpp:743` | 取消/关闭检查 + 分片等待 | 错误码 |
| `Socket::begin_operation` | `socket.cpp:228` | 在途计数 + keepalive | bool |
| `Socket::end_operation` | `socket.cpp:285` | 归零通知 + 释放 keepalive | void |
| `Socket::send_file` | `socket.cpp:1121` | sendfile 循环 | `SocketResult` |
| `IOManager::wait` | `io.cpp:2545` | 登记 waiter + park | `WaitResult` |
| `IOManager::wait_for` | `io.cpp:2699` | deadline 换算 + wait | `WaitResult` |
| `IOManager::wait_many` | `io.cpp:2740` | 多 fd 一次 arm | `WaitManyResult` |
| `IOManager::poll_once` | `io.cpp:2122` | epoll_wait + 事件分发 | int |
| `process_event` | `io.cpp:1607` | 按 registration_id 过滤 | void |
| `process_event_locked` | `io.cpp:1532` | 摘队首 waiter + 重 arm | void |
| `expire_timers` | `io.cpp:1681` | 两阶段超时处理 | void |
| `update_interest_locked` | `io.cpp:1374` | ADD/MOD/停用 | 错误码 |
| `register_wait` | `io.cpp:840` | 代际校验 + 入队 + arm | NodePtr |
| `Parser::feed` | `http.cpp:396` | 增量解析 | `ParseResult` |
| `Parser::feed_impl` | `http.cpp:447` | 头/体/chunked 状态机 | `ParseResult` |
| `Parser::finish` | `http.cpp:398` | EOF 定界收尾 | `ParseResult` |
| `ResponseWriter::start` | `http.cpp:906` | 强制 chunked、丢弃 file | bool |
| `ResponseWriter::flush` | `http.cpp:957` | **不写字节**，只查状态 | bool |
| `Server::serve_connection` | `http.cpp:1598` | 单连接串行循环 | void |
| `Client::do_request` | `http.cpp:786` | 建连/发/收 | `Result` |
| `StaticFiles::serve` | `http.cpp:1389` | Range/304/sendfile | bool |
| `Connection::StartRequest` | `connection.cpp:480` | 查对端上限 + 分配流号 | `ConnStatus` |
| `Connection::WriteData` | `connection.cpp:568` | 流控 + 写帧 | `ConnStatus` |
| `Connection::ReadEvent` | `connection.cpp:1080` | 帧分发 + 两道闸 | `ConnStatus` |
| `Connection::AssembleHeaderBlock` | `connection.cpp:978` | CONTINUATION 拼接 | `ConnStatus` |
| `TlsTransport::Write` | `tls_transport.cpp:623` | SSL_write + 信号快路径 | size_t |
| `WebSocket::Accept` | `websocket.cpp:406` | 校验握手 + 回 101 | `Ptr` |

## 3.2 状态机

**Socket 生命周期**：

```
Open ──begin_operation──▶ Inflight
  │                          │
  │ close()                  │ end_operation（归零）
  ▼                          ▼
Closing ──────────────▶ Closed（::close 已执行）
  ▲
  └─ 新操作试图开始 → EBADF（不再进入 Inflight）
```

**FdSlot 注册状态**：

```
未注册 ──register_wait──▶ registered, armed（EPOLL_CTL_ADD）
                              │ 事件返回（ONESHOT 自动 disarm）
                              ▼
                        armed=false，interest 可能非 0
                              │
                     interest==0 且无 waiter
                              ▼
                        停用（保留注册，合法状态）
```

**HTTP/1 Parser**：`NeedMore → Complete | Error`；
`Error` **不是终态**，可以 `reset()` 后继续喂（`include/go2cpp/network/http.hpp:234` 的注释）。

**HTTP/2 流**：`RefreshFromEndFlags` 由 `local_end/remote_end` 两个 bool
重算七状态，**不是直接赋值**；非法转移返回 `kInvalidTransition` 且不改字段。

**TLS 写路径**：`ScopedSigpipeBlock` 的三种构造结果（免调用 / 全局忽略 /
真屏蔽）+ 析构的两种结果（成功只恢复 / 失败消费 pending）。

## 3.3 锁与所有权

| 锁 | 保护 | 锁序 | 禁止 |
|---|---|---|---|
| `Socket::m_mutex` | close/keepalive/waiter | 叶子 | 持锁做 IO |
| `State::m_mutex` | deadline/registrations | 外层 | 嵌套条带锁（`expire_timers` 两阶段破环） |
| 条带锁 | slots 与 next_generation | 内层 | 持锁 park |
| `m_timer_mutex` | deadline 队列 | 与状态锁分离 | 持锁唤醒 waiter |

**所有权**：

- `Socket::Ptr` 拥有 fd；`Socket::fd()` 只是快照，**不转移所有权**；
- `WebSocket` **不拥有 fd**——析构只 `abort_transport()`，不 close；
- `IOManager::NodePtr` 由 waiter 与 slot 共享，回收进 node cache；
- `TimerRecord::node` 是 `weak_ptr`——避免超时队列延长节点寿命。

## 3.4 失败路径（N1–N12）

| # | 失败 | 触发 | 代码行为 | 调用方 |
|---|---|---|---|---|
| N1 | fd 已关闭 | begin_operation 看到 closing | EBADF，回滚计数 | 重试或放弃 |
| N2 | accept EAGAIN | 非阻塞 | wait_ready(kRead) | 超时 → ETIMEDOUT |
| N3 | connect EINPROGRESS | 非阻塞连接 | wait_ready(kWrite) | SO_ERROR 判真错 |
| N4 | 取消 | generation 变化 | wait_ready 返回 ECANCELED | 停止重试 |
| N5 | 超时 | deadline 到期 | expire_timers 认领 | 返回 ETIMEDOUT |
| N6 | 迟到事件 | registration_id 不符 | 丢弃，不唤醒 | 无 |
| N7 | ET 边沿被吃 | 同 fd 多 waiter | force_rearm + MOD | 无（必须做对） |
| N8 | 解析错误 | 超限/非法 | `ParseState::Error` | serve_connection 直接 return（不回 400） |
| N9 | 写失败 EPIPE | 对端已关 | `SocketResult.error` | sendfile 无 MSG_NOSIGNAL，靠 TLS/进程兜底 |
| N10 | HPACK 超限 | header list 过大 | 连接错误 + RST | 关闭连接 |
| N11 | 关闭与写并发 | inflight>0 | close 等归零 | 可能短暂阻塞 |
| N12 | Hook 关闭 | 位图 | 直通原始符号（真阻塞） | 别在生产留 |

## 3.5 Hook：22 个操作与 5 个不可关闭位

Hook 的本质是「把 libc 的阻塞调用换成协作式等待」，它不改变语义，
只改变等待方式。要理解它，只需要三张表。

### 3.6.1 操作清单

| 类别 | 操作 | 协作方式 |
|---|---|---|
| 读 | read / readv / recv / recvfrom | EAGAIN → IOManager 等待 |
| 写 | write / writev / send / sendto | 同上（写就绪） |
| 连接 | connect / accept | 同上 |
| 睡眠 | sleep / nanosleep / usleep / clock_nanosleep | 定时器 park |
| 多路 | poll / select / epoll_wait | 转交 IOManager |
| 元数据 | close / shutdown / fcntl / dup / dup2 | **不可关闭** |

### 3.6.2 为什么这 5 个不能关

`close/shutdown/fcntl/dup/dup2` 维护的是 fd 的元数据（非阻塞标志、
引用计数、fd 号映射）。如果 Hook 不接管它们：

- `close` 不更新 Hook 的内部表 → 下一次 `wait` 拿着已关闭的 fd 注册；
- `fcntl(F_SETFL)` 不更新标志 → Hook 把「用户设的 O_NONBLOCK」误判成
  「运行时自己的非阻塞」，于是拒绝 park，退化成 100 ms 分片轮询；
- `dup/dup2` 不复制元数据 → 新旧 fd 指向同一个文件描述，但 Hook 以为
  是两个独立的。

所以 `SetOpEnabled` 对这 5 个操作**硬拒绝**（返回 false，位图不变），
`SetAllOpsEnabled` 也会保留它们。

> 历史注释里记录过：accept 之后必须主动
> `mark_system_nonblocking(accepted)`，否则 Hook 会把运行时设置的非阻塞
> 误判成用户意图。那段纠正代码就在 `Socket::accept` 里（`socket.cpp:496`
> 之后），注释提到过旧的 10.3 次 `epoll_ctl`/请求数据。

### 3.6.3 开关的语义

- 进程级位图，不是每连接；
- 关闭是**惰性**的：已 park 的 Fiber 仍会被正常唤醒，只有新调用走原生阻塞；
- 必须 RAII 恢复，否则后续所有 Fiber 静默退化（不 park、阻塞 worker）。

## 3.6 静态文件：Range / 304 / sendfile

`StaticFiles::serve`（`http.cpp:1389`）是协议层里最「业务」的一段，
但它的分支设计值得单独看：

```
1. 解码 URL 路径 + 防穿越（.. / 绝对路径 / 符号链接）
2. stat 文件；不存在 → 404
3. 条件请求：If-None-Match（ETag）/ If-Modified-Since → 304（无 body）
4. Range：
     - 只支持单区间（multi-range 回 200 全量）
     - 非法区间 → 416 + Content-Range: bytes */size
     - 合法 → 206 + Content-Range: bytes start-end/size
5. HEAD → 只回头（Content-Length 仍是完整长度）
6. 普通 → 200 + sendfile 循环
```

**为什么只支持单区间**：multi-range 需要 multipart/byteranges 编码，
响应体要自己拼边界，且客户端支持度参差。单区间覆盖 99% 的断点续传/视频拖动。

**sendfile 循环的三个终止条件**：

- `transferred > 0` 且未写满 → 继续（部分写）；
- `transferred == 0` → 对端关闭或文件截断，中断；
- 负值 → EINTR 重试 / EAGAIN 等待 / 其它错误中断。

**HEAD 与 sendfile 的关系**：HEAD 只回头，不能走 sendfile；
`Response::to_string(is_head)` 负责把 body 长度写进头但不写体。

## 3.7 第三层小结 & 下一层看什么

**小结**：网络层的失败路径几乎都能归到「fd 已失效」「事件迟到」「边沿被吃」
「信号未消费」四类。记住这四类，比记 12 个错误码有用。

**下一层看什么**：拿超时、RST、关闭三个场景，看默认行为怎么坑人。

---

# 第四层 · 实战与陷阱

## 4.1 场景 1：connect 超时（含取消）

```
Socket::connect（socket.cpp:577）:
  step1 读取消代号作锚点
  step2 begin_operation 固定 fd
  step3 取消先检（代号已变 → ECANCELED）
  step4 记录 m_remote（供 reconnect）
  step5 ::connect 一次
        0 → 成功
        EINPROGRESS/EALREADY → 继续
        其它 → 真错误
  step6 wait_ready(kWrite, remaining)
  step7 getsockopt(SO_ERROR) 判结果
  step8 置 m_connected + end_operation
```

**坑 1**：`remaining(timeout, start)` 必须在分片之间重算，且
`Duration::max()` 要单独判断（无限超时不能做减法）。
源码注释里专门写了这一点。

**坑 2**：Fiber 可能换线程，`errno` 要重新写。`wait_ready` 返回后
重新查取消/关闭/超时，再写 errno。

**坑 3**：`SO_ERROR` 非 0 时**不要重试**——连接已经失败，重试只会重复超时。

## 4.2 场景 2：对端 RST（写路径 + SIGPIPE）

**背景**：客户端发完请求，对端 RST；本端继续写响应。

```
写路径：
  Socket::send → send_impl（socket.cpp:843）→ sendmsg(MSG_NOSIGNAL)
  → EPIPE/ECONNRESET → SocketResult.error
  sendfile 路径：没有 MSG_NOSIGNAL！（socket.cpp 顶部注释）
  → 依赖 TLS 的 ScopedSigpipeBlock 或进程 SIG_IGN
```

**为什么 sendfile 是例外**：`sendfile(2)` 没有 `MSG_NOSIGNAL` 参数，
对端关闭后继续发会收到 SIGPIPE。所以：
- HTTP 静态文件走 sendfile 时，必须有进程级 `SIG_IGN` 或 TLS 层兜底；
- TLS 路径已经用 `NoSignalBioWrite`（`MSG_NOSIGNAL`）替代了 OpenSSL 的 `write`。

**读路径遇到 RST**：`recv` 返回 `ECONNRESET`，`serve_connection` 直接 return，
不会写 400。上层需要自己的错误响应或连接级日志。

**验证**：`strace -f -e trace=sendfile,write,sendmsg` 看是否有裸 write；
`/proc/<pid>/status` 的 `SigIgn` 看 SIGPIPE 是否被忽略。

## 4.3 场景 3：close 与在途操作

```
线程 A：recv 阻塞在 wait_ready（在途计数=1）
线程 B：close()
  1. m_closing_flag=true
  2. wait_close_state：等 inflight==0（ParkingCondition）
  3. 线程 A 被唤醒（wait_ready 看到 closing）→ end_operation
  4. inflight 归零 → notify → 线程 B 继续
  5. 锁外释放 keepalive → 可能触发析构 → ::close(fd)
```

**坑 1**：`fd()` 是快照。拿到 fd 后在另一个线程 `::read` 会绕过在途计数，
close 可能已经把它关掉并复用。正确做法是 `begin_operation`。

**坑 2**：`WebSocket` 不拥有 fd，析构不 close。如果握手后把 `Socket::Ptr`
丢了，fd 泄漏。

**坑 3**：`shutdown()` 从不被 Hook 接管（Hook 的 `Originals` 里没有对应项），
所以对端关闭只能靠 `read` 返回 0 感知。

## 4.4 默认行为表

| 项 | 默认 | 出处 | 坑 |
|---|---|---|---|
| 新连接 `TCP_NODELAY` | 开（仅 IPv4/v6 stream） | `socket.cpp:456-467` | Unix socket 会 EOPNOTSUPP |
| `verify_peer` | **true** | `tls_transport.hpp` | 关掉 = 失去链校验 + 主机名匹配 |
| HTTP body 上限 | 64 MiB | `include/go2cpp/network/http.hpp:208` | 超限是 Error，不是 413 |
| HTTP/2 header 块上限 | 64 KiB | `connection.hpp` | 单片 HEADERS 也检查 |
| HTTP/2 并发流 | 100（本端通告） | `connection.hpp` | 对端值另存 |
| WS 帧/消息上限 | 16/64 MiB | `websocket.hpp` | 超限发送会拆 continuation |
| `auto_pong` | true | `websocket.hpp` | 关掉要自己回 pong |
| `GO2CPP_NETWORK_HOOK_ENABLED` | CMake 决定 | `cmake/Go2CppModules.cmake` | 关闭后 socket 里那段纠正也编译掉 |
| `GO2CPP_WITH_BENTO` | OFF | `cmake/Go2CppBento.cmake` | 不开就没有 MQTT/CoAP/SOCKS5 |

## 4.5 可选策略

| 场景 | 策略 | 代价 |
|---|---|---|
| 大量空闲连接 | 保留 FdSlot（默认） | 内存换 fcntl/epoll_ctl |
| 低延迟小响应 | `TCP_NODELAY` 开（默认） | 小包多时带宽利用率低 |
| 高吞吐 pipeline | bento 的 postpone/CORK/writev | 延迟换吞吐 |
| TLS 客户端 | 默认校验 + SNI | 需要 CA 配置 |
| 单 poller | `GO2CPP_IO_SINGLE_POLLER=1` | 减少 epoll 竞争，但单点 |
| Hook 关闭 | 生产可关（若全用 `*_f`） | 一旦有 Fiber 走 libc 就阻塞 M |

## 4.6 调试方法

- **`strace -f -c`**：看 `epoll_ctl`/`fcntl`/`rt_sigprocmask` 次数；
  每请求 epoll_ctl 异常高 → 检查 slot 是否被删。
- **`ss -ti`**：看重传、`cwnd`、`rtt`；pipeline 上不去先排除网络。
- **`tcpdump -i lo -w`**：RST 场景抓包确认是谁先发 RST。
- **gdb**：卡在 `epoll_wait` 是正常的；卡在 `wait_ready` 里的
  `ParkingCondition` 说明 waiter 没被认领——查 registration_id。
- **指标**：`IOManager` 的 waiter 数、deadline 队列长度、`poll_once` 返回值；
  打开 `collect_metrics` 有成本，压测时关。
- **日志**：`Parser` 的 `m_error` 会带原因字符串（超限/长度不一致），
  比错误码好用。

## 4.7 三个调试实录（可复现）

### 实录 1：连接静默

**现象**：压测跑几分钟后部分连接不再有响应，CPU 不高。

**定位**：`ss -tnp` 看连接还在 ESTAB；`strace -f -e epoll_ctl,epoll_wait`
发现该 fd 没有新的 `EPOLL_CTL_MOD`。

**根因**：ET+ONESHOT 下事件被消费后没有重新 arm（或没有读到 EAGAIN）。
修复点：`process_event_locked`（`io.cpp:1532`）之后的
`update_interest_locked`（`io.cpp:1374`）必须执行；同 fd 还有 waiter 时
`force_rearm=true` 触发 MOD。

**教训**：ET 的「必须读到 EAGAIN」不是建议，是正确性条件。

### 实录 2：close 卡住

**现象**：`Socket::close()` 偶尔阻塞几百毫秒。

**定位**：gdb 抓栈，卡在 `wait_close_state`；`p m_inflight` 显示 1。

**根因**：某个路径 `begin_operation` 成功后忘了 `end_operation`
（异常路径没有 RAII）。修复：把 begin/end 包成 scope guard，
或在 `SocketResult` 返回前统一收尾。

**教训**：在途计数必须与 RAII 绑定，不能靠「记得调用」。

### 实录 3：pipeline 只处理第一条

**现象**：一个 `recv` 里带两条请求，只回了第一条。

**定位**：在 `serve_connection` 打印 `parser.feed` 的 `consumed` 与
`m_buffer.size()`，发现用了 `erase(0, consumed)`。

**根因**：`consumed` 是本次喂入字节数，不是已消费偏移。
正确做法是 `take_remaining()`（swap + reset），再喂给下一轮。

**教训**：解析器的「消费了多少」和「缓冲区里剩多少」是两个概念，
API 没写清就会用错。

## 4.8 容量与默认值估算

按默认值粗算（**估算，非压测**）：

| 资源 | 单连接/单请求 | 1 万连接 | 备注 |
|---|---|---|---|
| Fiber 栈 VA | 8 MiB 预留 | 80 GB VA | 实际提交只顶部几 KiB |
| Fiber 栈物理 | 4 KiB 起 | ~40 MB | 按平均 4 KiB 估 |
| Socket 对象 | ~200 B + 控制块 | ~4 MB | 含 keepalive 引用 |
| FdSlot | ~150 B | ~1.5 MB | 保留空 slot 是故意的 |
| waiter 节点 | ~100 B | 按在途数 | 有 node cache |
| deadline 记录 | ~40 B | 按在途数 | weak_ptr |

**结论**：1 万连接的主要成本是**虚拟地址**，不是物理内存。
容器里要同时调 `vm.max_map_count` 与 `RLIMIT_NOFILE`。

## 4.9 上线前检查清单

| 检查 | 命令/位置 | 不合格的后果 |
|---|---|---|
| `verify_peer` 是否为 true | `tls_transport.hpp` | 中间人可劫持 |
| `RLIMIT_NOFILE` 是否够 | `ulimit -n` | accept 返回 EMFILE |
| `vm.max_map_count` | `sysctl vm.max_map_count` | Fiber/映射触顶 |
| `TCP_NODELAY` 是否符合预期 | `Socket::TcpNoDelayDefault()` | 小包延迟高 |
| Hook 开关是否被永久改过 | `OpEnabled` / 位图 | 静默退化 |
| 是否有人关过 SIGPIPE 忽略 | `/proc/self/status` SigIgn | RST 时进程被杀 |
| HTTP body 上限是否匹配业务 | `ParserLimits` | 大文件被拒 |
| HTTP/2 两道闸 | `max_header_block_size/list_size` | 内存放大 |
| 静态文件目录是否防穿越 | `StaticFiles::serve` | 任意文件读取 |
| TimerService 是否被用于 IO 超时 | 代码搜索 | 单线程拖慢全局 |

**一条经验**：这十项里，最难发现的是「Hook 开关被改过」——
它不报错，只是延迟从微秒级退化到 100 ms 级。上线前用
`strace -c -p` 看一次稳态 syscall 分布，比读代码更快。

## 4.10 第四层小结 & 下一层看什么

**小结**：三个场景的共同点是「状态在别处」——超时在 deadline 队列、
RST 在内核缓冲、关闭在 inflight 计数。排查时要顺着状态找，而不是顺着调用栈找。

**下一层看什么**：这些状态的维护成本有多高，用报告数字说话。

---

# 第五层 · 性能与调优

> 数字全部是**报告值**，本文未复跑；换机器/换内核必须重测。

## 5.1 报告数字

| 指标 | 数值 | 出处 |
|---|---|---|
| 单连接 ping-pong | 8.9k req/s | `docs/reports/asm-http-e2e.md:233`（受 WSL2 回环 RTT ~110 µs 限制） |
| pipelining 8 / 16 | 53.7k / 72.9k req/s | 同上 `:233`、`:127` |
| 8 连接并发 | 239.5k req/s | 同上（历史报告口径） |
| 静态文件 sendfile | strace 验证零拷贝 | 同上 `:100` |
| TLS 握手（共享 SSL_CTX） | mean 1.070×、p50 1.091×、p99 1.120× | `docs/reports/asm-tls.md:16` |
| TLS 32KB 写吞吐 | 0.933–1.008×（均值 0.975×） | 同上 `:20` |
| TLS 64B 小包写 | 0.946–1.064×（优化前 1.29–1.35×） | 同上 `:21` |
| TLS 信号系统调用 | 40 万次写 → 15 次 `rt_sigprocmask` | 同上 `:151-162` |
| 自研 ChaCha20-Poly1305 | 64B 0.52×，1MB **13.41×** | 同上 `:24` |

**判读**：TLS 封装层已接近裸 OpenSSL（<1.1×）；自研 AEAD 只在 ≤64B 领先，
≥256B 被 AVX2/AVX512 拉开——**纯标量代码的物理差距**，不要试图靠常量级优化追。

## 5.2 已知瓶径

| 瓶径 | 证据 | 量级 |
|---|---|---|
| 单连接 RTT | 8.9k vs 72.9k | 8×（不是 CPU 上限） |
| 每请求 `epoll_ctl` | 历史 10.75 次/请求，保留 slot 后降 | QPS +56% |
| 每请求 `fcntl` | 历史 6.13 次/请求 | 同上 |
| pipeline 写合并 | `kWriteBatchBytes`/CORK/writev | 单连接 8× |
| TLS 小包信号 | 每次写 4 次信号 syscall → 0 | 4 个数量级 |
| 64 条带碰撞 | 未压测 | 未知 |

## 5.3 调参建议

1. **先确认瓶颈在 RTT 还是 CPU**：`ss -ti` 看 rtt，`top -H` 看核占用。
   回环场景单连接串行永远打不过 pipelining。
2. **pipeline 走 bento 的批量写**：`kWriteBatchBytes`、CORK 阈值
   （`http_server.cpp` 的常量）、`FlushBatch`；不要指望 `http::Server`
   的「一请求一 write」。
3. **slot 不要提前删**：保留空 slot 是实测的更优点；只在 `notify_close`
   时删。
4. **静态文件**：大文件 sendfile；小文件（<16 KiB）读进用户态一次 send
   （bento 的 `kInlineFileBytes`），避免 sendfile 的 syscall 开销。
5. **TLS**：不要关 `verify_peer`；小包写确保走 `MSG_NOSIGNAL` BIO，
   不要退回「屏蔽-恢复」路径。
6. **Hook**：生产若不需要，编译期关掉；运行期关要 RAII 恢复。

## 5.4 反汇编视角（报告值）

| 函数 | 指令 | call | new | lock | 栈帧 | 判读 |
|---|---|---|---|---|---|---|
| `http::Response::to_string` | 633 | 52 | 0 | 0 | 360 B | 序列化是纯计算，但 52 次调用偏多 |
| `http::status_text` | 64 | 5 | 0 | 0 | 16 B | 小函数，可内联 |
| `http::ResponseWriter::start` | 137 | 15 | 0 | 1 | 0（红区） | 有一次原子，可接受 |
| `http::ResponseWriter::write` | 154 | 6 | 0 | 0 | 168 B | 直接落 socket，无缓冲 |
| `http::ResponseWriter::flush` | 9 | 0 | 0 | 0 | 0 | 证明它不写字节 |
| `http::ResponseWriter::close` | 70 | 5 | 0 | 0 | 72 B | 写终止块 |

出处：`docs/reports/asm-http-e2e.md:321-327`（报告值，未复跑）。

**判读**：`flush` 只有 9 条指令、0 次调用——这正是「flush 不写任何字节」
的汇编级证据。`to_string` 的 633 条指令里大部分是字符串拼接与
`to_chars`，属于「返回 `std::string` 的物理下限」，不是优化重点。

## 5.5 基准复跑步骤

```bash
# 1) 编译（独立构建目录，Release）
cmake -S . -B /tmp/build-http -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C /tmp/build-http go2cpp_network

# 2) 协议正确性（260 条断言，不注册 ctest）
g++ -O2 -std=c++17 tests/network/test_http_e2e.cpp ... -lgo2cpp_network

# 3) 吞吐（绑核，多轮，报中位数）
taskset -c 4-11 /tmp/build-http/net_perf_server --connections=8 --pipeline=16

# 4) syscall 证据
strace -f -c -p <server_pid>   # 稳态采样，不重启进程
```

**口径**：单连接 ping-pong 受 RTT 限制，不能代表服务端效率；
比较时必须同核数、同连接数、同 pipeline 深度，并注明是否绑核。

## 5.6 第五层小结

**小结**：网络模块的吞吐主要由「RTT、syscall 次数、写合并」决定，
不是由解析器指令数决定。优化顺序应为：减少 syscall（slot 缓存）→
合并写（pipeline）→ 内核旁路（sendfile/io_uring，未实现）。

---

## 5.7 三条调优结论

1. **先量 RTT，再谈优化**：回环单连接 8.9k 是物理上限，pipelining 才能过 50k。
2. **减少 syscall 比减少指令值钱**：slot 保留一项就带来 +56% QPS、P50 从 445ms 到 3.4ms。
3. **信号系统调用是 4 个数量级的差距**：TLS 小包写从每次 4 次降到 0 次，不是百分点。

以上均为报告值，复跑条件见 5.5。

# 附：自检清单

1. 能否画出「recv → EAGAIN → wait_ready → park → epoll → wake」全链，并说出每步的锁？
2. 为什么 ET+ONESHOT 要求「每次重 arm」且「必须读到 EAGAIN」？
3. `Socket::close()` 如何等在途归零？`fd()` 为什么不能跨操作持有？
4. `consumed` 与 `take_remaining()` 的区别？pipeline 在 `http::Server` 慢在哪？
5. TLS 写路径三级快路径是什么？只恢复掩码为什么会杀进程？
6. HTTP/2 两道 header 上限各防什么？为什么单片 HEADERS 要单独检查？
7. 能否用 `strace -c` 证明某个请求没有多余的 `epoll_ctl`？

# 附-2：未实测 / 未验证

- 本文所有吞吐/延迟都是报告值，**未复跑**；
- h2 服务端 + h2 over TLS 全链路未验证；
- 64 条带哈希碰撞未压测；
- QUIC vs TCP 弱网对照未归档；
- `sendfile` 在 RST 场景的 SIGPIPE 行为只按源码推断，未单独构造复现。
