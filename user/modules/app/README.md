# user/app —— 应用级示例（web / chat / p2p / cache / stream）

> 7 个**可运行的服务或完整闭环**：4 个服务型示例（web_server / chat_server 各有
> simple 与 full 两版）+ 3 个新能力示例（P2P 打洞、缓存策略对比、应用层流表）。

## 这个模块是什么

`user/` 下其他目录是「单文件、跑完即退」的组件示例；这里不同：**这些是服务或完整闭环**，
会监听端口、等待信号或驱动多进程，因此**不注册 ctest**，由各自 README 里的
curl / 内置客户端 / 脚本驱动。

它们回答的问题是：**把这些底层能力拼起来，能不能真的跑一个服务？**

| 示例 | 拼了什么 | 证明什么 |
| --- | --- | --- |
| `web_server_simple` | `bento::HttpServer` + `Scheduler` + `Channel` | HTTP 服务最小闭环 + 信号优雅关闭 |
| `web_server_full` | 再加路由/中间件/静态文件/SSE/INI/分级日志 | 工程化的服务分层 |
| `chat_server_simple` | `bento::LinkServer` + `PeerHub` | UDP 可靠广播的多客户端聊天 |
| `chat_server_full` | 再加房间/私聊/心跳/重连 token/持久化 | 有状态的会话服务 |
| `p2p_demo` | `p2p::P2PChannel` + `p2p::StunServer` | NAT 后两台主机 UDP 打洞 |
| `cache_demo` | `cache::Workload` + `cache::MakeCache`（7 策略） | 淘汰策略的命中率差异 |
| `stream_demo` | `app::Connection` + `StreamTable` + 资源/KV | HTTP/2 风格多路复用与两级流控 |

## 底层架构

```
  ┌───────────────────────────── user/app ─────────────────────────────┐
  │                                                                    │
  │  web_server_simple/full ──► bento::HttpServer ──► HTTP/1.1 + SSE    │
  │  chat_server_simple/full ─► bento::LinkServer ──► PeerHub（UDP）     │
  │  p2p_demo ────────────────► p2p::P2PChannel ───► StunServer（NAT）  │
  │  cache_demo ──────────────► cache::MakeCache（7 策略，header-only）  │
  │  stream_demo ─────────────► app::Connection                          │
  │                                  │                                  │
  │                                  ├─ FrameDecoder（线格式）           │
  │                                  ├─ StreamTable（状态/窗口/调度）     │
  │                                  ├─ ResourceReceiver / KvAssembler  │
  │                                  └─ ITransport（可注入）             │
  └────────────────────────────────────────────────────────────────────┘
```

**`stream_demo` 的分层最值得读**（`include/go2cpp/app/`）：

- `Connection` 把线格式、流表、业务重组串起来，但**不关心字节从哪来** ——
  传输通过 `ITransport` 注入，所以同一份逻辑能跑在内存管道、Bento Link（UDP）
  或 HTTP 隧道（TCP）上；
- `StreamTable` 是**纯逻辑层**（无 IO、无锁、不碰 socket），回答三个问题：
  这条帧能不能发/收（流状态机）、现在还能发多少字节（连接窗口 + 流窗口）、
  下一条该发哪条流（依赖树 + 权重的加权公平调度）；
- 把这三件事从 `Connection` 剥出来，是为了能用**纯内存**方式穷举测试状态转换
  与调度公平性。

**并发模型**：`Step()`/`ReadEvent()` 必须由**单一 IO 线程/Fiber** 调用；
`SendKv*`/`SendResource*`/`SendBytes` 可在任意线程调用（内部一把 IO 锁串行化）。

## 目录与规模

