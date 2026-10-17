/**
 * @file context.hpp
 * @brief Context 与 DoneSignal：取消传播的基础设施。
 *
 * ## 模块定位
 *
 * `Context` 是 Go 里 `context.Context` 的对应物，解决的是**"一个任务链条上的
 * 所有等待方如何同时被叫停"**：超时、显式取消、上游失败，都需要沿着调用链
 * 传播到所有正在等待的 Fiber。
 *
 * 它位于**同步原语之上、业务代码之下**：不参与调度决策，但它的 `Done()`
 * 信号会被 `Future`、`Channel`、`Timer` 等设施用来注册唤醒回调。
 *
 * ## 本文件定义什么
 *
 * - `Context`：不可变的取消树节点（WithCancel / WithTimeout / WithDeadline
 *   派生出子节点，父取消会级联到全部子节点）；
 * - `DoneSignal`：完成信号，支持注册回调（`AddCallback`）与批量通知。
 *   这是 Fiber 等待取消的挂载点；
 * - 一组取消原因错误（`CanceledError` / `DeadlineExceededError`），
 *   比较时按错误身份而非文本，避免把不同来源的取消混为一谈。
 *
 * ## 一个容易踩的点
 *
 * `DoneSignal` 的回调在**通知线程**上同步执行，所以回调里不能做阻塞操作 ——
 * 典型写法是在回调里只 `Notify()` 一个等待原语，把真正的处理留给被唤醒方。
 */
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

namespace go2cpp
{

    class Context;
    using ContextPtr = std::shared_ptr<Context>;
    using ContextTimePoint = std::chrono::steady_clock::time_point;
    using ContextDuration = std::chrono::steady_clock::duration;
    using CancelFunc = std::function<void()>;
    using CancelCauseFunc = std::function<void(ErrorPtr)>;

    namespace detail
    {
        struct DoneSignalAccess;
    }

    /**
     * 只关闭一次、可复制的完成通知。
     *
     * 依赖：内部共享状态和 ParkingCondition；Context 以它表示 Done 信号。
     * 对上层提供等待、截止时间等待及一次性回调注册，复制对象共享同一
     * 信号状态；回调始终在信号锁外执行。
     */
    class DoneSignal
    {
    public:
        using CallbackId = std::uint64_t;

        /** @brief 创建未完成的通知信号。 */
        DoneSignal();
        DoneSignal(const DoneSignal &) = default;
        DoneSignal &operator=(const DoneSignal &) = default;
        DoneSignal(DoneSignal &&) noexcept = default;
        DoneSignal &operator=(DoneSignal &&) noexcept = default;
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

        // 通知的内部状态，多份 DoneSignal 句柄共享同一份。
        // - 保护：State::mutex（见 .cpp）；本句柄自身无锁。
        // - 不变式：复制 DoneSignal 会共享此指针（因此副本是"同一个信号"），
        //   这正是 Context 能用一份镜像成员表达同一信号的前提。
        // - 可空？构造时必然分配（见 DoneSignal() 实现），运行期不为空。
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
    class ContextKey
    {
    public:
        /** @brief 创建匿名类型安全键。 */
        ContextKey();
        /** @brief 创建带诊断名称的类型安全键。 */
        explicit ContextKey(std::string name);

        /** @brief 返回键名。 */
        const std::string &Name() const noexcept { return m_name; }
        /** @brief 返回键的身份地址。 */
        const void *Identity() const noexcept { return m_token.get(); }
        // Context 节点在类型化值存在期间保留此锚点，避免分配器复用地址让
        // 后续键发生碰撞。
        std::shared_ptr<const std::uint64_t> Anchor() const noexcept
        {
            return m_token;
        }

