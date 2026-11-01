/**
 * @file crypto_rand.hpp
 * @brief Go `crypto/rand` 包的 C++ 对应物（Read / Int / Prime）。
 *
 * @author DeepSeek Harness
 * @date 2026-11-01
 * @copyright BSD-3-Clause（语义对照 Go `crypto/rand`，实现独立完成）
 *
 * @details
 * Go 的 `crypto/rand` 是**密码学安全**随机源，接口只有六个符号，其中本文件
 * 实现最常用的三个：
 *
 * ```go
 * import "crypto/rand"
 *
 * buf := make([]byte, 32)
 * n, err := rand.Read(buf)
 *
 * max := big.NewInt(1000)
 * v, err := rand.Int(rand.Reader, max)      // 0 <= v < max
 * p, err := rand.Prime(rand.Reader, 512)    // 512 位素数
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * #include "go2cpp/go/crypto_rand.hpp"
 *
 * std::uint8_t buf[32];
 * std::size_t n = go::crypto::rand::Read(buf, sizeof buf);
 *
 * go::crypto::rand::BigResult v = go::crypto::rand::Int(
 *     go::crypto::rand::BigInt::FromUint64(1000));
 * if (v.Ok()) { auto value = v.value; }
 *
 * go::crypto::rand::BigResult p = go::crypto::rand::Prime(512);
 * ```
 *
 * ## 熵源：getrandom(2) 优先，/dev/urandom 兜底
 *
 * Linux 上优先用 `getrandom(2)`（与 Go 1.24+ 的 `crypto/rand` 一致：内核
 * CSPRNG，不需要文件描述符，也不会被 `chroot`/`ulimit -n` 影响）；`getrandom`
 * 不可用（旧内核返回 `ENOSYS`）或返回短读时回退到 `/dev/urandom`。回退路径
 * 只打开一次 fd 并缓存，避免每条随机数都 `open(2)`。
 *
 * **为什么不用 `std::random_device`**：libstdc++ 的 `random_device` 在
 * Linux 上确实读 `/dev/urandom`，但标准只保证它是「非确定性随机源」的
 * 可选实现，且没有 `getrandom` 路径；作为密码学接口的底座，显式调用内核
 * 接口比依赖标准库实现更可审计。
 *
 * ## 与 Go 的对应关系
 *
 * | Go | 本文件 | 备注 |
 * |---|---|---|
 * | `rand.Reader io.Reader` | `rand::Reader()` → `Source&` | 进程级全局 CSPRNG |
 * | `rand.Read(b []byte) (int, error)` | `Read(buf, size)` → `std::size_t` | 保证读满；失败返回已读字节数 |
 * | `rand.Int(r io.Reader, max *big.Int)` | `Int(source, max)` → `BigResult` | `0 <= v < max`，拒绝采样 |
 * | `rand.Prime(r io.Reader, bits int)` | `Prime(source, bits)` → `BigResult` | 最高两位为 1（Go 同样保证） |
 * | `rand.Text()`（Go 1.24） | 未提供 | 只是一个 base32 便捷函数，不在本次缺口 |
 * | `crypto/rand.Read` 的 `err` | `BigResult::err` / 返回字节数 | C++ 无 `(n, err)` 元组，见下 |
 *
 * ## 语义边界（必须知道）
 *
 * - **`Read` 的返回值**：Go 保证「要么读满 `len(b)`，要么返回 error」；
 *   C++ 的 `Read(buf, size)` 返回**实际读到的字节数**，正常时等于 `size`。
 *   调用方用 `if (Read(buf, n) != n)` 判断失败——比检查 error 更直接。
 * - **`Int`/`Prime` 的错误**：Go 返回 `(*big.Int, error)`；C++ 返回
 *   `BigResult{value, err}`，`err == nullptr` 表示成功（与项目里
 *   `io.ReadResult{n, err}` 的形态一致，不用异常表达「可预期的参数错误」）。
 * - **`Int` 的均匀性**：与 Go 完全相同——取 `max.BitLen()` 位，最高字节
 *   掩码后做拒绝采样，**不做取模**（取模会引入偏置）。
 * - **`Prime` 的位长**：与 Go 相同——最高两位置 1，保证两个这样的素数之积
 *   恰好是 `2*bits` 位（RSA 的常见要求）。`bits < 2` 返回错误。
 * - **素数判定**：`ProbablyPrime` 用「小素数试除 + Miller-Rabin」。
 *   对 `< 2^64` 的输入，前 12 个素数底数是**确定性**的；更大的输入用
 *   固定底数序列（前 `reps` 个素数）做概率判定，误判概率上界 `4^-reps`。
 *   Go 的 `ProbablyPrime(20)` 额外做 Baillie-PSW（Lucas 测试），本实现
 *   用更多轮 MR 替代（20 轮 MR 的误判率已经低于 2^-40）。
 * - **`BigInt` 是无符号的**：Go 的 `big.Int` 有符号；`crypto/rand` 用到的
 *   只有非负值（`Int`/`Prime` 的参数与结果都非负），因此这里只实现无符号
 *   形态。`Sign()` 返回 0 或 1（永远不会是 -1），`Sub` 要求被减数不小于
 *   减数（与 `big.Int` 的 `Sub` 不同，见方法注释）。
 * - **性能**：`BigInt` 是 32 位 limb 的小学乘法 + Montgomery 模幂，定位是
 *   「测试与轻量密钥生成」；生成 2048 位素数约需数十毫秒，不追求
 *   GMP 级吞吐。需要批量大数运算请接 GMP/OpenSSL。
 *
 * ## 安全提示
 *
 * `Read` 的失败是**静默**的（返回短读）：调用方必须检查返回值。密码学场景
 * 下把短读当成功会直接用未初始化内存，务必写成 `if (Read(...) != n) 报错`。
 * 本文件的 `Prime`/`Int` 内部已经检查短读并返回 error。
 */

