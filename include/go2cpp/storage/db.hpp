/**
 * @file include/go2cpp/storage/db.hpp
 * @brief SQLite 嵌入式存储层：Go 风格的 DB / Stmt / Rows / 事务封装。
 *
 * ## 模块定位
 *
 * 位于**存储基础设施层**，向下只依赖系统 `libsqlite3`（3.8+ 已装）与 C++17
 * 标准库；向上给业务代码提供与 Go `database/sql` 形状接近的接口，避免调用方
 * 直接操作 `sqlite3_stmt*` 的生命周期、错误码与字符串所有权。
 *
 * 本模块**不**是 ORM，也不隐藏 SQL：SQL 文本、PRAGMA、事务边界仍由调用方
 * 书写，封装只负责资源管理、参数绑定、错误翻译与线程安全约定。
 *
 * ## 本文件定义什么
 *
 * - `Error`：不可变错误值（primary/extended 错误码 + 文本），替代 `sqlite3*`
 *   的错误轮询；
 * - `Value` / `Blob`：参数与列值的类型安全盒子（NULL/int64/double/text/blob/bool）；
 * - `Result<T>`：Go 风格 `(value, error)` 的 C++ 表达；
 * - `Stmt`：预编译语句（RAII，可复用；`Bind`/`Step`/`Reset`/列访问）；
 * - `Rows`：查询结果游标（`Next()` + `Err()`，列访问转发给 `Stmt`）；
 * - `DB`：连接封装（`Open`/`Close`/`Exec`/`Query`/`Prepare`/事务/PRAGMA）；
 * - `Transaction`：`Begin`/`Commit`/`Rollback` 的 RAII 辅助（析构自动回滚）。
 *
 * ## 阅读建议
 *
 * 先看 `DB::Exec` / `DB::Query` 的模板入口，再看 `ExecImpl`（多语句、参数
 * 计数、错误锁存）与 `Stmt::Step`（ROW/DONE/错误三态）；事务语义见
 * `DB::Begin/Commit/Rollback` 与 `Transaction`。性能数据与 objdump 证据见
 * `docs/modules/storage-sqlite.md`。
 *
 * ## 边界与陷阱
 *
 * - **线程模型**：`DB` 的公开方法内部用互斥量串行化，可跨线程共享；`Stmt`
 *   与 `Rows` 是**单线程对象**，同一语句不得并发 `Step`（SQLite 允许
 *   跨线程使用连接，但不允许同一 stmt 并发执行）。多线程写请每线程一个
 *   `DB`，并开启 WAL（`SetJournalMode("WAL")`）。
 * - **生命周期**：`Stmt`/`Rows` 不得比其 `DB` 活得更久（`DB::Close` 之后
 *   对语句的操作返回 `Code::kMisuse`，不会崩溃，但数据不可再依赖）。
 * - **文本所有权**：`ColumnTextView` 指向 SQLite 的内部缓冲，仅在该行被
 *   `Next()` 覆盖或语句 `Reset`/`Finalize` 前有效；需要保留时用
 *   `ColumnText`（拷贝）。
 * - **参数计数严格匹配**：绑定个数与 `?` 占位符个数不一致直接报
 *   `Code::kMisuse`，不做“缺省补 NULL”的猜测，避免静默写错数据。
 * - **多语句**：`Exec` 支持一次执行多条无参语句（脚本）；带参数时只允许
 *   单条语句，避免参数归属歧义。
 * - **WAL**：`SetJournalMode("WAL")` 返回后校验 SQLite 实际返回的模式；
 *   `:memory:` 数据库不支持 WAL，会原样返回 `memory`（调用方按需判断）。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// 只在公共头里前置声明，不把 sqlite3.h 的宏/结构泄漏给使用方；
// 需要原生句柄的调用方可以自己 #include <sqlite3.h> 后使用 Handle()。
struct sqlite3;
struct sqlite3_stmt;

namespace go2cpp {
namespace storage {

/**
 * @brief SQLite primary result code 的镜像。
 * @details 值与 SQLite 公开 ABI 一致（sqlite3.h 的 SQLITE_*），因此调用方
 *          不需要包含 sqlite3.h 就能做 `err.Code() == Code::kBusy` 判断。
 */
