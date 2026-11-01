# storage（SQLite 嵌入式存储层）

> 位置：`include/go2cpp/storage/**`、`src/storage/**`、`tests/storage/**`
> 构建开关：`-DGO2CPP_WITH_SQLITE=ON`（默认 OFF，不影响既有构建）
> 依赖：系统 `libsqlite3`（Ubuntu 24.04 实测 3.45.1，`pkg-config` 可发现）
> 测试：`ctest -R go2cpp_storage`（1 个用例，约 1.3–3.2 s）
> 基准：`go2cpp_storage_bench [dir] [rows]`（封装 vs 原生 sqlite3）

---

## 1. 定位与边界

`storage` 是**可选模块**，给 Go2Cpp 提供一个 Go `database/sql` 形状的 SQLite
访问层：SQL 文本由调用方书写，封装只负责

1. 资源生命周期（连接、语句、结果集、事务）；
2. 参数绑定与列读取的类型安全（`Value`/`Blob`）；
3. 错误翻译（primary/extended 错误码 + 文本，`Error`）；
4. 事务语义（`Begin`/`Commit`/`Rollback` + RAII `Transaction`）；
5. 并发约定（`DB` 内部串行化，`Stmt`/`Rows` 单线程使用）。

**不做**：ORM、SQL 生成器、连接池、异步 IO、ORM 级迁移。连接池/重试留给上层，
本模块只保证单连接的正确性、错误可判定与性能接近原生 sqlite3。

与 Go 的对应关系：

| Go `database/sql` | 本模块 | 说明 |
|---|---|---|
| `sql.Open` | `DB::Open(path)` → `Result<std::unique_ptr<DB>>` | `:memory:` 支持 |
| `db.Close` | `DB::Close()` | 幂等；`sqlite3_close_v2` 延迟关闭 |
| `db.Exec` | `DB::Exec(sql, args...)` | 无参时可多语句脚本 |
| `db.Query` | `DB::Query(sql, args...)` → `Rows` | 单语句 |
| `db.Prepare` | `DB::Prepare(sql)` → `Stmt` | 预编译复用 |
| `db.Begin/Commit/Rollback` | 同名方法；另有 `Transaction` RAII | `BEGIN IMMEDIATE` |
| `*sql.Stmt` | `Stmt::Bind/Step/Reset/Column*` | move-only、RAII |
| `*sql.Rows` | `Rows::Next/Err/Column*` | `while (rows.Next())` |
| `error` | `Error` | primary code 归一化 + extended code |
| `driver.Value` | `Value` | NULL/int64/double/text/blob/bool |

---

## 2. 文件清单

| 文件 | 职责 |
|---|---|
| `include/go2cpp/storage/db.hpp` | 公共 API：`Error`/`Value`/`Blob`/`Result<T>`/`Stmt`/`Rows`/`DB`/`Transaction` |
| `src/storage/db.cpp` | 实现：错误翻译、绑定、语句生命周期、事务、PRAGMA |
| `tests/storage/test_db.cpp` | 7 组测试（CRUD/事务/预编译/并发/持久化/边界/类型） |
| `tests/storage/bench_db.cpp` | 封装 vs 原生基准 + 逐字节一致验证 |
| `cmake/Go2CppStorage.cmake` | 默认 OFF 的构建入口、sqlite3 发现、测试/基准目标 |
| 顶层 `CMakeLists.txt` | 仅新增 1 行 `include(cmake/Go2CppStorage.cmake)` |

公共头**只前置声明** `sqlite3`/`sqlite3_stmt`，不包含 `<sqlite3.h>`；使用方需要
原生句柄时可以自己包含 `<sqlite3.h>` 后调用 `DB::Handle()` / `Stmt::Handle()`。
sqlite3 的链接与 include 路径是 `go2cpp_storage` 的 PRIVATE 依赖。

---

## 3. 快速上手

