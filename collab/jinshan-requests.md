# JinShanProject 需求清单（v2，按「通用/专属/可抽象」重写）

> 版本：v2（2026-10-31 20:40）
> 依据：Go2Cpp 的规则——**只做通用层**；专属能力 JinShan 自己做；**可抽象**的部分由 Go2Cpp 提供机制、JinShan 填语义。
> 上一版（REQ-001..013）大量是 RDRL/RDCH 专属需求，**已按新规则移出**，见文末 §4。
> 本轮只提 **5 条**，全部满足「别的项目（游戏/AI 推理/IM）也会用」这一判定。

---

## 1. 可抽象（Go2Cpp 提供机制，JinShan 填语义）

### REQ-001: FrameCodec —— 可配置帧编解码 + 拆包器

- **类型**：可抽象 · **优先级 P0**
- **接口**：
  ```go
  type FrameSpec struct {
      Magic      [4]byte
      Version    uint8
      HeaderLen  int                    // 12（默认）
      MaxPayload int                    // 65536
      Checksum   func([]byte) uint32    // nil = 不校验；默认 CRC32 IEEE
  }
  type Frame struct { Type uint8; Flags uint16; Payload []byte }

  type FrameCodec struct{}
  func NewFrameCodec(spec FrameSpec) *FrameCodec
  func (c *FrameCodec) Encode(f Frame) ([]byte, error)
  func (c *FrameCodec) Feed(buf []byte) (frames []Frame, consumed int, err error)
  func (c *FrameCodec) Reset()
  ```
- **用途**：JinShan 的 RDRL（magic `RDRL`）与 RDCH（magic `RDCH`）**共用一套机制**，只是 `FrameSpec` 不同。类型/标志的语义由 JinShan 解释。
- **约束**：
  - 处理**半包、粘包**：`Feed` 可多次喂入，返回已完整帧与已消耗字节；
  - `MaxPayload` 越界返回明确错误（不截断、不缓冲膨胀）；
  - Checksum 可插拔（nil / CRC32 / 自定义）；
  - **不解析 payload 语义**，只搬字节。
- **通用性判定**：任何自定义二进制协议（游戏封包、IM 帧、RPC）都需要。
- **验收**：
  1. 两种 `FrameSpec`（不同 magic/上限）各跑 23 种 type 的 encode→Feed→decode，字节一致；
  2. 半包（1 字节分 100 次喂）能正确拼帧；
  3. 粘包（3 帧连发）一次 Feed 返回 3 帧且 `consumed` 准确；
  4. 畸形：magic 错 / version 错 / length 越界 / CRC 错，各返回可区分错误；
  5. 1e6 帧压测无泄漏、无崩溃。

### REQ-002: RelayPipe —— 双向字节中继（背压 + 慢 peer）

- **类型**：可抽象 · **优先级 P0**
- **接口**：
  ```go
  type PipeConfig struct {
      CopyBuf      int            // 32768
      QueueCap     int            // 262144
      WriteTimeout time.Duration  // 30s
  }
  type PipeStats struct { AtoB, BtoA uint64; Dropped, Timeouts uint64 }

  type RelayPipe struct{}
  func NewRelayPipe(a, b io.ReadWriteCloser, cfg PipeConfig) *RelayPipe
  func (p *RelayPipe) Run(ctx context.Context) error
  func (p *RelayPipe) Stats() PipeStats
  ```
- **用途**：RDP 字节流在 server DATA ↔ viewer DATA 之间双向透传。**配对（谁和谁连）由 JinShan 的 Hub 完成，本组件不关心。**
- **约束**：
  - 有界队列 + 背压：满则阻塞或按策略失败，**不允许无界增长**；
  - **慢 peer 只断自身**，不拖垮另一条管道；
  - 写超时触发关闭并计入 `Timeouts`；
  - 不解析内容、不重排、不改写。
- **通用性判定**：TCP 代理、端口转发、隧道、跳板都需要。
- **验收**：
  1. 双向 1e7 字节 byte-exact（SHA-256 两端一致）；
  2. 一端慢读（sleep 读）导致队列满 → 只断该端，另一端继续；
  3. 写 deadline 30s 触发 → `Timeouts+1`；
  4. 并发 4 条管道互不串流。

### REQ-003: Hub —— 连接注册表（key → 多连接）