enum class Code : int {
    kOk = 0,          ///< 成功。
    kError = 1,       ///< 通用 SQL 错误。
    kInternal = 2,    ///< 内部逻辑错误。
    kPerm = 3,        ///< 无权限。
    kAbort = 4,       ///< 回调请求中止。
    kBusy = 5,        ///< 数据库被占用（并发写锁）。
    kLocked = 6,      ///< 表/数据库被锁。
    kNoMem = 7,       ///< 内存不足。
    kReadOnly = 8,    ///< 只读数据库。
    kInterrupt = 9,   ///< 被 sqlite3_interrupt 打断。
    kIoErr = 10,      ///< 磁盘 IO 错误。
    kCorrupt = 11,    ///< 数据库损坏。
    kNotFound = 12,   ///< 内部未找到。
    kFull = 13,       ///< 磁盘满。
    kCantOpen = 14,   ///< 无法打开文件。
    kProtocol = 15,   ///< 锁协议错误。
    kEmpty = 16,      ///< 空数据库。
    kSchema = 17,     ///< schema 已变化。
    kTooBig = 18,     ///< 值过大。
    kConstraint = 19, ///< 约束冲突（UNIQUE/NOT NULL/CHECK）。
    kMismatch = 20,   ///< 类型不匹配。
    kMisuse = 21,     ///< API 误用（参数个数错误、语句已关闭等）。
    kNoLfs = 22,      ///< 不支持大文件。
    kAuth = 23,       ///< 授权失败。
    kFormat = 24,     ///< 格式错误。
    kRange = 25,      ///< 参数越界。
    kNotADb = 26,     ///< 不是数据库文件。
    kNotice = 27,     ///< 通知。
    kWarning = 28,    ///< 警告。
    kRow = 100,       ///< 结果集还有一行（仅 Step 内部使用）。
    kDone = 101,      ///< 语句执行完毕（仅 Step 内部使用）。
};

/**
 * @brief 不可变错误值：primary code + extended code + 文本。
 * @details 空错误（默认构造）表示成功，`operator bool` 为 false；非空错误
 *          `operator bool` 为 true。错误文本在构造时定格，不受后续 SQLite
 *          调用影响，因此可以安全地跨线程传递。
 */
class Error {
public:
    /// @brief 构造成功（空）错误。
    Error() noexcept = default;
    /// @brief 构造带错误码与文本的错误；extended_code 可为 0。
    Error(int code, int extended_code, std::string message);

    /// @brief 是否成功（错误码为 0）。
    bool ok() const noexcept { return m_code == 0; }
    /// @brief 是否失败；与 `!ok()` 等价，便于 `if (err) ...`。
    explicit operator bool() const noexcept { return m_code != 0; }

    /// @brief SQLite primary result code。
    int Code() const noexcept { return m_code; }
    /// @brief SQLite extended result code；未启用扩展码时为 0。
    int ExtendedCode() const noexcept { return m_extended_code; }
    /// @brief 面向用户的错误文本；成功时为空串。
    const std::string& Message() const noexcept { return m_message; }
    /// @brief 是否为并发写锁（BUSY / BUSY_SNAPSHOT / BUSY_TIMEOUT 等）。
    bool IsBusy() const noexcept;
    /// @brief 是否为约束冲突（UNIQUE/NOT NULL/CHECK/FOREIGN KEY）。
    bool IsConstraint() const noexcept;
    /// @brief 形如 `SQLITE_BUSY(5/517): database is locked` 的完整文本。
    std::string ToString() const;

private:
    int m_code = 0;
    int m_extended_code = 0;
    std::string m_message;
};

/**
 * @brief 非拥有的二进制块视图（参数绑定用）。
 * @details `Value(Blob)` 绑定的是 SQLite 的 `SQLITE_TRANSIENT`，即绑定那一刻
 *          拷贝进语句；`Blob::data` 在 `Bind`/`Exec` 返回后即可释放。
 */
