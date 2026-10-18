/**
 * @file errors.hpp
 * @brief `bufio` 模块的哨兵错误（对应 Go 的 `bufio.ErrBufferFull` 等）。
 *
 * @details **为什么放在 .cpp 里定义**：哨兵错误靠**指针身份**比较
 *   （`streams::Is(err, bufio::ErrTooLong())`）。如果每个翻译单元都各自
 *   内联一份，跨单元比较就会失败。实现放在 `src/bufio/bufio.cpp`，所有
 *   使用方链接同一个 `go2cpp_bufio`，身份才稳定。
 *
 *   **为什么故意泄漏**：与项目 `streams::IoEof()`、`StrconvErrSyntax()` 一致，
 *   用 `new` 后不析构，避免静态析构顺序把退出阶段的比较搞乱。
 *
 *   **EOF 不在这里**：`bufio` 与 `io` 用的是**同一个** EOF 值
 *   （`streams::IoEof()`），否则「读到末尾」会在模块边界上变成两种身份。
 */
#pragma once

#include "go2cpp/error.hpp"

namespace go2cpp::bufio {

/// 对应 `bufio.ErrBufferFull`：请求的字节数超过缓冲容量（`Peek` 触发）。
ErrorPtr ErrBufferFull();

/// 对应 `bufio.ErrTooLong`：单行/单 token 超过缓冲上限（`ReadLine`、`Scanner` 触发）。
ErrorPtr ErrTooLong();

/// 对应 `bufio.ErrNegativeCount`：传入了负的计数（本实现按无符号计数，保留以对齐 Go）。
ErrorPtr ErrNegativeCount();

/**
 * @brief 自定义 `SplitFunc` 违反了契约（advance/token 越界）。
 * @details Go 用 `panic` 表达同类违约；C++ 侧转成可检查的错误，避免进程崩溃。
 */
ErrorPtr ErrInvalidAdvance();

}  // namespace go2cpp::bufio
