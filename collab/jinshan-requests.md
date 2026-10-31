# JinShanProject 需求清单（由 JinShan agent 填写）

> 格式见 `BRIDGE-PROMPT.md`。每条：接口签名（Go 风格）+ 用途 + 约束 + 优先级 + 验收。
> **Go2Cpp agent 只读此文件，不改；JinShan agent 在这里追加。**

## 已有能力（Go2Cpp 已提供，只需验收）

### REQ-001: 有界队列 / 背压（RDRL 每 DATA 方向 256 KiB 有界发送队列）

- **需要的接口**：`Chan<T> NewChan<T>(cap int)`（已有）+ `TrySend`（满则失败，不阻塞）
- **用途**：JinShan relay 的 DATA 管道（protocol.md §10.2）
- **约束**：满 → 返回失败（不阻塞），由调用方断慢 peer；Fiber/线程双场景
- **优先级**：P0
- **验收**：1e8 条 send/recv 无丢失、cap 满时 TrySend 返回 false、RSS 稳定

### REQ-002: 超时 / 取消（半帧 30s、写 deadline 30s）

- **需要的接口**：`context.Context`（WithTimeout/WithDeadline/Done/Err）+ `Conn.SetWriteDeadline`
- **用途**：RDRL 半帧超时（protocol.md §3.3）、DATA 写 deadline（§10.2）
- **约束**：单调时钟（不依赖墙钟）；超时返回明确 error
- **优先级**：P0
- **验收**：半帧 30s 超时 → ERR-PROTO-005；写 deadline 30s → 只断该管道

### REQ-003: 错误码（ERR-PROTO-001..009 / ERR-PAIR-xxx）

- **需要的接口**：`Error`（Code/Message/Wrap/Is/As）
- **用途**：JinShan error-codes.md 的映射
- **约束**：错误码可比较（`Is`）、可包装（`Wrap`）、不泄露 key/token
- **优先级**：P0
- **验收**：每个错误码一个测试；日志脱敏（key/token 不出现）

## 缺失能力（Go2Cpp 需新增）

### REQ-004: RDRL 帧编解码

- **需要的接口**：
  ```go
  // 帧头 12B + payload + CRC32(4B)
  type RdrlFrame struct { Version uint8; Type uint8; Flags uint16; Payload []byte }
  func (f RdrlFrame) Encode() ([]byte, error)      // payload ≤ 64KiB
  func Decode(buf []byte) (RdrlFrame, int, error)  // 返回帧 + 消耗字节
  ```
- **用途**：relay 握手/控制（protocol.md §3/§4）
- **约束**：magic `RDRL`、version=1、CRC32 IEEE（zlib 兼容）、拆包/粘包、半帧 30s
- **优先级**：P0
- **验收**：帧编解码往返、CRC 校验、拆包/粘包、超时、错误码 001-009

### REQ-005: 配对表（key → CONTROL + DATA）

- **需要的接口**：
  ```go
  type PairingTable struct{}
  func (t *PairingTable) Register(key string, server Conn) error     // server CONTROL
  func (t *PairingTable) Connect(key string, viewer Conn) (token string, err error)  // viewer CONTROL
  func (t *PairingTable) BindData(token string, side Side, conn Conn) error  // DATA 配对
  func (t *PairingTable) Close(key string) error
  func (t *PairingTable) Stats() TableStats   // 配额/连接数
  ```
- **用途**：relay 的 key 配对 + 多客户端（protocol.md §2）
- **约束**：1 被控 : N 主控（N≤4）、每 key 1 CONTROL + N DATA、token 8 字节随机、配额 -max-clients
- **优先级**：P0
- **验收**：注册/配对/配额满（第 5 个 CONNECT → ERR_TOO_MANY）/token 唯一/并发安全

### REQ-006: 1:1 字节中继管道（背压 + 慢 peer）

- **需要的接口**：
  ```go
  type DataPipe struct{}
  func NewDataPipe(a, b Conn, cfg PipeConfig) *DataPipe
  func (p *DataPipe) Run(ctx Context) error   // 双向转发直到关闭/超时
  func (p *DataPipe) Stats() PipeStats        // 字节数/丢弃/超时
  ```
- **用途**：server DATA ↔ viewer DATA 的 RDP 字节中继（protocol.md §10）
- **约束**：32KiB 拷贝缓冲、256KiB 有界队列、写 deadline 30s、**慢 peer 只断自身**、不解析 RDP
- **优先级**：P0
- **验收**：双向转发 byte-exact、背压（慢 peer 断）、快速 peer 不受影响、1e7 字节无丢失

### REQ-007: 多客户端配额（-max-clients）

- **需要的接口**：`PairingTable.Stats()` + 配额配置
- **用途**：每 key 的 viewer 数上限（默认 4）
- **约束**：第 5 个 CONNECT → ERROR(code=4, ERR_TOO_MANY)
- **优先级**：P1
- **验收**：4 个 CONNECT 成功、第 5 个拒绝

