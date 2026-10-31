# Go2Cpp 能提供什么（通用能力清单）

> JinShan 的 agent 读这个文件，了解「不用重复造」的能力。
> 每项给：接口签名（Go 风格）+ 现状 + 示例位置。

## 一、并发（9 原语，全部 ≥Go 或吊打）

| 能力 | Go 风格接口 | 现状 |
|---|---|---|
| **Channel** | `Chan<T> NewChan<T>(cap int)`；`Send/Recv/Close`；`Select(cases...)` | ✅（cap1024 10.73ns，赢 Go 2.2×） |
| **FastChannel**（无锁） | `FastChan<T, Cap>`；`TrySend/TryRecv` | ✅（cap64 0.46× Go） |
| **Mutex** | `sync.Mutex`（Lock/Unlock/TryLock） | ✅（3.75ns，同级 Go） |
| **RWMutex** | `sync.RWMutex`（RLock/RUnlock/Lock/Unlock） | ✅（写 1t 0.55-0.75× Go） |
| **ConditionVariable** | `sync.Cond`（Wait/Signal/Broadcast） | ✅ |
| **WaitGroup** | `sync.WaitGroup`（Add/Done/Wait） | ✅（AddDone 0.93× Go） |
| **Context** | `context.Context`（Background/WithCancel/WithTimeout/WithDeadline/Done/Err/Value） | ✅（IsDone 0.087× Go） |
| **Future** | `Future<T>`（Get/Wait/Then） | ✅ |
| **Fiber/Scheduler** | `go f()` 风格（Scheduler::go）；`Fiber.Suspend/Resume` | ✅（切换 23.5ns） |

**示例**：`collab/examples/concurrency-example.md`

## 二、网络

| 能力 | Go 风格接口 | 现状 |
|---|---|---|
| **TCP**（Dial/Listen/Accept） | `net.Conn`/`net.Listener` | ✅ |
| **UDP** | `net.UDPConn` | ✅ |
| **HTTP/SSE** | `HttpServer`（路由/中间件/SSE） | ✅（7.7k req/s） |
| **WebSocket** | `WsServer` | ✅（5.4k msg/s） |
| **MQTT/CoAP/SOCKS5** | 各 Server | ✅ |
| **批量 syscall** | `sendmmsg`/`recvmmsg`（内部） | ✅（10-13×） |
| **TCP_NODELAY** | 默认开（accept 后设） | ✅（延迟 292×） |

**示例**：`collab/examples/network-example.md`

## 三、缓存（**Go 标准库没有**）

| 能力 | 接口 | 现状 |
|---|---|---|
| **LRU** | `LruCache<K,V>(cap)`；`Get/Put/Erase/Stats` | ✅（2.57×） |
| **LRU-K**（防扫描） | `LruKCache<K,V>(cap, k)` | ✅（1.97×） |
| **2Q**（防扫描） | `TwoQueueCache<K,V>(cap)` | ✅（2.80×） |
| **W-TinyLFU**（Caffeine 同款） | `TinyLfuCache<K,V>(cap)` | ✅（1.76×） |
| **GDSF**（带权重） | `GdsfCache<K,V>(cap)` | ✅（1.04×） |
| **ARC**（自适应） | `ArcCache<K,V>(cap)` | ✅（2.58×） |
| **CLOCK**（低开销） | `ClockCache<K,V>(cap)` | ✅（2.12×） |
| **ThreadSafeCache** | `ThreadSafeCache<K,V>(inner)` | ✅ |
| **ShardedCache**（多核） | `ShardedCache<K,V>(inner, shards)` | ✅ |

**选型**：Web/游戏→W-TinyLFU 或 ARC；大小差异大/CDN→GDSF；DB/FS→LRU-K(2)/2Q；边缘→CLOCK；多核→分片。

**示例**：`collab/examples/cache-example.md`

## 四、Go 风格 API（`include/go2cpp/go/**`，进行中）

| 文件 | 内容 |
|---|---|
| `net.go.hpp` | `Conn`（Read/Write/Close/LocalAddr/RemoteAddr/SetDeadline）、`Listener`、`Dial`/`Listen` |
| `sync.go.hpp` | `Mutex`/`RWMutex`/`WaitGroup`/`Once`（包装现有） |
| `chan.go.hpp` | `Chan<T>`（send/recv/close/select） |
| `context.go.hpp` | `Context`（Background/WithCancel/WithTimeout/Done/Err/Value） |
| `error.go.hpp` | `Error`/`New`/`Wrap`/`Is`/`As`/`Join` |
| `time.go.hpp` | `Timer`/`Ticker`/`After`/`Sleep`/`Now`/`Since` |
| `io.go.hpp` | `Reader`/`Writer`/`Copy`/`ReadFull`/`WriteString` |

**示例**：`collab/examples/go-api-example.md`

## 五、P2P（公网穿透）

| 能力 | 接口 | 现状 |
|---|---|---|
| **STUN**（NAT 类型） | `StunClient::Detect()` | ✅（4 类 NAT 全对） |
| **UDP 打洞** | `P2PChannel::Connect()` | ✅ |
| **TCP 同时打开** | 同上（加分） | ✅ |
| **TURN 中继** | 打洞失败自动降级 | ✅ |
| **ICE 候选** | `GatherCandidates()` | ✅ |

> **注意**：JinShan 文档说「不做 P2P（CON-09）」——如果确实不需要，在 requests 里标「不需要」，避免白做。

## 六、应用层

| 能力 | 接口 | 现状 |
|---|---|---|
| **流表**（HTTP/2 风格） | `StreamTable`（多路复用/优先级/流控） | ✅（PickNext 20.6×） |
| **资源传输** | `ResourceTransfer`（分块/校验/断点续传） | ✅（294 MiB/s） |
| **KV 流** | `KvStream`（k1=v1;... 流表） | ✅（2.5-3.1×） |

**示例**：`collab/examples/app-example.md`

## 七、Relay（JinShan 专用，待做）

| 能力 | 接口 | 现状 |
|---|---|---|
| **RDRL 编解码** | `RdrlFrame Encode/Decode`（magic/version/type/flags/length/CRC32） | ❌ 待做 |
| **配对表** | `PairingTable`（key → CONTROL/DATA/配额） | ❌ 待做 |
| **1:1 中继管道** | `DataPipe`（32KiB 拷贝 + 256KiB 队列 + 慢 peer） | ❌ 待做 |

**示例**：`collab/examples/relay-example.md`

## 八、约束（JinShan 必须知道）

```
① 不引入第三方库（Boost/Zlib 已移除；OpenSSL 可选）
② Go 风格接口（net/sync/chan/context/error），底层不暴露
③ 平台：Linux（WSL2）+ Windows（JinShan 的 Client/Server）；跨平台见 docs/modules/portability.md
④ 性能：并发 ≥Go；网络 ≤1.1×Go（同机公平口径）
```

## 九、怎么提需求

在 `collab/jinshan-requests.md` 按格式写（见 `BRIDGE-PROMPT.md`）。
