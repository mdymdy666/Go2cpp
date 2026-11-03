# 应用层流框架（app）· 深入学习

> 面向「接手改这块代码的人」。所有 `文件:行` 都用 `sed -n` / `grep -n` 现场核实过；
> 拿不准的地方直接标「未核实」。本文**不写达标结论**，性能只引 `docs/metrics-ledger.md` 编号。
>
> 模块规模（实测）：`include/go2cpp/app/` **13 个头 / 3364 行**，`src/app/` **12 个实现 / 7542 行**。
> 注意 `user/docs/12-app.md:3` 写的是「11 个头、10 个实现」——已过时，`sha256_ni.hpp` 与
> `realtime_room`、`gateway` 都在册。`sha256_ni` **没有** `.cpp`（纯头，见 §5.7）。

---

## 第一部分 · 定位

### 1.1 它到底解决什么问题

`go2cpp::app` 不是业务层，是**应用层协议框架**：把「一条 TCP/UDP/HTTP 连接」切成多条有独立
窗口、优先级、生命周期和错误域的**逻辑流**。三个具体问题：

1. **一条连接跑多个逻辑流**。`StreamTable` 维护 idle/open/half_closed/closed 状态机、
   连接级 + 流级双窗口、依赖树父优先 + WFQ 加权调度。同一套代码既发 100 MB 资源，
   也发 200 字节小 KV 表，互不阻塞。
2. **资源分块 / 校验 / 续传**。`ResourceProducer` 按块切片（块头带 `index/offset/crc32`），
   `ResourceReceiver` 逐块校验 CRC、整文件校验 SHA-256，支持从第 N 块续传。
3. **KV 表按尺寸分档**。小于阈值 ⇒ HEADERS 一帧搞定；超过 ⇒ 自动分区（小值进 HEADERS、
   大值进 DATA）；也可强制「每键一条流」。

框架自身**不知道**字节从哪来（`ITransport` 抽象）、**不知道**帧的业务含义（`app.kind` 决定），三层可分别单测。

### 1.2 与浅层 `user/docs/12-app.md` 的关系

浅层已讲清**协议表、状态机、帧类型、握手**（9 B 帧头、`APP2\r\n\r\n` preface、SETTINGS 协商项、
WFQ 概念、实测数字）。本文**不重复**，只补浅层没展开的四块：

| 浅层写了 | 浅层没展开（本文补） | 本文位置 |
|---|---|---|
| 有 `Gateway` 组件 | 路由 Kind 优先级、`stable_sort` 比较键、中间件装配顺序、`InvokeChain` | §3.4 |
| 一句「`realtime_room` 广播」 | 每连接一条发送线程、队列满即踢、两处「锁外 join」 | §3.5 |
| 一句「HTTP 隧道」 | `BatchTransport` 双缓冲、`HandleRequest` 步进循环与 16 MiB 上限 | §3.5 |
| 「WFQ + 窗口」概念 | `PickNext` 堆顶命中即返回、`ChargeFinish` 移位查表、快照同步 | §3.1 |

帧头布局、`FrameType` 全表、SETTINGS 语义、`ResourceMeta` 字段表去浅层查，本文只给行号指针。

### 1.3 与相邻模块的边界（最容易越界）

| 你以为在这 | 实际在 | 依据 |
|---|---|---|
| SHA-NI 压缩函数 | `go2cpp::hash::Sha256`；`app::Sha256Fast` 只是薄壳 | `include/go2cpp/app/sha256_ni.hpp:38-63` |
| HTTP 解析/序列化 | `go2cpp::http`（`Parser`/`SerializeResponse`），gateway 只调用 | `src/app/gateway.cpp:1097,1123` |
| Link 收发与 UDP | `bento::Link`；`bento_link_transport` 只搬帧 | `src/app/bento_link_transport.cpp:19-46` |
| HTTP 服务端 | `bento::HttpServer`；`HttpTunnelServer` 只包一层 body 批处理 | `src/app/http_tunnel.cpp:144-164` |

---

## 第二部分 · 架构

### 2.1 文件清单（13 头 + 12 实现）

