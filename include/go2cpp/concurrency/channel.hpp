/**
 * @file concurrency/channel.hpp
 * @brief Go 风格 channel 与 select 的完整实现（模板，header-only）。
 *
 * ## 模块定位
 *
 * `Channel<T>` 是 Go `chan T` 的对应物：带缓冲/无缓冲、`Send/Recv/TrySend/
 * TryRecv/Close`（没有 Go 的 `range` 语法糖，排空请循环 `Recv`），语义与
 * Go 的 `hchan` 对齐（预分配环形缓冲 +
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
 *   `kClosed`（不抛异常）；`kAlreadyClosed` 只用于重复 `Close()`；
 * - 排空语义：关闭后仍能把缓冲区里的剩余值读完，再收 `kClosed` 退出
 *   （没有 `Range` 成员，需要调用方循环 `Recv`）；
 * - 手动 Fiber（未归属 Scheduler）里 `Select` 返回 `kInvalid`，而
 *   `ParkingCondition` 返回 `kCancelled` —— 两者语义不同，别当成等价。
 */
#pragma once

#include "go2cpp/context.hpp"
#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/concurrency/core/futex_mutex.hpp"
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

// 在不持有 Channel::m_mutex 的情况下等待**通道代际**变化（有缓冲通道的
// 「等空位 / 等数据」）。返回 true 表示代际已推进，调用方应重新加锁重试；
// false 表示预算耗尽，调用方应转入 park。
//
// 与 SpinForSignal 的区别：这里观察的是通道级代际，对端的任何 send/recv
// 都会推进它，因此**不需要先登记等待节点** —— 有缓冲通道的 park 代价在
// WSL2 上约 20-30µs（futex 唤醒延迟），而一次 4µs 自旋命中就能完全绕开它。
// 每 512 次 PAUSE 让出一次 CPU 的原因同 SpinForSignal。
inline bool SpinForChange(const std::atomic<std::uint64_t>& generation,
                          std::uint64_t sequence) noexcept {
  const int limit = SpinBudget();
  for (int i = 0; i < limit; ++i) {
    if (generation.load(std::memory_order_acquire) != sequence) {
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
  return generation.load(std::memory_order_acquire) != sequence;
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

// ---------------------------------------------------------------------------
// P0 单锁化所需的等待基础设施
//
// 设计要点（对应 docs/research/go-channel-opt.md §7 P0 #1/#2/#3/#4/#5）：
//
// 1. **等待队列归 Channel 所有**：节点是侵入式双链表，队列只持裸指针；
//    入队/出队/摘链全部在 Channel 自旋锁内完成，notify 不再经过
//    ParkingCondition 的内部 deque 与内部锁。
// 2. **节点池化**：节点从线程本地空闲链取还（上限 128，对应 Go 的
//    `p.sudogbuf[128]`），稳态每次阻塞收发 0 次堆分配。
// 3. **per-node futex**：每个节点有自己的 32 位唤醒字，唤醒方只推进目标
//    节点的字并 FUTEX_WAKE(1)（审计 D2：共享字会唤醒错节点）。
// 4. **取消不持节点**：Context 回调只持 `weak_ptr<CancelHub>` + token，
//    在 hub 锁与 Channel 锁内按 token 定位节点，CAS signal 后唤醒。
//    回调从不直接解引用队列外的节点，节点生命周期完全由等待者栈帧持有。
// 5. **不能持锁 park**：自旋锁只在极短的临界区内持有；等待者入队后立刻
//    解锁，futex/调度器 park 都发生在锁外。
// ---------------------------------------------------------------------------

/// @brief 等待节点的唤醒协议状态（锁外原子字，只负责「可以醒了」）。
enum class WaitSignal : std::uint32_t {
  kWaiting = 0,
  /// @brief 已被交接/已放入值（结果细节仍在 Channel 锁内）。
  kReady = 1,
  /// @brief 通道已关闭。
  kClosed = 2,
  /// @brief 被取消、超时或排队清理。
  kCancelled = 3,
};

/// @brief 等待节点公共头：池化、侵入式链表、per-node futex、Fiber 绑定。
///
/// 生命周期契约：节点地址由等待者线程独占（池也是线程本地的），
/// Channel 队列与取消回调只允许在持有 Channel 锁时访问节点；等待者退出
/// 等待并归还池之后，任何路径都不再持有该地址。
struct WaitNodeBase {
  /// @brief 唤醒协议字；交接方用 CAS 0→kReady/kClosed/kCancelled 决定唯一赢家。
  std::atomic<std::uint32_t> signal{0};
  /// @brief 该节点自己的 futex 代际字；唤醒方 fetch_add 后 FUTEX_WAKE。
  std::atomic<std::uint32_t> seq{0};
  /// @brief 是否已经真正进入 FUTEX_WAIT（0/1，仅用于跳过无谓系统调用）。
  std::atomic<std::uint32_t> parked{0};
  /// @brief 取消回调定位节点用的身份（每次从池取出时递增，复用地址后必变）。
  std::uint32_t generation{0};
  /// @brief 侵入式双向链表指针（仅 Channel 锁内读写）。
  WaitNodeBase* next{nullptr};
  WaitNodeBase* prev{nullptr};
  /// @brief 是否仍在 Channel 队列中（锁内维护）。
  bool linked{false};
  /// @brief 是否由受管 Fiber 等待（true 时走 Scheduler park/wake）。
  bool managed{false};
  /// @brief 受管等待的 Scheduler（类型擦除，channel.cpp 内转换）。
  void* scheduler{nullptr};
  /// @brief 受管等待的 Task（类型擦除，避免 header 依赖 scheduler.hpp）。
  std::shared_ptr<void> task;
  /// @brief select 节点归属；普通 Send/Recv 节点为空。
  std::shared_ptr<SelectWaitState> select_state;
  /// @brief select 节点的 case 下标。
  std::size_t select_index{static_cast<std::size_t>(-1)};

  /// @brief 归还池前把全部字段恢复成「新建」状态。
  void ResetBase() noexcept {
    signal.store(0, std::memory_order_relaxed);
    seq.store(0, std::memory_order_relaxed);
    parked.store(0, std::memory_order_relaxed);
    generation = 0;
    next = nullptr;
    prev = nullptr;
    linked = false;
    managed = false;
    scheduler = nullptr;
    task.reset();
    select_state.reset();
    select_index = static_cast<std::size_t>(-1);
  }

  bool IsSelect() const noexcept { return select_state != nullptr; }
};

/// @brief 取消回调与 Channel 生命周期的解耦点。
///
/// Context 可能比 Channel 活得久；回调只持 `weak_ptr<CancelHub>`，Channel
/// 析构时在 hub 锁内把 owner 置空，之后到达的回调看到空指针直接返回。
/// 锁序固定为 hub.mutex → Channel 锁，析构路径先置空再 Close，不构成环。
template <typename ChannelT>
struct CancelHub {
  std::mutex mutex;
  ChannelT* owner{nullptr};
};

/// @brief 侵入式双向等待队列（裸指针；节点由等待者线程持有）。
template <typename Node>
class WaitQueue {
 public:
  bool empty() const noexcept { return m_size == 0; }
  std::size_t size() const noexcept { return m_size; }
  Node* front() const noexcept { return static_cast<Node*>(m_head); }
  Node* back() const noexcept { return static_cast<Node*>(m_tail); }
  WaitNodeBase* head() const noexcept { return m_head; }

  void push_back(Node* node) noexcept {
    WaitNodeBase* base = node;
    base->prev = m_tail;
    base->next = nullptr;
    if (m_tail != nullptr) {
      m_tail->next = base;
    } else {
      m_head = base;
    }
    m_tail = base;
    base->linked = true;
    ++m_size;
  }

  Node* pop_front() noexcept {
    WaitNodeBase* node = m_head;
    if (node == nullptr) {
      return nullptr;
    }
    Unlink(node);
    return static_cast<Node*>(node);
  }

  /// @brief O(1) 摘链（节点不在队列中时无副作用）。
  void erase(Node* node) noexcept {
    if (node == nullptr || !node->linked) {
      return;
    }
    Unlink(node);
  }

  void swap(WaitQueue& other) noexcept {
    std::swap(m_head, other.m_head);
    std::swap(m_tail, other.m_tail);
    std::swap(m_size, other.m_size);
  }

 private:
  void Unlink(WaitNodeBase* node) noexcept {
    if (node->prev != nullptr) {
      node->prev->next = node->next;
    } else {
      m_head = node->next;
    }
    if (node->next != nullptr) {
      node->next->prev = node->prev;
    } else {
      m_tail = node->prev;
    }
    node->prev = nullptr;
    node->next = nullptr;
    node->linked = false;
    --m_size;
  }

  WaitNodeBase* m_head{nullptr};
  WaitNodeBase* m_tail{nullptr};
  std::size_t m_size{0};
};

/// @brief 线程本地节点池（稳态零分配；上限 128，超出后释放）。
template <typename Node>
class WaitNodePool {
 public:
  static Node* Acquire() {
    auto& pool = Local();
    if (!pool.empty()) {
      Node* node = pool.back();
      pool.pop_back();
      return node;
    }
    return new Node();
  }

  static void Release(Node* node) noexcept {
    if (node == nullptr) {
      return;
    }
    node->Reset();
    auto& pool = Local();
    if (pool.size() < kLocalCapacity) {
      pool.push_back(node);
    } else {
      delete node;
    }
  }

 private:
  static constexpr std::size_t kLocalCapacity = 128;

  static std::vector<Node*>& Local() {
    static thread_local std::vector<Node*> pool;
    return pool;
  }
};

// ---- 自旋锁：无竞争一次 CAS，有竞争先 PAUSE 自旋，最后 futex 兜底 ----

/// @brief 仅供 Channel 队列使用的互斥门（自旋 + futex）。
///
/// 为什么不是自研的两态 spinlock：那版 `exchange(0) + FUTEX_WAKE(1)` 依赖
/// 「被唤醒者一定会接手锁并继续唤醒链」。实测 MPMC 绑定 2 核时该依赖不成立
/// （等待者被信号打断/EAGAIN 重试路径退出唤醒链），锁空闲却有人永久睡在
/// futex 上（gdb 栈：消费者在 LockSlow、生产者在 WaitNative）。
///
/// 这里复用审计过的 `core::FutexMutex`（locked+waiters 双位状态、唤醒链确认
/// 有人接手、对「被唤醒者已退出」继续唤醒下一个），并在外层加 64 次 PAUSE
/// 自旋：Channel 临界区只有几十条指令，短竞争完全在用户态解决；只有真正
/// 久的竞争才进入 FUTEX_WAIT。**任何时候都不允许持本锁 park。**
class ChannelSpinLock {
 public:
  ChannelSpinLock() noexcept = default;
  ChannelSpinLock(const ChannelSpinLock&) = delete;
  ChannelSpinLock& operator=(const ChannelSpinLock&) = delete;

  void lock() noexcept {
    for (int spin = 0; spin < kSpinIterations; ++spin) {
      if (m_gate.try_lock()) {
        return;
      }
      Relax();
    }
    m_gate.lock();
  }

  bool try_lock() noexcept { return m_gate.try_lock(); }
  void unlock() noexcept { m_gate.unlock(); }

 private:
  static constexpr int kSpinIterations = 64;

  static void Relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  }

  alignas(64) core::FutexMutex m_gate;
};

// ---- select 锁集（对齐 Go selectgo 的 sellock / selunlock） ----

/// @brief 类型擦除后的一把 channel 锁：全序键 + 加/解锁函数。
///
/// 为什么需要类型擦除：`Select` 主循环在非模板的
/// `src/concurrency/channel.cpp` 里实现，拿不到 `Channel<T>` 的类型；而锁
/// 必须由 `Channel<T>` 自己的实例提供，因此把「键 + 函数指针」打包传递。
struct SelectLockEntry {
  /// @brief 全序键：channel 对象地址（生命周期内不变且唯一）。
  const void* key{nullptr};
  /// @brief 加锁；noexcept，与 ChannelSpinLock::lock 一致。
  void (*lock)(const void* key) noexcept = nullptr;
  /// @brief 解锁；必须与 lock 成对且按相反顺序调用。
  void (*unlock)(const void* key) noexcept = nullptr;

  explicit operator bool() const noexcept {
    return key != nullptr && lock != nullptr && unlock != nullptr;
  }
};

/// @brief Go `selectgo` 里 sellock 的对应物：按 channel 地址全序加锁。
///
/// ## 为什么必须排序
/// 一次 select 要同时持有全部参与 channel 的锁，「检查就绪 + 登记等待节点」
/// 才是一个原子步骤（对应 Go 的 `sellock` 之后集中处理所有 case）。若两个
/// select 以各自的 case 顺序加锁，就会形成 A→B / B→A 的锁序环：两个 goroutine
/// 各持一把锁、互等对方的另一把，永久挂起。Go 的解法是先对 hchan 指针做
/// 堆排序得到全局一致的顺序再加锁（`runtime/chan.go` 的 lockorder），这里
/// 用 `std::sort` 达到同样效果。
///
/// ## 去重
/// 同一个 channel 可以同时出现在一个 select 的两个 case 里（例如同一通道
/// 上同时收发）；ChannelSpinLock 不可重入，重复加锁会自锁。排序后相邻同键
/// 只保留一项。
///
/// ## RAII
/// 析构按加锁顺序逆序解锁；Select 的异常/提前返回路径无需手工回滚。锁函数
/// 为 noexcept，加锁过程不会抛异常留下「部分持锁」状态。
class SelectLockSet {
 public:
  SelectLockSet() = default;
  ~SelectLockSet() { Release(); }
  SelectLockSet(const SelectLockSet&) = delete;
  SelectLockSet& operator=(const SelectLockSet&) = delete;

  /// @brief 登记一个 channel 锁；空 entry（nil channel）被忽略。
  void Add(const SelectLockEntry& entry) {
    if (entry) {
      m_entries.push_back(entry);
    }
  }

  /// @brief 按 key 排序去重后依次加锁；重复调用是幂等的。
  void Acquire() {
    if (m_entries.size() == m_locked) {
      return;
    }
    std::sort(m_entries.begin(), m_entries.end(),
              [](const SelectLockEntry& lhs, const SelectLockEntry& rhs) {
                return lhs.key < rhs.key;
              });
    m_entries.erase(std::unique(m_entries.begin(), m_entries.end(),
                                [](const SelectLockEntry& lhs,
                                   const SelectLockEntry& rhs) {
                                  return lhs.key == rhs.key;
                                }),
                    m_entries.end());
    for (; m_locked < m_entries.size(); ++m_locked) {
      m_entries[m_locked].lock(m_entries[m_locked].key);
    }
  }

  /// @brief 逆序释放已持有的锁；重复调用是幂等的。
  void Release() noexcept {
    while (m_locked > 0) {
      --m_locked;
      m_entries[m_locked].unlock(m_entries[m_locked].key);
    }
  }

  /// @brief 当前已持有的锁数量（调试与测试用）。
  std::size_t LockedCount() const noexcept { return m_locked; }

  /// @brief 去重排序后的锁数量（调试与测试用）。
  std::size_t Size() const { return m_entries.size(); }

 private:
  std::vector<SelectLockEntry> m_entries;
  std::size_t m_locked{0};
};

// ---- 平台原语（实现在 src/concurrency/channel.cpp，避免 header 依赖） ----

/// @brief 唤醒一个在该 futex 字上等待的线程（用于 per-node 唤醒字）。
void ChannelFutexWake(std::atomic<std::uint32_t>* word) noexcept;
/// @brief 在 futex 字上等待（带超时）；返回后调用方自行重检状态。
void ChannelFutexWait(std::atomic<std::uint32_t>* word, std::uint32_t expected,
                      const std::optional<ContextTimePoint>& deadline) noexcept;

/// @brief 原生线程等待：自旋 → FUTEX_WAIT → 重检 signal/seq。
void ChannelWaitNative(WaitNodeBase* node,
                       const std::optional<ContextTimePoint>& deadline) noexcept;

/// @brief 把自己绑定到受管 Scheduler；返回 false 表示当前上下文不能 park。
bool ChannelBindManaged(WaitNodeBase* node) noexcept;

/// @brief 受管等待一次 park；返回 false 表示被取消/无法继续等待。
bool ChannelParkOnce(WaitNodeBase* node) noexcept;

/// @brief 唤醒一个受管等待节点（Scheduler::wake_or_cancel）。
void ChannelWakeManaged(WaitNodeBase* node) noexcept;

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
//
// 不变量：m_count == 0 表示空；m_count == m_capacity 表示满；
//         有效元素位于 [m_head, m_head + m_count) 按 m_capacity 取模的区间内。
//
// 注：所有操作由 Channel::m_mutex 串行化（见 Channel 的锁序说明）。曾评估
// 「分离的生产/消费锁」与「无锁索引」：单线程路径确实更快，但 select 的
// 探测-回滚、等待者直接交接都要与缓冲共享同一临界区，改造面覆盖
// Send/Recv/Select/Close 全部路径；本次只保留经压测验证的版本（见
// /tmp/channel_opt.md 的限制一节）。
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

  // 兼容 select 路径的统一入口（单锁版本下就是 Try* 的等价包装）。
  template <typename U>
  bool TryPush(U&& value) {
    if (m_count >= m_capacity) {
      return false;
    }
    emplace_back(std::forward<U>(value));
    return true;
  }

  bool TryPopInto(std::optional<T>* out) {
    if (m_count == 0) {
      return false;
    }
    out->emplace(std::move(m_slots[m_head]));
    pop_front();
    return true;
  }

  void PushBackFront(std::optional<T>&& value) noexcept {
    // 单锁版本下 select 回滚发生在同一次持锁期间，直接放回队首。
    if (!value.has_value()) {
      return;
    }
    m_head = (m_head == 0) ? (m_capacity - 1) : (m_head - 1);
    ::new (static_cast<void*>(m_slots + m_head)) T(std::move(*value));
    ++m_count;
    value.reset();
  }

  bool TryPopBack() noexcept {
    if (m_count == 0) {
      return false;
    }
    pop_back();
    return true;
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
    // 取消回调只持 weak_ptr<CancelHub>，因此 Channel 不是 shared_ptr 管理时
    // 也不会出现「回调访问已析构 Channel」的窗口。
    m_cancel_hub = std::make_shared<detail::CancelHub<Channel<T>>>();
    m_cancel_hub->owner = this;
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    m_debug_resource = GO2CPP_DEBUG_RESOURCE_REGISTER(kChannel, this, nullptr);
#endif
  }
  ~Channel() noexcept {
    // 先切断取消回调进入 Channel 的通道，再唤醒剩余等待者。
    if (m_cancel_hub) {
      std::lock_guard<std::mutex> gate(m_cancel_hub->mutex);
      m_cancel_hub->owner = nullptr;
    }
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
    std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
    return m_buffer.size();
  }
  // 查询通道是否已关闭。
  /** @brief 返回 Channel 是否已关闭。 */
  bool IsClosed() const {
    std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
    return m_closed;
  }
  bool closed() const { return IsClosed(); }

  /**
   * @brief 采集通道的只读调试快照（缓冲长度/等待者/select 注册数）。
   * @note 一次持锁读取；与 Send/Recv/Close 并发时结果只代表某一瞬间。
   */
  ChannelDebugSnapshot DebugSnapshot() const {
    ChannelDebugSnapshot snapshot;
    std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
    snapshot.capacity = m_capacity;
    snapshot.length = m_buffer.size();
    snapshot.closed = m_closed;
    snapshot.send_waiters = m_senders.size();
    snapshot.recv_waiters = m_receivers.size();
    for (detail::WaitNodeBase* node = m_senders.head(); node != nullptr;
         node = node->next) {
      if (node->select_state != nullptr) {
        ++snapshot.select_senders;
      }
    }
    for (detail::WaitNodeBase* node = m_receivers.head(); node != nullptr;
         node = node->next) {
      if (node->select_state != nullptr) {
        ++snapshot.select_receivers;
      }
    }
    snapshot.change_waiters =
        m_change_waiters.load(std::memory_order_relaxed);
    snapshot.generation = m_generation.load(std::memory_order_relaxed);
    return snapshot;
  }

  /**
   * @brief **实验性**批量发送：一次持锁把尽可能多的值放进缓冲。
   *
   * ## 语义
   * - 只走缓冲（`Capacity() > 0`）；通道已关闭、有等待中的接收者，或值类型
   *   不可复制时返回 0 —— 调用方回退到单条 `Send`/`TrySend` 即可。批量路径
   *   刻意不抢占等待者：单条路径的「等待者优先直接交接」规则保持不变；
   * - 遇到缓冲满即停止，返回实际写入条数（可为 0）；
   * - 不 park、不处理 context/deadline，因此是**非阻塞**的；
   * - 值按传入顺序入队，FIFO 语义与逐条 `TrySend` 相同。
   *
   * ## 为什么需要
   * 有缓冲 channel 的单条成本主要是锁与状态发布的固定开销（跨核 cacheline
   * 传递，本机实测 ~120ns/次）。批量 API 把这笔开销摊到 count 条上，适合
   * 「生产者成批产出、消费者成批消费」的流水线；单条 Send/Recv 的语义与
   * 延迟特征不变。
   *
   * @return 实际发送条数；0 表示当前无法批量发送（满/关闭/有等待接收者）。
   */
  std::size_t TrySendBatch(const T* values, std::size_t count) {
    if constexpr (!std::is_copy_constructible<T>::value) {
      return 0;
    } else {
      if (values == nullptr || count == 0 || m_capacity == 0) {
        return 0;
      }
      std::size_t sent = 0;
      try {
        std::unique_lock<detail::ChannelSpinLock> lock(m_lock);
        if (m_closed || !m_receivers.empty()) {
          return 0;
        }
        while (sent < count && m_buffer.TryPush(values[sent])) {
          ++sent;
        }
        if (sent == 0) {
          return 0;
        }
        const bool published = PublishChangeLocked();
        lock.unlock();
        if (published) {
          NotifyChange();
        }
        return sent;
      } catch (...) {
        // 已入队的条目仍然有效，如实返回已提交数量（不要谎报 0）。
        return sent;
      }
    }
  }

  /**
   * @brief **实验性**批量接收：一次持锁从缓冲取走最多 `max_count` 条。
   *
   * ## 语义
   * - 先按单条路径的规则把等待中的发送者补进缓冲（保持 FIFO），再从缓冲取；
   * - 缓冲空即停止；不 park、不处理 context/deadline（非阻塞）；
   * - 要求 `T` 可移动赋值，否则返回 0（API 仍可实例化）。
   *
   * @return 实际取走条数；0 表示当前没有可取的值。
   */
  std::size_t TryRecvBatch(T* values, std::size_t max_count) {
    if constexpr (!std::is_move_assignable<T>::value) {
      return 0;
    } else {
      if (values == nullptr || max_count == 0) {
        return 0;
      }
      std::size_t received = 0;
      try {
        std::unique_lock<detail::ChannelSpinLock> lock(m_lock);
        std::optional<T> staged;
        while (received < max_count) {
          if (!m_buffer.TryPopInto(&staged)) {
            // 每轮补一个等待中的发送者，保持与单条 Recv 相同的 FIFO 顺序。
            RefillBufferLocked();
            if (!m_buffer.TryPopInto(&staged)) {
              break;
            }
          }
          values[received] = std::move(*staged);
          staged.reset();
          ++received;
        }
        if (received == 0) {
          return 0;
        }
        const bool published = PublishChangeLocked();
        lock.unlock();
        if (published) {
          NotifyChange();
        }
        return received;
      } catch (...) {
        // 已取出的条目已经交给调用方，如实返回已提交数量。
        return received;
      }
    }
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
      // 非阻塞路径不取时间戳：clock_gettime 在空探测热路径上要 ~20ns，
      // 而 nonblocking 的语义只需要「无法立即完成就返回」，不需要 deadline。
      auto result = SendUntil(std::move(value), std::nullopt, {}, true);
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
      // 同 TrySend：非阻塞接收不需要真实 deadline，省掉 clock_gettime。
      auto result = RecvUntil(std::nullopt, {}, true);
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
    bool published = false;
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      if (m_closed) {
        return {ChannelStatus::kAlreadyClosed, ChannelAlreadyClosedError()};
      }
      m_closed = true;
      m_generation.fetch_add(1, std::memory_order_release);
      published = true;
      while (PendingSend* sender = m_senders.pop_front()) {
        if (sender->IsSelect()) {
          SelectProbe probe;
          probe.ready = true;
          probe.status = ChannelStatus::kClosed;
          probe.error = ChannelClosedError();
          try {
            (void)sender->select_state->TrySelect(sender->select_index,
                                                  std::move(probe));
          } catch (...) {
            // Close 仍须唤醒该 select；选择结果不可提交时把节点取消掉，
            // 避免异常穿过 Close 并留下永远等待的 Fiber。
            sender->select_state->Cancel();
          }
          sender->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kClosed),
              std::memory_order_release);
          ReleaseSelectNode(sender);
          continue;
        }
        sender->closed = true;
        sender->status = ChannelStatus::kClosed;
        sender->error = ChannelClosedError();
        sender->signal.store(
            static_cast<std::uint32_t>(detail::WaitSignal::kClosed),
            std::memory_order_release);
        WakeNodeLocked(sender);
      }
      while (PendingRecv* receiver = m_receivers.pop_front()) {
        if (receiver->IsSelect()) {
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
          receiver->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kClosed),
              std::memory_order_release);
          ReleaseSelectNode(receiver);
          continue;
        }
        receiver->ready = true;
        receiver->ok = false;
        receiver->status = ChannelStatus::kClosed;
        receiver->error = ChannelClosedError();
        receiver->signal.store(
            static_cast<std::uint32_t>(detail::WaitSignal::kClosed),
            std::memory_order_release);
        WakeNodeLocked(receiver);
      }
    }
    if (published) {
      NotifyChange();
    }
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
    std::size_t woken = 0;
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      for (detail::WaitNodeBase* node = m_senders.head(); node != nullptr;
           node = node->next) {
        if (node->select_state != nullptr) {
          continue;
        }
        // 只推进节点自己的 futex 字；等待者醒来重检状态后会继续等待。
        node->seq.fetch_add(1, std::memory_order_release);
        WakeNodeLocked(node);
        ++woken;
      }
      for (detail::WaitNodeBase* node = m_receivers.head(); node != nullptr;
           node = node->next) {
        if (node->select_state != nullptr) {
          continue;
        }
        node->seq.fetch_add(1, std::memory_order_release);
        WakeNodeLocked(node);
        ++woken;
      }
    }
    if (m_change_waiters.load(std::memory_order_relaxed) != 0) {
      std::lock_guard<std::mutex> lock(m_change_mutex);
      m_change_cv.notify_all();
    }
    return woken;
  }

  // 返回单调递增代际；每次发送、接收或关闭改变可用性时递增，供外部
  // select 实现判断是否需要重新探测。
  std::uint64_t Generation() const noexcept {
    return m_generation.load(std::memory_order_acquire);
  }

  // 等待 m_generation 变化。观察者计数用于让热路径的 send/recv 在没有
  // 观察者时既不递增代际、也不碰 ParkingCondition：等待者持 m_mutex 先
  // 递增计数，再检查 generation；通知者持 m_mutex 先递增 generation，
  // 解锁后读计数。两者由同一把锁排序，因此不会漏掉已注册的观察者，
  // 也不会把 generation 变化漏给已经在等待的调用者。
  bool WaitForChange(std::uint64_t generation, Duration timeout) const {
    // 注册（Channel 自旋锁内）与等待（m_change_mutex + ParkingCondition）分开：
    // 自旋锁绝不能在持有时 park。注册时先检查代际，闭合「通知先于注册」的
    // 窗口；通知方在自旋锁内推进代际、解锁后再拿 m_change_mutex 广播，因此
    // 谓词检查与 notify 之间不会丢唤醒。
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      m_change_waiters.fetch_add(1, std::memory_order_relaxed);
      m_generation_watchers.fetch_add(1, std::memory_order_relaxed);
      if (m_generation.load(std::memory_order_acquire) != generation) {
        m_change_waiters.fetch_sub(1, std::memory_order_relaxed);
        m_generation_watchers.fetch_sub(1, std::memory_order_relaxed);
        return true;
      }
    }
    bool changed = false;
    {
      std::unique_lock<std::mutex> lock(m_change_mutex);
      changed = m_change_cv.wait_for(lock, timeout, [this, generation] {
        return m_generation.load(std::memory_order_acquire) != generation;
      });
    }
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      m_change_waiters.fetch_sub(1, std::memory_order_relaxed);
      m_generation_watchers.fetch_sub(1, std::memory_order_relaxed);
    }
    return changed;
  }

  // ---- select 统一加锁（对齐 Go selectgo 的 sellock / selunlock） ----

  /// @brief 返回 select 锁集的排序键。channel 对象地址在其生命周期内不变，
  ///        足以作为全序键；同一 channel 在一个 select 中出现多次时由
  ///        SelectLockSet 去重（ChannelSpinLock 不可重入）。
  const void* SelectLockKey() const noexcept { return this; }

  /// @brief 按 lockorder 加锁。调用方必须已按 SelectLockKey 排序后调用。
  void SelectLock() const noexcept { m_lock.lock(); }

  /// @brief 按 lockorder 逆序解锁。
  void SelectUnlock() const noexcept { m_lock.unlock(); }

  /// @brief arm/disarm 在锁内推进 generation 后，由 Select 在锁外调用。
  void NotifySelectChange() noexcept { NotifyChange(); }

  // 注册 select 接收 case（单 case 加锁的兼容包装）。
  // 新代码应使用 SelectLockSet 一次性持有整组锁，然后调用
  // ArmSelectRecvLocked —— 逐 case 加锁/解锁无法保证 arm 阶段的原子性。
  bool ArmSelectRecv(const std::shared_ptr<detail::SelectWaitState>& state,
                     std::size_t index) {
    if (!state || state->IsSelected()) {
      return false;
    }
    bool armed = false;
    bool notify = false;
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      armed = ArmSelectRecvLocked(state, index, &notify);
    }
    if (notify) {
      NotifyChange();
    }
    return armed;
  }

  // 注册 select 接收 case 的无锁版本。
  //
  // 持有通道锁时再次检查就绪，避免发送者在探测与发布等待节点之间插入。
  //
  // @pre 调用方已按 SelectLockKey 全序持有本 channel 的锁（见 SelectLockSet）。
  // @param notify 输出：本调用推进了代际，调用方必须在解锁后 NotifyChange()。
  bool ArmSelectRecvLocked(
      const std::shared_ptr<detail::SelectWaitState>& state, std::size_t index,
      bool* notify) {
    if (notify == nullptr) {
      return false;
    }
    *notify = false;
    if (!state || state->IsSelected()) {
      return false;
    }
    // 先锁后验：锁外看起来未选中，持锁后仍需重查（防 ABA / 迟到选择）。
    if (state->IsSelected()) {
      return false;
    }

    std::optional<T> buffered;
    if (m_buffer.TryPopInto(&buffered)) {
      if constexpr (!std::is_copy_constructible<T>::value) {
        m_buffer.PushBackFront(std::move(buffered));
        return false;
      } else {
        SelectProbe probe;
        probe.ready = true;
        probe.ok = true;
        probe.status = ChannelStatus::kReady;
        if (!probe.SetValue(*buffered)) {
          m_buffer.PushBackFront(std::move(buffered));
          return false;
        }
        if (!state->TrySelect(index, std::move(probe))) {
          // 认领失败：把值放回队首（下一个消费者优先取到它），保持 FIFO。
          m_buffer.PushBackFront(std::move(buffered));
          return false;
        }
        RefillBufferLocked();
        m_generation.fetch_add(1, std::memory_order_release);
        *notify = true;
        return true;
      }
    }

    const std::size_t sender_scan_limit = m_senders.size();
    for (std::size_t scanned = 0;
         scanned < sender_scan_limit && !m_senders.empty(); ++scanned) {
      PendingSend* sender = m_senders.front();
      if (sender->select_state == state) {
        m_senders.pop_front();
        m_senders.push_back(sender);
        continue;
      }
      if (sender->signal.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(detail::WaitSignal::kWaiting)) {
        // 已被取消/超时的节点：从队列摘除。普通节点由等待者自己归还池，
        // select 节点没有外部持有者，这里归还。
        m_senders.pop_front();
        if (sender->IsSelect()) {
          ReleaseSelectNode(sender);
        }
        continue;
      }
      if (sender->IsSelect()) {
        if constexpr (!std::is_copy_constructible<T>::value) {
          m_senders.pop_front();
          ReleaseSelectNode(sender);
          continue;
        } else {
          SelectProbe recv_probe;
          recv_probe.ready = true;
          recv_probe.ok = true;
          recv_probe.status = ChannelStatus::kReady;
          if (!recv_probe.SetValue(*sender->value)) {
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
              m_senders.pop_front();
              ReleaseSelectNode(sender);
              continue;
            }
            continue;
          }
          m_senders.pop_front();
          ReleaseSelectNode(sender);
          m_generation.fetch_add(1, std::memory_order_release);
          *notify = true;
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
        if (!probe.SetValue(*sender->value)) {
          return false;
        }
        if (!state->TrySelect(index, std::move(probe))) {
          return false;
        }
        m_senders.pop_front();
        sender->accepted = true;
        sender->status = ChannelStatus::kReady;
        sender->signal.store(
            static_cast<std::uint32_t>(detail::WaitSignal::kReady),
            std::memory_order_release);
        WakeNodeLocked(sender);
        m_generation.fetch_add(1, std::memory_order_release);
        *notify = true;
        return true;
      }
    }

    if (m_closed) {
      SelectProbe probe;
      probe.ready = true;
      probe.status = ChannelStatus::kClosed;
      probe.error = ChannelClosedError();
      const bool selected = state->TrySelect(index, std::move(probe));
      *notify = selected;
      return selected;
    }

    PendingRecv* pending = AcquireRecvNode();
    pending->select_state = state;
    pending->select_index = index;
    pending->generation = NextWaiterGeneration();
    m_receivers.push_back(pending);
    m_generation.fetch_add(1, std::memory_order_release);
    *notify = true;
    return true;
  }

  // 注册 select 发送 case（单 case 加锁的兼容包装）。SendCase 按约定要求
  // 值可复制，使 arm 失败时值仍保留到其他 case 被选中。
  bool ArmSelectSend(const std::shared_ptr<detail::SelectWaitState>& state,
                     std::size_t index, const T& value) {
    if (!state || state->IsSelected()) {
      return false;
    }
    bool armed = false;
    bool notify = false;
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      armed = ArmSelectSendLocked(state, index, value, &notify);
    }
    if (notify) {
      NotifyChange();
    }
    return armed;
  }

  // 注册 select 发送 case 的无锁版本。
  //
  // @pre 调用方已按 SelectLockKey 全序持有本 channel 的锁（见 SelectLockSet）。
  // @param notify 输出：本调用推进了代际，调用方必须在解锁后 NotifyChange()。
  bool ArmSelectSendLocked(const std::shared_ptr<detail::SelectWaitState>& state,
                           std::size_t index, const T& value, bool* notify) {
    if (notify == nullptr) {
      return false;
    }
    *notify = false;
    if (!state || state->IsSelected()) {
      return false;
    }
    // 先锁后验：锁外看起来未选中，持锁后仍需重查（防 ABA / 迟到选择）。
    if (state->IsSelected()) {
      return false;
    }

    const std::size_t receiver_scan_limit = m_receivers.size();
    for (std::size_t scanned = 0;
         scanned < receiver_scan_limit && !m_receivers.empty(); ++scanned) {
      PendingRecv* receiver = m_receivers.front();
      if (receiver->select_state == state) {
        m_receivers.pop_front();
        m_receivers.push_back(receiver);
        continue;
      }
      if (receiver->signal.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(detail::WaitSignal::kWaiting)) {
        m_receivers.pop_front();
        if (receiver->IsSelect()) {
          ReleaseSelectNode(receiver);
        }
        continue;
      }
      SelectProbe send_probe;
      send_probe.ready = true;
      send_probe.ok = true;
      send_probe.status = ChannelStatus::kReady;
      if (receiver->IsSelect()) {
        if constexpr (!std::is_copy_constructible<T>::value) {
          m_receivers.pop_front();
          ReleaseSelectNode(receiver);
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
              m_receivers.pop_front();
              ReleaseSelectNode(receiver);
              continue;
            }
            continue;
          }
          m_receivers.pop_front();
          ReleaseSelectNode(receiver);
          m_generation.fetch_add(1, std::memory_order_release);
          *notify = true;
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
      receiver->signal.store(
          static_cast<std::uint32_t>(detail::WaitSignal::kReady),
          std::memory_order_release);
      WakeNodeLocked(receiver);
      m_generation.fetch_add(1, std::memory_order_release);
      *notify = true;
      return true;
    }

    if (m_closed) {
      SelectProbe probe;
      probe.ready = true;
      probe.status = ChannelStatus::kClosed;
      probe.error = ChannelClosedError();
      const bool selected = state->TrySelect(index, std::move(probe));
      *notify = selected;
      return selected;
    }

    if (m_buffer.TryPush(value)) {
      SelectProbe probe;
      probe.ready = true;
      probe.ok = true;
      probe.status = ChannelStatus::kReady;
      bool selected = false;
      try {
        selected = state->TrySelect(index, std::move(probe));
      } catch (...) {
        (void)m_buffer.TryPopBack();
        return false;
      }
      if (!selected) {
        (void)m_buffer.TryPopBack();
        return false;
      }
      m_generation.fetch_add(1, std::memory_order_release);
      *notify = true;
      return true;
    }

    PendingSend* pending = AcquireSendNode();
    try {
      pending->value.emplace(value);
    } catch (...) {
      ReleaseSendNode(pending);
      return false;
    }
    pending->select_state = state;
    pending->select_index = index;
    pending->generation = NextWaiterGeneration();
    m_senders.push_back(pending);
    m_generation.fetch_add(1, std::memory_order_release);
    *notify = true;
    return true;
  }

  // 撤销 select 注册的节点（单 case 加锁的兼容包装）。
  void DisarmSelect(const std::shared_ptr<detail::SelectWaitState>& state) {
    if (!state) {
      return;
    }
    bool changed = false;
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      changed = DisarmSelectLocked(state);
    }
    if (changed) {
      NotifyChange();
    }
  }

  // 撤销 select 注册的节点的无锁版本。
  //
  // @pre 调用方已按 SelectLockKey 全序持有本 channel 的锁（见 SelectLockSet）。
  // @return true 表示移除过等待节点（调用方需在解锁后 NotifyChange）。
  bool DisarmSelectLocked(
      const std::shared_ptr<detail::SelectWaitState>& state) {
    if (!state) {
      return false;
    }
    bool changed = false;
    {
      detail::WaitNodeBase* node = m_senders.head();
      while (node != nullptr) {
        detail::WaitNodeBase* next = node->next;
        if (node->select_state.get() == state.get()) {
          m_senders.erase(static_cast<PendingSend*>(node));
          ReleaseSelectNode(static_cast<PendingSend*>(node));
          changed = true;
        }
        node = next;
      }
      node = m_receivers.head();
      while (node != nullptr) {
        detail::WaitNodeBase* next = node->next;
        if (node->select_state.get() == state.get()) {
          m_receivers.erase(static_cast<PendingRecv*>(node));
          ReleaseSelectNode(static_cast<PendingRecv*>(node));
          changed = true;
        }
        node = next;
      }
      if (changed) {
        m_generation.fetch_add(1, std::memory_order_release);
      }
    }
    return changed;
  }

 private:
  /**
   * @brief 一个被登记的发送者（无缓冲、或缓冲已满时挂在 `m_senders`）。
   *
   * ## 与 P0 前的区别
   *
   * - 不再有 `core::ParkingCondition cv`：唤醒直接推进节点自己的 futex 字；
   * - 不再是 `shared_ptr`：节点由等待者线程从 `WaitNodePool` 取还，Channel
   *   队列只持裸指针；取消回调按（地址, generation）定位，不持有节点；
   * - 结果标志（`accepted`/`closed`/`status`/`error`）仍由 Channel 自旋锁
   *   保护，`signal` 原子字只负责「可以醒了」的唤醒协议，两者都按
   *   「先写锁内标志、再发布 signal、最后唤醒」的固定顺序写入。
   *
   * ## 不变量
   *
   * - `accepted == true` 时 `value` 已被移走（接收者持有），不要再读；
   * - `accepted`/`closed` 至多一个为真；两者都为假时 `status` 说明取消/超时。
   */
  struct PendingSend final : detail::WaitNodeBase {
    /// @brief 待发送的值；被接收者取走后为空。
    std::optional<T> value;
    /// @brief 是否已成功交接给接收者（与 `closed` 互斥）。
    bool accepted{false};
    /// @brief 是否因 channel 关闭而失败（对应 `kClosed`/`kAlreadyClosed`）。
    bool closed{false};
    ChannelStatus status{ChannelStatus::kWouldBlock};
    ErrorPtr error;

    /// @brief 归还池前恢复「新建」状态（同时释放残留值）。
    void Reset() noexcept {
      ResetBase();
      value.reset();
      accepted = false;
      closed = false;
      status = ChannelStatus::kWouldBlock;
      error.reset();
    }
  };

  /**
   * @brief 一个被登记的接收者（无缓冲、或缓冲已空时挂在 `m_receivers`）。
   *
   * 与 `PendingSend` 对称：`ready` ↔ `accepted`；两者都由 Channel 自旋锁
   * 保护，唤醒协议走同一个 `signal`/`seq` 原子字。`ready && ok` 时
   * `value` 必有值；`ok == false` 时 `value` 为空（关闭）。
   */
  struct PendingRecv final : detail::WaitNodeBase {
    /// @brief 接收到的值；`ready == false` 时为空。
    std::optional<T> value;
    /// @brief 是否已有值可取（与取消/超时互斥）。
    bool ready{false};
    bool ok{false};
    ChannelStatus status{ChannelStatus::kWouldBlock};
    ErrorPtr error;

    /// @brief 归还池前恢复「新建」状态（同时释放残留值）。
    void Reset() noexcept {
      ResetBase();
      value.reset();
      ready = false;
      ok = false;
      status = ChannelStatus::kWouldBlock;
      error.reset();
    }
  };

  // -------------------------------------------------------------------------
  // 节点池 / 唤醒 / 取消 / 等待辅助
  // -------------------------------------------------------------------------

  static PendingSend* AcquireSendNode() {
    return detail::WaitNodePool<PendingSend>::Acquire();
  }
  static PendingRecv* AcquireRecvNode() {
    return detail::WaitNodePool<PendingRecv>::Acquire();
  }
  static void ReleaseSendNode(PendingSend* node) noexcept {
    detail::WaitNodePool<PendingSend>::Release(node);
  }
  static void ReleaseRecvNode(PendingRecv* node) noexcept {
    detail::WaitNodePool<PendingRecv>::Release(node);
  }
  /// @brief select 节点没有等待者栈帧持有，交接/摘除方负责归还。
  static void ReleaseSelectNode(PendingSend* node) noexcept {
    detail::WaitNodePool<PendingSend>::Release(node);
  }
  static void ReleaseSelectNode(PendingRecv* node) noexcept {
    detail::WaitNodePool<PendingRecv>::Release(node);
  }

  /// @brief 等待节点身份（每次从池取出递增，地址复用时必然变化）。
  static std::uint32_t NextWaiterGeneration() noexcept {
    static thread_local std::uint32_t generation = 0;
    return ++generation;
  }

  /// @brief 持 Channel 锁调用：推进目标节点自己的 futex 字并定向唤醒。
  /// @note 调用前必须已经在锁内写完结果标志并发布 `signal`。
  static void WakeNodeLocked(detail::WaitNodeBase* node) noexcept {
    node->seq.fetch_add(1, std::memory_order_release);
    if (node->managed) {
      // 受管 Fiber：交给 Scheduler；同样在锁内完成，节点在解锁后不再被
      // 访问，因此不需要额外的 wake gate/refcount。
      detail::ChannelWakeManaged(node);
      return;
    }
    if (node->parked.load(std::memory_order_relaxed) != 0) {
      detail::ChannelFutexWake(&node->seq);
    }
  }

  /**
   * @brief 取消回调入口：按（地址, generation）定位节点并唤醒。
   *
   * 回调只持 `weak_ptr<CancelHub>`；这里在 Channel 锁内定位，节点若已
   * 不在队列（交接完成/等待者已退出）就直接返回，绝不访问队列外节点。
   */
  void CancelWaiter(std::uint64_t address, std::uint32_t generation) noexcept {
    std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
    if (CancelOneLocked(m_senders.head(), address, generation)) {
      return;
    }
    (void)CancelOneLocked(m_receivers.head(), address, generation);
  }

  static bool CancelOneLocked(detail::WaitNodeBase* node,
                              std::uint64_t address,
                              std::uint32_t generation) noexcept {
    for (; node != nullptr; node = node->next) {
      if (reinterpret_cast<std::uint64_t>(node) != address ||
          node->generation != generation) {
        continue;
      }
      std::uint32_t expected =
          static_cast<std::uint32_t>(detail::WaitSignal::kWaiting);
      if (node->signal.compare_exchange_strong(
              expected,
              static_cast<std::uint32_t>(detail::WaitSignal::kCancelled),
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        WakeNodeLocked(node);
      }
      return true;
    }
    return false;
  }

  /// @brief 注册「到期/取消时唤醒该节点」的 Context 回调。
  ///
  /// @warning 必须在**不持有 Channel 锁**时调用：Context 已取消时
  ///          `AddCallback` 会同步执行回调，而回调会走 hub.mutex → Channel
  ///          锁；自旋锁不可重入，持锁注册会立刻自锁。
  /// @return 回调 id；无 context 时为 0。分配失败向上抛，由调用方清理。
  DoneSignal::CallbackId AddCancelCallback(const ContextPtr& context,
                                           detail::WaitNodeBase* node) {
    if (!context) {
      return 0;
    }
    std::weak_ptr<detail::CancelHub<Channel<T>>> weak_hub = m_cancel_hub;
    const std::uint64_t address = reinterpret_cast<std::uint64_t>(node);
    const std::uint32_t generation = node->generation;
    return context->Done().AddCallback([weak_hub, address, generation] {
      if (auto hub = weak_hub.lock()) {
        std::lock_guard<std::mutex> gate(hub->mutex);
        if (hub->owner != nullptr) {
          hub->owner->CancelWaiter(address, generation);
        }
      }
    });
  }

  /// @brief 等待节点被唤醒/取消/超时。返回时 Channel 锁未持有。
  void WaitOnNode(detail::WaitNodeBase* node,
                  const std::optional<TimePoint>& deadline) {
    if (!node->managed) {
      detail::ChannelWaitNative(node, deadline);
      return;
    }
    // 受管 Fiber 的超时必须由 Timer 唤醒（park 期间不能依赖 futex 超时）。
    core::Timer timer;
    if (deadline.has_value()) {
      std::weak_ptr<detail::CancelHub<Channel<T>>> weak_hub = m_cancel_hub;
      const std::uint64_t address = reinterpret_cast<std::uint64_t>(node);
      const std::uint32_t generation = node->generation;
      try {
        timer = core::TimerService::Default().Schedule(
            *deadline, [weak_hub, address, generation] {
              if (auto hub = weak_hub.lock()) {
                std::lock_guard<std::mutex> gate(hub->mutex);
                if (hub->owner != nullptr) {
                  hub->owner->CancelWaiter(address, generation);
                }
              }
            });
      } catch (...) {
        // 定时器服务不可用：退化为只依赖调度器的取消/交接唤醒。
      }
    }
    for (;;) {
      if (node->signal.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(detail::WaitSignal::kWaiting)) {
        break;
      }
      if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
        break;
      }
      if (!detail::ChannelParkOnce(node)) {
        std::uint32_t expected =
            static_cast<std::uint32_t>(detail::WaitSignal::kWaiting);
        (void)node->signal.compare_exchange_strong(
            expected,
            static_cast<std::uint32_t>(detail::WaitSignal::kCancelled),
            std::memory_order_acq_rel, std::memory_order_acquire);
        break;
      }
    }
    timer.Cancel();
  }

