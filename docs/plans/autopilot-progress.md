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
| M03 | scheduler 三件套 | ⛔ **关闭**（用户 15:50 决定）：架构本就比 Go 复杂（无 GC/RAII/Context），优化 <5%、回归风险高；零改动 |
| borrow | Go 源码借鉴（3 函数，73 行：runnext 位移 / steal runnext / netpoll 前移） | ✅ 本轮（b5ccd296） |
| B1 | VPS `tunnel-server.service` | ✅ systemd enabled/active，restart 正常 |
| A6 | 清理 122/135/411k/0.98× 文档漂移 | ✅ 本轮（12 处 / 7 个 .md，未改历史数字，只加注） |
| M10 | os readv 三件套 | ✅ 本轮：iovec 合并 写 17.96→≈111×、读 15.32→≈213×；栈帧 16 KiB→40/56 B |
| M11 | gzip 三件套 | ⏳ 下一个 |
| B2 | 本机 start-all/stop-all/健康检查 | ✅ 36e980d（固化 scripts/shop/，全停→全起 6 项 200、幂等） |
| B3 | 外部探针 60s | ✅ 36e980d（probe.sh，5 分钟 5 次全 200） |
| B4 | 断线恢复 | ✅ 本轮：根因不是崩溃而是 GOAWAY 后僵尸态；改 client 退避重连 + watchdog 兜底；VPS restart 后 25s 自动 200 |
| C1–C10 | 抖音 P0 | ⏳ |

## 下一个任务

**JinShan v3 收口（下一个）**：
1. `df6a7f7c`（REQ-008 协议注入 + memset 反证）与 `e3f7876e`（REQ-006 绑定 `go2cpp::net::Socket` + bufio 批量 + `SocketOptions` 配置注入）交付后，主控复跑全量测试（socket 353 checks、event_loop 44、reliability 64、protocol 新增）；
2. 更新 `collab/acceptance.md` v3 章节（补 REQ-007/008 与「已绑定现有栈 + bufio 批量可配」说明）与 JinShan 回执 G-004；
3. **三件套**：架构审查（分层/所有权/零拷贝路径）→ `objdump` 指令/栈帧 → 正确性 + 性能复验（默认 vs batch 配置）；
4. 之后回 C2（抖音 user 服务）与 M11（gzip 口径对齐）。

## 本轮记录

