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
 *
 * ## 模块定位
 *
 * `bufio` 的错误身份层，是整个模块唯一的错误来源。`reader.hpp` 在 `ReadLine`/
 * `ReadSlice`/`UnreadByte`/`UnreadRune` 中返回这里的产品，`scanner.hpp` 在
 * `Scan`/`ReadMore` 中用它终止扫描，调用方则用 `go2cpp::Is(err, bufio::ErrXxx())`
 * 做身份判断。
 *
 * ## 本文件定义什么
 *
 * 7 个哨兵错误工厂函数（定义在 `src/bufio/bufio.cpp`）：`ErrBufferFull`、
 * `ErrTooLong`、`ErrNegativeCount`、`ErrInvalidAdvance`、`ErrInvalidUnreadByte`、
 * `ErrInvalidUnreadRune`、`ErrFinalToken`。EOF 不在其中，复用 `streams::IoEof()`。
 *
 * ## 阅读建议
 *
 * 只需记住两件事：返回的指针**进程内唯一**（不要自己 `new` 同类错误）；
 * 判断一律用 `go2cpp::Is`，不要比较错误文本。
 *
 * ## 边界与陷阱
 *
 * 这些函数故意 `new` 后不析构（见 `@details`），因此指针在首次调用后恒定；
 * 不要在静态初始化阶段把它们当作「尚未创建」的东西使用。
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

/// 对应 `bufio.ErrInvalidUnreadByte`：最近一次操作不是 `ReadByte`，`UnreadByte` 非法。
ErrorPtr ErrInvalidUnreadByte();

/// 对应 `bufio.ErrInvalidUnreadRune`：最近一次操作不是 `ReadRune`，`UnreadRune` 非法。
ErrorPtr ErrInvalidUnreadRune();

/**
 * @brief 对应 `bufio.ErrFinalToken`（Go 1.20+）。
 * @details `SplitFunc` 在最后一个 token 上返回它：`Scan()` 仍交付该 token，
 *          下一次调用返回 false 且 `Err()` 为**空**（不是错误）。
 */
ErrorPtr ErrFinalToken();

}  // namespace go2cpp::bufio