struct Blob {
    const void* data = nullptr; ///< 只读字节起点；可为空。
    std::size_t size = 0;       ///< 字节数。

    Blob() noexcept = default;
    Blob(const void* bytes, std::size_t length) noexcept
        : data(bytes), size(length) {}
};

/**
 * @brief 单个 SQL 参数/列值的类型安全盒子。
 * @details 构造接口覆盖整型、浮点、bool、字符串、string_view、Blob 与
 *          nullptr；整型统一存为 int64（SQLite 只有 64 位整数），浮点统一
 *          存为 double，bool 在绑定时写 0/1（读列时由调用方用
 *          `ColumnType`/`ColumnInt64` 判断）。
 */
class Value {
public:
    /// @brief 值类型；对应 SQLite 的 NULL/INTEGER/REAL/TEXT/BLOB。
    enum class Type : std::uint8_t {
        kNull = 0, ///< SQL NULL。
        kInt64,    ///< 64 位整数。
        kDouble,   ///< 双精度浮点。
        kText,     ///< UTF-8 文本（拷贝持有）。
        kBlob,     ///< 二进制块（绑定非拥有视图）。
        kBool,     ///< 布尔（绑定为 0/1）。
    };

    /// @brief 默认构造为 SQL NULL。
    Value() noexcept = default;
    /// @brief nullptr 构造为 SQL NULL。
    Value(std::nullptr_t) noexcept {}

    /// @brief 任意整型（char/short/int/long/long long/无符号）转 int64。
    template <typename T,
              std::enable_if_t<std::is_integral_v<T> &&
                                   !std::is_same_v<T, bool>,
                               int> = 0>
    Value(T number) noexcept
        : m_type(Type::kInt64), m_int(static_cast<std::int64_t>(number)) {}

    /// @brief 任意浮点转 double。
    template <typename T,
              std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    Value(T number) noexcept
        : m_type(Type::kDouble), m_double(static_cast<double>(number)) {}

    /// @brief bool 绑定为 1/0。
    Value(bool boolean) noexcept : m_type(Type::kBool), m_int(boolean ? 1 : 0) {}
    /// @brief 文本（拷贝持有，绑定 SQLITE_TRANSIENT）。
    Value(std::string text) : m_type(Type::kText), m_text(std::move(text)) {}
    /// @brief 文本视图（拷贝持有）。
    Value(std::string_view text) : m_type(Type::kText), m_text(text) {}
    /// @brief C 字符串；nullptr 视为 SQL NULL。
    Value(const char* text);
    /// @brief 二进制块（绑定非拥有视图，拷贝发生在 Bind/Exec 内部）。
    Value(Blob bytes) noexcept : m_type(Type::kBlob), m_blob(bytes) {}

    /// @brief 当前值类型。
    Type type() const noexcept { return m_type; }
    /// @brief 是否为 SQL NULL。
    bool IsNull() const noexcept { return m_type == Type::kNull; }

    /// @brief 读 int64（整数与 bool 返回存储值，其它类型返回 0）。
    std::int64_t Int64() const noexcept {
        return m_type == Type::kInt64 || m_type == Type::kBool ? m_int : 0;
    }
    /// @brief 读 double（非浮点类型返回 0）。
    double Double() const noexcept {
        return m_type == Type::kDouble ? m_double : 0.0;
    }
    /// @brief 读 bool（仅 kBool 且非 0 时为 true）。
    bool Bool() const noexcept {
        return m_type == Type::kBool && m_int != 0;
    }
    /// @brief 读文本（非文本类型返回空串）。
    const std::string& Text() const noexcept { return m_text; }
    /// @brief 读二进制视图。
    const Blob& BlobData() const noexcept { return m_blob; }

private:
    Type m_type = Type::kNull;
    std::int64_t m_int = 0;
    double m_double = 0.0;
    std::string m_text;
    Blob m_blob;
};

/**
 * @brief 把可变参数转成栈上 `std::array<Value, N>`，避免每次调用堆分配。
 * @note 文本参数的 std::string 仍在 Value 内部分配（短文本走 SSO）。
 */
