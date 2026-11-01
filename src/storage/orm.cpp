/**
 * @file src/storage/orm.cpp
 * @brief ORM 层实现：SQL 生成、表状态缓存、CRUD 内核、迁移执行。
 *
 * @details 模板层（`Session::Insert<T>` / `Query<T>`）只负责「取表映射 +
 *          类型转换」，真正的 SQL 拼装、语句缓存与执行都在本文件的非模板
 *          内核里完成。这样做的原因：
 *
 *          1. 热路径只有一处实现，`objdump` 可定位、可统计；
 *          2. 模板实例化不膨胀（1e6 行插入的是同一份 `InsertImpl`）；
 *          3. 迁移/诊断代码不进入模板，编译期开销可控。
 *
 *          并发：`Session` 用递归互斥量串行化「缓存查找 + 语句使用」，
 *          因为缓存的 `Stmt` 不能并发 `Step`；`DB` 自身还有一把锁。
 *          迁移用独立的递归互斥量，覆盖整个版本表读改写。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */
#include "go2cpp/storage/orm.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace go2cpp {
namespace storage {
namespace {

/// @brief 版本表记录已应用迁移的 DDL。
constexpr const char* kVersionTableColumns =
    "(\"version\" INTEGER PRIMARY KEY, "
    "\"name\" TEXT NOT NULL, "
    "\"applied_at\" INTEGER NOT NULL, "
    "\"duration_us\" INTEGER NOT NULL DEFAULT 0)";

/// @brief 当前 Unix 秒。
std::int64_t UnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// @brief 迁移失败时的统一错误文本（保留底层错误码）。
Error MigrationError(const MigrationStep& step, const char* phase,
                     const Error& inner) {
    std::string message = "migration ";
    message += std::to_string(step.version);
    message += " (";
    message += step.name;
    message += ") ";
    message += phase;
    message += " failed: ";
    message += inner.Message();
    return Error(inner.Code(), inner.ExtendedCode(), std::move(message));
}

/// @brief 逗号分隔的「已转义列名」列表。
std::string ColumnList(const TableDef& table,
                       const std::vector<int>& indexes) {
    std::string out;
    for (std::size_t index = 0; index < indexes.size(); ++index) {
        if (index != 0) {
            out += ", ";
        }
        out += QuoteIdentifier(table.columns[indexes[index]].name);
    }
    return out;
}

/// @brief `n` 个 `?` 占位符（逗号分隔）。
std::string Placeholders(std::size_t count) {
    std::string out;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            out += ", ";
        }
        out += "?";
    }
    return out;
}

} // namespace

// ===========================================================================
// 列/表描述
// ===========================================================================

const char* ColumnTypeName(ColumnType type) noexcept {
    switch (type) {
        case ColumnType::kInteger:
            return "INTEGER";
        case ColumnType::kReal:
            return "REAL";
        case ColumnType::kText:
            return "TEXT";
        case ColumnType::kBlob:
            return "BLOB";
    }
    return "TEXT";
}

const Column* TableDef::Find(std::string_view column_name) const noexcept {
    if (columns == nullptr) {
        return nullptr;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (columns[index].name == column_name) {
            return &columns[index];
        }
    }
    return nullptr;
}

