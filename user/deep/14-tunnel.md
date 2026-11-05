# 公网穿透 Tunnel（协议 / 多路复用 / 服务端 / 客户端）· 深入学习

> 读法：先读第一部分建立全局图，再按需下钻第二部分。行号以 2026-11-03 写入时的 WSL 工作树为准
> （当时 HEAD 为 `be73a4e`；树在动，读到时先 `sed -n` 复核）。
> **数字纪律**：性能数字必须带「报告名 + 日期」，禁止裸数字；没有报告的写「未实测」。
> 本文当期可引的唯一报告是 `docs/reports/tunnel-raw-default-2026-11-04.md`（2026-11-04，
> WSL2 环回，raw 按需选路）：8 MiB body **2385.3 MiB/s、开销 1.27×**（同机 mux 对照
> 770.5 MiB/s / 3.59×）；1 KiB 小请求走 mux p50 1.282 / p99 1.846 ms。
> 历史文档（`docs/modules/tunnel.md`、`user/docs/14-tunnel.md`）的 1.96× / 488 MiB/s 只在
> 「历史文档称…」的位置出现，**不得当结论**。配套考试：`docs/exam/14-tunnel.md`（20 题）。

## 代码地图

| 层 | 文件 | 规模 | 职责 |
|---|---|---|---|
| 协议 | `include/go2cpp/tunnel/protocol.hpp` / `src/tunnel/protocol.cpp` | 310 / 650 行 | 帧类型、12B 头规格、载荷编解码、PSK 挑战应答、子域名校验 |
| 传输 | `include/go2cpp/tunnel/net.hpp` / `src/tunnel/net.cpp` | 264 / 625 行 | 非阻塞 socket + `poll(2)`、`relay::Stream` 三件适配器 |
| 多路复用 | `include/go2cpp/tunnel/mux.hpp` / `src/tunnel/mux.cpp` | 320 / 639 行 | 流表、帧分发、心跳、GOAWAY、8 MiB 每流队列 |
| 服务端 | `include/go2cpp/tunnel/server.hpp` / `src/tunnel/server.cpp` | 269 / 1365 行 | 双端口、HTTP 路由、响应完成检测、drain、状态页、metrics |
| 客户端 | `include/go2cpp/tunnel/client.hpp` / `src/tunnel/client.cpp` | 216 / 522 行 | 握手、退避重连、数据连接、本地转发 |
| 入口 | `src/tunnel/server_main.cpp` / `client_main.cpp` | 214 / 247 行 | 命令行参数、信号、打印 URL |
| 构建 | `cmake/Go2CppTunnel.cmake` | 106 行 | 静态库 `go2cpp_tunnel` + 两个可执行 + 测试目标 |
| 测试 | `tests/tunnel/test_tunnel.cpp` | 930 行 | 协议/子域名/TCP 单测 + 端到端/鉴权/数据连接/重连/并发/性能 |

tunnel **不链接** `go2cpp_network`，也不链接 `go2cpp_bento`：它把 `src/bento/net/{crypto,protocol,x25519,backoff}.cpp` 与 `src/metrics/metrics.cpp` 直接编进 `go2cpp_tunnel`（`cmake/Go2CppTunnel.cmake:44-55`）。同一份源码，不是拷贝； 代价是依赖面被钉死——新增一个 bento 依赖就会把 Fiber/hook 带进这个独立守护进程。

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 入口点清单

**命令行入口**

| 入口 | 文件:行 | 谁调用 → 触发什么 | 返回 |
|---|---|---|---|
| `main()`（server） | `src/tunnel/server_main.cpp:101` | 运维 → 解析参数 → 构造 `TunnelServer` → `Start` → 200 ms 轮询等停止 | 0/1/2 |
| `--control-port` / `--public-port` | `server_main.cpp:109-120` | 写端口（默认 7443 / 8443；0 = 内核分配） | 非法 → 2 |
| `--control-host` / `--public-host` / `--display-host` | `server_main.cpp:121-138` | 监听地址（默认 `0.0.0.0`）；display 空则 `DetectLocalAddress()` | 非法 → 2 |
| `--domain` | `server_main.cpp:139-144` | 打开子域名路由，同时决定 `scheme` 是 `host` 还是 `path`（`server.cpp:948`） | 同上 |
| `--token` | `server_main.cpp:145-150` | 打开鉴权；构造时 `DerivePsk`（`server.cpp:493-496`） | 同上 |
| `--no-strip` / `--crc` | `server_main.cpp:151-154` | 不剥前缀 / 帧尾 CRC（两端必须一致） | — |
| `--max-streams` / `-h` | `server_main.cpp:155-160`、`:105-108` | 单隧道并发上限（默认 256）/ 帮助 | 非法 → 2 / 0 |
| `main()`（client） | `src/tunnel/client_main.cpp:122` | 运维 → 构造 → `Start` → `WaitOnline(15000)` → 打印 URL → 等停止 | 0/1/2 |
| `--server` / `--local` | `client_main.cpp:131-146` | `SplitHostPort`（`:94-118`）；纯数字 `--local` 在 `:188-203` 改写成 `127.0.0.1:<port>` | 非法 → 2 |
| `--subdomain` / `--token` | `client_main.cpp:147-158` | 期望子域名（空 = 服务端随机）/ 鉴权 token | 同上 |
| `--data-conns` | `client_main.cpp:159-168` | 额外数据连接数（0..64，越界拒绝） | 同上 |
| `--max-streams` / `--crc` / `--quiet` | `client_main.cpp:169-178` | 本地并发上限（默认 128）/ CRC / 不装事件回调 | 同上 |

⚠️ CMake target 名是 `tunnel_server` / `tunnel_client`，只有 `OUTPUT_NAME` 是 `tunnel-server` / `tunnel-client`（`cmake/Go2CppTunnel.cmake:77-85`）。所以 `cmake --build build --target tunnel-server` 找不到目标；`user/docs/14-tunnel.md:183` 写的就是这个错写法，本文以 CMake 文件为准。

**库入口：服务端**（`include/go2cpp/tunnel/server.hpp`）

| 入口 | 文件:行 | 谁调用 → 触发什么 | 返回 |
|---|---|---|---|
| `TunnelServer(config)` | `server.hpp:122` | 用户 → 派生 PSK + `InitMetrics()`（`server.cpp:491-498`） | 对象 |
| `Start(err)` | `server.hpp:133`，实现 `server.cpp:648-686` | 用户 → 探测 host、bind 两个端口、起 2 条 accept 线程 | bool |
| `Stop()` | `server.hpp:136`，实现 `server.cpp:688-731` | 用户/析构 → 关监听、join accept、**`BeginDrain()`**、断开所有会话、`WaitWorkers` | void |
| `BeginDrain()` | `server.hpp:159`，实现 `server.cpp:518-563` | `Stop`/用户 → 全部会话 `GOAWAY(1,0)`，轮询在途流至归零或超时 | void |
| `tunnels()` / `PublicUrlFor(sub)` | `server.hpp:147`、`:150` | 状态页/用户 → 快照 / 拼 URL（`server.cpp:1185-1199`） | 快照数组 / string |
| `control_port()` / `public_port()` / `draining()` / `auth_required()` / `connections_total()` | `server.hpp:139`、`:141`、`:162`、`:167`、`:170` | 测试与只读状态；`port=0` 时端口只能从这里取 | u16 / bool / u64 |
| `DetectLocalAddress()` | `server.hpp:175`，实现 `server.cpp:621-646` | `Start` → UDP `connect` 8.8.8.8 取源地址（不发包） | string |
| `set_event_handler(fn)` | `server.hpp:153`，实现 `server.cpp:1180-1183` | 用户 → 上线/下线/错误事件（在任意工作线程里调用） | void |

