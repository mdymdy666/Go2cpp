/**
 * @file concurrency/fast_channel.hpp  —— 实验版（/tmp 专用，不入库）
 * @brief 数据面极简通道 v2：SPSC 无锁 ring / MPMC Vyukov 无锁 ring / 对照单锁版。
 *
 * 与工作树 v1 的差别（只改热路径，等待协议沿用 channel.hpp 的 detail 设施）：
 *
 * 1. `FastSpscChannel`：保持「head/tail 各一条 cacheline + acquire/release」，
 *    取模改编译期掩码，热路径不再读 closed（只在失败路径读）。
 * 2. `FastChannel`：有缓冲时改用 **Vyukov 有界 MPMC ring**：
 *    - `enqueue_pos`（生产者 CAS）/ `dequeue_pos`（消费者 CAS）分处两条
 *      cacheline，每槽一个 `seq` 序号；
 *    - 稳态 1P1C 下生产者只碰 enqueue_pos + 自己写的槽，消费者只碰
 *      dequeue_pos + 槽的 seq，**没有同一地址上的 RMW 争用**；
 *    - 满/空才进入「锁 + 等待队列 + park」冷路径（与 v1 相同协议）。
 * 3. `FastChannelLock`：精简单锁对照实现（Go chansend/chanrecv 快路径的
 *    最小化版本），用于证明无锁 ring 的收益不是靠运气。
 *
 * 四种组合（Fiber↔Fiber、Fiber↔Thread、Thread↔Fiber、Thread↔Thread）共用
 * 同一套等待节点协议：交接方在锁内发布 signal + 推进 seq，原生线程的
 * FUTEX_WAKE 放到解锁之后；等待方先查 signal 再 park。
 */

#pragma once

#include "go2cpp/concurrency/channel.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace go2cpp {

/** @brief FastChannel/FastSpscChannel 的操作结果。 */
enum class FastStatus : std::uint8_t {
  /// @brief 成功。
  kOk = 0,
  /// @brief 非阻塞操作当前无法完成（满/空）。
  kWouldBlock = 1,
  /// @brief 通道已关闭（接收方在排空缓冲后才会看到）。
  kClosed = 2,
  /// @brief 当前上下文无法阻塞（手动 Fiber/无 Scheduler），或 park 被打断。
  kUnsupported = 3,
};

/** @brief `FastStatus::kOk` 的便捷判断。 */
constexpr bool FastOk(FastStatus status) noexcept {
  return status == FastStatus::kOk;
}

namespace detail {

/** @brief 就地构造槽位（不要求 T 默认构造；Cap==0 时可定义空数组）。 */
template <typename T>
class FastSlot final {
 public:
  FastSlot() noexcept = default;
  ~FastSlot() noexcept = default;
  FastSlot(const FastSlot&) = delete;
  FastSlot& operator=(const FastSlot&) = delete;

  template <typename U>
  void Construct(U&& value) {
    ::new (static_cast<void*>(m_storage)) T(std::forward<U>(value));
  }

  void Destroy() noexcept { Ptr()->~T(); }

  T* Ptr() noexcept { return std::launder(reinterpret_cast<T*>(m_storage)); }
  const T* Ptr() const noexcept {
    return std::launder(reinterpret_cast<const T*>(m_storage));
  }

 private:
  alignas(T) unsigned char m_storage[sizeof(T)];
};

/** @brief 池化的发送等待节点。 */
template <typename T>
struct FastSendNode final : WaitNodeBase {
  std::optional<T> value;
  bool accepted{false};
  bool closed{false};

  void Reset() noexcept {
    ResetBase();
    value.reset();
    accepted = false;
    closed = false;
  }
};

/** @brief 池化的接收等待节点。 */
template <typename T>
struct FastRecvNode final : WaitNodeBase {
  std::optional<T> value;
  bool ready{false};
  bool closed{false};

  void Reset() noexcept {
    ResetBase();
    value.reset();
    ready = false;
    closed = false;
  }
};

/// @brief 节点是否已被唤醒。
inline bool FastNodeSignaled(const WaitNodeBase* node) noexcept {
  return node->signal.load(std::memory_order_acquire) !=
         static_cast<std::uint32_t>(WaitSignal::kWaiting);
}

/// @brief CPU 自旋提示。
inline void FastRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/** @brief 一次唤醒目标：只带出 futex 字地址，唤醒在解锁后执行。 */
struct FastWakeHint {
  std::atomic<std::uint32_t>* futex_word{nullptr};
};

/// @brief 持锁发布节点结果（调用契约：结果标志已写完，必须在锁内）。
inline void FastPublishNodeLocked(WaitNodeBase* node, std::uint32_t signal,
                                  FastWakeHint* hint) noexcept {
  node->signal.store(signal, std::memory_order_release);
  node->seq.fetch_add(1, std::memory_order_release);
  if (node->managed) {
    ChannelWakeManaged(node);
    return;
  }
  if (hint != nullptr && node->parked.load(std::memory_order_relaxed) != 0) {
    hint->futex_word = &node->seq;
  }
}

/**
 * @brief 等待一个节点被唤醒；返回 false 表示当前上下文无法继续等待。
 *
 * 原生线程先做一小段无锁自旋，再交给 `ChannelWaitNative`；后者带 **1ms
 * 兜底超时**：SPSC 的等待槽协议里，等待者「注册 → 重检 → park」与交接方
 * 「推进 head/tail → 查等待槽」之间存在窗口，极端交错下双方会同时 park。
 * 超时重试让等待者重新检查 ring 状态，把这类丢唤醒从「永久挂起」降级为
 * 「最多 1ms 的一次多余往返」；正常交接仍在几十纳秒内完成，走不到超时。
 *
 * 受管 Fiber 走 `Scheduler::park_wait`（由调度器保证唤醒不丢）。
 */
inline bool FastWaitNode(WaitNodeBase* node) noexcept {
  if (!node->managed) {
    constexpr int kPreSpin = 256;
    for (int i = 0; i < kPreSpin; ++i) {
      if (FastNodeSignaled(node)) {
        return true;
      }
      FastRelax();
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    for (;;) {
      ChannelWaitNative(node, deadline);
      if (FastNodeSignaled(node)) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return true;  // 超时：当作 spurious 唤醒，调用方重试。
      }
    }
  }
  for (;;) {
    if (FastNodeSignaled(node)) {
      return true;
    }
    if (!ChannelParkOnce(node)) {
      return false;
    }
  }
}

/// @brief 解锁后唤醒（热路径为空操作）。
inline void FastWakeIfNeeded(const FastWakeHint& hint) noexcept {
  if (hint.futex_word != nullptr) [[unlikely]] {
    ChannelFutexWake(hint.futex_word);
  }
}

}  // namespace detail

