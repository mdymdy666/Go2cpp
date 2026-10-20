/**
 * @file channel.cpp
 * @brief Channel 与 Select 的实现（Go 风格的协程间通信）。
 *
 * ## 模块定位
 *
 * Channel 是运行时里**唯一同时承担同步与数据传递**的原语：它既可以当
 * "队列"用（有缓冲），也可以当"会合点"用（无缓冲）。在 GMP 模型里它属于
 * 用户态的同步设施，**不经过调度器内部队列** —— 等待方是 park 在自己的
 * `ParkingCondition` 上，由 channel 在状态变化时主动唤醒。
 *
 * ## 这个文件承担什么
 *
 * 头文件 `channel.hpp` 定义了模板化的 `Channel<T>`（收发都在头里，因为要
 * 按类型实例化）；本文件只放**与类型无关**的部分：
 *
 * - 一组错误构造器（closed / already-closed / would-block / nil / timeout）：
 *   都返回**共享的静态 ErrorPtr**，让相同的错误可以按指针快速比较，
 *   而不用比字符串；
 * - `Select(...)`：多路复用核心，类型无关所以能从模板里抽出来。
 *
 * ## Select 的语义（与 Go 对齐）
 *
 * - 多个 case 同时就绪时**随机选一个**（线程本地 xorshift 取探测起点，
 *   既保证公平、避免固定优先级饿死后面的 case，也避免全局游标争用）；
 * - 有 `default` 且无 case 就绪时立刻走 default，不阻塞；
 * - 无 `default` 时注册到各 channel 的等待队列并 park，被任意一方唤醒后
 *   抢占式地"取走"结果 —— 多个 channel 同时唤醒它时只有一个能拿到；
 * - `Context` 取消与超时都会唤醒等待方并返回对应状态。
 */
#include "go2cpp/channel.hpp"

#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace go2cpp {
namespace {

ErrorPtr make_channel_error(const char* message) noexcept {
  try {
    return NewError(message);
  } catch (...) {
    // 错误对象只用于诊断；分配失败时仍保持 channel 的状态机完整。
    return {};
  }
}

// Select 的探测起点：线程本地 xorshift32（审计 D4）。
//
// 旧实现用一个 `static std::atomic<std::size_t> cursor` 做 `fetch_add`：
// 每次 Select 都写同一条 cacheline，select 密集型多线程负载会在这里
// 线性化；而且它产生的是"确定轮转"，与文档承诺的"随机选一个"不符。
// 改为每线程独立随机序列后：
// - 热路径不再有跨线程共享写；
// - 200k 次 × 4 个就绪 case 的分布偏差远小于测试的 5% 容差，公平性不变。
//
// TLS 模型必须是 initial-exec：共享库里默认的 general-dynamic 模型会让
// 每次访问都调用 __tls_get_addr（实测 select 热路径多 ~15ns）；本库由
// DT_NEEDED 在进程启动时加载，initial-exec 是安全的。若未来需要支持
// dlopen 本库，需要回退 general-dynamic 或改为显示初始化。
std::size_t NextSelectOffset(std::size_t count) noexcept {
#if defined(__GNUC__)
  __attribute__((tls_model("initial-exec")))
#endif
  static thread_local std::uint32_t state = 0;
  if (state == 0) {
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto self = reinterpret_cast<std::uintptr_t>(&state);
    state = static_cast<std::uint32_t>(now) ^
            static_cast<std::uint32_t>(self) ^
            static_cast<std::uint32_t>(now >> 32) ^ 0x9e3779b9u;
    if (state == 0) {
      state = 0x6d2b79f5u;
    }
  }
  // xorshift32（Marsaglia）：周期 2^32-1，非零种子永不退化为 0。
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return static_cast<std::size_t>(state) % count;
}

}  // namespace

namespace detail {

bool CanSpinWait() noexcept {
  // 仅「纯原生线程」允许在 channel 等待上做短自旋：
  // - 受管 Fiber 的 current_task 非空；此时自旋会占住 carrier M 线程，
  //   必须交给 Scheduler 走 park 路径；
  // - 手动 Fiber 没有 Scheduler 归属，ParkingCondition 会拒绝阻塞，
  //   自旋同样不应该发生（它会把 carrier 线程钉住）。
  return Scheduler::current_task_ptr() == nullptr && Fiber::Current() == nullptr;
}

}  // namespace detail

