# 并发原语调试监控开销基线

- 原始数据：`concurrency_baseline_20261021-215753.tsv`
- 汇总数据：`concurrency_baseline_20261021-215753_summary.tsv`
- 每场景操作数 N=50000，每配置 3 轮取中位数，延迟采样上限 LAT_N，loadavg 上限 2.0
- OFF 构建：`/UserData/CodexWorkSpace/Go2Cpp-work/build-work`；ON 构建：`/UserData/CodexWorkSpace/Go2Cpp-work/build-debug-conc`
- 吞吐 = ops / 墙钟时间；p50/p99 含逐操作 `steady_clock` 计时成本（约 20–30 ns）
- 事件量 = `EventRing::TotalCountAll()` 累计增量；OFF 构建恒 0
- 窗口覆盖倍数 = 事件数 /（活跃 ring 数 × 每线程 320 槽）；>1 表示环形窗口在压测期间至少被覆盖一遍，历史事件必有丢失（事件环没有 dropped 计数器，这是下界）

## 1. 场景 × 配置（吞吐 / 延迟 / 事件）

| 场景 | 配置 | 轮数 | 吞吐 (ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | 事件/操作 | 窗口覆盖倍数 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| mutex4 | OFF | 3 | 20650371 | 32.0 | 1192.0 | 99202.0 | 0.000 | 0.00 |
| mutex4 | SAMPLED | 3 | 24945432 | 32.0 | 857.0 | 100757.0 | 0.000 | 0.00 |
| mutex4 | SAMPLED_ALL | 3 | 4243058 | 87.0 | 3167.0 | 177817.0 | 0.549 | 21.46 |
| mutex4 | FULL_NO_CAPTURE | 3 | 5711686 | 76.0 | 2255.0 | 156173.0 | 0.579 | 22.60 |
| mutex4 | FULL | 3 | 2516539 | 98.0 | 35447.0 | 239576.0 | 0.571 | 22.32 |
| mutex8 | OFF | 3 | 37591130 | 32.0 | 1117.0 | 238730.0 | 0.000 | 0.00 |
| mutex8 | SAMPLED | 3 | 22240786 | 32.0 | 553.0 | 224199.0 | 0.000 | 0.00 |
| mutex8 | SAMPLED_ALL | 3 | 4059420 | 87.0 | 2602.0 | 389943.0 | 0.533 | 10.41 |
| mutex8 | FULL_NO_CAPTURE | 3 | 5221066 | 76.0 | 1529.0 | 314613.0 | 0.559 | 10.92 |
| mutex8 | FULL | 3 | 2516310 | 98.0 | 34200.0 | 555447.0 | 0.568 | 11.10 |
| rwmix | OFF | 3 | 3624328 | 43.0 | 12742.0 | 857839.0 | 0.000 | 0.00 |
| rwmix | SAMPLED | 3 | 2733900 | 44.0 | 18685.0 | 864411.0 | 0.000 | 0.00 |
| rwmix | SAMPLED_ALL | 3 | 258862 | 251.0 | 717728.0 | 3486527.0 | 2.852 | 55.71 |
| rwmix | FULL_NO_CAPTURE | 3 | 341676 | 200.0 | 514735.0 | 3200721.0 | 2.846 | 55.59 |
| rwmix | FULL | 3 | 152276 | 371.0 | 1342472.0 | 4174749.0 | 3.130 | 61.14 |
| mpmc | OFF | 3 | 6604671 | 30.0 | 44394.0 | 140360.0 | 0.000 | 0.00 |
| mpmc | SAMPLED | 3 | 1940114 | 30.0 | 197014.0 | 442899.0 | 0.145 | 5.05 |
| mpmc | SAMPLED_ALL | 3 | 1536484 | 60.0 | 178276.0 | 435329.0 | 0.513 | 17.82 |
| mpmc | FULL_NO_CAPTURE | 3 | 1665134 | 50.0 | 184770.0 | 455588.0 | 0.536 | 18.60 |
| mpmc | FULL | 3 | 1147474 | 70.0 | 220062.0 | 521638.0 | 0.543 | 18.86 |
| pc_rtt | OFF | 3 | 2847808 | 675.0 | 790.0 | 3888.0 | 0.000 | 0.00 |
| pc_rtt | SAMPLED | 3 | 1733084 | 1071.0 | 2464.0 | 12774.0 | 5.500 | 572.93 |
| pc_rtt | SAMPLED_ALL | 3 | 1383976 | 1331.0 | 3202.0 | 14253.0 | 7.875 | 820.36 |
| pc_rtt | FULL_NO_CAPTURE | 3 | 1419700 | 1300.0 | 3088.0 | 14337.0 | 7.876 | 820.38 |
| pc_rtt | FULL | 3 | 980226 | 1757.0 | 4596.0 | 22289.0 | 7.876 | 820.38 |
| wg | OFF | 3 | 32104301 | 52.0 | 2194.0 | 8369.0 | 0.000 | 0.00 |
| wg | SAMPLED | 3 | 18792926 | 52.0 | 1185.0 | 14638.0 | 0.000 | 0.00 |
| wg | SAMPLED_ALL | 3 | 10321376 | 72.0 | 3794.0 | 34816.0 | 0.250 | 7.82 |
| wg | FULL_NO_CAPTURE | 3 | 12372464 | 63.0 | 613.0 | 3617.0 | 0.250 | 7.82 |
| wg | FULL | 3 | 10467241 | 73.0 | 4169.0 | 35128.0 | 0.250 | 7.82 |
| select2 | OFF | 3 | 1048136 | 1174.0 | 1341.0 | 8941.0 | 0.000 | 0.00 |
| select2 | SAMPLED | 3 | 122271 | 10833.0 | 28849.0 | 72325.0 | 5.502 | 171.93 |
| select2 | SAMPLED_ALL | 3 | 117292 | 11404.0 | 32665.0 | 83792.0 | 11.571 | 361.59 |
| select2 | FULL_NO_CAPTURE | 3 | 116968 | 10999.0 | 33704.0 | 103281.0 | 11.617 | 302.52 |
| select2 | FULL | 3 | 109610 | 11550.0 | 36916.0 | 63145.0 | 11.617 | 302.53 |
| select4 | OFF | 3 | 706056 | 1519.0 | 1867.0 | 12905.0 | 0.000 | 0.00 |
| select4 | SAMPLED | 3 | 80351 | 16128.0 | 48821.0 | 117482.0 | 5.502 | 171.92 |
| select4 | SAMPLED_ALL | 3 | 76905 | 16976.0 | 49016.0 | 118644.0 | 11.948 | 373.38 |
| select4 | FULL_NO_CAPTURE | 3 | 76827 | 16780.0 | 40648.0 | 98243.0 | 11.995 | 312.37 |
| select4 | FULL | 3 | 71843 | 17149.0 | 51805.0 | 146006.0 | 11.996 | 312.40 |
| mixed | OFF | 3 | 78601831 | 43.0 | 3093.0 | 13015.0 | 0.000 | 0.00 |
| mixed | SAMPLED | 3 | 27444846 | 44.0 | 17908.0 | 55724.0 | 0.048 | 9.06 |
| mixed | SAMPLED_ALL | 3 | 16234939 | 109.0 | 22891.0 | 70952.0 | 0.249 | 46.72 |
| mixed | FULL_NO_CAPTURE | 3 | 18928865 | 98.0 | 17420.0 | 59794.0 | 0.264 | 49.53 |
| mixed | FULL | 3 | 11510239 | 141.0 | 33972.0 | 90283.0 | 0.265 | 49.73 |

## 2. 相对 OFF 的开销倍数（同场景、中位数）

| 场景 | 配置 | 慢倍数 | p50 倍数 | p99 倍数 | 事件/操作 |
|---|---|---:|---:|---:|---:|
| mutex4 | SAMPLED | 0.83 | 1.00 | 0.72 | 0.000 |
| mutex4 | SAMPLED_ALL | 4.87 | 2.72 | 2.66 | 0.549 |
| mutex4 | FULL_NO_CAPTURE | 3.62 | 2.38 | 1.89 | 0.579 |
| mutex4 | FULL | 8.21 | 3.06 | 29.74 | 0.571 |
| mutex8 | SAMPLED | 1.69 | 1.00 | 0.50 | 0.000 |
| mutex8 | SAMPLED_ALL | 9.26 | 2.72 | 2.33 | 0.533 |
| mutex8 | FULL_NO_CAPTURE | 7.20 | 2.38 | 1.37 | 0.559 |
| mutex8 | FULL | 14.94 | 3.06 | 30.62 | 0.568 |
| rwmix | SAMPLED | 1.33 | 1.02 | 1.47 | 0.000 |
| rwmix | SAMPLED_ALL | 14.00 | 5.84 | 56.33 | 2.852 |
| rwmix | FULL_NO_CAPTURE | 10.61 | 4.65 | 40.40 | 2.846 |
| rwmix | FULL | 23.80 | 8.63 | 105.36 | 3.130 |
| mpmc | SAMPLED | 3.40 | 1.00 | 4.44 | 0.145 |
| mpmc | SAMPLED_ALL | 4.30 | 2.00 | 4.02 | 0.513 |
| mpmc | FULL_NO_CAPTURE | 3.97 | 1.67 | 4.16 | 0.536 |
| mpmc | FULL | 5.76 | 2.33 | 4.96 | 0.543 |
| pc_rtt | SAMPLED | 1.64 | 1.59 | 3.12 | 5.500 |
| pc_rtt | SAMPLED_ALL | 2.06 | 1.97 | 4.05 | 7.875 |
| pc_rtt | FULL_NO_CAPTURE | 2.01 | 1.93 | 3.91 | 7.876 |
| pc_rtt | FULL | 2.91 | 2.60 | 5.82 | 7.876 |
| wg | SAMPLED | 1.71 | 1.00 | 0.54 | 0.000 |
| wg | SAMPLED_ALL | 3.11 | 1.38 | 1.73 | 0.250 |
| wg | FULL_NO_CAPTURE | 2.59 | 1.21 | 0.28 | 0.250 |
| wg | FULL | 3.07 | 1.40 | 1.90 | 0.250 |
| select2 | SAMPLED | 8.57 | 9.23 | 21.51 | 5.502 |
| select2 | SAMPLED_ALL | 8.94 | 9.71 | 24.36 | 11.571 |
| select2 | FULL_NO_CAPTURE | 8.96 | 9.37 | 25.13 | 11.617 |
| select2 | FULL | 9.56 | 9.84 | 27.53 | 11.617 |
| select4 | SAMPLED | 8.79 | 10.62 | 26.15 | 5.502 |
| select4 | SAMPLED_ALL | 9.18 | 11.18 | 26.25 | 11.948 |
| select4 | FULL_NO_CAPTURE | 9.19 | 11.05 | 21.77 | 11.995 |
| select4 | FULL | 9.83 | 11.29 | 27.75 | 11.996 |
| mixed | SAMPLED | 2.86 | 1.02 | 5.79 | 0.048 |
| mixed | SAMPLED_ALL | 4.84 | 2.53 | 7.40 | 0.249 |
| mixed | FULL_NO_CAPTURE | 4.15 | 2.28 | 5.63 | 0.264 |
| mixed | FULL | 6.83 | 3.28 | 10.98 | 0.265 |

## 3. 微基准（单次调用成本）

| 微基准 | 配置 | ns/调用 | 事件/调用 | 说明 |
|---|---|---:|---:|---|
| record | OFF | 1.68 | 0.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | SAMPLED | 27.43 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | SAMPLED_ALL | 28.62 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | FULL_NO_CAPTURE | 27.55 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | FULL | 27.37 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record_sampled32 | OFF | 1.94 | 0.0000 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | SAMPLED | 5.22 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | SAMPLED_ALL | 5.11 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | FULL_NO_CAPTURE | 5.30 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | FULL | 5.10 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| lookup | OFF | 1.94 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | SAMPLED | 8.96 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | SAMPLED_ALL | 8.75 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | FULL_NO_CAPTURE | 8.69 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | FULL | 8.35 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| stack | OFF | 1.90 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | SAMPLED | 2.34 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | SAMPLED_ALL | 9.42 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | FULL_NO_CAPTURE | 2.28 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | FULL | 687.09 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| config | OFF | 0.52 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | SAMPLED | 1.38 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | SAMPLED_ALL | 1.20 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | FULL_NO_CAPTURE | 1.19 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | FULL | 1.18 | 0.0000 | DebugConfig::Enabled：原子掩码读 |

## 4. 事件构成（FULL 配置，按类别）

| 场景 | 事件明细 |
|---|---|
| mutex4 | mutex=28569 |
| mutex8 | mutex=29030 |
| rwmix | mutex=84089,rw=49710,cv=22178 |
| mpmc | park=7124,wake=5242,sched=2135,ch=33429,pc=6104,life=294 |
| pc_rtt | park=299999,wake=200000,sched=50018,pc=225031,life=12507 |
| wg | wg=12504 |
| select2 | park=149975,wake=99984,sched=25158,ch=186724,pc=112768,life=6257 |
| select4 | park=149964,wake=99976,sched=25215,ch=205537,pc=112885,life=6256 |
| mixed | park=8630,wake=5756,sched=1494,mutex=26707,ch=32339,pc=6474,life=372 |

## 5. 数据完整性与说明

- 所有 (配置, 场景) 组合均达到 3 轮有效样本。
- dropped_threads 合计：0；stack_samples dropped 峰值：0
- 窗口覆盖倍数峰值：820.4×（每线程 320 槽，事件多于此即回绕）
- `SAMPLED` 关闭 primitives/stacks/chains 类别；`SAMPLED_ALL` 打开全部类别但 shift=5。`GO2CPP_DEBUG_SAMPLE_SHIFT` 只降低 快路径采样事件与栈捕获频率，**不降低资源事件的记录频率**。
- `FULL_NO_CAPTURE` 关闭 stacks/chains 类别，用于把「事件编码 + 资源路径」与「backtrace / Fiber 链」拆开归因。
- valid=0（loadavg 超限）与 rc≠0 的轮次已从汇总剔除，原始 TSV 保留。

