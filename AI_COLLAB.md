# AI 协作记录（跨批次规范与第三方来源）

## 2026-11-01 质量标准（性能/工程/正确性 三条合一，压缩版）

> 三份细则：`~/.dsh/AGENTS.md`（性能，全局）、`docs/engineering-standards.md`（工程）、
> `docs/correctness-standards.md`（正确性）。本段是**压缩版 + 我的理解**，供每轮 agent 速读。

### 一、三条标准（一张表）

| 维度 | 门槛 | 证据 |
|---|---|---|
| **性能** | 接近汇编级/物理极限 | objdump 指令/分配 + 「离下限几倍」+ 前/后数据（taskset+load）+ ≥1e7 压测 |
| **工程** | 对标工业级 + 简单 | `docs/reports/vs-<module>.md` 对比矩阵 + ≤2000 行/≤15 接口 |
| **正确性** | 逐字节一致 + 全边界 | 对拍 + ≥1e7 压测 + ASan/UBSan + 随机 ≥1000 轮 + 真实场景 ≥5 + 边界 7 类 |

### 二、我的理解（为什么 / 怎么落地 / 常见坑）

**1. 性能**

- 「汇编级」不是指手写汇编，是指**每条指令都有价值**——无冗余拷贝、无虚调用、无临时对象、无未内联热函数。
- 「物理下限」必须**量化**：内存带宽（GB/s）、缓存延迟（ns）、原子（ns）、syscall（ns）。不量化就是空话。
- **常见坑**：把 0 warning 当完成；用「差不多」代替数字；只做冒烟测试；优化了非瓶颈。

**2. 工程**

- 「简单」是**最高级的工程**——工业级不是代码多，而是用最少代码解决最多问题。
- **常见坑**：过度抽象（3 层虚接口只用 1 个实现）；无必要 pimpl；模板元编程炫技；为「将来可能用」预留。
- 简单 → 易读 → 易验证 → 正确性和性能都受益。这是**杠杆**。

**3. 正确性**

- **对拍是金标准**（逐字节一致 > 感觉对）；**压测是银标准**（1e7 > 100）；**ASan 是保险**（内存错误潜伏期长）。
- **常见坑**：只测 happy path；跳过 ASan；「概率性通过」当正确；改了语义却没发现（没对拍）。

**4. 三者关系**

```
工程（简单）→ 易验证 → 正确性有保障
正确性（对拍）→ 性能优化不改变语义
性能（量化）→ 知道「还差几倍」→ 值得不值得继续优化
```

### 三、当前基线（压缩，2026-11-01）

| 模块 | 数据 | 离下限 |
|---|---|---|
| Fiber 切换 | 23.5ns | 1.12×（21ns） |
| Channel 1P1C | 10.73ns | 2.1×（~5ns） |
| 调度器任务 | 160ns | 1.19×（Go 135） |
| CRC32 | 21.37 GB/s | 1.4×（~30 GB/s） |
| Bento Encode | 99.7ns/帧 | 2×（~50ns） |
| 缓存 LRU | 79.62 Mops/s | 1.9×（~150） |
| SFU 转发 | 16.5ns/帧 | 待估（~10ns） |

**每次提交前对照此表更新「离下限几倍」。**

---


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
交付文档：`docs/audit/audit-pairing.md`、`/tmp/audit_pairing.md`。

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
`docs/audit/audit-udp-transport.md`。

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
ASan/LSan 0 报告。详细报告见 `docs/audit/audit-mcp-socket.md`。

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
- 报告：`docs/audit/audit-base-sync.md` §6/§8；详细数据
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

---

## 2026-10-31 网络层 4 项待办清理（B-01/B-02/B-03/B-06，子 agent：清网络层）

- B-01（P0）SOCKS5 建连 50ms：根因不是 Nagle，而是 `Socks5Server::Poll()` 的 pollfd
  掩码在 kGreeting/kRequest 只监听 POLLIN，握手响应待写时没有 POLLOUT，只能等 50ms
  轮询超时才刷出（strace 实测两次 send 间隔 52ms）。修复：所有阶段统一补待写方向
  POLLOUT；上游 raw connect socket 补 TCP_NODELAY。复测（taskset 2,3，loadavg 0.5~2.4）：
  p50 50.6ms→0.30~0.33ms，p99 52.6ms→0.64~0.79ms（4 轮），吞吐 42→2660~2933 ops/s。
- B-02（P1）tcp_proxy accept 补 TCP_NODELAY（core bento 不链接 network，用同语义
  setsockopt；Stats 加 nodelay_applied/nodelay_failed）。debug_server 经核实监听
  AF_UNIX，TCP_NODELAY 不适用，改为按 SO_DOMAIN 仅 TCP 域设置，STATUS 暴露
  accept_nodelay。socks5 upstream_fd 同类缺口一并补。
- B-03（P1）CoAP 默认种子：`random_seed=0` 表示每客户端自动生成
  （random_device ^ steady_clock ^ pid ^ 原子序号）；显式非 0 仍可复现。
  新增「双默认客户端请求同一服务端不串响应」测试。
- B-06（P1）HttpServer 示范接入 bufio：`use_bufio` 默认关，响应拆头+体两段
  `FlushBatch()` 一次 writev，超容量直写旁路（B-04）；`GetWriteStats()` 计数。
  实测单连接 3000 请求：默认 8439 ops/s，bufio 8060（0.955×），默认关是正确决策。
- 测试：bento_test_socks5 204 / tcp_proxy 46 / coap 38587 / http_server 109 全过；
  `--correctness` 37 场景 237 断言、`--nodelay` 5 场景 41 断言全过；
  全量 ctest 146 Passed / 5 Failed（并发锁、状态矩阵、tools 缺产物，均与本次无关）/
  75 Not Run（其他模块未构建）。
- 文件：src/bento/net/{socks5,tcp_proxy,coap,http_server}.cpp、src/debug/debug_server.cpp
  + 对应头文件 + 4 个测试；报告 /tmp/net_todo_clear.md。未 commit。

---

## 2026-10-31 P2P 公网穿透（子 agent：P2P 公网穿透）

- **新增** `include/go2cpp/p2p/**` + `src/p2p/**`：STUN（RFC 5389 子集）、
  RFC 3489 四类 NAT 检测、本地 STUN echo 服务器（双 IP/4 socket/CHANGE-REQUEST）、
  ICE 简化（host/srflx/relay 候选 + 优先级 + 并行连通性检查）、UDP 打洞、
  简化 TURN（Allocation/Send/Data + 服务器数据面 + 统计）、TCP 同时打开、
  `P2PChannel`（打洞/降级/keepalive/RTT/序列号缺口统计）。
- **新增** `tests/p2p/**`：4 个测试可执行 + 统一入口 `run_p2p_tests.sh` +
  基准 `bench_p2p`；24 个场景，含 4 类模拟 NAT、跨进程 fork+exec 打洞、
  2000 包压测、500 条随机数据、500 轮解析 fuzz。
- **新增** `cmake/Go2CppP2P.cmake`；顶层 `CMakeLists.txt` 在
  `Go2CppTests.cmake` 之后、`Go2CppInstall.cmake` 之前 include 一行。
  `GO2CPP_WITH_P2P` / `GO2CPP_P2P_TESTS` 默认 ON，零外部依赖。
- **文档** `docs/modules/p2p-nat-traversal.md`（原理/设计/部署/信令接口/限制）。
- **测试**（`.build-bento-wsl`，Release）：
  - `p2p_test_stun`：约 2030 断言 0 失败（四类 NAT 判定全对）；1000 次 Binding
    约 9919 req/s，P50 0.088ms；
  - `p2p_test_p2p`：94 断言 0 失败；FullCone/PortRestricted 打洞成功，
    Symmetric 降级中继成功；2000 包 100% 到达约 109MB/s；
  - `p2p_test_tcp_punch`：TCP 同时打开成功；
  - `p2p_test_p2p_interprocess`：两进程 host 直连与 srflx 打洞均 PASS；
  - `ctest -R p2p`：4/4 通过（6.9s）。
- **已知限制**：对称 NAT 不可打洞（只能中继）；STUN/TURN 无鉴权；
  ICE 无 nomination/角色；数据面无重传/拥塞控制；TCP 打洞平台相关；
  IPv6 打洞未覆盖（端点/STUN 支持 IPv6，MockNat 与 TCP 打洞仅 IPv4）。
- **状态**：未 commit；未改任何既有模块实现。

---

## 2026-10-31 文档全面对齐（子 agent：文档对齐）

- **盘点**：`.build-bento-wsl`（profile `bento+mcp+runtime`）`ctest -N` 实测 **230**；`docs/testing.md`
  仍登记 85。另用同一份源码重新 configure 两个配置实测：`bento+mcp`（`GO2CPP_BUILD_TESTS=OFF`）**80**、
  `mcp+osmon` **20**（旧文档写 85/35）。差异原因：`MCP_BUILD_TESTS` 未声明为 CMake option，
  未显式传参时 21 个 MCP 用例不注册；`GO2CPP_P2P_TESTS` 默认 ON 且不依赖 `GO2CPP_BUILD_TESTS`。
- **docs/testing.md**：现状索引与「构建配置与 ctest 数量」表改为 230/80/20，新增三份逐项测试地图
  （230 项主构建、80、20），补 p2p/cache/app/example 的分类构成与注册条件说明。
