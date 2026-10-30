#pragma once

/**
 * @file error.hpp
 * @brief 带类型链的不可变错误体系：错误码、类型反射、包装/聚合与图查询。
 *
 * @details 本文件是 `go2cpp::error` 的核心，参考 csvp 的 `csvp::Error`
 *          设计并入 Go2Cpp：
 *
 * - **类型链**：每个错误类型自报 `TypeName()` / `ParentTypeName()` /
 *   `InheritanceDepth()`，`Is()` 沿类型链判断“是不是某种错误”；
 *   这与指针身份判断（`go2cpp::Is`）是两件事；
 * - **不可变值语义**：错误以 `ErrorPtr = shared_ptr<const Error>` 传递，
 *   构造后不再修改，可跨线程/Fiber 共享；
 * - **错误图**：单原因由 `WrappedError` 表达，多原因由 `JoinError` 表达，
 *   因此错误实际构成有向无环图；所有图查询都用地址去重，遇到自指图
 *   （自定义 `UnwrapAll()` 造成的环）不会死循环；
 * - **零外部依赖**：只依赖标准库，可被任意模块（含 Bento）单独 include。
 *
 * ## 与旧错误体系的关系
 *
 * 项目已有的 `go2cpp::Error`（`go2cpp/tools/error.hpp`）是 Go `error`
 * 接口的等价物，只承载文本与原因；本文件是**新增**的类型链层，两者通过
 * `go2cpp/error/compat.hpp` 的 `ToLegacy()` / `FromLegacy()` 互转，
 * 不改动旧接口的任何语义。
 *
 * ## 类型链的写法
 *
 * 内置类型全部用 `GO2CPP_ERROR_TYPE` 生成反射函数；自定义类型照抄一行宏
 * 即可获得祖先链：
 *
 * ```cpp
 * class MyError final : public go2cpp::error::IOError {
 *  public:
 *     GO2CPP_ERROR_TYPE("demo.MyError", go2cpp::error::IOError)
 *     MyError() = default;
 *     // Message()/Code() 仍由派生类实现（IOError 已提供时无需重复）
 * };
 * ```
 *
 * 宏生成的是**编译期**父链（`ParentType::StaticIsA`），因此 `Is()` 不需要
 * 任何运行期注册表即可工作；`Registry` 只负责“名字 → 工厂/元数据”的
 * 反射查询，两者不重复存同一事实。
 *
 * ## 内存与并发
 *
 * 错误对象不可变，`Message()` / `Code()` / `Is()` 等只读方法可并发调用。
 * `WrappedError` / `JoinError` 的析构通过线程本地工作表迭代展开，十万层
 * 的深链析构不会爆栈（见 `detail::DestroyPending()`）。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/error/code.hpp"

#include <algorithm>
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

namespace go2cpp::error {

/// @brief 根类型名；所有错误类型的父链最终落到这里。
inline constexpr std::string_view kErrorRootTypeName = "go2cpp.Error";

/// @brief 允许的最大继承层数：根类型为 0，直接派生为 1。
/// @details 与参考实现 csvp 的 `kMaxErrorDepth` 一致；`Registry` 注册时
///          用该值拒绝异常深的反射链，避免 `Is()` 的字符串比较退化成
///          无界循环。
inline constexpr std::uint8_t kMaxErrorDepth = 32;

class Error;
/// @brief 不可变错误指针；所有接口都以它传递失败。
using ErrorPtr = std::shared_ptr<const Error>;

/**
 * @brief 错误节点接口（类型链版本）。
 *
 * @details 派生类必须实现 `Message()`、`Code()`、`TypeName()`、
 *          `ParentTypeName()`、`InheritanceDepth()`；有子节点的类型覆写
 *          `IsWrapping()` / `UnwrapAll()` / `Unwrap()`。
 *
 *          默认 `Is(target)` 实现为 `IsA(target.TypeName())`，即“目标的
 *          类型名是否在本对象的祖先链上”；根类型 `go2cpp.Error` 对任何
 *          错误都返回 true。
 *
 * @note 本接口的 `UnwrapAll(level)` 约定：`level == 0` 返回全部后代
 *       （去重、环安全），`level != 0` 只返回直接原因。默认参数为 1。
 */
