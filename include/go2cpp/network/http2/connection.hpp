/**
 * @file connection.hpp
 * @brief HTTP/2 连接层（握手、帧循环、流管理、HPACK 集成）。
 *
 * ## 定位
 *
 * 这是"协议逻辑"的最后一块：它把 HPACK、帧层、流状态机串起来，形成
 * 「客户端发请求 / 服务端回响应」的闭环。**它不关心字节从哪来**
 * —— IO 通过 `Transport` 注入，所以能完全脱离网络测试。
 *
 * ## 并发模型
 *
 * 全部接口都是**同步阻塞**语义（最贴近 Go 的 `http2`）：`Handshake`、
 * `ReadEvent`、`WriteData` 在调用线程/Fiber 上阻塞到完成为止。
 * 在生产者环境里，`Transport` 的实现用本项目的 `IOManager::wait` + Fiber
 * 把阻塞变成协程挂起，**连接层代码一行不改**。
 *
 * ## 已实现
 *
 * - 客户端/服务端握手（preface + SETTINGS 交换 + ACK）；
 * - 客户端发起请求（HEADERS）、发送 DATA、关闭流；
 * - 服务端接收 HEADERS/DATA 并作为事件投递；
 * - 流表管理（每条流一个 `StreamStateMachine`）；
 * - PING 自动回 ACK、GOAWAY、RST_STREAM、WINDOW_UPDATE 事件；
 * - 收到 DATA 后自动回 WINDOW_UPDATE 恢复窗口（简单策略，见下）。
 *
 * ## 明确未实现（不假装支持）
 *
 * - **CONTINUATION**：header block 超过一个帧时返回 `kUnsupported`。真实场景
 *   需要它（大 header），但它要求"同一时刻只允许一条流在发 header block"的
 *   互斥状态，属独立工作；
 * - **精细流控**：当前只在收到 DATA 后立刻把窗口补回去（等价于"不真正限流"）。
 *   真正的流控需要按 `SETTINGS_INITIAL_WINDOW_SIZE` 与接收方读取速度配平；
 * - **优先级**：PRIORITY 帧被解析但不参与调度（RFC 9113 已弃用）；
 * - **服务器推送的实际使用**：PUSH_PROMISE 能解析，但没有推送 API。
 */
#pragma once

#include "go2cpp/network/http2/frame.hpp"
#include "go2cpp/network/http2/hpack.hpp"
#include "go2cpp/network/http2/stream_state.hpp"
#include "go2cpp/network/http2/transport.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace go2cpp::net::http2 {

/** @brief 连接角色。 */
enum class Role : std::uint8_t { kClient, kServer };

/** @brief 连接配置。 */
struct ConnectionConfig final {
    std::uint32_t max_frame_size{kDefaultMaxFrameSize};
    std::uint32_t header_table_size{4096};
    std::uint32_t initial_window_size{65535};
    std::uint32_t max_concurrent_streams{100};
};

/** @brief 连接层的操作结果。 */
enum class ConnStatus : std::uint8_t {
    kOk,
    kClosed,
    kProtocolError,
    kUnsupported,
};

const char* ToString(ConnStatus status) noexcept;

/** @brief 从连接读到的一个事件。 */
struct IncomingEvent final {
    enum class Kind : std::uint8_t {
        kHeaders,
        kData,
        kSettings,
        kPing,
        kGoAway,
        kRstStream,
        kWindowUpdate,
        kUnknown,
    };
    Kind kind{Kind::kUnknown};
    std::uint32_t stream_id{0};
    bool end_stream{false};
    /// kind == kHeaders 时有效（已用 HPACK 解出）。
    std::vector<HeaderField> headers;
    /// kind == kData 时有效。
    std::vector<std::uint8_t> data;
    /// kind == kPing 时有效。
    std::uint64_t ping_opaque{0};
    /// kind == kGoAway / kRstStream 时有效。
    ErrorCode error_code{ErrorCode::kNoError};
    /// kind == kGoAway 时有效。
    std::uint32_t last_stream_id{0};
};

