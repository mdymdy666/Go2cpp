/**
 * @file chan.go.hpp
 * @brief Go `chan` 与 `select` 的 C++ 对应物。
 *
 * ## 定位
 * `go2cpp::Channel<T>` 已经实现了 Go channel 的核心语义（容量 0 的 rendezvous、
 * 有界缓冲、关闭唤醒、Fiber/线程混合等待、select 注册），本文件只做**书写层**
 * 的封装：
 *
 * ```go
 * ch := make(chan int, 8)
 * ch <- 42
 * v, ok := <-ch
 * close(ch)
 *
 * select {
 * case v := <-ch:
 * case ch <- 1:
 * case <-ctx.Done():
 * case <-time.After(time.Second):
 * default:
 * }
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * auto ch = go::Chan<int>::Make(8);
 * ch.Send(42);
 * auto [v, ok] = ch.Recv();
 * ch.Close();
 *
 * auto r = go::Select({ch.RecvCase(), ch.SendCase(1), go::DoneCase(ctx),
 *                      go::AfterCase(go::Second), go::DefaultCase()});
 * ```
 *
 * ## 语义边界
 * - Go 的 nil channel 永久阻塞；本封装的默认构造 `Chan` 是 nil，`Send`/`Recv`
 *   直接返回 false，**不会挂死**（安全差异，已在文档标注）。
 * - `Send` 在通道关闭后返回 false（Go 会 panic: send on closed channel）。
 * - `Recv` 返回 `std::pair<T, bool>`，第二个值等价于 Go 的 `ok`。
 * - `select` 在 C++ 只能通过 `SelectCase` 列表表达；`case <-time.After(d)`
 *   用 `AfterCase(d)`（time.go.hpp 提供）。
 */

#pragma once

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/go/error.go.hpp"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace go2cpp::go {

using ::go2cpp::ChannelRecvResult;
using ::go2cpp::ChannelSendResult;
using ::go2cpp::ChannelStatus;
using ::go2cpp::SelectCase;
using ::go2cpp::SelectResult;

/**
 * @brief 对应 Go 的 `chan T`。
 * @tparam T 元素类型；必须可 nothrow move（底层 Channel 的约束）。
 * @note 默认构造等价于 nil channel；用 `Make(capacity)` 创建。
 */
template <typename T>
class Chan final {
public:
    using Ptr = std::shared_ptr<Chan<T>>;
    using Element = T;

    /** @brief 创建 nil channel（Go 的 `var ch chan T`）。 */
    Chan() = default;

    /** @brief 创建指定容量的 channel；capacity 为 0 表示无缓冲。 */
    explicit Chan(std::size_t capacity)
        : m_channel(::go2cpp::Channel<T>::Create(capacity)) {}

    /** @brief 对应 `make(chan T, capacity)`。 */
    static Chan Make(std::size_t capacity = 0) {
        return Chan(capacity);
    }

    /** @brief 是否为 nil channel。 */
    bool Valid() const noexcept { return static_cast<bool>(m_channel); }
    explicit operator bool() const noexcept { return Valid(); }

    /** @brief 对应 `ch <- v`；阻塞直到发送成功或通道关闭。 */
    bool Send(T value) {
        if (!m_channel) {
            return false;
        }
        return m_channel->Send(std::move(value)).Ok();
    }

    /** @brief 带 Context 的发送；取消或超时时返回 false。 */
    bool SendCtx(const ContextPtr& context, T value) {
        if (!m_channel) {
            return false;
        }
        return m_channel->Send(std::move(value), context).Ok();
    }

    /** @brief 对应非阻塞 `select { case ch <- v: default: }`。 */
    bool TrySend(T value) {
        if (!m_channel) {
            return false;
        }
        return m_channel->TrySend(std::move(value)).Ok();
    }

    /** @brief 对应 `v, ok := <-ch`；关闭后返回 `{T{}, false}`。 */
    std::pair<T, bool> Recv() {
        if (!m_channel) {
            return {T{}, false};
        }
        auto result = m_channel->Recv();
        if (result.ready && result.value.has_value()) {
            return {std::move(*result.value), result.ok};
        }
        return {T{}, false};
    }

