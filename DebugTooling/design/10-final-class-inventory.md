# design · 10 final 类清单与调试策略（第 4 轮开发）

> 生成方式：正则定位 `class X final`，用花括号计数切出**类体**，在类体内检索 `virtual`。
> `virtual=Y` 指**该类自身**（含内嵌类型）出现虚函数，不是文件级统计。

## 统计

| 项 | 数量 |
|---|---:|
| `final` 类总数 | **81** |
| 自身含 `virtual` | **2** |
| 核心相关（调度器/锁/通道/Future/Context/定时器） | **16** |

## 使用者确认的实现策略（代理方案）

1. `final` 类**不继承、不复制实现**；
2. 需要观测或改行为的，建 `Debug*` **代理**（组合持有原类引用或共享指针）；
3. 原类内部**新增私有 `Debug*` 方法**，`Debug*` 代理通过 `friend` 访问；
4. 每个类的侵入只有几行，且**不复制实现、无上游漂移**。

## 核心类清单（优先处理）

| 文件 | 行 | 类名 | 继承 | 自身含 virtual | 类体行数 |
|---|---:|---|---|---|---:|
| `concurrency/channel.hpp` | 71 | `SelectValue` | 否 | **是** | 175 |
| `concurrency/channel.hpp` | 287 | `FunctionSelectCaster` | 是 | 否 | 42 |
| `concurrency/channel.hpp` | 440 | `SelectWaitState` | 否 | 否 | 115 |
| `concurrency/channel.hpp` | 605 | `RingBuffer` | 否 | 否 | 87 |
| `concurrency/channel.hpp` | 700 | `Channel` | 是 | 否 | 987 |
| `concurrency/channel.hpp` | 1713 | `SendOnlyChannel` | 否 | 否 | 47 |
| `concurrency/channel.hpp` | 1768 | `RecvOnlyChannel` | 否 | 否 | 43 |
| `concurrency/core/timer.hpp` | 93 | `TimerDecision` | 否 | 否 | 38 |
| `concurrency/core/timer.hpp` | 163 | `Timer` | 否 | 否 | 37 |
| `concurrency/core/timer.hpp` | 208 | `TimerService` | 否 | 否 | 135 |
| `concurrency/future.hpp` | 61 | `FutureCallbackGuard` | 否 | 否 | 32 |
| `concurrency/future.hpp` | 693 | `Promise` | 否 | 否 | 82 |
| `concurrency/future.hpp` | 790 | `Future` | 否 | 否 | 150 |
| `sync.hpp` | 78 | `Mutex` | 否 | 否 | 41 |
| `sync.hpp` | 129 | `ConditionVariable` | 否 | 否 | 40 |
| `sync.hpp` | 177 | `WaitGroup` | 否 | 否 | 41 |

## 全部 final 类（按文件分组）

