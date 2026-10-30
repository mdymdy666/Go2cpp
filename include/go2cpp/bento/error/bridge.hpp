#pragma once

/**
 * @file bridge.hpp
 * @brief `go2cpp::error` 与 `bento::error` 的互操作桥（薄封装，不改动原模块）。
 *
 * @details Bento 的 `bento/error` 是独立实现的通用错误图，语义与新体系高度
 *          一致（同样的 16 个 `ErrorCode`、不可变节点、包装/聚合）。为了
 *          避免两套错误在模块边界上“各说各话”，本文件提供零依赖的双向
 *          适配器：
 *
 * - `FromGo2Cpp(error)`：把新体系错误包装成 `bento::error::ErrorPtr`，
 *   保留消息、错误码、类型名、父类型名、继承深度与原因结构；
 * - `ToGo2Cpp(error)`：把 Bento 错误转成新体系错误，`JoinError` 结构无损，
 *   包装链按展平文本叶子化（Bento 的 `WrappedError::Message()` 同样只暴露
 *   展平文本，强行分层会重复或丢消息）。
 *
 * 现有 `bento/error/{error,mapping,registry}.hpp` 一行未改，所有既有调用点
 * 保持原样；需要跨体系传错误时才显式 include 本头。
 *
 * ## 错误码契约
 *
 * 两侧枚举必须逐值一致，本文件用 16 条 `static_assert` 在编译期锁定；
 * 任一侧增删或改值都会立刻编译失败，而不是运行期静默错位。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/bento/error/error.hpp"
#include "go2cpp/error/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace go2cpp::bento::error {

/// @brief 跨体系适配的递归深度上限，防止异常深图递归爆栈。
inline constexpr std::size_t kMaxBridgeDepth = 64;

#define GO2CPP_ERROR_BRIDGE_CODE_ALIGN(name)                                                        \
    static_assert(static_cast<std::int32_t>(::go2cpp::error::ErrorCode::name) ==                    \
                      static_cast<std::int32_t>(ErrorCode::name),                   \
                  "go2cpp::error::ErrorCode 与 bento::error::ErrorCode 数值不一致：" #name)

GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kOk);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kUnknown);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kInvalidArgument);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kNotFound);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kAlreadyExists);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kPermissionDenied);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kResourceExhausted);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kFailedPrecondition);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kOutOfRange);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kUnimplemented);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kInternal);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kUnavailable);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kDataLoss);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kCancelled);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kDeadlineExceeded);
GO2CPP_ERROR_BRIDGE_CODE_ALIGN(kWrappedError);

#undef GO2CPP_ERROR_BRIDGE_CODE_ALIGN

/// @brief 新体系错误码 → Bento 错误码（数值一致，零成本转换）。
inline ErrorCode ToBentoCode(::go2cpp::error::ErrorCode code) noexcept {
    return static_cast<ErrorCode>(static_cast<std::int32_t>(code));
}

/// @brief Bento 错误码 → 新体系错误码（数值一致，零成本转换）。
inline ::go2cpp::error::ErrorCode ToGo2CppCode(ErrorCode code) noexcept {
    return static_cast<::go2cpp::error::ErrorCode>(static_cast<std::int32_t>(code));
}

/**
 * @brief 把新体系错误适配成 `bento::error::Error`。
 *
 * @details 构造时一次性适配直接原因（有深度上限），`UnwrapAll()` 返回缓存，
 *          保证 Bento 侧按地址去重的查询（`As`/`WalkErrors`）语义不被破坏。
 */
class BentoAdapter final : public Error {
 public:
    /// @brief 适配一个不可变新体系错误；空指针得到空适配器。
    explicit BentoAdapter(::go2cpp::error::ErrorPtr inner) : BentoAdapter(std::move(inner), 0) {}

    /**
     * @brief 递归适配构造。
     * @param inner 被适配的新体系错误。
     * @param depth 已适配层数；一般传 0。
     * @note 公开只为让 `std::make_shared` 能构造；普通调用请用单参构造或
     *       `FromGo2Cpp()`。
     */
    BentoAdapter(::go2cpp::error::ErrorPtr inner, std::size_t depth)
        : m_inner(std::move(inner)) {
        if (!m_inner || depth >= kMaxBridgeDepth) {
            return;
        }
        const std::vector<::go2cpp::error::ErrorPtr> causes = m_inner->UnwrapAll();
        m_causes.reserve(causes.size());
        for (const auto& cause : causes) {
            if (!cause) {
                continue;
            }
            m_causes.push_back(std::make_shared<const BentoAdapter>(cause, depth + 1));
        }
    }

    std::string Message() const override {
        return m_inner ? m_inner->Message() : std::string();
    }