class Error {
 public:
    virtual ~Error() = default;

    /// @brief 面向用户的错误文本。
    virtual std::string Message() const = 0;
    /// @brief 错误码。
    virtual ErrorCode Code() const noexcept = 0;
    /// @brief 运行期类型名；由 `GO2CPP_ERROR_TYPE` 或手写实现提供。
    virtual std::string_view TypeName() const noexcept = 0;
    /// @brief 父类型名；直接派生自根类型的错误返回 `"go2cpp.Error"`。
    virtual std::string_view ParentTypeName() const noexcept = 0;
    /// @brief 继承深度；根类型为 0，直接派生为 1。
    virtual std::uint8_t InheritanceDepth() const noexcept = 0;

    /**
     * @brief 本对象是否属于 `type_name` 类型（自身或其后代）。
     * @param type_name 类型名，例如 `"go2cpp.IOError"`。
     * @return 类型链包含该名字返回 true。
     * @note 宏生成的实现走编译期父链；手写类型若不覆写，默认只匹配自身
     *       的 `TypeName()`（不会误判为祖先）。
     */
    virtual bool IsA(std::string_view type_name) const noexcept {
        return TypeName() == type_name;
    }

    /// @brief 是否可能包含子节点；默认 false。
    virtual bool IsWrapping() const noexcept { return false; }

    /// @brief 返回直接包装的原因；无原因时返回空指针。
    virtual ErrorPtr Unwrap() const { return {}; }

    /**
     * @brief 返回直接原因（`level != 0`）或全部后代（`level == 0`）。
     * @param level 0 = 全部后代（去重、环安全）；其他值 = 直接原因。
     */
    virtual std::vector<ErrorPtr> UnwrapAll(std::size_t level = 1) const {
        (void)level;
        return {};
    }

    /**
     * @brief 类型链身份判断：`target` 的类型名是否在本对象祖先链上。
     * @note 与旧体系 `go2cpp::Is(error, target)`（指针身份）语义不同：
     *       这里判的是“是不是某种错误”，不是“是不是同一条错误”。
     */
    virtual bool Is(const Error& target) const noexcept {
        return IsA(target.TypeName());
    }

    /// @brief Go 风格文本别名，便于调用方统一按 `ErrorString()` 取值。
    std::string ErrorString() const { return Message(); }

    /// @brief 根类型的静态元数据；宏生成的父链最终递归到这里。
    static constexpr std::string_view ErrorTypeName() noexcept {
        return kErrorRootTypeName;
    }

    /// @brief 根类型深度恒为 0。
    static constexpr std::uint8_t ErrorDepth() noexcept { return 0; }

    /// @brief 根类型的静态类型判断：只有根类型名匹配。
    static bool StaticIsA(std::string_view type_name) noexcept {
        return type_name == kErrorRootTypeName;
    }
};

/**
 * @brief 为派生类生成类型链所需的静态元数据与虚函数覆写。
 *
 * @param TypeNameValue 反射类型名（字符串字面量），例如 `"demo.MyError"`。
 * @param ParentType    直接基类的类型名（不带引号），例如
 *                      `go2cpp::error::IOError`。
 *
 * @note 宏应放在类的 `public:` 区域。只生成 `TypeName` / `ParentTypeName` /
 *       `InheritanceDepth` / `IsA` 与三个静态元数据函数；`Message()` 与
 *       `Code()` 仍由派生类（或其基类）提供。
 */
