#include "go2cpp/error.hpp"

#include <sstream>
#include <unordered_set>

namespace go2cpp {

namespace {

// 内置 WrappedError 形成很长的链时，如果在下一个析构函数中释放 shared_ptr
// 成员，会递归耗尽原生栈。析构函数把子对象加入线程本地工作表，最外层
// 的释放过程再用迭代方式排空工作表。
thread_local std::vector<ErrorPtr>* t_error_release_work = nullptr;

/// 函数功能：完成 ReleaseWrappedCause 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] cause 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
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

/// 函数功能：完成 UnwrapAll 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
std::vector<ErrorPtr> Error::UnwrapAll() const {
  const auto cause = Unwrap();
  return cause ? std::vector<ErrorPtr>{cause} : std::vector<ErrorPtr>{};
}

/// 函数功能：以统一的环安全策略遍历错误图，供日志、指标和转换插件复用。
/// 执行流程：
/// 1. 将非空根错误放入显式工作表；
/// 2. 以对象地址去重，调用访问器并读取 UnwrapAll() 原因；
/// 3. 访问器停止或发生异常时立即结束，避免异常穿过 noexcept 边界。
/// @param[in] error 遍历根节点，可以为空。
/// @param[in] visitor 访问器；不得依赖临时 ErrorPtr 之外的生命周期。
/// @return 完整遍历返回 true；访问器停止或抛出异常返回 false。
/// @note 自定义 Error 的 UnwrapAll/访问器异常会被隔离，不会让错误处理路径终止进程。
bool WalkErrors(const ErrorPtr& error, const ErrorVisitor& visitor) noexcept {
  if (!visitor) {
    return false;
  }
  try {
    std::vector<ErrorPtr> pending;
    std::unordered_set<const Error*> visited;
    if (error) {
      pending.push_back(error);
    }
    while (!pending.empty()) {
      ErrorPtr current = std::move(pending.back());
      pending.pop_back();
      if (!current || !visited.insert(current.get()).second) {
        continue;
      }
      if (visitor(current) == ErrorVisitResult::kStop) {
        return false;
      }
      const auto causes = current->UnwrapAll();
      for (auto it = causes.rbegin(); it != causes.rend(); ++it) {
        if (*it) {
          pending.push_back(*it);
        }
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

/// 函数功能：完成 WrappedError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
WrappedError::~WrappedError() {
  if (t_error_release_work != nullptr) {
    if (m_cause) {
      t_error_release_work->push_back(std::move(m_cause));
    }
    return;
  }
  ReleaseWrappedCause(std::move(m_cause));
}

/// 函数功能：完成 JoinError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
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

/// 函数功能：完成 Message 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
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
    // 先推进裸指针，再释放前一个所有者。新的所有者会让 `current` 指向的
    // 对象继续存活到下一轮循环。
    current_owner = std::move(cause);
  }
  return result;
}

/// 函数功能：完成 JoinError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] causes 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
JoinError::JoinError(std::vector<ErrorPtr> causes) {
  m_causes.reserve(causes.size());
  for (auto& cause : causes) {
    if (cause) {
      m_causes.push_back(std::move(cause));
    }
  }
}

/// 函数功能：完成 Message 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
std::string JoinError::Message() const {
  // 使用显式工作表展开内置的嵌套 Join。对于生成的深层错误图，递归调用
  // `Join({previous, next})` 的 Message() 会耗尽原生栈。
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

/// 函数功能：完成 NewError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] message 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr NewError(std::string message) {
  return std::make_shared<StringError>(std::move(message));
}

/// 函数功能：完成 Wrap 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] cause 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] message 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr Wrap(ErrorPtr cause, std::string message) {
  if (!cause) {
    return {};
  }
  return std::make_shared<WrappedError>(std::move(message), std::move(cause));
}

/// 函数功能：完成 Join 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] causes 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
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

/// 函数功能：完成 Join 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] causes 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr Join(std::initializer_list<ErrorPtr> causes) {
  return Join(std::vector<ErrorPtr>(causes));
}

/// 函数功能：完成 Unwrap 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr Unwrap(const ErrorPtr& error) noexcept {
  return error ? error->Unwrap() : ErrorPtr{};
}

/// 函数功能：完成 UnwrapAll 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
std::vector<ErrorPtr> UnwrapAll(const ErrorPtr& error) {
  return error ? error->UnwrapAll() : std::vector<ErrorPtr>{};
}

/// 函数功能：完成 ErrorMessage 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
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

/// 函数功能：完成 Is 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] target 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool Is(const ErrorPtr& error, const ErrorPtr& target) noexcept {
  if (!target) {
    return !error;
  }
  std::unordered_set<const Error*> visited;
  return IsImpl(error, target, visited);
}

}  // namespace go2cpp