```cpp
#include "go2cpp/storage/db.hpp"

using namespace go2cpp::storage;

auto opened = DB::Open("/tmp/app.db");
if (!opened) { std::fprintf(stderr, "%s\n", opened.Err().ToString().c_str()); }
std::unique_ptr<DB> db = std::move(opened.Value());

// WAL + 并发写常见配置。
db->SetJournalMode("WAL");
db->SetSynchronous("NORMAL");
db->SetBusyTimeout(5000);

db->Exec("CREATE TABLE IF NOT EXISTS kv(k TEXT PRIMARY KEY, v TEXT)");

// 参数绑定（全部走 sqlite3_bind_* + SQLITE_TRANSIENT，临时值安全）。
db->Exec("INSERT INTO kv(k, v) VALUES(?, ?)", "answer", "42");

// 查询。
auto rows = db->Query("SELECT k, v FROM kv WHERE k = ?", "answer");
while (rows.Value().Next()) {
    std::string k = rows.Value().Text(0);
    std::string v = rows.Value().Text(1);
}

// 事务：错误路径自动回滚。
Transaction tx(*db);
if (auto err = tx.Begin()) { /* ... */ }
for (int i = 0; i < 1000; ++i) {
    db->Exec("INSERT INTO kv(k, v) VALUES(?, ?)",
             "k" + std::to_string(i), std::to_string(i));
}
tx.Commit();
```

预编译复用（批量写入的唯一正确姿势）：

```cpp
db->Begin();
auto stmt = db->Prepare("INSERT INTO t(id, v) VALUES(?, ?)");
for (int i = 0; i < n; ++i) {
    stmt.Value().Reset();
    stmt.Value().ClearBindings();   // 可选；Bind 会全量覆盖
    stmt.Value().Bind(i, i * 2);
    stmt.Value().Step();
}
db->Commit();
```

**KV 表模拟**：LevelDB 的 `Put/Get/Delete` 用上面 `kv(k TEXT PRIMARY KEY, v BLOB)`
即可；本机没有 LevelDB 的 dev 库（只有 SQLite），因此本轮不引入 LevelDB。

---

## 4. 并发模型

| 对象 | 线程安全 | 说明 |
|---|---|---|
| `DB` | 是（内部 `std::mutex`） | 公开方法串行化；`Handle()` 返回裸指针不持锁 |
| `Stmt` | 否 | 同一语句不得并发 `Step`；跨线程复制/移动由调用方保证 |
| `Rows` | 否 | 持有 `Stmt`，单线程遍历 |
| `Transaction` | 否 | 与所属 `DB` 同线程使用 |

多线程写推荐：**每线程一个 `DB` + WAL + `busy_timeout`**。SQLite 在 WAL 下允许
「多读一写」：读者看到一致快照，写者之间靠 `SQLITE_BUSY`/忙等排队；本封装把
`busy_timeout` 默认设为 5 s，并提供 `SetBusyTimeout(ms)`。

事务统一用 `BEGIN IMMEDIATE`：读事务中途升级写会返回 BUSY，把写锁提前到
`BEGIN` 时获取，错误位置更靠前、更容易重试。

---

## 5. 构建

```bash
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_SQLITE=ON
cmake --build .build-bento-wsl --target go2cpp_storage go2cpp_storage_db_test -j
ctest --test-dir .build-bento-wsl -R go2cpp_storage --output-on-failure
```

- **默认 OFF**：`cmake -S . -B build` 只打印一行 `SQLite storage: OFF`，
  `build.ninja` 里除 `cmake/Go2CppStorage.cmake: phony` 的配置依赖外没有任何
  storage 编译/链接目标（已验证）。
- `GO2CPP_STORAGE_TESTS`（默认 ON）独立于顶层 `GO2CPP_BUILD_TESTS`，因此
  standalone 构建也能只构建存储层与测试。
- `-Werror` + `-Wall -Wextra -Wpedantic`；`-fno-stack-protector` /
  `-fno-semantic-interposition` 与项目模块库的热路径选项一致（共享库构建再加
  `-Bsymbolic-functions`）。
- `go2cpp_storage_bench` 不注册 ctest（基准不是回归测试），按需运行。

---

## 6. 测试覆盖

`tests/storage/test_db.cpp`（`go2cpp_storage_db_test`）一次完整运行 0 失败：

