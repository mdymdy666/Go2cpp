/**
 * @file zlib.hpp
 * @brief `compress::Zlib` —— zlib 格式（RFC 1950：2 字节头 + DEFLATE + adler32 尾）。
 *
 * @details 对应 Go 的 `compress/zlib`。DEFLATE 同样委托系统 zlib，
 *   `windowBits = 15` 让 zlib 负责写入/校验：
 *
 *   - 头 2 字节：`CMF`（0x78，CM=8 + 窗口 32 KiB）与 `FLG`（FLEVEL 由级别决定、
 *     FDICT=0、`(CMF<<8|FLG) % 31 == 0`）；
 *   - 尾 4 字节：未压缩数据的 **adler32**，大端序。
 *
 *   因此 `Zlib` 能检测尾部损坏（`ErrChecksum`），这是 raw `Flate` 做不到的。
 *
 *   ## 与 Go 的差异
 *   1. **不支持预置字典**（`FDICT=1`）：遇到会返回 `ErrDictionary()`。
 *      Go 的 `zlib.NewReaderDict` 需要字典参数，本模块没有这种使用场景，
 *      与其留一个「需要额外状态」的半成品接口，不如明确报错。
 *   2. 没有 `Resetter` / `Writer.Flush` 之外的扩展点。
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
 * @brief zlib 格式的 Reader / Writer 工厂类。
 *
 * @note zlib 是**单流**格式：解压到 `Z_STREAM_END` 即结束，流尾之后若还有
 *   多余字节会被忽略（与 Go `zlib.Reader` 一致）。需要拼接多段请用 `Gzip`。
 */
class Zlib {
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
   * @brief zlib 格式解压器。
   * @tparam ReaderT 底层 Reader（`streams::ReadResult Read(char*, size_t)`）。
   *
   * @warning 只引用 `source`，不拥有它。
   */
  template <typename ReaderT>
  class Reader {
   public:
    /// 包装数据源；首次 `Read` 时解析 2 字节头。
    explicit Reader(ReaderT& source)
        : m_engine(source, detail::kZlibWindowBits, false) {}

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// 解压到 `destination`。
    std::size_t Read(ByteSpan destination) { return m_engine.Read(destination); }

    /// 与 `streams::Reader` 契约互通。
    streams::ReadResult Read(char* buffer, std::size_t size) {
      return detail::ReadAsStream(m_engine, buffer, size);
    }

    /// 粘性错误；正常读完时为 `streams::IoEof()`。
    const ErrorPtr& Err() const noexcept { return m_engine.Err(); }

   private:
    detail::ReaderEngine<ReaderT> m_engine;
  };

  /**
   * @brief zlib 格式压缩器。
   * @tparam WriterT 底层 Writer。
   *
   * @note 与 `Flate::Writer` 一样，析构不自动 `Close()`。
   */
  template <typename WriterT>
  class Writer {
   public:
    /**
     * @brief 包装目标并初始化压缩器。
     * @param destination 底层 Writer。
     * @param level 压缩级别；非法值记 `ErrInvalidLevel()`。
     */
    explicit Writer(WriterT& destination,
                    int level = ::go2cpp::compress::Zlib::DefaultCompression)
        : m_engine(destination, level, detail::kZlibWindowBits) {}

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// 压缩 `source`；返回被接受的字节数。
    std::size_t Write(ByteSpan source) { return m_engine.Write(source); }

    /// 与 `streams::Writer` 契约互通。
    streams::WriteResult Write(const char* data, std::size_t size) {
      return detail::WriteAsStream(m_engine, data, size);
    }

    /// 冲刷到 deflate 块边界（流保持打开）。
    ErrorPtr Flush() { return m_engine.Flush(); }

    /// 结束流（写出 adler32 尾）。必须调用。
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
