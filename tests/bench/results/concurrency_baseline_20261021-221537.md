# 并发原语调试监控开销基线

- 原始数据：`concurrency_baseline_20261021-221537.tsv`
- 汇总数据：`concurrency_baseline_20261021-221537_summary.tsv`
- 每场景操作数 N=50000，每配置 3 轮取中位数，延迟采样上限 LAT_N，loadavg 上限 4
- OFF 构建：`/UserData/CodexWorkSpace/Go2Cpp-work/build-work`；ON 构建：`/UserData/CodexWorkSpace/Go2Cpp-work/build-debug-conc`
- 吞吐 = ops / 墙钟时间；p50/p99 含逐操作 `steady_clock` 计时成本（约 20–30 ns）
- 事件量 = `EventRing::TotalCountAll()` 累计增量；OFF 构建恒 0
- 窗口覆盖倍数 = 事件数 /（活跃 ring 数 × 每线程 320 槽）；>1 表示环形窗口在压测期间至少被覆盖一遍，历史事件必有丢失（事件环没有 dropped 计数器，这是下界）

## 1. 场景 × 配置（吞吐 / 延迟 / 事件）

| 场景 | 配置 | 轮数 | 吞吐 (ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | 事件/操作 | 窗口覆盖倍数 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| mutex4 | OFF | 3 | 27698345 | 32.0 | 991.0 | 112156.0 | 0.000 | 0.00 |
| mutex4 | SAMPLED | 3 | 17961067 | 32.0 | 1651.0 | 107701.0 | 0.000 | 0.00 |
| mutex4 | SAMPLED_ALL | 3 | 8023847 | 75.0 | 2675.0 | 129915.0 | 0.307 | 11.98 |
| mutex4 | FULL_NO_CAPTURE | 3 | 9881401 | 74.0 | 2620.0 | 143003.0 | 0.364 | 14.22 |
| mutex4 | FULL | 3 | 5744463 | 64.0 | 6467.0 | 146674.0 | 0.379 | 14.79 |
| mutex8 | OFF | 3 | 23362499 | 32.0 | 597.0 | 213591.0 | 0.000 | 0.00 |
| mutex8 | SAMPLED | 3 | 17713418 | 32.0 | 597.0 | 258809.0 | 0.000 | 0.00 |
| mutex8 | SAMPLED_ALL | 3 | 7970860 | 64.0 | 1343.0 | 309871.0 | 0.291 | 5.68 |
| mutex8 | FULL_NO_CAPTURE | 3 | 7684119 | 64.0 | 1278.0 | 315741.0 | 0.312 | 6.10 |
| mutex8 | FULL | 3 | 4724406 | 43.0 | 3846.0 | 358590.0 | 0.309 | 6.04 |
| rwmix | OFF | 3 | 3298420 | 43.0 | 7479.0 | 993803.0 | 0.000 | 0.00 |
| rwmix | SAMPLED | 3 | 3143632 | 43.0 | 36085.0 | 974231.0 | 0.000 | 0.00 |
| rwmix | SAMPLED_ALL | 3 | 532188 | 160.0 | 400909.0 | 2484316.0 | 2.027 | 39.59 |
| rwmix | FULL_NO_CAPTURE | 3 | 634502 | 138.0 | 332521.0 | 1988266.0 | 2.175 | 42.48 |
| rwmix | FULL | 3 | 372777 | 170.0 | 562658.0 | 2634888.0 | 2.166 | 42.30 |
| mpmc | OFF | 3 | 6459614 | 32.0 | 48731.0 | 151518.0 | 0.000 | 0.00 |
| mpmc | SAMPLED | 3 | 1771939 | 32.0 | 186051.0 | 511668.0 | 0.142 | 4.95 |
| mpmc | SAMPLED_ALL | 3 | 1562066 | 33.0 | 203310.0 | 461605.0 | 0.394 | 13.68 |
| mpmc | FULL_NO_CAPTURE | 3 | 1572903 | 33.0 | 183571.0 | 504415.0 | 0.436 | 15.15 |
| mpmc | FULL | 3 | 1283229 | 43.0 | 213774.0 | 560769.0 | 0.442 | 15.36 |
| pc_rtt | OFF | 3 | 2803244 | 682.0 | 1005.0 | 12121.0 | 0.000 | 0.00 |
| pc_rtt | SAMPLED | 3 | 1612497 | 1200.0 | 1601.0 | 13667.0 | 5.500 | 572.94 |
| pc_rtt | SAMPLED_ALL | 3 | 1271082 | 1438.0 | 3298.0 | 14998.0 | 8.626 | 898.50 |
| pc_rtt | FULL_NO_CAPTURE | 3 | 1323132 | 1395.0 | 3298.0 | 15549.0 | 8.626 | 898.50 |
| pc_rtt | FULL | 3 | 867289 | 2606.0 | 4238.0 | 18825.0 | 8.626 | 898.51 |
| wg | OFF | 3 | 31948445 | 54.0 | 1373.0 | 7980.0 | 0.000 | 0.00 |
| wg | SAMPLED | 3 | 17359208 | 54.0 | 1925.0 | 31530.0 | 0.000 | 0.00 |
| wg | SAMPLED_ALL | 3 | 14905214 | 54.0 | 1374.0 | 10229.0 | 0.125 | 3.91 |
| wg | FULL_NO_CAPTURE | 3 | 25830802 | 65.0 | 216.0 | 1351.0 | 0.125 | 3.91 |
| wg | FULL | 3 | 15480476 | 54.0 | 1287.0 | 9688.0 | 0.125 | 3.91 |
| select2 | OFF | 3 | 882542 | 1265.0 | 1525.0 | 14938.0 | 0.000 | 0.00 |
| select2 | SAMPLED | 3 | 116911 | 6853.0 | 33034.0 | 81248.0 | 5.502 | 171.93 |
| select2 | SAMPLED_ALL | 3 | 112545 | 11678.0 | 35228.0 | 80803.0 | 11.977 | 374.28 |
| select2 | FULL_NO_CAPTURE | 3 | 118713 | 10370.0 | 32233.0 | 73950.0 | 12.023 | 313.10 |
| select2 | FULL | 3 | 108263 | 11105.0 | 37600.0 | 78548.0 | 12.024 | 313.12 |
| select4 | OFF | 3 | 714503 | 1510.0 | 2206.0 | 15222.0 | 0.000 | 0.00 |
| select4 | SAMPLED | 3 | 82358 | 14557.0 | 41971.0 | 131750.0 | 5.502 | 171.94 |
| select4 | SAMPLED_ALL | 3 | 78884 | 16035.0 | 43354.0 | 113192.0 | 12.166 | 380.20 |
| select4 | FULL_NO_CAPTURE | 3 | 80669 | 15729.0 | 50394.0 | 138632.0 | 12.213 | 318.05 |
| select4 | FULL | 3 | 76962 | 15876.0 | 42620.0 | 114596.0 | 12.212 | 318.02 |
| mixed | OFF | 3 | 76375368 | 80.0 | 5450.0 | 26942.0 | 0.000 | 0.00 |
| mixed | SAMPLED | 3 | 27603000 | 40.0 | 16261.0 | 50075.0 | 0.048 | 9.07 |
| mixed | SAMPLED_ALL | 3 | 21583310 | 60.0 | 17543.0 | 50215.0 | 0.172 | 32.30 |
| mixed | FULL_NO_CAPTURE | 3 | 21693778 | 61.0 | 19918.0 | 48020.0 | 0.187 | 35.15 |
| mixed | FULL | 3 | 17683244 | 60.0 | 20729.0 | 62679.0 | 0.188 | 35.15 |

## 2. 相对 OFF 的开销倍数（同场景、中位数）

| 场景 | 配置 | 慢倍数 | p50 倍数 | p99 倍数 | 事件/操作 |
|---|---|---:|---:|---:|---:|
| mutex4 | SAMPLED | 1.54 | 1.00 | 1.67 | 0.000 |
| mutex4 | SAMPLED_ALL | 3.45 | 2.34 | 2.70 | 0.307 |
| mutex4 | FULL_NO_CAPTURE | 2.80 | 2.31 | 2.64 | 0.364 |
| mutex4 | FULL | 4.82 | 2.00 | 6.53 | 0.379 |
| mutex8 | SAMPLED | 1.32 | 1.00 | 1.00 | 0.000 |
| mutex8 | SAMPLED_ALL | 2.93 | 2.00 | 2.25 | 0.291 |
| mutex8 | FULL_NO_CAPTURE | 3.04 | 2.00 | 2.14 | 0.312 |
| mutex8 | FULL | 4.95 | 1.34 | 6.44 | 0.309 |
| rwmix | SAMPLED | 1.05 | 1.00 | 4.82 | 0.000 |
| rwmix | SAMPLED_ALL | 6.20 | 3.72 | 53.60 | 2.027 |
| rwmix | FULL_NO_CAPTURE | 5.20 | 3.21 | 44.46 | 2.175 |
| rwmix | FULL | 8.85 | 3.95 | 75.23 | 2.166 |
| mpmc | SAMPLED | 3.65 | 1.00 | 3.82 | 0.142 |
| mpmc | SAMPLED_ALL | 4.14 | 1.03 | 4.17 | 0.394 |
| mpmc | FULL_NO_CAPTURE | 4.11 | 1.03 | 3.77 | 0.436 |
| mpmc | FULL | 5.03 | 1.34 | 4.39 | 0.442 |
| pc_rtt | SAMPLED | 1.74 | 1.76 | 1.59 | 5.500 |
| pc_rtt | SAMPLED_ALL | 2.21 | 2.11 | 3.28 | 8.626 |
| pc_rtt | FULL_NO_CAPTURE | 2.12 | 2.05 | 3.28 | 8.626 |
| pc_rtt | FULL | 3.23 | 3.82 | 4.22 | 8.626 |
| wg | SAMPLED | 1.84 | 1.00 | 1.40 | 0.000 |
| wg | SAMPLED_ALL | 2.14 | 1.00 | 1.00 | 0.125 |
| wg | FULL_NO_CAPTURE | 1.24 | 1.20 | 0.16 | 0.125 |
| wg | FULL | 2.06 | 1.00 | 0.94 | 0.125 |
| select2 | SAMPLED | 7.55 | 5.42 | 21.66 | 5.502 |
| select2 | SAMPLED_ALL | 7.84 | 9.23 | 23.10 | 11.977 |
| select2 | FULL_NO_CAPTURE | 7.43 | 8.20 | 21.14 | 12.023 |
| select2 | FULL | 8.15 | 8.78 | 24.66 | 12.024 |
| select4 | SAMPLED | 8.68 | 9.64 | 19.03 | 5.502 |
| select4 | SAMPLED_ALL | 9.06 | 10.62 | 19.65 | 12.166 |
| select4 | FULL_NO_CAPTURE | 8.86 | 10.42 | 22.84 | 12.213 |
| select4 | FULL | 9.28 | 10.51 | 19.32 | 12.212 |
| mixed | SAMPLED | 2.77 | 0.50 | 2.98 | 0.048 |
| mixed | SAMPLED_ALL | 3.54 | 0.75 | 3.22 | 0.172 |
| mixed | FULL_NO_CAPTURE | 3.52 | 0.76 | 3.65 | 0.187 |
| mixed | FULL | 4.32 | 0.75 | 3.80 | 0.188 |

## 3. 微基准（单次调用成本）

| 微基准 | 配置 | ns/调用 | 事件/调用 | 说明 |
|---|---|---:|---:|---|
| record | OFF | 1.53 | 0.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | SAMPLED | 25.74 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | SAMPLED_ALL | 26.29 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | FULL_NO_CAPTURE | 26.18 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | FULL | 25.76 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record_sampled32 | OFF | 1.79 | 0.0000 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | SAMPLED | 4.94 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | SAMPLED_ALL | 4.99 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | FULL_NO_CAPTURE | 4.92 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | FULL | 5.04 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| lookup | OFF | 1.83 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | SAMPLED | 7.14 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | SAMPLED_ALL | 8.21 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | FULL_NO_CAPTURE | 8.22 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | FULL | 7.77 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| stack | OFF | 1.84 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | SAMPLED | 2.17 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | SAMPLED_ALL | 8.59 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | FULL_NO_CAPTURE | 2.11 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | FULL | 667.16 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| config | OFF | 0.48 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | SAMPLED | 1.18 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | SAMPLED_ALL | 1.11 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | FULL_NO_CAPTURE | 1.13 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | FULL | 1.10 | 0.0000 | DebugConfig::Enabled：原子掩码读 |

## 4. 事件构成（FULL 配置，按类别）

| 场景 | 事件明细 |
|---|---|
| mutex4 | mutex=17212 |
| mutex8 | mutex=15856 |
| rwmix | mutex=50432,rw=35608,cv=22237 |
| mpmc | park=7544,wake=5290,sched=2028,ch=20935,pc=8103,life=321 |
| pc_rtt | park=300000,wake=200000,sched=50034,pc=300032,life=12507 |
| wg | wg=6256 |
| select2 | park=149979,wake=99986,sched=25180,ch=169527,pc=150259,life=6257 |
| select4 | park=149958,wake=99972,sched=25197,ch=178880,pc=150349,life=6256 |
| mixed | park=7907,wake=5270,sched=1331,mutex=14065,ch=19390,pc=7940,life=341 |

## 5. 数据完整性与说明

- 所有 (配置, 场景) 组合均达到 3 轮有效样本。
- dropped_threads 合计：0；stack_samples dropped 峰值：0
- 窗口覆盖倍数峰值：898.5×（每线程 320 槽，事件多于此即回绕）
- `SAMPLED` 关闭 primitives/stacks/chains 类别；`SAMPLED_ALL` 打开全部类别但 shift=5。`GO2CPP_DEBUG_SAMPLE_SHIFT` 只降低 快路径采样事件与栈捕获频率，**不降低资源事件的记录频率**。
- `FULL_NO_CAPTURE` 关闭 stacks/chains 类别，用于把「事件编码 + 资源路径」与「backtrace / Fiber 链」拆开归因。
- valid=0（loadavg 超限）与 rc≠0 的轮次已从汇总剔除，原始 TSV 保留。

