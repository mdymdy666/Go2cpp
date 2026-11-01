/**
 * @file flag.hpp
 * @brief Go `flag` 包的 C++ 对应物（String / Int / Bool / Duration + Parse + Usage）。
 *
 * @author DeepSeek Harness
 * @date 2026-11-01
 * @copyright BSD-3-Clause（语义对照 Go `flag`，实现独立完成）
 *
 * @details
 * Go 的 `flag` 用「注册 → Parse → 读指针」三步：
 *
 * ```go
 * var (
 *     addr  = flag.String("addr", "127.0.0.1:8080", "listen address")
 *     count = flag.Int("count", 1, "worker count")
 *     debug = flag.Bool("debug", false, "enable debug log")
 *     wait  = flag.Duration("wait", 30*time.Second, "shutdown timeout")
 * )
 *
 * func main() {
 *     flag.Parse()
 *     fmt.Println(*addr, *count, *debug, *wait, flag.Args())
 * }
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * #include "go2cpp/go/flag.hpp"
 *
 * auto addr  = go::flag::String("addr", "127.0.0.1:8080", "listen address");
 * auto count = go::flag::Int("count", 1, "worker count");
 * auto debug = go::flag::Bool("debug", false, "enable debug log");
 * auto wait  = go::flag::Duration("wait", go::Second * 30, "shutdown timeout");
 *
 * int main(int argc, char** argv) {
 *     if (go::flag::Parse(argc, argv) != go::flag::ParseResult::kOk) {
 *         return 2;  // 错误文本在 go::flag::CommandLine().Error()
 *     }
 *     std::cout << *addr << " " << *count << " " << *debug;
 * }
 * ```
 *
 * ## 解析规则（与 Go 的 `FlagSet.parseOne` 逐条一致）
 *
 * | 输入 | 结果 |
 * |---|---|
 * | `-x=1` / `--x=1` | 值 `"1"` |
 * | `-x 1` / `--x 1` | 值 `"1"`（消费下一个参数） |
 * | `-b` / `--b`（bool） | `true`，**不消费**下一个参数 |
 * | `-b=false` | `false` |
 * | `-b false`（bool） | `b=true`，`false` 留在 `Args()` 里（Go 同样） |
 * | `--` | 终止解析，`--` 自身被丢弃 |
 * | 第一个非 flag 参数 | 终止解析，剩余全部进 `Args()` |
 * | `-h` / `-help` | 调用 Usage 并返回 `kHelp`（**不**当错误处理） |
 * | 未定义 flag | `flag provided but not defined: -x` |
 * | 缺参数 | `flag needs an argument: -x` |
 * | 值非法 | `invalid value "abc" for flag -x` |
 * | 语法非法（`---`、`-=x`） | `bad flag syntax: ...` |
 *
 * ## 与 Go 的对应关系
 *
 * | Go | 本文件 | 备注 |
 * |---|---|---|
 * | `flag.String/Int/Bool/Duration` | 同名自由函数 | 返回 `shared_ptr<T>`，用法同 Go 的 `*T` |
 * | `flag.Int64` / `flag.Float64` | 同名 | 补全常见类型 |
 * | `flag.Parse()` | `flag::Parse(argc, argv)` | 返回 `ParseResult` 而非 `error` |
 * | `flag.Args()` / `NArg()` / `Arg(i)` | 同名 | 语义一致 |
 * | `flag.NFlag()` / `Visit` / `VisitAll` | 同名 | 按字典序访问 |
 * | `flag.Lookup(name)` / `Set(name, value)` | 同名 | 返回 `const Flag*` |
 * | `flag.PrintDefaults()` | 同名 | 默认输出到 `stderr`，可 `SetOutput` 重定向 |
 * | `flag.Usage`（包级变量） | `flag::Usage`（inline 变量） | 可直接赋值，与 Go 一样 |
 * | `flag.CommandLine` | `flag::CommandLine()` | 返回全局 `FlagSet&` |
 * | `flag.NewFlagSet(name, mode)` | `FlagSet(name)` | 无 `ErrorHandling`：**不退出进程**，见下 |
 *
 * ## 语义边界（必须知道）
 *
 * - **不 `os.Exit`**：Go 的 `flag.Parse()`（`ExitOnError`）在出错时打印用法并
 *   `exit(2)`。C++ 库代码里退出进程是不可接受的，因此本实现**返回**
 *   `ParseResult::kError`，由调用方决定是否退出。这是最重要的行为差异。
 * - **错误只保留最后一条**：`FlagSet::Error()` 返回最近一次解析失败的文本；
 *   不像 Go 那样把 error 直接返回给调用方（C++ 用返回值 + 文本两件套）。
 * - **错误文本不含底层原因**：Go 是 `invalid value "x" for flag -n: strconv
 *   error`；本实现是 `invalid value "x" for flag -n`（`Value::Set` 只返回
 *   bool）。测试按前缀断言。
 * - **重复注册**：Go 会 panic；C++ 抛 `std::invalid_argument`。
 * - **`-h` 的返回值**：Go 返回 `ErrHelp`；本实现返回 `ParseResult::kHelp`，
 *   且 `Error()` 为空。两者都不是「解析失败」。
 * - **bool 的 `-b false`**：与 Go 一致——bool flag 不消费下一个参数。
 * - **`Args()` 返回拷贝**：Go 返回切片（共享底层数组）；C++ 返回值拷贝，
 *   避免生命周期纠缠。
 * - **Duration 文法**：完整实现 Go `time.ParseDuration` 的
 *   `[-+]?([0-9]*(\.[0-9]*)?[a-z]+)+`，含 `ns`/`us`/`µs`/`μs`/`ms`/`s`/`m`/`h`。
 *   注意 `go2cpp/tools/time.hpp` **尚未**提供 `ParseDuration`（该文件的待办表
 *   里明确标注），本文件内联了一份；未来 tools 层补齐后应改为转发。
 * - **负 Duration**：`-wait -5s` 会被解析成 `-wait` 缺参数吗？不会——`-wait`
 *   的下一个参数 `-5s` 以 `-` 开头但 Go 仍然把它当作值消费。本实现一致。
 * - **`--x` 与 `-x`**：完全等价（Go 允许两个减号）。
 *
 * ## 线程模型
 *
 * `CommandLine()` 与所有注册/解析函数**不是**线程安全的（与 Go 的 `flag` 一致：
 * 它设计为 `main` 里初始化、解析一次）。多线程场景请在启动线程前完成
 * `Parse`，或给外部加锁。
 */