template <typename... Args>
std::array<Value, sizeof...(Args)> MakeValues(const Args&... args) {
    return std::array<Value, sizeof...(Args)>{{Value(args)...}};
}

/**
 * @brief Go 风格 `(value, error)`：成功取 `Value()`，失败取 `Err()`。
 * @tparam T 值类型；需要可移动构造与默认构造。
 */
template <typename T>
class Result {
public:
    /// @brief 成功构造。
    Result(T value) : m_ok(true), m_value(std::move(value)) {}
    /// @brief 失败构造（此时 Value 为默认值，不可读）。
    Result(Error error) : m_ok(false), m_error(std::move(error)) {}

    /// @brief 是否成功。
    bool ok() const noexcept { return m_ok; }
    /// @brief 是否成功；便于 `if (auto r = db.Query(...); r) ...`。
    explicit operator bool() const noexcept { return m_ok; }

    /// @brief 取值的引用；仅在 `ok()` 为 true 时有效。
    T& Value() & { return m_value; }
    /// @brief 取值的 const 引用；仅在 `ok()` 为 true 时有效。
    const T& Value() const& { return m_value; }
    /// @brief 移动取值；仅在 `ok()` 为 true 时有效。
    T&& Value() && { return std::move(m_value); }

    /// @brief 取错误；成功时返回空错误。
    const Error& Err() const noexcept { return m_error; }

private:
    bool m_ok = false;
    T m_value{};
    Error m_error;
};

/**
 * @brief 预编译语句（RAII，move-only）。
 * @details `DB::Prepare` 创建；支持复用：`Bind` → `Step` → 读列 → `Reset` →
 *          再 `Bind`。析构时 `sqlite3_finalize`。绑定个数必须与语句的
 *          `?` 占位符个数严格一致。
 *
 * 典型复用循环（1e6 次插入）：
 * @code
 *   auto stmt = db.Prepare("INSERT INTO t(id, v) VALUES(?, ?)").Value();
 *   db.Begin();
 *   for (int i = 0; i < n; ++i) {
 *       stmt.Reset();
 *       stmt.ClearBindings();
 *       stmt.Bind(i, i * 3);
 *       stmt.Step();
 *   }
 *   db.Commit();
 * @endcode
 */
class Stmt {
public:
    /// @brief 空语句（无效）。
    Stmt() noexcept = default;
    /// @brief 移动构造：转移语句所有权。
    Stmt(Stmt&& other) noexcept;
    /// @brief 移动赋值：先释放当前语句再接管。
    Stmt& operator=(Stmt&& other) noexcept;
    /// @brief 析构：finalize 语句。
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    /// @brief 语句是否有效（未 finalize）。
    bool Valid() const noexcept { return m_stmt != nullptr; }
    /// @brief 有效性布尔转换。
    explicit operator bool() const noexcept { return Valid(); }
    /// @brief 原生 `sqlite3_stmt*`；仅供只读扩展使用，生命周期由本对象管理。
    sqlite3_stmt* Handle() const noexcept { return m_stmt; }

    /// @brief `?` 占位符个数（含命名参数）。
    int ParameterCount() const { return m_parameter_count; }
    /// @brief 结果列数。
    int ColumnCount() const { return m_column_count; }

    /**
     * @brief 绑定全部参数（变参版本）。
     * @return 成功返回空错误；个数不匹配或绑定失败返回错误。
     * @note 返回的引用在下一次调用本对象的方法前有效，需要保存请拷贝。
     * @note 只绑定参数，不执行；`Step` 前可反复调用覆盖。
     */
    template <typename... Args>
    const Error& Bind(const Args&... args) {
        const auto values = MakeValues(args...);
        return BindValues(values.data(), values.size());
    }

    /// @brief 按数组绑定；个数必须等于 `ParameterCount()`。
    const Error& BindValues(const Value* values, std::size_t count);
    /// @brief 按 `std::array` 绑定。
    template <std::size_t N>
    const Error& BindValues(const std::array<Value, N>& values) {
        return BindValues(values.data(), N);
    }