#define GO2CPP_ERROR_TYPE(TypeNameValue, ParentType)                                               \
    static constexpr std::string_view ErrorTypeName() noexcept { return (TypeNameValue); }         \
    static constexpr std::string_view ErrorParentTypeName() noexcept {                             \
        return ParentType::ErrorTypeName();                                                        \
    }                                                                                              \
    static constexpr std::uint8_t ErrorDepth() noexcept {                                          \
        return static_cast<std::uint8_t>(ParentType::ErrorDepth() + 1U);                           \
    }                                                                                              \
    static bool StaticIsA(std::string_view type_name) noexcept {                                   \
        return type_name == ErrorTypeName() || ParentType::StaticIsA(type_name);                   \
    }                                                                                              \
    std::string_view TypeName() const noexcept override { return ErrorTypeName(); }                \
    std::string_view ParentTypeName() const noexcept override {                                    \
        return ErrorParentTypeName();                                                              \
    }                                                                                              \
    std::uint8_t InheritanceDepth() const noexcept override { return ErrorDepth(); }               \
    bool IsA(std::string_view type_name) const noexcept override {                                 \
        return StaticIsA(type_name);                                                               \
    }

// ---------------------------------------------------------------------------
// 常用错误类型
// ---------------------------------------------------------------------------

/// @brief 叶子错误：只有文本与错误码（`NewError()` 的产物）。
class StringError final : public Error {
 public:
    GO2CPP_ERROR_TYPE("go2cpp.StringError", Error)

    StringError() = default;

    /**
     * @brief 创建不可变文本错误。
     * @param message 错误文本，可为空。
     * @param code 错误码，默认 `kUnknown`；业务私有码用 `static_cast` 传入。
     */
    explicit StringError(std::string message, ErrorCode code = ErrorCode::kUnknown)
        : m_message(std::move(message)), m_code(code) {}

    std::string Message() const override { return m_message; }
    ErrorCode Code() const noexcept override { return m_code; }

 private:
    std::string m_message;                          ///< 不可变错误文本。
    ErrorCode m_code = ErrorCode::kUnknown;         ///< 错误码。
};

/// @brief 参数错误：`kInvalidArgument`，可附带出错的参数名。
class ArgumentError final : public Error {
 public:
    GO2CPP_ERROR_TYPE("go2cpp.ArgumentError", Error)

    ArgumentError() = default;

    /**
     * @brief 创建参数错误。
     * @param message 错误说明。
     * @param argument 出错的参数名；非空时 `Message()` 追加 `[argument=...]`。
     */
    explicit ArgumentError(std::string message, std::string argument = {})
        : m_message(std::move(message)), m_argument(std::move(argument)) {}

    std::string Message() const override {
        if (m_argument.empty()) {
            return m_message;
        }
        std::string result = m_message;
        result += " [argument=";
        result += m_argument;
        result += ']';
        return result;
    }
    ErrorCode Code() const noexcept override { return ErrorCode::kInvalidArgument; }

    /// @brief 出错的参数名；未提供时为空。
    const std::string& Argument() const noexcept { return m_argument; }

 private:
    std::string m_message;  ///< 错误说明。
    std::string m_argument; ///< 参数名；构造时设置。
};

/**
 * @brief I/O 错误：读写失败、设备不可用等，默认码 `kUnavailable`。
 *
 * @details 作为 `TimeoutError` / `NotFoundError` / `PermissionError` 的
 *          基类提供路径上下文；子类通过构造函数指定更精确的错误码。
 */
class IOError : public Error {
 public:
    GO2CPP_ERROR_TYPE("go2cpp.IOError", Error)

    IOError() = default;

    /**
     * @brief 创建 I/O 错误。
     * @param message 错误说明。
     * @param path 关联路径；非空时 `Message()` 追加 `[path=...]`。
     * @param code 错误码；默认 `kUnavailable`（重试可能成功）。
     */
    explicit IOError(std::string message, std::string path = {},
                     ErrorCode code = ErrorCode::kUnavailable)
        : m_message(std::move(message)), m_path(std::move(path)), m_code(code) {}

    std::string Message() const override {
        if (m_path.empty()) {
            return m_message;
        }
        std::string result = m_message;
        result += " [path=";
        result += m_path;
        result += ']';
        return result;
    }
    ErrorCode Code() const noexcept override { return m_code; }