| 文件（头 / 实现） | 行数 | 职责 | 谁依赖它 |
|---|---|---|---|
| `frame` / `.cpp` | 398 / 510 | 9 B 帧头编解码、`FrameDecoder`（含零拷贝 `NextView`） | connection、bento_bridge |
| `transport` / `.cpp` | 146 / 176 | `ITransport` 接口 + `MemoryTransport`（回环测试） | 全部传输适配 |
| `stream_table` / `.cpp` | 500 / 1064 | 状态机、双窗口、就绪堆、WFQ、`FrameProducer` 调度 | connection |
| `connection` / `.cpp` | 391 / 1663 | 握手、`Step` 驱动、帧分发、发送预算、事件队列、KV/资源入口 | 应用 |
| `resource` / `.cpp` | 487 / 1043 | `ResourceMeta`、三种 `ChunkSource`/`ResourceSink`、生产者/接收者、索引 | connection、应用 |
| `kv` / `.cpp` | 211 / 449 | `KvTable`（开放寻址、文本/二进制编解码） | kv_stream、connection |
| `kv_stream` / `.cpp` | 234 / 298 | `KvProducer`（分区）、`KvAssembler`（重组）、`KvBatchCollector` | connection |
| `gateway` / `.cpp` | 460 / 1466 | HTTP 反向代理：路由、中间件、上游转发、健康聚合、JSON 配置 | 应用 |
| `realtime_room` / `.cpp` | 95 / 324 | `RoomHub`：房间广播、每连接发送线程、慢消费者踢出 | 应用 |
| `http_tunnel` / `.cpp` | 144 / 189 | 把帧流塞进 HTTP 请求/响应 body | 应用 |
| `bento_bridge` / `.cpp` | 118 / 211 | `app::Frame` ↔ `bento::net` 帧桥接编解码 | bento 场景 |
| `bento_link_transport` / `.cpp` | 108 / 149 | `ITransport` 的 Link 实现（channel 多路 + 背压） | bento 场景 |
| `sha256_ni.hpp`（无 cpp） | 72 | `Sha256Fast` 兼容壳，转调 `hash::Sha256` | resource |

### 2.2 入口点（入口 | 文件:行 | 谁调用 | 触发什么 | 返回什么）

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回什么 |
|---|---|---|---|---|
| `ConnectionConfig` | `include/go2cpp/app/connection.hpp:96-109` | 应用/隧道 | 配置聚合，**不建连不发帧** | 结构体 |
| `Connection::Start(error)` | `src/app/connection.cpp:308` | 应用或首个 Send/Step 懒启动 | 写 `APP2` preface + SETTINGS | bool |
| `Connection::Step(timeout_ms)` | `src/app/connection.cpp:333` | **仅一个 IO 线程** | 收帧→分发→`FlushLocked`→PING→剪枝 | 处理帧数；`-1`=已关闭 |
| `Connection::ReadEvent(out, timeout)` | `src/app/connection.cpp:425` | 应用 IO 线程 | 取事件；阻塞期用 `Step(≤20ms)` 推进 | bool |
| `Connection::SendKv / SendKvPerKey` | `src/app/connection.cpp:1305 / 1320` | 任意线程 | → `EnqueueKv` 建流挂 producer | `SendResult{stream_id,error}` |
| `Connection::SendResource` | `src/app/connection.cpp:1327` | 任意线程 | 建单向流 + `ResourceProducer` | `SendResult` |
| `Connection::SendBytes / Acknowledge` | `src/app/connection.cpp:1383 / 1646` | 任意线程 | 裸字节流 / 手动归还窗口 | `SendResult` / bool |
| `StreamTable::PickNext` | `src/app/stream_table.cpp:700` | `FlushLocked` | 选出下一条可发流 | `Stream*` 或 `nullptr` |
| `StreamTable::OnWindowUpdate` | `src/app/stream_table.cpp:858` | `HandleWindowUpdate` | 加窗口 + 只重排该流 | bool（溢出→false） |
| `ResourceReceiver::OnData` | `src/app/resource.cpp:611` | `HandleData` | 解析块头、校验、落 sink | bool |
| `ResourceReceiver::Finish` | `src/app/resource.cpp:701` | `CompleteReceive` | sink 收尾 + SHA-256 比对 | bool |
| `KvProducer::Produce` | `src/app/kv_stream.cpp:169` | `FillPending` | 首帧前 `Partition`，之后切片 | bool |
| `Gateway::Create(config)` | `src/app/gateway.cpp:1241` | 应用 | 建监听 + 装内置中间件 | `Result<unique_ptr<Gateway>>` |
| `Gateway::Run` | `src/app/gateway.cpp:1401` | 应用（阻塞线程） | accept → 有界队列 → worker | void（Stop 后返回） |
| `Gateway::AddRoute(RouteSpec)` | `src/app/gateway.cpp:1278` | 应用（Run 前后均可） | 解析 pattern → 排序插入/同名覆盖 | `Result<void>` |
| `Gateway::Use(Middleware)` | `src/app/gateway.cpp:1287` | 应用 | 追加中间件（**尾插**） | void |
| `RoomHub::Join / Broadcast` | `src/app/realtime_room.cpp:209 / 255` | 应用 | 建 `Conn`+起线程 / 逐连接 `Enqueue` | `ConnId` / 入队条数 |
| `HttpTunnelServer::HandleRequest` | `src/app/http_tunnel.cpp:165` | HTTP handler | Feed body → 循环 `Step` → 取响应 | `vector<uint8_t>` |
| `bento_bridge::Decoder` | `src/app/bento_bridge.cpp:151` | bento 场景 | 桥接帧解码 | `Status` |

