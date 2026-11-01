# KV 存储引擎（go2cpp/storage）

零依赖、嵌入式、单进程多线程的 KV 存储。接口在 `include/go2cpp/storage/kv.hpp`，
实现在 `src/storage/`（约 2.6k 行，LevelDB 的 1/10）。只依赖 POSIX
（`open/pwrite/mmap/fdatasync`）与 pthread。

设计参考 LevelDB 的 MemTable / WAL / SSTable / Arena / WriteBatch，
但刻意砍掉了 MVCC、快照读、多级 Compaction、MANIFEST、版本编辑这些复杂度大头。

---

## 1. 目标与边界

| 项 | 取值 |
|---|---|
| 数据模型 | 单版本 KV，`std::string_view` 进、`std::string` 出 |
| 写语义 | `WriteBatch` 原子（WAL 单条记录 + 内存一次应用） |
| 崩溃语义 | `sync_writes=false`：已 `write()` 进内核的都在；`sync_writes=true`：返回即落盘 |
| 并发 | 读无锁（RCU/epoch 延迟回收），写串行（一把写锁），fsync group commit |
| 容量 | 单机单进程，数据集 ≤ 磁盘；内存占用 ≈ memtable + 索引 + 打开段 |
| 不支持 | 事务隔离级别、多版本快照、范围删除、前缀 Seek、压缩、加密 |

---

## 2. 架构

```
          Put/Delete/Write                     Get/NewIterator
                │                                    │
                ▼                                    ▼
        ┌───────────────┐                  ┌──────────────────┐
        │  写锁 mutex_   │                  │ mutable MemTable │  ← 无锁，1 次原子 load
        └───────┬───────┘                  └────────┬─────────┘
                │                                   │ miss
      ┌─────────▼─────────┐              ┌──────────▼──────────┐
      │ WAL 追加（1MB buf）│              │ epoch Guard（读侧） │
      │ + group commit     │              └──────────┬──────────┘
      └─────────┬─────────┘                         │
                │                        ┌──────────▼──────────┐
      ┌─────────▼─────────┐              │ immutable MemTable  │
      │ MemTable（哈希）   │              │ （刷盘中，仍可读）   │
      └─────────┬─────────┘              └──────────┬──────────┘
                │ 满                             │ miss
      ┌─────────▼─────────┐              ┌──────────▼──────────┐
      │ SSTable（mmap）    │              │ SSTable × N（新→旧） │
      │ 块 + 索引 + Bloom  │◄─────────────┤ Bloom → 索引 → 块   │
      └─────────┬─────────┘              └─────────────────────┘
                │ 段数超限
      ┌─────────▼─────────┐
      │ Compaction（归并） │
      └───────────────────┘
```

组件与文件：

| 文件 | 职责 |
|---|---|
| `src/storage/arena.{hpp,cpp}` | 块式内存池（64KB，随 memtable 预算缩放） |
| `src/storage/memtable.{hpp,cpp}` | 开放寻址哈希表 + 槽位原子发布 |
| `src/storage/wal.{hpp,cpp}` | WAL 读写 + CRC32C（SSE4.2 / 软件表） |
| `src/storage/sstable.{hpp,cpp}` | 块构建/索引/Bloom/mmap 读取/归并迭代器 |
| `src/storage/kv.cpp` | KV 主逻辑、epoch 回收、刷盘、合并、迭代器 |
| `src/storage/format.hpp` | varint / xxHash32 / 类型定义（header-only） |

---

## 3. 磁盘格式

### 3.1 WAL（`NNNNNN.log`）

与 LevelDB log 同构：64KB 逻辑块，记录头 7 字节
`[crc32c:u32][len:u16][type:u8]`，`type ∈ {Full=1, First=2, Middle=3, Last=4}`。
CRC 覆盖 `type + payload`，落盘前做 LevelDB 同款掩码（`mask(crc)`）。
一条逻辑记录超过块剩余空间时按 `First/Middle/Last` 分段；块尾不足 7 字节用 0 填充。

批量：`WriteBatch` 整体序列化为**一条** WAL 记录（`[count:u32] + entries`），
因此一次批写只有一次记录头与一次 `write()`。

### 3.2 SSTable（`NNNNNN.sst`）

```
[data block 0] ... [data block N-1] [bloom] [index] [footer 48B]
```

数据块（16KB 目标）：

```
[count:u32][offsets_start:u32] + entries + offsets[count]（u32，相对块首）
entry: [klen:varint][key][vlen:varint][type:u8][value]
```

块内 offsets 数组让读端可以二分（LevelDB 是 restart 点 + 线性扫描）。

索引块同布局，条目为 `[last_key][offset:varint][size:varint]`；读端二分定位块。

Bloom：`[bytes:u32][k:u8][bits...]`，默认 10 bits/key、k = 6（`bits*0.69`）。

