# 存储模块 · 深入学习

> 面向「要接手改 `include/go2cpp/storage/**` / `src/storage/**` 的人」。
> 行号对源码复核过；性能只写「本机怎么测」，不写文档里的结论数字。

---

## 五层地图（这篇为什么这么长）

| 层 | 回答的问题 | 本文位置 | 读完能得到 |
|---|---|---|---|
| 层 1 · What | 存储抽象解决什么、边界在哪、什么时候不该用 | 第一部分 | 选型判断 |
| 层 2 · How | 连接/表/段/WAL/compaction 这些结构怎么协作 | 第二部分 | 改代码前的结构图 |
| 层 3 · 源码 | `DB::Exec/Query`、`KV::Put/Get`、`LevelDBKv`、`compaction` 逐函数逐分支 | 第三部分 | 能对着行号改 |
| 层 4 · 实战 | 写失败/读未命中/磁盘满：默认、可选、调试 | 第四部分 | 线上排障 |
| 层 5 · 性能 | 1.109×/1.252×/1.09×/4.4× 的口径、复现、下限 | 第五部分 | 能复现也能反驳 |

每一部分末尾都有「小结」，小结只写结论与边界，不重复正文。

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

### 1.5 层 1 小结：存储抽象的四个边界

1. **事务边界**：SQLite 封装有真事务（`BEGIN/COMMIT/ROLLBACK` + 迁移逐条事务）；自研 KV 只有单条/单批原子（`WriteBatch`），**没有跨 key 事务**。业务要「扣库存 + 写订单」原子，只能用 SQLite 或自己在 KV 上做补偿。
2. **一致性边界**：Queue 是 at-least-once（游标批量落盘），Watcher 是 at-most-once（事件日志写失败即丢），MemCache 写穿失败时缓存与 KV 可能不一致。三者都不提供 exactly-once。
3. **持久化边界**：`Put`/`Append` 返回 true 默认只代表「进了用户态缓冲」；真正的 `fdatasync` 由 `sync_writes`/`sync_on_append`/`Sync()` 决定。**默认配置下掉电会丢最后一段缓冲**。
4. **规模边界**：自研 KV 在 1e7（490 MB、4 段）读 1.1–1.25× LevelDB；但迭代器是「快照式全量排序」，1e8 级 RSS 不可控（`leveldb_kv.hpp:11` 注释）。SQLite 适合关系查询与小数据；LevelDB 是 opt-in 对照，不进生产默认路径。

一句话选型：**要事务选 SQLite，要顺序日志选 Queue，要前缀订阅选 Watcher，要低延迟 KV 选自研 KV；四者可以叠，但一致性由调用方兜底。**
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
- `Put`（`:700`）→ `WriteSingle`（`:661`）→ `WriteLocked`（`:637`）：先写 WAL，再改 MemTable；`ShouldFlush` 时 `FlushLocked`（`:825`）落 SST。
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


### 2.18 层 2 小结：结构决定了哪些语义「不可能」实现

- **一连接一 mutex**（`db.cpp:590`）决定 SQLite 封装的并发上限是「同一连接串行」；想要写并发只能每线程一个 `DB` + WAL，代价是 `busy_timeout` 竞争。
- **MemTable 是固定容量开放寻址**（`memtable.hpp:119-124`）决定它不能原地扩容，只能整表落盘换新表；这也是 `FlushLocked` 存在的根本原因。
- **SSTable 不可变 + 段列表新→旧**（`kv.cpp:445-470`）决定读必须逐段查、写只能追加；compaction 是唯一的回收手段。
- **WAL 是块格式 + CRC**（`wal.hpp:24-50`）决定恢复只能「按 CRC 截断到最后一条完整记录」，半条写不会复活，但同批前面的记录可能已生效。
- **游标与消息体分离**（`queue.cpp:207`）决定 Queue 只能 at-least-once；要 exactly-once 必须把游标写进消息段。

这五条不是实现细节，是**接口承诺的物理来源**。改结构可以，改承诺要先改文档与调用方。

---

## 第三部分 · 层 3：源码逐函数（每个分支都指到行）

> 层 2 回答「有哪些结构」，层 3 回答「一行输入进去，这些结构按什么顺序被改」。
> 本部分只写**读代码能确认的事实**；凡是推论，句子里会带「因为/所以」。

### 3.1 先看 `KV::Impl` 的字段账本（字段级）

`KV::Impl` 的完整字段在 `src/storage/kv.cpp:441-470`。把它按「谁保护谁」重排，比按声明顺序读有用得多：

