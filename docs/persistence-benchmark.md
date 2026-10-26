> 已合并至 docs/modules/persistence.md（模块文档体系整理，2026-10-26）；本文保留作历史细节。

# 事件流「追加写 + 持久化」性能基准与原型

> 日期：2026-10-21
> 范围：G2CRPLY v5 资源事件（40B 记录头 + 72B payload，逐条 CRC/vtime），
> `tools/replay` 现有 `Recorder` 基线 vs `tools/persistence` 独立原型。
> 结论先行：**单线程 2.96x、4 线程 13.83x**（均带 100ms `fdatasync` 语义），
> p99 从 4.46µs 降到 0.32µs；代价是 RSS 从 7.7MB 升到 25.9MB，
> 以及「Append 返回 ≠ 已持久化」的语义变化。

---

## 1. 目标与方法

### 1.1 问题

现有录制路径（`tools/replay/src/recorder.cpp`）：

- `Recorder::Write()`（`recorder.cpp:379-405`）对每条事件：
  1. 取全局 `m_mutex`；
  2. `m_seq.fetch_add` 分配 seq；
  3. `EncodeEvent()`（`format.cpp:296-326`）构造临时 `payload` vector
     （`reserve(64)`）+ 逐字节 `push_back`；
  4. 逐条 IEEE CRC32（`format.cpp:40-48`，按字节查表）；
  5. `WriteRaw()` → `fwrite()`（`recorder.cpp:362-377`），依赖 libc 默认
     4 KiB 缓冲。
- **没有任何 `fsync`/`fdatasync` 接口**；`Finalize()` 只 `fflush`
  （`recorder.cpp:487-490`），数据到页缓存即返回。
- 资源事件 sink 是**同步回调**（`recorder.cpp:111-132`），产生事件的
  worker 线程直接承受上述锁 + 编码 + 写路径。

因此基线有两个问题：单线程已接近编码/写路径上限；多线程被全局锁串行化，
尾延迟急剧恶化；且「持久化」语义缺失。

### 1.2 测试口径

| 项 | 设置 |
|---|---|
| 事件 | v5 资源事件，固定 112B；字段逐条变化（type/rid/actor/target/value/aux/bytes/address） |
| 规模 | N=1e5、1e6；1e6 文件约 112MB |
| 线程 | 单线程 / 4 线程（每线程独立生产者） |
| 计时 | `clock_gettime(CLOCK_MONOTONIC_RAW)` 逐事件包住 `Append/Write`；吞吐用墙钟（含 Finish/fdatasync） |
| 延迟 | p50/p99/p99.9，单位 ns；多线程合并所有线程样本 |
| 轮次 | 5 轮，取中位数；loadavg > 2.0 的轮次标记 `valid=0` 并剔除 |
| 校验 | 结束后 mmap 全量回读：逐条 CRC、资源事件条数、seq 单调性、逻辑 checksum |
| 环境 | WSL2，kernel 6.6.87.2-microsoft-standard-WSL2，12 vCPU，ext4（/dev/sdd），GCC 13.3，`-O3 -DNDEBUG` |
| 负载 | 全部轮次 loadavg 0.3–1.32（< 2.0） |

基线用真实 `Recorder`；`recorder-fsync` 是「给现有路径补 100ms fdatasync」
的近似（`raw-buffered`：同一 `EncodeEvent` + 1MiB 用户缓冲 + fdatasync），
因为 Recorder 本身没有 fsync API。

### 1.3 复现

```bash
cd /UserData/CodexWorkSpace/Go2Cpp-work

# 一键：构建基线 + 原型、跑原型自测、5 轮压测、输出 TSV/Markdown
ROUNDS=5 bash tests/bench/run_persistence_bench.sh

# 原型单独构建与测试
cmake -S tools/persistence -B /tmp/go2cpp-persistence-build \
      -DCMAKE_BUILD_TYPE=Release -DPERSISTENCE_BUILD_TESTS=ON
cmake --build /tmp/go2cpp-persistence-build -j
/tmp/go2cpp-persistence-build/persistence_tests
/tmp/go2cpp-persistence-build/platform_probe   # io_uring / O_DIRECT 探测
```

