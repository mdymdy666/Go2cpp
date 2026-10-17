# 架构图渲染验证报告

> 本文件记录 `ArchitectureDiagram/diagrams/` 下**全部 69 个 mermaid 块**的实机渲染结果。
>
> **为什么需要它**：架构图的"能否渲染"和"是否可读（宽高比）"**无法靠读代码判断**，
> 必须实机渲染。此前多次交付都只有静态检查，这份报告补上了真实数据。

## 一、渲染环境

| 项 | 值 |
|---|---|
渲染器 | `@mermaid-js/mermaid-cli`（`npm install` 195 packages，52 秒）|
运行时 | Windows 侧 `D:\soft_wares\nodejs\node.exe` **v24.19.0** |
为什么不用 WSL | WSL 里 `node` 不在 PATH（只有 `/mnt/d/.../node.exe`），`npx` 是调 `node` 的 shell 脚本 → 失败。**Windows 侧环境完整** |

渲染命令：
```bash
node <mermaid-cli>/src/cli.js -i block.mmd -o block.png -q
```

## 二、总览

| 范围 | 块数 | 渲染成功 | 失败 | 语法问题 |
|---|---:|---:|---:|---:|
G0-overview.md | 1 | 1 | 0 | 0 |
**G1~G10** | **59** | **59** | **0** | **0** |
G11-http2.md | 9 | 9 | 0 | 0 |
| **合计** | **69** | **69** | **0** | **0** |

**结论：全部 69 块都能渲染，零语法错误。**

> 注：此前有记录称 G1~G10 是 63 块，**实际是 59 块**。
> `G0(1) + G1~G10(59) + G11(9) = 69`，与 README 声称的 69 一致。

## 三、宽高比判定

判定标准：
- **扁条**：ratio > 2.5（横向铺太开，字号被迫缩小）
- **竖条**：ratio < 0.3（纵向太长，需要滚动很久）

| 类别 | 数量 | 分布 |
|---|---:|---|
**扁条** | **0** | — |
**竖条** | **6** | 见下表 |
正常（0.3 ≤ ratio ≤ 2.5）| 53 | — |

### 6 个竖条（全部是各文件的第 2 块「重要函数流程图」）

| 块 | 尺寸 | ratio |
|---|---|---:|
`G4-concurrency-02` | 541 × **4228** | **0.13** |
`G9-tools-04` | 428 × **3265** | **0.13** |
`G6-network-base-02` | 519 × **3247** | **0.16** |
`G1-foundation-02` | 691 × 3496 | 0.20 |
`G2-scheduler-02` | 726 × 3713 | 0.20 |
`G5-io-backend-02` | 534 × 2174 | 0.25 |

## 四、这 6 个竖条的**真实原因**（读完内容后才搞清的）

### 4.1 它们不是线性流程，而是 **4 个 subgraph 纵向堆叠**

| 图 | 4 个 subgraph 的主题 |
|---|---|
`G4-concurrency-02` | `Mutex::Lock/Unlock` · `Channel send/recv` · `select` · `Future/Promise` |
`G9-tools-04` | `go(fn)` · `defer` · `EventBatch::Wait` · `HybridGate::lock` |
`G1-foundation-02` | `Fiber::AcquireForScheduler` · `resume_from_scheduler` · `Suspend` · `ParkingCondition::wait` |
`G2-scheduler-02` / `G5-io-backend-02` / `G6-network-base-02` | 同构：4 个独立主题各占一个 subgraph |

**也就是说：每个"竖条"其实是 4 张独立流程图，被纵向拼在了一个 mermaid 块里。**
一个 subgraph 约 700~1000 像素高，4 个叠起来就是 3000~4200。

### 4.2 方向调整无效（已实测）

```
原样 flowchart TB:  541×4228  ratio=0.13   ← 竖条
改   flowchart LR:  784×58    ratio=13.52  ← 扁条，更糟
```

因为 LR 会把 **subgraph 内部**也变成横向，每个 subgraph 内部仍有多层分支，
横过来就成了超宽条。

### 4.3 为什么**不建议**拆成 4 张图

拆开确实能让每张图 ratio 落到 [0.3, 2.5]，但：

1. **违反使用者的交付约定**：原文要求是「**每个模块一张图**」。拆成 4 张，
   `diagrams/` 会从 12 个文件变成 12 个文件但块数从 69 涨到 87，
   "一个模块一张图"的形态被破坏；
2. **丢掉对比价值**：这 4 个主题放在一起的意义正是"4 个并发原语的实现路径并排对照"，
   拆开后读者要来回跳；
3. 这 6 个块的**内容都是正确的真实分支**，没有冗余。

**所以结论是：这是「一模块一图」这个要求本身的代价，不是缺陷。**
如果要真的改进，应该改变的是**需求**（允许每模块多图），而不是硬拆。

### 4.4 如果将来允许每模块多图

建议做法：
1. 每个 subgraph 独立成一个 mermaid 块，标题写成 `<模块> · <子主题>`
2. 在模块图里保留一张"索引图"（只有 4 个 subgraph 名 + 一句话），指向 4 张细节图
3. 拆完必须重跑渲染验证每块 ratio 落在 [0.3, 2.5]

## 五、按类型的分布

| 类型 | 数量 | 渲染成功 | 备注 |
|---|---:|---:|---|
`classDiagram` | 17 | 17 | 类图，宽的居多（G9-01 ratio=2.00、G10-02 ratio=2.09，接近上限但未超）|
`flowchart` | 10 | 10 | **6 个竖条全部来自这里**；其余 4 个正常 |
`graph` | 10 | 10 | 全部正常（0.55~1.41）|
`stateDiagram-v2` | 22 | 22 | 全部正常（0.41~2.00）|

**规律**：`stateDiagram-v2` 和 `graph` 都很稳；`flowchart` 因为画的是"函数实现流程"，
天然容易变长。

## 六、如何复现

```bash
# Windows 侧
cd E:\CodexWorkspace\Go2Cpp\.dsh_tmp\mermaid
node node_modules/@mermaid-js/mermaid-cli/src/cli.js \
     -i <block>.mmd -o <block>.png -q
```

块提取：用正则 ` ```mermaid\s*\n(.*?)``` ` 从各 md 文件里抽。

渲染产物（png/mmd）**不提交进仓库**，只在本地验证用。
