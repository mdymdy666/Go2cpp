/**
 * @file crypto.cpp
 * @brief ChaCha20-Poly1305（RFC 8439）、HChaCha20 KDF、重放窗口与加密帧实现。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include "go2cpp/bento/net/crypto.hpp"

#include <algorithm>
#include <cstring>
#include <random>

#include "go2cpp/bento/net/protocol.hpp"

namespace go2cpp::bento {
namespace {

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
void Poly1305Block(Poly1305Ctx* ctx, const std::uint8_t block[16], std::uint32_t hibit) noexcept {
    std::uint32_t h0 = ctx->h[0] + (Load32LE(block + 0) & 0x3ffffffU);
    std::uint32_t h1 = ctx->h[1] + ((Load32LE(block + 3) >> 2) & 0x3ffffffU);
    std::uint32_t h2 = ctx->h[2] + ((Load32LE(block + 6) >> 4) & 0x3ffffffU);
    std::uint32_t h3 = ctx->h[3] + ((Load32LE(block + 9) >> 6) & 0x3ffffffU);
    std::uint32_t h4 = ctx->h[4] + ((Load32LE(block + 12) >> 8) | hibit);

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
        Poly1305Block(ctx, data + offset, 1U << 24);
        offset += 16;
    }
    const std::size_t remainder = bytes - offset;
    if (remainder != 0) {
        std::uint8_t block[16] = {};
        std::memcpy(block, data + offset, remainder);
        Poly1305Block(ctx, block, 1U << 24);
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

}  // namespace

// ---------------------------------------------------------------------------
// 基础密码学原语
// ---------------------------------------------------------------------------

void SecureRandomBytes(std::uint8_t* out, std::size_t bytes) {
    if (out == nullptr || bytes == 0) {
        return;
    }
    std::random_device device;
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<std::uint8_t>(device());
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
    std::uint8_t stream[64];
    std::size_t offset = 0;
    while (offset < bytes) {
        std::uint32_t block[16];
        ChaCha20Permute(state, block);
        for (int i = 0; i < 16; ++i) {
            Store32LE(stream + i * 4, block[i]);
        }
        const std::size_t chunk = std::min<std::size_t>(64, bytes - offset);
        for (std::size_t i = 0; i < chunk; ++i) {
            output[offset + i] = static_cast<std::uint8_t>(input[offset + i] ^ stream[i]);
        }
        offset += chunk;
        ++state[12];
        if (state[12] == 0) {
            // 64B 块计数器溢出：RFC 8439 限制单条消息远小于 256GB，
            // 这里的递增仅为避免 UB；正常不会触发。
            ++state[13];
        }
    }
}

void Poly1305(const std::uint8_t key[32], const std::uint8_t* message, std::size_t bytes,
              std::uint8_t tag[16]) noexcept {
    Poly1305Ctx ctx;
    Poly1305Init(&ctx, key);
    std::size_t offset = 0;
    while (bytes - offset >= 16) {
        Poly1305Block(&ctx, message + offset, 1U << 24);
        offset += 16;
    }
    const std::size_t remainder = bytes - offset;
    if (remainder != 0) {
        std::uint8_t block[16] = {};
        std::memcpy(block, message + offset, remainder);
        block[remainder] = 1;
        Poly1305Block(&ctx, block, 0);
    }
    Poly1305Finish(&ctx, tag);
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
    Poly1305Block(&ctx, lengths, 1U << 24);
    Poly1305Finish(&ctx, tag);
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
    Poly1305Block(&ctx, lengths, 1U << 24);
    std::uint8_t expected[16];
    Poly1305Finish(&ctx, expected);
    if (!ConstantTimeEqual16(expected, tag)) {
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
    m_bitmap = 1ULL;  // highest 本身视为已消费。
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
    return SealFrame(handshake_key, direction, seq, inner_frame.data(), inner_frame.size(), out);
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
    if (!OpenFrame(handshake_key, direction, nullptr, packet, bytes, &inner_frame, &open_status)) {
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