- **docs/INDEX.md**：模块表补 `modules/app-framework.md`、`modules/p2p-nat-traversal.md`、
  `modules/cache.md`、`modules/bento-modules.md`、`modules/concurrency-usage.md`；调研节补
  `research/cache-algorithms.md`；顶部统计改为 73 主题 + 19 状态机 + 24 review + 8 research。
- **docs/modules.md**：第 1 节补 p2p/app/cache 三行与 target，修正 `GO2CPP_MODULE_TARGETS` 计数
  （24 常驻 + 条件 hook + 3 新模块），测试布局补 `tests/{p2p,app,cache}` 与 `example/app`。
- **docs/modules/cache.md**：新建（7 策略接口/论文差异/复杂度/测试/实测摘要/限制/边界）。
- **README**：可选能力、示例清单、模块 target、目录结构、CMake 选项（`GO2CPP_WITH_P2P`/`GO2CPP_P2P_TESTS`/
  `GO2CPP_WITH_APP`/`GO2CPP_APP_TESTS`/`MCP_BUILD_TESTS`）、测试表与主构建命令全部对齐。
- **验证**：`ctest -R docs_consistency` 通过（28 断言 0 失败，profile 强校验生效）；文档相对链接扫描
  428 条 0 失效；`.build-bento-wsl` 全量实测 149 passed / 7 failed / 74 not run（原因已写入 testing.md）；
  `mcp+osmon` 独立构建目录全量 20/20 passed。
- **状态**：未改任何代码；未 commit。

## 2026-11-01 网络/应用优化 批 2（每包/每帧固定税）

- **改动**
  - P2P（`src/p2p/p2p_channel.cpp`）：连接状态改为不可变 `ConnectionSnapshot` 原子指针；
    统计/时间戳/keepalive 标志全部原子化；per-packet 0 mutex；数据帧/keepalive/ACK 复用
    `m_tx_frame_scratch`/`m_rx_frame_scratch`（删除每包 `MakeFrame` 分配）；relay 指针原子化，
    Close 不再销毁 relay 对象（消除 Close/Send 并发 UAF）；每轮只取一次时钟。
  - PeerHub（`src/bento/net/peer_hub.cpp` + hpp）：`SendControl` 改用
    `m_tx_control_scratch` + `BuildControlFrameInto`（控制帧每帧 0 堆分配）；`Tick()` 内
    `BeginTxBatch/FlushTxBatch` 把 ACK/心跳/MAX_DATA/重传帧合并为 `sendmmsg`（每批 64，
    部分成功逐帧回退），统计在 flush 合并。
  - Resource（`src/app/resource.cpp` + hpp）：新增 `ChunkSource::View` 零拷贝视图
    （`MemoryChunkSource` 实现）；整块一帧或视图可用时逐帧直读 `Frame::payload`（单拷贝，
    无中间块缓冲）；文件/生成式分帧源保留整块缓冲；去掉每块 `assign(bytes,0)` 清零。
  - 新增基准：`tests/p2p/bench_p2p_fixed_tax.cpp`（1e7 包 UDP）、`tests/p2p/bench_p2p_memory.cpp`
    （1e7 包内存微基准），`cmake/Go2CppP2P.cmake` 只构建不注册 ctest。
- **关键文件**：`src/p2p/p2p_channel.cpp`、`src/bento/net/peer_hub.cpp`、
  `include/go2cpp/bento/net/peer_hub.hpp`、`src/app/resource.cpp`、
  `include/go2cpp/app/resource.hpp`、`tests/p2p/bench_p2p_fixed_tax.cpp`、
  `tests/p2p/bench_p2p_memory.cpp`、`cmake/Go2CppP2P.cmake`。
- **测试**
  - `p2p_test_p2p` 94、`p2p_test_stun` 2052、`p2p_test_tcp_punch` 1、`p2p_test_p2p_interprocess` 4；
  - `bento_test_peer_hub` 502、`bento_test_peer_hub_secure` 825；
  - `app_test_frame` 21100、`app_test_stream_table` 335、`app_test_resource` 174、`app_test_kv` 98、
    `app_test_connection` 180、`app_test_load` 2328（1 GiB SHA-256 校验通过）；
  - 压测：P2P 内存回环 1e7 包 222.7 → 134.1 ns/包（-40%）；UDP 1e7 包 215.7k pps、
    丢包 0.017%、零损坏；PeerHub `sendto` -5.6%、48 次 `sendmmsg`；1 GiB 资源 +14.8%。
- **状态**：完成，未提交（多 agent 共享工作树）。报告：`/tmp/batch2_opt.md`。

## 2026-10-31 · app 层汇编级审查与优化（流表/帧/资源/KV）

- **改动内容**
  - `StreamTable`：`PickNext` 从「每帧两遍全表遍历 + 父链 `map::find`」改为
    **就绪最小堆（按 `(finish,id)`）+ 待补产列表 `m_idle`**；`Stream` 增加
    `ready_slot/producer_slot/idle_slot` 三个 O(1) 索引；`ConsumePending`/`Requeue`
    成为发送完成/回滚的正式 API（清 pending 但保留 payload 容量）。
  - `LiveStreamCount` O(N)→O(1) 计数器；`PruneClosedStreams` 每 Step O(1) 早退 +
    关闭顺序 FIFO 懒删除（不再每轮全表扫描 + vector + sort）。
  - `Frame`：新增借用视图 `view/view_bytes` 与 `Size()/Bytes()`；新增
    `DecodeFrameView` + `FrameDecoder::NextView`（零拷贝）；`Connection::Step` 改用
    `NextView` 并以 `m_in_handle` 禁止事件回调重入解码器；`WriteFrame` 复用
    `m_write_buffer`（稳态 0 分配）。
  - 资源/KV：`ResourceReceiver::OnData` 整块直通（省一次整块 memcpy，新增
    `CommitChunk`）；`KvTable::ToBinaryInto` 让 HEADERS 复用 payload 容量。
- **关键文件**：`include/go2cpp/app/{stream_table,frame,connection,kv,resource}.hpp`、
  `src/app/{stream_table,frame,connection,kv,kv_stream,resource}.cpp`、
  `tests/app/app_test_stream_table.cpp`（适配 `ConsumePending`）。
- **测试**：8 个 app 可执行文件全过（63 场景；frame 21100 / stream_table 335 / kv 98 /
  connection 180 / resource 174 / bento 25 / http 29 / load 2328 checks）；ASan 全过。
- **数据**：`PickNext` 256 流 2553 → 124 ns/帧（20.6x）；端到端 KV 512 流
  7.7 → 2.5–3.1 µs/流；1e8 帧压测 124 ns/帧 / 12.4 s / 8.1 Mfps / RSS 4 MiB；
  1 GiB 资源（SHA-256 校验）268 MiB/s 不变（SHA 主导）；资源生产 +6%。
- **状态**：完成，未提交（多 agent 共享工作树）。报告：`/tmp/asm_app.md`。

## 2026-11-01 · relay 通用组件：Hub（REQ-003）+ Lease/Heartbeat（REQ-005）

- **改动内容**
  - 新增 `go2cpp::relay::Hub`：key → 多连接注册表，单锁 + 快照；`CloseKey`
    锁内摘表、锁外逐条 `Close()`（回调里可重入 Hub）；重复 Register/Unregister/
    CloseKey 均幂等；不内置配额，不关闭未显式关闭的连接（析构不关）。
    连接契约复用 `go2cpp::streams::Closer`（= Go io.Closer），类内别名
    `Hub::Stream`；与 RelayPipe 的 `relay::Stream` 无重定义，且可隐式转换注册。
  - 新增 `go2cpp::relay::Lease`：构造即布防，`steady_clock` +
    `wait_until` + generation 续约；`on_expire` 至多一次；到期后 Renew 无效；
    Stop/析构幂等；回调内可 Stop（自线程 detach）。
  - 新增 `go2cpp::relay::Heartbeat`：ticker 语义；send false/异常 → on_dead
    一次；Context 取消静默停止；取消回调只持 weak_ptr；Stop 幂等、可重启。
  - 构建：`cmake/Go2CppRelay.cmake` 改为聚合入口（只 include），目标与测试定义在
    `cmake/Go2CppRelayHubLease.cmake`；与 RelayPipe 的 `Go2CppRelayPipe.cmake`
    （目标 `go2cpp_relay_pipe`）分开，避免并行 agent 互相覆盖。
- **关键文件**：`include/go2cpp/relay/{hub,lease}.hpp`、`src/relay/{hub,lease}.cpp`、
  `tests/relay/{test_hub,test_lease,test_lease_90s,test_lease_clockjump,test_heartbeat,
  test_relay_scenarios,bench_relay}.cpp`、`tests/relay/{relay_test_util.hpp,fake_clock_jump.cpp}`、
  `cmake/Go2CppRelay.cmake`、`cmake/Go2CppRelayHubLease.cmake`、`docs/modules/hub-lease.md`。
