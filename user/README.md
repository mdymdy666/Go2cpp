# user/ —— 示例、模块手册与跨模块场景

> **这个目录回答「怎么用」**：可运行示例、按「模块 → 能力 → 异常」组织的手册、
> 以及跨模块组合场景。想看框架全貌回 [根 README](../README.md)；
> 要改代码、追数据流去 [`deep/`](deep/README.md)。

## 性能数字约定（写文档、改文档都必须遵守）

**禁止裸数字。** 任何吞吐 / 延迟 / 倍数都必须写成「数值（口径）+ 来源报告 + 日期」，例如：

> `2385 MiB/s、开销 1.27×（8 MiB body，WSL2 环回，单连接；见 docs/reports/tunnel-raw-default-2026-11-04.md，2026-11-04）`

- **来源只认 `docs/reports/`**。聊天记录、提交信息、`docs/**` 的历史模块文档都不算来源。
- 没有报告、也没有当场复跑 → 写「未实测」并说明为什么，**不写数字**。
- 引用历史文档的数字，必须标「历史数据，未复跑」，且不得当结论。
- 报告更新后，引用它的文档必须同步；**文档 ↔ 报告对照表**见
  [`docs/plans/doc-perf-sync.md`](../docs/plans/doc-perf-sync.md)。
- `deep/**` 的写作铁律见 [`deep/README.md`](deep/README.md)；
  `docs/**` 是交接摘要（历史快照），允许保留旧值但必须标「历史」。

## 目录分层

```
user/
├── README.md          ← 你在这里：模块索引 + 学习路径
├── QUICKSTART.md      ← 5 分钟跑通（装依赖 → 构建 → 第一个例子）
├── modules/           ← 每个模块一篇手册：能力清单 / 最小示例 / 3 个异常 / 示例清单
│   ├── concurrency/   ├── network/    ├── storage/    ├── cache/
│   ├── logging/       ├── config/     ├── debug/      ├── tools/
│   └── app/
├── recipes/           ← 跨模块组合场景（HTTP + 优雅关闭、热更新 + 日志、背压……）
├── deep/              ← 17 份深入文档（入口/行号/失败路径，面向改代码的人）
└── docs/              ← 17 份交接摘要（模块是什么/API/边界，面向第一次接触的人）
```

**三档阅读顺序**：`QUICKSTART` → `modules/<x>/README.md` → `deep/<x>.md`。
示例总数：**79 个单文件示例**（`modules/**/*.cpp`，多数注册为 ctest）+
**7 个应用级服务程序**（`modules/app/`），合计 86 个可运行程序。

## 5 分钟跑通

```sh
# 1) 依赖（Linux / WSL2；IO 层是 epoll）
sudo apt install -y build-essential cmake ninja-build g++-13

# 2) 构建（Bento 默认 OFF，这里打开以构建 app 示例）
cmake -S . -B build -G Ninja -DGO2CPP_WITH_BENTO=ON
cmake --build build -j"$(nproc)"

# 3) 第一个例子
./build/go2cpp_beginner_demo        # 退出码 0 = 示例自身断言全过
```

只构建/单跑某个例子：

```sh
cmake --build build --target go2cpp_http_demo -j
ctest --test-dir build -R go2cpp_http_demo --output-on-failure
```

完整步骤（含「改一个例子」）见 [QUICKSTART.md](QUICKSTART.md)。

## 模块索引

| 模块 | 一句话定位 | 示例数 | 深入文档 |
| --- | --- | ---: | --- |
| [并发 concurrency](modules/concurrency/README.md) | GMP 调度器、Fiber、Channel/Select、Context、同步原语、Future/Timer | 32 | [01-concurrency](deep/01-concurrency.md) |
| [网络 network](modules/network/README.md) | epoll IOManager、LD_PRELOAD Hook、HTTP/1.1/2、WebSocket/SSE、QUIC | 22 | [02-network](deep/02-network.md) |
| [存储 storage](modules/storage/README.md) | SQLite/ORM、LSM KV、LevelDB 对照、Queue/Watch/MemCache | 0（见 tests） | [04-storage](deep/04-storage.md) |
| [缓存 cache](modules/cache/README.md) | 7 种淘汰策略统一接口 + 线程安全/分片装饰器（header-only） | 1（`app/cache_demo`） | [05-cache](deep/05-cache.md) |
| [日志 logging](modules/logging/README.md) | Filter/Formatter/Worker/Sink 四层组合、异步投递、轮转 | 3 | [10-observability](deep/10-observability.md) |
| [配置 config](modules/config/README.md) | INI 解析、强类型 `ConfigVar`、两阶段提交热加载、只读快照 | 7 | [10-observability](deep/10-observability.md) |
| [调试 debug](modules/debug/README.md) | 只读内省：调度器快照、事件环、协作式断点、dump CLI | 1 | [10-observability](deep/10-observability.md) |
| [工具 tools](modules/tools/README.md) | Go 标准库对应物：fmt/strings/strconv/streams/time/error/defer | 14 | [11-tools](deep/11-tools.md) |
| [应用 app](modules/app/README.md) | 应用级服务：web/chat/p2p/cache/stream（含简单版与全量版） | 7 个程序 | [12-app](deep/12-app.md) |

