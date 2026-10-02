#pragma once

#include "go2cpp/error.hpp"

#include <any>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp {

class Context;
using ContextPtr = std::shared_ptr<Context>;
using ContextTimePoint = std::chrono::steady_clock::time_point;
using ContextDuration = std::chrono::steady_clock::duration;
using CancelFunc = std::function<void()>;
using CancelCauseFunc = std::function<void(ErrorPtr)>;

namespace detail {
struct DoneSignalAccess;
}

/**
 * 只关闭一次、可复制的完成通知。
 *
 * 依赖：内部共享状态和 ParkingCondition；Context 以它表示 Done 信号。
 * 对上层提供等待、截止时间等待及一次性回调注册，复制对象共享同一
 * 信号状态；回调始终在信号锁外执行。
 */
class DoneSignal {
public:
    using CallbackId = std::uint64_t;

    /** @brief 创建未完成的通知信号。 */
    DoneSignal();
    DoneSignal(const DoneSignal&) = default;
    DoneSignal& operator=(const DoneSignal&) = default;
    DoneSignal(DoneSignal&&) noexcept = default;
    DoneSignal& operator=(DoneSignal&&) noexcept = default;
    /** @brief 释放信号状态。 */
    ~DoneSignal();

    // 无限等待信号关闭。
    /** @brief 无限等待信号完成。 */
    void Wait() const;
    // 最多等待 timeout；返回 true 表示已经关闭。
    /** @brief 等待相对时长。@return 已完成返回 true。 */
    bool WaitFor(ContextDuration timeout) const;
    // 等待到单调时钟 deadline；返回 true 表示已经关闭。
    /** @brief 等待到绝对截止时间。@return 已完成返回 true。 */
    bool WaitUntil(ContextTimePoint deadline) const;
    // 查询信号是否已关闭，不阻塞。
    /** @brief 返回信号是否已经完成。 */
    bool IsDone() const noexcept;

    bool wait_for(ContextDuration timeout) const { return WaitFor(timeout); }
    bool wait_until(ContextTimePoint deadline) const { return WaitUntil(deadline); }
    bool is_done() const noexcept { return IsDone(); }

    // 注册最多执行一次且在信号锁外调用的回调。若回调捕获了自身的最后
    // owner，应先调用 RemoveCallback；信号无法推断回调的对象所有权。
    /** @brief 添加完成回调并返回回调 ID。 */
    CallbackId AddCallback(std::function<void()> callback) const;
    /** @brief 删除指定完成回调。 */
    void RemoveCallback(CallbackId id) const;

private:
    struct State;
    std::shared_ptr<State> m_state;

    CallbackId AddCallbackImpl(std::function<void()> callback,
                               bool internal) const;
    void Signal() const noexcept;
    friend class Context;
    friend struct detail::DoneSignalAccess;
};

/**
 * Context 的类型化身份键。复制键保持同一身份，不同键不会碰撞。
 * 依赖：shared_ptr 锚点维持键身份；Context 通过 Identity 查找值。
 * 对上层提供类型安全的 Value 查询，Name 仅用于调试元数据。
 */
template <typename T>
class ContextKey {
public:
    /** @brief 创建匿名类型安全键。 */
    ContextKey();
    /** @brief 创建带诊断名称的类型安全键。 */
    explicit ContextKey(std::string name);

    /** @brief 返回键名。 */
    const std::string& Name() const noexcept { return m_name; }
    /** @brief 返回键的身份地址。 */
    const void* Identity() const noexcept { return m_token.get(); }
    // Context 节点在类型化值存在期间保留此锚点，避免分配器复用地址让
    // 后续键发生碰撞。
    std::shared_ptr<const std::uint64_t> Anchor() const noexcept {
        return m_token;
    }

private:
    std::shared_ptr<const std::uint64_t> m_token;
    std::string m_name;
};

/**
 * 可取消的父子上下文。
 *
 * 依赖：DoneSignal 负责通知，Error 表示取消原因，steady_clock 提供截止
 * 时间，ContextRollback 可把临时子上下文和局部补偿动作绑定。
 * 对上层提供 Background/TODO、WithCancel、WithDeadline、WithTimeout、
 * WithValue、Done/Err/Cause/Value。Context 只管理取消树和值的生命周期，
 * 不拥有 Scheduler、Fiber 或用户资源；取消是幂等的，子节点只能从父节点
 * 继承取消而不能反向取消父节点。
 */
class Context final {
public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::function<ContextTimePoint()>;
    struct State;

    // 返回永不自动取消的根上下文；Background 用于正式运行，TODO 用于
    // 尚未确定父级的占位。
    /** @brief 创建永不取消的根 Context。 */
    static ContextPtr Background();
    /** @brief 创建用于占位的根 Context。 */
    static ContextPtr TODO();

