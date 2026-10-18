/**
 * @file errors.hpp
 * @brief `compress` 模块的哨兵错误（对应 Go 的 compress 系列包里的错误值）。
 *
 * @details **为什么放在 `.cpp` 里定义**：与 `bufio`、`streams` 一样，哨兵错误
 *   靠**指针身份**比较（`Is(err, compress::ErrChecksum())`）。头文件内联会让
 *   每个翻译单元各自构造一个对象，跨单元比较静默失效；因此实现放在
 *   `src/compress/errors.cpp`，所有使用方链接同一个 `go2cpp_compress`。
 *
 *   **为什么故意泄漏**：与项目其他哨兵错误（`streams::IoEof()`、
 *   `bufio::ErrTooLong()`）一致，用 `new` 后不析构，避免静态析构顺序把
 *   退出阶段的身份比较搞乱。
 *
 *   **EOF 不在这里**：解压到流末尾用 `streams::IoEof()`，与项目其余 Reader
 *   契约一致；否则「读到末尾」会在模块边界上变成两种身份。
 *
 *   错误文本沿用 Go 标准库 / zlib 的措辞，便于对照与检索：
 *
 *   | 本模块 | Go 对应物 | 触发条件 |
 *   | --- | --- | --- |
 *   | `ErrCorrupt` | `flate.CorruptInputError` | deflate 数据非法（非法块类型、距离越界…） |
 *   | `ErrChecksum` | `gzip.ErrChecksum` / `zlib.ErrChecksum` | adler32 / crc32 / ISIZE 不匹配 |
 *   | `ErrHeader` | `gzip.ErrHeader` / `zlib.ErrHeader` | 头非法（magic 错、方法错、FHCRC 错…） |
 *   | `ErrUnexpectedEof` | `io.ErrUnexpectedEOF` | 流在完整结束前输入耗尽 |
 *   | `ErrInvalidLevel` | （Go 无，级别非法时 panic） | 传入了 `[-1,9]` 之外的级别 |
 *   | `ErrClosed` | `flate.ErrClosed` / `gzip.ErrClosed` | `Close()` 之后继续写，或重复 `Close()` |
 *   | `ErrDictionary` | `zlib.ErrDictionary` | zlib 流要求预置字典（本模块不支持） |
 *   | `ErrInternal` | （Go 无） | zlib 初始化失败、内存不足等内部错误 |
 *
 * ## 模块定位与调用方
 *
 * 本文件是 `compress` 的错误基础设施层：`Flate` / `Zlib` / `Gzip` 三个公开类
 * 不各自造错误，而是从下面这些实现点转发出来，排查时按调用点跳转即可：
 *
 * - `detail::Inflater::Inflate`：把 zlib 返回码翻译成 `ErrDictionary`
 *   （`Z_NEED_DICT`）与 `ErrInternal`（`Z_MEM_ERROR` / 参数非法）；
 * - `detail::Inflater` 内部的 `MapDataError`：把 `Z_DATA_ERROR` 细分为
 *   `ErrChecksum` / `ErrHeader` / `ErrCorrupt`，是模块里唯一读 `strm.msg`
 *   文本的地方（详见 `src/compress/inflater.cpp` 文件头）；
 * - `detail::ReaderEngine::FinishOnSourceEof`：流未结束但源已尽 →
 *   `ErrUnexpectedEof`；`detail::ReaderEngine::Refill` 的空转上限 →
 *   `streams::IoErrNoProgress()`（不属于本文件）；
 * - `detail::WriterEngine` 的构造与 `Reset`（`ErrInvalidLevel`）、
 *   `Write`/`Flush`/`Close`（`ErrClosed`）、sink 失败或无进展
 *   （`ErrInternal`）。
 *
 * ## 阅读建议
 * 先看上面的表格确认「该用哪个哨兵」，需要定位来源时再按「调用方」跳到实现点。
 * `errors.cpp` 里为什么是八份独立 static、为什么故意泄漏，见本文件开头。
 */
#pragma once

#include "go2cpp/error.hpp"

namespace go2cpp::compress {

/// deflate 数据本身非法（对应 `flate.CorruptInputError`）。
ErrorPtr ErrCorrupt();

/// 校验和不匹配（zlib 的 adler32、gzip 的 crc32 / ISIZE）。
ErrorPtr ErrChecksum();

/// 压缩流头部非法（magic / CM / FLG / FHCRC 错误）。
ErrorPtr ErrHeader();

/// 输入在压缩流完整结束之前就耗尽了。
ErrorPtr ErrUnexpectedEof();

/// 压缩级别不在 `[-1, 9]` 内。
ErrorPtr ErrInvalidLevel();

/// Writer 已经 `Close()`，不允许再写/再关闭。
ErrorPtr ErrClosed();

/// zlib 流声明了预置字典（FDICT）；本模块不提供字典接口。
ErrorPtr ErrDictionary();

/// zlib 内部错误（初始化失败、内存不足等），正常情况下不应出现。
ErrorPtr ErrInternal();

}  // namespace go2cpp::compress
