# Autopilot 进度（Go2Cpp）

> 节奏：每 30 分钟一轮（定时任务 `Go2Cpp 自动驾驶推进`，id `schedule-0ae729ea`）。每轮只做「下一个任务」一项（可派 1–2 个子 agent）。
> 规则：做完必须验证；失败如实记；WSL 真树 commit，push 走 Windows 侧 Python；更新本文件。
> 真树：`/UserData/CodexWorkSpace/Go2cpp`（分支 `feat/isolated`）。

## 总目标（三阶段，按顺序）

| 阶段 | 目标 | 完成判据 |
|---|---|---|
| A 数字台账 | 所有性能数字带「命令/环境/日期/脚本」，可一条命令复跑 | `tools/verify_metrics.sh` 跑完无未解释 STALE；docs 无孤立数字 |
| B 公网变服务 | VPS systemd + 本机 start-all + 探针 + 断线恢复 | VPS 重启后公网 200；kill client 自动恢复；探针 30 分钟无失败 |
| C 抖音 P0 | 6 服务 + 网关串成一个应用 | 5 个真实场景 curl 可复现；压测报告进 `docs/reports/` |

## 性能不达标处理规则（2026-11-03 用户补充）

当 ledger 判定某模块**同口径**下超判据（vs Go 或工业库 >1.1×）时：
1. 先开子 agent 做「口径对齐 + 基线复测」，确认不是测量/口径错误；
2. 确认不达标才优化，顺序强制：**架构（锁粒度/所有权/数据结构/并发模型）→ 汇编（指令/call/栈帧/分配）→ 正确性验证（功能 + 随机 + 压测 + 可选 TSAN）**；
3. 前后数据都写回 ledger；不得用「优化了」替代「达标了」；
4. 子 agent 不许 commit，主控审查（抽查行号/数字溯源/越界/正确性）后合并；
5. 风险高且时限内无法验证的改动，回滚并如实记「尝试过、因何回滚」。

## 基准必须独占（2026-11-03 实测教训）

并行跑基准会制造假 STALE：A5 全量在 A4c 并行压测时出 3 条假 STALE（M03 628 vs 239、cpu 中位 1.84×；M04、M08 同因），A4c 结束后独占复跑 → `TOTAL=16 OK=12 STALE=0 SKIP=4`。**对照运行期间不得有其它基准进程**；判 STALE 前必须独占复跑一次。

## 进度表

| 任务 | 内容 | 状态 |
|---|---|---|
| A1 | ledger 建档 | ✅ be73a4e |
| A2 | `tools/verify_metrics.sh` 三基线 | ✅ 92dcb25 |
| A3 | M04/M05/M06 填实 | ✅ 55a3f0e |
| A4a | M07 LevelDB | ✅ 761ea6c |
| A4b | M10/M11/M12/M13 | ✅ e7fcbbb |
| A4c | M14 TLS / M15 bento | ✅ 本轮（TLS 1.061×、bento 282k pps；A4c 超时被 interrupt，数据从日志补收） |
| A5 | 全量校验脚本（16 条）+ CMake 模板 | ✅ 本轮（493 行，独占全量 OK=12 STALE=0 SKIP=4） |
| **M03** | **scheduler wall 三件套优化（架构→汇编→正确性）** | 🔄 下一个（用户要求） |
| M10 | os readv 三件套优化 | ⏳ 排队 |
| M11 | gzip 解压 三件套优化 | ⏳ 排队 |
| A6 | 清理 411k/160ns 等文档漂移 | ⏳ |
| A7 | 其他不达标项（concurrency 原语、os Process、network vs Go/nginx） | ⏳ |
| B1–B4 | 公网变服务 | ⏳ |
| C1–C10 | 抖音 P0 | ⏳ |

## 下一个任务

**M03 三件套优化**（用户 2026-11-03 点名）：
1. 固定口径复现 baseline（12 vCPU、`taskset -c 2,3`、4 worker、100k、5 轮，记录 loadavg），确认 239/299 不是宿主噪声；
2. **架构优化**：至少一条（runnext 投递策略 / incoming 条带轮转 / enqueue 路径减少原子或锁）；
3. **汇编优化**：至少一条，附 `objdump` 前后指令/call/栈帧对比；
4. **正确性**：`scheduler_smoke`、`queue_latency`、`channel_concurrency`、`select_lockorder`、`fiber_select` 全过 + 一次压力；任一失败立即回滚该版；
5. 改善 <5% 或无法证明收益 → 回滚并如实报告「不可优化/代价过高」。

## 本轮记录

- 13:20 启动；A1 be73a4e；A2 92dcb25；优化规则 456ec05；A7 首项 3a7aa4f；A3 55a3f0e；A4b e7fcbbb；A4a 761ea6c。
- 14:21 本轮：
  - **A5 完成**（`6aa0caba`，被 interrupt 但文件完整）：`tools/verify_metrics.sh` 493 行、`cmake/Go2CppVerifyMetrics.cmake` 41 行（不被 include，`cmake -P` 实测 rc=0）；16 条接线、`--quick/--json/--refs`、退出码 0/1/2；
  - 独占全量：`TOTAL=16 OK=12 STALE=0 SKIP=4`（SKIP: M01 已拆条、M05 需起服务+Go loadgen、M05b 无出处、M07 超 5 分钟预算）；`--quick` 4/4 OK；
  - 插曲：首次全量在 A4c 并行压测下出 3 条假 STALE（M03 628/cpu 1.84×、M04、M08），独占复跑全消——已写进 ledger「教训」与本节；
  - **A4c 完成**（`b03dca46`，超 10 分钟被 interrupt，数据从 `/tmp/tlsbench-run*.log` 补收）：TLS 握手 1.061×（复跑 1.178×）、32KB 写 0.939、64B 0.945；bento single 282,210 pps（loss 0）、多队列 2 核丢包 39–48%；结论：**TLS 达标、单 peer 多队列无收益复现**；无法复现项：tunnel 8 MiB 吞吐（无压测程序）、bento bulk STREAM 字节吞吐（只有 64B 消息级）；
  - 主控修复：A5 收尾时脚本被写到半成品（line 494 语法错），interrupt 后确认 493 行 `bash -n` 通过并独占复跑通过；残留 `bench_link_mq` 用 `pkill -x` 杀净。