| 示例 | 版本 | 文件 | 逻辑行 | 核心组件 |
| --- | --- | --- | --- | --- |
| web_server | simple | `web_server_simple/main.cpp` | 159 | `HttpServer`、`Scheduler`、`Channel` |
| web_server | full | `web_server_full/`（9 模块） | 1267 | + `sse::Stream`、Router、中间件 |
| chat_server | simple | `chat_server_simple/main.cpp` | 260 | `LinkServer`、`PeerHub` |
| chat_server | full | `chat_server_full/`（8 模块） | 1383 | + 房间/私聊/心跳/持久化 |
| p2p_demo | simple | `p2p_demo/main.cpp` + `run_demo.sh` | 409 | `p2p::P2PChannel`、`StunServer` |
| cache_demo | simple | `cache_demo/main.cpp` | 240 | `cache::Workload`、7 策略 |
| stream_demo | simple | `stream_demo/main.cpp` | 368 | `app::Connection`、`StreamTable` |

> full 版按职责拆成配置/日志/路由/协议/会话/存储多个模块，行数超出 simple 版是
> 刻意的：**没有用「省略号」占位**。只要最小实现就看 simple 版。

## 构建

```sh
cmake -S . -B build -G Ninja -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_APP=ON
cmake --build build --target \
  go2cpp_web_server_simple go2cpp_web_server_full \
  go2cpp_chat_server_simple go2cpp_chat_server_full \
  go2cpp_p2p_demo go2cpp_cache_demo go2cpp_stream_demo -j8
```

产物在 `build/user/app/` 下（与其他示例的 `build/` 根目录不同）。

各 target 的注册条件：

| target | 需要的开关 / 目标 | 关闭时的行为 |
| --- | --- | --- |
| `go2cpp_web_server_*` | `-DGO2CPP_WITH_BENTO=ON` → `go2cpp_bento_http` | 打印 `user/app: web_server 示例跳过` |
| `go2cpp_chat_server_*` | `-DGO2CPP_WITH_BENTO=ON` → `go2cpp_bento` | 打印 `user/app: chat_server 示例跳过` |
| `go2cpp_p2p_demo` | `-DGO2CPP_WITH_P2P=ON`（默认 ON） | 打印 `user/app: p2p_demo 跳过` |
| `go2cpp_cache_demo` | 仓库 `include/go2cpp/cache/`（header-only） | 头文件缺失时跳过 |
| `go2cpp_stream_demo` | `-DGO2CPP_WITH_APP=ON`（**默认 OFF**） | 不注册 target |

CMake 不会静默把示例编译成空壳：缺失依赖时打印明确原因。

## 运行

```sh
# 无端口的两个：直接跑
./build/user/app/go2cpp_cache_demo
./build/user/app/go2cpp_stream_demo      # STREAM_OK + SHA-256 一致

# web：指定端口（默认 8080）
./build/user/app/go2cpp_web_server_simple 28080
curl -s http://127.0.0.1:28080/health                       # {"status":"ok"}
curl -s 'http://127.0.0.1:28080/api/echo?name=Go2Cpp&times=3'
curl -s -o /dev/null -w '%{http_code}\n' 'http://127.0.0.1:28080/api/echo?times=9'  # 400
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:28080/nope                # 404
curl -s -X POST -o /dev/null -w '%{http_code}\n' http://127.0.0.1:28080/            # 405
pkill -TERM -f go2cpp_web_server_simple                     # 优雅关闭，退出码 0

# chat：用内置客户端互测
./build/user/app/go2cpp_chat_server_simple --client

# p2p：一条脚本起三个进程
bash user/app/p2p_demo/run_demo.sh
```

实测（WSL，2026-11-02）：`cache_demo` 三张命中率表正常；`stream_demo` 输出
`STREAM_OK`、SHA-256 一致（1 MiB / 8 块 / 58.8 MiB/s）；`web_server_simple`
的 200/400/404/405 与 SIGTERM 优雅关闭全部符合预期。

## 三个新示例一览

