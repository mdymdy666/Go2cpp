# 三件套覆盖盘点：哪些模块/实现还没做到「架构 + 汇编 + 正确性」

- 日期：2026-11-04（Asia/Shanghai）
- 工作树：WSL `/UserData/CodexWorkSpace/Go2cpp`（**真树**；E: 副本未读取、未参考）
- 性质：**只读盘点**，未改代码、未跑基准、未 commit。所有结论来自仓库内文件/提交，不引用外部记忆。
- 输入：`docs/metrics-ledger.md`（19 条 M01–M16）、`docs/reports/asm-*.md`（18 份）、
  `docs/reports/2026-11-0*.md` 中含 objdump 的 5 份、`git log --grep` 的 30 条、`src/*` 与 `tests/*` 目录。

## 0. 判定口径（先说清「完整」是什么意思）

三件套 = **架构优化**（锁/所有权/数据结构/算法/内存布局）+ **汇编级证据**（objdump / callgrind /
指令·call·栈帧·原子计数）+ **正确性**（ctest 或等价断言集，≥ 功能测试，最好含 1e5–1e6 压测）。

| 标记 | 含义 |
|---|---|
| ✅ 完整 | 三件齐备，且有**归档在仓库**的报告/台账可复核 |
| 🟡 部分 | 缺一件：常见是「有架构+吞吐、无指令级取证」，或「汇编做了一半又回滚」 |
| ❌ 缺失 | 无 asm 报告、无架构记录；最多只有功能测试或零散 bench |
| ⚪ 不适用 | 控制面/基础设施，不做数据面下限（如 SDP 信令、error/core） |

**汇编级证据的最低门槛**：报告里能查到「函数名 + 指令数/栈帧/call/原子」或 objdump/callgrind 命令输出。
只有吞吐数字（MiB/s、rps）**不算**汇编级证据——那是端到端性能，不是指令级。

---

## 1. 覆盖总表（44 个模块/实现）

