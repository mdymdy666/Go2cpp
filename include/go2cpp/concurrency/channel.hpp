/**
 * @file concurrency/channel.hpp
 * @brief Go 风格 channel 与 select 的完整实现（模板，header-only）。
 *
 * ## 模块定位
 *
 * `Channel<T>` 是 Go `chan T` 的对应物：带缓冲/无缓冲、`Send/Recv/TrySend/
 * TryRecv/Close/Range`，语义与 Go 的 `hchan` 对齐（预分配环形缓冲 +
 * `sendx/recvx/dataqsiz`，**刻意不用 `std::deque`**，见 §缓冲策略）。
 *
 * 它建在同步原语之上、业务代码之下：
 *
 * - 业务层（`example/concurrency/channel_full.cpp`、`http2` 的流控实验、
 *   压测的 ping-pong）直接使用 `Channel<T>`；
 * - `Select` 系列（`SelectValue`/`SelectCase`/`SelectWaitState`）把「多个
 *   channel 同时等待」编译成一次轮询 + 一个等待节点，供 `select { case <-ch: }`
 *   形态的代码使用；
 * - 底层 park/wake 走 `core::ParkingCondition`：受管 G 在此 park 并让出 M，
 *   普通线程走原生条件变量 —— 因此 channel 阻塞**不会占住 worker**。
 *
 * ## 本文件定义什么
 *
 * - `ChannelStatus`：操作结果（Ready/WouldBlock/Closed/AlreadyClosed/Cancelled/
 *   DeadlineExceeded/Empty/Invalid）；
 * - `ChannelSendResult<T>` / `ChannelRecvResult<T>`：带状态与值的返回结构；
 * - `SelectValue` / `SelectCaster` / `SelectCase` / `SelectProbe` /
 *   `SelectWaitState`：select 的编译期分发与运行期等待节点；
 * - `RingBuffer<T>`：`Channel<T>` 的环形缓冲（就地构造，`cap==0` 不分配）；
 * - `Channel<T>` 及其只读/只写视图（`SendOnlyChannel`/`RecvOnlyChannel`）。
 *
 * ## 缓冲策略（为什么不用 std::deque）
 *
 * Go 的 `hchan` 是「预分配数组 + sendx/recvx/dataqsiz」，索引 `++` 后回绕，
 * **锁内不做取模**（MPMC 下取模更慢）。我们保持同样的布局，这样：
 * 1. 与 Go 的语义、内存布局直觉一致，便于对照；
 * 2. 避免 `deque` 的额外一层间接与节点分配；
 * 3. 关闭/排空时的遍历顺序与 Go 相同。
 * 曾评估「原子标志 + Try* 无锁快路径」，**否决**：偏离 hchan，且漏一处就是
 * 偶发丢值（这类 bug 不崩溃、只丢数据）。
 *
 * ## 阅读建议
 *
 * 先看 `ChannelStatus` 与两个 Result 结构建立「操作可能返回什么」的心智模型；
 * 再看 `Channel<T>` 的 Send/Recv/Close；最后看 `Select*` —— 它把 N 个 channel
 * 的等待折叠成一个 `SelectWaitState`，理解关键是「先轮询、再登记、最后 park」。
 *
 * ## 边界与陷阱
 *
 * - 无缓冲 channel 的 `Send` 会等到有接收者（会 park），`cap==0` 不分配缓冲；
 * - `Close` 后 `Recv` 返回 `kClosed`；对已关闭的 channel `Send` 返回
 *   `kAlreadyClosed`（不抛异常）；
 * - `Range` 语义：关闭后仍能把缓冲区里的剩余值读完，再退出；
 * - 手动 Fiber（未归属 Scheduler）里 `Select` 返回 `kInvalid`，而
 *   `ParkingCondition` 返回 `kCancelled` —— 两者语义不同，别当成等价。
 */
#pragma once

#include "go2cpp/context.hpp"
#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/core/timer.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_resources.hpp"
#include "go2cpp/error.hpp"
#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
// 迁移 API：等待状态类型与 ChannelMigrationBase 都定义在 fiber.hpp。
// 默认构建不包含该头，保持原有编译依赖与类型布局。
#include "go2cpp/concurrency/fiber.hpp"
#endif

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp {

/**
 * @brief Channel/Select 操作结果状态。
 * @details kReady 表示成功，kClosed 表示已关闭读取，其他状态描述取消、
 *          超时、空通道和参数错误。
 */
enum class ChannelStatus {
  kReady = 0,
  kWouldBlock,
  kClosed,
  kAlreadyClosed,
  kCancelled,
  kDeadlineExceeded,
  kTimedOut,
  kNil,
  kInvalid,
};

/** @brief 判断状态是否代表一次可交付的结果。 */
inline bool ChannelStatusIsReady(ChannelStatus status) noexcept {
  return status == ChannelStatus::kReady || status == ChannelStatus::kClosed;
}

/** @brief 创建通道已关闭错误。 */
ErrorPtr ChannelClosedError() noexcept;
/** @brief 创建重复关闭错误。 */
ErrorPtr ChannelAlreadyClosedError() noexcept;
/** @brief 创建非阻塞操作暂不可用错误。 */
ErrorPtr ChannelWouldBlockError() noexcept;
/** @brief 创建 nil channel 错误。 */
ErrorPtr ChannelNilError() noexcept;
/** @brief 创建通道超时错误。 */
ErrorPtr ChannelTimeoutError() noexcept;

// SelectValue 是 select 结果使用的共享类型擦除载体。它不要求实际值可复制，
// 用户可以通过 Get/Take 或 SelectCaster 将它转换成自己的业务类型。
/**
 * Select 结果的类型擦除值。
 *
 * 依赖：内部 Concept/Holder 保存实际对象，std::any 仅作为兼容桥；对上层
 * 提供类型查询、只读 Get、独占 Take 和安全 As 转换。对象可在线程/Fiber
 * 间共享，但只有唯一持有者才能 Take move-only 值。
 */
class SelectValue final {
 private:
  struct Concept {
    virtual ~Concept() = default;
    virtual const std::type_info& Type() const noexcept = 0;
    virtual const void* Data() const noexcept = 0;
    virtual void* MutableData() noexcept = 0;
    virtual std::any ToAny() const = 0;
  };

  template <typename T>
  struct Holder final : Concept {
    template <typename U>
    explicit Holder(U&& value) : value(std::forward<U>(value)) {}

    const std::type_info& Type() const noexcept override { return typeid(T); }
    const void* Data() const noexcept override { return &value; }
    void* MutableData() noexcept override { return &value; }
    std::any ToAny() const override {
      if constexpr (std::is_copy_constructible<T>::value) {
        return std::any(value);
      } else {
        return {};
      }
    }

    T value;
  };

  // 兼容用户仍以 std::any 填充自定义 SelectProbe 的场景。类型信息
  // 保留在 any 中，Caster 可通过 As<T>() 取回具体值。
  struct AnyHolder final : Concept {
    explicit AnyHolder(std::any input) : value(std::move(input)) {}

    const std::type_info& Type() const noexcept override {
      return value.has_value() ? value.type() : typeid(void);
    }
    const void* Data() const noexcept override { return nullptr; }
    void* MutableData() noexcept override { return nullptr; }
    std::any ToAny() const override { return value; }

    std::any value;
  };

 public:
  SelectValue() noexcept = default;

  template <typename T>
  static SelectValue From(T&& value) {
    using ValueType = std::decay_t<T>;
    SelectValue result;
    result.m_value = std::make_shared<Holder<ValueType>>(
        std::forward<T>(value));
    return result;
  }

  static SelectValue FromAny(std::any value) {
    if (!value.has_value()) {
      return {};
    }
    SelectValue result;
    result.m_value = std::make_shared<AnyHolder>(std::move(value));
    return result;
  }

  bool HasValue() const noexcept { return static_cast<bool>(m_value); }
  explicit operator bool() const noexcept { return HasValue(); }

  const std::type_info& Type() const noexcept {
    return m_value ? m_value->Type() : typeid(void);
  }

  template <typename T>
  const std::decay_t<T>* Get() const noexcept {
    using ValueType = std::decay_t<T>;
    if (!m_value || m_value->Type() != typeid(ValueType)) {
      return nullptr;
    }
    if (const auto* value = m_value->Data()) {
      return static_cast<const ValueType*>(value);
    }
    if (const auto* any_holder = dynamic_cast<const AnyHolder*>(m_value.get())) {
      return std::any_cast<ValueType>(&any_holder->value);
    }
    return nullptr;
  }

  template <typename T>
  std::decay_t<T>* GetMutable() noexcept {
    using ValueType = std::decay_t<T>;
    if (!m_value || m_value->Type() != typeid(ValueType)) {
      return nullptr;
    }
    if (auto* value = m_value->MutableData()) {
      return static_cast<ValueType*>(value);
    }
    if (auto* any_holder = dynamic_cast<AnyHolder*>(m_value.get())) {
      return std::any_cast<ValueType>(&any_holder->value);
    }
    return nullptr;
  }

  template <typename T>
  std::optional<std::decay_t<T>> As() const {
    using ValueType = std::decay_t<T>;
    const auto* value = Get<ValueType>();
    if (value) {
      if constexpr (std::is_copy_constructible<ValueType>::value) {
        try {
          return *value;
        } catch (...) {
          return std::nullopt;
        }
      } else {
        return std::nullopt;
      }
    }

    // AnyHolder 没有可移植的 void* 访问，但 std::any_cast 能保留类型安全。
    if constexpr (std::is_copy_constructible<ValueType>::value) {
      try {
        auto any_value = ToAny();
        return std::any_cast<ValueType>(std::move(any_value));
      } catch (...) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }

  template <typename T>
  std::optional<std::decay_t<T>> Take() {
    using ValueType = std::decay_t<T>;
    if (m_value.use_count() != 1) {
      return std::nullopt;
    }
    if (auto* value = GetMutable<ValueType>()) {
      if constexpr (std::is_move_constructible<ValueType>::value) {
        try {
          return std::move(*value);
        } catch (...) {
          return std::nullopt;
        }
      } else {
        return std::nullopt;
      }
    }

    auto* any_holder = dynamic_cast<AnyHolder*>(m_value.get());
    if (!any_holder) {
      return std::nullopt;
    }
    if constexpr (std::is_move_constructible<ValueType>::value) {
      if (auto* value = std::any_cast<ValueType>(&any_holder->value)) {
        try {
          auto result = std::move(*value);
          any_holder->value.reset();
          return result;
        } catch (...) {
          return std::nullopt;
        }
      }
    }
    return std::nullopt;
  }

  // 仅用于兼容旧的 std::any 访问。不可复制对象会返回空 any。
  std::any ToAny() const {
    return m_value ? m_value->ToAny() : std::any{};
  }

 private:
  std::shared_ptr<Concept> m_value;
};

/**
 * SelectValue 的类型转换中介。
 *
 * 依赖：SelectValue 的类型擦除对象；Select/Channel 用它把不同通道的值
 * 交给统一结果。对上层提供可插拔 Cast 接口，转换失败返回空值，不把用户
 * 异常带入 select 状态机。
 */
class SelectCaster {
 public:
  virtual ~SelectCaster() = default;

  // 工厂转换器会把用户函数异常转换为空值；自定义实现也应返回失败值，
  // 不要把异常带入 select 状态机。SelectResult::Cast 还会做最后一道隔离。
  virtual SelectValue Cast(const SelectValue& source) const = 0;

  template <typename From, typename To, typename Function>
  static std::shared_ptr<const SelectCaster> Create(Function&& function);
};

using Caster = SelectCaster;

template <typename From, typename To, typename Function>
std::shared_ptr<const SelectCaster> MakeSelectCaster(Function&& function) {
  return SelectCaster::Create<From, To>(std::forward<Function>(function));
}

template <typename From, typename To, typename Function>
std::shared_ptr<const SelectCaster> MakeCaster(Function&& function) {
  return MakeSelectCaster<From, To>(std::forward<Function>(function));
}

namespace detail {

/// ChannelStatus → 资源事件结果（稳定映射；只用于诊断字段）。
inline go2cpp::debug::ResourceResult ChannelDebugResult(
    ChannelStatus status) noexcept {
  switch (status) {
    case ChannelStatus::kReady:
      return go2cpp::debug::ResourceResult::kOk;
    case ChannelStatus::kWouldBlock:
      return go2cpp::debug::ResourceResult::kWouldBlock;
    case ChannelStatus::kClosed:
    case ChannelStatus::kAlreadyClosed:
      return go2cpp::debug::ResourceResult::kClosed;
    case ChannelStatus::kCancelled:
      return go2cpp::debug::ResourceResult::kCancelled;
    case ChannelStatus::kDeadlineExceeded:
    case ChannelStatus::kTimedOut:
      return go2cpp::debug::ResourceResult::kTimeout;
    case ChannelStatus::kNil:
    case ChannelStatus::kInvalid:
      return go2cpp::debug::ResourceResult::kFailed;
  }
  return go2cpp::debug::ResourceResult::kFailed;
}

/// channel 值的调试摘要：整型/枚举/指针可直接比较，其它类型为 0。
template <typename T>
inline std::uint64_t ChannelDebugValue(const T& value) noexcept {
  return go2cpp::debug::ResourceValueDigest(value);
}

/// 值摘要是否有效（写入事件的 kResourceFlagValueKnown）。
template <typename T>
inline constexpr std::uint32_t ChannelDebugValueFlags() noexcept {
  return go2cpp::debug::ResourceValueDigestible<T>
             ? static_cast<std::uint32_t>(
                   go2cpp::debug::kResourceFlagValueKnown)
             : 0U;
}

template <typename T>
struct IsOptional : std::false_type {};

template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

template <typename From, typename To, typename Function>
class FunctionSelectCaster final : public SelectCaster {
 public:
  explicit FunctionSelectCaster(Function&& function)
      : m_function(std::forward<Function>(function)) {}

  SelectValue Cast(const SelectValue& source) const override {
    try {
      const auto* input = source.Get<From>();
      std::optional<From> copied_input;
      if (!input) {
        if constexpr (!std::is_copy_constructible<From>::value) {
          return {};
        } else {
          copied_input = source.As<From>();
          if (!copied_input.has_value()) {
            return {};
          }
          input = &*copied_input;
        }
      }
      using Result = std::invoke_result_t<Function&, const From&>;
      if constexpr (std::is_same<std::decay_t<Result>, SelectValue>::value) {
        return m_function(*input);
      } else if constexpr (IsOptional<std::decay_t<Result>>::value) {
        auto converted = m_function(*input);
        if (!converted.has_value()) {
          return {};
        }
        return SelectValue::From(std::move(*converted));
      } else {
        static_assert(std::is_constructible<To, Result>::value,
                      "SelectCaster result must construct the target type");
        return SelectValue::From(To(m_function(*input)));
      }
    } catch (...) {
      return {};
    }
  }

 private:
  mutable Function m_function;
};

template <typename From, typename To, typename Function>
std::shared_ptr<const SelectCaster> MakeFunctionSelectCaster(
    Function&& function) {
  using FunctionType = std::decay_t<Function>;
  return std::make_shared<FunctionSelectCaster<From, To, FunctionType>>(
      std::forward<Function>(function));
}

}  // namespace detail

template <typename From, typename To, typename Function>
std::shared_ptr<const SelectCaster> SelectCaster::Create(Function&& function) {
  return detail::MakeFunctionSelectCaster<From, To>(
      std::forward<Function>(function));
}

/** @brief Channel 发送操作的状态和错误结果。 */
struct ChannelSendResult {
  ChannelStatus status{ChannelStatus::kInvalid};
  ErrorPtr error;

  bool Ok() const noexcept { return status == ChannelStatus::kReady; }
  bool Succeeded() const noexcept { return Ok(); }
  bool Closed() const noexcept {
    return status == ChannelStatus::kClosed ||
           status == ChannelStatus::kAlreadyClosed;
  }
  bool Cancelled() const noexcept {
    return status == ChannelStatus::kCancelled ||
           status == ChannelStatus::kDeadlineExceeded ||
           status == ChannelStatus::kTimedOut;
  }
  explicit operator bool() const noexcept { return Ok(); }
};

/** @brief Select case 试探结果，包含可选类型擦除值。 */
struct SelectProbe {
  bool ready{false};
  bool ok{false};
  std::any value;
  SelectValue typed_value;
  ChannelStatus status{ChannelStatus::kWouldBlock};
  ErrorPtr error;

  template <typename T>
  bool SetValue(T&& input) noexcept {
    try {
      typed_value = SelectValue::From(std::forward<T>(input));
      try {
        value = typed_value.ToAny();
      } catch (...) {
        value.reset();
      }
      return typed_value.HasValue();
    } catch (...) {
      value.reset();
      typed_value = {};
      return false;
    }
  }

  bool SetAny(std::any input) noexcept {
    try {
      typed_value = SelectValue::FromAny(std::move(input));
      try {
        value = typed_value.ToAny();
      } catch (...) {
        value.reset();
      }
      return typed_value.HasValue();
    } catch (...) {
      value.reset();
      typed_value = {};
      return false;
    }
  }
};

struct SelectCase;

namespace detail {

// 通道值类型来自用户代码，复制或移动时可能抛异常。异常不能穿过等待状态
// 机：调用方收到 kInvalid，注册仍可重试。错误对象构造尽力完成，失败路径
// 不能破坏通道锁不变量。
inline ErrorPtr ValueOperationError() noexcept {
  try {
    return NewError("channel value construction failed");
  } catch (...) {
    return {};
  }
}

inline ContextTimePoint SaturatingDeadline(ContextDuration timeout) noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (timeout > ContextDuration::zero() &&
      now > ContextTimePoint::max() - timeout) {
    return ContextTimePoint::max();
  }
  if (timeout < ContextDuration::zero() &&
      (timeout.count() ==
           std::numeric_limits<ContextDuration::rep>::min() ||
       now < ContextTimePoint::min() - timeout)) {
    return ContextTimePoint::min();
  }
  return now + timeout;
}

// 是否为「纯原生线程」——只有这种上下文可以在 park 前做用户态自旋。
// 受管 Fiber 自旋会占住 carrier M 线程，手动 Fiber 没有 Scheduler 后端；
// 两者的判定需要 Scheduler/Fiber 的完整定义，因此实现放在 channel.cpp。
bool CanSpinWait() noexcept;

// 自适应自旋预算（单位：PAUSE 迭代次数，本机 1 次 ≈ 9ns）。
//
// 「短交接」场景（cap0/cap1 1P1C、request-response）里对端通常只需几微秒
// 就会到达，先自旋一小段就能完全绕开 futex 的睡眠 + 唤醒；而真正会久等的
// 场景必须尽快回到内核睡眠，不能烧 CPU。
//
// 调参不能只看「自旋失败就缩短」：1P1C 场景里等待方第一次自旋时对端往往
// 刚从 futex 醒来（约 4µs），固定的短预算会永远失败并把系统锁在
// 「自旋 2µs + park 4µs」的慢平衡上。因此这里用 park 的实际时长做反馈：
// - park 很短（≤30µs）说明"其实再等一会就来了" → 预算翻倍；
// - park 较长说明对端真的慢 → 预算折半。
// 一旦自旋能覆盖实际交接延迟，后续消息就完全在用户态完成，不再 park。
// 上限取 2048（≈18µs）：再长只会在单核/超订场景放大"占着 CPU 等对端"的
// 最坏情况，对命中率没有明显帮助。
inline int& SpinBudget() noexcept {
  static thread_local int budget = 512;
  return budget;
}

// 根据一次实际 park 的时长调整下一次自旋预算。
inline void ObserveParkDurationNs(long long parked_ns) noexcept {
  int& budget = SpinBudget();
  if (parked_ns <= 30000) {
    budget = budget >= 2048 ? 2048 : budget * 2;
  } else {
    budget = budget <= 128 ? 128 : budget / 2;
  }
}

// 在不持有 Channel::m_mutex 的情况下等待 `signaled` 被 release 置位。
// 返回 true 表示等到；false 表示预算耗尽，调用方应重新加锁并转入 park。
//
// 每 512 次 PAUSE（约 4.5µs）让出一次 CPU：如果对端恰好被调度到同一个核
// （容器 CPU 配额、taskset 单核、宿主超订），纯自旋会把对端饿到时间片耗尽，
// 反而比直接 park 慢一个数量级（实测可到 ~20µs/消息）；yield 让对端先跑，
// 跨核场景则只多出一次很短的调度往返。
inline bool SpinForSignal(const std::atomic<bool>& signaled) noexcept {
  const int limit = SpinBudget();
  for (int i = 0; i < limit; ++i) {
    if (signaled.load(std::memory_order_acquire)) {
      return true;
    }
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    if ((i & 511) == 511) {
      std::this_thread::yield();
    }
  }
  return signaled.load(std::memory_order_acquire);
}

// 一个 Select 调用的所有已挂起 case 共享此状态。通道操作在提交传输前必须
// 先取得它，避免两个就绪通道同时选中同一个调用者。
class SelectWaitState final {
 public:
  bool TrySelect(std::size_t index, SelectProbe probe) {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_selected || m_cancelled) {
        return false;
      }
      m_index = index;
      m_probe = std::move(probe);
      // std::any 赋值可能分配并抛异常；只有 probe 提交成功后才发布 selected。
      m_selected = true;
    }
    m_cv.notify_one();
    return true;
  }

  static bool TrySelectPair(const std::shared_ptr<SelectWaitState>& first,
                            std::size_t first_index,
                            SelectProbe first_probe,
                            const std::shared_ptr<SelectWaitState>& second,
                            std::size_t second_index,
                            SelectProbe second_probe) {
    if (!first || !second || first == second) {
      return false;
    }
    {
      std::scoped_lock lock(first->m_mutex, second->m_mutex);
      if (first->m_selected || first->m_cancelled || second->m_selected ||
          second->m_cancelled) {
        return false;
      }
      first->m_index = first_index;
      first->m_probe = std::move(first_probe);
      second->m_index = second_index;
      second->m_probe = std::move(second_probe);
      first->m_selected = true;
      second->m_selected = true;
    }
    first->m_cv.notify_one();
    second->m_cv.notify_one();
    return true;
  }

  bool IsSelected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_selected;
  }

  bool WaitFor(ContextDuration timeout, const ContextPtr& context = {}) {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, timeout,
                         [this, &context] {
                           return m_selected || m_cancelled ||
                                  (context && context->IsDone());
                         });
  }

  bool Wait(const ContextPtr& context = {}) {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait(lock, [this, &context] {
      return m_selected || m_cancelled || (context && context->IsDone());
    });
  }

  void Notify() { m_cv.notify_one(); }

  // 调用方已经选择取消、超时或其他非通道 case 后，阻止迟到的通道操作
  // 再次认领本次 select。
  bool Cancel() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_selected) {
        return false;
      }
      m_cancelled = true;
    }
    m_cv.notify_one();
    return true;
  }

  bool Take(std::size_t* index, SelectProbe* probe) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_selected || m_taken) {
      return false;
    }
    *index = m_index;
    // std::any 的复制可能失败；typed_value 使用共享载体，可以在这种
    // 情况下仍然把选择结果交给用户的 Caster。
    SelectProbe copy;
    copy.ready = m_probe.ready;
    copy.ok = m_probe.ok;
    copy.status = m_probe.status;
    copy.error = m_probe.error;
    // Take 只允许成功一次，直接移动共享载体，保留 move-only 结果的所有权。
    copy.typed_value = std::move(m_probe.typed_value);
    try {
      copy.value = m_probe.value;
    } catch (...) {
      copy.value.reset();
    }
    *probe = std::move(copy);
    m_taken = true;
    return true;
  }

 private:
  mutable std::mutex m_mutex;
  core::ParkingCondition m_cv;
  bool m_selected{false};
  bool m_cancelled{false};
  bool m_taken{false};
  std::size_t m_index{static_cast<std::size_t>(-1)};
  SelectProbe m_probe;
};

}  // namespace detail