- **测试**（`.build-bento-wsl`，Release；ctest 标签 relay）
  - `relay_hub` PASS（含 1e5 次并发 Register/Lookup/Unregister、CloseKey 竞态、2 万步随机对拍）；
  - `relay_lease` PASS（到期一次/续约重置/Stop/析构/0 超时/并发续约/300 租约/随机）；
  - `relay_heartbeat` PASS；`relay_scenarios` PASS（7 个真实场景）；
  - `relay_lease_clockjump` PASS（LD_PRELOAD 墙钟 ±1h，租约仍按单调时钟 800/700ms 到期）；
  - `relay_hub_tsan` PASS（TSan 无报告，`setarch --addr-no-randomize` 规避 WSL 启动问题）；
  - `relay_lease_90s` PASS（`elapsed=90000 ms`，`fired=1`）。
  - 压测：Hub 单线程 95.5 ns/迭代（10.47 Mops/s）；8 线程只读 Lookup 262 ns/次；
    CloseKey 72.1 ns/连接；Lease Renew 80.5 ns/次；50ms 租约迟到 P99=10ms；
    10ms 心跳抖动 P99=0ms（load average ≈3.8，数字偏保守）。
- **状态**：完成，未提交（多 agent 共享工作树）。报告：`/tmp/hub_lease.md`。
  已知边界：未接入 install(EXPORT)；每 Lease/Heartbeat 一个线程；Hub 单锁未分片。

## 2026-11-01 WebRTC 媒体端到端（M0+M1）

- **改动内容**
  - 新增 `go2cpp::media`：X11 XShm 采集 → 编码（raw-bgra / raw-i420 / 自研 16x16 块差分
    `delta-i420`（无损）/ H.264 探测）→ Bento `VideoChannel` 1200B 分片 → UDP 回环 →
    重组 → 解码 → SDL2/无头渲染；延迟以 32B `MediaHeader` 的 `capture_ns` 为时基，
    输出 p50/p99/p999。
  - 复用**未修改**的 `bento::VideoChannel` 分片/重组；新增 `MediaHeader`
    （magic `G2MF` + codec + w/h + frame_id + payload_len + capture_ns）。
  - 差分编码：16x16 宏块脏块 bitmap + 块内 RLE，I420 域内逐字节无损；静止帧收敛到
    36B/帧；每 N 帧强制关键帧，丢帧后自愈。
  - 构建：新增 `cmake/Go2CppMedia.cmake`（`-DGO2CPP_WITH_MEDIA=ON`，默认 OFF），
    顶层 `CMakeLists.txt` 追加一行 include；media 只编译 Bento 的
    protocol/relation/congestion/bandwidth/fec/channel 六个 TU
    （`go2cpp_media_bento_core`），不链接 link/peer_hub 等无关目标——既缩小依赖面，
    也让并行开发期间媒体模块可独立构建。
- **关键文件**：`include/go2cpp/media/*.hpp`（8）、`src/media/*.cpp`（11 +
  `codec_internal.hpp`）、`tests/media/*.cpp`（7 + `media_test_util.hpp`）、
  `cmake/Go2CppMedia.cmake`、`docs/modules/webrtc-media.md`。
- **测试**（`.build-bento-wsl`，Release；`ctest -R media_`）
  - `media_frame` / `media_codec` / `media_capture` / `media_transport` /
    `media_render` / `media_e2e` / `media_h264` 全 PASS（7/7，2.1s，1102 断言）。
  - 端到端 8 场景逐帧校验：static 100 帧 p50=0.24ms / p99=0.36ms / 2038fps；
    patch 100 帧 p99=0.98ms；X11 真实采集 30 帧 p99=1.00ms；
    丢 1 分片 → 该帧丢弃 → 强制关键帧后逐字节一致。
  - 差分编码：静止 128x96 关键帧 18512B → 差分帧 36B（514x）；噪声压缩比 1.004。
  - H.264：`h264_nvenc` / `libx264` 各 30 帧，PSNR 37.09 / 37.08 dB。
- **环境限制**（实测，已写进模块文档）：`XGetImage` 抓 WSLg root window 必 BadMatch
  → 只能走 XShm；`net.core.rmem_max=208KB` → 单帧突发 >300KB 丢包（默认 320x240）；
  无 libavcodec/x264/vpx 开发头文件 → H.264 只能子进程或探测。
- **状态**：完成 M0+M1，未提交（多 agent 共享工作树）。报告：`/tmp/webrtc_media.md`。
  已知边界：无 DTLS/SRTP、ICE、GCC/码率自适应、jitter buffer；帧格式是 Bento 自研，
  不兼容 RTP；1080p 受 rmem_max 与单帧分片上限限制。

## 2026-11-01 WebRTC 弱网收尾（抖动缓冲 + 降级策略）

- **改动内容**
  - 新增 `go2cpp::media::JitterBuffer`（`jitter_buffer.hpp/.cpp`）：包级环形缓冲
    （`seq & (capacity-1)` 定位，Push/Pop 均摊 O(1)、热路径零堆分配），RFC 3550
    抖动 EWMA（`J += (|D|-J)/16`）+ 目标深度 `clamp(base + 2J, 10ms, 400ms)`，
    空洞超时跳过（一次 Pop 可连续跳过整段丢包）、迟到帧丢弃/8 槽补发队列、
    断流重锚与 32 位序号回绕。
  - 新增 `go2cpp::media::DegradationController` / `ReconnectSupervisor` /
    `Downscale2xBgra`（`degradation.hpp/.cpp`）：四档阶梯（320x240@30 →
    320x240@15 → 160x120@15 → 160x120@10）、降级 200ms 快 / 升级 3s 慢的滞回、
    中等丢包冷却 1s、与 `bento::BandwidthEstimator` 的 `FromBbr` 联动、
    档内连续码率 `adaptive_bitrate_bps`、帧率相位控制、指数退避
    （100→5000ms，±20% 确定性抖动）+ 重连后关键帧重建、2:1 盒式下采样。
  - 构建：`cmake/Go2CppMedia.cmake` 追加 2 个源文件与 2 个测试目标
    （`media_test_jitter_buffer` / `media_test_degradation`），测试总数 9 → 11。
    **未修改任何既有源文件**。
- **关键文件**：`include/go2cpp/media/jitter_buffer.hpp`、`degradation.hpp`、
  `src/media/jitter_buffer.cpp`、`degradation.cpp`、
  `tests/media/test_jitter_buffer.cpp`、`test_degradation.cpp`、
  `cmake/Go2CppMedia.cmake`、`docs/modules/jitter-degradation.md`。
- **测试**（`.build-bento-wsl`，Release，`taskset -c 2`）
  - `ctest -R media_`：**11/11 PASS**（2.16s）。新增两个：jitter 17,269 断言 /
    0.18s、degradation 184 断言 / 0.00s。
  - JitterBuffer：顺序 2000 帧、块内乱序 2048 帧（reordered=1718）100% 按序、
    重复 ×3（duplicates=1000）、5% 丢包（lost=99/100）、连丢 10 一次跳过、
    迟到两种策略、断流重锚、32 位回绕 600 帧 0 误判、
    **±50ms 抖动 1e5 帧 → 100.000% 交付 / 0 违序 / target 90.53ms / held p99 77ms**。
  - 压测 1e6 帧：**Push 21.66 ns、Pop 3.13 ns**（6 次中位数，load 9.87）；
    低负载复测 18.7 / 2.7 ns。内存常量 798,720 B，1e6 帧堆增长 1,216 B。
  - 降级：阶梯 3 级降、丢包 6%/15% 触发、恢复 3 次升级、帧率 30fps 输入
    → 300/150/99、重连 8 次退避、BBR 真实估计器三级降、滞回 0 次震荡、
    下采样 320x240→160x120 逐像素 0 mismatch。
- **性能证据**：`Push` 189 指令 / 4 call（UpdateJitter、memcpy、ResetTo、
  PushLate）/ 0 lock / 0 分配；`PopInternal` 142 指令 / **0 call**；
  `ShouldDeliverFrame` 37 指令 / 0 call；重定位表证实 `operator new[]` 只在
  构造函数出现。离下限：Push **1.44×**（对实测 memcpy 15.00 ns）、Pop **2.1×**。
  强制内联 `UpdateJitter` 的实验为负结果（21.17 vs 19.15 ns），已回退。
- **环境限制**：WSL 内 DNS 返回回环、`web_fetch` 对 raw.githubusercontent 报
  「解析到内网」，开源检索只到 web_search 层；libwebrtc/pion/GStreamer 三者的
  许可证或依赖均不适合移植，取舍已写进文档 §2.3。
- **状态**：完成，未提交（多 agent 共享工作树）。报告：`/tmp/jitter_degradation.md`。
  已知边界：未与真实 UDP `tc netem` 联合压测；未接进 `pipeline.cpp`（按任务边界
  只新增不改现有）；单线程；补发队列固定 8 槽；末包丢包不可检测。


## 2026-11-01 · app 层剩余优化（流表 ready 堆/帧零视图/KV Parse/SHA-NI/锁）

