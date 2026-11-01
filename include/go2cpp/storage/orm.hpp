/**
 * @file include/go2cpp/storage/orm.hpp
 * @brief SQLite 表映射 / CRUD / 查询构造 / 迁移（ORM 层，构建在 db.hpp 之上）。
 *
 * ## 模块定位
 *
 * `db.hpp` 是 **SQL 层**：SQL 文本由调用方书写，封装只管资源、绑定、错误与
 * 事务。本文件是它上面的 **ORM 层**：把「结构体 ↔ 表」的映射、CRUD、简单
 * 查询构造与版本化迁移收进一个可选头文件，业务代码不再逐条手写 SQL 与
 * `sqlite3_stmt*` 生命周期。
 *
 * 分层（单向依赖，`db.hpp` 不知道本文件存在）：
 *
 * ```
 *  业务结构体 + GO2CPP_ORM_TABLE 映射
 *        │
 *   storage::Session / Query<T> / Migration      ← 本文件
 *        │
 *   storage::DB / Stmt / Rows / Transaction      ← db.hpp
 *        │
 *   libsqlite3
 * ```
 *
 * ## 设计取舍（对标 GORM / sqlite_orm，但更小）
 *
 * | 维度 | GORM（Go） | sqlite_orm（C++） | 本模块 |
 * |---|---|---|---|
 * | 映射方式 | struct tag + 反射 | `make_column`/`make_storage` 模板 | 宏 `GO2CPP_ORM_COLUMN`（显式、零运行期分派） |
 * | CRUD | `db.Create/First/Save/Delete` | `storage.insert/update/get` | `Session::Insert/Update/Delete/Find` |
 * | 查询 | 链式 `Where/Order/Limit` + 结构体 | 编译期 `where(...)` 表达式树 | 运行期子串 + 参数绑定（不解析、不建树） |
 * | 迁移 | `AutoMigrate` + 手写 SQL | 无内建 | `Migration` 版本表 + `Up/Down` |
 * | 事务 | `db.Transaction(fn)` | `transaction(fn)` | `Session::Transaction(fn)`（RAII + 回调） |
 * | 反射 | 运行期 reflect | 编译期模板 | **可选**：`GO2CPP_ORM_TABLE_REFLECT` 复用 `FieldList<T>` |
 *
 * **为什么默认用宏而不是全自动反射**：SQL 列类型必须与字段的 C++ 类型精确
 * 对应（`int32_t` 与 `int64_t` 在反射的 `TypeId` 里只差一个常量，但读写宽度
 * 不同）。宏在展开点就知道字段的静态类型，`bind`/`read` 回调由 `if constexpr`
 * 直接生成，热路径没有类型分派、没有 `memcpy` 大小判断。`reflect` 的
 * `FieldList<T>` 只有「名字 + TypeId + offset + size」，要还原确切 C++ 类型
 * 必须按 `TypeId × size` 猜，边界类型（`enum`/`char`）容易踩内存。所以反射
 * 自动映射作为**可选便捷入口**（`GO2CPP_ORM_TABLE_REFLECT`），只接受可精确
 * 还原的类型，其余在构建表时返回 `kErrMapping` 而不是静默写错。
 *
 * ## 本文件定义什么
 *
 * - `Column` / `TableDef` / `Table<T>`：运行期表描述（名字、类型、主键、
 *   索引、默认值、类型擦除的 bind/read 回调）；
 * - `GO2CPP_ORM_COLUMN` / `GO2CPP_ORM_TABLE`：声明式映射宏；
 * - `GO2CPP_ORM_TABLE_REFLECT`：复用 `reflect::FieldList<T>` 的自动映射（可选）；
 * - `Session`：一个 `DB` 上的工作会话（语句缓存 + CRUD + 事务 + 迁移入口）；
 * - `Query<T>`：`Where/OrderBy/Limit/Offset` + `All/First/Each/Count`；
 * - `Migration`：版本表 + `Up/UpTo/Down/DownTo`，含建表/加列/建索引的 SQL 生成。
 *
 * ## 边界与陷阱
 *
 * - **主键**：CRUD 要求表有且仅有一个 `INTEGER` 主键；复合主键/文本主键
 *   返回 `kErrMapping`（用 `Query<T>` 按任意条件查询不受限）。`AutoIncrement`
 *   只对整数主键有效，`Insert` 成功后回填结构体的主键字段。
 * - **线程模型**：`Session` 内部用递归互斥量保护语句缓存与语句使用，可跨
 *   线程共享；`Transaction(fn)` 持锁期间同一线程可继续调用本会话（重入），
 *   其它线程会阻塞到事务结束。`DB` 自身的锁仍在每层生效。
 * - **列数上限**：`kMaxColumns = 32`，超过返回 `kErrMapping`（绑定缓冲区是
 *   会话内复用的定长数组，不做每行堆分配）。
 * - **SQL 注入**：列名/表名由宏字面量提供，生成 SQL 时统一双引号转义；
 *   `Where`/`OrderBy` 的表达式是**原样拼接**的调用方输入，只允许传常量
 *   SQL 片段（参数用 `?` 绑定），不要把用户输入拼进表达式。
 * - **迁移的 `down`**：SQLite 3.35+ 才支持 `ALTER TABLE ... DROP COLUMN`；
 *   低版本需要重建表。本模块在低版本返回错误而不是静默降级。
 * - **语句缓存**：缓存 key 是 SQL 文本（`unordered_map<string, Stmt>`），
 *   `Query<T>` 的动态条件会产生不同 SQL；缓存上限 `kMaxCachedStatements`
 *   （默认 128），满了整体清空（不做 LRU）。
 *
 * ## 示例
 *
 * @code
 * struct User {
 *     std::int64_t id = 0;
 *     std::string name;
 *     int age = 0;
 *     std::vector<std::uint8_t> avatar;
 *     std::optional<std::string> email;
 * };
 *
 * GO2CPP_ORM_TABLE(User, "users",
 *                  GO2CPP_ORM_COLUMN(User, id).PrimaryKey().AutoIncrement(),
 *                  GO2CPP_ORM_COLUMN(User, name).NotNull().Unique(),
 *                  GO2CPP_ORM_COLUMN(User, age).Indexed(),
 *                  GO2CPP_ORM_COLUMN(User, avatar),
 *                  GO2CPP_ORM_COLUMN(User, email))
 *
 * auto opened = storage::DB::Open("app.db");
 * storage::DB& db = *opened.Value();
 * storage::Session session(db);
 * storage::Migration migration(db);
 * migration.AddCreateTable<User>(1, "create users");
 * migration.Up();
 *
 * User user;
 * user.name = "alice";
 * user.age = 30;
 * auto id = session.Insert(user);          // 回填 user.id
 * auto found = session.Find<User>(1);
 * auto adults = session.Query<User>().Where("age > ?", 18)
 *                      .OrderBy("age DESC").Limit(10).All();
 * @endcode
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "go2cpp/storage/db.hpp"

// 可选的 reflect 互操作：仓库里有 reflect 模块时启用
// GO2CPP_ORM_TABLE_REFLECT（复用 FieldList<T> 自动映射），否则只有显式宏。
// 只用到字段表（header-only 模板），不引入对 go2cpp_reflect 的链接依赖。
#if defined(__has_include)
#if __has_include(<go2cpp/reflect/field.hpp>)
#include <go2cpp/reflect/field.hpp>
#define GO2CPP_STORAGE_ORM_HAS_REFLECT 1
#endif
#endif

namespace go2cpp {
namespace storage {

// ===========================================================================
// 错误码（超出 SQLite 公开错误码范围，避免与 Code 混淆）
// ===========================================================================

/// @brief 查询/`Find` 没有命中任何行（对应 Go 的 `sql.ErrNoRows`）。
inline constexpr int kErrNotFound = 1001;
/// @brief 表映射非法（缺主键、主键非整数、列数超限、表未声明等）。
inline constexpr int kErrMapping = 1002;
/// @brief 迁移失败（版本表损坏、未知版本、`down` 不可用等）。
inline constexpr int kErrMigration = 1003;
/// @brief 嵌套事务/事务状态非法。
inline constexpr int kErrTransaction = 1004;

/// @brief 单表列数上限：绑定缓冲区是会话内复用的定长数组。
inline constexpr std::size_t kMaxColumns = 32;
/// @brief 语句缓存条数上限；超过后整体清空（不做 LRU）。
inline constexpr std::size_t kMaxCachedStatements = 128;

// ===========================================================================
// 列描述
// ===========================================================================

/// @brief SQLite 列类型（只保留 ORM 需要的四种亲和类型）。
enum class ColumnType : std::uint8_t {
    kInteger = 0, ///< INTEGER（int/uint/bool/enum）。
    kReal = 1,    ///< REAL（float/double）。
    kText = 2,    ///< TEXT（std::string）。
    kBlob = 3,    ///< BLOB（std::vector<std::uint8_t>）。
};

/// @brief 列类型对应的 SQL 类型名（`"INTEGER"`/`"REAL"`/`"TEXT"`/`"BLOB"`）。
const char* ColumnTypeName(ColumnType type) noexcept;

/// @brief 列标志位。
enum ColumnFlag : std::uint32_t {
    kColumnPrimaryKey = 1u << 0,    ///< PRIMARY KEY。
    kColumnAutoIncrement = 1u << 1, ///< INTEGER PRIMARY KEY AUTOINCREMENT。
    kColumnNotNull = 1u << 2,       ///< NOT NULL。
    kColumnUnique = 1u << 3,        ///< UNIQUE。
    kColumnIndexed = 1u << 4,       ///< 额外建普通索引（迁移建表时生成）。
};

/// @brief 绑定回调：把对象里的字段写进 `Value`（类型擦除，无运行期分派）。
using BindFn = void (*)(const void* object, const void* context, Value& out);
/// @brief 读列回调：把语句当前行的第 `column` 列写回对象字段。
using ReadFn = void (*)(const Stmt& statement, int column, void* object,
                        const void* context);
/// @brief 主键回填回调（仅 `AutoIncrement` 主键使用）。
using AssignFn = void (*)(void* object, std::int64_t key, const void* context);

/**
 * @brief 一列的运行期描述。
 *
 * @details 由 `MakeColumn` 创建，`GO2CPP_ORM_COLUMN` 宏在展开点填好
 *          `bind`/`read`/`assign` 三个类型擦除回调；`context` 留给反射桥接
 *          （宏路径恒为 `nullptr`）。链式方法返回**副本**，便于在静态数组
 *          初始化表达式里连续修饰。
 *
 * @note `default_sql` 是原样 SQL 片段（如 `"0"`、`"''"`、`"CURRENT_TIMESTAMP"`），
 *       不做引号/类型校验；调用方保证它是合法 SQL。
 */