#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
 public:
  // -------------------------------------------------------------------------
  // 跨进程迁移：P0 池化后暂不支持
  //
  // P0 把等待节点从 `shared_ptr<PendingRecv>`（堆节点 + 控制块）改为
  // 「thread_local 池 + 裸指针 + per-node futex」。迁移协议要求导出
  // shared_ptr 的（对象指针, 控制块指针）并在新进程重建引用计数，这与
  // 池化节点的所有权模型不兼容。这里保留接口但明确拒绝，避免给出语义
  // 不完整的近似实现；迁移需要等独立的节点所有权设计（P2 之后）。
  // -------------------------------------------------------------------------

  bool ExportWaitState(ChannelWaitState* out, std::string* error) override {
    if (out != nullptr) {
      *out = ChannelWaitState{};
    }
    if (error != nullptr) {
      *error = "P0 池化等待节点后不再支持跨进程导出（GO2CPP_MIGRATION_API）";
    }
    return false;
  }

  bool AdoptWaitState(const ChannelWaitState& state, ChannelWaitState* adopted,
                      std::string* error) override {
    (void)state;
    if (adopted != nullptr) {
      *adopted = ChannelWaitState{};
    }
    if (error != nullptr) {
      *error = "P0 池化等待节点后不再支持跨进程接纳（GO2CPP_MIGRATION_API）";
    }
    return false;
  }

  std::uint64_t MigrationObjectAddress() const noexcept override {
    return reinterpret_cast<std::uint64_t>(this);
  }

  std::uint64_t MigrationObjectSize() const noexcept override {
    return sizeof(Channel<T>);
  }

  std::uint64_t MigrationNodeSize() const noexcept override {
    return sizeof(PendingRecv);
  }

  void ReleaseMigrationHold() noexcept override {}

  bool CanAdoptMigrationWait(std::optional<TimePoint> deadline,
                             const ContextPtr& context) const noexcept {
    (void)deadline;
    (void)context;
    return false;
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
    if (m_change_waiters.load(std::memory_order_relaxed) == 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(m_change_mutex);
    m_change_cv.notify_all();
  }

  /**
   * @brief 快路径的状态变化发布（必须在持 Channel 自旋锁时调用）。
   *
   * ## 为什么需要门控
   *
   * 原实现每次 send/recv 都在锁内 `++m_generation`（一次性原子 RMW），再在
   * 锁外读 `m_change_waiters`。1P1C 场景下生产者和消费者交替写同一个
   * `m_generation` cacheline：**每条消息都把该 cacheline 在核间搬一次**
   * （实测这是 cap1024 1M 条从 50ns 退化到 140ns 的主因之一）。
   *
   * 这里改为：只有存在代际观察者（WaitForChange 或等待-自旋）时才写。
   * 观察者为 0 时快路径只剩一次 relaxed load，不产生任何跨核写。
   *
   * @return true 表示代际已推进，调用方应在解锁后调用 NotifyChange()。
   */
  bool PublishChangeLocked() noexcept {
    if (m_generation_watchers.load(std::memory_order_relaxed) == 0) {
      return false;
    }
    m_generation.fetch_add(1, std::memory_order_release);
    return true;
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
                              const ContextPtr& context, bool nonblocking,
                              bool allow_spin = true) {
    // step1: 上下文已取消/已超时，直接返回，不进入锁。
    if (const auto context_status = ContextSendStatus(context);
        context_status.status != ChannelStatus::kReady) {
      return context_status;
    }

    // step2: 全程只持 Channel 自旋锁；park 前一定解锁。
    std::unique_lock<detail::ChannelSpinLock> lock(m_lock);
    bool spin_attempted = false;
  retry_fast_path:
    if (m_closed) {
      return {ChannelStatus::kClosed, ChannelClosedError()};
    }

    // step3: 优先交接给等待中的接收者。等待队列是侵入式链表，摘链 O(1)；
    // 节点生命周期由等待者线程持有，交接方在锁内访问后立刻不再引用。
    while (!m_receivers.empty()) {
      PendingRecv* receiver = m_receivers.front();
      if (receiver->signal.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(detail::WaitSignal::kWaiting)) {
        // 已被取消/超时：取消回调不摘链，这里清理。普通节点由等待者归还池。
        m_receivers.pop_front();
        if (receiver->IsSelect()) {
          ReleaseSelectNode(receiver);
        }
        continue;
      }
      if (receiver->IsSelect()) {
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
          ReleaseSelectNode(receiver);
          if (!selected) {
            continue;
          }
          m_generation.fetch_add(1, std::memory_order_release);
          lock.unlock();
          NotifyChange();
          return {ChannelStatus::kReady, {}};
        } else {
          m_receivers.pop_front();
          ReleaseSelectNode(receiver);
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
      receiver->ready = true;
      receiver->ok = true;
      receiver->status = ChannelStatus::kReady;
      receiver->signal.store(
          static_cast<std::uint32_t>(detail::WaitSignal::kReady),
          std::memory_order_release);
      WakeNodeLocked(receiver);
      const bool published = PublishChangeLocked();
      lock.unlock();
      if (published) {
        NotifyChange();
      }
      return {ChannelStatus::kReady, {}};
    }

    // step4: 没有接收者但有缓冲空位——入缓冲。
    if (m_buffer.size() < m_capacity) {
      m_buffer.emplace_back(std::move(value));
      const bool published = PublishChangeLocked();
      lock.unlock();
      if (published) {
        NotifyChange();
      }
      return {ChannelStatus::kReady, {}};
    }

    // step4.5: 缓冲满且是原生线程——先自旋等一个空位，而不是立刻 park。
    // 与 P0 前一致：只自旋一次，预算耗尽说明对端确实慢。
    if (!spin_attempted && allow_spin && m_capacity > 1 && m_capacity <= 256 &&
        !nonblocking && detail::CanSpinWait()) {
      spin_attempted = true;
      const std::uint64_t sequence =
          m_generation.load(std::memory_order_relaxed);
      m_generation_watchers.fetch_add(1, std::memory_order_acq_rel);
      lock.unlock();
      (void)detail::SpinForChange(m_generation, sequence);
      m_generation_watchers.fetch_sub(1, std::memory_order_acq_rel);
      lock.lock();
      // 自旋窗口内缓冲可能出现空位；必须重检快路径，不能带着空位直接 park。
      goto retry_fast_path;
    }

    if (nonblocking) {
      return {ChannelStatus::kWouldBlock, ChannelWouldBlockError()};
    }
    if (DeadlineReached(deadline)) {
      return {ChannelStatus::kTimedOut, ChannelTimeoutError()};
    }
    if (core::ParkingCondition::FiberWaitUnsupported()) {
      return {ChannelStatus::kInvalid, detail::ValueOperationError()};
    }

    // step5: 登记节点并挂 context 取消回调。节点从线程本地池取，稳态零分配。
    PendingSend* node = nullptr;
    try {
      node = AcquireSendNode();
      node->value.emplace(std::move(value));
    } catch (...) {
      if (node != nullptr) {
        ReleaseSendNode(node);
      }
      return {ChannelStatus::kInvalid, detail::ValueOperationError()};
    }
    node->generation = NextWaiterGeneration();
    const bool native_waiter = detail::CanSpinWait();
    if (!native_waiter && !detail::ChannelBindManaged(node)) {
      ReleaseSendNode(node);
      return {ChannelStatus::kInvalid, detail::ValueOperationError()};
    }
    m_senders.push_back(node);
    // 窗口闭合：注册取消回调期间释放过 Channel 锁，可能已有接收者入队或
    // 缓冲出现空位。如果此刻直接去睡，两个方向会各自在队列里互等（P0
    // 第一版的挂起根因）。这里在同一临界区内撤销入队并完成发送。
    if (!m_receivers.empty()) {
      while (!m_receivers.empty()) {
        PendingRecv* receiver = m_receivers.front();
        if (receiver->signal.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(detail::WaitSignal::kWaiting)) {
          m_receivers.pop_front();
          if (receiver->IsSelect()) {
            ReleaseSelectNode(receiver);
          }
          continue;
        }
        if (receiver->IsSelect()) {
          if constexpr (!std::is_copy_constructible<T>::value) {
            m_receivers.pop_front();
            ReleaseSelectNode(receiver);
            continue;
          } else {
            SelectProbe probe;
            probe.ready = true;
            probe.ok = true;
            probe.status = ChannelStatus::kReady;
            bool value_ok = false;
            try {
              value_ok = probe.SetValue(*node->value);
            } catch (...) {
              m_senders.erase(node);
              ReleaseSendNode(node);
              return {ChannelStatus::kInvalid, detail::ValueOperationError()};
            }
            if (!value_ok) {
              m_senders.erase(node);
              ReleaseSendNode(node);
              return {ChannelStatus::kInvalid, detail::ValueOperationError()};
            }
            bool selected = false;
            try {
              selected = receiver->select_state->TrySelect(
                  receiver->select_index, std::move(probe));
            } catch (...) {
              m_senders.erase(node);
              ReleaseSendNode(node);
              return {ChannelStatus::kInvalid, detail::ValueOperationError()};
            }
            m_receivers.pop_front();
            ReleaseSelectNode(receiver);
            if (!selected) {
              continue;
            }
            m_senders.erase(node);
            ReleaseSendNode(node);
            m_generation.fetch_add(1, std::memory_order_release);
            lock.unlock();
            NotifyChange();
            return {ChannelStatus::kReady, {}};
          }
        }
        try {
          receiver->value.emplace(std::move(*node->value));
        } catch (...) {
          m_senders.erase(node);
          ReleaseSendNode(node);
          return {ChannelStatus::kInvalid, detail::ValueOperationError()};
        }
        m_receivers.pop_front();
        receiver->ready = true;
        receiver->ok = true;
        receiver->status = ChannelStatus::kReady;
        receiver->signal.store(
            static_cast<std::uint32_t>(detail::WaitSignal::kReady),
            std::memory_order_release);
        WakeNodeLocked(receiver);
        m_senders.erase(node);
        ReleaseSendNode(node);
        const bool published = PublishChangeLocked();
        lock.unlock();
        if (published) {
          NotifyChange();
        }
        return {ChannelStatus::kReady, {}};
      }
    }
    if (m_buffer.size() < m_capacity) {
      m_buffer.emplace_back(std::move(*node->value));
      m_senders.erase(node);
      ReleaseSendNode(node);
      const bool published = PublishChangeLocked();
      lock.unlock();
      if (published) {
        NotifyChange();
      }
      return {ChannelStatus::kReady, {}};
    }
    // 发送方即将 park：只在阻塞路径记录（缓冲命中/立即交接无开销）。
    GO2CPP_DEBUG_PARK_HINT(kChannelSend);
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    GO2CPP_DEBUG_RESOURCE_EVENT_EX(
        kChannelSend, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0,
        node->value.has_value() ? detail::ChannelDebugValue(*node->value) : 0,
        go2cpp::debug::ResourceResult::kBlocked,
        static_cast<std::uint32_t>(m_senders.size()), true,
        detail::ChannelDebugValueFlags<T>());
#else
    GO2CPP_DEBUG_EVENT(kChannelSend, 0, m_capacity);
#endif
    // 注册取消回调必须在不持 Channel 锁时进行：若 context 已取消，
    // AddCallback 会同步执行回调，而回调需要 Channel 锁（自旋锁不可重入）。
    CallbackGuard callback_guard{context};
    if (context) {
      lock.unlock();
      try {
        callback_guard.id = AddCancelCallback(context, node);
      } catch (...) {
        lock.lock();
        // 极端路径：注册失败时先看是否已经被交接/关闭，再决定清理。
        if (node->accepted) {
          lock.unlock();
          ReleaseSendNode(node);
          return {ChannelStatus::kReady, {}};
        }
        if (node->closed) {
          ErrorPtr error = node->error;
          lock.unlock();
          ReleaseSendNode(node);
          return {ChannelStatus::kClosed, error ? error : ChannelClosedError()};
        }
        if (node->linked) {
          node->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kCancelled),
              std::memory_order_release);
          m_senders.erase(node);
        }
        lock.unlock();
        ReleaseSendNode(node);
        return {ChannelStatus::kInvalid, detail::ValueOperationError()};
      }
      lock.lock();
    }

    // step6: 等待循环。每次醒来后在锁内重检 accepted/closed/context/deadline；
    // 都不成立就回到锁外睡眠（自旋 + futex / Scheduler park）。
    ChannelSendResult result{ChannelStatus::kWouldBlock, {}};
    bool moved_change = false;
    for (;;) {
      if (node->accepted) {
        result = {ChannelStatus::kReady, {}};
        break;
      }
      if (node->closed) {
        result = {ChannelStatus::kClosed,
                  node->error ? node->error : ChannelClosedError()};
        break;
      }

      ErrorPtr context_error;
      const auto context_status = ContextRecvStatus(context, &context_error);
      if (context_status != ChannelStatus::kReady ||
          core::ParkingCondition::CancellationRequested()) {
        node->status = context_status != ChannelStatus::kReady
                           ? context_status
                           : ChannelStatus::kCancelled;
        node->error = context_error ? context_error : CanceledError();
        if (node->linked) {
          node->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kCancelled),
              std::memory_order_release);
          m_senders.erase(node);
        }
        moved_change = true;
        result = {node->status, node->error};
        break;
      }
      if (DeadlineReached(deadline)) {
        node->status = ChannelStatus::kTimedOut;
        node->error = ChannelTimeoutError();
        if (node->linked) {
          node->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kCancelled),
              std::memory_order_release);
          m_senders.erase(node);
        }
        moved_change = true;
        result = {ChannelStatus::kTimedOut, node->error};
        break;
      }
      if (node->signal.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(detail::WaitSignal::kWaiting)) {
        // 取消回调已置位但 context/deadline 尚未可见（理论上不会发生）：
        // 按取消返回，绝不在已终止的节点上空转。
        node->status = ChannelStatus::kCancelled;
        node->error = CanceledError();
        if (node->linked) {
          m_senders.erase(node);
        }
        moved_change = true;
        result = {node->status, node->error};
        break;
      }

      lock.unlock();
      WaitOnNode(node, deadline);
      lock.lock();
    }

    // 收尾：兜底摘链（正常路径已在上面摘过或被交接方摘走）。
    if (node->linked) {
      node->signal.store(
          static_cast<std::uint32_t>(detail::WaitSignal::kCancelled),
          std::memory_order_release);
      m_senders.erase(node);
      moved_change = true;
    }
    if (moved_change) {
      m_generation.fetch_add(1, std::memory_order_release);
    }
    lock.unlock();
    if (moved_change) {
      NotifyChange();
    }
    ReleaseSendNode(node);
    return result;
  }

  ChannelRecvResult<T> RecvUntil(std::optional<TimePoint> deadline,
                                 const ContextPtr& context, bool nonblocking,
                                 bool allow_spin = true) {
    if (const auto context_status = ContextSendStatus(context);
        context_status.status != ChannelStatus::kReady) {
      return {std::nullopt, false, true, context_status.status,
              context_status.error};
    }

    std::unique_lock<detail::ChannelSpinLock> lock(m_lock);
    bool spin_attempted = false;
  retry_fast_path:
    if (!m_buffer.empty()) {
      // Channel<T> 要求值类型移动构造不抛异常，因此提取队首值不会因异常
      // 留下损坏的缓冲元素。
      T value = std::move(m_buffer.front());
      m_buffer.pop_front();
      // 将一个被阻塞的发送者补入刚刚空出的缓冲槽位。
      RefillBufferLocked();
      const bool published = PublishChangeLocked();
      lock.unlock();
      if (published) {
        NotifyChange();
      }
      return {std::optional<T>(std::move(value)), true, true,
              ChannelStatus::kReady, {}};
    }

    // 无缓冲发送者可以直接与该接收者会合交接。
    while (!m_senders.empty()) {
      PendingSend* sender = m_senders.front();
      if (sender->signal.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(detail::WaitSignal::kWaiting)) {
        m_senders.pop_front();
        if (sender->IsSelect()) {
          ReleaseSelectNode(sender);
        }
        continue;
      }
      if (sender->IsSelect()) {
        // 在值可以传输前不能先认领已选中的发送者。对可能抛异常的值类型
        // 要求存在 noexcept 的复制/移动路径；否则保留发送者在队列中。
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
            m_senders.pop_front();
            ReleaseSelectNode(sender);
            continue;
          }
        }
      }
      if constexpr (std::is_nothrow_move_constructible<T>::value) {
        T value = std::move(*sender->value);
        m_senders.pop_front();
        if (sender->IsSelect()) {
          ReleaseSelectNode(sender);
        } else {
          sender->accepted = true;
          sender->status = ChannelStatus::kReady;
          sender->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kReady),
              std::memory_order_release);
          WakeNodeLocked(sender);
        }
        const bool published = PublishChangeLocked();
        lock.unlock();
        if (published) {
          NotifyChange();
        }
        return {std::optional<T>(std::move(value)), true, true,
                ChannelStatus::kReady, {}};
      } else {
        // 唯一剩余的可接受路径是 noexcept 复制。
        T value = *sender->value;
        m_senders.pop_front();
        if (sender->IsSelect()) {
          ReleaseSelectNode(sender);
        } else {
          sender->accepted = true;
          sender->status = ChannelStatus::kReady;
          sender->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kReady),
              std::memory_order_release);
          WakeNodeLocked(sender);
        }
        const bool published = PublishChangeLocked();
        lock.unlock();
        if (published) {
          NotifyChange();
        }
        return {std::optional<T>(value), true, true, ChannelStatus::kReady, {}};
      }
    }

    // 缓冲空且没有待交接的发送者：原生线程先自旋等数据。
    if (!spin_attempted && allow_spin && m_capacity > 1 && m_capacity <= 256 &&
        !nonblocking && detail::CanSpinWait()) {
      spin_attempted = true;
      const std::uint64_t sequence =
          m_generation.load(std::memory_order_relaxed);
      m_generation_watchers.fetch_add(1, std::memory_order_acq_rel);
      lock.unlock();
      (void)detail::SpinForChange(m_generation, sequence);
      m_generation_watchers.fetch_sub(1, std::memory_order_acq_rel);
      lock.lock();
      goto retry_fast_path;
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
    if (nonblocking) {
      return {std::nullopt, false, false, ChannelStatus::kWouldBlock,
              ChannelWouldBlockError()};
    }
    if (DeadlineReached(deadline)) {
      return {std::nullopt, false, false, ChannelStatus::kTimedOut,
              ChannelTimeoutError()};
    }
    if (core::ParkingCondition::FiberWaitUnsupported()) {
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }

    PendingRecv* node = nullptr;
    try {
      node = AcquireRecvNode();
    } catch (...) {
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }
    node->generation = NextWaiterGeneration();
    const bool native_waiter = detail::CanSpinWait();
    if (!native_waiter && !detail::ChannelBindManaged(node)) {
      ReleaseRecvNode(node);
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }
    m_receivers.push_back(node);
    // 窗口闭合：注册取消回调期间可能已有发送者入队（与 SendUntil 对称）。
    // 在同一临界区内撤销入队并完成接收，避免双向互等。
    while (!m_senders.empty()) {
      PendingSend* sender = m_senders.front();
      if (sender->signal.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(detail::WaitSignal::kWaiting)) {
        m_senders.pop_front();
        if (sender->IsSelect()) {
          ReleaseSelectNode(sender);
        }
        continue;
      }
      if (sender->IsSelect()) {
        if constexpr (!std::is_nothrow_move_constructible<T>::value &&
                      !std::is_nothrow_copy_constructible<T>::value) {
          m_receivers.erase(node);
          ReleaseRecvNode(node);
          return {std::nullopt, false, true, ChannelStatus::kInvalid,
                  detail::ValueOperationError()};
        } else {
          SelectProbe probe;
          probe.ready = true;
          probe.ok = true;
          probe.status = ChannelStatus::kReady;
          if (!sender->select_state->TrySelect(sender->select_index,
                                               std::move(probe))) {
            m_senders.pop_front();
            ReleaseSelectNode(sender);
            continue;
          }
        }
      }
      if constexpr (std::is_nothrow_move_constructible<T>::value) {
        T value = std::move(*sender->value);
        m_senders.pop_front();
        m_receivers.erase(node);
        ReleaseRecvNode(node);
        if (sender->IsSelect()) {
          ReleaseSelectNode(sender);
        } else {
          sender->accepted = true;
          sender->status = ChannelStatus::kReady;
          sender->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kReady),
              std::memory_order_release);
          WakeNodeLocked(sender);
        }
        const bool published = PublishChangeLocked();
        lock.unlock();
        if (published) {
          NotifyChange();
        }
        return {std::optional<T>(std::move(value)), true, true,
                ChannelStatus::kReady, {}};
      } else {
        T value = *sender->value;
        m_senders.pop_front();
        m_receivers.erase(node);
        ReleaseRecvNode(node);
        if (sender->IsSelect()) {
          ReleaseSelectNode(sender);
        } else {
          sender->accepted = true;
          sender->status = ChannelStatus::kReady;
          sender->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kReady),
              std::memory_order_release);
          WakeNodeLocked(sender);
        }
        const bool published = PublishChangeLocked();
        lock.unlock();
        if (published) {
          NotifyChange();
        }
        return {std::optional<T>(value), true, true, ChannelStatus::kReady, {}};
      }
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
    // 注册取消回调必须在不持 Channel 锁时进行（原因见 SendUntil）。
    CallbackGuard callback_guard{context};
    if (context) {
      lock.unlock();
      try {
        callback_guard.id = AddCancelCallback(context, node);
      } catch (...) {
        lock.lock();
        if (node->ready) {
          ChannelRecvResult<T> ready_result{std::move(node->value), node->ok,
                                            true, node->status, node->error};
          lock.unlock();
          ReleaseRecvNode(node);
          return ready_result;
        }
        if (node->linked) {
          node->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kCancelled),
              std::memory_order_release);
          m_receivers.erase(node);
        }
        lock.unlock();
        ReleaseRecvNode(node);
        return {std::nullopt, false, true, ChannelStatus::kInvalid,
                detail::ValueOperationError()};
      }
      lock.lock();
    }

    ChannelRecvResult<T> result{std::nullopt, false, true,
                                ChannelStatus::kWouldBlock, {}};
    bool moved_change = false;
    for (;;) {
      if (node->ready) {
        result = {std::move(node->value), node->ok, true, node->status,
                  node->error};
        break;
      }
      if (node->signal.load(std::memory_order_acquire) ==
          static_cast<std::uint32_t>(detail::WaitSignal::kClosed)) {
        result = {std::nullopt, false, true, ChannelStatus::kClosed,
                  ChannelClosedError()};
        break;
      }

      context_error.reset();
      const auto status = ContextRecvStatus(context, &context_error);
      if (status != ChannelStatus::kReady ||
          core::ParkingCondition::CancellationRequested()) {
        node->status = status != ChannelStatus::kReady
                           ? status
                           : ChannelStatus::kCancelled;
        node->error = context_error ? context_error : CanceledError();
        if (node->linked) {
          node->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kCancelled),
              std::memory_order_release);
          m_receivers.erase(node);
        }
        moved_change = true;
        result = {std::nullopt, false, true, node->status, node->error};
        break;
      }
      if (DeadlineReached(deadline)) {
        node->status = ChannelStatus::kTimedOut;
        node->error = ChannelTimeoutError();
        if (node->linked) {
          node->signal.store(
              static_cast<std::uint32_t>(detail::WaitSignal::kCancelled),
              std::memory_order_release);
          m_receivers.erase(node);
        }
        moved_change = true;
        result = {std::nullopt, false, false, ChannelStatus::kTimedOut,
                  node->error};
        break;
      }
      if (node->signal.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(detail::WaitSignal::kWaiting)) {
        node->status = ChannelStatus::kCancelled;
        node->error = CanceledError();
        if (node->linked) {
          m_receivers.erase(node);
        }
        moved_change = true;
        result = {std::nullopt, false, true, node->status, node->error};
        break;
      }

      lock.unlock();
      WaitOnNode(node, deadline);
      lock.lock();
    }

    if (node->linked) {
      node->signal.store(
          static_cast<std::uint32_t>(detail::WaitSignal::kCancelled),
          std::memory_order_release);
      m_receivers.erase(node);
      moved_change = true;
    }
    if (moved_change) {
      m_generation.fetch_add(1, std::memory_order_release);
    }
    lock.unlock();
    if (moved_change) {
      NotifyChange();
    }
    ReleaseRecvNode(node);
    return result;
  }

  void RefillBufferLocked() {
    while (!m_senders.empty()) {
      PendingSend* sender = m_senders.front();
      if (sender->signal.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(detail::WaitSignal::kWaiting)) {
        m_senders.pop_front();
        if (sender->IsSelect()) {
          ReleaseSelectNode(sender);
        }
        continue;
      }
      if (sender->IsSelect()) {
        if constexpr (!std::is_nothrow_move_constructible<T>::value &&
                      !std::is_nothrow_copy_constructible<T>::value) {
          // 不要认领一个无法在不冒缓冲区半提交风险的情况下传输值的 select。
          break;
        } else {
          bool pushed = false;
          if constexpr (std::is_nothrow_move_constructible<T>::value) {
            pushed = m_buffer.TryPush(std::move(*sender->value));
          } else {
            pushed = m_buffer.TryPush(*sender->value);
          }
          if (!pushed) {
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
          m_senders.pop_front();
          ReleaseSelectNode(sender);
          if (!selected) {
            (void)m_buffer.TryPopBack();
          }
          continue;
        }
      }
      bool pushed = false;
      if constexpr (std::is_nothrow_move_constructible<T>::value) {
        pushed = m_buffer.TryPush(std::move(*sender->value));
      } else {
        pushed = m_buffer.TryPush(*sender->value);
      }
      if (!pushed) {
        break;
      }
      m_senders.pop_front();
      sender->accepted = true;
      sender->status = ChannelStatus::kReady;
      sender->signal.store(static_cast<std::uint32_t>(detail::WaitSignal::kReady),
                           std::memory_order_release);
      WakeNodeLocked(sender);
      break;
    }
  }

  /// @brief 固定容量（创建后不变）；0 表示无缓冲 rendezvous。
  std::size_t m_capacity{0};
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
  /// @brief 资源级历史编号（构造时注册；OFF 构建不存在该成员）。
  go2cpp::debug::ResourceId m_debug_resource{0};
#endif
  /// @brief 保护 buffer/senders/receivers/closed；Send/Recv/Close/Select 全竞争它。
  ///        自旋锁只在极短临界区内持有，**绝不允许持锁 park**。
  mutable detail::ChannelSpinLock m_lock;
  /// @brief WaitForChange 的等待互斥量与条件变量（冷路径，独立于自旋锁）。
  mutable std::mutex m_change_mutex;
  /// @brief 状态变化通知；等待者在锁外 park 在它上面（仅 WaitForChange）。
  mutable core::ParkingCondition m_change_cv;
  /// @brief 正在 m_change_cv 上等待的线程/G 数量；为 0 时省掉一次 notify_all。
  mutable std::atomic<std::uint32_t> m_change_waiters{0};
  /// @brief 环形缓冲（cap==0 时不分配）；Send 入队、Recv 出队。
  detail::RingBuffer<T> m_buffer;
  /// @brief 已登记但尚未拿到接收者的发送者队列（裸指针，节点由等待者持有）。
  detail::WaitQueue<PendingSend> m_senders;
  /// @brief 已登记但尚未拿到值的接收者队列（裸指针，节点由等待者持有）。
  detail::WaitQueue<PendingRecv> m_receivers;
  /// @brief 已关闭标记；Close 后 Send 返回 kClosed，Recv 读完缓冲后返回 kClosed。
  bool m_closed{false};
  /// @brief 代际观察者计数（WaitForChange 观察者 + 等待-自旋者）。
  ///        为 0 时快路径完全不写 m_generation。
  mutable std::atomic<std::uint32_t> m_generation_watchers{0};
  /// @brief 关闭/状态代际（原子：Generation() 需要无锁读；热路径已门控不写）。
  std::atomic<std::uint64_t> m_generation{0};
  /// @brief 取消回调与 Channel 生命周期的解耦点（弱引用捕获）。
  std::shared_ptr<detail::CancelHub<Channel<T>>> m_cancel_hub;
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
  // 内建 channel case 的 select 锁集入口与无锁版 arm/disarm。
  // Select 主循环按 SelectLockKey 全序一次性持有整组锁（Go selectgo 的
  // sellock），再调用 arm_locked，使「重验就绪 + 登记等待节点」在锁集内
  // 原子完成；非 channel case（DoneCase/AfterCase）不参与 channel 锁序。
  detail::SelectLockEntry lock_entry;
  std::function<bool(const std::shared_ptr<detail::SelectWaitState>&,
                     std::size_t, bool*)>
      arm_locked;
  std::function<bool(const std::shared_ptr<detail::SelectWaitState>&)>
      disarm_locked;
  // 锁外通知：arm_locked/disarm_locked 推进 generation 后由 Select 调用。
  std::function<void()> notify_change;

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
      {},
      {},
      {},
      {},
      {}};
  result.arm_checked = [channel](
      const std::shared_ptr<detail::SelectWaitState>& state,
      std::size_t index) {
    return !channel || channel->ArmSelectRecv(state, index);
  };
  // 锁集入口：key 用 channel 对象地址，函数指针在 Channel<T> 上实例化。
  result.lock_entry = detail::SelectLockEntry{
      channel ? channel->SelectLockKey() : nullptr,
      [](const void* key) noexcept {
        static_cast<const Channel<T>*>(key)->SelectLock();
      },
      [](const void* key) noexcept {
        static_cast<const Channel<T>*>(key)->SelectUnlock();
      }};
  result.arm_locked = [channel](
                          const std::shared_ptr<detail::SelectWaitState>& state,
                          std::size_t index, bool* notify) {
    if (!channel) {
      return false;
    }
    return channel->ArmSelectRecvLocked(state, index, notify);
  };
  result.disarm_locked =
      [channel](const std::shared_ptr<detail::SelectWaitState>& state) {
        return channel ? channel->DisarmSelectLocked(state) : false;
      };
  result.notify_change = [channel] {
    if (channel) {
      channel->NotifySelectChange();
    }
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
      {},
      {},
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
  result.lock_entry = detail::SelectLockEntry{
      channel ? channel->SelectLockKey() : nullptr,
      [](const void* key) noexcept {
        static_cast<const Channel<T>*>(key)->SelectLock();
      },
      [](const void* key) noexcept {
        static_cast<const Channel<T>*>(key)->SelectUnlock();
      }};
  result.arm_locked = [channel, shared_value](
                          const std::shared_ptr<detail::SelectWaitState>& state,
                          std::size_t index, bool* notify) {
    if constexpr (std::is_copy_constructible<T>::value) {
      if (!channel) {
        return false;
      }
      return channel->ArmSelectSendLocked(state, index, *shared_value, notify);
    }
    return true;
  };
  result.disarm_locked =
      [channel](const std::shared_ptr<detail::SelectWaitState>& state) {
        return channel ? channel->DisarmSelectLocked(state) : false;
      };
  result.notify_change = [channel] {
    if (channel) {
      channel->NotifySelectChange();
    }
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
      },
      {},
      {},
      {},
      {}};
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
      {},
      {},
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