#pragma once

#include "go2cpp/go/error.go.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#else
#include <mutex>
#include <random>
#endif

namespace go2cpp::go {
namespace crypto {
namespace rand {

// ============================================================================
// 随机源（对应 io.Reader）
// ============================================================================

/**
 * @brief 随机字节源，对应 Go 的 `io.Reader`。
 *
 * @details Go 的 `crypto/rand.Int`/`Prime` 接受任意 `io.Reader`，测试里可以
 *          注入确定性 reader 来复现失败路径。C++ 侧保留这个能力：`Source`
 *          是抽象基类，生产实现是 `OsSource`，测试可以实现计数器/固定缓冲。
 *
 * ## 契约
 *
 * - `Read` 返回实际写入 `out` 的字节数（0 表示 EOF 或错误）；
 * - **允许短读**：调用方（本文件的 `Read` 包装）会循环到读满或返回 0 为止；
 * - 实现必须线程安全（`Reader()` 返回的全局实例会被多线程共享）。
 */
class Source {
public:
    virtual ~Source() = default;

    /**
     * @brief 读取至多 `size` 字节到 `out`。
     * @param out 目标缓冲（非空，`size > 0`）。
     * @param size 请求字节数。
     * @return 实际读到的字节数；0 表示 EOF/错误。
     */
    virtual std::size_t Read(std::uint8_t* out, std::size_t size) = 0;
};

/**
 * @brief 操作系统 CSPRNG：Linux 用 `getrandom(2)`，回退 `/dev/urandom`。
 *
 * @details 线程安全（`getrandom` 本身是系统调用；`/dev/urandom` 的 fd 用
 *          函数内静态变量初始化，C++11 起线程安全）。
 */
class OsSource final : public Source {
public:
    std::size_t Read(std::uint8_t* out, std::size_t size) override;
};

/// 进程级 CSPRNG，对应 Go 的 `crypto/rand.Reader`。
Source& Reader();

// ============================================================================
// BigInt：crypto/rand 需要的最小无符号大整数
// ============================================================================

/**
 * @brief 无符号任意精度整数（32 位 limb，小端）。
 *
 * @details 只实现 `crypto/rand` 需要的子集：比较、加减乘、移位、取模、
 *          进制文本、字节转换、Miller-Rabin。**没有**有符号语义、没有
 *          `Div` 的完整商（只暴露 `Mod`），这是刻意的范围控制。
 *
 * ## 不变量
 *
 * - `m_limbs` 的小端表示**没有前导零 limb**：零值就是空 vector；
 * - 所有运算的返回值都重新规范化，因此 `Cmp` 可以只比 `size()` 与高位；
 * - 对象可平凡拷贝，跨线程只读共享是安全的。
 */
class BigInt final {
public:
    BigInt() = default;

    /// 由 64 位无符号数构造。
    static BigInt FromUint64(std::uint64_t value);

    /// 由**大端**字节序列构造（对应 `big.Int.SetBytes`）。
    static BigInt FromBytes(const std::uint8_t* bytes, std::size_t size);

    /// 由十六进制字符串构造（大小写均可，可带 `0x` 前缀）。
    static BigInt FromHex(std::string_view text);

    /// 由小端 32 位 limb 序列构造（**内部/测试用**：会重新规范化）。
    static BigInt FromLimbs(std::vector<std::uint32_t> limbs);

    /// 是否为零。
    bool IsZero() const noexcept { return m_limbs.empty(); }
    /// 符号：0（零）或 1（正）；本类型没有负数。
    int Sign() const noexcept { return m_limbs.empty() ? 0 : 1; }
    /// 二进制位长（`0` 的位长为 0）。
    std::size_t BitLen() const noexcept;
    /// 第 `index` 位（0 = 最低位）；越界返回 false。
    bool Bit(std::size_t index) const noexcept;
    /// 低 64 位的截断值。
    std::uint64_t Uint64() const noexcept;

    /// **大端**字节序列（无前导零；零值返回空 vector），对应 `big.Int.Bytes`。
    std::vector<std::uint8_t> Bytes() const;

