# 存储（交接文档）

> **定位**：两层存储——SQLite 封装（`storage::DB` + ORM）与自研 LSM KV 引擎（`storage::KV` + Queue/Watch/MemCache 功能层）。
> **源码**：`include/go2cpp/storage/**`、`src/storage/**`（约 7700 行实现）　**构建**：`cmake/Go2CppStorage.cmake`（`go2cpp_storage`）、`cmake/Go2CppKv.cmake`（`go2cpp_kv`）、`cmake/Go2CppLevelDb.cmake`（`go2cpp_leveldb`，适配层）
> **测试**：`tests/storage/`（14 个文件：`test_db` / `orm_test` / `kv_test` / `queue_test` / `watch_test` / `memcache_test` + 4 个 bench）
> **设计全文**：[storage-sqlite.md](../../docs/modules/storage-sqlite.md)、[storage-orm.md](../../docs/modules/storage-orm.md)、[storage-kv.md](../../docs/modules/storage-kv.md)、[storage-leveldb.md](../../docs/modules/storage-leveldb.md)、[storage-features.md](../../docs/modules/storage-features.md)
> **配套**：可运行示例 [user/app/README.md](../app/README.md)；持久化总览 [persistence.md](../../docs/modules/persistence.md)

## 1. 这个模块是什么

存储模块里其实有**两套独立的引擎**，选择取决于你要 SQL 还是要高吞吐 KV：

| 路线 | 引擎 | 事务/查询能力 | 适用 |
|---|---|---|---|
| SQL 路线 | 系统 `libsqlite3` 的 C++ 封装（`DB`/`Stmt`/`Rows`/`Transaction`）+ ORM（`Session`/`Query`/`Table<T>`/`Migration`） | 完整 SQL、事务、索引、迁移 | 关系数据、需要 JOIN/聚合、运维要能拿 sqlite3 命令行看 |
| KV 路线 | 自研 LSM（WAL → MemTable → immutable → SSTable，Bloom + mmap） | 单 key 读写 + 原子批 + 有序迭代；**无快照/无多版本** | 高吞吐点查/写、日志型数据、Queue/Watch/MemCache |
| 兼容路线 | `go2cpp::storage::ldb` 命名空间下的 leveldb 接口适配（`DB`/`Iterator`/`WriteBatch`/`Options`） | 对齐 leveldb API | 想复用 leveldb 生态代码时 |

功能层（都建在 KV 之上，共用同一份 WAL/SSTable 格式）：

- **Queue**：topic + consumer 游标的消息队列，段文件 `queue/NNNNNN.seg`，游标存 KV；
- **Watcher**：带前缀匹配的变更订阅，支持 Replay（全量/区间）与异步背压；
- **MemCache**：内存 KV + TTL 时间轮 + LRU，可写穿 `KV` 并 read-through 回填。

**不做什么**：不做跨进程共享（MemCache 是进程内）、不做分布式共识、不做 SQL 连接池
（`DB` 一把内部互斥锁串行化本连接所有公开操作）。

## 2. 架构 / 数据流

```
                    ┌─────────────────── SQL 路线 ───────────────────┐
  Session/Query ──▶ │ storage::DB（每连接一把 std::mutex，串行化）    │
  Migration     ──▶ │   Exec/Query/Prepare → libsqlite3（系统 so）   │
                    │   Transaction（RAII，析构自动 Rollback）        │
                    └────────────────────────────────────────────────┘

                    ┌─────────────────── KV 路线 ────────────────────┐
  KV::Put/Get   ──▶ │ 写：WriteBatch → WAL（64KB 块 + CRC32C）        │
  KV::Write     ──▶ │      → MemTable（Arena + 槽/载荷分离，无锁读）  │
  KV::NewIterator ▶ │ 读：MemTable → immutable → SSTable（mmap+Bloom）│
                    │ 落盘：MemTable 满 → SSTable；段数超 max → 合并  │
                    └──────────┬─────────────────┬───────────────────┘
                               │ 复用 WAL 格式    │ 复用 SSTable 块格式
                    ┌──────────▼──────┐  ┌───────▼─────────┐
                    │ Queue（SegLog） │  │ Watcher（SegLog）│
                    │  topic/consumer │  │  prefix + Replay │
                    └─────────────────┘  └─────────────────┘
                    ┌──────────────────────────────────────┐
                    │ MemCache（纯内存；可选写穿/回填 KV）    │
                    │  256 桶时间轮 + 侵入式 LRU + 节点池     │
                    └──────────────────────────────────────┘
```

