# Go runq 三函数借鉴：runqput / runqsteal / findRunnable

- **Go 版本**：`go1.27.1`（`/usr/local/go/VERSION` 首行；time 2026-08-28T16:20:06Z）
- **源码**：`/usr/local/go/src/runtime/proc.go`
- **方法**：只读 3 条路径 —— `runqput`(7529-7575)、`runqsteal`(7781-7795) 及其调用的 `runqgrab`(7713-7770)、`findRunnable`(3404 起) 的取任务顺序（3460 / 3484 / 3511 / 3899）；其余未读。
- **对照**：WSL 真树 `/UserData/CodexWorkSpace/Go2cpp/src/concurrency/scheduler.cpp`（未使用 E: 旧副本）。
- **口径**：预期收益只给定性判断，不写性能数字结论、不引用 docs 历史数据；本轮未跑基准。

## 三条差异

| # | Go 位置 | Go 做法（一句话） | 本项目位置 | 差异 | 可执行改动（一条动作） | 预期收益 | 风险 |
|---|---|---|---|---|---|---|---|
| 1 | proc.go:7529 `runqput` | runnext 是 CAS 单槽；被挤出的旧 G **无条件**踢回普通 runq 队尾继续入队 | scheduler.cpp:4015 `publish_runnext`（displaced 分支 4031-4033） | 本项目本地环满时 `clear_queued()+enqueue()`，走 admission 全局锁且可能轮转到别的 P | 环满分支改由「本 P overflow 槽 / 本 P incoming 条带」承接 displaced，不调 enqueue | 定性：减少唤醒热路径的全局锁争用与 P 漂移；量级不确定 | runnable 计数与 shutdown 边界；环满概率低，收益可能不显 |
| 2 | proc.go:7781 `runqsteal` + 7713 `runqgrab` | 偷一半 `n - n/2`；`stealRunNextG` 为真时允许偷 victim 的 runnext（先 3µs 退避） | scheduler.cpp:5859 `victim.queue.steal(...)`；5866 注释「不偷别的 P 的 runnext」 | Go 可由调用方开启偷 runnext；本项目硬编码永不偷 | 给 steal 段加 `steal_run_next` 开关，仅「本 P 环空 + incoming 空 + 空转≥1 轮」时允许，偷前先 `osyield` | 定性：跨 P 唤醒尾延迟可能改善；不确定 | 破坏缓存亲和（本项目注释称无条件偷会让跨 M 恢复率大幅上升），必须实测 |
| 3 | proc.go:3484 / 3511 / 3899 `findRunnable` | 本地 runqget → **非阻塞 netpoll(0)** → runqsteal；netpoll 有 pollingNet 单线程门控 | scheduler.cpp:5768 step4（全局+偷）→ 6043 step5 执行 → 6278 无 G 才 netpoll 阻塞 | Go 在偷之前先查一次 IO；本项目要等完全无 G 才进 IO 等待 | step4 前插一次 `netpoll(0)` 非阻塞探测 + 单线程门控（CAS 抢门），命中就地 claim | 定性：IO 就绪 G 不必等 M 入睡，改善尾延迟 | 多 M 同时 poll 的内核争用；空转路径多一次 syscall |

## 原文核对（sed -n 实测）

**1) runqput 的 runnext 单槽**
```
7529: func runqput(pp *p, gp *g, next bool) {
7547:   oldnext := pp.runnext
7548:   if !pp.runnext.cas(oldnext, guintptr(unsafe.Pointer(gp))) {
7555:   gp = oldnext.ptr()   // Kick the old runnext out to the regular run queue.
```
本项目：
```
4015: bool Scheduler::publish_runnext(PId processor_id,
4031:   if (!processor.queue.push(displaced)) {
4033:       (void)enqueue(displaced);
```

**2) runqsteal / runqgrab：偷一半与 stealRunNextG**
```
7781: func runqsteal(pp, p2 *p, stealRunNextG bool) *g {
7718:   n = n - n/2
7722:   if next := pp.runnext; next != 0 {
7746:       usleep(3)
```
本项目：
```
5859: stolen_count = victim.queue.steal(stolen, want);
5866: // 不偷别的 P 的 runnext：它是「最近唤醒」的快速槽，
```

**3) findRunnable 取任务顺序**
```
3460: gp := globrunqget()          // 每 61 tick 一次
3484: if gp, inheritTime := runqget(pp); gp != nil {
3511: list, delta := netpoll(0)    // 偷之前先非阻塞查 IO
3899: if gp := runqsteal(pp, p2, stealTimersOrRunNextG); gp != nil {
```
本项目：
```
5768: // step4: 慢路径——本地与 incoming 都空，进入全局 admission 锁。
6278: // Go runtime.netpoll 模型：没有 G 可跑时，M 自己进入 IO 后端等待。
```

## Top-1 建议

**选第 3 条（netpoll 前移）**。理由：改动局部、Go 有现成门控语义可抄、不触碰 runnext 与偷取的设计契约；第 1 条收益依赖环满概率，第 2 条已知会破坏亲和。

下一轮 agent 可执行的第一步：
1. `grep -n "netpoll" src/concurrency/*.cpp src/concurrency/*.h` 找到 IO 后端，确认是否已有 0 超时非阻塞 poll 接口。
2. 在 `scheduler.cpp` step4 前加 `probe_io_nonblocking()`（无接口则先补一个返回就绪数的 epoll/io_uring 0 超时版本）。
3. 加 `std::atomic<int> polling_io` 门控（CAS 抢门），开关默认关，先只测 IO 密集场景的 p99。
4. 不改 runnext / steal 段；先看事件计数 `metric_io_probe_hits`，再决定是否保留。

## 需实测的判断

- 第 1 条：displaced 在本地环满时走 enqueue 的频率（大概率接近 0，需统计确认）。
- 第 2 条：限制条件（空转≥1 轮）下偷 runnext 对跨 M 恢复率与 p99 的实际影响——项目注释已有相反证据，必须实测。
- 第 3 条：非阻塞 netpoll 前移是否真的减少 IO 唤醒延迟，以及多 M 下 poll 争用成本。