| 字段 | 行 | 类型 | 谁写 | 谁读 | 不变量 |
|---|---|---|---|---|---|
| `opt_` / `dir_` | `:441-442` | `Options` / `string` | 构造后不变 | 所有路径 | 构造后只读，所以不需要锁 |
| `mu_` | `:443` | `mutable std::mutex` | — | 写路径 / Flush / Compact | **只保护可变 MemTable 与 SSTable 列表的写侧** |
| `mutable_` | `:444` | `unique_ptr<MemTable>` | 写锁内替换 | 同上 | 非空时一定已发布到 `mutable_ptr_` |
| `mutable_ptr_` | `:445` | `atomic<MemTable*>` | `FlushLocked` | `Get` 首行 | acquire 读到即保证该 MemTable 可读 |
| `snapshot_` | `:446` | `atomic<const Snapshot*>` | `PublishSnapshot` | `Get` 慢路径 | 发布后不再修改；旧值延迟回收 |
| `epoch_` | `:447` | `EpochManager&` | — | `Get` 的 `Guard` | 全局单例，跨 KV 实例共享 |
| `wal_` / `wal_path_` | `:448-449` | `unique_ptr<WalWriter>` / `string` | 写锁内换文件 | 写路径 | `AddRecord` 成功前 MemTable 不能改 |
| `next_file_no_` | `:450` | `uint64_t` | 写锁内 | 建 SST/WAL 名 | 单调递增，不复用 |
| `issued_` / `sync_done_` | `:451-452` | `atomic<uint64_t>` | 写路径 / GroupSync | 等待者 | `sync_done_ >= target` 时 `target` 之前的写已 fsync |
| `sync_failed_` | `:453` | `atomic<bool>` | GroupSync 失败 | GroupSync 等待者 | 一旦 true，后续 sync 请求必须失败而不是假装成功 |
| `sync_mu_` | `:454` | `mutex` | — | GroupSync | 只让一个线程真正 `fdatasync` |
| `counters_` | `:455` | `Counters` | 所有热路径 | `GetStats` | 分片计数，读侧不加锁 |
| `last_error_` | `:456` | `string` | `SetError` | `LastError` | 只在失败路径写；成功不清空（这是诊断债，见 4.1） |
| `scratch_` | `:457` | `string` | 写锁内 | 写锁内 | 复用编码缓冲，避免每次 Put 分配 |
| `closed_` | `:458` | `bool` | `Close` | 写路径 | 只在 `mu_` 内读写 |

三个「为什么」：

1. **为什么 `mutable_` 和 `mutable_ptr_` 是两个字段**：`Get` 不能拿 `mu_`（否则读写全串行），所以它只 load 原子指针；写侧替换 MemTable 时先构造新的、再 store 指针、最后才在锁内替换 `mutable_`。读者看到的要么是旧表、要么是新表，不会看到「一半」。
2. **为什么 `snapshot_` 用 `atomic<const Snapshot*>` 而不是 `shared_ptr`**：读路径每次 `shared_ptr` 拷贝都要原子增减引用计数，热路径上这是可观测成本；epoch 把「什么时候能 delete」推迟到所有读者离开临界区之后（`kv.cpp:209-240` 的 `EpochManager`）。
3. **为什么 `scratch_` 在锁内**：它是写路径的编码缓冲，只有持锁者用；放在锁外复用会串数据。代价是每次 `Put` 先 `clear()` 再 `resize(4)`，但容量保留，稳态不分配。

**这一节的边界**：`KV::Impl` 没有「连接」概念，所以没有连接池。所谓「池」只有 `NodePool`（cache 模块）与 SSTable 的 mmap 页缓存（内核页缓存），存储层本身不维护 fd 池——每个 `KV` 实例一个目录、一把写锁、一个 WAL fd。

### 3.2 `KV::Put`：从一行调用到 WAL 落缓冲（逐分支）

调用链：`KV::Put`（`kv.hpp:135`）→ `Impl::Put`（`kv.cpp:704`）→ `WriteSingle`（`:661`）→ `WriteLocked`（`:641`）→ `WalWriter::AddRecord`（`wal.hpp:35` 声明）+ `ApplyBatch`（`kv.cpp:449` 声明，实现在 `:560` 附近）。

**`WriteSingle` 的五个阶段**（`kv.cpp:661-702`）：

1. **尺寸门**（`:658-661`）：`key.size()`/`value.size()` 超过 `0xffffffff` 直接 `SetError("key/value too large")`。这是格式上限，不是业务限制——超过就编不进 varint32。
2. **编码进 `scratch_`**（`:668-687`）：先 `clear()`，`resize(4)` 留出 batch 头，push type，再 varint32(key_len)+key、varint32(val_len)+value，最后 `EncodeFixed32(&scratch_[0], 1)` 回填「1 条记录」。**顺序不能换**：头部的 count 必须在内容写完后回填。
3. **`WriteLocked`**（`:688`）：见下。
4. **取序号**（`:692`）：`issued_.fetch_add(1)` 已经在 `WriteLocked` 里做过；这里读回自己的序号 `my`，供 group commit 等待。
5. **同步模式**（`:695-697`）：`opt_.sync_writes` 为真才 `GroupSync(my)`；默认 false，所以 `Put` 返回 true 只代表「进了 WAL 用户态缓冲」。

**`WriteLocked` 的四步**（`kv.cpp:641-655`）：

```text
if (mutable_->ShouldFlush())        // ① 先判断要不要落盘
    if (!FlushLocked()) return false;
if (wal_ == nullptr || !wal_->AddRecord(data))   // ② WAL 先写
    { SetError("wal append failed"); return false; }
if (!ApplyBatch(data))              // ③ 再改 MemTable
    { SetError("apply batch failed"); return false; }
counters_.Add(kWalRecords / kWalBytes); issued_++;  // ④ 统计
```

① 为什么先 `FlushLocked`：MemTable 超过预算后再写会让内存无界；先落盘再写新记录，保证「任何时刻只有一张可变 MemTable 超预算」。② 为什么 WAL 必须先于 `ApplyBatch`：崩溃恢复只能重放 WAL，如果先改内存再写日志，日志缺一条而内存已可见，恢复后就少了这条写（丢已确认写）。③ 为什么 `ApplyBatch` 失败不回滚 WAL：WAL 记录格式错误属于实现 bug/磁盘损坏，回滚也没有意义；此时 `last_error_` 被设置，后续写仍会尝试（没有熔断），调用方必须自己看返回值。

