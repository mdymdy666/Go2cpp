# KV 读路径收口：0.48× 溯源、同机复测与架构审计

- 时间：2026-11-04 13:00–13:20
- 机器：AMD Ryzen 9 8945HX（12 vCPU / 32 MiB L3）、19 GB RAM、WSL2（6.6.87）、ext4（/dev/sdd）
- 工具链：g++ 13.3.0，`-O3 -DNDEBUG`（与仓库 Release 的 `CMAKE_CXX_FLAGS_RELEASE` 一致），
  `-fno-semantic-interposition -fno-stack-protector`（与 `cmake/Go2CppKv.cmake` 一致）
- 负载：单线程 / `taskset -c 2,3`；key `key-<16 位数字>`（20B）、value `value-<16 位数字>`（22B）；
  1e7 条 = 490 MB 数据、4 个 SSTable
- 结论先行：**0.48× 是跨口径对比（对 LevelDB 公开数据），不是代码差距**；同机同负载
  读路径为 LevelDB 的 **1.109×（1e7 全流程）** 与 **1.252×（Get-only 三轮中位）**，
  已超过 1.1× 目标。本轮**不改代码**，理由与唯一的下一步见 §4。

---

## 1. 0.48× 的出处与口径

| 项 | 内容 |
|---|---|
| 数字出处 | `docs/reports/asm-storage-kv.md:60-68` §3「与 LevelDB 公开数据对比」表：LevelDB（公开）~100 万 ops/s，本实现 47.5 万 ops/s，倍率 **0.48x** |
| 被谁引用 | `docs/reports/three-piece-audit.md:43` A14（备注写 `asm-storage-kv.md:15-18`，即 §1 的 `475,125` 那一行）、`:200` 的收口顺序第 4 项 |
| 对比对象 | **LevelDB 的公开/文献数据**（~1e6 ops/s），不是同机实测；同一张表的「写」列也是公开区间 40~60 万 |
| 本实现口径 | 1e7 条 Put 后随机 Get：490 MB 工作集、4 段、每次平均 2.5 次段级 Bloom + 一次索引二分 |
| 参考侧口径 | 公开数据通常是小数据集、单段、块缓存热（或数据全在内存） |
| 台账已有反证 | `docs/metrics-ledger.md` M07（2026-11-03 同机同 harness）：Get **530,742（LevelDB）vs 576,109（自研）= 1.086×** |

即：`asm-storage-kv.md` 的数字是「本机 490 MB 随机读」对「LevelDB 公开的最好成绩」，
两者机器、数据规模、缓存状态都不同；台账 M07 已用同机实测推翻 0.48×，但
`asm-storage-kv.md` 与审计报告没有回写，导致审计继续引用旧值。

---

## 2. 同机复现

### 2.1 复现方式

仓库 `.build-bento-wsl` 的 `GO2CPP_WITH_LEVELDB:BOOL=OFF`，`third_party/` 也没有
leveldb 源码，所以台账 M07 的 `go2cpp_leveldb_kv_test` 不能直接构建。本轮的处理：

1. 用系统里已有的 LevelDB 源码（`/mnt/e/temp/leveldb`）编 Release 静态库
   （`libleveldb.a`，CMake Release）；
2. 按仓库 Release 选项手工编译 `tests/storage/test_leveldb_kv.cpp`（含
   `src/storage/{kv,memtable,arena,wal,sstable,leveldb_kv}.cpp` + `src/logging/log.cpp`），
   得到 `/var/tmp/kvbench`；
3. **源码树未改**（本轮没有对仓库做任何修改），基准与 M07 是同一份 harness。

另外自写一个 Get-only 隔离基准 `/var/tmp/kvget`：先各建一个 1e7 条的库，
之后每次只做「打乱 + 1e7 次 Get」，两个引擎交替跑 3 轮，避免写阶段的脏页/compaction
余波污染读数据。

### 2.2 1e7 全流程（同 M07 harness，生产编译选项）

```
taskset -c 2,3 /var/tmp/kvbench --bench --n=10000000
```

| 引擎 | Put ns/op | Put ops/s | Get ns/op | Get ops/s | VmHWM |
|---|---:|---:|---:|---:|---:|
| LevelDB | 1480.0 | 675,697 | 1628.8 | **613,941** | 442.1 MiB |
| 自研 KV | 6132.0 | 163,078 | 1468.6 | **680,915** | 624.3 MiB |

**读倍率 = 680,915 / 613,941 = 1.109×**（同一轮、同一台机器、同一份 key 序列）。

> 首轮（未加 `-DNDEBUG` 的对照构建）曾得到 LevelDB 503,612 / 自研 684,303 = 1.359×；
> LevelDB 侧跑间波动约 ±20%（503k→614k→687k），所以下文用 Get-only 三轮中位作为
> 主结论，全流程数字只作同 harness 的旁证。

