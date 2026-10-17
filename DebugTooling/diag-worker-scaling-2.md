
---

## 2026-10-17 16:10–16:40：性能任务第 1 轮 —— emit_event 快路径（含一次自造 bug）

使用者开启性能优化任务（定时到 20:00）。第 1 轮聚焦 `emit_event`。

### 发现的真实问题

`emit_event` 在 **observer 为 null 时也付出全部代价**：
取一次 `steady_clock::now()`（~25 ns）、加一把**全局** `observer_mutex`、拷贝一次 `shared_ptr`。
而它在 `worker_loop` 里**每个任务被调用两次**（`kTaskStarted` + 完成事件）。
`min_workers = N` 时 N 个 worker 争同一把锁。

### 修复

加 `std::atomic<bool> has_observer` 快路径：未装观察者时**直接返回**，不取时钟、不加锁、不拷
`shared_ptr`。`set_observer()` 与**构造函数**都要维护它（见下）。

### 我引入的 bug 与修复（重要）

第一版只在 `set_observer()` 里置位标志，**漏了构造函数路径** —— 而测试与生产代码都是通过
`config.observer` 在构造时装入观察者的。结果：`has_observer` 恒为 false，
**所有事件被快路径静默吞掉**。

`ctest` 报了 6 个检查失败（`test_scheduler.cpp:177-183` 的 `started` / `stopping` /
`workers_started` / `workers_stopped` / `tasks_started` / `tasks_terminal` 全为 0）。

修复：在 `Impl` 构造函数体开头补 `has_observer.store(observer != nullptr, release);`。
修完 `ctest` **100% passed, 80/80, 32.73 s**。

**教训**：给热路径加快路径标志，必须**枚举所有会修改被判定状态的地方**。这个项目里
观察者有两个入口（`set_observer` 与 `config.observer`），我只覆盖了一个。

### 性能实测（未达预期，如实记录）

负载 0.91（可信），档 0（无 observer）：

| workers | 修复前 | 修复后 | 修复前倍率 | 修复后倍率 |
|---:|---:|---:|---:|---:|
| 1 | 1,363.2 | 1,639.9 | 1.00x | 1.00x |
| 2 | 1,759.2 | 1,400.1 | 1.29x | 0.85x |
| 3 | 10,027.8 | 10,601.2 | 7.36x | 6.46x |
| 4 | 22,013.9 | 17,127.7 | 16.15x | 10.44x |

**判读**：4 worker 表面改善（16.15x → 10.44x），但 1 worker 也变了（1,363 → 1,640），
**方差显著**；3 worker 完全没改善。→ **`emit_event` 不是 worker 劣化的主因**。
这个修复本身在逻辑上是对的（省掉每次都取的时钟与全局锁），但**不宣称它解决了劣化**。

### 另一个发现：测量被外部负载污染

`go2cpp_tests` 曾跑出 **real 233 s / user 3.4 s + sys 2.4 s**（97% 时间在等），
`ctest` 报 Timeout。查下来是**外部会话**在跑 `ctest --test-dir build --timeout 600`
（不是本会话起的），把 load 抬到 2.03。之后单独重跑：**32.73 s，全绿**。
→ 那次 Timeout **不是我的改动引入的**，是环境干扰。

**教训**：性能测量前必须先 `uptime`；负载 > 1.5 时数字全部作废。

---

## 2026-10-17 16:40–17:10：第 2 轮 —— 四个假设逐个排除

### 尝试 1：本地队列快速路径（代码已改，但未解决）

读码发现 `worker_loop` 的找活慢路径把**读自己队列**（`take_local`，只需 `own_processor.mutex`）
也包在**全局 `admission_lock`（`m_impl->mutex`）** 里。已把本地队列检查提到全局锁外：

```
// 快速路径：只持本 P 自己的锁，不碰全局 admission mutex
if (!task) { lock(own_processor.mutex); pop front; }
if (!task) { lock(m_impl->mutex); ...原慢路径... }
```

实测（档 0，负载 1.07）：1/2/3/4 = 1712.4/1198.6/10509.3/17276.3 ns，倍率 1.00/0.70/6.14/10.09x。
**3 worker 无改善（1.02x）**。改动本身是合理的（减少全局锁持有），但不是根因。

### 尝试 2：惊群（`notify_all`）—— 已排除

枚举全部 `condition.notify_*`：正常入队路径（2445/2541/2593/2659）**已经用 `notify_one`**；
`notify_all` 只出现在启动（1953）、排水（2042）、关闭（2085）、线程资源耗尽（1409）、
native-blocking 替代（1477）与 `sysmon_wait_condition`/`m_completion_condition`（不同变量）。
→ 常规路径**不存在惊群**。

### 尝试 3：全局队列溢出 —— 已排除

`local_queue_limit` 默认 256，20,000 任务 / 4 P ≈ 5,000 每 P，确实大量溢出。给基准加了第 5 个
参数直接调它，实测：

| local_queue_limit | 1/2/3/4 worker 倍率 |
|---|---|
| 256（默认）| 1.00 / 1.26 / 6.11 / 11.20x |
| 8192 | 1.00 / 0.96 / **3.58** / **17.81x** |
| 65536 | 1.00 / 0.72 / 4.58 / 11.23x |

**无单调关系、方差极大** → 队列上限不是决定性因素。

### 重新定性（重要）

对比两个基准：

| 基准 | 任务形态 | 扩展性 |
|---|---|---|
| `gmp_compute` | 每 worker **一个 30 ms 长任务** | **正常**（1.00/2.00/3.74/7.33/10.43x）|
| `debug_observer` | **20,000 个 1.3 µs 短任务** | **劣化 6~11x** |

而 `diag-worker-scaling.md` 实验 2 已证明：任务数 2,000 → 80,000（40 倍）时每任务耗时
19,199 → 23,088 **不下降**，所以**不是"高频交接被摊薄"**，而是**每个任务都要付的稳态开销**。

结论：**劣化出现在"大量极短任务 + 多个常驻 worker"的组合下**，每个任务都付出 cross-worker 的
同步代价（数量级 µs），而任务本身只有 1.3 µs，于是被完全淹没。`min_workers=1` 时没有
cross-worker 交接，所以正常。

### 环境限制（影响后续方法）

- **`perf` 不可用**：`perf_event_paranoid = 2` 且容器内无权限；
- `/proc/PID/status` 的上下文切换采样失败：基准 3 秒就结束，来不及采样；
- 外部会话偶发跑 `ctest` 会把负载抬到 2.03，性能测量前必须 `uptime`。

### 下一步

用 `-pg` + `gprof`（不改业务代码，只重编译）拿热点函数，替代不可用的 `perf`。
