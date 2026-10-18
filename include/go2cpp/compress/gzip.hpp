/**
 * @file gzip.hpp
 * @brief `compress::Gzip` —— gzip 格式（RFC 1952：10 字节头 + DEFLATE + crc32/ISIZE 尾）。
 *
 * @details 对应 Go 的 `compress/gzip`。DEFLATE 同样委托系统 zlib，
 *   `windowBits = 15 + 16 = 31` 让 zlib 负责写入/校验 gzip 封装：
 *
 *   - 头 10 字节：magic `1f 8b`、CM=8、FLG=0（不写文件名/注释/额外字段）、
 *     MTIME=0（**不写入时间戳**，保证同样输入产出同样字节，测试可复现）、
 *     XFL（级别提示：级别 9 → 2，级别 1 → 4，其余 0）、OS；
 *   - 尾 8 字节：CRC32（未压缩数据）与 ISIZE（未压缩长度 mod 2^32），
 *     均为**小端序**。
 *
 *   zlib 会在解压时校验 CRC32 与 ISIZE，因此篡改尾部会得到 `ErrChecksum()`，
 *   篡改头部会得到 `ErrHeader()`。
 *
 *   ## 多成员（concatenated gzip）
 *
 *   gzip 允许把多个**完整 gzip 流**首尾相接（`cat a.gz b.gz > ab.gz`，
 *   `gzip -d ab.gz` 得到两段内容的拼接）。`Gzip::Reader` **默认开启**多成员
 *   解析（对齐 Go 的 `Reader.Multistream(true)` 默认值）：
 *
 *   - 一个成员 `Z_STREAM_END` 后，如果输入缓冲还有字节（或还能从源读到字节），
 *     就 `inflateReset2` 并继续解析下一个成员的头；
 *   - 源正好在某成员结束处耗尽 → 正常 `streams::IoEof()`；
 *   - 成员之间若有**非 gzip 数据**（例如补零），会得到 `ErrHeader()`。
 *
 *   需要「只解第一个成员」时调用 `Multistream(false)`，此时到达第一个成员
 *   末尾即报 EOF，剩余字节留在底层 Reader 中。
 *
 *   ## 与 Go 的差异
 *   1. **不支持自定义头字段**（`gzip.Header{Name, Comment, ModTime, OS}`）：
 *      本模块不写这些字段，也不解析后暴露给调用方。需要「读文件名」这类
 *      元信息的场景可以后续加 `Header()`，但那会让 `Reader` 增加一个
 *      与解压无关的状态对象，目前没有需求。
 *      （C 类：支持它需要放弃 `windowBits=31` 让 zlib 生成头部，改为自己写
 *      10 字节头 + raw deflate + 自己算 crc32/ISIZE，约 100 行；收益是
 *      「可复现的 MTIME/OS/文件名」，当前不需要。）
 *   2. `Resetter` 已支持：`Reader::Reset(source)` / `Writer::Reset(destination)`。
 *   3. 只支持单个 gzip 头部的 10 字节基础字段；不解析 FHCRC/FEXTRA。
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
 * @brief gzip 格式的 Reader / Writer 工厂类。
 */
class Gzip {
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
   * @brief gzip 解压器（默认支持多成员）。
   * @tparam ReaderT 底层 Reader（`streams::ReadResult Read(char*, size_t)`）。
   *
   * @warning 只引用 `source`，不拥有它。
   */
  template <typename ReaderT>
  class Reader {
   public:
    /// 包装数据源并开启多成员解析；首次 `Read` 时解析 10 字节头。
    explicit Reader(ReaderT& source)
        : m_engine(source, detail::kGzipWindowBits, true) {}

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// 解压到 `destination`；多个成员的内容按顺序连续交付。
    std::size_t Read(ByteSpan destination) { return m_engine.Read(destination); }

    /// 与 `streams::Reader` 契约互通。
    streams::ReadResult Read(char* buffer, std::size_t size) {
      return detail::ReadAsStream(m_engine, buffer, size);
    }

    /**
     * @brief 开关多成员解析。
     * @param enabled true 为默认行为（解析全部拼接成员）；
     *        false 表示只解第一个成员，到其末尾即 EOF。
     * @note 建议在首次 `Read` 之前调用；读到一半再切换的行为是「从下一个
     *       成员边界起生效」，不推荐依赖。
     */
    void Multistream(bool enabled) noexcept {
      m_engine.SetMultistream(enabled);
    }

    /// 当前是否开启多成员解析。
    bool Multistream() const noexcept { return m_engine.Multistream(); }

    /// 粘性错误；正常读完时为 `streams::IoEof()`。
    const ErrorPtr& Err() const noexcept { return m_engine.Err(); }

    /**
     * @brief 对应 Go `Resetter.Reset`：复用 zlib 状态并改绑到新 gzip 源。
     * @param source 新数据源；生命周期必须覆盖后续读取。
     * @return 成功为空错误。
     * @note 保留当前的 `Multistream()` 设置。
     */
    ErrorPtr Reset(ReaderT& source) {
      m_engine.Reset(source);
      return m_engine.Err();
    }

   private:
    detail::ReaderEngine<ReaderT> m_engine;
  };

  /**
   * @brief gzip 压缩器（不写文件名/注释/时间戳，输出可复现）。
   * @tparam WriterT 底层 Writer。
   *
   * @note 析构不自动 `Close()`：忘记 `Close()` 会得到**没有 crc32 尾**的
   *       半截 gzip 文件，解压方报 `ErrUnexpectedEof`。
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
                    int level = ::go2cpp::compress::Gzip::DefaultCompression)
        : m_engine(destination, level, detail::kGzipWindowBits) {}

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// 压缩 `source`；返回被接受的字节数。
    std::size_t Write(ByteSpan source) { return m_engine.Write(source); }

    /// 与 `streams::Writer` 契约互通。
    streams::WriteResult Write(const char* data, std::size_t size) {
      return detail::WriteAsStream(m_engine, data, size);
    }

    /// 冲刷到 deflate 块边界（流保持打开，可用于流式写）。
    ErrorPtr Flush() { return m_engine.Flush(); }

    /// 结束流（写出 crc32 + ISIZE）。必须调用。
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
    detail::WriterEngine<WriterT> m_engine;
  };
};

}  // namespace go2cpp::compress
