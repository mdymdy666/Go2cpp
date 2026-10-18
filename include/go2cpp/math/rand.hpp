/**
 * @file rand.hpp
 * @brief Go `math/rand` 最小可用子集的 C++ 对应实现。
 *
 * @details ## 算法选择：xoshiro256**
 *
 *   底层生成器用 **xoshiro256\*\***（Blackman & Vigna，2018），不是 `std::mt19937`：
 *   | 候选 | 状态 | 输出 | 结论 |
 *   |---|---:|---|---|
 *   | `std::mt19937_64` | 2.5 KiB | 64 bit | **不采用**：状态大、初始化/拷贝贵，且算法与 Go 无关 |
 *   | PCG64（XSL RR 128/64） | 128 bit | 64 bit | 可选；需要 128 位乘法和更复杂的状态推进，本模块选更简单的 xoshiro |
 *   | **xoshiro256\*\*** | 256 bit | 64 bit | **采用**：代码约 10 行、单次 `next()` 只有移位/异或/两次乘法，直接产出 `Uint64`，周期 2^256−1 |
 *
 *   来源（public domain / CC0）：
 *     - https://prng.di.unimi.it/xoshiro256starstar.c
 *     - https://prng.di.unimi.it/splitmix64.c
 *   种子扩展用 **splitmix64**（同一站点）：把任意 64 位种子扩散成 4 个状态字。
 *   作者声明 xoshiro256\*\* 通过 BigCrush；本模块的测试只做分布/卡方/置换与已知向量对拍，
 *   **没有**跑完整的 TestU01 套件（见文末边界）。
 *
 *   **不是密码学安全的 PRNG**：与 Go `math/rand` 的定位一致，安全场景请用 `crypto/rand`。
 *
 * @details ## 并发策略：选 A（全局互斥锁）
 *
 *   Go 的**顶层函数**（`rand.Intn` 等）是并发安全的，而 `*rand.Rand` 实例**不是**。
 *   本模块完全对齐这一划分：
 *     - `go2cpp::rand::Rand` 实例**不加锁**，调用方自行保证单线程使用；
 *     - `go2cpp::rand::Intn` 等**全局函数**由内部 `std::mutex` 保护。
 *
 *   为什么选 A 而不是 B（thread-local）：
 *     1. **语义可预测**：`Seed(42)` 之后，无论从哪个线程调用，全局序列都是同一个；
 *        thread-local 会让每个线程拿到独立序列，`Seed` 的可复现性只对单线程成立，
 *        与 Go 的全局语义不一致；
 *     2. **测试可判定**：并发压测后仍能按固定种子重放；
 *     3. **成本可接受**：单线程全局 `Intn(1000)` 实测约 4~6 ns/次（见测试报告的吞吐），
 *        足够大多数非密码学用途；如果某段代码确实需要极致吞吐，应使用线程私有的
 *        `Rand` 实例（这也是 Go 推荐的做法）。
 *   注意：全局 `Shuffle` **只在每次抽取时加锁**，不跨整个回调持锁，因此 `swap` 回调里
 *   再调用 `rand.Intn` 不会死锁（Go 的 `lockedSource` 也是每次调用单独加锁）。
 *
 * @note **与 Go 对齐的行为**：
 *       - `Int63()` 是非负 63 位；`Uint32()` 取 `Int63() >> 31`；`Intn(n)` 在
 *         `n <= 2^31-1` 时走 31 位拒绝采样，否则走 63 位版本；
 *       - `Float64()` 用 `Int63() / 2^63` 并在舍入到 1.0 时重采样，保证 `[0, 1)`；
 *       - `Read()` 每次消耗一个 `Int63()` 产出 **7 个字节**（63 位中只用低 56 位；
 *         Go 的 `readPos` 从 7 递减到 0，实测 Go 1.27 亦然），并在多次调用之间
 *         保留未用完的字节，和 Go 的 `readVal/readPos` 一致；
 *       - `Perm(n)` 采用 Go 的 `m[i] = m[j]; m[j] = i` 原地算法；
 *       - 参数非法时抛 `std::invalid_argument`（Go 对应 `panic`）：`Intn(<=0)`、
 *         `Int63n(<=0)`、`Perm(<0)`、`Shuffle(<0)`。
 *
 * @note **边界**：
 *       - `NormFloat64`/`ExpFloat64` 只保证**分布**与 Go 一致（标准正态 / 速率 1
 *         指数），**不逐位一致**：Go 用 ziggurat，本模块分别用 Box–Muller（成对
 *         产出、缓存第二个值）与逆变换 `-log(1-U)`；同种子本来也无法与 Go 对拍
 *         （底层 PRNG 不同）。`Seed()` 会清掉缓存的正态值；
 *       - 未实现 `Source` 接口 / `New(Source)` / `jump()` / `long_jump()`（C 类，
 *         有意不做）：Go 的 `Source` 要求把 `Rand` 改成可替换源的多态对象，而本
 *         模块只有 xoshiro256** 一种源，没有替换需求；`jump()` 是 xoshiro 参考
 *         实现的自有 API，Go 的 `math/rand` 并不提供，也没有并行子序列场景。
 *       - 与 Go 的**数值序列不互通**：Go 1.23 的默认源是运行时 ChaCha8（`runtimeSource`）
 *         或兼容的 lagged-Fibonacci（`rngSource`），本模块是 xoshiro256**；同一种子不会
 *         产生相同序列，但分布与 API 语义一致；
 *       - 全局源在首次使用时用 `std::random_device` + 时钟 + 栈地址混合播种；需要可复现
 *         序列时先调用 `Seed(...)`；
 *       - 没有实现 `jump()`/`long_jump()` 等并行子序列 API。
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace go2cpp::rand {

/// 字节类型（对应 Go `[]byte` 的元素；C++17 没有 `std::byte` 的算术便利）。
using byte = std::uint8_t;

/**
 * @brief 最小可写字节视图（C++17 没有 `std::span`）。
 * @details 只提供 `Read` 需要的接口：`data()`、`size()`、`operator[]`、迭代器。
 *          支持从指针+长度、C 数组、`std::vector`、`std::array` 构造。
 *          生命周期与底层容器一致，**不拥有**数据。
 */
