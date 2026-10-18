/**
 * @file writer_engine.hpp
 * @brief 压缩侧的实现细节：`Deflater`（zlib deflate 的 RAII 封装）与
 *        `WriterEngine<WriterT>`（三个格式共用的推入式压缩循环）。
 *
 * @details **为什么要有这一层**：`Flate` / `Zlib` / `Gzip` 的 Writer 只有
 *   windowBits 不同；`Write` / `Flush` / `Close` 的状态机完全一样：
 *
 *   - `Write`：`Z_NO_FLUSH` 推进，把 deflate 产出的块写到 sink；
 *   - `Flush`：`Z_SYNC_FLUSH`，让解码端能读到**当前已完成的数据**，流本身
 *     继续保持打开（这是流式压缩的语义，不是「写完了」）；
 *   - `Close`：`Z_FINISH` 直到 `Z_STREAM_END`，写出尾部（zlib adler32 /
 *     gzip crc32+ISIZE），然后 `deflateEnd`。
 *
 *   **只要忘记调用 `Close()`，输出就是半截流**——gzip/zlib 缺尾部校验和，
 *   解压方会得到 `ErrUnexpectedEof`。这与 Go 的 `flate.Writer` / `gzip.Writer`
 *   完全一致，也与本项目 `bufio::Writer` 析构不自动 flush 的风格一致：
 *   「数据何时离开进程」必须显式可审计，析构函数不替调用方做决定。
 *
 *   **错误是粘性的**：sink 写失败、级别非法、Close 后继续写，都会记录错误，
 *   后续调用原样返回它，不会「部分成功假装没事」。
 *
 *   **短写处理**：sink 的 `Write` 允许只写一部分（`count < size`）；本类循环
 *   写直到写完。`count == 0` 且无错误连续 100 次 → `streams::IoErrShortWrite()`。
 *
 * ## 本文件定义什么
 * - `FlushMode` / `DeflateStatus`：把 zlib 的 flush 参数与返回码收窄成本模块
 *   自己的枚举，公开头文件因此不必 include `<zlib.h>`；
 * - `Deflater`：zlib deflate 状态的 RAII 封装（输出缓冲、uInt 截断防护、
 *   `HuffmanOnly` 到 `Z_HUFFMAN_ONLY` 的映射）；
 * - `WriterEngine<WriterT>`：三个格式共用的推入式压缩循环 + 状态机；
 * - `WriteAsStream`：把引擎适配成 `streams::WriteResult` 的薄适配器。
 *
 * ## 阅读建议
 * 先看 `WriterEngine` 的状态机注释与 `docs/state-machines/WriterEngine.md`，
 * 再按 `Write` → `Flush` → `Close` 的顺序读三个泵；`Drain` 是所有写失败的
 * 汇合点（短写、谎报、空转都在那里处理）。
 */
#pragma once

#include "go2cpp/compress/byte_span.hpp"
#include "go2cpp/compress/detail/constants.hpp"
#include "go2cpp/compress/errors.hpp"
#include "go2cpp/compress/level.hpp"
#include "go2cpp/streams.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace go2cpp::compress::detail {

/// deflate 的冲刷方式（对应 zlib 的 `Z_NO_FLUSH` / `Z_SYNC_FLUSH` / `Z_FINISH`）。
enum class FlushMode : std::uint8_t {
  kNone,   ///< 正常推进，不保证下游能解出数据。
  kSync,   ///< 把当前数据推到解码端可解的位置（写一个空存储块），流不结束。
  kFinish, ///< 结束流，写出尾部校验和。
};

/// 一次 `deflate` 调用的结果分类。
enum class DeflateStatus : std::uint8_t {
  kOk,        ///< 正常；可能需要再次调用（输出缓冲写满了）。
  kStreamEnd, ///< `Z_FINISH` 完成，流已完整结束。
  kError,     ///< 内部错误（正常情况下不应出现）。
};

/**
 * @brief zlib `deflate` 状态的 RAII 封装（非模板，实现在 .cpp）。
 *
 * ## 定位
 * `detail` 层对 zlib deflate 的唯一接触面：`WriterEngine<WriterT>` 通过
 * `unique_ptr` 持有它，并按 `FlushMode` 驱动；本类只负责把输入挂到
 * `next_in`、调用 `deflate`、收集 `next_out` 的产出。三个格式的差异
 * （windowBits）在构造时传入，循环完全一致。
 *
 * ## 生命周期与线程模型
 * 由 `WriterEngine` 在构造时创建，随引擎析构调用 `deflateEnd`；`Reset`
 * 复用同一个 zlib 流。非线程安全：一个实例只能被持有它的 Writer 线程使用。
 *
 * @warning 非线程安全：每个 `Writer` 各持一个实例。
 */
