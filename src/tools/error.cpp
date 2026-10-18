/**
 * @file error.cpp
 * @brief `tools/error.hpp` 的实现：错误图遍历、深链安全析构与消息展平。
 *
 * ## 模块定位
 *
 * 错误基础设施的实现层：`Error` 的默认 `UnwrapAll`、三种内置错误的消息/
 * 析构、`Is`/`As` 的图遍历与工厂函数。被全项目所有模块间接使用。
 *
 * ## 本文件定义什么
 *
 * - `WalkErrors`：环安全、异常安全的错误图遍历；
 * - `WrappedError`/`JoinError` 的析构与 `Message()`；
 * - `NewError`/`Wrap`/`Join`/`Unwrap`/`UnwrapAll`/`ErrorMessage`；
 * - `Is` 及其内部 `IsImpl`；线程本地释放工作表。
 *
 * ## 阅读建议
 *
 * 先读两个析构函数的「工作表迭代释放」——这是深链/深图不爆栈的关键；
 * 再看 `WrappedError::Message`/`JoinError::Message` 如何用迭代展平。
 *
 * ## 边界与陷阱
 *
 * 析构期间不能再递归释放 `shared_ptr` 成员，否则深链会耗尽原生栈；所有
 * `noexcept` 查询（`WalkErrors`/`Is`）必须把自定义错误的异常吞掉并返回失败。
 */
#include "go2cpp/error.hpp"

#include <sstream>
#include <unordered_set>

namespace go2cpp
{

  namespace
  {

    // 内置 WrappedError 形成很长的链时，如果在下一个析构函数中释放 shared_ptr
    // 成员，会递归耗尽原生栈。析构函数把子对象加入线程本地工作表，最外层
    // 的释放过程再用迭代方式排空工作表。
    thread_local std::vector<ErrorPtr> *t_error_release_work = nullptr;

    /**
     * @brief 迭代释放一条 `WrappedError` 原因链，避免递归析构爆栈。
     *
     * ## 步骤
     * step1. 把传入的 cause 放进显式工作表；
     * step2. 把自己的工作表挂到线程本地槽位；
     * step3. 反复弹栈并 `reset()`；被释放的 `WrappedError` 会把其 cause 追加回工作表；
     * step4. 恢复线程本地槽位。
     */
    void ReleaseWrappedCause(ErrorPtr cause) noexcept
    {
      // step1: 非空 cause 才需要释放
      std::vector<ErrorPtr> pending;
      if (cause)
      {
        pending.push_back(std::move(cause));
      }
      // step2: 挂上线程本地工作表，析构函数会把子 cause 追加进来
      auto *previous = t_error_release_work;
      t_error_release_work = &pending;
      // step3: 迭代排空，避免递归
      while (!pending.empty())
      {
        ErrorPtr current = std::move(pending.back());
        pending.pop_back();
        current.reset();
      }
      t_error_release_work = previous;
    }

  } // namespace

