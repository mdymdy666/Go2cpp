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
| 3 | 10-16 10:45–11:05 | 定位用户可见锁（`include/go2cpp/sync.hpp`，219 行）并读其接口；读 `sync.cpp:665-760` 的 `Mutex::Impl` 与 `LockImpl` 争用策略；**确认 `WaitBackend` 是可替换等待后端但「选谁」不可替换**；**确认 `context` 无任何执行位置信息**（grep 计数 0）；产出场景索引骨架 34 个场景 + 20 个 dump 选项 | `sync.hpp:13-66,70-121`；`sync.cpp:665-686,711-760`；`grep -c "source_location|__builtin_return_address|dladdr|backtrace" src/concurrency/context.cpp` = 0 |

## 下一轮任务（第 4 轮）

1. 读 `src/concurrency/scheduler.cpp` 中 `emit_event()` 的实现与调用点，确认事件发射的**锁边界**
   （这决定观测插件能否安全地做重活）；
2. 读 `sync.cpp` 的 `Unlock` 路径与 `WaitNode` 的 `Arm/Disarm`，为 D1/D2/D3 场景补设计前提；
3. 开始展开 **A 类场景**（只读观测，8 个）的详细设计：前置条件 / 触发 / 期望信息 / 验证方式；
4. 起草 `design/02-dump-cli.md` 的输出 schema（ini 与 json 字段定义）。

## 未决问题（待后续轮次查证）

- 元数据（函数名/文件/行号/描述）走转译期注入还是运行期注册？需先确认转译器能力；
- 跨 M 迁移对「运行中」的 G 是否可行（现有注释只保证不强行切断调用栈）；
- 多机场景的时钟问题：`SchedulerEvent::timestamp` 是 `steady_clock`，跨机不可比，如何统一；
- `SchedulerConfig` 是否需要运行期 setter（B7 场景）。