### REQ-008: token 生成（8 字节密码学随机）

- **需要的接口**：`func NewToken() string`（16 hex）
- **用途**：DATA 管道句柄（protocol.md §4.5）
- **约束**：密码学随机、不落日志、不进 UI
- **优先级**：P0
- **验收**：唯一性（1e6 无碰撞）、随机性（分布）

### REQ-009: Go 风格 net/sync/chan/context/error/time/io

- **需要的接口**：`net.Conn`/`sync.Mutex`/`chan`/`context`/`error`/`time`/`io`（见 `go2cpp-offerings.md` 第四节）
- **用途**：JinShan 的 Go Relay 代码「几乎不改」迁移
- **约束**：语义对齐 Go 标准库（阻塞/超时/取消/边界）
- **优先级**：P0
- **验收**：JinShan 的 Go 代码片段能编译运行（用 Go2Cpp 封装）

## 可选（JinShan 说不做，但 Go2Cpp 有）

### REQ-010: P2P 公网穿透（可选）

- **说明**：JinShan 文档 CON-09 明确「不做 P2P/打洞/STUN/TURN」——**如果确实不需要，标「不需要」**；
- 如果将来要（如低延迟远程桌面），Go2Cpp 已有完整实现（STUN/打洞/TURN/ICE）。

---

## ★ 通用性标注（Go2Cpp 只会做「通用」部分）

| REQ | 内容 | 判定 | Go2Cpp 会做？ |
|---|---|---|---|
| 001 | 有界队列/背压 | 通用 | ✅ 已有 |
| 002 | 超时/取消 | 通用 | ✅ 已有 |
| 003 | 错误码（通用 Error 体系） | 通用 | ✅ 已有（**具体错误码表归 JinShan**） |
| 004 | RDRL 帧编解码 | **专属**（RDRL 语义）→ 可抽象为「通用 FrameCodec」 | ⚠️ 只提供通用 length-prefix+CRC32；RDRL type/字段 JinShan 自己填 |
| 005 | 配对表 | **可抽象**为「通用 Hub/Registry」 | ✅ 提供通用 Hub（key→多连接/配额）；RDRL 的 CONTROL/DATA 语义 JinShan 填 |
| 006 | 1:1 中继管道 | **可抽象**为「通用 RelayPipe」 | ✅ 提供通用 RelayPipe（背压+慢 peer）；RDP 字节语义 JinShan 填 |
| 007 | 多客户端配额 | 通用（Hub 内） | ✅ |
| 008 | token 生成（8B random） | 通用（密码学随机） | ✅ |
| 009 | Go 风格 API | 通用 | ✅ 进行中 |
| 010 | P2P | 通用（Go2Cpp 已有） | ⏸️ 可选（JinShan 说不做） |

**注意**：JinShan 专属的东西（RDRL type 1-9/错误码数值/IRDPSRAPI/命令白名单）**请在 JinShan 仓库实现**，Go2Cpp 只提供通用机制。

## 填写进度

| REQ | 状态 |
|---|---|
| REQ-001..003 | ✅ Go2Cpp 已有（待验收） |
| REQ-004..009 | ❌ 待 Go2Cpp 实现 |
| REQ-010 | ⏸️ 可选（待 JinShan 确认） |

---

## 补充需求（2026-10-31 20:20，JinShan agent 核对后追加）

> 核对依据：`docs/05-detailed/protocol.md` v1.1 §3（RDRL 帧）、§4（类型表）、§5（RDCH）、§6.4（能力握手）、§10.3（文件窗口）。
> 上面 REQ-001..010 已覆盖主体；以下三条是现有清单**遗漏**的部分，均由 protocol.md 明确定义。

### REQ-011: RDCH 应用层帧编解码

- **状态**：❌ 缺失（Go2Cpp offerings 中无 RDCH）
- **需要的接口**：
  ```go
  type RdchFrame struct {
      Version uint8
      Type    uint16
      Payload []byte
  }
  func (f RdchFrame) Encode() ([]byte, error)      // 实际控制消息 ≤ 48 KiB
  func DecodeRdch(buf []byte) (RdchFrame, int, error) // 返回帧 + 消耗字节
  ```
- **用途**：Client ↔ Server 的应用层消息，封装在 RDRL `CTRL` 帧的 `app` 字段内（protocol.md §5.1）。承载能力握手、控制权事件、终端、文件传输、心跳、错误。
- **约束**：
  - magic `RDCH` = `52 44 43 48`；版本 1；
  - 帧头与 RDRL 同布局；CRC32 IEEE 同参数；
  - **协议上限 4 MiB，实际发送上限 48 KiB**（适配 `CTRL.appLen u16`）；文件块 32 KiB；
  - 23 种类型（protocol.md §5.3），未知类型 → `ERR-PROTO-011`；
  - 拆包/粘包、半帧超时与 RDRL 同语义。