| # | 模块/实现 | 架构优化 | 汇编级证据 | 正确性测试 | 结论 |
|---|---|---|---|---|---|
| A01 | container（Heap/List/Ring/Element） | 有：两段式建环、句柄语义（asm-base §2） | 有：objdump×4，Heap 1.028–1.035× | 1e6 push/pop 对拍 | 🟡 List 1.047–1.159×、Element 1.46–1.49×、Ring 1.18–1.57× 未达标（`asm-base.md:20-23`） |
| A02 | math（Sqrt/Sin/Exp/Modf） | 有：内联与分派改写 | 有：LTO 对照实验，证明是跨 TU 边界代价 | 1e6 对拍 libm | 🟡 Sqrt 1.388–1.409×、Modf 1.322–1.381× 未达标（`asm-base.md:24-28,34`） |
| A03 | metrics（Registry/Render） | 有 | 有：objdump 热路径 | 渲染结果对拍 | ✅ 24.3–26.5 ns/指标、Registry 71.2–88.7 ns（`asm-base.md:29-30`） |
| A04 | path | 有：批量化与零分配拼接 | 有：objdump 统计 | 1e6 对拍 | ✅ 快 `std::filesystem` 3–9×（`asm-base.md:34`） |
| A05 | compress / gzip | 有：64 KiB 输出块 + CRC 合并 | ❌ **无**：`asm-compress.md` 全文 0 次 objdump/反汇编/指令数 | 吞吐 + zlib 同流对照 | 🟡 解压 5219.7 MiB/s、0.696× zlib（`asm-compress.md:14-19`），但**缺指令级取证** |
| A06 | reflect（二进制/文本） | 有：紧凑格式 + 不建树直读 | ❌ **无**：`asm-reflect.md` 0 次 objdump（只有 ns/op 分阶段） | 四项对拍 | 🟡 编码 51.4–55.9 ns、解码 51.5–54.5 ns，全项 <1.1×（`asm-reflect.md:16-22`）但无指令证据 |
| A07 | encoding / base64 | 有：数据布局 + 预置表 | 🟡 弱：`asm-tools.md` 仅 1 处 objdump 引用 | 16 MiB 对拍 Go | 🟡 1980 MiB/s、0.84× Go（`asm-tools.md:14-16`） |
| A08 | encoding / csv+hex+binary | ⚪ 不适用：csv 无生产调用点，hex 只处理短串（§A08） | ⚪ 不适用 | 仅 `tests/encoding/` 功能测试 + 1 MiB stress | ⚪ 不适用（无数据面调用） |
| A09 | hash（SHA-256/CRC32） | 有：SHA-NI、查表 CRC | 🟡 硬件指令已用，但**无归档反汇编**（`asm-reuse-hash.md` 0 次 objdump） | `tests/hash/bench_crc32_hw.cpp` | 🟡 SHA-256 2002.60 MiB/s、crc32 18731.55 MiB/s（`metrics-ledger.md` M12） |
| A10 | os / file（读写、writev/readv） | 有：相邻 iovec 合并、内联 syscall | 有：`Read` 46→38 指令 / 0 call / 0 B 栈帧（`asm-os.md:31`） | 1e6 逐字节 + 边界 | ✅ 写 109.72×、读 207.49×（`metrics-ledger.md` M10）；`asm-os.md:18-22` |
| A11 | os / proc（Process spawn+wait） | 有：去掉重复 /proc 读取 | 有：objdump 统计 | 同进程 A/B | 🟡 1.13–1.22× 未达标（`asm-os.md:24`） |
| A12 | os / shm+mq | 有 | 有：objdump 统计 | 功能 + 压测 | ✅ 并入 `asm-os.md` §4–5 |
| A13 | storage / sqlite 封装 | 有：`const Error&` 返回、语句复用 | 有：objdump + callgrind（`asm-storage-sqlite.md` 18 处） | 7 场景 × 7 轮 | ✅ 1e6 插入 1.074×（`asm-storage-sqlite.md:8-10`；M06） |
| A14 | storage / kv（LSM 引擎） | 有 | 有：objdump（`asm-storage-kv.md` 2 处） | 1e7 压测 | 🟡 Put 562,669 ops/s、Get 475,125（读 0.48× 于 LevelDB，`asm-storage-kv.md:15-18`） |
| A15 | storage / features（Queue/Watch/MemCache） | 有：复用 WAL/SSTable | 🟡 弱：仅 2 处 objdump 引用 | 三套功能测试 | 🟡 `asm-storage-features.md:12-22` |
| A16 | storage / leveldb 封装 | 有：pimpl + `string_view` 零拷贝 + relaxed 统计 | 有：objdump，Put 173/热 54、Get 197/热 69、0 分配 0 锁（`asm-storage-leveldb.md`） | `tests/storage/test_leveldb_kv.cpp`（1e7/1e8） | 🟡 汇编补齐；关 canary 后净增 5 条，缺独立端到端对标 |
| A17 | network / http 服务端（路由/静态/pipelining） | 有：每方法哈希 + 段 trie、sendfile、pipelining | ❌ **无**：`asm-http-e2e.md` 0 次 objdump | 260 条 socket 断言 | 🟡 nginx 1.03–1.89×、Go 1.28–3.21×（`asm-http-e2e.md:16-21`）；M05 120,755 rps |
| A18 | network / http parser + ws | 有：WS 掩码 PRNG、ResponseWriter | 有：审查 objdump 20 点（`2026-11-01-asm-network-review.md:13-17`）+ `f40e007` | 回归测试 | 🟡 WS 长连接 RSS 2.20–2.32×、p99 2.06–3.06× 于 Go（`ws-200k-bench.md:20-21`） |
| A19 | network / tls | 有 | 有：objdump 9 处 / 20 行（`asm-tls.md`） | 对拍 + 吞吐 | 🟡 握手 1.061×、64B 0.945 达标；**大包 AEAD 13.41×**（`asm-tls.md:29,240`） |
| A20 | network / bufio | ❌ 无报告 | ❌ 无 | `tests/bufio/bench_bufio_batch.cpp` | 🟡 有 bench 无优化记录（M08 1853.9 MiB/s 无对标），§A20 判定「要补（中）」 |
| A21 | bento / link（多队列） | 有：SO_REUSEPORT + recvmmsg 批收 | ❌ 无：`asm-legacy.md` 0 次 objdump | 1e6 包 | 🟡 单 peer 无收益（−1.3%）、2 核多队列丢包 39–48%（`asm-legacy.md:17`；M15） |
| A22 | bento / peer_hub FEC | 有：FEC 接入 | ❌ 无 | 1e6 包 + 丢包注入 | 🟡 重传 17→6（−64.7%）（`asm-legacy.md:18`） |
| A23 | p2p / stun+relay+random | 有：`06f9e59`（Relay 3.3×/STUN 5–10×） | 🟡 提交信息有数字，**无报告归档** | `tests/p2p/bench_*` 3 个 | 🟡 只能引用提交号，无 objdump 表 |
| A24 | rpc | 有：分层编解码（`docs/modules/rpc.md`） | 有：`docs/modules/rpc.md` 的 objdump 表（待归档 `asm-rpc.md`） | `tests/rpc/bench_rpc.cpp` 1e6/1e7 | ✅ 判定段「已具备，只需归档」（§A24） |
| A25 | tunnel | ❌ 无报告 | ❌ 无 | `tests/tunnel/test_tunnel.cpp` 功能 | ❌ 无吞吐程序（M14）；§A25 判定「要补（高）」 |
| A26 | rd_net（v3） | 有：分层单向、零拷贝反证、sendmmsg/writev | 有：`SendBatch` 3107 指令、`EventLoop::Run` 栈帧 152 B（`rdnet-v3-review.md:47-58`） | 406/44/64/44 全绿 + 1e6 | ✅ TCP 批量 20.8×、UDP 1.20×（`rdnet-v3-review.md:71-87`） |
| A27 | concurrency / channel | 有：Context 语义 + 错误路径改造 | 有：`EmptyTryRecv` 两次 `_M_release` 消除（M01b-a） | channel/select/fiber 4 套 + 89.3 s 压测 | 🟡 112.6 ns vs Go 23.36（语义增强），无锁化未做（M01b-a） |
| A28 | concurrency / scheduler | 有：P19–P22 四项（3 项保留） | 有：enqueue 597→580 指令、栈帧 104→88 B（M03） | ctest + smoke 20/20 + 1e6 | 🟡 **做了但未达标**（M03 2.21×，2026-11-03 用户决定关闭） |
| A29 | concurrency / fiber+timer+park | 🟡 无独立报告 | 🟡 仅 bench（M02 19.84 ns） | 测试存在 | 🟡 三件套未成文 |
| A30 | concurrency / sync（mutex/cv/atomic） | 有：`158abb3` 7 项 | 有：提交含栈/指令数字（696→104 B） | 并发压测 | ✅（但证据在提交信息，未成报告） |
| A31 | debug / 事件环 + replay | 有：分片计数、OFF 零开销 | 有：`Record` 剩 `endbr64;ret`、emit 171 指令/12 call/1 `lock addq`（`asm-debug-replay.md:24-40`） | 1e6 计数守恒 | ✅ ON_FULL 3.6–40.3× → 1.44–8.76× |
| A32 | logging | 有：格式化与堆分配削减 | 🟡 提交 `9ce8d4b` 有数字（787→139 ns、5→1 分配），**无报告** | 1e6 逐字节 + ASan | 🟡 证据未归档到 docs |
| A33 | media / audio | 有：SIMD µ-law/重采样 | 有：objdump（`asm-media-audio.md` 4 处） | 409,205 断言 | ✅ 1.006×/1.037×、重采样 0.098×、栈帧 1304→8 B |
| A34 | media / capture+render | 有：XShm 零拷贝、SSE2 行拷贝 | 有：objdump×5 + 前后对比 | ctest 12/12 + 1e5 帧 | ✅ 采集 0.965×、渲染 0.66×、vs FFmpeg 1.007×（`asm-media-capture-render.md:17-23`） |
| A35 | media / sfu | 🟡 审查级（最小解码改法已实现） | 有：345→347 静态、27.63 万→26.30 万动态指令/千帧（`asm-sfu.md:41-52`） | 1e6 帧转发 | 🟡 有取证、无「优化前→后」端到端口径 |
| A36 | media / signaling（SDP） | ⚪ 控制面 | 有：审查级反汇编 | 1e4 往返 | ⚪ 报告自述控制面，不做数据面下限（`asm-signaling.md:9-18`） |
| A37 | distributed（lease/lock/election） | 有 | 有：291 指令/29 call/296 B/0 原子 + callgrind（`asm-distributed.md:13-20`） | ctest 8/8 + 1e6 STRESS_OK | ✅ renew 65 ns、etcd 本地 5000–6200×（`asm-distributed.md:21-32`） |
| A38 | app（gateway/stream_table/kv） | 有：`de916bc` | 🟡 提交有数字（PickNext 20.6×、帧解码 98→4.7 ns），无报告 | app 测试 | 🟡 证据未归档 |
| A39 | cache（ARC/LRU/2Q/CLOCK） | 有：侵入式链表 + 开放寻址 + 64 节点块池（`057257b`） | 有：objdump，Get 73/306/373、Put 227/251/420、0 锁（`asm-cache.md`） | 命中率逐位一致 + `cache_bench` | ✅ 三条齐备（`asm-cache.md`） |
| A40 | relay | 有：并入 `06f9e59` | 🟡 同上 | `tests/relay/bench_relay.cpp` | 🟡 无独立报告 |
| A41 | frame（帧编解码） | ❌ 无报告 | ❌ 无 | `tests/frame/` + 1e6 帧压测（`frame-codec.md:215-222`） | 🟡 有压测，§A41 判定「要补（低）」 |
| A42 | config | ⚪ 不适用：控制面 | ⚪ 不适用 | `tests/config/` 5 个功能测试 | ⚪ 不适用（无数据面基准） |
| A43 | tools（Base64/gzip/JSON 工具） | 有 | 🟡 `asm-tools.md` 仅 1 处 objdump | 5/5 测试 | 🟡 7 达标 3 未达标（`asm-tools.md:25`） |
| A44 | core / error / go（基础设施） | ⚪ | ⚪ | error 5 套 ctest | ⚪ 不适用（无数据面热路径） |