### 2.3 总架构图

```
                     应用（游戏资源下发 / KvStream / HTTP 网关）
                                    │
   ┌────────────────────────────────┼──────────────────────────────────┐
   │                          Connection                               │
   │  m_io_mutex (recursive)  ┌────────────────────────────────────┐   │
   │  Step ──┬─ FrameDecoder::NextView ──► HandleFrame             │   │
   │         │                                ├─ HEADERS → KvAssembler / ResourceReceiver
   │         │                                ├─ DATA    → 同上
   │         │                                ├─ PING/SETTINGS/GOAWAY → 控制队列
   │         │                                └─ RESOURCE_INDEX / WINDOW_UPDATE → StreamTable / Event
   │         ├─ FlushLocked ──► StreamTable::PickNext ──► Frame& pending
   │         │                       │ （就绪堆，按 finish 最小）
   │         │                       └─ FrameProducer::Produce（Kv/Resource/Bytes）
   │         └─ Publish → m_event_mutex 队列 + 回调
   └────────────────┬─────────────────────────────────┬────────────────┘
                    │ ITransport::Read/WriteAll        │ 事件
        ┌───────────┴───────────┐          ┌──────────┴──────────┐
        │ MemoryTransport       │          │ ResourceReceiver    │
        │ BentoLinkTransport    │          │ KvAssembler         │
        │ HttpTunnel(Client/Srv)│          │ ResourceIndex       │
        └───────────────────────┘          └─────────────────────┘

  独立分支（与 Connection 无关，共用 net/http）：
   Gateway ─ accept ─► 有界队列 ─► worker ─► ServeConnection
              └─ Dispatch ─► 中间件链(AccessLog→RateLimit→Auth) ─► Match ─► Forward
   RoomHub ─ rooms[room][id] → 每 Conn 一条发送线程 + 有界队列
```

### 2.4 线程与锁边界（接手必读）

- **`m_io_mutex`（`recursive_mutex`，`connection.hpp:381`）**：`Step`、所有 `Send*`、`Start` 都拿它；
  `StreamTable` 内部**没有任何 mutex**（`grep -c mutex include/go2cpp/app/stream_table.hpp` = 0），
  全靠这把锁保护。递归是为了**事件回调里能重入发送 API**。
- **`m_event_mutex`（`connection.hpp:382`）**：只保护 `m_events`（上限 65536，`connection.hpp:385`）；
  `Publish`（`connection.cpp:224`）锁内 push、锁外 notify + 调回调。
- **`Step` 重入被显式拒绝**：`connection.cpp:333-341`——`m_in_handle` 为真直接 `return 0`，
  因为当前帧载荷是解码缓冲的借用视图。**在事件回调里调 `Step` 是空操作**。
- **Gateway / RoomHub**：前者 `routes_mutex`（`shared_mutex`）读写分离，worker 从有界队列取连接
  （`gateway.cpp:1412-1426`，满则**直接 close**）；后者一把 `std::mutex` 保护 `rooms`/`room_of`，
  而 `Conn::Stop()` 会 `join`，所以擦除后的 `shared_ptr` 必须**锁外释放**
  （`realtime_room.cpp:172-204` 注释、`:255-274` 的 `finished.clear()`）。

---

## 第三部分 · 关键实现

### 3.1 发送调度：双窗口 + 就绪堆 + WFQ（`stream_table.cpp:700-744`）

一次 `FlushLocked`（`connection.cpp:515`）的主循环：

1. `EmitWindowUpdates()`（`connection.cpp:487`）先还接收窗口，避免「只发不收」卡死对端；
   `budget = max_bytes_per_flush`（默认 2 MiB，`connection.hpp:105`）。
2. `PickNext(max_frame_payload)`：先 `DrainIdle` 补首帧，再在 `m_ready.empty() || m_refill_hint`
   时 `RefillReady`；**堆顶命中即返回、零堆操作**——函数内注释写明旧实现对每个候选
   `RemoveReady+PushReady`，256 流下约 600 ns/帧纯浪费。只有「已关闭 / `!IsReady` / 有就绪祖先」
   三种流出堆；父优先的流暂存 `m_held`，本轮结束再 `PushReady`。
