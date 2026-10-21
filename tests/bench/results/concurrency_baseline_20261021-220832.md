# 并发原语调试监控开销基线

- 原始数据：`concurrency_baseline_20261021-220832.tsv`
- 汇总数据：`concurrency_baseline_20261021-220832_summary.tsv`
- 每场景操作数 N=50000，每配置 3 轮取中位数，延迟采样上限 LAT_N，loadavg 上限 2.0
- OFF 构建：`/UserData/CodexWorkSpace/Go2Cpp-work/build-work`；ON 构建：`/UserData/CodexWorkSpace/Go2Cpp-work/build-debug-conc`
- 吞吐 = ops / 墙钟时间；p50/p99 含逐操作 `steady_clock` 计时成本（约 20–30 ns）
- 事件量 = `EventRing::TotalCountAll()` 累计增量；OFF 构建恒 0
- 窗口覆盖倍数 = 事件数 /（活跃 ring 数 × 每线程 320 槽）；>1 表示环形窗口在压测期间至少被覆盖一遍，历史事件必有丢失（事件环没有 dropped 计数器，这是下界）

## 1. 场景 × 配置（吞吐 / 延迟 / 事件）

| 场景 | 配置 | 轮数 | 吞吐 (ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | 事件/操作 | 窗口覆盖倍数 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| mutex4 | OFF | 3 | 20135861 | 32.0 | 944.0 | 88248.0 | 0.000 | 0.00 |
| mutex4 | SAMPLED | 3 | 20484080 | 33.0 | 1466.0 | 89599.0 | 0.000 | 0.00 |
| mutex4 | SAMPLED_ALL | 3 | 8437699 | 76.0 | 2375.0 | 130464.0 | 0.291 | 11.38 |
| mutex4 | FULL_NO_CAPTURE | 3 | 8985386 | 60.0 | 1253.0 | 131218.0 | 0.334 | 13.05 |
| mutex4 | FULL | 3 | 5256617 | 71.0 | 5591.0 | 161712.0 | 0.319 | 12.47 |
| mutex8 | OFF | 3 | 21772272 | 30.0 | 551.0 | 206455.0 | 0.000 | 0.00 |
| mutex8 | SAMPLED | 3 | 27798843 | 30.0 | 792.0 | 215704.0 | 0.000 | 0.00 |
| mutex8 | SAMPLED_ALL | 3 | 7824819 | 41.0 | 932.0 | 316048.0 | 0.289 | 5.64 |
| mutex8 | FULL_NO_CAPTURE | 3 | 7845423 | 60.0 | 932.0 | 311450.0 | 0.315 | 6.15 |
| mutex8 | FULL | 3 | 5480910 | 60.0 | 3907.0 | 343961.0 | 0.311 | 6.08 |
| rwmix | OFF | 3 | 3080662 | 40.0 | 9258.0 | 799654.0 | 0.000 | 0.00 |
| rwmix | SAMPLED | 3 | 2993559 | 40.0 | 17453.0 | 878709.0 | 0.000 | 0.00 |
| rwmix | SAMPLED_ALL | 3 | 627653 | 151.0 | 397654.0 | 2478784.0 | 2.180 | 42.57 |
| rwmix | FULL_NO_CAPTURE | 3 | 583976 | 141.0 | 326048.0 | 2071357.0 | 2.053 | 40.10 |
| rwmix | FULL | 3 | 347131 | 174.0 | 585068.0 | 2403149.0 | 2.070 | 40.44 |
| mpmc | OFF | 3 | 6100652 | 33.0 | 44512.0 | 154647.0 | 0.000 | 0.00 |
| mpmc | SAMPLED | 3 | 1756403 | 33.0 | 171046.0 | 475813.0 | 0.141 | 4.89 |
| mpmc | SAMPLED_ALL | 3 | 1501051 | 43.0 | 178031.0 | 540774.0 | 0.405 | 14.06 |
| mpmc | FULL_NO_CAPTURE | 3 | 1577942 | 33.0 | 197074.0 | 487328.0 | 0.430 | 14.93 |
| mpmc | FULL | 3 | 1201104 | 44.0 | 234736.0 | 658766.0 | 0.438 | 15.22 |
| pc_rtt | OFF | 3 | 2748394 | 663.0 | 1411.0 | 10735.0 | 0.000 | 0.00 |
| pc_rtt | SAMPLED | 3 | 1671670 | 1172.0 | 2561.0 | 13980.0 | 5.500 | 572.94 |
| pc_rtt | SAMPLED_ALL | 3 | 1290112 | 1444.0 | 3310.0 | 14935.0 | 8.626 | 898.50 |
| pc_rtt | FULL_NO_CAPTURE | 3 | 1340294 | 1367.0 | 1867.0 | 12948.0 | 8.626 | 898.50 |
| pc_rtt | FULL | 3 | 893790 | 2594.0 | 3321.0 | 25495.0 | 8.626 | 898.50 |
| wg | OFF | 3 | 33007540 | 54.0 | 1292.0 | 3169.0 | 0.000 | 0.00 |
| wg | SAMPLED | 3 | 17133243 | 54.0 | 2366.0 | 32952.0 | 0.000 | 0.00 |
| wg | SAMPLED_ALL | 3 | 15678289 | 55.0 | 3115.0 | 31878.0 | 0.125 | 3.91 |
| wg | FULL_NO_CAPTURE | 3 | 24662154 | 65.0 | 325.0 | 1584.0 | 0.125 | 3.91 |
| wg | FULL | 3 | 13042157 | 65.0 | 3658.0 | 35861.0 | 0.125 | 3.91 |
| select2 | OFF | 3 | 991822 | 1204.0 | 2705.0 | 12213.0 | 0.000 | 0.00 |
| select2 | SAMPLED | 3 | 115924 | 10490.0 | 30011.0 | 78069.0 | 5.502 | 171.93 |
| select2 | SAMPLED_ALL | 3 | 110564 | 11511.0 | 38596.0 | 80004.0 | 11.977 | 374.27 |
| select2 | FULL_NO_CAPTURE | 3 | 113459 | 11331.0 | 30804.0 | 77251.0 | 12.023 | 313.10 |
| select2 | FULL | 3 | 108184 | 11820.0 | 36957.0 | 84618.0 | 12.024 | 313.13 |
| select4 | OFF | 3 | 679373 | 1563.0 | 1889.0 | 15630.0 | 0.000 | 0.00 |
| select4 | SAMPLED | 3 | 80266 | 15814.0 | 50720.0 | 119154.0 | 5.502 | 171.92 |
| select4 | SAMPLED_ALL | 3 | 77366 | 16184.0 | 46335.0 | 102394.0 | 12.167 | 380.21 |
| select4 | FULL_NO_CAPTURE | 3 | 76838 | 16151.0 | 48382.0 | 105478.0 | 12.213 | 318.03 |
| select4 | FULL | 3 | 74199 | 16563.0 | 48322.0 | 137031.0 | 12.213 | 318.06 |
| mixed | OFF | 3 | 69183740 | 44.0 | 4668.0 | 18983.0 | 0.000 | 0.00 |
| mixed | SAMPLED | 3 | 27861928 | 44.0 | 17235.0 | 57157.0 | 0.048 | 9.05 |
| mixed | SAMPLED_ALL | 3 | 19219915 | 66.0 | 22261.0 | 59935.0 | 0.172 | 32.30 |
| mixed | FULL_NO_CAPTURE | 3 | 20775266 | 66.0 | 22142.0 | 58861.0 | 0.188 | 35.16 |
| mixed | FULL | 3 | 15721893 | 65.0 | 26028.0 | 68922.0 | 0.188 | 35.17 |

## 2. 相对 OFF 的开销倍数（同场景、中位数）

| 场景 | 配置 | 慢倍数 | p50 倍数 | p99 倍数 | 事件/操作 |
|---|---|---:|---:|---:|---:|
| mutex4 | SAMPLED | 0.98 | 1.03 | 1.55 | 0.000 |
| mutex4 | SAMPLED_ALL | 2.39 | 2.38 | 2.52 | 0.291 |
| mutex4 | FULL_NO_CAPTURE | 2.24 | 1.88 | 1.33 | 0.334 |
| mutex4 | FULL | 3.83 | 2.22 | 5.92 | 0.319 |
| mutex8 | SAMPLED | 0.78 | 1.00 | 1.44 | 0.000 |
| mutex8 | SAMPLED_ALL | 2.78 | 1.37 | 1.69 | 0.289 |
| mutex8 | FULL_NO_CAPTURE | 2.78 | 2.00 | 1.69 | 0.315 |
| mutex8 | FULL | 3.97 | 2.00 | 7.09 | 0.311 |
| rwmix | SAMPLED | 1.03 | 1.00 | 1.89 | 0.000 |
| rwmix | SAMPLED_ALL | 4.91 | 3.77 | 42.95 | 2.180 |
| rwmix | FULL_NO_CAPTURE | 5.28 | 3.52 | 35.22 | 2.053 |
| rwmix | FULL | 8.87 | 4.35 | 63.20 | 2.070 |
| mpmc | SAMPLED | 3.47 | 1.00 | 3.84 | 0.141 |
| mpmc | SAMPLED_ALL | 4.06 | 1.30 | 4.00 | 0.405 |
| mpmc | FULL_NO_CAPTURE | 3.87 | 1.00 | 4.43 | 0.430 |
| mpmc | FULL | 5.08 | 1.33 | 5.27 | 0.438 |
| pc_rtt | SAMPLED | 1.64 | 1.77 | 1.82 | 5.500 |
| pc_rtt | SAMPLED_ALL | 2.13 | 2.18 | 2.35 | 8.626 |
| pc_rtt | FULL_NO_CAPTURE | 2.05 | 2.06 | 1.32 | 8.626 |
| pc_rtt | FULL | 3.07 | 3.91 | 2.35 | 8.626 |
| wg | SAMPLED | 1.93 | 1.00 | 1.83 | 0.000 |
| wg | SAMPLED_ALL | 2.11 | 1.02 | 2.41 | 0.125 |
| wg | FULL_NO_CAPTURE | 1.34 | 1.20 | 0.25 | 0.125 |
| wg | FULL | 2.53 | 1.20 | 2.83 | 0.125 |
| select2 | SAMPLED | 8.56 | 8.71 | 11.09 | 5.502 |
| select2 | SAMPLED_ALL | 8.97 | 9.56 | 14.27 | 11.977 |
| select2 | FULL_NO_CAPTURE | 8.74 | 9.41 | 11.39 | 12.023 |
| select2 | FULL | 9.17 | 9.82 | 13.66 | 12.024 |
| select4 | SAMPLED | 8.46 | 10.12 | 26.85 | 5.502 |
| select4 | SAMPLED_ALL | 8.78 | 10.35 | 24.53 | 12.167 |
| select4 | FULL_NO_CAPTURE | 8.84 | 10.33 | 25.61 | 12.213 |
| select4 | FULL | 9.16 | 10.60 | 25.58 | 12.213 |
| mixed | SAMPLED | 2.48 | 1.00 | 3.69 | 0.048 |
| mixed | SAMPLED_ALL | 3.60 | 1.50 | 4.77 | 0.172 |
| mixed | FULL_NO_CAPTURE | 3.33 | 1.50 | 4.74 | 0.188 |
| mixed | FULL | 4.40 | 1.48 | 5.58 | 0.188 |

## 3. 微基准（单次调用成本）

| 微基准 | 配置 | ns/调用 | 事件/调用 | 说明 |
|---|---|---:|---:|---|
| record | OFF | 1.66 | 0.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | SAMPLED | 29.57 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | SAMPLED_ALL | 28.60 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | FULL_NO_CAPTURE | 27.64 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | FULL | 28.15 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record_sampled32 | OFF | 1.86 | 0.0000 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | SAMPLED | 5.20 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | SAMPLED_ALL | 4.97 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | FULL_NO_CAPTURE | 5.21 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | FULL | 5.30 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| lookup | OFF | 1.91 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | SAMPLED | 8.66 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | SAMPLED_ALL | 8.64 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | FULL_NO_CAPTURE | 8.67 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | FULL | 8.73 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| stack | OFF | 2.14 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | SAMPLED | 2.26 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | SAMPLED_ALL | 9.19 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | FULL_NO_CAPTURE | 2.24 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | FULL | 656.44 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| config | OFF | 0.51 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | SAMPLED | 1.22 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | SAMPLED_ALL | 1.19 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | FULL_NO_CAPTURE | 1.19 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | FULL | 1.15 | 0.0000 | DebugConfig::Enabled：原子掩码读 |

## 4. 事件构成（FULL 配置，按类别）

| 场景 | 事件明细 |
|---|---|
| mutex4 | mutex=15963 |
| mutex8 | mutex=16442 |
| rwmix | mutex=53632,rw=33874,cv=22005 |
| mpmc | park=7656,wake=5362,sched=2049,ch=21018,pc=8246,life=322 |
| pc_rtt | park=299997,wake=199998,sched=50032,pc=300029,life=12507 |
| wg | wg=6256 |
| select2 | park=149982,wake=99988,sched=25183,ch=169533,pc=150253,life=6258 |
| select4 | park=149970,wake=99980,sched=25224,ch=178876,pc=150366,life=6256 |
| mixed | park=7910,wake=5272,sched=1339,mutex=14066,ch=19389,pc=7947,life=342 |

## 5. 数据完整性与说明

- 所有 (配置, 场景) 组合均达到 3 轮有效样本。
- dropped_threads 合计：0；stack_samples dropped 峰值：0
- 窗口覆盖倍数峰值：898.5×（每线程 320 槽，事件多于此即回绕）
- `SAMPLED` 关闭 primitives/stacks/chains 类别；`SAMPLED_ALL` 打开全部类别但 shift=5。`GO2CPP_DEBUG_SAMPLE_SHIFT` 只降低 快路径采样事件与栈捕获频率，**不降低资源事件的记录频率**。
- `FULL_NO_CAPTURE` 关闭 stacks/chains 类别，用于把「事件编码 + 资源路径」与「backtrace / Fiber 链」拆开归因。
- valid=0（loadavg 超限）与 rc≠0 的轮次已从汇总剔除，原始 TSV 保留。

