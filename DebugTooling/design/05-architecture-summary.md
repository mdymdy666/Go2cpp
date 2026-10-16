# design · 05 架构总览（一页看完）

> 本文件是全部设计的入口：**交付物全景 / 结论 / 能力现状 / 五层架构 / 阶段图 /
> 34 场景分布 / 不能写死的设计点 / 需要使用者定案的事项 / 风险**。
> 所有"现状"判断都有源码行号，可在 `design/01-capability-matrix.md` 与
> `01-existing-infrastructure.md` 中逐条核对。

## 1. 一句话结论

Go2Cpp 的 GMP 与协程层**已经具备一套完整的只读观测基础设施**
（Fiber 唯一编号、状态、挂起原因、G→M→P 绑定、含墓碑帧的调用栈、P/M 快照、指标、
可运行期替换的事件观察者），也**已经有两个官方可替换点**
（`WaitBackend` 工厂、`HybridMutex` 自旋参数）；
真正缺的是 **元数据（名字/函数/行号）**、**运行时变更控制面**（迁移/插队/替换/断点/锁选择）、
**序列化与 CLI**、**远程与多机通道** 四类。

## 2. 交付物全景（实测规模）

| 文件 | 作用 |
|---|---|
| `README.md` | 任务、三步走、硬约束、交付结构 |
| `PROGRESS.md` | 逐轮进度（已完成 / 下一轮 / 证据） |
| `01-existing-infrastructure.md` | 项目已有的调试相关设施（源码证据） |
| `design/01-capability-matrix.md` | **能力矩阵**：需求 × 现状 × 缺口 |
| `design/02-dump-cli.md` | dump 的 ini/json schema、**20 个选项**、text 渲染示例 |
| `design/03-enumeration-and-registry.md` | **枚举全部 G/Fiber 的机制**（前置问题解法） |
| `design/04-encoding-roadmap.md` | **编码走向**：五层、模块、插件协议、多机、P0-P7 |
| `design/05-architecture-summary.md` | 本文件 |
| `scenarios/00-scenario-index.md` | **34 个场景**索引 + 20 个 dump 选项骨架 |
| `scenarios/A-observability.md` | A 类：只读观测 8 个 |
| `scenarios/B-mutation.md` | B 类：运行时变更 8 个 |
| `scenarios/C-visualization.md` | C 类：可视化与输出 7 个 |
| `scenarios/D-locks.md` | D 类：锁与同步 5 个 |
| `scenarios/E-remote-and-tooling.md` | E 类：远程/多机/gdb/工具 6 个 |

### 实测行数（`wc -l DebugTooling/*.md DebugTooling/design/*.md DebugTooling/scenarios/*.md`）

```text
167 DebugTooling/01-existing-infrastructure.md
   135 DebugTooling/FINAL-REPORT.md
    42 DebugTooling/PROGRESS.md
    56 DebugTooling/README.md
   258 DebugTooling/design/01-capability-matrix.md
   243 DebugTooling/design/02-dump-cli.md
   127 DebugTooling/design/03-enumeration-and-registry.md
   215 DebugTooling/design/04-encoding-roadmap.md
   185 DebugTooling/design/05-architecture-summary.md
   264 DebugTooling/design/06-interface-signatures.md
   174 DebugTooling/design/07-test-strategy.md
   365 DebugTooling/design/08-verified-details.md
   123 DebugTooling/scenarios/00-scenario-index.md
   115 DebugTooling/scenarios/A-observability.md
   132 DebugTooling/scenarios/B-mutation.md
    97 DebugTooling/scenarios/C-visualization.md
   142 DebugTooling/scenarios/D-locks.md
   116 DebugTooling/scenarios/E-remote-and-tooling.md
   110 DebugTooling/scenarios/verification-matrix.md
  3066 total
```

## 3. 能力现状（三句话）

1. **看得见的，大部分现成**：`Fiber::id()`（唯一、不复用）、`FiberState`（5 态）、
   `SuspendReason`（6 态）、`FiberExecutionBinding`（G/M/P）、`FiberContextFrame` 调用栈
   （含 `alive=false` 墓碑帧）、`processors()`/`machines()`/`metrics()`、
   `SchedulerObserver`（9 类事件，可运行期替换）。
2. **改得动的，几乎都要新建**：唯一已开放的是 `WaitBackend` 工厂（换"怎么等/怎么唤醒"）
   与 `HybridMutex::SetDefaultOptions`（换自旋次数）；**"选谁获得锁"、迁移、插队、替换、断点全部缺失**。
