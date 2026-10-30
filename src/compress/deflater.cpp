/**
 * @file deflater.cpp
 * @brief `detail::Deflater` 的实现：自研 DEFLATE 编码器的 RAII 封装。
 *
 * @details 与旧实现的区别：不再 include `<zlib.h>`，`deflateInit2` /
 *   `deflate` / `deflateEnd` 全部换成 `src/compress/deflate_writer.hpp` 里的
 *   `DeflateEncoder`。对上层 `WriterEngine` 的契约保持不变：
 *
 *   1. **输出缓冲固定 64 KiB**：`Run` 每次最多复制这么多字节；编码器内部
 *      可能还有未排空的字节，下一次 `Run` 会继续调用 `Drain`。
 *   2. **`kFinish` 的分支**：`Finish` 只写一次最终块与尾部；之后重复
 *      `Run(kFinish)` 只负责排空，排空完成才返回 `kStreamEnd`。
 *   3. **进度保证**：每次 `Run` 要么消费输入、要么产出输出（或同时），
 *      满足 `WriterEngine` 的「无进展即报错」判定。
 */
#include "go2cpp/compress/detail/writer_engine.hpp"

#include "go2cpp/compress/detail/constants.hpp"
#include "deflate_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::compress::detail {

namespace {

/// 单次 Run 最多喂给编码器的未压缩字节数；与编码器的块大小一致。
constexpr std::size_t kBlockFeedSize = 32U * 1024U;

} // namespace

/**
 * @brief `Deflater` 的 pimpl 状态：自研编码器 + 固定输出缓冲。
 *
 * ## 不变量
 * - `output.size() == kOutputBufferSize`；
 * - `avail` 是尚未交给编码器的输入字节数，`next_in` 指向其中第一个字节；
 * - `produced` 只反映最近一次 `Run` 的产出。
 */
struct Deflater::Impl {
    explicit Impl(int level, int window_bits) : encoder(level, window_bits) {
        output.resize(kOutputBufferSize);
    }

    DeflateEncoder encoder;            ///< 自研 DEFLATE 状态；构造/Reset/Run 使用
    std::vector<std::uint8_t> output;  ///< 固定 64 KiB 输出缓冲
    const std::uint8_t* next_in{nullptr}; ///< 未消费输入起点
    std::size_t avail{0};              ///< 未消费输入字节数
    std::size_t produced{0};           ///< 最近一次 Run 的产出
};

Deflater::Deflater(int level, int window_bits, int mem_level)
    : m_impl(new Impl(level, window_bits)) {
    // mem_level 是 zlib 的内存/速度旋钮；自研实现的哈希表大小固定，
    // 该参数仅为保持接口兼容而保留，不参与行为。
    (void)mem_level;
}

Deflater::~Deflater() = default;

bool Deflater::ok() const noexcept {
    return m_impl && m_impl->encoder.ok();
}

bool Deflater::Reset() noexcept {
    if (!ok()) {
        return false;
    }
    m_impl->encoder.Reset();
    m_impl->next_in = nullptr;
    m_impl->avail = 0;
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
    m_impl->next_in = data;
    m_impl->avail = std::min(size, kMaxChunkSize);
}

std::size_t Deflater::InputRemaining() const noexcept {
    return ok() ? m_impl->avail : 0;
}

DeflateStatus Deflater::Run(FlushMode mode) noexcept {
    if (!ok()) {
        return DeflateStatus::kError;
    }
    m_impl->produced = 0;
    // 每次最多吃一个块，避免 1 GiB 输入一次性堆进编码器内部缓冲。
    if (m_impl->next_in != nullptr && m_impl->avail > 0) {
        const std::size_t take = std::min<std::size_t>(m_impl->avail, kBlockFeedSize);
        m_impl->encoder.Feed(m_impl->next_in, take);
        m_impl->next_in += take;
        m_impl->avail -= take;
    }
    m_impl->encoder.Pump();
    const bool input_drained = m_impl->avail == 0;
    if (mode == FlushMode::kSync && input_drained) {
        m_impl->encoder.Sync();
    } else if (mode == FlushMode::kFinish && input_drained) {
        m_impl->encoder.Finish();
    }
    m_impl->produced = m_impl->encoder.Drain(m_impl->output.data(),
                                             m_impl->output.size());
    if (mode == FlushMode::kFinish && m_impl->encoder.finished() &&
        !m_impl->encoder.HasPending()) {
        return DeflateStatus::kStreamEnd;
    }
    return DeflateStatus::kOk;
}

std::string Deflater::Message() const {
    return m_impl ? m_impl->encoder.message() : std::string();
}

} // namespace go2cpp::compress::detail