### 2.3 Get-only 隔离（预建 1e7 库，交替 3 轮）

| 轮次 | LevelDB Get ops/s | 自研 KV Get ops/s | 倍率 |
|---|---:|---:|---:|
| 1 | 686,509 | 860,176 | 1.253× |
| 2 | 687,216 | 848,826 | 1.235× |
| 3 | 717,041 | 898,362 | 1.253× |
| **中位** | **687,216** | **860,176** | **1.252×** |

两个引擎都是 10,000,000/10,000,000 命中；KV 侧轮间离散 5.5%，比值稳定在 1.235–1.253×。

### 2.4 仓库自带 kv_bench（2e6，交叉验证）

`taskset -c 4,5 .build-bento-wsl/go2cpp_kv_bench --n=2000000`：

| 场景 | ns/op | ops/s |
|---|---:|---:|
| KV(ext4) Get 随机命中 | 909.65 | **1,099,327** |
| KV(tmpfs) Get 随机命中 | 892.48 | 1,120,478 |
| KV Get 8 线程 | 706.26 | 1,415,914 |
| MemTable Get（2e6 常驻） | 301.56 | 3,316,050 |
| 64 MiB 随机访存（依赖链） | 117.80 | — |
| fdatasync(4KiB) | 475.2 µs（中位） | — |

2e6 规模（~92 MB、刷盘少、末段 MemTable 命中多）下读已到 **110 万 ops/s**，
说明 47.5 万/s 这个数本身强烈依赖「1e7 + 490 MB + 4 段」这个最坏口径。

---

## 3. 读路径架构审计

### 3.1 结构（`src/storage/kv.cpp:767-817`）

1. `mutable_ptr_` 一次 acquire load，命中走 `MemTable::Lookup`：开放寻址哈希
   （16B 槽、平均 1.3 次探测、容量固定不 rehash）→ **无锁、无读者注册、无分配**；
2. 未命中才进慢路径：`EpochManager::Guard`（每线程独占槽，一次 epoch load + 一次
   release store，析构回 idle）保护快照指针 → immutable MemTable 线性查；
3. SSTable：`MayContain(key, hash)`（Bloom）→ `FindBlock`（索引 offset 数组二分）
   → 4 KB 块内顺序扫描（约 40 条，命中即停）→ `value->assign` 拷到调用方缓冲；
4. 全程 mmap，无 `read()/pread()`，无 CRC 校验（校验只在恢复/构建侧）。

### 3.2 objdump 取证（当前 `-O3` 构建，`asm_stats.py` + `objdump -d`）

| 函数 | 指令 | call | `lock` | 分配 | mutex | 栈帧 |
|---|---:|---:|---:|---:|---:|---:|
| `KV::Impl::Get` | 320 | 17 | **12** | 0 | 0 | 40 B |
| `MemTable::Lookup` | 229 | 2 | **0** | 0 | 0 | 40 B |
| `SSTable::Get` | 253 | 5 | 0 | 0 | 0 | 104 B |
| `SSTable::FindBlock` | 79 | 1 | 0 | 0 | 0 | 40 B |
| `SSTable::MayContain(key)` | 168 | 0 | 0 | 0 | 0 | 0 |

- 12 个 `lock` 全是分片统计的 `fetch_add`（7 个调用点在 `Counters::TlsShard`），
  没有一把 mutex/futex；读路径不持写锁。
- 0 次堆分配、0 次 `operator new`；`value` 的 `assign` 复用调用方 string 容量
  （基准里 `scratch` 只扩容一次）。
- `MayContain(key)` 的 168 条指令包含内联 xxHash32；`FindBlock` 的 79 条里
  没有 `call` 到比较函数（`Compare` 内联为 memcmp）。

### 3.3 与 LevelDB 的数据结构差异

| 层 | LevelDB | 本实现 | 读路径影响 |
|---|---|---|---|
| MemTable | 跳表（每层指针追踪，2~3 次 cache miss） | 开放寻址哈希（1.3 次探测） | 内存命中快 3~5×（3.3M/s vs 报告对照 1.55M/s） |
| 块内查找 | restart 点 + 线性扫描 | offset 数组 + 二分 | 块内更快；4 KB 顺序扫描是实测折中 |
| 读缓存 | 8 MB block cache（随机读命中率低） | mmap 零拷贝（靠页缓存） | 490 MB 工作集下 mmap 更省一次拷贝 |
| 索引 | 索引块 + 二分 | 索引块 + offset 数组二分 | 同量级 |
| Compaction | 后台线程 | 写锁内同步 | 写路径的代价，读路径不受影响 |

---

