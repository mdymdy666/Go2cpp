#pragma once

/**
 * @file framing.hpp
 * @brief TCP 字节流的分帧工具，解决粘包、半包和跨 read 分隔符。
 * @details TCP 没有消息边界；调用方应选择固定长度、分隔符或长度前缀之一，
 *          并设置 max_frame 防止恶意长度导致内存无限增长。
 *
 * ## 模块定位
 * TCP 分帧：把字节流切成消息。被服务端/客户端的自定义协议实现使用；
 * 不拥有 Socket，只管缓冲与切分。
 *
 * ## 本文件定义什么
 * - `Framer::Mode`：`Delimiter` / `Fixed` / `BigEndian32Length` 三种分帧模式；
 * - `Framer::FeedError`：失败分类；
 * - `Framer`：`try_feed()`（noexcept）、`feed()`（抛异常兼容层）、`take_frames()`、`reset()`。
 *
 * ## 阅读建议
 * 先看三种 Mode 的语义，再看 `try_feed()` 的循环（切出一帧后立即 erase），
 * 最后看失败态：`fail()` 会同时清空缓冲与已切出的帧。
 *
 * ## 边界与陷阱
 * - `try_feed()` 是 `noexcept`：超长/分配失败进入**失败态**，必须 `reset()` 才能复用；
 * - 失败时会**丢弃已切出的帧**，避免调用方把部分结果当成完整批次；
 * - 长度前缀模式先校验 `max_frame` 再等数据（防「先要 4GB 内存」的放大攻击）；
 * - `Fixed` 模式的 `max_frame` 等于 `frame_size`，不允许半帧拼接。
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
/**
 * @brief 增量分帧器。Feed 可以一次输入多帧，take_frames 取出完整帧。
 *
 * ## 定位
 * 被自定义协议的服务端/客户端使用（例如行协议、固定长度报文、4 字节长度前缀报文）；
 * 与 `ByteArray` 互补：前者切边界，后者做编解码。
 *
 * ## 不变量
 * - `m_failed` 为真后 `try_feed()` 一律返回 `Failed`，且缓冲/帧列表已清空；
 * - `m_buffer` 只保存**未成帧**的字节（切出的帧立即 erase）；
 * - `Fixed` 模式不会产生短帧；`Delimiter` 模式的分隔符不计入帧内容。
 */
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
     *
     * ## 步骤
     * step1. 已失败直接返回 `Failed`；
     * step2. 追加本次字节；
     * step3. 循环按 Mode 找一帧：Delimiter 找分隔符、Fixed 等够长度、
     *         BigEndian32Length 先读 4 字节大端长度再等 payload；
     * step4. 切出的帧 push 到 `m_frames`，`m_buffer` erase 掉已消费部分；
     * step5. 超长/分配失败 -> `fail()` 清空一切并进入失败态。
     *
     * ## 边界
     * 长度前缀模式在**等 payload 之前**就校验 `max_frame`，否则恶意长度会先触发扩容。
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
    // 分帧模式与参数；构造时固定，try_feed 只读。
    Mode m_mode; std::string m_delimiter; std::size_t m_frame_size; std::size_t m_max_frame;
    // 未成帧字节 / 已切出的帧；try_feed 写，take_frames 取走，fail 清空。
    std::string m_buffer; std::vector<std::string> m_frames;
    // 失败态；fail 置位，reset 清零，try_feed 首先检查。
    bool m_failed{false};
};

}  // namespace go2cpp::net

namespace go2cpp { using net::Framer; }
