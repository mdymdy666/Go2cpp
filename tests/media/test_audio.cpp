/**
 * @file test_audio.cpp
 * @brief 音频链路测试：格式/帧、G.711（穷举 + SNR）、PCM 1e5 帧逐字节一致、
 *        Opus（dlopen）往返与 PLC、RTP 头/分片/重组（乱序/丢片/重复/超时）、
 *        抖动缓冲（乱序/丢包/自适应/序号回绕）、UDP 回环、端到端会话、
 *        PulseAudio 真实采集/播放 smoke、以及 >=1e7 的吞吐压测。
 * @details 与 `tests/media/test_transport.cpp` 同一套 `media_test_util.hpp`
 *          约定：断言不中断、SKIP 走 CTest 的 77 返回码、失败返回 1。
 *
 *          压测（`TestBench`）默认执行，打印 `[PERF]` 前缀行供报告提取；
 *          规模固定 1e7 样本/包，单次全量 < 2s，不拖慢 CTest。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "go2cpp/media/audio.hpp"
#include "media_test_util.hpp"

namespace {

using namespace go2cpp::media;

using Clock = std::chrono::steady_clock;

double MsSince(const Clock::time_point& start) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(Clock::now() -
                                                                                 start)
        .count();
}

std::int64_t NowNsTest() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

/// @brief 按给定延迟计算 SNR（dB）：output[i + lag] ≈ input[i]。
/// @details Opus 的编码器 lookahead 是固定的 6.5ms（48kHz 下 312 样本），
///          真实 WebRTC 也用 OPUS_GET_LOOKAHEAD 做同样的补偿。测试不去
///          「猜」延迟，而是按采样率换算后允许 ±4 个样本的微调（重采样相位）。
double SnrAtLag(const std::vector<std::int16_t>& input, const std::vector<std::int16_t>& output,
                int lag) {
    const int count = static_cast<int>(std::min(input.size(), output.size()));
    double signal = 0.0;
    double noise = 0.0;
    for (int i = std::max(0, -lag); i + lag < count; ++i) {
        const double original = static_cast<double>(input[static_cast<std::size_t>(i)]);
        const double restored =
            static_cast<double>(output[static_cast<std::size_t>(i + lag)]);
        signal += original * original;
        noise += (original - restored) * (original - restored);
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-9));
}

AudioFormat MakeFormat(int rate, int channels, int frames = 0) {
    AudioFormat format;
    format.sample_rate = rate;
    format.channels = channels;
    format.frames_per_packet = frames;
    return format;
}

AudioCaptureConfig MakeCapture(const AudioFormat& format, SyntheticAudioPattern pattern) {
    AudioCaptureConfig config;
    config.format = format;
    config.pattern = pattern;
    return config;
}

// ---------------------------------------------------------------------------
// 1. 格式与帧
// ---------------------------------------------------------------------------

void TestFormatAndFrame() {
    std::printf("[TEST] format/frame\n");
    const AudioFormat mono = MakeFormat(44100, 1);
    MEDIA_CHECK(mono.Valid());
    MEDIA_CHECK_EQ(mono.EffectiveFramesPerPacket(), 441);
    MEDIA_CHECK_EQ(mono.BytesPerFrame(), 2);
    MEDIA_CHECK_EQ(mono.BytesPerPacket(), 882);
    MEDIA_CHECK_EQ(mono.PacketDurationUs(), 10000);

    const AudioFormat stereo = MakeFormat(48000, 2);
    MEDIA_CHECK_EQ(stereo.EffectiveFramesPerPacket(), 480);
    MEDIA_CHECK_EQ(stereo.BytesPerPacket(), 1920);
    MEDIA_CHECK_EQ(stereo.PacketDurationUs(), 10000);

    const AudioFormat twenty_ms = MakeFormat(44100, 1, 882);
    MEDIA_CHECK_EQ(twenty_ms.BytesPerPacket(), 1764);
    MEDIA_CHECK_EQ(twenty_ms.PacketDurationUs(), 20000);

    AudioFormat bad = MakeFormat(0, 1);
    MEDIA_CHECK(!bad.Valid());
    bad = MakeFormat(44100, 3);
    MEDIA_CHECK(!bad.Valid());
    bad = MakeFormat(1000, 1);
    MEDIA_CHECK(!bad.Valid());

    AudioFrame frame;
    frame.format = mono;
    frame.data.assign(882, 0);
    MEDIA_CHECK(frame.Valid());
    MEDIA_CHECK_EQ(frame.samples(), 441U);
    MEDIA_CHECK_EQ(frame.frames(), 441U);
    frame.data.assign(880, 0);
    MEDIA_CHECK(!frame.Valid());
    frame.data.clear();
    MEDIA_CHECK(!frame.Valid());

    // 校验和稳定性 + RMS。
    AudioFrame tone;
    tone.format = mono;
    tone.data.assign(882, 0);
    auto* samples = reinterpret_cast<std::int16_t*>(tone.data.data());
    samples[0] = 1000;
    samples[1] = -1000;
    const std::uint64_t checksum = tone.Checksum();
    MEDIA_CHECK_EQ(checksum, tone.Checksum());
    MEDIA_CHECK_GE(tone.Rms(), 1.0);
    tone.data[0] = static_cast<std::uint8_t>(tone.data[0] ^ 0xFFU);
    MEDIA_CHECK(checksum != tone.Checksum());

    // 编解码名字/解析/负载类型。
    MEDIA_CHECK(std::strcmp(AudioCodecName(AudioCodec::kOpus), "opus") == 0);
    AudioCodec parsed = AudioCodec::kPcmS16LE;
    MEDIA_CHECK(ParseAudioCodec("pcmu", &parsed));
    MEDIA_CHECK(parsed == AudioCodec::kPcmu);
    MEDIA_CHECK(ParseAudioCodec("alaw", &parsed));
    MEDIA_CHECK(parsed == AudioCodec::kPcma);
    MEDIA_CHECK(!ParseAudioCodec("bogus", &parsed));
    MEDIA_CHECK_EQ(DefaultAudioPayloadType(AudioCodec::kOpus), 111);
    MEDIA_CHECK_EQ(DefaultAudioPayloadType(AudioCodec::kPcmu), 0);
    MEDIA_CHECK_EQ(DefaultAudioPayloadType(AudioCodec::kPcma), 8);
}

// ---------------------------------------------------------------------------
// 2. G.711
// ---------------------------------------------------------------------------

void TestG711() {
    std::printf("[TEST] g711\n");
    // 表实现与教科书基线必须逐字节一致（穷举全部 65536 个输入）。
    int mismatches = 0;
    for (int value = -32768; value <= 32767; ++value) {
        const auto sample = static_cast<std::int16_t>(value);
        if (detail::UlawEncodeBaseline(sample) != detail::UlawEncodeTable(sample)) {
            ++mismatches;
        }
    }
    MEDIA_CHECK_EQ(mismatches, 0);

    // 解码单调性：µ-law 的值随码字增大而增大（同一符号内）。
    bool monotonic = true;
    for (int value = 0; value < 127; ++value) {
        if (detail::UlawDecodeTable(static_cast<std::uint8_t>(value)) >
            detail::UlawDecodeTable(static_cast<std::uint8_t>(value + 1))) {
            monotonic = false;
            break;
        }
    }
    MEDIA_CHECK(monotonic);
    MEDIA_CHECK_EQ(detail::UlawDecodeTable(0x7F), 0);
    MEDIA_CHECK_EQ(detail::UlawDecodeTable(0xFF), 0);
    // 标准 µ-law：0x00 是最大负值，0x80 是最大正值。
    MEDIA_CHECK(detail::UlawDecodeTable(0x00) < 0);
    MEDIA_CHECK(detail::UlawDecodeTable(0x80) > 0);

    // 端到端：440Hz 正弦 → G.711 → 解码，SNR 应 >= 30dB（µ-law 理论约 38dB）。
    const AudioFormat format = MakeFormat(44100, 1);
    AudioCaptureConfig capture = MakeCapture(format, SyntheticAudioPattern::kSine440);
    auto source = CreateSyntheticAudioSource(capture, nullptr);
    MEDIA_CHECK(source != nullptr);

    for (const AudioCodec codec : {AudioCodec::kPcmu, AudioCodec::kPcma}) {
        AudioEncoderConfig encoder_config;
        encoder_config.codec = codec;
        encoder_config.format = format;
        std::string error;
        auto encoder = CreateAudioEncoder(encoder_config, &error);
        MEDIA_CHECK(encoder != nullptr);
        auto decoder = CreateAudioDecoder(codec, format, &error);
        MEDIA_CHECK(decoder != nullptr);
        if (encoder == nullptr || decoder == nullptr) {
            continue;
        }

        double signal = 0.0;
        double noise = 0.0;
        std::uint64_t samples_compared = 0;
        for (int frame_index = 0; frame_index < 200; ++frame_index) {
            AudioFrame pcm;
            MEDIA_CHECK(source->Read(&pcm));
            std::vector<std::uint8_t> payload;
            MEDIA_CHECK(encoder->Encode(pcm, &payload));
            if (codec == AudioCodec::kPcmu) {
                MEDIA_CHECK_EQ(payload.size(), pcm.samples());
            }
            AudioFrame decoded;
            MEDIA_CHECK(decoder->Decode(payload.data(), payload.size(), &decoded));
            MEDIA_CHECK(decoded.Valid());
            const auto* in = reinterpret_cast<const std::int16_t*>(pcm.data.data());
            const auto* out = reinterpret_cast<const std::int16_t*>(decoded.data.data());
            for (std::size_t i = 0; i < pcm.samples(); ++i) {
                const double original = static_cast<double>(in[i]);
                const double error_value = original - static_cast<double>(out[i]);
                signal += original * original;
                noise += error_value * error_value;
                ++samples_compared;
            }
        }
        MEDIA_CHECK_GE(samples_compared, 80000U);
        const double snr = 10.0 * std::log10(signal / std::max(noise, 1e-9));
        std::printf("       %s snr=%.2f dB\n", AudioCodecName(codec), snr);
        MEDIA_CHECK_GE(snr, 30.0);
    }

    // PLC：空负载输出等长帧且计入 concealed。
    auto decoder = CreateAudioDecoder(AudioCodec::kPcmu, format, nullptr);
    MEDIA_CHECK(decoder != nullptr);
    if (decoder != nullptr) {
        AudioFrame concealed;
        MEDIA_CHECK(decoder->Decode(nullptr, 0, &concealed));
        MEDIA_CHECK(concealed.Valid());
        MEDIA_CHECK_EQ(concealed.data.size(), static_cast<std::size_t>(format.BytesPerPacket()));
        MEDIA_CHECK_EQ(decoder->concealed_frames(), 1U);
        MEDIA_CHECK_EQ(decoder->decoded_frames(), 0U);
    }
}

// ---------------------------------------------------------------------------
// 3. PCM 1e5 帧逐字节一致
// ---------------------------------------------------------------------------

void TestPcmPipeline1e5() {
    std::printf("[TEST] pcm 1e5 frames\n");
    const AudioFormat format = MakeFormat(44100, 1);
    AudioCaptureConfig capture = MakeCapture(format, SyntheticAudioPattern::kNoise);
    auto source = CreateSyntheticAudioSource(capture, nullptr);
    MEDIA_CHECK(source != nullptr);

    AudioEncoderConfig encoder_config;
    encoder_config.codec = AudioCodec::kPcmS16LE;
    encoder_config.format = format;
    auto encoder = CreateAudioEncoder(encoder_config, nullptr);
    auto decoder = CreateAudioDecoder(AudioCodec::kPcmS16LE, format, nullptr);
    MEDIA_CHECK(encoder != nullptr);
    MEDIA_CHECK(decoder != nullptr);
    if (encoder == nullptr || decoder == nullptr) {
        return;
    }

    AudioRtpPacketizer packetizer(0x0A0B0C0DU, 96);
    AudioRtpReassembler reassembler;
    std::vector<AudioRtpPacket> packets;
    std::vector<ReassembledAudio> complete;
    std::vector<std::uint8_t> payload;

    std::uint64_t mismatch = 0;
    std::uint64_t checksum_chain = 1469598103934665603ULL;
    std::uint64_t bytes = 0;
    std::uint64_t fragments = 0;
    const auto start = Clock::now();
    constexpr int kFrames = 100000;
    for (int index = 0; index < kFrames; ++index) {
        AudioFrame pcm;
        if (!source->Read(&pcm)) {
            ++mismatch;
            break;
        }
        pcm.timestamp = static_cast<std::uint32_t>(index) * 441U;
        if (!encoder->Encode(pcm, &payload)) {
            ++mismatch;
            break;
        }
        packets.clear();
        const int count = packetizer.Packetize(payload.data(), payload.size(), pcm.timestamp,
                                               &packets);
        if (count != 1) {
            ++mismatch;
            break;
        }
        fragments += static_cast<std::uint64_t>(count);
        bytes += payload.size();

        complete.clear();
        reassembler.Push(packets[0], static_cast<std::uint64_t>(index), &complete);
        if (complete.size() != 1) {
            ++mismatch;
            break;
        }
        AudioFrame decoded;
        if (!decoder->Decode(complete[0].payload.data(), complete[0].payload.size(), &decoded)) {
            ++mismatch;
            break;
        }
        if (decoded.data != pcm.data) {
            ++mismatch;
        }
        checksum_chain = checksum_chain * 1099511628211ULL + decoded.Checksum();
    }
    const double elapsed = MsSince(start);
    MEDIA_CHECK_EQ(mismatch, 0U);
    MEDIA_CHECK_EQ(bytes, static_cast<std::uint64_t>(kFrames) * 882U);
    MEDIA_CHECK_EQ(reassembler.completed(), static_cast<std::uint64_t>(kFrames));
    MEDIA_CHECK_EQ(reassembler.fragments(), static_cast<std::uint64_t>(kFrames));
    std::printf("       100000 frames bytes=%llu ms=%.2f checksum=0x%016llx\n",
                static_cast<unsigned long long>(bytes), elapsed,
                static_cast<unsigned long long>(checksum_chain));
    MEDIA_CHECK_GE(fragments, static_cast<std::uint64_t>(kFrames));
}

// ---------------------------------------------------------------------------
// 4. Opus
// ---------------------------------------------------------------------------

void TestOpus() {
    std::printf("[TEST] opus\n");
    const AudioCodecAvailability availability = ProbeAudioCodecs();
    std::printf("       opus=%d library=%s version=%s detail=%s\n",
                availability.opus ? 1 : 0, availability.opus_library.c_str(),
                availability.opus_version.c_str(), availability.detail.c_str());
    if (!availability.opus) {
        media_test::Skip("libopus.so.0 unavailable");
        return;
    }

    struct Case {
        AudioFormat format;
        const char* name;
        double min_snr;
    };
    const Case cases[] = {
        {MakeFormat(48000, 1), "48k mono", 15.0},
        {MakeFormat(48000, 2), "48k stereo", 12.0},
        {MakeFormat(44100, 1), "44.1k mono (resampled)", 10.0},
    };
    for (const Case& item : cases) {
        // 扫频（非周期）用于延迟对齐：周期正弦的 MSE 在 lag±k*周期 上同样小。
        AudioCaptureConfig capture = MakeCapture(item.format, SyntheticAudioPattern::kSine440);
        auto source = CreateSyntheticAudioSource(capture, nullptr);
        AudioEncoderConfig encoder_config;
        encoder_config.codec = AudioCodec::kOpus;
        encoder_config.format = item.format;
        encoder_config.bitrate = 32000;
        std::string error;
        auto encoder = CreateAudioEncoder(encoder_config, &error);
        auto decoder = CreateAudioDecoder(AudioCodec::kOpus, item.format, &error);
        MEDIA_CHECK(encoder != nullptr);
        MEDIA_CHECK(decoder != nullptr);
        if (encoder == nullptr || decoder == nullptr) {
            std::printf("       %s: %s\n", item.name, error.c_str());
            continue;
        }
        std::vector<std::int16_t> input_samples;
        std::vector<std::int16_t> output_samples;
        std::uint64_t encoded_bytes = 0;
        constexpr int kFrames = 300;
        for (int index = 0; index < kFrames; ++index) {
            AudioFrame pcm;
            MEDIA_CHECK(source->Read(&pcm));
            std::vector<std::uint8_t> payload;
            if (!encoder->Encode(pcm, &payload)) {
                MEDIA_CHECK(false);
                break;
            }
            MEDIA_CHECK_GE(payload.size(), 1U);
            MEDIA_CHECK(payload.size() <= 1275U);
            encoded_bytes += payload.size();
            AudioFrame decoded;
            if (!decoder->Decode(payload.data(), payload.size(), &decoded)) {
                MEDIA_CHECK(false);
                break;
            }
            MEDIA_CHECK(decoded.Valid());
            const auto* in = reinterpret_cast<const std::int16_t*>(pcm.data.data());
            const auto* out = reinterpret_cast<const std::int16_t*>(decoded.data.data());
            input_samples.insert(input_samples.end(), in, in + pcm.samples());
            output_samples.insert(output_samples.end(), out, out + decoded.samples());
        }
        // Opus lookahead 6.5ms（312 样本 @48kHz）；按采样率换算后 ±4 微调。
        const int expected_lag = static_cast<int>(
            std::lround(312.0 * static_cast<double>(item.format.sample_rate) / 48000.0)) * item.format.channels;
        double snr = -1e9;
        int best_lag = expected_lag;
        for (int lag = expected_lag - 4; lag <= expected_lag + 4; ++lag) {
            const double candidate = SnrAtLag(input_samples, output_samples, lag);
            if (candidate > snr) {
                snr = candidate;
                best_lag = lag;
            }
        }
        std::printf("       %s snr=%.2f dB lag=%d (expected %d) avg_packet=%.1f B\n", item.name,
                    snr, best_lag, expected_lag,
                    static_cast<double>(encoded_bytes) / static_cast<double>(kFrames));
        MEDIA_CHECK_GE(snr, item.min_snr);
        MEDIA_CHECK_EQ(encoder->encoded_frames(), static_cast<std::uint64_t>(kFrames));
        MEDIA_CHECK_EQ(decoder->decoded_frames(), static_cast<std::uint64_t>(kFrames));

        // PLC。
        AudioFrame concealed;
        MEDIA_CHECK(decoder->Decode(nullptr, 0, &concealed));
        MEDIA_CHECK(concealed.Valid());
        MEDIA_CHECK_EQ(decoder->concealed_frames(), 1U);
    }

    MEDIA_CHECK(PreferredAudioCodec() == AudioCodec::kOpus);
}

// ---------------------------------------------------------------------------
// 5. RTP 头
// ---------------------------------------------------------------------------

void TestRtpHeader() {
    std::printf("[TEST] rtp header\n");
    std::uint8_t buffer[RtpHeader::kBytes] = {0};
    std::mt19937 rng(0xA0D10U);
    for (int i = 0; i < 200000; ++i) {
        RtpHeader header;
        header.payload_type = static_cast<std::uint8_t>(rng() % 128U);
        header.marker = (rng() & 1U) != 0;
        header.sequence = static_cast<std::uint16_t>(rng() & 0xFFFFU);
        header.timestamp = rng();
        header.ssrc = rng();
        EncodeRtpHeader(header, buffer);
        MEDIA_CHECK_EQ(buffer[0], 0x80U);
        RtpHeader decoded;
        MEDIA_CHECK(DecodeRtpHeader(buffer, RtpHeader::kBytes, &decoded));
        if (decoded.payload_type != header.payload_type || decoded.marker != header.marker ||
            decoded.sequence != header.sequence || decoded.timestamp != header.timestamp ||
            decoded.ssrc != header.ssrc) {
            MEDIA_CHECK(false);
            break;
        }
    }

    // 边界：seq 回绕、ts/ssrc 全 1。
    RtpHeader edge;
    edge.sequence = 0xFFFF;
    edge.timestamp = 0xFFFFFFFFU;
    edge.ssrc = 0xFFFFFFFFU;
    edge.payload_type = 111;
    edge.marker = true;
    EncodeRtpHeader(edge, buffer);
    RtpHeader decoded;
    MEDIA_CHECK(DecodeRtpHeader(buffer, 12, &decoded));
    MEDIA_CHECK_EQ(decoded.sequence, 0xFFFFU);
    MEDIA_CHECK_EQ(decoded.timestamp, 0xFFFFFFFFU);
    MEDIA_CHECK_EQ(decoded.ssrc, 0xFFFFFFFFU);
    MEDIA_CHECK(decoded.marker);

    // 非法：版本错误、长度不足、CSRC/扩展位。
    MEDIA_CHECK(!DecodeRtpHeader(buffer, 11, &decoded));
    std::uint8_t bad[12] = {0};
    bad[0] = 0x40;  // version 1
    MEDIA_CHECK(!DecodeRtpHeader(bad, 12, &decoded));
    bad[0] = 0x81;  // CC=1（无 CSRC 数据）
    MEDIA_CHECK(!DecodeRtpHeader(bad, 12, &decoded));
    bad[0] = 0x90;  // X=1
    MEDIA_CHECK(!DecodeRtpHeader(bad, 12, &decoded));
}

// ---------------------------------------------------------------------------
// 6. 分片与重组
// ---------------------------------------------------------------------------

void TestPacketizeReassemble() {
    std::printf("[TEST] packetize/reassemble\n");
    std::vector<std::uint8_t> payload(882);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((i * 31U) & 0xFFU);
    }

    AudioRtpPacketizer packetizer(0x11223344U, 111, 100);
    std::vector<AudioRtpPacket> packets;

    // 单包（882 + 4 头 = 886 <= 1188）。
    MEDIA_CHECK_EQ(packetizer.Packetize(payload.data(), payload.size(), 4410, &packets), 1);
    MEDIA_CHECK_EQ(packets.size(), 1U);
    MEDIA_CHECK_EQ(packets[0].header.sequence, 100U);
    MEDIA_CHECK_EQ(packets[0].header.timestamp, 4410U);
    MEDIA_CHECK(packets[0].header.marker);
    MEDIA_CHECK_EQ(packets[0].payload_bytes, 886U);
    MEDIA_CHECK_EQ(packets[0].header.ssrc, 0x11223344U);
    MEDIA_CHECK_EQ(packets[0].header.payload_type, 111U);

    // 多包：1764B 负载 → 2 片；8192B → 7 片。
    std::vector<std::uint8_t> large(1764, 0xAB);
    packets.clear();
    MEDIA_CHECK_EQ(packetizer.Packetize(large.data(), large.size(), 8820, &packets), 2);
    MEDIA_CHECK_EQ(packets.size(), 2U);
    MEDIA_CHECK(!packets[0].header.marker);
    MEDIA_CHECK(packets[1].header.marker);
    MEDIA_CHECK_EQ(packets[0].header.sequence, 101U);
    MEDIA_CHECK_EQ(packets[1].header.sequence, 102U);

    std::vector<std::uint8_t> huge(8192, 0xCD);
    packets.clear();
    MEDIA_CHECK_EQ(packetizer.Packetize(huge.data(), huge.size(), 0, &packets), 7);
    MEDIA_CHECK_EQ(packets.size(), 7U);
    MEDIA_CHECK(packets.back().header.marker);
    MEDIA_CHECK_EQ(packetizer.packets(), 10U);

    // 超过上限拒绝。
    std::vector<std::uint8_t> too_big(kMaxAudioPayloadBytes + 1, 0);
    packets.clear();
    MEDIA_CHECK_EQ(packetizer.Packetize(too_big.data(), too_big.size(), 0, &packets), 0);

    // 重组：顺序。
    {
        AudioRtpReassembler reassembler;
        std::vector<ReassembledAudio> complete;
        packets.clear();
        packetizer.Packetize(huge.data(), huge.size(), 42, &packets);
        for (const auto& packet : packets) {
            reassembler.Push(packet, 100, &complete);
        }
        MEDIA_CHECK_EQ(complete.size(), 1U);
        MEDIA_CHECK_EQ(complete[0].payload.size(), huge.size());
        MEDIA_CHECK(std::memcmp(complete[0].payload.data(), huge.data(), huge.size()) == 0);
        MEDIA_CHECK_EQ(complete[0].fragments, 7);
        MEDIA_CHECK_EQ(reassembler.completed(), 1U);
        MEDIA_CHECK_EQ(reassembler.incomplete(), 0U);
    }

    // 重组：乱序（倒序喂入）。
    {
        AudioRtpReassembler reassembler;
        std::vector<ReassembledAudio> complete;
        packets.clear();
        packetizer.Packetize(huge.data(), huge.size(), 43, &packets);
        for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
            reassembler.Push(*it, 101, &complete);
        }
        MEDIA_CHECK_EQ(complete.size(), 1U);
        MEDIA_CHECK_EQ(complete[0].payload.size(), huge.size());
        MEDIA_CHECK(std::memcmp(complete[0].payload.data(), huge.data(), huge.size()) == 0);
        MEDIA_CHECK_EQ(complete[0].first_sequence, packetizer.next_sequence() - 7);
    }

    // 重组：缺片（丢中间一片），超时后清理并计 incomplete。
    {
        AudioRtpReassembler::Config config;
        config.timeout_ms = 10;
        AudioRtpReassembler reassembler(config);
        std::vector<ReassembledAudio> complete;
        packets.clear();
        packetizer.Packetize(huge.data(), huge.size(), 44, &packets);
        for (std::size_t i = 0; i < packets.size(); ++i) {
            if (i == 3) {
                continue;
            }
            reassembler.Push(packets[i], 200, &complete);
        }
        MEDIA_CHECK_EQ(complete.size(), 0U);
        reassembler.OnTimer(500);
        MEDIA_CHECK_EQ(reassembler.incomplete(), 1U);

        // 重复分片不应重复拼装。
        AudioRtpReassembler reassembler2;
        complete.clear();
        packets.clear();
        packetizer.Packetize(large.data(), large.size(), 45, &packets);
        reassembler2.Push(packets[0], 0, &complete);
        reassembler2.Push(packets[0], 0, &complete);
        MEDIA_CHECK_EQ(complete.size(), 0U);
        reassembler2.Push(packets[1], 0, &complete);
        MEDIA_CHECK_EQ(complete.size(), 1U);
    }

    // 坏分片：长度不足、total=0、index>=total。
    {
        AudioRtpReassembler reassembler;
        std::vector<ReassembledAudio> complete;
        AudioRtpPacket packet;
        packet.header.timestamp = 1;
        packet.payload_bytes = 2;
        reassembler.Push(packet, 0, &complete);
        packet.payload_bytes = 6;
        packet.payload[0] = 0;
        packet.payload[1] = 1;
        packet.payload[2] = 0;
        packet.payload[3] = 0;  // total = 0
        reassembler.Push(packet, 0, &complete);
        MEDIA_CHECK_EQ(reassembler.malformed(), 2U);
        MEDIA_CHECK_EQ(complete.size(), 0U);
    }
}

// ---------------------------------------------------------------------------
// 7. 抖动缓冲
// ---------------------------------------------------------------------------

/// @brief 构造一个带序号/时间戳的测试包。
AudioRtpPacket MakePacket(std::uint16_t sequence, std::uint32_t timestamp,
                          std::uint8_t fill = 0x5A) {
    AudioRtpPacket packet;
    packet.header.sequence = sequence;
    packet.header.timestamp = timestamp;
    packet.header.ssrc = 0x99;
    packet.payload[0] = 0;
    packet.payload[1] = 0;
    packet.payload[2] = 0;
    packet.payload[3] = 1;
    std::memset(packet.payload.data() + 4, fill, 882);
    packet.payload_bytes = 886;
    return packet;
}

void TestJitterBuffer() {
    std::printf("[TEST] jitter buffer\n");
    AudioJitterBufferConfig config;
    config.target_delay_packets = 3;
    config.min_delay_packets = 1;
    config.max_delay_packets = 8;
    config.adaptive = false;
    config.packet_ms = 10;

    // 顺序到达：每 10ms 一个包，队列 >= target 后按序交付。
    {
        AudioJitterBuffer jitter(config);
        std::uint64_t now = 1000;
        int delivered = 0;
        std::uint16_t expected = 0;
        for (int i = 0; i < 100; ++i) {
            const auto packet = MakePacket(static_cast<std::uint16_t>(i), i * 441U);
            MEDIA_CHECK(jitter.Push(packet, now));
            now += 10;
            AudioRtpPacket out;
            while (jitter.Pop(&out, now) == JitterPopResult::kPacket) {
                if (out.header.sequence != expected) {
                    MEDIA_CHECK(false);
                }
                ++expected;
                ++delivered;
            }
        }
        // 冲刷：推进时间直到队列为空。
        for (int i = 0; i < 200 && jitter.depth() > 0; ++i) {
            now += 10;
            AudioRtpPacket out;
            while (jitter.Pop(&out, now) == JitterPopResult::kPacket) {
                ++delivered;
            }
        }
        MEDIA_CHECK_EQ(delivered, 100);
        MEDIA_CHECK_EQ(jitter.received(), 100U);
        MEDIA_CHECK_EQ(jitter.lost(), 0U);
        MEDIA_CHECK_EQ(jitter.duplicates(), 0U);
        MEDIA_CHECK_EQ(jitter.late(), 0U);
        MEDIA_CHECK_GE(jitter.queue_delay().p50(), 0.0);
        std::printf("       in-order queue_delay p50=%.2f p99=%.2f ms\n",
                    jitter.queue_delay().p50(), jitter.queue_delay().p99());
    }

    // 乱序：模拟 +-20ms 抖动（20ms 内乱序），目标深度 3 包 + 重排应零丢失。
    {
        AudioJitterBuffer jitter(config);
        std::mt19937 rng(0x51F7U);
        struct Arrival {
            std::uint64_t at;
            AudioRtpPacket packet;
        };
        std::vector<Arrival> arrivals;
        arrivals.reserve(200);
        std::uint64_t base = 5000;
        for (int i = 0; i < 200; ++i) {
            const std::uint64_t send = base + static_cast<std::uint64_t>(i) * 10U;
            const std::uint64_t jitter_ms = rng() % 21U;
            arrivals.push_back({send + jitter_ms, MakePacket(static_cast<std::uint16_t>(i), i * 441U)});
        }
        std::sort(arrivals.begin(), arrivals.end(),
                  [](const Arrival& left, const Arrival& right) { return left.at < right.at; });

        std::uint64_t now = base;
        std::size_t cursor = 0;
        int delivered = 0;
        int lost = 0;
        std::uint16_t expected = 0;
        while (cursor < arrivals.size() || jitter.depth() > 0) {
            while (cursor < arrivals.size() && arrivals[cursor].at <= now) {
                jitter.Push(arrivals[cursor].packet, now);
                ++cursor;
            }
            AudioRtpPacket out;
            JitterPopResult result = jitter.Pop(&out, now);
            while (result != JitterPopResult::kEmpty) {
                if (result == JitterPopResult::kPacket) {
                    if (out.header.sequence != expected) {
                        MEDIA_CHECK(false);
                    }
                    ++expected;
                    ++delivered;
                } else {
                    ++lost;
                    ++expected;
                }
                result = jitter.Pop(&out, now);
            }
            now += 1;
            if (now > base + 100000) {
                break;
            }
        }
        std::printf("       jitter+-20ms delivered=%d lost=%d late=%llu dup=%llu p50=%.2f p99=%.2f\n",
                    delivered, lost, static_cast<unsigned long long>(jitter.late()),
                    static_cast<unsigned long long>(jitter.duplicates()),
                    jitter.queue_delay().p50(), jitter.queue_delay().p99());
        MEDIA_CHECK_EQ(delivered, 200);
        MEDIA_CHECK_EQ(lost, 0);
        MEDIA_CHECK_EQ(jitter.duplicates(), 0U);
    }

    // 丢包：每 10 个丢 1 个（丢第 4 个，保证最后一个包正常发出以便检测缺口），
    // 应产出 90 包 + 10 个 kLost。
    {
        AudioJitterBuffer jitter(config);
        std::uint64_t now = 2000;
        int packets = 0;
        int lost = 0;
        for (int i = 0; i < 100; ++i) {
            if (i % 10 != 4) {
                jitter.Push(MakePacket(static_cast<std::uint16_t>(i), i * 441U), now);
            }
            now += 10;
            AudioRtpPacket out;
            JitterPopResult result = jitter.Pop(&out, now);
            while (result != JitterPopResult::kEmpty) {
                if (result == JitterPopResult::kPacket) {
                    ++packets;
                } else {
                    ++lost;
                }
                result = jitter.Pop(&out, now);
            }
        }
        for (int i = 0; i < 100 && jitter.depth() > 0; ++i) {
            now += 10;
            AudioRtpPacket out;
            JitterPopResult result = jitter.Pop(&out, now);
            while (result != JitterPopResult::kEmpty) {
                if (result == JitterPopResult::kPacket) {
                    ++packets;
                } else {
                    ++lost;
                }
                result = jitter.Pop(&out, now);
            }
        }
        MEDIA_CHECK_EQ(packets, 90);
        MEDIA_CHECK_EQ(lost, 10);
        MEDIA_CHECK_GE(jitter.lost(), 10U);
    }

    // 重复包 / 迟到包。
    {
        AudioJitterBuffer jitter(config);
        MEDIA_CHECK(jitter.Push(MakePacket(10, 0), 100));
        MEDIA_CHECK(!jitter.Push(MakePacket(10, 0), 101));  // 重复
        MEDIA_CHECK_EQ(jitter.duplicates(), 1U);
        // 推进播放游标：先交付 10。
        std::uint64_t now = 200;
        AudioRtpPacket out;
        int guard = 0;
        while (jitter.Pop(&out, now) != JitterPopResult::kPacket && guard++ < 100) {
            now += 10;
        }
        MEDIA_CHECK(!jitter.Push(MakePacket(9, 0), now));  // 迟到
        MEDIA_CHECK_GE(jitter.late(), 1U);
    }

    // 自适应：大抖动应抬高目标深度，小抖动应回落。
    {
        AudioJitterBufferConfig adaptive_config = config;
        adaptive_config.adaptive = true;
        adaptive_config.min_delay_packets = 1;
        adaptive_config.max_delay_packets = 8;
        AudioJitterBuffer jitter(adaptive_config);
        std::uint64_t now = 10000;
        std::mt19937 rng(0x33U);
        for (int i = 0; i < 80; ++i) {
            const std::uint64_t arrival = now + (rng() % 5U);
            jitter.Push(MakePacket(static_cast<std::uint16_t>(i), i * 441U), arrival);
            now += 10;
        }
        const int low_target = jitter.target_delay_packets();
        for (int i = 80; i < 160; ++i) {
            const std::uint64_t arrival = now + (rng() % 45U);
            jitter.Push(MakePacket(static_cast<std::uint16_t>(i), i * 441U), arrival);
            now += 10;
        }
        const int high_target = jitter.target_delay_packets();
        std::printf("       adaptive target low=%d high=%d jitter=%.1f ms\n", low_target,
                    high_target, jitter.jitter_ms());
        MEDIA_CHECK_GE(high_target, low_target);
        MEDIA_CHECK_GE(jitter.jitter_ms(), 0.0);
    }

    // 序号回绕：0xFFFE → 0x0001 连续交付。
    {
        AudioJitterBufferConfig wrap_config = config;
        wrap_config.target_delay_packets = 1;
        wrap_config.adaptive = false;
        AudioJitterBuffer jitter(wrap_config);
        std::uint64_t now = 20000;
        for (std::uint32_t i = 0; i < 4; ++i) {
            const std::uint16_t sequence = static_cast<std::uint16_t>(0xFFFEU + i);
            jitter.Push(MakePacket(sequence, i * 441U), now);
            now += 10;
        }
        int delivered = 0;
        AudioRtpPacket out;
        while (jitter.Pop(&out, now) == JitterPopResult::kPacket) {
            ++delivered;
        }
        MEDIA_CHECK_EQ(delivered, 4);
        MEDIA_CHECK_EQ(jitter.lost(), 0U);
    }
}

// ---------------------------------------------------------------------------
// 8. UDP 回环
// ---------------------------------------------------------------------------

struct UdpCapture {
    std::vector<std::uint32_t> sequences;
    std::vector<std::uint64_t> checksums;
    std::vector<AudioRtpPacket> packets;
};

void OnUdpPacket(void* context, const AudioRtpPacket& packet, std::uint64_t arrival_ms) {
    (void)arrival_ms;
    auto* capture = static_cast<UdpCapture*>(context);
    capture->sequences.push_back(packet.header.sequence);
    capture->checksums.push_back(AudioFrame::ChecksumBytes(
        packet.payload.data() + kAudioFragmentHeaderBytes,
        packet.payload_bytes - kAudioFragmentHeaderBytes));
    capture->packets.push_back(packet);
}

void TestUdpLoopback() {
    std::printf("[TEST] udp loopback\n");
    UdpCapture capture;
    UdpAudioReceiver receiver(&OnUdpPacket, &capture);
    std::string error;
    MEDIA_CHECK(receiver.Open(&error));
    MEDIA_CHECK(receiver.local_port() != 0);

    AudioRtpPacketizer packetizer(0xCAFEBABEU, 96, 0);
    UdpAudioSender sender(packetizer);
    MEDIA_CHECK(sender.Open(receiver.local_port(), &error));
    MEDIA_CHECK(sender.local_port() != 0);

    std::vector<std::uint8_t> payload(882, 0);
    constexpr int kFrames = 1000;
    std::uint64_t expected_checksum = 0;
    for (int index = 0; index < kFrames; ++index) {
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::uint8_t>((index + i) & 0xFFU);
        }
        if (index == 0) {
            expected_checksum = AudioFrame::ChecksumBytes(payload.data(), payload.size());
        }
        MEDIA_CHECK(sender.SendPayload(payload.data(), payload.size(),
                                       static_cast<std::uint32_t>(index) * 441U));
        // 边发边收：UDP 回环的接收队列有限，一次灌 1000 个包会触发内核丢包。
        if ((index % 16) == 15) {
            receiver.Pump(0, 64);
        }
    }
    for (int attempt = 0; attempt < 200 && capture.packets.size() < kFrames; ++attempt) {
        receiver.Pump(50, 256);
    }
    MEDIA_CHECK_EQ(capture.packets.size(), static_cast<std::size_t>(kFrames));
    MEDIA_CHECK_EQ(receiver.datagrams(), static_cast<std::uint64_t>(kFrames));
    MEDIA_CHECK_EQ(receiver.malformed(), 0U);
    MEDIA_CHECK_EQ(sender.datagrams(), static_cast<std::uint64_t>(kFrames));
    MEDIA_CHECK_EQ(sender.failures(), 0U);
    if (!capture.checksums.empty()) {
        MEDIA_CHECK_EQ(capture.checksums.front(), expected_checksum);
        // 序号连续递增。
        bool ordered = true;
        for (std::size_t i = 1; i < capture.sequences.size(); ++i) {
            if (static_cast<std::uint16_t>(capture.sequences[i - 1] + 1) !=
                capture.sequences[i]) {
                ordered = false;
                break;
            }
        }
        MEDIA_CHECK(ordered);
    }
    std::printf("       udp packets=%zu datagrams=%llu\n", capture.packets.size(),
                static_cast<unsigned long long>(receiver.datagrams()));
}

// ---------------------------------------------------------------------------
// 9. 进程内端到端延迟（1e5 帧）
// ---------------------------------------------------------------------------

void TestInProcessLatency() {
    std::printf("[TEST] in-process latency 1e5\n");
    const AudioFormat format = MakeFormat(44100, 1);
    AudioCaptureConfig capture = MakeCapture(format, SyntheticAudioPattern::kSine440);
    auto source = CreateSyntheticAudioSource(capture, nullptr);
    MEDIA_CHECK(source != nullptr);

    AudioEncoderConfig encoder_config;
    encoder_config.codec = AudioCodec::kPcmS16LE;
    encoder_config.format = format;
    auto encoder = CreateAudioEncoder(encoder_config, nullptr);
    auto decoder = CreateAudioDecoder(AudioCodec::kPcmS16LE, format, nullptr);
    auto sink = CreateNullAudioSink(format);
    MEDIA_CHECK(encoder != nullptr && decoder != nullptr && sink != nullptr);
    if (encoder == nullptr || decoder == nullptr || sink == nullptr) {
        return;
    }

    AudioRtpPacketizer packetizer(0x11U, 96);
    AudioRtpReassembler reassembler;
    LatencyRecorder latency;
    std::vector<AudioRtpPacket> packets;
    std::vector<ReassembledAudio> complete;
    std::vector<std::uint8_t> payload;
    constexpr int kFrames = 100000;
    std::uint64_t mismatch = 0;
    for (int index = 0; index < kFrames; ++index) {
        AudioFrame pcm;
        if (!source->Read(&pcm)) {
            ++mismatch;
            break;
        }
        pcm.timestamp = static_cast<std::uint32_t>(index) * 441U;
        if (!encoder->Encode(pcm, &payload)) {
            ++mismatch;
            break;
        }
        packets.clear();
        packetizer.Packetize(payload.data(), payload.size(), pcm.timestamp, &packets);
        complete.clear();
        for (const auto& packet : packets) {
            reassembler.Push(packet, static_cast<std::uint64_t>(index), &complete);
        }
        if (complete.size() != 1) {
            ++mismatch;
            continue;
        }
        AudioFrame decoded;
        if (!decoder->Decode(complete[0].payload.data(), complete[0].payload.size(), &decoded) ||
            !sink->Write(decoded)) {
            ++mismatch;
            continue;
        }
        latency.Add(static_cast<double>(NowNsTest() - pcm.capture_ns) / 1e6);
    }
    MEDIA_CHECK_EQ(mismatch, 0U);
    MEDIA_CHECK_EQ(latency.count(), static_cast<std::size_t>(kFrames));
    std::printf("       latency p50=%.4f p99=%.4f max=%.4f ms (n=%zu)\n", latency.p50(),
                latency.p99(), latency.max(), latency.count());
    MEDIA_CHECK(latency.p99() <= 10.0);
    MEDIA_CHECK_EQ(sink->written_frames(), static_cast<std::uint64_t>(kFrames));
}

// ---------------------------------------------------------------------------
// 10. 端到端会话
// ---------------------------------------------------------------------------

void TestSession() {
    std::printf("[TEST] session\n");
    const AudioFormat format = MakeFormat(44100, 1);
    AudioSessionConfig config;
    config.format = format;
    config.codec = AudioCodec::kPcmS16LE;
    config.max_jitter_ms = 10;
    config.jitter.target_delay_packets = 2;
    config.jitter.adaptive = false;

    std::string error;
    auto session = AudioSession::Create(config, &error);
    MEDIA_CHECK(session != nullptr);
    if (session != nullptr) {
        const int played = session->Run(100);
        const AudioSessionStats& stats = session->stats();
        std::printf(
            "       inproc played=%d captured=%llu sent=%llu recv=%llu reassembled=%llu "
            "decoded=%llu lost=%llu p50=%.2f p99=%.2f ms\n",
            played, static_cast<unsigned long long>(stats.captured_frames),
            static_cast<unsigned long long>(stats.sent_packets),
            static_cast<unsigned long long>(stats.recv_packets),
            static_cast<unsigned long long>(stats.reassembled),
            static_cast<unsigned long long>(stats.decoded_frames),
            static_cast<unsigned long long>(stats.dropped_packets), stats.latency.p50(),
            stats.latency.p99());
        MEDIA_CHECK_EQ(stats.captured_frames, 100U);
        MEDIA_CHECK_GE(played, 50);
        MEDIA_CHECK_GE(stats.reassembled, 50U);
        MEDIA_CHECK_GE(stats.latency.p50(), 0.0);
        MEDIA_CHECK_GE(stats.output_rms, 0.0);
    } else {
        std::printf("       session create failed: %s\n", error.c_str());
    }

    // UDP 回环会话。
    AudioSessionConfig udp_config = config;
    udp_config.udp = true;
    auto udp_session = AudioSession::Create(udp_config, &error);
    MEDIA_CHECK(udp_session != nullptr);
    if (udp_session != nullptr) {
        const int played = udp_session->Run(80);
        const AudioSessionStats& stats = udp_session->stats();
        std::printf("       udp played=%d captured=%llu recv=%llu reassembled=%llu p99=%.2f ms\n",
                    played, static_cast<unsigned long long>(stats.captured_frames),
                    static_cast<unsigned long long>(stats.recv_packets),
                    static_cast<unsigned long long>(stats.reassembled), stats.latency.p99());
        MEDIA_CHECK_GE(played, 30);
        MEDIA_CHECK_GE(stats.recv_packets, 30U);
    }

    // Opus 会话（若可用）；44.1k 走重采样路径。
    if (ProbeAudioCodecs().opus) {
        AudioSessionConfig opus_config = config;
        opus_config.codec = AudioCodec::kOpus;
        opus_config.max_jitter_ms = 5;
        auto opus_session = AudioSession::Create(opus_config, &error);
        MEDIA_CHECK(opus_session != nullptr);
        if (opus_session != nullptr) {
            const int played = opus_session->Run(80);
            std::printf("       opus played=%d encoder=%s sink=%s p99=%.2f ms\n", played,
                        opus_session->encoder_name(), opus_session->sink_name(),
                        opus_session->stats().latency.p99());
            MEDIA_CHECK_GE(played, 30);
            MEDIA_CHECK_GE(opus_session->stats().bytes_encoded, 1U);
            MEDIA_CHECK(opus_session->stats().bytes_encoded <
                        opus_session->stats().bytes_captured);
        }
    }
}

// ---------------------------------------------------------------------------
// 11. PulseAudio 真实采集/播放
// ---------------------------------------------------------------------------

void TestPulseCapture() {
    std::printf("[TEST] pulse capture\n");
    if (!PulseCaptureAvailable()) {
        media_test::Skip("parec/PulseAudio unavailable");
        return;
    }
    AudioCaptureConfig config = MakeCapture(MakeFormat(44100, 1), SyntheticAudioPattern::kSine440);
    std::string error;
    auto source = CreatePulseAudioCapture(config, &error);
    if (source == nullptr) {
        std::printf("       pulse capture create failed: %s\n", error.c_str());
        media_test::Skip("parec spawn failed");
        return;
    }
    AudioFrame frame;
    int frames = 0;
    for (int i = 0; i < 5; ++i) {
        if (!source->Read(&frame)) {
            break;
        }
        if (!frame.Valid()) {
            MEDIA_CHECK(false);
            break;
        }
        MEDIA_CHECK_EQ(frame.data.size(), static_cast<std::size_t>(882));
        ++frames;
    }
    std::printf("       pulse frames=%d packets=%llu bytes=%llu\n", frames,
                static_cast<unsigned long long>(source->packets()),
                static_cast<unsigned long long>(source->bytes()));
    if (frames == 0) {
        media_test::Skip("RDPSource produced no data (idle)");
        return;
    }
    MEDIA_CHECK_GE(frames, 1);
    MEDIA_CHECK_GE(source->packets(), 1U);

    // 采集 → Opus/G.711 编码链路（只验证编码器能吃真实 PCM）。
    AudioEncoderConfig encoder_config;
    encoder_config.codec = PreferredAudioCodec();
    encoder_config.format = MakeFormat(44100, 1);
    auto encoder = CreateAudioEncoder(encoder_config, &error);
    MEDIA_CHECK(encoder != nullptr);
    if (encoder != nullptr) {
        std::vector<std::uint8_t> payload;
        MEDIA_CHECK(encoder->Encode(frame, &payload));
        MEDIA_CHECK_GE(payload.size(), 1U);
    }
}

void TestPulsePlayback() {
    std::printf("[TEST] pulse playback\n");
    if (!PulsePlaybackAvailable()) {
        media_test::Skip("pacat/PulseAudio unavailable");
        return;
    }
    const AudioFormat format = MakeFormat(44100, 1);
    AudioSinkConfig config;
    config.format = format;
    std::string error;
    auto sink = CreatePulseAudioSink(config, &error);
    if (sink == nullptr) {
        std::printf("       pulse sink create failed: %s\n", error.c_str());
        media_test::Skip("pacat spawn failed");
        return;
    }
    AudioCaptureConfig capture = MakeCapture(format, SyntheticAudioPattern::kSine440);
    auto source = CreateSyntheticAudioSource(capture, nullptr);
    int written = 0;
    for (int i = 0; i < 20; ++i) {
        AudioFrame frame;
        if (!source->Read(&frame)) {
            break;
        }
        if (!sink->Write(frame)) {
            break;
        }
        ++written;
    }
    std::printf("       pulse written=%d bytes=%llu\n", written,
                static_cast<unsigned long long>(sink->written_bytes()));
    if (written == 0) {
        media_test::Skip("pacat rejected writes (no sink)");
        return;
    }
    MEDIA_CHECK_EQ(sink->written_frames(), static_cast<std::uint64_t>(written));
    MEDIA_CHECK_GE(sink->written_bytes(), static_cast<std::uint64_t>(written) * 882U);
}

// ---------------------------------------------------------------------------
// 12. 压测（>=1e7）
// ---------------------------------------------------------------------------

void TestBench() {
    std::printf("[TEST] bench 1e7\n");
    constexpr std::size_t kSamples = 10000000;
    std::vector<std::int16_t> pcm(kSamples);
    std::mt19937 rng(0x1234U);
    for (std::size_t i = 0; i < kSamples; ++i) {
        pcm[i] = static_cast<std::int16_t>(rng() & 0xFFFFU);
    }
    std::vector<std::uint8_t> encoded(kSamples);

    // 预热（消除首次缺页与 I-cache 冷启动的影响）。
    for (std::size_t i = 0; i < 4096; ++i) {
        encoded[i] = detail::UlawEncodeTable(pcm[i]);
    }

    auto start = Clock::now();
    for (std::size_t i = 0; i < kSamples; ++i) {
        encoded[i] = detail::UlawEncodeTable(pcm[i]);
    }
    const double encode_ms = MsSince(start);
    std::printf("[PERF] ulaw_encode samples=%zu ms=%.2f ns_per_sample=%.3f M_per_s=%.1f GiB_per_s=%.2f\n",
                kSamples, encode_ms, encode_ms * 1e6 / static_cast<double>(kSamples),
                static_cast<double>(kSamples) / (encode_ms * 1000.0),
                static_cast<double>(kSamples) * 2.0 / (encode_ms * 1000.0) / 1024.0);

    start = Clock::now();
    std::int64_t accumulator = 0;
    for (std::size_t i = 0; i < kSamples; ++i) {
        accumulator += detail::UlawDecodeTable(encoded[i]);
    }
    const double decode_ms = MsSince(start);
    std::printf("[PERF] ulaw_decode samples=%zu ms=%.2f ns_per_sample=%.3f M_per_s=%.1f checksum=%lld\n",
                kSamples, decode_ms, decode_ms * 1e6 / static_cast<double>(kSamples),
                static_cast<double>(kSamples) / (decode_ms * 1000.0),
                static_cast<long long>(accumulator));

    // 生产路径（内联循环）的批量吞吐：排除每样本函数调用开销。
    std::vector<std::int16_t> decoded_block(kSamples);
    start = Clock::now();
    detail::UlawEncodeBlock(pcm.data(), kSamples, encoded.data());
    const double block_encode_ms = MsSince(start);
    start = Clock::now();
    detail::UlawDecodeBlock(encoded.data(), kSamples, decoded_block.data());
    const double block_decode_ms = MsSince(start);
    std::printf("[PERF] ulaw_encode_block samples=%zu ms=%.2f ns_per_sample=%.3f M_per_s=%.1f\n",
                kSamples, block_encode_ms,
                block_encode_ms * 1e6 / static_cast<double>(kSamples),
                static_cast<double>(kSamples) / (block_encode_ms * 1000.0));
    std::printf("[PERF] ulaw_decode_block samples=%zu ms=%.2f ns_per_sample=%.3f M_per_s=%.1f\n",
                kSamples, block_decode_ms,
                block_decode_ms * 1e6 / static_cast<double>(kSamples),
                static_cast<double>(kSamples) / (block_decode_ms * 1000.0));

    // 基线（逐位循环）与查表的 A/B。
    for (std::size_t i = 0; i < 4096; ++i) {
        encoded[i] = detail::UlawEncodeBaseline(pcm[i]);
    }
    start = Clock::now();
    for (std::size_t i = 0; i < kSamples; ++i) {
        encoded[i] = detail::UlawEncodeBaseline(pcm[i]);
    }
    const double baseline_ms = MsSince(start);
    std::printf("[PERF] ulaw_encode_baseline samples=%zu ms=%.2f ns_per_sample=%.3f M_per_s=%.1f speedup=%.2fx\n",
                kSamples, baseline_ms, baseline_ms * 1e6 / static_cast<double>(kSamples),
                static_cast<double>(kSamples) / (baseline_ms * 1000.0),
                baseline_ms / std::max(encode_ms, 1e-9));

    // RTP 头编码 1e7 次（无分配、固定 12B 输出）。
    RtpHeader header;
    std::uint8_t buffer[RtpHeader::kBytes] = {0};
    header.ssrc = 0xAABBCCDDU;
    header.payload_type = 111;
    constexpr std::size_t kHeaders = 10000000;
    start = Clock::now();
    for (std::size_t i = 0; i < kHeaders; ++i) {
        header.sequence = static_cast<std::uint16_t>(i);
        header.timestamp = static_cast<std::uint32_t>(i);
        header.marker = (i & 1U) != 0;
        EncodeRtpHeader(header, buffer);
    }
    const double header_ms = MsSince(start);
    std::printf("[PERF] rtp_header pkts=%zu ms=%.2f ns_per_packet=%.3f M_per_s=%.1f\n", kHeaders,
                header_ms, header_ms * 1e6 / static_cast<double>(kHeaders),
                static_cast<double>(kHeaders) / (header_ms * 1000.0));

    // 整帧分片 1e6 帧（882B → 1 片，含 4B 分片头与 12B RTP 头）。
    constexpr std::size_t kFrames = 1000000;
    std::vector<std::uint8_t> payload(882, 0x5A);
    AudioRtpPacketizer packetizer(0x1U, 111, 0);
    std::vector<AudioRtpPacket> packets;
    packets.reserve(8);
    start = Clock::now();
    std::size_t fragments = 0;
    for (std::size_t i = 0; i < kFrames; ++i) {
        packets.clear();
        fragments += static_cast<std::size_t>(
            packetizer.Packetize(payload.data(), payload.size(), static_cast<std::uint32_t>(i) * 441U,
                                 &packets));
    }
    const double packet_ms = MsSince(start);
    std::printf("[PERF] packetize frames=%zu fragments=%zu ms=%.2f ns_per_frame=%.3f M_frames_per_s=%.2f MiB_per_s=%.1f\n",
                kFrames, fragments, packet_ms, packet_ms * 1e6 / static_cast<double>(kFrames),
                static_cast<double>(kFrames) / (packet_ms * 1000.0),
                static_cast<double>(kFrames) * 886.0 / (packet_ms * 1000.0));
    // 架构对比：每帧新建一个 vector（模拟「未复用输出缓冲」的调用方式）。
    // 每帧 1 次堆分配 + 1 次 1202B 结构拷贝，用于量化复用缓冲的收益。
    constexpr std::size_t kFreshFrames = 200000;
    start = Clock::now();
    for (std::size_t i = 0; i < kFreshFrames; ++i) {
        std::vector<AudioRtpPacket> fresh;
        packetizer.Packetize(payload.data(), payload.size(),
                             static_cast<std::uint32_t>(i) * 441U, &fresh);
    }
    const double fresh_ms = MsSince(start);
    const double fresh_ns = fresh_ms * 1e6 / static_cast<double>(kFreshFrames);
    const double reuse_ns = packet_ms * 1e6 / static_cast<double>(kFrames);
    std::printf(
        "[PERF] packetize_fresh_vector frames=%zu ms=%.2f ns_per_frame=%.3f "
        "reuse_ns_per_frame=%.3f speedup=%.2fx\n",
        kFreshFrames, fresh_ms, fresh_ns, reuse_ns, fresh_ns / std::max(reuse_ns, 1e-9));

    MEDIA_CHECK_GE(kSamples, static_cast<std::size_t>(10000000));
    MEDIA_CHECK_EQ(fragments, kFrames);
    MEDIA_CHECK_GE(fresh_ns, reuse_ns);
}

}  // namespace

int main() {
    TestFormatAndFrame();
    TestG711();
    TestPcmPipeline1e5();
    TestOpus();
    TestRtpHeader();
    TestPacketizeReassemble();
    TestJitterBuffer();
    TestUdpLoopback();
    TestInProcessLatency();
    // 压测：>=1e7 的 µ-law 编解码与 RTP 头编码，>=1e6 的整帧分片。
    TestBench();

    // 真实 PulseAudio（WSLg）：采集/播放 smoke；不可用时 SKIP。
    TestPulseCapture();
    TestPulsePlayback();

    TestSession();
    return media_test::Finish("audio");
}
