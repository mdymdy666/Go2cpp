# G10 · 全项目汇总（模块报告收口）

> 本组是收口：把 G1~G9 的结论压成一页，并给出「图 → 报告 → 源码」三者的对应关系。
> 交付物：`../00-architecture.svg`（主图，含图内文字）、`../01-runtime-flow.md`（流程说明）、
> `../notes/nodes-edges.md`（图的数据模型）、`G1~G9-*.md`（分组模块报告，合计 2650 行）。

---

## 1. 一句话架构

> **Go2Cpp 用「固定栈 Fiber + GMP 调度 + 显式 park/wake 协议」复刻 Go 的 goroutine 语义，
> 用「Hook 拦截 libc + 独立 epoll poller 线程」复刻 netpoller，用「取消树 + 不可变错误图」复刻
> context/errors，并在其上叠一层「Go 风格新手 API」让转译代码写起来接近 Go；
> 它明确不提供 GC、异步抢占、自动栈增长、TLS 与 cgo。**

## 2. 模块清单与报告索引（27 个公开模块 → 9 组报告）

| 层 | 模块（公开头名） | 报告 | 规模（头+实现，行） |
|---|---|---|---|
| L0/L1 | `detail/context_backend`、`fiber`、`fiber_local`、`thread_policy`、`core/timer`、`core/parking_condition`、`core/hybrid_mutex`、`core/status` | **G1**（+G9 覆盖 fiber_local/thread_policy/hybrid_mutex/status） | 89 / 1502 / 183 / 202 / 1122 / 435 / 114 / 20 |
| L2 | `scheduler` | **G2** | 662 + 4048 = 4710 |
| L3 | `context`、`error` | **G3** | 505+2412 / 251+478 |
| L4 | `channel`、`future`、`sync` | **G4** | 2007+402 / 952+175 / 219+1324 |
| L5 | `io`、`io_backend`、`fd_manager`、`hook` | **G5** | 314+2642 / 87+228 / 69+72 / 102+2161 |
| L6 | `socket`、`socket_stream`、`address`、`bytearray`、`framing`、`endian`/`myendian` | **G6** | 225+568 / 153+280 / 94+196 / 129 / 80 |
| L7 | `http`、`websocket`、`sse` | **G7** | 246+620 / 176+715 / 205+613 |
| L8 | `control_flow`、`go`、`event`、`runtime` | **G9** | 241+346 / 220 / 424 / 36 |
| L9 | `log`、`config` | **G8** | 506+1024 / 541+1095 |

（合计约 32,000 行；最重的三个实现文件：`scheduler.cpp` 4048、`io.cpp` 2642、`context.cpp` 2412。）

## 3. 关键实测数字（可复现）

| 项 | 值 | 来源 |
|---|---|---|
| Fiber 默认栈 | 128 KiB（固定不增长） | `Fiber::DefaultStackSize()` |
| 128 KiB 栈可承受递归深度 | 帧 0 → **1021** 层；帧 256 B → **339** 层 | 本轮 `fiber_stack_limit_full` 场景 1 |
| 8 MiB pthread 对照 | **65496** 层 | 同上 |
| 栈溢出后果 | SIGSEGV → 进程立即死亡（无 handler/sigaltstack） | `grep` 全项目无 sigaction |
| G/M/P 编号起点 | G/M 从 **1**（0 = 非 worker 哨兵）；P 从 **0** | `scheduler.cpp` `s_next_g_id{1}`/`s_next_m_id{1}` |
| P 数默认 | `processor_count==0` → `hardware_concurrency()` | `SchedulerConfig` |
| incoming 条带 / 本地队列上限 / FiberBin | 16 / 256 / 32（上限 4096） | `scheduler.cpp` |
| Hook 拦截函数数 | **31** 个 libc 函数 | `src/network/hook.cpp` `Originals` |
| nlohmann 深嵌套临界（128 KiB 栈） | 解析 ≥524288 层；`dump()` 1162/609；拷贝 816/185（Release/无优化） | 本轮 fork 隔离实测，见 codex.md 第 25 轮 |

## 4. 横切差异总表（Go → Go2Cpp）

