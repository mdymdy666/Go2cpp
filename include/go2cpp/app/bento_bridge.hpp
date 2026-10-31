#pragma once

/**
 * @file bento_bridge.hpp
 * @brief 应用层帧 ↔ Bento 通用帧（20B 头 + CRC32）的桥接。
 *
 * @details 应用层帧（9B 头）可以独立跑在任何字节流上；当需要复用 Bento 的
 *          UDP 链路/多路复用基础设施时，用本文件把它装进 Bento 的通用帧：
 *
 *          ```
 *          +-------------------- Bento 帧（20B 头 + CRC32）--------------------+
 *          | magic "BENT" | ver | type=kAppFrameType | flags | len | crc      |
 *          +----------------------------------------------------------------+
 *          | App 帧（9B 头 + 载荷）                                          |
 *          +----------------------------------------------------------------+
 *          ```
 *
 *          这样 Bento 的 `FrameDecoder`/`Link` 只负责「消息边界 + 完整性」，
 *          应用层帧负责「流表语义」，两层职责不重叠。`type` 字段使用独立的
 *          `kAppFrameType`（ASCII "AP"），与 Bento Link 自己的帧/会话层帧
 *          区分开；混接到同一端口时会在类型检查处明确失败。
 *
 *          **为什么不用 Bento 的 STREAM 通道直接装 App 帧字节**：两种方式都
 *          支持；`bento_link_transport.hpp` 走字节流（更省 20B 帧头），本桥
 *          走消息帧（自带 CRC + 抗 UDP 丢包/截断的边界恢复）。按场景选用。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/app/frame.hpp"

namespace go2cpp::app::bento_bridge {

/// Bento 帧 `type` 字段里代表「应用层帧」的值（ASCII "AP"）。
inline constexpr std::uint16_t kAppFrameType = 0x4150;

/// 桥接配置。
struct Config {
    std::uint16_t frame_type = kAppFrameType;  ///< Bento 帧类型。
    std::uint32_t max_app_payload = kDefaultMaxFramePayload;  ///< App 帧载荷上限。
};

/// 编解码结果。
enum class Status : std::uint8_t {
    kOk = 0,             ///< 成功。
    kNeedMoreData = 1,   ///< 半包。
    kBadFrame = 2,       ///< Bento 帧非法（magic/CRC/长度）。
    kBadFrameType = 3,   ///< 不是应用层帧。
    kBadPayload = 4,     ///< App 帧载荷非法。
};

/// @brief 状态名。
const char* StatusName(Status status) noexcept;

/// @brief 把 App 帧编码成 Bento 帧字节；失败返回空。
std::vector<std::uint8_t> Encode(const Frame& frame, const Config& config = {});

/**
 * @brief 从缓冲区解析一个 Bento 帧并取出其中的 App 帧。
 * @param data 输入。
 * @param size 字节数。
 * @param out 输出 App 帧。
 * @param consumed 非空时写入消耗字节数（仅 kOk）。
 * @param config 桥接配置。
 */
Status Decode(const std::uint8_t* data, std::size_t size, Frame* out,
              std::size_t* consumed = nullptr, const Config& config = {});

/**
 * @brief 流式桥接解码器（处理粘包/半包）。
 */
class Decoder {
   public:
    explicit Decoder(const Config& config = {});
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /// @brief 追加字节。
    void Append(const std::uint8_t* data, std::size_t size);

    /// @brief 取出下一个 App 帧。
    Status Next(Frame* out);

    /// @brief 未消费字节数。
    std::size_t PendingBytes() const noexcept;

    /// @brief 清空。
    void Clear() noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  ///< 隐藏 Bento 头依赖。
};

}  // namespace go2cpp::app::bento_bridge
