# 审查批2：可扩展性 / 功能点 / 规范 / API 一致性

> **基线**：工作树 `2026-10-28`（`bento/app/**`、`net/udp_transport`、`net/x25519` 均为未提交新文件）
> **审查范围**：`include|src/bento/app/{fetch,file_browser,backup,zip,capability,session}`、
> `include|src/bento/net/{udp_transport,x25519}`、`docs/modules/bento-scenarios.md`
> **审查方式**：只读静态审查（未改代码、未重新构建、未复跑测试）。
> 测试结论仅引用既有产物 `build-review/Testing/Temporary/LastTest.log`（2026-10-28 12:51），
> 该日志**只含 9 个测试**（筛选运行），不是全量。
> **审查者**：审查批2 agent ｜ **日期**：2026-10-28

---

## 0. 结论摘要（先给判断）

**代码实现质量整体偏高**：安全默认（沙箱拒绝、原子落盘、上限预检、Zip Slip、低阶点拒绝）做得扎实，
Doxygen 中文注释密度与 net 既有风格接近，既有 9 个测试全部 PASS。
**问题集中不在实现，而在「体系一致性」**——app 层是**在文档与构建体系之外长出来的一套平行模块**：

| 维度 | 评分 | 一句话结论 |
|---|---|---|
| 1. 可扩展性 | **中** | 4 个扩展点各有不同答案：HTTP 不可换、zip 结构上可加压缩但需改 4 处、session 可换 transport 但服务端需自带 accept、审计/日志钩子**完全缺失** |
| 2. 功能点（FR-09/15/18/CON-06） | **中上** | FR-09/15/18 的主体能力齐备且质量高；真正的缺口是 **CON-06 的「降级策略」没有落地**（`Probe()` 无任何生产消费者）与 **FR-18 的「远端校验」缺失** |
| 3. 规范 | **差** | `bento-scenarios.md:171` 明文写「Bento 不引入 zip/HTTP」，而 zip/fetch 已实现；app 模块在 `docs/INDEX.md`、`codex.md`、`PROTOCOL.md` 中**全部缺席** |
| 4. API 一致性 | **中下** | 命名空间三套并存（`bento` / `bento::app` / `bento::net`）、状态名两种风格（`invalid_argument` vs `invalid-argument`）、6 套独立状态枚举、2 份重复的 `SleepBeforeRetry` |

**必须优先处理的三件事**（否则后续批次会持续踩坑）：
1. **A-01**：`bento-scenarios.md:171` 的「不适用」声明与已实现代码正面矛盾——这是文档级假陈述。
2. **A-02**：`docs/INDEX.md` 规定「新增文档必须登记」，`bento-scenarios.md` 与 6 个 app 模块均未登记，
   `codex.md` 也**没有任何** MOD-27/30/31/37/39 的变更记录（最后一条是 2026-10-27 第 26 节）。
3. **A-03**：`app::Session` 定义了**第二套线上协议**（8 种帧类型，编号 1..8 与 `protocol.hpp:FrameType`
   的 1..8 完全重叠），却没有任何线格式文档，`tests/bento/PROTOCOL.md`（680 行）只覆盖 Link 协议。

---

## 1. 审查范围与阅读状态

| 文件 | 行数 | 阅读程度 |
|---|---|---|
| `include/go2cpp/bento/app/fetch.hpp` | 240 | 全文 |
| `include/go2cpp/bento/app/zip.hpp` | 267 | 全文 |
| `include/go2cpp/bento/app/backup.hpp` | 187 | 全文 |
| `include/go2cpp/bento/app/file_browser.hpp` | 241 | 全文 |
| `include/go2cpp/bento/app/capability.hpp` | 165 | 全文 |
| `include/go2cpp/bento/app/session.hpp` | 356 | 全文 |
| `src/bento/app/fetch.cpp` | 1676 | 结构 + 关键段（1110–1676 全文；1–120、355–505 抽样） |
| `src/bento/app/zip.cpp` | 1202 | 结构 + Writer 全文（355–790）、常量/头字段 |
| `src/bento/app/backup.cpp` | 702 | 结构 + 221–300、300–419、440–702 |
| `src/bento/app/file_browser.cpp` | 628 | 结构 + 120–249、279–348、495–609 |
| `src/bento/app/capability.cpp` | 718 | 结构 + 302、356–420 抽样（15 项探测未逐项复核） |
| `src/bento/app/session.cpp` | 1156 | 结构 + 1–130、227–330、330–510、510–760、760–949、950–1156 |
| `src/bento/net/udp_transport.cpp` | 656 | 全文 |
| `src/bento/net/x25519.cpp` | 287 | 全文 |
| `docs/modules/bento-scenarios.md` | 510 | 全文 |
| `cmake/Go2CppBento.cmake` | 438 | 1–180、260–340（其余用 grep 定位） |

**对照物（只读）**：`include/go2cpp/bento/net/{transport,tcp_transport,frame,protocol,channel,log_hook}.hpp`、
`src/bento/net/tcp_transport.cpp`、`tests/bento/*`、`docs/INDEX.md`、`codex.md`、`docs/review/_TEMPLATE.md`。