struct Column {
    std::string name;              ///< 列名（默认等于字段名）。
    ColumnType type = ColumnType::kInteger; ///< 列类型。
    std::uint32_t flags = 0;       ///< `ColumnFlag` 位或。
    std::string default_sql;       ///< 建表/加列时的 DEFAULT 片段；空表示无。
    std::size_t offset = 0;        ///< 字段相对结构体起始的偏移（诊断用）。
    std::size_t size = 0;          ///< `sizeof(字段类型)`。
    BindFn bind = nullptr;         ///< 对象字段 → `Value`。
    ReadFn read = nullptr;         ///< 语句列 → 对象字段。
    AssignFn assign = nullptr;     ///< 自增主键 → 对象字段。
    const void* context = nullptr; ///< 回调上下文（反射桥接用；宏路径为空）。

    /// @brief 标记为主键（不自动加 NOT NULL：SQLite 的 INTEGER PRIMARY KEY
    ///        允许 NULL 并按 rowid 自增，语义由 `AutoIncrement` 决定）。
    Column PrimaryKey() const {
        Column copy = *this;
        copy.flags |= kColumnPrimaryKey;
        return copy;
    }
    /// @brief 标记为 `INTEGER PRIMARY KEY AUTOINCREMENT`（需同时 `PrimaryKey`）。
    Column AutoIncrement() const {
        Column copy = *this;
        copy.flags |= kColumnAutoIncrement;
        return copy;
    }
    /// @brief 标记 NOT NULL。
    Column NotNull() const {
        Column copy = *this;
        copy.flags |= kColumnNotNull;
        return copy;
    }
    /// @brief 标记 UNIQUE（列级约束）。
    Column Unique() const {
        Column copy = *this;
        copy.flags |= kColumnUnique;
        return copy;
    }
    /// @brief 标记普通索引（迁移建表时生成 `CREATE INDEX`）。
    Column Indexed() const {
        Column copy = *this;
        copy.flags |= kColumnIndexed;
        return copy;
    }
    /// @brief 设置 DEFAULT SQL 片段（原样写入 DDL）。
    Column Default(std::string sql) const {
        Column copy = *this;
        copy.default_sql = std::move(sql);
        return copy;
    }
    /// @brief 覆盖列名（默认为字段名）。
    Column Named(std::string column_name) const {
        Column copy = *this;
        copy.name = std::move(column_name);
        return copy;
    }
    /// @brief 是否主键。
    bool IsPrimaryKey() const noexcept {
        return (flags & kColumnPrimaryKey) != 0;
    }
    /// @brief 是否自增。
    bool IsAutoIncrement() const noexcept {
        return (flags & kColumnAutoIncrement) != 0;
    }
};

/// @brief 一张表的运行期描述：`Table<T>::Columns()` 返回的静态数组 + 表名。
struct TableDef {
    std::string name;     ///< 表名（原样，生成 SQL 时转义）。
    const Column* columns = nullptr; ///< 列数组（静态存储，进程生命周期）。
    std::size_t count = 0;           ///< 列数。

    /// @brief 按名字查列；未找到返回 nullptr。
    const Column* Find(std::string_view column_name) const noexcept;
    /// @brief 主键列下标；无主键返回 -1。
    int PrimaryKeyIndex() const noexcept;
};

// ===========================================================================
// 字段类型 → 列类型 / 绑定 / 读取
// ===========================================================================

/// @brief `std::optional<T>` 检测。
template <typename T>
struct IsOptional : std::false_type {};
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

/// @brief BLOB 字段类型检测（`std::vector<std::uint8_t>`）。
template <typename T>
struct IsBlob : std::false_type {};
template <>
struct IsBlob<std::vector<std::uint8_t>> : std::true_type {};

/// @brief 字段的静态类型是否被 ORM 支持（编译期断言用）。
template <typename T>
inline constexpr bool kIsColumnType =
    IsOptional<std::remove_cv_t<T>>::value ||
    std::is_same_v<std::remove_cv_t<T>, bool> ||
    std::is_enum_v<std::remove_cv_t<T>> ||
    std::is_integral_v<std::remove_cv_t<T>> ||
    std::is_floating_point_v<std::remove_cv_t<T>> ||
    std::is_same_v<std::remove_cv_t<T>, std::string> ||
    IsBlob<std::remove_cv_t<T>>::value;

