/**
 * @file tests/storage/orm_test.cpp
 * @brief ORM 层测试：表映射 / CRUD（1e6 行）/ 迁移 / 事务 / 查询构造 / 边界。
 *
 * 独立可执行，由 cmake/Go2CppStorage.cmake 在 `GO2CPP_WITH_SQLITE=ON` 时构建
 * 并注册为 ctest 用例 `go2cpp_storage_orm_test`。
 *
 * 覆盖：
 *  1. 映射：int/string/blob/optional/bool/double、列类型/偏移/主键/索引、
 *     生成 SQL 文本（含标识符转义）；
 *  2. CRUD：1e6 行插入（事务内）、抽样 Find、1e6 行 UPDATE、1e6 行 DELETE；
 *  3. 迁移：建表 / 加列 / 建索引 / 逐级回滚 / 幂等 / 失败回滚 / 版本号校验；
 *  4. 事务：提交、回滚、异常回滚、嵌套拒绝、事务内混合读改写；
 *  5. 查询：where/order/limit/offset/count/first、参数化、多条件、分页；
 *  6. 边界：无主键、复合主键、非整数自增主键、Find 不存在、空表、
 *     表名/列名含双引号、语句缓存上限行为。
 *
 * 用法：`go2cpp_storage_orm_test`（无参数，退出码 = 失败断言数）。
 */
#include "go2cpp/storage/orm.hpp"

#include "go2cpp/reflect/reflect.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using go2cpp::storage::AppliedMigration;
using go2cpp::storage::Column;
using go2cpp::storage::ColumnType;
using go2cpp::storage::DB;
using go2cpp::storage::Error;
using go2cpp::storage::Migration;
using go2cpp::storage::Query;
using go2cpp::storage::Session;
using go2cpp::storage::TableDef;
using go2cpp::storage::TableOf;
using go2cpp::storage::kErrMapping;
using go2cpp::storage::kErrMigration;
using go2cpp::storage::kErrNotFound;
using go2cpp::storage::kErrTransaction;

std::atomic<int> g_failures{0};

/// @brief 断言失败时记录文件/行号并继续（一次运行收集全部失败）。
#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            g_failures.fetch_add(1);                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #condition);                                        \
        }                                                                    \
    } while (false)

/// @brief 断言错误码（先检查失败，避免误读成功结果的默认值）。
#define CHECK_CODE(expr, expected)                                            \
    do {                                                                      \
        const ::go2cpp::storage::Error error_ = (expr);                       \
        if (error_.Code() != static_cast<int>(expected)) {                    \
            g_failures.fetch_add(1);                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s code=%d expected=%d (%s)\n",  \
                         __FILE__, __LINE__, #expr, error_.Code(),            \
                         static_cast<int>(expected),                          \
                         error_.Message().c_str());                           \
        }                                                                     \
    } while (false)

using Clock = std::chrono::steady_clock;

std::int64_t ElapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - start)
        .count();
}

/// @brief 每次运行独立的临时目录（析构清理，含 WAL/SHM 文件）。
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
                 ("go2cpp_orm_" + std::to_string(stamp) + "_" +
                  std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string File(const std::string& name) const {
        return (m_path / name).string();
    }

private:
    std::filesystem::path m_path;
};

/// @brief 打开文件库并设置测试用 PRAGMA；失败返回空指针。
std::unique_ptr<DB> OpenFileDb(const std::string& path) {
    auto opened = DB::Open(path);
    CHECK(opened.ok());
    if (!opened.ok()) {
        return nullptr;
    }
    std::unique_ptr<DB> db = std::move(opened.Value());
    CHECK(db->SetJournalMode("WAL").ok());
    CHECK(db->SetSynchronous("NORMAL").ok());
    return db;
}

/// @brief 打开内存库。
std::unique_ptr<DB> OpenMemoryDb() {
    auto opened = DB::Open(":memory:");
    CHECK(opened.ok());
    if (!opened.ok()) {
        return nullptr;
    }
    return std::move(opened.Value());
}

std::int64_t CountRows(DB& db, const std::string& sql) {
    auto queried = db.Query(sql);
    if (!queried) {
        CHECK(false);
        return -1;
    }
    auto& rows = queried.Value();
    std::int64_t count = -1;
    if (rows.Next()) {
        count = rows.Int64(0);
    }
    CHECK(!rows.Err());
    return count;
}

} // namespace

// ===========================================================================
// 表映射（必须在全局命名空间展开宏）
// ===========================================================================

struct Account {
    std::int64_t id = 0;
    std::string name;
    int age = 0;
    double balance = 0.0;
    bool active = false;
    std::optional<std::string> email;
    std::vector<std::uint8_t> avatar;
};

