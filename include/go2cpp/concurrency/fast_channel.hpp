/**
 * @file concurrency/fast_channel.hpp
 * @brief 数据面专用的极简通道：编译期容量、无 Context/取消/超时/select/调试。
 *
 * ## 模块定位
 *
 * `FastChannel<T, Cap>` 与 `FastSpscChannel<T, Cap>` 是 `Channel<T>` 的**数据面
 * 特化**：只保留「把值从 A 搬到 B」这一件事，去掉通用通道为控制面付出的全部
 * 代价：
 *
 * - 没有 `ContextPtr`/deadline/取消回调：`Send/Recv` 阻塞到底，只有对端交接
 *   或 `Close()` 能唤醒；
 * - 没有 select：等待节点不需要身份、generation、select_state、回调注册；
 * - **不参与调试**：不注册 debug 资源事件、不参与 replay/快照、不做栈捕获，
 *   即使 `GO2CPP_DEBUG_RESOURCES=1` 也不接入（本文件没有任何 GO2CPP_DEBUG_*
 *   调用点，热路径零调试分支）；
 * - 容量是**模板参数**：`std::array<Slot, Cap>` 内联在对象里，零堆分配、边界
 *   由编译器常量折叠，`Cap == 0` 时数组为空（无缓冲 rendezvous）；
 * - 快路径只做「lock → 检查 → 交接/入 buf → unlock」四步（对齐 Go `chansend`/
 *   `chanrecv` 的快路径），只检查「缓冲空/满」与 `closed`，没有代际、Context、
 *   调试或超时检查。
 *
 * 阻塞语义与通用 `Channel<T>` 完全一致（**跨 Fiber 与原生线程**）：
 *
 * | 等待方 | 唤醒方式 |
 * |---|---|
 * | 原生线程 | 自旋 → per-node futex（`ChannelWaitNative`） |
 * | 受管 Fiber | `Scheduler::park_wait`（`ChannelParkOnce`）→ `wake_or_cancel` |
 *
 * 四种组合（Fiber↔Fiber、Fiber↔线程、线程↔Fiber、线程↔线程）共用同一套节点
 * 协议：交接方在锁内写结果标志 + 发布 `signal` + 推进 `seq`，**原生线程的
 * `FUTEX_WAKE` 放到解锁之后**（只把 futex 字地址带出锁），等待方先查 `signal`
 * 再 park，因此不存在「唤醒丢失」窗口，也不会持锁做系统调用。
 *
 * ## 两种实现
 *
 * ### `FastChannel<T, Cap>`：单锁 MPMC
 *
 * 面向「多个生产者/多个消费者」的通用数据面。数据路径是一把
 * `detail::ChannelSpinLock`（64 次 PAUSE + `core::FutexMutex` 兜底），临界区内
 * 只做环形缓冲的入队/出队或等待者直接交接，**绝不持锁 park、不持锁 futex**。
 *
 * 与 Go `hchan` 的对应关系：
 *
 * - `m_head/m_tail/m_count` ↔ `recvx/sendx/qcount`；
 * - `m_senders/m_receivers` ↔ `sendq/recvq`；
 * - `m_closed` ↔ `closed`；
 * - 差别是我们不做 `sudog` 的堆分配（thread_local 节点池），并且用原子
 *   `head/tail` 给等待方做**无锁自旋提示**：缓冲满/空时先自旋重试快路径，
 *   而不是立刻入队 park —— 1P1C 稳态因此完全不进内核（Go 的 goroutine
 *   唤醒同样无系统调用，但它必须经过调度器队列）。
 *
 * ### `FastSpscChannel<T, Cap>`：无锁 ring（单生产者单消费者）
 *
 * 面向网络收发这类天然 1P1C 的数据面。`head`（消费者写）与 `tail`（生产者写）
 * 各占一条 cacheline，热路径只做 acquire load + release store，没有任何锁或
 * CAS；仅当缓冲满/空需要阻塞时才用一把 mutex 登记/唤醒等待者（阻塞路径不在
 * 吞吐热路径上）。这是 Go 的 `chan` 做不到的：Go 必须同时支持任意多
 * sender/receiver，只能走锁。
 *
 * ## 语义与边界
 *
 * - `Cap == 0`：无缓冲 rendezvous；`TrySend/TryRecv` 只在恰好有对端等待时成功。
 * - `Close()`：幂等；之后 `Send` 返回 `kClosed`；`Recv` 先把缓冲里的值排空，
 *   再返回 `kClosed`，并按 Go 的语义把输出参数写成零值 `T{}`。
 * - `Send/Recv` 需要阻塞但当前上下文**无法 park**（例如不属于任何 Scheduler 的
 *   手动 Fiber）时返回 `kUnsupported`，绝不阻塞 carrier 线程。
 * - 对象析构时**不得**仍有线程阻塞在它上面（与 `Channel<T>` 的契约一致）；
 *   析构会 `Close()`，但等待者醒来需要访问对象本身。
 * - `FastSpscChannel` 要求同一时刻至多一个生产者、一个消费者；多生产者/多消费
 *   者属于未定义行为（用 `FastChannel`）。
 * - `T` 必须 nothrow move constructible / nothrow move assignable / nothrow
 *   default constructible / nothrow destructible：数据面不允许值构造异常打断
 *   交接协议，`kClosed` 的零值语义也要求默认构造不抛。
 *
 * ## 性能
 *
 * 见 `tests/concurrency/bench_fast_channel.cpp` 与 `/tmp/fast_channel.md`：
 * 同场对照 Go `chan`（同一台机器、同一 taskset、同一消息数）。
 */
