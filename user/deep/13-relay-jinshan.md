# Relay / JinShan 基础件（frame / relay / rd_net / collab）· 深入学习

> 面向「要接手改 `include/go2cpp/frame/**`、`include/go2cpp/relay/**`、
> `include/go2cpp/rd_net/**` 以及要接 JinShan 需求的人」。
> 行号在本树用 `grep -n` / `sed -n` 复核（2026-11-03，分支 `feat/isolated` 工作区）；
> 性能只引台账编号，没有就写「无独立台账」，验收记录里的数字不当作性能结论。

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 定位：四块底座 + 一层协作

JinShan 原 relay 是 Go 写的「控制连接 + 数据连接 + 会话表」。迁到 C++ 时需要重写的不是业务，
而是四块**与业务无关的底座**：

| 底座 | 目录 | 解决什么 | 不解决什么 |
|---|---|---|---|
| 帧编解码 | `include/go2cpp/frame/`、`src/frame/` | magic/version/length/CRC、半包粘包、坏帧判定 | 不解析 payload 语义（type 由业务解释） |
| 双向中继 | `include/go2cpp/relay/relay_pipe.hpp`、`src/relay/relay_pipe.cpp` | A↔B 字节搬运、背压、慢 peer 判定 | 不做协议转换、不做鉴权 |
| 连接注册表 | `include/go2cpp/relay/hub.hpp`、`src/relay/hub.cpp` | key → 多条连接，CloseKey 广播 | 不感知连接存活、不做负载均衡 |
| 租约/心跳 | `include/go2cpp/relay/lease.hpp`、`src/relay/lease.cpp` | 到期回调、周期心跳、死亡判定 | 不做选主（选主在 distributed） |

第五块是 `include/go2cpp/rd_net/`（`socket` / `event_loop` / `protocol` / `reliability`），
它对应 JinShan 需求里的 REQ-006～REQ-009（Socket 抽象、事件循环、协议注入、可靠传输骨架），
**浅层 `user/docs/13-relay-jinshan.md` 完全没覆盖**（本轮 `grep -c rd_net user/docs/13-relay-jinshan.md`
返回 0），本文补这一段。

与浅层文档的分工：

| 文档 | 回答什么 |
|---|---|
| `user/docs/13-relay-jinshan.md` | 四块底座的 API、用法示例、设计全文入口、已知限制 |
| 本文 | 每步落在哪一行、RelayPipe 的队列/看门狗怎么配合、rd_net 的线程模型、collab 的 REQ 交付边界 |

协作层 `collab/` 不是代码：它是 Go2Cpp 与 JinShan 两个 agent 交换需求/验收的地方
（`collab/README.md:1` 起）。当前状态见 `collab/acceptance.md:3`（v3，2026-11-03 16:30）：
REQ-001/002/003/005 已实现并验收，REQ-004 通过；旧 REQ-006～013 中除 006～009 外
多数被判定为「专属」移出（`collab/acceptance.md:19`）。

### 1.2 入口点清单

