/**
 * @file tests/storage/asm_probe.cpp
 * @brief callgrind 用的小探针：分别用封装/原生插入 N 行，便于统计每行 Ir。
 *
 * 编译（手工，不加入 CMake）：
 *   g++ -O3 -std=c++17 -I include tests/storage/asm_probe.cpp \
 *       -L /tmp/build-sqlite -lgo2cpp_storage -lsqlite3 \
 *       -Wl,-rpath,/tmp/build-sqlite -o /tmp/asm_probe
 *
 * 运行：
 *   valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.wrapper /tmp/asm_probe 20000 wrapper
 *   valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.native  /tmp/asm_probe 20000 native
 *
 * 两个进程除插入路径外完全相同，总 Ir 之差 / 行数 = 封装每行多出的指令数。
 */
#include "go2cpp/storage/db.hpp"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void RunWrapper(int rows) {
    auto opened = go2cpp::storage::DB::Open(":memory:");
    if (!opened.ok()) {
        std::exit(1);
    }
    go2cpp::storage::DB& db = *opened.Value();
    (void)db.Exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, "
                  "name TEXT)");
    (void)db.Begin();
    auto stmt = db.Prepare("INSERT INTO t(id, v, name) VALUES(?, ?, ?)");
    if (!stmt.ok()) {
        std::exit(1);
    }
    for (int i = 0; i < rows; ++i) {
        stmt.Value().Reset();
        (void)stmt.Value().Bind(i, i * 2, "abcdefgh");
        (void)stmt.Value().Step();
    }
    (void)db.Commit();
}

void RunNative(int rows) {
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        std::exit(1);
    }
    (void)sqlite3_exec(db,
                       "CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, "
                       "name TEXT)",
                       nullptr, nullptr, nullptr);
    (void)sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    (void)sqlite3_prepare_v2(db, "INSERT INTO t(id, v, name) VALUES(?, ?, ?)",
                             -1, &stmt, nullptr);
    for (int i = 0; i < rows; ++i) {
        sqlite3_bind_int64(stmt, 1, i);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(i) * 2);
        sqlite3_bind_text(stmt, 3, "abcdefgh", 8, SQLITE_STATIC);
        (void)sqlite3_step(stmt);
        (void)sqlite3_reset(stmt);
    }
    (void)sqlite3_finalize(stmt);
    (void)sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    (void)sqlite3_close(db);
}

} // namespace

int main(int argc, char** argv) {
    const int rows = argc > 1 ? std::atoi(argv[1]) : 2000;
    const bool native = argc > 2 && std::strcmp(argv[2], "native") == 0;
    if (native) {
        RunNative(rows);
    } else {
        RunWrapper(rows);
    }
    return 0;
}
