# Go2Cpp —— Go 并发/网络语义的 C++ 复刻与优化工具库

> **一句话**：把 Go 运行时的并发与网络模型（GMP 调度、Fiber、Channel、Context、netpoll）复刻到 C++17，
> 在关键路径上做数量级优化；同时提供一套 **Go 语义开发工具**，让 Go 代码「几乎不改」迁移到 C++；
> 以及覆盖 **多网络场景** 的快速开发组件（TCP/UDP/HTTP/WebSocket/MQTT/CoAP/SOCKS5、RPC、P2P、
> 隧道、帧编解码、中继、缓存、存储、序列化、日志、配置）。

这不是 Go 解析器，也不是源码转译器——它是一个可独立链接的 C++ 库，用 C++ 的类型系统重新表达
Go 的运行时语义，并针对 C++ 场景做工程化取舍（无 GC、RAII、固定栈）。

---

## 一、三大定位

### 1. Go 运行时并发/网络的复刻与优化

| 能力 | 实现 | 与 Go 的关系 |
|---|---|---|
| GMP 调度 | `Task=G` / `Scheduler` / `P` / `M` / worker / sysmon，P 本地队列 + 全局队列 + 工作窃取 | 对齐 Go `proc.go`，但**纯协作式**（无异步抢占），用 `BlockingRegion` 替代 M |
| Fiber | 可迁移栈式协程 + `FiberLocal` + `FiberBin` 栈池 | 对齐 goroutine，但**固定栈**（默认 128 KiB，可调），无栈复制 |
| Channel | 类型化 `Channel<T>`（阻塞/关闭/select/Context 注入）+ `FastChannel`（Vyukov ring） | 对标 Go `chan`；通用 Channel 默认带 Context 语义，FastChannel 对标无锁队列 |
| 同步原语 | 调度器感知的 Mutex/RWMutex/Cond/WaitGroup，parking 机制 | 对齐 `sync` 包 |
| Context | 取消树 + 截止时间 + 取消原因 | 对齐 `context` |
| 错误链 | `Error/Wrap/Is/As/Unwrap/Join` | 对齐 `errors` |
| 网络 IO | epoll `IOManager` + 透明 Socket Hook + FD generation 关闭保护 | 对齐 netpoll + runtime hook |
| 调试 | `debug` 只读观察（事件环、等待图、诊断） | 自研 |

**优化不是口号**：所有性能数字都记录在 [`docs/metrics-ledger.md`](docs/metrics-ledger.md)，
每条带「命令 / 环境 / 日期 / 脚本」，可用 `tools/verify_metrics.sh` 一条命令复跑。

已量化的优化示例（详见台账）：

| 项 | 数据 |
|---|---|
| os readv 合并（M10） | 写 17.96× → **≈111×**、读 15.32× → **≈213×**（相邻 iovec 合并，内核逐段成本 32.2 µs → 1.0 µs） |
| FastChannel（M01b-b） | 16.07 / 6.23 ns 每操作（对标无锁队列，达标） |
| HTTP 吞吐（M05） | 120,755 rps |
| lru 缓存（M07） | 647.5k ops/s、82.4 MiB（vs 工业库 1.0×） |
| TLS 吞吐（M14） | 1.061× vs 工业库 |
| bento 流（M15） | 282,210 pps |
| 调度器 vs Go（M16） | Go@4 **162.7 ns**、C++ 同轮 283–359 ns；差距归因于 Context 语义/单锁/跨 M 唤醒（架构差异，非实现缺陷，已收口） |

### 2. Go 语义开发工具（Go 代码几乎不改迁移）

`include/go2cpp/go/**` 提供与 Go 标准库同形的 API：

