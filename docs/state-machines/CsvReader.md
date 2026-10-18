# CsvReader 状态机

- 源文件：`src/encoding/csv.cpp`（类定义在 `include/go2cpp/encoding/csv.hpp`）
- 关键函数：`CsvReader::Read()`（所有解析状态都在它内部推进）
- 配置函数：`SetFieldsPerRecord()`、`SetComment()`、`SetTrimLeadingSpace()` 只改配置，不改解析状态

## 解析状态

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Created | `Read` | RecordStart | 跳过空行与注释行，定位记录起点 |
| Created | `Read` | Eof | 输入为空或只有空行/注释 |
| RecordStart | `Read` | UnquotedField | 首字符不是 `"` |
| RecordStart | `Read` | QuotedField | 首字符是 `"`，进入引号字段 |
| RecordStart | `Read` | Eof | 已到达文本末尾 |
| UnquotedField | `Read` | AfterField | 遇到 `,`、`\n`、`\r\n` 或 EOF |
| UnquotedField | `Read` | Failed | 字段内出现裸 `"`（`CsvErrBareQuote`） |
| QuotedField | `Read` | QuoteSeen | 遇到 `"`，需要判断是转义还是闭合 |
| QuoteSeen | `Read` | QuotedField | 下一个字符仍是 `"`，写入一个字面 `"` |
| QuoteSeen | `Read` | AfterField | 闭合引号，检查后续只允许分隔符/换行/EOF |
| AfterField | `Read` | RecordStart | 记录结束，做 `FieldsPerRecord` 校验后返回 |
| AfterField | `Read` | UnquotedField | 遇到 `,`，同一记录的下一个字段 |
| AfterField | `Read` | QuotedField | 逗号后以 `"` 开头（引号字段） |
| AfterField | `Read` | Failed | 闭合引号后出现其它字符（`CsvErrQuote`） |
| QuotedField | `Read` | Failed | 文本耗尽仍未闭合（`CsvErrQuote`） |
| RecordStart/AfterField | `Read` | Failed | 字段数与 `FieldsPerRecord` 不符（`CsvErrFieldCount`） |
| Failed | `Read` | Failed | 直接返回同一个 `m_error`，不再解析 |

> `Eof` 表示为 `eof == true` 且 `error == nullptr`；`Failed` 表示为
> `m_error` 非空，后续 `Read()` 都是粘性失败。所有状态转化都发生在
> `Read()` 内部，外部无法单独触发 `QuotedField` 等中间态。

## 不变量

- `0 <= m_pos <= m_data.size()`，`m_line >= 1`；
- `m_error` 非空后不再推进 `m_pos`；
- 成功记录至少一个字段；空行与注释行不会产生记录；
- 引号字段内的 `\r\n` 已转成 `\n`；孤立 `\r` 原样保留；
- `m_fields_per_record` 只可能是 `0`、`-1` 或正整数。

## 非法转化

- Failed -> RecordStart：没有 `Reset()`，只能重新构造 `CsvReader`。
- QuoteSeen -> RecordStart：闭合引号后若还有非分隔符内容，直接 `CsvErrQuote`，不会把该内容当作新记录。
- EOF -> 继续解析：`Read()` 在 `eof` 后可以继续调用，但始终返回 `eof == true`（不会重新扫描）。
