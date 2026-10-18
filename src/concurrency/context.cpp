/**
 * @file context.cpp
 * @brief Context 取消树与 DoneSignal 的实现。
 *
 * ## 这个文件实现什么
 *
 * - `Context` 的取消树：`WithCancel` / `WithTimeout` / `WithDeadline` 派生
 *   子节点，父节点取消时级联取消所有子节点；
 * - `DoneSignal`：回调注册与批量通知。Fiber 等待取消就挂在这里；
 * - 取消原因错误的构造与按身份比较。
 *
 * ## 关键设计
 *
 * **级联发生在取消线程上、同步执行**。也就是说 `cancel()` 返回时，所有子
 * Context 都已经被标记为完成、所有注册的回调都已执行。这让"取消后立刻
 * 观察状态"是可预期的，但也意味着**回调里不能做阻塞操作** ——
 * 典型写法是回调只 `Notify()` 一个等待原语。
 *
 * 取消是**单向且幂等**的：一旦完成就不会回退，重复取消是安全的空操作。
 */
#include "go2cpp/context.hpp"
#include "go2cpp/core/parking_condition.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <map>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp
{

    /**
     * @brief `DoneSignal` 的共享状态（句柄可复制，状态只有一份）。
     *
     * ## 定位
     *
     * `DoneSignal` 是对外句柄（可拷贝、可跨 Fiber 传递），真正被等待/通知的是本结构。
     * 使用者：`Context::State::done`（取消信号）、`ContextRollback`（自己的完成信号）、
     * `Future`/`Channel`/`Timer`（通过 `Context::Done()` 间接注册回调）。
     *
     * ## 状态机（只有两态）
     *
     * `Pending -(Signal())-> Done`：单向、幂等。`Signal()` 之后所有 `Wait*` 立即返回，
     * 再注册的回调会被立刻执行一次（见 `AddCallbackImpl`）。
     *
     * ## 两组回调的顺序
     *
     * `internal_callbacks` 先于 `callbacks`：`ContextRollback` 依赖它在用户观察到取消
     * 之前先冻结 undo 记录。两组都在**释放 mutex 之后**调用，避免回调重入信号对象死锁。
     */
    struct DoneSignal::State
    {
        /// @brief 保护 done 与两张回调表；Signal 与 AddCallback/RemoveCallback 竞争它。
        mutable std::mutex mutex;
        /// @brief 等待载体：受管 G 在此 park、普通线程走原生条件变量（ParkingCondition 分流）。
        mutable core::ParkingCondition cv;
        /// @brief 完成标志；置真后不可回退（单向）。
        bool done{false};
        /// @brief 下一个回调 Id（从 1 起，0 表示无效）。
        CallbackId next_id{1};
        // 内部回调（例如 ContextRollback）必须先于用户观察回调执行。
        // 两组回调仍然都在 Signal() 的锁外调用，避免回调重入信号对象。
        /// @brief 内部回调表（先执行）；由 AddCallbackImpl 写入、Signal 一次性搬出。
        std::unordered_map<CallbackId, std::function<void()>> internal_callbacks;
        /// @brief 用户回调表（后执行）；由 AddCallback/RemoveCallback 维护、Signal 搬出。
        std::unordered_map<CallbackId, std::function<void()>> callbacks;
    };

    DoneSignal::DoneSignal() : m_state(std::make_shared<State>()) {}
    DoneSignal::~DoneSignal() = default;

    /// @brief 无限等待直到信号完成。
    /// @details 步骤：
    ///          1. 先把 m_state 复制到局部变量 `state`。这不是可选优化：它让等待
    ///             期间即使本 DoneSignal 对象被销毁，内部状态仍被局部 shared_ptr
    ///             持有，避免"等到一半对象没了"的悬空。
    ///          2. 用 state->mutex 构造 unique_lock（该锁属于共享状态，不属于句柄）。
    ///          3. 以谓词式等待 state->done 成立。等待由 ParkingCondition 实现，
    ///             它区分执行流：受管 G 在此 park 并让出承载它的 M，普通线程走原生
    ///             条件变量；谓词形式天然免疫虚假唤醒。
    /// @note 没有超时上界；需要上界时用 WaitFor / WaitUntil。
    void DoneSignal::Wait() const
    {
        const auto state = m_state;
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [state]
                       { return state->done; });
    }

    /// @brief 等待至多 timeout，返回信号是否已完成。
    /// @details 与 Wait 相同的三步（复制 state → 用共享状态的锁 → 谓词等待），
    ///          区别只在第 3 步使用带相对时限的等待形式。
    /// @param[in] timeout 相对时长；到期即返回，返回值随即为 false。
    /// @return 返回时信号已完成则为 true；超时返回 false。
    /// @note 超时只结束"本次等待"，不改变信号状态——信号一旦完成就永久保持完成。
    bool DoneSignal::WaitFor(ContextDuration timeout) const
    {
        const auto state = m_state;
        std::unique_lock<std::mutex> lock(state->mutex);
        return state->cv.wait_for(lock, timeout, [state]
                                  { return state->done; });
    }

    /// @brief 等待到绝对截止时间，返回信号是否已完成。
    /// @details 步骤同 WaitFor，只是把相对时长换成本次等待的绝对时限。
    /// @param[in] deadline 基于 steady_clock 的绝对时间点。
    /// @return 返回时信号已完成则为 true；到期返回 false。
    /// @note 绝对时限便于在循环中重试而不会因每轮重新计时而无限延后。
    bool DoneSignal::WaitUntil(ContextTimePoint deadline) const
    {
        const auto state = m_state;
        std::unique_lock<std::mutex> lock(state->mutex);
        return state->cv.wait_until(lock, deadline, [state]
                                    { return state->done; });
    }

    /// @brief 非阻塞查询信号是否已完成。
    /// @details 只有一步：加锁读取 state->done 后立即返回。
    /// @return 已完成返回 true。
    /// @warning 本函数声明为 noexcept，但实现里会加锁（m_state->mutex）。锁竞争时
    ///          std::mutex::lock 可能抛 std::system_error，从而触发 std::terminate。
    ///          这是已知的接口与实现不匹配（台账 X-25 系列），保留现状是因为调用点
    ///          都在可接受终止的路径上；若要修，应去掉 noexcept 或改用原子读。
    bool DoneSignal::IsDone() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->done;
    }

    /// @brief 注册一个"仅观察用"的完成回调。
    /// @details 只做一件事：转交给 AddCallbackImpl(callback, /*internal=*/false)，
    ///          即登记到用户回调组（执行顺序在内部回调之后，见 Signal）。
    /// @param[in] callback 要注册的回调；为空时返回 0 且不做任何登记。
    /// @return 非 0 的回调 ID（可用于 RemoveCallback）；空回调返回 0。
    /// @note 若注册时信号已完成，回调会被立即调用而不是登记，且同样吞掉异常。
    DoneSignal::CallbackId DoneSignal::AddCallback(
        std::function<void()> callback) const
    {
        return AddCallbackImpl(std::move(callback), false);
    }

    /// @brief 注册回调的共同实现，按 internal 选择回调组。
    /// @details 步骤：
    ///          1. 空回调直接返回 0。
    ///          2. 加锁检查 state->done：
    ///             - 已完成：只置 invoke_now，不登记（回调改到锁外立即执行）。这样
    ///               "注册"与"完成"无论谁先到达，语义都是"回调恰好被调用一次"。
    ///             - 未完成：分配 id（自增后若为 0 则再取一次，保证 0 恒为无效哨兵），
    ///               按 internal 插入 internal_callbacks 或 callbacks。
    ///          3. 锁外执行 invoke_now 分支，并吞掉回调抛出的异常。
    /// @param[in] callback 要注册的回调。
    /// @param[in] internal true 表示内部回调组。内部组先于用户回调执行，供
    ///            ContextRollback 之类"必须抢在观察者之前"的组件使用。
    /// @return 已登记时返回其 ID；已完成为即时调用、或空回调时返回 0。
    /// @note 立即调用分支是安全的：取消状态已经完成线性化，观察者无法撤销取消，
    ///       因此允许回调抛异常而不回滚任何状态。
    DoneSignal::CallbackId DoneSignal::AddCallbackImpl(
        std::function<void()> callback, bool internal) const
    {
        if (!callback)
        {
            return 0;
        }
        CallbackId id = 0;
        bool invoke_now = false;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->done)
            {
                invoke_now = true;
            }
            else
            {
                id = m_state->next_id++;
                if (id == 0)
                {
                    id = m_state->next_id++;
                }
                auto &callbacks = internal ? m_state->internal_callbacks
                                           : m_state->callbacks;
                callbacks.emplace(id, std::move(callback));
            }
        }
        if (invoke_now)
        {
            // 通知回调只用于观察。取消状态已经完成线性化，因此有问题的观察者
            // 不能撤销取消操作。
            try
            {
                callback();
            }
            catch (...)
            {
            }
        }
        return id;
    }

    /// @brief 注销一个尚未被触发的回调。
    /// @details 步骤：
    ///          1. id 为 0 直接返回（0 是"未登记"哨兵，永远不是有效 ID）。
    ///          2. 加锁，从 internal_callbacks 与 callbacks 两组里都尝试 erase。
    /// @param[in] id AddCallback / AddCallbackImpl 返回的 ID。
    /// @note 两组都 erase，是因为调用方不必知道该 ID 属于哪一组。若回调已经在
    ///       Signal 中被"搬出容器并执行"，此处 erase 不生效，也不会中断正在执行的
    ///       回调——注销只保证"尚未触发的不再触发"。
    void DoneSignal::RemoveCallback(CallbackId id) const
    {
        if (id == 0)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->callbacks.erase(id);
        m_state->internal_callbacks.erase(id);
    }

    /// @brief 把信号置为完成，并按固定顺序通知所有等待者与回调。
    /// @details 步骤（顺序即契约，不可调换）：
    ///          1. 加锁；若 state->done 已为真则直接返回——信号只关闭一次，
    ///             重复 Signal 幂等。
    ///          2. 置 state->done = true，并把 internal_callbacks 与 callbacks 整体
    ///             "搬"进两个局部 vector，同时清空原容器。搬走而非原地遍历有两个
    ///             目的：回调在锁外执行，且每个回调恰好执行一次。
    ///          3. 释放锁后先 notify_all() 唤醒所有等待者。必须在回调之前，否则
    ///             等待者会被回调的执行时间拖住。
    ///          4. 执行 internal_callbacks（内部组先跑）。
    ///          5. 执行 callbacks（用户观察者后跑）。
    ///          4、5 两步对每个回调单独 try/catch 吞异常：一个观察者抛异常不得影响
    ///          其余回调，也不得改变"信号已完成"这一事实。
    /// @note internal 先于用户是本设计的关键——ContextRollback 依赖它在用户观察到
    ///       取消之前先冻结自己的 undo 记录。也正因为用户回调跑在之后，它才需要
    ///       另一个独立的 RollbackDone 信号来做自己的通知。
    /// @warning 声明为 noexcept，但实现会加锁（同属 X-25 系列）。
    void DoneSignal::Signal() const noexcept
    {
        // step1: 先把两组回调搬到局部 vector，锁内只做状态翻转与搬运，
        //        绝不在持锁状态下执行用户代码（回调可能重入本对象）。
        std::vector<std::function<void()>> internal_callbacks;
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->done)
            {
                return;  // 幂等：已完成则直接返回，不重复通知
            }
            m_state->done = true;
            internal_callbacks.reserve(m_state->internal_callbacks.size());
            for (auto &entry : m_state->internal_callbacks)
            {
                internal_callbacks.push_back(std::move(entry.second));
            }
            m_state->internal_callbacks.clear();
            callbacks.reserve(m_state->callbacks.size());
            for (auto &entry : m_state->callbacks)
            {
                callbacks.push_back(std::move(entry.second));
            }
            m_state->callbacks.clear();
        }
        // step2: 唤醒所有 Wait 者。放在执行回调之前，让等待方尽早恢复。
        m_state->cv.notify_all();
        // step3: 内部回调先执行（ContextRollback 依赖这个顺序）。
        for (auto &callback : internal_callbacks)
        {
            try
            {
                callback();
            }
            catch (...)
            {
            }
        }
        // step4: 用户回调后执行；单个回调抛异常被吞掉，不阻断其余回调。
        for (auto &callback : callbacks)
        {
            try
            {
                callback();
            }
            catch (...)
            {
            }
        }
    }

    namespace detail
    {

        /**
         * @brief 只给 `context.cpp` 内部用的 `DoneSignal` 访问桥。
         *
         * ## 为什么需要它
         *
         * `DoneSignal::Signal()` 与 `AddCallbackImpl(..., internal=true)` 都不是公开
         * 接口：内部回调组必须先于用户回调执行，不能让业务代码插到前面。但 `Context`
         * 的取消路径需要它们。用这个只暴露两个静态方法的桥，代替「把 `Context` 设为
         * `DoneSignal` 的 friend」—— 把「谁能触发信号」限制在最小范围。
         *
         * ## 语义提醒
         *
         * `AddInternalCallback` 注册的回调会在**所有用户回调之前**执行；如果信号
         * 已完成，它会**立即被调用一次**（不是登记后再等）。
         */
        struct DoneSignalAccess
        {
            static void Signal(DoneSignal &signal) noexcept { signal.Signal(); }

            static DoneSignal::CallbackId AddInternalCallback(
                const DoneSignal &signal, std::function<void()> callback)
            {
                return signal.AddCallbackImpl(std::move(callback), true);
            }
        };

    } // namespace detail

    namespace
    {

        std::mutex s_clock_mutex;
        Context::NowFunction s_now_function;

        ContextTimePoint SaturatingDeadline(ContextTimePoint now,
                                            ContextDuration timeout) noexcept
        {
            // 始终在时间点域中完成加法。特别是 ContextDuration::max() 不能溢出回绕
            // 到过去，否则会被误认为需要立即取消。
            using Rep = ContextDuration::rep;
            if (timeout > ContextDuration::zero() &&
                now > ContextTimePoint::max() - timeout)
            {
                return ContextTimePoint::max();
            }
            if (timeout < ContextDuration::zero())
            {
                // 对 duration::min() 取相反数会溢出。它已经小于所有可表示的时间点，
                // 因而要先显式处理，再执行下面本来安全的减法。
                if constexpr (std::numeric_limits<Rep>::is_signed)
                {
                    if (timeout.count() == std::numeric_limits<Rep>::min() ||
                        now < ContextTimePoint::min() - timeout)
                    {
                        return ContextTimePoint::min();
                    }
                }
            }
            return now + timeout;
        }

        /**
         * @brief Context 专用的小型定时器服务（与 `concurrency::TimerService` 不是同一个）。
         *
         * ## 定位
         *
         * `Context::WithDeadline/WithTimeout` 向它注册：到期时把对应 `State` 标记为
         * `DeadlineExceeded` 并走取消路径（级联到子节点）。它比
         * `concurrency::TimerService` 简单：只有一个固定线程、一个 `multimap`，
         * 没有 executor / 异常处理器 / 时长告警。
         *
         * ## 为什么是进程级单例且**永不析构**
         *
         * Context 状态可能在任意静态析构阶段被销毁。若本服务先析构，
         * `State::~State()` 移除挂起定时器时就会访问已析构的单例。
         * 所以用 `new` + `atexit(ShutdownAtExit)`：退出时先停 worker（避免
         * Memcheck 报存活线程），但**对象本身不 delete**。
         *
         * ## 与状态机文档的关系
         *
         * 它是 `docs/state-machines/Context.md` 里
         * `Active -(TimerService 到期)-> Canceled(DeadlineExceeded)` 这条转化的实现者。
         */
        class TimerService
        {
        public:
            using Id = std::uint64_t;

            static TimerService &Instance()
            {
                // Context 状态可能在任意静态析构阶段被销毁。让服务及其同步原语一直
                // 存活到进程退出，保证 State::~State() 移除挂起定时器时不会访问已经
                // 析构的单例。退出钩子会先停止 worker，避免 Memcheck 报告存活线程。
                static TimerService *service = []
                {
                    auto *value = new TimerService();
                    std::atexit(&TimerService::ShutdownAtExit);
                    return value;
                }();
                return *service;
            }

            TimerService(const TimerService &) = delete;
            TimerService &operator=(const TimerService &) = delete;

            Id Add(const std::shared_ptr<Context::State> &state,
                   ContextTimePoint deadline);
            void Remove(Id id);

        private:
            /// @brief 一条待触发的定时器；`state` 用 weak_ptr 避免延长 Context 生命周期。
            struct Entry
            {
                /// @brief 绝对到期时间；`Run` 取最早的一条决定 `wait_until` 的时长。
                ContextTimePoint deadline;
                /// @brief 定时器 Id；`Remove` 用它定位。
                Id id;
                /// @brief 目标 Context 状态；弱引用 —— Context 先释放则到期时直接跳过。
                std::weak_ptr<Context::State> state;
            };

            TimerService() : m_thread([this]
                                      { Run(); }) {}

            ~TimerService() { Shutdown(); }

            static void ShutdownAtExit() noexcept { Instance().Shutdown(); }

            void Shutdown() noexcept
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stopping = true;
                    m_entries.clear();
                }
                m_cv.notify_all();
                if (m_thread.joinable() &&
                    m_thread.get_id() != std::this_thread::get_id())
                {
                    m_thread.join();
                }
            }

            void Run();

            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::multimap<ContextTimePoint, Entry> m_entries;
            std::atomic<Id> m_next_id{1};
            bool m_stopping{false};
            std::thread m_thread;
        };

    } // namespace

    /**
     * @brief Context 取消树节点的共享状态（`ContextPtr` 背后的真身）。
     *
     * ## 定位
     *
     * 本结构只在 `context.cpp` 内部可见。对上层暴露的是 `Context` 门面：
     * `Future`、`Channel`、`Timer`、`io::IOManager` 通过 `Context::Done()`
     * 注册取消回调，通过 `Err()/Cause()` 判断取消原因；`Scheduler` 本身不
     * 感知 Context（它只负责 park/wake）。
     *
     * ## 取消树
     *
     * `WithCancel/WithTimeout/WithDeadline` 创建的子节点通过 `parent` 挂到父节点，
     * 父节点在 `children` 里记录**弱引用**（避免父子环导致谁都释放不掉）。
     * 父取消时沿 `children` 遍历逐个 Cancel，级联在**调用 cancel 的线程**上同步完成。
     *
     * ## 线程模型
     *
     * 除 `done` 有自己的同步外，全部字段由 `mutex` 保护。`cancel()` 返回时
     * 所有子节点都已是完成态；回调里不能做阻塞操作（典型写法只 `Notify()` 一个等待原语）。
     */
    struct Context::State : std::enable_shared_from_this<Context::State>
    {
        /// @brief 保护本结构除 `done` 外的全部字段；Cancel/WithValue/Err/Children 读写。
        mutable std::mutex mutex;
        /// @brief 父节点（根节点为空）；用于取消向上传播与 Values 向上查找。
        std::shared_ptr<State> parent;
        /// @brief 子节点弱引用集合；父取消时遍历逐个 Cancel。弱引用避免父子环。
        std::vector<std::weak_ptr<State>> children;
        /// @brief `WithValue` 的类型化键值表；键是类型标识对象的地址。
        std::unordered_map<const void *, std::any> typed_values;
        // 只要值仍然可达，就保留用于标识类型化 key 的分配对象。若没有这个所有者
        // 而仅按裸指针查找，分配器复用地址后可能错误地别名到无关的后续 key。
        /// @brief typed_values 的键所有者：持有 key 对象本身，防止地址被复用后键混淆。
        std::unordered_map<const void *, std::shared_ptr<const std::uint64_t>>
            typed_key_anchors;
        /// @brief `WithNamedValue` 的字符串键值表（诊断/配置这类按名字取值的场景）。
        std::unordered_map<std::string, std::any> named_values;
        /// @brief 完成信号：`Wait/WaitFor/AddCallback` 都挂在这里，是 Fiber 等待取消的唯一挂载点。
        DoneSignal done;
        /// @brief 取消原因（非空即已取消，幂等写入一次）；`Err()` 返回它。
        ErrorPtr error;
        /// @brief `WithCancelCause` 记录的首个原因；普通取消为空，`Cause()` 返回它。
        ErrorPtr cause;
        /// @brief 绝对截止时间（`WithDeadline/WithTimeout` 设置）；到期由定时器线程调 Cancel。
        std::optional<ContextTimePoint> deadline;
        /// @brief 定时器 Id（0 表示未注册）；Cancel 成功后用它 `TimerService::Remove`。
        TimerService::Id timer_id{0};
        /// @brief 是否是根节点（Background/TODO）；根节点不会因父级联被取消。
        bool root{false};
        /// @brief 下一个 before-done 回调 Id（从 1 起，0 是无效 Id）。
        DoneSignal::CallbackId next_before_done_id{1};
        /// @brief 在 `done.Signal()` 之前执行的回调表；用于取消前必须先改内部状态的钩子。
        std::unordered_map<DoneSignal::CallbackId, std::function<void()>>
            before_done_callbacks;

        ~State();

        struct CancellationWork
        {
            std::shared_ptr<State> state;
            ErrorPtr error;
            ErrorPtr cause;
            bool deadline_error{false};
            bool remove_from_parent{false};
        };

        DoneSignal::CallbackId AddBeforeDoneCallback(
            std::function<void()> callback)
        {
            if (!callback)
            {
                return 0;
            }
            DoneSignal::CallbackId id = 0;
            bool invoke_now = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (error)
                {
                    invoke_now = true;
                }
                else
                {
                    id = next_before_done_id++;
                    if (id == 0)
                    {
                        id = next_before_done_id++;
                    }
                    before_done_callbacks.emplace(id, std::move(callback));
                }
            }
            if (invoke_now)
            {
                try
                {
                    callback();
                }
                catch (...)
                {
                    // 取消前置钩子只用于改变内部状态，不能让 Cancel 抛出。
                }
            }
            return id;
        }

        void RemoveBeforeDoneCallback(DoneSignal::CallbackId id) noexcept
        {
            if (id == 0)
            {
                return;
            }
            std::lock_guard<std::mutex> lock(mutex);
            before_done_callbacks.erase(id);
        }

        bool IsCanceled() const noexcept
        {
            std::lock_guard<std::mutex> lock(mutex);
            return error != nullptr;
        }

        void RemoveChildRaw(const State *child)
        {
            std::lock_guard<std::mutex> lock(mutex);
            children.erase(
                std::remove_if(children.begin(), children.end(),
                               [child](const std::weak_ptr<State> &weak_child)
                               {
                                   const auto current = weak_child.lock();
                                   return !current || current.get() == child;
                               }),
                children.end());
        }

        void RemoveChild(const std::shared_ptr<State> &child)
        {
            if (child)
            {
                RemoveChildRaw(child.get());
            }
        }

        static void DrainCancellation(std::vector<CancellationWork> work)
        {
            // 调用用户回调前先标记完整子树。Done 回调可以观察或等待后代；如果
            // 先通知父回调，取消传播期间同步的 parent->child 等待可能死锁。
            std::vector<std::shared_ptr<State>> to_signal;
            std::vector<std::function<void()>> before_done_callbacks;
            while (!work.empty())
            {
                CancellationWork item = std::move(work.back());
                work.pop_back();
                if (!item.state)
                {
                    continue;
                }

                std::vector<std::shared_ptr<State>> descendants;
                ErrorPtr local_error = item.error;
                ErrorPtr local_cause = item.cause;
                TimerService::Id old_timer = 0;
                std::shared_ptr<State> parent_to_remove;
                std::vector<std::function<void()>> local_before_done_callbacks;
                {
                    std::lock_guard<std::mutex> lock(item.state->mutex);
                    if (item.state->root || item.state->error)
                    {
                        continue;
                    }
                    if (!local_error)
                    {
                        local_error = item.deadline_error ? DeadlineExceededError()
                                                          : CanceledError();
                    }
                    if (!local_cause)
                    {
                        local_cause = local_error;
                    }
                    item.state->error = local_error;
                    item.state->cause = local_cause;
                    old_timer = item.state->timer_id;
                    item.state->timer_id = 0;
                    local_before_done_callbacks.reserve(
                        item.state->before_done_callbacks.size());
                    for (auto &entry : item.state->before_done_callbacks)
                    {
                        local_before_done_callbacks.push_back(
                            std::move(entry.second));
                    }
                    item.state->before_done_callbacks.clear();
                    if (item.remove_from_parent)
                    {
                        parent_to_remove = item.state->parent;
                    }
                    for (const auto &weak_child : item.state->children)
                    {
                        if (auto child = weak_child.lock())
                        {
                            descendants.emplace_back(std::move(child));
                        }
                    }
                    item.state->children.clear();
                }

                if (old_timer != 0)
                {
                    TimerService::Instance().Remove(old_timer);
                }
                if (parent_to_remove)
                {
                    parent_to_remove->RemoveChild(item.state);
                }
                for (auto &callback : local_before_done_callbacks)
                {
                    before_done_callbacks.emplace_back(std::move(callback));
                }
                to_signal.emplace_back(item.state);
                for (auto &child : descendants)
                {
                    work.push_back(
                        {std::move(child), local_error, local_cause, false});
                }
            }
            // 所有 Context 状态都已先标记取消，再让 rollback scope 冻结其
            // undo。这样 Done 的观察者、RecordUndo 和 Commit 不会在取消窗口
            // 中错误地把新动作当成可提交状态。真正的补偿由 DoneSignal 的
            // 内部回调在 done=true、普通用户回调之前执行。
            for (auto it = before_done_callbacks.rbegin();
                 it != before_done_callbacks.rend(); ++it)
            {
                try
                {
                    (*it)();
                }
                catch (...)
                {
                }
            }
            for (auto it = to_signal.rbegin(); it != to_signal.rend(); ++it)
            {
                (*it)->done.Signal();
            }
        }

        void AddChild(const std::shared_ptr<State> &child)
        {
            ErrorPtr inherited_error;
            ErrorPtr inherited_cause;
            {
                std::lock_guard<std::mutex> lock(mutex);
                children.erase(std::remove_if(children.begin(), children.end(),
                                              [](const auto &item)
                                              {
                                                  return item.expired();
                                              }),
                               children.end());
                if (!error)
                {
                    children.emplace_back(child);
                    return;
                }
                inherited_error = error;
                inherited_cause = cause;
            }
            child->CancelFromParent(std::move(inherited_error),
                                    std::move(inherited_cause));
        }

        void Cancel(ErrorPtr requested_cause, bool deadline_error = false)
        {
            std::vector<CancellationWork> work;
            work.push_back({shared_from_this(), {}, std::move(requested_cause), deadline_error, true});
            DrainCancellation(std::move(work));
        }

        void CancelFromParent(ErrorPtr inherited_error, ErrorPtr inherited_cause)
        {
            std::vector<CancellationWork> work;
            work.push_back({shared_from_this(), std::move(inherited_error),
                            std::move(inherited_cause), false, false});
            DrainCancellation(std::move(work));
        }

        std::any Lookup(const void *token, const std::string &name) const
        {
            auto owner = const_cast<State *>(this)->shared_from_this();
            while (owner)
            {
                std::shared_ptr<State> parent_owner;
                {
                    std::lock_guard<std::mutex> lock(owner->mutex);
                    if (token != nullptr)
                    {
                        const auto found = owner->typed_values.find(token);
                        if (found != owner->typed_values.end())
                        {
                            return found->second;
                        }
                    }
                    if (token == nullptr)
                    {
                        const auto found = owner->named_values.find(name);
                        if (found != owner->named_values.end())
                        {
                            return found->second;
                        }
                    }
                    parent_owner = owner->parent;
                }
                owner = std::move(parent_owner);
            }
            return std::any{};
        }
    };

    namespace
    {

        // child 持有父状态，因此调用者释放父句柄后，取消状态和值仍然有效。但普通
        // shared_ptr 析构会递归释放很长的父链。把每个父对象移入线程本地释放队列，
        // 再由自定义删除器迭代排空，避免深层 Context 树耗尽原生栈。
        thread_local bool t_draining_state_releases = false;
        thread_local std::vector<std::shared_ptr<Context::State>>
            t_state_release_queue;

        void QueueStateParent(std::shared_ptr<Context::State> parent) noexcept
        {
            if (parent)
            {
                t_state_release_queue.emplace_back(std::move(parent));
            }
        }

        struct StateDeleter
        {
            void operator()(Context::State *state) const noexcept
            {
                delete state;
                if (t_draining_state_releases)
                {
                    return;
                }
                t_draining_state_releases = true;
                while (!t_state_release_queue.empty())
                {
                    auto next = std::move(t_state_release_queue.back());
                    t_state_release_queue.pop_back();
                    next.reset();
                }
                t_draining_state_releases = false;
            }
        };

        std::shared_ptr<Context::State> MakeState()
        {
            return std::shared_ptr<Context::State>(new Context::State(),
                                                   StateDeleter{});
        }

    } // namespace

    Context::State::~State()
    {
        TimerService::Id old_timer = 0;
        std::shared_ptr<State> retained_parent;
        {
            std::lock_guard<std::mutex> lock(mutex);
            old_timer = timer_id;
            timer_id = 0;
            children.clear();
            retained_parent = std::move(parent);
        }
        if (old_timer != 0)
        {
            TimerService::Instance().Remove(old_timer);
        }
        if (retained_parent)
        {
            // 未取消的短命 child 也要及时移除 parent 的 weak 登记，避免长期
            // 只创建、不再创建新 child 时登记表单调增长。
            retained_parent->RemoveChildRaw(this);
        }
        QueueStateParent(std::move(retained_parent));
    }

    struct ContextRollback::State : std::enable_shared_from_this<State>
    {
        // 内部统一按「接收原因」的签名存储，因此带原因与不带原因的补偿动作
        // 共用同一条 LIFO 队列；旧式 UndoAction 只需在登记时包一层适配。
        using Action = std::function<void(const ErrorPtr &)>;
        mutable std::mutex mutex;
        std::weak_ptr<Context::State> context_state;
        std::vector<Action> actions;
        Status status{Status::kActive};
        std::uint64_t generation{1};
        bool rollback_on_cancel{true};
        bool abort_requested{false};
        bool context_cancel_claimed{false};
        bool manual_rollback_claimed{false};
        bool had_failure{false};
        std::exception_ptr first_failure;
        std::vector<std::exception_ptr> failures;
        ErrorPtr manual_cause;
        std::vector<Action> pending_context_actions;
        DoneSignal completion;

        bool ContextCanceled() const noexcept
        {
            const auto context = context_state.lock();
            return context && context->IsCanceled();
        }

        bool Record(UndoAction action, std::string *reason)
        {
            if (!action)
            {
                if (reason)
                {
                    *reason = "补偿动作为空";
                }
                return false;
            }
            return RecordImpl(Action([stored = std::move(action)](const ErrorPtr &)
                                     { stored(); }),
                              reason);
        }

        bool RecordWithCause(UndoActionWithCause action, std::string *reason)
        {
            if (!action)
            {
                if (reason)
                {
                    *reason = "补偿动作为空";
                }
                return false;
            }
            return RecordImpl(Action(std::move(action)), reason);
        }

        bool RecordImpl(Action action, std::string *reason)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (status != Status::kActive)
            {
                if (reason)
                {
                    *reason = status == Status::kCommitted ? "事务已提交"
                                                           : "事务已结束（回滚中或已回滚）";
                }
                return false;
            }
            if (rollback_on_cancel && ContextCanceled())
            {
                status = Status::kRollingBack;
                pending_context_actions.swap(actions);
                context_cancel_claimed = true;
                if (reason)
                {
                    *reason = "child Context 已被取消，事务转入回滚";
                }
                return false;
            }
            actions.emplace_back(std::move(action));
            return true;
        }

        std::size_t PendingCount() const noexcept
        {
            try
            {
                std::lock_guard<std::mutex> lock(mutex);
                return actions.size() + pending_context_actions.size();
            }
            catch (...)
            {
                return 0U;
            }
        }

        std::vector<std::exception_ptr> Failures() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return failures;
        }

        ErrorPtr ManualCause() const noexcept
        {
            try
            {
                std::lock_guard<std::mutex> lock(mutex);
                return manual_cause;
            }
            catch (...)
            {
                return {};
            }
        }

        ContextRollback::Savepoint Mark() const noexcept
        {
            ContextRollback::Savepoint mark;
            std::lock_guard<std::mutex> lock(mutex);
            if (status == Status::kActive &&
                (!rollback_on_cancel || !ContextCanceled()))
            {
                mark.m_state = const_cast<State *>(this)->shared_from_this();
                mark.m_depth = actions.size();
                mark.m_generation = generation;
            }
            return mark;
        }

        bool BeginPartial(const ContextRollback::Savepoint &mark,
                          std::vector<Action> &extracted)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (status == Status::kRollingBack)
            {
                abort_requested = true;
                return false;
            }
            if (status != Status::kActive || mark.m_generation != generation ||
                mark.m_depth > actions.size())
            {
                return false;
            }
            if (rollback_on_cancel && ContextCanceled())
            {
                status = Status::kRollingBack;
                pending_context_actions.swap(actions);
                context_cancel_claimed = true;
                return false;
            }
            extracted.reserve(actions.size() - mark.m_depth);
            status = Status::kRollingBack;
            while (actions.size() > mark.m_depth)
            {
                extracted.emplace_back(std::move(actions.back()));
                actions.pop_back();
            }
            std::reverse(extracted.begin(), extracted.end());
            return true;
        }

        void RecordFailure(std::exception_ptr failure) noexcept
        {
            if (!failure)
            {
                return;
            }
            // 本函数在 noexcept 路径上被调用（Execute 与取消回调），因此
            // 加锁失败、容器扩容失败都不能向外抛 —— 它是最后一道记录手段，
            // 宁可丢掉部分诊断信息，也不能让进程 terminate。
            try
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (status == Status::kCommitted || status == Status::kRolledBack ||
                    status == Status::kFailed)
                {
                    return;
                }
                had_failure = true;
                // first_failure 保持「只记第一个」的历史语义（failure() 依赖它）。
                // 先写它：exception_ptr 赋值不会抛，这样即使随后的 failures
                // 扩容失败，failure()/had_failure() 这一对仍然自洽。
                if (!first_failure)
                {
                    first_failure = failure;
                }
                // 再累积全部失败，供 failures() 完整诊断；这一步可能抛。
                failures.push_back(failure);
            }
            catch (...)
            {
                // 尽力而为：诊断记录失败不影响已经完成的状态转换。
            }
        }

        void Execute(std::vector<Action> &extracted, const ErrorPtr &cause) noexcept
        {
            for (auto it = extracted.rbegin(); it != extracted.rend(); ++it)
            {
                if (!*it)
                {
                    continue;
                }
                try
                {
                    (*it)(cause);
                }
                catch (...)
                {
                    RecordFailure(std::current_exception());
                }
            }
            extracted.clear();
        }

        bool FinishPartial(std::vector<Action> &remaining) noexcept
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (abort_requested)
            {
                abort_requested = false;
                remaining.swap(actions);
                return true;
            }
            status = Status::kActive;
            // savepoint 是一次性令牌。即使 vector 深度随后恢复到原值，
            // 旧令牌也不能重新解释新登记的动作，避免 ABA。
            if (++generation == 0)
            {
                generation = 1;
            }
            return false;
        }

        void FinishFull() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                status = had_failure ? Status::kFailed : Status::kRolledBack;
                if (++generation == 0)
                {
                    generation = 1;
                }
                abort_requested = false;
            }
            detail::DoneSignalAccess::Signal(completion);
        }

        void ClaimContextCancellation() noexcept
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!rollback_on_cancel || status == Status::kCommitted ||
                status == Status::kRolledBack || status == Status::kFailed)
            {
                return;
            }
            if (status == Status::kRollingBack)
            {
                if (!manual_rollback_claimed)
                {
                    abort_requested = true;
                }
                return;
            }
            status = Status::kRollingBack;
            pending_context_actions.swap(actions);
            context_cancel_claimed = true;
        }

        // 取消前置钩子只负责冻结动作日志。真正的补偿在 DoneSignal 的
        // 内部回调中执行：此时 Done 已经线性化并唤醒等待者，但普通用户
        // 回调尚未开始，因此等待 RollbackDone 不会和内部回调互相阻塞。
        void ClaimContextCancellationOnly() noexcept { ClaimContextCancellation(); }

        bool BeginManualRollback(const ErrorPtr &cause) noexcept
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (status == Status::kCommitted || status == Status::kRolledBack ||
                status == Status::kFailed)
            {
                return false;
            }
            if (status == Status::kRollingBack)
            {
                abort_requested = true;
                return false;
            }
            // 记录显式回滚原因（拷贝，调用方随后仍要把它交给 Cancel）；
            // 自动回滚路径不设置它，调用方可从 context()->Err() 取得原因。
            if (!manual_cause)
            {
                manual_cause = cause;
            }
            status = Status::kRollingBack;
            pending_context_actions.swap(actions);
            manual_rollback_claimed = true;
            return true;
        }

        void OnContextDone() noexcept
        {
            std::vector<Action> extracted;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (status == Status::kCommitted || status == Status::kRolledBack ||
                    status == Status::kFailed)
                {
                    return;
                }
                if (manual_rollback_claimed)
                {
                    manual_rollback_claimed = false;
                    extracted.swap(pending_context_actions);
                }
                else if (context_cancel_claimed)
                {
                    if (!rollback_on_cancel)
                    {
                        return;
                    }
                    context_cancel_claimed = false;
                    extracted.swap(pending_context_actions);
                }
                else if (status == Status::kRollingBack)
                {
                    if (!rollback_on_cancel)
                    {
                        return;
                    }
                    abort_requested = true;
                    return;
                }
                else
                {
                    if (!rollback_on_cancel)
                    {
                        return;
                    }
                    status = Status::kRollingBack;
                    extracted.swap(actions);
                }
            }
            Execute(extracted, ManualCause());
            FinishFull();
        }
    };

    // ContextRollback 的公开方法定义放在 Context::State 完整定义之后，
    // 这样回调只依赖自己的共享状态，不需要暴露 Context 的内部节点。
    ContextRollback::ContextRollback(ContextPtr parent, bool rollback_on_cancel)
        : m_state(std::make_shared<State>())
    {
        m_state->rollback_on_cancel = rollback_on_cancel;
        auto child_pair = Context::WithCancel(parent);
        m_context = std::move(child_pair.first);
        m_state->context_state = m_context->m_state;

        const std::weak_ptr<State> weak_state(m_state);
        try
        {
            m_before_callback_id = m_context->AddBeforeDoneCallback([weak_state]
                                                                    {
            if (const auto state = weak_state.lock()) {
                state->ClaimContextCancellationOnly();
            } });
            m_callback_id = detail::DoneSignalAccess::AddInternalCallback(
                m_context->Done(), [weak_state]
                {
            if (const auto state = weak_state.lock()) {
                state->OnContextDone();
            } });
        }
        catch (...)
        {
            DisarmCallback();
            try
            {
                m_context->Cancel();
            }
            catch (...)
            {
            }
            throw;
        }
    }

    ContextRollback::~ContextRollback() noexcept
    {
        if (m_state)
        {
            (void)Rollback();
        }
    }

    ContextRollback::ContextRollback(ContextRollback &&other) noexcept
    {
        std::lock_guard<std::mutex> lock(other.m_callback_mutex);
        m_state = std::move(other.m_state);
        m_context = std::move(other.m_context);
        m_callback_id = other.m_callback_id;
        m_before_callback_id = other.m_before_callback_id;
        other.m_callback_id = 0;
        other.m_before_callback_id = 0;
    }

    ContextRollback &ContextRollback::operator=(ContextRollback &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        DisarmCallback();
        if (m_state)
        {
            (void)Rollback();
        }
        {
            std::scoped_lock lock(m_callback_mutex, other.m_callback_mutex);
            m_state = std::move(other.m_state);
            m_context = std::move(other.m_context);
            m_callback_id = other.m_callback_id;
            m_before_callback_id = other.m_before_callback_id;
            other.m_callback_id = 0;
            other.m_before_callback_id = 0;
        }
        return *this;
    }

    void ContextRollback::DisarmCallback() noexcept
    {
        ContextPtr context;
        DoneSignal::CallbackId callback_id = 0;
        DoneSignal::CallbackId before_callback_id = 0;
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            callback_id = m_callback_id;
            before_callback_id = m_before_callback_id;
            m_callback_id = 0;
            m_before_callback_id = 0;
            context = m_context;
        }
        if (context && before_callback_id != 0)
        {
            context->RemoveBeforeDoneCallback(before_callback_id);
        }
        if (context && callback_id != 0)
        {
            context->Done().RemoveCallback(callback_id);
        }
    }

    bool ContextRollback::RecordUndo(UndoAction action, std::string *reason)
    {
        if (!m_state)
        {
            if (reason)
            {
                *reason = "事务状态不存在（对象已被移动）";
            }
            return false;
        }
        return m_state->Record(std::move(action), reason);
    }

    bool ContextRollback::RecordUndoWithCause(UndoActionWithCause action,
                                              std::string *reason)
    {
        if (!m_state)
        {
            if (reason)
            {
                *reason = "事务状态不存在（对象已被移动）";
            }
            return false;
        }
        return m_state->RecordWithCause(std::move(action), reason);
    }

    bool ContextRollback::Savepoint::valid() const noexcept
    {
        const auto state = m_state.lock();
        if (!state)
        {
            return false;
        }
        try
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->status == Status::kActive &&
                   state->generation == m_generation &&
                   m_depth <= state->actions.size() &&
                   (!state->rollback_on_cancel || !state->ContextCanceled());
        }
        catch (...)
        {
            // 查询型 noexcept 接口：拿不到锁时保守地报告「令牌无效」。
            return false;
        }
    }

    ContextRollback::Savepoint ContextRollback::Mark() const noexcept
    {
        return m_state ? m_state->Mark() : Savepoint{};
    }

    bool ContextRollback::RollbackTo(const Savepoint &mark) noexcept
    {
        if (!m_state || mark.m_state.lock() != m_state)
        {
            return false;
        }
        std::vector<State::Action> extracted;
        try
        {
            if (!m_state->BeginPartial(mark, extracted))
            {
                return false;
            }
        }
        catch (...)
        {
            m_state->RecordFailure(std::current_exception());
            return false;
        }
        // 局部回滚没有单一的回滚原因，带原因的补偿动作会收到空 cause。
        m_state->Execute(extracted, {});

        std::vector<State::Action> remaining;
        if (m_state->FinishPartial(remaining))
        {
            m_state->Execute(remaining, {});
            m_state->FinishFull();
            DisarmCallback();
        }
        return true;
    }

    bool ContextRollback::Rollback(ErrorPtr cause) noexcept
    {
        if (!m_state)
        {
            return false;
        }
        const bool owner = m_state->BeginManualRollback(cause);
        if (!owner)
        {
            Status current;
            {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                current = m_state->status;
            }
            if (current == Status::kCommitted)
            {
                return false;
            }
            bool cancellation_failed = false;
            if (m_context)
            {
                try
                {
                    m_context->Cancel(std::move(cause));
                }
                catch (...)
                {
                    m_state->RecordFailure(std::current_exception());
                    cancellation_failed = true;
                }
            }
            // 无论最初读取状态时是否已经看到 claim，取消和 claim 可能在
            // 此期间交错；等待 Done 后再尝试领取 pending actions，避免随后
            // 注销回调把唯一的执行路径移除后留下永久等待。若另一个 owner
            // 正在执行，OnContextDone 只会设置 abort_requested 或直接返回。
            if (m_context && !cancellation_failed && !m_context->Done().IsDone())
            {
                // 另一个取消拥有者可能已经写入 Err 但尚未发布 Done；先等
                // 取消线性化，再领取 pending undo，保持统一的观察顺序。
                m_context->Done().Wait();
            }
            m_state->OnContextDone();
            DisarmCallback();
            return true;
        }

        bool cancellation_failed = false;
        if (m_context)
        {
            try
            {
                m_context->Cancel(std::move(cause));
            }
            catch (...)
            {
                m_state->RecordFailure(std::current_exception());
                cancellation_failed = true;
            }
        }
        // 正常路径由 DoneSignal 的内部回调领取并执行 pending actions；
        // Cancel 失败、Context 已经在回调注销窗口完成等异常交错，则由
        // 当前线程兜底。OnContextDone 对重复调用是幂等的。
        if (m_context && !cancellation_failed && !m_context->Done().IsDone())
        {
            // 另一个线程可能已经写入 Err，但还没有完成 Done.Signal。等待
            // 取消线性化，避免在 Done 之前直接执行可能观察 Done 的 undo。
            m_context->Done().Wait();
        }
        m_state->OnContextDone();
        DisarmCallback();
        return true;
    }

    bool ContextRollback::Commit() noexcept
    {
        if (!m_state)
        {
            return false;
        }
        bool committed = false;
        bool context_claimed = false;
        std::vector<State::Action> discarded;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->status == Status::kActive && !m_state->had_failure)
            {
                if (m_state->rollback_on_cancel && m_state->ContextCanceled())
                {
                    m_state->status = Status::kRollingBack;
                    m_state->pending_context_actions.swap(m_state->actions);
                    m_state->context_cancel_claimed = true;
                    context_claimed = true;
                }
                else
                {
                    discarded.swap(m_state->actions);
                    m_state->status = Status::kCommitted;
                    if (++m_state->generation == 0)
                    {
                        m_state->generation = 1;
                    }
                    committed = true;
                }
            }
            else if (m_state->status == Status::kCommitted)
            {
                committed = true;
            }
        }
        if (context_claimed)
        {
            // 如果 Done 尚未发布，保留内部回调；它会在取消线程完成
            // 线性化后执行 pending undo。Done 已发布时才需要当前线程兜底。
            if (m_context && m_context->Done().IsDone())
            {
                m_state->OnContextDone();
                DisarmCallback();
            }
            return false;
        }
        if (committed)
        {
            DisarmCallback();
            // 丢弃动作对象本身可能释放临时资源；在通知等待者前完成析构，
            // 让 RollbackDone 覆盖整个提交清理过程，而不只是状态切换。
            discarded.clear();
            detail::DoneSignalAccess::Signal(m_state->completion);
        }
        return committed;
    }

    ContextRollback::Status ContextRollback::status() const noexcept
    {
        if (!m_state)
        {
            return Status::kRolledBack;
        }
        // 本接口是 noexcept，且 active()/committed()/rolled_back()/status_name()
        // 全部依赖它；加锁失败时必须给出确定答案而不是终止进程。
        try
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_state->status;
        }
        catch (...)
        {
            return Status::kRolledBack;
        }
    }

    bool ContextRollback::active() const noexcept
    {
        return status() == Status::kActive;
    }

    bool ContextRollback::committed() const noexcept
    {
        return status() == Status::kCommitted;
    }

    bool ContextRollback::rolled_back() const noexcept
    {
        const auto current = status();
        return current == Status::kRolledBack || current == Status::kFailed;
    }

    bool ContextRollback::had_failure() const noexcept
    {
        if (!m_state)
        {
            return false;
        }
        try
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_state->had_failure;
        }
        catch (...)
        {
            return false;
        }
    }

    std::exception_ptr ContextRollback::failure() const noexcept
    {
        if (!m_state)
        {
            return {};
        }
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->first_failure;
    }

    std::size_t ContextRollback::pending_undo_count() const noexcept
    {
        return m_state ? m_state->PendingCount() : 0U;
    }

    std::vector<std::exception_ptr> ContextRollback::failures() const
    {
        if (!m_state)
        {
            return {};
        }
        return m_state->Failures();
    }

    ErrorPtr ContextRollback::rollback_cause() const noexcept
    {
        if (!m_state)
        {
            return {};
        }
        // 显式 Rollback(cause) 记在事务状态上。
        if (const auto manual = m_state->ManualCause())
        {
            return manual;
        }
        // 只有确实进入过回滚的事务才有「回滚原因」。已提交（或仍活动）的事务
        // 即使 child Context 之后被取消，也不应把那次取消报告成本事务的回滚
        // 原因 —— 否则 commit 之后父级取消会让本接口返回与本事务无关的错误。
        const auto current = status();
        if (current == Status::kCommitted || current == Status::kActive)
        {
            return {};
        }
        // 自动回滚（父取消 / deadline 到期）没有显式原因，回退到 child Context 的 Err。
        if (!m_context)
        {
            return {};
        }
        try
        {
            return m_context->Err();
        }
        catch (...)
        {
            return {};
        }
    }

    const char *ContextRollback::StatusName(Status status) noexcept
    {
        switch (status)
        {
        case Status::kActive:
            return "active";
        case Status::kRollingBack:
            return "rolling_back";
        case Status::kCommitted:
            return "committed";
        case Status::kRolledBack:
            return "rolled_back";
        case Status::kFailed:
            return "failed";
        default:
            // 显式 default：越界的 Status（例如日志里打印反序列化结果）应给出
            // 确定答案，而不是依赖「枚举值总是有效」这一假设。
            return "unknown";
        }
    }

    DoneSignal ContextRollback::RollbackDone() const
    {
        if (m_state)
        {
            return m_state->completion;
        }

        // 移动后的空对象没有自己的事务状态；返回一个已经完成的信号，
        // 这样通用清理代码等待它时不会永久阻塞。
        static const DoneSignal completed = []
        {
            DoneSignal signal;
            detail::DoneSignalAccess::Signal(signal);
            return signal;
        }();
        return completed;
    }

    namespace
    {

        TimerService::Id TimerService::Add(const std::shared_ptr<Context::State> &state,
                                           ContextTimePoint deadline)
        {
            const Id id = m_next_id.fetch_add(1, std::memory_order_relaxed);
            const Id actual_id = id == 0 ? m_next_id.fetch_add(1, std::memory_order_relaxed)
                                         : id;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping)
                {
                    return 0;
                }
                m_entries.emplace(deadline, Entry{deadline, actual_id, state});
            }
            m_cv.notify_all();
            return actual_id;
        }

        void TimerService::Remove(Id id)
        {
            if (id == 0)
            {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto it = m_entries.begin(); it != m_entries.end();)
                {
                    if (it->second.id == id)
                    {
                        it = m_entries.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            m_cv.notify_all();
        }

        void TimerService::Run()
        {
            for (;;)
            {
                std::vector<std::weak_ptr<Context::State>> expired;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    if (m_stopping)
                    {
                        return;
                    }
                    if (m_entries.empty())
                    {
                        m_cv.wait(lock, [this]
                                  { return m_stopping || !m_entries.empty(); });
                        continue;
                    }
                    const auto deadline = m_entries.begin()->first;
                    if (std::chrono::steady_clock::now() < deadline)
                    {
                        m_cv.wait_until(lock, deadline);
                        continue;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    while (!m_entries.empty() && m_entries.begin()->first <= now)
                    {
                        expired.emplace_back(m_entries.begin()->second.state);
                        m_entries.erase(m_entries.begin());
                    }
                }
                for (auto &weak_state : expired)
                {
                    if (auto state = weak_state.lock())
                    {
                        state->Cancel({}, true);
                    }
                }
            }
        }

    } // namespace

    Context::Context(std::shared_ptr<State> state)
        : m_state(std::move(state)), m_done(m_state->done) {}

    DoneSignal::CallbackId Context::AddBeforeDoneCallback(
        std::function<void()> callback)
    {
        return m_state->AddBeforeDoneCallback(std::move(callback));
    }

    void Context::RemoveBeforeDoneCallback(DoneSignal::CallbackId id) const noexcept
    {
        m_state->RemoveBeforeDoneCallback(id);
    }

    ContextPtr Context::Background()
    {
        static const ContextPtr root = []
        {
            auto state = MakeState();
            state->root = true;
            return ContextPtr(new Context(std::move(state)));
        }();
        return root;
    }

    ContextPtr Context::TODO()
    {
        static const ContextPtr root = []
        {
            auto state = MakeState();
            state->root = true;
            return ContextPtr(new Context(std::move(state)));
        }();
        return root;
    }

    ContextPtr Context::MakeChild(const ContextPtr &parent)
    {
        // Go 会拒绝 nil parent；转译后的 C++ 使用显式 root，使操作保持内存安全
        // 且不抛出异常。
        const auto actual_parent = parent ? parent : Background();
        auto state = MakeState();
        state->parent = actual_parent->m_state; // child retains its parent anchor
        {
            std::lock_guard<std::mutex> lock(actual_parent->m_state->mutex);
            state->deadline = actual_parent->m_state->deadline;
        }
        auto child = ContextPtr(new Context(state));
        actual_parent->m_state->AddChild(state);
        return child;
    }

    ContextPtr Context::MakeValueContext(const ContextPtr &parent, const void *token,
                                         const std::string &name,
                                         std::shared_ptr<const std::uint64_t>
                                             token_anchor,
                                         std::any value)
    {
        auto child = MakeChild(parent);
        try
        {
            std::lock_guard<std::mutex> lock(child->m_state->mutex);
            if (token != nullptr)
            {
                child->m_state->typed_values[token] = std::move(value);
                child->m_state->typed_key_anchors[token] = std::move(token_anchor);
            }
            else
            {
                child->m_state->named_values[name] = std::move(value);
            }
        }
        catch (...)
        {
            try
            {
                child->Cancel();
            }
            catch (...)
            {
            }
            throw;
        }
        return child;
    }

    std::pair<ContextPtr, CancelFunc> Context::WithCancel(const ContextPtr &parent)
    {
        auto child = MakeChild(parent);
        const auto state = child->m_state;
        return {std::move(child), [state]
                { state->Cancel({}); }};
    }

    std::pair<ContextPtr, CancelCauseFunc> Context::WithCancelCause(
        const ContextPtr &parent)
    {
        auto child = MakeChild(parent);
        const auto state = child->m_state;
        return {std::move(child), [state](ErrorPtr cause)
                {
                    state->Cancel(std::move(cause));
                }};
    }

    std::pair<ContextPtr, CancelFunc> Context::WithDeadline(
        const ContextPtr &parent, ContextTimePoint requested_deadline)
    {
        auto child = MakeChild(parent);
        const auto inherited = child->Deadline();
        if (!inherited.has_value() || requested_deadline < *inherited)
        {
            {
                std::lock_guard<std::mutex> lock(child->m_state->mutex);
                if (child->m_state->error)
                {
                    // 创建 child 的过程中，父 Context 赢得了取消竞争。
                    requested_deadline = ContextTimePoint::max();
                }
                else
                {
                    child->m_state->deadline = requested_deadline;
                }
            }
            if (requested_deadline != ContextTimePoint::max())
            {
                if (requested_deadline <= Now())
                {
                    // Go 会在返回已经过期的 Context 前关闭它；这里同步完成该操作，
                    // 不依赖 timer worker 的下一次调度。
                    child->m_state->Cancel({}, true);
                }
                else
                {
                    TimerService::Id id = 0;
                    try
                    {
                        id = TimerService::Instance().Add(child->m_state,
                                                          requested_deadline);
                    }
                    catch (...)
                    {
                        try
                        {
                            child->Cancel();
                        }
                        catch (...)
                        {
                        }
                        throw;
                    }
                    if (id == 0)
                    {
                        // TimerService 正在停止，不能留下一个永远不会触发的
                        // deadline child；把注册失败转换为显式 deadline 取消。
                        child->m_state->Cancel({}, true);
                    }
                    else
                    {
                        std::lock_guard<std::mutex> lock(child->m_state->mutex);
                        if (!child->m_state->error)
                        {
                            child->m_state->timer_id = id;
                        }
                        else
                        {
                            TimerService::Instance().Remove(id);
                        }
                    }
                }
            }
        }
        const auto state = child->m_state;
        return {std::move(child), [state]
                { state->Cancel({}); }};
    }

    std::pair<ContextPtr, CancelFunc> Context::WithTimeout(
        const ContextPtr &parent, ContextDuration timeout)
    {
        return WithDeadline(parent, SaturatingDeadline(Now(), timeout));
    }

    ContextPtr Context::WithValue(const ContextPtr &parent, std::string key,
                                  std::any value)
    {
        return MakeValueContext(parent, nullptr, key, {}, std::move(value));
    }

    bool Context::IsDone() const noexcept { return m_state->done.IsDone(); }

    ErrorPtr Context::Err() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->error;
    }

    ErrorPtr Context::Cause() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->cause;
    }

    std::optional<ContextTimePoint> Context::Deadline() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->deadline;
    }

    bool Context::HasDeadline() const { return Deadline().has_value(); }

    std::any Context::LookupValue(const void *token, const std::string &name) const
    {
        return m_state->Lookup(token, name);
    }

    std::any Context::Value(const std::string &key) const
    {
        return LookupValue(nullptr, key);
    }

    void Context::Cancel(ErrorPtr cause) { m_state->Cancel(std::move(cause)); }

    void Context::CancelDeadline() { m_state->Cancel({}, true); }

    void Context::SetNowFunctionForTesting(NowFunction now)
    {
        std::lock_guard<std::mutex> lock(s_clock_mutex);
        s_now_function = std::move(now);
    }

    void Context::ResetNowFunctionForTesting()
    {
        std::lock_guard<std::mutex> lock(s_clock_mutex);
        s_now_function = {};
    }

    ContextTimePoint Context::Now()
    {
        NowFunction now;
        {
            std::lock_guard<std::mutex> lock(s_clock_mutex);
            now = s_now_function;
        }
        return now ? now() : Clock::now();
    }

    ContextPtr Background() { return Context::Background(); }
    ContextPtr TODO() { return Context::TODO(); }
    std::pair<ContextPtr, CancelFunc> WithCancel(const ContextPtr &parent)
    {
        return Context::WithCancel(parent);
    }
    std::pair<ContextPtr, CancelCauseFunc> WithCancelCause(const ContextPtr &parent)
    {
        return Context::WithCancelCause(parent);
    }
    std::pair<ContextPtr, CancelFunc> WithDeadline(const ContextPtr &parent,
                                                   ContextTimePoint deadline)
    {
        return Context::WithDeadline(parent, deadline);
    }
    std::pair<ContextPtr, CancelFunc> WithTimeout(const ContextPtr &parent,
                                                  ContextDuration timeout)
    {
        return Context::WithTimeout(parent, timeout);
    }
    ContextRollback WithRollback(const ContextPtr &parent, bool rollback_on_cancel)
    {
        return ContextRollback(parent, rollback_on_cancel);
    }
    ContextPtr WithValue(const ContextPtr &parent, std::string key, std::any value)
    {
        return Context::WithValue(parent, std::move(key), std::move(value));
    }

    ErrorPtr CanceledError()
    {
        static const ErrorPtr error = NewError("context canceled");
        return error;
    }

    ErrorPtr DeadlineExceededError()
    {
        static const ErrorPtr error = NewError("context deadline exceeded");
        return error;
    }

} // namespace go2cpp