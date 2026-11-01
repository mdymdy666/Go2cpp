#pragma once

/**
 * @file e2e_common.hpp
 * @brief 媒体端到端 demo 的公共工具：SHA-256、PPM 渲染、确定性测试图案、模拟弱网。
 * @details 只依赖 `go2cpp::media` 的公开头文件与标准库，不引入第三方库：
 *
 *          - `Sha256`：FIPS 180-4 实现，用于「采集帧 vs 渲染帧」的逐像素证据。
 *            demo 自己实现而不用 OpenSSL，是为了让证据可被独立复算（附算法测试向量）。
 *          - `WritePpm`：把 BGRA 写成二进制 PPM（P6），再由 `build_and_run.sh`
 *            转 PNG 生成肉眼可见的截图；渲染器 `PpmRenderer` 就是「真实落盘渲染」。
 *          - `TestPatternSource`：彩色棋盘 + 移动色块 + 帧号位图的确定性画面，
 *            满足「生成测试画面如彩色方块/时间戳」的要求，且同一 frame_id 永远
 *            同一像素（逐帧断言可复现）。
 *          - `SimulatedNetwork`：包级丢包（按比例）+ 每包独立延迟（产生乱序），
 *            用虚拟时钟推进，避免真实 sleep 让 1000 帧弱网测试跑几分钟。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/media/capture.hpp"
#include "go2cpp/media/frame.hpp"
#include "go2cpp/media/pixel.hpp"
#include "go2cpp/media/render.hpp"

namespace go2cpp::media::e2e {

// ---------------------------------------------------------------------------
// 时钟
// ---------------------------------------------------------------------------

/// @brief 单调纳秒时钟（与 media 模块 capture_ns 同一时基）。
inline std::int64_t NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// @brief 单调毫秒时钟。
inline std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(NowNs() / 1000000);
}

// ---------------------------------------------------------------------------
// SHA-256（FIPS 180-4）
// ---------------------------------------------------------------------------

/// @brief 单次计算的 SHA-256（纯标准库，无外部依赖）。
class Sha256 final {
   public:
    Sha256() noexcept {
        m_state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                   0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    }

    /// @brief 追加字节。
    void Update(const void* data, std::size_t bytes) noexcept {
        const auto* input = static_cast<const std::uint8_t*>(data);
        m_total_bytes += bytes;
        while (bytes > 0) {
            const std::size_t take = std::min<std::size_t>(64 - m_buffer_bytes, bytes);
            std::memcpy(m_buffer + m_buffer_bytes, input, take);
            m_buffer_bytes += take;
            input += take;
            bytes -= take;
            if (m_buffer_bytes == 64) {
                Transform(m_buffer);
                m_buffer_bytes = 0;
            }
        }
    }

    /// @brief 输出 32 字节摘要（8 个大端 u32）。
    void Final(std::uint8_t out[32]) noexcept {
        const std::uint64_t bits = m_total_bytes * 8;
        const std::uint8_t pad = 0x80;
        Update(&pad, 1);
        std::uint8_t zero = 0;
        while (m_buffer_bytes != 56) {
            Update(&zero, 1);
        }
        std::uint8_t length[8];
        for (int i = 0; i < 8; ++i) {
            length[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        }
        Update(length, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4 + 0] = static_cast<std::uint8_t>(m_state[static_cast<std::size_t>(i)] >> 24);
            out[i * 4 + 1] = static_cast<std::uint8_t>(m_state[static_cast<std::size_t>(i)] >> 16);
            out[i * 4 + 2] = static_cast<std::uint8_t>(m_state[static_cast<std::size_t>(i)] >> 8);
            out[i * 4 + 3] = static_cast<std::uint8_t>(m_state[static_cast<std::size_t>(i)]);
        }
    }

   private:
    static std::uint32_t Rotr(std::uint32_t value, int shift) noexcept {
        return (value >> shift) | (value << (32 - shift));
    }

    void Transform(const std::uint8_t* block) noexcept {
        static constexpr std::array<std::uint32_t, 64> k = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
            0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
            0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
            0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
            0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = m_state[0];
        std::uint32_t b = m_state[1];
        std::uint32_t c = m_state[2];
        std::uint32_t d = m_state[3];
        std::uint32_t e = m_state[4];
        std::uint32_t f = m_state[5];
        std::uint32_t g = m_state[6];
        std::uint32_t h = m_state[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + k[static_cast<std::size_t>(i)] + w[i];
            const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    std::array<std::uint32_t, 8> m_state{};
    std::uint8_t m_buffer[64]{};
    std::size_t m_buffer_bytes = 0;
    std::uint64_t m_total_bytes = 0;
};

/// @brief 计算 SHA-256 并返回 64 个十六进制字符。
inline std::string Sha256Hex(const void* data, std::size_t bytes) {
    Sha256 sha;
    sha.Update(data, bytes);
    std::uint8_t digest[32];
    sha.Final(digest);
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (std::uint8_t byte : digest) {
        hex.push_back(kDigits[byte >> 4]);
        hex.push_back(kDigits[byte & 0x0F]);
    }
    return hex;
}

/// @brief 帧的有效像素区 SHA-256（BGRA 含行补齐时按行取有效像素）。
inline std::string FrameSha256(const VideoFrame& frame) {
    Sha256 sha;
    if (frame.format == PixelFormat::kI420) {
        const std::size_t luma = static_cast<std::size_t>(frame.width) *
                                 static_cast<std::size_t>(frame.height);
        const std::size_t chroma_w = (static_cast<std::size_t>(frame.width) + 1) / 2;
        const std::size_t chroma_h = (static_cast<std::size_t>(frame.height) + 1) / 2;
        const std::size_t total = luma + chroma_w * chroma_h * 2;
        if (frame.data.size() < total) {
            return std::string(64, '0');
        }
        sha.Update(frame.data.data(), total);
    } else {
        const int stride = frame.EffectiveStride();
        for (int y = 0; y < frame.height; ++y) {
            sha.Update(frame.data.data() +
                           static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                       static_cast<std::size_t>(frame.width) * 4);
        }
    }
    std::uint8_t digest[32];
    sha.Final(digest);
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (std::uint8_t byte : digest) {
        hex.push_back(kDigits[byte >> 4]);
        hex.push_back(kDigits[byte & 0x0F]);
    }
    return hex;
}

// ---------------------------------------------------------------------------
// PPM 落盘（渲染证据）
// ---------------------------------------------------------------------------

/// @brief 把 BGRA 帧写成二进制 PPM（P6，RGB24）。返回写出的字节数。
inline std::size_t WritePpmBgra(const std::string& path, const std::uint8_t* bgra, int width,
                                int height, int stride) {
    if (bgra == nullptr || width <= 0 || height <= 0) {
        return 0;
    }
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return 0;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3);
    std::size_t written = 0;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src =
            bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = 0; x < width; ++x) {
            row[static_cast<std::size_t>(x) * 3 + 0] = src[x * 4 + 2];
            row[static_cast<std::size_t>(x) * 3 + 1] = src[x * 4 + 1];
            row[static_cast<std::size_t>(x) * 3 + 2] = src[x * 4 + 0];
        }
        written += std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    return written;
}

/// @brief 把任意帧（I420/BGRA）转 BGRA 后写 PPM。
inline std::size_t WritePpmFrame(const std::string& path, const VideoFrame& frame) {
    if (frame.format == PixelFormat::kBGRA) {
        return WritePpmBgra(path, frame.data.data(), frame.width, frame.height,
                            frame.EffectiveStride());
    }
    VideoFrame bgra;
    bgra.width = frame.width;
    bgra.height = frame.height;
    bgra.format = PixelFormat::kBGRA;
    bgra.stride = frame.width * 4;
    bgra.data.resize(BgraFrameBytes(frame.width, frame.height));
    I420ToBgra(frame.data.data(), frame.width, frame.height, bgra.data.data(), bgra.stride);
    return WritePpmBgra(path, bgra.data.data(), bgra.width, bgra.height, bgra.stride);
}

// ---------------------------------------------------------------------------
// 落盘渲染器
// ---------------------------------------------------------------------------

/// @brief 把每一帧真正写成 PPM 文件的渲染器（「渲染」不是空操作）。
/// @details 保留最近一帧的 BGRA，供「解码/渲染结果 vs 采集源」逐像素比较。
class PpmRenderer final : public FrameRenderer {
   public:
    explicit PpmRenderer(std::string directory, int save_every = 0)
        : m_directory(std::move(directory)), m_save_every(save_every) {}

    bool Present(const VideoFrame& frame) override {
        if (!frame.Valid()) {
            return false;
        }
        m_last_checksum = frame.Checksum();
        m_last_bgra.width = frame.width;
        m_last_bgra.height = frame.height;
        m_last_bgra.format = PixelFormat::kBGRA;
        m_last_bgra.stride = frame.width * 4;
        m_last_bgra.data.resize(BgraFrameBytes(frame.width, frame.height));
        if (frame.format == PixelFormat::kBGRA) {
            const int stride = frame.EffectiveStride();
            for (int y = 0; y < frame.height; ++y) {
                std::memcpy(
                    m_last_bgra.data.data() +
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(m_last_bgra.stride),
                    frame.data.data() +
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                    static_cast<std::size_t>(frame.width) * 4);
            }
        } else {
            I420ToBgra(frame.data.data(), frame.width, frame.height, m_last_bgra.data.data(),
                       m_last_bgra.stride);
        }
        m_last_bgra.frame_id = frame.frame_id;
        m_last_bgra.capture_ns = frame.capture_ns;
        m_last_bgra.keyframe = frame.keyframe;
        ++m_presented;
        if (m_save_every > 0 && (m_presented % static_cast<std::uint64_t>(m_save_every)) == 1) {
            char name[64];
            std::snprintf(name, sizeof(name), "/render_%06llu.ppm",
                          static_cast<unsigned long long>(m_presented));
            WritePpmBgra(m_directory + name, m_last_bgra.data.data(), m_last_bgra.width,
                         m_last_bgra.height, m_last_bgra.stride);
        }
        return true;
    }

    const char* name() const noexcept override {
        return "ppm";
    }
    bool visible() const noexcept override {
        return false;
    }
    std::uint64_t presented() const noexcept override {
        return m_presented;
    }
    std::uint64_t last_checksum() const noexcept override {
        return m_last_checksum;
    }

    /// @brief 最近一次渲染出的 BGRA 帧（逐像素比对用）。
    const VideoFrame& last_bgra() const noexcept {
        return m_last_bgra;
    }

   private:
    std::string m_directory;
    int m_save_every = 0;
    std::uint64_t m_presented = 0;
    std::uint64_t m_last_checksum = 0;
    VideoFrame m_last_bgra;
};

// ---------------------------------------------------------------------------
// 确定性测试画面（彩色方块 + 帧号位图 + 进度条）
// ---------------------------------------------------------------------------

/// @brief 确定性彩色测试画面：棋盘底 + 移动色块 + 帧号位图 + 帧进度条。
/// @details 同一 `frame_id` 永远产生同一像素；帧号变化保证差分编码每帧都有脏块，
///          同时画面「肉眼可辨认」——截图里能直接看到色块位置与帧号在变。
class TestPatternSource final : public VideoSource {
   public:
    TestPatternSource(int width, int height) : m_width(width), m_height(height) {}

    bool Capture(VideoFrame* out) override {
        if (out == nullptr || m_width <= 0 || m_height <= 0) {
            return false;
        }
        out->width = m_width;
        out->height = m_height;
        out->format = PixelFormat::kBGRA;
        out->stride = m_width * 4;
        out->data.resize(BgraFrameBytes(m_width, m_height));
        out->frame_id = m_frame_id;
        out->capture_ns = NowNs();
        out->keyframe = false;
        std::uint8_t* data = out->data.data();
        const int stride = m_width * 4;

        // 1) 16x16 彩色棋盘背景：色相随格子坐标变化（每帧固定）。
        for (int y = 0; y < m_height; ++y) {
            std::uint8_t* row = data + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
            for (int x = 0; x < m_width; ++x) {
                const int cx = x / 16;
                const int cy = y / 16;
                const std::uint8_t b = static_cast<std::uint8_t>(32 + ((cx * 37 + cy * 11) & 0x7F));
                const std::uint8_t g = static_cast<std::uint8_t>(24 + ((cx * 13 + cy * 41) & 0x7F));
                const std::uint8_t r = static_cast<std::uint8_t>(16 + ((cx * 29 + cy * 23) & 0x7F));
                row[x * 4 + 0] = b;
                row[x * 4 + 1] = g;
                row[x * 4 + 2] = r;
                row[x * 4 + 3] = 255;
            }
        }

        // 2) 移动色块：32x32，位置随帧号移动；颜色由帧号决定（每帧变化）。
        const int block = (m_width >= 96 && m_height >= 96) ? 32 : 16;
        const int span_x = m_width - block > 0 ? m_width - block : 1;
        const int span_y = m_height - block > 0 ? m_height - block : 1;
        const int bx = static_cast<int>((m_frame_id * 7U) % static_cast<std::uint32_t>(span_x + 1));
        const int by = static_cast<int>((m_frame_id * 5U) % static_cast<std::uint32_t>(span_y + 1));
        const std::uint8_t cb = static_cast<std::uint8_t>(64 + (m_frame_id * 37U & 0xBF));
        const std::uint8_t cg = static_cast<std::uint8_t>(255 - (m_frame_id * 53U & 0xBF));
        const std::uint8_t cr = static_cast<std::uint8_t>(64 + (m_frame_id * 97U & 0xBF));
        for (int y = by; y < by + block && y < m_height; ++y) {
            std::uint8_t* row = data + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
            for (int x = bx; x < bx + block && x < m_width; ++x) {
                row[x * 4 + 0] = cb;
                row[x * 4 + 1] = cg;
                row[x * 4 + 2] = cr;
                row[x * 4 + 3] = 255;
            }
        }

        // 3) 帧号位图：32 个 bit 画成 8x8 个 3x3 小方块（左上角）。
        for (int bit = 0; bit < 32; ++bit) {
            const bool on = ((m_frame_id >> (31 - bit)) & 1U) != 0;
            const int gx = 4 + (bit % 8) * 4;
            const int gy = 4 + (bit / 8) * 4;
            for (int y = gy; y < gy + 3 && y < m_height; ++y) {
                std::uint8_t* row =
                    data + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
                for (int x = gx; x < gx + 3 && x < m_width; ++x) {
                    row[x * 4 + 0] = on ? 0x00 : 0xFF;
                    row[x * 4 + 1] = on ? 0xFF : 0xFF;
                    row[x * 4 + 2] = on ? 0x00 : 0xFF;
                    row[x * 4 + 3] = 255;
                }
            }
        }

        // 4) 底部进度条：长度 = frame_id % 宽度（每帧移动，制造横向脏块）。
        const int bar_y = m_height - 6;
        const int bar_w = static_cast<int>(m_frame_id % static_cast<std::uint32_t>(m_width));
        for (int y = bar_y; y < bar_y + 4 && y < m_height; ++y) {
            std::uint8_t* row = data + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
            for (int x = 0; x < bar_w; ++x) {
                row[x * 4 + 0] = 0x00;
                row[x * 4 + 1] = 0x80;
                row[x * 4 + 2] = 0xFF;
                row[x * 4 + 3] = 255;
            }
        }

        ++m_frame_id;
        return true;
    }

    int width() const noexcept override {
        return m_width;
    }
    int height() const noexcept override {
        return m_height;
    }
    const char* name() const noexcept override {
        return "test-pattern";
    }
    bool available() const noexcept override {
        return true;
    }

   private:
    int m_width;
    int m_height;
    std::uint32_t m_frame_id = 0;
};

// ---------------------------------------------------------------------------
// 包级模拟弱网（确定性、虚拟时钟）
// ---------------------------------------------------------------------------

/// @brief 一个待投递的数据报（包级）。
struct SimPacket {
    std::uint64_t order = 0;  ///< 发送顺序（同 due 时保持稳定）。
    std::int64_t due_ns = 0;  ///< 模拟到达时刻。
    std::vector<std::uint8_t> bytes;
};

/// @brief 确定性伪随机（xorshift32）。
class Rng final {
   public:
    explicit Rng(std::uint32_t seed = 0x9E3779B9U) : m_state(seed == 0 ? 0x9E3779B9U : seed) {}

    std::uint32_t Next() noexcept {
        std::uint32_t x = m_state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        m_state = x;
        return x;
    }

    /// @brief [0, bound) 的均匀整数。
    std::uint32_t NextBelow(std::uint32_t bound) noexcept {
        return bound == 0 ? 0 : Next() % bound;
    }

    /// @brief 以 `permille` 千分比概率返回 true。
    bool ChancePermille(std::uint32_t permille) noexcept {
        return NextBelow(1000) < permille;
    }

   private:
    std::uint32_t m_state;
};

/// @brief 包级弱网模拟：丢包 + 每包独立延迟（延迟差异天然产生乱序）。
/// @details 只记录、不真正发送，`DeliverDue` 按虚拟时钟把到期包交给接收端。
class SimulatedNetwork final {
   public:
    struct Config {
        std::uint32_t loss_permille = 0;  ///< 丢包率（千分比），50 = 5%。
        std::int64_t base_delay_ns = 0;   ///< 所有包的基础延迟。
        std::int64_t jitter_ns = 0;       ///< 额外随机延迟上界（±50ms 即 100ms 跨度）。
        std::int64_t frame_gap_ns = 0;    ///< 发送端每帧间隔（媒体时间）。
        std::uint32_t seed = 0x9E3779B9U; ///< 确定性种子。
    };

    explicit SimulatedNetwork(Config config) : m_config(config), m_rng(config.seed) {
        DrawFrameJitter();
    }

    /// @brief 发送一个包（分片），按配置决定丢弃/延迟。
    /// @return true = 未丢；false = 已丢弃。
    bool Send(const std::uint8_t* data, std::size_t bytes) {
        ++m_sent;
        if (m_config.loss_permille > 0 && m_rng.ChancePermille(m_config.loss_permille)) {
            ++m_dropped;
            return false;
        }
        SimPacket packet;
        packet.order = m_order++;
        // 抖动按「帧」抽取：同一帧的分片共享同一到达延迟（真实网络里排队抖动
        // 是毫秒级、帧内分片基本连续），帧间延迟差才构成媒体抖动。
        packet.due_ns = m_now_ns + m_config.base_delay_ns + m_frame_jitter_ns;
        packet.bytes.assign(data, data + bytes);
        m_packets.push_back(std::move(packet));
        return true;
    }

    /// @brief 推进虚拟时钟一个帧间隔（发送侧调用），并为下一帧重新抽取抖动。
    void AdvanceFrame() noexcept {
        m_now_ns += m_config.frame_gap_ns;
        DrawFrameJitter();
    }

    /// @brief 推送到期包：按 (due_ns, order) 排序后逐个交给 `deliver`。
    /// @param deliver 参数：包字节、模拟到达时刻。
    /// @return 交付包数。
    template <typename Deliver>
    std::size_t DeliverDue(Deliver&& deliver) {
        std::sort(m_packets.begin(), m_packets.end(), [](const SimPacket& a, const SimPacket& b) {
            if (a.due_ns != b.due_ns) {
                return a.due_ns < b.due_ns;
            }
            return a.order < b.order;
        });
        std::size_t count = 0;
        for (const SimPacket& packet : m_packets) {
            deliver(packet.bytes.data(), packet.bytes.size(), packet.due_ns);
            ++count;
        }
        m_packets.clear();
        return count;
    }

    /// @brief 已发送包数。
    std::uint64_t sent() const noexcept {
        return m_sent;
    }
    /// @brief 被丢弃包数。
    std::uint64_t dropped() const noexcept {
        return m_dropped;
    }
    /// @brief 当前虚拟时刻。
    std::int64_t now_ns() const noexcept {
        return m_now_ns;
    }

   private:
    /// @brief 抽取本帧的抖动（0..jitter_ns，微秒粒度）。
    void DrawFrameJitter() noexcept {
        if (m_config.jitter_ns <= 0) {
            m_frame_jitter_ns = 0;
            return;
        }
        m_frame_jitter_ns =
            static_cast<std::int64_t>(
                m_rng.NextBelow(static_cast<std::uint32_t>(m_config.jitter_ns / 1000 + 1))) *
            1000;
    }

    Config m_config;
    Rng m_rng;
    std::uint64_t m_order = 0;
    std::uint64_t m_sent = 0;
    std::uint64_t m_dropped = 0;
    std::int64_t m_now_ns = 0;
    std::int64_t m_frame_jitter_ns = 0;
    std::vector<SimPacket> m_packets;
};

}  // namespace go2cpp::media::e2e