    /// 十进制文本，对应 `big.Int.String`。
    std::string String() const;
    /// 指定进制（2..36）的文本，对应 `big.Int.Text(base)`。
    std::string Text(int base) const;
    /// 二进制文本，对应 `big.Int.Text(2)`。
    std::string BitString() const { return Text(2); }

    /// 三路比较：-1 / 0 / 1。
    int Cmp(const BigInt& other) const noexcept;

    bool operator==(const BigInt& other) const noexcept { return Cmp(other) == 0; }
    bool operator!=(const BigInt& other) const noexcept { return Cmp(other) != 0; }
    bool operator<(const BigInt& other) const noexcept { return Cmp(other) < 0; }
    bool operator<=(const BigInt& other) const noexcept { return Cmp(other) <= 0; }
    bool operator>(const BigInt& other) const noexcept { return Cmp(other) > 0; }
    bool operator>=(const BigInt& other) const noexcept { return Cmp(other) >= 0; }

    /// 加法。
    BigInt Add(const BigInt& other) const;
    /// 减法；**要求 `*this >= other`**（无符号语义，不满足时抛 `std::invalid_argument`）。
    BigInt Sub(const BigInt& other) const;
    /// 乘法。
    BigInt Mul(const BigInt& other) const;
    /// 左移 `bits` 位。
    BigInt Shl(std::size_t bits) const;
    /// 右移 `bits` 位（向下取整）。
    BigInt Shr(std::size_t bits) const;
    /// 取模；`mod` 为零时抛 `std::invalid_argument`。
    BigInt Mod(const BigInt& mod) const;

    /**
     * @brief Miller-Rabin 素性测试，对应 `big.Int.ProbablyPrime(reps)`。
     * @param reps 轮数（≥1；实际用前 `reps` 个素数作底数）。
     * @return 大概率素数返回 true；**确定合数返回 false**。
     *
     * @note 对 `< 2^64` 的数，前 12 个素数底数已经确定性正确；更大的数
     *       每轮把误判概率上界再乘 1/4。
     */
    bool ProbablyPrime(int reps) const;

    /// 内部 limb（小端，无前导零）；给测试与调试用。
    const std::vector<std::uint32_t>& Limbs() const noexcept { return m_limbs; }

private:
    explicit BigInt(std::vector<std::uint32_t> limbs) : m_limbs(std::move(limbs)) {
        Normalize();
    }

    void Normalize() noexcept;
    void SetBitInPlace(std::size_t index);
    std::uint32_t ModSmall(std::uint32_t divisor) const noexcept;
    std::uint32_t DivSmallInPlace(std::uint32_t divisor) noexcept;