    /// @brief 清空全部绑定（未绑定的参数在 Step 时视为 NULL）。
    const Error& ClearBindings();

    /**
     * @brief 重置语句到执行前状态，保留绑定值。
     * @return 成功返回空错误；语句无效返回 `Code::kMisuse`。
     * @note 不返回上一次执行的错误——错误已在 `Step` 时返回过，这里重复
     *       上报会让调用方误判为 Reset 失败。
     */
    const Error& Reset();

    /**
     * @brief 执行一步。
     * @return 有行（ROW）或执行完毕（DONE）都返回空错误；否则返回错误。
     *         执行后用 `HasRow()` 区分「有行」与「完毕」。
     */
    const Error& Step();

    /// @brief 上一次 `Step` 是否停在结果行。
    bool HasRow() const noexcept { return m_has_row; }
    /// @brief 上一次 `Step` 是否执行完毕。
    bool Done() const noexcept { return m_done; }
    /// @brief 最近一次操作的错误（成功后不清空，便于诊断）。
    const Error& Err() const noexcept { return m_error; }

    /// @brief 手动 finalize；幂等，之后 `Valid()` 为 false。
    const Error& Finalize();

    /// @brief 列是否为 NULL。
    bool IsNull(int column) const;
    /// @brief 列按 int64 读取（NULL/文本按 0，SQLite 语义）。
    std::int64_t ColumnInt64(int column) const;
    /// @brief 列按 double 读取。
    double ColumnDouble(int column) const;
    /// @brief 列按文本读取（拷贝；NULL 返回空串）。
    std::string ColumnText(int column) const;
    /// @brief 列按文本视图读取（零拷贝，生命周期见文件头）。
    std::string_view ColumnTextView(int column) const;
    /// @brief 列按二进制读取（零拷贝，NULL 返回空 Blob）。
    Blob ColumnBlob(int column) const;
    /// @brief 列的原生类型码（SQLITE_INTEGER=1/REAL=2/TEXT=3/BLOB=4/NULL=5）。
    int ColumnType(int column) const;
    /// @brief 列名；越界或无效返回空串。
    const char* ColumnName(int column) const;

private:
    friend class DB;
    Stmt(sqlite3* db, sqlite3_stmt* stmt) noexcept;

    /// @brief 列号是否在当前结果集内（越界访问返回安全默认值）。
    /// @note 用构造时缓存的列数判断，避免每列一次 sqlite3_column_count 调用。
    bool ColumnInRange(int column) const noexcept {
        return m_stmt != nullptr && column >= 0 && column < m_column_count;
    }

    sqlite3* m_db = nullptr;       ///< 所属连接（仅用于错误文本）。
    sqlite3_stmt* m_stmt = nullptr; ///< 原生语句，析构 finalize。
    Error m_error;                 ///< 最近一次错误。
    int m_parameter_count = 0;     ///< 构造时缓存的绑定参数个数。
    int m_column_count = 0;        ///< 构造时缓存的结果列数。
    bool m_has_row = false;        ///< 上一次 Step 是否 ROW。
    bool m_done = false;           ///< 上一次 Step 是否 DONE。
};

/**
 * @brief 查询结果游标（move-only，持有已绑定的 `Stmt`）。
 * @details 使用 `while (rows.Next()) { ... }`；循环结束后用 `Err()` 判断是
 *          正常结束还是中途出错。`Next()` 会覆盖上一行的列视图。
 */
class Rows {
public:
    /// @brief 空结果（无效）。
    Rows() = default;
    /// @brief 接管一个已绑定、未执行的语句。
    explicit Rows(Stmt stmt) : m_stmt(std::move(stmt)) {}

    Rows(Rows&&) noexcept = default;
    Rows& operator=(Rows&&) noexcept = default;
    Rows(const Rows&) = delete;
    Rows& operator=(const Rows&) = delete;

