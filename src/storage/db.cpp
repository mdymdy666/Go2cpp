/**
 * @file src/storage/db.cpp
 * @brief SQLite 嵌入式存储层的实现：错误翻译、参数绑定、语句生命周期与事务。
 *
 * ## 模块定位
 *
 * `include/go2cpp/storage/db.hpp` 的实现层。上游是业务代码与
 * `tests/storage/test_db.cpp`；下游只有系统 `libsqlite3` 与 C++17 标准库，
 * 不依赖 go2cpp runtime 的任何其它模块（保证 `GO2CPP_WITH_SQLITE=OFF` 时
 * 完全不参与构建）。
 *
 * ## 本文件定义什么
 *
 * - `Error` 的错误码命名与 `ToString`；
 * - `Value(const char*)`、`Stmt` 的绑定/执行/列访问、`Rows::Next`；
 * - `DB` 的 Open/Close/Exec/Query/Prepare/事务/PRAGMA；
 * - `Transaction` 的自动回滚；
 * - 匿名命名空间里的绑定原语、大小写比较与错误构造。
 *
 * ## 阅读建议
 *
 * 先看 `DB::ExecImpl`（多语句循环、参数计数检查、错误锁存），再看
 * `Stmt::Step` 的 ROW/DONE/错误三态；`DB::Close` 用 `sqlite3_close_v2`
 * 处理「仍有语句未 finalize」的延迟关闭。
 *
 * ## 边界与陷阱
 *
 * - 所有绑定使用 `SQLITE_TRANSIENT`：参数生命周期只保证到 `Bind`/`Exec`
 *   返回，调用方可以立即释放临时字符串/blob；
 * - 多语句 `Exec` 的 `changes` 是逐条累加，`last_insert_row_id` 取最后一条；
 * - `Query`/`Prepare` 只接受单条语句，尾随分号/空白允许，多余语句报
 *   `Code::kMisuse`，避免静默丢弃；
 * - 事务用 `BEGIN IMMEDIATE`：读连接在事务中途升级为写会返回 BUSY，
 *   IMMEDIATE 在 BEGIN 时就把写锁拿下，错误位置更靠前、更容易重试。
 */
#include "go2cpp/storage/db.hpp"

#include "go2cpp/tools/strings.hpp"

#include <sqlite3.h>

#include <cctype>
#include <string>
#include <utility>

