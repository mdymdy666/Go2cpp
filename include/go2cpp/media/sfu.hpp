#pragma once

/**
 * @file sfu.hpp
 * @brief 多房间选择性转发（SFU）：房间/参与者、发布/订阅、分片转发与级联。
 * @details 本模块补上 WebRTC 媒体链路里「一对多」的那一段：发送端仍然用
 *          `VideoChannel` 把一帧拆成 <=1200B 的 Bento 视频分片，SFU 只做
 *          **按订阅关系转发**，不做全广播：
 *
 *          ```
 *          SfuPublisher(peer 1) --VideoChannel 分片--> SfuRouter --订阅--> SfuSubscriber(peer 3)
 *                                                     \--订阅--> SfuSubscriber(peer 4)
 *          ```
 *
 *          设计要点（为什么这样取舍）：
 *
 *          1. **转发单位是分片，不是整帧**。SFU 不需要解码/重编码，转发时只
 *             解析 Bento 帧头与 8B 视频子头，把原始字节（含帧头）交给订阅者；
 *             订阅者各自用 `VideoChannel::OnFrame` 重组。这样 SFU 的内存占用
 *             与帧大小无关，也天然兼容 FEC / 后续编码格式变化。
 *          2. **选择性转发靠流索引**。`(publisher, stream)` 映射到一份订阅者
 *             列表；没有订阅者的流在入口直接丢弃，不会遍历房间里的所有 peer。
 *          3. **中途订阅等关键帧**（`SfuConfig::require_keyframe_start`）。差分
 *             编码下从 P 帧开始解码必然花屏，真实 SFU 都从下一个关键帧开始转发；
 *             本实现按分片帧头的 key 标志判断，默认开启。
 *          4. **回调里可安全退订**。转发过程中收到的 Unsubscribe/Leave 只做
 *             标记与计数，真正的容器删除延迟到转发结束（`forward_depth`），
 *             避免 sink 在回调中退订自己导致迭代器/指针失效。
 *          5. **级联用 hop 计数兜底**。`SfuCascade` 把一个下游房间的流转发到
 *             上游 SFU；多级级联时每个分片带 hop 计数，超过
 *             `SfuConfig::max_cascade_hops` 丢弃，防止配置成环后无限循环。
 *
 *          线程模型：单线程（与 `VideoChannel`、`UdpVideoReceiver` 的 pump
 *          模型一致）。所有 `ForwardFragment` / 订阅变更应在同一线程调用；
 *          本模块不加锁。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/media/transport.hpp"

namespace go2cpp::media {

/// @brief 房间内 peer 标识（同一房间内唯一；不同房间可以重复使用）。
using SfuPeerId = std::uint32_t;

/// @brief 一个 peer 发布的流标识（同一 peer 内唯一）。
using SfuStreamId = std::uint32_t;

/// @brief 房间标识（UTF-8 字符串，便于日志与配置；转发热路径按值哈希）。
using SfuRoomId = std::string;

/// @brief SFU 操作结果。
enum class SfuStatus : std::uint8_t {
    kOk = 0,                    ///< 成功。
    kInvalidArgument,           ///< 参数非法（空房间名、空 sink 等）。
    kRoomExists,                ///< 房间已存在。
    kRoomNotFound,              ///< 房间不存在。
    kPeerExists,                ///< peer 已在房间内。
    kPeerNotFound,              ///< peer 不在房间内。
    kPeerLimitReached,          ///< 房间 peer 数达到上限。
    kStreamExists,              ///< 该 peer 已发布同名流。
    kStreamNotFound,            ///< 流不存在。
    kStreamLimitReached,        ///< 该 peer 的流数达到上限。
    kSubscriptionExists,        ///< 重复订阅。
    kSubscriptionNotFound,      ///< 订阅不存在。
    kSubscriptionLimitReached,  ///< 订阅者订阅数达到上限。
    kSelfSubscription,  ///< 订阅自己发布的流（应本地直通，不占 SFU 带宽）。
    kCascadeAlreadyRunning,  ///< 级联已启动。
    kCascadeLoop,            ///< 级联方向非法（指向自己或无法建立）。
};

/// @brief SFU 状态名（日志/测试断言用）。
const char* SfuStatusName(SfuStatus status) noexcept;

/// @brief 转发给订阅者的一个分片视图。
/// @details `data/bytes` 指向调用方提供的完整 Bento 分片，生命周期只到 sink
///          返回为止；`header/payload` 是已经解析好的视图，订阅端可以直接
///          `VideoChannel::OnFrame(header, payload, payload_bytes)` 重组。
struct SfuFragment {
    SfuPeerId publisher = 0;                ///< 发布者 peer id。
    SfuStreamId stream = 0;                 ///< 流 id。
    std::uint32_t frame_id = 0;             ///< 视频帧号（来自视频子头）。
    std::uint16_t frag_index = 0;           ///< 分片下标（0 起）。
    std::uint16_t frag_count = 1;           ///< 分片总数。
    bool key = false;                       ///< 本帧是否关键帧。
    std::uint8_t hops = 0;                  ///< 已经过几级 SFU 级联。
    const std::uint8_t* data = nullptr;     ///< 完整 Bento 分片（含帧头）。
    std::size_t bytes = 0;                  ///< 分片字节数。
    go2cpp::bento::FrameHeader header{};    ///< 解析后的 Bento 帧头。
    const std::uint8_t* payload = nullptr;  ///< 视频子头 + 数据（供 OnFrame）。
    std::size_t payload_bytes = 0;          ///< payload 字节数。
};

/// @brief 订阅回调：每个被转发的分片调用一次。
using SfuFragmentSink = std::function<void(const SfuFragment& fragment)>;

/// @brief SFU 配置。
struct SfuConfig {
    /// 单房间 peer 上限（防单个房间打满内存）。
    std::size_t max_peers_per_room = 64;
    /// 单 peer 可发布的流数上限。
    std::size_t max_streams_per_peer = 8;
    /// 单 peer 可订阅的流数上限（含跨 peer）。
    std::size_t max_subscriptions_per_peer = 64;
    /// 新订阅是否等到下一个关键帧才开始转发（差分编码必须）。
    bool require_keyframe_start = true;
    /// 级联最大跳数；分片 hops 超过该值直接丢弃（防止级联成环）。
    std::uint8_t max_cascade_hops = 3;
};

/// @brief 单个流的转发统计。
struct SfuStreamStats {
    std::uint64_t frames_forwarded = 0;     ///< 至少投递给一个订阅者的帧数。
    std::uint64_t fragments_forwarded = 0;  ///< 投递次数（N 个订阅者算 N 次）。
    std::uint64_t bytes_forwarded = 0;      ///< 投递字节数（按订阅者累计）。
};

/// @brief SFU 全量统计。
struct SfuStats {
    std::uint64_t rooms_created = 0;
    std::uint64_t rooms_removed = 0;
    std::uint64_t peers_joined = 0;
    std::uint64_t peers_left = 0;
    std::uint64_t streams_published = 0;
    std::uint64_t streams_unpublished = 0;
    std::uint64_t subscriptions_created = 0;
    std::uint64_t subscriptions_removed = 0;
    std::uint64_t fragments_received = 0;        ///< 解析成功的分片。
    std::uint64_t fragments_forwarded = 0;       ///< 实际投递次数。
    std::uint64_t fragments_no_subscriber = 0;   ///< 有流但无订阅者。
    std::uint64_t fragments_unknown_stream = 0;  ///< 房间/流不存在。
    std::uint64_t fragments_malformed = 0;       ///< 非 Bento 视频分片。
    std::uint64_t fragments_keyframe_gated = 0;  ///< 等待关键帧被挡下的分片。
    std::uint64_t fragments_hop_limit = 0;       ///< 级联跳数超限被丢弃。
    std::uint64_t frames_received = 0;           ///< 收到首分片（frag_index==0）的帧数。
    std::uint64_t frames_forwarded = 0;          ///< 至少转发给一个订阅者的帧数。
    std::uint64_t bytes_received = 0;
    std::uint64_t bytes_forwarded = 0;
};

/**
 * @brief 多房间 SFU 路由器。
 *
 * @details 生命周期：房间、peer、流、订阅都由 `SfuRouter` 拥有；订阅回调
 *          （`SfuFragmentSink`）由调用方负责保证其捕获的对象比订阅活得久
 *          （`SfuSubscriber` 在析构时会自动退订）。
 */