**`AddRecord` 内部的三件事**（`wal.hpp:35`，实现 `wal.cpp:168` 附近，与 SegLog 共用块格式）：

1. 当前块剩余空间不够 `7 + len` 时，先补零到块尾，再从新块起写（读侧靠 CRC 判断补零）。
2. 头 7 字节 = CRC32C(4) + length(2) + type(1)；type 区分整条/分片首/中/尾。
3. 记录体追加到 `buf_`；`buf_.size() >= flush_threshold_` 才 `write(fd)`。**这是用户态缓冲，不是 fsync**。

**失败路径总表**：

| 失败点 | 返回 | `last_error_` | 数据状态 | 调用方该做什么 |
|---|---|---|---|---|
| key/value 过大 | false | `key/value too large` | 未写 | 截断/拒绝业务输入 |
| `FlushLocked` 失败 | false | 由 Flush 设置（open/write 错误） | 旧 MemTable 仍在，新记录未写 | 停写、查磁盘、重启恢复 |
| `AddRecord` 失败 | false | `wal append failed` | WAL 可能半条（写失败时块尾可能脏） | 视为实例不可用，`Close` 后重开 |
| `ApplyBatch` 失败 | false | `apply batch failed` | WAL 有、MemTable 无 | 同上；重启后靠 WAL 补 |
| `GroupSync` 失败 | false | 无（只置 `sync_failed_`） | 数据在 WAL 缓冲/内核，未 fsync | 不能当持久化成功；重试或报错 |

最后一行是真实的坑：`sync_failed_` 是原子标志，`GroupSync` 失败后所有等待者都返回 false，但 `last_error_` 不会被写——所以「为什么失败」只能靠 errno/日志，不能靠 `LastError()`。这一点在 4.1 展开。

### 3.3 `KV::Get`：三级查找与两次哈希（逐分支）

`KV::Impl::Get` 在 `kv.cpp:769-817`，是本模块最热的一条路径。按执行顺序拆：

1. **计数**（`:768`）：`counters_.Add(kGets)`，分片 TLS 累加，一条 `lock addq`。这是读路径上 12 个 `lock` 的第一个。
2. **可变 MemTable**（`:769-780`）：acquire load `mutable_ptr_`，`Lookup` 返回三态：
   - `kFound` → 记 hits/memtable_hits，返回 true；
   - `kDeleted` → **直接返回 false**，不再查下层（墓碑遮蔽）；
   - `kNotFound` → 继续。
3. **进入 epoch 临界区**（`:784`）：`EpochManager::Guard` 构造时把当前线程标成活跃读者；析构时回 idle。它保证这期间 `snapshot_` 指向的 SSTable 不会被 delete。
4. **immutable MemTable 线性查**（`:785-800`）：`snap->immutables` 是 `FlushLocked` 正在落盘的那张表；命中/墓碑语义同上。
5. **算一次 key hash**（`:801`）：`Hash32(key)`，后面所有 SSTable 复用。
6. **逐段 SSTable**（`:802-815`）：
   - 先 `MayContain(key, key_hash)`（Bloom）。阴性 → `kBloomNegatives++`，跳过；**这是绝大多数未命中的出口**。
   - 阳性 → `table->Get(key, value)`：内部 `FindBlock` 二分索引 + 4 KiB 块内顺序扫描；命中拷 value 到调用方 string。
   - 墓碑 → 返回 false。
7. **全部未命中**（`:816`）：返回 false，不写任何东西。读不产生写放大。

**两次哈希的结构性冗余**（`docs/reports/storage-opt.md` §4 已记录）：`Get` 先 `table->MayContain(key, key_hash)`，`SSTable::Get` 内部又调一次 `MayContain(key)` 重新 `Hash32` + 再探 Bloom。命中段每次 Get 多一次哈希 + 一次 Bloom，估算 30–60 ns（3–5%）。修法是给 `SSTable::Get` 加 `bloom_checked` 参数；本次未改（不在允许修改范围）。

**为什么读路径能不加锁**：`mutable_ptr_` 与 `snapshot_` 都是原子发布；MemTable 的槽位用 `state` 原子从 `kConstructing(1)` 变 `kOccupied(2)`（`memtable.hpp:90-100`），读者看到 constructing 就重试。x86 上 acquire load 是普通 `mov`，所以「无锁」不是靠魔法，是靠「发布顺序 + 不回收」。代价是 epoch 的两次 release store（约 5 ns）。

**读路径的指令画像**（`storage-opt.md` §3.2，`-O3`、`objdump`）：`KV::Impl::Get` 320 条指令、17 个 call、12 个 `lock`、0 次分配、0 把 mutex、40 B 栈帧。12 个 `lock` **全部**是分片统计的 `fetch_add`（7 个调用点在 `Counters::TlsShard`）——读路径没有锁竞争，只有缓存行争用。

### 3.4 `KV::FlushLocked` 与 `MaybeCompactLocked`：写放大的现场

**`FlushLocked`**（`kv.cpp:829-900`）：

1. `mutable_` 为空或空表 → 直接返回 true（`:830-832`）。
2. 新建 SST 文件号，`SSTableBuilder` 把 MemTable 全量写盘（`:840` 起）；`Finish()` 里做收尾 fsync。
3. **换 WAL**（`:885-894`）：先开新 WAL、再 `unlink` 旧 WAL。顺序反了会在崩溃窗口里丢掉「已落 SST 但旧 WAL 已删」之间的写。
4. 发布新快照 `PublishSnapshot`：旧快照进 epoch 退休队列，读者走完才 delete。
5. `MaybeCompactLocked()`（`:900`）。