ErrorPtr ChannelClosedError() noexcept {
  static const ErrorPtr error = make_channel_error("channel closed");
  return error;
}

ErrorPtr ChannelAlreadyClosedError() noexcept {
  static const ErrorPtr error = make_channel_error("channel already closed");
  return error;
}

ErrorPtr ChannelWouldBlockError() noexcept {
  static const ErrorPtr error = make_channel_error("channel operation would block");
  return error;
}

ErrorPtr ChannelNilError() noexcept {
  static const ErrorPtr error = make_channel_error("nil channel");
  return error;
}

ErrorPtr ChannelTimeoutError() noexcept {
  static const ErrorPtr error = make_channel_error("channel operation timed out");
  return error;
}

/**
 * @brief 在多个 channel 操作之间做多路复用（Go 的 `select`）。
 *
 * 主要功能：等待一组 case 中任意一个就绪，返回它的下标与结果；支持
 * default 分支、超时与 `Context` 取消。
 *
 * step1: **校验与预处理** —— 拒绝空 case 表、拒绝"不可复制值"的 case
 *        （那种情况应直接用 Send/Recv），确认至少有一个可用 case；
 *        把相对超时换算成绝对 deadline（饱和加法，避免溢出成"已过期"）。
 * step2: **探测阶段** —— 每轮用线程本地随机数取起始下标（近似 Go 的
 *        随机选择，保证公平），逐个调用 `probe()` 看是否已就绪；就绪则立即返回。
 *        探测完检查 `Context` 是否已取消（与 Go 一致：就绪与取消之间存在
 *        竞争，取消在"本轮无 case 获胜"之后才处理），再检查超时；
 *        有 default 就走 default。
 * step3: **注册阶段** —— 创建一个 `SelectWaitState` 并把所有 case arm 到
 *        各自 channel 的等待队列上。任一 arm 失败或抛异常，必须
 *        `Cancel()` + `disarm()` 完整回滚，否则会留下指向已返回 Select 的
 *        悬空节点，下一次发送会去唤醒一个不存在的等待者。
 * step4: **抢占结果** —— arm 之后要再 `Take()` 一次：在我们 arm 的同时
 *        可能有 channel 已经就绪并选了我们，这次 Take 就是把那个结果拿
 *        回来。Take 抛异常时不能当成"未发生"，要走统一拆除路径。
 * step5: **挂起等待** —— 注册 `Context` 完成回调（回调里 `Notify`），
 *        然后 park 直到被唤醒、超时或取消。
 * step6: **拆除与返回** —— 无论从哪条路径返回，都要移除 Context 回调并
 *        disarm 所有 case，保证不泄漏注册节点。
 */