3. **两类能力必须分开**：观测面（低风险、可插拔、几乎零开销）与控制面（高风险、默认关闭、
   必须带前置校验）—— 因为 `SchedulerObserver` 的注释明确它「不能控制当前 G 的执行权」，
   且项目多次强调「不会从另一个线程强行切断任意 C++ 调用栈」。

## 4. 五层架构与模块划分

```
L5 工具   CLI(dump) │ HTTP 端点 │ 库 API │ gdb 脚本
L4 传输   stdio │ 文件 │ 环形缓冲 │ topic(Stratum 式)
L3 序列化 text │ ini │ json
L2 快照   枚举 / 过滤 / 一致性标注（只读）
L1 采集   SchedulerObserver（已存在）│ 只读快照接口（新增）
L0 探针   安全点断点 │ 锁事件钩子 │ 元数据注册（可选）
```

| 新增模块 | 职责 | 是否触碰核心 |
|---|---|---|
| `debug/snapshot` | 枚举 G/P/M、取快照 | 需加**只读**接口 |
| `debug/metadata` | 名字/描述/函数/行号 | 需给 `Task` 加只读访问器 |
| `debug/serialize` | text/ini/json | 否（json 复用项目 `json.hpp`） |
| `debug/sink` | stdio/文件/缓冲/topic | 否 |
| `debug/control` | 迁移/插队/替换/断点/锁选择 | 需加**受控**接口（默认关闭） |
| `debug/cli` | `Go2Cpp dump scheduler ...` | 否 |

## 5. 阶段划分（P0-P7）与关键路径

| 阶段 | 内容 | 依赖 |
|---|---|---|
| **P0** | 只读快照接口 + 元数据注册 | — |
| **P1** | 序列化 + stdio/文件 sink → **dump 可用** | P0 |
| **P2** | 事件采集 + 环形缓冲 + `--follow` | P1 |
| **P3** | 锁观测（D1/D2）+ 锁选择钩子（D3） | P0 |
| **P4** | 控制面（迁移/插队/延迟替换），默认关闭 | P0 |
| **P5** | 安全点断点 | P4 |
| **P6** | topic 传输 + 多机 | P1 + 时钟方案定案 |
| **P7** | gdb 混合（映射导出） | P0 |

**关键路径**：**P0 + P1 = 最小可用集**，且除 P0 的只读接口外**完全不触碰调度器热路径**。

## 6. 34 个场景的分布

| 类 | 数量 | 文件 | 现状概括 |
|---|---:|---|---|
| A 只读观测 | 8 | `scenarios/A-observability.md` | 数据基本齐备，缺"枚举入口"（已在 `design/03` 解决）与序列化 |
| B 运行时变更 | 8 | `scenarios/B-mutation.md` | 仅 B6（取消）与 B8（等待后端）现成，其余需新建且带安全边界 |
| C 可视化输出 | 7 | `scenarios/C-visualization.md` | 全部需新增渲染/输出层 |
| D 锁与同步 | 5 | `scenarios/D-locks.md` | 全部需新增只读接口；D3 需新增选择钩子 |
| E 远程与工具 | 6 | `scenarios/E-remote-and-tooling.md` | 全部需新建；含两个硬前提（时钟、gdb 边界） |

dump 选项：**20 个**（要求 ≥15），见 `design/02-dump-cli.md` §5。

## 7. 五个不能写死的设计点

| # | 设计点 | 为什么不能写死 |
|---:|---|---|
| 1 | 元数据来源（转译期注入 / 运行期注册） | 取决于转译器能否提供符号（**未确认**）；手写 C++ 又必须支持运行期 |
| 2 | 多机时间方案 | `steady_clock` 跨机不可比；四种方案代价不同，必须由使用者定 |
| 3 | 断点安全点粒度 | 只能停在协作挂起点，精度取决于安全点分布（**未统计**） |
| 4 | 调试通道的开关与开销保证 | 必须能证明"关闭时近零开销"（需实测 `emit_event` 空观察者路径） |
| 5 | `SchedulerConfig` 运行期可改范围 | 结构类字段注释明确禁止动态修改（`scheduler.hpp:163-165`） |

## 8. 需要使用者定案的事项（**阻塞后续编码**）