/** @brief Channel 接收操作的值、成功标记和状态。 */
template <typename T>
struct ChannelRecvResult {
  std::optional<T> value;
  bool ok{false};
  bool ready{false};
  ChannelStatus status{ChannelStatus::kInvalid};
  ErrorPtr error;

  bool Ok() const noexcept { return ok && status == ChannelStatus::kReady; }
  bool Closed() const noexcept {
    return ready && status == ChannelStatus::kClosed && !ok;
  }
  bool Cancelled() const noexcept {
    return status == ChannelStatus::kCancelled ||
           status == ChannelStatus::kDeadlineExceeded ||
           status == ChannelStatus::kTimedOut;
  }
  explicit operator bool() const noexcept { return Ok(); }

  // 便于模拟 Go 的 <-ch 赋值：没有值时返回 T 的零值。
  T ValueOrDefault() const {
    return value.has_value() ? *value : T{};
  }
};

/**
 * 支持 Fiber 与普通线程并发的 Go 风格通道。
 *
 * 依赖：Context 负责取消/截止时间，ParkingCondition 负责等待唤醒，
 * SelectWaitState 负责多路 select 的单赢家。对上层提供容量为 0 的无缓冲
 * 通道、容量大于 0 的 FIFO 缓冲通道、Send/Recv/Try/超时/关闭以及方向性
 * 视图。Channel 拥有等待节点和缓冲值，不拥有生产者/消费者任务；对象必须
 * 长于仍在使用它的等待者。T 必须可无异常移动构造和析构，内建 select 对
 * T 还要求可复制；不可复制值可通过 SelectValue 自定义路径承载。
 */
namespace detail {

// 与 Go hchan 的 buf/dataqsiz/sendx/recvx/qcount 对应的环形缓冲。
//
// 依赖：使用未初始化存储 + 就地构造，因此 T 只需要可移动构造与析构；
// capacity == 0 时不分配任何内存（对应 Go 的 dataqsiz == 0，即无缓冲通道）。
// 接口刻意保持 std::deque 的子集，使通道内部的调用点无需改动。
//
// 不变量：m_count == 0 表示空；m_count == m_capacity 表示满；
//         有效元素位于 [m_head, m_head + m_count) 按 m_capacity 取模的区间内。
template <typename T>
class RingBuffer final {
 public:
  RingBuffer() noexcept = default;

  explicit RingBuffer(std::size_t capacity) { Reset(capacity); }

  ~RingBuffer() noexcept { Release(); }

  RingBuffer(const RingBuffer&) = delete;
  RingBuffer& operator=(const RingBuffer&) = delete;

  std::size_t size() const noexcept { return m_count; }
  bool empty() const noexcept { return m_count == 0; }
  std::size_t capacity() const noexcept { return m_capacity; }

  T& front() noexcept { return m_slots[m_head]; }
  const T& front() const noexcept { return m_slots[m_head]; }

  // 调用点保证未满（size() < capacity）。构造失败时不推进计数，
  // 因此不会留下半个元素 —— 与原来 std::deque::emplace_back 的强保证一致。
  template <typename U>
  void emplace_back(U&& value) {
    ::new (static_cast<void*>(m_slots + m_tail)) T(std::forward<U>(value));
    ++m_tail;
    if (m_tail == m_capacity) {
      m_tail = 0;
    }
    ++m_count;
  }

  void pop_front() noexcept {
    m_slots[m_head].~T();
    ++m_head;
    if (m_head == m_capacity) {
      m_head = 0;
    }
    --m_count;
  }

  // 回滚最后一次 emplace_back：对应“先占位、认领失败再撤回”的路径。
  void pop_back() noexcept {
    std::size_t index = m_tail;
    if (index == 0) {
      index = m_capacity - 1;
    } else {
      --index;
    }
    m_slots[index].~T();
    m_tail = index;
    --m_count;
  }

 private:
  void Reset(std::size_t capacity) {
    m_capacity = capacity;
    m_head = 0;
    m_tail = 0;
    m_count = 0;
    if (capacity == 0) {
      return;
    }
    // 只分配原始存储，不构造任何元素。
    m_slots = std::allocator_traits<std::allocator<T>>::allocate(m_allocator,
                                                                capacity);
    m_allocated = true;
  }

  void Release() noexcept {
    while (m_count > 0) {
      pop_front();
    }
    if (m_allocated) {
      std::allocator_traits<std::allocator<T>>::deallocate(m_allocator, m_slots,
                                                          m_capacity);
      m_allocated = false;
    }
    m_slots = nullptr;
  }

  std::allocator<T> m_allocator;
  T* m_slots{nullptr};
  std::size_t m_capacity{0};
  std::size_t m_head{0};
  std::size_t m_tail{0};
  std::size_t m_count{0};
  bool m_allocated{false};
};

}  // namespace detail

/**
 * @brief Channel 的只读调试快照（与元素类型 T 无关，便于 debug 层统一序列化）。
 *
 * ## 语义
 * - `length` 是缓冲区当前元素数（不含正在交接的等待者）；
 * - `send_waiters/recv_waiters` 是已登记等待队列长度；
 * - `select_senders/select_receivers` 是上述队列中由 select 注册的节点数；
 * - `change_waiters` 是当前 park 在状态变化条件变量上的线程/G 数。
 * 全部字段在同一次持锁读取中采集；不改变 Channel 状态。
 */
struct ChannelDebugSnapshot {
    std::size_t capacity{0};
    std::size_t length{0};
    bool closed{false};
    std::size_t send_waiters{0};
    std::size_t recv_waiters{0};
    std::size_t select_senders{0};
    std::size_t select_receivers{0};
    std::size_t change_waiters{0};
    std::uint64_t generation{0};
};

