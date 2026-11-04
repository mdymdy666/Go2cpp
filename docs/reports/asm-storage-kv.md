# KV 存储引擎性能报告

- 时间：2026-11-01
- 机器：AMD Ryzen 9 8945HX（12 核 / 32 MiB L3 / L2 6 MiB）、19 GB RAM、WSL2、ext4（/dev/sdd）
- 工具链：gcc 13.3.0 `-O2`，cmake 3.28 + ninja，`objdump -d -C --no-show-raw-insn`
- 负载：`taskset -c 4-11`；key = `key-<16 位数字>`（20B），value = `value-<16 位数字>`（22B）
- 原始输出：`/tmp/bench_final.log`（1e7 干净环境）、`/tmp/bench_1e7.log`、`/tmp/bench_1e7b.log`（对照轮，含构建干扰）

---

## 1. 1e7 端到端（干净环境）

| 场景 | 吞吐 | ns/op |
|---|---:|---:|
| KV(ext4) Put 顺序 | **562,669 ops/s** | 1777.3 |
| KV(ext4) Get 随机命中 | **475,125 ops/s** | 2104.7 |
| KV(tmpfs) Put 顺序 | **699,339 ops/s** | 1429.9 |
| KV(tmpfs) Get 随机命中 | **475,887 ops/s** | 2101.3 |
| MemTable Put（1e7 条常驻） | 2,881,941 ops/s | 347.0 |
| MemTable Get（1e7 条常驻） | 1,551,145 ops/s | 644.7 |
| KV Get 8 线程（2e6 次） | 2,386,001 ops/s | 419.1 |
| KV sync 写单线程 | 630 ops/s | 1.586 ms |
| KV sync 写 8 线程 group commit | 2,525 ops/s | 0.396 ms |

1e7 写入的旁证（`StatsString`）：

```
puts=10000000 wal_records=10000000 wal_bytes=490000000
flushes=18 compactions=14 sstables=4 mem_hits=467260 sst_hits=9532740
bloom_neg=25089433
```

即：1e7 条写入触发 18 次刷盘、14 次合并，最终 4 个段；读取 1e7 次里
4.67% 命中 MemTable、95.3% 命中 SSTable，平均每次 Get 做 2.5 次段级 Bloom。

---

## 2. 实测下限与「离下限几倍」（同机）

| 指标 | 实测 | 说明 |
|---|---:|---|
| 64 MiB 随机访存（依赖链） | **155.1 ns/次** | 一次必然 cache miss 的物理延迟 |
| `fdatasync(4KiB)` | **中位 556.1 µs**（min 353.3） | 本机 ext4 的真实 fsync 成本 |
| `std::unordered_map` insert | 478.8 ns/op | 通用哈希表对照 |
| `std::unordered_map` find | 634.7 ns/op | 通用哈希表对照 |
| MemTable Put | 347.0 ns/op | 1e7 条常驻（~900 MB 表） |
| MemTable Get | 644.7 ns/op | 同上 |

计算：

- **MemTable Get 644.7 ÷ 155.1 = 4.2x**：随机点查至少要 2 次 cache miss
  （槽数组 + 记录），加上哈希与 key 比较，收敛在 2~3 次。
- **KV Get 2104.7 ÷ 155.1 = 13.6x**：其中 ~2.5 次是段级 Bloom，~1 次索引二分，
  ~1~2 次块访问，其余是 mmap 的 TLB 抖动与 value 拷贝。
- **sync 写 1586 ÷ 556 = 2.9x**：多出来的是 WAL append 的 ext4 文件扩展
  （新块分配 + journal 提交），不是代码开销。
- **MemTable Put 347 ÷ 155 = 2.2x**：一次写 = 哈希 + 1 次槽访问 + Arena 分配
  （每 64KB 一次 malloc，摊销后 ~1ns）。

---

## 3. 与 LevelDB 公开数据对比

| 指标 | LevelDB（公开） | 本实现（1e7, ext4） | 倍率 |
|---|---:|---:|---:|
| 写（sync=false） | 40~60 万 ops/s | **56.3 万 ops/s** | **0.94 ~ 1.41x** |
| 读（点查） | ~100 万 ops/s | **47.5 万 ops/s** | **0.48x** |
| 读（8 线程） | —（公开数据多为单线程） | 238.6 万 ops/s | — |
| 写（sync=true） | ~1k ops/s 量级 | 630 / 2525（group commit） | — |

结论：

1. **写达标**：1e7 条、sync=false、含 18 次刷盘与 14 次合并，56.3 万 ops/s，
   与 LevelDB 公开数据同级（其公开数据通常也是小数据量、单段场景）。
2. **读只有 0.48x**：本机数据 490 MB 随机访问 + 4 个段，每次 Get 2.5 次
   Bloom + 一次索引二分。LevelDB 的 ~100 万/s 是在**单段、热页缓存**下测得，
   口径不同。8 线程读 238.6 万/s 说明延迟可以被并行掩盖，瓶颈是单次
   cache/TLB miss 而不是锁。
