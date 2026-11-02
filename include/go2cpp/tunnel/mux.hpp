#pragma once

/**
 * @file mux.hpp
 * @brief 一条控制连接上的多路复用：流表 + 帧收发 + 心跳 + 有界接收队列。
 *
 * @details 公网穿透最常见的形式是「一条长连接到 VPS，所有浏览器连接复用
 *          它」。这比「每个浏览器连接一条新 TCP」省掉每连接的握手 RTT，也让
 *          本地只需一条出站连接（更容易穿过企业防火墙）。
 *
 * ## 线程模型
 *
 * ```
 *   控制连接 fd
 *      ├── ReceiveLoop   1 条线程：recv → FrameCodec → 分发到各 MuxStream
 *      └── HeartbeatLoop 1 条线程：定期 PING / 检查空闲超时
 * ```
 *
 * 每个 `MuxStream` 的 `Read` 由各自的 RelayPipe 读线程调用，`Write` 由写线程
 * 调用；`SendFrame` 用发送互斥量串行化，保证帧不会交错。
 *
 * ## 背压
 *
 * 接收侧：每个流有**独立的有界队列**（`recv_queue_cap` 字节）。接收线程**不
 * 阻塞**——一条慢流不能拖住整条控制连接。队列溢出时该流被关闭（保护整体），
 * 其余流继续工作。这是本版本的取舍：真正的按流窗口（yamux 式 credit）留给
 * 后续版本，见 `docs/modules/tunnel.md` 的「限制」。
 *
 * 发送侧：RelayPipe 自己的有界队列（`PipeConfig::queue_cap`）提供背压；帧写入
 * 带 30 秒超时，写不动就关流。
 *
 * ## 空闲与断线
 *
 * 心跳线程每 `ping_interval_ms` 发一个 `PING`；超过 `idle_timeout_ms` 没收到
 * **任何**帧就判定连接死亡，`Shutdown()` 关闭所有流。客户端据此触发重连。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/frame/frame_codec.hpp"
#include "go2cpp/relay/relay_pipe.hpp"
#include "go2cpp/tunnel/net.hpp"
#include "go2cpp/tunnel/protocol.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace go2cpp {
namespace tunnel {

class MuxSession;

/**
 * @brief 握手阶段的帧读取器。
 *
 * @details 客户端会把 `AUTH` 与 `HELLO` 连着写，一次 `recv` 里可能同时到达
 *          两帧；`frame::FrameCodec::Feed` 会把它们都解出来。本类把多余的帧
 *          存进内部队列，保证下一次 `Next()` 不会丢帧。
 */
class FrameReader final {
public:
    /**
     * @param conn 已连接的 TCP。
     * @param with_checksum 是否启用帧尾 CRC（两端必须一致）。
     */
    FrameReader(TcpConn::Ptr conn, bool with_checksum);

    /**
     * @brief 读下一帧。
     * @param timeout_ms 最长等待。
     * @param out 输出帧。
     * @param err 失败原因（可为空）。
     * @return 成功 true。
     */
    bool Next(int timeout_ms, frame::Frame* out, std::string* err);

private:
    TcpConn::Ptr m_conn;
    frame::FrameCodec m_codec;
    std::deque<frame::Frame> m_pending;
};

/**
 * @brief 直接往裸连接写一帧（握手阶段还没有 `MuxSession`）。
 * @return 写入成功返回 true。
 */
bool WriteFrame(const TcpConn::Ptr& conn, bool with_checksum, FrameType type,
                const std::vector<std::uint8_t>& payload);

/// `MuxSession` 的可调参数。
struct MuxConfig {
    /// 单流接收队列上限（字节）；溢出即关闭该流。默认 8 MiB。
    std::size_t recv_queue_cap = 8U * 1024U * 1024U;
    /// 无任何入站帧多久判定连接死亡（毫秒）；默认 90 s。
    int idle_timeout_ms = 90000;
    /// 心跳 PING 间隔（毫秒）；默认 15 s。
    int ping_interval_ms = 15000;
    /// 单次 `recv` 等待切片（毫秒）；默认 1000。
    int read_slice_ms = 1000;
    /// 是否启用帧尾 CRC；必须与对端一致，默认关闭。
    bool with_checksum = false;
};