/** * @brief 支持 Fiber 与线程并发的 FIFO Channel。
 * @details capacity=0 时为无缓冲 rendezvous，正值时使用有界 FIFO 缓冲；
 *          关闭会唤醒所有发送者和接收者，Select 通过内部等待节点接入。
 */
template <typename T>
class Channel final : public std::enable_shared_from_this<Channel<T>>
#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
    ,
    public ChannelMigrationBase
#endif
{
  static_assert(std::is_nothrow_move_constructible<T>::value,
                "Channel<T> requires a nothrow move constructor");
  static_assert(std::is_nothrow_destructible<T>::value,
                "Channel<T> requires a nothrow destructor");

 public:
  using Ptr = std::shared_ptr<Channel<T>>;
  using Duration = ContextDuration;
  using TimePoint = ContextTimePoint;

  // 创建容量为 capacity 的通道；capacity==0 表示无缓冲。
  /** @brief 创建指定容量的 Channel。@param capacity 0 表示无缓冲。 */
  static Ptr Create(std::size_t capacity = 0) {
    return std::make_shared<Channel<T>>(capacity);
  }

  explicit Channel(std::size_t capacity = 0)
      : m_capacity(capacity), m_buffer(capacity) {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    m_debug_resource = GO2CPP_DEBUG_RESOURCE_REGISTER(kChannel, this, nullptr);
#endif
  }
  ~Channel() noexcept {
    (void)Close();
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    GO2CPP_DEBUG_RESOURCE_UNREGISTER(m_debug_resource);
#endif
  }

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  // 返回固定容量（不含当前缓冲长度）。
  /** @brief 返回 Channel 容量。 */
  std::size_t Capacity() const noexcept { return m_capacity; }
  // 返回当前缓冲区长度；不包含正在交接的等待者。
  /** @brief 返回当前缓冲区内元素数量。 */
  std::size_t Len() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffer.size();
  }
  // 查询通道是否已关闭。
  /** @brief 返回 Channel 是否已关闭。 */
  bool IsClosed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
  }
  bool closed() const { return IsClosed(); }

  /**
   * @brief 采集通道的只读调试快照（缓冲长度/等待者/select 注册数）。
   * @note 一次持锁读取；与 Send/Recv/Close 并发时结果只代表某一瞬间。
   */
  ChannelDebugSnapshot DebugSnapshot() const {
    ChannelDebugSnapshot snapshot;
    std::lock_guard<std::mutex> lock(m_mutex);
    snapshot.capacity = m_capacity;
    snapshot.length = m_buffer.size();
    snapshot.closed = m_closed;
    snapshot.send_waiters = m_senders.size();
    snapshot.recv_waiters = m_receivers.size();
    for (const auto& node : m_senders) {
      if (node != nullptr && node->select_state) {
        ++snapshot.select_senders;
      }
    }
    for (const auto& node : m_receivers) {
      if (node != nullptr && node->select_state) {
        ++snapshot.select_receivers;
      }
    }
    snapshot.change_waiters =
        m_change_waiters.load(std::memory_order_relaxed);
    snapshot.generation = m_generation.load(std::memory_order_relaxed);
    return snapshot;
  }

  // 发送 value；阻塞直到接收者/缓冲可用、关闭或 context 取消。
  ChannelSendResult Send(T value, const ContextPtr& context = {}) {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    const std::uint64_t debug_value = detail::ChannelDebugValue(value);
#endif
    try {
      auto result = SendUntil(std::move(value), std::nullopt, context, false);
      if (result.status == ChannelStatus::kReady) {
        GO2CPP_DEBUG_EVENT_SAMPLED(
            kChannelSendOk, GO2CPP_DEBUG_BOUND_TASK(), m_capacity, 32);
      }
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
      GO2CPP_DEBUG_RESOURCE_EVENT_EX(
          kChannelSend, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0,
          debug_value, detail::ChannelDebugResult(result.status), 0, false,
          detail::ChannelDebugValueFlags<T>());
#endif
      return result;
    } catch (...) {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
      GO2CPP_DEBUG_RESOURCE_EVENT_EX(
          kChannelSend, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0,
          debug_value, go2cpp::debug::ResourceResult::kFailed, 0, false,
          detail::ChannelDebugValueFlags<T>());
#endif
      return {ChannelStatus::kInvalid, detail::ValueOperationError()};
    }
  }

  // 最多等待 timeout 发送；返回 ChannelSendResult 状态和错误。
  ChannelSendResult SendFor(T value, Duration timeout,
                            const ContextPtr& context = {}) {
    try {
      if (timeout < Duration::zero()) {
        timeout = Duration::zero();
      }
      return SendUntil(std::move(value), detail::SaturatingDeadline(timeout),
                       context, false);
    } catch (...) {
      return {ChannelStatus::kInvalid, detail::ValueOperationError()};
    }
  }

  // 非阻塞发送；当前不能完成时返回 kWouldBlock。
  ChannelSendResult TrySend(T value) {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    const std::uint64_t debug_value = detail::ChannelDebugValue(value);
#endif
    try {
      auto result =
          SendUntil(std::move(value), std::chrono::steady_clock::now(), {}, true);
      if (result.status == ChannelStatus::kReady) {
        GO2CPP_DEBUG_EVENT_SAMPLED(
            kChannelSendOk, GO2CPP_DEBUG_BOUND_TASK(), m_capacity, 32);
      }
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
      GO2CPP_DEBUG_RESOURCE_EVENT_EX(
          kChannelSend, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0,
          debug_value, detail::ChannelDebugResult(result.status), 0, false,
          detail::ChannelDebugValueFlags<T>() |
              static_cast<std::uint32_t>(
                  go2cpp::debug::kResourceFlagTry));
#endif
      return result;
    } catch (...) {
      return {ChannelStatus::kInvalid, detail::ValueOperationError()};
    }
  }

  // 接收一个值；关闭后返回 kClosed 与零值/空 optional。
  ChannelRecvResult<T> Recv(const ContextPtr& context = {}) {
    try {
      auto result = RecvUntil(std::nullopt, context, false);
      if (result.status == ChannelStatus::kReady) {
        GO2CPP_DEBUG_EVENT_SAMPLED(
            kChannelRecvOk, GO2CPP_DEBUG_BOUND_TASK(), m_capacity, 32);
      }
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
      std::uint64_t debug_value = 0;
      if (result.value.has_value()) {
        debug_value = detail::ChannelDebugValue(*result.value);
      }
      GO2CPP_DEBUG_RESOURCE_EVENT_EX(
          kChannelRecv, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0,
          debug_value, detail::ChannelDebugResult(result.status), 0, false,
          detail::ChannelDebugValueFlags<T>());
#endif
      return result;
    } catch (...) {
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }
  }

  // 最多等待 timeout 接收；超时不改变通道状态。
  ChannelRecvResult<T> RecvFor(Duration timeout, const ContextPtr& context = {}) {
    try {
      if (timeout < Duration::zero()) {
        timeout = Duration::zero();
      }
      return RecvUntil(detail::SaturatingDeadline(timeout), context, false);
    } catch (...) {
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }
  }

  // 非阻塞接收；当前没有值且未关闭时返回 kWouldBlock。
  ChannelRecvResult<T> TryRecv() {
    try {
      auto result = RecvUntil(std::chrono::steady_clock::now(), {}, true);
      if (result.status == ChannelStatus::kReady) {
        GO2CPP_DEBUG_EVENT_SAMPLED(
            kChannelRecvOk, GO2CPP_DEBUG_BOUND_TASK(), m_capacity, 32);
      }
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
      std::uint64_t debug_value = 0;
      if (result.value.has_value()) {
        debug_value = detail::ChannelDebugValue(*result.value);
      }
      GO2CPP_DEBUG_RESOURCE_EVENT_EX(
          kChannelRecv, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0,
          debug_value, detail::ChannelDebugResult(result.status), 0, false,
          detail::ChannelDebugValueFlags<T>() |
              static_cast<std::uint32_t>(
                  go2cpp::debug::kResourceFlagTry));
#endif
      return result;
    } catch (...) {
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }
  }

  // 关闭操作幂等并唤醒所有阻塞操作。重复关闭返回 kAlreadyClosed，不抛
  // C++ 异常；关闭后发送返回关闭状态，接收在排空缓冲后返回零值。
  ChannelSendResult Close() noexcept {
    std::deque<std::shared_ptr<PendingSend>> senders;
    std::deque<std::shared_ptr<PendingRecv>> receivers;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_closed) {
        return {ChannelStatus::kAlreadyClosed, ChannelAlreadyClosedError()};
      }
      m_closed = true;
      ++m_generation;
      senders.swap(m_senders);
      receivers.swap(m_receivers);
      for (auto& sender : senders) {
        if (sender->select_state) {
          SelectProbe probe;
          probe.ready = true;
          probe.status = ChannelStatus::kClosed;
          probe.error = ChannelClosedError();
          try {
            (void)sender->select_state->TrySelect(sender->select_index,
                                                  std::move(probe));
          } catch (...) {
            // Close 仍须唤醒该 select；选择结果不可提交时，把节点
            // 取消掉，避免异常穿过 Close 并留下永远等待的 Fiber。
            sender->select_state->Cancel();
          }
          sender->cancelled = true;
          continue;
        }
        sender->closed = true;
        sender->status = ChannelStatus::kClosed;
        sender->error = ChannelClosedError();
      }
      for (auto& receiver : receivers) {
        if (receiver->select_state) {
          SelectProbe probe;
          probe.ready = true;
          probe.status = ChannelStatus::kClosed;
          probe.error = ChannelClosedError();
          try {
            (void)receiver->select_state->TrySelect(receiver->select_index,
                                                    std::move(probe));
          } catch (...) {
            receiver->select_state->Cancel();
          }
          receiver->cancelled = true;
          continue;
        }
        receiver->ready = true;
        receiver->ok = false;
        receiver->status = ChannelStatus::kClosed;
        receiver->error = ChannelClosedError();
      }
    }
    for (auto& sender : senders) {
      sender->Signal();
      sender->NotifyIfParked();
    }
    for (auto& receiver : receivers) {
      receiver->Signal();
      receiver->NotifyIfParked();
    }
    NotifyChange();
    return {ChannelStatus::kReady, {}};
  }

  /**
   * @brief Doctor 专用：对阻塞在 Channel 上的等待者发一次 **spurious wake**。
   *
   * ## 语义（为什么安全）
   *
   * 本方法**不写任何结果标志、不入队/出队、不递增 generation、不关闭
   * channel**，只调用等待节点自己的 `ParkingCondition::notify_one()`：
   *
   * - `SendUntil`/`RecvUntil` 的等待循环被唤醒后会重检
   *   `accepted/closed/cancelled/context/deadline`，仍不满足就继续等；
   * - select 节点由 `SelectWaitState` 管理，不在此处唤醒（它们不是
   *   ParkingCondition 等待者），避免伪造 select 就绪；
   * - `WaitForChange` 观察者只重检 generation，不递增就不会被误判为变化。
   *
   * 因此它是「唤醒后重新检查条件」的可安全重试操作，不会改变 Channel
   * 的可见行为；用于诊断线程在全局卡死时踢一下等待者（例如确认它们确实
   * 只是等待，而不是唤醒令牌丢失）。
   *
   * @return 本次尝试唤醒的普通等待节点数（不含 select 节点与 change 观察者）。
   */
  std::size_t ForceWakeWaiters() {
    std::vector<std::shared_ptr<PendingSend>> senders;
    std::vector<std::shared_ptr<PendingRecv>> receivers;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      senders.assign(m_senders.begin(), m_senders.end());
      receivers.assign(m_receivers.begin(), m_receivers.end());
    }
    std::size_t woken = 0;
    for (auto& sender : senders) {
      if (!sender || sender->select_state) {
        continue;
      }
      // spin_wait 节点在「已发布但还没真正 park」的窗口里只认 signaled；
      // 置位让它提前结束自旋并重新加锁重检（重检后仍不满足会继续等）。
      if (sender->spin_wait) {
        sender->signaled.store(true, std::memory_order_release);
      }
      sender->NotifyIfParked();
      ++woken;
    }
    for (auto& receiver : receivers) {
      if (!receiver || receiver->select_state) {
        continue;
      }
      if (receiver->spin_wait) {
        receiver->signaled.store(true, std::memory_order_release);
      }
      receiver->NotifyIfParked();
      ++woken;
    }
    if (m_change_waiters.load(std::memory_order_acquire) != 0) {
      m_change_cv.notify_all();
    }
    return woken;
  }

  // 返回单调递增代际；每次发送、接收或关闭改变可用性时递增，供外部
  // select 实现判断是否需要重新探测。
  std::uint64_t Generation() const noexcept {
    return m_generation.load(std::memory_order_acquire);
  }

  // 等待 m_generation 变化。观察者计数用于让热路径的 send/recv 在
  // 没有观察者时跳过 ParkingCondition 的互斥量：等待者持 m_mutex 先
  // 递增计数，再检查 generation；通知者持 m_mutex 先递增 generation，
  // 解锁后读计数。两者由同一把锁排序，因此不会漏掉已注册的观察者，
  // 也不会把 generation 变化漏给已经在等待的调用者。
  bool WaitForChange(std::uint64_t generation, Duration timeout) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_change_waiters.fetch_add(1, std::memory_order_acq_rel);
    const bool changed = m_change_cv.wait_for(lock, timeout, [this, generation] {
      return m_generation.load(std::memory_order_relaxed) != generation;
    });
    m_change_waiters.fetch_sub(1, std::memory_order_acq_rel);
    return changed;
  }

  // 注册 select 接收 case。持有通道锁时再次检查就绪，避免发送者在探测
  // 与发布等待节点之间插入；调用方选择其他 case 或超时/取消获胜后，
  // DisarmSelect 会移除节点。
  bool ArmSelectRecv(const std::shared_ptr<detail::SelectWaitState>& state,
                     std::size_t index) {
    if (!state || state->IsSelected()) {
      return false;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    if (state->IsSelected()) {
      return false;
    }

    if (!m_buffer.empty()) {
      if constexpr (!std::is_copy_constructible<T>::value) {
        return false;
      } else {
        SelectProbe probe;
        probe.ready = true;
        probe.ok = true;
        probe.status = ChannelStatus::kReady;
        if (!probe.SetValue(m_buffer.front())) {
          return false;
        }
        if (!state->TrySelect(index, std::move(probe))) {
          return false;
        }
        m_buffer.pop_front();
        RefillBufferLocked();
        ++m_generation;
        lock.unlock();
        NotifyChange();
        return true;
      }
    }

    const std::size_t sender_scan_limit = m_senders.size();
    for (std::size_t scanned = 0;
         scanned < sender_scan_limit && !m_senders.empty(); ++scanned) {
      auto sender = m_senders.front();
      if (sender->select_state == state) {
        m_senders.pop_front();
        m_senders.emplace_back(std::move(sender));
        continue;
      }
      if (sender->cancelled) {
        m_senders.pop_front();
        continue;
      }
      if (sender->select_state) {
        if constexpr (!std::is_copy_constructible<T>::value) {
          sender->cancelled = true;
          m_senders.pop_front();
          continue;
        } else {
          SelectProbe recv_probe;
          recv_probe.ready = true;
          recv_probe.ok = true;
          recv_probe.status = ChannelStatus::kReady;
          if (!recv_probe.SetValue(sender->value)) {
            return false;
          }
          SelectProbe send_probe;
          send_probe.ready = true;
          send_probe.ok = true;
          send_probe.status = ChannelStatus::kReady;
          if (!detail::SelectWaitState::TrySelectPair(
                  state, index, std::move(recv_probe), sender->select_state,
                  sender->select_index, std::move(send_probe))) {
            if (state->IsSelected()) {
              return false;
            }
            if (sender->select_state->IsSelected()) {
              sender->cancelled = true;
              m_senders.pop_front();
              continue;
            }
            continue;
          }
          m_senders.pop_front();
          ++m_generation;
          lock.unlock();
          NotifyChange();
          return true;
        }
      }
      if constexpr (!std::is_copy_constructible<T>::value) {
        return false;
      } else {
        SelectProbe probe;
        probe.ready = true;
        probe.ok = true;
        probe.status = ChannelStatus::kReady;
        if (!probe.SetValue(sender->value)) {
          return false;
        }
        if (!state->TrySelect(index, std::move(probe))) {
          return false;
        }
        m_senders.pop_front();
        sender->accepted = true;
        sender->status = ChannelStatus::kReady;
        sender->Signal();
        sender->NotifyIfParked();
        ++m_generation;
        lock.unlock();
        NotifyChange();
        return true;
      }
    }

    if (m_closed) {
      SelectProbe probe;
      probe.ready = true;
      probe.status = ChannelStatus::kClosed;
      probe.error = ChannelClosedError();
      const bool selected = state->TrySelect(index, std::move(probe));
      lock.unlock();
      if (selected) {
        NotifyChange();
      }
      return selected;
    }

    auto pending = std::make_shared<PendingRecv>();
    pending->select_state = state;
    pending->select_index = index;
    m_receivers.emplace_back(std::move(pending));
    ++m_generation;
    lock.unlock();
    NotifyChange();
    return true;
  }

  // 注册 select 发送 case。SendCase 按约定要求值可复制，使 arm 失败时值
  // 仍保留到其他 case 被选中。
  bool ArmSelectSend(const std::shared_ptr<detail::SelectWaitState>& state,
                     std::size_t index, const T& value) {
    if (!state || state->IsSelected()) {
      return false;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    if (state->IsSelected()) {
      return false;
    }

    const std::size_t receiver_scan_limit = m_receivers.size();
    for (std::size_t scanned = 0;
         scanned < receiver_scan_limit && !m_receivers.empty(); ++scanned) {
      auto receiver = m_receivers.front();
      if (receiver->select_state == state) {
        m_receivers.pop_front();
        m_receivers.emplace_back(std::move(receiver));
        continue;
      }
      if (receiver->cancelled) {
        m_receivers.pop_front();
        continue;
      }
      SelectProbe send_probe;
      send_probe.ready = true;
      send_probe.ok = true;
      send_probe.status = ChannelStatus::kReady;
      if (receiver->select_state) {
        if constexpr (!std::is_copy_constructible<T>::value) {
          receiver->cancelled = true;
          m_receivers.pop_front();
          continue;
        } else {
          SelectProbe recv_probe;
          recv_probe.ready = true;
          recv_probe.ok = true;
          recv_probe.status = ChannelStatus::kReady;
          try {
            if (!recv_probe.SetValue(value)) {
              return false;
            }
          } catch (...) {
            return false;
          }
          if (!detail::SelectWaitState::TrySelectPair(
                  state, index, std::move(send_probe),
                  receiver->select_state, receiver->select_index,
                  std::move(recv_probe))) {
            if (state->IsSelected()) {
              return false;
            }
            if (receiver->select_state->IsSelected()) {
              receiver->cancelled = true;
              m_receivers.pop_front();
              continue;
            }
            continue;
          }
          m_receivers.pop_front();
          ++m_generation;
          lock.unlock();
          NotifyChange();
          return true;
        }
      }
      // 在发布 select 之前先构造接收值。用户类型抛异常时，双方都不会
      // 被选中，等待节点仍可由后续发送者重试。
      try {
        receiver->value.emplace(value);
      } catch (...) {
        return false;
      }
      if (!state->TrySelect(index, std::move(send_probe))) {
        receiver->value.reset();
        return false;
      }
      receiver->ok = true;
      receiver->ready = true;
      receiver->status = ChannelStatus::kReady;
      m_receivers.pop_front();
      receiver->Signal();
      receiver->NotifyIfParked();
      ++m_generation;
      lock.unlock();
      NotifyChange();
      return true;
    }

    if (m_closed) {
      SelectProbe probe;
      probe.ready = true;
      probe.status = ChannelStatus::kClosed;
      probe.error = ChannelClosedError();
      const bool selected = state->TrySelect(index, std::move(probe));
      lock.unlock();
      if (selected) {
        NotifyChange();
      }
      return selected;
    }

    if (m_buffer.size() < m_capacity) {
      SelectProbe probe;
      probe.ready = true;
      probe.ok = true;
      probe.status = ChannelStatus::kReady;
      try {
        m_buffer.emplace_back(value);
      } catch (...) {
        return false;
      }
      bool selected = false;
      try {
        selected = state->TrySelect(index, std::move(probe));
      } catch (...) {
        m_buffer.pop_back();
        return false;
      }
      if (!selected) {
        m_buffer.pop_back();
        return false;
      }
      ++m_generation;
      lock.unlock();
      NotifyChange();
      return true;
    }

    std::shared_ptr<PendingSend> pending;
    try {
      pending = std::make_shared<PendingSend>(value);
    } catch (...) {
      return false;
    }
    pending->select_state = state;
    pending->select_index = index;
    m_senders.emplace_back(std::move(pending));
    ++m_generation;
    lock.unlock();
    NotifyChange();
    return true;
  }

  void DisarmSelect(const std::shared_ptr<detail::SelectWaitState>& state) {
    if (!state) {
      return;
    }
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      for (auto it = m_senders.begin(); it != m_senders.end();) {
        if ((*it)->select_state == state) {
          (*it)->cancelled = true;
          it = m_senders.erase(it);
          changed = true;
        } else {
          ++it;
        }
      }
      for (auto it = m_receivers.begin(); it != m_receivers.end();) {
        if ((*it)->select_state == state) {
          (*it)->cancelled = true;
          it = m_receivers.erase(it);
          changed = true;
        } else {
          ++it;
        }
      }
      if (changed) {
        ++m_generation;
      }
    }
    if (changed) {
      NotifyChange();
    }
  }

 private:
  /**
   * @brief 一个被登记的发送者（无缓冲、或缓冲已满时挂在 `m_senders`）。
   *
   * ## 定位
   *
   * `SendUntil` 在无法立即交接时创建它并 park；接收方 `Recv` 从 `m_senders` 队头
   * 取走它完成 rendezvous（**绕过缓冲**），或 `Close` 遍历它置 `closed`。
   *
   * ## 字段的两组职责（不要混）
   *
   * | 组 | 字段 | 用途 |
   * |---|---|---|
   * | 交接结果 | `accepted`/`closed`/`cancelled`/`status`/`error` | 发送方醒来后据此构造 `ChannelSendResult` |
   * | select 归属 | `select_state`/`select_index` | 属于哪个 select、第几个 case（普通 Send 为 null / 最大值） |
   * | 等待载体 | `cv` | 发送者在此 park，等接收者取走值 |
   *
   * ## 不变量
   *
   * - 三个布尔位（`accepted`/`closed`/`cancelled`）**至多一个为真**：它们由
   *   `Unlock`/`Close`/取消回调三个源在 `m_mutex` 内竞争写入；
   * - `accepted == true` 时 `value` 已被移走（接收者持有），不要再读。
   */
  struct PendingSend {
    explicit PendingSend(T item) : value(std::move(item)) {}
    /// @brief 待发送的值；被接收者 `std::move` 取走后不要读。
    T value;
    /// @brief 是否已成功交接给接收者（与 `closed`/`cancelled` 互斥）。
    bool accepted{false};
    /// @brief 是否因 channel 关闭而失败（对应 `kClosed`/`kAlreadyClosed`）。
    bool closed{false};
    bool cancelled{false};
    ChannelStatus status{ChannelStatus::kWouldBlock};
    ErrorPtr error;
    std::shared_ptr<detail::SelectWaitState> select_state;
    std::size_t select_index{static_cast<std::size_t>(-1)};
    core::ParkingCondition cv;
    /// @brief 交接/关闭结果的原子发布位，供等待方在 park 前短自旋观察。
    ///        结果标志仍由 Channel::m_mutex 保护；这里只负责"可以醒了"的
    ///        release/acquire 握手，避免自旋方永久错过一次交接。
    /// @brief 该等待者是否在 park 前自旋（仅 capacity<=1 时可能为 true）。
    ///        由等待方在入队前写入；完成方在 m_mutex 内读取。cap>1 的等待者
    ///        从不自旋，完成方因此可以完全跳过 signaled/parked 的原子维护。
    bool spin_wait{false};
    std::atomic<bool> signaled{false};
    /// @brief 等待方是否已经真正进入 ParkingCondition（持 m_mutex 写）。
    ///        对端据此决定要不要 notify：仍在自旋的等待者会自己看到
    ///        `signaled`，跳过 notify 可省掉一次 ParkingCondition 内部锁；
    ///        Close 一次唤醒大量等待者时也能跳过已经醒了的节点。
    std::atomic<bool> parked{false};
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    /// @brief 所属 channel 的调试资源编号与等待者 gid（仅资源事件用）。
    std::uint64_t debug_channel{0};
    std::uint64_t debug_waiter_gid{0};
#endif

    /// @brief 在持 m_mutex 并写完结果标志后调用（只对自旋通道需要）。
    void Signal() noexcept {
      if (spin_wait) {
        signaled.store(true, std::memory_order_release);
      }
    }

    /// @brief 只有真正 park 的等待者才需要 ParkingCondition 唤醒。
    ///        自旋中的等待者会在 `signaled` 上看到结果；跳过 notify 可省掉
    ///        ParkingCondition 内部互斥量，也避免 Close 对已醒节点做无效唤醒。
    void NotifyIfParked() noexcept {
      if (!spin_wait || parked.load(std::memory_order_relaxed)) {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_DEBUG_RESOURCE_EVENT(
            kChannelWake, debug_channel, GO2CPP_DEBUG_BOUND_TASK(),
            debug_waiter_gid, 0, go2cpp::debug::ResourceResult::kWake, 0,
            true);
#else
        GO2CPP_DEBUG_EVENT(kChannelWake, 0, 1);
#endif
        // 外层优先：ParkingCondition::notify_one 内部的 kPCNotify 不会把
        // 这次唤醒的语义改写成 kParkingCondition。
        GO2CPP_DEBUG_WAKE_HINT(kChannelSend);
        cv.notify_one();
      }
    }
  };

  /**
   * @brief 一个被登记的接收者（无缓冲、或缓冲已空时挂在 `m_receivers`）。
   *
   * ## 定位
   *
   * `RecvUntil` 在无法立即取到值时创建它并 park；发送方 `Send` 直接把值塞进它的
   * `value` 并置 `ready`（**绕过缓冲**），或 `Close` 遍历它置 `cancelled`。
   *
   * ## 与 `PendingSend` 的对称性
   *
   * 两者字段一一对应（`accepted` ↔ `ready`、`closed` ↔ `cancelled`），但含义相反：
   * 发送方关心「值有没有被取走」，接收方关心「有没有拿到值」。
   *
   * ## 不变量
   *
   * - `ready && ok` 时 `value` 必有值；`!ok` 时 `value` 为空；
   * - `cancelled == true` 时不会再被任何发送方写入（`m_mutex` 内竞争）。
   */
  struct PendingRecv {
    /// @brief 接收到的值；`ready == false` 时为空。
    std::optional<T> value;
    /// @brief 是否已有值可取（与 `cancelled` 互斥）。
    bool ready{false};
    bool ok{false};
    bool cancelled{false};
    ChannelStatus status{ChannelStatus::kWouldBlock};
    ErrorPtr error;
    std::shared_ptr<detail::SelectWaitState> select_state;
    std::size_t select_index{static_cast<std::size_t>(-1)};
    core::ParkingCondition cv;
    /// @brief 交接/关闭结果的原子发布位；语义见 PendingSend::signaled。
    /// @brief 该等待者是否在 park 前自旋（仅 capacity<=1 时可能为 true）。
    ///        由等待方在入队前写入；完成方在 m_mutex 内读取。cap>1 的等待者
    ///        从不自旋，完成方因此可以完全跳过 signaled/parked 的原子维护。
    bool spin_wait{false};
    std::atomic<bool> signaled{false};
    /// @brief 等待方是否已经真正进入 ParkingCondition（持 m_mutex 写）。
    ///        对端据此决定要不要 notify：仍在自旋的等待者会自己看到
    ///        `signaled`，跳过 notify 可省掉一次 ParkingCondition 内部锁；
    ///        Close 一次唤醒大量等待者时也能跳过已经醒了的节点。
    std::atomic<bool> parked{false};
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    /// @brief 所属 channel 的调试资源编号与等待者 gid（仅资源事件用）。
    std::uint64_t debug_channel{0};
    std::uint64_t debug_waiter_gid{0};
#endif

    /// @brief 在持 m_mutex 并写完结果标志后调用（只对自旋通道需要）。
    void Signal() noexcept {
      if (spin_wait) {
        signaled.store(true, std::memory_order_release);
      }
    }

    /// @brief 只有真正 park 的等待者才需要 ParkingCondition 唤醒。
    ///        自旋中的等待者会在 `signaled` 上看到结果；跳过 notify 可省掉
    ///        ParkingCondition 内部互斥量，也避免 Close 对已醒节点做无效唤醒。
    void NotifyIfParked() noexcept {
      if (!spin_wait || parked.load(std::memory_order_relaxed)) {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_DEBUG_RESOURCE_EVENT(
            kChannelWake, debug_channel, GO2CPP_DEBUG_BOUND_TASK(),
            debug_waiter_gid, 0, go2cpp::debug::ResourceResult::kWake, 0,
            true);
#else
        GO2CPP_DEBUG_EVENT(kChannelWake, 0, 2);
#endif
        GO2CPP_DEBUG_WAKE_HINT(kChannelRecv);
        cv.notify_one();
      }
    }
  };

#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
 public:
  // -------------------------------------------------------------------------
  // 跨进程迁移：等待状态导出 / 接纳（最小子集）
  //
  // 支持范围：无缓冲通道、单生产者单消费者、无 select、无 deadline/context。
  // 迁移等待路径**不**进入 ParkingCondition：它把等待节点挂在队列上之后直接
  // park 当前 Fiber。这样挂起栈帧里没有 WaitNode/Scheduler/堆控制块等跨进程
  // 无效引用；恢复由宿主 resume，唤醒方不需要 Scheduler。
  // -------------------------------------------------------------------------

  /**
   * @brief 导出当前唯一的等待者（仅接收方向）。
   * @details 复杂场景（有缓冲、多个等待者、select、已关闭）明确拒绝，
   *          不做语义等价的近似导出。
   */
  bool ExportWaitState(ChannelWaitState* out, std::string* error) override {
    if (out == nullptr) {
      if (error != nullptr) *error = "ChannelWaitState 输出为空";
      return false;
    }
    *out = ChannelWaitState{};
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_capacity != 0 || !m_buffer.empty()) {
      if (error != nullptr) {
        *error = "迁移只支持无缓冲通道（capacity=" +
                 std::to_string(m_capacity) + "）";
      }
      return false;
    }
    if (!m_senders.empty()) {
      if (error != nullptr) *error = "通道上存在等待发送者，超出单生产者单消费者子集";
      return false;
    }
    if (m_receivers.size() != 1) {
      if (error != nullptr) {
        *error = "通道上必须恰好有一个接收等待者（实际 " +
                 std::to_string(m_receivers.size()) + "）";
      }
      return false;
    }
    if (m_closed) {
      if (error != nullptr) *error = "已关闭的通道不能导出等待状态";
      return false;
    }
    const auto& node = m_receivers.front();
    if (!node || node->select_state) {
      if (error != nullptr) *error = "select 等待者不在迁移子集内";
      return false;
    }
    if (node->ready || node->cancelled) {
      if (error != nullptr) *error = "等待节点已经就绪，导出没有意义";
      return false;
    }
    // std::shared_ptr 是「对象指针 + 控制块指针」两个机器字；迁移必须同时
    // 重建两者，否则恢复侧析构会访问旧进程的引用计数。
    static_assert(sizeof(node) == 2U * sizeof(std::uint64_t),
                  "std::shared_ptr 必须是两机器字布局");
    std::uint64_t words[2] = {0, 0};
    std::memcpy(words, &node, sizeof(words));
    out->kind = 1;
    out->channel_address = reinterpret_cast<std::uint64_t>(this);
    out->node_address = words[0];
    out->control_block = words[1];
    out->value_bytes = sizeof(T);
    out->has_value = node->ready ? 1U : 0U;
    return true;
  }

  /**
   * @brief 把导入的接收等待者挂回队列。
   * @details `state.pending_slot` 必须是**新栈**上的 shared_ptr 槽：就地
   *          构造会接管一份真实引用，避免发送方取走队列引用后节点被提前
   *          销毁。旧栈槽里的字节来自生成进程，不能调用其析构（控制块在
   *          旧进程），只能直接覆盖。
   */
  bool AdoptWaitState(const ChannelWaitState& state,
                      ChannelWaitState* adopted,
                      std::string* error) override {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (state.kind != 1) {
      if (error != nullptr) *error = "迁移只支持接收等待（kind=1）";
      return false;
    }
    if (state.value_bytes != sizeof(T)) {
      if (error != nullptr) {
        *error = "等待值类型大小不匹配（快照 " +
                 std::to_string(state.value_bytes) + "，当前 " +
                 std::to_string(sizeof(T)) + "）";
      }
      return false;
    }
    if (m_capacity != 0 || !m_buffer.empty() || !m_senders.empty() ||
        !m_receivers.empty() || m_closed) {
      if (error != nullptr) *error = "目标通道必须为空的无缓冲通道";
      return false;
    }
    auto node = std::make_shared<PendingRecv>();
    node->spin_wait = false;
    m_receivers.emplace_back(node);
    ++m_generation;
    if (state.pending_slot != 0) {
      auto* slot = reinterpret_cast<std::shared_ptr<PendingRecv>*>(
          static_cast<std::uintptr_t>(state.pending_slot));
      new (slot) std::shared_ptr<PendingRecv>(node);
    }
    // 额外保持一份引用，直到导入器完成栈槽构造（ReleaseMigrationHold）。
    m_migration_hold = node;
    if (adopted != nullptr) {
      *adopted = ChannelWaitState{};
      adopted->kind = 1;
      adopted->channel_address = reinterpret_cast<std::uint64_t>(this);
      std::uint64_t words[2] = {0, 0};
      std::memcpy(words, &node, sizeof(words));
      adopted->node_address = words[0];
      adopted->control_block = words[1];
      adopted->pending_slot = state.pending_slot;
      adopted->value_bytes = sizeof(T);
    }
    return true;
  }

  /** @brief 通道对象（`Channel<T>` 本体）的起始地址。 */
  std::uint64_t MigrationObjectAddress() const noexcept override {
    return reinterpret_cast<std::uint64_t>(this);
  }

  /** @brief 通道对象大小（句柄重定位的内部偏移上界）。 */
  std::uint64_t MigrationObjectSize() const noexcept override {
    return sizeof(Channel<T>);
  }

  /** @brief 等待节点大小（句柄重定位的内部偏移上界）。 */
  std::uint64_t MigrationNodeSize() const noexcept override {
    return sizeof(PendingRecv);
  }

  /** @brief 释放 Adopt 期间额外持有的节点引用。 */
  void ReleaseMigrationHold() noexcept override { m_migration_hold.reset(); }

  /**
   * @brief 判断当前等待是否落在迁移子集（调用方必须已持有 m_mutex）。
   * @note 只做状态检查，不修改通道。
   */
  bool CanAdoptMigrationWait(std::optional<TimePoint> deadline,
                             const ContextPtr& context) const noexcept {
    if (deadline.has_value() || context) return false;
    if (m_capacity != 0 || m_closed) return false;
    if (!m_buffer.empty() || !m_senders.empty() || !m_receivers.empty()) {
      return false;
    }
    return true;
  }

  /**
   * @brief 创建迁移等待节点（noinline：把分配器临时留在独立栈帧里）。
   *
   * @details make_shared 与 deque 入队会在调用者栈上留下容器内部指针
   * （`_M_node` 等）。这些是死值，但导出侧的指针审计无法证明它们已失效，
   * 会把它们判成未登记堆指针。把这两步放进 noinline 辅助函数后，它们
   * 的栈帧在返回后会被随后的 `Fiber::Suspend` 调用帧覆盖，审计面对的就
   * 只剩真正活跃的指针。
   */
  [[gnu::noinline]] static std::shared_ptr<PendingRecv>
  MakeMigrationWaitNode() {
    auto node = std::make_shared<PendingRecv>();
    node->spin_wait = false;
    return node;
  }

  /// @brief 把迁移等待节点挂进接收队列（noinline，原因见上）。
  [[gnu::noinline]] void EnqueueMigrationWaitNode(
      const std::shared_ptr<PendingRecv>& node) {
    // push_back 返回 void；emplace_back 在 C++17 返回元素引用，内联后会在
    // 调用者栈上留下 deque block 内的元素地址（堆指针），导出审计会把它
    // 误判成未登记堆指针。
    m_receivers.push_back(node);
    ++m_generation;
  }

  /**
   * @brief 迁移子集的阻塞接收：登记节点后直接 park 当前 Fiber。
   *
   * @details 调用方必须持有 `lock`（指向 m_mutex）。等待期间释放锁；
   * 返回时重新持有。恢复后 `self`/`pending` 可能已被导入路径重定位到
   * 新对象，代码只通过这两个栈上指针访问，不缓存其它堆地址。
   */
  ChannelRecvResult<T> RecvMigratable(std::unique_lock<std::mutex>& lock,
                                      Fiber* self) {
    std::shared_ptr<PendingRecv> pending;
    try {
      pending = MakeMigrationWaitNode();
    } catch (...) {
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }
    EnqueueMigrationWaitNode(pending);
    for (;;) {
      if (pending->ready) {
        return {std::move(pending->value), pending->ok, true, pending->status,
                pending->error};
      }
      if (pending->cancelled) {
        return {std::nullopt, false, true, pending->status, pending->error};
      }
      self->BeginMigrationWait(this, pending.get(), &pending, 1);
      lock.unlock();
      const bool suspended = Fiber::Suspend(SuspendReason::Park);
      if (!lock.owns_lock()) {
        lock.lock();
      }
      self->EndMigrationWait();
      if (!suspended) {
        ErasePending(m_receivers, pending);
        ++m_generation;
        return {std::nullopt, false, true, ChannelStatus::kInvalid,
                detail::ValueOperationError()};
      }
      // 恢复后重新检查（发送方可能已交接；也可能只是被宿主提前 resume）。
    }
  }

 private:
#endif  // GO2CPP_MIGRATION_API

  /**
   * @brief 在离开作用域时注销 Context 取消回调的 RAII 守卫。
   *
   * ## 定位
   *
   * `SendUntil`/`RecvUntil` 在 park 前把等待节点注册到 `Context::Done()`，
   * 本守卫保证**任何退出路径**（正常完成、取消、异常）都会 `RemoveCallback`。
   * 不注销虽然靠 `weak_ptr` + generation 也不会出错，但会让 Context 一直持有
   * 回调（每次取消多跑一次空回调）。
   *
   * ## 为什么析构里有 try/catch
   *
   * 析构是 `noexcept`；`RemoveCallback` 内部加锁，理论上可能抛 `std::system_error`。
   * 清理路径抛异常会直接 `terminate`，所以这里吞掉。
   */
  struct CallbackGuard {
    /// @brief 要注销回调的 Context（引用，不延长生命周期）。
    const ContextPtr& context;
    /// @brief 注册时拿到的回调 Id；0 表示未注册（无需注销）。
    DoneSignal::CallbackId id{0};
    ~CallbackGuard() noexcept {
      if (context && id != 0) {
        try {
          context->Done().RemoveCallback(id);
        } catch (...) {
          // 清理路径不能让异常越过 noexcept 析构函数。
        }
      }
    }
  };

  static ChannelSendResult ContextSendStatus(const ContextPtr& context) {
    if (!context || !context->IsDone()) {
      return {ChannelStatus::kReady, {}};
    }
    const auto err = context->Err();
    if (err && Is(err, DeadlineExceededError())) {
      return {ChannelStatus::kDeadlineExceeded, err};
    }
    return {ChannelStatus::kCancelled, err ? err : CanceledError()};
  }

  static ChannelStatus ContextRecvStatus(const ContextPtr& context,
                                         ErrorPtr* error) {
    if (!context || !context->IsDone()) {
      return ChannelStatus::kReady;
    }
    *error = context->Err();
    if (*error && Is(*error, DeadlineExceededError())) {
      return ChannelStatus::kDeadlineExceeded;
    }
    if (!*error) {
      *error = CanceledError();
    }
    return ChannelStatus::kCancelled;
  }

  static bool DeadlineReached(const std::optional<TimePoint>& deadline) {
    return deadline.has_value() && std::chrono::steady_clock::now() >= *deadline;
  }

  // 锁外调用：只在确有 WaitForChange 观察者时才唤醒，避免每次
  // send/recv 快速路径都为一个空等待队列支付一次互斥量。
  void NotifyChange() noexcept {
    if (m_change_waiters.load(std::memory_order_acquire) == 0) {
      return;
    }
    m_change_cv.notify_all();
  }

  void NotifyGeneration() {
    m_generation.fetch_add(1, std::memory_order_release);
    NotifyChange();
  }

  /**
   * @brief 向 channel 发送一个值，直到成功、超时、取消或 channel 关闭。
   * @param value 要发送的值（移动语义；失败时可能已被移动）。
   * @param deadline 绝对超时点；`nullopt` 表示无限等待。
   * @param context 可选的取消上下文；`Done()` 被触发时唤醒并返回 `kCancelled`。
   * @param nonblocking 非阻塞模式：无法立即完成时返回 `kWouldBlock` 而不是 park。
   * @return `ChannelSendResult`，`status` 说明结局，`error` 在失败时非空。
   *
   * ## 定位
   *
   * `channel::Send` / `SendFor` / `TrySend` 的共同落点，也是 GMP 里
   * **最典型的「多条件等待」**：一次调用要同时等「接收者出现」「缓冲区有空位」
   * 「context 取消」「deadline 到期」四件事中的任意一件。
   *
   * ## 步骤
   *
   * step1. 上下文检查：已取消/已超时就直接返回，不进入锁；
   * step2. 加锁后检查 `m_closed`——关闭的 channel 上发送立即返回 `kClosed`；
   * step3. **优先交接给等待中的接收者**：从 `m_receivers` 队首取，跳过已取消的
   *        节点；`select_state` 节点走 `TrySelect`（select 语义：可能同时有多个
   *        分支就绪，由 select 决定谁赢）；普通节点直接 `value.emplace` +
   *        `cv.notify_one()`。成功后**锁外** `NotifyChange()`；
   * step4. 没有接收者但**缓冲区有空位**：入缓冲，同样锁外通知；
   * step5. 既无接收者又无空位：检查 deadline / Fiber 支持，然后创建
   *        `PendingSend` 入 `m_senders`，并把 `context->Done()` 的回调注册为
   *        「唤醒这个 pending」——这样取消能立刻打断 park；
   * step6. 进入 `for (;;)` 等待循环：每次被唤醒后重检
   *        `accepted`/`closed`/`cancelled`/context/deadline，任一成立就清理
   *        并返回；否则继续 `wait_for`（有 deadline）或 `wait`（无 deadline）。
   *
   * ## 并发/失败语义
   *
   * - **`NotifyChange()` 必须在锁外**：它可能跨线程唤醒 `WaitForChange` 的
   *   观察者，持 `m_mutex` 调用会形成锁序反转；
   * - **值只在交接成功后移动**：`emplace` 抛异常时保留接收/发送节点，
   *   后续仍可重试（`optional::emplace` 的强保证）；
   * - **pending 用 `shared_ptr` 持有**：`context` 回调可能在 park 线程之外
   *   触发，裸指针会在唤醒前被销毁；
   * - **所有提前返回路径都要 `ErasePending` + `++m_generation`**，
   *   否则等待者会永远留在队列里，且 generation 不前进会让 select 看不到变化；
   * - **`nonblocking` 与 `deadline` 的交互**：无空位且已到 deadline 时，
   *   非阻塞返回 `kWouldBlock`，阻塞返回 `kTimedOut`——两者的 `errno` 语义不同；
   * - **Fiber 不支持时返回 `kInvalid`**（手动 Fiber 无法 park），
   *   绝不阻塞 carrier 线程。
   */
  ChannelSendResult SendUntil(T value, std::optional<TimePoint> deadline,
                              const ContextPtr& context, bool nonblocking) {
    // step1: 上下文已取消/已超时，直接返回，不进入锁。
    if (const auto context_status = ContextSendStatus(context);
        context_status.status != ChannelStatus::kReady) {
      return context_status;
    }

    // step2: 关闭的 channel 上发送立即失败。
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_closed) {
      return {ChannelStatus::kClosed, ChannelClosedError()};
    }

    // step3: 优先交接给等待中的接收者（select 节点走 TrySelect，普通节点直接塞值）。
    while (!m_receivers.empty()) {
      auto receiver = m_receivers.front();
      if (receiver->cancelled) {
        m_receivers.pop_front();
        continue;
      }
      if (receiver->select_state) {
        if constexpr (std::is_copy_constructible<T>::value) {
          SelectProbe probe;
          probe.ready = true;
          probe.ok = true;
          try {
            if (!probe.SetValue(value)) {
              return {ChannelStatus::kInvalid, detail::ValueOperationError()};
            }
          } catch (...) {
            // 值复制失败时保留接收节点，后续发送仍可重试。
            return {ChannelStatus::kInvalid, detail::ValueOperationError()};
          }
          probe.status = ChannelStatus::kReady;
          bool selected = false;
          try {
            selected = receiver->select_state->TrySelect(
                receiver->select_index, std::move(probe));
          } catch (...) {
            // 选择状态未发布时节点仍然有效，不能先把它从队列移除。
            return {ChannelStatus::kInvalid, detail::ValueOperationError()};
          }
          m_receivers.pop_front();
          if (!selected) {
            receiver->cancelled = true;
            continue;
          }
          ++m_generation;
          lock.unlock();
          NotifyChange();
          return {ChannelStatus::kReady, {}};
        } else {
          receiver->cancelled = true;
          m_receivers.pop_front();
          continue;
        }
      }
      try {
        receiver->value.emplace(std::move(value));
      } catch (...) {
        // 保留接收者在队列中。optional::emplace 提供抛异常时保持空值的
        // 强保证，后续发送者可以重试。
        return {ChannelStatus::kInvalid, detail::ValueOperationError()};
      }
      m_receivers.pop_front();
      receiver->ok = true;
      receiver->ready = true;
      receiver->status = ChannelStatus::kReady;
      receiver->Signal();
      receiver->NotifyIfParked();
      ++m_generation;
      lock.unlock();
      NotifyChange();
      return {ChannelStatus::kReady, {}};
    }

    // step4: 没有接收者但有缓冲空位——入缓冲。
    if (m_buffer.size() < m_capacity) {
      try {
        m_buffer.emplace_back(std::move(value));
      } catch (...) {
        return {ChannelStatus::kInvalid, detail::ValueOperationError()};
      }
      ++m_generation;
      lock.unlock();
      NotifyChange();
      return {ChannelStatus::kReady, {}};
    }

    if (DeadlineReached(deadline)) {
      return {nonblocking ? ChannelStatus::kWouldBlock
                          : ChannelStatus::kTimedOut,
              nonblocking ? ChannelWouldBlockError() : ChannelTimeoutError()};
    }

    if (core::ParkingCondition::FiberWaitUnsupported()) {
      return {ChannelStatus::kInvalid, detail::ValueOperationError()};
    }
    // step5: 既无接收者又无空位——登记 PendingSend，并挂 context 回调
    // （取消时立刻唤醒，不必等下一个 send/recv）。
    //
    // 自旋只对 capacity <= 1 的通道启用：有缓冲通道（cap64/1024）的吞吐依赖
    // "等待者睡眠期间生产者批量填缓冲"；自旋会让等待者醒来后逐条 rendezvous，
    // 把批量交接退化成每消息一次 pending 分配 + 交接（实测 cap1024 从
    // ~40ns/msg 退化到 ~100ns/msg）。容量 1 没有批量空间，cap0 更是纯 rendezvous。
    // 受管 Fiber 不参与自旋，否则会占住 carrier M 线程。
    const bool spin_before_park = detail::CanSpinWait() && m_capacity <= 1;
    std::shared_ptr<PendingSend> pending;
    try {
      pending = std::make_shared<PendingSend>(std::move(value));
      // 必须在入队前写好：完成方会在 m_mutex 内根据它决定是否维护
      // signaled/parked 原子协议（cap>1 的等待者直接 park，不需要）。
      pending->spin_wait = spin_before_park;
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
      pending->debug_channel = m_debug_resource;
      pending->debug_waiter_gid = GO2CPP_DEBUG_BOUND_TASK();
#endif
      m_senders.emplace_back(pending);
    } catch (...) {
      return {ChannelStatus::kInvalid, detail::ValueOperationError()};
    }
    // 发送方即将 park：只在阻塞路径记录（缓冲命中/立即交接无开销）。
    GO2CPP_DEBUG_PARK_HINT(kChannelSend);
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    GO2CPP_DEBUG_RESOURCE_EVENT_EX(
        kChannelSend, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0,
        detail::ChannelDebugValue(pending->value),
        go2cpp::debug::ResourceResult::kBlocked,
        static_cast<std::uint32_t>(m_senders.size()), true,
        detail::ChannelDebugValueFlags<T>());