- **类型**：可抽象 · **优先级 P1**
- **接口**：
  ```go
  type Hub struct{}
  func NewHub() *Hub
  func (h *Hub) Register(key string, conn io.Closer) error
  func (h *Hub) Lookup(key string) ([]io.Closer, bool)
  func (h *Hub) Unregister(key string, conn io.Closer)
  func (h *Hub) CloseKey(key string) error
  func (h *Hub) Keys() []string
  ```
- **用途**：relay 维护 `key → server CONTROL + viewer CONTROL + DATA 槽位`。**配额（≤4）、路由策略、token 语义由 JinShan 自己填。**
- **约束**：并发安全；连接关闭必须幂等；`CloseKey` 清理全部并回调（可选）；不内置业务配额（配额用 REQ-005 的限流组件）。
- **通用性判定**：任何"按房间/频道/用户分组管理连接"的场景都需要（IM 房间、游戏房间、SSE 推送）。
- **验收**：
  1. 并发 1e5 次 Register/Lookup/Unregister 无数据竞争（`-race`）；
  2. `CloseKey` 后 `Lookup` 返回空且连接全部关闭；
  3. 重复 Unregister 不 panic。

---

## 2. 通用（Go2Cpp 已有或部分已有）

### REQ-004: 有界队列 / 背压原语

- **类型**：通用 · **优先级 P0** · **状态：已有（`Chan<T>` + `TrySend`）**
- **用途**：REQ-002 的内部队列；业务侧的任务队列。
- **只需补**：`TrySend` 在满时返回 `false`（而非阻塞）的明确语义文档 + 一个示例。
- **验收**：cap 满时 `TrySend=false`；1e8 次 send/recv 无丢失；RSS 稳定。

### REQ-005: 超时 / 取消 / 租约 / 心跳

- **类型**：通用 · **优先级 P1** · **状态：Context 已有；Lease/Heartbeat 缺封装**
- **接口**（缺的部分）：
  ```go
  type Lease struct{}
  func NewLease(timeout time.Duration, onExpire func()) *Lease
  func (l *Lease) Renew()
  func (l *Lease) Stop()

  type Heartbeat struct{}
  func NewHeartbeat(interval time.Duration) *Heartbeat
  func (h *Heartbeat) Start(ctx context.Context, send func() error, onDead func())
  ```
- **用途**：半帧 30s、写 deadline 30s、RDCH 心跳 30s/90s 判半开。
- **约束**：单调时钟；`onExpire`/`onDead` 只触发一次；与业务超时分离。
- **验收**：90s 无 Renew 触发一次；中途 Renew 重置；系统时钟跳变不影响。

---

## 3. 我们自己做（**不提给 Go2Cpp**）

| 项 | 为什么不做 |
|---|---|
| RDRL 帧语义（type 1–9、错误码 0–14、状态机） | 专属协议 |
| RDCH 应用层协议（23 类型） | 专属协议 |
| 配对状态机（注册→token→DATA 配对→握手） | 业务状态机 |
| `IRDPSRAPI` 封装 / invitation 改写 | 平台专属 |
| 命令白名单 / 文件路径校验 | 安全策略 |
| 控制权状态机（VIEW/INTERACTIVE） | 业务逻辑 |
| JinShan 错误码表 | 业务错误 |
| token 语义（8B 随机 + 管道句柄） | 业务标识（随机源用 Go2Cpp 已有能力即可） |

---

## 4. 上一版被移出的需求（避免 Go2Cpp 重复阅读）

| 旧编号 | 内容 | 处理 |
|---|---|---|
| REQ-004（旧） | RDRL 帧编解码 | → 改为 **REQ-001 FrameCodec**（只保留可抽象部分） |
| REQ-005（旧） | 配对表 | → 改为 **REQ-003 Hub**（只保留注册表，配额/语义自留） |
| REQ-006（旧） | 1:1 中继管道 | → 改为 **REQ-002 RelayPipe**（保留管道机制） |
| REQ-007（旧） | 多客户端配额 | 移出（用 REQ-005 限流或自己计数） |
| REQ-008（旧） | token 生成 | 移出（随机源已有，封装自己做） |
| REQ-011（旧） | RDCH 帧编解码 | 移出（专属；用 REQ-001 的 FrameCodec 自己搭） |
| REQ-012（旧） | RDRL CTRL type 9 | 移出（专属 payload，用 FrameCodec 承载） |
| REQ-013（旧） | 心跳 | → 并入 **REQ-005** |
| REQ-001/002/003（旧） | 有界队列/超时/错误 | 保留为 **REQ-004/005** 与已有能力 |
| REQ-009（旧） | Go 风格 API | 保留（进行中，等完成后验收） |
| REQ-010（旧） | P2P | **明确不需要**（CON-09） |

