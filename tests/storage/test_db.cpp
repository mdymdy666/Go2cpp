/**
 * @file tests/storage/test_db.cpp
 * @brief SQLite 存储层测试：CRUD / 事务 / 预编译复用 / 并发 WAL / 持久化 / 边界。
 *
 * 独立可执行（不并入 go2cpp_tests），由 cmake/Go2CppStorage.cmake 在
 * `GO2CPP_WITH_SQLITE=ON` 时构建并注册为 ctest 用例 `go2cpp_storage_db_test`。
 *
 * 覆盖：
 *  1. Open/Close/PRAGMA 与外键；
 *  2. CRUD：1e6 行插入、全表扫描、1e6 行 UPDATE、1e6 行 DELETE；
 *  3. 事务：提交、回滚、嵌套拒绝、无事务 COMMIT、RAII 自动回滚；
 *  4. 预编译复用：同一 Stmt 1e6 次 Bind/Step/Reset；
 *  5. 并发：4 写 + 2 读，WAL，busy_timeout 排队；
 *  6. 持久化：关闭再打开，数据逐字节一致；
 *  7. 边界：空表、NULL、1MiB 文本/blob、非法 SQL、参数个数、约束冲突、
 *     并发写锁 BUSY、关闭后调用、多语句、空 SQL。
 *
 * 用法：`go2cpp_storage_db_test`（无参数，退出码 = 失败断言数）。
 */
#include "go2cpp/storage/db.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using go2cpp::storage::Blob;
using go2cpp::storage::Code;
using go2cpp::storage::DB;
using go2cpp::storage::Error;
using go2cpp::storage::Rows;
using go2cpp::storage::Stmt;

std::atomic<int> g_failures{0};
std::mutex g_output_mutex;

/// @brief 断言失败时记录文件/行号并继续（一次运行收集全部失败）。
#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            g_failures.fetch_add(1);                                         \
            std::lock_guard<std::mutex> lock(g_output_mutex);                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #condition);                                        \
        }                                                                    \
    } while (false)