    ErrorCode Code() const noexcept override {
        if (!m_inner) {
            return ErrorCode::kUnknown;
        }
        return ToBentoCode(m_inner->Code());
    }

    std::string_view TypeName() const noexcept override {
        return m_inner ? m_inner->TypeName() : std::string_view("bento.Error");
    }

    std::string_view ParentTypeName() const noexcept override {
        return m_inner ? m_inner->ParentTypeName() : std::string_view();
    }

    std::uint8_t InheritanceDepth() const noexcept override {
        return m_inner ? m_inner->InheritanceDepth() : std::uint8_t{0};
    }

    bool IsWrapping() const noexcept override {
        return m_inner ? m_inner->IsWrapping() : false;
    }

    std::vector<ErrorPtr> UnwrapAll(std::size_t level = 1) const override {
        if (level != 0) {
            return m_causes;
        }
        // 全部后代：沿已适配的子树做地址去重的先序展开。
        std::vector<ErrorPtr> result;
        std::vector<ErrorPtr> pending(m_causes.rbegin(), m_causes.rend());
        std::unordered_set<const Error*> visited;
        while (!pending.empty()) {
            ErrorPtr node = std::move(pending.back());
            pending.pop_back();
            if (!node || !visited.insert(node.get()).second) {
                continue;
            }
            result.push_back(node);
            const std::vector<ErrorPtr> children = node->UnwrapAll();
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (*it) {
                    pending.push_back(*it);
                }
            }
        }
        return result;
    }

    /// @brief 被适配的原始新体系错误；空适配器返回空指针。
    const ::go2cpp::error::ErrorPtr& Inner() const noexcept { return m_inner; }

 private:
    ::go2cpp::error::ErrorPtr m_inner;                ///< 原始新体系错误。
    std::vector<ErrorPtr> m_causes;   ///< 预适配的直接原因。
};

/**
 * @brief 新体系错误 → Bento 错误指针。
 * @param error 新体系错误；空指针返回空。
 */
inline ErrorPtr FromGo2Cpp(const ::go2cpp::error::ErrorPtr& error) {
    if (!error) {
        return {};
    }
    return std::make_shared<const BentoAdapter>(error);
}

namespace detail {

/**
 * @brief 沿包装链取最内层非包装节点的错误码。
 *
 * @details 包装链叶子化后，`kWrappedError` 不再表示“这是包装节点”，继续
 *          保留会让调用方按码分类时看不到根因。这里穿透到第一个非包装
 *          后代取码（遇到 Join 停止，Join 有自己的转换分支）。穿透只读，
 *          依赖父节点持有子节点 shared_ptr，节点在循环期间始终存活。
 */
inline ::go2cpp::error::ErrorCode ResolveLeafCode(const ErrorPtr& error) {
    const Error* node = error.get();
    for (std::size_t guard = 0; node != nullptr && guard < kMaxBridgeDepth; ++guard) {
        if (dynamic_cast<const JoinError*>(node) != nullptr) {
            break;
        }
        if (!node->IsWrapping()) {
            break;
        }
        const std::vector<ErrorPtr> causes = node->UnwrapAll();
        if (causes.empty() || !causes.front()) {
            break;
        }
        node = causes.front().get();
    }
    return node != nullptr ? ToGo2CppCode(node->Code()) : ::go2cpp::error::ErrorCode::kUnknown;
}

inline ::go2cpp::error::ErrorPtr ToGo2CppImpl(const ErrorPtr& error, std::size_t depth) {
    if (!error) {
        return {};
    }
    if (depth < kMaxBridgeDepth) {
        if (const auto* joined = dynamic_cast<const JoinError*>(error.get())) {
            std::vector<::go2cpp::error::ErrorPtr> causes;
            const std::vector<ErrorPtr> children = joined->UnwrapAll();
            causes.reserve(children.size());
            for (const auto& child : children) {
                if (child) {
                    causes.push_back(ToGo2CppImpl(child, depth + 1));
                }
            }
            return ::go2cpp::error::Join(std::move(causes));
        }
    }
    // 叶子与包装链都按文本叶子化：Bento 的 WrappedError::Message() 只暴露
    // 展平文本，强行按结构转换会重复或丢失消息片段；错误码取最内层叶子，
    // 保证按码分类仍能看到根因。
    return ::go2cpp::error::NewError(error->Message(), ResolveLeafCode(error));
}

} // namespace detail

/**
 * @brief Bento 错误 → 新体系错误指针。
 * @param error Bento 错误；空指针返回空。
 * @return `JoinError` 保留聚合结构，其余叶子化以保证文本无损。
 */
inline ::go2cpp::error::ErrorPtr ToGo2Cpp(const ErrorPtr& error) {
    return detail::ToGo2CppImpl(error, 0);
}

} // namespace go2cpp::bento::error