---

## 5. 建议实现顺序

1. **REQ-001 FrameCodec** + **REQ-002 RelayPipe**（这两条直接解锁 JinShan 的 A1/A4/A5 产品化，且最通用）
2. **REQ-003 Hub**（relay 侧重构用）
3. **REQ-004 / REQ-005**（小件，可穿插）

## 6. 验收方式

- Go2Cpp 交付后在 `collab/acceptance.md` 记录：用例 / 原始输出 / 结论；
- 不通过则在本文追加「REQ-xxx 补充」；
- 示例必须可编译运行（`collab/examples/`）。

---

## v3 新增：C++ 传输层与协议注入（2026-11-03）

> **背景**：v2 的 REQ-001..005 是 **Go 侧 relay** 的需求，已全部验收（见 `acceptance.md` v3）。
> 本节是 **C++ 侧 `rd::net`** 的新需求——JinShan 要把传输层从「TCP 中继」演进到「可靠 UDP」，
> 需要 Go2Cpp 提供**通用传输层 + 协议注入入口**。
> **规则不变**：Go2Cpp 只做通用层，协议语义全部 JinShan 自己填。
>
> **完整规格**（1471 行、27 行对照表、8 个 C++ 接口原型、24 条不做、六道门禁）：
> `collab/examples/REQUIREMENTS-JINSHAN-TRANSPORT-SPEC.md`

---

### REQ-006: 统一的 UDP/TCP Socket 抽象 + 四态错误模型

- **类型**：可抽象 · **优先级 P0**
- **背景**：现有 `rd::net` 只有 `kTimeout` / `kIoError` 两种错误，**RST 与 FIN 不可分**、
  UDP 超缓冲在 Linux 静默截断 / Windows 报错不一致，弱网问题无法定位。
- **接口**（C++17，`rd::net` 命名空间）：

```cpp
enum class IoStatus {
    kOk, kWouldBlock, kTimeout, kClosed, kReset, kTruncated, kIoError, kInvalid
};
struct SendResult { IoStatus status; std::uint64_t token; };

class DatagramSocket {          // UDP
public:
    static Result<DatagramSocket> Open(const Endpoint&, const SocketOptions&);
    IoStatus SendTo(ByteView data, const Endpoint& to, SendResult* out);
    IoStatus RecvFrom(MutableByteView buf, Endpoint* from, std::size_t* received);
    void Close();
};

class StreamSocket {            // TCP
public:
    static Result<StreamSocket> Open(const Endpoint&, const DialOptions&);
    IoStatus Send(ByteView data, SendResult* out);
    IoStatus Recv(MutableByteView buf, std::size_t* received);
    void Close();
};
```

- **约束**：
  1. **四态必须可分**：`kClosed`（对端 FIN）/ `kReset`（RST）/ `kTruncated`（UDP 包大于缓冲）/ `kWouldBlock`（非阻塞重试）；
  2. 非阻塞 + 事件驱动，**不许每连接一线程**；
  3. 缓冲区所有权明确：`ByteView` 借用，`BufferPtr` 移交；
  4. 错误必须可映射到平台 errno/WSA 错误码。
- **通用性判定**：任何做网络编程的项目都需要。
- **验收**：
  1. 对端 `shutdown(SD_SEND)` → `kClosed`；`SO_LINGER=0` + `close` → `kReset`；**两者必须可区分**；
  2. 发 70000 字节 UDP 包到 1024 字节缓冲的接收方 → `kTruncated`（**不是 kOk**）；
  3. 非阻塞满缓冲 → `kWouldBlock`，且**不丢数据**（下次重试成功）；
  4. 1e4 连接压测，`/W4` 0 warning、无泄漏。

---

### REQ-007: 事件循环 + 毫秒级定时器