    /** @brief 创建可取消子 Context 和取消函数。 */
    static std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr& parent);
    /** @brief 创建支持 Cause 的可取消子 Context。 */
    static std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(
        const ContextPtr& parent);
    /** @brief 创建带绝对截止时间的子 Context。 */
    static std::pair<ContextPtr, CancelFunc> WithDeadline(
        const ContextPtr& parent, ContextTimePoint deadline);
    /** @brief 创建带相对超时的子 Context。 */
    static std::pair<ContextPtr, CancelFunc> WithTimeout(
        const ContextPtr& parent, ContextDuration timeout);

    /** @brief 创建携带类型安全值的子 Context。 */
    template <typename T>
    static ContextPtr WithValue(const ContextPtr& parent,
                                const ContextKey<T>& key, T value) {
        return MakeValueContext(parent, key.Identity(), key.Name(),
                                key.Anchor(),
                                std::any(std::move(value)));
    }

    // 字符串键用于动态转译代码；优先使用类型化键，因为其身份不会意外
    // 与父链中的其他键碰撞。
    static ContextPtr WithValue(const ContextPtr& parent, std::string key,
                                std::any value);

    // 返回只读 Done 信号；取消发生后所有等待者都会被唤醒。
    /** @brief 返回取消完成通知信号。 */
    const DoneSignal& Done() const noexcept { return m_done; }
    /** @brief 返回可供内部注册回调的完成信号。 */
    DoneSignal& Done() noexcept { return m_done; }
    // 查询是否已取消；返回值是当前快照。
    /** @brief 返回 Context 是否已取消或到期。 */
    bool IsDone() const noexcept;
    // 返回标准取消错误；未取消时返回空。
    /** @brief 返回标准 Canceled/DeadlineExceeded 错误。 */
    ErrorPtr Err() const;
    // 返回最初取消原因；未取消时返回空。
    /** @brief 返回取消时记录的原始 Cause。 */
    ErrorPtr Cause() const;
    // 返回截止时间；没有截止时间时为空。
    /** @brief 返回截止时间；没有截止时间时返回空值。 */
    std::optional<ContextTimePoint> Deadline() const;
    // 查询是否设置了截止时间。
    /** @brief 判断 Context 是否设置了截止时间。 */
    bool HasDeadline() const;

    /** @brief 使用字符串键查询类型擦除值。 */
    std::any Value(const std::string& key) const;

    template <typename T>
    std::optional<T> Value(const ContextKey<T>& key) const {
        // 类型化查询只比较 Identity；字符串名称只是元数据，不能让无关
        // 的键在父链中发生碰撞。
        const std::any value = LookupValue(key.Identity(), {});
        if (!value.has_value()) {
            return std::nullopt;
        }
        const auto* converted = std::any_cast<T>(&value);
        if (converted == nullptr) {
            return std::nullopt;
        }
        return *converted;
    }

    // 取消操作幂等；cause 为空时映射为 CanceledError()。
    // 幂等取消当前上下文及其子树；cause 为空时使用 CanceledError。
    /** @brief 取消当前 Context 并向全部子节点传播。 */
    void Cancel(ErrorPtr cause = {});
    // 以 DeadlineExceededError 取消当前上下文及其子树。
    /** @brief 以 DeadlineExceeded 原因取消当前 Context。 */
    void CancelDeadline();

    // 测试和嵌入方可以注入单调时钟以创建超时；已经创建的定时器仍保留
    // 原先计算出的截止时间。
    /** @brief 注入测试时钟函数。 */
    static void SetNowFunctionForTesting(NowFunction now);
    /** @brief 恢复默认 steady_clock。 */
    static void ResetNowFunctionForTesting();
    /** @brief 读取当前上下文时钟。 */
    static ContextTimePoint Now();

private:
    explicit Context(std::shared_ptr<State> state);
    static ContextPtr MakeChild(const ContextPtr& parent);
    static ContextPtr MakeValueContext(const ContextPtr& parent,
                                       const void* token,
                                       const std::string& name,
                                       std::shared_ptr<const std::uint64_t>
                                           token_anchor,
                                       std::any value);
    std::any LookupValue(const void* token, const std::string& name) const;
    DoneSignal::CallbackId AddBeforeDoneCallback(
        std::function<void()> callback);
    void RemoveBeforeDoneCallback(DoneSignal::CallbackId id) const noexcept;

    std::shared_ptr<State> m_state;
    DoneSignal m_done;

    friend ContextPtr Background();
    friend ContextPtr TODO();
    friend std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr&);
    friend std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(
        const ContextPtr&);
    friend std::pair<ContextPtr, CancelFunc> WithDeadline(
        const ContextPtr&, ContextTimePoint);
    friend std::pair<ContextPtr, CancelFunc> WithTimeout(const ContextPtr&,
                                                         ContextDuration);
    friend class ContextRollback;
};

// ContextRollback 把一个临时子 Context 和一组局部补偿动作绑定在一起。
// 它只回滚尚未提交的本地状态，不恢复父 Context，也不跳过 C++ 析构函数。
class ContextRollback final {
    struct State;

public:
    enum class Status {
        kActive,
        kRollingBack,
        kCommitted,
        kRolledBack,
        kFailed,
    };

    using UndoAction = std::function<void()>;

    class Savepoint final {
    public:
        Savepoint() = default;

        bool valid() const noexcept;

