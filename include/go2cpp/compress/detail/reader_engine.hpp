/**
 * @file reader_engine.hpp
 * @brief 解压侧的实现细节：`Inflater`（zlib inflate 的 RAII 封装）与
 *        `ReaderEngine<ReaderT>`（三个格式共用的拉取式解压循环）。
 *
 * @details **为什么要有这一层**：`Flate` / `Zlib` / `Gzip` 三个 Reader 的差别
 *   只有 windowBits 与「是否支持多成员」两个参数，解压循环（向源要数据、
 *   处理部分输入、处理流结束、把 zlib 错误码翻译成项目错误）完全一样。
 *   把循环写三遍是 bug 的温床，因此抽成模板 `ReaderEngine`，三个公开类只是
 *   三行转发。
 *
 *   **输入模型**：拉取式。`ReaderEngine` 自己在 `Inflater` 内部维持 32 KiB
 *   输入缓冲，需要时才向 `ReaderT` 要下一块；调用方给的输出缓冲直接交给
 *   `inflate`，中间没有额外拷贝。因此「1 MiB 输入」实际只发生
 *   `1 MiB / 32 KiB = 32` 次底层 `Read`。
 *
 *   **`ReaderT` 契约**（与 `bufio` 的底层 Reader 完全一致）：
 *   @code
 *   streams::ReadResult Read(char* buffer, std::size_t size);
 *   @endcode
 *   允许 `count > 0` 同时带错误（Go `io.Reader` 契约）：数据先交付，
 *   错误在该块耗尽后的下一次读取时报告。`count == 0 && error == nil`
 *   连续 100 次 → `streams::IoErrNoProgress()`，避免挂死。
 *
 *   **zlib 常量不外泄**：公开头文件不 include `<zlib.h>`，`InflateStatus` /
 *   `DeflateStatus` 是本模块自己的枚举。好处有两点：一是 `zlib.h` 会把
 *   `Byte` / `uInt` / `Bytef` 等 typedef 塞进全局命名空间，容易与使用者冲突；
 *   二是「换实现」（例如以后接 zlib-ng 或 libdeflate）只改 `src/compress/`。
 */
#pragma once

#include "go2cpp/compress/byte_span.hpp"
#include "go2cpp/compress/detail/constants.hpp"
#include "go2cpp/compress/errors.hpp"
#include "go2cpp/streams.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace go2cpp::compress::detail {

/**
 * @brief 一次 `inflate` 调用的结果分类。
 * @details 只暴露调用方需要的三种状态；zlib 的 `Z_OK` / `Z_BUF_ERROR` 合并为
 *   `kOk`——两者对上层都是「还需要更多输入/输出空间」，由上层看
 *   `input_available()` 与产出字节数决定下一步。
 */
enum class InflateStatus : std::uint8_t {
  kOk,        ///< 正常推进；可能产出 0 字节（例如只解析了部分头）。
  kMemberEnd, ///< 当前 deflate 流 / 成员结束（gzip 多成员时可能还有下一个）。
  kError,     ///< 数据错误，具体错误已写入 out 参数。
};

/**
 * @brief zlib `inflate` 状态的 RAII 封装（非模板，实现在 .cpp）。
 *
 * @warning 非线程安全：每个 `Reader` 各持一个实例。
 */
class Inflater {
 public:
  /**
   * @brief 按 windowBits 初始化 inflate 状态。
   * @param window_bits `-15` raw deflate、`15` zlib、`31` gzip（见 constants.hpp）。
   */
  explicit Inflater(int window_bits);

  ~Inflater();

  Inflater(const Inflater&) = delete;
  Inflater& operator=(const Inflater&) = delete;

  /// `inflateInit2` 是否成功。
  bool ok() const noexcept;

  /// 输入缓冲的起始地址（有效容量见 `input_capacity()`）。
  std::uint8_t* input_buffer() noexcept;

  /// 输入缓冲容量（字节）。
  std::size_t input_capacity() const noexcept;

  /// 当前还**未被 zlib 消费**的输入字节数。
  std::size_t input_available() const noexcept;