    std::vector<std::uint32_t> m_limbs;  ///< 小端 limb；空 = 0，末位非零
};

/// `Int`/`Prime` 的结果：`err == nullptr` 表示成功，对应 Go 的 `(v, nil)`。
struct BigResult {
    BigInt value;  ///< 成功时的结果；失败时为零值（Go 返回 nil）
    error err;     ///< 失败原因；成功时为空
    bool Ok() const noexcept { return !err; }
};

// ============================================================================
// 随机读取
// ============================================================================

/**
 * @brief 读满 `size` 字节，对应 Go `rand.Read`。
 * @param source 随机源。
 * @param buffer 目标缓冲。
 * @param size 请求字节数。
 * @return 实际读到字节数；正常等于 `size`，小于 `size` 表示源故障。
 */
std::size_t Read(Source& source, void* buffer, std::size_t size);

/// 用进程级 `Reader()` 读满。
std::size_t Read(void* buffer, std::size_t size);

// ============================================================================
// Int / Prime
// ============================================================================

/**
 * @brief 从 `source` 取 `[0, max)` 内的均匀随机整数，对应 `crypto/rand.Int`。
 * @param source 随机源。
 * @param max 上界（必须 > 0）。
 * @return 结果或错误（`max <= 0` 时错误文本与 Go 一致）。
 */
BigResult Int(Source& source, const BigInt& max);

/// 用进程级 `Reader()` 的 `Int`。
BigResult Int(const BigInt& max);

/**
 * @brief 生成 `bits` 位素数，对应 `crypto/rand.Prime`。
 * @param source 随机源。
 * @param bits 位长（必须 ≥ 2）。
 * @return 素数或错误。
 *
 * @details 与 Go 一致：最高**两**位被置 1，因此两个 `bits` 位素数之积
 *          恰好是 `2*bits` 位；最低位被置 1（奇数）。`bits < 2` 返回错误
 *          （文本与 Go 一致）。
 *
 * @note **与 Go 1.26+ 的行为差异**：Go 的 `crypto/rand.Prime` 现在通过
 *       `crypto/internal/rand.CustomReader` 取随机字节，**默认忽略**传入的
 *       `io.Reader` 而改用全局 DRBG（除非 `GODEBUG=cryptocustomrand=1`）。
 *       本实现始终使用传入的 `Source`——这既保留了可测试性，也与 Go 1.25
 *       及更早版本的语义一致。生产环境请直接调用无 source 的重载（它用
 *       内核 CSPRNG）。
 */
BigResult Prime(Source& source, int bits);

/// 用进程级 `Reader()` 的 `Prime`。
BigResult Prime(int bits);

// ============================================================================
// 实现（header-only；全部 inline，便于 `go2cpp/go/` 保持零链接依赖）
// ============================================================================

namespace detail {

/// 去掉高位零 limb。
inline void Trim(std::vector<std::uint32_t>& limbs) noexcept {
    while (!limbs.empty() && limbs.back() == 0) {
        limbs.pop_back();
    }
}

/// limb 序列比较：-1 / 0 / 1（双方都必须已规范化）。
inline int CompareLimbs(const std::vector<std::uint32_t>& left,
                        const std::vector<std::uint32_t>& right) noexcept {
    if (left.size() != right.size()) {
        return left.size() < right.size() ? -1 : 1;
    }
    for (std::size_t index = left.size(); index-- > 0;) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
    }
    return 0;
}

/// 无符号加法（返回未规范化的 32 位 limb 序列，调用方负责 Trim）。
inline std::vector<std::uint32_t> AddLimbs(const std::vector<std::uint32_t>& left,
                                           const std::vector<std::uint32_t>& right) {
    const std::size_t size = left.size() > right.size() ? left.size() : right.size();
    std::vector<std::uint32_t> result(size + 1, 0);
    std::uint64_t carry = 0;
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint64_t sum = static_cast<std::uint64_t>(index < left.size() ? left[index] : 0) +
                                  (index < right.size() ? right[index] : 0) + carry;
        result[index] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32;
    }
    result[size] = static_cast<std::uint32_t>(carry);
    return result;
}

/// 无符号减法；调用方保证 `left >= right`。
inline std::vector<std::uint32_t> SubLimbs(const std::vector<std::uint32_t>& left,
                                           const std::vector<std::uint32_t>& right) {
    std::vector<std::uint32_t> result(left.size(), 0);
    std::uint64_t borrow = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const std::uint64_t subtrahend =
            static_cast<std::uint64_t>(index < right.size() ? right[index] : 0) + borrow;
        const std::uint64_t minuend = left[index];
        result[index] = static_cast<std::uint32_t>(minuend - subtrahend);
        borrow = minuend < subtrahend ? 1 : 0;
    }
    return result;
}

/// 小学乘法（返回 2N limb，未规范化）。
inline std::vector<std::uint32_t> MulLimbs(const std::vector<std::uint32_t>& left,
                                           const std::vector<std::uint32_t>& right) {
    if (left.empty() || right.empty()) {
        return {};
    }
    std::vector<std::uint32_t> result(left.size() + right.size(), 0);
    for (std::size_t i = 0; i < left.size(); ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < right.size(); ++j) {
            const std::uint64_t current = static_cast<std::uint64_t>(left[i]) * right[j] +
                                          result[i + j] + carry;
            result[i + j] = static_cast<std::uint32_t>(current);
            carry = current >> 32;
        }
        std::size_t index = i + right.size();
        while (carry != 0) {
            const std::uint64_t current = static_cast<std::uint64_t>(result[index]) + carry;
            result[index] = static_cast<std::uint32_t>(current);
            carry = current >> 32;
            ++index;
        }
    }
    return result;
}

/**
 * @brief Montgomery 模乘上下文（REDC + 固定底数模幂）。
 *
 * @details 只在模数为**奇数**时有效（素数判定与 RSA 场景都满足）；偶数模
 *          会让 `m0^{-1} mod 2^32` 不存在。`BigInt::ProbablyPrime` 已经先
 *          排除了偶数，这里不再重复检查（约定由调用方保证）。
 */
class Montgomery final {
public:
    explicit Montgomery(const BigInt& modulus)
        : m_modulus(modulus), m_limbs(modulus.Limbs()) {
        m_size = m_limbs.size();
        m_inverse = static_cast<std::uint32_t>(-InverseOdd(m_limbs[0]));
        m_one = Pow2Mod(32 * m_size, modulus);   // R mod m
        m_r2 = Pow2Mod(64 * m_size, modulus);    // R^2 mod m
    }

    /// 模数。
    const BigInt& Modulus() const noexcept { return m_modulus; }

    /// 转进 Montgomery 域：`a * R mod m`。
    BigInt ToMont(const BigInt& value) const {
        return Multiply(value.Mod(m_modulus), m_r2);
    }

    /// 转出 Montgomery 域：`a * R^{-1} mod m`。
    BigInt FromMont(const BigInt& value) const {
        return Multiply(value, BigInt::FromUint64(1));
    }

    /// 普通域模乘 `a * b mod m`。
    BigInt ModMul(const BigInt& left, const BigInt& right) const {
        return FromMont(Multiply(ToMont(left), ToMont(right)));
    }

