#pragma once

/**
 * @file framing.hpp
 * @brief TCP 字节流的分帧工具，解决粘包、半包和跨 read 分隔符。
 * @details TCP 没有消息边界；调用方应选择固定长度、分隔符或长度前缀之一，
 *          并设置 max_frame 防止恶意长度导致内存无限增长。
 */

#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::net {

/** @brief 增量分帧器。Feed 可以一次输入多帧，take_frames 取出完整帧。 */
class Framer final {
public:
    enum class Mode { Delimiter, Fixed, BigEndian32Length };
    enum class FeedError { None, FrameTooLarge, ResourceExhausted, Failed };
    static Framer delimiter(std::string delimiter, std::size_t max_frame = 16 * 1024 * 1024) {
        return Framer(Mode::Delimiter, std::move(delimiter), 0, max_frame);
    }
    static Framer fixed(std::size_t frame_size) { return Framer(Mode::Fixed, {}, frame_size, frame_size); }
    static Framer big_endian32(std::size_t max_frame = 16 * 1024 * 1024) { return Framer(Mode::BigEndian32Length, {}, 0, max_frame); }
    /**
     * @brief 输入字节并提取完整帧，不向调用方抛出分配异常。
     * @details 出现超长输入或内存分配失败时，分帧器进入失败态并释放未完成
     *          数据；调用 Reset 后可复用。失败期间已经缓存的帧也会清空，避免
     *          调用方把部分处理结果误认为完整批次。
     * @param data 本次 TCP 字节流片段。
     * @return None 表示成功；其他值表示分帧器已失败。
     */
    FeedError try_feed(std::string_view data) noexcept {
        if (m_failed) return FeedError::Failed;
        try {
            if (!data.empty()) m_buffer.append(data.data(), data.size());
            for (;;) {
                std::size_t frame_size = 0;
                std::size_t consumed = 0;
                if (m_mode == Mode::Delimiter) {
                    const auto position = m_buffer.find(m_delimiter);
                    if (position == std::string::npos) {
                        if (m_buffer.size() > m_max_frame) {
                            return fail(FeedError::FrameTooLarge);
                        }
                        return FeedError::None;
                    }
                    if (position > m_max_frame) {
                        return fail(FeedError::FrameTooLarge);
                    }
                    frame_size = position;
                    consumed = position + m_delimiter.size();
                } else if (m_mode == Mode::Fixed) {
                    if (m_buffer.size() < m_frame_size) return FeedError::None;
                    frame_size = m_frame_size;
                    consumed = frame_size;
                } else {
                    if (m_buffer.size() < 4) return FeedError::None;
                    const auto* value = reinterpret_cast<const std::uint8_t*>(m_buffer.data());
                    const std::size_t length =
                        (static_cast<std::size_t>(value[0]) << 24) |
                        (static_cast<std::size_t>(value[1]) << 16) |
                        (static_cast<std::size_t>(value[2]) << 8) |
                        static_cast<std::size_t>(value[3]);
                    if (length > m_max_frame ||
                        length > std::numeric_limits<std::size_t>::max() - 4) {
                        return fail(FeedError::FrameTooLarge);
                    }
                    if (m_buffer.size() < length + 4) return FeedError::None;
                    frame_size = length;
                    consumed = length + 4;
                }
                m_frames.emplace_back(
                    m_buffer.data() + (m_mode == Mode::BigEndian32Length ? 4 : 0),
                    frame_size);
                m_buffer.erase(0, consumed);
            }
        } catch (const std::length_error&) {
            return fail(FeedError::FrameTooLarge);
        } catch (const std::bad_alloc&) {
            return fail(FeedError::ResourceExhausted);
        } catch (...) {
            return fail(FeedError::ResourceExhausted);
        }
    }

    /** @brief 兼容接口；输入错误仍通过 length_error/bad_alloc 抛出。 */
    void feed(std::string_view data) {
        const auto result = try_feed(data);
        if (result == FeedError::FrameTooLarge) {
            throw std::length_error("frame too large");
        }
        if (result == FeedError::ResourceExhausted) throw std::bad_alloc();
        if (result == FeedError::Failed) {
            throw std::runtime_error("framer is failed; call reset before reuse");
        }
    }
    std::vector<std::string> take_frames() { std::vector<std::string> result; result.swap(m_frames); return result; }
    std::size_t buffered_size() const noexcept { return m_buffer.size(); }
    /** @brief 清理缓冲和失败态，供调用方在丢弃损坏连接后重新使用。 */
    void reset() noexcept { m_buffer.clear(); m_frames.clear(); m_failed = false; }

private:
    FeedError fail(FeedError error) noexcept {
        m_failed = true;
        std::string().swap(m_buffer);
        std::vector<std::string>().swap(m_frames);
        return error;
    }

    Framer(Mode mode, std::string delimiter, std::size_t frame_size, std::size_t max_frame)
        : m_mode(mode), m_delimiter(std::move(delimiter)), m_frame_size(frame_size), m_max_frame(max_frame) {
        if (m_mode == Mode::Delimiter && m_delimiter.empty()) throw std::invalid_argument("empty delimiter");
        if (m_mode == Mode::Fixed && m_frame_size == 0) throw std::invalid_argument("zero frame size");
    }
    Mode m_mode; std::string m_delimiter; std::size_t m_frame_size; std::size_t m_max_frame;
    std::string m_buffer; std::vector<std::string> m_frames;
    bool m_failed{false};
};

}  // namespace go2cpp::net

namespace go2cpp { using net::Framer; }
