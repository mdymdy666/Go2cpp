# AI 协作记录（跨批次规范与第三方来源）

本文件记录**第三方代码移植来源**与跨批次的 API/命名空间约定；日常功能变更
仍写入项目既有的 `codex.md`（遵循 `PROJECT_RULES.md` §6 的「沿用既有记录」）。

## 2026-10-28 Bento app 层规范修复与 x25519 移植来源

### 一、x25519（`include/go2cpp/bento/net/x25519.hpp`、`src/bento/net/x25519.cpp`）

- **上游**：TweetNaCl（2013-10-27 版），<https://tweetnacl.cr.yp.to/>。
- **许可证**：public domain（上游 README 明示 "public domain"；本仓库整体仍按 MIT 分发）。
- **移植范围**：`crypto_scalarmult` 的 Montgomery ladder 与域运算辅助函数
  `car25519` / `sel25519` / `pack25519` / `inv25519` / `A` / `Z` / `M` / `S`；
  16×16-bit limb 表示、进位规则、常数时间条件交换保持上游结构。
- **移植日期**：2026-10-28。
- **改动点**：C++17 固定 32B 接口、去掉全局状态、中间值用 `std::array` 承载、
  增加低阶点（全零共享密钥）拒绝与空指针检查；未改域运算的进位与交换逻辑。
- **验证**：`bento_test_x25519` 1160 checks（RFC 7748 §5.2/§6.1 向量、clamp、
  低阶点拒绝、随机往返），本轮 ctest 59/59 通过（见
  `docs/modules/bento-scenarios.md` §21.5）。
- **边界**：只做 ECDH，不做身份认证；共享密钥如何派生意钥由上层握手决定。

### 二、命名空间与状态名约定（2026-10-28 起）

- app 层统一 `go2cpp::bento::app`（fetch/zip/backup/file_browser/capability/session）；
  传输层新代码用 `go2cpp::bento::net`。
- `net/link.hpp`、`net/peer_hub.hpp`、`net/session_control.hpp` 仍在
  `go2cpp::bento` 根，属历史债，本批未迁移（回归面过大）。
- app 层状态名字符串统一 snake_case；kebab-case 只用于 JSON 字段/标签。
  net 既有模块与 `os/file.cpp` 的 kebab 状态名属历史债，本批未动。
- 重试等待统一 `app::detail::SleepBeforeRetry(base, failed_attempt, cap)`。

### 三、批2审查修复范围（2026-10-28）

- 修复 `docs/review/review-batch2-api.md` 的 A-01/A-02 与 C-01～C-05（命名空间、
  状态名、自由函数归属、重复退避、死枚举），详见 `codex.md` 第 27 节与
  `docs/modules/bento-scenarios.md` §21。
- 未修（明确留待后续）：app 层 `LogHook` 注入、`Upload` 远端校验、
  `Probe()` 降级消费者、D-05/D-06 并发缺陷（TOCTOU、`.part` 竞争）。

## 2026-10-28 Bento 文档终审（22 模块 → 5 篇组文档）

### 一、新增与登记

- `docs/modules/bento-transport.md`、`bento-security.md`、`bento-session.md`、
  `bento-app.md`、`bento-foundation.md`（共 1303 行，内容基线 `afc135a`）。
- `docs/INDEX.md` 模块文档表登记 5 行；`docs/modules/bento-modules.md` 增加
  「模块文档」列（21 行）与顶部导航行。

### 二、按代码事实纠正的既有记录（本轮不改代码）

- **X25519 已接线**：`link.cpp` 的 HELLO/HELLO_ACK 携带 32B 临时公钥，
  `X25519ComputeSharedSecret` + `DeriveSessionKeyWithX25519` 派生会话密钥，
  `LinkConfig::enable_x25519` 默认 `true`；失败不降级。`bento-modules.md` 原
  「X25519 集成…后续可选」与本文 §一末条「共享密钥如何派生由上层决定」均已过时。
- **FR-18 远端校验已实现**：`UploadOptions::verify_remote` +
  `VerifyRemote`（`src/bento/app/backup.cpp:1077-1090`，提交 `046d39f`）。
  `docs/modules/bento-scenarios.md` §21.2/§21.4 的「远端校验未做」是更早结论，
  本轮未改该文件（正被其它任务改动）。
