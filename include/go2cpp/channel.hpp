#pragma once

#include "go2cpp/context.hpp"
#include "go2cpp/error.hpp"
#include "go2cpp/panic_defer.hpp"

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
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <type_traits>
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

ErrorPtr ChannelClosedError();
ErrorPtr ChannelAlreadyClosedError();
ErrorPtr ChannelWouldBlockError();
ErrorPtr ChannelNilError();
ErrorPtr ChannelTimeoutError();

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
  ChannelStatus status{ChannelStatus::kWouldBlock};
  ErrorPtr error;
};

struct SelectCase;

namespace detail {

// Shared by every armed case in one Select call. A channel operation must
// claim this state before committing a transfer, which prevents two ready
// channels from selecting the same caller concurrently.
class SelectWaitState final {
 public:
  bool TrySelect(std::size_t index, SelectProbe probe) {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_selected || m_cancelled) {
        return false;
      }
      m_selected = true;
      m_index = index;
      m_probe = std::move(probe);
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
      first->m_selected = true;
      first->m_index = first_index;
      first->m_probe = std::move(first_probe);
      second->m_selected = true;
      second->m_index = second_index;
      second->m_probe = std::move(second_probe);
    }
    first->m_cv.notify_one();
    second->m_cv.notify_one();
    return true;
  }

  bool IsSelected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_selected;
  }

  bool WaitFor(ContextDuration timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, timeout,
                         [this] { return m_selected || m_cancelled; });
  }

  void Notify() { m_cv.notify_one(); }

  // Prevent a late channel operation from claiming a select after the caller
  // has committed to cancellation, timeout, or another non-channel case.
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
    m_taken = true;
    *index = m_index;
    *probe = std::move(m_probe);
    return true;
  }

 private:
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
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

  // A convenient zero-value accessor for code mirroring Go's <-ch assignment.
  T ValueOrDefault() const {
    return value.has_value() ? *value : T{};
  }
};

template <typename T>
class Channel final : public std::enable_shared_from_this<Channel<T>> {
 public:
  using Ptr = std::shared_ptr<Channel<T>>;
  using Duration = ContextDuration;
  using TimePoint = ContextTimePoint;

  static Ptr Create(std::size_t capacity = 0) {
    return std::make_shared<Channel<T>>(capacity);
  }

