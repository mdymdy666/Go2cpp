/**
 * @file flate.hpp
 * @brief `compress::Flate` —— raw DEFLATE（RFC 1951，**无头无尾**）。
 *
 * @details 对应 Go 的 `compress/flate`。DEFLATE 由本模块自研：
 *
 *   - 压缩：LZ77 滑动窗口 + 固定 Huffman（`src/compress/deflate_writer.hpp`）；
 *   - 解压：stored / fixed / dynamic Huffman（`deflate_reader.hpp`）。
 *
 *   `windowBits = -15` 仍表示「raw deflate」：不写也不解析 zlib 头（2 字节）
 *   与 adler32 尾（4 字节）。它只是构造参数，不再传给 zlib。
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
 *   2. Go 的 `flate.Reader` 是 `io.ReadCloser`；本类的 DEFLATE 状态在析构时
 *      释放，没有 `Close()` 方法（解压不需要「结束」动作）。
 *   3. 支持 `Resetter`（`Reader::Reset` / `Writer::Reset`）：复用 DEFLATE 状态，
 *      但**不**替上一个 writer 调用 `Close()`（与 Go 一致，半截流责任自负）。
 *
 *   ## 已知未支持项及原因（C 类，有意不做）
 *
 *   | 未支持项 | 原因 |
 *   |---|---|
 *   | 预置字典（Go `flate.NewReaderDict` / `NewWriterDict`） | 需要额外传字典并在解析到 FDICT 时装载；当前没有调用场景，且会让 API 出现两个构造重载。遇到 FDICT 时直接报 `ErrDictionary()`。 |
 *   | `Writer.Flush` 的 `Z_FULL_FLUSH` / 字节对齐细节 | Go 的 `Flush` 只保证「当前数据可解」，本模块写一个空存储块再对齐到字节边界；没有「重置历史窗口」的公开 API。 |
 *   | 自定义 `dict` 与硬件加速 | 纯 C++ 实现，不做 SIMD/硬件加速；热点是哈希链匹配与位读写。 |
 *
 * ## 阅读建议
 * 先看 `Flate::Writer` 的 `Write` / `Flush` / `Close` 生命周期（最容易踩坑的是
 * 忘记 `Close()`，会得到半截流），再看 `Reader`；想弄清「为什么三个格式共用
 * 一个引擎」时进 `detail/reader_engine.hpp` 与 `detail/writer_engine.hpp`。
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
 *
 * ## 定位
 * `compress` 公开门面的第一层：调用方通过它拿到 raw deflate 的 `Reader` /
 * `Writer`，实际解压/压缩分别委托给 `detail::ReaderEngine<ReaderT>` 与
 * `detail::WriterEngine<WriterT>`（构造参数 `-15` 表示 raw deflate）。
 * 与 `Zlib` / `Gzip` 并列，三者只在 windowBits 上不同。
 *
 * ## 生命周期与线程模型
 * 无状态、无成员，嵌套类可在任意线程构造；线程安全性由每个 `Reader` /
 * `Writer` 实例自己决定（实例之间不共享任何状态）。
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
  /// 只做 Huffman 编码（= `compress::HuffmanOnly`，数值 -2）。
  static constexpr int HuffmanOnly = ::go2cpp::compress::HuffmanOnly;

  /**
   * @brief raw DEFLATE 解压器。
   * @tparam ReaderT 底层 Reader（`streams::ReadResult Read(char*, size_t)`）。
   *
   * ## 定位
   * 供需要 raw deflate 的调用方（`tests/compress` 的往返测试、以及要自己包
   * 一层容器格式的代码）读取压缩数据。本类只做转发：把 `source` 交给
   * `detail::ReaderEngine<ReaderT>`，`Read` / `Err` / `Reset` 全部委托过去，
   * 真正的拉取循环在 `reader_engine.hpp`。
   *
   * ## 生命周期与线程模型
   * 不拥有 `source`（调用方保证其生命周期覆盖读取），引擎是成员、随本对象
   * 析构释放。非线程安全：与 `bufio::Reader` 一样，一个实例同一时刻只应被
   * 一个线程使用。
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

    /**
     * @brief 对应 Go `Resetter.Reset`：复用当前 DEFLATE 状态，改绑到新数据源。
     * @param source 新的底层 Reader（生命周期必须覆盖后续读取）。
     * @return 成功为空错误；只有 DEFLATE 状态复位失败才会返回 `ErrInternal()`。
     * @note 会清空上一次流的错误、EOF 标志与多成员状态，然后从新流的第一个
     *       字节开始解析（gzip 仍按当前 `Multistream()` 设置处理）。
     */
    ErrorPtr Reset(ReaderT& source) {
      m_engine.Reset(source);
      return m_engine.Err();
    }

   private:
    detail::ReaderEngine<ReaderT> m_engine;  ///< 解压循环 + 粘性错误；`Read`/`Reset`/`Err` 都转发给它，随本对象析构，非共享
  };

  /**
   * @brief raw DEFLATE 压缩器。
   * @tparam WriterT 底层 Writer（`streams::WriteResult Write(const char*, size_t)`）。
   *
   * @note `Writer` 析构**不会**自动 `Close()`：与 `bufio::Writer` 不自动
   *       `Flush()` 同理，`Close()` 失败必须能被调用方看到。
   *
   * ## 定位
   * 供需要产出 raw deflate 的调用方使用；压缩状态、冲刷与短写重试全部在
   * `detail::WriterEngine<WriterT>` 里，本类只暴露 Go 风格的
   * `Write` / `Flush` / `Close`。
   *
   * ## 生命周期与线程模型
   * 构造即初始化 DEFLATE（非法级别变成粘性错误，不抛异常）；必须显式 `Close()`
   * 才能得到完整流。非线程安全：每个实例独占一个 `WriterEngine`。
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

    /**
     * @brief 对应 Go `Resetter.Reset`：复用 DEFLATE 状态，改绑到新输出并开始新流。
     * @param destination 新底层 Writer；生命周期必须覆盖后续写入。
     * @return 成功为空错误（非法级别 / DEFLATE 复位失败时返回对应错误）。
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