| 测试 | 内容 | 实测耗时 |
|---|---|---|
| `open/pragmas/fk` | 打开/关闭、WAL 回报、外键约束、非法 PRAGMA 参数 | 11–104 ms |
| `transactions` | 提交/回滚、嵌套拒绝、无事务 COMMIT、RAII 自动回滚、约束错误后回滚 | 7–57 ms |
| `values/columns` | 5 种列类型、列名、bool、NULL、`Rows::Close`、视图/拷贝、重复 `Next` | <1 ms |
| `edges` | 空表、空 SQL/纯注释、非法 SQL、参数个数 0/少/多、NULL、int64 边界、1 MiB 文本/blob、UNIQUE/NOT NULL、多语句、并发写锁 BUSY、关闭后调用、CANTOPEN | 31–133 ms |
| `prepared-reuse-1e6` | 同一 `Stmt` 1e6 次 Bind/Step/Reset（含 ClearBindings） | 0.35–1.34 s（0.34–1.33 µs/op） |
| `persistence` | 关闭重开，`DumpTable` 26890 字节 `memcmp` 相等；普通（非 WAL）库同样验证 | 22–67 ms |
| `crud-1e6` | 1e6 插入 / 全表扫描 / 1e6 UPDATE / 1e6 DELETE | 0.79–3.06 s（insert 435–2188，read 87–94，update 142–662，delete 73–75 ms） |
| `concurrency-wal` | 4 写 × 25000 + 2 读者 ×200，WAL，`PRAGMA integrity_check` = ok | 92–309 ms |

> 采样期间机器上还有其它 agent 的高 CPU 任务，单次耗时随负载波动 3–4 倍；
> 断言全部是确定性的（行数/校验和/字节比较），不受耗时波动影响。

`crud-1e6` 吞吐：插入 **1.13–2.30 M 行/s**、扫描 **10.6–11.5 M 行/s**
（同一测试的两次采样，机器负载不同）。

边界细节：

- 参数个数严格相等：少绑/多绑/未绑都返回 `Code::kMisuse`，不做“缺省补 NULL”；
- `Exec` 带参数时拒绝多语句（脚本只允许无参）；`Query`/`Prepare` 只接受单条；
- `:memory:` 请求 `journal_mode=WAL` 返回错误并保留 `memory` 实际值；
- 并发写锁用 `busy_timeout=0` 验证 `Error::IsBusy()` 与 `Code::kBusy`；
- 关闭后 `Exec/Query/Begin/Prepare` 返回 `kMisuse` 而不是崩溃；`Handle()` 为 null；
- 1 MiB 文本含内嵌 `\0`、1 MiB blob 逐字节往返。

---

## 7. 性能（封装 vs 原生 sqlite3）

环境：WSL2 / Ubuntu 24.04 / g++ 13.3 `-O3` / SQLite 3.45.1 / 12 vCPU；
运行用 `taskset -c 0`，目录在 `/tmp`（ext4）。**注意**：采样期间机器上还有
其它 agent 的高 CPU 任务（`load average` 5–12），单次结果波动大；下表是
**5 次运行的中位数**，并给出波动区间。

### 7.1 1e6 行（5 次中位数）

| 场景 | 封装 | 原生 | 封装/原生 |
|---|---|---|---|
| 插入 1e6（文件，WAL，单事务） | 664.9 ms（1.50 M 行/s） | 587.7 ms（1.70 M 行/s） | **1.13x** |
| 插入 1e6（内存库） | 382.5 ms（2.61 M 行/s） | 322.6 ms（3.10 M 行/s） | **1.19x** |
| 扫描 1e6（文件） | 90.1 ms（11.1 M 行/s） | 93.6 ms（10.7 M 行/s） | **0.96x** |
| 预编译复用 1e6（内存） | 323.7 ms（3.09 M 行/s） | 308.8 ms（3.24 M 行/s） | **1.05x** |
| 微基准 1e7 次 `SELECT ?` | 772.5 ms（77.2 ns/op） | 745.9 ms（74.6 ns/op） | **1.04x** |

单次波动区间：插入 0.90–1.35x、扫描 0.90–1.09x、微基准 0.92–1.19x。

### 7.2 1e7 行压测（单次，同环境）

| 场景 | 封装 | 原生 | 封装/原生 |
|---|---|---|---|
| 插入 1e7（文件，WAL，单事务） | 8726 ms（1.15 M 行/s） | 7098 ms（1.41 M 行/s） | 1.23x |
| 插入 1e7（内存库） | 5649 ms（1.77 M 行/s） | 4628 ms（2.16 M 行/s） | 1.22x |
| 扫描 1e7（文件） | 948 ms（10.5 M 行/s） | 827 ms（12.1 M 行/s） | 1.15x |
| 预编译复用 1e7（内存） | 3081 ms（3.25 M 行/s） | 2467 ms（4.05 M 行/s） | 1.25x |
| 微基准 1e7 次 `SELECT ?` | 600.0 ms（60.0 ns/op） | 549.4 ms（54.9 ns/op） | 1.09x |

