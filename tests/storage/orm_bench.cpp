/**
 * @file tests/storage/orm_bench.cpp
 * @brief ORM 性能基准：原生 sqlite3 vs ORM（1e6 插入/查询/更新/删除）。
 *
 * 对比组：
 *  1. 1e6 行插入（文件库，WAL，synchronous=NORMAL，单事务）：
 *     - `native`：预编译 + sqlite3_bind_* + step（SQLite 引擎基线）；
 *     - `native-prepare-each`：每行 prepare/finalize（无语句缓存的基线）；
 *     - `orm`：`Session::Insert`；
 *     - `orm-no-cache`：每行 `ClearStatementCache()` + Insert（语句缓存前/后）。
 *  2. 1e6 次主键查询（内存库）：
 *     - `native-pure`：只 step + 读 value（SQLite 纯查询下限）；
 *     - `native-object`：读列并构造结构体（含 std::string 拷贝）；
 *     - `orm-find`：`Session::Find<T>`。
 *  3. 1e6 行全表扫描（内存库）：`native-pure` / `orm-each` / `orm-all`。
 *  4. 1e6 行 UPDATE / DELETE（内存库，单事务）。
 *
 * 输出：每个场景的 ns/op 与 ORM/原生比值；`orm` 与 `native-object` 的差值
 *       即 ORM 封装开销（语句缓存查找 + 递归锁 + Value 构造 + 回调）。
 *
 * 用法：`go2cpp_storage_orm_bench [rows] [输出目录]`
 *       缺省 rows=1e6，目录用临时目录并在结束时清理。
 */
#include "go2cpp/storage/orm.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using go2cpp::storage::DB;
using go2cpp::storage::Error;
using go2cpp::storage::Migration;
using go2cpp::storage::Session;
using go2cpp::storage::Stmt;

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now() - start)
               .count() /
           1000.0;
}

/// @brief 打印一组对比。
void Report(const char* scenario, std::int64_t count, double orm_ms,
            double native_ms, std::int64_t checksum) {
    const double orm_ns = count > 0 ? orm_ms * 1e6 / count : 0.0;
    const double native_ns = count > 0 ? native_ms * 1e6 / count : 0.0;
    const double ratio = native_ms > 0.0 ? orm_ms / native_ms : 0.0;
    std::printf(
        "%-22s count=%-8lld orm=%9.2f ms (%7.1f ns/op) | native=%9.2f ms "
        "(%7.1f ns/op) | ratio=%5.2fx | delta=%+7.1f ns/op | sum=%lld\n",
        scenario, static_cast<long long>(count), orm_ms, orm_ns, native_ms,
        native_ns, ratio, orm_ns - native_ns,
        static_cast<long long>(checksum));
}

/// @brief 每次运行独立的临时目录。
class TempDir {
public:
    TempDir() {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
                 ("go2cpp_orm_bench_" + std::to_string(stamp));
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

/// @brief 原生 sqlite3 连接的 RAII 包装（基准对照组）。
class NativeDb {
public:
    explicit NativeDb(const std::string& path) {
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        if (sqlite3_open_v2(path.c_str(), &m_db, flags, nullptr) != SQLITE_OK) {
            std::fprintf(stderr, "native open failed: %s\n",
                         sqlite3_errmsg(m_db));
            sqlite3_close(m_db);
            m_db = nullptr;
            return;
        }
        sqlite3_busy_timeout(m_db, 30000);
        Exec("PRAGMA journal_mode=WAL");
        Exec("PRAGMA synchronous=NORMAL");
    }
    ~NativeDb() {
        if (m_db != nullptr) {
            sqlite3_close(m_db);
        }
    }
    NativeDb(const NativeDb&) = delete;
    NativeDb& operator=(const NativeDb&) = delete;

    sqlite3* get() const { return m_db; }

    void Exec(const char* sql) {
        if (sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
            std::fprintf(stderr, "native exec failed: %s (%s)\n",
                         sqlite3_errmsg(m_db), sql);
        }
    }

    sqlite3_stmt* Prepare(const char* sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr) !=
            SQLITE_OK) {
            std::fprintf(stderr, "native prepare failed: %s (%s)\n",
                         sqlite3_errmsg(m_db), sql);
        }
        return statement;
    }

private:
    sqlite3* m_db = nullptr;
};

std::int64_t QueryInt64(sqlite3* db, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return -1;
    }
    std::int64_t value = 0;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    return value;
}

} // namespace

