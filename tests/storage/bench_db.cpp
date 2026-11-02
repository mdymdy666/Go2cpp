/**
 * @file tests/storage/bench_db.cpp
 * @brief SQLite 存储层性能基准：封装 vs 原生 sqlite3。
 *
 * 对比组：
 *  1. 1e6 行插入（文件库，WAL，synchronous=NORMAL，单事务）；
 *  2. 1e6 行插入（内存库）；
 *  3. 1e6 行全表扫描（封装 Rows vs 原生 sqlite3_step）；
 *  4. 1e6 次预编译复用（内存库，Bind/Step/Reset）；
 *  5. 1e7 次 `SELECT ?` 微基准（内存库，纯 API 开销）；
 *  6. 1e6 行 UPDATE / DELETE（文件库，同数据、同事务边界）；
 *  7. 逐字节一致：封装与原生写出的数据库文件与表内容逐字节比较。
 *
 * 用法：`go2cpp_storage_bench [输出数据库目录]`
 *       缺省在临时目录运行并在结束时清理。
 */
#include "go2cpp/storage/db.hpp"

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
using go2cpp::storage::Rows;
using go2cpp::storage::Stmt;

// 场景函数一律 noinline：objdump 才能按场景对比封装与原生循环体的
// 指令数 / call 数（见 /tmp/sqlite_asm.md）。每个场景只调用一次，开销可忽略。
#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now() - start)
               .count() /
           1000.0;
}

void Report(const char* name, std::int64_t count, double ms,
            std::int64_t checksum) {
    const double ops = ms > 0.0 ? count * 1000.0 / ms : 0.0;
    const double ns = count > 0 ? ms * 1e6 / count : 0.0;
    std::printf("%-40s count=%-9lld %9.3f ms %12.0f ops/s %8.1f ns/op "
                "sum=%lld\n",
                name, static_cast<long long>(count), ms, ops, ns,
                static_cast<long long>(checksum));
}

/// @brief 原生 sqlite3 连接的 RAII 包装（基准对照组）。
class NativeDb {
public:
    explicit NativeDb(const std::string& path) {
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX;
        if (sqlite3_open_v2(path.c_str(), &m_db, flags, nullptr) == SQLITE_OK) {
            sqlite3_busy_timeout(m_db, 15000);
            (void)sqlite3_exec(m_db, "PRAGMA journal_mode=WAL", nullptr,
                               nullptr, nullptr);
            (void)sqlite3_exec(m_db, "PRAGMA synchronous=NORMAL", nullptr,
                               nullptr, nullptr);
        }
    }
    ~NativeDb() {
        if (m_db != nullptr) {
            sqlite3_close(m_db);
        }
    }
    NativeDb(const NativeDb&) = delete;
    NativeDb& operator=(const NativeDb&) = delete;

    sqlite3* get() const { return m_db; }

private:
    sqlite3* m_db = nullptr;
};

bool NativeExec(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

/// @brief 封装插入 1e6 行；返回耗时、校验和与最后 rowid。
struct InsertStats {
    double ms = 0.0;
    std::int64_t checksum = 0;
    std::int64_t last_rowid = 0;
};

BENCH_NOINLINE
InsertStats InsertWithWrapper(const std::string& path, bool memory,
                              int rows) {
    InsertStats stats;
    const auto start = Clock::now();
    auto opened = DB::Open(path);
    if (!opened.ok()) {
        std::fprintf(stderr, "open failed: %s\n",
                     opened.Err().Message().c_str());
        return stats;
    }
    std::unique_ptr<DB> db = std::move(opened.Value());
    if (!memory) {
        (void)db->SetJournalMode("WAL");
        (void)db->SetSynchronous("NORMAL");
    }
    (void)db->Exec("DROP TABLE IF EXISTS t");
    if (!db->Exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, "
                  "name TEXT)")
             .ok()) {
        std::fprintf(stderr, "create failed: %s\n",
                     db->LastError().Message().c_str());
        return stats;
    }
    if (!db->Begin().ok()) {
        return stats;
    }
    auto stmt = db->Prepare("INSERT INTO t(id, v, name) VALUES(?, ?, ?)");
    if (!stmt.ok()) {
        return stats;
    }
    for (int i = 0; i < rows; ++i) {
        stmt.Value().Reset();
        (void)stmt.Value().Bind(i, i * 2, "abcdefgh");
        (void)stmt.Value().Step();
        stats.checksum += i * 2;
    }
    (void)db->Commit();
    stats.last_rowid = db->LastInsertRowId();
    if (!memory) {
        (void)db->Exec("PRAGMA wal_checkpoint(TRUNCATE)");
    }
    stats.ms = ElapsedMs(start);
    return stats;
}

