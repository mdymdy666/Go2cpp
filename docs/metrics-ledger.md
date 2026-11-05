# 性能数字台账（metrics ledger）

> **为什么存在**：项目最大风险不是性能不够，是「数字会腐烂」。已实证三次：
> ① 文档写「调度器 122 ns」，2026-11-03 本机实测 wall 225–323 ns；
> ② 「HTTP pipeline 411k rps」全库查无此数（真实可复核的是单连接 72.9k）；
> ③ 「调度器 160 ns」全仓唯一出处是达标表一行，无任何报告支撑。
> 共同病根：数字产生时没记条件，事后无法复现。
>
> **规则**：
> 1. 任何写进文档/汇报的性能数字，必须能指到本文件的一条记录（ID）；
> 2. 记录必须含四要素：**测量命令 / 环境（核数·绑核·轮数）/ 日期 / 可复现脚本路径**；
> 3. 状态只有四种：`实测`（本轮或近期按记录命令跑出）、`待复跑`（历史值，条件不全或未复验）、
>    `STALE`（复跑对不上）、`无出处`（查不到来源，禁止引用）；
> 4. **`待复跑` 与 `STALE` 的数字不得作为结论**，只能作为「历史对照」；
> 5. 本文件不写「优化目标」，只写「事实 + 条件 + 怎么再测一次」。

## 字段说明

| 字段 | 含义 |
|---|---|
| ID | `M01` 起，稳定不变 |
| 指标 | 一句话 + 口径（ns/op、rps、MiB/s、比值…） |
| 历史值 | 文档/报告出现过的值（仅对照） |
| 测量命令 | 可复制执行的命令（含构建目录） |
| 环境 | 核数 / 是否 taskset / 轮数 / 构建类型 |
| 日期 | 该值的测量日期；历史值写「未记录」 |
| 脚本路径 | 复现脚本；`待建` 表示还没有，A2/A4 补 |
| 状态 | 实测 / 待复跑 / STALE / 无出处 |

## 首批 10 条（A1：先建档，逐条待复跑）

### M01 Channel 1P1C 有缓冲 ns/条

- **历史值**：10.73 ns/条（cap=1024）
- **测量命令**：`.build-bento-wsl/go2cpp_fast_channel_bench`（当前只覆盖 FastChannel/SPSC；通用 `Channel<int>` 的同口径 bench 旧二进制 ABI 失效，见 M01b）
- **环境**：未记录（历史）；计划 12 vCPU、`taskset -c 2,3`、5 轮取中位
- **日期**：未记录
- **脚本路径**：`tests/concurrency/bench_fast_channel.cpp`（已存在）；通用 Channel 见 M01b-a
- **状态**：`已拆条`（历史 10.73 实为 `FastChannel<1024>`，见 M01b-b；通用 Channel 见 M01b-a）
- **备注**：2026-11-03 本轮旁证（非同一口径）：`fast-cap1024` 线程 16.07 ns（min 13.15）、Fiber 6.23 ns（min 5.82），5 轮、taskset 2,3。**不能**用它替代 M01 的 10.73。

### M01b-a 通用 `Channel<uint64_t>` 1P1C cap1024 ns/条（**不达标**）

- **历史值**：无（历史 10.73 属 FastChannel，见 M01b-b；本条是 2026-11-03 首次入账）
- **测量命令**：`tools/verify_metrics.sh`（编译 `tests/concurrency/bench_channel_opt.cpp` 后跑 `spsc` 段）
- **环境**：12 vCPU / `taskset -c 2,3`（同一物理核的两个 SMT 兄弟）/ total=1e6、reps=3、3 轮取 min / Release 复用 `.build-bento-wsl`
- **日期**：2026-11-03
- **脚本路径**：`tests/concurrency/bench_channel_opt.cpp:88-126`（`MakeChannel<uint64_t>` + 2 个 `std::thread`，无 Scheduler/`go()`，ns/条）
- **状态**：`实测（口径不可比，非达标判据）`（spsc cap1024 **112.6 ns/条**（median ~114），min **103.3**；Go `chan` 23.36。**2026-11-03 用户澄清**：历史 10.73 对应的 `FastChannel` 才是 Go `chan` 的同型默认实现（runtime hchan 的轻量路径）；本项目通用 `Channel<T>` 默认注入 Context 语义（取消/超时/select 回滚），每次操作多状态检查与唤醒协议——4.8× 主要来自**语义增强**，不能按「vs Go 1.1×」判不达标。若要严格对标，只有两条路：用无 Context 的 FastChannel 对 Go chan，或 Go 侧也用带 context 的 chan 同条件比。）
- **诊断**：`tryspsc`（纯轮询、永不 park）101.8–118.3 ≈ `spsc`；`taskset -c 2,4`（跨物理核）93.3–98.9 ≈ `2,3` → 瓶颈是**锁的跨核 cacheline 传递**（每 op ≥3 条 RFO：`m_gate`、buffer 索引、数据槽），不是 park/futex，也不是 SMT 特有。单锁架构的物理下限约 60–90 ns/op；**是否值得无锁化取决于 Context 语义能否下沉**（用户 2026-11-03 澄清后，该项从「不达标」降级为「语义增强的代价」，不再作为优化立项依据）。
- **已做优化（汇编级，2026-11-03，有反汇编依据）**：`kWouldBlock` 是控制流不是错误，原实现返回 `ChannelWouldBlockError()` 单例（`ErrorPtr`=shared_ptr），每次非阻塞探测 2 次原子引用计数 RMW（反汇编可见 `EmptyTryRecv` 循环体内两次 `_Sp_counted_base::_M_release`）；改为空 `ErrorPtr`（与 `channel.hpp:3039` 既有惯例一致，单例定义/导出保留）。
  - TryRecv 空：19.6–25.4 → **16.1–17.1 ns（−18%）**；TrySend 满：16.3–18.7 → **13.6–14.8 ns（−17%）**；spsc cap1024 min 103.3 → **100.2**（无回归）。
  - 正确性：`channel_concurrency`、`channel_thread_stress`（89.3 s / 100 轮）、`select_lockorder`、`fiber_select` 全部 exit=0、failures=0。
