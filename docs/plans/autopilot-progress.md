# Autopilot 进度（Go2Cpp）

> 节奏：每 15 分钟一轮（2026-11-03 14:46 用户从 30 分钟改为 15 分钟；定时任务 `Go2Cpp 自动驾驶推进`，id `schedule-0ae729ea`）。每轮只做「下一个任务」一项；用户 15:20 起允许并发多任务（本机基准互斥、不占 CPU 的任务可并行）。
> 规则：做完必须验证；失败如实记；WSL 真树 commit，push 走 Windows 侧 Python；更新本文件。
> **汇报格式（强制）**：每轮按 `docs/plans/reply-format.md` 的五段式——结论 / 完成内容 / 证据 / 下一步 / 待确认；禁止整段挤压、句子内堆砌代码、emoji 标题。
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
| v3-006 | 四态 Socket（绑定 net::Socket + SendBatch） | ✅ b88de85+dbd0f68+6820dd1：406 checks；TCP 批量 20.8×、UDP 1.20× |
| v3-007 | EventLoop + 毫秒定时器 | ✅ ec088d3：44 checks；1000 定时器 max 1.2 ms、64 包/tick |
| v3-008 | 协议注入 + memset 零拷贝反证 | ✅ 6820dd1：44 checks；反证合规 + 负向对照抓 bug |
| v3-009 | 可靠传输骨架 | ✅ 07eb241：64 checks；1e6 次 47–52 ns/op |
| v3-审查 | 三件套（架构/汇编/正确性+性能） | ✅ d3af2db（docs/reports/rdnet-v3-review.md） |
| C1 | 抖音骨架 + IDL | ✅ 89ccb58 |
| C2 | user 服务（存储/鉴权/4 接口） | ✅ 3ff147f：25 PASS；QPS 1918.5、p50 4.04ms、p99 7.55ms |
| C3 | video 服务（videos 表/游标/403/404） | ✅ d0d7798：52 PASS；QPS 3197/p99 0.51ms |
| C4 | feed 服务（热度/关注流/分页） | ✅ d0d7798：25 条 3 页无重复；QPS 1773.8/p99 0.94ms |
| C5 | interaction（点赞幂等/评论/计数） | ✅ 8709fd1：80 PASS |
| C6 | search（前缀+包含/空 q 400/分页） | ✅ 8709fd1：5 用户命中、非法参数 400 |
| C7 | live（创建/开播/关播/进出） | 🔄 63e1b702 进行中 |
| C8 | gateway（接通用组件：6 路由/401/聚合健康） | 🔄 63e1b702 进行中 |
| C9–C10 | 验收脚本 + loadgen + 可靠性 | ⏳ |
| 通用组件 | singleflight + 一致性哈希 + Gateway | ✅ 93864a6（singleflight 80.6 ns/op、一致性哈希 13.1 ns/op）+ 50a2abf（Gateway checks=50） |
| B-REQ | B 站复刻需求文档 | ✅ 2a9d43a（255 行） |
| B-弹幕 | B 站弹幕服务（WS/广播/历史/限流） | ✅ d55cb83：A 发 B 收；50 连接 p99 9.43ms |
| realtime | 通用实时房间组件（给抖音复用） | 🔄 精简版重派中（首次派单超时） |
| B1–B4 | B 站其余（用户/视频/会员/压测） | ⏸ 暂停（抖音优先）；需求文档保留 |

## 下一个任务

**C7+C8 抖音 live + gateway（下一个，63e1b702 在跑）**：
1. 扩展 store rooms 表；live_service（29105）：创建/开播/关播/列表/进出（viewer_count 不为负、非房主 403）；
2. gateway.cpp 改用 `go2cpp::app::gateway`：6 条前缀路由 + Bearer 中间件 + `/api/health/all` 聚合；
3. 验收：经网关 6 路由 200、无 token 401、未知 404、聚合健康；live 进房并发 viewer_count 归零；
4. 压测：经网关 `/api/feed` 1000 次报 QPS/p99。
注：B 站弹幕 demo 修复（禁跳进度/批量跳出/大小速度）由 `5d0663af` 并行；代码量统计：C++ 498,319 行 / 1090 文件（include 103,899 + src 153,800 + tests 234,304 + app 6,316）。

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