| 入口（类/函数） | 文件:行 | 谁调用它 | 触发什么 | 返回什么 |
|---|---|---|---|---|
| `frame::FrameSpec` | `include/go2cpp/frame/frame_codec.hpp:187` | 调用方 | 配 magic/version/header_len/max_payload/checksum | 值类型 |
| `MakeMagic("RDRL")` | `frame_codec.hpp:227` | 调用方 | 编译期校验 4 字符并转 4 字节 | `array<uint8_t,4>` |
| `MakeFrameSpec(...)` | `frame_codec.hpp:243` | 调用方 | 常用组合的便捷构造 | `FrameSpec` |
| `FrameCodec::FrameCodec(spec)` | `frame_codec.hpp:293` | 每条连接 | 保存 spec；非法 spec 后续全部 `kInvalidSpec` | 对象 |
| `FrameCodec::Encode` | `frame_codec.hpp:310`（实现 `src/frame/frame_codec.cpp:129`） | 发送侧 | 填头/长度/CRC，输出整帧 | bool + Error |
| `FrameCodec::Feed` | `frame_codec.hpp:327`（实现 `src/frame/frame_codec.cpp:186`） | 接收侧 | 追加缓冲、切出所有完整帧 | bool + frames + consumed |
| `FrameCodec::Reset` | `frame_codec.hpp:334`（实现 `src/frame/frame_codec.cpp:364`） | 重连 | 丢半包、释放缓冲 | void |
| `relay::RelayPipe::RelayPipe` | `include/go2cpp/relay/relay_pipe.hpp:200`（实现 `src/relay/relay_pipe.cpp:735`） | 中继服务 | 归一化配置、建两个方向 | 对象 |
| `RelayPipe::Run` | `relay_pipe.cpp:745` | 中继服务 | 起两个方向线程 + 看门狗，阻塞至结束/取消 | bool + Error |
| `RelayPipe::Stats` | `relay_pipe.cpp:837` | 诊断 | 原子读四个计数 | `PipeStats` |
| `RelayPipe::Config` | `relay_pipe.cpp:847` | 诊断 | 读归一化配置 | `const PipeConfig&` |
| `Hub::Register` | `include/go2cpp/relay/hub.hpp:91` 类内（实现 `src/relay/hub.cpp:27`） | 接入层 | key 下追加连接（同对象去重） | bool |
| `Hub::Lookup` | `src/relay/hub.cpp:44` | 业务 | 取快照（shared_ptr 副本） | vector |
| `Hub::Unregister` | `hub.cpp:55` | 连接退出 | 按对象身份移除，幂等 | void |
| `Hub::CloseKey` | `hub.cpp:77` | 运维/踢人 | 锁内摘表、锁外逐个 Close | bool |
| `Hub::Keys` | `hub.cpp:99` | 诊断 | 字典序 key 快照 | vector |
| `Lease::Lease(timeout, cb)` | `include/go2cpp/relay/lease.hpp:94`（实现 `src/relay/lease.cpp:51`） | 会话 | 起计时线程，到期回调一次 | 对象 |
| `Lease::Renew` | `lease.cpp:61` | 心跳 | 重置 deadline、`++generation` | void |
| `Lease::Stop` | `lease.cpp:77` | 析构 | 停线程，幂等，之后不再回调 | void |
| `Heartbeat::Start(ctx, send, on_dead)` | `include/go2cpp/relay/lease.hpp:163`（实现 `src/relay/lease.cpp:148`） | 会话 | 起周期线程，send 失败触发 on_dead 一次 | void |
| `rd::net::EventLoop::Create` | `include/go2cpp/rd_net/event_loop.hpp:56` | 应用 | 建 epoll/eventfd 等 | `Result<unique_ptr>` |
| `EventLoop::AttachDatagram/AttachStream` | `event_loop.hpp:64/67` | 应用 | 把 fd 与 handler 绑进 loop | bool |
| `EventLoop::ScheduleAfter` | `event_loop.hpp:80` | 插件 | 定时器，loop 线程回调 | timer id |
| `EventLoop::Post` | `event_loop.hpp:90` | 其它线程 | 唯一跨线程投递入口 | void |
| `EventLoop::Run/Stop` | `event_loop.hpp:95/98` | 应用 | 阻塞跑 tick / 请求停止 | void |
| `rd::net::Protocol` | `include/go2cpp/rd_net/protocol.hpp:70` | 插件作者 | 继承并实现 OnDatagram/OnTimer/OnSendComplete | 纯虚 |
| `StreamProtocol` | `protocol.hpp:124` | TCP 插件 | 流式版本的回调集合 | 纯虚 |
| `AttachProtocol` | `protocol.hpp:191/196` | 应用 | 把协议挂到 loop，返回 attachment | `unique_ptr` |
| `DatagramSocket::SendTo/SendBatch/RecvFrom` | `include/go2cpp/rd_net/socket.hpp:153/190/197` | 插件 | 四态错误模型的非阻塞 IO | `IoStatus` |
| `StreamSocket::Open/Send` | `socket.hpp:236/243` | 插件 | 非阻塞连接 + 部分发送 | `Result`/`IoStatus` |
| `SeqNum` / `RttEstimator` / `RetransmitQueue` | `include/go2cpp/rd_net/reliability.hpp:23/63/101` | 可靠层 | 序号比较、RFC 6298 RTO、按 deadline 重传 | 值/容器 |
| `CongestionController` | `reliability.hpp:168` | 策略实现 | 纯虚：OnAck/OnLoss/Window | 纯虚 |