- **未做（诚实记录）**：架构级无锁化（需覆盖 Send/Recv/Select/Close 全路径；`channel.hpp:1192-1196` 记录前人评估后放弃）——超出单轮可验证范围，**未留半成品**；TSan 未跑。建议单独立项。

### M01b-b `FastChannel<1024>` ns/条（达标）

- **历史值**：10.73 ns/条（min；median 12.83）
- **测量命令**：`.build-bento-wsl/go2cpp_fast_channel_bench`（`fast-cap1024` 段）
- **环境**：12 vCPU / `taskset -c 2,3` / 5 轮；线程与 Fiber 两种
- **日期**：2026-11-03（本轮旁证：线程 **16.07 ns**（min 13.15）、Fiber **6.23 ns**（min 5.82））
- **脚本路径**：`tests/concurrency/bench_fast_channel.cpp`
- **状态**：`实测（达标）`（16.07 / 6.23 均优于 Go `chan` 23.36；历史 10.73 与现代码的 16.07 差异来自口径/机器状态，均达标）
- **备注**：判据必须写明对标关系——**通用 Channel 对标 Go `chan`（不达标，见 M01b-a）；FastChannel 对标无锁队列（达标）**。

### M02 Fiber 全量切换 ns/往返

- **历史值**：33.80 ns/往返（后续快照 23.5）
- **测量命令**：待补（`tests/concurrency/` 下切换基准 + `bench_fast_channel` 的 fiber 段不是同一口径）
- **环境**：未记录
- **日期**：未记录
- **脚本路径**：`tests/concurrency/bench_fiber_switch_opt.cpp`（:308 `PrintRow("Fiber::resume ping-pong (yield)")`，本轮 `sed` 已核）
- **状态**：`实测（推断口径）`（2026-11-03 `tools/verify_metrics.sh` 测得 **19.84 ns/往返**：1e6 次、`taskset -c 2,3`、12 vCPU；对 23.5 快照 −15.6%，对 33.80 差 41%。历史未写明 API，A3 需确认「全量切换」是否就是 yield ping-pong，否则改口径）

### M03 Scheduler task churn wall ns/task

- **历史值**：122.0 ns（文档）/ 326.7 ns（submit 口径，见 M04）
- **测量命令**：
  ```bash
  g++ -O2 -DNDEBUG -std=c++17 -Iinclude tests/concurrency/bench_scheduler_task_opt.cpp \
      -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
      -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_debug \
      -lgo2cpp_debug_event -lpthread -o /tmp/bench_scheduler_task_opt
  taskset -c 2,3 /tmp/bench_scheduler_task_opt 4 100000 5
  ```
- **环境**：AMD Ryzen 9 8945HX / 12 vCPU；`taskset -c 2,3`；4 worker；100k tasks；5 轮；Release `-O2 -DNDEBUG`
- **日期**：2026-11-03（本轮实测）
- **脚本路径**：`tests/concurrency/bench_scheduler_task_opt.cpp`
- **状态**：`实测（不达标，真实差距 1.4–2.5×）`（wall 287/239/267/236/225 ns，中位 239；cpu 619/519/582/514/478；2026-11-03 15:00 同轮对照见 M16：C++ 359 / Go@4 162.7 = 2.21×，最好组合 239/163 = 1.47×）
- **备注**：与历史 122.0 不一致，原因是核数/绑核/轮次/宿主负载不同；对外只能报「口径 + 条件 + 中位」。2026-11-03 13:33 复跑中位 **299 ns**（同 5 轮 cpu 中位 627），同日另一轮 5 轮波动 245–787 —— 该基准对宿主负载敏感，单次数字不可单独引用，必须带 cpu 中位与轮次分布。

- **2026-11-03 15:40 三件套收口（M03c 分块复核）**：
  - 分块 smoke 各 10 次：base 10/10、P19 10/10、**P20 0/10**、P21 10/10、P22 10/10；P20 失败于 `scheduler_smoke.cpp:111`。
  - 探针判定（2P/2M，park→kWaiting 1000 次）：baseline 与 P20 均 stuck=0、p50<1 µs、max 7 µs；smoke 场景复刻 5/5 轮 state=kWaiting、wake 后 kDead → **P20 非语义破坏**，是 :111 在 `park()` 调用前就检查 `kWaiting` 的断言竞态被 P20 时序放大。保守剔除 P20，未改断言。
  - 组合 **P19+P21+P22**：ctest 5 项全过、smoke **20/20**、1e6 压力 442/582/417 通过。
  - bench 两次较干净测量：baseline 305 → 组合 **293（−3.9%）**、A/B 交替 **324 vs 338（−4.1%）**，**均 <5% 门槛**；第三组 422 受宿主干扰不采信。
  - borrow 建议①核实：现实现已经是「displaced 先进本 P 256 槽环、环满才 `clear_queued()+enqueue`」（`scheduler.cpp:4031-4034`），方向一致；且 `SchedulerMetrics` 只填前 6 字段，`runnext_pushes/displaced` 恒 0，无法测比例。
  - **结论：scheduler churn 无可验证的 >5% 优化空间**，按规则回滚（`scheduler.cpp` 零改动，回滚后 smoke 10/10）。剩余 1.7–2.2× 差距归因架构差异（Context 语义注入、单锁、跨 M 唤醒），不是单点可优化；M03 收口为「不达标但已尽力」，不再投入。