    /// @brief 关联路径；未提供时为空。
    const std::string& Path() const noexcept { return m_path; }

 private:
    std::string m_message;                     ///< 错误说明。
    std::string m_path;                        ///< 关联路径。
    ErrorCode m_code = ErrorCode::kUnavailable; ///< 错误码。
};

/// @brief 超时错误：`kDeadlineExceeded`，是 I/O 错误的特化。
class TimeoutError final : public IOError {
 public:
    GO2CPP_ERROR_TYPE("go2cpp.TimeoutError", IOError)

    TimeoutError() : IOError(std::string(), std::string(), ErrorCode::kDeadlineExceeded) {}

    /**
     * @brief 创建超时错误。
     * @param message 错误说明。
     * @param path 关联路径。
     * @param timeout_ms 超时阈值（毫秒）；0 表示未记录。
     */
    explicit TimeoutError(std::string message, std::string path = {}, std::uint64_t timeout_ms = 0)
        : IOError(std::move(message), std::move(path), ErrorCode::kDeadlineExceeded),
          m_timeout_ms(timeout_ms) {}

    /// @brief 超时阈值（毫秒）；0 表示未记录。
    std::uint64_t TimeoutMs() const noexcept { return m_timeout_ms; }

 private:
    std::uint64_t m_timeout_ms = 0; ///< 超时阈值。
};

/// @brief 资源不存在：`kNotFound`，是 I/O 错误的特化。
class NotFoundError : public IOError {
 public:
    GO2CPP_ERROR_TYPE("go2cpp.NotFoundError", IOError)

    NotFoundError() : IOError(std::string(), std::string(), ErrorCode::kNotFound) {}

    /**
     * @brief 创建“不存在”错误。
     * @param message 错误说明。
     * @param path 关联的资源名/路径。
     */
    explicit NotFoundError(std::string message, std::string path = {})
        : IOError(std::move(message), std::move(path), ErrorCode::kNotFound) {}
};

/// @brief 文件不存在：`kNotFound`，是“不存在”错误的特化（深度 3）。
class FileNotFoundError final : public NotFoundError {
 public:
    GO2CPP_ERROR_TYPE("go2cpp.FileNotFoundError", NotFoundError)

    FileNotFoundError() = default;

    /**
     * @brief 创建文件不存在错误。
     * @param message 错误说明。
     * @param path 文件路径。
     */
    explicit FileNotFoundError(std::string message, std::string path = {})
        : NotFoundError(std::move(message), std::move(path)) {}
};

/// @brief 权限不足：`kPermissionDenied`，是 I/O 错误的特化。
class PermissionError final : public IOError {
 public:
    GO2CPP_ERROR_TYPE("go2cpp.PermissionError", IOError)

    PermissionError() : IOError(std::string(), std::string(), ErrorCode::kPermissionDenied) {}

    /**
     * @brief 创建权限错误。
     * @param message 错误说明。
     * @param path 关联路径。
     */
    explicit PermissionError(std::string message, std::string path = {})
        : IOError(std::move(message), std::move(path), ErrorCode::kPermissionDenied) {}
};

namespace detail {

/// @brief 深链析构的线程本地工作表；见 `WrappedError::~WrappedError`。
inline thread_local std::vector<ErrorPtr> t_destroy_pending;

/// @brief 是否已有析构循环在处理中（重入标记）。
inline thread_local bool t_destroy_draining = false;

/**
 * @brief 把待释放节点放入工作表并迭代释放，避免深链递归析构爆栈。
 *
 * @details 模式：第一个进入的析构函数开启 `draining`，之后重入的
 *          `WrappedError`/`JoinError` 析构只把子节点推进工作表就返回；
 *          外层循环逐个 `reset()`，每层释放都是常数栈深。析构函数不能
 *          抛异常，`push_back` 的 `bad_alloc` 被吞掉并放弃进一步展开
 *          （此时仍会正常释放已入表节点，只是可能退回少量递归）。
 */
inline void DrainDestroyQueue() noexcept {
    while (!t_destroy_pending.empty()) {
        ErrorPtr node = std::move(t_destroy_pending.back());
        t_destroy_pending.pop_back();
        node.reset();
    }
}

} // namespace detail

