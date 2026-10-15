/**
 * @file time.hpp
 * @brief Go `time` 包核心子集的 C++ 对应实现（语义对齐）。
 *
 * @details 复用优先的结论：C++ 侧有 `std::chrono`，但语义与 Go 的 `time` 不匹配：
 *   - Go 的 `Duration` 是**强类型 int64 纳秒**且带 `String()` 的特定格式（`1h2m3.5s` / `1.5ms` / `300ns`）；
 *   - Go 的 `Time` 带**单调时钟**读数（`monotonic` 字段），`Sub` 优先用单调读数；
 *   - `time.Sleep` 挂起的是 **goroutine**，不是线程。
 *   因此本模块自研，但**复用项目已有的两个能力**：
 *   ① 时钟用 `std::chrono`（`system_clock` 取墙钟 + `steady_clock` 取单调读数）；
 *   ② `Sleep` **直接调用 `::nanosleep`** —— 项目的 Hook 已拦截 `sleep`/`usleep`/`nanosleep`
 *      （见 `src/network/hook.cpp` 的 `cooperative_sleep_for`：临时 `eventfd` + `manager->WaitFor`），
 *      在 Fiber 上下文中会变成**协作式等待**，不会占住 M。这与 Go「挂起当前 goroutine」的语义一致。
 *
 *   语义对照来源：本机 `third_party/go1.23.0.src.tar.gz` 内的 `go/src/time/`（Go 1.23.0，BSD-3-Clause），
 *   **独立实现**。`Duration.String()` 的格式算法逐分支对照了上游 `time.go` 的 `Duration.format`
 *   （`u < Second` 时按 ns/µs/ms 选单位与精度 `prec = 0/3/6`，`u == 0` → `"0s"`；
 *   否则秒带 9 位小数并去掉尾随零，再补 `m`/`h`）。
 *
 * @note **边界**（相对 Go 的 `time`）：
 *       - `Sleep` 在**非 Hook / 非 Fiber** 上下文中会退化为真实线程睡眠（Go 不会）；
 *       - `Time.String()` 采用简化的 `"YYYY-MM-DD HH:MM:SS[.fffffffff] +0000"` 形式，
 *         **不实现** Go 的布局串（`2006-01-02 15:04:05`）、时区名（MST）与 RFC3339 家族；
 *       - `After`/`Tick`/`Ticker`/`Timer`（需要与 `Channel<Time>` 和 `core::TimerService` 对接）
 *         本轮未实现，见文件末尾的待办说明；
 *       - 未实现 `Parse`/`Format`/`LoadLocation`/`Duration.Parse`。
 */

#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace go2cpp::time {

// ============================================================================
// Duration（对应 time.Duration：int64 纳秒）
// ============================================================================

/**
 * @brief 对应 `time.Duration`。
 * @details 单位与 Go 一致：底层是 **int64 纳秒**；`String()` 输出 Go 的紧凑形式。
 */
class Duration final {
public:
    constexpr Duration() noexcept = default;

    /// 从纳秒构造（对应 Go 的 `time.Duration(n)` 转换）。
    static constexpr Duration FromNanoseconds(std::int64_t nanoseconds) noexcept {
        return Duration(nanoseconds);
    }

    /// 从秒构造（浮点，对应 `time.Duration(seconds * float64(time.Second))`）。
    static constexpr Duration FromSeconds(double seconds) noexcept {
        return Duration(static_cast<std::int64_t>(seconds * 1e9));
    }