| 包 | 头文件 | 覆盖 |
|---|---|---|
| `chan` | `go/chan.go.hpp` | 有缓冲/无缓冲、select、range、close |
| `context` | `go/context.go.hpp` | WithCancel/Timeout/Deadline/Value |
| `error` | `go/error.go.hpp` | errors.New/Is/As/Unwrap/Join |
| `io` | `go/io.go.hpp` | Reader/Writer/Closer/ReadAll/Copy |
| `net` | `go/net.go.hpp` | Dial/Listen/Accept/Conn |
| `sync` | `go/sync.go.hpp` | Mutex/RWMutex/WaitGroup/Once |
| `time` | `go/time.go.hpp` | Timer/Ticker/After/Sleep |
| `flag` | `go/flag.hpp` | 命令行解析 |
| 其他 | `os_signal` / `crypto_rand` / `crc32` | 对齐 Go 同名包 |

设计目标：**Go 业务代码迁移到 C++ 时，主要改 import 与少量语法，不改并发/网络结构。**

### 3. 多网络场景快速开发 C++ 库

| 场景 | 组件 |
|---|---|
| 传输层 | TCP/UDP（`network/socket`）、地址/流/字节数组、endian、批量 send/recv（`bufio_socket`） |
| 应用协议 | HTTP/1.1 客户端/服务端、WebSocket、SSE、HTTP/2（HPACK/帧/流状态机）、QUIC/HTTP3（可选 picoquic） |
| 物联网 | MQTT、CoAP |
| 代理/隧道 | SOCKS5、TCP 隧道（`tunnel`）、HTTP 隧道（`app/http_tunnel`） |
| RPC | 服务端/客户端/注册表、protobuf 风格编解码 |
| P2P | STUN、打洞、TURN、ICE 候选、P2PChannel |
| 可靠传输 | 帧编解码（FrameCodec）、中继管道（RelayPipe）、连接注册表（Hub）、租约/心跳（Lease/Heartbeat）、可靠骨架（SeqNum/重传队列/RFC 6298/拥塞控制纯虚）——见 `rd_net` |
| 缓存 | 7 种淘汰策略：LRU / LRU-K / 2Q / W-TinyLFU / GDSF / ARC / CLOCK |
| 存储 | SQLite（DB/ORM/迁移）、LevelDB 兼容 KV、memcache、队列、watch |
| 序列化 | JSON、CSV、二进制、base64/hex、protobuf 风格 reflect codec |
| 工程化 | 日志（分级/脱敏/滚动）、配置（INI + 热加载 + ConfigVar）、指标（metrics）、限流、退避、压缩（gzip/snappy/lz4/zstd） |
| 运维 | MCP 框架、eBPF 主机监控（sched/futex/syscall/net/tcp/fileio）、CLI、诊断/回放 |
| 应用层 | 流表框架（`app`：帧/流表/连接/资源/KV/传输适配）、通用 Gateway（注册式路由/中间件/可插拔 Resolver/JSON 热更新）、实时房间（RoomHub）、单飞（singleflight）、一致性哈希 |

---

## 二、能力矩阵（按模块）

| 层 | 模块 | 状态 |
|---|---|---|
| 并发 | `scheduler` `fiber` `channel` `sync` `future` `context` `error` `timer` | ✅ 9 原语，1e9×3 压测通过 |
| 网络 | `socket` `http` `websocket` `sse` `http2` `quic`（可选）`mqtt` `coap` `socks5` | ✅ 14/14 ≤1.1× Go |
| 缓存 | `cache`（7 策略） | ✅ Go 标准库没有；几何 1.91× |
| 存储 | `storage`（SQLite/ORM/LevelDB/memcache/queue/watch） | ✅ |
| 序列化 | `encoding`（JSON/CSV/binary/base64/hex）+ reflect codec | ✅ |
| RPC | `rpc`（server/client/registry） | ✅ |
| P2P | `p2p`（STUN/TURN/打洞/ICE） | ✅ |
| 可靠传输 | `rd_net`（四态 socket/EventLoop/协议注入/可靠骨架） | ✅ v3 交付（406/44/44/64 checks） |
| 应用层 | `app`（流表/资源/KV/隧道）、`app::gateway`、`app::realtime_room`、`singleflight`、`consistent_hash` | ✅ |
| 工程化 | `logging` `config` `metrics` `compress` `bufio` | ✅ |
| 运维 | `mcp` `bpf` `os_monitor` `cli` `debug` | ✅ 可选构建 |
| Go 语义 | `go/**`（chan/context/error/io/net/sync/time/flag） | 🔄 持续补齐 |

