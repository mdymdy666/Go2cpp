/**
 * @file future.hpp
 * @brief Future / Promise：一次性的异步结果传递原语。
 *
 * ## 模块定位
 *
 * Future 解决的是「一个线程/Fiber 产出结果，另一个（或多个）等待并读取」。
 * 它在运行时的位置介于**同步原语**与**调度器**之间：
 *
 * - 等待本身走 `FutureWaitBackend` —— 默认实现基于 `ParkingCondition`，
 *   所以在受管 Fiber 里等待会 **park 当前协程**而不是阻塞 carrier 线程；
 *   普通线程上则退化为原生条件等待；
 * - 它**不拥有**调度器，也不要求调用方在 Fiber 里，因此可以在初始化阶段、
 *   回调里、甚至别的调度器实现中使用（通过替换后端）。
 *
 * ## 与 Channel 的分工
 *
 * - `Channel`：可多次收发、有缓冲、面向"流"；
 * - `Future`：**只完成一次**、面向"单个结果 + 错误"。
 *   需要一次性结果时不要用容量 1 的 channel 代替 —— 会丢掉
 *   broken-promise（生产者提前消失）这个语义。
 *
 * ## 能力清单
 *
 * - `Promise::SetValue/SetError/SetException/Cancel` 四种完成方式；
 * - `Promise` 析构时若仍未完成，自动发布 broken-promise 错误，
 *   保证消费者**永远不会永久等待**；
 * - `Future::Get/GetFor/GetUntil/WaitFor` 阻塞读取，支持超时；
 * - 与 `Context` 集成：Context 取消会让等待方被唤醒并返回取消状态；
 * - `Future` 可复制、可跨线程/Fiber 共享，底层值在终态后只读。
 *
 * ## 实现层次
 *
 * | 层 | 职责 |
 * |---|---|
 * | `FutureWaitBackend` | 可替换的等待/唤醒协议（默认 ParkingCondition）|
 * | `detail::FutureState<T>` | 共享状态：值/错误/异常 + 一次性终态机 |
 * | `detail::FutureResult<T>` | 读取结果的返回值封装（含状态与错误）|
 * | `Promise<T>` | 写端，唯一完成者 |
 * | `Future<T>` | 读端，可复制、多等待者 |
 */
#pragma once

#include "go2cpp/context.hpp"
#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/error.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace go2cpp {

/**
 * Future 等待后端的可替换协议。
 *
 * 默认后端使用 ParkingCondition，因此受管 Fiber 会 park 而不会阻塞
 * carrier 线程，普通线程仍使用原生条件等待。插件可以在创建新的
 * Promise 之前注册自定义实现，例如接入其他 Fiber 调度器、事件循环或
 * 平台专用等待原语。后端只负责等待和广播，不拥有 Future 状态。
 */
class FutureWaitBackend {
public:
    /** @brief 释放等待后端。 */
    virtual ~FutureWaitBackend() = default;

    /**
     * @brief 等待谓词满足、到达截止时间或运行时取消。
     * @param lock Future 状态锁；调用时必须已经加锁，返回时保持加锁。
     * @param deadline 可选的 steady_clock 绝对截止时间。
     * @param predicate 状态谓词；后端必须允许虚假唤醒并重复检查。
     * @return 谓词满足返回 true，超时或取消返回 false。
     */
    virtual bool Wait(
        std::unique_lock<std::mutex>& lock,
        std::optional<ContextTimePoint> deadline,
        const std::function<bool()>& predicate) = 0;

    /** @brief 唤醒等待同一共享状态的全部调用者。 */
    virtual void NotifyAll() noexcept = 0;
};

using FutureWaitBackendFactory =
    std::function<std::shared_ptr<FutureWaitBackend>()>;

namespace detail {

/**
 * @brief Future 等待期间的 Context 回调作用域守卫。
 * @details Wait 后端可能抛出扩展点异常；守卫保证异常展开时仍移除 Done
 *          回调，避免取消树长期保留 Future 状态。析构函数永不向外抛出。
 *
 *          守卫只保存 `const Context*`，不复制 `ContextPtr`：Context 的
 *          生命周期由 Wait 的调用方保证（参数是 const 引用，调用期间不会
 *          释放），因此这里不需要为了“保活”再做一次原子引用计数。等待
 *          路径因此少两次 shared_ptr 原子操作。
 */
class FutureCallbackGuard final {
public:
    /** @brief 创建尚未绑定回调 ID 的守卫。 */
    explicit FutureCallbackGuard(const Context* context) noexcept
        : m_context(context) {}

    /** @brief 移除尚未清理的 Context 回调。 */
    ~FutureCallbackGuard() noexcept { Reset(); }

    FutureCallbackGuard(const FutureCallbackGuard&) = delete;
    FutureCallbackGuard& operator=(const FutureCallbackGuard&) = delete;

    /** @brief 记录 AddCallback 返回的 ID。 */
    void SetId(DoneSignal::CallbackId id) noexcept { m_id = id; }

    /** @brief 立即移除回调；重复调用安全。 */
    void Reset() noexcept {
        const Context* const context = m_context;
        const auto id = m_id;
        m_context = nullptr;
        m_id = 0;
        if (context == nullptr || id == 0) {
            return;
        }
        try {
            context->Done().RemoveCallback(id);
        } catch (...) {
            // 回调清理位于异常展开或析构路径，不能再次传播异常。
        }
    }

private:
    const Context* m_context;
    DoneSignal::CallbackId m_id{0};
};

}  // namespace detail