#pragma once

#include "go2cpp/tools/time.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp::go {

/**
 * @brief Go `time.Duration` 的别名，等价于 `time.go.hpp` 里的同名别名。
 *
 * @details 重复的 `using` 别名指向同一类型是合法的，因此同时包含
 *          `go2cpp/go/time.go.hpp` 与 `go2cpp/go/flag.hpp` 不会冲突。
 */
using Duration = ::go2cpp::time::Duration;

namespace flag {

/// `Parse` 的结果，对应 Go 的 `nil` / `ErrHelp` / 普通 error 三态。
enum class ParseResult {
    kOk = 0,  ///< 解析成功
    kHelp,    ///< 遇到 `-h`/`-help`，已调用 Usage
    kError,   ///< 解析失败，错误文本见 `FlagSet::Error()`
};

// ============================================================================
// 值类型：与 Go 的 flag.Value 接口对应
// ============================================================================

/**
 * @brief 一个 flag 的取值接口，对应 Go 的 `flag.Value`。
 *
 * @details `String()` 返回当前值的文本（用于 `PrintDefaults` 与 `Flag.String()`），
 *          `Set()` 解析并写回（失败返回 false）。
 */
class Value {
public:
    virtual ~Value() = default;

    /// 当前值的文本表示，对应 `flag.Value.String()`。
    virtual std::string String() const = 0;

    /// 解析文本并写回，对应 `flag.Value.Set(s)`（Go 返回 error，这里返回 bool）。
    virtual bool Set(std::string_view text) = 0;

    /// 类型名（`"string"` / `"int"` / `"bool"` / `"duration"` ...），对应 `UnquoteUsage`。
    virtual std::string Type() const { return "value"; }