**计数（与上表逐行核对）：44 项 / ✅ 13 / 🟡 26 / ❌ 1 / ⚪ 4（2026-11-04 落地缺失项归属判定 + A16/A39 更新）。**

---

## 2. 已完整的清单（11 项正文证据；A24/A39 证据见表注与本日报告）

1. **os / file**——`asm-os.md:18-33`：单条 read 1.01–1.03×、write 1.00–1.01×；writev/readv 架构合并后
   写 17.9×→109.72×、读 20.9×→207.49×（`metrics-ledger.md` M10）；`File::Read` 46→38 指令、栈帧 24→0 B。
2. **os / shm+mq**——`asm-os.md` §4–5，objdump 统计 + 压测。
3. **metrics**——`asm-base.md:29-30`：Prometheus 渲染 24.3–26.5 ns/指标、Registry 71.2–88.7 ns。
4. **path**——`asm-base.md:34`：快 `std::filesystem` 3–9×，objdump 统计齐。
5. **storage / sqlite**——`asm-storage-sqlite.md:8-10`：1e6 插入 **1.074×**，7 场景 6 个 <1.1×，objdump+callgrind 双证。
6. **rd_net v3**——`rdnet-v3-review.md:47-87`：`SendBatch` 3107 指令、`EventLoop::Run` 栈帧 152 B；
   四套 406/44/64/44 全绿；TCP 批量 20.8×。
7. **distributed**——`asm-distributed.md:13-32`：`RenewLease` 291 指令 / 29 call / 296 B / 热路径 0 `lock` 前缀；
   callgrind renew 837 Ir；ctest 8/8 + 1e6 `STRESS_OK`。`2026-11-04-distributed-p1.md:12-21` 追加 P1 快照正确性。
8. **debug / replay**——`asm-debug-replay.md:20-42`：OFF 剩 `endbr64;ret`；ON_FULL 3.6–40.3× → 1.44–8.76×；1e6 守恒。
9. **media / audio**——`asm-media-audio.md:17-29`：Opus 包装 1.006×/1.037×、重采样 0.098×、栈帧 1304→8 B、
   409,205 断言。
10. **media / capture+render**——`asm-media-capture-render.md:17-23`：采集 0.965×、渲染 0.66×、vs FFmpeg 1.007×、
    逐像素 0 差异、ctest 12/12。
11. **sync（mutex/cv/atomic）**——`158abb3`：Mutex 开关、NotifyOne 判空、CV 短路、TLS 镜像 23→4 条、
    enqueue 栈 696→104 B。证据在提交信息，未成报告（可提升为 ✅ 的最低要求已满足，但建议补档）。