    /// 对应 `Duration.Nanoseconds()`。
    constexpr std::int64_t Nanoseconds() const noexcept { return m_nanoseconds; }
    /// 对应 `Duration.Microseconds()`。
    constexpr std::int64_t Microseconds() const noexcept { return m_nanoseconds / 1000; }
    /// 对应 `Duration.Milliseconds()`。
    constexpr std::int64_t Milliseconds() const noexcept { return m_nanoseconds / 1000000; }
    /// 对应 `Duration.Seconds()`（浮点，返回秒）。
    constexpr double Seconds() const noexcept {
        return static_cast<double>(m_nanoseconds) / 1e9;
    }
    /// 对应 `Duration.Minutes()`。
    constexpr double Minutes() const noexcept {
        return static_cast<double>(m_nanoseconds) / 6e10;
    }
    /// 对应 `Duration.Hours()`。
    constexpr double Hours() const noexcept {
        return static_cast<double>(m_nanoseconds) / 3.6e12;
    }

    /// 对应 `Duration.Abs()`。
    constexpr Duration Abs() const noexcept {
        return m_nanoseconds < 0 ? Duration(-m_nanoseconds) : Duration(m_nanoseconds);
    }

    /// 对应 `Duration.Truncate(m)`：向零截断到 `m` 的整数倍。
    constexpr Duration Truncate(Duration unit) const noexcept {
        if (unit.m_nanoseconds == 0) return *this;
        return Duration(m_nanoseconds - m_nanoseconds % unit.m_nanoseconds);
    }

    /// 对应 `Duration.Round(m)`：四舍五入到 `m` 的整数倍（半值远离零）。
    constexpr Duration Round(Duration unit) const noexcept {
        if (unit.m_nanoseconds == 0) return *this;
        const std::int64_t remainder = m_nanoseconds % unit.m_nanoseconds;
        if (remainder == 0) return *this;
        const std::int64_t half = unit.m_nanoseconds / 2;
        Duration truncated(m_nanoseconds - remainder);
        if (m_nanoseconds < 0) {
            if (-remainder >= half) truncated = truncated - unit;
        } else {
            if (remainder >= half) truncated = truncated + unit;
        }
        return truncated;
    }

    /// 对应 `Duration.String()`（格式算法逐分支对照 Go 的 `Duration.format`）。
    std::string String() const {
        constexpr std::int64_t kSecond = 1000000000LL;
        constexpr std::int64_t kMillisecond = 1000000LL;
        constexpr std::int64_t kMicrosecond = 1000LL;

        std::uint64_t value = static_cast<std::uint64_t>(m_nanoseconds);
        const bool negative = m_nanoseconds < 0;
        if (negative) value = static_cast<std::uint64_t>(-value);

        std::string digits;                     // 逆序累积，最后翻转
        std::string suffix;

        if (value < static_cast<std::uint64_t>(kSecond)) {
            int precision = 0;
            if (value == 0) return "0s";
            if (value < static_cast<std::uint64_t>(kMicrosecond)) {
                precision = 0;
                suffix = "ns";
            } else if (value < static_cast<std::uint64_t>(kMillisecond)) {
                precision = 3;
                suffix = "\xC2\xB5s";           // Go 用 U+00B5 'µ'（两字节 UTF-8）
            } else {
                precision = 6;
                suffix = "ms";
            }
            value = AppendFraction(digits, value, precision);
            AppendInteger(digits, value);      // Go 在 fmtFrac 之后还会调用 fmtInt 补整数部分
        } else {
            suffix = "s";
            value = AppendFraction(digits, value, 9);       // 秒带最多 9 位小数
            AppendInteger(digits, value % 60);              // 整数秒
            value /= 60;
            if (value > 0) {
                digits.push_back('m');
                AppendInteger(digits, value % 60);
                value /= 60;
                if (value > 0) {
                    digits.push_back('h');
                    AppendInteger(digits, value);
                }
            }
        }

        std::string out;
        if (negative) out.push_back('-');
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) out.push_back(*it);
        out += suffix;
        return out;
    }

    // ---- 运算符（与 Go 一致：Duration 之间可加减乘除、可与整数比较） ----
    constexpr Duration operator+(Duration other) const noexcept {
        return Duration(m_nanoseconds + other.m_nanoseconds);
    }
    constexpr Duration operator-(Duration other) const noexcept {
        return Duration(m_nanoseconds - other.m_nanoseconds);
    }
    constexpr Duration operator*(std::int64_t factor) const noexcept {
        return Duration(m_nanoseconds * factor);
    }
    constexpr Duration operator/(std::int64_t divisor) const noexcept {
        return Duration(m_nanoseconds / divisor);
    }
    constexpr double operator/(Duration other) const noexcept {
        return static_cast<double>(m_nanoseconds) / static_cast<double>(other.m_nanoseconds);
    }
    constexpr Duration operator-() const noexcept { return Duration(-m_nanoseconds); }
    constexpr bool operator==(Duration other) const noexcept {
        return m_nanoseconds == other.m_nanoseconds;
    }
    constexpr bool operator!=(Duration other) const noexcept {
        return m_nanoseconds != other.m_nanoseconds;
    }
    constexpr bool operator<(Duration other) const noexcept {
        return m_nanoseconds < other.m_nanoseconds;
    }
    constexpr bool operator<=(Duration other) const noexcept {
        return m_nanoseconds <= other.m_nanoseconds;
    }
    constexpr bool operator>(Duration other) const noexcept {
        return m_nanoseconds > other.m_nanoseconds;
    }
    constexpr bool operator>=(Duration other) const noexcept {
        return m_nanoseconds >= other.m_nanoseconds;
    }

    constexpr explicit Duration(std::int64_t nanoseconds) noexcept : m_nanoseconds(nanoseconds) {}

