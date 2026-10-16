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
| 6 | 10-16 12:00–12:30 | 读 `Task::run`/`release_callable`（定案 B2：m_function 在 Fiber 首次进入时被移出，故替换只对未首次 run 的 G 有意义）；读 `requeue_to_processor`（定案 B3：基础已存在但仅接受未入队的 G，唤醒策略为 running==0 或队列≥16 且有空闲 M）；定位本地入队点 `processor.queue.push_back`（定案 B4 插入点）；产出 **编码走向** `design/04-encoding-roadmap.md`（分层/模块/插件协议/多机/阶段）与 `scenarios/C-visualization.md`；清理文档中残留的「10 类」表述 | `scheduler.cpp:126-130,830-905,2527-2574,2547`；`grep -rn '10 类' DebugTooling/` |
| 7 | 10-16 12:30–13:00 | 展开 **D 类**（`scenarios/D-locks.md`）与 **E 类**（`scenarios/E-remote-and-tooling.md`）；**34 个场景全部展开完毕**；D3 给出接口形态设计（WaitSelector：从等待队列中选谁，而非释放后重新竞争）；诚实记录两项做不到的能力：**锁持有者不可知**（Go 风格所有权）、**完整死锁环检测不可行**；E3 给出时钟问题的四个方案并建议 A+D | `sync.cpp:665-686,955-1005`；`sync.hpp:70-77,110-128`；`hybrid_mutex.hpp:23-57`；`fiber.hpp:36-48,65,199-201`；`scheduler.hpp:130-137` |
| 8 | 10-16 13:00–13:20 | 补 `design/02-dump-cli.md` 的 **text 渲染示例**（含五条渲染约定：缺信息如实呈现、快照一致性标注、kTaskSuspended 兜底语义、collect_metrics 关闭时的显示、列宽与颜色）；撰写 `design/05-architecture-summary.md`（交付物全景 + 实测行数 + 五层架构 + P0-P7 + 34 场景分布 + 5 个不能写死的点 + **7 项需使用者定案** + 风险 + 证据索引） | `wc -l` 实测；`docs/HANDOFF` 无涉及 |
| 9 | 10-16 13:20–13:40 | **全文一致性复核**（文件清单/事件数量/场景数量/选项数量）—— 发现并修正第 6 轮批量替换造成的 **2 处误伤**（把「引用历史错误」的文本也替换了，导致「写过 9 类……这是错的」这种自相矛盾）；撰写 `FINAL-REPORT.md`（三步完成情况、现有能力与四类缺口、三个「能做的比预期少」的定案、34 场景与 20 选项、**六项做不到的能力**、7 项待定案、复核记录、后续建议） | `grep -rn '9 类\|10 类'` 复核输出；`find . -name '*.md'` = 14 个文件 |
| 10 | 10-16 深化①：接口签名汇总 | 新增 `design/06-interface-signatures.md`：把散落在各文档的接口设计汇总为 **15 个接口**（R 只读 6 / C 受控 5 / P 插件 4 / T 传输 3），每个标注归属层、前置条件、失败语义、线程安全、默认开关与源码依据；给出接口依赖顺序与关键路径（R1/R2 → T1 → P3 即可产出可用 dump）；把 C1 的语义收窄、C3 的改栈禁止、C5 的禁止重新竞争标为**硬约束** | `design/06-interface-signatures.md`（依据列复用第 2-7 轮已核验的行号） |

## 下一轮任务（第 11 轮）

**深化②：测试策略与验收标准** —— 新增 `design/07-test-strategy.md`：

1. 按项目规范（`PROJECT_RULES.md` 第 3 节）给出三类测试方案：压测指标（吞吐/延迟 P50/P95/P99/内存）、
   随机测试（随机枚举规模、随机状态过滤、并发快照竞态）、≥5 个真实场景；
2. **重点写清「调试模块自身的零开销证明」**：如何量化 `emit_event` 在无观察者时的成本、
   如何证明 sink 非阻塞（测量 worker 线程的最坏停顿）；
3. 给出验收标准与失败判据（含 C 组控制面的破坏性测试与回滚验证）。