**`MaybeCompactLocked`**（`kv.cpp:904-1000`）是本轮性能修复的核心，逐行：

```text
cur = snapshot_.load(acquire)
if (cur == nullptr || cur->sstables.size() <= max_sstables) return;  // ①
total = cur->sstables.size()
// ② 扫描所有相邻段对，取 file_size 比值最接近 1 的一对
for (i = total; i > 1; --i) {
    a = sst[i-2]->file_size(); b = sst[i-1]->file_size();
    ratio = (hi * 1024) / lo;
    if (ratio < best_ratio) { best_ratio = ratio; first = i-2; }
}
// ③ 只合并 [first, first+2)，输出段替换该区间（保持新→旧顺序）
// ④ 多路归并：最小堆 + 同 key 时 src 小（更新）优先
// ⑤ drop_tombstones = (inputs.size() == total)：只有覆盖全部段才能丢墓碑
```

- ① 是**触发条件**：段数超过 `max_sstables`（默认 4）才合并。注意是 `>`，所以第 5 段出现时才动手。
- ② 是**几何策略**：旧实现固定取最旧两段，最旧的大段被之后每个新段反复重写，单字节重写次数 O(N) → 1e7 条实测写放大 36×。改成「大小最接近的相邻对」后，每段只在遇到同量级邻居时重写，单字节 O(log N) 次，写放大降到 7×。
- ③ 的「原地替换区间」很关键：早期写成 `resize(keep)` + 尾插，会把中间位置的合并结果挪到队尾，破坏「新→旧」顺序，读侧就会先读到旧值。现在是 `[first, first+2)` 换成新段。
- ④ 同 key 时 `src` 小优先，因为 `sstables[0]` 最新；这保证合并后仍是新值覆盖旧值。
- ⑤ 墓碑只有「合并覆盖全部段」时才能丢；否则删掉的 key 会在更旧段里复活（这是 LSM 最经典的 bug）。

**没采用的方案与原因**（`storage-opt.md` §3 备注）：后台 compaction 线程能把 Put 阶段做到 1.73 M/s、写放大 4×，但与现有测试「未 `Close` 就 `remove_all(dir)`」冲突——后台线程还在写目录，`remove_all` 抛 `Directory not empty`，`kv_test` 崩溃。要保留异步必须改测试清理方式或加生命周期协议，超出「只改 `src/storage`」的边界。

### 3.5 LevelDB 包装：`Put/Get/Close` 的四个易错点

`src/storage/leveldb_kv.cpp` 是 opt-in 对照实现（默认 OFF），但它把「官方 LevelDB 的语义」暴露给测试，所以接口边界必须精确：

| 函数 | 行 | 做了什么 | 易错点 |
|---|---|---|---|
| `DB::Open` | `:277` | 翻译 Options、建 block cache、bloom、`leveldb::DB::Open` | 失败只写 `g_last_open_error`，返回 nullptr；不看这个串就只剩「打开失败」 |
| `Put` | `:218`（声明）/ 实现 `:380` 附近 | `impl_->puts++`（relaxed）+ `db->Put` | 默认 `WriteOptions.sync=false`；返回 true 不等于落盘 |
| `Get` | `:228` | `db->Get` + `gets/hits` 计数 | 未命中返回 false，不区分「不存在」与「读错误」；错误在 `LastError()` |
| `Write` | `:224` | 原子批 `leveldb::WriteBatch` | 批内一条失败整批不生效；与自研 KV 的 `WriteBatch` 语义一致 |
| `Iterator` | `:232` | `NewIterator` + `SeekToFirst` | `Key()/Value()` 是 `string_view`，下一次 `Next/Seek` 后失效 |
| `Close` | `:509` | `delete db` → `filter_policy.reset()` → `cache.reset()` | 顺序不能反：`DBImpl` 析构前还持有 filter/cache |
| `Flush` | `:499` | `CompactRange(nullptr,nullptr)` | **只做 compaction，不是 fsync 屏障**；掉电仍可能丢最近写 |

`GetStats`（`:526`）里 `wal_*`/`memtable_flushes` 恒 0——包装层拿不到 LevelDB 内部 WAL 计数。把这份 Stats 与自研 KV 的同名字段直接比较，会得出「LevelDB 不写 WAL」的错误结论。

### 3.6 Queue / Watcher / MemCache 的代码级差异

这三个功能层都建在 `KV` 上，但「谁负责持久化」不同：

| 组件 | 数据落哪 | 谁 fsync | 崩溃后恢复 | 关键行 |
|---|---|---|---|---|
| Queue | 消息体 → SegLog；游标 → KV | `Sync()` 或 `sync_on_append` | 扫段重建 `next_offset`，游标取 KV 值 | `queue.cpp:131` / `:207` / `:472` |
| Watcher | 事件 → SegLog；订阅游标 → KV | 异步派发线程按批 | `Replay` 从游标重放；无日志则只能实时 | `watch.cpp:552` / `:565` |
| MemCache | 值 → KV（`persist=true` 写穿）；索引不落盘 | 跟随 KV 的 `sync_writes` | 重启后缓存空，命中率从 0 重建 | `memcache.cpp:572` / `:577` |

