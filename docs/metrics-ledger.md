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
- **状态**：`实测（不达标 4.8×）`（spsc cap1024 **112.6 ns/条**（median ~114），min **103.3**；对照 Go `chan uint64` cap1024 = **23.36**（`docs/reports/vs-concurrency.md:86/90`，同为 `taskset -c 2,3` SMT 兄弟），比值 4.8×）
- **诊断**：`tryspsc`（纯轮询、永不 park）101.8–118.3 ≈ `spsc`；`taskset -c 2,4`（跨物理核）93.3–98.9 ≈ `2,3` → 瓶颈是**锁的跨核 cacheline 传递**（每 op ≥3 条 RFO：`m_gate`、buffer 索引、数据槽），不是 park/futex，也不是 SMT 特有。单锁架构的物理下限约 60–90 ns/op，达标线 25.7 ns **在单锁下不可达**，必须无锁化。
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
- **状态**：`实测`（wall 287/239/267/236/225 ns，中位 239；cpu 619/519/582/514/478）
- **备注**：与历史 122.0 不一致，原因是核数/绑核/轮次/宿主负载不同；对外只能报「口径 + 条件 + 中位」。2026-11-03 13:33 复跑中位 **299 ns**（同 5 轮 cpu 中位 627），同日另一轮 5 轮波动 245–787 —— 该基准对宿主负载敏感，单次数字不可单独引用，必须带 cpu 中位与轮次分布。

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

- **历史值**：1e7 Put 58.3 万 ops/s（3.7× 自研）；1e8 后 RSS 87 MiB（数据量 4.9%）
- **测量命令**：`.build-bento-wsl/go2cpp_leveldb_kv_test`（本轮 1e5 写入 669k ops/s、VmHWM 9.0 MiB，规模不同）
- **环境**：未记录
- **日期**：未记录
- **脚本路径**：`tests/storage/test_leveldb_kv.cpp`
- **状态**：`待复跑`

### M08 bufio 1 MiB 分块读 MiB/s

- **历史值**：1853.9 MiB/s（本轮复跑得到同值，`Read` 2048 次、底层 1415 次、512B 请求）
- **测量命令**：`.build-bento-wsl/go2cpp_bufio_test`
- **环境**：12 vCPU；未绑核；单线程；页缓存热
- **日期**：2026-11-03
- **脚本路径**：`tests/bufio/test_bufio.cpp`（场景「分块传输 1MiB」）
- **状态**：`实测`

### M09 Cache hotspot 命中率（ARC/LRU）

- **历史值**：ARC 0.7817 / LRU 0.5580（capacity=2000、300k 操作）
- **测量命令**：`.build-bento-wsl/go2cpp_cache_test`（hotspot 段）
- **环境**：12 vCPU；未绑核；单线程；固定 seed
- **日期**：2026-11-03（本轮实测 ARC 0.7764 / LRU 0.5594 / W-TinyLFU 0.7843 / 2Q 0.6521 / CLOCK 0.5327）
- **脚本路径**：`tests/cache/test_scenarios.cpp`
- **状态**：`实测`

### M10 os 批量 writev/readv 加速比

- **历史值**：writev 17.9×、readv 20.9×（1e6 条 8B、批 1024）
- **测量命令**：`.build-bento-wsl/go2cpp_os_test`（本轮只验证功能 `failures=0`，未计时）
- **环境**：未记录
- **日期**：未记录
- **脚本路径**：`tests/os/test_os_file.cpp`（`ScenarioVectoredIo`）
- **状态**：`待复跑`

## 历史值汇总（只作对照，不得引用为结论）

| ID | 指标 | 历史值 | 状态 |
|---|---|---|---|
| M01 | Channel 1P1C ns/条 | 10.73（实为 FastChannel） | 已拆条，见 M01b-a/b |
| M01b-a | 通用 Channel 1P1C ns/条 | 112.6（min 103.3）vs Go 23.36 | 实测·不达标 4.8× |
| M01b-b | FastChannel<1024> ns/条 | 16.07（线程）/6.23（fiber）vs Go 23.36 | 实测·达标 |
| M02 | Fiber 全量切换 ns/往返 | 33.80 | 实测（推断口径 19.84） |
| M03 | scheduler wall ns/task | 122.0（历史）/ 239（本轮中位） | 实测（本轮） |
| M04 | scheduler submit ns/task | 119.0（batch1024）/ 355.7（batch65536） | 实测·双口径 |
| M05 | HTTP pipeline=16 rps | 120,755（6 轮中位） | 实测 |
| M05b | HTTP pipeline 411k rps | 无出处 | 无出处 |
| M06 | SQLite 1e6 插入 | 1.074×（7 轮中位） | 实测·达标 |
| M07 | LevelDB 1e7 Put / 1e8 RSS | 583k / 87 MiB | 待复跑 |
| M08 | bufio 1 MiB 分块 | 1853.9 MiB/s | 实测 |
| M09 | cache hotspot ARC/LRU | 0.7817 / 0.5580（历史）→ 0.7764 / 0.5594（本轮） | 实测 |
| M10 | os 批量 writev/readv | 17.9× / 20.9× | 待复跑 |

## 待办

- ✅ A2 完成：`tools/verify_metrics.sh`（M01b/M02/M03），M01b 已拆为 a/b。
- ✅ A3 完成：M04/M05/M06 已填实（M04 双口径、M05 120.8k、M06 1.074×）。
- ⏳ A4：M07、M10 及其余模块（bento/link、TLS、gzip、reflect/hash）。
- ⏳ A6：把 docs 里 411k、160ns 等改为指向本文件或标 STALE。
- ⏳ A7 架构级：M01b-a 通用 Channel 无锁化（覆盖 Send/Recv/Select/Close，需完整验证窗口，单独立项）。
