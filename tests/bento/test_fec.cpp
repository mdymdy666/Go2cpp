/**
 * @file test_fec.cpp
 * @brief VIDEO XOR FEC：丢 1 片恢复整帧；丢 2 片绝不误恢复；group=4/8。
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

struct Delivered {
    std::uint32_t frame_id = 0;
    bool key = false;
    std::vector<std::uint8_t> data;
};

std::vector<std::uint8_t> MakeData(std::size_t bytes, std::uint8_t seed) {
    std::vector<std::uint8_t> data(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 37U + seed) & 0xFFU);
    }
    return data;
}

struct WireFrame {
    std::vector<std::uint8_t> bytes;
    bool parity = false;
    std::uint16_t frag_index = 0;
    std::uint16_t frag_count = 0;
    std::uint16_t group_start = 0;
};

WireFrame ParseWire(const std::vector<std::uint8_t>& bytes) {
    WireFrame wire;
    wire.bytes = bytes;
    FrameView view;
    if (!ParseFrame(bytes.data(), bytes.size(), &view)) {
        return wire;
    }
    wire.parity = (view.header.flags & kFlagFec) != 0;
    VideoFragmentHeader video;
    if (DecodeVideoHeader(view.payload, view.payload_bytes, &video)) {
        wire.frag_index = video.frag_index;
        wire.frag_count = video.frag_count;
    }
    if (wire.parity && view.payload_bytes >= kVideoHeaderBytes + 6) {
        wire.group_start = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(view.payload[kVideoHeaderBytes]) << 8) |
            view.payload[kVideoHeaderBytes + 1]);
    }
    return wire;
}

bool Feed(VideoChannel& receiver, const std::vector<std::uint8_t>& bytes) {
    FrameView view;
    if (!ParseFrame(bytes.data(), bytes.size(), &view)) {
        return false;
    }
    receiver.OnFrame(view.header, view.payload, view.payload_bytes);
    return true;
}

VideoChannel::FrameSink Capture(std::vector<std::vector<std::uint8_t>>* out) {
    return [out](const std::uint8_t* data, std::size_t bytes) {
        out->emplace_back(data, data + bytes);
        return true;
    };
}

VideoChannel::DeliverFn Deliver(std::vector<Delivered>* out) {
    return [out](std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes) {
        out->push_back(Delivered{frame_id, key, std::vector<std::uint8_t>(data, data + bytes)});
    };
}

}  // namespace

int main() {
    std::vector<Delivered> delivered;
    VideoChannel receiver([](const std::uint8_t*, std::size_t) { return true; },
                          Deliver(&delivered));

    // ---- group=4：丢 1 片，校验片恢复整帧 ----
    std::vector<std::vector<std::uint8_t>> sent;
    VideoChannel sender(Capture(&sent), [](std::uint32_t, bool, const std::uint8_t*,
                                           std::size_t) {},
                        VideoChannel::Config{4});
    const std::vector<std::uint8_t> frame1 = MakeData(4800, 7);
    BENTO_CHECK(sender.SendFrame(1, true, frame1.data(), frame1.size()));
    BENTO_CHECK_EQ(sent.size(), 5U);  // 4 数据片 + 1 校验片
    BENTO_CHECK_EQ(sender.stats().fragments_sent, 4U);
    BENTO_CHECK_EQ(sender.stats().fec_parity_sent, 1U);

    std::vector<WireFrame> wires;
    for (const auto& bytes : sent) {
        wires.push_back(ParseWire(bytes));
    }
    BENTO_CHECK_EQ(wires.back().parity, true);
    BENTO_CHECK_EQ(wires.back().frag_index, 4U);  // 题目约定 frag_idx = N
    BENTO_CHECK_EQ(wires.back().frag_count, 4U);
    for (std::size_t i = 0; i < 4; ++i) {
        BENTO_CHECK(!wires[i].parity);
        BENTO_CHECK_EQ(wires[i].frag_index, static_cast<std::uint16_t>(i));
    }

    // 故意丢第 3 片（index=2），且校验片先于缺失片到达。
    BENTO_CHECK(Feed(receiver, wires[0].bytes));
    BENTO_CHECK(Feed(receiver, wires[1].bytes));
    BENTO_CHECK(Feed(receiver, wires[3].bytes));
    BENTO_CHECK_EQ(delivered.size(), 0U);
    BENTO_CHECK(Feed(receiver, wires[4].bytes));
    BENTO_CHECK_EQ(delivered.size(), 1U);
    BENTO_CHECK_EQ(delivered[0].frame_id, 1U);
    BENTO_CHECK(delivered[0].key);
    BENTO_CHECK(delivered[0].data == frame1);
    BENTO_CHECK_EQ(receiver.stats().fec_parity_received, 1U);
    BENTO_CHECK_EQ(receiver.stats().fec_recovered, 1U);

    // ---- group=4：丢 2 片，只有一个校验片 -> 不交付，超时丢弃 ----
    sent.clear();
    const std::vector<std::uint8_t> frame2 = MakeData(4800, 9);
    BENTO_CHECK(sender.SendFrame(2, false, frame2.data(), frame2.size()));
    BENTO_CHECK_EQ(sent.size(), 5U);
    std::vector<WireFrame> wires2;
    for (const auto& bytes : sent) {
        wires2.push_back(ParseWire(bytes));
    }
    BENTO_CHECK(Feed(receiver, wires2[0].bytes));
    BENTO_CHECK(Feed(receiver, wires2[1].bytes));
    BENTO_CHECK(Feed(receiver, wires2[4].bytes));
    BENTO_CHECK_EQ(delivered.size(), 1U);  // 仍然只有 frame1
    receiver.OnTimer(NowMs() + VideoChannel::kReassemblyTimeoutMs + 100);
    BENTO_CHECK_EQ(delivered.size(), 1U);
    BENTO_CHECK_EQ(receiver.stats().frames_incomplete_dropped, 1U);

    // ---- group=8：丢 1 片恢复 ----
    sent.clear();
    VideoChannel sender8(Capture(&sent), [](std::uint32_t, bool, const std::uint8_t*,
                                            std::size_t) {},
                         VideoChannel::Config{8});
    const std::vector<std::uint8_t> frame3 = MakeData(9600, 11);
    BENTO_CHECK(sender8.SendFrame(3, false, frame3.data(), frame3.size()));
    BENTO_CHECK_EQ(sent.size(), 9U);  // 8 数据片 + 1 校验片
    std::vector<WireFrame> wires3;
    for (const auto& bytes : sent) {
        wires3.push_back(ParseWire(bytes));
    }
    for (std::size_t i = 0; i < 8; ++i) {
        if (i == 5) {
            continue;  // 丢第 6 片
        }
        BENTO_CHECK(Feed(receiver, wires3[i].bytes));
    }
    BENTO_CHECK(Feed(receiver, wires3[8].bytes));
    BENTO_CHECK_EQ(delivered.size(), 2U);
    BENTO_CHECK_EQ(delivered[1].frame_id, 3U);
    BENTO_CHECK(delivered[1].data == frame3);
    BENTO_CHECK_EQ(receiver.stats().fec_recovered, 2U);

    // ---- group=4 / 3 组：每组各丢 1 片，三组都靠校验片恢复 ----
    sent.clear();
    VideoChannel sender4(Capture(&sent), [](std::uint32_t, bool, const std::uint8_t*,
                                            std::size_t) {},
                         VideoChannel::Config{4});
    const std::vector<std::uint8_t> frame4 = MakeData(12000, 13);  // 10 片 -> 4/4/2
    BENTO_CHECK(sender4.SendFrame(4, false, frame4.data(), frame4.size()));
    BENTO_CHECK_EQ(sent.size(), 13U);  // 10 数据片 + 3 校验片
    BENTO_CHECK_EQ(sender4.stats().fec_parity_sent, 3U);
    std::vector<WireFrame> wires4;
    for (const auto& bytes : sent) {
        wires4.push_back(ParseWire(bytes));
    }
    std::size_t parity_seen = 0;
    std::size_t recovered_before = receiver.stats().fec_recovered;
    for (std::size_t i = 0; i < 10; ++i) {
        if (i == 1 || i == 5 || i == 9) {
            continue;  // 每组丢 1 片
        }
        BENTO_CHECK(Feed(receiver, wires4[i].bytes));
    }
    for (std::size_t i = 10; i < wires4.size(); ++i) {
        BENTO_CHECK(wires4[i].parity);
        ++parity_seen;
        BENTO_CHECK(Feed(receiver, wires4[i].bytes));
    }
    BENTO_CHECK_EQ(parity_seen, 3U);
    BENTO_CHECK_EQ(delivered.size(), 3U);
    BENTO_CHECK_EQ(delivered[2].frame_id, 4U);
    BENTO_CHECK(delivered[2].data == frame4);
    BENTO_CHECK_EQ(receiver.stats().fec_recovered, recovered_before + 3U);

    // ---- 默认（group=0）：不产生校验片，旧行为不变 ----
    sent.clear();
    VideoChannel plain(Capture(&sent), [](std::uint32_t, bool, const std::uint8_t*,
                                          std::size_t) {});
    BENTO_CHECK(plain.SendFrame(5, false, frame1.data(), frame1.size()));
    BENTO_CHECK_EQ(sent.size(), 4U);
    BENTO_CHECK_EQ(plain.stats().fec_parity_sent, 0U);

    std::printf(
        "[INFO] fec: rx_parity=%llu recovered=%llu incomplete_dropped=%llu "
        "tx_parity=%llu\n",
        static_cast<unsigned long long>(receiver.stats().fec_parity_received),
        static_cast<unsigned long long>(receiver.stats().fec_recovered),
        static_cast<unsigned long long>(receiver.stats().frames_incomplete_dropped),
        static_cast<unsigned long long>(sender4.stats().fec_parity_sent));
    return bento_test::Finish("test_fec");
}