1e7 与 1e6 的吞吐一致（插入 ~500–900 ns/行、扫描 ~85–95 ns/行），没有随规模
退化；`sum` 校验和与 1e6 组一致（分别为 `99999990000000` /
`249999975000000` / `50000005000000`）。

### 7.3 优化前/后（同一台机器）

| 指标 | 优化前 | +缓存参数/列数 | +热路径编译选项（当前） |
|---|---|---|---|
| 微基准 1e7 封装/原生 | 1.52x（189.9 vs 124.8 ns） | 1.21x（111.6 vs 73.9 ns） | **1.04x**（77.2 vs 74.6 ns） |
| 预编译 1e6 封装/原生 | 1.17x | 1.02x | 1.05x |
| 插入 1e6 文件 | 1.13x | 1.03x | 1.13x（噪声内） |
| 扫描 1e6 文件 | 1.09x | 1.09x | 0.96x（噪声内） |

两项优化：`Stmt` 构造时缓存 `parameter_count`/`column_count`（去掉每次
`Bind`/`Column*` 的 `sqlite3_bind_parameter_count`/`sqlite3_column_count` 调用）；
`go2cpp_storage` 加 `-fno-stack-protector`（`Step`/`BindValues` 原本每行一次
canary）与 `-fno-semantic-interposition`。

---

## 8. 离下限几倍

三个参照系，结论都是「封装不是瓶颈」：

1. **任务给定 SQLite WAL 插入下限 ≈ 1e6 行/s**
   实测封装 **1.15–1.50 M 行/s**（1e7 / 1e6 组），是下限的 **1.15–1.50x**；
   原生 1.41–1.70 M 行/s。封装与下限量级相同，没有把引擎拖慢一个数量级。

2. **原生 sqlite3 同配置（封装可达基线）**
   插入 1.13–1.25x、扫描 0.96–1.15x、预编译 1.05–1.25x、微基准 1.04–1.09x。
   即封装开销 **0–25%**，中位约 **10%**；批量插入每行 SQLite 本体 500–900 ns，
   封装只占其中几十纳秒。

3. **指令数（callgrind，2000 行单事务插入）**
   每行约 **3429 Ir**：`Stmt::Step` 链 2784（VDBE/Btree 96%）+ `BindValues` 444
   + `Reset` 201。其中封装层自身（exclusive）仅 **136 Ir/行 = 4.0%**；
   SQLite 内核约 3293 Ir/行 = 96%。

与「顺序写带宽」这个物理上限相比，SQLite 每行约 500–900 ns（30 MB / 1e6 行，
10 GB/s memcpy 下限约 3 ns/行）——差约 **200x**，这是 btree + WAL 页写 +
事务日志的引擎代价，不是封装引入的；封装层只贡献 4% 指令预算。

---

## 9. objdump 证据

对链接后的 `go2cpp_storage_db_test` 反汇编（`objdump -d -C --no-show-raw-insn`，
`-O3`），统计函数体静态指令数与冒烟级特征：

| 函数 | 指令数 | call 数 | 栈保护 | mutex | 热路径 |
|---|---:|---:|---:|---:|---|
| `Stmt::BindValues` | 494 | 15 | 0 | 0 | 每参数 1 次 `sqlite3_bind_*@plt` |
| `Stmt::Step` | 389 | 9 | 0 | 0 | 1 次 `sqlite3_step@plt` |
| `Stmt::Reset` | 104 | 2 | 0 | 0 | 1 次 `sqlite3_reset@plt` |
| `Stmt::ColumnInt64` | 16 | 0 | 0 | 0 | 无调用（内联坐标计算） |
| `Rows::Next` | 52 | 3 | 0 | 0 | 调 `Stmt::Step`（无错误构造） |
| `DB::ExecImpl` | 853 | 35 | 0 | **2** | `pthread_mutex_lock/unlock` 各 1 |
| `DB::QueryImpl` | 1176 | 34 | 0 | **2** | 同上 |
| `DB::Prepare` | 999 | 28 | 0 | **2** | 同上 |
| `DB::BindLocked` | 103 | 9 | 0 | 0 | 每参数 1 次 bind |