---

## 3. 部分 / 缺失清单（按优先级）

### P0-1 A17 HTTP 服务端（架构 ✅ / 汇编 ❌ / 正确性 ✅）

- **现状**：`asm-http-e2e.md` 做了路由 trie、sendfile、pipelining，260 条断言 0 失败，但**全文 0 次 objdump**；
  对标数字（nginx 1.03–1.89×）是端到端吞吐，不是指令级。
- **该做什么**：对 `http::Router::Match`、`ResponseWriter::write`、`serve_connection` 三处出 objdump；
  当前 `2026-11-01-asm-network-review.md:16-17` 已指出「每 chunk 构造 ostringstream + 3 次 send」「每请求 14 次堆分配」，
  需给出落地后的**指令/call/分配前后对比**并归档。
- **收益/风险**：M05 已 120.8k rps；再压缩固定开销预计 10–20% 单连接 rps。风险低（只改写出路径），
  但必须保留 260 条断言回归。

### P0-2 A18 WebSocket 长连接（架构 ✅ / 汇编 ✅ / 正确性 🟡 / 结果 ❌）

- **现状**：掩码 PRNG 已从 36 µs/帧 降到 ns 级（`f40e007`），但 `ws-200k-bench.md:20-22` 显示
  20k 连接 RSS 690.4 MB（Go 298.2 MB，2.32×）、心跳 p99 6.372 ms（Go 2.082 ms，3.06×）。
- **该做什么**：三件套只做了「汇编单体」，缺**架构级内存**：per-conn 缓冲、fiber 栈预留（8 MiB VA × 连接数）、
  读缓冲上限。优先做 per-conn 内存核算表，再决定栈/缓冲缩容。
- **收益/风险**：RSS 若降到 1.5× 以内即改变战略判据；风险是栈缩容触发更多按页增长（已有 SIGSEGV 处理，可测）。

### P0-3 A05 compress + A06 reflect（架构 ✅ / 汇编 ❌ / 正确性 ✅）

- **现状**：两份报告都是纯吞吐报告（0 次 objdump）。compress 已 0.696× zlib、reflect 全项 <1.1×，
  **结论达标但取证不合规**——按本仓规则（性能口径要求 objdump 热函数），它们进不了「完整」。
- **该做什么**：补 `inflate` 主循环与 `reflect` `EncodeBinary/DecodeBinary` 的 objdump（指令/call/栈帧）+
  物理下限（memcpy 带宽、分支数），各写一节进原报告。
- **收益/风险**：零代码风险，纯取证；收益是让「达标」可复核，避免下轮重复怀疑。

### P1-1 A09 hash（硬件指令已用 / 反汇编未归档）

- **现状**：SHA-NI、CRC32 硬件指令已在用（2002.60 / 18731.55 MiB/s，M12），但 `asm-reuse-hash.md` 0 次 objdump。
- **该做什么**：`Sha256::Block`、`Crc32::Update` 出 objdump，确认无软件回退分支进热路径、无 `memcpy` 调用。
- **收益/风险**：无；纯证据补齐。

### P1-2 A19 TLS 大包 AEAD（13.41× 不达标）

- **现状**：`asm-tls.md:29,240` 明确大包 13.41×，结论是「需 AVX2/AVX512 向量化」；小包已达标。
- **该做什么**：`src/bento/net/crypto.cpp` 的 AEAD 改用 VAES/VPCLMULQDQ（若有）或至少 AVX2 多块并行；
  按 1 KiB/16 KiB/1 MiB 三档出前后对比。
- **收益/风险**：大包场景 10× 量级空间；风险是 CPU 特性分派（需 runtime dispatch + 回退），
  以及 `asm-tls.md:386` 已注明该文件在边界外——要先确认改动边界。

### P1-3 A01/A02 container + math（未达标项收口）

- **现状**：`asm-base.md:20-28`：List 1.05–1.16×、Element 1.46–1.49×、Ring 1.18–1.57×、Sqrt 1.39×、Modf 1.32×。
- **该做什么**：List/Ring 的差距来自 Go 语义句柄（两段式建环、`Element*` 间接），要么文档化「语义代价」并关闭，
  要么提供无句柄的 FastList 对标；Sqrt/Modf 已由 LTO 证明是跨 TU 边界，考虑 `-flto` 或头文件内联。
- **收益/风险**：百分点级；风险是 ABI/头文件膨胀。

### P2-1 A16 leveldb 封装 / A20 bufio（完全缺失但已有压测）

- **现状**：leveldb 封装有 1e7/1e8 压测（M07：647.5k ops/s、RSS 82.4 MiB），但无架构记录、无汇编；
  bufio 只有 M08 1853.9 MiB/s 单点，无对标。
- **该做什么**：leveldb 封装先做参数/语句复用审视，再对 `Put/Get` 出 objdump；bufio 补 Go `bufio` 同口径对照。
- **收益/风险**：低（封装层本就应接近 1.0×）；价值在于补齐覆盖表。

### P2-2 A24 rpc / A25 tunnel / A41 frame / A42 config / A08 encoding csv+hex（❌）

- **现状**：都有功能测试，**无 asm 报告、无架构记录、无基准**。
- **该做什么**：先判断是否在数据面——rpc/tunnel 在（要补），frame 在（帧编解码是每帧路径），
  config/encoding-csv 偏控制面（可降级为 ⚪ 并写明理由）。
- **收益/风险**：`frame` 最值得做（每帧调用），rpc/tunnel 次之。

### P2-3 A23/A38/A39/A40/A32 证据未归档（有提交数字，无报告）