产物：`tests/bench/results/persistence_<ts>.tsv`（原始轮次）、
`_summary.tsv`（中位数）、`.md`（同本文表格）。

---

## 2. 基线数据（现状）

5 轮中位数，完整事件流 1e6 条：

| 配置 | 事件/秒 | p50 (ns) | p99 (ns) | p99.9 (ns) | 1e6 耗时 (ms) | RSS (KB) | 文件 (B) | fsync |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| recorder 1 线程 | **1,269,110** | 591 | 4,458 | 19,938 | 788 | 7,752 | 112,000,252 | 无 |
| recorder 4 线程 | **430,855** | 622 | 173,869 | 407,792 | 2,321 | 7,608 | 112,000,252 | 无 |
| recorder 1e5 单线程 | 974,237 | 591 | 4,559 | 11,652 | 102.6 | 4,236 | 11,200,252 | 无 |
| recorder-fsync 100ms 单线程 | 1,198,081 | 611 | 662 | 4,950 | 835 | 7,792 | 112,000,181 | 9 次，p99 9.06ms |

关键事实：

1. **单线程约 1.27M 事件/s**，与任务给出的 `resource_v5_demo` 基线
   （10000 事件 / 7.6ms ≈ 1.3M/s）一致。
2. **4 线程反而降到 0.43M/s**：全局 `m_mutex` 串行化 + 持锁 `fwrite`，
   p99 从 4.5µs 恶化到 174µs（38x）。
3. 加上 100ms fdatasync 后单线程降到 1.20M/s（-5.6%），
   但持久窗口从「整轮 835ms」降到 100.6ms。
4. 微基准拆分（`/tmp/g2c-persist/micro.cpp`）：`EncodeEvent` 约 308ns/条；
   本原型的定长编码器约 175ns/条（含 CRC）。逐条 `write(2)` 只有 0.57M/s，
   说明 **syscall + 逐条编码/加锁** 是主要成本，而不是磁盘带宽
   （`dd` 顺序写 512MiB + fdatasync 约 720MB/s）。

---

## 3. 原型设计（`tools/persistence`）

### 3.1 总体结构

```
persist_bench / 业务线程
   │  Append(event)  —— 只做定长编码 + memcpy 进本线程 batch
   ▼
[Producer 0] batch 256KiB   [Producer 1] batch 256KiB ...
   │ 封批：slot + seq 区间在同一把极短自旋锁内分配
   ▼
MPSC 环形队列（std::atomic<Batch*>，容量 queue_batches）
   │
   ▼
单后台线程：顺序 write → 可选 sync_file_range 预回写 → fdatasync 策略
   │
   ├─ 单文件：G2CRPLY 头 + kMeta + 记录 + kEnd
   └─ 段模式：seg-000001… + manifest（段号/字节数/seq 范围/时间范围）
```

文件：`include/persistence/{encoder,writer,reader}.hpp`、
`src/{encoder,writer,reader}.cpp`；不修改 `src/**` 与 `tools/replay`。

### 3.2 关键优化

1. **定长编码器**（`encoder.cpp`）：
   直接向目标缓冲区写 112B（memcpy 小端 store），一次头 CRC + 一次
   payload CRC，避免 `EncodeEvent` 的临时 vector 和逐字节 push_back。
   与 `replay::EncodeEvent` 逐字节兼容，单测用 `DecodeEvent` 逐字段校验。
2. **批量攒批**：默认 `batch_bytes=256KiB`，满批才封；100 万事件约 428 批，
   把 write 次数从 1e6 降到约 430。
3. **每线程 batch + MPSC 环**：
   - 生产者只写自己的 batch，**事件级无锁、无全局互斥量**；
   - 封批时在同一把自旋锁内分配 `slot` 与 seq 区间，保证
     「文件顺序 == seq 顺序」；
   - 后台线程顺序消费，生产者与 IO 解耦。
