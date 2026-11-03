# rd::net v3 交付三件套审查（2026-11-03）

> 对象：JinShan v3（REQ-006…009）交付物。
> 环境：WSL2 / Ubuntu 24.04 / g++ 13.3.0 / `-std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror`。
> 提交：`07eb241`（009）、`b88de85`+`dbd0f68`（006）、`ec088d3`（007）、`6820dd1`（008+SendBatch+命名空间统一）。

## 一、架构审查

### 1.1 分层与依赖（单向，无循环）

```
socket.hpp        (IoStatus/SendResult/DatagramSocket/StreamSocket/SendBatch)
   ↑
protocol.hpp      (Protocol/StreamProtocol/ProtocolContext/ByteView)
   ↑
event_loop.hpp    (EventLoop/定时器/Attach)
reliability.hpp   (SeqNum/RttEstimator/RetransmitQueue/CongestionController，独立)
```

- `event_loop.hpp:43` → include `protocol.hpp`；`protocol.hpp` 只前置声明 `EventLoop`/`ProtocolContext`。
- 底层绑定：`go2cpp::net::Socket`（生命周期/bind/connect/close）+ `bufio::SocketBatchWriter`（批量）+ `streams::WriteResult`（错误）。

### 1.2 所有权与生命周期

| 对象 | 所有权 | 风险 |
|---|---|---|
| `Socket::Ptr` | `DatagramSocket/StreamSocket` 独占持有 | 析构顺序由成员决定，已用 `Close()` 幂等 |
| `fd()` | **瞬时借用**（源码注释明确不跨操作持有） | 跨线程 `Close` 会 ABA；当前 `EventLoop` 单线程用法安全，多线程需上层串行化（已文档化） |
| `ByteView` | 回调期间借用，**不拷贝** | 回调返回后失效；已用 memset 反证验证 |
| `bufio::BatchEntry` | `SendBatch` 调用期间借用，不保存指针 | 调用后覆盖源缓冲会改变已入队内容（设计如此） |

### 1.3 零拷贝路径

`OnDatagram(ByteView)` → `ProtocolContext::SendTo(ByteView)` → 入队（不拷贝）→ loop tick 内 `sendto`。memset 反证：`SendTo` 返回后立即覆盖源缓冲为 `0xCC`，对端收到全 `0xCC`；负向对照（实现里偷偷拷贝）得「全 `0x5A`、failures=2」——用例能抓 bug。

### 1.4 批量路径

`SendBatch(entries, count)` → `SocketBatchWriter::WriteBatch` → UDP `sendmmsg` / TCP `writev`；`count>1024` 截断；部分提交返回 `kWouldBlock` 且 `last_batch_submitted()=K`。

### 1.5 架构层发现的问题（如实）

1. **单条 `SendTo` 开批量反而慢约 14%**（12.97k vs 11.68k ns/op）：单条 API 下 `sendmmsg` 只能提交 1 段，额外 `ProbeWritable`/段表开销纯亏。默认全关（`batch_bytes=0`），真收益走 `SendBatch`。
2. **`fd()` 瞬时值的 ABA 风险**：跨线程关闭后 fd 可能被复用；单线程 EventLoop 安全，多线程需上层串行化（文档化，未强制）。
3. **统一 `Protocol` 含两组重载**（事件循环层 + 协议层带 `ProtocolContext&`）：接口略胖，但换来单一类型，避免 `plugin` 命名空间分裂。
4. **`ack_bits` 范围**：`tests/rd_net` 8 处是测试自身；交付目录 `include/go2cpp/rd_net` + `src/rd_net` 0 命中；G6 按交付目录判。

## 二、汇编级检查（objdump --no-show-raw-insn）

| 符号 | 指令数 | 说明 |
|---|---:|---|
| `SendBatch`（含 -O2 内联的批量路径） | 3107 | 含 `WriteBatch` 与段表填充 |
| `WriteBatch`（bufio 适配层） | 311 | 独立符号 |
| `sendv` 调用点 | 3 | TCP `writev` 路径 |
| `EventLoop::Run` 栈帧 | **152 B**（`sub $0x98,%rsp`） | poll 循环，无大栈缓冲 |
| `DatagramBridge::OnDatagram` | 28 | 桥接转发（协议层） |

- `sendmmsg` 是 libc 动态符号，本地无定义，调用经 PLT（objdump 只见 `call *...@plt`），未计入本地指令数。
- 结论：热路径无每包分配；批量路径的指令数主要来自段表填充（一次填 N 段、一次 syscall），与「N 次单条 send」的 N×指令量相比是数量级差（TCP 实测 21×）。

## 三、正确性 + 性能复验（本轮实测）

### 3.1 四套测试

| 套件 | 结果 |
|---|---|
| socket（含 SendBatch/四态/1e4 echo/100 socket/10 万压测） | `checks=406 failures=0` |
| event_loop（tick 顺序/1000 定时器/Post 1e5/防饿死/Cancel/Stop/200 轮无泄漏） | `checks=44 failures=0` |
| reliability（回绕/RFC 6298/1e5 队列/随机对拍 1e5/1e6 压测） | `检查 64 项，失败 0 项` |
| protocol（memset 反证/echo/ACK 32↔64/1 万次 SendTo/64 包 tick） | `checks=44 failures=0` |

### 3.2 性能（3 轮，`taskset` 未限核，负载 <2）

| 场景 | 批量 | 单条 | 提升 |
|---|---|---|---|
| TCP 64×1KB（`writev`） | 210 ns/段、4646 MiB/s | 4379 ns/段、223 MiB/s | **20.8×** |
| UDP 64×1KB（`sendmmsg`） | 3040 ns/dgram、322 MiB/s | 3657 ns/dgram、267 MiB/s | **1.20×** |
| 单条 SentTo 开 batch（错误用法） | 12969 ns/op | 11680 ns/op | 0.90×（故默认关） |

### 3.3 四态证据

FIN：`revents=0x1`、`errno=0` → `kClosed`；RST（`SO_LINGER{1,0}`）：`revents=0x19`、`errno=104` → `kReset`；UDP 65507→1024 缓冲：`kTruncated`、`actual=65507`；满缓冲：`kWouldBlock`，重试后 `524288` 全达。

## 四、结论

- **架构**：分层单向、所有权清晰、零拷贝经反证、批量走系统调用合并——符合契约 §1.1「只搬字节」边界；`git diff -- include/go2cpp/rd_net src/rd_net` 在协议测试中为 0（协议全在测试侧）。
- **汇编**：热路径无每包分配；`EventLoop::Run` 栈帧 152 B；批量路径指令量由段数决定而非包数。
- **正确性/性能**：四套 406/44/64/44 全绿；TCP 批量 20.8×、UDP 1.20× 为真实收益；单条开批量慢 14% 已如实记录并默认关闭。
- **遗留**：`fd()` 跨线程 ABA（文档化）、`Protocol` 双组重载（设计取舍）、`ack_bits` 范围限定（G6 按交付目录）。
