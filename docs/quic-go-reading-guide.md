# quic-go 源码阅读指南（面向本项目）

> 目的：为 Go2Cpp 增加 QUIC/HTTP3 能力做准备。**不自己实现 QUIC** ——
> 协议本身是数万行工程，正确做法是读成熟实现、必要时集成成熟库。
>
> 本文件记录**为什么选 quic-go**、**按什么顺序读**、**哪些设计值得借鉴**。

## 一、为什么选 quic-go

| 库 | 语言 | 协议核心行数 | 可读性 | 结论 |
|---|---|---:|---|---|
| **quic-go** | Go | ~1.28 万（`internal/` 7 个协议包）+ 顶层 ~0.7 万 | ⭐⭐⭐⭐⭐ | **选它** |
| picoquic | C | ~2 万 | ⭐⭐⭐⭐ | 最贴近 C++ 集成，但代码风格偏老（大量全局函数与宏）|
| quiche | Rust | ~3 万 | ⭐⭐⭐⭐ | 需要 Rust 工具链 |
| ngtcp2 | C | ~4 万 | ⭐⭐⭐ | 生产级，回调式接口，偏复杂 |
| msquic / lsquic | C | ~5 万 | ⭐⭐ | 不适合学习 |

**决定性理由**：
1. **本项目本身就是 Go2Cpp** —— quic-go 的 Go 代码风格、命名、并发模型与本项目定位一致，读起来摩擦最小；
2. **`internal/wire/` 一个帧一个文件**，这是本库最出色的组织方式，非常适合按需精读；
3. **测试极全**（214 个非测试文件对应几乎同等数量的 `_test.go`），每个协议细节都能找到对应断言。

**位置**：`E:\CodexWorkspace\quic-go`（**未加入本项目 git**，体积较大，按需保留）。

规模参考：214 个非测试 `.go` 文件，39744 行（含全部内部包）。

## 二、代码结构总览

### 协议核心（`internal/`，按行数排序）

| 包 | 行数 | 文件数 | 内容 |
|---|---:|---:|---|
| `wire` | 3054 | 33 | **帧编解码 + 包头 + 传输参数**（最该先读）|
| `ackhandler` | 2730 | 14 | ACK 生成、丢包检测、PTO 定时器 |
| `handshake` | 2164 | 16 | TLS 1.3 集成、密钥派生 |
| `congestion` | 913 | 7 | 拥塞控制（NewReno / Cubic / BBR）|
| `protocol` | 820 | 9 | 包类型、错误码、常量（词汇表）|
| `utils` | 801 | 8 | 字节缓冲、计时工具 |
| `qerr` | 221 | 2 | QUIC 传输错误码 |

### 顶层

| 文件 | 行数 | 内容 |
|---|---:|---|
| `connection.go` | 3190 | **连接状态机**（最核心，也最长）|
| `server.go` | 1134 | 服务端握手编排 |
| `packet_packer.go` | 1043 | 组装包（帧 → 字节）|
| `receive_stream.go` | 578 | 流接收与重排 |
| `packet_unpacker.go` | 222 | 拆包（字节 → 包）|
| `stream.go` | 263 | 流 API |
| `client.go` | 109 | 客户端入口 |
| `config.go` | 132 | 配置项 |

## 三、推荐阅读顺序（从易到难，7 步）

### 第 1 步：建立词汇表 —— `internal/protocol/`（820 行）

先读常量与枚举，知道 QUIC 里有哪些包类型、哪些错误码。

对应 RFC 9000 §17（包类型）、§20（错误码）。

**收获**：后面看代码时不会卡在"这个数字是什么"。

### 第 2 步：帧编解码 —— `internal/wire/`（3054 行，33 文件）★ 精华

**这是本库最值得读的部分**。每个帧一个文件：

```
ping_frame.go          ack_frame.go           crypto_frame.go
stream_frame.go        reset_stream_frame.go  stop_sending_frame.go
max_data_frame.go      max_stream_data_frame.go
new_connection_id_frame.go  retire_connection_id_frame.go
path_challenge_frame.go     path_response_frame.go
handshake_done_frame.go     new_token_frame.go
... 以及 frame_parser.go（按类型分发）
```

建议先读 `ping_frame.go`（最简单的帧，能看清 `Parse`/`Append` 的成对结构），
再读 `stream_frame.go`（最常用、字段最多），最后读 `frame_parser.go`（分发逻辑）。