BENCH_NOINLINE
InsertStats InsertWithNative(const std::string& path, int rows) {
    InsertStats stats;
    const auto start = Clock::now();
    NativeDb native(path);
    sqlite3* db = native.get();
    if (db == nullptr) {
        return stats;
    }
    (void)NativeExec(db, "DROP TABLE IF EXISTS t");
    (void)NativeExec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, "
                         "name TEXT)");
    (void)NativeExec(db, "BEGIN IMMEDIATE");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO t(id, v, name) VALUES(?, ?, ?)", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        return stats;
    }
    for (int i = 0; i < rows; ++i) {
        sqlite3_bind_int64(stmt, 1, i);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(i) * 2);
        sqlite3_bind_text(stmt, 3, "abcdefgh", 8, SQLITE_STATIC);
        (void)sqlite3_step(stmt);
        (void)sqlite3_reset(stmt);
        stats.checksum += i * 2;
    }
    (void)sqlite3_finalize(stmt);
    (void)NativeExec(db, "COMMIT");
    stats.last_rowid = sqlite3_last_insert_rowid(db);
    (void)NativeExec(db, "PRAGMA wal_checkpoint(TRUNCATE)");
    stats.ms = ElapsedMs(start);
    return stats;
}


/// @brief 原生插入，绑定策略 SQLITE_TRANSIENT（对齐封装旧语义）。
BENCH_NOINLINE
InsertStats InsertWithNativeTransient(const std::string& path, int rows) {
    InsertStats stats;
    const auto start = Clock::now();
    NativeDb native(path);
    sqlite3* db = native.get();
    if (db == nullptr) {
        return stats;
    }
    (void)NativeExec(db, "DROP TABLE IF EXISTS t");
    (void)NativeExec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, "
                         "name TEXT)");
    (void)NativeExec(db, "BEGIN IMMEDIATE");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO t(id, v, name) VALUES(?, ?, ?)", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        return stats;
    }
    for (int i = 0; i < rows; ++i) {
        sqlite3_bind_int64(stmt, 1, i);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(i) * 2);
        sqlite3_bind_text(stmt, 3, "abcdefgh", 8, SQLITE_TRANSIENT);
        (void)sqlite3_step(stmt);
        (void)sqlite3_reset(stmt);
        stats.checksum += i * 2;
    }
    (void)sqlite3_finalize(stmt);
    (void)NativeExec(db, "COMMIT");
    stats.last_rowid = sqlite3_last_insert_rowid(db);
    (void)NativeExec(db, "PRAGMA wal_checkpoint(TRUNCATE)");
    stats.ms = ElapsedMs(start);
    return stats;
}

/// @brief 封装 UPDATE 1e6 行（单事务），checksum = changes。
BENCH_NOINLINE
InsertStats UpdateWithWrapper(const std::string& path) {
    InsertStats stats;
    const auto start = Clock::now();
    auto opened = DB::Open(path);
    if (!opened.ok()) {
        return stats;
    }
    std::unique_ptr<DB> db = std::move(opened.Value());
    if (!db->Begin().ok()) {
        return stats;
    }
    const auto updated = db->Exec("UPDATE t SET v = v + 1");
    if (!updated.ok()) {
        (void)db->Rollback();
        return stats;
    }
    stats.checksum = updated.Value().changes;
    if (!db->Commit().ok()) {
        return stats;
    }
    stats.ms = ElapsedMs(start);
    return stats;
}

/// @brief 原生 UPDATE 1e6 行（单事务），checksum = changes。
BENCH_NOINLINE
InsertStats UpdateWithNative(const std::string& path) {
    InsertStats stats;
    const auto start = Clock::now();
    NativeDb native(path);
    sqlite3* db = native.get();
    if (db == nullptr) {
        return stats;
    }
    (void)NativeExec(db, "BEGIN IMMEDIATE");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE t SET v = v + 1", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        return stats;
    }
    (void)sqlite3_step(stmt);
    (void)sqlite3_finalize(stmt);
    stats.checksum = sqlite3_changes(db);
    (void)NativeExec(db, "COMMIT");
    stats.ms = ElapsedMs(start);
    return stats;
}

