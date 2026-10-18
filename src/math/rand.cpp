/**
 * @file rand.cpp
 * @brief `go2cpp/math/rand.hpp` 的实现：xoshiro256** + splitmix64 播种。
 *
 * @details 算法来源（public domain / CC0）：
 *   - xoshiro256** 1.0：David Blackman & Sebastiano Vigna (2018)
 *     https://prng.di.unimi.it/xoshiro256starstar.c
 *   - splitmix64 播种：Sebastiano Vigna (2015)
 *     https://prng.di.unimi.it/splitmix64.c
 *   本文件按上述公开算法独立实现；`next()` 的状态推进顺序与参考实现逐行一致。
 *
 *   并发：实例不加锁；全局函数用进程级 `std::mutex` 保护（策略 A，详见头文件）。
 *
 * @author Go2Cpp project
 * @date 2026-10-18
 */

#include "go2cpp/math/rand.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <random>

namespace go2cpp::rand {

namespace {

/// xoshiro256** 的 64 位循环左移。
constexpr std::uint64_t Rotl(std::uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
}

/// splitmix64：把 64 位种子扩散成高质量状态字（官方参考实现）。
std::uint64_t SplitMix64(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/**
 * @brief 为全局生成器取一个不可预测的初始种子。
 * @details 优先用 `std::random_device`；某些平台（旧版 MinGW、部分容器）的
 *          `random_device` 可能抛异常或退化为确定性序列，因此再混入高精度时钟与
 *          栈地址，至少保证同一进程的多次运行不会从同一状态开始。
 */
std::uint64_t EntropySeed() noexcept {
    std::uint64_t seed = 0;
    try {
        std::random_device device;
        seed = (static_cast<std::uint64_t>(device()) << 32) ^
               static_cast<std::uint64_t>(device());
    } catch (...) {
        seed = 0;
    }
    seed ^= static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    seed ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&seed));
    return seed;
}

std::mutex& GlobalMutex() {
    static std::mutex mutex;
    return mutex;
}

Rand& GlobalRand() {
    static Rand generator(EntropySeed());
    return generator;
}

}  // namespace

Rand::Rand(std::uint64_t seed) noexcept { Seed(seed); }

void Rand::Seed(std::uint64_t seed) noexcept {
    std::uint64_t x = seed;
    for (auto& word : m_state) {
        word = SplitMix64(x);
    }
    // xoshiro 要求状态不能全零（概率极低，但 splitmix64 的输出序列在理论上可能
    // 连续四个 0；显式兜底，避免生成器退化为恒零）。
    if ((m_state[0] | m_state[1] | m_state[2] | m_state[3]) == 0) {
        m_state[0] = 0x9e3779b97f4a7c15ULL;
    }
    m_read_val = 0;
    m_read_pos = 0;
    m_norm_cache = 0.0;
    m_has_norm_cache = false;
}

std::uint64_t Rand::Uint64() noexcept {
    // xoshiro256** 1.0 的 next()（逐行对照官方参考实现）。
    const std::uint64_t result = Rotl(m_state[1] * 5, 7) * 9;
    const std::uint64_t t = m_state[1] << 17;
    m_state[2] ^= m_state[0];
    m_state[3] ^= m_state[1];
    m_state[1] ^= m_state[2];
    m_state[0] ^= m_state[3];
    m_state[2] ^= t;
    m_state[3] = Rotl(m_state[3], 45);
    return result;
}

std::uint32_t Rand::Uint32() noexcept {
    // Go 1.23: func (r *Rand) Uint32() uint32 { return uint32(r.Int63() >> 31) }
    return static_cast<std::uint32_t>(Int63() >> 31);
}

std::int64_t Rand::Int63() noexcept {
    return static_cast<std::int64_t>(Uint64() >> 1);
}

std::int32_t Rand::Int31() noexcept {
    // Go 1.23: func (r *Rand) Int31() int32 { return int32(r.Int63() >> 32) }
    return static_cast<std::int32_t>(Int63() >> 32);
}

std::int64_t Rand::Int() noexcept {
    // Go 的 Int() 在 64 位平台上就是 Int63()（int 宽度 == 64）。
    return Int63();
}

std::int64_t Rand::Int63n(std::int64_t n) {
    if (n <= 0) {
        throw std::invalid_argument("rand.Int63n: n must be positive");
    }
    // 2 的幂直接掩码；Go 的实现也是这条快速路径。
    if ((n & (n - 1)) == 0) {
        return Int63() & (n - 1);
    }
    const std::uint64_t un = static_cast<std::uint64_t>(n);
    // 拒绝采样：只接受 <= (2^63-1) - (2^63 mod n) 的值，保证取模后均匀。
    const std::int64_t max_value = static_cast<std::int64_t>(
        (std::uint64_t{1} << 63) - 1ULL - ((std::uint64_t{1} << 63) % un));
    std::int64_t value = Int63();
    while (value > max_value) {
        value = Int63();
    }
    return value % n;
}

std::int32_t Rand::Int31n(std::int32_t n) {
    if (n <= 0) {
        throw std::invalid_argument("rand.Int31n: n must be positive");
    }
    // 2 的幂直接掩码（Go 的快速路径；n 是正 int32，不存在符号问题）。
    if ((n & (n - 1)) == 0) {
        return static_cast<std::int32_t>(Int31() & (n - 1));
    }
    // 拒绝采样：只接受 <= (2^31-1) - (2^31 mod n) 的值。
    const std::uint32_t un = static_cast<std::uint32_t>(n);
    const std::int32_t max_value = static_cast<std::int32_t>(
        (std::uint32_t{1} << 31) - 1ULL - ((std::uint32_t{1} << 31) % un));
    std::int32_t value = Int31();
    while (value > max_value) {
        value = Int31();
    }
    return static_cast<std::int32_t>(value % n);
}