- 17:20 **REQ-008 完成**（6820dd1）：memset 反证合规（负向对照 failures=2 能抓 bug）、SendTo 1 万次 0.79 ms、64 包/tick、44 checks。
- 17:30 **命名空间统一**（425a460b）：`event_loop.hpp` include `protocol.hpp`、删占位类 -43 行、`protocol.hpp` 回 `rd::net`；两测试 44/44 全过。
- 17:35 **SendBatch**（5e102f7f）：一次 sendmmsg/writev 提交 N 段；TCP **21.2×**（225 vs 4804 ns/seg）、UDP **1.20×**；部分提交 kWouldBlock + K；checks 406。
- 17:40 主控统一提交 6820dd1（REQ-008+SendBatch+命名空间+event_loop 链接修复），四套 406/44/64/44 全绿；acceptance v3 与 JinShan G-004 已写。
- 17:45 **三件套完成**（d3af2db）：架构（分层/所有权/零拷贝/批量）、汇编（SendBatch 3107 指令、WriteBatch 311、EventLoop::Run 栈帧 152B）、正确性+性能（TCP 20.8×/UDP 1.20×、单条开批量 0.90× 默认关）。

- 16:45 **C2 完成**（dbd542d5 → 3ff147f）：`douyin_store`（SQLite WAL + users/follows）、`douyin_auth`（SHA-256+盐/HMAC-SHA256/base64url）、user_service 4 接口；accept_user.sh **25 PASS / 0 FAIL**；并发 100 不同 email 全 200、同 email 1×200+99×409；GET 1000 次 **QPS 1918.5、p50 4.04ms、p99 7.55ms**；随机 token 1000 全对。
- 16:50 **通用 Gateway 组件**（b2d4cf18）按用户要求重做为注册式路由 + 中间件链 + 可插拔 Resolver + JSON 热加载（初版 `std::vector<Route>` 被否）。

- 17:20 **通用组件收口**：`50a2abf` Gateway（注册式路由/中间件/Resolver/JSON 热更新，checks=50、QPS 2206.5）；`93864a6` singleflight + 一致性哈希（80.6/13.1 ns/op）。
- 17:40 **转向 B 站复刻**：用户确认「B 站弹幕+播放+会员」核心闭环；需求文档 `app/bilibili/REQUIREMENTS.md` 255 行（5 表、WS 协议、7 场景、B1-B4）提交 `2a9d43a`；B1（存储+用户）派 `4ff5f73f`。

- 17:55 **抖音恢复优先**（用户决定）：B 站 B1（`4ff5f73f`）已停，B 站需求文档保留；恢复抖音 C3+C4（`7767cd2c`：扩展 store videos + video_service + feed_service + 分页/压测）。

- 17:05 **C3+C4 完成**（7767cd2c → d0d7798）：videos 游标分页 52 PASS、25 条 3 页无重复、详情 QPS 3197/p99 0.51ms、feed QPS 1773.8/p99 0.94ms；修多进程 WAL ORM 残留游标。
- 17:06 **B 站弹幕完成**（675cf514 → d55cb83）：A 发 B 收（3 条同时到达）、429/屏蔽词/房间隔离、50 连接 p99 9.43ms。
- 17:07 realtime 组件首次派单超时（零产出），已缩小范围重派。

- 18:05 **C5+C6 完成**（344a41e8 产出 + 主控验证 → 8709fd1）：interaction（点赞幂等/评论/计数）+ search（空 q/非法 type 400、limit 夹紧）；accept 脚本 80 PASS / 0 FAIL。
- 18:06 代码量统计：C++ 498,319 行 / 1,090 文件（include 103,899、src 153,800、tests 234,304、app 6,316）；已给用户审查策略（自动化门禁 + 接口/测试审查 + 高风险抽查 + 每轮 diff），待确认是否生成审查地图。