- **D-05/D-06 现状**：`file_browser.cpp:203-224` 已改为 `link()` 发布、
  `backup.cpp:901-905` 已改为 `.part.<pid>.<seq>`；本文档终审未做并发复现，
  不宣称缺陷已消除，`bento-app.md` §5 同时列旧记录与新现状。
- **PROTOCOL.md §12 修正 7 处**（帧类型范围、flags 解码行为、CRC 引用、
  ChannelOpen 校验强度、`Flush` 公开性、丢帧范围、ctest 名），详见 `codex.md` §29。

### 三、验证口径

- 示例 10 段全部 `g++ -std=c++17 -Iinclude -fsyntax-only` 通过；相对链接 0 失效；
- 测试数字均为 `grep -c` 静态计数或引用既有运行记录，**本轮未运行 ctest**。

## 2026-10-29 pairing 模块审查（压测 / 随机 / 场景 / 解耦 / 测试复查）

### 一、结论

无 P0；修复 1 个 P1 与 2 个 P2 加固；2 项性能优化；P2 记录 6 项。
交付文档：`example/WarningDocement/audit-pairing.md`、`/tmp/audit_pairing.md`。

### 二、代码改动

- `src/bento/net/pairing.cpp`、`include/go2cpp/bento/net/pairing.hpp`：
  1. **P1**：`Activate` 幂等——修复前每次调用都重置 `deadline_ms`/`next_retransmit_ms`，
     应用在驱动循环里每帧调用一次会让 CONFIRM 永不重传、15s 超时永不触发。
     有损链路（30% 丢包）收敛率 220/300 → 300/300。
  2. **P2**：成功 RESPONSE 必须回显 `key_hash`，否则返回 `kKeyMismatch` 且不迁移。
  3. **P2**：客户端未发 CONFIRM（`m_confirm_sent == false`）时拒绝 ACK。
  4. 性能：默认策略改为进程级静态实例（构造 52.5→21.7ns）；`m_changes` 由
     `std::deque` 改为 `std::array<StateChange,8>`（完整配对 352.9→152.0ns，2.3×）。

### 三、测试改动

- `tests/bento/test_pairing.cpp`：13 场景 8,239 断言 → **20 场景 18,307 断言**；
  新增 Activate 幂等、key_hash/ACK 防御、30% 丢包链路、8 线程 1000 并发、
  随机帧 fuzz、100 万次编解码、枚举/空指针边界、timeout 钳位等 8 个场景。
- `tests/bento/audit_pairing_stress.cpp`：新增独立压测程序（不注册 CTest）。

### 四、验证结果

- `ninja` 全量 44 目标、0 警告；`ctest -R bento -j4` **48/48 通过**
  （`bento_netem` 因无 CAP_NET_ADMIN 按设计 SKIP）。
- 压测（Release，12 vCPU）：编解码 164 Mops/s（6.08ns/帧）；完整配对
  6.58 M pairs/s（152ns/pair，P99 0.13µs）；1000 并发配对 1000/1000；
  20 万轮构造/配对/销毁 RSS 增量 0KB；valgrind 87,030 allocs = 87,030 frees。
- ASan+UBSan：106,552 checks，0 失败、0 报告。
- 覆盖率（`--coverage` + gcov）：pairing.cpp 行 98.56%、分支执行 100%；
  剩余未覆盖行均为不可达防御分支。

### 五、后续状态

- **未修（已记录）**：不可达防御分支（P2-3）、时钟回绕（P2-4）、Closed 状态
  `kAlreadyRegistered` 语义（P2-5）、`kAlreadyPaired` 从未返回（P2-6）、
  `m_outbound` 仍为 deque（P2-7）。
- 未 commit；未改动 `bento` 其他模块、base、构建脚本。

## 2026-10-29 udp_transport 模块审查（压测 / 随机 / 场景 / 解耦 / 测试复查）

### 一、结论

无 P0；修复 2 个 P1、记录 7 个 P2 / 5 个 P3。详见
`example/WarningDocement/audit-udp-transport.md`。

### 二、代码改动

- `src/bento/net/udp_transport.cpp`：`NormalizeTimeout` 钳制到 24h（修复
  `milliseconds::max()` 的 `time_point + duration` 有符号溢出，UBSan 实测
  `chrono.h:225`）；`Read`/`RecvFrom` 在 `timeout > 0` 时改为 recv-first，仅
  `EAGAIN` 才 `poll`；`timeout == 0` 保持 poll-first（空闲轮询不退化）。