std::int64_t Rand::Intn(std::int64_t n) {
    if (n <= 0) {
        throw std::invalid_argument("rand.Intn: n must be positive");
    }
    // Go 1.23: n <= 2^31-1 时走 Int31n（31 位拒绝采样），否则走 Int63n。
    if (n <= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
        return static_cast<std::int64_t>(Int31n(static_cast<std::int32_t>(n)));
    }
    return Int63n(n);
}

double Rand::Float64() noexcept {
    // Go 的实现：f = Int63() / 2^63；理论上可能四舍五入到 1.0（1/2^53 概率），
    // 此时重采样，保证严格小于 1.0。
    for (;;) {
        const double value = static_cast<double>(Int63()) / 9223372036854775808.0;
        if (value != 1.0) {
            return value;
        }
    }
}

float Rand::Float32() noexcept {
    for (;;) {
        const float value = static_cast<float>(Float64());
        if (value != 1.0f) {
            return value;
        }
    }
}

double Rand::NormFloat64() noexcept {
    // Box–Muller：两个独立的 U(0,1) 生成一对标准正态值，缓存第二个。
    // 拒绝 u1 == 0（Float64 可以返回 0，而 log(0) = -Inf）。
    if (m_has_norm_cache) {
        m_has_norm_cache = false;
        return m_norm_cache;
    }
    double u1 = 0.0;
    do {
        u1 = Float64();
    } while (u1 <= 0.0);
    const double u2 = Float64();
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    const double radius = std::sqrt(-2.0 * std::log(u1));
    const double angle = kTwoPi * u2;
    m_norm_cache = radius * std::sin(angle);
    m_has_norm_cache = true;
    return radius * std::cos(angle);
}

double Rand::ExpFloat64() noexcept {
    // 逆变换：U ~ U[0,1) => -log(1-U) ~ Exp(1)。Float64() 严格小于 1，
    // 因此 1-U ∈ (0,1]，不会出现 log(0)。
    return -std::log(1.0 - Float64());
}

std::vector<std::int64_t> Rand::Perm(std::int64_t n) {
    if (n < 0) {
        throw std::invalid_argument("rand.Perm: negative n");
    }
    std::vector<std::int64_t> result(static_cast<std::size_t>(n));
    // Go 的原地算法：i=0 时也执行一次 m[0] = m[0]，这会消耗一次随机数；
    // 为了保持与 Go 相同的随机流消耗，不跳过 i=0。
    for (std::int64_t i = 0; i < n; ++i) {
        const std::int64_t j = Intn(i + 1);
        result[static_cast<std::size_t>(i)] = result[static_cast<std::size_t>(j)];
        result[static_cast<std::size_t>(j)] = i;
    }
    return result;
}

/**
 * @brief 用随机字节填满 `p`，保留未用完的字节供下次调用。
 *
 * ## 步骤
 * step1. 逐字节循环；仅当 `m_read_pos == 0` 时才消耗一个新的 `Int63()` 并把
 *        `m_read_pos` 置为 7；
 * step2. 取 `m_read_val` 的最低字节写入 `p[i]`，右移 8 位、计数减一。
 *
 * ## 为什么是 7 字节而不是 8
 * `Int63()` 只有 63 位，Go 的 `readPos` 从 7 递减到 0、每次只产出 7 字节（只用低 56 位）。
 * 改成 8 会改变随机流消耗，与 Go 的 `readVal/readPos` 语义脱节；空 `p` 不消耗随机数。
 */
std::size_t Rand::Read(ByteSpan p) noexcept {
    for (std::size_t i = 0; i < p.size(); ++i) {
        // step1: 缓冲用尽才重新抽取 7 个字节。
        if (m_read_pos == 0) {
            m_read_val = Int63();
            m_read_pos = 7;
        }
        // step2: 取最低字节，右移后计数减一。
        p[i] = static_cast<byte>(m_read_val);
        m_read_val >>= 8;
        --m_read_pos;
    }
    return p.size();
}

// ============================================================================
// 全局函数：一个进程级 Rand + mutex（策略 A）
// ============================================================================

void Seed(std::int64_t seed) {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    GlobalRand().Seed(static_cast<std::uint64_t>(seed));
}

std::uint64_t Uint64() {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Uint64();
}

std::uint32_t Uint32() {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Uint32();
}

std::int64_t Int63() {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Int63();
}

std::int32_t Int31() {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Int31();
}

std::int64_t Int() {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Int();
}

std::int64_t Int63n(std::int64_t n) {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Int63n(n);
}

std::int32_t Int31n(std::int32_t n) {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Int31n(n);
}

std::int64_t Intn(std::int64_t n) {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Intn(n);
}

double Float64() {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Float64();
}

float Float32() {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Float32();
}

double NormFloat64() {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().NormFloat64();
}

double ExpFloat64() {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().ExpFloat64();
}

std::vector<std::int64_t> Perm(std::int64_t n) {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Perm(n);
}

std::size_t Read(ByteSpan p) {
    std::lock_guard<std::mutex> lock(GlobalMutex());
    return GlobalRand().Read(p);
}

}  // namespace go2cpp::rand