| 维度 | Go | Go2Cpp | 影响 |
|---|---|---|---|
| 栈 | 自动增长（2 KiB→1 GiB） | **固定 128 KiB**，越界即进程死亡 | 深递归必须改迭代或换 Fiber（手动等价物） |
| 抢占 | 异步抢占（信号/协作点） | **纯协作**（park/yield/suspend 点） | 长 CPU 循环会占住 M |
| GC | 三色标记 + 写屏障 | `shared_ptr` + RAII + 显式所有权 | 无环检测（用 `weak_ptr` 手工打破） |
| 阻塞感知 | runtime 自动感知 syscall | **必须 Hook 或显式 `BlockingRegion`** | 未声明的阻塞调用调度器看不见 |
| 取消 | `context.Done()` channel | `DoneSignal`（可复制、可无限次等）+ `ContextRollback` 补偿事务 | 多出 `RollbackDone` 这一独立信号 |
| 错误 | `error` 接口 + `errors.Is/As` | `ErrorPtr` + `Wrap`（**Message 是整条链**）+ `Is` 身份比较 | 不能拿文本判断层级 |
| channel | 语言内建语法 | `Channel<T>` + `Select`（内建 case 只接受可复制 T） | move-only 值需走普通 Send/Recv 或 `SelectValue` |
| select 公平性 | 伪随机 | **轮转** cursor | — |
| panic/recover | 栈展开 | **显式状态协议**（无展开） | 必须手工在 defer 回调里 recover |
| defer | 函数级 | **作用域级** RAII | 循环里每次都注册一个 |
| IO | netpoller + `net.Conn` | epoll readiness + `WaitStatus`/errno + Hook 拦截 | 调用方需自己重试 syscall |
| TLS/HTTP2 | 标准库内建 | **有**（`go2cpp::net::http2`：HPACK/帧层/流状态机/连接层/TLS+ALPN）| 见 G11 与 `../docs/network.md` |
| 调度器调参 | `GOMAXPROCS()` 运行期 | 启动期配置（`SchedulerConfig::processor_count`） | 不能热改结构参数 |

## 5. 已知缺陷与台账（来自 `docs/quality_backlog.md` + 本轮复核）

| 编号 | 摘要 | 严重度 | 本轮状态 |
|---|---|---|---|
| X-25 | 13 处 `noexcept` 查询接口内部加锁（锁失败即 `terminate`） | 中 | 未修（ContextRollback 三处已兜底） |
| X-31 | 8 个进程级错误单例依赖函数局部 static；退出阶段 `Is(err,{})` 会退化 | 低 | 未修（`go.hpp` 的调度器已用 `new` 规避） |
| X-35 | **用户决策：框架不允许 `HybridGate`**，需替换为 `sync::Mutex`（4 处使用点） | 中 | 未执行（本轮仅记录定位与约束） |
| X-30 | `panic/recover` 边界是 `thread_local` 深度（跨 Fiber 会串） | 低（文档口径） | 已在 `compatibility.md` 声明为调用方约束 |
| X-10 | `wake_for_io` 与 `wake_registered`/`wake_or_cancel` 对 `kPending` 返回值语义相反 | 中 | 本轮已写进 G2 报告 |
| X-27 | `EventBatch` 写方法在加锁失败时静默返回 | 低 | 本轮已写进 G9 报告 |
| X-12 | `maintenance_budget` 被扩容与回收共用（实测内存影响为噪声） | 中 | 保持登记 |
| X-33 | `HybridGate` 名中的「混合」不是线程/Fiber 混合 | 低-中 | 本轮已写进 G4/G9 报告 |

## 6. 怎么读这套交付物

```text
① 先看图：ArchitectureDiagram/00-architecture.svg
     —— 分层（L0 底 → L9 顶）、关键依赖边、底部 4 块图内文字（运行流程 / 协议 / 数字 / 边界）
② 再看流程：ArchitectureDiagram/01-runtime-flow.md
     —— 一次 go() 的六阶段（提交→取任务→切入 Fiber→等待→唤醒→完成）+ 取消/IO/错误三条横向流
③ 需要细节时查分组报告：ArchitectureDiagram/modules/G1~G10-*.md
     每份都含：职责与边界 / 符号清单 / 机制 / 不变量 / 坑点 / 与 Go 差异 / 对架构图的贡献
④ 图的数据模型（节点/边/文字块的原始清单）：ArchitectureDiagram/notes/nodes-edges.md
⑤ 重新生成图：dot -Tsvg 00-architecture.dot -o 00-architecture.svg（Graphviz 2.43+，中文字体 Droid Sans Fallback）
```

## 7. 覆盖面声明（诚实边界）

- 本汇总基于**符号级实读**（头文件签名）+ **实测**（fork 隔离探深、nlohmann 深嵌套临界、构建与 ctest），
  未做逐行通读的实现文件：`scheduler.cpp`(4048)、`io.cpp`(2642)、`hook.cpp`(2161)、`context.cpp`(2412)、
  `channel.hpp`(2007)——这些通过子代理结构化深读 + 关键符号复核覆盖。
- `context.cpp` 内约 71 处模板化注释与实际代码不符（已在 G3 声明以代码为准）。
- 未验证项：`wait_many` 极端补充采样的公平性、Hook 在 `-Bsymbolic` 构建下的自绑定、TSan 未在 WSL 通过。