GO2CPP_ORM_TABLE(Account, "accounts",
                 GO2CPP_ORM_COLUMN(Account, id).PrimaryKey().AutoIncrement(),
                 GO2CPP_ORM_COLUMN(Account, name).NotNull().Unique(),
                 GO2CPP_ORM_COLUMN(Account, age).Indexed(),
                 GO2CPP_ORM_COLUMN(Account, balance).Default("0"),
                 GO2CPP_ORM_COLUMN(Account, active),
                 GO2CPP_ORM_COLUMN(Account, email),
                 GO2CPP_ORM_COLUMN(Account, avatar))

struct Sample {
    std::int64_t id = 0;
    std::int64_t value = 0;
    std::string tag;
};

GO2CPP_ORM_TABLE(Sample, "samples",
                 GO2CPP_ORM_COLUMN(Sample, id).PrimaryKey().AutoIncrement(),
                 GO2CPP_ORM_COLUMN(Sample, value).Indexed(),
                 GO2CPP_ORM_COLUMN(Sample, tag))

struct NoKey {
    std::int64_t value = 0;
    std::string text;
};

GO2CPP_ORM_TABLE(NoKey, "no_key", GO2CPP_ORM_COLUMN(NoKey, value),
                 GO2CPP_ORM_COLUMN(NoKey, text))

struct TwoKeys {
    std::int64_t a = 0;
    std::int64_t b = 0;
};

GO2CPP_ORM_TABLE(TwoKeys, "two_keys",
                 GO2CPP_ORM_COLUMN(TwoKeys, a).PrimaryKey(),
                 GO2CPP_ORM_COLUMN(TwoKeys, b).PrimaryKey())

struct TextKey {
    std::string key;
    std::int64_t value = 0;
};

GO2CPP_ORM_TABLE(TextKey, "text_key",
                 GO2CPP_ORM_COLUMN(TextKey, key).PrimaryKey().AutoIncrement(),
                 GO2CPP_ORM_COLUMN(TextKey, value))

struct Quoted {
    std::int64_t id = 0;
    std::string text;
};

GO2CPP_ORM_TABLE(Quoted, "quo\"ted",
                 GO2CPP_ORM_COLUMN(Quoted, id).PrimaryKey().AutoIncrement(),
                 GO2CPP_ORM_COLUMN(Quoted, text).Named("te\"xt"))

struct Score {
    std::int64_t id = 0;
    std::string name;
};

GO2CPP_ORM_TABLE(Score, "scores",
                 GO2CPP_ORM_COLUMN(Score, id).PrimaryKey().AutoIncrement(),
                 GO2CPP_ORM_COLUMN(Score, name))

// --- reflect 桥接：复用 FieldList<T> 自动映射 -----------------------------

struct ReflectedAll {
    std::int64_t id = 0;
    std::int8_t i8 = 0;
    std::int16_t i16 = 0;
    std::int32_t i32 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    bool flag = false;
    double real = 0.0;
    std::string text;
    std::vector<std::uint8_t> blob;
};