    /** @brief 带 Context 的接收；取消/超时时返回空 optional。 */
    std::optional<T> RecvCtx(const ContextPtr& context, error* out_error = nullptr) {
        if (!m_channel) {
            if (out_error != nullptr) {
                *out_error = New("receive on nil channel");
            }
            return std::nullopt;
        }
        auto result = m_channel->Recv(context);
        if (result.ready && result.value.has_value()) {
            return std::move(*result.value);
        }
        if (out_error != nullptr) {
            *out_error = result.error;
        }
        return std::nullopt;
    }

    /** @brief 接收并写入 out；返回是否拿到值。 */
    bool Recv(T& out) {
        auto result = Recv();
        if (!result.second) {
            return false;
        }
        out = std::move(result.first);
        return true;
    }

    /** @brief 非阻塞接收；没有数据时返回 false。 */
    bool TryRecv(T& out) {
        if (!m_channel) {
            return false;
        }
        auto result = m_channel->TryRecv();
        if (!result.ready || !result.value.has_value()) {
            return false;
        }
        out = std::move(*result.value);
        return true;
    }

    /** @brief 对应 `close(ch)`；幂等，重复关闭返回 false。 */
    bool Close() {
        if (!m_channel) {
            return false;
        }
        return m_channel->Close().Ok();
    }

    /** @brief 对应 `cap(ch)`。 */
    std::size_t Cap() const noexcept {
        return m_channel ? m_channel->Capacity() : 0;
    }

    /** @brief 对应 `len(ch)`。 */
    std::size_t Len() const { return m_channel ? m_channel->Len() : 0; }

    /** @brief 通道是否已关闭。 */
    bool IsClosed() const { return m_channel ? m_channel->IsClosed() : true; }

    /** @brief 构造 `case v := <-ch` 的 select 分支。 */
    SelectCase RecvCase() const { return ::go2cpp::RecvCase(m_channel); }

    /** @brief 构造 `case ch <- v:` 的 select 分支。 */
    SelectCase SendCase(T value) const {
        return ::go2cpp::SendCase(m_channel, std::move(value));
    }

    /** @brief 暴露底层 Channel，便于与既有 API 互操作。 */
    ::go2cpp::ChannelPtr<T> Raw() const noexcept { return m_channel; }

private:
    ::go2cpp::ChannelPtr<T> m_channel;
};

/** @brief 对应 `make(chan T, n)` 的自由函数形式。 */
template <typename T>
inline Chan<T> MakeChan(std::size_t capacity = 0) {
    return Chan<T>::Make(capacity);
}

// ============================================================================
// select
// ============================================================================

/** @brief 对应 `select { ... }` 中的 `default:` 分支。 */
inline SelectCase DefaultCase() {
    return ::go2cpp::DefaultCase();
}

/** @brief 对应 `case <-ctx.Done():`。 */
inline SelectCase DoneCase(const ContextPtr& context) {
    return ::go2cpp::DoneCase(context);
}

/**
 * @brief 执行多路 select。
 * @param cases 候选分支（`Chan::RecvCase()` / `Chan::SendCase()` /
 *              `DefaultCase()` / `DoneCase()` / `AfterCase()`）。
 * @param context 可选取消上下文。
 * @param timeout 可选整体超时。
 * @return 选中下标、值、状态；`SelectResult::index` 为分支下标。
 * @note 与 Go 的差异：Go 的 select 是语法；C++ 用 case 列表，语义一致
 *       （随机公平选择由底层实现负责，见 channel.hpp 的 Select）。
 */
inline SelectResult Select(std::initializer_list<SelectCase> cases,
                           const ContextPtr& context = {},
                           std::optional<::go2cpp::ContextDuration> timeout =
                               std::nullopt) {
    return ::go2cpp::Select(cases, context, timeout);
}

/** @brief `Select` 的 vector 重载，便于动态构造分支表。 */
inline SelectResult Select(const std::vector<SelectCase>& cases,
                           const ContextPtr& context = {},
                           std::optional<::go2cpp::ContextDuration> timeout =
                               std::nullopt) {
    return ::go2cpp::Select(cases, context, timeout);
}

}  // namespace go2cpp::go