- **类型**：可抽象 · **优先级 P0**
- **背景**：可靠 UDP 需要 RTO 定时器（毫秒级），现有 `rd::net` 没有事件循环抽象。
- **接口**：

```cpp
class EventLoop {
public:
    static Result<std::unique_ptr<EventLoop>> Create();
    void AttachDatagram(DatagramSocket&, Protocol* handler);
    void AttachStream(StreamSocket&, StreamProtocol* handler);
    std::uint64_t ScheduleAfter(std::chrono::milliseconds delay, std::function<void()> cb);
    void CancelTimer(std::uint64_t id);
    void Post(std::function<void()> task);        // 跨线程投递
    void Run();                                   // 阻塞运行
    void Stop();
};
```

- **约束**：
  1. **所有回调在 loop 线程**（单线程模型，上层不用加锁）；
  2. `ScheduleAfter` 精度 **≤ 5ms**；
  3. **单 tick 每 socket 最多 64 个包**（防饿死其他 socket）；
  4. **单 tick 回调顺序固定**；
  5. Windows 后端选型自由（`select` 可接受，但 **FD_SETSIZE=64 限制必须文档化**）。
- **通用性判定**：任何 IO 密集服务都需要。
- **验收**：
  1. 1000 个定时器 1 秒内到期，**误差 < 5ms**；
  2. 一个 socket 疯狂收包时，另一个 socket 的定时器**仍准时**；
  3. `Post` 从另一线程 1e5 次，**全部执行且同线程内保序**；
  4. Linux/Windows **行为一致**。

---

### REQ-008: 协议注入入口（**核心**）

- **类型**：可抽象 · **优先级 P0**
- **背景**：JinShan 要挂入自定义可靠 UDP 协议（RDT/UDP v1：24 字节头、握手、ACK 位图、
  重传、分片重组），但 **Go2Cpp 不应该理解这些**。
- **接口**：

```cpp
using ByteView = std::string_view;     // 借用，回调期间有效，零拷贝
class BufferPtr;                        // 移交，引用计数

enum class DetachReason { kEof, kReset, kTimeout, kError, kClosed };

class Protocol {                        // UDP 协议插件
public:
    virtual ~Protocol() = default;
    virtual void OnAttached(ProtocolContext&) = 0;
    virtual void OnDatagram(ProtocolContext&, ByteView data, const Endpoint& from) = 0;
    virtual void OnTimer(ProtocolContext&, std::uint64_t timer_id) = 0;
    virtual void OnSendComplete(ProtocolContext&, std::uint64_t token, SendResult) = 0;
    virtual void OnDetached(ProtocolContext&, DetachReason) = 0;
};

class ProtocolContext {
public:
    SendResult SendTo(ByteView data, const Endpoint& to);   // 一律入队，不同步 send
    std::uint64_t ScheduleAfter(std::chrono::milliseconds, std::function<void()>);
    const Endpoint& LocalEndpoint() const;
};
```

- **约束**：
  1. `ByteView` **不拷贝**，回调返回后失效——上层要么当场处理，要么显式 `BufferPtr::Copy`；
  2. `SendTo` **一律入队**，返回 `token`；完成时 `OnSendComplete` 回传；
  3. **Go2Cpp 不解析协议内容**：不认 magic、不校验字段、不做分帧、不做重排、
     不做重传决策、不做握手、不统计协议层丢包；
  4. 回调**全部在 loop 线程**；
  5. 单 tick 每 socket ≤ 64 包。
- **通用性判定**：任何"想用 UDP 但需要自己可靠性"的项目都需要（游戏/实时音视频/IM）。
- **验收**：
  1. **零拷贝**：发送后立刻把源缓冲覆盖成 `0xCC`，对端收到 `0xCC` = 合规；
     收到原值 = 偷偷拷贝了（**判 bug**）；
  2. **不改 Go2Cpp 一行**就能挂入自定义协议（用 20 行 echo 协议验证，`git diff` 为空）；
  3. 上层把 ACK 位图 32→64 位、MTU 1200→9000 → **Go2Cpp 不需要改**；
  4. `SendTo` 入队后立刻返回，不阻塞 loop。

---

### REQ-009: 可靠传输的「通用骨架」（容器 + 公式 + 纯虚，不含策略）

- **类型**：可抽象 · **优先级 P1**
- **背景**：JinShan 的 RDT/UDP 需要重传队列、RTT 估算、拥塞控制。
  这些**框架**通用，但**策略**是协议专属。