namespace go2cpp {
namespace storage {
namespace {

/// @brief primary result code 的稳定名字；未知码返回 `SQLITE_UNKNOWN`。
const char* CodeName(int code) noexcept {
    switch (code) {
        case SQLITE_OK:
            return "SQLITE_OK";
        case SQLITE_ERROR:
            return "SQLITE_ERROR";
        case SQLITE_INTERNAL:
            return "SQLITE_INTERNAL";
        case SQLITE_PERM:
            return "SQLITE_PERM";
        case SQLITE_ABORT:
            return "SQLITE_ABORT";
        case SQLITE_BUSY:
            return "SQLITE_BUSY";
        case SQLITE_LOCKED:
            return "SQLITE_LOCKED";
        case SQLITE_NOMEM:
            return "SQLITE_NOMEM";
        case SQLITE_READONLY:
            return "SQLITE_READONLY";
        case SQLITE_INTERRUPT:
            return "SQLITE_INTERRUPT";
        case SQLITE_IOERR:
            return "SQLITE_IOERR";
        case SQLITE_CORRUPT:
            return "SQLITE_CORRUPT";
        case SQLITE_NOTFOUND:
            return "SQLITE_NOTFOUND";
        case SQLITE_FULL:
            return "SQLITE_FULL";
        case SQLITE_CANTOPEN:
            return "SQLITE_CANTOPEN";
        case SQLITE_PROTOCOL:
            return "SQLITE_PROTOCOL";
        case SQLITE_EMPTY:
            return "SQLITE_EMPTY";
        case SQLITE_SCHEMA:
            return "SQLITE_SCHEMA";
        case SQLITE_TOOBIG:
            return "SQLITE_TOOBIG";
        case SQLITE_CONSTRAINT:
            return "SQLITE_CONSTRAINT";
        case SQLITE_MISMATCH:
            return "SQLITE_MISMATCH";
        case SQLITE_MISUSE:
            return "SQLITE_MISUSE";
        case SQLITE_NOLFS:
            return "SQLITE_NOLFS";
        case SQLITE_AUTH:
            return "SQLITE_AUTH";
        case SQLITE_FORMAT:
            return "SQLITE_FORMAT";
        case SQLITE_RANGE:
            return "SQLITE_RANGE";
        case SQLITE_NOTADB:
            return "SQLITE_NOTADB";
        case SQLITE_NOTICE:
            return "SQLITE_NOTICE";
        case SQLITE_WARNING:
            return "SQLITE_WARNING";
        case SQLITE_ROW:
            return "SQLITE_ROW";
        case SQLITE_DONE:
            return "SQLITE_DONE";
        default:
            return "SQLITE_UNKNOWN";
    }
}

/// @brief 从连接当前错误状态构造 Error；`db` 为空时退化为 errstr。
/// @note 开启扩展错误码后 sqlite3_exec/step 的返回值可能是 787、2067 这类
///       扩展码；`Error::Code()` 统一归一化为 primary code（低 8 位）。
Error ErrorFromDb(sqlite3* db, int rc) {
    const int primary = rc & 0xFF;
    if (db == nullptr) {
        return Error(primary, 0, sqlite3_errstr(primary));
    }
    const char* message = sqlite3_errmsg(db);
    if (message == nullptr || *message == '\0') {
        message = sqlite3_errstr(primary);
    }
    return Error(primary, sqlite3_extended_errcode(db), message);
}

/// @brief SQLite 绑定原语：把 Value 写入第 `index` 个参数（1 基）。
int BindOne(sqlite3_stmt* stmt, int index, const Value& value) noexcept {
    switch (value.type()) {
        case Value::Type::kNull:
            return sqlite3_bind_null(stmt, index);
        case Value::Type::kInt64:
            return sqlite3_bind_int64(stmt, index,
                                      static_cast<sqlite3_int64>(value.Int64()));
        case Value::Type::kBool:
            return sqlite3_bind_int(stmt, index, value.Bool() ? 1 : 0);
        case Value::Type::kDouble:
            return sqlite3_bind_double(stmt, index, value.Double());
        case Value::Type::kText:
            return sqlite3_bind_text(
                stmt, index, value.Text().data(),
                static_cast<int>(value.Text().size()), SQLITE_TRANSIENT);
        case Value::Type::kBlob:
            return sqlite3_bind_blob(
                stmt, index, value.BlobData().data,
                static_cast<int>(value.BlobData().size), SQLITE_TRANSIENT);
    }
    return SQLITE_MISUSE;
}

/// @brief ASCII 大写（PRAGMA 关键字不受 locale 影响）。
std::string ToUpperAscii(std::string_view text) { return AsciiToUpper(text); }

/// @brief 大小写不敏感的 ASCII 比较（journal_mode 返回值）。
bool EqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(left[i]);
        const unsigned char b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

/// @brief `tail` 是否只剩空白与分号（用于拒绝第二条语句）。
bool TailIsOnlyWhitespace(const char* tail) noexcept {
    if (tail == nullptr) {
        return true;
    }
    for (const char* p = tail; *p != '\0'; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (!std::isspace(ch) && ch != ';') {
            return false;
        }
    }
    return true;
}

/// @brief 成功时返回的空错误：静态对象，热路径零构造/析构。
/// @note 返回引用的 API 在下一次调用同一对象的方法前有效；调用方要长期
///       保存时请拷贝（`Error e = stmt.Step();`）。
const Error kOkError{};

/// @brief 参数个数不匹配的统一错误文本。
Error ParamCountError(int expected, std::size_t actual) {
    return Error(static_cast<int>(Code::kMisuse), 0,
                 "parameter count mismatch: SQL expects " +
                     std::to_string(expected) + ", got " +
                     std::to_string(actual));
}

} // namespace

// ---------------------------------------------------------------------------
// Error
// ---------------------------------------------------------------------------

Error::Error(int code, int extended_code, std::string message)
    : m_code(code),
      m_extended_code(extended_code),
      m_message(std::move(message)) {}

bool Error::IsBusy() const noexcept {
    // 扩展码的低 8 位是 primary code；BUSY_SNAPSHOT 等都由 BUSY 派生。
    return m_code == static_cast<int>(Code::kBusy) ||
           (m_extended_code & 0xFF) == static_cast<int>(Code::kBusy);
}

bool Error::IsConstraint() const noexcept {
    return m_code == static_cast<int>(Code::kConstraint) ||
           (m_extended_code & 0xFF) == static_cast<int>(Code::kConstraint);
}

std::string Error::ToString() const {
    if (ok()) {
        return "SQLITE_OK";
    }
    std::string out = CodeName(m_code);
    out += '(';
    out += std::to_string(m_code);
    if (m_extended_code != 0 && m_extended_code != m_code) {
        out += '/';
        out += std::to_string(m_extended_code);
    }
    out += ")";
    if (!m_message.empty()) {
        out += ": ";
        out += m_message;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

Value::Value(const char* text) {
    if (text == nullptr) {
        return; // SQL NULL
    }
    m_type = Type::kText;
    m_text.assign(text);
}

// ---------------------------------------------------------------------------
// Stmt
// ---------------------------------------------------------------------------

Stmt::Stmt(sqlite3* db, sqlite3_stmt* stmt) noexcept
    : m_db(db),
      m_stmt(stmt),
      m_parameter_count(sqlite3_bind_parameter_count(stmt)),
      m_column_count(sqlite3_column_count(stmt)) {}

Stmt::Stmt(Stmt&& other) noexcept
    : m_db(other.m_db),
      m_stmt(other.m_stmt),
      m_error(std::move(other.m_error)),
      m_parameter_count(other.m_parameter_count),
      m_column_count(other.m_column_count),
      m_has_row(other.m_has_row),
      m_done(other.m_done) {
    other.m_db = nullptr;
    other.m_stmt = nullptr;
    other.m_parameter_count = 0;
    other.m_column_count = 0;
    other.m_has_row = false;
    other.m_done = false;
}

Stmt& Stmt::operator=(Stmt&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Finalize();
    m_db = other.m_db;
    m_stmt = other.m_stmt;
    m_error = std::move(other.m_error);
    m_parameter_count = other.m_parameter_count;
    m_column_count = other.m_column_count;
    m_has_row = other.m_has_row;
    m_done = other.m_done;
    other.m_db = nullptr;
    other.m_stmt = nullptr;
    other.m_parameter_count = 0;
    other.m_column_count = 0;
    other.m_has_row = false;
    other.m_done = false;
    return *this;
}

Stmt::~Stmt() {
    if (m_stmt != nullptr) {
        sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
    }
}

const Error& Stmt::BindValues(const Value* values, std::size_t count) {
    if (m_stmt == nullptr) {
        m_error = Error(static_cast<int>(Code::kMisuse), 0,
                        "cannot bind a closed statement");
        return m_error;
    }
    const int expected = m_parameter_count;
    if (count != static_cast<std::size_t>(expected)) {
        m_error = ParamCountError(expected, count);
        return m_error;
    }
    for (int index = 0; index < expected; ++index) {
        const int rc = BindOne(m_stmt, index + 1, values[index]);
        if (rc != SQLITE_OK) {
            m_error = ErrorFromDb(sqlite3_db_handle(m_stmt), rc);
            return m_error;
        }
    }
    return kOkError;
}

const Error& Stmt::ClearBindings() {
    if (m_stmt == nullptr) {
        m_error = Error(static_cast<int>(Code::kMisuse), 0,
                        "cannot clear bindings of a closed statement");
        return m_error;
    }
    if (sqlite3_clear_bindings(m_stmt) != SQLITE_OK) {
        m_error = ErrorFromDb(sqlite3_db_handle(m_stmt), SQLITE_ERROR);
        return m_error;
    }
    return kOkError;
}

const Error& Stmt::Reset() {
    if (m_stmt == nullptr) {
        m_error = Error(static_cast<int>(Code::kMisuse), 0,
                        "cannot reset a closed statement");
        return m_error;
    }
    // sqlite3_reset 的返回值是「上一次 Step 的错误」；这里不重复上报，
    // 错误已在 Step 时返回给调用方（见头文件说明）。
    (void)sqlite3_reset(m_stmt);
    m_has_row = false;
    m_done = false;
    return kOkError;
}

const Error& Stmt::Step() {
    if (m_stmt == nullptr) {
        m_error = Error(static_cast<int>(Code::kMisuse), 0,
                        "cannot step a closed statement");
        return m_error;
    }
    m_has_row = false;
    m_done = false;
    const int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW) {
        m_has_row = true;
        return kOkError;
    }
    if (rc == SQLITE_DONE) {
        m_done = true;
        return kOkError;
    }
    m_error = ErrorFromDb(sqlite3_db_handle(m_stmt), rc);
    return m_error;
}

const Error& Stmt::Finalize() {
    if (m_stmt == nullptr) {
        return kOkError;
    }
    // finalize 的返回值是上一次执行的错误码；错误已由 Step 返回，这里
    // 只负责释放，不再重复上报。
    (void)sqlite3_finalize(m_stmt);
    m_stmt = nullptr;
    m_db = nullptr;
    m_parameter_count = 0;
    m_column_count = 0;
    m_has_row = false;
    m_done = false;
    return kOkError;
}

bool Stmt::IsNull(int column) const {
    if (!ColumnInRange(column)) {
        return true;
    }
    return sqlite3_column_type(m_stmt, column) == SQLITE_NULL;
}

std::int64_t Stmt::ColumnInt64(int column) const {
    if (!ColumnInRange(column)) {
        return 0;
    }
    return static_cast<std::int64_t>(sqlite3_column_int64(m_stmt, column));
}

double Stmt::ColumnDouble(int column) const {
    if (!ColumnInRange(column)) {
        return 0.0;
    }
    return sqlite3_column_double(m_stmt, column);
}

std::string Stmt::ColumnText(int column) const {
    if (!ColumnInRange(column)) {
        return {};
    }
    const unsigned char* text = sqlite3_column_text(m_stmt, column);
    if (text == nullptr) {
        return {};
    }
    const int bytes = sqlite3_column_bytes(m_stmt, column);
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(bytes));
}

std::string_view Stmt::ColumnTextView(int column) const {
    if (!ColumnInRange(column)) {
        return {};
    }
    const unsigned char* text = sqlite3_column_text(m_stmt, column);
    if (text == nullptr) {
        return {};
    }
    const int bytes = sqlite3_column_bytes(m_stmt, column);
    return std::string_view(reinterpret_cast<const char*>(text),
                            static_cast<std::size_t>(bytes));
}

Blob Stmt::ColumnBlob(int column) const {
    if (!ColumnInRange(column)) {
        return {};
    }
    const void* data = sqlite3_column_blob(m_stmt, column);
    if (data == nullptr) {
        return {};
    }
    const int bytes = sqlite3_column_bytes(m_stmt, column);
    return Blob(data, static_cast<std::size_t>(bytes));
}

int Stmt::ColumnType(int column) const {
    if (!ColumnInRange(column)) {
        return SQLITE_NULL;
    }
    return sqlite3_column_type(m_stmt, column);
}

const char* Stmt::ColumnName(int column) const {
    if (!ColumnInRange(column)) {
        return "";
    }
    const char* name = sqlite3_column_name(m_stmt, column);
    return name != nullptr ? name : "";
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

/**
 * @brief 把游标推进到下一行；结果集结束、已关闭或出错均返回 false。
 *
 * @param （无显式参数；隐式状态是本对象持有的 Stmt 与上一次 step 的结果）
 *
 * @return
 * - true：当前行可读——调用方**必须**在本行内取完需要的数据再调 Next；
 * - false：结束/已关闭/出错——调用方**必须**用 `Err()` 区分「正常结束」与「执行失败」。
 *
 * ## 定位
 * 唯一游标推进点。错一次就会丢行、重复 step 触发 SQLITE_MISUSE，或把错误当结束静默吞掉。
 *
 * ## 步骤
 * step1. 已 Close 直接返回 false，且不覆盖既有错误（与 Go Rows 语义一致）；
 * step2. 已 Done 不再 step，避免对已完成的语句再次 step；
 * step3. Step 出错时锁存到 m_err 并返回 false；
 * step4. 以 HasRow 判定本行是否可读。
 *
 * ## 并发/失败语义
 * - 调用约束：单线程顺序使用；Rows 独占该语句，不得并发迭代同一对象。
 * - 原子性：单次 step 由 SQLite 保证原子。
 * - 失败语义：错误被锁存而非抛出，可重复查询 Err()。
 * - 不变量：返回 true 时列访问接口在本行有效，直到下一次 Next/Close。
 */
bool Rows::Next() {
    // step1: finalize 过的语句再 step 属于误用，这里直接短路且不污染 m_err。
    if (!m_stmt.Valid()) {
        // 已 Close：与 Go 的 Rows 关闭后 Next=false 一致，不产生新错误。
        return false;
    }
    if (m_stmt.Done()) {
        // 已遍历到结尾：重复 Next 不重新执行（再 step 会返回 SQLITE_MISUSE）。
        return false;
    }
    // step3: 错误只锁存不抛；调用方用 Err() 判断是失败还是正常结束。
    const Error& error = m_stmt.Step();
    if (error) {
        m_err = error;
        return false;
    }
    if (!m_stmt.HasRow()) {
        return false;
    }
    return true;
}

Error Rows::Close() {
    m_err = {};
    return m_stmt.Finalize();
}

// ---------------------------------------------------------------------------
// DB
// ---------------------------------------------------------------------------

DB::DB(sqlite3* db, std::string path)
    : m_db(db), m_path(std::move(path)) {}

DB::~DB() {
    Close();
}

/**
 * @brief 打开（必要时创建）一个 SQLite 数据库文件，并把连接所有权移交调用方。
 *
 * @param path [in] 数据库文件路径，UTF-8 编码；不可为 nullptr；`:memory:` 表示私有内存库——只在本次调用期间被读取，所有权不转移。
 *
 * @return
 * - `Result` 持有值：打开成功——调用方**必须**用 `std::unique_ptr` 独占持有，析构时自动 Close；
 * - `Result` 持有错误：打开失败——调用方**必须**先看 `Error::Code()` 区分 BUSY/PERM 再决定是否重试，不要盲目循环。
 *
 * ## 定位
 * 全模块唯一的连接创建点：扩展错误码与 busy 超时只在这里设置一次。漏掉扩展码会把 BUSY_SNAPSHOT 折叠成 BUSY，上层无法区分「快照失效」与「锁竞争」；漏掉 busy_timeout 会让并发写在默认 0ms 下立刻失败。
 *
 * ## 步骤
 * step1. 以 READWRITE|CREATE|FULLMUTEX 打开；FULLMUTEX 让连接可跨线程使用；
 * step2. 失败时先从 raw 取 errmsg/extended code，再 close，避免句柄泄漏；
 * step3. 主错误码只保留低 8 位，扩展码原样保留，便于上层细分；
 * step4. 打开扩展错误码并设 5s busy 超时；
 * step5. 把裸句柄包装成 unique_ptr，由 DB 析构负责关闭。
 *
 * ## 并发/失败语义
 * - 调用约束：可在任意线程调用；返回的连接自身线程安全（FULLMUTEX）。
 * - 原子性：失败路径已关闭 raw 句柄，不留下半开连接。
 * - 失败语义：不抛异常（`std::bad_alloc` 除外）；失败后 raw 不泄漏。
 * - 不变量：成功返回的连接已启用扩展错误码与 5s busy 超时。
 */
Result<std::unique_ptr<DB>> DB::Open(const std::string& path) {
    sqlite3* raw = nullptr;
    // step1: 一次把读写/创建/跨线程三件事都定死，后续不再改打开方式。
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                      SQLITE_OPEN_FULLMUTEX;
    const int rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string message =
            raw != nullptr ? sqlite3_errmsg(raw) : sqlite3_errstr(rc);
        const int extended =
            raw != nullptr ? sqlite3_extended_errcode(raw) : 0;
        if (raw != nullptr) {
            sqlite3_close(raw);
        }
        return Error(rc & 0xFF, extended, std::move(message));
    }
    // 扩展错误码让 BUSY_SNAPSHOT / CONSTRAINT_UNIQUE 等可被区分；
    // 默认 5s busy 超时避免并发写直接失败。
    sqlite3_extended_result_codes(raw, 1);
    sqlite3_busy_timeout(raw, 5000);
    return std::unique_ptr<DB>(new DB(raw, path));
}

/**
 * @brief 关闭连接；仍有未 finalize 的语句时按 close_v2 语义延迟回收。
 *
 * @return
 * - 空 `Error`：已关闭或本就未打开——调用方**不要**重复关闭；
 * - 非空 `Error`：仍有未 finalize 的语句——连接保持打开，调用方**必须**先 finalize 语句再重试。
 *
 * ## 定位
 * 唯一释放 sqlite3 句柄的地方。漏关会泄漏文件描述符；提前关会 UAF。
 *
 * ## 步骤
 * step1. 持锁，与所有公开方法互斥；
 * step2. 已关闭时幂等返回成功；
 * step3. 先置空再 close_v2，防止回调重入看到悬空句柄；
 * step4. 关闭失败时恢复 m_db 并锁存 last_error，调用方拿到可重试的句柄。
 *
 * ## 并发/失败语义
 * - 调用约束：任意线程；内部持 m_mutex，不可在持有期间回调用户代码。
 * - 原子性：句柄摘除与 close 在同一把锁内完成。
 * - 失败语义：失败后 m_db 仍有效（不是悬空指针），可修正后重试。
 * - 不变量：返回空 Error 后 IsOpen() 必为 false。
 */
Error DB::Close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_db == nullptr) {
        return {};
    }
    // step3: 先摘空 m_db 再 close_v2；这样 close 回调里任何重入都看到「已关闭」，
    //        而不是一个正在析构的悬空句柄。
    sqlite3* db = m_db;
    m_db = nullptr;
    const int rc = sqlite3_close_v2(db);
    if (rc != SQLITE_OK) {
        m_db = db;
        m_last_error = ErrorFromDb(db, rc);
        return m_last_error;
    }
    return {};
}