### 1.3 总架构图

```
                 业务（JinShan relay / 测试）
   ┌───────────────┬──────────────────┬──────────────────┐
   ▼               ▼                  ▼                  ▼
FrameCodec     RelayPipe            Hub              Lease/Heartbeat
(每连接一个)   A↔B 双向中继        key→连接表        到期/心跳线程
   │               │                  │                  │
   │          [有界队列]              │                  │
   │          reader→queue→writer     │                  │
   │          watchdog(5ms)           │                  │
   └───────────────┴──────────────────┴──────────────────┘
                              │
                    rd::net（可选底座）
   EventLoop ── Attach ──> Protocol（OnDatagram/OnTimer/OnSendComplete）
      │                         │
      └── DatagramSocket / StreamSocket（非阻塞 + 批量）
                              │
                    reliability（SeqNum/RttEstimator/RetransmitQueue/
                                   CongestionController 纯虚）
```

线程与所有权边界：

- `FrameCodec` **有状态、不可拷贝/移动**（`frame_codec.hpp:296-299` 的删除声明），
  一个实例同一时刻只能被一个线程 `Feed`；`Encode` 只读 spec，可并发（头注释说明）。
- `RelayPipe` 每个方向一条读线程 + 一条写线程，共 2 条；另有一条看门狗线程
  （`RelayPipeState` 的 `watchdog` 成员，见 `relay_pipe.cpp:114-175`；`WatchdogLoop` 在 694）。
- `Hub` 一把 `m_mutex`，只在表操作期持有；`CloseKey` 明确「锁内摘表、锁外 Close」
  （`hub.cpp:77-97`），避免 Close 回调 Hub 时死锁。
- `Lease`/`Heartbeat` 各一条后台线程；停止时若在自身线程上则 detach
  （`lease.cpp:226-237` 的 `JoinWorker`）。
- `rd_net` 的 `EventLoop` 是单线程 Reactor（`Run` 在调用线程跑），`Post` 是唯一跨线程入口。

### 1.4 场景 A：FrameCodec 处理半包与粘包（12 步）

1. 调用方用 `FrameSpec` 指定 magic/version/header_len/max_payload/checksum
   （`frame_codec.hpp:187-199`）；默认 12 字节头、64 KiB 载荷上限
   （常量在 `frame_codec.hpp:92/94/96`）。
2. `MakeMagic` 在编译期检查长度（`frame_codec.hpp:227`），避免写入「RDR」这种半个 magic。
3. 构造 `FrameCodec`；构造时校验 spec，非法则后续操作返回 `kInvalidSpec`
   （`frame_codec.hpp:293` 前的注释）。
4. 接收循环把 `read()` 到的字节直接喂 `Feed`（`frame_codec.hpp:327`，实现 186）。
5. `FeedImpl`（`src/frame/frame_codec.cpp:219`）把新字节追加到内部 `m_buffer`。
6. 只要缓冲 ≥ `header_len`，`ValidateHeader`（`frame_codec.cpp:302`）检查 magic/version/length；
   length 越界立即报错，不截断。
7. 完整帧由 `ExtractFrame`（`frame_codec.cpp:337`）校验 CRC、取出 payload，追加到 `frames`。
8. `Feed` 正常返回时 `consumed == len`（全部接管）；半包是正常路径：返回 true、frames 为空。
9. 出错时内部缓冲已清空，调用方**必须断开连接**（头注释明确写了这一点）。
10. 半包不代表错误：下次 `Feed` 继续拼；`BufferedBytes()` 可用于诊断（`frame_codec.hpp:341`）。
11. 重连/换协议调 `Reset`（`frame_codec.cpp:364`），只清重组状态，不动 spec。
12. 错误码分四类：magic/version/length/checksum（`frame_codec.hpp:125` 的 `ErrorCode`），
    调用方可区分处理；`frame::Error` 是值类型，不是 `go2cpp::Error` 接口
    （`frame_codec.hpp:147`，头注释也强调）。

