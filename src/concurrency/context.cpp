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
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp
{

    namespace
    {

        /**
         * @brief 带内联容量的小向量：常见规模不碰堆。
         *
         * ## 为什么需要
         *
         * Context 创建路径的分配次数是硬指标（≤1.1× Go）。取消树里三个容器
         * （children / 取消工作栈 / 待通知列表）在绝大多数场景只有 0~2 个元素，
         * 用 std::vector 会让每次 WithCancel 都付一次 malloc；这里把前
         * `kInline` 个元素直接放在对象内，超出才退化为堆。
         *
         * ## 约束
         *
         * - 只支持本文件用到的接口（push_back / emplace_back / pop_back /
         *   erase_if / clear / reserve / 迭代器），不支持拷贝与移动语义；
         * - 元素必须可移动构造；析构按逆序执行；
         * - 迭代器是裸指针，仅在未触发 grow 期间有效（调用方都按此使用）。
         */
        template <typename T, std::size_t kInline>
        class SmallVector final
        {
        public:
            SmallVector() noexcept = default;
            SmallVector(const SmallVector &) = delete;
            SmallVector &operator=(const SmallVector &) = delete;
            SmallVector(SmallVector &&) = delete;
            SmallVector &operator=(SmallVector &&) = delete;

            ~SmallVector() noexcept
            {
                clear();
                if (m_heap != nullptr)
                {
                    ::operator delete(m_heap);
                }
            }

            std::size_t size() const noexcept { return m_size; }
            bool empty() const noexcept { return m_size == 0; }

            T &back() noexcept { return *std::launder(data() + m_size - 1); }
            const T &back() const noexcept
            {
                return *std::launder(data() + m_size - 1);
            }
            T &front() noexcept { return *std::launder(data()); }
            const T &front() const noexcept { return *std::launder(data()); }

            T *begin() noexcept { return data(); }
            T *end() noexcept { return data() + m_size; }
            const T *begin() const noexcept { return data(); }
            const T *end() const noexcept { return data() + m_size; }

            void reserve(std::size_t capacity)
            {
                if (capacity <= m_capacity)
                {
                    return;
                }
                std::size_t next = m_capacity;
                while (next < capacity)
                {
                    next *= 2;
                }
                grow(next);
            }

            template <typename... Args>
            T &emplace_back(Args &&...args)
            {
                if (m_size == m_capacity)
                {
                    reserve(m_capacity + 1);
                }
                T *slot = data() + m_size;
                new (slot) T(std::forward<Args>(args)...);
                ++m_size;
                return *slot;
            }

            void push_back(const T &value) { emplace_back(value); }
            void push_back(T &&value) { emplace_back(std::move(value)); }

            void pop_back() noexcept
            {
                --m_size;
                std::launder(data() + m_size)->~T();
            }

            void clear() noexcept
            {
                while (m_size != 0)
                {
                    --m_size;
                    std::launder(data() + m_size)->~T();
                }
            }

            /// @brief 就地删除命中谓词的元素，其余元素保持相对顺序。
            template <typename Pred>
            void erase_if(Pred pred)
            {
                std::size_t out = 0;
                for (std::size_t i = 0; i < m_size; ++i)
                {
                    T *slot = std::launder(data() + i);
                    if (pred(*slot))
                    {
                        slot->~T();
                        continue;
                    }
                    if (out != i)
                    {
                        T *target = std::launder(data() + out);
                        new (target) T(std::move(*slot));
                        slot->~T();
                    }
                    ++out;
                }
                m_size = out;
            }

        private:
            T *data() noexcept
            {
                return m_heap != nullptr
                           ? std::launder(m_heap)
                           : std::launder(reinterpret_cast<T *>(m_inline));
            }
            const T *data() const noexcept
            {
                return m_heap != nullptr
                           ? std::launder(m_heap)
                           : std::launder(reinterpret_cast<const T *>(m_inline));
            }

            void grow(std::size_t capacity)
            {
                auto *next = static_cast<T *>(::operator new(capacity * sizeof(T)));
                for (std::size_t i = 0; i < m_size; ++i)
                {
                    T *slot = std::launder(data() + i);
                    new (next + i) T(std::move(*slot));
                    slot->~T();
                }
                if (m_heap != nullptr)
                {
                    ::operator delete(m_heap);
                }
                m_heap = next;
                m_capacity = capacity;
            }

            alignas(T) unsigned char m_inline[sizeof(T) * kInline];
            T *m_heap{nullptr};
            std::size_t m_size{0};
            std::size_t m_capacity{kInline};
        };

    } // namespace

    namespace
    {

        /**
         * @brief Context 共享块的线程本地分配池。
         *
         * ## 为什么需要
         *
         * 单块分配方案已经把每个 Context 的堆分配压到 1 次，但 glibc 的
         * malloc/free 一次往返仍要约 21+10ns，占创建+销毁预算的 1/3。Context
         * 生命周期短、大小固定，正是对象池最合适的形状。
         *
         * ## 做法
         *
         * 复用块的前 8 字节当侵入式 next 指针，空闲块挂在线程本地链表上。
         * 分配是「摘链 + 复用」，释放是「压链」，稳态零系统调用、零分支预测
         * 失败。跨线程释放时块进入释放者线程的池子，因此不存在竞争。
         *
         * ## 边界
         *
         * - AddressSanitizer 构建下关闭池化，让 ASan 继续看到真实的
         *   malloc/free（否则池化会掩盖 use-after-free 的检测）；
         * - 空闲链上限 1024 块，超过的直接还给系统，避免峰值后常驻内存；
         * - 只服务单块分配（`allocate(1)`），其它尺寸走普通 new。
         */
        template <typename T>
        class BlockPoolAllocator final
        {
        public:
            using value_type = T;

            template <typename U>
            struct rebind
            {
                using other = BlockPoolAllocator<U>;
            };

            BlockPoolAllocator() noexcept = default;
            template <typename U>
            BlockPoolAllocator(const BlockPoolAllocator<U> &) noexcept
            {
            }

            T *allocate(std::size_t count)
            {
                if (count != 1)
                {
                    return static_cast<T *>(::operator new(count * sizeof(T)));
                }
#if defined(__SANITIZE_ADDRESS__)
                return static_cast<T *>(::operator new(sizeof(T)));
#else
                void *&head = FreeListHead();
                if (head == nullptr)
                {
                    return static_cast<T *>(::operator new(sizeof(T)));
                }
                void *block = head;
                head = *static_cast<void **>(block);
                --FreeListCount();
                return static_cast<T *>(block);
#endif
            }

            void deallocate(T *block, std::size_t count) noexcept
            {
                if (count != 1)
                {
                    ::operator delete(block);
                    return;
                }
#if defined(__SANITIZE_ADDRESS__)
                ::operator delete(block);
#else
                if (FreeListCount() >= kMaxCachedBlocks)
                {
                    ::operator delete(block);
                    return;
                }
                void *&head = FreeListHead();
                *reinterpret_cast<void **>(block) = head;
                head = block;
                ++FreeListCount();
#endif
            }

        private:
            static constexpr std::size_t kMaxCachedBlocks = 1024;

            static void *&FreeListHead() noexcept
            {
                static thread_local void *head = nullptr;
                return head;
            }

            static std::size_t &FreeListCount() noexcept
            {
                static thread_local std::size_t count = 0;
                return count;
            }
        };

        /**
         * @brief `Context::State` 专用的轻量互斥。
         *
         * ## 为什么不用 std::mutex
         *
         * 这把锁只保护几组指针/计数，临界区是「读写几个字段 + 搬运小容器」，
         * 从不在持锁期间执行用户回调、不进入内核等待原语。glibc 互斥量在无
         * 竞争时仍有 ~4.7ns/对的开销，而创建/取消/析构路径各要拿 1~2 次；
         * 换成一个原子字 + pause 自旋后单次约 2ns，单线程基准直接受益。
         *
         * ## 安全性
         *
         * - 临界区极短且无阻塞调用，不会被持锁线程挂起很久；
         * - 与 `DoneSignal` 的 `std::mutex` 互不嵌套（取消路径先释放状态锁
         *   再 `Signal`），因此不会与条件等待路径形成锁序问题；
         * - 竞争方（timer 线程 / 取消链）最多自旋几十个周期，可接受。
         */
        class StateLock final
        {
        public:
            void lock() noexcept
            {
                for (;;)
                {
                    if (!m_flag.exchange(true, std::memory_order_acquire))
                    {
                        return;
                    }
                    while (m_flag.load(std::memory_order_relaxed))
                    {
#if defined(__x86_64__) || defined(__i386__)
                        __builtin_ia32_pause();
#endif
                    }
                }
            }

            void unlock() noexcept
            {
                m_flag.store(false, std::memory_order_release);
            }

        private:
            std::atomic<bool> m_flag{false};
        };

    } // namespace

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
        /// @brief 一条待执行回调：Id 用于注销，function 在信号锁外执行。
        struct CallbackEntry
        {
            CallbackId id{0};
            std::function<void()> callback;
        };

        /// @brief 保护 done 的写入与两张回调表；Signal 与 AddCallback/RemoveCallback 竞争它。
        mutable std::mutex mutex;
        /// @brief 等待载体：受管 G 在此 park、普通线程走原生条件变量（ParkingCondition 分流）。
        mutable core::ParkingCondition cv;
        /// @brief 完成标志；置真后不可回退（单向）。
        /// @details 这是**唯一**的完成状态。写入始终在 mutex 内（Signal），读取走
        ///          无锁 acquire（IsDone/Wait 谓词），因此取消的热路径检查不再取锁。
        std::atomic<bool> done{false};
        /// @brief 当前登记的 Wait* 等待者数量（受 mutex 保护）。
        /// @details Signal 在无人等待时跳过 notify_all，省掉一次条件变量锁与 futex
        ///          唤醒；计数只影响性能，不参与正确性判断（谓词仍会重检 done）。
        std::size_t waiter_count{0};

        /// @brief 在 mutex 持有期内登记/注销一个等待者，保证 Signal 看不到半登记状态。
        struct WaiterScope
        {
            explicit WaiterScope(State &owner) noexcept : state(owner)
            {
                ++state.waiter_count;
            }
            ~WaiterScope() { --state.waiter_count; }
            WaiterScope(const WaiterScope &) = delete;
            WaiterScope &operator=(const WaiterScope &) = delete;
            State &state;
        };
        /// @brief 下一个回调 Id（从 1 起，0 表示无效）。
        CallbackId next_id{1};
        // 内部回调（例如 ContextRollback）必须先于用户观察回调执行。
        // 两组回调仍然都在 Signal() 的锁外调用，避免回调重入信号对象。
        // 用连续 vector 而不是 unordered_map：注册/注销是 O(1) 尾插 + 线性扫描，
        // 取消时整体 swap 搬出，避免每个回调一次哈希节点分配。
        /// @brief 内部回调表（先执行）；由 AddCallbackImpl 写入、Signal 一次性搬出。
        std::vector<CallbackEntry> internal_callbacks;
        /// @brief 用户回调表（后执行）；由 AddCallback/RemoveCallback 维护、Signal 搬出。
        std::vector<CallbackEntry> callbacks;
        /// @brief 指向拥有本状态的 Context 块（仅借用模式用于副本保活）。
        /// @details 拥有模式（独立构造的 DoneSignal）恒为空；Context 建块后
        ///          写入自己的 shared_ptr，借用句柄被拷贝时用它把副本变成
        ///          「持有」，从而支持「先复制 Done()，后销毁 Context」。
        std::weak_ptr<void> owner;
    };

    DoneSignal::DoneSignal()
        : m_state(std::make_shared<State>()), m_done_flag(&m_state->done)
    {
    }

    /// @brief 借用构造：状态由外部所有者持有，本句柄不分配、不参与所有权。
    /// @details 只有 `Context`（及其内嵌状态）使用这条路径。因为
    ///          `Context::State` 与 `Context` 在同一次分配里，裸指针在
    ///          `ContextPtr` / CancelFunc 存活期间始终有效，所以无需引用计数。
    DoneSignal::DoneSignal(BorrowTag, State *state) noexcept
        : m_state(), m_borrowed(state), m_done_flag(&state->done)
    {
    }

    /// @brief 返回拥有/借用模式下的真实状态。
    DoneSignal::State &DoneSignal::state() const noexcept
    {
        return m_borrowed != nullptr ? *m_borrowed : *m_state;
    }

    /// @brief 拷贝构造：借用句柄的副本提升为持有，自行保证状态存活。
    DoneSignal::DoneSignal(const DoneSignal &other) noexcept
        : m_state(other.m_state), m_borrowed(other.m_borrowed),
          m_owner(other.m_owner), m_done_flag(other.m_done_flag)
    {
        if (m_owner == nullptr && m_borrowed != nullptr)
        {
            m_owner = m_borrowed->owner.lock();
        }
    }

    /// @brief 拷贝赋值：语义同拷贝构造（借用副本提升为持有）。
    DoneSignal &DoneSignal::operator=(const DoneSignal &other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        m_state = other.m_state;
        m_borrowed = other.m_borrowed;
        m_done_flag = other.m_done_flag;
        m_owner = other.m_owner;
        if (m_owner == nullptr && m_borrowed != nullptr)
        {
            m_owner = m_borrowed->owner.lock();
        }
        return *this;
    }

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
        // 快路径：已完成则不再加锁；已取消 Context 上的重复等待很常见。
        if (IsDone())
        {
            return;
        }
        State &st = state();
        std::unique_lock<std::mutex> lock(st.mutex);
        if (st.done.load(std::memory_order_relaxed))
        {
            return;
        }
        State::WaiterScope waiter(st);
        st.cv.wait(lock, [&st]
                   { return st.done.load(std::memory_order_acquire); });
    }

    /// @brief 等待至多 timeout，返回信号是否已完成。
    /// @details 与 Wait 相同的三步（复制 state → 用共享状态的锁 → 谓词等待），
    ///          区别只在第 3 步使用带相对时限的等待形式。
    /// @param[in] timeout 相对时长；到期即返回，返回值随即为 false。
    /// @return 返回时信号已完成则为 true；超时返回 false。
    /// @note 超时只结束"本次等待"，不改变信号状态——信号一旦完成就永久保持完成。
    bool DoneSignal::WaitFor(ContextDuration timeout) const
    {
        if (IsDone())
        {
            return true;
        }
        State &st = state();
        std::unique_lock<std::mutex> lock(st.mutex);
        if (st.done.load(std::memory_order_relaxed))
        {
            return true;
        }
        State::WaiterScope waiter(st);
        return st.cv.wait_for(lock, timeout, [&st]
                              { return st.done.load(std::memory_order_acquire); });
    }

    /// @brief 等待到绝对截止时间，返回信号是否已完成。
    /// @details 步骤同 WaitFor，只是把相对时长换成本次等待的绝对时限。
    /// @param[in] deadline 基于 steady_clock 的绝对时间点。
    /// @return 返回时信号已完成则为 true；到期返回 false。
    /// @note 绝对时限便于在循环中重试而不会因每轮重新计时而无限延后。
    bool DoneSignal::WaitUntil(ContextTimePoint deadline) const
    {
        if (IsDone())
        {
            return true;
        }
        State &st = state();
        std::unique_lock<std::mutex> lock(st.mutex);
        if (st.done.load(std::memory_order_relaxed))
        {
            return true;
        }
        State::WaiterScope waiter(st);
        return st.cv.wait_until(lock, deadline, [&st]
                                { return st.done.load(std::memory_order_acquire); });
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
        State &st = state();
        CallbackId id = 0;
        bool invoke_now = false;
        {
            std::lock_guard<std::mutex> lock(st.mutex);
            if (st.done.load(std::memory_order_relaxed))
            {
                invoke_now = true;
            }
            else
            {
                id = st.next_id++;
                if (id == 0)
                {
                    id = st.next_id++;
                }
                auto &callbacks = internal ? st.internal_callbacks
                                           : st.callbacks;
                callbacks.push_back(State::CallbackEntry{id, std::move(callback)});
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
        State &st = state();
        std::lock_guard<std::mutex> lock(st.mutex);
        // 两组都是小 vector；用「与尾元素交换后 pop」避免整段搬移，同时把
        // 注销成本限制在 O(已登记回调数)，不再有哈希节点释放。
        const auto remove_from = [id](std::vector<State::CallbackEntry> &entries)
        {
            for (auto it = entries.begin(); it != entries.end(); ++it)
            {
                if (it->id == id)
                {
                    *it = std::move(entries.back());
                    entries.pop_back();
                    return;
                }
            }
        };
        remove_from(st.callbacks);
        remove_from(st.internal_callbacks);
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
        // 快路径：已完成时直接返回，完全不加锁。取消是幂等的，
        // 重复 Signal 在 Context 取消传播里很常见（父与 timer 同时触发）。
        if (m_done_flag->load(std::memory_order_acquire))
        {
            return;
        }
        State &st = state();
        std::vector<State::CallbackEntry> internal_callbacks;
        std::vector<State::CallbackEntry> callbacks;
        bool has_waiters = false;
        {
            std::lock_guard<std::mutex> lock(st.mutex);
            if (st.done.load(std::memory_order_relaxed))
            {
                return;  // 幂等：已完成则直接返回，不重复通知
            }
            // release：先让 done=true 之前写入的取消状态（error/cause）对
            // acquire 读 IsDone 的线程可见。
            st.done.store(true, std::memory_order_release);
            internal_callbacks.swap(st.internal_callbacks);
            callbacks.swap(st.callbacks);
            has_waiters = st.waiter_count != 0;
        }
        // step2: 唤醒所有 Wait 者。放在执行回调之前，让等待方尽早恢复。
        //        没有登记等待者时跳过：notify_all 本身要取等待队列锁。
        if (has_waiters)
        {
            st.cv.notify_all();
        }
        // step3: 内部回调先执行（ContextRollback 依赖这个顺序）。
        for (auto &entry : internal_callbacks)
        {
            try
            {
                entry.callback();
            }
            catch (...)
            {
            }
        }
        // step4: 用户回调后执行；单个回调抛异常被吞掉，不阻断其余回调。
        for (auto &entry : callbacks)
        {
            try
            {
                entry.callback();
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

        /// @brief 取消路径用的标准错误（静态单例引用，避免每次构造一份拷贝）。
        const ErrorPtr &CanceledErrorRef()
        {
            static const ErrorPtr error = CanceledError();
            return error;
        }

        const ErrorPtr &DeadlineExceededErrorRef()
        {
            static const ErrorPtr error = DeadlineExceededError();
            return error;
        }

        std::mutex s_clock_mutex;
        Context::NowFunction s_now_function;
        // 是否安装了测试时钟。发布版本里没有任何注入时，Now() 走一次
        // acquire 原子读后直接 steady_clock::now()，不再每取一次时间都
        // 抢 s_clock_mutex 并拷贝 std::function。
        std::atomic<bool> s_has_custom_clock{false};

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
            /// @brief worker 当前正在等待的绝对时刻（休眠中为空）。
            /// @details 旧实现用「表是否为空 / 新 deadline 是否早于表头」判断
            ///          要不要唤醒 worker。表空时它看不到 worker 其实还睡在
            ///          上一轮的 deadline 上，于是每个 WithTimeout 都白付一次
            ///          futex 唤醒（基准里约 400ns）。改记 worker 的真实睡眠
            ///          目标后，只要新 deadline 不早于它就不用醒 —— 取消/到期
            ///          的语义不变（worker 醒来总重新读表）。
            std::optional<ContextTimePoint> m_worker_deadline;
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
    struct Context::State
    {
        /// @brief 保护本结构除 `done_core` 外的全部字段；Cancel/WithValue/Err/Children 读写。
        mutable StateLock mutex;
        /// @brief 指向自身的弱引用，替代 enable_shared_from_this。
        /// @details Context 与 State 同处一块共享分配，State 不是由
        ///          shared_ptr<State> 直接拥有的对象，因此不能继承
        ///          enable_shared_from_this；工厂在建块后把别名句柄写进来，
        ///          Self() 用它取得强引用（与控制块共享，不额外分配）。
        std::weak_ptr<State> self;
        /// @brief 父节点（根节点为空）；用于取消向上传播与 Values 向上查找。
        std::shared_ptr<State> parent;
        /// @brief 子节点弱引用集合；父取消时遍历逐个 Cancel。弱引用避免父子环。
        /// @details 内联 2 个名额：典型调用链每个父节点只有 1 个活跃 child，
        ///          这样第一次 AddChild 不再付一次堆分配。
        SmallVector<std::weak_ptr<State>, 2> children;
        /// @brief `WithValue` 的类型化键值表；键是类型标识对象的地址。
        std::unordered_map<const void *, std::any> typed_values;
        // 只要值仍然可达，就保留用于标识类型化 key 的分配对象。若没有这个所有者
        // 而仅按裸指针查找，分配器复用地址后可能错误地别名到无关的后续 key。
        /// @brief typed_values 的键所有者：持有 key 对象本身，防止地址被复用后键混淆。
        std::unordered_map<const void *, std::shared_ptr<const std::uint64_t>>
            typed_key_anchors;
        /// @brief `WithNamedValue` 的字符串键值表（诊断/配置这类按名字取值的场景）。
        std::unordered_map<std::string, std::any> named_values;
        /// @brief 内嵌的完成状态：随 Context 同块分配，不再单独 make_shared。
        /// @details 这是单次分配方案的关键一步——原来的 DoneSignal 成员内部
        ///          还持有一次 make_shared，等于每个 Context 多一次堆分配。
        DoneSignal::State done_core;
        /// @brief 借用 done_core 的句柄视图（不拥有、不分配）。
        DoneSignal done_view{Context::MakeBorrowedDone(&done_core)};
        /// @brief 取消原因（非空即已取消，幂等写入一次）；`Err()` 返回它。
        ErrorPtr error;
        /// @brief `WithCancelCause` 记录的首个原因；普通取消为空，`Cause()` 返回它。
        ErrorPtr cause;
        /// @brief 绝对截止时间（`WithDeadline/WithTimeout` 设置）；到期由定时器线程调 Cancel。
        std::optional<ContextTimePoint> deadline;
        /// @brief 定时器 Id（0 表示未注册）；Cancel 成功后用它 `TimerService::Remove`。
        TimerService::Id timer_id{0};
        /// @brief 是否已登记在父节点的 children 里。
        /// @details Background/TODO 根节点永不取消，孩子不需要登记；有了这个
        ///          标记，取消/析构就不会为"从未登记"的父子关系白拿一次父锁。
        /// - 保护：State::mutex（写）与父的 State::mutex（AddChild 内写）。
        bool in_parent{false};
        /// @brief 是否是根节点（Background/TODO）；根节点不会因父级联被取消。
        bool root{false};
        /// @brief 下一个 before-done 回调 Id（从 1 起，0 是无效 Id）。
        DoneSignal::CallbackId next_before_done_id{1};
        /// @brief 在 `done_view.Signal()` 之前执行的回调表；用于取消前必须先改内部状态的钩子。
        /// @details 用内联 4 个名额的小向量代替 unordered_map：回滚钩子通常
        ///          只有 0~1 个，哈希节点分配会直接抬高创建/取消路径的分配数。
        SmallVector<std::pair<DoneSignal::CallbackId, std::function<void()>>, 4>
            before_done_callbacks;

        /// @brief 返回与自身共享控制块的强引用（等价于旧 shared_from_this）。
        /// @details 只在 State 存活期间调用；调用方持有的强引用保证 lock() 成功。
        std::shared_ptr<State> Self() const { return self.lock(); }

        ~State();

        /**
         * @brief 一个等待被执行的取消动作（`Cancel` 级联时的「待办项」）。
         *
         * ## 为什么把取消做成工作项而不是边遍历边递归
         *
         * 取消会沿 `children` 级联：如果边遍历边递归，调用栈深度等于取消树深度
         * （深链会爆栈），而且持锁遍历容易与子节点自己的取消竞争。做法是把待取消
         * 的节点收集成队列，**在锁外**逐个处理。
         *
         * ## 字段
         */
        struct CancellationWork
        {
            /// @brief 待取消的状态（裸指针，避免每次取消都多一组原子计数）。
            /// @details 生命周期由下面的 keepalive（子节点）或调用者持有的
            ///          ContextPtr（根节点）保证，处理时一定仍然有效。
            State *raw{nullptr};
            /// @brief 强引用保活：子节点由 weak 提升而来，根节点来自 self。
            std::shared_ptr<State> keepalive;
            /// @brief 要写入 `state->error` 的错误（幂等：已有 error 则跳过）。
            ErrorPtr error;
            /// @brief 要写入 `state->cause` 的原因（仅 `WithCancelCause` 使用）。
            ErrorPtr cause;
            /// @brief 本次取消是否由 deadline 触发（决定错误类型为 DeadlineExceeded）。
            bool deadline_error{false};
            /// @brief 处理完是否从父节点 `children` 中移除（减少后续遍历开销）。
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
                std::lock_guard<StateLock> lock(mutex);
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
                    before_done_callbacks.emplace_back(id, std::move(callback));
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
            std::lock_guard<StateLock> lock(mutex);
            before_done_callbacks.erase_if(
                [id](const auto &entry)
                { return entry.first == id; });
        }

        bool IsCanceled() const noexcept
        {
            std::lock_guard<StateLock> lock(mutex);
            return error != nullptr;
        }

        void RemoveChildRaw(const State *child)
        {
            std::lock_guard<StateLock> lock(mutex);
            children.erase_if(
                [child](const std::weak_ptr<State> &weak_child)
                {
                    const auto current = weak_child.lock();
                    return !current || current.get() == child;
                });
        }

        void RemoveChild(const std::shared_ptr<State> &child)
        {
            if (child)
            {
                RemoveChildRaw(child.get());
            }
        }

        /**
         * @brief 把取消传播到整棵子树，并触发所有 `Done` 等待者。
         * @param work 初始待处理项（`Cancel` 传根节点，`CancelFromParent` 传子节点
         *        并附带继承的 error/cause）。
         *
         * ## 定位
         *
         * `Context::Cancel` 与 `CancelFromParent` 的唯一实现，也是**取消语义的
         * 心脏**：从触发点向下遍历所有后代，标记 `error`，然后统一唤醒。
         *
         * ## 步骤
         *
         * step1. 用**显式栈**（`work`）而不是递归遍历：深层 Context 树递归会爆栈，
         *        这里每轮取一个节点、把它的后代压回栈，直到栈空；
         * step2. 每轮在 `state->mutex` 内完成「标记 + 摘除」：写入 `error`/`cause`，
         *        取出 `timer_id`、清空 `before_done_callbacks`、收集 `children`，
         *        然后 `children.clear()`——**已经取消过的子树（`error` 非空）直接跳过**，
         *        保证每个节点只处理一次；
         * step3. 锁外做三件事：移除该节点的超时 timer、从父节点解绑、
         *        把它的 `before_done_callbacks` 收集到全局列表；
         * step4. 把 `descendants` 压回 `work`，继续下一轮（迭代传播）；
         * step5. 全部节点标记完成后，**按逆序**执行 `before_done_callbacks`，
         *        再**按逆序** `done.Signal()` 唤醒所有等待者。
         *
         * ## 并发/失败语义
         *
         * - **先标记、后唤醒（两阶段）**：调用用户回调前必须先标记完整子树。
         *          因为 `Done` 回调可能观察或等待后代，若先通知父回调，
         *          取消传播期间同步的 parent→child 等待会死锁；
         * - **锁外执行回调与 Signal**：`Signal()` 会唤醒 Fiber（可能跨 M），
         *          持 `state->mutex` 调用会锁序反转；
         * - **逆序**：`before_done_callbacks` 与 `done.Signal()` 都按逆序执行，
         *          与注册顺序相反——这是 rollback scope 的语义要求（后注册的
         *          undo 先回滚）；
         * - **回调异常被吞掉**（`catch (...)`）：取消是「尽力而为」的清理路径，
         *          一个观察者抛异常不能阻止其它观察者被唤醒；
         * - **timer 必须在锁外移除**：`TimerService::Remove` 会拿定时器锁，
         *          而锁顺序约定是「状态锁 → 定时器锁」不存在环，所以先释放状态锁；
         * - **幂等**：`error` 非空或 `root` 为真的节点直接跳过，重复取消不产生副作用。
         */
        static void DrainCancellation(SmallVector<CancellationWork, 8> &work)
        {
            // step1/step2 的全局约定：先标记完整子树，再执行回调与 Signal。
            // 调用用户回调前先标记完整子树。Done 回调可以观察或等待后代；如果
            // 先通知父回调，取消传播期间同步的 parent->child 等待可能死锁。
            // 两个收集容器同样用内联容量：单节点取消（最常见）零堆分配。
            SmallVector<std::shared_ptr<State>, 8> to_signal;
            SmallVector<std::function<void()>, 4> before_done_callbacks;
            // step1/step2: 用显式栈迭代（不递归，避免深层 Context 树爆栈）；
            // 每轮在 state->mutex 内标记并摘除该节点。
            while (!work.empty())
            {
                CancellationWork item = std::move(work.back());
                work.pop_back();
                State *state = item.raw;
                if (state == nullptr)
                {
                    continue;
                }

                // step2: 本轮的局部收集区——标记、摘 timer、摘回调、收集后代。
                // 注意：不再为每个节点单独分配 descendants vector，子节点直接
                // 压回 work 复用容量。深度取消时这是每节点一次 malloc 的节省。
                ErrorPtr local_error = item.error;
                ErrorPtr local_cause = item.cause;
                TimerService::Id old_timer = 0;
                State *parent_to_remove = nullptr;
                {
                    std::lock_guard<StateLock> lock(state->mutex);
                    if (state->root || state->error)
                    {
                        continue;
                    }
                    if (!local_error)
                    {
                        local_error = item.deadline_error
                                          ? DeadlineExceededErrorRef()
                                          : CanceledErrorRef();
                    }
                    if (!local_cause)
                    {
                        local_cause = local_error;
                    }
                    state->error = local_error;
                    state->cause = local_cause;
                    old_timer = state->timer_id;
                    state->timer_id = 0;
                    for (auto &entry : state->before_done_callbacks)
                    {
                        before_done_callbacks.push_back(
                            std::move(entry.second));
                    }
                    state->before_done_callbacks.clear();
                    if (item.remove_from_parent && state->in_parent)
                    {
                        // 裸指针足够：state 由 to_signal 保活，父由
                        // state->parent 保活，本轮结束前都不会失效。
                        parent_to_remove = state->parent.get();
                        state->in_parent = false;
                    }
                    for (const auto &weak_child : state->children)
                    {
                        if (auto child = weak_child.lock())
                        {
                            work.push_back({child.get(), std::move(child),
                                            local_error, local_cause, false});
                        }
                    }
                    state->children.clear();
                }

                if (old_timer != 0)
                {
                    TimerService::Instance().Remove(old_timer);
                }
                if (parent_to_remove != nullptr)
                {
                    parent_to_remove->RemoveChildRaw(state);
                }
                to_signal.push_back(std::move(item.keepalive));
            }
            // step5: 两阶段完成——先逆序执行 before_done 回调，再逆序 Signal。
            // 所有 Context 状态都已先标记取消，再让 rollback scope 冻结其
            // undo。这样 Done 的观察者、RecordUndo 和 Commit 不会在取消窗口
            // 中错误地把新动作当成可提交状态。真正的补偿由 DoneSignal 的
            // 内部回调在 done=true、普通用户回调之前执行。
            for (std::size_t i = before_done_callbacks.size(); i != 0; --i)
            {
                try
                {
                    before_done_callbacks.begin()[i - 1]();
                }
                catch (...)
                {
                }
            }
            for (std::size_t i = to_signal.size(); i != 0; --i)
            {
                to_signal.begin()[i - 1]->done_view.Signal();
            }
        }

        /// @brief 把 child 挂到本节点下，并返回本节点当前的 deadline 供其继承。
        /// @details 一次加锁同时完成「清理失效登记 + 读 deadline + 挂接」，
        ///          省掉创建路径上第二次锁。若本节点已取消，则改为把取消
        ///          状态下发给 child（返回 nullopt），与旧行为一致。
        std::optional<ContextTimePoint> AddChild(State *child)
        {
            ErrorPtr inherited_error;
            ErrorPtr inherited_cause;
            // deadline 与「父是否已取消」无关：Go 的语义是子节点继承父的
            // 有效 deadline，即使父已经因到期/取消而关闭。因此两条返回路径
            // 都带上同一个快照。
            std::optional<ContextTimePoint> inherited_deadline;
            {
                std::lock_guard<StateLock> lock(mutex);
                inherited_deadline = deadline;
                children.erase_if([](const std::weak_ptr<State> &item)
                                  { return item.expired(); });
                if (!error)
                {
                    children.push_back(child->self);
                    child->in_parent = true;
                    return inherited_deadline;
                }
                inherited_error = error;
                inherited_cause = cause;
            }
            child->CancelFromParent(std::move(inherited_error),
                                    std::move(inherited_cause));
            return inherited_deadline;
        }

        void Cancel(ErrorPtr requested_cause, bool deadline_error = false)
        {
            // 快路径：已完成时直接返回，连 self 弱引用都不取（幂等取消在
            // 取消传播里很常见：父级联与 timer 到期可能同时到达）。
            if (done_view.IsDone())
            {
                return;
            }
            SmallVector<CancellationWork, 8> work;
            work.push_back({this, Self(), {}, std::move(requested_cause),
                            deadline_error, true});
            DrainCancellation(work);
        }

        void CancelFromParent(ErrorPtr inherited_error, ErrorPtr inherited_cause)
        {
            if (done_view.IsDone())
            {
                return;
            }
            SmallVector<CancellationWork, 8> work;
            work.push_back({this, Self(), std::move(inherited_error),
                            std::move(inherited_cause), false, false});
            DrainCancellation(work);
        }

        std::any Lookup(const void *token, const std::string &name) const
        {
            auto owner = const_cast<State *>(this)->Self();
            while (owner)
            {
                std::shared_ptr<State> parent_owner;
                {
                    std::lock_guard<StateLock> lock(owner->mutex);
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
        // 再由排空循环迭代释放，避免深层 Context 树耗尽原生栈。
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

        /**
         * @brief 迭代排空「延迟释放队列」。
         *
         * ## 为什么需要它
         *
         * `State::~State()` 会取消所有子节点，而取消回调可能又析构别的 `State` ——
         * 直接递归 `delete` 会让调用栈深度等于取消树深度（深链爆栈），也可能在
         * 某个回调里重入释放逻辑。
         *
         * ## 做法
         *
         * 析构时把父链释放（`next.reset()`）推迟到一个 **thread_local 队列**，
         * 由本函数循环排空；`t_draining_state_releases` 防重入 —— 排空过程中
         * 又触发的释放只入队，不再嵌套排空。单块分配方案下删除器交给
         * `make_shared` 的控制块，所以排空必须由 `~State` 自己发起。
         */
        void DrainDeferredStateReleases() noexcept
        {
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

    } // namespace

    Context::State::~State()
    {
        TimerService::Id old_timer = 0;
        bool canceled = false;
        bool was_in_parent = false;
        std::shared_ptr<State> retained_parent;
        {
            std::lock_guard<StateLock> lock(mutex);
            old_timer = timer_id;
            timer_id = 0;
            canceled = error != nullptr;
            was_in_parent = in_parent;
            in_parent = false;
            children.clear();
            retained_parent = std::move(parent);
        }
        if (old_timer != 0)
        {
            TimerService::Instance().Remove(old_timer);
        }
        if (retained_parent)
        {
            // 已取消节点在取消路径上已经摘除父登记（Cancel 显式摘除、
            // CancelFromParent 由父清空 children），这里再锁一次父节点纯属
            // 重复。只有未取消的短命 child 需要主动摘除，避免长期只创建、
            // 不再创建新 child 时登记表单调增长。
            if (!canceled && was_in_parent)
            {
                retained_parent->RemoveChildRaw(this);
            }
            // 只有「自己是父的最后一个持有者」时，这次释放才可能在本线程触发
            // 父块析构（深链递归风险）；其余情况直接 release 即可，省掉每轮
            // 创建都要访问一次动态 TLS 释放队列的开销。
            if (retained_parent.use_count() > 1)
            {
                retained_parent.reset();
            }
            else
            {
                QueueStateParent(std::move(retained_parent));
                // 单块分配方案下没有自定义删除器，父链的迭代释放必须在这里
                // 主动排空。
                DrainDeferredStateReleases();
            }
        }
    }

    namespace detail
    {

        /**
         * @brief `Context` 与它的取消状态所在的共享块（单次分配）。
         *
         * ## 为什么合并
         *
         * 旧实现每个 Context 要付 5~6 次分配：`State` + 控制块、`DoneSignal::State`、
         * `Context` + 控制块、父节点 children 数组。Go 的 cancelCtx 只有一次分配，
         * 这是 3.5× 差距的主因。
         *
         * ## 布局
         *
         * - `context_storage`：`Context` 的 placement new 存储（`Context` 定义在
         *   头文件，不能直接做成员，否则 .cpp 里的 `State` 无法内嵌）；
         * - `state`：取消状态，含内嵌的完成信号状态与内联小容器。
         *
         * 两者共用 `std::make_shared<ContextBlock>()` 的一个控制块：`ContextPtr`、
         * CancelFunc、父链与 timer 登记都用别名 shared_ptr 指向同一控制块。
         *
         * ## 析构
         *
         * 先显式调用 `~Context()`，再由成员析构销毁 `state`；`~State` 负责把
         * 父链释放入队并排空，避免深链递归爆栈。
         */
        struct ContextBlock
        {
            alignas(Context) unsigned char context_storage[sizeof(Context)];
            Context::State state;
            bool context_built{false};

            ContextBlock() = default;
            ContextBlock(const ContextBlock &) = delete;
            ContextBlock &operator=(const ContextBlock &) = delete;

            ~ContextBlock()
            {
                if (context_built)
                {
                    context()->~Context();
                }
            }

            Context *context() noexcept
            {
                return std::launder(reinterpret_cast<Context *>(context_storage));
            }
        };

        struct ContextFactory
        {
            /// @brief 一次分配同时承载 Context 与它的状态。
            static ContextPtr Create()
            {
                auto block = std::allocate_shared<ContextBlock>(
                    BlockPoolAllocator<ContextBlock>{});
                std::shared_ptr<Context::State> state_owner(block, &block->state);
                block->state.self = state_owner;
                // 借用句柄的副本靠它保活；Context 自己那份 m_done 保持空，
                // 因此不构成自引用环。
                block->state.done_core.owner = block;
                new (block->context_storage) Context(&block->state);
                block->context_built = true;
                return ContextPtr(block, block->context());
            }

            /// @brief 创建永不取消的根节点。
            static ContextPtr CreateRoot()
            {
                auto value = Create();
                value->m_state->root = true;
                return value;
            }
        };

    } // namespace detail

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
    ContextRollback::ContextRollback(const ContextPtr &parent,
                                     bool rollback_on_cancel)
        : m_state(std::make_shared<State>())
    {
        m_state->rollback_on_cancel = rollback_on_cancel;
        auto child_pair = Context::WithCancel(parent);
        m_context = std::move(child_pair.first);
        m_state->context_state = m_context->StateHandle();

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
            bool wake_worker = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping)
                {
                    return 0;
                }
                // 只有「新 deadline 早于 worker 当前睡眠目标」才需要唤醒：
                // 否则 worker 醒来后会重新读表，结果与现在插进去完全一致。
                // 表空并不代表 worker 没在睡 —— 它可能正睡在上一轮（已取消）
                // 的 deadline 上，这正是不再每次都 notify 的关键。
                wake_worker = !m_worker_deadline.has_value() ||
                              deadline < *m_worker_deadline;
                m_entries.emplace(deadline, Entry{deadline, actual_id, state});
            }
            if (wake_worker)
            {
                m_cv.notify_one();
            }
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
            // 不 notify：被移除的 timer 即使曾是 worker 的唤醒点，worker 也只会
            // 在旧 deadline 上多醒一次并重新计算最早项，不会漏掉任何仍登记的
            // timer。取消短命 WithTimeout 因此省掉一次 futex 唤醒。
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
                        // 无限等待：任何新登记都必须唤醒。
                        m_worker_deadline.reset();
                        m_cv.wait(lock, [this]
                                  { return m_stopping || !m_entries.empty(); });
                        continue;
                    }
                    const auto deadline = m_entries.begin()->first;
                    if (std::chrono::steady_clock::now() < deadline)
                    {
                        // 记录真实睡眠目标，Add 据此判断要不要 notify。
                        m_worker_deadline = deadline;
                        m_cv.wait_until(lock, deadline);
                        m_worker_deadline.reset();
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

    DoneSignal Context::MakeBorrowedDone(DoneSignal::State *state)
    {
        return DoneSignal(DoneSignal::BorrowTag{}, state);
    }

    Context::Context(State *state)
        : m_state(state), m_done(MakeBorrowedDone(&state->done_core)),
          m_deadline(state->deadline) {}

    std::shared_ptr<Context::State> Context::StateHandle() const
    {
        return m_state->self.lock();
    }

    DoneSignal::CallbackId Context::AddBeforeDoneCallback(
        std::function<void()> callback)
    {
        return m_state->AddBeforeDoneCallback(std::move(callback));
    }

    void Context::RemoveBeforeDoneCallback(DoneSignal::CallbackId id) const noexcept
    {
        m_state->RemoveBeforeDoneCallback(id);
    }

    namespace
    {

        /// @brief 进程级根 Context（Background 单例）；返回引用，免一次拷贝。
        const ContextPtr &BackgroundRef()
        {
            static const ContextPtr root = detail::ContextFactory::CreateRoot();
            return root;
        }

    } // namespace

    ContextPtr Context::Background() { return BackgroundRef(); }

    ContextPtr Context::TODO()
    {
        static const ContextPtr root = detail::ContextFactory::CreateRoot();
        return root;
    }

    ContextPtr Context::MakeChild(const ContextPtr &parent)
    {
        // Go 会拒绝 nil parent；转译后的 C++ 使用显式 root，使操作保持内存安全
        // 且不抛出异常。
        const ContextPtr &actual_parent = parent ? parent : BackgroundRef();
        ContextPtr child = detail::ContextFactory::Create();
        State *child_state = child->m_state;
        // 父链用别名 shared_ptr 保活父块；child_state->self 只是弱引用，
        // 因此这条赋值不会形成环（父只 weak 引用 child）。
        child_state->parent = actual_parent->StateHandle();
        // Background/TODO 是永不取消的根：它们不会级联取消孩子，因此不需要
        // 把孩子登记进 children（省一次父锁 + 一次 weak 拷贝），但 parent
        // 指针必须保留，WithValue 的值查找仍要走父链。
        if (!actual_parent->m_state->root)
        {
            // 一次加锁完成「清理失效子登记 + 读父 deadline + 挂接」。
            const auto inherited =
                actual_parent->m_state->AddChild(child_state);
            if (inherited.has_value())
            {
                child_state->deadline = inherited;
                child->m_deadline = inherited;
            }
        }
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
            std::lock_guard<StateLock> lock(child->m_state->mutex);
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
        auto state = child->StateHandle();
        return {std::move(child), [state = std::move(state)]
                { state->Cancel({}); }};
    }

    std::pair<ContextPtr, CancelCauseFunc> Context::WithCancelCause(
        const ContextPtr &parent)
    {
        auto child = MakeChild(parent);
        auto state = child->StateHandle();
        return {std::move(child), [state = std::move(state)](ErrorPtr cause)
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
                std::lock_guard<StateLock> lock(child->m_state->mutex);
                if (child->m_state->error)
                {
                    // 创建 child 的过程中，父 Context 赢得了取消竞争。
                    requested_deadline = ContextTimePoint::max();
                }
                else
                {
                    child->m_state->deadline = requested_deadline;
                    // 同步不可变缓存（Deadline()/HasDeadline() 的无锁来源）。
                    child->m_deadline = requested_deadline;
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
                        id = TimerService::Instance().Add(
                            child->StateHandle(), requested_deadline);
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
                        std::lock_guard<StateLock> lock(child->m_state->mutex);
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
        auto state = child->StateHandle();
        return {std::move(child), [state = std::move(state)]
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

    ErrorPtr Context::Err() const
    {
        std::lock_guard<StateLock> lock(m_state->mutex);
        return m_state->error;
    }

    ErrorPtr Context::Cause() const
    {
        std::lock_guard<StateLock> lock(m_state->mutex);
        return m_state->cause;
    }

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
        {
            std::lock_guard<std::mutex> lock(s_clock_mutex);
            s_now_function = std::move(now);
        }
        // release 配对 Now() 的 acquire：要么看到旧时钟，要么看到完整的
        // std::function；不会出现「标志为真但函数只写了一半」。
        s_has_custom_clock.store(true, std::memory_order_release);
    }

    void Context::ResetNowFunctionForTesting()
    {
        {
            std::lock_guard<std::mutex> lock(s_clock_mutex);
            s_now_function = {};
        }
        s_has_custom_clock.store(false, std::memory_order_release);
    }

    ContextTimePoint Context::Now()
    {
        if (!s_has_custom_clock.load(std::memory_order_acquire))
        {
            return Clock::now();
        }
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