private:
    /// 把 `value / 10^precision` 的小数部分（去掉尾随零、必要时含小数点）追加到 `digits`，
    /// 返回剩下的小数位（即整数部分）。对应 Go 的 `fmtFrac`。
    static std::uint64_t AppendFraction(std::string& digits, std::uint64_t value, int precision) {
        bool printing = false;
        for (int i = 0; i < precision; ++i) {
            const std::uint64_t digit = value % 10;
            printing = printing || digit != 0;
            if (printing) digits.push_back(static_cast<char>('0' + digit));
            value /= 10;
        }
        if (printing) digits.push_back('.');
        return value;
    }

    /// 把十进制整数追加到 `digits`（逆序）。对应 Go 的 `fmtInt`。
    static void AppendInteger(std::string& digits, std::uint64_t value) {
        if (value == 0) {
            digits.push_back('0');
            return;
        }
        while (value > 0) {
            digits.push_back(static_cast<char>('0' + value % 10));
            value /= 10;
        }
    }

    std::int64_t m_nanoseconds{0};
};

/// 对应 Go 的 `2 * time.Second` 写法（Go 允许整数在左、Duration 在右）。
constexpr Duration operator*(std::int64_t factor, Duration duration) noexcept {
    return duration * factor;
}

// ---- 单位常量（对应 time.Nanosecond ~ time.Hour） ----
inline constexpr Duration Nanosecond{std::int64_t{1}};
inline constexpr Duration Microsecond{std::int64_t{1000}};
inline constexpr Duration Millisecond{std::int64_t{1000000}};
inline constexpr Duration Second{std::int64_t{1000000000}};
inline constexpr Duration Minute{std::int64_t{60} * 1000000000LL};
inline constexpr Duration Hour{std::int64_t{3600} * 1000000000LL};

// ============================================================================
// Time（对应 time.Time）
// ============================================================================

/**
 * @brief 对应 `time.Time`。
 * @details 同时保存**墙钟**（`system_clock`，用于 `Unix*` 与显示）与**单调读数**
 *          （`steady_clock`，用于 `Sub`/`Since`）—— 与 Go 的 `Time` 同时带 wall/ext 字段同理，
 *          这样系统时钟被调整时 `Since` 仍然单调。
 * @note `IsZero()` 对应 Go 的零值判断（Go 的零 Time 是 1 年 1 月 1 日 00:00:00 UTC）。
 */
class Time final {
public:
    using SystemClock = std::chrono::system_clock;
    using SteadyClock = std::chrono::steady_clock;

    constexpr Time() noexcept = default;