class Deflater {
 public:
  /**
   * @brief 按级别与 windowBits 初始化 deflate 状态。
   * @param level `[-1, 9]`（调用方应先校验）。
   * @param window_bits `-15` raw deflate、`15` zlib、`31` gzip。
   * @param mem_level zlib memLevel（本模块固定 8，见 constants.hpp）。
   */
  Deflater(int level, int window_bits, int mem_level);

  ~Deflater();

  Deflater(const Deflater&) = delete;
  Deflater& operator=(const Deflater&) = delete;

  /// `deflateInit2` 是否成功。
  bool ok() const noexcept;

  /// 对应 Go `Resetter`：把 deflate 状态复位到「新流开始」。
  /// @return 成功 true；失败时 `ok()` 变为 false。
  bool Reset() noexcept;

  /// 输出缓冲起始地址（容量见 `output_capacity()`）。
  std::uint8_t* output() noexcept;

  /// 输出缓冲容量（字节）。
  std::size_t output_capacity() const noexcept;

  /// 最近一次 `Run()` 产出的字节数（每次 `Run` 开始时归零）。
  std::size_t output_size() const noexcept;

  /**
   * @brief 设置本次要压缩的输入。
   * @param data 输入数据（不拷贝；调用方保证在 `Run` 期间有效）。
   * @param size 字节数，会被钳制到 `kMaxChunkSize`。
   */
  void SetInput(const std::uint8_t* data, std::size_t size) noexcept;

  /// 还没有被 zlib 消费的输入字节数。
  std::size_t InputRemaining() const noexcept;

  /**
   * @brief 执行一次 `deflate`。
   * @param mode 冲刷方式。
   * @return 状态分类；产出字节见 `output_size()`。
   */
  DeflateStatus Run(FlushMode mode) noexcept;

  /// zlib 的错误文本（`strm.msg`）；没有时为空串。
  std::string Message() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

/**
 * @brief 三个格式共用的压缩循环。
 * @tparam WriterT 底层 Writer 类型（需满足 `streams::WriteResult Write(const char*, size_t)`）。
 *
 * ## 定位
 * 三个公开 Writer（`Flate::Writer` / `Zlib::Writer` / `Gzip::Writer`）的共同
 * 实现：它们只负责传 `destination`、level、windowBits，之后 `Write` /
 * `Flush` / `Close` / `Err` / `Reset` 全部由本类完成。上层通过
 * `WriteAsStream` 接到 `streams::WriteString` / `bufio::Writer` 时也是转发到
 * 本类。
 *
 * ## 生命周期与线程模型
 * 由公开 Writer 以成员方式持有，引用 `WriterT` 但不拥有它；不共享、不加锁。
 * 构造时创建 `Deflater`（失败则记 `ErrInternal`），`Reset` 复用或重建它。
 *
 * ## 状态机
 * 有 Open / Writing / Flushing / Finishing / Closed / Failed 六个状态，状态
 * 转化表见 `docs/state-machines/WriterEngine.md`。`Write`、`Flush`、`Close`
 * 各自是一个泵，`m_error` 与 `m_closed` 是状态载体。
 *
 * ## 不变量
 * - `m_error` 一旦非空即粘性：所有入口先返回它，不再推进 zlib；
 * - `m_closed == true` 后 `Write` / `Flush` 返回 `ErrClosed()`；
 * - `m_deflater` 只在构造失败时为 null（此时 `m_error` 非空）；
 * - `Drain()` 成功返回后，`output_size()` 那批字节已全部交给 sink。
 *
 * @warning `WriterEngine` 只**引用** `WriterT`，不拥有它。
 */
template <typename WriterT>
class WriterEngine {
 public:
  /**
   * @brief 绑定目标并初始化 deflate。
   * @param destination 底层 Writer，按引用保存。
   * @param level 压缩级别；非法时不会崩溃，而是把 `ErrInvalidLevel()` 记为粘性错误。
   * @param window_bits `-15` / `15` / `31`。
   */
  WriterEngine(WriterT& destination, int level, int window_bits)
      : m_destination(&destination),
        m_level(level),
        m_window_bits(window_bits) {
    if (!IsValidLevel(level)) {
      m_error = ErrInvalidLevel();
      return;
    }
    m_deflater.reset(new Deflater(level, window_bits, kMemoryLevel));
    if (!m_deflater->ok()) {
      m_error = ErrInternal();
    }
  }

  WriterEngine(const WriterEngine&) = delete;
  WriterEngine& operator=(const WriterEngine&) = delete;