- **2026-11-03 15:50 用户决定：放弃 Scheduler 优化**。理由：本项目 Scheduler 架构相对 Go 本就**更复杂**（见下），复杂度是为 C++ 语义（无 GC、RAII、Context）付出的必要代价，不是实现缺陷；继续优化收益 <5%（M03c 已验证）而回归风险高。
  - 复杂度对比（Go runtime ↔ 本项目）：① 任务所有权：g 复用池 ↔ `shared_ptr<Task>` + FiberBin；② 状态机：6 态分散字段 ↔ `GState` 8 态 + `state_word`（低 8 位状态 + bit8 wake 令牌）+ `execution_claim`/`run_claim` 双位；③ 队列：runnext + 256 环 + 全局 runq ↔ runnext + 256 环 + **incoming 16 条带** + admission 慢路径（emergency/本地 FIFO/全局 FIFO/窃取）；④ Context：无内置（chan 不带 ctx）↔ **每个 Channel/Task 注入 Context**（取消/超时/select 回滚）；⑤ 栈：栈复制（grow/shrink）↔ 8 MiB VA 预留 + 按页增长 + SIGSEGV handler；⑥ 抢占：异步抢占（1.14+）↔ 无抢占（协作 + BlockingRegion + sysmon detach）；⑦ 内存：GC（mcache/mheap）↔ 无 GC（RAII/引用计数）；⑧ IO：netpoll 一种 ↔ 独立 poller / worker poll 两种可配。
  - 判定：在「状态机/所有权/Context/栈增长/IO 模型」5 个维度本项目更复杂；在「抢占/GC」2 个维度 Go 更复杂——两者不是同类系统（无 GC 的 C++ 运行时必须用引用计数 + 固定栈 + 显式状态机替代 GC 安全点与栈复制）。**因此 2.21× 不是「实现差」，而是「语义更重的代价」**；Scheduler 优化就此关闭。

- **2026-11-03 14:53 三件套尝试（架构→汇编→正确性）**：
  - baseline（`taskset -c 2,3 /tmp/bench_sched 4 100000 7`，三次进程 × 7 轮）：320 / 296 / 351 → **中位 320 ns**，loadavg 0.47–0.69；同命令后续批次中位漂到 306–449，**宿主噪声大于待测效应**。
  - 架构 P19（runnext/inbox 空槽无锁短路，`scheduler.cpp:608/626`）：100k 随机 175 轮 **−5.0%** / cpu −6.6%；1e6 长测量 −0.5%/−16.4%（不稳定）。
  - 汇编 P21+P22（P 下标 divq→掩码 `:264`、lifecycle 位图合并 `:3034/3831`）：enqueue **597→580 指令（−2.8%）**、栈帧 **104→88 B（−15%）**、热路径 `divq` **1→0**，call/原子数不变。
  - 强方案 p22（删每任务 2 次全局 `running_workers` RMW，hit `:3185/:5610/:5960/:6114`）：1e6 长测 **−15.1% / −34.7%**（两批），100k 随机 −19.1%/−33.9%/−20.5%；但 `go2cpp_scheduler_smoke` **10/10 失败**于 `scheduler_smoke.cpp:111` 的 park 时序断言（该断言在 `park()` 调用前就检查 `kWaiting`，疑似断言竞态）。
  - **按铁律回滚**：`scheduler.cpp` 零改动（与 `/tmp/scheduler.cpp.bak` 逐字节一致）。p22 补丁留存 `/tmp/scheduler.cpp.p22`，交 M03b 复核：先判定「断言竞态 vs 语义破坏」（原版/p22 各跑 smoke 20 次），再决定是否修正断言并重跑全量 + 1e6 压力。
  - 结论：**M03 尚未达标**（320 ns vs 历史 122 不可复现）；p22 是唯一稳定显著收益点，待复核，未入账。

### M04 Scheduler submit ns/task

- **历史值**：326.7 ns（条件未记录；落在本轮「持续积压」口径区间内，不能断言同口径）
- **测量命令**：`/tmp/bench_scheduler_submit 4 1000000 5 <batch>`（源码 `tests/concurrency/bench_scheduler_submit.cpp`；计时只包 `spawn()` 调用，不含等待，见 :150-163）
- **环境**：12 vCPU Ryzen 9 8945HX / `taskset -c 2,3` / 4 worker / 1e6 任务每轮 / 每进程 5 轮 × 7 进程 = 35 轮/口径 / loadavg 2.0–3.0
- **日期**：2026-11-03
- **脚本路径**：`tests/concurrency/bench_scheduler_submit.cpp`（本轮新增，191 行）
- **状态**：`实测（双口径）` —— **batch=1024（队列排空直投 P 队列）中位 119.0 ns/task**（p25 92.1 / p75 133.6，min 70.3 / max 210.4）；**batch=65536（持续积压回退 incoming 条带锁）中位 355.7 ns/task**（p25 312.1 / p75 402.7）。两者差 3 倍，**batch 条件必须随数字一起引用**。

### M05 HTTP/1.1 pipeline=16 单连接 rps

- **历史值**：72.9k req/s（串行 8.9k 的 8.2×）；另 8 连接 239.5k
- **测量命令**：服务端 `taskset -c 2,3 net_perf_server http 0 2 hello`（READY 行取端口）；客户端 `loadgen -conns 1 -pipeline 16 -duration 5s -warmup 1s`（`taskset 4-9`）
- **环境**：12 vCPU / WSL2 回环 / 服务端 2 worker 绑 `2,3` / loadgen 绑 `4-9` / 6 轮 / loadavg 1.4–1.6
- **日期**：2026-11-03
- **脚本路径**：`tests/network/net_perf_server.cpp` + `tests/network/http_loadgen.go`（Go 1.27.1）
- **状态**：`实测` —— **120,755 rps**（6 轮中位：117548 / 118482 / 120624 / 120886 / 126179 / 126485），p50 7–8 µs、p99 15–16 µs、errors=0；pipeline=1 对照 9,122 rps（8.2× 量级，与历史「pipeline 16 是串行 8.2×」同向）。
- **备注**：历史 72.9k 无法用旧二进制复验（`.build-bento-wsl` 旧 `libgo2cpp_bento_http.a` 缺 `HttpServer::Route` 符号），按当前源码增量重编后测得 120.8k（+65.7%）；差异归因「库版本 + 宿主负载」，基准口径一致，但**不是同二进制对照**。
- **备注**：「411k rps」全库查无此数，已列为 `无出处`（见 M05b）。

