/**
 * @file inflater.cpp
 * @brief `detail::Inflater` 的实现：自研 DEFLATE 解码器的 RAII 封装。
 *
 * @details 不再 include `<zlib.h>`；`inflateInit2` / `inflate` / `inflateEnd`
 *   换成 `src/compress/deflate_reader.hpp` 的 `InflateDecoder`。保留的语义：
 *
 *   1. **错误分类**：解码器直接返回 `ErrHeader` / `ErrChecksum` /
 *      `ErrDictionary` / `ErrCorrupt`，不再依赖 zlib 的 `strm.msg` 文本匹配；
 *   2. **多成员重置**：`ResetForNextMember` 保留未消费输入与位缓存，只清空
 *      成员级状态（校验和、窗口、块状态），与 zlib 的 `inflateReset2` 等价；
 *   3. **uInt 截断防护**：单次输出上限仍按 `kMaxChunkSize` 截断，由上层循环。
 */
#include "go2cpp/compress/detail/reader_engine.hpp"

#include "go2cpp/compress/detail/constants.hpp"
#include "deflate_reader.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace go2cpp::compress::detail {

/**
 * @brief `Inflater` 的 pimpl 状态：自研解码器 + 固定输入缓冲。
 *
 * ## 不变量
 * - `input.size() == kInputBufferSize`；
 * - 解码器的 `input_available()` 等于 `input` 中尚未被位读取器取走的字节数；
 * - `CommitInput` 只在 `input_available() == 0` 时被上层调用。
 */
struct Inflater::Impl {
    explicit Impl(int window_bits) : decoder(window_bits) {
        input.resize(kInputBufferSize);
    }

    InflateDecoder decoder;            ///< 自研 DEFLATE 状态
    std::vector<std::uint8_t> input;   ///< 固定 32 KiB 输入缓冲
};

Inflater::Inflater(int window_bits) : m_impl(new Impl(window_bits)) {}

Inflater::~Inflater() = default;

bool Inflater::ok() const noexcept {
    return m_impl && m_impl->decoder.ok();
}

bool Inflater::Reset() noexcept {
    if (!ok()) {
        return false;
    }
    m_impl->decoder.Reset();
    return true;
}

std::uint8_t* Inflater::input_buffer() noexcept {
    return m_impl ? m_impl->input.data() : nullptr;
}

std::size_t Inflater::input_capacity() const noexcept {
    return m_impl ? m_impl->input.size() : 0;
}

std::size_t Inflater::input_available() const noexcept {
    return m_impl ? m_impl->decoder.input_available() : 0;
}

void Inflater::CommitInput(std::size_t bytes) noexcept {
    if (!ok() || input_available() != 0) {
        // 缓冲里还有未消费字节时不能拼接新数据：位读取器仍指向缓冲内部。
        return;
    }
    m_impl->decoder.CommitInput(m_impl->input.data(),
                                std::min(bytes, m_impl->input.size()));
}

InflateStatus Inflater::Inflate(std::uint8_t* out, std::size_t out_size,
                                std::size_t* produced, ErrorPtr* error) noexcept {
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
    const DecodeStatus status =
        m_impl->decoder.Inflate(out, chunk, produced, error);
    switch (status) {
        case DecodeStatus::kOk:
            return InflateStatus::kOk;
        case DecodeStatus::kMemberEnd:
            return InflateStatus::kMemberEnd;
        case DecodeStatus::kError:
            return InflateStatus::kError;
    }
    return InflateStatus::kError;
}

void Inflater::ResetForNextMember() noexcept {
    if (!ok()) {
        return;
    }
    m_impl->decoder.ResetForNextMember();
}

} // namespace go2cpp::compress::detail
