/**
 * @file csv.cpp
 * @brief `go2cpp/encoding/csv.hpp` 的实现：Go 风格 CSV Reader / Writer。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause
 *
 * @details Reader 是一个逐字符状态机：引号字段与非引号字段分开扫描，
 *          每条记录解析完才做 `FieldsPerRecord` 校验。这样错误能定位到
 *          记录起始行，且中途失败不会污染已返回的记录。
 *
 *          与 Go 的一个已知差异：Go 的 `Read()` 在解析错误后仍可继续调用
 *          （错误只返回一次）；本实现把错误设为粘性，避免调用方在错位状态上
 *          继续解析产生误导性结果。
 *
 * ## 模块定位
 *
 * `csv.hpp` 的实现层：`CsvReader` 的逐字符解析状态机与 `CsvWriter` 的
 * 加引号/拼接逻辑。被数据导入、配置读取、导出等上层直接使用。
 *
 * ## 本文件定义什么
 *
 * - `CsvReader::Read`：完整解析流程（空行/注释跳过、两类字段、字段数校验）；
 * - `CsvReader` 的配置/诊断函数（分隔符、注释、字段数、行号/偏移）；
 * - `CsvWriter` 的 `Write`/`FieldNeedsQuotes`/`Release` 等；
 * - 内部工具 `StartsWithSpace`（Writer 的引号判定）。
 *
 * ## 阅读建议
 *
 * 先读 `CsvReader::Read` 的 5 个步骤，再对照 `docs/state-machines/CsvReader.md`
 * 看状态转化；Writer 侧先看「何时加引号」再看转义循环。
 *
 * ## 边界与陷阱
 *
 * Reader 的错误是粘性的；字段内 CRLF 统一成 LF；孤立 `\r` 原样保留；
 * 结尾逗号会产生一个空尾字段。Writer 的 `Flush()` 是空操作。
 */

#include "go2cpp/encoding/csv.hpp"

#include "go2cpp/encoding/errors.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::encoding {

namespace {

/// 首字节是否是 Go `unicode.IsSpace` 会判定的空白（用于 Writer 加引号规则）。
bool StartsWithSpace(const std::string& field) noexcept {
    const unsigned char c = static_cast<unsigned char>(field[0]);
    if (c < 0x80) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
               c == '\r';
    }
    // 常见 Unicode 空白：U+0085、U+00A0、U+1680、U+2000..U+200A、
    // U+2028、U+2029、U+202F、U+205F、U+3000。只解码首个 rune，足够覆盖
    // 实际 CSV 数据里会出现的空白字符。
    const std::size_t size = field.size();
    if (c == 0xC2 && size >= 2) {
        const unsigned char c1 = static_cast<unsigned char>(field[1]);
        return c1 == 0x85 || c1 == 0xA0;
    }
    if (c == 0xE1 && size >= 3) {
        return static_cast<unsigned char>(field[1]) == 0x9A &&
               static_cast<unsigned char>(field[2]) == 0x80;
    }
    if (c == 0xE2 && size >= 3) {
        const unsigned char c1 = static_cast<unsigned char>(field[1]);
        const unsigned char c2 = static_cast<unsigned char>(field[2]);
        if (c1 == 0x80) {
            // U+2000..U+200A / U+2028 / U+2029 / U+202F
            if (c2 >= 0x80 && c2 <= 0x8A) return true;
            if (c2 == 0xA8 || c2 == 0xA9 || c2 == 0xAF) return true;
        }
        if (c1 == 0x81 && c2 == 0x9F) return true;  // U+205F
    }
    if (c == 0xE3 && size >= 3) {
        return static_cast<unsigned char>(field[1]) == 0x80 &&
               static_cast<unsigned char>(field[2]) == 0x80;  // U+3000
    }
    return false;
}

}  // namespace

bool CsvWriter::IsValidDelimiter(char comma) noexcept {
    return comma != '\0' && comma != '"' && comma != '\r' && comma != '\n';
}

CsvReader::CsvReader(std::string data, char comma)
    : m_data(std::move(data)), m_comma(comma) {
    if (!CsvWriter::IsValidDelimiter(comma)) {
        throw std::invalid_argument("csv: invalid field delimiter");
    }
}

ErrorPtr CsvReader::ParseError(const ErrorPtr& kind, std::size_t start_line,
                               const std::string& description) const {
    std::string message = "record on line " + std::to_string(start_line);
    message += ": ";
    message += description;
    return detail::Tagged(std::move(message), kind);
}

