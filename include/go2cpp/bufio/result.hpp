/**
 * @file result.hpp
 * @brief `bufio` 模块的「值 + 错误」返回类型。
 *
 * @details **为什么不用异常**：Go 的 `bufio` 全部用 `(value, error)` 报错，
 *   项目其余模块（`streams`、`os`）也统一返回 `ErrorPtr`。异常在这里既
 *   无法表达 Go 的「数据 + 错误同时返回」契约（例如 `ReadString` 在 EOF 前
 *   读到的半行），也会让 `Writer` 的析构/刷新路径复杂化。
 *
 *   **为什么允许「带值失败」**：Go 的 `Peek` 在数据不足时会同时返回
 *   「已有字节」和 `io.EOF`；`ReadString` 同理。`Result<T>(value, error)`
 *   就是给这两个 API 用的，其余路径只构造成功或只构造失败。
 *
 *   **错误判空**：`ok()` 为真表示没有错误。`Value()` 在失败时返回
 *   默认构造的值（不会抛异常、不会崩溃），调用方应先看 `ok()`。
 */
#pragma once

#include "go2cpp/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace go2cpp::bufio {

/**
 * @brief 携带一个值或一条错误的返回包装。
 * @tparam T 值类型；必须可默认构造（`ByteSpan`、`std::string`、整数均满足）。
 */
template <typename T>
class Result {
public:
    /// 成功：只有值。
    Result(T value) : m_value(std::move(value)) {}
    /// 失败：只有错误。
    Result(ErrorPtr error) : m_error(std::move(error)) {}
    /// 部分成功：既有值也有错误（对应 Go 的 `n > 0, err != nil`）。
    Result(T value, ErrorPtr error)
        : m_value(std::move(value)), m_error(std::move(error)) {}

    /// 没有错误即成功。
    bool ok() const noexcept { return !m_error; }
    explicit operator bool() const noexcept { return ok(); }

    /// 错误（可能为空）。引用在 `Result` 存活期内有效。
    const ErrorPtr& Err() const noexcept { return m_error; }

    /// 读取值；失败时是默认构造值，调用方须先判断 `ok()`。
    T& Value() & noexcept { return m_value; }
    const T& Value() const& noexcept { return m_value; }
    T&& Value() && noexcept { return std::move(m_value); }

    /// 失败时的兜底取值，省掉调用方的 if。
    T ValueOr(T fallback) const& {
        return m_error ? std::move(fallback) : m_value;
    }

private:
    T m_value{};
    ErrorPtr m_error;
};

/**
 * @brief `Result<void>`：只表达成功/失败。
 * @details 显式特化而不是用 `Result<std::monostate>`，避免在
 *          `Writer::WriteByte`、`Flush` 这类「只关心错误」的 API 上
 *          引入无意义的值。
 */
template <>
class Result<void> {
public:
    /// 成功。
    Result() = default;
    /// 失败。
    Result(ErrorPtr error) : m_error(std::move(error)) {}

    /// 没有错误即成功。
    bool ok() const noexcept { return !m_error; }
    explicit operator bool() const noexcept { return ok(); }

    /// 错误（可能为空）。
    const ErrorPtr& Err() const noexcept { return m_error; }

private:
    ErrorPtr m_error;
};

}  // namespace go2cpp::bufio
