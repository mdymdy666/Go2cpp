/**
 * @file test_math.cpp
 * @brief `go2cpp::math` 的正式测试：语义边界 + 随机 libm 对拍 + 真实场景 + 压测。
 *
 * 测试设计（按 PROJECT_RULES.md 第 3 节）：
 *   - 随机对拍：每个一元函数 10 万个随机输入（含特殊值），二元函数 10 万对输入，
 *     与系统 libm 对比 ULP 差；容差 1 ULP（实测 0）；
 *   - 真实场景：8 个场景覆盖全部 API（几何、统计、物理/图形、金融、浮点分解、
 *     信号/激活、特殊值校验、整数边界）；
 *   - 压测：sqrt/exp/log 各 1000 万次，报告吞吐；硬件环境在 `main` 开头打印。
 *
 * 说明：本模块的实现大量委托 libm，所以「与 libm 对拍」验证的是**参数转发、
 * 特殊值编写与 Go 语义修正**没有改变结果；对 Go 特有的 `Max`/`Min` 与 `Modf(Inf)`，
 * 测试使用独立的 Go 语义参考实现。
 *
 * 注意：注释中不得出现连续的斜杠加星号（GCC -Wcomment）。
 */
#include "go2cpp/math/math.hpp"
#include "test_math_env.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace m = go2cpp::math;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[math] FAILED: %s\n", what);
        ++g_failures;
    }
}

void CheckNear(const char* what, double actual, double expected, double tolerance) {
    const bool both_nan = std::isnan(actual) && std::isnan(expected);
    const bool equal_inf = std::isinf(actual) && std::isinf(expected) && actual == expected;
    const bool close = std::isfinite(actual) && std::isfinite(expected) &&
                       std::fabs(actual - expected) <= tolerance;
    if (!(both_nan || equal_inf || close)) {
        std::fprintf(stderr,
                     "[math] FAILED: %s (actual=%.17g expected=%.17g tolerance=%g)\n",
                     what, actual, expected, tolerance);
        ++g_failures;
    }
    std::printf("    %-46s got=%.15g expected=%.15g\n", what, actual, expected);
}