---

## 2. 可扩展性评估（回答任务四问）

### 2.1 HTTP 客户端可换传输？—— **不能，且是设计上明确放弃**

`Fetch()` 是自由函数（`fetch.hpp:237`），HTTP 客户端全部是 `fetch.cpp` 内匿名命名空间的静态函数
（`ConnectTcp:530`、`SendAll:630`、`ReadHeaders:855`、`HttpGetToFile:1126`），**没有任何注入点**
（无 `Fetcher`/`HttpTransport` 接口、无回调、无 options 里的后端选择）。

- 合理性：符合「Simplicity First」；测试已用进程内 TCP server（`test_fetch.cpp:153 TestHttpServer`）驱动真实路径，
  不需要 mock，因此**缺少注入点并未损害可测性**。
- 真实代价：`options` 是唯一扩展面，无法替换（a）代理支持（b）HTTP/2（c）自定义 header/认证。
  FR-09 若后续出现「企业代理」场景，只能改源码。

**评估**：不构成缺陷，但应在 `fetch.hpp` 补一句「本模块不提供可替换后端；需要代理/自定义认证时由调用方封装」
（当前文件头只解释了「为什么 http 自研、https 调 curl」，没写「不可替换」这一契约）。

### 2.2 zip 可加压缩？—— **结构上可，但当前需改 4 处，且已有 1 个前置条件满足**

| 现状 | 位置 | 加压缩的影响 |
|---|---|---|
| method 字段硬编码 `0` | `zip.cpp:317`（本地头）、`zip.cpp:336`（中央目录） | 需把常量参数化 |
| 读完数据后 **seek 回填** CRC + 压缩大小 + 原始大小 | `zip.cpp:451-455` | **这是好前提**：deflate 也能先写占位再回填，无需改 `kFlagDataDescriptor` |
| 上限检查按**未压缩**大小 | `zip.cpp:423-429` | 加压缩后需区分「压缩后归档大小」与「未压缩总量」两套限额 |
| `Filler` 是私有 `std::function<bool(ostream&, Crc32&)>`（`zip.cpp:370`） | 内部 | 需改为「可插流变换」或新增 `AddFile` 重载 |

读者侧拒绝非 0 method（`zip.cpp:944`），**明确失败不静默跳过**——这一点符合规范，扩展时不会留下静默错误。

**评估**：可扩展性「中」。明确放弃压缩是合理取舍（零依赖），**但需在文档记录性能后果**：
FR-18 备份上传走网络，store-only 使上传字节数 = 原始字节数（桌面+文档可能数百 MB）。
`bento-scenarios.md` 未记录这一带宽代价。

### 2.3 session 可换 transport？—— **可以，但服务端必须外部注入已 accept 的连接**

`SessionConfig::transport_factory`（`session.hpp:222`）+ `OpenTransport()`（`session.cpp:356-377`）
是干净的注入点，测试可注入 `MemoryTransport` 做确定性复现——**这是本批最好的扩展性设计**。

但有一条硬限制（`session.cpp:348-351`）：

```cpp
if (config.server && !config.transport_factory) {
    // ITransport 没有 Listen/Accept；服务端必须注入已接受的连接。
    return FailIdle(SessionError::kTransportUnavailable, ...);
}
```

即 `Session` **不能自己监听**：服务端必须先 `TcpTransport::Listen/Accept`，再把连接塞进 factory。
头文件第 220–221 行已如实说明（「服务端直接报错」）。定位为「会话编排器」可以接受，
但应在文档写明服务端样板代码（当前 `session.hpp` 只给了客户端 `@code` 示例）。

### 2.4 审计 / 日志钩子 —— **app 层完全缺失，与 net 的 `LogHook` 断裂**

- net 已有 `log_hook.hpp:LogHook`（`LogLevel`/`LogEvent`/计数/锁外 sink），`PeerHub::SetLogHook` 已接入。
- **6 个 app 模块没有一个接受日志钩子**：`Fetch`/`Backup`/`Upload`/`Zip`/`FileBrowser` 只在结果结构里回一个
  `std::string message`/`error`；`Session` 只有 `on_state`/`on_error`/`on_data`/`on_control`。
- 后果：**审计维度无法满足**——「谁在什么时候把哪个 URL 下到哪个目录」「哪个 zip 传到了哪个 endpoint、
  带什么 token」这些安全审计事件，调用方只能自己包一层；库里既不打日志也不发事件。
  传输层（net）有统一日志，应用层（app）没有，两层可观测性不对等。

**建议（不要求在本次修）**：给 `FetchOptions`/`UploadOptions`/`SessionConfig` 增加可选的
`bento::LogHook*` 字段，事件至少覆盖「下载开始/成功/失败」「上传尝试/重试/成功」「会话状态迁移」。
或者明确在文档声明「app 层不接日志，审计由调用方负责」——**但当前两头都没写**。

