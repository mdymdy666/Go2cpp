# storage ORM 层（表映射 / CRUD / 查询构造 / 迁移）

> 位置：`include/go2cpp/storage/orm.hpp`、`src/storage/orm.cpp`、`tests/storage/orm_test.cpp`、`tests/storage/orm_bench.cpp`
> 构建开关：与 SQLite 层同一个（`-DGO2CPP_WITH_SQLITE=ON`，默认 OFF）
> 依赖：`go2cpp/storage/db.hpp`（SQL 层）+ 系统 `libsqlite3`；可选复用 `go2cpp/reflect`（仅头文件）
> 测试：`ctest -R go2cpp_storage_orm`（1 个用例，约 2.8 s）
> 基准：`go2cpp_storage_orm_bench [rows]`（原生 sqlite3 vs `db.hpp` 层 vs ORM）

---

## 1. 定位与分层

`storage` 目录里有两层，单向依赖：

```
业务结构体 + GO2CPP_ORM_TABLE 映射
        │
storage::Session / Query<T> / Migration      ← 本层（orm.hpp/orm.cpp）
        │
storage::DB / Stmt / Rows / Transaction      ← db.hpp（SQL 层，[storage-sqlite](storage-sqlite.md)）
        │
libsqlite3
```

- **SQL 层**（`db.hpp`）：SQL 文本由调用方书写，只管资源/绑定/错误/事务；
- **ORM 层**（本文件）：把「结构体 ↔ 表」的映射、CRUD、简单查询构造与版本化
  迁移收进一个可选头文件。`db.hpp` 不知道本层存在，不引入反向依赖。

**不做**：关系（has-many/belongs-to）、懒加载、连接池、自动迁移（AutoMigrate
式反射推断 schema）、SQL 表达式树。迁移是**显式声明**的 SQL 步骤，不是从结构
体差异反推。

---

## 2. 文件清单

| 文件 | 职责 |
|---|---|
| `include/go2cpp/storage/orm.hpp` | 公共 API：`Column`/`TableDef`/`Table<T>`、映射宏、`Session`、`Query<T>`、`Migration`、可选反射桥接 |
| `src/storage/orm.cpp` | 实现：SQL 生成、表状态与语句缓存、CRUD 内核、迁移执行 |
| `tests/storage/orm_test.cpp` | 7 组测试（映射/往返/CRUD 1e6/迁移/事务/查询/反射桥接/边界） |
| `tests/storage/orm_bench.cpp` | 原生 vs `db.hpp` 层 vs ORM 的 1e6 插入/查询/更新/删除基准 |
| `cmake/Go2CppStorage.cmake` | 追加 `orm.cpp` 到 `go2cpp_storage`；新增 `go2cpp_storage_orm_test` / `go2cpp_storage_orm_bench` 目标 |

`orm.hpp` 只多引入一个可选依赖：仓库里存在 `go2cpp/reflect/field.hpp` 时启用
`GO2CPP_ORM_TABLE_REFLECT`（只用字段表模板，不链接 `go2cpp_reflect`）。

---

## 3. 快速上手

```cpp
#include "go2cpp/storage/orm.hpp"

struct User {
    std::int64_t id = 0;
    std::string name;
    int age = 0;
    std::optional<std::string> email;
    std::vector<std::uint8_t> avatar;
};

GO2CPP_ORM_TABLE(User, "users",
                 GO2CPP_ORM_COLUMN(User, id).PrimaryKey().AutoIncrement(),
                 GO2CPP_ORM_COLUMN(User, name).NotNull().Unique(),
                 GO2CPP_ORM_COLUMN(User, age).Indexed(),
                 GO2CPP_ORM_COLUMN(User, email),
                 GO2CPP_ORM_COLUMN(User, avatar))

using namespace go2cpp::storage;

auto opened = DB::Open("app.db");
DB& db = *opened.Value();
db.SetJournalMode("WAL");
db.SetSynchronous("NORMAL");

Migration migration(db);
migration.AddCreateTable<User>(1, "create users");   // 建表 + Indexed() 索引
if (Error error = migration.Up()) { /* ... */ }

Session session(db);

User user;
user.name = "alice";
user.age = 30;
auto id = session.Insert(user);        // 回填 user.id
auto found = session.Find<User>(1);    // Result<User>，不存在 → kErrNotFound

auto adults = session.Query<User>()
                  .Where("age > ?", 18)
                  .OrderBy("age DESC")
                  .Limit(10)
                  .All();              // Result<std::vector<User>>

Error committed = session.Transaction([&]() -> Error {
    user.age = 31;
    if (auto updated = session.Update(user); !updated) {
        return updated.Err();          // 返回错误 → 自动回滚
    }
    return {};
});
```