### 2.1 KV 写读路径的关键设计

| 环节 | 做法 | 为什么 |
|---|---|---|
| 写锁 | `Put/Delete/Write` 之间由内部写锁串行 | 单写者让 WAL 顺序追加与 MemTable 发布变简单 |
| WAL | 64 KiB 块 + CRC32C + 1 MiB 用户态缓冲，**group commit** | 8 线程 sync 写 630 → **2,525 ops/s**（4×） |
| MemTable 读 | 槽位原子发布 + epoch 延迟回收，**读不持锁** | 8 线程随机 Get **2,386,001 ops/s** |
| 块大小 | 默认 **4096**（不是 16KB） | 块内顺序扫描可提前退出；16KB 每次比较跳新 cache line（1e7 读 1915 ns vs 4KB ~700 ns） |
| SSTable 数 | 默认 `max_sstables=4` | 段数决定每次 Get 的 Bloom 次数；2 段读最快但 Put 慢 2× |
| 读加速 | 每 key 10 bit Bloom + mmap 零拷贝 | 1e7 次 Get 平均 2.5 次段级 Bloom，命中率 95.3% 走 SSTable |

### 2.2 线程模型（交接重点）

- `DB` **不是线程安全的连接**：内部 `m_mutex` 串行化，多线程共用一个 `DB` 能跑但没并行度；
  要高并发请每线程一个 `DB`（SQLite 自身支持多连接）。
- `KV`：**任意线程可并发 Get**；写之间串行。`Get` 返回的 value 是拷贝，不存在悬空视图。
- `Queue`/`Watcher`/`MemCache`：单进程多线程可用，`Append`/`Consume` 内部有锁；
  **跨进程同时打开同一目录是未定义行为**。
- 没有 Fiber 感知：这些接口不会 park，在受管 G 上调用等于**原地阻塞 M**——重 IO 请放到边界线程池。

## 3. 对外 API（关键接口）

### 3.1 SQL 路线

```cpp
// include/go2cpp/storage/db.hpp（namespace go2cpp::storage）
class DB {
    static Result<std::unique_ptr<DB>> Open(const std::string& path);   // ":memory:" = 内存库
    Error Close();                       bool IsOpen() const;
    template <typename... Args> Result<ExecResult> Exec(const std::string& sql, const Args&... args);
    template <typename... Args> Result<Rows> Query(const std::string& sql, const Args&... args);
    Result<Stmt> Prepare(const std::string& sql);
    Error Begin();  Error Commit();  Error Rollback();  bool InTransaction() const;
    Error SetBusyTimeout(int milliseconds);
    Error SetJournalMode(std::string_view mode);   // DELETE/TRUNCATE/PERSIST/MEMORY/WAL/OFF
    Error SetSynchronous(std::string_view level);  // OFF/NORMAL/FULL/EXTRA 或 0..3
    Error SetForeignKeys(bool enabled);
    std::int64_t LastInsertRowId() const;   int Changes() const;   Error LastError() const;
    sqlite3* Handle() const;             // 只读扩展，不能在本对象外 finalize/close
};

class Transaction {   // RAII：析构时若未 Commit 自动 Rollback
    explicit Transaction(DB& db) noexcept;   // 不自动 Begin
    Error Begin();  Error Commit();  Error Rollback();  bool Active() const;
};

class Stmt {  // 预编译语句
    Error Bind(int index, ...);  Error Step();  Error Reset();
    std::int64_t ColumnInt64(int i);  std::string_view ColumnTextView(int i);  // 零拷贝
    std::string ColumnText(int i);    std::vector<std::uint8_t> ColumnBlob(int i);
};
```