- `include/go2cpp/bento/net/udp_transport.hpp`：同步超时上限与 recv-first 说明。
- 未改 base、未改构建脚本。

### 三、测试改动

- `tests/bento/test_udp_transport.cpp`：15 用例 / 158 checks → 22 用例 / 218 checks。
  新增超时钳制、非阻塞就绪、通配 Bind、connected SendTo 其它对端、connected
  Read 静默截断、4 线程并发 SendTo、fd 泄漏 7 个用例；修正 1 个过严断言、收紧
  1 个过宽断言、修掉 IPv6 用例的 SKIP 逃逸。
- `tests/bento/audit_udp_transport_stress.cpp`：新增独立审计程序（不注册 CTest）。

### 四、验证结果

- 42 万包压测（64B 222.9k pps / 1400B 208.5k pps / 64KB 719.8 MiB/s）+ 2000 轮
  随机 + 6 场景 + 4 线程并发：audit `--mode=all` **2080 checks / 0 失败**，
  fd 全程 +0，RSS 4.4 MB。
- 热路径 microbench：`RecvFrom` 1513→824 ns/call（−45.5%）、`Read`
  1231→601 ns（−51.2%）、`RecvFrom(0ms)` 无回退。
- `ctest -R bento` **48/48**（`bento_netem` 无 CAP_NET_ADMIN 按设计 SKIP）。
- ASan+UBSan 0 报告；valgrind 0 errors / 无泄漏；`-Werror` 0 警告。

### 五、后续状态

- 未修（已记录）：默认 rcvbuf 无调优 API（P2-3）、通配 Bind 单地址族（P2-4）、
  `WaitFor` 不查 `revents` / `m_fd` 并发竞争（P2-5）、解析失败统一
  `EHOSTUNREACH`（P2-6）、connected Read 静默截断（P2-7，文档已声明）、
  主机名每包解析 9.5 µs（P2-1）。
- 文档 `docs/modules/bento-foundation.md:16` 的「15 用例」待同步为 22。
- 未 commit。

## 2026-10-30 MCP Socket 工具（socket_list / socket_stats / socket_events / socket_close）

### 一、结论

4 个 socket 工具完成并接入 `mcp_server`；`mcp/` 编译 0 警告；35/35 ctest 通过；
ASan/LSan 0 报告。详细报告见 `example/WarningDocement/audit-mcp-socket.md`。

### 二、代码改动

- 新增 `mcp/include/mcp/tools/socket_registry.hpp`、`mcp/src/tools/socket_registry.cpp`：
  进程内 socket 注册表（`/proc/self/fd` + `SO_TYPE`/`getsockname`/`getpeername` +
  `TCP_INFO`）、白名单 inode 校验（防 fd 复用 ABA）、每 fd 256 条事件环、
  JSON Lines 审计（默认 `log/mcp-socket-audit.log`，fail-closed）。
- 新增 `mcp/include/mcp/tools/socket_tools.hpp`、`mcp/src/tools/socket_tools.cpp`：
  四个 MCP 工具，参数校验与 JSON 输出；审计汇可注入（测试用内存实现）。
- 改 `mcp/src/app/application.cpp`：注册 socket 工具。
- 改 `mcp/CMakeLists.txt`、`mcp/tests/CMakeLists.txt`：源文件与测试目标接线。
- 改 `mcp/README.md`、`mcp/docs/architecture.md`：工具总数 33（22+3+4+4）、
  ctest 21（集成 35）。
- 未改 base、未 commit。

### 三、测试改动

- 新增 `mcp/tests/test_socket_tools.cpp`（8 用例 / 1479 断言）：真实 TCP/UDP/Unix、
  TCP_INFO 收发、事件顺序与截断、close+审计、随机 fuzz（1000 组）、压测（2000 次）。
- 新增 `mcp/tests/test_socket_whitelist.cpp`（8 用例 / 84 断言）：白名单拒绝、
  普通文件 fd、ABA、fail-closed、审计文件 0600、环形溢出、外部关闭、8 线程并发。
- 新增 `mcp/tests/test_socket_e2e.py`（34 断言）：真 `mcp_server`（stdio）。
- 新增 `mcp/tests/socket_test_util.hpp`：真 socket 构造与 fd RAII。

### 四、验证结果

- `.build-mcp-socket-wsl`（Release）：clean 重建 mcp 目标 0 警告；全量 ctest **35/35**
  （61.19s）。