- **改动内容**
  - `StreamTable`：`PickNext` 堆顶命中时不再 `RemoveReady`+`PushReady`（旧实现每帧 2 次
    O(log n) sift）；`ConsumePending` 清 pending 后**留在堆里**（补产不改变 finish，堆序仍有效），
    只有无后续帧才出堆；`ChargeFinish` 权重为 2 的幂时用查表左移替代 64 位 `idiv`；
    `Stream` 新增 `Stream* parent` 指针缓存，`HasReadyAncestor`/`IsAncestor` 不再每跳
    `std::map::find`（`Erase`/`PruneClosedStreams` 前把子流父指针置空）。
  - `Frame`：`GetU16/U32/U64` 改为一次未对齐 `memcpy` + `bswap`；`DecodeImpl` 一次 32 位
    大端读同时取 length/type；`KnownType` 改位图判定。
  - `bento_bridge`：新增 `DecodeView` 与 `Decoder::NextView`（Bento→App 载荷零拷贝，
    旧路径每帧 1 次 malloc + 2 次 memcpy）。
  - `KvTable::ParseBinary`：按 count 一次性预分配 entries + 哈希索引，解析期不再
    重建索引（旧实现 512 键触发 ~10 次全量重哈希），重复键仍原地覆盖。
  - 资源：新增 `include/go2cpp/app/sha256_ni.hpp`（x86 SHA-NI 压缩函数 + 运行时
    `__builtin_cpu_supports("sha")` 检测 + 软件回退），`resource.cpp` 三处 SHA 接入。
  - `Connection`：`m_closed` 改 `std::atomic<bool>`，`closed()` 无锁读（0.23 ns vs 4.64 ns）。
- **关键文件**：`include/go2cpp/app/{stream_table,connection,bento_bridge,sha256_ni}.hpp`、
  `src/app/{stream_table,frame,kv,resource,connection,bento_bridge}.cpp`。
- **测试**：`ctest -R app_` **8/8 通过**（63 场景、24069 断言 0 失败）；
  SHA-256 另做 NIST 向量 + 301 组长度 + 200 组随机分片对拍，资源 1 GiB 摘要逐字节一致。
- **数据**（CPU 时间口径、同核、5 轮 min，load 5~7）：
  `PickNext` 256 流 60.26→**7.01 ns/op**（8.6×）；flush 256 流 112.57→**60.65 ns/帧**（1.86×）；
  flush 256 流依赖树 121.84→**59.50**（2.05×）；`KvTable::ParseBinary` 512 键
  18320→**10746 ns**（1.70×，分配 16→**2 次/Parse**）；`ResourceReceiver` 1 GiB
  2597→**575.6 ms（394→1779 MiB/s，4.5×）**；SHA-256 0.42→**2.29 GB/s**（5.3×）；
  帧视图解码 3.26→3.21 ns。1e8 帧压测：256 流 116.68→**59.65 ns/帧（16.77 Mfps）**，
  RSS 3.4~3.9 MiB 零增长。
- **状态**：完成，未 commit。报告 `/tmp/asm_app_rest.md`。
  未做（有量化理由）：ready 堆数组冗余 finish/id（再省 5~8 ns/帧）、KvTable arena+view
  （改所有权 API）、Connection 无锁化（上限 7.6%）。

## 2026-11-01 KV 存储引擎（`go2cpp/storage/kv`）

- **改动**：新增零依赖嵌入式 KV（开放寻址 MemTable + WAL + mmap SSTable + Arena +
  WriteBatch + epoch 无锁读），核心实现 ~2.6k 行（LevelDB 的 1/10）。
- **关键文件**：`include/go2cpp/storage/kv.hpp`、`src/storage/{arena,memtable,wal,sstable,kv}.cpp`
  （+ `format.hpp`/4 个内部头）、`cmake/Go2CppKv.cmake`、
  `tests/storage/{kv_test,kv_recovery_test,kv_concurrent_test,kv_bench}.cpp`、
  `docs/modules/storage-kv.md`、`docs/reports/asm-storage-kv.md`。
- **测试**：`ctest -R go2cpp_kv` **4/4 通过**（52665 断言功能 + 22 崩溃恢复 + 10031 并发）；
  覆盖 kill -9 后 WAL 重放、WAL 截断/位翻转、1MB value、64KB key、空 key/空 value、
  8 线程并发写读、边写边迭代。
- **数据**（1e7、`taskset -c 4-11`、ext4、gcc 13.3 -O2）：写 **56.3 万 ops/s**、
  读 **47.5 万 ops/s**、8 线程读 **238.6 万 ops/s**、sync 写 630（单线程）/
  **2525**（8 线程 group commit）。实测下限：随机访存 **155 ns/次**、
  `fdatasync` 中位 **556 µs**；MemTable Get 4.2x、KV Get 13.6x 下限。
  objdump：`MemTable::Lookup` 149 指令 / 0 call / 0 lock / 0 分配；
  CRC32C 走 SSE4.2 `crc32` 指令；SSTable 读无 `read/pread`。
- **状态**：完成，未 commit。报告 `/tmp/kv_storage.md`（另存 `docs/reports/asm-storage-kv.md`）。
  已知短板：读为 LevelDB 公开数据的 0.48x（段级 Bloom ×2.5 + TLB miss）；
  迭代器为快照式（全量排序），只适合中小数据集。

## 2026-11-01 · Go 标准库缺口（os/signal、hash/crc32、crypto/rand、flag）

- **改动内容**：新增四个 header-only 兼容层 + 4 个测试目标（追加到
  `cmake/Go2CppGoApi.cmake` 的清单，既有三个目标定义不变）。
  - `os/signal`：`sigaction`（进程级）+ self-pipe（写端非阻塞）+ 常驻分发线程。
    不用 `signalfd` 的原因：`pthread_sigmask` 只影响当前线程，进程里任一未屏蔽
    线程会按默认处置吃掉信号。handler 只做 async-signal-safe 三件事：读原子掩码、
    `write(2)` 一字节、无监听者时 `SIG_DFL` + `raise(2)`（Go 的 dieFromSignal）。
    `Notify/Stop/Ignore/Reset` 全实现，通道满丢弃、多次 Notify 并集。
  - `hash/crc32`：**零新算法**，映射到既有 `go2cpp/hash/crc32.hpp`（PCLMULQDQ
    IEEE + SSE4.2 Castagnoli + slicing-by-8）。
  - `crypto/rand`：`getrandom(2)` 优先 / `/dev/urandom` 兜底；自带 32 位 limb
    无符号 `BigInt`（Montgomery 模幂 + 前 60 素数试除 + Miller-Rabin）；
    `Int` 按 **Go 1.27** 语义用 `max-1` 的位长（2 的幂时少读一字节、
    `max==1` 不读源）；`Prime` 最高两位置 1。
  - `flag`：`FlagSet` + 包级 `CommandLine`；`-x=v`/`-x v`/`--`/bool 不消费参数/
    首个非 flag 参数终止/`-h` 返回 `kHelp`；内联完整 `ParseDuration` 文法
    （`tools/time.hpp` 尚未实现，未来应改为转发）。
- **关键文件**：`include/go2cpp/go/{os_signal,crc32,crypto_rand,flag}.hpp`、
  `tests/go_api/test_go_{crc32,crypto_rand,flag,signal}.cpp`、
  `tests/go_api/scripts/gen_{crc32,rand}_oracle.go`、
  `tests/go_api/testdata/{crc32,rand}_oracle.txt`、`cmake/Go2CppGoApi.cmake`、
  `docs/modules/go-stdlib-gaps.md`。
- **测试**（`-Wall -Wextra -Wpedantic -Werror`）
  - `go_api_crc32` 7 场景 PASS：1e6 字节 × 3 多项式与 **Go 1.27 oracle 逐项相等**；
    同数据与不查表的逐位实现相等；2000 次随机切分 `Update` 拼接一致；19 种边界长度。
  - `go_api_crypto_rand` 9 场景 PASS：`Int` **75 个值逐值等于 Go**；400 个候选
    （2..256 位）素性判定与 Go `ProbablyPrime(20)` 一致；1e6 字节 125000 个
    8 字节块零重复、卡方 297.3；2..256 位 15 个素数位长/最高两位/素性全过。
  - `go_api_flag` 10 场景 PASS：四种赋值、bool 三形态、Duration 11 合法 + 8 非法、
    5 条错误路径、`--`/位置参数、NFlag/Visit/VisitAll/Lookup/Set、`-h`、PrintDefaults。
  - `go_api_signal` 8 场景 PASS：子进程 `kill(2)` 端到端、Stop、Ignore、双通道、
    **Stop 后默认处置恢复（子进程被 SIGTERM 杀死，waitpid 见 WIFSIGNALED）**、
    无缓冲通道 100 次信号不阻塞、多次 Notify 并集。
- **数据**（Release，单核，64 × 1 MiB）：crc32 **IEEE 18.38 GB/s /
  Castagnoli 12.18 GB/s / Koopman 3.20 GB/s**；flag/signal 无性能指标（一次解析 /
  每信号一次分发）。
- **状态**：完成，未 commit。报告 `/tmp/go_stdlib_gaps.md`。
  两处 cmake 中间态（其他 agent 的 `Go2CppMetricsPrometheus.cmake` 与
  `Go2CppKv.cmake` 引用了未创建的测试源）导致 `.build-bento-wsl` 一度无法
  regenerate；最终四个目标在主目录 ctest **4/4 通过**（crypto_rand 的二进制用
  `ninja -t commands` 提取的同参数命令手工重编，`strings` 确认旧版已消失）。已知边界：未提供
  `signal.NotifyContext`；`BigInt` 无符号；`Prime` 始终使用传入 Source
  （Go 1.26+ 忽略它）；`flag` 解析失败不退出进程（与 Go `ExitOnError` 的刻意差异）。


## 2026-11-01 · 监控指标导出（Prometheus）+ HTTP /metrics