  explicit Channel(std::size_t capacity = 0) : m_capacity(capacity) {}
  ~Channel() { Close(); }

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  std::size_t Capacity() const noexcept { return m_capacity; }
  std::size_t Len() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffer.size();
  }
  bool IsClosed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
  }
  bool closed() const { return IsClosed(); }

  ChannelSendResult Send(T value, const ContextPtr& context = {}) {
    return SendUntil(std::move(value), std::nullopt, context, false);
  }

  // Explicit Go-like boundary. The ordinary Send API is status based so
  // callers can handle a close race without C++ exceptions.
  ChannelSendResult SendOrPanic(T value, const ContextPtr& context = {}) {
    auto result = Send(std::move(value), context);
    if (result.Closed()) {
      panic_defer::panic(
          panic_defer::PanicValue::text("send on closed channel"));
    }
    return result;
  }

  ChannelSendResult SendFor(T value, Duration timeout,
                            const ContextPtr& context = {}) {
    if (timeout < Duration::zero()) {
      timeout = Duration::zero();
    }
    return SendUntil(std::move(value), std::chrono::steady_clock::now() + timeout,
                     context, false);
  }

  ChannelSendResult TrySend(T value) {
    return SendUntil(std::move(value), std::chrono::steady_clock::now(), {}, true);
  }

  ChannelRecvResult<T> Recv(const ContextPtr& context = {}) {
    return RecvUntil(std::nullopt, context, false);
  }

  ChannelRecvResult<T> RecvFor(Duration timeout, const ContextPtr& context = {}) {
    if (timeout < Duration::zero()) {
      timeout = Duration::zero();
    }
    return RecvUntil(std::chrono::steady_clock::now() + timeout, context, false);
  }

  ChannelRecvResult<T> TryRecv() {
    return RecvUntil(std::chrono::steady_clock::now(), {}, true);
  }

  // Close is idempotent and wakes every blocked operation.  A repeated close
  // returns kAlreadyClosed; no C++ exception is thrown.
  ChannelSendResult Close() {
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
          sender->select_state->TrySelect(sender->select_index,
                                          std::move(probe));
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
          receiver->select_state->TrySelect(receiver->select_index,
                                            std::move(probe));
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

  ChannelSendResult CloseOrPanic() {
    auto result = Close();
    if (result.status == ChannelStatus::kAlreadyClosed) {
      panic_defer::panic(
          panic_defer::PanicValue::text("close of closed channel"));
    }
    return result;
  }

  // A monotonically increasing generation is useful to external select
  // implementations.  It changes whenever a send, receive, or close changes
  // channel readiness.
  std::uint64_t Generation() const noexcept {
    return m_generation.load(std::memory_order_acquire);
  }

  bool WaitForChange(std::uint64_t generation, Duration timeout) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_change_cv.wait_for(lock, timeout, [this, generation] {
      return m_generation.load(std::memory_order_relaxed) != generation;
    });
  }

  // Register a select receive case. The registration rechecks readiness while
  // holding the channel lock, so a sender cannot slip between the probe and
  // waiter publication. A select node is removed by DisarmSelect when the
  // caller chooses another case or its timeout/cancellation wins.
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
        probe.value = m_buffer.front();
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
          recv_probe.value = sender->value;
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
        probe.value = sender->value;
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

  // Register a select send case. SendCase values are copyable by contract so
  // a failed arm leaves the value available until another case is selected.
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
          recv_probe.value = value;
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
      if (!state->TrySelect(index, std::move(send_probe))) {
        return false;
      }
      receiver->value.emplace(value);
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
      if (!state->TrySelect(index, std::move(probe))) {
        return false;
      }
      m_buffer.emplace_back(value);
      ++m_generation;
      lock.unlock();
      m_change_cv.notify_all();
      return true;
    }

    auto pending = std::make_shared<PendingSend>(value);
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
    std::condition_variable cv;
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
    std::condition_variable cv;
  };

  struct CallbackGuard {
    const ContextPtr& context;
    DoneSignal::CallbackId id{0};
    ~CallbackGuard() {
      if (context && id != 0) {
        context->Done().RemoveCallback(id);
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

    // Handoff to the oldest waiting receiver before using the buffer.
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
          probe.value = value;
          probe.status = ChannelStatus::kReady;
          m_receivers.pop_front();
          if (!receiver->select_state->TrySelect(receiver->select_index,
                                                 std::move(probe))) {
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
      m_receivers.pop_front();
      receiver->value.emplace(std::move(value));
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
      m_buffer.emplace_back(std::move(value));
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

    auto pending = std::make_shared<PendingSend>(std::move(value));
    m_senders.emplace_back(pending);
    CallbackGuard callback_guard{context};
    if (context) {
      callback_guard.id = context->Done().AddCallback(
          [pending] { pending->cv.notify_one(); });
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
      if (context_status != ChannelStatus::kReady) {
        pending->cancelled = true;
        pending->status = context_status;
        pending->error = context_error;
        ErasePending(m_senders, pending);
        ++m_generation;
        lock.unlock();
        m_change_cv.notify_all();
        return {context_status, context_error};
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
        if (wait_for > std::chrono::milliseconds(2)) {
          wait_for = std::chrono::milliseconds(2);
        }
        pending->cv.wait_for(lock, wait_for, [&] {
          return pending->accepted || pending->closed || pending->cancelled ||
                 (context && context->IsDone());
        });
      } else {
        pending->cv.wait_for(lock, std::chrono::milliseconds(2), [&] {
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
      T value = std::move(m_buffer.front());
      m_buffer.pop_front();
      // Refill one blocked sender into a newly available buffer slot.
      RefillBufferLocked();
      ++m_generation;
      lock.unlock();
      m_change_cv.notify_all();
      return {std::optional<T>(std::move(value)), true, true,
              ChannelStatus::kReady, {}};
    }

    // An unbuffered sender can rendezvous directly with this receiver.
    while (!m_senders.empty()) {
      auto sender = m_senders.front();
      m_senders.pop_front();
      if (sender->cancelled) {
        continue;
      }
      if (sender->select_state) {
        SelectProbe probe;
        probe.ready = true;
        probe.ok = true;
        probe.status = ChannelStatus::kReady;
        if (!sender->select_state->TrySelect(sender->select_index,
                                             std::move(probe))) {
          sender->cancelled = true;
          continue;
        }
      }
      T value = std::move(sender->value);
      sender->accepted = true;
      sender->status = ChannelStatus::kReady;
      sender->cv.notify_one();
      ++m_generation;
      lock.unlock();
      m_change_cv.notify_all();
      return {std::optional<T>(std::move(value)), true, true,
              ChannelStatus::kReady, {}};
    }

    // Closed channels drain buffered values first, then yield the zero value.
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

    auto pending = std::make_shared<PendingRecv>();
    m_receivers.emplace_back(pending);
    CallbackGuard callback_guard{context};
    if (context) {
      callback_guard.id = context->Done().AddCallback(
          [pending] { pending->cv.notify_one(); });
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
      if (status != ChannelStatus::kReady) {
        pending->cancelled = true;
        pending->status = status;
        pending->error = context_error;
        ErasePending(m_receivers, pending);
        ++m_generation;
        lock.unlock();
        m_change_cv.notify_all();
        return {std::nullopt, false, true, status, context_error};
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
        if (wait_for > std::chrono::milliseconds(2)) {
          wait_for = std::chrono::milliseconds(2);
        }
        pending->cv.wait_for(lock, wait_for, [&] {
          return pending->ready || pending->cancelled ||
                 (context && context->IsDone());
        });
      } else {
        pending->cv.wait_for(lock, std::chrono::milliseconds(2), [&] {
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
        m_buffer.emplace_back(std::move(sender->value));
        sender->cancelled = true;
        m_senders.pop_front();
        continue;
      }
      m_buffer.emplace_back(std::move(sender->value));
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
  mutable std::condition_variable m_change_cv;
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

// Directional channel views mirror Go's chan<- and <-chan at the C++ type
// level. They intentionally do not expose the underlying bidirectional handle:
// a receive-only view cannot send or close, while a send-only view may send and
// close but cannot receive.
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

  ChannelSendResult SendOrPanic(T value, const ContextPtr& context = {}) const {
    if (!m_channel) {
      return {ChannelStatus::kNil, ChannelNilError()};
    }
    return m_channel->SendOrPanic(std::move(value), context);
  }

  ChannelSendResult Close() const {
    if (!m_channel) {
      return {ChannelStatus::kNil, ChannelNilError()};
    }
    return m_channel->Close();
  }

  ChannelSendResult CloseOrPanic() const {
    if (!m_channel) {
      return {ChannelStatus::kNil, ChannelNilError()};
    }
    return m_channel->CloseOrPanic();
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

// Checked free functions make nil-channel behavior explicit when translated
// code holds a nullable ChannelPtr (a null handle reports kNil instead of
// waiting forever as a Go nil channel would).
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

struct SelectCase {
  std::function<SelectProbe()> probe;
  bool is_default{false};
  std::function<void(const std::shared_ptr<detail::SelectWaitState>&,
                     std::size_t)>
      arm;
  std::function<void(const std::shared_ptr<detail::SelectWaitState>&)> disarm;

  explicit operator bool() const noexcept {
    return static_cast<bool>(probe) || is_default || static_cast<bool>(arm);
  }
};

struct SelectResult {
  static constexpr std::size_t kNoSelection = static_cast<std::size_t>(-1);

  std::size_t index{kNoSelection};
  bool selected{false};
  bool ok{false};
  std::any value;
  ChannelStatus status{ChannelStatus::kWouldBlock};
  ErrorPtr error;

  explicit operator bool() const noexcept { return selected; }

  template <typename T>
  std::optional<T> Value() const {
    if (!value.has_value()) {
      return std::nullopt;
    }
    try {
      return std::any_cast<T>(value);
    } catch (const std::bad_any_cast&) {
      return std::nullopt;
    }
  }
};

template <typename T>
SelectCase RecvCase(const ChannelPtr<T>& channel) {
  return SelectCase{[channel] {
                      if (!channel) {
                        SelectProbe probe;
                        probe.ready = true;
                        probe.status = ChannelStatus::kNil;
                        probe.error = ChannelNilError();
                        return probe;
                      }
                      auto result = channel->TryRecv();
                      if (!result.ready) {
                        return SelectProbe{};
                      }
                      SelectProbe probe;
                      probe.ready = true;
                      probe.ok = result.ok;
                      probe.status = result.status;
                      probe.error = result.error;
                      if (result.value.has_value()) {
                        probe.value = std::move(*result.value);
                      }
                      return probe;
                    },
                    false,
                    [channel](const std::shared_ptr<detail::SelectWaitState>&
                                  state,
                              std::size_t index) {
                      if (channel) {
                        channel->ArmSelectRecv(state, index);
                      }
                    },
                    [channel](const std::shared_ptr<detail::SelectWaitState>&
                                  state) {
                      if (channel) {
                        channel->DisarmSelect(state);
                      }
                    }};
}

template <typename T>
SelectCase SendCase(const ChannelPtr<T>& channel, T value) {
  // Select send cases require a copyable value because failed probes must leave
  // the value available for a later round.
  auto shared_value = std::make_shared<T>(std::move(value));
  return SelectCase{[channel, shared_value] {
                      if (!channel) {
                        SelectProbe probe;
                        probe.ready = true;
                        probe.status = ChannelStatus::kNil;
                        probe.error = ChannelNilError();
                        return probe;
                      }
                      auto result = channel->TrySend(*shared_value);
                      if (result.status == ChannelStatus::kWouldBlock) {
                        return SelectProbe{};
                      }
                      SelectProbe probe;
                      probe.ready = true;
                      probe.ok = result.Ok();
                      probe.status = result.status;
                      probe.error = result.error;
                      return probe;
                    },
                    false,
                    [channel, shared_value](
                        const std::shared_ptr<detail::SelectWaitState>& state,
                        std::size_t index) {
                      if (channel) {
                        channel->ArmSelectSend(state, index, *shared_value);
                      }
                    },
                    [channel](const std::shared_ptr<detail::SelectWaitState>&
                                  state) {
                      if (channel) {
                        channel->DisarmSelect(state);
                      }
                    }};
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

SelectResult Select(const std::vector<SelectCase>& cases,
                    const ContextPtr& context = {},
                    std::optional<ContextDuration> timeout = std::nullopt);

inline SelectResult Select(std::initializer_list<SelectCase> cases,
                           const ContextPtr& context = {},
                           std::optional<ContextDuration> timeout = std::nullopt) {
  return Select(std::vector<SelectCase>(cases), context, timeout);
}

}  // namespace go2cpp
