/**
 * @file test_csv.cpp
 * @brief `encoding::Csv` 的单元测试：Reader 引号/换行/空行语义、Writer 引号规则。
 *
 * 覆盖：
 *   - 普通记录、字段内逗号、字段内换行、`""` 转义、CRLF 归一化；
 *   - 空行跳过、空字段、结尾逗号、无结尾换行的最后一条记录；
 *   - `FieldsPerRecord` 推断 / 固定 / 关闭；
 *   - 三类错误：裸引号、引号后多余内容、引号未闭合、字段数不符；
 *   - Writer 的「何时加引号」规则、`\r\n` 选项、分隔符校验；
 *   - Writer → Reader 往返。
 */

#include "encoding_test_support.hpp"

#include "go2cpp/encoding/csv.hpp"
#include "go2cpp/error.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace e = go2cpp::encoding;

using go2cpp::ErrorMessage;
using go2cpp::Is;

namespace {

std::vector<std::vector<std::string>> ReadAll(const std::string& text,
                                              std::size_t* records_out = nullptr) {
    e::CsvReader reader(text);
    std::vector<std::vector<std::string>> records;
    for (;;) {
        const auto result = reader.Read();
        if (result.eof) {
            break;
        }
        if (result.error) {
            ENCODING_CHECK(false);
            std::fprintf(stderr, "    unexpected CSV error: %s\n",
                         ErrorMessage(result.error).c_str());
            break;
        }
        records.push_back(result.record);
    }
    if (records_out != nullptr) {
        *records_out = records.size();
    }
    return records;
}

void CheckRecord(const std::vector<std::string>& actual,
                 const std::vector<std::string>& expected, const char* what) {
    if (actual == expected) {
        return;
    }
    ENCODING_CHECK(false);
    std::fprintf(stderr, "    %s: field count %zu != %zu\n", what, actual.size(),
                 expected.size());
    for (std::size_t i = 0; i < actual.size() && i < expected.size(); ++i) {
        if (actual[i] != expected[i]) {
            std::fprintf(stderr, "      field %zu: \"%s\" != \"%s\"\n", i,
                         actual[i].c_str(), expected[i].c_str());
        }
    }
}

/// 断言 Reader 报出指定错误锚点。
void ExpectReadError(const std::string& text, const go2cpp::ErrorPtr& anchor,
                     const char* what) {
    e::CsvReader reader(text);
    const auto result = reader.Read();
    ENCODING_CHECK(result.error != nullptr);
    ENCODING_CHECK(Is(result.error, anchor));
    ENCODING_CHECK(!result.eof);
    std::printf("    rejected %-28s (%s)\n", what,
                ErrorMessage(result.error).c_str());
}

}  // namespace