#else
    GO2CPP_DEBUG_EVENT(kChannelSend, 0, m_capacity);
#endif
    CallbackGuard callback_guard{context};
    if (context) {
      try {
        callback_guard.id = context->Done().AddCallback(
            [pending] { pending->cv.notify_one(); });
      } catch (...) {
        ErasePending(m_senders, pending);
        ++m_generation;
        return {ChannelStatus::kInvalid, detail::ValueOperationError()};
      }
    }

    // step6: park 等待循环——每次唤醒后重检 accepted/closed/cancelled/
    // context/deadline，任一成立就清理并返回。原生线程在 park 前先做一次
    // 自适应短自旋，自旋成功即免掉一次 futex 睡眠 + 唤醒。
    bool spin_attempted = false;
    for (;;) {
      if (pending->accepted) {
        return {ChannelStatus::kReady, {}};
      }
      if (pending->closed) {
        return {ChannelStatus::kClosed,
                pending->error ? pending->error : ChannelClosedError()};
      }
      if (pending->cancelled) {
        return {pending->status, pending->error};
      }

      ErrorPtr context_error;
      const auto context_status = ContextRecvStatus(context, &context_error);
      if (context_status != ChannelStatus::kReady ||
          core::ParkingCondition::CancellationRequested()) {
        pending->cancelled = true;
        pending->status = context_status != ChannelStatus::kReady
                              ? context_status
                              : ChannelStatus::kCancelled;
        pending->error = context_error ? context_error : CanceledError();
        ErasePending(m_senders, pending);
        ++m_generation;
        lock.unlock();
        NotifyChange();
        return {pending->status, pending->error};
      }
      if (DeadlineReached(deadline)) {
        pending->cancelled = true;
        pending->status = ChannelStatus::kTimedOut;
        pending->error = ChannelTimeoutError();
        ErasePending(m_senders, pending);
        ++m_generation;
        lock.unlock();
        NotifyChange();
        return {ChannelStatus::kTimedOut, pending->error};
      }

      if (spin_before_park && !spin_attempted) {
        // 自旋期间不持 m_mutex，对端可以正常把它认领走；返回后重新加锁
        // 并按最新标志裁决，若仍无结果才真正 park。
        spin_attempted = true;
        lock.unlock();
        (void)detail::SpinForSignal(pending->signaled);
        lock.lock();
        continue;
      }

      if (spin_before_park) {
        pending->parked.store(true, std::memory_order_relaxed);
      }
      // 只有启用自旋的通道才需要 park 时长反馈；有缓冲通道（cap>1）的
      // 高频 park 路径不能为一次 clock_gettime 付费（64 线程压测下实测
      // 会带来 ~10% 的回退）。
      const auto park_start = spin_before_park
                                  ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
      if (deadline.has_value()) {
        auto wait_for = *deadline - std::chrono::steady_clock::now();
        pending->cv.wait_for(lock, wait_for, [&] {
          return pending->accepted || pending->closed || pending->cancelled ||
                 (context && context->IsDone());
        });
      } else {
        pending->cv.wait(lock, [&] {
          return pending->accepted || pending->closed || pending->cancelled ||
                 (context && context->IsDone());
        });
      }
      if (spin_before_park) {
        pending->parked.store(false, std::memory_order_relaxed);
      }
      if (spin_before_park &&
          pending->signaled.load(std::memory_order_acquire)) {
        detail::ObserveParkDurationNs(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - park_start)
                .count());
      }
    }
  }

  ChannelRecvResult<T> RecvUntil(std::optional<TimePoint> deadline,
                                 const ContextPtr& context, bool nonblocking) {
    if (const auto context_status = ContextSendStatus(context);
        context_status.status != ChannelStatus::kReady) {
      return {std::nullopt, false, true, context_status.status,
              context_status.error};
    }

    std::unique_lock<std::mutex> lock(m_mutex);

    if (!m_buffer.empty()) {
      // Channel<T> 要求值类型移动构造不抛异常，因此提取队首值不会因异常
      // 留下损坏的缓冲元素。
      T value = std::move(m_buffer.front());
      m_buffer.pop_front();
      // 将一个被阻塞的发送者补入刚刚空出的缓冲槽位。
      RefillBufferLocked();
      ++m_generation;
      lock.unlock();
      NotifyChange();
      return {std::optional<T>(std::move(value)), true, true,
              ChannelStatus::kReady, {}};
    }

    // 无缓冲发送者可以直接与该接收者会合交接。
    while (!m_senders.empty()) {
      auto sender = m_senders.front();
      if (sender->cancelled) {
        m_senders.pop_front();
        continue;
      }
      if (sender->select_state) {
        // 在值可以传输前不能先认领已选中的发送者。对可能抛异常的值类型
        // 要求存在 noexcept 的复制/移动路径；否则保留发送者在队列中，
        // 返回可恢复的操作失败。
        if constexpr (!std::is_nothrow_move_constructible<T>::value &&
                      !std::is_nothrow_copy_constructible<T>::value) {
          return {std::nullopt, false, true, ChannelStatus::kInvalid,
                  detail::ValueOperationError()};
        } else {
          SelectProbe probe;
          probe.ready = true;
          probe.ok = true;
          probe.status = ChannelStatus::kReady;
          if (!sender->select_state->TrySelect(sender->select_index,
                                               std::move(probe))) {
            sender->cancelled = true;
            m_senders.pop_front();
            continue;
          }
        }
      }
      if constexpr (std::is_nothrow_move_constructible<T>::value) {
        T value = std::move(sender->value);
        m_senders.pop_front();
        sender->accepted = true;
        sender->status = ChannelStatus::kReady;
        sender->Signal();
        sender->NotifyIfParked();
        ++m_generation;
        lock.unlock();
        NotifyChange();
        return {std::optional<T>(std::move(value)), true, true,
                ChannelStatus::kReady, {}};
      } else {
        // 唯一剩余的可接受路径是 noexcept 复制。返回复制值，避免选中后
        // 再调用可能抛异常的移动构造。
        T value = sender->value;
        m_senders.pop_front();
        sender->accepted = true;
        sender->status = ChannelStatus::kReady;
        sender->Signal();
        sender->NotifyIfParked();
        ++m_generation;
        lock.unlock();
        NotifyChange();
        return {std::optional<T>(value), true, true,
                ChannelStatus::kReady, {}};
      }
    }

    // 关闭通道先排空缓冲值，之后返回零值。
    if (m_closed) {
      return {std::nullopt, false, true, ChannelStatus::kClosed,
              ChannelClosedError()};
    }

    ErrorPtr context_error;
    const auto context_status = ContextRecvStatus(context, &context_error);
    if (context_status != ChannelStatus::kReady) {
      return {std::nullopt, false, true, context_status, context_error};
    }
    if (DeadlineReached(deadline)) {
      return {std::nullopt, false, false,
              nonblocking ? ChannelStatus::kWouldBlock
                          : ChannelStatus::kTimedOut,
              nonblocking ? ChannelWouldBlockError() : ChannelTimeoutError()};
    }

#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
    // 迁移等待子集：显式开启迁移的 Fiber + 无缓冲空通道 + 无 context/deadline。
    // 走独立路径，避免在挂起栈上留下 ParkingCondition 的堆等待节点。
    if (Fiber* const migration_fiber = Fiber::Current();
        migration_fiber != nullptr && migration_fiber->migration_enabled() &&
        CanAdoptMigrationWait(deadline, context)) {
      return RecvMigratable(lock, migration_fiber);
    }
#endif
    if (core::ParkingCondition::FiberWaitUnsupported()) {
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }
    // 与 SendUntil 相同：原生线程在 park 前先做一次自适应短自旋，且只对
    // capacity <= 1 的通道启用（原因见 SendUntil 的注释：有缓冲通道依赖
    // 等待者睡眠期间批量填/取缓冲，自旋会把批量交接退化成逐条 rendezvous）。
    const bool spin_before_park = detail::CanSpinWait() && m_capacity <= 1;
    std::shared_ptr<PendingRecv> pending;
    try {
      pending = std::make_shared<PendingRecv>();
      // 必须在入队前写好：完成方据此决定是否维护 signaled/parked 协议。
      pending->spin_wait = spin_before_park;
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
      pending->debug_channel = m_debug_resource;
      pending->debug_waiter_gid = GO2CPP_DEBUG_BOUND_TASK();
#endif
      m_receivers.emplace_back(pending);
    } catch (...) {
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }
    // 接收方即将 park：只在阻塞路径记录（缓冲命中/立即交接无开销）。
    GO2CPP_DEBUG_PARK_HINT(kChannelRecv);
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    GO2CPP_DEBUG_RESOURCE_EVENT_EX(
        kChannelRecv, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
        go2cpp::debug::ResourceResult::kBlocked,
        static_cast<std::uint32_t>(m_receivers.size()), true, 0U);
#else
    GO2CPP_DEBUG_EVENT(kChannelRecv, 0, m_capacity);
#endif
    CallbackGuard callback_guard{context};
    if (context) {
      try {
        callback_guard.id = context->Done().AddCallback(
            [pending] { pending->cv.notify_one(); });
      } catch (...) {
        ErasePending(m_receivers, pending);
        ++m_generation;
        return {std::nullopt, false, true, ChannelStatus::kInvalid,
                detail::ValueOperationError()};
      }
    }

    bool spin_attempted = false;
    for (;;) {
      if (pending->ready) {
        return {std::move(pending->value), pending->ok, true, pending->status,
                pending->error};
      }
      if (pending->cancelled) {
        return {std::nullopt, false, true, pending->status, pending->error};
      }

      context_error.reset();
      const auto status = ContextRecvStatus(context, &context_error);
      if (status != ChannelStatus::kReady ||
          core::ParkingCondition::CancellationRequested()) {
        pending->cancelled = true;
        pending->status = status != ChannelStatus::kReady
                              ? status
                              : ChannelStatus::kCancelled;
        pending->error = context_error ? context_error : CanceledError();
        ErasePending(m_receivers, pending);
        ++m_generation;
        lock.unlock();
        NotifyChange();
        return {std::nullopt, false, true, pending->status, pending->error};
      }
      if (DeadlineReached(deadline)) {
        pending->cancelled = true;
        pending->status = ChannelStatus::kTimedOut;
        pending->error = ChannelTimeoutError();
        ErasePending(m_receivers, pending);
        ++m_generation;
        lock.unlock();
        NotifyChange();
        return {std::nullopt, false, false, ChannelStatus::kTimedOut,
                pending->error};
      }

      if (spin_before_park && !spin_attempted) {
        // 与 SendUntil 相同的「先自旋、后 park」：接收方的交接通常由正在
        // 发送的对端在几百纳秒内完成，能省掉一次 futex 往返。
        spin_attempted = true;
        lock.unlock();
        (void)detail::SpinForSignal(pending->signaled);
        lock.lock();
        continue;
      }

      if (spin_before_park) {
        pending->parked.store(true, std::memory_order_relaxed);
      }
      // 只有启用自旋的通道才需要 park 时长反馈；有缓冲通道（cap>1）的
      // 高频 park 路径不能为一次 clock_gettime 付费（64 线程压测下实测
      // 会带来 ~10% 的回退）。
      const auto park_start = spin_before_park
                                  ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
      if (deadline.has_value()) {
        auto wait_for = *deadline - std::chrono::steady_clock::now();
        pending->cv.wait_for(lock, wait_for, [&] {
          return pending->ready || pending->cancelled ||
                 (context && context->IsDone());
        });
      } else {
        pending->cv.wait(lock, [&] {
          return pending->ready || pending->cancelled ||
                 (context && context->IsDone());
        });
      }
      if (spin_before_park) {
        pending->parked.store(false, std::memory_order_relaxed);
      }
      if (spin_before_park &&
          pending->signaled.load(std::memory_order_acquire)) {
        detail::ObserveParkDurationNs(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - park_start)
                .count());
      }
    }
  }

  void RefillBufferLocked() {
    while (!m_senders.empty() && m_buffer.size() < m_capacity) {
      auto sender = m_senders.front();
      if (sender->cancelled) {
        m_senders.pop_front();
        continue;
      }
      if (sender->select_state) {
        if constexpr (!std::is_nothrow_move_constructible<T>::value &&
                      !std::is_nothrow_copy_constructible<T>::value) {
          // 不要认领一个无法在不冒缓冲区半提交风险的情况下传输值的 select。
          break;
        } else {
          try {
            if constexpr (std::is_nothrow_move_constructible<T>::value) {
              m_buffer.emplace_back(std::move(sender->value));
            } else {
              m_buffer.emplace_back(sender->value);
            }
          } catch (...) {
            break;
          }
          SelectProbe probe;
          probe.ready = true;
          probe.ok = true;
          probe.status = ChannelStatus::kReady;
          bool selected = false;
          try {
            selected = sender->select_state->TrySelect(sender->select_index,
                                                       std::move(probe));
          } catch (...) {
            selected = false;
          }
          if (!selected) {
            m_buffer.pop_back();
            sender->cancelled = true;
            m_senders.pop_front();
            continue;
          }
          sender->cancelled = true;
          m_senders.pop_front();
          continue;
        }
      }
      try {
        if constexpr (std::is_nothrow_move_constructible<T>::value) {
          m_buffer.emplace_back(std::move(sender->value));
        } else {
          m_buffer.emplace_back(sender->value);
        }
      } catch (...) {
        break;
      }
      sender->accepted = true;
      sender->status = ChannelStatus::kReady;
      sender->Signal();
      sender->NotifyIfParked();
      m_senders.pop_front();
      break;
    }
  }

  template <typename Node>
  static void ErasePending(std::deque<std::shared_ptr<Node>>& queue,
                           const std::shared_ptr<Node>& node) {
    queue.erase(std::remove(queue.begin(), queue.end(), node), queue.end());
  }

  /// @brief 固定容量（创建后不变）；0 表示无缓冲 rendezvous。
  std::size_t m_capacity{0};
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
  /// @brief 资源级历史编号（构造时注册；OFF 构建不存在该成员）。
  go2cpp::debug::ResourceId m_debug_resource{0};
