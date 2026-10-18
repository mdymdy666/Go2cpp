/**
 * @file math.hpp
 * @brief Go `math` 包最小可用子集的 C++ 对应实现（语义对齐）。
 *
 * @details 复用优先的结论：
 *   | 候选 | 结论 |
 *   |---|---|
 *   | C 标准库 libm（`std::sqrt`/`std::exp`/...） | **采用为底层**：Go 的 `math` 在语义上就是 IEEE-754 双精度初等函数，libm 是经过数十年验证的实现；本项目不重复造轮子 |
 *   | Go 官方 `go/src/math` 的纯 Go 实现 | **只作语义对照**：本机 `third_party/go1.23.0.src.tar.gz`（Go 1.23.0，BSD-3-Clause）。Go 自己也在 amd64 上把多数函数交给汇编/libm 实现，纯 Go 版本主要用于无汇编平台 |
 *   | [musl libm](https://git.musl-libc.org/cgit/musl/tree/src/math)、[SLEEF](https://sleef.org/) | **不采用**：引入外部数学库会显著增加构建依赖，而当前目标只是 Go 语义的最小可用子集 |
 *
 *   **哪些直接委托 libm**：`Abs`/`Floor`/`Ceil`/`Round`/`Trunc`/`Mod`/`Pow`/`Sqrt`/
 *   `Cbrt`/`Hypot`/`Exp`/`Exp2`/`Log`/`Log2`/`Log10`/`Sin`/`Cos`/`Tan`/`Asin`/`Acos`/
 *   `Atan`/`Atan2`/`Sinh`/`Cosh`/`Tanh`/`Copysign`/`Signbit`。这些函数的 IEEE-754
 *   特殊值行为与 Go 文档一致（`Sqrt(-0) == -0`、`Log(±0) == -Inf`、`Atan2(±0, -0) == ±Pi` 等），
 *   逐项对照过 Go 1.23 `go/src/math/` 下各 `.go` 文件的文档与实现。
 *
 *   **哪些自己写**：
 *     - `Max`/`Min`：**不能**直接用 `std::max`/`std::min`（它们对 NaN 的取舍依赖参数顺序），
 *       必须按 Go 的顺序实现：先判 `+Inf`/`-Inf`，再判 NaN，最后处理 ±0；
 *     - `Modf`：Go 对 `±Inf` 返回 `±Inf, NaN`，而 C 的 `std::modf` 返回 `±Inf, ±0`，
 *       这个分歧必须显式修掉；
 *     - `Frexp`：`±0`/`±Inf`/`NaN` 的指数固定为 0，先短路再委托 `std::frexp`；
 *     - `NaN`/`Inf`/`IsNaN`/`IsInf`：只做位级/特殊值判断，无需 libm 调用。
 *
 *   语义对照来源：`third_party/go1.23.0.src.tar.gz` 内的 `go/src/math/`
 *   （Go 1.23.0，BSD-3-Clause），**独立实现，未复制上游代码**。
 *
 * @note **与 Go 的已知差异（可接受）**：
 *       - 超越函数的**末位**可能与 Go 的纯 Go 实现差 1 ULP；本模块保证与**系统 libm**
 *         一致，随机对拍实测最大 0 ULP（见 `tests/math/test_math.cpp`）。Go 也不保证
 *         跨平台逐位一致，因此把「1 ULP 内」作为验收边界；
 *       - `NaN()` 返回平台 libm 的 quiet NaN（x86_64 上为 `0x7ff8000000000000`），
 *         Go 的 `NaN()` 使用 `0x7FF8000000000001`；NaN 的 payload 不属于 Go 的 API 契约，
 *         本模块只保证 `IsNaN` 与符号位行为。
 *
 *   ## 已知未支持项及原因（C 类，有意不做）
 *
 *   | 未支持项 | 不做的原因 |
 *   |---|---|
 *   | `Gamma`/`Lgamma`/`Erf`/`Erfc`/`J0`/`Y0`/`J1`/`Y1`（及 `*inc` 变体） | 特殊函数，纯 C++ 实现需数百行多项式/连分式，精度验收还要独立 oracle；`libm` 只有 `tgamma`/`lgamma`/`erf`/`erfc`/`j0` 等 GNU 扩展，边界语义（极点、负整数、NaN）与 Go 不完全一致。当前项目无调用场景，因此**不引入半成品**。需要时可单独引入 musl/SLEEF。 |
 *   | `Dim`（`max(x-y, 0)` 且 NaN 传播） | 可用现有 `Max(x-y, 0)` 表达，没有兼容性风险，不单列 API。 |
 *   | `Lgamma` 的符号输出（Go 的 `(lgamma, sign)` 双返回值） | 依赖上一行的特殊函数实现。 |
 *   | FMA/汇编加速 | 编译器已为 `std::sqrt` 生成 `sqrtsd`；其余超越函数由系统 libm 选最优实现，自己写汇编收益小、可移植性差。 |
 *
 * @warning C++ 不允许同名函数与变量共存。为同时支持 Go 的函数调用 `Inf(sign)` 与
 *          常量式用法 `math.Inf`，本模块把 `Inf` 定义成**可调用常量对象**：
 *          `math::Inf(1)` 走 `operator()`，`double x = math::Inf;` 走隐式转换。
 *          注意 `auto x = math::Inf;` 得到的是对象而不是 `double`，需要显式写成
 *          `double x = math::Inf;`（或直接调用 `math::Inf(1)`）。
 *
 * ## 模块定位
 *
 * Go 标准库的**纯计算适配层**，位于最底层且**零依赖**（不碰 error、不碰运行时）。
 * 上游是 Go `math` 包的函数与常量语义，下游是 `src/math/math.cpp` 的实现与
 * `tests/math/test_math.cpp` 的逐值对拍；业务代码若要做数值计算可直接使用。
 *
 * ## 本文件定义什么
 *
 * - 常量：`Pi`/`E`/`Sqrt2` 等超越常数，`MaxFloat64`/`SmallestNonzeroFloat64` 等浮点边界，
 *   `MaxInt8`…`MaxUint64`/`MaxInt` 等整数边界；
 * - 特殊值与位级转换：`Inf`（可调用常量）、`NaN`、`IsNaN`、`IsInf`、`Signbit`、
 *   `Copysign`、`Float32bits`/`Float64bits` 及其反向构造；
 * - 初等函数声明：基本运算、指数对数、三角/双曲、`Modf`/`Frexp`/`Ldexp`。
 *
 * ## 阅读建议
 *
 * 先读 @details 里「哪些直接委托 libm、哪些必须自己写」的清单，理解 `Max`/`Min` 的
 * 判断顺序与 `Modf` 对 `±Inf` 的修正；再到 `src/math/math.cpp` 看实现。位级转换
 * （`Float64bits` 等）建议先看 `std::memcpy` 那段注释，理解为什么不能用 `union`。
 *
 * ## 边界与陷阱
 *
 * - 超越函数只保证与**系统 libm** 逐位一致，不保证与 Go 纯 Go 实现一致（末位可能差
 *   1 ULP）；需要跨平台可复现的数值请自行固定实现，不要把本模块当 oracle。
 * - `math::Inf` 是对象而非函数：`auto x = math::Inf;` 的类型是 `InfConstant`，
 *   参与重载解析时可能与 `double` 版本不同，写 `double x = math::Inf;` 最稳。
 * - 所有函数都标了 `noexcept`：NaN/Inf 输入不会抛异常，而是按 IEEE-754 传播。
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <limits>

namespace go2cpp::math {

// ============================================================================
// 常量（对应 Go math 包的常量）
// ============================================================================

/// 圆周率 π（Go `math.Pi`，bits = 0x400921fb54442d18）。
inline constexpr double Pi = 3.14159265358979323846264338327950288419716939937510582097494459;
/// 自然常数 e（Go `math.E`，bits = 0x4005bf0a8b145769）。
inline constexpr double E = 2.71828182845904523536028747135266249775724709369995957496696763;
/// √2（Go `math.Sqrt2`，bits = 0x3ff6a09e667f3bcd）。
inline constexpr double Sqrt2 = 1.41421356237309504880168872420969807856967187537694807317667974;
/// √e（Go `math.SqrtE`，bits = 0x3ffa61298e1e069c）。
inline constexpr double SqrtE = 1.64872127070012814684865078781416357165377610071014801157507931;
/// √π（Go `math.SqrtPi`，bits = 0x3ffc5bf891b4ef6b）。
inline constexpr double SqrtPi = 1.77245385090551602729816748334114518279754945612238712821380779;
/// ln2（Go `math.Ln2`，bits = 0x3fe62e42fefa39ef）。
inline constexpr double Ln2 = 0.693147180559945309417232121458176568075500134360255254120680009;
/// log2(e) = 1/ln2（Go `math.Log2E`，bits = 0x3ff71547652b82fe）。
inline constexpr double Log2E = 1.442695040888963407359924681001892137426645954152985934135449408;
/// ln10（Go `math.Ln10`，bits = 0x40026bb1bbb55516）。
inline constexpr double Ln10 = 2.30258509299404568401799145468436420760110148862877297603332790;
/// log10(e) = 1/ln10（Go `math.Log10E`，bits = 0x3fdbcb7b1526e50e）。
inline constexpr double Log10E = 0.434294481903251827651128918916605082294397005803666566114453783;

/// 最大有限 float64（Go `math.MaxFloat64`，bits = 0x7fefffffffffffff）。
inline constexpr double MaxFloat64 = std::numeric_limits<double>::max();
/// 最小非零 float64（Go `math.SmallestNonzeroFloat64`，bits = 0x0000000000000001）。
inline constexpr double SmallestNonzeroFloat64 = std::numeric_limits<double>::denorm_min();
/// 最大有限 float32（Go `math.MaxFloat32`，bits = 0x7f7fffff）。
inline constexpr float MaxFloat32 = std::numeric_limits<float>::max();
/// 最小非零 float32（Go `math.SmallestNonzeroFloat32`，bits = 0x00000001）。
inline constexpr float SmallestNonzeroFloat32 = std::numeric_limits<float>::denorm_min();

// ----------------------------------------------------------------------------
// 整数边界（对应 Go math 包里的 MaxInt8 / MinInt8 / ... 常量）
//
// Go 把这些常量放在 math 包的 const.go 里；C++ 侧用 numeric_limits 取值，
// 避免手写十进制/十六进制时漏掉边界（尤其是 MinInt64 无法用正字面量取负）。
// ----------------------------------------------------------------------------

/// 对应 `math.MaxInt8`。
inline constexpr std::int8_t MaxInt8 = std::numeric_limits<std::int8_t>::max();
/// 对应 `math.MinInt8`。
inline constexpr std::int8_t MinInt8 = std::numeric_limits<std::int8_t>::min();
/// 对应 `math.MaxInt16`。
inline constexpr std::int16_t MaxInt16 = std::numeric_limits<std::int16_t>::max();
/// 对应 `math.MinInt16`。
inline constexpr std::int16_t MinInt16 = std::numeric_limits<std::int16_t>::min();
/// 对应 `math.MaxInt32`。
inline constexpr std::int32_t MaxInt32 = std::numeric_limits<std::int32_t>::max();
/// 对应 `math.MinInt32`。
inline constexpr std::int32_t MinInt32 = std::numeric_limits<std::int32_t>::min();
/// 对应 `math.MaxInt64`。
inline constexpr std::int64_t MaxInt64 = std::numeric_limits<std::int64_t>::max();
/// 对应 `math.MinInt64`。
inline constexpr std::int64_t MinInt64 = std::numeric_limits<std::int64_t>::min();

/// 对应 `math.MaxUint8`。
inline constexpr std::uint8_t MaxUint8 = std::numeric_limits<std::uint8_t>::max();
/// 对应 `math.MaxUint16`。
inline constexpr std::uint16_t MaxUint16 = std::numeric_limits<std::uint16_t>::max();
/// 对应 `math.MaxUint32`。
inline constexpr std::uint32_t MaxUint32 = std::numeric_limits<std::uint32_t>::max();
/// 对应 `math.MaxUint64`。
inline constexpr std::uint64_t MaxUint64 = std::numeric_limits<std::uint64_t>::max();

/// 对应 `math.MaxInt`。Go 的 `int` 宽度等于指针宽度（amd64 上为 64 位），
/// 这里用 `std::intptr_t` 表达同一平台约定；32 位平台上会自动变成 32 位边界。
inline constexpr std::intptr_t MaxInt = std::numeric_limits<std::intptr_t>::max();
/// 对应 `math.MinInt`。
inline constexpr std::intptr_t MinInt = std::numeric_limits<std::intptr_t>::min();

// ============================================================================
// 特殊值（对应 math.NaN / math.Inf / math.IsNaN / math.IsInf / math.Signbit / math.Copysign）
// ============================================================================

/**
 * @brief 对应 `math.Inf(sign)`，同时提供常量式用法 `math.Inf`。
 * @details `sign >= 0` 返回 `+Inf`，否则返回 `-Inf`（与 Go 一致）。
 *          隐式转换到 `double` 得到 `+Inf`，因此 `double x = math::Inf;` 可直接使用。
 *
 * ## 定位
 *
 * 为同时支持 Go 的 `math.Inf(sign)` 调用形态与 `math.Inf` 常量形态而存在；
 * `src/math/math.cpp` 的 `Max`/`Min` 用它返回 `±Inf`，用户代码用隐式转换取 `+Inf`。
 *
 * ## 不变量
 *
 * - `operator()(sign)` 对 `sign >= 0` 恒为 `+Inf`、对 `sign < 0` 恒为 `-Inf`；
 *   `sign == 0` 走 `>= 0` 分支，所以 `Inf(0) == +Inf`。
 * - 隐式转换与 `operator()(0)` 恒等（都为 `+Inf`），二者不会产生分歧。
 */