  /**
   * @brief 压缩 `source`。
   * @param source 待压缩数据。
   * @return 被接受的输入字节数。成功时等于 `source.size()`；失败时是出错前
   *         已消费的字节数（`Err()` 说明原因）。
   * @note 返回值不代表「已到达 sink」——deflate 可能把它们留在内部窗口里，
   *       只有 `Flush()` / `Close()` 成功才保证下游能读到。
   *
   * ## 步骤
   * step1. 粘性错误 / 已 Close 的入口检查（后者把 `ErrClosed` 记为粘性）；
   * step2. 按 `kMaxChunkSize` 切片，避免 `avail_in` 的 `uInt` 截断；
   * step3. 对每片反复 `Run(kNone)`，直到这一片的输入被 zlib 消费完；
   * step4. 每轮 `Drain()` 把已产出的字节写出去；写失败立即记录并返回
   *        「出错前已接受的字节数」；
   * step5. 输入没减少且没有产出 → zlib 不再前进，报 `ErrInternal` 而不是
   *        死循环（级别 0/stored 可能只产出不消费，所以要同时看两者）。
   */
  std::size_t Write(ByteSpan source) {
    // step1: 粘性错误优先；Close 后再写不能静默成功。
    if (m_error) {
      return 0;
    }
    if (m_closed) {
      m_error = ErrClosed();
      return 0;
    }
    std::size_t accepted = 0;
    while (accepted < source.size()) {
      const std::size_t chunk =
          std::min(source.size() - accepted, kMaxChunkSize);
      // step2: 单片不超过 uInt 上限，剩余部分下一轮再喂。
      m_deflater->SetInput(source.data() + accepted, chunk);
      // step3: 内层泵：zlib 可能一次只吃掉一部分输入。
      while (m_deflater->InputRemaining() > 0) {
        const std::size_t before = m_deflater->InputRemaining();
        const DeflateStatus status = m_deflater->Run(FlushMode::kNone);
        // step5: 「进展」有两种——消费输入或产出输出；两者都没有才是卡死。
        // 注意「进展」有两种：消费了输入，或产出了输出。级别 0（stored）
        // 在输出缓冲写满时可能**不消费新输入**只吐数据，若只检查输入就会
        // 把它误判成卡死。
        const std::size_t produced = m_deflater->output_size();
        // step4: 先 Drain 再判状态：zlib 产出的字节可能已经写失败。
        const ErrorPtr write_error = Drain();
        if (write_error) {
          m_error = write_error;
          return accepted + (chunk - m_deflater->InputRemaining());
        }
        if (status == DeflateStatus::kError ||
            (m_deflater->InputRemaining() == before && produced == 0)) {
          // deflate 不再消费输入也不再产出：要么内部错误，要么 zlib 状态异常。
          // 不重试、不死循环，直接报错。
          m_error = ErrInternal();
          return accepted + (chunk - m_deflater->InputRemaining());
        }
      }
      accepted += chunk;
    }
    return accepted;
  }

  /**
   * @brief 把当前已完成的数据推给 sink（`Z_SYNC_FLUSH`）。
   * @return 成功为空错误；失败返回粘性错误。
   * @note 之后仍然可以继续 `Write`——这是与 `Close()` 的关键区别。
   *
   * ## 步骤
   * step1. 粘性错误 / Closed / deflater 缺失的入口检查；
   * step2. 循环 `Run(kSync)` + `Drain()`，直到某次产出小于输出缓冲容量
   *        （说明 zlib 已把所有待冲刷数据交出）；写失败或内部错误立即返回。
   */
  ErrorPtr Flush() {
    // step1: 已关闭的 Writer 不允许再冲刷。
    if (m_error) {
      return m_error;
    }
    if (m_closed) {
      return ErrClosed();
    }
    if (!m_deflater) {
      return ErrInternal();
    }
    // step2: 反复 Z_SYNC_FLUSH，直到输出缓冲没被写满。
    for (;;) {
      const DeflateStatus status = m_deflater->Run(FlushMode::kSync);
      const std::size_t produced = m_deflater->output_size();
      const ErrorPtr write_error = Drain();
      if (write_error) {
        m_error = write_error;
        return m_error;
      }
      if (status == DeflateStatus::kError) {
        m_error = ErrInternal();
        return m_error;
      }
      // 输出缓冲没写满 ⇒ zlib 已经把所有待冲刷数据交出来了。
      if (produced < m_deflater->output_capacity()) {
        return {};
      }
    }
  }

