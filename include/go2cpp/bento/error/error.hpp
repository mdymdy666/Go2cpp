#pragma once

/**
 * @file error.hpp
 * @brief Bento 通用错误基础设施：错误码、不可变错误图、遍历与查询。
 *
 * @details 本模块只依赖标准库，不 include 任何 Bento 业务头文件，供
 *          `bento/net`、未来的 `bento/reflect` 使用者以及外部调用方共享。
 *
 *          错误以 `ErrorPtr = shared_ptr<const Error>` 传递，节点构造后不可变；
 *          单链由 `WrappedError` 表达，多原因由 `JoinError` 表达，因此错误
 *          实际构成有向图而不是纯链表。所有查询接口（`Is`/`As`/`Has`/
 *          `WalkErrors`/`UnwrapAll(0)`）都用地址去重，遇到自指/环不会死循环。
 *
 * ## 错误码的两套用途
 *
 * - `ErrorCode`：固定枚举，语义与 gRPC 状态码对齐（0–14），另外保留
 *   `kWrappedError`（15）标记“这是一个包装节点”。业务模块可以
 *   `static_cast<ErrorCode>(>=1000)` 携带私有码。
 * - `ErrorCodeRegistry`（registry.hpp）：运行期注册的稳定字符串码，用于
 *   模块自定义错误（例如 `"bento.protocol"`），避免各模块手写枚举撞值。
 *
 * ## 文本规则
 *
 * - `WrappedError::Message()` 把连续的同类型包装展平为 `"a: b: c"`；
 * - `JoinError::Message()` 用换行连接各原因，嵌套 Join 会被展平；
 * - 空消息片段被跳过，不会产生多余的 `": "` 或空行；
 * - `Message()` 假定错误图无环；环安全的入口是 `Is`/`As`/`Has`/`WalkErrors`。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace go2cpp::bento::error {

/**
 * @brief 通用错误码，语义对齐 gRPC 状态码。
 *
 * @details 0–14 与 Bento 场景常用的 gRPC 状态一一对应；15 保留给包装节点
 *          （`WrappedError`/`JoinError`）。业务模块的私有码从 1000 起，
 *          通过 `static_cast<ErrorCode>` 存入 `StringError`。
 */
enum class ErrorCode : std::int32_t {
    kOk = 0,                 ///< 成功（用于“无错误”的占位）。
    kUnknown = 1,            ///< 未知错误。
    kInvalidArgument = 2,    ///< 参数非法。
    kNotFound = 3,           ///< 目标不存在。
    kAlreadyExists = 4,      ///< 目标已存在。
    kPermissionDenied = 5,   ///< 权限不足。
    kResourceExhausted = 6,  ///< 资源耗尽（连接/内存/配额）。
    kFailedPrecondition = 7, ///< 当前状态不满足前置条件。
    kOutOfRange = 8,         ///< 越界（数值/索引/长度）。
    kUnimplemented = 9,      ///< 功能未实现。
    kInternal = 10,          ///< 内部错误。
    kUnavailable = 11,       ///< 服务/对端暂不可用。
    kDataLoss = 12,          ///< 数据损坏或丢失。
    kCancelled = 13,         ///< 调用方取消。
    kDeadlineExceeded = 14,  ///< 超时。
    kWrappedError = 15,      ///< 包装节点（WrappedError/JoinError 的 Code）。
};

class Error;
using ErrorPtr = std::shared_ptr<const Error>;

/**
 * @brief 不可变错误节点接口。
 *
 * @details 具体类型必须实现 `Message`/`Code`/`TypeName`/`ParentTypeName`/
 *          `InheritanceDepth`。有子节点的类型覆写 `IsWrapping()` 与
 *          `UnwrapAll()`；自定义 `Is()` 可实现按标记匹配。
 *
 * @note `UnwrapAll(level)` 的约定与参考设计一致：`level == 0` 返回全部后代
 *       （去重、环安全），`level != 0` 只返回直接原因。默认 `level = 1`。
 */
class Error {
 public:
    virtual ~Error() = default;

    /// @brief 面向用户的错误文本。
    virtual std::string Message() const = 0;
    /// @brief 错误码。
    virtual ErrorCode Code() const noexcept = 0;
    /// @brief 运行期类型名；由 `BENTO_ERROR_CLASS` 或手写实现提供。
    virtual std::string_view TypeName() const noexcept = 0;
    /// @brief 父类型名；根类型 `bento.Error` 的直接子类返回 `"bento.Error"`。
    virtual std::string_view ParentTypeName() const noexcept = 0;
    /// @brief 继承深度；根为 0，直接派生为 1，最多 3 层。
    virtual std::uint8_t InheritanceDepth() const noexcept = 0;