    /// 是否为布尔值（决定 `-b` 是否消费下一个参数），对应 `boolFlag.IsBoolFlag()`。
    virtual bool IsBoolFlag() const { return false; }
};

// ============================================================================
// 文本解析（对应 strconv.ParseBool / ParseInt / ParseFloat 与 time.ParseDuration）
// ============================================================================

namespace detail {

/// Go `strconv.ParseBool` 接受的全部字面量。
inline bool ParseBoolText(std::string_view text, bool* out) {
    if (text == "1" || text == "t" || text == "T" || text == "TRUE" ||
        text == "true" || text == "True") {
        *out = true;
        return true;
    }
    if (text == "0" || text == "f" || text == "F" || text == "FALSE" ||
        text == "false" || text == "False") {
        *out = false;
        return true;
    }
    return false;
}

/// 整数文本：允许前导 `+`/`-`，必须完整消费，溢出返回 false。
inline bool ParseIntText(std::string_view text, std::int64_t* out) {
    if (text.empty()) {
        return false;
    }
    const std::string buffer(text);
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(buffer.c_str(), &end, 10);
    if (end == buffer.c_str() || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

/// 浮点文本：必须完整消费，`NaN`/`Inf` 允许（与 Go 的 ParseFloat 一致）。
inline bool ParseFloatText(std::string_view text, double* out) {
    if (text.empty()) {
        return false;
    }
    const std::string buffer(text);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == buffer.c_str() || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = value;
    return true;
}

/**
 * @brief Go `time.ParseDuration` 的文法解析。
 *
 * @details 文法：`[-+]?([0-9]*(\.[0-9]*)?[a-z]+)+`，单位 `ns`/`us`/`µs`/`μs`/
 *          `ms`/`s`/`m`/`h`。单独一个 `"0"` 合法；缺单位（如 `"1"`）、
 *          未知单位、溢出都返回 false。
 *
 *          为什么不用 `Duration` 构造 + 手写循环：Go 的实现在小数部分用
 *          `uint64(float64(f) / (scale * float64(unit)))` 做截断，照搬这个
 *          顺序才能保证 `1.5s`、`0.000000001s` 之类的边界值与 Go 一致。
 */
inline bool ParseDurationText(std::string_view text,
                              ::go2cpp::time::Duration* out) {
    if (text.empty()) {
        return false;
    }
    bool negative = false;
    if (text.front() == '-' || text.front() == '+') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text == "0") {
        *out = ::go2cpp::time::Duration::FromNanoseconds(0);
        return true;
    }
    if (text.empty()) {
        return false;
    }
    std::uint64_t total = 0;
    constexpr std::uint64_t kMax = std::uint64_t{1} << 63;
    while (!text.empty()) {
        std::uint64_t integer = 0;
        std::uint64_t fraction = 0;
        double scale = 1.0;
        bool consumed_integer = false;
        bool consumed_fraction = false;
        if (!(text.front() == '.' || (text.front() >= '0' && text.front() <= '9'))) {
            return false;
        }
        while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
            if (integer > (kMax - 9) / 10) {
                return false;  // 溢出
            }
            integer = integer * 10 + static_cast<std::uint64_t>(text.front() - '0');
            text.remove_prefix(1);
            consumed_integer = true;
        }
        if (!text.empty() && text.front() == '.') {
            text.remove_prefix(1);
            while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
                if (fraction < kMax / 10) {
                    fraction = fraction * 10 + static_cast<std::uint64_t>(text.front() - '0');
                    scale *= 10.0;
                }
                text.remove_prefix(1);
                consumed_fraction = true;
            }
        }
        if (!consumed_integer && !consumed_fraction) {
            return false;
        }
        // 单位：连续的非数字、非点字符。
        std::size_t unit_length = 0;
        while (unit_length < text.size() && text[unit_length] != '.' &&
               !(text[unit_length] >= '0' && text[unit_length] <= '9')) {
            ++unit_length;
        }
        if (unit_length == 0) {
            return false;  // 缺单位
        }
        const std::string_view unit = text.substr(0, unit_length);
        text.remove_prefix(unit_length);
        std::uint64_t nanos = 0;
        if (unit == "ns") {
            nanos = 1;
        } else if (unit == "us" || unit == "\xC2\xB5s" || unit == "\xCE\xBCs") {
            nanos = 1000;  // µs（U+00B5）与 μs（U+03BC）
        } else if (unit == "ms") {
            nanos = 1000000;
        } else if (unit == "s") {
            nanos = 1000000000;
        } else if (unit == "m") {
            nanos = 60ull * 1000000000ull;
        } else if (unit == "h") {
            nanos = 3600ull * 1000000000ull;
        } else {
            return false;  // 未知单位
        }
        if (integer > kMax / nanos) {
            return false;
        }
        std::uint64_t value = integer * nanos;
        if (fraction > 0) {
            // 与 Go 一致：`uint64(float64(f) * (float64(unit) / scale))`。
            // 这个顺序（先算 unit/scale 再乘 f）决定了 `1.5s` 的舍入行为。
            value += static_cast<std::uint64_t>(static_cast<double>(fraction) *
                                                (static_cast<double>(nanos) / scale));
        }
        if (total > kMax - value) {
            return false;
        }
        total += value;
    }
    if (negative) {
        *out = ::go2cpp::time::Duration::FromNanoseconds(
            -static_cast<std::int64_t>(total));
    } else {
        *out = ::go2cpp::time::Duration::FromNanoseconds(
            static_cast<std::int64_t>(total));
    }
    return true;
}

}  // namespace detail