/// @brief 封装 DELETE 1e6 行（单事务），checksum = changes。
BENCH_NOINLINE
InsertStats DeleteWithWrapper(const std::string& path) {
    InsertStats stats;
    const auto start = Clock::now();
    auto opened = DB::Open(path);
    if (!opened.ok()) {
        return stats;
    }
    std::unique_ptr<DB> db = std::move(opened.Value());
    if (!db->Begin().ok()) {
        return stats;
    }
    const auto deleted = db->Exec("DELETE FROM t");
    if (!deleted.ok()) {
        (void)db->Rollback();
        return stats;
    }
    stats.checksum = deleted.Value().changes;
    if (!db->Commit().ok()) {
        return stats;
    }
    stats.ms = ElapsedMs(start);
    return stats;
}

/// @brief 原生 DELETE 1e6 行（单事务），checksum = changes。
BENCH_NOINLINE
InsertStats DeleteWithNative(const std::string& path) {
    InsertStats stats;
    const auto start = Clock::now();
    NativeDb native(path);
    sqlite3* db = native.get();
    if (db == nullptr) {
        return stats;
    }
    (void)NativeExec(db, "BEGIN IMMEDIATE");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM t", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        return stats;
    }
    (void)sqlite3_step(stmt);
    (void)sqlite3_finalize(stmt);
    stats.checksum = sqlite3_changes(db);
    (void)NativeExec(db, "COMMIT");
    stats.ms = ElapsedMs(start);
    return stats;
}

/// @brief 封装全表扫描；校验和按 id*3+v 累加。
BENCH_NOINLINE
InsertStats SelectWithWrapper(const std::string& path) {
    InsertStats stats;
    const auto start = Clock::now();
    auto opened = DB::Open(path);
    if (!opened.ok()) {
        return stats;
    }
    std::unique_ptr<DB> db = std::move(opened.Value());
    auto rows = db->Query("SELECT id, v, name FROM t ORDER BY id");
    if (!rows.ok()) {
        return stats;
    }
    std::int64_t seen = 0;
    while (rows.Value().Next()) {
        const std::int64_t id = rows.Value().Int64(0);
        const std::int64_t v = rows.Value().Int64(1);
        stats.checksum += id * 3 + v;
        ++seen;
    }
    stats.last_rowid = seen;
    stats.ms = ElapsedMs(start);
    return stats;
}

BENCH_NOINLINE
InsertStats SelectWithNative(const std::string& path) {
    InsertStats stats;
    const auto start = Clock::now();
    NativeDb native(path);
    sqlite3* db = native.get();
    if (db == nullptr) {
        return stats;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, v, name FROM t ORDER BY id", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        return stats;
    }
    int rc = SQLITE_ROW;
    std::int64_t seen = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const std::int64_t id = sqlite3_column_int64(stmt, 0);
        const std::int64_t v = sqlite3_column_int64(stmt, 1);
        stats.checksum += id * 3 + v;
        ++seen;
    }
    (void)sqlite3_finalize(stmt);
    stats.last_rowid = seen;
    stats.ms = ElapsedMs(start);
    return stats;
}

/// @brief 预编译复用：1e6 次 Bind/Step/Reset（内存库）。
BENCH_NOINLINE
InsertStats PreparedWithWrapper(int rows) {
    InsertStats stats;
    const auto start = Clock::now();
    auto opened = DB::Open(":memory:");
    if (!opened.ok()) {
        return stats;
    }
    std::unique_ptr<DB> db = std::move(opened.Value());
    (void)db->Exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER)");
    auto stmt = db->Prepare("INSERT INTO t(id, v) VALUES(?, ?)");
    if (!stmt.ok()) {
        return stats;
    }
    (void)db->Begin();
    for (int i = 0; i < rows; ++i) {
        stmt.Value().Reset();
        stmt.Value().ClearBindings();
        (void)stmt.Value().Bind(i, i + 1);
        (void)stmt.Value().Step();
        stats.checksum += i + 1;
    }
    (void)db->Commit();
    stats.ms = ElapsedMs(start);
    return stats;
}

