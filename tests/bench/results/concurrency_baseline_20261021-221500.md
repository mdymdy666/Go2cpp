# 并发原语调试监控开销基线

- 原始数据：`concurrency_baseline_20261021-221500.tsv`
- 汇总数据：`concurrency_baseline_20261021-221500_summary.tsv`
- 每场景操作数 N=50000，每配置 3 轮取中位数，延迟采样上限 LAT_N，loadavg 上限 2.0
- OFF 构建：`/UserData/CodexWorkSpace/Go2Cpp-work/build-work`；ON 构建：`/UserData/CodexWorkSpace/Go2Cpp-work/build-debug-conc`
- 吞吐 = ops / 墙钟时间；p50/p99 含逐操作 `steady_clock` 计时成本（约 20–30 ns）
- 事件量 = `EventRing::TotalCountAll()` 累计增量；OFF 构建恒 0
- 窗口覆盖倍数 = 事件数 /（活跃 ring 数 × 每线程 320 槽）；>1 表示环形窗口在压测期间至少被覆盖一遍，历史事件必有丢失（事件环没有 dropped 计数器，这是下界）

## 1. 场景 × 配置（吞吐 / 延迟 / 事件）

| 场景 | 配置 | 轮数 | 吞吐 (ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | 事件/操作 | 窗口覆盖倍数 |
|---|---|---:|---:|---:|---:|---:|---:|---:|

## 2. 相对 OFF 的开销倍数（同场景、中位数）

| 场景 | 配置 | 慢倍数 | p50 倍数 | p99 倍数 | 事件/操作 |
|---|---|---:|---:|---:|---:|

## 3. 微基准（单次调用成本）

| 微基准 | 配置 | ns/调用 | 事件/调用 | 说明 |
|---|---|---:|---:|---|

## 4. 事件构成（FULL 配置，按类别）

| 场景 | 事件明细 |
|---|---|

## 5. 数据完整性与说明

- 所有 (配置, 场景) 组合均达到 3 轮有效样本。
- dropped_threads 合计：0；stack_samples dropped 峰值：0
- 窗口覆盖倍数峰值：0.0×（每线程 320 槽，事件多于此即回绕）
- `SAMPLED` 关闭 primitives/stacks/chains 类别；`SAMPLED_ALL` 打开全部类别但 shift=5。`GO2CPP_DEBUG_SAMPLE_SHIFT` 只降低 快路径采样事件与栈捕获频率，**不降低资源事件的记录频率**。
- `FULL_NO_CAPTURE` 关闭 stacks/chains 类别，用于把「事件编码 + 资源路径」与「backtrace / Fiber 链」拆开归因。
- valid=0（loadavg 超限）与 rc≠0 的轮次已从汇总剔除，原始 TSV 保留。

