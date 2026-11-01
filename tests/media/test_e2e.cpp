/**
 * @file test_e2e.cpp
 * @brief 端到端闭环：采集 → 编码 → VideoChannel 1200B 分片 → UDP 回环 → 重组
 *        → 解码 → 渲染，100 帧逐帧校验 + 延迟 p50/p99 + 帧率。
 *
 *        覆盖的「真实场景」（每个都是一个独立子用例）：
 *          1. 静止桌面（差分编码收敛到 36B/帧，100 帧逐帧校验）；
 *          2. 窗口拖动（每帧少量脏块，100 帧）；
 *          3. 字幕/滚动刷新（滚动条 + 帧号，60 帧）；
 *          4. 全画面噪声（不可压缩，40 帧，验证不超分片上限）；
 *          5. raw-i420 兜底路径（30 帧）；
 *          6. raw-bgra 兜底路径（15 帧，小尺寸）；
 *          7. 真实 X11 采集（30 帧，无 DISPLAY 时 SKIP）；
 *          8. 丢分片 → 丢帧 → 差分参考断裂 → 关键帧恢复（精确注入丢包）。
 *
 *        尺寸为什么是 320x240 而不是 1080p：WSL2 的 `net.core.rmem_max` 是
 *        208KB，内核 UDP 接收队列约 2x208KB（含每 skb ~768B 开销），单帧突发
 *        超过约 300KB 就会丢包；320x240 的 I420 关键帧 115200B（97 片）在预算内。
 *        这是环境限制，不是分片/重组逻辑的问题，详见 docs/modules/webrtc-media.md。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/media/codec.hpp"
#include "go2cpp/media/pipeline.hpp"
#include "go2cpp/media/pixel.hpp"
#include "go2cpp/media/stats.hpp"
#include "media_test_util.hpp"

namespace {

using namespace go2cpp::media;

/// @brief 一次端到端运行的结果。
struct E2EResult {
    std::uint64_t sent = 0;
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;
    std::uint64_t verified = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    double elapsed_ms = 0.0;
    double fps = 0.0;
    LatencyRecorder latency;
};

/// @brief 期望的解码结果校验和。
/// @details delta/raw-i420 解码输出 I420，raw-bgra 解码输出 BGRA —— 必须按
///          解码器的输出格式算校验和，否则会把「格式不同」误判成「内容错」。
std::uint64_t ExpectedChecksum(const VideoFrame& bgra, CodecKind codec) {
    if (codec == CodecKind::kRawBgra) {
        return bgra.Checksum();
    }
    VideoFrame i420;
    i420.width = bgra.width;
    i420.height = bgra.height;
    i420.format = PixelFormat::kI420;
    i420.data.assign(I420FrameBytes(bgra.width, bgra.height), 0);
    BgraToI420(bgra.data.data(), bgra.width, bgra.height, bgra.EffectiveStride(), i420.data.data());
    return i420.Checksum();
}

/// @brief 跑一次端到端回环。
/// @param verify 逐帧校验（要求严格顺序、零丢帧）。
/// @param receiver_out 输出接收端（供调用方读取 transport 统计）；可为 nullptr。
E2EResult RunLoopback(const MediaConfig& config, int frames, bool verify,
                      std::unique_ptr<MediaReceiver>* receiver_out, bool* ok) {
    E2EResult result;
    *ok = false;
    std::string error;
    auto receiver = MediaReceiver::Create(config, &error);
    if (!receiver) {
        std::printf("[FAIL] receiver create: %s\n", error.c_str());
        return result;
    }
    auto sender = MediaSender::Create(config, receiver->local_port(), &error);
    if (!sender) {
        std::printf("[FAIL] sender create: %s\n", error.c_str());
        return result;
    }
    std::printf("[INFO] source=%s encoder=%s renderer=%s codec=%s\n", sender->source_name(),
                sender->codec_name(), receiver->renderer_name(), receiver->codec_name());

    std::vector<std::uint64_t> expected;
    expected.reserve(static_cast<std::size_t>(frames));
    std::size_t verified = 0;
    const auto start = std::chrono::steady_clock::now();
    bool failed = false;
    for (int i = 0; i < frames; ++i) {
        VideoFrame captured;
        std::size_t encoded_bytes = 0;
        if (!sender->SendNext(&captured, &encoded_bytes)) {
            std::printf("[FAIL] SendNext failed at frame %d\n", i);
            failed = true;
            break;
        }
        ++result.sent;
        result.bytes_sent += encoded_bytes;
        if (verify) {
            expected.push_back(ExpectedChecksum(captured, config.codec));
        }
        // 泵到本帧送达（或 500ms 超时）。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (receiver->frames_delivered() <= static_cast<std::uint64_t>(i) &&
               std::chrono::steady_clock::now() < deadline) {
            receiver->Pump(1);
        }
        while (receiver->frames_delivered() > verified) {
            if (verify && verified < expected.size()) {
                const std::uint64_t actual = receiver->last_frame().Checksum();
                if (actual != expected[verified]) {
                    std::printf("[FAIL] frame %zu checksum mismatch: got %016llx want %016llx\n",
                                verified, static_cast<unsigned long long>(actual),
                                static_cast<unsigned long long>(expected[verified]));
                    failed = true;
                }
            }
            ++verified;
        }
    }
    // 收尾：把仍在链路上的包泵完。
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (receiver->frames_delivered() < result.sent &&
           std::chrono::steady_clock::now() < drain_deadline) {
        receiver->Pump(2);
        while (receiver->frames_delivered() > verified) {
            if (verify && verified < expected.size()) {
                const std::uint64_t actual = receiver->last_frame().Checksum();
                if (actual != expected[verified]) {
                    std::printf("[FAIL] drain frame %zu checksum mismatch\n", verified);
                    failed = true;
                }
            }
            ++verified;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.fps = result.elapsed_ms > 0.0
                     ? static_cast<double>(result.sent) * 1000.0 / result.elapsed_ms
                     : 0.0;
    result.delivered = receiver->frames_delivered();
    result.dropped = receiver->frames_dropped();
    result.bytes_received = receiver->bytes_received();
    result.latency = receiver->latency();
    result.verified = verified;
    std::printf(
        "[INFO] sent=%llu delivered=%llu verified=%llu dropped=%llu bytes_tx=%llu bytes_rx=%llu "
        "elapsed=%.1fms fps=%.1f\n",
        static_cast<unsigned long long>(result.sent),
        static_cast<unsigned long long>(result.delivered),
        static_cast<unsigned long long>(result.verified),
        static_cast<unsigned long long>(result.dropped),
        static_cast<unsigned long long>(result.bytes_sent),
        static_cast<unsigned long long>(result.bytes_received), result.elapsed_ms, result.fps);
    std::printf("[INFO] latency: %s\n", result.latency.Summary().c_str());
    if (receiver_out != nullptr) {
        *receiver_out = std::move(receiver);
    }
    *ok = !failed;
    return result;
}

/// @brief 断言一次严格回环的验收线。
void CheckStrict(const E2EResult& result, int frames, const char* scene, double min_fps,
                 double max_p99_ms) {
    MEDIA_CHECK_EQ(result.sent, static_cast<std::uint64_t>(frames));
    MEDIA_CHECK_EQ(result.delivered, static_cast<std::uint64_t>(frames));
    MEDIA_CHECK_EQ(result.verified, static_cast<std::uint64_t>(frames));
    MEDIA_CHECK_EQ(result.dropped, 0U);
    MEDIA_CHECK_EQ(result.bytes_sent, result.bytes_received);
    MEDIA_CHECK_GE(result.fps, min_fps);
    if (static_cast<int>(result.latency.count()) > 0) {
        const double p99 = result.latency.p99();
        std::printf("[INFO] scene=%s p50=%.3fms p99=%.3fms max=%.3fms\n", scene,
                    result.latency.p50(), p99, result.latency.max());
        MEDIA_CHECK_GE(max_p99_ms, p99);
    }
}

void SceneStatic() {
    MediaConfig config;
    config.width = 320;
    config.height = 240;
    config.codec = CodecKind::kDeltaI420;
    config.pattern = SyntheticPattern::kStatic;
    config.keyframe_interval = 0;  // 只首帧关键帧，验证静止收敛
    bool ok = false;
    const E2EResult result = RunLoopback(config, 100, true, nullptr, &ok);
    MEDIA_CHECK(ok);
    CheckStrict(result, 100, "static-320x240", 100.0, 100.0);
    // 静止收敛：平均每帧字节数应远小于首帧 I420。
    const double avg = static_cast<double>(result.bytes_sent) / 100.0;
    MEDIA_CHECK(avg < static_cast<double>(I420FrameBytes(320, 240)) / 4.0);
}

void ScenePatch() {
    MediaConfig config;
    config.width = 320;
    config.height = 240;
    config.pattern = SyntheticPattern::kPatch;
    config.keyframe_interval = 30;
    bool ok = false;
    const E2EResult result = RunLoopback(config, 100, true, nullptr, &ok);
    MEDIA_CHECK(ok);
    CheckStrict(result, 100, "patch-320x240", 100.0, 100.0);
}

void SceneScroll() {
    MediaConfig config;
    config.width = 256;
    config.height = 192;
    config.pattern = SyntheticPattern::kScroll;
    config.keyframe_interval = 20;
    bool ok = false;
    const E2EResult result = RunLoopback(config, 60, true, nullptr, &ok);
    MEDIA_CHECK(ok);
    CheckStrict(result, 60, "scroll-256x192", 100.0, 100.0);
}

void SceneNoise() {
    MediaConfig config;
    config.width = 192;
    config.height = 144;
    config.pattern = SyntheticPattern::kNoise;
    config.keyframe_interval = 10;
    bool ok = false;
    const E2EResult result = RunLoopback(config, 40, true, nullptr, &ok);
    MEDIA_CHECK(ok);
    CheckStrict(result, 40, "noise-192x144", 60.0, 200.0);
    // 噪声不可压缩：编码字节应接近 I420 大小 + 块头。
    const double ratio = static_cast<double>(result.bytes_sent) /
                         (40.0 * static_cast<double>(I420FrameBytes(192, 144)));
    std::printf("[INFO] noise compression ratio = %.3f\n", ratio);
    MEDIA_CHECK_GE(1.3, ratio);
}

void SceneRawI420() {
    MediaConfig config;
    config.width = 320;
    config.height = 240;
    config.codec = CodecKind::kRawI420;
    config.pattern = SyntheticPattern::kStatic;
    bool ok = false;
    const E2EResult result = RunLoopback(config, 30, true, nullptr, &ok);
    MEDIA_CHECK(ok);
    CheckStrict(result, 30, "raw-i420-320x240", 100.0, 100.0);
    MEDIA_CHECK_EQ(result.bytes_sent,
                   static_cast<std::uint64_t>(30) * (MediaHeader::kBytes + I420FrameBytes(320, 240)));
}

void SceneRawBgra() {
    MediaConfig config;
    config.width = 160;
    config.height = 120;
    config.codec = CodecKind::kRawBgra;
    config.pattern = SyntheticPattern::kPatch;
    bool ok = false;
    const E2EResult result = RunLoopback(config, 15, true, nullptr, &ok);
    MEDIA_CHECK(ok);
    CheckStrict(result, 15, "raw-bgra-160x120", 100.0, 100.0);
    MEDIA_CHECK_EQ(result.bytes_sent,
                   static_cast<std::uint64_t>(15) * (MediaHeader::kBytes + BgraFrameBytes(160, 120)));
}

void SceneX11() {
    if (!X11Available()) {
        media_test::Skip("X11 unavailable (DISPLAY unset) - real capture scene skipped");
        return;
    }
    MediaConfig config;
    config.use_x11 = true;
    config.width = 320;
    config.height = 240;
    config.x11.width = 320;
    config.x11.height = 240;
    config.x11.use_shm = true;
    config.keyframe_interval = 15;
    bool ok = false;
    std::unique_ptr<MediaReceiver> receiver;
    const E2EResult result = RunLoopback(config, 30, true, &receiver, &ok);
    if (result.sent == 0) {
        media_test::Skip("X11 capture source could not be created");
        return;
    }
    MEDIA_CHECK(ok);
    CheckStrict(result, 30, "x11-320x240", 60.0, 150.0);
    if (receiver) {
        std::printf("[INFO] x11 transport: frames_delivered=%llu\n",
                    static_cast<unsigned long long>(receiver->transport_stats().frames_delivered));
    }
}

/// @brief 精确注入丢包：丢一个分片 → 该帧不交付 → 差分参考断裂 → 关键帧恢复。
void SceneLossRecovery() {
    const int width = 320;
    const int height = 240;
    auto encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    auto decoder = CreateDecoder(CodecKind::kDeltaI420, width, height);
    MEDIA_CHECK(encoder != nullptr);
    MEDIA_CHECK(decoder != nullptr);

    std::vector<std::vector<std::uint8_t>> packets;
    go2cpp::bento::VideoChannel sender(
        [&packets](const std::uint8_t* data, std::size_t bytes) {
            packets.emplace_back(data, data + bytes);
            return true;
        },
        [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});
    go2cpp::bento::VideoChannel receiver(
        [](const std::uint8_t*, std::size_t) { return true; },
        [&decoder](std::uint32_t, bool, const std::uint8_t* data, std::size_t bytes) {
            VideoFrame frame;
            decoder->Decode(data, bytes, &frame);
        });

    SyntheticCapture scroll(width, height, SyntheticPattern::kScroll);
    SyntheticCapture noise(width, height, SyntheticPattern::kNoise);
    std::vector<std::uint8_t> encoded;

    auto flush = [&packets, &receiver]() {
        for (const auto& packet : packets) {
            go2cpp::bento::FrameView view;
            MEDIA_CHECK(go2cpp::bento::ParseFrame(packet.data(), packet.size(), &view));
            receiver.OnFrame(view.header, view.payload, view.payload_bytes);
        }
        packets.clear();
    };

    // 帧 1：关键帧完整送达。帧号从 1 开始（0 会被 VideoChannel 当 stale 丢弃）。
    VideoFrame frame;
    MEDIA_CHECK(scroll.Capture(&frame));
    frame.frame_id = 1;
    MEDIA_CHECK(encoder->Encode(frame, &encoded));
    MEDIA_CHECK(sender.SendFrame(1, true, encoded.data(), encoded.size()));
    flush();
    MEDIA_CHECK_EQ(decoder->decoded_frames(), 1U);

    // 帧 2：用不可压缩的噪声帧保证有多个分片，丢掉最后一个 → 缺片。
    MEDIA_CHECK(noise.Capture(&frame));
    frame.frame_id = 2;
    MEDIA_CHECK(encoder->Encode(frame, &encoded));
    MEDIA_CHECK(sender.SendFrame(2, false, encoded.data(), encoded.size()));
    MEDIA_CHECK_GE(packets.size(), 2U);
    for (std::size_t i = 0; i + 1 < packets.size(); ++i) {
        go2cpp::bento::FrameView view;
        MEDIA_CHECK(go2cpp::bento::ParseFrame(packets[i].data(), packets[i].size(), &view));
        receiver.OnFrame(view.header, view.payload, view.payload_bytes);
    }
    packets.clear();
    receiver.OnTimer(go2cpp::bento::NowMs() +
                     go2cpp::bento::VideoChannel::kReassemblyTimeoutMs + 100);
    MEDIA_CHECK_EQ(decoder->decoded_frames(), 1U);
    MEDIA_CHECK_GE(receiver.stats().frames_incomplete_dropped, 1U);

    // 帧 3：delta（相对帧 2），解码端参考帧仍是帧 1 → 内容错但不崩。
    MEDIA_CHECK(scroll.Capture(&frame));
    frame.frame_id = 3;
    MEDIA_CHECK(encoder->Encode(frame, &encoded));
    MEDIA_CHECK(sender.SendFrame(3, false, encoded.data(), encoded.size()));
    flush();
    MEDIA_CHECK_EQ(decoder->decoded_frames(), 2U);
    MEDIA_CHECK_EQ(decoder->dropped_frames(), 0U);

    // 帧 4：强制关键帧 → 解码内容与发送帧 4 逐字节一致。
    encoder->ForceKeyframe();
    MEDIA_CHECK(scroll.Capture(&frame));
    frame.frame_id = 4;
    MEDIA_CHECK(encoder->Encode(frame, &encoded));
    MEDIA_CHECK(sender.SendFrame(4, true, encoded.data(), encoded.size()));
    const std::vector<std::uint8_t> key_payload = encoded;
    flush();
    MEDIA_CHECK_EQ(decoder->decoded_frames(), 3U);

    VideoFrame recovered;
    MEDIA_CHECK(decoder->Decode(key_payload.data(), key_payload.size(), &recovered));
    VideoFrame expect;
    expect.width = width;
    expect.height = height;
    expect.format = PixelFormat::kI420;
    expect.data.assign(I420FrameBytes(width, height), 0);
    BgraToI420(frame.data.data(), width, height, frame.EffectiveStride(), expect.data.data());
    MEDIA_CHECK(FrameEqual(expect, recovered));
    std::printf("[INFO] loss recovery: decoded=%llu dropped=%llu incomplete=%llu\n",
                static_cast<unsigned long long>(decoder->decoded_frames()),
                static_cast<unsigned long long>(decoder->dropped_frames()),
                static_cast<unsigned long long>(receiver.stats().frames_incomplete_dropped));
}

void PrintAvailability() {
    const CodecAvailability availability = ProbeAvailability();
    std::printf("[INFO] availability: x11=%d sdl=%d ffmpeg_h264=%d nvenc=%d\n",
                availability.x11_capture ? 1 : 0, availability.sdl_render ? 1 : 0,
                availability.h264 ? 1 : 0, availability.h264_nvenc ? 1 : 0);
}

}  // namespace

int main() {
    PrintAvailability();
    SceneStatic();
    ScenePatch();
    SceneScroll();
    SceneNoise();
    SceneRawI420();
    SceneRawBgra();
    SceneX11();
    SceneLossRecovery();
    return media_test::Finish("test_e2e");
}