/// @brief 断言错误码。
#define CHECK_CODE(expr, expected)                                           \
    do {                                                                     \
        const ::go2cpp::storage::Error error_ = (expr);                      \
        if (error_.Code() != static_cast<int>(expected)) {                   \
            g_failures.fetch_add(1);                                         \
            std::lock_guard<std::mutex> lock(g_output_mutex);                \
            std::fprintf(stderr, "FAIL %s:%d: %s code=%d expected=%d (%s)\n",\
                         __FILE__, __LINE__, #expr, error_.Code(),           \
                         static_cast<int>(expected),                         \
                         error_.Message().c_str());                          \
        }                                                                    \
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
                 ("go2cpp_storage_" + std::to_string(stamp) + "_" +
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

/// @brief 打开文件库并设置测试用 PRAGMA；失败时记录断言并返回空指针。
std::unique_ptr<DB> OpenFileDb(const std::string& path) {
    auto opened = DB::Open(path);
    CHECK(opened.ok());
    if (!opened.ok()) {
        return nullptr;
    }
    std::unique_ptr<DB> db = std::move(opened.Value());
    CHECK(db->SetJournalMode("WAL").ok());
    CHECK(db->SetSynchronous("NORMAL").ok());
    CHECK(db->SetBusyTimeout(15000).ok());
    return db;
}

/// @brief 把表 t(id, v, name) 按 id 序列化为确定字节串，用于逐字节比较。
std::string DumpTable(DB& db) {
    std::string out;
    auto rows = db.Query("SELECT id, v, name FROM t ORDER BY id");
    CHECK(rows.ok());
    if (!rows.ok()) {
        return out;
    }
    while (rows.Value().Next()) {
        const std::int64_t id = rows.Value().Int64(0);
        const std::int64_t v = rows.Value().Int64(1);
        const std::string name = rows.Value().Text(2);
        const std::uint32_t name_size = static_cast<std::uint32_t>(name.size());
        out.append(reinterpret_cast<const char*>(&id), sizeof(id));
        out.append(reinterpret_cast<const char*>(&v), sizeof(v));
        out.append(reinterpret_cast<const char*>(&name_size),
                   sizeof(name_size));
        out.append(name);
    }
    CHECK(rows.Value().Err().ok());
    return out;
}

// ---------------------------------------------------------------------------
// 1. 打开/PRAGMA/外键
// ---------------------------------------------------------------------------
void TestOpenAndPragmas() {
    const auto start = Clock::now();
    TempDir dir;
    auto db = OpenFileDb(dir.File("basic.db"));
    if (db == nullptr) {
        return;
    }

    CHECK(db->IsOpen());
    CHECK(db->Path() == dir.File("basic.db"));
    CHECK(db->Handle() != nullptr);

    // journal_mode 应回报 "wal"。
    {
        auto rows = db->Query("PRAGMA journal_mode");
        CHECK(rows.ok());
        CHECK(rows.Value().Next());
        CHECK(rows.Value().Text(0) == "wal");
    }

    CHECK(db->SetForeignKeys(true).ok());
    CHECK(db->Exec("CREATE TABLE parent(id INTEGER PRIMARY KEY)").ok());
    CHECK(db->Exec("CREATE TABLE child(id INTEGER PRIMARY KEY, "
                   "pid INTEGER REFERENCES parent(id))")
              .ok());
    CHECK(db->Exec("INSERT INTO parent(id) VALUES(1)").ok());
    const auto orphan = db->Exec("INSERT INTO child(id, pid) VALUES(2, 99)");
    CHECK_CODE(orphan.Err(), Code::kConstraint);
    CHECK(db->Exec("INSERT INTO child(id, pid) VALUES(2, 1)").ok());

    // 未知 PRAGMA 取值直接拒绝，不拼进 SQL。
    CHECK_CODE(db->SetJournalMode("WAL; DROP TABLE parent"),
               Code::kRange);
    CHECK_CODE(db->SetSynchronous("fastest"), Code::kRange);
    CHECK_CODE(db->SetBusyTimeout(-1), Code::kRange);

    CHECK(db->Close().ok());
    CHECK(!db->IsOpen());
    CHECK(db->Close().ok());

    std::printf("[test] %-34s ok (%lld ms)\n", "open/pragmas/fk",
                static_cast<long long>(ElapsedMs(start)));
}

// ---------------------------------------------------------------------------
// 2. CRUD：1e6 行插入 / 扫描 / 更新 / 删除
// ---------------------------------------------------------------------------
void TestCrudMillion() {
    constexpr int kRows = 1000000;
    const auto start = Clock::now();
    TempDir dir;
    auto db = OpenFileDb(dir.File("crud.db"));
    if (db == nullptr) {
        return;
    }
    CHECK(db->Exec("CREATE TABLE crud(id INTEGER PRIMARY KEY, v INTEGER, "
                   "name TEXT)")
              .ok());

    // ---- Create：单事务 + 预编译语句 ----
    const auto insert_start = Clock::now();
    {
        CHECK(db->Begin().ok());
        auto stmt = db->Prepare("INSERT INTO crud(id, v, name) VALUES(?, ?, ?)");
        CHECK(stmt.ok());
        if (!stmt.ok()) {
            return;
        }
        for (int i = 0; i < kRows; ++i) {
            CHECK(stmt.Value().Bind(i, i * 2, "abcdefgh").ok());
            const Error error = stmt.Value().Step();
            CHECK(error.ok());
            CHECK(stmt.Value().Done());
            stmt.Value().Reset();
        }
        CHECK(db->Commit().ok());
    }
    const auto insert_ms = ElapsedMs(insert_start);
    CHECK(db->Changes() >= 0);
    {
        auto count = db->Query("SELECT COUNT(*) FROM crud");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == kRows);
    }

    // ---- Read：全表扫描 1e6 行 ----
    const auto read_start = Clock::now();
    {
        auto rows = db->Query("SELECT id, v, name FROM crud ORDER BY id");
        CHECK(rows.ok());
        std::int64_t seen = 0;
        bool content_ok = true;
        while (rows.Value().Next()) {
            if (rows.Value().Int64(0) != seen ||
                rows.Value().Int64(1) != seen * 2 ||
                rows.Value().TextView(2) != std::string_view("abcdefgh")) {
                content_ok = false;
            }
            ++seen;
        }
        CHECK(rows.Value().Err().ok());
        CHECK(seen == kRows);
        CHECK(content_ok);
    }
    const auto read_ms = ElapsedMs(read_start);

    // ---- Update：1e6 行，一条语句一个事务 ----
    const auto update_start = Clock::now();
    {
        CHECK(db->Begin().ok());
        const auto updated = db->Exec("UPDATE crud SET v = v + 1");
        CHECK(updated.ok());
        CHECK(updated.Value().changes == kRows);
        CHECK(db->Commit().ok());
    }
    const auto update_ms = ElapsedMs(update_start);
    {
        auto first = db->Query("SELECT v FROM crud WHERE id = 0");
        CHECK(first.ok());
        CHECK(first.Value().Next());
        CHECK(first.Value().Int64(0) == 1);
        auto rows = db->Query("SELECT SUM(v) FROM crud");
        CHECK(rows.ok());
        CHECK(rows.Value().Next());
        // sum(i*2+1) for i in [0, 1e6) = 2*sum(i) + 1e6 = 1e12。
        CHECK(rows.Value().Int64(0) == 1000000000000LL);
    }

    // ---- Delete：1e6 行 ----
    const auto delete_start = Clock::now();
    {
        CHECK(db->Begin().ok());
        const auto deleted = db->Exec("DELETE FROM crud");
        CHECK(deleted.ok());
        CHECK(deleted.Value().changes == kRows);
        CHECK(db->Commit().ok());
    }
    const auto delete_ms = ElapsedMs(delete_start);
    {
        auto count = db->Query("SELECT COUNT(*) FROM crud");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == 0);
    }

    std::printf(
        "[test] %-34s ok (%lld ms: insert=%lld read=%lld update=%lld "
        "delete=%lld)\n",
        "crud-1e6", static_cast<long long>(ElapsedMs(start)),
        static_cast<long long>(insert_ms), static_cast<long long>(read_ms),
        static_cast<long long>(update_ms),
        static_cast<long long>(delete_ms));
    std::printf("[perf] insert-1e6 %.0f rows/s, read-1e6 %.0f rows/s\n",
                insert_ms > 0 ? kRows * 1000.0 / insert_ms : 0.0,
                read_ms > 0 ? kRows * 1000.0 / read_ms : 0.0);
}