---

## 3. 功能点对照 JinShan（FR-09 / FR-15 / FR-18 / CON-06）

需求原文取自 `JinShanProject/docs/01-requirements/RTM.md:24,49,55,58`。

| 需求 | 验收要求（RTM） | 对应实现 | 状态 | 缺口 |
|---|---|---|---|---|
| **FR-09** 下发「下载并保存」指令 | 被控下载到指定目录并**校验（哈希/可执行）**；大文件不整包塞虚拟通道 | `fetch.cpp:1541 Fetch()`：URL 解析、SHA-256 流式计算与比对（`FetchOptions::expected_sha256`）、64 MiB 默认上限、临时文件原子 rename、重试/退避、重定向、大小预检 | **基本满足** | ① 只校验 SHA-256，「可执行」校验（x 位/ELF 头）无；② 无代理支持；③ **凭证进 argv**（见 D-04） |
| **FR-15** 远程文件访问等价能力 | 目录可浏览；文件可下载到本地缓存并打开；可上传；**等价边界文档化** | `FileBrowser::List/Get/Put/Stat`（`file_browser.cpp:368/511/550/429`）+ 根目录沙箱 + 原子复制 + 4 个便利自由函数 | **满足（实现）** | **「等价边界文档化」没有做**：`bento-scenarios.md` 没有 FR-15 的映射行，`docs/modules/` 也没有 file_browser 文档；「本地缓存目录」概念缺失（Get 的 local_path 由调用方给，库不管缓存） |
| **FR-18** 自动备份并上传 zip | 按配置目录打包；上传成功并**在远端校验**；失败重试并**保留本地包** | `Backup()`（`backup.cpp:490`）：basename 条目名、`.part` → rename、失败保留旧包；`Upload()`（`backup.cpp:652`）：PUT/POST、自定义 header、Bearer token（stdin，不进 argv）、指数退避、只删临时文件 | **大部分满足** | ① **「远端校验」完全缺失**：只判断 2xx，无 HEAD/ETag/回读校验（RTM 明确要求）；② store-only 不压缩（带宽）；③ `.part` 固定名，**并发备份互相踩**（D-06） |
| **CON-06** 差环境可用为先 | **设计文档列出降级策略**；至少一种受限环境有验证记录 | `capability.cpp:Probe()` 15 项只读探测（/proc、epoll、cgroup、rlimit、ifaddrs、statfs、uname） | **半个** | **`Probe()` 没有任何生产消费者**（全仓 grep：src/ 下零引用，只有 `test_capability.cpp` 调用）。即「探测出来以后谁降级」没有答案；文档 11.1/11.2 只写了「全部组件用普通 socket、无特权调用」，不是「降级策略」 |

**额外发现（CON-06 的直接冲突点）**：`fetch` 的 HTTPS 与 `Upload` **整条链路都依赖系统 `curl` 可执行文件**
（`fetch.cpp:1260-1294`、`backup.cpp:351-358`）。在精简容器/PE 类环境里 `curl` 缺失时：
`Fetch` 返回 `kBackendUnavailable`、`Upload` 返回 `kCurlMissing`——**HTTP 明文路径可用、HTTPS 全不可用**。
这是「能启动、能连上」目标下的真实降级断点，但 `bento-scenarios.md` 的 §11（差环境）没有任何一条记录它。

---

## 4. 规范问题

### 4.1 【A-01｜高】文档与代码正面矛盾

`docs/modules/bento-scenarios.md:171`：

> `| 备份打包上传 / AVI 录制 | FR-18、FR-17、RISK-050、RISK-054 | 文件格式与 HTTP 上传属应用层；Bento 不引入 zip/AVI 依赖 |`

而事实是：`src/bento/app/zip.cpp`（1202 行自研 ZIP）、`backup.cpp`（备份+上传）、`fetch.cpp`（1676 行自研 HTTP 客户端）
都已落地，`cmake/Go2CppBento.cmake:77-87` 还专门建了 `go2cpp_bento_app` 目标。

且 `bento-scenarios.md:238-240` 的 §18 开头自称：

> 「按任务边界，改动只落在 `include/go2cpp/bento/net/**`、`src/bento/net/**`、`tests/bento/**`、`cmake/Go2CppBento.cmake`、`docs/modules/bento-scenarios.md`」

——**app 模块的存在与该边界声明直接冲突**，文档没有任何一节记录 batch2 的扩展。

**影响**：任何按此文档评估能力的人（包括后续 agent）都会得出「Bento 没有 zip/HTTP/备份」的错误结论；
§16 的覆盖统计（66 条场景、已有 30）也因此失真。
**建议**：新增 §21「P2 落地（app 层）」，把 6 个模块登记进场景矩阵（FR-09/15/18/CON-06 对应行改为「已补」），
并修正 §14 的「不引入 zip」表述为「已引入 store-only 自研 zip，理由见 §21」。

### 4.2 【A-02｜高】文档索引与变更记录双缺失

