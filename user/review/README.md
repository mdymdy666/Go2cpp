# user/review —— 用户审查意见与改进建议

> 用途：**记录用户对代码的审查意见、想法、改进建议**（用户口述，我整理归档）。
> 不是我自己做的审查——我的审查在 `docs/reports/*-review.md`；这里是**用户的判断**。

## 记录格式（每个模块一个文件）

```
user/review/<模块>-review.md
```

每条意见一个编号条目：

```markdown
### R-<模块>-<序号>  <一句话标题>

- **用户原话**：<尽量保留原话，不要改写结论>
- **上下文**：<在哪个文件/函数/行为上看到的>
- **我的理解**：<我读代码后的确认，含 file:line>
- **改进建议**：<具体怎么做，可落地>
- **状态**：待评估 / 已确认 / 已修（commit）/ 已否决（理由）
```

## 现有审查文档（本目录）

| 文档 | 说明 |
|---|---|
| `user/review/审查顺序-并发与网络.md` | 从底到顶的文件级审查顺序（16+14 步） |
| `user/review/审查顺序-类级.md` | 类级审查顺序（18+18 步，每类带 file:line） |
| `user/review/专题-定时器perP改进.md` | TimerService 改 Go per-P 模式（⏸ 暂缓，设计保留） |
| `user/review/WaitNode统一-整改.md` | 三套 WaitNode 同构整改（待开工，丢唤醒专项防复发的第一优先级） |
| `user/review/专项-Fiber链级元信息.md` | Fiber 链级元信息集中化（当前实现拆散在 FiberRecord 上，中间节点引用会读到过时值） |

## 接上的历史文档

| 文档 | 说明 |
|---|---|
| `docs/reports/baseline-review-2026-11-04.md` | 11-04 基座大改审查（6 路，P0×1/P1×8/P2×8/P3×5） |
| `docs/reports/2026-11-01-asm-concurrency-review.md` | 11-01 并发汇编审查 |
| `docs/reports/2026-11-01-asm-network-review.md` | 11-01 网络汇编审查 |
| `docs/reports/2026-11-01-concurrency-opt-review.md` | 11-01 并发优化审查 |
| `docs/reports/2026-11-01-network-app-opt-review.md` | 11-01 网络/应用优化审查 |
| `docs/reports/2026-11-01-scheduler-opt-review.md` | 11-01 调度器优化审查 |
| `docs/reports/three-piece-audit.md` | 三件套覆盖盘点（44 模块） |
| `docs/reports/http-link-audit.md` | HTTP 链路 34 bug 全清单 |
| `docs/reports/rdnet-v3-review.md` | rd_net v3 审查 |
| `docs/plans/lost-wakeup-plan.md` | 丢唤醒专项（含 7.0 三套 WaitNode 同构整改） |

## 协作方式

1. 用户指定模块（如「看 storage 的 kv」）；
2. 我读代码，把用户的意见逐条按上面格式记进 `user/review/<模块>-review.md`；
3. 用户确认/补充后，我更新状态（待评估 → 已确认 → 已修）；
4. 每条意见落点写清 `file:line`，避免空泛。

## 本对话的边界（2026-11-05 用户明确）

- **不进行实际代码编写工作**：不派 agent 实施、不改源码、不跑构建；
- **不问「是否开工」**：所有专项/整改只记录设计与状态，等用户另行安排；
- 我的产出只有：**读代码 + 归档用户的意见与建议 + 必要时更新文档状态**。