template <>
struct go2cpp::reflect::FieldList<ReflectedAll> {
    static constexpr go2cpp::reflect::TypeId kId = 900;
    static constexpr const char* kName = "ReflectedAll";
    static const go2cpp::reflect::FieldDescriptor* Fields(std::size_t& count) {
        static const go2cpp::reflect::FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(ReflectedAll, id),
            GO2CPP_REFLECT_FIELD(ReflectedAll, i8),
            GO2CPP_REFLECT_FIELD(ReflectedAll, i16),
            GO2CPP_REFLECT_FIELD(ReflectedAll, i32),
            GO2CPP_REFLECT_FIELD(ReflectedAll, u32),
            GO2CPP_REFLECT_FIELD(ReflectedAll, u64),
            GO2CPP_REFLECT_FIELD(ReflectedAll, flag),
            GO2CPP_REFLECT_FIELD(ReflectedAll, real),
            GO2CPP_REFLECT_FIELD(ReflectedAll, text),
            GO2CPP_REFLECT_FIELD(ReflectedAll, blob),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

GO2CPP_ORM_TABLE_REFLECT(ReflectedAll, "reflected_all", "id")

struct NoReflect {
    std::int64_t id = 0;
};

namespace {

// ===========================================================================
// 1. 映射
// ===========================================================================

void TestMapping() {
    const auto& table = TableOf<Account>();
    CHECK(table.name == "accounts");
    CHECK(table.count == 7);
    CHECK(table.columns[0].name == "id");
    CHECK(table.columns[0].type == ColumnType::kInteger);
    CHECK(table.columns[0].size == sizeof(std::int64_t));
    CHECK(table.columns[0].IsPrimaryKey());
    CHECK(table.columns[0].IsAutoIncrement());
    CHECK(table.columns[1].type == ColumnType::kText);
    CHECK((table.columns[1].flags & go2cpp::storage::kColumnNotNull) != 0);
    CHECK((table.columns[1].flags & go2cpp::storage::kColumnUnique) != 0);
    CHECK(table.columns[2].type == ColumnType::kInteger);
    CHECK((table.columns[2].flags & go2cpp::storage::kColumnIndexed) != 0);
    CHECK(table.columns[3].type == ColumnType::kReal);
    CHECK(table.columns[3].default_sql == "0");
    CHECK(table.columns[4].type == ColumnType::kInteger);   // bool
    CHECK(table.columns[5].type == ColumnType::kText);      // optional<string>
    CHECK(table.columns[6].type == ColumnType::kBlob);
    CHECK(table.columns[1].offset > table.columns[0].offset);
    CHECK(table.PrimaryKeyIndex() == 0);
    CHECK(table.Find("age") == &table.columns[2]);
    CHECK(table.Find("missing") == nullptr);

    // 生成 SQL：类型/主键/唯一/索引/默认值都在。
    const std::string create = go2cpp::storage::CreateTableSql(table);
    CHECK(create.find("CREATE TABLE IF NOT EXISTS \"accounts\"") == 0);
    CHECK(create.find("\"id\" INTEGER PRIMARY KEY AUTOINCREMENT") !=
          std::string::npos);
    CHECK(create.find("\"name\" TEXT NOT NULL UNIQUE") != std::string::npos);
    CHECK(create.find("\"balance\" REAL DEFAULT 0") != std::string::npos);
    CHECK(create.find("CREATE INDEX IF NOT EXISTS \"accounts_age_idx\"") !=
          std::string::npos);
    CHECK(go2cpp::storage::DropTableSql("accounts") ==
          "DROP TABLE IF EXISTS \"accounts\"");
    CHECK(go2cpp::storage::QuoteIdentifier("a\"b") == "\"a\"\"b\"");
    CHECK(go2cpp::storage::AddColumnSql("t", "c", ColumnType::kText, true) ==
          "ALTER TABLE \"t\" ADD COLUMN \"c\" TEXT NOT NULL DEFAULT ''");

    // 类型推导静态检查。
    static_assert(go2cpp::storage::ColumnTypeOf<int>() ==
                  ColumnType::kInteger);
    static_assert(go2cpp::storage::ColumnTypeOf<double>() == ColumnType::kReal);
    static_assert(go2cpp::storage::ColumnTypeOf<std::string>() ==
                  ColumnType::kText);
    static_assert(go2cpp::storage::ColumnTypeOf<std::vector<std::uint8_t>>() ==
                  ColumnType::kBlob);
    static_assert(go2cpp::storage::ColumnTypeOf<std::optional<int>>() ==
                  ColumnType::kInteger);
}

void TestRoundTrip() {
    auto db = OpenMemoryDb();
    if (!db) {
        return;
    }
    Session session(*db);
    Migration migration(*db);
    migration.AddCreateTable<Account>(1, "create accounts");
    CHECK(migration.Up().ok());

    Account alice;
    alice.name = "alice";
    alice.age = 30;
    alice.balance = 12.5;
    alice.active = true;
    alice.email = "alice@example.com";
    alice.avatar = {0x00, 0x01, 0xfe, 0xff, 0x00};
    auto inserted = session.Insert(alice);
    CHECK(inserted.ok());
    CHECK(alice.id == 1);

    Account bob;
    bob.name = "bob";
    bob.age = -7;
    bob.balance = -0.25;
    bob.active = false;
    bob.email = std::nullopt;
    auto bob_inserted = session.Insert(bob);
    CHECK(bob_inserted.ok());
    CHECK(bob.id == 2);

    auto found = session.Find<Account>(alice.id);
    CHECK(found.ok());
    const Account& loaded = found.Value();
    CHECK(loaded.id == alice.id);
    CHECK(loaded.name == "alice");
    CHECK(loaded.age == 30);
    CHECK(loaded.balance == 12.5);
    CHECK(loaded.active);
    CHECK(loaded.email.has_value() && *loaded.email == "alice@example.com");
    CHECK(loaded.avatar == alice.avatar);

    auto loaded_bob = session.Find<Account>(bob.id);
    CHECK(loaded_bob.ok());
    CHECK(!loaded_bob.Value().email.has_value());
    CHECK(!loaded_bob.Value().active);
    CHECK(loaded_bob.Value().avatar.empty());
    CHECK(loaded_bob.Value().age == -7);

    // 空串与 NULL 必须可区分（optional 的价值）。
    Account empty;
    empty.name = "empty";
    empty.email = std::string{};
    CHECK(session.Insert(empty).ok());
    auto loaded_empty = session.Find<Account>(empty.id);
    CHECK(loaded_empty.ok());
    CHECK(loaded_empty.Value().email.has_value());
    CHECK(loaded_empty.Value().email->empty());

    // 二进制里的 0x00 不截断。
    CHECK(loaded.avatar.size() == 5);
    CHECK(loaded.avatar[0] == 0x00);
    CHECK(loaded.avatar[2] == 0xfe);

    // 不存在的主键。
    CHECK_CODE(session.Find<Account>(999).Err(), kErrNotFound);
}

// ===========================================================================
// 2. CRUD：1e6 行
// ===========================================================================

void TestCrudMillion() {
    TempDir temp;
    auto db = OpenFileDb(temp.File("crud.db"));
    if (!db) {
        return;
    }
    Session session(*db);
    Migration migration(*db);
    migration.AddCreateTable<Sample>(1, "create samples");
    CHECK(migration.Up().ok());

    constexpr std::int64_t kRows = 1000000;
    const auto insert_start = Clock::now();
    CHECK(session.Transaction([&]() -> Error {
        Sample row;
        for (std::int64_t index = 0; index < kRows; ++index) {
            row = Sample{};
            row.value = index * 3;
            row.tag = (index % 2) == 0 ? "even" : "odd";
            auto inserted = session.Insert(row);
            if (!inserted) {
                return inserted.Err();
            }
            if (row.id != index + 1) {
                return Error(kErrMapping, 0, "rowid mismatch");
            }
        }
        return {};
    }).ok());
    const auto insert_ms = ElapsedMs(insert_start);
    CHECK(CountRows(*db, "SELECT COUNT(*) FROM samples") == kRows);

    // 抽样按主键查（含首尾）。
    const auto find_start = Clock::now();
    for (std::int64_t index = 1; index <= kRows; index += 997) {
        auto found = session.Find<Sample>(index);
        if (!found.ok()) {
            CHECK(false);
            break;
        }
        CHECK(found.Value().id == index);
        CHECK(found.Value().value == (index - 1) * 3);
    }
    CHECK(session.Find<Sample>(1).Value().tag == "even");
    CHECK(session.Find<Sample>(2).Value().tag == "odd");
    const auto find_ms = ElapsedMs(find_start);

    // 1e6 行 UPDATE：每条把 value 翻倍。
    const auto update_start = Clock::now();
    CHECK(session.Transaction([&]() -> Error {
        for (std::int64_t index = 1; index <= kRows; ++index) {
            Sample row;
            row.id = index;
            row.value = (index - 1) * 6;
            row.tag = (index % 2) == 0 ? "even" : "odd";
            auto updated = session.Update(row);
            if (!updated) {
                return updated.Err();
            }
            if (updated.Value() != 1) {
                return Error(kErrMapping, 0, "update affected != 1");
            }
        }
        return {};
    }).ok());
    const auto update_ms = ElapsedMs(update_start);
    CHECK(CountRows(*db, "SELECT COUNT(*) FROM samples WHERE value % 6 = 0") ==
          kRows);

    // 1e6 行 DELETE。
    const auto delete_start = Clock::now();
    CHECK(session.Transaction([&]() -> Error {
        for (std::int64_t index = 1; index <= kRows; ++index) {
            auto removed = session.Delete<Sample>(index);
            if (!removed) {
                return removed.Err();
            }
            if (removed.Value() != 1) {
                return Error(kErrMapping, 0, "delete affected != 1");
            }
        }
        return {};
    }).ok());
    const auto delete_ms = ElapsedMs(delete_start);
    CHECK(CountRows(*db, "SELECT COUNT(*) FROM samples") == 0);
    // 删除不存在的主键返回 0，不是错误。
    auto missing = session.Delete<Sample>(1);
    CHECK(missing.ok() && missing.Value() == 0);

    std::printf(
        "[crud] 1e6 insert=%lld ms  sample-find=%lld ms  1e6 update=%lld ms  "
        "1e6 delete=%lld ms\n",
        static_cast<long long>(insert_ms), static_cast<long long>(find_ms),
        static_cast<long long>(update_ms), static_cast<long long>(delete_ms));
}

// ===========================================================================
// 3. 迁移
// ===========================================================================

bool TableExists(DB& db, const std::string& name) {
    return CountRows(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                         "AND name='" +
                             name + "'") == 1;
}

bool IndexExists(DB& db, const std::string& name) {
    return CountRows(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' "
                         "AND name='" +
                             name + "'") == 1;
}

bool ColumnExists(DB& db, const std::string& table, const std::string& column) {
    auto queried = db.Query("PRAGMA table_info(" + table + ")");
    if (!queried) {
        return false;
    }
    auto& rows = queried.Value();
    while (rows.Next()) {
        if (rows.Text(1) == column) {
            return true;
        }
    }
    return false;
}

void TestMigration() {
    TempDir temp;
    auto db = OpenFileDb(temp.File("migrate.db"));
    if (!db) {
        return;
    }
    Migration migration(*db);
    migration.AddCreateTable<Score>(1, "create scores")
        .AddColumn(2, "add score column", "scores", "score", ColumnType::kInteger,
                   true, "0")
        .AddCreateIndex(3, "index score", "scores_score_idx", "scores",
                        {"score"});
    CHECK(migration.Steps().size() == 3);

    CHECK(migration.Up().ok());
    CHECK(migration.CurrentVersion().ok());
    CHECK(migration.CurrentVersion().Value() == 3);
    CHECK(TableExists(*db, "scores"));
    CHECK(ColumnExists(*db, "scores", "score"));
    CHECK(IndexExists(*db, "scores_score_idx"));
    CHECK(CountRows(*db, "SELECT COUNT(*) FROM \"_go2cpp_migrations\"") == 3);

    // 幂等：再次 Up 不重复执行、版本不变。
    CHECK(migration.Up().ok());
    CHECK(CountRows(*db, "SELECT COUNT(*) FROM \"_go2cpp_migrations\"") == 3);

    // 版本表内容。
    auto applied = migration.Applied();
    CHECK(applied.ok());
    CHECK(applied.Value().size() == 3);
    CHECK(applied.Value()[0].version == 1);
    CHECK(applied.Value()[0].name == "create scores");
    CHECK(applied.Value()[1].version == 2);
    CHECK(applied.Value()[2].version == 3);
    CHECK(applied.Value()[2].applied_at > 0);

    // 逐级回滚：索引 → 列 → 表。
    CHECK(migration.Down(1).ok());
    CHECK(migration.CurrentVersion().Value() == 2);
    CHECK(!IndexExists(*db, "scores_score_idx"));
    CHECK(ColumnExists(*db, "scores", "score"));
    CHECK(migration.Down(1).ok());
    CHECK(migration.CurrentVersion().Value() == 1);
    CHECK(!ColumnExists(*db, "scores", "score"));
    CHECK(migration.Down(1).ok());
    CHECK(migration.CurrentVersion().Value() == 0);
    CHECK(!TableExists(*db, "scores"));

    // 回滚到底后再次升级。
    CHECK(migration.Up().ok());
    CHECK(migration.CurrentVersion().Value() == 3);

    // 失败迁移回滚：合法建表 + 非法语句，事务内整体撤销。
    {
        Migration broken(*db);
        broken.AddCreateTable<Score>(1, "create scores");
        broken.Add(10, "broken", "CREATE TABLE broken_table (id INTEGER); "
                                 "THIS IS NOT SQL;",
                   "DROP TABLE IF EXISTS broken_table;");
        // 版本 1/2/3 已应用（同一版本表），只跑版本 10。
        auto failed = broken.Up();
        CHECK(!failed.ok());
        CHECK(failed.Message().find("migration 10 (broken)") !=
              std::string::npos);
        CHECK(!TableExists(*db, "broken_table"));
        CHECK(CountRows(*db, "SELECT COUNT(*) FROM \"_go2cpp_migrations\"") == 3);
        CHECK(broken.CurrentVersion().Value() == 3);
    }

    // 版本号非递增：声明期错误在 Up 时上报。
    {
        Migration invalid(*db);
        invalid.Add(5, "five", "SELECT 1");
        invalid.Add(4, "four", "SELECT 1");
        CHECK_CODE(invalid.Up(), kErrMigration);
    }

    // 已应用但未声明的版本无法回滚。
    {
        auto inserted = db->Exec(
            "INSERT INTO \"_go2cpp_migrations\" (version, name, applied_at, "
            "duration_us) VALUES (99, 'manual', 0, 0)");
        CHECK(inserted.ok());
        Migration partial(*db);
        partial.AddCreateTable<Score>(1, "create scores");
        CHECK_CODE(partial.Down(1), kErrMigration);
        CHECK(db->Exec("DELETE FROM \"_go2cpp_migrations\" WHERE version = 99")
                  .ok());
    }

    // MigrateTo：先降后升。
    CHECK(migration.MigrateTo(2).ok());
    CHECK(migration.CurrentVersion().Value() == 2);
    CHECK(migration.MigrateTo(3).ok());
    CHECK(migration.CurrentVersion().Value() == 3);
    CHECK(migration.MigrateTo(3).ok());

    // 自定义版本表名。
    {
        Migration custom(*db, "_custom_migrations");
        custom.Add(1, "one", "CREATE TABLE custom_probe (id INTEGER)");
        CHECK(custom.Up().ok());
        CHECK(TableExists(*db, "_custom_migrations"));
        CHECK(TableExists(*db, "custom_probe"));
    }
}

// ===========================================================================
// 4. 事务
// ===========================================================================

void TestTransaction() {
    auto db = OpenMemoryDb();
    if (!db) {
        return;
    }
    Session session(*db);
    Migration migration(*db);
    migration.AddCreateTable<Sample>(1, "create samples");
    CHECK(migration.Up().ok());

    // 提交。
    Error committed = session.Transaction([&]() -> Error {
        Sample row;
        row.value = 1;
        row.tag = "one";
        auto inserted = session.Insert(row);
        if (!inserted) {
            return inserted.Err();
        }
        row.value = 2;
        row.tag = "two";
        inserted = session.Insert(row);
        if (!inserted) {
            return inserted.Err();
        }
        return {};
    });
    CHECK(committed.ok());
    CHECK(CountRows(*db, "SELECT COUNT(*) FROM samples") == 2);

    // 回滚（body 返回错误）。
    Error aborted = session.Transaction([&]() -> Error {
        Sample row;
        row.value = 3;
        row.tag = "three";
        auto inserted = session.Insert(row);
        if (!inserted) {
            return inserted.Err();
        }
        return Error(kErrMapping, 0, "abort on purpose");
    });
    CHECK(!aborted.ok());
    CHECK(aborted.Code() == kErrMapping);
    CHECK(CountRows(*db, "SELECT COUNT(*) FROM samples") == 2);

    // 异常回滚。
    bool caught = false;
    try {
        session.Transaction([&]() {
            Sample row;
            row.value = 4;
            row.tag = "four";
            (void)session.Insert(row);
            throw std::runtime_error("boom");
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
    CHECK(CountRows(*db, "SELECT COUNT(*) FROM samples") == 2);

    // 事务内混合读改写后回滚。
    Error mixed = session.Transaction([&]() -> Error {
        auto found = session.Find<Sample>(1);
        if (!found.ok()) {
            return found.Err();
        }
        Sample row = found.Value();
        row.tag = "changed";
        if (auto updated = session.Update(row); !updated) {
            return updated.Err();
        }
        if (auto removed = session.Delete<Sample>(2); !removed) {
            return removed.Err();
        }
        return Error(kErrTransaction, 0, "rollback mixed");
    });
    CHECK(!mixed.ok());
    CHECK(session.Find<Sample>(1).Value().tag == "one");
    CHECK(session.Find<Sample>(2).ok());

    // 嵌套事务拒绝。
    Error nested = session.Transaction([&]() -> Error {
        Error inner = session.Transaction([]() -> Error { return {}; });
        CHECK(inner.Code() == kErrTransaction);
        return Error(kErrTransaction, 0, "outer abort");
    });
    CHECK(nested.Code() == kErrTransaction);
    CHECK(!db->InTransaction());

    // 事务外仍可用。
    Sample after;
    after.value = 5;
    after.tag = "five";
    CHECK(session.Insert(after).ok());
    CHECK(CountRows(*db, "SELECT COUNT(*) FROM samples") == 3);
}

// ===========================================================================
// 5. 查询
// ===========================================================================

void TestQuery() {
    auto db = OpenMemoryDb();
    if (!db) {
        return;
    }
    Session session(*db);
    Migration migration(*db);
    migration.AddCreateTable<Sample>(1, "create samples");
    CHECK(migration.Up().ok());

    CHECK(session.Transaction([&]() -> Error {
        Sample row;
        for (int index = 0; index < 100; ++index) {
            row = Sample{};
            row.value = index;
            row.tag = "tag" + std::to_string(index % 10);
            auto inserted = session.Insert(row);
            if (!inserted) {
                return inserted.Err();
            }
        }
        return {};
    }).ok());

    Query<Sample> query = session.Query<Sample>();
    query.Where("value > ?", 10);
    CHECK(query.Sql() ==
          "SELECT \"id\", \"value\", \"tag\" FROM \"samples\" WHERE "
          "(value > ?)");
    auto selected = query.OrderBy("value DESC").Limit(5).All();
    CHECK(selected.ok());
    CHECK(selected.Value().size() == 5);
    CHECK(selected.Value()[0].value == 99);
    CHECK(selected.Value()[4].value == 95);

    // 多条件 AND。
    auto ranged = session.Query<Sample>()
                      .Where("value >= ?", 20)
                      .Where("value < ?", 30)
                      .OrderByColumn("value")
                      .All();
    CHECK(ranged.ok());
    CHECK(ranged.Value().size() == 10);
    CHECK(ranged.Value().front().value == 20);
    CHECK(ranged.Value().back().value == 29);

    // 分页：limit + offset。
    auto page = session.Query<Sample>()
                    .OrderByColumn("id")
                    .Limit(10)
                    .Offset(20)
                    .All();
    CHECK(page.ok());
    CHECK(page.Value().size() == 10);
    CHECK(page.Value().front().value == 20);

    // Count 只看 where，忽略 order/limit。
    auto counted = session.Query<Sample>().Where("value < ?", 10).Limit(3).Count();
    CHECK(counted.ok());
    CHECK(counted.Value() == 10);
    auto all_count = session.Count<Sample>();
    CHECK(all_count.ok() && all_count.Value() == 100);

    // First / FirstOptional。
    auto first = session.Query<Sample>().OrderByColumn("value", true).First();
    CHECK(first.ok());
    CHECK(first.Value().value == 99);
    auto none = session.Query<Sample>().Where("value > ?", 1000).First();
    CHECK_CODE(none.Err(), kErrNotFound);
    auto maybe = session.Query<Sample>().Where("value > ?", 1000).FirstOptional();
    CHECK(maybe.ok());
    CHECK(!maybe.Value().has_value());

    // Each 提前结束；返回值是遍历到的行数。
    std::int64_t visited = 0;
    auto walked = session.Query<Sample>().OrderByColumn("value").Each(
        [&visited](const Sample& row) {
            ++visited;
            CHECK(row.value == visited - 1);
            return visited < 5;
        });
    CHECK(walked.ok());
    CHECK(walked.Value() == 5);
    CHECK(visited == 5);

    // 参数化：引号/注入串按值匹配，不改变语义。
    Sample tricky;
    tricky.value = 1000;
    tricky.tag = "a' OR '1'='1";
    CHECK(session.Insert(tricky).ok());
    auto injection = session.Query<Sample>().Where("tag = ?", tricky.tag).All();
    CHECK(injection.ok());
    CHECK(injection.Value().size() == 1);
    CHECK(injection.Value()[0].id == tricky.id);
    auto injection_all =
        session.Query<Sample>().Where("tag = 'a'' OR ''1''=''1'").All();
    CHECK(injection_all.ok());
    CHECK(injection_all.Value().size() == 1);

    // 空结果。
    auto empty = session.Query<Sample>().Where("value < ?", -1).All();
    CHECK(empty.ok());
    CHECK(empty.Value().empty());

    // 语句缓存：不同 SQL 各自缓存，Clear 后清空。
    const std::size_t cached = session.CachedStatementCount();
    CHECK(cached >= 2);
    session.ClearStatementCache();
    CHECK(session.CachedStatementCount() == 0);
    // 清空后仍能继续查询。
    CHECK(session.Query<Sample>().Count().Value() == 101);
}

// ===========================================================================
// 6. 边界
// ===========================================================================

void TestEdge() {
    auto db = OpenMemoryDb();
    if (!db) {
        return;
    }
    Session session(*db);
    Migration migration(*db);
    migration.AddCreateTable<NoKey>(1, "create no_key")
        .AddCreateTable<Quoted>(2, "create quoted")
        .AddCreateTable<Score>(3, "create scores");
    CHECK(migration.Up().ok());

    // 无主键：CRUD 拒绝，错误可判定。
    {
        NoKey row;
        row.value = 1;
        CHECK_CODE(session.Insert(row).Err(), kErrMapping);
        CHECK_CODE(session.Update(row).Err(), kErrMapping);
        CHECK_CODE(session.Delete<NoKey>(1).Err(), kErrMapping);
        CHECK_CODE(session.Find<NoKey>(1).Err(), kErrMapping);
        // 查询仍然可用（Query 不依赖主键）。
        CHECK(session.Query<NoKey>().All().ok());
    }

    // 复合主键拒绝。
    {
        TwoKeys row;
        CHECK_CODE(session.Insert(row).Err(), kErrMapping);
    }

    // 非整数自增主键拒绝。
    {
        TextKey row;
        CHECK_CODE(session.Insert(row).Err(), kErrMapping);
    }

    // 分页 SQL 生成（无 limit 的 offset）。
    CHECK(session.Query<Score>().Offset(5).Sql().find("LIMIT -1 OFFSET 5") !=
          std::string::npos);

    // 表名/列名含双引号：建表 + 插入 + 查询 + Find 往返。
    {
        Quoted row;
        row.text = "quoted";
        CHECK(session.Insert(row).ok());
        auto found = session.Find<Quoted>(row.id);
        CHECK(found.ok());
        CHECK(found.Value().text == "quoted");
        auto all = session.Query<Quoted>().All();
        CHECK(all.ok() && all.Value().size() == 1);
        CHECK(go2cpp::storage::QuoteIdentifier("quo\"ted") == "\"quo\"\"ted\"");
    }

    // 空表。
    CHECK(session.Query<Score>().Count().Value() == 0);
    CHECK(session.All<Score>().Value().empty());
    CHECK_CODE(session.Find<Score>(1).Err(), kErrNotFound);
}

// ===========================================================================
// 7. reflect 桥接
// ===========================================================================

void TestReflectBridge() {
    // 字段表 → 列描述：宽度按 size 精确还原。
    std::size_t count = 0;
    const Column* columns =
        go2cpp::storage::ReflectedColumns<ReflectedAll>(count, "id");
    CHECK(count == 10);
    CHECK(columns != nullptr);
    CHECK(columns[0].name == "id");
    CHECK(columns[0].IsPrimaryKey());
    CHECK(columns[0].IsAutoIncrement());
    CHECK(columns[0].type == ColumnType::kInteger);
    CHECK(columns[1].type == ColumnType::kInteger && columns[1].size == 1);
    CHECK(columns[2].type == ColumnType::kInteger && columns[2].size == 2);
    CHECK(columns[3].type == ColumnType::kInteger && columns[3].size == 4);
    CHECK(columns[4].type == ColumnType::kInteger && columns[4].size == 4);
    CHECK(columns[5].type == ColumnType::kInteger && columns[5].size == 8);
    CHECK(columns[6].type == ColumnType::kInteger && columns[6].size == 1);
    CHECK(columns[7].type == ColumnType::kReal);
    CHECK(columns[8].type == ColumnType::kText);
    CHECK(columns[9].type == ColumnType::kBlob);

    // 没有 FieldList 的类型 → 空映射（CRUD 会返回 kErrMapping）。
    std::size_t broken_count = 0;
    (void)go2cpp::storage::ReflectedColumns<NoReflect>(broken_count, "id");
    CHECK(broken_count == 0);

    // 生成 SQL 与建表。
    const std::string create =
        go2cpp::storage::CreateTableSql(TableOf<ReflectedAll>());
    CHECK(create.find("\"id\" INTEGER PRIMARY KEY AUTOINCREMENT") !=
          std::string::npos);
    CHECK(create.find("\"text\" TEXT") != std::string::npos);
    CHECK(create.find("\"blob\" BLOB") != std::string::npos);

    auto db = OpenMemoryDb();
    if (!db) {
        return;
    }
    Session session(*db);
    Migration migration(*db);
    migration.AddCreateTable<ReflectedAll>(1, "create reflected_all");
    CHECK(migration.Up().ok());

    // 极值往返：保证按字段真实宽度读写（而不是一律 64 位）。
    ReflectedAll row;
    row.i8 = -128;
    row.i16 = -32768;
    row.i32 = std::numeric_limits<std::int32_t>::min();
    row.u32 = 4294967295u;
    row.u64 = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    row.flag = true;
    row.real = -1.5;
    row.text = "reflect 中文";
    row.blob = {0x00, 0xff, 0x7f};
    auto inserted = session.Insert(row);
    CHECK(inserted.ok());
    CHECK(row.id == 1);

    auto found = session.Find<ReflectedAll>(1);
    CHECK(found.ok());
    const ReflectedAll& loaded = found.Value();
    CHECK(loaded.i8 == -128);
    CHECK(loaded.i16 == -32768);
    CHECK(loaded.i32 == std::numeric_limits<std::int32_t>::min());
    CHECK(loaded.u32 == 4294967295u);
    CHECK(loaded.u64 == static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max()));
    CHECK(loaded.flag);
    CHECK(loaded.real == -1.5);
    CHECK(loaded.text == "reflect 中文");
    CHECK(loaded.blob == row.blob);

    // 反射表同样支持 Query/Update/Delete。
    CHECK(session.Query<ReflectedAll>().Count().Value() == 1);
    auto updated = session.Update(found.Value());
    CHECK(updated.ok());
    CHECK(updated.Value() == 1);
    auto removed = session.Delete<ReflectedAll>(1);
    CHECK(removed.ok());
    CHECK(removed.Value() == 1);
    CHECK(session.Count<ReflectedAll>().Value() == 0);
}

// ===========================================================================
// 主流程
// ===========================================================================

} // namespace

int main() {
    const auto start = Clock::now();
    TestMapping();
    TestRoundTrip();
    TestCrudMillion();
    TestMigration();
    TestTransaction();
    TestQuery();
    TestEdge();
    TestReflectBridge();
    const auto total_ms = ElapsedMs(start);

    if (g_failures.load() == 0) {
        std::printf("go2cpp_storage_orm_test: all checks passed (%lld ms)\n",
                    static_cast<long long>(total_ms));
        return 0;
    }
    std::fprintf(stderr, "go2cpp_storage_orm_test: %d check(s) failed\n",
                 g_failures.load());
    return g_failures.load();
}