    private:
        std::weak_ptr<State> m_state;
        std::size_t m_depth{0};
        std::uint64_t m_generation{0};

        friend class ContextRollback;
    };

    // rollback_on_cancel 为 true 时，child Context 被父级取消或 deadline
    // 取消后，尚未提交的 undo 会在 child Done 已线性化后由触发取消的
    // 线程执行；普通 Done 观察回调会在内部 undo 之后运行。
    explicit ContextRollback(ContextPtr parent,
                             bool rollback_on_cancel = true);
    ~ContextRollback() noexcept;

    ContextRollback(const ContextRollback&) = delete;
    ContextRollback& operator=(const ContextRollback&) = delete;
    ContextRollback(ContextRollback&& other) noexcept;
    ContextRollback& operator=(ContextRollback&& other) noexcept;

    ContextPtr context() const noexcept { return m_context; }

    // 成功返回 true。动作应幂等、短小、非阻塞，最好声明为 noexcept。
    bool RecordUndo(UndoAction action);
    bool record_undo(UndoAction action) { return RecordUndo(std::move(action)); }

    Savepoint Mark() const noexcept;
    Savepoint savepoint() const noexcept { return Mark(); }

    // 只撤销 mark 之后登记的动作，事务通常仍保持 active；并发完整回滚
    // 或取消可能把请求升级为完整回滚。一次成功的局部回滚会使已有
    // Savepoint 全部失效，调用方应重新创建令牌。
    bool RollbackTo(const Savepoint& mark) noexcept;
    bool rollback_to(const Savepoint& mark) noexcept {
        return RollbackTo(mark);
    }

    // 显式回滚会取消 child Context，并按 LIFO 执行全部剩余动作。
    // 已经完成回滚时重复调用仍返回 true；已提交则返回 false。
    bool Rollback(ErrorPtr cause = {}) noexcept;
    bool rollback(ErrorPtr cause = {}) noexcept {
        return Rollback(std::move(cause));
    }

    // 提交只丢弃 undo，不取消 child Context；调用方可以继续把 context()
    // 交给下游。提交和回滚竞争时，以先完成状态转换者为准。
    bool Commit() noexcept;
    bool commit() noexcept { return Commit(); }

    Status status() const noexcept;
    bool active() const noexcept;
    bool committed() const noexcept;
    bool rolled_back() const noexcept;
    bool had_failure() const noexcept;
    std::exception_ptr failure() const noexcept;

    // 与 child Context 的 Done 分离：该信号表示 commit 或完整 rollback
    // 的补偿动作已经执行完毕。需要等待 undo 完成时使用它。undo 不得
    // 等待自身的 RollbackDone，也不应在回调中阻塞或再次取得外部锁。
    DoneSignal RollbackDone() const;
    DoneSignal rollback_done() const { return RollbackDone(); }

private:
    std::shared_ptr<State> m_state;
    ContextPtr m_context;
    mutable std::mutex m_callback_mutex;
    DoneSignal::CallbackId m_callback_id{0};
    DoneSignal::CallbackId m_before_callback_id{0};

    void DisarmCallback() noexcept;
};

using RollbackScope = ContextRollback;

ContextRollback WithRollback(const ContextPtr& parent,
                             bool rollback_on_cancel = true);

/** @brief 创建 Background 根 Context。 */
ContextPtr Background();
/** @brief 创建 TODO 根 Context。 */
ContextPtr TODO();
/** @brief 创建可取消子 Context。 */
std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr& parent);
/** @brief 创建可记录 Cause 的可取消子 Context。 */
std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(
    const ContextPtr& parent);
/** @brief 创建带截止时间的子 Context。 */
std::pair<ContextPtr, CancelFunc> WithDeadline(const ContextPtr& parent,
                                               ContextTimePoint deadline);
/** @brief 创建带超时的子 Context。 */
std::pair<ContextPtr, CancelFunc> WithTimeout(const ContextPtr& parent,
                                             ContextDuration timeout);

template <typename T>
ContextPtr WithValue(const ContextPtr& parent, const ContextKey<T>& key, T value) {
    return Context::WithValue(parent, key, std::move(value));
}

/** @brief 创建携带字符串键值的子 Context。 */
ContextPtr WithValue(const ContextPtr& parent, std::string key, std::any value);

/** @brief 返回标准 Canceled 错误。 */
ErrorPtr CanceledError();
/** @brief 返回标准 DeadlineExceeded 错误。 */
ErrorPtr DeadlineExceededError();
inline ErrorPtr ErrCanceled() { return CanceledError(); }
inline ErrorPtr ErrDeadlineExceeded() { return DeadlineExceededError(); }
inline ErrorPtr Canceled() { return CanceledError(); }
inline ErrorPtr DeadlineExceeded() { return DeadlineExceededError(); }

template <typename T>
ContextKey<T>::ContextKey()
    : m_token(std::make_shared<const std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(this))) {}

template <typename T>
ContextKey<T>::ContextKey(std::string name)
    : m_token(std::make_shared<const std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(this))),
      m_name(std::move(name)) {}

}  // namespace go2cpp