bool CsvReader::SetFieldsPerRecord(int fields) {
    if (fields < -1) {
        return false;
    }
    m_fields_per_record = fields;
    return true;
}

bool CsvReader::SetComment(char comment) noexcept {
    if (comment == '\r' || comment == '\n') {
        return false;
    }
    m_comment = comment;
    return true;
}

/**
 * @brief 解析下一条记录。
 *
 * ## 步骤
 * step1. 已有粘性错误直接返回；否则跳过空行与注释行，定位记录起点；
 * step2. 逐字段解析：先按需裁掉前导空白，再按首字符分派引号/非引号；
 * step3. 引号字段：处理 `""` 转义、字段内 CRLF→LF，闭合后只允许
 *        分隔符/换行/EOF；
 * step4. 非引号字段：遇分隔符/换行结束，裸 `"` 报错；处理结尾逗号；
 * step5. 记录级 `FieldsPerRecord` 校验（首条推断/固定/关闭），
 *        失败置粘性错误。
 *
 * ## 失败语义
 * 任何解析错误都会写入 `m_error` 并立即返回；后续调用返回同一个错误。
 */
CsvReadResult CsvReader::Read() {
    CsvReadResult result;
    // step1: 粘性错误优先
    if (m_error) {
        result.error = m_error;
        return result;
    }

    const std::size_t size = m_data.size();

    // step1: 跳过空行与注释行，与 Go 一致
    for (;;) {
        if (m_pos >= size) {
            result.eof = true;
            return result;
        }
        if (m_data[m_pos] == '\n') {
            ++m_pos;
            ++m_line;
            continue;
        }
        if (m_data[m_pos] == '\r' && m_pos + 1 < size &&
            m_data[m_pos + 1] == '\n') {
            m_pos += 2;
            ++m_line;
            continue;
        }
        if (m_comment != '\0' && m_data[m_pos] == m_comment) {
            // 注释行：整行忽略（含行尾 \r）；下一轮继续跳过空行/注释。
            while (m_pos < size && m_data[m_pos] != '\n') {
                ++m_pos;
            }
            if (m_pos < size) {
                ++m_pos;
                ++m_line;
            }
            continue;
        }
        break;
    }

    const std::size_t start_line = m_line;
    std::vector<std::string> record;

    // step2: 逐字段解析；每个字段独立处理前导空白
    for (;;) {
        if (m_pos >= size) {
            // 只可能由「结尾逗号」走到这里：补一个空尾字段。
            record.emplace_back();
            break;
        }

        // TrimLeadingSpace：在解析每个字段前裁掉 ASCII 空格/Tab。放在这里
        // 而不是记录级，才能同时覆盖 `a, b` 这种逗号后的前导空白。
        if (m_trim_leading_space) {
            while (m_pos < size &&
                   (m_data[m_pos] == ' ' || m_data[m_pos] == '\t')) {
                ++m_pos;
            }
            if (m_pos >= size) {
                record.emplace_back();
                break;
            }
        }

        std::string field;
        // step3: 引号字段
        if (m_data[m_pos] == '"') {
            // ---- 引号字段 ----
            ++m_pos;
            bool closed = false;
            while (m_pos < size) {
                const char c = m_data[m_pos++];
                if (c == '"') {
                    if (m_pos < size && m_data[m_pos] == '"') {
                        field.push_back('"');
                        ++m_pos;
                        continue;
                    }
                    closed = true;
                    break;
                }
                if (c == '\r') {
                    if (m_pos < size && m_data[m_pos] == '\n') {
                        // 字段内 CRLF 统一成 LF（对齐 Go 的 readLine）。
                        field.push_back('\n');
                        ++m_pos;
                        ++m_line;
                    } else {
                        field.push_back('\r');
                    }
                } else if (c == '\n') {
                    field.push_back('\n');
                    ++m_line;
                } else {
                    field.push_back(c);
                }
            }

            if (!closed) {
                result.error = ParseError(
                    CsvErrQuote(), start_line,
                    "extraneous or missing \" in quoted-field");
                m_error = result.error;
                return result;
            }

            if (m_pos < size) {
                const char after = m_data[m_pos];
                if (after == ',') {
                    record.push_back(std::move(field));
                    ++m_pos;
                    continue;
                }
                if (after == '\n') {
                    record.push_back(std::move(field));
                    ++m_pos;
                    ++m_line;
                    break;
                }
                if (after == '\r' && m_pos + 1 < size &&
                    m_data[m_pos + 1] == '\n') {
                    record.push_back(std::move(field));
                    m_pos += 2;
                    ++m_line;
                    break;
                }
                result.error = ParseError(
                    CsvErrQuote(), start_line,
                    "extraneous or missing \" in quoted-field");
                m_error = result.error;
                return result;
            }

            record.push_back(std::move(field));
            break;
        }

        // step4: 非引号字段
        while (m_pos < size) {
            const char c = m_data[m_pos];
            if (c == ',') break;
            if (c == '\n') break;
            if (c == '\r' && m_pos + 1 < size && m_data[m_pos + 1] == '\n') {
                break;
            }
            if (c == '"') {
                result.error = ParseError(CsvErrBareQuote(), start_line,
                                          "bare \" in non-quoted-field");
                m_error = result.error;
                return result;
            }
            field.push_back(c);
            ++m_pos;
        }
        record.push_back(std::move(field));

        if (m_pos >= size) {
            break;
        }
        const char separator = m_data[m_pos];
        if (separator == ',') {
            ++m_pos;
            if (m_pos >= size) {
                record.emplace_back();
                break;
            }
            continue;
        }
        if (separator == '\n') {
            ++m_pos;
            ++m_line;
            break;
        }
        // 只剩 CRLF 一种可能（孤立 '\r' 已被当作字段内容）。
        m_pos += 2;
        ++m_line;
        break;
    }

    // step5: 字段数校验（0=首条推断，-1=不校验，>0=固定）
    if (m_fields_per_record == 0) {
        m_fields_per_record = static_cast<int>(record.size());
    } else if (m_fields_per_record > 0 &&
               record.size() != static_cast<std::size_t>(m_fields_per_record)) {
        result.error = ParseError(CsvErrFieldCount(), start_line,
                                  "wrong number of fields");
        m_error = result.error;
        return result;
    }

    result.record = std::move(record);
    return result;
}