BENCH_NOINLINE
InsertStats PreparedWithNative(int rows) {
    InsertStats stats;
    const auto start = Clock::now();
    NativeDb native(":memory:");
    sqlite3* db = native.get();
    if (db == nullptr) {
        return stats;
    }
    (void)NativeExec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER)");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO t(id, v) VALUES(?, ?)", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return stats;
    }
    (void)NativeExec(db, "BEGIN IMMEDIATE");
    for (int i = 0; i < rows; ++i) {
        sqlite3_bind_int64(stmt, 1, i);
        sqlite3_bind_int64(stmt, 2, i + 1);
        (void)sqlite3_step(stmt);
        (void)sqlite3_reset(stmt);
        stats.checksum += i + 1;
    }
    (void)sqlite3_finalize(stmt);
    (void)NativeExec(db, "COMMIT");
    stats.ms = ElapsedMs(start);
    return stats;
}

/// @brief 1e7 次 `SELECT ?` 微基准：只测绑定/执行/取列/重置的 API 开销。
BENCH_NOINLINE
InsertStats MicroWithWrapper(std::int64_t iterations) {
    InsertStats stats;
    const auto start = Clock::now();
    auto opened = DB::Open(":memory:");
    if (!opened.ok()) {
        return stats;
    }
    std::unique_ptr<DB> db = std::move(opened.Value());
    auto stmt = db->Prepare("SELECT ?");
    if (!stmt.ok()) {
        return stats;
    }
    for (std::int64_t i = 0; i < iterations; ++i) {
        stmt.Value().Reset();
        (void)stmt.Value().Bind(i);
        (void)stmt.Value().Step();
        stats.checksum += stmt.Value().ColumnInt64(0);
    }
    stats.ms = ElapsedMs(start);
    return stats;
}

BENCH_NOINLINE
InsertStats MicroWithNative(std::int64_t iterations) {
    InsertStats stats;
    const auto start = Clock::now();
    NativeDb native(":memory:");
    sqlite3* db = native.get();
    if (db == nullptr) {
        return stats;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT ?", -1, &stmt, nullptr) != SQLITE_OK) {
        return stats;
    }
    for (std::int64_t i = 0; i < iterations; ++i) {
        sqlite3_bind_int64(stmt, 1, i);
        (void)sqlite3_step(stmt);
        stats.checksum += sqlite3_column_int64(stmt, 0);
        (void)sqlite3_reset(stmt);
    }
    (void)sqlite3_finalize(stmt);
    stats.ms = ElapsedMs(start);
    return stats;
}

/// @brief 把表 t 按 id 序列化为确定字节串（用于逐字节一致比较）。
std::string DumpTable(DB& db) {
    std::string out;
    auto rows = db.Query("SELECT id, v, name FROM t ORDER BY id");
    if (!rows.ok()) {
        return out;
    }
    while (rows.Value().Next()) {
        const std::int64_t id = rows.Value().Int64(0);
        const std::int64_t v = rows.Value().Int64(1);
        const std::string name = rows.Value().Text(2);
        const std::uint32_t size = static_cast<std::uint32_t>(name.size());
        out.append(reinterpret_cast<const char*>(&id), sizeof(id));
        out.append(reinterpret_cast<const char*>(&v), sizeof(v));
        out.append(reinterpret_cast<const char*>(&size), sizeof(size));
        out.append(name);
    }
    return out;
}

std::uint64_t Fnv1a64(const std::string& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool ReadFileBytes(const std::string& path, std::string* out) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    out->clear();
    char buffer[65536];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out->append(buffer, got);
    }
    std::fclose(file);
    return true;
}