/**
 * @brief 由字段的 C++ 类型推导列类型。
 * @details `optional<T>` 递归到 `T`；枚举按 INTEGER 存；`bool` 按 INTEGER
 *          存 0/1；BLOB 只认 `std::vector<std::uint8_t>`。
 */
template <typename T>
constexpr ColumnType ColumnTypeOf() {
    using Field = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (IsOptional<Field>::value) {
        return ColumnTypeOf<typename Field::value_type>();
    } else if constexpr (std::is_same_v<Field, bool> ||
                         std::is_enum_v<Field> || std::is_integral_v<Field>) {
        return ColumnType::kInteger;
    } else if constexpr (std::is_floating_point_v<Field>) {
        return ColumnType::kReal;
    } else if constexpr (std::is_same_v<Field, std::string>) {
        return ColumnType::kText;
    } else if constexpr (IsBlob<Field>::value) {
        return ColumnType::kBlob;
    } else {
        static_assert(kIsColumnType<Field>,
                      "ORM column type must be integer/enum/bool/float/double/"
                      "std::string/std::vector<std::uint8_t>/std::optional<T>");
        return ColumnType::kText;
    }
}

/**
 * @brief 把字段值写进 `Value`（供 `Stmt::BindValues` 使用）。
 * @note `optional` 无值时写 SQL NULL；BLOB 写非拥有视图，`BindValues` 内部
 *       会以 `SQLITE_TRANSIENT` 拷贝，因此字段在调用期间有效即可。
 */
template <typename T>
void BindField(const T& field, Value& out) {
    using Field = std::remove_cv_t<T>;
    if constexpr (IsOptional<Field>::value) {
        if (field.has_value()) {
            BindField(*field, out);
        } else {
            out = Value(nullptr);
        }
    } else if constexpr (std::is_same_v<Field, bool>) {
        out = Value(field);
    } else if constexpr (std::is_enum_v<Field>) {
        out = Value(static_cast<std::int64_t>(field));
    } else if constexpr (std::is_integral_v<Field>) {
        out = Value(static_cast<std::int64_t>(field));
    } else if constexpr (std::is_floating_point_v<Field>) {
        out = Value(static_cast<double>(field));
    } else if constexpr (std::is_same_v<Field, std::string>) {
        out = Value(std::string_view(field));
    } else if constexpr (IsBlob<Field>::value) {
        out = Value(Blob{field.data(), field.size()});
    } else {
        static_assert(kIsColumnType<Field>, "unsupported ORM field type");
    }
}

/**
 * @brief 从语句当前行读取第 `column` 列到字段。
 * @details 非 `optional` 字段遇到 SQL NULL 时按 SQLite 语义取 0/空串/空
 *          BLOB；`optional` 遇到 NULL 则 `reset()`。
 */
template <typename T>
void ReadField(const Stmt& statement, int column, T& field) {
    using Field = std::remove_cv_t<T>;
    if constexpr (IsOptional<Field>::value) {
        using Inner = typename Field::value_type;
        if (statement.IsNull(column)) {
            field.reset();
        } else {
            Inner value{};
            ReadField(statement, column, value);
            field = std::move(value);
        }
    } else if constexpr (std::is_same_v<Field, bool>) {
        field = statement.ColumnInt64(column) != 0;
    } else if constexpr (std::is_enum_v<Field>) {
        field = static_cast<Field>(statement.ColumnInt64(column));
    } else if constexpr (std::is_integral_v<Field>) {
        field = static_cast<Field>(statement.ColumnInt64(column));
    } else if constexpr (std::is_floating_point_v<Field>) {
        field = static_cast<Field>(statement.ColumnDouble(column));
    } else if constexpr (std::is_same_v<Field, std::string>) {
        field = statement.ColumnText(column);
    } else if constexpr (IsBlob<Field>::value) {
        const Blob bytes = statement.ColumnBlob(column);
        if (bytes.size == 0) {
            field.clear();
        } else {
            const auto* begin = static_cast<const std::uint8_t*>(bytes.data);
            field.assign(begin, begin + bytes.size);
        }
    } else {
        static_assert(kIsColumnType<Field>, "unsupported ORM field type");
    }
}

/// @brief 自增主键回填；只对整数/枚举/`optional<整数>` 字段生效，其余为 no-op。
template <typename T>
void AssignKey(T& field, std::int64_t key) {
    using Field = std::remove_cv_t<T>;
    if constexpr (IsOptional<Field>::value) {
        using Inner = typename Field::value_type;
        if constexpr (std::is_integral_v<Inner> || std::is_enum_v<Inner>) {
            field = static_cast<Inner>(key);
        }
    } else if constexpr (std::is_enum_v<Field>) {
        field = static_cast<Field>(key);
    } else if constexpr (std::is_integral_v<Field>) {
        field = static_cast<Field>(key);
    } else {
        (void)field;
        (void)key;
    }
}

// ===========================================================================
// 列构造 + 表映射
// ===========================================================================

/**
 * @brief 计算成员相对结构体起始的偏移。
 * @details 用临时探针对象取地址（与 `reflect::FieldOffset` 同思路），
 *          不用 `offsetof`——含 `std::string` 的结构体不是 standard-layout，
 *          `offsetof` 会触发 `-Winvalid-offsetof`。结构体不可默认构造时
 *          返回 0（偏移只用于诊断，不参与读写）。
 */
template <typename Class, typename Member>
std::size_t MemberOffset(Member Class::* member) {
    if constexpr (std::is_default_constructible_v<Class>) {
        Class probe{};
        const auto* base = reinterpret_cast<const unsigned char*>(&probe);
        const auto* field =
            reinterpret_cast<const unsigned char*>(&(probe.*member));
        return static_cast<std::size_t>(field - base);
    } else {
        (void)member;
        return 0;
    }
}

/**
 * @brief 构造一列的运行期描述（供 `GO2CPP_ORM_COLUMN` 使用）。
 * @param name 列名（宏里是 `#member`）。
 * @param member 成员指针，仅用于推导类型与偏移。
 * @param bind/read/assign 类型擦除回调。
 */
template <typename Class, typename Member>
Column MakeColumn(const char* name, Member Class::* member, BindFn bind,
                  ReadFn read, AssignFn assign) {
    Column column;
    column.name = name;
    column.type = ColumnTypeOf<Member>();
    column.size = sizeof(Member);
    column.offset = MemberOffset(member);
    column.bind = bind;
    column.read = read;
    column.assign = assign;
    return column;
}

/// @brief 表映射主模板；由 `GO2CPP_ORM_TABLE` / `GO2CPP_ORM_TABLE_REFLECT` 特化。
template <typename T>
struct Table;

/// @brief 检测 `Table<T>` 是否提供了映射。
template <typename T, typename = void>
struct HasTable : std::false_type {};

template <typename T>
struct HasTable<
    T, std::void_t<decltype(Table<T>::kTableName),
                   decltype(Table<T>::Columns(std::declval<std::size_t&>()))>>
    : std::true_type {};

/**
 * @brief 取 `T` 的表描述（静态存储，首次调用构建一次）。
 * @note 返回的 `TableDef` 与 `Table<T>::Columns()` 的静态数组同生命周期。
 */