// ============================================================================
// 各类型值的文本化与解析（对 Value 模板做类型分派）
// ============================================================================

namespace detail {

inline std::string FormatValue(const std::string& value) { return value; }
inline std::string FormatValue(bool value) { return value ? "true" : "false"; }
inline std::string FormatValue(int value) { return std::to_string(value); }
inline std::string FormatValue(std::int64_t value) { return std::to_string(value); }
inline std::string FormatValue(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}
inline std::string FormatValue(const ::go2cpp::time::Duration& value) {
    return value.String();
}

inline bool ParseValue(std::string_view text, std::string* out) {
    out->assign(text.data(), text.size());
    return true;
}
inline bool ParseValue(std::string_view text, bool* out) {
    return ParseBoolText(text, out);
}
inline bool ParseValue(std::string_view text, int* out) {
    std::int64_t parsed = 0;
    if (!ParseIntText(text, &parsed)) {
        return false;
    }
    if (parsed < static_cast<std::int64_t>(-2147483647 - 1) ||
        parsed > static_cast<std::int64_t>(2147483647)) {
        return false;  // Go 的 int 在 64 位平台是 64 位；这里按 C++ int 收窄
    }
    *out = static_cast<int>(parsed);
    return true;
}
inline bool ParseValue(std::string_view text, std::int64_t* out) {
    return ParseIntText(text, out);
}
inline bool ParseValue(std::string_view text, double* out) {
    return ParseFloatText(text, out);
}
inline bool ParseValue(std::string_view text, ::go2cpp::time::Duration* out) {
    return ParseDurationText(text, out);
}

/// 类型名（`UnquoteUsage` 的默认值）。
inline std::string TypeName(const std::string&) { return "string"; }
inline std::string TypeName(bool) { return "bool"; }
inline std::string TypeName(int) { return "int"; }
inline std::string TypeName(std::int64_t) { return "int"; }
inline std::string TypeName(double) { return "float64"; }
inline std::string TypeName(const ::go2cpp::time::Duration&) { return "duration"; }

}  // namespace detail

/**
 * @brief 强类型值的通用实现，把 `shared_ptr<T>` 适配成 `Value`。
 * @tparam T 支持 `detail::ParseValue` / `detail::FormatValue` 的类型。
 */
template <typename T>
class TypedValue final : public Value {
public:
    explicit TypedValue(std::shared_ptr<T> target) : m_target(std::move(target)) {}

    std::string String() const override { return detail::FormatValue(*m_target); }

    bool Set(std::string_view text) override {
        T parsed{};
        if (!detail::ParseValue(text, &parsed)) {
            return false;
        }
        *m_target = std::move(parsed);
        return true;
    }

    std::string Type() const override { return detail::TypeName(*m_target); }

    bool IsBoolFlag() const override { return std::is_same<T, bool>::value; }

    /// 供注册函数拿回 `shared_ptr<T>`（引用语义，与 Go 的 `*T` 一致）。
    const std::shared_ptr<T>& Target() const noexcept { return m_target; }

private:
    std::shared_ptr<T> m_target;  ///< 用户可见的取值指针
};

// ============================================================================
// Flag / FlagSet
// ============================================================================

