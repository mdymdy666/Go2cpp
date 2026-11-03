# 存储模块 · 深入学习

> 面向「要接手改 `include/go2cpp/storage/**` / `src/storage/**` 的人」。
> 行号对源码复核过；性能只写「本机怎么测」，不写文档里的结论数字。

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 入口点清单

| 入口（类/函数） | 文件:行 | 谁调用它 | 触发什么 | 返回什么 |
|---|---|---|---|---|
| `DB::Open(path)` | `include/go2cpp/storage/db.hpp:489`（实现 `src/storage/db.cpp:510`） | 业务/测试 | `sqlite3_open_v2` + `extended_result_codes` + `busy_timeout` | `Result<unique_ptr<DB>>` |
| `DB::Exec(sql, args...)` | `include/go2cpp/storage/db.hpp:514` | 业务/ORM | 逐条 prepare/bind/step/finalize | `Result<ExecResult>` |
| `DB::Query(sql, args...)` | `include/go2cpp/storage/db.hpp:525`（实现 `db.cpp:644`） | 业务/ORM | 单条 prepare + bind，返回游标 | `Result<Rows>` |
| `DB::Prepare(sql)` | `include/go2cpp/storage/db.hpp:531`（实现 `db.cpp:676`） | 高频复用 | 编译一条语句 | `Result<Stmt>` |
| `DB::Begin/Commit/Rollback` | `db.hpp:534/536/538`（实现 `db.cpp:702/724/745`） | 业务 | `BEGIN/COMMIT/ROLLBACK` | `Error` |
| `DB::SetJournalMode(mode)` | `db.hpp:546`（实现 `db.cpp:788`） | 初始化 | PRAGMA journal_mode 并校验实际值 | `Error` |
| `DB::Close()` | `db.hpp:499`（实现 `db.cpp:532`） | 业务 | `sqlite3_close_v2` | `Error` |
| `Stmt::Step()` | `db.hpp:361`（实现 `db.cpp:359`） | 复用循环 | ROW/DONE/错误三态 | `const Error&` |
| `Rows::Next()` | `db.hpp:429`（实现 `db.cpp:474`） | 遍历 | 前进一行，错误存 `Rows::Err` | `bool` |
| `storage::Session::Find/All` | `include/go2cpp/storage/orm.hpp:1049/1067` | 业务 | 按主键查/全表查 | `Result<T>` |
| `storage::Query<T>::All/First` | `orm.hpp:1272/1285` | 业务 | 条件查询（SQL 拼装 + 参数绑定） | `Result<vector<T>>` / `Result<T>` |
| `storage::Migration::Up/Down` | `orm.hpp:1480/1484`（实现 `src/storage/orm.cpp:915/925`） | 启动/运维 | 逐条应用/回滚迁移 | `Error` |
| `ldb::DB::Open` | `include/go2cpp/storage/leveldb_kv.hpp:210`（实现 `leveldb_kv.cpp:277`） | 测试/对照 | 创建 leveldb::Options + block cache + bloom | `unique_ptr<DB>` |
| `ldb::DB::Put/Get/Write` | `leveldb_kv.hpp:218/228/224` | 测试/对照 | 单写/单读/原子批 | `bool` |
| `ldb::DB::NewIterator` | `leveldb_kv.hpp:232` | 测试/对照 | 有序迭代器 | `unique_ptr<Iterator>` |
| `ldb::DB::Flush/Close/GetStats` | `leveldb_kv.hpp:237/239/244`（实现 `leveldb_kv.cpp:499/509/526`） | 测试/运维 | 全层 compaction / 释放 / 统计 | `void`/`void`/`Stats` |
| `KV::Open` | `include/go2cpp/storage/kv.hpp:128`（实现 `src/storage/kv.cpp:1170`） | Queue/Watch/MemCache/业务 | 恢复 WAL、打开 SSTable | `unique_ptr<KV>` |
| `KV::Put/Get/Delete/Write` | `kv.hpp:135/136/137/140`（实现 `kv.cpp:1178/1182/1186/1188`） | 功能层 | MemTable + WAL 写、epoch 无锁读 | `bool` |
| `Queue::Open` | `include/go2cpp/storage/queue.hpp:85`（实现 `queue.cpp:624`） | 业务 | 挂 KV + SegLog，恢复 topic | `unique_ptr<Queue>` |
| `Queue::Append/Consume/Ack` | `queue.hpp:93/97/101`（实现 `queue.cpp:633/638/643`） | 业务 | 顺序追加 / 读 / 推进游标 | `bool` |
| `Queue::FlushCursors/Sync` | `queue.hpp:115/118`（实现 `queue.cpp:660/662`） | 业务 | 批量落盘游标 / fdatasync | `bool` |
| `Watcher::Open/Watch/Put/Delete` | `include/go2cpp/storage/watch.hpp:83/93/99/100`（实现 `watch.cpp:532/541/552/556`） | 业务 | 订阅前缀、包装写 | `unique_ptr`/`id`/`bool` |
| `Watcher::Notify/Replay` | `watch.hpp:104/107`（实现 `watch.cpp:560/565`） | 业务 | 主动事件 / 按游标重放 | `bool` |
| `MemCache::Open` | `include/go2cpp/storage/memcache.hpp:67`（实现 `memcache.cpp:563`） | 业务 | 分片 + TTL 轮 | `unique_ptr` |
| `MemCache::Set/Get/Peek/Delete/Expire` | `memcache.hpp:77/80/83/85/88`（实现 `memcache.cpp:572/577/581`） | 业务 | 写穿/回填/过期 | `bool` |
| `MemCache::PurgeExpired` | `memcache.hpp:94`（实现 `memcache.cpp:245`） | 业务/定时 | 主动清过期 | 清理条数 |