    /// @brief 是否可能包含子节点；默认 false。
    virtual bool IsWrapping() const noexcept { return false; }
    /// @brief 返回直接原因（`level != 0`）或全部后代（`level == 0`）。
    virtual std::vector<ErrorPtr> UnwrapAll(std::size_t level = 1) const;
    /// @brief 身份判断；默认按对象地址，自定义类型可覆写。
    virtual bool Is(const Error& target) const noexcept { return this == &target; }
};

/// @brief 枚举码的稳定名字；未知/私有码返回 `"UNKNOWN"`。
std::string_view ErrorCodeName(ErrorCode code) noexcept;

/**
 * @brief 遍历错误图时访问者对当前节点的处理决定。
 *
 * @details `WalkErrors` 是前序深度优先：先把当前节点放进结果，再按返回值
 *          决定是否展开子节点。
 */
enum class ErrorVisitResult : std::uint8_t {
    kContinue = 0,     ///< 继续访问当前节点的子节点。
    kSkipChildren = 1, ///< 不展开当前节点，继续兄弟节点。
    kStop = 2,         ///< 立即结束遍历，`WalkResult::status` 记为 kStop。
};

using ErrorVisitor = std::function<ErrorVisitResult(const ErrorPtr&)>;

/// @brief WalkErrors 的结果快照。
struct WalkResult {
    std::vector<ErrorPtr> errors;                              ///< 已访问节点（前序）。
    ErrorVisitResult status = ErrorVisitResult::kContinue;     ///< 结束原因。
    bool ok = true;                                            ///< false = 访问器为空或抛异常。
};

/**
 * @brief 前序深度优先遍历错误图。
 * @param error 起点；空指针时返回空结果（`ok == true`）。
 * @param visitor 访问器；为空时 `ok == false`。
 * @return 访问记录；visitor 抛异常时保留已访问部分并置 `ok = false`。
 * @note 每个对象最多访问一次，自指图不会死循环。
 */
WalkResult WalkErrors(const ErrorPtr& error, const ErrorVisitor& visitor) noexcept;

/// @brief 叶子错误：只有文本与错误码。
class StringError final : public Error {
 public:
    explicit StringError(std::string message, ErrorCode code = ErrorCode::kUnknown)
        : m_message(std::move(message)), m_code(code) {}

    std::string Message() const override { return m_message; }
    ErrorCode Code() const noexcept override { return m_code; }
    std::string_view TypeName() const noexcept override { return "bento.StringError"; }
    std::string_view ParentTypeName() const noexcept override { return "bento.Error"; }
    std::uint8_t InheritanceDepth() const noexcept override { return 1; }

 private:
    std::string m_message;
    ErrorCode m_code;
};

/// @brief 单原因包装：`Message()` 展平连续包装为 `"context: cause"`。
class WrappedError : public Error {
 public:
    WrappedError(std::string message, ErrorPtr cause);
    ~WrappedError() override;

    WrappedError(const WrappedError&) = delete;
    WrappedError& operator=(const WrappedError&) = delete;

    std::string Message() const override;
    ErrorCode Code() const noexcept override { return ErrorCode::kWrappedError; }
    std::string_view TypeName() const noexcept override { return "bento.WrappedError"; }
    std::string_view ParentTypeName() const noexcept override { return "bento.Error"; }
    std::uint8_t InheritanceDepth() const noexcept override { return 1; }
    bool IsWrapping() const noexcept override { return true; }
    std::vector<ErrorPtr> UnwrapAll(std::size_t level = 1) const override;

 private:
    std::string m_message;
    ErrorPtr m_cause;
};

/// @brief 多原因聚合：`Message()` 用换行连接各叶子文本。
class JoinError : public Error {
 public:
    explicit JoinError(std::vector<ErrorPtr> causes);
    ~JoinError() override;

    JoinError(const JoinError&) = delete;
    JoinError& operator=(const JoinError&) = delete;

    std::string Message() const override;
    ErrorCode Code() const noexcept override { return ErrorCode::kWrappedError; }
    std::string_view TypeName() const noexcept override { return "bento.JoinError"; }
    std::string_view ParentTypeName() const noexcept override { return "bento.Error"; }
    std::uint8_t InheritanceDepth() const noexcept override { return 1; }
    bool IsWrapping() const noexcept override { return true; }
    std::vector<ErrorPtr> UnwrapAll(std::size_t level = 1) const override;

 private:
    std::vector<ErrorPtr> m_causes;
};

/// @brief 创建叶子错误；`message` 可为空。
ErrorPtr NewError(std::string message, ErrorCode code = ErrorCode::kUnknown);

