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

/**
 * @brief `Inflater` 的 pimpl 状态：zlib inflate 流 + 固定输入缓冲。
 *
 * ## 定位
 * `detail::Inflater` 的私有实现，被 `ReaderEngine` 通过 `unique_ptr` 间接
 * 持有；`Flate::Reader`/`Zlib::Reader`/`Gzip::Reader` 只传 windowBits，
 * 三个格式共用这一份实现。
 *
 * ## 状态机
 * 只有两态：`Ready`（`initialized == true`）与 `Failed`（`inflateInit2` 或
 * `Reset` 失败）。成员边界/EOF/解压中的状态由 `ReaderEngine` 管理，见
 * `docs/state-machines/ReaderEngine.md`。
 *
 * ## 不变量
 * - `input.size() == kInputBufferSize`；
 * - `window_bits` 构造后不变，`Reset`/`ResetForNextMember` 复用它；
 * - `avail_in <= input.size()`；`avail_in != 0` 时不得 `CommitInput`。
 */
struct Inflater::Impl {
  z_stream stream{};              ///< zlib inflate 状态；构造/Reset/Inflate 读写，析构 inflateEnd
  std::vector<std::uint8_t> input; ///< 固定 32 KiB 输入缓冲；构造分配，CommitInput 挂给 zlib
  int window_bits{0};             ///< -15 raw / 15 zlib / 31 gzip；构造设置，Reset* 复用
  bool initialized{false};        ///< inflateInit2/Reset 是否成功；失败后禁止调用 zlib
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

bool Inflater::Reset() noexcept {
  if (!ok()) {
    return false;
  }
  // 复位后必须清掉旧输入指针/计数：继续指向 input 缓冲的旧位置会让下一次
  // CommitInput 看到 avail_in != 0 而静默丢弃新数据。
  if (inflateReset2(&m_impl->stream, m_impl->window_bits) != Z_OK) {
    m_impl->initialized = false;
    return false;
  }
  m_impl->stream.next_in = Z_NULL;
  m_impl->stream.avail_in = 0;
  m_impl->stream.next_out = Z_NULL;
  m_impl->stream.avail_out = 0;
  return true;
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