struct InfConstant final {
    /// 对应 `math.Inf(sign)`。
    constexpr double operator()(int sign) const noexcept {
        return sign >= 0 ? std::numeric_limits<double>::infinity()
                         : -std::numeric_limits<double>::infinity();
    }

    /// 常量式用法：`double x = math::Inf;`。
    constexpr operator double() const noexcept {
        return std::numeric_limits<double>::infinity();
    }
};

/// 可调用常量：`Inf(1)` / `Inf(-1)` / `Inf(0)` 或 `double x = Inf;`。
inline constexpr InfConstant Inf{};

/// 对应 `math.NaN()`：返回一个 quiet NaN。
double NaN() noexcept;

/**
 * @brief 对应 `math.IsNaN`。
 * @details 用 `x != x` 判断，不依赖 libm 的 `isnan` 宏展开形式。
 */
bool IsNaN(double x) noexcept;

/**
 * @brief 对应 `math.IsInf(x, sign)`。
 * @param x 待判断值。
 * @param sign `> 0` 只认 `+Inf`，`< 0` 只认 `-Inf`，`== 0` 两者都认。
 */
bool IsInf(double x, int sign) noexcept;

/// 对应 `math.Signbit`：返回 true 当且仅当符号位为 1（`-0.0`、负数、负 NaN 均为 true）。
bool Signbit(double x) noexcept;