1. `docs/INDEX.md:280-282` 的「模块文档」表只登记 `network.md`/`tools.md`/`persistence.md` 三篇；
   `docs/modules/bento-scenarios.md` **不在索引任何位置**（`grep 'bento-scenarios' docs/INDEX.md` 零命中）。
   而 `INDEX.md` 开头自己规定：「本索引是文档的唯一入口；新增/改名文档必须在此登记」。
2. `codex.md`（107 KB，项目既有 AI 协作记录，最后一节是 **2026-10-27 第 26 节 Bento P1**）
   **完全没有** MOD-27/30/31/37/39（fetch/file_browser/backup/zip/capability/session）与
   udp_transport/x25519 的任何记录（`grep -n 'MOD-27\|MOD-30\|MOD-31\|MOD-37\|MOD-39\|bento/app' codex.md` → 零命中）。
3. `docs/` 全目录（`grep -rn 'bento/app\|MOD-27\|...' docs/ --include=*.md`）零命中——**6 个模块在文档体系里不存在**。

按项目规则（`PROJECT_RULES.md` §6）：「按每次重要 git 变动或逻辑变动单元记录，写入项目内的 AI 协作记录文件」。
本批 6 个模块 + 2 个 net 模块属于典型的重要逻辑单元，却漏记。

### 4.3 【A-03｜高】第二套线上协议无文档，且帧类型编号与既有协议重叠

`session.cpp:32-39` 定义了会话协议帧类型：

```cpp
constexpr std::uint16_t kFramePairing = 1;   kFrameHeartbeat = 2;  kFrameData = 3;
kFrameControl = 4;  kFrameClose = 5;  kFrameChannelOpen = 6;
kFrameChannelAck = 7;  kFrameControlRequest = 8;
```

对比 `include/go2cpp/bento/net/protocol.hpp:75-87`：

```cpp
enum class FrameType : std::uint8_t {
    kHello = 0x01, kHelloAck = 0x02, kStream = 0x03, kDatagram = 0x04,
    kVideo = 0x05, kAck = 0x06, kBarrier = 0x07, kMaxData = 0x08, ...
```

两者都写进同一个 `net::Frame.type`（`frame.hpp:90`，uint16），**magic/version 相同**（`kFrameMagic`/`kFrameVersion`），
编号 1..8 **语义完全不同**。后果：

- 同一个 `FrameDecoder` 无法区分「这是 Link 协议还是 Session 协议」，混接端口时错误会被解释成另一种协议的错误帧；
- `tests/bento/PROTOCOL.md`（680 行，Link 协议的 v2 线格式权威文档）**完全没有 Session 协议的章节**；
- `session.hpp:42-53` 只在自己的注释里列了一张表，等于**线格式文档写在头文件注释里**，与 net 的做法不一致。

**建议**：二选一——（a）给 Session 分配独立 `version` 或 `flags` 位标识协议（改动线格式，需评审）；
（b）至少在 `PROTOCOL.md` 增补「§12 会话层协议（app::Session）」并声明「与 Link 协议不互通，靠端口隔离」。

### 4.4 【A-04｜中】`bento-scenarios.md` 自检命令与当前代码脱节

`bento-scenarios.md:218-222` 给出的复核命令只统计场景行数与状态分布，**不校验「引用的文件/接口是否真实存在」**。
自检清单第 3 条「每行的 Go2Cpp 能力都指向真实文件与接口」是人工承诺，没有机械校验手段；
`bento_test_*` 目标名同样没有纳入自检（本批新增 5 个 app 测试目标，文档未提及）。

### 4.5 【A-05｜中】示例可跑性未验证，且缺链接项风险

`bento-scenarios.md:433` 给出的编译命令：

```bash
g++ -std=c++17 -I include /tmp/bento_wiring.cpp build-review/libgo2cpp_bento.a -lpthread -o /tmp/bento_wiring
```

- 示例代码用到的 `PeerHub`/`SessionControl` 确实在 `go2cpp_bento` 目标内（`cmake:43-44`），`LogHook` 是 header-only，**这部分成立**；
- 但 `go2cpp_bento` 在完整构建下 `PUBLIC` 链接 `go2cpp_hash`（`cmake:66-72`），而 `build-review/` 里
  **只有 5 个 bento 静态库、没有 `libgo2cpp_hash.a`**（`ls build-review/*.a`）。若该目录是 standalone 模式
  （crc32 源文件已并入），命令可行；若是完整模式，示例**缺 `libgo2cpp_hash.a`** 会 undefined reference。
- **本次未实际编译该示例**，无法断言「可跑」。按项目规则（§0.1「不许声称未经验证的事」），
  文档应标注「示例在 X 环境下验证通过」或直接把示例纳入 ctest（例如 `bento_example_wiring` 目标）。

### 4.6 【A-06｜低】中文注释质量（正面）