- **改动内容**
  - 新增 `include/go2cpp/metrics/prometheus.hpp` / `src/metrics/prometheus.cpp`：
    Prometheus 文本格式 0.0.4 导出（HELP/TYPE/样本/转义/NaN·Inf），
    histogram 分位数族 `<name>_quantile{quantile="0.5"}`（p50/p90/p99，
    桶内线性插值，与 `Histogram::Quantile()` 同语义）；`TextWriter` 纯
    `std::string::append` + `std::to_chars`，热路径 0 堆分配；
    `AppendQuantileFamilies` 对任意导出文本追加分位数（允许 out 与输入同一对象）。
  - 新增 `include/go2cpp/metrics/endpoint.hpp` / `src/metrics/endpoint.cpp`：
    `PrometheusEndpoint` 挂 `/metrics`（GET/HEAD 200、404、405+Allow、
    `text/plain; version=0.0.4`），`Attach(server)` / `Attach(server, fallback)`
    模板适配 `bento::HttpServer` 的 `OnRequest`，不硬依赖 bento 库。
  - 新增 `tests/metrics/{test_prometheus,test_prometheus_endpoint,bench_prometheus}.cpp`
    与 `cmake/Go2CppMetricsPrometheus.cmake`（挂到现有 `go2cpp_metrics`，
    新增 `PUBLIC go2cpp_network` 依赖：endpoint 用 `http::Headers::set`）。
  - `docs/modules/metrics-prometheus.md`。**未改动** `metrics.hpp` / `metrics.cpp`。
- **测试**（`.build-bento-wsl`，Release）
  - `ctest -R go2cpp_metrics`：**3/3 PASS**（现有 `go2cpp_metrics_test` 1.85s、
    新 `go2cpp_metrics_prometheus_test` 84 断言 0 失败、`go2cpp_metrics_endpoint_test`
    38 断言 0 失败）。
  - 官方 `prometheus_client.parser` 解析导出文本：3 族 / 9 样本全部接受。
  - 并发：4 写 + 8 读 × 100 次导出，0 解析失败 / 0 单调性失败，最终计数精确。
  - 真实 `bento::HttpServer` + `http::Get`：200/404/405/HEAD/fallback/4 线程并发抓取。
- **数据**（`taskset -c 2`，1e6 系列，3 次最小，load 3.94）
  - 纯渲染 **28.04 ns/指标（1349 MB/s），memcpy 的 5.79×**（memcpy 7808 MB/s）；
  - 端到端（Registry + 导出）**100.53 ns/指标（377 MB/s）**；现有
    `Registry::Render()` 基线 131.25 ns（差异为负载波动，两者同一函数）；
  - 分位数后处理 1e4 histogram 系列（10 桶）：**12.35 ms（1.24 µs/系列）**，
    首版 O(n²) 934 ms 的 **75×**（上一行序列快路径 + 标签哈希索引）。
- **状态**：完成，未 commit。报告 `/tmp/metrics_prometheus.md`。
  已知边界：端点无缓存/gzip/鉴权；分位数是桶内插值估算；文本解析只认本项目
  导出形态（第三方文本的乱序桶/时间戳会跳过）。


## 2026-11-01 · 日志轮转 + 配置热更新（ops-log-config）

- **改动内容**
  - 新增 `include/go2cpp/logging/rotate.hpp`：`RotatingLogFile` / `RotatingLogSink` /
    `RotationOptions` / `RotationPeriod`。按大小（`max_bytes`）与本地自然小时/
    自然日（`period`）轮转；`max_files` 保留 N 份并按文件名删除最旧；归档用
    自研 `compress::Gzip` 压缩（`.gz.tmp` + 原子 rename，失败保留原文件）；
    写入先进入内存缓冲，`fwrite` 完整成功后才移除，轮转临界区只做
    flush/fsync/rename/fopen，压缩与清理走单后台线程——轮转期间零丢失。
  - 新增 `include/go2cpp/config/watch.hpp`：`FileWatcher`（Linux inotify 优先、
    轮询兜底；监听父目录，只订阅 CLOSE_WRITE/MOVED_TO 等稳定事件）、
    `ConfigStore<T>`（8 槽引用计数 + 原子 active 索引，读侧无锁）、
    `ConfigWatcher<T>`（监听 → debounce → loader/validator → 原子发布 → 回调，
    失败保留旧配置）。
  - 新增 `tests/logging/test_log_rotate.cpp`（8 场景，含 1e6 条/12 次轮转压测）、
    `tests/config/test_config_watch.cpp`（9 场景，含 1e4 次变更与无锁读并发）。
  - 新增 `cmake/Go2CppOpsLogConfig.cmake`（两个 ctest 目标，默认 ON）；
    顶层 `CMakeLists.txt` 末尾追加一行 include。
  - 新增 `docs/modules/ops-log-config.md`。**未改动** `log.hpp` / `config.hpp`
    及任何既有源码、测试。
- **测试**（`.build-bento-wsl`，Release，`-Wall -Wextra -Wpedantic -Werror`）
  - `ctest -R 'go2cpp_(log_rotate|config_watch)_test'`：**2/2 PASS**
    （8.61 s + 3.13 s）。
  - 日志：1e6 条 / 39.9 MB / 12 次轮转，逐行验证 0..999999 **0 丢失、
    0 重复、0 乱序、0 撕裂**；12 个 `.gz` 全部通过 `Gzip::Reader` 的
    CRC32/ISIZE 校验；并发 8 线程 × 5000 条、8 个真实场景（含打开失败恢复、
    保留策略、按天轮转、随机混合）全部 PASS。
  - 配置：1e4 次原子替换变更 **0 失败**（206.6 µs/次）；4 读者 + 2000 次
    发布共 956 万次 `Load()` **0 撕裂**；删除重建、非法配置保留旧值、
    validator 拒绝、轮询回退、Stop 语义全部 PASS。
  - TSAN（`setarch -R`，WSL 需关闭 ASLR）：配置热更新测试 PASS，
    **0 条 ThreadSanitizer 警告**。
- **数据**（WSL ext4，/tmp，单次运行）
  - 日志：write 0.160 s（约 238 MB/s）、close（压缩+清理）0.157 s、
    端到端 0.317 s（120 MB/s）。
  - 配置：`ConfigStore::Load()` 3.58 ns/次；`Store()` 1.0 µs/次（1 写 + 4 读）；
    inotify 1e4 次变更事件数恰好 10000。
- **状态**：完成，未 commit。报告 `/tmp/ops_log_config.md`。
  已知边界：日志磁盘长期不可写超过 64 MiB 缓冲安全阀会拒收新数据并计数；
  配置快照槽位固定 8 个（长期持有需 `share()`）；轮询模式同尺寸原地覆盖
  可能受文件系统时间戳粒度影响；未在 Windows 上验证。

### 2026-11-01 · LSM 功能层：Queue（MQ）/ Subscribe（Watch）/ MemCache

- **范围**：在 aecc513 的 KV 引擎之上补三个功能层，**共用同一套 LSM**。
  新增文件，未改动 `kv.hpp` / `kv.cpp` / `wal.*` / `memtable.*` / `sstable.*`。
- **新增**
  - `include/go2cpp/storage/queue.hpp` + `src/storage/queue.cpp`（MQ）：
    `Append/Consume/Ack/Seek/FlushCursors/Sync`，at-least-once；
    游标是 KV 记录 `queue/<topic>/<consumer>/offset`，每 4096 次 Ack 批量
    写一次 `WriteBatch`；内存 deque + 日志冷读预取（4096 条/次）。
  - `include/go2cpp/storage/watch.hpp` + `src/storage/watch.cpp`（订阅）：
    `Watch/Unwatch/Put/Delete/Write/Notify/Replay/FlushOffsets`；
    写 KV 成功后再发事件，事件写变更日志（同 WAL 格式）；
    同步（默认）与异步（单线程 + 背压）两种派发；订阅游标
    `watch/<id>/offset` 存 KV；无订阅者时写路径只多一次原子读。
  - `include/go2cpp/storage/memcache.hpp` + `src/storage/memcache.cpp`（缓存）：
    `Set/Get/Peek/Delete/Expire/Clear/PurgeExpired`，TTL（复用
    `cache/detail/time_wheel.hpp` 的 256 桶时间轮）+ LRU（复用
    `cache/detail/intrusive.hpp` 的侵入式链表与节点池）+ 分片锁；
    `persist` 写穿 KV、`read_through` 回填。
  - `src/storage/seglog.{hpp,cpp}`：Queue/Watch 共用的「WAL 顺序追加 +
    SSTable 段索引」层（写端复用 `WalWriter`，段封口把稀疏检查点写成
    SSTable，读端自带按偏移定位的扫描器，恢复时截断尾部垃圾）。
  - 测试：`tests/storage/{queue,watch,memcache}_test.cpp` +
    `tests/storage/features_bench.cpp`。
  - 文档：`docs/modules/storage-features.md`、`docs/reports/asm-storage-features.md`。
  - `cmake/Go2CppKv.cmake`：库追加 4 个源文件，新增 3 个测试目标 + 1 个 bench 目标。
