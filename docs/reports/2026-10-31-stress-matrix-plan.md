# 并发结构重压测正确性验证（用户 10-31 要求）

> 时间点：并发部分（Cond/RW/Context/Select/M 亲和/MainFiber）完成后立即执行。

## 要求

1. **每个并发结构**都要验证（不是只 Channel）：
   - Mutex、ConditionVariable、WaitGroup、RWMutex、SpinMutex
   - Channel、FastChannel（MPMC/SPSC）、Select
   - WaitNode/ParkingCondition、Scheduler park/wake、Fiber 切换
   - Context（取消/超时）、Error（Fiber-local）、Future
2. **每个结构 ≥5 个「很可能出问题的」复杂场景**，包括但不限于：
   - **Context 取消**（操作中途 Deadline/Cancel 打断）
   - **网络通信**（与 Socket/HTTP/Channel 混合）
   - **线程 Fiber 混合高并发**（f2f/f2t/t2f/t2t 四组合 + 多线程多 Fiber）
   - **关闭/析构竞态**（Close 与操作并发、析构与等待并发）
   - **嵌套/重入**（锁嵌套、Channel 嵌套、select 嵌套）
3. **每个场景 10^8 次操作，跑 3 轮**（不是 1 轮）。
4. 断言必须真实（计数/顺序/无丢失/无重复/无死锁/watchdog/RSS 稳定）。

## 场景矩阵（每结构 5 场景）

| # | 场景 | 说明 |
|---|---|---|
| S1 | **Context 取消** | 操作中途 Cancel/Deadline 打断；取消后状态一致（无泄漏/无残留/下次可用） |
| S2 | **网络通信** | 与 Socket/HTTP 混合：Channel 传帧、Mutex 保护连接表、CV 等 IO 事件 |
| S3 | **线程 Fiber 混合高并发** | f2f/f2t/t2f/t2t 各 10^8；多线程 × 多 Fiber；Scheduler 多 P |
| S4 | **关闭/析构竞态** | Close 与 Send/Recv/Wait 并发；析构与等待并发；不崩溃/不挂死/无 UAF |
| S5 | **嵌套/重入** | 锁嵌套（A→B）、Channel 嵌套（channel of channel）、select 嵌套（多层 select） |

## 执行方式

- 每个结构：5 场景 × 10^8 × 3 轮；
- 预计总量：~15 结构 × 5 × 3 = 225 组 × 10^8 ≈ 2.25×10^10 次操作；
- 时间：需分批跑（每组 10^8 约几十秒到几分钟）；
- 工具：`taskset` 绑核、记录 loadavg、watchdog（180s 无进展即失败）、RSS 监控；
- 失败即记录（场景/轮次/计数/栈）。

## 产出

- `docs/reports/2026-10-31-stress-matrix.md`（每结构 × 5 场景 × 3 轮的结果表）；
- 失败项进 backlog（含复现命令）。
