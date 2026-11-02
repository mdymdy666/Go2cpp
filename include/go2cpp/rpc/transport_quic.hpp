#pragma once

/**
 * @file transport_quic.hpp
 * @brief RPC over QUIC 风格传输：UDP + Bento 可靠层（多流复用 + 0-RTT/1-RTT）。
 *
 * @details
 * 本模块把 RPC 的字节流传输从 TCP 换到「QUIC 风格」的 UDP 可靠传输。协议栈
 * **不重写**：帧格式、SACK/快速重传/NewReno/BBR、流控（MAX_DATA）、分片重组、
 * PSK/X25519 加密与 0-RTT 全部复用 `go2cpp::bento` 的 `Link`
 * （`include/go2cpp/bento/net/link.hpp` + `channel.hpp` + `protocol.hpp`）。
 * 本层只补 Bento 没有的两件事：
 *
 * 1. **字节流语义**：`rpc::Transport` 要的是 `WriteAll`/`Read` 字节流，Bento
 *    的 STREAM 是「消息」语义（单条 <= 330KB）。`QuicStream` 把任意长度写入
 *    切成 `max_chunk_bytes` 的块逐条发送，接收侧按到达顺序拼回字节流；
 *    可靠通道本身保证消息有序，因此拼接不引入额外重排逻辑。
 * 2. **多流复用（call_id → stream）**：一条 UDP 连接（一个 `Link`）上开
 *    最多 8 条可靠通道（Bento 的 channel 就是 stream），每条通道由独立的
 *    `ReliableChannel` 维护 seq/重传/流控，因此**一条流的重传不会阻塞另一条
 *    流**（无 TCP 的队头阻塞）。RPC 层再在每个 stream 内部用 call_id 做
 *    并发调用复用，形成「连接 → stream → call」三级复用。
 *
 * ## 连接建立（0-RTT / 1-RTT）
 *
 * - **1-RTT**：`QuicClient::Dial` → `Link::Connect` 发 HELLO，收 HELLO_ACK
 *   后 Connected（Bento 的握手，含可选 X25519 + PSK 加密）。
 * - **0-RTT**：纯 PSK 会话下首次连接可 `ExportSessionTicket()`；下一次
 *   `Dial` 通过 `QuicOptions::session_ticket` 传入票据，内部走
 *   `Link::Resume`，**不发 HELLO**，首个数据帧即用恢复出的会话密钥发送。
 *   服务端 `Link` 通过 conn_id + 进程内会话缓存接受（见
 *   `bento/net/link.cpp` 的 `TryAdoptCachedSession`）。
 *
 * ## 服务端模型
 *
 * `QuicListener` 内部持有一个 `bento::Link`（监听方，单 peer）。客户端连上
 * 后，服务端在**收到某条 channel 的首个数据帧时**惰性创建一条 `QuicStream`
 * 并交给 `OnStream` 回调（通常直接 `rpc::Server::AddConnection`）。惰性创建
 * 让 1-RTT 与 0-RTT 恢复共用同一条路径：恢复后的会话没有 HELLO 事件，但首帧
 * 仍会到达。
 *
 * 注意：Bento `Link` 是单 peer 链路（一个 socket 一个对端）。需要「一个端口
 * 接 N 个客户端」时应使用 `bento::PeerHub`/`LinkServer`（线格式完全一致），
 * 本层暂不提供多 peer 后端；RPC 场景下每个客户端进程一个 QUIC 连接即可。
 *
 * ## 延迟相关的默认设置
 *
 * - `QuicMode::kLowLatency`（默认）关闭 BBR 与 pacing：回环/同机房链路下
 *   BBR 的初始 pacing 速率（768KB/s）远低于 RPC 小消息的实际速率，会把
 *   p50 拖到 130µs 以上；关闭后由 256 条发送窗口兜底。公网/弱网请显式选
 *   `kCongestionControl`。
 * - ACK 间隔 2ms（Bento 默认 20ms）。数据帧本身总是捎带 ACK，独立 ACK 只
 *   在接收队列抽干时补发，因此小请求-响应的确认不必等 ACK 定时器。
 * - 接收线程 `poll(1ms)`：有数据时内核立即唤醒，1ms 只是空转周期。
 * - 心跳 1s、断链判定 30s：RPC 空闲连接不应被 3s 的默认 peer_timeout 杀掉。
 *
 * ## 安全
 *
 * - `pre_shared_key` 非空时强制加密（PSK；`enable_x25519=true` 时带前向保密，
 *   但 X25519 会话不支持 0-RTT 票据，这是 Bento 的既定策略）；
 * - `pre_shared_key` 为空时走 Bento 的**明文模式**（`require_encryption=false`），
 *   仅供本机/可信内网压测与对照；生产必须配 PSK。
 *
 * ## 线程与生命周期
 *
 * - `QuicClient`/`QuicListener` 各自持有一个接收线程（驱动 `Link::RunOnce`）；
 * - `QuicStream::WriteAll` 可在任意线程并发调用（Bento 发送路径线程安全，
 *   但同一 `Link` 的发送会在其内部锁上串行）；
 * - `QuicStream::Read` 阻塞，`Close` 可从其他线程唤醒；
 * - 连接对象由 `QuicStream` 共享持有：`QuicClient` 析构不会让仍在读取的
 *   transport 悬空，只会把连接标记关闭并唤醒所有读者。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "go2cpp/rpc/status.hpp"
#include "go2cpp/rpc/transport.hpp"

namespace go2cpp::rpc {

class QuicConnection;
struct QuicStreamSlot;

/// @brief QUIC 传输的拥塞控制档位。
enum class QuicMode : std::uint8_t {
    /// 关闭 BBR/pacing（Bento `CongestionAlgorithm::kNone`）：回环/同机房
    /// 低延迟优先。发送窗口 256 条仍是硬上限，不会无界灌包。
    kLowLatency = 0,
    /// BBR + pacing（Bento 默认）：公网/弱网吞吐优先，代价是小消息 p50 会
    /// 被 pacing 令牌抬高（实测见 docs/modules/rpc-quic.md）。
    kCongestionControl = 1,
};

/// @brief QUIC 传输配置。
struct QuicOptions {
    /// 拥塞控制档位。
    QuicMode mode = QuicMode::kLowLatency;
    /// 32 字节 PSK；非空即启用加密（此时 `require_encryption` 强制为 true）。
    std::vector<std::uint8_t> pre_shared_key;
    /// 是否在加密握手中附加 X25519 临时公钥（前向保密）。0-RTT 票据要求
    /// 纯 PSK（`enable_x25519=false` + 服务端 `allow_legacy_peer`）。
    bool enable_x25519 = true;
    /// 是否允许 0-RTT 会话票据恢复。仅在纯 PSK 且客户端提供票据时生效。
    bool enable_0rtt = false;
    /// 明文模式开关。无 PSK 时本层会显式关闭 Bento 的 require_encryption，
    /// 启用明文——这是**不安全**的调试/压测模式。
    bool allow_plaintext = true;
    /// 客户端本地绑定地址（回环压测默认 127.0.0.1）。
    std::string bind_address = "127.0.0.1";
    /// 客户端 0-RTT 会话票据（首次连接后从 `QuicClient::session_ticket()` 取）。
    std::vector<std::uint8_t> session_ticket;
    /// 每条连接预开的 stream 数（1..8，Bento 单链路可靠通道上限 8）。
    std::uint8_t streams = 4;
    /// 接收线程每次 `RunOnce` 的 poll 超时（毫秒）。有数据会立即唤醒。
    std::uint32_t poll_timeout_ms = 1;
    /// 接收线程忙轮询（`RunOnce(0)` + yield）。延迟最低，代价是连接期间
    /// 一个核心接近满载；回环/延迟敏感场景可开，公网连接不要开。
    bool busy_poll = false;
    /// `Read` 在阻塞前自旋等待的微秒数（0 = 直接阻塞）。RPC 读线程的
    /// futex 唤醒在回环上要 5~15µs；短自旋等接收线程把数据推进队列后
    /// 直接返回，可省掉这一次唤醒。
    std::uint32_t read_spin_us = 0;
    /// 客户端不启后台接收线程，由 `QuicStream::Read` 直接驱动 `Link::RunOnce`
    /// 收包。省掉「接收线程 → RPC 读线程」的一次调度（回环实测 ~10µs），
    /// 代价是读线程在等待响应期间忙轮询。只对客户端、单 stream 生效；
    /// 服务端必须保留接收线程（要收 HELLO 与多 stream 数据）。
    bool inline_receive = false;
    /// 建立连接（HELLO/HELLO_ACK）超时。
    std::uint32_t connect_timeout_ms = 3000;
    /// 单次 `WriteAll` 等待发送窗口的最长时间。
    std::uint32_t send_timeout_ms = 5000;
    /// 多久收不到任何有效帧判定断链（RPC 空闲连接要留足）。
    std::uint32_t peer_timeout_ms = 30000;
    /// 心跳间隔（纯 ACK 帧，兼作 ACK 补发）。
    std::uint32_t heartbeat_interval_ms = 1000;
    /// 独立 ACK 的最小间隔（Bento 默认 20ms；数据帧本身捎带 ACK）。
    std::uint32_t ack_interval_ms = 2;
    /// `WriteAll` 单条 STREAM 消息的最大块大小（<= Bento 单条 330KB 上限）。
    std::size_t max_chunk_bytes = 16u * 1024u;
    /// 可靠通道流控窗口（RFC 9000 MAX_DATA 语义），必须双方一致。
    std::size_t initial_max_data = 64u * 1024u * 1024u;
    /// 0-RTT 会话票据有效期（Bento 侧）。
    std::uint32_t session_ticket_lifetime_ms = 60000;
};

/// @brief QUIC 传输的可观测计数器（线程安全快照）。
struct QuicStats {
    std::uint64_t streams_opened = 0;   ///< 创建过的 stream 数。
    std::uint64_t bytes_sent = 0;       ///< 应用层写入字节数（切片前）。
    std::uint64_t bytes_received = 0;   ///< 应用层读出字节数。
    std::uint64_t chunks_sent = 0;      ///< 发到 Bento STREAM 的消息数。
    std::uint64_t chunks_received = 0;  ///< 从 Bento STREAM 收到的消息数。
    std::uint64_t handshakes = 0;       ///< 完成的 1-RTT 握手数。
    std::uint64_t resumptions = 0;      ///< 0-RTT 恢复次数。
    std::uint64_t write_blocks = 0;     ///< `WriteAll` 遇到发送窗口满的次数。
    std::uint64_t retransmits = 0;      ///< Bento 可靠层重传次数。
    std::uint64_t frames_sent = 0;      ///< 链路层发出帧数。
    std::uint64_t frames_received = 0;  ///< 链路层收到帧数。
    std::uint64_t srtt_ms = 0;          ///< 平滑 RTT（毫秒）。
    std::uint64_t pto_ms = 0;           ///< 当前 PTO（毫秒）。
};

/// @brief 一条 QUIC stream 的传输实现（字节流，线程安全的写/阻塞读）。
class QuicStream final : public Transport {
   public:
    ~QuicStream() override;

    QuicStream(const QuicStream&) = delete;
    QuicStream& operator=(const QuicStream&) = delete;

    bool WriteAll(const void* data, std::size_t size, Status* status) override;
    std::ptrdiff_t Read(void* data, std::size_t size, Status* status) override;
    void Close() noexcept override;
    int fd() const noexcept override { return -1; }

    /// @brief stream 在本连接内的通道号（0..7）。
    std::uint8_t channel() const noexcept { return m_channel; }

    /// @brief 是否已关闭（本地 Close 或连接断开）。
    bool closed() const;

    /// @brief 本 stream 的收发计数快照。
    QuicStats stats() const;

   private:
    friend class QuicConnection;

    QuicStream(std::shared_ptr<QuicConnection> connection, std::uint8_t channel,
               std::shared_ptr<QuicStreamSlot> slot);
    void PushData(const std::uint8_t* data, std::size_t bytes);
    void MarkClosed();

    std::shared_ptr<QuicConnection> m_connection;   ///< 连接（保活）。
    std::shared_ptr<QuicStreamSlot> m_slot;        ///< 接收队列与计数。
    std::uint8_t m_channel = 0;                    ///< 通道号。
    std::uint32_t m_read_spin_us = 0;              ///< Read 阻塞前的自旋时长。
};

/// @brief QUIC 客户端：一条 UDP 连接 + N 条 stream。
class QuicClient {
   public:
    ~QuicClient();
    QuicClient(const QuicClient&) = delete;
    QuicClient& operator=(const QuicClient&) = delete;

    /**
     * @brief 建立到远端的 QUIC 风格连接。
     * @param target 服务端地址（`host:port`）。
     * @param options 配置。
     * @param status 失败状态（可为 nullptr）。
     * @return 成功返回对象；失败返回 nullptr。
     * @note `options.enable_0rtt` 且 `options.session_ticket` 非空时优先
     *       0-RTT 恢复，失败自动回退 1-RTT HELLO。
     */
    static std::unique_ptr<QuicClient> Dial(const Endpoint& target, const QuicOptions& options,
                                            Status* status);

    /// @brief 打开一条 stream；通道用尽返回 nullptr。
    std::unique_ptr<QuicStream> OpenStream(Status* status);

    /// @brief 是否已连接（1-RTT 完成或 0-RTT 恢复成功）。
    bool connected() const;
    /// @brief 当前会话是否来自 0-RTT 恢复。
    bool resumed() const;
    /// @brief 导出会话票据（仅纯 PSK 会话；不可用时为空）。
    std::vector<std::uint8_t> session_ticket() const;

    /// @brief 连接级统计快照。
    QuicStats stats() const;
    /// @brief 本地 UDP 端口（用于验证多 stream 复用同一五元组）。
    std::uint16_t local_port() const;
    /// @brief 远端地址文本。
    std::string peer_address() const;

    /// @brief 关闭连接并唤醒所有 stream 读者（幂等）。
    void Close();

   private:
    QuicClient();
    std::shared_ptr<QuicConnection> m_connection;
};

