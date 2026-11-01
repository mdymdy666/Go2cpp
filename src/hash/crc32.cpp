/**
 * @file crc32.cpp
 * @brief `go2cpp::hash::Crc32` 的实现（x86-64 硬件加速 + slicing-by-8 回退）。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18（硬件路径 2026-11-01）
 * @copyright BSD-3-Clause（语义对照 Go `hash/crc32`，实现独立完成；
 *            IEEE 折叠算法参照 Intel 白皮书《Fast CRC Computation for Generic
 *            Polynomials Using PCLMULQDQ》，常量与 fold/Barrett 归约结构与
 *            zlib-ng `arch/x86/crc32_pclmulqdq_tpl.h`（Zlib 许可证）一致）
 *
 * @details 三条路径，输出必须逐字节一致：
 *
 *   | 路径 | 触发条件 | 实现 |
 *   |---|---|---|
 *   | CRC-32C 硬件 | `table.polynomial == Castagnoli` 且 CPU 有 SSE4.2 | `crc32q` 8 字节/条 |
 *   | IEEE 硬件 | `table.polynomial == IEEE` 且 CPU 有 PCLMUL+SSE4.1 | 128 位折叠 + Barrett 归约 |
 *   | 表驱动 | 其它 | slicing-by-8 |
 *
 * 分派在 `Update` 入口做，不在循环里做；硬件函数用函数级
 * `__attribute__((target(...)))` 编译，因此 `-O3` 的全局选项不必改动，
 * 也不会把 PCLMUL 指令泄漏到不支持的老 CPU 上（运行期 CPUID 决定是否调用）。
 *
 * @note `crc32q` 的多项式**固定是 CRC-32C**，不能用于 IEEE。IEEE 的硬件
 *       加速只能走 PCLMULQDQ 折叠，这点在头文件里有表格说明。
 */

#include "go2cpp/hash/crc32.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>  // SSE4.2：_mm_crc32_u8/u64
#include <smmintrin.h>  // SSE4.1：_mm_blend_epi16 / _mm_extract_epi32
#include <wmmintrin.h>  // PCLMUL：_mm_clmulepi64_si128
#define GO2CPP_CRC32_X86_64 1
#endif

