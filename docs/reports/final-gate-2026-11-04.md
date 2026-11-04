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
| ③ | TSan | **黄（部分修复）** | 纯净 HEAD 重建 `.build-gate-tsan/go2cpp_tunnel_test`：170 checks / 0 功能失败、**0 data race**；thread leak 修复后 3 次中 1 次复现，残留为 `Direction` 线程 finished-not-joined（根因在 server.cpp 的 detached worker，见修复记录） |
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

### 门禁状态变化

- ② ASan：红 → **绿**（3/3 零泄漏）。
- ③ TSan：红 → **黄**（watchdog 路径已加固；残留为 `Direction` 线程 finished-not-joined，
  需上层修 server.cpp 的 detached worker 生命周期）。
