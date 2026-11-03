# Autopilot 进度（Go2Cpp）

> 节奏：每 30 分钟一轮（定时任务 `Go2Cpp 自动驾驶推进`，id `schedule-0ae729ea`）。每轮只做「下一个任务」一项。
> 规则：做完必须验证；失败如实记；WSL 真树 commit，push 走 Windows 侧 Python；更新本文件。
> 真树：`/UserData/CodexWorkSpace/Go2cpp`（分支 `feat/isolated`）。

## 总目标（三阶段，按顺序）

| 阶段 | 目标 | 完成判据 |
|---|---|---|
| A 数字台账 | 所有性能数字带「命令/环境/日期/脚本」，可一条命令复跑 | `tools/verify_metrics.sh` 跑完无未解释 STALE；docs 无孤立数字 |
| B 公网变服务 | VPS systemd + 本机 start-all + 探针 + 断线恢复 | VPS 重启后公网 200；kill client 自动恢复；探针 30 分钟无失败 |
| C 抖音 P0 | 6 服务 + 网关串成一个应用 | 5 个真实场景 curl 可复现；压测报告进 `docs/reports/` |

## 进度表

| 任务 | 内容 | 状态 |
|---|---|---|
| A1 | `docs/metrics-ledger.md`：四要素格式 + 首批 10 条核心数字，逐条标待复跑 | ✅ 2026-11-03 |
| A2 | `tools/verify_metrics.sh`：跑通 Channel/Fiber/scheduler 三基线，对不上打 STALE | ⏳ 下一个 |
| A3 | 复跑并填实 5 条：Channel、Fiber、scheduler（wall+submit）、HTTP pipeline=16、SQLite 插入 | ⏳ |
| A4 | 复跑其余：bento/link、bufio、cache、os 批量、TLS、gzip、reflect/hash；不能跑的标原因 | ⏳ |
| A5 | ledger 接成可校验：一条命令全量对照 + STALE 列表；CI/CMake 钩子警告 | ⏳ |
| A6 | 清理漂移：411k、160ns 等改为指向 ledger 或标 STALE；grep 无孤立数字 | ⏳ |
| B1 | VPS `tunnel-server.service`（`--display-host 82.157.147.127`），enable + 重启验证 | ⏳ |
| B2 | 本机 `start-all.sh` + 健康检查（8080/9101/9102/9103/tunnel-client） | ⏳ |
| B3 | 外部探针：每 60s curl 公网 health，失败写日志与连续失败数 | ⏳ |
| B4 | 断线恢复验证：kill tunnel-client → 自动重连 → 公网 200；结论写 HANDOFF | ⏳ |
| C1 | `/tmp/douyin` 骨架 + 6 服务端口 + 共享 IDL | ⏳ |
| C2 | user 服务（注册/登录/资料，SQLite） | ⏳ |
| C3 | video 服务（上传元数据/列表，LevelDB） | ⏳ |
| C4 | feed 服务（时间线聚合，调 user+video） | ⏳ |
| C5 | interaction 服务（点赞/评论/关注，SQLite） | ⏳ |
| C6 | live 服务（房间/信令占位，WebSocket） | ⏳ |
| C7 | search 服务（内存倒排索引） | ⏳ |
| C8 | 网关（静态地址路由 + 聚合，不做服务发现） | ⏳ |
| C9 | 端到端验收：≥5 个真实场景 curl 可复现 | ⏳ |
| C10 | 压测 + 报告写进 `docs/reports/` | ⏳ |

## 下一个任务

**A2** — 建 `tools/verify_metrics.sh`：先接 M01b（通用 Channel）、M02（Fiber 切换）、M03（scheduler task churn）三条基线；能跑的输出实测值与 ledger 对照，对不上打 `STALE`；没接线的条目输出 `未接线`，不许假装通过。

## 本轮记录

- 2026-11-03 13:20 启动：建定时任务 `schedule-0ae729ea`（1800s），建本文件。
- 2026-11-03 13:2x **A1 完成**：新建 `docs/metrics-ledger.md`（12 条：M01/M01b/M02/M03/M04/M05/M05b/M06/M07/M08/M09/M10），四要素齐全；
  - M03/M08/M09 标 `实测`（2026-11-03 本机数据与命令），其余 `待复跑`，M05b（411k）标 `无出处`；
  - 核实：`tests/concurrency/bench_channel_opt.cpp` 存在；`grep -rn "411k|411000" --include=*.md` 无原始出处，只命中 exam 与 deep 教程的「无出处」说明；`160 ns` 只在 `docs/reports/asm-distributed.md:265`（分布式清扫，非调度器）。
  - 未做（留给后续轮）：A2 脚本、其余数字复跑。
