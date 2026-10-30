#pragma once

/**
 * @file compat.hpp
 * @brief 新旧错误体系互操作：`go2cpp::error` 与 `go2cpp::Error` 双向适配。
 *
 * @details 项目里存在两套错误表示，各自解决不同问题：
 *
 * - `go2cpp::Error`（`go2cpp/tools/error.hpp`）：Go `error` 接口的等价物，
 *   全项目既有模块的返回类型，只承载文本与原因；
 * - `go2cpp::error::Error`（`go2cpp/error/error.hpp`）：本模块新增的类型链
 *   层，额外提供错误码、类型名、继承深度与祖先链判断。
 *
 * 两者都不可变，但接口不同，不能互相继承（否则两套虚函数会形成菱形）。
 * 这里用**适配器**在边界上转换，保持各自接口不变：
 *
 * - `ToLegacy(error)`：把新错误交给只接受 `go2cpp::ErrorPtr` 的旧模块；
 * - `FromLegacy(legacy)`：把旧模块返回的错误接入新体系的类型链/错误码。
 *
 * ## 有损转换的边界（重要）
 *
 * - 旧 `WrappedError` 只暴露**展平后**的文本，不提供“本层文本”，因此
 *   `FromLegacy` 对包装链只保留完整文本（退化为 `StringError`），不会为了
 *   结构对称而丢消息或重复拼接；
 * - 旧 `JoinError` 的多原因结构可以无损恢复为新的 `JoinError`；
 * - 反向 `ToLegacy` 不丢信息：适配器把新错误的类型名/深度留在自身，
 *   原因结构按 `UnwrapAll()` 逐层适配（深度上限 `kMaxAdapterDepth`）。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/error/error.hpp"
#include "go2cpp/tools/error.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::error {

/// @brief 跨体系适配的递归深度上限；超过后更深的原因不再展开。
/// @details 两层体系各有自己的深链保护，这里只需要防止适配器自身在异常
///          深的图（例如 10 万层包装）上递归构造爆栈。
inline constexpr std::size_t kMaxAdapterDepth = 64;

/**
 * @brief 把 `go2cpp::error::ErrorPtr` 适配成旧体系 `go2cpp::Error`。
 *
 * @details 构造时一次性适配直接原因，`Unwrap()` / `UnwrapAll()` 返回缓存，
 *          避免每次遍历重新分配适配器（旧体系的 `As<T>` 用地址去重，
 *          重复创建的适配器会破坏去重效果）。
 */
class LegacyAdapter final : public ::go2cpp::Error {
 public:
    /// @brief 适配一个不可变错误节点；空指针会得到一个空适配器。
    explicit LegacyAdapter(ErrorPtr inner) : LegacyAdapter(std::move(inner), 0) {}

    /**
     * @brief 递归适配构造。
     * @param inner 被适配的新体系错误。
     * @param depth 已适配的层数；调用方一般传 0，超过 `kMaxAdapterDepth` 时
     *              不再展开更深的原因。
     * @note 公开只为让 `std::make_shared` 能构造（库代码无法访问私有构造）；
     *       普通调用请用单参构造或 `ToLegacy()`。
     */
    LegacyAdapter(ErrorPtr inner, std::size_t depth) : m_inner(std::move(inner)) {
        if (!m_inner || depth >= kMaxAdapterDepth) {
            return;
        }
        const std::vector<ErrorPtr> causes = m_inner->UnwrapAll();
        m_causes.reserve(causes.size());
        for (const auto& cause : causes) {
            if (!cause) {
                continue;
            }
            m_causes.push_back(std::make_shared<const LegacyAdapter>(cause, depth + 1));
        }
    }

    std::string Message() const override {
        return m_inner ? m_inner->Message() : std::string();
    }

    std::string ErrorString() const override { return Message(); }

    ::go2cpp::ErrorPtr Unwrap() const override {
        return m_causes.empty() ? ::go2cpp::ErrorPtr() : m_causes.front();
    }

    std::vector<::go2cpp::ErrorPtr> UnwrapAll() const override { return m_causes; }

    /// @brief 被适配的原始新体系错误；空适配器返回空指针。
    const ErrorPtr& Inner() const noexcept { return m_inner; }

 private:
    ErrorPtr m_inner;                         ///< 原始新体系错误。
    std::vector<::go2cpp::ErrorPtr> m_causes; ///< 预适配的直接原因。
};

/**
 * @brief 新体系错误 → 旧体系错误指针。
 * @param error 新体系错误；空指针返回空。
 */
inline ::go2cpp::ErrorPtr ToLegacy(const ErrorPtr& error) {
    if (!error) {
        return {};
    }
    return std::make_shared<const LegacyAdapter>(error);
}

namespace detail {

inline ErrorPtr FromLegacyImpl(const ::go2cpp::ErrorPtr& legacy, std::size_t depth);

} // namespace detail

/**
 * @brief 旧体系错误 → 新体系错误指针。
 * @param legacy 旧体系错误；空指针返回空。
 * @return 旧 `JoinError` 保留聚合结构；其余叶子化以文本为准（见文件头说明）。
 */
inline ErrorPtr FromLegacy(const ::go2cpp::ErrorPtr& legacy) {
    return detail::FromLegacyImpl(legacy, 0);
}

namespace detail {

inline ErrorPtr FromLegacyImpl(const ::go2cpp::ErrorPtr& legacy, std::size_t depth) {
    if (!legacy) {
        return {};
    }
    if (depth >= kMaxAdapterDepth) {
        // 超深图直接叶子化：文本仍然完整，只是丢掉更深的结构。
        return NewError(legacy->Message());
    }
    if (const auto* joined = dynamic_cast<const ::go2cpp::JoinError*>(legacy.get())) {
        std::vector<ErrorPtr> causes;
        const std::vector<::go2cpp::ErrorPtr> children = joined->UnwrapAll();
        causes.reserve(children.size());
        for (const auto& child : children) {
            if (child) {
                causes.push_back(FromLegacyImpl(child, depth + 1));
            }
        }
        return Join(std::move(causes));
    }
    // 旧 WrappedError 的 Message() 已经是 "a: b: c" 展平文本；按结构转换会让
    // 本层文本重复或丢失，因此这里以文本为准。
    return NewError(legacy->Message());
}

} // namespace detail

} // namespace go2cpp::error