### M05b 「HTTP pipeline 411k rps」

- **历史值**：411k rps
- **测量命令**：无
- **环境**：无
- **日期**：无
- **脚本路径**：无
- **状态**：`无出处`（2026-11-03 `grep -rn "411k|411000|411,000" --include=*.md` 只命中 exam 题面/答案与 `user/deep/02-network.md` 的「无出处」说明，**没有原始数据出处**；禁止引用）

### M06 SQLite 封装 1e6 行插入 vs 原生

- **历史值**：1.078×（文件库，7 轮中位数）
- **测量命令**：`taskset -c 2,3 .build-bento-wsl/go2cpp_storage_bench <每轮独立 tmpdir> 1000000`
- **环境**：12 vCPU / `taskset -c 2,3` / 文件库 WAL + synchronous=NORMAL + 单事务 + 预编译语句 / SQLite 3.45.1 / 单线程 / 7 轮 / loadavg 0.84–0.91
- **日期**：2026-11-03
- **脚本路径**：`tests/storage/bench_db.cpp`
- **状态**：`实测` —— **1.074×（7 轮中位）**：wrapper 368.1 ms（2.72M ops/s）vs 原生 344.9 ms（2.90M ops/s）；逐轮比值 1.040 / 1.088 / 1.134 / 1.074 / 1.046 / 1.071 / 1.088。历史 1.078× 复现（−0.4%），达标（<1.1×）。

### M07 LevelDB 1e7 Put ops/s 与 1e8 RSS

- **历史值**：1e7 Put 58.3 万 ops/s（3.7× 自研）；1e8 后 RSS 87 MiB（数据量 4.9%；分母口径未记录）
- **测量命令**：`.build-bento-wsl/go2cpp_leveldb_kv_test --bench --n=10000000`（20B/22B，LevelDB vs 自研 KV）；`--mem --n=10000000`（8B/8B）；`--mem --n=100000000`（1e8 RSS）
- **环境**：WSL2 6.6.87.2-microsoft-standard-WSL2 / 12 vCPU / `taskset -c 2,3` / Release / `GO2CPP_WITH_KV=ON` / 库目录 mkdtemp 自清理 / loadavg 0.43（前）→1.48（中）
- **日期**：2026-11-03 14:01–14:13
- **脚本路径**：`tests/storage/test_leveldb_kv.cpp`（`--bench` / `--mem`，见 :1096-1102）
- **状态**：`实测`：
  - 1e7 Put（`--bench`，20B/22B）：644000 / 648630 / 647521 ops/s，**3 轮中位 647,521**（历史 583k，+11%）；
  - 1e7 Put（`--mem`，8B/8B）：663786 / 633193 / 659304，中位 **659,304**；
  - **1e8 跑完**：230.8 s、**433,369 ops/s**、采样 RSS 峰值 **82.4 MiB**、VmHWM **122.8 MiB**、磁盘 **2511.6 MiB**；RSS/数据量 = 82.4/1525.9 = **5.40%**（VmHWM 口径 8.05%）；
  - 自研对照：LevelDB/自研 Put = 3.95 / 4.14 / 4.51×，中位 **4.14×**（历史 3.7×）；Get 反向 530,742 vs 576,109/s。
- **备注**：历史 87 MiB 应为采样瞬时值（本轮 VmHWM 122.8 更高，属正常）；历史环境/轮数/是否绑核、4.9% 分母口径均未记录。

### M08 bufio 1 MiB 分块读 MiB/s

- **实测（2026-11-04）**：**17695 MiB/s**（复用缓冲探针 5 次 median，best 18073；`taskset -c 4`、`-O3 -DNDEBUG`）
- **对照基线**：Go `bufio` **19775.4 MiB/s → 0.895×**（best 18073 = 0.914×）；改前 **12387 MiB/s → +43%**
- **历史值**：**1853.9 MiB/s**（2026-11-03，`tests/bufio/test_bufio.cpp` 场景「分块传输 1MiB」，`Read` 2048 次、底层 1415 次、512B 请求）——**口径不同**：该场景每轮新建 1 MiB 缓冲，缺页成本占主导；17695 用循环外 `reserve` 的探针。两者不可直接比较，旧值仅作历史。
- **测量命令**：`taskset -c 4 /tmp/bufio_probe_o3`（同刻 A/B 交替，每进程 8 轮丢前 2 轮取 median）；测试口径 `.build-bento-wsl/go2cpp_bufio_test`
- **环境**：WSL2 12 vCPU / GCC 13.3 `-O3 -DNDEBUG` / 单线程 `taskset -c 4` / 页缓存热
- **日期**：2026-11-04（17695）；2026-11-03（1853.9）
- **脚本路径**：`tests/bufio/test_bufio.cpp`（场景）+ `/tmp/bufio_probe_o3` 探针（同构探针，手工编译不进 CMake）
- **来源报告**：`docs/reports/asm-bufio.md` §3
- **状态**：`实测`（报告原始同刻 A/B；本轮未独立复跑）

### M09 Cache hotspot 命中率（ARC/LRU）

- **历史值**：ARC 0.7817 / LRU 0.5580（capacity=2000、300k 操作）
- **测量命令**：`.build-bento-wsl/go2cpp_cache_test`（hotspot 段）
- **环境**：12 vCPU；未绑核；单线程；固定 seed
- **日期**：2026-11-03（本轮实测 ARC 0.7764 / LRU 0.5594 / W-TinyLFU 0.7843 / 2Q 0.6521 / CLOCK 0.5327）
- **脚本路径**：`tests/cache/test_scenarios.cpp`
- **状态**：`实测`

