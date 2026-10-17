
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