    /// 普通域模幂 `base^exponent mod m`。
    BigInt ModPow(const BigInt& base, const BigInt& exponent) const {
        BigInt accumulator = m_one;
        BigInt multiplier = ToMont(base);
        for (std::size_t bit = exponent.BitLen(); bit-- > 0;) {
            accumulator = Multiply(accumulator, accumulator);
            if (exponent.Bit(bit)) {
                accumulator = Multiply(accumulator, multiplier);
            }
        }
        return FromMont(accumulator);
    }

    /// 一次 Montgomery 乘法：`a * b * R^{-1} mod m`（REDC）。
    BigInt Multiply(const BigInt& left, const BigInt& right) const {
        std::vector<std::uint32_t> lhs = left.Limbs();
        std::vector<std::uint32_t> rhs = right.Limbs();
        lhs.resize(m_size, 0);
        rhs.resize(m_size, 0);
        std::vector<std::uint32_t> workspace = MulLimbs(lhs, rhs);
        workspace.resize(2 * m_size + 1, 0);
        for (std::size_t index = 0; index < m_size; ++index) {
            const std::uint32_t factor = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(workspace[index]) * m_inverse);
            std::uint64_t carry = 0;
            for (std::size_t j = 0; j < m_size; ++j) {
                const std::uint64_t current =
                    static_cast<std::uint64_t>(factor) * m_limbs[j] +
                    workspace[index + j] + carry;
                workspace[index + j] = static_cast<std::uint32_t>(current);
                carry = current >> 32;
            }
            std::size_t position = index + m_size;
            while (carry != 0) {
                const std::uint64_t current =
                    static_cast<std::uint64_t>(workspace[position]) + carry;
                workspace[position] = static_cast<std::uint32_t>(current);
                carry = current >> 32;
                ++position;
            }
        }
        std::vector<std::uint32_t> shifted(workspace.begin() + static_cast<std::ptrdiff_t>(m_size),
                                           workspace.end());
        Trim(shifted);
        BigInt result = BigInt::FromLimbs(std::move(shifted));
        if (result.Cmp(m_modulus) >= 0) {
            result = result.Sub(m_modulus);
        }
        return result;
    }

private:
    /// 求奇数在模 2^32 下的逆（Newton 迭代，5 轮收敛到 32 位）。
    static std::uint32_t InverseOdd(std::uint32_t value) noexcept {
        std::uint32_t inverse = 1;
        for (int round = 0; round < 5; ++round) {
            inverse *= 2u - value * inverse;
        }
        return inverse;
    }

    /// `2^bits mod modulus`（`bits` 为 32 的整数倍时最省事，但这里通用）。
    static BigInt Pow2Mod(std::size_t bits, const BigInt& modulus) {
        std::vector<std::uint32_t> value(bits / 32 + 1, 0);
        value[bits / 32] = std::uint32_t{1} << (bits % 32);
        return BigInt::FromLimbs(std::move(value)).Mod(modulus);
    }

    BigInt m_modulus;             ///< 模数（奇数）
    std::vector<std::uint32_t> m_limbs;  ///< 模数的 limb 副本（REDC 热路径避免重复取）
    std::size_t m_size{0};        ///< 模数的 limb 数
    std::uint32_t m_inverse{0};   ///< `-m^{-1} mod 2^32`
    BigInt m_one;                 ///< `R mod m`
    BigInt m_r2;                  ///< `R^2 mod m`
};

/// 前若干个素数：既做小素数试除，也做 Miller-Rabin 的固定底数。
inline const std::uint32_t* SmallPrimes(std::size_t& count) noexcept {
    static const std::uint32_t kPrimes[] = {
        2,  3,  5,  7,  11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53,
        59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131,
        137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193, 197, 199,
        211, 223, 227, 229, 233, 239, 241, 251, 257, 263, 269, 271, 277, 281};
    count = sizeof(kPrimes) / sizeof(kPrimes[0]);
    return kPrimes;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// BigInt：构造与转换
// ---------------------------------------------------------------------------

inline BigInt BigInt::FromUint64(std::uint64_t value) {
    std::vector<std::uint32_t> limbs;
    while (value != 0) {
        limbs.push_back(static_cast<std::uint32_t>(value));
        value >>= 32;
    }
    return BigInt(std::move(limbs));
}

inline BigInt BigInt::FromBytes(const std::uint8_t* bytes, std::size_t size) {
    std::vector<std::uint32_t> limbs;
    if (bytes == nullptr || size == 0) {
        return BigInt();
    }
    limbs.reserve((size + 3) / 4);
    std::size_t index = size;
    while (index > 0) {
        const std::size_t start = index >= 4 ? index - 4 : 0;
        std::uint32_t limb = 0;
        for (std::size_t position = start; position < index; ++position) {
            limb = (limb << 8) | bytes[position];
        }
        limbs.push_back(limb);
        index = start;
    }
    return BigInt(std::move(limbs));
}

inline BigInt BigInt::FromHex(std::string_view text) {
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    std::vector<std::uint32_t> limbs;
    std::uint32_t accumulator = 0;
    int shift = 0;
    for (auto iterator = text.rbegin(); iterator != text.rend(); ++iterator) {
        const char character = *iterator;
        int digit = -1;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10;
        }
        if (digit < 0) {
            throw std::invalid_argument("BigInt::FromHex: invalid hex digit");
        }
        accumulator |= static_cast<std::uint32_t>(digit) << shift;
        shift += 4;
        if (shift == 32) {
            limbs.push_back(accumulator);
            accumulator = 0;
            shift = 0;
        }
    }
    if (shift != 0) {
        limbs.push_back(accumulator);
    }
    return BigInt(std::move(limbs));
}

