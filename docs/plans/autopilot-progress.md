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

## 进度表

| 任务 | 内容 | 状态 |
|---|---|---|
| A1 | `docs/metrics-ledger.md`：四要素格式 + 首批 10 条核心数字 | ✅ be73a4e |
| A2 | `tools/verify_metrics.sh`：接 M01b/M02/M03，跑出 OK/STALE/未接线 | ✅ 本轮（M01b STALE、M02 推断口径、M03 OK） |
| A3 | 复跑并填实剩余 5 条：M04 scheduler submit、M05 HTTP pipeline=16、M06 SQLite 插入；并确认 M01b 历史 10.73 口径、M02 API 口径 | ⏳ 下一个 |
| A4 | 复跑其余：bento/link、bufio、cache、os 批量、TLS、gzip、reflect/hash；不能跑的标原因 | ⏳ |
| A5 | ledger 接成可校验：一条命令全量对照 + STALE 列表；CI/CMake 钩子警告 | ⏳ |
| A6 | 清理漂移：411k、160ns 等改为指向 ledger 或标 STALE；grep 无孤立数字 | ⏳ |
| A7 | 不达标模块优化（首个：M01b 通用 Channel 112.6 ns vs Go 23.36；先口径对齐，确认不达标才按架构→汇编→正确性优化） | 🔄 子 agent `310cf0ca` |
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

**A3** — 复跑并填实剩余 5 条，写在 `docs/metrics-ledger.md`：
1. **M04 scheduler submit ns/task**（单段计时，区别于 M03 的 wall；需要新计时脚本或改 bench）；
2. **M05 HTTP pipeline=16 单连接 rps**（`tests/network/net_perf_server.cpp` + `tests/network/http_loadgen.go`，Go 环境；跑不到就写「无法复现：缺 X」）；
3. **M06 SQLite 1e6 行插入 vs 原生**（`tests/storage/bench_db.cpp`，7 轮中位）；
4. 确认 **M01b** 历史 10.73 的原始口径（是否 FastChannel 而非通用 Channel；查 docs 与提交史）；
5. 确认 **M02** 的 ledger API 口径（yield ping-pong 是否等价「全量切换」）。

约束：每条必须附「命令 + 环境（核数/绑核/轮数）+ 日期 + 脚本路径」；跑不出就写「无法复现 + 原因」，**不许沿用旧数字**。M01b 若确认历史口径不同，把 ledger 的 M01b 拆成两条（通用 Channel / FastChannel）并各自给实测。

## 本轮记录

- 2026-11-03 13:20 启动：建定时任务 `schedule-0ae729ea`（1800s），建本文件。
- 2026-11-03 13:2x **A1 完成**（be73a4e）：`docs/metrics-ledger.md` 12 条（M01/M01b/M02/M03/M04/M05/M05b/M06/M07/M08/M09/M10），M03/M08/M09 标实测，M05b（411k）标无出处。
- 2026-11-03 13:33 **A2 完成**（派子 agent `90a2d336`）：`tools/verify_metrics.sh` 198 行 + `.out` 72 行；
  - 实跑：M01b **112.6 ns** vs 10.73 → **STALE**（+949%）；M02 **19.84 ns** vs 23.5 → OK（推断口径）；M03 **299 ns** vs 239 → OK（+25%，cpu 中位 627）；
  - `TOTAL=3 OK=2 STALE=1 未接线=0`，退出码 1；
  - 主控抽查：`wc -l` 198/72 一致；行号 3 条（bench_channel_opt.cpp:84 PrintRow、bench_fiber_switch_opt.cpp:308 yield PrintRow、bench_scheduler_task_opt.cpp:102 printf）`sed` 全部命中；`git status` 仅新增 tools 两文件，未越界；无临时垃圾；
  - 已写回 ledger：M01b → STALE、M02 → 实测（推断口径）、M03 备注补 299；
  - 另有子 agent `18625e09`（doc-drift 台账）在跑，未回。