**Queue 的游标协议**（`queue.cpp:207-230`）：`Ack` 只在 `offset+1 > cur` 时更新，`BumpAckLocked` 累计到 `cursor_flush_interval`（默认 4096）才批量写 KV。这带来 at-least-once：崩溃最多重复一个批量窗口，不丢。**为什么不做 exactly-once**：游标与消息体不在同一个原子写里，要 exactly-once 必须把游标写进消息段（同一次 `Append`），代价是每次消费都要写日志。

**Watcher 的锁外回调**（`watch.cpp:272` / `:444`）：同步模式在写线程内回调，但**不持 `subs_mu_`/`log_mu_`**——先拷贝匹配的订阅者列表，释放锁，再逐个回调。所以回调里再写同一 KV 不会自锁，但会递归触发下一轮事件（业务要自己防环）。

**MemCache 的 TTL 轮**（`memcache.cpp:404-430` + `time_wheel.hpp:63`）：到期时间是绝对毫秒，插入 256 桶轮；`Get` 命中先比较绝对时间，过期就 `RemoveLocked` 并返回 false。`purge_on_get=false`（默认）时每 64 次 Get 才扫一次轮——**过期条目不保证及时释放**，只保证不会被读到。

### 3.7 层 3 小结：三条「顺序不能换」

1. **WAL 先于 MemTable**（`kv.cpp:647` → `:651`）：反了就是丢已确认写。
2. **新 WAL 开好再删旧 WAL**（`:885-894`）：反了就是崩溃窗口丢数据。
3. **合并输出替换原区间，而不是挪到队尾**（`kv.cpp` 合并发布段）：反了就是新值被旧值覆盖。

---

## 第四部分 · 层 4：三个异常场景的「默认 / 可选 / 调试」

> 层 3 讲的是正常路径；层 4 讲「坏了以后系统到底处于什么状态」。
> 每个场景固定三问：**默认行为是什么**、**能开什么可选行为**、**怎么把证据捞出来**。

### 4.1 场景一：写失败（WAL 追加失败 / `fdatasync` 失败 / 写锁内 Flush 失败）

**默认行为（不改任何配置）**

| 失败点 | 返回值 | `last_error_` | 内存状态 | WAL 状态 |
|---|---|---|---|---|
| key/value 超 4 GiB | false | `key/value too large` | 未动 | 未写 |
| `FlushLocked` 打开/写 SST 失败 | false | `open compaction output failed: ...` 或 builder 的错误 | 旧 MemTable 保留、仍可读 | 旧 WAL 保留 |
| `WalWriter::AddRecord` 失败 | false | `wal append failed` | **未改** | 可能半条（块尾脏） |
| `ApplyBatch` 失败 | false | `apply batch failed` | 未改 | 有完整条 |
| `GroupSync` 的 `fdatasync` 失败 | false | **不设置**（只置 `sync_failed_`） | 已改（内存可见） | 已 write 未 sync |

关键结论：**`Put` 返回 false 时，内存与 WAL 可能不一致**。`AddRecord` 失败那条最危险——WAL 可能写了半条，重启时 `WalReader` 会按 CRC 截断到上一条完整记录，于是「失败的写」不会复活；但同一次 `write` 里排在它前面的记录可能已经在 `buf_` 里，`FlushBuffer` 会把它们一起写出去。**所以失败后最安全的动作是 `Close` 再 `Open`**，不要在同一实例上继续写。

**可选行为**

1. `Options::sync_writes=true`：每次 `Put` 走 `GroupSync`，返回 true 才代表 `fdatasync` 完成。代价是每次写一个 fsync（本机 4 KiB 约 475 µs），吞吐掉两个数量级；适合「元数据/游标」这类低频强持久写，不适合消息体。
2. `Queue` 的 `sync_on_append=true`：把「追加即持久」变成默认，代价同上。
3. 业务侧重试：只在返回 false 且 `LastError()` 属于**可重试**类别（`EINTR`、`EAGAIN`）时重试；`ENOSPC`/`EIO` 重试只会烧 CPU。
4. 熔断：本模块**没有**内置熔断，`last_error_` 设置后写路径仍会继续尝试。要熔断必须业务层看到 false 后自己停写并重建实例。

**调试**

```bash
# 1) 看实例自报的错误（注意：GroupSync 失败不写这里）
#    C++ 侧：kv.LastError() / kv.GetStats().wal_syncs

# 2) 看系统调用在哪一步失败
strace -f -e trace=write,fsync,fdatasync,openat,unlink -p <pid>

# 3) 看 WAL 是否在长（写失败但文件在长 = 半条/缓冲）
ls -l ./data/*.log && stat -c '%s %y' ./data/*.log

# 4) 看统计是否自洽：puts 增而 wal_records 不增 = AddRecord 前就失败
#    GetStats() 打印 puts/gets/wal_records/wal_bytes/wal_syncs
```

一个常见误判：`wal_syncs=0` 不代表「没写 WAL」，只代表 `sync_writes=false`（默认）。WAL 仍然每条都写，只是不 fsync。`storage-opt.md` §2 的 36× 写放大实验里 `wal_syncs=0`，WAL 完全不是瓶颈。

### 4.2 场景二：读未命中（key 不存在 / 被删 / Bloom 假阳性 / 读错误）

**默认行为**：`Get` 返回 false，`value` 不被修改（调用方必须自己保证 `value` 的初值可接受）。四种「false」在代码里**不可区分**：

