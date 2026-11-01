/**
 * @file sfu.cpp
 * @brief 多房间 SFU 实现：房间/peer/流/订阅索引、分片选择性转发与级联。
 * @details 核心不变量：
 *
 *          - 一个 `Stream` 归发布它的 peer 所有；peer 离开时整条流被标记
 *            dead，等转发结束后统一回收（`Cleanup()`），因此转发回调里调用
 *            `Leave`/`Unsubscribe` 不会让正在遍历的容器失效；
 *          - 订阅计数在**失活时**立即扣减，查询接口因此在回调内也能看到一致
 *            结果；真正的删除延后到 `forward_depth == 0`；
 *          - 转发热路径不做分配：`SfuFragment` 栈上构造，订阅列表按索引遍历。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/sfu.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::media {
namespace {

/// @brief (publisher, stream) 的扁平键；避免在每个分片上构造 pair/字符串。
std::uint64_t MakeStreamKey(SfuPeerId publisher, SfuStreamId stream) noexcept {
    return (static_cast<std::uint64_t>(publisher) << 32U) | static_cast<std::uint64_t>(stream);
}

}  // namespace

const char* SfuStatusName(SfuStatus status) noexcept {
    switch (status) {
        case SfuStatus::kOk:
            return "ok";
        case SfuStatus::kInvalidArgument:
            return "invalid-argument";
        case SfuStatus::kRoomExists:
            return "room-exists";
        case SfuStatus::kRoomNotFound:
            return "room-not-found";
        case SfuStatus::kPeerExists:
            return "peer-exists";
        case SfuStatus::kPeerNotFound:
            return "peer-not-found";
        case SfuStatus::kPeerLimitReached:
            return "peer-limit-reached";
        case SfuStatus::kStreamExists:
            return "stream-exists";
        case SfuStatus::kStreamNotFound:
            return "stream-not-found";
        case SfuStatus::kStreamLimitReached:
            return "stream-limit-reached";
        case SfuStatus::kSubscriptionExists:
            return "subscription-exists";
        case SfuStatus::kSubscriptionNotFound:
            return "subscription-not-found";
        case SfuStatus::kSubscriptionLimitReached:
            return "subscription-limit-reached";
        case SfuStatus::kSelfSubscription:
            return "self-subscription";
        case SfuStatus::kCascadeAlreadyRunning:
            return "cascade-already-running";
        case SfuStatus::kCascadeLoop:
            return "cascade-loop";
    }
    return "unknown";
}

struct SfuRouter::Impl {
    /// @brief 一条订阅。`active=false` 后等转发结束再释放。
    struct Subscription {
        SfuPeerId subscriber = 0;
        SfuFragmentSink sink;
        bool active = true;
        /// 是否还在等关键帧（仅在 config.require_keyframe_start 时置位）。
        bool waiting_key = false;
    };

    /// @brief 一条已发布的流及其订阅者。
    struct Stream {
        SfuPeerId publisher = 0;
        SfuStreamId stream = 0;
        /// deque：回调里新增订阅不会让已有元素的地址失效。
        std::deque<std::unique_ptr<Subscription>> subscriptions;
        SfuStreamStats stats;
        bool dead = false;
        int forward_depth = 0;
    };

    /// @brief 房间内参与者。
    struct Peer {
        std::set<SfuStreamId> published;
        std::size_t subscriptions = 0;  ///< active 订阅数。
        bool dead = false;
    };

    /// @brief 房间。
    struct Room {
        std::map<SfuPeerId, Peer> peers;
        std::map<std::uint64_t, std::unique_ptr<Stream>> streams;
        bool dead = false;
    };

    explicit Impl(SfuConfig cfg) : config(cfg) {}

    /// @brief 查找未删除的房间；公开查询接口都用它。
    Room* FindRoom(const SfuRoomId& room) {
        auto it = rooms.find(room);
        if (it == rooms.end() || it->second.dead) {
            return nullptr;
        }
        return &it->second;
    }

    const Room* FindRoom(const SfuRoomId& room) const {
        auto it = rooms.find(room);
        if (it == rooms.end() || it->second.dead) {
            return nullptr;
        }
        return &it->second;
    }

    Stream* FindStream(Room& room, SfuPeerId publisher, SfuStreamId stream) {
        auto it = room.streams.find(MakeStreamKey(publisher, stream));
        if (it == room.streams.end() || it->second->dead) {
            return nullptr;
        }
        return it->second.get();
    }

    const Stream* FindStream(const Room& room, SfuPeerId publisher, SfuStreamId stream) const {
        auto it = room.streams.find(MakeStreamKey(publisher, stream));
        if (it == room.streams.end() || it->second->dead) {
            return nullptr;
        }
        return it->second.get();
    }

    /// @brief 失活一条订阅：立即扣减计数，延迟释放。
    void ReleaseSubscription(Room& room, Subscription& subscription) {
        if (!subscription.active) {
            return;
        }
        subscription.active = false;
        auto peer = room.peers.find(subscription.subscriber);
        if (peer != room.peers.end() && peer->second.subscriptions > 0) {
            --peer->second.subscriptions;
        }
        ++stats.subscriptions_removed;
    }

    /// @brief 标记一条流为 dead，并失活其全部订阅。
    void MarkStreamDead(Room& room, Stream& stream) {
        if (stream.dead) {
            return;
        }
        stream.dead = true;
        for (auto& subscription : stream.subscriptions) {
            ReleaseSubscription(room, *subscription);
        }
        ++stats.streams_unpublished;
    }

    /// @brief 删除已经失活的订阅；只在没有转发进行时调用。
    static void CompactStream(Stream& stream) {
        auto& subscriptions = stream.subscriptions;
        for (auto it = subscriptions.begin(); it != subscriptions.end();) {
            if (!(*it)->active) {
                it = subscriptions.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// @brief 回收 dead 房间/流/peer 与失活订阅。
    void Cleanup() {
        if (forward_depth > 0) {
            cleanup_pending = true;
            return;
        }
        cleanup_pending = false;
        for (auto room_it = rooms.begin(); room_it != rooms.end();) {
            Room& room = room_it->second;
            if (room.dead) {
                room_it = rooms.erase(room_it);
                continue;
            }
            for (auto stream_it = room.streams.begin(); stream_it != room.streams.end();) {
                Stream& stream = *stream_it->second;
                if (stream.dead) {
                    stream_it = room.streams.erase(stream_it);
                    continue;
                }
                CompactStream(stream);
                ++stream_it;
            }
            for (auto peer_it = room.peers.begin(); peer_it != room.peers.end();) {
                if (peer_it->second.dead) {
                    peer_it = room.peers.erase(peer_it);
                } else {
                    ++peer_it;
                }
            }
            ++room_it;
        }
    }

    SfuConfig config;
    std::map<SfuRoomId, Room> rooms;
    SfuStats stats;
    /// 当前嵌套的转发层数（含级联回调）；>0 时禁止真正删除容器元素。
    int forward_depth = 0;
    bool cleanup_pending = false;
};

SfuRouter::SfuRouter(SfuConfig config) : m_impl(std::make_unique<Impl>(std::move(config))) {}

SfuRouter::~SfuRouter() = default;

SfuStatus SfuRouter::CreateRoom(const SfuRoomId& room) {
    if (room.empty()) {
        return SfuStatus::kInvalidArgument;
    }
    m_impl->Cleanup();
    if (m_impl->rooms.find(room) != m_impl->rooms.end()) {
        return SfuStatus::kRoomExists;
    }
    m_impl->rooms.try_emplace(room);
    ++m_impl->stats.rooms_created;
    return SfuStatus::kOk;
}

SfuStatus SfuRouter::RemoveRoom(const SfuRoomId& room) {
    auto it = m_impl->rooms.find(room);
    if (it == m_impl->rooms.end() || it->second.dead) {
        return SfuStatus::kRoomNotFound;
    }
    Impl::Room& state = it->second;
    for (auto& entry : state.streams) {
        m_impl->MarkStreamDead(state, *entry.second);
    }
    m_impl->stats.peers_left += state.peers.size();
    for (auto& peer : state.peers) {
        peer.second.dead = true;
    }
    state.dead = true;
    ++m_impl->stats.rooms_removed;
    m_impl->Cleanup();
    return SfuStatus::kOk;
}

bool SfuRouter::HasRoom(const SfuRoomId& room) const {
    return m_impl->FindRoom(room) != nullptr;
}

std::size_t SfuRouter::room_count() const noexcept {
    std::size_t count = 0;
    for (const auto& entry : m_impl->rooms) {
        if (!entry.second.dead) {
            ++count;
        }
    }
    return count;
}

std::vector<SfuRoomId> SfuRouter::rooms() const {
    std::vector<SfuRoomId> names;
    names.reserve(m_impl->rooms.size());
    for (const auto& entry : m_impl->rooms) {
        if (!entry.second.dead) {
            names.push_back(entry.first);
        }
    }
    return names;
}

SfuStatus SfuRouter::Join(const SfuRoomId& room, SfuPeerId peer) {
    if (peer == 0) {
        return SfuStatus::kInvalidArgument;
    }
    Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return SfuStatus::kRoomNotFound;
    }
    auto existing = state->peers.find(peer);
    if (existing != state->peers.end() && !existing->second.dead) {
        return SfuStatus::kPeerExists;
    }
    if (state->peers.size() >= m_impl->config.max_peers_per_room) {
        return SfuStatus::kPeerLimitReached;
    }
    state->peers[peer] = Impl::Peer{};
    ++m_impl->stats.peers_joined;
    return SfuStatus::kOk;
}

SfuStatus SfuRouter::Leave(const SfuRoomId& room, SfuPeerId peer) {
    Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return SfuStatus::kRoomNotFound;
    }
    auto peer_it = state->peers.find(peer);
    if (peer_it == state->peers.end() || peer_it->second.dead) {
        return SfuStatus::kPeerNotFound;
    }
    // 先清理它发布的流：订阅这些流的其他 peer 会被一并失活。
    const std::vector<SfuStreamId> published(peer_it->second.published.begin(),
                                             peer_it->second.published.end());
    for (const SfuStreamId stream : published) {
        auto stream_it = state->streams.find(MakeStreamKey(peer, stream));
        if (stream_it != state->streams.end()) {
            m_impl->MarkStreamDead(*state, *stream_it->second);
        }
    }
    peer_it->second.published.clear();

    // 再清理它作为订阅者的订阅。
    for (auto& entry : state->streams) {
        Impl::Stream& stream = *entry.second;
        if (stream.dead) {
            continue;
        }
        for (auto& subscription : stream.subscriptions) {
            if (subscription->active && subscription->subscriber == peer) {
                m_impl->ReleaseSubscription(*state, *subscription);
            }
        }
    }
    peer_it->second.dead = true;
    ++m_impl->stats.peers_left;
    m_impl->Cleanup();
    return SfuStatus::kOk;
}

bool SfuRouter::HasPeer(const SfuRoomId& room, SfuPeerId peer) const {
    const Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return false;
    }
    auto it = state->peers.find(peer);
    return it != state->peers.end() && !it->second.dead;
}

std::size_t SfuRouter::peer_count(const SfuRoomId& room) const {
    const Impl::Room* state = m_impl->FindRoom(room);
    return state == nullptr ? 0 : state->peers.size();
}

std::vector<SfuPeerId> SfuRouter::peers(const SfuRoomId& room) const {
    std::vector<SfuPeerId> ids;
    const Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return ids;
    }
    ids.reserve(state->peers.size());
    for (const auto& entry : state->peers) {
        ids.push_back(entry.first);
    }
    return ids;
}

SfuStatus SfuRouter::Publish(const SfuRoomId& room, SfuPeerId publisher, SfuStreamId stream) {
    if (stream == 0) {
        return SfuStatus::kInvalidArgument;
    }
    Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return SfuStatus::kRoomNotFound;
    }
    auto peer_it = state->peers.find(publisher);
    if (peer_it == state->peers.end() || peer_it->second.dead) {
        return SfuStatus::kPeerNotFound;
    }
    if (peer_it->second.published.size() >= m_impl->config.max_streams_per_peer) {
        return SfuStatus::kStreamLimitReached;
    }
    if (peer_it->second.published.count(stream) != 0) {
        return SfuStatus::kStreamExists;
    }
    auto existing = state->streams.find(MakeStreamKey(publisher, stream));
    if (existing != state->streams.end() && !existing->second->dead) {
        return SfuStatus::kStreamExists;
    }
    auto created = std::make_unique<Impl::Stream>();
    created->publisher = publisher;
    created->stream = stream;
    state->streams[MakeStreamKey(publisher, stream)] = std::move(created);
    peer_it->second.published.insert(stream);
    ++m_impl->stats.streams_published;
    return SfuStatus::kOk;
}

SfuStatus SfuRouter::Unpublish(const SfuRoomId& room, SfuPeerId publisher, SfuStreamId stream) {
    Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return SfuStatus::kRoomNotFound;
    }
    auto peer_it = state->peers.find(publisher);
    if (peer_it == state->peers.end() || peer_it->second.dead) {
        return SfuStatus::kPeerNotFound;
    }
    auto stream_it = state->streams.find(MakeStreamKey(publisher, stream));
    if (stream_it == state->streams.end() || stream_it->second->dead) {
        return SfuStatus::kStreamNotFound;
    }
    m_impl->MarkStreamDead(*state, *stream_it->second);
    peer_it->second.published.erase(stream);
    m_impl->Cleanup();
    return SfuStatus::kOk;
}

bool SfuRouter::HasStream(const SfuRoomId& room, SfuPeerId publisher, SfuStreamId stream) const {
    const Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return false;
    }
    return m_impl->FindStream(*state, publisher, stream) != nullptr;
}

std::size_t SfuRouter::stream_count(const SfuRoomId& room) const {
    const Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const auto& entry : state->streams) {
        if (!entry.second->dead) {
            ++count;
        }
    }
    return count;
}

std::vector<SfuStreamId> SfuRouter::streams(const SfuRoomId& room, SfuPeerId publisher) const {
    std::vector<SfuStreamId> ids;
    const Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return ids;
    }
    auto peer_it = state->peers.find(publisher);
    if (peer_it == state->peers.end()) {
        return ids;
    }
    ids.assign(peer_it->second.published.begin(), peer_it->second.published.end());
    return ids;
}

SfuStreamStats SfuRouter::stream_stats(const SfuRoomId& room, SfuPeerId publisher,
                                       SfuStreamId stream) const {
    const Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return SfuStreamStats{};
    }
    const Impl::Stream* found = m_impl->FindStream(*state, publisher, stream);
    return found == nullptr ? SfuStreamStats{} : found->stats;
}

SfuStatus SfuRouter::Subscribe(const SfuRoomId& room, SfuPeerId subscriber, SfuPeerId publisher,
                               SfuStreamId stream, SfuFragmentSink sink) {
    if (!sink) {
        return SfuStatus::kInvalidArgument;
    }
    if (subscriber == publisher) {
        return SfuStatus::kSelfSubscription;
    }
    Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return SfuStatus::kRoomNotFound;
    }
    auto subscriber_it = state->peers.find(subscriber);
    if (subscriber_it == state->peers.end() || subscriber_it->second.dead) {
        return SfuStatus::kPeerNotFound;
    }
    auto publisher_it = state->peers.find(publisher);
    if (publisher_it == state->peers.end() || publisher_it->second.dead) {
        return SfuStatus::kPeerNotFound;
    }
    if (subscriber_it->second.subscriptions >= m_impl->config.max_subscriptions_per_peer) {
        return SfuStatus::kSubscriptionLimitReached;
    }
    Impl::Stream* found = m_impl->FindStream(*state, publisher, stream);
    if (found == nullptr) {
        return SfuStatus::kStreamNotFound;
    }
    for (const auto& subscription : found->subscriptions) {
        if (subscription->active && subscription->subscriber == subscriber) {
            return SfuStatus::kSubscriptionExists;
        }
    }
    auto created = std::make_unique<Impl::Subscription>();
    created->subscriber = subscriber;
    created->sink = std::move(sink);
    created->waiting_key = m_impl->config.require_keyframe_start;
    found->subscriptions.push_back(std::move(created));
    ++subscriber_it->second.subscriptions;
    ++m_impl->stats.subscriptions_created;
    return SfuStatus::kOk;
}

SfuStatus SfuRouter::Unsubscribe(const SfuRoomId& room, SfuPeerId subscriber, SfuPeerId publisher,
                                 SfuStreamId stream) {
    Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return SfuStatus::kRoomNotFound;
    }
    Impl::Stream* found = m_impl->FindStream(*state, publisher, stream);
    if (found == nullptr) {
        return SfuStatus::kStreamNotFound;
    }
    for (auto& subscription : found->subscriptions) {
        if (subscription->active && subscription->subscriber == subscriber) {
            m_impl->ReleaseSubscription(*state, *subscription);
            m_impl->Cleanup();
            return SfuStatus::kOk;
        }
    }
    return SfuStatus::kSubscriptionNotFound;
}

std::size_t SfuRouter::subscription_count(const SfuRoomId& room) const {
    const Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const auto& entry : state->streams) {
        if (entry.second->dead) {
            continue;
        }
        for (const auto& subscription : entry.second->subscriptions) {
            if (subscription->active) {
                ++count;
            }
        }
    }
    return count;
}

std::size_t SfuRouter::subscription_count(const SfuRoomId& room, SfuPeerId subscriber) const {
    const Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return 0;
    }
    auto it = state->peers.find(subscriber);
    return it == state->peers.end() ? 0 : it->second.subscriptions;
}

std::size_t SfuRouter::subscriber_count(const SfuRoomId& room, SfuPeerId publisher,
                                        SfuStreamId stream) const {
    const Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        return 0;
    }
    const Impl::Stream* found = m_impl->FindStream(*state, publisher, stream);
    if (found == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const auto& subscription : found->subscriptions) {
        if (subscription->active) {
            ++count;
        }
    }
    return count;
}

std::size_t SfuRouter::ForwardFragment(const SfuRoomId& room, SfuPeerId publisher,
                                       SfuStreamId stream, const std::uint8_t* data,
                                       std::size_t bytes, std::uint8_t hops) {
    if (data == nullptr || bytes == 0) {
        ++m_impl->stats.fragments_malformed;
        return 0;
    }
    if (hops > m_impl->config.max_cascade_hops) {
        ++m_impl->stats.fragments_hop_limit;
        return 0;
    }
    // 热路径只做 SFU 必需的最小校验：帧头 + 视频子头。VideoChannel 的数据分片
    // 不会带捎带 ACK（kFlagAck），因此这里直接拒绝，省掉 ParseFrame 里为
    // 通用帧类型准备的长度/ACK/FEC 分支（实测转发吞吐 35.6→60.5 M 帧/s，
    // 见 docs/reports/asm-sfu.md）。
    if (bytes < go2cpp::bento::kFrameHeaderBytes + go2cpp::bento::kVideoHeaderBytes) {
        ++m_impl->stats.fragments_malformed;
        return 0;
    }
    go2cpp::bento::FrameHeader header;
    if (!go2cpp::bento::DecodeHeader(data, bytes, &header) ||
        header.type != go2cpp::bento::FrameType::kVideo ||
        header.channel != go2cpp::bento::kChannelVideo ||
        (header.flags & go2cpp::bento::kFlagAck) != 0) {
        ++m_impl->stats.fragments_malformed;
        return 0;
    }
    const std::uint8_t* payload = data + go2cpp::bento::kFrameHeaderBytes;
    const std::size_t payload_bytes = bytes - go2cpp::bento::kFrameHeaderBytes;
    go2cpp::bento::VideoFragmentHeader video_header;
    if (!go2cpp::bento::DecodeVideoHeader(payload, payload_bytes, &video_header) ||
        video_header.frag_count == 0 || video_header.frag_index >= video_header.frag_count ||
        video_header.frag_count > go2cpp::bento::VideoChannel::kMaxFragments ||
        payload_bytes < go2cpp::bento::kVideoHeaderBytes ||
        payload_bytes - go2cpp::bento::kVideoHeaderBytes >
            go2cpp::bento::VideoChannel::kFragmentBytes) {
        ++m_impl->stats.fragments_malformed;
        return 0;
    }

    Impl::Room* state = m_impl->FindRoom(room);
    if (state == nullptr) {
        ++m_impl->stats.fragments_unknown_stream;
        return 0;
    }
    Impl::Stream* found = m_impl->FindStream(*state, publisher, stream);
    if (found == nullptr) {
        ++m_impl->stats.fragments_unknown_stream;
        return 0;
    }

    ++m_impl->stats.fragments_received;
    m_impl->stats.bytes_received += bytes;
    if (video_header.frag_index == 0) {
        ++m_impl->stats.frames_received;
    }

    bool has_active = false;
    for (const auto& subscription : found->subscriptions) {
        if (subscription->active) {
            has_active = true;
            break;
        }
    }
    if (!has_active) {
        ++m_impl->stats.fragments_no_subscriber;
        return 0;
    }

    const bool key = (header.flags & go2cpp::bento::kFlagKey) != 0;
    SfuFragment fragment;
    fragment.publisher = publisher;
    fragment.stream = stream;
    fragment.frame_id = video_header.frame_id;
    fragment.frag_index = video_header.frag_index;
    fragment.frag_count = video_header.frag_count;
    fragment.key = key;
    fragment.hops = hops;
    fragment.data = data;
    fragment.bytes = bytes;
    fragment.header = header;
    fragment.payload = payload;
    fragment.payload_bytes = payload_bytes;

    ++found->forward_depth;
    ++m_impl->forward_depth;
    std::size_t delivered = 0;
    const std::size_t total = found->subscriptions.size();
    for (std::size_t index = 0; index < total; ++index) {
        Impl::Subscription* subscription = found->subscriptions[index].get();
        if (!subscription->active) {
            continue;
        }
        if (subscription->waiting_key) {
            // 必须从关键帧的**首片**开始，否则重组器缺前面的分片，等于把
            // 一个永远补不齐的帧交给订阅端。
            if (!key || video_header.frag_index != 0) {
                ++m_impl->stats.fragments_keyframe_gated;
                continue;
            }
            subscription->waiting_key = false;
        }
        subscription->sink(fragment);
        ++delivered;
    }
    --found->forward_depth;
    --m_impl->forward_depth;

    if (delivered > 0) {
        m_impl->stats.fragments_forwarded += delivered;
        m_impl->stats.bytes_forwarded += delivered * bytes;
        found->stats.fragments_forwarded += delivered;
        found->stats.bytes_forwarded += delivered * bytes;
        if (video_header.frag_index == 0) {
            ++m_impl->stats.frames_forwarded;
            ++found->stats.frames_forwarded;
        }
    }
    if (m_impl->forward_depth == 0 && m_impl->cleanup_pending) {
        m_impl->Cleanup();
    }
    return delivered;
}

const SfuStats& SfuRouter::stats() const noexcept {
    return m_impl->stats;
}

const SfuConfig& SfuRouter::config() const noexcept {
    return m_impl->config;
}

// ---------------------------------------------------------------------------
// SfuPublisher
// ---------------------------------------------------------------------------

struct SfuPublisher::Impl {
    Impl(SfuRouter& router_ref, SfuRoomId room_ref, SfuPeerId peer_ref, SfuStreamId stream_ref)
        : router(&router_ref),
          room(std::move(room_ref)),
          peer(peer_ref),
          stream(stream_ref),
          channel(std::make_unique<go2cpp::bento::VideoChannel>(
              [this](const std::uint8_t* data, std::size_t bytes) {
                  router->ForwardFragment(room, peer, stream, data, bytes, 0);
                  // 没有订阅者也算「发送成功」：SFU 丢弃是正常的选择性转发结果，
                  // 不能让 VideoChannel 把它计成 fragments_send_failed。
                  return true;
              },
              go2cpp::bento::VideoChannel::DeliverFn{})) {}

    SfuRouter* router = nullptr;
    SfuRoomId room;
    SfuPeerId peer = 0;
    SfuStreamId stream = 0;
    std::unique_ptr<go2cpp::bento::VideoChannel> channel;
    bool running = false;
    std::uint64_t frames_sent = 0;
};

SfuPublisher::SfuPublisher(SfuRouter& router, SfuRoomId room, SfuPeerId peer, SfuStreamId stream)
    : m_impl(std::make_unique<Impl>(router, std::move(room), peer, stream)) {}

SfuPublisher::~SfuPublisher() = default;

SfuStatus SfuPublisher::Start() {
    SfuStatus status = m_impl->router->Publish(m_impl->room, m_impl->peer, m_impl->stream);
    if (status == SfuStatus::kOk) {
        m_impl->running = true;
    }
    return status;
}

bool SfuPublisher::SendFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data,
                             std::size_t bytes) {
    if (!m_impl->running) {
        return false;
    }
    if (!m_impl->channel->SendFrame(frame_id, key, data, bytes)) {
        return false;
    }
    ++m_impl->frames_sent;
    return true;
}

const go2cpp::bento::VideoChannel::Stats& SfuPublisher::channel_stats() const noexcept {
    return m_impl->channel->stats();
}

std::uint64_t SfuPublisher::frames_sent() const noexcept {
    return m_impl->frames_sent;
}

// ---------------------------------------------------------------------------
// SfuSubscriber
// ---------------------------------------------------------------------------

struct SfuSubscriber::Impl {
    Impl(SfuRouter& router_ref, SfuRoomId room_ref, SfuPeerId self_ref, SfuPeerId publisher_ref,
         SfuStreamId stream_ref, FrameDeliverFn deliver_ref)
        : router(&router_ref),
          room(std::move(room_ref)),
          self(self_ref),
          publisher(publisher_ref),
          stream(stream_ref),
          deliver(std::move(deliver_ref)),
          channel(std::make_unique<go2cpp::bento::VideoChannel>(
              [](const std::uint8_t*, std::size_t) { return true; },
              [this](std::uint32_t frame_id, bool key, const std::uint8_t* data,
                     std::size_t bytes) {
                  if (deliver) {
                      deliver(frame_id, key, data, bytes);
                  }
              })) {}

    SfuRouter* router = nullptr;
    SfuRoomId room;
    SfuPeerId self = 0;
    SfuPeerId publisher = 0;
    SfuStreamId stream = 0;
    FrameDeliverFn deliver;
    std::unique_ptr<go2cpp::bento::VideoChannel> channel;
    bool subscribed = false;
};

SfuSubscriber::SfuSubscriber(SfuRouter& router, SfuRoomId room, SfuPeerId self, SfuPeerId publisher,
                             SfuStreamId stream, FrameDeliverFn deliver)
    : m_impl(std::make_unique<Impl>(router, std::move(room), self, publisher, stream,
                                    std::move(deliver))) {}

SfuSubscriber::~SfuSubscriber() {
    if (m_impl->subscribed) {
        m_impl->router->Unsubscribe(m_impl->room, m_impl->self, m_impl->publisher, m_impl->stream);
        m_impl->subscribed = false;
    }
    m_impl->channel.reset();
}

SfuStatus SfuSubscriber::Start() {
    SfuStatus status = m_impl->router->Subscribe(
        m_impl->room, m_impl->self, m_impl->publisher, m_impl->stream,
        [this](const SfuFragment& fragment) {
            m_impl->channel->OnFrame(fragment.header, fragment.payload, fragment.payload_bytes);
        });
    if (status == SfuStatus::kOk) {
        m_impl->subscribed = true;
    }
    return status;
}

void SfuSubscriber::OnTimer(std::uint64_t now_ms) {
    m_impl->channel->OnTimer(now_ms);
}

const go2cpp::bento::VideoChannel::Stats& SfuSubscriber::channel_stats() const noexcept {
    return m_impl->channel->stats();
}

std::uint64_t SfuSubscriber::frames_delivered() const noexcept {
    return m_impl->channel->stats().frames_delivered;
}

// ---------------------------------------------------------------------------
// SfuCascade
// ---------------------------------------------------------------------------

struct SfuCascade::Impl {
    explicit Impl(SfuCascadeConfig cfg) : config(std::move(cfg)) {}

    SfuCascadeConfig config;
    bool running = false;
    SfuCascadeStats stats;
};

SfuCascade::SfuCascade(SfuCascadeConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

SfuCascade::~SfuCascade() {
    Stop();
}

SfuStatus SfuCascade::Start() {
    if (m_impl->running) {
        return SfuStatus::kCascadeAlreadyRunning;
    }
    SfuCascadeConfig& config = m_impl->config;
    if (config.downstream == nullptr || config.upstream == nullptr ||
        config.downstream_room.empty() || config.upstream_room.empty() || config.bridge_peer == 0 ||
        config.publisher == 0 || config.stream == 0) {
        return SfuStatus::kInvalidArgument;
    }
    if (config.downstream == config.upstream) {
        return SfuStatus::kCascadeLoop;
    }
    if (config.bridge_peer == config.publisher) {
        return SfuStatus::kCascadeLoop;
    }
    if (!config.downstream->HasRoom(config.downstream_room)) {
        return SfuStatus::kRoomNotFound;
    }
    if (!config.downstream->HasPeer(config.downstream_room, config.bridge_peer) ||
        !config.downstream->HasPeer(config.downstream_room, config.publisher)) {
        return SfuStatus::kPeerNotFound;
    }
    if (!config.downstream->HasStream(config.downstream_room, config.publisher, config.stream)) {
        return SfuStatus::kStreamNotFound;
    }

    // 上游侧幂等准备：房间、桥接 peer、桥接流。
    SfuStatus status = config.upstream->CreateRoom(config.upstream_room);
    if (status != SfuStatus::kOk && status != SfuStatus::kRoomExists) {
        return status;
    }
    status = config.upstream->Join(config.upstream_room, config.bridge_peer);
    if (status != SfuStatus::kOk && status != SfuStatus::kPeerExists) {
        return status;
    }
    status = config.upstream->Publish(config.upstream_room, config.bridge_peer, config.stream);
    if (status != SfuStatus::kOk && status != SfuStatus::kStreamExists) {
        return status;
    }

    m_impl->running = true;
    status = config.downstream->Subscribe(
        config.downstream_room, config.bridge_peer, config.publisher, config.stream,
        [this](const SfuFragment& fragment) {
            if (!m_impl->running) {
                return;
            }
            const std::size_t delivered = m_impl->config.upstream->ForwardFragment(
                m_impl->config.upstream_room, m_impl->config.bridge_peer, m_impl->config.stream,
                fragment.data, fragment.bytes,
                static_cast<std::uint8_t>(
                    std::min<unsigned>(255U, static_cast<unsigned>(fragment.hops) + 1U)));
            if (delivered > 0) {
                ++m_impl->stats.fragments_forwarded;
                m_impl->stats.bytes_forwarded += fragment.bytes;
            } else {
                ++m_impl->stats.forward_failures;
            }
        });
    if (status != SfuStatus::kOk) {
        m_impl->running = false;
        return status;
    }
    return SfuStatus::kOk;
}

void SfuCascade::Stop() {
    if (!m_impl->running) {
        return;
    }
    m_impl->running = false;
    m_impl->config.downstream->Unsubscribe(m_impl->config.downstream_room,
                                           m_impl->config.bridge_peer, m_impl->config.publisher,
                                           m_impl->config.stream);
}

bool SfuCascade::running() const noexcept {
    return m_impl->running;
}

const SfuCascadeStats& SfuCascade::stats() const noexcept {
    return m_impl->stats;
}

}  // namespace go2cpp::media