**库入口：客户端**（`include/go2cpp/tunnel/client.hpp`）

| 入口 | 文件:行 | 谁调用 → 触发什么 | 返回 |
|---|---|---|---|
| `TunnelClient(config)` | `client.hpp:109`，实现 `client.cpp:42-55` | 用户 → 派生 PSK + 注册 3 个 metrics | 对象 |
| `Start(err)` | `client.hpp:119`，实现 `client.cpp:59-76` | 用户 → 只起 1 条 `RunLoop` 线程，立即返回 | bool |
| `Stop()` | `client.hpp:122`，实现 `client.cpp:78-109` | 用户 → 置停、join 主循环与数据线程、等 relay 归零（上限 10 s） | void |
| `WaitOnline(ms)` | `client.hpp:129`，实现 `client.cpp:111-116` | 用户/main → 等首次 `HELLO_ACK` 成功 | bool |
| `public_url()` / `subdomain()` / `Stats()` | `client.hpp:136`、`:139`、`:142` | 用户 → 读状态（`m_state_mutex` 保护） | string / 结构 |
| `server_draining()` | `client.hpp:145` | 用户 → 是否收到 GOAWAY（收到即不再重连） | bool |
| `data_connections()` | `client.hpp:150` | 用户 → **直接回配置值**，不是实际连上的条数 | int |

⚠️ 要判断「实际有几条数据连接活着」，只能看服务端 `/__tunnel__/status` 的 `data_connections`（`server.cpp:608-609`）。

**状态端点与指标**

| 入口 | 文件:行 | 触发条件 → 返回 |
|---|---|---|
| `GET /__tunnel__/status` | `server.cpp:1017-1021` | 公网口命中该路径（先于路由）→ JSON，字段见 `server.cpp:588-619` |
| 隧道目录页 | `server.cpp:1076-1105` | `path` 为 `/`、`/index.html`、`/?…` 且未命中 → HTML 表格 |
| 6 个 server metrics | `server.cpp:502-516` | 进程级 Registry：connections/streams/auth_failures/bytes_in/bytes_out/online |
| 3 个 client metrics | `client.cpp:48-54` | reconnects/streams/online |

> **关于「7 个 metrics」**：任务简报写 7，按源码逐条数是 **6 + 3 = 9**
> （`server.cpp:504-515`、`client.cpp:49-54`），没有第 7 个独立指标名。本文以源码为准。

### 1.2 总架构图

```
 浏览器/手机（不装任何东西）                                    内网服务 :8080
        │ HTTP                                                        ▲
        ▼                                                             │ TCP
 ┌─────────────────────── VPS（公网）───────────────────────┐         │
 │ tunnel-server                                           │         │
 │  PublicAcceptLoop(1 线程)      ControlAcceptLoop(1 线程) │         │
 │  server.cpp:744-757            server.cpp:733-742        │         │
 │        │ accept :8443                │ accept :7443      │         │
 │        ▼                             ▼                   │         │
 │  HandlePublic(每连接 1 线程)    HandleControl(每连接 1 线程)       │
 │  server.cpp:988-1167           server.cpp:759-986        │         │
 │   ├ ReadHttpHead 只读首个头      ├ CHALLENGE→AUTH→HELLO→ACK        │
 │   ├ 路由 /shop/ 或 Host: shop.*  ├ Register(entry)        │         │
 │   ├ OpenStream(stream_id) ─┐     └ 200ms 轮询会话存活      │         │
 │   └ RelayPipe(public⇄stream)│                            │         │
 │                             ▼                            │         │
 │  ┌────────────────────────────────────────────────────┐  │         │
 │  │ MuxSession（每条控制/数据连接一个）                 │  │         │
 │  │  ReceiveLoop + HeartbeatLoop  mux.cpp:573 / :608    │  │         │
 │  │  12B TUNL 帧 → FrameCodec(半包/粘包) → Dispatch     │  │         │
 │  │  流表 m_streams: id → MuxStream                     │  │         │
 │  │  MuxStream: 8 MiB 有界 deque（mux.cpp:212-230）     │  │         │
 │  └────────────────────────┬───────────────────────────┘  │         │
 └───────────────────────────┼──────────────────────────────┘         │
        1 条控制 TCP + N 条数据 TCP（--data-conns）                   │
        帧：HELLO/ACK/OPEN/DATA/CLOSE/PING/PONG/GOAWAY                 │
 ┌───────────────────────────┼──────────────────────────────┐         │
 │ tunnel-client（NAT 后）   ▼                              │         │
 │  RunLoop(1) client.cpp:368-408                           │         │
 │   ├ ConnectAndRegister → 握手 + ACK（client.cpp:180-314）│         │
 │   ├ 断线 → Backoff 0.5s→15s ±20%（client.cpp:170-178）   │         │
 │   └ 收 GOAWAY → m_server_draining → 不重连                │         │
 │  DataLoop × data_conns（client.cpp:334-366，跟随代际）    │         │
 │  HandleOpen（接收线程回调）→ 起 1 条 relay 线程（:447-489）│        │
 │   └ RelayLocal：连 127.0.0.1:8080 + RelayPipe（:491-519） │        │
 └──────────────────────────────────────────────────────────┘         │
                                                                      │
  每条流的字节路径（背压从右往左）：                                   │
  DATA(≤65532B) → MuxStream 8MiB 队列 → RelayPipe 256KiB 队列 ────────┘
  → socket 发送缓冲 → 对端
```

**四个边界**：① 控制/数据连接默认同一条 TCP，`--data-conns N` 才分离 （`client.cpp:316-332`；服务端分支 `server.cpp:889-946`）；② 12B 头由 `FrameCodec` 定界（`frame_codec.hpp:91-96`），tunnel 不做流内分片；③ 线程：`MuxSession` 2 条 + 每公网连接 1 条 + 每流每端 5 条 `RelayPipe` 线程（`relay_pipe.cpp:393-397`、`:787-789`）； ④ 队列：隧道侧 8 MiB/流（`mux.hpp:106`）溢出关流，中继侧 256 KiB/方向 （`relay_pipe.hpp:152`）溢出阻塞读线程。

### 1.3 复杂场景 ≥10 步详解

#### 场景 A：首次连接（15 步；无 token 时第 7–10 步退化为「全零挑战」）