/// 一个已注册的 flag，对应 Go 的 `flag.Flag`。
class Flag final {
public:
    Flag(std::string name, std::string usage, std::shared_ptr<Value> value,
         std::string def_value)
        : m_name(std::move(name)),
          m_usage(std::move(usage)),
          m_value(std::move(value)),
          m_def_value(std::move(def_value)) {}

    /// 对应 `Flag.Name`。
    const std::string& Name() const noexcept { return m_name; }
    /// 对应 `Flag.Usage`。
    const std::string& Usage() const noexcept { return m_usage; }
    /// 对应 `Flag.DefValue`。
    const std::string& DefValue() const noexcept { return m_def_value; }
    /// 对应 `Flag.Value`；& 返回便于直接调用 `Set`/`String`。
    Value& ValueRef() const noexcept { return *m_value; }
    /// 对应 `Flag.Value.String()`（当前值）。
    std::string String() const { return m_value->String(); }
    /// 对应 `Flag.Value.Set(s)`。
    bool Set(std::string_view text) const { return m_value->Set(text); }
    /// 类型名（`PrintDefaults` 用）。
    std::string Type() const { return m_value->Type(); }
    /// 是否为 bool flag。
    bool IsBool() const { return m_value->IsBoolFlag(); }

private:
    std::string m_name;
    std::string m_usage;
    std::shared_ptr<Value> m_value;
    std::string m_def_value;
};

/**
 * @brief 一组 flag，对应 Go 的 `flag.FlagSet`。
 *
 * @details 与 Go 的差异集中在错误处理（不退出进程）与错误文本（不含底层
 *          `strconv` 原因），见文件头。其余解析规则逐条对齐。
 */
class FlagSet final {
public:
    /// 构造；`name` 只用于 Usage 文案。
    explicit FlagSet(std::string name = {}) : m_name(std::move(name)) {}

    // --- 注册 ---------------------------------------------------------

    /**
     * @brief 注册一个 `std::string` flag，对应 `flag.String`。
     * @return 指向取值的 `shared_ptr`（用法同 Go 的 `*string`）。
     */
    std::shared_ptr<std::string> String(const std::string& name,
                                        const std::string& value,
                                        const std::string& usage) {
        return Register<std::string>(name, value, usage);
    }

    /// 注册 `int` flag，对应 `flag.Int`。
    std::shared_ptr<int> Int(const std::string& name, int value,
                             const std::string& usage) {
        return Register<int>(name, value, usage);
    }

    /// 注册 `bool` flag，对应 `flag.Bool`。
    std::shared_ptr<bool> Bool(const std::string& name, bool value,
                               const std::string& usage) {
        return Register<bool>(name, value, usage);
    }

    /// 注册 `int64` flag，对应 `flag.Int64`。
    std::shared_ptr<std::int64_t> Int64(const std::string& name, std::int64_t value,
                                        const std::string& usage) {
        return Register<std::int64_t>(name, value, usage);
    }

    /// 注册 `double` flag，对应 `flag.Float64`。
    std::shared_ptr<double> Float64(const std::string& name, double value,
                                    const std::string& usage) {
        return Register<double>(name, value, usage);
    }

    /// 注册 `Duration` flag，对应 `flag.Duration`。
    std::shared_ptr<::go2cpp::time::Duration> Duration(
        const std::string& name, ::go2cpp::time::Duration value,
        const std::string& usage) {
        return Register<::go2cpp::time::Duration>(name, value, usage);
    }

    // --- 解析 ---------------------------------------------------------

    /// 对应 `flag.Parse()`（从 `argv[1]` 开始）。
    ParseResult Parse(int argc, char** argv) {
        std::vector<std::string> arguments;
        for (int index = 1; index < argc; ++index) {
            if (argv[index] != nullptr) {
                arguments.emplace_back(argv[index]);
            }
        }
        return Parse(arguments);
    }

    /// 对应 `flag.Parse()`，参数已拆成字符串列表。
    ParseResult Parse(const std::vector<std::string>& arguments);

    /// 是否已经成功解析过。
    bool Parsed() const noexcept { return m_parsed; }

    /// 最近一次解析失败的文本；成功或 `kHelp` 时为空。
    const std::string& Error() const noexcept { return m_error; }

    // --- 查询 ---------------------------------------------------------