| 原因 | 代码路径 | 统计 | 能否区分 |
|---|---|---|---|
| key 从未存在 | 三级查找都 miss | `gets++`，无 hits | 不能（只能看 `bloom_negatives`） |
| key 被删（墓碑） | MemTable/immutable/SST 返回 `kDeleted` | 无 hits | 不能；墓碑会**遮蔽**更旧的值，这是正确行为 |
| Bloom 假阳性后块内未找到 | `MayContain` 真、`Get` 返回 not found | `bloom_negatives` 不增 | 不能 |
| 读错误（mmap 失败/文件被删） | `SSTable::Get` 返回 not found 或错误 | 无专用计数 | 不能；只能看 `LastError()`（读路径不设置它） |

**这是有意的接口选择**：`Get` 的契约是「在不在」，不是「为什么不在」。要区分必须自己加计数或换 `Iterator`（能区分「没有下一个」与「读失败」更差）。

**可选行为**

1. `MemCache::read_through=true`（`memcache.hpp:35` 的选项）：未命中时回填 KV。注意它只在「KV 非空且开启」时生效；`read_through=false` 时 `Get` 未命中就真的什么都不做。
2. Bloom 参数：`Options` 里 SSTable 的 bloom bits 越高假阳性越低、内存越大；默认 10 bit/key 约 1%。调高到 16 bit 假阳性约 0.05%，但每段 Bloom 从约 3 MB 涨到约 5 MB（4 段多 8 MB 常驻）。
3. 预取/缓存：`KV` 不做 block cache，靠 mmap + 内核页缓存；工作集超过内存时读放大由页回收决定，调 `vm.swappiness`/`madvise` 比调代码更直接。

**调试**

```bash
# 1) 看命中的是哪一层：memtable_hits / immutable_hits / sstable_hits
#    memtable_hits 高 = 工作集新；sstable_hits 高 = 冷读，受页缓存影响
# 2) 看 Bloom 是否在挡：bloom_negatives 与 gets 的比值
#    比值接近 1 = 绝大多数 key 根本不在段里（正常）；接近 0 = 段少或 key 集中
# 3) 确认 key 是否真被删：用 Iterator 扫前缀，墓碑不会出现在迭代器里
# 4) 确认不是读错误：读路径不写 last_error_，要 strace -e openat,mmap 或看 dmesg
```

**一个真实陷阱**：`MemCache` 的 `Get` 命中过期条目时返回 false 并删除（`memcache.cpp:577`），调用方如果把它当「KV 里也没有」就会少一次回源。正确写法是「MemCache miss → 查 KV → 回填」，而不是「MemCache miss → 返回不存在」。

### 4.3 场景三：磁盘满（ENOSPC / SQLite `kFull` / 段文件写不进去）

**默认行为**

| 层 | 触发点 | 返回/错误 | 已有数据 | 后续 |
|---|---|---|---|---|
| SQLite | `sqlite3_step` 返回 `SQLITE_FULL` | `Code::kFull`（映射见 `db.cpp` 错误表） | 事务未提交则回滚 | 连接仍可用；VACUUM 要额外空间，满了做不了 |
| 自研 KV | `WalWriter::AddRecord` 的 `write` 返回 ENOSPC | `wal append failed`，`Put` false | WAL 文件可能半条 | 实例应重建；继续写只会反复失败 |
| 自研 KV | `SSTableBuilder::Finish` 写失败 | `FlushLocked` false，`Put` false | 旧 MemTable 保留 | 同上 |
| Queue | 段文件写失败 | `Append` false；`Fail(...)` 记录 | 已写段完好 | 消费仍可读旧段 |
| Watcher | 事件日志写失败 | 同步模式下写调用返回 false | KV 已写、事件没记 | 订阅者收不到这条；`Replay` 也补不回 |
| MemCache | `persist=true` 写穿失败 | `Set` false | 缓存里可能已有新值 | **缓存与 KV 不一致**；需要业务决定是否回滚缓存 |

最后一行最隐蔽：`Set` 先改缓存再写穿（顺序见 `memcache.cpp:572` 起），写穿失败时缓存里是新值、KV 里是旧值。默认没有回滚——`persist` 的语义是「尽力写穿」，不是「缓存与存储强一致」。

**可选行为**

1. **预留水位**：应用层在磁盘使用率 > 90% 时停写并告警；存储层不做这件事，因为「预留多少」是部署决策。
2. **先删后写**：`KV::FlushLocked` 会 `unlink` 旧 WAL（`:893`），能回收一点空间，但只在换 WAL 时；不要指望它救急。
3. **换目录/扩容**：`KV` 的目录在 `Open(dir)` 时固定，不能热迁移；要迁移必须 `Close` → 拷贝 → 新实例 `Open`。
4. **SQLite 的 `PRAGMA auto_vacuum`**：只能建库时设，已有库要 `VACUUM`，而 `VACUUM` 需要约等量的临时空间——磁盘满时是死锁，必须先在外部腾空间。

**调试**

```bash
# 1) 谁满了：数据目录所在分区，不是根分区
df -h ./data && du -sh ./data/*

# 2) 哪些文件在长：WAL 还是 SST
ls -lS ./data | head

# 3) 确认 errno：write 返回 -1 且 errno=ENOSPC
strace -f -e trace=write,openat,unlink -p <pid> 2>&1 | grep -E 'ENOSPC|write'

# 4) SQLite：错误码是 kFull 还是 kIoErr（前者满、后者硬件/权限）
#    db.LastError().message / extended_code
```