int TableDef::PrimaryKeyIndex() const noexcept {
    if (columns == nullptr) {
        return -1;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (columns[index].IsPrimaryKey()) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

// ===========================================================================
// SQL 生成
// ===========================================================================

std::string QuoteIdentifier(std::string_view identifier) {
    std::string out;
    out.reserve(identifier.size() + 2);
    out.push_back('"');
    for (char character : identifier) {
        if (character == '"') {
            out.push_back('"');
        }
        out.push_back(character);
    }
    out.push_back('"');
    return out;
}

std::string CreateTableSql(const TableDef& table) {
    if (table.name.empty() || table.columns == nullptr || table.count == 0) {
        return {};
    }
    std::string sql = "CREATE TABLE IF NOT EXISTS ";
    sql += QuoteIdentifier(table.name);
    sql += " (";
    for (std::size_t index = 0; index < table.count; ++index) {
        const Column& column = table.columns[index];
        if (index != 0) {
            sql += ", ";
        }
        sql += QuoteIdentifier(column.name);
        sql += " ";
        sql += ColumnTypeName(column.type);
        if (column.IsPrimaryKey()) {
            sql += " PRIMARY KEY";
            if (column.IsAutoIncrement()) {
                sql += " AUTOINCREMENT";
            }
        }
        if ((column.flags & kColumnNotNull) != 0) {
            sql += " NOT NULL";
        }
        if ((column.flags & kColumnUnique) != 0) {
            sql += " UNIQUE";
        }
        if (!column.default_sql.empty()) {
            sql += " DEFAULT ";
            sql += column.default_sql;
        }
    }
    sql += ")";
    // 列上标了 Indexed() 的额外生成普通索引（同一条脚本，多条语句）。
    for (std::size_t index = 0; index < table.count; ++index) {
        const Column& column = table.columns[index];
        if ((column.flags & kColumnIndexed) == 0) {
            continue;
        }
        std::string index_name = table.name;
        index_name += "_";
        index_name += column.name;
        index_name += "_idx";
        sql += "; ";
        sql += CreateIndexSql(index_name, table.name, {column.name}, false);
    }
    return sql;
}

std::string DropTableSql(std::string_view table) {
    return "DROP TABLE IF EXISTS " + QuoteIdentifier(table);
}

std::string CreateIndexSql(std::string_view index_name, std::string_view table,
                           const std::vector<std::string>& columns,
                           bool unique) {
    if (index_name.empty() || table.empty() || columns.empty()) {
        return {};
    }
    std::string sql = unique ? "CREATE UNIQUE INDEX IF NOT EXISTS "
                             : "CREATE INDEX IF NOT EXISTS ";
    sql += QuoteIdentifier(index_name);
    sql += " ON ";
    sql += QuoteIdentifier(table);
    sql += " (";
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) {
            sql += ", ";
        }
        sql += QuoteIdentifier(columns[index]);
    }
    sql += ")";
    return sql;
}

std::string AddColumnSql(std::string_view table, std::string_view column,
                         ColumnType type, bool not_null,
                         std::string_view default_sql) {
    if (table.empty() || column.empty()) {
        return {};
    }
    std::string sql = "ALTER TABLE ";
    sql += QuoteIdentifier(table);
    sql += " ADD COLUMN ";
    sql += QuoteIdentifier(column);
    sql += " ";
    sql += ColumnTypeName(type);
    if (not_null) {
        // SQLite 要求 ADD COLUMN 的 NOT NULL 带非 NULL 默认值；调用方没给
        // 时按类型补一个哑默认值，避免迁移在运行期才炸。
        std::string fallback;
        if (default_sql.empty()) {
            switch (type) {
                case ColumnType::kInteger:
                case ColumnType::kReal:
                    fallback = "0";
                    break;
                case ColumnType::kBlob:
                    fallback = "x''";
                    break;
                case ColumnType::kText:
                    fallback = "''";
                    break;
            }
            default_sql = fallback;
        }
        sql += " NOT NULL";
    }
    if (!default_sql.empty()) {
        sql += " DEFAULT ";
        sql += default_sql;
    }
    return sql;
}

std::string DropColumnSql(std::string_view table, std::string_view column) {
    if (table.empty() || column.empty()) {
        return {};
    }
    return "ALTER TABLE " + QuoteIdentifier(table) + " DROP COLUMN " +
           QuoteIdentifier(column);
}

// ===========================================================================
// Session::TableState
// ===========================================================================