template <typename T>
const TableDef& TableOf() {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    static_assert(HasTable<Value>::value,
                  "missing table mapping: use GO2CPP_ORM_TABLE(...) or "
                  "GO2CPP_ORM_TABLE_REFLECT(...) for this type");
    static const TableDef def = [] {
        std::size_t count = 0;
        const Column* columns = Table<Value>::Columns(count);
        TableDef table;
        table.name = Table<Value>::kTableName;
        table.columns = columns;
        table.count = count;
        return table;
    }();
    return def;
}

// ===========================================================================
// 映射宏
// ===========================================================================

/**
 * @brief 声明一个映射列。
 *
 * 用法：`GO2CPP_ORM_COLUMN(User, id).PrimaryKey().AutoIncrement()`
 *
 * @note 支持字段类型：整数/枚举/`bool`/浮点/`std::string`/
 *       `std::vector<std::uint8_t>`/上述类型的 `std::optional<T>`。
 */
#define GO2CPP_ORM_COLUMN(Class, member)                                           \
    ::go2cpp::storage::MakeColumn<Class>(                                          \
        #member, &Class::member,                                                   \
        [](const void* object, const void* context,                                \
           ::go2cpp::storage::Value& out) {                                        \
            (void)context;                                                         \
            ::go2cpp::storage::BindField(                                          \
                static_cast<const Class*>(object)->member, out);                   \
        },                                                                         \
        [](const ::go2cpp::storage::Stmt& statement, int column, void* object,     \
           const void* context) {                                                  \
            (void)context;                                                         \
            ::go2cpp::storage::ReadField(                                          \
                statement, column, static_cast<Class*>(object)->member);           \
        },                                                                         \
        [](void* object, std::int64_t key, const void* context) {                  \
            (void)context;                                                         \
            ::go2cpp::storage::AssignKey(                                          \
                static_cast<Class*>(object)->member, key);                         \
        })

/**
 * @brief 声明 `T` 的表映射。
 *
 * @param Class 结构体类型（不能含逗号；模板类型请先 `using`）。
 * @param table_name 表名字符串字面量。
 * @param ... 若干 `GO2CPP_ORM_COLUMN(Class, member)` 列声明。
 *
 * @code
 * GO2CPP_ORM_TABLE(User, "users",
 *                  GO2CPP_ORM_COLUMN(User, id).PrimaryKey().AutoIncrement(),
 *                  GO2CPP_ORM_COLUMN(User, name).NotNull().Unique(),
 *                  GO2CPP_ORM_COLUMN(User, age).Indexed(),
 *                  GO2CPP_ORM_COLUMN(User, avatar),
 *                  GO2CPP_ORM_COLUMN(User, email))
 * @endcode
 *
 * @note 宏必须在全局命名空间（或与 `Table` 相同的命名空间）展开；列数组
 *       是函数内静态存储，首次取表时初始化一次。
 */
#define GO2CPP_ORM_TABLE(Class, table_name, ...)                                   \
    template <>                                                                    \
    struct go2cpp::storage::Table<Class> {                                         \
        static constexpr const char* kTableName = table_name;                      \
        static const ::go2cpp::storage::Column* Columns(std::size_t& count) {      \
            static const ::go2cpp::storage::Column kColumns[] = {__VA_ARGS__};     \
            count = sizeof(kColumns) / sizeof(kColumns[0]);                        \
            return kColumns;                                                       \
        }                                                                          \
    };

#if defined(GO2CPP_STORAGE_ORM_HAS_REFLECT)

