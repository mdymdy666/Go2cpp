/**
 * @file crypto.cpp
 * @brief ChaCha20-Poly1305（RFC 8439）、HChaCha20 KDF、重放窗口与加密帧实现。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include "go2cpp/config/env.hpp"
#include "go2cpp/bento/net/crypto.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#define BENTO_CRYPTO_X86_SIMD 1
#else
#define BENTO_CRYPTO_X86_SIMD 0
#endif

#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/go/crypto_rand.hpp"

namespace go2cpp::bento {
namespace {

/// @brief 多路 Poly1305 是否可用：x86 SIMD、GCC/Clang 的 target 属性，
///        以及 radix-2^44 标量路径（MSVC 走 5x26 标量回退，不参与多路）。
#if BENTO_CRYPTO_X86_SIMD && (defined(__GNUC__) || defined(__clang__)) && \
    defined(__SIZEOF_INT128__)
#define BENTO_POLY_MULTI_SIMD 1
#else
#define BENTO_POLY_MULTI_SIMD 0
#endif

/// @brief SIMD 后端档位；Poly1305 与 ChaCha20 共用同一份运行期探测结果。
///        kAvx512Ifma 表示 AVX-512F/DQ/BW/VL + IFMA（Poly1305 走 radix-2^44
///        8 路，ChaCha20 与 kAvx512 同样走 16 lane 内核）。
enum class SimdBackend : std::uint8_t {
    kScalar = 0,
    kAvx2 = 1,
    kAvx512 = 2,
    kAvx512Ifma = 3
};

/// @brief 进程内缓存的 SIMD 后端（定义在 ChaCha20 SIMD 段；此处前向声明，
///        使 Poly1305 能早于该定义复用同一份 __builtin_cpu_supports 探测）。
SimdBackend ActiveSimdBackend() noexcept;

inline std::uint32_t Rotl32(std::uint32_t value, int shift) noexcept {
    return (value << shift) | (value >> (32 - shift));
}

inline std::uint32_t Load32LE(const std::uint8_t* in) noexcept {
    return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) |
           (static_cast<std::uint32_t>(in[3]) << 24);
}

inline void Store32LE(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
    out[2] = static_cast<std::uint8_t>(value >> 16);
    out[3] = static_cast<std::uint8_t>(value >> 24);
}

inline std::uint64_t Load64LE(const std::uint8_t* in) noexcept {
    return static_cast<std::uint64_t>(Load32LE(in)) |
           (static_cast<std::uint64_t>(Load32LE(in + 4)) << 32);
}

inline void Store64LE(std::uint8_t* out, std::uint64_t value) noexcept {
    Store32LE(out, static_cast<std::uint32_t>(value));
    Store32LE(out + 4, static_cast<std::uint32_t>(value >> 32));
}

inline void Store32BE(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

inline std::uint32_t Load32BE(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

inline void Store64BE(std::uint8_t* out, std::uint64_t value) noexcept {
    Store32BE(out, static_cast<std::uint32_t>(value >> 32));
    Store32BE(out + 4, static_cast<std::uint32_t>(value));
}

inline std::uint64_t Load64BE(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint64_t>(Load32BE(in)) << 32) | Load32BE(in + 4);
}

inline void QuarterRound(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c,
                         std::uint32_t& d) noexcept {
    a += b;
    d ^= a;
    d = Rotl32(d, 16);
    c += d;
    b ^= c;
    b = Rotl32(b, 12);
    a += b;
    d ^= a;
    d = Rotl32(d, 8);
    c += d;
    b ^= c;
    b = Rotl32(b, 7);
}

/// @brief 20 轮 ChaCha 排列（10 次 double round）后叠加初始状态。
void ChaCha20Permute(const std::uint32_t input[16], std::uint32_t output[16]) noexcept {
    std::uint32_t x[16];
    std::memcpy(x, input, sizeof(x));
    for (int round = 0; round < 10; ++round) {
        QuarterRound(x[0], x[4], x[8], x[12]);
        QuarterRound(x[1], x[5], x[9], x[13]);
        QuarterRound(x[2], x[6], x[10], x[14]);
        QuarterRound(x[3], x[7], x[11], x[15]);
        QuarterRound(x[0], x[5], x[10], x[15]);
        QuarterRound(x[1], x[6], x[11], x[12]);
        QuarterRound(x[2], x[7], x[8], x[13]);
        QuarterRound(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) {
        output[i] = x[i] + input[i];
    }
}

/// @brief 构造 ChaCha20 初始状态；nonce 是 12B（3 words）。
void ChaCha20InitState(const std::uint8_t key[32], std::uint32_t counter,
                       const std::uint8_t nonce[12], std::uint32_t state[16]) noexcept {
    state[0] = 0x61707865U;  // "expa"
    state[1] = 0x3320646eU;  // "nd 3"
    state[2] = 0x79622d32U;  // "2-by"
    state[3] = 0x6b206574U;  // "te k"
    for (int i = 0; i < 8; ++i) {
        state[4 + i] = Load32LE(key + i * 4);
    }
    state[12] = counter;
    state[13] = Load32LE(nonce + 0);
    state[14] = Load32LE(nonce + 4);
    state[15] = Load32LE(nonce + 8);
}

/// 一个 ChaCha20 密钥流块的字节数。
constexpr std::size_t kChaChaBlockBytes = 64;

/// @brief 把初始状态里的 counter 换成指定值后做 20 轮排列。
/// @details 单块路径（尾部不足 256B）复用同一个 state，只覆盖 counter 字。
void ChaCha20PermuteAt(const std::uint32_t state[16], std::uint32_t counter,
                       std::uint32_t output[16]) noexcept {
    std::uint32_t lane_state[16];
    std::memcpy(lane_state, state, sizeof(lane_state));
    lane_state[12] = counter;
    ChaCha20Permute(lane_state, output);
}

/// @brief 4 路并行的 ChaCha20 块生成：counter .. counter+3。
/// @details 四个 lane 相互独立，交错执行可让 CPU 同时利用多条整数流水线；
///          本机 1MiB 缓冲实测 ChaCha20 吞吐 709→830 MB/s（约 +17%），
///          每个 lane 的输出与逐块调用逐字节一致。
void ChaCha20Permute4(const std::uint32_t state[16], std::uint32_t counter,
                      std::uint32_t blocks[4][16]) noexcept {
    std::uint32_t x[4][16];
    std::uint32_t lane_state[4][16];
    for (int lane = 0; lane < 4; ++lane) {
        std::memcpy(lane_state[lane], state, sizeof(lane_state[lane]));
        lane_state[lane][12] = counter + static_cast<std::uint32_t>(lane);
        std::memcpy(x[lane], lane_state[lane], sizeof(x[lane]));
    }
    for (int round = 0; round < 10; ++round) {
        for (int lane = 0; lane < 4; ++lane) {
            QuarterRound(x[lane][0], x[lane][4], x[lane][8], x[lane][12]);
            QuarterRound(x[lane][1], x[lane][5], x[lane][9], x[lane][13]);
            QuarterRound(x[lane][2], x[lane][6], x[lane][10], x[lane][14]);
            QuarterRound(x[lane][3], x[lane][7], x[lane][11], x[lane][15]);
            QuarterRound(x[lane][0], x[lane][5], x[lane][10], x[lane][15]);
            QuarterRound(x[lane][1], x[lane][6], x[lane][11], x[lane][12]);
            QuarterRound(x[lane][2], x[lane][7], x[lane][8], x[lane][13]);
            QuarterRound(x[lane][3], x[lane][4], x[lane][9], x[lane][14]);
        }
    }
    for (int lane = 0; lane < 4; ++lane) {
        for (int i = 0; i < 16; ++i) {
            blocks[lane][i] = x[lane][i] + lane_state[lane][i];
        }
    }
}

/// @brief Poly1305 一次性 MAC 状态。
/// @details 默认走 radix 2^44 的 3-limb 表示：每 16B 块 9 次 64×64→128 乘法，
///          进位链只有 3 段；无 128 位整数（MSVC）时退回 5×26 位 limb 实现。
#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 Poly1305Wide;
struct Poly1305Ctx {
    std::uint64_t r[3] = {};
    std::uint64_t h[3] = {};
    std::uint64_t pad[2] = {};
};
/// 低两 limb 44 位、最高 limb 42 位（3×44 = 132 > 130）。
constexpr std::uint64_t kPoly1305Mask44 = (std::uint64_t{1} << 44) - 1;
constexpr std::uint64_t kPoly1305Mask42 = (std::uint64_t{1} << 42) - 1;
/// 完整 16B 块在 bit 128 处的隐含位，折算到 2^88 limb 是 2^40。
constexpr std::uint64_t kPoly1305Hibit = std::uint64_t{1} << 40;

#if BENTO_POLY_MULTI_SIMD
/// @brief 多路 SIMD 吸收完整 16B 块（AVX-512 8 路 / AVX2 4 路，运行期分派）。
/// @return 已消费字节数；0 表示没有完整批次，调用方继续走标量路径。
std::size_t Poly1305BlocksSimd(std::uint64_t h[3], const std::uint64_t r[3],
                               const std::uint8_t* data, std::size_t bytes) noexcept;
#endif

void Poly1305Init(Poly1305Ctx* ctx, const std::uint8_t key[32]) noexcept {
    // clamp：r 的低 4 位与高 4 位（bit 124..127）必须清零（RFC 8439 §2.5）。
    const std::uint64_t r0 = Load64LE(key + 0) & 0x0ffffffc0fffffffULL;
    const std::uint64_t r1 = Load64LE(key + 8) & 0x0ffffffc0ffffffcULL;
    ctx->r[0] = r0 & kPoly1305Mask44;
    ctx->r[1] = ((r0 >> 44) | (r1 << 20)) & kPoly1305Mask44;
    ctx->r[2] = r1 >> 24;
    ctx->pad[0] = Load64LE(key + 16);
    ctx->pad[1] = Load64LE(key + 24);
    ctx->h[0] = 0;
    ctx->h[1] = 0;
    ctx->h[2] = 0;
}

/// @brief 吸收一个 16B 块；hibit 为 2^40（完整块）或 0（末尾补 1 的块）。
void Poly1305Block(Poly1305Ctx* ctx, const std::uint8_t block[16],
                   std::uint64_t hibit) noexcept {
    const std::uint64_t lo = Load64LE(block + 0);
    const std::uint64_t hi = Load64LE(block + 8);

    // 2^132 ≡ 4·2^130 ≡ 20 (mod 2^130-5)：跨 limb 的项折回时乘 20。
    const std::uint64_t s1 = ctx->r[1] * 20U;
    const std::uint64_t s2 = ctx->r[2] * 20U;

    std::uint64_t h0 = ctx->h[0] + (lo & kPoly1305Mask44);
    std::uint64_t h1 = ctx->h[1] + (((lo >> 44) | (hi << 20)) & kPoly1305Mask44);
    std::uint64_t h2 = ctx->h[2] + ((hi >> 24) | hibit);

    Poly1305Wide d0 = static_cast<Poly1305Wide>(h0) * ctx->r[0] +
                      static_cast<Poly1305Wide>(h1) * s2 +
                      static_cast<Poly1305Wide>(h2) * s1;
    Poly1305Wide d1 = static_cast<Poly1305Wide>(h0) * ctx->r[1] +
                      static_cast<Poly1305Wide>(h1) * ctx->r[0] +
                      static_cast<Poly1305Wide>(h2) * s2;
    Poly1305Wide d2 = static_cast<Poly1305Wide>(h0) * ctx->r[2] +
                      static_cast<Poly1305Wide>(h1) * ctx->r[1] +
                      static_cast<Poly1305Wide>(h2) * ctx->r[0];

    std::uint64_t carry = static_cast<std::uint64_t>(d0 >> 44);
    h0 = static_cast<std::uint64_t>(d0) & kPoly1305Mask44;
    d1 += carry;
    carry = static_cast<std::uint64_t>(d1 >> 44);
    h1 = static_cast<std::uint64_t>(d1) & kPoly1305Mask44;
    d2 += carry;
    carry = static_cast<std::uint64_t>(d2 >> 42);
    h2 = static_cast<std::uint64_t>(d2) & kPoly1305Mask42;
    h0 += carry * 5U;
    carry = h0 >> 44;
    h0 &= kPoly1305Mask44;
    h1 += carry;

    ctx->h[0] = h0;
    ctx->h[1] = h1;
    ctx->h[2] = h2;
}

/// @brief 处理 AEAD 的“数据 + 零填充到 16B”段。
void Poly1305UpdatePadded(Poly1305Ctx* ctx, const std::uint8_t* data,
                          std::size_t bytes) noexcept {
    std::size_t offset = 0;
#if BENTO_POLY_MULTI_SIMD
    // 完整批次走多路 SIMD；不足一批的剩余块与末尾尾块仍走标量。
    offset += Poly1305BlocksSimd(ctx->h, ctx->r, data, bytes);
#endif
    while (bytes - offset >= 16) {
        Poly1305Block(ctx, data + offset, kPoly1305Hibit);
        offset += 16;
    }
    const std::size_t remainder = bytes - offset;
    if (remainder != 0) {
        std::uint8_t block[16] = {};
        std::memcpy(block, data + offset, remainder);
        Poly1305Block(ctx, block, kPoly1305Hibit);
    }
}

void Poly1305Finish(const Poly1305Ctx* ctx, std::uint8_t tag[16]) noexcept {
    std::uint64_t h0 = ctx->h[0];
    std::uint64_t h1 = ctx->h[1];
    std::uint64_t h2 = ctx->h[2];

    // 全套进位，把 h 收敛到 h0/h1 < 2^44、h2 < 2^42。
    std::uint64_t carry = h1 >> 44;
    h1 &= kPoly1305Mask44;
    h2 += carry;
    carry = h2 >> 42;
    h2 &= kPoly1305Mask42;
    h0 += carry * 5U;
    carry = h0 >> 44;
    h0 &= kPoly1305Mask44;
    h1 += carry;

    // 计算 h - p = h + 5 - 2^130；借位则说明 h < p，保留 h。
    std::uint64_t g0 = h0 + 5U;
    carry = g0 >> 44;
    g0 &= kPoly1305Mask44;
    std::uint64_t g1 = h1 + carry;
    carry = g1 >> 44;
    g1 &= kPoly1305Mask44;
    std::uint64_t g2 = h2 + carry - (std::uint64_t{1} << 42);
    const std::uint64_t select = (g2 >> 63) - 1U;
    g0 &= select;
    g1 &= select;
    g2 &= select;
    h0 = (h0 & ~select) | g0;
    h1 = (h1 & ~select) | g1;
    h2 = (h2 & ~select) | g2;

    // h mod 2^128，再按 64 位小端重打包。
    const std::uint64_t f0 = h0 | (h1 << 44);
    const std::uint64_t f1 = (h1 >> 20) | (h2 << 24);

    Poly1305Wide acc = static_cast<Poly1305Wide>(f0) + ctx->pad[0];
    std::uint64_t out0 = static_cast<std::uint64_t>(acc);
    std::uint64_t out1 = f1 + static_cast<std::uint64_t>(acc >> 64) + ctx->pad[1];
    Store64LE(tag + 0, out0);
    Store64LE(tag + 8, out1);
}

#else  // !defined(__SIZEOF_INT128__)

/// 5×26 位 limb 回退实现（poly1305-donna 结构）。
constexpr std::uint64_t kPoly1305Hibit = 1U << 24;
/// @brief 5×26 位 limb 的 Poly1305 状态（poly1305-donna 结构）。
struct Poly1305Ctx {
    std::uint32_t r[5] = {};
    std::uint32_t h[5] = {};
    std::uint32_t pad[4] = {};
};

void Poly1305Init(Poly1305Ctx* ctx, const std::uint8_t key[32]) noexcept {
    ctx->r[0] = Load32LE(key + 0) & 0x3ffffffU;
    ctx->r[1] = (Load32LE(key + 3) >> 2) & 0x3ffff03U;
    ctx->r[2] = (Load32LE(key + 6) >> 4) & 0x3ffc0ffU;
    ctx->r[3] = (Load32LE(key + 9) >> 6) & 0x3f03fffU;
    ctx->r[4] = (Load32LE(key + 12) >> 8) & 0x00fffffU;
    ctx->pad[0] = Load32LE(key + 16);
    ctx->pad[1] = Load32LE(key + 20);
    ctx->pad[2] = Load32LE(key + 24);
    ctx->pad[3] = Load32LE(key + 28);
    std::memset(ctx->h, 0, sizeof(ctx->h));
}

/// @brief 吸收一个 16B 块；hibit 为 1<<24（完整块）或 0（末尾补 1 的块）。
void Poly1305Block(Poly1305Ctx* ctx, const std::uint8_t block[16],
                   std::uint64_t hibit) noexcept {
    std::uint32_t h0 = ctx->h[0] + (Load32LE(block + 0) & 0x3ffffffU);
    std::uint32_t h1 = ctx->h[1] + ((Load32LE(block + 3) >> 2) & 0x3ffffffU);
    std::uint32_t h2 = ctx->h[2] + ((Load32LE(block + 6) >> 4) & 0x3ffffffU);
    std::uint32_t h3 = ctx->h[3] + ((Load32LE(block + 9) >> 6) & 0x3ffffffU);
    std::uint32_t h4 = ctx->h[4] + ((Load32LE(block + 12) >> 8) | static_cast<std::uint32_t>(hibit));

    const std::uint32_t r0 = ctx->r[0];
    const std::uint32_t r1 = ctx->r[1];
    const std::uint32_t r2 = ctx->r[2];
    const std::uint32_t r3 = ctx->r[3];
    const std::uint32_t r4 = ctx->r[4];
    const std::uint32_t s1 = r1 * 5U;
    const std::uint32_t s2 = r2 * 5U;
    const std::uint32_t s3 = r3 * 5U;
    const std::uint32_t s4 = r4 * 5U;

    std::uint64_t d0 = static_cast<std::uint64_t>(h0) * r0 +
                       static_cast<std::uint64_t>(h1) * s4 +
                       static_cast<std::uint64_t>(h2) * s3 +
                       static_cast<std::uint64_t>(h3) * s2 +
                       static_cast<std::uint64_t>(h4) * s1;
    std::uint64_t d1 = static_cast<std::uint64_t>(h0) * r1 +
                       static_cast<std::uint64_t>(h1) * r0 +
                       static_cast<std::uint64_t>(h2) * s4 +
                       static_cast<std::uint64_t>(h3) * s3 +
                       static_cast<std::uint64_t>(h4) * s2;
    std::uint64_t d2 = static_cast<std::uint64_t>(h0) * r2 +
                       static_cast<std::uint64_t>(h1) * r1 +
                       static_cast<std::uint64_t>(h2) * r0 +
                       static_cast<std::uint64_t>(h3) * s4 +
                       static_cast<std::uint64_t>(h4) * s3;
    std::uint64_t d3 = static_cast<std::uint64_t>(h0) * r3 +
                       static_cast<std::uint64_t>(h1) * r2 +
                       static_cast<std::uint64_t>(h2) * r1 +
                       static_cast<std::uint64_t>(h3) * r0 +
                       static_cast<std::uint64_t>(h4) * s4;
    std::uint64_t d4 = static_cast<std::uint64_t>(h0) * r4 +
                       static_cast<std::uint64_t>(h1) * r3 +
                       static_cast<std::uint64_t>(h2) * r2 +
                       static_cast<std::uint64_t>(h3) * r1 +
                       static_cast<std::uint64_t>(h4) * r0;

    std::uint64_t carry = d0 >> 26;
    h0 = static_cast<std::uint32_t>(d0) & 0x3ffffffU;
    d1 += carry;
    carry = d1 >> 26;
    h1 = static_cast<std::uint32_t>(d1) & 0x3ffffffU;
    d2 += carry;
    carry = d2 >> 26;
    h2 = static_cast<std::uint32_t>(d2) & 0x3ffffffU;
    d3 += carry;
    carry = d3 >> 26;
    h3 = static_cast<std::uint32_t>(d3) & 0x3ffffffU;
    d4 += carry;
    carry = d4 >> 26;
    h4 = static_cast<std::uint32_t>(d4) & 0x3ffffffU;
    h0 += static_cast<std::uint32_t>(carry) * 5U;
    carry = h0 >> 26;
    h0 &= 0x3ffffffU;
    h1 += static_cast<std::uint32_t>(carry);

    ctx->h[0] = h0;
    ctx->h[1] = h1;
    ctx->h[2] = h2;
    ctx->h[3] = h3;
    ctx->h[4] = h4;
}

/// @brief 处理 AEAD 的“数据 + 零填充到 16B”段。
void Poly1305UpdatePadded(Poly1305Ctx* ctx, const std::uint8_t* data,
                          std::size_t bytes) noexcept {
    std::size_t offset = 0;
    while (bytes - offset >= 16) {
        Poly1305Block(ctx, data + offset, kPoly1305Hibit);
        offset += 16;
    }
    const std::size_t remainder = bytes - offset;
    if (remainder != 0) {
        std::uint8_t block[16] = {};
        std::memcpy(block, data + offset, remainder);
        Poly1305Block(ctx, block, kPoly1305Hibit);
    }
}

void Poly1305Finish(const Poly1305Ctx* ctx, std::uint8_t tag[16]) noexcept {
    std::uint32_t h0 = ctx->h[0];
    std::uint32_t h1 = ctx->h[1];
    std::uint32_t h2 = ctx->h[2];
    std::uint32_t h3 = ctx->h[3];
    std::uint32_t h4 = ctx->h[4];

    std::uint32_t carry = h1 >> 26;
    h1 &= 0x3ffffffU;
    h2 += carry;
    carry = h2 >> 26;
    h2 &= 0x3ffffffU;
    h3 += carry;
    carry = h3 >> 26;
    h3 &= 0x3ffffffU;
    h4 += carry;
    carry = h4 >> 26;
    h4 &= 0x3ffffffU;
    h0 += carry * 5U;
    carry = h0 >> 26;
    h0 &= 0x3ffffffU;
    h1 += carry;

    std::uint32_t g0 = h0 + 5U;
    carry = g0 >> 26;
    g0 &= 0x3ffffffU;
    std::uint32_t g1 = h1 + carry;
    carry = g1 >> 26;
    g1 &= 0x3ffffffU;
    std::uint32_t g2 = h2 + carry;
    carry = g2 >> 26;
    g2 &= 0x3ffffffU;
    std::uint32_t g3 = h3 + carry;
    carry = g3 >> 26;
    g3 &= 0x3ffffffU;
    std::uint32_t g4 = h4 + carry - (1U << 26);

    const std::uint32_t select = (g4 >> 31) - 1U;
    g4 &= 0x3ffffffU;
    h0 = (h0 & ~select) | (g0 & select);
    h1 = (h1 & ~select) | (g1 & select);
    h2 = (h2 & ~select) | (g2 & select);
    h3 = (h3 & ~select) | (g3 & select);
    h4 = (h4 & ~select) | (g4 & select);

    // 把 5×26 位 limb 重新打包成 4×32 位字（只取 mod 2^128 的低位）。
    // 注意：不能直接用 h0 | (h1 << 26) 当 word1；donna 的 finish 需要
    // 按 26 位偏移拆分：word1 = h1>>6 | h2<<20，word2 = h2>>12 | h3<<14，
    // word3 = h3>>18 | h4<<8。
    const std::uint32_t word0 = h0 | (h1 << 26);
    const std::uint32_t word1 = (h1 >> 6) | (h2 << 20);
    const std::uint32_t word2 = (h2 >> 12) | (h3 << 14);
    const std::uint32_t word3 = (h3 >> 18) | (h4 << 8);

    std::uint64_t f = static_cast<std::uint64_t>(word0) + ctx->pad[0];
    const std::uint32_t out0 = static_cast<std::uint32_t>(f);
    f = (f >> 32) + static_cast<std::uint64_t>(word1) + ctx->pad[1];
    const std::uint32_t out1 = static_cast<std::uint32_t>(f);
    f = (f >> 32) + static_cast<std::uint64_t>(word2) + ctx->pad[2];
    const std::uint32_t out2 = static_cast<std::uint32_t>(f);
    f = (f >> 32) + static_cast<std::uint64_t>(word3) + ctx->pad[3];
    const std::uint32_t out3 = static_cast<std::uint32_t>(f);

    Store32LE(tag + 0, out0);
    Store32LE(tag + 4, out1);
    Store32LE(tag + 8, out2);
    Store32LE(tag + 12, out3);
}

#endif

/// 握手与数据方向密钥的域分离标签（各 16B）。
constexpr std::uint8_t kLabelHandshakeC2S[16] = {'B', 'E', 'N', 'T', 'O', '-', 'H', 'S',
                                                 '-', 'C', '2', 'S', '-', '0', '0', '1'};
constexpr std::uint8_t kLabelHandshakeS2C[16] = {'B', 'E', 'N', 'T', 'O', '-', 'H', 'S',
                                                 '-', 'S', '2', 'C', '-', '0', '0', '1'};
constexpr std::uint8_t kLabelDataC2S[16] = {'B', 'E', 'N', 'T', 'O', '-', 'D', 'A',
                                            'T', 'A', '-', 'C', '2', 'S', '-', '1'};
constexpr std::uint8_t kLabelDataS2C[16] = {'B', 'E', 'N', 'T', 'O', '-', 'D', 'A',
                                            'T', 'A', '-', 'S', '2', 'C', '-', '1'};
/// 会话票据 MAC 根密钥 / PSK 指纹的域分离标签。
constexpr std::uint8_t kLabelTicketKey[16] = {'B', 'E', 'N', 'T', 'O', '-', 'T',
                                              'I', 'C', 'K', 'E', 'T', '-', '0', '1'};
constexpr std::uint8_t kLabelPskFingerprint[16] = {'B', 'E', 'N', 'T', 'O', '-', 'P',
                                                   'S', 'K', '-', 'F', 'P', '-', '0', '1'};
/// X25519 共享密钥混入 PSK 的域分离标签。
constexpr std::uint8_t kLabelX25519Mix[16] = {'B', 'E', 'N', 'T', 'O', '-', 'X', '2',
                                               '5', '5', '1', '9', '-', '0', '0', '1'};

const std::uint8_t* DirectionLabel(SecureDirection direction) noexcept {
    return direction == SecureDirection::kClientToServer ? kLabelDataC2S : kLabelDataS2C;
}

// ---------------------------------------------------------------------------
// ChaCha20 SIMD 后端（AVX2 8 路 / AVX-512 16 路）
// ---------------------------------------------------------------------------
//
// 布局：vertical —— 每个 SIMD 向量装 N 个 block 的同一个 state word
// （lane k = block k）。20 轮排列全是整向量操作，最后用 N×N 的 32-bit
// 转置把“word 主序”还原成“block 主序”，再与输入异或写出。
//
// 指令成本（每 64B 密钥流）：
//   AVX-512 16 lane：每批 1024B 的轮函数 960 条 + 转置 64 条 + 输出 48 条；
//     vprold 单指令完成 32-bit 循环移位。
//   AVX2 8 lane：每批 512B 的轮函数 1280 条；没有 32-bit 循环移位，
//     16/8 位旋转用 vpshufb（1 条），12/7 位用 shift+shift+or（3 条）。
//   两者都只在完整批上工作，不足一批的尾部交给原标量 4 路/单块路径。
//
// 运行时选择：默认按 __builtin_cpu_supports 探测；环境变量
// BENTO_CRYPTO_SIMD=scalar|avx2|avx512|avx512ifma 可强制降级（测试与虚拟化排障用，
// 每进程只读一次，且不会越过硬件实际能力）。

/// 解析 BENTO_CRYPTO_SIMD 的取值；未知值按最保守的标量处理。
SimdBackend ParseSimdBackend(const char* text) noexcept {
    if (text == nullptr) {
        return SimdBackend::kScalar;
    }
    if (std::strcmp(text, "avx2") == 0) {
        return SimdBackend::kAvx2;
    }
    if (std::strcmp(text, "avx512") == 0 || std::strcmp(text, "avx-512") == 0) {
        return SimdBackend::kAvx512;
    }
    if (std::strcmp(text, "avx512ifma") == 0 || std::strcmp(text, "avx512-ifma") == 0 ||
        std::strcmp(text, "ifma") == 0) {
        return SimdBackend::kAvx512Ifma;
    }
    return SimdBackend::kScalar;
}

/// @brief 探测本机可用的最快后端；环境变量只能在此基础上下调。
SimdBackend DetectSimdBackend() noexcept {
    SimdBackend detected = SimdBackend::kScalar;
#if BENTO_CRYPTO_X86_SIMD
#if defined(__GNUC__) && !defined(__clang__)
    // GCC 需要显式初始化 CPU 特性表（共享库里首次调用可能早于构造器）。
    __builtin_cpu_init();
#endif
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq") &&
        __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512vl")) {
        detected = __builtin_cpu_supports("avx512ifma") ? SimdBackend::kAvx512Ifma
                                                       : SimdBackend::kAvx512;
    } else if (__builtin_cpu_supports("avx2")) {
        detected = SimdBackend::kAvx2;
    }
#endif
    try {
        const auto override_text = go2cpp::config::GetEnv("BENTO_CRYPTO_SIMD");
        if (!override_text.has_value()) {
            return detected;
        }
        const SimdBackend requested = ParseSimdBackend(override_text->c_str());
        // 强制值不能把程序推上本机不支持的指令集。
        return requested < detected ? requested : detected;
    } catch (...) {
        // noexcept 函数：读取失败时只使用 CPU 探测结果。
        return detected;
    }
}

/// @brief 进程内缓存的后端选择（magic static 只做一次探测）。
SimdBackend ActiveSimdBackend() noexcept {
    static const SimdBackend backend = DetectSimdBackend();
    return backend;
}

#if BENTO_CRYPTO_X86_SIMD

#define BENTO_CRYPTO_TARGET_AVX2 __attribute__((target("avx2")))
#define BENTO_CRYPTO_TARGET_AVX512 \
    __attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))
#define BENTO_CRYPTO_TARGET_AVX512IFMA \
    __attribute__((target("avx512f,avx512dq,avx512bw,avx512vl,avx512ifma")))

#define BENTO_CRYPTO_AVX2_ROTL16(value) _mm256_shuffle_epi8((value), rotate16)
#define BENTO_CRYPTO_AVX2_ROTL8(value) _mm256_shuffle_epi8((value), rotate8)
#define BENTO_CRYPTO_AVX2_ROTL12(value) \
    _mm256_or_si256(_mm256_slli_epi32((value), 12), _mm256_srli_epi32((value), 20))
#define BENTO_CRYPTO_AVX2_ROTL7(value) \
    _mm256_or_si256(_mm256_slli_epi32((value), 7), _mm256_srli_epi32((value), 25))

#define BENTO_CRYPTO_AVX2_QR(a, b, c, d)           \
    (a) = _mm256_add_epi32((a), (b));              \
    (d) = _mm256_xor_si256((d), (a));              \
    (d) = BENTO_CRYPTO_AVX2_ROTL16(d);             \
    (c) = _mm256_add_epi32((c), (d));              \
    (b) = _mm256_xor_si256((b), (c));              \
    (b) = BENTO_CRYPTO_AVX2_ROTL12(b);             \
    (a) = _mm256_add_epi32((a), (b));              \
    (d) = _mm256_xor_si256((d), (a));              \
    (d) = BENTO_CRYPTO_AVX2_ROTL8(d);              \
    (c) = _mm256_add_epi32((c), (d));              \
    (b) = _mm256_xor_si256((b), (c));              \
    (b) = BENTO_CRYPTO_AVX2_ROTL7(b)

#define BENTO_CRYPTO_AVX512_QR(a, b, c, d)                    \
    (a) = _mm512_add_epi32((a), (b));                         \
    (d) = _mm512_rol_epi32(_mm512_xor_si512((d), (a)), 16);   \
    (c) = _mm512_add_epi32((c), (d));                         \
    (b) = _mm512_rol_epi32(_mm512_xor_si512((b), (c)), 12);   \
    (a) = _mm512_add_epi32((a), (b));                         \
    (d) = _mm512_rol_epi32(_mm512_xor_si512((d), (a)), 8);    \
    (c) = _mm512_add_epi32((c), (d));                         \
    (b) = _mm512_rol_epi32(_mm512_xor_si512((b), (c)), 7)

/// @brief 8×8 的 32-bit 转置（AVX2）：行主序 -> 列主序。
/// @details 三段 unpack + permute2x128 网络，共 24 条指令，原地覆盖。
BENTO_CRYPTO_TARGET_AVX2
inline void ChaCha20Transpose8(__m256i v[8]) noexcept {
    const __m256i t0 = _mm256_unpacklo_epi32(v[0], v[1]);
    const __m256i t1 = _mm256_unpackhi_epi32(v[0], v[1]);
    const __m256i t2 = _mm256_unpacklo_epi32(v[2], v[3]);
    const __m256i t3 = _mm256_unpackhi_epi32(v[2], v[3]);
    const __m256i t4 = _mm256_unpacklo_epi32(v[4], v[5]);
    const __m256i t5 = _mm256_unpackhi_epi32(v[4], v[5]);
    const __m256i t6 = _mm256_unpacklo_epi32(v[6], v[7]);
    const __m256i t7 = _mm256_unpackhi_epi32(v[6], v[7]);
    const __m256i s0 = _mm256_unpacklo_epi64(t0, t2);
    const __m256i s1 = _mm256_unpackhi_epi64(t0, t2);
    const __m256i s2 = _mm256_unpacklo_epi64(t1, t3);
    const __m256i s3 = _mm256_unpackhi_epi64(t1, t3);
    const __m256i s4 = _mm256_unpacklo_epi64(t4, t6);
    const __m256i s5 = _mm256_unpackhi_epi64(t4, t6);
    const __m256i s6 = _mm256_unpacklo_epi64(t5, t7);
    const __m256i s7 = _mm256_unpackhi_epi64(t5, t7);
    v[0] = _mm256_permute2x128_si256(s0, s4, 0x20);
    v[1] = _mm256_permute2x128_si256(s1, s5, 0x20);
    v[2] = _mm256_permute2x128_si256(s2, s6, 0x20);
    v[3] = _mm256_permute2x128_si256(s3, s7, 0x20);
    v[4] = _mm256_permute2x128_si256(s0, s4, 0x31);
    v[5] = _mm256_permute2x128_si256(s1, s5, 0x31);
    v[6] = _mm256_permute2x128_si256(s2, s6, 0x31);
    v[7] = _mm256_permute2x128_si256(s3, s7, 0x31);
}

/// @brief 16×16 的 32-bit 转置（AVX-512）：行主序 -> 列主序。
/// @details 4 组 128-bit lane 内 4×4 转置（32 条）+ 4 组 lane 级 4×4
///          转置（32 条），共 64 条指令，原地覆盖。
BENTO_CRYPTO_TARGET_AVX512
inline void ChaCha20Transpose16(__m512i v[16]) noexcept {
    __m512i a0 = _mm512_unpacklo_epi32(v[0], v[1]);
    __m512i a1 = _mm512_unpackhi_epi32(v[0], v[1]);
    __m512i a2 = _mm512_unpacklo_epi32(v[2], v[3]);
    __m512i a3 = _mm512_unpackhi_epi32(v[2], v[3]);
    __m512i z0 = _mm512_unpacklo_epi64(a0, a2);
    __m512i z1 = _mm512_unpackhi_epi64(a0, a2);
    __m512i z2 = _mm512_unpacklo_epi64(a1, a3);
    __m512i z3 = _mm512_unpackhi_epi64(a1, a3);
    a0 = _mm512_unpacklo_epi32(v[4], v[5]);
    a1 = _mm512_unpackhi_epi32(v[4], v[5]);
    a2 = _mm512_unpacklo_epi32(v[6], v[7]);
    a3 = _mm512_unpackhi_epi32(v[6], v[7]);
    __m512i z4 = _mm512_unpacklo_epi64(a0, a2);
    __m512i z5 = _mm512_unpackhi_epi64(a0, a2);
    __m512i z6 = _mm512_unpacklo_epi64(a1, a3);
    __m512i z7 = _mm512_unpackhi_epi64(a1, a3);
    a0 = _mm512_unpacklo_epi32(v[8], v[9]);
    a1 = _mm512_unpackhi_epi32(v[8], v[9]);
    a2 = _mm512_unpacklo_epi32(v[10], v[11]);
    a3 = _mm512_unpackhi_epi32(v[10], v[11]);
    __m512i z8 = _mm512_unpacklo_epi64(a0, a2);
    __m512i z9 = _mm512_unpackhi_epi64(a0, a2);
    __m512i z10 = _mm512_unpacklo_epi64(a1, a3);
    __m512i z11 = _mm512_unpackhi_epi64(a1, a3);
    a0 = _mm512_unpacklo_epi32(v[12], v[13]);
    a1 = _mm512_unpackhi_epi32(v[12], v[13]);
    a2 = _mm512_unpacklo_epi32(v[14], v[15]);
    a3 = _mm512_unpackhi_epi32(v[14], v[15]);
    __m512i z12 = _mm512_unpacklo_epi64(a0, a2);
    __m512i z13 = _mm512_unpackhi_epi64(a0, a2);
    __m512i z14 = _mm512_unpacklo_epi64(a1, a3);
    __m512i z15 = _mm512_unpackhi_epi64(a1, a3);

    const __m512i b0 = _mm512_shuffle_i32x4(z0, z4, 0x44);
    const __m512i b1 = _mm512_shuffle_i32x4(z0, z4, 0xEE);
    const __m512i b2 = _mm512_shuffle_i32x4(z8, z12, 0x44);
    const __m512i b3 = _mm512_shuffle_i32x4(z8, z12, 0xEE);
    v[0] = _mm512_shuffle_i32x4(b0, b2, 0x88);
    v[4] = _mm512_shuffle_i32x4(b0, b2, 0xDD);
    v[8] = _mm512_shuffle_i32x4(b1, b3, 0x88);
    v[12] = _mm512_shuffle_i32x4(b1, b3, 0xDD);

    const __m512i c0 = _mm512_shuffle_i32x4(z1, z5, 0x44);
    const __m512i c1 = _mm512_shuffle_i32x4(z1, z5, 0xEE);
    const __m512i c2 = _mm512_shuffle_i32x4(z9, z13, 0x44);
    const __m512i c3 = _mm512_shuffle_i32x4(z9, z13, 0xEE);
    v[1] = _mm512_shuffle_i32x4(c0, c2, 0x88);
    v[5] = _mm512_shuffle_i32x4(c0, c2, 0xDD);
    v[9] = _mm512_shuffle_i32x4(c1, c3, 0x88);
    v[13] = _mm512_shuffle_i32x4(c1, c3, 0xDD);

    const __m512i d0 = _mm512_shuffle_i32x4(z2, z6, 0x44);
    const __m512i d1 = _mm512_shuffle_i32x4(z2, z6, 0xEE);
    const __m512i d2 = _mm512_shuffle_i32x4(z10, z14, 0x44);
    const __m512i d3 = _mm512_shuffle_i32x4(z10, z14, 0xEE);
    v[2] = _mm512_shuffle_i32x4(d0, d2, 0x88);
    v[6] = _mm512_shuffle_i32x4(d0, d2, 0xDD);
    v[10] = _mm512_shuffle_i32x4(d1, d3, 0x88);
    v[14] = _mm512_shuffle_i32x4(d1, d3, 0xDD);

    const __m512i e0 = _mm512_shuffle_i32x4(z3, z7, 0x44);
    const __m512i e1 = _mm512_shuffle_i32x4(z3, z7, 0xEE);
    const __m512i e2 = _mm512_shuffle_i32x4(z11, z15, 0x44);
    const __m512i e3 = _mm512_shuffle_i32x4(z11, z15, 0xEE);
    v[3] = _mm512_shuffle_i32x4(e0, e2, 0x88);
    v[7] = _mm512_shuffle_i32x4(e0, e2, 0xDD);
    v[11] = _mm512_shuffle_i32x4(e1, e3, 0x88);
    v[15] = _mm512_shuffle_i32x4(e1, e3, 0xDD);
}

/// @brief 8 块（512B）一批的 ChaCha20 异或（AVX2）。
/// @return 实际处理的字节数（8×64 的整数倍）。
BENTO_CRYPTO_TARGET_AVX2
std::size_t ChaCha20XorAvx2(const std::uint32_t state[16], std::uint32_t counter,
                            const std::uint8_t* input, std::uint8_t* output,
                            std::size_t bytes) noexcept {
    const __m256i rotate16 = _mm256_setr_epi8(
        2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13,
        2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13);
    const __m256i rotate8 = _mm256_setr_epi8(
        3, 0, 1, 2, 7, 4, 5, 6, 11, 8, 9, 10, 15, 12, 13, 14,
        3, 0, 1, 2, 7, 4, 5, 6, 11, 8, 9, 10, 15, 12, 13, 14);
    const __m256i lane_ids = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);

    std::size_t offset = 0;
    while (bytes - offset >= 8 * kChaChaBlockBytes) {
        const __m256i counter_vec = _mm256_add_epi32(
            _mm256_set1_epi32(static_cast<int>(counter +
                                               static_cast<std::uint32_t>(offset / kChaChaBlockBytes))),
            lane_ids);
        __m256i x[16];
        x[0] = _mm256_set1_epi32(0x61707865);
        x[1] = _mm256_set1_epi32(0x3320646e);
        x[2] = _mm256_set1_epi32(0x79622d32);
        x[3] = _mm256_set1_epi32(0x6b206574);
        for (int i = 0; i < 8; ++i) {
            x[4 + i] = _mm256_set1_epi32(static_cast<int>(state[4 + i]));
        }
        x[12] = counter_vec;
        x[13] = _mm256_set1_epi32(static_cast<int>(state[13]));
        x[14] = _mm256_set1_epi32(static_cast<int>(state[14]));
        x[15] = _mm256_set1_epi32(static_cast<int>(state[15]));

        for (int round = 0; round < 10; ++round) {
            BENTO_CRYPTO_AVX2_QR(x[0], x[4], x[8], x[12]);
            BENTO_CRYPTO_AVX2_QR(x[1], x[5], x[9], x[13]);
            BENTO_CRYPTO_AVX2_QR(x[2], x[6], x[10], x[14]);
            BENTO_CRYPTO_AVX2_QR(x[3], x[7], x[11], x[15]);
            BENTO_CRYPTO_AVX2_QR(x[0], x[5], x[10], x[15]);
            BENTO_CRYPTO_AVX2_QR(x[1], x[6], x[11], x[12]);
            BENTO_CRYPTO_AVX2_QR(x[2], x[7], x[8], x[13]);
            BENTO_CRYPTO_AVX2_QR(x[3], x[4], x[9], x[14]);
        }
        x[0] = _mm256_add_epi32(x[0], _mm256_set1_epi32(0x61707865));
        x[1] = _mm256_add_epi32(x[1], _mm256_set1_epi32(0x3320646e));
        x[2] = _mm256_add_epi32(x[2], _mm256_set1_epi32(0x79622d32));
        x[3] = _mm256_add_epi32(x[3], _mm256_set1_epi32(0x6b206574));
        for (int i = 0; i < 8; ++i) {
            x[4 + i] = _mm256_add_epi32(x[4 + i],
                                        _mm256_set1_epi32(static_cast<int>(state[4 + i])));
        }
        x[12] = _mm256_add_epi32(x[12], counter_vec);
        x[13] = _mm256_add_epi32(x[13], _mm256_set1_epi32(static_cast<int>(state[13])));
        x[14] = _mm256_add_epi32(x[14], _mm256_set1_epi32(static_cast<int>(state[14])));
        x[15] = _mm256_add_epi32(x[15], _mm256_set1_epi32(static_cast<int>(state[15])));

        ChaCha20Transpose8(x);
        ChaCha20Transpose8(x + 8);
        for (int lane = 0; lane < 8; ++lane) {
            const std::size_t at = offset + static_cast<std::size_t>(lane) * kChaChaBlockBytes;
            const __m256i in_lo =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input + at));
            const __m256i in_hi =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input + at + 32));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + at),
                                _mm256_xor_si256(x[lane], in_lo));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + at + 32),
                                _mm256_xor_si256(x[8 + lane], in_hi));
        }
        offset += 8 * kChaChaBlockBytes;
    }
    return offset;
}

/// @brief 16 块（1024B）一批的 ChaCha20 异或（AVX-512）。
/// @return 实际处理的字节数（16×64 的整数倍）。
BENTO_CRYPTO_TARGET_AVX512
std::size_t ChaCha20XorAvx512(const std::uint32_t state[16], std::uint32_t counter,
                              const std::uint8_t* input, std::uint8_t* output,
                              std::size_t bytes) noexcept {
    const __m512i lane_ids = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                               14, 15);
    std::size_t offset = 0;
    while (bytes - offset >= 16 * kChaChaBlockBytes) {
        const __m512i counter_vec = _mm512_add_epi32(
            _mm512_set1_epi32(static_cast<int>(counter +
                                               static_cast<std::uint32_t>(offset / kChaChaBlockBytes))),
            lane_ids);
        __m512i x[16];
        x[0] = _mm512_set1_epi32(0x61707865);
        x[1] = _mm512_set1_epi32(0x3320646e);
        x[2] = _mm512_set1_epi32(0x79622d32);
        x[3] = _mm512_set1_epi32(0x6b206574);
        for (int i = 0; i < 8; ++i) {
            x[4 + i] = _mm512_set1_epi32(static_cast<int>(state[4 + i]));
        }
        x[12] = counter_vec;
        x[13] = _mm512_set1_epi32(static_cast<int>(state[13]));
        x[14] = _mm512_set1_epi32(static_cast<int>(state[14]));
        x[15] = _mm512_set1_epi32(static_cast<int>(state[15]));

        for (int round = 0; round < 10; ++round) {
            BENTO_CRYPTO_AVX512_QR(x[0], x[4], x[8], x[12]);
            BENTO_CRYPTO_AVX512_QR(x[1], x[5], x[9], x[13]);
            BENTO_CRYPTO_AVX512_QR(x[2], x[6], x[10], x[14]);
            BENTO_CRYPTO_AVX512_QR(x[3], x[7], x[11], x[15]);
            BENTO_CRYPTO_AVX512_QR(x[0], x[5], x[10], x[15]);
            BENTO_CRYPTO_AVX512_QR(x[1], x[6], x[11], x[12]);
            BENTO_CRYPTO_AVX512_QR(x[2], x[7], x[8], x[13]);
            BENTO_CRYPTO_AVX512_QR(x[3], x[4], x[9], x[14]);
        }
        x[0] = _mm512_add_epi32(x[0], _mm512_set1_epi32(0x61707865));
        x[1] = _mm512_add_epi32(x[1], _mm512_set1_epi32(0x3320646e));
        x[2] = _mm512_add_epi32(x[2], _mm512_set1_epi32(0x79622d32));
        x[3] = _mm512_add_epi32(x[3], _mm512_set1_epi32(0x6b206574));
        for (int i = 0; i < 8; ++i) {
            x[4 + i] = _mm512_add_epi32(x[4 + i],
                                        _mm512_set1_epi32(static_cast<int>(state[4 + i])));
        }
        x[12] = _mm512_add_epi32(x[12], counter_vec);
        x[13] = _mm512_add_epi32(x[13], _mm512_set1_epi32(static_cast<int>(state[13])));
        x[14] = _mm512_add_epi32(x[14], _mm512_set1_epi32(static_cast<int>(state[14])));
        x[15] = _mm512_add_epi32(x[15], _mm512_set1_epi32(static_cast<int>(state[15])));

        ChaCha20Transpose16(x);
        for (int lane = 0; lane < 16; ++lane) {
            const std::size_t at = offset + static_cast<std::size_t>(lane) * kChaChaBlockBytes;
            const __m512i in = _mm512_loadu_si512(input + at);
            _mm512_storeu_si512(output + at, _mm512_xor_si512(x[lane], in));
        }
        offset += 16 * kChaChaBlockBytes;
    }
    return offset;
}

// ---------------------------------------------------------------------------
// 多路 SIMD Poly1305（AVX2 4 lane / AVX-512 8 lane，radix 2^26 × 5 limb）
// ---------------------------------------------------------------------------
//
// 串行 Poly1305 的硬下限是「每 16B 一次乘→进位」的强依赖：标量 radix-2^44
// 已经压到 21 周期/块（约 3 GB/s），再抠指令数也突破不了单块串行链。
//
// 多路化利用的是展开恒等式
//
//   ((h+m0)·r + m1)·r + … + m_{N-1})·r
//     = (h+m0)·r^N + m1·r^{N-1} + … + m_{N-1}·r^1
//
// 右端 N 个部分积**互相独立**：lane k 负责 (h·[k==0] + m_k)·r^{N-k}，N 个
// 乘法在各自 lane 内并行累加，最后跨 lane 求和并做**一次** 26 位进位链。
// 每 N 块只剩一条串行进位链，瓶颈从依赖链变成吞吐。
//
// 为什么用 radix 2^26（5 limb）而不是标量路径的 radix 2^44（3 limb）：
// `vpmuludq` 是 32×32→64 的无符号乘，44 位 limb 放不进 32 位操作数。
// 26 位 limb 下 5 项累加上界 5×5×2^52 < 2^57，64 位累加器不溢出。
// 两条路径只在批次边界互换 44 位/26 位表示，剩余不足 N 块的尾部仍走标量，
// 结果按 mod (2^130-5) 相等。
//
// 代价：每批一次跨 lane 水平求和（4 lane 折叠 5 条、8 lane 折叠 6 条向量
// 指令）与一次表示转换，摊到 N 块上约 1–2 条/块。
#if BENTO_POLY_MULTI_SIMD

/// 26 位 limb 掩码；5×26 = 130 位。
constexpr std::uint32_t kLimb26Mask = (std::uint32_t{1} << 26) - 1;
/// 完整 16B 块在 bit 128 的隐含位，折算到 limb4（bit 104 起）是 2^24。
constexpr std::uint32_t kLimb26Hibit = std::uint32_t{1} << 24;

/// @brief 把 3×44 位（radix 2^44）表示的 130 位数拆成 5×26 位。
/// @param in [in] 3 个 limb：低 44/44/42 位（调用方保证不越界）
/// @param out [out] 5 个 26 位 limb，写入后每项 < 2^26
void Poly1305WideTo26(const std::uint64_t in[3], std::uint32_t out[5]) noexcept {
    out[0] = static_cast<std::uint32_t>(in[0]) & kLimb26Mask;
    out[1] = static_cast<std::uint32_t>((in[0] >> 26) | (in[1] << 18)) & kLimb26Mask;
    out[2] = static_cast<std::uint32_t>(in[1] >> 8) & kLimb26Mask;
    out[3] = static_cast<std::uint32_t>((in[1] >> 34) | (in[2] << 10)) & kLimb26Mask;
    out[4] = static_cast<std::uint32_t>(in[2] >> 16) & kLimb26Mask;
}

/// @brief 把 5×26 位重新打包回 3×44 位，交给标量 Poly1305Finish。
/// @param in [in] 5 个 26 位 limb；每项 < 2^27 都能正确打包（多出的高位自然
///        落到相邻 limb 的合法位上）
/// @param out [out] 3 个 limb：低 44/44/42 位
void Poly1305WideFrom26(const std::uint32_t in[5], std::uint64_t out[3]) noexcept {
    out[0] = static_cast<std::uint64_t>(in[0]) |
             (static_cast<std::uint64_t>(in[1] & 0x3ffffU) << 26);
    out[1] = static_cast<std::uint64_t>(in[1] >> 18) |
             (static_cast<std::uint64_t>(in[2]) << 8) |
             (static_cast<std::uint64_t>(in[3] & 0x3ffU) << 34);
    out[2] = static_cast<std::uint64_t>(in[3] >> 10) |
             (static_cast<std::uint64_t>(in[4]) << 16);
}

/// @brief 26 位 5-limb 的模 (2^130-5) 乘法（poly1305-donna 结构）。
/// @param a [in] 被乘数，5 个 limb（可略超 26 位）
/// @param b [in] 乘数，5 个 limb（每项 < 2^27）
/// @param out [out] 乘积，5 个 limb，每项 < 2^26
/// @details 跨 limb 折回乘 5（2^130 ≡ 5）；d_i 上界 5×5×2^52 < 2^57，
///          64 位累加器不溢出。只用于 r 的幂预计算，不在热路径里。
void Poly1305Mul26(const std::uint32_t a[5], const std::uint32_t b[5],
                   std::uint32_t out[5]) noexcept {
    const std::uint64_t s1 = static_cast<std::uint64_t>(b[1]) * 5U;
    const std::uint64_t s2 = static_cast<std::uint64_t>(b[2]) * 5U;
    const std::uint64_t s3 = static_cast<std::uint64_t>(b[3]) * 5U;
    const std::uint64_t s4 = static_cast<std::uint64_t>(b[4]) * 5U;
    std::uint64_t d0 = static_cast<std::uint64_t>(a[0]) * b[0] +
                       static_cast<std::uint64_t>(a[1]) * s4 +
                       static_cast<std::uint64_t>(a[2]) * s3 +
                       static_cast<std::uint64_t>(a[3]) * s2 +
                       static_cast<std::uint64_t>(a[4]) * s1;
    std::uint64_t d1 = static_cast<std::uint64_t>(a[0]) * b[1] +
                       static_cast<std::uint64_t>(a[1]) * b[0] +
                       static_cast<std::uint64_t>(a[2]) * s4 +
                       static_cast<std::uint64_t>(a[3]) * s3 +
                       static_cast<std::uint64_t>(a[4]) * s2;
    std::uint64_t d2 = static_cast<std::uint64_t>(a[0]) * b[2] +
                       static_cast<std::uint64_t>(a[1]) * b[1] +
                       static_cast<std::uint64_t>(a[2]) * b[0] +
                       static_cast<std::uint64_t>(a[3]) * s4 +
                       static_cast<std::uint64_t>(a[4]) * s3;
    std::uint64_t d3 = static_cast<std::uint64_t>(a[0]) * b[3] +
                       static_cast<std::uint64_t>(a[1]) * b[2] +
                       static_cast<std::uint64_t>(a[2]) * b[1] +
                       static_cast<std::uint64_t>(a[3]) * b[0] +
                       static_cast<std::uint64_t>(a[4]) * s4;
    std::uint64_t d4 = static_cast<std::uint64_t>(a[0]) * b[4] +
                       static_cast<std::uint64_t>(a[1]) * b[3] +
                       static_cast<std::uint64_t>(a[2]) * b[2] +
                       static_cast<std::uint64_t>(a[3]) * b[1] +
                       static_cast<std::uint64_t>(a[4]) * b[0];
    std::uint64_t carry = d0 >> 26;
    out[0] = static_cast<std::uint32_t>(d0) & kLimb26Mask;
    d1 += carry;
    carry = d1 >> 26;
    out[1] = static_cast<std::uint32_t>(d1) & kLimb26Mask;
    d2 += carry;
    carry = d2 >> 26;
    out[2] = static_cast<std::uint32_t>(d2) & kLimb26Mask;
    d3 += carry;
    carry = d3 >> 26;
    out[3] = static_cast<std::uint32_t>(d3) & kLimb26Mask;
    d4 += carry;
    carry = d4 >> 26;
    out[4] = static_cast<std::uint32_t>(d4) & kLimb26Mask;
    std::uint64_t fold = static_cast<std::uint64_t>(out[0]) + carry * 5U;
    carry = fold >> 26;
    out[0] = static_cast<std::uint32_t>(fold) & kLimb26Mask;
    out[1] = static_cast<std::uint32_t>(out[1] + carry);
}

/// @brief 4 个 lane 的 64 位水平求和（AVX2 4 路每批一次的跨 lane 归并）。
BENTO_CRYPTO_TARGET_AVX2
inline std::uint64_t Poly1305Sum4x64(__m256i value) noexcept {
    const __m128i low = _mm256_castsi256_si128(value);
    const __m128i high = _mm256_extracti128_si256(value, 1);
    __m128i sum = _mm_add_epi64(low, high);
    sum = _mm_add_epi64(sum, _mm_unpackhi_epi64(sum, sum));
    return static_cast<std::uint64_t>(_mm_cvtsi128_si64(sum));
}

/// @brief 8 个 lane 的 64 位水平求和（AVX-512 8 路每批一次的跨 lane 归并）。
/// @details 两级 128 位块交换：256 位半互换 → 相邻 128 位互换，使 8 个 lane
///          的同一份和落在每个 128 位块的两个 64 位槽上，取最低槽即可。
BENTO_CRYPTO_TARGET_AVX512
inline std::uint64_t Poly1305Sum8x64(__m512i value) noexcept {
    // 第一级：交换 256 位半；第二级：交换相邻 128 位块。两级之后每个 128 位
    // lane 的两个槽分别装“偶数下标和”与“奇数下标和”，再合并一次才是总和。
    __m512i sum = _mm512_add_epi64(value, _mm512_shuffle_i64x2(value, value, 0x4E));
    sum = _mm512_add_epi64(sum, _mm512_shuffle_i64x2(sum, sum, 0xB1));
    const __m128i low = _mm512_castsi512_si128(sum);
    return static_cast<std::uint64_t>(
        _mm_cvtsi128_si64(_mm_add_epi64(low, _mm_unpackhi_epi64(low, low))));
}

/// @brief 4 路（每批 4×16B）Poly1305 吸收（AVX2）。
/// @param h [in,out] radix-2^44 的 3 limb 累积值；成功批次后推进
/// @param r [in] radix-2^44 的 3 limb 密钥（Poly1305Init 已 clamp）
/// @param data [in] 连续 16B 块；不可为 nullptr
/// @param bytes [in] 可用字节数（不足 4 块时本函数不消费）
/// @return 已消费字节数，恒为 64 的整数倍；调用方必须从 data+返回值 续处理
BENTO_CRYPTO_TARGET_AVX2
std::size_t Poly1305BlocksAvx2(std::uint64_t h[3], const std::uint64_t r[3],
                               const std::uint8_t* data, std::size_t bytes) noexcept {
    const std::size_t batches = bytes / (4 * 16);
    if (batches == 0) {
        return 0;
    }
    // step1: r^1..r^4 的 26 位 limb。lane k 用 r^(4-k)，把 4 块的串行链展开成
    //        4 个独立乘；这一步每批只算一次，摊到 4 块上不足 1 条/块。
    std::uint32_t rp[4][5];
    Poly1305WideTo26(r, rp[0]);
    for (int k = 1; k < 4; ++k) {
        Poly1305Mul26(rp[k - 1], rp[0], rp[k]);
    }
    const __m256i rv0 = _mm256_setr_epi64x(rp[3][0], rp[2][0], rp[1][0], rp[0][0]);
    const __m256i rv1 = _mm256_setr_epi64x(rp[3][1], rp[2][1], rp[1][1], rp[0][1]);
    const __m256i rv2 = _mm256_setr_epi64x(rp[3][2], rp[2][2], rp[1][2], rp[0][2]);
    const __m256i rv3 = _mm256_setr_epi64x(rp[3][3], rp[2][3], rp[1][3], rp[0][3]);
    const __m256i rv4 = _mm256_setr_epi64x(rp[3][4], rp[2][4], rp[1][4], rp[0][4]);
    const __m256i sv1 = _mm256_setr_epi64x(rp[3][1] * 5U, rp[2][1] * 5U, rp[1][1] * 5U,
                                           rp[0][1] * 5U);
    const __m256i sv2 = _mm256_setr_epi64x(rp[3][2] * 5U, rp[2][2] * 5U, rp[1][2] * 5U,
                                           rp[0][2] * 5U);
    const __m256i sv3 = _mm256_setr_epi64x(rp[3][3] * 5U, rp[2][3] * 5U, rp[1][3] * 5U,
                                           rp[0][3] * 5U);
    const __m256i sv4 = _mm256_setr_epi64x(rp[3][4] * 5U, rp[2][4] * 5U, rp[1][4] * 5U,
                                           rp[0][4] * 5U);
    const __m256i mask26 = _mm256_set1_epi64x(kLimb26Mask);
    const __m256i hibit = _mm256_set1_epi64x(kLimb26Hibit);
    std::uint32_t acc[5];
    Poly1305WideTo26(h, acc);
    std::size_t offset = 0;
    // step2: 每批加载 4 块，用 2 条 unpack + 2 条 permute 把 lo/hi 两列转置成
    //        lane 主序（lane k = 块 k），再按 26 位切 5 个 limb。
    for (std::size_t batch = 0; batch < batches; ++batch) {
        const __m256i z0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + offset));
        const __m256i z1 =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + offset + 32));
        const __m256i lo = _mm256_permute4x64_epi64(_mm256_unpacklo_epi64(z0, z1), 0xD8);
        const __m256i hi = _mm256_permute4x64_epi64(_mm256_unpackhi_epi64(z0, z1), 0xD8);
        // step3: 5 个 limb；lane 0 额外带上累积值 h（其余 lane 为 0），这样
        //        lane 0 的部分积就是 (h+m0)·r^4。
        const __m256i h0 = _mm256_add_epi64(_mm256_and_si256(lo, mask26),
                                            _mm256_setr_epi64x(acc[0], 0, 0, 0));
        const __m256i h1 =
            _mm256_add_epi64(_mm256_and_si256(_mm256_srli_epi64(lo, 26), mask26),
                             _mm256_setr_epi64x(acc[1], 0, 0, 0));
        const __m256i h2 =
            _mm256_add_epi64(_mm256_and_si256(
                                 _mm256_or_si256(_mm256_srli_epi64(lo, 52),
                                                 _mm256_slli_epi64(hi, 12)),
                                 mask26),
                             _mm256_setr_epi64x(acc[2], 0, 0, 0));
        const __m256i h3 =
            _mm256_add_epi64(_mm256_and_si256(_mm256_srli_epi64(hi, 14), mask26),
                             _mm256_setr_epi64x(acc[3], 0, 0, 0));
        const __m256i h4 =
            _mm256_add_epi64(_mm256_or_si256(_mm256_srli_epi64(hi, 40), hibit),
                             _mm256_setr_epi64x(acc[4], 0, 0, 0));
        // step4: 5×5 卷积（25 条 vpmuludq + 20 条 vpaddq），4 个 lane 互不依赖。
        __m256i d0 = _mm256_mul_epu32(h0, rv0);
        d0 = _mm256_add_epi64(d0, _mm256_mul_epu32(h1, sv4));
        d0 = _mm256_add_epi64(d0, _mm256_mul_epu32(h2, sv3));
        d0 = _mm256_add_epi64(d0, _mm256_mul_epu32(h3, sv2));
        d0 = _mm256_add_epi64(d0, _mm256_mul_epu32(h4, sv1));
        __m256i d1 = _mm256_mul_epu32(h0, rv1);
        d1 = _mm256_add_epi64(d1, _mm256_mul_epu32(h1, rv0));
        d1 = _mm256_add_epi64(d1, _mm256_mul_epu32(h2, sv4));
        d1 = _mm256_add_epi64(d1, _mm256_mul_epu32(h3, sv3));
        d1 = _mm256_add_epi64(d1, _mm256_mul_epu32(h4, sv2));
        __m256i d2 = _mm256_mul_epu32(h0, rv2);
        d2 = _mm256_add_epi64(d2, _mm256_mul_epu32(h1, rv1));
        d2 = _mm256_add_epi64(d2, _mm256_mul_epu32(h2, rv0));
        d2 = _mm256_add_epi64(d2, _mm256_mul_epu32(h3, sv4));
        d2 = _mm256_add_epi64(d2, _mm256_mul_epu32(h4, sv3));
        __m256i d3 = _mm256_mul_epu32(h0, rv3);
        d3 = _mm256_add_epi64(d3, _mm256_mul_epu32(h1, rv2));
        d3 = _mm256_add_epi64(d3, _mm256_mul_epu32(h2, rv1));
        d3 = _mm256_add_epi64(d3, _mm256_mul_epu32(h3, rv0));
        d3 = _mm256_add_epi64(d3, _mm256_mul_epu32(h4, sv4));
        __m256i d4 = _mm256_mul_epu32(h0, rv4);
        d4 = _mm256_add_epi64(d4, _mm256_mul_epu32(h1, rv3));
        d4 = _mm256_add_epi64(d4, _mm256_mul_epu32(h2, rv2));
        d4 = _mm256_add_epi64(d4, _mm256_mul_epu32(h3, rv1));
        d4 = _mm256_add_epi64(d4, _mm256_mul_epu32(h4, rv0));
        // step5: 跨 lane 求和 → 一条 26 位进位链。进位链只有 5 段，且每批
        //        （64B）才走一次，不再是每块一次。
        std::uint64_t s0 = Poly1305Sum4x64(d0);
        std::uint64_t s1 = Poly1305Sum4x64(d1);
        std::uint64_t s2 = Poly1305Sum4x64(d2);
        std::uint64_t s3 = Poly1305Sum4x64(d3);
        std::uint64_t s4 = Poly1305Sum4x64(d4);
        std::uint64_t carry = s0 >> 26;
        acc[0] = static_cast<std::uint32_t>(s0) & kLimb26Mask;
        s1 += carry;
        carry = s1 >> 26;
        acc[1] = static_cast<std::uint32_t>(s1) & kLimb26Mask;
        s2 += carry;
        carry = s2 >> 26;
        acc[2] = static_cast<std::uint32_t>(s2) & kLimb26Mask;
        s3 += carry;
        carry = s3 >> 26;
        acc[3] = static_cast<std::uint32_t>(s3) & kLimb26Mask;
        s4 += carry;
        carry = s4 >> 26;
        acc[4] = static_cast<std::uint32_t>(s4) & kLimb26Mask;
        acc[0] += static_cast<std::uint32_t>(carry) * 5U;
        carry = acc[0] >> 26;
        acc[0] &= kLimb26Mask;
        acc[1] += static_cast<std::uint32_t>(carry);
        offset += 4 * 16;
    }
    // step6: 批内是弱归约（允许 limb 略超 26 位），打包回 44 位前做满进位链。
    std::uint64_t carry = static_cast<std::uint64_t>(acc[1]) >> 26;
    acc[1] &= kLimb26Mask;
    std::uint64_t limb2 = static_cast<std::uint64_t>(acc[2]) + carry;
    carry = limb2 >> 26;
    acc[2] = static_cast<std::uint32_t>(limb2) & kLimb26Mask;
    std::uint64_t limb3 = static_cast<std::uint64_t>(acc[3]) + carry;
    carry = limb3 >> 26;
    acc[3] = static_cast<std::uint32_t>(limb3) & kLimb26Mask;
    std::uint64_t limb4 = static_cast<std::uint64_t>(acc[4]) + carry;
    carry = limb4 >> 26;
    acc[4] = static_cast<std::uint32_t>(limb4) & kLimb26Mask;
    std::uint64_t limb0 = static_cast<std::uint64_t>(acc[0]) + carry * 5U;
    carry = limb0 >> 26;
    acc[0] = static_cast<std::uint32_t>(limb0) & kLimb26Mask;
    acc[1] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(acc[1]) + carry);
    Poly1305WideFrom26(acc, h);
    return offset;
}

/// @brief 8 路（每批 8×16B）Poly1305 吸收（AVX-512）。
/// @param h [in,out] radix-2^44 的 3 limb 累积值；成功批次后推进
/// @param r [in] radix-2^44 的 3 limb 密钥（Poly1305Init 已 clamp）
/// @param data [in] 连续 16B 块；不可为 nullptr
/// @param bytes [in] 可用字节数（不足 8 块时本函数不消费）
/// @return 已消费字节数，恒为 128 的整数倍
/// @details 结构与 AVX2 4 路相同，lane 数翻倍到 8：每批 128B 的乘法仍是 25
///          条（每条 `vpmuludq` 覆盖 8 个 lane），归并与进位链次数减半，
///          因此每块摊销的卷积指令从 6.25 条降到 3.1 条。
BENTO_CRYPTO_TARGET_AVX512
std::size_t Poly1305BlocksAvx512(std::uint64_t h[3], const std::uint64_t r[3],
                                 const std::uint8_t* data, std::size_t bytes) noexcept {
    const std::size_t batches = bytes / (8 * 16);
    if (batches == 0) {
        return 0;
    }
    std::uint32_t rp[8][5];
    Poly1305WideTo26(r, rp[0]);
    for (int k = 1; k < 8; ++k) {
        Poly1305Mul26(rp[k - 1], rp[0], rp[k]);
    }
    __m512i rv[5];
    for (int j = 0; j < 5; ++j) {
        rv[j] = _mm512_setr_epi64(rp[7][j], rp[6][j], rp[5][j], rp[4][j], rp[3][j], rp[2][j],
                                  rp[1][j], rp[0][j]);
    }
    __m512i sv[5];
    for (int j = 1; j < 5; ++j) {
        sv[j] = _mm512_setr_epi64(rp[7][j] * 5U, rp[6][j] * 5U, rp[5][j] * 5U, rp[4][j] * 5U,
                                   rp[3][j] * 5U, rp[2][j] * 5U, rp[1][j] * 5U, rp[0][j] * 5U);
    }
    const __m512i mask26 = _mm512_set1_epi64(kLimb26Mask);
    const __m512i hibit = _mm512_set1_epi64(kLimb26Hibit);
    const __m512i order_lo = _mm512_setr_epi64(0, 2, 4, 6, 8, 10, 12, 14);
    const __m512i order_hi = _mm512_setr_epi64(1, 3, 5, 7, 9, 11, 13, 15);
    std::uint32_t acc[5];
    Poly1305WideTo26(h, acc);
    std::size_t offset = 0;
    for (std::size_t batch = 0; batch < batches; ++batch) {
        const __m512i z0 = _mm512_loadu_si512(data + offset);
        const __m512i z1 = _mm512_loadu_si512(data + offset + 64);
        // 两条 permutex2var 完成 8×2 的 lo/hi 转置（lane k = 块 k）。
        const __m512i lo = _mm512_permutex2var_epi64(z0, order_lo, z1);
        const __m512i hi = _mm512_permutex2var_epi64(z0, order_hi, z1);
        __m512i h0 = _mm512_add_epi64(_mm512_and_si512(lo, mask26),
                                      _mm512_maskz_set1_epi64(0x01, acc[0]));
        __m512i h1 = _mm512_add_epi64(_mm512_and_si512(_mm512_srli_epi64(lo, 26), mask26),
                                      _mm512_maskz_set1_epi64(0x01, acc[1]));
        __m512i h2 = _mm512_add_epi64(
            _mm512_and_si512(
                _mm512_or_si512(_mm512_srli_epi64(lo, 52), _mm512_slli_epi64(hi, 12)), mask26),
            _mm512_maskz_set1_epi64(0x01, acc[2]));
        __m512i h3 = _mm512_add_epi64(_mm512_and_si512(_mm512_srli_epi64(hi, 14), mask26),
                                      _mm512_maskz_set1_epi64(0x01, acc[3]));
        __m512i h4 = _mm512_add_epi64(_mm512_or_si512(_mm512_srli_epi64(hi, 40), hibit),
                                      _mm512_maskz_set1_epi64(0x01, acc[4]));
        __m512i d0 = _mm512_mul_epu32(h0, rv[0]);
        d0 = _mm512_add_epi64(d0, _mm512_mul_epu32(h1, sv[4]));
        d0 = _mm512_add_epi64(d0, _mm512_mul_epu32(h2, sv[3]));
        d0 = _mm512_add_epi64(d0, _mm512_mul_epu32(h3, sv[2]));
        d0 = _mm512_add_epi64(d0, _mm512_mul_epu32(h4, sv[1]));
        __m512i d1 = _mm512_mul_epu32(h0, rv[1]);
        d1 = _mm512_add_epi64(d1, _mm512_mul_epu32(h1, rv[0]));
        d1 = _mm512_add_epi64(d1, _mm512_mul_epu32(h2, sv[4]));
        d1 = _mm512_add_epi64(d1, _mm512_mul_epu32(h3, sv[3]));
        d1 = _mm512_add_epi64(d1, _mm512_mul_epu32(h4, sv[2]));
        __m512i d2 = _mm512_mul_epu32(h0, rv[2]);
        d2 = _mm512_add_epi64(d2, _mm512_mul_epu32(h1, rv[1]));
        d2 = _mm512_add_epi64(d2, _mm512_mul_epu32(h2, rv[0]));
        d2 = _mm512_add_epi64(d2, _mm512_mul_epu32(h3, sv[4]));
        d2 = _mm512_add_epi64(d2, _mm512_mul_epu32(h4, sv[3]));
        __m512i d3 = _mm512_mul_epu32(h0, rv[3]);
        d3 = _mm512_add_epi64(d3, _mm512_mul_epu32(h1, rv[2]));
        d3 = _mm512_add_epi64(d3, _mm512_mul_epu32(h2, rv[1]));
        d3 = _mm512_add_epi64(d3, _mm512_mul_epu32(h3, rv[0]));
        d3 = _mm512_add_epi64(d3, _mm512_mul_epu32(h4, sv[4]));
        __m512i d4 = _mm512_mul_epu32(h0, rv[4]);
        d4 = _mm512_add_epi64(d4, _mm512_mul_epu32(h1, rv[3]));
        d4 = _mm512_add_epi64(d4, _mm512_mul_epu32(h2, rv[2]));
        d4 = _mm512_add_epi64(d4, _mm512_mul_epu32(h3, rv[1]));
        d4 = _mm512_add_epi64(d4, _mm512_mul_epu32(h4, rv[0]));
        std::uint64_t s0 = Poly1305Sum8x64(d0);
        std::uint64_t s1 = Poly1305Sum8x64(d1);
        std::uint64_t s2 = Poly1305Sum8x64(d2);
        std::uint64_t s3 = Poly1305Sum8x64(d3);
        std::uint64_t s4 = Poly1305Sum8x64(d4);
        std::uint64_t carry = s0 >> 26;
        acc[0] = static_cast<std::uint32_t>(s0) & kLimb26Mask;
        s1 += carry;
        carry = s1 >> 26;
        acc[1] = static_cast<std::uint32_t>(s1) & kLimb26Mask;
        s2 += carry;
        carry = s2 >> 26;
        acc[2] = static_cast<std::uint32_t>(s2) & kLimb26Mask;
        s3 += carry;
        carry = s3 >> 26;
        acc[3] = static_cast<std::uint32_t>(s3) & kLimb26Mask;
        s4 += carry;
        carry = s4 >> 26;
        acc[4] = static_cast<std::uint32_t>(s4) & kLimb26Mask;
        acc[0] += static_cast<std::uint32_t>(carry) * 5U;
        carry = acc[0] >> 26;
        acc[0] &= kLimb26Mask;
        acc[1] += static_cast<std::uint32_t>(carry);
        offset += 8 * 16;
    }
    std::uint64_t carry = static_cast<std::uint64_t>(acc[1]) >> 26;
    acc[1] &= kLimb26Mask;
    std::uint64_t limb2 = static_cast<std::uint64_t>(acc[2]) + carry;
    carry = limb2 >> 26;
    acc[2] = static_cast<std::uint32_t>(limb2) & kLimb26Mask;
    std::uint64_t limb3 = static_cast<std::uint64_t>(acc[3]) + carry;
    carry = limb3 >> 26;
    acc[3] = static_cast<std::uint32_t>(limb3) & kLimb26Mask;
    std::uint64_t limb4 = static_cast<std::uint64_t>(acc[4]) + carry;
    carry = limb4 >> 26;
    acc[4] = static_cast<std::uint32_t>(limb4) & kLimb26Mask;
    std::uint64_t limb0 = static_cast<std::uint64_t>(acc[0]) + carry * 5U;
    carry = limb0 >> 26;
    acc[0] = static_cast<std::uint32_t>(limb0) & kLimb26Mask;
    acc[1] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(acc[1]) + carry);
    Poly1305WideFrom26(acc, h);
    return offset;
}

// ---------------------------------------------------------------------------
// AVX-512 IFMA 8 路 Poly1305（radix 2^44 × 3 limb，vpmadd52luq/huq）
// ---------------------------------------------------------------------------
//
// 2^26×5 版本受限于 `vpmuludq` 只能做 32×32→64：44 位 limb 放不进 32 位操作数，
// 只能把表示拆成 5 个 26 位 limb，每批 8 块要 25 条 mul + 20 条 add，跨 lane
// 归并 5 次、26 位进位链 5 段。IFMA 的 `vpmadd52luq/huq` 直接做 52×52→104 的
// 乘加，于是可以沿用标量路径的 radix-2^44 表示，每批只需 9 对乘加（18 条），
// 进位链 3 段，且**批次边界不需要 44/26 位表示转换**。
//
// 代价：一个 64 位累加器装不下 88 位的部分积，每个卷积结果 d 拆成
// lo（d mod 2^52）与 hi（d >> 52）两个累加器，跨 lane 归并从 5 次升到 6 次。
// 但每次归并只是 64 位加法（无 26 位掩码/进位），且归约每批 128B 才做一次。
//
// 上界（8 lane 求和后，单位 bit）：
//   h0,h1 < 2^45，h2 < 2^42；r0,r1 < 2^44，r2 < 2^42；20·r1 < 2^49，20·r2 < 2^47。
//   d0 = h0·r0 + h1·(20r2) + h2·(20r1) < 2^92 → lo 的 8 lane 和 < 2^55、
//   hi 的 8 lane 和 < 2^43，两者都远小于 2^64，不会溢出。

/// @brief radix-2^44 的 3-limb 模乘（只用于 r 的幂预计算，不在热路径）。
/// @param a [in] 3 limb，低 44/44/42 位
/// @param b [in] 3 limb，低 44/44/42 位
/// @param out [out] 3 limb：低 44/44/42 位（完整进位后；out[1] 允许略超 44 位）
void Poly1305Mul44(const std::uint64_t a[3], const std::uint64_t b[3],
                   std::uint64_t out[3]) noexcept {
    const std::uint64_t s1 = b[1] * 20U;
    const std::uint64_t s2 = b[2] * 20U;
    Poly1305Wide d0 = static_cast<Poly1305Wide>(a[0]) * b[0] +
                      static_cast<Poly1305Wide>(a[1]) * s2 +
                      static_cast<Poly1305Wide>(a[2]) * s1;
    Poly1305Wide d1 = static_cast<Poly1305Wide>(a[0]) * b[1] +
                      static_cast<Poly1305Wide>(a[1]) * b[0] +
                      static_cast<Poly1305Wide>(a[2]) * s2;
    Poly1305Wide d2 = static_cast<Poly1305Wide>(a[0]) * b[2] +
                      static_cast<Poly1305Wide>(a[1]) * b[1] +
                      static_cast<Poly1305Wide>(a[2]) * b[0];
    std::uint64_t carry = static_cast<std::uint64_t>(d0 >> 44);
    out[0] = static_cast<std::uint64_t>(d0) & kPoly1305Mask44;
    d1 += carry;
    carry = static_cast<std::uint64_t>(d1 >> 44);
    out[1] = static_cast<std::uint64_t>(d1) & kPoly1305Mask44;
    d2 += carry;
    carry = static_cast<std::uint64_t>(d2 >> 42);
    out[2] = static_cast<std::uint64_t>(d2) & kPoly1305Mask42;
    out[0] += carry * 5U;
    carry = out[0] >> 44;
    out[0] &= kPoly1305Mask44;
    out[1] += carry;
}

/// @brief 8 路（每批 8×16B）Poly1305 吸收（AVX-512 IFMA）。
/// @param h [in,out] radix-2^44 的 3 limb 累积值；成功批次后推进
/// @param r [in] radix-2^44 的 3 limb 密钥（Poly1305Init 已 clamp）
/// @param data [in] 连续 16B 块；不可为 nullptr
/// @param bytes [in] 可用字节数（不足 8 块时本函数不消费）
/// @return 已消费字节数，恒为 128 的整数倍
/// @details lane k 负责 `(h·[k==0] + m_k)·r^(8-k)`；8 个部分积在各自 lane 内
///          用 18 条 `vpmadd52` 累加，**每批一次**跨 lane 归并 + 3 段进位链。
/// @brief 8 lane 的 64 位水平求和（IFMA 路径专用）。
/// @param value [in] 8 个待求和 lane
/// @return 8 lane 之和
/// @details 与 Poly1305Sum8x64 相比少用两条 512 位 shuffle：先降到 256 位、
///          再降到 128 位，512 位指令只有 1 条 `vextracti64x4`，在 512 位
///          双泵的 Zen 4 上每次省约 4 个 μop（每批 6 次，约 20 μop）。
BENTO_CRYPTO_TARGET_AVX512IFMA
inline std::uint64_t Poly1305Sum8x64Ifma(__m512i value) noexcept {
    const __m256i half = _mm256_add_epi64(_mm512_castsi512_si256(value),
                                          _mm512_extracti64x4_epi64(value, 1));
    __m128i low = _mm_add_epi64(_mm256_castsi256_si128(half),
                                _mm256_extracti128_si256(half, 1));
    low = _mm_add_epi64(low, _mm_unpackhi_epi64(low, low));
    return static_cast<std::uint64_t>(_mm_cvtsi128_si64(low));
}

BENTO_CRYPTO_TARGET_AVX512IFMA
std::size_t Poly1305BlocksIfma(std::uint64_t h[3], const std::uint64_t r[3],
                               const std::uint8_t* data, std::size_t bytes) noexcept {
    const std::size_t batches = bytes / (8 * 16);
    if (batches == 0) {
        return 0;
    }
    // step1: r^1..r^8（radix 2^44 标量乘，每调用一次）；lane k 取 r^(8-k)。
    std::uint64_t rp[8][3];
    rp[0][0] = r[0];
    rp[0][1] = r[1];
    rp[0][2] = r[2];
    for (int k = 1; k < 8; ++k) {
        Poly1305Mul44(rp[k - 1], r, rp[k]);
    }
    __m512i rv0 = _mm512_setr_epi64(
        static_cast<long long>(rp[7][0]), static_cast<long long>(rp[6][0]),
        static_cast<long long>(rp[5][0]), static_cast<long long>(rp[4][0]),
        static_cast<long long>(rp[3][0]), static_cast<long long>(rp[2][0]),
        static_cast<long long>(rp[1][0]), static_cast<long long>(rp[0][0]));
    __m512i rv1 = _mm512_setr_epi64(
        static_cast<long long>(rp[7][1]), static_cast<long long>(rp[6][1]),
        static_cast<long long>(rp[5][1]), static_cast<long long>(rp[4][1]),
        static_cast<long long>(rp[3][1]), static_cast<long long>(rp[2][1]),
        static_cast<long long>(rp[1][1]), static_cast<long long>(rp[0][1]));
    __m512i rv2 = _mm512_setr_epi64(
        static_cast<long long>(rp[7][2]), static_cast<long long>(rp[6][2]),
        static_cast<long long>(rp[5][2]), static_cast<long long>(rp[4][2]),
        static_cast<long long>(rp[3][2]), static_cast<long long>(rp[2][2]),
        static_cast<long long>(rp[1][2]), static_cast<long long>(rp[0][2]));
    // 跨 limb 折回乘 20（2^132 ≡ 20 mod 2^130-5）；20·r1 < 2^49、20·r2 < 2^47，
    // 都落在 vpmadd52 的 52 位操作数范围内。
    __m512i sv1 = _mm512_setr_epi64(
        static_cast<long long>(rp[7][1] * 20U), static_cast<long long>(rp[6][1] * 20U),
        static_cast<long long>(rp[5][1] * 20U), static_cast<long long>(rp[4][1] * 20U),
        static_cast<long long>(rp[3][1] * 20U), static_cast<long long>(rp[2][1] * 20U),
        static_cast<long long>(rp[1][1] * 20U), static_cast<long long>(rp[0][1] * 20U));
    __m512i sv2 = _mm512_setr_epi64(
        static_cast<long long>(rp[7][2] * 20U), static_cast<long long>(rp[6][2] * 20U),
        static_cast<long long>(rp[5][2] * 20U), static_cast<long long>(rp[4][2] * 20U),
        static_cast<long long>(rp[3][2] * 20U), static_cast<long long>(rp[2][2] * 20U),
        static_cast<long long>(rp[1][2] * 20U), static_cast<long long>(rp[0][2] * 20U));
    const __m512i mask44 = _mm512_set1_epi64(static_cast<long long>(kPoly1305Mask44));
    const __m512i hibit = _mm512_set1_epi64(static_cast<long long>(kPoly1305Hibit));
    const __m512i order_lo = _mm512_setr_epi64(0, 2, 4, 6, 8, 10, 12, 14);
    const __m512i order_hi = _mm512_setr_epi64(1, 3, 5, 7, 9, 11, 13, 15);
    std::uint64_t acc0 = h[0];
    std::uint64_t acc1 = h[1];
    std::uint64_t acc2 = h[2];
    std::size_t offset = 0;
    for (std::size_t batch = 0; batch < batches; ++batch) {
        const __m512i z0 = _mm512_loadu_si512(data + offset);
        const __m512i z1 = _mm512_loadu_si512(data + offset + 64);
        // step2: 两条 permutex2var 把 8×2 的 lo/hi 转置成 lane 主序（lane k = 块 k）。
        const __m512i lo = _mm512_permutex2var_epi64(z0, order_lo, z1);
        const __m512i hi = _mm512_permutex2var_epi64(z0, order_hi, z1);
        // step3: 3 个 44/44/42 位 limb；lane 0 额外带上累积值 h（其余 lane 为 0），
        //        这样 lane 0 的部分积就是 (h+m0)·r^8。
        const __m512i c0 = _mm512_add_epi64(
            _mm512_and_si512(lo, mask44),
            _mm512_maskz_set1_epi64(0x01, static_cast<long long>(acc0)));
        const __m512i c1 = _mm512_add_epi64(
            _mm512_and_si512(
                _mm512_or_si512(_mm512_srli_epi64(lo, 44), _mm512_slli_epi64(hi, 20)),
                mask44),
            _mm512_maskz_set1_epi64(0x01, static_cast<long long>(acc1)));
        const __m512i c2 = _mm512_add_epi64(
            _mm512_or_si512(_mm512_srli_epi64(hi, 24), hibit),
            _mm512_maskz_set1_epi64(0x01, static_cast<long long>(acc2)));
        // step4: 9 对 vpmadd52 完成 3×3 卷积；lo 累加低 52 位、hi 累加高 52 位，
        //        8 个 lane 完全并行，18 条指令覆盖 128B。
        // 按“轮次”交错：每个累加器的相邻两条 madd 之间隔 6 条指令，
        // vpmadd52 的 4–5 周期延迟被其余累加器遮住（原顺序 3 条连发，
        // 关键路径 12–15 周期）。
        const __m512i zero = _mm512_setzero_si512();
        __m512i lo0 = _mm512_madd52lo_epu64(zero, c0, rv0);
        __m512i lo1 = _mm512_madd52lo_epu64(zero, c0, rv1);
        __m512i lo2 = _mm512_madd52lo_epu64(zero, c0, rv2);
        __m512i hi0 = _mm512_madd52hi_epu64(zero, c0, rv0);
        __m512i hi1 = _mm512_madd52hi_epu64(zero, c0, rv1);
        __m512i hi2 = _mm512_madd52hi_epu64(zero, c0, rv2);
        lo0 = _mm512_madd52lo_epu64(lo0, c1, sv2);
        lo1 = _mm512_madd52lo_epu64(lo1, c1, rv0);
        lo2 = _mm512_madd52lo_epu64(lo2, c1, rv1);
        hi0 = _mm512_madd52hi_epu64(hi0, c1, sv2);
        hi1 = _mm512_madd52hi_epu64(hi1, c1, rv0);
        hi2 = _mm512_madd52hi_epu64(hi2, c1, rv1);
        lo0 = _mm512_madd52lo_epu64(lo0, c2, sv1);
        lo1 = _mm512_madd52lo_epu64(lo1, c2, sv2);
        lo2 = _mm512_madd52lo_epu64(lo2, c2, rv0);
        hi0 = _mm512_madd52hi_epu64(hi0, c2, sv1);
        hi1 = _mm512_madd52hi_epu64(hi1, c2, sv2);
        hi2 = _mm512_madd52hi_epu64(hi2, c2, rv0);
        // step5: 每批一次跨 lane 归并（6 次 64 位水平求和）+ 3 段进位链。
        //        d = lo + (hi << 52)，所以 d >> 44 = (lo >> 44) + (hi << 8)。
        const std::uint64_t s_lo0 = Poly1305Sum8x64Ifma(lo0);
        const std::uint64_t s_hi0 = Poly1305Sum8x64Ifma(hi0);
        const std::uint64_t s_lo1 = Poly1305Sum8x64Ifma(lo1);
        const std::uint64_t s_hi1 = Poly1305Sum8x64Ifma(hi1);
        const std::uint64_t s_lo2 = Poly1305Sum8x64Ifma(lo2);
        const std::uint64_t s_hi2 = Poly1305Sum8x64Ifma(hi2);
        acc0 = s_lo0 & kPoly1305Mask44;
        std::uint64_t carry = (s_lo0 >> 44) + (s_hi0 << 8);
        const std::uint64_t v1 = s_lo1 + carry;
        acc1 = v1 & kPoly1305Mask44;
        carry = (v1 >> 44) + (s_hi1 << 8);
        const std::uint64_t v2 = s_lo2 + carry;
        acc2 = v2 & kPoly1305Mask42;
        carry = (v2 >> 42) + (s_hi2 << 10);
        acc0 += carry * 5U;
        carry = acc0 >> 44;
        acc0 &= kPoly1305Mask44;
        acc1 += carry;
        offset += 8 * 16;
    }
    h[0] = acc0;
    h[1] = acc1;
    h[2] = acc2;
    return offset;
}

/// @brief 多路 Poly1305 的统一入口：按运行期后端选 IFMA 8 路 / AVX-512 8 路 /
///        AVX2 4 路 / 标量。
/// @param h [in,out] radix-2^44 的 3 limb 累积值；被消费的批次推进
/// @param r [in] radix-2^44 的 3 limb 密钥
/// @param data [in] 连续 16B 块；可为 nullptr（此时必返回 0）
/// @param bytes [in] 可用字节数
/// @return 已消费字节数（4×16 或 8×16 的整数倍）；0 表示没有完整批次，
///         调用方须用标量路径处理剩余字节
std::size_t Poly1305BlocksSimd(std::uint64_t h[3], const std::uint64_t r[3],
                               const std::uint8_t* data, std::size_t bytes) noexcept {
    if (data == nullptr) {
        return 0;
    }
    const SimdBackend backend = ActiveSimdBackend();
    if (backend == SimdBackend::kScalar) {
        return 0;
    }
    std::size_t consumed = 0;
    if (backend == SimdBackend::kAvx512Ifma) {
        consumed += Poly1305BlocksIfma(h, r, data, bytes);
    } else if (backend == SimdBackend::kAvx512) {
        consumed += Poly1305BlocksAvx512(h, r, data, bytes);
    }
    // AVX-512 的 8 块批之后可能剩 4..7 块，交给 4 路批收尾。
    if (bytes - consumed >= 4 * 16) {
        consumed += Poly1305BlocksAvx2(h, r, data + consumed, bytes - consumed);
    }
    return consumed;
}

#endif  // BENTO_POLY_MULTI_SIMD

#endif  // BENTO_CRYPTO_X86_SIMD


}  // namespace

// ---------------------------------------------------------------------------
// 基础密码学原语
// ---------------------------------------------------------------------------

const char* SecurityErrorName(SecurityError error) noexcept {
    switch (error) {
        case SecurityError::kOk:
            return "kOk";
        case SecurityError::kEncryptionRequired:
            return "kEncryptionRequired";
        case SecurityError::kLegacyPeerRejected:
            return "kLegacyPeerRejected";
        case SecurityError::kInvalidPsk:
            return "kInvalidPsk";
    }
    return "kUnknown";
}

const char* SecurityErrorMessage(SecurityError error) noexcept {
    switch (error) {
        case SecurityError::kOk:
            return "kOk: security policy satisfied";
        case SecurityError::kEncryptionRequired:
            return "kEncryptionRequired: encryption is required but no 32-byte "
                   "pre-shared key is configured (set pre_shared_key or explicitly "
                   "disable require_encryption, which is insecure)";
        case SecurityError::kLegacyPeerRejected:
            return "kLegacyPeerRejected: peer has no X25519 public key and "
                   "allow_legacy_peer=false (set allow_legacy_peer=true to accept "
                   "PSK-only legacy peers, which loses forward secrecy)";
        case SecurityError::kInvalidPsk:
            return "kInvalidPsk: pre_shared_key must be exactly 32 bytes";
    }
    return "kUnknown: unknown security error";
}

void SecureRandomBytes(std::uint8_t* out, std::size_t bytes) {
    if (out == nullptr || bytes == 0) {
        return;
    }
    // 唯一 CSPRNG 实现：go::crypto::rand（getrandom(2) → /dev/urandom）。
    // 本函数曾用 std::random_device：与 p2p::SecureRandomBytes 同名同签名、
    // 安全强度却不同（见 docs/reports/reuse-audit.md T3），现已统一到同一底座。
    if (go2cpp::go::crypto::rand::Read(out, bytes) != bytes) {
        // 熵源整体不可用属于系统级故障：不回退到确定性填充，
        // 避免静默产出可预测的密钥 / nonce。
        throw std::runtime_error("bento: secure random source unavailable");
    }
}

void ChaCha20Block(const std::uint8_t key[32], std::uint32_t counter,
                   const std::uint8_t nonce[12], std::uint8_t out[64]) noexcept {
    std::uint32_t state[16];
    std::uint32_t block[16];
    ChaCha20InitState(key, counter, nonce, state);
    ChaCha20Permute(state, block);
    for (int i = 0; i < 16; ++i) {
        Store32LE(out + i * 4, block[i]);
    }
}

void HChaCha20(const std::uint8_t key[32], const std::uint8_t input[16],
               std::uint8_t out[32]) noexcept {
    std::uint32_t state[16];
    state[0] = 0x61707865U;
    state[1] = 0x3320646eU;
    state[2] = 0x79622d32U;
    state[3] = 0x6b206574U;
    for (int i = 0; i < 8; ++i) {
        state[4 + i] = Load32LE(key + i * 4);
    }
    for (int i = 0; i < 4; ++i) {
        state[12 + i] = Load32LE(input + i * 4);
    }
    std::uint32_t x[16];
    std::memcpy(x, state, sizeof(x));
    for (int round = 0; round < 10; ++round) {
        QuarterRound(x[0], x[4], x[8], x[12]);
        QuarterRound(x[1], x[5], x[9], x[13]);
        QuarterRound(x[2], x[6], x[10], x[14]);
        QuarterRound(x[3], x[7], x[11], x[15]);
        QuarterRound(x[0], x[5], x[10], x[15]);
        QuarterRound(x[1], x[6], x[11], x[12]);
        QuarterRound(x[2], x[7], x[8], x[13]);
        QuarterRound(x[3], x[4], x[9], x[14]);
    }
    // HChaCha20 输出 state[0..3] 与 state[12..15]，不做初始加法。
    for (int i = 0; i < 4; ++i) {
        Store32LE(out + i * 4, x[i]);
        Store32LE(out + 16 + i * 4, x[12 + i]);
    }
}

void ChaCha20Xor(const std::uint8_t key[32], std::uint32_t counter,
                 const std::uint8_t nonce[12], const std::uint8_t* input, std::uint8_t* output,
                 std::size_t bytes) noexcept {
    if (bytes == 0 || (input == nullptr && output == nullptr)) {
        return;
    }
    std::uint32_t state[16];
    ChaCha20InitState(key, counter, nonce, state);
    std::size_t offset = 0;
    // SIMD 批量块：AVX-512 每批 16 块（1024B）、AVX2 每批 8 块（512B），
    // 剩余不足一批的尾部继续走下面的标量 4 路 / 单块路径。
    const SimdBackend backend = ActiveSimdBackend();
#if BENTO_CRYPTO_X86_SIMD
    if (backend >= SimdBackend::kAvx512) {
        offset += ChaCha20XorAvx512(state, counter, input, output, bytes);
    }
    if (backend != SimdBackend::kScalar && offset < bytes) {
        offset += ChaCha20XorAvx2(
            state, counter + static_cast<std::uint32_t>(offset / kChaChaBlockBytes),
            input + offset, output + offset, bytes - offset);
    }
#else
    (void)backend;
#endif
    // 4 块一组：一次排列产出 256B 密钥流，直接按 32 位字异或。
    while (bytes - offset >= 4 * kChaChaBlockBytes) {
        std::uint32_t blocks[4][16];
        ChaCha20Permute4(state, counter + static_cast<std::uint32_t>(offset / kChaChaBlockBytes),
                         blocks);
        for (int lane = 0; lane < 4; ++lane) {
            const std::size_t lane_offset =
                offset + static_cast<std::size_t>(lane) * kChaChaBlockBytes;
            for (int i = 0; i < 16; ++i) {
                const std::uint32_t value =
                    Load32LE(input + lane_offset + static_cast<std::size_t>(i) * 4) ^
                    blocks[lane][i];
                Store32LE(output + lane_offset + static_cast<std::size_t>(i) * 4, value);
            }
        }
        offset += 4 * kChaChaBlockBytes;
    }
    while (offset < bytes) {
        std::uint32_t block[16];
        ChaCha20PermuteAt(state, counter + static_cast<std::uint32_t>(offset / kChaChaBlockBytes),
                          block);
        const std::size_t chunk = std::min<std::size_t>(kChaChaBlockBytes, bytes - offset);
        if (chunk == kChaChaBlockBytes) {
            for (int i = 0; i < 16; ++i) {
                const std::uint32_t value =
                    Load32LE(input + offset + static_cast<std::size_t>(i) * 4) ^ block[i];
                Store32LE(output + offset + static_cast<std::size_t>(i) * 4, value);
            }
        } else {
            // 尾块逐字节异或：input/output 允许同一地址（原地）。
            std::uint8_t stream[kChaChaBlockBytes];
            for (int i = 0; i < 16; ++i) {
                Store32LE(stream + i * 4, block[i]);
            }
            for (std::size_t i = 0; i < chunk; ++i) {
                output[offset + i] = static_cast<std::uint8_t>(input[offset + i] ^ stream[i]);
            }
        }
        offset += chunk;
    }
}

void Poly1305(const std::uint8_t key[32], const std::uint8_t* message, std::size_t bytes,
              std::uint8_t tag[16]) noexcept {
    Poly1305Ctx ctx;
    Poly1305Init(&ctx, key);
    std::size_t offset = 0;
#if BENTO_POLY_MULTI_SIMD
    offset += Poly1305BlocksSimd(ctx.h, ctx.r, message, bytes);
#endif
    while (bytes - offset >= 16) {
        Poly1305Block(&ctx, message + offset, kPoly1305Hibit);
        offset += 16;
    }
    const std::size_t remainder = bytes - offset;
    if (remainder != 0) {
        std::uint8_t block[16] = {};
        std::memcpy(block, message + offset, remainder);
        block[remainder] = 1;
        Poly1305Block(&ctx, block, 0);
        SecureZero(block, sizeof(block));
    }
    Poly1305Finish(&ctx, tag);
    SecureZero(&ctx, sizeof(ctx));
}

void AeadSeal(const std::uint8_t key[32], const std::uint8_t nonce[12], const std::uint8_t* aad,
              std::size_t aad_bytes, const std::uint8_t* plaintext, std::size_t bytes,
              std::uint8_t* ciphertext, std::uint8_t tag[16]) noexcept {
    std::uint8_t block0[64];
    ChaCha20Block(key, 0, nonce, block0);
    ChaCha20Xor(key, 1, nonce, plaintext, ciphertext, bytes);

    Poly1305Ctx ctx;
    Poly1305Init(&ctx, block0);
    Poly1305UpdatePadded(&ctx, aad, aad_bytes);
    Poly1305UpdatePadded(&ctx, ciphertext, bytes);
    std::uint8_t lengths[16];
    Store64LE(lengths, static_cast<std::uint64_t>(aad_bytes));
    Store64LE(lengths + 8, static_cast<std::uint64_t>(bytes));
    Poly1305Block(&ctx, lengths, kPoly1305Hibit);
    Poly1305Finish(&ctx, tag);
    // 一次性 Poly1305 密钥（block0）与 MAC 中间状态用完即清，
    // 避免留在栈上被后续调用或 core dump 带走。
    SecureZero(block0, sizeof(block0));
    SecureZero(&ctx, sizeof(ctx));
}

bool ConstantTimeEqual16(const std::uint8_t* a, const std::uint8_t* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    std::uint32_t difference = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        difference |= static_cast<std::uint32_t>(a[i] ^ b[i]);
    }
    return difference == 0;
}

bool AeadOpen(const std::uint8_t key[32], const std::uint8_t nonce[12], const std::uint8_t* aad,
              std::size_t aad_bytes, const std::uint8_t* ciphertext, std::size_t bytes,
              const std::uint8_t tag[16], std::uint8_t* plaintext) noexcept {
    std::uint8_t block0[64];
    ChaCha20Block(key, 0, nonce, block0);
    Poly1305Ctx ctx;
    Poly1305Init(&ctx, block0);
    Poly1305UpdatePadded(&ctx, aad, aad_bytes);
    Poly1305UpdatePadded(&ctx, ciphertext, bytes);
    std::uint8_t lengths[16];
    Store64LE(lengths, static_cast<std::uint64_t>(aad_bytes));
    Store64LE(lengths + 8, static_cast<std::uint64_t>(bytes));
    Poly1305Block(&ctx, lengths, kPoly1305Hibit);
    std::uint8_t expected[16];
    Poly1305Finish(&ctx, expected);
    const bool authenticated = ConstantTimeEqual16(expected, tag);
    SecureZero(block0, sizeof(block0));
    SecureZero(&ctx, sizeof(ctx));
    SecureZero(expected, sizeof(expected));
    if (!authenticated) {
        return false;
    }
    ChaCha20Xor(key, 1, nonce, ciphertext, plaintext, bytes);
    return true;
}

// ---------------------------------------------------------------------------
// KDF / nonce
// ---------------------------------------------------------------------------

void DeriveHandshakeKey(const std::uint8_t psk[32], SecureDirection direction,
                        std::uint8_t out[32]) noexcept {
    HChaCha20(psk, direction == SecureDirection::kClientToServer ? kLabelHandshakeC2S
                                                                 : kLabelHandshakeS2C,
              out);
}

namespace {

/// 把 32B 根密钥与双方 nonce、方向标签逐层绑定成方向密钥。
void DeriveDirectionKey(const std::uint8_t root[32], const std::uint8_t client_nonce[16],
                        const std::uint8_t server_nonce[16], SecureDirection direction,
                        std::uint8_t out[32]) noexcept {
    std::uint8_t bound[32];
    std::uint8_t stage[32];
    HChaCha20(root, client_nonce, bound);
    HChaCha20(bound, server_nonce, stage);
    HChaCha20(stage, DirectionLabel(direction), out);
    SecureZero(bound, sizeof(bound));
    SecureZero(stage, sizeof(stage));
}

}  // namespace

void DeriveSessionKey(const std::uint8_t psk[32], const std::uint8_t client_nonce[16],
                      const std::uint8_t server_nonce[16], SecureDirection direction,
                      std::uint8_t out[32]) noexcept {
    DeriveDirectionKey(psk, client_nonce, server_nonce, direction, out);
}

void DeriveSessionKeyWithX25519(const std::uint8_t psk[32], const std::uint8_t shared[32],
                                const std::uint8_t client_nonce[16],
                                const std::uint8_t server_nonce[16],
                                SecureDirection direction, std::uint8_t out[32]) noexcept {
    if (psk == nullptr || shared == nullptr || client_nonce == nullptr ||
        server_nonce == nullptr || out == nullptr) {
        return;
    }
    // 两层 HChaCha20 把 32B 共享密钥完整混入 PSK：
    //   mixed = HChaCha20(HChaCha20(HChaCha20(psk, label), shared[0..16]), shared[16..32])
    // 只混 16B 会让后一半共享密钥不参与，这里两半都吸收。
    std::uint8_t stage[32];
    std::uint8_t mixed[32];
    HChaCha20(psk, kLabelX25519Mix, stage);
    HChaCha20(stage, shared, mixed);
    HChaCha20(mixed, shared + 16, stage);
    DeriveDirectionKey(stage, client_nonce, server_nonce, direction, out);
    SecureZero(stage, sizeof(stage));
    SecureZero(mixed, sizeof(mixed));
}

void SecureZero(void* data, std::size_t bytes) noexcept {
    if (data == nullptr || bytes == 0) {
        return;
    }
    volatile std::uint8_t* bytes_out = static_cast<volatile std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        bytes_out[i] = 0;
    }
}

void DerivePskFingerprint(const std::uint8_t psk[32], std::uint8_t out[8]) noexcept {
    if (psk == nullptr || out == nullptr) {
        return;
    }
    std::uint8_t digest[32];
    HChaCha20(psk, kLabelPskFingerprint, digest);
    std::memcpy(out, digest, 8);
    SecureZero(digest, sizeof(digest));
}

void MakeSecureNonce(SecureDirection direction, std::uint32_t seq, std::uint8_t nonce[12]) noexcept {
    Store32BE(nonce, static_cast<std::uint32_t>(direction));
    Store64BE(nonce + 4, static_cast<std::uint64_t>(seq));
}

// ---------------------------------------------------------------------------
// 重放窗口
// ---------------------------------------------------------------------------

void ReplayWindow::Reset() noexcept {
    m_highest = 0;
    m_bitmap = 0;
    m_initialized = false;
}

void ReplayWindow::InitializeBefore(std::uint32_t next_seq) noexcept {
    if (next_seq <= 1) {
        Reset();
        return;
    }
    m_initialized = true;
    m_highest = next_seq - 1;
    // 0-RTT 恢复后，窗口内所有 < next_seq 的序号都属于旧会话、已经消费过。
    // 只置 highest 一位会让 age 1..63 的旧包仍通过 WouldAccept，等于给
    // 攻击者留了 64 个重放入口（旧会话密钥在恢复后仍然有效）。这里把
    // 整个窗口标记为已接收；新序号 > highest 仍然正常接受。
    m_bitmap = ~0ULL;
}

bool ReplayWindow::WouldAccept(std::uint32_t seq) const noexcept {
    if (seq == 0) {
        return false;
    }
    if (!m_initialized) {
        return true;
    }
    const std::int32_t delta = static_cast<std::int32_t>(seq - m_highest);
    if (delta > 0) {
        return true;
    }
    const std::uint32_t age = static_cast<std::uint32_t>(-delta);
    if (age >= kWindow) {
        return false;
    }
    return (m_bitmap & (1ULL << age)) == 0;
}

bool ReplayWindow::Accept(std::uint32_t seq) noexcept {
    if (!WouldAccept(seq)) {
        return false;
    }
    if (!m_initialized) {
        m_initialized = true;
        m_highest = seq;
        m_bitmap = 1;
        return true;
    }
    const std::int32_t delta = static_cast<std::int32_t>(seq - m_highest);
    if (delta > 0) {
        const std::uint32_t shift = static_cast<std::uint32_t>(delta);
        m_bitmap = shift >= 64 ? 1ULL : ((m_bitmap << shift) | 1ULL);
        m_highest = seq;
    } else {
        const std::uint32_t age = static_cast<std::uint32_t>(-delta);
        m_bitmap |= (1ULL << age);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 握手消息编解码
// ---------------------------------------------------------------------------

bool EncodeHelloBody(const HelloBody& body, std::uint8_t* out, std::size_t bytes) noexcept {
    if (out == nullptr || bytes < kLegacyHelloBodyBytes) {
        return false;
    }
    const bool with_x25519 = bytes >= kHelloBodyBytes;
    if (!with_x25519 && bytes != kLegacyHelloBodyBytes) {
        return false;
    }
    std::memcpy(out, kSecureMagic, 4);
    out[4] = kSecureHandshakeVersion;
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
    Store32BE(out + 8, body.session);
    std::memcpy(out + 12, body.nonce, kHandshakeNonceBytes);
    Store64BE(out + 28, body.timestamp_ms);
    if (with_x25519) {
        std::memcpy(out + kLegacyHelloBodyBytes, body.x25519_public, kX25519KeyBytes);
    }
    return true;
}

bool DecodeHelloBody(const std::uint8_t* data, std::size_t bytes, HelloBody* out) noexcept {
    if (data == nullptr || out == nullptr || bytes < kLegacyHelloBodyBytes) {
        return false;
    }
    const bool with_x25519 = bytes >= kHelloBodyBytes;
    if (!with_x25519 && bytes != kLegacyHelloBodyBytes) {
        return false;
    }
    if (std::memcmp(data, kSecureMagic, 4) != 0 || data[4] != kSecureHandshakeVersion) {
        return false;
    }
    out->session = Load32BE(data + 8);
    std::memcpy(out->nonce, data + 12, kHandshakeNonceBytes);
    out->timestamp_ms = Load64BE(data + 28);
    std::memset(out->x25519_public, 0, sizeof(out->x25519_public));
    if (with_x25519) {
        std::memcpy(out->x25519_public, data + kLegacyHelloBodyBytes, kX25519KeyBytes);
    }
    return out->session != 0;
}

bool EncodeHelloAckBody(const HelloAckBody& body, std::uint8_t* out,
                        std::size_t bytes) noexcept {
    if (out == nullptr || bytes < kLegacyHelloAckBodyBytes) {
        return false;
    }
    const bool with_x25519 = bytes >= kHelloAckBodyBytes;
    if (!with_x25519 && bytes != kLegacyHelloAckBodyBytes) {
        return false;
    }
    std::memcpy(out, kSecureMagic, 4);
    out[4] = kSecureHandshakeVersion;
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
    Store32BE(out + 8, body.session);
    std::memcpy(out + 12, body.client_nonce, kHandshakeNonceBytes);
    std::memcpy(out + 28, body.server_nonce, kHandshakeNonceBytes);
    Store64BE(out + 44, body.timestamp_ms);
    if (with_x25519) {
        std::memcpy(out + kLegacyHelloAckBodyBytes, body.x25519_public, kX25519KeyBytes);
    }
    return true;
}

bool DecodeHelloAckBody(const std::uint8_t* data, std::size_t bytes,
                        HelloAckBody* out) noexcept {
    if (data == nullptr || out == nullptr || bytes < kLegacyHelloAckBodyBytes) {
        return false;
    }
    const bool with_x25519 = bytes >= kHelloAckBodyBytes;
    if (!with_x25519 && bytes != kLegacyHelloAckBodyBytes) {
        return false;
    }
    if (std::memcmp(data, kSecureMagic, 4) != 0 || data[4] != kSecureHandshakeVersion) {
        return false;
    }
    out->session = Load32BE(data + 8);
    std::memcpy(out->client_nonce, data + 12, kHandshakeNonceBytes);
    std::memcpy(out->server_nonce, data + 28, kHandshakeNonceBytes);
    out->timestamp_ms = Load64BE(data + 44);
    std::memset(out->x25519_public, 0, sizeof(out->x25519_public));
    if (with_x25519) {
        std::memcpy(out->x25519_public, data + kLegacyHelloAckBodyBytes, kX25519KeyBytes);
    }
    return out->session != 0;
}

// ---------------------------------------------------------------------------
// 0-RTT 会话票据
// ---------------------------------------------------------------------------

namespace {

/// 票据布局（小端/大端无关，全部显式按 BE 写）：
///   0 magic(4) | 4 ver(1) | 5 flags(1) | 6 reserved(2)
///   8 conn_id(4) | 12 session(4) | 16 issued(8) | 24 expires(8)
///   32 client_nonce(16) | 48 server_nonce(16)
///   64 client_next(4) | 68 server_next(4) | 72 reserved(4) | 76 tag(16)
constexpr std::size_t kTicketMacOffset = 76;
constexpr std::size_t kTicketMacInputOffset = 8;
constexpr std::size_t kTicketMacInputBytes = 16;
constexpr std::uint8_t kTicketMagic[4] = {'B', 'T', 'K', 'T'};

/// 用 PSK 派生票据 MAC 根密钥，再用票据中的 (conn_id, session, issued)
/// 作为 HChaCha20 的 16B 输入派生一次性 Poly1305 密钥。
void TicketMac(const std::uint8_t psk[32], const std::uint8_t* data,
               std::size_t bytes, std::uint8_t tag[16]) noexcept {
    std::uint8_t ticket_key[32];
    std::uint8_t one_time_key[32];
    HChaCha20(psk, kLabelTicketKey, ticket_key);
    HChaCha20(ticket_key, data + kTicketMacInputOffset, one_time_key);
    Poly1305(one_time_key, data, bytes, tag);
    // 票据 MAC 的一次性密钥与根密钥用完即清，避免留在栈上。
    SecureZero(ticket_key, sizeof(ticket_key));
    SecureZero(one_time_key, sizeof(one_time_key));
}

}  // namespace

bool EncodeSessionTicket(const SessionTicket& ticket, const std::uint8_t psk[32],
                         std::vector<std::uint8_t>* out) {
    if (psk == nullptr || out == nullptr || ticket.conn_id == 0 || ticket.session_id == 0 ||
        ticket.client_next_wire_seq == 0 || ticket.server_next_wire_seq == 0 ||
        ticket.expires_ms < ticket.issued_ms) {
        return false;
    }
    std::uint8_t raw[kSessionTicketBytes] = {};
    std::memcpy(raw, kTicketMagic, 4);
    raw[4] = kSessionTicketVersion;
    raw[5] = 0x01;  // bit0：PSK 模式票据。
    Store32BE(raw + 8, ticket.conn_id);
    Store32BE(raw + 12, ticket.session_id);
    Store64BE(raw + 16, ticket.issued_ms);
    Store64BE(raw + 24, ticket.expires_ms);
    std::memcpy(raw + 32, ticket.client_nonce, kHandshakeNonceBytes);
    std::memcpy(raw + 48, ticket.server_nonce, kHandshakeNonceBytes);
    Store32BE(raw + 64, ticket.client_next_wire_seq);
    Store32BE(raw + 68, ticket.server_next_wire_seq);
    TicketMac(psk, raw, kTicketMacOffset, raw + kTicketMacOffset);
    out->assign(raw, raw + kSessionTicketBytes);
    return true;
}

bool DecodeSessionTicket(const std::uint8_t* data, std::size_t bytes, const std::uint8_t psk[32],
                         SessionTicket* out) {
    if (data == nullptr || psk == nullptr || out == nullptr || bytes != kSessionTicketBytes) {
        return false;
    }
    if (std::memcmp(data, kTicketMagic, 4) != 0 || data[4] != kSessionTicketVersion ||
        (data[5] & 0x01) == 0 || (data[5] & 0xFE) != 0 || data[6] != 0 || data[7] != 0) {
        return false;
    }
    std::uint8_t expected[16];
    TicketMac(psk, data, kTicketMacOffset, expected);
    if (!ConstantTimeEqual16(expected, data + kTicketMacOffset)) {
        return false;
    }
    if (Load32BE(data + 72) != 0) {
        return false;
    }
    SessionTicket ticket;
    ticket.conn_id = Load32BE(data + 8);
    ticket.session_id = Load32BE(data + 12);
    ticket.issued_ms = Load64BE(data + 16);
    ticket.expires_ms = Load64BE(data + 24);
    std::memcpy(ticket.client_nonce, data + 32, kHandshakeNonceBytes);
    std::memcpy(ticket.server_nonce, data + 48, kHandshakeNonceBytes);
    ticket.client_next_wire_seq = Load32BE(data + 64);
    ticket.server_next_wire_seq = Load32BE(data + 68);
    if (ticket.conn_id == 0 || ticket.session_id == 0 || ticket.client_next_wire_seq == 0 ||
        ticket.server_next_wire_seq == 0 || ticket.expires_ms < ticket.issued_ms) {
        return false;
    }
    *out = ticket;
    return true;
}

// ---------------------------------------------------------------------------
// 加密帧封装
// ---------------------------------------------------------------------------

bool SealFrame(const std::uint8_t key[32], SecureDirection direction, std::uint32_t seq,
               const std::uint8_t* inner, std::size_t bytes, std::vector<std::uint8_t>* out) {
    if (key == nullptr || inner == nullptr || out == nullptr || seq == 0 || bytes == 0 ||
        bytes > kMaxProbeFrameBytes) {
        return false;
    }
    FrameHeader inner_header;
    if (!DecodeHeader(inner, bytes, &inner_header)) {
        return false;
    }
    FrameHeader outer;
    outer.version = kProtocolVersion;
    outer.type = inner_header.type;
    outer.channel = inner_header.channel;
    outer.flags = static_cast<std::uint8_t>(inner_header.flags | kFlagEncrypted);
    // conn_id 必须明文可见（接收端按它路由），并参与 AAD 认证。
    outer.conn_id = inner_header.conn_id;
    outer.seq = seq;
    outer.dep_seq = 0;

    out->assign(kFrameHeaderBytes + bytes + kAeadTagBytes, 0);
    EncodeHeader(outer, out->data());
    std::uint8_t nonce[kChaCha20NonceBytes];
    MakeSecureNonce(direction, seq, nonce);
    AeadSeal(key, nonce, out->data(), kFrameHeaderBytes, inner, bytes,
             out->data() + kFrameHeaderBytes, out->data() + kFrameHeaderBytes + bytes);
    return true;
}

bool OpenFrame(const std::uint8_t key[32], SecureDirection direction, ReplayWindow* replay,
               const std::uint8_t* packet, std::size_t bytes, std::vector<std::uint8_t>* inner,
               OpenStatus* status) {
    if (status != nullptr) {
        *status = OpenStatus::kMalformed;
    }
    // 失败路径（重放/畸形/认证失败）不得把上一次的明文留在调用方缓冲里。
    if (inner != nullptr) {
        inner->clear();
    }
    if (key == nullptr || packet == nullptr || inner == nullptr || bytes < kFrameHeaderBytes) {
        return false;
    }
    FrameHeader outer;
    if (!DecodeHeader(packet, bytes, &outer)) {
        if (status != nullptr) {
            *status = OpenStatus::kMalformed;
        }
        return false;
    }
    if ((outer.flags & kFlagEncrypted) == 0) {
        if (status != nullptr) {
            *status = OpenStatus::kNotEncrypted;
        }
        return false;
    }
    if (bytes < kSecureOverheadBytes + kFrameHeaderBytes) {
        if (status != nullptr) {
            *status = OpenStatus::kMalformed;
        }
        return false;
    }
    if (outer.seq == 0) {
        if (status != nullptr) {
            *status = OpenStatus::kMalformed;
        }
        return false;
    }
    if (replay != nullptr && !replay->WouldAccept(outer.seq)) {
        if (status != nullptr) {
            *status = OpenStatus::kReplay;
        }
        return false;
    }

    const std::size_t ciphertext_bytes = bytes - kFrameHeaderBytes - kAeadTagBytes;
    if (ciphertext_bytes < kFrameHeaderBytes || ciphertext_bytes > kMaxProbeFrameBytes) {
        if (status != nullptr) {
            *status = OpenStatus::kMalformed;
        }
        return false;
    }
    std::uint8_t nonce[kChaCha20NonceBytes];
    MakeSecureNonce(direction, outer.seq, nonce);
    inner->assign(ciphertext_bytes, 0);
    if (!AeadOpen(key, nonce, packet, kFrameHeaderBytes, packet + kFrameHeaderBytes,
                  ciphertext_bytes, packet + kFrameHeaderBytes + ciphertext_bytes,
                  inner->data())) {
        if (status != nullptr) {
            *status = OpenStatus::kAuthFailed;
        }
        return false;
    }
    if (replay != nullptr) {
        replay->Accept(outer.seq);
    }
    if (status != nullptr) {
        *status = OpenStatus::kOk;
    }
    return true;
}

bool SealHandshakeFrame(const std::uint8_t psk[32], SecureDirection direction,
                        std::uint8_t inner_type, std::uint32_t session, std::uint32_t conn_id,
                        std::uint32_t seq, const std::uint8_t* body, std::size_t body_bytes,
                        std::vector<std::uint8_t>* out) {
    if (psk == nullptr || body == nullptr || out == nullptr || session == 0 || conn_id == 0 ||
        seq == 0) {
        return false;
    }
    if (inner_type != static_cast<std::uint8_t>(FrameType::kHello) &&
        inner_type != static_cast<std::uint8_t>(FrameType::kHelloAck)) {
        return false;
    }
    FrameHeader header;
    header.type = static_cast<FrameType>(inner_type);
    header.channel = 0;
    header.flags = 0;
    header.conn_id = conn_id;
    header.seq = session;
    header.dep_seq = 0;
    const std::vector<std::uint8_t> inner_frame = BuildFrame(header, nullptr, body, body_bytes);
    if (inner_frame.empty()) {
        return false;
    }
    std::uint8_t handshake_key[32];
    DeriveHandshakeKey(psk, direction, handshake_key);
    const bool sealed =
        SealFrame(handshake_key, direction, seq, inner_frame.data(), inner_frame.size(), out);
    SecureZero(handshake_key, sizeof(handshake_key));
    return sealed;
}

bool OpenHandshakeFrame(const std::uint8_t psk[32], SecureDirection direction,
                        const std::uint8_t* packet, std::size_t bytes, std::uint8_t* inner_type,
                        std::uint32_t* session, std::uint32_t* conn_id,
                        std::vector<std::uint8_t>* body, OpenStatus* status) {
    if (psk == nullptr || packet == nullptr || inner_type == nullptr || session == nullptr ||
        body == nullptr) {
        return false;
    }
    std::uint8_t handshake_key[32];
    DeriveHandshakeKey(psk, direction, handshake_key);
    std::vector<std::uint8_t> inner_frame;
    OpenStatus open_status = OpenStatus::kMalformed;
    const bool opened =
        OpenFrame(handshake_key, direction, nullptr, packet, bytes, &inner_frame, &open_status);
    SecureZero(handshake_key, sizeof(handshake_key));
    if (!opened) {
        if (status != nullptr) {
            *status = open_status;
        }
        return false;
    }
    FrameView view;
    if (!ParseFrame(inner_frame.data(), inner_frame.size(), &view)) {
        if (status != nullptr) {
            *status = OpenStatus::kMalformed;
        }
        return false;
    }
    if (view.header.type != FrameType::kHello && view.header.type != FrameType::kHelloAck) {
        if (status != nullptr) {
            *status = OpenStatus::kMalformed;
        }
        return false;
    }
    *inner_type = static_cast<std::uint8_t>(view.header.type);
    *session = view.header.seq;
    if (conn_id != nullptr) {
        *conn_id = view.header.conn_id;
    }
    body->assign(view.payload, view.payload + view.payload_bytes);
    if (status != nullptr) {
        *status = OpenStatus::kOk;
    }
    return true;
}

}  // namespace go2cpp::bento
