# user/ —— Go2Cpp 示例、文档与快速入门

> **这个目录是「怎么用」**：可运行示例、按模块组织的说明、以及从零到跑通的路。
> 想看框架全貌回 [根 README](../README.md)，想看设计取舍去 [docs/](../docs/)。

## 这是什么

**Go2Cpp 运行时语义兼容层** —— 一个可独立使用的 C++17 库，用 C++ 重建 Go 运行时
的核心语义子集（GMP 调度、channel/select、context 取消树、Fiber、epoll IO、日志、配置）。

它**不是** Go 解析器，也**不是** Go→C++ 转译器；它提供的是**手写 C++ 也能用的
Go 风格运行时**。

`user/` 下有 **86 个可编译、可运行的示例程序**：79 个单文件示例（其中 75 个注册为
ctest）加 7 个应用级服务程序。

## 能做什么

| 能力 | 一句话 | 从这里开始 |
| --- | --- | --- |
| 绿色线程调度 | GMP 三层（G/P/M）、工作窃取、M 动态扩缩、park/wake | `concurrency/beginner_demo.cpp` |
| Fiber | 独立栈 + 池化复用、`FiberLocal`、跨线程迁移 API | `concurrency/fiber_sync_demo.cpp` |
| Channel / Select | 与 Go `hchan` 对齐的环形缓冲、多路等待、超时/取消 | `concurrency/channel_demo.cpp` |
| Context | 取消树级联、超时、带值、局部回滚（savepoint + 补偿） | `concurrency/context_rollback_demo.cpp` |
| 同步原语 | Mutex/CV/WaitGroup 同时服务 Fiber 与 OS 线程，跨线程解锁 | `concurrency/sync_demo.cpp` |
| 网络 IO | epoll `IOManager` + `LD_PRELOAD` Hook，阻塞式写法不占线程 | `network/io_wait_many_demo.cpp` |
| HTTP/1.1 | 增量解析器（半包/粘包/chunked）、Client、Server、Router、静态文件 | `network/http_demo.cpp` |
| WebSocket / SSE | RFC 6455 握手与分片；SSE 事件编码 + 增量解析 + 心跳 | `network/websocket_demo.cpp`、`network/sse_demo.cpp` |
| 日志 | Filter/Formatter/Worker/Sink 四层组合，异步投递、文件轮转 | `logging/log_demo.cpp` |
| 配置 | INI 解析、强类型 `ConfigVar`、两阶段提交热加载、只读运行时快照 | `config/config_demo.cpp` |
| 调试内省 | 调度器快照（text/ini/json）、事件采集、协作式断点、dump CLI | `debug/debug_module_demo.cpp` |
| 应用框架 | HTTP/2 风格流表、两级流控、资源/KV 分块传输（`-DGO2CPP_WITH_APP=ON`） | `app/stream_demo/` |
| P2P / 缓存 | UDP 打洞 + STUN；7 种缓存淘汰策略对比 | `app/p2p_demo/`、`app/cache_demo/` |

## 目录地图

```
user/
├── README.md            ← 你在这里（总览 + 目录地图 + 架构 + 学习路径）
├── QUICKSTART.md        ← 从零开始：装依赖 → 构建 → 跑第一个例子
├── concurrency/         ← 调度器 / Fiber / Channel / Context / Sync / Future / Timer（32 个）
├── network/             ← socket / epoll / hook / 分帧 / HTTP / WebSocket / SSE / HTTP2 / QUIC（22 个）
├── tools/               ← Go 标准库对应物：fmt/strings/strconv/streams/time/error/defer（14 个）
├── config/              ← INI、ConfigVar、配置中心与热加载（7 个）
├── logging/             ← 日志四层组合、INI 日志配置（3 个）
├── debug/               ← 运行期内省、事件环、断点、dump CLI（1 个）
├── app/                 ← 应用级服务：web/chat/p2p/cache/stream（7 个程序）
└── WarningDocement/     ← 历史模块审计记录（不是示例，按需查证用）
```

每个子目录都有自己的 `README.md`（这个模块是什么 / 架构 / 最小用法 / 示例清单 /
源码位置 / 注意事项）。

## 快速入门（3 步）