说明：

- 指令数含错误分支/冷代码，热路径约占 20–30%（例如 `Step` 热路径是 1 次
  `sqlite3_step` + 两次 `cmp`/`sete`，其余是 `Error` 构造）；
- 全部函数 `stack_chk=0`（`-fno-stack-protector` 生效；优化前 `Step`/`BindValues`
  每行一次 canary）；
- `DB` 公开方法恰好 1 次 lock + 1 次 unlock，无嵌套/无递归锁；`Stmt`/`Rows`
  热路径 0 mutex（单线程对象）；
- 热路径无堆分配：`Value` 用栈上 `std::array`，短文本 SSO；只有错误路径构造
  `std::string`。

复现：

```bash
objdump -d -C --no-show-raw-insn .build-bento-wsl/go2cpp_storage_db_test \
  | grep -A20 '<go2cpp::storage::Stmt::Step()>:'
```

---

## 10. 逐字节一致与持久化

1. **封装与原生写出的数据库文件完全相同**
   `bench_db.cpp` 用相同 schema、相同 1000 行、相同 `journal_mode=WAL` /
   `synchronous=NORMAL`、相同 `wal_checkpoint(TRUNCATE)`，分别用封装与原生
   写入；两个文件均 **28672 字节**，`sha256` 均为
   `50450b0206a6ece0bc34d90aad41a4d84fa5693c83a95ff3e5cf536764c460ee`。
   两次独立运行（不同目录）的四个文件 `sha256` 全部相同 → 写入路径确定性。
2. **内容层逐字节一致**：按 `id` 序列化 `id/v/name` 的 28000 字节 dump，
   FNV-1a64 均为 `08493cf6b68e2cd3`。
3. **持久化**：`test_persistence` 写入 1000 行 → `DumpTable` 得 26890 字节 →
   `Close` → 重新 `Open` → 再次 dump，`size` 与 `memcmp` 均相等；普通
   （非 WAL）库同样验证。
4. **并发后完整性**：4 写 + 2 读结束后 `PRAGMA integrity_check` 返回 `ok`，
   `COUNT(*) = 100000`，`SUM(v)` 与理论值 `n(n-1)/2` 相等。

---

## 11. 边界与陷阱

- **文本生命周期**：`ColumnTextView` 是零拷贝，仅在该行下一次 `Next()` 或
  语句 `Reset`/`Finalize` 前有效；需要保留用 `ColumnText`（拷贝）。
- **`Exec` 多语句**：无参时按脚本逐条执行，`changes` 逐条累加；带参数只允许
  一条，否则返回 `kMisuse`（避免参数归属歧义）。
- **空输入**：`Exec("")`/纯注释成功且无副作用；`Query("")` 返回 `kMisuse`。
- **错误码**：`Error::Code()` 已归一化为 primary code（如 19），
  `ExtendedCode()` 保留 787/2067/1299 等扩展码；`IsBusy()`/`IsConstraint()`
  同时看两者。
- **关闭语义**：`Close` 幂等；`sqlite3_close_v2` 允许仍有语句未 finalize 时
  延迟关闭，之后对语句的操作返回 `kMisuse`。
- **WAL 与 `:memory:`**：内存库不支持 WAL，`SetJournalMode("WAL")` 会比对
  实际返回值并报错，不会静默忽略。
- **绑定个数**：严格匹配，不猜测；用 `Stmt::ParameterCount()` 可在运行时检查。

---

## 12. 后续

- **LevelDB**：本机只有 SQLite 的 dev 库（`sqlite3.h` + `libsqlite3`），没有
  LevelDB 的 `leveldb/db.h`/`libleveldb`。KV 语义可用本模块的
  `kv(k TEXT PRIMARY KEY, v BLOB)` 表模拟（§3）；等 LevelDB dev 库可用后再加
  一个同形状的 `KV` 后端（接口按 `Put/Get/Delete/Scan` 设计）。
- **连接池**：上层可按「每线程一个 `DB`」自建；或后续加 `Pool<DB>`（需先有
  跨线程借用/归还语义与健康检查）。
- **Statement 缓存**：`Exec` 每次重新 prepare（SQLite 无内建语句缓存）；
  高频调用方应使用 `Prepare` 复用。
