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
