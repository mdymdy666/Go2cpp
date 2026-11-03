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
- **脚本路径**：`tests/concurrency/bench_fast_channel.cpp`（已存在）；通用 Channel 需重建 `bench_channel_opt`
- **状态**：`待复跑`
- **备注**：2026-11-03 本轮旁证（非同一口径）：`fast-cap1024` 线程 16.07 ns（min 13.15）、Fiber 6.23 ns（min 5.82），5 轮、taskset 2,3。**不能**用它替代 M01 的 10.73。

### M01b 通用 `Channel<int>` 1P1C ns/条

- **历史值**：10.73（与 M01 同源，口径为通用 Channel）
- **测量命令**：需重新编译 `tests/concurrency/bench_channel_opt.cpp`（旧二进制报 `undefined symbol: Context::IsDone`）
- **环境**：待定
- **日期**：未记录
- **脚本路径**：`tests/concurrency/bench_channel_opt.cpp`（2026-11-03 `ls` 已确认存在）
- **状态**：`STALE`（2026-11-03 `tools/verify_metrics.sh` 实测 **112.6 ns/条**：cap=1024 SPSC、1e6×3 reps、`taskset -c 2,3`、12 vCPU、复用 `.build-bento-wsl` 库；与历史 10.73 差 9 倍。两者都被标为「通用 Channel 1P1C」，故判 STALE；A3 需查历史 10.73 的原始口径——可能是不同实现或早已退化）

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

- **历史值**：326.7 ns
- **测量命令**：待补（submit 侧单段计时，非 wall）
- **环境**：未记录
- **日期**：未记录
- **脚本路径**：`待建`（需单段 rdtsc 计时脚本）
- **状态**：`待复跑`

### M05 HTTP/1.1 pipeline=16 单连接 rps

- **历史值**：72.9k req/s（串行 8.9k 的 8.2×）；另 8 连接 239.5k
- **测量命令**：`net_perf_server` + `tests/network/http_loadgen.go`（需 Go 环境）；当前无现成脚本
- **环境**：未记录（WSL2 回环口径）
- **日期**：未记录
- **脚本路径**：`tests/network/net_perf_server.cpp` + `http_loadgen.go`（`待接线`）
- **状态**：`待复跑`
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
- **测量命令**：`.build-bento-wsl/go2cpp_storage_bench`（本轮用 `go2cpp_storage_db_test` 得到 insert 2.72M rows/s，非同一口径）
- **环境**：未记录
- **日期**：未记录
- **脚本路径**：`tests/storage/bench_db.cpp`
- **状态**：`待复跑`

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
| M01 | Channel 1P1C ns/条 | 10.73 | 待复跑 |
| M02 | Fiber 全量切换 ns/往返 | 33.80 | 待复跑 |
| M03 | scheduler wall ns/task | 122.0（历史）/ 239（本轮中位） | 实测（本轮） |
| M04 | scheduler submit ns/task | 326.7 | 待复跑 |
| M05 | HTTP pipeline=16 rps | 72.9k | 待复跑 |
| M05b | HTTP pipeline 411k rps | 无出处 | 无出处 |
| M06 | SQLite 1e6 插入 | 1.078× | 待复跑 |
| M07 | LevelDB 1e7 Put / 1e8 RSS | 583k / 87 MiB | 待复跑 |
| M08 | bufio 1 MiB 分块 | 1853.9 MiB/s | 实测 |
| M09 | cache hotspot ARC/LRU | 0.7817 / 0.5580（历史）→ 0.7764 / 0.5594（本轮） | 实测 |
| M10 | os 批量 writev/readv | 17.9× / 20.9× | 待复跑 |

## 待办

- A2：`tools/verify_metrics.sh` 先接 M01/M02/M03 三条（其余输出「未接线」而不是假跑）。
- A3：填实 M01、M02、M03、M05、M06。
- A4：填实 M07、M10 及其余模块（bento/link、TLS、gzip、reflect/hash）。
- A6：把 docs 里 411k、160ns 等改为指向本文件或标 STALE。