// ===========================================================================
// 被映射的表（与测试同结构）
// ===========================================================================

struct Sample {
    std::int64_t id = 0;
    std::int64_t value = 0;
    std::string tag;
};

GO2CPP_ORM_TABLE(Sample, "samples",
                 GO2CPP_ORM_COLUMN(Sample, id).PrimaryKey().AutoIncrement(),
                 GO2CPP_ORM_COLUMN(Sample, value).Indexed(),
                 GO2CPP_ORM_COLUMN(Sample, tag))

namespace {

const char* TagOf(std::int64_t index) {
    return (index % 2) == 0 ? "even" : "odd";
}

std::int64_t ValueOf(std::int64_t index) {
    return index * 3;
}

// ===========================================================================
// 1. 插入
// ===========================================================================

/// @brief 原生插入：单事务 + 一个预编译语句。
double NativeInsert(NativeDb& db, std::int64_t rows) {
    db.Exec("BEGIN IMMEDIATE");
    sqlite3_stmt* statement =
        db.Prepare("INSERT INTO samples (value, tag) VALUES (?, ?)");
    const auto start = Clock::now();
    for (std::int64_t index = 0; index < rows; ++index) {
        sqlite3_bind_int64(statement, 1, ValueOf(index));
        const char* tag = TagOf(index);
        sqlite3_bind_text(statement, 2, tag, -1, SQLITE_STATIC);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    const double ms = ElapsedMs(start);
    db.Exec("COMMIT");
    sqlite3_finalize(statement);
    return ms;
}

/// @brief 原生插入（每行 prepare/finalize）：无语句缓存的对照。
double NativeInsertPrepareEach(NativeDb& db, std::int64_t rows) {
    db.Exec("BEGIN IMMEDIATE");
    const auto start = Clock::now();
    for (std::int64_t index = 0; index < rows; ++index) {
        sqlite3_stmt* statement =
            db.Prepare("INSERT INTO samples (value, tag) VALUES (?, ?)");
        sqlite3_bind_int64(statement, 1, ValueOf(index));
        const char* tag = TagOf(index);
        sqlite3_bind_text(statement, 2, tag, -1, SQLITE_STATIC);
        sqlite3_step(statement);
        sqlite3_finalize(statement);
    }
    const double ms = ElapsedMs(start);
    db.Exec("COMMIT");
    return ms;
}

/// @brief ORM 插入：Session::Transaction + Insert（语句缓存命中）。
double OrmInsert(Session& session, std::int64_t rows, std::int64_t* checksum) {
    const auto start = Clock::now();
    const Error error = session.Transaction([&]() -> Error {
        Sample row;
        for (std::int64_t index = 0; index < rows; ++index) {
            row = Sample{};
            row.value = ValueOf(index);
            row.tag = TagOf(index);
            auto inserted = session.Insert(row);
            if (!inserted) {
                return inserted.Err();
            }
        }
        return {};
    });
    const double ms = ElapsedMs(start);
    if (error) {
        std::fprintf(stderr, "orm insert failed: %s\n",
                     error.Message().c_str());
    }
    *checksum = rows;
    return ms;
}

/// @brief db.hpp 层插入（不经 ORM）：把 ORM 开销拆成「封装层 + ORM 层」。
double DbLayerInsert(DB& db, std::int64_t rows) {
    auto prepared = db.Prepare("INSERT INTO samples (value, tag) VALUES (?, ?)");
    if (!prepared) {
        return -1.0;
    }
    Stmt& statement = prepared.Value();
    if (Error begun = db.Begin(); begun) {
        return -1.0;
    }
    const auto start = Clock::now();
    for (std::int64_t index = 0; index < rows; ++index) {
        (void)statement.Reset();
        (void)statement.Bind(ValueOf(index), TagOf(index));
        (void)statement.Step();
    }
    const double ms = ElapsedMs(start);
    (void)db.Commit();
    return ms;
}

/// @brief ORM 插入（每行清空语句缓存）：无缓存的对照。
double OrmInsertNoCache(Session& session, std::int64_t rows,
                        std::int64_t* checksum) {
    const auto start = Clock::now();
    const Error error = session.Transaction([&]() -> Error {
        Sample row;
        for (std::int64_t index = 0; index < rows; ++index) {
            row = Sample{};
            row.value = ValueOf(index);
            row.tag = TagOf(index);
            session.ClearStatementCache();
            auto inserted = session.Insert(row);
            if (!inserted) {
                return inserted.Err();
            }
        }
        return {};
    });
    const double ms = ElapsedMs(start);
    if (error) {
        std::fprintf(stderr, "orm no-cache insert failed: %s\n",
                     error.Message().c_str());
    }
    *checksum = rows;
    return ms;
}

void BenchInsert(std::int64_t rows, TempDir& temp) {
    // 原生基线。
    NativeDb native(temp.File("native_insert.db"));
    if (native.get() == nullptr) {
        return;
    }
    native.Exec(
        "CREATE TABLE samples (id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "value INTEGER NOT NULL, tag TEXT NOT NULL)");
    const double native_ms = NativeInsert(native, rows);
    const std::int64_t native_sum =
        QueryInt64(native.get(), "SELECT SUM(value) FROM samples");
    std::printf("[insert] native rows=%lld sum=%lld\n",
                static_cast<long long>(rows),
                static_cast<long long>(native_sum));

    // ORM。
    auto orm_opened = DB::Open(temp.File("orm_insert.db"));
    if (!orm_opened) {
        std::fprintf(stderr, "orm open failed\n");
        return;
    }
    std::unique_ptr<DB> orm_db = std::move(orm_opened.Value());
    orm_db->SetJournalMode("WAL");
    orm_db->SetSynchronous("NORMAL");
    Session session(*orm_db);
    Migration migration(*orm_db);
    migration.AddCreateTable<Sample>(1, "create samples");
    if (Error error = migration.Up(); error) {
        std::fprintf(stderr, "orm schema failed: %s\n",
                     error.Message().c_str());
        return;
    }
    std::int64_t orm_sum = 0;
    const double orm_ms = OrmInsert(session, rows, &orm_sum);
    const std::int64_t stored_sum =
        QueryInt64(orm_db->Handle(), "SELECT SUM(value) FROM samples");
    std::printf("[insert] orm    rows=%lld sum=%lld\n",
                static_cast<long long>(rows),
                static_cast<long long>(stored_sum));
    Report("insert-1e6-file", rows, orm_ms, native_ms, stored_sum);

    // db.hpp 层对照（同一封装，不经 ORM）：把 ORM 额外开销拆成
    // 「db.hpp 封装层」与「ORM 层（表状态/锁/回调/Result）」两段。
    {
        auto layer_opened = DB::Open(temp.File("db_layer_insert.db"));
        if (layer_opened) {
            std::unique_ptr<DB> layer_db = std::move(layer_opened.Value());
            layer_db->SetJournalMode("WAL");
            layer_db->SetSynchronous("NORMAL");
            (void)layer_db->Exec(
                "CREATE TABLE samples (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                "value INTEGER NOT NULL, tag TEXT NOT NULL)");
            const double layer_ms = DbLayerInsert(*layer_db, rows);
            const std::int64_t layer_sum = QueryInt64(
                layer_db->Handle(), "SELECT SUM(value) FROM samples");
            if (layer_ms >= 0.0 && layer_sum == native_sum) {
                Report("insert-db-layer", rows, layer_ms, native_ms,
                       layer_sum);
            }
        }
    }

    // 语句缓存前/后：1e5 行子样本，避免无缓存路径耗时过长。
    const std::int64_t probe_rows = rows / 10 > 0 ? rows / 10 : rows;
    NativeDb probe_native(temp.File("native_nocache.db"));
    if (probe_native.get() != nullptr) {
        probe_native.Exec(
            "CREATE TABLE samples (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "value INTEGER NOT NULL, tag TEXT NOT NULL)");
        const double prepare_each_ms = NativeInsertPrepareEach(probe_native,
                                                              probe_rows);
        auto probe_db_opened = DB::Open(temp.File("orm_nocache.db"));
        if (probe_db_opened) {
            std::unique_ptr<DB> probe_db = std::move(probe_db_opened.Value());
            probe_db->SetJournalMode("WAL");
            probe_db->SetSynchronous("NORMAL");
            Session probe_session(*probe_db);
            Migration probe_migration(*probe_db);
            probe_migration.AddCreateTable<Sample>(1, "create samples");
            (void)probe_migration.Up();
            std::int64_t ignored = 0;
            const double no_cache_ms =
                OrmInsertNoCache(probe_session, probe_rows, &ignored);
            Report("insert-no-cache", probe_rows, no_cache_ms,
                   prepare_each_ms, ignored);
        }
    }
}

// ===========================================================================
// 2/3. 查询与扫描（内存库）
// ===========================================================================

/// @brief 原生：1e6 次主键查询，只读 value（SQLite 纯查询下限）。
double NativeFindPure(NativeDb& db, std::int64_t rows, std::int64_t* sum) {
    sqlite3_stmt* statement =
        db.Prepare("SELECT id, value, tag FROM samples WHERE id = ?");
    const auto start = Clock::now();
    std::int64_t total = 0;
    for (std::int64_t index = 1; index <= rows; ++index) {
        sqlite3_bind_int64(statement, 1, index);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            total += sqlite3_column_int64(statement, 1);
        }
        sqlite3_reset(statement);
    }
    const double ms = ElapsedMs(start);
    sqlite3_finalize(statement);
    *sum = total;
    return ms;
}

/// @brief 原生：主键查询 + 构造 Sample（含 std::string 拷贝）。
double NativeFindObject(NativeDb& db, std::int64_t rows, std::int64_t* sum) {
    sqlite3_stmt* statement =
        db.Prepare("SELECT id, value, tag FROM samples WHERE id = ?");
    const auto start = Clock::now();
    std::int64_t total = 0;
    for (std::int64_t index = 1; index <= rows; ++index) {
        sqlite3_bind_int64(statement, 1, index);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            Sample row;
            row.id = sqlite3_column_int64(statement, 0);
            row.value = sqlite3_column_int64(statement, 1);
            const auto* text =
                reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
            row.tag = text != nullptr ? text : "";
            total += row.value;
        }
        sqlite3_reset(statement);
    }
    const double ms = ElapsedMs(start);
    sqlite3_finalize(statement);
    *sum = total;
    return ms;
}

