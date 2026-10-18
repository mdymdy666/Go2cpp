/**
 * @file errors.hpp
 * @brief `encoding` 模块内部使用的「带身份标签的错误」工具。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause
 *
 * @details 为什么需要它：Go 的错误文本通常只有一句（例如
 *          `encoding/hex: odd length hex string`），但 `errors.Is` 又要求按身份
 *          匹配错误类别。项目通用做法是 `Wrap(anchor, message)`，而
 *          `WrappedError::Message()` 会把两层文本拼接成
 *          `message: anchor`；直接用于本模块会得到重复文本。
 *
 *          本类只保存**一句完整文本**，同时保留 `kind` 供 `Is` 匹配：
 *          `Message()` 返回原样文本，`Unwrap()` 返回身份锚点。
 *
 * @note 这是模块内部头文件，不属于四个子模块的公开 API；调用方应使用
 *       各子模块头文件里公开的错误锚点（`Base64CorruptInput()` 等）。
 */

#pragma once

#include "go2cpp/tools/error.hpp"

#include <memory>
#include <string>
#include <utility>

namespace go2cpp::encoding::detail {

/**
 * @brief 带身份锚点的文本错误。
 */
class TaggedError final : public Error {
 public:
    /**
     * @brief 构造错误。
     * @param message 完整错误文本（`Message()` 原样返回）。
     * @param kind 身份锚点，用于 `Is` 匹配。
     */
    TaggedError(std::string message, ErrorPtr kind)
        : m_message(std::move(message)), m_kind(std::move(kind)) {}

    /// 返回构造时给出的完整文本。
    std::string Message() const override { return m_message; }

    /// 返回身份锚点，使通用的错误链遍历也能匹配。
    ErrorPtr Unwrap() const override { return m_kind; }

    /// 直接匹配身份锚点（`this == &target` 保持与基类一致的语义）。
    bool Is(const Error& target) const noexcept override {
        return this == &target || (m_kind != nullptr && m_kind.get() == &target);
    }

 private:
    std::string m_message;
    ErrorPtr m_kind;
};

/// 创建 `TaggedError`；`kind` 为空时返回空错误。
inline ErrorPtr Tagged(std::string message, ErrorPtr kind) {
    if (!kind) {
        return {};
    }
    return std::make_shared<TaggedError>(std::move(message), std::move(kind));
}

}  // namespace go2cpp::encoding::detail