> 编译开关：`GO2CPP_WITH_SQLITE`（`cmake/Go2CppStorage.cmake`，默认 OFF）、`GO2CPP_WITH_KV`（`cmake/Go2CppKv.cmake:14`，默认 ON）、`GO2CPP_WITH_LEVELDB`（`cmake/Go2CppLevelDb.cmake:19`，默认 OFF）。

### 1.2 总架构图

```
业务代码（app / tools / tests）
   │                       │                          │
   ▼                       ▼                          ▼
SQLite 封装 db.hpp     ORM orm.hpp              LSM 功能层
DB/Stmt/Rows/Value     Session/Query/Migration  Queue / Watcher / MemCache
   │                       │                          │
   │                       │                          ▼
   │                       │                  SegLog（WAL 块格式 + 段索引）
   │                       │                          │
   │                       │                          ▼
   │                       │                 自研 KV（MemTable + WAL + SSTable + epoch）
   │                       │                          │
   ▼                       ▼                          ▼
libsqlite3           libsqlite3               ./data/*.log / *.sst / MANIFEST
（进程内，无服务）    （同一连接）              （用户态缓冲 + fdatasync）

旁路对照（默认不编译）：
ldb::DB ── 官方 LevelDB（block cache + L0-L6 compaction），唯一使用方是 tests/storage/test_leveldb_kv.cpp
```

线程/锁边界：
- SQLite：`DB::m_mutex` 串行化同一连接的所有 `Exec/Query/Prepare/事务`；`Stmt/Rows` 是单线程对象，不加锁。
- LSM：`KV::Impl` 内部有写锁 + epoch 无锁读；Queue 每个 topic 一把锁；Watcher 有 `subs_mu_`/`log_mu_`/`q_mu_`；MemCache 按哈希分片，每片一把锁。

### 1.3 复杂场景详解

#### 场景 A：一条参数化 INSERT 的完整路径（12 步）

1. 业务调用 `db->Exec("INSERT INTO t(a) VALUES(?)", 42)` → 模板把 `42` 变成 `Value` 数组，调用 `ExecImpl`（`src/storage/db.cpp:587`）。
2. `std::lock_guard<std::mutex>` 锁住 `m_mutex`（`db.cpp:590`）：同一 `DB` 的写/读/事务全部串行。
3. `CheckOpenLocked()`：`Close()` 之后返回 `kMisuse`（`db.cpp:562` 附近），不会解引用空指针。
4. `sqlite3_prepare_v2(m_db, cursor, -1, &stmt, &tail)`（`db.cpp:600`）：`tail` 指向下一条语句起点；SQL 语法错误在这里变成 `Code::kError`。
5. `BindLocked`：先比较 `count` 与 `sqlite3_bind_parameter_count`，不一致直接 `ParamCountError`（不猜 NULL）；再逐个 `BindOne`（int64/double/text/blob/null）。
6. `sqlite3_step(stmt)`（`db.cpp:624`）：INSERT 返回 `SQLITE_DONE`；约束冲突返回 `SQLITE_CONSTRAINT`，被翻译成 `Code::kConstraint`。
7. 成功：`sqlite3_last_insert_rowid` + `sqlite3_changes` 累加进 `ExecResult`（`db.cpp:631`-`:632`）。
8. `sqlite3_finalize(stmt)`（`db.cpp:633`）：语句资源释放；`executed=true`。
9. `cursor = tail` 继续循环；纯空白/注释时 `prepare` 返回 `stmt==nullptr`，按 `tail` 前进或结束。
10. 带参数且已执行过一条时，第二条触发 `kMisuse`（`db.cpp:611`-`:618`）——第一条已生效，不回滚。
11. 返回 `ExecResult`；错误时 `m_last_error` 也被更新，便于诊断。
12. 阻塞/失败路径：写锁被占时 SQLite 内部等待 `busy_timeout=5000`（`db.cpp:528`），超时返回 `SQLITE_BUSY`；`Close()` 之后调用返回 `kMisuse` 而不是崩溃。