/// @brief ORM：1e6 次 Find<T>。
double OrmFind(Session& session, std::int64_t rows, std::int64_t* sum) {
    const auto start = Clock::now();
    std::int64_t total = 0;
    for (std::int64_t index = 1; index <= rows; ++index) {
        auto found = session.Find<Sample>(index);
        if (!found.ok()) {
            std::fprintf(stderr, "orm find failed: %s\n",
                         found.Err().Message().c_str());
            break;
        }
        total += found.Value().value;
    }
    const double ms = ElapsedMs(start);
    *sum = total;
    return ms;
}

/// @brief 原生：全表扫描，只累加 value。
double NativeScan(NativeDb& db, std::int64_t* sum) {
    sqlite3_stmt* statement =
        db.Prepare("SELECT id, value, tag FROM samples");
    const auto start = Clock::now();
    std::int64_t total = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        total += sqlite3_column_int64(statement, 1);
    }
    const double ms = ElapsedMs(start);
    sqlite3_finalize(statement);
    *sum = total;
    return ms;
}

/// @brief ORM：全表扫描（Each，不物化）。
double OrmScanEach(Session& session, std::int64_t* sum) {
    const auto start = Clock::now();
    std::int64_t total = 0;
    auto walked = session.Query<Sample>().Each(
        [&total](const Sample& row) { total += row.value; });
    const double ms = ElapsedMs(start);
    if (!walked) {
        std::fprintf(stderr, "orm scan failed: %s\n",
                     walked.Err().Message().c_str());
    }
    *sum = total;
    return ms;
}