3. 取 `stream->pending` **零拷贝**直接写出；DATA 帧先 `ConsumeSend` 扣窗口，失败则 `Requeue`
   + `++flow_control_blocks` 后 break。
4. `ConsumePending` 清 pending 但**保留 payload 容量**（`connection.cpp:545` 注释：下一帧零分配）
   → `SendData`/`SendHeaders` 推进状态；DATA 与非 DATA 分别 `ChargeFinish`；推进失败 ⇒
   发 `RST_STREAM(kProtocolError)` + `Reset` + `CleanupStream`（`connection.cpp:563` 起）。

**WFQ 计费**（`stream_table.cpp:833`）：`finish += bytes * 2^20 / weight`。权重是 2 的幂时走
`kPow2Shift` 查表 + 左移（`stream_table.cpp:810-831` 的 `MakePow2Shift`），非 2 的幂才做 64 位除法。
finish **单调增**，堆内只需 `SiftDown`（`stream_table.cpp:846-851`），但堆项的 finish 是**快照**，
必须先写回 `m_ready[slot].finish` 再下沉，否则比较用的是旧键。

**窗口更新的三个细节**：

- 连接窗口：`+increment > kMaxWindow` ⇒ 报错 `connection window overflow`（`stream_table.cpp:858-871`，
  `kMaxWindow = 0x7fffffff` 在 `stream_table.cpp:20`）。
- 流窗口：未知流上的 WINDOW_UPDATE **按 RFC 静默忽略**；`has_pending && IsReady` 时只
  `PushReady` 这一条，不惊动全表（`stream_table.cpp:877-884`）。`SetInitialWindowSize` 对所有流加
  同一 delta，任一为负/溢出则**整体失败**（`stream_table.cpp:890-916`）。
- 归还窗口阈值是初始窗口的一半（`AcknowledgeReceived`，`stream_table.cpp:918-935`）：
  「攒够半窗再发 WINDOW_UPDATE」，不是每帧都发。

`FillPending`（`stream_table.cpp:608`）负责「窗口太小就不生产」：`allowed < 16` 直接返回 false
（`stream_table.cpp:634-637`），避免把大帧切碎。生产者给出超过 `limit` 的帧时**直接
`producer_done=true` 丢弃**（`stream_table.cpp:647-652`），不把非法帧塞进调度器。

### 3.2 资源：分块 + CRC + SHA-256 + 续传（`resource.cpp`）

**发送侧**：

1. `BuildResourceMeta`（`resource.cpp:759`）先整文件扫一遍算 SHA-256 + CRC32。
2. `ResourceProducer::BuildHeaders`（`resource.cpp:817`）把 meta 塞进 HEADERS。
3. `BeginChunk`（`resource.cpp:839-890`）决定这一块怎么发：
   - 整块装得进一帧 ⇒ `m_chunk_direct = true`，读进 payload 后再算 CRC；
   - 装不进但源支持零拷贝 `View` ⇒ 直接对底层内存算 CRC；
   - 都不行 ⇒ `m_chunk.resize(bytes)` 整块读入再算 CRC。注释写明理由：预扫描会把源多读一遍，
     生成式源还翻倍 CPU，整块读入反而更省。
4. `Produce`（`resource.cpp:892`）每帧载荷 = `index(u32) | offset(u32) | crc32(u32) | data`
   （`kResourceChunkHeaderBytes` = 12）。

**接收侧 `OnData`（`resource.cpp:611-673`）的校验顺序**（错一步整条流 RST）：
`size<12` → duplicate（`index < m_completed_chunks`）→ gap（`index > m_completed_chunks`）→
index 越界 → offset 不连续（`offset != m_chunk_buffer.size()`）→ 超块长
（`offset + data_bytes > expected`）。每条都置 `m_error` 让整条流失败。

**整块直通**：`offset==0 && buffer 空 && data_bytes==expected` 时跳过 `m_chunk_buffer`，
直接 `CommitChunk`——1 GiB 资源就省 1 GiB 的 memcpy。碎块走 `insert` 拼满再提交；
**CRC 先校验、后写 sink**，坏块不污染 sink（`resource.cpp:675` 的 `CommitChunk`）。

**续传语义**：`ResourceReceiver(meta, sink, start_chunk)`（`resource.cpp:577-606`）把
`m_completed_chunks` 初始化为 `start_chunk`、`m_partial = start_chunk > 0`；`CommitChunk` 里
只有 `!m_partial || index >= m_start_chunk` 才喂给 SHA。所以续传时哈希只覆盖后半段，
`Finish`（`resource.cpp:701`）也就**只在非 partial 时比对整文件 SHA-256**——这是设计，见 §5.4。