1. **解析参数**：`client_main.cpp:122-184` 写 `TunnelClientConfig`；`--server` 走 `SplitHostPort`（`:94-118`）。非法参数直接返回 2，不进入网络阶段。
2. **构造**：token 非空 → `DerivePsk`（`client.cpp:44-47`）+ 注册 3 个 metrics（`:48-54`）。 状态：**无连接**。
3. **起线程**：`m_running.exchange(true)` 防重入 → 起 `RunLoop`（`client.cpp:59-76`）。 失败只回滚标志；对象不宜复用（见 §2.5）。
4. **拨号**：`ConnectAndRegister(false)`（`client.cpp:180-185`）→ `TcpConn::Dial` （`net.cpp:195-251`）：非阻塞 `connect` + `EINPROGRESS` + `poll(POLLOUT)` + `SO_ERROR`， `ConfigureFd` 设非阻塞/CLOEXEC/NODELAY/KEEPALIVE（`:110-123`）。 失败 → 返回 false，走第 14 步退避。
5. **服务端 accept**：`ControlAcceptLoop`（`server.cpp:733-742`）→ `Spawn(HandleControl)` （`:1335-1356`，detach 线程 + `m_workers` 加一）。
6. **发挑战**：`server.cpp:769-783`。有 token → `MakeChallenge`（`protocol.cpp:483-491`）， CSPRNG 失败返回空 → `Emit` + 关连接（`server.cpp:772-776`）；无 token → 16B 全零。
7. **客户端读挑战**：`FrameReader::Next(5000,…)`（`client.cpp:195-205`；`mux.cpp:53-104` 解半包、多余帧进 `m_pending`）。非 CHALLENGE → 关连接。
8. **判断要不要鉴权**：`client.cpp:214-220` 扫 16B 是否全零；要鉴权而自己没 token → `"server requires --token"` 失败（`:222-229`）。
9. **算 tag**：`ComputeAuthTag`（`protocol.cpp:503-518`）：KDF → `nonce=challenge[0..12)` → `AeadSeal(aad=challenge, 空明文)` → `SecureZero`。
10. **发 AUTH + HELLO**：`WriteFrame(AUTH)`（`client.cpp:238-245`；`mux.cpp:106-122`） + HELLO（`:258-275`）。两帧可能被一次 `recv` 读到——这正是 `FrameReader` 要缓存的原因。
11. **服务端验 tag**：`server.cpp:789-813` → `protocol.cpp:520-534`。失败 → `auth_failures`+1、回 `HELLO_ACK(kUnauthorized)`、关连接；客户端只看到状态码。
12. **读 HELLO**：`server.cpp:816-832`；第一帧不是 HELLO 就关（`:821-825`）；
    `DecodeHello` 解码后再校验长度（`protocol.cpp:266-289`）。
13. **版本/子域名/随机分配**：`server.cpp:834-870`。空子域名最多试 8 次
    `MakeRandomSubdomain(6)` 且要 `Lookup` 为空；8 次全撞 → 关连接**且不发 ACK**
    （客户端只能等 10 s 超时）。
14. **建会话 + ACK + 注册**：`MuxSession::Create`（`server.cpp:879`）→
    `SendPayload(HELLO_ACK)`（`:953-959`，`Start` 之前同步写）→ `Start(OnOpen())`
    （`:960-965`）→ `Register`（`:967`）→ `online`+1（`:968-972`）。数据连接走
    `:889-946` 分支：`Lookup` 不到控制会话 → `"unknown tunnel for data connection"`。
15. **客户端上线**：`WaitHelloAck`（`mux.cpp:288-301`）被 `Dispatch(kHelloAck)` 唤醒
    （`:459-476`）→ `SetState(true,…)`（`client.cpp:418`）→ `StartDataConnections()`
    （`:424`）→ 200 ms 轮询（`:426-433`）；main 的 `WaitOnline` 返回并打印 URL
    （`client_main.cpp:233-239`）。

**失败分支**：拨号失败→退避（`client.cpp:391`）；鉴权失败→`fatal` 置 `m_running=false`
不再重试（`:302-308`）；分配失败→10 s 超时后重试（`client.hpp:78`）。

#### 场景 B：一次公网请求的完整往返（17 步，含背压）

1. **浏览器连接**：`PublicAcceptLoop`（`server.cpp:744-757`）`Accept(500)` →
   `connections_total`+1 → `Spawn(HandlePublic)`。
2. **读请求头**：`ReadHttpHead`（`server.cpp:992` → `:240-282`）按 2 s 分片读到
   `\r\n\r\n`；上限 64 KiB（`:44`）、总超时 10 s（`:46`）；多读的 body 进 `extra`
   （`:278-280`）——不能丢，后面要送进隧道。
3. **解析头**：`ParseHttpHead`（`:1003` → `:98-144`）；`CONNECT` 直接 405（`:1008-1013`）。
4. **状态端点短路**：`/__tunnel__/status` 直接回 JSON（`:1017-1021`），不进路由。
5. **路径路由**：首段 `IsValidSubdomain` + `Lookup`（`:1025-1049`）；`prefix="/shop"`，
   `strip_prefix` 决定是否剥（`:1046`）。
6. **Host 路由（`--domain` 非空）**：`NormalizeHost` + 后缀 `.<domain>`，候选段不能再含
   `.`（`:1050-1066`）；两条都命中时**路径优先**。
7. **墓碑 502**：未命中但首段 300 s 内下线过 → 502（`:1068-1074`；墓碑写入
   `server.cpp:1267-1275`）。
8. **目录页 / 404**：`/`、`/index.html`、`/?…` → HTML（`:1076-1105`）；其它 404。
9. **入口上限**：控制会话 `streams_active() >= max_streams_per_tunnel` → 503
   （`:1119-1125`）；这是单条会话的活跃流，不含数据连接。
10. **选会话**：`PickSession`（`:1127` → `:565-586`）剔除死数据连接后轮询取模，
    没有数据连接就回落控制连接。
11. **开流**：`OpenStream`（`mux.cpp:303-332`）：`alive()` 且非 `draining()` 才继续；
    `m_next_id` 自增（跳过 0）、入表、发 OPEN；发送失败 → `FailStream` 并返回空
    （`:326-330`）→ 服务端回 502（`server.cpp:1134-1138`）。
12. **改写请求头**：`RewriteRequestHead`（`server.cpp:285-332`）换请求行、补
    `X-Forwarded-For/Proto/Host/Prefix`，路径模式强制 `Connection: close`（`:327-329`）；
    再拼 `prefix += extra`（`:1144`）。
13. **装管道**：`SocketStream` → `PrefixedStream` → `ResponseCompletionGuard`（仅路径模式
    启用，`:1150-1152`）→ `EofClosingStream` 双向（`:1153-1154`）→ `RelayPipe::Run`
    （`:1162-1164`）。
14. **客户端收流**：`Dispatch(kOpen)`（`mux.cpp:477-499`）建流 + 回调 `HandleOpen`
    （`client.cpp:447-489`）；`m_workers >= max_local_streams(128)` → 写 503 关流
    （`:451-463`），否则 `m_workers`+1 + detach relay 线程。
15. **连本地**：`RelayLocal`（`client.cpp:491-519`）`Dial` 失败 → 隧道流里写 502
    （`:496-502`）；成功 → `RelayPipe(copy_buf 32K, queue_cap 256K, write_timeout 30s)`
    （`:510-514`）。
16. **数据回程**：`MuxStream::Write` 按 65532 分帧（`mux.cpp:162-194`）→ 服务端
    `Dispatch(kData)` → `PushData`（`:212-230`）→ 公网侧写线程 → socket。
17. **收尾**：`ResponseCompletionGuard` 按 `Content-Length` 判完成，先关隧道流再关公网
    连接（`server.cpp:414-430`）；`Run` 返回 → `stream->Close()` + `conn->Shutdown()`
    （`:1165-1166`）→ CLOSE 帧 + 摘表（`mux.cpp:196-210`）。

**背压三闸门**：本地慢 → RelayPipe 256 KiB 满 → 读线程阻塞（`relay_pipe.hpp:32-43`）→
隧道 8 MiB 涨 → `PushData` 返回 false → `FailStream("recv queue overflow")`
（`mux.cpp:520-522`、`:427-440`）只死该流；目的端 30 s 写不动 → 看门狗（5 ms 一跳，
`relay_pipe.cpp:72`、`:694-717`）关目的端让 `Run` 返回。

