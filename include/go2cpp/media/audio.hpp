#pragma once

/**
 * @file audio.hpp
 * @brief 音频链路：采集（PulseAudio/ALSA/合成）→ 编码（Opus/G.711/PCM）→
 *        RTP 风格分片 → UDP → 抖动缓冲 → 重组 → 解码 → 播放（PulseAudio/ALSA/空）。
 * @details 本文件补上 WebRTC 媒体管线里唯一缺失的「音频」一段，与既有的
 *          `capture.hpp` / `codec.hpp` / `transport.hpp`（视频）并列，不修改任何
 *          现有实现。视频侧用 Bento `VideoChannel` 的 1200B 分片，音频侧**不复用**
 *          该分片（音频包更密、需要按 RTP seq/timestamp 做抖动缓冲），而是实现
 *          一套最小的 RTP 风格线格式：
 *
 *          ```
 *          AudioSource ──s16le PCM──► AudioEncoder ──payload──►
 *          AudioRtpPacketizer（12B RTP 头 + 4B 分片头 + 数据，MTU 1200）──►
 *          UdpAudioSender ──UDP 回环──► UdpAudioReceiver ──►
 *          AudioJitterBuffer（按 seq 重排/去重/丢包计数）──►
 *          AudioRtpReassembler（按 timestamp 拼分片）──► AudioDecoder ──► AudioSink
 *          ```
 *
 *          ## 线格式（全部大端）
 *
 *          RTP 固定头 12 字节（RFC 3550 的最小子集，V=2、P=0、X=0、CC=0）：
 *
 *          | 偏移 | 字段   | 类型 | 语义                          |
 *          |------|--------|------|-------------------------------|
 *          | 0    | flags  | u8   | 0x80（V=2）                    |
 *          | 1    | pt/m   | u8   | bit7 = marker，bit0..6 = PT    |
 *          | 2    | seq    | u16  | 包序号（回绕）                  |
 *          | 4    | ts     | u32  | 时间戳（源采样率时钟，见下）      |
 *          | 8    | ssrc   | u32  | 同步源                         |
 *          | 12   | frag   | u16  | 分片序号（本项目扩展，0 起）      |
 *          | 14   | total  | u16  | 分片总数（1 表示不分片）           |
 *          | 16   | data   | ...  | 编码负载                        |
 *
 *          `frag/total` 是**本项目扩展**，且**总是存在**（每个 RTP 负载前 4 字节）：
 *          音频帧可能超过 1188B（例如 20ms@48kHz 双声道 s16le = 3840B），需要分片
 *          并让接收端无歧义地按 timestamp 重组。缺这 4 字节、或字段非法的包一律
 *          记 `malformed` 丢弃 —— 不做「裸包启发式识别」，避免把标准 Opus 负载的
 *          前 4 字节误判成合法分片头。与标准 RTP 互通需要在网关侧剥离/补齐该扩展，
 *          见 `docs/modules/media-audio.md` §6。
 *
 *          ## 时间戳口径
 *
 *          `timestamp` 用**源采样率时钟**（44.1kHz → 每 10ms 帧 +441），而不是
 *          Opus 规定的 48kHz 时钟。原因：接收端只用 timestamp 做「同一帧的分片
 *          分组」与乱序排序，不用于解码时钟（解码器从 `AudioFormat` 取采样率）；
 *          用源时钟可以让「采集帧号 ↔ timestamp」保持整数关系，便于逐帧校验。
 *          与标准 RTP 互操作时需要按 48k 重标（文档 §6）。
 *
 *          ## 后端事实（WSL2 / Ubuntu 24.04，实测）
 *
 *          - PulseAudio 走 `/mnt/wslg/PulseServer`：默认源 `RDPSource`
 *            （s16le 1ch 44.1kHz）、默认宿 `RDPSink`（s16le 2ch 44.1kHz）；
 *            `parec` / `pacat` / `paplay` 均可用 → 采集与播放走**子进程 + 管道**，
 *            零链接依赖（与 `ffmpeg_codec.cpp` 同样的取舍）。
 *          - `libopus.so.0` **存在**但 `pkg-config --exists opus` 为假（无 dev 头），
 *            因此 Opus 后端用 `dlopen("libopus.so.0")` + 手写符号原型实现，
 *            编译期不依赖 `libopus-dev`；dlopen 失败时工厂返回 nullptr，
 *            上层自动降级到 G.711（`PreferredAudioCodec()`）。
 *          - ALSA：`libasound.so.2` 存在但 `arecord`/`aplay` **未安装**
 *            （alsa-utils 缺失），ALSA 后端代码保留、探测为不可用，测试 SKIP。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/media/stats.hpp"

namespace go2cpp::media {

// ===========================================================================
// 格式与帧
// ===========================================================================

/// @brief 采样格式（当前只实现小端 16 位有符号整型）。
enum class AudioSampleFormat : std::uint8_t {
    kS16LE = 0,  ///< interleaved signed 16-bit little-endian。
};

/// @brief 音频编解码器。
enum class AudioCodec : std::uint8_t {
    kOpus = 0,     ///< libopus（dlopen；48kHz 内部时钟，支持 44.1k 重采样）。
    kPcmu = 1,     ///< G.711 µ-law（8 bit/样本）。
    kPcma = 2,     ///< G.711 A-law（8 bit/样本）。
    kPcmS16LE = 3, ///< 不压缩（直通，逐字节可校验）。
};

/// @brief 编解码器可读名字。
const char* AudioCodecName(AudioCodec codec) noexcept;

/// @brief 从字符串解析（"opus"/"pcmu"/"pcma"/"pcm"）。
bool ParseAudioCodec(const char* text, AudioCodec* out) noexcept;

/// @brief RTP 动态负载类型约定（Opus 111、PCMU 0、PCMA 8、L16 96）。
std::uint8_t DefaultAudioPayloadType(AudioCodec codec) noexcept;

/// @brief 音频格式（采样率/声道/采样格式/每包帧数）。
struct AudioFormat {
    int sample_rate = 44100;  ///< 采样率（Hz）。
    int channels = 1;         ///< 声道数（1/2）。
    AudioSampleFormat sample_format = AudioSampleFormat::kS16LE;  ///< 采样格式。
    /// 每包帧数（一帧 = 所有声道的一个采样点）；0 表示按 10ms 推导。
    int frames_per_packet = 0;

    /// @brief 默认打包时长（毫秒）。
    static constexpr int kDefaultPacketMs = 10;

    /// @brief 实际生效的每包帧数（`frames_per_packet <= 0` 时取 10ms）。
    int EffectiveFramesPerPacket() const noexcept;

    /// @brief 单样本字节数（只支持 s16le，恒为 2）。
    int BytesPerSample() const noexcept {
        return 2;
    }

    /// @brief 单帧（所有声道）字节数。
    int BytesPerFrame() const noexcept {
        return channels * BytesPerSample();
    }

    /// @brief 一个包的 PCM 字节数。
    int BytesPerPacket() const noexcept {
        return EffectiveFramesPerPacket() * BytesPerFrame();
    }

    /// @brief 一个包的时长（微秒）。
    int PacketDurationUs() const noexcept;

    /// @brief 格式是否合法。
    bool Valid() const noexcept;

    /// @brief 相等比较（RTP 协商/测试断言用）。
    bool operator==(const AudioFormat& other) const noexcept;
    bool operator!=(const AudioFormat& other) const noexcept {
        return !(*this == other);
    }
};

/// @brief 单帧音频负载的上限（分片前）；超过该值的帧被拒绝。
/// @details 8KiB 足以容纳 20ms@48kHz 双声道 s16le（3840B）与 Opus 最大包
///          （1275B）；与 RTP 分片配合可拆成 <=7 片。
inline constexpr std::size_t kMaxAudioPayloadBytes = 8192;

/// @brief 一帧 PCM 音频 + 传输元数据。
struct AudioFrame {
    AudioFormat format;             ///< 格式。
    std::uint32_t sequence = 0;     ///< 包序号（源端维护，回绕）。
    std::uint32_t timestamp = 0;    ///< RTP 时间戳（源采样率时钟）。
    std::uint32_t ssrc = 0;         ///< 同步源。
    std::int64_t capture_ns = 0;    ///< 采集时刻（steady_clock 纳秒）。
    std::vector<std::uint8_t> data; ///< interleaved s16le 样本。

    /// @brief 样本数（= data.size() / 2）。
    std::size_t samples() const noexcept {
        return data.size() / 2;
    }

    /// @brief 帧数（= samples() / channels）。
    std::size_t frames() const noexcept {
        return format.channels > 0 ? samples() / static_cast<std::size_t>(format.channels) : 0;
    }

    /// @brief 帧是否可参与编解码：格式合法、长度与格式一致、不超上限。
    bool Valid() const noexcept;

    /// @brief 内容校验（FNV-1a 64）。
    std::uint64_t Checksum() const noexcept;

    /// @brief 对一段字节做同样的 FNV-1a 64。
    static std::uint64_t ChecksumBytes(const std::uint8_t* data, std::size_t bytes) noexcept;

    /// @brief PCM 的均方根（0..32767），用于「有没有声音」的快速判断。
    double Rms() const noexcept;
};

// ===========================================================================
// 采集
// ===========================================================================

/// @brief 合成音频图案（确定性，便于逐帧比对）。
enum class SyntheticAudioPattern : std::uint8_t {
    kSilence = 0,  ///< 全零。
    kSine440 = 1,  ///< 440Hz 正弦（-6dBFS）。
    kSweep = 2,    ///< 200Hz → 4kHz 线性扫频（每 2 秒一轮）。
    kNoise = 3,    ///< 白噪声（最坏情况，不可压缩）。
    kDtmf = 4,     ///< 双音多频（697+1209Hz，模拟按键音）。
};

/// @brief 图案可读名字。
const char* SyntheticAudioPatternName(SyntheticAudioPattern pattern) noexcept;

/// @brief 采集配置。
struct AudioCaptureConfig {
    AudioFormat format;                              ///< 期望格式。
    std::string device;                              ///< Pulse 源名；空 = 默认源。
    int latency_ms = 20;                             ///< Pulse 目标延迟。
    SyntheticAudioPattern pattern = SyntheticAudioPattern::kSine440;  ///< 合成图案。
    std::uint32_t seed = 0x5EED1234U;                ///< 合成噪声种子。
};

/// @brief 音频采集源抽象。
class AudioSource {
   public:
    virtual ~AudioSource() = default;

    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;
    AudioSource(AudioSource&&) = delete;
    AudioSource& operator=(AudioSource&&) = delete;

    /// @brief 阻塞读一个包（正好 `format.BytesPerPacket()` 字节）。
    /// @return 成功返回 true；子进程退出/超时/格式不符返回 false。
    virtual bool Read(AudioFrame* out) = 0;

    /// @brief 可读名字。
    virtual const char* name() const noexcept = 0;

    /// @brief 后端是否可用（子进程是否还活着 / 合成源恒 true）。
    virtual bool available() const noexcept = 0;

    /// @brief 已读出的包数。
    virtual std::uint64_t packets() const noexcept = 0;

    /// @brief 已读出的字节数。
    virtual std::uint64_t bytes() const noexcept = 0;

    /// @brief 采集格式。
    virtual AudioFormat format() const noexcept = 0;

   protected:
    AudioSource() = default;
};

/// @brief PulseAudio 是否可用（`pactl info` 探测，结果缓存）。
bool PulseAudioAvailable();

/// @brief `parec` 是否可执行。
bool PulseCaptureAvailable();

/// @brief `pacat` 是否可执行。
bool PulsePlaybackAvailable();

/// @brief `arecord` 是否可执行（WSL2 默认未安装 alsa-utils）。
bool AlsaCaptureAvailable();

/// @brief `aplay` 是否可执行。
bool AlsaPlaybackAvailable();

/// @brief 创建 PulseAudio 采集源（`parec` 子进程）。
/// @param error 失败原因（可为 nullptr）。
std::unique_ptr<AudioSource> CreatePulseAudioCapture(const AudioCaptureConfig& config,
                                                     std::string* error);

/// @brief 创建 ALSA 采集源（`arecord` 子进程；CLI 缺失时返回 nullptr）。
std::unique_ptr<AudioSource> CreateAlsaAudioCapture(const AudioCaptureConfig& config,
                                                    std::string* error);

/// @brief 创建合成采集源（无外部依赖，确定性）。
std::unique_ptr<AudioSource> CreateSyntheticAudioSource(const AudioCaptureConfig& config,
                                                        std::string* error = nullptr);

/// @brief 自动选择采集源：Pulse → ALSA → 合成（并写入 `error` 说明降级原因）。
std::unique_ptr<AudioSource> CreateAudioSource(const AudioCaptureConfig& config,
                                               std::string* error);

// ===========================================================================
// 编码 / 解码
// ===========================================================================

/// @brief 编码配置。
struct AudioEncoderConfig {
    AudioCodec codec = AudioCodec::kOpus;  ///< 编码器。
    AudioFormat format;                    ///< 输入 PCM 格式。
    int bitrate = 32000;                   ///< 目标码率（Opus 生效）。
    int complexity = 5;                    ///< Opus 复杂度 0..10。
    bool inband_fec = false;               ///< Opus 带内 FEC。
    bool dtx = false;                      ///< Opus 不连续传输（静音省流）。
};

/// @brief 音频编码器接口。
class AudioEncoder {
   public:
    virtual ~AudioEncoder() = default;

    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;
    AudioEncoder(AudioEncoder&&) = delete;
    AudioEncoder& operator=(AudioEncoder&&) = delete;

    /// @brief 编码一帧 PCM，输出编码负载（不含 RTP 头）。
    virtual bool Encode(const AudioFrame& frame, std::vector<std::uint8_t>* out) = 0;

    /// @brief 编码器种类。
    virtual AudioCodec codec() const noexcept = 0;

    /// @brief 可读名字（含后端信息，如 "opus(libopus.so.0)"）。
    virtual const char* name() const noexcept = 0;

    /// @brief 已编码帧数。
    virtual std::uint64_t encoded_frames() const noexcept = 0;

    /// @brief 已产出的负载字节数。
    virtual std::uint64_t encoded_bytes() const noexcept = 0;

   protected:
    AudioEncoder() = default;
};

/// @brief 音频解码器接口。
class AudioDecoder {
   public:
    virtual ~AudioDecoder() = default;

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    AudioDecoder(AudioDecoder&&) = delete;
    AudioDecoder& operator=(AudioDecoder&&) = delete;

    /// @brief 解码一个负载。
    /// @param payload 负载字节；`nullptr` 或 `bytes == 0` 表示**丢包隐藏（PLC）**，
    ///        输出一个与正常帧等长的替代 PCM。
    /// @param out 输出帧（format 使用创建时的格式）。
    virtual bool Decode(const std::uint8_t* payload, std::size_t bytes, AudioFrame* out) = 0;

    /// @brief 解码器种类。
    virtual AudioCodec codec() const noexcept = 0;

    /// @brief 成功解码帧数（PLC 不计入）。
    virtual std::uint64_t decoded_frames() const noexcept = 0;

    /// @brief 已解码样本数。
    virtual std::uint64_t decoded_samples() const noexcept = 0;

    /// @brief PLC 帧数。
    virtual std::uint64_t concealed_frames() const noexcept = 0;

    /// @brief 跳过的坏包数（长度非法/解码失败）。
    virtual std::uint64_t dropped_frames() const noexcept = 0;

   protected:
    AudioDecoder() = default;
};

/// @brief 创建编码器；后端不可用（Opus 未加载）返回 nullptr 并写 `error`。
std::unique_ptr<AudioEncoder> CreateAudioEncoder(const AudioEncoderConfig& config,
                                                 std::string* error);

/// @brief 创建解码器；后端不可用返回 nullptr 并写 `error`。
std::unique_ptr<AudioDecoder> CreateAudioDecoder(AudioCodec codec, const AudioFormat& format,
                                                 std::string* error);

/// @brief 后端可用性（进程内只探测一次）。
struct AudioCodecAvailability {
    bool opus = false;          ///< libopus.so.0 已加载且符号齐全。
    bool pcmu = true;           ///< 内置 G.711 µ-law 恒可用。
    bool pcma = true;           ///< 内置 G.711 A-law 恒可用。
    bool pcm_s16le = true;      ///< 直通恒可用。
    std::string opus_library;   ///< 实际加载的库路径（空 = 未加载）。
    std::string opus_version;   ///< `opus_get_version_string()`。
    std::string detail;         ///< 探测说明（失败原因等）。
};

/// @brief 探测音频编解码后端。
AudioCodecAvailability ProbeAudioCodecs();

/// @brief 首选编码器：Opus 可用则 Opus，否则 G.711 µ-law。
AudioCodec PreferredAudioCodec();

// ===========================================================================
// RTP 风格封装与分片
// ===========================================================================

/// @brief RTP 固定头（12B，V=2 / P=0 / X=0 / CC=0）。
struct RtpHeader {
    /// 固定字节数。
    static constexpr std::size_t kBytes = 12;
    /// 协议版本。
    static constexpr std::uint8_t kVersion = 2;
    /// 单包负载上限（1200 MTU − 12B RTP 头 − 4B 分片头）。
    static constexpr std::size_t kMaxPayloadBytes = 1188;

    std::uint8_t payload_type = 111;  ///< 负载类型（7 bit）。
    bool marker = false;              ///< M 位（本实现用于「帧最后一片」）。
    std::uint16_t sequence = 0;       ///< 序号。
    std::uint32_t timestamp = 0;      ///< 时间戳（源采样率时钟）。
    std::uint32_t ssrc = 0;           ///< 同步源。
};

/// @brief 编码 RTP 头到 12 字节（大端）。
void EncodeRtpHeader(const RtpHeader& header, std::uint8_t* out) noexcept;

/// @brief 解析 RTP 头；版本不符/长度不足/PT 越界返回 false。
bool DecodeRtpHeader(const std::uint8_t* data, std::size_t bytes, RtpHeader* out) noexcept;

/// @brief 分片头字节数（frag u16 + total u16）。
inline constexpr std::size_t kAudioFragmentHeaderBytes = 4;

/// @brief 一个 RTP 包（固定容量负载，避免每包一次堆分配）。
struct AudioRtpPacket {
    RtpHeader header;                                               ///< RTP 头。
    std::array<std::uint8_t, RtpHeader::kMaxPayloadBytes> payload;  ///< 负载（未初始化，按需写）。
    std::uint16_t payload_bytes = 0;                                ///< 负载字节数。

    /// @brief 负载指针。
    const std::uint8_t* data() const noexcept {
        return payload.data();
    }

    /// @brief 负载是否合法。
    bool Valid() const noexcept {
        return payload_bytes > 0 && payload_bytes <= payload.size();
    }
};

/// @brief 将编码负载切成 RTP 分片。
/// @details 分片头 `[u16 frag_index][u16 frag_total]` 写在每个 RTP 负载之前；
///          单包（`bytes <= max_payload - 4`）时 total=1、index=0。
class AudioRtpPacketizer {
   public:
    /// @param ssrc 同步源。
    /// @param payload_type 负载类型（0..127）。
    /// @param initial_sequence 起始序号。
    /// @param max_payload 单包负载上限（默认 1188；不得小于 5）。
    AudioRtpPacketizer(std::uint32_t ssrc, std::uint8_t payload_type,
                       std::uint16_t initial_sequence = 0,
                       std::size_t max_payload = RtpHeader::kMaxPayloadBytes);

    /// @brief 打包一段编码负载。
    /// @param payload 负载指针（可为空，仅当 bytes==0）。
    /// @param bytes 负载字节数（0 也允许，生成一个空分片）。
    /// @param timestamp 时间戳。
    /// @param out 输出分片（会被 append；调用方应 `clear()` 后复用）。
    /// @return 分片数；负载超上限（> `kMaxAudioPayloadBytes`）返回 0。
    int Packetize(const std::uint8_t* payload, std::size_t bytes, std::uint32_t timestamp,
                  std::vector<AudioRtpPacket>* out);

    /// @brief 打包一个「编码后」的帧（`frame.data` 视为负载，`frame.timestamp` 生效）。
    int PacketizeFrame(const AudioFrame& frame, std::vector<AudioRtpPacket>* out);

    /// @brief 下一次将使用的序号。
    std::uint16_t next_sequence() const noexcept {
        return m_sequence;
    }

    /// @brief 已产出的分片数。
    std::uint64_t packets() const noexcept {
        return m_packets;
    }

    /// @brief 单包负载上限。
    std::size_t max_payload() const noexcept {
        return m_max_payload;
    }

   private:
    std::uint32_t m_ssrc;
    std::uint8_t m_payload_type;
    std::uint16_t m_sequence;
    std::size_t m_max_payload;
    std::uint64_t m_packets = 0;
};

/// @brief 重组后的完整编码帧。
struct ReassembledAudio {
    std::uint32_t timestamp = 0;         ///< 帧时间戳。
    std::uint32_t ssrc = 0;              ///< 同步源。
    std::uint16_t first_sequence = 0;    ///< 首片序号。
    std::uint64_t first_arrival_ms = 0;  ///< 首片到达时刻（单调毫秒）。
    int fragments = 0;                   ///< 分片数。
    std::vector<std::uint8_t> payload;   ///< 拼接后的编码负载。
};

/// @brief 把同一 timestamp 的分片拼回完整负载。
class AudioRtpReassembler {
   public:
    /// @brief 重组配置。
    struct Config {
        std::size_t max_pending = 8;       ///< 同时挂起的 timestamp 组数。
        std::uint16_t max_fragments = 32;  ///< 单帧分片上限。
        std::uint64_t timeout_ms = 200;    ///< 组超时（缺片丢弃）。
    };

    /// @brief 使用默认配置构造。
    AudioRtpReassembler();

    /// @brief 使用指定配置构造。
    explicit AudioRtpReassembler(const Config& config);

    /// @brief 喂一个 RTP 包。
    /// @param complete 已完整的帧（按 timestamp 升序追加）。
    /// @param now_ms 单调毫秒时钟。
    void Push(const AudioRtpPacket& packet, std::uint64_t now_ms,
              std::vector<ReassembledAudio>* complete);

    /// @brief 清理超时组（应周期性调用）。
    void OnTimer(std::uint64_t now_ms);

    /// @brief 收到的分片数。
    std::uint64_t fragments() const noexcept {
        return m_fragments;
    }

    /// @brief 完成帧数。
    std::uint64_t completed() const noexcept {
        return m_completed;
    }

    /// @brief 缺片丢弃的组数。
    std::uint64_t incomplete() const noexcept {
        return m_incomplete;
    }

    /// @brief 坏分片（长度/字段非法）数。
    std::uint64_t malformed() const noexcept {
        return m_malformed;
    }

   private:
    struct Pending {
        std::uint32_t timestamp = 0;
        std::uint32_t ssrc = 0;
        std::uint16_t total = 0;
        std::uint16_t received = 0;
        std::uint16_t first_sequence = 0;
        std::uint64_t first_arrival_ms = 0;
        std::vector<std::vector<std::uint8_t>> fragments;
        std::vector<bool> present;
    };

    bool Finish(const Pending& pending, ReassembledAudio* out) const;
    void DropOldest();

    Config m_config;
    std::vector<Pending> m_pending;
    std::uint64_t m_fragments = 0;
    std::uint64_t m_completed = 0;
    std::uint64_t m_incomplete = 0;
    std::uint64_t m_malformed = 0;
};

// ===========================================================================
// 抖动缓冲
// ===========================================================================

/// @brief 抖动缓冲配置。
struct AudioJitterBufferConfig {
    int target_delay_packets = 3;     ///< 初始目标深度（包数）。
    int min_delay_packets = 1;        ///< 自适应下限。
    int max_delay_packets = 12;       ///< 自适应上限。
    bool adaptive = true;             ///< 按到达抖动自适应调整深度。
    std::size_t max_packets = 512;    ///< 队列上限（超出丢最旧）。
    int max_reorder_distance = 128;   ///< 允许的乱序距离（扩展序号差）。
    int packet_ms = 10;               ///< 单包时长（毫秒），用于抖动/深度换算。
    std::uint64_t max_wait_ms = 200;  ///< 单包最长排队时间（防卡死）。
};

/// @brief Pop 的结果。
enum class JitterPopResult : std::uint8_t {
    kEmpty = 0,  ///< 暂时没有可交付的包。
    kPacket = 1, ///< 交付一个包。
    kLost = 2,   ///< 判定丢包（调用方应做 PLC）。
};

/// @brief 音频抖动缓冲：按 RTP 序号重排、去重、丢包检测、自适应深度。
class AudioJitterBuffer {
   public:
    explicit AudioJitterBuffer(
        const AudioJitterBufferConfig& config = AudioJitterBufferConfig());

    /// @brief 入队一个包。
    /// @return 接收返回 true；迟到/重复/越界返回 false（计入对应计数）。
    bool Push(const AudioRtpPacket& packet, std::uint64_t now_ms);

    /// @brief 尝试取出一个可交付的包（或一个丢包标记）。
    JitterPopResult Pop(AudioRtpPacket* out, std::uint64_t now_ms);

    /// @brief 清理超期包（计入 lost）。
    void OnTimer(std::uint64_t now_ms);

    /// @brief 已接收（入队成功）包数。
    std::uint64_t received() const noexcept {
        return m_received;
    }

    /// @brief 重复包数。
    std::uint64_t duplicates() const noexcept {
        return m_duplicates;
    }

    /// @brief 迟到包数（早于播放游标或超出乱序距离）。
    std::uint64_t late() const noexcept {
        return m_late;
    }

    /// @brief 判定丢失的包数。
    std::uint64_t lost() const noexcept {
        return m_lost;
    }

    /// @brief 已交付包数。
    std::uint64_t delivered() const noexcept {
        return m_delivered;
    }

    /// @brief 当前排队包数。
    std::size_t depth() const noexcept {
        return m_queue.size();
    }

    /// @brief 当前目标深度（包数）。
    int target_delay_packets() const noexcept {
        return m_target_packets;
    }

    /// @brief 到达抖动估计（毫秒，EWMA）。
    double jitter_ms() const noexcept {
        return m_jitter_ms;
    }

    /// @brief 累计排队延迟（毫秒），用于 p50/p99 统计。
    const LatencyRecorder& queue_delay() const noexcept {
        return m_queue_delay;
    }

    /// @brief 已交付/丢弃的扩展序号（调试用）。
    std::uint64_t next_sequence() const noexcept {
        return m_next_sequence;
    }

   private:
    struct Entry {
        AudioRtpPacket packet;
        std::uint64_t arrival_ms = 0;
    };

    void UpdateTarget(std::uint64_t now_ms);

    AudioJitterBufferConfig m_config;
    std::vector<std::pair<std::uint64_t, Entry>> m_queue;  // 按扩展序号有序
    bool m_have_base = false;
    std::uint16_t m_last_sequence = 0;
    std::uint64_t m_last_extended = 0;
    std::uint64_t m_next_sequence = 0;
    int m_target_packets = 3;
    double m_jitter_ms = 0.0;
    std::uint64_t m_last_arrival_ms = 0;
    bool m_have_arrival = false;
    std::uint64_t m_received = 0;
    std::uint64_t m_duplicates = 0;
    std::uint64_t m_late = 0;
    std::uint64_t m_lost = 0;
    std::uint64_t m_delivered = 0;
    LatencyRecorder m_queue_delay;
};

// ===========================================================================
// UDP 收发
// ===========================================================================

/// @brief UDP 音频发送端（RTP 分片 + `sendto`）。
class UdpAudioSender {
   public:
    explicit UdpAudioSender(const AudioRtpPacketizer& packetizer);
    ~UdpAudioSender();

    UdpAudioSender(const UdpAudioSender&) = delete;
    UdpAudioSender& operator=(const UdpAudioSender&) = delete;

    /// @brief 打开本地 socket 并记录对端端口。
    bool Open(std::uint16_t peer_port, std::string* error);

    /// @brief 本端绑定端口。
    std::uint16_t local_port() const noexcept;

    /// @brief 发送一个编码负载（自动分片）。
    /// @return 全部分片发送成功返回 true。
    bool SendPayload(const std::uint8_t* payload, std::size_t bytes, std::uint32_t timestamp);

    /// @brief 发送一个编码帧的负载（`frame.data`）。
    bool SendFrame(const AudioFrame& encoded);

    /// @brief 已发送数据报数。
    std::uint64_t datagrams() const noexcept;

    /// @brief 发送失败的分片数。
    std::uint64_t failures() const noexcept;

    /// @brief 已发送字节数（含头）。
    std::uint64_t bytes() const noexcept;

    /// @brief 底层分片器。
    AudioRtpPacketizer* packetizer() noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @brief UDP 音频接收端。
class UdpAudioReceiver {
   public:
    /// @brief 收包回调：包 + 到达时刻（单调毫秒）。
    using PacketFn = void (*)(void* context, const AudioRtpPacket& packet,
                              std::uint64_t arrival_ms);

    UdpAudioReceiver(PacketFn callback, void* context);
    ~UdpAudioReceiver();

    UdpAudioReceiver(const UdpAudioReceiver&) = delete;
    UdpAudioReceiver& operator=(const UdpAudioReceiver&) = delete;

    /// @brief 绑定 127.0.0.1:0。
    bool Open(std::string* error);

    /// @brief 本地端口。
    std::uint16_t local_port() const noexcept;

    /// @brief 收包并驱动回调。
    /// @return 交付的数据报数。
    int Pump(int timeout_ms, int max_packets);

    /// @brief 收到的数据报数。
    std::uint64_t datagrams() const noexcept;

    /// @brief 解析失败的包数。
    std::uint64_t malformed() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ===========================================================================
// 进程内链路（抖动/丢包模拟，供测试与示例）
// ===========================================================================

/// @brief 进程内音频链路：同一套分片/重组，可注入抖动与丢包。
class InProcessAudioLink {
   public:
    /// @param ssrc 同步源。
    /// @param initial_sequence 起始序号。
    /// @param max_payload 单包负载上限。
    InProcessAudioLink(std::uint32_t ssrc, std::uint16_t initial_sequence = 0,
                       std::size_t max_payload = RtpHeader::kMaxPayloadBytes);
    ~InProcessAudioLink();

    InProcessAudioLink(const InProcessAudioLink&) = delete;
    InProcessAudioLink& operator=(const InProcessAudioLink&) = delete;

    /// @brief 发送一段编码负载（按 `now_ms` 计算到达时刻）。
    void Send(const std::uint8_t* payload, std::size_t bytes, std::uint32_t timestamp,
              std::uint64_t now_ms);

    /// @brief 设置抖动：每个包的额外延迟在 `[0, max_jitter_ms]` 内均匀分布。
    void SetJitter(int max_jitter_ms, std::uint32_t seed = 0x9E3779B9U) noexcept;

    /// @brief 每 `drop_every` 个分片丢 1 个（0/1 表示不丢）。
    void SetDropEvery(std::uint32_t drop_every) noexcept;

    /// @brief 设置交付回调。
    using DeliverFn = void (*)(void* context, const AudioRtpPacket& packet,
                               std::uint64_t arrival_ms);
    void SetDeliver(DeliverFn callback, void* context) noexcept;

    /// @brief 交付所有到达时刻 <= `now_ms` 的包。
    /// @return 交付数。
    int PumpDue(std::uint64_t now_ms);

    /// @brief 仍在队列里的包数。
    std::size_t pending() const noexcept;

    /// @brief 发送的分片数。
    std::uint64_t sent() const noexcept {
        return m_sent;
    }

    /// @brief 丢弃的分片数。
    std::uint64_t dropped() const noexcept {
        return m_dropped;
    }

    /// @brief 已交付分片数。
    std::uint64_t delivered() const noexcept {
        return m_delivered;
    }

    /// @brief 发送端分片器（用于读取序号等）。
    const AudioRtpPacketizer& packetizer() const noexcept {
        return m_packetizer;
    }

   private:
    struct Queued {
        std::uint64_t due_ms = 0;
        AudioRtpPacket packet;
    };
    std::uint32_t NextRandom() noexcept;

    AudioRtpPacketizer m_packetizer;
    std::vector<Queued> m_queue;
    DeliverFn m_deliver = nullptr;
    void* m_context = nullptr;
    int m_max_jitter_ms = 0;
    std::uint32_t m_rng = 0x9E3779B9U;
    std::uint32_t m_drop_every = 0;
    std::uint64_t m_sent = 0;
    std::uint64_t m_dropped = 0;
    std::uint64_t m_delivered = 0;
};

// ===========================================================================
// 播放
// ===========================================================================

/// @brief 播放（sink）配置。
struct AudioSinkConfig {
    AudioFormat format;   ///< 播放格式。
    std::string device;   ///< Pulse 宿名；空 = 默认宿。
    int latency_ms = 40;  ///< Pulse 目标延迟。
};

/// @brief 音频输出抽象。
class AudioSink {
   public:
    virtual ~AudioSink() = default;

    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;
    AudioSink(AudioSink&&) = delete;
    AudioSink& operator=(AudioSink&&) = delete;

    /// @brief 打开后端（子进程/空后端）。
    virtual bool Open(std::string* error) = 0;

    /// @brief 写一帧 PCM（同步；阻塞到管道接受或失败）。
    virtual bool Write(const AudioFrame& frame) = 0;

    /// @brief 冲刷（关闭子进程 stdin 并等待退出）。
    virtual void Close() = 0;

    /// @brief 可读名字。
    virtual const char* name() const noexcept = 0;

    /// @brief 后端是否可用。
    virtual bool available() const noexcept = 0;

    /// @brief 已写帧数。
    virtual std::uint64_t written_frames() const noexcept = 0;

    /// @brief 已写字节数。
    virtual std::uint64_t written_bytes() const noexcept = 0;

   protected:
    AudioSink() = default;
};

/// @brief 创建 PulseAudio 播放后端（`pacat` 子进程）。
std::unique_ptr<AudioSink> CreatePulseAudioSink(const AudioSinkConfig& config,
                                                std::string* error);

/// @brief 创建 ALSA 播放后端（`aplay` 子进程；CLI 缺失返回 nullptr）。
std::unique_ptr<AudioSink> CreateAlsaAudioSink(const AudioSinkConfig& config, std::string* error);

/// @brief 创建空播放后端（只统计，不依赖音频设备；用于 CI/压测）。
std::unique_ptr<AudioSink> CreateNullAudioSink(const AudioFormat& format);

/// @brief 自动选择播放后端：Pulse → ALSA → 空（并写 `error` 说明降级原因）。
std::unique_ptr<AudioSink> CreateAudioSink(const AudioSinkConfig& config, std::string* error);

// ===========================================================================
// 性能 A/B 钩子（仅测试与基准使用，生产路径不调用）
// ===========================================================================

namespace detail {

/// @brief 教科书式 µ-law 编码（逐位找 exponent），性能对照的基线版本。
std::uint8_t UlawEncodeBaseline(std::int16_t pcm) noexcept;

/// @brief 查表 µ-law 编码（与生产路径同一实现）。
std::uint8_t UlawEncodeTable(std::int16_t pcm) noexcept;

/// @brief 查表 µ-law 解码（与生产路径同一实现）。
std::int16_t UlawDecodeTable(std::uint8_t value) noexcept;

/// @brief 批量 µ-law 编码（生产路径同款内联循环，无每样本函数调用）。
void UlawEncodeBlock(const std::int16_t* pcm, std::size_t samples,
                     std::uint8_t* out) noexcept;

/// @brief 批量 µ-law 解码（生产路径同款内联循环）。
void UlawDecodeBlock(const std::uint8_t* law, std::size_t samples,
                     std::int16_t* out) noexcept;

}  // namespace detail

// ===========================================================================
// 端到端会话
// ===========================================================================

/// @brief 会话配置。
struct AudioSessionConfig {
    AudioFormat format;                       ///< 格式（收发一致）。
    AudioCodec codec = AudioCodec::kOpus;     ///< 编码器。
    int bitrate = 32000;                      ///< 码率。
    bool real_capture = false;                ///< true = Pulse/ALSA 采集；false = 合成。
    bool real_playback = false;               ///< true = Pulse/ALSA 播放；false = 空后端。
    bool udp = false;                         ///< true = 真实 UDP 回环；false = 进程内链路。
    int max_jitter_ms = 0;                    ///< 进程内链路的抖动幅度。
    std::uint32_t jitter_seed = 0x1234ABCDU;  ///< 抖动随机种子。
    std::uint32_t drop_every = 0;             ///< 进程内链路丢包节奏。
    AudioCaptureConfig capture;               ///< 采集参数（real_capture 时生效）。
    AudioSinkConfig sink;                     ///< 播放参数（real_playback 时生效）。
    AudioJitterBufferConfig jitter;           ///< 抖动缓冲参数。
    std::uint32_t ssrc = 0x47503241U;         ///< 同步源。
};

/// @brief 会话统计。
struct AudioSessionStats {
    std::uint64_t captured_frames = 0;  ///< 采集帧数。
    std::uint64_t encoded_frames = 0;   ///< 编码帧数。
    std::uint64_t sent_packets = 0;     ///< 发送的分片数。
    std::uint64_t recv_packets = 0;     ///< 收到的分片数。
    std::uint64_t reassembled = 0;      ///< 重组帧数。
    std::uint64_t decoded_frames = 0;   ///< 解码帧数。
    std::uint64_t played_frames = 0;    ///< 播放帧数。
    std::uint64_t concealed = 0;        ///< PLC 帧数。
    std::uint64_t dropped_packets = 0;  ///< 抖动缓冲判定的丢包数。
    std::uint64_t bytes_captured = 0;   ///< 采集 PCM 字节数。
    std::uint64_t bytes_encoded = 0;    ///< 编码后字节数。
    LatencyRecorder latency;            ///< 采集→播放延迟（毫秒）。
    double output_rms = 0.0;            ///< 输出 PCM 的平均 RMS。
};

/// @brief 单线程端到端会话：采集 → 编码 → 分片 → 传输 → 抖动 → 重组 → 解码 → 播放。
/// @details 与视频侧 `MediaSender/MediaReceiver` 一样是单线程 pump 模型：
///          `SendNext()` 生产一帧，`Pump()` 消费已到达的包，`Run()` 把两者串起来。
class AudioSession {
   public:
    ~AudioSession();

    AudioSession(const AudioSession&) = delete;
    AudioSession& operator=(const AudioSession&) = delete;

    /// @brief 创建会话（会按配置创建采集/播放/传输后端）。
    /// @param error 失败原因。
    static std::unique_ptr<AudioSession> Create(const AudioSessionConfig& config,
                                                std::string* error);

    /// @brief 采集并发送一帧。
    /// @param captured_pcm 非空时输出采集到的原始 PCM（供逐帧比对）。
    /// @return 成功返回 true。
    bool SendNext(AudioFrame* captured_pcm);

    /// @brief 消费已到达的包并把解码结果写入播放后端。
    /// @return 本次交付（播放）的帧数。
    int Pump();

    /// @brief 单线程跑 `frames` 帧（每帧：SendNext + Pump + 等待一个包时长）。
    /// @return 实际播放帧数。
    int Run(int frames);

    /// @brief 统计。
    const AudioSessionStats& stats() const noexcept {
        return m_stats;
    }

    /// @brief 编码器名字（"opus(libopus.so.0)"/"pcmu"）。
    const char* encoder_name() const noexcept;

    /// @brief 采集后端名字。
    const char* capture_name() const noexcept;

    /// @brief 播放后端名字。
    const char* sink_name() const noexcept;

    /// @brief 当前抖动缓冲目标深度（包）。
    int jitter_target_packets() const noexcept;

    /// @brief 到达抖动估计（毫秒）。
    double jitter_ms() const noexcept;

   private:
    struct Impl;
    explicit AudioSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
    AudioSessionStats m_stats;
};

}  // namespace go2cpp::media
