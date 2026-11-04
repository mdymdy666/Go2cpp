# 最终全量验收（隔离 worktree，2026-11-04）

- 真树：WSL `/UserData/CodexWorkSpace/Go2cpp`（小写 pp），HEAD `d5cb0fa`。
- 隔离：`git worktree add --detach /tmp/final-gate HEAD`；构建目录 `/tmp/final-gate/.build-gate`，**全量重建**
  （Release + `GO2CPP_WITH_SQLITE=ON`，`cmake --build -j4`，BUILD_EXIT=0）。未改代码、未 commit。
- 工作区另有 5 个未提交文件（test_go_net 时序修复、03-partial-write 文档等）**未进入本树**，
  因此本结果是纯 HEAD 口径；这些改动不影响下述失败点。
- 注：WSL 时钟比宿主慢约 36 分钟，本文所有 mtime/耗时均为 WSL 时钟。

## 六项门禁矩阵

| # | 门禁 | 结果 | 证据 |
|---|---|---|---|
| ① | 全量 ctest | **绿** | 203 项：200 通过 / 0 失败 / 3 跳过（tools-migration、tools-cli、tools-replay，缺 go 工具链），`100% tests passed`，159.86 s（`-j4`，为 40 分钟盒采用并行；0 失败故无需串行复核） |
| ② | ASan/UBSan | **绿** | 3 个核心二进制：`net_batch` 46156 checks/0 fail；`kv` 52665 checks/0 fail；`fast_channel` 修复后 3/3 次 0 泄漏（`WaitNodePool` TLS 池已归还）、功能全过；无 ASan 越界、无 UBSan 报告 |
| ③ | TSan | **绿（20/20 零 thread leak）** | `setarch x86_64 -R` + TSan `go2cpp_tunnel_test` 连跑 20 次（两组各 10 次）：thread leak **0/20**，每次均 `170 checks / 0 failures`（ratio 3.46–3.87x，全部 < 4.0）。根因是 `MuxStream::Read` 的条件变量**丢失唤醒**（`Close`/`MarkPeerClosed` 在锁外改标志 + notify，撞上「谓词已判 false、尚未 futex 等待」窗口 → 读线程永久阻塞 → `RelayPipe::Run` 的 `Join` 卡死）；已改为持锁置位 + 唤醒，并在 `SendPayload` 取到发送锁后复查 `alive()`（session Stop 后不再进入 30 s 写）。0 data race。详见 ③-4 |
| ④ | 零告警 | **红** | 构建日志 54 条 `warning:`，排除 8 条 debug_dump 后 **46 条**：unused-function 23、unused-result 15、-Wcomment 14、switch 2；集中于未改动的既有文件（test_hook_correctness 13、hpack.cpp 8、debug_event_ring 5、container/ring/list 头 14） |
| ⑤ | 构建一致性 | **绿** | `check_build_freshness.sh --build-dir .build-gate` → `RESULT: FRESH`（源码最新 18:52:49 < 产物最新 18:57:34；sources 491 / artifacts 34） |
| ⑥ | soak 二进制一致性 | **红（不一致）** | soak 报告自述被测二进制构建于 18:24–18:36；而 `src/tunnel/{client,server}.cpp` 于 **18:44:06** 被 `fe54cc5` 修改（200 行），soak 的 tunnel 段**不含**该修复。network/ws/http 源码在 18:35 后未变，这两段有效 |

## 真失败清单（已排除端口冲突/并发构建/环境限制）

1. **P2 内存泄漏（ASan 实测）——已修复（2026-11-04）**：根因是 `WaitNodePool` 的
   thread_local 池用 `std::vector<Node*>` 存裸指针，线程退出只析构 vector，堆缓冲里的
   节点永不释放（泄漏栈全部指向 `channel.hpp` 的 `new Node()`，覆盖 SPSC/MPMC worker 与
   main 的 CloseDrain 两条路径）。修法：池槽位改为 TLS 内固定数组 + 长度，`~LocalPool`
   在线程退出时逐个 delete。`.build-asan` 连跑 3 次 0 泄漏、功能全过。详见文末修复记录。