```sh
# 1) 装依赖（Ubuntu 24.04 示例；需要 Linux，因为 IO 层是 epoll）
sudo apt install -y build-essential cmake ninja-build g++-13

# 2) 构建（在仓库根目录；Bento 传输协议默认 OFF，这里打开以构建 app 示例）
cmake -S . -B build -G Ninja -DGO2CPP_WITH_BENTO=ON
cmake --build build -j"$(nproc)"

# 3) 跑第一个例子
./build/go2cpp_beginner_demo
```

看到 `[beginner][场景1] ...` 并退出码为 0 就成功了。接着读
[QUICKSTART.md](QUICKSTART.md)（含「改一个例子」的完整步骤）。

> 只想验证某一模块：`cmake --build build --target go2cpp_http_demo -j`，
> 产物固定在 `build/go2cpp_<名字>`；`ctest -R go2cpp_http_demo` 可单跑。

## 模块 → 源码位置

| 模块 | 示例目录 | 头文件 | 实现 |
| --- | --- | --- | --- |
| 调度器 | `concurrency/` | `include/go2cpp/concurrency/scheduler/scheduler.hpp` | `src/concurrency/scheduler.cpp` |
| Fiber | `concurrency/` | `include/go2cpp/concurrency/fiber.hpp` | `src/concurrency/fiber.cpp` |
| Channel/Select | `concurrency/` | `include/go2cpp/concurrency/channel.hpp` | header-only |
| Context | `concurrency/` | `include/go2cpp/concurrency/context.hpp` | `src/concurrency/context.cpp` |
| 同步原语 | `concurrency/` | `include/go2cpp/sync.hpp` | `src/concurrency/sync.cpp` |
| IO 多路复用 | `network/` | `include/go2cpp/network/io.hpp` | `src/network/io.cpp` |
| Socket | `network/` | `include/go2cpp/network/socket.hpp` | `src/network/socket.cpp` |
| Hook | `network/` | `include/go2cpp/network/hook.hpp` | `src/network/hook.cpp` |
| HTTP/1.1 | `network/` | `include/go2cpp/network/http.hpp` | `src/network/http.cpp` |
| WebSocket | `network/` | `include/go2cpp/network/websocket.hpp` | `src/network/websocket.cpp` |
| SSE | `network/` | `include/go2cpp/network/sse.hpp` | `src/network/sse.cpp` |
| 日志 | `logging/` | `include/go2cpp/logging/log.hpp` | `src/logging/log.cpp` |
| 配置 | `config/` | `include/go2cpp/config/config.hpp` | `src/config/config.cpp` |
| 调试 | `debug/` | `include/go2cpp/debug/*.hpp` | `src/debug/` |
| Go 标准库工具 | `tools/` | `include/go2cpp/tools/{fmt,strings,strconv,streams,time,error,control_flow}.hpp` | `src/tools/` |
| 应用层框架 | `app/stream_demo` | `include/go2cpp/app/*.hpp` | `src/app/` |
| P2P 穿透 | `app/p2p_demo` | `include/go2cpp/p2p/` | `src/p2p/` |
| 缓存策略 | `app/cache_demo` | `include/go2cpp/cache/cache.hpp` | header-only |

## 架构一览

```
┌────────────────────────────────────────────────────────────────────────────┐
│ 应用层   user/app 示例（web/chat/p2p/cache/stream）                          │
│          go2cpp::app（流表/资源/KV）· go2cpp::p2p · mcp/ · tools/cli        │
├────────────────────────────────────────────────────────────────────────────┤
│ 媒体/存储  media · storage · cache（7 策略）· compress · encoding · bufio     │
├────────────────────────────────────────────────────────────────────────────┤
│ 协议      HTTP/1.1 · WebSocket · SSE · HTTP/2 · HTTP/3(QUIC)                 │
├────────────────────────────────────────────────────────────────────────────┤
│ 网络      net::Socket（RAII）· io::IOManager（epoll）· net::hook（拦截）      │
├────────────────────────────────────────────────────────────────────────────┤
│ 并发      Scheduler(GMP) · Fiber · Channel/Select · Context · sync · Future   │
│           TimerService · ParkingCondition                                    │
├────────────────────────────────────────────────────────────────────────────┤
│ 基础      error（不可变错误链）· fmt/strings/strconv/streams/time · logging    │
│           config · debug（只读内省层）                                        │
└────────────────────────────────────────────────────────────────────────────┘
        横向：thread_policy（线程参与策略）· reflect（字段反射）· metrics
```

