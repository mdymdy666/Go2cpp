# Go2Cpp 架构图与模块报告（ArchitectureDiagram）

> 本目录是「熟悉项目模块 → 产出模块化详细报告 → 汇总为一张含文字描述的架构/运行流程图」
> 这条任务链的交付位置。
> 生成方式：定时任务（每 15 分钟一轮，截至 2026-10-15 22:00）。
> 进度与轮次安排见 [PROGRESS.md](PROGRESS.md)。

## 交付物

| # | 交付物 | 路径 | 说明 |
|---|---|---|---|
| 1 | **架构主图（最终成果）** | `00-architecture.svg` | 一张图承载整个运行流程/架构，**图内直接写有文字描述**；自包含 SVG，浏览器/图片查看器可直接打开 |
| 2 | 架构图源（可再生成/可编辑） | `00-architecture.mmd` | Mermaid 源，便于后续修改与二次渲染 |
| 3 | 运行流程说明 | `01-runtime-flow.md` | 图内文字对应的完整叙述：一次 `go()` 到任务结束的逐段落 |
| 4 | 模块详细报告（10 组） | `modules/G1..G10-*.md` | 按依赖方向分组；每组含职责、符号清单、机制、不变量、坑点、与 Go 的差异、**对架构图的贡献** |
| 5 | 进度总表 | `PROGRESS.md` | 每轮更新：已完成/进行中/下一轮/证据 |

## 模块报告分组（按依赖方向，与 `docs/HANDOFF.md` §4.3 一致）

| 组 | 主题 | 覆盖模块 | 依赖位置 |
|---|---|---|---|
| G1 | 地基层 | `detail/context_backend`、`fiber`、`core/timer`、`core/parking_condition` | 最底层，只依赖 libc/pthread |
| G2 | GMP 调度 | `scheduler`（Task/M/P/worker_loop/sysmon/FiberBin） | 依赖 G1 |
| G3 | 取消与错误 | `context`、`error` | 依赖 G1+G2 |
| G4 | 并发原语 | `channel`、`future`、`sync` | 依赖 G1~G3 |
| G5 | IO 后端 | `io`、`io_backend`、`fd_manager`、`hook` | 依赖 G1~G4 |
| G6 | 网络基础 | `socket`、`address`、`bytearray`、`framing`、`endian`/`myendian`、`socket_stream` | 部分独立于调度器 |
| G7 | 应用协议 | `http`、`websocket`、`sse` | 依赖 G5+G6 |
| G8 | 横切设施 | `log`、`config` | 被几乎所有模块使用 |
| G9 | 工具与新手层 | `tools/control_flow`、`tools/go`、`tools/event`、`thread_policy`、`fiber_local`、`runtime`、`core/hybrid_mutex`、`core/status` | 跨层 |
| G10 | 汇总 | 架构图 + 运行流程 + 全项目结论 | 收口 |

## 约定

- 所有结论都以 **代码与实测** 为依据；引用处给符号名（行号会漂移）。
- 每份模块报告的最后一节固定为「对架构图的贡献」：给出该模块在图中的**节点名、连线、以及图内文字**，供 G10 汇总时直接取用。
- 与交接文档不一致的地方，一律标注「本轮实测修正」并给出证据。