- **测试**（`.build-bento-wsl`，Release，`-Wall -Wextra -Wpedantic -Werror`）
  - `go2cpp_queue_test --n=1000000`：**PASS**（59 209 checks / 0 failures）。
    1e6 全量 append+consume+ack，offset 严格 0..999999，0 丢失 0 重复；
    游标持久化（重开 KV 续上）；fork+SIGKILL 崩溃恢复（Head=100000、
    游标=10、续写 offset=100000）；256KB 段滚动产生多段 + `.idx`；
    尾部垃圾截断后可继续追加。
  - `go2cpp_watch_test --n=100000`：**PASS**（111 180 checks / 0 failures）。
    1e5 次写触发回调，seq 严格连续、key 顺序一致；Replay 全量/区间；
    稳定 id 游标持久化；异步模式 1e4 条全部派发且顺序正确；无订阅者时
    events=0、日志 0 字节。
  - `go2cpp_memcache_test --n=10000000`：**PASS**（574 checks / 0 failures）。
    1e7 Get+Set（容量 1e6，命中率 99.9%）、TTL 过期与覆盖清 TTL、
    LRU 淘汰与命中提升、4 线程 × 5e5 并发、写穿 + 回填。
- **数据**（1e7，WSL ext4，`taskset -c 0-3`，日志 `/tmp/bench_1e7.log`）
  - Queue：append 196.1 ns（509 万/s）、consume 24.2 ns（4125 万/s）、
    consume+ack 69.7 ns（1434 万/s）、冷读 2 169.6 ns；
    裸 WAL 追加（SegLog 40B）71.6 ns → append 距下限 **2.7×**。
  - Watch：写 + 同步回调 785.4 ns（127 万/s，回调本身 53 ns）；
    无订阅者 533.8 ns（KV::Put 基线 460.8 ns，+16%）。
  - MemCache：Get 859.8 ns（116 万/s）、Peek 570.9 ns、Set 721.5 ns；
    **纯查找与项目 LruCache（585.3 ns）持平**；裸 unordered_map 143.9 ns。
  - 下限：memcpy 46~61 GB/s、裸顺序写 /dev/shm 1.73 GB/s、/tmp 0.54 GB/s、
    fdatasync 1.8~2.1 ms。
- **优化前后**（同进程 A/B，`QueueOptions.topic_cache`）
  - queue.append：n=2e6 188.1 → **114.4 ns（−39.2%）**；
    n=1e7 302.3 → **196.1 ns（−35.1%）**。
  - MemCache 的条目重排与 Get 侧过期扫描节流：1e7 实测**无统计显著收益**
    （噪声 ±25%），如实记录，未包装成优化。
  - objdump：`MemCache::Impl::Get` 有 7 个 `lock` 前缀（统计计数器），
    已识别「分片内普通计数」可省 ~8%，因落在噪声内未做。
- **状态**：完成，**未 commit**。报告 `/tmp/lsm_features.md` 与
  `docs/reports/asm-storage-features.md`。已知边界：冷读每次 Refill 都会
  打开段索引（未做索引缓存）；Queue 单 topic 消费串行；Watch 前缀线性匹配；
  MemCache 分片数应按并发度配置（单线程 8 分片比 1 分片慢 14%）。

## 2026-11-02 工具类模块汇编级优化（Base64 / gzip / reflect JSON）

- **Base64**：反查表移入 `constexpr` 构造期（256 项成员表，四个预定义实例编译期常量）；
  编码改 `resize` + 单次 32-bit store（原每字符 `push_back`）；解码单遍 quantum +
  `thread_local` scratch（零堆分配，长度错误才走「先找非法字符」慢路径），端序分支安全。
  16 MiB：编码 1004→**1980 MiB/s**、解码 600→**1566 MiB/s**（Go 同机 1663/1582，0.84×/1.01×）；
  60 B 编码 58.5→**30.9 ns**（Go 41.9）。
- **gzip 压缩**：BitWriter 改 64 位累加器 + 32 位整字落盘（`Align` 按 `m_bits&7` 补位）；
  固定 Huffman 反转码表（288+32 项）；哈希链跨块持久（绝对位置 + 32 KiB 环形前驱表，
  不再每块重建 128 KiB head / 重插 history）；匹配比较 8 字节 + ctz；按格式只算一种校验和；
  CRC slice-by-8。1 MiB L6：235.7→**550 MiB/s**（zlib 421，0.77×）。
- **gzip 解压**：LZ77 匹配整段 `memcpy`（距离<长度时倍增展开）+ 窗口环形批量读写 +
  批量校验和（gzip slice-by-16 CRC32 / zlib adler32 分块 / raw 跳过）。
  1 MiB：486.8→**1880 MiB/s**（zlib 2984，1.59×；剩余瓶颈是 CRC，下一步 PCLMULQDQ）。
- **reflect JSON**：`Buffer::GetStringView`（字段名零分配）、`DeserializeReflect` 栈数组、
  `DecodeBinary` 零拷贝指针解析（原 3 次整消息/载荷复制）、payload `move`、
  `WireFieldTable<T>()` 字段表按类型缓存、`JsonWriter` 转义零临时串、
  `JsonParser` 字符串整段 append + `members.reserve(8)`、Envelope 直接写 `out`。
  二进制编 225.6→**79.6 ns**（protobuf 81.7，0.97×）、解 159.3→**111.7 ns**（protobuf 78.1，1.43×）；
  文本编 340.4→**259.4 ns**、解 929.3→**772.3 ns**（nlohmann 477.3/809.3，0.54×/0.95×；
  rapidjson 103.8/275.9，2.50×/2.80×）；JSON 文档解析 334.3→292.4 ns（rapidjson 275.9，1.06×）。
- **测试**：`ctest -R 'encoding|compress|reflect'` 5/5 通过（compress 1196 checks，含 GNU gzip /
  Python zlib 双向互操作；reflect 10969 checks；encoding 全过）；全量 496 目标
  `-Werror` 构建成功。未 commit。
- **基准**：`tests/bench/bench_tools_industry.cpp` + `tools_bench.proto`（手工编译，不登记 CMake）；
  Go 对照 `/tmp/gobench/main.go`。
- **报告**：`/tmp/tools_vs_industry.md`（每项 vs 工业库与是否 <1.1×）。
- **未达标**：gzip 解压 1.59×（CRC 需 PCLMULQDQ）、reflect 二进制解码 1.43×（自描述字段名开销）、
  文本路径 vs rapidjson 2.5~2.8×（envelope + 建树，vs nlohmann 已达标）。

## 2026-11-02 · 复用纪律（先查项目内，再查项目外）

**背景**：这一天内发生 3 次「重复造轮子」——
① picoquic 早已在 ~/build-deps/picoquic（docs/quic.md 写死），仍自研 QUIC 570 行；
② LevelDB 集成前没查 docs/modules/，重复 clone/编译（正确做法后来还是自编译，但查文档能少走弯路）；
③ 9eab7203 为「不改一行 CMake」把 21 GB/s 的 CRC 实现复制进 src/compress/crc32_x86.hpp。

**规则（写进 docs/engineering-standards.md §1.1，强制）**：
1. 新功能动工前**先查项目内**：docs/ 模块文档 → xample/+xamples/ → grep 全仓 → docs/reports/reuse-audit.md → AI_COLLAB.md；
2. 再查项目外工业库（§四参考清单）；
3. **禁止**不问不查就新写一份；**禁止**把已有实现复制进自己模块（宁可改一行 CMake 加链接）；
4. 复用顺序：项目内已有 > 工业库 > 自己写。

**当天的量化结果**：复用审查（docs/reports/reuse-audit.md，408 行）查出真重复 15 组 / 部分重复 4 组 / 看似重复但不同 14 组 / 薄封装 8 类；据此派出 4 个修复 agent（随机源 5 处、endian 13 处、CRC 2 套、字符串/解析 39 处、SHA-256 3 份、FNV 5 处）。

## 2026-11-04 · 分布式 P1：MetadataStore + Coordinator 快照持久化

**背景**：`docs/reports/distributed-analysis.md` P1（4 人天）——`Coordinator` 的 fencing 计数只在内存，
重启即回退，破坏「永不复用」。P0（NodeId/Trace/metrics，`99ed212`）已完成，本轮只做 P1。

**改动**：
- 新增 `include/go2cpp/distributed/metadata_store.hpp` + `src/distributed/metadata_store.cpp`：
  KV + 全局 revision + 前缀 Watch（游标重放/同步异步派发/回调异常隔离）+ 可选 WAL 文件后端；
- 新增 `src/distributed/coordinator_snapshot.cpp` + `src/distributed/snapshot_codec.hpp`：
  Coordinator 快照（fencing 表 + 租约表 + 选举 epoch，版本 1，尾部 CRC32，临时文件 + fsync + rename 原子替换）；
- `coordinator.{hpp,cpp}`：`CoordinatorOptions::{snapshot_path,snapshot_on_write,snapshot_fsync}`、
  `Restore()/Snapshot()/healthy()/load_error()/persist_error()`；授予路径 dirty + 返回前落盘；
  快照损坏 → fail-stop（写操作 `kUnavailable`），不静默回退；
- `tests/distributed/test_metadata_store.cpp` + `test_coordinator_snapshot.cpp`；
- `cmake/Go2CppDistributed.cmake`：两个源文件 + 两个测试目标（label distributed）。

**未动**：`transport.cpp`、rpc；未 commit。