/// @brief QUIC 服务端：UDP 监听 + 按 stream 交付给 RPC Server。
class QuicListener {
   public:
    /// @brief 新 stream 回调（在接收线程内同步执行，必须快速返回）。
    using StreamHandler = std::function<void(std::unique_ptr<QuicStream>)>;

    QuicListener();
    ~QuicListener();
    QuicListener(const QuicListener&) = delete;
    QuicListener& operator=(const QuicListener&) = delete;

    /**
     * @brief 绑定 UDP 端口（不启动接收线程）。
     * @param bind 监听地址；host 为空表示 0.0.0.0。
     * @param options 配置；`streams` 决定每条连接可用的 stream 数。
     * @param status 失败状态。
     * @return 成功 true。实际端口用 `port()` 查询。
     */
    bool Listen(const Endpoint& bind, const QuicOptions& options, Status* status);
    /// @brief 设置新 stream 回调（应在 `Start` 之前）。
    void OnStream(StreamHandler handler);
    /// @brief 启动接收线程。
    bool Start();
    /// @brief 停止接收线程并关闭所有 stream（幂等）。
    void Stop();
    /// @brief 是否运行中。
    bool running() const;
    /// @brief 实际绑定端口；未监听返回 0。
    std::uint16_t port() const;
    /// @brief 连接级统计快照。
    QuicStats stats() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::rpc