#pragma once

#include "go2cpp/concurrency/channel.hpp"

#include <array>
#include <atomic>
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

/**
 * @brief 一个就地构造的槽位：不要求 T 默认构造，也不产生堆分配。
 *
 * `FastChannel`/`FastSpscChannel` 的槽位数组用本类型而不是 `T`，这样：
 * 1. `T` 可以没有默认构造函数（`std::array<T, Cap>` 做不到）；
 * 2. `Cap == 0` 时数组仍可定义为 `std::array<FastSlot<T>, 0>`；
 * 3. 槽位生命周期完全由通道的入队/出队代码显式管理（placement new +
 *    析构），不会出现「默认构造一遍、赋值一遍」的浪费。
 */
template <typename T>
class FastSlot final {
 public:
  FastSlot() noexcept = default;
  ~FastSlot() noexcept = default;
  FastSlot(const FastSlot&) = delete;
  FastSlot& operator=(const FastSlot&) = delete;

  /** @brief 在槽位内就地构造 T。 */
  template <typename U>
  void Construct(U&& value) {
    ::new (static_cast<void*>(m_storage)) T(std::forward<U>(value));
  }

  /** @brief 析构槽位内的 T（调用方保证槽位处于构造状态）。 */
  void Destroy() noexcept { Ptr()->~T(); }

  T* Ptr() noexcept {
    return std::launder(reinterpret_cast<T*>(m_storage));
  }

  const T* Ptr() const noexcept {
    return std::launder(reinterpret_cast<const T*>(m_storage));
  }

 private:
  alignas(T) unsigned char m_storage[sizeof(T)];
};

/** @brief 池化的发送等待节点（仅在阻塞路径创建，稳态零堆分配）。 */
template <typename T>
struct FastSendNode final : WaitNodeBase {
  /// @brief 待发送的值；被接收者取走或取回后为空。
  std::optional<T> value;
  /// @brief 是否已交接给接收者（与 closed 互斥）。
  bool accepted{false};
  /// @brief 是否因 Close 失败。
  bool closed{false};

  /** @brief 归还节点池前恢复「新建」状态。 */
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
  /// @brief 接收到的值；ready == false 时为空。
  std::optional<T> value;
  /// @brief 是否已有值可取（与 closed 互斥）。
  bool ready{false};
  /// @brief 是否因 Close 失败。
  bool closed{false};

  /** @brief 归还节点池前恢复「新建」状态。 */
  void Reset() noexcept {
    ResetBase();
    value.reset();
    ready = false;
    closed = false;
  }
};

/// @brief 判断一个等待节点是否已被唤醒（signal 不再是 kWaiting）。
inline bool FastNodeSignaled(const WaitNodeBase* node) noexcept {
  return node->signal.load(std::memory_order_acquire) !=
         static_cast<std::uint32_t>(WaitSignal::kWaiting);
}

/// @brief CPU 自旋提示（数据面短自旋用）。
inline void FastRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/**
 * @brief 一次唤醒的目标：只带出 futex 字地址，让 `FUTEX_WAKE` 在解锁后执行。
 *
 * 为什么不在锁内做系统调用：`FUTEX_WAKE` 是 syscall（~0.5~2µs），持锁期间做
 * 会把所有竞争方一起拖住。锁内只发布 `signal`/推进 `seq`，解锁后按这里记录
 * 的地址唤醒。节点在解锁后可能已被等待者归还池甚至复用，但 futex 字地址本身
 * 仍然有效（池节点不会被释放），最坏情况是一次 spurious 唤醒。
 */