- 压测：2000 次混合调用 14,504 ops/s，P50 11.5µs / P95 199.8µs / P99 234.0µs，0 失败。
- 随机：1000 组 fuzz，接受 317 / 拒绝 683，0 崩溃。
- `.build-mcp-socket-asan`（Debug + ASan/LSan）：两套件 0 报告，exit 0。

### 五、后续状态

- 未接入：业务侧 socket 创建后不会自动 `Register()`，需改 `src/network`（超出本批边界）；
  未登记 socket 只能只读观察。
- 已知限制：事件仅进程内、UDP/Unix 无 TCP_INFO、审计非事务、close 与外部并发
  关闭同一 fd 存在理论 TOCTOU。见审计报告第 7 节。
- 未 commit。

## 2026-10-30 sync::Mutex P1 修复（状态字非原子清理破坏互斥性）

### 一、问题与修复

- **缺陷**：`sync::Mutex` 的超时/取消清理、`Arm()` 失败清理、`DropWakeToken`
  与 `Unlock` 慢路径共 5 处用非原子 `load`+`store` 改写 `m_state`；
  `TryLock`/`Lock` 快路径的 CAS 不持 `m_mutex`，store 用旧快照覆盖刚写入的
  `kLocked`，使两个执行流同时持锁。修复前 3 秒实测 9–34 次
  `unlock of unlocked mutex`、629–1683 次双持。
- **修复**：新增 `Mutex::Impl::UpdateStateAtomic`（CAS 循环，
  `sync.cpp:1084-1106`），把上述 5 处（共 10 个 `store`）全部改为原子 RMW；
  `m_state.store` 计数归零。状态机（正常/饥饿/唤醒令牌）与分支条件不变，
  CAS 仍在原 `m_mutex` 临界区内。
- **顺带**：`waiters_after`/`debug_count` 包进 `#if GO2CPP_DEBUG_RESOURCES`，
  消除 Release 下 2 个 `-Wunused-but-set-variable`。
- 只改 `src/concurrency/sync.cpp`；未改 base 其他；未 commit。

### 二、验证

- 修复后 10 轮 × 3 秒：0 次解锁异常、0 次双持（helper 约 13.7 亿次锁操作）。
- 随机 8×800 × 10 次：0 overlap / 0 unlock_error / clean_after=1。
- `GO2CPP_TEST_FILTER=sync`、`=locks` 通过；`go2cpp_sync_cancel_stress 5000` 通过。
- `.build-bento-wsl` 干净重建：`sync.cpp` 0 警告；全量 0 error（其他文件既有
  24 个警告，不在本任务边界内）。
- 报告：`example/WarningDocement/audit-base-sync.md` §6/§8；详细数据
  `/tmp/sync_p1_fix.md`。

### 三、后续状态

- 未 commit；未跑 TSan/ASan（普通 Release，行为复现判定）。
- Fiber 场景下的 P1 修复未单独压测（复现与回归均为 native 线程）。

## 2026-10-30 scheduler shutdown O(n²) 修复（P1-1）

### 一、结论

修复 `audit-base-scheduler.md` 的 P1-1：draining 阶段每完成一个 G 做两次全表扫描
导致 `shutdown()` O(n²)。改为 O(1) 存活计数判定；10 万 parked G 关停
45.2 s → **3.33 s**，40 万积压 >6 min → **13.16 s**。

### 二、代码改动

- `include/go2cpp/concurrency/scheduler/scheduler.hpp`：
  - `Task::prepare_enqueue()` 增加可选 `live_tasks` 计数指针；
  - `Task` 新增 `m_live_tasks` / `m_live_counted` 与
    `bind_live_counter_locked()` / `release_live_counter_locked()`；
- `src/concurrency/scheduler.cpp`：
  - `Impl` 新增 `std::atomic<std::size_t> live_tasks`；
  - 首次入队成功 +1，四个终态转移点 -1（同一 `transition_mutex` 内）；
  - `finish_draining_locked()` / `shutdown_for()` 等待谓词改为 O(1) 计数，
    归零后仍做一次全表确认兜底；
  - worker 完成路径只在计数归零时回收注册表，draining 期间去掉逐 G 广播；
  - stale 队列节点的回收推迟到计数归零。
- 未改 base 其他模块；未 commit。

### 三、验证结果

- `.build-bento-wsl` 完整 `ninja`：0 warning / 0 error。
- 现有 scheduler 测试套件（scheduler/stress/terminal_reclaim/dynamic_scheduler/smoke）：
  failures=0。
