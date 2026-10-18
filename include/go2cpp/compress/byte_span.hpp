/**
 * @file byte_span.hpp
 * @brief `compress` 模块使用的非拥有字节视图（复用 `bufio` 的 `ByteSpan`）。
 *
 * @details **为什么复用而不是自己定义一份**：`compress::Reader::Read` 与
 *   `bufio::Reader::Read` / `bufio::Writer::Write` 是同一个形状的接口
 *   （`Read(ByteSpan) -> size_t`、`Write(ByteSpan) -> size_t`）。如果 `compress`
 *   定义自己的 span 类型，两个模块就无法直接互操作：
 *
 *   @code
 *   go2cpp::compress::Gzip::Reader<Source> source(...);
 *   go2cpp::bufio::Reader<decltype(source)> buffered(source);  // 需要同一 ByteSpan
 *   @endcode
 *
 *   （实际上两者通过 `Read(char*, size_t)` 重载也能组合，但共享类型让
 *   `Peek`/`Write` 这类直接传 span 的路径零成本。）
 *
 *   `bufio/byte_span.hpp` 只依赖标准库、没有任何链接期符号，因此该复用
 *   **不引入模块间链接依赖**：`go2cpp_compress` 不需要 `target_link_libraries`
 *   到 `go2cpp_bufio`。
 */
#pragma once

#include "go2cpp/bufio/byte_span.hpp"

namespace go2cpp::compress {

/// Go `[]byte` 的非拥有视图；语义与生命周期约束见 `bufio/byte_span.hpp`。
using ByteSpan = ::go2cpp::bufio::ByteSpan;

/// 由 `char*` 缓冲构造可写视图（与 `bufio::AsBytes` 同一函数）。
using ::go2cpp::bufio::AsBytes;

/// 由 `ByteSpan` 构造只读 `string_view`。
using ::go2cpp::bufio::AsStringView;

/// 把 `ByteSpan` 复制成 `std::string`。
using ::go2cpp::bufio::ToString;

}  // namespace go2cpp::compress