namespace go2cpp::hash {

// 三个内建表都是 constexpr 生成，属于常量初始化（不做运行期计算）。
const Table IEEETable = MakeTable(IEEE);
const Table CastagnoliTable = MakeTable(Castagnoli);
const Table KoopmanTable = MakeTable(Koopman);

namespace {

/// 读取 4 字节 little-endian（CRC 的 32 位字按最低位先算，等价于 LE 读入）。
inline std::uint32_t LoadLE32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

/// 表驱动主体：slicing-by-8，任意表、任意长度（唯一可移植路径）。
std::uint32_t UpdateTable(std::uint32_t crc, const Table& table,
                          const std::uint8_t* p, std::size_t remaining) noexcept {
    const std::uint32_t* const t = table.entries.data();

    // 与 Go 一致：进入时取反，离开时再取反。调用方传的 crc 是「已完成」的值。
    crc = ~crc;

    // slicing-by-8：一次消费 8 字节，8 张表分别处理 4 个 CRC 字节与 4 个数据字节。
    while (remaining >= 8) {
        crc ^= LoadLE32(p);
        crc = t[7 * 256 + (crc & 0xffu)] ^
              t[6 * 256 + ((crc >> 8) & 0xffu)] ^
              t[5 * 256 + ((crc >> 16) & 0xffu)] ^
              t[4 * 256 + ((crc >> 24) & 0xffu)] ^
              t[3 * 256 + p[4]] ^
              t[2 * 256 + p[5]] ^
              t[1 * 256 + p[6]] ^
              t[0 * 256 + p[7]];
        p += 8;
        remaining -= 8;
    }

    // 尾部不足 8 字节，退回基础 256 表逐字节处理。
    while (remaining != 0) {
        crc = t[(crc ^ *p) & 0xffu] ^ (crc >> 8);
        ++p;
        --remaining;
    }

    return ~crc;
}

#if defined(GO2CPP_CRC32_X86_64)

// 编译期已全局启用对应指令集时（`-msse4.2` / `-mpclmul`），不需要 target 属性，
// 也不需要运行期 CPUID；否则用函数级 target 属性 + 运行期分派。这个分支就是
// 任务要求保留的 `#if defined(__SSE4_2__)` 语义：全局宏打开就走编译期直连。
#if defined(__SSE4_2__)
#define GO2CPP_CRC32_SSE42_FN
#define GO2CPP_CRC32_SSE42_ALWAYS 1
#else
#define GO2CPP_CRC32_SSE42_FN __attribute__((target("sse4.2")))
#endif

#if defined(__PCLMUL__) && defined(__SSE4_1__)
#define GO2CPP_CRC32_PCLMUL_FN
#define GO2CPP_CRC32_PCLMUL_ALWAYS 1
#else
#define GO2CPP_CRC32_PCLMUL_FN __attribute__((target("sse4.1,pclmul")))
#endif

/// 一次性 CPU 特性探测。`__cpu_model` 由 libgcc 在 main 之前初始化，
/// 这里再显式 init 一次是为了覆盖「静态初始化期就调用 CRC」的极端场景。
bool CpuSupportsSse42() noexcept {
#if defined(GO2CPP_CRC32_SSE42_ALWAYS)
    return true;
#else
    static const bool kSupported = (__builtin_cpu_init(), __builtin_cpu_supports("sse4.2") != 0);
    return kSupported;
#endif
}

bool CpuSupportsPclmul() noexcept {
#if defined(GO2CPP_CRC32_PCLMUL_ALWAYS)
    return true;
#else
    static const bool kSupported =
        (__builtin_cpu_init(),
         __builtin_cpu_supports("pclmul") != 0 && __builtin_cpu_supports("sse4.1") != 0);
    return kSupported;
#endif
}

/// 128 位折叠的一步（zlib-ng `fold_state_4`）：四路并行状态各乘 x^128。
GO2CPP_CRC32_PCLMUL_FN inline void FoldState4(__m128i* c0, __m128i* c1, __m128i* c2, __m128i* c3,
                       __m128i k) noexcept {
    __m128i l0 = _mm_clmulepi64_si128(*c0, k, 0x01);
    __m128i h0 = _mm_clmulepi64_si128(*c0, k, 0x10);
    __m128i l1 = _mm_clmulepi64_si128(*c1, k, 0x01);
    __m128i h1 = _mm_clmulepi64_si128(*c1, k, 0x10);
    __m128i l2 = _mm_clmulepi64_si128(*c2, k, 0x01);
    __m128i h2 = _mm_clmulepi64_si128(*c2, k, 0x10);
    __m128i l3 = _mm_clmulepi64_si128(*c3, k, 0x01);
    __m128i h3 = _mm_clmulepi64_si128(*c3, k, 0x10);
    *c0 = _mm_xor_si128(l0, h0);
    *c1 = _mm_xor_si128(l1, h1);
    *c2 = _mm_xor_si128(l2, h2);
    *c3 = _mm_xor_si128(l3, h3);
}

/// 单路折叠一步：把 c0 折进 c3，其余状态前移（zlib-ng `fold_state_1`）。
GO2CPP_CRC32_PCLMUL_FN inline void FoldState1(__m128i* c0, __m128i* c1, __m128i* c2, __m128i* c3,
                       __m128i k) noexcept {
    __m128i l = _mm_clmulepi64_si128(*c0, k, 0x01);
    __m128i h = _mm_clmulepi64_si128(*c0, k, 0x10);
    *c0 = *c1;
    *c1 = *c2;
    *c2 = *c3;
    *c3 = _mm_xor_si128(l, h);
}

GO2CPP_CRC32_PCLMUL_FN inline void FoldState2(__m128i* c0, __m128i* c1, __m128i* c2, __m128i* c3,
                       __m128i k) noexcept {
    __m128i l0 = _mm_clmulepi64_si128(*c0, k, 0x01);
    __m128i h0 = _mm_clmulepi64_si128(*c0, k, 0x10);
    __m128i l1 = _mm_clmulepi64_si128(*c1, k, 0x01);
    __m128i h1 = _mm_clmulepi64_si128(*c1, k, 0x10);
    *c0 = *c2;
    *c1 = *c3;
    *c2 = _mm_xor_si128(l0, h0);
    *c3 = _mm_xor_si128(l1, h1);
}

GO2CPP_CRC32_PCLMUL_FN inline void FoldState3(__m128i* c0, __m128i* c1, __m128i* c2, __m128i* c3,
                       __m128i k) noexcept {
    __m128i l0 = _mm_clmulepi64_si128(*c0, k, 0x01);
    __m128i h0 = _mm_clmulepi64_si128(*c0, k, 0x10);
    __m128i l1 = _mm_clmulepi64_si128(*c1, k, 0x01);
    __m128i h1 = _mm_clmulepi64_si128(*c1, k, 0x10);
    __m128i l2 = _mm_clmulepi64_si128(*c2, k, 0x01);
    __m128i h2 = _mm_clmulepi64_si128(*c2, k, 0x10);
    *c0 = *c3;
    *c1 = _mm_xor_si128(l0, h0);
    *c2 = _mm_xor_si128(l1, h1);
    *c3 = _mm_xor_si128(l2, h2);
}

/**
 * @brief IEEE 多项式折叠：处理 16 字节整数倍前缀，返回**已完成** CRC。
 * @param crc 进入时的已完成 CRC（可能是分段计算的前半段）。
 * @param blocks 前缀长度，必须是 16 的整数倍且 >= 16。
 *
 * @details 结构逐段对应 Intel 白皮书：4 路并行折叠（每轮 64B）→ 归并到 128 位
 *          → Barrett 归约到 32 位。初始状态 `0x9db42487` 是「CRC 初值 0xFFFFFFFF
 *          对应的折叠种子」，`crc != 0` 时先把首块 16B 与 crc 异或吸收进状态。
 */
GO2CPP_CRC32_PCLMUL_FN
std::uint32_t FoldIeeePrefix(std::uint32_t crc, const std::uint8_t* src,
                             std::size_t blocks) noexcept {
    const __m128i fold4 = _mm_set_epi32(0x00000001, 0x54442bd4, 0x00000001, 0xc6e41596);
    const __m128i k12 = _mm_set_epi32(0x00000001, 0x751997d0, 0x00000000, 0xccaa009e);
    const __m128i barrett = _mm_set_epi32(0x00000001, 0xdb710640, 0xb4e5b025, 0xf7011641);

    __m128i c0 = _mm_cvtsi32_si128(0x9db42487);
    __m128i c1 = _mm_setzero_si128();
    __m128i c2 = _mm_setzero_si128();
    __m128i c3 = _mm_setzero_si128();

    std::size_t len = blocks;
    if (crc != 0) {
        // 首块吸收已有 CRC：先推进一拍，再把数据与 crc 异或进最新的状态。
        len -= 16;
        __m128i t0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        src += 16;
        FoldState1(&c0, &c1, &c2, &c3, fold4);
        c3 = _mm_xor_si128(_mm_xor_si128(c3, t0), _mm_cvtsi32_si128(static_cast<int>(crc)));
    }

    while (len >= 64) {
        len -= 64;
        __m128i t0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        __m128i t1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 16));
        __m128i t2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 32));
        __m128i t3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 48));
        src += 64;
        FoldState4(&c0, &c1, &c2, &c3, fold4);
        c0 = _mm_xor_si128(c0, t0);
        c1 = _mm_xor_si128(c1, t1);
        c2 = _mm_xor_si128(c2, t2);
        c3 = _mm_xor_si128(c3, t3);
    }

    if (len >= 48) {
        __m128i t0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        __m128i t1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 16));
        __m128i t2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 32));
        FoldState3(&c0, &c1, &c2, &c3, fold4);
        c1 = _mm_xor_si128(c1, t0);
        c2 = _mm_xor_si128(c2, t1);
        c3 = _mm_xor_si128(c3, t2);
    } else if (len >= 32) {
        __m128i t0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        __m128i t1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 16));
        FoldState2(&c0, &c1, &c2, &c3, fold4);
        c2 = _mm_xor_si128(c2, t0);
        c3 = _mm_xor_si128(c3, t1);
    } else if (len >= 16) {
        __m128i t0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        FoldState1(&c0, &c1, &c2, &c3, fold4);
        c3 = _mm_xor_si128(c3, t0);
    }

    // 四路状态归并成一 128 位值（k12 = x^128 / x^192 mod P 的反射常量）。
    __m128i l0 = _mm_clmulepi64_si128(c0, k12, 0x01);
    __m128i h0 = _mm_clmulepi64_si128(c0, k12, 0x10);
    c1 = _mm_xor_si128(_mm_xor_si128(c1, l0), h0);
    __m128i l1 = _mm_clmulepi64_si128(c1, k12, 0x01);
    __m128i h1 = _mm_clmulepi64_si128(c1, k12, 0x10);
    c2 = _mm_xor_si128(_mm_xor_si128(c2, l1), h1);
    __m128i l2 = _mm_clmulepi64_si128(c2, k12, 0x01);
    __m128i h2 = _mm_clmulepi64_si128(c2, k12, 0x10);
    c3 = _mm_xor_si128(_mm_xor_si128(c3, l2), h2);

    // 两阶段 Barrett 归约：128 位 → 32 位余式。
    __m128i tmp0 = _mm_clmulepi64_si128(c3, barrett, 0x00);
    __m128i tmp1 = _mm_clmulepi64_si128(tmp0, barrett, 0x10);
    tmp1 = _mm_blend_epi16(tmp1, _mm_setzero_si128(), 0xcf);
    tmp0 = _mm_xor_si128(tmp1, c3);
    __m128i res_a = _mm_clmulepi64_si128(tmp0, barrett, 0x01);
    __m128i res_b = _mm_clmulepi64_si128(res_a, barrett, 0x10);
    return ~static_cast<std::uint32_t>(_mm_extract_epi32(res_b, 2));
}