- **接口**：

```cpp
template <typename Tag>
class SeqNum {                          // 语义由上层定义
public:
    explicit SeqNum(std::uint32_t v);
    std::uint32_t value() const;
    bool LessThan(SeqNum other) const;  // 无符号回绕安全
    SeqNum operator+(std::uint32_t) const;
};

class RetransmitQueue {                 // 只存，不决定何时重传
public:
    void Push(std::uint64_t token, ByteView data, std::chrono::steady_clock::time_point deadline);
    std::vector<Entry> PopDue(std::chrono::steady_clock::time_point now);
    void Ack(std::uint64_t token);
    std::size_t Size() const;
};

class RttEstimator {                    // RFC 6298 公式，参数由上层传
public:
    RttEstimator(std::chrono::milliseconds initial_rto, std::chrono::milliseconds max_rto);
    void AddSample(std::chrono::milliseconds rtt);
    std::chrono::milliseconds Rto() const;
};

class CongestionController {            // 纯虚，上层实现
public:
    virtual ~CongestionController() = default;
    virtual void OnAck(std::size_t bytes) = 0;
    virtual void OnLoss() = 0;
    virtual std::size_t Window() const = 0;
};
```

- **约束**：
  1. **序号语义由上层定义**：Go2Cpp **不得**假设 `seq == 字节偏移`、`seq == 帧号`、
     `seq 从 0 开始`、`一个 seq 只发一次`；
  2. `RetransmitQueue` **只存 + 按 deadline 返回**，不决定重传几次、不决定 RTO 怎么算；
  3. `RttEstimator` 只实现 RFC 6298 的**公式**（`srtt` 7/8、`rttvar` 3/4、`RTO = srtt + 4*rttvar`），
     **参数由上层传**；
  4. `CongestionController` **纯虚**，JinShan 自己实现 AIMD。
- **通用性判定**：任何可靠 UDP（KCP/QUIC/RDPEUDP）都需要这些容器。
- **验收**：
  1. `SeqNum` 在 `0xFFFFFFFF → 0` 回绕时 `LessThan` 正确；
  2. `RetransmitQueue` 1e5 项，`PopDue` 返回全部到期项且**不重复**；
  3. `RttEstimator` 用 RFC 6298 示例（`srtt=100ms, rttvar=50ms` → `RTO=300ms`）**结果一致**；
  4. 上层把 `CongestionController` 换成 AIMD / CUBIC → **Go2Cpp 不需要改**。

---

## v3 需求汇总

| REQ | 内容 | 类型 | 优先级 | 与 v2 的关系 |
|---|---|---|---|---|
| REQ-006 | UDP/TCP 统一抽象 + 四态错误模型 | 可抽象 | P0 | **新增**（C++ 侧） |
| REQ-007 | 事件循环 + 毫秒定时器 | 可抽象 | P0 | **新增**（C++ 侧） |
| REQ-008 | **协议注入入口** | 可抽象 | P0 | **新增**（C++ 侧，核心） |
| REQ-009 | 可靠传输通用骨架 | 可抽象 | P1 | **新增**（C++ 侧） |

**与 v2 的关系**：v2 的 REQ-001..005 是 **Go 侧 relay**（已验收）；
v3 的 REQ-006..009 是 **C++ 侧 `rd::net`**（新提）。**两套并行，互不替代。**

---

## 三个最容易分歧的点（建议优先讨论）

### 分歧 1：Windows 性能指标 vs 后端选型（**冲突最强**）

- **现状**：Windows 平台层只有 `select` 后端，`FD_SETSIZE=64`
  （`src/platform/windows/io_poller.cpp:98-101`，源码自标 `[未验证]`），**单 loop 上限 63 socket**；
- **我们的要求**：100k pps / 0.9 Gbps 回环；
- **预期冲突**：`select` 很可能到不了，对方会说"指标不可达 / 换 IOCP 要加时间"；
- **我们的立场**：**后端选型自由，指标不因后端放宽**。

### 分歧 2：「可靠骨架」的理解差异

- **Go2Cpp 可能按 KCP/QUIC 思路**认为骨架里该有 ACK 位图、快速重传、重传上限；
- **我们要的是**：纯容器 + 定时器驱动 + 纯虚接口；
- **这是"通用层"定义之争**，需逐条确认（规格 §1.3 已预先裁决）。