footer：`magic(8) index_off(8) index_size(8) bloom_off(8) bloom_size(8) num_entries(8)`。

写入顺序：`<path>.tmp` → `fdatasync` → `rename`，保证崩溃时要么没有新段、要么是完整段。
合并完成后旧段立即 `unlink`（已 mmap 的读者仍持有 inode）。

### 3.3 文件编号语义

- `NNNNNN.log` / `NNNNNN.sst` 共用一个单调递增编号；
- 打开时按编号升序重放所有 `.log`，按编号降序（新→旧）打开 `.sst`；
- 刷盘流程：写新 `.sst`（fsync+rename）→ 开新 `.log` → 删旧 `.log`；
- 崩溃在任意一步都幂等：旧 log 的数据要么已在 sst、要么重放后与 sst 一致。

---

## 4. 并发与一致性

### 4.1 读路径（无锁）

1. **快路径**：`mutable_ptr_.load(acquire)` 后直接在哈希表里查。
   槽位用 `state`（empty/constructing/occupied/deleted）原子发布，读者看到
   `constructing` 重试；x86 上 acquire load 不产生额外指令。命中即返回，
   **不碰锁、不注册读者、不碰 epoch**。
2. **慢路径**：未命中才构造 `EpochManager::Guard`（每个线程独占一条
   cache line 的槽，写当前 epoch，析构恢复），然后读 immutable 列表与
   SSTable 列表（`shared_ptr` 快照）。

### 4.2 回收（epoch / RCU）

- 快照（`Snapshot{immutables, sstables}`）是不可变对象，发布用
  `atomic<const Snapshot*>::exchange(release)`；旧快照交给 `Retire()`。
- `Retire()` 记录当时的 epoch 并推进全局 epoch；`Reclaim()` 扫描所有线程槽，
  取最小活跃 epoch，释放 `retire_epoch < min_active` 的对象。
- 正确性要点：读者先读 epoch 再写自己的槽，然后才读快照；回收者先发布新
  快照、再推进 epoch、再扫描。任何在发布前进入的读者都会让 `min_active`
  停留在旧值，回收被推迟而不是提前。

### 4.3 写路径与 group commit

- 写锁串行化：WAL 追加（用户态 1MB 缓冲）→ 应用到 MemTable → 检查刷盘。
- `sync_writes=true` 时 fsync 在**锁外**做：写者解锁后抢 `sync_mu_`，
  抢到的线程成为 leader：短暂持写锁把 WAL 缓冲 `write()` 进内核，
  记录 `issued_` 水位，然后 `fdatasync`，最后把 `sync_done_` 推到该水位。
  等待者必须回到循环顶部重新抢 leader 资格（只自旋等水位会死锁，见 §7.4）。

### 4.4 可见性

- 写完成（返回 true）即对后续读可见（同一线程或其它线程）：
  MemTable 槽位 release 发布 + 读者 acquire。
- 刷盘期间旧 MemTable 变成 immutable，读者仍能查到（`Snapshot` 里保留引用）；
  新 MemTable 立即接管写入。顺序是「先挂 immutable，再换 mutable」，
  反过来会丢数据。

---

## 5. 与 LevelDB 的差异

| 维度 | LevelDB | 本实现 | 取舍理由 |
|---|---|---|---|
| MemTable | 跳表（O(log n)，指针追逐） | 开放寻址哈希（16B 槽，SoA） | 短 key 点查快 3~5 倍；代价是丢掉了有序遍历，迭代器改为「快照 + 排序」 |
| 迭代器 | 跳表/SSTable 归并，流式 | 快照收集 + 排序去重 | 实现简单；内存 O(条目数)，只适合中小数据集 |
| WAL 写 | 每条记录 `Append`+`Flush` | 1MB 用户态缓冲批量 `write()` | 系统调用降 2~4 个数量级 |
| WAL fsync | 每条写一次 fsync | group commit（一次 fsync 覆盖一批） | 并发 sync 写吞吐 ~5x |
| CRC | 软件表 + 运行时检测 | SSE4.2 `crc32` 指令 | 0.4 cycle/B vs ~3 cycle/B |
| SSTable 块内查找 | restart 点 + 线性扫描（16 条一组） | offset 数组 + 二分 | 块内 ~5 次比较，读放大更小 |
| 压缩 | Snappy 可选 | 无 | 省掉解压分支；代价是磁盘占用更大 |
| Bloom | 每段一个 | 每段一个（同款双哈希） | 一致 |
| Compaction | 多级（0~6 级）+ 后台线程 + MANIFEST | 段数超限时合并最旧的 N 段，写锁内完成 | 省掉 MANIFEST/版本编辑；代价是写放大更高、flush 有抖动 |
| 删除 | tombstone + 多级合并 | tombstone，合并全部段时才丢弃 | 一致 |
| 快照/MVCC | 支持 | 不支持（单版本） | 砍掉版本链与引用计数 |
| 读并发 | 每读 `Version::Ref/Unref`（原子） | 快路径零原子；慢路径 epoch 槽 | 热读更便宜 |
| 崩溃恢复 | MANIFEST + log 重放 | 扫目录 + 全部 log 重放（幂等） | 省掉 MANIFEST，靠编号与幂等 |