/// CRC-32C 硬件路径：`crc32q` 一次吃 8 字节；剩余 <8 字节用 `crc32b` 收尾。
GO2CPP_CRC32_SSE42_FN
std::uint32_t Crc32cPrefix(std::uint32_t crc, const std::uint8_t* p,
                           std::size_t n) noexcept {
    std::uint64_t state = ~static_cast<std::uint64_t>(crc);
    while (n >= 8) {
        std::uint64_t word;
        // memcpy 在 x86-64 上编译成一条 unaligned mov，不引入函数调用；
        // 直接解引用 uint64_t* 会违反严格别名/对齐规则。
        std::memcpy(&word, p, sizeof(word));
        state = _mm_crc32_u64(state, word);
        p += 8;
        n -= 8;
    }
    std::uint32_t state32 = static_cast<std::uint32_t>(state);
    while (n != 0) {
        state32 = _mm_crc32_u8(state32, *p);
        ++p;
        --n;
    }
    return ~state32;
}

#endif  // GO2CPP_CRC32_X86_64

/// IEEE 折叠的最小长度：小于它时函数调用/收尾开销盖过向量化收益。
constexpr std::size_t kIeeeFoldMinBytes = 64;

}  // namespace

std::uint32_t Crc32::Update(std::uint32_t crc, const Table& table,
                            ByteSpan data) noexcept {
    const std::uint8_t* p = data.data();
    const std::size_t n = data.size();
    if (n == 0) {
        return crc;
    }

#if defined(GO2CPP_CRC32_X86_64)
    if (table.polynomial == IEEE && n >= kIeeeFoldMinBytes && CpuSupportsPclmul()) {
        const std::size_t blocks = n & ~static_cast<std::size_t>(15);
        const std::uint32_t head = FoldIeeePrefix(crc, p, blocks);
        // 尾部（<16B）交回表驱动：折叠路径不必为 partial block 写移位分支。
        return UpdateTable(head, table, p + blocks, n - blocks);
    }
    if (table.polynomial == Castagnoli && n >= 8 && CpuSupportsSse42()) {
        return Crc32cPrefix(crc, p, n);
    }
#endif

    return UpdateTable(crc, table, p, n);
}

std::uint32_t Crc32::ChecksumIEEE(ByteSpan data) noexcept {
    return Update(0, IEEETable, data);
}

std::uint32_t Crc32::Checksum(ByteSpan data, const Table& table) noexcept {
    return Update(0, table, data);
}

void Crc32::Sum(ByteSpan out) const {
    assert(out.size() >= kSize);
    // 与 Go `crc32.digest.Sum` 相同：大端写出。
    out[0] = static_cast<std::uint8_t>(m_crc >> 24);
    out[1] = static_cast<std::uint8_t>(m_crc >> 16);
    out[2] = static_cast<std::uint8_t>(m_crc >> 8);
    out[3] = static_cast<std::uint8_t>(m_crc);
}

}  // namespace go2cpp::hash