4. **fsync 策略**（`SyncPolicy`）：
   - `kNone`：只写页缓存（最快，掉电可丢全部）；
   - `kBatch`：累计 `sync_bytes`（默认 4MiB）后 fdatasync；
   - `kInterval`：后台线程每 `interval_ms`（默认 100ms）fdatasync，
     空闲时也会触发；**Append 不等待 fsync**。
5. **writeback-ahead**：每累计 8MiB 调 `sync_file_range(..., SYNC_FILE_RANGE_WRITE)`
   提前启动内核回写，让最终 fdatasync 只等少量脏页。这不是持久化保证，
   只是降低尾延迟（4 线程 interval 场景实测从约 4.6M/s 提升到约 6.0M/s）。
6. **段文件 + manifest + 范围索引**（借鉴 Stratum 的不可变段设计）：
   每 8MiB 滚动一个 `seg-000001` 文件；结束时原子写 manifest
   （每段字节数、first/last seq、first/last ts、事件数）；读取端按
   seq/时间范围先筛段，再 mmap 顺序解码。段文件是裸记录流，
   不是独立 G2CRPLY 文件。

### 3.3 持久化语义与代价

| 策略 | 最坏持久窗口 | 1e6 事件/秒（线程） | fsync 次数 | 说明 |
|---|---|---:|---:|---|
| recorder 现状 | **无保证**（只到页缓存） | 1,269,110（1） | 0 | 只有 fflush |
| recorder-fsync 近似 | ~100ms + fsync | 1,198,081（1） | 9 | 单把锁内同步 fsync |
| `mpsc + kNone` | 无限（掉电全丢） | 4,025,286（1） | 0 | 最快，适合可重放/可重算数据 |
| `mpsc + kBatch(4MiB)` | 约 8ms | 3,605,638（1） | 26 | 持久窗口小，fsync 次数多 |
| `mpsc + kInterval(100ms)` | 约 100ms + fsync p99 7.0ms | **3,760,040（1）** | 4 | 推荐默认 |
| `segmented + kInterval(100ms)` | 约 12ms（段滚动也 fsync） | 3,890,627（4） | 15 | 换范围索引，写放大更高 |

**语义边界**：`Append()` 返回时事件只在本线程 batch 中，不保证已 write，
更不保证已 fsync；需要「提交即持久」的调用方必须增加显式 barrier
（当前原型未提供，可在 consumer 队列上做 flush-and-wait）。这是用
p99（0.32µs vs 4.46µs）换持久窗口的明确取舍。

---

## 4. 优化后数据（5 轮中位数）

### 4.1 绝对数据

| 配置 | 事件/秒 | p50 (ns) | p99 (ns) | p99.9 (ns) | 1e6 耗时 (ms) | RSS (KB) | 峰值 RSS (KB) | 磁盘 (B) | fsync 次数 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| recorder 1 线程 | 1,269,110 | 591 | 4,458 | 19,938 | 788.0 | 7,752 | 11,520 | 112,000,252 | 0 |
| recorder 4 线程 | 430,855 | 622 | 173,869 | 407,792 | 2,321.0 | 7,608 | 8,528 | 112,000,252 | 0 |
| recorder-fsync 1 线程 | 1,198,081 | 611 | 662 | 4,950 | 834.7 | 7,792 | 12,520 | 112,000,181 | 9 |
| **mpsc + 100ms, 1 线程** | **3,760,040** | 201 | 321 | 1,082 | 266.0 | 25,904 | 29,668 | 112,000,181 | 4 |
| **mpsc + 100ms, 4 线程** | **5,959,257** | 220 | 551 | 1,913 | 167.8 | 26,196 | 27,132 | 112,000,181 | 3 |
| mpsc 无 fsync, 1 线程 | 4,025,286 | 201 | 271 | 581 | 248.4 | 25,820 | 29,568 | 112,000,181 | 0 |
| mpsc batch 4MiB, 1 线程 | 3,605,638 | 201 | 290 | 832 | 277.3 | 25,916 | 29,680 | 112,000,181 | 26 |
| segmented 无 fsync, 4 线程 | 7,939,604 | 230 | 491 | 891 | 126.0 | 26,212 | 27,096 | 112,001,124 | 0 |
| segmented + 100ms, 4 线程 | 3,890,627 | 211 | 451 | 1,001 | 257.0 | 26,200 | 27,132 | 112,001,123 | 15 |
| buffered（单锁对照）, 1 线程 | 2,495,495 | 220 | 240 | 691 | 400.7 | 8,636 | 12,396 | 112,000,181 | 0 |
| mpsc + 100ms, 1 线程 (1e5) | 3,190,739 | 201 | 450 | 481 | 313.4 | 22,376 | 22,640 | 11,200,181 | 2 |