/// @brief 每张表的预生成 SQL + 列索引 + 复用绑定缓冲。
struct Session::TableState {
    TableDef table;                     ///< 表描述副本（columns 指向静态数组）。
    int primary_key = -1;               ///< 主键列下标。
    bool auto_increment = false;        ///< 主键是否自增。
    std::vector<int> insert_columns;    ///< INSERT 绑定的列。
    std::vector<int> update_columns;    ///< UPDATE SET 的列。
    std::string select_list;            ///< `"a", "b", ...`。
    std::string insert_sql;
    std::string update_sql;
    std::string delete_sql;
    std::string find_sql;
    std::string all_sql;
    std::vector<Value> bind_values;     ///< 复用的绑定缓冲（会话内串行）。
    Error error;                        ///< CRUD 不可用时的固定错误。
    // 四条 CRUD 语句预编译后直接挂在这里：热路径不再按 SQL 文本做哈希查找
    // （1e6 行插入时每次 find(std::string) 是数十纳秒级的固定开销）。
    Stmt insert_stmt;
    Stmt update_stmt;
    Stmt delete_stmt;
    Stmt find_stmt;

    /// @brief 校验表定义并生成全部 SQL；返回空错误表示可用。
    Error Prepare(const TableDef& definition);
};

Error Session::TableState::Prepare(const TableDef& table) {
    TableState& state = *this;
    if (table.name.empty()) {
        return Error(kErrMapping, 0, "table mapping has an empty table name");
    }
    if (table.columns == nullptr || table.count == 0) {
        return Error(kErrMapping, 0, "table mapping has no columns");
    }
    if (table.count > kMaxColumns) {
        return Error(kErrMapping, 0,
                     "table has more than kMaxColumns (" +
                         std::to_string(kMaxColumns) + ") columns");
    }
    state.table = table;

    int primary_key = -1;
    for (std::size_t index = 0; index < table.count; ++index) {
        if (!table.columns[index].IsPrimaryKey()) {
            continue;
        }
        if (primary_key >= 0) {
            return Error(kErrMapping, 0,
                         "composite primary keys are not supported");
        }
        primary_key = static_cast<int>(index);
    }
    state.primary_key = primary_key;
    if (primary_key < 0) {
        return Error(kErrMapping, 0,
                     "table \"" + table.name +
                         "\" has no primary key; CRUD requires one");
    }
    const Column& key_column = table.columns[primary_key];
    state.auto_increment = key_column.IsAutoIncrement();
    if (state.auto_increment && key_column.type != ColumnType::kInteger) {
        return Error(kErrMapping, 0,
                     "AUTOINCREMENT requires an INTEGER primary key column \"" +
                         key_column.name + "\"");
    }

    state.select_list.clear();
    for (std::size_t index = 0; index < table.count; ++index) {
        if (index != 0) {
            state.select_list += ", ";
        }
        state.select_list += QuoteIdentifier(table.columns[index].name);
    }

    state.insert_columns.clear();
    state.update_columns.clear();
    for (std::size_t index = 0; index < table.count; ++index) {
        const int position = static_cast<int>(index);
        if (position == primary_key && state.auto_increment) {
            continue; // 自增主键交给 SQLite 生成。
        }
        state.insert_columns.push_back(position);
        if (position != primary_key) {
            state.update_columns.push_back(position);
        }
    }

    const std::string quoted_table = QuoteIdentifier(table.name);
    if (state.insert_columns.empty()) {
        state.insert_sql = "INSERT INTO " + quoted_table + " DEFAULT VALUES";
    } else {
        state.insert_sql = "INSERT INTO " + quoted_table + " (" +
                           ColumnList(table, state.insert_columns) +
                           ") VALUES (" +
                           Placeholders(state.insert_columns.size()) + ")";
    }
    if (state.update_columns.empty()) {
        state.update_sql.clear();
    } else {
        state.update_sql = "UPDATE " + quoted_table + " SET ";
        for (std::size_t index = 0; index < state.update_columns.size();
             ++index) {
            if (index != 0) {
                state.update_sql += ", ";
            }
            state.update_sql +=
                QuoteIdentifier(table.columns[state.update_columns[index]].name);
            state.update_sql += " = ?";
        }
        state.update_sql += " WHERE " + QuoteIdentifier(key_column.name) +
                            " = ?";
    }
    state.delete_sql = "DELETE FROM " + quoted_table + " WHERE " +
                       QuoteIdentifier(key_column.name) + " = ?";
    state.find_sql = "SELECT " + state.select_list + " FROM " + quoted_table +
                     " WHERE " + QuoteIdentifier(key_column.name) + " = ?";
    state.all_sql = "SELECT " + state.select_list + " FROM " + quoted_table;

    const std::size_t bind_slots =
        std::max(state.insert_columns.size(), state.update_columns.size() + 1);
    if (state.bind_values.size() < bind_slots) {
        state.bind_values.resize(bind_slots);
    }
    return {};
}