抽查的 8 个文件均满足项目要求：文件头有 `@file/@brief/@details/@author/@date/@copyright`、
设计取舍写「为什么」（如 `fetch.hpp:24-31` 解释为何 http 自研 / https 调 curl、
`zip.cpp:5-20` 解释为何固定头长并 seek 回填）、协议字段与魔法数逐项注释
（`zip.cpp:52-67`）、枚举成员逐项注释（`fetch.hpp:127-145`、`zip.hpp:81-94`）。**这一维度优于当前 net 平均水平**。

---

## 5. API 一致性清单（与既有 bento/net 风格对比）

### 5.1 一致的部分（值得保持）

- **常量命名**：`kXxxDefaultYyy` / `kXxxMaxYyy` 与 `net/congestion.hpp`、`channel.hpp` 一致（`fetch.hpp:88-114`、`zip.hpp:64-72`）。
- **状态码 + 转文本函数**：`XxxStatus` / `XxxStatusToString` 与 `FrameStatus/FrameStatusName`、`Code/CodeName` 同构
  （`zip.hpp:97`、`backup.hpp:66,117,133`、`file_browser.hpp:113`）。
- **PIMPL + 删除拷贝**：`ZipWriter`（`zip.hpp:234-237`）、`Session`（`session.hpp:351-353`）与 `Link`/`PeerHub` 一致。
- **结果结构 + `bool ok()`**：`FetchResult`/`ZipResult`/`BackupResult`/`UploadResult` 与 `net::Result` 同构。
- **上限防护 + 拒绝而非修正**：与 `channel.hpp` 的 `kMaxStreamMessageBytes` 防御思路一致。
- **UDP 与 TCP 传输的对齐**：`udp_transport.cpp` 的 `Resolve`（EHOSTUNREACH 统一）、`Close` 幂等、`ShutdownWrite` 报错语义
  与 `tcp_transport.cpp` 逐项一致（对比 `tcp_transport.cpp:84,101,453,477`），这是本批最规范的部分。

### 5.2 不一致清单

| ID | 位置 | 不一致事实 | 影响 / 建议 |
|---|---|---|---|
| **C-01** | `fetch.hpp:81`、`zip.hpp:57`、`backup.hpp:49`、`file_browser.hpp:66` 用 `namespace go2cpp::bento`；`capability.hpp:66`、`session.hpp:105` 用 `go2cpp::bento::app` | app 目录内**两套命名空间并存** | `using namespace go2cpp::bento;` 后 `Probe`/`Session` 仍需 `app::` 限定；反之亦然。建议统一到 `go2cpp::bento::app`（需一次机械重命名 + 更新 6 个测试） |
| **C-02** | `fetch.cpp:1444-1474` 返回 `"invalid_argument"`（下划线）；`backup.cpp:632-647`、`zip.cpp:793-800` 返回 `"invalid-argument"`（连字符） | 同一仓库**两种状态名风格**，且 `net` 的 `CodeName`/`FrameStatusName` 各成一套 | 状态名是日志/指标标签（`capability.hpp:75` 明说「可作为指标标签」），风格分裂会让仪表盘出现两种 label。建议统一为下划线（与 C++ 标识符一致） |
| **C-03** | `file_browser.hpp:221-239` 在 `go2cpp::bento` 根命名空间定义自由函数 `List`、`Get`、`Put` | 三个**极通用的名字**进了公共命名空间 | 与未来任何 `List`/`Get`/`Put` 冲突；`using namespace go2cpp::bento` 的用户会被污染。建议改为 `ListFiles`/`GetFile`/`PutFile`，或移入 `app::file_browser` 子命名空间 |
| **C-04** | `fetch.hpp:151 FetchOptions` 无文件名字段；但 `ResolveFetchPath` 公开接受 `max_path_bytes`（`fetch.hpp:209`），而 `Fetch()` 内部**硬编码** `kFetchMaxFilenameBytes`（`fetch.cpp:1601`） | 公开 API 可收紧、`Fetch()` 不可 | 调用方无法通过 options 改文件名上限；建议加 `FetchOptions::max_filename_bytes` 或在文档声明「固定 200B」 |
| **C-05** | `fetch.cpp:1393 SleepBeforeRetry`（封顶 2000ms）与 `backup.cpp:453 SleepBeforeRetry`（封顶 30s，`kMaxRetryDelay`） | **同名函数、同名语义、不同参数与上限**，各写一份 | 后续调退避策略要改两处且容易只改一处。建议抽 `app/retry.hpp`（或复用 `net/backoff.hpp`——它已有 `Backoff` 实现） |
| **C-06** | `fetch.cpp:1260-1294`（argv 传 URL）与 `backup.cpp:249-297`（`--config -` 走 stdin） | 同为调 curl，**凭证处理策略相反**：备份刻意不让 token 进 argv，fetch 把完整 URL（可能含预签名 query/token）放进 argv | 同一模块族内部安全策略不一致，见 D-04。建议 fetch 也走 `--config -` |
| **C-07** | `backup.hpp:127` `UploadStatus::kRetryExhausted` 标注「预留」，`backup.cpp:692-698` 实现**永不返回**该值 | 死枚举 | 调用方若写 `case kRetryExhausted:` 永远走不到。建议删除该枚举值（诚实 API 优于预留） |
| **C-08** | `capability.hpp` 无「Ceiling」常量，`file_browser.cpp:286` 用魔法数 `10 * 1000 * 1000` 校验 `max_entries` | 与 `fetch.hpp:90 kFetchMaxBytesCeiling`、`zip.hpp:66 kZipMaxEntriesCeiling` 的「显式硬上限常量」惯例不符 | 建议补 `kFileBrowserMaxEntriesCeiling` 并在头文件注释中出现 |
| **C-09** | `file_browser.hpp:133` 注释「1 .. 10000000」与 `file_browser.cpp:286` 硬编码数字 | 注释与代码各说一遍，无常量名 | 改一处忘另一处；应统一到常量 |
| **C-10** | `session.hpp:117` `SessionStateName` 返回 `"idle"`；`capability.hpp:75` `CapabilityStatusName` 返回 `"available"`（纯小写单词）；而其余 `*StatusToString` 返回带分隔符的复合名 | 三套命名函数（`...Name` vs `...ToString`）并存 | 建议约定：枚举值名用 `XxxName`，错误/结果显示用 `XxxStatusToString`，并在 `extensibility.md` 记录该约定 |
| **C-11** | `session.hpp:222` `transport_factory` 返回 `std::unique_ptr<ITransport>`；但 `SessionConfig::now` 用 `std::function`（`session.hpp:224`），`LogHook` 用 `Sink` 回调，`PeerHub` 用公有成员回调 | 注入风格四种并存（工厂 / function / 回调成员 / 指针） | 属于既有风格延续，**不建议本批改**；但应在 `extensibility.md` 记录「Bento 的注入点选择原则」 |
| **C-12** | `udp_transport.hpp:153-154` 声称「零长度 UDP 数据报返回 kOk 且 bytes==0，这是与 ITransport 文档中『成功读取至少 1 字节』的唯一例外」 | `transport.hpp:175` 本身已写明「capacity == 0 是合法的空操作，返回 kOk 且 bytes == 0」 | 「唯一例外」表述**不准确**：`capacity==0` 已在接口契约内。建议改为「零长度数据报是 Read 返回 0 字节的第二种情形」 |