---

## 4. 表映射

### 4.1 显式宏（默认路径）

```cpp
GO2CPP_ORM_TABLE(Class, "table_name", 列声明...)
GO2CPP_ORM_COLUMN(Class, member)      // 返回 Column，可链式修饰
```

链式修饰（都返回副本，便于在静态数组初始化里连续调用）：

| 方法 | 效果 |
|---|---|
| `.PrimaryKey()` | `PRIMARY KEY`（不自动加 NOT NULL） |
| `.AutoIncrement()` | `INTEGER PRIMARY KEY AUTOINCREMENT`（只对整数主键有效） |
| `.NotNull()` | `NOT NULL` |
| `.Unique()` | `UNIQUE`（列级） |
| `.Indexed()` | 建表时额外生成 `CREATE INDEX` |
| `.Default(String sql)` | `DEFAULT <sql>` 原样片段 |
| `.Named(String)` | 覆盖列名（默认等于字段名） |

支持的字段类型（`ColumnTypeOf<T>()` 编译期推导）：

| C++ 类型 | 列类型 | 绑定 |
|---|---|---|
| `bool` | INTEGER | 0/1 |
| 整数（含 `int8_t`…`int64_t`、`char`） | INTEGER | int64 |
| 枚举 | INTEGER | 底层值 |
| `float`/`double` | REAL | double |
| `std::string` | TEXT | `SQLITE_TRANSIENT` 拷贝 |
| `std::vector<std::uint8_t>` | BLOB | 非拥有视图，绑定时拷贝 |
| 以上类型的 `std::optional<T>` | 同 T | 无值 → SQL NULL |

**为什么默认用宏**：SQL 列类型必须与字段的 C++ 类型精确对应。宏在展开点就知道
字段静态类型，`bind`/`read` 由 `if constexpr` 直接生成，热路径没有类型分派、
没有 `memcpy` 宽度判断；映射错误在编译期（`static_assert`）或首次准备表时
（`kErrMapping`）暴露。

### 4.2 反射桥接（可选）

已用 `GO2CPP_REFLECT_FIELD` 声明过字段表的结构体可以零改动映射：

```cpp
GO2CPP_ORM_TABLE_REFLECT(User, "users", "id")   // 第三个参数：主键字段名
```

- 字段顺序即列顺序；主键列若为整数自动 `AUTOINCREMENT`；
- 只接受可精确还原的类型：`bool` / 1/2/4/8 字节整型 / `double` /
  `std::string` / `std::vector<std::uint8_t>`；
- 其它类型（`float`、嵌套结构体、`optional`）会让**整表映射为空**，首次 CRUD
  返回 `kErrMapping`（不静默丢列）；
- `uint64` 超过 `INT64_MAX` 的部分 SQLite INTEGER 无法表示（按位回绕）。

反射桥接用 `memcpy` 读写（不知道字段的静态类型，用 `TypeId × size` 分派），
比宏路径多一次分支，属于「便捷入口」而非热路径首选。

### 4.3 主键约束

- CRUD 要求**有且仅有一个** `INTEGER` 主键；复合主键/文本主键返回
  `kErrMapping`（`Query<T>` 仍可用）；