namespace {

/// @brief 惰性预编译：语句无效（首次使用或缓存被清空）时 prepare 一次。
Error EnsureStatement(DB& db, const std::string& sql, Stmt& target) {
    if (target.Valid()) {
        return {};
    }
    auto prepared = db.Prepare(sql);
    if (!prepared) {
        return prepared.Err();
    }
    target = std::move(prepared.Value());
    return {};
}

} // namespace

// ===========================================================================
// Session
// ===========================================================================

thread_local Session* Session::t_transaction_owner = nullptr;

Session::Session(DB& db) : m_db(&db), m_raw(db.Handle()) {}
Session::~Session() = default;

Session::TableState* Session::StateLocked(const TableDef& table,
                                          Error& error) {
    if (table.columns == nullptr) {
        error = Error(kErrMapping, 0, "table mapping has no columns");
        return nullptr;
    }
    if (table.columns == m_last_table_key && m_last_table != nullptr) {
        return m_last_table;
    }
    const auto cached = m_tables.find(table.columns);
    if (cached != m_tables.end()) {
        if (cached->second->error) {
            error = cached->second->error;
            return nullptr;
        }
        return cached->second.get();
    }
    auto state = std::make_unique<TableState>();
    Error built = state->Prepare(table);
    TableState* raw = state.get();
    m_tables.emplace(table.columns, std::move(state));
    if (built) {
        raw->error = built;
        error = built;
        return nullptr;
    }
    m_last_table_key = table.columns;
    m_last_table = raw;
    return raw;
}

Stmt* Session::StatementLocked(const std::string& sql, Error& error) {
    const auto cached = m_statements.find(sql);
    if (cached != m_statements.end()) {
        return &cached->second;
    }
    if (m_statements.size() >= kMaxCachedStatements) {
        m_statements.clear();
    }
    auto prepared = m_db->Prepare(sql);
    if (!prepared) {
        error = prepared.Err();
        return nullptr;
    }
    auto entry = m_statements.emplace(sql, std::move(prepared.Value()));
    return &entry.first->second;
}

Result<std::int64_t> Session::InsertImpl(const TableDef& table, void* object,
                                         bool fill_key) {
    auto lock = Lock();
    Error error;
    TableState* state = StateLocked(table, error);
    if (state == nullptr) {
        return error;
    }
    Stmt* statement = &state->insert_stmt;
    if (Error prepared = EnsureStatement(*m_db, state->insert_sql, *statement)) {
        return prepared;
    }
    const std::size_t count = state->insert_columns.size();
    if (static_cast<std::size_t>(statement->ParameterCount()) != count) {
        return Error(kErrMapping, 0,
                     "insert SQL expects " +
                         std::to_string(statement->ParameterCount()) +
                         " parameters, mapping binds " +
                         std::to_string(count));
    }
    for (std::size_t index = 0; index < count; ++index) {
        const Column& column = table.columns[state->insert_columns[index]];
        if (column.bind == nullptr) {
            return Error(kErrMapping, 0,
                         "column \"" + column.name + "\" has no bind callback");
        }
        column.bind(object, column.context, state->bind_values[index]);
    }
    if (Error reset = statement->Reset()) {
        return reset;
    }
    if (Error bound = statement->BindValues(state->bind_values.data(), count)) {
        return bound;
    }
    if (Error stepped = statement->Step()) {
        return stepped;
    }
    if (!statement->Done()) {
        return Error(kErrMapping, 0, "INSERT statement did not complete");
    }
    const std::int64_t row_id = static_cast<std::int64_t>(
        sqlite3_last_insert_rowid(m_raw));
    if (fill_key && state->auto_increment && state->primary_key >= 0) {
        const Column& key = table.columns[state->primary_key];
        if (key.assign != nullptr) {
            key.assign(object, row_id, key.context);
        }
    }
    return row_id;
}