bool DB::IsOpen() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_db != nullptr;
}

sqlite3* DB::Handle() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_db;
}

Error DB::CheckOpenLocked() const {
    if (m_db != nullptr) {
        return {};
    }
    return Error(static_cast<int>(Code::kMisuse), 0, "database is closed");
}

Error DB::MakeErrorLocked(int rc) const {
    if (m_db == nullptr) {
        return Error(rc, 0, sqlite3_errstr(rc));
    }
    return ErrorFromDb(m_db, rc);
}

Error DB::BindLocked(sqlite3_stmt* stmt, const Value* params,
                     std::size_t count) {
    const int expected = sqlite3_bind_parameter_count(stmt);
    if (count != static_cast<std::size_t>(expected)) {
        return ParamCountError(expected, count);
    }
    for (int index = 0; index < expected; ++index) {
        const int rc = BindOne(stmt, index + 1, params[index]);
        if (rc != SQLITE_OK) {
            return MakeErrorLocked(rc);
        }
    }
    return {};
}

/**
 * @brief 依次执行 sql 里的每条语句；带参数时只允许单条。
 *
 * @param sql [in] 一条或多条 SQL 文本，UTF-8；带参数时必须只含单条语句——引用在本调用期间有效。
 * @param params [in] 绑定值数组，可为 nullptr（仅当 count==0）；按 1-based 占位符顺序绑定——所有权借用，SQLITE_TRANSIENT 保证返回后调用方可立即释放。
 * @param count [in] 参数个数，必须等于占位符总数，否则返回参数个数错误；无负数语义（size_t）。
 *
 * @return
 * - 成功 `ExecResult`：changes 是多条语句的累加值，last_insert_row_id 取最后一条——调用方**必须**按语句条数理解该值；
 * - 失败 `Error`：已执行的前序语句**不会回滚**——调用方**必须**用事务包裹多语句写。
 *
 * ## 定位
 * Exec 的唯一实现，所有 DDL/DML 都从这里落地。错了会半执行、静默丢语句或参数错位写库。
 *
 * ## 步骤
 * step1. 持锁并检查连接是否已关闭；
 * step2. 循环 prepare，一次推进一条语句；
 * step3. stmt 为空表示空白/纯注释，前移游标继续；
 * step4. 已执行过语句又带参数时拒绝，避免参数归属歧义；
 * step5. 绑定参数并 step，失败路径也要 finalize；
 * step6. 累加 changes、记录最后 rowid；
 * step7. 成功后立即 finalize，释放语句资源；
 * step8. 全程未执行任何语句时，按 SQLite CLI 语义返回成功。
 *
 * ## 并发/失败语义
 * - 调用约束：任意线程；内部持 m_mutex，同一连接上的 Exec/Query 被串行化。
 * - 原子性：单语句原子，多语句非原子；失败时 m_last_error 被更新。
 * - 失败语义：不抛异常，返回 Error；失败后连接仍可用。
 * - 不变量：成功返回时 SQL 中的语句全部执行完毕且无残留 prepared stmt。
 */