- `AutoIncrement` 只对整数主键有效；
- `Insert(T&)` 成功后回填主键字段；`Insert(const T&)` 不回填。

---

## 5. CRUD 与语句缓存

`Session` 持有三类缓存（构造时绑定一个 `DB`，不拥有它）：

1. **表状态**（每表一次）：SQL 文本、参与 INSERT/UPDATE 的列下标、主键位置、
   复用的 `std::vector<Value>` 绑定缓冲、4 条预编译语句；
2. **语句缓存**（`Query<T>` 的动态 SQL）：`unordered_map<string, Stmt>`，上限
   `kMaxCachedStatements = 128`，满了整体清空（不做 LRU）；
3. **最近表的快速路径**：单表热路径免哈希查找。

热路径关键决策：

- **CRUD 语句内嵌在表状态里**，不按 SQL 文本做哈希查找；
- **绑定缓冲复用**：每行只写 `value`，不重新构造 `Value` 数组；
- **事务内零会话锁**：`Transaction(fn)` 持锁，同线程 `body` 内通过 thread-local
  标记跳过加锁（不重入锁，也不使用 `recursive_mutex`）；
- **原生句柄缓存**：`sqlite3_last_insert_rowid` / `sqlite3_changes` 直接走缓存的
  `sqlite3*`，不经过 `DB` 的互斥量；
- **`Reset` 不可省**：`sqlite3_bind_*` 在语句 `step` 到 DONE 后返回
  `SQLITE_MISUSE`，每次绑定前必须 `sqlite3_reset`（实测去掉后插入全部失败）。

操作语义：

| API | SQL | 返回 |
|---|---|---|
| `Insert(T&)` | `INSERT INTO t (非自增列) VALUES (?,...)` | `Result<int64_t>`（rowid） |
| `Insert(const T&)` | 同上 | rowid；不回填主键 |
| `Update(const T&)` | `UPDATE t SET 非主键列=? WHERE 主键=?` | `Result<int>`（受影响行数，0 = 不存在） |
| `Delete<T>(id)` | `DELETE FROM t WHERE 主键=?` | `Result<int>`（0 = 不存在，不算错误） |
| `Find<T>(id)` | `SELECT 全部列 FROM t WHERE 主键=?` | `Result<T>`；不存在 → `kErrNotFound` |
| `All<T>()` | `SELECT 全部列 FROM t` | `Result<std::vector<T>>` |
| `Count<T>()` | `SELECT COUNT(*)` | `Result<int64_t>` |

表/列名统一 `QuoteIdentifier` 双引号转义（内部 `"` 翻倍），表名含空格/引号也能
建表与 CRUD（测试覆盖 `quo"ted` 表名与 `te"xt` 列名）。

---

## 6. 查询构造

```cpp
session.Query<User>()
    .Where("age > ?", 18)            // 参数绑定；多次 Where 以 AND 连接
    .Where("name LIKE ?", "a%")
    .OrderByColumn("age", true)      // 自动转义列名 + DESC
    .OrderBy("id")                   // 原样表达式
    .Limit(10)
    .Offset(20)
    .All();
```

| 方法 | 说明 |
|---|---|
| `Where(clause, args...)` | 子句原样拼接（括号包裹），参数按顺序绑定到 `?` |
| `OrderBy(expr)` | 原样表达式（可含多个列/方向） |
| `OrderByColumn(col, desc)` | 列名自动转义 |
| `Limit(n)` / `Offset(n)` | 负数 limit = 不限；只有 offset 时生成 `LIMIT -1 OFFSET n` |
| `All()` / `First()` / `FirstOptional()` | 物化 / 首行（不存在 → `kErrNotFound` / `nullopt`） |
| `Each(fn)` | 逐行回调，`fn` 返回 `bool` 可提前结束；返回遍历行数 |
| `Count()` | 只看 `Where`，忽略 `OrderBy`/`Limit`/`Offset` |
| `Sql()` | 生成的 SELECT 文本（调试） |