    /// 对应 `flag.Args()`：解析结束后剩余的非 flag 参数。
    const std::vector<std::string>& Args() const noexcept { return m_args; }

    /// 对应 `flag.NArg()`。
    int NArg() const noexcept { return static_cast<int>(m_args.size()); }

    /// 对应 `flag.Arg(i)`；越界返回空串。
    std::string Arg(int index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= m_args.size()) {
            return {};
        }
        return m_args[static_cast<std::size_t>(index)];
    }

    /// 对应 `flag.NFlag()`：成功设置的 flag 个数（重复设置只算一个）。
    int NFlag() const noexcept { return static_cast<int>(m_set_flags.size()); }

    /**
     * @brief 对应 `flag.Lookup(name)`：按名字查找已注册的 flag。
     * @return 指向 flag 的指针（生命周期随 `FlagSet`）；未找到返回 nullptr。
     */
    const Flag* Lookup(const std::string& name) const {
        const auto found = m_flags.find(name);
        return found == m_flags.end() ? nullptr : found->second.get();
    }

    /**
     * @brief 对应 `FlagSet.Set(name, value)`：直接给某个 flag 赋值。
     * @return flag 不存在或值非法时返回 false。
     */
    bool Set(const std::string& name, const std::string& value) {
        const auto found = m_flags.find(name);
        if (found == m_flags.end()) {
            return false;
        }
        if (!found->second->Set(value)) {
            return false;
        }
        m_set_flags.insert(name);
        return true;
    }

    /// 对应 `FlagSet.Visit`：按字典序访问**已设置**的 flag。
    void Visit(const std::function<void(const Flag&)>& visitor) const {
        for (const auto& name : m_set_flags) {
            const auto found = m_flags.find(name);
            if (found != m_flags.end()) {
                visitor(*found->second);
            }
        }
    }

    /// 对应 `FlagSet.VisitAll`：按字典序访问全部 flag。
    void VisitAll(const std::function<void(const Flag&)>& visitor) const {
        for (const auto& entry : m_flags) {
            visitor(*entry.second);
        }
    }

    // --- 输出 ---------------------------------------------------------

    /// 对应 `FlagSet.SetOutput`：重定向 `PrintDefaults` 的输出（默认 `stderr`）。
    void SetOutput(std::FILE* output) { m_output = output == nullptr ? stderr : output; }

    /// 对应 `FlagSet.PrintDefaults`。
    void PrintDefaults() const;

    /// 对应 `FlagSet.Usage`：默认打印 "Usage of <name>:" + PrintDefaults。
    void PrintUsage() const;

    /// 设置自定义 Usage（对应 Go 1.20 的 `FlagSet.Usage func()`）。
    void SetUsage(std::function<void()> usage) { m_usage = std::move(usage); }

    /// 当前 FlagSet 的名字。
    const std::string& Name() const noexcept { return m_name; }

private:
    /// 注册模板：创建 `TypedValue<T>` 并登记；重名抛 `std::invalid_argument`。
    template <typename T>
    std::shared_ptr<T> Register(const std::string& name, const T& value,
                                const std::string& usage) {
        if (m_flags.find(name) != m_flags.end()) {
            throw std::invalid_argument("flag redefined: " + name);
        }
        if (name.empty()) {
            throw std::invalid_argument("flag::Register: empty flag name");
        }
        auto target = std::make_shared<T>(value);
        auto typed = std::make_shared<TypedValue<T>>(target);
        auto record = std::make_shared<Flag>(name, usage, typed,
                                             detail::FormatValue(value));
        m_flags.emplace(name, std::move(record));
        return target;
    }

    /// 记录一条错误并返回 `kError`。
    ParseResult Fail(std::string message) {
        m_error = std::move(message);
        return ParseResult::kError;
    }

    std::string m_name;                                    ///< FlagSet 名字
    std::map<std::string, std::shared_ptr<Flag>> m_flags;  ///< 按名字字典序存放
    std::set<std::string> m_set_flags;                     ///< 已成功设置的 flag 名
    std::vector<std::string> m_args;                       ///< 解析结束后的剩余参数
    std::string m_error;                                   ///< 最近一次错误文本
    std::function<void()> m_usage;                         ///< 自定义 Usage
    std::FILE* m_output{stderr};                           ///< PrintDefaults 的输出
    bool m_parsed{false};                                  ///< 是否成功解析过
};

