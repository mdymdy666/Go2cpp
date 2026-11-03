/**
 * @file bili_store.cpp
 * @brief danmaku 表实现：WAL + 参数化 SQL，全部经 `storage::DB`。
 *
 * @author bili demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "bili_store.hpp"

#include "go2cpp/storage/db.hpp"

#include <ctime>
#include <limits>
#include <utility>

namespace bili {

namespace {

/// @brief 建表 + 索引脚本（幂等，与 REQUIREMENTS.md §4 一致）。
constexpr const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS danmaku ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  video_id INTEGER NOT NULL,"
    "  user_id INTEGER NOT NULL,"
    "  text TEXT NOT NULL,"
    "  time_ms INTEGER NOT NULL,"
    "  color INTEGER NOT NULL DEFAULT 16777215,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_danmaku_video_time ON danmaku(video_id, time_ms);";

/// @brief 历史查询默认/上限条数。
constexpr std::size_t kDefaultLimit = 1000;
constexpr std::size_t kMaxLimit = 5000;

/// @brief 把 `storage::Error` 写成可读文本。
std::string DescribeError(const go2cpp::storage::Error& error) {
    return error.ToString();
}

}  // namespace

DanmakuStore::DanmakuStore() = default;
DanmakuStore::~DanmakuStore() = default;

bool DanmakuStore::Open(const std::string& path, std::string* error) {
    auto opened = go2cpp::storage::DB::Open(path);
    if (!opened) {
        if (error != nullptr) {
            *error = "打开数据库失败: " + DescribeError(opened.Err());
        }
        return false;
    }
    m_db = std::move(opened).Value();
    if (auto busy = m_db->SetBusyTimeout(5000); busy) {
        if (error != nullptr) {
            *error = "设置 busy_timeout 失败: " + DescribeError(busy);
        }
        return false;
    }
    if (auto wal = m_db->SetJournalMode("WAL"); wal) {
        if (error != nullptr) {
            *error = "设置 WAL 失败: " + DescribeError(wal);
        }
        return false;
    }
    return true;
}

bool DanmakuStore::EnsureSchema(std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    auto executed = m_db->Exec(kSchemaSql);
    if (!executed) {
        if (error != nullptr) {
            *error = "建表失败: " + DescribeError(executed.Err());
        }
        return false;
    }
    return true;
}

bool DanmakuStore::Ok() const noexcept {
    return m_db != nullptr;
}

bool DanmakuStore::InsertDanmaku(const DanmakuRow& row, DanmakuRow* out,
                                 std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    const std::int64_t created_at = static_cast<std::int64_t>(std::time(nullptr));
    auto executed = m_db->Exec(
        "INSERT INTO danmaku(video_id, user_id, text, time_ms, color, created_at) "
        "VALUES(?, ?, ?, ?, ?, ?)",
        row.video_id, row.user_id, row.text, row.time_ms, row.color, created_at);
    if (!executed) {
        if (error != nullptr) {
            *error = "写入弹幕失败: " + DescribeError(executed.Err());
        }
        return false;
    }
    if (out != nullptr) {
        *out = row;
        out->id = executed.Value().last_insert_row_id;
        out->created_at = created_at;
    }
    return true;
}

bool DanmakuStore::ListDanmaku(std::int64_t video_id, std::int64_t from_ms,
                               std::int64_t to_ms, std::size_t limit,
                               std::vector<DanmakuRow>* out, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return false;
    }
    if (limit == 0) {
        limit = kDefaultLimit;
    }
    if (limit > kMaxLimit) {
        limit = kMaxLimit;
    }
    const std::int64_t begin = from_ms < 0 ? 0 : from_ms;
    const std::int64_t end =
        to_ms < 0 ? std::numeric_limits<std::int64_t>::max() : to_ms;
    auto rows = m_db->Query(
        "SELECT id, video_id, user_id, text, time_ms, color, created_at "
        "FROM danmaku WHERE video_id = ? AND time_ms >= ? AND time_ms <= ? "
        "ORDER BY time_ms, id LIMIT ?",
        video_id, begin, end, static_cast<std::int64_t>(limit));
    if (!rows) {
        if (error != nullptr) {
            *error = "查询弹幕失败: " + DescribeError(rows.Err());
        }
        return false;
    }
    if (out != nullptr) {
        out->clear();
        while (rows.Value().Next()) {
            DanmakuRow row;
            row.id = rows.Value().Int64(0);
            row.video_id = rows.Value().Int64(1);
            row.user_id = rows.Value().Int64(2);
            row.text = rows.Value().Text(3);
            row.time_ms = rows.Value().Int64(4);
            row.color = rows.Value().Int64(5);
            row.created_at = rows.Value().Int64(6);
            out->push_back(std::move(row));
        }
        if (rows.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历弹幕失败: " + DescribeError(rows.Value().Err());
            }
            return false;
        }
    }
    return true;
}

std::int64_t DanmakuStore::CountDanmaku(std::int64_t video_id, std::string* error) {
    if (!Ok()) {
        if (error != nullptr) {
            *error = "数据库尚未打开";
        }
        return -1;
    }
    auto rows = m_db->Query("SELECT COUNT(*) FROM danmaku WHERE video_id = ?", video_id);
    if (!rows) {
        if (error != nullptr) {
            *error = "统计弹幕失败: " + DescribeError(rows.Err());
        }
        return -1;
    }
    if (!rows.Value().Next()) {
        if (rows.Value().Err()) {
            if (error != nullptr) {
                *error = "遍历统计结果失败: " + DescribeError(rows.Value().Err());
            }
        }
        return -1;
    }
    return rows.Value().Int64(0);
}

}  // namespace bili