  /**
   * @brief 把「刚读进输入缓冲的 `bytes` 字节」交给 zlib。
   * @param bytes 本次新读入的字节数（会被钳制到容量内）。
   * @note 前提：`input_available() == 0`。缓冲里还有未消费字节时调用会被忽略，
   *       因为此时 `next_in` 指向缓冲中部，无法把新数据接在正确位置。
   */
  void CommitInput(std::size_t bytes) noexcept;

  /**
   * @brief 解压一批数据到 `out`。
   * @param[out] out 目标缓冲。
   * @param out_size 目标缓冲可用字节数。
   * @param[out] produced 本次实际产出的字节数。
   * @param[out] error 失败时的错误（成功/结束时置空）。
   * @return 状态分类；`kError` 时 `*error` 非空。
   */
  InflateStatus Inflate(std::uint8_t* out, std::size_t out_size,
                        std::size_t* produced, ErrorPtr* error) noexcept;

  /**
   * @brief 结束当前成员，准备解析下一个成员（gzip 多成员用）。
   * @details 保留未消费的输入字节（`next_in` / `avail_in` 在 `inflateReset2`
   *   前后手动保存与恢复，不依赖 zlib 文档里「是否保留」的措辞）。
   */
  void ResetForNextMember() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

/**
 * @brief 三个格式共用的解压循环。
 * @tparam ReaderT 底层 Reader 类型（需满足文件头的 `Read(char*, size_t)` 契约）。
 *
 * @warning `ReaderEngine` 只**引用** `ReaderT`，不拥有它；底层对象生命周期
 *          必须覆盖本对象。
 */
template <typename ReaderT>
class ReaderEngine {
 public:
  /**
   * @brief 绑定数据源并初始化 inflate。
   * @param source 底层 Reader，按引用保存。
   * @param window_bits `-15` / `15` / `31`（见 constants.hpp）。
   * @param multistream 是否把连续的多个压缩成员当作一个逻辑流（gzip 用）。
   */
  ReaderEngine(ReaderT& source, int window_bits, bool multistream)
      : m_source(&source),
        m_inflater(window_bits),
        m_multistream(multistream) {
    if (!m_inflater.ok()) {
      m_error = ErrInternal();
    }
  }

  ReaderEngine(const ReaderEngine&) = delete;
  ReaderEngine& operator=(const ReaderEngine&) = delete;

  /**
   * @brief 解压到 `destination`。
   * @param destination 目标缓冲；为空时直接返回 0（不触发任何底层读）。
   * @return 实际写入的字节数。`0` 时需要看 `Err()`：`streams::IoEof()` 表示
   *         正常结束，其他错误表示失败，空错误表示「暂时没有数据」——本实现
   *         只在极少数情况下返回「0 且无错误」，调用方按 `io.Reader` 惯例重试即可。
   * @note 一次调用可能填满整个 `destination`，也可能只返回一部分：底层数据
   *       不足时提前返回，不会空转等待。
   */
  std::size_t Read(ByteSpan destination) {
    if (m_error) {
      return 0;
    }
    if (destination.empty()) {
      return 0;
    }
    std::size_t total = 0;
    while (total < destination.size()) {
      if (m_stream_end) {
        // 流已完整结束：把 EOF 留给「没有数据可交」的那次调用，避免
        // 同时返回数据 + EOF（Go 的 Reader 也允许，但分开更不容易用错）。
        if (total == 0) {
          m_error = streams::IoEof();
        }
        break;
      }
      if (m_inflater.input_available() == 0) {
        if (!Refill(false)) {
          break;  // Refill 已写 m_error 或置 m_stream_end
        }
        continue;
      }
      std::size_t produced = 0;
      ErrorPtr error;
      const std::size_t available_before = m_inflater.input_available();
      const InflateStatus status = m_inflater.Inflate(
          destination.data() + total, destination.size() - total, &produced,
          &error);
      total += produced;
      if (status == InflateStatus::kError) {
        m_error = error ? error : ErrCorrupt();
        break;
      }
      if (status == InflateStatus::kMemberEnd) {
        if (!m_multistream) {
          m_stream_end = true;
          continue;
        }
        if (m_inflater.input_available() > 0) {
          // 拼接的成员就在当前缓冲里：直接开启下一个。
          m_inflater.ResetForNextMember();
          continue;
        }
        // 当前成员结束、缓冲已空：必须再向源要一次数据，才能区分
        // 「这是最后一个成员」和「后面还跟着一个成员」。
        if (!Refill(true)) {
          break;
        }
        m_inflater.ResetForNextMember();
        continue;
      }
      // kOk 但没有消费输入也没有产出：zlib 不再前进（例如非法块被容错吞掉），
      // 继续循环会死等，直接判损坏。
      if (produced == 0 && m_inflater.input_available() == available_before) {
        m_error = ErrCorrupt();
        break;
      }
    }
    return total;
  }

