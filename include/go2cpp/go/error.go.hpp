/**
 * @file error.go.hpp
 * @brief Go `error` / `errors` 包的 C++ 对应物（Go 风格薄封装）。
 *
 * ## 定位
 * 本文件属于 `go2cpp/go/` 目录的「Go 接口对接层」。它**不重新实现**错误值，
 * 而是把已有的 `go2cpp::Error` / `go2cpp::errors` 按 Go 的书写习惯暴露：
 *
 * ```go
 * var ErrClosed = errors.New("use of closed network connection")
 * err := fmt.Errorf("read frame: %w", err)
 * if errors.Is(err, context.Canceled) { ... }
 * var target *net.OpError
 * if errors.As(err, &target) { ... }
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * go::error err = go::New("use of closed network connection");
 * err = go::Wrap(err, "read frame");
 * if (go::Is(err, go::ErrClosed())) { ... }
 * auto target = go::As<MyError>(err);
 * ```
 *
 * ## 语义边界（必须知道）
 * - Go 的 `error` 是接口值，C++ 用 `go2cpp::ErrorPtr`（`shared_ptr<const Error>`）
 *   表达；零值 `nullptr` 等价于 Go 的 `nil`。
 * - `errors.Is` 按错误图遍历做**身份/自定义 Is** 匹配，不做文本比较；与 Go 一致。
 * - `errors.As` 需要目标类型继承 `go2cpp::Error`；返回 `shared_ptr<const T>`。
 * - Go 的 `errors.Join`（Go 1.20）对应 `go2cpp::Join`，可聚合多条原因。
 * - Go 的 `fmt.Errorf("%w")` 对应 `Wrap`；`%v` 对应 `New`。
 *
 * @note 本文件只依赖 `go2cpp/tools/error.hpp`，可被 net/io/context 等复用。
 */

#pragma once

#include "go2cpp/tools/error.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::go {

/** @brief 对应 Go 的 `error` 接口值；空指针即 `nil`。 */
using error = ErrorPtr;

/** @brief 对应 `errors.New`（文本错误）。 */
inline error New(std::string message) {
    return NewError(std::move(message));
}

/** @brief 对应 `fmt.Errorf("...: %w", cause)`，不带格式化。 */
inline error Wrap(error cause, std::string message) {
    return go2cpp::Wrap(std::move(cause), std::move(message));
}

/** @brief 对应 `errors.Join`（多条错误的聚合）。 */
inline error Join(std::vector<error> causes) {
    return go2cpp::Join(std::move(causes));
}

/** @brief 对应 `errors.Join` 的初始化列表形式。 */
inline error Join(std::initializer_list<error> causes) {
    return go2cpp::Join(causes);
}

/** @brief 对应 `errors.Unwrap`；只解一层。 */
inline error Unwrap(const error& err) noexcept {
    return go2cpp::Unwrap(err);
}

/** @brief 对应 `errors.Is`；空 target 只匹配 nil。 */
inline bool Is(const error& err, const error& target) noexcept {
    return go2cpp::Is(err, target);
}

/** @brief 对应 `errors.As`；目标类型必须继承 `go2cpp::Error`。 */
template <typename T>
inline std::shared_ptr<const T> As(const error& err) noexcept {
    return go2cpp::As<T>(err);
}

/** @brief 对应 `err.Error()`；nil 返回空串（Go 中 nil.Error() 会 panic，此处更安全）。 */
inline std::string ErrorString(const error& err) {
    return ErrorMessage(err);
}

/** @brief 对应 `err != nil`。 */
inline bool HasError(const error& err) noexcept {
    return static_cast<bool>(err);
}

// ============================================================================
// 哨兵错误（对应 Go 的 var ErrXxx = errors.New(...)）
// ============================================================================