/// @brief 封装与原生各写 rows 行，比较数据库文件字节与内容 dump。
BENCH_NOINLINE
void ByteIdentical(const std::string& dir, int rows, const char* tag) {
    const std::string wrapper_path =
        dir + "/bytes_" + tag + "_wrapper.db";
    const std::string native_path = dir + "/bytes_" + tag + "_native.db";

    {
        auto opened = DB::Open(wrapper_path);
        if (!opened.ok()) {
            std::fprintf(stderr, "byte-identical open failed\n");
            return;
        }
        DB& db = *opened.Value();
        (void)db.SetJournalMode("WAL");
        (void)db.SetSynchronous("NORMAL");
        (void)db.Exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, "
                      "name TEXT)");
        (void)db.Begin();
        auto stmt = db.Prepare("INSERT INTO t(id, v, name) VALUES(?, ?, ?)");
        for (int i = 0; i < rows; ++i) {
            stmt.Value().Reset();
            (void)stmt.Value().Bind(i, i * 2, "abcdefgh");
            (void)stmt.Value().Step();
        }
        (void)db.Commit();
        (void)db.Exec("PRAGMA wal_checkpoint(TRUNCATE)");
        (void)db.Close();
    }
    {
        NativeDb native(native_path);
        sqlite3* db = native.get();
        if (db == nullptr) {
            return;
        }
        (void)NativeExec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, "
                             "v INTEGER, name TEXT)");
        (void)NativeExec(db, "BEGIN IMMEDIATE");
        sqlite3_stmt* stmt = nullptr;
        (void)sqlite3_prepare_v2(
            db, "INSERT INTO t(id, v, name) VALUES(?, ?, ?)", -1, &stmt,
            nullptr);
        for (int i = 0; i < rows; ++i) {
            sqlite3_bind_int64(stmt, 1, i);
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(i) * 2);
            sqlite3_bind_text(stmt, 3, "abcdefgh", 8, SQLITE_STATIC);
            (void)sqlite3_step(stmt);
            (void)sqlite3_reset(stmt);
        }
        (void)sqlite3_finalize(stmt);
        (void)NativeExec(db, "COMMIT");
        (void)NativeExec(db, "PRAGMA wal_checkpoint(TRUNCATE)");
    }

    std::string wrapper_bytes;
    std::string native_bytes;
    const bool read_ok = ReadFileBytes(wrapper_path, &wrapper_bytes) &&
                         ReadFileBytes(native_path, &native_bytes);
    const bool identical =
        read_ok && wrapper_bytes.size() == native_bytes.size() &&
        std::memcmp(wrapper_bytes.data(), native_bytes.data(),
                    wrapper_bytes.size()) == 0;

    // 内容层一致性（即使文件字节因 WAL 残留不同）：包装读、原生读，逐字节比较。
    std::string wrapper_dump;
    {
        auto opened = DB::Open(wrapper_path);
        if (opened.ok()) {
            wrapper_dump = DumpTable(*opened.Value());
        }
    }
    std::string native_dump;
    {
        NativeDb native(native_path);
        if (native.get() != nullptr) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(native.get(),
                                   "SELECT id, v, name FROM t ORDER BY id", -1,
                                   &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const std::int64_t id = sqlite3_column_int64(stmt, 0);
                    const std::int64_t v = sqlite3_column_int64(stmt, 1);
                    const char* name = reinterpret_cast<const char*>(
                        sqlite3_column_text(stmt, 2));
                    const int size = sqlite3_column_bytes(stmt, 2);
                    const std::uint32_t name_size =
                        static_cast<std::uint32_t>(size);
                    native_dump.append(reinterpret_cast<const char*>(&id),
                                       sizeof(id));
                    native_dump.append(reinterpret_cast<const char*>(&v),
                                       sizeof(v));
                    native_dump.append(
                        reinterpret_cast<const char*>(&name_size),
                        sizeof(name_size));
                    native_dump.append(name, static_cast<std::size_t>(size));
                }
                (void)sqlite3_finalize(stmt);
            }
        }
    }

    std::printf("[byte-identical:%s] files: %s=%zu bytes, %s=%zu bytes, "
                "file_bytes_equal=%s\n", tag,
                wrapper_path.c_str(), wrapper_bytes.size(),
                native_path.c_str(), native_bytes.size(),
                identical ? "yes" : "no");
    std::printf("[byte-identical:%s] dump: wrapper=%zu bytes fnv=%016llx, "
                "native=%zu bytes fnv=%016llx, dump_equal=%s\n", tag,
                wrapper_dump.size(),
                static_cast<unsigned long long>(Fnv1a64(wrapper_dump)),
                native_dump.size(),
                static_cast<unsigned long long>(Fnv1a64(native_dump)),
                wrapper_dump == native_dump ? "yes" : "no");
}

} // namespace

