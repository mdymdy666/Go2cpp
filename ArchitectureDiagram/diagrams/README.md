# 模块一图（diagrams）—— 本目录是**交付主体**

> 使用者要求的是「**每个模块一张图**」，不是每模块一篇讲解文档。
> 旧的 `../modules/G1..G10-*.md`（约 178KB 纯文字）**保留**作为图的**注解来源**，但不是交付形态。

## 一、每张图固定包含四类内容

| 章节 | 内容 | 图型 |
|---|---|---|
| ① 类架构 | 每个类**用了底层哪些能力**、**主要职责**是什么 | `classDiagram` |
| ② 重要函数 | 关键函数的**功能**与**具体实现要点**（含真实现分支） | `flowchart` |
| ③ 向上层提供 | 本模块**最终向上层提供了什么功能**，以及谁在用 | `graph LR` |
| ④ 状态流转 | 关键**状态机**与**执行流程**（如 Fiber 在 Scheduler 中如何流转与调度） | `stateDiagram-v2` ×2~3 |
| 附 | 与 Go 的差异表 + 必要的 ASCII 时序/栈变化图 | — |

## 二、11 个图文件

| 组 | 主题 | 文件 | 行数 | Mermaid |
|---|---|---|---:|---:|
| **G0** | **总览：10 组如何拼成一次 `go()`（建议先看）** | [`G0-overview.md`](G0-overview.md) | 163 | 4 |
| G1 | 地基层（Fiber / Timer / ParkingCondition） | [`G1-foundation.md`](G1-foundation.md) | 308 | 5 |
| G2 | **GMP 调度**（Task/Scheduler/P/M/worker_loop/sysmon） | [`G2-scheduler.md`](G2-scheduler.md) | 325 | 5 |
| G3 | 取消与错误（Context / DoneSignal / Error 家族） | [`G3-context-error.md`](G3-context-error.md) | 218 | 5 |
| G4 | 并发原语（Channel / Future / sync / WaitSelector） | [`G4-concurrency.md`](G4-concurrency.md) | 247 | 5 |
| G5 | IO 后端（IOBackend / FdManager / hook） | [`G5-io-backend.md`](G5-io-backend.md) | 195 | 5 |
| G6 | 网络基础（Socket / Address / Framer） | [`G6-network-base.md`](G6-network-base.md) | 239 | 5 |
| G7 | 应用协议（HTTP / WebSocket / SSE） | [`G7-protocols.md`](G7-protocols.md) | 269 | 6 |
| G8 | 横切设施（log / config） | [`G8-log-config.md`](G8-log-config.md) | 246 | 5 |
| G9 | 工具与新手层（go/defer/select/HybridMutex/FiberLocal/thread_policy） | [`G9-tools.md`](G9-tools.md) | 249 | 5 |
| G10 | **调试模块**（本次新增的 go2cpp::debug） | [`G10-debug.md`](G10-debug.md) | 315 | 6 |
| | **合计** | | **2774** | **56** |

## 三、渲染验证状态

**11 个文件、56 张图，全部用 `@mermaid-js/mermaid-cli` 实机渲染为 SVG 成功**，
产物数与期望数完全一致。

过程中发现并修复了 **9 处**语法问题（静态检查全部漏掉），详见
[`../../DebugTooling/mermaid-render-verification.md`](../../DebugTooling/mermaid-render-verification.md)。
其中 6 处是**节点标签内嵌半角引号**（Mermaid 把内层引号当标签结束符）。

## 四、怎么渲染

编辑器直接看即可（GitHub / VS Code / JetBrains / Typora / Obsidian 都内置 Mermaid）。
命令行导出 SVG/PNG：

```sh
npx -y @mermaid-js/mermaid-cli -i G2-scheduler.md -o G2.svg
npx -y @mermaid-js/mermaid-cli -i G2-scheduler.md -o G2.png -b white
```

> 注意：对多图 Markdown，产物是 `<base>-1.svg` … `<base>-N.svg`，**不会**生成 `<base>.svg`。

## 五、维护约定（改图前请读）

1. **一模块一文件**，`G<组>-<主题>.md`，与 `../modules/` 的组号一致；
2. **固定四章节 + 固定图型**，便于横向对比；
3. **标签内不要用半角双引号**，统一用 `「」` —— 这是最容易踩的坑（我踩了 6 次）；
4. classDiagram 关系只用合法箭头：`<|--`、`*--`、`o--`、`-->`、`..>`、`..|>`、`<..`、`<|..`；
5. stateDiagram 标签里**避免 `::`**，改用 `.`；
6. 换行用 `<br/>`，**不要在 mermaid 块里写裸换行**（会被解析成新状态/新节点）；
7. 图里只写**从源码核实过**的内容；拿不到精确签名时只描述语义并注明；
8. 每张图都要与 Go 做差异对照 —— **这个项目最容易出错的就是按 Go 语义误用**；
9. **改完必须实机渲染一次**（静态检查会漏）。