#### 场景 B：Queue 的 Append→Consume→Ack（14 步）

1. `Queue::Open(kv, dir)`（`queue.cpp:624`）→ `Impl::Open` 挂 KV 与 SegLog。
2. `recover_topics=true` 时扫描日志重建每个 topic 的 `next_offset`/条数（`queue.cpp` 的 recovery 段）。
3. `Append(topic,msg)`（`queue.cpp:633` → `Impl::Append` `:131`）：取/建 topic 状态，锁该 topic。
4. `offset = next_offset++`（topic 内连续序号）。
5. 记录编码后 `SegLog::Append`（`src/storage/seglog.cpp:441`）：写用户态缓冲（默认 1 MiB），满才 `write`。
6. 同时把消息副本放进内存 deque（受 `retain` 限制；0 表示不限制到全部 Ack）。
7. 返回 true；`sync_on_append=false` 时尚未 `fdatasync`，崩溃可能丢最后几条。
8. `Consume(topic,consumer)`（`queue.cpp:638` → `Impl::Consume` `:175`）：先查内存 deque。
9. 内存没有（已回收/重启）：用 `SegLog::Cursor` 从 `start_seq` 定位，批量预取（`kRefillBatch`）后返回。
10. Consume **不推进游标**：同一条可以重复读，这是 at-least-once 的读侧语义。
11. `Ack(topic,consumer,offset)`（`queue.cpp:643` → `Impl::Ack` `:207`）：`offset+1 > cur` 才更新，乱序 Ack 不会把游标拉回。
12. 更新后 `MarkDirty` + `stats_.acks++`，并 `ReclaimLocked` 回收已确认的内存消息。
13. `BumpAckLocked()`（`queue.cpp:472`）累计到 `cursor_flush_interval`（默认 4096）才返回 true → `FlushCursors()`（`:257`）。
14. `FlushCursors` 把多个消费者游标合成一个 `WriteBatch` 写进 KV；失败 `Fail("cursor persist failed")`，游标退回上次落盘值（重启后重复投递）。

#### 场景 C：LevelDB 的 Put→Close→重开（10 步）

1. `ldb::DB::Open(path, options)`（`leveldb_kv.hpp:210` → `leveldb_kv.cpp:277`）。
2. 组装 `leveldb::Options`：`write_buffer_size`/`max_file_size`/`block_size`/`compression`。
3. `NewLRUCache(block_cache_size)` 给 block cache 设上限（`leveldb_kv.cpp` Open 段）。
4. `NewBloomFilterPolicy(bloom_bits_per_key)` 写进 `filter_policy`（10 位约 1% 假阳性，代码默认）。
5. `leveldb::DB::Open` 成功 → 存进 `DbImpl::db` 原子指针；失败写 `g_last_open_error`，返回 nullptr。
6. `Put(key,value)`（`:218`）：`impl_->puts++`（relaxed），调用 `leveldb::DB::Put`。
7. 失败：`Fail(status)` 记 `LastError`、`ok=false`，返回 false；调用方必须看返回值。
8. `Close()`（`:509`）：`delete db`，再 `filter_policy.reset()`、`cache.reset()`——顺序不能反（DBImpl 析构前还持有二者）。
9. 重开：LevelDB 从 WAL 恢复 memtable；未 `Flush` 的数据只要 WAL 在就还在。
10. 真正的持久化保证来自 `WriteOptions.sync`（每次写 fdatasync）；`Flush()`（`:499`）= `CompactRange(nullptr,nullptr)`，只把 memtable 落 SST + 全层 compaction，**不是** fsync 屏障。

#### 场景 D：MemCache 的 TTL 写入与过期（12 步）