/**
 * @brief 单原因包装：`Message()` 展平连续包装为 `"a: b: c"`。
 *
 * @details 展平只沿 `WrappedError` 链进行，链尾若是普通错误则把它的
 *          `Message()` 作为最后一段追加；空片段被跳过，不会产生多余的
 *          `": "`。包装链由构造时给定，不可变，因此天然无环。
 */
class WrappedError final : public Error {
 public:
    GO2CPP_ERROR_TYPE("go2cpp.WrappedError", Error)

    WrappedError() = default;

    /**
     * @brief 创建包装错误。
     * @param message 当前层描述。
     * @param cause 被包装的原因；为空的包装由 `Wrap()` 拒绝创建。
     */
    WrappedError(std::string message, ErrorPtr cause)
        : m_message(std::move(message)), m_cause(std::move(cause)) {}

    ~WrappedError() override {
        if (!m_cause) {
            return;
        }
        if (detail::t_destroy_draining) {
            try {
                detail::t_destroy_pending.push_back(std::move(m_cause));
            } catch (...) {
            }
            return;
        }
        detail::t_destroy_draining = true;
        try {
            detail::t_destroy_pending.push_back(std::move(m_cause));
            detail::DrainDestroyQueue();
        } catch (...) {
        }
        detail::t_destroy_draining = false;
    }

    WrappedError(const WrappedError&) = delete;
    WrappedError& operator=(const WrappedError&) = delete;

    std::string Message() const override {
        std::string result;
        const auto append = [&result](std::string_view part) {
            if (part.empty()) {
                return;
            }
            if (!result.empty()) {
                result += ": ";
            }
            result += part;
        };
        append(m_message);
        const Error* node = m_cause.get();
        std::size_t depth = 0;
        while (node != nullptr) {
            const auto* wrapped = dynamic_cast<const WrappedError*>(node);
            if (wrapped == nullptr) {
                break;
            }
            append(wrapped->m_message);
            node = wrapped->m_cause.get();
            if (++depth >= kMaxErrorDepth) {
                // 异常深的链只展平前 kMaxErrorDepth 层：再往下就退化为叶子
                // 文本，避免一条日志吃掉不可控的时间与内存。
                break;
            }
        }
        if (node != nullptr) {
            append(node->Message());
        }
        return result;
    }

    ErrorCode Code() const noexcept override { return ErrorCode::kWrappedError; }
    bool IsWrapping() const noexcept override { return true; }
    ErrorPtr Unwrap() const override { return m_cause; }

    std::vector<ErrorPtr> UnwrapAll(std::size_t level = 1) const override;

 private:
    std::string m_message; ///< 当前层描述。
    ErrorPtr m_cause;      ///< 被包装的直接原因。
};

/**
 * @brief 多原因聚合：`Message()` 用换行连接各原因。
 *
 * @details 构造时过滤空指针；嵌套 `JoinError` 的文本本身已含换行，
 *          直接拼接即可，不额外做结构展平。
 */
class JoinError final : public Error {
 public:
    GO2CPP_ERROR_TYPE("go2cpp.JoinError", Error)

    JoinError() = default;

    /// @brief 创建聚合错误；空指针原因被过滤。
    explicit JoinError(std::vector<ErrorPtr> causes) : m_causes(std::move(causes)) {
        m_causes.erase(std::remove(m_causes.begin(), m_causes.end(), nullptr), m_causes.end());
    }