  /**
   * @brief 结束压缩流：写出尾部校验和并释放 zlib 状态。
   * @return 成功为空错误；失败返回错误（已写入的部分不回退）。
   * @note 重复调用返回 `ErrClosed()`；`Close()` 之后的 `Write` / `Flush`
   *       同样返回 `ErrClosed()`。
   *
   * ## 步骤
   * step1. 入口检查（粘性错误优先，其次 Closed）；
   * step2. 循环 `Run(kFinish)` + `Drain()`：只有 `kStreamEnd` 算成功；写失败 /
   *        内部错误 / 无产出都置 `m_closed` 后返回错误（已写出的字节不回退）。
   */
  ErrorPtr Close() {
    // step1: 重复 Close 与「带错误 Close」都在这里收敛。
    if (m_error) {
      return m_error;
    }
    if (m_closed) {
      return ErrClosed();
    }
    if (!m_deflater) {
      return ErrInternal();
    }
    // step2: Z_FINISH 在输出缓冲不足时会返回 kOk，必须循环到 kStreamEnd。
    for (;;) {
      const DeflateStatus status = m_deflater->Run(FlushMode::kFinish);
      const std::size_t produced = m_deflater->output_size();
      const ErrorPtr write_error = Drain();
      if (write_error) {
        m_error = write_error;
        m_closed = true;
        return m_error;
      }
      if (status == DeflateStatus::kStreamEnd) {
        m_closed = true;
        return {};
      }
      if (status == DeflateStatus::kError) {
        m_error = ErrInternal();
        m_closed = true;
        return m_error;
      }
      if (produced == 0) {
        // Z_FINISH 既没产出也没结束：zlib 拒绝前进，报错而不是死循环。
        m_error = ErrInternal();
        m_closed = true;
        return m_error;
      }
    }
  }

  /// 粘性错误；一切正常时为空。
  const ErrorPtr& Err() const noexcept { return m_error; }

  /// 是否已经成功或尝试过 `Close()`。
  bool Closed() const noexcept { return m_closed; }

  /**
   * @brief 对应 Go `Resetter.Reset`：复用 deflate 状态并改绑到新 sink。
   * @details 不替旧 sink 补 `Close()`（与 Go 一致，见公开 Writer 的文档）。
   *          参数非法或 zlib 复位失败时返回错误并把错误记为粘性。
   */
  ErrorPtr Reset(WriterT& destination) {
    m_destination = &destination;
    m_closed = false;
    m_error = {};
    if (!IsValidLevel(m_level)) {
      m_error = ErrInvalidLevel();
      return m_error;
    }
    if (!m_deflater) {
      m_deflater.reset(new Deflater(m_level, m_window_bits, kMemoryLevel));
      if (!m_deflater->ok()) {
        m_error = ErrInternal();
      }
      return m_error;
    }
    if (!m_deflater->Reset()) {
      m_error = ErrInternal();
    }
    return m_error;
  }

 private:
  /**
   * @brief 把 `deflate` 输出缓冲里的字节全部写到 sink。
   *
   * ## 步骤
   * step1. 本次没有产出：直接成功（`Write`/`Flush`/`Close` 的常规路径）；
   * step2. 循环写剩余字节；`count > 0` 推进指针并清零空转计数；
   * step3. sink 报 `count > remaining`（谎报多写）按短写失败处理，不信任它；
   * step4. 连续 `kMaxIdleTransfers` 次 `count == 0` 且无错误 → `IoErrShortWrite`。
   *
   * @note 返回错误时可能有部分字节已经写出，调用方不能假设「要么全写要么全没写」。
   */
  ErrorPtr Drain() {
    // step1: deflate 本次没产出时无需触碰 sink。
    const std::size_t produced = m_deflater->output_size();
    if (produced == 0) {
      return {};
    }
    const std::uint8_t* data = m_deflater->output();
    std::size_t remaining = produced;
    int idle = 0;
    // step2: 短写是常态（管道/缓冲写），循环到写完为止。
    while (remaining > 0) {
      const streams::WriteResult result = m_destination->Write(
          reinterpret_cast<const char*>(data), remaining);
      if (result.error) {
        return result.error;
      }
      // step3: sink 谎报写入量说明其实现有问题，按短写失败处理。
      if (result.count > remaining) {
        // sink 谎报「写得比给的多」：不信任它，按短写处理。
        return streams::IoErrShortWrite();
      }
      if (result.count > 0) {
        idle = 0;
        data += result.count;
        remaining -= result.count;
      // step4: 连续空写且无错误，说明对端没有进展。
      } else if (++idle >= kMaxIdleTransfers) {
        return streams::IoErrShortWrite();
      }
    }
    return {};
  }

  WriterT* m_destination;
  int m_level;
  int m_window_bits;
  std::unique_ptr<Deflater> m_deflater;
  bool m_closed{false};
  ErrorPtr m_error;
};

/**
 * @brief 把 `WriterEngine` 适配成 `streams::WriteResult`（供 `streams::Writer` /
 *        `bufio::Writer` 组合使用）。
 */
template <typename EngineT>
inline streams::WriteResult WriteAsStream(EngineT& engine, const char* data,
                                          std::size_t size) {
  const std::size_t accepted = engine.Write(
      ByteSpan(reinterpret_cast<std::uint8_t*>(const_cast<char*>(data)), size));
  if (const ErrorPtr& error = engine.Err()) {
    return streams::WriteResult{accepted, error};
  }
  return streams::WriteResult{accepted, {}};
}

}  // namespace go2cpp::compress::detail