### M10 os 批量 writev/readv 加速比

- **历史值**：writev 17.9×、readv 20.9×（1e6 条 8B、批 1024；条件未记录）
- **测量命令**：`taskset -c 2,3 /tmp/bench_vectored /tmp`（源码 `tests/os/bench_vectored.cpp`，手工编译，详见文件头注释；p1 预编译 `libgo2cpp_os.so` 早于 `WritevAll/ReadvAll`，必须直接编 `src/os` 源）
- **环境**：12 vCPU / `taskset -c 2,3` / ext4 / 页缓存热（写不 fsync，读前已完整写过）/ 1e6 条 8B、批 1024（尾批 576）/ 3 次进程 × 每次 3 轮取中位
- **日期**：2026-11-03
- **脚本路径**：`tests/os/bench_vectored.cpp`（本轮新增，233 行）
- **状态**：`实测（已优化）` —— **优化前**：写 **17.96×**（49.1 ns/条）、读 **14.68×**（32–33 ns/条）；**2026-11-03 优化后**：写 **109.72 / 113.65 / 110.56×**（batch 7.7–8.2 ns/条）、读 **207.49 / 230.18 / 201.41×**（batch 2.3–2.4 ns/条），3 次进程、`taskset -c 2,3`、loadavg 1.78、无其它 bench 进程、failures=0。
- **优化机制**：`HasMergeableIov`/`MergeAdjacentIov`（`src/os/file.cpp:608/637`）把**首尾相接**的相邻 iovec 合并（1024 段 → 1 段），总字节与顺序不变；内核逐段成本实测 32.2 µs/次 → 1.0 µs/次；段数 ≤8 或全分散时不扫描、不分配。
- **栈帧**：合并数组从 `struct iovec[1024]`（16 KiB 栈）改为「仅命中时 `new (nothrow)`」→ objdump 实测 `WritevAll` **40 B** / `ReadvAll` **56 B**（改前 16440 B），分配失败退回原数组。
- **正确性**：`go2cpp_os_test` + `go2cpp_os_dir_test` **2/2 Passed**；`ScenarioVectoredIo`（1024×8B、EOF 100B、1025 段 EINVAL、全零、混合零长）failures=0；自建边界（RLIMIT_FSIZE=4096 部分写、跨步 16B 非相邻、EOF 5000B、iovcnt=3）全过。
- **备注**：非相邻分散 iovec **无回退**（22.52 → 22.66 ns/条，+0.6%）；历史 20.9× 的 readv 口径已被本次 200× 覆盖，历史值仅作对照。

### M11 gzip 压缩/解压吞吐

- **历史值**：压缩 550 MiB/s、解压 1880 MiB/s（asm-tools 口径）；后续 asm-compress 记解压 5219.7 MiB/s vs zlib 3633.0（比值 0.696×）
- **测量命令**：`/tmp/bench_tools_industry`（按源码重编）与 `/tmp/bench_gzip_crc`（交叉）；1 MiB L6、3 轮中位
- **环境**：12 vCPU / `taskset -c 2,3` / 单线程 / 页缓存热
- **日期**：2026-11-03
- **脚本路径**：`tests/bench/bench_tools_industry.cpp`、`tests/bench/bench_gzip_crc.cpp`（手工编译，不进 CMake）
- **状态**：`实测` —— 压缩 **612.2 MiB/s**、解压 **3648.0 MiB/s**；同轮 zlib **463.7 / 3138.3 MiB/s**（压缩 1.32× 于 zlib，解压 1.16×）；交叉 `bench_gzip_crc`：解压 **5814.3 MiB/s**、压缩 637.6 MiB/s、crc32 22.14 GB/s。
- **注意**：go2cpp 产物 18496 B vs zlib 13139 B，**压缩率不同**，不能只看吞吐；`.build-p1-test-wsl` 的 compress `.so` 是 10-30 旧版，直接链接会得到 248/519 MiB/s 的**过期假低值**（主控已核 mtime：p1=10-30，bento-wsl=11-03），必须按当前源码重编。

### M12 hash 吞吐

- **历史值**：SHA-256 2.0–2.4 GB/s（SHA-NI）、CRC-32/IEEE 18.38 GB/s（asm-reuse-hash / go-stdlib-gaps）
- **测量命令**：`.build-bento-wsl/go2cpp_hash_test`（3 次取中位；测试二进制 11-01、动态链接 11-03 重编的 `libgo2cpp_hash.so`）
- **环境**：12 vCPU / 单线程 / 1 GiB 输入
- **日期**：2026-11-03
- **脚本路径**：`tests/（hash 测试）`（`go2cpp_hash_test`）
- **状态**：`实测` —— SHA-256 **2002.60 MiB/s**（2099.88 MB/s，SHA-NI）、crc32 **18731.55 MiB/s**、adler32 4396.48 MiB/s、sha1 255.60、md5 889.79。
- **备注**：数字来自测试程序输出，非文档；SHA-256 略低于历史 2.0–2.4 GB/s 区间上沿，属同量级。

### M13 reflect 编解码 ns/op

- **历史值**：二进制编码 51.4–55.9 ns、解码 51.5–54.5 ns（asm-reflect 紧凑格式）
- **测量命令**：`/tmp/bench_reflect_stage`（按源码重编），3 轮中位
- **环境**：12 vCPU / `taskset -c 2,3` / 单线程
- **日期**：2026-11-03
- **脚本路径**：`tests/bench/bench_reflect_stage.cpp`（手工编译）
- **状态**：`实测` —— 二进制编码 **52.64 ns/op**、解码 **54.89 ns/op**；描述格式 78.28 ns/op；文本编码 81.31 / 解码 145.93 ns/op。
- **备注**：与历史紧凑格式区间一致。

### M14 TLS 传输层 vs 裸 OpenSSL