Result<ExecResult> DB::ExecImpl(const std::string& sql, const Value* params,
                                std::size_t count) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const Error error = CheckOpenLocked(); error) {
        return error;
    }
    ExecResult result{};
    bool executed = false;
    // step2: 用 tail 游标逐条推进，而不是一次性 split——SQLite 的切分规则以 prepare 为准。
    const char* cursor = sql.c_str();
    while (cursor != nullptr && *cursor != '\0') {
        sqlite3_stmt* stmt = nullptr;
        const char* tail = nullptr;
        int rc = sqlite3_prepare_v2(m_db, cursor, -1, &stmt, &tail);
        if (rc != SQLITE_OK) {
            m_last_error = MakeErrorLocked(rc);
            return m_last_error;
        }
        if (stmt == nullptr) {
            // 空白/纯注释：prepare 不产生语句，移动游标继续。
            if (tail == nullptr || tail == cursor) {
                break;
            }
            cursor = tail;
            continue;
        }
        // step4: 多语句 + 参数会让「第 N 个参数属于哪条语句」变得不可判定，直接拒绝。
        if (executed && count != 0) {
            (void)sqlite3_finalize(stmt);
            m_last_error = Error(
                static_cast<int>(Code::kMisuse), 0,
                "multiple statements cannot be combined with parameters");
            return m_last_error;
        }
        if (const Error error = BindLocked(stmt, params, count); error) {
            (void)sqlite3_finalize(stmt);
            m_last_error = error;
            return error;
        }
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            const Error error = MakeErrorLocked(rc);
            (void)sqlite3_finalize(stmt);
            m_last_error = error;
            return error;
        }
        result.last_insert_row_id = sqlite3_last_insert_rowid(m_db);
        // step6: last_insert_rowid 是连接级状态，取最后一次；changes 逐条累加。
        result.changes += sqlite3_changes(m_db);
        (void)sqlite3_finalize(stmt);
        executed = true;
        cursor = tail;
    }
    if (!executed) {
        // 纯空白/纯注释输入：与 SQLite CLI 一致，视为成功且无副作用。
        return result;
    }
    return result;
}

