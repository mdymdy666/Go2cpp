# Autopilot 进度（Go2Cpp）

> 节奏：每 15 分钟一轮（2026-11-03 14:46 用户从 30 分钟改为 15 分钟；定时任务 `Go2Cpp 自动驾驶推进`，id `schedule-0ae729ea`）。每轮只做「下一个任务」一项；用户 15:20 起允许并发多任务（本机基准互斥、不占 CPU 的任务可并行）。
> 规则：做完必须验证；失败如实记；WSL 真树 commit，push 走 Windows 侧 Python；更新本文件。
> 真树：`/UserData/CodexWorkSpace/Go2cpp`（分支 `feat/isolated`）。

## 总目标（三阶段，按顺序）

| 阶段 | 目标 | 完成判据 |
|---|---|---|
| A 数字台账 | 所有性能数字带「命令/环境/日期/脚本」，可一条命令复跑 | `tools/verify_metrics.sh` 跑完无未解释 STALE；docs 无孤立数字 |
| B 公网变服务 | VPS systemd + 本机 start-all + 探针 + 断线恢复 | VPS 重启后公网 200；kill client 自动恢复；探针 30 分钟无失败 |
| C 抖音 P0 | 6 服务 + 网关串成应用 | 5 个真实场景 curl 可复现；压测报告进 `docs/reports/` |

## 性能不达标处理规则（用户补充）

同口径超判据（vs Go/工业库 >1.1×）时：① 先口径对齐；② 架构 → 汇编 → 正确性验证；③ 前后数据回写 ledger；④ 子 agent 不 commit；⑤ 高风险无法验证则回滚并如实记。

## 基准必须独占（实测教训）

并行跑基准会造假 STALE（A5 全量在 A4c 并行压测下出 3 条假 STALE）。对照运行期间不得有其它基准进程。

## 进度表

| 任务 | 内容 | 状态 |
|---|---|---|
| A1–A5 | ledger/三基线/M04-M15/全量校验 | ✅ 至 b0c9013 |
| M16 | Go 基线本机实测 + 固化 | ✅ 1b4ae22 + 本轮固化（tools/bench/） |
| M03 | scheduler 三件套 | 🔄 判定不达标 2.21×；目标追 Go@4 151–163 |
| borrow | Go 源码借鉴（3 函数，73 行：runnext 位移 / steal runnext / netpoll 前移） | ✅ 本轮（b5ccd296） |
| B1 | VPS `tunnel-server.service` | 🔄 子 agent `8af8dbe4` 在跑 |
| A6 | 清理 122/135/411k/0.98× 文档漂移 | 🔄 子 agent `f3638275` 在跑 |
| M10/M11 | os readv / gzip 三件套 | ⏳ 排队（等基准独占窗口） |
| B2–B4 | 本机 start-all / 探针 / 断线恢复 | ⏳ |
| C1–C10 | 抖音 P0 | ⏳ |

## 下一个任务

**M03c（进行中）**：p22 分块复核 + borrow 建议 1。
1. 二分定位 p22 的哪一块触发 smoke（P19/P20/P21/P22 分别单独应用各跑 20 次）；判定「断言竞态」还是「语义破坏」（park 是否最终到 kWaiting、wake 是否正常）；
2. 若为语义破坏 → 永久放弃该块；若为竞态 → 修断言（`wait_until(kWaiting, 200ms)`）后全量 ctest + 1e6 压力；
3. 落地 borrow 建议 1（`publish_runnext` 的 displaced 由本 P overflow 承接，避免 `enqueue` 全局锁）：先测 displaced 频率，再改，跑 bench + 正确性；
4. 给「baseline → 每块 → 组合」的前/后数据；目标追 Go@4 151–163（当前 283–359）。
注：borrow 的 Top-1 是 netpoll 前移，但它对 churn 基准收益不确定、且与 M03c 改同一文件，排在其后单独做。

## 本轮记录

- 13:20 启动；A1 be73a4e；A2 92dcb25；优化规则 456ec05；A7 3a7aa4f；A3 55a3f0e；A4b e7fcbbb；A4a 761ea6c；A5+A4c b0c9013；M03 第一轮 46b9d65；Go 基线 1b4ae22。
- 14:53 M03 三件套第一轮：baseline 320、P19 −5%、汇编 enqueue 597→580/栈帧 104→88B/divq 1→0、p22 −15~35% 但 smoke 失败 → 回滚（零改动）。
- 15:00 Go 重测：Go@4 162.7（@1 736/@2 264.9/@8 183.4/GOGC off 179.4）、C++ 359 → 2.21×（最好组合 1.47×）；历史 122/135 判口径不可考。
- 15:03 派 borrow（`7bfcd2c6`，11 分钟无产出被 interrupt）→ 改派 `b5ccd296`（只读 3 函数、8 分钟时间盒）。
- 15:14 按用户「并发开启其他任务」并发 4 个：`8af8dbe4`（VPS systemd）、`f3638275`（A6 文档漂移）、`24415abc`（固化 Go 基准）、`b5ccd296`（borrow）。
- 15:18 **固化完成**（`24415abc`）：`tools/bench/go_sched_bench.go`（75 行，正文与 /tmp 版 `BODY_IDENTICAL`）+ `tools/bench/README.md`（37 行）；编译 BUILD_OK；3 轮 290/176/133（median 176.19，min 133.43），复跑 256/131.8/184.2、5 轮中位 184.55 —— 与 M16 162.7 同量级但抖动大（核 2 有 cpptools/mysqld 共享），程序逐字节一致；主控抽查：75/37 行、BODY_IDENTICAL、`git status` 仅 `?? tools/bench/`。ledger M16 脚本路径已更新。已合并 `a2ce97b`。
- 15:22 **borrow 完成**（`b5ccd296`，73 行，8 分钟时间盒内）：3 条建议——① `publish_runnext` 的 displaced 满环时走 `enqueue` 全局锁（Go 无条件踢回普通 runq 尾，`proc.go:7547-7549` ↔ `scheduler.cpp:4015/4031-4033`）→ 改由本 P overflow/incoming 承接；② 本项目刻意不偷 runnext（`:5866` 注释 + 历史「跨 M 恢复 15%→85%」反证），Go 有 `stealRunNextG` 3µs 退避（`proc.go:7720-7724`）→ 加开关、默认关；③ netpoll(0) 前移（Go `proc.go:3510-3513` 在偷之前 poll，本项目 `:6278` 无 G 才进 IO）→ Top-1；主控抽查：Go 7547/7718/7720/3510-3512 与 scheduler.cpp 4015/4031-4033/5859/5866 全部命中；主控决定先做「p22 分块复核 + 建议①」而非 netpoll（对 churn 收益更直接）。