### 分歧 3：缓冲区交付形态

- **Go2Cpp 可能倾向** `std::vector<uint8_t>`（每包一次分配 + 拷贝）；
- **我们要的是**：回调期间借用（`ByteView`），零额外拷贝；
- **验收方法**：`memset` 反证法（见 REQ-008 验收 1）。

---

## 建议实现顺序

1. **REQ-006 四态错误模型**（最小改动，先让错误可分）
2. **REQ-007 事件循环**（REQ-008 的前置）
3. **REQ-008 协议注入**（核心，解锁 UDP）
4. **REQ-009 可靠骨架**（最后，可用 REQ-008 验证）

---

## 验收方式

- Go2Cpp 交付后在 `collab/acceptance.md` **追加 v3 章节**，按 REQ-006..009 记录；
- 格式沿用现有：用例 / 原始输出 / 结论；
- 示例必须可编译运行（`collab/examples/`），**不接受伪代码**。

---

## v4 需求（2026-11-04，JinShan 协调者）

> 背景：v3 的 `rd::net` 已在 Linux/WSL2 验证；JinShan 的 Client/Server 是 **Windows**（Win7 SP1+，MSVC v143，`/MT`）。v4 只提「阻塞对接」与「用户明确点名的云上传」两类。

### REQ-010: Windows（MSVC）下的 `rd::net` 可用性 —— P0

- **类型**：可抽象（平台适配，不新增语义）
- **现状**：`include/go2cpp/rd_net/*` + `src/rd_net/*` 只在 WSL2/Linux 编译运行过；`event_loop` 依赖 `poll(2)` + `eventfd`，Windows 无直接对位。
- **需求**：
  1. MSVC 2022 x64 下 `rd_net` 源码零错误编译（`/W4`，`_WIN32_WINNT=0x0601`）；
  2. `tests/rd_net/` 四个脚本在 Windows 下可构建运行，checks 与 Linux 一致（406/44/44/64）；
  3. 明确后端替换：`poll` → `WSAPoll`、`eventfd` → `CreateEvent` 或 socketpair 替代，写清语义差异；
  4. 若 Windows 暂不支持，给「不支持 + 原因 + 替代路径（用现有 WinsockTransport 兜底）」的明确结论。
- **约束**：不引第三方库；不改 `rd::net` 的 Go 风格接口；Win7 SP1 目标。
- **验收**：Windows 构建日志 + 四测试原始输出；或明确不支持的书面结论。
- **参考**：`product/common/src/net/`（JinShan 现有 `Go2CppTransport`/`WinsockTransport`）、`docs/03-architecture/build.md`。

### REQ-011: 云端上传（ResourceTransfer + HTTP 断点续传）example —— P1

- **类型**：可抽象（机制通用，语义 JinShan 填）
- **背景**：JinShan 的 FR-18「备份打包上传」目前是 WinHTTP PUT/POST + 自研 store-only zip，**无断点续传、无分片校验、无并发**；用户明确提示「云端上传在 Go2Cpp 框架内有对应能力体系」。
- **需求**：给一个可编译 example：
  1. 本地目录 → 多个分片（大小可配），逐个 HTTP PUT 到 endpoint；
  2. 失败重试 + 断点续传（服务端返回已收分片清单）；
  3. 每片 SHA-256 校验 + 整体清单校验；
  4. 上传进度回调 `Progress(done, total)`。
- **接口**：基于 `ResourceTransfer`（offerings §六）+ HTTP client（§二），保持 Go 风格。
- **约束**：不引第三方；Windows + Linux 都能跑；≥1 GiB 分片不 OOM。
- **验收**：example 跑通（正常上传 + 断网重试 + 续传 + SHA 对比），附原始输出。
- **参考**：`product/common/src/backup.cpp`、`docs/00-pm/TASK-004`。

### REQ-012（候选，不编号执行）：AI 流式客户端 example

- 若产品要内嵌 AI（当前 FR-13 由外部 MCP 客户端提供 AI），需要 SSE 流式客户端 + MCP over HTTP。优先级 P2，等 REQ-010/011 落地后再评估。

---

## v4 验收方式