`Rows` 是单向游标（`Next()` 推进），`Exec` 可一次跑多条以 `;` 分隔的脚本，
但**带绑定参数时只允许单条语句**。

### 3.2 ORM

```cpp
// include/go2cpp/storage/orm.hpp
struct User {
    std::int64_t id = 0;
    std::string name;
    int age = 0;
    std::vector<std::uint8_t> avatar;
    std::optional<std::string> email;
};

GO2CPP_ORM_TABLE(User, "users",
                 GO2CPP_ORM_COLUMN(User, id).PrimaryKey().AutoIncrement(),
                 GO2CPP_ORM_COLUMN(User, name).NotNull().Unique(),
                 GO2CPP_ORM_COLUMN(User, age).Indexed(),
                 GO2CPP_ORM_COLUMN(User, avatar),
                 GO2CPP_ORM_COLUMN(User, email))

class Session {
    explicit Session(DB& db);            // 不转移所有权
    template <typename T> Result<std::int64_t> Insert(T& value);   // 回填自增主键
    template <typename T> Result<int> Update(const T& value);      // 按主键
    template <typename T> Result<int> Delete(std::int64_t key);
    template <typename T> Result<T> Find(std::int64_t key);        // 不存在 → kErrNotFound
    template <typename T> Query<T> Query();                        // Where/OrderBy/Limit/All
    template <typename T> Result<std::vector<T>> All();
    template <typename T> Result<std::int64_t> Count();
    template <typename F> Result<void> Transact(F&& body);          // 提交/回滚
};

class Migration {
    template <typename T> void AddCreateTable(int version, std::string name);
    Result<void> Up();
};
```

映射有三种：宏（`GO2CPP_ORM_COLUMN`/`GO2CPP_ORM_TABLE`，**显式、零运行期分派**）、
`make_column`/`make_storage` 模板、反射版 `GO2CPP_ORM_TABLE_REFLECT`。
可选类型用 `std::optional<T>`（→ NULL），二进制用 `std::vector<std::uint8_t>`（→ BLOB）。

### 3.3 KV 与功能层

```cpp
// include/go2cpp/storage/kv.hpp
class KV {
    static std::unique_ptr<KV> Open(const std::string& path, const Options& options = Options());
    bool Put(std::string_view key, std::string_view value);
    bool Get(std::string_view key, std::string* value) const;
    bool Delete(std::string_view key);
    bool Write(const WriteBatch& batch);            // 原子批：全可见或全不可见
    std::unique_ptr<Iterator> NewIterator();        // 快照式有序迭代
    void Flush();  void Close();
    bool ok() const;  Stats GetStats() const;  std::string StatsString() const;
};
struct Options {   // 默认值面向「小 value、高吞吐」嵌入式负载
    std::size_t memtable_size = 8u << 20;   std::size_t block_size = 4096;
    std::size_t sstables_per_compaction = 2;  std::size_t max_sstables = 4;
    std::size_t write_buffer_size = 1u << 20; int bloom_bits_per_key = 10;
    bool sync_writes = false;  bool use_mmap = true;  bool create_if_missing = true;
};

// include/go2cpp/storage/queue.hpp
class Queue {
    static std::unique_ptr<Queue> Open(KV* kv, const std::string& dir, const QueueOptions& = {});
    bool Append(std::string_view topic, std::string_view msg, std::uint64_t* offset = nullptr);
    bool Consume(std::string_view topic, std::string_view consumer, QueueMessage* msg);
    bool Ack(std::string_view topic, std::string_view consumer, std::uint64_t offset);
    bool Seek(...);  std::uint64_t Cursor(...);  std::uint64_t Head(std::string_view topic);
    bool FlushCursors();  bool Sync();  QueueStats GetStats() const;
};
// include/go2cpp/storage/memcache.hpp
class MemCache {
    static std::unique_ptr<MemCache> Open(KV* kv, const MemCacheOptions& = {});
    bool Set(std::string_view key, std::string_view value, std::chrono::milliseconds ttl = {});
    bool Get(std::string_view key, std::string* value);   // 命中提升 LRU
    bool Peek(std::string_view key, std::string* value);  // 不提升、不计命中
    bool Delete(std::string_view key);  bool Expire(std::string_view key);
    std::size_t PurgeExpired();  std::size_t Size() const;  std::size_t Capacity() const;
};
```