1. `MemCache::Open(kv, options)`（`memcache.cpp:563`）按 `shards` 建 N 个 `Shard`（`:68`）。
2. 每个 `Shard` 有自己的索引、LRU 链、`TimestampWheel`（`:410`）和 mutex。
3. `Set(key,value,ttl)`（`:572`）：算哈希 → `ShardFor`（`:417`）→ 锁该片。
4. `FindLocked`（`:425`）查条目；已存在则更新值、TTL、`LruTouch`（`:486`）。
5. 新条目：若该片容量满，`EvictLocked`（`:506`）淘汰 LRU 尾（`evictions++`）。
6. `ttl > 0` 时把绝对到期时间插进时间轮（`time_wheel.hpp:87`）。
7. `persist=true` 时写穿 KV（`write_throughs++`）；KV 失败会回传 false。
8. `Get`（`:577`）：锁片 → `FindLocked` → 命中先判过期。
9. 过期：`RemoveLocked` + `expirations++`，返回 false（不会返回过期值）。
10. 未过期：`LruTouch` + 拷 value，`hits++`。
11. `purge_on_get=false`（默认）时每 64 次 Get 才扫一次 TTL 轮，其余靠命中逐条判断。
12. `PurgeExpired`（`:245`）逐片主动清理；`Clear` 清空索引/链/轮，不重置统计。

### 1.4 关键不变量

| # | 不变量 | 位置 | 破了会怎样 |
|---|---|---|---|
| 1 | `DB::m_mutex` 保护同一连接的所有操作；`Stmt/Rows` 单线程 | `db.cpp:590` | 同一 stmt 并发 Step → SQLite misuse/崩溃 |
| 2 | 绑定参数个数必须等于占位符个数 | `db.cpp` `BindLocked` | 静默写错列/数据 |
| 3 | `Stmt/Rows` 不活得比 `DB` 久；Close 后操作返回 kMisuse | `db.cpp:532`（close_v2）+ `:562` | 悬空 `sqlite3*`；v2 延迟关闭只能兜底 |
| 4 | `ColumnTextView` 只到下一次 Next/Reset/Finalize | `db.hpp:40` | 读到被覆盖的行缓冲 |
| 5 | 每条迁移独立事务；`m_build_error` 污染整个对象 | `orm.cpp:818` / `:850` | 批量迁移「部分生效」被误认为全失败 |
| 6 | LevelDB 迭代器必须在 `DB::Close` 前销毁 | `leveldb_kv.cpp:257` | Close 后 Key/Value 是 UAF（兜底只防 double free） |
| 7 | Queue 游标单调取 max；Ack 批量落盘 | `queue.cpp:217` / `:472` | 游标回退→重复投递；不回退保证不丢 |
| 8 | Watch 同步回调在写线程内、KV 写成功后；无订阅者不写日志 | `watch.cpp:272` / `watch.hpp:13` | 回调里再写同一 KV 可能自锁/递归 |
| 9 | MemCache 每片一把锁；TTL 用绝对时间戳比较 | `memcache.cpp:404` / `time_wheel.hpp:63` | 跨片无全局 LRU；回绕误判过期 |
| 10 | KV 写入先过 WAL 才可见；epoch 无锁读 | `kv.cpp:637` / `:765` | 崩溃丢已确认写；读到半写状态 |

---

## 第二部分 · 分：逐块细讲

### 2.1 SQLite 封装（`db.hpp` / `db.cpp`）

**职责**：把 `sqlite3*`/`sqlite3_stmt*` 的生命周期、错误码、字符串所有权收进 RAII，SQL 文本仍由调用方写。

**数据结构**：
- `Error`（`db.hpp:115`）：`code/extended_code/message`，空错误 = 成功。
- `Value`（`db.hpp:167`）：NULL/int64/double/text/blob/bool 的标签联合；`Blob` 是 `{data,size}` 视图。
- `Stmt`（`db.hpp:300`）：`m_db/m_stmt/m_error/m_parameter_count/m_column_count/m_has_row/m_done`；构造时缓存参数/列数，避免每个列访问都问 SQLite。
- `Rows`（`db.hpp:414`）：持有 `Stmt` + `m_err`；`Next()` 把执行错误收进 `m_err`。
- `DB`（`db.hpp:484`）：`m_db` + `m_mutex` + `m_last_error`；`Handle()` 暴露原生句柄但加锁。

**关键函数**：`ExecImpl`（`db.cpp:587`）逐条 prepare/绑定/step/finalize；`QueryImpl`（`:644`）只允许单条；`Prepare`（`:676`）只允许单条；`BindLocked` 严格校验参数数；`Step`（`:359`）区分 ROW/DONE/错误。

**失败路径**：prepare 语法错误 → `kError`；约束 → `kConstraint`；busy 5s 超时 → `kBusy`；`Close` 后 → `kMisuse`；多语句+参数 → `kMisuse`（第一条可能已生效）。

**典型误用**：用 `Exec` 跑带参数的多语句脚本（部分执行）；把 `ColumnTextView` 存进 `std::string_view` 跨行使用。

### 2.2 事务、Close 与 WAL

