#pragma once

#include "go2cpp/context.hpp"
#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/error.hpp"

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace go2cpp {

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

inline bool ChannelStatusIsReady(ChannelStatus status) noexcept {
  return status == ChannelStatus::kReady || status == ChannelStatus::kClosed;
}

ErrorPtr ChannelClosedError() noexcept;
ErrorPtr ChannelAlreadyClosedError() noexcept;
ErrorPtr ChannelWouldBlockError() noexcept;
ErrorPtr ChannelNilError() noexcept;
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
template <typename T>
class Channel final : public std::enable_shared_from_this<Channel<T>> {
  static_assert(std::is_nothrow_move_constructible<T>::value,
                "Channel<T> requires a nothrow move constructor");
  static_assert(std::is_nothrow_destructible<T>::value,
                "Channel<T> requires a nothrow destructor");

 public:
  using Ptr = std::shared_ptr<Channel<T>>;
  using Duration = ContextDuration;
  using TimePoint = ContextTimePoint;

  // 创建容量为 capacity 的通道；capacity==0 表示无缓冲。
  static Ptr Create(std::size_t capacity = 0) {
    return std::make_shared<Channel<T>>(capacity);
  }

  explicit Channel(std::size_t capacity = 0) : m_capacity(capacity) {}
  ~Channel() noexcept { (void)Close(); }

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  // 返回固定容量（不含当前缓冲长度）。
  std::size_t Capacity() const noexcept { return m_capacity; }
  // 返回当前缓冲区长度；不包含正在交接的等待者。
  std::size_t Len() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffer.size();
  }
  // 查询通道是否已关闭。
  bool IsClosed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
  }
  bool closed() const { return IsClosed(); }

  // 发送 value；阻塞直到接收者/缓冲可用、关闭或 context 取消。
  ChannelSendResult Send(T value, const ContextPtr& context = {}) {
    try {
      return SendUntil(std::move(value), std::nullopt, context, false);
    } catch (...) {
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
    try {
      return SendUntil(std::move(value), std::chrono::steady_clock::now(), {}, true);
    } catch (...) {
      return {ChannelStatus::kInvalid, detail::ValueOperationError()};
    }
  }

  // 接收一个值；关闭后返回 kClosed 与零值/空 optional。
  ChannelRecvResult<T> Recv(const ContextPtr& context = {}) {
    try {
      return RecvUntil(std::nullopt, context, false);
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
      return RecvUntil(std::chrono::steady_clock::now(), {}, true);
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
      sender->cv.notify_one();
    }
    for (auto& receiver : receivers) {
      receiver->cv.notify_one();
    }
    m_change_cv.notify_all();
    return {ChannelStatus::kReady, {}};
  }

  // 返回单调递增代际；每次发送、接收或关闭改变可用性时递增，供外部
  // select 实现判断是否需要重新探测。
  std::uint64_t Generation() const noexcept {
    return m_generation.load(std::memory_order_acquire);
  }

  bool WaitForChange(std::uint64_t generation, Duration timeout) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_change_cv.wait_for(lock, timeout, [this, generation] {
      return m_generation.load(std::memory_order_relaxed) != generation;
    });
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
        m_change_cv.notify_all();
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
          m_change_cv.notify_all();
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
        sender->cv.notify_one();
        ++m_generation;
        lock.unlock();
        m_change_cv.notify_all();
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
        m_change_cv.notify_all();
      }
      return selected;
    }

    auto pending = std::make_shared<PendingRecv>();
    pending->select_state = state;
    pending->select_index = index;
    m_receivers.emplace_back(std::move(pending));
    ++m_generation;
    lock.unlock();
    m_change_cv.notify_all();
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
          m_change_cv.notify_all();
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
      receiver->cv.notify_one();
      ++m_generation;
      lock.unlock();
      m_change_cv.notify_all();
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
        m_change_cv.notify_all();
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
      m_change_cv.notify_all();
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
    m_change_cv.notify_all();
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
      m_change_cv.notify_all();
    }
  }

 private:
  struct PendingSend {
    explicit PendingSend(T item) : value(std::move(item)) {}
    T value;
    bool accepted{false};
    bool closed{false};
    bool cancelled{false};
    ChannelStatus status{ChannelStatus::kWouldBlock};
    ErrorPtr error;
    std::shared_ptr<detail::SelectWaitState> select_state;
    std::size_t select_index{static_cast<std::size_t>(-1)};
    core::ParkingCondition cv;
  };

  struct PendingRecv {
    std::optional<T> value;
    bool ready{false};
    bool ok{false};
    bool cancelled{false};
    ChannelStatus status{ChannelStatus::kWouldBlock};
    ErrorPtr error;
    std::shared_ptr<detail::SelectWaitState> select_state;
    std::size_t select_index{static_cast<std::size_t>(-1)};
    core::ParkingCondition cv;
  };

  struct CallbackGuard {
    const ContextPtr& context;
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

  void NotifyGeneration() {
    m_generation.fetch_add(1, std::memory_order_release);
    m_change_cv.notify_all();
  }

  ChannelSendResult SendUntil(T value, std::optional<TimePoint> deadline,
                              const ContextPtr& context, bool nonblocking) {
    if (const auto context_status = ContextSendStatus(context);
        context_status.status != ChannelStatus::kReady) {
      return context_status;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_closed) {
      return {ChannelStatus::kClosed, ChannelClosedError()};
    }

    // 优先把值交接给最早等待的接收者，再使用缓冲区。
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
          m_change_cv.notify_all();
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
      receiver->cv.notify_one();
      ++m_generation;
      lock.unlock();
      m_change_cv.notify_all();
      return {ChannelStatus::kReady, {}};
    }

    if (m_buffer.size() < m_capacity) {
      try {
        m_buffer.emplace_back(std::move(value));
      } catch (...) {
        return {ChannelStatus::kInvalid, detail::ValueOperationError()};
      }
      ++m_generation;
      lock.unlock();
      m_change_cv.notify_all();
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
    std::shared_ptr<PendingSend> pending;
    try {
      pending = std::make_shared<PendingSend>(std::move(value));
      m_senders.emplace_back(pending);
    } catch (...) {
      return {ChannelStatus::kInvalid, detail::ValueOperationError()};
    }
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
        m_change_cv.notify_all();
        return {pending->status, pending->error};
      }
      if (DeadlineReached(deadline)) {
        pending->cancelled = true;
        pending->status = ChannelStatus::kTimedOut;
        pending->error = ChannelTimeoutError();
        ErasePending(m_senders, pending);
        ++m_generation;
        lock.unlock();
        m_change_cv.notify_all();
        return {ChannelStatus::kTimedOut, pending->error};
      }

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
      m_change_cv.notify_all();
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
        sender->cv.notify_one();
        ++m_generation;
        lock.unlock();
        m_change_cv.notify_all();
        return {std::optional<T>(std::move(value)), true, true,
                ChannelStatus::kReady, {}};
      } else {
        // 唯一剩余的可接受路径是 noexcept 复制。返回复制值，避免选中后
        // 再调用可能抛异常的移动构造。
        T value = sender->value;
        m_senders.pop_front();
        sender->accepted = true;
        sender->status = ChannelStatus::kReady;
        sender->cv.notify_one();
        ++m_generation;
        lock.unlock();
        m_change_cv.notify_all();
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

    if (core::ParkingCondition::FiberWaitUnsupported()) {
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }
    std::shared_ptr<PendingRecv> pending;
    try {
      pending = std::make_shared<PendingRecv>();
      m_receivers.emplace_back(pending);
    } catch (...) {
      return {std::nullopt, false, true, ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }
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
        m_change_cv.notify_all();
        return {std::nullopt, false, true, pending->status, pending->error};
      }
      if (DeadlineReached(deadline)) {
        pending->cancelled = true;
        pending->status = ChannelStatus::kTimedOut;
        pending->error = ChannelTimeoutError();
        ErasePending(m_receivers, pending);
        ++m_generation;
        lock.unlock();
        m_change_cv.notify_all();
        return {std::nullopt, false, false, ChannelStatus::kTimedOut,
                pending->error};
      }

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
      sender->cv.notify_one();
      m_senders.pop_front();
      break;
    }
  }

  template <typename Node>
  static void ErasePending(std::deque<std::shared_ptr<Node>>& queue,
                           const std::shared_ptr<Node>& node) {
    queue.erase(std::remove(queue.begin(), queue.end(), node), queue.end());
  }

  std::size_t m_capacity{0};
  mutable std::mutex m_mutex;
  mutable core::ParkingCondition m_change_cv;
  std::deque<T> m_buffer;
  std::deque<std::shared_ptr<PendingSend>> m_senders;
  std::deque<std::shared_ptr<PendingRecv>> m_receivers;
  bool m_closed{false};
  std::atomic<std::uint64_t> m_generation{0};
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

  explicit operator bool() const noexcept {
    return static_cast<bool>(probe) || is_default || static_cast<bool>(arm) ||
           unsupported;
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
      }};
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
      }};
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
                    {}};
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
