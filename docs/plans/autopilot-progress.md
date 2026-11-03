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
| borrow | Go 源码借鉴（runqput/runqsteal/findrunnable） | 🔄 子 agent `b5ccd296` 在跑（8 分钟时间盒） |
| B1 | VPS `tunnel-server.service` | 🔄 子 agent `8af8dbe4` 在跑 |
| A6 | 清理 122/135/411k/0.98× 文档漂移 | 🔄 子 agent `f3638275` 在跑 |
| M10/M11 | os readv / gzip 三件套 | ⏳ 排队（等基准独占窗口） |
| B2–B4 | 本机 start-all / 探针 / 断线恢复 | ⏳ |
| C1–C10 | 抖音 P0 | ⏳ |

## 下一个任务

等三路交付（borrow / B1 / A6）后并行审查；随后**选 borrow 的 Top-1 落地架构改动**（目标 Go@4 151–163，当前 283–359，需 1.7–2.2×）：
- 先 baseline（同命令 3 次中位，独占）→ 一条架构改动 → 汇编对比（若涉及）→ 全量正确性 + 1e6 压力 → 前/后数据；失败即回滚。

## 本轮记录

- 13:20 启动；A1 be73a4e；A2 92dcb25；优化规则 456ec05；A7 3a7aa4f；A3 55a3f0e；A4b e7fcbbb；A4a 761ea6c；A5+A4c b0c9013；M03 第一轮 46b9d65；Go 基线 1b4ae22。
- 14:53 M03 三件套第一轮：baseline 320、P19 −5%、汇编 enqueue 597→580/栈帧 104→88B/divq 1→0、p22 −15~35% 但 smoke 失败 → 回滚（零改动）。
- 15:00 Go 重测：Go@4 162.7（@1 736/@2 264.9/@8 183.4/GOGC off 179.4）、C++ 359 → 2.21×（最好组合 1.47×）；历史 122/135 判口径不可考。
- 15:03 派 borrow（`7bfcd2c6`，11 分钟无产出被 interrupt）→ 改派 `b5ccd296`（只读 3 函数、8 分钟时间盒）。
- 15:14 按用户「并发开启其他任务」并发 4 个：`8af8dbe4`（VPS systemd）、`f3638275`（A6 文档漂移）、`24415abc`（固化 Go 基准）、`b5ccd296`（borrow）。
- 15:18 **固化完成**（`24415abc`）：`tools/bench/go_sched_bench.go`（75 行，正文与 /tmp 版 `BODY_IDENTICAL`）+ `tools/bench/README.md`（37 行）；编译 BUILD_OK；3 轮 290/176/133（median 176.19，min 133.43），复跑 256/131.8/184.2、5 轮中位 184.55 —— 与 M16 162.7 同量级但抖动大（核 2 有 cpptools/mysqld 共享），程序逐字节一致；主控抽查：75/37 行、BODY_IDENTICAL、`git status` 仅 `?? tools/bench/`。ledger M16 脚本路径已更新。