- `Begin`（`db.cpp:702`）执行 `BEGIN`（默认 deferred），`Commit`（`:724`）/`Rollback`（`:745`）直接转发；`Transaction` RAII（`db.hpp:589`）析构自动回滚。
- `Close`（`:532`）用 `sqlite3_close_v2`：有未 finalize 的 stmt 时延迟关闭并返回 OK；`m_db` 先置空，后续操作得到 `kMisuse`。这解释了为什么 `Stmt` 可以不比 DB 先析构而不崩溃。
- `SetJournalMode`（`:788`）执行 PRAGMA 后**校验实际返回值**：`:memory:` 不支持 WAL，会原样返回 `memory`；调用方不能假设设置成功。
- `Open`（`:510`）打开扩展错误码并设 `busy_timeout=5000`（`:528`）；多线程写建议每线程一个 DB + WAL。

### 2.3 ORM 映射（`orm.hpp`）

**职责**：宏/反射把 C++ 结构体映射到表；`TableDef`/`Column` 是运行时元数据，SQL 由 `CreateTableSql`/`InsertSql` 生成。

**关键结构**：`Column`（`orm.hpp:202`）、`TableDef`（`:268`）、`Table<T>`（`:470` 声明，`:558`/`:946` 两套宏生成特化）、`Session`（`:1005`）、`Query<T>`（`:1193`）。

**关键函数**：`Session::Find`（`:1049`）按主键查；`All`（`:1067`）全表；`Query::All`（`:1272`）/`First`（`:1285`）拼 WHERE + 绑定参数。

**失败路径**：SQL 拼装错误/类型不匹配返回 `Result<T>` 的错误；`Save` 用 `INSERT ... ON CONFLICT` 还是 update 取决于主键存在性（读 `orm.cpp` 实现）。

**典型误用**：以为宏映射有运行期反射开销——实际是编译期生成的 `Table<T>` 特化；但字段类型必须与 `ColumnType` 对得上，否则绑定失败。

### 2.4 迁移（`orm.hpp` / `orm.cpp`）

- `Migration`（`orm.hpp:1444`）持有 `DB*`、版本表名、`vector<MigrationStep>`、`recursive_mutex`、`m_build_error`。
- `Add`（`orm.cpp:712`）要求版本严格递增；违反只置 `m_build_error`，不抛。
- `AddCreateTable`（`orm.hpp:1458`）由 `TableDef` 生成 `CREATE TABLE` + `DROP TABLE`。
- `AddColumn`（`orm.cpp:746`）在 SQLite < 3.35 时生成不出 `down`，置 `m_build_error`（本机 3.45 不触发）。
- `ApplyLocked`（`:810`）：`BEGIN IMMEDIATE` → up/down SQL → 写/删版本行 → `COMMIT`；失败回滚该条。
- `UpToLocked`（`:849`）开头检查 `m_build_error`，然后按版本排序逐条 `ApplyLocked`——**整批不是原子**。
- `Down`（`:925`）只回滚最近 N 条已应用；遇到未声明的版本直接报错，已回滚的不恢复。

**典型误用**：把 `Up()` 包在业务事务里（SQLite 不允许嵌套事务）；期待 `Up()` 全成功或全失败。

### 2.5 LevelDB 包装（`leveldb_kv.hpp` / `.cpp`）

- `DbImpl`（`leveldb_kv.cpp:206`）被 `DB` 与 `Iterator` 共享：`atomic<leveldb::DB*> db`、`cache`、`filter_policy`、错误串、统计计数器。
- `DB::Open`（`:277`）把包装 Options 翻译成 `leveldb::Options`；`block_cache_size=0` 表示不用 block cache；`compression=true` 但本机 libleveldb 未链接 snappy 时静默退化。
- `Iterator`（`leveldb_kv.hpp:138`）的 `Key()/Value()` 是 `string_view`，只在下一次 `Next/Seek` 前有效。
- `Iterator::~Iterator`（`leveldb_kv.cpp:257`）只在 `LoadDb()!=nullptr` 时 delete；Close 后跳过 delete（泄漏对象，避免 UAF delete），但视图仍然失效。
- `Flush`（`:499`）= `CompactRange(nullptr,nullptr)`；`Close`（`:509`）先 delete DB 再 reset cache/filter。
- `GetStats`（`:526`）：包装层计数 + `approximate-memory-usage` + `num-files-at-levelN`；`wal_*`/`memtable_flushes` 恒 0，与自研同名统计不可直接比较。

**典型误用**：Close 后继续用迭代器；把 `memtable_bytes` 当自研 KV 的 MemTable 内存。

### 2.6 自研 KV（`kv.hpp` / `kv.cpp`）