    /// 对应 `time.Now()`：同时取墙钟与单调读数。
    static Time Now() {
        return Time(SystemClock::now(), SteadyClock::now());
    }

    /// 对应 `time.Unix(sec, nsec)`。
    static Time FromUnix(std::int64_t seconds, std::int64_t nanoseconds = 0) {
        const auto point = SystemClock::time_point(std::chrono::seconds(seconds) +
                                                   std::chrono::nanoseconds(nanoseconds));
        return Time(point, {});
    }

    /// 从 `system_clock` 时间点构造（无单调读数）。
    static Time FromSystemClock(SystemClock::time_point point) { return Time(point, {}); }

    /// 对应 `Time.Unix()`（秒）。
    std::int64_t Unix() const {
        return std::chrono::duration_cast<std::chrono::seconds>(m_wall.time_since_epoch()).count();
    }
    /// 对应 `Time.UnixMilli()`。
    std::int64_t UnixMilli() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(m_wall.time_since_epoch())
            .count();
    }
    /// 对应 `Time.UnixMicro()`。
    std::int64_t UnixMicro() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(m_wall.time_since_epoch())
            .count();
    }
    /// 对应 `Time.UnixNano()`。
    std::int64_t UnixNano() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(m_wall.time_since_epoch())
            .count();
    }

    /// 对应 `Time.Add(d)`（保留单调读数）。
    Time Add(Duration delta) const {
        const auto wall = m_wall + std::chrono::nanoseconds(delta.Nanoseconds());
        // 单调读数只在"非零 Time"时才有意义；零值 Time 的 Add 结果仍无单调读数。
        if (!m_has_monotonic) return Time(wall, {});
        return Time(wall, m_monotonic + std::chrono::nanoseconds(delta.Nanoseconds()));
    }

    /// 对应 `Time.Sub(u)`：优先用单调读数（两者都有时），否则回落到墙钟。
    Duration Sub(const Time& other) const {
        if (m_has_monotonic && other.m_has_monotonic) {
            const auto delta = m_monotonic - other.m_monotonic;
            return Duration(std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count());
        }
        const auto delta = m_wall - other.m_wall;
        return Duration(std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count());
    }

    /// 对应 `Time.Before(u)`。
    bool Before(const Time& other) const {
        if (m_has_monotonic && other.m_has_monotonic) return m_monotonic < other.m_monotonic;
        return m_wall < other.m_wall;
    }
    /// 对应 `Time.After(u)`。
    bool After(const Time& other) const {
        if (m_has_monotonic && other.m_has_monotonic) return m_monotonic > other.m_monotonic;
        return m_wall > other.m_wall;
    }
    /// 对应 `Time.Equal(u)`（只用墙钟，与 Go 一致：单调读数不参与 Equal）。
    bool Equal(const Time& other) const { return m_wall == other.m_wall; }
    /// 对应 `Time.IsZero()`（Go 的零 Time 是 0001-01-01T00:00:00Z）。
    bool IsZero() const noexcept { return m_is_zero; }

    /// 返回底层 `system_clock` 时间点（便于与 C++ 生态互操作）。
    SystemClock::time_point ToSystemClock() const noexcept { return m_wall; }

    /// 对应 `Time.String()` 的**简化形式**（见文件头边界说明）。
    std::string String() const {
        const std::time_t seconds = static_cast<std::time_t>(Unix());
        const std::int64_t nanos = UnixNano() - static_cast<std::int64_t>(seconds) * 1000000000LL;
        std::tm broken{};
        gmtime_r(&seconds, &broken);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", broken.tm_year + 1900,
                      broken.tm_mon + 1, broken.tm_mday, broken.tm_hour, broken.tm_min, broken.tm_sec);
        std::string out(buffer);
        if (nanos != 0) {
            char fraction[24];
            std::snprintf(fraction, sizeof(fraction), ".%09lld", static_cast<long long>(nanos));
            std::string digits(fraction);
            while (!digits.empty() && digits.back() == '0') digits.pop_back();
            if (digits == ".") digits.clear();
            out += digits;
        }
        out += " +0000";
        return out;
    }

    bool operator==(const Time& other) const { return Equal(other); }
    bool operator!=(const Time& other) const { return !Equal(other); }
    bool operator<(const Time& other) const { return Before(other); }
    bool operator<=(const Time& other) const { return Before(other) || Equal(other); }
    bool operator>(const Time& other) const { return After(other); }
    bool operator>=(const Time& other) const { return After(other) || Equal(other); }