`sink` 有 `Memory`/`File`/`Discard` 三种（`include/go2cpp/app/resource.hpp:250/268/294`）；
`Finish` 前必须「chunk 数齐 + 无残留半块」，否则判 incomplete。

### 3.3 KV：小表一帧 / 大表分区 / 每键一流（`kv_stream.cpp`）

三档由 `KvProducer::Partition`（`kv_stream.cpp:76`）在**首帧生产时**一次性决定：

| 条件 | mode | 线上形态 |
|---|---|---|
| 无大值 且 `small.ToBinary().size() <= single_limit` | `Single` | 一个 HEADERS + END_STREAM |
| 否则 | `Stream` | HEADERS(小值) + 若干 DATA(大值二进制) |
| 调用方显式 `KvMode::kPerKey`（`connection.cpp:1257-1290`） | `PerKey` | 每键各建一条流，各自一个 producer |

关键数字：

- `frame_limit = max_payload - 256`（`kv_stream.cpp:94-101`），256 B 是给保留键留的余量
  （`app.kind`/`app.kv.mode`/`app.kv.keys`/`app.kv.partial`）。不留这个余量，
  「刚好装满」的 HEADERS 加上保留键就超限发不出去。
- `single_limit = min(frame_limit, small_kv_threshold)`，默认阈值 4096（`connection.hpp:99`）。
- 小值太多、HEADERS 装不下时，**按值长度从大到小逐个搬进大值区**直到能装下
  （`kv_stream.cpp:107-120`）——O(n²) 但只发生在首帧、n 是键数，可接受。
- `ProduceHeaders`（`kv_stream.cpp:124`）会**再检查一次** `payload.size() > max_payload`，
  超了就清空 + `m_done=true` 放弃这条流（`kv_stream.cpp:132-137`）。

接收侧 `KvAssembler`（`kv_stream.cpp:189/210/231`）：`OnHeaders` 读保留键判 mode，
`OnData` 追加 body（受 `m_max_bytes` 约束），`Finish` 反序列化 + `StripReserved` 摘掉框架键
（`kv_stream.cpp:262`）。`KvBatchCollector`（`kv_stream.cpp:282`）负责 PerKey 合并，
`Merge()` 按 `expected` 数量凑齐。

### 3.4 Gateway：路由匹配 + 中间件链（`gateway.cpp`）

**路由优先级 =「Kind 枚举序 → priority 降序 → 注册序升序」**：
`Kind { Custom=0, Static=1, Param=2, Wildcard=3 }`（`gateway.cpp:580`），`Match`（`gateway.cpp:747`）
先按方法过滤，再 `stable_sort`（`gateway.cpp:758-772`），最后逐个 `MatchPath` 试。
所以 **Custom > Static > Param > Wildcard**；`priority` 只在**同 Kind** 内起作用——
`priority=100` 的通配路由压不过 `priority=0` 的静态路由。这是最容易踩的一条。

同名覆盖：同 Kind + 同 pattern + 同 method_mask 时**复用原 `order` 原地替换**
（`gateway.cpp:700-708`），保证热更新不改变匹配顺序。

`MatchPath` 三种形态（静态全等 / 分段参数 / 前缀通配）。通配只允许 `**` 作最后一段，
且不能与 `:param` 混用（`gateway.cpp:644-666`）。

**Dispatch（`gateway.cpp:835`）的顺序**：

1. `erase(kAuthHeader); erase(kUserHeader)`（`gateway.cpp:835-843`）——客户端传的内部头一律作废。
2. `SnapshotMiddlewares()` 复制链（避免持锁跑用户代码）。
3. 选 tail：`/healthz` → 固定 JSON；`/healthz/all` → `AggregateHealth()`；否则 `Match`，
   404 直接返回并仍走 `ApplyDecorate`；命中且 `require_auth` ⇒ **框架自己写 `kAuthHeader=1`**。
4. `InvokeChain(chain, 0, ...)` 递归下行（`gateway.cpp:560`），tail 是路由 handler。
5. 链返回错误 ⇒ 统一 500；再统计 401/429；最后 `ApplyDecorate`（可改写状态/头，但
   **禁止改 `Content-Length`/`Transfer-Encoding`**，`gateway.cpp:901` 附近有显式 erase）。

**内置中间件顺序**（`gateway.cpp:1262-1269`）：`AccessLog → RateLimit → Auth`，日志最外层。
`Gateway::Use` 是尾插（`gateway.cpp:1287`），所以「Create 之后 Use」的中间件排在 Auth **之后**
（更内层）。

**连接处理**：`Run`（`gateway.cpp:1401`）accept 后加锁 `active[ptr]=conn` + 入队 + `notify_one`；
队列满直接 `close`。`ServeConnection`（`gateway.cpp:1096`）是**一连接一请求**：
解析 → `Dispatch` → `SerializeResponse` → `WriteAll` → 关闭，**不支持 keep-alive**（`:1123` 起）。

