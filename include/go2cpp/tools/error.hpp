/**
 * @file error.hpp
 * @brief 不可变错误值、错误链/错误图遍历，以及 Go `errors` 风格的薄封装。
 *
 * ## 模块定位
 *
 * 全项目的错误基础设施：所有模块用 `ErrorPtr`（`shared_ptr<const Error>`）
 * 返回失败，用 `Is()`/`As()`/`WalkErrors()` 做身份、类型与全图判断。它位于
 * 最底层，只依赖标准库；`bufio`、`encoding`、`compress`、`os`、`network`
 * 等都建立在它之上。
 *
 * ## 本文件定义什么
 *
 * - `Error` / `ErrorPtr`：不可变错误节点接口与共享指针；
 * - `StringError` / `WrappedError` / `JoinError`：叶子、单原因包装、多原因聚合；
 * - `WalkErrors` / `Is` / `As` / `Unwrap`：环安全的遍历与查询；
 * - `NewError` / `Wrap` / `Join`：工厂；
 * - `errors` 命名空间：Go 风格别名（`errors::New`/`Wrap`/`Is`/`As`）。
 *
 * ## 阅读建议
 *
 * 先读 `Error` 的不可变契约与 `WrappedError::Message` 的展平规则，再看
 * `Is`/`As`/`WalkErrors` 如何用地址去重处理错误图（而不是错误链）。
 *
 * ## 边界与陷阱
 *
 * `ErrorPtr` 是 `shared_ptr<const Error>`，错误对象构造后不可修改；`AsMutable`
 * 只是显式适配出口，共享时修改并不安全。深链/深 Join 的析构与文本展开都用
 * 迭代而不是递归，避免爆栈。
 */
/**
 * @file error.hpp
 * @brief 不可变错误值、错误链/错误图遍历，以及 Go `errors` 风格的薄封装。
 *
 * ## 模块定位
 *
 * 全项目的错误基础设施：所有模块用 `ErrorPtr`（`shared_ptr<const Error>`）
 * 返回失败，用 `Is()`/`As()`/`WalkErrors()` 做身份、类型与全图判断。它位于
 * 最底层，只依赖标准库；`bufio`、`encoding`、`compress`、`os`、`network`
 * 等都建立在它之上。
 *
 * ## 本文件定义什么
 *
 * - `Error` / `ErrorPtr`：不可变错误节点接口与共享指针；
 * - `StringError` / `WrappedError` / `JoinError`：叶子、单原因包装、多原因聚合；
 * - `WalkErrors` / `Is` / `As` / `Unwrap`：环安全的遍历与查询；
 * - `NewError` / `Wrap` / `Join`：工厂；
 * - `errors` 命名空间：Go 风格别名（`errors::New`/`Wrap`/`Is`/`As`）。
 *
 * ## 阅读建议
 *
 * 先读 `Error` 的不可变契约与 `WrappedError::Message` 的展平规则，再看
 * `Is`/`As`/`WalkErrors` 如何用地址去重处理错误图（而不是错误链）。
 *
 * ## 边界与陷阱
 *
 * `ErrorPtr` 是 `shared_ptr<const Error>`，错误对象构造后不可修改；`AsMutable`
 * 只是显式适配出口，共享时修改并不安全。深链/深 Join 的析构与文本展开都用
 * 迭代而不是递归，避免爆栈。
 */
#pragma once

#include <cstdint>
#include <initializer_list>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace go2cpp {

/**
 * @brief 兼容层使用的不可变错误值（Go `error` 接口的等价物）。
 *
 * ## 定位
 * 全项目错误体系的根接口。各模块只通过 `ErrorPtr` 暴露失败；上层用 `Is`/
 * `As`/`WalkErrors` 判断身份与类型。
 *
 * ## 生命周期与线程模型
 * 实现必须不可变；`ErrorPtr` 可安全地在线程/Fiber 之间共享。只读方法
 * （`Message`/`Unwrap`/`UnwrapAll`/`Is`）应允许并发调用。
 *
 * ## 不变量
 * - 对象一经构造就不再修改（包括 `Unwrap()` 返回的原因指针）；
 * - 无原因时 `Unwrap()` 返回空；`UnwrapAll()` 默认把单原因包装成一项；
 * - `Is()` 默认按对象地址匹配，自定义错误可覆写。
 */
class Error {
 public:
 
  /** @brief 虚析构，允许通过 ErrorPtr 安全释放。 */
  virtual ~Error() = default;
  /** @brief 返回面向用户的错误文本。 */
  virtual std::string Message() const = 0;
  // 接近 Go 的拼写，便于调用方使用 Error() 风格接口。
  /** @brief Go 风格 Error() 文本别名。 */
  virtual std::string ErrorString() const { return Message(); }
  /** @brief 返回直接包装的原因；无原因时返回空指针。 */
  virtual std::shared_ptr<const Error> Unwrap() const { return {}; }
  /** @brief 返回全部直接原因，JoinError 可包含多个。 */
  virtual std::vector<std::shared_ptr<const Error>> UnwrapAll() const;
  // 自定义错误可以重写此函数，实现 Go 的 Is 判断。
  /** @brief 判断当前错误是否匹配目标错误。 */
  virtual bool Is(const Error& target) const noexcept {
    return this == &target;
  }
};