    ~JoinError() override {
        if (m_causes.empty()) {
            return;
        }
        if (detail::t_destroy_draining) {
            try {
                for (auto& cause : m_causes) {
                    if (cause) {
                        detail::t_destroy_pending.push_back(std::move(cause));
                    }
                }
            } catch (...) {
            }
            return;
        }
        detail::t_destroy_draining = true;
        try {
            for (auto& cause : m_causes) {
                if (cause) {
                    detail::t_destroy_pending.push_back(std::move(cause));
                }
            }
            detail::DrainDestroyQueue();
        } catch (...) {
        }
        detail::t_destroy_draining = false;
    }

    JoinError(const JoinError&) = delete;
    JoinError& operator=(const JoinError&) = delete;

    std::string Message() const override {
        std::string result;
        for (const auto& cause : m_causes) {
            if (!cause) {
                continue;
            }
            const std::string text = cause->Message();
            if (text.empty()) {
                continue;
            }
            if (!result.empty()) {
                result += '\n';
            }
            result += text;
        }
        return result;
    }

    ErrorCode Code() const noexcept override { return ErrorCode::kWrappedError; }
    bool IsWrapping() const noexcept override { return true; }
    ErrorPtr Unwrap() const override { return m_causes.empty() ? ErrorPtr{} : m_causes.front(); }
    std::vector<ErrorPtr> UnwrapAll(std::size_t level = 1) const override;

 private:
    std::vector<ErrorPtr> m_causes; ///< 全部非空原因。
};

namespace detail {

/**
 * @brief 收集 `root` 的全部后代（不含 root 自身），地址去重、环安全。
 * @param root 起点；调用其 `UnwrapAll()`（直接原因）展开。
 * @return 先序（父在前、子在后）的后代列表。
 */
inline std::vector<ErrorPtr> CollectDescendants(const Error& root) {
    std::vector<ErrorPtr> result;
    std::vector<ErrorPtr> pending;
    const std::vector<ErrorPtr> causes = root.UnwrapAll();
    for (auto it = causes.rbegin(); it != causes.rend(); ++it) {
        if (*it) {
            pending.push_back(*it);
        }
    }
    std::unordered_set<const Error*> visited;
    while (!pending.empty()) {
        ErrorPtr node = std::move(pending.back());
        pending.pop_back();
        if (!node || !visited.insert(node.get()).second) {
            continue;
        }
        result.push_back(node);
        if (!node->IsWrapping()) {
            continue;
        }
        const std::vector<ErrorPtr> children = node->UnwrapAll();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (*it) {
                pending.push_back(*it);
            }
        }
    }
    return result;
}

} // namespace detail

inline std::vector<ErrorPtr> WrappedError::UnwrapAll(std::size_t level) const {
    if (level != 0) {
        if (!m_cause) {
            return {};
        }
        return {m_cause};
    }
    return detail::CollectDescendants(*this);
}

inline std::vector<ErrorPtr> JoinError::UnwrapAll(std::size_t level) const {
    if (level != 0) {
        return m_causes;
    }
    return detail::CollectDescendants(*this);
}

// ---------------------------------------------------------------------------
// 工厂
// ---------------------------------------------------------------------------

/// @brief 以 `const T` 分配错误对象；统一入口，保证错误值创建后不可变。
template <typename T, typename... Args>
std::shared_ptr<const T> MakeError(Args&&... args) {
    static_assert(std::is_base_of<Error, T>::value, "T 必须派生自 go2cpp::error::Error");
    return std::make_shared<const T>(std::forward<Args>(args)...);
}

/**
 * @brief 创建叶子文本错误。
 * @param message 错误文本，可为空。
 * @param code 错误码。
 */
inline ErrorPtr NewError(std::string message, ErrorCode code = ErrorCode::kUnknown) {
    return MakeError<StringError>(std::move(message), code);
}

/**
 * @brief 在 `cause` 外包装一层描述。
 * @param cause 原因；为空时返回空指针（不静默丢弃上下文之外的语义）。
 * @param message 当前层描述，可为空（此时文本等价于原因文本）。
 */
inline ErrorPtr Wrap(ErrorPtr cause, std::string message) {
    if (!cause) {
        return nullptr;
    }
    return MakeError<WrappedError>(std::move(message), std::move(cause));
}

