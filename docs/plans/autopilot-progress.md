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
| A2 | `tools/verify_metrics.sh`：接 M01b/M02/M03，跑出 OK/STALE/未接线 | ✅ 92dcb25 |
| A3 | 复跑并填实剩余 5 条：M04 scheduler submit、M05 HTTP pipeline=16、M06 SQLite 插入 | 🔄 子 agent `41fd2142` 在跑 |
| A4 | 复跑其余：bento/link、bufio、cache、os 批量、TLS、gzip、reflect/hash；不能跑的标原因 | ⏳ |
| A5 | ledger 接成可校验：一条命令全量对照 + STALE 列表；CI/CMake 钩子警告 | ⏳ |
| A6 | 清理漂移：411k、160ns 等改为指向 ledger 或标 STALE；grep 无孤立数字 | ⏳ |
| A7 | 不达标模块优化（架构→汇编→正确性） | 🔄 首项 M01b 已完成判定+汇编级；架构级（无锁化）建议单独立项 |
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

**A4**（A3 交付后若已填实 M04/M05/M06，则进入 A4）— 复跑其余模块：
bento/link、bufio（已有 M08）、cache（已有 M09）、os 批量 syscall（M10）、TLS（握手 1.070×）、gzip（压缩/解压）、reflect/hash（SHA-256 2.0–2.4 GB/s、FNV）。
每条四要素；不能复跑的写「无法复现 + 缺什么」。M01b 的架构级无锁化单独立项（需完整验证窗口）。

## 本轮记录

- 2026-11-03 13:20 启动：建定时任务 `schedule-0ae729ea`（1800s），建本文件。
- 13:2x **A1**（be73a4e）：ledger 12 条，M03/M08/M09 实测，M05b 无出处。
- 13:33 **A2**（92dcb25）：`tools/verify_metrics.sh` 198 行；M01b 112.6→STALE、M02 19.84 OK、M03 299 OK；抽查 3 行号命中、无越界。
- 13:43 用户补充「不达标模块按架构→汇编→正确性优化」，写成硬规则（456ec05）。
- 13:50 **A7 首项 M01b 完成**（子 agent `310cf0ca`）：
  - 口径判定：历史 10.73 是 `FastChannel<1024>`（`vs-concurrency.md:88`、`survey.md:133`），不是通用 Channel；Go 23.36 同为 `taskset -c 2,3` SMT 兄弟；
  - 通用 Channel 实测 112.6（min 103.3）vs Go 23.36 = **4.8×，不达标**；`tryspsc≈spsc`、跨物理核同值 → 瓶颈是跨核 cacheline 传递，单锁下限 60–90 ns，达标需无锁化；
  - 汇编级优化：`kWouldBlock` 返回空 `ErrorPtr`（省 2 次 shared_ptr 原子 RMW）→ TryRecv −18%、TrySend −17%、spsc 103.3→100.2 无回归；4 个正确性用例全过；
  - 架构级未做（覆盖 Send/Recv/Select/Close 全路径，超出可验证范围），建议立项；
  - 主控审查：diff 仅 +11/−3 两处、证据行号全命中（vs-concurrency:88、survey:43/51/133、channel.hpp:3039）、9 项历史改动完好、ctest 3/3 通过；
  - ledger 拆为 M01b-a（不达标 4.8×）与 M01b-b（FastChannel 达标）。
- 13:51 **A3 派活**（子 agent `41fd2142`）：M04/M05/M06，Go 1.27.1 已确认可用，`net_perf_server.cpp` 与 `http_loadgen.go` 存在。