2. **P2 线程泄漏（TSan 实测，偶发）——watchdog 已加固，残留未闭环**：`RelayPipe::Run`
   的看门狗启停已用 RAII guard + `~RelayPipeState` 兜底 + 防重入封闭（任何返回/异常路径
   都 `StopWatchdog()`）。但修复后 3 次中 1 次仍报 thread leak，栈为
   `relay_pipe.cpp:914`（`directions[0]->Start`）← `server.cpp:1236`（`HandlePublic`），
   TSan 标注线程 **finished but not joined**：真正卡住的是 `Run` 在 `done_cv.wait` 等另一
   方向，而 `TunnelServer::Spawn`（server.cpp:1607）detach 的 worker 在 `Stop` 时未被等待、
   在途 public 连接未关闭（`Run(nullptr)` 用 Background context 永不取消）。relay 侧无法
   单独闭环，需上层修；**无 data race**（170 checks 验证）。详见文末修复记录。
3. **P3 告警门禁未达标**：46 条既有 warning（非本次新增，但「零告警」判红）。
4. **P2 覆盖缺口（⑥）**：soak tunnel 段早于 `fe54cc5`，330 GB/21,119 轮结论不能覆盖当前 HEAD 的
   tunnel 代码；需在干净环境重跑 tunnel soak（≥300 s）后方可引用。

## 结论

**未达工业级放行（有阻塞）**：①⑤ 绿；②③④⑥ 红。其中 ②③ 是真实产品缺陷（内存/线程泄漏），
④ 是门禁标准未达成（既有债），⑥ 是验证覆盖缺口而非新缺陷。建议按序修 ②③ → 清 ④ →
重跑 ⑥ 的 tunnel 段，再复验本矩阵。


## 修复记录（2026-11-04 19:00–19:45，未 commit）

### ② ASan 内存泄漏 —— 已闭环

- 根因：`WaitNodePool` 的 thread_local 池以 `std::vector<Node*>` 持有裸指针；线程退出时
  只析构 vector 自身，堆缓冲里的节点永久失去所有者。泄漏栈全部指向 `channel.hpp` 的
  `new Node()`，覆盖 SPSC/MPMC worker 与 main 的 CloseDrain 两条路径（3968 B / 38 次）。
- 修法（`include/go2cpp/concurrency/channel.hpp`）：池槽位改为 TLS 内固定数组 + 长度，
  `~LocalPool` 在线程退出时逐个 delete；槽位在 TLS 内，即使泄漏检查早于 TLS 析构执行，
  缓存节点也从 TLS 根可达，不会误报。
- 验证：`.build-asan` 重编译后以 `setarch x86_64 -R` + `ASAN_OPTIONS=detect_leaks=1`
  连跑 3 次：0 泄漏、`all checks passed`；原 3968 B/38 次报告消失。

### ③ TSan 线程泄漏 —— watchdog 已加固，残留根因在 detached worker

- 已修（`src/relay/relay_pipe.cpp`）：`Run` 在 `StartWatchdog()` 后立即构造 RAII guard，
  任何返回/异常路径都 `StopWatchdog()`；`~RelayPipeState` 兜底 `StopWatchdog()`；
  `StartWatchdog` 防重入（旧线程先 join）。正常路径行为不变。
- 实测残留（3 次中 1 次）：TSan 栈为 `relay_pipe.cpp:914`（`directions[0]->Start`）←
  `server.cpp:1236`（`HandlePublic`），线程状态 **finished but not joined**。即 watchdog
  已被 guard 正常 join，泄漏的是 Direction 线程：`Run` 仍卡在 `done_cv.wait` 等另一方向，
  进程已退出。
- 根因：`TunnelServer::Spawn`（`server.cpp:1607`）detach 的 `HandlePublic` worker 在
  `TunnelServer::Stop` 时未被等待，在途 public 连接未关闭；`RelayPipe::Run(nullptr)` 使用
  `Context::Background()` 永不取消，故 `Run` 只能等两个方向自然结束。
- 结论：relay 侧无法单独闭环；需在 `server.cpp` 的 Stop/析构路径跟踪并关闭在途 public
  连接、join worker（超出本次「只改 relay_pipe.cpp + channel.hpp」授权，保留为阻塞项）。

### ③-2 TSan 线程泄漏 —— server 侧取消链路已打通，未清零（2026-11-04 19:10–19:45）

- 根因确认：`TunnelServer::Spawn` 的记账已在 `fe54cc5` 落地（`m_workers` 计数 +
  `stopping` 判定），但 `HandlePublic` 里 `RelayPipe::Run(nullptr)` 传的是
  `Context::Background()`——**永不取消**。`Stop()` 关掉监听后，在途 public 连接的 `Run`
  只能等两端自然 EOF，卡在 `done_cv.wait`；`WaitWorkers` 超时后对象析构，Direction 线程
  finished-not-joined。