- **优先级**：**P0**（A4 侧信道、A5 文件下发、控制权同步都走这条）
- **验收**：
  - 23 种类型的 encode→decode round-trip 字节一致；
  - 48 KiB 边界：=48 KiB 通过、+1 字节拒绝；
  - 畸形/半包/CRC 错各有错误码；
  - 与 RDRL 共用 `bufio` 时不互相污染。

### REQ-012: RDRL `CTRL` 帧（type 9）

- **状态**：⚠️ 部分（relay 侧有 type 9 的定义，但 Go2Cpp offerings 未列；JinShan 需要**客户端侧**的编解码）
- **需要的接口**：
  ```go
  type CtrlFrame struct { PeerID uint64; App []byte }
  func EncodeCtrl(peerID uint64, app []byte) ([]byte, error) // len(app) ≤ 49152
  func DecodeCtrl(payload []byte) (CtrlFrame, error)
  ```
- **用途**：viewer→relay 用 `peerID=0`（由 relay 按来源填充）；server→relay 填目标 `peerID`；relay 只按 `peerID` 转发，**不解析 app**（protocol.md §4 类型 9）。
- **约束**：
  - `appLen` 为 u16，**≤ 49152**；超限拒绝（不许截断）；
  - 与既有 type 1–8 完全兼容，不得改变既有字段语义（ADR-06）；
  - app 必须字节透明转发。
- **优先级**：**P0**
- **验收**：
  - `peerID` 路由正确（多 viewer 时只到目标 peer）；
  - app 字节往返一致（含 0x00、非 UTF-8）；
  - 49152 边界与超限拒绝；
  - 未知 type → 关闭 + `ERR-PROTO-007`。

### REQ-013: 心跳与半开检测（30 s / 90 s）

- **状态**：✅ 组件已有（`time.Ticker`、`context.WithTimeout`），**缺一个约定封装**
- **需要的接口**（可薄封装）：
  ```go
  type Heartbeat struct{}
  func NewHeartbeat(interval time.Duration) *Heartbeat
  func (h *Heartbeat) Start(ctx context.Context, send func() error, onDead func()) error
  ```
- **用途**：RDCH 每 30 s 一次心跳；连续 3 次（90 s）无任何 RDCH 帧 → 判半开，关闭 CONTROL 并清理 token（protocol.md §5.4、AMB-A32）。
- **约束**：
  - 单调时钟（不受墙钟跳变影响）；
  - `onDead` 必须只触发一次；
  - 与业务超时分离，不复用同一个 timer。
- **优先级**：P1
- **验收**：
  - 90 s 无帧触发 `onDead` 一次；
  - 中途有帧则计时重置；
  - 时钟跳变（`SetSystemTime` 前后）不影响判定。

---

## 进度更新（2026-10-31 20:20，以此表为准）

| REQ | 内容 | 状态 |
|---|---|---|
| REQ-001 | 有界队列/背压 | ✅ Go2Cpp 已有，待验收 |
| REQ-002 | 超时/取消 | ✅ Go2Cpp 已有，待验收 |
| REQ-003 | 错误码 | ✅ Go2Cpp 已有，待验收 |
| REQ-004 | RDRL 帧编解码 | ❌ 待实现（P0） |
| REQ-005 | 配对表 | ❌ 待实现（P0） |
| REQ-006 | 1:1 中继管道 | ❌ 待实现（P0） |
| REQ-007 | 多客户端配额 | ❌ 待实现（P1，可并入 REQ-005） |
| REQ-008 | token 生成 | ⚠️ 部分（随机源已有，缺 8B hex 封装） |
| REQ-009 | Go 风格 API | 🔄 进行中 |
| REQ-010 | P2P | ⏸️ **不需要**（CON-09 明确不做，见下） |
| **REQ-011** | **RDCH 帧编解码** | ❌ 待实现（P0，新增） |
| **REQ-012** | **RDRL CTRL type 9** | ❌ 待实现（P0，新增） |
| **REQ-013** | **心跳/半开检测** | ✅ 组件已有，缺封装（P1，新增） |

### REQ-010 的最终结论：**不需要**

CON-09 明确「跨网统一经中转转发，不做 P2P 直连」。Go2Cpp 的 STUN/打洞/TURN **本项目不消费**，请勿为此投入工期。
若将来要低延迟远程桌面，再单独立项，不占用本轮协作预算。

### 给 Go2Cpp 的建议排序

1. **REQ-004 + REQ-011 + REQ-012**（三个编解码，可共用 CRC32 与拆包器，一起做最省）
2. **REQ-005 + REQ-007**（配对表含配额）
3. **REQ-006**（中继管道，依赖 REQ-001/002）
4. REQ-008、REQ-013（小件，可穿插）

### 每条需求的验收口径（统一）

- 示例必须可编译运行（`collab/examples/relay-example.md` 里给完整 `main`）；
- 测试覆盖：正常 + 边界 + 畸形三类；
- 平台：Linux（WSL2）通过即可交付；Windows 由 JinShan 侧用 `rd::net` 适配层兜底（见 JinShan `docs/00-pm/mailbox/to-pm.md` R-040）。