- **现状**：p2p、app、cache、relay、logging 的优化数字只存在于 commit message（`06f9e59`/`de916bc`/
  `057257b`/`9ce8d4b`），仓库内无 objdump 归档。
- **该做什么**：各补一份 `asm-<module>.md`（可复用提交里的数字 + 重跑一次 objdump），成本约 0.5 天/份。
- **收益/风险**：零代码风险；不做的话下一轮审计仍只能判 🟡。


### 缺失项归属判定（2026-11-04）

对 P2-2 列的 7 个「无三件套」模块逐个判定归属。判据只有三条：**在不在数据面**（每次请求 / 每帧是否经过）、
**有没有可复跑的基准**、**仓库内有没有可复核的证据**（file:line）。本节只做判定，**不改上表**；
若采纳，覆盖计数的变化见小节末尾。

**结论速览**

| 模块 | 判定 | 要补的一件 | 现有基准 / 压测 |
|---|---|---|---|
| A08 encoding/csv+hex | ⚪ 不适用 | —（hex 保留现有 1 MiB 压测作回归） | `tests/encoding/test_stress.cpp`（hex/base64 1 MiB） |
| A16 leveldb 封装 | 要补（低） | 汇编：`ldb::DB::{Put,Get}` objdump | `test_leveldb_kv.cpp --bench` 1e7/1e8 ✅ |
| A20 bufio | 要补（中） | 对标 + 汇编：Go `bufio` 同口径、`Writer::Flush`/`Reader::fill` | `bench_bufio_batch.cpp` ✅、M08 单点无对标 |
| A24 rpc | **已具备，只需归档** | 无（把 `docs/modules/rpc.md` 的表搬进 `asm-rpc.md`） | `bench_rpc.cpp` 1e6/1e7 ✅ |
| A25 tunnel | 要补（高） | 基准 + 汇编：8 MiB 吞吐程序、转发循环 objdump | ❌ 仅 300 次延迟对比（M14 备注「无法复现」） |
| A41 frame | 要补（低） | 汇编：`Feed` 逐字节喂入 + 错误恢复路径 | 1e6 帧压测 ✅（`frame-codec.md:215-222`） |
| A42 config | ⚪ 不适用 | —（控制面，不该有数据面基准） | 无 bench（5 个功能测试） |

#### A08 encoding/csv+hex → ⚪ 不适用

- **csv 无生产调用点**：`src/encoding/csv.cpp`（419 行）的 `CsvReader` 只从构造时传入的**完整内存文本**解析，
  不支持 `io::Reader` 流式（`include/go2cpp/encoding/csv.hpp:24-25,37`），`CsvWriter` 只写内存 `std::string`、
  `Flush()` 是空操作（`:83`）；全仓 `encoding/csv.hpp` 的引用只有实现自身（`src/encoding/csv.cpp:40`）、
  汇总头（`include/go2cpp/encoding/encoding.hpp:37`）和 `tests/encoding/{test_csv,test_random,test_scenarios}.cpp`，
  **src/app/tools 无任何调用**（`grep -rn 'encoding/csv.hpp' src app tools` 结果为 0）。
- **hex 在热路径之外**：`include/go2cpp/encoding/hex_core.hpp`（168 行，header-only inline）被 12 个模块使用，
  但都是**短串**——`keystore.cpp:20`（key_id 8 hex / 掩码）、`token_pairing.cpp:32`、`so_manifest.cpp:28`、
  `reflect/json.cpp:11`、`logging/redact.hpp:63`、`p2p/candidate.cpp:11`、`app/resource.cpp:12`、
  `bento/app/{capability,backup,fetch}.cpp`、`bento/error/mapping.cpp:16`、`bento/net/file_transfer.cpp:22`。
  调用频率是握手 / 配置 / 日志级，不是每帧。
- **基准**：`tests/encoding/test_stress.cpp:1-10` 有 1 MiB、15 轮取最快的 hex/base64 吞吐（取最快是为了压 WSL 抖动），
  足以证明「没有逐字节分配退化」；csv 无 bench，也没有调用方需要。
- **结论**：csv ⚪（无调用）；hex ⚪（不承压，保留 stress 作回归）。**不补三件套。**

#### A16 leveldb 封装 → 要补（低优先）

- **读/写路径**（`src/storage/leveldb_kv.cpp`）：`DB::Put`（:316-340）= `impl_->LoadDb()`（一次 `atomic` load）
  + `ToSlice`（:84，只取指针与长度，**零拷贝**）+ `leveldb::DB::Put` + 可选 stats（`enable_stats` 关时无计数）；
  `DB::Get`（:432-470）往调用方 `std::string*` 写，空指针时用栈上 `scratch` 兜底。
- **压测在**：`tests/storage/test_leveldb_kv.cpp`（`--bench` 1e7、`--mem` 1e8）；台账
  `docs/metrics-ledger.md:149-161`（647,521 ops/s、RSS 82.4 MiB、VmHWM 122.8 MiB）；
  `docs/modules/storage-leveldb.md:256-274` 有与自研 KV 的 1e7 对比表（Put 3.7×、Get 0.88×）。
- **缺**：objdump。封装层只有 1 次 atomic load + 1 次转发，**预期额外 ≤5 条指令、0 次堆分配**（stats 关）。
- **该做什么**：对 `ldb::DB::Put`/`Get` 出指令/call/栈帧/原子计数，与裸 `leveldb::DB::Put/Get` 并排。
  收益不是提速，而是把「封装层 ≈1.0×」从推断变成证据（P2-1 已列，成本 ≈0.5 天）。

#### A20 bufio → 要补（中优先）

