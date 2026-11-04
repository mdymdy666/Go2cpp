# tunnel raw 通道兼容性（能力协商已存在）

> 本文回答遗留问题：「老客户端不认 `OPEN_RAW`，大 body 会断控制连接」——**实际上协议里已经有能力协商**，
> 老客户端会自动回退 mux，不会断连。

## 一、协商机制（已实现）

`HELLO.flags` 的 **bit1**（`kHelloFlagRawStream = 1U << 1U`，`protocol.hpp:121`）：

| 端 | 行为 |
|---|---|
| **客户端**（`client.cpp:322`） | 启动时 `hello.flags |= kHelloFlagRawStream`——声明「我支持 raw」 |
| **服务端**（`server.cpp:988`） | `if ((hello.flags & kHelloFlagRawStream) != 0)` 才允许该连接走 raw 数据通道 |

## 二、对老客户端的影响

| 客户端版本 | HELLO.flags | 服务端行为 |
|---|---|---|
| **新客户端**（含 raw） | bit1 = 1 | 大 body（`Content-Length > 1 MiB`）走 raw，小请求走 mux |
| **老客户端**（无 raw） | bit1 = 0 | **一律走 mux**——不会收到 `OPEN_RAW`，不会断连 |

## 三、结论

- **不存在「老客户端断连」问题**——能力协商在协议层，老客户端自动回退 mux；
- `GO2CPP_TUNNEL_RAW_THRESHOLD` 只控制「**已声明支持 raw 的客户端**」里哪些请求走 raw（默认 1 MiB）；
- 若某中间版本客户端误声明了 bit1 但不支持 `OPEN_RAW`，可服务端设 `GO2CPP_TUNNEL_RAW_THRESHOLD=-1` 强制全 mux。

## 四、验证

- `test_tunnel` 的 `TestRawAckNoPreread`（raw 握手 + ACK 不预读）；
- `TestRawStreamLimit`（raw 并发上限，显式设 `THRESHOLD=0`）；
- 21 个 tunnel 测试用例（含 raw 在 `THRESHOLD=0/-1` 两种极端）全过。

## 五、遗留的实测数据

| 场景 | 吞吐 | 开销 |
|---|---|---|
| raw 8 MiB | **2385 MiB/s** | **1.27×** |
| mux 8 MiB | 770 MiB/s | 3.59× |
| 小请求 1 KiB（mux） | — | p50 1.282 ms |