/// @brief ORM：全表扫描并物化 vector。
double OrmScanAll(Session& session, std::int64_t* sum) {
    const auto start = Clock::now();
    auto rows = session.Query<Sample>().All();
    const double ms = ElapsedMs(start);
    std::int64_t total = 0;
    if (rows.ok()) {
        for (const Sample& row : rows.Value()) {
            total += row.value;
        }
    } else {
        std::fprintf(stderr, "orm all failed: %s\n",
                     rows.Err().Message().c_str());
    }
    *sum = total;
    return ms;
}

/// @brief 准备内存库（原生写入 rows 行）。
bool FillMemoryDb(NativeDb& db, std::int64_t rows) {
    db.Exec(
        "CREATE TABLE samples (id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "value INTEGER NOT NULL, tag TEXT NOT NULL)");
    db.Exec("BEGIN IMMEDIATE");
    sqlite3_stmt* statement =
        db.Prepare("INSERT INTO samples (value, tag) VALUES (?, ?)");
    for (std::int64_t index = 0; index < rows; ++index) {
        sqlite3_bind_int64(statement, 1, ValueOf(index));
        sqlite3_bind_text(statement, 2, TagOf(index), -1, SQLITE_STATIC);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);
    db.Exec("COMMIT");
    return QueryInt64(db.get(), "SELECT COUNT(*) FROM samples") == rows;
}