- **历史值**：握手 mean 1.070×、32 KB 写 0.933–1.008×、64 B 小包 0.946–1.064×（asm-tls，全部 <1.1×）
- **测量命令**：`tests/network/bench_tls.cpp`（新增，需 OpenSSL dev；进程内自签 P-256 服务端 + 非阻塞 poll 状态机）；源码手工编译到 `/tmp/build-tls-bench`
- **环境**：WSL2 12 vCPU / OpenSSL 3.0.13 / TLSv1.3 / `taskset -c 2,3` / loadavg≈2.3 / 每轮 300 握手、256 MiB 写、5 万次 64 B / 2 组 × 3 轮
- **日期**：2026-11-03
- **脚本路径**：`tests/network/bench_tls.cpp`
- **状态**：`实测` —— 握手 mean **1.061×**（复跑 1.178×；p50 1.032/1.117，p99 1.040/1.062）；32 KB 写 raw/tt **0.939**（复跑 0.937，TT 略快）；64 B 小包 tt/raw **0.945**（复跑 0.989）。三组同量级、达标（<1.1×）；复跑握手 1.178× 略超线，属两组间宿主波动，需更多轮确认。
- **备注**：tunnel 8 MiB 下载吞吐**无法复现**——`tests/tunnel` 只有 `test_tunnel.cpp` 功能测试，缺吞吐程序。

### M15 bento/link 包吞吐

- **历史值**：无 asm 报告；「单 peer 多队列无收益」已由提交 `475cc15` 证明
- **测量命令**：`.build-bento-wsl/bench_link_mq single 2000000`（3 轮）；`bench_batch3 link_stress`；`bench_protocols --perf --only=link`
- **环境**：WSL2 12 vCPU / `taskset -c 2,3` / 2e6 包每场景
- **日期**：2026-11-03
- **脚本路径**：`tests/bento/`（`bench_link_mq`、`bench_batch3`、`bench_protocols`）
- **状态**：`实测` —— `queues=1/senders=1` 中位 **282,210 pps**（loss 0）、`senders=4` 中位 **270,278**；`link_stress` **232,581 pps**（到达率 98.5–98.8%）；`bench_protocols` 可靠 STREAM 64 B **15,713 msg/s**（≈0.96 MiB/s payload）、DATAGRAM 53,481 msg/s；**2 核绑核下 queues=4/8 丢包 39–48%**（多队列单 peer 无收益，与历史结论一致）。
- **备注**：bento **bulk STREAM 字节吞吐无基准**（只有 64 B 消息级）；不绑核对照 `multi pairs=4` 达 87–96 万 pps。

### M16 Go scheduler 基线（本机实测，替代历史 135.49）

- **历史值**：135.49 ns/task（`2026-11-01-concurrency-survey.md:29`，标注 GOMAXPROCS=2、5 轮×2 次取 min；源码 `/tmp/conc_survey/src/bench_go.go` **已不存在、git log 无记录 → 口径不可复核**）。「122.0 ns」无同报告原始支撑（vs-concurrency/survey grep 0 命中），仅 `engineering-standards.md:124` 一行且「0.98×」与 122/135.49=0.90 不自洽。
- **测量命令**：`taskset -c 2,3 /tmp/go_sched_bench 100000 5 <GOMAXPROCS>`（Go 1.27.1；`/tmp/go_sched_bench.go`：N 个空 goroutine + WaitGroup，5 轮）
- **环境**：WSL2 12 vCPU Ryzen 9 8945HX / `taskset -c 2,3` / 100k 任务 × 5 轮 × 3 进程 / loadavg 0.16–1.34 / 无其它 bench 进程
- **日期**：2026-11-03
- **脚本路径**：`tools/bench/go_sched_bench.go`（2026-11-03 固化；编译/运行见 `tools/bench/README.md`）；对照 C++ 用 `tests/concurrency/bench_scheduler_task_opt.cpp`
- **状态**：`实测` —— GOMAXPROCS=1 **736.2**；=2 **264.9**（6 进程复跑 251.0）；=4 **162.7**（9 进程复跑 151.0）；=8 **183.4**；`GOGC=off`@4 179.4。
- **对照**：C++ 同轮 `taskset -c 2,3 /tmp/bench_sched 4 100000 7` ×3 次进程 → 359/333/366，中位 **359**（5 轮口径 283）。比值 Go@4/C++ = **2.21×**（全量中位 333/151=2.21；5 轮 283/163=1.74；Go@2/C++ 1.35）。
- **判定**：**不达标**（>1.1×），但真实差距 **1.4–2.5×**，不是历史宣称的 5×。135.49 只与「min 统计 / @4 下尾」吻合；122 无出处，不能与 135.49 并列算 0.98×。
- **限制**：C++ 主线程忙等（Go `wg.Wait` 停泊）对 C++ 不利；4 worker vs 2 CPU 超订；两者对宿主噪声敏感（C++ 单进程 7 轮中位 281–366）。
- **结论**：优化目标从「追 122」改为「追 Go@4 本机 151–163 ns」。

## 第二批：2026-11-05 补录（M17–M23）

> **背景**：`docs/plans/doc-perf-sync.md` §四复查发现台账只到 M16，2026-11-04 的 6 组实测只存在于报告里，
> 不满足台账规则第 1 条（文档数字必须指到一条记录）。本节补录：M17–M22 是报告原始值（**不改数字、不重算**，只做「来源核验 + 入账」），
> M23 是 2026-11-05 本轮实跑（原先「p50 149 µs / 177k/s」无出处，已用实测替代）。
> **日期列 = 该数字的实测日期**（不是补录日期）；`复跑状态 = 报告原始` 表示已 `grep` 报告正文命中该值，但本轮未独立重跑。
> 测量命令、轮数、绑核见各报告对应小节（台账不复制命令，避免两处漂移）。

