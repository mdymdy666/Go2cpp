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