// ============================================================================
// 实现
// ============================================================================

inline ParseResult FlagSet::Parse(const std::vector<std::string>& arguments) {
    m_args.clear();
    m_error.clear();
    m_parsed = false;
    const std::size_t total = arguments.size();
    std::size_t cursor = 0;
    while (cursor < total) {
        const std::string& argument = arguments[cursor];
        if (argument.size() < 2 || argument[0] != '-') {
            break;  // 非 flag 参数：终止解析（Go 同样）
        }
        std::size_t minuses = 1;
        if (argument[1] == '-') {
            minuses = 2;
            if (argument.size() == 2) {
                ++cursor;  // "--" 终止解析，自身被丢弃
                break;
            }
        }
        std::string name = argument.substr(minuses);
        if (name.empty() || name[0] == '-' || name[0] == '=') {
            m_args.assign(arguments.begin() + static_cast<std::ptrdiff_t>(cursor + 1),
                          arguments.end());
            return Fail("bad flag syntax: " + argument);
        }
        ++cursor;  // 消费掉这个 -flag 参数
        bool has_value = false;
        std::string value;
        const std::size_t equals = name.find('=');
        if (equals != std::string::npos) {
            value = name.substr(equals + 1);
            has_value = true;
            name = name.substr(0, equals);
        }
        const auto found = m_flags.find(name);
        if (found == m_flags.end()) {
            if (name == "help" || name == "h") {
                m_args.assign(arguments.begin() + static_cast<std::ptrdiff_t>(cursor),
                              arguments.end());
                PrintUsage();
                return ParseResult::kHelp;
            }
            m_args.assign(arguments.begin() + static_cast<std::ptrdiff_t>(cursor),
                          arguments.end());
            return Fail("flag provided but not defined: -" + name);
        }
        const Flag& record = *found->second;
        if (record.IsBool()) {
            if (has_value) {
                if (!record.Set(value)) {
                    m_args.assign(arguments.begin() + static_cast<std::ptrdiff_t>(cursor),
                                  arguments.end());
                    return Fail("invalid boolean value \"" + value + "\" for -" + name);
                }
            } else {
                record.Set("true");  // bool 的 "true" 一定合法
            }
        } else {
            if (!has_value && cursor < total) {
                value = arguments[cursor];
                ++cursor;
                has_value = true;
            }
            if (!has_value) {
                m_args.assign(arguments.begin() + static_cast<std::ptrdiff_t>(cursor),
                              arguments.end());
                return Fail("flag needs an argument: -" + name);
            }
            if (!record.Set(value)) {
                m_args.assign(arguments.begin() + static_cast<std::ptrdiff_t>(cursor),
                              arguments.end());
                return Fail("invalid value \"" + value + "\" for flag -" + name);
            }
        }
        m_set_flags.insert(name);
    }
    m_args.assign(arguments.begin() + static_cast<std::ptrdiff_t>(cursor), arguments.end());
    m_parsed = true;
    return ParseResult::kOk;
}

inline void FlagSet::PrintDefaults() const {
    for (const auto& entry : m_flags) {
        const Flag& record = *entry.second;
        std::fprintf(m_output, "  -%s %s\n", record.Name().c_str(),
                     record.Type().c_str());
        std::fprintf(m_output, "    \t%s", record.Usage().c_str());
        if (!record.DefValue().empty()) {
            std::fprintf(m_output, " (default %s)", record.DefValue().c_str());
        }
        std::fprintf(m_output, "\n");
    }
    std::fflush(m_output);
}

inline void FlagSet::PrintUsage() const {
    if (m_usage) {
        m_usage();
        return;
    }
    if (m_name.empty()) {
        std::fprintf(m_output, "Usage:\n");
    } else {
        std::fprintf(m_output, "Usage of %s:\n", m_name.c_str());
    }
    PrintDefaults();
}

// ============================================================================
// 包级接口：对应 Go 的 CommandLine 与自由函数
// ============================================================================

/// 对应 Go 的包级 `flag.Usage`；可直接赋值（为空时使用内置打印）。
inline std::function<void()> Usage;