Result<int> Session::UpdateImpl(const TableDef& table, const void* object) {
    auto lock = Lock();
    Error error;
    TableState* state = StateLocked(table, error);
    if (state == nullptr) {
        return error;
    }
    if (state->update_sql.empty()) {
        return Error(kErrMapping, 0,
                     "table \"" + table.name + "\" has no updatable columns");
    }
    Stmt* statement = &state->update_stmt;
    if (Error prepared = EnsureStatement(*m_db, state->update_sql, *statement)) {
        return prepared;
    }
    const std::size_t count = state->update_columns.size();
    const std::size_t expected = count + 1;
    if (static_cast<std::size_t>(statement->ParameterCount()) != expected) {
        return Error(kErrMapping, 0,
                     "update SQL expects " +
                         std::to_string(statement->ParameterCount()) +
                         " parameters, mapping binds " +
                         std::to_string(expected));
    }
    for (std::size_t index = 0; index < count; ++index) {
        const Column& column = table.columns[state->update_columns[index]];
        column.bind(object, column.context, state->bind_values[index]);
    }
    const Column& key = table.columns[state->primary_key];
    key.bind(object, key.context, state->bind_values[count]);
    if (Error reset = statement->Reset()) {
        return reset;
    }
    if (Error bound = statement->BindValues(state->bind_values.data(), expected)) {
        return bound;
    }
    if (Error stepped = statement->Step()) {
        return stepped;
    }
    if (!statement->Done()) {
        return Error(kErrMapping, 0, "UPDATE statement did not complete");
    }
    return sqlite3_changes(m_raw);
}

Result<int> Session::DeleteImpl(const TableDef& table, std::int64_t key) {
    auto lock = Lock();
    Error error;
    TableState* state = StateLocked(table, error);
    if (state == nullptr) {
        return error;
    }
    Stmt* statement = &state->delete_stmt;
    if (Error prepared = EnsureStatement(*m_db, state->delete_sql, *statement)) {
        return prepared;
    }
    const Value key_value(key);
    if (Error reset = statement->Reset()) {
        return reset;
    }
    if (Error bound = statement->BindValues(&key_value, 1)) {
        return bound;
    }
    if (Error stepped = statement->Step()) {
        return stepped;
    }
    if (!statement->Done()) {
        return Error(kErrMapping, 0, "DELETE statement did not complete");
    }
    return sqlite3_changes(m_raw);
}

Result<bool> Session::FindImpl(const TableDef& table, std::int64_t key,
                               void* object) {
    auto lock = Lock();
    Error error;
    TableState* state = StateLocked(table, error);
    if (state == nullptr) {
        return error;
    }
    Stmt* statement = &state->find_stmt;
    if (Error prepared = EnsureStatement(*m_db, state->find_sql, *statement)) {
        return prepared;
    }
    const Value key_value(key);
    if (Error reset = statement->Reset()) {
        return reset;
    }
    if (Error bound = statement->BindValues(&key_value, 1)) {
        return bound;
    }
    if (Error stepped = statement->Step()) {
        return stepped;
    }
    if (!statement->HasRow()) {
        return false;
    }
    const int available = statement->ColumnCount();
    std::size_t count = table.count;
    if (count > static_cast<std::size_t>(available)) {
        count = static_cast<std::size_t>(available);
    }
    for (std::size_t index = 0; index < count; ++index) {
        const Column& column = table.columns[index];
        if (column.read != nullptr) {
            column.read(*statement, static_cast<int>(index), object,
                        column.context);
        }
    }
    return true;
}

