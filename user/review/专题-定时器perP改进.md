# 专题改进：TimerService 改成 Go 的 per-P 模式

> 来源：2026-11-05 审查（用户提出「作为专题改进，使用 go 的模式来写代码」）。
> 现状：`src/concurrency/timer.cpp` 用 **单线程 + 全局 `std::multimap`**（`timer.cpp:283`），
> 所有业务定时器由一个后台线程串行处理。
> 问题：**海量业务定时器时单线程成为瓶颈**（每秒 10 万回调会积压）。
> 参照：Go 的 `runtime/time.go` —— **每个 P 一个最小堆**（`type timers struct` / `timerHeaped`）。

---

## 一、Go 的模式（源码依据）

```go
// /usr/local/go/src/runtime/time.go:136
type timers struct {
    // ...
    heap []*timer          // 每个 P 一个最小堆
}
// timerHeaped (:206) 标记「已入某个 P 的堆」
// addtimer / siftupTimer / siftdownTimer 维护
```

**关键点**：
1. **per-P**：每个 P 有自己的 timer 堆，插入/删除只在本地 P 的锁内（无全局竞争）；
2. **谁执行回调**：`timerproc` 在 P 上执行（不是独立线程）——回调天然并行；
3. **窃取/迁移**：P 销毁时 timer 迁移到其他 P；
4. **IO deadline 另算**：`pollDesc` 自带 `rt`/`wt` timer（`netpoll.go:109`），不走 timer 堆。

## 二、go2cpp 的改造方案

### 目标

| 项 | 现状 | 目标 |
|---|---|---|
| 存储 | 全局 `multimap` + `m_mutex` | **per-P `multimap`（或最小堆）** |
| 执行 | 单后台线程 | **在 P 的 worker 上执行**（或每 P 一个轻量线程） |
| 回调并发 | 串行 | 随 P 数并行 |
| 跨 P 迁移 | — | P 销毁/负载均衡时迁移 |

### 分阶段（避免一次改太大）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P1（本次）** | 把全局 `multimap` 拆成 **per-P 的 `multimap`**；`Schedule` 按「当前线程的 P id」或 hash 落桶；每个分片一个 timer 线程 | 功能不变、单线程瓶颈消除、`ctest` 全过 |
| **P2** | 复用 worker 做 timer 检查（而非每分片一个线程）；P 销毁时迁移 | 100 万 timer 插入/删除压测、回调并发度 |
| **P3** | IO deadline 保持现状（搭车 epoll）——**不要并入 timer 堆** | — |

## 三、必须保留的现有正确性

1. **`m_generation` 机制**（`timer.cpp:283` 注释）——插入/删除/重置时递增，后台线程据此重算等待时长，**不依赖 notify 时序**（少一次丢唤醒机会）；
2. **回调在锁外执行**——回调里可再操作定时器不死锁；
3. **析构时若在后台线程上直接 terminate**——防悬空；
4. **回调异常交给 `m_exception_handler`**——不让线程被打死；
5. **`Cancel` 的 O(1) 反向索引**——per-P 后反向索引也要按 P 分片。

## 四、风险

| 风险 | 缓解 |
|---|---|
| per-P 后「跨 P 的 timer 取消」需要找到正确的 P | `Timer::State` 记录所属分片 id；取消时按 id 路由 |
| P 销毁时 timer 丢失 | 迁移到「当前 P」或「默认 P」 |
| 回调并发带来线程安全问题 | 回调契约本来就是「可能在任意线程执行」——保持 |
| 测试覆盖 | 现有 `test_timer*.cpp` 必须全过 + 新增并发压测 |

## 五、实施清单

- [ ] 读 `timer.cpp` 的 `Impl` 全部（1343 行）——理解 `m_entries`/`m_generation`/`Cancel`/`Run`
- [ ] 设计 per-P 数据结构（`std::vector<PerPTimer>`，每个含 `multimap` + `mutex` + `cv`）
- [ ] `Schedule` 路由到分片（按当前线程 id 或 round-robin）
- [ ] 每个分片的 timer 执行：P1 先每分片一个线程，P2 再复用 worker
- [ ] `Cancel` 按 `State::owner` 路由
- [ ] 迁移（分片销毁时）
- [ ] 测试：功能 + 100 万 timer 压测 + 并发回调
- [ ] 性能对比：现状 vs per-P（插入/删除吞吐、回调延迟 p99）

## 六、与「丢唤醒专项」的关系

`TimerService::Run()` 的 `cv.wait_until` 是丢唤醒排查清单里的 **P0 第 4 项**——本次改造要**同时**检查：
- `m_generation` 递增与 `cv` 通知是否同锁（当前看是 `m_generation.load()` + `wait_until` 谓词，需确认）；
- per-P 后每个分片的 `cv` 同样要遵守「置位 + notify 同锁」。

## 七、完成后的流程（三件套 + 正确性审查）

per-P 改造（P1）落地后，**必须按三件套顺序验收**，不能只看「功能测试过」：

### 三件套

| # | 阶段 | 验收标准 |
|---|---|---|
| **① 架构** | per-P 分片已落地 | 分片数、路由策略、`Cancel` 路由、迁移策略都有明确文档；单线程瓶颈消除（100 万 timer 压测） |
| **② 汇编** | `objdump` 取证热函数 | 至少取证：`Schedule`（插入路径）、`Run`（等待/弹出）、`Cancel`（查找）；记录指令数/call 数/分配数/原子数；**前后对比**（改造前 vs 改造后） |
| **③ 正确性** | 全量验证 | 现有 `test_timer` 系列全过；新增「并发到期 / 跨分片取消 / 分片销毁迁移」用例；**ASan/UBSan/TSan 三套 0 报告**；`ctest -R timer` 全绿 |

### 正确性审查（三件套之后）

按 `docs/plans/lost-wakeup-plan.md` 的 4 步法审查新代码：

- [ ] 每个分片的 `cv`：谓词 atomic/同锁 + while；**置位 + notify 同锁**；
- [ ] `m_generation` 在分片后是否仍然有效（每分片一个 generation？）；
- [ ] `Cancel` 路由到错误分片的后果（返回失败 vs 静默丢）；
- [ ] 分片销毁/迁移时在途 timer 的处理；
- [ ] 回调异常路径（`m_exception_handler`）在分片线程上是否仍安全；
- [ ] 析构顺序（先停线程再销毁 multimap）。

### 交付物

1. 代码（`timer.cpp` / `timer.hpp`）；
2. `docs/reports/asm-timer.md`（汇编取证）；
3. 三件套验收表（架构/汇编/正确性各一行，含证据）；
4. 审查记录（`user/review/定时器-审查.md`）。
