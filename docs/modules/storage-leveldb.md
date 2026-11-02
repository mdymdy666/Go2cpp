# LevelDB 存储后端（go2cpp/storage/leveldb_kv.hpp）

官方 LevelDB 1.23 的 C++ 包装层，接口形状与自研 `go2cpp/storage/kv.hpp` 对齐。
生产路径用 LevelDB，自研 KV 保留为对照实现（两套头文件可在同一个 TU 内共存，
`--bench` 直接逐操作对拍）。

引入动机只有一个：**1e8 级数据量下 RSS 必须与数据量脱钩**。自研 KV 的块索引 /
SSTable 元数据随数据量增长，而 LevelDB 用固定容量 block cache（默认 8 MiB）+
L0–L6 多级 compaction，RSS 由配置而非数据量决定（实测见 §7）。

| 项 | 取值 |
|---|---|
| 后端 | LevelDB 1.23.0（官方静态库 `libleveldb.a`） |
| 头文件 | `include/go2cpp/storage/leveldb_kv.hpp` |
| 实现 | `src/storage/leveldb_kv.cpp`（约 600 行） |
| 构建 | `cmake/Go2CppLevelDb.cmake`，`GO2CPP_WITH_LEVELDB` **默认 OFF** |
| 测试 | `tests/storage/test_leveldb_kv.cpp`（对拍 / 边界 / kill -9 / 1e7 / 1e8） |
| 依赖 | `libleveldb.a` + pthread（POSIX） |

---

## 1. 目标与边界

| 项 | 取值 |
|---|---|
| 数据模型 | 有序 KV（LevelDB 原生 MVCC + 快照） |
| 写语义 | `WriteBatch` 原子（一次 WAL 记录 + 一次可见性切换） |
| 崩溃语义 | `sync_writes=false`：WAL 进内核即返回；`sync_writes=true`：返回前 fdatasync |
| 并发 | 多线程并发读；写由 LevelDB 内部 mutex 串行（group commit） |
| 容量 | 单机单进程，数据集 ≤ 磁盘 |
| 内存 | `memtable + block_cache + 少量索引`，**与数据量无关**（§7） |
| 不支持 | 事务隔离级别、跨进程访问、前缀 Seek 专门 API（用 `Seek(prefix)` 即可） |

与自研 KV 的能力差异：

| 能力 | 自研 KV | LevelDB |
|---|---|---|
| 多版本 / 快照读 | 无 | 原生（`GetSnapshot`） |
| Compaction | 段数超限合并最旧 2 段 | 多级 L0–L6 + 大小触发 |
| 块缓存 | 无（mmap + page cache） | LRU block cache，容量固定 |
| Bloom | 有 | 有（`NewBloomFilterPolicy`） |
| 压缩 | 无 | Snappy/Zstd（本库未链接，见 §5） |
| MANIFEST / 版本编辑 | 无 | 有 |
| 代码量 | ~2.6k 行 | ~30k 行 |

---

## 2. 架构

包装层只做四件事：参数映射、Slice 零拷贝、统计计数、生命周期托管。

```
用户代码
   │  Put/Get/Delete/Write/NewIterator（std::string_view）
   ▼
go2cpp::storage::ldb::DB          ← 本包装层（pimpl，~600 行）
   │  leveldb::Slice（只取指针+长度，零拷贝）
   ▼
leveldb::DB（官方实现，libgo2cpp_leveldb.a 静态链接进最终二进制）
   ├─ MemTable / immutable MemTable
   ├─ WAL（log 文件）
   ├─ SSTable（L0–L6，block + index + bloom）
   └─ BlockCache（LRU，容量 = Options::block_cache_size）
```

生命周期托管（`DbImpl` 共享所有权）：

```
DB ──shared_ptr──┐
                 ├──► DbImpl { atomic<leveldb::DB*> db; cache; filter_policy; stats }
Iterator ─shared─┘
```

* DB 包装先销毁、迭代器后销毁 → `DbImpl` 仍被迭代器持有，底层 DB 不关闭，
  迭代器继续可用（析构时安全 `delete`）；
* 显式 `Close()` 后再析构迭代器 → `db == nullptr`，析构跳过 `delete`，
  **宁可泄漏一个迭代器对象也不 UAF**（错误用法的兜底，正常用法不会走到）。

