# Go2Cpp 理解力考试 · 存储卷（04 storage）

> **考的是「为什么 / 坏在哪 / 边界在哪」**，不是背 API 名。
> **本卷数字均需自测，文档数字仅作历史对照。** 本项目迭代极快，`docs/` 里的性能数字随时可能过时；
> 凡涉及数量的结论，一律以你自己跑出来的为准，并写明测量条件（构建类型、是否 taskset、数据规模）。
> 追查位置只指向源码/头文件/测试/工具；不要拿 `docs/modules/storage-*.md` 的数字当答案。
>
> 覆盖：SQLite 封装、ORM + migrations、LevelDB 集成、LSM 功能层（Queue/Watch/MemCache）、
> 以及「自研 LSM 是否已被弃用」这个流传甚广的错误前提。
>
> 难度分布：L4 = 5（本卷全部是推演/陷阱题，没有背 API 的送分题）。

---

## 一、SQLite 封装

### Q01 【L4 陷阱】`Exec(sql, args)` 带参数执行多语句，为什么会在「第一条已经写进去」之后才报错？

- **痛点**：把 `Exec("...; ...", args)` 当成一个原子批处理。实际上它是「逐条 prepare → 第一条执行 → 第二条发现带参数 → 报 kMisuse」，返回错误时第一条已经 finalize 生效，而且没有事务兜底。
- **追查位置**：
  - `src/storage/db.cpp:587`（`DB::ExecImpl` 用 `sqlite3_prepare_v2` 的 tail 指针循环处理多语句）
  - `src/storage/db.cpp:612`（`if (executed && count != 0)`：拒绝发生在**第二条**，此时 `executed` 已为 true）
  - `src/storage/db.cpp:633`（第一条语句在这里 `sqlite3_finalize`，`executed=true` 在下一行）
  - `include/go2cpp/storage/db.hpp:46`（头注释只写「带参数时只允许单条语句」，没写「第一条可能已生效」）
  - `src/storage/db.cpp:662`（`Query` 用 `TailIsOnlyWhitespace` 直接拒绝多语句，与 `Exec` 策略相反）
  - `tests/storage/test_db.cpp:788`（测试只断言返回 `kMisuse`，**不检查表里是否多了一行**）
