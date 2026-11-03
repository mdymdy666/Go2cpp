# 04 storage 答卷（5 题）

> 环境：WSL2 / 12 vCPU。自证用 `.build-bento-wsl` 现成二进制（Release）。
> 所有行号 `sed` 核对；本卷题面行号逐条吻合。

## Q01

**答**：`ExecImpl` 是「循环 `sqlite3_prepare_v2` + `tail` 前进」的多语句执行器，参数校验在循环内逐条做；拒绝条件是 `executed && count != 0`——第一条无参语句会正常 `step` 并 `finalize`，第二条才发现「带参数还多语句」返回 `kMisuse`。`Exec` 内部**不开事务**，所以第一条已生效、不回滚；要原子性必须自己 `Begin/Commit` 且改成单条。`Query`/`Prepare` 策略相反：`TailIsOnlyWhitespace(tail)` 直接拒绝多语句。测试只断言错误码，不检查表里是否多了一行。

**代码证据**：
```text
src/storage/db.cpp:587-589  Result<ExecResult> DB::ExecImpl(...) { std::lock_guard<std::mutex> lock(m_mutex);
src/storage/db.cpp:612  if (executed && count != 0) { (void)sqlite3_finalize(stmt); ...
src/storage/db.cpp:633-635  (void)sqlite3_finalize(stmt); executed = true; cursor = tail;
src/storage/db.cpp:662  if (!TailIsOnlyWhitespace(tail)) { ... kMisuse
include/go2cpp/storage/db.hpp:45-46  // 带参数时只允许单条语句，避免参数归属歧义。
```

**自证**：`.build-bento-wsl/go2cpp_storage_db_test` → `=== PASS (0 failures) ===`（prepared 0.286 µs/op、1e6 insert 2.72M rows/s、read 12.2M rows/s、WAL 4 写×25k 通过）。该测试**不覆盖**「部分执行」副作用。

**我没答出的**：没有写 `Exec("INSERT ...; INSERT ...", 5)` + `SELECT COUNT(*)` 的复现（题面也标注现有测试不覆盖）。

## Q02

**答**：声明期错误只置 `m_build_error`，不抛异常：`Add()` 版本号非递增（714-716）、`AddColumn()` 在 SQLite <3.35 生成不出 down SQL（753-758）都只记录。`UpToLocked` 第一件事就是 `if (m_build_error) return m_build_error;`，所以**整对象毒化**——哪怕只想跑一条无关迁移也失败。而 `Up()` 是逐条提交：每条迁移内部各自 `BEGIN IMMEDIATE`/`COMMIT`（818），批量循环逐条 `ApplyLocked`（872-874），第 N 条失败时 1..N-1 已提交、版本表停 N-1。`BEGIN IMMEDIATE` 还意味着调用方不能把 `Up()` 包进外层事务（SQLite 不允许嵌套）。已应用但未声明的版本会让 `Down()` 报错且已回滚步骤不恢复。

**代码证据**：
```text
src/storage/orm.cpp:714-716  if (!m_steps.empty() && version <= m_steps.back().version) { // 错误留到 Up()/Down()
src/storage/orm.cpp:753-758  if (sqlite3_libversion_number() < 3035000) return {}; ... if (down.empty()) { if (!m_build_error) ...
src/storage/orm.cpp:850-851  if (m_build_error) { return m_build_error; }
src/storage/orm.cpp:818  if (auto begun = m_db->Exec("BEGIN IMMEDIATE"); !begun) { ...
include/go2cpp/storage/orm.hpp:1440-1442  // 每条迁移在独立事务中执行...任一步失败回滚该条迁移...SQLite 的 DDL 是事务性的
```

**自证**：`.build-bento-wsl/go2cpp_storage_orm_test` → `all checks passed (2568 ms)`（1e6 insert 791 ms、update 1113 ms、delete 520 ms）。测试只覆盖「单条迁移内部撤销」，不覆盖「11 成功、12 失败」。

**我没答出的**：没有加版本 11/12 验证 `CurrentVersion()` 停在 11；也没有确认本机 libsqlite 版本（CLI 未装）。

## Q03

**答**：`DbImpl` 被 `DB` 与 `Iterator` 用 `shared_ptr` 共享，所以「迭代器比 DB 包装对象活得久」本身合法。但显式 `Close()` 把 `db` 原子指针置空并释放 LevelDB 内部结构后，迭代器析构看到 `LoadDb()==nullptr` 就跳过 `delete`——只避免「二次释放 `leveldb::Iterator` 对象」，代价是迭代器对象本身泄漏。跳过 delete **不保证** `Key()/Value()/Valid()` 还能用：它们解引用已释放的 memtable/SSTable，是 UAF 读。正确契约仍是「先销毁迭代器，再 Close」。