`leveldb` 头文件只在 `src/storage/leveldb_kv.cpp` 出现；公开头只做前置声明，
不会把第三方宏/类型泄漏给使用方。

---

## 3. API

与 `kv.hpp` 的对应关系：

| 自研 `storage::` | LevelDB `storage::ldb::` | 说明 |
|---|---|---|
| `KV::Open(path, Options)` | `DB::Open(path, Options)` | 失败返回 `nullptr`；原因见 `DB::LastOpenError()` |
| `Put(k, v)` | `Put(k, v)` / `Put(k, v, WriteOptions)` | `WriteOptions{sync}` 覆盖默认 |
| `Get(k, &v)` | `Get(k, &v)` / `Get(k, &v, ReadOptions)` | `v == nullptr` 仅判断存在性 |
| `Delete(k)` | `Delete(k)` | 幂等 |
| `Write(WriteBatch)` | `Write(ldb::WriteBatch)` | 一次 WAL 记录 |
| `NewIterator()` | `NewIterator()` / `NewIterator(ReadOptions)` | 构造后停在第一个元素 |
| `Flush()` | `Flush()` | **语义更重**，见 §5 |
| `Close()` | `Close()` | 关闭后所有操作返回 false |
| `ok()` / `LastError()` | 同名 | |
| `GetStats()` / `StatsString()` | 同名 | 字段同名；LevelDB 不暴露的为 0 |
| `Files()` | 同名 | 目录内文件名 |

`Iterator` 同时提供 Go 风格与 `kv.hpp` 风格两套名字：

```cpp
bool Valid() const;
std::string_view Key() const;    // Go 风格
std::string_view key() const;    // kv.hpp 风格（同义）
std::string_view Value() const;
std::string_view value() const;
void Next();
void Prev();                     // LevelDB 原生，自研没有
void Seek(std::string_view);
void SeekToFirst();
void SeekToLast();
bool ok() const;
std::string LastError() const;
```

`ReadOptions{verify_checksums, fill_cache}`、`WriteOptions{sync}` 是单次调用的
覆盖项；`Options` 里的同名字段作为默认值。

`Options` 与 `leveldb::Options` 的映射：

| `ldb::Options` | `leveldb::Options` |
|---|---|
| `write_buffer_size` | `write_buffer_size`（默认 4 MiB） |
| `max_file_size` | `max_file_size`（默认 2 MiB） |
| `block_size` | `block_size` |
| `block_cache_size` | `block_cache`（`NewLRUCache`；0 = 关闭） |
| `bloom_bits_per_key` | `filter_policy`（`NewBloomFilterPolicy`；0 = 关闭） |
| `compression` | `compression`（true = `kSnappyCompression`） |
| `create_if_missing` / `error_if_exists` / `paranoid_checks` | 同名字段 |
| `sync_writes` / `verify_checksums` / `fill_cache` | 默认 `WriteOptions` / `ReadOptions` |
| `enable_stats` | 包装层统计开关（压测时关掉，与裸库同口径） |

---