**注入边界**：`Where`/`OrderBy` 是调用方写的常量 SQL 片段，**不要拼接用户
输入**；用户数据一律走 `?` 参数（测试用 `a' OR '1'='1` 验证参数化不改变语义）。

---

## 7. 迁移

版本表（默认 `_go2cpp_migrations`）：

```sql
CREATE TABLE IF NOT EXISTS "_go2cpp_migrations" (
    "version"     INTEGER PRIMARY KEY,
    "name"        TEXT    NOT NULL,
    "applied_at"  INTEGER NOT NULL,
    "duration_us" INTEGER NOT NULL DEFAULT 0
)
```

```cpp
Migration migration(db);
migration.AddCreateTable<User>(1, "create users")
         .AddColumn(2, "add score", "users", "score", ColumnType::kReal,
                    /*not_null=*/true, /*default=*/"0")
         .AddCreateIndex(3, "index score", "users_score_idx", "users", {"score"});

Error up = migration.Up();          // 应用全部未应用（按版本升序）
Error to2 = migration.MigrateTo(2); // 升/降到指定版本
Error down = migration.Down(1);     // 回滚最近 1 条
```

- 每条迁移在**独立事务**内执行：`BEGIN IMMEDIATE` → `up_sql` → 写版本行 →
  `COMMIT`；失败回滚并返回带版本号的错误（`migration 2 (broken) up failed: ...`）；
- SQLite 的 DDL 是事务性的，失败不会留下半张表；
- 版本号必须严格递增，否则 `Add` 记录声明期错误，`Up()`/`Down()` 返回
  `kErrMigration`；
- `Down` 要求迁移在本次声明列表里（已应用但未声明 → `kErrMigration`）；
- `AddColumn` 的 `down` 用 `ALTER TABLE ... DROP COLUMN`，需要 SQLite 3.35+
  （Ubuntu 24.04 实测 3.45.1）；低版本在声明期报错而不是运行期炸。

---

## 8. 事务与线程模型

```cpp
Error result = session.Transaction([&]() -> Error {
    // 这里可以继续用同一个 session（同线程零锁）
    ...
    return {};                     // 提交
});
```

- `body` 返回 `Error` 非空 → 回滚并原样返回该错误；返回空 → 提交；
- `body` 抛异常 → `storage::Transaction` 的 RAII 回滚，异常继续向外传播；
- 事务内再次调用 `Transaction` → `kErrTransaction`（不嵌套）；
- `Session` 可跨线程共享（会话锁串行化缓存与语句使用）；但**事务 body 内不要
  把同一个 `Session` 交给其它线程**——那个线程会阻塞在会话锁上直到事务结束；
- 多线程写建议每线程一个 `DB` + WAL（与 SQL 层约定一致）。

---

## 9. 构建

```bash
cmake -S . -B build -DGO2CPP_WITH_SQLITE=ON
cmake --build build -j
ctest --test-dir build -R go2cpp_storage        # db_test + orm_test
./build/go2cpp_storage_orm_bench 1000000        # 基准（不注册 ctest）
```

`Go2CppStorage.cmake` 新增两个目标，均不改变 `GO2CPP_WITH_SQLITE=OFF` 时的
构建面：`go2cpp_storage_orm_test`（ctest 用例）与 `go2cpp_storage_orm_bench`
（按需运行）。ORM 测试额外链接 `go2cpp_reflect`（只有反射桥接用例需要，按
`if (TARGET go2cpp_reflect)` 保护）。

---

## 10. 测试覆盖

`tests/storage/orm_test.cpp`（约 2.8 s，退出码 = 失败断言数）：