using ErrorPtr = std::shared_ptr<const Error>;

/**
 * @brief 错误图遍历回调的返回值。
 * @details Error 既可以形成单链，也可以通过 JoinError 形成有向图；遍历
 *          使用地址去重，因此自定义错误中的环不会导致无限循环。
 *
 * ## 定位
 * `WalkErrors` 的回调协议；日志、指标、错误转换器等插件用它决定是否继续遍历。
 */
enum class ErrorVisitResult : std::uint8_t {
  /** 继续访问尚未访问的原因。 */
  kContinue,
  /** 停止访问并立即返回。 */
  kStop,
};

using ErrorVisitor =
    std::function<ErrorVisitResult(const ErrorPtr& error)>;

/**
 * @brief 迭代遍历错误及其全部原因。
 * @param error 遍历起点；空指针不会调用 visitor。
 * @param visitor 访问器；每个错误对象最多访问一次。
 * @return visitor 全部返回 kContinue 且未发生异常时返回 true；访问器
 *          返回 kStop 或抛出异常时返回 false。
 * @note 该函数是 Is/As 之外的插件扩展出口，日志、指标和错误转换器可以
 *       复用统一的环安全遍历规则，不需要依赖 WrappedError/JoinError。
 */
bool WalkErrors(const ErrorPtr& error, const ErrorVisitor& visitor) noexcept;

/**
 * @brief 只保存一条不可变文本的叶子错误。
 *
 * ## 定位
 * `NewError()` 的产物，是最常用的错误实现；适合直接返回给上层。
 *
 * ## 不变量
 * `m_message` 构造后不再改变；`Unwrap()` 恒为空。
 */
class StringError final : public Error {
 public:
  /** @brief 创建不可变文本错误。 */
  explicit StringError(std::string message) : m_message(std::move(message)) {}
  /** @brief 返回保存的错误文本。 */
  std::string Message() const override { return m_message; }

 private:
  std::string m_message; ///< 不可变错误文本；构造时设置，Message() 返回
};

/**
 * @brief 带单一 cause 的包装错误；Message 返回当前说明，Unwrap 返回原始原因。
 *
 * ## 定位
 * `Wrap(cause, message)` 的产物，用于给底层错误补充上下文；被各模块的
 * 错误转换路径广泛使用。
 *
 * ## 不变量
 * - `m_cause` 非空（`Wrap` 对空 cause 直接返回空）；
 * - `Message()` 把同类型包装链展平成 `a: b: c`，不递归；
 * - 析构通过线程本地工作表迭代释放，避免深链递归爆栈。
 */
class WrappedError final : public Error {
 public:
  /** @brief 创建带原因的包装错误。@param message 当前层描述。@param cause 原因错误。 */
  WrappedError(std::string message, ErrorPtr cause)
      : m_message(std::move(message)), m_cause(std::move(cause)) {}
  ~WrappedError() override;

  /** @brief 返回包装层描述。 */
  std::string Message() const override;
  /** @brief 返回被包装的直接原因。 */
  ErrorPtr Unwrap() const override { return m_cause; }

 private:
  std::string m_message; ///< 当前层描述；构造时设置，Message() 展平链时读取
  ErrorPtr m_cause;      ///< 被包装的直接原因；构造时设置，Unwrap()/析构读取
};

/**
 * @brief 聚合多个原因的错误；UnwrapAll 返回全部非空原因，供 Is/As 遍历。
 *
 * ## 定位
 * `Join()` 的产物，用于并行任务、批量操作与多错误汇总；把错误链扩展成错误图。
 *
 * ## 不变量
 * - `m_causes` 只含非空指针；
 * - `Message()` 用换行连接各原因，嵌套 `JoinError` 会被展开而不是递归；
 * - 析构同样用工作表迭代释放，避免深图递归爆栈。
 */
class JoinError final : public Error {
 public:
  /** @brief 创建包含多个原因的聚合错误。 */
  explicit JoinError(std::vector<ErrorPtr> causes);
  ~JoinError() override;

  /** @brief 返回聚合错误文本。 */
  std::string Message() const override;
  /** @brief 返回全部被聚合原因。 */
  std::vector<ErrorPtr> UnwrapAll() const override { return m_causes; }

 private:
  std::vector<ErrorPtr> m_causes; ///< 全部非空原因；构造时过滤并保存，UnwrapAll()/Message()/析构读取
};

