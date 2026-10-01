#pragma once

#include <initializer_list>
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
  virtual ~Error() = default;
  virtual std::string Message() const = 0;
  // 接近 Go 的拼写，便于调用方使用 Error() 风格接口。
  virtual std::string ErrorString() const { return Message(); }
  virtual std::shared_ptr<const Error> Unwrap() const { return {}; }
  virtual std::vector<std::shared_ptr<const Error>> UnwrapAll() const;
  // 自定义错误可以重写此函数，实现 Go 的 Is 判断。
  virtual bool Is(const Error& target) const noexcept {
    return this == &target;
  }
};

using ErrorPtr = std::shared_ptr<const Error>;

/** 只保存一条不可变文本的叶子错误。依赖 Error 接口，适合直接返回给上层。 */
class StringError final : public Error {
 public:
  explicit StringError(std::string message) : m_message(std::move(message)) {}
  std::string Message() const override { return m_message; }

 private:
  std::string m_message;
};

/** 带单一 cause 的包装错误；Message 返回当前说明，Unwrap 返回原始原因。 */
class WrappedError final : public Error {
 public:
  WrappedError(std::string message, ErrorPtr cause)
      : m_message(std::move(message)), m_cause(std::move(cause)) {}
  ~WrappedError() override;

  std::string Message() const override;
  ErrorPtr Unwrap() const override { return m_cause; }

 private:
  std::string m_message;
  ErrorPtr m_cause;
};

/** 聚合多个原因的错误；UnwrapAll 返回全部非空原因，供 Is/As 遍历。 */
class JoinError final : public Error {
 public:
  explicit JoinError(std::vector<ErrorPtr> causes);
  ~JoinError() override;

  std::string Message() const override;
  std::vector<ErrorPtr> UnwrapAll() const override { return m_causes; }

 private:
  std::vector<ErrorPtr> m_causes;
};

ErrorPtr NewError(std::string message);
ErrorPtr Wrap(ErrorPtr cause, std::string message);
ErrorPtr Join(std::vector<ErrorPtr> causes);
ErrorPtr Join(std::initializer_list<ErrorPtr> causes);

ErrorPtr Unwrap(const ErrorPtr& error) noexcept;
std::vector<ErrorPtr> UnwrapAll(const ErrorPtr& error);
std::string ErrorMessage(const ErrorPtr& error);

/**
 * 遍历单原因或多原因错误链。允许并忽略环；空 target 只匹配空错误，
 * 与 errors.Is(err, nil) 相同。
 */
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