    private:
        // 键的身份载体。**身份取的是这个堆对象的地址**（Identity() 返回
        // m_token.get()），而不是它保存的数值——数值只是用 this 填充的占位。
        // - 保护：无锁。构造后 m_token 不再改变（const），拷贝共享同一对象。
        // - 不变式：只要本键存活，该堆地址就被占用，因此不会被分配器复用给
        //   别的键；不同 ContextKey 实例必然拿到不同地址，故身份永不碰撞。
        //   Context 侧另存一份锚点（typed_key_anchors）把这一保证延伸到
        //   "值仍然存在"的整个期间。
        std::shared_ptr<const std::uint64_t> m_token;

        // 仅用于诊断与日志的键名。**不参与身份匹配**——类型化查询只比较
        // m_token.get()，否则父链中同名的无关键会互相命中。
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
    class Context final
    {
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
        static std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr &parent);
        /** @brief 创建支持 Cause 的可取消子 Context。 */
        static std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(
            const ContextPtr &parent);
        /** @brief 创建带绝对截止时间的子 Context。 */
        static std::pair<ContextPtr, CancelFunc> WithDeadline(
            const ContextPtr &parent, ContextTimePoint deadline);
        /** @brief 创建带相对超时的子 Context。 */
        static std::pair<ContextPtr, CancelFunc> WithTimeout(
            const ContextPtr &parent, ContextDuration timeout);

        /** @brief 创建携带类型安全值的子 Context。 */
        template <typename T>
        static ContextPtr WithValue(const ContextPtr &parent,
                                    const ContextKey<T> &key, T value)
        {
            return MakeValueContext(parent, key.Identity(), key.Name(),
                                    key.Anchor(),
                                    std::any(std::move(value)));
        }

        // 字符串键用于动态转译代码；优先使用类型化键，因为其身份不会意外
        // 与父链中的其他键碰撞。
        static ContextPtr WithValue(const ContextPtr &parent, std::string key,
                                    std::any value);

        // 返回只读 Done 信号；取消发生后所有等待者都会被唤醒。
        /** @brief 返回取消完成通知信号。 */
        const DoneSignal &Done() const noexcept { return m_done; }
        /** @brief 返回可供内部注册回调的完成信号。 */
        DoneSignal &Done() noexcept { return m_done; }
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
        std::any Value(const std::string &key) const;

        template <typename T>
        std::optional<T> Value(const ContextKey<T> &key) const
        {
            // 类型化查询只比较 Identity；字符串名称只是元数据，不能让无关
            // 的键在父链中发生碰撞。
            const std::any value = LookupValue(key.Identity(), {});
            if (!value.has_value())
            {
                return std::nullopt;
            }
            const auto *converted = std::any_cast<T>(&value);
            if (converted == nullptr)
            {
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
        static ContextPtr MakeChild(const ContextPtr &parent);
        static ContextPtr MakeValueContext(const ContextPtr &parent,
                                           const void *token,
                                           const std::string &name,
                                           std::shared_ptr<const std::uint64_t>
                                               token_anchor,
                                           std::any value);
        std::any LookupValue(const void *token, const std::string &name) const;
        DoneSignal::CallbackId AddBeforeDoneCallback(
            std::function<void()> callback);
        void RemoveBeforeDoneCallback(DoneSignal::CallbackId id) const noexcept;

        // 取消树的节点状态，由本句柄与所有派生句柄共享。
        // - 保护：State 自带 mutex（见 .cpp 中 State 定义），本指针本身只读。
        // - 不变式：构造后再不改指；Context 的私有构造函数是唯一入口，
        //   外部只能经 Background/TODO/WithCancel 等工厂拿到实例。
        // - 生命周期：与 State 同步；CancelFunc 另持有 State 的 weak_ptr，
        //   所以句柄全部销毁后取消能力是否仍在取决于 State 是否还有owner。
        std::shared_ptr<State> m_state;

        // **镜像成员**：值是 m_state->done 的拷贝。DoneSignal 复制时共享内部
        // State，所以它与 m_state->done 实际是同一个信号，不会出现两份状态。
        // - 存在理由：State 是 .cpp 内的不完整类型，头文件无法写出
        //   "const DoneSignal&" 的返回；这里放一份完整类型成员，让 Done()
        //   能在头文件内联返回引用。
        // - 不变式：必须与 m_state 同步构造（见 Context 构造函数初始化列表），
        //   且此后再不重新赋值。
        DoneSignal m_done;

        friend ContextPtr Background();
        friend ContextPtr TODO();
        friend std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr &);
        friend std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(
            const ContextPtr &);
        friend std::pair<ContextPtr, CancelFunc> WithDeadline(
            const ContextPtr &, ContextTimePoint);
        friend std::pair<ContextPtr, CancelFunc> WithTimeout(const ContextPtr &,
                                                             ContextDuration);
        friend class ContextRollback;
    };