/** @brief 创建文本错误。 */
ErrorPtr NewError(std::string message);
/** @brief 在 cause 外包装一层描述。 */
ErrorPtr Wrap(ErrorPtr cause, std::string message);
/** @brief 聚合多个原因错误。 */
ErrorPtr Join(std::vector<ErrorPtr> causes);
/** @brief 以初始化列表聚合多个原因。 */
ErrorPtr Join(std::initializer_list<ErrorPtr> causes);

/** @brief 返回错误的直接原因。 */
ErrorPtr Unwrap(const ErrorPtr& error) noexcept;
/** @brief 返回错误链中的全部原因。 */
std::vector<ErrorPtr> UnwrapAll(const ErrorPtr& error);
/** @brief 返回空安全的错误文本。 */
std::string ErrorMessage(const ErrorPtr& error);

/**
 * 遍历单原因或多原因错误链。允许并忽略环；空 target 只匹配空错误，
 * 与 errors.Is(err, nil) 相同。
 */
/** @brief 按错误链判断 error 是否匹配 target。 */
bool Is(const ErrorPtr& error, const ErrorPtr& target) noexcept;

namespace detail {

template <typename T>
std::shared_ptr<const T> AsImpl(const ErrorPtr& error,
                                std::unordered_set<const Error*>& visited) {
  static_assert(std::is_base_of<Error, T>::value,
                "As<T> requires T to derive from go2cpp::Error");
  if (!error) {
    return {};
  }
  std::vector<ErrorPtr> pending;
  pending.push_back(error);
  while (!pending.empty()) {
    ErrorPtr next = std::move(pending.back());
    pending.pop_back();
    if (!next || !visited.insert(next.get()).second) {
      continue;
    }
    if (const auto direct = std::dynamic_pointer_cast<const T>(next)) {
      return direct;
    }
    auto causes = next->UnwrapAll();
    for (auto it = causes.rbegin(); it != causes.rend(); ++it) {
      if (*it) {
        pending.push_back(*it);
      }
    }
  }
  return {};
}

}  // namespace detail

template <typename T>
std::shared_ptr<const T> As(const ErrorPtr& error) noexcept {
  try {
    std::unordered_set<const Error*> visited;
    return detail::AsImpl<T>(error, visited);
  } catch (...) {
    // Is/As 是查询型 noexcept API；扩展 Error 的解包、哈希容器分配失败
    // 都必须转换为“未匹配”，不能让异常穿过边界并触发 terminate。
    return {};
  }
}

template <typename T>
std::shared_ptr<T> AsMutable(const ErrorPtr& error) noexcept {
  // 显式适配出口：ErrorPtr 被共享时修改返回对象并不安全。普通调用方应
  // 使用 As<T>()，并保持错误值不可变。
  static_assert(std::is_base_of<Error, T>::value,
                "AsMutable<T> requires T to derive from go2cpp::Error");
  if (!error) {
    return {};
  }
  return std::const_pointer_cast<T>(As<T>(error));
}

inline bool errors_is(const ErrorPtr& error, const ErrorPtr& target) noexcept {
  return Is(error, target);
}

template <typename T>
std::shared_ptr<const T> errors_as(const ErrorPtr& error) noexcept {
  return As<T>(error);
}

/**
 * @brief Go 风格的错误函数命名空间。
 * @details 这些函数只是现有 NewError/Wrap/Join/Is/As 的薄封装，不改变错误
 *          对象的不可变和共享所有权约束。新手可以直接使用 errors::New、
 *          errors::Wrap 和 errors::Is；既有 CamelCase API 继续保留。
 */
namespace errors {

/** @brief 创建一条文本错误，等价于 NewError。 */
inline ErrorPtr New(std::string message) {
  return NewError(std::move(message));
}

/** @brief 在 cause 外增加一层描述，等价于 Wrap。 */
inline ErrorPtr Wrap(ErrorPtr cause, std::string message) {
  return go2cpp::Wrap(std::move(cause), std::move(message));
}

/** @brief 聚合多条错误，等价于 Join。 */
inline ErrorPtr Join(std::vector<ErrorPtr> causes) {
  return go2cpp::Join(std::move(causes));
}

/** @brief 以初始化列表聚合多条错误。 */
inline ErrorPtr Join(std::initializer_list<ErrorPtr> causes) {
  return go2cpp::Join(causes);
}

/** @brief 读取直接包装原因，等价于 Unwrap。 */
inline ErrorPtr Unwrap(const ErrorPtr& error) noexcept {
  return go2cpp::Unwrap(error);
}

/** @brief 判断错误链是否匹配目标，等价于 Is。 */
inline bool Is(const ErrorPtr& error, const ErrorPtr& target) noexcept {
  return go2cpp::Is(error, target);
}

/** @brief 按类型提取错误，等价于 As。 */
template <typename T>
inline std::shared_ptr<const T> As(const ErrorPtr& error) noexcept {
  return go2cpp::As<T>(error);
}

}  // namespace errors

}  // namespace go2cpp