---

## 三、快速开始

### 3.1 `go()`：提交一个协程

最常用形式——启动调度器，提交 Fiber，等它结束：

```cpp
#include "go2cpp/runtime.hpp"

int main() {
    go2cpp::Scheduler scheduler(4);   // 4 个 P（worker 线程）
    scheduler.start();

    auto task = go2cpp::go(scheduler, [] {
        // 这是一条 Fiber：内部 Sleep / read / Channel 会自动 park，不阻塞 worker
    });
    task->wait();                     // 阻塞当前线程，直到 Fiber 结束

    if (auto err = task->failure()) { // Fiber 边界捕获的异常（可选）
        std::fprintf(stderr, "task failed: %s\n", err->Error().c_str());
    }
    scheduler.shutdown();
}
```

不想管理调度器时，用进程级默认调度器：

```cpp
auto task = go2cpp::go([] { /* Fiber 工作 */ });
task->wait();
go2cpp::shutdown_default_scheduler();   // 程序结束前必须显式关闭
```

Fiber 之间用 Channel / Context 协作：

```cpp
auto ch = go2cpp::make_channel<int>(8);           // 有缓冲 Channel
go2cpp::go([ch] { ch->Send(42); });               // 发送
int v = 0;
ch->Recv(&v);                                     // 当前 Fiber 或主线程接收

auto ctx = go2cpp::context::WithCancel(go2cpp::Context::Background());
go2cpp::go([ctx] {
    if (auto err = go2cpp::Sleep(std::chrono::seconds(10), ctx); err) {
        // ctx 取消后 Sleep 立即返回
    }
});
ctx->Cancel();
```

要点：

| 概念 | 说明 |
|---|---|
| `go(scheduler, fn)` | 提交一个 Fiber，返回 `Task`；`Task::wait()` 等待、`failure()` 取异常 |
| `fiber` | 轻量描述器，`fiber::bind()` 可把已有任务挂到调度器 |
| 自动 park | Fiber 内的 `Sleep`/`read`/`write`/Channel 操作会让出 worker，不阻塞线程 |
| 线程 vs Fiber | 普通线程可直接用 Channel/Context；`BlockingRegion` 把「真阻塞」显式标出 |
| 禁止 | 持锁时 park（Go 不存在的死锁形态，本项目会检测并报错） |

完整示例：`example/concurrency/beginner_demo.cpp`、`runtime_demo.cpp`；规则见
[`docs/fiber_guide.md`](docs/fiber_guide.md)。

### 3.2 UDP 协议注入（`rd::net`，约 20 行 echo）

上层自定义 UDP 协议（RDT/UDP、游戏封包、IM 帧）只需要实现 `Protocol`，
**不改 Go2Cpp 一行代码**：

```cpp
#include "go2cpp/rd_net/event_loop.hpp"
#include "go2cpp/rd_net/protocol.hpp"

// 自定义协议：收到什么就回什么（真实协议在这里解析 24 字节头/ACK/重传）
class Echo : public rd::net::Protocol {
public:
    void OnDatagram(rd::net::ProtocolContext& ctx, rd::net::ByteView data,
                    const rd::net::Endpoint& from) override {
        ctx.SendTo(data, from);   // ByteView 借用：不拷贝，本 tick 内真正 send
    }
    void OnSendComplete(rd::net::ProtocolContext&, std::uint64_t token,
                        rd::net::SendResult result) override {
        // 一个数据报真正发完（成功/失败）：token 与 SendTo 返回值一致
    }
};

int main() {
    auto loop = rd::net::EventLoop::Create();                 // 单线程事件循环
    auto sock = rd::net::DatagramSocket::Open({"0.0.0.0", 9000}, {});
    if (!loop || !sock) return 1;

    Echo echo;
    rd::net::AttachProtocol(**loop, *sock, echo);             // 注入点：不动库代码
    (*loop)->Run();                                           // 阻塞，直到 Stop()
}
```