- **在数据面**：`include/go2cpp/network/bufio_socket.hpp:3-31` 是 `bufio` 与 `net::Socket` 的适配层
  （缓冲读、`FlushBatch` 批量写、`writev`/`sendmmsg`）；`src/rd_net/socket.cpp:570,923,980,1123` 的
  `SendBatch` 直接用 `bufio::BatchEntry`；rd_net 的 TCP 批量 20.8× 就建立在它上面。
- **基准**：`tests/bufio/bench_bufio_batch.cpp`（611 行，真实 TCP loopback，16/64/256 B 三档 + 4 种 `FlushPolicy`，
  输出 msg/s + p50/p99/p999 + syscall 计数），CMake **只构建不注册 ctest**（`cmake/Go2CppTests.cmake:1115-1121`，
  理由「基准不是正确性用例」正确）；M08 `test_bufio.cpp`「分块传输 1 MiB」1853.9 MiB/s
  （`metrics-ledger.md:163-171`）。
- **已有架构结论但不在 asm 报告**：`docs/reports/2026-10-31-http-perf.md:166-190` 三档写路径对比
  （小包 21–33×、大包 **−19%**、batch 的真实价值是「何时成批」），全文 objdump **0 次**。
- **缺**：① Go `bufio` 同口径 1 MiB 分块读对照——M08 **没有对标**，1853.9 MiB/s 无法判断好坏；
  ② `bufio::Writer::Flush`/`Reader::fill` 的 objdump（memcpy 次数、call 数）。
- **预期收益**：把 M08 从单点变成有对照的结论；若对照 <0.9× 再谈优化（当前无证据支持优化）。成本 ≈0.5–1 天。

#### A24 rpc → 已具备三件套，只需归档（不补做）

- **汇编证据已在仓库**：`docs/modules/rpc.md:300-320` 有 9 个符号的指令/call/栈帧/分配/原子表——
  `EncodeRequest` 165/9/24 B、`DecodeRequest` 517/13/88 B、`ServiceRegistry::FindUnlocked` 59/2/24 B、
  `ServerConnection::Process` 793/50/328 B、`Connection::Wait` 255/20/104 B、`FrameCodec::Encode` 328/11/8 B、
  `FrameCodec::FeedImpl` 284/13/88 B、`MethodBinder::Invoke` 469/19/392 B，并注明「直接 malloc/free 为 0」。
- **性能与下限**：`rpc.md:252-296`——1e6 次 p50 97.3 µs / p99 299.2 µs、协议层 109.2 ns/op、
  序列化 70.5 ns/op、TCP echo 1B 下限 106.2 µs、连接复用前后 546.3→97.6 µs（**5.6×**）。
- **正确性**：5 套 ctest（`rpc.md:322-328`）+ `tests/rpc/bench_rpc.cpp`（379 行，1e6/1e7 可复跑）。
- **缺口只有两处**：`rpc.md:6` 引用的 `/tmp/rpc.md` **不是仓库文件**；覆盖总表未收录 rpc。
- **该做什么**：新建 `docs/reports/asm-rpc.md`，把 `rpc.md:300-320` 的表与复跑命令搬过去，并把 A24 行指向它。
  **成本 0.5 天、零代码风险**；不做的代价是每轮审计继续把 rpc 误判为 ❌。

#### A25 tunnel → 要补（高优先）

- **在数据面**：`src/tunnel/mux.cpp:46-121`（`FrameReader::Next` 500 ms 超时读、`WriteFrame`）、`:132+`
  （`MuxStream::Read/Write` 按 chunk 走 `WriteAll`）；`src/tunnel/net.cpp` 为 RelayPipe 转发。
- **日志现状**（用户提到「刚加了日志」）：`GO2CPP_LOG` 只出现在进程入口——`src/tunnel/client_main.cpp` 13 条、
  `server_main.cpp` 14 条；**`mux.cpp`/`net.cpp`/`server.cpp`/`client.cpp`/`protocol.cpp` 各 0 条**。
  即：入口有观测、每 chunk 转发路径无日志（不污染热路径，但转发路径本身没有观测点）。
- **基准**：`tests/tunnel/test_tunnel.cpp:868-908` 用例 9 只做「直连 vs 隧道」300 次 p50 对比（1.96×），
  **没有 8 MiB 吞吐程序**；`docs/metrics-ledger.md:232` 明确「tunnel 8 MiB 下载吞吐无法复现——缺吞吐程序」。
- **文档数字无法复跑**：`docs/modules/tunnel.md:229-241` 的 8 MiB 488 MiB/s（0.37×）、keep-alive 2.99×、
  32 并发，来自一次性手工脚本，仓库内无对应可执行目标。
- **该做什么**：① 新增 `tests/tunnel/bench_tunnel_throughput.cpp`（复用测试里的本地站点，8 MiB ×3 轮，
  直连 vs 隧道，输出 MiB/s + p50）；② 对 `FrameReader::Next`/`WriteFrame`/`MuxStream::Write` 出 objdump；
  ③ 与 rd_net 已有 `SendBatch`（3107 指令、TCP 批量 20.8×）对比，判断转发循环能否借 `sendmmsg`。
- **预期收益**：488 MiB/s 已超 1 Gbps 网卡上限（119 MiB/s），**优化空间不在吞吐而在固定开销**
  （两端各 5 条线程 + 4 次队列唤醒 ≈0.6 ms，`tunnel.md:241` 自述）。基准的主要价值是止损：
  让 1.96× 可复跑，给 `4fcc407`（日志）/`d79abe8`（GOAWAY 退避重连）之后的改动留回归锚点。

#### A41 frame → 要补（低优先，已有 2/3）