namespace detail {

/**
 * @brief 带「同类即相等」语义的哨兵错误。
 * @details `errors.Is` 对哨兵通常比较身份；但本项目的 streams/network 层
 *          会重复构造 `"EOF"` 之类文本错误，因此哨兵额外实现
 *          「同类 + 文本相同」即匹配，避免调用方被迫持有唯一实例。
 *          这是对 Go 身份的**兼容扩展**，不会让不同语义的错误互相匹配。
 */
class SentinelError final : public Error {
public:
    explicit SentinelError(std::string message) : m_message(std::move(message)) {}

    std::string Message() const override { return m_message; }

    bool Is(const Error& target) const noexcept override {
        if (this == &target) {
            return true;
        }
        const auto* other = dynamic_cast<const SentinelError*>(&target);
        return other != nullptr && other->m_message == m_message;
    }

private:
    std::string m_message;
};

inline error MakeSentinel(const char* message) {
    return std::make_shared<SentinelError>(message);
}

}  // namespace detail

/** @brief 对应 `io.EOF`。 */
inline const error& ErrEOF() {
    static const error kValue = detail::MakeSentinel("EOF");
    return kValue;
}

/** @brief 对应 `io.ErrUnexpectedEOF`。 */
inline const error& ErrUnexpectedEOF() {
    static const error kValue = detail::MakeSentinel("unexpected EOF");
    return kValue;
}

/** @brief 对应 `io.ErrShortWrite`。 */
inline const error& ErrShortWrite() {
    static const error kValue = detail::MakeSentinel("short write");
    return kValue;
}

/** @brief 对应 `io.ErrNoProgress`。 */
inline const error& ErrNoProgress() {
    static const error kValue = detail::MakeSentinel("multiple Read calls return no data or error");
    return kValue;
}

/** @brief 对应 `os.ErrClosed` / `net.ErrClosed`。 */
inline const error& ErrClosed() {
    static const error kValue = detail::MakeSentinel("use of closed network connection");
    return kValue;
}

/** @brief 对应 `os.ErrDeadlineExceeded`。 */
inline const error& ErrDeadlineExceeded() {
    static const error kValue = detail::MakeSentinel("i/o timeout");
    return kValue;
}

/** @brief 超时错误的别名，语义同 `ErrDeadlineExceeded`。 */
inline const error& ErrTimeout() {
    return ErrDeadlineExceeded();
}

// ============================================================================
// (T, error) 结果
// ============================================================================

/**
 * @brief 对应 Go 的多返回值 `(T, error)`。
 * @details 只用于**值类型**结果；void 结果请直接用 `error`（nil 表示成功）。
 *          默认构造的 `Result<T>` 是 `value{}` + `err = nullptr`，即「零值 + 无错误」，
 *          与 Go 的 `var v T; var err error` 一致。
 */
template <typename T>
struct Result {
    T value{};
    error err;

    /** @brief 是否成功（err == nil）。 */
    bool Ok() const noexcept { return !err; }
    explicit operator bool() const noexcept { return Ok(); }
    /** @brief 成功时取值；失败时返回零值。 */
    const T& Value() const noexcept { return value; }
    T& Value() noexcept { return value; }
};

/** @brief 无返回值的 `(error)` 结果，便于统一书写。 */
struct Status {
    error err;

    bool Ok() const noexcept { return !err; }
    explicit operator bool() const noexcept { return Ok(); }
};

/** @brief 构造成功结果。 */
template <typename T>
inline Result<T> Ok(T value) {
    Result<T> result;
    result.value = std::move(value);
    return result;
}

/** @brief 构造失败结果。 */
template <typename T>
inline Result<T> Fail(error err) {
    Result<T> result;
    result.err = std::move(err);
    return result;
}

/** @brief 构造只有错误的失败状态。 */
inline Status Fail(error err) {
    Status status;
    status.err = std::move(err);
    return status;
}

/** @brief 构造成功状态。 */
inline Status Ok() {
    return Status{};
}

}  // namespace go2cpp::go