void RunCsvTests() {
    go2cpp_encoding_test::Announce("csv: basic records");

    {
        const auto records = ReadAll("a,b,c\n1,2,3\n");
        ENCODING_REQUIRE(records.size() == 2);
        CheckRecord(records[0], {"a", "b", "c"}, "header");
        CheckRecord(records[1], {"1", "2", "3"}, "row");
    }
    {
        // 最后一条记录没有结尾换行。
        const auto records = ReadAll("a,b\nc,d");
        ENCODING_REQUIRE(records.size() == 2);
        CheckRecord(records[1], {"c", "d"}, "no trailing newline");
    }
    {
        // 空行被跳过；末尾连续空行同理。
        const auto records = ReadAll("\n\na,b\n\nc,d\n\n");
        ENCODING_REQUIRE(records.size() == 2);
        CheckRecord(records[0], {"a", "b"}, "after blank lines");
        CheckRecord(records[1], {"c", "d"}, "middle blank line");
    }
    {
        // CRLF 行结束符不进入字段。
        const auto records = ReadAll("a,b\r\nc,d\r\n");
        ENCODING_REQUIRE(records.size() == 2);
        CheckRecord(records[0], {"a", "b"}, "crlf first");
        CheckRecord(records[1], {"c", "d"}, "crlf second");
    }
    {
        // 结尾逗号产生空尾字段。
        const auto records = ReadAll("a,\n,b\n,\n");
        ENCODING_REQUIRE(records.size() == 3);
        CheckRecord(records[0], {"a", ""}, "trailing empty");
        CheckRecord(records[1], {"", "b"}, "leading empty");
        CheckRecord(records[2], {"", ""}, "both empty");
    }
    {
        e::CsvReader reader("");
        const auto result = reader.Read();
        ENCODING_CHECK(result.eof);
        ENCODING_CHECK(result.error == nullptr);
        const auto again = reader.Read();
        ENCODING_CHECK(again.eof);
    }

    go2cpp_encoding_test::Announce("csv: quoted fields");

    {
        const auto records = ReadAll("\"a,b\",c\n");
        ENCODING_REQUIRE(records.size() == 1);
        CheckRecord(records[0], {"a,b", "c"}, "comma in quotes");
    }
    {
        const auto records = ReadAll("\"He said \"\"hi\"\"\",x\n");
        ENCODING_REQUIRE(records.size() == 1);
        CheckRecord(records[0], {"He said \"hi\"", "x"}, "escaped quotes");
    }
    {
        const auto records = ReadAll("\"line1\nline2\",z\n");
        ENCODING_REQUIRE(records.size() == 1);
        CheckRecord(records[0], {"line1\nline2", "z"}, "embedded newline");
    }
    {
        // 引号字段内的 CRLF 归一化为 LF（对齐 Go）。
        const auto records = ReadAll("\"line1\r\nline2\",z\r\n");
        ENCODING_REQUIRE(records.size() == 1);
        CheckRecord(records[0], {"line1\nline2", "z"}, "embedded crlf");
    }
    {
        // 引号字段里的空串是合法字段，不是空行；两行的字段数不同，
        // 因此关闭 FieldsPerRecord 推断。
        e::CsvReader reader("\"\"\n\"\",\"\"\n");
        ENCODING_REQUIRE(reader.SetFieldsPerRecord(-1));
        const auto first = reader.Read();
        ENCODING_REQUIRE(first);
        CheckRecord(first.record, {""}, "quoted empty");
        const auto second = reader.Read();
        ENCODING_REQUIRE(second);
        CheckRecord(second.record, {"", ""}, "two quoted empty");
    }
    {
        // 字段内孤立 \r 原样保留。
        const auto records = ReadAll("a\rb,c\n");
        ENCODING_REQUIRE(records.size() == 1);
        CheckRecord(records[0], {"a\rb", "c"}, "lone carriage return");
    }
    {
        // 字段以空格开头时它不是引号字段，随后的 `"` 属于非法裸引号（Go 同样报错）。
        ExpectReadError(" \"a\",b\n", e::CsvErrBareQuote(), "quote after space");
    }

    go2cpp_encoding_test::Announce("csv: FieldsPerRecord");

    {
        e::CsvReader reader("a,b\n1,2\n3,4\n");
        ENCODING_REQUIRE(reader.FieldsPerRecord() == 0);
        const auto first = reader.Read();
        ENCODING_REQUIRE(first);
        ENCODING_CHECK(reader.FieldsPerRecord() == 2);
        const auto second = reader.Read();
        ENCODING_REQUIRE(second);
        const auto third = reader.Read();
        ENCODING_REQUIRE(third);
        ENCODING_CHECK(reader.Read().eof);
    }
    {
        e::CsvReader reader("a,b\n1,2,3\n");
        ENCODING_REQUIRE(reader.Read());
        const auto bad = reader.Read();
        ENCODING_CHECK(bad.error != nullptr);
        ENCODING_CHECK(Is(bad.error, e::CsvErrFieldCount()));
        // 粘性错误：再次 Read 仍返回同一错误。
        const auto again = reader.Read();
        ENCODING_CHECK(again.error != nullptr);
        ENCODING_CHECK(Is(again.error, e::CsvErrFieldCount()));
    }
    {
        e::CsvReader reader("a,b\n1,2,3\n");
        ENCODING_REQUIRE(reader.SetFieldsPerRecord(-1));
        ENCODING_REQUIRE(reader.Read());
        const auto second = reader.Read();
        ENCODING_REQUIRE(second);
        CheckRecord(second.record, {"1", "2", "3"}, "variable fields");
        ENCODING_CHECK(!reader.SetFieldsPerRecord(-2));
        ENCODING_CHECK(reader.SetFieldsPerRecord(1));
    }
    {
        // 固定字段数在第一次读取时就生效。
        e::CsvReader reader("a,b,c\n", ',');
        reader.SetFieldsPerRecord(2);
        const auto result = reader.Read();
        ENCODING_CHECK(result.error != nullptr);
        ENCODING_CHECK(Is(result.error, e::CsvErrFieldCount()));
    }

    go2cpp_encoding_test::Announce("csv: parse errors");

    ExpectReadError("a\"b,c\n", e::CsvErrBareQuote(), "bare quote");
    ExpectReadError("\"a\"b,c\n", e::CsvErrQuote(), "text after closing quote");
    ExpectReadError("\"abc\n", e::CsvErrQuote(), "unterminated quote");
    ExpectReadError("\"abc", e::CsvErrQuote(), "unterminated at eof");
    {
        // 错误文本带记录起始行号。
        e::CsvReader reader("ok,1\n\"bad\n");
        ENCODING_REQUIRE(reader.Read());
        const auto result = reader.Read();
        ENCODING_REQUIRE(result.error != nullptr);
        ENCODING_CHECK(ErrorMessage(result.error).find("line 2") !=
                       std::string::npos);
    }

    go2cpp_encoding_test::Announce("csv: writer quoting rules");

    {
        e::CsvWriter writer;
        ENCODING_CHECK(writer.Write({"a", "b", "c"}) == nullptr);
        ENCODING_CHECK(writer.Str() == "a,b,c\n");
        writer.Reset();
        ENCODING_CHECK(writer.Write({"a,b", "c"}) == nullptr);
        ENCODING_CHECK(writer.Str() == "\"a,b\",c\n");
        writer.Reset();
        ENCODING_CHECK(writer.Write({"He said \"hi\"", "x"}) == nullptr);
        ENCODING_CHECK(writer.Str() == "\"He said \"\"hi\"\"\",x\n");
        writer.Reset();
        ENCODING_CHECK(writer.Write({"line1\nline2"}) == nullptr);
        ENCODING_CHECK(writer.Str() == "\"line1\nline2\"\n");
        writer.Reset();
        ENCODING_CHECK(writer.Write({"a\rb"}) == nullptr);
        ENCODING_CHECK(writer.Str() == "\"a\rb\"\n");
        writer.Reset();
        ENCODING_CHECK(writer.Write({" leading"}) == nullptr);
        ENCODING_CHECK(writer.Str() == "\" leading\"\n");
        writer.Reset();
        // 空字段不加引号；空记录只写换行。
        ENCODING_CHECK(writer.Write({"", ""}) == nullptr);
        ENCODING_CHECK(writer.Str() == ",\n");
        writer.Reset();
        ENCODING_CHECK(writer.Write({}) == nullptr);
        ENCODING_CHECK(writer.Str() == "\n");
        writer.Reset();
        // PostgreSQL 特例。
        ENCODING_CHECK(writer.Write({"\\."}) == nullptr);
        ENCODING_CHECK(writer.Str() == "\"\\.\"\n");
        // U+3000（全角空格）开头也要加引号。
        writer.Reset();
        ENCODING_CHECK(writer.Write({"\xe3\x80\x80x"}) == nullptr);
        ENCODING_CHECK(writer.Str() == "\"\xe3\x80\x80x\"\n");
    }
    {
        e::CsvWriter writer;
        writer.SetUseCRLF(true);
        ENCODING_CHECK(writer.UseCRLF());
        ENCODING_CHECK(writer.Write({"a", "b"}) == nullptr);
        ENCODING_CHECK(writer.Str() == "a,b\r\n");
        // CRLF 输出的记录能被 Reader 正确解析。
        const auto records = ReadAll(writer.Str());
        ENCODING_REQUIRE(records.size() == 1);
        CheckRecord(records[0], {"a", "b"}, "crlf round-trip");
    }
    {
        e::CsvWriter writer(';');
        ENCODING_CHECK(writer.Comma() == ';');
        ENCODING_CHECK(writer.Write({"a,b", "c"}) == nullptr);
        ENCODING_CHECK(writer.Str() == "a,b;c\n");
        ENCODING_CHECK(!writer.SetComma('"'));
        ENCODING_CHECK(!writer.SetComma('\n'));
        ENCODING_CHECK(!writer.SetComma('\0'));
        ENCODING_CHECK(writer.Comma() == ';');
        ENCODING_CHECK(writer.SetComma(' '));
        writer.Reset();
        ENCODING_CHECK(writer.Write({"a b", "c"}) == nullptr);
        ENCODING_CHECK(writer.Str() == "\"a b\" c\n");
    }
    {
        // 非法分隔符在构造时拒绝。
        bool threw = false;
        try {
            e::CsvWriter invalid('\n');
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        ENCODING_CHECK(threw);
        threw = false;
        try {
            e::CsvReader invalid("a,b", '"');
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        ENCODING_CHECK(threw);
    }

    go2cpp_encoding_test::Announce("csv: writer -> reader round-trip");

    {
        const std::vector<std::vector<std::string>> rows = {
            {"name", "note", "value"},
            {"alice", "likes, commas", "1"},
            {"bob", "says \"hi\"", "2"},
            {"carol", "line1\nline2", "3"},
            {"dave", " padded ", "4"},
            {"", "", "5"},
        };
        e::CsvWriter writer;
        for (const auto& row : rows) {
            ENCODING_CHECK(writer.Write(row) == nullptr);
        }
        const std::string text = writer.Str();
        // 逗号 / 引号 / 换行字段都必须被引号包裹。
        ENCODING_CHECK(text.find("\"likes, commas\"") != std::string::npos);
        ENCODING_CHECK(text.find("\"says \"\"hi\"\"\"") != std::string::npos);
        ENCODING_CHECK(text.find("\"line1\nline2\"") != std::string::npos);
        ENCODING_CHECK(text.find("\" padded \"") != std::string::npos);

        const auto parsed = ReadAll(text);
        ENCODING_REQUIRE(parsed.size() == rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            CheckRecord(parsed[i], rows[i], "round-trip row");
        }

        // Release 取走内容并清空。
        std::string released = writer.Release();
        ENCODING_CHECK(released == text);
        ENCODING_CHECK(writer.Str().empty());
    }
}