**三条主干关系**：

1. **下层不知道上层**：`error`/`tools` 不引用并发；并发不引用网络；网络不引用 app。
   `debug` 是唯一「上层观察下层」的模块，框架对 `debug` 零依赖。
2. **阻塞语义统一由 `ParkingCondition` 承接**：Channel、Select、sync、Timer、IO 的
   等待最终都汇总到它 —— 受管 Fiber park 让出 M，普通线程走原生条件变量。
3. **协议层不拥有连接**：`http::Parser`/`websocket::WebSocket`/`sse::Stream` 只操作
   传入的 `Socket` 或纯字节流，所以既能跑服务端，也能跑 `socketpair` 上的测试。

## 学习路径

**新手（1–2 小时）** —— 先建立「Fiber 是什么、阻塞怎么写」的直觉：

1. `concurrency/beginner_demo.cpp` —— `go()` + Channel + Select 超时（66 行）
2. `concurrency/runtime_basic_demo.cpp` —— 一个聚合头用多个子系统
3. `concurrency/channel_demo.cpp` —— 无缓冲/有缓冲/Close 的差别
4. `concurrency/context_demo.cpp` —— 取消、超时、带值
5. `tools/strings_demo.cpp` + `tools/fmt_demo.cpp` —— 与 Go 一致的字符串/格式化
6. `network/http_demo.cpp` —— 纯计算的 HTTP 往返（不碰 socket）

**进阶（半天）** —— 开始关心并发正确性与 IO 模型：

7. `concurrency/scheduler_full.cpp` —— P/M 规模、并行度观测、关闭时序
8. `concurrency/sync_full.cpp` —— 跨执行流解锁、`LockFor` 超时
9. `concurrency/mixed_runtime_demo.cpp` —— 普通线程与 Fiber 混用同步原语
10. `network/io_wait_many_demo.cpp` —— epoll 多 fd 等待 + Channel 桥接 + 取消
11. `network/websocket_full.cpp` / `sse_full.cpp` —— 协议边界的七种情况
12. `config/config_full.cpp` + `logging/log_full.cpp` —— 生产配置与日志

**专家（按需）** —— 性能边界、故障定位、跨进程：

13. `concurrency/fiber_stack_limit_full.cpp` —— 栈上限实测与六条对策
14. `concurrency/parking_condition_full.cpp` / `thread_policy_full.cpp` —— 调度器内部机制
15. `debug/debug_module_demo.cpp` —— 快照、事件环、断点、dump
16. `app/stream_demo/` —— 流表 + 两级流控 + 资源/KV 传输
17. `app/p2p_demo/` —— STUN + UDP 打洞多进程闭环

## 文档索引

| 想了解 | 看这里 |
| --- | --- |
| 3 步跑起来、改一个例子 | [QUICKSTART.md](QUICKSTART.md) |
| 各模块用法与示例清单 | `concurrency/README.md`、`network/README.md`、`tools/README.md`、`config/README.md`、`logging/README.md`、`debug/README.md`、`app/README.md` |
| 环境准备与故障排查 | [docs/getting_started.md](../docs/getting_started.md) |
| 网络/协议设计 | [docs/network.md](../docs/network.md)、[docs/sse.md](../docs/sse.md)、[docs/quic.md](../docs/quic.md) |
| Fiber/Context/控制流指南 | [docs/fiber_guide.md](../docs/fiber_guide.md)、[docs/context_guide.md](../docs/context_guide.md)、[docs/control_flow_guide.md](../docs/control_flow_guide.md) |
| 错误处理 | [docs/error_guide.md](../docs/error_guide.md) |
| 历史审计记录 | `WarningDocement/`（模块级问题的原始记录，非教程） |

## 全模块交接文档（16 份）