**验证**：11 个 distributed ctest 全过；`-O2 -Wall -Wextra -Wpedantic -Werror` 三份实现单独编译零告警；
实测重启 fencing `1 → 2 → 3`、截断快照拒绝启动；报告 `docs/reports/2026-11-04-distributed-p1.md`
（架构/快照格式/fsync 策略/汇编指令数与耗时/未决项）。

**坑**：改 `CoordinatorOptions` 布局后，只重建新 target 会让**旧测试二进制**与新 `.so` ABI 不匹配
（表现为 stack smashing / watchdog 超时）。改公共头后必须重建全部相关测试目标再跑 ctest。

## 2026-11-04 · Gateway 高危修复：Dispatch 异常兜底 + 请求总时限 + 同包流水线

**背景**：HTTP 链路审计（`src/app/gateway.cpp` @ `19562e9`）确认 2 个高危 + 1 个中级：
handler 异常导致 SIGABRT；slowloris 靠单次 recv 30s 无限占住 worker；`take_remaining()`
未调用导致一次 send 多个请求只回第一个。

**改动**：
- `src/app/gateway.cpp` / `include/go2cpp/app/gateway.hpp`：
  `Dispatch` 与 `WorkerLoop` 调用点双层 try/catch → 500 + close + `Stats::internal_errors`；
  新增 `Config::request_timeout`（默认 10000ms，0 归一化）与 `Stats::request_timeouts`，
  `ServeConnection` 每请求从首次 recv 前建立总 deadline，每次 recv 只等剩余时间；
  `pending = parser.take_remaining()` 驱动同包流水线排空（最多 100 请求/连接），
  `SerializeResponse` 的 Connection 头跟随 `Response::close`。
- `tests/app/test_gateway.cpp`：新增 [9] throw→500、[10] worker=2 半开连接、[11] 3 请求流水线。

**验证**：`tests/app/build_and_run_gateway.sh` → `checks=62 failures=0`，`-Werror` 零告警。
前后对比：throw 由 `Aborted (core dumped)` → 500 且进程存活；2 条半开连接下新请求
29773ms → 9712ms（默认 10s）/ 601ms（配置 800ms）；同包 3 请求 1 响应 → 3 响应。

**未动/未提交**：其他文件未改，未 commit；handler 执行时间不在总时限内、跨包 keep-alive
未启用、`test_gateway` 仍未注册 CTest，均记入 `docs/reports/http-link-audit.md` TODO。

**坑**：`http::Parser` 完成后 `m_finished=true`，必须 `take_remaining()` 后再用**新 Parser**
解析下一条，否则 feed 直接返回 Complete；`Response::close` 必须与实际连接去留同步，
否则客户端会按 keep-alive 等待已关闭的连接。

## 2026-11-04 · 抖音写接口幂等键中间件（可靠性主线 1）

**背景**：切网/断线重连后客户端重发，抖音评论等写接口没有幂等键，同一请求
重发会重复执行（重复评论）。服务端此前只有示例 `Idempotency-Key` 思路
（`user/modules/network/02-connection-reset.cpp`），无实现。

**改动**：
- 新增 `app/douyin/include/idempotency.hpp`：`IdempotencyMiddleware`，去重表
  `idempotency_keys(key PK, method, path, status, response_body, created_at)`
  与业务同库（`data/douyin.db`，表前缀 `idempotency_`）；只对
  POST/PUT/PATCH/DELETE 生效；key 命中且未过 TTL → 回放首次状态码/body 并加
  `Idempotency-Replayed: true`；未命中 → 执行 handler，仅 2xx 写表；TTL 默认
  24h（`DOUYIN_IDEMPOTENCY_TTL_SECONDS` 可覆盖，测试用），访问时懒删除过期行；
  缺 key 放行并记 WARN，DB 故障降级直通。
- `app/douyin/include/douyin_http.hpp`：`HttpResponse` 增加 `headers`（额外响应
  头），`Serialize` 输出时跳过含 CR/LF 的字段（防响应头注入）。
- `app/douyin/include/douyin_store.hpp`：新增 `Store::Database()` 借用底层
  `storage::DB*`，中间件复用同一连接。
- `app/douyin/src/interaction_service.cpp`：启动时 `EnsureSchema` 建幂等表，
  整个路由外层挂 `IdempotencyMiddleware`（`POST /api/comment`、`/api/like`
  自动受保护；GET/HEAD 在中间件内直通）。
- 新增 `app/douyin/scripts/idempotency_test.sh`：四组真实 HTTP 验收。

**验证**：
- `scripts/idempotency_test.sh`（独立库 `data/idempotency_test.db`，29201/29202/
  29204 三进程）：22 项断言全过。同 key 两次 → 第二次同 comment id、响应体
  字节一致、`Idempotency-Replayed: true`、DB 1 条；不同 key → 2 条；无 key →
  2 条且日志有「缺少 Idempotency-Key」WARN；TTL=1s 重启等 2.5s → 重新执行
  （新 id、无 Replayed），累计 6 条。
- `app/douyin/build.sh`：8 目标（含 gateway/loadgen）全 ok，`-Werror` 零告警。
- `scripts/accept_all.sh`：70 PASS / 0 FAIL，无回归。
- 生产：按 pid 停旧 interaction（122204）→ 新 pid 130904 重启（`idempotency_ttl=86400s`），
  `serve_public.pids` 已更新；公网 `http://82.157.147.127:8443/douyin/health` = 200、
  `/api/feed` = 200。

**未提交**：不 commit。

**坑**：并行 agent 同改 `douyin_store.hpp` 导致上传声明块重复声明而编译失败，
已按当前版本恢复（`StoredUpload`/上传方法各 1 份）；同文件改动需串行合并。

## 2026-11-04 · 抖音「我的主页播视频」+ 推荐流随机化

**背景**：用户报「我的主页点自己的视频放不了」+「智能推荐要随机、要能刷到自己」。

**根因（实测）**：
- 服务端 URL 没问题：`GET /api/video/mine` 返回 `videos/upload_31_*.mp4`，经网关
  `GET /videos/...` 与公网 `/douyin/videos/...` 均 200 / `video/mp4`。
- 真正问题在**两条播放路径**：主页点视频走独立 `<video>` 弹层（`openVideoModal`），
  与 feed 的 `playOnly`/IntersectionObserver 不是同一套；该弹层没有 error 监听、
  没有 controls，地址前缀只认 `^videos/`（历史数据是 `/videos/x`、纯文件名或外链时
  会静默回退成随机 demo 视频）。一旦这条路径出问题，界面就是黑屏无提示。
- 推荐流是固定热度序（`kVideoOrder`），0 互动的新视频永远在末页，`limit=20`
  的首屏（全库 22 条）根本刷不到自己。

**改动**：
- `include/douyin_home_page.hpp`：删除主页弹层（HTML/CSS/JS 共 3 处），新增
  `renderFeed()` 与 `playInFeed(item,v)`——主页点视频先关主页，视频不在当前
  feed 就 `state.items.unshift` 后重建，再 `scrollIntoView` + `playOnly`，与 feed
  完全同一套播放器；`videoSrc()` 兼容 `http(s)://`、`/videos/x`、`x.mp4`、`videos/x`。
- `src/douyin_store.cpp` / `include/douyin_store.hpp`：`ListFeed` 改随机推荐——
  全库候选（不按作者过滤）→ `std::shuffle(mt19937_64(seed))`；游标编码
  `r<seed_hex>_<index>`（`EncodeFeedCursor`/`DecodeFeedCursor`），同一 seed
  重新洗牌从下标续取，**随机但翻页不重不漏**。
- `src/feed_service.cpp`：`ReadCursor` 改用 `DecodeFeedCursor`（非法 400）。
- `scripts/accept_all.sh`：feed 断言从「最新在前」改为「集合覆盖 + 5 次首屏出现
  不同顺序」。

**验证**：
- 随机：连续 5 次 `/api/feed?limit=20` 顺序互不相同（4/4 相邻不同）；10 次
  首屏 10/10 都能刷到 admin 自己的视频（id 59/60）；`limit=5` 三页 15 条无重复；
  非法 cursor 400。
- 主页播放（无头 Edge + CDP 实跑）：点「我的主页」→ 2 张卡 → 点卡 → 主页关闭、
  feed 该屏 active、`readyState=4`、`paused=false`、`currentTime` 前进；把首屏压到
  2 条（视频不在 feed）时自动补插成 3 屏并播放。
- `scripts/accept_all.sh`：70 PASS / 0 FAIL；`build.sh` 8 目标 `-Werror` 零告警；
  公网 `/douyin/` 200（页面已含 `playInFeed`、无 `openVideoModal`）、公网 feed
  3 次顺序不同、公网 mine 两条 url 均 200。
- 按 pid 重启 7 进程（user 131636 / video 131637 / feed 131638 / interaction 131639 /
  live 131640 / search 131641 / gateway 131642），未用 pkill -f。

**未提交**：本 agent 不 commit。注意 `douyin_store.hpp` 的改动被并行 agent 的
`5dcca19`（幂等键中间件）一并提交；`src/video_service.cpp`、`scripts/accept_upload_resume.sh`
是并行 agent 的改动，不属于本次。


## 2026-11-04 · 抖音上传断点续传（分片上传协议）

**背景**：`POST /api/video/upload` 是整包 POST，断线即失败、重传从头开始。