inline BigInt BigInt::FromLimbs(std::vector<std::uint32_t> limbs) {
    return BigInt(std::move(limbs));
}

inline void BigInt::Normalize() noexcept {
    detail::Trim(m_limbs);
}

inline std::size_t BigInt::BitLen() const noexcept {
    if (m_limbs.empty()) {
        return 0;
    }
    const std::uint32_t top = m_limbs.back();
    std::size_t bits = 0;
    for (std::uint32_t probe = top; probe != 0; probe >>= 1) {
        ++bits;
    }
    return (m_limbs.size() - 1) * 32 + bits;
}

inline bool BigInt::Bit(std::size_t index) const noexcept {
    const std::size_t limb = index / 32;
    if (limb >= m_limbs.size()) {
        return false;
    }
    return ((m_limbs[limb] >> (index % 32)) & 1u) != 0;
}

inline std::uint64_t BigInt::Uint64() const noexcept {
    std::uint64_t value = 0;
    if (!m_limbs.empty()) {
        value = m_limbs[0];
        if (m_limbs.size() > 1) {
            value |= static_cast<std::uint64_t>(m_limbs[1]) << 32;
        }
    }
    return value;
}

inline std::vector<std::uint8_t> BigInt::Bytes() const {
    std::vector<std::uint8_t> output;
    if (m_limbs.empty()) {
        return output;
    }
    const std::size_t limbs = m_limbs.size();
    output.reserve(limbs * 4);
    for (std::size_t index = limbs; index-- > 0;) {
        const std::uint32_t limb = m_limbs[index];
        const std::uint8_t bytes[4] = {
            static_cast<std::uint8_t>(limb >> 24), static_cast<std::uint8_t>(limb >> 16),
            static_cast<std::uint8_t>(limb >> 8), static_cast<std::uint8_t>(limb)};
        for (std::size_t offset = 0; offset < 4; ++offset) {
            if (output.empty() && index == limbs - 1 && bytes[offset] == 0) {
                continue;  // 去掉最高 limb 的前导零字节
            }
            output.push_back(bytes[offset]);
        }
    }
    return output;
}

inline int BigInt::Cmp(const BigInt& other) const noexcept {
    return detail::CompareLimbs(m_limbs, other.m_limbs);
}

inline BigInt BigInt::Add(const BigInt& other) const {
    return BigInt(detail::AddLimbs(m_limbs, other.m_limbs));
}

inline BigInt BigInt::Sub(const BigInt& other) const {
    if (Cmp(other) < 0) {
        throw std::invalid_argument("BigInt::Sub: negative result is not representable");
    }
    return BigInt(detail::SubLimbs(m_limbs, other.m_limbs));
}

inline BigInt BigInt::Mul(const BigInt& other) const {
    return BigInt(detail::MulLimbs(m_limbs, other.m_limbs));
}

inline BigInt BigInt::Shl(std::size_t bits) const {
    if (IsZero()) {
        return BigInt();
    }
    const std::size_t limb_shift = bits / 32;
    const std::size_t bit_shift = bits % 32;
    std::vector<std::uint32_t> result(m_limbs.size() + limb_shift + 1, 0);
    for (std::size_t index = 0; index < m_limbs.size(); ++index) {
        result[index + limb_shift] |= m_limbs[index] << bit_shift;
        if (bit_shift != 0) {
            result[index + limb_shift + 1] |= m_limbs[index] >> (32 - bit_shift);
        }
    }
    return BigInt(std::move(result));
}

inline BigInt BigInt::Shr(std::size_t bits) const {
    const std::size_t limb_shift = bits / 32;
    if (limb_shift >= m_limbs.size()) {
        return BigInt();
    }
    const std::size_t bit_shift = bits % 32;
    std::vector<std::uint32_t> result(m_limbs.size() - limb_shift, 0);
    for (std::size_t index = 0; index < result.size(); ++index) {
        std::uint32_t value = m_limbs[index + limb_shift];
        if (bit_shift != 0) {
            value >>= bit_shift;
            if (index + limb_shift + 1 < m_limbs.size()) {
                value |= m_limbs[index + limb_shift + 1] << (32 - bit_shift);
            }
        }
        result[index] = value;
    }
    return BigInt(std::move(result));
}

