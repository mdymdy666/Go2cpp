/**
 * @file audio_e2e_demo.cpp
 * @brief 音频端到端演示：采集 → 编码 → RTP 分片 → 传输 → 抖动缓冲 → 重组 → 解码 → 播放。
 * @details 五个场景覆盖从纯内存到真实 PulseAudio 设备的全谱：
 *
 *          1. `inproc-pcm`   合成源 → PCM 直通 → 进程内链路 → 空播放（基线，零丢失）
 *          2. `inproc-opus`  合成源 → Opus（dlopen libopus）→ 进程内链路 → 空播放
 *          3. `udp-opus`     Opus → 真实 UDP 回环（127.0.0.1）
 *          4. `weaknet-opus` ±20ms 抖动 + 每 50 包丢 1 个 → PLC 掩盖
 *          5. `pulse-opus`   WSLg PulseAudio（RDPSource/RDPSink）真实采集与播放
 *
 *          每个场景输出一行摘要，并把全部统计写入 `artifacts/session.json`；
 *          采集到的 PCM 写成 `artifacts/captured.wav`（44.1kHz/16bit/mono），
 *          可直接用 `aplay`/`ffplay` 试听合成源。
 *
 *          用法：
 *          ```
 *          ./audio_e2e_demo --frames 120 --artifacts ./artifacts
 *          ./audio_e2e_demo --codec opus --udp --jitter-ms 20 --drop-every 50
 *          ./audio_e2e_demo --real        # 需要 PulseAudio（WSLg 默认满足）
 *          ```
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "go2cpp/media/audio.hpp"

namespace {

using namespace go2cpp::media;

/// @brief 演示配置。
struct DemoOptions {
    int frames = 120;                 ///< 每个场景的帧数（10ms/帧）。
    int jitter_ms = 20;               ///< weaknet 场景的抖动幅度。
    std::uint32_t drop_every = 50;    ///< weaknet 场景的丢包节奏（0 = 不丢）。
    bool run_udp = true;              ///< 是否跑 UDP 场景。
    bool run_real = false;            ///< 是否跑 PulseAudio 真实设备场景。
    std::string artifacts = "artifacts";  ///< 产物目录。
};

/// @brief 一个场景的结果。
struct ScenarioResult {
    std::string name;
    std::string codec;
    std::string encoder;
    std::string capture;
    std::string sink;
    bool ok = false;
    int played = 0;
    AudioSessionStats stats;
};

AudioFormat DemoFormat() {
    AudioFormat format;
    format.sample_rate = 44100;
    format.channels = 1;
    format.frames_per_packet = 0;  // 10ms → 441 帧
    return format;
}

/// @brief 写一个最小的 16-bit PCM WAV 文件（44 字节头 + 数据）。
bool WriteWav(const std::string& path, const std::vector<std::uint8_t>& pcm, int sample_rate,
              int channels) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(pcm.size());
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * 2);
    const std::uint32_t byte_rate = static_cast<std::uint32_t>(sample_rate) * block_align;
    const std::uint32_t riff_bytes = 36U + data_bytes;
    const auto put32 = [&file](std::uint32_t value) {
        const std::uint8_t bytes[4] = {
            static_cast<std::uint8_t>(value & 0xFFU), static_cast<std::uint8_t>((value >> 8) & 0xFFU),
            static_cast<std::uint8_t>((value >> 16) & 0xFFU),
            static_cast<std::uint8_t>((value >> 24) & 0xFFU)};
        file.write(reinterpret_cast<const char*>(bytes), 4);
    };
    const auto put16 = [&file](std::uint16_t value) {
        const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xFFU),
                                       static_cast<std::uint8_t>((value >> 8) & 0xFFU)};
        file.write(reinterpret_cast<const char*>(bytes), 2);
    };
    file.write("RIFF", 4);
    put32(riff_bytes);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    put32(16);
    put16(1);  // PCM
    put16(static_cast<std::uint16_t>(channels));
    put32(static_cast<std::uint32_t>(sample_rate));
    put32(byte_rate);
    put16(block_align);
    put16(16);
    file.write("data", 4);
    put32(data_bytes);
    file.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
    return file.good();
}

/// @brief 跑一个场景。
ScenarioResult RunScenario(const std::string& name, AudioCodec codec, bool udp, int jitter_ms,
                           std::uint32_t drop_every, bool real, int frames,
                           std::vector<std::uint8_t>* captured_pcm) {
    ScenarioResult result;
    result.name = name;
    result.codec = AudioCodecName(codec);

    AudioSessionConfig config;
    config.format = DemoFormat();
    config.codec = codec;
    config.bitrate = 32000;
    config.udp = udp;
    config.max_jitter_ms = jitter_ms;
    config.drop_every = drop_every;
    config.real_capture = real;
    config.real_playback = real;
    config.capture.format = config.format;
    config.capture.pattern = SyntheticAudioPattern::kSine440;
    config.sink.format = config.format;
    config.jitter.target_delay_packets = 3;
    config.jitter.adaptive = true;

    std::string error;
    auto session = AudioSession::Create(config, &error);
    if (session == nullptr) {
        std::printf("  [SKIP] %-14s %s\n", name.c_str(), error.c_str());
        return result;
    }
    result.encoder = session->encoder_name();
    result.capture = session->capture_name();
    result.sink = session->sink_name();

    if (captured_pcm != nullptr && !real) {
        // 抓前 10 帧（100ms）写 WAV，再跑剩余帧。
        const int capture_frames = std::min(frames, 10);
        for (int i = 0; i < capture_frames; ++i) {
            AudioFrame frame;
            if (!session->SendNext(&frame)) {
                break;
            }
            captured_pcm->insert(captured_pcm->end(), frame.data.begin(), frame.data.end());
            result.played += session->Pump();
        }
        result.played += session->Run(std::max(0, frames - capture_frames));
    } else {
        result.played = session->Run(frames);
    }
    result.stats = session->stats();
    result.ok = true;

    std::printf(
        "  %-14s codec=%-5s enc=%-16s cap=%-16s sink=%-12s played=%3d "
        "p50=%6.2f p99=%6.2f ms jitter=%.1f drop=%llu\n",
        name.c_str(), result.codec.c_str(), result.encoder.c_str(), result.capture.c_str(),
        result.sink.c_str(), result.played, result.stats.latency.p50(),
        result.stats.latency.p99(), session->jitter_ms(),
        static_cast<unsigned long long>(result.stats.dropped_packets));
    return result;
}

void WriteJson(const std::string& path, const std::vector<ScenarioResult>& results,
               const DemoOptions& options) {
    std::ofstream file(path);
    if (!file) {
        return;
    }
    file << "{\n";
    file << "  \"frames_per_scenario\": " << options.frames << ",\n";
    file << "  \"format\": {\"sample_rate\": 44100, \"channels\": 1, \"frame_ms\": 10},\n";
    file << "  \"scenarios\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const ScenarioResult& item = results[i];
        file << "    {\"name\": \"" << item.name << "\", \"codec\": \"" << item.codec
             << "\", \"encoder\": \"" << item.encoder << "\", \"capture\": \"" << item.capture
             << "\", \"sink\": \"" << item.sink << "\", \"ok\": " << (item.ok ? "true" : "false")
             << ", \"played\": " << item.played
             << ", \"captured\": " << item.stats.captured_frames
             << ", \"encoded\": " << item.stats.encoded_frames
             << ", \"sent_packets\": " << item.stats.sent_packets
             << ", \"recv_packets\": " << item.stats.recv_packets
             << ", \"reassembled\": " << item.stats.reassembled
             << ", \"decoded\": " << item.stats.decoded_frames
             << ", \"concealed\": " << item.stats.concealed
             << ", \"dropped\": " << item.stats.dropped_packets
             << ", \"bytes_captured\": " << item.stats.bytes_captured
             << ", \"bytes_encoded\": " << item.stats.bytes_encoded;
        file.setf(std::ios::fixed);
        file.precision(3);
        file << ", \"latency_p50_ms\": " << item.stats.latency.p50()
             << ", \"latency_p99_ms\": " << item.stats.latency.p99()
             << ", \"latency_max_ms\": " << item.stats.latency.max()
             << ", \"output_rms\": " << item.stats.output_rms << "}";
        file << (i + 1 == results.size() ? "\n" : ",\n");
    }
    file << "  ]\n}\n";
}

/// @brief 解析命令行。
bool ParseArgs(int argc, char** argv, DemoOptions* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 需要参数\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--frames") {
            const char* value = next("--frames");
            if (value == nullptr) {
                return false;
            }
            options->frames = std::max(1, std::atoi(value));
        } else if (arg == "--jitter-ms") {
            const char* value = next("--jitter-ms");
            if (value == nullptr) {
                return false;
            }
            options->jitter_ms = std::max(0, std::atoi(value));
        } else if (arg == "--drop-every") {
            const char* value = next("--drop-every");
            if (value == nullptr) {
                return false;
            }
            options->drop_every = static_cast<std::uint32_t>(std::max(0, std::atoi(value)));
        } else if (arg == "--artifacts") {
            const char* value = next("--artifacts");
            if (value == nullptr) {
                return false;
            }
            options->artifacts = value;
        } else if (arg == "--real") {
            options->run_real = true;
        } else if (arg == "--no-udp") {
            options->run_udp = false;
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "用法: audio_e2e_demo [--frames N] [--jitter-ms N] [--drop-every N] "
                "[--artifacts DIR] [--real] [--no-udp]\n");
            return false;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    DemoOptions options;
    if (!ParseArgs(argc, argv, &options)) {
        return 1;
    }
    std::error_code error_code;
    std::filesystem::create_directories(options.artifacts, error_code);

    const AudioCodecAvailability availability = ProbeAudioCodecs();
    std::printf("PulseAudio: capture=%d playback=%d | opus=%d (%s) | ALSA: arecord=%d aplay=%d\n",
                PulseCaptureAvailable() ? 1 : 0, PulsePlaybackAvailable() ? 1 : 0,
                availability.opus ? 1 : 0, availability.opus_version.c_str(),
                AlsaCaptureAvailable() ? 1 : 0, AlsaPlaybackAvailable() ? 1 : 0);
    std::printf("frames/scenario=%d jitter=%dms drop_every=%u artifacts=%s\n", options.frames,
                options.jitter_ms, options.drop_every, options.artifacts.c_str());

    std::vector<ScenarioResult> results;
    std::vector<std::uint8_t> captured_pcm;

    results.push_back(RunScenario("inproc-pcm", AudioCodec::kPcmS16LE, false, 0, 0, false,
                                  options.frames, &captured_pcm));
    results.push_back(RunScenario("inproc-g711", AudioCodec::kPcmu, false, 0, 0, false,
                                  options.frames, nullptr));
    if (availability.opus) {
        results.push_back(RunScenario("inproc-opus", AudioCodec::kOpus, false, 0, 0, false,
                                      options.frames, nullptr));
        results.push_back(RunScenario("weaknet-opus", AudioCodec::kOpus, false, options.jitter_ms,
                                      options.drop_every, false, options.frames, nullptr));
        if (options.run_udp) {
            results.push_back(RunScenario("udp-opus", AudioCodec::kOpus, true, 0, 0, false,
                                          options.frames, nullptr));
        }
    } else {
        std::printf("  [SKIP] opus 场景：libopus.so.0 不可用\n");
    }
    if (options.run_real) {
        if (PulseCaptureAvailable() && PulsePlaybackAvailable()) {
            results.push_back(RunScenario("pulse-opus", availability.opus ? AudioCodec::kOpus
                                                                          : AudioCodec::kPcmu,
                                          false, 0, 0, true, options.frames, nullptr));
        } else {
            std::printf("  [SKIP] pulse 场景：PulseAudio 不可用\n");
        }
    }

    const std::string wave = options.artifacts + "/captured.wav";
    if (!captured_pcm.empty() && WriteWav(wave, captured_pcm, 44100, 1)) {
        std::printf("wrote %s (%zu bytes PCM)\n", wave.c_str(), captured_pcm.size());
    }
    const std::string json = options.artifacts + "/session.json";
    WriteJson(json, results, options);
    std::printf("wrote %s (%zu scenarios)\n", json.c_str(), results.size());

    int failures = 0;
    for (const ScenarioResult& item : results) {
        if (!item.ok) {
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
