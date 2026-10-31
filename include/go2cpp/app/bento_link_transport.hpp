#pragma once

/**
 * @file bento_link_transport.hpp
 * @brief 把 Bento Link 的 STREAM（可靠有序）通道适配成应用层 `ITransport`。
 *
 * @details Bento Link 的一条可靠通道保证「有序、不丢、自动分片（单条消息
 *          ≤ ~330KB）」，但**不保留应用层的帧边界**语义——对应用层来说它就是
 *          一条可靠字节流。本适配器在它上面做三件事：
 *
 *          1. `WriteAll` 把字节流按 32 KiB 切成多条 STREAM 消息（远低于
 *             Bento 单条上限，给加密/协议头留余量）。接收端按字节流拼帧，
 *             不依赖消息边界；
 *          2. `on_stream` 回调把同一通道的数据追加到接收队列（支持粘包/半包）；
 *          3. `Read` 从队列取字节，`Close` 唤醒所有等待者。
 *
 *          Link 本身有发送窗口（默认 256 条 / 1MiB）。窗口满时
 *          `TrySendStream` 返回 `kWouldBlock`，本适配器短暂退避后重试，
 *          整体超过 `send_timeout_ms` 才失败——这对上层是「慢对端」而不是
 *          「丢数据」。
 *
 * ## 线程模型
 *
 *          - `on_stream` 回调运行在 Link 的 `RunOnce` 接收线程；
 *          - `WriteAll` 可在 Connection 的 IO 线程调用（Link 发送线程安全）；
 *          - `Read` 在 Connection 的 IO 线程阻塞等待，条件变量唤醒；
 *          - `Install()` 必须在连接建立前调用，且只调用一次。
 *
 * ## 不负责什么
 *
 *          不接管 Link 的生命周期（Open/Connect/Close 由调用方管理），不实现
 *          重连，也不在 Link 断开时自动关闭 Connection。生产环境应在
 *          `Link::on_disconnected` 里调用 Connection::Close。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "go2cpp/app/transport.hpp"
#include "go2cpp/bento/net/link.hpp"

namespace go2cpp::app {

/**
 * @brief Bento Link STREAM 通道上的字节流适配器。
 */
class BentoLinkTransport final : public ITransport {
   public:
    /**
     * @param link 已 Open 的 Link（不拥有）。
     * @param channel 可靠通道号（0..7，默认 20 会被 Bento 拒绝；必须 < 8）。
     * @param max_message_bytes 单条 STREAM 消息上限（默认 32 KiB）。
     * @param send_timeout_ms 发送窗口满时的总重试预算。
     */
    explicit BentoLinkTransport(go2cpp::bento::Link* link, std::uint8_t channel = 3,
                                std::size_t max_message_bytes = 32 * 1024,
                                std::uint32_t send_timeout_ms = 10000);

    ~BentoLinkTransport() override;

    BentoLinkTransport(const BentoLinkTransport&) = delete;
    BentoLinkTransport& operator=(const BentoLinkTransport&) = delete;

    /**
     * @brief 安装 on_stream 回调（链式调用原回调）。
     * @return 成功返回 true；Link 为空/已安装返回 false。
     */
    bool Install();

    /// @brief Link 接收线程把同通道数据喂进来（Install 后自动调用）。
    void OnLinkData(const std::uint8_t* data, std::size_t size);

    bool WriteAll(const std::uint8_t* data, std::size_t size) override;
    int Read(std::uint8_t* data, std::size_t capacity,
             std::uint32_t timeout_ms) override;
    void Close() override;
    bool closed() const override;
    std::string Describe() const override;

    /// @brief 接收队列里未读字节数。
    std::size_t pending() const;

    /// @brief 通道号。
    std::uint8_t channel() const noexcept { return m_channel; }

   private:
    go2cpp::bento::Link* m_link = nullptr;  ///< 链路（不拥有）。
    std::uint8_t m_channel = 3;             ///< 可靠通道。
    std::size_t m_max_message = 32 * 1024;  ///< 单条消息上限。
    std::uint32_t m_send_timeout_ms = 10000;///< 发送重试预算。
    mutable std::mutex m_mutex;             ///< 保护接收队列。
    std::condition_variable m_cv;           ///< 读等待。
    std::vector<std::uint8_t> m_rx;         ///< 未读字节。
    bool m_closed = false;                  ///< 是否已关闭。
    bool m_installed = false;               ///< 是否已安装回调。
    std::function<void(std::uint8_t, const std::uint8_t*, std::size_t)> m_previous;
};

}  // namespace go2cpp::app
