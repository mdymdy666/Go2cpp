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
 * 兼容层使用的不可变错误值。
 *
 * ErrorPtr 是可为空的 Go error 接口等价物。错误实现构造后必须保持不可变，
 * 因此共享 ErrorPtr 可以安全地在线程和 Fiber 之间传递。
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

/** 只保存一条不可变文本的叶子错误。依赖 Error 接口，适合直接返回给上层。 */
class StringError final : public Error {
 public:
  /** @brief 创建不可变文本错误。 */
  explicit StringError(std::string message) : m_message(std::move(message)) {}
  /** @brief 返回保存的错误文本。 */
  std::string Message() const override { return m_message; }

 private:
  std::string m_message;
};

/** 带单一 cause 的包装错误；Message 返回当前说明，Unwrap 返回原始原因。 */
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
  std::string m_message;
  ErrorPtr m_cause;
};

/** 聚合多个原因的错误；UnwrapAll 返回全部非空原因，供 Is/As 遍历。 */
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
  std::vector<ErrorPtr> m_causes;
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
                                std::unordered_set<const Error*>& visited) noexcept {
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
  std::unordered_set<const Error*> visited;
  return detail::AsImpl<T>(error, visited);
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

}  // namespace go2cpp