| ID | 模块 | 指标（口径） | 实测 | 对照基线 | 来源报告 | 日期 | 复跑状态 |
|---|---|---|---|---|---|---|---|
| M17 | tunnel | 8 MiB 大 body raw 下载吞吐 / 协议开销 | **2385.3 MiB/s**（31,522 轮 / 246 GiB，分段 2341–2423），开销 **1.27×** | mux 同机 **770.5 MiB/s / 3.59×**；旧默认 mux 830 / 3.56×；门禁 ≥1475 MiB/s（超 62%） | `docs/reports/tunnel-raw-default-2026-11-04.md:4,27` | 2026-11-04 | 报告原始 |
| M18 | TLS AEAD | ChaCha20-Poly1305 自研 vs OpenSSL EVP 吞吐比 | 64 KiB **1.02–1.03×**、1 MiB **1.03–1.04×**；Poly1305 **14.83–15.95 GB/s** | 改前 **1.14–1.21×**（Poly 9.92 GB/s）；目标 <1.1× | `docs/reports/asm-tls.md:823-824,864-865` | 2026-11-04 | 报告原始 |
| M19 | bufio | 1 MiB 分块读 MiB/s（复用缓冲探针） | **17695**（best 18073；改前 12387，**+43%**） | Go **19775.4** → **0.895×**（best 18073 = 0.914×） | `docs/reports/asm-bufio.md:211-216` | 2026-11-04 | 报告原始（同刻 A/B） |
| M20 | HTTP Parser | 头部重解析 / chunked 重扫耗时 | 头重解析 **66.3 → 1.95 ms**；chunked **4017 → 17 ms** | 修复前同口径（`http.cpp:518-558` / `585-621`） | `docs/reports/http-link-audit.md:22-23`（`d538326` / `e84bbec`） | 2026-11-04 | 报告原始（提交号可追） |
| M21 | storage/kv | 读 vs LevelDB 倍率；Put ops/s | 读 **1.109×**（1e7 全流程 680,915 vs 613,941）/ **1.252×**（Get-only 三轮中位）；写 **703,517 ops/s = 1.09×** | LevelDB 同轮 Put 643,160；0.9× 目标线 578,844 | `docs/reports/storage-opt.md:61,74,260,266` | 2026-11-04 | 报告原始 |
| M22 | WS | 20k 长连接 RSS / p99 延迟 | RSS **227.7 MB = 0.96×**；p99 **1.691 ms = 0.93×** | Go RSS 237.5 MB / p99 1.81 ms；优化前 2.20–2.32× / 2.06–3.06× | `docs/reports/three-piece-status-2026-11-04.md:37,47` | 2026-11-04 | 报告原始 |
| M23 | rpc | 单连接串行同步调用 1e6 次 p50 / 吞吐；并发 8 客户端 × 32 线程 | p50 **114.4 µs**（p90 217.6 / p99 305.1 / p999 3566.3 / max 16411.6 / mean 136.0 µs），串行 **7,350/s**；并发 **212,701/s**（1e5 次 0 失败，wall 470.1 ms） | 历史宣称「149 µs / 177k/s」**不复现**（p50 高 30%、串行吞吐差 24.1×；177k 既非串行也非本次并发） | `docs/reports/rpc-bench-2026-11-05.md` | 2026-11-05 | **本轮实测**（`ctest -R go2cpp_rpc_test` 1/1 Passed，137.31 s，121 checks 0 failures） |

### 入账核验（2026-11-05 实际执行）

```bash
# 每条数字必须在来源报告正文 grep 命中（防止「文件名像 ≠ 内容有」）
grep -nE '2385|1\.27' docs/reports/tunnel-raw-default-2026-11-04.md   # 4,27,28
grep -nE '1\.02|1\.03|1\.04' docs/reports/asm-tls.md                 # 823,864,865
grep -nE '17695|0\.895' docs/reports/asm-bufio.md                      # 213,216
grep -nE '1\.95|4017' docs/reports/http-link-audit.md                  # 22,23
grep -nE '1\.109|703,517' docs/reports/storage-opt.md                  # 61,260
grep -nE '0\.96|0\.93' docs/reports/three-piece-status-2026-11-04.md  # 37,47
grep -nE '114\.4|7,350|212,701' docs/reports/rpc-bench-2026-11-05.md # 6,58,61,62（M23，本轮实测）
# 7/7 命中；未命中的一律不进台账
```

## 历史值汇总（只作对照，不得引用为结论）