| # | 问题 | 备选 | 我的建议 |
|---:|---|---|---|
| 1 | 元数据注入方式 | 转译期 / 运行期 / 共存 | **共存**：转译期自动注入 + 运行期可覆盖 |
| 2 | 多机时钟方案 | 双时间戳 / 逻辑时钟 / 外部脉冲 / 不统一 | **双时间戳为基础 + 明确声明不做绝对对齐** |
| 3 | 控制面启用方式 | 编译期宏 / 运行期配置 / 环境变量 | **编译期默认关闭 + 运行期显式开启**（双重） |
| 4 | 是否接受"断点只能停在安全点" | 接受 / 不接受（则需风险评估信号抢占） | 建议**接受**（强行切断栈会破坏 RAII） |
| 5 | 是否要 owner 追踪（为锁持有者/死锁检测） | 要 / 不要 | 建议**只在调试构建中开启**，并声明行为差异 |
| 6 | 工具形态范围 | 库+CLI / 再加 HTTP / 再加 topic | 建议 **P0 库 + CLI**，HTTP 待评估依赖 |
| 7 | 调试数据脱敏要求 | 允许含参数与局部值 / 仅结构信息 | 建议**默认仅结构信息**，敏感数据需显式开启 |

## 9. 风险清单

| 风险 | 影响 | 缓解 |
|---|---|---|
| 观察者回调拖慢 worker | 改变被观测对象时序（观察者效应） | sink 非阻塞入队 + 独立消费线程 + 丢弃计数 |
| 控制面误用（迁移/插队） | 破坏 FIFO 公平性、引发难以复现的问题 | 默认关闭 + 前置校验 + 破坏性操作需显式确认 |
| 快照不一致被误读 | 使用者以为拿到同一瞬间全景 | 输出中显式标注"分片级一致、全局非原子" |
| 元数据缺失被伪装 | 使用者把 `unknown` 当成"确实未知" | 强制区分"未注册"与"无来源" |
| 多机时间误读 | 时间线结论错误 | 输出标注所用时钟方案 |
| gdb 期望错位 | 使用者以为 gdb 能看到 Fiber 栈 | 文档多处显式声明（E4） |

## 10. 证据索引（本文结论的源码出处）

| 结论 | 出处 |
|---|---|
| GMP 三态枚举 | `scheduler/scheduler.hpp:47-101` |
| 9 类事件与 `SchedulerEvent` | `scheduler.hpp:103-138` |
| 观察者边界（不能控制执行权） | `scheduler.hpp:140-183` |
| `SchedulerConfig` 16 字段 | `scheduler.hpp:161-197` |
| P/M 快照与指标 | `scheduler.hpp:210-256` |
| `Task` 公开接口 | `scheduler.hpp:258-390` |
| **`m_function` 私有、无元数据** | `scheduler.hpp:356,362` |
| 快照/计数接口 | `scheduler.hpp:582-610` |
| `emit_event` 锁外回调 | `scheduler.cpp:1104-1122` |
| 任务事件三元分发（`kTaskSuspended` 兜底） | `scheduler.cpp:3843-3851` |
| 指标仅在 `collect_metrics` 时累计 | `scheduler.cpp:3852-3862` |
| **`m_function` 首次进入时被移出** | `scheduler.cpp:830-905` |
| `requeue_to_processor`（迁移基础） | `scheduler.cpp:2527-2574` |
| 本地入队点（插队插入点） | `scheduler.cpp:2547` |
| **注册表强持有全部非终态 G** | `scheduler.cpp:1529,1792-1805` |
| 注册表遍历范式 | `scheduler.cpp:1694-1705,1743-1750` |
| `Processor::queue`（本地队列明细来源） | `scheduler.cpp:1007-1021` |
| Fiber 编号/状态/挂起原因 | `fiber.hpp:14-30,199-221` |
| G→M→P 绑定（观测元数据） | `fiber.hpp:36-48` |
| 调用栈帧与墓碑帧 | `fiber.hpp:54-69,206-211` |
| `Fiber::resume` 必须串行 | `fiber.hpp:90-93` |
| 锁为 FIFO 混合互斥 + Go 风格所有权 | `sync.hpp:70-77` |
| `WaitBackend` 可替换（只管 park/wake） | `sync.hpp:13-66`、`sync.cpp:612/627` |
| `Mutex::Impl`（无 owner 记录） | `sync.cpp:665-686` |
| `LockImpl` 协作重试 256/4 次策略 | `sync.cpp:711-760` |
| `Mutex::Unlock` 严格 FIFO + 所有权无缝转移 | `sync.cpp:955-1005` |
| `HybridMutex` 仅自旋策略可调 | `hybrid_mutex.hpp:23-57` |
| **`context` 无任何执行位置信息** | `grep -c` 于 `context.cpp` 结果为 0 |

## 11. 本文件的自我约束

- 只写**已读源码确认**的内容；未确认项标注"未确认"；
- 阶段划分与接口签名是**设计建议**，不是实现承诺；
- 凡"做不到"的能力（锁持有者、死锁环检测、任意行断点、跨机绝对时间、
  gdb 看 Fiber 栈、回放重建运行时）**一律显式声明**，不用模糊措辞掩盖。