### 3.5 传输侧两件套：HTTP 隧道与实时房间

**HTTP 隧道**把帧流塞进请求/响应 body，适合「只有 HTTP 能过」的网络。

- 服务端 `BatchTransport`（`http_tunnel.cpp:87`）只是两个 `vector`：`m_in`（本次请求 body）
  + `m_out`（本次响应 body）；`Read` 从 `m_in` 拷、`WriteAll` 追加 `m_out`，`Feed()` 换 body、
  `TakeOutput()` 取走响应。客户端 `HttpTunnelClientTransport`（`http_tunnel.cpp:21-78`）纯适配：
  `WriteAll` 攒请求、`Read` 从上次响应剩余取，取空返回 0（不是 -1），`-1` 只在 `m_closed` 时给。
- `HandleRequest`（`http_tunnel.cpp:165`）的核心是**步进循环**：

  ```cpp
  Feed(body);
  for (i < max_steps) {
      before = output_size();
      frames = m_conn->Step(0);          // 非阻塞推进
      if (frames < 0) break;             // 连接已关
      if (InputEmpty() && frames==0 && output_size()==before) break;  // 无进展→收工
      if (InputEmpty() && output_size() > 16MiB) break;               // 病态输入保护
  }
  return TakeOutput();
  ```

  「连续一步无进展」是终止条件而非固定步数，正常请求 1-2 步返回。16 MiB 上限
  （`http_tunnel.cpp:182`）防「对端一直发、响应无限涨」把 worker 内存打爆。

**实时房间**（`realtime_room.cpp`）的取舍很明确：**每连接一条发送线程 + 有界队列**。

- `Conn::Enqueue`（`realtime_room.cpp:70`）队列满时**不阻塞、不只丢单条**，而是
  `m_overflow=true; m_stopping=true; m_queue.clear()` 并返回 false（`realtime_room.cpp:78-90`）
  ——慢消费者整条踢掉，避免一条卡死的连接拖住整个房间。
- `Broadcast`（`realtime_room.cpp:255`）锁内逐连接 `Enqueue`，锁外释放已结束的 `Conn`
  （`realtime_room.cpp:272`）——因为 `~Conn` 会 `join`，锁内析构就是持锁等线程。
- `Join` 也是**锁外 `conn->Start()`**（`realtime_room.cpp:223`），避免线程启动被 hub 锁串行化。
- `Leave` 幂等：`room_of` 找不到直接返回（`realtime_room.cpp:230-234`）。

### 3.6 帧编解码与桥接（改协议时必看）

- `kFrameHeaderBytes = 9`、`kMaxFramePayloadLimit = (1<<24)-1`（`frame.hpp:69,75`）。
- `FrameDecoder::NextView`（`frame.cpp:265`）返回**借用视图**，这是 `Step` 禁止重入的根因；
  `Next`（`frame.cpp:239`）才拷贝，`Append`（`frame.cpp:222`）追加，`Clear`（`frame.cpp:287`）重置。
  `DecodeImpl`（`frame.cpp:54`）是唯一解析点，三个重载都汇到它（`frame.cpp:174/179/185`），改帧头只改一处。
- `bento_bridge::Decoder`（`bento_bridge.cpp:151`）把 `app::Frame` 包进 `bento::net` 帧：写侧算
  checksum（`:76`）、读侧 `Decode`/`DecodeView`（`:90/120`），**只做格式搬运**不做流控。
  `BentoLinkTransport::WriteAll`（`bento_link_transport.cpp:61`）按 `m_max_message` 切块（`:79`），
  `Read`（`:102`）用 `m_cv.wait_for` 等数据（`:115`），`Install()`（`:29`）注册 `OnLinkData`。

---

## 第四部分 · 数据与性能

**app 模块没有独立台账条目**：`docs/metrics-ledger.md` 无 app 专属 M 编号。只能借相邻条目，
且**不得**把它们当成 app 框架本身吞吐：

| 编号 | 台账内容 | 与 app 的关系 | 依据 |
|---|---|---|---|
| M05 | HTTP/1.1 pipeline=16 单连接 rps | `HttpTunnelServer` 走 `bento::HttpServer`，上限由它决定，**不是** app 帧层 | `metrics-ledger.md:120` |
| M05b | 「HTTP pipeline 411k rps」 | 台账判为 `无出处`；引用 app 隧道性能时**不要**用这个数 | `metrics-ledger.md:131` |
| M15 | bento/link 包吞吐（single 282,210 pps 等） | `bento_link_transport` 的底层上限；STREAM 64 B 消息级，**bulk 字节吞吐无基准** | `metrics-ledger.md:234` |