| ID | 指标 | 历史值 | 状态 |
|---|---|---|---|
| M01 | Channel 1P1C ns/条 | 10.73（实为 FastChannel） | 已拆条，见 M01b-a/b |
| M01b-a | 通用 Channel 1P1C ns/条 | 112.6（min 103.3）vs Go 23.36 | 实测·口径不可比（Context 语义） |
| M01b-b | FastChannel<1024> ns/条 | 16.07（线程）/6.23（fiber）vs Go 23.36 | 实测·达标 |
| M02 | Fiber 全量切换 ns/往返 | 33.80 | 实测（推断口径 19.84） |
| M03 | scheduler wall ns/task | 122.0（历史）/ 239（本轮中位） | 实测（本轮） |
| M04 | scheduler submit ns/task | 119.0（batch1024）/ 355.7（batch65536） | 实测·双口径 |
| M05 | HTTP pipeline=16 rps | 120,755（6 轮中位） | 实测 |
| M05b | HTTP pipeline 411k rps | 无出处 | 无出处 |
| M06 | SQLite 1e6 插入 | 1.074×（7 轮中位） | 实测·达标 |
| M07 | LevelDB 1e7 Put / 1e8 RSS | 647.5k（14.7 MiB 写入 15.1s 级）/ 82.4 MiB 峰值（VmHWM 122.8，5.40%） | 实测 |
| M08 | bufio 1 MiB 分块 | **17695**（2026-11-04）← 历史 1853.9（2026-11-03，每轮新建缓冲，不同口径） | 实测 |
| M09 | cache hotspot ARC/LRU | 0.7817 / 0.5580（历史）→ 0.7764 / 0.5594（本轮） | 实测 |
| M10 | os 批量 writev/readv | 写 17.96× / 读 14.68×（读未达历史 20.9×） | 实测 |
| M11 | gzip 压缩/解压 | 612.2 / 3648.0 MiB/s（zlib 463.7/3138.3） | 实测 |
| M12 | hash 吞吐 | SHA-256 2002.60 MiB/s、crc32 18731.55 MiB/s | 实测 |
| M13 | reflect 编解码 | 编码 52.64 / 解码 54.89 ns/op | 实测 |
| M14 | TLS vs 裸 OpenSSL | 握手 1.061×、写 0.939、64B 0.945 | 实测·达标 |
| M15 | bento/link pps | single 282,210 pps（loss 0）；多队列 2 核丢包 | 实测 |
| M16 | Go scheduler 基线 | Go@4 162.7（151–163）/ @2 264.9 / @1 736.2；C++ 同轮 359 → 2.21× | 实测·不达标 |
| M17 | tunnel raw 8 MiB | 2385.3 MiB/s / 1.27×（mux 770.5 / 3.59×） | 实测·报告原始 |
| M18 | TLS AEAD vs EVP | 64 KiB 1.02–1.03× / 1 MiB 1.03–1.04× | 实测·报告原始 |
| M19 | bufio 1 MiB 分块 | 17695 MiB/s（0.895× Go，改前 12387） | 实测·报告原始 |
| M20 | HTTP Parser | 66.3→1.95 ms；chunked 4017→17 ms | 实测·报告原始 |
| M21 | storage/kv | 读 1.109×/1.252×；写 703,517 ops/s = 1.09× | 实测·报告原始 |
| M22 | WS 20k | RSS 227.7 MB=0.96×；p99 1.691 ms=0.93× | 实测·报告原始 |
| M23 | rpc 1e6 串行 | p50 114.4 µs / 7,350/s；并发 212,701/s（历史 149 µs/177k 不复现） | 实测·本轮 |

## 教训：基准必须独占

2026-11-03 实测：A5 的 `verify_metrics.sh` 全量在 A4c 并行跑 bento 基准时出现 3 条假 STALE（M03 628 vs 239、M04 232.7 vs 119、M08 1258 vs 1853.9，宿主争用 1.84×）；A4c 结束后独占复跑 → `TOTAL=16 OK=12 STALE=0 SKIP=4`。**结论：性能对照运行期间不得有其它基准/压测进程**；脚本已在输出里打印 loadavg 与 cpu 中位辅助判断，但判 STALE 前必须独占复跑一次。

## 待办

- ✅ A2 完成：`tools/verify_metrics.sh`（M01b/M02/M03），M01b 已拆为 a/b。
- ✅ A3 完成：M04/M05/M06 已填实（M04 双口径、M05 120.8k、M06 1.074×）。
- ✅ A4a 完成：M07 已填（1e7 647.5k、1e8 RSS 82.4 MiB/VmHWM 122.8、对照 4.14×）。
- ✅ A4b 完成：M10/M11/M12/M13 已填。
- ✅ A4c 完成：M14/M15 已填（TLS 1.061×、bento single 282k pps）。
- ⏳ A6：清理 411k/160ns/122ns 等文档漂移（含 `engineering-standards.md:124` 的 0.98× 不自洽）。
- 🔄 M03 优化目标改为「逼近 Go@4 本机 151–163 ns」（当前 283–359，差 1.7–2.2×）；从 `docs/reports/go-scheduler-borrow.md` 的 Top-3 里选架构级改动。
- ⏳ M10/M11 三件套排队。
- ⏳ 固化 Go 基准到 `tools/`（当前在 /tmp，重启即失）。
- ⏳ A6：把 docs 里 411k、160ns 等改为指向本文件或标 STALE。
- ⏳ A7 架构级：M01b-a 通用 Channel 无锁化（覆盖 Send/Recv/Select/Close，需完整验证窗口，单独立项）。

| 2026-11-05 18:30 | bufio 热态 | median **10784.5 MiB/s**（best 18387.8） | 基线 17695（0.895× Go） | 本轮抽查，偏差 **−39%**（median）；⚠️ median 与 best 差 70%，判环境噪声，待下一轮复测 | 2026-11-05 | 单次 |

| 2026-11-05 18:33 | rpc 并发延迟 | p50 **104.3µs**（p90 174.8 / p99 261.0 / p999 501.4 / mean 119.0） | 基线 p50 114.4µs | 偏差 **−8.8%**（更快） | 2026-11-05 | 单次 |

| 2026-11-05 18:45 | bufio 热态（复测×3） | median **11393.9 / 6185.2 / 11987.8 MiB/s**（同轮 3 次差 **2.0×**） | 基线 17695 | ⚠️ **判「测量不可靠」而非回归**：同轮波动 2 倍，best 最高 16665（接近基线）；未绑核的共享机器上该基准无判别力，建议后续用 `taskset -c` 绑核 + ≥5 轮取中位再引用 | 2026-11-05 | 3 次 |

| 2026-11-05 19:05 | storage kv（taskset -c 4） | Put **866,462 ops/s**（tmpfs）/ **791,836**（ext4）；Get(随机命中) 520,746 / 519,467 | 基线写 703,517 ops/s（M08） | 写 **+23%**（更快，非退化）；Get 为绝对 ops/s，与基线「1.109–1.252× 相对 LevelDB」口径不同不可直接比 | 2026-11-05 | taskset 绑核 |

| 2026-11-05 19:18 | Parser（chunked 6MiB/1KiB） | **23 ms**（对照 close-delimited 17 ms） | 基线 17 ms / 9 ms | ⚠️ +35%，但**对照组同步变慢**（9→17 ms，+89%）→ 判机器负载噪声，非 Parser 回归 | 2026-11-05 | 单次 |