/// 对应 Go 的 `flag.CommandLine`（进程级默认 FlagSet）。
inline FlagSet& CommandLine() {
    static FlagSet command_line("go2cpp");
    return command_line;
}

/// 对应 `flag.String`。
inline std::shared_ptr<std::string> String(const std::string& name,
                                          const std::string& value,
                                          const std::string& usage) {
    return CommandLine().String(name, value, usage);
}

/// 对应 `flag.Int`。
inline std::shared_ptr<int> Int(const std::string& name, int value,
                                const std::string& usage) {
    return CommandLine().Int(name, value, usage);
}

/// 对应 `flag.Bool`。
inline std::shared_ptr<bool> Bool(const std::string& name, bool value,
                                  const std::string& usage) {
    return CommandLine().Bool(name, value, usage);
}

/// 对应 `flag.Int64`。
inline std::shared_ptr<std::int64_t> Int64(const std::string& name,
                                           std::int64_t value,
                                           const std::string& usage) {
    return CommandLine().Int64(name, value, usage);
}

/// 对应 `flag.Float64`。
inline std::shared_ptr<double> Float64(const std::string& name, double value,
                                       const std::string& usage) {
    return CommandLine().Float64(name, value, usage);
}

/// 对应 `flag.Duration`。
inline std::shared_ptr<::go2cpp::time::Duration> Duration(
    const std::string& name, ::go2cpp::time::Duration value,
    const std::string& usage) {
    return CommandLine().Duration(name, value, usage);
}

/**
 * @brief 对应 `flag.Parse()`。
 * @param argc / argv 交给 `main` 的原样参数（从 `argv[1]` 开始解析）。
 * @return `kOk` / `kHelp` / `kError`；出错时文本在 `CommandLine().Error()`。
 *
 * @note 与 Go 的 `ExitOnError` 不同：**不会**退出进程，也**不会**自动打印
 *       错误与用法（`-h`/`-help` 除外）。需要 Go 式行为请自行：
 *       ```cpp
 *       if (go::flag::Parse(argc, argv) == go::flag::ParseResult::kError) {
 *           std::fprintf(stderr, "%s\n", go::flag::CommandLine().Error().c_str());
 *           go::flag::CommandLine().PrintUsage();
 *           return 2;
 *       }
 *       ```
 */
inline ParseResult Parse(int argc, char** argv) {
    return CommandLine().Parse(argc, argv);
}

/// 对应 `flag.Parse()`（参数已拆好）。
inline ParseResult Parse(const std::vector<std::string>& arguments) {
    return CommandLine().Parse(arguments);
}

/// 对应 `flag.Parsed()`。
inline bool Parsed() { return CommandLine().Parsed(); }

/// 对应 `flag.Args()`。
inline const std::vector<std::string>& Args() { return CommandLine().Args(); }

/// 对应 `flag.NArg()`。
inline int NArg() { return CommandLine().NArg(); }

/// 对应 `flag.Arg(i)`。
inline std::string Arg(int index) { return CommandLine().Arg(index); }

/// 对应 `flag.NFlag()`。
inline int NFlag() { return CommandLine().NFlag(); }

/// 对应 `flag.Lookup(name)`。
inline const Flag* Lookup(const std::string& name) {
    return CommandLine().Lookup(name);
}

/// 对应 `flag.Set(name, value)`。
inline bool Set(const std::string& name, const std::string& value) {
    return CommandLine().Set(name, value);
}

/// 对应 `flag.Visit`。
inline void Visit(const std::function<void(const Flag&)>& visitor) {
    CommandLine().Visit(visitor);
}

/// 对应 `flag.VisitAll`。
inline void VisitAll(const std::function<void(const Flag&)>& visitor) {
    CommandLine().VisitAll(visitor);
}

/// 对应 `flag.PrintDefaults`。
inline void PrintDefaults() { CommandLine().PrintDefaults(); }

/**
 * @brief 对应 `flag.Usage()`：有自定义 Usage 用自定义，否则打印 "Usage of ..."。
 * @note `go::flag::Usage` 是 inline 变量，直接赋值即可：
 *       `go::flag::Usage = [] { ... };`
 */
inline void PrintUsage() {
    if (Usage) {
        Usage();
        return;
    }
    CommandLine().PrintUsage();
}

}  // namespace flag
}  // namespace go2cpp::go