std::uint64_t Bits(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double FromBits(std::uint64_t bits) {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool SameBits(double left, double right) {
    if (std::isnan(left) || std::isnan(right)) {
        return std::isnan(left) && std::isnan(right);
    }
    return Bits(left) == Bits(right);
}

void CheckBits(const char* what, double actual, std::uint64_t expected) {
    const std::uint64_t got = Bits(actual);
    if (got != expected) {
        std::fprintf(stderr, "[math] FAILED: %s bits=0x%016llx expected=0x%016llx\n",
                     what, static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(expected));
        ++g_failures;
    }
    std::printf("    %-46s bits=0x%016llx\n", what,
                static_cast<unsigned long long>(got));
}

/// 把 double 映射成单调递增的整数，用于计算 ULP 距离。
std::uint64_t OrderedBits(double value) {
    const std::uint64_t bits = Bits(value);
    if ((bits & 0x8000000000000000ULL) != 0) {
        return ~bits;
    }
    return bits | 0x8000000000000000ULL;
}

/**
 * @brief 两个 double 的 ULP 距离。
 * @details 任一为 NaN 时：两个都是 NaN 记 0，否则记 UINT64_MAX；两个零符号不同
 *          也记 UINT64_MAX（否则 +0/-0 只差 1 ULP，会漏掉符号错误）。
 */
std::uint64_t UlpDiff(double left, double right) {
    if (std::isnan(left) || std::isnan(right)) {
        return (std::isnan(left) && std::isnan(right)) ? 0 : UINT64_MAX;
    }
    if (left == 0.0 && right == 0.0 && std::signbit(left) != std::signbit(right)) {
        return UINT64_MAX;
    }
    const std::uint64_t a = OrderedBits(left);
    const std::uint64_t b = OrderedBits(right);
    return a > b ? a - b : b - a;
}

const std::vector<double>& Specials() {
    static const std::vector<double> values = {
        0.0,
        -0.0,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
        -std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::denorm_min(),
        -std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::max(),
        -std::numeric_limits<double>::max(),
        std::numeric_limits<double>::min(),
        -std::numeric_limits<double>::min(),
        1.0,
        -1.0,
        0.5,
        -0.5,
        2.0,
        -2.0,
        10.0,
        -10.0,
        m::Pi,
        -m::Pi,
        m::E,
        -m::E,
        1e-300,
        -1e-300,
        1e300,
        -1e300,
        0x1p63,
        -0x1p63,
        0x1p64,
        -0x1p64,
    };
    return values;
}

/// 测试输入生成器：1/4 随机位模式、1/4 [-100,100]、1/4 [-2,2]、1/4 特殊值。
class InputGenerator final {
public:
    explicit InputGenerator(std::uint64_t seed) : m_rng(seed) {}

    double Uniform(double low, double high) {
        const double unit = static_cast<double>(m_rng() >> 11) / 9007199254740992.0;
        return low + (high - low) * unit;
    }

    double Next() {
        switch (m_rng() % 4U) {
            case 0:
                return FromBits(m_rng());
            case 1:
                return Uniform(-100.0, 100.0);
            case 2:
                return Uniform(-2.0, 2.0);
            default: {
                const auto& values = Specials();
                return values[static_cast<std::size_t>(m_rng() % values.size())];
            }
        }
    }

    std::uint64_t Raw() { return m_rng(); }

private:
    std::mt19937_64 m_rng;
};

using UnaryFn = double (*)(double);
using BinaryFn = double (*)(double, double);

struct UnaryCase final {
    const char* name;
    UnaryFn ours;
    UnaryFn reference;
};

struct BinaryCase final {
    const char* name;
    BinaryFn ours;
    BinaryFn reference;
};

/// Go 的 Max 参考实现（测试侧独立写一遍，避免直接调用被测代码）。
double ReferenceMax(double x, double y) {
    if ((std::isinf(x) && x > 0.0) || (std::isinf(y) && y > 0.0)) {
        return std::numeric_limits<double>::infinity();
    }
    if (std::isnan(x) || std::isnan(y)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (x == 0.0 && x == y) {
        return std::signbit(x) ? y : x;
    }
    return x > y ? x : y;
}

/// Go 的 Min 参考实现。
double ReferenceMin(double x, double y) {
    if ((std::isinf(x) && x < 0.0) || (std::isinf(y) && y < 0.0)) {
        return -std::numeric_limits<double>::infinity();
    }
    if (std::isnan(x) || std::isnan(y)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (x == 0.0 && x == y) {
        return std::signbit(x) ? x : y;
    }
    return x < y ? x : y;
}

void RunUnaryComparison(const UnaryCase& test, InputGenerator& inputs) {
    std::uint64_t max_ulp = 0;
    std::size_t mismatches = 0;
    auto compare = [&](double x) {
        const double actual = test.ours(x);
        const double expected = test.reference(x);
        const std::uint64_t diff = UlpDiff(actual, expected);
        if (diff > 1) {
            ++mismatches;
            if (diff > max_ulp) {
                max_ulp = diff;
            }
        }
    };
    for (double value : Specials()) {
        compare(value);
    }
    for (int i = 0; i < 100000; ++i) {
        compare(inputs.Next());
    }
    std::printf("[math][random] %-7s n=%zu max_ulp=%llu mismatches=%zu\n", test.name,
                static_cast<std::size_t>(Specials().size()) + 100000,
                static_cast<unsigned long long>(max_ulp), mismatches);
    if (mismatches != 0) {
        std::fprintf(stderr, "[math] FAILED: %s mismatches=%zu max_ulp=%llu\n", test.name,
                     mismatches, static_cast<unsigned long long>(max_ulp));
        ++g_failures;
    }
}

void RunBinaryComparison(const BinaryCase& test, InputGenerator& inputs) {
    std::uint64_t max_ulp = 0;
    std::size_t mismatches = 0;
    auto compare = [&](double x, double y) {
        const double actual = test.ours(x, y);
        const double expected = test.reference(x, y);
        const std::uint64_t diff = UlpDiff(actual, expected);
        if (diff > 1) {
            ++mismatches;
            if (diff > max_ulp) {
                max_ulp = diff;
            }
        }
    };
    for (double x : Specials()) {
        for (double y : Specials()) {
            compare(x, y);
        }
    }
    for (int i = 0; i < 100000; ++i) {
        compare(inputs.Next(), inputs.Next());
    }
    std::printf("[math][random] %-7s n=%zu max_ulp=%llu mismatches=%zu\n", test.name,
                100000 + Specials().size() * Specials().size(),
                static_cast<unsigned long long>(max_ulp), mismatches);
    if (mismatches != 0) {
        std::fprintf(stderr, "[math] FAILED: %s mismatches=%zu max_ulp=%llu\n", test.name,
                     mismatches, static_cast<unsigned long long>(max_ulp));
        ++g_failures;
    }
}

void RunModfComparison(InputGenerator& inputs) {
    std::uint64_t max_ulp = 0;
    std::size_t mismatches = 0;
    std::size_t sum_failures = 0;
    auto compare = [&](double x) {
        const m::ModfResult result = m::Modf(x);
        double reference_integral = 0.0;
        double reference_fractional = 0.0;
        if (std::isinf(x)) {
            reference_integral = x;
            reference_fractional = std::numeric_limits<double>::quiet_NaN();
        } else if (std::isnan(x)) {
            reference_integral = x;
            reference_fractional = x;
        } else {
            reference_fractional = std::modf(x, &reference_integral);
        }
        const std::uint64_t diff_integral = UlpDiff(result.integral, reference_integral);
        const std::uint64_t diff_fractional =
            UlpDiff(result.fractional, reference_fractional);
        if (diff_integral > 1 || diff_fractional > 1) {
            ++mismatches;
            max_ulp = std::max(max_ulp, std::max(diff_integral, diff_fractional));
        }
        if (std::isfinite(x) && result.integral + result.fractional != x) {
            ++sum_failures;
        }
    };
    for (double value : Specials()) {
        compare(value);
    }
    for (int i = 0; i < 100000; ++i) {
        compare(inputs.Next());
    }
    std::printf("[math][random] Modf    n=%zu max_ulp=%llu mismatches=%zu sum_failures=%zu\n",
                100000 + Specials().size(),
                static_cast<unsigned long long>(max_ulp), mismatches, sum_failures);
    if (mismatches != 0 || sum_failures != 0) {
        std::fprintf(stderr,
                     "[math] FAILED: Modf mismatches=%zu sum_failures=%zu max_ulp=%llu\n",
                     mismatches, sum_failures, static_cast<unsigned long long>(max_ulp));
        ++g_failures;
    }
}

void RunFrexpComparison(InputGenerator& inputs) {
    std::size_t mismatches = 0;
    std::size_t roundtrip_failures = 0;
    std::size_t range_failures = 0;
    auto compare = [&](double x) {
        const m::FrexpResult result = m::Frexp(x);
        double reference_frac = 0.0;
        int reference_exp = 0;
        if (x == 0.0 || std::isinf(x) || std::isnan(x)) {
            reference_frac = x;
            reference_exp = 0;
        } else {
            reference_frac = std::frexp(x, &reference_exp);
        }
        if (!SameBits(result.frac, reference_frac) || result.exp != reference_exp) {
            ++mismatches;
        }
        if (std::isfinite(x) && x != 0.0) {
            const double magnitude = std::fabs(result.frac);
            if (magnitude < 0.5 || magnitude >= 1.0) {
                ++range_failures;
            }
            if (!SameBits(m::Ldexp(result.frac, result.exp), x)) {
                ++roundtrip_failures;
            }
        }
    };
    for (double value : Specials()) {
        compare(value);
    }
    for (int i = 0; i < 100000; ++i) {
        compare(inputs.Next());
    }
    std::printf(
        "[math][random] Frexp   n=%zu mismatches=%zu roundtrip_failures=%zu range_failures=%zu\n",
        100000 + Specials().size(), mismatches, roundtrip_failures, range_failures);
    if (mismatches != 0 || roundtrip_failures != 0 || range_failures != 0) {
        std::fprintf(stderr,
                     "[math] FAILED: Frexp mismatches=%zu roundtrip=%zu range=%zu\n",
                     mismatches, roundtrip_failures, range_failures);
        ++g_failures;
    }
}

void RunLdexpComparison(InputGenerator& inputs) {
    std::uint64_t max_ulp = 0;
    std::size_t mismatches = 0;
    auto compare = [&](double frac, int exp) {
        const std::uint64_t diff = UlpDiff(m::Ldexp(frac, exp), std::ldexp(frac, exp));
        if (diff > 1) {
            ++mismatches;
            max_ulp = std::max(max_ulp, diff);
        }
    };
    const std::array<int, 12> exponents = {-2000, -1075, -1074, -1022, -1, 0,
                                           1,     2,     1023,  1024, 1075, 2000};
    for (double value : Specials()) {
        for (int exp : exponents) {
            compare(value, exp);
        }
    }
    for (int i = 0; i < 100000; ++i) {
        const int exp = static_cast<int>(inputs.Raw() % 4401U) - 2200;
        compare(inputs.Next(), exp);
    }
    std::printf("[math][random] Ldexp   n=%zu max_ulp=%llu mismatches=%zu\n",
                100000 + Specials().size() * exponents.size(),
                static_cast<unsigned long long>(max_ulp), mismatches);
    if (mismatches != 0) {
        std::fprintf(stderr, "[math] FAILED: Ldexp mismatches=%zu max_ulp=%llu\n",
                     mismatches, static_cast<unsigned long long>(max_ulp));
        ++g_failures;
    }
}

void RunRandomComparisons() {
    std::printf("[math][random] 与系统 libm 对拍（容差 1 ULP；零符号不同计为失败）\n");
    InputGenerator inputs(0x5EED1234ULL);

    const std::array<UnaryCase, 21> unary_cases = {{
        {"Abs", m::Abs, static_cast<UnaryFn>(std::fabs)},
        {"Floor", m::Floor, static_cast<UnaryFn>(std::floor)},
        {"Ceil", m::Ceil, static_cast<UnaryFn>(std::ceil)},
        {"Round", m::Round, static_cast<UnaryFn>(std::round)},
        {"Trunc", m::Trunc, static_cast<UnaryFn>(std::trunc)},
        {"Sqrt", m::Sqrt, static_cast<UnaryFn>(std::sqrt)},
        {"Cbrt", m::Cbrt, static_cast<UnaryFn>(std::cbrt)},
        {"Exp", m::Exp, static_cast<UnaryFn>(std::exp)},
        {"Exp2", m::Exp2, static_cast<UnaryFn>(std::exp2)},
        {"Log", m::Log, static_cast<UnaryFn>(std::log)},
        {"Log2", m::Log2, static_cast<UnaryFn>(std::log2)},
        {"Log10", m::Log10, static_cast<UnaryFn>(std::log10)},
        {"Sin", m::Sin, static_cast<UnaryFn>(std::sin)},
        {"Cos", m::Cos, static_cast<UnaryFn>(std::cos)},
        {"Tan", m::Tan, static_cast<UnaryFn>(std::tan)},
        {"Asin", m::Asin, static_cast<UnaryFn>(std::asin)},
        {"Acos", m::Acos, static_cast<UnaryFn>(std::acos)},
        {"Atan", m::Atan, static_cast<UnaryFn>(std::atan)},
        {"Sinh", m::Sinh, static_cast<UnaryFn>(std::sinh)},
        {"Cosh", m::Cosh, static_cast<UnaryFn>(std::cosh)},
        {"Tanh", m::Tanh, static_cast<UnaryFn>(std::tanh)},
    }};
    for (const auto& test : unary_cases) {
        RunUnaryComparison(test, inputs);
    }

    const std::array<BinaryCase, 7> binary_cases = {{
        {"Max", m::Max, ReferenceMax},
        {"Min", m::Min, ReferenceMin},
        {"Mod", m::Mod, static_cast<BinaryFn>(std::fmod)},
        {"Pow", m::Pow, static_cast<BinaryFn>(std::pow)},
        {"Hypot", m::Hypot, static_cast<BinaryFn>(std::hypot)},
        {"Atan2", m::Atan2, static_cast<BinaryFn>(std::atan2)},
        {"Copysign", m::Copysign, static_cast<BinaryFn>(std::copysign)},
    }};
    for (const auto& test : binary_cases) {
        RunBinaryComparison(test, inputs);
    }

    RunModfComparison(inputs);
    RunFrexpComparison(inputs);
    RunLdexpComparison(inputs);
}

void RunConstantChecks() {
    std::printf("[math][constants] 逐位校验\n");
    CheckBits("Pi", m::Pi, 0x400921fb54442d18ULL);
    CheckBits("E", m::E, 0x4005bf0a8b145769ULL);
    CheckBits("Sqrt2", m::Sqrt2, 0x3ff6a09e667f3bcdULL);
    CheckBits("SqrtE", m::SqrtE, 0x3ffa61298e1e069cULL);
    CheckBits("SqrtPi", m::SqrtPi, 0x3ffc5bf891b4ef6bULL);
    CheckBits("Ln2", m::Ln2, 0x3fe62e42fefa39efULL);
    CheckBits("Log2E", m::Log2E, 0x3ff71547652b82feULL);
    CheckBits("Ln10", m::Ln10, 0x40026bb1bbb55516ULL);
    CheckBits("Log10E", m::Log10E, 0x3fdbcb7b1526e50eULL);
    CheckBits("MaxFloat64", m::MaxFloat64, 0x7fefffffffffffffULL);
    CheckBits("SmallestNonzeroFloat64", m::SmallestNonzeroFloat64, 0x0000000000000001ULL);

    Check(m::MaxInt8 == 127, "MaxInt8");
    Check(m::MinInt8 == -128, "MinInt8");
    Check(m::MaxInt16 == 32767, "MaxInt16");
    Check(m::MinInt16 == -32768, "MinInt16");
    Check(m::MaxInt32 == 2147483647, "MaxInt32");
    Check(m::MinInt32 == -2147483647 - 1, "MinInt32");
    Check(m::MaxInt64 == 9223372036854775807LL, "MaxInt64");
    Check(m::MinInt64 == -9223372036854775807LL - 1, "MinInt64");
    Check(m::MaxUint8 == 255, "MaxUint8");
    Check(m::MaxUint16 == 65535, "MaxUint16");
    Check(m::MaxUint32 == 4294967295U, "MaxUint32");
    Check(m::MaxUint64 == 18446744073709551615ULL, "MaxUint64");
    Check(m::MaxInt == static_cast<std::intptr_t>(9223372036854775807LL), "MaxInt");
    Check(m::MinInt == static_cast<std::intptr_t>(-9223372036854775807LL - 1), "MinInt");
}

void RunSemanticChecks() {
    std::printf("[math][semantics] Go 特殊值语义\n");

    // Abs 与符号零
    Check(m::Abs(-0.0) == 0.0 && !std::signbit(m::Abs(-0.0)), "Abs(-0) == +0");
    Check(std::isnan(m::Abs(m::NaN())), "Abs(NaN) == NaN");

    // Max / Min 的 NaN 与 Inf 顺序
    Check(std::isnan(m::Max(m::NaN(), 1.0)), "Max(NaN, 1) == NaN");
    Check(std::isnan(m::Max(1.0, m::NaN())), "Max(1, NaN) == NaN");
    Check(m::Max(m::Inf, m::NaN()) == m::Inf, "Max(+Inf, NaN) == +Inf");
    Check(m::Max(m::NaN(), m::Inf) == m::Inf, "Max(NaN, +Inf) == +Inf");
    Check(m::Max(-m::Inf, m::Inf) == m::Inf, "Max(-Inf, +Inf) == +Inf");
    Check(std::isnan(m::Min(m::NaN(), 1.0)), "Min(NaN, 1) == NaN");
    Check(m::Min(-m::Inf, m::NaN()) == -m::Inf, "Min(-Inf, NaN) == -Inf");
    Check(m::Min(m::NaN(), -m::Inf) == -m::Inf, "Min(NaN, -Inf) == -Inf");
    Check(std::signbit(m::Max(-0.0, -0.0)), "Max(-0, -0) == -0");
    Check(!std::signbit(m::Max(0.0, -0.0)), "Max(+0, -0) == +0");
    Check(!std::signbit(m::Max(-0.0, 0.0)), "Max(-0, +0) == +0");
    Check(std::signbit(m::Min(0.0, -0.0)), "Min(+0, -0) == -0");
    Check(std::signbit(m::Min(-0.0, 0.0)), "Min(-0, +0) == -0");
    Check(!std::signbit(m::Min(0.0, 0.0)), "Min(+0, +0) == +0");

    // Round 半值远离零；-0 保留
    Check(m::Round(-0.5) == -1.0, "Round(-0.5) == -1");
    Check(m::Round(0.5) == 1.0, "Round(0.5) == 1");
    Check(m::Round(2.5) == 3.0, "Round(2.5) == 3");
    Check(m::Round(-2.5) == -3.0, "Round(-2.5) == -3");
    Check(std::signbit(m::Round(-0.4)), "Round(-0.4) == -0");
    Check(std::signbit(m::Floor(-0.0)), "Floor(-0) == -0");
    Check(std::signbit(m::Ceil(-0.0)), "Ceil(-0) == -0");
    Check(std::signbit(m::Trunc(-0.0)), "Trunc(-0) == -0");

    // Mod 的符号与特殊值
    Check(m::Mod(5.5, 2.0) == 1.5, "Mod(5.5, 2) == 1.5");
    Check(m::Mod(-5.5, 2.0) == -1.5, "Mod(-5.5, 2) == -1.5");
    Check(m::Mod(5.5, -2.0) == 1.5, "Mod(5.5, -2) == 1.5");
    Check(m::Mod(1.0, m::Inf) == 1.0, "Mod(1, +Inf) == 1");
    Check(std::isnan(m::Mod(m::Inf, 2.0)), "Mod(+Inf, 2) == NaN");
    Check(std::isnan(m::Mod(1.0, 0.0)), "Mod(1, 0) == NaN");

    // Pow / Sqrt / Cbrt / Hypot
    Check(m::Pow(2.0, 10.0) == 1024.0, "Pow(2, 10) == 1024");
    Check(m::Pow(-2.0, 3.0) == -8.0, "Pow(-2, 3) == -8");
    Check(std::isnan(m::Pow(-2.0, 0.5)), "Pow(-2, 0.5) == NaN");
    Check(m::Pow(m::NaN(), 0.0) == 1.0, "Pow(NaN, 0) == 1");
    Check(m::Pow(1.0, m::NaN()) == 1.0, "Pow(1, NaN) == 1");
    Check(m::Pow(0.0, -1.0) == m::Inf, "Pow(+0, -1) == +Inf");
    Check(m::Pow(-0.0, -3.0) == -m::Inf, "Pow(-0, -3) == -Inf");
    Check(std::signbit(m::Sqrt(-0.0)), "Sqrt(-0) == -0");
    Check(std::isnan(m::Sqrt(-1.0)), "Sqrt(-1) == NaN");
    // libm 的 cbrt 不保证对完全立方数返回精确整数；允许 1 ULP 级误差。
    CheckNear("Cbrt(-27)", m::Cbrt(-27.0), -3.0, 1e-15);
    Check(std::signbit(m::Cbrt(-0.0)), "Cbrt(-0) == -0");
    Check(m::Hypot(3.0, 4.0) == 5.0, "Hypot(3, 4) == 5");
    Check(m::Hypot(m::Inf, m::NaN()) == m::Inf, "Hypot(+Inf, NaN) == +Inf");

    // 指数 / 对数
    Check(m::Exp(0.0) == 1.0, "Exp(0) == 1");
    Check(m::Exp(-m::Inf) == 0.0, "Exp(-Inf) == 0");
    Check(m::Exp2(10.0) == 1024.0, "Exp2(10) == 1024");
    Check(m::Log(0.0) == -m::Inf, "Log(+0) == -Inf");
    Check(m::Log(-0.0) == -m::Inf, "Log(-0) == -Inf");
    Check(std::isnan(m::Log(-1.0)), "Log(-1) == NaN");
    Check(m::Log2(8.0) == 3.0, "Log2(8) == 3");
    Check(m::Log10(1000.0) == 3.0, "Log10(1000) == 3");

    // 三角 / 双曲
    Check(std::signbit(m::Sin(-0.0)), "Sin(-0) == -0");
    Check(std::isnan(m::Sin(m::Inf)), "Sin(+Inf) == NaN");
    Check(m::Cos(0.0) == 1.0, "Cos(0) == 1");
    Check(m::Tan(m::Pi / 4.0) > 0.9999999999999998, "Tan(Pi/4) ~= 1");
    Check(std::isnan(m::Asin(2.0)), "Asin(2) == NaN");
    Check(std::isnan(m::Acos(-2.0)), "Acos(-2) == NaN");
    Check(m::Atan(1.0) == m::Pi / 4.0, "Atan(1) == Pi/4");
    Check(m::Atan2(1.0, 1.0) == m::Pi / 4.0, "Atan2(1, 1) == Pi/4");
    Check(m::Atan2(0.0, -1.0) == m::Pi, "Atan2(+0, -1) == +Pi");
    Check(m::Atan2(-0.0, -1.0) == -m::Pi, "Atan2(-0, -1) == -Pi");
    Check(std::signbit(m::Sinh(-0.0)), "Sinh(-0) == -0");
    Check(m::Cosh(-m::Inf) == m::Inf, "Cosh(-Inf) == +Inf");
    Check(m::Tanh(m::Inf) == 1.0, "Tanh(+Inf) == 1");
    Check(m::Tanh(-m::Inf) == -1.0, "Tanh(-Inf) == -1");
    Check(std::signbit(m::Tanh(-0.0)), "Tanh(-0) == -0");

    // Modf / Frexp / Ldexp
    const m::ModfResult modf_inf = m::Modf(m::Inf);
    Check(modf_inf.integral == m::Inf && std::isnan(modf_inf.fractional),
          "Modf(+Inf) == +Inf, NaN");
    const m::ModfResult modf_neg = m::Modf(-3.75);
    Check(modf_neg.integral == -3.0 && modf_neg.fractional == -0.75 && std::signbit(modf_neg.integral),
          "Modf(-3.75) == -3, -0.75");
    const m::ModfResult modf_zero = m::Modf(-0.0);
    Check(std::signbit(modf_zero.integral) && std::signbit(modf_zero.fractional),
          "Modf(-0) == -0, -0");
    const m::FrexpResult frexp_zero = m::Frexp(-0.0);
    Check(std::signbit(frexp_zero.frac) && frexp_zero.exp == 0, "Frexp(-0) == -0, 0");
    const m::FrexpResult frexp_inf = m::Frexp(-m::Inf);
    Check(frexp_inf.frac == -m::Inf && frexp_inf.exp == 0, "Frexp(-Inf) == -Inf, 0");
    Check(m::Ldexp(0.5, 3) == 4.0, "Ldexp(0.5, 3) == 4");
    Check(m::Ldexp(1.0, 1024) == m::Inf, "Ldexp(1, 1024) == +Inf");
    Check(m::Ldexp(-1.0, 1024) == -m::Inf, "Ldexp(-1, 1024) == -Inf");
    CheckBits("Ldexp(1, -1074)", m::Ldexp(1.0, -1074), 0x0000000000000001ULL);
    Check(m::Ldexp(1.0, -1075) == 0.0, "Ldexp(1, -1075) == +0");

    // NaN / Inf / 符号
    Check(m::IsNaN(m::NaN()), "IsNaN(NaN)");
    Check(!m::IsNaN(1.0), "!IsNaN(1)");
    Check(m::NaN() != m::NaN(), "NaN != NaN");
    Check(m::IsInf(m::Inf, 0) && m::IsInf(m::Inf, 1) && !m::IsInf(m::Inf, -1),
          "IsInf(+Inf, 0/1/-1)");
    Check(m::IsInf(-m::Inf, 0) && m::IsInf(-m::Inf, -1) && !m::IsInf(-m::Inf, 1),
          "IsInf(-Inf, 0/1/-1)");
    Check(!m::IsInf(1.0, 0), "!IsInf(1, 0)");
    Check(m::Signbit(-0.0) && !m::Signbit(0.0) && m::Signbit(-1.0) && !m::Signbit(1.0),
          "Signbit");
    Check(m::Copysign(3.5, -0.0) == -3.5, "Copysign(3.5, -0) == -3.5");
    Check(m::Copysign(-3.5, 0.0) == 3.5, "Copysign(-3.5, +0) == 3.5");
    const double implicit_inf = m::Inf;
    Check(implicit_inf == m::Inf(1) && std::isinf(implicit_inf), "Inf constant conversion");
}

void ScenarioGeometry() {
    std::printf("[math][场景1] 几何计算：直角三角形 / 余弦定理 / 三维距离\n");
    const double leg_a = 3.0;
    const double leg_b = 4.0;
    CheckNear("Hypot(3, 4)", m::Hypot(leg_a, leg_b), 5.0, 1e-15);
    CheckNear("Sqrt(3^2 + 4^2)", m::Sqrt(leg_a * leg_a + leg_b * leg_b), 5.0, 1e-15);

    const double angle = m::Atan2(leg_a, leg_b);
    CheckNear("Acos(4/5)", m::Acos(leg_b / 5.0), angle, 1e-15);
    CheckNear("Asin(3/5)", m::Asin(leg_a / 5.0), angle, 1e-15);
    CheckNear("Sin(angle) * 5", m::Sin(angle) * 5.0, leg_a, 1e-15);
    CheckNear("Cos(angle) * 5", m::Cos(angle) * 5.0, leg_b, 1e-15);
    CheckNear("Tan(angle)", m::Tan(angle), leg_a / leg_b, 1e-15);
    CheckNear("Atan(3/4)", m::Atan(leg_a / leg_b), angle, 1e-15);

    const double a = 7.0;
    const double b = 8.0;
    const double c = 9.0;
    const double gamma = m::Acos((a * a + b * b - c * c) / (2.0 * a * b));
    CheckNear("law of cosines reconstructs c",
              m::Sqrt(a * a + b * b - 2.0 * a * b * m::Cos(gamma)), c, 1e-12);
    const double semi = (a + b + c) / 2.0;
    CheckNear("Heron area (7,8,9)",
              m::Sqrt(semi * (semi - a) * (semi - b) * (semi - c)), 26.832815729997478,
              1e-12);

    CheckNear("3D distance Hypot(Hypot(3,4),12)", m::Hypot(m::Hypot(3.0, 4.0), 12.0),
              13.0, 1e-15);
}

void ScenarioStatistics() {
    std::printf("[math][场景2] 统计：几何平均 / 指数衰减 / 对数尺度 / 熵\n");
    const std::array<double, 5> samples = {1.0, 2.0, 4.0, 8.0, 16.0};
    double log_sum = 0.0;
    for (double value : samples) {
        log_sum += m::Log(value);
    }
    CheckNear("geometric mean Exp(mean(Log))",
              m::Exp(log_sum / static_cast<double>(samples.size())), 4.0, 1e-14);

    CheckNear("100 * Exp(-0.5 * 2)", 100.0 * m::Exp(-0.5 * 2.0), 36.787944117144235,
              1e-12);
    CheckNear("Log2(1024)", m::Log2(1024.0), 10.0, 0.0);
    CheckNear("Log10(1e6)", m::Log10(1e6), 6.0, 0.0);
    CheckNear("Log(Exp(3))", m::Log(m::Exp(3.0)), 3.0, 1e-15);

    double entropy = 0.0;
    for (int i = 0; i < 8; ++i) {
        const double p = 1.0 / 8.0;
        entropy -= p * m::Log2(p);
    }
    CheckNear("uniform 8-way entropy", entropy, 3.0, 1e-15);

    CheckNear("Sqrt2 * Sqrt2", m::Sqrt2 * m::Sqrt2, 2.0, 1e-15);
    CheckNear("SqrtE * SqrtE", m::SqrtE * m::SqrtE, m::E, 1e-15);
    CheckNear("SqrtPi * SqrtPi", m::SqrtPi * m::SqrtPi, m::Pi, 1e-15);
    CheckNear("Exp(1) == E", m::Exp(1.0), m::E, 1e-15);
    CheckNear("Log(2) == Ln2", m::Log(2.0), m::Ln2, 1e-16);
    CheckNear("1/Ln2 == Log2E", 1.0 / m::Ln2, m::Log2E, 1e-15);
    CheckNear("Log(10) == Ln10", m::Log(10.0), m::Ln10, 1e-16);
    CheckNear("1/Ln10 == Log10E", 1.0 / m::Ln10, m::Log10E, 1e-15);
}

void ScenarioPhysics() {
    std::printf("[math][场景3] 物理/图形：抛体 / 旋转 / 球半径 / 正态密度\n");
    const double speed = 20.0;
    const double theta = m::Pi / 6.0;
    const double time = 1.5;
    const double gravity = 9.81;
    const double x = speed * m::Cos(theta) * time;
    const double y = speed * m::Sin(theta) * time - 0.5 * gravity * time * time;
    CheckNear("projectile x(1.5s)", x, 25.980762113533157, 1e-12);
    CheckNear("projectile y(1.5s)", y, 3.96375, 1e-12);

    // Pi/3 本身不是精确的 60°，三角函数结果与解析值差 1 ULP 属于正常范围。
    CheckNear("Cos(60deg)", m::Cos(m::Pi / 3.0), 0.5, 1e-15);
    CheckNear("Sin(60deg)", m::Sin(m::Pi / 3.0), m::Sqrt(3.0) / 2.0, 1e-15);

    const double volume = 1000.0;
    const double radius = m::Cbrt(volume * 3.0 / (4.0 * m::Pi));
    CheckNear("sphere volume from radius", 4.0 / 3.0 * m::Pi * radius * radius * radius,
              volume, 1e-12);

    const double pdf_at_zero = 1.0 / m::Sqrt(2.0 * m::Pi);
    CheckNear("normal pdf(0)", pdf_at_zero, 0.3989422804014327, 1e-15);

    const double degrees = 370.0;
    const double wrapped = m::Mod(degrees, 360.0);
    const double normalized = wrapped < 0.0 ? wrapped + 360.0 : wrapped;
    CheckNear("angle 370 -> 10", normalized, 10.0, 1e-15);
    CheckNear("Mod(-10, 360)", m::Mod(-10.0, 360.0), -10.0, 1e-15);
}

void ScenarioFinance() {
    std::printf("[math][场景4] 金融：复利 / 连续复利 / 分位取整 / 夹逼\n");
    const double principal = 10000.0;
    const double rate = 0.05;
    const int periods_per_year = 12;
    const int years = 10;
    const double growth = 1.0 + rate / static_cast<double>(periods_per_year);
    const double compound =
        principal * m::Pow(growth, static_cast<double>(periods_per_year * years));
    CheckNear("monthly compound 10y", compound, 16470.09497690285, 1e-10);
    CheckNear("continuous compound 10y", principal * m::Exp(rate * years),
              16487.212707001281, 1e-10);
    CheckNear("effective annual rate", m::Pow(compound / principal, 1.0 / years) - 1.0,
              0.0511618978817327, 1e-14);

    const double amount = 12.375;
    const double cents = m::Round(amount * 100.0) / 100.0;
    CheckNear("round 12.375 to cents", cents, 12.38, 1e-15);
    CheckNear("round -12.375 to cents", m::Round(-amount * 100.0) / 100.0, -12.38, 1e-15);
    CheckNear("Floor(12.7)", m::Floor(12.7), 12.0, 0.0);
    CheckNear("Ceil(12.1)", m::Ceil(12.1), 13.0, 0.0);
    CheckNear("Trunc(-12.7)", m::Trunc(-12.7), -12.0, 0.0);

    CheckNear("clamp -5 to [0,100]", m::Max(0.0, m::Min(100.0, -5.0)), 0.0, 0.0);
    CheckNear("clamp 150 to [0,100]", m::Max(0.0, m::Min(100.0, 150.0)), 100.0, 0.0);
    CheckNear("Abs(-7.5)", m::Abs(-7.5), 7.5, 0.0);

    const double cash = 13.75;
    const double whole = m::Floor(cash);
    const double fraction = m::Mod(cash, 1.0);
    CheckNear("cash split integral", whole, 13.0, 0.0);
    CheckNear("cash split fraction", fraction, 0.75, 0.0);
    CheckNear("cash split sum", whole + fraction, cash, 1e-15);
}

void ScenarioDecomposition() {
    std::printf("[math][场景5] 浮点分解：Frexp/Ldexp/Modf\n");
    const double value = 1234.5678;
    const m::FrexpResult frexp_value = m::Frexp(value);
    CheckNear("Frexp frac in [0.5,1)", std::fabs(frexp_value.frac) >= 0.5 &&
                                              std::fabs(frexp_value.frac) < 1.0
                                          ? 1.0
                                          : 0.0,
              1.0, 0.0);
    CheckBits("Frexp/Ldexp roundtrip", m::Ldexp(frexp_value.frac, frexp_value.exp),
              Bits(value));

    const m::ModfResult modf_value = m::Modf(value);
    CheckNear("Modf integral", modf_value.integral, 1234.0, 0.0);
    // 1234.5678 的二进制表示本身就有误差，小数部分约为 0.5678000000000338。
    CheckNear("Modf fractional", modf_value.fractional, 0.5678, 1e-12);
    CheckNear("Modf sum", modf_value.integral + modf_value.fractional, value, 1e-15);

    const double scaled = m::Ldexp(0.1, 10);
    CheckNear("Ldexp(0.1, 10)", scaled, 102.4, 1e-14);
    const m::FrexpResult scaled_frexp = m::Frexp(scaled);
    CheckNear("Frexp(102.4) frac", scaled_frexp.frac, 0.8, 1e-15);
    Check(scaled_frexp.exp == 7, "Frexp(102.4) exp == 7");

    CheckNear("Modf(-3.75) integral", m::Modf(-3.75).integral, -3.0, 0.0);
    CheckNear("Modf(-3.75) fractional", m::Modf(-3.75).fractional, -0.75, 0.0);
    CheckBits("Ldexp(0.5, -1073) == denorm_min", m::Ldexp(0.5, -1073),
              Bits(m::SmallestNonzeroFloat64));
    Check(m::Ldexp(1.0, 1024) == m::Inf, "Ldexp overflow == +Inf");
    Check(m::Ldexp(1.0, -1075) == 0.0, "Ldexp underflow == +0");
}

void ScenarioSignal() {
    std::printf("[math][场景6] 信号/激活：Tanh / Cosh / Exp2 / softplus\n");
    CheckNear("Tanh(1)", m::Tanh(1.0), 0.7615941559557649, 1e-15);
    CheckNear("Tanh(-1)", m::Tanh(-1.0), -0.7615941559557649, 1e-15);
    CheckNear("Tanh(+Inf)", m::Tanh(m::Inf), 1.0, 0.0);
    CheckNear("Tanh(-Inf)", m::Tanh(-m::Inf), -1.0, 0.0);

    const double a = 2.0;
    const double span = 1.0;
    CheckNear("catenary 2*Cosh(0.5)", a * m::Cosh(span / a), 2.255251930412762, 1e-15);

    for (double x : {0.1, 1.0, 5.0, -3.0}) {
        // |x| 较大时 Cosh 与 Sinh 都很大，相减发生灾难性抵消；1e-9 是该恒等式
        // 在双精度下的合理容差，不是实现误差被放宽。
        CheckNear("Cosh^2 - Sinh^2 == 1",
                  m::Cosh(x) * m::Cosh(x) - m::Sinh(x) * m::Sinh(x), 1.0, 1e-9);
    }

    CheckNear("Exp2(10)", m::Exp2(10.0), 1024.0, 0.0);
    CheckNear("half-life Exp2(-1)", m::Exp2(-1.0), 0.5, 0.0);
    CheckNear("half-life 10 periods", m::Exp2(-10.0), 1.0 / 1024.0, 0.0);

    CheckNear("softplus(0) == Ln2", m::Log(1.0 + m::Exp(0.0)), m::Ln2, 1e-15);
    const double x = 1.5;
    CheckNear("sigmoid == (tanh(x/2)+1)/2", 1.0 / (1.0 + m::Exp(-x)),
              (m::Tanh(x / 2.0) + 1.0) / 2.0, 1e-15);
}

void ScenarioSpecialValues() {
    std::printf("[math][场景7] 传感器校验：NaN/Inf 清洗 / Copysign / 极值\n");
    const double readings[] = {25.5, m::NaN(), m::Inf, -m::Inf, -0.0};
    const double expected[] = {25.5, 0.0, 0.0, 0.0, -0.0};
    for (std::size_t i = 0; i < sizeof(readings) / sizeof(readings[0]); ++i) {
        const double raw = readings[i];
        const double sanitized = (m::IsNaN(raw) || m::IsInf(raw, 0)) ? 0.0 : raw;
        CheckNear("sanitize sensor reading", sanitized, expected[i], 0.0);
    }
    Check(m::Signbit(readings[4]), "sensor -0 preserves sign");

    CheckNear("Copysign(Abs(-3), -0)", m::Copysign(m::Abs(-3.0), -0.0), -3.0, 0.0);
    Check(m::IsInf(m::MaxFloat64 + m::MaxFloat64, 1), "MaxFloat64 + MaxFloat64 == +Inf");
    CheckNear("SmallestNonzero / 2", m::SmallestNonzeroFloat64 / 2.0, 0.0, 0.0);
    CheckNear("Inf function +0", m::Inf(1) > 0.0 ? 1.0 : 0.0, 1.0, 0.0);
    CheckNear("Inf function -0", m::Inf(-1) < 0.0 ? 1.0 : 0.0, 1.0, 0.0);
    Check(m::IsInf(m::Inf, 0), "Inf constant is +Inf");

    Check(std::isnan(m::Max(m::NaN(), 1.0)), "Max(NaN, 1) == NaN");
    Check(m::Max(m::Inf, m::NaN()) == m::Inf, "Max(+Inf, NaN) == +Inf");
    Check(m::Min(-m::Inf, m::NaN()) == -m::Inf, "Min(-Inf, NaN) == -Inf");
}

void ScenarioIntegerLimits() {
    std::printf("[math][场景8] 整数边界常量\n");
    std::printf("    MaxInt8=%d MinInt8=%d MaxUint8=%u\n", static_cast<int>(m::MaxInt8),
                static_cast<int>(m::MinInt8), static_cast<unsigned>(m::MaxUint8));
    std::printf("    MaxInt16=%d MinInt16=%d MaxUint16=%u\n", static_cast<int>(m::MaxInt16),
                static_cast<int>(m::MinInt16), static_cast<unsigned>(m::MaxUint16));
    std::printf("    MaxInt32=%d MinInt32=%d MaxUint32=%u\n", m::MaxInt32, m::MinInt32,
                m::MaxUint32);
    std::printf("    MaxInt64=%lld MinInt64=%lld MaxUint64=%llu\n",
                static_cast<long long>(m::MaxInt64), static_cast<long long>(m::MinInt64),
                static_cast<unsigned long long>(m::MaxUint64));
    std::printf("    MaxInt=%lld MinInt=%lld (intptr_t width=%zu)\n",
                static_cast<long long>(m::MaxInt), static_cast<long long>(m::MinInt),
                sizeof(std::intptr_t) * 8);
    Check(m::MaxInt8 - m::MinInt8 == 255, "MaxInt8 - MinInt8 == 255");
    Check(m::MaxUint8 == static_cast<std::uint8_t>(m::MaxInt8) * 2U + 1U,
          "MaxUint8 == MaxInt8*2 + 1");
    Check(static_cast<std::int64_t>(m::MinInt8) + 1 == -127, "MinInt8 + 1 == -127");
}

template <typename Fn>
void BenchmarkUnary(const char* name, Fn fn, double low, double high,
                    std::size_t iterations) {
    constexpr std::size_t kInputs = 4096;
    std::vector<double> values(kInputs);
    std::mt19937_64 rng(0xC0FFEEULL);
    std::uniform_real_distribution<double> distribution(low, high);
    for (double& value : values) {
        value = distribution(rng);
    }
    volatile double sink = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        sink = sink + fn(values[i & (kInputs - 1)]);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double throughput = static_cast<double>(iterations) / seconds / 1e6;
    std::printf("[math][bench] %-5s %zu 次：%.3f s，%.2f Mops/s（sink=%.6g）\n", name,
                iterations, seconds, throughput, static_cast<double>(sink));
    if (!(seconds > 0.0) || !(throughput > 0.0)) {
        std::fprintf(stderr, "[math] FAILED: invalid benchmark measurement for %s\n", name);
        ++g_failures;
    }
}

void RunBenchmarks() {
    std::printf("[math][bench] 压测：每项 1000 万次，输入预先随机生成（4096 个），"
                "累加结果防止优化器删除\n");
    BenchmarkUnary("sqrt", m::Sqrt, 0.25, 4.0, 10000000);
    BenchmarkUnary("exp", m::Exp, -5.0, 5.0, 10000000);
    BenchmarkUnary("log", m::Log, 0.001, 1000.0, 10000000);
}

}  // namespace

// 编译期校验整数常量与 Inf/NaN 的 constexpr 能力。
static_assert(m::MaxInt8 == 127, "MaxInt8");
static_assert(m::MinInt8 == -128, "MinInt8");
static_assert(m::MaxInt16 == 32767, "MaxInt16");
static_assert(m::MinInt16 == -32768, "MinInt16");
static_assert(m::MaxInt32 == 2147483647, "MaxInt32");
static_assert(m::MinInt32 == -2147483647 - 1, "MinInt32");
static_assert(m::MaxInt64 == 9223372036854775807LL, "MaxInt64");
static_assert(m::MinInt64 == -9223372036854775807LL - 1, "MinInt64");
static_assert(m::MaxUint8 == 255, "MaxUint8");
static_assert(m::MaxUint16 == 65535, "MaxUint16");
static_assert(m::MaxUint32 == 4294967295U, "MaxUint32");
static_assert(m::MaxUint64 == 18446744073709551615ULL, "MaxUint64");
static_assert(m::Pi > 3.14 && m::Pi < 3.15, "Pi in range");
static_assert(m::Inf(1) > 0.0 && m::Inf(-1) < 0.0, "Inf constexpr call");
static_assert(static_cast<double>(m::Inf) > 0.0, "Inf constexpr conversion");

int main() {
    std::printf("=== go2cpp::math 模块测试 ===\n");
    go2cpp_math_test::PrintEnvironment("math");

    RunConstantChecks();
    RunSemanticChecks();
    RunRandomComparisons();

    std::printf("[math][scenarios] 真实场景\n");
    ScenarioGeometry();
    ScenarioStatistics();
    ScenarioPhysics();
    ScenarioFinance();
    ScenarioDecomposition();
    ScenarioSignal();
    ScenarioSpecialValues();
    ScenarioIntegerLimits();

    RunBenchmarks();

    if (g_failures == 0) {
        std::printf("\n[math] failures=0\n");
    } else {
        std::printf("\n[math] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
