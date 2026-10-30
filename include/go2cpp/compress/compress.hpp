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
 *   **DEFLATE 由本模块自研**：编码器在 `src/compress/deflate_writer.hpp`
 *   （LZ77 + 固定 Huffman），解码器在 `src/compress/deflate_reader.hpp`
 *   （stored / fixed / dynamic Huffman）。RFC 1950/1952 的头尾与
 *   adler32/crc32 也在本模块内实现，因此不再依赖系统 zlib。
 *
 *   本模块负责三件事：
 *
 *   1. **格式封装**：raw deflate（RFC 1951）、zlib（RFC 1950）、gzip（RFC 1952）；
 *   2. **流式 IO 适配**：把任意 `ReaderT` / `WriterT`（`streams` 契约）接到
 *      自研编解码器的增量状态机，处理部分读写、短写、EOF、多成员 gzip；
 *   3. **API 形态**：与项目其他模块一致的 `Read(ByteSpan)` / `Write(ByteSpan)`、
 *      粘性 `Err()`、显式 `Flush()` / `Close()`。
 *
 *   格式正确性由**独立实现**（Python `zlib`、GNU `gzip`）的双向互操作证明，
 *   见 `tests/compress/test_interop.cpp`；自己压、自己解只证明内部一致。
 *
 *   ## 依赖
 *   零外部压缩库依赖：链接 `go2cpp::compress` 只需要 `go2cpp::error`，
 *   不再传递 `ZLIB::ZLIB`，也不需要 `find_package(ZLIB)`。
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