**代码证据**：
```text
src/storage/leveldb_kv.cpp:257-263  Iterator::~Iterator() { ... // 只有底层 DB 仍存活时才析构...delete 会 UAF...选择泄漏一个迭代器对象，而不是崩溃
src/storage/leveldb_kv.cpp:510-512  delete impl_->db.exchange(nullptr, ...); impl_->filter_policy.reset(); impl_->cache.reset();
include/go2cpp/storage/leveldb_kv.hpp:23  // 迭代器必须在 DB 关闭前销毁；key()/value() 只在下一次 Next/Seek 前有效
```

**自证**：`.build-bento-wsl/go2cpp_leveldb_kv_test --mem --n=100000` → 写入 669273 ops/s、RSS 基线 4.1 MiB/峰值 4.1 MiB/VmHWM 9.0 MiB、随机读 100000/100000 命中、`RSS 峰值 < 512 MiB —— PASS`。测试按正确顺序 `iterator.reset()` 再 `Close()`。

**我没答出的**：没有用 ASAN 构建复现「先 Close 再 Key()」的 heap-use-after-free（需要 `-DGO2CPP_WITH_LEVELDB=ON` + ASAN）。

## Q04

**答**：`Consume` 只读取、不推进游标；`Ack` 才推进（单调取 max：`offset+1 > cur` 才更新，乱序/重复 Ack 不回退）。Ack 只更新内存游标并 `MarkDirty`/`BumpAckLocked`，每累计 `cursor_flush_interval`（当前 4096，以代码常量为准）次才把游标批量写一次 KV；`FlushCursors()` 可显式收窄窗口。崩溃后从最后一次落盘的游标重放，**最多重复一整个批量窗口、不会丢**（at-least-once）。另外 `sync_on_append=false`（默认）时 Append 本身也可能丢最后几条未 `fdatasync` 的数据——「不丢」只覆盖已确认并已 sync 的部分。

**代码证据**：
```text
include/go2cpp/storage/queue.hpp:14-15  // 语义：at-least-once。Consume 只读取、不推进游标；Ack 才推进。崩溃或进程重启后游标不回退（批量落盘窗口内最多重复投递 N 条，不会丢）
include/go2cpp/storage/queue.hpp:45-46  // 每 N 次 Ack 把游标批量写一次 KV（0 = 不自动写，只能显式 FlushCursors）。cursor_flush_interval = 4096;
src/storage/queue.cpp:207-209  bool Ack(...) { if (!opened_) ...
src/storage/queue.cpp:223  need_flush = BumpAckLocked();
src/storage/queue.cpp:227  if (need_flush) { FlushCursors(); }
```

**自证**：`.build-bento-wsl/go2cpp_queue_test --n=20000` → `PASS: 59209 checks, 0 failures`（append/consume+ack 各 4.26M/s，含 fork+SIGKILL 崩溃恢复场景）。

**我没答出的**：没有把 `cursor_flush_interval` 调大后量化「重启重复条数随窗口变化」。

## Q05

**答**：「已弃用自研 LSM、切到 LevelDB」是错误前提。三条可复核证据：① `cmake/Go2CppKv.cmake:13` `option(GO2CPP_WITH_KV ... ON)`——自研 KV 是默认构建；② `cmake/Go2CppLevelDb.cmake:19` `option(GO2CPP_WITH_LEVELDB ... OFF)`，21 行 OFF 时 `return()`，连包装层都不编译；③ `include/go2cpp/storage/leveldb_kv.hpp:4-6` 明确定位「opt-in 对照后端，不是生产默认路径……当前仓库里唯一使用方是 tests/storage/test_leveldb_kv.cpp」。另外 `grep -rn "ldb::" src/ include/ | grep -v storage/leveldb_kv` 为空——没有生产代码在运行时切换后端。LevelDB 保留的理由是自研 KV 在 1e8 级数据下 RSS 不可控这个**已知短板**，属于对照/兜底，不是迁移。

**代码证据**：
```text
cmake/Go2CppKv.cmake:13  option(GO2CPP_WITH_KV "Build the embedded KV storage engine" ON)
cmake/Go2CppLevelDb.cmake:19  option(GO2CPP_WITH_LEVELDB "Build the official LevelDB storage backend" OFF)
cmake/Go2CppLevelDb.cmake:21  if (NOT GO2CPP_WITH_LEVELDB)
include/go2cpp/storage/leveldb_kv.hpp:4  // **定位：opt-in 对照后端，不是生产默认路径。**
```

**自证**：`grep -rn "ldb::" src/ include/ | grep -v storage/leveldb_kv` → 空输出；`grep -n "GO2CPP_WITH_LEVELDB" cmake/Go2CppLevelDb.cmake` → 19/21 的 option 与 OFF 分支。

**我没答出的**：没有重新 configure 一个 `-DGO2CPP_WITH_LEVELDB=OFF` 的构建目录打印 `LevelDB storage: OFF`（.build-bento-wsl 已有 leveldb 二进制，说明该目录是 ON 构建）。
