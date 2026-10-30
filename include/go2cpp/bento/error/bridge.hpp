#pragma once

/**
 * @file bridge.hpp
 * @brief `go2cpp::error` 与 `bento::error` 的互操作桥（薄封装，不改动原模块）。
 *
 * @details Bento 的 `bento/error` 是独立实现的通用错误图，模型与新体系
 *          一致（不可变节点、包装/聚合、按地址去重的图查询），但错误码
 *          词表不同：Bento 沿用 csvp 的 16 码，Go2Cpp 用自研的 23 码。为了
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
 * 两侧枚举**不再逐值一致**：`kLegacyCodeBridge` 给出 csvp/gRPC 16 码 →
 * Go2Cpp 23 码的映射表，`ToGo2CppCode()` 查表、`ToBentoCode()` 反向有损
 * 退化。16 条往返约束仍由 `static_assert` 在编译期锁定：任一侧改值都会
 * 立刻编译失败，而不是运行期静默错位。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/bento/error/error.hpp"
#include "go2cpp/error/error.hpp"

#include <array>
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

/**
 * @brief 一条跨词表映射记录。
 *
 * @details `legacy` 是 Bento/csvp 的 16 码，`current` 是 Go2Cpp 自研码，
 *          `note` 记录语义差异，排查跨体系日志时可直接对着表看。
 */
struct LegacyCodeBridgeEntry {
    ErrorCode legacy;                   ///< Bento/csvp 错误码（0–15）。
    ::go2cpp::error::ErrorCode current; ///< Go2Cpp 错误码（0–22）。
    std::string_view note;              ///< 语义说明。
};

/**
 * @brief csvp/gRPC 16 码 → Go2Cpp 23 码的映射表。
 *
 * @details 两侧**不再要求数值一致**：Go2Cpp 的码按失败原因分类（类型/
 *          格式/超时/取消/IO/并发……），旧词表只作为兼容面存在。完整设计
 *          见 `go2cpp/error/code.hpp`；纯数值兼容入口见
 *          `go2cpp/error/legacy_code.hpp` 的 `FromLegacyCode()`。
 */
inline constexpr std::array<LegacyCodeBridgeEntry, 16> kLegacyCodeBridge = {{
    {ErrorCode::kOk, ::go2cpp::error::ErrorCode::kOk, "同义"},
    {ErrorCode::kUnknown, ::go2cpp::error::ErrorCode::kUnknown, "同义"},
    {ErrorCode::kInvalidArgument, ::go2cpp::error::ErrorCode::kInvalidArgument, "同义"},
    {ErrorCode::kNotFound, ::go2cpp::error::ErrorCode::kNotFound, "同义"},
    {ErrorCode::kAlreadyExists, ::go2cpp::error::ErrorCode::kAlreadyExists, "同义"},
    {ErrorCode::kPermissionDenied, ::go2cpp::error::ErrorCode::kPermissionDenied, "同义"},
    {ErrorCode::kResourceExhausted, ::go2cpp::error::ErrorCode::kResourceExhausted, "同义"},
    {ErrorCode::kFailedPrecondition, ::go2cpp::error::ErrorCode::kFailedPrecondition, "同义"},
    {ErrorCode::kOutOfRange, ::go2cpp::error::ErrorCode::kOutOfRange, "同义"},
    {ErrorCode::kUnimplemented, ::go2cpp::error::ErrorCode::kUnimplemented, "同义"},
    {ErrorCode::kInternal, ::go2cpp::error::ErrorCode::kInternal, "同义"},
    {ErrorCode::kUnavailable, ::go2cpp::error::ErrorCode::kUnavailable, "同义"},
    {ErrorCode::kDataLoss, ::go2cpp::error::ErrorCode::kCorruption, "改名：本地语义是数据损坏/校验失败"},
    {ErrorCode::kCancelled, ::go2cpp::error::ErrorCode::kCancelled, "同义"},
    {ErrorCode::kDeadlineExceeded, ::go2cpp::error::ErrorCode::kTimeout, "改名：deadline 归入统一的超时码"},
    {ErrorCode::kWrappedError, ::go2cpp::error::ErrorCode::kWrappedError, "同义"},
}};

/// @brief Bento/csvp 错误码 → Go2Cpp 错误码（查表；未知值退化为 `kUnknown`）。
constexpr ::go2cpp::error::ErrorCode ToGo2CppCode(ErrorCode code) noexcept {
    for (const LegacyCodeBridgeEntry& entry : kLegacyCodeBridge) {
        if (entry.legacy == code) {
            return entry.current;
        }
    }
    return ::go2cpp::error::ErrorCode::kUnknown;
}