| 组 | 内容 |
|---|---|
| 映射 | 列类型/偏移/主键/索引/默认值、`CreateTableSql` 文本、标识符转义、`ColumnTypeOf` 静态断言 |
| 往返 | int/负值/`int64_t`/`double`/`bool`/`optional`（NULL 与空串区分）/BLOB（含 `0x00`/`0xFF`）/UTF-8 中文 |
| CRUD 1e6 | 事务内 1e6 插入（校验 rowid 递增）、抽样 Find、1e6 UPDATE、1e6 DELETE、删除不存在返回 0 |
| 迁移 | 建表/加列/建索引/逐级回滚/幂等/失败事务回滚/版本号非递增/未声明版本拒绝/`MigrateTo`/自定义版本表名 |
| 事务 | 提交、错误回滚、异常回滚、事务内混合读改写后回滚、嵌套拒绝、事务外恢复 |
| 查询 | where/多条件 AND/order 升降序/limit/offset/count/first/first_optional/each 提前结束/参数化注入/空结果/语句缓存清空 |
| 反射桥接 | 10 种字段宽度极值往返、无字段表 → 空映射、建表 SQL、Query/Update/Delete |
| 边界 | 无主键/复合主键/非整数自增、Find 不存在、空表、表名与列名含双引号 |

---

## 11. 性能（1e6 行）

环境：WSL2 / Ubuntu 24.04 / g++ 13.3 `-O3` / SQLite 3.45.1 / 12 vCPU；
`taskset -c 2`，临时目录在 `/tmp`（ext4）。采样时机器上还有其它任务
（`load average` 5–12），下表是**三次运行的中位数**；单次波动约 ±10%。

### 11.1 ORM vs `db.hpp` 封装层 vs 原生 sqlite3

| 场景 | ORM | `db.hpp` 层 | 原生 sqlite3 | ORM/原生 |
|---|---|---|---|---|
| 插入 1e6（文件库，WAL，单事务） | **547.5 ms**（547.5 ns/op，1.83 M 行/s） | 451.7 ms（451.7 ns/op） | 405.5 ms（405.5 ns/op，2.47 M 行/s） | **1.35x** |
| 插入 1e5（每行清空语句缓存） | 1498 ms（1498 ns/op） | — | 1425 ms（1425 ns/op） | 1.05x |
| 主键查询 1e6（内存库） | **428.8 ms**（428.8 ns/op） | — | 347.9 ms（含对象构造）/ 321.7 ms（纯 step） | **1.23x** / 1.33x |
| 全表扫描 1e6（内存库，Each） | **91.7 ms**（91.7 ns/op） | — | 54.0 ms（54.0 ns/op） | **1.70x** |
| 全表扫描 1e6（内存库，All 物化） | 170.3 ms（170.3 ns/op） | — | — | — |
| UPDATE 1e6（内存库，单事务） | **509.3 ms**（509.3 ns/op） | — | 468.7 ms（468.7 ns/op） | **1.09x** |
| DELETE 1e6（内存库，单事务） | **265.6 ms**（265.6 ns/op） | — | 219.6 ms（219.6 ns/op） | **1.21x** |

`db.hpp` 层对照组用同一个封装（`DB::Prepare` + `Stmt::Bind/Step/Reset`），
用来把 ORM 开销拆成「封装层 + ORM 层」：插入时封装层 +46 ns/行（1.11x），
ORM 层再 +96 ns/行。

### 11.2 优化前/后（同一台机器，200k 行，3 次中位数）

| 指标 | 优化前（`recursive_mutex` 每行加锁） | 优化后（thread-local 事务标记 + 表状态快速路径 + 缓存原生句柄） |
|---|---|---|
| 插入 200k（文件库） | 574.6 ns/op | **490 ns/op（-15%，-85 ns/op）** |
| 主键查询 200k | 384.9 ns/op | 407 ns/op（噪声内） |
| UPDATE 200k | 508 ns/op | 517 ns/op（噪声内） |
| DELETE 200k | 230.5 ns/op | 245 ns/op（噪声内） |

其它前后对比：

| 前后项 | 前 | 后 | 比值 |
|---|---|---|---|
| 语句缓存（1e5 插入） | 每行 `ClearStatementCache` 1498 ns/op | 缓存命中 547.5 ns/op（1e6 组） | **2.7x** |
| 表上 `Indexed()`（1e6 插入） | 带 `value` 索引 857 ns/op | 无索引 547.5 ns/op | 1.56x（索引维护成本，非 ORM 开销） |