- **验收标准**（答到这些算过）：
  1. `ExecImpl` 天然支持多语句：它循环 `prepare` 并用 `tail` 前进，不是「一次 prepare 整串」。
  2. 参数个数校验在循环内逐条做（`BindLocked`），所以第一条占位符数量匹配时会正常 `step` 并 `finalize`。
  3. 拒绝条件是 `executed && count != 0`：第一条无参也照样执行，第二条才被拒；返回 `kMisuse` 时 `ExecResult`（`changes`/`last_insert_rowid`）不会交给调用方。
  4. `Exec` 内部**不开事务**，所以已执行的语句不会回滚；要原子性必须自己 `Begin/Commit`，并把多语句改成单条。
  5. `Query`/`Prepare` 的策略相反：`TailIsOnlyWhitespace` 直接拒绝多语句（`src/storage/db.cpp:662`、`:693`），三者的「多语句」语义故意不一致。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_storage_db_test` → `edges` 场景通过，但它只断言错误码（`tests/storage/test_db.cpp:788`），证明不了没有副作用。正面验证要在同一个 DB 上执行 `Exec("INSERT INTO t VALUES(?); INSERT INTO t VALUES(9);", 5)` 后再 `SELECT COUNT(*) FROM t`：会看到 1 行而不是 0 行。项目没有现成测试覆盖这个副作用，**测试通过不等于语义正确**。
- **常见错误答案**：「参数化多语句会被整体拒绝，什么都不执行」「返回错误就代表事务回滚了」「SQLite 的 prepare 一次能编译整串」——零分。

### Q02 【L4 陷阱】ORM 的 `Migration` 为什么会被一条「声明期错误」整体毒化，`Up()` 也不是整批原子？

- **痛点**：以为迁移失败只会影响出错的那一条。实际上 `m_build_error` 一旦被置位，后续**任何** `Up()/Down()/UpTo()/DownTo()` 都在入口直接返回它；而 `Up()` 又是逐条提交的，前面的版本已经进版本表。
- **追查位置**：
  - `src/storage/orm.cpp:714`（`Add` 发现版本号非递增：不抛异常，只置 `m_build_error`）
  - `src/storage/orm.cpp:753`（`AddColumn` 在 SQLite < 3.35 时生成不出 `down` SQL）
  - `src/storage/orm.cpp:757`（置 `m_build_error`：`ALTER TABLE ... DROP COLUMN` 需要 3.35+）
  - `src/storage/orm.cpp:850`（`UpToLocked` 第一件事就是 `if (m_build_error) return m_build_error;`）
  - `src/storage/orm.cpp:874`（pending 逐条 `ApplyLocked`，每条内部各自 `BEGIN IMMEDIATE`/`COMMIT`）
  - `src/storage/orm.cpp:818`（每条迁移自己的 `BEGIN IMMEDIATE`）
  - `include/go2cpp/storage/orm.hpp:1441`（文档承认「每条迁移在独立事务」）
  - `tests/storage/orm_test.cpp:589`（只测了「一条迁移内部多语句失败整体撤销」，没测「11 成功、12 失败」的批量场景）
- **验收标准**：
  1. `Add()` 的版本号校验和 `AddColumn()` 的 SQLite 版本校验都只记 `m_build_error`，链式声明不报错；错误延迟到 `Up()` 才出现，且**整对象**生效——哪怕你只想跑一条与出错项无关的迁移。
  2. 本机 SQLite 3.45 不会命中 3.35 分支；要复现必须读条件分支或用低版本库，不能靠「跑测试通过」证明这段不存在。
  3. 每条迁移独立事务：批量 `Up()` 中第 N 条失败时，第 1..N-1 条**已提交**，版本表停在第 N-1；文档「失败不会留下半张表」只对**单条**成立。
  4. `BEGIN IMMEDIATE` 意味着调用方不能把 `Migration::Up()` 包在自己的外层事务里（SQLite 不允许嵌套事务），否则在 `begin` 阶段就失败。
  5. 已应用但未声明的版本（例如手工往版本表插了一行）会让 `Down()` 直接报错，且**之前已经回滚的步骤不会恢复**——回滚也不是整批原子。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_storage_orm_test` → `all checks passed`，其中 `TestMigration` 的 broken 用例只覆盖「单条迁移内部撤销」（`broken_table` 不存在、版本表仍是 3）。要观察批量非原子：在同一 `Migration` 里加版本 11（合法）与 12（非法 SQL），`Up()` 失败后 `CurrentVersion()` 会停在 11。`sqlite3 --version` 用来判断你是否可能命中 3.35 分支。
- **常见错误答案**：「`Up()` 要么全成功要么全失败」「版本号写错只影响那一条」「SQLite 的 DDL 不能回滚，所以迁移做不了原子」——零分（SQLite 的 DDL 是事务性的，单条确实原子；错的是整批假设）。

---

## 二、LevelDB 集成

### Q03 【L4 陷阱】`DB::Close()` 之后迭代器再析构，为什么代码选择「泄漏」而不是崩溃？它真的安全吗？

- **痛点**：看到注释「宁可泄漏一个迭代器对象，也不崩溃」，就以为 Close 后还能安全持有迭代器。实际兜底只避免二次释放 `leveldb::Iterator` 对象，迭代器内部的 table cache / memtable 已随 DB 释放，`Key()/Value()` 是 UAF 读。
- **追查位置**：
  - `src/storage/leveldb_kv.cpp:257`（`Iterator::~Iterator`：只有 `owner_->LoadDb() != nullptr` 才 `delete iter_`）
  - `src/storage/leveldb_kv.cpp:261`（注释写明「table cache / memtable 引用已随 DB 释放，delete 会 UAF」）
  - `src/storage/leveldb_kv.cpp:509`（`DB::Close` 先 `delete db` 再 `filter_policy.reset()`、`cache.reset()`）
  - `include/go2cpp/storage/leveldb_kv.hpp:23`（原生约束：迭代器必须在 DB 关闭前销毁；`key()/value()` 只在下一次 Next/Seek 前有效）
  - `tests/storage/test_leveldb_kv.cpp:1088`（测试先 `iterator.reset()` 再 `db->Close()`，顺序不能反）