---

## 6. 源码级优化与证据（objdump）

工具：`g++ 13.3 -O2 -g`，`objdump -d -C --no-show-raw-insn`。统计口径：函数体的
指令数 / `call` 数 / `lock` 前缀数 / 分支数 / 分配调用数。

| 函数 | 指令 | call | lock | 分配 | 说明 |
|---|---:|---:|---:|---:|---|
| `MemTable::Lookup` | 149 | 0 | 0 | 0 | 哈希 + 探测 + key 比较全内联；无锁、无函数调用 |
| `MemTable::Insert` | 169 | 0 | 1 | 0 | 唯一的 `lock` 是 `size_.fetch_add`（统计条目数） |
| `MemTable::Get` | 6 | 0 | 0 | 0 | 纯转发，内联后为零开销 |
| `SSTable::Get` | 210 | 2 | 0 | 0 | 两次二分；`call` 是 `MayContain`/`memcmp` |
| `SSTable::FindBlock` | 66 | 0 | 0 | 0 | 索引二分（varint 解码内联） |
| `SSTable::MayContain` | 136 | 0 | 0 | 0 | Bloom 位测试，无分支预测失败惩罚 |
| `KV::Impl::Get` | 249 | 8 | 10 | 0 | 10 个 `lock` 全是分片统计的 `fetch_add`；热路径（mutable 命中）只走 3 个 |

关键结论：

1. **读热路径没有 `lock` 前缀以外的同步原语**：`KV::Impl::Get` 的 10 个
   `lock` 全是统计计数器（`lock addq`），没有 mutex/futex 调用；
   把统计关掉后只剩 x86 的普通 `mov`（acquire load 在 x86 不产生指令）。
2. **没有分配**：`MemTable::Lookup`/`Insert`/`SSTable::Get` 的 objdump 里没有
   `malloc`/`operator new`/`operator delete` 调用；写入的内存来自 Arena 的
   64KB 块（1e7 次写入 ≈ 1.6 万次 `malloc`，而不是 1e7 次）。
3. **CRC32C 走硬件**：`wal.asm` 里有 3 条 `crc32q/crc32l/crc32b`
   （SSE4.2），软件表只在无 SSE4.2 的机器上作为回退。
4. **SSTable 读无系统调用**：`SSTable::Get` 的 `call` 只有 `MayContain` 与
   `memcmp`，没有 `read/pread/mmap`——数据在 `SSTable::Open` 时一次 mmap，
   之后是纯内存访问。

哈希函数选择：`internal::Hash32` 是 xxHash32 风格（4 路并行、每 16 字节
一次循环），`std::hash<std::string>`（libstdc++ 的 MurmurHash2 逐字节）
在 20B key 上实测慢 ~2.5x；这也是 `MemTable::Lookup` 只有 149 条指令的原因。

---

## 7. 性能

环境：AMD Ryzen 9 8945HX（12 核 / 32MiB L3）、19GB RAM、WSL2 ext4（/dev/sdd）、
gcc 13.3 `-O2`。`taskset -c 4-11`。key = `key-<16 位数字>`（20B），
value = `value-<16 位数字>`（22B）。

### 7.1 1e7 端到端（干净环境，`/tmp/bench_final.log`）

| 场景 | 吞吐 | ns/op |
|---|---:|---:|
| KV(ext4) Put 顺序 | 562,669 ops/s | 1777.3 |
| KV(ext4) Get 随机命中 | 475,125 ops/s | 2104.7 |
| KV(tmpfs) Put 顺序 | 699,339 ops/s | 1429.9 |
| MemTable Put（1e7 条常驻） | 2,881,941 ops/s | 347.0 |
| MemTable Get（1e7 条常驻） | 1,551,145 ops/s | 644.7 |
| KV Get 8 线程 | 2,386,001 ops/s | 419.1 |
| KV sync 写单线程 | 630 ops/s | 1.586 ms |
| KV sync 写 8 线程 group commit | 2,525 ops/s | 0.396 ms |

1e7 次写入触发 18 次刷盘 + 14 次合并，稳态 4 个段；1e7 次读取里
4.7% 命中 MemTable、95.3% 命中 SSTable（平均 2.5 次段级 Bloom）。

完整数据与「离下限几倍」见 `docs/reports/asm-storage-kv.md` 与 `/tmp/kv_storage.md`。