- 13:20 启动；A1 be73a4e；A2 92dcb25；优化规则 456ec05；A7 3a7aa4f；A3 55a3f0e；A4b e7fcbbb；A4a 761ea6c；A5+A4c b0c9013；M03 第一轮 46b9d65；Go 基线 1b4ae22。
- 14:53 M03 三件套第一轮：baseline 320、P19 −5%、汇编 enqueue 597→580/栈帧 104→88B/divq 1→0、p22 −15~35% 但 smoke 失败 → 回滚（零改动）。
- 15:00 Go 重测：Go@4 162.7（@1 736/@2 264.9/@8 183.4/GOGC off 179.4）、C++ 359 → 2.21×（最好组合 1.47×）；历史 122/135 判口径不可考。
- 15:03 派 borrow（`7bfcd2c6`，11 分钟无产出被 interrupt）→ 改派 `b5ccd296`（只读 3 函数、8 分钟时间盒）。
- 15:14 按用户「并发开启其他任务」并发 4 个：`8af8dbe4`（VPS systemd）、`f3638275`（A6 文档漂移）、`24415abc`（固化 Go 基准）、`b5ccd296`（borrow）。
- 15:18 **固化完成**（`24415abc`）：`tools/bench/go_sched_bench.go`（75 行，正文与 /tmp 版 `BODY_IDENTICAL`）+ `tools/bench/README.md`（37 行）；编译 BUILD_OK；3 轮 290/176/133（median 176.19，min 133.43），复跑 256/131.8/184.2、5 轮中位 184.55 —— 与 M16 162.7 同量级但抖动大（核 2 有 cpptools/mysqld 共享），程序逐字节一致；主控抽查：75/37 行、BODY_IDENTICAL、`git status` 仅 `?? tools/bench/`。ledger M16 脚本路径已更新。已合并 `a2ce97b`。
- 15:22 **borrow 完成**（`b5ccd296`，73 行）：3 条建议（runnext displaced 走全局锁 / steal runnext / netpoll 前移），Go 与 scheduler.cpp 行号抽查全命中；已合并 `30220e3`。主控决定先做「p22 分块复核 + 建议①」。
- 15:26 **B1 完成**（`8af8dbe4`）：VPS 装 `/etc/systemd/system/tunnel-server.service`（17 行，`--display-host 82.157.147.127`，Restart=always），`enable --now` 后 is-enabled=enabled / is-active=active、8443+7443 LISTEN、`systemctl --failed` 无本项目；`restart` 后 PID 308685→308884、端口仍在。**公网 404** 根因定位：`/__tunnel__/status` 的 `tunnels:[]`，本机 client(PID 1684) 存活但未重注册。主控重启 client（`pkill -x` + `setsid nohup`，新 PID 198565）后：**public_products=200、public_health=200**，status 出现 `"subdomain":"shop"`、online_ms=6690。
- 15:28 **A6 完成**（`f3638275`）：12 处 / 7 个 .md 加注，历史数字保留；主控抽查 3 处通过；已合并 `6e1c568`。
- 15:50 **用户决定放弃 Scheduler 优化**：本项目 Scheduler 在状态机/所有权/Context/栈增长/IO 模型五个维度比 Go 更复杂（无 GC 的 C++ 运行时必须用引用计数+固定栈+显式状态机替代 GC 安全点与栈复制），2.21× 是语义代价不是实现差；M03 关闭，后续不再投入。
- 16:50 **M10 收口**（`a60c480b` + `f5653fb1` 两轮）：
  - 瓶颈：1024 段 × 8B 的 readv 32.2 µs/次 vs 单段 1.0 µs/次 → 99% 是内核逐段处理；
  - 改动：`MergeAdjacentIov`（只合并首尾相接、零长丢弃、≤8 段不扫描）；合并数组从 16 KiB 栈改为「仅命中时 `new(nothrow)`」；
  - 结果（3 次进程、taskset 2,3）：写 **109.72/113.65/110.56×**、读 **207.49/230.18/201.41×**，failures=0；栈帧 16440 B → 40/56 B；非相邻 iovec 无回退（+0.6%）；
  - 正确性：ctest 2/2、ScenarioVectoredIo + 自建边界全过。
- 16:20 **B4 修复完成**（`3a632dd2`）：
  - 根因纠正：不是「server restart 让 client 退出」，而是 GOAWAY → `RunLoop` 旧 `:377-380` 直接 break → 控制线程退出、进程存活但永不再连（**僵尸态**，公网 404）；「tunnel-client 已停止」只在 SIGINT/SIGTERM 时打印，是人工 pkill 打的；
  - 修 `src/tunnel/client.cpp:377-395`：GOAWAY 分支改为清 flag + `backoff.Next()`（500ms→15s、抖动 0.2）退避重连；`kUnauthorized/kBadVersion` 的 fatal 不重试语义保留；
  - 新增 `scripts/shop/watchdog.sh`（85 行，flock 单实例、10s 轮询 `pgrep -x` ∪ 公网 health、按精确 pid 收尾），定位「运维兜底非自愈」；
  - 验证：VPS restart 后 T+10..60s 全 200、PID 不变（日志「退避 486ms 后重连」）；`pkill -x tunnel-client` 后 T+5s 502 → T+10s 200（watchdog 拉起）；错误 token 10 秒 11 行不刷屏；
  - 主控复跑：VPS restart → 25s 后 `public_health=200`、client 存活（PID 211474）；
  - 已知缺口：错误 token 时 server 在 AUTH 阶段回 kUnauthorized 后立刻断连（`server.cpp:797-814`），client 未起 mux、ACK 丢失 → 误判为网络错误并退避重试（与修复前一致，属 server 侧既有协议缺口，未动）。
- 15:42 **M03c 收口**（`109eec90`，超时 45 分钟）：分块 smoke 各 10 次 → **P20 0/10**、其余 10/10；探针证明 P20 非语义破坏（park 最终到 kWaiting、p50<1µs、wake 正常），是 :111 断言在 park 前检查的竞态被放大；组合 **P19+P21+P22** 通过 ctest 5/5、smoke 20/20、1e6 压力；bench baseline 305 → 组合 **293（−3.9%）/ 324 vs 338（−4.1%）**，**<5% 门槛** → 回滚（`scheduler.cpp` 与 bak 逐字节一致，无改动）；borrow 建议①核实「现实现已一致」且 metrics 无法测 displaced。**M03 判「无可验证 >5% 空间」收口。**