| 示例 | 演示能力 | 运行方式 | 成功标志 |
| --- | --- | --- | --- |
| `p2p_demo` | STUN 地址发现、候选交换、UDP 打洞、keepalive/统计 | `bash user/app/p2p_demo/run_demo.sh` | `P2P_OK role=a/b`、`P2P 演示成功` |
| `cache_demo` | 7 策略 × 3 场景命中率/字节命中率对比 | 直接运行 | 三张表 + 平均排名，退出码 0 |
| `stream_demo` | 流表 + 资源分块传输 + KV 流式/单帧模式 | 直接运行 | `STREAM_OK`、输出文件 SHA-256 一致 |

`p2p_demo` 起三个进程：`stun`（STUN echo）+ `peer a`（绑 127.0.0.1）+ `peer b`
（绑 127.0.0.2）；两个 peer 关闭 host 候选、只交换 STUN 反射地址（srflx），
用文件邮箱做信令，打洞由 `P2PChannel::Connect()` 内部的 STUN Binding 检查完成。

`cache_demo` 用固定 seed 生成三组逐请求相同的负载（hotspot / scan / weighted），
7 种策略（LRU、LRU-2、2Q、W-TinyLFU、GDSF、ARC、CLOCK）在同一输入上比较对象
命中率与字节命中率。

## simple 与 full 对照

| 能力 | web_simple | web_full | chat_simple | chat_full |
| --- | --- | --- | --- | --- |
| 路由 | 方法 + 精确路径 | `Router`（`:param`） | 命令前缀 | 命令前缀 |
| 中间件 | — | Recover/RequestId/AccessLog/CORS | — | — |
| 静态文件 | — | 防目录穿越 + MIME + 大小上限 | — | — |
| SSE | — | SseHub（定时 + Channel 背压 + 心跳） | — | — |
| JSON API | `/api/echo` | echo/items/stats/routes/time/broadcast | — | — |
| 配置 | 命令行端口 | INI + 命令行覆盖 | 命令行 | INI + 命令行覆盖 |
| 日志 | stdout | 分级 + 文件 + 请求 ID/耗时 | stdout | 分级 + 文件 |
| 优雅关闭 | 信号 → Channel → `Stop` | SseHub → HttpServer → Scheduler | 信号 → `Stop` | 心跳 → LinkServer → Scheduler |
| 会话/房间 | — | — | 昵称 + 单房间广播 | 昵称/房间/私聊/在线列表/重连 token |
| 持久化 | — | — | — | 文本追加 + 加入时回放 |

## 源码位置

| 示例 | 依赖的框架源码 |
| --- | --- |
| web_server_* | `include/go2cpp/bento/net/http_server.hpp` + `src/bento/net/` |
| chat_server_* | `include/go2cpp/bento/net/{link,link_server,peer_hub}.hpp` + `src/bento/net/` |
| p2p_demo | `include/go2cpp/p2p/` + `src/p2p/` |
| cache_demo | `include/go2cpp/cache/cache.hpp` + `cache/{lru,lru_k,two_queue,tiny_lfu,gdsf,arc,clock}.hpp`（header-only，7 策略） |
| stream_demo | `include/go2cpp/app/{connection,stream_table,resource,kv,transport}.hpp` + `src/app/` |

各示例目录下还有一份独立 README（功能 / 构建 / 运行 / 逐条测试命令 / 实测输出）。

## 注意事项

- **所有命令以仓库根目录为工作目录**：静态文件与配置都是相对路径。
- **服务型示例不注册 ctest**：它们需要端口和信号，放进 ctest 会挂住 CI；
  要在 CI 里跑请自行加超时与后台进程管理。
- **端口冲突**：`web_server_simple` 默认 8080，`web_server_full` 8081，
  chat 用 UDP 9100/9200。被占用时传参或改配置。
- **`stream_demo` 默认不构建**：需要 `-DGO2CPP_WITH_APP=ON`，否则 target 不存在
  （不是构建失败）。
- **IM 模块不在本工作树**：`include/go2cpp/im/` 在早期规划里存在，当前 checkout
  没有该目录，因此没有 `im_demo`；需要 IM 能力时参考 `chat_server_full` 的
  会话/房间模式自行扩展。