inline BigInt BigInt::Mod(const BigInt& modulus) const {
    if (modulus.IsZero()) {
        throw std::invalid_argument("BigInt::Mod: division by zero");
    }
    const int comparison = Cmp(modulus);
    if (comparison < 0) {
        return *this;
    }
    if (comparison == 0) {
        return BigInt();
    }
    // 二进制长除法：逐位「余数左移 + 补位 + 条件减」。位长由输入决定，
    // 最多 2*BitLen(modulus) 轮（Montgomery 的 R^2 也走这里，只调用一次）。
    std::vector<std::uint32_t> remainder;
    for (std::size_t bit = BitLen(); bit-- > 0;) {
        std::uint32_t carry = Bit(bit) ? 1u : 0u;
        for (std::uint32_t& limb : remainder) {
            const std::uint32_t next = limb >> 31;
            limb = (limb << 1) | carry;
            carry = next;
        }
        if (carry != 0) {
            remainder.push_back(carry);
        }
        detail::Trim(remainder);
        if (detail::CompareLimbs(remainder, modulus.Limbs()) >= 0) {
            remainder = detail::SubLimbs(remainder, modulus.Limbs());
        }
    }
    return BigInt(std::move(remainder));
}

inline std::uint32_t BigInt::ModSmall(std::uint32_t divisor) const noexcept {
    std::uint64_t remainder = 0;
    for (std::size_t index = m_limbs.size(); index-- > 0;) {
        remainder = ((remainder << 32) | m_limbs[index]) % divisor;
    }
    return static_cast<std::uint32_t>(remainder);
}

inline std::uint32_t BigInt::DivSmallInPlace(std::uint32_t divisor) noexcept {
    std::uint64_t remainder = 0;
    for (std::size_t index = m_limbs.size(); index-- > 0;) {
        const std::uint64_t current = (remainder << 32) | m_limbs[index];
        m_limbs[index] = static_cast<std::uint32_t>(current / divisor);
        remainder = current % divisor;
    }
    Normalize();
    return static_cast<std::uint32_t>(remainder);
}

inline std::string BigInt::Text(int base) const {
    if (base < 2 || base > 36) {
        throw std::invalid_argument("BigInt::Text: base must be in [2, 36]");
    }
    if (IsZero()) {
        return "0";
    }
    static constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    BigInt working = *this;
    std::string output;
    while (!working.IsZero()) {
        const std::uint32_t remainder = working.DivSmallInPlace(static_cast<std::uint32_t>(base));
        output.push_back(kDigits[remainder]);
    }
    std::reverse(output.begin(), output.end());
    return output;
}

inline std::string BigInt::String() const {
    return Text(10);
}