/**
 * @brief 一个 HTTP/2 连接。
 * @details 非线程安全：一个连接只能由一个线程/Fiber 驱动（与 Go 的 `http2`
 *          用法一致 —— 单 goroutine 读，写需外部同步）。这一点在类注释里
 *          说明而不加锁，是为了让协议逻辑保持简单、可测。
 */
class Connection final {
public:
    Connection(Transport* transport, Role role,
               ConnectionConfig config = ConnectionConfig{});
    ~Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Role role() const noexcept { return m_role; }
    const ConnectionConfig& config() const noexcept { return m_config; }
    bool handshaked() const noexcept { return m_handshaked; }

    /**
     * @brief 完成握手（客户端发 preface+SETTINGS 并等对端 SETTINGS；
     *        服务端反之）。
     * @return 状态。
     */
    ConnStatus Handshake(std::string* error = nullptr);

    /**
     * @brief 客户端发起一个请求：分配新流并发送 HEADERS。
     * @param headers 请求头（必须含 `:method`/`:path`/`:scheme`/`:authority`）。
     * @param end_stream 是否在 HEADERS 上就结束发送方向（无 body）。
     * @param out_stream_id 成功时写入分配的流 id。
     * @return 状态。非客户端调用返回 `kUnsupported`。
     */
    ConnStatus StartRequest(const std::vector<HeaderField>& headers, bool end_stream,
                            std::uint32_t* out_stream_id, std::string* error = nullptr);

    /** @brief 在已打开的流上发送 DATA。 */
    ConnStatus WriteData(std::uint32_t stream_id, const std::vector<std::uint8_t>& data,
                         bool end_stream, std::string* error = nullptr);

    /** @brief 服务端回响应：发送 HEADERS。 */
    ConnStatus SendResponseHeaders(std::uint32_t stream_id,
                                   const std::vector<HeaderField>& headers, bool end_stream,
                                   std::string* error = nullptr);

    /** @brief 主动关闭一条流（RST_STREAM）。 */
    ConnStatus ResetStream(std::uint32_t stream_id, ErrorCode code,
                           std::string* error = nullptr);

    /** @brief 发送 GOAWAY 并标记连接不再接受新流。 */
    ConnStatus SendGoAway(ErrorCode code, std::string* error = nullptr);

    /**
     * @brief 读取下一个事件（阻塞）。
     * @param out 成功时写入事件。
     * @return `kOk` 表示拿到事件；`kClosed` 表示对端关闭。
     */
    ConnStatus ReadEvent(IncomingEvent* out, std::string* error = nullptr);

    /** @brief 流表（只读，便于测试与诊断）。 */
    const std::map<std::uint32_t, StreamStateMachine>& Streams() const noexcept {
        return m_streams;
    }

    /** @brief 当前已写出的未刷新字节数（含在 Transport 内的）。 */
    std::size_t PendingInboundBytes() const noexcept { return m_read_buffer.size(); }

private:
    /// 保证缓冲区至少有 `need` 字节；不足时从 Transport 继续读。
    bool FillBuffer(std::size_t need, std::string* error);
    /// 从缓冲区取出一个完整帧。
    ConnStatus TakeFrame(Frame* out, std::string* error);
    /// 写一帧（直接经 Transport）。
    ConnStatus WriteFrame(const Frame& frame, std::string* error);
    /// 处理握手期的 SETTINGS 交换。
    ConnStatus FinishSettingsHandshake(std::string* error);
    /// 取或创建流状态机。
    StreamStateMachine* StreamFor(std::uint32_t stream_id);

    Transport* m_transport{nullptr};
    Role m_role{Role::kClient};
    ConnectionConfig m_config;
    bool m_handshaked{false};
    /// 下一个可用的客户端流 id（奇数）。
    std::uint32_t m_next_stream_id{1};
    std::uint32_t m_peer_max_frame_size{kDefaultMaxFrameSize};
    std::vector<std::uint8_t> m_read_buffer;
    Decoder m_decoder;
    Encoder m_encoder;
    std::map<std::uint32_t, StreamStateMachine> m_streams;
    /// 握手期间暂存的 SETTINGS 是否已收到/已发送。
    bool m_peer_settings_received{false};
    bool m_local_settings_sent{false};
};

}  // namespace go2cpp::net::http2