/**
 * @brief 准备并执行一条查询语句，把游标封装成 Rows 返回。
 *
 * @param sql [in] 单条 SQL（SELECT/PRAGMA 等），UTF-8；尾随分号与空白允许——引用在本调用期间有效。
 * @param params [in] 绑定值数组，可为 nullptr（仅当 count==0）——所有权借用，绑定后即可释放。
 * @param count [in] 参数个数，必须与占位符总数相等。
 *
 * @return
 * - 成功 `Rows`：持有语句所有权，析构/Close 时 finalize——调用方**必须**迭代完或显式 Close；
 * - 失败 `Error`：语句已 finalize，连接不受影响——调用方**不要**继续使用旧 Rows。
 *
 * ## 定位
 * 唯一查询入口。把「第二条语句」静默丢弃会让调用方以为 SQL 全部生效，因此这里是严格的单语句检查点。
 *
 * ## 步骤
 * step1. 持锁并检查连接；
 * step2. prepare 单条语句；
 * step3. 空语句报 misuse，尾随非空白报 misuse；
 * step4. 绑定参数；
 * step5. 把 stmt 所有权移交给 Rows，锁在构造后释放。
 *
 * ## 并发/失败语义
 * - 调用约束：任意线程；锁只覆盖 prepare/bind，Rows 迭代期间不持锁。
 * - 原子性：prepare 失败不会留下半初始化语句。
 * - 失败语义：所有失败路径都 finalize 临时 stmt。
 * - 不变量：成功返回的 Rows 至少包含一条可执行语句。
 */