/// @brief 包装原因；`cause` 为空时返回空指针（不静默丢弃上下文之外的语义）。
ErrorPtr Wrap(ErrorPtr cause, std::string message);

/// @brief 聚合多个原因；过滤空指针，全部为空时返回空指针。
ErrorPtr Join(std::vector<ErrorPtr> causes);

/// @brief 聚合多个原因（initializer_list 便捷重载）。
ErrorPtr Join(std::initializer_list<ErrorPtr> causes);

/// @brief 是否为非空包装节点。
inline bool IsWrappedError(const ErrorPtr& error) noexcept {
    return error != nullptr && error->IsWrapping();
}

/// @brief 便捷入口：`error ? error->UnwrapAll(level) : {}`。
std::vector<ErrorPtr> UnwrapAll(const ErrorPtr& error, std::size_t level = 1);

/// @brief 错误文本；空指针返回空串。
std::string ErrorMessage(const ErrorPtr& error);

/**
 * @brief 判断错误图中是否存在与 `target` 身份匹配的节点。
 * @details `target` 为空时按 Go `errors.Is(err, nil)` 语义返回 `!error`。
 *          遍历环安全；自定义 `UnwrapAll()` 抛异常时返回 false。
 */
bool Is(const ErrorPtr& error, const ErrorPtr& target) noexcept;

/// @brief 判断错误图中是否存在指定错误码的节点；环安全、异常安全。
bool Has(const ErrorPtr& error, ErrorCode code) noexcept;

namespace detail {

/**
 * @brief 错误图访问集：前 8 个节点内联存储，超过容量后才分配哈希表。
 *
 * @details 正常业务错误图通常只有个位数节点；`unordered_set` 的桶分配
 *          （每次查询 1-2 次）比节点遍历本身更贵。内联数组让常见路径零分配，
 *          地址去重的语义保持不变。
 *
 * @note 溢出后分配哈希表失败会抛 `std::bad_alloc`，由 `Is`/`Has`/`As`
 *       等 noexcept 入口统一转换为“未匹配/失败”，不做静默降级。
 */
class VisitSet {
 public:
    /**
     * @brief 记录一个节点。
     * @return 首次出现返回 true；重复出现返回 false。
     */
    bool Insert(const Error* node) {
        for (std::size_t index = 0; index < m_inline_count; ++index) {
            if (m_inline[index] == node) {
                return false;
            }
        }
        if (!m_overflow) {
            if (m_inline_count < kInlineCapacity) {
                m_inline[m_inline_count++] = node;
                return true;
            }
            m_overflow = std::make_unique<std::unordered_set<const Error*>>();
            m_overflow->reserve(kInlineCapacity * 2U);
            m_overflow->insert(m_inline, m_inline + m_inline_count);
        }
        return m_overflow->insert(node).second;
    }

 private:
    static constexpr std::size_t kInlineCapacity = 8; ///< 内联槽位数量。
    const Error* m_inline[kInlineCapacity] = {};      ///< 内联节点表。
    std::size_t m_inline_count = 0;                   ///< 已用槽位。
    std::unique_ptr<std::unordered_set<const Error*>> m_overflow; ///< 溢出后的哈希表。
};

template <typename T>
std::shared_ptr<const T> FindFirstError(const ErrorPtr& root) noexcept {
    static_assert(std::is_base_of<Error, T>::value, "T 必须派生自 bento::error::Error");
    try {
        if (!root) {
            return {};
        }
        std::vector<ErrorPtr> pending;
        pending.push_back(root);
        VisitSet visited;
        while (!pending.empty()) {
            ErrorPtr current = std::move(pending.back());
            pending.pop_back();
            if (!current || !visited.Insert(current.get())) {
                continue;
            }
            if (const auto found = std::dynamic_pointer_cast<const T>(current)) {
                return found;
            }
            if (!current->IsWrapping()) {
                continue;
            }
            const std::vector<ErrorPtr> children = current->UnwrapAll();
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (*it) {
                    pending.push_back(*it);
                }
            }
        }
    } catch (...) {
        // 查询接口是 noexcept：自定义错误抛出的异常统一转换为“未匹配”。
    }
    return {};
}

} // namespace detail

/// @brief 沿错误图找第一个动态类型为 `T` 的节点；未找到返回空。
template <typename T>
std::shared_ptr<const T> As(const ErrorPtr& error) noexcept {
    return detail::FindFirstError<T>(error);
}

/// @brief 错误图中是否存在动态类型为 `T` 的节点。
template <typename T>
bool Has(const ErrorPtr& error) noexcept {
    return static_cast<bool>(detail::FindFirstError<T>(error));
}

} // namespace go2cpp::bento::error