### 7.2 实测下限

| 下限 | 实测 | 说明 |
|---|---:|---|
| 64MiB 随机访存（依赖链） | 155.1 ns/访问 | 一次必然 cache miss 的物理延迟 |
| `fdatasync(4KiB)` | 中位 556.1 µs（min 353.3） | 本机 ext4 的实际 fsync 成本 |
| `std::unordered_map::find`（20B key） | 634.7 ns/op | 通用哈希表对照 |
| `std::unordered_map::insert` | 478.8 ns/op | 同上 |

「离下限几倍」：MemTable Get 4.2x（644.7 / 155.1，随机点查至少 2 次 miss）、
KV Get 13.6x（2104.7 / 155.1，其中 2.5 次是段级 Bloom）、sync 写 2.9x
（1586 / 556，差值是 WAL append 的 ext4 文件扩展）。

与 LevelDB 公开数据：写 56.3 万/s 对 40~60 万/s（0.94~1.41x，达标）；
读 47.5 万/s 对 ~100 万/s（0.48x，口径差异见报告）。

### 7.3 设计上的代价

- **迭代器是快照式**：`NewIterator()` 会把可变/不可变 MemTable 与所有
  SSTable 的条目收集、排序、去重。1e7 条数据要几百 MB 临时内存、秒级延迟。
  只适合「小数据集遍历/调试」，不适合全库扫描。LevelDB 的流式归并不受此限。
- **Compaction 在写锁内**：段数超限时会在写路径上阻塞做归并，写延迟出现
  周期性尖峰（毫秒级）。LevelDB 放后台线程。
- **Bloom 只在段级**：段数多时每次 Get 要对每段查一次 Bloom；默认压到 2 段，
  代价是写放大更高（合并更频繁）。
- **无压缩**：磁盘占用约为原始数据的 1.3~1.5 倍（索引 + 块内冗余 + 每段
  固定开销）。

### 7.4 踩过的坑（回归测试覆盖）

| 问题 | 现象 | 根因 | 修复 |
|---|---|---|---|
| 块内偏移错 8 字节 | SSTable 全部读不到 | `BlockBuilder` 的 offset 没算块头 8 字节 | offset 改为 `8 + entries_.size()` |
| 小 memtable 每写必刷 | 2 万次写产生 2.6 万个 sst | Arena 固定 64KB 块 > 32KB 预算 | Arena 块随预算缩放 |
| 旧段文件不删 | 目录里 2.6 万个 `.sst` | Compaction 只更新快照不 unlink | 合并后 unlink 输入段 |
| 恢复后不刷盘 | 1e6 条 `flushes=0` | 恢复用双倍预算建表，之后没调回 | `SetBudget(opt_.memtable_size)` |
| group commit 死锁 | 8 线程 sync 写挂起 | 等待者只自旋等水位，不重新抢 leader | 循环回到顶部重抢 |
| 统计计数器是垃圾值 | `puts=6321187638083559672` | `std::atomic` 默认构造不清零 | `Counters()` 显式清零 |

---

## 8. 测试

| 目标 | 文件 | 覆盖 |
|---|---|---|
| 功能/边界/持久化 | `tests/storage/kv_test.cpp` | Put/Get/Delete/覆盖、空 key、空 value、二进制 key、1MB value、64KB key、WriteBatch 原子性、迭代器 Seek、跨 flush/compaction 读、关闭重开 |
| 崩溃恢复 | `tests/storage/kv_recovery_test.cpp` | `fork + SIGKILL` 后 WAL 重放（sync=true 全量恢复）、WAL 截断（丢最后一条）、WAL 位翻转（损坏点前可读）、刷盘途中 kill（无错值） |
| 并发 | `tests/storage/kv_concurrent_test.cpp` | 8 线程写 + 自读校验、4 读 1 写混合、并发删除、边写边迭代 |
| 基准 | `tests/storage/kv_bench.cpp` | 1e6/1e7 写读、sync 写、8 线程读、下限测量、`unordered_map` 对照 |

运行：

```bash
cmake -S . -B build -DGO2CPP_WITH_KV=ON -DGO2CPP_BUILD_TESTS=ON
cmake --build build --target go2cpp_kv go2cpp_kv_test go2cpp_kv_recovery_test \
      go2cpp_kv_concurrent_test go2cpp_kv_bench
ctest --test-dir build -R go2cpp_kv
./build/go2cpp_kv_bench --n=10000000        # 完整 1e7
```

---

## 9. 限制与后续

- 单进程；多进程同时打开同一目录没有加锁保护。
- 迭代器全量快照（见 §7.3）。
- 无 TTL / 无列族 / 无前缀 Seek。
- 后续可做：后台 Compaction 线程、流式迭代器、块级 CRC、WAL 预分配、
  全局 Bloom（多段查询一次排除）。
