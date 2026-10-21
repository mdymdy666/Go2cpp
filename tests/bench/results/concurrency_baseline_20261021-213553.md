# 并发原语调试监控开销基线

- 原始数据：`concurrency_baseline_20261021-213553.tsv`
- 汇总数据：`concurrency_baseline_20261021-213553_summary.tsv`
- 每场景操作数 N=50000，每配置 3 轮取中位数，延迟采样上限 LAT_N，loadavg 上限 2.0
- OFF 构建：`/UserData/CodexWorkSpace/Go2Cpp-work/build-work`；ON 构建：`/UserData/CodexWorkSpace/Go2Cpp-work/build-debug-conc`
- 吞吐 = ops / 墙钟时间；p50/p99 含逐操作 `steady_clock` 计时成本（约 20–30 ns）
- 事件量 = `EventRing::TotalCountAll()` 累计增量；OFF 构建恒 0
- 窗口覆盖倍数 = 事件数 /（活跃 ring 数 × 每线程 320 槽）；>1 表示环形窗口在压测期间至少被覆盖一遍，历史事件必有丢失（事件环没有 dropped 计数器，这是下界）

## 1. 场景 × 配置（吞吐 / 延迟 / 事件）

| 场景 | 配置 | 轮数 | 吞吐 (ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | 事件/操作 | 窗口覆盖倍数 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| mutex4 | OFF | 3 | 22479669 | 32.0 | 1693.0 | 91488.0 | 0.000 | 0.00 |
| mutex4 | SAMPLED | 3 | 21807958 | 33.0 | 1335.0 | 109147.0 | 0.000 | 0.00 |
| mutex4 | SAMPLED_ALL | 3 | 3698898 | 239.0 | 5872.0 | 169453.0 | 2.257 | 88.17 |
| mutex4 | FULL_NO_CAPTURE | 3 | 3812419 | 120.0 | 5427.0 | 163140.0 | 2.145 | 83.80 |
| mutex4 | FULL | 3 | 117004 | 3115.0 | 607069.0 | 1189206.0 | 2.345 | 91.59 |
| mutex8 | OFF | 3 | 18583181 | 32.0 | 825.0 | 245179.0 | 0.000 | 0.00 |
| mutex8 | SAMPLED | 3 | 21601967 | 33.0 | 1031.0 | 246126.0 | 0.000 | 0.00 |
| mutex8 | SAMPLED_ALL | 3 | 3023987 | 206.0 | 6730.0 | 440673.0 | 2.037 | 39.78 |
| mutex8 | FULL_NO_CAPTURE | 3 | 3549810 | 119.0 | 3146.0 | 361493.0 | 2.080 | 40.63 |
| mutex8 | FULL | 3 | 121853 | 2235.0 | 1112512.0 | 2128196.0 | 2.347 | 45.84 |
| rwmix | OFF | 3 | 3680455 | 43.0 | 14924.0 | 929777.0 | 0.000 | 0.00 |
| rwmix | SAMPLED | 3 | 2825315 | 44.0 | 79110.0 | 1016634.0 | 0.000 | 0.00 |
| rwmix | SAMPLED_ALL | 3 | 109485 | 858.0 | 1552410.0 | 4978894.0 | 8.097 | 158.15 |
| rwmix | FULL_NO_CAPTURE | 3 | 472795 | 402.0 | 576326.0 | 3942034.0 | 8.160 | 159.38 |
| rwmix | FULL | 3 | 56100 | 18050.0 | 914502.0 | 7566625.0 | 9.221 | 180.09 |
| mpmc | OFF | 3 | 7756971 | 30.0 | 39676.0 | 131259.0 | 0.000 | 0.00 |
| mpmc | SAMPLED | 3 | 2381347 | 30.0 | 160508.0 | 364431.0 | 0.225 | 7.80 |
| mpmc | SAMPLED_ALL | 3 | 693154 | 101.0 | 371984.0 | 838276.0 | 1.405 | 48.79 |
| mpmc | FULL_NO_CAPTURE | 3 | 1650495 | 70.0 | 159953.0 | 447748.0 | 1.420 | 49.32 |
| mpmc | FULL | 3 | 673718 | 1253.0 | 287454.0 | 884334.0 | 1.250 | 43.41 |
| pc_rtt | OFF | 3 | 2752882 | 681.0 | 841.0 | 3097.0 | 0.000 | 0.00 |
| pc_rtt | SAMPLED | 3 | 1054390 | 1777.0 | 4001.0 | 16208.0 | 9.000 | 937.53 |
| pc_rtt | SAMPLED_ALL | 3 | 206987 | 9163.0 | 24094.0 | 75102.0 | 13.002 | 1354.37 |
| pc_rtt | FULL_NO_CAPTURE | 3 | 904406 | 2097.0 | 4597.0 | 21019.0 | 13.001 | 1354.23 |
| pc_rtt | FULL | 3 | 171444 | 10887.0 | 26159.0 | 67140.0 | 13.002 | 1015.81 |
| wg | OFF | 3 | 28541942 | 85.0 | 1735.0 | 9780.0 | 0.000 | 0.00 |
| wg | SAMPLED | 3 | 17688521 | 53.0 | 1426.0 | 24562.0 | 0.000 | 0.00 |
| wg | SAMPLED_ALL | 3 | 9796406 | 117.0 | 2703.0 | 38749.0 | 1.000 | 31.25 |
| wg | FULL_NO_CAPTURE | 3 | 11773997 | 117.0 | 2757.0 | 24296.0 | 1.000 | 31.25 |
| wg | FULL | 3 | 2902524 | 1043.0 | 4098.0 | 34704.0 | 1.000 | 31.25 |
| select2 | OFF | 3 | 1011322 | 1191.0 | 2786.0 | 14428.0 | 0.000 | 0.00 |
| select2 | SAMPLED | 3 | 229088 | 3533.0 | 16575.0 | 50827.0 | 9.001 | 281.28 |
| select2 | SAMPLED_ALL | 3 | 89065 | 12441.0 | 56674.0 | 108874.0 | 18.761 | 586.29 |
| select2 | FULL_NO_CAPTURE | 3 | 210090 | 4034.0 | 17464.0 | 49510.0 | 18.802 | 587.56 |
| select2 | FULL | 3 | 64054 | 18512.0 | 53544.0 | 113660.0 | 18.811 | 489.86 |
| select4 | OFF | 3 | 699409 | 1516.0 | 1839.0 | 14875.0 | 0.000 | 0.00 |
| select4 | SAMPLED | 3 | 163811 | 4377.0 | 18550.0 | 58080.0 | 9.002 | 281.30 |
| select4 | SAMPLED_ALL | 3 | 75453 | 13460.0 | 41731.0 | 109139.0 | 20.264 | 633.25 |
| select4 | FULL_NO_CAPTURE | 3 | 152936 | 5026.0 | 27960.0 | 51588.0 | 20.306 | 528.81 |
| select4 | FULL | 3 | 51768 | 21647.0 | 59893.0 | 130421.0 | 20.318 | 529.12 |
| mixed | OFF | 3 | 75689856 | 43.0 | 4822.0 | 16010.0 | 0.000 | 0.00 |
| mixed | SAMPLED | 3 | 36313783 | 54.0 | 14853.0 | 68855.0 | 0.079 | 14.81 |
| mixed | SAMPLED_ALL | 3 | 6658651 | 193.0 | 40562.0 | 133883.0 | 0.802 | 150.33 |
| mixed | FULL_NO_CAPTURE | 3 | 17998246 | 161.0 | 23341.0 | 84136.0 | 0.809 | 151.74 |
| mixed | FULL | 3 | 1163185 | 3344.0 | 318927.0 | 382744.0 | 0.840 | 157.40 |

## 2. 相对 OFF 的开销倍数（同场景、中位数）

| 场景 | 配置 | 慢倍数 | p50 倍数 | p99 倍数 | 事件/操作 |
|---|---|---:|---:|---:|---:|
| mutex4 | SAMPLED | 1.03 | 1.03 | 0.79 | 0.000 |
| mutex4 | SAMPLED_ALL | 6.08 | 7.47 | 3.47 | 2.257 |
| mutex4 | FULL_NO_CAPTURE | 5.90 | 3.75 | 3.21 | 2.145 |
| mutex4 | FULL | 192.13 | 97.34 | 358.58 | 2.345 |
| mutex8 | SAMPLED | 0.86 | 1.03 | 1.25 | 0.000 |
| mutex8 | SAMPLED_ALL | 6.15 | 6.44 | 8.16 | 2.037 |
| mutex8 | FULL_NO_CAPTURE | 5.23 | 3.72 | 3.81 | 2.080 |
| mutex8 | FULL | 152.51 | 69.84 | 1348.50 | 2.347 |
| rwmix | SAMPLED | 1.30 | 1.02 | 5.30 | 0.000 |
| rwmix | SAMPLED_ALL | 33.62 | 19.95 | 104.02 | 8.097 |
| rwmix | FULL_NO_CAPTURE | 7.78 | 9.35 | 38.62 | 8.160 |
| rwmix | FULL | 65.61 | 419.77 | 61.28 | 9.221 |
| mpmc | SAMPLED | 3.26 | 1.00 | 4.05 | 0.225 |
| mpmc | SAMPLED_ALL | 11.19 | 3.37 | 9.38 | 1.405 |
| mpmc | FULL_NO_CAPTURE | 4.70 | 2.33 | 4.03 | 1.420 |
| mpmc | FULL | 11.51 | 41.77 | 7.25 | 1.250 |
| pc_rtt | SAMPLED | 2.61 | 2.61 | 4.76 | 9.000 |
| pc_rtt | SAMPLED_ALL | 13.30 | 13.46 | 28.65 | 13.002 |
| pc_rtt | FULL_NO_CAPTURE | 3.04 | 3.08 | 5.47 | 13.001 |
| pc_rtt | FULL | 16.06 | 15.99 | 31.10 | 13.002 |
| wg | SAMPLED | 1.61 | 0.62 | 0.82 | 0.000 |
| wg | SAMPLED_ALL | 2.91 | 1.38 | 1.56 | 1.000 |
| wg | FULL_NO_CAPTURE | 2.42 | 1.38 | 1.59 | 1.000 |
| wg | FULL | 9.83 | 12.27 | 2.36 | 1.000 |
| select2 | SAMPLED | 4.41 | 2.97 | 5.95 | 9.001 |
| select2 | SAMPLED_ALL | 11.35 | 10.45 | 20.34 | 18.761 |
| select2 | FULL_NO_CAPTURE | 4.81 | 3.39 | 6.27 | 18.802 |
| select2 | FULL | 15.79 | 15.54 | 19.22 | 18.811 |
| select4 | SAMPLED | 4.27 | 2.89 | 10.09 | 9.002 |
| select4 | SAMPLED_ALL | 9.27 | 8.88 | 22.69 | 20.264 |
| select4 | FULL_NO_CAPTURE | 4.57 | 3.32 | 15.20 | 20.306 |
| select4 | FULL | 13.51 | 14.28 | 32.57 | 20.318 |
| mixed | SAMPLED | 2.08 | 1.26 | 3.08 | 0.079 |
| mixed | SAMPLED_ALL | 11.37 | 4.49 | 8.41 | 0.802 |
| mixed | FULL_NO_CAPTURE | 4.21 | 3.74 | 4.84 | 0.809 |
| mixed | FULL | 65.07 | 77.77 | 66.14 | 0.840 |

## 3. 微基准（单次调用成本）

| 微基准 | 配置 | ns/调用 | 事件/调用 | 说明 |
|---|---|---:|---:|---|
| record | OFF | 1.61 | 0.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | SAMPLED | 25.81 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | SAMPLED_ALL | 25.67 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | FULL_NO_CAPTURE | 26.44 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record | FULL | 26.05 | 1.0000 | EventRing::Record 直接调用：112B 记录 + TotalCount fetch_add + 环写 |
| record_sampled32 | OFF | 1.89 | 0.0000 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | SAMPLED | 4.75 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | SAMPLED_ALL | 4.69 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | FULL_NO_CAPTURE | 4.67 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| record_sampled32 | FULL | 4.70 | 0.0312 | RecordSampled(every=32)：采样判断 + 命中时同上 |
| lookup | OFF | 1.85 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | SAMPLED | 8.58 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | SAMPLED_ALL | 8.72 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | FULL_NO_CAPTURE | 8.66 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| lookup | FULL | 8.48 | 0.0000 | FindResourceByAddress：shared_mutex + 哈希查找 |
| stack | OFF | 1.84 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | SAMPLED | 2.38 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | SAMPLED_ALL | 11.12 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | FULL_NO_CAPTURE | 2.36 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| stack | FULL | 660.01 | 0.0000 | CaptureStackSample：backtrace + 栈表去重锁 |
| config | OFF | 0.48 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | SAMPLED | 1.11 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | SAMPLED_ALL | 1.11 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | FULL_NO_CAPTURE | 1.13 | 0.0000 | DebugConfig::Enabled：原子掩码读 |
| config | FULL | 1.12 | 0.0000 | DebugConfig::Enabled：原子掩码读 |

## 4. 事件构成（FULL 配置，按类别）

| 场景 | 事件明细 |
|---|---|
| mutex4 | mutex=119753 |
| mutex8 | mutex=117836 |
| rwmix | mutex=309708,rw=112952,cv=38383 |
| mpmc | park=4155,wake=2906,sched=6002,ch=106061,pc=4582,life=1325 |
| pc_rtt | park=300000,wake=200000,sched=400116,pc=300126,life=100006 |
| wg | wg=50003 |
| select2 | park=149982,wake=99988,sched=200283,ch=289842,pc=150439,life=50000 |
| select4 | park=149985,wake=99990,sched=200324,ch=365063,pc=150544,life=50001 |
| mixed | park=9154,wake=6108,sched=12729,mutex=102175,ch=107117,pc=8966,life=3057 |

## 5. 数据完整性与说明

- 所有 (配置, 场景) 组合均达到 3 轮有效样本。
- dropped_threads 合计：0；stack_samples dropped 峰值：0
- 窗口覆盖倍数峰值：1354.4×（每线程 320 槽，事件多于此即回绕）
- `SAMPLED` 关闭 primitives/stacks/chains 类别；`SAMPLED_ALL` 打开全部类别但 shift=5。`GO2CPP_DEBUG_SAMPLE_SHIFT` 只降低 快路径采样事件与栈捕获频率，**不降低资源事件的记录频率**。
- `FULL_NO_CAPTURE` 关闭 stacks/chains 类别，用于把「事件编码 + 资源路径」与「backtrace / Fiber 链」拆开归因。
- valid=0（loadavg 超限）与 rc≠0 的轮次已从汇总剔除，原始 TSV 保留。