[`docs/`](docs/) 下每个模块一份**交接文档**（180–400 行），统一 9 节结构：
这个模块是什么 / 架构与数据流 / 对外 API / 最小用法 / 源码位置 / 性能数据 /
边界与已知限制 / 测试与验证 / 交接注意。新人接手某模块时**先读这一份**，
再按文末链接进入设计全文。

| # | 模块 | 交接文档 | 一句话 |
|---:|---|---|---|
| 1 | 并发 | [01-concurrency.md](docs/01-concurrency.md) | GMP 调度器 + Fiber + Channel/Select + Context/Future/Timer + 跨执行流同步 |
| 2 | 网络 | [02-network.md](docs/02-network.md) | epoll `IOManager` + `LD_PRELOAD` Hook + Socket/TCP/UDP + HTTP/1.1/2 + WS/SSE + TLS |
| 3 | Bento | [03-bento.md](docs/03-bento.md) | UDP 上的 Bento v0.2 可靠传输：STREAM/DATAGRAM/VIDEO + SACK/FEC/BBR + PeerHub |
| 4 | 存储 | [04-storage.md](docs/04-storage.md) | SQLite/ORM、LevelDB 风格 KV、LSM、Queue/Watch、MemCache |
| 5 | 缓存 | [05-cache.md](docs/05-cache.md) | 7 种淘汰策略统一接口（LRU/LRU-K/2Q/TinyLFU/GDSF/ARC/CLOCK）与命中率对比 |
| 6 | 媒体 | [06-media.md](docs/06-media.md) | 采集→编码→分片→jitter→降级→SFU/信令→渲染/音频/AVI 的实时链路 |
| 7 | P2P | [07-p2p.md](docs/07-p2p.md) | STUN/TURN/ICE、UDP 打洞、TCP 同时打开与候选交换 |
| 8 | RPC | [08-rpc.md](docs/08-rpc.md) | G2RP 协议、服务注册、同步/异步/流式调用、WebSocket 传输 |
| 9 | 分布式 | [09-distributed.md](docs/09-distributed.md) | ServiceRegistry、分布式锁、LeaderElection、Coordinator |
| 10 | 可观测 | [10-observability.md](docs/10-observability.md) | metrics/Prometheus、日志/脱敏/轮转、配置热更新、debug 事件环/回放 |
| 11 | 工具 | [11-tools.md](docs/11-tools.md) | hash/encoding/compress/reflect/container/math/path/strconv/strings |
| 12 | 应用层 | [12-app.md](docs/12-app.md) | HTTP/2 风格流表、两级流控、资源/KV 分块传输、bento app |
| 13 | Relay/JinShan | [13-relay-jinshan.md](docs/13-relay-jinshan.md) | FrameCodec + RelayPipe + Hub/Lease + 有界队列（relay 替换四件套） |
| 14 | Tunnel | [14-tunnel.md](docs/14-tunnel.md) | 公网穿透：PSK 鉴权、多路复用、心跳、drain、metrics |
| 15 | os | [15-os.md](docs/15-os.md) | file/dir + Process/Run + pipe/shm/mq/pty + Supervisor/ProcessPool |
| 16 | Go API | [16-go-api.md](docs/16-go-api.md) | Go 标准库兼容层：net/sync/chan/context/error/time/io/os/signal/crc32/rand/flag |

> 约定：每份都标注「哪些数据是实测、哪些没测」；性能数字一律指向
> [`docs/reports/asm-*.md`](../docs/reports/) 或 `docs/modules/*.md` 的原始出处，
> 不复制二手数字。

## 约定

- **`demo` vs `full`**：`demo` 是 3 个左右主干场景的最小版；`full` 是同一组件的
  6–8 个场景，含错误/边界与规模测试。
- **退出码即结论**：示例自己校验并返回 0/1；日志里的「失败状态」往往是**教学内容**
  （演示错误路径），不代表进程失败。
- **目标名 = 文件名**：`user/network/http_demo.cpp` → `go2cpp_http_demo`，
  产物在构建目录根部（`app/` 的产物在 `build/user/app/`）。
- **示例注册列表**在 `cmake/Go2CppTests.cmake` 的 `GO2CPP_EXAMPLES`；
  新增示例需要两处登记：该列表 + 本目录的 README 表格。