- 与 v3 相同：Go2Cpp 交付后在 `acceptance.md` 追加原始输出；JinShan 侧独立复验后签收。
- 平台要求：Windows 结论必须来自真实 MSVC 构建，不接受「Linux 通过即可用」。

### REQ-010 补充：JinShan 侧 MSVC 实测（2026-11-04 11:40，不等回执先自测）

> 结论先行：**实现层已能用于 Windows，测试层还差一个 POSIX 依赖。**

- 环境：Windows 11 26200 / MSVC 2022 BuildTools x64 / `/std:c++17 /W4 /utf-8 /D_WIN32_WINNT=0x0601` / 链接 `ws2_32.lib`
- 源码：`include/go2cpp/rd_net/` + `src/rd_net/`（HEAD `99ed212`）

| 项 | 结果 |
|---|---|
| `socket.cpp` / `event_loop.cpp` / `protocol.cpp` 编译 | ✅ 零错误零警告（`/W4`） |
| 链接三个 obj + 探测 main | ✅ 通过 |
| 运行：`EventLoop::Create()` → `ScheduleAfter(10ms)` → `Stop()` | ✅ 退出码 0，`attached=0 timers=0`，定时器 id=1 |
| `tests/rd_net/test_protocol.cpp` 编译 | ✅ |
| `tests/rd_net/test_reliability.cpp` 编译 | ✅ |
| `tests/rd_net/test_event_loop.cpp` 编译 | ❌ `fatal error C1083: 无法打开包括文件: dirent.h`（fd 泄漏检查用 `opendir`） |

**建议的最小修法**（二选一，请贵方选）：
1. `_WIN32` 下用 `GetProcessHandleCount()` 或 `_getmaxstdio()` 替代 `dirent` 计数；
2. `_WIN32` 下把「200 轮 Create/destroy 后 fd 数不变」这条用例标为 skip 并打印原因，其余用例照跑。

**请回执**：选哪种、以及 `test_socket.cpp`（同类 POSIX 依赖）是否一并处理。

### REQ-010 补充 2：Windows 实跑结果（2026-11-04 11:35）

| 测试 | Windows（MSVC 2022 x64）结果 |
|---|---|
| `test_protocol.cpp` | ✅ 编译 + 运行：`checks=44 failures=0` |
| `test_reliability.cpp` | ✅ 编译 + 运行：`检查 64 项，失败 0 项`（5 个场景全过） |
| `test_event_loop.cpp` | ❌ 编译失败：`dirent.h` 缺失 |
| `test_socket.cpp` | ❌ 编译失败：`arpa/inet.h` / `dirent.h` / `poll.h` / `sys/socket.h` / `unistd.h` 全缺（纯 POSIX 写法） |

**结论**：`rd::net` 的**实现层**和**两套核心测试**已在 Windows 实跑通过；剩下两套测试是纯 POSIX 写法，需要按 REQ-010 建议改（`dirent` → `GetProcessHandleCount`；POSIX socket 头 → `_WIN32` 分支）。产品侧现在就可以开始在 Windows 使用 `rd::net`，不必等全部测试迁移。

### Go2Cpp 回执（2026-11-04）

**1. 选方案 1：`dirent` → `GetProcessHandleCount`，不 skip**

- `tests/rd_net/test_event_loop.cpp`：`CountOpenFds()` 在 `#if defined(_WIN32)` 下改用
  `GetProcessHandleCount(GetCurrentProcess(), &count)`；POSIX 分支保留
  `opendir("/proc/self/fd")`。**两条路径都实现**，用例照跑，`rounds==200`、
  「fd 前后差 ≤1」等检查项不变。
- 口径说明：Windows 返回的是**进程全部句柄数**（含非 socket 句柄），比 Linux 的
  fd 计数宽；本用例只比较前后差值，每轮泄漏一个句柄一样会被抓到，所以不需要 skip。
- 顺带修了同一文件的 `ReadVmRssKb()`（原先读 `/proc/self/status`）：Windows 下改用
  `GetProcessMemoryInfo().WorkingSetSize`。否则该函数返回 -1，`rss_delta ≤ 4096KB`
  会变成恒真断言。

**2. `test_socket.cpp` 一并处理（不 skip）**

