/**
 * @file inflater.cpp
 * @brief `detail::Inflater` 的实现：zlib `inflate` 状态的 RAII 封装。
 *
 * @details 这里承担三件容易出错的事，集中在一处处理好过在三个格式里各写一遍：
 *
 *   1. **错误码翻译**：`Z_DATA_ERROR` 是 zlib 对「数据非法」「校验和不匹配」
 *      「头非法」的统一出口，具体原因只在 `strm.msg` 的文本里。本文件按
 *      zlib 1.2.x~1.3 的固定措辞做映射（`incorrect data check` → 校验和、
 *      `incorrect header check` → 头、其余 → 损坏）。措辞来自 zlib 源码
 *      `inflate.c` / `inflateback`，不是猜的；若未来 zlib 改文案，最坏退化是
 *      把校验和错误报成 `ErrCorrupt`，**不会**漏报错误。
 *   2. **多成员重置**：gzip 的 `Z_STREAM_END` 之后调用 `inflateReset2`，
 *      手动保存/恢复 `next_in` / `avail_in`，不依赖「inflateReset 是否保留
 *      输入指针」的文档措辞（不同版本实现细节不同，实测 zlib 1.3 保留，
 *      但显式保存没有成本）。
 *   3. **uInt 截断防护**：`avail_out` 是 32 位，超过 `kMaxChunkSize` 的
 *      请求会被截断成小块，循环由上层继续。
 */
#include "go2cpp/compress/detail/reader_engine.hpp"

#include "go2cpp/compress/detail/constants.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace go2cpp::compress::detail {

namespace {

/**
 * @brief 把 zlib `Z_DATA_ERROR` 细分成「校验和 / 头 / 数据损坏」。
 * @param stream 出错的 z_stream（`msg` 可空）。
 */
ErrorPtr MapDataError(const z_stream& stream) {
  const std::string message = stream.msg ? stream.msg : "";
  if (message.find("incorrect data check") != std::string::npos ||
      message.find("incorrect length check") != std::string::npos) {
    // adler32 不匹配（zlib）、crc32 或 ISIZE 不匹配（gzip）。
    return ErrChecksum();
  }
  if (message.find("incorrect header check") != std::string::npos ||
      message.find("unknown compression method") != std::string::npos ||
      message.find("invalid window size") != std::string::npos ||
      message.find("unknown header flags set") != std::string::npos ||
      message.find("header crc mismatch") != std::string::npos) {
    return ErrHeader();
  }
  return ErrCorrupt();
}

}  // namespace

struct Inflater::Impl {
  z_stream stream{};
  std::vector<std::uint8_t> input;
  int window_bits{0};
  bool initialized{false};
};

Inflater::Inflater(int window_bits) : m_impl(new Impl()) {
  m_impl->input.resize(kInputBufferSize);
  m_impl->window_bits = window_bits;
  m_impl->stream.zalloc = Z_NULL;
  m_impl->stream.zfree = Z_NULL;
  m_impl->stream.opaque = Z_NULL;
  m_impl->stream.next_in = Z_NULL;
  m_impl->stream.avail_in = 0;
  m_impl->initialized = inflateInit2(&m_impl->stream, window_bits) == Z_OK;
}

Inflater::~Inflater() {
  if (m_impl && m_impl->initialized) {
    inflateEnd(&m_impl->stream);
  }
}

bool Inflater::ok() const noexcept {
  return m_impl && m_impl->initialized;
}

std::uint8_t* Inflater::input_buffer() noexcept {
  return m_impl ? m_impl->input.data() : nullptr;
}

std::size_t Inflater::input_capacity() const noexcept {
  return m_impl ? m_impl->input.size() : 0;
}

std::size_t Inflater::input_available() const noexcept {
  return (m_impl && m_impl->initialized) ? m_impl->stream.avail_in : 0;
}

void Inflater::CommitInput(std::size_t bytes) noexcept {
  if (!ok() || m_impl->stream.avail_in != 0) {
    // 缓冲里还有未消费字节时不能拼接新数据：next_in 指向缓冲中部。
    // 这是引擎的内部前提，违反说明引擎有 bug，直接忽略而不是写坏数据。
    return;
  }
  const std::size_t limited =
      std::min(bytes, input_capacity());
  m_impl->stream.next_in =
      reinterpret_cast<Bytef*>(m_impl->input.data());
  m_impl->stream.avail_in = static_cast<uInt>(limited);
}

InflateStatus Inflater::Inflate(std::uint8_t* out, std::size_t out_size,
                                std::size_t* produced,
                                ErrorPtr* error) noexcept {
  if (produced) {
    *produced = 0;
  }
  if (error) {
    *error = {};
  }
  if (!ok() || out == nullptr || out_size == 0) {
    if (error) {
      *error = ErrInternal();
    }
    return InflateStatus::kError;
  }
  const std::size_t chunk = std::min(out_size, kMaxChunkSize);
  m_impl->stream.next_out = reinterpret_cast<Bytef*>(out);
  m_impl->stream.avail_out = static_cast<uInt>(chunk);
  const int result = inflate(&m_impl->stream, Z_NO_FLUSH);
  if (produced) {
    *produced = chunk - m_impl->stream.avail_out;
  }
  switch (result) {
    case Z_OK:
    case Z_BUF_ERROR:
      // Z_BUF_ERROR 只表示「这次没有进展」，不是错误：可能是输入耗尽，
      // 也可能是输出空间不足。上层会通过 input_available() 与产出量判断。
      return InflateStatus::kOk;
    case Z_STREAM_END:
      return InflateStatus::kMemberEnd;
    case Z_NEED_DICT:
      if (error) {
        *error = ErrDictionary();
      }
      return InflateStatus::kError;
    case Z_DATA_ERROR:
      if (error) {
        *error = MapDataError(m_impl->stream);
      }
      return InflateStatus::kError;
    case Z_MEM_ERROR:
    default:
      if (error) {
        *error = ErrInternal();
      }
      return InflateStatus::kError;
  }
}

void Inflater::ResetForNextMember() noexcept {
  if (!ok()) {
    return;
  }
  Bytef* const next_in = m_impl->stream.next_in;
  const uInt avail_in = m_impl->stream.avail_in;
  if (inflateReset2(&m_impl->stream, m_impl->window_bits) != Z_OK) {
    m_impl->initialized = false;
    return;
  }
  m_impl->stream.next_in = next_in;
  m_impl->stream.avail_in = avail_in;
}

}  // namespace go2cpp::compress::detail