inline bool BigInt::ProbablyPrime(int reps) const {
    const int rounds = reps > 0 ? reps : 1;
    if (IsZero()) {
        return false;
    }
    const BigInt one = BigInt::FromUint64(1);
    const BigInt two = BigInt::FromUint64(2);
    if (Cmp(one) == 0) {
        return false;
    }
    if (Cmp(two) == 0) {
        return true;
    }
    if ((m_limbs[0] & 1u) == 0) {
        return false;  // 偶数
    }
    std::size_t prime_count = 0;
    const std::uint32_t* primes = detail::SmallPrimes(prime_count);
    for (std::size_t index = 0; index < prime_count; ++index) {
        const BigInt small = BigInt::FromUint64(primes[index]);
        if (Cmp(small) == 0) {
            return true;
        }
        if (ModSmall(primes[index]) == 0) {
            return false;
        }
    }
    // n - 1 = d * 2^s，d 为奇数。
    const BigInt n_minus_one = Sub(one);
    BigInt d = n_minus_one;
    int power = 0;
    while (!d.IsZero() && (d.Limbs()[0] & 1u) == 0) {
        d = d.Shr(1);
        ++power;
    }
    const detail::Montgomery context(*this);
    for (int round = 0; round < rounds; ++round) {
        const std::uint32_t base = primes[static_cast<std::size_t>(round) % prime_count];
        if (base >= 2) {
            const BigInt witness = BigInt::FromUint64(base);
            if (witness.Cmp(n_minus_one) >= 0) {
                continue;  // 底数必须 < n-1
            }
            BigInt value = context.ModPow(witness, d);
            if (value == one || value == n_minus_one) {
                continue;
            }
            bool composite = true;
            for (int step = 1; step < power; ++step) {
                value = context.ModMul(value, value);
                if (value.Cmp(n_minus_one) == 0) {
                    composite = false;
                    break;
                }
            }
            if (composite) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 随机源
// ---------------------------------------------------------------------------

#if !defined(_WIN32)

namespace detail {

/// `/dev/urandom` 兜底；fd 只打开一次（函数内静态初始化线程安全）。
inline std::size_t ReadUrandom(std::uint8_t* out, std::size_t size) {
    static int descriptor = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return 0;
    }
    std::size_t total = 0;
    while (total < size) {
        const ssize_t got = ::read(descriptor, out + total, size - total);
        if (got > 0) {
            total += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    return total;
}

}  // namespace detail

inline std::size_t OsSource::Read(std::uint8_t* out, std::size_t size) {
    if (out == nullptr || size == 0) {
        return 0;
    }
#if defined(__linux__)
    std::size_t total = 0;
    while (total < size) {
        const ssize_t got = ::getrandom(out + total, size - total, 0);
        if (got > 0) {
            total += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;  // ENOSYS / EAGAIN 等：交给 /dev/urandom
    }
    if (total == size) {
        return total;
    }
    return total + detail::ReadUrandom(out + total, size - total);
#else
    return detail::ReadUrandom(out, size);
#endif
}

#else  // _WIN32：用 std::random_device（MSVC 实现基于 BCryptGenRandom）

inline std::size_t OsSource::Read(std::uint8_t* out, std::size_t size) {
    if (out == nullptr || size == 0) {
        return 0;
    }
    static std::mutex mutex;
    static std::random_device device;
    std::lock_guard<std::mutex> lock(mutex);
    std::size_t total = 0;
    while (total < size) {
        const std::uint32_t word = static_cast<std::uint32_t>(device());
        const std::size_t chunk = size - total < 4 ? size - total : 4;
        for (std::size_t index = 0; index < chunk; ++index) {
            out[total + index] = static_cast<std::uint8_t>(word >> (8 * index));
        }
        total += chunk;
    }
    return total;
}

#endif  // _WIN32

/// 进程级 CSPRNG 实例（对应 Go 的 `crypto/rand.Reader`）。
inline Source& Reader() {
    static OsSource source;
    return source;
}

inline std::size_t Read(Source& source, void* buffer, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    std::size_t total = 0;
    while (total < size) {
        const std::size_t got = source.Read(bytes + total, size - total);
        if (got == 0) {
            break;
        }
        total += got;
    }
    return total;
}

inline std::size_t Read(void* buffer, std::size_t size) {
    return Read(Reader(), buffer, size);
}

// ---------------------------------------------------------------------------
// Int / Prime
// ---------------------------------------------------------------------------

inline BigResult Int(Source& source, const BigInt& max) {
    if (max.Sign() <= 0) {
        return {BigInt(), New("crypto/rand: argument to Int is <= 0")};
    }
    // 与 Go 1.27 一致：取 `max-1` 的位长（`max` 是 2 的幂时正好少读一个
    // 字节，且 `max == 1` 只有唯一结果 0，不消耗随机源）。
    const BigInt upper = max.Sub(BigInt::FromUint64(1));
    const std::size_t bits = upper.BitLen();
    if (bits == 0) {
        return {BigInt::FromUint64(0), error()};
    }
    const std::size_t bytes = (bits + 7) / 8;
    const std::size_t top_bits = bits % 8 == 0 ? 8 : bits % 8;
    const std::uint8_t top_mask =
        static_cast<std::uint8_t>((1u << top_bits) - 1u);  // 丢掉最高字节的多余位
    std::vector<std::uint8_t> buffer(bytes, 0);
    for (;;) {
        if (Read(source, buffer.data(), bytes) != bytes) {
            return {BigInt(), New("crypto/rand: failed to read random bytes")};
        }
        buffer[0] &= top_mask;
        BigInt candidate = BigInt::FromBytes(buffer.data(), bytes);
        if (candidate.Cmp(max) < 0) {
            return {std::move(candidate), error()};
        }
    }
}

inline BigResult Int(const BigInt& max) {
    return Int(Reader(), max);
}

inline BigResult Prime(Source& source, int bits) {
    if (bits < 2) {
        return {BigInt(), New("crypto/rand: prime size must be at least 2-bit")};
    }
    const std::size_t total_bits = static_cast<std::size_t>(bits);
    const std::size_t top_bits = total_bits % 8 == 0 ? 8 : total_bits % 8;
    const std::size_t bytes = (total_bits + 7) / 8;
    std::vector<std::uint8_t> buffer(bytes, 0);
    for (;;) {
        if (Read(source, buffer.data(), bytes) != bytes) {
            return {BigInt(), New("crypto/rand: failed to read random bytes")};
        }
        buffer[0] &= static_cast<std::uint8_t>((1u << top_bits) - 1u);
        if (top_bits >= 2) {
            buffer[0] |= static_cast<std::uint8_t>(3u << (top_bits - 2));  // 最高两位置 1
        } else {
            buffer[0] |= 1u;
            if (bytes > 1) {
                buffer[1] |= 0x80u;
            }
        }
        buffer[bytes - 1] |= 1u;  // 奇数
        BigInt candidate = BigInt::FromBytes(buffer.data(), bytes);
        if (candidate.ProbablyPrime(20)) {
            return {std::move(candidate), error()};
        }
    }
}

inline BigResult Prime(int bits) {
    return Prime(Reader(), bits);
}

}  // namespace rand
}  // namespace crypto
}  // namespace go2cpp::go
