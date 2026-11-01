/**
 * @file test_sfu.cpp
 * @brief SFU 测试：单房间 4 peer 发布/订阅、多房间隔离、peer 离开清理、
 *        关键帧门控、级联、异常输入与 1e6 帧转发吞吐。
 * @details 断言使用 `media_test_util.hpp` 的轻量宏（不依赖 gtest），
 *          失败会打印文件/行号，进程最终以非零码退出，CTest 可直接接入。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/media/sfu.hpp"
#include "media_test_util.hpp"

namespace {

using go2cpp::media::SfuCascade;
using go2cpp::media::SfuCascadeConfig;
using go2cpp::media::SfuConfig;
using go2cpp::media::SfuFragment;
using go2cpp::media::SfuPeerId;
using go2cpp::media::SfuPublisher;
using go2cpp::media::SfuRouter;
using go2cpp::media::SfuStatus;
using go2cpp::media::SfuStreamId;
using go2cpp::media::SfuSubscriber;

/// @brief 一帧的交付记录。
struct Delivered {
    std::vector<std::uint32_t> frame_ids;
    std::vector<bool> keys;
    std::vector<std::vector<std::uint8_t>> payloads;
};

/// @brief 生成可校验的帧内容。
std::vector<std::uint8_t> MakePayload(std::size_t bytes, std::uint8_t seed,
                                      std::uint32_t frame_id) {
    std::vector<std::uint8_t> data(bytes, 0);
    for (std::size_t index = 0; index < bytes; ++index) {
        data[index] = static_cast<std::uint8_t>((index * 31U + seed * 7U + frame_id * 13U) & 0xFFU);
    }
    return data;
}

/// @brief 采集一组帧的期望内容。
std::vector<std::vector<std::uint8_t>> MakeFrames(std::size_t count, std::size_t base_bytes,
                                                  std::size_t step_bytes, std::uint8_t seed) {
    std::vector<std::vector<std::uint8_t>> frames;
    frames.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        frames.push_back(MakePayload(base_bytes + index * step_bytes, seed,
                                     static_cast<std::uint32_t>(index + 1)));
    }
    return frames;
}

/// @brief 交付回调：把完整帧记录到 Delivered。
go2cpp::media::FrameDeliverFn MakeSink(Delivered* delivered) {
    return
        [delivered](std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes) {
            delivered->frame_ids.push_back(frame_id);
            delivered->keys.push_back(key);
            delivered->payloads.emplace_back(data, data + bytes);
        };
}

/// @brief 用 VideoChannel 生成一个单分片 Bento 视频帧（供吞吐测试复用）。
std::vector<std::uint8_t> MakeSingleFragment(std::uint32_t frame_id, bool key,
                                             std::size_t payload_bytes) {
    std::vector<std::uint8_t> captured;
    go2cpp::bento::VideoChannel channel(
        [&captured](const std::uint8_t* data, std::size_t bytes) {
            captured.assign(data, data + bytes);
            return true;
        },
        go2cpp::bento::VideoChannel::DeliverFn{});
    const std::vector<std::uint8_t> payload = MakePayload(payload_bytes, 9, frame_id);
    if (!channel.SendFrame(frame_id, key, payload.data(), payload.size())) {
        return {};
    }
    return captured;
}

/// @brief 分片数（与 VideoChannel 的 1200B 规则一致）。
std::size_t FragmentCount(std::size_t bytes) {
    return bytes == 0 ? 1 : (bytes + 1199) / 1200;
}

// ---------------------------------------------------------------------------
// 1. 单房间 4 peer：2 发布 2 订阅，选择性转发 + VideoChannel 端到端
// ---------------------------------------------------------------------------
void TestSingleRoomFourPeers() {
    SfuRouter router;
    MEDIA_CHECK_EQ(router.CreateRoom("room-1"), SfuStatus::kOk);
    for (SfuPeerId peer = 1; peer <= 4; ++peer) {
        MEDIA_CHECK_EQ(router.Join("room-1", peer), SfuStatus::kOk);
    }
    MEDIA_CHECK_EQ(router.peer_count("room-1"), std::size_t{4});

    SfuPublisher publisher1(router, "room-1", 1, 100);
    SfuPublisher publisher2(router, "room-1", 2, 200);
    MEDIA_CHECK_EQ(publisher1.Start(), SfuStatus::kOk);
    MEDIA_CHECK_EQ(publisher2.Start(), SfuStatus::kOk);

    Delivered delivered3;
    Delivered delivered4;
    SfuSubscriber subscriber3(router, "room-1", 3, 1, 100, MakeSink(&delivered3));
    SfuSubscriber subscriber4(router, "room-1", 4, 2, 200, MakeSink(&delivered4));
    MEDIA_CHECK_EQ(subscriber3.Start(), SfuStatus::kOk);
    MEDIA_CHECK_EQ(subscriber4.Start(), SfuStatus::kOk);

    // 订阅 1 的只有 3，订阅 2 的只有 4：任何跨流投递都会被下面的断言抓到。
    MEDIA_CHECK_EQ(router.subscriber_count("room-1", 1, 100), std::size_t{1});
    MEDIA_CHECK_EQ(router.subscriber_count("room-1", 2, 200), std::size_t{1});
    MEDIA_CHECK_EQ(router.subscriber_count("room-1", 1, 200), std::size_t{0});

    const std::vector<std::vector<std::uint8_t>> frames1 = MakeFrames(20, 300, 37, 0x11);
    const std::vector<std::vector<std::uint8_t>> frames2 = MakeFrames(12, 1500, 211, 0x22);

    std::size_t expected_fragments = 0;
    for (std::size_t index = 0; index < frames1.size(); ++index) {
        const bool key = index == 0;
        MEDIA_CHECK(publisher1.SendFrame(static_cast<std::uint32_t>(index + 1), key,
                                         frames1[index].data(), frames1[index].size()));
        expected_fragments += FragmentCount(frames1[index].size());
    }
    for (std::size_t index = 0; index < frames2.size(); ++index) {
        const bool key = index == 0;
        MEDIA_CHECK(publisher2.SendFrame(static_cast<std::uint32_t>(index + 1), key,
                                         frames2[index].data(), frames2[index].size()));
        expected_fragments += FragmentCount(frames2[index].size());
    }

    MEDIA_CHECK_EQ(subscriber3.frames_delivered(), std::uint64_t{20});
    MEDIA_CHECK_EQ(subscriber4.frames_delivered(), std::uint64_t{12});
    MEDIA_CHECK_EQ(delivered3.payloads.size(), frames1.size());
    MEDIA_CHECK_EQ(delivered4.payloads.size(), frames2.size());

    bool matched = delivered3.payloads.size() == frames1.size();
    for (std::size_t index = 0; matched && index < frames1.size(); ++index) {
        matched = delivered3.payloads[index] == frames1[index];
        matched = matched && delivered3.frame_ids[index] == index + 1;
    }
    MEDIA_CHECK(matched);
    matched = delivered4.payloads.size() == frames2.size();
    for (std::size_t index = 0; matched && index < frames2.size(); ++index) {
        matched = delivered4.payloads[index] == frames2[index];
        matched = matched && delivered4.frame_ids[index] == index + 1;
    }
    MEDIA_CHECK(matched);

    // 发布端分片数 = 收到的分片数；每个分片各投递 1 次。
    MEDIA_CHECK_EQ(router.stats().fragments_received,
                   static_cast<std::uint64_t>(expected_fragments));
    MEDIA_CHECK_EQ(router.stats().fragments_forwarded,
                   static_cast<std::uint64_t>(expected_fragments));
    MEDIA_CHECK_EQ(router.stats().frames_forwarded, std::uint64_t{32});
    MEDIA_CHECK_EQ(router.stats().fragments_no_subscriber, std::uint64_t{0});
    MEDIA_CHECK_EQ(router.stream_stats("room-1", 1, 100).frames_forwarded, std::uint64_t{20});
    MEDIA_CHECK_EQ(router.stream_stats("room-1", 2, 200).frames_forwarded, std::uint64_t{12});
    std::printf("[INFO] room-1: 4 peers, 2 publishers, 2 subscribers, %zu fragments\n",
                expected_fragments);
}

// ---------------------------------------------------------------------------
// 2. 多房间隔离：相同 peer id / stream id 在不同房间互不影响
// ---------------------------------------------------------------------------
void TestMultiRoomIsolation() {
    SfuRouter router;
    MEDIA_CHECK_EQ(router.CreateRoom("room-a"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.CreateRoom("room-b"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.CreateRoom("room-a"), SfuStatus::kRoomExists);

    for (const char* room : {"room-a", "room-b"}) {
        MEDIA_CHECK_EQ(router.Join(room, 1), SfuStatus::kOk);
        MEDIA_CHECK_EQ(router.Join(room, 2), SfuStatus::kOk);
    }
    MEDIA_CHECK_EQ(router.room_count(), std::size_t{2});

    SfuPublisher publisher_a(router, "room-a", 1, 7);
    SfuPublisher publisher_b(router, "room-b", 1, 7);
    MEDIA_CHECK_EQ(publisher_a.Start(), SfuStatus::kOk);
    MEDIA_CHECK_EQ(publisher_b.Start(), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.stream_count("room-a"), std::size_t{1});
    MEDIA_CHECK_EQ(router.stream_count("room-b"), std::size_t{1});

    Delivered delivered_a;
    Delivered delivered_b;
    SfuSubscriber subscriber_a(router, "room-a", 2, 1, 7, MakeSink(&delivered_a));
    SfuSubscriber subscriber_b(router, "room-b", 2, 1, 7, MakeSink(&delivered_b));
    MEDIA_CHECK_EQ(subscriber_a.Start(), SfuStatus::kOk);
    MEDIA_CHECK_EQ(subscriber_b.Start(), SfuStatus::kOk);

    const std::vector<std::vector<std::uint8_t>> frames_a = MakeFrames(6, 400, 50, 0xA1);
    const std::vector<std::vector<std::uint8_t>> frames_b = MakeFrames(9, 500, 40, 0xB2);
    for (std::size_t index = 0; index < frames_a.size(); ++index) {
        MEDIA_CHECK(publisher_a.SendFrame(static_cast<std::uint32_t>(index + 1), index == 0,
                                          frames_a[index].data(), frames_a[index].size()));
    }
    for (std::size_t index = 0; index < frames_b.size(); ++index) {
        MEDIA_CHECK(publisher_b.SendFrame(static_cast<std::uint32_t>(index + 1), index == 0,
                                          frames_b[index].data(), frames_b[index].size()));
    }
    MEDIA_CHECK_EQ(delivered_a.payloads.size(), frames_a.size());
    MEDIA_CHECK_EQ(delivered_b.payloads.size(), frames_b.size());
    bool isolated = true;
    const std::size_t compare_a = std::min(delivered_a.payloads.size(), frames_a.size());
    for (std::size_t index = 0; index < compare_a && isolated; ++index) {
        isolated = delivered_a.payloads[index] == frames_a[index];
    }
    const std::size_t compare_b = std::min(delivered_b.payloads.size(), frames_b.size());
    for (std::size_t index = 0; index < compare_b && isolated; ++index) {
        isolated = delivered_b.payloads[index] == frames_b[index];
    }
    MEDIA_CHECK(isolated);
    MEDIA_CHECK_EQ(router.stream_stats("room-a", 1, 7).frames_forwarded, std::uint64_t{6});
    MEDIA_CHECK_EQ(router.stream_stats("room-b", 1, 7).frames_forwarded, std::uint64_t{9});

    // 房间 A 的离开不影响房间 B 的同名 peer/流。
    MEDIA_CHECK_EQ(router.Leave("room-a", 1), SfuStatus::kOk);
    MEDIA_CHECK(router.HasPeer("room-b", 1));
    MEDIA_CHECK(router.HasStream("room-b", 1, 7));
    MEDIA_CHECK(!router.HasStream("room-a", 1, 7));
    MEDIA_CHECK_EQ(router.stream_count("room-a"), std::size_t{0});
    MEDIA_CHECK_EQ(router.stream_count("room-b"), std::size_t{1});
    MEDIA_CHECK_EQ(router.subscription_count("room-a"), std::size_t{0});
    MEDIA_CHECK_EQ(router.subscription_count("room-b"), std::size_t{1});
}

// ---------------------------------------------------------------------------
// 3. peer 离开清理（含转发回调里离开的可重入场景）
// ---------------------------------------------------------------------------
void TestPeerLeaveCleanup() {
    SfuRouter router;
    MEDIA_CHECK_EQ(router.CreateRoom("clean"), SfuStatus::kOk);
    for (SfuPeerId peer = 1; peer <= 4; ++peer) {
        MEDIA_CHECK_EQ(router.Join("clean", peer), SfuStatus::kOk);
    }
    MEDIA_CHECK_EQ(router.Publish("clean", 1, 100), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Publish("clean", 2, 200), SfuStatus::kOk);
    std::size_t to_stream_100 = 0;
    std::size_t to_stream_200 = 0;
    auto sink3a = [&to_stream_100](const SfuFragment&) { ++to_stream_100; };
    auto sink3b = [&to_stream_200](const SfuFragment&) { ++to_stream_200; };
    auto sink4a = [](const SfuFragment&) {};
    MEDIA_CHECK_EQ(router.Subscribe("clean", 3, 1, 100, sink3a), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Subscribe("clean", 3, 2, 200, sink3b), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Subscribe("clean", 4, 1, 100, sink4a), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.subscription_count("clean"), std::size_t{3});
    MEDIA_CHECK_EQ(router.subscription_count("clean", 3), std::size_t{2});
    MEDIA_CHECK_EQ(router.subscription_count("clean", 4), std::size_t{1});

    // 离开发布者 1：它的流被删除，订阅它的 3/4 各失去一条订阅。
    MEDIA_CHECK_EQ(router.Leave("clean", 1), SfuStatus::kOk);
    MEDIA_CHECK(!router.HasPeer("clean", 1));
    MEDIA_CHECK(!router.HasStream("clean", 1, 100));
    MEDIA_CHECK_EQ(router.stream_count("clean"), std::size_t{1});
    MEDIA_CHECK_EQ(router.subscription_count("clean"), std::size_t{1});
    MEDIA_CHECK_EQ(router.subscription_count("clean", 3), std::size_t{1});
    MEDIA_CHECK_EQ(router.subscription_count("clean", 4), std::size_t{0});
    MEDIA_CHECK_EQ(router.peer_count("clean"), std::size_t{3});

    const std::vector<std::uint8_t> payload = MakePayload(700, 5, 1);
    const std::vector<std::uint8_t> fragment = MakeSingleFragment(1, true, payload.size());
    MEDIA_CHECK_EQ(router.ForwardFragment("clean", 2, 200, fragment.data(), fragment.size()),
                   std::size_t{1});
    MEDIA_CHECK_EQ(to_stream_200, std::size_t{1});
    MEDIA_CHECK_EQ(to_stream_100, std::size_t{0});
    MEDIA_CHECK_EQ(router.ForwardFragment("clean", 1, 100, fragment.data(), fragment.size()),
                   std::size_t{0});

    MEDIA_CHECK_EQ(router.Leave("clean", 3), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.subscription_count("clean"), std::size_t{0});
    MEDIA_CHECK_EQ(router.Leave("clean", 4), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Leave("clean", 2), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.peer_count("clean"), std::size_t{0});
    MEDIA_CHECK_EQ(router.RemoveRoom("clean"), SfuStatus::kOk);
    MEDIA_CHECK(!router.HasRoom("clean"));
    MEDIA_CHECK_EQ(router.room_count(), std::size_t{0});

    // 重新建房/加入/发布/订阅，验证清理后状态可复用。
    MEDIA_CHECK_EQ(router.CreateRoom("clean"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Join("clean", 1), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Join("clean", 3), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Publish("clean", 1, 100), SfuStatus::kOk);
    std::size_t revived = 0;
    MEDIA_CHECK_EQ(
        router.Subscribe("clean", 3, 1, 100, [&revived](const SfuFragment&) { ++revived; }),
        SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.ForwardFragment("clean", 1, 100, fragment.data(), fragment.size()),
                   std::size_t{1});
    MEDIA_CHECK_EQ(revived, std::size_t{1});

    // 可重入：订阅回调里让发布者离开，不能破坏正在遍历的容器。
    SfuRouter reentrant;
    MEDIA_CHECK_EQ(reentrant.CreateRoom("re"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(reentrant.Join("re", 1), SfuStatus::kOk);
    MEDIA_CHECK_EQ(reentrant.Join("re", 2), SfuStatus::kOk);
    MEDIA_CHECK_EQ(reentrant.Publish("re", 1, 1), SfuStatus::kOk);
    int callbacks = 0;
    MEDIA_CHECK_EQ(reentrant.Subscribe("re", 2, 1, 1,
                                       [&reentrant, &callbacks](const SfuFragment&) {
                                           ++callbacks;
                                           reentrant.Leave("re", 1);
                                       }),
                   SfuStatus::kOk);
    MEDIA_CHECK_EQ(reentrant.ForwardFragment("re", 1, 1, fragment.data(), fragment.size()),
                   std::size_t{1});
    MEDIA_CHECK_EQ(callbacks, 1);
    MEDIA_CHECK(!reentrant.HasPeer("re", 1));
    MEDIA_CHECK_EQ(reentrant.stream_count("re"), std::size_t{0});
    MEDIA_CHECK_EQ(reentrant.subscription_count("re"), std::size_t{0});
    MEDIA_CHECK_EQ(reentrant.peer_count("re"), std::size_t{1});
}

// ---------------------------------------------------------------------------
// 4. 关键帧门控、重复订阅、上限与退订
// ---------------------------------------------------------------------------
void TestSubscriptionLifecycle() {
    SfuConfig config;
    config.require_keyframe_start = true;
    SfuRouter router(config);
    MEDIA_CHECK_EQ(router.CreateRoom("gate"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Join("gate", 1), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Join("gate", 2), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Join("gate", 3), SfuStatus::kOk);

    SfuPublisher publisher(router, "gate", 1, 100);
    MEDIA_CHECK_EQ(publisher.Start(), SfuStatus::kOk);
    std::size_t gated_sink_calls = 0;
    MEDIA_CHECK_EQ(
        router.Subscribe("gate", 2, 1, 100,
                         [&gated_sink_calls](const SfuFragment&) { ++gated_sink_calls; }),
        SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Subscribe("gate", 2, 1, 100, [](const SfuFragment&) {}),
                   SfuStatus::kSubscriptionExists);
    MEDIA_CHECK_EQ(router.Subscribe("gate", 2, 2, 100, [](const SfuFragment&) {}),
                   SfuStatus::kSelfSubscription);
    MEDIA_CHECK_EQ(router.Subscribe("gate", 2, 3, 100, [](const SfuFragment&) {}),
                   SfuStatus::kStreamNotFound);
    MEDIA_CHECK_EQ(router.Subscribe("gate", 3, 1, 100, nullptr), SfuStatus::kInvalidArgument);
    MEDIA_CHECK_EQ(router.Subscribe("missing", 2, 1, 100, [](const SfuFragment&) {}),
                   SfuStatus::kRoomNotFound);
    MEDIA_CHECK_EQ(router.Subscribe("gate", 9, 1, 100, [](const SfuFragment&) {}),
                   SfuStatus::kPeerNotFound);

    const std::vector<std::uint8_t> p_frame = MakePayload(640, 1, 1);
    MEDIA_CHECK(publisher.SendFrame(1, false, p_frame.data(), p_frame.size()));
    MEDIA_CHECK_EQ(gated_sink_calls, std::size_t{0});
    MEDIA_CHECK_EQ(router.stats().fragments_keyframe_gated, std::uint64_t{1});

    const std::vector<std::uint8_t> key_frame = MakePayload(640, 2, 2);
    MEDIA_CHECK(publisher.SendFrame(2, true, key_frame.data(), key_frame.size()));
    MEDIA_CHECK_EQ(gated_sink_calls, std::size_t{1});
    MEDIA_CHECK_EQ(router.stats().fragments_keyframe_gated, std::uint64_t{1});

    MEDIA_CHECK_EQ(router.Unsubscribe("gate", 2, 1, 100), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Unsubscribe("gate", 2, 1, 100), SfuStatus::kSubscriptionNotFound);
    MEDIA_CHECK(publisher.SendFrame(3, true, key_frame.data(), key_frame.size()));
    MEDIA_CHECK_EQ(gated_sink_calls, std::size_t{1});
    MEDIA_CHECK_EQ(router.stats().fragments_no_subscriber, std::uint64_t{1});

    // 重新订阅后仍从下一个关键帧开始。
    std::size_t resubscribed = 0;
    MEDIA_CHECK_EQ(router.Subscribe("gate", 2, 1, 100,
                                    [&resubscribed](const SfuFragment&) { ++resubscribed; }),
                   SfuStatus::kOk);
    MEDIA_CHECK(publisher.SendFrame(4, false, p_frame.data(), p_frame.size()));
    MEDIA_CHECK_EQ(resubscribed, std::size_t{0});
    MEDIA_CHECK(publisher.SendFrame(5, true, key_frame.data(), key_frame.size()));
    MEDIA_CHECK_EQ(resubscribed, std::size_t{1});

    // 上限：peer / stream / subscription。
    SfuConfig limits;
    limits.max_peers_per_room = 2;
    limits.max_streams_per_peer = 2;
    limits.max_subscriptions_per_peer = 1;
    SfuRouter limited(limits);
    MEDIA_CHECK_EQ(limited.CreateRoom("limits"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(limited.Join("limits", 1), SfuStatus::kOk);
    MEDIA_CHECK_EQ(limited.Join("limits", 2), SfuStatus::kOk);
    MEDIA_CHECK_EQ(limited.Join("limits", 3), SfuStatus::kPeerLimitReached);
    MEDIA_CHECK_EQ(limited.Publish("limits", 1, 100), SfuStatus::kOk);
    MEDIA_CHECK_EQ(limited.Publish("limits", 1, 101), SfuStatus::kOk);
    MEDIA_CHECK_EQ(limited.Publish("limits", 1, 102), SfuStatus::kStreamLimitReached);
    MEDIA_CHECK_EQ(limited.Subscribe("limits", 2, 1, 100, [](const SfuFragment&) {}),
                   SfuStatus::kOk);
    MEDIA_CHECK_EQ(limited.Subscribe("limits", 2, 1, 101, [](const SfuFragment&) {}),
                   SfuStatus::kSubscriptionLimitReached);
    MEDIA_CHECK_EQ(limited.Unpublish("limits", 1, 100), SfuStatus::kOk);
    MEDIA_CHECK_EQ(limited.Unpublish("limits", 1, 100), SfuStatus::kStreamNotFound);
    MEDIA_CHECK_EQ(limited.ForwardFragment("limits", 1, 100, nullptr, 0), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 5. 级联：下游发布者 → 上游订阅者，hop 上限兜底
// ---------------------------------------------------------------------------
void TestCascade() {
    SfuRouter upstream;
    SfuRouter downstream;
    MEDIA_CHECK_EQ(upstream.CreateRoom("up"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(upstream.Join("up", 20), SfuStatus::kOk);
    MEDIA_CHECK_EQ(downstream.CreateRoom("down"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(downstream.Join("down", 10), SfuStatus::kOk);
    MEDIA_CHECK_EQ(downstream.Join("down", 1), SfuStatus::kOk);
    MEDIA_CHECK_EQ(downstream.Publish("down", 10, 1), SfuStatus::kOk);

    Delivered upstream_delivered;

    SfuCascadeConfig cascade_config;
    cascade_config.downstream = &downstream;
    cascade_config.downstream_room = "down";
    cascade_config.publisher = 10;
    cascade_config.stream = 1;
    cascade_config.bridge_peer = 1;
    cascade_config.upstream = &upstream;
    cascade_config.upstream_room = "up";
    SfuCascade cascade(cascade_config);
    MEDIA_CHECK_EQ(cascade.Start(), SfuStatus::kOk);
    MEDIA_CHECK(cascade.running());
    MEDIA_CHECK_EQ(cascade.Start(), SfuStatus::kCascadeAlreadyRunning);
    MEDIA_CHECK_EQ(upstream.Publish("up", 1, 1), SfuStatus::kStreamExists);
    MEDIA_CHECK_EQ(upstream.stream_count("up"), std::size_t{1});

    // 级联 Start 会先在上游建房/加入桥接 peer/发布桥接流，订阅者必须在这之后再订阅。
    SfuSubscriber upstream_subscriber(upstream, "up", 20, 1, 1, MakeSink(&upstream_delivered));
    MEDIA_CHECK_EQ(upstream_subscriber.Start(), SfuStatus::kOk);

    // 流已在上面手工发布过，这里直接用底层 VideoChannel 发送。
    const std::vector<std::vector<std::uint8_t>> frames = MakeFrames(10, 350, 25, 0x33);
    go2cpp::bento::VideoChannel channel(
        [&downstream](const std::uint8_t* data, std::size_t bytes) {
            downstream.ForwardFragment("down", 10, 1, data, bytes, 0);
            return true;
        },
        go2cpp::bento::VideoChannel::DeliverFn{});
    for (std::size_t index = 0; index < frames.size(); ++index) {
        MEDIA_CHECK(channel.SendFrame(static_cast<std::uint32_t>(index + 1), index == 0,
                                      frames[index].data(), frames[index].size()));
    }
    MEDIA_CHECK_EQ(upstream_delivered.payloads.size(), frames.size());
    bool matched = upstream_delivered.payloads.size() == frames.size();
    for (std::size_t index = 0; matched && index < frames.size(); ++index) {
        matched = upstream_delivered.payloads[index] == frames[index];
    }
    MEDIA_CHECK(matched);
    MEDIA_CHECK_GE(cascade.stats().fragments_forwarded, std::uint64_t{10});
    MEDIA_CHECK_EQ(cascade.stats().forward_failures, std::uint64_t{0});

    cascade.Stop();
    MEDIA_CHECK(!cascade.running());
    const std::size_t delivered_before = upstream_delivered.payloads.size();
    MEDIA_CHECK(channel.SendFrame(11, true, frames.front().data(), frames.front().size()));
    MEDIA_CHECK_EQ(upstream_delivered.payloads.size(), delivered_before);
    MEDIA_CHECK_EQ(cascade.Start(), SfuStatus::kOk);

    // 反向/自环/缺房间都拒绝。
    SfuCascadeConfig loop_config = cascade_config;
    loop_config.upstream = &downstream;
    SfuCascade loop(loop_config);
    MEDIA_CHECK_EQ(loop.Start(), SfuStatus::kCascadeLoop);

    SfuCascadeConfig bridge_self = cascade_config;
    bridge_self.bridge_peer = bridge_self.publisher;
    SfuCascade self_bridge(bridge_self);
    MEDIA_CHECK_EQ(self_bridge.Start(), SfuStatus::kCascadeLoop);

    SfuCascadeConfig missing = cascade_config;
    missing.downstream_room = "missing";
    SfuCascade missing_room(missing);
    MEDIA_CHECK_EQ(missing_room.Start(), SfuStatus::kRoomNotFound);

    // hop 上限：直接以 hops > max_cascade_hops 调转发，必须丢弃并计数。
    SfuConfig hop_config;
    hop_config.max_cascade_hops = 3;
    SfuRouter hop_router(hop_config);
    MEDIA_CHECK_EQ(hop_router.CreateRoom("hop"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(hop_router.Join("hop", 1), SfuStatus::kOk);
    MEDIA_CHECK_EQ(hop_router.Join("hop", 2), SfuStatus::kOk);
    MEDIA_CHECK_EQ(hop_router.Publish("hop", 1, 1), SfuStatus::kOk);
    std::size_t hops_delivered = 0;
    MEDIA_CHECK_EQ(hop_router.Subscribe(
                       "hop", 2, 1, 1, [&hops_delivered](const SfuFragment&) { ++hops_delivered; }),
                   SfuStatus::kOk);
    const std::vector<std::uint8_t> fragment = MakeSingleFragment(1, true, 300);
    MEDIA_CHECK_EQ(hop_router.ForwardFragment("hop", 1, 1, fragment.data(), fragment.size(), 4),
                   std::size_t{0});
    MEDIA_CHECK_EQ(hops_delivered, std::size_t{0});
    MEDIA_CHECK_EQ(hop_router.stats().fragments_hop_limit, std::uint64_t{1});
    MEDIA_CHECK_EQ(hop_router.ForwardFragment("hop", 1, 1, fragment.data(), fragment.size(), 3),
                   std::size_t{1});
    MEDIA_CHECK_EQ(hops_delivered, std::size_t{1});
}

// ---------------------------------------------------------------------------
// 6. 异常输入与统计
// ---------------------------------------------------------------------------
void TestMalformedAndStats() {
    SfuRouter router;
    MEDIA_CHECK_EQ(router.CreateRoom(""), SfuStatus::kInvalidArgument);
    MEDIA_CHECK_EQ(router.CreateRoom("m"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Join("m", 0), SfuStatus::kInvalidArgument);
    MEDIA_CHECK_EQ(router.Join("missing", 1), SfuStatus::kRoomNotFound);
    MEDIA_CHECK_EQ(router.Join("m", 1), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Join("m", 1), SfuStatus::kPeerExists);
    MEDIA_CHECK_EQ(router.Join("m", 2), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Publish("m", 1, 0), SfuStatus::kInvalidArgument);
    MEDIA_CHECK_EQ(router.Publish("m", 9, 1), SfuStatus::kPeerNotFound);
    MEDIA_CHECK_EQ(router.Publish("m", 1, 1), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Publish("m", 1, 1), SfuStatus::kStreamExists);
    MEDIA_CHECK_EQ(router.Unpublish("m", 9, 1), SfuStatus::kPeerNotFound);
    MEDIA_CHECK_EQ(router.Unpublish("m", 1, 9), SfuStatus::kStreamNotFound);
    MEDIA_CHECK_EQ(router.RemoveRoom("missing"), SfuStatus::kRoomNotFound);
    MEDIA_CHECK_EQ(router.Leave("missing", 1), SfuStatus::kRoomNotFound);
    MEDIA_CHECK_EQ(router.Leave("m", 9), SfuStatus::kPeerNotFound);
    MEDIA_CHECK_EQ(router.Unsubscribe("m", 2, 1, 1), SfuStatus::kSubscriptionNotFound);
    MEDIA_CHECK_EQ(router.subscriber_count("missing", 1, 1), std::size_t{0});
    MEDIA_CHECK_EQ(router.stream_count("missing"), std::size_t{0});
    MEDIA_CHECK_EQ(router.peer_count("missing"), std::size_t{0});
    MEDIA_CHECK(router.rooms().size() == 1);

    const std::uint8_t garbage[17] = {0xFF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                      0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    MEDIA_CHECK_EQ(router.ForwardFragment("m", 1, 1, nullptr, 0), std::size_t{0});
    MEDIA_CHECK_EQ(router.stats().fragments_malformed, std::uint64_t{1});
    MEDIA_CHECK_EQ(router.ForwardFragment("m", 1, 1, garbage, sizeof(garbage)), std::size_t{0});
    MEDIA_CHECK_EQ(router.stats().fragments_malformed, std::uint64_t{2});

    go2cpp::bento::FrameHeader ack_header;
    ack_header.type = go2cpp::bento::FrameType::kAck;
    ack_header.channel = 0;
    ack_header.seq = 1;
    const std::vector<std::uint8_t> ack =
        go2cpp::bento::BuildFrame(ack_header, nullptr, nullptr, 0);
    MEDIA_CHECK_EQ(router.ForwardFragment("m", 1, 1, ack.data(), ack.size()), std::size_t{0});
    MEDIA_CHECK_EQ(router.stats().fragments_malformed, std::uint64_t{3});

    const std::vector<std::uint8_t> fragment = MakeSingleFragment(1, true, 600);
    MEDIA_CHECK_EQ(router.ForwardFragment("missing", 1, 1, fragment.data(), fragment.size()),
                   std::size_t{0});
    MEDIA_CHECK_EQ(router.ForwardFragment("m", 2, 1, fragment.data(), fragment.size()),
                   std::size_t{0});
    MEDIA_CHECK_EQ(router.stats().fragments_unknown_stream, std::uint64_t{2});

    // 有流但无订阅者：计入 no_subscriber，不投递。
    MEDIA_CHECK_EQ(router.ForwardFragment("m", 1, 1, fragment.data(), fragment.size()),
                   std::size_t{0});
    MEDIA_CHECK_EQ(router.stats().fragments_no_subscriber, std::uint64_t{1});
    MEDIA_CHECK_EQ(router.stats().fragments_received, std::uint64_t{1});
    MEDIA_CHECK_EQ(router.stats().frames_received, std::uint64_t{1});

    std::size_t received = 0;
    MEDIA_CHECK_EQ(router.Subscribe("m", 2, 1, 1,
                                    [&received](const SfuFragment& fragment_view) {
                                        ++received;
                                        MEDIA_CHECK_EQ(fragment_view.publisher, 1U);
                                        MEDIA_CHECK_EQ(fragment_view.stream, 1U);
                                        MEDIA_CHECK_EQ(fragment_view.frag_index, 0);
                                        MEDIA_CHECK_EQ(fragment_view.frag_count, 1);
                                        MEDIA_CHECK(fragment_view.key);
                                        MEDIA_CHECK(fragment_view.data != nullptr);
                                        MEDIA_CHECK(fragment_view.payload != nullptr);
                                    }),
                   SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.ForwardFragment("m", 1, 1, fragment.data(), fragment.size()),
                   std::size_t{1});
    MEDIA_CHECK_EQ(received, std::size_t{1});
    MEDIA_CHECK_EQ(router.stats().fragments_forwarded, std::uint64_t{1});
    MEDIA_CHECK_EQ(router.stats().bytes_forwarded, static_cast<std::uint64_t>(fragment.size()));

    // 状态名映射（日志可读性）：每个枚举都有非空字符串。
    const SfuStatus statuses[] = {SfuStatus::kOk,
                                  SfuStatus::kInvalidArgument,
                                  SfuStatus::kRoomExists,
                                  SfuStatus::kRoomNotFound,
                                  SfuStatus::kPeerExists,
                                  SfuStatus::kPeerNotFound,
                                  SfuStatus::kPeerLimitReached,
                                  SfuStatus::kStreamExists,
                                  SfuStatus::kStreamNotFound,
                                  SfuStatus::kStreamLimitReached,
                                  SfuStatus::kSubscriptionExists,
                                  SfuStatus::kSubscriptionNotFound,
                                  SfuStatus::kSubscriptionLimitReached,
                                  SfuStatus::kSelfSubscription,
                                  SfuStatus::kCascadeAlreadyRunning,
                                  SfuStatus::kCascadeLoop};
    for (const SfuStatus status : statuses) {
        MEDIA_CHECK(go2cpp::media::SfuStatusName(status) != nullptr);
    }
}

// ---------------------------------------------------------------------------
// 7. 转发吞吐：1e6 帧（裸分片 + VideoChannel 端到端两级指标）
// ---------------------------------------------------------------------------
void TestForwardThroughput() {
    SfuConfig config;
    config.require_keyframe_start = false;  // 吞吐路径不测门控
    SfuRouter router(config);
    MEDIA_CHECK_EQ(router.CreateRoom("perf"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Join("perf", 1), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Join("perf", 2), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Join("perf", 3), SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Publish("perf", 1, 1), SfuStatus::kOk);
    std::uint64_t sink2 = 0;
    std::uint64_t sink3 = 0;
    MEDIA_CHECK_EQ(router.Subscribe("perf", 2, 1, 1, [&sink2](const SfuFragment&) { ++sink2; }),
                   SfuStatus::kOk);
    MEDIA_CHECK_EQ(router.Subscribe("perf", 3, 1, 1, [&sink3](const SfuFragment&) { ++sink3; }),
                   SfuStatus::kOk);

    const std::vector<std::uint8_t> fragment = MakeSingleFragment(1, true, 900);
    MEDIA_CHECK(!fragment.empty());
    constexpr std::uint64_t kFrames = 1000000ULL;
    for (int warmup = 0; warmup < 1000; ++warmup) {
        router.ForwardFragment("perf", 1, 1, fragment.data(), fragment.size());
    }
    sink2 = 0;
    sink3 = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < kFrames; ++index) {
        router.ForwardFragment("perf", 1, 1, fragment.data(), fragment.size());
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - begin).count();
    const double frames_per_second = static_cast<double>(kFrames) / seconds;
    MEDIA_CHECK_EQ(sink2, kFrames);
    MEDIA_CHECK_EQ(sink3, kFrames);
    MEDIA_CHECK_EQ(router.stats().fragments_received, kFrames + 1000);
    MEDIA_CHECK_EQ(router.stats().fragments_forwarded, (kFrames + 1000) * 2);
    MEDIA_CHECK_EQ(router.stats().frames_forwarded, kFrames + 1000);
    MEDIA_CHECK_GE(frames_per_second, 200000.0);
    std::printf("[PERF] raw forward: %llu frames in %.3f s = %.0f frames/s (2 subscribers)\n",
                static_cast<unsigned long long>(kFrames), seconds, frames_per_second);

    // VideoChannel 端到端：发布端分片 → SFU → 订阅端重组，验证真实链路吞吐。
    SfuRouter channel_router;
    MEDIA_CHECK_EQ(channel_router.CreateRoom("perf-e2e"), SfuStatus::kOk);
    MEDIA_CHECK_EQ(channel_router.Join("perf-e2e", 1), SfuStatus::kOk);
    MEDIA_CHECK_EQ(channel_router.Join("perf-e2e", 2), SfuStatus::kOk);
    SfuPublisher publisher(channel_router, "perf-e2e", 1, 1);
    MEDIA_CHECK_EQ(publisher.Start(), SfuStatus::kOk);
    std::uint64_t delivered_e2e = 0;
    SfuSubscriber subscriber(channel_router, "perf-e2e", 2, 1, 1,
                             [&delivered_e2e](std::uint32_t, bool, const std::uint8_t*,
                                              std::size_t) { ++delivered_e2e; });
    MEDIA_CHECK_EQ(subscriber.Start(), SfuStatus::kOk);
    constexpr std::uint64_t kE2eFrames = 20000ULL;
    const std::vector<std::uint8_t> payload = MakePayload(256, 4, 1);
    const auto e2e_begin = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < kE2eFrames; ++index) {
        MEDIA_CHECK(publisher.SendFrame(static_cast<std::uint32_t>(index + 1), index == 0,
                                        payload.data(), payload.size()));
    }
    const auto e2e_end = std::chrono::steady_clock::now();
    const double e2e_seconds = std::chrono::duration<double>(e2e_end - e2e_begin).count();
    const double e2e_fps = static_cast<double>(kE2eFrames) / e2e_seconds;
    MEDIA_CHECK_EQ(delivered_e2e, kE2eFrames);
    MEDIA_CHECK_EQ(subscriber.frames_delivered(), kE2eFrames);
    MEDIA_CHECK_EQ(channel_router.stats().fragments_forwarded, kE2eFrames);
    MEDIA_CHECK_GE(e2e_fps, 5000.0);
    std::printf("[PERF] VideoChannel end-to-end: %llu frames in %.3f s = %.0f frames/s\n",
                static_cast<unsigned long long>(kE2eFrames), e2e_seconds, e2e_fps);
}

// ---------------------------------------------------------------------------
// 8. 随机拓扑（模型对照）：随机 join/leave/publish/subscribe/forward，
//    每一步都核对 peer/stream/subscription 计数与投递集合
// ---------------------------------------------------------------------------
void TestRandomizedTopology() {
    SfuConfig config;
    config.require_keyframe_start = false;
    SfuRouter router(config);
    std::mt19937 rng(20261101U);
    auto pick = [&rng](std::size_t count) {
        return std::uniform_int_distribution<std::size_t>(0, count - 1)(rng);
    };
    const std::vector<std::string> room_names = {"r0", "r1", "r2", "r3", "r4"};
    const std::vector<SfuPeerId> peer_ids = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<SfuStreamId> stream_ids = {10, 11, 12};

    std::map<std::string, std::set<SfuPeerId>> model_peers;
    std::map<std::string, std::set<std::pair<SfuPeerId, SfuStreamId>>> model_streams;
    using SubKey = std::tuple<std::string, SfuPeerId, SfuStreamId, SfuPeerId>;
    std::set<SubKey> active_subs;
    std::map<SubKey, std::uint64_t> delivered;
    for (const std::string& room : room_names) {
        MEDIA_CHECK_EQ(router.CreateRoom(room), SfuStatus::kOk);
    }
    const std::vector<std::uint8_t> fragment = MakeSingleFragment(1, true, 500);
    MEDIA_CHECK(!fragment.empty());

    auto count_subs = [&active_subs](const std::string& room) {
        std::size_t count = 0;
        for (const SubKey& key : active_subs) {
            if (std::get<0>(key) == room) {
                ++count;
            }
        }
        return count;
    };
    auto has_peer = [&model_peers](const std::string& room, SfuPeerId peer) {
        return model_peers[room].count(peer) != 0;
    };
    auto has_stream = [&model_streams](const std::string& room, SfuPeerId publisher,
                                       SfuStreamId stream) {
        return model_streams[room].count({publisher, stream}) != 0;
    };

    for (int step = 0; step < 3000; ++step) {
        const std::string& room = room_names[pick(room_names.size())];
        const int operation = static_cast<int>(pick(100));
        if (operation < 18) {
            const SfuPeerId peer = peer_ids[pick(peer_ids.size())];
            const SfuStatus status = router.Join(room, peer);
            if (status == SfuStatus::kOk) {
                model_peers[room].insert(peer);
            } else {
                MEDIA_CHECK_EQ(status, SfuStatus::kPeerExists);
            }
        } else if (operation < 30) {
            const SfuPeerId peer = peer_ids[pick(peer_ids.size())];
            const SfuStatus status = router.Leave(room, peer);
            if (status == SfuStatus::kOk) {
                model_peers[room].erase(peer);
                for (auto it = model_streams[room].begin(); it != model_streams[room].end();) {
                    if (it->first == peer) {
                        for (auto sub = active_subs.begin(); sub != active_subs.end();) {
                            if (std::get<0>(*sub) == room && std::get<1>(*sub) == peer &&
                                std::get<2>(*sub) == it->second) {
                                sub = active_subs.erase(sub);
                            } else {
                                ++sub;
                            }
                        }
                        it = model_streams[room].erase(it);
                    } else {
                        ++it;
                    }
                }
                for (auto sub = active_subs.begin(); sub != active_subs.end();) {
                    if (std::get<0>(*sub) == room && std::get<3>(*sub) == peer) {
                        sub = active_subs.erase(sub);
                    } else {
                        ++sub;
                    }
                }
            } else {
                MEDIA_CHECK_EQ(status, SfuStatus::kPeerNotFound);
            }
        } else if (operation < 48) {
            const SfuPeerId peer = peer_ids[pick(peer_ids.size())];
            const SfuStreamId stream = stream_ids[pick(stream_ids.size())];
            if (!has_peer(room, peer)) {
                continue;
            }
            const SfuStatus status = router.Publish(room, peer, stream);
            if (status == SfuStatus::kOk) {
                model_streams[room].insert({peer, stream});
            } else {
                MEDIA_CHECK_EQ(status, SfuStatus::kStreamExists);
            }
        } else if (operation < 58) {
            const SfuPeerId peer = peer_ids[pick(peer_ids.size())];
            const SfuStreamId stream = stream_ids[pick(stream_ids.size())];
            if (!has_stream(room, peer, stream)) {
                continue;
            }
            MEDIA_CHECK_EQ(router.Unpublish(room, peer, stream), SfuStatus::kOk);
            model_streams[room].erase({peer, stream});
            for (auto sub = active_subs.begin(); sub != active_subs.end();) {
                if (std::get<0>(*sub) == room && std::get<1>(*sub) == peer &&
                    std::get<2>(*sub) == stream) {
                    sub = active_subs.erase(sub);
                } else {
                    ++sub;
                }
            }
        } else if (operation < 88) {
            // 随机挑一条已存在的流，再随机决定订阅/退订。
            if (model_streams[room].empty() || model_peers[room].size() < 2) {
                continue;
            }
            auto stream_it = model_streams[room].begin();
            std::advance(stream_it, static_cast<long>(pick(model_streams[room].size())));
            const SfuPeerId publisher = stream_it->first;
            const SfuStreamId stream = stream_it->second;
            SfuPeerId subscriber = peer_ids[pick(peer_ids.size())];
            if (!has_peer(room, subscriber) || subscriber == publisher) {
                continue;
            }
            const SubKey key{room, publisher, stream, subscriber};
            if (active_subs.count(key) != 0) {
                MEDIA_CHECK_EQ(router.Unsubscribe(room, subscriber, publisher, stream),
                               SfuStatus::kOk);
                active_subs.erase(key);
            } else {
                const SfuStatus status =
                    router.Subscribe(room, subscriber, publisher, stream,
                                     [&delivered, key](const SfuFragment&) { ++delivered[key]; });
                if (status == SfuStatus::kOk) {
                    active_subs.insert(key);
                } else {
                    MEDIA_CHECK_EQ(status, SfuStatus::kSubscriptionLimitReached);
                }
            }
        } else {
            if (model_streams[room].empty()) {
                continue;
            }
            auto stream_it = model_streams[room].begin();
            std::advance(stream_it, static_cast<long>(pick(model_streams[room].size())));
            const SfuPeerId publisher = stream_it->first;
            const SfuStreamId stream = stream_it->second;
            std::size_t expected = 0;
            for (const SubKey& key : active_subs) {
                if (std::get<0>(key) == room && std::get<1>(key) == publisher &&
                    std::get<2>(key) == stream) {
                    ++expected;
                }
            }
            const std::map<SubKey, std::uint64_t> before = delivered;
            const std::size_t forwarded =
                router.ForwardFragment(room, publisher, stream, fragment.data(), fragment.size());
            MEDIA_CHECK_EQ(forwarded, expected);
            for (const SubKey& key : active_subs) {
                const std::uint64_t previous = before.count(key) != 0 ? before.at(key) : 0;
                if (std::get<0>(key) == room && std::get<1>(key) == publisher &&
                    std::get<2>(key) == stream) {
                    MEDIA_CHECK_EQ(delivered[key], previous + 1);
                } else {
                    MEDIA_CHECK_EQ(delivered[key], previous);
                }
            }
        }
        MEDIA_CHECK_EQ(router.peer_count(room), model_peers[room].size());
        MEDIA_CHECK_EQ(router.stream_count(room), model_streams[room].size());
        MEDIA_CHECK_EQ(router.subscription_count(room), count_subs(room));
    }

    // 全局账本：加入-离开、发布-取消、订阅-退订都要对得上。
    std::size_t live_peers = 0;
    std::size_t live_streams = 0;
    for (const std::string& room : room_names) {
        live_peers += model_peers[room].size();
        live_streams += model_streams[room].size();
    }
    MEDIA_CHECK_EQ(router.stats().peers_joined - router.stats().peers_left, live_peers);
    MEDIA_CHECK_EQ(router.stats().streams_published - router.stats().streams_unpublished,
                   live_streams);
    MEDIA_CHECK_EQ(router.stats().subscriptions_created - router.stats().subscriptions_removed,
                   active_subs.size());
    std::printf("[INFO] randomized topology: 3000 steps, %zu peers, %zu streams, %zu subs\n",
                live_peers, live_streams, active_subs.size());
}

}  // namespace

int main() {
    TestSingleRoomFourPeers();
    TestMultiRoomIsolation();
    TestPeerLeaveCleanup();
    TestSubscriptionLifecycle();
    TestCascade();
    TestMalformedAndStats();
    TestRandomizedTopology();
    TestForwardThroughput();
    return media_test::Finish("test_sfu");
}