#### 场景 C：故障与恢复（14 步）

1. **心跳起跑**：`HeartbeatLoop`（`mux.cpp:608-630`）每 `max(1000, ping_interval_ms)` 醒一次；
   两侧默认 15 s（`server.hpp:83`、`client.hpp:72`）。
2. **判死**：醒后看 `now - m_last_rx_ms > idle_timeout_ms`（`mux.cpp:619-626`）；超了
   `Shutdown("heartbeat timeout")`（`mux.cpp:624`），没超就发 PING（`mux.cpp:627-628`）。
3. **什么算活着**：`m_last_rx_ms` 在接收线程收到**任何字节**时更新（`mux.cpp:589`），
   所以 PONG/数据/GOAWAY 都算；PONG 本身不参与判定（`:551-552`），PING 只是原样回
   PONG（`:546-550`）。
4. **90 s = 6 个周期**：默认 15 s/90 s（`mux.hpp:108-110`），容忍连丢约 5 次。
5. **Shutdown 幂等**：`m_alive.exchange(false)` 一次生效（`mux.cpp:373-384`）：关连接、
   全流 `MarkPeerClosed`、唤醒 `WaitHelloAck` 与心跳。
6. **接收线程退出**：EOF/错误 → `Shutdown("receive loop ended")`（`:573-606`）；所有
   `MuxStream::Read` 因 `m_eof` 返回 EOF（`:132-147`），relay 线程逐层退出。
7. **服务端下线**：`HandleControl` 200 ms 轮询（`server.cpp:974-976`）看到 `!alive()` →
   `Unregister`（`:980`）→ 摘表 + 写墓碑（`:1266-1275`）→ `online`-1（`:982-984`）。
8. **公网侧**：后续请求命中墓碑 → 502 `is offline`（`:1068-1074`）；换个子域名仍是 404。
9. **客户端收尾**：`RunOnce` 轮询退出（`client.cpp:426-433`）→ 累加字节（`:437-438`）→
   `session->Stop()`（`:439`）→ `SetState(false)`（`:443`）。
10. **换代**：`RunLoop` 里 `m_generation`+1（`:373`）；`DataLoop` 每 200 ms 比对代际
    决定重连（`:352-358`）。
11. **退避**：曾经上线过 → 固定 200 ms（`:382-389`）；否则 `backoff.Next()`（`:391`），
    0.5 s→15 s ±20%（`:170-178`）。`Reset` 只在成功后调（`:384`、`:351`）。
12. **明确拒绝不重试**：`kUnauthorized`/`kBadVersion` → `m_running=false`
    （`client.cpp:302-308`），主循环下一轮退出。
13. **GOAWAY**：`Dispatch(kGoAway)` 只置 `m_draining`（`mux.cpp:553-563`）；
    `RunOnce` 看到后置 `m_server_draining`（`client.cpp:430-432`）；`RunLoop`
    `if (server_draining()) break`（`:377-380`）→ 不再重连。
14. **服务端 drain**：`BeginDrain`（`server.cpp:518-563`）幂等 `exchange` → 收集控制+数据
    会话 → 逐条 `SendGoAway(1,0)`（`:536-538`）→ 每 50 ms 统计 `streams_active` 直到 0
    或 5 s（`server.cpp:541-562`）；`Stop` 内部先调它（`server.cpp:704-705`），而 `server_main.cpp:211`
    只调 `Stop()`——顺序被改，`SIGTERM` 就变成立即断连。
    另外同名接管走 `Register`：旧会话 `GOAWAY(2,0)` + `Shutdown`（`server.cpp:1240-1251`），旧会话
    退场时 `Unregister` 被代际校验挡住（`server.cpp:1263-1265`）。

**失败分支**：心跳超时与 EOF 都进 `Shutdown`，但日志字符串不同（排障先读日志）；
退避期间 `wait_for` 可被 `Stop()` 立刻打断（`client.cpp:403-405` + `:82`）。

### 1.4 关键不变量（破了就一定出 bug）

| # | 不变量 | 依据 | 破了会怎样 |
|---|---|---|---|
| I1 | 帧头 12B、字段宽度固定（magic4/ver1/type1/flags2/len4，小端） | `frame_codec.hpp:91-96`、`protocol.cpp:222-230` | 只改一端 header_len → length 从错位得到，首帧即 `kBadMagic`，连接永远建不起来 |
| I2 | `with_checksum` 两端必须一致 | `protocol.hpp:146`、`mux.cpp:111`、`:244`、`:574` | 帧尾多 4B 被当成下一帧 magic，半包状态机错位，表现为随机断连 |
| I3 | 单帧 ≤65536；DATA body ≤65532（stream_id 占 4B） | `protocol.hpp:78-80`、`mux.cpp:179-180` | 超长 `Encode` 失败；绕过常量直接组帧 → 写路径「写一半失败」 |
| I4 | stream_id 单调不复用；只有服务端开流 | `mux.hpp:297`、`mux.cpp:308-311`、`protocol.hpp:44-45` | 两端各自从 1 开流 → 同 id 两条流，`Dispatch` 按 id 查表（`mux.cpp:508-519`）把字节串到别的流 |
| I5 | 接收线程永不阻塞在单条流上 | `mux.cpp:212-230`、`:520-522` | 改成阻塞 push → 一条慢流卡死整条连接（含 PING），拖到 90 s 超时 |
| I6 | 锁序 stream → session；分发先拷 `shared_ptr` 再放锁 | `mux.cpp:9-10`、`:508-515`、`:442-455` | 持 `m_streams_mutex` 调流方法 → ABBA；或析构流时还用裸指针 → UAF |
| I7 | 同连接帧写必须串行化 | `mux.cpp:345`、`mux.hpp:292` | 两线程各写半帧 → 对端 `Feed` 失败 → **整条连接**作废（`mux.cpp:594-597`），不是丢一帧 |
| I8 | 溢出处置是关流，不是关连接 | `mux.cpp:427-440`、`:520-522` | 改成关连接 → 一条慢流打掉同隧道所有并发请求（线上表现为偶发全站 502） |
| I9 | OPEN 后发送失败必须 `FailStream` 并返回空 | `mux.cpp:326-330` | 只返回空不清理 → 流表留孤儿，`streams_active` 只增不减，最终打满 256 |
| I10 | 替换注册先 GOAWAY 再 Shutdown；`Unregister` 校验 session 代际 | `server.cpp:1240-1251`、`server.cpp:1263-1265` | 直接关 → 在途响应截断；不校验 → 旧会话退出时删掉新 entry，表现为「刚上线就掉线」 |

---

## 第二部分 · 分：逐块细讲

### 2.1 protocol：帧格式、载荷、PSK 派生

**职责**：只做「结构体 ↔ 字节」与「token ↔ PSK ↔ tag」的纯函数，不碰 socket/线程，
因此最容易单测（`test_tunnel.cpp:349`）。

**关键常量**