int main(int argc, char** argv) {
    std::string dir;
    bool own_dir = false;
    if (argc > 1) {
        dir = argv[1];
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    } else {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        dir = (std::filesystem::temp_directory_path() /
               ("go2cpp_storage_bench_" + std::to_string(stamp)))
                  .string();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        own_dir = true;
    }

    const int kRows = argc > 2 ? std::atoi(argv[2]) : 1000000;
    constexpr std::int64_t kMicroRows = 10000000;

    std::printf("=== go2cpp storage benchmark (SQLite %s) ===\n",
                sqlite3_libversion());
    std::printf("dir=%s rows=%d\n", dir.c_str(), kRows);

    // ---- 1. 文件库插入 1e6 ----
    const InsertStats wrapper_file =
        InsertWithWrapper(dir + "/wrapper.db", false, kRows);
    Report("insert-file-wrapper", kRows, wrapper_file.ms,
           wrapper_file.checksum);
    const InsertStats native_file =
        InsertWithNative(dir + "/native.db", kRows);
    Report("insert-file-native", kRows, native_file.ms,
           native_file.checksum);
    const InsertStats native_transient =
        InsertWithNativeTransient(dir + "/native_transient.db", kRows);
    Report("insert-file-native-transient", kRows, native_transient.ms,
           native_transient.checksum);

    // ---- 2. 内存库插入 1e6 ----
    const InsertStats wrapper_mem =
        InsertWithWrapper(":memory:", true, kRows);
    Report("insert-memory-wrapper", kRows, wrapper_mem.ms,
           wrapper_mem.checksum);
    const InsertStats native_mem = InsertWithNative(":memory:", kRows);
    Report("insert-memory-native", kRows, native_mem.ms,
           native_mem.checksum);

    // ---- 3. 全表扫描 1e6（两个 1e6 行文件）----
    const InsertStats wrapper_select =
        SelectWithWrapper(dir + "/wrapper.db");
    Report("select-file-wrapper", kRows, wrapper_select.ms,
           wrapper_select.checksum);
    const InsertStats native_select = SelectWithNative(dir + "/native.db");
    Report("select-file-native", kRows, native_select.ms,
           native_select.checksum);

    // ---- 3b. 同一对 1e6 行文件库上 UPDATE 再 DELETE ----
    const InsertStats wrapper_update =
        UpdateWithWrapper(dir + "/wrapper.db");
    Report("update-file-wrapper", kRows, wrapper_update.ms,
           wrapper_update.checksum);
    const InsertStats native_update = UpdateWithNative(dir + "/native.db");
    Report("update-file-native", kRows, native_update.ms,
           native_update.checksum);
    const InsertStats wrapper_delete =
        DeleteWithWrapper(dir + "/wrapper.db");
    Report("delete-file-wrapper", kRows, wrapper_delete.ms,
           wrapper_delete.checksum);
    const InsertStats native_delete = DeleteWithNative(dir + "/native.db");
    Report("delete-file-native", kRows, native_delete.ms,
           native_delete.checksum);

    // ---- 4. 预编译复用 1e6（内存库插入）----
    const InsertStats wrapper_prep = PreparedWithWrapper(kRows);
    Report("prepared-memory-wrapper", kRows, wrapper_prep.ms,
           wrapper_prep.checksum);
    const InsertStats native_prep = PreparedWithNative(kRows);
    Report("prepared-memory-native", kRows, native_prep.ms,
           native_prep.checksum);

    // ---- 5. 1e7 微基准（内存库 SELECT ?）----
    const InsertStats wrapper_micro = MicroWithWrapper(kMicroRows);
    Report("micro-1e7-select-wrapper", kMicroRows, wrapper_micro.ms,
           wrapper_micro.checksum);
    const InsertStats native_micro = MicroWithNative(kMicroRows);
    Report("micro-1e7-select-native", kMicroRows, native_micro.ms,
           native_micro.checksum);

    // ---- 6. 逐字节一致（1000 行与 1e6 行两档）----
    ByteIdentical(dir, 1000, "1k");
    ByteIdentical(dir, kRows, "1m");

    // ---- 汇总（封装/原生倍数）----
    auto ratio = [](double wrapper_ms, double native_ms) {
        return native_ms > 0.0 ? wrapper_ms / native_ms : 0.0;
    };
    std::printf("=== ratio (wrapper/native, <1 means wrapper faster) ===\n");
    std::printf("insert-file:   %.3fx\n",
                ratio(wrapper_file.ms, native_file.ms));
    std::printf("insert-memory: %.3fx\n",
                ratio(wrapper_mem.ms, native_mem.ms));
    std::printf("select-file:   %.3fx\n",
                ratio(wrapper_select.ms, native_select.ms));
    std::printf("update-file:   %.3fx\n",
                ratio(wrapper_update.ms, native_update.ms));
    std::printf("delete-file:   %.3fx\n",
                ratio(wrapper_delete.ms, native_delete.ms));
    std::printf("insert-file (vs native TRANSIENT): %.3fx\n",
                ratio(wrapper_file.ms, native_transient.ms));
    std::printf("prepared-1e6:      %.3fx\n",
                ratio(wrapper_prep.ms, native_prep.ms));
    std::printf("micro-1e7:         %.3fx\n",
                ratio(wrapper_micro.ms, native_micro.ms));

    if (own_dir) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    return 0;
}