    /// @brief 前进一行；无更多行或出错时返回 false（错误见 `Err()`）。
    /// @note 遍历结束后重复调用返回 false；`Close()` 之后调用也返回 false，
    ///       不会重新执行语句或产生新的错误。
    bool Next();
    /// @brief 当前是否停在有效行。
    bool HasRow() const noexcept { return m_stmt.HasRow(); }
    /// @brief 遍历错误；正常结束为空错误。
    const Error& Err() const noexcept { return m_err; }

    /// @brief 提前关闭（finalize 语句）；幂等。
    Error Close();

    /// @brief 结果列数。
    int ColumnCount() const { return m_stmt.ColumnCount(); }
    /// @brief 列是否 NULL。
    bool IsNull(int column) const { return m_stmt.IsNull(column); }
    /// @brief 列按 int64 读取。
    std::int64_t Int64(int column) const { return m_stmt.ColumnInt64(column); }
    /// @brief 列按 double 读取。
    double Double(int column) const { return m_stmt.ColumnDouble(column); }
    /// @brief 列按文本读取（拷贝）。
    std::string Text(int column) const { return m_stmt.ColumnText(column); }
    /// @brief 列按文本视图读取（零拷贝）。
    std::string_view TextView(int column) const {
        return m_stmt.ColumnTextView(column);
    }
    /// @brief 列按二进制读取。
    Blob BlobValue(int column) const { return m_stmt.ColumnBlob(column); }
    /// @brief 列的原生类型码。
    int ColumnType(int column) const { return m_stmt.ColumnType(column); }
    /// @brief 列名。
    const char* ColumnName(int column) const {
        return m_stmt.ColumnName(column);
    }
    /// @brief 底层语句（已执行部分步骤）；一般不需要直接使用。
    Stmt& Statement() noexcept { return m_stmt; }
    /// @brief 底层语句的 const 版本。
    const Stmt& Statement() const noexcept { return m_stmt; }

private:
    Stmt m_stmt;  ///< 结果语句。
    Error m_err;  ///< Next 过程中记录的错误。
};

/// @brief `Exec` 的执行结果：最后插入 rowid 与受影响行数。
struct ExecResult {
    std::int64_t last_insert_row_id = 0; ///< sqlite3_last_insert_rowid。
    int changes = 0;                     ///< sqlite3_changes（多语句时累加）。
};

/**
 * @brief 一个 SQLite 连接（Go `*sql.DB` 的精简对应物）。
 * @details 公开方法内部持有一把互斥量，因此同一个 `DB` 可以被多个线程调用；
 *          但高并发写场景仍建议每线程一个 `DB` + WAL，减少锁竞争。所有
 *          失败路径都返回 `Error`（或 `Result<T>` 里的 `Error`），不抛异常。
 *
 * 生命周期：`Open` 返回 `std::unique_ptr<DB>`；`Close` 幂等；析构自动 Close。
 */
class DB {
public:
    /// @brief 打开（不存在则创建）数据库。
    /// @param path 文件路径；`:memory:` 表示内存库。
    /// @return 成功返回连接；失败返回 `Error`（`Code::kCantOpen` 等）。
    static Result<std::unique_ptr<DB>> Open(const std::string& path);

    /// @brief 析构：若未关闭则 `Close()`。
    ~DB();
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;
    DB(DB&&) = delete;
    DB& operator=(DB&&) = delete;

    /// @brief 关闭连接；幂等。关闭后其它调用返回 `Code::kMisuse`。
    Error Close();
    /// @brief 连接是否打开。
    bool IsOpen() const;
    /// @brief 数据库路径（构造后不变）。
    const std::string& Path() const noexcept { return m_path; }
    /// @brief 原生 `sqlite3*`；仅供只读扩展，不能在本对象之外 finalize/close。
    sqlite3* Handle() const;

    /**
     * @brief 执行一条或多条无结果语句。
     * @param sql SQL 文本；可包含多条以 `;` 分隔的语句（脚本）。
     * @param args 绑定参数；非空时只允许单条语句。
     * @return `ExecResult`；失败返回错误。
     */
    template <typename... Args>
    Result<ExecResult> Exec(const std::string& sql, const Args&... args) {
        const auto values = MakeValues(args...);
        return ExecImpl(sql, values.data(), values.size());
    }