/// 对应 `math.Copysign`：返回 magnitude 为 `x`、符号为 `y` 的值。
double Copysign(double x, double y) noexcept;

// ----------------------------------------------------------------------------
// 位级转换（对应 Go math 包的 Float32bits / Float32frombits / Float64bits /
// Float64frombits）
//
// 用 std::memcpy 而不是 reinterpret_cast / union：严格别名规则下只有 memcpy
// 是合法且可被优化器识别的“按位重解释”；x86/ARM 上都会编译成一次 mov。
// 这四个函数**保留 NaN 的 payload 与符号位**，不做任何浮点运算。
// ----------------------------------------------------------------------------

/// 对应 `math.Float32bits`：返回 float32 的 IEEE-754 位模式。
inline std::uint32_t Float32bits(float f) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

/// 对应 `math.Float32frombits`：按位模式构造 float32（可为 NaN/Inf/次正规数）。
inline float Float32frombits(std::uint32_t bits) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

/// 对应 `math.Float64bits`：返回 float64 的 IEEE-754 位模式。
inline std::uint64_t Float64bits(double f) noexcept {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

/// 对应 `math.Float64frombits`：按位模式构造 float64（可为 NaN/Inf/次正规数）。
inline double Float64frombits(std::uint64_t bits) noexcept {
    double f = 0.0;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// ============================================================================
// 基本运算
// ============================================================================

/// 对应 `math.Abs`：`Abs(±0) = +0`，`Abs(NaN) = NaN`。
double Abs(double x) noexcept;

/**
 * @brief 对应 `math.Max`。
 * @details Go 的顺序语义**必须**保留：
 *          `Max(x, +Inf) = +Inf`（即使另一个是 NaN）；`Max(x, NaN) = NaN`；
 *          `Max(+0, ±0) = +0`；`Max(-0, -0) = -0`。
 *          这三点都与 `std::max` 的直接用法不同。
 */
double Max(double x, double y) noexcept;

/**
 * @brief 对应 `math.Min`。
 * @details `Min(x, -Inf) = -Inf`（即使另一个是 NaN）；`Min(x, NaN) = NaN`；
 *          `Min(-0, ±0) = -0`。
 */
double Min(double x, double y) noexcept;

/// 对应 `math.Floor`：向 −∞ 取整；`Floor(±0) = ±0`。
double Floor(double x) noexcept;
/// 对应 `math.Ceil`：向 +∞ 取整；`Ceil(±0) = ±0`。
double Ceil(double x) noexcept;
/// 对应 `math.Round`：四舍五入、**半值远离零**（`Round(-0.5) = -1`、`Round(2.5) = 3`）。
double Round(double x) noexcept;
/// 对应 `math.Trunc`：向零取整；`Trunc(±0) = ±0`。
double Trunc(double x) noexcept;
/// 对应 `math.Mod`：余数符号跟随 `x`，与 C 的 `std::fmod` 语义一致。
double Mod(double x, double y) noexcept;
/// 对应 `math.Pow`：IEEE-754 `pow` 语义（`Pow(NaN, 0) = 1`、`Pow(1, NaN) = 1`）。
double Pow(double x, double y) noexcept;
/// 对应 `math.Sqrt`：`Sqrt(-0) = -0`，`Sqrt(x < 0) = NaN`。
double Sqrt(double x) noexcept;
/// 对应 `math.Cbrt`：负数的立方根为负；`Cbrt(±0) = ±0`。
double Cbrt(double x) noexcept;
/// 对应 `math.Hypot`：`sqrt(x*x + y*y)` 的防溢出实现；任一参数为 `±Inf` 时返回 `+Inf`（即使另一个是 NaN）。
double Hypot(double x, double y) noexcept;

// ============================================================================
// 指数与对数
// ============================================================================

/// 对应 `math.Exp`：`Exp(-Inf) = 0`，`Exp(+Inf) = +Inf`。
double Exp(double x) noexcept;
/// 对应 `math.Exp2`：2 的 `x` 次幂。
double Exp2(double x) noexcept;
/// 对应 `math.Log`：自然对数；`Log(±0) = -Inf`，`Log(x < 0) = NaN`。
double Log(double x) noexcept;
/// 对应 `math.Log2`。
double Log2(double x) noexcept;
/// 对应 `math.Log10`。
double Log10(double x) noexcept;

// ============================================================================
// 三角函数与双曲函数
// ============================================================================

/// 对应 `math.Sin`：`Sin(±0) = ±0`，`Sin(±Inf) = NaN`。
double Sin(double x) noexcept;
/// 对应 `math.Cos`。
double Cos(double x) noexcept;
/// 对应 `math.Tan`。
double Tan(double x) noexcept;
/// 对应 `math.Asin`：`|x| > 1` 返回 NaN。
double Asin(double x) noexcept;
/// 对应 `math.Acos`：`|x| > 1` 返回 NaN。
double Acos(double x) noexcept;
/// 对应 `math.Atan`：`Atan(±Inf) = ±Pi/2`。
double Atan(double x) noexcept;
/// 对应 `math.Atan2`：按 `y`、`x` 的符号定象限。
double Atan2(double y, double x) noexcept;
/// 对应 `math.Sinh`。
double Sinh(double x) noexcept;
/// 对应 `math.Cosh`：偶函数，`Cosh(±Inf) = +Inf`。
double Cosh(double x) noexcept;
/// 对应 `math.Tanh`：`Tanh(±Inf) = ±1`。
double Tanh(double x) noexcept;

// ============================================================================
// 浮点分解
// ============================================================================

/// `math.Modf` 的返回值：`integral + fractional == f`（有限值时）。
/**
 * ## 定位
 *
 * `Modf` 的双返回值载体，供 `tests/math/test_math.cpp` 逐分支校验；Go 侧对应
 * `func Modf(f float64) (int float64, frac float64)`。
 *
 * ## 不变量
 *
 * - 有限输入下 `integral + fractional == f` 严格成立，两部分符号都与 `f` 相同。
 * - `f` 为 `±Inf` 时 `fractional` 是 NaN（Go 文档要求），此时上面的等式不成立。
 * - `f` 为 NaN 时两部分都是 NaN。
 */
struct ModfResult final {
    /// 整数部分，符号与 `f` 相同（`f = -0` 时为 `-0`）。
    double integral;
    /// 小数部分，符号与 `f` 相同。
    double fractional;
};

/**
 * @brief 对应 `math.Modf`。
 * @details 对 `±Inf` 返回 `±Inf, NaN`（Go 文档明确规定；`std::modf` 返回的是 `±Inf, ±0`，
 *          所以这里单独处理），对 `NaN` 返回 `NaN, NaN`。
 */
ModfResult Modf(double f) noexcept;

/// `math.Frexp` 的返回值：`frac * 2^exp == f`，且 `0.5 <= |frac| < 1`（有限非零时）。
/**
 * ## 定位
 *
 * `Frexp` 的双返回值载体，供 `tests/math/test_math.cpp` 与需要拆解浮点指数的调用方
 * 使用；Go 侧对应 `func Frexp(f float64) (frac float64, exp int)`。
 *
 * ## 不变量
 *
 * - 有限非零输入下 `frac * 2^exp == f` 精确成立，且 `0.5 <= |frac| < 1`。
 * - `±0`、`±Inf`、`NaN` 输入的 `exp` 恒为 0，`frac` 等于输入（保留符号）。
 */
struct FrexpResult final {
    /// 归一化后的尾数，区间 `[0.5, 1)`（负数时 `(-1, -0.5]`）。
    double frac;
    /// 二进制指数。
    int exp;
};

/**
 * @brief 对应 `math.Frexp`。
 * @details `Frexp(±0) = ±0, 0`；`Frexp(±Inf) = ±Inf, 0`；`Frexp(NaN) = NaN, 0`（Go 文档）。
 *          有限非零值委托 `std::frexp`，其归一化规则与 Go 的 `normalize` 一致（含次正规数）。
 */
FrexpResult Frexp(double f) noexcept;

/// 对应 `math.Ldexp`：返回 `frac * 2^exp`；溢出为 `±Inf`，下溢为 `±0`。
double Ldexp(double frac, int exp) noexcept;

}  // namespace go2cpp::math
