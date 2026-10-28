/**
 * @file x25519.cpp
 * @brief X25519（RFC 7748）Montgomery ladder：16×16-bit limb 域运算与标量乘法。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 域元素使用 16 个 16-bit limb（有符号 64 位承载）表示：
 *
 *          f = sum(f[i] * 2^(16*i)), i = 0..15。
 *
 *          这比 5×51-bit 表示慢一些，但加减乘的进位规则简单、容易审计，
 *          且不依赖 `__int128`。域运算采用 TweetNaCl 的 carry 技巧：
 *          每条 limb 加上 2^16 后右移取得进位，再减回 2^16；第 15 条
 *          limb 的进位按 2^256 ≡ 38 (mod 2^255-19) 折回第 0 条。
 *
 *          Montgomery ladder 的每轮固定执行 5 次乘法、4 次平方和若干加减，
 *          标量 bit 只用于 `Select` 的掩码交换，不参与分支。
 */

#include "go2cpp/bento/net/x25519.hpp"

#include <cstring>
#include <random>

namespace go2cpp::bento {
namespace {

using Limb = std::int64_t;
using Field = Limb[16];

/// 16-bit limb 掩码。
constexpr Limb kLimbMask = 0xFFFF;

/// 2^256 mod (2^255 - 19) = 38。
constexpr Limb kCarryFactor = 38;

/// 曲线常数 a24 = 121665，按 16-bit limb 拆成 0xDB41 + 1 * 2^16。
constexpr Field kA24 = {0xDB41, 1};

/**
 * @brief 把可能超出 16-bit 的 limb 归一化到 [0, 65535]。
 *
 * @details 每条 limb 先加上 2^16，使负数也能用算术右移得到正确进位；
 *          进位写入下一条 limb，第 15 条 limb 的进位折回第 0 条并乘 38。
 *          一次调用后 limb 仍在有符号 64 位范围内，两次调用足以让
 *          乘法结果完全归一化。
 */
void Carry(Field value) noexcept {
    for (int i = 0; i < 16; ++i) {
        value[i] += (1LL << 16);
        const Limb carry = value[i] >> 16;
        const int next = (i + 1) & 15;
        const Limb factor = (i == 15) ? kCarryFactor : 1;
        value[next] += factor * (carry - 1);
        value[i] -= carry * (1LL << 16);
    }
}

/// 常数时间条件交换：bit 为 1 时交换 p、q，为 0 时不变。
void Select(Field p, Field q, int bit) noexcept {
    const Limb mask = ~static_cast<Limb>(bit - 1);
    for (int i = 0; i < 16; ++i) {
        const Limb difference = mask & (p[i] ^ q[i]);
        p[i] ^= difference;
        q[i] ^= difference;
    }
}

/// 域加法（不做进位，允许 limb 暂时超出 16 bit）。
void Add(Field out, const Field a, const Field b) noexcept {
    for (int i = 0; i < 16; ++i) {
        out[i] = a[i] + b[i];
    }
}

/// 域减法（不做借位，允许 limb 暂时为负）。
void Sub(Field out, const Field a, const Field b) noexcept {
    for (int i = 0; i < 16; ++i) {
        out[i] = a[i] - b[i];
    }
}

/// 域乘法：先算 31 条 limb 的卷积，把高 15 条按 38 折回，再两次进位。
void Mul(Field out, const Field a, const Field b) noexcept {
    Limb product[31] = {};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            product[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; ++i) {
        product[i] += kCarryFactor * product[i + 16];
    }
    for (int i = 0; i < 16; ++i) {
        out[i] = product[i];
    }
    Carry(out);
    Carry(out);
}

/// 域平方。
void Square(Field out, const Field a) noexcept {
    Mul(out, a, a);
}

/// 域求逆：out = in^(p-2)，使用固定平方-乘法链（指数公开）。
void Invert(Field out, const Field in) noexcept {
    Field value;
    for (int i = 0; i < 16; ++i) {
        value[i] = in[i];
    }
    for (int bit = 253; bit >= 0; --bit) {
        Square(value, value);
        if (bit != 2 && bit != 4) {
            Mul(value, value, in);
        }
    }
    for (int i = 0; i < 16; ++i) {
        out[i] = value[i];
    }
}

/**
 * @brief 把域元素编码成 32 字节小端，并做模 p 的完全约简。
 *
 * @details 先做三次进位；再用两次「减 p」的尝试把 [p, 2^255) 的值
 *          拉回 [0, p)。是否发生借位用 `Select` 的掩码选择，避免
 *          依赖输入值做分支。
 */
void Pack(std::uint8_t out[kX25519KeyBytes], const Field n) noexcept {
    Field value;
    for (int i = 0; i < 16; ++i) {
        value[i] = n[i];
    }
    Carry(value);
    Carry(value);
    Carry(value);

    Field reduced;
    for (int round = 0; round < 2; ++round) {
        reduced[0] = value[0] - 0xFFED;
        for (int i = 1; i < 15; ++i) {
            reduced[i] = value[i] - kLimbMask - ((reduced[i - 1] >> 16) & 1);
            reduced[i - 1] &= kLimbMask;
        }
        reduced[15] = value[15] - 0x7FFF - ((reduced[14] >> 16) & 1);
        const Limb borrow = (reduced[15] >> 16) & 1;
        reduced[14] &= kLimbMask;
        Select(value, reduced, 1 - static_cast<int>(borrow));
    }

    for (int i = 0; i < 16; ++i) {
        out[2 * i] = static_cast<std::uint8_t>(value[i] & 0xFF);
        out[2 * i + 1] = static_cast<std::uint8_t>((value[i] >> 8) & 0xFF);
    }
}

/// 把 32 字节小端编码解成域元素；按 RFC 7748 忽略最高位。
void Unpack(Field out, const std::uint8_t in[kX25519KeyBytes]) noexcept {
    for (int i = 0; i < 16; ++i) {
        out[i] = static_cast<Limb>(in[2 * i]) |
                 (static_cast<Limb>(in[2 * i + 1]) << 8);
    }
    out[15] &= 0x7FFF;
}

/// 常数时间判断一段内存是否全零。
bool AllZero(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint8_t accumulator = 0;
    for (std::size_t i = 0; i < size; ++i) {
        accumulator |= data[i];
    }
    return accumulator == 0;
}

/**
 * @brief RFC 7748 §5 的 X25519 标量乘法。
 *
 * @param out 32B 输出 u 坐标。
 * @param scalar 32B 标量（函数内部 clamp，不修改调用方缓冲）。
 * @param point 32B 输入 u 坐标（最高位忽略）。
 *
 * @note 结果是仿射 u 坐标；若结果为全零，调用方需要自行拒绝（见
 *       `X25519ComputeSharedSecret`）。
 */
void ScalarMult(std::uint8_t out[kX25519KeyBytes],
                const std::uint8_t scalar[kX25519KeyBytes],
                const std::uint8_t point[kX25519KeyBytes]) noexcept {
    std::uint8_t clamped[kX25519KeyBytes];
    std::memcpy(clamped, scalar, kX25519KeyBytes);
    clamped[0] &= 248;
    clamped[31] &= 127;
    clamped[31] |= 64;

    Field x;
    Field a = {};
    Field b = {};
    Field c = {};
    Field d = {};
    Field e = {};
    Field f = {};
    Unpack(x, point);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
    }
    a[0] = 1;
    d[0] = 1;