---

## 6. 缺陷清单（按严重度）

> 说明：以下均为**静态审查推断**，未构造复现用例；标注「推断」的条目需要在修复前先写失败测试。

| ID | 严重度 | 位置 | 问题 | 证据 / 推断路径 | 建议 |
|---|---|---|---|---|---|
| **D-01** | P1 | `session.cpp:600-630` | `HandleControl` 收到 `Control(GRANT, peer_id, mode)` 时**不校验 `peer_id` 是否属于本会话**（未比对 `remote_peer_id`/`local_peer_id`），直接 `control->Grant(peer_id, mode)` | 第 612 行只校验 `peer_id != 0`；第 621-623 行直接 Grant。任意对端可把控制权授予会话外的 peer id | 白名单校验：`peer_id == remote_peer_id \|\| peer_id == config.local_peer_id`，否则 `ProtocolFail` |
| **D-02** | P1 | `session.cpp:719`、`session.cpp:989` | 控制权请求成功后**硬编码广播 `ControlMode::kInteract`**，与 `SessionControlConfig` 的策略/默认模式无关 | 两处 `BroadcastControl(kControlGrant, peer_id, ControlMode::kInteract)` | 应从 `control->Snapshot()` 取实际授予的模式，或给 `SessionConfig` 加「请求时申请的模式」 |
| **D-03** | P1 | `fetch.cpp:1250-1258` | HTTPS 分支**先 `O_CREAT\|O_EXCL\|O_NOFOLLOW` 建临时文件，随后 `CloseFd` 让 curl 按路径重新打开**——`fetch.hpp:40-41` 声称的「避免预置符号链接把写重定向到沙箱外」在 HTTPS 路径上不成立 | `OpenTempFile`（1095）→ `CloseFd(&fd)`（1258）→ curl `--output temp_path`（1281）。窗口内替换为 symlink 即逃逸，且后续 `stat`（1347）+`HashFile`（1357）读的也是同路径 | HTTPS 改为父进程持 fd（`curl --output /dev/fd/N` 或读回 stdout 由父进程写）；否则文档不能声称该保护覆盖 HTTPS 路径 |
| **D-04** | P1 | `fetch.cpp:1284` | 完整 URL 进 `curl` argv（`BuildUrlText(url)`），可被同机任意用户从 `/proc/<pid>/cmdline` 读到 | 对比 `backup.hpp:18-20` 明确写「token 通过 stdin 传给 curl，不进入进程参数」——同一代码库相反做法 | 与 backup 统一：`curl --config -`，URL 写进 config |
| **D-05** | P2 | `file_browser.cpp:536-547`（Get）、`573-583`（Put） | `overwrite=false` 的「目标已存在」检查与 `CopyAtomically` 的 `rename()` 之间存在 **TOCTOU**：检查通过后目标被创建，`rename` 仍会覆盖 | `rename`（`file_browser.cpp:221`）无 `RENAME_NOREPLACE` | 用 `renameat2(..., RENAME_NOREPLACE)`（Linux ≥3.15）或 `link()+unlink()`；文档同步说明 |
| **D-06** | P2 | `backup.cpp:519` | 临时包名固定为 `out_zip + ".part"`，**不是 pid/序号唯一** | 对比 `fetch.cpp:1107 MakeTempPath`（pid+seq）、`file_browser.cpp:149`（pid+seq）——同批其他模块都做了唯一化 | 改成 `.part.<pid>.<seq>`；或文档声明「同一 out_zip 不支持并发 Backup」 |
| **D-07** | P2 | `session.cpp:880-896`、`829-843` | `SendFrame` 队列满时返回 `false`，但**内部帧的调用方忽略返回值**：`SendChannelAck:833`、`BroadcastControl:842` 未检查（`SendCloseFrame:741` 有检查） | 队列满 → ACK 静默丢弃 → 通道握手可能卡到超时；`stats` 无 drop 计数，`SessionError::kQueueFull` 只对 `Send()` 暴露 | `SendFrame` 返回 `SessionError`，或至少 `++stats.dropped_frames` |
| **D-08** | P2 | `session.cpp:927-948` | `Flush()` 每帧用 `io_timeout_ms`（默认 50ms），outbox 上限 256 → 单次 `Poll` 最坏阻塞约 12.8s，**远超调用方传入的 `timeout_ms`** | `Poll(timeout_ms)` 只把 budget 交给 `Read`（`session.cpp:453-454`） | 文档明确「Poll 最坏阻塞 = timeout_ms + outbox×io_timeout_ms」；或给 Flush 加总预算 |
| **D-09** | P2 | `session.cpp:576-598`、`session.hpp:160-169` | `HandleData` 只接受 `channel_id == config.channel_id`，**Session 是单通道实现** | `session.hpp:160` 注释已如实写「当前实现为单通道」 | 可接受；但 FR-06 多通道场景若要用 Session 需要扩展，建议在 `bento-scenarios.md` 标注 |
| **D-10** | P3 | `fetch.cpp:1630-1635`、`1664` | SHA-256 不匹配时 `result.sha256` 残留「实际值」但 `status != kOk`；`result.path` 已清空 | 头文件只说「成功时必有」 | 现状可接受，文档补一句「失败时 `sha256` 可能是本次尝试的实际值」 |
| **D-11** | P3 | `backup.cpp:493` | `Backup()` **一进函数就** `result.archive_path = out_zip`，失败时也非空 | `backup.hpp:101` 说「成功时 `archive_path` 有效」 | 失败时清空，或头文件改为「始终回显输入路径」 |
| **D-12** | P3 | `zip.cpp:524-529`、`770-779` | `ZipWriter::Open` 在已 opened 时先 `Abort()`——会**删除上一个输出路径的文件**（不是新路径） | `Abort` 无条件 `fs::remove(output_path)` | 语义应文档化：「Open 会先中止并删除上一份输出」 |
| **D-13** | P3 | `backup.cpp:670-690` | `Upload` 对 **4xx（400/401/403）也重试**（只有 curl 缺失/本地错误才提前返回），与 `fetch` 的 `IsRetryable`（`fetch.cpp:1375-1390`，4xx 不重试）策略相反 | 注释声明「非 2xx 都重试」是有意为之 | 两模块策略应统一或在文档对比说明（token 错误重试 4 次纯属浪费） |