// ---------------------------------------------------------------------------
// 3. 事务：提交 / 回滚 / 嵌套拒绝 / RAII
// ---------------------------------------------------------------------------
void TestTransactions() {
    const auto start = Clock::now();
    TempDir dir;
    auto db = OpenFileDb(dir.File("tx.db"));
    if (db == nullptr) {
        return;
    }
    CHECK(db->Exec("CREATE TABLE tx(a INTEGER)").ok());

    // 回滚。
    CHECK(db->Begin().ok());
    CHECK(db->InTransaction());
    CHECK_CODE(db->Begin(), Code::kMisuse); // 嵌套拒绝
    CHECK(db->Exec("INSERT INTO tx VALUES(1)").ok());
    CHECK(db->Rollback().ok());
    CHECK(!db->InTransaction());
    CHECK_CODE(db->Rollback(), Code::kMisuse); // 无事务
    {
        auto count = db->Query("SELECT COUNT(*) FROM tx");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == 0);
    }

    // 提交。
    CHECK(db->Begin().ok());
    for (int i = 0; i < 100; ++i) {
        CHECK(db->Exec("INSERT INTO tx VALUES(?)", i).ok());
    }
    CHECK(db->Commit().ok());
    CHECK_CODE(db->Commit(), Code::kMisuse); // 无事务
    {
        auto count = db->Query("SELECT COUNT(*) FROM tx");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == 100);
    }

    // RAII：作用域结束未提交 → 自动回滚。
    {
        go2cpp::storage::Transaction tx(*db);
        CHECK(tx.Begin().ok());
        CHECK(tx.Active());
        CHECK(db->Exec("INSERT INTO tx VALUES(1000)").ok());
        CHECK_CODE(tx.Begin(), Code::kMisuse);
    }
    CHECK(!db->InTransaction());
    {
        auto count = db->Query("SELECT COUNT(*) FROM tx");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == 100);
    }

    // RAII：显式提交。
    {
        go2cpp::storage::Transaction tx(*db);
        CHECK(tx.Begin().ok());
        CHECK(db->Exec("INSERT INTO tx VALUES(2000)").ok());
        CHECK(tx.Commit().ok());
        CHECK(!tx.Active());
        CHECK_CODE(tx.Commit(), Code::kMisuse);
    }
    {
        auto count = db->Query("SELECT COUNT(*) FROM tx");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == 101);
    }

    // 事务中约束错误：语句失败、事务仍可回滚。
    CHECK(db->Exec("CREATE TABLE uniq(u INTEGER UNIQUE)").ok());
    CHECK(db->Begin().ok());
    CHECK(db->Exec("INSERT INTO uniq VALUES(1)").ok());
    CHECK_CODE(db->Exec("INSERT INTO uniq VALUES(1)").Err(),
               Code::kConstraint);
    CHECK(db->Rollback().ok());
    {
        auto count = db->Query("SELECT COUNT(*) FROM uniq");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == 0);
    }

    std::printf("[test] %-34s ok (%lld ms)\n", "transactions",
                static_cast<long long>(ElapsedMs(start)));
}