Result<Rows> DB::QueryImpl(const std::string& sql, const Value* params,
                           std::size_t count) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const Error error = CheckOpenLocked(); error) {
        return error;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    const int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
        m_last_error = MakeErrorLocked(rc);
        return m_last_error;
    }
    if (stmt == nullptr) {
        m_last_error = Error(static_cast<int>(Code::kMisuse), 0,
                             "empty SQL statement");
        return m_last_error;
    }
    if (!TailIsOnlyWhitespace(tail)) {
        (void)sqlite3_finalize(stmt);
        m_last_error = Error(static_cast<int>(Code::kMisuse), 0,
                             "Query accepts a single SQL statement");
        return m_last_error;
    }
    if (const Error error = BindLocked(stmt, params, count); error) {
        (void)sqlite3_finalize(stmt);
        m_last_error = error;
        return error;
    }
    // step5: 所有权移交 Rows；本函数返回后不再触碰 stmt。
    return Rows(Stmt(m_db, stmt));
}

Result<Stmt> DB::Prepare(const std::string& sql) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const Error error = CheckOpenLocked(); error) {
        return error;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    const int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, &tail);
    if (rc != SQLITE_OK) {
        m_last_error = MakeErrorLocked(rc);
        return m_last_error;
    }
    if (stmt == nullptr) {
        m_last_error = Error(static_cast<int>(Code::kMisuse), 0,
                             "empty SQL statement");
        return m_last_error;
    }
    if (!TailIsOnlyWhitespace(tail)) {
        (void)sqlite3_finalize(stmt);
        m_last_error = Error(static_cast<int>(Code::kMisuse), 0,
                             "Prepare accepts a single SQL statement");
        return m_last_error;
    }
    return Stmt(m_db, stmt);
}