### 6.1 `x25519` / `udp_transport` 专项

| ID | 严重度 | 位置 | 问题 |
|---|---|---|---|
| **D-14** | P2（规范） | `x25519.cpp:12-15` | 实现实质是 **TweetNaCl 的移植**（`Carry`/`Mul`/`Invert`/`Pack`/`Select` 与 TweetNaCl 的 `car25519`/`M`/`inv25519`/`pack25519`/`sel25519` 逐行同构，作者原文也写「采用 TweetNaCl 的 carry 技巧」），但**没有按 `PROJECT_RULES.md` §2「移植规范」记录来源**：缺仓库/版本/许可证（public domain）/移植日期/改动点。算法正确性已由 RFC 7748 向量验证（`test_x25519` 1160 checks PASS），**问题只在出处合规** |
| **D-15** | P3 | `x25519.cpp:244-253` | `FillRandom` 每次调用构造 `std::random_device`；libstdc++ 上读 `/dev/urandom`（安全），但该类型在部分实现上是确定性伪随机。若项目已有 `os::random`/`crypto.hpp` 的随机源，应统一（**本次未核实是否存在**） |
| **D-16** | P3 | `udp_transport.cpp:74-81` | `getaddrinfo` 失败统一映射 `EHOSTUNREACH`，丢失 `EAI_NONAME`/`EAI_AGAIN` 区分；与 `tcp_transport.cpp:84` 完全一致（**属既有约定，不算新缺陷**），但 `fetch` 有 `kDnsError` 细分，跨层可观测性不对等 |

