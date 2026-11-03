# Autopilot 进度（Go2Cpp）

> 节奏：每 15 分钟一轮（2026-11-03 14:46 用户从 30 分钟改为 15 分钟；定时任务 `Go2Cpp 自动驾驶推进`，id `schedule-0ae729ea`）。每轮只做「下一个任务」一项（可派 1–2 个子 agent）。
> 规则：做完必须验证；失败如实记；WSL 真树 commit，push 走 Windows 侧 Python；更新本文件。
> 真树：`/UserData/CodexWorkSpace/Go2cpp`（分支 `feat/isolated`）。

## 总目标（三阶段，按顺序）

| 阶段 | 目标 | 完成判据 |
|---|---|---|
| A 数字台账 | 所有性能数字带「命令/环境/日期/脚本」，可一条命令复跑 | `tools/verify_metrics.sh` 跑完无未解释 STALE；docs 无孤立数字 |
| B 公网变服务 | VPS systemd + 本机 start-all + 探针 + 断线恢复 | VPS 重启后公网 200；kill client 自动恢复；探针 30 分钟无失败 |
| C 抖音 P0 | 6 服务 + 网关串成应用 | 5 个真实场景 curl 可复现；压测报告进 `docs/reports/` |

## 性能不达标处理规则（2026-11-03 用户补充）

同口径超判据（vs Go/工业库 >1.1×）时：① 先口径对齐；② 架构 → 汇编 → 正确性验证（功能+随机+压测+可选 TSAN）；③ 前后数据回写 ledger；④ 子 agent 不 commit；⑤ 高风险无法验证则回滚并如实记。

## 基准必须独占（实测教训）

并行跑基准会造假 STALE（A5 全量在 A4c 并行压测下出 3 条假 STALE，独占复跑全消）。对照运行期间不得有其它基准进程。

## 进度表

| 任务 | 内容 | 状态 |
|---|---|---|
| A1–A5 | ledger 建档、三基线脚本、M04/M05/M06、M07、M10–M13、M14/M15、全量校验 | ✅ 至 b0c9013 |
| M03 | scheduler 三件套 | 🔄 第一轮回滚；**Go 重测后判定：不达标 2.21×，目标改为追 Go@4 151–163** |
| Go 基线 | M16 本机实测（替代历史 135.49） | ✅ 本轮（Go@4 162.7、@2 264.9、@1 736.2；C++ 359 → 2.21×；历史 122/135 口径不可考） |
| Go 借鉴 | `docs/reports/go-scheduler-borrow.md`（≥8 条可借鉴点 + Top-3） | 🔄 子 agent `7bfcd2c6` 在跑 |
| M03b | p22 复核 | ⏸ 已 interrupt（用户判断「几乎没有优化空间」后让位给 Go 重测；p22 补丁仍在 `/tmp/scheduler.cpp.p22`） |
| M10/M11 | os readv / gzip 三件套 | ⏳ 排队 |
| A6 | 清理 411k/160ns/122ns 漂移 | ⏳ |
| B1–B4 | 公网变服务 | ⏳ |
| C1–C10 | 抖音 P0 | ⏳ |

## 下一个任务

**择一执行（等 `7bfcd2c6` 的 Top-3 回来后定）**：从 `docs/reports/go-scheduler-borrow.md` 的 Top-3 里选一条**架构级**改动落地（对标 Go@4 151–163 ns，当前 283–359，需 1.7–2.2× 改善）：
- 候选方向（按 Go 源码常见落差）：runnext/runq 的批量转移（runqputslow）、work stealing 一次偷一半、findrunnable 顺序、gfree 批量回收、park/wake 的 g0 栈切换成本。
- 要求：先 baseline（同命令 3 次中位）→ 一条架构改动 → 汇编对比（若涉及）→ 全量正确性 + 1e6 压力 → 前/后数据；失败即回滚。

另：把 `/tmp/go_sched_bench.go` 固化到 `tools/bench/go_sched_bench.go`（否则重启即失，M16 不可复现）。

## 本轮记录

- 13:20 启动；A1 be73a4e；A2 92dcb25；优化规则 456ec05；A7 3a7aa4f；A3 55a3f0e；A4b e7fcbbb；A4a 761ea6c；A5+A4c b0c9013；M03 第一轮 46b9d65。
- 14:46 节奏改 15 分钟（900s）。
- 14:53 M03 三件套第一轮：baseline 320、P19 −5%、汇编 enqueue 597→580/栈帧 104→88B/divq 1→0、p22 −15~35% 但 smoke 10/10 失败 → 按铁律回滚（scheduler.cpp 零改动）。
- **15:00 Go 基准重测**（`9fdeb66a`）：
  - 历史口径查实：`survey.md:29` 135.49 是 **min**（median 185），源码 `/tmp/conc_survey/src/bench_go.go` 已丢失、git 无记录；`122.0` 仅 `engineering-standards.md:124` 一行，且 0.98× 与 122/135.49=0.90 不自洽 → **两个数字都口径不可考**；
  - 本机 Go 1.27.1 实测（taskset 2,3、100k×5 轮×3 进程）：@1 **736.2**、@2 **264.9**（复跑 251）、@4 **162.7**（复跑 151）、@8 183.4、GOGC=off@4 179.4；
  - C++ 同轮 359（5 轮口径 283）→ **Go@4/C++ = 2.21×**（最好组合 1.47×）；**不达标，但真实差距 1.4–2.5×，不是历史宣称的 5×**；
  - 判定与建议已写 ledger M16；优化目标从「追 122」改为「追 Go@4 本机 151–163」。
- 15:03 派 `7bfcd2c6` 读 Go 1.27.1 runtime 源码（/usr/local/go/src/runtime/）出「可借鉴点清单 + Top-3」；GOROOT 路径已确认，`third_party/go-reference` 只有 README+SHA256SUMS 无源码。