- 随机 shutdown 竞态 5 seeds × 6000 步 = 30,000 步：0 失败。
- shutdown/cancel/wake 三方竞态 20 轮 × 4000 任务：0 失败。
- 扩展性（3 次取最小）：1 万 221 ms、5 万 1,268 ms、10 万 3,325 ms、
  40 万 13,161 ms（均 `workers=0 / alive=0`）。
- 详细报告：`/tmp/scheduler_shutdown_fix.md`（副本
  `/var/tmp/shutdown_fix/scheduler_shutdown_fix.md`）。

### 四、后续状态

- 10 万未达 <2 s：剩余为 10 万个 parked Fiber 的栈释放成本（strace 52.8% 在
  mmap/munmap/mprotect；单 worker 1,831 ms < 4 worker 3,325 ms）；建议后续在
  fiber 模块做栈 arena / 批量串行释放，超出本批边界。
- 未 commit。

## 2026-10-30 Fiber 切换优化（参考 Go gosave/gogo）

### 一、结论

Fiber 一次完整切换往返（resume→Suspend→resume，1e6 次 ping-pong）
**72.9 ns/op → 34.4 ns/op（-52.9%）**；调度器热路径
`resume_from_scheduler` **55.2 → 31.3 ns/op（-43.3%）**；逐次采样
P50 80 → 50 ns、P99 126 → 51 ns。换栈汇编本身只有 6.2–6.7 ns，
优化前约 66 ns 是 Fiber 层簿记（TLS general-dynamic、binder 重复加锁、
errno noinline），不是寄存器保存。

### 二、代码改动

- `src/concurrency/fiber.cpp`（唯一实现改动）：
  - 7 个热路径 `thread_local` 改 `tls_model("initial-exec")`（与
    `channel.cpp` Select 游标同一取舍；dlopen 本库需回退 general-dynamic）；
  - `bind_caller()` 快速路径：父级已绑定且等于本次调用者时只做 2 次
    atomic load；`m_parent`/`m_parent_bound` 改 `std::atomic`（写侧仍持锁）；
  - errno：`t_errno_slot` 每线程缓存 `&errno`（initial-exec），
    `swap_errno` 合并读旧值+写新值，往返 noinline 调用 5 → 3 次；
  - `suspend()` 返回路径删除 4 次冗余原子/TLS 写（resume_locked 跳转前
    已写成相同值），并删除冗余 errno 恢复；
  - growable 支持结果每线程缓存 `t_growable_support_state`（0/1/2），
    `m_stack_growable_active` 在 `initialize_context` 固化；迁移导入路径
    同步补该字段；
  - `Fiber::Suspend` / `SuspendForScheduler` 直接读 TLS，去掉 `Current()`
    的 noinline/PLT 调用。
- `tests/concurrency/bench_fiber_switch_opt.cpp`（新增）：底层
  `JumpContext` 裸往返、resume/park/scheduler ping-pong、双 Fiber 交替、
  栈热/冷分配，含批量分块中位数与逐次采样分位数。
- `tests/concurrency/test_fiber_switch_opt.cpp`（新增）：6 个真实场景
  （errno 隔离、嵌套 Fiber、跨线程迁移、取消+析构挂起、scheduler park、
  4 Fiber×10 万次交替）+ 随机序列，兼容 ASan/TSan 源码插桩。
- `include/go2cpp/concurrency/fiber.hpp`：未改（ABI/语义不变）；未改
  scheduler；未 commit。

### 三、验证结果

- `go2cpp_tests`：`GO2CPP_TEST_FILTER=fiber|fiber_growable|fiber_local|debug_fiber`
  全部 `all checks passed`（growable 含 256 KiB/1 MiB 深递归、迁移增长、
  4 worker 并发）。
- `go2cpp_fiber_select_test` failures=0；`go2cpp_concurrency_combinations`
  PASS 12 场景；`go2cpp_concurrency_fuzz` PASS 1000 轮/1e7 ops；
  `go2cpp_scheduler_smoke` 返回 0。
- 专项随机：4 seeds × 5k/20k ops，failures=0。
- ASan+UBSan（`-fsanitize=address,undefined`，2000 ops）：0 错误。
- TSan（`-fsanitize=thread`，生产配置 `GO2CPP_DEBUG_DUMP=OFF`，1000/5000
  ops，`setarch -R` 规避 ASLR 映射）：**0 条 ThreadSanitizer WARNING**。
