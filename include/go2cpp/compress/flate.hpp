/**
 * @file flate.hpp
 * @brief `compress::Flate` —— raw DEFLATE（RFC 1951，**无头无尾**）。
 *
 * @details 对应 Go 的 `compress/flate`。DEFLATE 本身（Huffman 编码 + LZ77
 *   滑动窗口）**不在本模块实现**，全部委托系统 zlib（`-lz`）：
 *
 *   - 压缩：`deflateInit2(level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY)`；
 *   - 解压：`inflateInit2(-15)`。
 *
 *   `windowBits = -15` 是 zlib 表达「raw deflate」的唯一方式：负号表示
 *   **不写也不解析** zlib 头（2 字节）与 adler32 尾（4 字节）。
 *
 *   @warning raw deflate **没有任何校验和**：被篡改的流可能解出「长度不同但
 *   不报错」的数据，也可能报 `ErrCorrupt`，取决于损坏位置。要检测损坏请用
 *   `Zlib`（adler32）或 `Gzip`（crc32 + ISIZE）。这是格式本身的限制，
 *   不是实现缺陷。
 *
 *   ## 用法
 *   @code
 *   go2cpp::compress::Flate::Writer writer(sink, go2cpp::compress::Flate::BestSpeed);
 *   writer.Write(bytes);
 *   if (writer.Flush()) { ... }            // 可选：让下游先读到已完成部分
 *   if (auto err = writer.Close()) { ... } // 必须调用，否则流不完整
 *
 *   go2cpp::compress::Flate::Reader reader(source);
 *   std::size_t n = reader.Read(buffer);
 *   if (go2cpp::Is(reader.Err(), go2cpp::streams::IoEof())) { ... }
 *   @endcode
 *
 *   ## 与 Go 的差异
 *   1. Go 的 `flate.NewWriter` 返回 `(w, error)`；这里把错误存进 Writer 并在
 *      `Err()` / `Write()` / `Flush()` / `Close()` 返回。构造函数不抛异常，
 *      便于和项目其他模块（`bufio`、`os`）的 `Err()` 风格一致。
 *   2. Go 的 `flate.Reader` 是 `io.ReadCloser`；本类的 zlib 状态在析构时释放，
 *      没有 `Close()` 方法（解压不需要「结束」动作）。
 *   3. 不支持 `Resetter`（复用 zlib 状态跨流）；本模块一个流一个对象，
 *      实现简单且没有共享状态的坑。
 */
#pragma once

#include "go2cpp/compress/byte_span.hpp"
#include "go2cpp/compress/detail/constants.hpp"
#include "go2cpp/compress/detail/reader_engine.hpp"
#include "go2cpp/compress/detail/writer_engine.hpp"
#include "go2cpp/compress/errors.hpp"
#include "go2cpp/compress/level.hpp"
#include "go2cpp/streams.hpp"

#include <cstddef>

namespace go2cpp::compress {

/**
 * @brief raw DEFLATE 格式的 Reader / Writer 工厂类。
 *
 * @details 类本身没有状态，只提供嵌套的 `Reader<ReaderT>` / `Writer<WriterT>`
 *   与级别常量。这样写法与 Go 的 `flate.NewReader` / `flate.NewWriter` 对应，
 *   同时让 `compress::Flate::Reader<Source>` 的读法明确表达「格式 + 传输方向」。
 */
class Flate {
 public:
  /// 不压缩，只打包（= `compress::NoCompression`）。
  static constexpr int NoCompression = ::go2cpp::compress::NoCompression;
  /// 最快速度（= `compress::BestSpeed`）。
  static constexpr int BestSpeed = ::go2cpp::compress::BestSpeed;
  /// 最高压缩率（= `compress::BestCompression`）。
  static constexpr int BestCompression = ::go2cpp::compress::BestCompression;
  /// 默认级别（= `compress::DefaultCompression`）。
  static constexpr int DefaultCompression =
      ::go2cpp::compress::DefaultCompression;

  /**
   * @brief raw DEFLATE 解压器。
   * @tparam ReaderT 底层 Reader（`streams::ReadResult Read(char*, size_t)`）。
   *
   * @warning 只引用 `source`，不拥有它。
   */
  template <typename ReaderT>
  class Reader {
   public:
    /// 包装数据源；不读取任何字节（首次 `Read` 才开始解析）。
    explicit Reader(ReaderT& source)
        : m_engine(source, detail::kRawFlateWindowBits, false) {}

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// 解压到 `destination`；语义见 `detail::ReaderEngine::Read`。
    std::size_t Read(ByteSpan destination) { return m_engine.Read(destination); }

    /// 与 `streams::Reader` 契约互通（供 `bufio::Reader` 等组合）。
    streams::ReadResult Read(char* buffer, std::size_t size) {
      return detail::ReadAsStream(m_engine, buffer, size);
    }

    /// 粘性错误；正常读完时为 `streams::IoEof()`。
    const ErrorPtr& Err() const noexcept { return m_engine.Err(); }

   private:
    detail::ReaderEngine<ReaderT> m_engine;
  };

  /**
   * @brief raw DEFLATE 压缩器。
   * @tparam WriterT 底层 Writer（`streams::WriteResult Write(const char*, size_t)`）。
   *
   * @note `Writer` 析构**不会**自动 `Close()`：与 `bufio::Writer` 不自动
   *       `Flush()` 同理，`Close()` 失败必须能被调用方看到。
   */
  template <typename WriterT>
  class Writer {
   public:
    /**
     * @brief 包装目标并初始化压缩器。
     * @param destination 底层 Writer。
     * @param level 压缩级别；非法值不会崩溃，而是记 `ErrInvalidLevel()`。
     */
    explicit Writer(WriterT& destination,
                    int level = ::go2cpp::compress::Flate::DefaultCompression)
        : m_engine(destination, level, detail::kRawFlateWindowBits) {}

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// 压缩 `source`；返回被接受的字节数（见 `detail::WriterEngine::Write`）。
    std::size_t Write(ByteSpan source) { return m_engine.Write(source); }

    /// 与 `streams::Writer` 契约互通（供 `streams::WriteString` 等复用）。
    streams::WriteResult Write(const char* data, std::size_t size) {
      return detail::WriteAsStream(m_engine, data, size);
    }

    /// 冲刷到 deflate 块边界（流保持打开）。
    ErrorPtr Flush() { return m_engine.Flush(); }

    /// 结束流（写出最终块）。必须调用。
    ErrorPtr Close() { return m_engine.Close(); }

    /// 粘性错误；一切正常时为空。
    const ErrorPtr& Err() const noexcept { return m_engine.Err(); }

    /// 是否已经 `Close()`。
    bool Closed() const noexcept { return m_engine.Closed(); }

   private:
    detail::WriterEngine<WriterT> m_engine;
  };
};

}  // namespace go2cpp::compress