// ===========================================================================
// FastSpscChannel：单生产者单消费者无锁 ring
// ===========================================================================

/**
 * @brief 单生产者/单消费者无锁 ring（数据面 1P1C 首选）。
 *
 * 热路径：生产者只读 head、只写 tail；消费者只读 tail、只写 head。
 * 不取锁、不 CAS、不跨核 RMW。阻塞路径（满/空）用一把 mutex 登记等待者。
 */
template <typename T, std::size_t Cap>
class FastSpscChannel final {
 public:
  static_assert(Cap > 0, "FastSpscChannel requires a positive capacity");
  static_assert(std::is_nothrow_move_constructible<T>::value,
                "FastSpscChannel<T, Cap> requires a nothrow move constructor");
  static_assert(std::is_nothrow_move_assignable<T>::value,
                "FastSpscChannel<T, Cap> requires a nothrow move assignment");
  static_assert(std::is_nothrow_default_constructible<T>::value,
                "FastSpscChannel<T, Cap> requires a nothrow default ctor");
  static_assert(std::is_nothrow_destructible<T>::value,
                "FastSpscChannel<T, Cap> requires a nothrow destructor");

  static constexpr std::size_t kCapacity = Cap;

  FastSpscChannel() = default;
  ~FastSpscChannel() noexcept { Close(); }

  FastSpscChannel(const FastSpscChannel&) = delete;
  FastSpscChannel& operator=(const FastSpscChannel&) = delete;

  static constexpr std::size_t Capacity() noexcept { return Cap; }

  /** @brief 非阻塞发送（仅生产者调用）。 */
  FastStatus TrySend(T value) {
    if (m_closed.load(std::memory_order_acquire)) {
      return FastStatus::kClosed;
    }
    return Push(value) ? FastStatus::kOk : FastStatus::kWouldBlock;
  }

  /** @brief 非阻塞接收（仅消费者调用）。 */
  FastStatus TryRecv(T& out) {
    if (Pop(out)) {
      return FastStatus::kOk;
    }
    if (m_closed.load(std::memory_order_acquire)) {
      out = T{};
      return FastStatus::kClosed;
    }
    return FastStatus::kWouldBlock;
  }

  /** @brief 阻塞发送（仅生产者调用）。 */
  FastStatus Send(T value) {
    for (;;) {
      if (Push(value)) {
        return FastStatus::kOk;
      }
      if (m_closed.load(std::memory_order_acquire)) {
        return FastStatus::kClosed;
      }
      if (detail::CanSpinWait() && SpinForSpace()) {
        continue;
      }
      SpscWaiter node;
      node.Reset();
      if (!detail::CanSpinWait() && !detail::ChannelBindManaged(&node)) {
        return FastStatus::kUnsupported;
      }
      {
        std::lock_guard<std::mutex> lock(m_wait_mutex);
        if (m_closed.load(std::memory_order_acquire)) {
          return FastStatus::kClosed;
        }
        if (HasSpace()) {
          continue;
        }
        m_sender_waiter.store(&node, std::memory_order_release);
        if (HasSpace()) {
          m_sender_waiter.store(nullptr, std::memory_order_release);
          continue;
        }
      }
      const bool signaled = detail::FastWaitNode(&node);
      Unregister(m_sender_waiter, &node);
      if (!signaled) {
        return FastStatus::kUnsupported;
      }
      if (node.signal.load(std::memory_order_acquire) ==
          static_cast<std::uint32_t>(detail::WaitSignal::kClosed)) {
        return FastStatus::kClosed;
      }
    }
  }

  /** @brief 阻塞接收（仅消费者调用）。 */
  FastStatus Recv(T& out) {
    for (;;) {
      if (Pop(out)) {
        return FastStatus::kOk;
      }
      if (m_closed.load(std::memory_order_acquire)) {
        out = T{};
        return FastStatus::kClosed;
      }
      if (detail::CanSpinWait() && SpinForData()) {
        continue;
      }
      SpscWaiter node;
      node.Reset();
      if (!detail::CanSpinWait() && !detail::ChannelBindManaged(&node)) {
        return FastStatus::kUnsupported;
      }
      {
        std::lock_guard<std::mutex> lock(m_wait_mutex);
        if (m_closed.load(std::memory_order_acquire)) {
          out = T{};
          return FastStatus::kClosed;
        }
        if (m_head.load(std::memory_order_acquire) !=
            m_tail.load(std::memory_order_acquire)) {
          continue;
        }
        m_receiver_waiter.store(&node, std::memory_order_release);
        if (m_head.load(std::memory_order_acquire) !=
            m_tail.load(std::memory_order_acquire)) {
          m_receiver_waiter.store(nullptr, std::memory_order_release);
          continue;
        }
      }
      const bool signaled = detail::FastWaitNode(&node);
      Unregister(m_receiver_waiter, &node);
      if (!signaled) {
        return FastStatus::kUnsupported;
      }
      if (node.signal.load(std::memory_order_acquire) ==
          static_cast<std::uint32_t>(detail::WaitSignal::kClosed)) {
        out = T{};
        return FastStatus::kClosed;
      }
    }
  }

  /** @brief 关闭通道并唤醒两个方向上的等待者；幂等。 */
  void Close() noexcept {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    std::lock_guard<std::mutex> lock(m_wait_mutex);
    WakeLocked(m_sender_waiter);
    WakeLocked(m_receiver_waiter);
  }

  bool IsClosed() const noexcept {
    return m_closed.load(std::memory_order_acquire);
  }

  std::size_t Len() const noexcept {
    const std::size_t head = m_head.load(std::memory_order_acquire);
    const std::size_t tail = m_tail.load(std::memory_order_acquire);
    return tail - head;
  }

 private:
  static constexpr int kSpscSpin = 512;

  struct SpscWaiter final : detail::WaitNodeBase {
    void Reset() noexcept { ResetBase(); }
  };

  /// @brief 环形下标（Cap 为 2 的幂时是掩码）。
  static constexpr std::size_t Index(std::size_t pos) noexcept {
    if constexpr ((Cap & (Cap - 1)) == 0) {
      return pos & (Cap - 1);
    } else {
      return pos % Cap;
    }
  }

  bool Push(T& value) {
    const std::size_t tail = m_tail.load(std::memory_order_relaxed);
    const std::size_t head = m_head.load(std::memory_order_acquire);
    if (tail - head >= Cap) {
      return false;
    }
    const std::size_t index = Index(tail);
    m_slots[index].Construct(std::move(value));
    m_tail.store(tail + 1, std::memory_order_release);
    // 无条件查一次等待槽（内部先 relaxed load，无等待者时只有一条 load）：
    // 不依赖「由空变非空」这类水位判断，避免唤醒丢失。
    WakeWaiter(m_receiver_waiter);
    return true;
  }