- 项目 `run_tsan.sh` 线程级套件：TSan 库构建成功，但 `tsan_smoke` 链接
  失败（`cannot find -lgo2cpp_hook`，该构建目录未产出 hook 库）——未跑通，
  已知环境问题，非本次改动引入。
- 详细报告：`/tmp/fiber_switch_opt2.md`。

### 四、后续状态

- 剩余 34.4 ns 中约 28 ns 是 CAS resume claim、状态原子写/读、TLS
  保存/恢复；继续压缩需要改 record 状态布局或去掉 claim，收益 <5 ns、
  风险高于本批边界，暂不做。
- initial-exec 依赖"库在进程启动时加载"；若未来需要 dlopen 本库，必须
  回退 general-dynamic（注释已写）。
- 未 commit。

## 2026-10-30 FastChannel 数据面通道（子 agent：极简实现）

- 新增 `include/go2cpp/concurrency/fast_channel.hpp`：
  - `FastChannel<T, Cap>`：编译期容量、`std::array` 零堆分配、单锁 MPMC
    （`detail::ChannelSpinLock`），无 Context/超时/取消/select/调试；
  - `FastSpscChannel<T, Cap>`：单生产者单消费者无锁 ring（原子 head/tail，
    acquire/release），阻塞时才用一把 mutex 登记等待者；
  - 阻塞语义复用 Channel P0 的 per-node futex / Scheduler park 原语，
    支持 Fiber↔Fiber / Fiber↔Thread / Thread↔Fiber / Thread↔Thread。
- 新增 `tests/concurrency/test_fast_channel.cpp`（功能/并发/FIFO/close/随机/
  Fiber×Thread 四组合/5 个真实场景）、`bench_fast_channel.cpp`、
  `go_channel_bench.go`（Go 对照）。
- `cmake/Go2CppTests.cmake` 末尾注册 `go2cpp_fast_channel_test`（ctest）与
  `go2cpp_fast_channel_bench`（不注册 ctest）。
- 基准（空载，taskset 2,3，200k×5，median）：SPSC cap1024 20.6ns
  （Go 25.5，0.81×）、cap64 25.6ns（Go 24.8，1.03×，min 17.4ns）；
  FastChannel cap64 118.6ns、cap1024 95.3ns、cap0 814ns。
- 已知限制：FastChannel 的 park 路径未无锁化，单条延迟高于 Go 同 P
  goroutine handoff；cap0 无缓冲每次都要 park；满负载（其他 agent 的
  -j12 构建，load 200+）下测试会超时，空载通过。
- 状态：未 commit。

## 2026-10-30 网络批量 syscall + 零拷贝（子 agent）

- 新增 `Socket::recv_batch`（recvmmsg）/`send_batch`（sendmmsg）/`send_file`（sendfile），
  头文件 `include/go2cpp/network/socket.hpp`，实现 `src/network/socket.cpp`；
  仓库没有独立 UdpSocket/TcpSocket（TCP/UDP 统一由 Socket 承载），批量 API 落在 Socket 上。
- 基准（WSL2 loopback，taskset 2,3，5 次中位数，3 轮复测）：
  UDP 批量收发 **1.36–1.44×**（内核每包 ~3.1 µs 限制，syscall 仅 ~1.3 µs）；
  TCP 小消息 writev 批量 **10.3–13.4×**；sendfile 零拷贝 **1.54–1.63×**（3662 vs 2253 MiB/s）。
- 新增 `tests/network/test_net_batch.cpp`（46,156 断言：参数/超时/取消/关闭/
  sendfile offset+EOF+慢读+对端关闭/Fiber/300 轮随机/5 个场景）与
  `tests/network/bench_net_batch.cpp`；`cmake/Go2CppTests.cmake` 注册
  `go2cpp_net_batch_test`（ctest）与 `bench_net_batch`（不注册 ctest）。
- 验证：现有 network/io/fd_manager 测试全过；新测试 ASan+UBSan 0 错误；普通构建 30 轮稳定。
- 已知限制：UDP 2× 未达；UDP GSO 在本环境 `EINVAL` 未集成；未 commit。
- 详细报告：`/tmp/net_batch.md`。

## 2026-10-30 Reflect 序列化模块（子 agent：网络层序列化）

