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

struct Deflater::Impl {
  z_stream stream{};
  std::vector<std::uint8_t> output;
  std::size_t produced{0};
  bool initialized{false};
};

Deflater::Deflater(int level, int window_bits, int mem_level)
    : m_impl(new Impl()) {
  m_impl->output.resize(kOutputBufferSize);
  m_impl->stream.zalloc = Z_NULL;
  m_impl->stream.zfree = Z_NULL;
  m_impl->stream.opaque = Z_NULL;
  m_impl->stream.next_in = Z_NULL;
  m_impl->stream.avail_in = 0;
  // Z_DEFAULT_STRATEGY 是 Go flate 与 zlib 的默认策略；本模块不暴露策略参数。
  m_impl->initialized =
      deflateInit2(&m_impl->stream, level, Z_DEFLATED, window_bits, mem_level,
                   Z_DEFAULT_STRATEGY) == Z_OK;
}

Deflater::~Deflater() {
  if (m_impl && m_impl->initialized) {
    deflateEnd(&m_impl->stream);
  }
}

bool Deflater::ok() const noexcept {
  return m_impl && m_impl->initialized;
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