Result<std::int64_t> Session::ForEachRow(const std::string& sql,
                                         const Value* params,
                                         std::size_t param_count, RowSink sink,
                                         void* context) {
    auto lock = Lock();
    Error error;
    Stmt* statement = StatementLocked(sql, error);
    if (statement == nullptr) {
        return error;
    }
    if (static_cast<std::size_t>(statement->ParameterCount()) != param_count) {
        return Error(kErrMapping, 0,
                     "query expects " +
                         std::to_string(statement->ParameterCount()) +
                         " parameters, got " + std::to_string(param_count));
    }
    if (Error reset = statement->Reset()) {
        return reset;
    }
    if (Error bound = statement->BindValues(params, param_count)) {
        return bound;
    }
    std::int64_t rows = 0;
    for (;;) {
        if (Error stepped = statement->Step()) {
            return stepped;
        }
        if (!statement->HasRow()) {
            break;
        }
        ++rows;
        if (sink != nullptr && !sink(context, *statement)) {
            break;
        }
    }
    return rows;
}

std::size_t Session::CachedStatementCount() const {
    auto lock = Lock();
    return m_statements.size();
}

void Session::ClearStatementCache() {
    auto lock = Lock();
    // 表状态里的 CRUD 语句与查询语句一并释放；下次使用时惰性重建
    // （TableState 保留 SQL 文本与列索引，不丢映射信息）。
    for (auto& entry : m_tables) {
        TableState& state = *entry.second;
        (void)state.insert_stmt.Finalize();
        (void)state.update_stmt.Finalize();
        (void)state.delete_stmt.Finalize();
        (void)state.find_stmt.Finalize();
    }
    m_statements.clear();
}

// ===========================================================================
// Migration
// ===========================================================================

Migration::Migration(DB& db, std::string version_table)
    : m_db(&db), m_version_table(std::move(version_table)) {}

Migration::~Migration() = default;

Migration& Migration::Add(std::int64_t version, std::string name,
                          std::string up_sql, std::string down_sql) {
    if (!m_steps.empty() && version <= m_steps.back().version) {
        // 版本号必须严格递增；错误留到 Up()/Down() 时统一上报，避免在
        // 链式声明里抛异常。
        if (!m_build_error) {
            m_build_error = Error(
                kErrMigration, 0,
                "migration versions must strictly increase: " +
                    std::to_string(version) + " after " +
                    std::to_string(m_steps.back().version));
        }
        return *this;
    }
    MigrationStep step;
    step.version = version;
    step.name = std::move(name);
    step.up_sql = std::move(up_sql);
    step.down_sql = std::move(down_sql);
    m_steps.push_back(std::move(step));
    return *this;
}

Migration& Migration::AddCreateIndex(std::int64_t version, std::string name,
                                     std::string index_name, std::string table,
                                     std::vector<std::string> columns,
                                     bool unique) {
    const std::string up =
        CreateIndexSql(index_name, table, columns, unique);
    const std::string down = "DROP INDEX IF EXISTS " +
                             QuoteIdentifier(index_name);
    return Add(version, std::move(name), up, down);
}

Migration& Migration::AddColumn(std::int64_t version, std::string name,
                                std::string table, std::string column,
                                ColumnType type, bool not_null,
                                std::string default_sql) {
    const std::string up =
        AddColumnSql(table, column, type, not_null, default_sql);
    const std::string down = [&] {
        if (sqlite3_libversion_number() < 3035000) {
            return std::string{};
        }
        return DropColumnSql(table, column);
    }();
    if (down.empty()) {
        if (!m_build_error) {
            m_build_error = Error(
                kErrMigration, 0,
                "SQLite " + std::string(sqlite3_libversion()) +
                    " does not support ALTER TABLE ... DROP COLUMN (needs 3.35+)");
        }
    }
    return Add(version, std::move(name), up, down);
}

