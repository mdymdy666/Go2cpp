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

CsvReadResult CsvReader::Read() {
    CsvReadResult result;
    if (m_error) {
        result.error = m_error;
        return result;
    }

    const std::size_t size = m_data.size();

    // 空行（仅含行结束符）直接跳过，与 Go 一致。
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
        break;
    }

    const std::size_t start_line = m_line;
    std::vector<std::string> record;

    for (;;) {
        if (m_pos >= size) {
            // 只可能由「结尾逗号」走到这里：补一个空尾字段。
            record.emplace_back();
            break;
        }

        std::string field;
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

        // ---- 非引号字段 ----
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

ErrorPtr CsvWriter::Write(const std::vector<std::string>& record) {
    for (std::size_t i = 0; i < record.size(); ++i) {
        if (i > 0) {
            m_buf.push_back(m_comma);
        }
        const std::string& field = record[i];
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