失败路径：spec 非法 → 所有操作 `kInvalidSpec`；payload 超限 → `kPayloadTooLarge`；
CRC 错 → 对应错误码并清空缓冲；`len==0` 且 `data==nullptr` 合法（头注释）。

### 1.5 场景 B：RelayPipe 一次中继从启动到取消（14 步）

1. 调用方构造 `RelayPipe(a, b, cfg)`（`relay_pipe.cpp:735`）；构造只做配置归一化，
   不启动线程。
2. `NormalizeConfig`（`relay_pipe.cpp:75`）把 0 值回落默认：`copy_buf=32KiB`（64）、
   `queue_cap=256KiB`（65）、`write_timeout=30s`（66）；并保证 `queue_cap >= copy_buf`（82-84）。
3. `Run(ctx, err)`（`relay_pipe.cpp:745`）先清空 `*err`。
4. 检查两端非空（750-755），空指针直接返回错误。
5. `run_started` 用 CAS 保证只跑一次（757-765），重复调用返回
   `"relay: Run already called"`。
6. 归一化上下文：空指针等价 `Context::Background()`（768）；已取消则 `ForceCloseAll` 并返回（769-774）。
7. 注册取消回调：只捕获 `shared_ptr<State>`，不捕获 RelayPipe、不访问 Stream
   （注释在 776-778），回调只置原子标志 + notify。
8. `StartWatchdog`（788）起看门狗线程；两个方向各 `Start()`（789-790）。
9. `Run` 在 `done_cv` 上等「两个方向都结束」或「取消」（793-799）。
10. 若取消且未完成 → `ForceCloseAll()`（800-801）强关两端；随后 join 两个方向（804-805）、
    停看门狗（806）、移除取消回调（807）。
11. 汇总 `Clean()`/`Error()`（809-816）：两个方向都干净 EOF 才算成功。
12. 取消语义（818-825）：取消且（未完成或非干净）→ 返回 false，错误取 `ctx->Err()`。
13. 非取消但有方向失败 → 返回第一条错误（826-831）。
14. 数据路径：读线程 `Read` → `ChunkQueue::Push`（`relay_pipe.cpp:526`）；写线程 `Pop`（569）
    → 循环 `Write` 直到 chunk 写完；队列满时读线程阻塞（背压），持续满则 `kStalled` →
    `Abort(TimeoutError, true)`（527-533）。

慢 peer 判定不是「单次写超时」：看门狗每 5ms（`kWatchdogTick`，`relay_pipe.cpp:72`）扫两个
写槽位，超过 deadline 就 `NoteTimeout` 并 `Close` 目标（694-720）；写线程自己也有
`kMaxNoProgress=100` 的零进展上限（69）。

### 1.6 场景 C：Hub 广播关闭（10 步）

1. 接入层 `Register(key, conn)`（`hub.cpp:27`）：空连接 false；同 key 下同对象去重（31-41）。
2. 业务 `Lookup(key)`（44）拿 `shared_ptr` 快照；遍历期间连接不会被 Hub 并发销毁（45-52）。
3. 连接退出 `Unregister(key, conn)`（55）：按对象身份 erase（65-70），最后一条删掉后 key 消失。
4. 运维调 `CloseKey(key)`（77）：锁内把整个 `ConnList` move 出来并 erase（80-86）。
5. 解锁后逐个 `Close()`（`hub.cpp:77-97`），所以 Close 里可以安全回调 Hub。
6. 返回时 key 已不可见，`Lookup` 保证为空。
7. 同一条连接挂多个 key 时，每个 key 各 Close 一次，依赖 Close 幂等（hub.hpp 注释）。
8. `Close()` 的错误被忽略：Hub 不做 IO 错误处理。
9. `Keys()`（99）返回排序后的快照，便于断言/日志。
10. Hub 不感知连接状态：已关闭的连接仍可登记，调用方保证不这么做（hub.hpp 注释）。

### 1.7 场景 D：rd_net 一个 tick 里发生了什么（10 步）