  std::vector<ErrorPtr> Error::UnwrapAll() const
  {
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
  bool WalkErrors(const ErrorPtr &error, const ErrorVisitor &visitor) noexcept
  {
    // step1: 空访问器直接失败，避免调用空 std::function
    if (!visitor)
    {
      return false;
    }
    try
    {
      // step2: 显式工作表 + 地址去重，兼容有环的错误图
      std::vector<ErrorPtr> pending;
      std::unordered_set<const Error *> visited;
      if (error)
      {
        pending.push_back(error);
      }
      while (!pending.empty())
      {
        ErrorPtr current = std::move(pending.back());
        pending.pop_back();
        if (!current || !visited.insert(current.get()).second)
        {
          continue;
        }
        // step3: 访问器可以叫停；随后把全部原因压栈（逆序保持原顺序）
        if (visitor(current) == ErrorVisitResult::kStop)
        {
          return false;
        }
        const auto causes = current->UnwrapAll();
        for (auto it = causes.rbegin(); it != causes.rend(); ++it)
        {
          if (*it)
          {
            pending.push_back(*it);
          }
        }
      }
      return true;
    }
    catch (...)
    {
      // step4: 自定义 UnwrapAll/访问器抛异常时按「遍历失败」返回，不穿过 noexcept
      return false;
    }
  }

  WrappedError::~WrappedError()
  {
    // 已经处于释放流程中：把 cause 交回工作表，由最外层循环迭代释放
    if (t_error_release_work != nullptr)
    {
      if (m_cause)
      {
        t_error_release_work->push_back(std::move(m_cause));
      }
      return;
    }
    // 最外层析构：建立工作表并迭代释放整条链
    ReleaseWrappedCause(std::move(m_cause));
  }

  JoinError::~JoinError()
  {
    // 已经处于释放流程中：把全部原因交回工作表
    if (t_error_release_work != nullptr)
    {
      for (auto &cause : m_causes)
      {
        if (cause)
        {
          t_error_release_work->push_back(std::move(cause));
        }
      }
      return;
    }

    // 最外层析构：先搬进本地工作表，再挂到线程本地槽位迭代排空
    std::vector<ErrorPtr> pending;
    pending.reserve(m_causes.size());
    for (auto &cause : m_causes)
    {
      if (cause)
      {
        pending.push_back(std::move(cause));
      }
    }
    auto *previous = t_error_release_work;
    t_error_release_work = &pending;
    while (!pending.empty())
    {
      ErrorPtr current = std::move(pending.back());
      pending.pop_back();
      current.reset();
    }
    t_error_release_work = previous;
  }

  std::string WrappedError::Message() const
  {
    // step1: 逐层拼接同类型包装的 m_message
    std::string result;
    const WrappedError *current = this;
    ErrorPtr current_owner;
    const auto append = [&result](const std::string &part)
    {
      if (part.empty())
      {
        return;
      }
      if (!result.empty())
      {
        result += ": ";
      }
      result += part;
    };

    for (;;)
    {
      // step2: 先追加当前层描述
      append(current->m_message);
      ErrorPtr cause = current->m_cause;
      if (!cause)
      {
        break;
      }
      const auto *wrapped = dynamic_cast<const WrappedError *>(cause.get());
      if (wrapped == nullptr)
      {
        // step3: 遇到非 WrappedError 原因，递归它的 Message() 后结束
        append(cause->Message());
        break;
      }
      // step4: 同类包装继续迭代，不递归
      current = wrapped;
      // 先推进裸指针，再释放前一个所有者。新的所有者会让 `current` 指向的
      // 对象继续存活到下一轮循环。
      current_owner = std::move(cause);
    }
    return result;
  }

  JoinError::JoinError(std::vector<ErrorPtr> causes)
  {
    m_causes.reserve(causes.size());
    for (auto &cause : causes)
    {
      if (cause)
      {
        m_causes.push_back(std::move(cause));
      }
    }
  }

  std::string JoinError::Message() const
  {
    // 使用显式工作表展开内置的嵌套 Join。对于生成的深层错误图，递归调用
    // `Join({previous, next})` 的 Message() 会耗尽原生栈。
    //
    // ## 步骤
    // step1. 把全部原因逆序压栈，保证输出顺序与原因顺序一致；
    // step2. 弹栈；遇到嵌套 JoinError 就把它的原因展开压栈，不递归调用；
    // step3. 遇到叶子错误则取其 Message()，非空时用换行连接；
    // step4. 工作表排空后返回拼好的文本。
    std::string result;
    std::vector<ErrorPtr> pending;
    for (auto it = m_causes.rbegin(); it != m_causes.rend(); ++it)
    {
      if (*it)
      {
        pending.push_back(*it);
      }
    }
    // step2: 迭代展开，嵌套 Join 只把子原因压栈
    bool first = true;
    while (!pending.empty())
    {
      ErrorPtr current = std::move(pending.back());
      pending.pop_back();
      if (!current)
      {
        continue;
      }
      if (const auto *nested = dynamic_cast<const JoinError *>(current.get()))
      {
        for (auto it = nested->m_causes.rbegin();
             it != nested->m_causes.rend(); ++it)
        {
          if (*it)
          {
            pending.push_back(*it);
          }
        }
        continue;
      }
      // step3: 叶子错误：取文本并用换行拼接
      const std::string part = current->Message();
      if (part.empty())
      {
        continue;
      }
      if (!first)
      {
        result.push_back('\n');
      }
      first = false;
      result += part;
    }
    // step4: 全部展开完成
    return result;
  }

  ErrorPtr NewError(std::string message)
  {
    return std::make_shared<StringError>(std::move(message));
  }

  ErrorPtr Wrap(ErrorPtr cause, std::string message)
  {
    if (!cause)
    {
      return {};
    }
    return std::make_shared<WrappedError>(std::move(message), std::move(cause));
  }

  ErrorPtr Join(std::vector<ErrorPtr> causes)
  {
    std::vector<ErrorPtr> filtered;
    filtered.reserve(causes.size());
    for (auto &cause : causes)
    {
      if (cause)
      {
        filtered.push_back(std::move(cause));
      }
    }
    if (filtered.empty())
    {
      return {};
    }
    return std::make_shared<JoinError>(std::move(filtered));
  }

  ErrorPtr Join(std::initializer_list<ErrorPtr> causes)
  {
    return Join(std::vector<ErrorPtr>(causes));
  }

  ErrorPtr Unwrap(const ErrorPtr &error) noexcept
  {
    try
    {
      return error ? error->Unwrap() : ErrorPtr{};
    }
    catch (...)
    {
      return {};
    }
  }

  std::vector<ErrorPtr> UnwrapAll(const ErrorPtr &error)
  {
    return error ? error->UnwrapAll() : std::vector<ErrorPtr>{};
  }

  std::string ErrorMessage(const ErrorPtr &error)
  {
    return error ? error->Message() : std::string{};
  }

  namespace
  {
    bool IsImpl(const ErrorPtr &current, const ErrorPtr &target,
                std::unordered_set<const Error *> &visited)
    {
      std::vector<ErrorPtr> pending;
      if (current)
      {
        pending.push_back(current);
      }
      while (!pending.empty())
      {
        ErrorPtr next = std::move(pending.back());
        pending.pop_back();
        if (!next || !visited.insert(next.get()).second)
        {
          continue;
        }
        if (next.get() == target.get() || next->Is(*target))
        {
          return true;
        }
        auto causes = next->UnwrapAll();
        for (auto it = causes.rbegin(); it != causes.rend(); ++it)
        {
          if (*it)
          {
            pending.push_back(*it);
          }
        }
      }
      return false;
    }
  } // namespace

  bool Is(const ErrorPtr &error, const ErrorPtr &target) noexcept
  {
    try
    {
      if (!target)
      {
        return !error;
      }
      std::unordered_set<const Error *> visited;
      return IsImpl(error, target, visited);
    }
    catch (...)
    {
      // 自定义 Error::UnwrapAll 可以抛异常，遍历容器也可能分配失败；查询
      // 边界以“未匹配”报告，不允许异常逃出 noexcept。
      return false;
    }
  }

} // namespace go2cpp