## 4. 构建

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_LEVELDB=ON
cmake --build .build-bento-wsl --target go2cpp_leveldb go2cpp_leveldb_kv_test -j12
ctest --test-dir .build-bento-wsl -R go2cpp_leveldb --output-on-failure
```

默认 OFF，不开时本文件直接 `return()`，不影响其余目标。

依赖定位（`cmake/Go2CppLevelDb.cmake`）：

| 项 | 默认路径 |
|---|---|
| 静态库 | `$ENV{HOME}/.local/lib/libleveldb.a` |
| 头文件 | `$ENV{HOME}/.local/include/leveldb/db.h` |
| 线程库 | `find_package(Threads REQUIRED)` → `-lpthread` |

两个 CMake 细节：

1. `find_library/find_path` **不展开 `~`**，必须写 `$ENV{HOME}`；
2. `find_library` 的 `NAMES` 不能直接写 `libleveldb.a`（会被拼成
   `liblibleveldb.a.a`），静态库用 `find_file` 精确匹配。

**为什么 `go2cpp_leveldb` 是 STATIC**：预编译的 `libleveldb.a` 没有开 `-fPIC`，
链接共享库会报 `relocation R_X86_64_PC32 ... recompile with -fPIC`。做成静态库
后 LevelDB 符号只进最终可执行文件，PIE 可执行文件链接正常。

---

## 5. 语义差异与限制

### 5.1 `Flush()` 很重

LevelDB 没有「只刷 memtable」的公开 API。`Flush()` 走
`CompactRange(nullptr, nullptr)`：先 `TEST_CompactMemTable`，再逐层做全量
compaction。**写路径上不要频繁调用**；需要持久化保证时用 `sync_writes=true`。
自研 KV 的 `Flush()` 只刷 memtable + 切 WAL，代价完全不同。

### 5.2 压缩实际是关闭的

本机 `libleveldb.a` 编译时未链接 snappy（`nm` 无 snappy 符号）。
`compression=true` 时 LevelDB 在写表阶段检测到压缩不可用，自动回退为
`kNoCompression`，不报错。要真正启用压缩需重编 LevelDB 并链接 snappy/zstd。

### 5.3 `Stats` 只填 LevelDB 能提供的项

| 字段 | 来源 |
|---|---|
| `puts/gets/hits/deletes/write_batches/bytes_read/bytes_written` | 包装层 relaxed 原子计数 |
| `memtable_bytes` | `GetProperty("approximate-memory-usage")`（含 block cache 估算） |
| `sstable_count` | `num-files-at-level0..6` 之和 |
| `wal_*/memtable_flushes/compactions/*_hits/bloom_negatives/...` | 恒为 0（LevelDB 不暴露） |

`bytes_written` 对批写按序列化后字节数计（含 12B header），是近似值。

### 5.4 迭代器与 `Close()`

* 正常用法：迭代器在 DB 关闭前销毁（作用域结束即可）。
* 兜底：`Close()` 后析构迭代器不会崩溃（跳过 `delete`，泄漏一个对象）。
* `Close()` 与正在进行的 Put/Get **不能并发**（与自研 KV 相同约定）。

### 5.5 单进程

LevelDB 用 `LOCK` 文件保证独占，同一目录不能被两个进程同时打开（崩溃后锁随
进程退出释放，`kill -9` 可重开，见 §6.3）。

---

## 6. 测试

`tests/storage/test_leveldb_kv.cpp` 一个二进制三种模式：

```bash
# 功能 + 对拍 + 崩溃恢复（ctest 默认，~1s）
./go2cpp_leveldb_kv_test

# 1e7 Put/Get 对比（LevelDB vs 自研），建议绑核
taskset -c 2 ./go2cpp_leveldb_kv_test --bench

# 1e8 RSS 有界验证
taskset -c 2 ./go2cpp_leveldb_kv_test --mem
```

### 6.1 对拍（vs 自研 KV）

同一操作序列喂给两个引擎，逐阶段比较：

1. 顺序 Put 2000 条 → 全量扫描逐条比较 + 抽样 Get；
2. 覆盖写（每 3 条改一条）；
3. 删除偶数 key；
4. 混合原子批（500 Put + 250 Delete）；
5. 空批 + 重复删除（幂等）；
6. 重开（WAL 恢复）后再比较；
7. `Get(k, nullptr)` 存在性路径；
8. 迭代器 `Seek` 之后的序列逐条比较。

### 6.2 边界

空 key、空 value、含 `\0` 的二进制 key、1 MiB value、重复写、重复删除、
`Seek` 越界、`SeekToLast` + `Prev`、`error_if_exists`、`Close()` 后所有操作
返回 false、`Flush()` 后大 value 可读、重开持久化。

### 6.3 崩溃恢复

子进程先 `sync` 写 500 条（返回即落盘）并通过管道通知父进程，然后继续异步写；
父进程收到通知 200ms 后 `kill -9`。重开后断言 500 条 **全部** 恢复（`missing=0`），
且 DB 可继续读写、全量扫描不报错、迭代器 `status()` 为 OK。

### 6.4 1e8 RSS

独立子进程写 1e8 条（8B key / 8B value），每 100 万条采样 `/proc/self/status`
的 `VmRSS`，记录峰值；结束后随机读 10 万条校验数据真实落盘；断言
`VmRSS` 与 `VmHWM` 均 < 512 MiB。

---

## 7. 性能

### 7.1 1e7 Put/Get 对比（taskset -c 2，单线程）

数据集：1e7 条，key 20B（`key-%016llu`）+ value 22B（`value-%016llu`）；
顺序写、随机读（Fisher-Yates 全排列）；两个引擎各跑在独立子进程（VmHWM 互不
污染），`Options::enable_stats=false` 让 LevelDB 侧开销最接近裸库。

| 引擎 | Put ns/op | Put ops/s | Get ns/op | Get ops/s | VmHWM | 磁盘 |
|---|---|---|---|---|---|---|
| LevelDB | 1713.8 | **583,491** | 1809.6 | 552,618 | **441.7 MiB** | **357.7 MiB** |
| 自研 KV | 6392.9 | 156,423 | 1662.7 | **601,417** | 624.6 MiB | 483.2 MiB |

* **Put**：LevelDB 快 **3.7x**（583k vs 156k ops/s）。自研在 1e7 规模下
  compaction 频繁（每 8 MiB 刷一段、4 段触发合并、每次合并最旧 2 段），写放大
  显著；LevelDB 的 L0 顺序追加 + 后台 compaction 摊薄了这部分成本。
* **Get**：自研略快 **8.8%**（601k vs 552k ops/s）。自研 mmap + 直接块查找路径
  更短；LevelDB 多一层 block cache/LRU 锁，但差距在 10% 以内。
* **RSS**：441.7 vs 624.6 MiB。两者都含 bench 自身的 80 MiB 随机索引数组
  （1e7 × 8B），扣掉后 DB 侧约 **362 vs 545 MiB**。
* **磁盘**：357.7 vs 483.2 MiB，LevelDB 的块编码更紧凑。

### 7.2 1e8 RSS 有界（核心动机）

1e8 条（8B key / 8B value，落盘约 2.5 GiB），`taskset -c 2`，每 100 万条采样
`/proc/self/status` 的 `VmRSS`：

| 写入进度 | 累计耗时 | RSS |
|---:|---:|---:|
| 1.05e7 | 26.7 s | 40.6 MiB |
| 2.10e7 | 58.0 s | 33.4 MiB |
| 3.15e7 | 93.5 s | 57.9 MiB |
| 4.19e7 | 129.5 s | 57.3 MiB |
| 5.24e7 | 170.2 s | 53.7 MiB |
| 6.29e7 | 210.4 s | 69.2 MiB |
| 7.34e7 | 282.3 s | 68.5 MiB |
| 8.39e7 | 327.4 s | 73.6 MiB |
| 9.44e7 | 379.9 s | 75.2 MiB |
| 1.00e8 | 405.3 s | — |

* 写入：405.3 s，平均 **246,761 ops/s**（前期约 400k/s，compaction 追上后稳定在
  200k/s 左右）
* **RSS 采样峰值 87.4 MiB，VmHWM 123.4 MiB**（基线 4.2 MiB）
* 落盘 **2,522.4 MiB**
* 随机读 10 万条：**100,000/100,000 命中**
* 断言 `RSS < 512 MiB`：**PASS**

RSS 曲线没有随写入量单调增长（40 → 33 → 58 → 57 → 54 → 69 → 68 → 74 → 75 MiB），
波动来自 compaction，不是数据量累积。**2.5 GiB 数据只对应 123.4 MiB VmHWM（4.9%）**，
且由 `block_cache`（8 MiB）+ memtable（8 MiB）+ compaction 缓冲决定，与条数无关。
把它与 1e7 的 441.7 MiB 相比并不矛盾：1e7 那组是 20B key / 22B value + bench 自身
80 MiB 索引数组，且 VmHWM 统计整个进程；1e8 这组才是「只测数据库」的形态。

---

## 8. 已知限制与后续

* 压缩未启用（§5.2）；若换成 gzip/zstd 需重编 `libleveldb.a`。
* `Flush()` 的全量 compaction 在小批量刷盘场景太贵，后续可改用
  `CompactRange` 的区间参数或直接依赖 WAL。
* 快照 API（`GetSnapshot` / `ReleaseSnapshot`）尚未包装；LevelDB 原生支持，
  需要一致性遍历时值得补。
* `Stats` 的 compaction / WAL 指标 LevelDB 只以文本形式暴露在
  `GetProperty("leveldb.stats")` 中，未解析。

---

## 9. 参考

* LevelDB 官方源码：`/tmp/leveldb`（1.23.0）
* 自研实现与对比数据：`docs/modules/storage-kv.md`
* 构建入口：`cmake/Go2CppLevelDb.cmake`