- 文件内新增 `tsock` 平台抽象层：POSIX 分支保持原调用；`_WIN32` 分支为
  `poll` → `select`、`close` → `closesocket`、`errno` → `WSAGetLastError()`、
  `EINTR/ECONNRESET/EMSGSIZE` → `WSAEINTR/WSAECONNRESET/WSAEMSGSIZE`、
  `read` → `recv`、`MSG_DONTWAIT` → 零超时 `select` 探测后 `recv`、
  `/proc/self/fd` → `GetProcessHandleCount`、`/proc/self/status` → `GetProcessMemoryInfo`。
- Windows 的 `SOCKET` 是 `UINT_PTR`：测试侧统一用 `std::uintptr_t` 保存句柄，
  不再塞 `int`（同时避免 MSVC `/W4` 的 C4244）；等事件一律用 `native_handle()`。
- **断言条数、场景、阈值一处未改**（406 条 checks 口径不变）；`main` 开头补
  `WSAStartup(MAKEWORD(2,2))`。
- 注意：测试侧用的是 `select`，不是 `WSAPoll`（理由见第 3 条）。

**3. 后端说明修正（重要）**

你们需求里写的期望是 `poll` → `WSAPoll`。**实际实现不是 WSAPoll**：
`src/rd_net/event_loop.cpp` 的 `_WIN32` 分支用
**`WSAEventSelect + WaitForMultipleObjectsEx`**；只有测试侧用 `select`。原因：

- `WSAPoll` 在 Windows 上有文档明示的限制：对非阻塞 `connect` 失败/被拒绝的 socket
  不会给出 `POLLERR/POLLHUP`，UDP 上也不报告 ICMP 端口不可达；而 `rd::net` 的
  `kWouldBlock` / `kReset` 语义依赖这些事件。
- 事件循环还要等**跨线程唤醒**（`Post` / `Stop` 唤醒阻塞中的等待）。
  `WSAPoll` 只能等 socket，等不了 HANDLE；`WaitForMultipleObjectsEx` 可以同时等
  唤醒事件与 socket 事件。

**代价**：受 `MAXIMUM_WAIT_OBJECTS=64` 限制，单个 `EventLoop` 最多 **63 个 socket**
（索引 0 固定给唤醒事件），超出时 `Attach` 返回失败。请 Client/Server 侧按此容量
规划；需要更多连接时开多个 loop（每个 loop 独立线程）。

**4. 构建入口**

- 新增 `tests/rd_net/build_and_run_windows.bat`：`cl /std:c++17 /W4 /WX` 编译并运行
  4 个测试，链接 `ws2_32.lib` + `psapi.lib` + 四个 `go2cpp_*.lib`；用法见文件头注释。
- 新增 `tests/rd_net/CMakeLists.txt`：`rd_net_test_socket` / `rd_net_test_event_loop` /
  `rd_net_test_protocol` / `rd_net_test_reliability` 四个目标 + ctest 用例；
  MSVC 下 `/W4 /WX`，Windows 下自动链接 `ws2_32 psapi`，POSIX 下链 `Threads::Threads`。

**5. 我们做过的验证（非 MSVC，如实说明）**

| 验证 | 结果 |
|---|---|
| POSIX 分支：`g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsyntax-only`（两个文件） | ✅ 零错误零警告 |
| Windows 分支：`g++ -D_WIN32 -D_WIN32_WINNT=0x0601` + 自写最小 winsock2/ws2tcpip/windows/psapi 桩，`-fsyntax-only`（两个文件） | ✅ 零错误 |
| 真机 MSVC 编译 / 运行 / `/W4` 零警告 | ❌ **我们无法验证**（Linux 侧没有 MSVC） |

**需要你们复验**：

1. `build_and_run_windows.bat`（或 CMake 目标）在 MSVC 2022 x64 下 `/W4 /WX` 零警告；
2. `test_event_loop` 的「200 轮 Create/destroy 后句柄数不变」与「RSS delta ≤ 4096KB」
   两条在 Windows 上是否稳定（句柄数口径不同，必要时把阈值放宽到实测值）；
3. `test_socket` 406 条 checks 全过，重点看场景 2（RST）、场景 4（满缓冲）、
   场景 7（TCP 部分提交）；
4. 若 `psapi.lib` 在你们的 `/MT` 配置下链接有问题，告诉我，我改成
   `GetProcAddress("K32GetProcessMemoryInfo")` 动态取，去掉 psapi 导入库依赖。