`Queue::Open` 的 `kv` 可以是 `nullptr`（游标只存内存，进程退出即丢）；`MemCache` 同理，
但 `persist`/`read_through` 需要非空。

## 4. 最小用法（可编译示例）

```cpp
#include "go2cpp/storage/kv.hpp"
#include <iostream>
#include <string>

int main() {
    using namespace go2cpp::storage;

    Options opts;
    opts.memtable_size = 4u << 20;     // 4 MiB 就刷一次 SSTable
    opts.max_sstables = 4;
    auto kv = KV::Open("/tmp/demo-kv", opts);
    if (!kv || !kv->ok()) { std::cerr << "open failed\n"; return 1; }

    for (int i = 0; i < 1000; ++i) {
        if (!kv->Put("key-" + std::to_string(i), "value-" + std::to_string(i))) return 1;
    }

    WriteBatch batch;                  // 原子批：一次 WAL 记录
    batch.Put("batch-a", "1");
    batch.Put("batch-b", "2");
    if (!kv->Write(batch)) return 1;

    std::string value;
    if (kv->Get("key-500", &value)) std::cout << "get=" << value << '\n';
    std::cout << kv->StatsString() << '\n';   // puts/flushes/sstables/mem_hits/sst_hits
    kv->Close();                              // Flush + WAL fsync + 释放文件
    return 0;
}
```