  bool Pop(T& out) {
    const std::size_t head = m_head.load(std::memory_order_relaxed);
    const std::size_t tail = m_tail.load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }
    const std::size_t index = Index(head);
    out = std::move(*m_slots[index].Ptr());
    m_slots[index].Destroy();
    m_head.store(head + 1, std::memory_order_release);
    // 同 Push：无条件查一次等待槽，不依赖「由满变非满」的水位判断。
    WakeWaiter(m_sender_waiter);
    return true;
  }

  bool HasSpace() const noexcept {
    const std::size_t tail = m_tail.load(std::memory_order_relaxed);
    const std::size_t head = m_head.load(std::memory_order_acquire);
    return tail - head < Cap;
  }

  bool SpinForSpace() const noexcept {
    const std::size_t tail = m_tail.load(std::memory_order_relaxed);
    for (int i = 0; i < kSpscSpin; ++i) {
      if (m_closed.load(std::memory_order_relaxed)) {
        return true;
      }
      if (tail - m_head.load(std::memory_order_acquire) < Cap) {
        return true;
      }
      detail::FastRelax();
    }
    return false;
  }

  bool SpinForData() const noexcept {
    const std::size_t head = m_head.load(std::memory_order_relaxed);
    for (int i = 0; i < kSpscSpin; ++i) {
      if (m_closed.load(std::memory_order_relaxed)) {
        return true;
      }
      if (m_tail.load(std::memory_order_acquire) != head) {
        return true;
      }
      detail::FastRelax();
    }
    return false;
  }

  void WakeWaiter(std::atomic<detail::WaitNodeBase*>& slot) {
    if (slot.load(std::memory_order_relaxed) == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(m_wait_mutex);
    WakeLocked(slot);
  }

  static void WakeLocked(std::atomic<detail::WaitNodeBase*>& slot) noexcept {
    detail::WaitNodeBase* const node =
        slot.exchange(nullptr, std::memory_order_acq_rel);
    if (node == nullptr) {
      return;
    }
    node->signal.store(static_cast<std::uint32_t>(detail::WaitSignal::kReady),
                       std::memory_order_release);
    node->seq.fetch_add(1, std::memory_order_release);
    if (node->managed) {
      detail::ChannelWakeManaged(node);
      return;
    }
    if (node->parked.load(std::memory_order_relaxed) != 0) {
      detail::ChannelFutexWake(&node->seq);
    }
  }

  void Unregister(std::atomic<detail::WaitNodeBase*>& slot,
                  detail::WaitNodeBase* node) noexcept {
    std::lock_guard<std::mutex> lock(m_wait_mutex);
    detail::WaitNodeBase* const registered =
        slot.load(std::memory_order_acquire);
    if (registered == node) {
      slot.store(nullptr, std::memory_order_release);
    }
  }

  alignas(64) std::atomic<std::size_t> m_tail{0};
  alignas(64) std::atomic<std::size_t> m_head{0};
  alignas(64) std::atomic<bool> m_closed{false};
  alignas(64) mutable std::mutex m_wait_mutex;
  std::atomic<detail::WaitNodeBase*> m_sender_waiter{nullptr};
  std::atomic<detail::WaitNodeBase*> m_receiver_waiter{nullptr};
  alignas(64) std::array<detail::FastSlot<T>, Cap> m_slots;
};

// ===========================================================================
// FastChannel：Vyukov 有界 MPMC ring + 锁回退（Cap > 0）
// ===========================================================================

/**
 * @brief 数据面 MPMC 通道：Vyukov 有界队列热路径 + 锁保护的 park 冷路径。
 *
 * ## 热路径（Cap > 0）
 *
 * 每个槽位带一个序号 `seq`：
 * - 生产者：`pos = enqueue_pos`；槽 `pos % Cap` 的 `seq == pos` 时该槽可写，
 *   CAS 推进 `enqueue_pos` 后写入值，再 release 发布 `seq = pos + 1`；
 * - 消费者：`pos = dequeue_pos`；槽的 `seq == pos + 1` 时可读，CAS 推进
 *   `dequeue_pos` 后读值，再 release 发布 `seq = pos + Cap`。
 *
 * 生产者只写 `enqueue_pos` 与自己写的槽，消费者只写 `dequeue_pos` 与槽序号，
 * 两个位置分处不同 cacheline，**没有同一地址的 RMW 争用**；这是它比「单锁
 * 快路径」更快的原因（见 bench 里 `FastChannelLock` 的对照）。
 *
 * ## 冷路径
 *
 * 满/空才进锁：`m_wait_lock` 只保护两向等待队列（侵入式链表），
 * **绝不持锁 park、不持锁 futex**。有等待的接收者时发送方优先直接交接
 * （绕过 ring），与 Go 的 `send`/`recv` 快路径语义一致。
 *
 * ## Cap == 0
 *
 * 无缓冲 rendezvous：没有 ring，只有「锁内直接交接 + park」。
 */
template <typename T, std::size_t Cap>
class FastChannel final {
 public:
  static_assert(std::is_nothrow_move_constructible<T>::value,
                "FastChannel<T, Cap> requires a nothrow move constructor");
  static_assert(std::is_nothrow_move_assignable<T>::value,
                "FastChannel<T, Cap> requires a nothrow move assignment");
  static_assert(std::is_nothrow_default_constructible<T>::value,
                "FastChannel<T, Cap> requires a nothrow default constructor "
                "(closed Receive writes a zero value like Go)");
  static_assert(std::is_nothrow_destructible<T>::value,
                "FastChannel<T, Cap> requires a nothrow destructor");

  static constexpr std::size_t kCapacity = Cap;

  FastChannel() {
    if constexpr (Cap > 0) {
      for (std::size_t i = 0; i < Cap; ++i) {
        m_buf[i].seq.store(i, std::memory_order_relaxed);
      }
    }
  }
  ~FastChannel() noexcept { Close(); }

  FastChannel(const FastChannel&) = delete;
  FastChannel& operator=(const FastChannel&) = delete;

  static constexpr std::size_t Capacity() noexcept { return Cap; }

  /** @brief 非阻塞发送；满时返回 `kWouldBlock`，关闭后返回 `kClosed`。 */
  FastStatus TrySend(T value) {
    if (m_closed.load(std::memory_order_acquire)) {
      return FastStatus::kClosed;
    }
    if constexpr (Cap > 0) {
      if (TryEnqueue(value)) {
        return FastStatus::kOk;
      }
      return FastStatus::kWouldBlock;
    } else {
      detail::FastWakeHint hint;
      const FastStatus status = HandoffSend(value, &hint);
      detail::FastWakeIfNeeded(hint);
      return status;
    }
  }

