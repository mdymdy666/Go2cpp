# DebugTooling 进度表

> 每轮更新：已完成 / 进行中 / 下一轮任务 / 证据。时间均为 Asia/Shanghai。

## 总览

| 阶段 | 目标 | 状态 |
|---|---|---|
| 步骤 1 | ≥30 个真实调试场景 + 可视化设计 | 🚧 进行中 |
| 步骤 2 | 结合代码的编码走向分析（多插件/多机/工具形态） | ⏳ 未开始 |
| 步骤 3 | 向使用者展示汇报 | ⏳ 未开始 |

## 逐轮记录

| 轮次 | 时间 | 完成内容 | 证据 |
|---:|---|---|---|
| 1 | 10-16 10:05–10:20 | 开启定时任务（每 900 s，至 14:00）与目标；读项目规范；摸清 GMP 代码分布；**读 `scheduler.hpp` 核心定义，确认项目已有观察/快照/指标设施**；建立本目录与首批文档 | `schedule-49527cbd`、`goal-ff4d88c7`；`scheduler/scheduler.hpp:26-260`；`wc -l` 统计见 `01-existing-infrastructure.md` |
| 2 | 10-16 10:25–10:40 | 读 `Task` 与 `Scheduler` 全部公开接口（`scheduler.hpp:258-663`）、`fiber.hpp` 全文（228 行）、`core/hybrid_mutex.hpp` 全文（114 行）；**确认 Fiber 侧已有完整调试设施**（编号/状态/挂起原因/G-M-P 绑定/调用栈快照）；产出 **能力矩阵** `design/01-capability-matrix.md` | `fiber.hpp:14-30,36-48,54-69,199-221`；`scheduler.hpp:463-465,582-610`；`hybrid_mutex.hpp:23-57` |

## 下一轮任务（第 3 轮）

1. 读 `src/concurrency/sync.cpp`（1324 行）的锁实现，确认用户可见 `Mutex`/`RWMutex`/`Cond`
   是否有等待者队列、是否有可替换的公平性策略 —— 这是「锁争用决策」场景的前提；
2. 读 `src/concurrency/context.cpp` 的公开部分，确认是否维护**执行位置（行号）**；
3. 读 `src/concurrency/scheduler.cpp` 里 `emit_event()` 的调用点，确认事件发射的锁边界；
4. 开始撰写 `scenarios/`：按「只读观测 / 运行时变更 / 可视化 / 锁 / 远程」五类，先列出场景骨架。

## 未决问题（待后续轮次查证）

- `Task` 是否有「名字/描述/行号」元数据？若没有，需要何种注入方式？
- Fiber 的当前执行位置（行号）能否从上下文取得？需要符号信息吗？
- 跨 M 迁移的安全边界：Fiber 正在运行时迁移是否可行，还是只能对 parked 的 G 操作？
- 锁的可插拔策略点在哪里？现有 `HybridMutex` 的策略是否已可替换？
