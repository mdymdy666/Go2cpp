/**
 * @file deflater.cpp
 * @brief `detail::Deflater` 的实现：zlib `deflate` 状态的 RAII 封装。
 *
 * @details 关键点：
 *
 *   1. **输出缓冲固定 64 KiB**：`deflate` 的 `avail_out` 为 0 时会带着剩余
 *      输入返回 `Z_OK`，上层再调用一次即可。固定缓冲让「每次 run 产多少」
 *      可预测，也避免每次分配。
 *   2. **`Z_FINISH` 的分支**：`deflate(Z_FINISH)` 在输出空间不足时返回
 *      `Z_OK` 而不是 `Z_STREAM_END`，必须继续调用直到 `Z_STREAM_END`；
 *      上层 `WriterEngine::Close` 负责这个循环，本文件只如实返回状态。
 *   3. **`Z_BUF_ERROR`**：对 deflate 而言表示「没有进展」（例如 `Z_FINISH`
 *      下输出缓冲已满且无法再产出）。归为 `kOk`，由上层的进展检查决定是否
 *      报错，避免把可恢复情况当致命错误。
 */
#include "go2cpp/compress/detail/writer_engine.hpp"

#include "go2cpp/compress/detail/constants.hpp"

#include <zlib.h>

#include <algorithm>
#include <string>
#include <vector>

namespace go2cpp::compress::detail {

namespace {

/// 把本模块的冲刷方式翻译成 zlib 的 flush 参数。
int ToZlibFlush(FlushMode mode) noexcept {
  switch (mode) {
    case FlushMode::kSync:
      return Z_SYNC_FLUSH;
    case FlushMode::kFinish:
      return Z_FINISH;
    case FlushMode::kNone:
    default:
      return Z_NO_FLUSH;
  }
}

}  // namespace

/**
 * @brief `Deflater` 的 pimpl 状态：zlib deflate 流 + 固定输出缓冲。
 *
 * ## 定位
 * `detail::Deflater` 的私有实现，被 `WriterEngine` 通过 `unique_ptr` 间接
 * 持有；`Flate::Writer`/`Zlib::Writer`/`Gzip::Writer` 只传 windowBits，
 * 三个格式共用这一份实现。
 *
 * ## 状态机
 * 只有两态：`Ready`（`initialized == true`）与 `Failed`（`deflateInit2` 或
 * `Reset` 失败）。`Run`/`SetInput` 不改变它；「开始/冲刷/结束/关闭」的流状态
 * 由 `WriterEngine` 管理，见 `docs/state-machines/WriterEngine.md`。
 *
 * ## 不变量
 * - `output.size() == kOutputBufferSize`；
 * - 任意时刻 `produced <= output.size()`；
 * - `initialized == false` 后不得再调用任何 zlib 函数。
 */
struct Deflater::Impl {
  z_stream stream{};                ///< zlib deflate 状态；构造/Reset/Run 读写，析构 deflateEnd
  std::vector<std::uint8_t> output; ///< 固定 64 KiB 输出缓冲；构造分配，Run 写 [0, produced)
  std::size_t produced{0};          ///< 最近一次 Run 的产出字节数；Run 写，output_size() 读
  bool initialized{false};          ///< deflateInit2/Reset 是否成功；失败后禁止调用 zlib
};

Deflater::Deflater(int level, int window_bits, int mem_level)
    : m_impl(new Impl()) {
  m_impl->output.resize(kOutputBufferSize);
  m_impl->stream.zalloc = Z_NULL;
  m_impl->stream.zfree = Z_NULL;
  m_impl->stream.opaque = Z_NULL;
  m_impl->stream.next_in = Z_NULL;
  m_impl->stream.avail_in = 0;
  // Go 的 HuffmanOnly(-2) 在 zlib 里不是合法 level，而是
  // 「默认级别 + Z_HUFFMAN_ONLY 策略」；其余级别用默认策略。
  const bool huffman_only = level == HuffmanOnly;
  const int zlib_level = huffman_only ? Z_DEFAULT_COMPRESSION : level;
  const int strategy = huffman_only ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY;
  m_impl->initialized =
      deflateInit2(&m_impl->stream, zlib_level, Z_DEFLATED, window_bits,
                   mem_level, strategy) == Z_OK;
}

Deflater::~Deflater() {
  if (m_impl && m_impl->initialized) {
    deflateEnd(&m_impl->stream);
  }
}

bool Deflater::ok() const noexcept {
  return m_impl && m_impl->initialized;
}

bool Deflater::Reset() noexcept {
  if (!ok()) {
    return false;
  }
  if (deflateReset(&m_impl->stream) != Z_OK) {
    m_impl->initialized = false;
    return false;
  }
  m_impl->stream.next_in = Z_NULL;
  m_impl->stream.avail_in = 0;
  m_impl->stream.next_out = Z_NULL;
  m_impl->stream.avail_out = 0;
  m_impl->produced = 0;
  return true;
}

std::uint8_t* Deflater::output() noexcept {
  return m_impl ? m_impl->output.data() : nullptr;
}

std::size_t Deflater::output_capacity() const noexcept {
  return m_impl ? m_impl->output.size() : 0;
}

std::size_t Deflater::output_size() const noexcept {
  return m_impl ? m_impl->produced : 0;
}

void Deflater::SetInput(const std::uint8_t* data, std::size_t size) noexcept {
  if (!ok()) {
    return;
  }
  const std::size_t limited = std::min(size, kMaxChunkSize);
  m_impl->stream.next_in =
      const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
  m_impl->stream.avail_in = static_cast<uInt>(limited);
}

std::size_t Deflater::InputRemaining() const noexcept {
  return ok() ? m_impl->stream.avail_in : 0;
}

DeflateStatus Deflater::Run(FlushMode mode) noexcept {
  if (!ok()) {
    return DeflateStatus::kError;
  }
  m_impl->produced = 0;
  m_impl->stream.next_out =
      reinterpret_cast<Bytef*>(m_impl->output.data());
  m_impl->stream.avail_out = static_cast<uInt>(m_impl->output.size());
  const int result = deflate(&m_impl->stream, ToZlibFlush(mode));
  m_impl->produced = m_impl->output.size() - m_impl->stream.avail_out;
  switch (result) {
    case Z_OK:
    case Z_BUF_ERROR:
      return DeflateStatus::kOk;
    case Z_STREAM_END:
      return DeflateStatus::kStreamEnd;
    default:
      return DeflateStatus::kError;
  }
}

std::string Deflater::Message() const {
  if (!ok() || m_impl->stream.msg == nullptr) {
    return std::string();
  }
  return std::string(m_impl->stream.msg);
}

}  // namespace go2cpp::compress::detail
