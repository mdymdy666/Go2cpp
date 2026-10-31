# 可运行的应用示例（simple + full）

`example/app/` 提供两类应用、每类两个版本，全部可以编译并实际运行。它们与
`example/` 下其他示例的区别是：**这些是服务**，会监听端口、等待信号，因此
不注册 ctest，由本文档里的 curl / 客户端命令驱动。

## 目录与规模

| 示例 | 版本 | 文件 | 总行数（含注释） | 逻辑行（去注释/空行） | 用到的核心组件 |
| --- | --- | --- | --- | --- | --- |
| web_server | simple | `web_server_simple/main.cpp` | 201 | 159 | `HttpServer`、`Scheduler`、`Channel` |
| web_server | full | `web_server_full/`（9 个模块） | 1739 | 1267 | `HttpServer`、`Scheduler`、`Channel`、`sse::Stream` |
| chat_server | simple | `chat_server_simple/main.cpp` | 310 | 260 | `LinkServer`、`PeerHub` |
| chat_server | full | `chat_server_full/`（8 个模块） | 1791 | 1383 | `LinkServer`、`PeerHub`、`Scheduler`、`Channel` |

> 关于 full 版行数：任务给的参考规模是 400–600 行，但 full 版按职责拆成
> 配置/日志/协议/会话/存储/服务端/客户端多个模块，并按项目规范保留了文件头
> 与公开接口注释，因此总行数超出参考区间。**功能没有用“省略号”占位**，
> 每个模块都可编译、可运行；如果只要最小实现，看 simple 版。

## 构建

```sh
# 仓库根目录；构建目录沿用项目已有的 .build-bento-wsl
cmake -S . -B .build-bento-wsl -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build .build-bento-wsl --target \
  go2cpp_web_server_simple go2cpp_web_server_full \
  go2cpp_chat_server_simple go2cpp_chat_server_full -j8
```

四个 target 只有在 `GO2CPP_WITH_BENTO=ON` 且 `go2cpp_network` 可用时才注册：

- `go2cpp_bento_http` → `bento::HttpServer`（web 两版）；
- `go2cpp_bento` → `bento::LinkServer` / `bento::PeerHub`（chat 两版）。

Bento 关闭时 CMake 打印 `example/app: ... 跳过`，不会静默生成空壳 target。

## 学习路径

1. **`web_server_simple`**：先看 `HttpServer` 的 `Listen → OnRequest → Start → Stop`
   生命周期，再看 `Scheduler + Channel` 如何把信号处理与关闭动作解耦。
2. **`web_server_full`**：在 simple 之上补齐路由参数、中间件洋葱链、静态文件
   安全校验、SSE 广播（每个订阅者一个 Channel 做背压）、INI 配置、分级日志、
   异常恢复和健康检查。
3. **`chat_server_simple`**：先理解 `LinkServer` 的 peer 回调与 `Broadcast`，
   再看同仓的 `PeerHub` 客户端如何与它互通（UDP 可靠通道）。
4. **`chat_server_full`**：看会话表、房间、私聊、在线列表、应用层心跳、
   重连 token、消息持久化如何拆成独立模块并共享状态。

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

## 测试约定

- 所有测试命令都以**仓库根目录**为工作目录（静态文件路径与配置文件都是相对路径）。
- web 版用 `curl`，chat 版用同仓构建的 `--client` 模式，不需要额外工具。
- 每个示例 README 的「测试」一节给出可复制的命令与期望输出。
- 这些服务不注册 ctest：需要端口和信号，放进 ctest 会挂住 CI。若要在 CI 里跑，
  自行加超时与后台进程管理。