struct FastWakeHint {
  std::atomic<std::uint32_t>* futex_word{nullptr};
};

/**
 * @brief 持锁发布一个等待节点的结果并准备唤醒。
 *
 * 调用契约：结果标志已写完；**必须在锁内调用**。原生线程只把 futex 字地址
 * 写进 `hint`（真正的 `FUTEX_WAKE` 由调用方解锁后执行）；受管 Fiber 的
 * `wake_or_cancel` 需要读节点上的 Scheduler/Task，因此仍在锁内完成。
 */
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
 * @brief 等待一个节点被唤醒；返回 false 表示当前上下文无法继续等待
 *        （park 失败），调用方必须摘链并返回 `kUnsupported`。
 *
 * 原生线程先做一小段无锁自旋（覆盖 1P1C 的短交接窗口，避免 futex 往返），
 * 再交给 `ChannelWaitNative`（内部还有自适应自旋 + per-node futex，无
 * deadline）；受管 Fiber 走 `Scheduler::park_wait`，spurious 唤醒后继续 park。
 */
inline bool FastWaitNode(WaitNodeBase* node) noexcept {
  if (!node->managed) {
    constexpr int kPreSpin = 1024;
    for (int i = 0; i < kPreSpin; ++i) {
      if (FastNodeSignaled(node)) {
        return true;
      }
      FastRelax();
    }
    ChannelWaitNative(node, std::nullopt);
    return true;
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

}  // namespace detail

/**
 * @brief 数据面专用、编译期固定容量的 MPMC 通道（单锁快路径）。
 *
 * @tparam T   载荷类型；要求 nothrow move 构造/赋值/默认构造/析构。
 * @tparam Cap 编译期容量；`0` 表示无缓冲 rendezvous。
 *
 * 线程安全：任意多个生产者/消费者可以并发 `Send/Recv/TrySend/TryRecv/Close`。
 * 对象本身较大（`Cap * sizeof(T)` 内联），建议静态/堆存储，不要放在小栈帧里。
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

  /// @brief 编译期容量（`0` 表示无缓冲）。
  static constexpr std::size_t kCapacity = Cap;

  FastChannel() = default;
  ~FastChannel() noexcept { Close(); }

  FastChannel(const FastChannel&) = delete;
  FastChannel& operator=(const FastChannel&) = delete;

  /** @brief 返回编译期容量。 */
  static constexpr std::size_t Capacity() noexcept { return Cap; }

  /** @brief 非阻塞发送；满时返回 `kWouldBlock`，关闭后返回 `kClosed`。 */
  FastStatus TrySend(T value) {
    if (m_closed.load(std::memory_order_acquire)) {
      return FastStatus::kClosed;
    }
    detail::FastWakeHint hint;
    FastStatus status = FastStatus::kWouldBlock;
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      status = PushLocked(value, &hint);
    }
    if (hint.futex_word != nullptr) {
      detail::ChannelFutexWake(hint.futex_word);
    }
    return status;
  }

  /** @brief 非阻塞接收；空时返回 `kWouldBlock`，关闭且排空后返回 `kClosed`。 */
  FastStatus TryRecv(T& out) {
    if (m_closed.load(std::memory_order_acquire) &&
        m_head.load(std::memory_order_acquire) ==
            m_tail.load(std::memory_order_acquire) &&
        m_waiting_senders.load(std::memory_order_acquire) == 0) {
      out = T{};
      return FastStatus::kClosed;
    }
    detail::FastWakeHint hint;
    FastStatus status = FastStatus::kWouldBlock;
    {
      std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
      status = PopLocked(out, &hint);
    }
    if (hint.futex_word != nullptr) {
      detail::ChannelFutexWake(hint.futex_word);
    }
    return status;
  }

  /**
   * @brief 阻塞发送：满时先自旋、再 park，直到有消费者取走一个值或通道关闭。
   * @return `kOk`、`kClosed`，或无法 park 时的 `kUnsupported`。
   */
  FastStatus Send(T value) {
    for (;;) {
      if (m_closed.load(std::memory_order_acquire)) {
        return FastStatus::kClosed;
      }
      if (HasSpaceHint() ||
          m_waiting_receivers.load(std::memory_order_acquire) != 0) {
        detail::FastWakeHint hint;
        FastStatus status = FastStatus::kWouldBlock;
        {
          std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
          status = PushLocked(value, &hint);
        }
        if (hint.futex_word != nullptr) {
          detail::ChannelFutexWake(hint.futex_word);
        }
        if (status != FastStatus::kWouldBlock) {
          return status;
        }
      }
      // 短自旋：1P1C 稳态在这里等到消费者腾出空位，完全避开 futex 往返。
      // 受管 Fiber 不自旋：自旋会占住 carrier M，必须直接交给 Scheduler park。
      if (detail::CanSpinWait() && SpinForSpace()) {
        continue;
      }
      // 长等待：登记发送者节点并 park。
      SendNode* const node = detail::WaitNodePool<SendNode>::Acquire();
      bool need_park = false;
      {
        detail::FastWakeHint hint;
        std::unique_lock<detail::ChannelSpinLock> lock(m_lock);
        // 窗口闭合：自旋期间条件可能已变化，持锁再试一次快路径。
        const FastStatus status = PushLocked(value, &hint);
        if (status != FastStatus::kWouldBlock) {
          lock.unlock();
          if (hint.futex_word != nullptr) {
            detail::ChannelFutexWake(hint.futex_word);
          }
          detail::WaitNodePool<SendNode>::Release(node);
          return status;
        }
        if (m_closed.load(std::memory_order_relaxed)) {
          lock.unlock();
          detail::WaitNodePool<SendNode>::Release(node);
          return FastStatus::kClosed;
        }
        node->value.emplace(std::move(value));
        if (!detail::CanSpinWait() && !detail::ChannelBindManaged(node)) {
          value = std::move(*node->value);
          node->value.reset();
          lock.unlock();
          detail::WaitNodePool<SendNode>::Release(node);
          return FastStatus::kUnsupported;
        }
        m_senders.push_back(node);
        m_waiting_senders.fetch_add(1, std::memory_order_relaxed);
        need_park = true;
        lock.unlock();
      }
      (void)need_park;
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
      // spurious 唤醒：回到循环重试。
    }
  }

  /**
   * @brief 阻塞接收：空时先自旋、再 park，直到有值可取或通道关闭（排空后）。
   * @param out 接收结果；返回 `kOk` 时写入新值，返回 `kClosed` 时写入零值。
   * @return `kOk`、`kClosed`，或无法 park 时的 `kUnsupported`。
   */
  FastStatus Recv(T& out) {
    for (;;) {
      if (m_closed.load(std::memory_order_acquire) && !HasDataHint() &&
          m_waiting_senders.load(std::memory_order_acquire) == 0) {
        out = T{};
        return FastStatus::kClosed;
      }
      if (HasDataHint() ||
          m_waiting_senders.load(std::memory_order_acquire) != 0) {
        detail::FastWakeHint hint;
        FastStatus status = FastStatus::kWouldBlock;
        {
          std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
          status = PopLocked(out, &hint);
        }
        if (hint.futex_word != nullptr) {
          detail::ChannelFutexWake(hint.futex_word);
        }
        if (status != FastStatus::kWouldBlock) {
          return status;
        }
      }
      if (detail::CanSpinWait() && SpinForData()) {
        continue;
      }
      RecvNode* const node = detail::WaitNodePool<RecvNode>::Acquire();
      {
        detail::FastWakeHint hint;
        std::unique_lock<detail::ChannelSpinLock> lock(m_lock);
        const FastStatus status = PopLocked(out, &hint);
        if (status != FastStatus::kWouldBlock) {
          lock.unlock();
          if (hint.futex_word != nullptr) {
            detail::ChannelFutexWake(hint.futex_word);
          }
          detail::WaitNodePool<RecvNode>::Release(node);
          return status;
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

  /**
   * @brief 关闭通道并唤醒全部等待者；幂等。
   *
   * 关闭后：`Send/TrySend` 返回 `kClosed`；`Recv/TryRecv` 先把缓冲里的值按
   * FIFO 排空，然后再返回 `kClosed`（并把输出参数写零，与 Go 的 close 语义
   * 一致）。
   */
  void Close() noexcept {
    std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    while (SendNode* const sender = m_senders.pop_front()) {
      sender->closed = true;
      detail::FastPublishNodeLocked(
          sender, static_cast<std::uint32_t>(detail::WaitSignal::kClosed),
          nullptr);
      // 冷路径：持锁唤醒可以接受（Close 不在吞吐热路径上）。
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

  /** @brief 是否已关闭。 */
  bool IsClosed() const noexcept {
    return m_closed.load(std::memory_order_acquire);
  }

  /** @brief 当前缓冲内元素数（不含正在交接的等待者）。 */
  std::size_t Len() const {
    std::lock_guard<detail::ChannelSpinLock> lock(m_lock);
    return m_count;
  }

 private:
  using SendNode = detail::FastSendNode<T>;
  using RecvNode = detail::FastRecvNode<T>;

  /// @brief 自旋预算（PAUSE 次数）；覆盖 1P1C 的短交接窗口。
  static constexpr int kSpinBudget = 64;

  /** @brief 无锁探测：缓冲是否还有空位（cap0 恒为 false）。 */
  bool HasSpaceHint() const noexcept {
    if constexpr (Cap == 0) {
      return false;
    } else {
      const std::size_t tail = m_tail.load(std::memory_order_acquire);
      const std::size_t head = m_head.load(std::memory_order_acquire);
      return tail - head < Cap;
    }
  }

  /** @brief 无锁探测：缓冲是否非空。 */
  bool HasDataHint() const noexcept {
    if constexpr (Cap == 0) {
      return false;
    } else {
      const std::size_t head = m_head.load(std::memory_order_acquire);
      const std::size_t tail = m_tail.load(std::memory_order_acquire);
      return head != tail;
    }
  }

  /** @brief 自旋等待「有空间/有接收者/已关闭」；返回 true 表示值得重试。 */
  bool SpinForSpace() const noexcept {
    for (int i = 0; i < kSpinBudget; ++i) {
      if (m_closed.load(std::memory_order_relaxed)) {
        return true;
      }
      if (HasSpaceHint() ||
          m_waiting_receivers.load(std::memory_order_acquire) != 0) {
        return true;
      }
      detail::FastRelax();
    }
    return false;
  }

  /** @brief 自旋等待「有数据/有发送者/已关闭」；返回 true 表示值得重试。 */
  bool SpinForData() const noexcept {
    for (int i = 0; i < kSpinBudget; ++i) {
      if (m_closed.load(std::memory_order_relaxed)) {
        return true;
      }
      if (HasDataHint() ||
          m_waiting_senders.load(std::memory_order_acquire) != 0) {
        return true;
      }
      detail::FastRelax();
    }
    return false;
  }

  /**
   * @brief 持锁的发送快路径：优先直接交接给等待的接收者，其次入缓冲。
   * @return `kOk` / `kClosed` / `kWouldBlock`（需要 park）。
   */
  FastStatus PushLocked(T& value, detail::FastWakeHint* hint) {
    if (m_closed.load(std::memory_order_relaxed)) {
      return FastStatus::kClosed;
    }
    if (!m_receivers.empty()) {
      // 直接交接：发送者把值写进接收者的节点，绕过缓冲（Go 的 send 快路径）。
      RecvNode* const receiver = m_receivers.pop_front();
      m_waiting_receivers.fetch_sub(1, std::memory_order_relaxed);
      receiver->value.emplace(std::move(value));
      receiver->ready = true;
      detail::FastPublishNodeLocked(
          receiver, static_cast<std::uint32_t>(detail::WaitSignal::kReady),
          hint);
      return FastStatus::kOk;
    }
    if constexpr (Cap > 0) {
      if (m_count < Cap) {
        m_slots[m_tail.load(std::memory_order_relaxed)].Construct(
            std::move(value));
        m_tail.store(NextIndex(m_tail.load(std::memory_order_relaxed)),
                     std::memory_order_release);
        ++m_count;
        return FastStatus::kOk;
      }
    }
    return FastStatus::kWouldBlock;
  }

  /**
   * @brief 持锁的接收快路径：优先出缓冲，其次从等待的发送者取。
   * @return `kOk` / `kClosed` / `kWouldBlock`（需要 park）。
   */
  FastStatus PopLocked(T& out, detail::FastWakeHint* hint) {
    if constexpr (Cap > 0) {
      if (m_count > 0) {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        out = std::move(*m_slots[head].Ptr());
        m_slots[head].Destroy();
        m_head.store(NextIndex(head), std::memory_order_release);
        --m_count;
        // 出队后空出一个槽位：把一个阻塞的发送者补进来（Go 的 recv 补位）。
        RefillLocked(hint);
        return FastStatus::kOk;
      }
    }
    if (!m_senders.empty()) {
      // 无缓冲（或缓冲空但有发送者）：直接与发送者会合。
      SendNode* const sender = m_senders.pop_front();
      m_waiting_senders.fetch_sub(1, std::memory_order_relaxed);
      out = std::move(*sender->value);
      sender->value.reset();
      sender->accepted = true;
      detail::FastPublishNodeLocked(
          sender, static_cast<std::uint32_t>(detail::WaitSignal::kReady),
          hint);
      return FastStatus::kOk;
    }
    if (m_closed.load(std::memory_order_relaxed)) {
      out = T{};
      return FastStatus::kClosed;
    }
    return FastStatus::kWouldBlock;
  }

  /** @brief 持锁：把队首发送者的值补进刚空出的缓冲槽位并唤醒它。 */
  void RefillLocked(detail::FastWakeHint* hint) noexcept {
    if constexpr (Cap > 0) {
      if (m_count >= Cap || m_senders.empty()) {
        return;
      }
      SendNode* const sender = m_senders.pop_front();
      m_waiting_senders.fetch_sub(1, std::memory_order_relaxed);
      const std::size_t tail = m_tail.load(std::memory_order_relaxed);
      m_slots[tail].Construct(std::move(*sender->value));
      sender->value.reset();
      m_tail.store(NextIndex(tail), std::memory_order_release);
      ++m_count;
      sender->accepted = true;
      detail::FastPublishNodeLocked(
          sender, static_cast<std::uint32_t>(detail::WaitSignal::kReady),
          hint);
    }
  }

  /// @brief 环形下标前进一格（Cap>0）。
  static constexpr std::size_t NextIndex(std::size_t index) noexcept {
    return index + 1 == Cap ? 0 : index + 1;
  }

  /// @brief 保护全部字段的单锁；单独一条 cacheline，避免与队列字段互相干扰。
  alignas(64) mutable detail::ChannelSpinLock m_lock;
  /// @brief 环形缓冲的读下标（锁内写；锁外 acquire 读用于自旋提示）。
  std::atomic<std::size_t> m_head{0};
  /// @brief 环形缓冲的写下标（锁内写；锁外 acquire 读用于自旋提示）。
  std::atomic<std::size_t> m_tail{0};
  /// @brief 缓冲内元素数（锁内维护）。
  std::size_t m_count{0};
  /// @brief 已关闭标记（atomic：TrySend/Recv 可以先做一次无锁快照）。
  std::atomic<bool> m_closed{false};
  /// @brief 阻塞中的发送者数量（锁内维护；锁外 acquire 读用于自旋提示）。
  std::atomic<std::uint32_t> m_waiting_senders{0};
  /// @brief 阻塞中的接收者数量（锁内维护；锁外 acquire 读用于自旋提示）。
  std::atomic<std::uint32_t> m_waiting_receivers{0};
  /// @brief 阻塞中的发送者队列（裸指针；节点由等待者线程持有）。
  detail::WaitQueue<SendNode> m_senders;
  /// @brief 阻塞中的接收者队列（裸指针；节点由等待者线程持有）。
  detail::WaitQueue<RecvNode> m_receivers;
  /// @brief 编译期定长缓冲；Cap==0 时为空数组，不占存储。
  alignas(64) std::array<detail::FastSlot<T>, Cap> m_slots;
};

/**
 * @brief 单生产者/单消费者无锁 ring（数据面默认实现）。
 *
 * @tparam T   载荷类型；要求 nothrow move 构造/赋值/默认构造/析构。
 * @tparam Cap 编译期容量；必须 ≥ 1。
 *
 * ## 数据路径
 *
 * `TrySend` 只写 `m_tail`（生产者独占的 cacheline）与自己的槽位；`TryRecv`
 * 只写 `m_head`（消费者独占）。两者通过 acquire/release 建立 happens-before：
 * 生产者先构造槽位、再 release 发布 `tail`；消费者 acquire 读 `tail` 后才读
 * 槽位。因此热路径没有锁、没有 CAS、没有跨核 RMW —— 这是它比通用
 * `FastChannel`/Go `chan` 快的原因。
 *
 * ## 阻塞路径
 *
 * 缓冲满/空时才用一把 `std::mutex` 登记等待者（每方向至多一个），唤醒也在
 * 同一把锁内完成。热路径只多一次 `atomic<WaitNodeBase*>` 的 relaxed load，
 * 用于判断「需不需要叫醒睡着的那一方」。
 *
 * ## 使用约束
 *
 * 同一时刻至多一个生产者、一个消费者。多生产者/多消费者请用
 * `FastChannel`；对 `m_tail` 的并发写是未定义行为。
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

  /// @brief 编译期容量。
  static constexpr std::size_t kCapacity = Cap;

  FastSpscChannel() = default;
  ~FastSpscChannel() noexcept { Close(); }

  FastSpscChannel(const FastSpscChannel&) = delete;
  FastSpscChannel& operator=(const FastSpscChannel&) = delete;

  /** @brief 返回编译期容量。 */
  static constexpr std::size_t Capacity() noexcept { return Cap; }

  /**
   * @brief 非阻塞发送（仅生产者调用）。
   * @return `kOk` / `kWouldBlock`（满） / `kClosed`。
   */
  FastStatus TrySend(T value) {
    if (m_closed.load(std::memory_order_acquire)) {
      return FastStatus::kClosed;
    }
    return Push(value) ? FastStatus::kOk : FastStatus::kWouldBlock;
  }

  /**
   * @brief 非阻塞接收（仅消费者调用）。
   * @return `kOk` / `kWouldBlock`（空） / `kClosed`。
   */
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

  /**
   * @brief 阻塞发送（仅生产者调用）：满时 park，直到消费者取走或关闭。
   * @return `kOk` / `kClosed` / `kUnsupported`。
   */
  FastStatus Send(T value) {
    for (;;) {
      if (m_closed.load(std::memory_order_acquire)) {
        return FastStatus::kClosed;
      }
      if (Push(value)) {
        return FastStatus::kOk;
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
        // 持锁重检：避免在「重检 → 注册」窗口里错过消费者腾出的空位。
        if (m_closed.load(std::memory_order_acquire)) {
          return FastStatus::kClosed;
        }
        if (HasSpace()) {
          continue;
        }
        m_sender_waiter.store(&node, std::memory_order_release);
        // 窗口闭合：消费者可能在本线程 store 之前取走数据，此时它读到的
        // waiter 是 null、不会唤醒；若本线程直接 park 就会睡死在一个已经
        // 非满的 ring 上。store 之后重检一次，满足则撤销注册并重试。
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
      // 被数据送达唤醒：回到循环重试（值还没被移动）。
    }
  }

  /**
   * @brief 阻塞接收（仅消费者调用）：空时 park，直到有值或关闭（排空后）。
   * @param out 接收结果；返回 `kOk` 时写入新值，返回 `kClosed` 时写入零值。
   * @return `kOk` / `kClosed` / `kUnsupported`。
   */
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
        // 持锁重检只能读 head/tail，不能调用 Pop：Pop 成功时会唤醒发送者，
        // 而唤醒需要同一把 m_wait_mutex（非递归），会立刻自死锁。
        if (m_closed.load(std::memory_order_acquire)) {
          out = T{};
          return FastStatus::kClosed;
        }
        if (m_head.load(std::memory_order_acquire) !=
            m_tail.load(std::memory_order_acquire)) {
          continue;  // 数据已到：解锁后回循环顶部 Pop
        }
        m_receiver_waiter.store(&node, std::memory_order_release);
        // 窗口闭合（与 Send 对称）：生产者可能在 store 之前 Push，且看到
        // 的 waiter 是 null；store 之后重检 ring，非空就撤销注册并重试。
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
      // 被数据送达唤醒：回到循环重试。
    }
  }

  /**
   * @brief 关闭通道并唤醒两个方向上阻塞的等待者；幂等。
   *
   * 关闭后 `Send/TrySend` 返回 `kClosed`；`Recv/TryRecv` 先排空 ring 再返回
   * `kClosed`。与并发的 `Send` 的边界：调用方应保证关闭后不再有生产者发送
   * （Go 的 close 与 send 并发同样是未定义行为）。
   */
  void Close() noexcept {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    std::lock_guard<std::mutex> lock(m_wait_mutex);
    WakeLocked(m_sender_waiter);
    WakeLocked(m_receiver_waiter);
  }

  /** @brief 是否已关闭。 */
  bool IsClosed() const noexcept {
    return m_closed.load(std::memory_order_acquire);
  }

  /** @brief 当前 ring 内元素数（近似值；与生产者并发时可能立刻变化）。 */
  std::size_t Len() const noexcept {
    const std::size_t head = m_head.load(std::memory_order_acquire);
    const std::size_t tail = m_tail.load(std::memory_order_acquire);
    return tail - head;
  }

 private:
  /// @brief 阻塞前的短自旋预算（PAUSE 次数）。
  static constexpr int kSpscSpin = 4096;

  /// @brief 栈上的等待节点：生命周期由等待者栈帧保证，唤醒方在 mutex 内使用。
  struct SpscWaiter final : detail::WaitNodeBase {
    void Reset() noexcept { ResetBase(); }
  };

  /** @brief 生产者：写入槽位并发布 tail；返回 false 表示 ring 已满。 */
  bool Push(T& value) {
    const std::size_t tail = m_tail.load(std::memory_order_relaxed);
    const std::size_t head = m_head.load(std::memory_order_acquire);
    if (tail - head >= Cap) {
      return false;
    }
    m_slots[tail % Cap].Construct(std::move(value));
    m_tail.store(tail + 1, std::memory_order_release);
    if (tail == head) {
      // 由空变非空：可能有消费者在睡；没有等待者时只多一次 relaxed load。
      WakeWaiter(m_receiver_waiter);
    }
    return true;
  }

  /** @brief 消费者：读取槽位并发布 head；返回 false 表示 ring 已空。 */
  bool Pop(T& out) {
    const std::size_t head = m_head.load(std::memory_order_relaxed);
    const std::size_t tail = m_tail.load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }
    out = std::move(*m_slots[head % Cap].Ptr());
    m_slots[head % Cap].Destroy();
    m_head.store(head + 1, std::memory_order_release);
    if (tail - head == Cap) {
      // 由满变非满：可能有生产者在睡。
      WakeWaiter(m_sender_waiter);
    }
    return true;
  }

  bool HasSpace() const noexcept {
    const std::size_t tail = m_tail.load(std::memory_order_relaxed);
    const std::size_t head = m_head.load(std::memory_order_acquire);
    return tail - head < Cap;
  }

  /// @brief SPSC 阻塞前的短自旋：等消费者前进 head（生产者侧）。
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

  /// @brief SPSC 阻塞前的短自旋：等生产者前进 tail（消费者侧）。
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

  /**
   * @brief 唤醒一个等待者；只在「状态可能从不可用变为可用」时调用。
   *
   * 热路径先做一次 relaxed load，没有等待者就直接返回；有等待者时进 mutex
   * 取走指针，在 mutex 内完成全部节点访问（`signal`/`seq`/park/wake），
   * 这样等待者注销并销毁栈上节点后不会再有唤醒方访问它。
   */
  void WakeWaiter(std::atomic<detail::WaitNodeBase*>& slot) {
    if (slot.load(std::memory_order_relaxed) == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(m_wait_mutex);
    WakeLocked(slot);
  }

  /** @brief 持 `m_wait_mutex` 调用：取走等待者并唤醒。 */
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

  /**
   * @brief 注销等待者：与唤醒方在 `m_wait_mutex` 内串行化。
   *
   * 唤醒方取走指针后会在同一把锁内完成唤醒，因此本函数返回后节点可以被安全
   * 销毁/复用（不会再有任何访问）。
   */
  void Unregister(std::atomic<detail::WaitNodeBase*>& slot,
                  detail::WaitNodeBase* node) noexcept {
    std::lock_guard<std::mutex> lock(m_wait_mutex);
    detail::WaitNodeBase* const registered =
        slot.load(std::memory_order_acquire);
    if (registered == node) {
      slot.store(nullptr, std::memory_order_release);
    }
  }

  /// @brief 生产者独占写、消费者读：ring 尾（单独一条 cacheline）。
  alignas(64) std::atomic<std::size_t> m_tail{0};
  /// @brief 消费者独占写、生产者读：ring 头（单独一条 cacheline）。
  alignas(64) std::atomic<std::size_t> m_head{0};
  /// @brief 关闭标记（单独一条 cacheline，读多写少）。
  alignas(64) std::atomic<bool> m_closed{false};
  /// @brief 阻塞路径互斥量：只保护两个等待者指针与唤醒协议。
  alignas(64) mutable std::mutex m_wait_mutex;
  /// @brief 阻塞中的生产者（仅一个；热路径 relaxed load）。
  std::atomic<detail::WaitNodeBase*> m_sender_waiter{nullptr};
  /// @brief 阻塞中的消费者（仅一个；热路径 relaxed load）。
  std::atomic<detail::WaitNodeBase*> m_receiver_waiter{nullptr};
  /// @brief 编译期定长 ring。
  alignas(64) std::array<detail::FastSlot<T>, Cap> m_slots;
};

}  // namespace go2cpp