### 4.2 加速比

| 优化配置 | 基线 | 加速比 |
|---|---|---:|
| mpsc + 100ms fsync, 1 线程 | recorder（无 fsync）, 1 线程 | **2.96x** |
| mpsc + 100ms fsync, 1 线程 | recorder-fsync 100ms, 1 线程 | **3.14x** |
| mpsc + 100ms fsync, 4 线程 | recorder（无 fsync）, 4 线程 | **13.83x** |
| mpsc 无 fsync, 1 线程 | recorder（无 fsync）, 1 线程 | 3.17x |
| segmented + 100ms fsync, 4 线程 | recorder（无 fsync）, 4 线程 | 9.03x |
| mpsc + 100ms fsync, 1 线程 (1e5) | recorder（无 fsync）, 1 线程 (1e5) | 3.28x |

**目标 ≥2x 达成**：带 100ms fsync 的推荐配置单线程 2.96x、4 线程 13.83x；
即使与「recorder + 同语义 fsync」比也是 3.14x。延迟侧 p99 从 4.46µs
降到 0.32µs（单线程）。代价：RSS 增加约 18MB（64 个 256KiB batch 的队列上限），
以及 Append 不再等待落盘。

> **轮间波动**：单线程 interval 配置在多次运行中为 3.70–3.83M/s，较稳定；
> 4 线程 interval 配置受页缓存/回写状态影响，实测 4.95–7.60M/s，
> 5 轮中位数为 5.96M/s。表中比值用中位数；若取保守值 4.95M/s，
> 相对 recorder 单线程仍有 3.90x、相对 4 线程仍有 11.5x。
> 因此「≥2x」结论对单线程和保守 4 线程值都成立。
> 表中数据采集后，最终代码只增加了一次 `relaxed` 原子失败标志读；
> 用最终二进制复测 t1=3.70M/s、t4=4.95M/s，差异在轮间波动范围内。

### 4.3 回读与段索引

| 读模式 | 记录数 | 耗时 (ms) | 回读事件/秒 | 说明 |
|---|---:|---:|---:|---|
| 全量 scan（单文件） | 1,000,000 | 218.4 | 4.58M | mmap + 逐条 CRC |
| 全量 scan（段模式） | 1,000,000 | 215.6 | 4.64M | 14 个 8MiB 段 |
| seq 范围 [900000,1000000] | 100,000 | **21.7** | 4.55M | 先按段索引筛段，再顺序解码 |
| 时间范围（段首/段尾索引） | 1,000,000 | 215.6 | 4.64M | 段级时间范围筛选 |

范围查询比全量扫描快约 **9.9x**（21.7ms vs 215.6ms），但当前只做到
**段级**定位：从段首开始解码，没有记录级稀疏索引；后续可在段内每 4096 条
写一个 `(seq,offset)` 稀疏索引，把 9.9x 提升到接近 15x（段内偏移）。

---

## 5. Stratum 评估

### 5.1 对象澄清