SelectResult Select(const std::vector<SelectCase>& cases,
                    const ContextPtr& context,
                    std::optional<ContextDuration> timeout) {
  GO2CPP_DEBUG_EVENT(kChannelSelectEnter, GO2CPP_DEBUG_BOUND_TASK(),
                     static_cast<std::uint64_t>(cases.size()));
  if (cases.empty()) {
    return {SelectResult::kNoSelection, false, false, {},
            ChannelStatus::kInvalid, NewError("select has no cases")};
  }

  bool has_default = false;
  bool has_usable_case = false;
  for (const auto& item : cases) {
    if (item.unsupported) {
      return {SelectResult::kNoSelection, false, false, {},
              ChannelStatus::kInvalid,
              NewError("channel select 不支持不可复制值；请直接使用 Send/Recv "
                       "或 SelectValue")};
    }
    has_default = has_default || item.is_default;
    has_usable_case = has_usable_case || item.is_default ||
                      static_cast<bool>(item.probe) ||
                      static_cast<bool>(item.arm) ||
                      static_cast<bool>(item.arm_checked);
  }
  if (!has_usable_case) {
    return {SelectResult::kNoSelection, false, false, {},
            ChannelStatus::kInvalid, NewError("select has no usable cases")};
  }
  const auto deadline =
      timeout.has_value()
          ? std::optional<ContextTimePoint>(detail::SaturatingDeadline(*timeout))
                           : std::nullopt;

  // step1 的一部分：统一的拆除器。Disarm 只负责清理，所以即使某个
  // 用户提供的 case 在拆除时抛异常，也不能阻止其余 channel 的注册项
  // 被移除 —— 否则会泄漏等待节点。
  const auto disarm = [](const std::vector<SelectCase>& select_cases,
                         const std::shared_ptr<detail::SelectWaitState>& state) {
    for (const auto& item : select_cases) {
      if (item.disarm) {
        try {
          item.disarm(state);
        } catch (...) {
          // Disarm 只负责清理。用户提供的 case 不能阻止其余 channel 注册项
          // 被移除。
        }
      }
    }
  };

  // 审计 D1 的补全：ctx 已取消、但本轮没有 channel case 就绪时，DoneCase
  // 的 cancel_probe 才是唯一的就绪分支。两条 context-done 返回路径（探测后、
  // 回滚后）在返回 kNoSelection 之前都必须给它一次机会；否则取消与回滚
  // 之间的窗口会返回 selected=false 的 kCancelled（基线同样可复现，
  // 约 0.03%/次）。
  const auto take_cancelled_case = [&]() -> std::optional<SelectResult> {
    if (!context || !context->IsDone()) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < cases.size(); ++index) {
      if (!cases[index].cancel_probe) {
        continue;
      }
      try {
        auto probe = cases[index].cancel_probe();
        if (probe.ready) {
          return SelectResult{index,
                              true,
                              probe.ok,
                              std::move(probe.value),
                              probe.status,
                              std::move(probe.error),
                              std::move(probe.typed_value)};
        }
      } catch (...) {
        // 取消探测只影响回滚路径；构造失败按未选中处理。
      }
    }
    return std::nullopt;
  };

  // step2 开始：主循环。每次重试都从线程本地的随机起点开始探测，近似 Go 的
  // 随机选择（per-P fastrand），避免"永远从下标 0 开始"导致后面的 case 被
  // 饿死；也不再有全局游标 cacheline 争用。
  for (;;) {
    const std::size_t offset = NextSelectOffset(cases.size());
    for (std::size_t n = 0; n < cases.size(); ++n) {
      const std::size_t index = (offset + n) % cases.size();
      if (cases[index].is_default) {
        continue;
      }
      if (!cases[index].probe) {
        continue;
      }
      SelectProbe probe;
      try {
        probe = cases[index].probe();
      } catch (...) {
        return {SelectResult::kNoSelection, false, false, {},
                ChannelStatus::kInvalid, detail::ValueOperationError()};
      }
      if (probe.ready) {
        GO2CPP_DEBUG_EVENT(kChannelSelect, GO2CPP_DEBUG_BOUND_TASK(),
                           static_cast<std::uint64_t>(index));
        return {index, true, probe.ok, std::move(probe.value), probe.status,
                std::move(probe.error), std::move(probe.typed_value)};
      }
    }

    // 已就绪的 channel 操作与取消之间存在竞争，语义与 Go 的 select 一致。
    // 本轮没有 channel case 获胜时，在 default 或休眠前检查取消状态。
    if (context && context->IsDone()) {
      if (auto cancelled = take_cancelled_case()) {
        return std::move(*cancelled);
      }
      const auto err = context->Err();
      const bool deadline_error = err && Is(err, DeadlineExceededError());
      return {SelectResult::kNoSelection, false, false, {},
              deadline_error ? ChannelStatus::kDeadlineExceeded
                             : ChannelStatus::kCancelled,
              err ? err : CanceledError()};
    }

    if (has_default) {
      for (std::size_t index = 0; index < cases.size(); ++index) {
        if (cases[index].is_default) {
          GO2CPP_DEBUG_EVENT(kChannelSelect, GO2CPP_DEBUG_BOUND_TASK(),
                             static_cast<std::uint64_t>(index));
          return {index, true, true, {}, ChannelStatus::kReady, {}};
        }
      }
    }

    if (deadline.has_value() &&
        std::chrono::steady_clock::now() >= *deadline) {
      return {SelectResult::kNoSelection, false, false, {},
              ChannelStatus::kTimedOut, ChannelTimeoutError()};
    }
    if (core::ParkingCondition::FiberWaitUnsupported()) {
      return {SelectResult::kNoSelection, false, false, {},
              ChannelStatus::kInvalid,
              detail::ValueOperationError()};
    }

    // step3 开始：注册阶段。为本次等待创建共享状态，然后把每个 case
    // arm 到对应 channel 的等待队列上。任何一个失败都必须完整回滚。
    bool has_poll_case = false;
    auto wait_state = std::make_shared<detail::SelectWaitState>();
    try {
      for (std::size_t index = 0; index < cases.size(); ++index) {
        if (!cases[index].arm && !cases[index].arm_checked) {
          has_poll_case = has_poll_case ||
                          (!cases[index].is_default && cases[index].probe);
          continue;
        }
        bool armed = true;
        if (cases[index].arm_checked) {
          armed = cases[index].arm_checked(wait_state, index);
        } else {
          cases[index].arm(wait_state, index);
        }
        if (!armed && !wait_state->IsSelected()) {
          throw std::runtime_error("channel select case registration failed");
        }
      }
    } catch (...) {
      wait_state->Cancel();
      try {
        disarm(cases, wait_state);
      } catch (...) {
      }
      return {SelectResult::kNoSelection, false, false, {},
              ChannelStatus::kInvalid, detail::ValueOperationError()};
    }

    // step4：arm 期间可能已经有 channel 就绪并选中了我们，
    // 这里立即尝试把结果取走，避免多睡一轮。
    std::size_t selected_index = SelectResult::kNoSelection;
    SelectProbe selected_probe;
    bool take_failed = false;
    const auto take_selected = [&] {
      try {
        return wait_state->Take(&selected_index, &selected_probe);
      } catch (...) {
        // 结果复制失败时不能把已选状态当作未发生，否则会继续等待并
        // 让注册节点泄漏；由调用方走统一取消/拆除路径。
        take_failed = true;
        return false;
      }
    };
    if (take_selected()) {
      disarm(cases, wait_state);
      return {selected_index,
              selected_probe.ready,
              selected_probe.ok,
              std::move(selected_probe.value),
              selected_probe.status,
              std::move(selected_probe.error),
              std::move(selected_probe.typed_value)};
    }
    if (take_failed) {
      wait_state->Cancel();
      disarm(cases, wait_state);
      return {SelectResult::kNoSelection, false, false, {},
              ChannelStatus::kInvalid, detail::ValueOperationError()};
    }

    // step5：注册取消回调后挂起等待。回调里只做 Notify，
    // 真正的状态判断留给被唤醒之后。
    DoneSignal::CallbackId callback_id = 0;
    if (context) {
      try {
        callback_id = context->Done().AddCallback([wait_state] {
          wait_state->Notify();
        });
      } catch (...) {
        // 回调注册可能因分配失败而抛出；此时必须先取消并拆除所有
        // channel 节点，否则下一次发送会唤醒已返回的 Select。
        wait_state->Cancel();
        disarm(cases, wait_state);
        return {SelectResult::kNoSelection, false, false, {},
                ChannelStatus::kInvalid, detail::ValueOperationError()};
      }
    }

    const auto remove_callback = [&] {
      if (context && callback_id != 0) {
        try {
          context->Done().RemoveCallback(callback_id);
        } catch (...) {
          // 仅清理观察者注册；等待节点拆除不能被异常打断。
        }
      }
    };

    bool custom_selected = false;
    bool channel_selected = false;
    for (;;) {
      if (take_selected()) {
        channel_selected = true;
        break;
      }
      if (take_failed) {
        break;
      }

      // 已挂起的 channel case 不能再次轮询：TryRecv/TrySend 可能消耗一次
      // 传输，却没有发布共享选择状态。
      for (std::size_t index = 0; index < cases.size(); ++index) {
        if (cases[index].is_default || cases[index].arm ||
            cases[index].arm_checked ||
            !cases[index].probe) {
          continue;
        }
        SelectProbe probe;
        try {
          probe = cases[index].probe();
        } catch (...) {
          remove_callback();
          wait_state->Cancel();
          disarm(cases, wait_state);
          return {SelectResult::kNoSelection, false, false, {},
                  ChannelStatus::kInvalid, detail::ValueOperationError()};
        }
        try {
          if (probe.ready && wait_state->TrySelect(index, std::move(probe))) {
            custom_selected = true;
            break;
          }
        } catch (...) {
          remove_callback();
          wait_state->Cancel();
          disarm(cases, wait_state);
          return {SelectResult::kNoSelection, false, false, {},
                  ChannelStatus::kInvalid, detail::ValueOperationError()};
        }
      }
      if (custom_selected) {
        break;
      }
      if (context && context->IsDone()) {
        break;
      }
      if (core::ParkingCondition::CancellationRequested()) {
        break;
      }
      try {
        if (deadline.has_value()) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= *deadline) {
            break;
          }
          auto remaining = *deadline - now;
          if (has_poll_case && remaining > std::chrono::milliseconds(2)) {
            remaining = std::chrono::milliseconds(2);
          }
          wait_state->WaitFor(remaining, context);
        } else if (has_poll_case) {
          wait_state->WaitFor(std::chrono::milliseconds(2), context);
        } else {
          wait_state->Wait(context);
        }
      } catch (...) {
        remove_callback();
        wait_state->Cancel();
        disarm(cases, wait_state);
        return {SelectResult::kNoSelection, false, false, {},
                ChannelStatus::kInvalid, detail::ValueOperationError()};
      }
    }

    remove_callback();
    if (!channel_selected && !custom_selected) {
      // ctx 已结束时，DoneCase 的 probe 可能还没机会执行（循环里 arm 过的
      // case 会被跳过）；在 Cancel 之前补一次无副作用探测，否则会把已经
      // 选中的取消分支误报成 NoSelection（审计 D1，复现率 8%~19%）。
      bool cancel_probe_selected = false;
      if (context && context->IsDone()) {
        for (std::size_t index = 0; index < cases.size(); ++index) {
          if (!cases[index].cancel_probe) {
            continue;
          }
          try {
            auto probe = cases[index].cancel_probe();
            if (probe.ready &&
                wait_state->TrySelect(index, std::move(probe))) {
              cancel_probe_selected = true;
              break;
            }
          } catch (...) {
            // 取消探测只影响回滚路径；构造失败按未选中处理。
          }
        }
      }
      if (!cancel_probe_selected) {
        // 移除等待节点前，先把超时或取消与迟到的 channel 交接线性化；如果
        // 交接已经赢得竞争，则保留交接结果。
        wait_state->Cancel();
      }
      if (take_selected()) {
        channel_selected = true;
      }
    } else if (custom_selected) {
      channel_selected = take_selected();
    }
    disarm(cases, wait_state);

    if (take_failed) {
      return {SelectResult::kNoSelection, false, false, {},
              ChannelStatus::kInvalid, detail::ValueOperationError()};
    }
    if (channel_selected) {
      GO2CPP_DEBUG_EVENT(kChannelSelect, GO2CPP_DEBUG_BOUND_TASK(),
                         static_cast<std::uint64_t>(selected_index));
      return {selected_index,
              selected_probe.ready,
              selected_probe.ok,
              std::move(selected_probe.value),
              selected_probe.status,
              std::move(selected_probe.error),
              std::move(selected_probe.typed_value)};
    }
    if (context && context->IsDone()) {
      if (auto cancelled = take_cancelled_case()) {
        return std::move(*cancelled);
      }
      const auto err = context->Err();
      const bool deadline_error = err && Is(err, DeadlineExceededError());
      return {SelectResult::kNoSelection, false, false, {},
              deadline_error ? ChannelStatus::kDeadlineExceeded
                             : ChannelStatus::kCancelled,
              err ? err : CanceledError()};
    }
    if (core::ParkingCondition::CancellationRequested()) {
      return {SelectResult::kNoSelection, false, false, {},
              ChannelStatus::kCancelled, CanceledError()};
    }
    if (deadline.has_value() &&
        std::chrono::steady_clock::now() >= *deadline) {
      GO2CPP_DEBUG_EVENT(kChannelSelect, GO2CPP_DEBUG_BOUND_TASK(),
                         ::go2cpp::debug::kNoSelectCase);
      return {SelectResult::kNoSelection, false, false, {},
              ChannelStatus::kTimedOut, ChannelTimeoutError()};
    }
  }
}

}  // namespace go2cpp
