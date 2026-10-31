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

## 填写进度

| REQ | 状态 |
|---|---|
| REQ-001..003 | ✅ Go2Cpp 已有（待验收） |
| REQ-004..009 | ❌ 待 Go2Cpp 实现 |
| REQ-010 | ⏸️ 可选（待 JinShan 确认） |