/**
 * @brief Go2Cpp 错误码 → Bento/csvp 错误码（**有损**）。
 *
 * @details 旧词表装不下的新码按语义就近退化：类型/格式错误 →
 *          INVALID_ARGUMENT；未认证 → PERMISSION_DENIED；IO 错误/未就绪 →
 *          UNAVAILABLE；中止/死锁 → FAILED_PRECONDITION；数据损坏 →
 *          DATA_LOSS。无损性可用 `IsBentoCodeLossless()` 判断。
 * @note 这是跨体系边界的最后手段；能直接传新码时不要先退化成 Bento 码。
 */
constexpr ErrorCode ToBentoCode(::go2cpp::error::ErrorCode code) noexcept {
    switch (code) {
        case ::go2cpp::error::ErrorCode::kOk:
            return ErrorCode::kOk;
        case ::go2cpp::error::ErrorCode::kUnknown:
            return ErrorCode::kUnknown;
        case ::go2cpp::error::ErrorCode::kTypeMismatch:
            return ErrorCode::kInvalidArgument;
        case ::go2cpp::error::ErrorCode::kInvalidArgument:
            return ErrorCode::kInvalidArgument;
        case ::go2cpp::error::ErrorCode::kOutOfRange:
            return ErrorCode::kOutOfRange;
        case ::go2cpp::error::ErrorCode::kFormatError:
            return ErrorCode::kInvalidArgument;
        case ::go2cpp::error::ErrorCode::kNotFound:
            return ErrorCode::kNotFound;
        case ::go2cpp::error::ErrorCode::kAlreadyExists:
            return ErrorCode::kAlreadyExists;
        case ::go2cpp::error::ErrorCode::kPermissionDenied:
            return ErrorCode::kPermissionDenied;
        case ::go2cpp::error::ErrorCode::kUnauthenticated:
            return ErrorCode::kPermissionDenied;
        case ::go2cpp::error::ErrorCode::kTimeout:
            return ErrorCode::kDeadlineExceeded;
        case ::go2cpp::error::ErrorCode::kCancelled:
            return ErrorCode::kCancelled;
        case ::go2cpp::error::ErrorCode::kResourceExhausted:
            return ErrorCode::kResourceExhausted;
        case ::go2cpp::error::ErrorCode::kUnavailable:
            return ErrorCode::kUnavailable;
        case ::go2cpp::error::ErrorCode::kFailedPrecondition:
            return ErrorCode::kFailedPrecondition;
        case ::go2cpp::error::ErrorCode::kAborted:
            return ErrorCode::kFailedPrecondition;
        case ::go2cpp::error::ErrorCode::kIOError:
            return ErrorCode::kUnavailable;
        case ::go2cpp::error::ErrorCode::kCorruption:
            return ErrorCode::kDataLoss;
        case ::go2cpp::error::ErrorCode::kInternal:
            return ErrorCode::kInternal;
        case ::go2cpp::error::ErrorCode::kUnimplemented:
            return ErrorCode::kUnimplemented;
        case ::go2cpp::error::ErrorCode::kWouldBlock:
            return ErrorCode::kUnavailable;
        case ::go2cpp::error::ErrorCode::kDeadlock:
            return ErrorCode::kFailedPrecondition;
        case ::go2cpp::error::ErrorCode::kWrappedError:
            return ErrorCode::kWrappedError;
    }
    return ErrorCode::kUnknown;
}

/**
 * @brief Go2Cpp 码能否经 Bento 码无损往返。
 * @details 为 true 的 16 个码跨桥不丢语义；其余新码会退化（见 `ToBentoCode`）。
 */
constexpr bool IsBentoCodeLossless(::go2cpp::error::ErrorCode code) noexcept {
    return ToGo2CppCode(ToBentoCode(code)) == code;
}

namespace detail {

/// @brief 16 个旧码经「新码 → Bento 码」必须回到原值（映射表自洽性检查）。
constexpr bool BridgeRoundTrips() noexcept {
    for (const LegacyCodeBridgeEntry& entry : kLegacyCodeBridge) {
        if (ToBentoCode(entry.current) != entry.legacy) {
            return false;
        }
    }
    return true;
}

} // namespace detail

static_assert(kLegacyCodeBridge.size() == 16, "映射表必须覆盖 csvp/gRPC 的全部 16 个码");
static_assert(detail::BridgeRoundTrips(),
              "kLegacyCodeBridge 与 ToBentoCode 不一致：16 个旧码必须能经新码往返");


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
