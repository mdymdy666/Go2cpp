/**
 * @file compress.hpp
 * @brief `compress` 模块入口：flate / zlib / gzip 三种 DEFLATE 封装。
 *
 * @details 对应 Go 的 `compress/flate`、`compress/zlib`、`compress/gzip`。
 *
 *   | Go | 本项目 |
 *   |---|---|
 *   | `flate.NewReader` / `*flate.Reader` | `compress::Flate::Reader<ReaderT>` |
 *   | `flate.NewWriter` / `*flate.Writer` | `compress::Flate::Writer<WriterT>` |
 *   | `zlib.NewReader` / `*zlib.Reader` | `compress::Zlib::Reader<ReaderT>` |
 *   | `zlib.NewWriter` / `*zlib.Writer` | `compress::Zlib::Writer<WriterT>` |
 *   | `gzip.NewReader` / `*gzip.Reader` | `compress::Gzip::Reader<ReaderT>` |
 *   | `gzip.NewWriter` / `*gzip.Writer` | `compress::Gzip::Writer<WriterT>` |
 *   | `flate.DefaultCompression` 等 | `compress::DefaultCompression` 等（每个类也有同名常量） |
 *
 *   **本模块不自研 DEFLATE**。Huffman 编码 + LZ77 滑动窗口是数百行的算法，
 *   自实现几乎必然在「跨块匹配、动态码表、边界距离」上出错，而且自己压、
 *   自己解可以两边同错还能通过测试。这里全部委托系统 zlib（`-lz`），
 *   只负责三件事：
 *
 *   1. **格式选择**：windowBits `-15` / `15` / `31`；
 *   2. **流式 IO 适配**：把任意 `ReaderT` / `WriterT`（`streams` 契约）接到
 *      zlib 的 `next_in` / `next_out` 模型上，处理部分读写、短写、EOF、
 *      多成员 gzip、错误码翻译；
 *   3. **API 形态**：与项目其他模块一致的 `Read(ByteSpan)` / `Write(ByteSpan)`、
 *      粘性 `Err()`、显式 `Flush()` / `Close()`。
 *
 *   因此格式正确性由 zlib 保证，而**互操作**（`gzip -d` 能解、`gzip -c` 能读）
 *   才是本模块需要证明的东西——见 `tests/compress/test_interop.cpp`。
 *
 *   ## 依赖
 *   CMake 用 `find_package(ZLIB REQUIRED)` 解析系统 zlib，链接 `go2cpp::compress`
 *   即可传递 `ZLIB::ZLIB`。若目标平台没有 zlib，configure 会直接失败——
 *   这是有意的：**不提供「没有 zlib 时的空实现」**，否则会静默产出坏数据。
 *
 *   ## 用法
 *   @code
 *   std::string compressed;
 *   go2cpp::compress::StringSink sink;  // 见 tests/compress 的测试桩
 *
 *   go2cpp::compress::Gzip::Writer<go2cpp::compress::StringSink> writer(
 *       sink, go2cpp::compress::Gzip::BestCompression);
 *   writer.Write(go2cpp::compress::AsBytes(payload));
 *   if (auto err = writer.Close()) { ... }
 *   @endcode
 *
 * ## 阅读建议
 * 本文件只是聚合入口，没有实现。建议顺序：
 * 1. 先看 `level.hpp`（级别取值与合法性），选一个级别；
 * 2. 再按所需格式读 `flate.hpp` / `zlib.hpp` / `gzip.hpp` 的 `Writer`/`Reader`；
 * 3. 需要理解短读、短写、多成员、错误粘性时，进 `detail/writer_engine.hpp`
 *    与 `detail/reader_engine.hpp`——三个格式的差异只有 windowBits。
 */
#pragma once

#include "go2cpp/compress/byte_span.hpp"
#include "go2cpp/compress/errors.hpp"
#include "go2cpp/compress/flate.hpp"
#include "go2cpp/compress/gzip.hpp"
#include "go2cpp/compress/level.hpp"
#include "go2cpp/compress/zlib.hpp"