Migration& Migration::AddDropColumn(std::int64_t version, std::string name,
                                    std::string table, std::string column,
                                    ColumnType type, bool not_null,
                                    std::string default_sql) {
    const std::string up = DropColumnSql(table, column);
    const std::string down =
        AddColumnSql(table, column, type, not_null, default_sql);
    return Add(version, std::move(name), up, down);
}

Error Migration::EnsureTableLocked() {
    const std::string sql =
        "CREATE TABLE IF NOT EXISTS " + QuoteIdentifier(m_version_table) + " " +
        kVersionTableColumns;
    auto created = m_db->Exec(sql);
    if (!created) {
        return Error(kErrMigration, created.Err().ExtendedCode(),
                     "cannot create migration table: " +
                         created.Err().Message());
    }
    return {};
}

Result<std::vector<std::int64_t>> Migration::AppliedVersionsLocked() {
    auto queried = m_db->Query("SELECT \"version\" FROM " +
                               QuoteIdentifier(m_version_table) +
                               " ORDER BY \"version\"");
    if (!queried) {
        return queried.Err();
    }
    Rows& rows = queried.Value();
    std::vector<std::int64_t> versions;
    while (rows.Next()) {
        versions.push_back(rows.Int64(0));
    }
    if (rows.Err()) {
        return rows.Err();
    }
    return versions;
}

Error Migration::ApplyLocked(const MigrationStep& step, bool up) {
    const std::string& sql = up ? step.up_sql : step.down_sql;
    if (!up && sql.empty()) {
        return Error(kErrMigration, 0,
                     "migration " + std::to_string(step.version) + " (" +
                         step.name + ") has no down SQL; cannot roll back");
    }
    const auto started = std::chrono::steady_clock::now();
    if (auto begun = m_db->Exec("BEGIN IMMEDIATE"); !begun) {
        return MigrationError(step, "begin", begun.Err());
    }
    if (!sql.empty()) {
        if (auto executed = m_db->Exec(sql); !executed) {
            (void)m_db->Rollback();
            return MigrationError(step, up ? "up" : "down", executed.Err());
        }
    }
    const std::int64_t elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count();
    Result<ExecResult> recorded = up
        ? m_db->Exec("INSERT INTO " + QuoteIdentifier(m_version_table) +
                         " (\"version\", \"name\", \"applied_at\", "
                         "\"duration_us\") VALUES (?, ?, ?, ?)",
                     step.version, step.name, UnixSeconds(), elapsed_us)
        : m_db->Exec("DELETE FROM " + QuoteIdentifier(m_version_table) +
                         " WHERE \"version\" = ?",
                     step.version);
    if (!recorded) {
        (void)m_db->Rollback();
        return MigrationError(step, "record", recorded.Err());
    }
    if (auto committed = m_db->Commit(); !committed) {
        return MigrationError(step, "commit", committed);
    }
    return {};
}

Error Migration::UpToLocked(std::int64_t target) {
    if (m_build_error) {
        return m_build_error;
    }
    if (auto ensured = EnsureTableLocked(); ensured) {
        return ensured;
    }
    auto applied = AppliedVersionsLocked();
    if (!applied) {
        return applied.Err();
    }
    std::unordered_set<std::int64_t> done(applied.Value().begin(),
                                          applied.Value().end());
    std::vector<const MigrationStep*> pending;
    for (const MigrationStep& step : m_steps) {
        if (step.version <= target && done.find(step.version) == done.end()) {
            pending.push_back(&step);
        }
    }
    std::sort(pending.begin(), pending.end(),
              [](const MigrationStep* left, const MigrationStep* right) {
                  return left->version < right->version;
              });
    for (const MigrationStep* step : pending) {
        if (Error applied_error = ApplyLocked(*step, true)) {
            return applied_error;
        }
    }
    return {};
}