  /** @brief 非阻塞接收；空时返回 `kWouldBlock`，关闭且排空后返回 `kClosed`。 */
  FastStatus TryRecv(T& out) {
    if constexpr (Cap > 0) {
      if (TryDequeue(out)) {
        return FastStatus::kOk;
      }
    }
    if (m_closed.load(std::memory_order_acquire)) {
      out = T{};
      return FastStatus::kClosed;
    }
    if constexpr (Cap == 0) {
      detail::FastWakeHint hint;
      const FastStatus status = HandoffRecv(out, &hint);
      detail::FastWakeIfNeeded(hint);
      return status;
    }
    return FastStatus::kWouldBlock;
  }

  /**
   * @brief 阻塞发送：满时先自旋、再 park，直到有空位或通道关闭。
   */
  FastStatus Send(T value) {
    for (;;) {
      if (m_closed.load(std::memory_order_acquire)) {
        return FastStatus::kClosed;
      }
      if constexpr (Cap > 0) {
        // FIFO 由 ring 独占保证：**绝不绕过 ring 直交**（直交会让等待者先拿到
        // 新值、再取到环里的旧值，破坏顺序）。等待者由「入环后唤醒」覆盖。
        if (TryEnqueue(value)) {
          if (m_waiting_receivers.load(std::memory_order_relaxed) != 0)
              [[unlikely]] {
            WakeOneReceiver();
          }
          return FastStatus::kOk;
        }
      } else {
        if (m_waiting_receivers.load(std::memory_order_relaxed) != 0) {
          detail::FastWakeHint hint;
          const FastStatus status = HandoffSend(value, &hint);
          detail::FastWakeIfNeeded(hint);
          if (status != FastStatus::kWouldBlock) {
            return status;
          }
        }
      }
      if (detail::CanSpinWait() && SpinForSpace()) {
        continue;
      }
      const FastStatus parked = ParkAsSender(value);
      if (parked != FastStatus::kWouldBlock) {
        return parked;
      }
      // kWouldBlock：spurious 唤醒，value 已取回，循环重试。
    }
  }

  /**
   * @brief 阻塞接收：空时先自旋、再 park，直到有值或关闭（排空后）。
   */
  FastStatus Recv(T& out) {
    for (;;) {
      if constexpr (Cap > 0) {
        if (TryDequeue(out)) {
          // 有空位了：叫醒一个阻塞发送者，让它自己重新入环。
          if (m_waiting_senders.load(std::memory_order_relaxed) != 0)
              [[unlikely]] {
            WakeOneSender();
          }
          return FastStatus::kOk;
        }
      }
      if (m_closed.load(std::memory_order_acquire)) {
        out = T{};
        return FastStatus::kClosed;
      }
      if constexpr (Cap == 0) {
        // 无缓冲 rendezvous：只能直接交接。
        if (m_waiting_senders.load(std::memory_order_relaxed) != 0) {
          detail::FastWakeHint hint;
          const FastStatus status = HandoffRecv(out, &hint);
          detail::FastWakeIfNeeded(hint);
          if (status != FastStatus::kWouldBlock) {
            return status;
          }
        }
      }
      if (detail::CanSpinWait() && SpinForData()) {
        continue;
      }
      const FastStatus parked = ParkAsReceiver(out);
      if (parked != FastStatus::kWouldBlock) {
        return parked;
      }
    }
  }

  /** @brief 关闭通道并唤醒全部等待者；幂等。 */
  void Close() noexcept {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    detail::FastWakeHint hint;
    std::unique_lock<detail::ChannelSpinLock> lock(m_wait_lock);
    while (SendNode* const sender = m_senders.pop_front()) {
      sender->closed = true;
      detail::FastPublishNodeLocked(
          sender, static_cast<std::uint32_t>(detail::WaitSignal::kClosed),
          nullptr);
      if (!sender->managed &&
          sender->parked.load(std::memory_order_relaxed) != 0) {
        detail::ChannelFutexWake(&sender->seq);
      }
    }
    while (RecvNode* const receiver = m_receivers.pop_front()) {
      receiver->closed = true;
      detail::FastPublishNodeLocked(
          receiver, static_cast<std::uint32_t>(detail::WaitSignal::kClosed),
          nullptr);
      if (!receiver->managed &&
          receiver->parked.load(std::memory_order_relaxed) != 0) {
        detail::ChannelFutexWake(&receiver->seq);
      }
    }
    m_waiting_senders.store(0, std::memory_order_relaxed);
    m_waiting_receivers.store(0, std::memory_order_relaxed);
    lock.unlock();
  }

  bool IsClosed() const noexcept {
    return m_closed.load(std::memory_order_acquire);
  }

  std::size_t Len() const noexcept {
    if constexpr (Cap == 0) {
      return 0;
    } else {
      const std::size_t enq = m_enqueue.load(std::memory_order_acquire);
      const std::size_t deq = m_dequeue.load(std::memory_order_acquire);
      return enq - deq;
    }
  }

 private:
  using SendNode = detail::FastSendNode<T>;
  using RecvNode = detail::FastRecvNode<T>;

  /// @brief Vyukov 槽位：每槽独占一条 cacheline（`seq` 被生产者和消费者双向
  ///        读写，和相邻槽挤在一起会造成伪共享）。
  struct alignas(64) Slot final {
    std::atomic<std::size_t> seq{0};
    detail::FastSlot<T> value;
  };

  static constexpr std::size_t Index(std::size_t pos) noexcept {
    if constexpr ((Cap & (Cap - 1)) == 0) {
      return pos & (Cap - 1);
    } else {
      return pos % Cap;
    }
  }

  // ---- Vyukov 队列 -------------------------------------------------------

