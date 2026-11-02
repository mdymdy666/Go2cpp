/**
 * @file sha256.cpp
 * @brief SHA-256（FIPS 180-4）的实现：标量 + x86 SHA-NI 硬件加速。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 FIPS 180-4 独立实现）
 */

#include "go2cpp/hash/sha256.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "go2cpp/network/endian.hpp"

// SHA-NI 只在 x86 的 GCC/Clang 上启用：`__attribute__((target("sha")))` 与
// `__builtin_cpu_supports` 都是编译器扩展；其它平台（含 MSVC）走标量路径。
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define GO2CPP_HASH_SHA_NI 1
#else
#define GO2CPP_HASH_SHA_NI 0
#endif

namespace go2cpp::hash {

namespace {

inline std::uint32_t Rotr(std::uint32_t value, int shift) noexcept {
    return (value >> shift) | (value << (32 - shift));
}

inline std::uint32_t LoadBE32(const std::uint8_t* p) noexcept {
    return go2cpp::net::ReadBE32(p);
}

/// FIPS 180-4 §4.2.2 的 64 个轮常量：前 64 个素数立方根小数部分的前 32 位。
constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

/// FIPS 180-4 §5.3.3 的初始链接变量：前 8 个素数平方根小数部分的前 32 位。
constexpr std::uint32_t kInitialState[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

/// 标量压缩：处理一个 64 字节块，原地更新 8 个链接变量。
void CompressScalar(std::uint32_t* state, const std::uint8_t* block) noexcept {
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = LoadBE32(block + i * 4);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kK[i] + w[i];
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

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/// 标量批量压缩：连续 `blocks` 个块。
void CompressBlocksScalar(std::uint32_t* state, const std::uint8_t* data,
                          std::size_t blocks) noexcept {
    for (std::size_t i = 0; i < blocks; ++i) {
        CompressScalar(state, data + i * 64);
    }
}

#if GO2CPP_HASH_SHA_NI

/// @brief SHA-NI 压缩函数：处理一个 64 字节块，原地更新 8 个链接变量。
/// @note 结构来自 Intel《Intel SHA Extensions》白皮书的参考实现（16 组 × 4 轮）。
__attribute__((target("sha,sse4.1"))) void CompressShaNi(
    std::uint32_t* state, const std::uint8_t* data) noexcept {
    __m128i state0;
    __m128i state1;
    __m128i msg;
    __m128i tmp;
    __m128i msg0;
    __m128i msg1;
    __m128i msg2;
    __m128i msg3;
    __m128i abef_save;
    __m128i cdgh_save;
    const __m128i mask = _mm_set_epi64x(static_cast<long long>(0x0c0d0e0f08090a0bULL),
                                        static_cast<long long>(0x0405060700010203ULL));

    /* 载入状态并重排成 ABEF / CDGH */
    tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[0]));
    state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[4]));
    tmp = _mm_shuffle_epi32(tmp, 0xB1);         /* CDAB */
    state1 = _mm_shuffle_epi32(state1, 0x1B);   /* EFGH */
    state0 = _mm_alignr_epi8(tmp, state1, 8);   /* ABEF */
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);/* CDGH */

    /* Rounds 0-3 */
    abef_save = state0;
    cdgh_save = state1;
    msg = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 0));
    msg0 = _mm_shuffle_epi8(msg, mask);
    msg = _mm_add_epi32(
        msg0, _mm_set_epi64x(static_cast<long long>(0xE9B5DBA5B5C0FBCFULL),
                             static_cast<long long>(0x71374491428A2F98ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Rounds 4-7 */
    msg1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16));
    msg1 = _mm_shuffle_epi8(msg1, mask);
    msg = _mm_add_epi32(
        msg1, _mm_set_epi64x(static_cast<long long>(0xAB1C5ED5923F82A4ULL),
                             static_cast<long long>(0x59F111F13956C25BULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg0 = _mm_sha256msg1_epu32(msg0, msg1);

    /* Rounds 8-11 */
    msg2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 32));
    msg2 = _mm_shuffle_epi8(msg2, mask);
    msg = _mm_add_epi32(
        msg2, _mm_set_epi64x(static_cast<long long>(0x550C7DC3243185BEULL),
                             static_cast<long long>(0x12835B01D807AA98ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg1 = _mm_sha256msg1_epu32(msg1, msg2);

    /* Rounds 12-15 */
    msg3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 48));
    msg3 = _mm_shuffle_epi8(msg3, mask);
    msg = _mm_add_epi32(
        msg3, _mm_set_epi64x(static_cast<long long>(0xC19BF1749BDC06A7ULL),
                             static_cast<long long>(0x80DEB1FE72BE5D74ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg3, msg2, 4);
    msg0 = _mm_add_epi32(msg0, tmp);
    msg0 = _mm_sha256msg2_epu32(msg0, msg3);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg2 = _mm_sha256msg1_epu32(msg2, msg3);

    /* Rounds 16-19 */
    msg = _mm_add_epi32(
        msg0, _mm_set_epi64x(static_cast<long long>(0x240CA1CC0FC19DC6ULL),
                             static_cast<long long>(0xEFBE4786E49B69C1ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg0, msg3, 4);
    msg1 = _mm_add_epi32(msg1, tmp);
    msg1 = _mm_sha256msg2_epu32(msg1, msg0);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg3 = _mm_sha256msg1_epu32(msg3, msg0);

    /* Rounds 20-23 */
    msg = _mm_add_epi32(
        msg1, _mm_set_epi64x(static_cast<long long>(0x76F988DA5CB0A9DCULL),
                             static_cast<long long>(0x4A7484AA2DE92C6FULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg1, msg0, 4);
    msg2 = _mm_add_epi32(msg2, tmp);
    msg2 = _mm_sha256msg2_epu32(msg2, msg1);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg0 = _mm_sha256msg1_epu32(msg0, msg1);

    /* Rounds 24-27 */
    msg = _mm_add_epi32(
        msg2, _mm_set_epi64x(static_cast<long long>(0xBF597FC7B00327C8ULL),
                             static_cast<long long>(0xA831C66D983E5152ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg2, msg1, 4);
    msg3 = _mm_add_epi32(msg3, tmp);
    msg3 = _mm_sha256msg2_epu32(msg3, msg2);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg1 = _mm_sha256msg1_epu32(msg1, msg2);

    /* Rounds 28-31 */
    msg = _mm_add_epi32(
        msg3, _mm_set_epi64x(static_cast<long long>(0x1429296706CA6351ULL),
                             static_cast<long long>(0xD5A79147C6E00BF3ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg3, msg2, 4);
    msg0 = _mm_add_epi32(msg0, tmp);
    msg0 = _mm_sha256msg2_epu32(msg0, msg3);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg2 = _mm_sha256msg1_epu32(msg2, msg3);

    /* Rounds 32-35 */
    msg = _mm_add_epi32(
        msg0, _mm_set_epi64x(static_cast<long long>(0x53380D134D2C6DFCULL),
                             static_cast<long long>(0x2E1B213827B70A85ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg0, msg3, 4);
    msg1 = _mm_add_epi32(msg1, tmp);
    msg1 = _mm_sha256msg2_epu32(msg1, msg0);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg3 = _mm_sha256msg1_epu32(msg3, msg0);

    /* Rounds 36-39 */
    msg = _mm_add_epi32(
        msg1, _mm_set_epi64x(static_cast<long long>(0x92722C8581C2C92EULL),
                             static_cast<long long>(0x766A0ABB650A7354ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg1, msg0, 4);
    msg2 = _mm_add_epi32(msg2, tmp);
    msg2 = _mm_sha256msg2_epu32(msg2, msg1);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg0 = _mm_sha256msg1_epu32(msg0, msg1);

    /* Rounds 40-43 */
    msg = _mm_add_epi32(
        msg2, _mm_set_epi64x(static_cast<long long>(0xC76C51A3C24B8B70ULL),
                             static_cast<long long>(0xA81A664BA2BFE8A1ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg2, msg1, 4);
    msg3 = _mm_add_epi32(msg3, tmp);
    msg3 = _mm_sha256msg2_epu32(msg3, msg2);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg1 = _mm_sha256msg1_epu32(msg1, msg2);

    /* Rounds 44-47 */
    msg = _mm_add_epi32(
        msg3, _mm_set_epi64x(static_cast<long long>(0x106AA070F40E3585ULL),
                             static_cast<long long>(0xD6990624D192E819ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg3, msg2, 4);
    msg0 = _mm_add_epi32(msg0, tmp);
    msg0 = _mm_sha256msg2_epu32(msg0, msg3);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg2 = _mm_sha256msg1_epu32(msg2, msg3);

    /* Rounds 48-51 */
    msg = _mm_add_epi32(
        msg0, _mm_set_epi64x(static_cast<long long>(0x34B0BCB52748774CULL),
                             static_cast<long long>(0x1E376C0819A4C116ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg0, msg3, 4);
    msg1 = _mm_add_epi32(msg1, tmp);
    msg1 = _mm_sha256msg2_epu32(msg1, msg0);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg3 = _mm_sha256msg1_epu32(msg3, msg0);

    /* Rounds 52-55 */
    msg = _mm_add_epi32(
        msg1, _mm_set_epi64x(static_cast<long long>(0x682E6FF35B9CCA4FULL),
                             static_cast<long long>(0x4ED8AA4A391C0CB3ULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg1, msg0, 4);
    msg2 = _mm_add_epi32(msg2, tmp);
    msg2 = _mm_sha256msg2_epu32(msg2, msg1);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Rounds 56-59 */
    msg = _mm_add_epi32(
        msg2, _mm_set_epi64x(static_cast<long long>(0x8CC7020884C87814ULL),
                             static_cast<long long>(0x78A5636F748F82EEULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg2, msg1, 4);
    msg3 = _mm_add_epi32(msg3, tmp);
    msg3 = _mm_sha256msg2_epu32(msg3, msg2);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Rounds 60-63 */
    msg = _mm_add_epi32(
        msg3, _mm_set_epi64x(static_cast<long long>(0xC67178F2BEF9A3F7ULL),
                             static_cast<long long>(0xA4506CEB90BEFFFAULL)));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* 累加回状态 */
    state0 = _mm_add_epi32(state0, abef_save);
    state1 = _mm_add_epi32(state1, cdgh_save);

    /* 还原成 A..H 顺序 */
    tmp = _mm_shuffle_epi32(state0, 0x1B);       /* FEBA */
    state1 = _mm_shuffle_epi32(state1, 0xB1);    /* DCHG */
    state0 = _mm_blend_epi16(tmp, state1, 0xF0); /* DCBA */
    state1 = _mm_alignr_epi8(state1, tmp, 8);    /* HGFE */

    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[0]), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[4]), state1);
}

/// @brief 批量压缩：把「每块一次函数调用」摊成一次调用（1 MiB 只需 1 次）。
__attribute__((target("sha,sse4.1"))) void CompressBlocksShaNi(
    std::uint32_t* state, const std::uint8_t* data, std::size_t blocks) noexcept {
    for (std::size_t i = 0; i < blocks; ++i) {
        CompressShaNi(state, data + i * 64);
    }
}

/// @brief 进程内只探测一次 CPUID（`__builtin_cpu_supports` 依赖 libgcc 的
///        `__cpu_model`，用函数局部静态量保证初始化顺序安全）。
bool ShaNiAvailable() noexcept {
    static const bool available = []() noexcept {
        __builtin_cpu_init();
        return __builtin_cpu_supports("sha") != 0;
    }();
    return available;
}

#endif  // GO2CPP_HASH_SHA_NI

/// @brief 是否启用 SHA-NI：CPU 支持且未被测试开关禁用。
///
/// `GO2CPP_HASH_DISABLE_SHA_NI` 是测试/排障开关：设置后强制走标量路径，
/// 用于在没有「无 SHA-NI 机器」的环境里验证回退路径的逐字节一致性。
bool ShaNiEnabled() noexcept {
#if GO2CPP_HASH_SHA_NI
    return std::getenv("GO2CPP_HASH_DISABLE_SHA_NI") == nullptr && ShaNiAvailable();
#else
    return false;
#endif
}

/// @brief 统一压缩入口：一次调用压缩 `blocks` 个块。
///
/// 标量分支保持同 TU 直接调用（编译器可内联），不给回退路径增加间接调用
/// 开销；硬件分支走带 `target("sha,sse4.1")` 的批量函数。
void CompressBlocks(std::uint32_t* state, const std::uint8_t* data,
                    std::size_t blocks) noexcept {
#if GO2CPP_HASH_SHA_NI
    static const bool sha_ni = ShaNiEnabled();
    if (sha_ni) {
        CompressBlocksShaNi(state, data, blocks);
        return;
    }
#endif
    CompressBlocksScalar(state, data, blocks);
}

}  // namespace

bool Sha256::HardwareAccelerated() noexcept {
    return ShaNiEnabled();
}

void Sha256::Reset() {
    for (std::size_t i = 0; i < 8; ++i) {
        m_state[i] = kInitialState[i];
    }
    m_length = 0;
    m_buffer_len = 0;
}

void Sha256::Transform(const std::uint8_t* block) noexcept {
    CompressBlocks(m_state, block, 1);
}

void Sha256::TransformRange(const std::uint8_t* data, std::size_t blocks) noexcept {
    CompressBlocks(m_state, data, blocks);
}

void Sha256::Write(ByteSpan p) {
    const std::uint8_t* data = p.data();
    std::size_t remaining = p.size();
    m_length += remaining;

    if (m_buffer_len != 0) {
        const std::size_t need = kBlockSize - m_buffer_len;
        const std::size_t take = remaining < need ? remaining : need;
        if (take != 0) {
            std::memcpy(m_buffer + m_buffer_len, data, take);
        }
        m_buffer_len += take;
        // 空 Write 时 p.data() 可以是 nullptr；C++ 对 nullptr 做 `+ 0`
        // 也是未定义行为，因此只有真的拷贝了字节才推进指针。
        if (take != 0) {
            data += take;
            remaining -= take;
        }
        if (m_buffer_len == kBlockSize) {
            Transform(m_buffer);
            m_buffer_len = 0;
        }
    }

    // 整块批量压缩：硬件路径下 1 MiB 只付一次函数调用/分派开销。
    while (remaining >= kBlockSize) {
        const std::size_t blocks = remaining / kBlockSize;
        TransformRange(data, blocks);
        const std::size_t consumed = blocks * kBlockSize;
        data += consumed;
        remaining -= consumed;
    }

    if (remaining != 0) {
        std::memcpy(m_buffer, data, remaining);
        m_buffer_len = remaining;
    }
}

void Sha256::Finish(std::uint8_t* out) noexcept {
    const std::uint64_t bit_length = m_length * 8;

    std::uint8_t pad[64] = {};
    pad[0] = 0x80;
    const std::size_t pad_len =
        m_buffer_len < 56 ? 56 - m_buffer_len : 120 - m_buffer_len;
    Write(ByteSpan(pad, pad_len));

    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (8 * (7 - i)));
    }
    Write(ByteSpan(length_bytes, 8));

    for (std::size_t i = 0; i < 8; ++i) {
        const std::uint32_t value = m_state[i];
        out[i * 4 + 0] = static_cast<std::uint8_t>(value >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(value >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(value >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(value);
    }
}

void Sha256::Sum(ByteSpan out) const {
    assert(out.size() >= kSize);
    Sha256 copy = *this;
    std::uint8_t digest[kSize];
    copy.Finish(digest);
    std::memcpy(out.data(), digest, kSize);
}

std::array<std::uint8_t, Sha256::kSize> Sha256::Digest(ByteSpan data) {
    std::uint32_t state[8];
    for (std::size_t i = 0; i < 8; ++i) {
        state[i] = kInitialState[i];
    }

    const std::uint8_t* bytes = data.data();
    const std::size_t size = data.size();
    const std::size_t full_blocks = size / kBlockSize;
    if (full_blocks != 0) {
        CompressBlocks(state, bytes, full_blocks);
    }

    // 尾部：剩余字节 + 0x80 + 0 填充 + 64 位大端比特长度（FIPS 180-4 §5.1.1）。
    const std::size_t remainder = size - full_blocks * kBlockSize;
    std::uint8_t tail[128] = {};
    if (remainder != 0) {
        std::memcpy(tail, bytes + full_blocks * kBlockSize, remainder);
    }
    tail[remainder] = 0x80u;
    const std::size_t padded = remainder < 56 ? 64 : 128;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(size) * 8u;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded - 1 - i] = static_cast<std::uint8_t>(bit_length >> (8 * i));
    }
    CompressBlocks(state, tail, padded / kBlockSize);

    std::array<std::uint8_t, kSize> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>(state[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
    }
    return out;
}

}  // namespace go2cpp::hash