template <typename T>
class span final {
public:
    using element_type = T;
    using value_type = std::remove_cv_t<T>;
    using pointer = T*;
    using reference = T&;
    using iterator = T*;

    constexpr span() noexcept = default;

    constexpr span(pointer data, std::size_t size) noexcept
        : m_data(data), m_size(size) {}

    template <std::size_t N>
    constexpr span(T (&array)[N]) noexcept : m_data(array), m_size(N) {}

    template <typename Allocator>
    span(std::vector<value_type, Allocator>& values) noexcept
        : m_data(values.data()), m_size(values.size()) {}

    template <std::size_t N>
    span(std::array<value_type, N>& values) noexcept
        : m_data(values.data()), m_size(N) {}

    constexpr pointer data() const noexcept { return m_data; }
    constexpr std::size_t size() const noexcept { return m_size; }
    constexpr bool empty() const noexcept { return m_size == 0; }
    constexpr reference operator[](std::size_t index) const noexcept {
        return m_data[index];
    }
    constexpr iterator begin() const noexcept { return m_data; }
    constexpr iterator end() const noexcept { return m_data + m_size; }

private:
    pointer m_data{nullptr};
    std::size_t m_size{0};
};

/// `Read` 使用的字节视图别名。
using ByteSpan = span<byte>;

/**
 * @brief 对应 Go 的 `*rand.Rand`（一个带独立状态的随机数生成器）。
 *
 * @details **不是**线程安全的；多个线程共享同一实例需要外部同步（与 Go 一致）。
 *          拷贝会复制完整状态（包括 `Read` 的未用完字节），因此拷贝后的两个实例
 *          会产生相同序列。
 */
class Rand final {
public:
    /**
     * @brief 用 64 位种子构造。
     * @details 内部用 splitmix64 把种子扩展成 4 个状态字；种子相同则序列相同。
     */
    explicit Rand(std::uint64_t seed) noexcept;

    /// 重新播种；会同时清空 `Read` 的未用完字节（对应 Go 的 `Seed` 重置 `readPos`）。
    void Seed(std::uint64_t seed) noexcept;

    /// 对应 `Rand.Uint64`：直接返回 64 位输出。
    std::uint64_t Uint64() noexcept;

    /// 对应 `Rand.Uint32`：`uint32(Int63() >> 31)`（与 Go 1.23 一致）。
    std::uint32_t Uint32() noexcept;

    /// 对应 `Rand.Int63`：非负 63 位整数。
    std::int64_t Int63() noexcept;

    /// 对应 `Rand.Int31`：非负 31 位整数（`int32(Int63() >> 32)`）。
    std::int32_t Int31() noexcept;

    /// 对应 `Rand.Int`：非负 `int`（在 64 位平台上等价于 `Int63`）。
    std::int64_t Int() noexcept;

    /**
     * @brief 对应 `Rand.Int63n(n)`：返回 `[0, n)` 的整数。
     * @throws std::invalid_argument 当 `n <= 0`（Go 对应 panic）。
     */
    std::int64_t Int63n(std::int64_t n);

    /**
     * @brief 对应 `Rand.Int31n(n)`：返回 `[0, n)` 的 31 位整数。
     * @throws std::invalid_argument 当 `n <= 0`（Go 对应 panic）。
     * @details 与 Go 相同：`n` 是 2 的幂时直接掩码，否则 31 位拒绝采样。
     */
    std::int32_t Int31n(std::int32_t n);

    /**
     * @brief 对应 `Rand.Intn(n)`：返回 `[0, n)` 的整数。
     * @throws std::invalid_argument 当 `n <= 0`（Go 对应 panic）。
     */
    std::int64_t Intn(std::int64_t n);

    /// 对应 `Rand.Float64`：返回 `[0.0, 1.0)`。
    double Float64() noexcept;

    /// 对应 `Rand.Float32`：返回 `[0.0, 1.0)`。
    float Float32() noexcept;