/**
 * @brief 注册新建 Future 使用的等待后端工厂。
 * @details 工厂只影响注册成功后创建的共享状态；已存在的 Future 不迁移
 *          后端，因此不会改变正在等待的调用者。传入空工厂恢复默认后端。
 * @param factory 后端工厂；工厂抛异常或返回空指针时自动回退默认实现。
 * @return 注册成功返回 true；并发注册时后一次调用覆盖前一次。
 */
bool SetFutureWaitBackendFactory(FutureWaitBackendFactory factory);

/** @brief 清除自定义等待后端，恢复默认 ParkingCondition 实现。 */
void ResetFutureWaitBackendFactory() noexcept;

/** @brief 创建一个 Future 状态使用的等待后端；供模板状态内部调用。 */
std::shared_ptr<FutureWaitBackend> CreateFutureWaitBackend() noexcept;

/**
 * Future 等待或完成后的结果状态。
 *
 * kReady/kError/kException/kCancelled 表示 Promise 已经进入终态；
 * kTimedOut/kDeadlineExceeded 表示本次等待没有改变 Future 状态，调用者
 * 可以稍后再次等待。kInvalid 表示 Future 没有关联有效的共享状态。
 */
/** @brief Future 单次等待或完成结果状态。 */
enum class FutureStatus : std::uint8_t {
    kReady,
    kError,
    kException,
    kCancelled,
    kTimedOut,
    kDeadlineExceeded,
    kInvalid,
};

const char* FutureStatusName(FutureStatus status) noexcept;

/**
 * 显式 GetOrThrow 失败时抛出的异常。运行时内部不会使用这个异常做跳转。
 */
/** @brief Future::GetOrThrow 抛出的状态异常。 */
class FutureError : public std::runtime_error {
public:
    /** @brief 创建带状态和 ErrorPtr 的 Future 异常。 */
    FutureError(FutureStatus status, ErrorPtr error);
    /** @brief 释放异常对象。 */
    ~FutureError() override;

    /** @brief 返回 Future 状态。 */
    FutureStatus status() const noexcept { return m_status; }
    /** @brief 返回底层错误。 */
    const ErrorPtr& error() const noexcept { return m_error; }

private:
    FutureStatus m_status;
    ErrorPtr m_error;
};

/** Promise 被销毁时仍未完成，对应 Future 收到的错误。 */
/**
 * @brief 创建 Promise 在销毁前未完成时的错误。
 * @note 本函数返回的是**进程级共享单例**：所有因 Promise 被放弃而产生的错误
 *       都是同一个对象，因此 `Is(errA, errB)` 对两个不同的 broken promise 会
 *       返回 true。这是为避免每次 Promise 析构重复分配而做的有意设计，
 *       与 `CanceledError()` / `DeadlineExceededError()` 等哨兵错误的用法一致。
 */
ErrorPtr BrokenPromiseError();

namespace detail {

enum class FutureStateKind : std::uint8_t {
    kPending,
    kValue,
    kError,
    kException,
    kCancelled,
    // 仅状态字内部使用：CAS 成功的写者已占用状态，正在写 payload。
    kClaimed,
};

// 共享状态字（m_word）的位布局：低 3 位是终态种类，bit3 表示 payload
// 已写完并对外发布。把“占用”和“发布”拆成两步，是为了让写者用一次
// CAS 独占 payload、再用一次 release store 发布终态；读端只需一次
// acquire load 即可安全读取 payload，不需要互斥锁。
constexpr std::uint32_t kFutureKindMask = 0x7u;
constexpr std::uint32_t kFuturePublishedBit = 0x8u;
constexpr std::uint32_t kFuturePendingWord = 0u;
constexpr std::uint32_t kFutureClaimedWord = 5u;
constexpr std::uint32_t kFutureValueWord =
    static_cast<std::uint32_t>(FutureStateKind::kValue);
constexpr std::uint32_t kFutureErrorWord =
    static_cast<std::uint32_t>(FutureStateKind::kError);
constexpr std::uint32_t kFutureExceptionWord =
    static_cast<std::uint32_t>(FutureStateKind::kException);
constexpr std::uint32_t kFutureCancelledWord =
    static_cast<std::uint32_t>(FutureStateKind::kCancelled);

/** @brief 把已发布的状态字映射为对外的 FutureStatus。 */
inline FutureStatus FutureStatusFromWord(std::uint32_t word) noexcept {
    switch (word & kFutureKindMask) {
    case kFutureValueWord:
        return FutureStatus::kReady;
    case kFutureErrorWord:
        return FutureStatus::kError;
    case kFutureExceptionWord:
        return FutureStatus::kException;
    case kFutureCancelledWord:
        return FutureStatus::kCancelled;
    default:
        // pending / claimed：尚未发布，等待方按“未完成”处理。
        return FutureStatus::kTimedOut;
    }
}

// 每个 Future 等待者有独立的取消标记。不能把标记放在共享状态中，
// 否则一个调用者的超时会误唤醒并影响其他等待者。回调只会写一次
// （Context 的 DoneSignal 保证回调至多执行一次），等待者只读。
struct FutureWaitRegistration final {
    std::atomic<bool> cancelled{false};
};

/**
 * @brief 等待者计数的 RAII 增减。
 * @details 构造时发布“本线程即将阻塞”，析构时撤销。完成方据此判断是否
 *          需要唤醒后端：无人等待时连一次虚调用都不做。递减放在析构里
 *          保证等待后端抛异常时计数不会泄漏（否则完成方会永远多做一次
 *          NotifyAll，虽然正确但会拖慢完成路径）。
 */
class FutureWaiterCount final {
public:
    explicit FutureWaiterCount(std::atomic<std::uint32_t>& count) noexcept
        : m_count(count) {
        m_count.fetch_add(1, std::memory_order_acq_rel);
    }

