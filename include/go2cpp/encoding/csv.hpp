/**
 * @file csv.hpp
 * @brief Go `encoding/csv` 的简化 C++ 对应实现（Reader / Writer）。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（语义对齐 Go 标准库；本文件为独立实现）
 *
 * @details 复用优先的结论（检索日期 2026-10-18）：
 *   | 候选 | 结论 |
 *   |---|---|
 *   | Go 标准库 `encoding/csv`（Go 1.23.0，BSD-3-Clause） | **语义参照**：引号规则、
 *     `""` 转义、字段内换行、空行跳过、`FieldsPerRecord` 推断、`ErrBareQuote` /
 *     `ErrQuote` / `ErrFieldCount` 三类错误、Writer 的「何时加引号」规则都按它对齐 |
 *   | [rapidcsv](https://github.com/d99kris/rapidcsv)（MIT，header-only） | **不采用**：
 *     它面向「整张表按行/列索引」的 DataFrame 风格，会把数据一次性解析成二维表；
 *     本模块需要的是 Go 风格的**流式逐条 `Read()` + 严格错误定位**，接口语义不匹配 |
 *   | 自研 | **采用**：核心状态机约 150 行，且必须精确复刻 Go 的引号/换行规则 |
 *
 * ## 实现范围（简化点）
 *
 * 与 Go 的 `encoding/csv` 相比，本模块**只保留**需求覆盖的核心能力：
 * - 固定逗号分隔（可 `SetComma` 改成其它单字节分隔符），不支持多字节 rune；
 * - `Reader` 从**内存中的完整文本**解析（构造时传入 `std::string`）；Go 的
 *   `io.Reader` 流式读取未实现 —— 调用方先读文件再构造即可；
 * - 已支持 `TrimLeadingSpace`（`SetTrimLeadingSpace`）与 `Comment`
 *   （`SetComment`）；默认全关，等价于 Go 的零值 Reader；
 * - 不支持 UTF-8 BOM 自动剥离（Go 也不剥离）；首字段若带 BOM 会原样保留。
 *
 * ## 已知未支持项及原因（C 类，有意不做）
 *
 * | 未支持项 | 原因 |
 * |---|---|
 * | `LazyQuotes` | Go 特有的宽松模式（允许非引号字段里的裸 `"`、引号字段里落单的 `"`）。它会让同一个文件在不同解析器里得到不同结果，本模块刻意只保留严格模式；需要容错时应在数据源侧修数据。 |
 * | `ReuseRecord` | Go 的性能优化：把上一条记录复用为缓冲区，减少分配。C++ 侧 `Read()` 返回值语义是「调用方拥有」，复用会引入别名陷阱；需要省分配时由调用方复用 `CsvReadResult` 变量即可。 |
 * | `Writer.Error` | Go 的 `Writer` 包装 `io.Writer` 后需要 `Error()` 报告延迟写入错误；本实现的 `CsvWriter` 只写内存 `std::string`，没有可失败的底层 IO，`Write` 恒返回 `nullptr`（见其注释）。 |
 * | `Reader` 从 `io.Reader` 流式解析 | 本模块构造时持有完整文本（`std::string`）；流式解析需要 `bufio.Reader` 级别的增量状态机，属于后续扩展。当前调用方先读文件再构造即可。 |
 *
 * ## 关键语义（与 Go 对齐）
 *
 * **Reader**
 * - 空行（只有 `\n` 或 `\r\n` 的行）**被跳过**；只含空白字符的行不是空行。
 * - 字段内的 `\r\n` 统一转成 `\n`（包括引号字段内部）；孤立的 `\r` 原样保留。
 * - 引号字段必须以 `"` 开头；内部 `""` 表示一个 `"`；闭合引号后只能是分隔符、
 *   换行或输入结束，否则报 `CsvErrQuote()`。
 * - 非引号字段里出现 `"` 报 `CsvErrBareQuote()`（与 Go 默认一致）。
 * - 引号未闭合报 `CsvErrQuote()`。
 * - `FieldsPerRecord`：默认 `0` 表示用第一条记录推断字段数并强制后续一致；
 *   `-1` 关闭校验；`> 0` 使用调用方指定的数量。不一致报 `CsvErrFieldCount()`。
 * - 结尾无换行的最后一条记录正常返回；结尾逗号（如 `"a,"`）产生一个空尾字段。
 * - `Read()` 读到结尾返回 `eof == true`（`error == nullptr`），对应 Go 的 `io.EOF`。
 *
 * **Writer**
 * - 字段含分隔符、`"`、`\r`、`\n`，或以空白字符开头时加引号；内部 `"` 写成 `""`。
 * - 空字段不加引号；字段恰好为 `\.` 时加引号（Go 为 PostgreSQL 保留的特例）。
 * - 默认换行 `\n`；`SetUseCRLF(true)` 改为 `\r\n`（对应 Go `UseCRLF`）。
 * - 分隔符必须是单字节且不能是 `\0`、`"`、`\r`、`\n`（对应 Go `validDelim`）。
 */