| 名字 | 值 | 行号 | 说明 |
|---|---|---|---|
| `kProtocolVersion` / `kFrameMagic` | 1 / `"TUNL"` | `protocol.hpp:74`、`:76` | 版本不等 → 服务端拒绝 |
| `kMaxPayload` / `kMaxDataChunk` | 65536 / 65532 | `protocol.hpp:78`、`:80` | 后者 = 前者 − 4（stream_id） |
| `kMaxSubdomainLen` / `kMaxTokenLen` | 63 / 256 | `protocol.hpp:82`、`:84` | DNS label 上限 / 防 HELLO 过大 |
| `kHelloFlagDataConnection` | `1<<0` | `protocol.hpp:102` | 数据连接标记 |
| `kPskBytes`/`kChallengeBytes`/`kAuthTagBytes` | 32 / 16 / 16 | `protocol.hpp:105-109` | 与 bento crypto 一致 |
| `FrameType` | 1..10 | `protocol.hpp:87-98` | HELLO(1)…GOAWAY(10) |
| `Status` | 0..4 | `protocol.hpp:127-133` | ok/rejected/bad_version/unauthorized/server_shutdown |
| `GoAway` | reason + retry_after_ms | `protocol.hpp:115-118` | **没有** last-stream-id |

**帧布局**

```
 0   4  magic   "TUNL"      逐字节相等，否则 kBadMagic
 4   1  version 1           不等 → kBadVersion（不降级协商）
 5   1  type    1..10
 6   2  flags   恒 0        （HELLO 的 flags 在 payload 里）
 8   4  length  小端 ≤65536
12   N  payload
```

`MakeTunnelFrameSpec`（`protocol.cpp:222-230`）用 `MakeFrameSpec("TUNL",1,65536)` 再把
`checksum` 置空——这是 tunnel 与帧层唯一的耦合点。

**关键函数**：`DerivePsk`（`protocol.cpp:474-481`）：

```cpp
Psk DerivePsk(const std::string& secret) {
    Psk psk{};
    const auto digest = go2cpp::hash::Sha256::Digest(go2cpp::hash::ByteSpan(
        reinterpret_cast<std::uint8_t*>(const_cast<char*>(secret.data())),
        secret.size()));
    std::memcpy(psk.data(), digest.data(), psk.size());
    return psk;
}
```

`ComputeAuthTag`（`protocol.cpp:503-518`）四步：① `DeriveAuthKey` →
`bento::DeriveHandshakeKey(psk, kClientToServer)`（`:496-499`，方向隔离）；② `nonce` 取
`challenge[0..12)`（`:510-511`）；③ `AeadSeal(key, nonce, aad=challenge, 空明文)`
（`:513-515`）；④ `SecureZero(key)`（`:516`）。`VerifyAuthTag`（`:520-534`）是镜像
`AeadOpen`，常量时间比较在 AEAD 内部。

**状态机（握手帧序）**

```
S→C CHALLENGE(16B)   总是第一步；全零 = 不鉴权
C→S AUTH(16B tag)    必须在 HELLO 之前
C→S HELLO(ver/flags/sub/local_port/token)
S→C HELLO_ACK(status/sub/url/msg)   status==kOk 才进数据阶段
```

服务端先验 AUTH 再解 HELLO（`server.cpp:789-832`），所以错误 token 探测不到子域名是否
存在；但正确 token 能顶掉同子域名（`Register` 接管，`server.cpp:1231-1252`）。

**失败路径与限制**：CSPRNG 不可用 → 服务端硬失败关连接（`server.cpp:772-776`），不会
降级成不鉴权；子域名分配失败**不发 ACK**（`:857-870`），客户端只能等 10 s；
`EncodeHello` 编码前校验、`DecodeHello` 解码后再校验（`protocol.cpp:242-250`、`:281-286`）；
不做传输加密，敏感场景需前置 nginx/caddy。

**典型误用**：改 `kProtocolVersion` 只改一端——帧层仍能解析，表现为「连上就 10 s 超时」；
把 `challenge` 当随机种子复用（它同时是 nonce 与 aad，复用直接泄漏 tag）。

### 2.2 net：监听、连接、超时语义

**职责**：提供「非阻塞 fd + poll + 可跨线程打断」的最小 TCP 层。特意不链
`go2cpp_network`：那里的等待路径与 IOManager/Fiber/hook 耦合（`net.hpp:7-17`）。

**关键数据结构**

| 结构 | 字段 | 行号 | 说明 |
|---|---|---|---|
| `IoResult` | count/error/eof | `net.hpp:46-52` | `ok()` = 无错且非 EOF |
| `TcpConn` | m_fd / m_closed / m_write_mutex | `net.hpp:132-134` | 写串行化；`m_closed` 是 `Shutdown` 幂等闸 |
| `TcpListener` | m_fd / m_port / m_closed | `net.hpp:176-178` | `m_port` 供 `BoundPort()` |
| `SocketStream` | m_conn / m_read_timeout_ms | `net.hpp:209-210` | 吃掉 `ETIMEDOUT` 继续等 |
| `PrefixedStream` | m_inner / m_prefix / m_offset | `net.hpp:233-235` | 先吐预读字节 |
| `EofClosingStream` | m_inner / m_peer | `net.hpp:259-260` | 本方向 EOF → 关对端 |

**关键函数**：`Dial`（`net.cpp:195-251`）= Resolve → socket + `ConfigureFd`
（`:110-123`）→ `connect`（`EINPROGRESS` 不算失败，`:220`）→ `poll(POLLOUT)` 超时关 fd
（`:228-236`）→ `SO_ERROR` 区分「可写但连不上」（`:237-248`）。
`Read`（`:272-339`）语义：`m_closed` → `ECANCELED`；poll 超时 → `ETIMEDOUT`（**不是**
连接坏了）；`recv==0` → `eof`；`ECONNRESET/EPIPE` 也按 `eof`（`:328-334`）。
`WriteAll`（`:341-395`）持 `m_write_mutex`，`MSG_NOSIGNAL`，`EAGAIN` 再 poll 到 deadline。
`Shutdown`（`:397-404`）只 `shutdown(SHUT_RDWR)` **不 close fd**，fd 由析构释放
（`:264-270`），避免 fd 复用 ABA。`Listen`（`:430-472`）= `SO_REUSEADDR` + bind +
listen(512) + `getsockname` 回读端口（`:459-470`，`port=0` 测试入口靠它）。

**状态机**

```
socket → ConfigureFd(O_NONBLOCK) → connect / bind+listen
 → 使用（poll + recv/send）→ Shutdown()：SHUT_RDWR，fd 仍有效但读写立刻失败
 → ~TcpConn()：close(fd)
```

`Accept`/`Read` 都在 poll 前后各查一次 `m_closed`（`net.cpp:484-494`），所以能从任意
线程打断阻塞调用。

**失败路径与限制**：`Accept` 超时返回空，调用方当「没新连接」而非错误（`:487-491`）；
`TcpListener::Close` 同样只 shutdown（`:513-520`）；仅 POSIX——Windows 上
`cmake/Go2CppTunnel.cmake:28-31` 直接 `return()`。

**典型误用**：把 `ETIMEDOUT` 当失败（`SocketStream::Read` 正是靠忽略它区分空闲与断开，
`net.cpp:541-544`）；`Shutdown()` 后继续用 `fd()` 做系统调用（注释写明「仅诊断」，
`net.hpp:111-112`）。

### 2.3 mux：流表、8 MiB 队列、分发

**职责**：把一条 TCP 变成「N 条流 + 心跳 + 关停」。所有并发在这里汇合。

**关键数据结构**