关键语义：

| 项 | 行为 |
|---|---|
| `ByteView` | **借用视图**，回调返回后失效；`SendTo` 只入队、不拷贝（memset 反证通过） |
| `SendTo` | 一律入队并返回 `token`；真正 send 在同一 tick 内完成，`OnSendComplete` 回调 |
| `ScheduleAfter` | 毫秒定时器（RTO/心跳），到期回调 `OnTimer` |
| 单 tick 包数 | 每 socket ≤ 64 包，防止饿死其它 socket |
| 边界 | Go2Cpp **不认 magic、不校验字段、不分帧、不重排、不决定重传**——全由上层协议实现 |
| 线程 | 所有回调在 `Run()` 所在线程，上层不用加锁 |

编译（`rd_net` 是源码模块，直接编进你的程序）：

```sh
g++ -std=c++17 -O2 -Iinclude \
    src/rd_net/socket.cpp src/rd_net/event_loop.cpp src/rd_net/protocol.cpp \
    your_server.cc \
    -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
    -lgo2cpp_network -lgo2cpp_io -lgo2cpp_context -lgo2cpp_error -lpthread
```

完整可编译示例：`collab/examples/{frame_codec,relay_pipe,hub,lease}/`；
协议注入验收记录见 [`docs/reports/rdnet-v3-review.md`](docs/reports/rdnet-v3-review.md)。

### 3.3 最小 HTTP 服务

```cpp
#include "go2cpp/bento/net/http_server.hpp"

int main() {
    auto server = go2cpp::bento::net::HttpServer::Create(8080);
    server->Route(go2cpp::http::Method::kGet, "/hello",
                  [](const go2cpp::http::Request&) {
                      return go2cpp::http::Response::Text("hello");
                  });
    server->Start();
}
```

应用层脚手架（流表/连接/资源/KV）见 `example/app/`；
通用网关（注册式路由 + 中间件 + 可插拔上游）见 `include/go2cpp/app/gateway.hpp`。

## 四、构建与测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

推荐在源码树外构建。主构建（Bento + MCP + runtime + app + p2p）的完整配置：

```sh
cmake -S . -B .build-bento-wsl -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MCP=ON -DMCP_BUILD_TESTS=ON \
      -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_WITH_APP=ON -DGO2CPP_WITH_P2P=ON \
      -DGO2CPP_BUILD_OS_MONITOR_TESTS=OFF
cmake --build .build-bento-wsl --parallel
ctest --test-dir .build-bento-wsl --output-on-failure
```

测试数量随配置变化，**以 `ctest -N` 为准**：主构建 290 项；`bento+mcp` 80 项；`mcp+osmon` 20 项
（详见 [`docs/testing.md`](docs/testing.md)「测试地图」）。

---

## 五、目录结构

| 路径 | 内容 |
|---|---|
| `include/go2cpp/` | 公共头文件（按模块子目录；`*.hpp` 为兼容转发头） |
| `src/` | 实现：`concurrency/` `network/` `os/` `logging/` `config/` `storage/` `cache/` `bufio/` `p2p/` `app/` `rd_net/` `tunnel/` `rpc/` `encoding/` `hash/` `compress/` |
| `app/` | 应用级 demo：`douyin/`（电商/短视频 6 服务 + 网关）、`bilibili/`（弹幕服务 + Web demo） |
| `collab/` | 与 JinShanProject 的协作区（需求/验收/示例） |
| `tests/` | 测试源码（约 23 万行），按模块划分 |
| `tools/` | 基准（`bench/`）、指标校验（`verify_metrics.sh`）、CLI、迁移/回放 |
| `docs/` | 设计与使用文档，入口 [`docs/INDEX.md`](docs/INDEX.md)；性能台账 [`docs/metrics-ledger.md`](docs/metrics-ledger.md) |
| `example/` | 可运行示例，清单见 [`example/README.md`](example/README.md) |
| `mcp/` `bpf/` | 可选：MCP 框架、eBPF 监控 |
| `third_party/` | 第三方与 Go 参考源码（多数归档被 ignore） |