**改动**：
- `app/douyin/src/video_service.cpp`：新增 4 个接口——`POST /api/video/upload/init`
  （生成 UUID v4 + total/title）、`POST /api/video/upload/chunk`
  （`Content-Range: bytes a-b/N`，顺序追加 `web/videos/.upload_<id>.part`）、
  `POST /api/video/upload/finish`（大小 + ftyp 校验 → 改名正式文件 + 建 videos 行）、
  `GET /api/video/upload/status/<id>`（断点进度）。跨用户/不存在统一 400，防会话探测；
  `.part` 实际大小与 DB 进度不一致时以磁盘校准（崩溃窗口自愈）。
- `app/douyin/include/douyin_store.hpp` + `src/douyin_store.cpp`：`upload_sessions`
  表（upload_id 主键、user_id、path、title、total、received_bytes）+ 4 个访问器，服务重启不丢。
- `app/douyin/scripts/accept_upload_resume.sh`：50 项真实验收（10 MB 分 5 片 cmp 一致、
  重启服务后续传、续传中 kill -9 客户端无半片、乱序/总大小不符/跨用户/非法头 400）。

**验证**：`accept_upload_resume.sh` 50 PASS / 0 FAIL；官方 `accept_all.sh`
70 PASS / 0 FAIL（默认端口全量）；`build.sh` 8 目标 `-Werror` 零告警。
**未决**：协议只接受连续前缀（乱序直接 400，不支持空洞补传）；`upload_sessions`
无定期清理，中断会话的 `.part` 需要后续 GC 策略。
**未提交**。

## 2026-11-04 · SendAll + DNS BlockingRegion（H-06 / H-04）

**背景**：`docs/reports/mindset-network.md` 的两个高危——`Socket::send` 部分成功即
返回 `ok()=true`（按 Go `Write` 用会静默丢数据，库无 SendAll）；`Address::Lookup`
直调 `getaddrinfo`、无超时、Hook 未接管，DNS 慢时卡 M 5–20s。

**改动**：
- `include/go2cpp/network/socket.hpp` + `src/network/socket.cpp`：新增
  `Socket::SendAll`——循环 sendmsg 到全量/超时/错误，整个循环共享一份总超时
  （`remaining()` 逐轮扣减），一次 `begin_operation`；失败返回已进入内核的字节数
  （调用方从 `data + transferred` 续传），零进展返回 -1；空写 `{0,0}`、
  `nullptr` → `EINVAL`、单次写 0 字节按 `EPIPE` 上报防空转。`send` 语义不变，
  只补文档标注「小包用 send，大包/循环用 SendAll」。
- `src/network/address.cpp`：`Address::Lookup` 的 `getaddrinfo` 外包
  `std::optional<BlockingRegion>`（仅 `Scheduler::current_task() != nullptr` 时
  emplace），region 只覆盖该调用、不跨 park/yield；普通线程上为 no-op。

**验证**（`GO2CPP_TEST_FILTER=network`，连跑 2 次全过，约 13 s/次）：
- 单次 `send` 8 MiB 短写对照：`transferred=130964 / 8388608`（`ok=true`）；
- `SendAll` + 慢读端（64 KiB/1 ms）：`sent=8388608 received=8388608`，
  199 ms / 198 ms，逐字节内容校验通过；
- `SendAll` 对端不读 + 100 ms 总超时：`transferred=146176`、`error=110`
  （`ETIMEDOUT`），证明超时不被每轮重置且保留进度；
- 受管 Fiber 内 `LookupAnyIPAddress("localhost:80")` 成功，`sysmon_running`
  且 `sysmon_pass_count > 0`；普通线程对照结果一致；
- `build-channel-audit` 增量构建，`-Wall -Wextra -Wpedantic` 零告警。

**未决**：DNS 仍是「阻塞但可见」（sysmon 可补 M），真正的异步 resolver 线程池
不在本次范围；`SendAll` 未覆盖 UDP 批量语义（批量走 `send_batch`）。
**未提交**：本 agent 不 commit。

## 2026-11-04 · 抖音 demo 视频压缩（5 个素材）

**背景**：`app/douyin/web/videos/demo1-5.mp4` 合计 8.58 MB（demo3 52 s / 4.37 MB，854×480），
北京服务器带宽有限，在不明显损失观感的前提下压缩体积。

**改动**（只动数据文件，未改代码）：用 ffmpeg 重编码
`libx264 -crf 28 -preset medium -vf scale=min(720,iw):-2 -pix_fmt yuv420p`，
音轨 `aac 96k`，`-movflags +faststart`；原文件备份到 `web/videos/orig/demoN.orig.mp4`
（静态路由不递归子目录，`/videos/orig/...` 返回 404，备份不会被公网取到）。

**结果**：
| 文件 | 原体积 | 新体积 | 压缩率 | 时长 | 分辨率 |
|---|---|---|---|---|---|
| demo1 | 788,493 | 245,840 | 68.8% | 10.027 s | 320×176（未放大） |
| demo2 | 1,128,375 | 143,723 | 87.3% | 5.055 s | 960×540 → 720×406 |
| demo3 | 4,372,373 | 1,919,183 | 56.1% | 52.208 s | 854×480 → 720×404 |
| demo4 | 1,118,597 | 599,436 | 46.4% | 10.000 s | 640×360（未放大） |
| demo5 | 1,174,960 | 868,422 | 26.1% | 10.010 s | 640×360（未放大） |
| 合计 | 8,582,798 | 3,776,604 | 56.0% | — | — |

**验证**：ffprobe 全部 h264+aac、时长变化 ≤0.001 s；`ffmpeg -f null` 全量解码零错误；
`curl -I http://127.0.0.1:28080/videos/demoN.mp4` 全部 200 + `video/mp4`；
`moov` 在 offset 36（faststart，可边下边播）；从 HTTP 源 seek 到 20 s 解码 2 s 成功。
DB `videos.url` 存相对路径 `videos/demoN.mp4`（20 条引用），文件名未变，**无需改库**。
`upload_31_*.mp4` 未触碰（4717361 B / 原 mtime 不变）。

**未决**：静态服务对 `Range` 返回 200 全量而非 206，浏览器不能 seek；
demo5 压缩率仅 26.1%（源码率本身偏高），要更小需再降 CRF。
**未提交**：视频被 `.gitignore`（`web/videos/*.mp4`）覆盖，本 agent 不 commit。

## 2026-11-04 WS 断线补发（Resume）—— bilibili 弹幕房间

**改动**：
- `include/go2cpp/app/realtime_room.hpp`：新增 header-only `realtime::SeqJournal`
  （单房间单调 seq + 容量可配环形缓冲 + `Since(last_seq)` 区间/gap 判定）；
  `RoomHub` 本体未改，接口与行为零变化。
- `app/bilibili/include/bili_danmaku_room.hpp` / `src/bili_danmaku_room.cpp`：
  `RoomHub` 接入 SeqJournal，业务广播注入 `"seq":N`；新增 `JoinWithResume()`
  （持锁「先补发、后登记」，补发必然先于后续实时消息）、`BroadcastEphemeral()`
  （online 是派生状态，不占 seq、不进缓冲、重连时重新广播）、`LatestSeq()`；
  `Broadcast()` 改为锁内 Enqueue（Enqueue 不再做 socket I/O，慢连接由发送线程关闭）；
  `Leave()` 保留 journal，房间暂空时序号不回退。
- `app/bilibili/src/danmaku_service.cpp`：WS 握手支持 `?last_seq=N`；窗口滚出时回
  `{"type":"resume_gap","earliest":x,"latest":y}`；新增 `--resume-buffer N`
  （默认 1000）；连接队列上限取 `max(--max-queue, resume-buffer)`，避免补发被
  慢消费者保护踢掉。
- 新增测试：`tests/app/app_test_realtime_resume.cpp`（注册为 ctest `app_realtime_resume`）、
  `app/bilibili/scripts/accept_resume.sh` + `accept_resume.py`。

**协议**：可续传业务帧为 `{"seq":N,...}`（房间内从 1 单调递增，弹幕）；online 为
瞬时帧不带 seq。重连带 `?last_seq=N` → 补发 `(N, latest]`；N 早于缓冲起点或大于
latest（服务重启）→ `resume_gap`，客户端走 REST 全量刷新。

**测试结果**：
- `ctest -R realtime`：`app_realtime_resume` 1/1 PASS（737 项检查，300 轮随机，
  20 万条 Assign = 2211 万条/秒）。
- `app/bilibili/scripts/accept_resume.sh`：45 PASS / 0 FAIL。含 A 收 5 条断开 →
  发 10 条 → `last_seq=5` 补发 **seq 6..15**；`last_seq=0`（缓冲 4）→
  `resume_gap earliest=5`；边界/随机 last_seq；20 客户端 × 200 条共 4000 条
  连续无洞（9103 条/秒）；并发随机重连 10 例补发连续；限流第 6 条仍 429。
- 既有 `tests/app/test_realtime_room.cpp`：52 项 0 失败（RoomHub 未回归）。
- bili `build.sh`（-Wall -Wextra -Wpedantic -Werror）：零告警。
- 抖音 `accept_all.sh` 未跑：并发会话正在重编 douyin `gateway` 且占用
  28080/29105；douyin 源码不引用 `realtime_room`/`SeqJournal`（grep 无结果），
  本次改动对抖音零影响。

**已知限制 / 后续**：journal 按 video_id 常驻内存，生产需加 TTL 或房间空闲回收；
未 commit。