/**
 * @brief 开启一个写事务（BEGIN IMMEDIATE），保证 BEGIN 时就拿到写锁。
 *
 * @return
 * - 空 `Error`：事务已开启——调用方**必须**随后 Commit 或 Rollback，不得嵌套 Begin；
 * - `kMisuse`：已关闭或已有活动事务——调用方**不要**重试 Begin，先结束当前事务；
 * - 其他 `Error`：锁/IO 失败——调用方**必须**按 IsBusy 决定退避重试。
 *
 * ## 定位
 * 事务唯一起点。用 IMMEDIATE 把「事务中途升级为写」的 BUSY 提前到 BEGIN，错误位置更可控。
 *
 * ## 步骤
 * step1. 持锁并检查连接；
 * step2. autocommit==0 说明事务已活动，拒绝嵌套；
 * step3. 执行 BEGIN IMMEDIATE，失败时记录并释放 errmsg。
 *
 * ## 并发/失败语义
 * - 调用约束：任意线程；事务状态是连接级的，跨线程共享，调用方需自行保证同一连接不并发开事务。
 * - 原子性：BEGIN 本身由 SQLite 原子完成。
 * - 失败语义：失败后连接仍处于 autocommit，可安全重试。
 * - 不变量：返回成功时 InTransaction() 为 true。
 */
Error DB::Begin() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const Error error = CheckOpenLocked(); error) {
        return error;
    }
    // step2: autocommit 为 0 表示已有事务；嵌套 BEGIN 会让 Commit 语义错位，直接拒绝。
    if (sqlite3_get_autocommit(m_db) == 0) {
        return Error(static_cast<int>(Code::kMisuse), 0,
                     "a transaction is already active");
    }
    char* message = nullptr;
    const int rc = sqlite3_exec(m_db, "BEGIN IMMEDIATE", nullptr, nullptr,
                                &message);
    if (rc != SQLITE_OK) {
        m_last_error = ErrorFromDb(m_db, rc);
        if (message != nullptr) {
            sqlite3_free(message);
        }
        return m_last_error;
    }
    return {};
}

/**
 * @brief 提交当前事务；无活动事务时报 misuse。
 *
 * @return
 * - 空 `Error`：事务已提交——调用方**必须**把「提交成功」当作数据可见的唯一信号；
 * - `kMisuse`：无活动事务或连接已关闭——调用方**不要**重复 Commit；
 * - 其他 `Error`：提交失败（BUSY/IO）——调用方**必须**按 IsBusy 决定重试或回滚。
 *
 * ## 定位
 * 事务唯一提交点。错把失败当成功会导致「以为已落库」的上层状态机出现半提交。
 *
 * ## 步骤
 * step1. 持锁并检查连接；
 * step2. autocommit!=0 说明没有活动事务，报 misuse；
 * step3. 执行 COMMIT，失败时锁存错误并释放 errmsg。
 *
 * ## 并发/失败语义
 * - 调用约束：任意线程；与同连接的写操作串行（持 m_mutex）。
 * - 原子性：COMMIT 由 SQLite 保证原子；失败后事务仍处于活动状态，需显式 Rollback。
 * - 失败语义：不抛异常；失败后 m_last_error 反映具体原因。
 * - 不变量：返回空 Error 后 InTransaction() 为 false 且数据对其他连接可见。
 */
Error DB::Commit() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const Error error = CheckOpenLocked(); error) {
        return error;
    }
    if (sqlite3_get_autocommit(m_db) != 0) {
        return Error(static_cast<int>(Code::kMisuse), 0,
                     "no transaction is active");
    }
    char* message = nullptr;
    // step3: COMMIT 失败时事务并未结束，调用方仍需 Rollback 才能回到 autocommit。
    const int rc = sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        m_last_error = ErrorFromDb(m_db, rc);
        if (message != nullptr) {
            sqlite3_free(message);
        }
        return m_last_error;
    }
    return {};
}

/**
 * @brief 回滚当前事务，丢弃自 Begin 以来的全部写入。
 *
 * @return
 * - 空 `Error`：已回滚——调用方**必须**把对象状态恢复到 Begin 之前；
 * - `kMisuse`：无活动事务或连接已关闭——调用方**不要**重复回滚；
 * - 其他 `Error`：回滚失败——调用方**必须**停止复用该连接并上报。
 *
 * ## 定位
 * 事务唯一撤销点，也是异常路径上保证「不留半写」的最后一道闸。
 *
 * ## 步骤
 * step1. 持锁并检查连接；
 * step2. 无活动事务时报 misuse；
 * step3. 执行 ROLLBACK，失败时锁存错误并释放 errmsg。
 *
 * ## 并发/失败语义
 * - 调用约束：任意线程；与同连接写操作串行。
 * - 原子性：ROLLBACK 由 SQLite 原子完成。
 * - 失败语义：失败后事务状态不变，仍可再试；m_last_error 记录原因。
 * - 不变量：返回空 Error 后 autocommit 恢复为 1。
 */