    for (int bit_index = 254; bit_index >= 0; --bit_index) {
        const int bit = (clamped[bit_index >> 3] >> (bit_index & 7)) & 1;
        Select(a, b, bit);
        Select(c, d, bit);

        Add(e, a, c);
        Sub(a, a, c);
        Add(c, b, d);
        Sub(b, b, d);
        Square(d, e);
        Square(f, a);
        Mul(a, c, a);
        Mul(c, b, e);
        Add(e, a, c);
        Sub(a, a, c);
        Square(b, a);
        Sub(c, d, f);
        Mul(a, c, kA24);
        Add(a, a, d);
        Mul(c, c, a);
        Mul(a, d, f);
        Mul(d, b, x);
        Square(b, e);

        Select(a, b, bit);
        Select(c, d, bit);
    }

    Invert(c, c);
    Mul(a, a, c);
    Pack(out, a);
}

/// 用 std::random_device 填满缓冲；每次取 32 位，按小端拆字节。
void FillRandom(std::uint8_t* out, std::size_t size) {
    std::random_device device;
    std::size_t offset = 0;
    while (offset < size) {
        const unsigned int value = device();
        for (int byte = 0; byte < 4 && offset < size; ++byte, ++offset) {
            out[offset] = static_cast<std::uint8_t>(value >> (8 * byte));
        }
    }
}

}  // namespace

void X25519GeneratePrivateKey(std::uint8_t private_key[kX25519KeyBytes]) {
    if (private_key == nullptr) {
        return;
    }
    FillRandom(private_key, kX25519KeyBytes);
}

bool X25519PublicKeyFromPrivate(const std::uint8_t private_key[kX25519KeyBytes],
                                std::uint8_t public_key[kX25519KeyBytes]) noexcept {
    if (private_key == nullptr || public_key == nullptr) {
        return false;
    }
    ScalarMult(public_key, private_key, kX25519BasePoint);
    return true;
}

bool X25519ComputeSharedSecret(const std::uint8_t private_key[kX25519KeyBytes],
                               const std::uint8_t peer_public_key[kX25519KeyBytes],
                               std::uint8_t shared_secret[kX25519KeyBytes]) noexcept {
    if (private_key == nullptr || peer_public_key == nullptr || shared_secret == nullptr) {
        return false;
    }
    ScalarMult(shared_secret, private_key, peer_public_key);
    if (AllZero(shared_secret, kX25519KeyBytes)) {
        std::memset(shared_secret, 0, kX25519KeyBytes);
        return false;
    }
    return true;
}

}  // namespace go2cpp::bento
