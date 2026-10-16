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
| 4 | 10-16 11:15–11:35 | 读 `emit_event` 实现（锁边界）与 `Mutex::Unlock`（FIFO 交接细节）；产出 `design/02-dump-cli.md`（ini/json schema + 字段→接口映射 + 20 选项行为）与 `scenarios/A-observability.md`（A 类 8 场景详细设计）；**暴露两个结构性问题：无法枚举全部 Fiber、事件无历史缓冲** | `scheduler.cpp:1104-1122`（锁外回调+异常隔离，发射点 6 处）；`sync.cpp:955-1005`（严格 FIFO + 所有权无缝转移，TryLock 不能插队） |
| 5 | 10-16 11:35–12:00 | **解决 dump 前置问题：枚举全部 G/Fiber** —— 找到 `task_registries`（16 分片 + 每分片锁，注释确认强持有每个非终态 G）与既有遍历范式；确认 `Processor::queue` 是某 P 的本地队列明细来源；确认 9 类事件在 6 个发射点的分发（3843-3851 一处覆盖 4 种）；**更正前几轮「10 类事件」的口误**；产出 `design/03-enumeration-and-registry.md` 与 `scenarios/B-mutation.md` | `scheduler.cpp:1002,1007-1021,1529,1694-1705,1792-1805,3843-3851` |

## 下一轮任务（第 6 轮）

1. 读 `Task::run()` 与 `release_callable()` 的实现，确定 B2（替换函数）的可行语义；
2. 读 `requeue_to_processor` 的实现与调用点，定 B3（跨 M 迁移）的接口形状；
3. 读 P 队列的取任务路径（worker 出队），定 B4（插队）的插入点；
4. 展开 **C 类场景**（可视化与输出 7 个）的详细设计，并补 text 渲染示例。

## 未决问题（待后续轮次查证）

- 元数据走「转译期注入」还是「运行期注册」？还是两者共存（需使用者定案）；
- 断点安全点的分布与数量（决定 B5 的可用精度）；
- 多机时钟统一方案（E3 的前提）。
