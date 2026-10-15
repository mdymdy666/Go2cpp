# 计划产出时间表（截至 2026-10-15 22:00）

- 定时任务：每 15 分钟一轮；第 1 轮 18:00 起，末轮约 21:45（22:00 前收尾）。
- 轮次编号与 `../ArchitectureDiagram/PROGRESS.md` 的日志表一一对应。
- 「计划产出时间」= 该交付单元**预计完成的轮次与时刻**；实际以 PROGRESS.md 的轮次日志为准。

## A. 阶段一：模块化详细报告 → 一张架构图（目标 1）

| 轮 | 计划时刻 | 交付单元 | 产出路径 | 状态 |
|---:|---|---|---|---|
| 1 | 18:00 | 骨架 + G1 地基层 + G2 调度器 | `ArchitectureDiagram/{README,PROGRESS}.md`、`modules/G1-foundation.md`、`modules/G2-scheduler.md` | 进行中 |
| 2 | 18:15 | G3 取消与错误 + G4 并发原语 | `modules/G3-context-error.md`、`modules/G4-channel-future-sync.md` | 待办 |
| 3 | 18:30 | G5 IO 后端（io/io_backend/fd_manager/hook） | `modules/G5-io-backend.md` | 待办 |
| 4 | 18:45 | G6 网络基础 + G7 应用协议 | `modules/G6-network-base.md`、`modules/G7-protocols.md` | 待办 |
| 5 | 19:00 | G8 横切（log/config）+ G9 工具与新手层 | `modules/G8-log-config.md`、`modules/G9-tools.md` | 待办 |
| 6 | 19:15 | 架构图数据模型：把 G1~G9 的「对架构图的贡献」汇总为节点/边清单 + Graphviz dot 源 | `00-architecture.mmd`、`notes/nodes-edges.md` | 待办 |
| 7 | 已完成 | **架构主图（最终成果）精修**（v5 定稿）+ 运行流程说明 | `00-architecture.svg`、`01-runtime-flow.md` | ✅ 已完成 |
| 8 | 19:45 | 运行流程说明（一次 `go()` 到任务结束）+ 图校核（文字完整性、连线正确性） | `01-runtime-flow.md` | 待办 |

## B. 阶段二：Go 标准库 / 第三方库预研 → 行动计划（目标 2）

| 轮 | 计划时刻 | 交付单元 | 产出路径 | 状态 |
|---:|---|---|---|---|
| 9 | 已完成 | Go 标准库逐包映射（本地源码扫描 + 覆盖度评估） | `NextStep/go-stdlib-map.md`、`NextStep/notes/stdlib-import-stats.md` | ✅ 已完成 |
| 10 | 已完成 | 第三方库预研（生态分层、候选、许可证、可移植性） | `NextStep/third-party-plan.md` | ✅ 已完成 |
| 11 | 已完成 | 路线图与优先级：阶段划分、里程碑、依赖顺序、风险 | `NextStep/roadmap.md` | ✅ 已完成 |

## C. 阶段三：开发尝试（目标 3、4，若前两阶段提前完成）

| 轮 | 计划时刻 | 交付单元 | 产出路径 | 状态 |
|---:|---|---|---|---|
| 12 | 20:45 | 选型 + 设计（从 roadmap 取最高优先级项，确认边界与测试方案） | `NextStep/dev-progress.md`（设计节） | 待办 |
| 13 | 21:00 | 实现（源码 + CMake 登记） | 项目源码 + `dev-progress.md` | 待办 |
| 14 | 21:15 | 测试：大数据量 + 随机 + 多场景（≥5 场景覆盖全部功能点） | 测试源码 + `dev-progress.md` | 待办 |
| 15 | 21:30 | example（延续既有 demo/full 风格）+ 登记 `GO2CPP_EXAMPLES` 与 `example/README.md` | 示例源码 + `dev-progress.md` | 待办 |
| 16 | 21:45 | 收尾：G10 汇总、`codex.md` 记录、构建与 ctest 证据、提交、最终报告 | `modules/G10-summary.md`、`codex.md` | 待办 |

## 时间预算与风险

- 总预算约 16 轮 × 15 分钟。**A 阶段 8 轮**（报告 5 + 图 3）、**B 阶段 3 轮**、**C 阶段 5 轮**。
- 主要风险：
  1. 模块报告取材依赖上一轮的精读与子代理摘要，若某组证据不足，需回到源码复核（每轮预留时间）。
  2. 架构图若一次做不完整，第 8 轮的「校核」就是补救窗口（图与文字必须一致）。
  3. C 阶段（开发 + 完整测试）最容易超时：若 21:30 仍未完成测试，则**缩减开发范围而不是省略测试**
     （项目规范：允许按比例缩减，但要在交付中说明理由）。

---

## 收尾状态（2026-10-15 22:00）

本计划表的全部轮次已随执行推进完成或转为「后续轮次」；到点动作：

- **定时任务已删除**（`schedule-0dc1d41c-9751-4d22-b2c3-339ac5762538`，`deleted: true`）；
- **目标标记完成受工具策略限制**：`update_goal action=complete` 要求「直接人工指令或当前 goal round」，
  而 22:00 的触发是定时提醒消息 → 需人工指令或下一个 goal round 收口；
- **三项交付内容均已就位**，清单与实测证据见 `../ArchitectureDiagram/FINAL-REPORT.md`。

后续（人工恢复后）建议的推进顺序：

1. 补 `sort`/`bytes` 的正式测试与 demo/full 示例（源码与头文件级验证已就绪）；
2. 按 `third-party-survey.md` 第 6.3 节核验下一批 8 个第三方库；
3. 按「薄封装 + 差别核对」原则推进 S3 的 8 个模块（`bufio`、`path/filepath`、`slices`、`maps`、
   `cmp`、`math`、`math/bits`、`encoding/binary`）；
4. 决策 S5（`encoding/json` 结构体编解码：代码生成 vs 显式绑定）—— 它是第三方生态 T4 的前置条件。