> 示例数只统计 `user/modules/` 下的可运行示例；`cache` 的 7 策略对比放在
> `modules/app/cache_demo/`，`storage` 的可运行代码在 `tests/storage/`。

## 跨模块场景（recipes）

| 场景 | 组合 | 文档 |
| --- | --- | --- |
| HTTP 服务 + 优雅关闭 | network + concurrency + app | [recipes/01-http-graceful-shutdown.md](recipes/01-http-graceful-shutdown.md) |
| 配置热更新驱动日志级别 | config + logging + 调度器 | [recipes/02-config-hot-reload-logging.md](recipes/02-config-hot-reload-logging.md) |
| 有界队列做背压 | concurrency + app | [recipes/03-backpressure-bounded-queue.md](recipes/03-backpressure-bounded-queue.md) |

索引与新增约定见 [recipes/README.md](recipes/README.md)。

## 学习路径

**入门（1–2 小时）**——建立「Fiber 是什么、阻塞怎么写」的直觉：

1. [`concurrency/beginner_demo.cpp`](modules/concurrency/beginner_demo.cpp) —— `go()` + Channel + Select 超时
2. [`concurrency/runtime_basic_demo.cpp`](modules/concurrency/runtime_basic_demo.cpp) —— 聚合头一次用多个子系统
3. [`concurrency/channel_demo.cpp`](modules/concurrency/channel_demo.cpp) —— 无缓冲/有缓冲/Close
4. [`concurrency/context_demo.cpp`](modules/concurrency/context_demo.cpp) —— 取消、超时、带值
5. [`tools/strings_demo.cpp`](modules/tools/strings_demo.cpp) + [`tools/fmt_demo.cpp`](modules/tools/fmt_demo.cpp)
6. [`network/http_demo.cpp`](modules/network/http_demo.cpp) —— 不碰 socket 的 HTTP 往返

**进阶（半天）**——并发正确性与 IO 模型：

7. [`concurrency/scheduler_full.cpp`](modules/concurrency/scheduler_full.cpp) —— P/M 规模、并行度、关闭时序
8. [`concurrency/sync_full.cpp`](modules/concurrency/sync_full.cpp) —— 跨执行流解锁、`LockFor` 超时
9. [`concurrency/mixed_runtime_demo.cpp`](modules/concurrency/mixed_runtime_demo.cpp) —— 普通线程与 Fiber 混用
10. [`network/io_wait_many_demo.cpp`](modules/network/io_wait_many_demo.cpp) —— epoll 多 fd + Channel 桥接
11. [`network/websocket_full.cpp`](modules/network/websocket_full.cpp) / [`sse_full.cpp`](modules/network/sse_full.cpp) —— 协议边界
12. [`config/config_full.cpp`](modules/config/config_full.cpp) + [`logging/log_full.cpp`](modules/logging/log_full.cpp)

**专家（按需）**——性能边界、故障定位、跨进程：

13. [`concurrency/fiber_stack_limit_full.cpp`](modules/concurrency/fiber_stack_limit_full.cpp) —— 栈上限实测与对策
14. [`concurrency/parking_condition_full.cpp`](modules/concurrency/parking_condition_full.cpp) / [`thread_policy_full.cpp`](modules/concurrency/thread_policy_full.cpp)
15. [`debug/debug_module_demo.cpp`](modules/debug/debug_module_demo.cpp) —— 快照、事件环、断点、dump
16. [`app/stream_demo/`](modules/app/stream_demo/) —— 流表 + 两级流控 + 资源/KV 传输
17. [`app/p2p_demo/`](modules/app/p2p_demo/) —— STUN + UDP 打洞多进程闭环

## 架构一览