- 修法（`src/tunnel/server.cpp` + `include/go2cpp/tunnel/server.hpp`）：
  1. 新增 `m_stop_ctx` / `m_stop_cancel`（`Context::WithCancel(Background())`）；
     `Stop()` 在置 `stopping` 之前 `m_stop_cancel()`，在途 `RelayPipe::Run` 立即从
     `done_cv.wait` 醒来并 `ForceCloseAll`；
  2. 新增 `m_inflight_public` 登记表 + `TrackInflight` / `UntrackInflight`，
     `HandlePublic` 入口用 RAII 守卫登记（所有早退路径都注销）；`Stop()` 取快照后在锁外
     逐条 `TcpConn::Shutdown()`（幂等、任意线程可调、只 `shutdown(SHUT_RDWR)` 不 `close`），
     打断卡在 `ReadHttpHead` / `Read` 的 worker；
  3. `HandlePublic` 与 `ServeRawStream` 的 3 处 `Run(nullptr)` 改传 `m_stop_ctx`。
- 验证（`build-tsan`：Release + `-DGO2CPP_ENABLE_TSAN=ON`，12 核）：
  `setarch x86_64 -R ./go2cpp_tunnel_test` 连跑 5 次 → 泄漏线程数 **1 / 2 / 0 / 0 / 0**
  （修前基线 4/5 次复现）。诊断打印证实取消链路有效：`Stop()` 阶段 `stop cancel fired`
  正常触发，`pipe.Run` 在 Stop 之前的自然返回路径 `IsDone()==0`。
- 残余（**超出本 agent 授权，需上层继续**）：
  1. `src/tunnel/client.cpp:618` 的 `RelayLocal` 仍是 `pipe.Run(nullptr)`，与 server 侧
     修前完全同构——ts2 的泄漏栈之一即 `TunnelClient::RelayLocal`（5 次中 1 次）；
  2. `HandlePublic` 仍有偶发 1 条 Direction leak（5 次中 2 次）：推测为 `Join()` 卡在另一
     方向对 `MuxStream::Write`（session 已 Stop、写队列无超时）上，reader 已 finished 而
     writer 未退；修它要动 mux/client，超出「只改 server.cpp/server.hpp」授权。
- 性能断言：`tests/tunnel/test_tunnel.cpp:884 ratio < 4.0` 在 TSan 下波动到 5.69x / 8.58x，
  同一二进制常规构建连跑两次也是 4.08x / 5.69x，属阈值过敏；本次改动不在热路径
  （每条连接只多 2 次 mutex 登记，relay 数据面未动）。

### ③-3 TSan 线程泄漏 —— client 侧闭环（2026-11-04 19:20–19:50）

- 根因：`src/tunnel/client.cpp` 的 `RelayLocal` / `RelayRawStream` 与 server 修前同构，
  `RelayPipe::Run(nullptr)` / `SplicePipe::Run(nullptr)` 用的是 `Context::Background()`
  ——永不取消。`Stop()` 只 join 主线程与数据线程，在途的 detached 转发线程卡在
  `done_cv.wait` 等另一方向；`active` 不归零，等待超时后对象析构，TSan 即报 thread leak。
- 修法（`src/tunnel/client.cpp` + `include/go2cpp/tunnel/client.hpp`，未 commit）：
  1. 新增 `m_stop_ctx` / `m_stop_cancel`（`Start()` 里 `Context::WithCancel(Background())`）；
     `Stop()` 在置 `stopping` **之前** cancel，在途 `Run` 立即从 `done_cv.wait` 醒来并
     `ForceCloseAll`；
  2. 新增 `m_inflight_local` 登记表 + `TrackInflight` / `UntrackInflight`；`RelayLocal` /
     `RelayRawStream` 拨号成功后用 RAII 守卫登记（所有返回路径都注销）；`Stop()` 取快照后
     在锁外逐条 `TcpConn::Shutdown()`，打断阻塞在本地 `Read` / 写队列上的 worker；
  3. `RelayLocal` 的 2 处、`RelayRawStream` 的 1 处 `Run(nullptr)` 全部改传 `m_stop_ctx`；
  4. `Stop()` 改为先置 `stopping` → 再 join 主/数据线程 → 最后 `WaitWorkers`（超时
     `kStopTimeoutMs = 15000`，原内联 10 s 等待抽成函数，注释同步）。