/**
 * @brief 多路复用中的一条流（对上层就是一个 `relay::Stream`）。
 *
 * @details 生命周期：由 `MuxSession` 的流表和上层（RelayPipe 线程）的
 *          `shared_ptr` 共同持有。会话关闭时流表被清空，但上层仍持有引用，
 *          因此不会有悬垂指针；`Read` 在关闭后返回 EOF，让 RelayPipe 收尾。
 */
class MuxStream final : public relay::Stream,
                        public std::enable_shared_from_this<MuxStream> {
public:
    using Ptr = std::shared_ptr<MuxStream>;

    ~MuxStream() override = default;

    streams::ReadResult Read(char* data, std::size_t size) override;
    streams::WriteResult Write(const char* data, std::size_t size) override;
    ErrorPtr Close() override;

    /// 流 id（服务端分配）。
    std::uint32_t id() const noexcept { return m_id; }
    /// 对端是否已关闭 / 会话是否已断开。
    bool peer_closed() const noexcept {
        return m_eof.load(std::memory_order_acquire);
    }
    /// 收到的字节数。
    std::uint64_t bytes_in() const noexcept {
        return m_bytes_in.load(std::memory_order_relaxed);
    }
    /// 发出的字节数。
    std::uint64_t bytes_out() const noexcept {
        return m_bytes_out.load(std::memory_order_relaxed);
    }

    // ---- 仅供 MuxSession 调用（接收线程 / 心跳线程） ----

    /// 接收线程投递一段数据；队列溢出返回 false（调用方应关闭该流）。
    bool PushData(const std::uint8_t* data, std::size_t length);
    /// 对端关闭（收到 CLOSE 帧或会话断开）。
    void MarkPeerClosed(const char* reason);

private:
    friend class MuxSession;

    MuxStream(std::weak_ptr<MuxSession> session, std::uint32_t id,
              std::size_t queue_cap);

    std::weak_ptr<MuxSession> m_session;
    std::uint32_t m_id;
    std::size_t m_queue_cap;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::vector<std::uint8_t>> m_queue;
    std::size_t m_queued_bytes = 0;
    std::atomic<bool> m_closed{false};  ///< 本地已调用 Close（或会话关闭）。
    std::atomic<bool> m_eof{false};     ///< 对端关闭 / 会话断开。

    std::atomic<std::uint64_t> m_bytes_in{0};
    std::atomic<std::uint64_t> m_bytes_out{0};
};

/**
 * @brief 多路复用会话：一条控制连接 + 若干 `MuxStream`。
 *
 * @details 使用顺序（两侧不同）：
 *
 * - **服务端**：`Dial`/`Accept` 得到连接 → 同步读 `HELLO`（用独立
 *   `FrameCodec`）→ `Create` + `Start` → 注册到路由表。
 * - **客户端**：`Dial` → `Create` → 发 `HELLO` → `Start` → `WaitHelloAck`
 *   （ACK 之后可能紧跟 `OPEN`，所以必须由接收线程解析）。
 */
class MuxSession final : public std::enable_shared_from_this<MuxSession> {
public:
    using Ptr = std::shared_ptr<MuxSession>;
    /// 新流回调；在**接收线程**里调用，实现必须立即返回（例如起一条工作线程）。
    using OnOpen = std::function<void(MuxStream::Ptr)>;

    static Ptr Create(TcpConn::Ptr conn, MuxConfig config);

    ~MuxSession();

    MuxSession(const MuxSession&) = delete;
    MuxSession& operator=(const MuxSession&) = delete;

    /**
     * @brief 启动接收线程与心跳线程。
     * @param on_open 新流回调（服务端传空，因为只有服务端发起 OPEN）。
     * @param err 失败原因（可为空）。
     * @return 成功 true；已启动或连接为空返回 false。
     */
    bool Start(OnOpen on_open, ErrorPtr* err);

    /// 等待对端 `HELLO_ACK`；返回 true 表示注册成功。
    bool WaitHelloAck(int timeout_ms, HelloAck* ack);