1. 应用 `EventLoop::Create()`（`event_loop.hpp:56`）建 loop；失败返回 `Err(kIoError, errno)`。
2. `AttachDatagram/AttachStream`（64/67）把 fd 与 handler 绑进 slot；重复绑定只更新 handler。
3. `AttachProtocol`（`protocol.hpp:191`）把 `Protocol` 包装成 `ProtocolAttachment`，
   负责把 loop 层回调转成 `ProtocolContext` 版本。
4. `Run()`（95）进入循环：算 poll 超时 → poll → 处理 IO 事件 → `FireDueTimers` →
   `DrainPostedTasks`（私有方法声明从 `event_loop.hpp:104` 起）。
5. 每个 tick 序号自增，回调里 `tick_count()` 读到的是本 tick 的序号（`event_loop.hpp:94`）。
6. 定时器：`ScheduleAfter`（80）返回非 0 id；`CancelTimer`（84）惰性删除，已触发的返回 true
   但不再执行。
7. 跨线程 `Post`（90）把任务投进队列并唤醒 poll；`Run` 返回后未执行的任务被丢弃。
8. 数据报到达：`Protocol::OnDatagram(context, data, from)`（`protocol.hpp:89`），
   `data` 是借用视图，回调返回后失效。
9. 发送完成：`OnSendComplete(context, token, result)`（`protocol.hpp:109`），
   token 与 `SendTo` 返回值一致。
10. 摘除：`OnDetached(context, reason)`（`protocol.hpp:117`），reason 见
    `DetachReason`（`protocol.hpp:53`）；之后不会再有回调。

---

## 第二部分 · 分：逐块细讲

### 2.1 FrameCodec：帧格式与错误分类

帧头最小 12 字节 = magic(4) + version(1) + type(1) + flags(2) + length(4)
（`frame_codec.hpp:92` 的常量注释）；`kMaxHeaderLen=4096`（98）留扩展区；
`kChecksumBytes=4`（102）只在启用 checksum 时存在（帧尾）。

`FrameSpec::MaxFrameBytes()`（`frame_codec.hpp:210`）把「头 + 载荷 + 校验和」算成上界，
`Feed` 用它做缓冲上限，避免恶意长度导致无限增长。

`Frame` 结构（`frame_codec.hpp:168`）只有 `type/flags/payload`，**不解析业务语义**；
JinShan 的 RDRL/RDCH 共用同一套机制，只是 `FrameSpec.magic` 不同（`collab/jinshan-requests.md:12`
的 REQ-001 验收要求「两种 FrameSpec 各跑 23 种 type」）。

CRC 复用 `hash::Crc32::ChecksumIEEE`（`frame_codec.hpp:116` 的 `Crc32IeeeFrameChecksum`），
不另造轮子；与 zlib/Go 的 IEEE 多项式一致（头注释）。

### 2.2 RelayPipe：队列、看门狗与统计

队列是「有界字节队列」：`queue_cap` 按字节计（`relay_pipe.hpp:148-153`），
队列里每个 chunk 是一个 `vector<char>`，上限 `copy_buf`。`Push` 返回三种结果
（`relay_pipe.cpp:249-301`）：`kOk`、`kClosed`、`kStalled`。`kStalled` 表示
「持续满」而不是瞬时满，由 `Push/Pop/DiscardAll` 维护状态（362 附近注释）。

统计四个计数器都是 relaxed 原子（`RelayPipeState` 字段在 `relay_pipe.cpp:126-132`）：
`a_to_b`、`b_to_a`、`dropped`、`timeouts`；`Stats()`（837）在运行期读不阻塞。

超时错误是哨兵：`ErrTimeout()`（`relay_pipe.hpp:178`）故意泄漏，便于 `Is(err, ErrTimeout())`
跨包装识别；不要比较文本。

### 2.3 Hub：为什么必须「锁外 Close」

`CloseKey` 的实现顺序是刻意的（`hub.cpp:77-97`）：如果持 `m_mutex` 调 `conn->Close()`，
而 Close 内部又回调 `Hub::Unregister`（很常见的写法），就会自死锁。所以先 move 出列表、
erase key，再解锁逐个 Close。这条约束也适用于任何新增的「遍历并回调」接口。