---

## 12. 离下限几倍

三个参照系：

1. **SQLite 原生同配置（本机实测下限）**
   插入 405.5 ns/行 = **2.47 M 行/s**（WAL + `synchronous=NORMAL` + 单事务 +
   预编译复用）。这是「引擎本体」的下限，不含任何 ORM/封装代码。
   ORM 547.5 ns/行 = 1.83 M 行/s，是引擎下限的 **1.35x**；`db.hpp` 层 451.7
   ns/行 = 1.11x。

2. **每行绝对开销**
   ORM 比原生多 **142 ns/行**：其中 `db.hpp` 封装 46 ns（`Value` 构造 +
   `SQLITE_TRANSIENT` 文本拷贝 + 参数计数校验），ORM 层 96 ns（表状态分支 +
   两个函数指针间接调用 + `Result`/`Error` 构造 + 主键回填）。
   查询侧 ORM 比「原生 + 构造对象」多 **81 ns/行**（会话锁 + 语句缓存查找 +
   列读取回调 + `Result<T>`）。

3. **顺序写带宽（物理上限参照）**
   1e6 行 × 30 B ≈ 30 MB；10 GB/s memcpy 下限约 **3 ns/行**。SQLite 的
   405–547 ns/行 是它的 **~150–180x**，这是 b-tree + WAL 页写 + 事务日志的
   引擎代价，不是 ORM 引入的。

---

## 13. objdump 证据

对 `libgo2cpp_storage.so.0.1.0` 反汇编（`objdump -d -C --no-show-raw-insn`，
`-O3`，`-fno-stack-protector -fno-semantic-interposition`）：

| 函数 | 指令数 | call | PLT | mutex | new/free | canary |
|---|---:|---:|---:|---:|---:|---:|
| `Session::InsertImpl` | 1074 | 52 | 40 | **1/1** | 0/9 | 0 |
| `Session::UpdateImpl` | 1051 | 56 | 44 | **1/1** | 0/9 | 0 |
| `Session::DeleteImpl` | 696 | 22 | 16 | **1/1** | 0/5 | 0 |
| `Session::FindImpl` | 654 | 20 | 14 | **1/1** | 0/5 | 0 |
| `Session::ForEachRow` | 1104 | 40 | 33 | **1/1** | 0/12 | 0 |
| `Session::StateLocked` | 383 | 32 | 24 | 0/0 | 2/7 | 0 |
| `Stmt::BindValues` | 494 | 15 | 13 | 0/0 | 0/3 | 0 |
| `Stmt::Step` | 389 | 9 | 8 | 0/0 | 0/3 | 0 |
| `Stmt::Reset` | 104 | 2 | 2 | 0/0 | 0/0 | 0 |

说明：

- 每个 CRUD 内核静态只有 **1 对 mutex 调用点**（`Session::Lock()` 的
  lock/unlock）。事务内通过 thread-local 标记跳过，基准插入路径实际
  **0 次 `pthread_mutex_lock`**；非事务路径每行一次无竞争加锁。
- `new/free` 列里的 free 全部来自 `std::string` 析构（`Value`/`Error`）；
  SSO 文本不会走到，热路径无新增堆分配。`StateLocked` 的 2 次 new 只在
  首次准备表时发生。
- 全部函数 `stack_chk=0`（`-fno-stack-protector` 生效）；`Stmt::Step`/
  `Stmt::BindValues` 与 `storage-sqlite.md` 的静态统计一致（389/494）。
- 静态指令数含错误分支/冷代码，热路径约占 20–30%；`InsertImpl` 的 1074
  条静态指令里，每行实际执行的是「表状态分支 → 两次 bind 回调 → Reset →
  BindValues → Step → 回填 → Result 构造」。

复现：