编译：`g++ -std=c++17 -O2 -I include demo.cpp build/libgo2cpp_kv.a -lpthread`。SQLite 路线改成
`#include "go2cpp/storage/db.hpp"` + 链接 `libgo2cpp_storage.a -lsqlite3`。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/storage/db.hpp` / `src/storage/db.cpp` | 615 / 949 | SQLite 封装：`DB`/`Stmt`/`Rows`/`Transaction`/`Error`/`Value`/`Blob` |
| `include/go2cpp/storage/orm.hpp` / `src/storage/orm.cpp` | 1519 / 1033 | `Session`/`Query`/`Table<T>`/`Migration`、列映射宏与反射 |
| `include/go2cpp/storage/kv.hpp` / `src/storage/kv.cpp` | 169 / 1202 | KV 门面：WriteBatch、刷盘、Compaction、统计 |
| `src/storage/wal.cpp` / `seglog.cpp` | 414 / 779 | WAL（64 KiB 块 + CRC32C + group commit）与段日志（Queue/Watch 共用） |
| `src/storage/memtable.cpp` / `arena.cpp` / `sstable.cpp` | 174 / 108 / 590 | Arena + 槽/载荷分离的 MemTable；SSTable 块格式、Bloom、mmap 读、合并 |
| `include/go2cpp/storage/queue.hpp` / `src/storage/queue.cpp` | 142 / 683 | topic/consumer 游标队列 |
| `include/go2cpp/storage/watch.hpp` / `src/storage/watch.cpp` | 134 / 592 | 前缀订阅、Replay、背压 |
| `include/go2cpp/storage/memcache.hpp` / `src/storage/memcache.cpp` | 115 / 608 | TTL 时间轮 + LRU + 写穿/回填 |
| `include/go2cpp/storage/leveldb_kv.hpp` / `src/storage/leveldb_kv.cpp` | 260 / 603 | leveldb 兼容适配（`DB`/`Iterator`/`FilterPolicy`/`Cache`） |

## 6. 性能数据

### 6.1 KV 引擎（1e7，WSL ext4/tmpfs，2026-11-01）

来源：[docs/reports/asm-storage-kv.md](../../docs/reports/asm-storage-kv.md)，`taskset -c 4-11`，
key 20 B / value 22 B，GCC 13.3 `-O2`。

| 场景 | 吞吐 | ns/op |
|---|---:|---:|
| KV(ext4) Put 顺序 | **562,669 ops/s** | 1777.3 |
| KV(ext4) Get 随机命中 | **475,125 ops/s** | 2104.7 |
| KV(tmpfs) Put 顺序 | **699,339 ops/s** | 1429.9 |
| MemTable Put（1e7 常驻） | 2,881,941 ops/s | 347.0 |
| MemTable Get（1e7 常驻） | 1,551,145 ops/s | 644.7 |
| KV Get 8 线程（2e6 次） | **2,386,001 ops/s** | 419.1 |
| KV sync 写单线程 | 630 ops/s | 1.586 ms |
| KV sync 写 8 线程 group commit | **2,525 ops/s** | 0.396 ms |

同机物理下限：64 MiB 随机访存 **155.1 ns/次**、`fdatasync(4KiB)` 中位 **556.1 µs**。
离下限的倍数：MemTable Get **4.2×**（至少 2 次 cache miss）、KV Get **13.6×**（2.5 次 Bloom +
索引二分 + 块访问 + TLB）、sync 写 **2.9×**（多出来的是 ext4 文件扩展，不是代码）。

### 6.2 SQLite 封装（2026-11-02）

来源：[docs/reports/asm-storage-sqlite.md](../../docs/reports/asm-storage-sqlite.md)，7 轮取中位数。

| 指标 | 值 |
|---|---|
| 1e6 行插入（文件库） | **1.078×**（<1.1×，热路径返回 `const Error&` 从 1.131× 降下来） |
| 场景达标 | 7 个场景 **6 个 <1.1×**；内存插入 1.103× 在边界（波动区间跨 1.0） |
| 指令级（`objdump -d`，见下） | `Step` 348 / `BindValues` 449 / `Reset` 218 / `Rows::Next` 159 |

指令级：`DB::Prepare()` **999** 条 / 栈帧 152 B / **2 把 mutex**、`DB::ExecImpl()` **853** 条 /
**2 把 mutex**；列读取 `ColumnInt64()` **16** 条、`ColumnTextView()`（零拷贝）**35** 条（拷贝版 76 条）。

### 6.3 功能层（1e7，单次运行）

来源：[docs/reports/asm-storage-features.md](../../docs/reports/asm-storage-features.md)：
`queue.append(40B)` **196.1 ns/op**（5,099,337 ops/s，234 MB/s）、
`queue.consume(40B)` **24.2 ns/op**（41,245,422 ops/s，1,572 MB/s）。

## 7. 边界与已知限制

1. **KV 是单版本引擎**：没有快照、没有多版本、没有事务隔离级别。`NewIterator()` 给的是
   「当前 MemTable + immutable + SSTable 并集」的一致视图，但**不冻结写入**。
2. **`sync_writes=true` 掉一个数量级**：单线程 630 ops/s vs 默认（内核回写）56 万；
   崩溃可能丢最后一段是默认行为。要「崩溃不丢已确认写」就必须开，并接受 group commit 前的延迟。
3. **Compaction 只合并最旧的若干段**：`max_sstables=4`、每次合并 2 段；不做分层/分级的
   全量策略，写放大与段数是手工调的。
4. **`DB` 连接非线程安全**：内部一把 `std::mutex` 串行化所有公开操作；`Stmt`/`Rows`
   必须在同一线程内用完。多线程请每线程一个 `DB`。
5. **`Exec` 带参数只允许单条语句**：多语句脚本不能绑定参数（要逐条 `Prepare`）。
6. **ORM 是零运行期分派的宏映射**：加字段必须改 `GO2CPP_ORM_TABLE` 列表，否则不参与建表/查询；
   反射版（`GO2CPP_ORM_TABLE_REFLECT`）是另一条路径，两者不能混用于同一类型。
7. **Queue/Watcher 段文件不自动删除**：段滚动只写新段，保留策略要上层按 offset 自己清。
8. **MemCache 是进程内缓存**：无跨进程共享；`persist` 写穿 KV 会同步付出 KV 的写成本。
9. **全部接口都不 Fiber 感知**：没有 park，在受管 G 上调用等于阻塞 M。重 IO 请走边界线程池，
   或包进 `Scheduler::BlockingRegion`（见 01-concurrency.md §3.2）。**跨进程打开同一目录是未定义行为**。

## 8. 测试与验证方法

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build -R "storage|kv|queue|watch|memcache|orm|leveldb" --output-on-failure
./build/go2cpp_kv_test          # 基本读写/批/迭代/恢复
./build/go2cpp_queue_test       # 1e6 全量 + fork+SIGKILL 崩溃恢复
```

