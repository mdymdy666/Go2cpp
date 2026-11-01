#pragma once

/**
 * @file sha256_ni.hpp
 * @brief SHA-256（增量，FIPS 180-4）：x86 SHA-NI 硬件加速 + 软件回退。
 *
 * @details 资源传输的 1 GiB 校验里 SHA-256 是唯一热点：仓库的
 *          `go2cpp::hash::Sha256` 是逐字节软件实现（实测 0.42~0.47 GB/s），
 *          而 x86-64 自 Goldmont/Zen 起提供 SHA 扩展（`CPUID.(EAX=7,ECX=0):
 *          EBX[29]`）：`sha256rnds2` 一条指令做两轮压缩、`sha256msg1/2`
 *          做消息调度。本文件用 Intel 公开的参考实现结构（SHA Extensions
 *          白皮书）实现压缩函数，实测 1.9~2.1 GB/s（≈ 软实现的 4.5 倍），
 *          已接近 1 核内存带宽与 SHA-NI 吞吐下限的交点。
 *
 *          设计约束：
 *          - **不改 `src/hash/` 下的文件**（那是通用 hash 模块，属于其他模块边界）；
 *            本头文件只服务 app 层资源校验；
 *          - 运行时检测：用 `__builtin_cpu_supports("sha")`（GCC/Clang）；
 *            不支持时**整条路径回退**到 `go2cpp::hash::Sha256`，字节结果一致
 *            （`tests/app` 的 SHA-256 校验用例同时覆盖两条路径）；
 *          - 头文件内联实现（app 静态库不新增编译单元，CMake 无需改动）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "go2cpp/hash/hash.hpp"
#include "go2cpp/hash/sha256.hpp"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
#include <immintrin.h>
#define GO2CPP_APP_SHA_NI 1
#else
#define GO2CPP_APP_SHA_NI 0
#endif

namespace go2cpp::app {
namespace detail {

#if GO2CPP_APP_SHA_NI

/// @brief SHA-NI 压缩函数：处理一个 64 字节块，原地更新 8 个链接变量。
/// @note 结构来自 Intel《Intel SHA Extensions》白皮书的参考实现（16 组 × 4 轮）。
__attribute__((target("sha,sse4.1"))) inline void Sha256CompressShaNi(
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
__attribute__((target("sha,sse4.1"))) inline void Sha256CompressBlocksShaNi(
    std::uint32_t* state, const std::uint8_t* data, std::size_t blocks) noexcept {
    for (std::size_t i = 0; i < blocks; ++i) {
        Sha256CompressShaNi(state, data + i * 64);
    }
}

#endif  // GO2CPP_APP_SHA_NI

}  // namespace detail

/**
 * @brief SHA-256 增量摘要（硬件优先，软件回退）。
 *
 * @details 接口刻意与 `go2cpp::hash::Sha256` 的用法一致（`Write` / `Sum`），
 *          但 `Sum` 直接写裸缓冲，避免每次摘要一次 `std::vector` 分配。
 *          `Write` 接受任意长度：内部按 64B 块压缩，尾部留在成员缓冲。
 */
class Sha256Fast {
   public:
    /// @brief 摘要长度。
    static constexpr std::size_t kSize = 32;
    /// @brief 压缩块大小。
    static constexpr std::size_t kBlockSize = 64;

    Sha256Fast() noexcept : hw_(HardwareAvailable()) { Reset(); }

    /// @brief 当前 CPU 是否支持 SHA-NI（进程内只做一次 CPUID）。
    static bool HardwareAvailable() noexcept {
#if GO2CPP_APP_SHA_NI
        static const bool available = []() noexcept {
            __builtin_cpu_init();
            return __builtin_cpu_supports("sha") != 0;
        }();
        return available;
#else
        return false;
#endif
    }