| 文件 | 行 | 类名 | 继承 | 自身含 virtual | 类体行数 | 相关性 |
|---|---:|---|---|---|---:|---|
| `concurrency/channel.hpp` | 71 | `SelectValue` | 否 | **是** | 175 | **★通道** |
| `concurrency/channel.hpp` | 287 | `FunctionSelectCaster` | 是 | 否 | 42 | **★通道** |
| `concurrency/channel.hpp` | 440 | `SelectWaitState` | 否 | 否 | 115 | **★通道** |
| `concurrency/channel.hpp` | 605 | `RingBuffer` | 否 | 否 | 87 | **★通道** |
| `concurrency/channel.hpp` | 700 | `Channel` | 是 | 否 | 987 | **★通道** |
| `concurrency/channel.hpp` | 1713 | `SendOnlyChannel` | 否 | 否 | 47 | **★通道** |
| `concurrency/channel.hpp` | 1768 | `RecvOnlyChannel` | 否 | 否 | 43 | **★通道** |
| `concurrency/core/hybrid_mutex.hpp` | 33 | `HybridGate` | 否 | 否 | 75 | ○辅助 |
| `concurrency/core/parking_condition.hpp` | 22 | `ParkingCondition` | 否 | 否 | 75 | ○辅助 |
| `concurrency/core/timer.hpp` | 93 | `TimerDecision` | 否 | 否 | 38 | **★定时器** |
| `concurrency/core/timer.hpp` | 163 | `Timer` | 否 | 否 | 37 | **★定时器** |
| `concurrency/core/timer.hpp` | 208 | `TimerService` | 否 | 否 | 135 | **★定时器** |
| `concurrency/fiber_local.hpp` | 50 | `FiberLocalCache` | 否 | 否 | 87 | ○辅助 |
| `concurrency/future.hpp` | 61 | `FutureCallbackGuard` | 否 | 否 | 32 | **★Future** |
| `concurrency/future.hpp` | 693 | `Promise` | 否 | 否 | 82 | **★Future** |
| `concurrency/future.hpp` | 790 | `Future` | 否 | 否 | 150 | **★Future** |
| `concurrency/thread_policy.hpp` | 75 | `ScopedThreadParticipation` | 否 | 否 | 29 | ○辅助 |
| `concurrency/thread_policy.hpp` | 123 | `ScopedThreadHookMode` | 否 | 否 | 17 | ○辅助 |
| `config/config.hpp` | 32 | `IniFile` | 否 | 否 | 21 | △一般 |
| `config/config.hpp` | 179 | `ConfigVar` | 是 | 否 | 215 | △一般 |
| `config/config.hpp` | 400 | `Config` | 否 | 否 | 89 | △一般 |
| `debug/debug_fiber.hpp` | 119 | `DebugRegistry` | 否 | 否 | 63 | △一般 |
| `logging/log.hpp` | 79 | `MinimumLevelFilter` | 是 | 否 | 14 | △一般 |
| `logging/log.hpp` | 204 | `PatternFormatter` | 是 | 否 | 21 | △一般 |
| `logging/log.hpp` | 267 | `RotatingFileSink` | 是 | 否 | 18 | △一般 |
| `logging/log.hpp` | 287 | `StdoutSink` | 是 | 否 | 8 | △一般 |
| `logging/log.hpp` | 297 | `StderrSink` | 是 | 否 | 8 | △一般 |
| `logging/log.hpp` | 307 | `MemorySink` | 是 | 否 | 13 | △一般 |
| `logging/log.hpp` | 323 | `CallbackSink` | 是 | 否 | 12 | △一般 |
| `logging/log.hpp` | 379 | `Logger` | 是 | 否 | 50 | △一般 |
| `logging/log.hpp` | 431 | `LoggerManager` | 否 | 否 | 32 | △一般 |
| `logging/log.hpp` | 469 | `LogLine` | 否 | 否 | 16 | △一般 |
| `network/address.hpp` | 67 | `IPv4Address` | 是 | 否 | 20 | △一般 |
| `network/address.hpp` | 89 | `IPv6Address` | 是 | 否 | 20 | △一般 |
| `network/address.hpp` | 111 | `UnixAddress` | 是 | 否 | 16 | △一般 |
| `network/address.hpp` | 129 | `UnknownAddress` | 是 | 否 | 13 | △一般 |
| `network/byte_array.hpp` | 22 | `ByteArray` | 否 | 否 | 69 | △一般 |
| `network/fd_manager.hpp` | 26 | `FdContext` | 否 | 否 | 21 | △一般 |
| `network/fd_manager.hpp` | 49 | `FdManager` | 否 | 否 | 20 | △一般 |
| `network/framing.hpp` | 22 | `Framer` | 否 | 否 | 104 | △一般 |
| `network/hook.hpp` | 58 | `ScopedEnable` | 否 | 否 | 15 | △一般 |
| `network/hook.hpp` | 78 | `ScopedIOManagerBinding` | 否 | 否 | 15 | △一般 |
| `network/http.hpp` | 38 | `Headers` | 否 | 否 | 16 | △一般 |
| `network/http.hpp` | 56 | `Request` | 否 | 否 | 17 | △一般 |
| `network/http.hpp` | 75 | `Response` | 否 | 否 | 13 | △一般 |
| `network/http.hpp` | 106 | `Parser` | 否 | 否 | 31 | △一般 |
| `network/http.hpp` | 162 | `Client` | 否 | 否 | 14 | △一般 |
| `network/http.hpp` | 189 | `ResponseWriter` | 否 | 否 | 15 | △一般 |
| `network/http.hpp` | 215 | `Server` | 否 | 否 | 21 | △一般 |
| `network/io.hpp` | 30 | `DescriptorToken` | 否 | 否 | 19 | △一般 |
| `network/io.hpp` | 62 | `DescriptorGuard` | 否 | 否 | 15 | △一般 |
| `network/io.hpp` | 171 | `IOManager` | 否 | 否 | 132 | △一般 |
| `network/socket.hpp` | 48 | `Socket` | 是 | 否 | 127 | △一般 |
| `network/socket.hpp` | 177 | `SocketStream` | 否 | 否 | 20 | △一般 |
| `network/sse.hpp` | 94 | `Parser` | 否 | 否 | 37 | △一般 |
| `network/sse.hpp` | 143 | `Stream` | 否 | 否 | 47 | △一般 |
| `network/websocket.hpp` | 81 | `WebSocket` | 是 | 否 | 85 | △一般 |
| `sync.hpp` | 78 | `Mutex` | 否 | 否 | 41 | **★锁** |
| `sync.hpp` | 129 | `ConditionVariable` | 否 | 否 | 40 | **★锁** |
| `sync.hpp` | 177 | `WaitGroup` | 否 | 否 | 41 | **★锁** |
| `tools/bytes.hpp` | 248 | `Buffer` | 是 | 否 | 204 | △一般 |
| `tools/bytes.hpp` | 460 | `Reader` | 是 | 否 | 71 | △一般 |
| `tools/control_flow.hpp` | 48 | `defer` | 否 | **是** | 89 | △一般 |
| `tools/control_flow.hpp` | 147 | `panic` | 否 | 否 | 45 | △一般 |
| `tools/control_flow.hpp` | 202 | `recover` | 否 | 否 | 31 | △一般 |
| `tools/error.hpp` | 71 | `StringError` | 是 | 否 | 10 | △一般 |
| `tools/error.hpp` | 83 | `WrappedError` | 是 | 否 | 16 | △一般 |
| `tools/error.hpp` | 101 | `JoinError` | 是 | 否 | 14 | △一般 |
| `tools/event.hpp` | 32 | `EventBatch` | 否 | 否 | 388 | △一般 |
| `tools/go.hpp` | 156 | `fiber` | 否 | 否 | 63 | △一般 |
| `tools/streams.hpp` | 377 | `LimitedReader` | 是 | 否 | 20 | △一般 |
| `tools/streams.hpp` | 406 | `MultiReader` | 是 | 否 | 23 | △一般 |
| `tools/streams.hpp` | 433 | `MultiWriter` | 是 | 否 | 17 | △一般 |
| `tools/streams.hpp` | 454 | `TeeReader` | 是 | 否 | 20 | △一般 |
| `tools/streams.hpp` | 478 | `SectionReader` | 是 | 否 | 43 | △一般 |
| `tools/streams.hpp` | 525 | `DiscardWriter` | 是 | 否 | 7 | △一般 |
| `tools/streams.hpp` | 555 | `StringReader` | 是 | 否 | 56 | △一般 |
| `tools/streams.hpp` | 615 | `BufferWriter` | 是 | 否 | 42 | △一般 |
| `tools/strings.hpp` | 600 | `StringBuilder` | 否 | 否 | 53 | △一般 |
| `tools/time.hpp` | 48 | `Duration` | 否 | 否 | 177 | △一般 |
| `tools/time.hpp` | 250 | `Time` | 否 | 否 | 120 | △一般 |

## 自身含虚函数的类（这些类去掉 `final` 会让产物略大）

- `SelectValue`（`concurrency/channel.hpp`:71），类体 175 行
- `defer`（`tools/control_flow.hpp`:48），类体 89 行
