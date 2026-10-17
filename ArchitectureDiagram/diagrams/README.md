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

## 二、11 个图文件（共 63 张 Mermaid）

| 组 | 主题 | 文件 | 行数 | Mermaid |
|---|---|---|---:|---:|
| G0 | 总览：10 组如何拼成一次 `go()` | [`G0-overview.md`](G0-overview.md) | 163 | 4 |
| G1 | 地基层（Fiber / Timer / ParkingCondition） | [`G1-foundation.md`](G1-foundation.md) | 308 | 5 |
| G2 | **GMP 调度**（Task/Scheduler/P/M/worker_loop/sysmon） | [`G2-scheduler.md`](G2-scheduler.md) | 321 | 5 |
| G3 | 取消与错误 | [`G3-context-error.md`](G3-context-error.md) | 218 | 5 |
| G4 | 并发原语（Channel / Future / sync / WaitSelector） | [`G4-concurrency.md`](G4-concurrency.md) | 247 | 5 |
| G5 | IO 后端 | [`G5-io-backend.md`](G5-io-backend.md) | 195 | 5 |
| G6 | 网络基础 | [`G6-network-base.md`](G6-network-base.md) | 239 | 5 |
| G7 | 应用协议（HTTP / WebSocket / SSE） | [`G7-protocols.md`](G7-protocols.md) | 269 | 6 |
| G8 | 横切设施（log / config，类图拆 3 张） | [`G8-log-config.md`](G8-log-config.md) | 264 | 7 |
| G9 | 工具与新手层（类图拆 3 张） | [`G9-tools.md`](G9-tools.md) | 290 | 7 |
| G10 | **调试模块**（类图拆 4 张） | [`G10-debug.md`](G10-debug.md) | 348 | 9 |
| | **合计 11 张图 + 1 个 README** | | **2862** | **63** |

> G8/G9/G10 的**类图各拆成 3~4 张**：类数超过约 12 个时，Mermaid 会把它们横向铺开，
> 渲染成宽高比 > 2.5 的"扁条"，文字小到不可读（实测 G8 为 3.10、G9 为 3.41、G10 为 3.10）。
> 拆分后全部图宽高比落在 **0.32~2.09**。

## 三、渲染与可读性验证状态

**11 个文件、63 张图，全部用 `@mermaid-js/mermaid-cli` 实机渲染为 SVG/PNG 成功**，
产物数与期望数完全一致；并用 **PNG 宽高比**自动检测可读性。

验证过程中发现并修复了 **14 处**问题（**前 8 处全部被静态检查漏掉**）：

| 类别 | 数量 | 说明 |
|---|---:|---|
| 节点标签内嵌半角引号 | 6 | Mermaid 把内层引号当标签结束符 |
| 非法关系箭头 `<-..` | 1 | classDiagram 不支持，应为 `..>` |
| stateDiagram 标签里的 `::` | 1 | 会破坏解析 |
| mermaid 块内裸换行 | 4 | Python 的 `\n` 变真换行，被当成新状态 |
| 类图过宽（扁条） | 3 | G8 / G9 / G10 各拆图解决 |

详见 [`../../DebugTooling/mermaid-render-verification.md`](../../DebugTooling/mermaid-render-verification.md)。

## 四、怎么渲染

编辑器直接看即可（GitHub / VS Code / JetBrains / Typora / Obsidian 都内置 Mermaid）。
命令行导出：

```sh
npx -y @mermaid-js/mermaid-cli -i G2-scheduler.md -o G2.svg
npx -y @mermaid-js/mermaid-cli -i G2-scheduler.md -o G2.png -b white
```

> 注意：对多图 Markdown，产物是 `<base>-1.svg` … `<base>-N.svg`，**不会**生成 `<base>.svg`。

## 五、维护约定（改图前请读）

1. **一模块一文件**，`G<组>-<主题>.md`，与 `../modules/` 的组号一致；
2. **固定四章节 + 固定图型**，便于横向对比；
3. **标签内不要用半角双引号**，统一用 `「」` —— 最容易踩的坑（踩了 6 次）；
4. classDiagram 关系只用合法箭头：`<|--`、`*--`、`o--`、`-->`、`..>`、`..|>`、`<..`、`<|..`；
5. stateDiagram 标签里**避免 `::`**，改用 `.`；
6. 换行用 `<br/>`，**不要在 mermaid 块里写裸换行**（会被解析成新状态/新节点）；
7. **单张 classDiagram 的类数控制在 12 个以内**，超过就拆（否则会被横向铺开成"扁条"）；
8. 图里只写**从源码核实过**的内容；拿不到精确签名时只描述语义并注明；
9. 每张图都要与 Go 做差异对照 —— **这个项目最容易出错的就是按 Go 语义误用**；
10. **改完必须实机渲染一次**（静态检查会漏），并检查宽高比（> 2.5 视为扁条）。