源码里可见的**结构性能事实**（不是实测数字，只是实现特征）：

- `PickNext` 命中堆顶时**不做堆操作**，稳态 O(1)；WFQ 计费 2 的幂权重走移位查表，非 2 的幂才 `idiv`。
- `pending` 缓冲与 payload 容量**跨帧复用**（`stream_table.cpp:641` 附近、`connection.cpp:545`），
  稳态发送路径无每帧分配；资源整块直通省一次全量 memcpy（1 GiB 资源即省 1 GiB 搬运）。
- `Step` 单次最多 64 帧（`connection.hpp:104`）、单次 flush 预算 2 MiB（`connection.hpp:105`），
  两者共同决定单次 `Step` 的最坏工作量。
- **未实测**：app 帧层 pps / 字节吞吐、`realtime_room` 广播扇出上限、gateway 单 worker QPS——
  都没有可信数据，谁要给数字必须自己压。

---

## 第五部分 · 边界与陷阱

1. **默认不构建**。`option(GO2CPP_WITH_APP ... OFF)`（`cmake/Go2CppApp.cmake:16`）——不加
   `-DGO2CPP_WITH_APP=ON`，`src/app/*.cpp` 根本不参与编译，改完以为生效其实没编。
   测试开关 `GO2CPP_APP_TESTS` 默认 **ON**（`:86`），但整块被 `if (GO2CPP_WITH_APP AND
   GO2CPP_APP_TESTS)`（`:88`）包着，WITH_APP=OFF 时测试也不存在。

2. **窗口溢出会直接杀掉整条连接**。`OnWindowUpdate` 在 increment 为 0 / >`kMaxStreamId` /
   累加超 `kMaxWindow` 时返回 false（`stream_table.cpp:858-884`），调用点按连接级错误处理。
   对端恶意或有 bug 地连发大 increment 时，不是「忽略这次更新」而是**断连**。

3. **KV 大表内存放大 2-3 倍**。`Partition`（`kv_stream.cpp:76`）先把整表拆成 `small`+`large`
   两份 `KvTable`（各一份字符串拷贝），再 `large.ToBinary()` 生成第三份 body（`kv_stream.cpp:121`）。
   一张 64 MiB 的表在首帧瞬间可能吃到 ~192 MiB。要发超大表应走 `SendBytes` 自己流式切，
   不要指望 `KvProducer`。

4. **续传的两个硬约束**。①`Finish` 只在 `!m_partial` 时比对整文件哈希（`resource.cpp:701-750` 区间）
   ——partial 模式只校验每块 CRC，若上次会话写坏了前 N 块，本端**不会发现**，要端到端保证得由
   上层在续传前自己核对前 N 块。②`OnData` 只接受 `index == m_completed_chunks`
   （`resource.cpp:611-673`），重复即 duplicate、跳跃即 gap，**不支持乱序/单块重传**；
   要加选择性重传等于改协议语义，必须同步改对端与 `ResourceIndex`。

6. **`Step` 在事件回调里重入是空操作**。`connection.cpp:333-341` 直接 `return 0`。
   在 `SetEventHandler` 回调里「顺手推一下 IO」不推进任何东西，必须回主循环再 `Step`。

7. **SHA-NI 不在 app 里**。`sha256_ni.hpp` 是兼容壳（该文件 `:10-17` 注释写明 2026-11-02
   下沉到 `hash` 模块），`HardwareAvailable()` 只转调 `hash::Sha256::HardwareAccelerated()`
   （`sha256_ni.hpp:45`）。想在 app 侧「优化哈希」是改错地方；CPU 不支持 SHA-NI 时由 `hash`
   内部回退标量实现，app 无感。**回退路径的具体实现未核实**（在 `src/hash/sha256.cpp`）。

8. **Gateway 路由优先级不看跨 Kind 的 `priority`**。Kind 枚举序优先（`gateway.cpp:580,758-772`），
   所以 `/**` 的 `priority=999` 依然排在 Static 之后。想要「高优先级兜底」只能注册成
   Custom Matcher（`AddCustomRoute`，`gateway.cpp:714`）。

9. **Gateway 一连接一请求**。`ServeConnection`（`gateway.cpp:1096`）处理完一个请求就写回并关闭
   （`:1123` 起），没有 keep-alive 复用。压测时 TCP 建连开销占大头，
   不要拿它跟「pipeline=16」的数字直接对比（对照 M05 时注意口径）。

