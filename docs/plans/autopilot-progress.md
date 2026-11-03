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
| A2 | `tools/verify_metrics.sh`：接 M01b/M02/M03 | ✅ 92dcb25 |
| A3 | M04/M05/M06 复跑填实 | ✅ 本轮（M04 双口径 119/355.7、M05 120.8k、M06 1.074×） |
| A4 | 复跑其余：M07 LevelDB、M10 os 批量、TLS、gzip、reflect/hash、bento/link | ⏳ 下一个 |
| A5 | ledger 接成可校验：一条命令全量对照 + STALE 列表；CI/CMake 钩子警告 | ⏳ |
| A6 | 清理漂移：411k、160ns 等改为指向 ledger 或标 STALE；grep 无孤立数字 | ⏳ |
| A7 | 不达标模块优化 | 🔄 首项 M01b 完成判定+汇编级；**架构级（通用 Channel 无锁化）待立项** |
| B1 | VPS `tunnel-server.service`（`--display-host 82.157.147.127`），enable + 重启验证 | ⏳ |
| B2 | 本机 `start-all.sh` + 健康检查 | ⏳ |
| B3 | 外部探针：每 60s curl 公网 health | ⏳ |
| B4 | 断线恢复验证；结论写 HANDOFF | ⏳ |
| C1 | `/tmp/douyin` 骨架 + 6 服务端口 + 共享 IDL | ⏳ |
| C2 | user 服务（SQLite） | ⏳ |
| C3 | video 服务（LevelDB） | ⏳ |
| C4 | feed 服务（聚合 user+video） | ⏳ |
| C5 | interaction 服务（SQLite） | ⏳ |
| C6 | live 服务（WebSocket 占位） | ⏳ |
| C7 | search 服务（内存倒排） | ⏳ |
| C8 | 网关（静态路由 + 聚合） | ⏳ |
| C9 | 端到端验收：≥5 个真实场景 | ⏳ |
| C10 | 压测 + 报告进 `docs/reports/` | ⏳ |

## 下一个任务

**A4** — 复跑其余模块并填实 ledger（每条四要素；跑不动写「无法复现 + 缺什么」，不许沿用旧数字）：
- **A4a**：M07 LevelDB 1e7 Put ops/s + 1e8 RSS（`go2cpp_leveldb_kv_test`；1e8 若超时就只报 1e7 + 说明）；
- **A4b**：M10 os 批量 writev/readv（`go2cpp_os_test` 的 `ScenarioVectoredIo`，需要自己计时）、TLS（`asm-tls` 的握手/写路径基准若已有工具则跑）、gzip（`bench_gzip_crc` 或 compress 测试）、reflect/hash（`bench_reflect_stage` / hash 测试）、bento/link（`tests/bento` 的吞吐基准或 `go2cpp_bento_*`）。

## 本轮记录

- 13:20 启动；13:2x A1（be73a4e）；13:33 A2（92dcb25）；13:43 用户补充优化规则（456ec05）。
- 13:50 **A7 首项 M01b**（310cf0ca）：口径判定 + 汇编级优化（TryRecv −18%/TrySend −17%），合并 3a7aa4f；ledger 拆 M01b-a（不达标 4.8×）/M01b-b（达标）。
- 13:59 **A3 完成**（41fd2142）：
  - 新增 `tests/concurrency/bench_scheduler_submit.cpp`（191 行，计时只包 `spawn()`，:150-163 已抽查）；
  - M04：batch=1024 中位 **119.0 ns**（35 轮，p25 92.1/p75 133.6），batch=65536 中位 **355.7 ns**（p25 312.1/p75 402.7）——历史 326.7 落在积压口径，条件未记录，**不能断言同口径**；
  - M05：pipeline=16 单连接 **120,755 rps**（6 轮中位，p50 7–8 µs、p99 15–16 µs、errors=0），pipeline=1 对照 9,122 rps；历史 72.9k 无法用旧二进制复验（旧 `libgo2cpp_bento_http.a` 缺 `HttpServer::Route`），重编后 +65.7%，已标注「库版本+负载差异，非同二进制对照」；
  - M06：**1.074×**（7 轮中位，wrapper 368.1 ms vs native 344.9 ms），历史 1.078 复现（−0.4%），达标；
  - 主控抽查：bench 文件 191 行、计时区 :156-163 确实只包 spawn、`git status` 仅新增该文件 + 9 项历史完好、日志 `/tmp/m04_*.log`/`m06_rounds.txt` 存在、`HttpServer::Route` 在源码 :259（旧库缺符号说法成立）。