#pragma once

#include "go2cpp/tools/error.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace go2cpp::encoding {

/// 对应 Go `csv.ErrBareQuote`：非引号字段中出现裸 `"`。
inline ErrorPtr CsvErrBareQuote() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("bare \" in non-quoted-field"));
    return *kError;
}

/// 对应 Go `csv.ErrQuote`：引号字段后有多余内容或引号未闭合。
inline ErrorPtr CsvErrQuote() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("extraneous or missing \" in quoted-field"));
    return *kError;
}

/// 对应 Go `csv.ErrFieldCount`：记录字段数与 `FieldsPerRecord` 不符。
inline ErrorPtr CsvErrFieldCount() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("wrong number of fields"));
    return *kError;
}

/// `CsvReader::Read` 的返回值：对应 Go 的 `([]string, error)`。
struct CsvReadResult {
    /// 成功时的字段列表；EOF 或失败时为空。
    std::vector<std::string> record;
    /// 成功 / EOF 时为 `nullptr`。
    ErrorPtr error;
    /// 是否已到输入末尾；与 `error` 互斥。
    bool eof = false;

    /// 仅当「成功读到一条记录」时为 true（EOF 与错误为 false）。
    explicit operator bool() const noexcept { return error == nullptr && !eof; }
};

/**
 * @brief 逐条读取 CSV 记录，对应 Go 的 `*csv.Reader`。
 *
 * @note 非线程安全：一个实例只应被一个线程/Fiber 使用（与 Go 一致）。
 */
class CsvReader {
 public:
    /**
     * @brief 用完整文本构造 Reader。
     * @param data 待解析的 CSV 文本（被复制保存，调用方无需保持生命周期）。
     * @param comma 字段分隔符，默认 `','`。
     * @throws std::invalid_argument 当 `comma` 不是合法分隔符时。
     */
    explicit CsvReader(std::string data, char comma = ',');

    /**
     * @brief 读取下一条记录。
     * @return 成功时 `record` 有值；到末尾时 `eof == true`；失败时 `error` 非空。
     * @note 出错后实例进入「已失败」状态：后续调用会返回同一个错误，
     *       避免调用方误以为还能继续解析。可重新构造实例恢复。
     */
    CsvReadResult Read();

    /**
     * @brief 设置期望字段数。
     * @param fields `0` = 由首条记录推断；`-1` = 不校验；`> 0` = 固定数量。
     * @return 参数合法返回 true，否则 false 且不修改。
     */
    bool SetFieldsPerRecord(int fields);

    /// 当前期望字段数（`0`/`-1`/正整数）。
    int FieldsPerRecord() const noexcept { return m_fields_per_record; }

    /**
     * @brief 对应 Go `Reader.TrimLeadingSpace`：忽略每个字段开头的前导空白。
     * @param enabled true 开启（默认 false）。
     * @note 只处理 ASCII 空格与 `\t`（不含 Unicode 空白，见文件头 C 类清单中
     *       对 `ScanWords`/rune 分类的同类说明）。引号字段内部不裁剪。
     */
    void SetTrimLeadingSpace(bool enabled) noexcept {
        m_trim_leading_space = enabled;
    }