// ---------------------------------------------------------------------------
// 4. 预编译复用：1e6 次 Bind/Step/Reset（内存库）
// ---------------------------------------------------------------------------
void TestPreparedReuseMillion() {
    constexpr int kRows = 1000000;
    const auto start = Clock::now();
    TempDir dir;
    auto db = OpenFileDb(dir.File("prep.db"));
    if (db == nullptr) {
        return;
    }
    CHECK(db->Exec("CREATE TABLE p(id INTEGER PRIMARY KEY, v INTEGER)").ok());

    auto stmt = db->Prepare("INSERT INTO p(id, v) VALUES(?, ?)");
    CHECK(stmt.ok());
    if (!stmt.ok()) {
        return;
    }
    const auto loop_start = Clock::now();
    CHECK(db->Begin().ok());
    for (int i = 0; i < kRows; ++i) {
        stmt.Value().Reset();
        stmt.Value().ClearBindings();
        CHECK(stmt.Value().Bind(i, i + 1).ok());
        CHECK(stmt.Value().Step().ok());
    }
    CHECK(db->Commit().ok());
    const auto loop_ms = ElapsedMs(loop_start);

    CHECK(stmt.Value().ParameterCount() == 2);
    CHECK(stmt.Value().ColumnCount() == 0);
    {
        auto count = db->Query("SELECT COUNT(*) FROM p");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == kRows);
    }

    // 复用同一条语句做查询：绑定 → 多行遍历 → Reset 再来。
    auto select = db->Prepare("SELECT id, v FROM p WHERE id >= ? ORDER BY id "
                              "LIMIT 3");
    CHECK(select.ok());
    if (select.ok()) {
        CHECK(select.Value().Bind(kRows - 3).ok());
        std::int64_t seen = 0;
        while (select.Value().Step().ok() && select.Value().HasRow()) {
            CHECK(select.Value().ColumnInt64(0) == kRows - 3 + seen);
            ++seen;
        }
        CHECK(seen == 3);
        CHECK(select.Value().Reset().ok());
        CHECK(select.Value().Bind(0).ok());
        CHECK(select.Value().Step().ok());
        CHECK(select.Value().HasRow());
        CHECK(select.Value().ColumnInt64(0) == 0);
    }

    std::printf(
        "[test] %-34s ok (%lld ms, %.3f us/op)\n", "prepared-reuse-1e6",
        static_cast<long long>(ElapsedMs(start)),
        loop_ms > 0 ? loop_ms * 1000.0 / kRows : 0.0);
}

// ---------------------------------------------------------------------------
// 5. 持久化：关闭再打开，数据逐字节一致
// ---------------------------------------------------------------------------
void TestPersistence() {
    const auto start = Clock::now();
    TempDir dir;
    const std::string path = dir.File("persist.db");
    std::string before;
    {
        auto db = OpenFileDb(path);
        if (db == nullptr) {
            return;
        }
        CHECK(db->Exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, "
                       "name TEXT)")
                  .ok());
        CHECK(db->Begin().ok());
        auto stmt = db->Prepare("INSERT INTO t(id, v, name) VALUES(?, ?, ?)");
        CHECK(stmt.ok());
        for (int i = 0; i < 1000; ++i) {
            stmt.Value().Reset();
            CHECK(stmt.Value().Bind(i, i * 7, "row-" + std::to_string(i)).ok());
            CHECK(stmt.Value().Step().ok());
        }
        CHECK(db->Commit().ok());
        before = DumpTable(*db);
        CHECK(db->Close().ok());
    }

    // 关闭后重新打开：数据仍在，且序列化字节完全一致。
    {
        auto db = OpenFileDb(path);
        if (db == nullptr) {
            return;
        }
        const std::string after = DumpTable(*db);
        CHECK(after.size() == before.size());
        CHECK(after == before);
        CHECK(std::memcmp(after.data(), before.data(), before.size()) == 0);
        auto count = db->Query("SELECT COUNT(*) FROM t");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == 1000);
    }

    // 非 WAL 的普通库同样验证一遍（关闭会 checkpoint）。
    const std::string plain = dir.File("plain.db");
    {
        auto db = DB::Open(plain);
        CHECK(db.ok());
        CHECK(db.Value()->Exec("CREATE TABLE x(a INTEGER)").ok());
        CHECK(db.Value()->Exec("INSERT INTO x VALUES(42)").ok());
    }
    {
        auto db = DB::Open(plain);
        CHECK(db.ok());
        auto rows = db.Value()->Query("SELECT a FROM x");
        CHECK(rows.ok());
        CHECK(rows.Value().Next());
        CHECK(rows.Value().Int64(0) == 42);
    }

    std::printf("[test] %-34s ok (%lld ms, %zu bytes identical)\n",
                "persistence", static_cast<long long>(ElapsedMs(start)),
                before.size());
}