任务原写的 `electro-geek/Stratum` 是 GitHub 上一个同名 C++ 存储引擎；
用户随后澄清：**Stratum 是用户本地项目**，路径
`/mnt/e/CodexWorkspace/Stratum`（WSL 开发目录 `/UserData/CodexWorkSpace/Stratum`，
remote `https://github.com/mdymdy666/Stratum`），按 WSL/Linux 项目只读评估。
候选 2 `replikativ/stratum` 是 Clojure/ClojureScript 列存数据库。

### 5.2 本地 Stratum 事实（只读 + 本机复测）

| 项 | 结论 |
|---|---|
| 语言/构建 | C++17、CMake ≥3.16、GCC 13.3；嵌入式库 + CLI，不是服务 |
| 许可证 | **Apache-2.0**（SPDX 一致；第三方 Fast DDS Apache-2.0、LevelDB BSD-3 仅设计参考；无 GPL） |
| V1 存储 | 自研 LSM：WAL（LevelDB log 格式，32KiB block + CRC）→ `std::map` MemTable → SSTable（追加记录、读时全量建索引）→ L0/L1/L2 compaction |
| V1 API | `Open/Put/PutBatch/Get/Delete/NewIterator(begin,end)/Snapshot/Flush/Close`；key/value 任意 bytes，无 schema |
| V1 持久化 | `SyncPolicy::{kNone,kBatch,kAlways}`；默认 `kBatch` **不逐条 fsync**，只在 `Flush()/Close()` 时 WAL fsync |
| V2 `StratumDB` | `TimeKey(20B)` + `AppendBatch(events,sync)` + `QueryRange(time)`；依赖外部 LSMengine/LevelDB 静态库，硬编码 `/UserData/LeaningLevelDB/LSMengine`，需 `-fno-rtti -fno-exceptions` |
| 实测 1e6×112B（单线程） | V1 默认 4MiB memtable **0.11M rec/s**；64MiB 阈值 **0.24M rec/s**；`StratumDB` 默认 4MiB **0.53M/s**，64MiB + batch 5000 **1.80M/s（sync=false 短窗口）** |
| 校验 | WAL 有自身块 CRC；**SSTable 无记录级 CRC**，value 完整性要上层做（G2CRPLY 逐条 CRC 可补） |

### 5.3 可借鉴的设计点（已落到原型）

| Stratum 设计 | 原型对应 | 效果 |
|---|---|---|
| WAL 追加写 + 块 CRC | 记录级 CRC + 不可变段文件 | 去掉全局锁后仍能逐条校验 |
| MemTable 达阈值刷 SSTable | batch 满 256KiB 封批 + 后台落盘 | 把 1e6 次 write 降到约 430 次 |
| 不可变 SSTable + 段级元数据 | `seg-*` + manifest（seq/时间范围） | 范围查询 9.9x |
| `kNone/kBatch/kAlways` | `kNone/kBatch/kInterval` | 明确持久窗口 vs 吞吐 |
| V2 TimeKey 时间索引 | 段级 first/last ts | 时间范围先筛段 |
| 已知缺陷（SSTable 无 CRC、std::map O(n)、单锁串行写） | 原型避开：逐条 CRC、数组 batch、每线程独立 buffer | 不重复其性能坑 |

### 5.4 直接集成结论：**不直接链接，仅参考设计**

- **吞吐不匹配**：V1 在 112B 记录、1e6 条下只有 0.11–0.24M rec/s，
  低于现有 Recorder 基线（1.27M/s）；V2 的 1.80M/s 是 `sync=false` 短窗口，
  且依赖外部 LSMengine 硬编码路径，移植/构建风险高。
- **语义不匹配**：Stratum 没有「事件流」抽象：无 seq/时间二级索引
  （V1）、无记录级 CRC、`PutBatch` 非严格原子；这些都要 Go2Cpp 侧补，
  胶水约 150–500 行 + 压测/故障注入，V1 2–4 人日、V2 5–10 人日。