## 4. 本轮是否改代码：**不改**（已达标，且没有不越界的有效改动）

1. **目标已达成**：1.109×（全流程）/ 1.252×（Get-only 中位），均 ≥1.1×。
2. `kv.cpp` 内剩余可动的只有两处，收益都 <2%，且有语义代价：
   - 命中路径 3 次分片计数 `fetch_add`（`lock addq`，约 15–20 ns，占单次 Get
     1.3–1.7%）；去掉/合并会让 `GetStats()` 的 gets/hits/memtable_hits/sstable_hits
     语义变化；
   - `EpochManager::Guard` 的两次 release store（约 5 ns，<0.5%）；去掉会破坏
     `Retire/Reclaim` 的延迟回收安全性（可能 use-after-free）。
3. **发现一处真正的结构性冗余**（下一步，需改 `src/storage/sstable.{hpp,cpp}`，
   不在本任务允许修改的文件范围）：
   `KV::Impl::Get` 先 `table->MayContain(key, key_hash)`，随后 `SSTable::Get`
   内部又调 `MayContain(key)` 重新 `Hash32` + 再探一次 Bloom —— 命中段每次
   Get 重复一次哈希 + 一次 Bloom 探测，按本机数据估算 30–60 ns（3–5%）。
   修法：给 `SSTable::Get` 增加 `bool bloom_checked`（或 `GetBloomVerified`），
   KV 侧二选一调用即可，语义与统计可保持精确。

---

## 5. 为什么再快不了（物理下限）

- Get-only 单次 **1162.6 ns**；同机 64 MiB 随机依赖链下限 117.8 ns（旧报告 1e7
  口径 155.1 ns）→ **7.5~9.9× 下限**。
- 每次 Get 平均 2.5 次段级 Bloom（4 段，每段 Bloom 约 3 MB）+ 约 15 次索引二分
  （每段索引 ~900 KB）+ 1 次 4 KB 块扫描，合计约 4~6 次 LLC/TLB miss；
  490 MB 工作集远超 32 MiB L3。
- LevelDB 的「~100 万/s」对应单段、热缓存；**同机同负载它只有 61~72 万/s**。
  因此 0.48× 的差距是口径，不是可优化的代码路径。

---

## 6. 正确性

本轮未改代码，仍跑三组测试确认基线健康：

| 测试 | 结果 |
|---|---|
| `go2cpp_kv_test` | **52,665 checks, 0 failures** |
| `go2cpp_kv_concurrent_test` | **10,031 checks, 0 failures**（8 线程并发读写/刷盘） |
| `go2cpp_kv_recovery_test` | **22 checks, 0 failures**（含 crash_during_flush 恢复 19,626/20,000） |

`ctest --test-dir .build-bento-wsl -L storage` 在该构建里返回 `No tests were found`
（storage 测试没有注册 label），所以直接执行上述三个二进制。

---

## 7. 顺带观察（写路径，建议另立任务）

同一 harness、同一轮 1e7：

| 引擎 | Put ns/op | Put ops/s |
|---|---:|---:|
| LevelDB | 1480.0 | 675,697 |
| 自研 KV | 6132.0 | **163,078** |

- 与台账 M07 的 647,521 ops/s 差 4×；但 `go2cpp_kv_bench --n=2000000`
  （32 MB memtable、几乎不触发 compaction）Put 仍有 **1,924,739 ops/s**。
- 指向「1e7 + 默认 8 MB memtable 下的同步 compaction/刷盘」被放大（写锁内合并），
  也可能叠加本轮并发 I/O 干扰；**未定位，不写入结论**。建议单独开
  「写路径 Put 回退排查」任务，用 8 MB/32 MB memtable 对照 + fsync 计数取证。

---

## 8. 建议回写

- `docs/reports/three-piece-audit.md` A14：读倍率 0.48× → **1.25×**（同机 Get-only
  中位；全流程 1.11×）。
- `docs/metrics-ledger.md` M07：追加 2026-11-04 复测（全流程 1.109× / Get-only 1.252×），
  并注明 LevelDB 侧轮间波动 ±20%，以 Get-only 中位为准。
- `docs/reports/asm-storage-kv.md` 已追加 §7 复核说明。


## 写路径定位（2026-11-04）

### 1. 复现

同 harness（key 20B / value 22B，单线程，`taskset -c 2,3`，默认 8 MB memtable，
`sync_writes=false`）：

| 引擎 | Put ns/op | Put ops/s | 来源 |
|---|---:|---:|---|
| LevelDB（同轮对照） | 1554.8 | 643,160 | `/var/tmp/kvput --ldb` |
| 自研 KV（修复前） | 6262.1 | **159,690** | 与 §2.2 的 163,078 一致 |