/// @brief 聚合多个原因；过滤空指针，全部为空时返回空指针。
inline ErrorPtr Join(std::vector<ErrorPtr> causes) {
    std::vector<ErrorPtr> filtered;
    filtered.reserve(causes.size());
    for (auto& cause : causes) {
        if (cause) {
            filtered.push_back(std::move(cause));
        }
    }
    if (filtered.empty()) {
        return nullptr;
    }
    return MakeError<JoinError>(std::move(filtered));
}

/// @brief 聚合多个原因（initializer_list 便捷重载）。
inline ErrorPtr Join(std::initializer_list<ErrorPtr> causes) {
    return Join(std::vector<ErrorPtr>(causes.begin(), causes.end()));
}

/// @brief 是否为非空包装节点。
inline bool IsWrappedError(const ErrorPtr& error) noexcept {
    return error != nullptr && error->IsWrapping();
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

/// @brief 错误文本；空指针返回空串。
inline std::string ErrorMessage(const ErrorPtr& error) {
    return error ? error->Message() : std::string();
}

/// @brief 错误码；空指针返回 `kOk`（与“无错误”语义一致）。
inline ErrorCode ErrorCodeOf(const ErrorPtr& error) noexcept {
    return error ? error->Code() : ErrorCode::kOk;
}

/// @brief 类型名是否在 `error` 的类型链上；空指针返回 false。
inline bool IsA(const ErrorPtr& error, std::string_view type_name) noexcept {
    return error && error->IsA(type_name);
}

/// @brief `error` 是否为根类型（任何非空错误都满足）。
inline bool IsErrorRoot(const ErrorPtr& error) noexcept {
    return error && error->IsA(kErrorRootTypeName);
}

/// @brief 完整类型链匹配：`error` 是否属于 `ancestor` 或其后代。
inline bool Is(const ErrorPtr& error, const Error& ancestor) noexcept {
    return error && error->Is(ancestor);
}

/// @brief 便捷入口：`error ? error->UnwrapAll(level) : {}`。
inline std::vector<ErrorPtr> UnwrapAll(const ErrorPtr& error, std::size_t level = 1) {
    return error ? error->UnwrapAll(level) : std::vector<ErrorPtr>{};
}

namespace detail {

template <typename T>
std::shared_ptr<const T> FindFirstError(const ErrorPtr& root) noexcept {
    using Plain = std::remove_const_t<T>;
    static_assert(std::is_base_of<Error, Plain>::value, "T 必须派生自 go2cpp::error::Error");
    try {
        if (!root) {
            return {};
        }
        std::vector<ErrorPtr> pending;
        pending.push_back(root);
        std::unordered_set<const Error*> visited;
        while (!pending.empty()) {
            ErrorPtr node = std::move(pending.back());
            pending.pop_back();
            if (!node || !visited.insert(node.get()).second) {
                continue;
            }
            if (const auto found = std::dynamic_pointer_cast<const Plain>(node)) {
                return found;
            }
            if (!node->IsWrapping()) {
                continue;
            }
            const std::vector<ErrorPtr> children = node->UnwrapAll();
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

/// @brief 错误图中是否存在指定错误码的节点（含自身）；环安全、异常安全。
inline bool HasErrorCode(const ErrorPtr& error, ErrorCode code) noexcept {
    if (!error) {
        return false;
    }
    try {
        std::vector<ErrorPtr> pending;
        pending.push_back(error);
        std::unordered_set<const Error*> visited;
        while (!pending.empty()) {
            ErrorPtr node = std::move(pending.back());
            pending.pop_back();
            if (!node || !visited.insert(node.get()).second) {
                continue;
            }
            if (node->Code() == code) {
                return true;
            }
            if (!node->IsWrapping()) {
                continue;
            }
            const std::vector<ErrorPtr> children = node->UnwrapAll();
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (*it) {
                    pending.push_back(*it);
                }
            }
        }
    } catch (...) {
    }
    return false;
}

// ---------------------------------------------------------------------------
// 遍历
// ---------------------------------------------------------------------------

/// @brief 遍历错误图时访问者对“当前节点”的处理决定。
enum class ErrorVisitResult : std::uint8_t {
    kContinue = 0,     ///< 继续访问当前节点的子节点。
    kSkipChildren = 1, ///< 不展开当前节点，继续兄弟节点。
    kStop = 2,         ///< 立即结束遍历。
};

using ErrorVisitor = std::function<ErrorVisitResult(const ErrorPtr&)>;

/// @brief `WalkErrors` 的结果快照。
struct WalkResult {
    std::vector<ErrorPtr> errors;                          ///< 已访问节点（前序）。
    ErrorVisitResult status = ErrorVisitResult::kContinue; ///< 结束原因。
    bool ok = true;                                        ///< false = 访问器为空或抛异常。
};

/**
 * @brief 前序深度优先遍历错误图。
 * @param error 起点；空指针返回空结果（`ok == true`）。
 * @param visitor 访问器；为空时 `ok == false`。
 * @return 访问记录；visitor 抛异常时保留已访问部分并置 `ok = false`。
 * @note 每个对象最多访问一次，自指图不会死循环。
 */
inline WalkResult WalkErrors(const ErrorPtr& error, const ErrorVisitor& visitor) noexcept {
    WalkResult result;
    if (!visitor) {
        result.ok = false;
        return result;
    }
    if (!error) {
        return result;
    }
    try {
        std::vector<ErrorPtr> pending;
        pending.push_back(error);
        std::unordered_set<const Error*> visited;
        while (!pending.empty()) {
            ErrorPtr node = std::move(pending.back());
            pending.pop_back();
            if (!node || !visited.insert(node.get()).second) {
                continue;
            }
            result.errors.push_back(node);
            const ErrorVisitResult decision = visitor(node);
            if (decision == ErrorVisitResult::kStop) {
                result.status = ErrorVisitResult::kStop;
                return result;
            }
            if (decision == ErrorVisitResult::kSkipChildren || !node->IsWrapping()) {
                continue;
            }
            const std::vector<ErrorPtr> children = node->UnwrapAll();
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (*it) {
                    pending.push_back(*it);
                }
            }
        }
    } catch (...) {
        result.ok = false;
    }
    return result;
}

namespace detail {

/// @brief 树形文本的递归深度上限，防止异常深的错误链撑爆字符串。
inline constexpr std::size_t kMaxTreeDepth = 64;

inline void AppendTreeLine(std::string& out, const ErrorPtr& error, std::size_t depth,
                           std::unordered_set<const Error*>& visited) {
    if (!error || depth > kMaxTreeDepth) {
        return;
    }
    if (!visited.insert(error.get()).second) {
        return;
    }
    for (std::size_t index = 0; index < depth; ++index) {
        out += "  ";
    }
    if (depth != 0) {
        out += "- ";
    }
    out += error->TypeName();
    out += ": ";
    // 多行消息压平成 " / "，保证树形对齐。
    const std::string message = error->Message();
    for (const char ch : message) {
        if (ch == '\n') {
            out += " / ";
        } else {
            out += ch;
        }
    }
    out += '\n';
    if (!error->IsWrapping()) {
        return;
    }
    const std::vector<ErrorPtr> children = error->UnwrapAll();
    for (const auto& child : children) {
        AppendTreeLine(out, child, depth + 1, visited);
    }
}

} // namespace detail

/**
 * @brief 把错误图格式化成树形文本。
 * @param error 根节点；空指针返回 `"<null>"`。
 * @return 每行一个节点，子节点缩进两格；多行消息压平为 `" / "`。
 */
inline std::string ErrorTreeString(const ErrorPtr& error) {
    if (!error) {
        return "<null>";
    }
    std::string out;
    std::unordered_set<const Error*> visited;
    detail::AppendTreeLine(out, error, 0, visited);
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

} // namespace go2cpp::error