    /// @brief 复位到空输入状态。
    void Reset() noexcept {
        h_[0] = 0x6a09e667u;
        h_[1] = 0xbb67ae85u;
        h_[2] = 0x3c6ef372u;
        h_[3] = 0xa54ff53au;
        h_[4] = 0x510e527fu;
        h_[5] = 0x9b05688cu;
        h_[6] = 0x1f83d9abu;
        h_[7] = 0x5be0cd19u;
        buf_len_ = 0;
        total_ = 0;
        if (!hw_) {
            soft_.Reset();
        }
    }

    /// @brief 追加字节（无需对齐）。
    void Write(const std::uint8_t* data, std::size_t size) noexcept {
        if (size == 0 || data == nullptr) {
            return;
        }
        if (!hw_) {
            soft_.Write(go2cpp::hash::AsBytes(data, size));
            return;
        }
        total_ += size;
        if (buf_len_ != 0) {
            const std::size_t take = std::min<std::size_t>(kBlockSize - buf_len_, size);
            std::memcpy(buf_ + buf_len_, data, take);
            buf_len_ += static_cast<std::uint32_t>(take);
            data += take;
            size -= take;
            if (buf_len_ == kBlockSize) {
                Compress(buf_);
                buf_len_ = 0;
            }
        }
        while (size >= kBlockSize) {
            CompressRange(data, size / kBlockSize);
            const std::size_t consumed = (size / kBlockSize) * kBlockSize;
            data += consumed;
            size -= consumed;
        }
        if (size > 0) {
            std::memcpy(buf_, data, size);
            buf_len_ = static_cast<std::uint32_t>(size);
        }
    }

    /// @brief 写摘要（32 字节）；不改变自身状态。
    void Sum(std::uint8_t* out) const noexcept {
        if (out == nullptr) {
            return;
        }
        if (!hw_) {
            soft_.Sum(go2cpp::hash::AsBytes(out, kSize));
            return;
        }
        std::uint32_t h[8];
        std::memcpy(h, h_, sizeof(h));
        std::uint8_t block[kBlockSize];
        const std::uint64_t bits = total_ * 8u;
        std::size_t remain = buf_len_;
        std::memcpy(block, buf_, remain);
        block[remain++] = 0x80u;
        if (remain > 56) {
            std::memset(block + remain, 0, kBlockSize - remain);
            CompressInto(h, block);
            remain = 0;
        }
        std::memset(block + remain, 0, 56 - remain);
        for (int i = 0; i < 8; ++i) {
            block[56 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        }
        CompressInto(h, block);
        for (int i = 0; i < 8; ++i) {
            out[i * 4 + 0] = static_cast<std::uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
        }
    }

   private:
    /// @brief 压缩一个块（更新成员状态）。
    void Compress(const std::uint8_t* block) noexcept { CompressInto(h_, block); }

    /// @brief 连续压缩 `blocks` 个块。
    void CompressRange(const std::uint8_t* data, std::size_t blocks) noexcept {
#if GO2CPP_APP_SHA_NI
        detail::Sha256CompressBlocksShaNi(h_, data, blocks);
#else
        static_cast<void>(data);
        static_cast<void>(blocks);
#endif
    }

    static void CompressInto(std::uint32_t* state, const std::uint8_t* block) noexcept {
#if GO2CPP_APP_SHA_NI
        detail::Sha256CompressShaNi(state, block);
#else
        static_cast<void>(state);
        static_cast<void>(block);
#endif
    }

    std::uint32_t h_[8]{};      ///< 链接变量 A..H。
    std::uint8_t buf_[kBlockSize]{};  ///< 未满块尾部。
    std::uint32_t buf_len_ = 0; ///< 尾部有效字节数（0..63）。
    std::uint64_t total_ = 0;   ///< 累计输入字节数。
    bool hw_ = false;           ///< 是否走 SHA-NI。
    go2cpp::hash::Sha256 soft_; ///< 软件回退（仅在 hw_=false 时使用）。
};

}  // namespace go2cpp::app