    // ContextRollback 把一个临时子 Context 和一组局部补偿动作绑定在一起。
    // 它只回滚尚未提交的本地状态，不恢复父 Context，也不跳过 C++ 析构函数。
    class ContextRollback final
    {
        struct State;

    public:
        enum class Status
        {
            kActive,
            kRollingBack,
            kCommitted,
            kRolledBack,
            kFailed,
        };

        using UndoAction = std::function<void()>;

        /**
         * @brief 可接收回滚原因的补偿动作。
         * @details cause 是触发本次回滚的原因：显式 Rollback(cause) 时为该 cause；
         *          父级取消或 deadline 到期触发的自动回滚时为 nullptr（原因可从
         *          context()->Err() 取得）；RollbackTo() 局部回滚与析构回滚时
         *          同样为 nullptr。
         */
        using UndoActionWithCause = std::function<void(const ErrorPtr &cause)>;

        class Savepoint final
        {
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

        ContextRollback(const ContextRollback &) = delete;
        ContextRollback &operator=(const ContextRollback &) = delete;
        ContextRollback(ContextRollback &&other) noexcept;
        ContextRollback &operator=(ContextRollback &&other) noexcept;

        ContextPtr context() const noexcept { return m_context; }

        // 成功返回 true。动作应幂等、短小、非阻塞，最好声明为 noexcept。
        // reason 可选；失败时写入可读原因（动作为空 / 事务已结束 /
        // child Context 已被取消而转入回滚）。
        bool RecordUndo(UndoAction action, std::string *reason = nullptr);
        bool record_undo(UndoAction action, std::string *reason = nullptr)
        {
            return RecordUndo(std::move(action), reason);
        }

        /**
         * @brief 登记一个会在回滚时收到「回滚原因」的补偿动作。
         * @details 语义与 RecordUndo 完全一致，唯一区别是执行时会把本次回滚的
         *          原因作为参数传入，便于按超时/取消/业务失败做不同处置。
         *          两种动作共用同一条 LIFO 队列，可以混用。
         */
        bool RecordUndoWithCause(UndoActionWithCause action,
                                 std::string *reason = nullptr);
        bool record_undo_with_cause(UndoActionWithCause action,
                                    std::string *reason = nullptr)
        {
            return RecordUndoWithCause(std::move(action), reason);
        }

        Savepoint Mark() const noexcept;
        Savepoint savepoint() const noexcept { return Mark(); }

        // 只撤销 mark 之后登记的动作，事务通常仍保持 active；并发完整回滚
        // 或取消可能把请求升级为完整回滚。一次成功的局部回滚会使已有
        // Savepoint 全部失效，调用方应重新创建令牌。
        bool RollbackTo(const Savepoint &mark) noexcept;
        bool rollback_to(const Savepoint &mark) noexcept
        {
            return RollbackTo(mark);
        }

        // 显式回滚会取消 child Context，并按 LIFO 执行全部剩余动作。
        // 已经完成回滚时重复调用仍返回 true；已提交则返回 false。
        bool Rollback(ErrorPtr cause = {}) noexcept;
        bool rollback(ErrorPtr cause = {}) noexcept
        {
            return Rollback(std::move(cause));
        }

        // 提交只丢弃 undo，不取消 child Context；调用方可以继续把 context()
        // 交给下游。提交和回滚竞争时，以先完成状态转换者为准。
        bool Commit() noexcept;
        bool commit() noexcept { return Commit(); }

