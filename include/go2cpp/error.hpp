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
 * Immutable error value used by the compatibility layer.
 *
 * ErrorPtr is the library's nil-able equivalent of a Go error interface.
 * Error implementations must remain immutable after construction, which makes
 * a shared ErrorPtr safe to pass between goroutines.
 */
class Error {
 public:
  virtual ~Error() = default;
  virtual std::string Message() const = 0;
  // Go-like spelling for callers that prefer Error().
  virtual std::string ErrorString() const { return Message(); }
  virtual std::shared_ptr<const Error> Unwrap() const { return {}; }
  virtual std::vector<std::shared_ptr<const Error>> UnwrapAll() const;
  // A custom error may override this to implement Go's Is method.
  virtual bool Is(const Error& target) const noexcept {
    return this == &target;
  }
};

using ErrorPtr = std::shared_ptr<const Error>;

class StringError final : public Error {
 public:
  explicit StringError(std::string message) : m_message(std::move(message)) {}
  std::string Message() const override { return m_message; }

 private:
  std::string m_message;
};

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
 * Traverse a single- or multi-cause chain.  Cycles are tolerated and ignored.
 * A null target matches only a null error, like errors.Is(err, nil).
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
  // Explicit adapter escape hatch: mutating the returned object is not safe
  // while the ErrorPtr is shared. Ordinary callers should use As<T>() and
  // keep error values immutable.
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