| 文件 | 覆盖 | 规模 |
|---|---|---|
| `kv_test.cpp` / `kv_recovery_test.cpp` / `kv_concurrent_test.cpp` | 读写、批、迭代、崩溃恢复、并发 | — |
| `test_db.cpp` / `orm_test.cpp` / `test_leveldb_kv.cpp` | SQL 封装、事务、ORM 增删改查、迁移、leveldb 兼容接口 | — |
| `queue_test.cpp` | 多消费者游标、段滚动、尾部垃圾截断、fork+SIGKILL | 1e6（**59,209** checks，0 失败） |
| `watch_test.cpp` | 前缀/多订阅/Replay/背压/顺序 | 1e5（**111,180** checks，0 失败） |
| `memcache_test.cpp` | TTL/LRU/多线程/写穿/回填 | 1e7（**574** checks，0 失败） |
| `bench_db.cpp` / `kv_bench.cpp` / `orm_bench.cpp` / `features_bench.cpp` | 性能基准（生成 §6 的数据） | — |

崩溃恢复是最重要的验证：`queue_test` 用子进程 append 10 万条 + 消费 10 条 + `Sync()` +
`KV::Close()` 后 `raise(SIGKILL)`，父进程重开校验 `Head==100000`、游标==10、再 append 的
offset==100000。**改 WAL/段日志格式必须跑这个用例**。

## 9. 交接注意

- **先决定走哪条路线**：要 SQL/事务/JOIN → `storage::DB`；要百万级点查/写 → `storage::KV`。
  两者目录格式无关，放同一个目录下不会互相破坏，但也没有跨引擎事务。**`Options.block_size=4096`
  不要随手改回 16KB**：注释里写明了实测（1915 ns vs ~700 ns），这是 cache line 效应，不是玄学。
- **`max_sstables` 与 `sstables_per_compaction` 是一对**：调大前者读变慢（Bloom 变多）、
  调小后者写变慢（合并频繁）。当前 4/2 是折中值，改之前先跑 `kv_bench`。
- **WAL 格式是三处共用**：`kv.cpp`、`queue.cpp`（SegLog）、`watch.cpp`（SegLog）。改
  `WalWriter` 的块头/CRC 必须三处一起测，`kv_recovery_test` + `queue_test` 的 SIGKILL 用例都要过。
- **`DB::Handle()` 是逃生口**：需要 sqlite3 扩展（如自定义函数、`sqlite3_backup`）时用它，
  但**不要**在 `DB` 之外 `sqlite3_close`，否则 `DB` 析构会二次关闭。
- **事务默认 `BEGIN IMMEDIATE`**：避免「读升级写」死锁；如果你的场景是长只读，用
  `Query` 不加事务即可。
- **未完成事项**：KV 无快照/多版本、无分层 compaction、无跨进程锁；Queue 段保留策略未实现；
  ORM 无关系（JOIN）映射与懒加载。