`Hub::Stream` 直接复用 `streams::Closer`（`hub.hpp:98` 的 `using Stream`），
不新造接口；`RelayPipe` 的 `relay::Stream`（Reader+Writer+Closer）可隐式转换。

### 2.4 Lease / Heartbeat：代际计数与弱引用

`Lease` 用 `m_generation` 区分「醒来是因为续约」还是「真到期」（`lease.hpp` 字段注释，
`lease.cpp:104-105` 读 generation 与 deadline）。没有这个计数，条件变量的虚假唤醒
或一次恰好落在 deadline 上的 Renew 会被误判为到期。

`Heartbeat` 的取消回调只持 `weak_ptr<State>`（`lease.cpp:168-175`），
即使 `Context` 的回调晚于对象销毁执行也不会碰已释放内存；`JoinWorker` 在自身线程上
detach 而不是 join（226-237），避免 `std::system_error`/死锁。

### 2.5 rd_net：Socket 四态与批量

`IoStatus`（`socket.hpp:40`）是四态错误模型：Ok / 可重试 / 已关闭 / 错误（具体枚举值见
头文件 40-52）；`SendResult`（62）带 token 与已发字节数。

`DatagramSocket::SendBatch`（190）一次提交多个段；`StreamSocket` 的 `Send` 明确可能部分发送
（243 的注释：kOk 时用 `last_transferred()` 推进视图）。批量入口 `WriteBatch` 走
`writev`，不受 `DialOptions::batch_*` 影响（245 一带注释）——后者只控制单条 Send 的隐式切段。

`EventLoop` 的实现文件 `src/rd_net/event_loop.cpp` 601 行、`socket.cpp` 1030 行、
`protocol.cpp` 383 行（`wc -l` 实测）；它们不在主 CMake 里统一构建，测试靠
`tests/rd_net/build_and_run_*.sh` 手工编译（脚本第 5-13 行可见 g++ 命令与链接库）。

### 2.6 reliability：只给骨架，不给策略

`SeqNum`（`reliability.hpp:23`）用无符号回绕比较（`operator<` 在 35-37），
`RttEstimator`（63）严格按 RFC 6298：首样本 `srtt=R, rttvar=R/2`，之后
`rttvar=3/4*rttvar+1/4*|srtt-R|`、`srtt=7/8*srtt+1/8*R`、`RTO=srtt+4*rttvar`，
夹在 `[initial_rto, max_rto]`（`reliability.hpp:55-58` 的参数）。
`RetransmitQueue`（101）只按 deadline 存/取，不决定重传次数；同 token 再 Push 视为替换。
`CongestionController`（168）是纯虚：`OnAck/OnLoss/Window`，策略由使用方实现——
这是 REQ-009 说的「通用骨架」。

### 2.7 数据与性能

- **无独立台账**：`docs/metrics-ledger.md` 无 frame/relay/rd_net 条目（本轮
  `grep -n 'relay\|frame\|rd_net' docs/metrics-ledger.md` 无命中）。
- M15（bento/link 包吞吐）测的是 `tests/bento/` 的 link，不是 `relay::RelayPipe`，
  **不能拿来做本模块的性能证据**。
- `tests/relay/bench_relay.cpp` 与 `go2cpp_relay_bench`（`cmake/Go2CppRelayHubLease.cmake:96`）
  存在，但本文未运行，不报数字。
- 结构性事实：RelayPipe 每方向一次 `Read` + 一次队列 Push/Pop + 一次或多次 `Write`；
  队列上限 256 KiB，超出即背压到读线程，不会无界增长。

### 2.8 边界与陷阱

1. **FrameCodec 不可拷贝**（`frame_codec.hpp:296-299`）：想让两条连接共享一个实例会在
   编译期失败，这是有意的——半包状态会被串流。
2. **`Feed` 的 `consumed` 语义易错**：成功恒等于 `len`（全部接管），不是「解析掉的字节」；
   半包时也是 `len`。
3. **坏帧后必须断开**：`Feed` 返回 false 时内部缓冲已清空，继续用同一实例会把后续字节
   当成新流的开头，得到看似合法的错误帧。
