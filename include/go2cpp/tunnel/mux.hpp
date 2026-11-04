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

    /**
     * @brief 精确读取下一帧：只消费该帧的字节，不从连接预读。
     *
     * @param timeout_ms [in] 最长等待（毫秒）；<=0 立即超时
     * @param out [out] 成功时写入；失败时保持原值
     * @param err [out] 失败原因；成功时保持原值；可为 nullptr
     * @return 成功 true；超时 / 连接关闭 / 帧畸形返回 false
     *
     * @details `Next()` 一次 recv 8 KiB，适合握手阶段连发多帧；但 raw 数据
     *          连接在 HELLO_ACK 之后立刻切换为裸字节流，预读会把紧随其后的
     *          HTTP 请求头吞进 `FrameCodec` 内部缓冲并在析构时丢弃（表现为
     *          随机「请求缺头」或 `bad magic`）。本方法按帧头声明的长度
     *          精确读取，读完 ACK 后连接上不会少一个字节。
     */
    bool NextExact(int timeout_ms, frame::Frame* out, std::string* err);

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

    /**
     * @brief 接收线程投递一段数据；队列溢出返回 false（调用方应关闭该流）。
     * @param data [in] 数据首地址，借用（拷贝进队列后即可失效）。
     * @param length [in] 字节数；`0` 直接返回 true。
     * @param notify [in] 是否唤醒读线程。批量投递同一流的连续帧时，只有最后
     *             一帧传 true，把「每帧一次 futex 唤醒」摊薄为「每批一次」。
     * @return 队列未溢出返回 true；溢出返回 false（调用方应 FailStream）。
     */
    bool PushData(const std::uint8_t* data, std::size_t length,
                  bool notify = true);
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

    /// raw 流回调：在**接收线程**里调用 `(stream_id, peer_ip)`；实现必须
    /// 立即返回（例如起一条工作线程去拨数据连接）。
    using OnRawOpen = std::function<void(std::uint32_t, const std::string&)>;

    /// 设置 raw 流回调；必须在 `Start()` 之前调用（接收线程启动后只读）。
    void set_on_raw_open(OnRawOpen handler) { m_on_raw_open = std::move(handler); }

    /**
     * @brief 分配一个流 id（发送 `kOpenRaw` 前用）。
     * @return 流 id；会话已关闭 / 正在 drain 时返回 0。
     * @note 分配后必须紧接 `SendOpenRaw`，否则 id 空洞（对端不会知道）。
     */
    std::uint32_t AllocateStreamId();

    /**
     * @brief 通知对端「这个流走 per-stream 裸 TCP 数据通道」（不创建 MuxStream）。
     * @param stream_id [in] 已由 `AllocateStreamId()` 分配的 id；0 返回 false。
     * @param peer_ip [in] 公网对端 IP（仅展示）。
     * @param peer_port [in] 公网对端端口。
     * @return 帧已写出返回 true；连接不可用 / 编码失败返回 false。
     */
    bool SendOpenRaw(std::uint32_t stream_id, const std::string& peer_ip = {},
                     std::uint16_t peer_port = 0);

    /**
     * @brief 把底层连接从会话里摘出来（per-stream 裸 TCP 数据通道用）。
     *
     * @return 被摘出的连接；调用后会话不再持有它，`Shutdown()` 不会
     *         `shutdown` 这条连接的 fd——所有权移交给调用方，由调用方
     *         负责关闭。
     * @note 仅用于**尚未 `Start()`** 的连接（无接收/心跳线程）；线程安全。
     */
    TcpConn::Ptr DetachConn();

    /// 发送一帧（线程安全）；返回 false 表示连接已不可用。
    bool SendPayload(FrameType type, const std::vector<std::uint8_t>& payload);

    /**
     * @brief 把一段流数据切成 DATA 帧，用一次 writev 批量发出（合并写）。
     *
     * @param stream_id [in] 目标流 id；由本端分配，对端按它分发。
     * @param data [in] 载荷首地址；`size == 0` 时可为 nullptr，否则不可为 nullptr。
     *            只在本次调用期间被读取（直接进 iovec，不复制）。
     * @param size [in] 字节数；按 `kMaxDataChunk` 切帧，单次最多
     *            `kMaxSendBatchFrames` 帧（约 1 MiB），超出部分连续分批。
     * @return 全部字节写完返回 true；连接不可用 / 写入失败返回 false。
     *
     * ## 定位
     * 隧道数据面唯一的出站提交点：一次 syscall 写多帧，同时把发送互斥量的
     * 持有次数从「每帧一次」降到「每批一次」。做错了会出现帧交错（对端解码出
     * 错误流）或半批丢失（流卡死），因此这里必须整批持锁。
     *
     * ## 步骤
     * step1. 逐帧计算 body 长度（末帧可能不足 `kMaxDataChunk`），帧头写进栈上
     *         缓冲，body 直接指向 `data`（零拷贝）。
     * step2. 组 iovec：每帧两段（头 + body），最多 `2 * kMaxSendBatchFrames` 段。
     * step3. 持 `m_send_mutex` 调 `TcpConn::WriteV`；失败即整条连接不可用。
     * step4. 推进 offset 到下一批，直到写完 `size`。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程；与 `SendPayload` 共用 `m_send_mutex`，帧不会交错。
     * - 原子性：一批内的多帧由 `WriteV` 保证字节流连续；跨批不保证。
     * - 失败语义：不抛异常；返回 false 后调用方必须按连接失效处理（关流）。
     * - 不变量：返回 true 时 `m_bytes_out` 已累加全部线上字节（含帧头）。
     * - 前提：仅在 `with_checksum == false` 时可用；启用校验时帧尾需要 CRC，
     *   调用方必须回退到 `SendPayload`（逐帧 `FrameCodec::Encode`）。
     */
    bool SendData(std::uint32_t stream_id, const char* data, std::size_t size);

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

    /// 底层控制连接（按值返回：`DetachConn()` 会并发清空 `m_conn`）。
    TcpConn::Ptr conn() const {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        return m_conn;
    }

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
    /**
     * @brief 处理一帧；DATA 帧按 `notify_data` 决定是否唤醒该流读线程。
     *
     * @param frame [in] 已解出的完整帧（payload 借用，调用期间有效）。
     * @param notify_data [in] DATA 帧入队后是否 `notify_one`；批量投递同一流的
     *        连续帧时，非末帧传 false 可把唤醒次数从每帧一次降到每批一次。
     *        非 DATA 帧忽略此参数。
     *
     * ## 定位
     * 接收线程唯一的帧分派点；`notify` 传错的最坏后果是读线程延迟到下一帧
     * 才被唤醒（性能问题），不会丢数据——队列本身是持锁入队的。
     */
    void Dispatch(const frame::Frame& frame, bool notify_data = true);
    void RemoveStream(std::uint32_t id);
    /// 从表里删除并标记对端关闭 + 发 CLOSE 帧（队列溢出等异常路径）。
    void FailStream(std::uint32_t id, const char* reason);
    void MarkAllStreamsClosed(const char* reason);
    static std::int64_t NowMs();

    mutable std::mutex m_conn_mutex;
    TcpConn::Ptr m_conn;
    MuxConfig m_config;
    OnOpen m_on_open;
    OnRawOpen m_on_raw_open;

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