    /**
     * @brief 开一条新流并发送 `OPEN`。
     * @param peer_ip 触发本次流的公网对端 IP（仅用于展示/日志）。
     * @param peer_port 公网对端端口。
     * @return 新流；会话已关闭或发送失败返回空。
     */
    MuxStream::Ptr OpenStream(const std::string& peer_ip = {},
                              std::uint16_t peer_port = 0);

    /// 发送一帧（线程安全）；返回 false 表示连接已不可用。
    bool SendPayload(FrameType type, const std::vector<std::uint8_t>& payload);

    /**
     * @brief 发起优雅关闭：通知对端不再接受新流，在途流继续跑完。
     * @param reason 0=正常关闭、1=维护、2=被替换。
     * @param retry_after_ms 建议重连延迟；0 表示不要重连。
     * @return 帧已发出返回 true。
     */
    bool SendGoAway(std::uint8_t reason = 0, std::uint32_t retry_after_ms = 0);

    /// 是否已收到或发出 `GOAWAY`（此后 `OpenStream` 返回空）。
    bool draining() const noexcept {
        return m_draining.load(std::memory_order_acquire);
    }

    /// 关闭会话：断开连接、唤醒所有流、停止心跳（不 join，可重入）。
    void Shutdown(const char* reason);

    /// `Shutdown` + join 线程；幂等，析构时自动调用。
    void Stop();

    bool alive() const noexcept { return m_alive.load(std::memory_order_acquire); }

    /// 底层控制连接。
    const TcpConn::Ptr& conn() const noexcept { return m_conn; }

    /// 最近一次收到帧的时间（毫秒时间戳，诊断用）。
    std::int64_t last_rx_ms() const noexcept {
        return m_last_rx_ms.load(std::memory_order_relaxed);
    }

    /// 已创建过的流总数。
    std::uint64_t streams_total() const noexcept {
        return m_streams_total.load(std::memory_order_relaxed);
    }
    /// 当前活跃流数。
    std::size_t streams_active() const;
    /// 累计入站字节（含帧头）。
    std::uint64_t bytes_in() const noexcept {
        return m_bytes_in.load(std::memory_order_relaxed);
    }
    /// 累计出站字节。
    std::uint64_t bytes_out() const noexcept {
        return m_bytes_out.load(std::memory_order_relaxed);
    }
    /// 收到的 HELLO_ACK（客户端侧；未收到时为默认值）。
    HelloAck hello_ack() const;

private:
    friend class MuxStream;

    MuxSession(TcpConn::Ptr conn, MuxConfig config);

    void ReceiveLoop();
    void HeartbeatLoop();
    void Dispatch(const frame::Frame& frame);
    void RemoveStream(std::uint32_t id);
    /// 从表里删除并标记对端关闭 + 发 CLOSE 帧（队列溢出等异常路径）。
    void FailStream(std::uint32_t id, const char* reason);
    void MarkAllStreamsClosed(const char* reason);
    static std::int64_t NowMs();

    TcpConn::Ptr m_conn;
    MuxConfig m_config;
    OnOpen m_on_open;

    std::atomic<bool> m_alive{true};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_draining{false};

    std::mutex m_send_mutex;
    std::unique_ptr<frame::FrameCodec> m_encoder;

    mutable std::mutex m_streams_mutex;
    std::unordered_map<std::uint32_t, MuxStream::Ptr> m_streams;
    std::atomic<std::uint32_t> m_next_id{1};
    std::atomic<std::uint64_t> m_streams_total{0};

    mutable std::mutex m_ack_mutex;
    std::condition_variable m_ack_cv;
    HelloAck m_hello_ack;
    bool m_ack_received = false;

    std::mutex m_closed_mutex;
    std::condition_variable m_closed_cv;

    std::atomic<std::int64_t> m_last_rx_ms{0};
    std::atomic<std::uint64_t> m_bytes_in{0};
    std::atomic<std::uint64_t> m_bytes_out{0};
    std::atomic<std::uint64_t> m_ping_nonce{1};

    std::thread m_receive_thread;
    std::thread m_heartbeat_thread;
    std::mutex m_stop_mutex;
    bool m_stopped = false;
};

}  // namespace tunnel
}  // namespace go2cpp