10. **`max_queued_connections` 满时静默 close**（`gateway.cpp:1412-1426`）。客户端只看到连接被关，
    没有 503、没有日志（`accepted=false` 分支只有 `close()`）；`realtime_room` 队列满同理
    （`Conn::Enqueue`，`:78-90`，`overflow()` 只是诊断位，无重连/降级）。要可观测得自己加计数。

---

## 第六部分 · 测试

### 6.1 测试资产盘点（`tests/app/`，14 个文件）

| 文件 | 覆盖什么 | 是否进 CTest |
|---|---|---|
| `app_test_frame.cpp` | 9 B 帧头编解码、非法长度/R 位、`NextView` 借用语义 | ✅ `app_frame` |
| `app_test_stream_table.cpp` | 状态机、双窗口、WFQ 权重、父优先 | ✅ `app_stream_table` |
| `app_test_resource.cpp` | 分块/CRC/SHA-256/续传/坏块 | ✅ `app_resource` |
| `app_test_kv.cpp` | `KvTable` 编解码、三档分派、`KvAssembler` 重组 | ✅ `app_kv` |
| `app_test_connection.cpp` | 握手、事件、端到端收发（`MemoryTransport`） | ✅ `app_connection` |
| `app_test_load.cpp` | 1 GiB 资源 + 1000 并发流 | ✅ `app_load`（TIMEOUT 900） |
| `app_test_bento.cpp` | 帧桥 + Link 真实 UDP 回环 | ✅ `app_bento`（需 `TARGET go2cpp_bento`） |
| `app_test_http.cpp` | `HttpServer` 真实 TCP 帧隧道 | ✅ `app_http`（需 `TARGET go2cpp_bento_http`） |
| `test_gateway.cpp` | 路由/鉴权/透传/超时/502/健康聚合/中间件/Resolver/配置/压测 | ❌ **未进 CTest** |
| `test_realtime_room.cpp` | 加入/离开/广播/慢消费者踢出 | ❌ **未进 CTest** |
| `bench_app_ready.cpp` | `ready()` 就绪耗时基准 | ❌ **未进 CTest** |
| `app_test_support.hpp` / 两个 `build_and_run_*.sh` | 测试夹具 / 手工编译 gateway 与 realtime | — |

「是否进 CTest」依据：`cmake/Go2CppApp.cmake:92-99` 核心列表只有 6 个，
`:136`/`:146` 追加 bento 与 http；`grep -rn 'test_gateway\|test_realtime_room\|bench_app_ready'`
在 `CMakeLists.txt` 与 `cmake/` 下 **0 命中**。所以这三个必须用脚本跑。

### 6.2 运行命令

```bash
# 1) 配置（默认 OFF，必须显式打开；APP_TESTS 默认就是 ON）
cmake -S . -B .build-app -DGO2CPP_WITH_APP=ON -DGO2CPP_APP_TESTS=ON

# 2) 构建
cmake --build .build-app -j

# 3) 跑 app 测试（-R 匹配的是 add_test 名，不是可执行文件名）
ctest --test-dir .build-app -R 'app_' --output-on-failure

# 4) 单项
ctest --test-dir .build-app -R app_stream_table -V

# 5) gateway / realtime / bench（不在 CTest，用脚本）
tests/app/build_and_run_gateway.sh
GO2CPP_BUILD=/path/to/build tests/app/build_and_run_gateway.sh   # 指定已有构建目录
tests/app/build_and_run_realtime.sh
```

注意点：

- `app_load` 的 TIMEOUT 是 900 s，其余 180 s（`cmake/Go2CppApp.cmake:110-128` 区间），跑全量别用默认超时。
- `app_bento` / `app_http` 是**条件目标**：没有 `go2cpp_bento` / `go2cpp_bento_http` 时静默不注册
  （`:135-153`），`ctest -N` 里看不到，不是配置错了。
- `build_and_run_gateway.sh` 默认找 `.build-bento-wsl`（脚本第 11 行 `GO2CPP_BUILD:-$ROOT/.build-bento-wsl`），
  没有该目录直接报错退出；测试编译带 `-Wall -Wextra -Wpedantic -Werror`（`:104`），新文件必须零警告。

### 6.3 改代码时最小回归集

- 动 `frame`/`stream_table` ⇒ `app_frame` + `app_stream_table` + `app_connection`。
- 动 `resource`/`sha256_ni` ⇒ `app_resource` + `app_load`。
- 动 `kv`/`kv_stream` ⇒ `app_kv` + `app_connection`。
- 动 `connection` ⇒ 全部 core 6 个（含 `app_load`）。
- 动 `gateway`/`realtime_room` ⇒ 对应 shell 脚本（唯一入口，见 §6.1）。
- 动 `http_tunnel`/`bento_*` ⇒ `app_bento` + `app_http`（需 bento 目标）。
