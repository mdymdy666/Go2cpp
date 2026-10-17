# Go2Cpp 架构图（ArchitectureDiagram）

> **交付主体已变更为 [`diagrams/`](diagrams/)** ——「**每个模块一张图**」。
> 旧的 `modules/G1..G10-*.md`（约 178KB 纯文字讲解）**保留**，作为图的**注解来源**，
> 但它们不是使用者要求的交付形态。

> **实机渲染验证**：全部 69 个 mermaid 块的渲染结果与宽高比见 [`render-verification.md`](render-verification.md)。结论：**69/69 渲染成功、0 语法错误、0 扁条、6 个长流程图（已记录未修）**。


## 一、该看什么

| 想看什么 | 去哪里 |
|---|---|
| **每个模块的架构图（主交付）** | [`diagrams/`](diagrams/) —— **12 个文件、69 个 Mermaid 图** |
| 一次性看整个运行流程 | [`00-architecture.svg`](00-architecture.svg)（总览图，含文字描述） |
| 一次 `go()` 到任务结束的逐段叙述 | [`01-runtime-flow.md`](01-runtime-flow.md) |
| 某个模块的**详细文字讲解**（职责/符号/机制/不变量/坑点/与 Go 差异） | `modules/G1..G10-*.md` |
| 节点与边的原始清单 | [`notes/nodes-edges.md`](notes/nodes-edges.md) |
| 本轮开发进度与证据 | [`../DebugTooling/DEV-PROGRESS.md`](../DebugTooling/DEV-PROGRESS.md) |

## 二、每张图固定包含四类内容

使用者明确要求「一图至少包含类架构 + 重要函数的功能与具体实现 + 向上层提供什么 + 关键函数/状态流转的流程图」，
因此 `diagrams/` 下每个文件都按同一结构组织：

| 章节 | 内容 | 图型 |
|---|---|---|
| ① 类架构 | 每个类**用了底层哪些能力**、**主要职责**是什么 | `classDiagram` |
| ② 重要函数 | 关键函数的**功能**与**具体实现要点**（含真实现分支） | `flowchart` |
| ③ 向上层提供 | 本模块**最终向上层提供了什么功能**，以及谁在用 | `graph LR` |
| ④ 状态流转 | 关键**状态机**与**执行流程**（如 Fiber 在 Scheduler 中如何流转与调度） | `stateDiagram-v2` × 2~3 |
| 附 | 与 Go 的差异表 + 必要的 ASCII 时序/栈变化图 | — |

## 三、10 个模块图

| 组 | 主题 | 文件 | 行数 | Mermaid |
|---|---|---|---:|---:|
| **G0** | **总览：一张图看懂（分层 + 依赖 + 生命周期）** | [`diagrams/G0-overview.md`](diagrams/G0-overview.md) | **112** | **1** |
| G1 | 地基层（Fiber / Timer / ParkingCondition） | [`diagrams/G1-foundation.md`](diagrams/G1-foundation.md) | 308 | 5 |
| G2 | **GMP 调度**（Task / Scheduler / P / M / worker_loop / sysmon） | [`diagrams/G2-scheduler.md`](diagrams/G2-scheduler.md) | 321 | 5 |
| G3 | 取消与错误（Context / DoneSignal / Error 家族） | [`diagrams/G3-context-error.md`](diagrams/G3-context-error.md) | 218 | 5 |
| G4 | 并发原语（Channel / Future / sync / WaitSelector） | [`diagrams/G4-concurrency.md`](diagrams/G4-concurrency.md) | 247 | 5 |
| G5 | IO 后端（IOBackend / FdManager / hook） | [`diagrams/G5-io-backend.md`](diagrams/G5-io-backend.md) | 195 | 5 |
| G6 | 网络基础（Socket / Address / Framer） | [`diagrams/G6-network-base.md`](diagrams/G6-network-base.md) | 239 | 5 |
| G7 | 应用协议（HTTP/1.1 / WebSocket / SSE） | [`diagrams/G7-protocols.md`](diagrams/G7-protocols.md) | 269 | 6 |
| G8 | 横切设施（log / config） | [`diagrams/G8-log-config.md`](diagrams/G8-log-config.md) | 264 | 7 |
| G9 | 工具与新手层（go / defer / select / HybridMutex / FiberLocal / thread_policy） | [`diagrams/G9-tools.md`](diagrams/G9-tools.md) | 290 | 7 |
| G10 | **调试模块**（go2cpp::debug） | [`diagrams/G10-debug.md`](diagrams/G10-debug.md) | 348 | 9 |
| **G11** | **HTTP/2 全栈**（HPACK / Huffman / 帧层 / 流状态机 / 连接层 / Transport / TLS） | [`diagrams/G11-http2.md`](diagrams/G11-http2.md) | **387** | **9** |
| | **合计 12 张图 + 2 个 README** | | **3198** | **69** |

> **G0 只有 1 张图**，这是刻意的：它的职责是让读者**一屏之内**建立
> 「有哪些层、谁能依赖谁、一次协程怎么穿过它们」的整体感。
> 时序、状态机、阻塞细节都在 G1~G11 的下钻图里。原来的 G0 有 4 张图，
> 「一图看懂」名不副实，已收拢为单图（渲染后 784×956，宽高比 0.82）。

## 四、怎么渲染

Mermaid 在以下环境**直接可见**，无需额外工具：

- GitHub / GitLab 的 Markdown 预览；
- VS Code（内置 Markdown 预览 + Mermaid 扩展）；
- JetBrains 系列（Markdown 插件支持）；
- Typora / Obsidian 等本地编辑器。

命令行渲染（可选）：

```sh
# 需要 Node 环境；把单个 md 里所有 mermaid 块导出为 svg
npx -y @mermaid-js/mermaid-cli -i diagrams/G2-scheduler.md -o G2-scheduler.svg
```

## 五、约定（后续维护者请注意）

1. **一模块一文件**，文件名 `G<组>-<主题>.md`，与 `modules/` 的组号一致；
2. **固定四章节 + 固定图型**，便于横向对比；
3. 图里只写**从源码核实过**的内容；拿不到精确签名时**只描述语义**，并注明"未逐一核对"；
4. **每张图都要与 Go 做差异对照** —— 这个项目最容易出错的地方就是「按 Go 语义误用」；
5. 状态机必须写清**触发条件**（谁在什么情况下推动状态转移），不能只画箭头。