namespace detail {

/// @brief 反射列的上下文：字段偏移 + TypeId + 字段字节数。
struct ReflectColumnContext {
    std::size_t offset = 0;    ///< 相对结构体起始的偏移。
    std::uint32_t type_id = 0; ///< `reflect::TypeId`。
    std::size_t size = 0;      ///< `sizeof(字段类型)`。
};

/**
 * @brief 「TypeId × 字段字节数」能否精确还原 C++ 类型。
 * @details reflect 的整型 TypeId 只按「符号 + 是否 8 字节」分类，1/2/4 字节
 *          共用 `kInt32Type`/`kUint32Type`；靠 `size` 才能选对读写宽度。
 *          没有特化的类型（如 `float`，reflect 只给 `double` 固定 id）返回
 *          false，整表映射失败而不是按错误宽度读写。
 */
inline bool IsSupportedReflectColumn(std::uint32_t type_id,
                                     std::size_t size) noexcept {
    switch (type_id) {
        case reflect::kBoolType:
            return size == sizeof(bool);
        case reflect::kInt32Type:
        case reflect::kUint32Type:
            return size == 1 || size == 2 || size == 4;
        case reflect::kInt64Type:
        case reflect::kUint64Type:
            return size == sizeof(std::int64_t);
        case reflect::kDoubleType:
            return size == sizeof(double);
        case reflect::kStringType:
            return size == sizeof(std::string);
        case reflect::kBytesType:
            return size == sizeof(std::vector<std::uint8_t>);
        default:
            return false;
    }
}

/// @brief TypeId → 列类型。
inline ColumnType ReflectColumnType(std::uint32_t type_id) noexcept {
    switch (type_id) {
        case reflect::kDoubleType:
            return ColumnType::kReal;
        case reflect::kStringType:
            return ColumnType::kText;
        case reflect::kBytesType:
            return ColumnType::kBlob;
        default:
            return ColumnType::kInteger;
    }
}

/// @brief 反射字段 → `Value`（按 TypeId × size 分派，memcpy 避免别名问题）。
inline void BindReflected(const void* object, const void* context,
                          Value& out) {
    const auto* column = static_cast<const ReflectColumnContext*>(context);
    const auto* field =
        static_cast<const unsigned char*>(object) + column->offset;
    switch (column->type_id) {
        case reflect::kBoolType: {
            bool value = false;
            std::memcpy(&value, field, sizeof(value));
            out = Value(value);
            return;
        }
        case reflect::kInt32Type:
            switch (column->size) {
                case 1: {
                    std::int8_t value = 0;
                    std::memcpy(&value, field, sizeof(value));
                    out = Value(static_cast<std::int64_t>(value));
                    return;
                }
                case 2: {
                    std::int16_t value = 0;
                    std::memcpy(&value, field, sizeof(value));
                    out = Value(static_cast<std::int64_t>(value));
                    return;
                }
                default: {
                    std::int32_t value = 0;
                    std::memcpy(&value, field, sizeof(value));
                    out = Value(static_cast<std::int64_t>(value));
                    return;
                }
            }
        case reflect::kUint32Type:
            switch (column->size) {
                case 1: {
                    std::uint8_t value = 0;
                    std::memcpy(&value, field, sizeof(value));
                    out = Value(static_cast<std::int64_t>(value));
                    return;
                }
                case 2: {
                    std::uint16_t value = 0;
                    std::memcpy(&value, field, sizeof(value));
                    out = Value(static_cast<std::int64_t>(value));
                    return;
                }
                default: {
                    std::uint32_t value = 0;
                    std::memcpy(&value, field, sizeof(value));
                    out = Value(static_cast<std::int64_t>(value));
                    return;
                }
            }
        case reflect::kInt64Type: {
            std::int64_t value = 0;
            std::memcpy(&value, field, sizeof(value));
            out = Value(value);
            return;
        }
        case reflect::kUint64Type: {
            std::uint64_t value = 0;
            std::memcpy(&value, field, sizeof(value));
            out = Value(static_cast<std::int64_t>(value));
            return;
        }
        case reflect::kDoubleType: {
            double value = 0.0;
            std::memcpy(&value, field, sizeof(value));
            out = Value(value);
            return;
        }
        case reflect::kStringType: {
            out = Value(
                std::string_view(*reinterpret_cast<const std::string*>(field)));
            return;
        }
        case reflect::kBytesType: {
            const auto& bytes =
                *reinterpret_cast<const std::vector<std::uint8_t>*>(field);
            out = Value(Blob{bytes.data(), bytes.size()});
            return;
        }
        default:
            break;
    }
    out = Value(nullptr);
}

/// @brief 语句列 → 反射字段（与 BindReflected 对称）。
inline void ReadReflected(const Stmt& statement, int column, void* object,
                          const void* context) {
    const auto* meta = static_cast<const ReflectColumnContext*>(context);
    auto* field = static_cast<unsigned char*>(object) + meta->offset;
    switch (meta->type_id) {
        case reflect::kBoolType: {
            const bool value = statement.ColumnInt64(column) != 0;
            std::memcpy(field, &value, sizeof(value));
            return;
        }
        case reflect::kInt32Type:
            switch (meta->size) {
                case 1: {
                    const auto value = static_cast<std::int8_t>(
                        statement.ColumnInt64(column));
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
                case 2: {
                    const auto value = static_cast<std::int16_t>(
                        statement.ColumnInt64(column));
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
                default: {
                    const auto value = static_cast<std::int32_t>(
                        statement.ColumnInt64(column));
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
            }
        case reflect::kUint32Type:
            switch (meta->size) {
                case 1: {
                    const auto value = static_cast<std::uint8_t>(
                        statement.ColumnInt64(column));
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
                case 2: {
                    const auto value = static_cast<std::uint16_t>(
                        statement.ColumnInt64(column));
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
                default: {
                    const auto value = static_cast<std::uint32_t>(
                        statement.ColumnInt64(column));
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
            }
        case reflect::kInt64Type: {
            const std::int64_t value = statement.ColumnInt64(column);
            std::memcpy(field, &value, sizeof(value));
            return;
        }
        case reflect::kUint64Type: {
            const auto value =
                static_cast<std::uint64_t>(statement.ColumnInt64(column));
            std::memcpy(field, &value, sizeof(value));
            return;
        }
        case reflect::kDoubleType: {
            const double value = statement.ColumnDouble(column);
            std::memcpy(field, &value, sizeof(value));
            return;
        }
        case reflect::kStringType: {
            *reinterpret_cast<std::string*>(field) = statement.ColumnText(column);
            return;
        }
        case reflect::kBytesType: {
            auto& bytes = *reinterpret_cast<std::vector<std::uint8_t>*>(field);
            const Blob view = statement.ColumnBlob(column);
            if (view.size == 0) {
                bytes.clear();
            } else {
                const auto* begin =
                    static_cast<const std::uint8_t*>(view.data);
                bytes.assign(begin, begin + view.size);
            }
            return;
        }
        default:
            return;
    }
}

/// @brief 自增主键回填（只写整数列；其它类型主键不回填）。
inline void AssignReflected(void* object, std::int64_t key,
                            const void* context) {
    const auto* meta = static_cast<const ReflectColumnContext*>(context);
    auto* field = static_cast<unsigned char*>(object) + meta->offset;
    switch (meta->type_id) {
        case reflect::kInt32Type:
            switch (meta->size) {
                case 1: {
                    const auto value = static_cast<std::int8_t>(key);
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
                case 2: {
                    const auto value = static_cast<std::int16_t>(key);
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
                default: {
                    const auto value = static_cast<std::int32_t>(key);
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
            }
        case reflect::kUint32Type:
            switch (meta->size) {
                case 1: {
                    const auto value = static_cast<std::uint8_t>(key);
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
                case 2: {
                    const auto value = static_cast<std::uint16_t>(key);
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
                default: {
                    const auto value = static_cast<std::uint32_t>(key);
                    std::memcpy(field, &value, sizeof(value));
                    return;
                }
            }
        case reflect::kInt64Type: {
            std::memcpy(field, &key, sizeof(key));
            return;
        }
        case reflect::kUint64Type: {
            const auto value = static_cast<std::uint64_t>(key);
            std::memcpy(field, &value, sizeof(value));
            return;
        }
        default:
            return;
    }
}

} // namespace detail

/**
 * @brief 由 `reflect::FieldList<T>` 构建列描述（`GO2CPP_ORM_TABLE_REFLECT` 用）。
 * @param count 输出列数；映射不支持时输出 0。
 * @param primary_key 主键字段名；空串表示无主键（只读查询）。
 * @return 列数组（静态存储）；不支持的类型组合返回空表。
 *
 * @details 主键列若为整数类型自动加 `AUTOINCREMENT`；`uint64` 超过
 *          `INT64_MAX` 的部分无法用 SQLite INTEGER 表示（按位回绕，
 *          文档已标注）。
 */
template <typename T>
const Column* ReflectedColumns(std::size_t& count,
                               std::string_view primary_key) {
    static const std::vector<Column> columns =
        [primary = std::string(primary_key)] {
            std::vector<Column> built;
            if constexpr (!reflect::HasFields<T>()) {
                return built;
            } else {
                static const std::vector<detail::ReflectColumnContext>
                    contexts = [] {
                        std::vector<detail::ReflectColumnContext> meta;
                        std::size_t field_count = 0;
                        const auto* fields =
                            reflect::FieldList<T>::Fields(field_count);
                        meta.reserve(field_count);
                        for (std::size_t index = 0; index < field_count;
                             ++index) {
                            detail::ReflectColumnContext context;
                            context.offset = fields[index].offset;
                            context.type_id = fields[index].type_id;
                            context.size = fields[index].size;
                            meta.push_back(context);
                        }
                        return meta;
                    }();
                std::size_t field_count = 0;
                const auto* fields = reflect::FieldList<T>::Fields(field_count);
                built.reserve(field_count);
                for (std::size_t index = 0; index < field_count; ++index) {
                    const reflect::FieldDescriptor& field = fields[index];
                    if (!detail::IsSupportedReflectColumn(field.type_id,
                                                          field.size)) {
                        built.clear();
                        return built;
                    }
                    Column column;
                    column.name = field.name;
                    column.type = detail::ReflectColumnType(field.type_id);
                    column.size = field.size;
                    column.offset = field.offset;
                    column.bind = &detail::BindReflected;
                    column.read = &detail::ReadReflected;
                    column.context = &contexts[index];
                    if (!primary.empty() && primary == field.name) {
                        column.flags |= kColumnPrimaryKey;
                        if (column.type == ColumnType::kInteger) {
                            column.flags |= kColumnAutoIncrement;
                            column.assign = &detail::AssignReflected;
                        }
                    }
                    built.push_back(std::move(column));
                }
                return built;
            }
        }();
    count = columns.size();
    return columns.data();
}

/**
 * @brief 用 `reflect::FieldList<T>` 自动生成表映射。
 *
 * @param Class 结构体类型（已用 `GO2CPP_REFLECT_FIELD` 声明字段表）。
 * @param table_name 表名字符串字面量。
 * @param primary_key_name 主键字段名（字符串字面量；空串表示无主键）。
 *
 * @code
 * GO2CPP_ORM_TABLE_REFLECT(User, "users", "id")
 * @endcode
 *
 * @note 只支持 `bool`/1/2/4/8 字节整型/`double`/`std::string`/
 *       `std::vector<std::uint8_t>`；出现其它类型（如 `float`、嵌套结构体、
 *       `optional`）时整表映射为空，首次 CRUD 返回 `kErrMapping`。
 */
#define GO2CPP_ORM_TABLE_REFLECT(Class, table_name, primary_key_name)              \
    template <>                                                                    \
    struct go2cpp::storage::Table<Class> {                                         \
        static constexpr const char* kTableName = table_name;                      \
        static const ::go2cpp::storage::Column* Columns(std::size_t& count) {      \
            return ::go2cpp::storage::ReflectedColumns<Class>(                     \
                count, primary_key_name);                                          \
        }                                                                          \
    };

#else

/// @brief 未提供 reflect 模块时的占位宏：给出清晰的编译错误。
#define GO2CPP_ORM_TABLE_REFLECT(Class, table_name, primary_key_name)              \
    static_assert(false,                                                           \
                  "GO2CPP_ORM_TABLE_REFLECT requires go2cpp/reflect/field.hpp")

#endif // GO2CPP_STORAGE_ORM_HAS_REFLECT

// ===========================================================================
// SQL 生成辅助（实现见 orm.cpp）
// ===========================================================================

/// @brief 双引号转义标识符（内部 `"` 翻倍）。
std::string QuoteIdentifier(std::string_view identifier);
/// @brief 生成 `CREATE TABLE IF NOT EXISTS`（含列级约束 + `Indexed()` 索引）。
std::string CreateTableSql(const TableDef& table);
/// @brief 生成 `DROP TABLE IF EXISTS`。
std::string DropTableSql(std::string_view table);
/// @brief 生成 `CREATE [UNIQUE] INDEX IF NOT EXISTS`；columns 按顺序转义。
std::string CreateIndexSql(std::string_view index_name, std::string_view table,
                           const std::vector<std::string>& columns,
                           bool unique = false);
/// @brief 生成 `ALTER TABLE ... ADD COLUMN`。
std::string AddColumnSql(std::string_view table, std::string_view column,
                         ColumnType type, bool not_null = false,
                         std::string_view default_sql = {});
/// @brief 生成 `ALTER TABLE ... DROP COLUMN`（SQLite 3.35+）。
std::string DropColumnSql(std::string_view table, std::string_view column);

// ===========================================================================
// Session / Query
// ===========================================================================

template <typename T>
class Query;

/// @brief `Session::ForEachRow` 的行回调；返回 false 提前结束遍历。
using RowSink = bool (*)(void* context, const Stmt& statement);

/**
 * @brief 一个 `DB` 上的 ORM 工作会话。
 *
 * @details 持有：
 *  - 表状态缓存（每表一次：SQL 文本、参与插入/更新的列、主键位置、绑定缓冲）；
 *  - 语句缓存（`SQL 文本 → Stmt`），热路径不重复 prepare；
 *  - 递归互斥量：CRUD 与 `Transaction(fn)` 可重入（事务体内继续用本会话）。
 *
 * 所有公开方法返回 `Error`/`Result<T>`，不抛异常。`Session` 不拥有 `DB`，
 * 必须保证 `DB` 比 `Session` 活得久。
 */
class Session {
public:
    /// @brief 绑定到一个已打开的连接；不转移所有权。
    explicit Session(DB& db);
    /// @brief 析构：释放语句缓存（`Stmt` 自行 finalize）。
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    /// @brief 底层连接。
    DB& Raw() noexcept { return *m_db; }
    /// @brief 底层连接的 const 版本。
    const DB& Raw() const noexcept { return *m_db; }

    /// @brief 插入一行；成功后把自增主键回填到 `value` 的主键字段。
    template <typename T>
    Result<std::int64_t> Insert(T& value) {
        return InsertImpl(TableOf<T>(), &value, true);
    }

    /// @brief 插入一行（`const` 对象，不回填主键）。
    template <typename T>
    Result<std::int64_t> Insert(const T& value) {
        return InsertImpl(
            TableOf<T>(), const_cast<void*>(static_cast<const void*>(&value)),
            false);
    }

    /// @brief 按主键更新其余列；返回受影响行数（0 表示主键不存在）。
    template <typename T>
    Result<int> Update(const T& value) {
        return UpdateImpl(TableOf<T>(), &value);
    }

    /// @brief 按主键删除；返回受影响行数（0 表示不存在）。
    template <typename T>
    Result<int> Delete(std::int64_t key) {
        return DeleteImpl(TableOf<T>(), key);
    }

    /// @brief 按主键查询；不存在返回 `kErrNotFound`。
    template <typename T>
    Result<T> Find(std::int64_t key) {
        T value{};
        auto found = FindImpl(TableOf<T>(), key, &value);
        if (!found) {
            return found.Err();
        }
        if (!found.Value()) {
            return Error(kErrNotFound, 0, "row not found");
        }
        return value;
    }

    /// @brief 查询构造器入口：`session.Query<User>().Where(...).All()`。
    template <typename T>
    Query<T> Query();

    /// @brief 全表扫描并物化为 `std::vector<T>`。
    template <typename T>
    Result<std::vector<T>> All() {
        return Query<T>().All();
    }

    /// @brief 全表行数。
    template <typename T>
    Result<std::int64_t> Count() {
        return Query<T>().Count();
    }

    /**
     * @brief 在事务中执行 `body`；正常返回则提交，返回错误/抛异常则回滚。
     * @param body 无参可调用对象，返回 `Error` 或 `void`。
     * @return 提交结果，或 `body` 的错误（已回滚），或提交错误。
     *
     * @details 事务持有会话锁；同一线程在 `body` 内继续调用本会话时通过
     *          thread-local 标记跳过重复加锁（不重入锁，热路径也不用
     *          recursive_mutex）。`body` 抛异常时由 `storage::Transaction`
     *          的 RAII 回滚后继续向外传播。
     * @warning 不要在 `body` 里把同一个 `Session` 交给其它线程使用：
     *          那个线程会阻塞在会话锁上直到事务结束。
     */
    template <typename F>
    Error Transaction(F&& body) {
        using ResultType = std::invoke_result_t<F&>;
        static_assert(std::is_same_v<ResultType, Error> ||
                          std::is_same_v<ResultType, void>,
                      "Session::Transaction body must return Error or void");
        std::unique_lock<std::mutex> lock(m_mutex, std::defer_lock);
        if (t_transaction_owner != this) {
            lock.lock();
        }
        if (m_db->InTransaction()) {
            return Error(kErrTransaction, 0,
                         "Transaction() called inside an active transaction");
        }
        storage::Transaction transaction(*m_db);
        if (Error error = transaction.Begin()) {
            return error;
        }
        // 事务体内同线程调用不再加锁；离开作用域（含异常）恢复原标记。
        TransactionOwnerGuard guard{t_transaction_owner};
        t_transaction_owner = this;
        if constexpr (std::is_same_v<ResultType, Error>) {
            if (Error error = body()) {
                (void)transaction.Rollback();
                return error;
            }
        } else {
            body();
        }
        return transaction.Commit();
    }

    // ---- 非模板内核（orm.cpp）；模板层只是加映射与类型转换 ----

    /// @brief 插入内核：`object` 指向具体结构体。
    Result<std::int64_t> InsertImpl(const TableDef& table, void* object,
                                    bool fill_key);
    /// @brief 更新内核。
    Result<int> UpdateImpl(const TableDef& table, const void* object);
    /// @brief 删除内核。
    Result<int> DeleteImpl(const TableDef& table, std::int64_t key);
    /// @brief 按主键查找内核；`Value()` 为 false 表示没有该行。
    Result<bool> FindImpl(const TableDef& table, std::int64_t key, void* object);
    /// @brief 执行查询并逐行回调；返回行数。
    Result<std::int64_t> ForEachRow(const std::string& sql,
                                    const Value* params,
                                    std::size_t param_count, RowSink sink,
                                    void* context);

    /// @brief 当前缓存的语句条数（诊断/测试用）。
    std::size_t CachedStatementCount() const;
    /// @brief 清空语句缓存（会 finalize 全部缓存语句）。
    void ClearStatementCache();

private:
    struct TableState;

    /// @brief 事务期间恢复 thread-local 标记的 RAII 守卫。
    struct TransactionOwnerGuard {
        Session* previous = nullptr;
        ~TransactionOwnerGuard() { Session::t_transaction_owner = previous; }
    };

    /// @brief 取/建表状态；失败写 `error` 并返回 nullptr。
    TableState* StateLocked(const TableDef& table, Error& error);
    /// @brief 取/建预编译语句；失败写 `error` 并返回 nullptr。
    Stmt* StatementLocked(const std::string& sql, Error& error);

    /// @brief 加会话锁；事务 body 内同线程已持锁时返回未上锁的 unique_lock。
    /// @note 非事务路径每次加锁（互斥量无竞争快路径）；事务路径零锁。
    std::unique_lock<std::mutex> Lock() const {
        std::unique_lock<std::mutex> lock(m_mutex, std::defer_lock);
        if (t_transaction_owner != this) {
            lock.lock();
        }
        return lock;
    }

    DB* m_db = nullptr;
    /// @brief 缓存的原生句柄：避免每行 `LastInsertRowId()`/`Changes()` 走
    ///        `DB` 的互斥量与 `Handle()`。生命周期由 `DB` 保证（Session 不得
    ///        比 DB 活得久）。
    sqlite3* m_raw = nullptr;
    mutable std::mutex m_mutex;
    /// @brief 当前线程正持有本会话事务的标记（见 Transaction）。
    static thread_local Session* t_transaction_owner;
    std::unordered_map<const void*, std::unique_ptr<TableState>> m_tables;
    std::unordered_map<std::string, Stmt> m_statements;
    /// @brief 最近使用的表状态：单表热路径免哈希查找。
    const void* m_last_table_key = nullptr;
    TableState* m_last_table = nullptr;
};

/**
 * @brief 链式查询构造器（运行期字符串 + 参数绑定，不解析 SQL 表达式树）。
 *
 * @details `Where` 子句原样拼接（参数用 `?` 绑定），多次 `Where` 以 `AND`
 *          连接。`OrderBy` 同样是原样表达式；只想按列名排序时用
 *          `OrderByColumn`（自动转义）。`Limit`/`Offset` 生成 `LIMIT`/`OFFSET`。
 *
 * @warning `Where`/`OrderBy` 的字符串是**调用方输入**，不要拼接用户数据；
 *          用户数据一律走参数占位符。
 */
template <typename T>
class Query {
public:
    /// @brief 由 `Session::Query<T>()` 构造。
    Query(Session& session, const TableDef& table)
        : m_session(&session), m_table(&table) {}

    /// @brief 追加一个 `AND` 条件；参数按顺序绑定到 `?`。
    template <typename... Args>
    Query& Where(std::string clause, Args&&... args) {
        if (!m_where.empty()) {
            m_where += " AND ";
        }
        m_where += "(";
        m_where += clause;
        m_where += ")";
        (m_params.emplace_back(std::forward<Args>(args)), ...);
        return *this;
    }

    /// @brief 追加排序表达式（原样拼接，可含 `DESC`/多个列）。
    Query& OrderBy(std::string expression) {
        if (!m_order.empty()) {
            m_order += ", ";
        }
        m_order += expression;
        return *this;
    }

    /// @brief 按列名排序（自动双引号转义）。
    Query& OrderByColumn(std::string_view column, bool descending = false) {
        if (!m_order.empty()) {
            m_order += ", ";
        }
        m_order += QuoteIdentifier(column);
        m_order += descending ? " DESC" : " ASC";
        return *this;
    }

    /// @brief 最多返回 `count` 行；负数表示不限。
    Query& Limit(std::int64_t count) {
        m_limit = count;
        return *this;
    }

    /// @brief 跳过前 `count` 行。
    Query& Offset(std::int64_t count) {
        m_offset = count;
        return *this;
    }

    /// @brief 生成的 SELECT 文本（调试/测试用，不含绑定值）。
    std::string Sql() const { return BuildSelect(-1); }

    /// @brief 执行查询，逐行构造 `T` 并回调；返回行数。
    template <typename F>
    Result<std::int64_t> Each(F&& fn) {
        using CallResult = std::invoke_result_t<F&, const T&>;
        struct SinkContext {
            F* fn;
        };
        const std::string sql = BuildSelect(-1);
        SinkContext context{&fn};
        return m_session->ForEachRow(
            sql, m_params.data(), m_params.size(),
            [](void* raw, const Stmt& statement) -> bool {
                auto* sink = static_cast<SinkContext*>(raw);
                T value{};
                ReadRow(statement, value);
                if constexpr (std::is_same_v<CallResult, bool>) {
                    return (*sink->fn)(value);
                } else {
                    (*sink->fn)(value);
                    return true;
                }
            },
            &context);
    }

    /// @brief 执行查询并物化为 `std::vector<T>`。
    Result<std::vector<T>> All() {
        std::vector<T> rows;
        if (m_limit > 0) {
            rows.reserve(static_cast<std::size_t>(m_limit));
        }
        auto walked = Each([&rows](const T& row) { rows.push_back(row); });
        if (!walked) {
            return walked.Err();
        }
        return rows;
    }

    /// @brief 取第一行；没有行返回 `kErrNotFound`。
    Result<T> First() {
        T value{};
        bool found = false;
        std::pair<T*, bool*> context{&value, &found};
        const std::string sql = BuildSelect(1);
        auto walked = m_session->ForEachRow(
            sql, m_params.data(), m_params.size(),
            [](void* raw, const Stmt& statement) -> bool {
                auto* pair = static_cast<std::pair<T*, bool*>*>(raw);
                ReadRow(statement, *pair->first);
                *pair->second = true;
                return false;
            },
            &context);
        if (!walked) {
            return walked.Err();
        }
        if (!found) {
            return Error(kErrNotFound, 0, "row not found");
        }
        return value;
    }

    /// @brief 取第一行；没有行返回 `std::nullopt`。
    Result<std::optional<T>> FirstOptional() {
        auto first = First();
        if (first.ok()) {
            return std::optional<T>(std::move(first.Value()));
        }
        if (first.Err().Code() == kErrNotFound) {
            return std::optional<T>{};
        }
        return first.Err();
    }

    /// @brief 满足条件的行数（忽略 `OrderBy`/`Limit`/`Offset`）。
    Result<std::int64_t> Count() {
        std::string sql =
            "SELECT COUNT(*) FROM " + QuoteIdentifier(m_table->name);
        if (!m_where.empty()) {
            sql += " WHERE ";
            sql += m_where;
        }
        std::int64_t total = 0;
        auto walked = m_session->ForEachRow(
            sql, m_params.data(), m_params.size(),
            [](void* raw, const Stmt& statement) -> bool {
                *static_cast<std::int64_t*>(raw) = statement.ColumnInt64(0);
                return false;
            },
            &total);
        if (!walked) {
            return walked.Err();
        }
        return total;
    }

private:
    /// @brief 读一行到 `value`（列顺序与映射顺序一致，越界安全）。
    static void ReadRow(const Stmt& statement, T& value) {
        const TableDef& table = TableOf<T>();
        const int available = statement.ColumnCount();
        std::size_t count = table.count;
        if (count > static_cast<std::size_t>(available)) {
            count = static_cast<std::size_t>(available);
        }
        for (std::size_t index = 0; index < count; ++index) {
            const Column& column = table.columns[index];
            if (column.read != nullptr) {
                column.read(statement, static_cast<int>(index), &value,
                            column.context);
            }
        }
    }

    /// @brief 生成 SELECT；`limit_override >= 0` 时覆盖 LIMIT（First 用）。
    std::string BuildSelect(std::int64_t limit_override) const {
        std::string sql = "SELECT ";
        for (std::size_t index = 0; index < m_table->count; ++index) {
            if (index != 0) {
                sql += ", ";
            }
            sql += QuoteIdentifier(m_table->columns[index].name);
        }
        sql += " FROM ";
        sql += QuoteIdentifier(m_table->name);
        if (!m_where.empty()) {
            sql += " WHERE ";
            sql += m_where;
        }
        if (!m_order.empty()) {
            sql += " ORDER BY ";
            sql += m_order;
        }
        const std::int64_t limit =
            limit_override >= 0 ? limit_override : m_limit;
        if (limit >= 0 || m_offset > 0) {
            sql += " LIMIT ";
            sql += std::to_string(limit >= 0 ? limit : -1);
            if (m_offset > 0) {
                sql += " OFFSET ";
                sql += std::to_string(m_offset);
            }
        }
        return sql;
    }

    Session* m_session = nullptr;
    const TableDef* m_table = nullptr;
    std::vector<Value> m_params;
    std::string m_where;
    std::string m_order;
    std::int64_t m_limit = -1;
    std::int64_t m_offset = 0;
};

template <typename T>
Query<T> Session::Query() {
    return ::go2cpp::storage::Query<T>(*this, TableOf<T>());
}

// ===========================================================================
// 迁移
// ===========================================================================

/// @brief 一条迁移声明。
struct MigrationStep {
    std::int64_t version = 0; ///< 版本号（必须严格递增）。
    std::string name;         ///< 迁移名（写入版本表）。
    std::string up_sql;       ///< 升级 SQL（可多条语句）。
    std::string down_sql;     ///< 回滚 SQL（为空表示不可回滚）。
};

/// @brief 版本表里的一行（已应用迁移）。
struct AppliedMigration {
    std::int64_t version = 0;
    std::string name;
    std::int64_t applied_at = 0;  ///< Unix 秒。
    std::int64_t duration_us = 0; ///< 该迁移执行耗时（微秒）。
};

/**
 * @brief 版本化迁移管理器。
 *
 * @details 版本表默认 `_go2cpp_migrations`，结构：
 *
 * ```sql
 * CREATE TABLE IF NOT EXISTS "_go2cpp_migrations" (
 *     "version"     INTEGER PRIMARY KEY,
 *     "name"        TEXT    NOT NULL,
 *     "applied_at"  INTEGER NOT NULL,
 *     "duration_us" INTEGER NOT NULL DEFAULT 0
 * )
 * ```
 *
 * 每条迁移在**独立事务**中执行：`BEGIN IMMEDIATE` → `up_sql` → 写版本行
 * → `COMMIT`；任一步失败回滚该条迁移并返回带版本号的错误。SQLite 的 DDL
 * 是事务性的，因此失败不会留下半张表。
 */
class Migration {
public:
    /// @brief 绑定连接；`version_table` 可自定义版本表名。
    explicit Migration(DB& db, std::string version_table = "_go2cpp_migrations");
    /// @brief 析构。
    ~Migration();
    Migration(const Migration&) = delete;
    Migration& operator=(const Migration&) = delete;

    /// @brief 追加一条迁移；版本号必须严格递增。
    Migration& Add(std::int64_t version, std::string name, std::string up_sql,
                   std::string down_sql = {});
    /// @brief 追加「按映射建表 + 索引」迁移；`down` 为 DROP TABLE。
    template <typename T>
    Migration& AddCreateTable(std::int64_t version, std::string name) {
        const TableDef& table = TableOf<T>();
        return Add(version, std::move(name), CreateTableSql(table),
                   DropTableSql(table.name));
    }
    /// @brief 追加「建索引」迁移；`down` 为 DROP INDEX。
    Migration& AddCreateIndex(std::int64_t version, std::string name,
                              std::string index_name, std::string table,
                              std::vector<std::string> columns,
                              bool unique = false);
    /// @brief 追加「加列」迁移；`down` 为 DROP COLUMN（SQLite 3.35+）。
    Migration& AddColumn(std::int64_t version, std::string name,
                         std::string table, std::string column,
                         ColumnType type, bool not_null = false,
                         std::string default_sql = {});
    /// @brief 追加「删列」迁移（`down` 只能重建空列，数据不可恢复）。
    Migration& AddDropColumn(std::int64_t version, std::string name,
                             std::string table, std::string column,
                             ColumnType type, bool not_null = false,
                             std::string default_sql = {});

    /// @brief 应用全部未应用的迁移（按版本升序）。
    Error Up();
    /// @brief 升级到 `target` 版本（含）。
    Error UpTo(std::int64_t target);
    /// @brief 回滚最近 `steps` 条已应用迁移。
    Error Down(int steps = 1);
    /// @brief 回滚到 `target` 版本（保留 <= target 的迁移）。
    Error DownTo(std::int64_t target);
    /// @brief 自动升/降级到 `target`。
    Error MigrateTo(std::int64_t target);

    /// @brief 版本表当前最大版本；无记录返回 0。
    Result<std::int64_t> CurrentVersion();
    /// @brief 版本表全部记录（按版本升序）。
    Result<std::vector<AppliedMigration>> Applied();
    /// @brief 已声明的迁移列表。
    const std::vector<MigrationStep>& Steps() const noexcept { return m_steps; }
    /// @brief 版本表名。
    const std::string& VersionTable() const noexcept { return m_version_table; }

private:
    /// @brief 确保版本表存在。
    Error EnsureTableLocked();
    /// @brief 版本表已应用版本集合。
    Result<std::vector<std::int64_t>> AppliedVersionsLocked();
    /// @brief 执行单条迁移（up 或 down）并更新版本表。
    Error ApplyLocked(const MigrationStep& step, bool up);
    /// @brief 升级到 `target`（`INT64_MAX` 表示全部）。
    Error UpToLocked(std::int64_t target);
    /// @brief 回滚到 `target`（保留 <= target 的迁移）。
    Error DownToLocked(std::int64_t target);

    DB* m_db = nullptr;
    std::string m_version_table;
    std::vector<MigrationStep> m_steps;
    Error m_build_error; ///< 声明期错误（版本号非递增、低版本 DROP COLUMN 等）。
    std::recursive_mutex m_mutex;
};

} // namespace storage
} // namespace go2cpp