#endif
  /// @brief 保护 buffer/senders/receivers/closed；Send/Recv/Close/Select 全部竞争它。
  mutable std::mutex m_mutex;
  /// @brief 状态变化通知（有值可收 / 有空间可发 / 已关闭）；等待者在锁外 park 在它上面。
  mutable core::ParkingCondition m_change_cv;
  /// @brief 正在 m_change_cv 上等待的线程/G 数量；为 0 时省掉一次 notify_all。
  mutable std::atomic<std::uint32_t> m_change_waiters{0};
  /// @brief 环形缓冲（cap==0 时不分配）；Send 入队、Recv 出队，索引回绕在 RingBuffer 内部。
  detail::RingBuffer<T> m_buffer;
  /// @brief 已登记但尚未拿到接收者的发送者队列；Recv 从这里直接取走值完成交接（绕过缓冲）。
  std::deque<std::shared_ptr<PendingSend>> m_senders;
  /// @brief 已登记但尚未拿到值的接收者队列；Send 直接把值塞给它完成交接。
  std::deque<std::shared_ptr<PendingRecv>> m_receivers;
  /// @brief 已关闭标记；Close 后 Send 返回 kAlreadyClosed，Recv 读完缓冲后返回 kClosed。
  bool m_closed{false};
  /// @brief 关闭代际（每次 Close +1）；Select 用它判断等待节点是否跨代失效。
  std::atomic<std::uint64_t> m_generation{0};