- **许可证**：Apache-2.0 与 Go2Cpp MIT 兼容，风险低；但直接引入会新增
  第三方库与构建面（V2 还要 `-fno-rtti -fno-exceptions`）。
- **推荐路线**：**参考设计**（批量 WAL、不可变段、manifest/范围索引、
  fsync 策略），不直接链接；若未来要接，优先 V1 `stratum_storage_lsm`
  + `stratum_core`，key=8B 大端 seq、value=112B 记录，CRC 由 Go2Cpp 校验。

### 5.5 候选 2：`replikativ/stratum` —— 不可用

Clojure/ClojureScript 列存数据库，需要 JVM/Clojure 运行时；列存模型不适合
逐条 112B 变长事件流；G2CRPLY 的 seq/CRC/vtime 需要自定义 schema；许可证
未确认（疑 EPL-1.0，与 MIT 链接分发需额外合规评估）。结论：**不可用**。

---

## 6. 可选技术评估（未采用）

| 技术 | 本机探测 | 结论 |
|---|---|---|
| `io_uring` | `io_uring_setup(64)` 成功，`features=0x3fff` | **可用但未接入**：1e6 事件只有约 430 次 write（256KiB 批），syscall 摊销收益远小于编码/CRC；接入主要价值是异步 fsync 重叠，需额外 200+ 行 ring 管理，风险/收益不划算 |
| `O_DIRECT` | 4KiB 对齐写成功 | **未采用**：112B 变长记录需对齐填充，绕过 page cache 会放大写放大；本项目瓶颈不是缓存污染 |
| LZ4/zstd | 系统有 `liblz4` / `libzstd` | **未采用**：事件流可压缩，但需要块 framing + 回放解压，且磁盘 720MB/s 不是瓶颈；压缩会增加 CPU 与格式复杂度 |
| `sync_file_range` | Linux 可用 | **已采用**：每 8MiB 预回写，4 线程 interval 吞吐约 4.6M→6.0M/s |

---

## 7. 边界、未验证项与风险

1. **环境**：仅在 WSL2 + ext4（VHDX）测；未在裸机 NVMe、XFS、不同内核上复测。
   `dd` 顺序写 512MiB + fdatasync 约 720MB/s，但页缓存状态会跨轮次影响
   4 线程结果（同一配置轮次间可见约 2x 波动），因此只采信 5 轮中位数与
   相对比值，不把单轮绝对值当承诺。
2. **掉电/崩溃**：未做真实断电、内核 panic、磁盘满、fsync 错误注入；
   `kNone` 明确不保证持久化，`kBatch/kInterval` 的最坏窗口按 fsync 间隔估算。
   `writeback-ahead` 只提前回写，不构成持久化保证。
3. **语义**：`Append` 返回 ≠ 已 write/已 fsync；没有 `FlushAndWait` API。
   段模式每次滚动会对封存段 fsync（`kInterval` 下持久窗口约 12ms，
   但吞吐低于单文件）；单文件原型写了 `kEnd` 但整文件 CRC 字段为 0，
   **不能直接喂给现有 replay CLI**（会被 kEnd checksum 校验拒绝）；
   原型用自带 reader 做逐条 CRC/计数/checksum 校验。
4. **seq 空洞**：每批按 `batch_bytes/112` 预留 seq 区间，批未填满会留空洞；
   文件顺序严格递增，回放端只依赖文件顺序与单调 seq（本原型 reader 已验证）。
   若未来要求 seq 无空洞，需要封批时回填/重算 CRC。
5. **事件类型**：压测只用 v5 定长资源事件；v4 事件（88B）与 `kNote/kTimer`
   走通用回退路径，单条上限 64KiB，未做 1MiB 大记录压测。
6. **多线程线性化**：seq 顺序 = 批次封存顺序，不等于真实并发事件的墙钟顺序；
   现有 Recorder 用「调用方持锁 + Write 内分配 seq」保证线性化，本原型在
   批量级近似；对回放驱动（park/wake 顺序）是否足够，需要按真实负载验证。