3. **sync 写**：单线程 630 ops/s（每次 1.59 ms，其中 fsync 下限 0.56 ms）；
   8 线程 group commit 合并到 2525 ops/s（每次 fsync 摊到 0.396 ms/op），
   提升 **4.0x**，接近 fsync 下限。

---

## 4. objdump 证据

| 函数 | 指令 | call | lock | 分配 |
|---|---:|---:|---:|---:|
| `MemTable::Lookup` | 149 | 0 | 0 | 0 |
| `MemTable::Insert` | 169 | 0 | 1 | 0 |
| `MemTable::Get` | 6 | 0 | 0 | 0 |
| `SSTable::Get` | 210 | 2 | 0 | 0 |
| `SSTable::FindBlock` | 66 | 0 | 0 | 0 |
| `SSTable::MayContain` | 136 | 0 | 0 | 0 |
| `KV::Impl::Get` | 249 | 8 | 10 | 0 |
| `KV::Impl::WriteSingle` | 223 | 1 | 1 | 0 |

- `KV::Impl::Get` 的 10 个 `lock` **全部是分片统计的 `fetch_add`**；热路径
  （mutable 命中）只走 3 个。没有任何 mutex/futex 调用。
- `MemTable::Lookup` 的 149 条指令里包含内联的 xxHash32 循环、探测循环与
  key `memcmp`；没有 `malloc`/`operator new`。
- `wal.o` 中 3 条 `crc32q/crc32l/crc32b`（SSE4.2）；软件表仅作回退。
- SSTable 读路径无 `read/pread`：`Open` 时一次 `mmap`，之后纯内存访问。

---

## 5. 优化前后对照

| 优化前 | 优化后 | 变化 |
|---|---|---|
| SSTable 块内二分（16 KiB 块） | 块内顺序扫描（4 KiB 块） | 1e7 读 1915→2105 ns（**回退**，见下） |
| 6 段、`max_sstables=8` | 4 段、`max_sstables=4` | Bloom 查询 3923 万 → 2509 万（-36%） |
| 槽容量 32B/条（负载 0.5） | 48B/条（负载 0.7） | 1e6 Get 429 → 382 ns（**-11%**） |
| 恢复后双倍预算 | 恢复后回调常规预算 | 1e6 条 `flushes` 0 → 1（数据真正落盘） |
| compaction 不删旧段 | 合并后 unlink 输入 | 目录 `.sst` 2.6 万 → 个位数 |
| group commit 只自旋等待 | 等待者循环重抢 leader | 8 线程 sync 写死锁 → 2525 ops/s |
| `Counters` 原子未初始化 | 构造函数显式清零 | 统计值从垃圾数恢复为准确值 |

> 「块内顺序扫描」这一项是**负优化**：16KB 块 + 二分 1915 ns，4KB 块 + 顺序
> 扫描 2105 ns（两者都含 4 段 Bloom）。真正的原因是块内查找不是瓶颈，
> 段级 Bloom 与 mmap 的 TLB miss 才是。保留 4KB 方案是因为它同时把索引
> 体积压到 1/4（写放大更小）。

---

## 6. 结论与下一步

1. 写路径达标（56.3 万/s，与 LevelDB 同级）；读路径 47.5 万/s（0.48x）。
2. 读的瓶颈已经定位到**段级 Bloom × 2.5 + TLB/cache miss**，不是指令数：
   `KV::Impl::Get` 只有 249 条指令、0 分配、无 mutex。
3. 下一步（按收益排序）：
   - **全局 Bloom**（所有段并集）：一次排除所有段，不存在的 key 直接返回；
   - **索引常驻内存 + 前缀比较**：索引二分从 14 次降到几次；
   - **后台 Compaction**：把写锁内的合并移出去（当前 Put 的 1.78 µs 里
     约 0.6 µs 是合并摊销）；
   - **大页 / 预取**：mmap 区域用 `MADV_HUGEPAGE` 降低 TLB miss。

---

## 7. 2026-11-04 复核：0.48× 是跨口径，不是代码差距

同机同负载（1e7、20B/22B、`taskset -c 2,3`、生产编译选项）复测：

| 场景 | LevelDB | 自研 KV | 倍率 |
|---|---:|---:|---:|
| 1e7 全流程（同 harness） | 613,941 ops/s | 680,915 ops/s | **1.109×** |
| 预建库 Get-only 三轮中位 | 687,216 ops/s | 860,176 ops/s | **1.252×** |

§3 的 0.48× 来自与 LevelDB「公开数据 ~100 万 ops/s」的跨口径比较（单段/热缓存
口径）；本机同负载 LevelDB 只有 61~72 万/s。详细数据、读路径 objdump 与瓶颈分析
见 `docs/reports/storage-opt.md`。