// ---------------------------------------------------------------------------
// 6. 并发：4 写 + 2 读，WAL
// ---------------------------------------------------------------------------
void TestConcurrencyWal() {
    const auto start = Clock::now();
    TempDir dir;
    const std::string path = dir.File("concurrent.db");
    constexpr int kWriters = 4;
    constexpr int kRowsPerWriter = 25000;

    {
        auto db = OpenFileDb(path);
        if (db == nullptr) {
            return;
        }
        CHECK(db->Exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER)")
                  .ok());
    }

    std::atomic<int> writer_errors{0};
    std::atomic<std::int64_t> rows_written{0};
    std::vector<std::thread> threads;
    threads.reserve(kWriters + 2);

    for (int writer = 0; writer < kWriters; ++writer) {
        threads.emplace_back([path, writer, &writer_errors, &rows_written]() {
            auto opened = DB::Open(path);
            if (!opened.ok()) {
                writer_errors.fetch_add(1);
                return;
            }
            DB& db = *opened.Value();
            if (!db.SetBusyTimeout(20000).ok() ||
                !db.SetJournalMode("WAL").ok()) {
                writer_errors.fetch_add(1);
                return;
            }
            const int base = writer * kRowsPerWriter;
            if (!db.Begin().ok()) {
                writer_errors.fetch_add(1);
                return;
            }
            auto stmt = db.Prepare("INSERT INTO t(id, v) VALUES(?, ?)");
            if (!stmt.ok()) {
                writer_errors.fetch_add(1);
                return;
            }
            for (int i = 0; i < kRowsPerWriter; ++i) {
                stmt.Value().Reset();
                if (!stmt.Value().Bind(base + i, base + i).ok() ||
                    !stmt.Value().Step().ok()) {
                    writer_errors.fetch_add(1);
                    (void)db.Rollback();
                    return;
                }
            }
            if (!db.Commit().ok()) {
                writer_errors.fetch_add(1);
                return;
            }
            rows_written.fetch_add(kRowsPerWriter);
        });
    }

    // 读线程：WAL 下读不阻塞写，允许读到部分数据。
    std::atomic<int> reader_errors{0};
    for (int reader = 0; reader < 2; ++reader) {
        threads.emplace_back([path, &reader_errors]() {
            auto opened = DB::Open(path);
            if (!opened.ok()) {
                reader_errors.fetch_add(1);
                return;
            }
            DB& db = *opened.Value();
            if (!db.SetBusyTimeout(20000).ok() ||
                !db.SetJournalMode("WAL").ok()) {
                reader_errors.fetch_add(1);
                return;
            }
            for (int i = 0; i < 200; ++i) {
                auto rows = db.Query("SELECT COUNT(*) FROM t");
                if (!rows.ok() || !rows.Value().Next() ||
                    rows.Value().Int64(0) < 0) {
                    reader_errors.fetch_add(1);
                    return;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    CHECK(writer_errors.load() == 0);
    CHECK(reader_errors.load() == 0);
    CHECK(rows_written.load() == kWriters * kRowsPerWriter);

    {
        auto db = OpenFileDb(path);
        if (db == nullptr) {
            return;
        }
        auto count = db->Query("SELECT COUNT(*) FROM t");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == kWriters * kRowsPerWriter);
        auto sum = db->Query("SELECT SUM(v) FROM t");
        CHECK(sum.ok());
        CHECK(sum.Value().Next());
        const std::int64_t n = kWriters * kRowsPerWriter;
        CHECK(sum.Value().Int64(0) == n * (n - 1) / 2);
        auto check = db->Query("PRAGMA integrity_check");
        CHECK(check.ok());
        CHECK(check.Value().Next());
        CHECK(check.Value().Text(0) == "ok");
    }

    std::printf("[test] %-34s ok (%lld ms, %d writers x %d rows + 2 readers)\n",
                "concurrency-wal", static_cast<long long>(ElapsedMs(start)),
                kWriters, kRowsPerWriter);
}

// ---------------------------------------------------------------------------
// 7. 边界：空表/NULL/超长值/非法 SQL/参数/写锁/关闭后
// ---------------------------------------------------------------------------
void TestEdges() {
    const auto start = Clock::now();
    TempDir dir;
    auto db = OpenFileDb(dir.File("edges.db"));
    if (db == nullptr) {
        return;
    }

    // 空表。
    CHECK(db->Exec("CREATE TABLE empty(a INTEGER)").ok());
    {
        auto rows = db->Query("SELECT a FROM empty");
        CHECK(rows.ok());
        CHECK(!rows.Value().Next());
        CHECK(rows.Value().Err().ok());
        CHECK(rows.Value().ColumnCount() == 1);
    }

    // 空 SQL / 纯注释：Exec 成功（无副作用），Query 报错。
    CHECK(db->Exec("").ok());
    CHECK(db->Exec("   \n\t ").ok());
    CHECK(db->Exec("-- only a comment\n").ok());
    CHECK(db->Exec("/* block comment */").ok());
    CHECK_CODE(db->Query("").Err(), Code::kMisuse);
    CHECK_CODE(db->Query("  -- comment\n").Err(), Code::kMisuse);

    // 非法 SQL：语法错误带位置信息。
    {
        const auto error = db->Exec("SELEC * FROM nope").Err();
        CHECK_CODE(error, Code::kError);
        CHECK(!error.Message().empty());
    }
    {
        const auto error = db->Exec("CREATE TABLE bad(a INTEGER").Err();
        CHECK_CODE(error, Code::kError);
    }

    // 参数个数严格匹配。
    CHECK(db->Exec("CREATE TABLE args(a INTEGER, b TEXT)").ok());
    CHECK_CODE(db->Exec("INSERT INTO args VALUES(?, ?)").Err(),
               Code::kMisuse);
    CHECK_CODE(db->Exec("INSERT INTO args VALUES(?, ?)", 1).Err(),
               Code::kMisuse);
    CHECK_CODE(db->Exec("INSERT INTO args VALUES(?, ?)", 1, "x", 3).Err(),
               Code::kMisuse);
    CHECK(db->Exec("INSERT INTO args VALUES(?, ?)", 1, "x").ok());
    CHECK_CODE(db->Query("SELECT 1", 1).Err(), Code::kMisuse);

    // NULL / 类型 / int64 边界。
    CHECK(db->Exec("CREATE TABLE types(i INTEGER, d REAL, s TEXT, b BLOB)")
              .ok());
    CHECK(db->Exec("INSERT INTO types VALUES(NULL, NULL, NULL, NULL)").ok());
    {
        auto rows = db->Query("SELECT i, d, s, b FROM types");
        CHECK(rows.ok());
        CHECK(rows.Value().Next());
        for (int column = 0; column < 4; ++column) {
            CHECK(rows.Value().IsNull(column));
            CHECK(rows.Value().ColumnType(column) == 5); // SQLITE_NULL
        }
        CHECK(rows.Value().Int64(0) == 0);
        CHECK(rows.Value().Text(2).empty());
    }
    const std::int64_t min_i64 = std::numeric_limits<std::int64_t>::min();
    const std::int64_t max_i64 = std::numeric_limits<std::int64_t>::max();
    CHECK(db->Exec("DELETE FROM types").ok());
    CHECK(db->Exec("INSERT INTO types(i, d, s) VALUES(?, ?, ?)", min_i64,
                   1.5, nullptr)
              .ok());
    CHECK(db->Exec("INSERT INTO types(i, d, s) VALUES(?, ?, ?)", max_i64,
                   -0.0, "text")
              .ok());
    {
        auto rows = db->Query("SELECT i, d, s FROM types ORDER BY i");
        CHECK(rows.ok());
        CHECK(rows.Value().Next());
        CHECK(rows.Value().Int64(0) == min_i64);
        CHECK(rows.Value().Double(1) == 1.5);
        CHECK(rows.Value().IsNull(2));
        CHECK(rows.Value().Next());
        CHECK(rows.Value().Int64(0) == max_i64);
        CHECK(rows.Value().Text(2) == "text");
        CHECK(!rows.Value().Next());
    }

    // 超长文本 / blob：1 MiB，逐字节往返。
    {
        std::string big_text(1024 * 1024, 'x');
        big_text[0] = '\0';
        big_text[big_text.size() - 1] = '\0'; // 内嵌 NUL 也必须保真
        std::string big_blob(1024 * 1024, '\0');
        for (std::size_t i = 0; i < big_blob.size(); ++i) {
            big_blob[i] = static_cast<char>(i % 251);
        }
        CHECK(db->Exec("DELETE FROM types").ok());
        CHECK(db->Exec("INSERT INTO types(s, b) VALUES(?, ?)", big_text,
                       Blob(big_blob.data(), big_blob.size()))
                  .ok());
        auto rows = db->Query("SELECT s, b FROM types");
        CHECK(rows.ok());
        CHECK(rows.Value().Next());
        const std::string read_text = rows.Value().Text(0);
        CHECK(read_text.size() == big_text.size());
        CHECK(std::memcmp(read_text.data(), big_text.data(), big_text.size()) ==
              0);
        const Blob read_blob = rows.Value().BlobValue(1);
        CHECK(read_blob.size == big_blob.size());
        CHECK(std::memcmp(read_blob.data, big_blob.data(), big_blob.size()) ==
              0);
    }

    // 约束冲突。
    CHECK(db->Exec("CREATE TABLE uniq(u INTEGER UNIQUE)").ok());
    CHECK(db->Exec("INSERT INTO uniq VALUES(1)").ok());
    CHECK_CODE(db->Exec("INSERT INTO uniq VALUES(1)").Err(),
               Code::kConstraint);
    CHECK(db->Exec("CREATE TABLE nn(a INTEGER NOT NULL)").ok());
    CHECK_CODE(db->Exec("INSERT INTO nn(a) VALUES(NULL)").Err(),
               Code::kConstraint);

    // 多语句脚本（无参数）。
    CHECK(db->Exec("CREATE TABLE ms(a INTEGER); INSERT INTO ms VALUES(1); "
                   "INSERT INTO ms VALUES(2);")
              .ok());
    {
        auto count = db->Query("SELECT COUNT(*) FROM ms");
        CHECK(count.ok());
        CHECK(count.Value().Next());
        CHECK(count.Value().Int64(0) == 2);
    }
    // 带参数时不允许多语句。
    CHECK_CODE(db->Exec("INSERT INTO ms VALUES(?); INSERT INTO ms VALUES(9);",
                        5)
                   .Err(),
               Code::kMisuse);
    // Query 只接受单条语句。
    CHECK_CODE(db->Query("SELECT 1; SELECT 2").Err(), Code::kMisuse);
    // Prepare 只接受单条语句。
    CHECK_CODE(db->Prepare("SELECT 1; SELECT 2").Err(), Code::kMisuse);

    // 并发写锁：A 持有写事务，B 的 busy_timeout=0 立即 BUSY。
    {
        auto locker = OpenFileDb(dir.File("lock.db"));
        CHECK(locker != nullptr);
        if (locker != nullptr) {
            CHECK(locker->Exec("CREATE TABLE lock_t(a INTEGER)").ok());
            auto other = DB::Open(dir.File("lock.db"));
            CHECK(other.ok());
            if (other.ok()) {
                DB& second = *other.Value();
                CHECK(second.SetBusyTimeout(0).ok());
                auto begin_first = locker->Begin();
                CHECK(begin_first.ok());
                const auto busy = second.Begin();
                CHECK(!busy.ok());
                CHECK(busy.IsBusy());
                CHECK_CODE(busy, Code::kBusy);
                CHECK(locker->Rollback().ok());
                // 锁释放后 B 可以再拿。
                CHECK(second.Begin().ok());
                CHECK(second.Rollback().ok());
            }
        }
    }

    // 关闭后调用：返回 kMisuse，不崩溃。
    {
        auto closed = DB::Open(dir.File("closed.db"));
        CHECK(closed.ok());
        CHECK(closed.Value()->Close().ok());
        CHECK_CODE(closed.Value()->Exec("SELECT 1").Err(), Code::kMisuse);
        CHECK_CODE(closed.Value()->Query("SELECT 1").Err(), Code::kMisuse);
        CHECK_CODE(closed.Value()->Begin(), Code::kMisuse);
        CHECK_CODE(closed.Value()->Prepare("SELECT 1").Err(),
                   Code::kMisuse);
        CHECK(closed.Value()->LastInsertRowId() == 0);
        CHECK(closed.Value()->Changes() == 0);
        CHECK(!closed.Value()->IsOpen());
        CHECK(closed.Value()->Handle() == nullptr);
        // 关闭后再 Close 幂等。
        CHECK(closed.Value()->Close().ok());
    }

    // 打开不存在的目录：CANTOPEN。
    {
        auto opened = DB::Open(dir.File("no_such_dir/x.db"));
        CHECK(!opened.ok());
        CHECK(opened.Err().Code() != 0);
    }

    std::printf("[test] %-34s ok (%lld ms)\n", "edges",
                static_cast<long long>(ElapsedMs(start)));
}

// ---------------------------------------------------------------------------
// 8. 值类型与列元数据
// ---------------------------------------------------------------------------
void TestValuesAndColumns() {
    const auto start = Clock::now();
    auto db = DB::Open(":memory:");
    CHECK(db.ok());
    if (!db.ok()) {
        return;
    }
    DB& handle = *db.Value();

    CHECK(handle.Exec("CREATE TABLE v(i INTEGER, r REAL, t TEXT, b BLOB, "
                      "n INTEGER)")
              .ok());
    const std::string blob_bytes("\x00\x01\x02\xff", 4);
    CHECK(handle
              .Exec("INSERT INTO v(i, r, t, b, n) VALUES(?, ?, ?, ?, ?)", 42,
                    3.25, "héllo", Blob(blob_bytes.data(), blob_bytes.size()),
                    nullptr)
              .ok());

    // 逐列访问 + 列名 + 参数计数。
    auto stmt = handle.Prepare("SELECT i, r, t, b, n FROM v WHERE i = ?");
    CHECK(stmt.ok());
    if (!stmt.ok()) {
        return;
    }
    CHECK(stmt.Value().ParameterCount() == 1);
    CHECK(stmt.Value().ColumnCount() == 5);
    CHECK(stmt.Value().Bind(42).ok());
    CHECK(stmt.Value().Step().ok());
    CHECK(stmt.Value().HasRow());
    CHECK(std::strcmp(stmt.Value().ColumnName(0), "i") == 0);
    CHECK(std::strcmp(stmt.Value().ColumnName(2), "t") == 0);
    CHECK(stmt.Value().ColumnInt64(0) == 42);
    CHECK(stmt.Value().ColumnDouble(1) == 3.25);
    CHECK(stmt.Value().ColumnText(2) == "héllo");
    CHECK(stmt.Value().ColumnType(0) == 1); // SQLITE_INTEGER
    CHECK(stmt.Value().ColumnType(1) == 2); // SQLITE_REAL
    CHECK(stmt.Value().ColumnType(2) == 3); // SQLITE_TEXT
    CHECK(stmt.Value().ColumnType(3) == 4); // SQLITE_BLOB
    CHECK(stmt.Value().ColumnType(4) == 5); // SQLITE_NULL
    {
        const Blob blob = stmt.Value().ColumnBlob(3);
        CHECK(blob.size == blob_bytes.size());
        CHECK(std::memcmp(blob.data, blob_bytes.data(), blob_bytes.size()) ==
              0);
    }
    CHECK(stmt.Value().IsNull(4));
    CHECK(stmt.Value().Step().ok()); // 第二步：DONE 也是成功
    CHECK(stmt.Value().Done());

    // bool 绑定为 1/0。
    CHECK(handle.Exec("CREATE TABLE flag(b INTEGER)").ok());
    CHECK(handle.Exec("INSERT INTO flag VALUES(?)", true).ok());
    CHECK(handle.Exec("INSERT INTO flag VALUES(?)", false).ok());
    {
        auto rows = handle.Query("SELECT b FROM flag ORDER BY b DESC");
        CHECK(rows.ok());
        CHECK(rows.Value().Next());
        CHECK(rows.Value().Int64(0) == 1);
        CHECK(rows.Value().Next());
        CHECK(rows.Value().Int64(0) == 0);
        // 遍历结束后重复 Next：返回 false 且不产生新错误。
        CHECK(!rows.Value().Next());
        CHECK(!rows.Value().Next());
        CHECK(rows.Value().Err().ok());
    }

    // 结果集列名越界与提前 Close。
    {
        auto rows = handle.Query("SELECT 1 AS one, 'two' AS two");
        CHECK(rows.ok());
        CHECK(rows.Value().ColumnCount() == 2);
        CHECK(std::strcmp(rows.Value().ColumnName(0), "one") == 0);
        CHECK(std::strcmp(rows.Value().ColumnName(1), "two") == 0);
        CHECK(rows.Value().Close().ok());
        CHECK_CODE(rows.Value().Err(), Code::kOk);
        CHECK(!rows.Value().Next());
    }

    std::printf("[test] %-34s ok (%lld ms)\n", "values/columns",
                static_cast<long long>(ElapsedMs(start)));
}

} // namespace

int main() {
    std::printf("=== go2cpp storage (SQLite) tests ===\n");
    TestOpenAndPragmas();
    TestTransactions();
    TestValuesAndColumns();
    TestEdges();
    TestPreparedReuseMillion();
    TestPersistence();
    TestCrudMillion();
    TestConcurrencyWal();
    const int failures = g_failures.load();
    std::printf("=== %s (%d failure%s) ===\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
