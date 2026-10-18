/**
 * @file bufio.hpp
 * @brief `bufio` 模块入口：带缓冲的 IO（对应 Go 的 `bufio` 包）。
 *
 * @details 对外三件套：
 *
 *   | Go | 本项目 |
 *   |---|---|
 *   | `bufio.NewReader` / `*bufio.Reader` | `bufio::Reader<ReaderT>` |
 *   | `bufio.NewWriter` / `*bufio.Writer` | `bufio::Writer<WriterT>` |
 *   | `bufio.NewScanner` / `*bufio.Scanner` | `bufio::Scanner<ReaderT>` |
 *
 *   **核心价值**：把 N 次小 `Read` 合并成 1 次大 `Read`。压测数据见
 *   `tests/bufio/test_bufio_stress.cpp`（1 MiB：裸读 1,048,576 次系统调用
 *   vs 4096 缓冲 256 次）。
 *
 *   典型用法：
 *   @code
 *   go2cpp::bufio::Reader reader(file_source);          // 默认 4096
 *   auto line = reader.ReadLine();
 *   if (!line.ok() && !go2cpp::streams::Is(line.Err(), go2cpp::streams::IoEof())) {
 *       // 处理错误
 *   }
 *   @endcode
 *
 *   已支持（与 Go 同名）：
 *   - `Reader`：`Read`/`ReadByte`/`ReadRune`/`UnreadByte`/`UnreadRune`/
 *     `ReadLine`/`ReadSlice`/`ReadBytes`/`ReadString`/`Peek`/`Discard`/
 *     `Buffered`/`Size`/`Err`；
 *   - `Writer`：`Write`/`WriteByte`/`WriteString`/`ReadFrom`/`Reset`/`Flush`/
 *     `Buffered`/`Available`/`Size`/`Err`；
 *   - `Scanner`：`Scan`/`Text`/`Bytes`/`Err`/`Split`/`SetSplit`/`Buffer`/
 *     `MaxTokenSize`，以及 `ErrFinalToken` 终止语义。
 *
 *   已知未支持项及原因（C 类，有意不做）：
 *   | 未支持项 | 原因 |
 *   |---|---|
 *   | `Peek(n > Size())` 先把源读一截再返回 `ErrBufferFull`（Go 行为） | 需要「部分数据 + 错误」的返回形态；本项目 `Result<ByteSpan>` 已支持带值失败，但**先读掉一截再报错**更难排查，因此保留当前「直接报错」。 |
 *   | `ReadLine` 的 `isPrefix` 两段式（Go 用于处理超长行） | 签名固定为 `Result<std::string>`，没有前缀标志位；任意长行请用无界的 `ReadString('\n')`。 |
 *   | `ScanWords` 的 Unicode 空白 | 本模块没有通用 rune 分类表；按 ASCII 空白切分并在测试里固定行为。 |
 *   | `Scanner.Buffer` 复用调用方提供的缓冲区 | 缓冲由 `std::vector` 独占，复用外部内存需要额外的生命周期契约；只保留 `max` 上限语义。 |
 *   | `Writer.AvailableBuffer`（Go 1.18+） | 需要把内部缓冲直接暴露给调用方并保证「下一次 Write 前后的长度约定」，收益与风险不匹配。 |
 *
 *   设计边界（详见各头文件）：
 *   - `Peek` 零拷贝，`n > Size()` → `ErrBufferFull`；
 *   - `ReadLine` 超长行 → `ErrTooLong`（无 `isPrefix` 两段式）；
 *   - `Writer` 缓冲满自动 flush，**析构不 flush**；
 *   - `Flush` 循环处理部分写（Go 只写一次，短写即报错）；
 *   - `Scanner` 支持自定义 `SplitFunc`，预置 `ScanLines`/`ScanWords`/`ScanBytes`。
 *
 *   **链接**：三个类都是模板（头文件即可实例化），但哨兵错误
 *   （`ErrBufferFull` 等）与三个预置 `SplitFunc` 有唯一定义，位于
 *   `src/bufio/bufio.cpp`。使用方必须链接 `go2cpp::bufio`（CMake 目标
 *   `go2cpp_bufio`）。与 `go2cpp::os` 一样，它**不在** `go2cpp::runtime`
 *   的导出表里，需要显式链接。
 */
#pragma once

#include "go2cpp/bufio/byte_span.hpp"
#include "go2cpp/bufio/errors.hpp"
#include "go2cpp/bufio/limits.hpp"
#include "go2cpp/bufio/reader.hpp"
#include "go2cpp/bufio/result.hpp"
#include "go2cpp/bufio/scanner.hpp"
#include "go2cpp/bufio/writer.hpp"
