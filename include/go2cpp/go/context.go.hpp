/**
 * @file context.go.hpp
 * @brief Go `context` 包的 C++ 对应物（Background/TODO/WithCancel/WithTimeout）。
 *
 * ## 定位
 * `go2cpp::Context` 已经是 Go `context.Context` 的语义对应物（取消树、Done
 * 信号、Err/Cause、Deadline、Value），本文件只补**书写层**：
 *
 * ```go
 * ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
 * defer cancel()
 * select {
 * case <-ctx.Done():
 *     return ctx.Err()
 * case <-work:
 * }
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * auto ctx = go::WithTimeout(go::Background(), 10 * go::Second);
 * // ctx.context 是 ContextPtr，ctx.cancel 是 CancelFunc
 * if (go::WaitDone(ctx.context, 10 * go::Second)) { return go::Err(ctx.context); }
 * ```
 *
 * ## 与 Go 的差异
 * - Go 的 `WithCancel` 返回 `(Context, CancelFunc)`；C++ 返回 `CancelContext`
 *   （两个字段 `context` / `cancel`），可用结构化绑定：
 *   `auto [ctx, cancel] = go::WithCancel(parent);`
 * - `<-ctx.Done()` 写成 `go::WaitDone(ctx)` 或 `ctx->Done().Wait()`。
 * - `ctx.Value(key)` 在 C++ 用类型化 `ContextKey<T>`（推荐）或字符串键；
 *   返回 `std::optional<T>` / `std::any`。
 */

#pragma once

#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/go/error.go.hpp"
#include "go2cpp/go/time.go.hpp"

#include <any>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace go2cpp::go {

using Context = ::go2cpp::Context;
using ContextPtr = ::go2cpp::ContextPtr;
using CancelFunc = ::go2cpp::CancelFunc;
using CancelCauseFunc = ::go2cpp::CancelCauseFunc;
using DoneSignal = ::go2cpp::DoneSignal;

/** @brief 对应 `context.Background()`。 */
inline ContextPtr Background() {
    return Context::Background();
}

/** @brief 对应 `context.TODO()`。 */
inline ContextPtr TODO() {
    return Context::TODO();
}

/**
 * @brief 对应 `context.WithCancel` 的返回值 `(ctx, cancel)`。
 * @details 支持结构化绑定：
 *          `auto [ctx, cancel] = go::WithCancel(parent);`
 */
struct CancelContext {
    ContextPtr context;
    CancelFunc cancel;

    explicit operator bool() const noexcept {
        return static_cast<bool>(context);
    }
};

/** @brief 对应 `context.WithCancel(parent)`。 */
inline CancelContext WithCancel(const ContextPtr& parent) {
    auto [context, cancel] = Context::WithCancel(parent);
    return CancelContext{std::move(context), std::move(cancel)};
}

/** @brief 对应 `context.WithCancelCause(parent)`。 */
inline std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(
    const ContextPtr& parent) {
    return Context::WithCancelCause(parent);
}

/** @brief 对应 `context.WithTimeout(parent, d)`。 */
inline CancelContext WithTimeout(const ContextPtr& parent, Duration timeout) {
    auto [context, cancel] = Context::WithTimeout(
        parent, std::chrono::nanoseconds(timeout.Nanoseconds()));
    return CancelContext{std::move(context), std::move(cancel)};
}

/** @brief 对应 `context.WithDeadline(parent, t)`（t 为单调时钟截止点）。 */
inline CancelContext WithDeadline(const ContextPtr& parent,
                                  ::go2cpp::ContextTimePoint deadline) {
    auto [context, cancel] = Context::WithDeadline(parent, deadline);
    return CancelContext{std::move(context), std::move(cancel)};
}

/** @brief 字符串键版本，对应 `context.WithValue(parent, key, value)`。 */
inline ContextPtr WithValue(const ContextPtr& parent, std::string key,
                            std::any value) {
    return Context::WithValue(parent, std::move(key), std::move(value));
}

/** @brief 类型化键版本，推荐使用（避免键碰撞）。 */
template <typename T>
inline ContextPtr WithValue(const ContextPtr& parent,
                            const ::go2cpp::ContextKey<T>& key, T value) {
    return Context::WithValue(parent, key, std::move(value));
}

/** @brief 对应 `ctx.Done()`；返回可 Wait/IsDone 的完成信号。 */
inline const DoneSignal& Done(const ContextPtr& context) {
    static const ContextPtr kBackground = Context::Background();
    return context ? context->Done() : kBackground->Done();
}

/** @brief 对应 `ctx.Err()`；未取消返回 nil。 */
inline error Err(const ContextPtr& context) {
    return context ? context->Err() : error{};
}

/** @brief 对应 `ctx.Cause()`（Go 1.20）。 */
inline error Cause(const ContextPtr& context) {
    return context ? context->Cause() : error{};
}

/** @brief 对应 `context.Canceled`；复用已有 Context 错误实例。 */
inline const error& ErrCanceled() {
    static const error kValue = CanceledError();
    return kValue;
}

/** @brief 对应 `<-ctx.Done()`，无限等待。 */
inline void WaitDone(const ContextPtr& context) {
    Done(context).Wait();
}

/**
 * @brief 带相对超时的 `<-ctx.Done()`。
 * @return 已取消返回 true；超时返回 false。
 */
inline bool WaitDone(const ContextPtr& context, Duration timeout) {
    return Done(context).WaitFor(std::chrono::nanoseconds(timeout.Nanoseconds()));
}

/** @brief 对应 `ctx.Deadline()`；无截止时间返回空。 */
inline std::optional<Time> Deadline(const ContextPtr& context) {
    if (!context) {
        return std::nullopt;
    }
    const auto deadline = context->Deadline();
    if (!deadline.has_value()) {
        return std::nullopt;
    }
    const auto remaining = *deadline - ::go2cpp::Context::Now();
    return Time::Now().Add(Duration(
        std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count()));
}

/** @brief 对应 `ctx.Value(key)` 的字符串键版本。 */
inline std::any Value(const ContextPtr& context, const std::string& key) {
    return context ? context->Value(key) : std::any{};
}

/** @brief 对应 `ctx.Value(key)` 的类型化键版本。 */
template <typename T>
inline std::optional<T> Value(const ContextPtr& context,
                              const ::go2cpp::ContextKey<T>& key) {
    return context ? context->Value(key) : std::nullopt;
}

}  // namespace go2cpp::go