## 抖音 demo 状态（2026-11-04 10:20，用户指示「暂停抖音 demo」）

- **已暂停**：7 个服务与 tunnel-client 全部停止，端口已释放；代码/数据/素材保留在 `app/douyin/`；
- 保留内容：`app/douyin/`（6 服务 + gateway + 前端 + 上传 + 脚本）、`data/douyin.db`（1001 用户 / 1000+ 视频）、
  `web/videos/`（demo1-5 真实素材）；`scripts/serve_public.sh` 可一键恢复；
- 恢复命令：`cd app/douyin && bash scripts/serve_public.sh`；
- 后续改动需用户明确指令。

## 今日完成（2026-11-04）

| 项 | 提交 | 结果 |
|---|---|---|
| CI + 一键门禁（ci.sh 312 行 + Actions + badge） | 9544b7d | quick 10m16s；278 项 270P/4F/4S（4 项既有环境问题，CI_ALLOW_KNOWN 排除 + tsan） |
| cgo 开销调研（274 行） | a885d8c | 固定 17-32ns/次；同算法 C 比 Go 慢 4-20%；加速靠 SIMD |
| 外部市场调研（354 行） | a458ebc | 胜算法：C++ 实时长连接网关 |
| 内部能力盘点（350 行） | 9f22316 | 达标 15.8%；能做 3 类/做不了 3 类 |
| 战略决策（87 行） | 2dbe116 | 先 WS 压测验证 A + 跨平台 + 分布式 P0 |
| 分布式 P0（NodeId/Trace/metrics，1410 行） | 99ed212 | 1e6 无碰撞；2/2 测试；三件套 |
| Scheduler 进程级唯一 + 16 示例迁移 | 7c2329c | 计数 0→1→2→1→0 |
| ErrorScope（Fiber 链多重 Error + drain） | bc3e256 | 5 用例 40 checks；infra-usage.md 188 行 |
| rd_net Windows（MSVC）实现（2090 行） | c3f88b2 | _WIN32 分支；63 socket 上限；platform-diff.md 189 行 |

## 今日完成（2026-11-04，41 个提交）

| 线 | 内容 | 关键提交 |
|---|---|---|
| 三件套 | 44 模块盘点 + 7 份新汇编证据（HTTP/gzip/reflect/TLS/WS/tunnel/bufio）+ 缺失项归属 + 真差距表 | 54c439b/756aa6b/76508ed |
| 架构优化 | **WS 单 poller（p99 −45%，0.93× Go）**、**TLS 多路 SIMD（13.40×→1.28×）**、**tunnel 合并写（+59%）+ RelayPipe 直通（延迟 −22%）** | e9f84d5/7a4d310/b5b2a9e/718336c |
| 基础设施 | reflect/error/config/log 覆盖审计 + storage/app/rpc 接日志 + app 纳入构建 + platform-diff 9 项 | 85315e1/4fcc407/0f6fd2e/daea8d5 |
| JinShan | **REQ-010 打回闭环（真 MSVC 2026 编译+运行 406/44/44/64 全过）**；REQ-011 待口径 | 4cc501a |
| 存储 | kv 读 1.11–1.25×（0.48× 是误引）；写路径 0.24× 待立项 | 7d2870b |

## 下一个任务（2026-11-04 13:52 起）

1. **REQ-011 回执**（等用户选：A 声明只交付 Linux / B 做 network Windows 适配）；
2. **TLS AVX-512 IFMA**（Poly1305 9.28→18-24 GB/s，300–400 行，可把 1.28× 压到 ≤1.1×）；
3. **tunnel splice/io_uring**（每段零拷贝，目标开销 ≤2×；删线程已证伪）；
4. **WS 剩余**：每事件条带锁 + epoll_ctl MOD + 单批事件数（客户端单线程已构成下限）；
5. **kv 写路径 0.24×**（8MB memtable 同步 compaction，需另立任务）；
6. CI Windows 转门禁（待 GitHub runner 首次通过）。