Error Migration::DownToLocked(std::int64_t target) {
    if (m_build_error) {
        return m_build_error;
    }
    if (auto ensured = EnsureTableLocked(); ensured) {
        return ensured;
    }
    auto applied = AppliedVersionsLocked();
    if (!applied) {
        return applied.Err();
    }
    std::vector<std::int64_t> versions = std::move(applied.Value());
    for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        if (*it <= target) {
            break;
        }
        const MigrationStep* step = nullptr;
        for (const MigrationStep& candidate : m_steps) {
            if (candidate.version == *it) {
                step = &candidate;
                break;
            }
        }
        if (step == nullptr) {
            return Error(kErrMigration, 0,
                         "applied migration " + std::to_string(*it) +
                             " is not declared; cannot roll back");
        }
        if (Error applied_error = ApplyLocked(*step, false)) {
            return applied_error;
        }
    }
    return {};
}

Error Migration::Up() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return UpToLocked(std::numeric_limits<std::int64_t>::max());
}

Error Migration::UpTo(std::int64_t target) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return UpToLocked(target);
}

Error Migration::Down(int steps) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_build_error) {
        return m_build_error;
    }
    if (steps <= 0) {
        return {};
    }
    if (auto ensured = EnsureTableLocked(); ensured) {
        return ensured;
    }
    auto applied = AppliedVersionsLocked();
    if (!applied) {
        return applied.Err();
    }
    const std::vector<std::int64_t> versions = std::move(applied.Value());
    const int limit = std::min<int>(steps, static_cast<int>(versions.size()));
    for (int index = 0; index < limit; ++index) {
        const std::int64_t version = versions[versions.size() - 1 -
                                              static_cast<std::size_t>(index)];
        const MigrationStep* step = nullptr;
        for (const MigrationStep& candidate : m_steps) {
            if (candidate.version == version) {
                step = &candidate;
                break;
            }
        }
        if (step == nullptr) {
            return Error(kErrMigration, 0,
                         "applied migration " + std::to_string(version) +
                             " is not declared; cannot roll back");
        }
        if (Error applied_error = ApplyLocked(*step, false)) {
            return applied_error;
        }
    }
    return {};
}

Error Migration::DownTo(std::int64_t target) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return DownToLocked(target);
}

Error Migration::MigrateTo(std::int64_t target) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto current = CurrentVersion();
    if (!current) {
        return current.Err();
    }
    if (current.Value() < target) {
        return UpToLocked(target);
    }
    if (current.Value() > target) {
        return DownToLocked(target);
    }
    return {};
}

Result<std::int64_t> Migration::CurrentVersion() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (auto ensured = EnsureTableLocked(); ensured) {
        return ensured;
    }
    auto queried = m_db->Query("SELECT COALESCE(MAX(\"version\"), 0) FROM " +
                               QuoteIdentifier(m_version_table));
    if (!queried) {
        return queried.Err();
    }
    Rows& rows = queried.Value();
    std::int64_t version = 0;
    if (rows.Next()) {
        version = rows.Int64(0);
    }
    if (rows.Err()) {
        return rows.Err();
    }
    return version;
}

Result<std::vector<AppliedMigration>> Migration::Applied() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (auto ensured = EnsureTableLocked(); ensured) {
        return ensured;
    }
    auto queried = m_db->Query(
        "SELECT \"version\", \"name\", \"applied_at\", \"duration_us\" FROM " +
        QuoteIdentifier(m_version_table) + " ORDER BY \"version\"");
    if (!queried) {
        return queried.Err();
    }
    Rows& rows = queried.Value();
    std::vector<AppliedMigration> history;
    while (rows.Next()) {
        AppliedMigration entry;
        entry.version = rows.Int64(0);
        entry.name = rows.Text(1);
        entry.applied_at = rows.Int64(2);
        entry.duration_us = rows.Int64(3);
        history.push_back(std::move(entry));
    }
    if (rows.Err()) {
        return rows.Err();
    }
    return history;
}

} // namespace storage
} // namespace go2cpp