#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
  /// @brief AdoptWaitState 创建的新节点保活引用；ReleaseMigrationHold 释放。
  std::shared_ptr<PendingRecv> m_migration_hold;
#endif
};

template <typename T>
using ChannelPtr = std::shared_ptr<Channel<T>>;

template <typename T>
ChannelPtr<T> MakeChannel(std::size_t capacity = 0) {
  return Channel<T>::Create(capacity);
}

template <typename T>
ChannelSendResult SendChecked(const ChannelPtr<T>& channel, T value,
                             const ContextPtr& context = {});

template <typename T>
ChannelRecvResult<T> RecvChecked(const ChannelPtr<T>& channel,
                                const ContextPtr& context = {});

// 方向通道视图在 C++ 类型层模拟 Go 的 chan<- 与 <-chan。它们刻意不暴露
// 双向句柄：只读视图不能发送或关闭；只写视图可以发送和关闭但不能接收。
/**
 * 只发送通道视图。
 *
 * 依赖：共享 Channel<T>；对上层仅暴露 Send/TrySend/Close 等发送侧能力，
 * 不允许接收。视图不拥有独立缓冲区，底层 Channel 必须在使用期间存活。
 */
template <typename T>
class SendOnlyChannel final {
 public:
  SendOnlyChannel() = default;

  ChannelSendResult Send(T value, const ContextPtr& context = {}) const {
    return SendChecked(m_channel, std::move(value), context);
  }

  ChannelSendResult SendFor(T value, ContextDuration timeout,
                            const ContextPtr& context = {}) const {
    if (!m_channel) {
      return {ChannelStatus::kNil, ChannelNilError()};
    }
    return m_channel->SendFor(std::move(value), timeout, context);
  }

  ChannelSendResult TrySend(T value) const {
    if (!m_channel) {
      return {ChannelStatus::kNil, ChannelNilError()};
    }
    return m_channel->TrySend(std::move(value));
  }

  ChannelSendResult Close() const noexcept {
    if (!m_channel) {
      return {ChannelStatus::kNil, ChannelNilError()};
    }
    return m_channel->Close();
  }

  std::size_t Capacity() const noexcept {
    return m_channel ? m_channel->Capacity() : 0;
  }

  bool IsClosed() const { return m_channel && m_channel->IsClosed(); }
  explicit operator bool() const noexcept { return static_cast<bool>(m_channel); }

 private:
  explicit SendOnlyChannel(ChannelPtr<T> channel)
      : m_channel(std::move(channel)) {}

  ChannelPtr<T> m_channel;
  template <typename U>
  friend SendOnlyChannel<U> AsSendOnly(const ChannelPtr<U>& channel);
  template <typename U>
  friend SelectCase SendCase(const SendOnlyChannel<U>& channel, U value);
};

/**
 * 只接收通道视图。
 *
 * 依赖：共享 Channel<T>；对上层仅暴露 Recv/TryRecv 等接收侧能力，不允许
 * 发送或关闭。视图不拥有独立缓冲区，底层 Channel 必须在使用期间存活。
 */
template <typename T>
class RecvOnlyChannel final {
 public:
  RecvOnlyChannel() = default;

  ChannelRecvResult<T> Recv(const ContextPtr& context = {}) const {
    return RecvChecked(m_channel, context);
  }

  ChannelRecvResult<T> RecvFor(ContextDuration timeout,
                               const ContextPtr& context = {}) const {
    if (!m_channel) {
      return {std::nullopt, false, true, ChannelStatus::kNil,
              ChannelNilError()};
    }
    return m_channel->RecvFor(timeout, context);
  }

  ChannelRecvResult<T> TryRecv() const {
    if (!m_channel) {
      return {std::nullopt, false, true, ChannelStatus::kNil,
              ChannelNilError()};
    }
    return m_channel->TryRecv();
  }

  std::size_t Capacity() const noexcept {
    return m_channel ? m_channel->Capacity() : 0;
  }

  std::size_t Len() const { return m_channel ? m_channel->Len() : 0; }
  bool IsClosed() const { return m_channel && m_channel->IsClosed(); }
  explicit operator bool() const noexcept { return static_cast<bool>(m_channel); }

 private:
  explicit RecvOnlyChannel(ChannelPtr<T> channel)
      : m_channel(std::move(channel)) {}

  ChannelPtr<T> m_channel;
  template <typename U>
  friend RecvOnlyChannel<U> AsRecvOnly(const ChannelPtr<U>& channel);
  template <typename U>
  friend SelectCase RecvCase(const RecvOnlyChannel<U>& channel);
};

template <typename T>
SendOnlyChannel<T> AsSendOnly(const ChannelPtr<T>& channel) {
  return SendOnlyChannel<T>(channel);
}

template <typename T>
RecvOnlyChannel<T> AsRecvOnly(const ChannelPtr<T>& channel) {
  return RecvOnlyChannel<T>(channel);
}

// 检查型自由函数把 nil 通道行为显式化：可空 ChannelPtr 返回 kNil，避免
// 像 Go 的 nil 通道一样永久等待。
template <typename T>
ChannelSendResult SendChecked(const ChannelPtr<T>& channel, T value,
                             const ContextPtr& context) {
  if (!channel) {
    return {ChannelStatus::kNil, ChannelNilError()};
  }
  return channel->Send(std::move(value), context);
}

template <typename T>
ChannelRecvResult<T> RecvChecked(const ChannelPtr<T>& channel,
                                const ContextPtr& context) {
  if (!channel) {
    return {std::nullopt, false, true, ChannelStatus::kNil, ChannelNilError()};
  }
  return channel->Recv(context);
}

/**
 * Select 的一个候选分支。
 *
 * probe 用于非阻塞探测；arm/disarm 分别登记和撤销等待节点；is_default
 * 表示默认分支。Select 会保证最多一个 case 提交成功，unsupported 用于
 * 显式报告当前值类型不满足内建通道 select 的约束。
 */
struct SelectCase {
  std::function<SelectProbe()> probe;
  bool is_default{false};
  std::function<void(const std::shared_ptr<detail::SelectWaitState>&,
                     std::size_t)>
      arm;
  std::function<void(const std::shared_ptr<detail::SelectWaitState>&)> disarm;
  // 内建 Channel Select 对不可复制 T 先显式拒绝，避免 arm 失败后永久等待。
  // 独立 SelectValue 仍可承载 move-only 值。
  bool unsupported{false};
  // 可选的带结果注册器。false 表示注册失败；若此时 wait_state 已经
  // 被其他 case 选中，则 Select 会继续清理而不是把它误判为错误。
  std::function<bool(const std::shared_ptr<detail::SelectWaitState>&,
                     std::size_t)>
      arm_checked;
  // 可选的「无副作用取消探测」：仅 DoneCase 使用。ctx 已结束时返回
  // CancelledProbe；Select 在回滚（Cancel）之前会重试它，避免循环里
  // 因 arm 过的 case 被跳过而漏掉取消分支。
  std::function<SelectProbe()> cancel_probe;

  explicit operator bool() const noexcept {
    return static_cast<bool>(probe) || is_default || static_cast<bool>(arm) ||
           static_cast<bool>(arm_checked) || unsupported;
  }
};

/**
 * Select 的结构化结果。
 *
 * index 是选中的 case 下标，selected 表示是否有分支完成，status/error
 * 表示通道或超时/取消结果；typed_value 保留不可复制值的类型擦除对象。
 */
struct SelectResult {
  static constexpr std::size_t kNoSelection = static_cast<std::size_t>(-1);

  std::size_t index{kNoSelection};
  bool selected{false};
  bool ok{false};
  std::any value;
  ChannelStatus status{ChannelStatus::kWouldBlock};
  ErrorPtr error;
  SelectValue typed_value;

  SelectResult() = default;
  SelectResult(std::size_t result_index, bool result_selected,
               bool result_ok, std::any result_value,
               ChannelStatus result_status, ErrorPtr result_error,
               SelectValue result_typed_value = {})
      : index(result_index),
        selected(result_selected),
        ok(result_ok),
        value(std::move(result_value)),
        status(result_status),
        error(std::move(result_error)),
        typed_value(std::move(result_typed_value)) {}

  explicit operator bool() const noexcept { return selected; }

  template <typename T>
  std::optional<std::decay_t<T>> Value() const {
    using ValueType = std::decay_t<T>;
    // std::any 只能保存可复制值；对 move-only 类型不要实例化
    // std::any_cast<T>()，调用方可使用 SelectResult::TakeValue<T>()。
    if constexpr (!std::is_copy_constructible<ValueType>::value) {
      return std::nullopt;
    } else {
      if (!value.has_value()) {
        return typed_value.As<ValueType>();
      }
      try {
        return std::any_cast<ValueType>(value);
      } catch (...) {
        return typed_value.As<ValueType>();
      }
    }
  }

  const SelectValue& ValueObject() const noexcept { return typed_value; }

  template <typename T>
  std::optional<std::decay_t<T>> TypedValue() const {
    return typed_value.As<T>();
  }

  template <typename T>
  std::optional<std::decay_t<T>> TakeValue() {
    return typed_value.Take<T>();
  }

  template <typename T>
  std::optional<T> Cast(const SelectCaster& caster) const {
    if (!typed_value.HasValue()) {
      return std::nullopt;
    }
    try {
      return caster.Cast(typed_value).As<T>();
    } catch (...) {
      return std::nullopt;
    }
  }