- 新增 `include/go2cpp/reflect/**`（11 个头文件）与 `src/reflect/**`（4 个实现）：
  `TypeId`/`TypeTraits`/`TypeRegistry`（内置固定 id + 动态/固定 id 注册）、
  `Serializer<T>` 钩子与通用 `Serialize/Deserialize`、字段反射
  （`FieldDescriptor` + `GO2CPP_REFLECT_FIELD` + `FindField` 按名访问）、
  varint + 字段名去重 + 可选 RLE、JSON-like 文本与二进制紧凑两种格式、
  `ReflectCodec` 与 `ReflectFrameCodec`（Bento 通用帧对接）。
- 构建：`cmake/Go2CppReflect.cmake` 定义 `go2cpp_reflect`（`-Wall -Wextra
  -Wpedantic -Werror`）；`CMakeLists.txt` 末尾接入。未改
  `include/go2cpp/bento/reflect/**`，旧 Bento 反射保持兼容。
- 测试：`tests/reflect/test_reflect_codec.cpp`（16 用例，10969 checks）在
  `.build-bento-wsl` 下全部 PASS（`ctest -R reflect_codec`）；含随机往返、
  3000 轮 fuzz、RLE 压缩炸弹/截断、帧粘包/半包/CRC、6 个真实场景。
- 压测（Release -O3，单线程，约 60B 消息）：二进制编+解 2,165,648 ops/s
  （0.462 µs/op），文本 747,389 ops/s（1.338 µs/op）。
- ASan+UBSan（`-fsanitize=address,undefined`，leak 检测开启）16/16 PASS，
  0 报告；临时构建目录已清理。
- 详细报告：`/tmp/reflect_codec.md`。
- 已知限制：文本模式不支持「仅特化 Serializer、无 FieldList」的类型；
  二进制未知字段直接拒绝（暂无 schema 兼容跳过）；字段宏要求结构体可默认构造。
- 状态：未 commit。

---

## 2026-10-31 网络协议正确性 + 性能统一基准（deepseek）

- **改动**：新增 `tests/bento/bench_protocols.cpp`（六协议 + OmniNetServer M1 的
  真实场景正确性 + 吞吐/延迟基准 + 写路径三档对比）；`cmake/Go2CppBento.cmake`
  注册 `bento_bench_protocols` 目标（ctest 只跑 `--correctness`，性能手工触发）；
  新增 `docs/reports/2026-10-31-http-perf.md`。**未改动任何协议实现。**
- **测试**：`--correctness` 37 场景 / 237 断言 / 0 失败（ctest `bento_bench_protocols`
  6.04s 通过）；性能数据见报告。
- **关键发现**：① 服务端 accept socket 未设 `TCP_NODELAY`，WS/SOCKS5 小包往返会被
  Nagle+延迟 ACK 压成 ~40ms（基准用客户端 `TCP_QUICKACK` 绕过，产品侧建议 accept
  后设 NODELAY）；② CoAP 客户端默认随机种子固定，多实例会用相同 (MID,Token) 被
  去重缓存误判；③ 小包 bufio 提升 21~33 倍，≥4KB 大包 bufio 反而慢 19%（应直写）。
- **状态**：正确性全绿；性能数据已归档；未见协议实现缺陷，只有上述可优化点。
- **复现**：`taskset -c 2,3 ./.build-bento-wsl/bento_bench_protocols --correctness|--perf`


---

## 2026-10-31 example/app 双版本应用示例（子 agent：example 双版本）

- 新增 `example/app/**`：web_server（simple/full）与 chat_server（simple/full）
  四个可运行服务示例；每版 README 含功能、运行方式与 curl/客户端测试步骤；
  `example/app/README.md` 含 simple vs full 对照表与学习路径。
- 构建：`CMakeLists.txt` 在 `Go2CppBento.cmake` 之后执行
  `add_subdirectory(example/app)`（仅 `GO2CPP_BUILD_EXAMPLES=ON` 时）；
  `example/app/CMakeLists.txt` 按 Bento 目标存在性注册四个 target。服务类示例
  监听端口并等待信号，因此不注册 ctest。
- 测试（`.build-bento-wsl`，Release）：
  - web_simple：curl 200/400/404/405、`/health`、SIGTERM 优雅关闭 exit=0；
  - web_full：静态文件/路由参数/JSON API/异常 500/目录穿越 403/OPTIONS 204/
    SSE welcome+tick+demo+heartbeat/200 并发请求/SIGTERM exit=0；
  - chat_simple：双客户端广播、WHO、退出码 0；
  - chat_full：9 peer 覆盖房间隔离、私聊、在线列表、历史回放、重复昵称、
    未加入、未知命令、超长消息、心跳超时、token 重连、并发发送、持久化、
    优雅关闭；测试断言 26 项全部 PASS。
