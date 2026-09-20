#include "go2cpp/error.hpp"

#include <sstream>
#include <unordered_set>

namespace go2cpp {

namespace {

// A long chain of the built-in WrappedError values otherwise releases one
// shared_ptr member from inside the next destructor and can exhaust the
// native stack. Destructors append their child to this thread-local worklist
// while the outermost release drains it iteratively.
thread_local std::vector<ErrorPtr>* t_error_release_work = nullptr;

void ReleaseWrappedCause(ErrorPtr cause) noexcept {
  std::vector<ErrorPtr> pending;
  if (cause) {
    pending.push_back(std::move(cause));
  }
  auto* previous = t_error_release_work;
  t_error_release_work = &pending;
  while (!pending.empty()) {
    ErrorPtr current = std::move(pending.back());
    pending.pop_back();
    current.reset();
  }
  t_error_release_work = previous;
}

}  // namespace

std::vector<ErrorPtr> Error::UnwrapAll() const {
  const auto cause = Unwrap();
  return cause ? std::vector<ErrorPtr>{cause} : std::vector<ErrorPtr>{};
}

WrappedError::~WrappedError() {
  if (t_error_release_work != nullptr) {
    if (m_cause) {
      t_error_release_work->push_back(std::move(m_cause));
    }
    return;
  }
  ReleaseWrappedCause(std::move(m_cause));
}

JoinError::~JoinError() {
  if (t_error_release_work != nullptr) {
    for (auto& cause : m_causes) {
      if (cause) {
        t_error_release_work->push_back(std::move(cause));
      }
    }
    return;
  }

  std::vector<ErrorPtr> pending;
  pending.reserve(m_causes.size());
  for (auto& cause : m_causes) {
    if (cause) {
      pending.push_back(std::move(cause));
    }
  }
  auto* previous = t_error_release_work;
  t_error_release_work = &pending;
  while (!pending.empty()) {
    ErrorPtr current = std::move(pending.back());
    pending.pop_back();
    current.reset();
  }
  t_error_release_work = previous;
}

std::string WrappedError::Message() const {
  std::string result;
  const WrappedError* current = this;
  ErrorPtr current_owner;
  const auto append = [&result](const std::string& part) {
    if (part.empty()) {
      return;
    }
    if (!result.empty()) {
      result += ": ";
    }
    result += part;
  };

  for (;;) {
    append(current->m_message);
    ErrorPtr cause = current->m_cause;
    if (!cause) {
      break;
    }
    const auto* wrapped = dynamic_cast<const WrappedError*>(cause.get());
    if (wrapped == nullptr) {
      append(cause->Message());
      break;
    }
    current = wrapped;
    // Advance the raw pointer before releasing the previous owner. The new
    // owner keeps the object referenced by `current` alive for the next loop.
    current_owner = std::move(cause);
  }
  return result;
}

JoinError::JoinError(std::vector<ErrorPtr> causes) {
  m_causes.reserve(causes.size());
  for (auto& cause : causes) {
    if (cause) {
      m_causes.push_back(std::move(cause));
    }
  }
}

std::string JoinError::Message() const {
  // Flatten built-in nested joins with an explicit worklist. Calling
  // Message() recursively for `Join({previous, next})` would exhaust the
  // native stack for a generated deep error graph.
  std::string result;
  std::vector<ErrorPtr> pending;
  for (auto it = m_causes.rbegin(); it != m_causes.rend(); ++it) {
    if (*it) {
      pending.push_back(*it);
    }
  }
  bool first = true;
  while (!pending.empty()) {
    ErrorPtr current = std::move(pending.back());
    pending.pop_back();
    if (!current) {
      continue;
    }
    if (const auto* nested = dynamic_cast<const JoinError*>(current.get())) {
      for (auto it = nested->m_causes.rbegin();
           it != nested->m_causes.rend(); ++it) {
        if (*it) {
          pending.push_back(*it);
        }
      }
      continue;
    }
    const std::string part = current->Message();
    if (part.empty()) {
      continue;
    }
    if (!first) {
      result.push_back('\n');
    }
    first = false;
    result += part;
  }
  return result;
}

ErrorPtr NewError(std::string message) {
  return std::make_shared<StringError>(std::move(message));
}

ErrorPtr Wrap(ErrorPtr cause, std::string message) {
  if (!cause) {
    return {};
  }
  return std::make_shared<WrappedError>(std::move(message), std::move(cause));
}

ErrorPtr Join(std::vector<ErrorPtr> causes) {
  std::vector<ErrorPtr> filtered;
  filtered.reserve(causes.size());
  for (auto& cause : causes) {
    if (cause) {
      filtered.push_back(std::move(cause));
    }
  }
  if (filtered.empty()) {
    return {};
  }
  return std::make_shared<JoinError>(std::move(filtered));
}

ErrorPtr Join(std::initializer_list<ErrorPtr> causes) {
  return Join(std::vector<ErrorPtr>(causes));
}

ErrorPtr Unwrap(const ErrorPtr& error) noexcept {
  return error ? error->Unwrap() : ErrorPtr{};
}

std::vector<ErrorPtr> UnwrapAll(const ErrorPtr& error) {
  return error ? error->UnwrapAll() : std::vector<ErrorPtr>{};
}

std::string ErrorMessage(const ErrorPtr& error) {
  return error ? error->Message() : std::string{};
}

namespace {
bool IsImpl(const ErrorPtr& current, const ErrorPtr& target,
            std::unordered_set<const Error*>& visited) noexcept {
  std::vector<ErrorPtr> pending;
  if (current) {
    pending.push_back(current);
  }
  while (!pending.empty()) {
    ErrorPtr next = std::move(pending.back());
    pending.pop_back();
    if (!next || !visited.insert(next.get()).second) {
      continue;
    }
    if (next.get() == target.get() || next->Is(*target)) {
      return true;
    }
    auto causes = next->UnwrapAll();
    for (auto it = causes.rbegin(); it != causes.rend(); ++it) {
      if (*it) {
        pending.push_back(*it);
      }
    }
  }
  return false;
}
}  // namespace

bool Is(const ErrorPtr& error, const ErrorPtr& target) noexcept {
  if (!target) {
    return !error;
  }
  std::unordered_set<const Error*> visited;
  return IsImpl(error, target, visited);
}

}  // namespace go2cpp