  /// @brief 入环；失败表示满。
  bool TryEnqueue(T& value) {
    static_assert(Cap > 0, "TryEnqueue requires Cap > 0");
    std::size_t pos = m_enqueue.load(std::memory_order_relaxed);
    for (;;) {
      Slot& slot = m_buf[Index(pos)];
      const std::size_t seq = slot.seq.load(std::memory_order_acquire);
      const std::intptr_t diff =
          static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
      if (diff == 0) {
        if (m_enqueue.compare_exchange_weak(pos, pos + 1,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
          slot.value.Construct(std::move(value));
          slot.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
        // CAS 失败：pos 已被刷新为最新值，继续。
      } else if (diff < 0) {
        return false;  // 满
      } else {
        pos = m_enqueue.load(std::memory_order_relaxed);
      }
    }
  }

  /// @brief 出环；失败表示空。
  bool TryDequeue(T& out) {
    static_assert(Cap > 0, "TryDequeue requires Cap > 0");
    std::size_t pos = m_dequeue.load(std::memory_order_relaxed);
    for (;;) {
      Slot& slot = m_buf[Index(pos)];
      const std::size_t seq = slot.seq.load(std::memory_order_acquire);
      const std::intptr_t diff = static_cast<std::intptr_t>(seq) -
                                 static_cast<std::intptr_t>(pos + 1);
      if (diff == 0) {
        if (m_dequeue.compare_exchange_weak(pos, pos + 1,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
          out = std::move(*slot.value.Ptr());
          slot.value.Destroy();
          slot.seq.store(pos + Cap, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // 空
      } else {
        pos = m_dequeue.load(std::memory_order_relaxed);
      }
    }
  }

  // ---- 冷路径：直接交接 / park -------------------------------------------

  /// @brief 发送者把值直接交给一个等待中的接收者（锁内）。
  FastStatus HandoffSend(T& value, detail::FastWakeHint* hint) {
    std::unique_lock<detail::ChannelSpinLock> lock(m_wait_lock);
    if (m_closed.load(std::memory_order_relaxed)) {
      return FastStatus::kClosed;
    }
    RecvNode* const receiver = m_receivers.pop_front();
    if (receiver == nullptr) {
      return FastStatus::kWouldBlock;
    }
    m_waiting_receivers.fetch_sub(1, std::memory_order_relaxed);
    receiver->value.emplace(std::move(value));
    receiver->ready = true;
    detail::FastPublishNodeLocked(
        receiver, static_cast<std::uint32_t>(detail::WaitSignal::kReady), hint);
    lock.unlock();
    return FastStatus::kOk;
  }

  /// @brief 接收者从一个等待中的发送者直接取值（锁内）。
  FastStatus HandoffRecv(T& out, detail::FastWakeHint* hint) {
    std::unique_lock<detail::ChannelSpinLock> lock(m_wait_lock);
    SendNode* const sender = m_senders.pop_front();
    if (sender == nullptr) {
      return FastStatus::kWouldBlock;
    }
    m_waiting_senders.fetch_sub(1, std::memory_order_relaxed);
    out = std::move(*sender->value);
    sender->value.reset();
    sender->accepted = true;
    detail::FastPublishNodeLocked(
        sender, static_cast<std::uint32_t>(detail::WaitSignal::kReady), hint);
    lock.unlock();
    return FastStatus::kOk;
  }

  /// @brief 唤一个等待中的接收者（ring 里已有值）。
  ///
  /// 只推进唤醒字，**不设置 `ready`**：接收者醒来走 spurious 分支回主循环，
  /// 由 `TryDequeue` 从 ring 取。`ready` 只用于「直交已经把值写进节点」。
  void WakeOneReceiver() {
    detail::FastWakeHint hint;
    std::unique_lock<detail::ChannelSpinLock> lock(m_wait_lock);
    RecvNode* const receiver = m_receivers.pop_front();
    if (receiver == nullptr) {
      return;
    }
    m_waiting_receivers.fetch_sub(1, std::memory_order_relaxed);
    detail::FastPublishNodeLocked(
        receiver, static_cast<std::uint32_t>(detail::WaitSignal::kReady), &hint);
    lock.unlock();
    detail::FastWakeIfNeeded(hint);
  }

  /// @brief 出队空出一个位置后叫醒一个阻塞发送者（**不搬值**）。
  ///
  /// 数据只走 ring 这一条通道：搬值会把「生产者尚未发布 seq 的在途槽」
  /// 绕过去，消费者因此能提前拿到更新的值（实测表现为 delta == Cap 的
  /// FIFO 违规）。这里只推进唤醒字，发送者醒来自己重新 TryEnqueue。
  void WakeOneSender() {
    detail::FastWakeHint hint;
    std::unique_lock<detail::ChannelSpinLock> lock(m_wait_lock);
    SendNode* const sender = m_senders.pop_front();
    if (sender == nullptr) {
      return;
    }
    m_waiting_senders.fetch_sub(1, std::memory_order_relaxed);
    detail::FastPublishNodeLocked(
        sender, static_cast<std::uint32_t>(detail::WaitSignal::kReady), &hint);
    lock.unlock();
    detail::FastWakeIfNeeded(hint);
  }

  /**
   * @brief 登记为发送者并 park。
   *
   * @return `kOk`（值已被取走）、`kClosed`、`kUnsupported`（无法 park），或
   *         `kWouldBlock`（spurious 唤醒，值已写回 `value`，调用方重试）。
   */
  FastStatus ParkAsSender(T& value) {
    SendNode* const node = detail::WaitNodePool<SendNode>::Acquire();
    {
      detail::FastWakeHint hint;
      std::unique_lock<detail::ChannelSpinLock> lock(m_wait_lock);
      if (m_closed.load(std::memory_order_relaxed)) {
        lock.unlock();
        detail::WaitNodePool<SendNode>::Release(node);
        return FastStatus::kClosed;
      }
      // 锁内重试（窗口闭合）：有缓冲时只入环；无缓冲才直交。
      if constexpr (Cap > 0) {
        if (TryEnqueue(value)) {
          lock.unlock();
          detail::WaitNodePool<SendNode>::Release(node);
          if (m_waiting_receivers.load(std::memory_order_relaxed) != 0) {
            WakeOneReceiver();
          }
          return FastStatus::kOk;
        }
      } else {
        RecvNode* const receiver = m_receivers.pop_front();
        if (receiver != nullptr) {
          m_waiting_receivers.fetch_sub(1, std::memory_order_relaxed);
          receiver->value.emplace(std::move(value));
          receiver->ready = true;
          detail::FastPublishNodeLocked(
              receiver, static_cast<std::uint32_t>(detail::WaitSignal::kReady),
              &hint);
          lock.unlock();
          detail::FastWakeIfNeeded(hint);
          detail::WaitNodePool<SendNode>::Release(node);
          return FastStatus::kOk;
        }
      }
      if (!detail::CanSpinWait() && !detail::ChannelBindManaged(node)) {
        lock.unlock();
        detail::WaitNodePool<SendNode>::Release(node);
        return FastStatus::kUnsupported;
      }
      node->value.emplace(std::move(value));
      node->accepted = false;
      node->closed = false;
      m_senders.push_back(node);
      m_waiting_senders.fetch_add(1, std::memory_order_relaxed);
      // 窗口闭合：登记后必须再试一次入环。否则「生产者登记 / 消费者搬值」
      // 交错时双方都会错过对方，导致双向 park 死锁。
      if constexpr (Cap > 0) {
        if (TryEnqueue(*node->value)) {
          m_senders.erase(node);
          m_waiting_senders.fetch_sub(1, std::memory_order_relaxed);
          lock.unlock();
          detail::WaitNodePool<SendNode>::Release(node);
          if (m_waiting_receivers.load(std::memory_order_relaxed) != 0) {
            WakeOneReceiver();
          }
          return FastStatus::kOk;
        }
      }
      lock.unlock();
    }
    const bool signaled = detail::FastWaitNode(node);
    {
      std::unique_lock<detail::ChannelSpinLock> lock(m_wait_lock);
      if (node->accepted) {
        lock.unlock();
        detail::WaitNodePool<SendNode>::Release(node);
        return FastStatus::kOk;
      }
      if (node->closed) {
        lock.unlock();
        detail::WaitNodePool<SendNode>::Release(node);
        return FastStatus::kClosed;
      }
      if (node->linked) {
        m_senders.erase(node);
        m_waiting_senders.fetch_sub(1, std::memory_order_relaxed);
      }
      value = std::move(*node->value);
      node->value.reset();
      lock.unlock();
      detail::WaitNodePool<SendNode>::Release(node);
    }
    if (!signaled) {
      return FastStatus::kUnsupported;
    }
    // spurious：值已取回，回外层重试。
    return FastStatus::kWouldBlock;
  }

  /**
   * @brief 登记为接收者并 park。
   *
   * @return `kOk`（`out` 已填）、`kClosed`（`out` 已写零）、`kUnsupported`，
   *         或 `kWouldBlock`（spurious，调用方重试）。
   */
  FastStatus ParkAsReceiver(T& out) {
    RecvNode* const node = detail::WaitNodePool<RecvNode>::Acquire();
    {
      detail::FastWakeHint hint;
      std::unique_lock<detail::ChannelSpinLock> lock(m_wait_lock);
      // 锁内重试：有缓冲时**只出环**（数据只走 ring）。此时节点尚未入队，
      // 成功路径不能动等待计数。
      if constexpr (Cap > 0) {
        if (TryDequeue(out)) {
          lock.unlock();
          detail::WaitNodePool<RecvNode>::Release(node);
          if (m_waiting_senders.load(std::memory_order_relaxed) != 0) {
            WakeOneSender();
          }
          return FastStatus::kOk;
        }
      } else {
        SendNode* const sender = m_senders.pop_front();
        if (sender != nullptr) {
          m_waiting_senders.fetch_sub(1, std::memory_order_relaxed);
          out = std::move(*sender->value);
          sender->value.reset();
          sender->accepted = true;
          detail::FastPublishNodeLocked(
              sender, static_cast<std::uint32_t>(detail::WaitSignal::kReady),
              &hint);
          lock.unlock();
          detail::FastWakeIfNeeded(hint);
          detail::WaitNodePool<RecvNode>::Release(node);
          return FastStatus::kOk;
        }
      }
      if (m_closed.load(std::memory_order_relaxed)) {
        lock.unlock();
        detail::WaitNodePool<RecvNode>::Release(node);
        out = T{};
        return FastStatus::kClosed;
      }
      if (!detail::CanSpinWait() && !detail::ChannelBindManaged(node)) {
        lock.unlock();
        detail::WaitNodePool<RecvNode>::Release(node);
        return FastStatus::kUnsupported;
      }
      node->ready = false;
      node->closed = false;
      m_receivers.push_back(node);
      m_waiting_receivers.fetch_add(1, std::memory_order_relaxed);
      // 窗口闭合：登记后必须再试一次出环（与 ParkAsSender 对称）。
      if constexpr (Cap > 0) {
        if (TryDequeue(out)) {
          m_receivers.erase(node);
          m_waiting_receivers.fetch_sub(1, std::memory_order_relaxed);
          lock.unlock();
          detail::WaitNodePool<RecvNode>::Release(node);
          if (m_waiting_senders.load(std::memory_order_relaxed) != 0) {
            WakeOneSender();
          }
          return FastStatus::kOk;
        }
      }
      lock.unlock();
    }
    const bool signaled = detail::FastWaitNode(node);
    {
      std::unique_lock<detail::ChannelSpinLock> lock(m_wait_lock);
      if (node->ready) {
        out = std::move(*node->value);
        node->value.reset();
        lock.unlock();
        detail::WaitNodePool<RecvNode>::Release(node);
        return FastStatus::kOk;
      }
      if (node->closed) {
        lock.unlock();
        detail::WaitNodePool<RecvNode>::Release(node);
        // 关闭时缓冲里可能还有 close 前入队的值：排空后再返回。
        if constexpr (Cap > 0) {
          if (TryDequeue(out)) {
            return FastStatus::kOk;
          }
        }
        out = T{};
        return FastStatus::kClosed;
      }
      if (node->linked) {
        m_receivers.erase(node);
        m_waiting_receivers.fetch_sub(1, std::memory_order_relaxed);
      }
      lock.unlock();
      detail::WaitNodePool<RecvNode>::Release(node);
    }
    if (!signaled) {
      return FastStatus::kUnsupported;
    }
    return FastStatus::kWouldBlock;
  }

  bool SpinForSpace() const noexcept {
    for (int i = 0; i < kSpinBudget; ++i) {
      if (m_closed.load(std::memory_order_relaxed)) {
        return true;
      }
      if (m_waiting_receivers.load(std::memory_order_relaxed) != 0) {
        return true;
      }
      if constexpr (Cap > 0) {
        const std::size_t enq = m_enqueue.load(std::memory_order_relaxed);
        const std::size_t deq = m_dequeue.load(std::memory_order_acquire);
        if (enq - deq < Cap) {
          return true;
        }
      }
      detail::FastRelax();
    }
    return false;
  }

  bool SpinForData() const noexcept {
    for (int i = 0; i < kSpinBudget; ++i) {
      if (m_closed.load(std::memory_order_relaxed)) {
        return true;
      }
      if (m_waiting_senders.load(std::memory_order_relaxed) != 0) {
        return true;
      }
      if constexpr (Cap > 0) {
        const std::size_t deq = m_dequeue.load(std::memory_order_relaxed);
        const std::size_t enq = m_enqueue.load(std::memory_order_acquire);
        if (deq != enq) {
          return true;
        }
      }
      detail::FastRelax();
    }
    return false;
  }

  /// @brief 阻塞前自旋预算：无缓冲时交接窗口极短，长自旋只会占用同核 SMT
  ///        的执行资源；有缓冲时 ring 吸收抖动，值得多自旋一会儿再进 futex。
  static constexpr int kSpinBudget = Cap == 0 ? 64 : 2048;

  // 生产者独占写：入队位置（单独一条 cacheline）。
  alignas(64) std::atomic<std::size_t> m_enqueue{0};
  // 消费者独占写：出队位置（单独一条 cacheline）。
  alignas(64) std::atomic<std::size_t> m_dequeue{0};
  // 关闭标记（读多写少，独立 cacheline）。
  alignas(64) std::atomic<bool> m_closed{false};
  // 等待队列互斥量（冷路径）。
  alignas(64) mutable detail::ChannelSpinLock m_wait_lock;
  // 阻塞中的发送者/接收者计数（锁内维护；热路径 relaxed 读）。
  std::atomic<std::uint32_t> m_waiting_senders{0};
  std::atomic<std::uint32_t> m_waiting_receivers{0};
  // 等待队列（裸指针；节点由等待者线程持有）。
  detail::WaitQueue<SendNode> m_senders;
  detail::WaitQueue<RecvNode> m_receivers;

  // 编译期定长 ring。
  alignas(64) std::array<Slot, Cap> m_buf;
};

// ===========================================================================
// FastChannelLock：单锁 MPMC 对照实现
// ===========================================================================

/**
 * @brief 精简单锁 MPMC（Go `chansend`/`chanrecv` 快路径的最小化版本）。
 *
 * 只保留「lock → 判满/判空 → 入缓冲/出缓冲 → unlock」四步，用于和
 * `FastChannel`（Vyukov ring）做同场对照，量化「去掉同一地址 RMW 争用」
 * 带来的收益。语义与 `FastChannel` 完全一致。
 */
template <typename T, std::size_t Cap>
class FastChannelLock final {
 public:
  static_assert(std::is_nothrow_move_constructible<T>::value, "");
  static_assert(std::is_nothrow_move_assignable<T>::value, "");
  static_assert(std::is_nothrow_default_constructible<T>::value, "");
  static_assert(std::is_nothrow_destructible<T>::value, "");

  static constexpr std::size_t kCapacity = Cap;

  FastChannelLock() = default;
  ~FastChannelLock() noexcept { Close(); }
  FastChannelLock(const FastChannelLock&) = delete;
  FastChannelLock& operator=(const FastChannelLock&) = delete;

  static constexpr std::size_t Capacity() noexcept { return Cap; }

  FastStatus TrySend(T value) {
    detail::FastWakeHint hint;
    FastStatus status;
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      status = PushLocked(value, &hint);
    }
    detail::FastWakeIfNeeded(hint);
    return status;
  }

  FastStatus TryRecv(T& out) {
    detail::FastWakeHint hint;
    FastStatus status;
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      status = PopLocked(out, &hint);
    }
    detail::FastWakeIfNeeded(hint);
    return status;
  }

  FastStatus Send(T value) {
    for (;;) {
      if (m_closed.load(std::memory_order_acquire)) {
        return FastStatus::kClosed;
      }
      detail::FastWakeHint hint;
      FastStatus status = FastStatus::kWouldBlock;
      {
        std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
        status = PushLocked(value, &hint);
      }
      detail::FastWakeIfNeeded(hint);
      if (status != FastStatus::kWouldBlock) {
        return status;
      }
      if (detail::CanSpinWait() && SpinForSpace()) {
        continue;
      }
      SendNode* const node = detail::WaitNodePool<SendNode>::Acquire();
      {
        detail::FastWakeHint hint2;
        std::unique_lock<detail::ChannelSpinLock> lock(m_lock);
        const FastStatus retry = PushLocked(value, &hint2);
        if (retry != FastStatus::kWouldBlock) {
          lock.unlock();
          detail::FastWakeIfNeeded(hint2);
          detail::WaitNodePool<SendNode>::Release(node);
          return retry;
        }
        if (m_closed.load(std::memory_order_relaxed)) {
          lock.unlock();
          detail::WaitNodePool<SendNode>::Release(node);
          return FastStatus::kClosed;
        }
        if (!detail::CanSpinWait() && !detail::ChannelBindManaged(node)) {
          lock.unlock();
          detail::WaitNodePool<SendNode>::Release(node);
          return FastStatus::kUnsupported;
        }
        node->value.emplace(std::move(value));
        m_senders.push_back(node);
        m_waiting_senders.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
      }
      const bool signaled = detail::FastWaitNode(node);
      std::unique_lock<detail::ChannelSpinLock> lock(m_lock);
      if (node->accepted) {
        lock.unlock();
        detail::WaitNodePool<SendNode>::Release(node);
        return FastStatus::kOk;
      }
      if (node->closed) {
        lock.unlock();
        detail::WaitNodePool<SendNode>::Release(node);
        return FastStatus::kClosed;
      }
      if (node->linked) {
        m_senders.erase(node);
        m_waiting_senders.fetch_sub(1, std::memory_order_relaxed);
      }
      value = std::move(*node->value);
      node->value.reset();
      lock.unlock();
      detail::WaitNodePool<SendNode>::Release(node);
      if (!signaled) {
        return FastStatus::kUnsupported;
      }
    }
  }

  FastStatus Recv(T& out) {
    for (;;) {
      if (m_closed.load(std::memory_order_acquire) && !HasData()) {
        out = T{};
        return FastStatus::kClosed;
      }
      detail::FastWakeHint hint;
      FastStatus status = FastStatus::kWouldBlock;
      {
        std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
        status = PopLocked(out, &hint);
      }
      detail::FastWakeIfNeeded(hint);
      if (status != FastStatus::kWouldBlock) {
        return status;
      }
      if (detail::CanSpinWait() && SpinForData()) {
        continue;
      }
      RecvNode* const node = detail::WaitNodePool<RecvNode>::Acquire();
      {
        detail::FastWakeHint hint2;
        std::unique_lock<detail::ChannelSpinLock> lock(m_lock);
        const FastStatus retry = PopLocked(out, &hint2);
        if (retry != FastStatus::kWouldBlock) {
          lock.unlock();
          detail::FastWakeIfNeeded(hint2);
          detail::WaitNodePool<RecvNode>::Release(node);
          return retry;
        }
        if (m_closed.load(std::memory_order_relaxed)) {
          out = T{};
          lock.unlock();
          detail::WaitNodePool<RecvNode>::Release(node);
          return FastStatus::kClosed;
        }
        if (!detail::CanSpinWait() && !detail::ChannelBindManaged(node)) {
          lock.unlock();
          detail::WaitNodePool<RecvNode>::Release(node);
          return FastStatus::kUnsupported;
        }
        m_receivers.push_back(node);
        m_waiting_receivers.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
      }
      const bool signaled = detail::FastWaitNode(node);
      std::unique_lock<detail::ChannelSpinLock> lock(m_lock);
      if (node->ready) {
        out = std::move(*node->value);
        lock.unlock();
        detail::WaitNodePool<RecvNode>::Release(node);
        return FastStatus::kOk;
      }
      if (node->closed) {
        out = T{};
        lock.unlock();
        detail::WaitNodePool<RecvNode>::Release(node);
        return FastStatus::kClosed;
      }
      if (node->linked) {
        m_receivers.erase(node);
        m_waiting_receivers.fetch_sub(1, std::memory_order_relaxed);
      }
      lock.unlock();
      detail::WaitNodePool<RecvNode>::Release(node);
      if (!signaled) {
        return FastStatus::kUnsupported;
      }
    }
  }

  void Close() noexcept {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
    while (SendNode* const sender = m_senders.pop_front()) {
      sender->closed = true;
      detail::FastPublishNodeLocked(
          sender, static_cast<std::uint32_t>(detail::WaitSignal::kClosed),
          nullptr);
      if (!sender->managed &&
          sender->parked.load(std::memory_order_relaxed) != 0) {
        detail::ChannelFutexWake(&sender->seq);
      }
    }
    while (RecvNode* const receiver = m_receivers.pop_front()) {
      receiver->closed = true;
      detail::FastPublishNodeLocked(
          receiver, static_cast<std::uint32_t>(detail::WaitSignal::kClosed),
          nullptr);
      if (!receiver->managed &&
          receiver->parked.load(std::memory_order_relaxed) != 0) {
        detail::ChannelFutexWake(&receiver->seq);
      }
    }
    m_waiting_senders.store(0, std::memory_order_relaxed);
    m_waiting_receivers.store(0, std::memory_order_relaxed);
  }

  bool IsClosed() const noexcept {
    return m_closed.load(std::memory_order_acquire);
  }

  std::size_t Len() {
    std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
    return m_count;
  }

 private:
  using SendNode = detail::FastSendNode<T>;
  using RecvNode = detail::FastRecvNode<T>;

  static constexpr int kSpinBudget = 128;

  static constexpr std::size_t Index(std::size_t pos) noexcept {
    if constexpr (Cap > 0 && (Cap & (Cap - 1)) == 0) {
      return pos & (Cap - 1);
    } else if constexpr (Cap > 0) {
      return pos % Cap;
    } else {
      return 0;
    }
  }

  bool HasData() const noexcept {
    if constexpr (Cap == 0) {
      return false;
    } else {
      return m_count != 0;
    }
  }

  FastStatus PushLocked(T& value, detail::FastWakeHint* hint) {
    if (m_closed.load(std::memory_order_relaxed)) {
      return FastStatus::kClosed;
    }
    if (m_waiting_receivers.load(std::memory_order_relaxed) != 0) {
      RecvNode* const receiver = m_receivers.pop_front();
      if (receiver != nullptr) {
        m_waiting_receivers.fetch_sub(1, std::memory_order_relaxed);
        receiver->value.emplace(std::move(value));
        receiver->ready = true;
        detail::FastPublishNodeLocked(
            receiver, static_cast<std::uint32_t>(detail::WaitSignal::kReady),
            hint);
        return FastStatus::kOk;
      }
    }
    if constexpr (Cap > 0) {
      if (m_count < Cap) {
        m_slots[Index(m_tail)].Construct(std::move(value));
        ++m_tail;
        ++m_count;
        return FastStatus::kOk;
      }
    }
    return FastStatus::kWouldBlock;
  }

  FastStatus PopLocked(T& out, detail::FastWakeHint* hint) {
    if constexpr (Cap > 0) {
      if (m_count != 0) {
        out = std::move(*m_slots[Index(m_head)].Ptr());
        m_slots[Index(m_head)].Destroy();
        ++m_head;
        --m_count;
        RefillLocked(hint);
        return FastStatus::kOk;
      }
    }
    if (m_waiting_senders.load(std::memory_order_relaxed) != 0) {
      SendNode* const sender = m_senders.pop_front();
      if (sender != nullptr) {
        m_waiting_senders.fetch_sub(1, std::memory_order_relaxed);
        out = std::move(*sender->value);
        sender->value.reset();
        sender->accepted = true;
        detail::FastPublishNodeLocked(
            sender, static_cast<std::uint32_t>(detail::WaitSignal::kReady),
            hint);
        return FastStatus::kOk;
      }
    }
    if (m_closed.load(std::memory_order_relaxed)) {
      out = T{};
      return FastStatus::kClosed;
    }
    return FastStatus::kWouldBlock;
  }

  void RefillLocked(detail::FastWakeHint* hint) noexcept {
    if constexpr (Cap > 0) {
      if (m_waiting_senders.load(std::memory_order_relaxed) == 0) {
        return;
      }
      SendNode* const sender = m_senders.pop_front();
      if (sender == nullptr) {
        return;
      }
      m_waiting_senders.fetch_sub(1, std::memory_order_relaxed);
      m_slots[Index(m_tail)].Construct(std::move(*sender->value));
      sender->value.reset();
      ++m_tail;
      ++m_count;
      sender->accepted = true;
      detail::FastPublishNodeLocked(
          sender, static_cast<std::uint32_t>(detail::WaitSignal::kReady), hint);
    }
  }

  bool SpinForSpace() const noexcept {
    for (int i = 0; i < kSpinBudget; ++i) {
      if (m_closed.load(std::memory_order_relaxed)) {
        return true;
      }
      if (m_waiting_receivers.load(std::memory_order_relaxed) != 0) {
        return true;
      }
      if constexpr (Cap > 0) {
        if (m_count < Cap) {
          return true;
        }
      }
      detail::FastRelax();
    }
    return false;
  }

  bool SpinForData() const noexcept {
    for (int i = 0; i < kSpinBudget; ++i) {
      if (m_closed.load(std::memory_order_relaxed)) {
        return true;
      }
      if (m_waiting_senders.load(std::memory_order_relaxed) != 0) {
        return true;
      }
      if (HasData()) {
        return true;
      }
      detail::FastRelax();
    }
    return false;
  }

  alignas(64) mutable detail::ChannelSpinLock m_lock;
  std::size_t m_head{0};
  std::size_t m_tail{0};
  std::size_t m_count{0};
  std::atomic<bool> m_closed{false};
  std::atomic<std::uint32_t> m_waiting_senders{0};
  std::atomic<std::uint32_t> m_waiting_receivers{0};
  detail::WaitQueue<SendNode> m_senders;
  detail::WaitQueue<RecvNode> m_receivers;
  alignas(64) std::array<detail::FastSlot<T>, Cap> m_slots;
};

}  // namespace go2cpp