Error DB::Rollback() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const Error error = CheckOpenLocked(); error) {
        return error;
    }
    if (sqlite3_get_autocommit(m_db) != 0) {
        return Error(static_cast<int>(Code::kMisuse), 0,
                     "no transaction is active");
    }
    char* message = nullptr;
    // step3: 回滚失败属于严重状态，保留 m_last_error 供上层上报而不是静默吞掉。
    const int rc = sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        m_last_error = ErrorFromDb(m_db, rc);
        if (message != nullptr) {
            sqlite3_free(message);
        }
        return m_last_error;
    }
    return {};
}

bool DB::InTransaction() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_db != nullptr && sqlite3_get_autocommit(m_db) == 0;
}

Error DB::SetBusyTimeout(int milliseconds) {
    if (milliseconds < 0 || milliseconds > 600000) {
        return Error(static_cast<int>(Code::kRange), 0,
                     "busy timeout must be in [0, 600000] milliseconds");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const Error error = CheckOpenLocked(); error) {
        return error;
    }
    const int rc = sqlite3_busy_timeout(m_db, milliseconds);
    if (rc != SQLITE_OK) {
        m_last_error = MakeErrorLocked(rc);
        return m_last_error;
    }
    return {};
}

Error DB::SetJournalMode(std::string_view mode) {
    static constexpr const char* kModes[] = {"DELETE", "TRUNCATE", "PERSIST",
                                             "MEMORY", "WAL",    "OFF"};
    const std::string requested = ToUpperAscii(mode);
    bool known = false;
    for (const char* candidate : kModes) {
        if (requested == candidate) {
            known = true;
            break;
        }
    }
    if (!known) {
        return Error(static_cast<int>(Code::kRange), 0,
                     "unknown journal mode: " + std::string(mode));
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const Error error = CheckOpenLocked(); error) {
        return error;
    }
    const std::string sql = "PRAGMA journal_mode=" + requested;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        m_last_error = MakeErrorLocked(rc);
        return m_last_error;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        const Error error = MakeErrorLocked(rc);
        (void)sqlite3_finalize(stmt);
        m_last_error = error;
        return error;
    }
    const unsigned char* actual_text = sqlite3_column_text(stmt, 0);
    const std::string actual =
        actual_text != nullptr
            ? std::string(reinterpret_cast<const char*>(actual_text))
            : std::string();
    (void)sqlite3_finalize(stmt);
    if (!EqualsIgnoreCase(actual, requested)) {
        // 典型情况：:memory: 数据库请求 WAL，SQLite 返回 "memory"。
        return Error(static_cast<int>(Code::kError), 0,
                     "journal_mode=" + requested + " was not applied (actual: " +
                         actual + ")");
    }
    return {};
}

Error DB::SetSynchronous(std::string_view level) {
    static constexpr const char* kLevels[] = {"OFF", "NORMAL", "FULL",
                                              "EXTRA"};
    static constexpr const char* kNumeric[] = {"0", "1", "2", "3"};
    const std::string requested = ToUpperAscii(level);
    bool known = false;
    for (int i = 0; i < 4; ++i) {
        if (requested == kLevels[i] || requested == kNumeric[i]) {
            known = true;
            break;
        }
    }
    if (!known) {
        return Error(static_cast<int>(Code::kRange), 0,
                     "unknown synchronous level: " + std::string(level));
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const Error error = CheckOpenLocked(); error) {
        return error;
    }
    const std::string sql = "PRAGMA synchronous=" + requested;
    const int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        m_last_error = MakeErrorLocked(rc);
        return m_last_error;
    }
    return {};
}

Error DB::SetForeignKeys(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const Error error = CheckOpenLocked(); error) {
        return error;
    }
    const int rc = sqlite3_exec(m_db,
                                enabled ? "PRAGMA foreign_keys=ON"
                                        : "PRAGMA foreign_keys=OFF",
                                nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        m_last_error = MakeErrorLocked(rc);
        return m_last_error;
    }
    return {};
}

std::int64_t DB::LastInsertRowId() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_db == nullptr) {
        return 0;
    }
    return static_cast<std::int64_t>(sqlite3_last_insert_rowid(m_db));
}

int DB::Changes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_db == nullptr) {
        return 0;
    }
    return sqlite3_changes(m_db);
}

Error DB::LastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error;
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::~Transaction() {
    if (m_active && m_db != nullptr) {
        (void)m_db->Rollback();
    }
}

Error Transaction::Begin() {
    if (m_active) {
        return Error(static_cast<int>(Code::kMisuse), 0,
                     "transaction is already active");
    }
    const Error error = m_db->Begin();
    if (!error) {
        m_active = true;
    }
    return error;
}

Error Transaction::Commit() {
    if (!m_active) {
        return Error(static_cast<int>(Code::kMisuse), 0,
                     "transaction is not active");
    }
    const Error error = m_db->Commit();
    if (!error) {
        m_active = false;
    }
    return error;
}

Error Transaction::Rollback() {
    if (!m_active) {
        return Error(static_cast<int>(Code::kMisuse), 0,
                     "transaction is not active");
    }
    const Error error = m_db->Rollback();
    if (!error) {
        m_active = false;
    }
    return error;
}

} // namespace storage
} // namespace go2cpp
