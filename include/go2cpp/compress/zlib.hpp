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
 *      （C 类：需要时用 zlib 的 `inflateSetDictionary`/`deflateSetDictionary` 补齐。）
 *   2. `Resetter` 已支持：`Reader::Reset(source)` / `Writer::Reset(destination)`
 *      复用 zlib 状态；`Writer.Flush` 语义与 Go 相同（`Z_SYNC_FLUSH`）。
 *
 * ## 阅读建议
 * 先看 `Reader` 的「单流」语义（到 `Z_STREAM_END` 即结束，尾后多余字节忽略），
 * 再对比 `gzip.hpp` 的多成员；`Writer` 侧与 `Flate::Writer` 完全同构，只是
 * windowBits 为 15、尾部多一个 adler32。错误码翻译在 `inflater.cpp` 文件头。
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
 *
 * ## 定位
 * `compress` 公开门面的 zlib 分支：向调用方提供 `Zlib::Reader` /
 * `Zlib::Writer`，二者分别委托给 `detail::ReaderEngine<ReaderT>` 与
 * `detail::WriterEngine<WriterT>`（构造参数 `15`）。与 `Flate` / `Gzip` 并列；
 * 它是三者中「有校验和、但只支持单流」的折中选择。
 *
 * ## 生命周期与线程模型
 * 工厂本身无状态；线程安全性由每个 `Reader` / `Writer` 实例自己决定。
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
  /// 只做 Huffman 编码（= `compress::HuffmanOnly`，数值 -2）。
  static constexpr int HuffmanOnly = ::go2cpp::compress::HuffmanOnly;

  /**
   * @brief zlib 格式解压器。
   * @tparam ReaderT 底层 Reader（`streams::ReadResult Read(char*, size_t)`）。
   *
   * ## 定位
   * 供需要 adler32 校验、且数据只有单个 zlib 流的调用方使用；构造时
   * `multistream=false`，全部工作转给 `detail::ReaderEngine<ReaderT>`。
   * 流结束后的剩余字节不会被读取（留在底层 Reader 里）。
   *
   * ## 状态机
   * 「读头 / 解 body / 读尾 / 结束」同样由 `ReaderEngine` 管理，但单流模式下
   * 命中 `Z_STREAM_END` 就直接进入 EOF，不经过「成员结束」分支；
   * 状态转化表见 `docs/state-machines/ReaderEngine.md`。
   *
   * ## 生命周期与线程模型
   * 只引用 `source`；zlib 状态随对象析构释放。非线程安全。
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

    /// 对应 Go `Resetter.Reset`：复用 zlib 状态并改绑到新数据源。
    ErrorPtr Reset(ReaderT& source) {
      m_engine.Reset(source);
      return m_engine.Err();
    }

   private:
    detail::ReaderEngine<ReaderT> m_engine;  ///< 解压循环 + 粘性错误；`Read`/`Reset`/`Err` 都转发给它，随本对象析构，非共享
  };

  /**
   * @brief zlib 格式压缩器。
   * @tparam WriterT 底层 Writer。
   *
   * @note 与 `Flate::Writer` 一样，析构不自动 `Close()`。
   *
   * ## 定位
   * 供需要产出 zlib 流的调用方使用；头 2 字节与 adler32 尾由 zlib 按
   * windowBits=15 生成，本类只把 `Write`/`Flush`/`Close` 转给
   * `detail::WriterEngine<WriterT>`。
   *
   * ## 生命周期与线程模型
   * 构造即初始化 zlib（非法级别变成粘性错误）；必须显式 `Close()` 才会写出
   * adler32 尾。非线程安全：每个实例独占一个 `WriterEngine`。
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

    /**
     * @brief 对应 Go `Resetter.Reset`：复用 zlib 状态，改绑到新输出并开始新流。
     * @param destination 新底层 Writer；生命周期必须覆盖后续写入。
     * @return 成功为空错误（非法级别 / zlib 复位失败时返回对应错误）。
     * @warning 与 Go 相同：**不会**替上一次流调用 `Close()`。上一个 sink 里
     *          未 `Close()` 的数据仍是半截流；需要完整流请先 `Close()` 再 `Reset`。
     */
    ErrorPtr Reset(WriterT& destination) {
      return m_engine.Reset(destination);
    }

   private:
    detail::WriterEngine<WriterT> m_engine;  ///< 压缩循环 + 粘性错误；`Write`/`Flush`/`Close` 都转发给它，随本对象析构，非共享
  };
};

}  // namespace go2cpp::compress