- `KV::Impl`：`mutable_` MemTable + `wal_` + SSTable 列表 + epoch/version 管理 + 写锁。
- `Open`（`kv.cpp:1170` → `Impl::Open` `:486`）→ `Recover`（`:502`）重放 WAL。
- `Put`（`:700`）→ `WriteSingle`（`:657`）→ `WriteLocked`（`:637`）：先写 WAL，再改 MemTable；`ShouldFlush` 时 `FlushLocked`（`:825`）落 SST。
- `Get`（`:765`）：先 MemTable，再各层 SSTable；epoch 保护读，不拿写锁。
- `MaybeCompactLocked`（`:900`）在层数/文件数超限时合并；`GetStats`（`:1101`）汇总计数。
- 已知短板（`leveldb_kv.hpp:11` 注释）：1e8 级数据 RSS 不可控、读性能低于 LevelDB、迭代器是快照式全量排序。

**典型误用**：把迭代器当实时视图（它是快照）；以为 `Flush` 等于 fsync（真正 durability 看 `Options.sync`/WAL）。

### 2.7 SegLog（`src/storage/seglog.hpp` / `.cpp`）

- `SegLog::Append`（`seglog.cpp:441`）复用 WAL 块格式（64 KiB 块 + 7B 头 + CRC32C），写用户态 1 MiB 缓冲，满才 `write`。
- 段满 `segment_size` 时封段，把稀疏检查点写成 SSTable（`index_interval` 条一个点），冷启动从检查点定位。
- `Cursor`（`seglog.hpp:138`）按 `seq` 定位；`Next` 返回记录，尾部垃圾在恢复时截断。
- 崩溃语义：Append 成功 = 已进用户态缓冲或已 write 进内核，不等于已 `fdatasync`。

**典型误用**：把 `Append` 当持久化确认；Queue 的 `sync_on_append` 默认 false 就是为此。

### 2.8 Queue（`queue.hpp` / `.cpp`）

- `QueueOptions`（`queue.hpp:34`）：`segment_size`、`write_buffer`、`index_interval`、`sync_on_append`、`cursor_flush_interval`、`retain`、`recover_topics`、`topic_cache`。
- `Queue::Impl`：`map<topic, TopicState>` + 全局日志 `SegLog` + 每 topic 的 `mu`/`deque`/`cursors`。
- `Append`（`queue.cpp:131`）：offset 连续；内存副本受 `retain` 限制，超限后消费转冷读。
- `Consume`（`:175`）：内存优先，冷读用 `SegLog::Cursor` 批量预取。
- `Ack`（`:207`）：单调 max；每 `cursor_flush_interval` 次批量写 KV。
- 语义：at-least-once；崩溃后最多重复一个批量窗口，不丢。

**典型误用**：以为 Consume 会推进游标；以为 Ack 立刻持久化。

### 2.9 Watcher（`watch.hpp` / `.cpp`）

- `WatchOptions`（`watch.hpp:44`）：`sync_callbacks`、`queue_capacity`、`persist_events`、`cursor_flush_interval`。
- `Watcher::Impl`：订阅表（`subs_mu_`）、事件日志 `SegLog`（`log_mu_`）、异步队列（`q_mu_` + 条件变量）。
- `Put/Delete/Write`（`watch.cpp:552/556/`）：先写 KV，成功后匹配前缀订阅者，写事件日志（有人听才写），再 `Dispatch`（`:444`）。
- 同步模式回调在写线程内（`:272`）；异步模式入队，单后台线程派发（`:461`/`:479`），队列满阻塞（背压）。
- `Replay`（`:565`）按订阅游标从事件日志重放。

**典型误用**：在同步回调里再写同一 KV（可能重入/自锁）；`persist_events=false` 时调 `Replay`（没有历史）。

### 2.10 MemCache（`memcache.hpp` / `.cpp`）

- `MemCacheOptions`（`memcache.hpp:35`）：`capacity`、`shards`、`persist`、`read_through`、`purge_batch`、`purge_on_get`。
- `Shard`（`memcache.cpp:404`）：索引表 + LRU 链 + `TimestampWheel` + mutex；`ShardFor`（`:417`）按 hash 取模。
- `Set`（`:572`）：写穿可选；`Get`（`:577`）：命中判过期、提升 LRU；`PurgeExpired`（`:245`）主动清。
- TTL 用绝对时间戳进 256 桶时间轮（`time_wheel.hpp:63`），回绕靠绝对比较。

**典型误用**：容量/分片配成单线程（多分片反而多锁开销）；以为 `read_through` 会在未命中时自动回填（需要 KV 非空且开启）。

### 2.11 后端取舍与测试

