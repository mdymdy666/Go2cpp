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
| 5 | 10-16 11:35–12:00 | **解决 dump 前置问题：枚举全部 G/Fiber** —— 找到 `task_registries`（16 分片 + 每分片锁，注释确认强持有每个非终态 G）与既有遍历范式；确认 `Processor::queue` 是某 P 的本地队列明细来源；确认 9 类事件在 6 个发射点的分发（3843-3851 一处覆盖 4 种）；**更正前几轮「9 类事件」的口误**；产出 `design/03-enumeration-and-registry.md` 与 `scenarios/B-mutation.md` | `scheduler.cpp:1002,1007-1021,1529,1694-1705,1792-1805,3843-3851` |
| 6 | 10-16 12:00–12:30 | 读 `Task::run`/`release_callable`（定案 B2：m_function 在 Fiber 首次进入时被移出，故替换只对未首次 run 的 G 有意义）；读 `requeue_to_processor`（定案 B3：基础已存在但仅接受未入队的 G，唤醒策略为 running==0 或队列≥16 且有空闲 M）；定位本地入队点 `processor.queue.push_back`（定案 B4 插入点）；产出 **编码走向** `design/04-encoding-roadmap.md`（分层/模块/插件协议/多机/阶段）与 `scenarios/C-visualization.md`；清理文档中残留的「10 类」表述 | `scheduler.cpp:126-130,830-905,2527-2574,2547`；`grep -rn '10 类' DebugTooling/` |
| 7 | 10-16 12:30–13:00 | 展开 **D 类**（`scenarios/D-locks.md`）与 **E 类**（`scenarios/E-remote-and-tooling.md`）；**34 个场景全部展开完毕**；D3 给出接口形态设计（WaitSelector：从等待队列中选谁，而非释放后重新竞争）；诚实记录两项做不到的能力：**锁持有者不可知**（Go 风格所有权）、**完整死锁环检测不可行**；E3 给出时钟问题的四个方案并建议 A+D | `sync.cpp:665-686,955-1005`；`sync.hpp:70-77,110-128`；`hybrid_mutex.hpp:23-57`；`fiber.hpp:36-48,65,199-201`；`scheduler.hpp:130-137` |
| 8 | 10-16 13:00–13:20 | 补 `design/02-dump-cli.md` 的 **text 渲染示例**（含五条渲染约定：缺信息如实呈现、快照一致性标注、kTaskSuspended 兜底语义、collect_metrics 关闭时的显示、列宽与颜色）；撰写 `design/05-architecture-summary.md`（交付物全景 + 实测行数 + 五层架构 + P0-P7 + 34 场景分布 + 5 个不能写死的点 + **7 项需使用者定案** + 风险 + 证据索引） | `wc -l` 实测；`docs/HANDOFF` 无涉及 |

## 下一轮任务（第 9 轮）

1. 撰写 **最终报告** `FINAL-REPORT.md`：面向使用者的完整汇报（目标三步的完成情况、
   交付物清单与实测规模、关键结论、需要定案的 7 项、后续建议）；
2. 复核全部文档的内部一致性（行号引用、场景数量、事件数量）；
3. 准备向使用者展示汇报。