**恢复顺序**（很重要）：先腾空间 → 再 `Close` 当前实例 → 再 `Open`。不要「腾一点空间后继续用同一实例」：WAL 可能停在半条记录上，`AddRecord` 的 `block_offset_` 已经前进，后续记录会接在半条之后，重启时 CRC 截断会把它们一起丢掉。

### 4.4 层 4 小结：三个场景的共同结构

- **默认都不抛异常**：全部走 `bool` + `LastError()`/`Code`，因为存储层要能在 `-fno-exceptions` 的嵌入式构建里用。
- **默认都不熔断**：失败后实例仍接受调用，是否重建由调用方决定。这是「库」而不是「服务」的定位。
- **默认都不保证跨结构一致**：WAL 与 MemTable、缓存与 KV、KV 与事件日志，三者都没有分布式事务；一致性边界在调用方。

---

## 第五部分 · 层 5：性能（数字、口径、复现、以及别抄的旧值）

> 原则：**只写本机可复现的数字 + 引用出处**；文档里出现的旧数字必须标口径，
> 因为本项目已经发生过一次「0.48× 被跨口径对比制造出来」的事故。

### 5.1 机器与编译口径

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 9 8945HX（12 vCPU / 32 MiB L3） |
| 内存/系统 | 19 GB / WSL2 6.6.87 / ext4（`/dev/sdd`） |
| 编译器 | g++ 13.3.0 |
| 选项 | `-O3 -DNDEBUG -fno-semantic-interposition -fno-stack-protector`（与 Release 一致） |
| 负载 | 单线程、`taskset -c 2,3`；key 20 B、value 22 B；1e7 条 = 490 MB、4 段 |

出处：`docs/reports/storage-opt.md` §1/§2。换机器必须重测，尤其 L3 大小与磁盘。

### 5.2 读：1.109×（全流程）与 1.252×（Get-only 中位）

同机、同 harness、同 key 序列，对官方 LevelDB：

| 口径 | LevelDB | 自研 KV | 倍率 |
|---|---:|---:|---:|
| 1e7 全流程 Get（Put 之后随机读） | 613,941 ops/s | 680,915 | **1.109×** |
| Get-only 三轮中位（预建库，交替跑） | 687,216 | 860,176 | **1.252×** |
| 仓库自带 `kv_bench` 2e6 | — | 1,099,327（ext4） | — |
| MemTable-only（2e6 常驻） | — | 3,316,050 | — |

**为什么两个口径差这么多**：全流程的读紧跟在 1e7 次写之后，页缓存被写脏页挤压、compaction 余波未消；Get-only 是「库已建好、只读」，更接近稳态服务。对外报数时**必须两个都给**，只报 1.252× 会被质疑「挑好的说」。

**为什么能赢**：MemTable 用开放寻址哈希（16 B 槽、平均 1.3 次探测、不 rehash）而不是跳表；SSTable 全程 mmap、无 `read()`、无读侧 CRC；块内用 offset 数组二分 + 4 KiB 顺序扫描（约 40 条）。代价是写路径（见 5.3）。

**结构差异表**（`storage-opt.md` §3.3）：

| 层 | LevelDB | 本实现 | 读路径影响 |
|---|---|---|---|
| MemTable | 跳表，2–3 次 cache miss | 开放寻址，1.3 次探测 | 内存命中 3–5× |
| 块内 | restart 点 + 线性 | offset 二分 + 线性 | 块内更快 |
| 读缓存 | 8 MB block cache | mmap + 页缓存 | 大工作集少一次拷贝 |
| Compaction | 后台线程 | 写锁内同步 | 读不受影响、写受影响 |

### 5.3 写：1.09×（修复后），以及 36× 写放大是怎么来的

同一 harness、1e7 Put、默认 8 MB MemTable、`sync_writes=false`：

| 版本 | Put ops/s | ns/op | `bytes_written` | 写放大 | 结束段数 |
|---|---:|---:|---:|---:|---:|
| 修复前（固定合并最旧两段） | 159,690 | 6262.1 | 17,688 MiB | **36×** | 4 |
| 修复后（相邻最近大小配对） | **703,517** | 1421.4 | 3,464 MiB | **7×** | 4 |
| 同轮 LevelDB | 643,160 | 1554.8 | — | — | — |
| 仓库 `kv_bench` 2e6（32 MB MemTable） | 1,924,739 | — | — | — | — |

- **4.4×** = 703,517 / 159,690；写放大 36× → 7×；Put 阶段 wall 62.6 s → 14.21 s。
- **1.09×** = 703,517 / 643,160，达到「≥0.9× LevelDB」的目标。
- `kv_bench` 的 1.92 M/s **不是**引擎更快：它用 32 MB MemTable，92 MB 数据只 flush 3 次、几乎不触发合并。把 1e7 的结果与 2e6 的结果并列比较，就是重复 0.48× 的口径错误。

**二次方的数学**：旧策略每次取最旧两段，`max_sstables=4` 意味着第 5 段起「每写满一个 8 MB MemTable 就重写一次最旧的大段」。1e7 条触发约 61 次 flush，最旧段被重写约 60 次 → 单字节 O(N) 次。几何策略下每段只在遇到同量级邻居时被重写，单字节 O(log N) 次。

**为什么没直接上后台线程**：后台合并实测 Put 1.73 M/s、写放大 4×，但与 `kv_test` 的「未 Close 就 `remove_all(dir)`」冲突（`Directory not empty`）。要异步必须改测试清理或加生命周期协议；本轮只改 `src/storage`，所以保留同步 + 几何策略。