private:
    Time(SystemClock::time_point wall, SteadyClock::time_point monotonic) noexcept
        : m_wall(wall),
          m_monotonic(monotonic),
          m_has_monotonic(monotonic != SteadyClock::time_point{}),
          m_is_zero(false) {}

    SystemClock::time_point m_wall{};
    SteadyClock::time_point m_monotonic{};
    bool m_has_monotonic{false};
    /// Go 的零 Time 是「1 年 1 月 1 日 00:00:00 UTC」，与 Unix 0 不同，故用显式标记。
    bool m_is_zero{true};
};

/// 对应 `time.Since(t)`。
inline Duration Since(const Time& start) { return Time::Now().Sub(start); }

/// 对应 `time.Until(t)`。
inline Duration Until(const Time& deadline) { return deadline.Sub(Time::Now()); }

// ============================================================================
// Sleep（挂起当前 goroutine，而不是阻塞线程）
// ============================================================================

/**
 * @brief 对应 `time.Sleep(d)`：`d <= 0` 立即返回。
 * @details 实现直接调用 `::nanosleep`。**项目的 Hook 已拦截 `sleep`/`usleep`/`nanosleep`**
 *          （`src/network/hook.cpp` 的 `cooperative_sleep_for`：临时 `eventfd` + `manager->WaitFor`），
 *          因此当 Hook 启用且当前执行流绑定在 `IOManager` 上时，这里是**协作式等待**：
 *          M 会被让给其他可运行的 G，而不是被睡住。
 * @note 在**未启用 Hook**（或纯线程上下文）时，它会退化为真实的线程睡眠 —— 这是与 Go 的差异，
 *       已写入文件头边界；跨平台方面 `nanosleep` 是 POSIX 接口（本项目目标平台为 Linux）。
 */
inline void Sleep(Duration duration) {
    if (duration <= Duration{}) return;                 // Go：非正时长立即返回
    const std::int64_t total = duration.Nanoseconds();
    timespec request{};
    request.tv_sec = static_cast<std::time_t>(total / 1000000000LL);
    request.tv_nsec = static_cast<long>(total % 1000000000LL);
    timespec remaining{};
    while (::nanosleep(&request, &remaining) == -1 && errno == EINTR) {
        request = remaining;                            // 被信号打断则继续睡剩余时间
    }
}

}  // namespace go2cpp::time

/**
 * @brief 本轮未实现、已明确列入待办的 Go `time` 成员。
 *
 * | 未实现 | 原因 / 前置 |
 * |---|---|
 * | `After(d)` / `Tick(d)` / `NewTicker` / `NewTimer` | 需要与 `Channel<Time>`（`go2cpp::Channel`）和 `core::TimerService` 对接；接口形态要先定「返回的通道由谁持有」 |
 * | `Format` / `Parse` | Go 的布局串（`2006-01-02`）是一套独立小语言，且与 `Time.String()` 的完整形式绑定 |
 * | `LoadLocation` / `FixedZone` | 依赖 tzdata 与 Go 的时区数据库语义 |
 * | `Duration.Parse` | 需要实现 Go 的 `ParseDuration` 文法（`1h2m3.5s`、`-1.5s`、`1µs`） |
 * | 单调读数的完整语义 | 当前 `Time` 已保存单调读数并用于 `Sub`/`Since`，但未实现 Go 的 wall/ext 剥离规则 |
 */