class SfuRouter {
   public:
    explicit SfuRouter(SfuConfig config = {});
    ~SfuRouter();

    SfuRouter(const SfuRouter&) = delete;
    SfuRouter& operator=(const SfuRouter&) = delete;

    // --- 房间 ---
    SfuStatus CreateRoom(const SfuRoomId& room);
    /// @brief 删除房间（同时清理其所有 peer/流/订阅）。
    SfuStatus RemoveRoom(const SfuRoomId& room);
    bool HasRoom(const SfuRoomId& room) const;
    std::size_t room_count() const noexcept;
    std::vector<SfuRoomId> rooms() const;

    // --- 参与者 ---
    SfuStatus Join(const SfuRoomId& room, SfuPeerId peer);
    /// @brief peer 离开：清理它发布的流，以及它作为订阅者的全部订阅。
    SfuStatus Leave(const SfuRoomId& room, SfuPeerId peer);
    bool HasPeer(const SfuRoomId& room, SfuPeerId peer) const;
    std::size_t peer_count(const SfuRoomId& room) const;
    std::vector<SfuPeerId> peers(const SfuRoomId& room) const;

    // --- 发布 ---
    SfuStatus Publish(const SfuRoomId& room, SfuPeerId publisher, SfuStreamId stream);
    SfuStatus Unpublish(const SfuRoomId& room, SfuPeerId publisher, SfuStreamId stream);
    bool HasStream(const SfuRoomId& room, SfuPeerId publisher, SfuStreamId stream) const;
    std::size_t stream_count(const SfuRoomId& room) const;
    std::vector<SfuStreamId> streams(const SfuRoomId& room, SfuPeerId publisher) const;
    SfuStreamStats stream_stats(const SfuRoomId& room, SfuPeerId publisher,
                                SfuStreamId stream) const;