7. **Stratum**：只读评估 + 本机基准，未修改/提交其仓库；V1/V2 的多线程、
   长稳、断电、1MiB 单记录、`sync=true` 吞吐未测；E: 快照与 WSL 开发目录
   的 `lsm_v2` 存在差异，本文数据以本机 WSL 构建为准。
8. **测试**：`persistence_tests` 覆盖编码往返、三引擎写读、4 线程、
   段滚动、seq/时间范围、v4/v5 混合、截断/CRC 损坏/manifest 大小不一致、
   空/单条、interval fsync、跨引擎 checksum；**未覆盖**真实调度器
   并发事件流（需要 GO2CPP_REPLAY=ON 构建 + 真实 channel/mutex 负载）。
9. **工作树范围**：本次只新增 `tools/persistence/**`、`tests/bench/persistence_bench.cpp`、
   `tests/bench/run_persistence_bench.sh`、`tests/bench/results/persistence_*`、
   `docs/persistence-benchmark.md`；未改 `src/**` 与 `tools/replay`。
   工作树中 `src/debug/**` 的改动来自其它 agent 的并发工作，不在本次范围。

---

## 8. 结论与建议

1. **目标达成**：推荐配置 `mpsc + 100ms fdatasync` 单线程 **3.76M 事件/s
   （2.96x）**，4 线程 **5.96M/s（13.83x）**；p99 0.32µs（单线程）。
   即使与「recorder + 同语义 fsync」比也有 3.14x。
2. **默认建议**：
   - 吞吐优先、可接受 ~100ms 持久窗口 → `kInterval=100ms`；
   - 持久窗口优先 → `kBatch=4MiB`（约 8ms 窗口，单线程 3.61M/s）；
   - 可重放/可重算数据 → `kNone`（4.03M/s，掉电全丢）。
3. **生产化前必须补**：`FlushAndWait` barrier、整文件 CRC / 标准 G2CRPLY
   导出、记录级稀疏索引、真实调度器负载与断电/磁盘满故障注入、
   长稳（小时级）与多进程并发写。
4. **Stratum**：本地项目 Apache-2.0、C++17，设计可借鉴（WAL 批量、不可变段、
   manifest/范围索引、fsync 策略），但**不直接集成**：V1 吞吐低于基线，
   V2 依赖重且无记录级 CRC；直接链接收益 < 适配成本。
5. **不 commit**：所有改动为工作树新增文件，未提交。

---

## 9. 文件清单

| 路径 | 说明 |
|---|---|
| `tests/bench/persistence_bench.cpp` | 基线压测（真实 Recorder / raw / raw-buffered + fsync 策略） |
| `tests/bench/run_persistence_bench.sh` | 一键构建 + 自测 + 5 轮压测 + 汇总 |
| `tools/persistence/include/persistence/encoder.hpp` | 定长编码器接口 |
| `tools/persistence/src/encoder.cpp` | 112B/88B 记录编码 + 逻辑 checksum |
| `tools/persistence/include/persistence/writer.hpp` | 写入引擎接口（buffered/mpsc/segmented、fsync 策略） |
| `tools/persistence/src/writer.cpp` | 批量/MPSC/后台线程/段文件/manifest/writeback-ahead |
| `tools/persistence/include/persistence/reader.hpp` | mmap 读取端接口（全量/seq 范围/时间范围） |
| `tools/persistence/src/reader.cpp` | 逐条 CRC、seq 单调、段级范围筛选 |
| `tools/persistence/bench/persist_bench.cpp` | 原型压测（append 延迟、finish、fsync、回读） |
| `tools/persistence/tests/persistence_tests.cpp` | 随机/边界/故障注入测试 |
| `tools/persistence/tests/platform_probe.cpp` | io_uring / O_DIRECT 探测 |
| `tools/persistence/CMakeLists.txt` | 独立 CMake 工程 |
| `tests/bench/results/persistence_20261021-204139*.{tsv,md}` | 本文数据原始/汇总 |