- 依赖：`bento::HttpServer`、`bento::LinkServer`/`PeerHub`、`Scheduler`、`Channel`；
  未改库代码。
- 已知限制：full 版总行数（web 1739 / chat 1791，含项目规范注释）超出任务
  参考的 400–600；UDP 明文、token 仅存内存、无身份认证（均为示例边界，
  见各 README）。
- 状态：未 commit。

## 2026-10-31 缓存淘汰算法专题（子 agent：cache-topic）

- 新增 include/go2cpp/cache/**：统一 Cache<K,V> 接口 + 7 种策略
  （LRU / LRU-K(2) / 2Q / W-TinyLFU / GDSF / ARC / CLOCK）+ 工厂、分片、
  线程安全包装 + 5 场景负载生成器。
- 新增 tests/cache/**：12 个测试 TU（单元/随机/5 场景/压测）+ 基准
  go2cpp_cache_bench。
- 新增 docs/research/cache-algorithms.md（1039 行）：10 种算法综述、
  防扫描与 GDSF 权重专节、多场景选型表、实测数据。
- 新增 cmake/Go2CppCache.cmake，顶层 CMakeLists.txt 末尾 include；
  header-only INTERFACE target，未改现有模块。
- 构建（.build-bento-wsl，Release，GCC 13.3）：go2cpp_cache_test、
  go2cpp_cache_bench 均 0 警告。
- 测试：ctest -R cache 2/2 通过；go2cpp_cache_test 全绿（0.64 s）；
  go2cpp_cache_bench --full 35 组数据（命中率/吞吐/峰值内存/扫描恢复）。
- 关键实测：weighted 场景 GDSF 0.8158 vs LRU 0.5579；CDN GDSF 0.7483 vs
  LRU 0.4771；scan 场景 LRU-K 0.5197 / ARC 0.5104 / LRU 0.3674；
  TinyLFU 热点 0.7821。
- 已知限制：GDSF 只设字节预算时条目数和元数据内存会放大，文档给出
  条目上限建议；LIRS / CLOCK-Pro / S3-FIFO 仅综述未实现。
- 状态：未 commit。

---

## 2026-10-31 应用层框架（流表/资源传输/KV 流，子 agent：应用层框架）

- 新增 `include/go2cpp/app/**`（10 个头）与 `src/app/**`（10 个实现）：9B 帧头
  （DATA/HEADERS/PRIORITY/RST/SETTINGS/PING/GOAWAY/WINDOW_UPDATE/RESOURCE_INDEX）、
  流状态机、连接/流两级窗口、依赖树 + WFQ 调度、资源分块（SHA-256/CRC-32/断点
  续传/索引）、KV 流（小表单帧/大表流式/每键一流）、Bento 20B 帧桥、Bento Link
  STREAM 传输适配、HttpServer 批式隧道。
- 新增 `tests/app/**`（8 个可执行、62 个场景）：流表 12、资源 10、KV 10、连接 11、
  帧 7、负载 3、Bento 4、HTTP 5；含 1000 并发流与 1 GiB 资源。
- CMake：`cmake/Go2CppApp.cmake`，`-DGO2CPP_WITH_APP=ON` 打开（默认 OFF）；
  测试注册 8 个 ctest（`app_*`）。
- 实测（Release，`.build-bento-wsl`）：4 MiB 内存资源 21 ms（190 MiB/s）；
  1 GiB 资源 3992 ms（256 MiB/s）+ 元数据扫描 3348 ms；1000 并发 KV 流 14 ms
  （客户端 1002 帧）；1 MiB over Bento Link 7.7 MiB/s（每 2 ms 驱动 RunOnce 的
  测试频率限制）；256 KiB over HttpServer 6 次请求。
- 文档：`docs/modules/app-framework.md`（设计/API/用法/与 HTTP/2 对比/限制）。
- 边界：未改任何现有协议实现；未 commit。
- 已知限制：无 HPACK、HEADERS 单帧（超限自动分区到 DATA）、HTTP 隧道半双工、
  跨进程续传只校验续传段 SHA-256、事件队列有界 65536。