- **验收标准**：
  1. `DbImpl` 被 `DB` 与 `Iterator` 用 `shared_ptr` 共享，所以 DB 包装对象先销毁时底层 `leveldb::DB` 仍活着——这是「迭代器比 DB 包装对象活得久」合法的原因。
  2. 显式 `Close()` 把 `db` 原子指针置空，迭代器析构看到 `LoadDb()==nullptr` 就跳过 `delete`，避免访问已释放的 LevelDB 内部结构；代价是迭代器对象本身泄漏。
  3. 跳过 delete 只保证「不二次释放」，**不保证** `Key()/Value()/Valid()` 还能用：它们解引用已经释放的 memtable/SSTable。正确契约仍是「先销毁迭代器，再 Close」。
  4. `DB::Close()` 之后再 `Open()` 同一个 path 不会复活旧迭代器；`LastError`/统计仍可读（`impl_` 还活着）。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_leveldb_kv_test` → `[1/3]` 的 `TestIteratorLifetime` 通过；它故意在 `Close()` 前 `iterator.reset()`。把顺序颠倒后用 ASAN 构建跑同一测试，会看到 `Key()` 路径的 heap-use-after-free（构建需 `-DGO2CPP_WITH_LEVELDB=ON`）。
- **常见错误答案**：「包装层会阻止 Close，所以迭代器永远安全」「析构直接 delete 也没事，LevelDB 内部有引用计数」「Close 后迭代器返回空，不会读已释放内存」——零分。

### Q04 【L4 推演】Queue 的游标为什么不是每条 Ack 都落盘？崩溃后到底会重复多少条？

- **痛点**：把 at-least-once 当成「最多丢一条」或者「exactly-once」。实际语义是：Consume 不推进游标，Ack 才推进，而游标每 `cursor_flush_interval` 次 Ack 才批量写一次 KV；崩溃后从最后一次落盘的游标重放，最多重复一整个批量窗口，但不会丢。
- **追查位置**：
  - `include/go2cpp/storage/queue.hpp:14`（语义注释：Consume 只读、Ack 才推进；批量落盘窗口内最多重复 N 条）
  - `include/go2cpp/storage/queue.hpp:46`（`cursor_flush_interval = 4096`，0 表示只能显式 `FlushCursors`）
  - `src/storage/queue.cpp:207`（`Ack` 只更新内存游标并 `MarkDirty`）
  - `src/storage/queue.cpp:223`（`need_flush = BumpAckLocked()`，只有累计到阈值才批量写）
  - `src/storage/queue.cpp:227`（`FlushCursors()` 把多个游标合成一个 `WriteBatch`）
  - `tests/storage/queue_test.cpp:412`（`TestCrashRecovery`：fork+SIGKILL 后验证续写 offset 与游标）
- **验收标准**：
  1. `Consume` 可以重复读同一条（offset 是 topic 内连续序号，Ack 才推进）；重复读不是 bug，是语义。
  2. `Ack` 的推进是「单调取 max」：`offset + 1 > cur` 才更新，乱序/重复 Ack 不会把游标拉回。
  3. 批量落盘意味着进程崩溃时最多重放 `cursor_flush_interval` 条（当前代码默认 4096，但以你读到的常量为准，别背数字）；`FlushCursors()` 可显式收窄窗口。
  4. `sync_on_append=false`（默认）时 Append 本身也可能丢最后几条未 `fdatasync` 的数据；「不丢」只覆盖已确认并已 sync 的部分。
  5. 冷读路径（`retain` 超限/重启后）从日志扫描而不是内存 deque，`cold_reads`/`cold_scanned` 统计会增长。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_queue_test --n=100000` → 输出 `PASS: N checks, 0 failures`，其中崩溃恢复用例打印 `Head/游标/续写 offset`；把 `cursor_flush_interval` 调大再跑同一测试，观察重启后重复条数随窗口变化（这是 e2e 语义验证，不是背数字）。