---

## 六、应用示例

| 示例 | 内容 | 入口 |
|---|---|---|
| 电商 demo | gateway + product/review/order 三服务 + 公网隧道（VPS systemd + 探针 + 断线恢复） | `scripts/shop/`（守护脚本） |
| 抖音 demo | 6 服务 + 网关：用户/视频/Feed/互动/搜索/直播，SQLite 持久化 | `app/douyin/` + [`REQUIREMENTS.md`](app/douyin/REQUIREMENTS.md) |
| B 站弹幕 | WebSocket 房间广播 + 历史弹幕 + 限流 + 40s 视频 Web demo | `app/bilibili/` + [`REQUIREMENTS.md`](app/bilibili/REQUIREMENTS.md) |

---

## 七、设计约束（重要）

| 维度 | Go | Go2Cpp | 后果 |
|---|---|---|---|
| 调度 | 抢占式 | **纯协作式**（只有显式 park/yield） | 长计算会饿死同 P 的 G；持锁 park 是 Go 不存在的死锁形态 |
| 栈 | 可增长（复制搬迁） | **固定栈**（默认 128 KiB） | 深递归会失败；FiberBin 池化复用降低建栈成本 |
| 内存 | GC | **无 GC**（RAII + 引用计数） | 无 GC 停顿；需显式管理生命周期 |
| Channel | 无 Context | **默认注入 Context**（取消/超时/select 回滚） | 比 Go `chan` 多一次状态检查，语义更强 |

这些差异是**设计取舍**，不是缺陷；对应模块的「vs Go」差距已在台账中如实标注（如 M03 调度器收口为
「架构代价，非实现缺陷」）。

---

## 八、文档导航

| 主题 | 入口 |
|---|---|
| 新手 | [`docs/getting_started.md`](docs/getting_started.md) |
| 兼容性/设计边界 | [`docs/compatibility.md`](docs/compatibility.md)、[`docs/design.md`](docs/design.md) |
| 调度器/模块 | [`docs/scheduler.md`](docs/scheduler.md)、[`docs/modules.md`](docs/modules.md) |
| 网络 | [`docs/network.md`](docs/network.md) |
| 性能台账 | [`docs/metrics-ledger.md`](docs/metrics-ledger.md) + `tools/verify_metrics.sh` |
| 测试 | [`docs/testing.md`](docs/testing.md) |
| 协作（JinShan） | [`collab/README.md`](collab/README.md)、[`docs/reports/rdnet-v3-review.md`](docs/reports/rdnet-v3-review.md) |
| 全量索引 | [`docs/INDEX.md`](docs/INDEX.md) |

---

## 九、CMake 选项（摘要）

| 选项 | 默认 | 说明 |
|---|---|---|
| `BUILD_SHARED_LIBS` | `ON` | 共享模块；静态构建需同时 `-DGO2CPP_BUILD_HOOK=OFF` |
| `GO2CPP_BUILD_HOOK` | `ON` | Linux 系统调用 Hook（透明 park） |
| `GO2CPP_BUILD_TESTS` | `ON` | 构建并注册 ctest |
| `GO2CPP_WITH_P2P` | `ON` | P2P 公网穿透 |
| `GO2CPP_WITH_APP` | `OFF` | 应用层流表框架 |
| `GO2CPP_WITH_MCP` | `OFF` | 内置 MCP 框架 |
| `GO2CPP_ENABLE_EBPF` | `ON` | eBPF 监控（缺依赖自动降级） |
| `GO2CPP_ENABLE_TSAN` | `OFF` | ThreadSanitizer |

完整列表见下方历史版本或 `cmake/Go2CppOptions.cmake`。