- 验证（`.build-tsan`：`-fsanitize=thread -fno-omit-frame-pointer -g -O1`，12 核）：
  - 首轮 5 次 thread leak：**0 / 0 / 1 / 0 / 0**，唯一 1 条栈为 `HandlePublic`
    （`server.cpp:1287`），**client 栈 0 条**；该次 `ratio=4.14x` 触发阈值断言。
  - 追加 10 次：client 栈 **0/15 次**；server 栈 1 条（同 `server.cpp:1287`）；
    `ratio` 失败 1 次（4.14x，阈值 4.0）；其余 9 次 `170 checks / 0 failures`。
  - `setarch x86_64 -R ctest -R '^tunnel$'`：**1/1 Passed**（6.18 s）。裸 `ctest` 不带
    `setarch` 会因 TSan `unexpected memory mapping` 直接失败（ASLR 运行方式问题，非代码）。
- 剩余（**超出本 agent 授权**）：`HandlePublic` 偶发 1 条 Direction leak（15 次中 2 次，
  栈 `relay_pipe.cpp:916` ← `server.cpp:1287`），与 ③-2 记录一致；需在 server/mux 侧保证
  `Run` 返回后两个 Direction 都能 join（writer 卡在无超时的 `MuxStream::Write`）。

### ③-4 TSan thread leak 清零 —— mux 条件变量丢失唤醒（2026-11-04 19:35–20:05）

- 复现：`.build-tsan` 重编译后首跑即 1 条，栈 `relay_pipe.cpp:916`（`directions[1]->Start`）
  ← `server.cpp:1287`（`HandlePublic` 的 `pipe.Run`），TSan 标记该线程 **finished**。
- 根因（`src/tunnel/mux.cpp`）：`MuxStream::Read` 在 `m_mutex` 内检查「队列非空 /
  `m_closed` / `m_eof`」后进入 `m_cv.wait`，但 `Close()` 与 `MarkPeerClosed()` 在
  **锁外**改标志并 `notify_all`。两者交错时（谓词检查为 false → 对端置位 + notify →
  本线程才挂 futex）唤醒丢失，读线程永久睡眠；`RelayPipe::Run` 因此卡在 `Join`，进程退出
  时 Direction 线程即 TSan thread leak。这也解释了「Stop 取消链路已打通、仍偶发 1 条」，
  并与 TSan 的 finished 状态相符（泄漏的不是 writer，而是 Run 在 join 另一个方向）。
- 修法（仅 `src/tunnel/mux.cpp`，未改 relay_pipe）：
  1. `MuxStream::Close()` 改为**持 `m_mutex`** 做 CAS 置位 + `notify_all`；网络动作
     （CLOSE 帧 / `RemoveStream`）移到锁外，避免持流锁做最长 30 s 的写；
  2. `MarkPeerClosed()` 同样持锁置位 + 唤醒；
  3. `MuxSession::SendPayload()` 在拿到 `m_send_mutex` 后**复查 `alive()`**：等锁
     期间会话已 Stop 时立即失败返回，不再进入 `WriteAll`（writer 因而可 join）。
  说明：`PushData` 本就「锁内入队、锁外 notify」，谓词受锁保护，无丢失唤醒。
- 验证：
  - `setarch x86_64 -R` + TSan 连跑 **20 次（两组各 10 次）：thread leak 0/20**；每次均
    `170 checks / 0 failures`，ratio 3.46x–3.87x。
  - 修前同一二进制首跑即复现 1 条（栈见上）。
  - `setarch x86_64 -R ctest --test-dir .build-tsan -R '^tunnel$'`：
    **1/1 Passed（6.19 s）**；`clang-format --dry-run --Werror` 通过，重编译零告警。
- 为什么没给 `Run` 的 `done_cv.wait` 加 5 s 超时：它等的是「两方向都结束」，
  keep-alive / 大文件等长连接正常可持续数分钟，5 s 超时会误杀健康连接；真正要修的是
  唤醒丢失，已在 mux 侧闭环。

### 门禁状态变化

- ② ASan：红 → **绿**（3/3 零泄漏）。
- ③ TSan：红 → **绿**（mux 丢失唤醒修复后 20/20 零 thread leak，170 checks 全过；
  `ctest -R '^tunnel$'` 通过）。
