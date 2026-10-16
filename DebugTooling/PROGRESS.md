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
| 11 | 10-16 深化②：测试策略 | 新增 `design/07-test-strategy.md`：提出**两个验收维度**（功能正确性 + **观察者效应**，后者是项目规范未覆盖、调试工具特有的）；压测给出 5 项指标与 5 维条件矩阵（G 数/P 数/负载类型/事件速率/并发 dump）；随机测试 7 项（含并发竞态与转义 fuzz）；真实场景 6 个（覆盖 A–E 全部类别）；**零开销证明设计为三档对照实验 + emit_event 微基准 + 最坏停顿测量**，并给出待校准的建议阈值与「拿到数据前不得宣称零开销」的约束；破坏性测试 7 项与失败判据 5 条 | `design/07-test-strategy.md`；依据 `scheduler.cpp:1104-1122`（无观察者路径与异常隔离） |
| 12 | 10-16 深化③：场景×验证矩阵 | 新增 `scenarios/verification-matrix.md`：34 个场景逐条映射「验证方式/测试类型/验收判据/依赖接口/阻塞项」；统计出 **24 个无阻塞项**、7 个依赖未定案；给出**发布前检查清单**（功能层/干扰层/控制面/文档记录 四组共 18 项）与编码阶段 10 步推进顺序 | `scenarios/verification-matrix.md`；接口编号引自 `design/06`；测试类型代号引自 `design/07` |
| 13 | 10-16 核验①：worker 出队路径与事件发射锁边界 | 读 `worker_loop`（`scheduler.cpp:3494` 起）确认**四级出队顺序**：本地 P 队列（队首 `pop_front`，FIFO）→ incoming 条带（`incoming_probe` 轮转）→ 全局队列 → **work stealing 从其他 P 的队尾偷**（注释原文 steal from the back）；并确认**本地快路径有门槛**（`runnable > processors.size()`）；`grep -B4 -A3 emit_event` 确认 **6 个发射点全部不持调度器状态锁**，且 `kWorkerStarted` 在 thread-local 的 M/P 赋值之后发出（插件可在回调中安全读 `current_*`）；据此**订正** B4/C2（插队需考虑偷取方向）与 `design/04` §3（非阻塞要求来自时序而非锁竞争）；新增 `design/08-verified-details.md` | `scheduler.cpp:1939,1992,3503,3519-3541,3742,3833,3848,4036` |
| 14 | 10-16 核验②：等待机制与安全点统计 | 读默认 `WaitBackend`（`sync.cpp:31-100`）与 `WaitNode` 核心（`167-235`、`296-345`）：确认 `TryFinish` 是 `m_result` 的 **CAS 单赢家**（与 `SelectWaitState::TrySelect` 同构）、存在**两条等待路径**（Fiber 走 `park_wait`、线程走 `WaitNative`）；**新发现** `WaitNode` 有 `generation` 机制解决「缓存节点 + 迟到取消回调」竞态，且节点会被 `t_wait_node_cache` 复用→ 调试快照**必须排除缓存节点**以免虚报等待者；**统计安全点：真实挂起点仅 5 处**（`scheduler.cpp:2661/2723/2744/2782/2809`，对应 Yield/Park/Park/Io/通用）→ **解除 B5 的阻塞项**并给出可承诺精度（四类语义位置，粒度=一次挂起） | `sync.cpp:31-100,167-185,194-235,301-314,360`；`scheduler.cpp:2661-2823`；`fiber.cpp:1258` |
| 15 | 10-16 核验③：条件变量与 sysmon 阻塞观测 | 读 `ConditionVariable`（`Implement`/`Wait`/`NotifyOne`/`NotifyAll`，`sync.cpp:1021-1180`）：确认 `NotifyOne` 与 `Mutex::Unlock` **完全同构**（队首取 + TryFinish 认领 + 失效跳过 + 锁外 Wake），`NotifyAll` 用 `swap` 端出队列后锁外逐个唤醒（与 `Scheduler::Close` 同手法）；读 sysmon 阻塞观测的**完整生命周期**（声明 `3200-3218` → 超阈 detach `1432-1458` → 退出清理 `3255-3270`，阈值默认 50ms）；**订正 A6 三个易误读点**：`blocking_task` 是阻塞中的 G id、`long_syscall_count` 是累计次数而非时长、detach 会减 `active_machines` 从而可能启动替代 M；并指出「当前阻塞时长」需新增字段（`blocking_since_ns` 未导出） | `sync.cpp:1021-1180`；`scheduler.cpp:1051-1054,1432-1458,3200-3218,3255-3270,3386-3388`；`scheduler.hpp:178-181` |
| 16 | 10-16 核验④：Context 回调与 Hook 阻塞协作 | 读 `DoneSignal`/`Context` 回调机制（`context.hpp:41-89,256`；`context.cpp:23-27,105-176,266,409-410`）：确认回调分**三类**（internal / before-done / 普通）且**生命周期由调用方负责**（注释原文：信号无法推断回调的对象所有权）→ 调试插件必须用 RAII（与 `CallbackGuard` 同纪律）；读 `hook.cpp`：确认被拦截的 syscall（io/connect/sleep）在 managed G 上用 `BlockingRegion` 声明阻塞，**普通线程保持原生语义**（`hook.cpp:963` 注释）→ 据此指出 A6 的**观测覆盖边界**（未声明的原生阻塞不可见）；补 B6 建议（优先用 `kTaskCancelled` 事件而非自行挂钩回调）；并给出第 13-16 轮核验小结（订正 9 处、解除 1 个阻塞项、4 条易误读、1 条覆盖边界） | `context.hpp:41-89,256-258`；`context.cpp:23-27,105,126,176,266,409-410`；`hook.cpp:206,832-846,940-963,1025` |
| 17 | 10-16 收官准备 | 采集最新规模（`wc -l`，全部交付 md 合计 3066 行）并更新 `design/05-architecture-summary.md` 的实测行数块；把核验阶段成果合并进 `FINAL-REPORT.md`（新增第 10 节）：四项核验结论、对第 6 节的修正（任意行断点边界**量化为 5 个挂起点**、新增第 7 项**观测覆盖边界**）、四条易误读提示、并把待定案事项**精确分为 A 硬阻塞 3 项 + B 偏好确认 4 项**，同时记录核验净成果（订正 9 处、解除 1 个阻塞、4 条易误读、1 条覆盖边界） | `wc -l DebugTooling/**/*.md`；`design/08-verified-details.md` 小结 |
| 18 | 10-16 一致性复核与速览 | 跨文档一致性复核：**安全点 5 处、事件 9 类、场景 34、接口 15 全部一致**；发现并修正**一处文档内冲突**（`FINAL-REPORT` 第 7 节写 7 项、第 10.4 节写 3+4 项）——第 7 节标题改为保留初版记录并指向 §10.4 的精确分类；新增 `QUICKSTART.md`（一页速览：先读什么、3 项硬阻塞、现在就能开工的 24 个场景与第一步、做不到的 7 件事、交付物地图） | `grep -rn "5 个挂起点\|9 类\|34 个\|15 个接口"` 复核输出 |

## 下一轮任务（第 19 轮）

1. 抽查核验过的源码行号是否仍然有效（例如 `scheduler.cpp:1529`、`sync.cpp:955`、
   `fiber.hpp:206`、`hybrid_mutex.hpp:23`、`context.hpp:73`、`hook.cpp:963`）；
2. 若发现行号漂移则订正；
3. 准备 14:00 的收官汇报（届时删除定时任务）。
