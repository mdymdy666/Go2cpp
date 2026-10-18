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
 *
 * ## 模块定位
 *
 * `encoding` 内部的错误包装层：base64/hex/binary 的失败需要「Go 风格的一句
 * 完整文本」+「可按身份匹配的锚点」，通用 `Wrap` 会把文本拼成两层，因此单独
 * 实现 `TaggedError`。只被 `src/encoding/` 下的 `.cpp` 使用，不是公开 API。
 *
 * ## 本文件定义什么
 *
 * - `TaggedError`：保存完整文本与身份锚点的 `Error` 子类；
 * - `Tagged()`：构造 `TaggedError` 的工厂（`kind` 为空时返回空错误）。
 *
 * ## 阅读建议
 *
 * 先看文件头「为什么需要它」的重复文本问题，再看 `TaggedError` 的
 * `Message()`/`Unwrap()`/`Is()` 三个覆写各自的职责。
 *
 * ## 边界与陷阱
 *
 * `Message()` 不会拼接锚点文本；`Is()` 直接比较对象地址与锚点地址，不做
 * 文本比较。`Tagged()` 传入空 `kind` 时返回空错误，调用方会静默成功。
 */

#pragma once

#include "go2cpp/tools/error.hpp"

#include <memory>
#include <string>
#include <utility>

namespace go2cpp::encoding::detail {

/**
 * @brief 带身份锚点的文本错误。
 *
 * ## 定位
 * `encoding` 内部错误工厂的载体：`HexInvalidByte()`/`ErrShortBuffer()` 等
 * 锚点需要携带具体下标或长度时，用本类包一层。
 *
 * ## 不变量
 * - `m_kind` 非空（由 `Tagged()` 保证）；
 * - `Message()` 原样返回 `m_message`，不拼接任何锚点文本；
 * - `Is()` 同时接受「对象自身身份」与「`m_kind` 身份」两种匹配。
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
    std::string m_message; ///< 完整错误文本；构造时设置，Message() 原样返回
    ErrorPtr m_kind;       ///< 身份锚点；构造时设置，Unwrap()/Is() 读取
};

/// 创建 `TaggedError`；`kind` 为空时返回空错误。
inline ErrorPtr Tagged(std::string message, ErrorPtr kind) {
    if (!kind) {
        return {};
    }
    return std::make_shared<TaggedError>(std::move(message), std::move(kind));
}

}  // namespace go2cpp::encoding::detail
