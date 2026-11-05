// orm 模块微基准：单行 insert / 主键 select 1e5 次 ops/s。
//
// 口径：SQLite 文件库（WAL，synchronous=NORMAL），单事务内 Session::Insert
// 1e5 行；随后按主键 Session::Find<Sample> 1e5 次。每轮开始 DELETE 清表，
// 保证行数恒定 1e5。固定迭代数（任务要求 1e5），若单次不足 100 ms 则重复
// K 轮再除以 K —— 保证样本窗口落在 100 ms–1 s。

#include <sqlite3.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "bench_common.hpp"
#include "go2cpp/storage/orm.hpp"

namespace {

using go2cpp::storage::DB;
using go2cpp::storage::Error;
using go2cpp::storage::Migration;
using go2cpp::storage::Session;

constexpr std::size_t kRows = 100000;
constexpr std::size_t kTargetMs = 120;

struct Sample {
    std::int64_t id = 0;
    std::int64_t value = 0;
    std::string tag;
};

}  // namespace

GO2CPP_ORM_TABLE(Sample, "samples",
                 GO2CPP_ORM_COLUMN(Sample, id).PrimaryKey().AutoIncrement(),
                 GO2CPP_ORM_COLUMN(Sample, value),
                 GO2CPP_ORM_COLUMN(Sample, tag))

namespace {

double InsertBatch(Session& session, DB& db) {
    (void)db.Exec("DELETE FROM samples");
    const auto start = bench::Clock::now();
    const Error error = session.Transaction([&]() -> Error {
        Sample row;
        for (std::size_t i = 0; i < kRows; ++i) {
            row = Sample{};
            row.value = static_cast<std::int64_t>(i * 3);
            row.tag = (i % 2) == 0 ? "even" : "odd";
            auto inserted = session.Insert(row);
            if (!inserted) {
                return inserted.Err();
            }
        }
        return {};
    });
    const double ms = bench::MsSince(start);
    if (error) {
        std::printf("orm_insert_error=%s\n", error.Message().c_str());
        return -1.0;
    }
    return ms;
}

double FindBatch(Session& session) {
    const auto start = bench::Clock::now();
    std::size_t hits = 0;
    for (std::size_t i = 0; i < kRows; ++i) {
        auto found = session.Find<Sample>(static_cast<std::int64_t>(i + 1));
        if (found) {
            hits += static_cast<std::size_t>(found.Value().value);
        }
    }
    const double ms = bench::MsSince(start);
    bench::g_sink = hits;
    return ms;
}

void Report(const char* name, double total_ms, double rounds) {
    const double ms = total_ms / rounds;
    const double ops = static_cast<double>(kRows);
    std::printf("%s ops=%.0f total_ms=%.3f ops_per_sec=%.0f ns_per_op=%.3f\n", name, ops, ms,
                ops / (ms / 1000.0), ms * 1e6 / ops);
    std::fflush(stdout);
}

}  // namespace

int main() {
    const std::string path = "/tmp/perf_uncovered_orm.db";
    (void)std::remove(path.c_str());
    auto opened = DB::Open(path);
    if (!opened) {
        std::printf("orm_open_error=%s\n", opened.Err().Message().c_str());
        return 1;
    }
    std::unique_ptr<DB> db = std::move(opened.Value());
    (void)db->Exec("PRAGMA journal_mode=WAL");
    (void)db->Exec("PRAGMA synchronous=NORMAL");

    Session session(*db);
    Migration migration(*db);
    migration.AddCreateTable<Sample>(1, "create samples");
    if (Error error = migration.Up()) {
        std::printf("orm_migration_error=%s\n", error.Message().c_str());
        return 1;
    }

    // insert：固定 1e5 行/轮，重复到窗口 >= kTargetMs。
    {
        double rounds = 0;
        double total = 0;
        while (total < kTargetMs && rounds < 8) {
            const double ms = InsertBatch(session, *db);
            if (ms < 0) {
                return 1;
            }
            total += ms;
            rounds += 1;
        }
        Report("orm_insert", total, rounds);
    }

    // select：同一批 1e5 行，重复读取到窗口 >= kTargetMs。
    {
        double rounds = 0;
        double total = 0;
        while (total < kTargetMs && rounds < 8) {
            total += FindBatch(session);
            rounds += 1;
        }
        Report("orm_select", total, rounds);
    }

    db.reset();
    (void)std::remove(path.c_str());
    return 0;
}