| 结构 | 字段 | 行号 | 说明 |
|---|---|---|---|
| `MuxConfig` | recv_queue_cap 8 MiB / idle 90 s / ping 15 s / read_slice 1 s / checksum | `mux.hpp:106-114` | 每流上限与计时器 |
| `MuxStream` | m_mutex / m_cv | `mux.hpp:167-168` | 保护队列与关闭标志 |
| | m_queue / m_queued_bytes | `mux.hpp:169-170` | `deque<vector<uint8_t>>` + 字节记账 |
| | m_closed / m_eof | `mux.hpp:171-172` | 本地关 / 对端关，两个独立原子 |
| | m_bytes_in / m_bytes_out | `mux.hpp:174-175` | relaxed 统计 |
| `MuxSession` | m_send_mutex / m_encoder | `mux.hpp:292-293` | 写串行化 + 复用 codec |
| | m_streams | `mux.hpp:296` | `id → shared_ptr<MuxStream>` |
| | m_next_id / m_streams_total | `mux.hpp:297-298` | 单调 id / 计数 |
| | m_ack_cv / m_hello_ack | `mux.hpp:300-303` | `WaitHelloAck` 等待点 |
| | m_closed_cv / m_last_rx_ms | `mux.hpp:305-306`、`:308` | 心跳可打断睡眠 / 判死依据 |

**关键函数**：`ReceiveLoop`（`mux.cpp:573-606`）建**独立** codec（`:574`），
`Read(64 KiB, read_slice)`，超时 continue（`:578-585`），收到字节**先**更新
`m_last_rx_ms` 与 bytes（`mux.cpp:589-590`，早于解析），`Feed` 失败即 `break`（`mux.cpp:594-597`），
逐帧 `Dispatch` 并查 `alive()`（`:598-603`），末尾 `Shutdown`（`:605`）。

`Dispatch`（`:457-571`）关键分支：`kHelloAck` 写 ACK + notify（`:459-471`），非 kOk
立即关（`:472-475`）；`kOpen` 建流入表 + 回调，无回调 `FailStream`（`:477-499`）；
`kData` 查表（锁外持 `shared_ptr`）→ 查不到静默丢（`mux.cpp:508-519`）→ `PushData` 失败
`FailStream("recv queue overflow")`（`:520-522`）；`kClose` 摘表 + `MarkPeerClosed`
（`:525-543`）；`kPing` 原样回 PONG（`:546-549`）；`kGoAway` 只置标志（`:553-562`）；
握手帧出现在已建会话 → 关（`:564-568`）。

`PushData`（`:212-230`）的核心三行：

```cpp
if (m_closed.load(...) || m_eof.load(...)) { return true; }   // 已关：静默丢弃
if (m_queued_bytes + length > m_queue_cap) { return false; }   // 溢出：交给 FailStream
m_queue.emplace_back(data, data + length); m_queued_bytes += length;
```

`SendPayload`（`:334-355`）在 `m_send_mutex` 内编码 + `WriteAll(30000)`：**整条连接的
所有流共用一把锁**，一条大流会把其它流的 PING 排到后面（`--data-conns` 就是为此）。

**状态机（一条流）**

| 事件 | 本端 | 对端 | 结果 |
|---|---|---|---|
| OpenStream / Dispatch(kOpen) | open | open | 双向可用 |
| 本端 Close() | m_closed=true | 收 CLOSE | 摘表；Read 返回 EOF |
| 收 CLOSE | 摘表 + m_eof=true | peer closed | Read 返回 EOF；Write 报错 |
| 会话 Shutdown | 清表 + 全 m_eof | 连接断 | relay 线程退出 |
| 队列溢出 | FailStream（摘表+发 CLOSE code=1） | 收 CLOSE | 只死这一条 |

**失败路径与限制**：没有 per-stream credit 窗口（只有「8 MiB + 溢出关流」）；
发送路径每条连接一把锁；id 32 位、只处理了「恰好 0」（`:308-311`）；
`Stop()` 的自 join 分支会 detach（`:396-409`）——在工作线程里调用不打转，但返回后线程
可能仍访问 `this`，所以「Stop 返回后安全」只在非工作线程成立。

**典型误用**：以为 DATA 有优先级（FIFO 一把锁）；以为 `draining()` 后 `OpenStream` 还会
「稍后成功」（`mux.cpp:305-307` 直接返回空，调用方必须当「路已关」）。

### 2.4 server：路由、状态页、metrics、drain

**职责**：两个端口、一张内存路由表、每连接一条线程。

**关键数据结构**

| 结构 | 字段 | 行号 | 说明 |
|---|---|---|---|
| `Entry` | subdomain/session/peer/local_target/scheme | `server.hpp:184-188` | 一条在线隧道 |
| | started_ms | `server.hpp:189` | `online_ms` 基准 |
| | mutex / data_sessions | `server.hpp:190-191` | 数据连接池 |
| | round_robin / draining | `server.hpp:192-193` | 轮询下标 / 本隧道 drain |
| `TunnelServer` | m_tunnels | `server.hpp:240` | **内存表，重启即丢** |
| | m_offline | `server.hpp:243` | 墓碑，保留 300 s |
| | m_psk / m_auth_required | `server.hpp:252-253` | 构造时定型，运行期不可改 |
| | m_workers / m_worker_cv | `server.hpp:263-265` | `Spawn` 记账 |

**路由优先序（`server.cpp:988-1167`）**

```
1 /__tunnel__/status → JSON（:1017）
2 首段合法子域名 + Lookup 命中 → 路径模式（:1025-1049）
3 Host 以 .<domain> 结尾 + 前缀命中 → 子域名模式（:1050-1066）
4 未命中但首段在墓碑里（300s）→ 502（:1068-1074）
5 未命中且 path ∈ {"/","/index.html","/?…"} → 目录页（:1076-1105）
6 其它 → 404（:1106-1109）
```

**关键函数**：`HandleControl`（`:759-986`）顺序是硬的：挑战 → 验 AUTH → 读 HELLO →
校验版本/子域名 → 分配 → 建会话 → 数据连接分支 / 控制注册；失败点全是「发一帧或不发 +
Shutdown」，没有半残中间态。`Register`（`:1211-1254`）：新 entry 先入表（`:1229`），
旧 entry 若存在且 session 不同 → `SendGoAway(2,0)`（`:1240-1245`）→ `Shutdown`
（`:1246-1251`）。`BeginDrain`（`:518-563`）幂等，逐条 `SendGoAway(1,0)`（`:536-538`），
每 50 ms 统计 `streams_active` 直到 0 或 5 s（`:541-562`）。注意它**只统计已进流表的流**：
已 accept、还在读请求头的连接不计，所以「归零」不等于「没有半截请求」。
`BuildStatusJson`（`:588-619`）的 11 个隧道字段与 `TunnelSnapshot`（`server.hpp:95-108`）
一一对应；`draining` 有顶层与每隧道两层。

**metrics（6 + 3）**

