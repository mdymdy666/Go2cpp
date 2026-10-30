# Go2Cpp 文档索引（INDEX）

> 盘点基准：`HEAD 1b46de8`（MCP 已内置），2026-10-26。
> 统计：`docs/` 下 **102 篇既有文档**（不含本索引）= 73 篇主题文档（23,894 行）+ 19 篇状态机 + 10 篇 review；
> 另有 MCP 架构文档 `mcp/docs/architecture.md`。
> 本索引是文档的唯一入口；新增/改名文档必须在此登记。代码语义不在本索引范围内。

## 目录

- [主题导航](#主题导航)
- [状态图例](#状态图例)
- [一、入门（6）](#一入门6)
- [二、核心设计（13）](#二核心设计13)
- [三、使用指南（16）](#三使用指南16)
- [四、诊断与调试（6）](#四诊断与调试6)
- [五、测试与性能（20）](#五测试与性能20)
- [六、历史与交接（12）](#六历史与交接12)
- [七、MCP / eBPF / 工具（入口）](#七mcp--ebpf--工具入口)
- [八、附录：状态机文档（19）](#八附录状态机文档19)
- [九、附录：review 问题库（10）](#九附录review-问题库10)
- [十、重复 / 过时 / 孤立清单](#十重复--过时--孤立清单)
- [维护约定](#维护约定)
- [模块文档（docs/modules/）](#模块文档docsmodules)

## 主题导航

| 主题 | 入口 | 适合谁 |
| --- | --- | --- |
| 入门 | [getting_started.md](getting_started.md) → [compatibility.md](compatibility.md) → [design.md](design.md) | 第一次接触本项目 |
| 核心设计 | [modules/scheduler.md](modules/scheduler.md)、[modules/fiber.md](modules/fiber.md)、[boundary_audit.md](boundary_audit.md)、[fiber-migration.md](fiber-migration.md) | 改运行时、评审架构 |
| 使用指南 | [configuration.md](configuration.md)、[logging.md](logging.md)、[fiber_guide.md](fiber_guide.md)、[network.md](network.md) | 业务接入 |
| 诊断与调试 | [diagnostic-channel.md](diagnostic-channel.md)、[debug-reliability.md](debug-reliability.md)、[concurrency-park-points.md](concurrency-park-points.md) | 排障、定位挂起 |
| 测试与性能 | [testing.md](testing.md)、[performance-baseline-2026-10-19.md](performance-baseline-2026-10-19.md) | 跑测试、看性能 |
| 历史与交接 | [AGENT-HANDOFF.md](AGENT-HANDOFF.md)、[NEXT-STEPS.md](NEXT-STEPS.md) | 追溯决策 |
| MCP / eBPF | [modules/mcp.md](modules/mcp.md)、[mcp/docs/architecture.md](../mcp/docs/architecture.md) | 扩展 MCP 与监控 |

## 状态图例

- **现行**：与当前 HEAD 的实现/用法一致，可直接照做。
- **历史**：某次会话/轮次/旧基线的快照，只作追溯；其中的数字、HEAD、行号不再代表现状。
- **待更新**：主体仍有效，但含过期计数、路径、行号或未跟踪状态，引用前需核对。

---

## 一、入门（6）

| 文档 | 一句话说明 | 状态 |
| --- | --- | --- |
| [getting_started.md](getting_started.md) | 环境要求、最短可运行程序、Fiber/线程混合规则、生命周期与排错步骤 | 现行 |
| [compatibility.md](compatibility.md) | Go 能力 → 已实现子集 → 明确差异/前置条件 → 验证方式的兼容性矩阵 | 现行 |
| [dependencies.md](dependencies.md) | Go 1.4.3/1.23.0 参考源码、第三方依赖与版本、归档校验方式 | 现行 |
| [design.md](design.md) | 运行时范围、模块划分、线程/Fiber/IO 契约与不承诺项 | 现行 |
| [extensibility.md](extensibility.md) | 各模块核心职责、可替换边界、不应承担的职责 | 现行 |
| [modules.md](modules.md) | 模块目录 → CMake target → 头文件布局 → 依赖边界 | 待更新（计数已修为 24，但第 1 节表格仍只列 7 个目录） |

## 二、核心设计（13）

| 文档 | 一句话说明 | 状态 |
| --- | --- | --- |
| [scheduler.md](scheduler.md) | G/M/P 数据结构、所有权、状态不变量与 park/wake 契约 | 现行 |
| [boundary_audit.md](boundary_audit.md) | 并发、生命周期、错误、扩展点的边界与“不能这样用”清单 | 现行 |
| [correctness_audit.md](correctness_audit.md) | 可被代码与测试共同支持的正确性范围及适用前提 | 现行 |
| [go_reference_audit.md](go_reference_audit.md) | 与 Go 1.23 参考源码的逐项偏差审计（非逐行移植声明） | 现行 |
| [fiber-growable-stack-design.md](fiber-growable-stack-design.md) | Fiber 从固定 128KB 到可变长栈的 P0/P1 方案与风险 | 现行（P0 已落地，P1 未做） |
| [fiber-migration.md](fiber-migration.md) | Fiber/Channel 跨进程迁移：safepoint id、句柄表、签名 v3 | 现行 |
| [so-reload-design.md](so-reload-design.md) | SO 热重载配置层的分层、事务时序与失败回滚设计 | 现行（已实现） |
| [so-reload-scenarios.md](so-reload-scenarios.md) | 17 个工业级热重载场景/边界及“复用哪个 config 原语” | 现行 |
| [timer_design_review.md](timer_design_review.md) | TimerService/Scheduler/IOManager 审计结论与解耦约束（已并入 modules/timer.md） | 历史 |
| [hook-blocking-policy.md](hook-blocking-policy.md) | 哪些阻塞系统调用应 hook（park Fiber）、哪些绝不应 hook | 现行 |
| [context_rollback.md](context_rollback.md) | ContextRollback 的受限补偿语义（非事务、非 longjmp） | 现行 |
| [quic.md](quic.md) | QUIC/HTTP3 现状：picoquic 薄封装，自研实现已删除 | 现行 |
| [trace_format.md](trace_format.md) | C-L1 trace 二进制格式详细设计（方向计划阶段 1） | 历史（设计未落地，源码无 trace 实现） |

## 三、使用指南（16）

| 文档 | 一句话说明 | 状态 |
| --- | --- | --- |
| [configuration.md](configuration.md) | INI 解析、强类型 `ConfigVar<T>`、监听器、热加载与校验 | 现行 |
| [logging.md](logging.md) | Filter/Worker/Formatter/Sink 分层、默认 `log/go2cpp.log` 行为 | 现行 |
| [fiber_guide.md](fiber_guide.md) | Fiber/FiberLocal/thread_policy 场景驱动讲解（861 行，核心长文） | 现行 |
| [context_guide.md](context_guide.md) | Context 取消树、截止时间、值传递与 rollback 的场景讲解 | 现行 |
| [control_flow_guide.md](control_flow_guide.md) | defer/panic/recover 显式控制流的用法与边界 | 现行 |
| [error_guide.md](error_guide.md) | 错误链 `Is/As/Unwrap/Join` 的实现与用法 | 现行 |
| [network.md](network.md) | 地址、缓冲、Socket、Framer、HTTP、WebSocket 分层与等待规则 | 现行 |
| [sse.md](sse.md) | Server-Sent Events 编码、增量解析与写入 | 现行 |
| [websocket.md](websocket.md) | RFC 6455 WebSocket P1 协议层用法 | 现行 |
| [timer_usage.md](timer_usage.md) | TimerService/Timer/TimerExecutor 一次性与周期任务用法 | 现行 |
| [cli.md](cli.md) | `go2cpp-cli` 统一命令入口、协议对齐与子命令 | 现行 |
| [replay-service.md](replay-service.md) | 语义级 record/replay v2：select、虚拟时钟、检查点 | 现行 |
| [so-reload-usage.md](so-reload-usage.md) | SO 热重载接入步骤、事务语义与运维注意事项 | 现行 |
| [integration-test.md](integration-test.md) | Debug/迁移/重放/CLI 四线端到端联调记录（44 项断言） | 历史 |
| [quic-go-reading-guide.md](quic-go-reading-guide.md) | 为什么读 quic-go、按什么顺序读、哪些设计值得借鉴 | 现行 |
| [quic-lib-evaluation.md](quic-lib-evaluation.md) | 5 个 QUIC 库对比、选 picoquic 的理由与集成卡点 | 现行 |

## 四、诊断与调试（6）

| 文档 | 一句话说明 | 状态 |
| --- | --- | --- |
| [diagnostic-channel.md](diagnostic-channel.md) | Doctor 在线诊断：Unix socket/命令平面、白名单唤醒、等待图、死锁检测 | 现行 |
| [debug-reliability.md](debug-reliability.md) | 哪些 debug 结论可信、哪些只能参考；窗口截断与采样偏差 | 现行 |
| [debug-overhead.md](debug-overhead.md) | `GO2CPP_DEBUG_DUMP` 全开 vs 关闭的 6 场景 × 3 配置压测 | 现行 |
| [concurrency-baseline.md](concurrency-baseline.md) | 并发原语调试监控开销基线（抓栈是主要成本，v7 数据） | 现行 |
| [concurrency-park-points.md](concurrency-park-points.md) | 并发 park 点清单与临界区禁用清单 | 现行 |
| [channel-hang.md](channel-hang.md) | 线程版 Channel 挂起调查：装置 Gate bug 必现，原始挂起 13.4 亿条 0 复现 | 现行（已结案） |

## 五、测试与性能（20）

| 文档 | 一句话说明 | 状态 |
| --- | --- | --- |
| [testing.md](testing.md) | 测试矩阵、构建目录、CTest 超时/看门狗与 CI 入口 | 待更新（跨多次重构，含 130 等旧计数） |
| [test-audit-2026-10-21.md](test-audit-2026-10-21.md) | 全量测试审计：0–8 节为早间快照，第 9 节为 `ctest -N` 清单 | 历史 |
| [performance.md](performance.md) | 2026-10-21 性能数据汇总（并发原语/内存/切换/IO） | 现行 |
| [performance-baseline-2026-10-19.md](performance-baseline-2026-10-19.md) | **当前唯一有效性能基线**：公平对比方法、三轮优化根因与前后数据 | 现行 |
| [memory-benchmark.md](memory-benchmark.md) | Fiber vs 线程内存基准：省虚拟地址与固定开销，RSS 只略省 | 现行 |
| [switch-benchmark.md](switch-benchmark.md) | Fiber ~500ns vs 线程 ~28µs 的切换开销基准 | 现行 |
| [timer_benchmark.md](timer_benchmark.md) | 定时任务三模型（线程/TimerService/P-Fiber）基准 | 现行 |
| [persistence-benchmark.md](persistence-benchmark.md) | 事件流“追加写 + 持久化”原型基准（单线程 2.96×/4 线程 13.83×） | 现行 |
| [concurrency-5rounds.md](concurrency-5rounds.md) | 随机 + 大数据量 + 多场景的 5 轮并发测试报告 | 历史 |
| [concurrency_audit_2026-10-18.md](concurrency_audit_2026-10-18.md) | channel/future/io 并发设施审计（113/113 时期） | 历史 |
| [module-audit-2026-10-20.md](module-audit-2026-10-20.md) | 7 个基础模块并行审计（缺陷行号、复现命令、压测） | 历史 |
| [state_machine_audit_2026-10-18.md](state_machine_audit_2026-10-18.md) | 12 类状态机正确性与并发风险审查 | 历史 |
| [bug-report-2026-10-20.md](bug-report-2026-10-20.md) | 五能力（Debug/迁移/回放/CLI/Doctor）对抗性缺陷报告 | 历史 |
| [quality_backlog.md](quality_backlog.md) | 文档/测试/示例质量台账与待办（含旧副本读取警告） | 现行（待办未清） |
| [parameter-tuning.md](parameter-tuning.md) | 影响性能的默认参数扫描与源码默认值调整记录 | 历史 |
| [gmp-perf-analysis.md](gmp-perf-analysis.md) | GMP vs thread/epoll+池的旧基准分析（epoll 池模型不公平） | 历史 |
| [gmp-scenario-analysis.md](gmp-scenario-analysis.md) | GMP 分场景实测（“每轮追加”，未完成） | 历史 |
| [gmp-use-cases.md](gmp-use-cases.md) | GMP 适用场景修正版（旧版 3.6× 等结论已作废） | 历史 |
| [gmp-vs-thread-epoll-2026-10-18.md](gmp-vs-thread-epoll-2026-10-18.md) | 三模型同协议压测报告（旧 benchmark） | 历史 |
| [coost-comparison.md](coost-comparison.md) | coost 对比与 GMP 优化线索（数据自标作废，保留方法论） | 历史 |

## 六、历史与交接（12）

| 文档 | 一句话说明 | 状态 |
| --- | --- | --- |
| [AGENT-HANDOFF.md](AGENT-HANDOFF.md) | 面向新 AI Agent 的 10 分钟开工手册（2026-10-19，HEAD `ab03bea`） | 历史 |
| [HANDOFF.md](HANDOFF.md) | 零上下文项目认知 + 当前任务进度（2026-10-15） | 历史 |
| [NEXT-STEPS.md](NEXT-STEPS.md) | 未完成项与后续计划（2026-10-18，HEAD `37a2196`，113/113） | 历史 |
| [PROGRESS-ROUND2.md](PROGRESS-ROUND2.md) | 第二轮 6 小时推进记录（2026-10-18） | 历史 |
| [development-directions.md](development-directions.md) | 开发方向池（20 个，封顶存档，含逐轮追加） | 历史 |
| [direction_plan.md](direction_plan.md) | 方向决策与落地路线：竞品/就业/三维打分（2026-10-13） | 历史 |
| [annotation-style.md](annotation-style.md) | 全项目注释格式唯一依据（2026-10-19 注释轮） | 历史 |
| [annotation-progress.md](annotation-progress.md) | 注释工作模块分工与进度（113/113 时期） | 历史 |
| [function-annotation-progress.md](function-annotation-progress.md) | 重要函数级注释进度（同上，已停） | 历史 |
| [so-reload-progress.md](so-reload-progress.md) | SO 热重载 step1–step4 进度与审查记录（ctest 114/114 时期） | 历史 |
| [gmp-optimization-log.md](gmp-optimization-log.md) | 1,663 行 GMP 优化轮次日志（对比数据自标作废） | 历史 |
| [code-review-guide.md](code-review-guide.md) | 代码导读/审查路线图（基线 `bd99603`，行号已漂移） | 待更新（未跟踪） |

## 七、MCP / eBPF / 工具（入口）

| 文档 | 一句话说明 | 状态 |
| --- | --- | --- |
| [mcp/docs/architecture.md](../mcp/docs/architecture.md) | MCP 分层架构、`go2cpp::mcp` 命名空间、各层目录与依赖（MCP 唯一文档） | 现行 |
| `mcp/examples/agent_demo.cpp` | 最小 MCP agent 示例（非文档） | 现行 |
| `bpf/*.bpf.c` | 6 个 eBPF 程序：sched/futex/syscall/net/tcp/fileio 延迟与统计 | 缺口：无使用/设计文档 |

> MCP 构建：`-DGO2CPP_WITH_MCP=ON`（默认 OFF），产物 `mcp_server`；本地配置
> `mcp/mcp.ini` 不入库，模板为 `mcp/mcp.ini.example`。

---

## 八、附录：状态机文档（19）

位置 `docs/state-machines/`，描述对象生命周期与状态迁移，属于实现契约的补充。

| 文档 | 对象 |
| --- | --- |
| [BufferedSink.md](state-machines/BufferedSink.md) | 日志批量缓冲 |
| [Channel.md](state-machines/Channel.md) | 类型化 Channel |
| [Config.md](state-machines/Config.md) | 配置热加载事务 |
| [Context.md](state-machines/Context.md) | Context 取消树 |
| [CsvReader.md](state-machines/CsvReader.md) | CSV 读取器 |
| [EventBatch.md](state-machines/EventBatch.md) | 事件批次 |
| [File.md](state-machines/File.md) | `os::File` |
| [Histogram.md](state-machines/Histogram.md) | 指标直方图 |
| [Http2Connection.md](state-machines/Http2Connection.md) | HTTP/2 连接层 |
| [Http2StreamState.md](state-machines/Http2StreamState.md) | HTTP/2 流状态机 |
| [JsonDepthScanner.md](state-machines/JsonDepthScanner.md) | JSON 深度扫描器 |
| [Panic.md](state-machines/Panic.md) | panic 状态 |
| [ReaderEngine.md](state-machines/ReaderEngine.md) | 读取引擎 |
| [Reader.md](state-machines/Reader.md) | 缓冲读取器 |
| [Scanner.md](state-machines/Scanner.md) | 扫描器 |
| [SoReloader.md](state-machines/SoReloader.md) | SO 热重载器 |
| [Stream.md](state-machines/Stream.md) | SSE 服务端流 |
| [WebSocket.md](state-machines/WebSocket.md) | `net::WebSocket` |
| [WriterEngine.md](state-machines/WriterEngine.md) | 写入引擎 |

## 九、附录：review 问题库（10）

位置 `docs/review/`，与 `code-review-guide.md` 互补：导读回答“怎么读”，本目录回答“读出了什么”。
**整个目录当前未被 git 跟踪**（`?? docs/review/`），引用前注意基线为 `bd99603`。

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| [README.md](review/README.md) | 索引、状态图例、维护规则 | 未跟踪 |
| [_TEMPLATE.md](review/_TEMPLATE.md) | 单模块问题文档模板 | 未跟踪 |
| [00-overview.md](review/00-overview.md) | 模块阅读顺序与依赖摘要 | 未跟踪 |
| [00-concurrency-policy.md](review/00-concurrency-policy.md) | 全局并发安全政策（默认 `sync::Mutex`，禁裸 `std::mutex`） | 未跟踪 |
| [01-error.md](review/01-error.md) | error 模块问题记录（含与 Go error 生态差距） | 未跟踪 |
| [02-config.md](review/02-config.md) | config 模块问题记录（**暂停，待专项审查**） | 未跟踪 |
| [03-log.md](review/03-log.md) | log 模块问题记录（4 条已确认问题） | 未跟踪 |
| [04-context.md](review/04-context.md) | context 模块问题记录（待读） | 未跟踪 |
| [05-default-value-drift.md](review/05-default-value-drift.md) | 默认配置漂移（文档一致性）问题 | 未跟踪 |
| [06-fiber.md](review/06-fiber.md) | fiber 状态机与成员变量跟踪（专项审查待办） | 未跟踪 |

---

## 十、重复 / 过时 / 孤立清单

### 重复与重叠

1. **性能基线有四份**：[performance-baseline-2026-10-19.md](performance-baseline-2026-10-19.md) 是唯一有效基线；
   [performance.md](performance.md) 是 2026-10-21 汇总；`gmp-optimization-log.md`、`gmp-perf-analysis.md`、
   `gmp-vs-thread-epoll-2026-10-18.md`、`coost-comparison.md` 共用同一批已作废旧基准。
2. **交接文档四份**：`HANDOFF.md`(10-15) → `AGENT-HANDOFF.md`(10-19) → `NEXT-STEPS.md`(10-18)、
   `PROGRESS-ROUND2.md`(10-18)，内容互相覆盖，只剩 `AGENT-HANDOFF.md` 有方法论价值。
3. **注释进度两份**：`annotation-progress.md` 与 `function-annotation-progress.md` 主题重叠、均已停止。
4. **SO 热重载四份**：`so-reload-design/scenarios/usage/progress` 分工清晰，不算重复；
   但 `so-reload-design.md` 与 `so-reload-scenarios.md` 在“17 个场景”上有重复叙述。
5. **审计系列**：`concurrency_audit`、`module-audit`、`state_machine_audit`、`bug-report` 是不同时间/范围的快照，
   不是同一份文档的重复；只应按日期引用。

### 过时（数字/结论不再代表现状）

- `gmp-optimization-log.md`、`gmp-perf-analysis.md`、`gmp-use-cases.md`、`gmp-vs-thread-epoll-2026-10-18.md`、
  `coost-comparison.md`：旧 epoll 池模型不公平，作者已自标结论作废。
- `test-audit-2026-10-21.md` 0–8 节、`NEXT-STEPS.md`、`AGENT-HANDOFF.md`、`annotation-progress.md`、
  `function-annotation-progress.md`、`so-reload-progress.md`：写死 113/114/115/130/135 等 ctest 计数，
  当前构建为 14（见下）。
- `modules.md`：原写“13 个模块 target”，本次已修正为 24（含 `math/rand/path/container/compress/encoding/hash/metrics/debug`）；第 1 节表格仍只列 7 个目录。
- `testing.md`：路径约定与 130/183 等数字跨多次重构，引用前必须按当前树核对。
- `code-review-guide.md`：基线 `bd99603`，行号已漂移，且文件未跟踪。
- `trace_format.md`：设计未落地，源码无 trace 实现。

### 孤立（零引用，没有任何其他文档链接）

按引用扫描（排除自身）共 14 篇零引用：
`channel-hang.md`、`concurrency-5rounds.md`、`concurrency_audit_2026-10-18.md`、
`concurrency-park-points.md`、`debug-overhead.md`、`development-directions.md`、`fiber_guide.md`、
`integration-test.md`、`persistence-benchmark.md`、`PROGRESS-ROUND2.md`、`quality_backlog.md`、
`timer_design_review.md`、`timer_usage.md`、`trace_format.md`。

其中 `fiber_guide.md`、`timer_usage.md`、`concurrency-park-points.md`、`debug-overhead.md`、
`channel-hang.md`、`quality_backlog.md` 是现行有效文档，本索引已把它们接到入口，不再孤立；
其余为历史快照，保留原样即可。

### 缺口

- ~~`mcp/` 没有 README，`mcp/docs/architecture.md` 顶部链接 `../README.md` 断链（全仓唯一断链）。~~
  **2026-10-26 已修复**：新增 `mcp/README.md`（指向 `docs/modules/mcp.md`）。
- `bpf/` 6 个 eBPF 程序没有使用/设计文档，只在 `cmake/Go2CppMonitor.cmake` 注释里有说明。
- ~~`go2cpp::os` 无独立文档~~：**2026-10-26 已补** `docs/modules/os.md`；接入说明仍在 `modules.md`。

## 维护约定

1. 新增文档：先放对应主题表格，写一句说明，标状态；不要新建第二份索引。
2. 改代码导致数字/路径变化：优先改文档中的“当前值”，历史快照保留并在状态列标“历史”。
3. 状态为“历史”的文档不再更新内容；需要新结论时新建文档或更新“现行”文档。
4. 文档之间用相对链接（本目录内直接文件名），跨目录用 `../`，避免绝对路径。

---

## 模块文档（docs/modules/）

单个模块的集中文档：API 表、设计（带 `file:line` 引用）、边界与测试数据。本目录自
2026-10-26 起建立，按模块拆分；每篇标注适用 HEAD 与源码位置，是「现行」文档。

| 文档 | 一句话说明 | 状态 |
| --- | --- | --- |
| [modules/channel.md](modules/channel.md) | Channel/Select：cap0 直接交接、select 单赢家与 cancel_probe、park/wake、关闭语义、13.4 亿条压测 | 现行 |
| [modules/sync.md](modules/sync.md) | sync：Mutex 正常/饥饿两态与令牌交接、RWMutex/Recursive/Timed/Spin、CV/WaitGroup/PC、性能数据 | 现行 |
| [modules/scheduler.md](modules/scheduler.md) | scheduler（GMP）：P/M/G 与状态机、四级取任务与 work stealing、park/wake 协议、sysmon/BlockingRegion、shutdown 与回收、已证伪清单 | 现行 |
| [modules/fiber.md](modules/fiber.md) | fiber：growable 栈与 SIGSEGV 增长、上下文切换汇编、FiberBin/FiberRecord、两种迁移、切换/内存数据 | 现行 |
| [modules/os.md](modules/os.md) | os：Process/Spawn/Run、G2CF 通道、SPSC 环、mq/pty、Supervisor、ProcessPool、IoCapability；R1–R31 覆盖与迁移前压测数据 | 现行 |
| [modules/mcp.md](modules/mcp.md) | mcp：六层框架、JSON-RPC 分发、事件驱动增量索引、SSE 多会话、插件通知、29 工具与 e2e 数据 | 现行 |
| [modules/timer.md](modules/timer.md) | timer：Timer/TimerService、multimap+反向索引（不是最小堆/时间轮）、arm/fire/cancel、TimerDecision 周期语义、与 Scheduler 的 TimerExecutor 解耦、边界与实测数据 | 现行 |
| [modules/io.md](modules/io.md) | io（epoll）：IOManager 64 条带 + EPOLLET 重 arm、poller 线程与 worker 自 poll 双驱动、fd 代际、park/wake 与 Hook 集成、边界与测试 | 现行 |
| [modules/tls.md](modules/tls.md) | tls：OpenSSL 客户端 Transport、TLS 1.2+ 握手、证书/SNI/主机名校验、ALPN=h2、close_notify 与 SIGPIPE、边界与 TLS/ALPN 测试 | 现行 |
| [modules/ws.md](modules/ws.md) | ws：RFC 6455 握手、帧格式/掩码/分片、Ping/Pong、Close code、读写并发与超时、状态机与测试 | 现行 |
| [modules/sse.md](modules/sse.md) | sse：独立模块；Event 编码、增量 Parser（BOM/CRLF/CR/finish）、chunked Stream、心跳/取消/背压、重连边界与测试 | 现行 |
| [modules/network.md](modules/network.md) | network 总览：分层/构建目标/Hook 22 个操作/HTTP-2 核心边界/QUIC-默认 OFF | 现行 |
| [modules/tools.md](modules/tools.md) | tools 总览：CLI 7 子命令、迁移（仅 v3）、回放（G2CRPLY v5）、持久化原型 | 现行 |
| [modules/persistence.md](modules/persistence.md) | 持久化原型：MPSC+批量+fdatasync、段/manifest、2.96x/13.83x | 现行 |
| [modules/bento-scenarios.md](modules/bento-scenarios.md) | Bento 场景矩阵（JinShan 66 条）+ P0/P1 落地记录 + §21 app 层（fetch/zip/backup/file_browser/capability/session）与 udp_transport/x25519 | 现行 |
| [modules/bento-transport.md](modules/bento-transport.md) | Bento 传输组：ITransport 契约（deadline/半关闭/部分写/幂等关闭）、TcpTransport/MemoryTransport、20B 通用帧与 CRC32、心跳、指数退避、有界 relay；含边界与测试数据 | 现行 |
| [modules/bento-security.md](modules/bento-security.md) | Bento 安全组：ChaCha20-Poly1305 AEAD、重放窗口、X25519 ECDH（已接线，`afc135a`）、keystore、PSK 握手与前向保密 | 现行 |
| [modules/bento-session.md](modules/bento-session.md) | Bento 会话组：pairing/session_table/session_control/peer_hub/app::Session，以及与 `tests/bento/PROTOCOL.md` §12 的逐项对齐表 | 现行 |
| [modules/bento-app.md](modules/bento-app.md) | Bento app 组：fetch/file_browser/backup/zip/capability/command/file_transfer；沙箱、校验与已知缺陷（D-05/D-06） | 现行 |
| [modules/bento-foundation.md](modules/bento-foundation.md) | Bento 地基组：error/registry/mapping、reflect、udp_transport、keystore 错误码补充 | 现行 |

> **基础库**（bufio/compress/container/encoding/hash/math/path）：实现简单、API 稳定，
> 直接看 `include/go2cpp/<module>/` 头文件注释；无独立模块文档。
| [modules/http1.md](modules/http1.md) | http1：HTTP/1.x 消息与增量解析、严格 chunked/trailer、全局 deadline 客户端、keep-alive 服务端、chunked Writer、边界与测试 | 现行 |
| [modules/http2.md](modules/http2.md) | http2：HPACK/Huffman、10 种帧、7 状态流机、CONTINUATION/双闸/GOAWAY/MAX_CONCURRENT、流控、Transport 与 ctest 数据 | 现行 |
| [modules/debug.md](modules/debug.md) | debug：事件环 per-thread id + ts 排序、双段关键区、资源历史 10 类结构（8 插桩/2 边界）、6 基础视图 + 4 资源视图、DebugServer 只读协议、Doctor 白名单/等待图/死锁判定；合并 3 篇旧文档 | 现行 |
| [modules/logging.md](modules/logging.md) | logging：Level/LogRecord、Filter→Formatter→Sink 四层、BufferedSink 两把锁与惰性刷新、File/Rotating/Stdout/Memory/Callback、热加载与边界 | 现行 |
| [modules/metrics.md](modules/metrics.md) | metrics：Counter/Gauge/Histogram、标签族缓存、固定桶与分位插值、Prometheus 文本导出、注册冲突与标签基数边界 | 现行 |
| [modules/config.md](modules/config.md) | config：INI 语法与 `\xHH` 往返、ConfigVar 两阶段事务、Config 整批热加载、`.lock` 协议、RuntimeConfig 默认值/校验、`GO2CPP_CONFIG_REGISTER`、SO manifest 层 | 现行 |

> 合并说明：`channel-hang.md`、`concurrency-5rounds.md`、`concurrency-baseline.md`、
> `concurrency-park-points.md`、`correctness_audit.md` 已在顶部标注合并去向，正文保留为
> 历史快照。其他模块文档按同一约定放入本目录，并在本表登记。
>
> `scheduler`/`fiber` 的合并：`scheduler.md`、`gmp-optimization-log.md`、`gmp-perf-analysis.md`、
> `gmp-scenario-analysis.md`、`gmp-use-cases.md`、`concurrency_audit_2026-10-18.md` 已并入
> `modules/scheduler.md`；`fiber_guide.md`、`fiber-growable-stack-design.md`、
> `fiber-migration.md`、`coost-comparison.md` 已并入 `modules/fiber.md`。

> `timer`/`io` 的合并：`timer_design_review.md`、`timer_usage.md`、`timer_benchmark.md`
> 已并入 `modules/timer.md`；`gmp-vs-thread-epoll-2026-10-18.md` 的 IO 结论与
> `network.md` 的 IOManager 细节已并入 `modules/io.md`（`network.md` 仅部分合并，
> Socket/WebSocket/SSE 等章节仍是现行说明）。

## 调研文档（补充）

- [socket-debug-mode](research/socket-debug-mode.md)：Socket Debug 模式调研（方案 D：自持计数 + 事件复用 + 第 11 视图）