```
┌────────────────────────────────────────────────────────────────────────────┐
│ 应用层   user/modules/app 示例（web/chat/p2p/cache/stream）                  │
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
2. **阻塞语义统一由 `ParkingCondition` 承接**：Channel、Select、sync、Timer、IO
   的等待最终都汇总到它——受管 Fiber park 让出 M，普通线程走原生条件变量。
3. **协议层不拥有连接**：`http::Parser`/`websocket::WebSocket`/`sse::Stream` 只操作
   传入的 `Socket` 或纯字节流，所以既能跑服务端，也能跑 `socketpair` 上的测试。

## 模块 → 源码位置

| 模块 | 示例目录 | 头文件 | 实现 |
| --- | --- | --- | --- |
| 调度器 | `modules/concurrency/` | `include/go2cpp/concurrency/scheduler/scheduler.hpp` | `src/concurrency/scheduler.cpp` |
| Fiber | `modules/concurrency/` | `include/go2cpp/concurrency/fiber.hpp` | `src/concurrency/fiber.cpp` |
| Channel/Select | `modules/concurrency/` | `include/go2cpp/concurrency/channel.hpp` | header-only |
| Context | `modules/concurrency/` | `include/go2cpp/concurrency/context.hpp` | `src/concurrency/context.cpp` |
| 同步原语 | `modules/concurrency/` | `include/go2cpp/sync.hpp` | `src/concurrency/sync.cpp` |
| IO 多路复用 | `modules/network/` | `include/go2cpp/network/io.hpp` | `src/network/io.cpp` |
| Socket / Hook | `modules/network/` | `include/go2cpp/network/{socket,hook}.hpp` | `src/network/{socket,hook}.cpp` |
| HTTP / WebSocket / SSE | `modules/network/` | `include/go2cpp/network/{http,websocket,sse}.hpp` | `src/network/` |
| 日志 | `modules/logging/` | `include/go2cpp/logging/log.hpp` | `src/logging/log.cpp` |
| 配置 | `modules/config/` | `include/go2cpp/config/config.hpp` | `src/config/config.cpp` |
| 调试 | `modules/debug/` | `include/go2cpp/debug/*.hpp` | `src/debug/` |
| Go 标准库工具 | `modules/tools/` | `include/go2cpp/tools/*.hpp` | 多为 header-only |
| 应用层框架 | `modules/app/stream_demo` | `include/go2cpp/app/*.hpp` | `src/app/` |
| P2P / 缓存 | `modules/app/{p2p_demo,cache_demo}` | `include/go2cpp/{p2p,cache}/` | `src/p2p/`、header-only |

## 约定

- **`demo` vs `full`**：`demo` 是 3 个左右主干场景的最小版；`full` 是同组件的
  6–8 个场景，含错误/边界与规模测试。
- **退出码即结论**：示例自己校验并返回 0/1；日志里的「失败状态」往往是**教学内容**
  （演示错误路径），不代表进程失败。
- **目标名 = 文件名**：`modules/network/http_demo.cpp` → `go2cpp_http_demo`，
  产物在构建目录根部（`app/` 的产物在 `build/user/app/`）。
- **示例编号**：各模块 README 的示例清单用 `01、02…` 表示学习顺序；文件名保留
  `*_demo`/`*_full` 以稳定 ctest target 名（`go2cpp_<文件名>`），两者不冲突。
- **新增示例要登记三处**：`cmake/Go2CppTests.cmake` 的 `GO2CPP_EXAMPLES`、
  所属模块 README 的示例清单、`recipes/` 若涉及跨模块组合再补一篇。

## 文档索引

| 想了解 | 看这里 |
| --- | --- |
| 5 分钟跑起来、改一个例子 | [QUICKSTART.md](QUICKSTART.md) |
| 某模块的能力/示例/常见异常 | `modules/<模块>/README.md`（见上方索引表） |
| 跨模块组合场景 | [recipes/README.md](recipes/README.md) |
| 改代码前必读（入口/行号/失败路径） | [deep/README.md](deep/README.md)（17 份） |
| 模块交接摘要 | [docs/](docs/)（17 份） |
| 环境准备与故障排查 | [../docs/getting_started.md](../docs/getting_started.md) |
| 设计专题 | `../docs/network.md`、`../docs/sse.md`、`../docs/quic.md`、`../docs/error_guide.md` |
| 历史审计记录 | [`../docs/audit/`](../docs/audit/README.md)（37 份限制/审计，非教程） |