- **每帧路径**：`src/frame/frame_codec.cpp:129`（`Encode`）、`:186`（`Feed`）、`:219`（`FeedImpl`，
  显式 `resize`+`memcpy` 规避 GCC 13 `-Wstringop-overflow` 假阳性）；被 rpc（`include/go2cpp/rpc/protocol.hpp:52`、
  `src/rpc/client.cpp:26`、`server.cpp:27`）与 tunnel（`include/go2cpp/tunnel/protocol.hpp:62`、`mux.hpp:42`）直接使用。
- **架构 + 部分汇编已在**：`docs/modules/frame-codec.md:212-222`（1e6 帧 **4.59 Mframes/s**、peak buffered 134 B、
  常用路径只拷 1 次 payload）；objdump 两项在 `rpc.md:312-313`（`FrameCodec::Encode` 328 指令 / 8 B 栈帧、
  `FrameCodec::FeedImpl` 284 / 88 B）。
- **正确性**：`tests/frame/test_frame_codec.cpp`（1130 行，黄金向量/半包/粘包/畸形/随机/fuzz/1e6 压测），
  ctest 注册于 `cmake/Go2CppFrame.cmake:37-43`。
- **缺**：`Feed` 的**逐字节喂入**指令数与 `memcpy` 次数（测试「100 B 帧按 1 B 喂 100 次」决定半包是否 O(n²)），
  以及错误恢复路径（清空缓冲）的取证。
- **该做什么**：补这两个路径的 objdump + 一次 1 B 切分的帧率（对比整帧 4.59 Mframes/s）。成本半天。
- **注意**：不要与 app 层 `src/app/frame.cpp`（HTTP2 9 B 帧，`de916bc` 98→4.7 ns）混淆——那是另一条线，已记在 A38。

#### A42 config → ⚪ 不适用

- **只在启动/运维路径**：`src/config/config.cpp`（1382 行）的 `LoadFromIni`（:599）、`LoadFromFile`（:697）
  由启动或显式调用触发；`StartWatcher`（:768）的读取间隔是**调用方传入**的 `interval`，
  线程用 `cv.wait_for(interval)` 轮询 mtime（:802），空闲不占 CPU。
- **调用频率**：`Config::Instance()` 全仓 13 处（`src/network/hook.cpp`、`src/os/proc.cpp`、`src/os/config.cpp`、
  `mcp/src/config/config.cpp` 等），都在初始化/进程入口，**没有每请求读取**。
- **测试**：`tests/config/` 5 个功能测试（reflect/registry_macro/transaction/watch/so_manifest），无 bench；
  `docs/modules/config.md` 无性能章节（`ops-log-config.md:249-250` 的 `ConfigStore::Load` 3.58 ns 是 logging 配置，
  不是本模块）。
- **结论**：⚪ 控制面，不做数据面下限；最多在归档时补一句「启动路径 1 次 `LoadFromFile`，不进入承压路径」。

**已采纳本判定并同步总表（2026-11-04）**：✅ 11→**13**（A24、A39）、🟡 24→**26**（A16/A20/A41 升 🟡，A39 升 ✅）、
❌ 7→**1**（只剩 A25）、⚪ 2→**4**（A08/A42）。

**要补项 TOP3（按「数据面程度 × 证据缺口 × 成本」）**

1. **tunnel**——唯一「数据面 + 零吞吐基准 + 文档数字不可复现」的模块，且刚经历日志/退避改动，最需要回归锚点。
2. **bufio**——rd_net 批量发送的底座；M08 只有单点没有对标，补一个 Go 对照 + 两个函数 objdump，成本最低。
3. **frame**——每帧路径，2/3 证据已在，只差 `Feed` 逐字节路径，半天可完成。

（rpc 不计入 TOP3：证据已齐，属「归档」而非「补做」，0.5 天即可从 ❌ 纠正为 ✅。）

**本节边界**：未跑任何基准、未改代码、未 commit；时间盒 30 分钟；所有数字均转引仓库内文件，不引用 `/tmp` 或外部记忆。

---

## 4. 用户会踩到的热路径 × 三件套

| 热路径 | 代表实现 | 三件套 | 汇编级？ | 备注 |
|---|---|---|---|---|
| **HTTP 服务端** | `bento/http`、`network/http` | 🟡 | **否** | 见 P0-1；M05 120.8k rps 但无指令取证 |
| **WS 长连接服务端** | `bento::WsServer` | 🟡 | 是（掩码/帧） | 但 RSS/p99 输给 Go（`ws-200k-bench.md`） |
| **序列化** | reflect、encoding/base64 | 🟡 | **否**（reflect）/ 弱（base64） | csv/hex 完全没做 |
| **缓存** | `cache/`（ARC/LRU/2Q/CLOCK） | 🟡 | **否**（只有提交数字） | 命中率已对齐，缺指令级 |
| **存储** | sqlite ✅、kv ✅、leveldb 封装 ❌ | 混合 | sqlite 是 | leveldb 封装无任何记录 |
| **压缩** | gzip | 🟡 | **否** | 达标但取证不合规 |
| **加密** | TLS / AEAD | 🟡 | 是 | 大包 13.41× 未达标 |
| **调度器** | `Scheduler` | 🟡 | 是（P21/P22） | 已决定不再投入（M03） |
| **网络 IO** | rd_net ✅、bento/link 🟡 | 混合 | rd_net 是 / link 否 | link 多队列架构结论「无收益」 |
| **文件 IO** | `os/file`（writev/readv） | ✅ | 是 | 本仓最强项之一 |
| **内存分配** | cache 节点池、Channel/任务 | 🟡 | 部分 | 无全局 allocator 报告；WS per-conn 内存是最大缺口 |