    // --- 订阅 ---
    SfuStatus Subscribe(const SfuRoomId& room, SfuPeerId subscriber, SfuPeerId publisher,
                        SfuStreamId stream, SfuFragmentSink sink);
    SfuStatus Unsubscribe(const SfuRoomId& room, SfuPeerId subscriber, SfuPeerId publisher,
                          SfuStreamId stream);
    std::size_t subscription_count(const SfuRoomId& room) const;
    std::size_t subscription_count(const SfuRoomId& room, SfuPeerId subscriber) const;
    std::size_t subscriber_count(const SfuRoomId& room, SfuPeerId publisher,
                                 SfuStreamId stream) const;

    /**
     * @brief 转发一个完整的 Bento 视频分片。
     * @param data 完整分片（16B 帧头 + 8B 视频子头 + 数据）。
     * @param hops 已经过的级联跳数；普通发布者传 0，级联链路传上一跳 + 1。
     * @return 实际投递的订阅者数量（0 表示无人订阅/非法/被挡下）。
     * @note 非 Bento 视频帧、超长帧、视频子头非法都计入 fragments_malformed，
     *       不抛异常。
     */
    std::size_t ForwardFragment(const SfuRoomId& room, SfuPeerId publisher, SfuStreamId stream,
                                const std::uint8_t* data, std::size_t bytes, std::uint8_t hops = 0);

    const SfuStats& stats() const noexcept;
    const SfuConfig& config() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * @brief 发布端：把 `VideoChannel` 的分片直接喂给 SFU。
 *
 * @details 典型用法：
 * ```cpp
 * router.Join("room", 1);
 * SfuPublisher publisher(router, "room", 1, 100);
 * publisher.Start();
 * publisher.SendFrame(frame_id, key, encoded.data(), encoded.size());
 * ```
 * 必须在 `SfuRouter` 之后析构。
 */
class SfuPublisher {
   public:
    SfuPublisher(SfuRouter& router, SfuRoomId room, SfuPeerId peer, SfuStreamId stream);
    ~SfuPublisher();