---

## 7. 测试与验证现状（如实记录）

| 项 | 事实 | 判定 |
|---|---|---|
| 既有 `LastTest.log` | 2026-10-28 12:51，**只含 9 个测试**：`bento_zip`/`backup`/`upload`/`fetch`/`file_browser`/`capability`/`session`/`udp_transport`/`x25519` 等，编号从 19/59 起 | 逐项 `Test Passed`，但**不是全量 59/59**；`codex.md` 末节自述「未全量复跑 ctest」与之吻合 |
| checks 数 | zip 245 / backup 85 / upload 97 / fetch 138 / file_browser 1881 / capability 124 / session 180 / udp 158 / x25519 1160 | 覆盖密度尚可；**fetch 1676 行源码仅 138 checks**，相对偏薄 |
| 真实服务端 | `test_fetch.cpp:153`、`test_upload.cpp:142` 自带 `TestHttpServer`（socket + 线程）驱动真实 HTTP 路径 | 符合「真实场景模拟」要求（非 mock） |
| 压测 | `bento_backup` 输出 `[perf] backup 16MiB: 0.032s, 497.1 MiB/s` | 有单点性能数据；**无并发/长跑/随机压力**（对照 `PROJECT_RULES` §3，app 层不足） |
| 未覆盖 | `capability` 只测了注入假路径的单元场景（247 行），没有「真实受限环境」验证记录 | CON-06 的「至少一种受限环境有验证记录」验收项**未满足** |

---

## 8. 未验证 / 无法确认项（不作结论）

1. **示例可跑性**：`bento-scenarios.md:433` 的编译命令未实际执行（本次只读审查，不构建）。
   `build-review/` 缺 `libgo2cpp_hash.a`，链接是否失败取决于构建模式，**未验证**。
2. **`go2cpp_bento_app` 的 standalone 降级**：`cmake:80-102` 只在 `TARGET go2cpp_os AND go2cpp_hash` 时创建
   `go2cpp_bento_app`，否则只打一句 STATUS。而 app 层的 zip/fetch/backup **在 standalone 下不可用**；
   该限制是否与 §20 宣称的「Bento 可单独拷进其他工程」冲突，**文档中未找到说明**（应属 A-01 同类问题）。
3. **`capability.cpp` 15 项探测实现质量**：只抽读 epoll/cgroup-memory/cgroup-cpu 三段（302、356–420），
   rlimit/ifaddrs/statfs/uname 未逐项复核。
4. **`go2cpp::os::Run` 的 stdin 传递**（`backup.cpp:358`）是否可能因管道缓冲（64KB config）阻塞，未验证。
5. **并发安全**：`FileBrowser::Get/Put` 的 TOCTOU（D-05）、`Backup` 的 `.part` 竞争（D-06）均为静态推断，
   未构造复现用例。
6. **Session 与 Link 混接**（A-03）的实际错误表现未实测。
7. **本轮未运行任何测试/构建**，所有 PASS 结论均来自既有 `LastTest.log` 的转述。

---

## 9. 建议修复顺序

| 顺序 | 项 | 工作量 | 理由 |
|---|---|---|---|
| 1 | **A-01** 修正 `bento-scenarios.md:171` + 补 §21 落地记录 | 0.5d | 文档假陈述优先级最高；成本最低 |
| 2 | **A-02** 登记 `docs/INDEX.md` + 补 `codex.md` 变更记录 | 0.5d | 项目规则硬要求；后续审查的基线 |
| 3 | **D-03/D-04** fetch 的 HTTPS 临时文件与 argv 凭证 | 1d | 安全；改动集中在 `PerformHttpsAttempt` |
| 4 | **D-01/D-02** Session 的 Control 帧校验与模式硬编码 | 0.5d | 协议正确性；改动局部 |
| 5 | **A-03** 补 `PROTOCOL.md` 会话层章节 | 1d | 不补则第二套协议永远无权威定义 |
| 6 | **C-01** 统一命名空间到 `bento::app` | 1d | 机械改动，但越晚改成本越高 |
| 7 | **C-02/C-03/C-05/C-07** 状态名/自由函数/重复退避/死枚举 | 1.5d | 公共 API 面收敛 |
| 8 | **D-05/D-06/D-07** TOCTOU、`.part` 竞争、静默丢帧 | 1.5d | 需先写失败测试 |
| 9 | FR-18「远端校验」、CON-06「降级策略落地」、app 层日志钩子 | 2–3d | 属功能补齐，建议单独立项 |

---

## 10. 变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-10-28 | v1.0 | 首次审查：覆盖 `bento/app` 6 模块 + `udp_transport` + `x25519` + `bento-scenarios.md`；输出 4 维评估、12 条一致性问题（C-01~C-12）、16 条缺陷（D-01~D-16）、6 条规范缺失（A-01~A-06）；未改代码、未运行测试 |