| 后端 | 默认 | 定位 | 关键证据 |
|---|---|---|---|
| 自研 KV | ON（`Go2CppKv.cmake:14`） | 生产默认；功能层都建在它上面 | `kv.hpp`、`queue/watch/memcache.cpp` |
| SQLite 封装 | OFF（`Go2CppStorage.cmake`） | 关系型/事务/迁移 | `db.cpp`、`orm.cpp` |
| LevelDB | OFF（`Go2CppLevelDb.cmake:19`） | opt-in 对照；RSS 有界 | `leveldb_kv.hpp:4`：唯一使用方是测试 |

怎么跑（不写结论，只给命令）：
```bash
cd .build-bento-wsl
./go2cpp_storage_db_test
./go2cpp_storage_orm_test
./go2cpp_queue_test --n=100000
./go2cpp_watch_test --n=10000
./go2cpp_memcache_test --n=1000000
./go2cpp_kv_test
./go2cpp_leveldb_kv_test          # 需要构建时 -DGO2CPP_WITH_LEVELDB=ON
```
记录构建类型、数据规模、是否 taskset；换环境必须重测。

### 2.12 锁与并发地图

| 组件 | 锁 | 保护范围 | 并发语义 | 位置 |
|---|---|---|---|---|
| `DB` | `m_mutex`（`std::mutex`） | `m_db` + 所有 Exec/Query/Prepare/事务/PRAGMA | 同一连接串行；多线程建议每线程一个 DB + WAL | `db.cpp:590` |
| `Stmt`/`Rows` | 无 | 单线程对象 | 同一 stmt 不得并发 Step | `db.hpp:39` |
| `KV` | 写锁 + epoch | WAL/MemTable/SSTable 元数据 | 写串行、读无锁（epoch 校验） | `kv.cpp:637` / `:765` |
| `Queue` | 每 topic 一把 `mu` + `cursor_mu_` | topic 状态/游标 | 不同 topic 并行；同 topic 串行 | `queue.cpp` 的 `TopicState` |
| `Watcher` | `subs_mu_`/`log_mu_`/`q_mu_` | 订阅表/事件日志/异步队列 | 写路径不持 subs 锁回调；异步单线程派发 | `watch.cpp:149/122/113` |
| `MemCache` | 每 Shard 一把 | 索引 + LRU + TTL 轮 | 分片并行；跨片无全局顺序 | `memcache.cpp:404` |
| `Migration` | `recursive_mutex` | 整个迁移过程 | 同一对象串行；不保护外部事务 | `orm.hpp:1512` |

### 2.13 迁移状态机（`Migration`）

| 当前状态 | 事件 | 结果 | 位置 |
|---|---|---|---|
| 空对象 | `Add(v,...)` 且 v 递增 | 追加 step | `orm.cpp:712` |
| 空对象 | `Add(v,...)` 且 v 非递增 | 置 `m_build_error`，不追加 | `orm.cpp:714` |
| 有 build_error | 任意 `Up/Down` | 立即返回 build_error | `orm.cpp:850` |
| 无 build_error | `Up()` | 建版本表 → 读已应用 → 逐条 ApplyLocked | `orm.cpp:849` |
| ApplyLocked | `BEGIN IMMEDIATE` 失败 | 返回 begin 错误（可能外层已有事务） | `orm.cpp:818` |
| ApplyLocked | up/down SQL 失败 | `Rollback` + 带版本号错误 | `orm.cpp:820` |
| ApplyLocked | 写/删版本行失败 | `Rollback` + record 错误 | `orm.cpp:839` |
| ApplyLocked | `COMMIT` 失败 | 返回 commit 错误（状态不确定） | `orm.cpp:843` |
| 批量 Up | 第 N 条失败 | 1..N-1 已提交，版本表停在 N-1 | `orm.cpp:874` |
| Down | 已应用版本未声明 | 直接报错，已回滚的不恢复 | `orm.cpp:880` 起 |

### 2.14 错误码总表（SQLite 封装）

| 场景 | `Code` | 来源 | 行号 |
|---|---|---|---|
| 参数个数不匹配 | `kMisuse` | `ParamCountError` | `db.cpp` `BindLocked` |
| SQL 语法错误 | `kError` | `sqlite3_prepare_v2` | `db.cpp:600` |
| UNIQUE/NOT NULL | `kConstraint` | `sqlite3_step` | `db.cpp:624` |
| 写锁竞争超时 | `kBusy` | `busy_timeout=5000` | `db.cpp:528` |
| Close 后调用 | `kMisuse` | `CheckOpenLocked` | `db.cpp:562` |
| Query/Prepare 多语句 | `kMisuse` | `TailIsOnlyWhitespace` | `db.cpp:662` / `:693` |
| Exec 带参多语句 | `kMisuse` | `executed && count!=0` | `db.cpp:611` |
| 迁移声明错误 | `kErrMigration` | `m_build_error` | `orm.cpp:757` |

