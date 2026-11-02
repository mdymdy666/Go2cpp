/**
 * @file audio.cpp
 * @brief 音频链路实现：PulseAudio/ALSA 子进程采集与播放、G.711/PCM/Opus 编码、
 *        RTP 风格分片与重组、抖动缓冲、UDP 收发、进程内链路与端到端会话。
 * @details 实现取舍（与 `audio.hpp` 文件头呼应）：
 *
 *          1. **子进程而非链接库**：WSL2 的 PulseAudio 只提供 CLI 工具
 *             （`parec`/`pacat`），没有 `libpulse-dev` 也能跑；与视频侧
 *             `ffmpeg_codec.cpp` 的取舍一致。代价是每包一次管道拷贝（10ms 一包，
 *             44.1kHz 单声道 = 882B，拷贝开销可忽略）。
 *          2. **Opus 用 dlopen**：本机有 `libopus.so.0` 但没有 `opus/opus.h`，
 *             因此不写 `#include <opus.h>`，而是 `dlsym` 取 8 个符号并手写原型。
 *             好处是编译期零依赖、运行期有就用；坏处是符号表变化只能在运行期
 *             发现（`ProbeAudioCodecs()` 会给出明确失败原因）。
 *          3. **G.711 是查表实现，同时保留逐位基线**：`UlawEncodeBaseline` 是
 *             教科书式「移位找 exponent」循环，仅供性能 A/B 与回归对照；
 *             生产路径走 `kUlawEncodeExponent` 查表。两者输出逐字节一致，
 *             测试会穷举 65536 个输入断言这一点。
 *          4. **重采样**：Opus 只接受 8/12/16/24/48kHz，44.1kHz 需要 441→480
 *             的转换。这里用 16.16 定点线性插值（每样本约 3 条算术指令），
 *             不做带限滤波：对语音/测试音足够，对高频音乐会有轻微镜像，
 *             文档里标注为已知限制。
 *          5. **无锁**：与既有媒体模块一致，全是单线程 pump 模型；抖动缓冲、
 *             重组器、UDP 收发都不带内部线程。会话层由调用方驱动。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/audio.hpp"

#include "go2cpp/hash/fnv.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "go2cpp/media/transport.hpp"

extern char** environ;

namespace go2cpp::media {
namespace {

// ---------------------------------------------------------------------------
// 时钟与字节序
// ---------------------------------------------------------------------------

std::int64_t NowNs() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(NowNs() / 1000000);
}

inline void PutBe16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value & 0xFFU);
}

inline std::uint16_t GetBe16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

inline void PutBe32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    out[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

inline std::uint32_t GetBe32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

/// @brief FNV-1a 64（与 VideoFrame::Checksum 同一算法，便于跨模块对齐）。
/// @note 偏移量沿用历史的 1469598103934665603（少一位），保证既有校验值不变。
std::uint64_t Fnv1a64(const std::uint8_t* data, std::size_t bytes) noexcept {
    return go2cpp::hash::Fnv1a64Update(1469598103934665603ULL, data, bytes);
}

// ---------------------------------------------------------------------------
// G.711（µ-law / A-law）
// ---------------------------------------------------------------------------
//
// 教科书实现（`UlawEncodeBaseline`）的 exponent 靠循环移位找；生产实现
// （`UlawLinearToLaw`）用 256 项查表把「bias 后的幅值 >> 7」直接映射到
// exponent。两张表都是 constexpr 生成，进程启动零成本。

/// @brief 带 bias 的幅值 -> exponent（0..7）查表。
struct UlawEncodeTable {
    std::array<std::uint8_t, 256> exponent{};
};

constexpr UlawEncodeTable BuildUlawEncodeTable() noexcept {
    // seg_uend[8]：µ-law 各段的幅值上界（15-bit 输入含 bias）。
    constexpr std::uint16_t kSegEnd[8] = {0xFF, 0x1FF, 0x3FF, 0x7FF,
                                          0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};
    UlawEncodeTable table{};
    for (std::size_t index = 0; index < table.exponent.size(); ++index) {
        const std::uint16_t magnitude = static_cast<std::uint16_t>(index << 7);
        std::uint8_t exponent = 0;
        while (exponent < 7 && magnitude > kSegEnd[exponent]) {
            ++exponent;
        }
        table.exponent[index] = exponent;
    }
    return table;
}

inline constexpr UlawEncodeTable kUlawEncode = BuildUlawEncodeTable();

/// @brief µ-law 解码查表（256 项，constexpr 生成）。
struct UlawDecodeTable {
    std::array<std::int16_t, 256> linear{};
};

constexpr UlawDecodeTable BuildUlawDecodeTable() noexcept {
    UlawDecodeTable table{};
    for (std::size_t i = 0; i < table.linear.size(); ++i) {
        const std::uint8_t magnitude = static_cast<std::uint8_t>(~static_cast<std::uint8_t>(i));
        const int sign = magnitude & 0x80;
        const int exponent = (magnitude >> 4) & 0x07;
        const int mantissa = magnitude & 0x0F;
        int sample = ((mantissa << 3) + 0x84) << exponent;
        sample -= 0x84;
        if (sample > 32767) {
            sample = 32767;
        }
        table.linear[i] = static_cast<std::int16_t>(sign ? -sample : sample);
    }
    return table;
}

inline constexpr UlawDecodeTable kUlawDecode = BuildUlawDecodeTable();

/// @brief 教科书式 µ-law 编码（逐位找 exponent），仅用于性能 A/B 与回归。
std::uint8_t UlawEncodeBaselineImpl(std::int16_t pcm) noexcept {
    constexpr int kClip = 32635;
    const int sign = (pcm >> 8) & 0x80;
    int sample = pcm < 0 ? -static_cast<int>(pcm) : static_cast<int>(pcm);
    if (sample > kClip) {
        sample = kClip;
    }
    sample += 0x84;
    int exponent = 7;
    for (int mask = 0x4000; exponent > 0 && (sample & mask) == 0; mask >>= 1) {
        --exponent;
    }
    const int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return static_cast<std::uint8_t>(~(sign | (exponent << 4) | mantissa));
}

/// @brief 查表 µ-law 编码（生产路径）。
std::uint8_t UlawEncodeTableImpl(std::int16_t pcm) noexcept {
    constexpr int kClip = 32635;
    const int sign = (pcm >> 8) & 0x80;
    int sample = pcm < 0 ? -static_cast<int>(pcm) : static_cast<int>(pcm);
    if (sample > kClip) {
        sample = kClip;
    }
    sample += 0x84;
    const int exponent = kUlawEncode.exponent[static_cast<std::size_t>(sample >> 7)];
    const int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return static_cast<std::uint8_t>(~(sign | (exponent << 4) | mantissa));
}

/// @brief 查表 µ-law 解码（生产路径）。
std::int16_t UlawDecodeTableImpl(std::uint8_t value) noexcept {
    return kUlawDecode.linear[value];
}

/// @brief A-law 解码（公式版；A-law 不是本模块的性能重点）。
std::int16_t AlawLawToLinear(std::uint8_t value) noexcept {
    const std::uint8_t a = static_cast<std::uint8_t>(value ^ 0x55);
    int t = (a & 0x0F) << 4;
    const int seg = (a & 0x70) >> 4;
    if (seg == 0) {
        t += 8;
    } else if (seg == 1) {
        t += 0x108;
    } else {
        t += 0x108;
        t <<= seg - 1;
    }
    // 符号位取**原始码字**的 bit7（XOR 0x55 会翻转它，标准实现如此）。
    return static_cast<std::int16_t>((value & 0x80) ? t : -t);
}

/// @brief A-law 编码（公式版）。
std::uint8_t AlawLinearToLaw(std::int16_t pcm) noexcept {
    constexpr std::uint16_t kSegEnd[8] = {0x1F, 0x3F, 0x7F, 0xFF,
                                          0x1FF, 0x3FF, 0x7FF, 0xFFF};
    int sample = pcm >> 3;
    int mask = 0xD5;
    if (sample >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        sample = -sample - 1;
    }
    const std::uint16_t magnitude = static_cast<std::uint16_t>(sample);
    int seg = 0;
    while (seg < 8 && magnitude > kSegEnd[seg]) {
        ++seg;
    }
    if (seg >= 8) {
        return static_cast<std::uint8_t>(0x7F ^ mask);
    }
    int aval = seg << 4;
    // A-law 的段内步长 = 2^(seg-1)（seg>=2），seg 0/1 步长为 1 个 13-bit LSB。
    const int shift = seg < 2 ? 1 : seg;
    aval |= (magnitude >> shift) & 0x0F;
    return static_cast<std::uint8_t>(aval ^ mask);
}

// ---------------------------------------------------------------------------
// 子进程管道（PulseAudio / ALSA CLI）
// ---------------------------------------------------------------------------

void IgnoreSigPipeOnce() {
    static std::once_flag once;
    std::call_once(once, []() { ::signal(SIGPIPE, SIG_IGN); });
}

bool SetError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool ExecutableExists(const char* name) {
    if (name == nullptr || *name == '\0') {
        return false;
    }
    if (std::strchr(name, '/') != nullptr) {
        return ::access(name, X_OK) == 0;
    }
    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return false;
    }
    std::string current;
    for (const char* cursor = path;; ++cursor) {
        if (*cursor == ':' || *cursor == '\0') {
            if (!current.empty()) {
                std::string candidate = current + "/" + name;
                if (::access(candidate.c_str(), X_OK) == 0) {
                    return true;
                }
            }
            current.clear();
            if (*cursor == '\0') {
                break;
            }
        } else {
            current.push_back(*cursor);
        }
    }
    return false;
}

bool RunPactlInfo() {
    ::FILE* pipe = ::popen("pactl info 2>/dev/null", "r");
    if (pipe == nullptr) {
        return false;
    }
    char buffer[512];
    bool found = false;
    while (::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        if (std::strstr(buffer, "Server String:") != nullptr) {
            found = true;
            break;
        }
    }
    ::pclose(pipe);
    return found;
}

/// @brief 子进程 + 单端管道。读模式读子进程 stdout，写模式写子进程 stdin。
class PipeProcess {
   public:
    PipeProcess() = default;
    ~PipeProcess() {
        Close();
    }

    PipeProcess(const PipeProcess&) = delete;
    PipeProcess& operator=(const PipeProcess&) = delete;

    bool SpawnRead(const std::vector<std::string>& argv, std::string* error) {
        return Spawn(argv, true, error);
    }

    bool SpawnWrite(const std::vector<std::string>& argv, std::string* error) {
        IgnoreSigPipeOnce();
        return Spawn(argv, false, error);
    }

    bool ReadExact(void* data, std::size_t bytes, int timeout_ms) {
        auto* cursor = static_cast<std::uint8_t*>(data);
        std::size_t got = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (got < bytes) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            const int wait_ms = left > 0 ? static_cast<int>(left) : 0;
            struct pollfd pfd {};
            pfd.fd = m_fd;
            pfd.events = POLLIN;
            const int rc = ::poll(&pfd, 1, wait_ms);
            if (rc == 0) {
                return false;
            }
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
                (pfd.revents & POLLIN) == 0) {
                return false;
            }
            const ssize_t count = ::read(m_fd, cursor + got, bytes - got);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                m_failed = true;
                return false;
            }
            if (count == 0) {
                m_failed = true;
                return false;
            }
            got += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool WriteAll(const void* data, std::size_t bytes, int timeout_ms) {
        const auto* cursor = static_cast<const std::uint8_t*>(data);
        std::size_t sent = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (sent < bytes) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            const int wait_ms = left > 0 ? static_cast<int>(left) : 0;
            struct pollfd pfd {};
            pfd.fd = m_fd;
            pfd.events = POLLOUT;
            const int rc = ::poll(&pfd, 1, wait_ms);
            if (rc == 0) {
                return false;
            }
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
                (pfd.revents & POLLOUT) == 0) {
                m_failed = true;
                return false;
            }
            const ssize_t count = ::write(m_fd, cursor + sent, bytes - sent);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                m_failed = true;
                return false;
            }
            if (count == 0) {
                return false;
            }
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    void Close() {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        if (m_pid > 0) {
            int status = 0;
            for (int i = 0; i < 50; ++i) {
                const pid_t reaped = ::waitpid(m_pid, &status, WNOHANG);
                if (reaped == m_pid) {
                    m_pid = -1;
                    return;
                }
                ::usleep(10 * 1000);
            }
            ::kill(m_pid, SIGTERM);
            ::waitpid(m_pid, &status, 0);
            m_pid = -1;
        }
    }

    bool alive() const noexcept {
        return m_fd >= 0 && !m_failed;
    }

    int fd() const noexcept {
        return m_fd;
    }

   private:
    bool Spawn(const std::vector<std::string>& argv, bool for_read, std::string* error) {
        if (argv.empty()) {
            return SetError(error, "empty argv");
        }
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) {
            return SetError(error, "pipe() failed");
        }
        const int child_end = for_read ? fds[1] : fds[0];
        const int parent_end = for_read ? fds[0] : fds[1];

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, child_end, for_read ? STDOUT_FILENO : STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, child_end);
        posix_spawn_file_actions_addclose(&actions, parent_end);

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const std::string& item : argv) {
            args.push_back(const_cast<char*>(item.c_str()));
        }
        args.push_back(nullptr);

        pid_t child = -1;
        const int rc = ::posix_spawnp(&child, argv[0].c_str(), &actions, nullptr, args.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(child_end);
        if (rc != 0) {
            ::close(parent_end);
            return SetError(error, std::string("posix_spawnp(") + argv[0] + ") failed: " +
                                       std::strerror(rc));
        }
        m_pid = child;
        m_fd = parent_end;
        m_writer = !for_read;
        return true;
    }

    pid_t m_pid = -1;
    int m_fd = -1;
    bool m_writer = false;
    bool m_failed = false;
};

// ---------------------------------------------------------------------------
// Opus（dlopen，无 dev 头）
// ---------------------------------------------------------------------------

/// @brief libopus 的最小函数表（原型照抄 opus_defines.h / opus.h）。
struct OpusApi {
    void* library = nullptr;
    void* (*encoder_create)(std::int32_t rate, int channels, int application, int* error) = nullptr;
    std::int32_t (*encode)(void* encoder, const std::int16_t* pcm, int frame_size,
                           unsigned char* data, std::int32_t max_bytes) = nullptr;
    int (*encoder_ctl)(void* encoder, int request, ...) = nullptr;
    void (*encoder_destroy)(void* encoder) = nullptr;
    void* (*decoder_create)(std::int32_t rate, int channels, int* error) = nullptr;
    int (*decode)(void* decoder, const unsigned char* data, std::int32_t length,
                  std::int16_t* pcm, int frame_size, int decode_fec) = nullptr;
    void (*decoder_destroy)(void* decoder) = nullptr;
    const char* (*version_string)() = nullptr;

    bool Ready() const noexcept {
        return library != nullptr && encoder_create != nullptr && encode != nullptr &&
               encoder_ctl != nullptr && encoder_destroy != nullptr &&
               decoder_create != nullptr && decode != nullptr && decoder_destroy != nullptr;
    }
};

const OpusApi& Opus() {
    static const OpusApi api = []() {
        OpusApi result;
        constexpr const char* kCandidates[] = {"libopus.so.0", "libopus.so", "libopus.so.0.9.1"};
        for (const char* name : kCandidates) {
            result.library = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (result.library != nullptr) {
                break;
            }
        }
        if (result.library == nullptr) {
            return result;
        }
        auto load = [&result](const char* symbol) { return ::dlsym(result.library, symbol); };
        result.encoder_create = reinterpret_cast<decltype(result.encoder_create)>(
            load("opus_encoder_create"));
        result.encode = reinterpret_cast<decltype(result.encode)>(load("opus_encode"));
        result.encoder_ctl = reinterpret_cast<decltype(result.encoder_ctl)>(load("opus_encoder_ctl"));
        result.encoder_destroy =
            reinterpret_cast<decltype(result.encoder_destroy)>(load("opus_encoder_destroy"));
        result.decoder_create = reinterpret_cast<decltype(result.decoder_create)>(
            load("opus_decoder_create"));
        result.decode = reinterpret_cast<decltype(result.decode)>(load("opus_decode"));
        result.decoder_destroy =
            reinterpret_cast<decltype(result.decoder_destroy)>(load("opus_decoder_destroy"));
        result.version_string =
            reinterpret_cast<decltype(result.version_string)>(load("opus_get_version_string"));
        return result;
    }();
    return api;
}

// Opus 常量（opus_defines.h）。
constexpr int kOpusApplicationAudio = 2049;
constexpr int kOpusSetBitrate = 4002;
constexpr int kOpusSetComplexity = 4010;
constexpr int kOpusSetInbandFec = 4012;
constexpr int kOpusSetDtx = 4016;
constexpr std::int32_t kOpusClockRate = 48000;
constexpr int kOpusMaxPacketBytes = 1275;

/// @brief Opus 期望的 48kHz 帧长（取 10/20/40/60ms 四档）。
int OpusFrameSamples(const AudioFormat& format) noexcept {
    const int us = format.PacketDurationUs();
    if (us >= 55000) {
        return 2880;
    }
    if (us >= 35000) {
        return 1920;
    }
    if (us >= 15000) {
        return 960;
    }
    return 480;
}

/// @brief 16.16 定点线性重采样（单/双声道交错处理）。
void ResampleLinear(const std::int16_t* in, int in_frames, int in_rate, std::int16_t* out,
                    int out_frames, int out_rate, int channels) noexcept {
    if (in_frames <= 0 || out_frames <= 0 || channels <= 0) {
        return;
    }
    if (in_rate == out_rate) {
        std::memcpy(out, in, static_cast<std::size_t>(in_frames) * channels * sizeof(std::int16_t));
        return;
    }
    const std::uint32_t step = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(in_rate) << 16) / static_cast<std::uint64_t>(out_rate));
    std::uint32_t position = 0;
    const int last_frame = in_frames - 1;
    for (int frame = 0; frame < out_frames; ++frame) {
        const int index = static_cast<int>(position >> 16);
        const int next = index < last_frame ? index + 1 : last_frame;
        const int frac = static_cast<int>(position & 0xFFFFU);
        for (int channel = 0; channel < channels; ++channel) {
            const int a = in[static_cast<std::size_t>(index) * channels + channel];
            const int b = in[static_cast<std::size_t>(next) * channels + channel];
            const int mixed = (a * (0x10000 - frac) + b * frac) >> 16;
            out[static_cast<std::size_t>(frame) * channels + channel] =
                static_cast<std::int16_t>(mixed);
        }
        position += step;
        if (position > (static_cast<std::uint32_t>(last_frame) << 16)) {
            position = static_cast<std::uint32_t>(last_frame) << 16;
        }
    }
}

// ---------------------------------------------------------------------------
// 合成音源的正弦表
// ---------------------------------------------------------------------------

constexpr std::size_t kSineTableSize = 4096;

std::array<std::int16_t, kSineTableSize> BuildSineTable() noexcept {
    std::array<std::int16_t, kSineTableSize> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        const double phase = 6.283185307179586 * static_cast<double>(i) /
                             static_cast<double>(kSineTableSize);
        table[i] = static_cast<std::int16_t>(16384.0 * std::sin(phase));
    }
    return table;
}

const std::array<std::int16_t, kSineTableSize>& SineTable() {
    static const std::array<std::int16_t, kSineTableSize> table = BuildSineTable();
    return table;
}

}  // namespace

// ===========================================================================
// 格式与帧
// ===========================================================================

const char* AudioCodecName(AudioCodec codec) noexcept {
    switch (codec) {
        case AudioCodec::kOpus:
            return "opus";
        case AudioCodec::kPcmu:
            return "pcmu";
        case AudioCodec::kPcma:
            return "pcma";
        case AudioCodec::kPcmS16LE:
            return "pcm";
    }
    return "unknown";
}

bool ParseAudioCodec(const char* text, AudioCodec* out) noexcept {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    if (std::strcmp(text, "opus") == 0) {
        *out = AudioCodec::kOpus;
        return true;
    }
    if (std::strcmp(text, "pcmu") == 0 || std::strcmp(text, "ulaw") == 0) {
        *out = AudioCodec::kPcmu;
        return true;
    }
    if (std::strcmp(text, "pcma") == 0 || std::strcmp(text, "alaw") == 0) {
        *out = AudioCodec::kPcma;
        return true;
    }
    if (std::strcmp(text, "pcm") == 0 || std::strcmp(text, "pcm_s16le") == 0) {
        *out = AudioCodec::kPcmS16LE;
        return true;
    }
    return false;
}

std::uint8_t DefaultAudioPayloadType(AudioCodec codec) noexcept {
    switch (codec) {
        case AudioCodec::kPcmu:
            return 0;
        case AudioCodec::kPcma:
            return 8;
        case AudioCodec::kPcmS16LE:
            return 96;
        case AudioCodec::kOpus:
        default:
            return 111;
    }
}

int AudioFormat::EffectiveFramesPerPacket() const noexcept {
    if (frames_per_packet > 0) {
        return frames_per_packet;
    }
    if (sample_rate <= 0) {
        return 0;
    }
    return static_cast<int>((static_cast<std::int64_t>(sample_rate) * kDefaultPacketMs) / 1000);
}

int AudioFormat::PacketDurationUs() const noexcept {
    const int frames = EffectiveFramesPerPacket();
    if (sample_rate <= 0 || frames <= 0) {
        return 0;
    }
    return static_cast<int>((static_cast<std::int64_t>(frames) * 1000000) / sample_rate);
}

bool AudioFormat::Valid() const noexcept {
    if (sample_rate < 8000 || sample_rate > 192000) {
        return false;
    }
    if (channels < 1 || channels > 2) {
        return false;
    }
    if (sample_format != AudioSampleFormat::kS16LE) {
        return false;
    }
    if (frames_per_packet < 0) {
        return false;
    }
    const int frames = EffectiveFramesPerPacket();
    if (frames <= 0) {
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels) * 2;
    return bytes <= kMaxAudioPayloadBytes;
}

bool AudioFormat::operator==(const AudioFormat& other) const noexcept {
    return sample_rate == other.sample_rate && channels == other.channels &&
           sample_format == other.sample_format &&
           EffectiveFramesPerPacket() == other.EffectiveFramesPerPacket();
}

bool AudioFrame::Valid() const noexcept {
    if (!format.Valid() || data.empty() || data.size() > kMaxAudioPayloadBytes) {
        return false;
    }
    return data.size() == static_cast<std::size_t>(format.BytesPerPacket());
}

std::uint64_t AudioFrame::Checksum() const noexcept {
    return ChecksumBytes(data.data(), data.size());
}

std::uint64_t AudioFrame::ChecksumBytes(const std::uint8_t* data, std::size_t bytes) noexcept {
    return Fnv1a64(data, bytes);
}

double AudioFrame::Rms() const noexcept {
    if (data.size() < 2) {
        return 0.0;
    }
    const auto* samples = reinterpret_cast<const std::int16_t*>(data.data());
    const std::size_t count = data.size() / 2;
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double value = static_cast<double>(samples[i]);
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(count));
}

const char* SyntheticAudioPatternName(SyntheticAudioPattern pattern) noexcept {
    switch (pattern) {
        case SyntheticAudioPattern::kSilence:
            return "silence";
        case SyntheticAudioPattern::kSine440:
            return "sine440";
        case SyntheticAudioPattern::kSweep:
            return "sweep";
        case SyntheticAudioPattern::kNoise:
            return "noise";
        case SyntheticAudioPattern::kDtmf:
            return "dtmf";
    }
    return "unknown";
}

// ===========================================================================
// 采集
// ===========================================================================

bool PulseAudioAvailable() {
    static const bool available = RunPactlInfo();
    return available;
}

bool PulseCaptureAvailable() {
    static const bool available = PulseAudioAvailable() && ExecutableExists("parec");
    return available;
}

bool PulsePlaybackAvailable() {
    static const bool available = PulseAudioAvailable() && ExecutableExists("pacat");
    return available;
}

bool AlsaCaptureAvailable() {
    static const bool available = ExecutableExists("arecord");
    return available;
}

bool AlsaPlaybackAvailable() {
    static const bool available = ExecutableExists("aplay");
    return available;
}

namespace {

/// @brief CLI 采集源（Pulse 的 `parec` / ALSA 的 `arecord` 共用）。
class ProcessAudioCapture final : public AudioSource {
   public:
    ProcessAudioCapture(const AudioCaptureConfig& config, bool pulse)
        : m_format(config.format), m_pulse(pulse), m_config(config) {}

    bool Open(std::string* error) {
        if (!m_format.Valid()) {
            return SetError(error, "invalid audio format");
        }
        if (m_pulse) {
            if (!PulseCaptureAvailable()) {
                return SetError(error, "PulseAudio capture unavailable");
            }
            std::vector<std::string> argv = {
                "parec",
                "--format=s16le",
                "--rate=" + std::to_string(m_format.sample_rate),
                "--channels=" + std::to_string(m_format.channels),
                "--raw",
                "--latency-msec=" + std::to_string(m_config.latency_ms)};
            if (!m_config.device.empty()) {
                argv.push_back("--device=" + m_config.device);
            }
            if (!m_process.SpawnRead(argv, error)) {
                return false;
            }
        } else {
            if (!AlsaCaptureAvailable()) {
                return SetError(error, "arecord unavailable (alsa-utils not installed)");
            }
            std::vector<std::string> argv = {
                "arecord",
                "-q",
                "-t",
                "raw",
                "-f",
                "S16_LE",
                "-r",
                std::to_string(m_format.sample_rate),
                "-c",
                std::to_string(m_format.channels),
                "--buffer-time=" + std::to_string(m_config.latency_ms * 1000)};
            if (!m_config.device.empty()) {
                argv.push_back("-D");
                argv.push_back(m_config.device);
            }
            if (!m_process.SpawnRead(argv, error)) {
                return false;
            }
        }
        m_packets = 0;
        m_bytes = 0;
        m_sequence = 0;
        m_timestamp = 0;
        return true;
    }

    bool Read(AudioFrame* out) override {
        const std::size_t bytes = static_cast<std::size_t>(m_format.BytesPerPacket());
        out->format = m_format;
        out->data.resize(bytes);
        if (!m_process.ReadExact(out->data.data(), bytes, 2000)) {
            out->data.clear();
            return false;
        }
        out->sequence = m_sequence++;
        out->timestamp = m_timestamp;
        m_timestamp += static_cast<std::uint32_t>(m_format.EffectiveFramesPerPacket());
        out->capture_ns = NowNs();
        ++m_packets;
        m_bytes += bytes;
        return true;
    }

    const char* name() const noexcept override {
        return m_pulse ? "pulse:parec" : "alsa:arecord";
    }

    bool available() const noexcept override {
        return m_process.alive();
    }

    std::uint64_t packets() const noexcept override {
        return m_packets;
    }

    std::uint64_t bytes() const noexcept override {
        return m_bytes;
    }

    AudioFormat format() const noexcept override {
        return m_format;
    }

   private:
    AudioFormat m_format;
    bool m_pulse;
    AudioCaptureConfig m_config;
    PipeProcess m_process;
    std::uint64_t m_packets = 0;
    std::uint64_t m_bytes = 0;
    std::uint32_t m_sequence = 0;
    std::uint32_t m_timestamp = 0;
};

/// @brief 合成音源（确定性；跨包相位连续）。
class SyntheticAudioCapture final : public AudioSource {
   public:
    SyntheticAudioCapture(const AudioCaptureConfig& config,
                          const std::array<std::int16_t, kSineTableSize>& sine)
        : m_format(config.format),
          m_pattern(config.pattern),
          m_seed(config.seed == 0 ? 0x5EED1234U : config.seed),
          m_sine(sine) {
        const double rate = static_cast<double>(m_format.sample_rate > 0 ? m_format.sample_rate : 44100);
        m_sine_step = static_cast<std::uint32_t>(440.0 * 4294967296.0 / rate);
        m_dtmf_low_step = static_cast<std::uint32_t>(697.0 * 4294967296.0 / rate);
        m_dtmf_high_step = static_cast<std::uint32_t>(1209.0 * 4294967296.0 / rate);
    }

    bool Read(AudioFrame* out) override {
        const int frames = m_format.EffectiveFramesPerPacket();
        const int channels = m_format.channels;
        const std::size_t bytes = static_cast<std::size_t>(frames) * channels * 2;
        out->format = m_format;
        out->data.resize(bytes);
        auto* samples = reinterpret_cast<std::int16_t*>(out->data.data());
        // 每个「帧」生成一次样本后复制到所有声道：立体声左右一致，
        // 与真实单源上混（mono → stereo）的行为一致，也便于逐样本比对。
        for (int frame = 0; frame < frames; ++frame) {
            const std::int16_t value = NextSample();
            for (int channel = 0; channel < channels; ++channel) {
                samples[static_cast<std::size_t>(frame) * channels + channel] = value;
            }
        }
        out->sequence = m_sequence++;
        out->timestamp = m_timestamp;
        m_timestamp += static_cast<std::uint32_t>(frames);
        out->capture_ns = NowNs();
        ++m_packets;
        m_bytes += bytes;
        return true;
    }

    const char* name() const noexcept override {
        return "synthetic";
    }

    bool available() const noexcept override {
        return true;
    }

    std::uint64_t packets() const noexcept override {
        return m_packets;
    }

    std::uint64_t bytes() const noexcept override {
        return m_bytes;
    }

    AudioFormat format() const noexcept override {
        return m_format;
    }

   private:
    std::int16_t NextSample() noexcept {
        const auto rate = static_cast<std::uint32_t>(m_format.sample_rate);
        switch (m_pattern) {
            case SyntheticAudioPattern::kSilence:
                return 0;
            case SyntheticAudioPattern::kSine440: {
                const std::int16_t value = m_sine[(m_phase >> 20) & (kSineTableSize - 1)];
                m_phase += m_sine_step;
                return value;
            }
            case SyntheticAudioPattern::kSweep: {
                const double elapsed = static_cast<double>(m_sweep_samples) / static_cast<double>(rate);
                const double cycle = std::fmod(elapsed, 2.0);
                const double frequency = 200.0 + 3800.0 * (cycle / 2.0);
                const auto step = static_cast<std::uint32_t>(frequency * 4294967296.0 /
                                                             static_cast<double>(rate));
                const std::int16_t value = m_sine[(m_sweep_phase >> 20) & (kSineTableSize - 1)];
                m_sweep_phase += step;
                ++m_sweep_samples;
                return value;
            }
            case SyntheticAudioPattern::kNoise: {
                m_seed ^= m_seed << 13;
                m_seed ^= m_seed >> 17;
                m_seed ^= m_seed << 5;
                return static_cast<std::int16_t>(static_cast<int>(m_seed >> 16) - 32768);
            }
            case SyntheticAudioPattern::kDtmf: {
                const std::int16_t low = m_sine[(m_phase >> 20) & (kSineTableSize - 1)];
                const std::int16_t high = m_sine[(m_phase_high >> 20) & (kSineTableSize - 1)];
                m_phase += m_dtmf_low_step;
                m_phase_high += m_dtmf_high_step;
                return static_cast<std::int16_t>((static_cast<int>(low) + high) / 2);
            }
        }
        return 0;
    }

    AudioFormat m_format;
    SyntheticAudioPattern m_pattern;
    std::uint32_t m_seed;
    const std::array<std::int16_t, kSineTableSize>& m_sine;
    std::uint32_t m_sine_step = 0;
    std::uint32_t m_dtmf_low_step = 0;
    std::uint32_t m_dtmf_high_step = 0;
    std::uint32_t m_phase = 0;
    std::uint32_t m_phase_high = 0;
    std::uint32_t m_sweep_phase = 0;
    std::uint64_t m_sweep_samples = 0;
    std::uint64_t m_packets = 0;
    std::uint64_t m_bytes = 0;
    std::uint32_t m_sequence = 0;
    std::uint32_t m_timestamp = 0;
};

}  // namespace

std::unique_ptr<AudioSource> CreatePulseAudioCapture(const AudioCaptureConfig& config,
                                                     std::string* error) {
    auto source = std::make_unique<ProcessAudioCapture>(config, true);
    if (!source->Open(error)) {
        return nullptr;
    }
    return source;
}

std::unique_ptr<AudioSource> CreateAlsaAudioCapture(const AudioCaptureConfig& config,
                                                    std::string* error) {
    auto source = std::make_unique<ProcessAudioCapture>(config, false);
    if (!source->Open(error)) {
        return nullptr;
    }
    return source;
}

std::unique_ptr<AudioSource> CreateSyntheticAudioSource(const AudioCaptureConfig& config,
                                                        std::string* error) {
    if (!config.format.Valid()) {
        SetError(error, "invalid audio format");
        return nullptr;
    }
    return std::make_unique<SyntheticAudioCapture>(config, SineTable());
}

std::unique_ptr<AudioSource> CreateAudioSource(const AudioCaptureConfig& config,
                                               std::string* error) {
    std::string reason;
    if (std::unique_ptr<AudioSource> source = CreatePulseAudioCapture(config, &reason)) {
        return source;
    }
    if (std::unique_ptr<AudioSource> source = CreateAlsaAudioCapture(config, &reason)) {
        return source;
    }
    if (error != nullptr) {
        *error = "audio capture degraded to synthetic: " + reason;
    }
    return CreateSyntheticAudioSource(config, nullptr);
}

// ===========================================================================
// 性能 A/B 钩子（仅测试使用，生产路径不调用）
// ===========================================================================

namespace detail {

std::uint8_t UlawEncodeBaseline(std::int16_t pcm) noexcept {
    return UlawEncodeBaselineImpl(pcm);
}

std::uint8_t UlawEncodeTable(std::int16_t pcm) noexcept {
    return UlawEncodeTableImpl(pcm);
}

std::int16_t UlawDecodeTable(std::uint8_t value) noexcept {
    return UlawDecodeTableImpl(value);
}

void UlawEncodeBlock(const std::int16_t* pcm, std::size_t samples,
                     std::uint8_t* out) noexcept {
    for (std::size_t i = 0; i < samples; ++i) {
        out[i] = UlawEncodeTableImpl(pcm[i]);
    }
}

void UlawDecodeBlock(const std::uint8_t* law, std::size_t samples,
                     std::int16_t* out) noexcept {
    for (std::size_t i = 0; i < samples; ++i) {
        out[i] = UlawDecodeTableImpl(law[i]);
    }
}

}  // namespace detail
// ===========================================================================
// 编码 / 解码
// ===========================================================================

namespace {

/// @brief 不压缩（直通）编码器；用于逐字节校验与最坏情况带宽。
class PcmAudioEncoder final : public AudioEncoder {
   public:
    explicit PcmAudioEncoder(const AudioFormat& format) : m_format(format) {}

    bool Encode(const AudioFrame& frame, std::vector<std::uint8_t>* out) override {
        if (!frame.Valid()) {
            return false;
        }
        out->resize(frame.data.size());
        std::memcpy(out->data(), frame.data.data(), frame.data.size());
        ++m_frames;
        m_bytes += frame.data.size();
        return true;
    }

    AudioCodec codec() const noexcept override {
        return AudioCodec::kPcmS16LE;
    }

    const char* name() const noexcept override {
        return "pcm-s16le";
    }

    std::uint64_t encoded_frames() const noexcept override {
        return m_frames;
    }

    std::uint64_t encoded_bytes() const noexcept override {
        return m_bytes;
    }

   private:
    AudioFormat m_format;
    std::uint64_t m_frames = 0;
    std::uint64_t m_bytes = 0;
};

/// @brief G.711 编码器（µ-law / A-law，每个样本 1 字节）。
class G711AudioEncoder final : public AudioEncoder {
   public:
    G711AudioEncoder(AudioCodec codec, const AudioFormat& format) : m_codec(codec), m_format(format) {}

    bool Encode(const AudioFrame& frame, std::vector<std::uint8_t>* out) override {
        if (!frame.Valid()) {
            return false;
        }
        const std::size_t count = frame.data.size() / 2;
        out->resize(count);
        const auto* samples = reinterpret_cast<const std::int16_t*>(frame.data.data());
        if (m_codec == AudioCodec::kPcmu) {
            for (std::size_t i = 0; i < count; ++i) {
                (*out)[i] = UlawEncodeTableImpl(samples[i]);
            }
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                (*out)[i] = AlawLinearToLaw(samples[i]);
            }
        }
        ++m_frames;
        m_bytes += count;
        return true;
    }

    AudioCodec codec() const noexcept override {
        return m_codec;
    }

    const char* name() const noexcept override {
        return m_codec == AudioCodec::kPcmu ? "g711-ulaw" : "g711-alaw";
    }

    std::uint64_t encoded_frames() const noexcept override {
        return m_frames;
    }

    std::uint64_t encoded_bytes() const noexcept override {
        return m_bytes;
    }

   private:
    AudioCodec m_codec;
    AudioFormat m_format;
    std::uint64_t m_frames = 0;
    std::uint64_t m_bytes = 0;
};

/// @brief G.711 解码器（含衰减重复的简化 PLC）。
class G711AudioDecoder final : public AudioDecoder {   public:
    G711AudioDecoder(AudioCodec codec, const AudioFormat& format) : m_codec(codec), m_format(format) {}

    bool Decode(const std::uint8_t* payload, std::size_t bytes, AudioFrame* out) override {
        const std::size_t target_samples = static_cast<std::size_t>(m_format.BytesPerPacket()) / 2;
        out->format = m_format;
        out->data.resize(target_samples * 2);
        auto* samples = reinterpret_cast<std::int16_t*>(out->data.data());
        if (payload == nullptr || bytes == 0) {
            // PLC：重复上一帧并衰减一半；没有历史则静音。
            if (m_last.size() == target_samples) {
                for (std::size_t i = 0; i < target_samples; ++i) {
                    samples[i] = static_cast<std::int16_t>(m_last[i] / 2);
                    m_last[i] = samples[i];
                }
            } else {
                std::memset(samples, 0, out->data.size());
                m_last.assign(target_samples, 0);
            }
            ++m_concealed;
            return true;
        }
        if (bytes > target_samples) {
            ++m_dropped;
            return false;
        }
        for (std::size_t i = 0; i < bytes; ++i) {
            samples[i] = m_codec == AudioCodec::kPcmu ? UlawDecodeTableImpl(payload[i])
                                                      : AlawLawToLinear(payload[i]);
        }
        for (std::size_t i = bytes; i < target_samples; ++i) {
            samples[i] = 0;
        }
        m_last.assign(samples, samples + target_samples);
        ++m_frames;
        m_samples += bytes;
        return true;
    }

    AudioCodec codec() const noexcept override {
        return m_codec;
    }

    std::uint64_t decoded_frames() const noexcept override {
        return m_frames;
    }

    std::uint64_t decoded_samples() const noexcept override {
        return m_samples;
    }

    std::uint64_t concealed_frames() const noexcept override {
        return m_concealed;
    }

    std::uint64_t dropped_frames() const noexcept override {
        return m_dropped;
    }

   private:
    AudioCodec m_codec;
    AudioFormat m_format;
    std::vector<std::int16_t> m_last;
    std::uint64_t m_frames = 0;
    std::uint64_t m_samples = 0;
    std::uint64_t m_concealed = 0;
    std::uint64_t m_dropped = 0;
};

/// @brief 不压缩（直通）解码器；与 `PcmAudioEncoder` 配对，逐字节可校验。
class PcmAudioDecoder final : public AudioDecoder {
   public:
    explicit PcmAudioDecoder(const AudioFormat& format) : m_format(format) {}

    bool Decode(const std::uint8_t* payload, std::size_t bytes, AudioFrame* out) override {
        const std::size_t expected = static_cast<std::size_t>(m_format.BytesPerPacket());
        out->format = m_format;
        out->data.resize(expected);
        if (payload == nullptr || bytes == 0) {
            std::memset(out->data.data(), 0, expected);
            ++m_concealed;
            return true;
        }
        if (bytes != expected) {
            ++m_dropped;
            return false;
        }
        std::memcpy(out->data.data(), payload, expected);
        ++m_frames;
        m_samples += expected / 2;
        return true;
    }

    AudioCodec codec() const noexcept override {
        return AudioCodec::kPcmS16LE;
    }

    std::uint64_t decoded_frames() const noexcept override {
        return m_frames;
    }

    std::uint64_t decoded_samples() const noexcept override {
        return m_samples;
    }

    std::uint64_t concealed_frames() const noexcept override {
        return m_concealed;
    }

    std::uint64_t dropped_frames() const noexcept override {
        return m_dropped;
    }

   private:
    AudioFormat m_format;
    std::uint64_t m_frames = 0;
    std::uint64_t m_samples = 0;
    std::uint64_t m_concealed = 0;
    std::uint64_t m_dropped = 0;
};

/// @brief Opus 编码器（dlopen libopus，48kHz 内部时钟 + 重采样）。
class OpusAudioEncoder final : public AudioEncoder {
   public:
    explicit OpusAudioEncoder(const AudioEncoderConfig& config)
        : m_config(config), m_frame_samples(OpusFrameSamples(config.format)) {}

    ~OpusAudioEncoder() override {
        if (m_encoder != nullptr) {
            Opus().encoder_destroy(m_encoder);
        }
    }

    bool Init(std::string* error) {
        const OpusApi& api = Opus();
        if (!api.Ready()) {
            return SetError(error, "libopus not available (dlopen failed)");
        }
        int opus_error = 0;
        m_encoder = api.encoder_create(kOpusClockRate, m_config.format.channels,
                                       kOpusApplicationAudio, &opus_error);
        if (m_encoder == nullptr || opus_error != 0) {
            return SetError(error, "opus_encoder_create failed: " + std::to_string(opus_error));
        }
        if (m_config.bitrate > 0) {
            api.encoder_ctl(m_encoder, kOpusSetBitrate, m_config.bitrate);
        }
        api.encoder_ctl(m_encoder, kOpusSetComplexity, std::clamp(m_config.complexity, 0, 10));
        api.encoder_ctl(m_encoder, kOpusSetInbandFec, m_config.inband_fec ? 1 : 0);
        api.encoder_ctl(m_encoder, kOpusSetDtx, m_config.dtx ? 1 : 0);
        m_pcm.resize(static_cast<std::size_t>(m_frame_samples) *
                     static_cast<std::size_t>(m_config.format.channels));
        m_name = "opus(libopus.so.0)";
        return true;
    }

    bool Encode(const AudioFrame& frame, std::vector<std::uint8_t>* out) override {
        if (!frame.Valid()) {
            return false;
        }
        const int channels = m_config.format.channels;
        const int in_frames = static_cast<int>(frame.frames());
        if (in_frames <= 0) {
            return false;
        }
        const auto* input = reinterpret_cast<const std::int16_t*>(frame.data.data());
        const std::int16_t* pcm = input;
        if (frame.format.sample_rate != kOpusClockRate || in_frames != m_frame_samples) {
            ResampleLinear(input, in_frames, frame.format.sample_rate, m_pcm.data(), m_frame_samples,
                           kOpusClockRate, channels);
            pcm = m_pcm.data();
        }
        std::array<unsigned char, kOpusMaxPacketBytes> buffer{};
        const std::int32_t produced =
            Opus().encode(m_encoder, pcm, m_frame_samples, buffer.data(), kOpusMaxPacketBytes);
        if (produced < 0) {
            return false;
        }
        out->assign(buffer.data(), buffer.data() + produced);
        ++m_frames;
        m_bytes += static_cast<std::uint64_t>(produced);
        return true;
    }

    AudioCodec codec() const noexcept override {
        return AudioCodec::kOpus;
    }

    const char* name() const noexcept override {
        return m_name.c_str();
    }

    std::uint64_t encoded_frames() const noexcept override {
        return m_frames;
    }

    std::uint64_t encoded_bytes() const noexcept override {
        return m_bytes;
    }

   private:
    AudioEncoderConfig m_config;
    int m_frame_samples;
    void* m_encoder = nullptr;
    std::vector<std::int16_t> m_pcm;
    std::string m_name;
    std::uint64_t m_frames = 0;
    std::uint64_t m_bytes = 0;
};

/// @brief Opus 解码器（含 PLC），输出重采样回源采样率。
class OpusAudioDecoder final : public AudioDecoder {
   public:
    explicit OpusAudioDecoder(const AudioFormat& format)
        : m_format(format), m_frame_samples(OpusFrameSamples(format)) {}

    ~OpusAudioDecoder() override {
        if (m_decoder != nullptr) {
            Opus().decoder_destroy(m_decoder);
        }
    }

    bool Init(std::string* error) {
        const OpusApi& api = Opus();
        if (!api.Ready()) {
            return SetError(error, "libopus not available (dlopen failed)");
        }
        int opus_error = 0;
        m_decoder = api.decoder_create(kOpusClockRate, m_format.channels, &opus_error);
        if (m_decoder == nullptr || opus_error != 0) {
            return SetError(error, "opus_decoder_create failed: " + std::to_string(opus_error));
        }
        m_pcm48.resize(static_cast<std::size_t>(m_frame_samples) *
                       static_cast<std::size_t>(m_format.channels));
        return true;
    }

    bool Decode(const std::uint8_t* payload, std::size_t bytes, AudioFrame* out) override {
        const int channels = m_format.channels;
        const int out_frames = m_format.EffectiveFramesPerPacket();
        out->format = m_format;
        out->data.resize(static_cast<std::size_t>(out_frames) * channels * 2);
        int decoded = 0;
        if (payload == nullptr || bytes == 0) {
            decoded = Opus().decode(m_decoder, nullptr, 0, m_pcm48.data(), m_frame_samples, 0);
            if (decoded < 0) {
                ++m_dropped;
                return false;
            }
            ++m_concealed;
        } else {
            if (bytes > kMaxAudioPayloadBytes) {
                ++m_dropped;
                return false;
            }
            decoded = Opus().decode(m_decoder, payload, static_cast<std::int32_t>(bytes),
                                    m_pcm48.data(), m_frame_samples, 0);
            if (decoded < 0) {
                ++m_dropped;
                return false;
            }
            ++m_frames;
            m_samples += static_cast<std::uint64_t>(decoded);
        }
        auto* output = reinterpret_cast<std::int16_t*>(out->data.data());
        if (decoded == out_frames && m_format.sample_rate == kOpusClockRate) {
            std::memcpy(output, m_pcm48.data(),
                        static_cast<std::size_t>(out_frames) * channels * sizeof(std::int16_t));
        } else {
            ResampleLinear(m_pcm48.data(), decoded, kOpusClockRate, output, out_frames,
                           m_format.sample_rate, channels);
        }
        return true;
    }

    AudioCodec codec() const noexcept override {
        return AudioCodec::kOpus;
    }

    std::uint64_t decoded_frames() const noexcept override {
        return m_frames;
    }

    std::uint64_t decoded_samples() const noexcept override {
        return m_samples;
    }

    std::uint64_t concealed_frames() const noexcept override {
        return m_concealed;
    }

    std::uint64_t dropped_frames() const noexcept override {
        return m_dropped;
    }

   private:
    AudioFormat m_format;
    int m_frame_samples;
    void* m_decoder = nullptr;
    std::vector<std::int16_t> m_pcm48;
    std::uint64_t m_frames = 0;
    std::uint64_t m_samples = 0;
    std::uint64_t m_concealed = 0;
    std::uint64_t m_dropped = 0;
};

}  // namespace

std::unique_ptr<AudioEncoder> CreateAudioEncoder(const AudioEncoderConfig& config,
                                                 std::string* error) {
    if (!config.format.Valid()) {
        SetError(error, "invalid audio format");
        return nullptr;
    }
    switch (config.codec) {
        case AudioCodec::kOpus: {
            auto encoder = std::make_unique<OpusAudioEncoder>(config);
            if (!encoder->Init(error)) {
                return nullptr;
            }
            return encoder;
        }
        case AudioCodec::kPcmu:
        case AudioCodec::kPcma:
            return std::make_unique<G711AudioEncoder>(config.codec, config.format);
        case AudioCodec::kPcmS16LE:
            return std::make_unique<PcmAudioEncoder>(config.format);
    }
    SetError(error, "unknown audio codec");
    return nullptr;
}

std::unique_ptr<AudioDecoder> CreateAudioDecoder(AudioCodec codec, const AudioFormat& format,
                                                 std::string* error) {
    if (!format.Valid()) {
        SetError(error, "invalid audio format");
        return nullptr;
    }
    switch (codec) {
        case AudioCodec::kOpus: {
            auto decoder = std::make_unique<OpusAudioDecoder>(format);
            if (!decoder->Init(error)) {
                return nullptr;
            }
            return decoder;
        }
        case AudioCodec::kPcmu:
        case AudioCodec::kPcma:
            return std::make_unique<G711AudioDecoder>(codec, format);
        case AudioCodec::kPcmS16LE:
            return std::make_unique<PcmAudioDecoder>(format);
    }
    SetError(error, "unknown audio codec");
    return nullptr;
}

AudioCodecAvailability ProbeAudioCodecs() {
    static const AudioCodecAvailability availability = []() {
        AudioCodecAvailability result;
        const OpusApi& api = Opus();
        result.opus = api.Ready();
        if (api.library != nullptr) {
            result.opus_library = "libopus.so.0";
        }
        if (api.version_string != nullptr) {
            result.opus_version = api.version_string();
        }
        if (result.opus) {
            result.detail = "libopus loaded: " + result.opus_version;
        } else {
            result.detail = "dlopen(libopus.so.0) failed or symbols missing";
        }
        return result;
    }();
    return availability;
}

AudioCodec PreferredAudioCodec() {
    return ProbeAudioCodecs().opus ? AudioCodec::kOpus : AudioCodec::kPcmu;
}

// ===========================================================================
// RTP 头 / 分片 / 重组
// ===========================================================================

void EncodeRtpHeader(const RtpHeader& header, std::uint8_t* out) noexcept {
    out[0] = static_cast<std::uint8_t>(RtpHeader::kVersion << 6);
    out[1] = static_cast<std::uint8_t>((header.marker ? 0x80U : 0U) | (header.payload_type & 0x7FU));
    PutBe16(out + 2, header.sequence);
    PutBe32(out + 4, header.timestamp);
    PutBe32(out + 8, header.ssrc);
}

bool DecodeRtpHeader(const std::uint8_t* data, std::size_t bytes, RtpHeader* out) noexcept {
    if (data == nullptr || out == nullptr || bytes < RtpHeader::kBytes) {
        return false;
    }
    const std::uint8_t version = static_cast<std::uint8_t>(data[0] >> 6);
    if (version != RtpHeader::kVersion) {
        return false;
    }
    // 只支持 P=0/X=0/CC=0：带扩展或 CSRC 的包无法在 12B 内定位负载，直接拒绝。
    if ((data[0] & 0x3FU) != 0) {
        return false;
    }
    out->payload_type = static_cast<std::uint8_t>(data[1] & 0x7FU);
    out->marker = (data[1] & 0x80U) != 0;
    out->sequence = GetBe16(data + 2);
    out->timestamp = GetBe32(data + 4);
    out->ssrc = GetBe32(data + 8);
    return true;
}

AudioRtpPacketizer::AudioRtpPacketizer(std::uint32_t ssrc, std::uint8_t payload_type,
                                       std::uint16_t initial_sequence, std::size_t max_payload)
    : m_ssrc(ssrc),
      m_payload_type(static_cast<std::uint8_t>(payload_type & 0x7FU)),
      m_sequence(initial_sequence),
      m_max_payload(std::max<std::size_t>(max_payload, kAudioFragmentHeaderBytes + 1)) {}

int AudioRtpPacketizer::Packetize(const std::uint8_t* payload, std::size_t bytes,
                                  std::uint32_t timestamp, std::vector<AudioRtpPacket>* out) {
    if (out == nullptr || bytes > kMaxAudioPayloadBytes) {
        return 0;
    }
    if (payload == nullptr && bytes > 0) {
        return 0;
    }
    const std::size_t chunk = m_max_payload - kAudioFragmentHeaderBytes;
    const std::size_t total = bytes == 0 ? 1 : (bytes + chunk - 1) / chunk;
    if (total > 0xFFFFU) {
        return 0;
    }
    for (std::size_t index = 0; index < total; ++index) {
        AudioRtpPacket packet;
        packet.header.ssrc = m_ssrc;
        packet.header.payload_type = m_payload_type;
        packet.header.timestamp = timestamp;
        packet.header.sequence = m_sequence;
        packet.header.marker = (index + 1 == total);
        PutBe16(packet.payload.data(), static_cast<std::uint16_t>(index));
        PutBe16(packet.payload.data() + 2, static_cast<std::uint16_t>(total));
        const std::size_t offset = index * chunk;
        const std::size_t length = std::min(chunk, bytes - offset);
        if (length > 0) {
            std::memcpy(packet.payload.data() + kAudioFragmentHeaderBytes, payload + offset, length);
        }
        packet.payload_bytes = static_cast<std::uint16_t>(kAudioFragmentHeaderBytes + length);
        out->push_back(packet);
        ++m_sequence;
        ++m_packets;
    }
    return static_cast<int>(total);
}

int AudioRtpPacketizer::PacketizeFrame(const AudioFrame& frame, std::vector<AudioRtpPacket>* out) {
    if (out == nullptr) {
        return 0;
    }
    return Packetize(frame.data.data(), frame.data.size(), frame.timestamp, out);
}

AudioRtpReassembler::AudioRtpReassembler() : AudioRtpReassembler(Config()) {}

AudioRtpReassembler::AudioRtpReassembler(const Config& config) : m_config(config) {
    if (m_config.max_pending == 0) {
        m_config.max_pending = 1;
    }
    if (m_config.max_fragments == 0) {
        m_config.max_fragments = 1;
    }
}

void AudioRtpReassembler::Push(const AudioRtpPacket& packet, std::uint64_t now_ms,
                               std::vector<ReassembledAudio>* complete) {
    if (complete == nullptr) {
        return;
    }
    if (packet.payload_bytes < kAudioFragmentHeaderBytes) {
        ++m_malformed;
        return;
    }
    const std::uint16_t index = GetBe16(packet.payload.data());
    const std::uint16_t total = GetBe16(packet.payload.data() + 2);
    if (total == 0 || index >= total || total > m_config.max_fragments) {
        ++m_malformed;
        return;
    }
    ++m_fragments;

    Pending* group = nullptr;
    for (Pending& candidate : m_pending) {
        if (candidate.timestamp == packet.header.timestamp &&
            candidate.ssrc == packet.header.ssrc) {
            group = &candidate;
            break;
        }
    }
    if (group == nullptr) {
        if (m_pending.size() >= m_config.max_pending) {
            DropOldest();
        }
        Pending fresh;
        fresh.timestamp = packet.header.timestamp;
        fresh.ssrc = packet.header.ssrc;
        fresh.total = total;
        fresh.first_sequence = packet.header.sequence;
        fresh.first_arrival_ms = now_ms;
        fresh.fragments.resize(total);
        fresh.present.assign(total, false);
        m_pending.push_back(std::move(fresh));
        group = &m_pending.back();
    } else if (group->total != total) {
        ++m_malformed;
        return;
    }
    if (group->present[index]) {
        return;
    }
    const std::uint8_t* body = packet.payload.data() + kAudioFragmentHeaderBytes;
    const std::size_t body_bytes = packet.payload_bytes - kAudioFragmentHeaderBytes;
    group->fragments[index].assign(body, body + body_bytes);
    group->present[index] = true;
    if (index == 0) {
        group->first_sequence = packet.header.sequence;
    }
    ++group->received;
    if (group->received < group->total || group->total == 0) {
        return;
    }

    ReassembledAudio assembled;
    if (!Finish(*group, &assembled)) {
        ++m_incomplete;
    } else {
        ++m_completed;
        complete->push_back(std::move(assembled));
    }
    const std::uint32_t timestamp = group->timestamp;
    const std::uint32_t ssrc = group->ssrc;
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [timestamp, ssrc](const Pending& item) {
                                       return item.timestamp == timestamp && item.ssrc == ssrc;
                                   }),
                    m_pending.end());
}

void AudioRtpReassembler::OnTimer(std::uint64_t now_ms) {
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->received < it->total && now_ms - it->first_arrival_ms > m_config.timeout_ms) {
            ++m_incomplete;
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}

bool AudioRtpReassembler::Finish(const Pending& pending, ReassembledAudio* out) const {
    std::size_t bytes = 0;
    for (const auto& fragment : pending.fragments) {
        bytes += fragment.size();
    }
    out->timestamp = pending.timestamp;
    out->ssrc = pending.ssrc;
    out->first_sequence = pending.first_sequence;
    out->first_arrival_ms = pending.first_arrival_ms;
    out->fragments = pending.total;
    out->payload.clear();
    out->payload.reserve(bytes);
    for (const auto& fragment : pending.fragments) {
        out->payload.insert(out->payload.end(), fragment.begin(), fragment.end());
    }
    return pending.received == pending.total;
}

void AudioRtpReassembler::DropOldest() {
    if (m_pending.empty()) {
        return;
    }
    auto oldest = m_pending.begin();
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (it->first_arrival_ms < oldest->first_arrival_ms) {
            oldest = it;
        }
    }
    if (oldest->received < oldest->total) {
        ++m_incomplete;
    }
    m_pending.erase(oldest);
}

// ===========================================================================
// 抖动缓冲
// ===========================================================================

AudioJitterBuffer::AudioJitterBuffer(const AudioJitterBufferConfig& config) : m_config(config) {
    m_config.packet_ms = std::max(1, m_config.packet_ms);
    m_config.min_delay_packets = std::max(1, m_config.min_delay_packets);
    m_config.max_delay_packets = std::max(m_config.min_delay_packets, m_config.max_delay_packets);
    m_config.target_delay_packets = std::clamp(m_config.target_delay_packets,
                                               m_config.min_delay_packets,
                                               m_config.max_delay_packets);
    m_target_packets = m_config.target_delay_packets;
}

bool AudioJitterBuffer::Push(const AudioRtpPacket& packet, std::uint64_t now_ms) {
    if (!packet.Valid()) {
        return false;
    }
    std::uint64_t extended = 0;
    if (!m_have_base) {
        m_have_base = true;
        m_last_sequence = packet.header.sequence;
        m_last_extended = packet.header.sequence;
        m_next_sequence = packet.header.sequence;
        extended = m_last_extended;
    } else {
        const std::uint16_t diff =
            static_cast<std::uint16_t>(packet.header.sequence - m_last_sequence);
        const std::int32_t signed_diff =
            diff < 0x8000U ? static_cast<std::int32_t>(diff)
                           : static_cast<std::int32_t>(diff) - 0x10000;
        extended = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(m_last_extended) + signed_diff);
        if (signed_diff > 0) {
            m_last_sequence = packet.header.sequence;
            m_last_extended = extended;
        }
    }

    if (extended < m_next_sequence) {
        ++m_late;
        return false;
    }
    if (extended > m_next_sequence + static_cast<std::uint64_t>(m_config.max_reorder_distance)) {
        ++m_late;
        return false;
    }
    for (const auto& entry : m_queue) {
        if (entry.first == extended) {
            ++m_duplicates;
            return false;
        }
    }
    if (m_queue.size() >= m_config.max_packets) {
        m_queue.erase(m_queue.begin());
        ++m_lost;
    }
    Entry entry;
    entry.packet = packet;
    entry.arrival_ms = now_ms;
    const auto position = std::upper_bound(
        m_queue.begin(), m_queue.end(), extended,
        [](std::uint64_t value, const std::pair<std::uint64_t, Entry>& item) {
            return value < item.first;
        });
    m_queue.insert(position, {extended, entry});
    ++m_received;

    // 到达间隔抖动（EWMA，RFC 3550 的简化：只保留一阶偏差）。
    // 乱序到达时 now 可能小于上一包到达时刻，因此必须用有符号差，否则
    // uint64 下溢会把抖动量抬到 1e19 量级（自适应深度随之溢出）。
    if (m_have_arrival) {
        const std::int64_t delta = static_cast<std::int64_t>(now_ms) -
                                   static_cast<std::int64_t>(m_last_arrival_ms);
        const double raw = std::fabs(static_cast<double>(delta) -
                                     static_cast<double>(m_config.packet_ms));
        const double deviation = std::min(raw, static_cast<double>(m_config.packet_ms) * 10.0);
        m_jitter_ms += (deviation - m_jitter_ms) / 16.0;
    }
    m_last_arrival_ms = now_ms;
    m_have_arrival = true;
    if (m_config.adaptive && (m_received % 8U) == 0U) {
        UpdateTarget(now_ms);
    }
    return true;
}

JitterPopResult AudioJitterBuffer::Pop(AudioRtpPacket* out, std::uint64_t now_ms) {
    if (!m_have_base || m_queue.empty()) {
        return JitterPopResult::kEmpty;
    }
    while (!m_queue.empty() && m_queue.front().first < m_next_sequence) {
        m_queue.erase(m_queue.begin());
    }
    if (m_queue.empty()) {
        return JitterPopResult::kEmpty;
    }
    const auto& front = m_queue.front();
    const double target_ms = static_cast<double>(m_target_packets) *
                             static_cast<double>(m_config.packet_ms);
    const double waited = static_cast<double>(now_ms - front.second.arrival_ms);
    if (front.first > m_next_sequence) {
        // 缺包：等队首已排队 target_ms 仍未补上，才判丢；抖动窗口内先等。
        if (waited < target_ms) {
            return JitterPopResult::kEmpty;
        }
        ++m_lost;
        ++m_next_sequence;
        return JitterPopResult::kLost;
    }
    const bool buffered_enough = m_queue.size() >= static_cast<std::size_t>(m_target_packets);
    if (!buffered_enough && waited < target_ms) {
        return JitterPopResult::kEmpty;
    }
    if (out != nullptr) {
        *out = front.second.packet;
    }
    m_queue_delay.Add(waited);
    m_queue.erase(m_queue.begin());
    ++m_delivered;
    ++m_next_sequence;
    return JitterPopResult::kPacket;
}

void AudioJitterBuffer::OnTimer(std::uint64_t now_ms) {
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        const auto age = now_ms - it->second.arrival_ms;
        if (age > m_config.max_wait_ms) {
            if (it->first == m_next_sequence) {
                ++m_next_sequence;
            }
            ++m_lost;
            it = m_queue.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioJitterBuffer::UpdateTarget(std::uint64_t now_ms) {
    (void)now_ms;
    const double packet_ms = static_cast<double>(m_config.packet_ms);
    int desired = 1 + static_cast<int>(std::ceil(m_jitter_ms / packet_ms));
    desired = std::clamp(desired, m_config.min_delay_packets, m_config.max_delay_packets);
    // 慢升快降：升一次 +1，降一次 -1，避免抖动突刺导致深度来回跳。
    if (desired > m_target_packets) {
        ++m_target_packets;
    } else if (desired < m_target_packets) {
        --m_target_packets;
    }
    m_target_packets =
        std::clamp(m_target_packets, m_config.min_delay_packets, m_config.max_delay_packets);
}

// ===========================================================================
// UDP 收发
// ===========================================================================

struct UdpAudioSender::Impl {
    explicit Impl(const AudioRtpPacketizer& source) : packetizer(source) {}

    AudioRtpPacketizer packetizer;
    UdpEndpoint endpoint;
    std::vector<AudioRtpPacket> packets;
    std::array<std::uint8_t, 1200> datagram{};
    std::uint64_t datagrams = 0;
    std::uint64_t failures = 0;
    std::uint64_t bytes = 0;
};

UdpAudioSender::UdpAudioSender(const AudioRtpPacketizer& packetizer)
    : m_impl(std::make_unique<Impl>(packetizer)) {}

UdpAudioSender::~UdpAudioSender() = default;

bool UdpAudioSender::Open(std::uint16_t peer_port, std::string* error) {
    if (!m_impl->endpoint.Open(error)) {
        return false;
    }
    m_impl->endpoint.SetPeerPort(peer_port);
    return true;
}

std::uint16_t UdpAudioSender::local_port() const noexcept {
    return m_impl->endpoint.port();
}

bool UdpAudioSender::SendPayload(const std::uint8_t* payload, std::size_t bytes,
                                 std::uint32_t timestamp) {
    m_impl->packets.clear();
    const int count = m_impl->packetizer.Packetize(payload, bytes, timestamp, &m_impl->packets);
    if (count <= 0) {
        return false;
    }
    bool ok = true;
    for (const AudioRtpPacket& packet : m_impl->packets) {
        EncodeRtpHeader(packet.header, m_impl->datagram.data());
        std::memcpy(m_impl->datagram.data() + RtpHeader::kBytes, packet.payload.data(),
                    packet.payload_bytes);
        const std::size_t total = RtpHeader::kBytes + packet.payload_bytes;
        if (!m_impl->endpoint.Send(m_impl->datagram.data(), total)) {
            ++m_impl->failures;
            ok = false;
            continue;
        }
        ++m_impl->datagrams;
        m_impl->bytes += total;
    }
    return ok;
}

bool UdpAudioSender::SendFrame(const AudioFrame& encoded) {
    return SendPayload(encoded.data.data(), encoded.data.size(), encoded.timestamp);
}

std::uint64_t UdpAudioSender::datagrams() const noexcept {
    return m_impl->datagrams;
}

std::uint64_t UdpAudioSender::failures() const noexcept {
    return m_impl->failures;
}

std::uint64_t UdpAudioSender::bytes() const noexcept {
    return m_impl->bytes;
}

AudioRtpPacketizer* UdpAudioSender::packetizer() noexcept {
    return &m_impl->packetizer;
}

struct UdpAudioReceiver::Impl {
    Impl(PacketFn callback, void* context) : callback(callback), context(context) {}

    PacketFn callback = nullptr;
    void* context = nullptr;
    UdpEndpoint endpoint;
    std::array<std::uint8_t, 2048> buffer{};
    std::uint64_t datagrams = 0;
    std::uint64_t malformed = 0;
};

UdpAudioReceiver::UdpAudioReceiver(PacketFn callback, void* context)
    : m_impl(std::make_unique<Impl>(callback, context)) {}

UdpAudioReceiver::~UdpAudioReceiver() = default;

bool UdpAudioReceiver::Open(std::string* error) {
    return m_impl->endpoint.Open(error);
}

std::uint16_t UdpAudioReceiver::local_port() const noexcept {
    return m_impl->endpoint.port();
}

int UdpAudioReceiver::Pump(int timeout_ms, int max_packets) {
    int delivered = 0;
    for (int i = 0; i < max_packets; ++i) {
        const int received = m_impl->endpoint.Recv(m_impl->buffer.data(), m_impl->buffer.size(),
                                                   i == 0 ? timeout_ms : 0);
        if (received <= 0) {
            break;
        }
        ++m_impl->datagrams;
        AudioRtpPacket packet;
        if (!DecodeRtpHeader(m_impl->buffer.data(), static_cast<std::size_t>(received),
                             &packet.header)) {
            ++m_impl->malformed;
            continue;
        }
        const std::size_t payload_bytes =
            static_cast<std::size_t>(received) - RtpHeader::kBytes;
        if (payload_bytes == 0 || payload_bytes > packet.payload.size()) {
            ++m_impl->malformed;
            continue;
        }
        std::memcpy(packet.payload.data(), m_impl->buffer.data() + RtpHeader::kBytes, payload_bytes);
        packet.payload_bytes = static_cast<std::uint16_t>(payload_bytes);
        if (m_impl->callback != nullptr) {
            m_impl->callback(m_impl->context, packet, NowMs());
        }
        ++delivered;
    }
    return delivered;
}

std::uint64_t UdpAudioReceiver::datagrams() const noexcept {
    return m_impl->datagrams;
}

std::uint64_t UdpAudioReceiver::malformed() const noexcept {
    return m_impl->malformed;
}

// ===========================================================================
// 进程内链路
// ===========================================================================

InProcessAudioLink::InProcessAudioLink(std::uint32_t ssrc, std::uint16_t initial_sequence,
                                       std::size_t max_payload)
    : m_packetizer(ssrc, 111, initial_sequence, max_payload) {}

InProcessAudioLink::~InProcessAudioLink() = default;

void InProcessAudioLink::Send(const std::uint8_t* payload, std::size_t bytes,
                              std::uint32_t timestamp, std::uint64_t now_ms) {
    std::vector<AudioRtpPacket> packets;
    const int count = m_packetizer.Packetize(payload, bytes, timestamp, &packets);
    for (int i = 0; i < count; ++i) {
        ++m_sent;
        if (m_drop_every > 1 && (m_sent % m_drop_every) == 0) {
            ++m_dropped;
            continue;
        }
        Queued item;
        item.due_ms = now_ms;
        if (m_max_jitter_ms > 0) {
            item.due_ms += NextRandom() % static_cast<std::uint32_t>(m_max_jitter_ms + 1);
        }
        item.packet = packets[static_cast<std::size_t>(i)];
        m_queue.push_back(std::move(item));
    }
    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const Queued& left, const Queued& right) {
                         return left.due_ms < right.due_ms;
                     });
}

void InProcessAudioLink::SetJitter(int max_jitter_ms, std::uint32_t seed) noexcept {
    m_max_jitter_ms = std::max(0, max_jitter_ms);
    m_rng = seed == 0 ? 0x9E3779B9U : seed;
}

void InProcessAudioLink::SetDropEvery(std::uint32_t drop_every) noexcept {
    m_drop_every = drop_every;
}

void InProcessAudioLink::SetDeliver(DeliverFn callback, void* context) noexcept {
    m_deliver = callback;
    m_context = context;
}

int InProcessAudioLink::PumpDue(std::uint64_t now_ms) {
    int delivered = 0;
    while (!m_queue.empty() && m_queue.front().due_ms <= now_ms) {
        if (m_deliver != nullptr) {
            m_deliver(m_context, m_queue.front().packet, m_queue.front().due_ms);
        }
        m_queue.erase(m_queue.begin());
        ++m_delivered;
        ++delivered;
    }
    return delivered;
}

std::size_t InProcessAudioLink::pending() const noexcept {
    return m_queue.size();
}

std::uint32_t InProcessAudioLink::NextRandom() noexcept {
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return m_rng;
}

// ===========================================================================
// 播放
// ===========================================================================

namespace {

/// @brief CLI 播放后端（Pulse 的 `pacat` / ALSA 的 `aplay` 共用）。
class ProcessAudioSink final : public AudioSink {
   public:
    ProcessAudioSink(const AudioSinkConfig& config, bool pulse)
        : m_config(config), m_pulse(pulse) {}

    bool Open(std::string* error) override {
        if (!m_config.format.Valid()) {
            return SetError(error, "invalid audio format");
        }
        std::vector<std::string> argv;
        if (m_pulse) {
            if (!PulsePlaybackAvailable()) {
                return SetError(error, "PulseAudio playback unavailable");
            }
            argv = {"pacat",
                    "--playback",
                    "--format=s16le",
                    "--rate=" + std::to_string(m_config.format.sample_rate),
                    "--channels=" + std::to_string(m_config.format.channels),
                    "--raw",
                    "--latency-msec=" + std::to_string(m_config.latency_ms)};
            if (!m_config.device.empty()) {
                argv.push_back("--device=" + m_config.device);
            }
        } else {
            if (!AlsaPlaybackAvailable()) {
                return SetError(error, "aplay unavailable (alsa-utils not installed)");
            }
            argv = {"aplay",
                    "-q",
                    "-t",
                    "raw",
                    "-f",
                    "S16_LE",
                    "-r",
                    std::to_string(m_config.format.sample_rate),
                    "-c",
                    std::to_string(m_config.format.channels),
                    "--buffer-time=" + std::to_string(m_config.latency_ms * 1000)};
            if (!m_config.device.empty()) {
                argv.push_back("-D");
                argv.push_back(m_config.device);
            }
        }
        return m_process.SpawnWrite(argv, error);
    }

    bool Write(const AudioFrame& frame) override {
        if (!frame.Valid()) {
            return false;
        }
        if (!m_process.WriteAll(frame.data.data(), frame.data.size(), 1000)) {
            return false;
        }
        ++m_frames;
        m_bytes += frame.data.size();
        return true;
    }

    void Close() override {
        m_process.Close();
    }

    const char* name() const noexcept override {
        return m_pulse ? "pulse:pacat" : "alsa:aplay";
    }

    bool available() const noexcept override {
        return m_process.alive();
    }

    std::uint64_t written_frames() const noexcept override {
        return m_frames;
    }

    std::uint64_t written_bytes() const noexcept override {
        return m_bytes;
    }

   private:
    AudioSinkConfig m_config;
    bool m_pulse;
    PipeProcess m_process;
    std::uint64_t m_frames = 0;
    std::uint64_t m_bytes = 0;
};

/// @brief 空播放后端：只统计，不碰音频设备。
class NullAudioSink final : public AudioSink {
   public:
    explicit NullAudioSink(const AudioFormat& format) : m_format(format) {}

    bool Open(std::string* error) override {
        (void)error;
        return true;
    }

    bool Write(const AudioFrame& frame) override {
        if (!frame.Valid()) {
            return false;
        }
        ++m_frames;
        m_bytes += frame.data.size();
        return true;
    }

    void Close() override {}

    const char* name() const noexcept override {
        return "null";
    }

    bool available() const noexcept override {
        return true;
    }

    std::uint64_t written_frames() const noexcept override {
        return m_frames;
    }

    std::uint64_t written_bytes() const noexcept override {
        return m_bytes;
    }

   private:
    AudioFormat m_format;
    std::uint64_t m_frames = 0;
    std::uint64_t m_bytes = 0;
};

}  // namespace

std::unique_ptr<AudioSink> CreatePulseAudioSink(const AudioSinkConfig& config,
                                                std::string* error) {
    auto sink = std::make_unique<ProcessAudioSink>(config, true);
    if (!sink->Open(error)) {
        return nullptr;
    }
    return sink;
}

std::unique_ptr<AudioSink> CreateAlsaAudioSink(const AudioSinkConfig& config, std::string* error) {
    auto sink = std::make_unique<ProcessAudioSink>(config, false);
    if (!sink->Open(error)) {
        return nullptr;
    }
    return sink;
}

std::unique_ptr<AudioSink> CreateNullAudioSink(const AudioFormat& format) {
    if (!format.Valid()) {
        return nullptr;
    }
    return std::make_unique<NullAudioSink>(format);
}

std::unique_ptr<AudioSink> CreateAudioSink(const AudioSinkConfig& config, std::string* error) {
    std::string reason;
    if (std::unique_ptr<AudioSink> sink = CreatePulseAudioSink(config, &reason)) {
        return sink;
    }
    if (std::unique_ptr<AudioSink> sink = CreateAlsaAudioSink(config, &reason)) {
        return sink;
    }
    if (error != nullptr) {
        *error = "audio playback degraded to null: " + reason;
    }
    return CreateNullAudioSink(config.format);
}

// ===========================================================================
// 端到端会话
// ===========================================================================

struct AudioSession::Impl {
    AudioSessionConfig config;
    std::unique_ptr<AudioSource> source;
    std::unique_ptr<AudioEncoder> encoder;
    std::unique_ptr<AudioRtpPacketizer> packetizer;
    std::unique_ptr<UdpAudioSender> udp_sender;
    std::unique_ptr<UdpAudioReceiver> udp_receiver;
    std::unique_ptr<InProcessAudioLink> link;
    std::unique_ptr<AudioJitterBuffer> jitter;
    std::unique_ptr<AudioRtpReassembler> reassembler;
    std::unique_ptr<AudioDecoder> decoder;
    std::unique_ptr<AudioSink> sink;

    AudioSessionStats* stats = nullptr;
    AudioFrame frame;
    std::vector<std::uint8_t> payload;
    std::vector<AudioRtpPacket> packets;
    std::vector<ReassembledAudio> complete;
    std::map<std::uint32_t, std::int64_t> capture_times;
    std::uint32_t next_timestamp = 0;
    std::uint32_t next_sequence = 0;
    std::uint64_t last_sent_packets = 0;
    std::uint64_t last_link_sent = 0;
    double rms_sum = 0.0;
    std::uint64_t rms_count = 0;

    static void OnUdpPacket(void* context, const AudioRtpPacket& packet,
                            std::uint64_t arrival_ms) {
        auto* impl = static_cast<Impl*>(context);
        if (impl->stats != nullptr) {
            ++impl->stats->recv_packets;
        }
        impl->jitter->Push(packet, arrival_ms);
    }

    static void OnLinkPacket(void* context, const AudioRtpPacket& packet,
                             std::uint64_t arrival_ms) {
        auto* impl = static_cast<Impl*>(context);
        if (impl->stats != nullptr) {
            ++impl->stats->recv_packets;
        }
        impl->jitter->Push(packet, arrival_ms);
    }
};

AudioSession::AudioSession(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

AudioSession::~AudioSession() {
    if (m_impl && m_impl->sink) {
        m_impl->sink->Close();
    }
}

std::unique_ptr<AudioSession> AudioSession::Create(const AudioSessionConfig& config,
                                                   std::string* error) {
    if (!config.format.Valid()) {
        SetError(error, "invalid audio format");
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->config = config;

    AudioEncoderConfig encoder_config;
    encoder_config.codec = config.codec;
    encoder_config.format = config.format;
    encoder_config.bitrate = config.bitrate;
    impl->encoder = CreateAudioEncoder(encoder_config, error);
    if (!impl->encoder) {
        return nullptr;
    }

    AudioCaptureConfig capture = config.capture;
    capture.format = config.format;
    if (config.real_capture) {
        impl->source = CreateAudioSource(capture, error);
    } else {
        impl->source = CreateSyntheticAudioSource(capture, error);
        if (impl->source == nullptr) {
            return nullptr;
        }
    }
    if (!impl->source) {
        SetError(error, "failed to create audio source");
        return nullptr;
    }

    AudioSinkConfig sink_config = config.sink;
    sink_config.format = config.format;
    if (config.real_playback) {
        impl->sink = CreateAudioSink(sink_config, error);
    } else {
        impl->sink = CreateNullAudioSink(config.format);
    }
    if (!impl->sink) {
        SetError(error, "failed to create audio sink");
        return nullptr;
    }

    impl->packetizer = std::make_unique<AudioRtpPacketizer>(
        config.ssrc, DefaultAudioPayloadType(config.codec));
    impl->jitter = std::make_unique<AudioJitterBuffer>(config.jitter);
    impl->reassembler = std::make_unique<AudioRtpReassembler>();
    impl->decoder = CreateAudioDecoder(config.codec, config.format, error);
    if (!impl->decoder) {
        return nullptr;
    }

    if (config.udp) {
        impl->udp_receiver = std::make_unique<UdpAudioReceiver>(&Impl::OnUdpPacket, impl.get());
        if (!impl->udp_receiver->Open(error)) {
            return nullptr;
        }
        impl->udp_sender = std::make_unique<UdpAudioSender>(*impl->packetizer);
        if (!impl->udp_sender->Open(impl->udp_receiver->local_port(), error)) {
            return nullptr;
        }
    } else {
        impl->link = std::make_unique<InProcessAudioLink>(config.ssrc);
        impl->link->SetJitter(config.max_jitter_ms, config.jitter_seed);
        impl->link->SetDropEvery(config.drop_every);
        impl->link->SetDeliver(&Impl::OnLinkPacket, impl.get());
    }

    auto session = std::unique_ptr<AudioSession>(new AudioSession(std::move(impl)));
    session->m_impl->stats = &session->m_stats;
    return session;
}

bool AudioSession::SendNext(AudioFrame* captured_pcm) {
    AudioFrame& frame = m_impl->frame;
    if (!m_impl->source->Read(&frame)) {
        return false;
    }
    frame.ssrc = m_impl->config.ssrc;
    frame.timestamp = m_impl->next_timestamp;
    m_impl->next_timestamp +=
        static_cast<std::uint32_t>(frame.format.EffectiveFramesPerPacket());
    ++m_stats.captured_frames;
    m_stats.bytes_captured += frame.data.size();
    if (captured_pcm != nullptr) {
        *captured_pcm = frame;
    }
    m_impl->capture_times[frame.timestamp] = frame.capture_ns;
    if (m_impl->capture_times.size() > 256) {
        m_impl->capture_times.erase(m_impl->capture_times.begin());
    }

    if (!m_impl->encoder->Encode(frame, &m_impl->payload)) {
        return false;
    }
    ++m_stats.encoded_frames;
    m_stats.bytes_encoded += m_impl->payload.size();

    if (m_impl->config.udp) {
        if (!m_impl->udp_sender->SendPayload(m_impl->payload.data(), m_impl->payload.size(),
                                             frame.timestamp)) {
            return false;
        }
        const std::uint64_t total = m_impl->udp_sender->packetizer()->packets();
        m_stats.sent_packets += total - m_impl->last_sent_packets;
        m_impl->last_sent_packets = total;
    } else {
        m_impl->link->Send(m_impl->payload.data(), m_impl->payload.size(), frame.timestamp, NowMs());
        const std::uint64_t total = m_impl->link->sent();
        m_stats.sent_packets += total - m_impl->last_link_sent;
        m_impl->last_link_sent = total;
    }
    ++m_impl->next_sequence;
    return true;
}

int AudioSession::Pump() {
    const std::uint64_t now = NowMs();
    if (m_impl->config.udp) {
        m_impl->udp_receiver->Pump(0, 64);
    } else {
        m_impl->link->PumpDue(now);
    }
    int played = 0;
    for (int i = 0; i < 64; ++i) {
        AudioRtpPacket packet;
        const JitterPopResult result = m_impl->jitter->Pop(&packet, now);
        if (result == JitterPopResult::kEmpty) {
            break;
        }
        if (result == JitterPopResult::kLost) {
            ++m_stats.dropped_packets;
            AudioFrame frame;
            if (!m_impl->decoder->Decode(nullptr, 0, &frame)) {
                continue;
            }
            ++m_stats.concealed;
            if (m_impl->sink->Write(frame)) {
                ++m_stats.played_frames;
                ++played;
                m_impl->rms_sum += frame.Rms();
                ++m_impl->rms_count;
            }
            continue;
        }
        m_impl->complete.clear();
        m_impl->reassembler->Push(packet, now, &m_impl->complete);
        for (const ReassembledAudio& group : m_impl->complete) {
            ++m_stats.reassembled;
            AudioFrame frame;
            if (!m_impl->decoder->Decode(group.payload.data(), group.payload.size(), &frame)) {
                continue;
            }
            frame.timestamp = group.timestamp;
            ++m_stats.decoded_frames;
            const auto captured = m_impl->capture_times.find(group.timestamp);
            if (captured != m_impl->capture_times.end()) {
                if (captured->second > 0) {
                    const double latency_ms =
                        static_cast<double>(now) - static_cast<double>(captured->second) / 1e6;
                    if (latency_ms >= 0.0) {
                        m_stats.latency.Add(latency_ms);
                    }
                }
                m_impl->capture_times.erase(captured);
            }
            if (!m_impl->sink->Write(frame)) {
                continue;
            }
            ++m_stats.played_frames;
            ++played;
            m_impl->rms_sum += frame.Rms();
            ++m_impl->rms_count;
        }
    }
    if (m_impl->rms_count > 0) {
        m_stats.output_rms = m_impl->rms_sum / static_cast<double>(m_impl->rms_count);
    }
    return played;
}

int AudioSession::Run(int frames) {
    int played = 0;
    const int packet_ms =
        std::max(1, m_impl->config.format.PacketDurationUs() / 1000);
    for (int i = 0; i < frames; ++i) {
        if (!SendNext(nullptr)) {
            break;
        }
        played += Pump();
        const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(packet_ms);
        while (NowMs() < deadline) {
            played += Pump();
            ::usleep(500);
        }
    }
    // 冲刷：抖动缓冲最深 12 包 × 10ms + 20ms 抖动，留 100ms 余量。
    for (int i = 0; i < 100; ++i) {
        played += Pump();
        ::usleep(1000);
    }
    return played;
}

const char* AudioSession::encoder_name() const noexcept {
    return m_impl->encoder->name();
}

const char* AudioSession::capture_name() const noexcept {
    return m_impl->source->name();
}

const char* AudioSession::sink_name() const noexcept {
    return m_impl->sink->name();
}

int AudioSession::jitter_target_packets() const noexcept {
    return m_impl->jitter->target_delay_packets();
}

double AudioSession::jitter_ms() const noexcept {
    return m_impl->jitter->jitter_ms();
}

}  // namespace go2cpp::media