    SfuPublisher(const SfuPublisher&) = delete;
    SfuPublisher& operator=(const SfuPublisher&) = delete;

    /// @brief 在路由器上注册流（peer 必须已经 Join）。
    SfuStatus Start();

    /// @brief 经 `VideoChannel` 分片后转发；语义同 `VideoChannel::SendFrame`。
    bool SendFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes);

    const go2cpp::bento::VideoChannel::Stats& channel_stats() const noexcept;

    /// @brief 已调用 SendFrame 且未超过分片上限的帧数。
    std::uint64_t frames_sent() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * @brief 订阅端：把 SFU 转发的分片交给私有 `VideoChannel` 重组。
 *
 * @details 一个 `SfuSubscriber` 只对应一条 `(publisher, stream)` 订阅；
 *          订阅多个流就创建多个实例（各自的 frame_id 空间互相独立，这也
 *          避免了不同发布者 frame_id 混在一个重组器里的歧义）。
 *          必须在 `SfuRouter` 之后析构（析构时会自动 Unsubscribe）。
 */
class SfuSubscriber {
   public:
    SfuSubscriber(SfuRouter& router, SfuRoomId room, SfuPeerId self, SfuPeerId publisher,
                  SfuStreamId stream, FrameDeliverFn deliver);
    ~SfuSubscriber();

    SfuSubscriber(const SfuSubscriber&) = delete;
    SfuSubscriber& operator=(const SfuSubscriber&) = delete;

    /// @brief 建立订阅；成功后开始接收分片。
    SfuStatus Start();

    /// @brief 驱动重组超时清理（缺片 1s 后丢弃，同 VideoChannel）。
    void OnTimer(std::uint64_t now_ms);

    const go2cpp::bento::VideoChannel::Stats& channel_stats() const noexcept;

    /// @brief 已重组并交付的完整帧数。
    std::uint64_t frames_delivered() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @brief 级联配置：把下游房间某条流转发到上游 SFU 的同名流。
struct SfuCascadeConfig {
    SfuRouter* downstream = nullptr;  ///< 下游 SFU（发布者所在）。
    SfuRoomId downstream_room;        ///< 下游房间。
    SfuPeerId publisher = 0;          ///< 下游发布者。
    SfuStreamId stream = 0;           ///< 流 id（上下游同名）。
    SfuPeerId bridge_peer = 0;        ///< 在两端的桥接 peer id（不能等于 publisher）。
    SfuRouter* upstream = nullptr;    ///< 上游 SFU（订阅者所在）。
    SfuRoomId upstream_room;          ///< 上游房间（不存在则自动创建）。
};

/// @brief 级联统计。
struct SfuCascadeStats {
    std::uint64_t fragments_forwarded = 0;
    std::uint64_t bytes_forwarded = 0;
    std::uint64_t forward_failures = 0;
};

/**
 * @brief 两个 SFU 之间的单向级联链路。
 *
 * @details `Start()` 会：
 *          1. 校验下游房间/桥接 peer/发布者/流都存在；
 *          2. 在上游房间建房、加入桥接 peer、发布同名流（幂等）；
 *          3. 在下游订阅 `(publisher, stream)`，回调里以 `hops + 1` 转发到上游。
 *
 *          级联必须保持**单向无环**；`SfuConfig::max_cascade_hops` 是最后一道
 *          保险，成环时最多绕 3 跳即被丢弃并计数。
 */
class SfuCascade {
   public:
    explicit SfuCascade(SfuCascadeConfig config);
    ~SfuCascade();

    SfuCascade(const SfuCascade&) = delete;
    SfuCascade& operator=(const SfuCascade&) = delete;

    SfuStatus Start();
    void Stop();
    bool running() const noexcept;
    const SfuCascadeStats& stats() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::media