  /// 粘性错误；正常结束时为 `streams::IoEof()`。
  const ErrorPtr& Err() const noexcept { return m_error; }

  /// 切换多成员模式（仅 `Gzip::Reader` 公开）。
  void SetMultistream(bool enabled) noexcept { m_multistream = enabled; }

  /// 当前是否处于多成员模式。
  bool Multistream() const noexcept { return m_multistream; }

 private:
  /**
   * @brief 向底层 Reader 要一块输入。
   * @param after_member_end 当前是否处于「刚读完一个成员」的边界。
   * @return 拿到至少 1 字节返回 true；否则 false（`m_error` 或 `m_stream_end` 已定）。
   * @details 两种边界对「源已耗尽」的解释不同：流没结束就耗尽 → 数据被截断
   *   （`ErrUnexpectedEof`）；成员刚结束就耗尽 → 正常 EOF。
   */
  bool Refill(bool after_member_end) {
    if (m_deferred_error) {
      m_error = m_deferred_error;
      return false;
    }
    if (m_source_eof) {
      FinishOnSourceEof(after_member_end);
      return false;
    }
    int idle = 0;
    for (;;) {
      const streams::ReadResult result = m_source->Read(
          reinterpret_cast<char*>(m_inflater.input_buffer()),
          m_inflater.input_capacity());
      if (result.count > 0) {
        m_inflater.CommitInput(result.count);
        if (result.error) {
          if (go2cpp::Is(result.error, streams::IoEof())) {
            m_source_eof = true;
          } else {
            // 「数据 + 错误」：先交付数据，等下次需要输入时再报错。
            m_deferred_error = result.error;
          }
        }
        return true;
      }
      if (result.error) {
        if (go2cpp::Is(result.error, streams::IoEof())) {
          m_source_eof = true;
        } else {
          m_error = result.error;
          return false;
        }
      } else if (++idle >= kMaxIdleTransfers) {
        m_error = streams::IoErrNoProgress();
        return false;
      }
      if (m_source_eof) {
        FinishOnSourceEof(after_member_end);
        return false;
      }
    }
  }

  /// 源已耗尽时决定是「正常结束」还是「截断错误」。
  void FinishOnSourceEof(bool after_member_end) {
    if (after_member_end) {
      m_stream_end = true;
      return;
    }
    m_error = ErrUnexpectedEof();
  }

  ReaderT* m_source;
  Inflater m_inflater;
  bool m_multistream;
  bool m_source_eof{false};
  bool m_stream_end{false};
  ErrorPtr m_deferred_error;
  ErrorPtr m_error;
};

/**
 * @brief 把 `ReaderEngine` 适配成 `streams::ReadResult`（供 `streams::Reader` /
 *        `bufio::Reader` 组合使用）。
 */
template <typename EngineT>
inline streams::ReadResult ReadAsStream(EngineT& engine, char* buffer,
                                        std::size_t size) {
  const std::size_t count =
      engine.Read(ByteSpan(reinterpret_cast<std::uint8_t*>(buffer), size));
  if (const ErrorPtr& error = engine.Err()) {
    return streams::ReadResult{count, error};
  }
  return streams::ReadResult{count, {}};
}

}  // namespace go2cpp::compress::detail