### 2.15 容量满 / 淘汰 / 回收分别发生在哪

| 层 | 满的条件 | 回收动作 | 位置 |
|---|---|---|---|
| SQLite | 页缓存/磁盘满 | 返回 `kFull`/`kIoErr`；不自动 VACUUM | `db.cpp` 错误映射 |
| 自研 KV | MemTable 超 `write_buffer_size` | `FlushLocked` 落 SST，后台/写线程 compaction | `kv.cpp:825` / `:900` |
| Queue | 段文件超 `segment_size` | 封段 + 稀疏检查点写 SST | `seglog.cpp` 封段逻辑 |
| Queue 内存 | `retain` 超限 | 丢弃未确认内存副本，消费转冷读 | `queue.cpp:131` 起 |
| Watcher 异步 | 队列到 `queue_capacity` | 写线程阻塞（背压），不丢事件 | `watch.cpp:461` |
| MemCache | 片容量满 | LRU 尾淘汰（`evictions++`） | `memcache.cpp:506` |
| MemCache TTL | 到期 | Get 命中判过期 / 每 64 次扫轮 / PurgeExpired | `memcache.cpp:577` / `:245` |

### 2.16 改这个模块前的检查清单

1. 动 `DB` 的公开方法：先确认是否在 `m_mutex` 内、是否检查 `CheckOpenLocked`；`Handle()` 返回的原生指针不能跨线程持有。
2. 动 `Exec`：保持「多语句无参 / 带参单条」的语义，并注意部分执行；不能改成「先扫一遍拒绝」而不评估兼容性。
3. 动 ORM：宏生成的 `Table<T>` 必须与 `TableDef` 列顺序一致；改字段类型要同步 `ColumnType`。
4. 动迁移：任何新校验都只置 `m_build_error`（不要抛），并考虑它对整个对象的影响；批量语义不可悄悄改成全局事务。
5. 动 LevelDB：`Iterator` 生命周期、`Close` 顺序（db → filter/cache）、`Stats` 语义三处最容易坏。
6. 动自研 KV：WAL 先写、MemTable 后改、epoch 读校验，顺序不能换；`Flush` 不等于 fsync。
7. 动 Queue：游标单调 max、批量落盘窗口、冷读回退，三者的组合决定 at-least-once。
8. 动 Watcher：回调里不能持 `subs_mu_`/`log_mu_`；无订阅者路径必须保持 O(1) 原子读。
9. 动 MemCache：TTL 轮用绝对时间戳；分片数按并发度配，单线程多分片更慢。
10. 任何性能结论必须附命令/环境（2.11），不抄文档数字。

### 2.17 一次 `DB::Close()` 的完整时序（为什么 Close 后不崩但也不好用）

1. 调用方 `db->Close()`（`db.cpp:532`）。
2. 先拿 `m_mutex`：与正在执行的 Exec/Query 串行，保证没有人正在用 `m_db`。
3. 保存局部 `db = m_db`，立刻 `m_db = nullptr`：后续任何公开方法走 `CheckOpenLocked` 得到 `kMisuse`（`db.cpp:562`）。
4. `sqlite3_close_v2(db)`（`db.cpp:539`）：若还有未 finalize 的 stmt，SQLite 不返回 BUSY，而是把连接标记为 zombie，等最后一个 stmt finalize 后真正释放。
5. 返回 OK 时 `m_db` 保持空；返回非 OK 时把 `m_db` 还原并记 `m_last_error`（本实现只有极少数情况会走到）。
6. 调用方此时仍可持有 `Stmt`/`Rows` 对象；它们的 `sqlite3_stmt*` 仍有效（v2 语义），但所属连接已进入 zombie。
7. 对 `Stmt` 调 `Step/Reset/Bind`：`Stmt` 自己不检查 DB 状态，操作可能返回 `SQLITE_MISUSE`，也可能继续工作到 finalize——**不要依赖**。
8. `Stmt` 析构 → `sqlite3_finalize`；最后一个 stmt 释放后 SQLite 真正关闭连接。
9. `Rows` 内部持有 `Stmt`，所以 `Rows` 析构会触发同样的 finalize。
10. 若调用方先 `Close` 再 `Open` 同一个 path：新连接与旧 zombie 无关；旧 `Stmt` 仍指向旧连接，属于未定义使用。
11. 想避免这一整类问题：让 `DB` 比所有 `Stmt/Rows` 活得久（作用域顺序），或显式 `stmt.Finalize()` 后再 `Close`。
12. 这也是 `db.hpp:39` 那条「Stmt/Rows 不得比 DB 活得更久」的由来：v2 只保证不崩溃，不保证数据可依赖。