**其中「没做过汇编级」的（需补）：HTTP 端到端、reflect、encoding/csv+hex、cache、gzip、leveldb 封装、
bufio、bento/link、bento/peer_hub、rpc、tunnel、frame、config。**

---

## 5. 建议顺序（前 5 项）

1. **A17 HTTP 服务端补汇编取证**（P0-1）——热路径第一入口，报告现有但缺指令级；成本 0.5 天，零功能风险。
2. **A18 WS 长连接内存架构**（P0-2）——唯一「三件套做了却输给 Go」的数据面；RSS 2.3× 是战略级问题，
   先做 per-conn 内存核算再动刀。
3. **A05+A06 补 objdump**（P0-3）——两处「已达标但不合规」，补齐后覆盖表 ✅ 从 11 升到 13。
4. **A15/A14 存储层收口**（kv 读 0.48×、features 弱取证）——存储是用户最先踩的路径之一；
   若确认改用 LevelDB，则把 LSM 标「冻结」并只维护封装层。
5. **A08/A41/A42/A24/A25 判定归属**——先花 2 小时判定 frame 是否每帧路径；是则排进 P1，否则降为 ⚪ 并写理由。
   （P1-2 TLS 大包与 P1-3 container/math 可作为并行线，不占主线。）

---

## 6. 判定的已知偏差（诚实记录）

1. **未复跑基准**：所有数字转引自台账/报告。台账标 `待复跑`/`STALE` 的（M03 的历史 122 ns、M05b 的 411k）
   不得作为结论；本文引用时已注明「历史」或「无出处」。
2. **「有架构」判定**依据是报告/提交能指出具体改动（数据结构/锁/IO 模型/内存布局），不是「优化过」三个字；
   A01/A02 属审查型报告，按报告自述的改动清单计为「有」。
3. **A28 scheduler 判 🟡 而非 ❌**：三件都做了（P19–P22 + smoke + 1e6），但结论不达标且用户已决定关闭
   （`metrics-ledger.md` M03）。若按「是否达标」口径，它应记为 ❌。
4. **A36 signaling 判 ⚪**：报告自述控制面（`asm-signaling.md:9-18`），数据面下限不适用；若要求覆盖
   *所有*实现，应改 🟡。
5. **A44 core/error/go 判 ⚪**：无数据面热路径，但 `error` 有 5 套 ctest，不是零覆盖。
6. **口径敏感性**：若把「commit message 里的数字」也算作汇编级证据，✅ 会从 11 升到 16
   （A23/A32/A38/A39/A40 转 ✅）；本文坚持「仓库内可复核的报告/台账」为准，故仍判 🟡。

---

## 附录 A：证据索引（文件 → 关键数字）

| 文件 | 行数 | objdump | 覆盖模块 |
|---|---:|---:|---|
| `asm-base.md` | 302 | 4 | container/math/metrics/path |
| `asm-os.md` | 372 | 4 | os/file/proc/shm/mq |
| `asm-storage-sqlite.md` | 327 | 3 | storage/sqlite |
| `asm-storage-kv.md` | 136 | 2 | storage/kv |
| `asm-storage-features.md` | 196 | 2 | storage/queue/watch/memcache |
| `asm-distributed.md` | 472 | 4 | distributed |
| `asm-debug-replay.md` | 343 | 4 | debug/replay |
| `asm-media-audio.md` | 434 | 4 | media/audio |
| `asm-media-capture-render.md` | 235 | 5 | media/capture/render |
| `asm-sfu.md` | 145 | 1 | media/sfu |
| `asm-signaling.md` | 134 | 1 | media/signaling |
| `asm-tls.md` | 448 | 9 | network/tls |
| `asm-http-e2e.md` | 286 | 0 | network/http |
| `asm-tools.md` | 129 | 1 | tools/encoding/reflect 基线 |
| `asm-compress.md` | 184 | 0 | compress |
| `asm-reflect.md` | 202 | 0 | reflect |
| `asm-legacy.md` | 138 | 0 | bento/link、peer_hub |
| `asm-reuse-hash.md` | — | 0 | hash |
| `asm-reuse-random-endian.md` | 152 | 0 | 随机/端序（安全修复，非性能） |
| `rdnet-v3-review.md` | 88 | 有 | rd_net |
| `2026-11-01-asm-concurrency-review.md` | 243 | 有 | concurrency（审查） |
| `2026-11-01-asm-network-review.md` | 386 | 有 | network（审查 20 点） |
| `ws-200k-bench.md` | — | 0 | WS 长连接压测 |

## 附录 B：本次盘点的复核命令（只读）

```bash
cd /UserData/CodexWorkSpace/Go2cpp
ls docs/reports/asm-*.md | wc -l                       # 18
grep -c objdump docs/reports/asm-*.md                  # 0 的即为「无指令级」
grep -rn "add_test" --include=CMakeLists.txt . | wc -l # 41
ls -d tests/*/ | wc -l                                 # 35 个测试目录
git log --oneline --grep="objdump" --grep="汇编" --grep="三件套" | head -30
```

## 附录 C：明确的无证据声明

- **A05/A06/A17/A21/A22** 的「达标」只有吞吐数字，**没有**指令级证据——本文判 🟡 而非 ✅，原因在此。
- **A23/A32/A38/A39/A40** 的优化只存在于 commit message，仓库内**无** objdump 归档；若需引用，
  必须重跑反汇编后才能作为结论。
- 本文**未**重新跑任何基准（时间盒 35 分钟，且规则要求基准独占）；表中所有数字均转引自
  `docs/metrics-ledger.md` 或对应报告，状态以台账标注为准（`实测`/`待复跑`）。
