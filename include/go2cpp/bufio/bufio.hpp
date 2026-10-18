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
