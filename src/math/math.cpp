/**
 * @file math.cpp
 * @brief `go2cpp/math/math.hpp` 的实现：Go `math` 最小子集。
 *
 * @details 实现策略（详细取舍见头文件）：
 *   - 绝大多数函数**直接委托**系统 libm（`std::sqrt`/`std::exp`/...），因为这些函数的
 *     IEEE-754 特殊值语义与 Go 文档一致，且 libm 经过充分验证；
 *   - `Max`/`Min` 按 Go 的**判断顺序**自己实现（先 ±Inf，再 NaN，最后 ±0），
 *     不能直接换成 `std::max`/`std::min`；
 *   - `Modf` 对 `±Inf` 返回 `±Inf, NaN`（Go 文档要求），修正 `std::modf` 返回 `±0` 的差异；
 *   - `Frexp` 对 `±0`/`±Inf`/`NaN` 先短路，指数固定为 0。
 *
 *   语义对照来源：`third_party/go1.23.0.src.tar.gz` 内 `go/src/math/`（Go 1.23.0，
 *   BSD-3-Clause）。本文件为独立实现，未复制上游代码。
 *
 * @author Go2Cpp project
 * @date 2026-10-18
 */

#include "go2cpp/math/math.hpp"

#include <cmath>
#include <limits>

namespace go2cpp::math {

namespace {

/// 平台 quiet NaN。Go 使用 0x7FF8000000000001，这里只保证「是 NaN」而不承诺 payload。
double QuietNaN() noexcept {
    return std::numeric_limits<double>::quiet_NaN();
}

}  // namespace

double NaN() noexcept { return QuietNaN(); }

bool IsNaN(double x) noexcept { return x != x; }

bool IsInf(double x, int sign) noexcept {
    if (!std::isinf(x)) {
        return false;
    }
    if (sign > 0) {
        return x > 0.0;
    }
    if (sign < 0) {
        return x < 0.0;
    }
    return true;
}

bool Signbit(double x) noexcept { return std::signbit(x); }

double Copysign(double x, double y) noexcept { return std::copysign(x, y); }

double Abs(double x) noexcept { return std::fabs(x); }

double Max(double x, double y) noexcept {
    // 顺序不能换：Go 先判 +Inf，再判 NaN。所以 Max(+Inf, NaN) 是 +Inf，不是 NaN。
    if (IsInf(x, 1) || IsInf(y, 1)) {
        return Inf(1);
    }
    if (IsNaN(x) || IsNaN(y)) {
        return QuietNaN();
    }
    if (x == 0.0 && x == y) {
        // ±0 的取舍：只要有一个是 +0 就返回 +0；两个 -0 才返回 -0。
        return Signbit(x) ? y : x;
    }
    return x > y ? x : y;
}

double Min(double x, double y) noexcept {
    // 对称地：先判 -Inf，再判 NaN。所以 Min(-Inf, NaN) 是 -Inf。
    if (IsInf(x, -1) || IsInf(y, -1)) {
        return Inf(-1);
    }
    if (IsNaN(x) || IsNaN(y)) {
        return QuietNaN();
    }
    if (x == 0.0 && x == y) {
        // 只要有一个是 -0 就返回 -0；两个 +0 才返回 +0。
        return Signbit(x) ? x : y;
    }
    return x < y ? x : y;
}

double Floor(double x) noexcept { return std::floor(x); }
double Ceil(double x) noexcept { return std::ceil(x); }
double Round(double x) noexcept { return std::round(x); }
double Trunc(double x) noexcept { return std::trunc(x); }
double Mod(double x, double y) noexcept { return std::fmod(x, y); }
double Pow(double x, double y) noexcept { return std::pow(x, y); }
double Sqrt(double x) noexcept { return std::sqrt(x); }
double Cbrt(double x) noexcept { return std::cbrt(x); }
double Hypot(double x, double y) noexcept { return std::hypot(x, y); }

double Exp(double x) noexcept { return std::exp(x); }
double Exp2(double x) noexcept { return std::exp2(x); }
double Log(double x) noexcept { return std::log(x); }
double Log2(double x) noexcept { return std::log2(x); }
double Log10(double x) noexcept { return std::log10(x); }

double Sin(double x) noexcept { return std::sin(x); }
double Cos(double x) noexcept { return std::cos(x); }
double Tan(double x) noexcept { return std::tan(x); }
double Asin(double x) noexcept { return std::asin(x); }
double Acos(double x) noexcept { return std::acos(x); }
double Atan(double x) noexcept { return std::atan(x); }
double Atan2(double y, double x) noexcept { return std::atan2(y, x); }
double Sinh(double x) noexcept { return std::sinh(x); }
double Cosh(double x) noexcept { return std::cosh(x); }
double Tanh(double x) noexcept { return std::tanh(x); }

ModfResult Modf(double f) noexcept {
    if (std::isinf(f)) {
        // Go: Modf(±Inf) = ±Inf, NaN。std::modf 的小数部分是 ±0，必须显式改成 NaN。
        return ModfResult{f, QuietNaN()};
    }
    if (std::isnan(f)) {
        // Go: Modf(NaN) = NaN, NaN。显式返回两份 NaN，避免依赖 libm 对 *iptr 的赋值。
        return ModfResult{f, f};
    }
    double integral = 0.0;
    const double fractional = std::modf(f, &integral);
    return ModfResult{integral, fractional};
}

FrexpResult Frexp(double f) noexcept {
    if (f == 0.0) {
        return FrexpResult{f, 0};  // 保留 -0 的符号，指数为 0
    }
    if (std::isinf(f) || std::isnan(f)) {
        return FrexpResult{f, 0};
    }
    int exp = 0;
    const double frac = std::frexp(f, &exp);
    return FrexpResult{frac, exp};
}

double Ldexp(double frac, int exp) noexcept { return std::ldexp(frac, exp); }

}  // namespace go2cpp::math