- **常见错误答案**：「Ack 后立刻持久化，所以重启不重复」「游标存在内存里，崩溃会丢消息」「at-least-once 等于 exactly-once，重复是 bug」——零分。

---

## 三、后端取舍

### Q05 【L4 推演】「项目已经弃用自研 LSM、切到 LevelDB」这句话错在哪？给出三条可复核证据。

- **痛点**：把「LevelDB 集成 + 1e8 RSS 有界」误读成「生产默认后端已切换」。真实状态是：自研 KV 仍是默认路径（`GO2CPP_WITH_KV=ON`），LevelDB 是 opt-in 对照后端（`GO2CPP_WITH_LEVELDB=OFF`），生产 `src/` 里没有任何 `ldb::` 调用方。
- **追查位置**：
  - `cmake/Go2CppLevelDb.cmake:19`（`option(GO2CPP_WITH_LEVELDB ... OFF)`）
  - `cmake/Go2CppLevelDb.cmake:21`（OFF 时打印 `LevelDB storage: OFF` 并 `return()`，连包装层都不编译）
  - `cmake/Go2CppKv.cmake:14`（`option(GO2CPP_WITH_KV ... ON)`，默认构建的仍是自研引擎）
  - `include/go2cpp/storage/leveldb_kv.hpp:4`（「opt-in 对照后端，不是生产默认路径」）
  - `include/go2cpp/storage/leveldb_kv.hpp:6`（「当前仓库里唯一使用方是 tests/storage/test_leveldb_kv.cpp」）
  - `include/go2cpp/storage/leveldb_kv.hpp:12`（RSS 有界是**该后端自身**的实测结论，不代表生产已切换）
- **验收标准**：
  1. 默认 `cmake` 配置下 LevelDB 目标不存在；必须显式 `-DGO2CPP_WITH_LEVELDB=ON` 才生成 `go2cpp_leveldb_kv_test`。
  2. 自研 KV 是默认构建的一部分（`go2cpp_kv` 库 + `go2cpp_kv_test` 等），功能层 Queue/Watch/MemCache 也建在它之上。
  3. `grep -rn "ldb::" src/ include/` 除 `storage/leveldb_kv.*` 自身外没有命中——没有生产代码在运行时切换后端。
  4. 两套实现能在同一个 TU 共存、可逐操作对拍；LevelDB 的 `Stats` 与自研同名统计语义不同（例如 `memtable_bytes` 取 `approximate-memory-usage`，`wal_*` 恒 0），不能直接相加比较。
  5. 保留 LevelDB 的理由是「自研 KV 在 1e8 级数据下 RSS 不可控」这个**已知短板**，属于对照/兜底，不是迁移。
- **怎么自证**：
  - `grep -rn "GO2CPP_WITH_LEVELDB" cmake/ CMakeLists.txt` → 只看到 `option(... OFF)` 与 OFF 分支；
  - `grep -rn "ldb::" src/ include/ | grep -v storage/leveldb_kv` → 空；
  - `cmake -S . -B /tmp/g2c-off -DGO2CPP_WITH_LEVELDB=OFF 2>&1 | grep -i leveldb` → 打印 `LevelDB storage: OFF`；
  - `cd .build-bento-wsl && ./go2cpp_leveldb_kv_test --mem --n=1000000`（需构建时 ON）观察它自己打印的 RSS 断言，而不是把它当生产结论。
- **常见错误答案**：「LevelDB 更快所以已经替换了自研 KV」「`GO2CPP_WITH_LEVELDB` 默认 ON」「两套 Stats 字段同名所以可以直接对比」——零分。

---

## 命题备注（给自己人看）

- 本卷 5 题全部指向可复核的源码/测试；Q01 的「部分执行」与 Q02 的「批量非原子」现有测试**没有正面覆盖**，需要自写复现，属于只能半自证的两题。
- Q03/Q04/Q05 都能用仓库里的二进制或 `grep` 直接观察；Q04 的数字（4096）只是当前代码常量，不是性能结论。