### 5.4 物理下限：还差 7.5–9.9×

| 指标 | 值 |
|---|---:|
| Get-only 单次 | 1162.6 ns |
| 同机 64 MiB 随机依赖链 | 117.8 ns |
| 倍数 | **7.5–9.9×** |

差在哪（`storage-opt.md` §5）：每次 Get 平均 2.5 次段级 Bloom（4 段、每段约 3 MB）+ 约 15 次索引二分（每段索引约 900 KB）+ 1 次 4 KiB 块扫描，合计 4–6 次 LLC/TLB miss；490 MB 工作集远超 32 MiB L3。**这不是指令数问题，是访存问题**，继续抠指令收益 <2%。

已量化的两处可省（都 <2%，且有语义代价）：

1. 命中路径 3 次分片 `fetch_add`（约 15–20 ns，占 1.3–1.7%）：去掉/合并会改变 `GetStats()` 的 gets/hits/memtable_hits/sstable_hits 语义。
2. `EpochManager::Guard` 两次 release store（约 5 ns，<0.5%）：去掉会破坏 `Retire/Reclaim` 的延迟回收安全，可能 UAF。

### 5.5 怎么复现（命令级）

```bash
# 0) 构建（Release，storage 默认不开 SQLite/LevelDB）
cmake -S . -B .build-bento-wsl -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_KV=ON
cmake --build .build-bento-wsl -j

# 1) 仓库自带 KV 基准（2e6，快）
taskset -c 4,5 .build-bento-wsl/go2cpp_kv_bench --n=2000000

# 2) 1e7 同机对照：需要系统 LevelDB 静态库 + 手工编 test_leveldb_kv.cpp
#    具体步骤见 docs/reports/storage-opt.md §2.1（源码树未改，产物在 /var/tmp）

# 3) 正确性（性能改动后必跑）
.build-bento-wsl/go2cpp_kv_test
.build-bento-wsl/go2cpp_kv_concurrent_test
.build-bento-wsl/go2cpp_kv_recovery_test
```

记录：CPU 型号、是否 `taskset`、构建类型、`n`、MemTable 大小、`max_sstables`。缺一项，数字就不可比。

### 5.6 别抄的旧数字：0.48× 的完整溯源

| 项 | 内容 |
|---|---|
| 旧数字 | 「本实现 47.5 万 ops/s，LevelDB 公开约 100 万，0.48×」 |
| 出处 | `docs/reports/asm-storage-kv.md:60-68` |
| 被谁引用 | `three-piece-audit.md:43` A14、`:200` |
| 错在哪 | LevelDB 侧是**公开/文献数据**（小数据集、单段、块缓存热），本实现是**1e7、490 MB、4 段、Bloom 冷**；机器、规模、缓存状态三项全不同 |
| 反证 | `docs/metrics-ledger.md` M07 同机同 harness：576,109 vs 530,742 = 1.086×（2026-11-03）；本轮复测 1.109–1.252× |
| 正确写法 | 「同机同负载 1.109×（全流程）/ 1.252×（Get-only 中位）；0.48× 为跨口径对比，已作废」 |

**教训**：性能数字必须带四元组（机器、数据规模、缓存状态、编译选项）。只写「X 倍」而不写口径，等于制造下一次 0.48×。

### 5.7 层 5 小结：这个模块的性能边界

1. **读已经达标**：1.109–1.252×，剩下的 7.5–9.9× 是访存下限，指令级优化收益 <2%。
2. **写靠架构达标**：4.4× 来自「固定两段 → 几何配对」这一处结构性修改，不是指令优化。
3. **真正的下一个数量级**是后台 compaction（1.73 M/s），但它需要生命周期协议，属于架构 + 接口改动，不是「调参」。
4. **任何报告都要写清口径**，否则会被下一份审计当成事实引用。

---

## 收尾：这篇文档的五层地图与自检

| 层 | 回答什么 | 在哪 |
|---|---|---|
| 层 1 · What | 存储抽象解决什么、边界在哪 | 第一部分 1.1–1.4 |
| 层 2 · How | 关键结构与它们的协作方式 | 第二部分 2.1–2.17 |
| 层 3 · 源码 | 逐函数、逐分支、逐行号 | 第三部分 3.1–3.7 |
| 层 4 · 实战 | 写失败/读未命中/磁盘满的默认-可选-调试 | 第四部分 4.1–4.4 |
| 层 5 · 性能 | 1.109/1.252/1.09/4.4×，口径与下限 | 第五部分 5.1–5.7 |

读完之后应该能回答：

1. `KV::Put` 返回 true 时，数据在「用户态缓冲 / 内核页缓存 / 磁盘」的哪一层？（默认：用户态 WAL 缓冲，未 fsync）
2. `Get` 返回 false 的四种原因，代码里能不能区分？（不能；靠统计与 Iterator 间接判断）
3. 为什么合并必须「原地替换区间」而不是「尾插」？（保持新→旧顺序，否则旧值覆盖新值）
4. 为什么墓碑只在 `inputs.size() == total` 时能丢？（否则被删 key 从更旧段复活）
5. 写放大 36× 的数学结构是什么？为什么几何配对能降到 7×？（O(N) 重写 → O(log N)）
6. 0.48× 是怎么来的、为什么必须作废？（跨口径对比；同机实测 1.086–1.252×）
