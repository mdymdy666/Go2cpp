/**
 * @file test_media_e2e_jitter.cpp
 * @brief pipeline 接入 jitter/degradation 的回归测试（§5 接线）。
 *
 * 覆盖：
 *   1. 收端 jitter 开启：100 帧 delta 逐帧 I420 逐字节一致、零丢帧；
 *   2. jitter 关闭 vs 开启：两者都正确，开启至少不更差（延迟/交付）；
 *   3. 发送端 degradation：带宽下降 → 档位 full→reduced→low→minimal，
 *      发送尺寸真的从 320x240 变成 160x120，收端解码尺寸跟随且内容逐字节正确；
 *   4. 收端 degradation 帧率门控：低带宽档实际跳帧（frames_skipped > 0）；
 *   5. jitter 分块边界：小 chunk（512B）强制关键帧切成多块，拼帧仍逐字节一致。
 *
 * 构建（独立，不改主 CMake）：
 *   cmake -S examples/media_e2e -B .build-media-e2e -G Ninja
 *   cmake --build .build-media-e2e -j && .build-media-e2e/media_test_e2e_jitter
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/media/codec.hpp"
#include "go2cpp/media/degradation.hpp"
#include "go2cpp/media/jitter_buffer.hpp"
#include "go2cpp/media/pipeline.hpp"
#include "go2cpp/media/pixel.hpp"
#include "go2cpp/media/stats.hpp"

#include "media_test_util.hpp"

namespace {

using namespace go2cpp::media;

/// @brief 采集 BGRA → I420 参考帧。
VideoFrame ToI420(const VideoFrame& bgra) {
    VideoFrame frame;
    frame.width = bgra.width;
    frame.height = bgra.height;
    frame.format = PixelFormat::kI420;
    frame.data.assign(I420FrameBytes(bgra.width, bgra.height), 0);
    BgraToI420(bgra.data.data(), bgra.width, bgra.height, bgra.EffectiveStride(),
               frame.data.data());
    return frame;
}

/// @brief FNV-1a 64（与 VideoFrame::Checksum 不同：这里只用于自校验，不参与断言）。
std::uint64_t HashBytes(const std::uint8_t* data, std::size_t bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < bytes; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

MediaConfig BaseConfig(CodecKind codec, int width, int height) {
    MediaConfig config;
    config.width = width;
    config.height = height;
    config.codec = codec;
    config.pattern = SyntheticPattern::kPatch;
    config.keyframe_interval = 30;
    return config;
}

/// @brief 一次回环的统计。
struct LoopStats {
    std::uint64_t sent = 0;
    std::uint64_t delivered = 0;
    std::uint64_t correct = 0;
    std::uint64_t wrong = 0;
    JitterStats jitter;
    LatencyRecorder latency;
    bool ok = true;
};

/// @brief 跑 N 帧 pipeline 回环，逐帧校验 I420 内容。
LoopStats RunLoop(const MediaConfig& config, int frames) {
    LoopStats stats;
    std::string error;
    auto receiver = MediaReceiver::Create(config, &error);
    if (!receiver) {
        stats.ok = false;
        std::printf("[FAIL] receiver create: %s\n", error.c_str());
        return stats;
    }
    MediaConfig sender_config = config;
    auto sender = MediaSender::Create(sender_config, receiver->local_port(), &error);
    if (!sender) {
        stats.ok = false;
        std::printf("[FAIL] sender create: %s\n", error.c_str());
        return stats;
    }
    for (int i = 0; i < frames; ++i) {
        VideoFrame captured;
        std::size_t encoded_bytes = 0;
        if (!sender->SendNext(&captured, &encoded_bytes)) {
            stats.ok = false;
            break;
        }
        if (encoded_bytes == 0) {
            continue;
        }
        ++stats.sent;
        const VideoFrame reference = ToI420(captured);
        const std::uint64_t want = reference.Checksum();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (receiver->frames_delivered() < stats.sent &&
               std::chrono::steady_clock::now() < deadline) {
            receiver->Pump(1);
        }
        if (receiver->frames_delivered() < stats.sent) {
            stats.ok = false;
            std::printf("[FAIL] frame timeout at %d\n", i);
            break;
        }
        const VideoFrame& got = receiver->last_frame();
        if (got.frame_id != captured.frame_id) {
            stats.ok = false;
        }
        if (got.Checksum() == want && FrameEqual(got, reference)) {
            ++stats.correct;
        } else {
            ++stats.wrong;
        }
    }
    for (int i = 0; i < 100 && receiver->frames_delivered() < stats.sent; ++i) {
        receiver->Pump(2);
    }
    stats.delivered = receiver->frames_delivered();
    stats.jitter = receiver->jitter_stats();
    stats.latency = receiver->latency();
    return stats;
}

/// @brief 1) jitter 开启：100 帧逐字节一致、零丢帧、统计非零。
void TestJitterLoopback() {
    MediaConfig config = BaseConfig(CodecKind::kDeltaI420, 320, 240);
    config.jitter = true;
    config.jitter_chunk_bytes = 4096;
    config.jitter_config.base_delay_ns = 5'000'000;
    config.jitter_config.min_delay_ns = 2'000'000;
    const LoopStats stats = RunLoop(config, 100);
    MEDIA_CHECK(stats.ok);
    MEDIA_CHECK_EQ(stats.sent, 100U);
    MEDIA_CHECK_EQ(stats.delivered, 100U);
    MEDIA_CHECK_EQ(stats.wrong, 0U);
    MEDIA_CHECK_EQ(stats.correct, 100U);
    MEDIA_CHECK(stats.jitter.received > 0);
    MEDIA_CHECK(stats.jitter.accepted > 0);
    MEDIA_CHECK_EQ(stats.jitter.delivered, stats.jitter.accepted);
    MEDIA_CHECK(stats.jitter.target_delay_ns > 0);
    std::printf("[jitter] received=%llu accepted=%llu delivered=%llu target=%.1fms "
                "latency=%s\n",
                static_cast<unsigned long long>(stats.jitter.received),
                static_cast<unsigned long long>(stats.jitter.accepted),
                static_cast<unsigned long long>(stats.jitter.delivered),
                static_cast<double>(stats.jitter.target_delay_ns) / 1e6,
                stats.latency.Summary().c_str());
}

/// @brief 2) jitter 关闭 vs 开启：两者都正确，开启延迟不低于关闭。
void TestJitterVersusPlain() {
    MediaConfig plain = BaseConfig(CodecKind::kDeltaI420, 320, 240);
    plain.jitter = false;
    const LoopStats no_jitter = RunLoop(plain, 80);

    MediaConfig with_jitter = plain;
    with_jitter.jitter = true;
    with_jitter.jitter_chunk_bytes = 8192;
    const LoopStats jitter = RunLoop(with_jitter, 80);

    MEDIA_CHECK(no_jitter.ok);
    MEDIA_CHECK(jitter.ok);
    MEDIA_CHECK_EQ(no_jitter.wrong, 0U);
    MEDIA_CHECK_EQ(jitter.wrong, 0U);
    MEDIA_CHECK_EQ(no_jitter.delivered, 80U);
    MEDIA_CHECK_EQ(jitter.delivered, 80U);
    // jitter 在无空洞时立即交付，p50 与 plain 同级；这里只排除「显著变慢」。
    MEDIA_CHECK_GE(no_jitter.latency.p50() + 2.0, jitter.latency.p50());
    std::printf("[jitter-vs-plain] plain p50=%.3fms jitter p50=%.3fms\n", no_jitter.latency.p50(),
                jitter.latency.p50());
}

/// @brief 3) 发送端 degradation：带宽阶梯 → 档位/尺寸/帧率真的变化。
void TestSenderDegradation() {
    MediaConfig sender_config = BaseConfig(CodecKind::kDeltaI420, 320, 240);
    sender_config.degradation = true;
    sender_config.keyframe_interval = 15;
    sender_config.degradation_config.downgrade_hold_ms = 30;
    sender_config.degradation_config.upgrade_hold_ms = 60000;
    sender_config.degradation_config.max_downgrade_step = 2;
    MediaConfig receiver_config = sender_config;
    receiver_config.degradation = false;

    std::string error;
    auto receiver = MediaReceiver::Create(receiver_config, &error);
    MEDIA_CHECK(receiver != nullptr);
    if (!receiver) {
        return;
    }
    auto sender = MediaSender::Create(sender_config, receiver->local_port(), &error);
    MEDIA_CHECK(sender != nullptr);
    if (!sender) {
        return;
    }

    std::map<std::uint32_t, std::uint64_t> expected;
    std::uint64_t delivered_correct = 0;
    std::uint64_t delivered_wrong = 0;
    std::uint64_t events = 0;
    std::uint64_t sent = 0;
    const std::uint64_t bandwidths[] = {2'000'000, 700'000, 320'000, 120'000};
    for (std::uint64_t bps : bandwidths) {
        for (int i = 0; i < 50; ++i) {
            NetworkSample sample;
            sample.available_bps = bps;
            sample.loss_rate = 0.0;
            sample.valid = true;
            if (sender->OnNetworkSample(sample).changed) {
                ++events;
            }
            VideoFrame captured;
            std::size_t encoded_bytes = 0;
            MEDIA_CHECK(sender->SendNext(&captured, &encoded_bytes));
            if (encoded_bytes == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            ++sent;
            expected[captured.frame_id] = ToI420(captured).Checksum();
            const std::uint64_t before = receiver->frames_delivered();
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            while (receiver->frames_delivered() == before &&
                   std::chrono::steady_clock::now() < deadline) {
                receiver->Pump(1);
            }
            if (receiver->frames_delivered() > before) {
                const VideoFrame& got = receiver->last_frame();
                const auto it = expected.find(got.frame_id);
                if (it != expected.end() && got.Checksum() == it->second) {
                    ++delivered_correct;
                } else {
                    ++delivered_wrong;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
    MEDIA_CHECK_GE(events, 3U);
    MEDIA_CHECK(sender->degradation() != nullptr);
    MEDIA_CHECK_EQ(static_cast<int>(sender->degradation()->level()),
                   static_cast<int>(QualityLevel::kMinimal));
    MEDIA_CHECK_EQ(sender->current_width(), 160);
    MEDIA_CHECK_EQ(sender->current_height(), 120);
    MEDIA_CHECK_EQ(sender->degradation()->target_fps(), 10);
    MEDIA_CHECK_EQ(delivered_wrong, 0U);
    MEDIA_CHECK_GE(delivered_correct, 10U);
    MEDIA_CHECK(receiver->last_frame().width == 160);
    MEDIA_CHECK(receiver->last_frame().height == 120);
    std::printf("[degradation] events=%llu sent=%llu correct=%llu wrong=%llu final=%s %dx%d@%d\n",
                static_cast<unsigned long long>(events),
                static_cast<unsigned long long>(sent),
                static_cast<unsigned long long>(delivered_correct),
                static_cast<unsigned long long>(delivered_wrong),
                QualityLevelName(sender->degradation()->level()), sender->current_width(),
                sender->current_height(), sender->degradation()->target_fps());
}

/// @brief 4) 收端 degradation 帧率门控：低带宽时实际跳过部分帧。
void TestReceiverFpsGate() {
    MediaConfig receiver_config = BaseConfig(CodecKind::kDeltaI420, 320, 240);
    receiver_config.degradation = true;
    receiver_config.degradation_config.downgrade_hold_ms = 20;
    receiver_config.degradation_config.upgrade_hold_ms = 60000;
    MediaConfig sender_config = receiver_config;
    sender_config.degradation = false;

    std::string error;
    auto receiver = MediaReceiver::Create(receiver_config, &error);
    MEDIA_CHECK(receiver != nullptr);
    if (!receiver) {
        return;
    }
    auto sender = MediaSender::Create(sender_config, receiver->local_port(), &error);
    MEDIA_CHECK(sender != nullptr);
    if (!sender) {
        return;
    }
    NetworkSample low;
    low.available_bps = 100'000;
    low.loss_rate = 0.2;
    low.valid = true;
    std::uint64_t sent = 0;
    for (int i = 0; i < 80; ++i) {
        receiver->OnNetworkSample(low);
        VideoFrame captured;
        std::size_t encoded_bytes = 0;
        MEDIA_CHECK(sender->SendNext(&captured, &encoded_bytes));
        if (encoded_bytes == 0) {
            continue;
        }
        ++sent;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
        const std::uint64_t before = receiver->frames_delivered();
        while (receiver->frames_delivered() == before &&
               std::chrono::steady_clock::now() < deadline) {
            receiver->Pump(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    for (int i = 0; i < 200 && receiver->frames_delivered() < sent; ++i) {
        receiver->Pump(2);
    }
    MEDIA_CHECK_GE(sent, 40U);
    MEDIA_CHECK_GE(receiver->frames_skipped(), 1U);
    MEDIA_CHECK(receiver->frames_delivered() < sent);
    std::printf("[fps-gate] sent=%llu delivered=%llu skipped=%llu\n",
                static_cast<unsigned long long>(sent),
                static_cast<unsigned long long>(receiver->frames_delivered()),
                static_cast<unsigned long long>(receiver->frames_skipped()));
}

/// @brief 5) jitter 分块边界：小 chunk 让关键帧切成多块，拼帧仍逐字节一致。
void TestJitterChunkBoundary() {
    MediaConfig config = BaseConfig(CodecKind::kDeltaI420, 160, 120);
    config.jitter = true;
    config.jitter_chunk_bytes = 512;  // 关键帧 13KB → 26 块
    config.jitter_config.base_delay_ns = 3'000'000;
    config.jitter_config.capacity = 512;
    const LoopStats stats = RunLoop(config, 40);
    MEDIA_CHECK(stats.ok);
    MEDIA_CHECK_EQ(stats.delivered, 40U);
    MEDIA_CHECK_EQ(stats.wrong, 0U);
    MEDIA_CHECK(stats.jitter.accepted > 40U);  // 每帧至少 2 块（关键帧 26 块）
    std::printf("[chunk] accepted_blocks=%llu delivered_blocks=%llu\n",
                static_cast<unsigned long long>(stats.jitter.accepted),
                static_cast<unsigned long long>(stats.jitter.delivered));
}

/// @brief 顺手确认 SHA/校验路径自身的确定性（防止比对函数本身有 bug）。
void TestSelfCheck() {
    const std::uint8_t bytes[] = {1, 2, 3, 4, 5};
    MEDIA_CHECK_EQ(HashBytes(bytes, sizeof(bytes)), HashBytes(bytes, sizeof(bytes)));
    MEDIA_CHECK(HashBytes(bytes, sizeof(bytes)) != HashBytes(bytes + 1, sizeof(bytes) - 1));
    VideoFrame frame;
    frame.width = 3;
    frame.height = 2;
    frame.data = {0, 1, 2, 3, 4, 5};
    MEDIA_CHECK_EQ(frame.Checksum(), frame.Checksum());
}

}  // namespace

int main() {
    TestSelfCheck();
    TestJitterLoopback();
    TestJitterVersusPlain();
    TestSenderDegradation();
    TestReceiverFpsGate();
    TestJitterChunkBoundary();
    return media_test::Finish("test_media_e2e_jitter");
}