```bash
objdump -d -C --no-show-raw-insn \
  .build-bento-wsl/libgo2cpp_storage.so.0.1.0 \
  | grep -A30 '<go2cpp::storage::Session::InsertImpl'
```

---

## 14. 边界与陷阱

- **`Reset` 必须保留**：`sqlite3_bind_*` 在语句 `step` 到 DONE 后返回
  `SQLITE_MISUSE`（实测去掉 `sqlite3_reset` 后插入 100% 失败）。缓存语句的
  正确顺序是 `Reset` → `BindValues` → `Step`。
- **事务内跨线程**：`Transaction(fn)` 持会话锁，`fn` 内把同一个 `Session` 交给
  其它线程会阻塞到事务结束。
- **绑定列数上限**：`kMaxColumns = 32`，超过返回 `kErrMapping`。
- **主键**：CRUD 需要单一整数主键；文本/复合主键只能用 `Query<T>`。
- **`uint64`**：超过 `INT64_MAX` 无法用 SQLite INTEGER 表示（反射桥接按位回绕）。
- **迁移 `down`**：`DROP COLUMN` 需要 SQLite 3.35+；`AddCreateTable` 的 `down`
  是 `DROP TABLE`，会丢数据。
- **存储过程/触发器**：迁移脚本可包含多条语句（`DB::Exec` 脚本模式），但事务
  语义仍由 SQLite 决定。
- **线程安全**：`Session` 的公开方法都上锁；`Query<T>` 对象本身是单线程的
  （持有参数向量与 SQL 片段）。

---

## 15. 与 GORM / sqlite_orm 的对比

| 维度 | GORM（Go） | sqlite_orm（C++） | 本模块 |
|---|---|---|---|
| 映射 | struct tag + 运行期反射 | `make_column`/`make_storage` 模板 | 宏显式声明 + 编译期 `if constexpr` 回调；可选 `FieldList<T>` 桥接 |
| 主键回填 | `Create` 后写入 | `insert` 返回 rowid | `Insert(T&)` 回填 |
| 查询 | 链式 + 结构体条件 | 编译期表达式树 | 运行期子串 + `?` 绑定（不解析、不建树） |
| 迁移 | `AutoMigrate`（反射推断） | 无内建 | 版本表 + 显式 `Up/Down`（建表/加列/索引） |
| 事务 | `db.Transaction(fn)` | `transaction(fn)` | `Session::Transaction(fn)`（RAII + 回调） |
| 错误 | `error` | 异常 | `Error`/`Result<T>`（不抛异常） |
| 运行期反射 | 有（映射与回调链） | 无 | **无**（宏路径）；反射桥接为可选便捷入口 |

**性能位置**：GORM 的 `Create` 相对原生 `database/sql` 常见在 1.2–2x 区间
（反射 + 回调链 + 语句缓存），本模块插入 1.35x、查询 1.23x，处于同一量级；
差异来自本模块**没有运行期反射**（宏在展开点生成类型擦除回调）、**没有回调
链**（CRUD 直接落到 4 条预编译语句）、以及每表一次的表状态准备。

> 说明：没有在同一台机器上跑 GORM（Go 工具链与依赖不可用），上表是结构对比
> 与本模块自测数据；GORM 的区间取自其公开基准的常见量级，非本机实测。

---

## 16. 后续

- **关系映射**：has-many/belongs-to 需要外键元数据与预加载策略，当前只有单表；
- **批量插入**：`InsertMany(span)` 可用 `BEGIN` + 循环复用同一条语句；若要
  接近原生批量，需要多行 `VALUES (...),(...)` 的 SQL 生成；
- **查询表达式树**：`Where("age > ?", 18)` 是字符串，拼错在运行期才报；
  编译期表达式（sqlite_orm 风格）是另一个量级的模板工程；
- **迁移 checksum**：版本表预留了 `duration_us`，尚未记录脚本校验和；改历史
  迁移不会被发现（当前靠「版本号不复用」约定）。