4. **`max_payload` 是硬上限**：超限报错不截断（头注释），不要指望它帮你分片。
5. **RelayPipe 只能 Run 一次**（`relay_pipe.cpp:757-765`）：要重用必须重新构造。
6. **`queue_cap < copy_buf` 会被静默抬高**（`relay_pipe.cpp:82-84`）：配 1 KiB 队列不会得到
   1 KiB 的背压，实际至少一个 chunk。
7. **慢 peer 只看写 deadline 与队列停滞**，不看你业务层的 ACK；应用层超时归应用层。
8. **Hub::CloseKey 会关闭挂在 key 下的所有连接**，包括你不想踢的；它是广播不是单个。
9. **Hub 不感知存活**：连接已经在别处 Close 后仍留在表里，`Lookup` 会返回死连接。
10. **`Lease` 到期回调最多一次**，`Stop` 之后不会再触发；不要用 `Renew` 去「复活」已经
    触发过回调的租约（源码未提供复活路径）。
11. **rd_net 的 `OnDatagram` 视图只在回调期间有效**：保存指针会悬空。
12. **`ScheduleAfter` 的定时器在 loop 线程执行**：回调里做阻塞 IO 会拖住整条 loop。
13. **collab 的 REQ 状态不等于性能结论**：`collab/acceptance.md:16` 的 REQ-004 写了
    「1e8 无丢失 + RSS 0.00 MiB 增长」，但那行是验收记录、不是 `docs/metrics-ledger.md`，
    本文不把它当性能证据。

### 2.9 测试

| 层 | 测试文件 | ctest 名/命令 |
|---|---|---|
| 帧 | `tests/frame/test_frame_codec.cpp` | `go2cpp_frame_codec_test`（`cmake/Go2CppFrame.cmake:42`） |
| 中继 | `tests/relay/test_relay_pipe.cpp` | `relay_pipe`（`cmake/Go2CppRelayPipe.cmake:69`，标签 `relay`，TIMEOUT 900） |
| 中继场景 | `tests/relay/test_relay_scenarios.cpp` | `relay_scenarios`（`cmake/Go2CppRelayHubLease.cmake:102`） |
| Hub | `tests/relay/test_hub.cpp` | `relay_hub`（同文件 99） |
| 租约 | `tests/relay/test_lease.cpp`、`test_lease_90s.cpp`、`test_lease_clockjump.cpp` | `relay_lease` / `relay_lease_90s`（标签 `relay;long`）/ `relay_lease_clockjump`（99-113） |
| 心跳 | `tests/relay/test_heartbeat.cpp` | `relay_heartbeat`（101） |
| 压测 | `tests/relay/bench_relay.cpp` | `go2cpp_relay_bench`（96） |
| rd_net | `tests/rd_net/test_{socket,event_loop,protocol,reliability}.cpp` | 手工脚本 `tests/rd_net/build_and_run_*.sh`（未进 ctest，未核实） |

常用命令：

```bash
cmake -S . -B build -DGO2CPP_WITH_RELAY=ON
cmake --build build -j
ctest --test-dir build -L relay --output-on-failure
# 90 秒租约用例（慢）
ctest --test-dir build -R relay_lease_90s --output-on-failure
# rd_net 单测
bash tests/rd_net/build_and_run_protocol.sh
```

### 2.10 改这个模块前的检查清单

- [ ] 加 `FrameSpec` 字段是否让 `MaxFrameBytes()`/`ValidateHeader` 同步（否则越界算错）。
- [ ] 改 `Feed` 的返回语义要同时改所有调用方：半包仍返回 true 是契约。
- [ ] 改 RelayPipe 的取消路径要保证「取消回调不访问 Stream」，否则 Run 返回后可能碰悬空。
- [ ] 任何 Hub 新接口：回调一律锁外调用。
- [ ] 任何 Lease/Heartbeat 改动：确认在自身线程上 Stop 不会 join 自己。
- [ ] 新增 rd_net 协议：`OnDetached` 之后不得再假设收到回调。
- [ ] 接 JinShan 新需求前先看 `collab/acceptance.md` 的状态表，避免重复实现已移出的专属需求。