CsvWriter::CsvWriter() : CsvWriter(',') {}

CsvWriter::CsvWriter(char comma) {
    if (!IsValidDelimiter(comma)) {
        throw std::invalid_argument("csv: invalid field delimiter");
    }
    m_comma = comma;
}

bool CsvWriter::SetComma(char comma) noexcept {
    if (!IsValidDelimiter(comma)) {
        return false;
    }
    m_comma = comma;
    return true;
}

bool CsvWriter::FieldNeedsQuotes(const std::string& field) const noexcept {
    if (field.empty()) {
        return false;
    }
    if (field == "\\.") {
        // Go 为 PostgreSQL 的 COPY 保留的特例。
        return true;
    }
    if (m_comma == ' ') {
        if (field.find(' ') != std::string::npos) {
            return true;
        }
    } else if (field.find(m_comma) != std::string::npos ||
               field.find_first_of("\"\r\n") != std::string::npos) {
        return true;
    }
    return StartsWithSpace(field);
}

/**
 * @brief 把一条记录追加到内部缓冲。
 *
 * ## 步骤
 * step1. 逐字段写入；非首字段先写分隔符；
 * step2. 需要引号的字段用 `"` 包裹，内部 `"` 写成 `""`；
 * step3. 记录末尾按 `m_use_crlf` 追加 `\n` 或 `\r\n`。
 *
 * @return 目前恒为 `nullptr`；保留 `ErrorPtr` 是为了与 Reader 形态一致。
 */
ErrorPtr CsvWriter::Write(const std::vector<std::string>& record) {
    // step1: 逐字段拼接，字段之间写分隔符
    for (std::size_t i = 0; i < record.size(); ++i) {
        if (i > 0) {
            m_buf.push_back(m_comma);
        }
        const std::string& field = record[i];
        // step2: 不需要引号的原样写入；需要时包裹并转义内部引号
        if (!FieldNeedsQuotes(field)) {
            m_buf += field;
            continue;
        }
        m_buf.push_back('"');
        for (const char c : field) {
            if (c == '"') {
                m_buf.push_back('"');
            }
            m_buf.push_back(c);
        }
        m_buf.push_back('"');
    }
    // step3: 记录尾换行
    if (m_use_crlf) {
        m_buf += "\r\n";
    } else {
        m_buf.push_back('\n');
    }
    return nullptr;
}

std::string CsvWriter::Release() {
    std::string out = std::move(m_buf);
    m_buf.clear();
    return out;
}

}  // namespace go2cpp::encoding