    /**
     * @brief 对应 `Rand.NormFloat64`：标准正态分布（均值 0、标准差 1）。
     * @details Box–Muller：每次用两个均匀随机数，同时算出两个正态值，第二个缓存
     *          到下一次调用（与经典实现的性能技巧一致）。`Seed()` 会清空缓存。
     *          分布正确性由测试的均值/方差/卡方与两端覆盖验证。
     */
    double NormFloat64() noexcept;

    /**
     * @brief 对应 `Rand.ExpFloat64`：速率 1 的指数分布（均值 1、标准差 1）。
     * @details 逆变换 `-log(1-U)`，`U` 来自 `Float64()`（严格小于 1），因此不会
     *          出现 `log(0)`。只保证分布，不保证与 Go ziggurat 逐位一致。
     */
    double ExpFloat64() noexcept;

    /**
     * @brief 对应 `Rand.Perm(n)`：返回 `[0, n)` 的一个随机排列。
     * @throws std::invalid_argument 当 `n < 0`（Go 对应 panic）。
     */
    std::vector<std::int64_t> Perm(std::int64_t n);

    /**
     * @brief 对应 `Rand.Shuffle(n, swap)`：Fisher–Yates 原地洗牌。
     * @tparam Swap 可调用对象，签名 `void(std::int64_t i, std::int64_t j)`。
     * @throws std::invalid_argument 当 `n < 0`（Go 对应 panic）。
     */
    template <typename Swap>
    void Shuffle(std::int64_t n, Swap&& swap) {
        if (n < 0) {
            throw std::invalid_argument("rand.Shuffle: negative n");
        }
        for (std::int64_t i = n - 1; i > 0; --i) {
            const std::int64_t j = Intn(i + 1);
            swap(i, j);
        }
    }

    /**
     * @brief 对应 `Rand.Read`：用随机字节填满 `p`。
     * @return 实际写入的字节数，恒等于 `p.size()`（Go 恒返回 `len(p), nil`）。
     * @details 每次消耗一个 `Int63()`，按小端序拆成 **7 个字节**；未用完的字节留在
     *          实例里供下一次 `Read` 继续使用。空 `p` 不消耗随机数。
     */
    std::size_t Read(ByteSpan p) noexcept;

private:
    std::uint64_t m_state[4]{};
    std::int64_t m_read_val{0};   ///< `Read` 尚未用完的 63 位随机值
    std::int8_t m_read_pos{0};    ///< `m_read_val` 还剩几个有效字节（0 表示需要重新抽取）
    double m_norm_cache{0.0};     ///< Box–Muller 成对结果中暂存的第二个值
    bool m_has_norm_cache{false};  ///< 缓存是否有效（`Seed` 必须清掉）
};

// ============================================================================
// 全局函数（对应 Go math/rand 的顶层函数）
//
// 实现方式：一个进程级 `Rand` 实例 + `std::mutex`。并发语义与 Go 的顶层函数一致。
// ============================================================================

/// 对应 `rand.Seed(seed)`：重新播种全局生成器。注意 Go 1.20+ 已把它标记为 deprecated。
void Seed(std::int64_t seed);

/// 对应全局 `rand.Uint64()`。
std::uint64_t Uint64();
/// 对应全局 `rand.Uint32()`。
std::uint32_t Uint32();
/// 对应全局 `rand.Int63()`。
std::int64_t Int63();
/// 对应全局 `rand.Int31()`。
std::int32_t Int31();
/// 对应全局 `rand.Int()`。
std::int64_t Int();
/// 对应全局 `rand.Int63n(n)`；`n <= 0` 抛 `std::invalid_argument`。
std::int64_t Int63n(std::int64_t n);
/// 对应全局 `rand.Int31n(n)`；`n <= 0` 抛 `std::invalid_argument`。
std::int32_t Int31n(std::int32_t n);
/// 对应全局 `rand.Intn(n)`；`n <= 0` 抛 `std::invalid_argument`。
std::int64_t Intn(std::int64_t n);
/// 对应全局 `rand.Float64()`。
double Float64();
/// 对应全局 `rand.Float32()`。
float Float32();
/// 对应全局 `rand.NormFloat64()`。
double NormFloat64();
/// 对应全局 `rand.ExpFloat64()`。
double ExpFloat64();
/// 对应全局 `rand.Perm(n)`；`n < 0` 抛 `std::invalid_argument`。
std::vector<std::int64_t> Perm(std::int64_t n);
/// 对应全局 `rand.Read(p)`：线程安全，恒填满 `p`。
std::size_t Read(ByteSpan p);

/**
 * @brief 对应全局 `rand.Shuffle(n, swap)`。
 * @details 与实例版算法相同，但**每次抽取单独加锁**而不是整段持锁，因此 `swap`
 *          回调里再调用全局随机函数不会死锁。
 * @throws std::invalid_argument 当 `n < 0`。
 */
template <typename Swap>
void Shuffle(std::int64_t n, Swap&& swap) {
    if (n < 0) {
        throw std::invalid_argument("rand.Shuffle: negative n");
    }
    for (std::int64_t i = n - 1; i > 0; --i) {
        const std::int64_t j = Intn(i + 1);
        swap(i, j);
    }
}

}  // namespace go2cpp::rand