对应 RFC 9000 §19（帧格式）。

**收获**：QUIC 的每个帧都是「解析 + 序列化」成对出现，这个模式可以直接搬到 C++。

### 第 3 步：包结构 —— `packet_unpacker.go`（222）+ `packet_packer.go`（1043）

理解 long header（握手期）与 short header（应用期）的区别、包号编码、
以及一个 UDP datagram 里如何装多个包（coalescing）。

对应 RFC 9000 §17。

**收获**：这是 QUIC 与 TCP 最大的结构差异 —— 包是独立可解析的。

### 第 4 步：ACK 与丢包恢复 —— `internal/ackhandler/`（2730 行）

理解 ACK range 编码、PTO（Probe Timeout）与"丢包检测"（不是超时重传）。

对应 RFC 9002（丢包恢复与拥塞控制）。

**收获**：QUIC 没有"重传"，只有"重新发送"；PTO 触发的是探测包而非重传。

### 第 5 步：拥塞控制 —— `internal/congestion/`（913 行）

NewReno / Cubic / BBR 三个实现的接口一致，可以横向对比。

**收获**：拥塞控制是可插拔的，接口很窄（`OnPacketSent` / `OnPacketAcked` / `OnPacketLost`）。

### 第 6 步：TLS 集成 —— `internal/handshake/`（2164 行）

QUIC 把 TLS 1.3 当作"提供密钥的握手"，不承载数据 —— 这点与 TLS over TCP 完全不同。

对应 RFC 9001。

**收获**：理解 `CRYPTO` 帧如何承载 TLS 消息、以及 0-RTT 的密钥时序。

### 第 7 步：连接状态机 —— `connection.go`（3190 行）

最后读。它是前六步的综合体，包含连接状态、流管理、流控、路径验证。

**不建议从头读到尾**，而是带着具体问题查（比如"收到 STREAM 帧时怎么处理"）。

## 四、对本项目（C++）的借鉴点

| quic-go 的做法 | 可以搬到本项目的什么 |
|---|---|
`internal/wire/` 一帧一文件 + `Parse`/`Append` 成对 | 现有 `framing.hpp` 可以按同样方式组织，每个帧独立可测 |
`frame_parser.go` 用查表分发 | 避免长 `switch`，便于加帧类型 |
`internal/protocol/` 集中常量 | 现有 `http.hpp` 的常量比较分散 |
拥塞控制的窄接口（3 个回调）| 本项目若要加，也应是可插拔接口而非内联逻辑 |
ACK range 的区间表示 | 可直接借鉴数据结构（有序区间列表）|

**不建议照搬的**：
- quic-go 的 `connection.go` 单文件 3190 行 —— 在 C++ 里应拆成
  状态机 / 流管理 / 流控 / 路径验证几个独立类；
- Go 的 goroutine + channel 编排 —— 本项目用 Fiber，但**不要让 QUIC 的连接状态机
  依赖 Fiber 的调度语义**，否则没法在普通线程里复用。

## 五、怎么加深理解

```bash
cd E:/CodexWorkspace/quic-go

# 看某个帧的测试（每个帧都有对应 _test.go，是最好的规格说明）
cat internal/wire/stream_frame_test.go

# 跑 wire 包的测试
go test ./internal/wire/

# 跑端到端示例（需要 Go 环境）
go run example/client/main.go -url https://quic.rocks:4433/
```

## 六、后续若要真正集成

**quic-go 是 Go 库，C++ 项目无法直接链接。** 三条路：

| 方案 | 说明 | 代价 |
|---|---|---|
**用 C/C++ 库**（picoquic / ngtcp2 / msquic）| 真正可集成 | 需要重新读它们的代码 |
**Go 子进程 / CGO 桥** | 用 Go 写一个 QUIC 终结器，C++ 通过本地 socket 与之通信 | 部署复杂，多一层序列化 |
**只借鉴设计，暂不集成** | 先把 HTTP/2 做好（本项目连 HTTP/2 都还没有）| 无立即收益 |

**我的建议**：**先读 quic-go 学设计，暂不集成**。理由是：本项目目前连
HTTP/2 都没有（`grep http2` 零命中），QUIC/HTTP3 的前置条件不具备。
把 HTTP/2 的多路复用做扎实，再谈 HTTP/3 更合理。