### 2. 根因：合并策略造成 36× 写放大，且全部在写锁内

旧 `KV::Impl::MaybeCompactLocked()` 每次固定取「最旧两段」合并，而
`max_sstables = 4` 意味着第 5 段起**每写满一个 8 MB memtable 就合并一次最旧两段**。
最旧的大段因此被之后每一个新段反复重写（二次方）：

| 配置（1e7 Put，8 MB memtable） | Put ops/s | `bytes_written` | 写放大 | 结束段数 |
|---|---:|---:|---:|---:|
| max_sstables=4（默认，基线） | 159,690 | 17,688 MiB | **36×** | 4 |
| max_sstables=8 | 168,702 | 15,870 MiB | 32× | 8 |
| max_sstables=16 | 251,335 | 12,537 MiB | 25× | 16 |

- 62.6 s 里约 55 s 是 compaction 重写（17.7 GiB ÷ 实测 322 MB/s ≈ 55 s）；
- 提高 `max_sstables` 只减小常数、不改变二次方性质；
- **WAL 不是瓶颈**：`sync_writes=false`（默认）下 `wal_syncs=0`，只有 1 MB 用户态
  缓冲的 `write()`；全仓 `fdatasync/fsync` 只出现在 `WalWriter::Sync()`（group commit，
  仅在 `sync_writes=true` 时调用）与 `SSTableBuilder::Finish()` 的收尾；
- **`kv_bench` 2e6 的 1.92 M/s 来自配置差异**：`tests/storage/kv_bench.cpp:262-266`
  用 32 MB memtable，92 MB 数据只有 3 次 flush、几乎不触发合并；不是引擎更快。

### 3. 改动（`src/storage/kv.cpp`，+29/−8，未 commit）

1. **几何合并**：`MaybeCompactLocked()` 不再固定取最旧两段，而是扫描所有相邻段对，
   取**大小最接近**（`hi*1024/lo` 最小）的一对合并——相邻保证 key 遮蔽顺序不变，
   大小相近让每段只在遇到同量级邻居时才重写，单字节重写 O(log N) 次；
2. **原地替换区间**：发布时把 `[first, first+2)` 换成新段（而不是 `resize(keep)` +
   尾插），中间位置的合并结果仍保持「新→旧」顺序；
3. **墓碑丢弃条件**修正为 `inputs.size() == total`（只有合并覆盖全部段时才可丢）。

> 已验证但未采用：后台 compaction 线程（I/O 在锁外，Put 阶段 1.73 M/s、写放大 4×）。
> 它与现有测试「未 `Close` 就 `std::filesystem::remove_all(dir)`」冲突——后台线程还在
> 写目录，`remove_all` 抛 `filesystem_error: Directory not empty`（`kv_test` 崩溃）。
> 要保留异步就必须改测试或加生命周期协议，超出本次「只改 `src/storage`」的边界，
> 故只保留几何策略；单它已达标。

### 4. 前后对比（1e7 Put，同机同 harness）

| 指标 | 修复前 | 修复后 | 变化 |
|---|---:|---:|---:|
| Put ops/s | 159,690 | **703,517** | **4.4×** |
| Put ns/op | 6262.1 | 1421.4 | |
| 写放大（`bytes_written`） | 17,688 MiB | 3,464 MiB | 36× → 7× |
| Put 阶段 wall | 62.6 s | 14.21 s | |
| 结束段数 | 4 | 4 | 不变 |
| 1e7 随机 Get ops/s | 680,915（§2.2） | **820,857** | 读无退化（段数仍 4） |
| 同轮 LevelDB Put | 643,160 | 643,160 | **1.09×**（0.9× 目标 = 578,844） |

**已达 0.9× LevelDB 目标**（703,517 ≥ 578,844，实际 1.09×）；`close` 0.17 s。

### 5. 正确性

| 测试 | 结果 |
|---|---|
| `go2cpp_kv_test` | 52,665 checks, 0 failures（0.88 s） |
| `go2cpp_kv_concurrent_test` | 10,031 checks, 0 failures |
| `go2cpp_kv_recovery_test` | 22 checks, 0 failures（crash_during_flush 19,626/20,000） |

`g++ -Wall -Wextra -Wpedantic` 编译 `src/storage/kv.cpp` 零告警；改动未提交。

### 6. 遗留

- 写放大仍有 7×（几何合并每字节约 log2(61)≈6 次 + 首次落盘）。再降需要 4 路归并
  （几何级数变 4×）或多层 LSM，但要同步改读侧段数预算；
- 后台合并是写路径的下一个数量级（1.73 M/s），但需要配套目录生命周期协议，
  建议单独任务：改测试的清理方式 + 给 `KV` 增加显式 `Close` 约束或合并线程 join 钩子。
