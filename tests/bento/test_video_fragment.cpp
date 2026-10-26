/**
 * @file test_video_fragment.cpp
 * @brief VIDEO 分片发送/乱序重组/旧帧丢弃/缺片超时/非法分片边界。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

struct DeliveredFrame {
    std::uint32_t frame_id = 0;
    bool key = false;
    std::vector<std::uint8_t> data;
};

std::vector<std::uint8_t> MakeFrameData(std::size_t bytes, std::uint8_t seed) {
    std::vector<std::uint8_t> data(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 37U + seed) & 0xFFU);
    }
    return data;
}

bool Feed(VideoChannel& receiver, const std::vector<std::uint8_t>& frame_bytes) {
    FrameView view;
    if (!ParseFrame(frame_bytes.data(), frame_bytes.size(), &view)) {
        return false;
    }
    receiver.OnFrame(view.header, view.payload, view.payload_bytes);
    return true;
}

}  // namespace

int main() {
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<DeliveredFrame> delivered;

    VideoChannel sender(
        [&sent](const std::uint8_t* data, std::size_t bytes) {
            sent.emplace_back(data, data + bytes);
            return true;
        },
        [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});
    VideoChannel receiver([](const std::uint8_t*, std::size_t) { return true; },
                          [&delivered](std::uint32_t frame_id, bool key, const std::uint8_t* data,
                                       std::size_t bytes) {
                              delivered.push_back(DeliveredFrame{
                                  frame_id, key, std::vector<std::uint8_t>(data, data + bytes)});
                          });

    // 正常帧：3000B -> 3 片，乱序 2/0/1 注入。
    const std::vector<std::uint8_t> frame7 = MakeFrameData(3000, 7);
    BENTO_CHECK(sender.SendFrame(7, true, frame7.data(), frame7.size()));
    BENTO_CHECK_EQ(sent.size(), 3U);
    BENTO_CHECK(Feed(receiver, sent[2]));
    BENTO_CHECK(Feed(receiver, sent[0]));
    BENTO_CHECK_EQ(delivered.size(), 0U);
    BENTO_CHECK(Feed(receiver, sent[1]));
    BENTO_CHECK_EQ(delivered.size(), 1U);
    BENTO_CHECK_EQ(delivered[0].frame_id, 7U);
    BENTO_CHECK(delivered[0].key);
    BENTO_CHECK_EQ(delivered[0].data.size(), frame7.size());
    BENTO_CHECK(delivered[0].data == frame7);
    BENTO_CHECK_EQ(receiver.stats().frames_delivered, 1U);

    // 旧 frame_id：直接 stale，不建立重组状态。
    sent.clear();
    const std::vector<std::uint8_t> frame5 = MakeFrameData(100, 5);
    BENTO_CHECK(sender.SendFrame(5, false, frame5.data(), frame5.size()));
    BENTO_CHECK_EQ(sent.size(), 1U);
    BENTO_CHECK(Feed(receiver, sent[0]));
    BENTO_CHECK_EQ(delivered.size(), 1U);
    BENTO_CHECK_EQ(receiver.stats().fragments_stale, 1U);

    // 缺一片：重组项超时后丢弃；补齐后也不会交付。
    sent.clear();
    const std::vector<std::uint8_t> frame8 = MakeFrameData(2000, 8);
    BENTO_CHECK(sender.SendFrame(8, false, frame8.data(), frame8.size()));
    BENTO_CHECK_EQ(sent.size(), 2U);
    BENTO_CHECK(Feed(receiver, sent[0]));
    receiver.OnTimer(NowMs() + VideoChannel::kReassemblyTimeoutMs + 100);
    BENTO_CHECK_EQ(receiver.stats().frames_incomplete_dropped, 1U);
    BENTO_CHECK(Feed(receiver, sent[1]));
    BENTO_CHECK_EQ(delivered.size(), 1U);

    // 空帧：1 个 0B 分片，合法。
    sent.clear();
    BENTO_CHECK(sender.SendFrame(9, false, nullptr, 0));
    BENTO_CHECK_EQ(sent.size(), 1U);
    BENTO_CHECK(Feed(receiver, sent[0]));
    BENTO_CHECK_EQ(delivered.size(), 2U);
    BENTO_CHECK_EQ(delivered[1].frame_id, 9U);
    BENTO_CHECK_EQ(delivered[1].data.size(), 0U);
    BENTO_CHECK(!delivered[1].key);

    // 非法分片：frag_index >= frag_count；非末片不是满片。
    FrameHeader header;
    header.type = FrameType::kVideo;
    header.channel = kChannelVideo;
    header.seq = 100;
    std::vector<std::uint8_t> bad_payload(kVideoHeaderBytes + 1200, 0);
    VideoFragmentHeader bad{11, 2, 2};
    EncodeVideoHeader(bad, bad_payload.data());
    const std::vector<std::uint8_t> bad_frame =
        BuildFrame(header, nullptr, bad_payload.data(), bad_payload.size());
    BENTO_CHECK(Feed(receiver, bad_frame));
    BENTO_CHECK_EQ(receiver.stats().fragments_invalid, 1U);

    std::vector<std::uint8_t> short_non_last(kVideoHeaderBytes + 100, 0);
    VideoFragmentHeader bad2{12, 0, 2};
    EncodeVideoHeader(bad2, short_non_last.data());
    const std::vector<std::uint8_t> bad_frame2 =
        BuildFrame(header, nullptr, short_non_last.data(), short_non_last.size());
    BENTO_CHECK(Feed(receiver, bad_frame2));
    BENTO_CHECK_EQ(receiver.stats().fragments_invalid, 2U);

    // 超过最大分片数：拒绝发送。
    const std::vector<std::uint8_t> huge(
        VideoChannel::kMaxFragments * VideoChannel::kFragmentBytes + 1, 0);
    BENTO_CHECK(!sender.SendFrame(10, false, huge.data(), huge.size()));
    BENTO_CHECK_EQ(sender.stats().frames_sent, 4U);
    BENTO_CHECK_EQ(sender.stats().fragments_sent, 3U + 1U + 2U + 1U);

    std::printf(
        "[INFO] video: tx_fragments=%llu rx_frames=%llu incomplete=%llu invalid=%llu "
        "stale=%llu\n",
        static_cast<unsigned long long>(sender.stats().fragments_sent),
        static_cast<unsigned long long>(receiver.stats().frames_delivered),
        static_cast<unsigned long long>(receiver.stats().frames_incomplete_dropped),
        static_cast<unsigned long long>(receiver.stats().fragments_invalid),
        static_cast<unsigned long long>(receiver.stats().fragments_stale));

    return bento_test::Finish("test_video_fragment");
}