    ~FutureWaiterCount() noexcept {
        m_count.fetch_sub(1, std::memory_order_acq_rel);
    }

    FutureWaiterCount(const FutureWaiterCount&) = delete;
    FutureWaiterCount& operator=(const FutureWaiterCount&) = delete;

private:
    std::atomic<std::uint32_t>& m_count;
};

template <typename T, bool IsVoid = std::is_void_v<T>>
class FutureState;

/**
 * @brief 非 void Future 的共享状态（引用计数的核心）。
 *
 * ## 作用
 *
 * `Promise` 与所有 `Future` 副本共享同一个 `FutureState`：
 * 写端只负责把它推进到**某个终态**，读端等待并读取。生命周期由
 * `shared_ptr` 管理，因此读写两端的销毁顺序不需要协调。
 *
 * ## 一次性终态机（原子字，无互斥锁）
 *
 * `m_word` 是 32 位原子字：低 3 位是终态种类，bit3 是“payload 已发布”。
 * 写端先用一次 CAS 把 `kPending` 换成 `kClaimed`，**CAS 成功者独占写
 * payload**，再用 release 语义发布 `kind | kPublished`。CAS 失败者直接
 * 返回 false，因此 `SetValue / SetError / SetException / Cancel` 仍然
 * 只有第一个成功——与旧实现的互斥锁语义完全一致。
 *
 * 因为 CAS 与发布之间不再持锁，**无等待者时完成路径是纯原子操作**：
 * 不碰 `m_mutex`，也不调用唤醒后端。这正是 Go 中 `sync.Once` +
 * “先写结果再关 channel”能达到的成本量级。
 *
 * ## 读端快路径
 *
 * 读端用一次 acquire load 读 `m_word`；已发布时直接返回状态并读取
 * payload（release/acquire 建立 happens-before）。`Get`/`Status` 在
 * 已就绪时因此**无锁**，这是最常见的调用形态。
 *
 * ## 等待与唤醒
 *
 * 真正需要阻塞时才使用 `m_mutex` + `FutureWaitBackend`：
 *
 * - `m_waiters` 记录当前阻塞的等待者数量。完成路径看到 0 时跳过
 *   `NotifyAll()`，避免每完成一个 Future 都做一次虚调用/系统调用；
 * - 有等待者时，完成路径在 `m_mutex` 内发布终态。这一步不能省：
 *   `FutureWaitBackend::Wait` 接收 `unique_lock`，等待者的谓词检查与
 *   “进入条件等待”之间必须与发布串行化，否则可能丢失唤醒；
 * - 等待者在 `m_mutex` 内先递增 `m_waiters`、再**复检**终态，闭合
 *   “完成方在计数前看到 0、等待者随后才进入等待”的窗口。
 *
 * ## 并发要点
 *
 * - payload（`m_value` / `m_error` / `m_exception`）由 CAS 赢家单写，
 *   发布前读端不会读取；发布后只读；
 * - Context 只以 `const Context*` 形式参与，不拷贝 `shared_ptr`；
 * - 等待仍然经过 `FutureWaitBackend::Wait`，可替换后端（自定义调度器、
 *   事件循环）的行为不变。
 */
template <typename T>
class FutureState<T, false> final
    : public std::enable_shared_from_this<FutureState<T, false>> {
public:
    FutureState() : m_wait_backend(CreateFutureWaitBackend()) {}
    FutureState(const FutureState&) = delete;
    FutureState& operator=(const FutureState&) = delete;

    bool SetValue(T value) {
        // 值在 CAS 之前构造：用户类型的拷贝/移动不会落在“占用状态”与
        // “发布状态”之间；构造抛异常时状态仍是 pending，可重试。
        auto stored = std::make_shared<const T>(std::move(value));
        if (!Claim()) {
            return false;
        }
        m_value = std::move(stored);
        Publish(FutureStateKind::kValue);
        return true;
    }

    bool SetError(ErrorPtr error) {
        if (!error) {
            error = NewError("go2cpp::Promise::SetError received null error");
        }
        if (!Claim()) {
            return false;
        }
        m_error = std::move(error);
        Publish(FutureStateKind::kError);
        return true;
    }

    bool SetException(std::exception_ptr exception) {
        if (!exception) {
            return SetError(NewError(
                "go2cpp::Promise::SetException received null exception"));
        }
        if (!Claim()) {
            return false;
        }
        m_exception = std::move(exception);
        Publish(FutureStateKind::kException);
        return true;
    }

    bool Cancel(ErrorPtr error) {
        if (!error) {
            error = CanceledError();
        }
        if (!Claim()) {
            return false;
        }
        m_error = std::move(error);
        Publish(FutureStateKind::kCancelled);
        return true;
    }

    FutureStatus Wait(const ContextPtr& context,
                      std::optional<ContextTimePoint> deadline) const {
        // 快路径 1：终态已发布。一次 acquire load，无锁，也不碰 Context。
        // Get 绝大多数发生在 SetValue 之后，这条路径是主要收益来源。
        FutureStatus status = FutureStatus::kInvalid;
        if (TryGetPublished(status)) {
            return status;
        }

        const Context* const raw_context = context.get();
        // 快路径 2：Context 已取消/到期。省掉等待票据分配和回调注册；
        // 若终态在两次检查之间发布，仍以终态为准（与旧实现一致）。
        if (raw_context != nullptr && raw_context->IsDone()) {
            if (TryGetPublished(status)) {
                return status;
            }
            return ContextCancelStatus(raw_context);
        }
        // 快路径 3：无 Context 的零时长/已过期等待（TryGet、GetFor(0)）：
        // 直接按本地 deadline 返回，不分配任何等待对象。
        if (raw_context == nullptr && deadline &&
            Context::Now() >= *deadline) {
            return FutureStatus::kTimedOut;
        }

        // 慢路径：只有真正要阻塞时才分配等待票据、注册取消回调。
        auto registration = std::make_shared<FutureWaitRegistration>();
        FutureCallbackGuard callback_guard(raw_context);
        if (raw_context != nullptr) {
            // 先注册回调，再取得状态锁：若取消已经发生，AddCallback 会
            // 同步执行回调；此时尚未持有状态锁，不会自锁。
            callback_guard.SetId(raw_context->Done().AddCallback(
                [weak_state = this->weak_from_this(), registration] {
                    const auto state = weak_state.lock();
                    if (!state) {
                        return;
                    }
                    // 与等待者的谓词检查共用 m_mutex：保证“标记取消”与
                    // “检查谓词 → 进入等待”串行化，不会丢唤醒。
                    {
                        std::lock_guard<std::mutex> lock(state->m_mutex);
                        registration->cancelled.store(
                            true, std::memory_order_release);
                    }
                    state->m_wait_backend->NotifyAll();
                }));
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        const auto cancelled = [&]() noexcept {
            return registration->cancelled.load(std::memory_order_acquire) ||
                   (raw_context != nullptr && raw_context->IsDone());
        };
        if (!TryGetPublished(status) && !cancelled() &&
            !(deadline && Context::Now() >= *deadline) &&
            !core::ParkingCondition::CancellationRequested()) {
            // 计数必须在复检之前：完成方要么看到计数、在锁内发布并唤醒，
            // 要么在计数之前发布，被下面的复检看到。
            FutureWaiterCount waiter_count(m_waiters);
            if (!TryGetPublished(status) && !cancelled()) {
                const auto predicate = [&]() {
                    FutureStatus ignored;
                    return TryGetPublished(ignored) || cancelled();
                };
                if (deadline) {
                    (void)m_wait_backend->Wait(lock, *deadline, predicate);
                } else {
                    (void)m_wait_backend->Wait(lock, std::nullopt, predicate);
                }
            }
        }
        status = StatusLocked(cancelled(), raw_context, deadline);
        lock.unlock();
        callback_guard.Reset();
        return status;
    }

    FutureStatus Status() const noexcept {
        FutureStatus status = FutureStatus::kTimedOut;
        if (TryGetPublished(status)) {
            return status;
        }
        return FutureStatus::kTimedOut;
    }

    std::shared_ptr<const T> Value() const {
        // 只在发布后读取 payload；未发布时返回空，避免与写者竞争。
        if ((m_word.load(std::memory_order_acquire) & kFuturePublishedBit) ==
            0) {
            return {};
        }
        return m_value;
    }

    ErrorPtr ErrorValue() const {
        if ((m_word.load(std::memory_order_acquire) & kFuturePublishedBit) ==
            0) {
            return {};
        }
        return m_error;
    }

    std::exception_ptr ExceptionValue() const {
        if ((m_word.load(std::memory_order_acquire) & kFuturePublishedBit) ==
            0) {
            return {};
        }
        return m_exception;
    }

    /** @brief 已发布时取回终态；未发布返回 false。用于 Get 的无锁快路径。 */
    bool TryGetPublished(FutureStatus& status) const noexcept {
        const std::uint32_t word = m_word.load(std::memory_order_acquire);
        if ((word & kFuturePublishedBit) == 0) {
            return false;
        }
        status = FutureStatusFromWord(word);
        return true;
    }

private:
    /** @brief 抢占唯一写者资格；成功者负责写 payload 并发布。 */
    bool Claim() noexcept {
        std::uint32_t expected = kFuturePendingWord;
        return m_word.compare_exchange_strong(
            expected, kFutureClaimedWord, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    /** @brief 发布终态并唤醒等待者。 */
    void Publish(FutureStateKind kind) noexcept {
        const std::uint32_t word =
            static_cast<std::uint32_t>(kind) | kFuturePublishedBit;
        // 有等待者时必须持锁发布：等待者的谓词检查在锁内，这样“发布”
        // 与“检查后进入等待”被串行化，notify 不会落在两者之间而丢失。
        if (m_waiters.load(std::memory_order_acquire) != 0) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_word.store(word, std::memory_order_release);
            }
            // 唤醒在锁外，避免被唤醒者立刻再抢锁。
            m_wait_backend->NotifyAll();
            return;
        }
        m_word.store(word, std::memory_order_release);
        // 等待者可能在 store 之后才递增计数。补一次广播：此时它还没进入
        // 等待，会在复检中看到终态；若已经进入等待，这次广播正好唤醒它。
        if (m_waiters.load(std::memory_order_acquire) != 0) {
            m_wait_backend->NotifyAll();
        }
    }

    FutureStatus StatusLocked(bool cancelled, const Context* context,
                              std::optional<ContextTimePoint> deadline) const {
        FutureStatus status = FutureStatus::kInvalid;
        if (TryGetPublished(status)) {
            return status;
        }
        if (cancelled) {
            return ContextCancelStatus(context);
        }
        const auto now = Context::Now();
        if (context != nullptr) {
            const auto context_deadline = context->Deadline();
            if (context_deadline && now >= *context_deadline) {
                // Context 的截止时间优先于本次等待的本地 deadline；定时器
                // 线程可能尚未来得及把 done 标志发布到回调观察者。
                return FutureStatus::kDeadlineExceeded;
            }
        }
        if (deadline && now >= *deadline) {
            return FutureStatus::kTimedOut;
        }
        // ParkingCondition 可能因 scheduler cancellation/shutdown 返回，
        // 这时不能继续假装阻塞；调用者可再次等待。
        if (core::ParkingCondition::CancellationRequested()) {
            return FutureStatus::kCancelled;
        }
        return FutureStatus::kTimedOut;
    }

    static FutureStatus ContextCancelStatus(const Context* context) {
        const auto context_error =
            context != nullptr ? context->Err() : ErrorPtr{};
        if (context_error && Is(context_error, DeadlineExceededError())) {
            return FutureStatus::kDeadlineExceeded;
        }
        return FutureStatus::kCancelled;
    }

    mutable std::mutex m_mutex;
    std::shared_ptr<FutureWaitBackend> m_wait_backend;
    mutable std::atomic<std::uint32_t> m_word{kFuturePendingWord};
    mutable std::atomic<std::uint32_t> m_waiters{0};
    // payload：CAS 赢家单写，发布后只读。读端通过 m_word 的
    // release/acquire 建立 happens-before，因此不需要额外加锁。
    std::shared_ptr<const T> m_value;
    ErrorPtr m_error;
    std::exception_ptr m_exception;
};

template <typename T>
class FutureState<T, true> final
    : public std::enable_shared_from_this<FutureState<T, true>> {
public:
    FutureState() : m_wait_backend(CreateFutureWaitBackend()) {}
    FutureState(const FutureState&) = delete;
    FutureState& operator=(const FutureState&) = delete;

    bool SetValue() {
        if (!Claim()) {
            return false;
        }
        Publish(FutureStateKind::kValue);
        return true;
    }

    bool SetError(ErrorPtr error) {
        if (!error) {
            error = NewError("go2cpp::Promise::SetError received null error");
        }
        if (!Claim()) {
            return false;
        }
        m_error = std::move(error);
        Publish(FutureStateKind::kError);
        return true;
    }

    bool SetException(std::exception_ptr exception) {
        if (!exception) {
            return SetError(NewError(
                "go2cpp::Promise::SetException received null exception"));
        }
        if (!Claim()) {
            return false;
        }
        m_exception = std::move(exception);
        Publish(FutureStateKind::kException);
        return true;
    }

    bool Cancel(ErrorPtr error) {
        if (!error) {
            error = CanceledError();
        }
        if (!Claim()) {
            return false;
        }
        m_error = std::move(error);
        Publish(FutureStateKind::kCancelled);
        return true;
    }

    FutureStatus Wait(const ContextPtr& context,
                      std::optional<ContextTimePoint> deadline) const {
        // 快路径 1：终态已发布。一次 acquire load，无锁，也不碰 Context。
        FutureStatus status = FutureStatus::kInvalid;
        if (TryGetPublished(status)) {
            return status;
        }

        const Context* const raw_context = context.get();
        // 快路径 2：Context 已取消/到期。省掉等待票据分配和回调注册。
        if (raw_context != nullptr && raw_context->IsDone()) {
            if (TryGetPublished(status)) {
                return status;
            }
            return ContextCancelStatus(raw_context);
        }
        // 快路径 3：无 Context 的零时长/已过期等待（TryGet、GetFor(0)）。
        if (raw_context == nullptr && deadline &&
            Context::Now() >= *deadline) {
            return FutureStatus::kTimedOut;
        }

        auto registration = std::make_shared<FutureWaitRegistration>();
        FutureCallbackGuard callback_guard(raw_context);
        if (raw_context != nullptr) {
            callback_guard.SetId(raw_context->Done().AddCallback(
                [weak_state = this->weak_from_this(), registration] {
                    const auto state = weak_state.lock();
                    if (!state) {
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(state->m_mutex);
                        registration->cancelled.store(
                            true, std::memory_order_release);
                    }
                    state->m_wait_backend->NotifyAll();
                }));
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        const auto cancelled = [&]() noexcept {
            return registration->cancelled.load(std::memory_order_acquire) ||
                   (raw_context != nullptr && raw_context->IsDone());
        };
        if (!TryGetPublished(status) && !cancelled() &&
            !(deadline && Context::Now() >= *deadline) &&
            !core::ParkingCondition::CancellationRequested()) {
            FutureWaiterCount waiter_count(m_waiters);
            if (!TryGetPublished(status) && !cancelled()) {
                const auto predicate = [&]() {
                    FutureStatus ignored;
                    return TryGetPublished(ignored) || cancelled();
                };
                if (deadline) {
                    (void)m_wait_backend->Wait(lock, *deadline, predicate);
                } else {
                    (void)m_wait_backend->Wait(lock, std::nullopt, predicate);
                }
            }
        }
        status = StatusLocked(cancelled(), raw_context, deadline);
        lock.unlock();
        callback_guard.Reset();
        return status;
    }

    FutureStatus Status() const noexcept {
        FutureStatus status = FutureStatus::kTimedOut;
        if (TryGetPublished(status)) {
            return status;
        }
        return FutureStatus::kTimedOut;
    }

    ErrorPtr ErrorValue() const {
        if ((m_word.load(std::memory_order_acquire) & kFuturePublishedBit) ==
            0) {
            return {};
        }
        return m_error;
    }

    std::exception_ptr ExceptionValue() const {
        if ((m_word.load(std::memory_order_acquire) & kFuturePublishedBit) ==
            0) {
            return {};
        }
        return m_exception;
    }

    /** @brief 已发布时取回终态；未发布返回 false。用于 Get 的无锁快路径。 */
    bool TryGetPublished(FutureStatus& status) const noexcept {
        const std::uint32_t word = m_word.load(std::memory_order_acquire);
        if ((word & kFuturePublishedBit) == 0) {
            return false;
        }
        status = FutureStatusFromWord(word);
        return true;
    }

private:
    bool Claim() noexcept {
        std::uint32_t expected = kFuturePendingWord;
        return m_word.compare_exchange_strong(
            expected, kFutureClaimedWord, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    void Publish(FutureStateKind kind) noexcept {
        const std::uint32_t word =
            static_cast<std::uint32_t>(kind) | kFuturePublishedBit;
        if (m_waiters.load(std::memory_order_acquire) != 0) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_word.store(word, std::memory_order_release);
            }
            m_wait_backend->NotifyAll();
            return;
        }
        m_word.store(word, std::memory_order_release);
        if (m_waiters.load(std::memory_order_acquire) != 0) {
            m_wait_backend->NotifyAll();
        }
    }

    FutureStatus StatusLocked(bool cancelled, const Context* context,
                              std::optional<ContextTimePoint> deadline) const {
        FutureStatus status = FutureStatus::kInvalid;
        if (TryGetPublished(status)) {
            return status;
        }
        if (cancelled) {
            return ContextCancelStatus(context);
        }
        const auto now = Context::Now();
        if (context != nullptr) {
            const auto context_deadline = context->Deadline();
            if (context_deadline && now >= *context_deadline) {
                return FutureStatus::kDeadlineExceeded;
            }
        }
        if (deadline && now >= *deadline) {
            return FutureStatus::kTimedOut;
        }
        if (core::ParkingCondition::CancellationRequested()) {
            return FutureStatus::kCancelled;
        }
        return FutureStatus::kTimedOut;
    }

    static FutureStatus ContextCancelStatus(const Context* context) {
        const auto context_error =
            context != nullptr ? context->Err() : ErrorPtr{};
        if (context_error && Is(context_error, DeadlineExceededError())) {
            return FutureStatus::kDeadlineExceeded;
        }
        return FutureStatus::kCancelled;
    }

    mutable std::mutex m_mutex;
    std::shared_ptr<FutureWaitBackend> m_wait_backend;
    mutable std::atomic<std::uint32_t> m_word{kFuturePendingWord};
    mutable std::atomic<std::uint32_t> m_waiters{0};
    ErrorPtr m_error;
    std::exception_ptr m_exception;
};

template <typename T>
std::optional<ContextTimePoint> MergeDeadline(
    const ContextPtr& context, std::optional<ContextTimePoint> deadline) {
    if (!context) {
        return deadline;
    }
    const auto context_deadline = context->Deadline();
    if (!context_deadline) {
        return deadline;
    }
    if (!deadline || *context_deadline < *deadline) {
        return context_deadline;
    }
    return deadline;
}

template <typename T>
std::optional<ContextTimePoint> DeadlineAfter(ContextDuration timeout) {
    const auto now = Context::Now();
    if (timeout <= ContextDuration::zero()) {
        return now;
    }
    if (now > ContextTimePoint::max() - timeout) {
        return ContextTimePoint::max();
    }
    return now + timeout;
}

}  // namespace detail

template <typename T>
/** @brief 非 void Future 的完成值、状态和错误。 */
struct FutureResult {
    FutureStatus status{FutureStatus::kInvalid};
    std::shared_ptr<const T> value;
    ErrorPtr error;
    std::exception_ptr exception;

    bool Ok() const noexcept { return status == FutureStatus::kReady; }
    bool Ready() const noexcept {
        return status == FutureStatus::kReady ||
               status == FutureStatus::kError ||
               status == FutureStatus::kException ||
               status == FutureStatus::kCancelled;
    }
    bool TimedOut() const noexcept {
        return status == FutureStatus::kTimedOut ||
               status == FutureStatus::kDeadlineExceeded;
    }
    bool Cancelled() const noexcept {
        return status == FutureStatus::kCancelled ||
               status == FutureStatus::kDeadlineExceeded;
    }
    const T* Value() const noexcept { return value.get(); }
    explicit operator bool() const noexcept { return Ok(); }

    // 只在 T 可复制时提供便捷复制；移动类型请使用 Value() 指针或
    // SharedValue()，这样不会隐式消费共享 Future 的值。
    template <typename U = T,
              typename = std::enable_if_t<std::is_copy_constructible_v<U>>>
    std::optional<U> CopyValue() const {
        if (!Ok() || !value) {
            return std::nullopt;
        }
        return *value;
    }
};

template <>
/** @brief void Future 的完成状态和错误。 */
struct FutureResult<void> {
    FutureStatus status{FutureStatus::kInvalid};
    ErrorPtr error;
    std::exception_ptr exception;

    bool Ok() const noexcept { return status == FutureStatus::kReady; }
    bool Ready() const noexcept {
        return status == FutureStatus::kReady ||
               status == FutureStatus::kError ||
               status == FutureStatus::kException ||
               status == FutureStatus::kCancelled;
    }
    bool TimedOut() const noexcept {
        return status == FutureStatus::kTimedOut ||
               status == FutureStatus::kDeadlineExceeded;
    }
    bool Cancelled() const noexcept {
        return status == FutureStatus::kCancelled ||
               status == FutureStatus::kDeadlineExceeded;
    }
    explicit operator bool() const noexcept { return Ok(); }
};

template <typename T>
class Future;

template <typename T>
/**
 * @brief Future 的唯一完成端（一次性生产端）。
 * @details Promise 可由一个线程/Fiber 设置值、错误或异常；对应 Future 可
 *          被多个等待方安全读取。
 *
 *          依赖 `detail::FutureState` 保存共享状态，`Context` / `DoneSignal`
 *          负责取消和等待唤醒，`Error` / `exception_ptr` 表示失败。
 *          对上层提供 SetValue / SetError / SetException / Cancel。
 *          Promise 析构时若仍未完成会发布 BrokenPromise，保证消费者不会
 *          永久等待。Promise 不能复制，只能移动。
 */
class Promise final {
public:
    /** @brief 创建未完成 Promise。 */
    Promise() : m_state(std::make_shared<detail::FutureState<T>>()) {}
    ~Promise() {
        // Promise 析构不是异常路径；未完成时发布可观察的 broken-promise
        // 错误，确保 Future 永远不会永久等待。
        if (m_state) {
            (void)m_state->SetError(BrokenPromiseError());
        }
    }

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;

    Promise(Promise&& other) noexcept : m_state(std::move(other.m_state)) {}
    Promise& operator=(Promise&& other) noexcept {
        if (this != &other) {
            if (m_state) {
                (void)m_state->SetError(BrokenPromiseError());
            }
            m_state = std::move(other.m_state);
        }
        return *this;
    }

    /** @brief 返回 Promise 是否持有有效共享状态。 */
    bool valid() const noexcept { return static_cast<bool>(m_state); }
    bool Valid() const noexcept { return valid(); }

    // 返回与本 Promise 共享状态的 Future；多次调用得到的 Future 可并行等待。
    /** @brief 获取对应 Future 读取端。 */
    Future<T> GetFuture() const noexcept;
    Future<T> future() const noexcept { return GetFuture(); }

    template <typename U = T,
              typename = std::enable_if_t<!std::is_void_v<U>>>
    // 发布成功值；只有第一个终态写入者返回 true，后续写入返回 false。
    /** @brief 设置非 void 结果值，仅第一次调用成功。 */
    bool SetValue(U value) {
        return m_state && m_state->SetValue(std::move(value));
    }

    template <typename U = T,
              typename = std::enable_if_t<std::is_void_v<U>>, int = 0>
    /** @brief 设置 void Future 成功状态。 */
    bool SetValue() {
        return m_state && m_state->SetValue();
    }

    // 发布错误值；error 为空时由实现生成参数错误。
    /** @brief 设置错误结果。 */
    bool SetError(ErrorPtr error) {
        return m_state && m_state->SetError(std::move(error));
    }
    // 发布异常指针；空指针会转换为错误状态。
    /** @brief 设置异常结果。 */
    bool SetException(std::exception_ptr exception) {
        return m_state && m_state->SetException(std::move(exception));
    }
    // 发布取消终态；error 为空时使用 CanceledError。
    /** @brief 以取消错误完成 Promise。 */
    bool Cancel(ErrorPtr error = {}) {
        return m_state && m_state->Cancel(std::move(error));
    }

    // 小写别名便于转译代码贴近 Go/C++ 标准库调用风格。
    template <typename U = T,
              typename = std::enable_if_t<!std::is_void_v<U>>>
    bool set_value(U value) {
        return SetValue(std::move(value));
    }
    bool set_error(ErrorPtr error) { return SetError(std::move(error)); }
    bool set_exception(std::exception_ptr exception) {
        return SetException(std::move(exception));
    }
    bool cancel(ErrorPtr error = {}) { return Cancel(std::move(error)); }

private:
    std::shared_ptr<detail::FutureState<T>> m_state;
    friend class Future<T>;
};

template <typename T>
/**
 * @brief 可复制的异步结果读取端（共享消费端）。
 * @details 支持阻塞、超时、Context 取消和 GetOrThrow；底层状态通过共享
 *          `FutureState` 管理生命周期。
 *
 *          依赖 `Promise` / `detail::FutureState` 的一次性终态，以及
 *          `Context` 的取消和 Deadline。对上层提供等待、超时等待、状态
 *          查询和 `FutureResult`；默认只返回状态，不主动抛异常，调用者
 *          明确使用 GetOrThrow 时才转换为异常。
 *          Future 可复制并在线程与 Fiber 间共享，底层值在终态后只读。
 */
class Future final {
public:
    /** @brief 创建无效 Future。 */
    Future() = default;

    /** @brief 返回是否关联有效共享状态。 */
    bool valid() const noexcept { return static_cast<bool>(m_state); }
    bool Valid() const noexcept { return valid(); }

    // 无限等待终态；返回 FutureStatus。
    /** @brief 无限等待完成或 Context 取消。 */
    FutureStatus Wait(const ContextPtr& context = {}) const {
        return WaitUntil(context, std::nullopt);
    }

    // 最多等待 timeout；超时只影响本次等待，不改变 Future 状态。
    /** @brief 等待相对时长。 */
    FutureStatus WaitFor(ContextDuration timeout,
                         const ContextPtr& context = {}) const {
        if (!m_state) {
            return FutureStatus::kInvalid;
        }
        // 已就绪快路径：连 deadline 计算（Context::Now）都省掉。
        FutureStatus status = FutureStatus::kInvalid;
        if (m_state->TryGetPublished(status)) {
            return status;
        }
        if (timeout <= ContextDuration::zero()) {
            // 零/负超时只探测当前状态：不计算 deadline，也不注册取消回调。
            if (context && context->IsDone()) {
                return m_state->Wait(context, std::nullopt);
            }
            return FutureStatus::kTimedOut;
        }
        return WaitUntil(context, detail::DeadlineAfter<T>(timeout));
    }

    /** @brief 等待到绝对截止时间。 */
    FutureStatus WaitUntil(
        const ContextPtr& context,
        std::optional<ContextTimePoint> deadline) const {
        if (!m_state) {
            return FutureStatus::kInvalid;
        }
        // 已完成时 deadline 合并与 Context 检查都不参与（与旧语义一致），
        // 一次 acquire load 即可返回。
        FutureStatus status = FutureStatus::kInvalid;
        if (m_state->TryGetPublished(status)) {
            return status;
        }
        return m_state->Wait(context,
                             detail::MergeDeadline<T>(context, deadline));
    }

    // 等待并返回值/错误/异常的结构化结果，不抛出异常。
    /** @brief 等待并返回结构化结果。 */
    FutureResult<T> GetResult(const ContextPtr& context = {}) const {
        return GetResultUntil(context, std::nullopt);
    }

    // 带相对超时取得结构化结果；超时返回 kTimedOut 或 kDeadlineExceeded。
    /** @brief 在相对时限内等待并返回结构化结果。 */
    FutureResult<T> GetResultFor(ContextDuration timeout,
                                 const ContextPtr& context = {}) const {
        FutureResult<T> result;
        if (!m_state) {
            result.status = FutureStatus::kInvalid;
            return result;
        }
        // 已就绪快路径：连 deadline 计算都省掉（TryGet/GetFor(0) 是热点）。
        if (!m_state->TryGetPublished(result.status)) {
            if (timeout <= ContextDuration::zero()) {
                // 零/负超时只探测：不计算 deadline，也不注册取消回调。
                if (context && context->IsDone()) {
                    result.status = m_state->Wait(context, std::nullopt);
                } else {
                    result.status = FutureStatus::kTimedOut;
                }
            } else {
                result.status = m_state->Wait(
                    context,
                    detail::MergeDeadline<T>(context,
                                             detail::DeadlineAfter<T>(timeout)));
            }
        }
        FillResult(result);
        return result;
    }

    /** @brief 在绝对截止时间前等待并返回结构化结果。 */
    FutureResult<T> GetResultUntil(
        const ContextPtr& context,
        std::optional<ContextTimePoint> deadline) const {
        FutureResult<T> result;
        if (!m_state) {
            result.status = FutureStatus::kInvalid;
            return result;
        }
        // 已就绪快路径：一次 acquire load，完全绕开 deadline 合并、Context
        // 访问与状态锁 —— Get 的绝大多数调用发生在 Promise 完成之后。
        if (!m_state->TryGetPublished(result.status)) {
            result.status = m_state->Wait(
                context, detail::MergeDeadline<T>(context, deadline));
        }
        FillResult(result);
        return result;
    }

    // 零超时探测当前结果；未完成时立即返回超时状态。
    /** @brief 非阻塞读取当前结果。 */
    FutureResult<T> TryGet() const {
        return GetResultFor(ContextDuration::zero());
    }

    // 返回共享状态的当前终态快照；无状态 Future 返回 kInvalid。
    /** @brief 返回当前状态，不等待。 */
    FutureStatus Status() const noexcept {
        return m_state ? m_state->Status() : FutureStatus::kInvalid;
    }

    // 显式异常边界：Future 内部始终返回状态；只有调用者明确选择
    // GetOrThrow 时，error/exception 才会转换为 C++ 异常。
    template <typename U = T,
              std::enable_if_t<!std::is_void_v<U>, int> = 0>
    U GetOrThrow(const ContextPtr& context = {}) const {
        static_assert(std::is_copy_constructible_v<U>,
                      "GetOrThrow requires a copy-constructible value; "
                      "use GetResult().Value() for move-only values");
        const auto result = GetResult(context);
        if (result.status == FutureStatus::kReady && result.value) {
            return *result.value;
        }
        ThrowResult(result.status, result.error, result.exception);
    }

    template <typename U = T,
              std::enable_if_t<std::is_void_v<U>, int> = 0>
    void GetOrThrow(const ContextPtr& context = {}) const {
        const auto result = GetResult(context);
        if (result.status == FutureStatus::kReady) {
            return;
        }
        ThrowResult(result.status, result.error, result.exception);
    }

    FutureResult<T> get(const ContextPtr& context = {}) const {
        return GetResult(context);
    }
    FutureResult<T> get_for(ContextDuration timeout,
                            const ContextPtr& context = {}) const {
        return GetResultFor(timeout, context);
    }

private:
    /** @brief 按终态把 payload 填进结果（只拷贝对应的一项）。 */
    void FillResult(FutureResult<T>& result) const {
        if constexpr (!std::is_void_v<T>) {
            if (result.status == FutureStatus::kReady) {
                result.value = m_state->Value();
            }
        }
        if (result.status == FutureStatus::kError ||
            result.status == FutureStatus::kCancelled) {
            result.error = m_state->ErrorValue();
        } else if (result.status == FutureStatus::kException) {
            result.exception = m_state->ExceptionValue();
        }
    }

    explicit Future(std::shared_ptr<detail::FutureState<T>> state)
        : m_state(std::move(state)) {}

    [[noreturn]] static void ThrowResult(FutureStatus status,
                                         const ErrorPtr& error,
                            const std::exception_ptr& exception) {
        if (status == FutureStatus::kException && exception) {
            std::rethrow_exception(exception);
        }
        ErrorPtr actual = error;
        if (!actual) {
            switch (status) {
            case FutureStatus::kCancelled:
                actual = CanceledError();
                break;
            case FutureStatus::kDeadlineExceeded:
                actual = DeadlineExceededError();
                break;
            case FutureStatus::kTimedOut:
                actual = NewError("go2cpp future wait timed out");
                break;
            case FutureStatus::kInvalid:
                actual = NewError("go2cpp invalid future");
                break;
            default:
                actual = NewError("go2cpp future completed without a value");
                break;
            }
        }
        throw FutureError(status, std::move(actual));
    }

    std::shared_ptr<detail::FutureState<T>> m_state;
    friend class Promise<T>;
};

template <typename T>
Future<T> Promise<T>::GetFuture() const noexcept {
    return Future<T>(m_state);
}

template <typename T>
std::pair<Promise<T>, Future<T>> MakePromise() {
    Promise<T> promise;
    auto future = promise.GetFuture();
    return {std::move(promise), std::move(future)};
}

}  // namespace go2cpp