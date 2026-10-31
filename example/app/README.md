# 可运行的应用示例（simple + full + 新能力）

`example/app/` 提供 **7 个可编译、可实际运行**的示例：4 个服务型示例
（web_server / chat_server 各有 simple 与 full 两版）+ 3 个新能力示例
（P2P 打洞、缓存策略、应用层流表）。

它们与 `example/` 下其他示例的区别是：**这些是服务或完整闭环**，会监听
端口、等待信号或驱动多进程，因此不注册 ctest，由各示例 README 里的
curl / 客户端 / 脚本命令驱动。

## 目录与规模

| 示例 | 版本 | 文件 | 总行数（含注释） | 逻辑行（去注释/空行） | 用到的核心组件 |
| --- | --- | --- | --- | --- | --- |
| web_server | simple | `web_server_simple/main.cpp` | 201 | 159 | `HttpServer`、`Scheduler`、`Channel` |
| web_server | full | `web_server_full/`（9 个模块） | 1739 | 1267 | `HttpServer`、`Scheduler`、`Channel`、`sse::Stream` |
| chat_server | simple | `chat_server_simple/main.cpp` | 310 | 260 | `LinkServer`、`PeerHub` |
| chat_server | full | `chat_server_full/`（8 个模块） | 1791 | 1383 | `LinkServer`、`PeerHub`、`Scheduler`、`Channel` |
| p2p_demo | simple | `p2p_demo/main.cpp` + `run_demo.sh` | 492 | 409 | `p2p::P2PChannel`、`p2p::StunServer` |
| cache_demo | simple | `cache_demo/main.cpp` | 300 | 240 | `cache::Workload`、`cache::MakeCache`（7 策略） |
| stream_demo | simple | `stream_demo/main.cpp` | 437 | 368 | `app::Connection`、`StreamTable`、`ResourceProducer`、`KvProducer` |

> 关于 full 版行数：任务给的参考规模是 400–600 行，但 full 版按职责拆成
> 配置/日志/协议/会话/存储/服务端/客户端多个模块，并按项目规范保留了文件头
> 与公开接口注释，因此总行数超出参考区间。**功能没有用“省略号”占位**，
> 每个模块都可编译、可运行；如果只要最小实现，看 simple 版。
>
> 三个新示例都带完整的错误处理与逐行注释，因此单文件在 300–440 行；
> 它们各自演示的是**一个新子系统**，不是对 web/chat 的简化。

## 构建

```sh
# 仓库根目录；构建目录沿用项目已有的 .build-bento-wsl
cmake -S . -B .build-bento-wsl -DGO2CPP_BUILD_EXAMPLES=ON \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_APP=ON
cmake --build .build-bento-wsl --target \
  go2cpp_web_server_simple go2cpp_web_server_full \
  go2cpp_chat_server_simple go2cpp_chat_server_full \
  go2cpp_p2p_demo go2cpp_cache_demo go2cpp_stream_demo -j8
```

各 target 的注册条件：

| target | 需要的开关 / 目标 | 关闭时的行为 |
| --- | --- | --- |
| `go2cpp_web_server_*` | `GO2CPP_WITH_BENTO=ON` → `go2cpp_bento_http` | 打印 `example/app: web_server 示例跳过` |
| `go2cpp_chat_server_*` | `GO2CPP_WITH_BENTO=ON` → `go2cpp_bento` | 打印 `example/app: chat_server 示例跳过` |
| `go2cpp_p2p_demo` | `GO2CPP_WITH_P2P=ON`（默认 ON） | 打印 `example/app: p2p_demo 跳过` |
| `go2cpp_cache_demo` | 仓库 `include/go2cpp/cache/`（header-only） | 头文件缺失时跳过 |
| `go2cpp_stream_demo` | `GO2CPP_WITH_APP=ON`（默认 OFF） | 不注册 target |

CMake 不会静默把示例编译成空壳：缺失依赖时打印明确原因。

## 运行与学习路径

1. **`web_server_simple`**：先看 `HttpServer` 的 `Listen → OnRequest → Start →
   Stop` 生命周期，再看 `Scheduler + Channel` 如何把信号处理与关闭动作解耦。
2. **`web_server_full`**：在 simple 之上补齐路由参数、中间件洋葱链、静态文件
   安全校验、SSE 广播（每个订阅者一个 Channel 做背压）、INI 配置、分级日志、
   异常恢复和健康检查。
3. **`chat_server_simple`**：先理解 `LinkServer` 的 peer 回调与 `Broadcast`，
   再看同仓的 `PeerHub` 客户端如何与它互通（UDP 可靠通道）。
4. **`chat_server_full`**：看会话表、房间、私聊、在线列表、应用层心跳、
   重连 token、消息持久化如何拆成独立模块并共享状态。
5. **`p2p_demo`**：STUN echo + 两个独立进程，看 NAT 后主机收集 srflx 候选、
   交换候选、UDP 打洞、收发与统计的完整流程。
6. **`cache_demo`**：7 种淘汰策略在热点/扫描/权重三组负载上的命中率对比，
   理解 recency、频率准入、抗扫描与大小感知的取舍。
7. **`stream_demo`**：两个 `Connection` 背靠背，看流表如何用 3 条流同时传
   一个文件与两张 KV 表，以及两级窗口、WINDOW_UPDATE、SHA-256 校验。

每个示例目录下都有独立 README（功能、构建、运行、逐条测试命令与实测输出）。

## 三个新示例一览