| 指标 | 行号 | 定位什么 |
|---|---|---|
| `go2cpp_tunnel_connections_total` | `server.cpp:504-505` | accept 总量；与 streams 差值大 = 大量 404/502/503 |
| `go2cpp_tunnel_streams_total` | `server.cpp:506-507` | 成功开流数；只涨 connections 不涨它 = 路由/上限问题 |
| `go2cpp_tunnel_auth_failures_total` | `server.cpp:508-509` | 持续涨 = 扫 token 或部署不一致 |
| `go2cpp_tunnel_bytes_in_total` | `server.cpp:510-511` | 公网→隧道字节 |
| `go2cpp_tunnel_bytes_out_total` | `server.cpp:512-513` | 隧道→公网字节；单向不动 = 半关闭语义错 |
| `go2cpp_tunnel_online` | `server.cpp:514-515` | 在线隧道数（Inc/Dec 在 `:970-984`） |
| `go2cpp_tunnel_client_reconnects_total` | `client.cpp:49-50` | 快速上涨 = 退避失效/token 错 |
| `go2cpp_tunnel_client_streams_total` | `client.cpp:51-52` | 应与服务端 streams 接近 |
| `go2cpp_tunnel_client_online` | `client.cpp:53-54` | 客户端在线（0/1） |

**状态机**

```
(无) --Register--> online --Unregister--> offline(墓碑 300s) --新Register--> online
                     └--BeginDrain--> draining（仍可 Lookup，但 OpenStream 返回空 → 502）
```

**失败路径与限制**：只读第一个请求头（`ReadHttpHead`，`server.cpp:246-280`），路径模式
必须 `Connection: close`（`:327-329`）；响应完成检测只认 `Content-Length`
（`ResponseCompletionGuard`，`:349-441`），chunked 与无 CL 都 `m_disabled`
（`:385-394`），之后只能靠客户端关连接或 90 s idle；`max_streams_per_tunnel` 只看控制
会话（`:1119-1120`）；隧道表在内存，多 VPS 各一份。

**典型误用**：把 `/__tunnel__/status` 当 Prometheus exposition 抓；用 `--domain` 却不配
泛解析，路径请求被 404 掩盖。

### 2.5 client：握手、退避、本地转发

**职责**：一条 `RunLoop` 维护控制连接，N 条 `DataLoop` 维护数据连接，每条流一条 relay 线程。

**关键数据结构**

| 结构 | 字段 | 行号 | 说明 |
|---|---|---|---|
| `TunnelClientConfig` | server_host/port、local_host/port | `client.hpp:52-58` | 两端地址 |
| | reconnect_min/max 500/15000 | `client.hpp:66-67` | 退避上下限 |
| | recv_queue_cap 8 MiB | `client.hpp:69` | 与 MuxConfig 同值 |
| | max_local_streams 128 | `client.hpp:74` | 超出回 503 |
| | hello_timeout_ms 10000 | `client.hpp:78` | ACK 等待 |
| | data_conns 0 / reconnect_jitter 0.2 | `client.hpp:82`、`:84` | 0 = 单连接 |
| `TunnelClient` | m_generation / m_server_draining | `client.hpp:197-198` | 控制连接代际 / 收到 GOAWAY |
| | m_workers / m_worker_cv | `client.hpp:212-213` | relay 线程记账 |

**关键函数**：`RunLoop`（`client.cpp:368-408`）= `RunOnce` → `m_generation`+1（`:373`）
→ 检查 stop / draining（`:374-380`）→ 上线过则固定 200 ms、否则 `backoff.Next()`
（`:382-391`）→ 可打断 `wait_for`（`:403-405`）。`RunOnce`（`:410-445`）= 连接 →
`SetState(true)` → `StartDataConnections`（`:424`）→ 200 ms 轮询到断开 → 累加字节 →
`Stop` 会话 → `SetState(false)`。`HandleOpen`（`:447-489`）在接收线程里跑：超限写 503 关流
（`:451-463`），否则 `m_workers`+1 + detach。`RelayLocal`（`:491-519`）连本地，失败写 502
（`:496-502`），成功组 `RelayPipe(32K/256K/30s)`（`:510-514`），返回后关流 + 关本地连接。

⚠️ 源码注释（`client.cpp:503-507`）解释了一个反直觉选择：**本地侧 EOF 不传播**。本地服务
FIN 就立刻关隧道流的话，CLOSE 帧会先于剩余 DATA 到达服务端，响应被截断；所以让服务端先
读到公网侧 EOF 再关。

**状态机**

```
[未连接] --dial--> [握手] --ACK ok--> [在线] --断线--> [退避] --> [未连接]
                     ├--fatal 拒绝--> 停止（m_running=false）
                     └--其它拒绝--> 退避重试
```

**失败路径与限制**：`WaitOnline` 谓词含 `!m_running`（`:113-115`），fatal 拒绝立即返回；
`Stop()` 里 `m_worker_cv.wait_for(10000)` 等不到就返回（`:100-104`），本地服务 hang 死时
「Stop 返回后无线程」不成立；**回调里调 `Stop()` 不是永久死锁**——线程 join 有 self 判断
（`:83-98`）会走 detach，真实后果是「detach 掉正在跑 `RunLoop` 的线程 + 最多等 10 s」，
之后对象析构时该线程可能仍访问 `this`。考试题面（`docs/exam/14-tunnel.md` Q17）说
「永久阻塞」与源码不符，以源码为准。`Stop` 后不能 `Start`：`m_data_started` 与线程容器未复位。

**典型误用**：把 `data_connections()` 当实际条数（见 §1.1）；在事件回调里做 DNS/写文件
（回调在 `RunLoop`/`DataLoop` 线程上，会推迟退避与心跳判定）。

### 2.6 RelayPipe：背压与 5 条线程

**职责**：把两个 `relay::Stream` 双向对接，带「有界队列 + 写 deadline + 看门狗」。

**每实例线程数**：watchdog（`relay_pipe.cpp:209`、`:694-717`，5 ms 一跳 `:72`）+
direction[0] reader/writer（`:395-396`）+ direction[1] reader/writer（`:740`）= **5 条**；
`Run` 里依次 Start（`:787-789`）。一条浏览器请求跨两端 = 服务端 5 + 客户端 5 =
**10 条线程/请求**，所以 32 并发约 320 条，不是 160（考试 Q13 只算了单端）。

**队列与背压链**

```
隧道 DATA → MuxStream 8 MiB 队列（mux.cpp:212-230）
 → MuxStream::Read（mux.cpp:132-160，按调用方 size 切片）
 → RelayPipe 256 KiB 队列（relay_pipe.hpp:152）→ writer → socket
```

| 闸门 | 满了会怎样 | 依据 |
|---|---|---|
| 8 MiB 每流队列 | 关该流 | `mux.cpp:520-522` |
| 256 KiB 方向队列 | 读线程阻塞（真背压） | `relay_pipe.hpp:32-43` |
| socket 缓冲 + 30 s deadline | 看门狗关目的端 | `relay_pipe.cpp:694-717` |

**限制**：`queue_cap < copy_buf` 时容量被抬到 `copy_buf`（`relay_pipe.cpp:82-84`），实际
容量不等于配置值；写超时按「单个 chunk」算，不是整条流；线程数随流线性增长，无线程池。

**典型误用**：把 `queue_cap` 当总内存上限——它是**每方向**上限，一条流最多
2×256 KiB + 2×32 KiB；调大只是把压力推回 8 MiB 队列。

### 2.7 FrameCodec 复用点

tunnel 里有 **3 个** codec 实例：

| 用途 | 位置 | 为什么单独 |
|---|---|---|
| 握手同步读 | `FrameReader`（`mux.cpp:49-51` + `m_pending`） | 一次 recv 可能含 AUTH+HELLO，多余帧要缓存（`:99-103`） |
| 会话发送 | `m_encoder`（`mux.cpp:243-245`） | 在 `m_send_mutex` 内串行，codec 有状态不能共享 |
| 接收线程 | 局部 codec（`mux.cpp:574`） | 与发送解耦，不加锁 |