void BenchQuery(std::int64_t rows) {
    NativeDb native(":memory:");
    if (native.get() == nullptr || !FillMemoryDb(native, rows)) {
        std::fprintf(stderr, "native memory db prepare failed\n");
        return;
    }
    auto orm_opened = DB::Open(":memory:");
    if (!orm_opened) {
        return;
    }
    std::unique_ptr<DB> orm_db = std::move(orm_opened.Value());
    Session session(*orm_db);
    Migration migration(*orm_db);
    migration.AddCreateTable<Sample>(1, "create samples");
    if (Error error = migration.Up(); error) {
        return;
    }
    {
        // 用 ORM 灌数据，保证两边初始状态一致。
        const Error error = session.Transaction([&]() -> Error {
            Sample row;
            for (std::int64_t index = 0; index < rows; ++index) {
                row = Sample{};
                row.value = ValueOf(index);
                row.tag = TagOf(index);
                if (auto inserted = session.Insert(row); !inserted) {
                    return inserted.Err();
                }
            }
            return {};
        });
        if (error) {
            return;
        }
    }

    std::int64_t native_sum = 0;
    std::int64_t object_sum = 0;
    std::int64_t orm_sum = 0;
    const double native_find_ms = NativeFindPure(native, rows, &native_sum);
    const double object_find_ms =
        NativeFindObject(native, rows, &object_sum);
    const double orm_find_ms = OrmFind(session, rows, &orm_sum);
    std::printf("[find] native=%lld object=%lld orm=%lld\n",
                static_cast<long long>(native_sum),
                static_cast<long long>(object_sum),
                static_cast<long long>(orm_sum));
    Report("find-pk-memory", rows, orm_find_ms, object_find_ms, orm_sum);
    std::printf(
        "  [breakdown] native-pure=%.1f ns/op  +object=%.1f ns/op  "
        "orm-extra=%+.1f ns/op\n",
        native_find_ms * 1e6 / static_cast<double>(rows),
        object_find_ms * 1e6 / static_cast<double>(rows),
        (orm_find_ms - object_find_ms) * 1e6 / static_cast<double>(rows));

    std::int64_t native_scan_sum = 0;
    std::int64_t each_sum = 0;
    std::int64_t all_sum = 0;
    const double native_scan_ms = NativeScan(native, &native_scan_sum);
    const double each_ms = OrmScanEach(session, &each_sum);
    const double all_ms = OrmScanAll(session, &all_sum);
    std::printf("[scan] native=%lld each=%lld all=%lld\n",
                static_cast<long long>(native_scan_sum),
                static_cast<long long>(each_sum),
                static_cast<long long>(all_sum));
    Report("scan-1e6-memory", rows, each_ms, native_scan_ms, each_sum);
    std::printf("  [breakdown] orm-all=%.1f ms (%.1f ns/op)  each=%.1f ms\n",
                all_ms, all_ms * 1e6 / static_cast<double>(rows), each_ms);
}

// ===========================================================================
// 4. 更新 / 删除（内存库）
// ===========================================================================