    /// 是否开启前导空白裁剪。
    bool TrimLeadingSpace() const noexcept { return m_trim_leading_space; }

    /**
     * @brief 对应 Go `Reader.Comment`：以该字符开头的整行被忽略。
     * @param comment 注释字符；传 `'\0'` 关闭注释功能。
     * @return 合法返回 true；`'\r'`/`'\n'` 返回 false 且不修改
     *         （Go 对非法注释符 panic，这里降级为 bool）。
     * @note 注释判定发生在记录起始处；引号字段内部的 `#` 仍是普通内容。
     */
    bool SetComment(char comment) noexcept;

    /// 当前注释字符；`'\0'` 表示关闭。
    char Comment() const noexcept { return m_comment; }

    /// 下一次解析位置所在的行号（1 起）；仅用于诊断。
    std::size_t Line() const noexcept { return m_line; }

    /// 下一次解析位置（字节偏移）；仅用于诊断。
    std::size_t Offset() const noexcept { return m_pos; }

    /// 分隔符。
    char Comma() const noexcept { return m_comma; }

 private:
    /// 生成带行号的解析错误。
    ErrorPtr ParseError(const ErrorPtr& kind, std::size_t start_line,
                        const std::string& detail) const;

    std::string m_data;
    std::size_t m_pos = 0;
    std::size_t m_line = 1;
    int m_fields_per_record = 0;
    char m_comma;
    bool m_trim_leading_space = false;
    char m_comment = '\0';
    ErrorPtr m_error;
};

/**
 * @brief 把记录写成 CSV 文本，对应 Go 的 `*csv.Writer`。
 *
 * @details 输出保存在内部缓冲区，`Str()` 取得、`Release()` 取走；`Flush()` 是
 *          兼容 Go 习惯的空操作（本实现直接写内存，无缓冲层）。
 */
class CsvWriter {
 public:
    CsvWriter();
    explicit CsvWriter(char comma);

    /// 设置分隔符；非法时返回 false 且保持原值。
    bool SetComma(char comma) noexcept;

    /// 当前分隔符。
    char Comma() const noexcept { return m_comma; }

    /// 设置换行符是否使用 `\r\n`（默认 `\n`）。
    void SetUseCRLF(bool use_crlf) noexcept { m_use_crlf = use_crlf; }

    /// 是否使用 `\r\n`。
    bool UseCRLF() const noexcept { return m_use_crlf; }

    /**
     * @brief 写一条记录（追加到内部缓冲区）。
     * @return 目前恒为 `nullptr`；保留 ErrorPtr 返回值是为了与 Reader 及未来
     *         可能的错误（如写入失败）保持一致的调用形态。
     */
    ErrorPtr Write(const std::vector<std::string>& record);

    /// Go `Writer.Flush` 的兼容空操作（内存写入无需刷新）。
    void Flush() noexcept {}

    /// 已生成的 CSV 文本。
    const std::string& Str() const noexcept { return m_buf; }

    /// 可修改的已生成文本。
    std::string& Str() noexcept { return m_buf; }

    /// 取走内部缓冲区并清空。
    std::string Release();

    /// 清空内部缓冲区（保留分隔符与换行设置）。
    void Reset() noexcept { m_buf.clear(); }

    /// 判断单字节分隔符是否合法（对应 Go `validDelim` 的 ASCII 子集）。
    static bool IsValidDelimiter(char comma) noexcept;

 private:
    /// 字段是否需要加引号（对齐 Go `fieldNeedsQuotes`）。
    bool FieldNeedsQuotes(const std::string& field) const noexcept;

    std::string m_buf;
    char m_comma = ',';
    bool m_use_crlf = false;
};

/// Go 包名 `encoding/csv` 的门面：`Csv::Reader` / `Csv::Writer`。
struct Csv {
    using Reader = CsvReader;
    using Writer = CsvWriter;
    using ReadResult = CsvReadResult;
    using Error = ErrorPtr;
};

}  // namespace go2cpp::encoding