规格由 `MakeTunnelFrameSpec` 定：`header_len=12`、`max_payload=65536`
（`frame_codec.hpp:92-96`）；开 CRC 后帧尾多 4 B（`:102`），整帧 = `12+length+4`——这就是
「两端必须一致」的物理原因。

**限制**：`ReceiveLoop` 固定 64 KiB 缓冲（`mux.cpp:575`），正好装下单帧；调大
`kMaxPayload` 必须同步改这里。

### 2.8 心跳与退避计时器

**心跳**（`mux.cpp:608-630`）：

```
interval = max(1000, ping_interval_ms)      // :609，防 0 值忙转
wait_for(interval) 可被 Shutdown 打断        // :612-615
now - m_last_rx_ms > idle_timeout_ms → 死   // :619-626
发 PING(nonce++)                             // :627-628
```

- 90 s / 15 s = **6 倍**，容忍连丢约 5 个 PING/PONG；
- 判死看「任何入站字节」（`:589`），大流量期间 PONG 全丢也不误判；
- 每条会话一个心跳线程：N 条数据连接 = N 份心跳流量。

**退避**（`client.cpp:170-178`）：`base=max(100,500)`、`max=15000`、`jitter=0.2`；
上限处加抖动是为了多客户端同时断线后不同刻重连；上线过则固定 200 ms 快速恢复
（`:382-389`）；**明确拒绝不重试**（`:302-308`）。历史坑（仅背景、本文未复测）：
`docs/audit/audit-heartbeat-backoff.md` 称旧 `bento::Backoff` 在上限附近把抖动压平。

### 2.9 drain / GOAWAY

**谁发**：`BeginDrain`（`server.cpp:518-563`，reason=1）、`Register` 接管
（`:1240-1245`，reason=2）。**谁收**：`Dispatch(kGoAway)` 只置 `m_draining`
（`mux.cpp:553-563`）→ 客户端 `RunOnce` 置 `m_server_draining`（`client.cpp:430-432`）→
`RunLoop` 停止重连（`:377-380`）。

**时序**

```
SIGTERM → g_stop=true → server.Stop()
 → 关监听 + join accept 线程
 → BeginDrain: m_draining=true → 每条会话 SendGoAway(1,0)
 → 每 50ms 统计 streams_active，直到 0 或 5000ms
 → 逐个 session->Stop() → WaitWorkers(10000+5000)
```

**三个易误解点**：① GOAWAY **没有 last-stream-id**，客户端只能停开新流、在途流继续；
② drain 期间**仍在 accept**（`server.cpp:744-757`），新请求走到 `OpenStream` 返回空 →
502（`:1133-1138`），不是拒连；③ `server_main.cpp:211` 只调 `Stop()`，优雅关闭完全靠
`Stop` 内部调 `BeginDrain`（`server.cpp:704-705`）。

**已知边界**：drain 超时后 `Stop` 硬断（`server.cpp:706-723`）；客户端一旦置 `m_server_draining`
就没有复位路径，服务端重启后**客户端不会自动回来**，需要重启客户端进程。

### 2.10 测试与「未实测」清单

| 测试 | 行号 | 覆盖 |
|---|---|---|
| `TestProtocol` / `TestSubdomain` / `TestTcp` | `:349` / `:481` / `:507` | 编解码边界、子域名规则、TCP 超时 |
| `TestEndToEnd` / `TestAuth` | `:649` / `:717` | 请求 200 / 错误 token `WaitOnline` 超时 |
| `TestDataConnections` | `:777` | `data_conns=2`，10 请求全 200，status 含 `"data_connections":2` |
| `TestReconnect` | `:801` | 停客户端 → 502 → 新客户端同子域名接管 |
| `TestConcurrency` | `:839` | 10 线程并发 POST，响应体各自匹配 |
| `TestPerformance` | `test_tunnel.cpp:867` | 直连 vs 隧道 p50，断言 `ratio < 4.0`（`test_tunnel.cpp:911`） |

**两个与交接文档不符的事实**：① `test_tunnel.cpp:916-930` 的 `main` 依次跑 9 个测试，
**没有 `--filter`**；`user/docs/14-tunnel.md:298` 写的 `--filter protocol` 在源码里不存在。
② 「153 项检查」出自历史文档（`docs/modules/tunnel.md:245`），**未实测**：`build/` 下没有
`go2cpp_tunnel_test` 产物。

**已有实测（2026-11-04，raw 按需选路）**——数字出处 `docs/reports/tunnel-raw-default-2026-11-04.md`；口径：WSL2 环回、单连接、端口自动分配——**不能外推到公网**。

| 场景 | 隧道 MiB/s | 直连 MiB/s | 开销 | 隧道 p50 / p99 |
|---|---:|---:|---:|---|
| 8 MiB body，raw（`Content-Length` > 1 MiB，默认） | **2385.3** | 3039.2 | **1.27×** | 3.331 / 4.410 ms |
| 8 MiB body，强制 mux（阈值 `-1`） | 770.5 | 2766.3 | 3.59× | 10.328 / 13.409 ms |
| 1 KiB 小请求（默认→mux） | 0.7 | 3.2 | 4.22× | 1.282 / 1.846 ms |

- 选路规则：`Content-Length > GO2CPP_TUNNEL_RAW_THRESHOLD`（默认 1048576）→ raw，其余 → mux；
  `0` = 全 raw，`-1` = 全 mux（`src/tunnel/protocol.cpp`）。
- soak：4 组共 **63,048 请求、0 错误**；`ctest -R '^tunnel$'` **170 checks** 通过。
- **历史数字**（`docs/modules/tunnel.md` §6 的 1.96× / 2.99× / 488 MiB/s，以及更早的
  「mux 830 MiB/s / 3.56×」旧默认）：**未复跑**，且口径是旧默认 mux，只作演进对照，
  不得当结论。旧默认 → 现默认的吞吐提升为 **+187%**、开销 −64%（同一报告）。

要复跑请用仓库现有目标（**本文未执行**）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tunnel_server tunnel_client go2cpp_tunnel_test -j
./build/go2cpp_tunnel_test    # 只报通过/失败与 checks；吞吐需自建 soak 驱动
```

---

## 自检清单（读完请回到源码）

- [ ] 能画出控制连接 / 数据连接在 `MuxSession` 层的分叉点（`server.cpp:886-946`）。
- [ ] 能说清 12B 头里 `length` 与 `kMaxDataChunk` 的差值来源（`protocol.hpp:78-80`）。
- [ ] 能指出 PSK 派生链的四个函数与方向隔离（`protocol.cpp:474-534`）。
- [ ] 能说出 8 MiB 溢出后死一条流还是死一条连接，以及为什么（`mux.cpp:520-522`）。
- [ ] 能列出 6 个 server + 3 个 client metrics（`server.cpp:504-515`、`client.cpp:49-54`）。
- [ ] 能解释 `BeginDrain` 为何「归零」不等于「没有半截请求」。
- [ ] 能说出一个 RelayPipe 为何 5 条线程、一条请求为何跨两端 10 条
      （`relay_pipe.cpp:393-397`、`:787-789`）。
- [ ] 能指出回调里 `Stop()` 的真实后果（detach，不是永久死锁）。
- [ ] 能说清本文的性能数字来自哪份报告、什么口径（WSL2 环回 / 单连接 / 8 MiB body），
      以及为什么不能外推到公网。