    /**
     * @brief 执行查询并返回结果游标。
     * @param sql 单条 SELECT/PRAGMA/INSERT..RETURNING 语句。
     * @param args 绑定参数（个数必须与占位符一致）。
     */
    template <typename... Args>
    Result<Rows> Query(const std::string& sql, const Args&... args) {
        const auto values = MakeValues(args...);
        return QueryImpl(sql, values.data(), values.size());
    }

    /// @brief 预编译一条语句（不绑定、不执行）。
    Result<Stmt> Prepare(const std::string& sql);

    /// @brief 开始事务（`BEGIN IMMEDIATE`，避免读升级写时的死锁）。
    Error Begin();
    /// @brief 提交事务；无活动事务返回 `Code::kMisuse`。
    Error Commit();
    /// @brief 回滚事务；无活动事务返回 `Code::kMisuse`。
    Error Rollback();
    /// @brief 当前是否处于事务中（`sqlite3_get_autocommit == 0`）。
    bool InTransaction() const;

    /// @brief 设置 busy 超时（毫秒）；0 表示不等待，直接返回 BUSY。
    Error SetBusyTimeout(int milliseconds);
    /// @brief 设置 journal 模式（`DELETE`/`TRUNCATE`/`PERSIST`/`MEMORY`/`WAL`/`OFF`）。
    /// @return 实际生效的模式与请求不一致时返回错误（`:memory:` 不支持 WAL）。
    Error SetJournalMode(std::string_view mode);
    /// @brief 设置同步级别（`OFF`/`NORMAL`/`FULL`/`EXTRA`，或 0..3）。
    Error SetSynchronous(std::string_view level);
    /// @brief 打开/关闭外键约束。
    Error SetForeignKeys(bool enabled);

    /// @brief 最近一次 INSERT 的 rowid。
    std::int64_t LastInsertRowId() const;
    /// @brief 最近一次语句修改的行数。
    int Changes() const;
    /// @brief 最近一次失败的 `Error` 副本。
    Error LastError() const;

private:
    DB(sqlite3* db, std::string path);

    Result<ExecResult> ExecImpl(const std::string& sql, const Value* params,
                                std::size_t count);
    Result<Rows> QueryImpl(const std::string& sql, const Value* params,
                           std::size_t count);
    Error BindLocked(sqlite3_stmt* stmt, const Value* params,
                     std::size_t count);
    Error MakeErrorLocked(int rc) const;
    Error CheckOpenLocked() const;

    mutable std::mutex m_mutex; ///< 串行化本连接上的公开操作。
    sqlite3* m_db = nullptr;    ///< 原生连接；Close 后置空。
    std::string m_path;         ///< 构造后不变。
    Error m_last_error;         ///< 最近一次失败（诊断用）。
};

/**
 * @brief 事务的 RAII 辅助。
 * @details 构造时 `Begin()`；若析构时仍未 `Commit()` 则自动 `Rollback()`，
 *          避免异常/提前返回留下悬挂事务。`Commit`/`Rollback` 幂等。
 *
 * @code
 *   storage::Transaction tx(db);
 *   if (auto err = tx.Begin()) return err;
 *   ...
 *   return tx.Commit();
 * @endcode
 */
class Transaction {
public:
    /// @brief 绑定到连接；不自动 Begin。
    explicit Transaction(DB& db) noexcept : m_db(&db) {}
    /// @brief 析构：活动事务自动回滚（忽略回滚错误）。
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    /// @brief 开始事务；成功后 `Active()` 为 true。
    Error Begin();
    /// @brief 提交；成功后 `Active()` 为 false。
    Error Commit();
    /// @brief 回滚；成功后 `Active()` 为 false。
    Error Rollback();
    /// @brief 当前是否有活动事务。
    bool Active() const noexcept { return m_active; }

private:
    DB* m_db = nullptr;
    bool m_active = false;
};

} // namespace storage
} // namespace go2cpp