  template <typename T>
  std::optional<T> Cast(
      const std::shared_ptr<const SelectCaster>& caster) const {
    if (!caster) {
      return std::nullopt;
    }
    return Cast<T>(*caster);
  }
};

template <typename T>
SelectCase RecvCase(const ChannelPtr<T>& channel) {
  SelectCase result{
      [channel] {
        if (!channel) {
          SelectProbe probe;
          probe.ready = true;
          probe.status = ChannelStatus::kNil;
          probe.error = ChannelNilError();
          return probe;
        }
        auto value = channel->TryRecv();
        if (!value.ready) {
          return SelectProbe{};
        }
        SelectProbe probe;
        probe.ready = true;
        probe.ok = value.ok;
        probe.status = value.status;
        probe.error = value.error;
        if (value.value.has_value()) {
          if (!probe.SetValue(std::move(*value.value))) {
            probe.ready = true;
            probe.status = ChannelStatus::kInvalid;
            probe.error = detail::ValueOperationError();
          }
        }
        return probe;
      },
      false,
      [channel](const std::shared_ptr<detail::SelectWaitState>& state,
                std::size_t index) {
        if (channel) {
          channel->ArmSelectRecv(state, index);
        }
      },
      [channel](const std::shared_ptr<detail::SelectWaitState>& state) {
        if (channel) {
          channel->DisarmSelect(state);
        }
      },
      {},
      {},
      {}};
  result.arm_checked = [channel](
      const std::shared_ptr<detail::SelectWaitState>& state,
      std::size_t index) {
    return !channel || channel->ArmSelectRecv(state, index);
  };
  result.unsupported = !std::is_copy_constructible<T>::value;
  return result;
}
template <typename T>
SelectCase SendCase(const ChannelPtr<T>& channel, T value) {
  // 失败的 probe 还要保留发送值，因此内建 select 只接受可复制 T。
  auto shared_value = std::make_shared<T>(std::move(value));
  SelectCase result{
      [channel, shared_value] {
        if constexpr (!std::is_copy_constructible<T>::value) {
          SelectProbe probe;
          probe.ready = true;
          probe.status = ChannelStatus::kInvalid;
          probe.error = detail::ValueOperationError();
          return probe;
        } else {
          if (!channel) {
            SelectProbe probe;
            probe.ready = true;
            probe.status = ChannelStatus::kNil;
            probe.error = ChannelNilError();
            return probe;
          }
          auto send_result = channel->TrySend(*shared_value);
          if (send_result.status == ChannelStatus::kWouldBlock) {
            return SelectProbe{};
          }
          SelectProbe probe;
          probe.ready = true;
          probe.ok = send_result.Ok();
          probe.status = send_result.status;
          probe.error = send_result.error;
          return probe;
        }
      },
      false,
      [channel, shared_value](
          const std::shared_ptr<detail::SelectWaitState>& state,
          std::size_t index) {
        if constexpr (std::is_copy_constructible<T>::value) {
          if (channel) {
            channel->ArmSelectSend(state, index, *shared_value);
          }
        } else {
          (void)channel;
          (void)state;
          (void)index;
        }
      },
      [channel](const std::shared_ptr<detail::SelectWaitState>& state) {
        if (channel) {
          channel->DisarmSelect(state);
        }
      },
      {},
      {},
      {}};
  result.arm_checked = [channel, shared_value](
      const std::shared_ptr<detail::SelectWaitState>& state,
      std::size_t index) {
    if constexpr (std::is_copy_constructible<T>::value) {
      return !channel || channel->ArmSelectSend(state, index, *shared_value);
    }
    return true;
  };
  result.unsupported = !std::is_copy_constructible<T>::value;
  return result;
}
template <typename T>
SelectCase RecvCase(const RecvOnlyChannel<T>& channel) {
  return RecvCase(channel.m_channel);
}

template <typename T>
SelectCase SendCase(const SendOnlyChannel<T>& channel, T value) {
  return SendCase(channel.m_channel, std::move(value));
}

inline SelectCase DefaultCase() {
  return SelectCase{[] {
                      SelectProbe probe;
                      probe.ready = true;
                      probe.ok = true;
                      probe.status = ChannelStatus::kReady;
                      return probe;
                    },
                    true,
                    {},
                    {},
                    {},
                    {},
                    {}};
}

namespace detail {

/**
 * @brief 一次 `Select` 调用期间，非通道 case 的「可撤销注册」登记表。
 *
 * ## 定位
 * `DoneCase` / `AfterCase` 不是 channel：它们靠 `SelectCase::arm` 注册一个外部
 * 事件源、靠 `SelectCase::disarm` 撤销。arm 与 disarm 之间**只有**
 * `SelectWaitState` 这一个共同参数，因此需要一张以它为键的登记表，把「注册时
 * 拿到的句柄」交给「撤销时使用」。本表就是那张表，回调 id 与 Timer 句柄共用。
 *
 * ## 为什么不能用 case 内的单个槽位
 * `Select` 在没有任何分支获胜时会**重新 arm**（`src/concurrency/channel.cpp`
 * 的主循环），同一个 `wait_state` 会被反复使用；同一个 `SelectCase` 对象也可能
 * 被多个线程共享（case 表按值传入，且内部全是 const 可调用的 `std::function`）。
 * 单槽位在并发使用时会互相覆盖，导致 disarm 删错 id —— 前一个回调永远留在
 * `Done` 上，每调用一次 select 就泄漏一个回调。
 *
 * ## 并发/失败语义
 * 所有方法内部加锁；同一个键的 arm/disarm 由同一次 Select 调用发起（同一线程），
 * 不同 Select 的键不同。`Take*` 对不存在的键返回空值 —— `Select` 的回滚路径会对
 * **所有** case 调 disarm（包括从未 arm 成功的），这必须是无害的。
 */
class SelectCaseRegistry final {
 public:
  /** @brief 登记 Done 回调 id；同一键上的旧值由调用方先 Take 清理。 */
  void StoreCallback(const void* key, DoneSignal::CallbackId id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks[key] = id;
  }

  /** @brief 取走并删除回调 id；键不存在返回 0（0 是 DoneSignal 的无效哨兵）。 */
  DoneSignal::CallbackId TakeCallback(const void* key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_callbacks.find(key);
    if (found == m_callbacks.end()) {
      return 0;
    }
    const DoneSignal::CallbackId id = found->second;
    m_callbacks.erase(found);
    return id;
  }

  /** @brief 登记定时器句柄；同一键上的旧句柄由调用方先 Take 取消。 */
  void StoreTimer(const void* key, std::shared_ptr<core::Timer> timer) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_timers[key] = std::move(timer);
  }

  /** @brief 取走并删除定时器句柄；键不存在返回空指针。 */
  std::shared_ptr<core::Timer> TakeTimer(const void* key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_timers.find(key);
    if (found == m_timers.end()) {
      return {};
    }
    auto timer = std::move(found->second);
    m_timers.erase(found);
    return timer;
  }

 private:
  std::mutex m_mutex;  ///< 保护两张表；arm/disarm 可能来自不同线程
  /// wait_state → Done 回调 id；arm 写入、disarm 取走
  std::unordered_map<const void*, DoneSignal::CallbackId> m_callbacks;
  /// wait_state → 已排程的 Timer；持有句柄避免 arm 返回后析构即取消
  std::unordered_map<const void*, std::shared_ptr<core::Timer>> m_timers;
};

/** @brief 构造「ctx 已取消」的探测结果；probe 与 Done 回调共用同一份语义。 */
inline SelectProbe CancelledProbe(const ContextPtr& ctx) {
  SelectProbe probe;
  probe.ready = true;
  probe.ok = false;
  probe.status = ChannelStatus::kCancelled;
  const auto err = ctx ? ctx->Err() : ErrorPtr{};
  probe.error = err ? err : CanceledError();
  return probe;
}

/** @brief 构造「计时器到期」的探测结果；probe 与 Timer 回调共用。 */
inline SelectProbe TimerFiredProbe() {
  SelectProbe probe;
  probe.ready = true;
  probe.ok = true;
  probe.status = ChannelStatus::kReady;
  return probe;
}

}  // namespace detail

/**
 * @brief 构造 `case <-ctx.Done()` 的 select 分支。
 *
 * ## 步骤
 * step1. `probe`：`ctx->IsDone()` 为真时返回就绪，状态 `kCancelled`，错误取
 *        `ctx->Err()`（取消原因为空时退化为 `CanceledError()`）；
 * step2. `arm`：`ctx->Done().AddCallback(...)` 注册回调，回调里只做
 *        `wait_state->TrySelect(index, CancelledProbe(ctx))`；返回的回调 id
 *        存进共享登记表（见 `detail::SelectCaseRegistry`），键是本次 Select 的
 *        `wait_state`；
 * step3. `disarm`：从登记表取回 id 并 `RemoveCallback`，然后删除表项。
 *
 * ## 并发/失败语义
 * - `ctx` 为空时永远不就绪（对齐 Go 的 nil channel）；只有这一个分支的 `Select`
 *   会永久等待，调用方需自行保证非空；
 * - 取消与就绪 channel 同时可选中时，由 `Select` 的轮转探测决定赢家，与 Go 的
 *   随机选择一致，**不保证** context 优先；
 * - arm 时若 `ctx` 已取消，`AddCallback` 会同步执行回调并返回 0，此时 disarm 不再
 *   调用 `RemoveCallback`（0 是无效 id）；
 * - 漏掉 disarm 会让每次 select 在 `Done` 上留下一个永不触发的回调，长期运行等于
 *   按调用次数泄漏内存，因此 disarm 必须与 arm 成对。
 */
inline SelectCase DoneCase(const ContextPtr& ctx) {
  auto registry = std::make_shared<detail::SelectCaseRegistry>();
  return SelectCase{
      [ctx] {
        if (ctx && ctx->IsDone()) {
          return detail::CancelledProbe(ctx);
        }
        return SelectProbe{};
      },
      false,
      [ctx, registry](const std::shared_ptr<detail::SelectWaitState>& state,
                      std::size_t index) {
        // step2.1: Select 重试会复用同一个 wait_state，先清掉上一轮残留的 id，
        //          否则旧回调既不会被触发也不会被移除。
        const auto previous = registry->TakeCallback(state.get());
        if (ctx && previous != 0) {
          ctx->Done().RemoveCallback(previous);
        }
        if (!ctx) {
          return;
        }
        // step2.2: 回调在 Done 完成时由 Signal 在锁外调用，且 Done 状态先发布，
        //          所以这里读 ctx->Err() 拿到的一定是最终原因。
        const auto id = ctx->Done().AddCallback([ctx, state, index] {
          try {
            (void)state->TrySelect(index, detail::CancelledProbe(ctx));
          } catch (...) {
            // 回调只负责唤醒；构造失败时 Select 侧另有错误返回路径。
          }
        });
        if (id != 0) {
          registry->StoreCallback(state.get(), id);
        }
      },
      [ctx, registry](const std::shared_ptr<detail::SelectWaitState>& state) {
        const auto id = registry->TakeCallback(state.get());
        if (ctx && id != 0) {
          ctx->Done().RemoveCallback(id);
        }
      },
      {},
      {},
      [ctx] {
        if (ctx && ctx->IsDone()) {
          return detail::CancelledProbe(ctx);
        }
        return SelectProbe{};
      }};
}

/**
 * @brief 构造 `case <-time.After(d)` 的 select 分支。
 *
 * ## 步骤
 * step1. `probe`：`steady_clock::now() >= deadline` 即就绪（等价于 Go 的
 *        `time.After` 已经触发），命中结果 `ok=true`、`status=kReady`；
 * step2. `arm`：`core::TimerService::Default().Schedule(deadline, cb)` 排程一次性
 *        定时器，回调里 `wait_state->TrySelect(index, TimerFiredProbe())`；
 *        **Timer 句柄存进 `std::shared_ptr` 登记表**——局部 `Timer` 在 arm 返回时
 *        析构，而析构等价于 Cancel，回调将永远不会触发，这是最容易踩的坑；
 * step3. `disarm`：取回 `Timer` 并 `Cancel()`，然后删除表项。
 *
 * ## 并发/失败语义
 * - 传入过去的 deadline 时 probe 立即就绪，不会排程任何定时器（无泄漏）；
 * - 定时器回调可能已经进入执行队列，`Cancel()` 只能撤销尚未执行的触达；此时
 *   回调里的 `TrySelect` 会因状态已被其他分支选中或已取消而返回 false；
 * - `TimerService::Default()` 已停止时 `Schedule` 抛异常，`Select` 会按注册失败
 *   走统一回滚路径并返回 `kInvalid`；
 * - 未 disarm 的 `Timer` 会一直挂在服务队列里直到到期（`PendingCount()` 可见），
 *   大批量使用必须依靠 disarm 取消。
 */
inline SelectCase AfterCase(std::chrono::steady_clock::time_point deadline) {
  auto registry = std::make_shared<detail::SelectCaseRegistry>();
  return SelectCase{
      [deadline] {
        if (std::chrono::steady_clock::now() >= deadline) {
          return detail::TimerFiredProbe();
        }
        return SelectProbe{};
      },
      false,
      [deadline, registry](
          const std::shared_ptr<detail::SelectWaitState>& state,
          std::size_t index) {
        // step2.1: Select 重试复用 wait_state，先把上一轮没撤掉的定时器取消。
        if (auto previous = registry->TakeTimer(state.get())) {
          previous->Cancel();
        }
        // step2.2: Timer 必须由 shared_ptr 持有到 disarm；局部变量析构即取消。
        auto timer = std::make_shared<core::Timer>(
            core::TimerService::Default().Schedule(deadline, [state, index] {
              try {
                (void)state->TrySelect(index, detail::TimerFiredProbe());
              } catch (...) {
                // 唤醒失败说明本次 select 已由其他分支获胜，无需额外处理。
              }
            }));
        registry->StoreTimer(state.get(), std::move(timer));
      },
      [registry](const std::shared_ptr<detail::SelectWaitState>& state) {
        if (auto timer = registry->TakeTimer(state.get())) {
          timer->Cancel();
        }
      },
      {},
      {},
      {}};
}

/** @brief `AfterCase` 的相对时长版本，等价 Go 的 `time.After(d)`。 */
inline SelectCase AfterCase(ContextDuration timeout) {
  return AfterCase(detail::SaturatingDeadline(timeout));
}

// 执行多路 select。cases 是候选分支，context 可取消，timeout 是可选相对
// 超时；返回选中下标、值和状态。没有就绪分支且无 default 时会等待。
SelectResult Select(const std::vector<SelectCase>& cases,
                    const ContextPtr& context = {},
                    std::optional<ContextDuration> timeout = std::nullopt);

inline SelectResult Select(std::initializer_list<SelectCase> cases,
                           const ContextPtr& context = {},
                           std::optional<ContextDuration> timeout = std::nullopt) {
  return Select(std::vector<SelectCase>(cases), context, timeout);
}

}  // namespace go2cpp
