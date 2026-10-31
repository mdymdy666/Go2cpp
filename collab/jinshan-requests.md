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