double NativeUpdate(NativeDb& db, std::int64_t rows) {
    db.Exec("BEGIN IMMEDIATE");
    sqlite3_stmt* statement =
        db.Prepare("UPDATE samples SET value = ?, tag = ? WHERE id = ?");
    const auto start = Clock::now();
    for (std::int64_t index = 1; index <= rows; ++index) {
        sqlite3_bind_int64(statement, 1, ValueOf(index - 1) * 2);
        sqlite3_bind_text(statement, 2, TagOf(index), -1, SQLITE_STATIC);
        sqlite3_bind_int64(statement, 3, index);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    const double ms = ElapsedMs(start);
    db.Exec("COMMIT");
    sqlite3_finalize(statement);
    return ms;
}

double OrmUpdate(Session& session, std::int64_t rows) {
    const auto start = Clock::now();
    const Error error = session.Transaction([&]() -> Error {
        for (std::int64_t index = 1; index <= rows; ++index) {
            Sample row;
            row.id = index;
            row.value = ValueOf(index - 1) * 2;
            row.tag = TagOf(index);
            if (auto updated = session.Update(row); !updated) {
                return updated.Err();
            }
        }
        return {};
    });
    const double ms = ElapsedMs(start);
    if (error) {
        std::fprintf(stderr, "orm update failed: %s\n",
                     error.Message().c_str());
    }
    return ms;
}

double NativeDelete(NativeDb& db, std::int64_t rows) {
    db.Exec("BEGIN IMMEDIATE");
    sqlite3_stmt* statement = db.Prepare("DELETE FROM samples WHERE id = ?");
    const auto start = Clock::now();
    for (std::int64_t index = 1; index <= rows; ++index) {
        sqlite3_bind_int64(statement, 1, index);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    const double ms = ElapsedMs(start);
    db.Exec("COMMIT");
    sqlite3_finalize(statement);
    return ms;
}

double OrmDelete(Session& session, std::int64_t rows) {
    const auto start = Clock::now();
    const Error error = session.Transaction([&]() -> Error {
        for (std::int64_t index = 1; index <= rows; ++index) {
            if (auto removed = session.Delete<Sample>(index); !removed) {
                return removed.Err();
            }
        }
        return {};
    });
    const double ms = ElapsedMs(start);
    if (error) {
        std::fprintf(stderr, "orm delete failed: %s\n",
                     error.Message().c_str());
    }
    return ms;
}

void BenchUpdateDelete(std::int64_t rows) {
    NativeDb native(":memory:");
    if (native.get() == nullptr || !FillMemoryDb(native, rows)) {
        return;
    }
    auto orm_opened = DB::Open(":memory:");
    if (!orm_opened) {
        return;
    }
    std::unique_ptr<DB> orm_db = std::move(orm_opened.Value());
    Session session(*orm_db);
    Migration migration(*orm_db);
    migration.AddCreateTable<Sample>(1, "create samples");
    if (Error error = migration.Up(); error) {
        return;
    }
    {
        const Error error = session.Transaction([&]() -> Error {
            Sample row;
            for (std::int64_t index = 0; index < rows; ++index) {
                row = Sample{};
                row.value = ValueOf(index);
                row.tag = TagOf(index);
                if (auto inserted = session.Insert(row); !inserted) {
                    return inserted.Err();
                }
            }
            return {};
        });
        if (error) {
            return;
        }
    }

    const double native_update_ms = NativeUpdate(native, rows);
    const double orm_update_ms = OrmUpdate(session, rows);
    const std::int64_t update_sum =
        QueryInt64(orm_db->Handle(), "SELECT SUM(value) FROM samples");
    const std::int64_t native_update_sum =
        QueryInt64(native.get(), "SELECT SUM(value) FROM samples");
    if (update_sum != native_update_sum) {
        std::fprintf(stderr, "update checksum mismatch: %lld vs %lld\n",
                     static_cast<long long>(update_sum),
                     static_cast<long long>(native_update_sum));
    }
    Report("update-1e6-memory", rows, orm_update_ms, native_update_ms,
           update_sum);

    const double native_delete_ms = NativeDelete(native, rows);
    const double orm_delete_ms = OrmDelete(session, rows);
    const std::int64_t remaining =
        QueryInt64(orm_db->Handle(), "SELECT COUNT(*) FROM samples");
    const std::int64_t native_remaining =
        QueryInt64(native.get(), "SELECT COUNT(*) FROM samples");
    if (remaining != 0 || native_remaining != 0) {
        std::fprintf(stderr, "delete leftover: orm=%lld native=%lld\n",
                     static_cast<long long>(remaining),
                     static_cast<long long>(native_remaining));
    }
    Report("delete-1e6-memory", rows, orm_delete_ms, native_delete_ms,
           remaining);
}

} // namespace

int main(int argc, char** argv) {
    std::int64_t rows = 1000000;
    if (argc >= 2) {
        rows = std::strtoll(argv[1], nullptr, 10);
        if (rows <= 0) {
            rows = 1000000;
        }
    }
    std::printf(
        "go2cpp_storage_orm_bench rows=%lld sqlite=%s\n",
        static_cast<long long>(rows), sqlite3_libversion());

    TempDir temp;
    BenchInsert(rows, temp);
    BenchQuery(rows);
    BenchUpdateDelete(rows);
    std::printf("done\n");
    return 0;
}
