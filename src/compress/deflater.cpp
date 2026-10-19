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

/**
 * @brief 驱动一次 zlib 的 `deflate()`，把输入压缩到内部输出缓冲。
 * @param mode 刷新模式（`None` / `Sync` / `Full` / `Finish`），决定 zlib 是否
 *        强制输出当前累积的数据。
 * @return `kOk`（还有更多输出可产）/ `kStreamEnd`（流已结束）/ `kError`。
 *        每次调用后通过 `output()` / `output_size()` 取本次产出的字节。
 *
 * ## 定位
 *
 * 压缩侧的**唯一引擎调用点**：上层 `Compressor` 的循环反复调它，直到
 * `kStreamEnd`（或输入耗尽）。解压侧有对称的 `Inflater::Run`。
 *
 * ## 步骤
 *
 * step1. `ok()` 检查：zlib 流未初始化成功时直接返回 `kError`；
 * step2. 重置输出游标：`next_out` 指向内部缓冲起点、`avail_out` 设为缓冲容量；
 *        **每次 Run 都从缓冲开头写**——上层必须先消费上一批输出再调用；
 * step3. 调用 `deflate()`（`ToZlibFlush` 把 FlushMode 映射成 `Z_*` 常量）；
 * step4. 用 `缓冲容量 - avail_out` 算出本次产出字节数（存到 `produced`）；
 * step5. 把 zlib 返回码映射成本项目的 `DeflateStatus`。
 *
 * ## 并发/失败语义
 *
 * - **`Z_BUF_ERROR` 映射成 `kOk` 而不是错误**：它表示「输出缓冲满了，没进展」，
 *   是正常的背压信号——调用方应消费输出后**再次调用**，而不是当成失败；
 * - **`noexcept`**：本函数在压缩热路径上，不能因异常中断流状态；
 * - **`m_impl->produced` 每次先清零**，保证 `output_size()` 反映的是本次产出，
 *   不会把上一次的字节数带出来；
 * - **输入由 `SetInput` 单独设置**（并被 `kMaxChunkSize` 截断），本函数不碰
 *   `next_in`/`avail_in`；因此调用方必须自己跟踪 `InputRemaining()`；
 * - **内部输出缓冲是固定的**：单次 Run 最多产出 `output_capacity()` 字节，
 *   压缩比很高时（大量重复数据）需要多轮 Run 才能把输入吃完。
 */
DeflateStatus Deflater::Run(FlushMode mode) noexcept {
  // step1: zlib 流不可用时直接报错。
  if (!ok()) {
    return DeflateStatus::kError;
  }
  // step2: 每次 Run 都从输出缓冲开头写，先重置游标与剩余容量。
  m_impl->produced = 0;
  m_impl->stream.next_out =
      reinterpret_cast<Bytef*>(m_impl->output.data());
  m_impl->stream.avail_out = static_cast<uInt>(m_impl->output.size());
  // step3: 唯一一次 deflate() 调用。
  const int result = deflate(&m_impl->stream, ToZlibFlush(mode));
  // step4: 产出字节数 = 容量 - 剩余。
  m_impl->produced = m_impl->output.size() - m_impl->stream.avail_out;
  // step5: 返回码映射（Z_BUF_ERROR 是背压信号，不是错误）。
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