        Status status() const noexcept;
        bool active() const noexcept;

        /** @brief 返回尚未执行（或被领取执行）的补偿动作数量。 */
        std::size_t pending_undo_count() const noexcept;
        /**
         * @brief 返回全部补偿动作抛出的异常，按发生顺序。
         * @details 与 failure() 的区别：failure() 只给第一个（历史行为，保持不变），
         *          本接口给出全部，便于完整诊断。
         */
        std::vector<std::exception_ptr> failures() const;
        /**
         * @brief 返回本次回滚的原因。
         * @details 显式 Rollback(cause) 返回该 cause；自动回滚（父取消/deadline）
         *          返回 context()->Err()；尚未回滚或无法确定时返回空。
         */
        ErrorPtr rollback_cause() const noexcept;
        /** @brief 返回状态名称；未知值返回 "unknown"。 */
        static const char *StatusName(Status status) noexcept;
        const char *status_name() const noexcept { return StatusName(status()); }
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
        // 补偿事务的内部状态（savepoint 栈、undo 队列、失败聚合），
        // 由 Savepoint 与本对象共享。
        // - 保护：State 自带 mutex（见 .cpp）。
        // - 不变式：构造后再不改指；Savepoint 持有它的 weak_ptr，因此
        //   ContextRollback 销毁后旧 savepoint 会自动失效（valid() 为假），
        //   不会访问已释放的状态。
        std::shared_ptr<State> m_state;

        // 本事务绑定的子 Context（由工厂创建的临时子节点）。
        // - 保护：只读，构造后不再改变。
        // - 不变式：用于对外暴露 context() 以及从 context()->Err() 取回
        //   自动回滚时的原因（此时 UndoActionWithCause 收到的是 nullptr）。
        ContextPtr m_context;

        // 保护下面两个回调 ID 的注销动作。
        // - 为何需要独立一把锁：DisarmCallback() 可能在析构路径上被调用，
        //   而它要修改本对象；若复用 State 的锁，会与"回调在锁外执行"
        //   的约定冲突（回调里可能再次触发 DisarmCallback）。
        // - mutable 的原因：DisarmCallback() 语义上不改变事务状态，
        //   仅解除登记，故可在 const 语境下调用。
        mutable std::mutex m_callback_mutex;

        // 注册在父/自身 DoneSignal 上的取消回调 ID；0 表示"未登记"，
        // 与 DoneSignal 用 0 作为无效 ID 的约定一致。DisarmCallback()
        // 会把它清零，因此"是否为 0"就是"回调是否仍然armed"的判定依据。
        DoneSignal::CallbackId m_callback_id{0};
        DoneSignal::CallbackId m_before_callback_id{0};

        void DisarmCallback() noexcept;
    };

    using RollbackScope = ContextRollback;

    ContextRollback WithRollback(const ContextPtr &parent,
                                 bool rollback_on_cancel = true);

    /** @brief 创建 Background 根 Context。 */
    ContextPtr Background();
    /** @brief 创建 TODO 根 Context。 */
    ContextPtr TODO();
    /** @brief 创建可取消子 Context。 */
    std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr &parent);
    /** @brief 创建可记录 Cause 的可取消子 Context。 */
    std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(
        const ContextPtr &parent);
    /** @brief 创建带截止时间的子 Context。 */
    std::pair<ContextPtr, CancelFunc> WithDeadline(const ContextPtr &parent,
                                                   ContextTimePoint deadline);
    /** @brief 创建带超时的子 Context。 */
    std::pair<ContextPtr, CancelFunc> WithTimeout(const ContextPtr &parent,
                                                  ContextDuration timeout);

    template <typename T>
    ContextPtr WithValue(const ContextPtr &parent, const ContextKey<T> &key, T value)
    {
        return Context::WithValue(parent, key, std::move(value));
    }

    /** @brief 创建携带字符串键值的子 Context。 */
    ContextPtr WithValue(const ContextPtr &parent, std::string key, std::any value);

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

} // namespace go2cpp