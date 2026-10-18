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
 *
 * ## 模块定位
 *
 * `go2cpp_math` library target 的唯一实现单元，向上支撑 `include/go2cpp/math/math.hpp`
 * 声明的全部函数。没有运行时依赖：不碰 error、不碰调度器，纯 CPU 计算。
 *
 * ## 本文件定义什么
 *
 * - 匿名命名空间的 `QuietNaN()`：全模块唯一的 NaN 来源；
 * - `NaN`/`IsNaN`/`IsInf`/`Signbit`/`Copysign`/`Abs` 等特殊值与基本运算；
 * - `Max`/`Min`/`Modf`/`Frexp`：**不能**直接转发 libm 的四个函数（判断顺序与特殊值
 *   语义与 Go 不同）；
 * - 其余初等函数：一行转发到 `std::*`。
 *
 * ## 阅读建议
 *
 * 直接看 `Max`/`Min`/`Modf`/`Frexp` 四段 —— 它们是本文件里仅有的「有逻辑」的代码；
 * 其余函数都是转发，扫一眼即可。为什么必须自实现的原因写在各自的步骤注释里。
 *
 * ## 边界与陷阱
 *
 * - `QuietNaN()` 只保证「是 NaN」，不承诺 payload（Go 用 `0x7FF8000000000001`）；
 *   依赖 NaN 位模式做序列化/哈希的代码会与 Go 不一致。
 * - `Max`/`Min` 的判断顺序是**语义的一部分**，调整顺序会静默改变 `±Inf` 与 NaN
 *   组合的结果；不要「简化」成 `std::max`/`std::min`。
 * - 所有函数都是 `noexcept`：libm 不会抛异常，本文件也不引入异常路径。
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

/**
 * @brief 按 Go 的判断顺序求最大值 —— 顺序本身就是语义。
 *
 * ## 步骤
 * step1. 任一参数为 `+Inf` 立即返回 `+Inf`。必须先于 NaN 判断：否则
 *        `Max(+Inf, NaN)` 会错误地返回 NaN（`std::max` 的典型行为）；
 * step2. 任一参数为 NaN 则返回 NaN（此时 +Inf 已被排除）；
 * step3. 两者相等且为 0 时，按符号位选 `+0`：`x == y == 0` 只可能是 `±0` 组合；
 * step4. 其余情况直接比较取大者。
 *
 * ## 为什么不能用 `std::max`
 * `std::max(x, y)` 返回 `x < y ? y : x`，对 `Max(+Inf, NaN)` 会返回 NaN，
 * 与 Go 文档「`Max(x, +Inf) = +Inf`」冲突。
 */
double Max(double x, double y) noexcept {
    // step1: +Inf 优先于 NaN，顺序不能换（Go 先判 +Inf，再判 NaN）。
    if (IsInf(x, 1) || IsInf(y, 1)) {
        return Inf(1);
    }
    // step2: 已排除 +Inf，任一 NaN 都返回 NaN。
    if (IsNaN(x) || IsNaN(y)) {
        return QuietNaN();
    }
    // step3: ±0 的取舍 —— 只要有一个是 +0 就返回 +0；两个 -0 才返回 -0。
    if (x == 0.0 && x == y) {
        return Signbit(x) ? y : x;
    }
    // step4: 常规比较。
    return x > y ? x : y;
}

/**
 * @brief 按 Go 的判断顺序求最小值（与 `Max` 对称）。
 *
 * ## 步骤
 * step1. 任一参数为 `-Inf` 立即返回 `-Inf`，先于 NaN 判断，保证 `Min(-Inf, NaN) = -Inf`；
 * step2. 任一参数为 NaN 则返回 NaN；
 * step3. 两者相等且为 0 时按符号位选 `-0`；
 * step4. 其余情况直接比较取小者。
 *
 * ## 为什么不能用 `std::min`
 * 与 `Max` 同理：`std::min(-Inf, NaN)` 返回 NaN，而 Go 文档要求 `-Inf`。
 */
double Min(double x, double y) noexcept {
    // step1: -Inf 优先于 NaN。
    if (IsInf(x, -1) || IsInf(y, -1)) {
        return Inf(-1);
    }
    // step2: 任一 NaN 都返回 NaN。
    if (IsNaN(x) || IsNaN(y)) {
        return QuietNaN();
    }
    // step3: 只要有一个是 -0 就返回 -0；两个 +0 才返回 +0。
    if (x == 0.0 && x == y) {
        return Signbit(x) ? x : y;
    }
    // step4: 常规比较。
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

/**
 * @brief 按 Go 的特殊值规则拆分整数/小数部分。
 *
 * ## 步骤
 * step1. `±Inf` 直接返回 `{f, NaN}` —— `std::modf` 给的是 `±0`，与 Go 文档冲突；
 * step2. `NaN` 直接返回 `{f, f}`，不依赖 libm 如何写 `*iptr`；
 * step3. 其余（含 `±0`、次正规数）交给 `std::modf`，其符号约定与 Go 一致。
 */
ModfResult Modf(double f) noexcept {
    // step1: Go 要求 Modf(±Inf) = ±Inf, NaN。
    if (std::isinf(f)) {
        return ModfResult{f, QuietNaN()};
    }
    // step2: Go 要求 Modf(NaN) = NaN, NaN；显式返回两份，避免依赖 libm 的 *iptr 赋值。
    if (std::isnan(f)) {
        return ModfResult{f, f};
    }
    // step3: 有限值（含 ±0 与次正规数）委托 libm。
    double integral = 0.0;
    const double fractional = std::modf(f, &integral);
    return ModfResult{integral, fractional};
}

/**
 * @brief 按 Go 的特殊值规则拆出尾数与指数。
 *
 * ## 步骤
 * step1. `±0` 直接返回 `{f, 0}`：必须保留 `-0` 的符号位，不能让 `std::frexp` 处理；
 * step2. `±Inf` / `NaN` 返回 `{f, 0}`（Go 文档规定指数为 0）；
 * step3. 有限非零值委托 `std::frexp`，其归一化规则（含次正规数）与 Go 一致。
 */
FrexpResult Frexp(double f) noexcept {
    // step1: ±0 保留符号，指数 0。
    if (f == 0.0) {
        return FrexpResult{f, 0};
    }
    // step2: ±Inf / NaN 的指数固定为 0。
    if (std::isinf(f) || std::isnan(f)) {
        return FrexpResult{f, 0};
    }
    // step3: 有限非零值走 libm。
    int exp = 0;
    const double frac = std::frexp(f, &exp);
    return FrexpResult{frac, exp};
}

double Ldexp(double frac, int exp) noexcept { return std::ldexp(frac, exp); }

}  // namespace go2cpp::math