| 示例 | 演示能力 | 运行方式 | 成功标志 |
| --- | --- | --- | --- |
| `p2p_demo` | STUN 地址发现、候选交换、UDP 打洞、keepalive/统计 | `bash example/app/p2p_demo/run_demo.sh`（或三个终端） | `P2P_OK role=a/b`、`P2P 演示成功` |
| `cache_demo` | 7 策略 × 3 场景命中率/字节命中率对比 | `./.build-bento-wsl/example/app/go2cpp_cache_demo` | 三张表 + 平均排名，退出码 0 |
| `stream_demo` | 流表 + 资源分块传输 + KV 流式/单帧模式 | `./.build-bento-wsl/example/app/go2cpp_stream_demo` | `STREAM_OK`、输出文件 SHA-256 一致 |

### p2p_demo

三个进程：`stun`（STUN echo）+ `peer a`（绑 127.0.0.1）+ `peer b`（绑
127.0.0.2）。两个 peer 关闭 host 候选、只交换 STUN 反射地址（srflx），
模拟两台 NAT 后的主机；信令用文件邮箱（候选写入 `--signal-dir`，轮询读
对端文件）。打洞由 `P2PChannel::Connect()` 内部的 STUN Binding 检查完成。
详见 `p2p_demo/README.md`。

### cache_demo

`Workload` 用固定 seed 生成三组逐请求相同的负载（hotspot / scan /
weighted），7 种策略（LRU、LRU-2、2Q、W-TinyLFU、GDSF、ARC、CLOCK）在同一
输入上比较；同时打印对象命中率与按 weight 加权的字节命中率。详见
`cache_demo/README.md`。

### stream_demo

`MemoryTransport` 连接两个 `Connection`：客户端发一条资源流（文件分块 +
CRC/SHA-256）和两条 KV 流（大表走 HEADERS+DATA，小表单帧），服务端落盘并
校验，最后示例自己比对输出文件的 SHA-256。详见 `stream_demo/README.md`。

## simple 与 full 对照

| 能力 | web_server_simple | web_server_full | chat_server_simple | chat_server_full |
| --- | --- | --- | --- | --- |
| 路由 | 方法 + 精确路径 if/else | Router 类，支持 `:param` | 命令前缀 | 命令前缀 |
| 中间件 | 无 | Recover / RequestId / AccessLog / CORS | 无 | 无（协议层直接处理） |
| 静态文件 | 无 | 防目录穿越 + MIME + 大小上限 | 无 | 无 |
| SSE | 无 | SseHub：Fiber 定时 + Channel 背压 + 心跳 | 无 | 无 |
| JSON API | `/api/echo` | echo/items/stats/routes/time/broadcast | 无 | 无 |
| 错误处理 | 404/405 文本 | 统一 JSON + 异常恢复 500 | 控制台提示 | `ERROR` 文本协议 |
| 配置 | 命令行端口 | INI + 命令行覆盖 | 命令行 | INI + 命令行覆盖 |
| 日志 | stdout | 分级 + 文件 + 请求 ID/耗时 | stdout | 分级 + 文件 |
| 优雅关闭 | 信号 → Channel → `Stop` | SseHub → HttpServer → Scheduler 逆序 | 信号 → `Stop` | 心跳 Fiber → LinkServer → Scheduler |
| 会话/房间 | — | — | 昵称 + 单房间广播 | 昵称/房间/私聊/在线列表/心跳/重连 token |
| 消息持久化 | — | — | 无 | 文本文件追加 + 加入时回放 |
| 多客户端 | — | 4 worker 并发 | 已验证 2 客户端广播 | 已验证峰值 8 在线、并发、token 重连 |

## 全部示例的能力对照

| 示例 | 依赖 target | 监听/传输 | 核心能力 | 验证入口 |
| --- | --- | --- | --- | --- |
| `web_server_simple` | `go2cpp_bento_http` | TCP 8080 | HttpServer 生命周期、信号关闭 | curl（README 5 组命令） |
| `web_server_full` | `go2cpp_bento_http` | TCP 8081 | 路由/中间件/静态/SSE/日志/健康检查 | curl + SSE |
| `chat_server_simple` | `go2cpp_bento` | UDP 9100 | LinkServer 广播、PeerHub 客户端 | 内置 `--client` |
| `chat_server_full` | `go2cpp_bento` | UDP 9200 | 房间/私聊/心跳/重连/持久化 | 内置 `--client` |
| `p2p_demo` | `go2cpp_p2p` | UDP（STUN 34780 + 随机端口） | 候选/打洞/keepalive/统计 | `run_demo.sh` |
| `cache_demo` | header-only | 无 | 7 策略命中率对比 | 直接运行 |
| `stream_demo` | `go2cpp_app` | 内存管道（无端口） | 流表/资源/KV/流控/校验 | 直接运行 |

> `include/go2cpp/im/` 的 IM 模块目前**暂停**（不参与本轮示例），因此没有
> 对应示例；恢复后可按 `chat_server_full` 的会话/房间模式补一个
> `im_demo`。

## 测试约定

- 所有测试命令都以**仓库根目录**为工作目录（静态文件路径与配置文件都是相对路径）。
- web 版用 `curl`，chat 版用同仓构建的 `--client` 模式，p2p 用自带
  `run_demo.sh`，cache/stream 直接运行即可，不需要额外工具。
- 每个示例 README 的「测试/预期输出」一节给出可复制的命令与期望输出。
- 这些服务不注册 ctest：需要端口和信号，放进 ctest 会挂住 CI。若要在 CI 里跑，
  自行加超时与后台进程管理。
- 本目录所有示例在 2026-10-31 的 WSL Release 构建下实测：4 个既有示例
  32 项断言全部通过；3 个新示例分别输出 `P2P_OK` / 排名表 / `STREAM_OK`。
