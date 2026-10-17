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
 * - 多个 case 同时就绪时**随机选一个**（用轮转游标近似公平，避免固定优先
 *   级饿死后面的 case）；
 * - 有 `default` 且无 case 就绪时立刻走 default，不阻塞；
 * - 无 `default` 时注册到各 channel 的等待队列并 park，被任意一方唤醒后
 *   抢占式地"取走"结果 —— 多个 channel 同时唤醒它时只有一个能拿到；
 * - `Context` 取消与超时都会唤醒等待方并返回对应状态。
 */
#include "go2cpp/channel.hpp"

#include <atomic>
#include <chrono>
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

}  // namespace

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
 * step2: **探测阶段** —— 每轮用 `cursor` 轮转起始下标（近似随机，保证
 *        公平），逐个调用 `probe()` 看是否已就绪；就绪则立即返回。
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
  static std::atomic<std::size_t> cursor{0};

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

  // step2 开始：主循环。每次重试都换一个轮转起点，近似 Go 的随机选择，
  // 避免"永远从下标 0 开始"导致后面的 case 被饿死。
  for (;;) {
    const std::size_t offset = cursor.fetch_add(1, std::memory_order_relaxed);
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
        return {index, true, probe.ok, std::move(probe.value), probe.status,
                std::move(probe.error), std::move(probe.typed_value)};
      }
    }

    // 已就绪的 channel 操作与取消之间存在竞争，语义与 Go 的 select 一致。
    // 本轮没有 channel case 获胜时，在 default 或休眠前检查取消状态。
    if (context && context->IsDone()) {
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
      // 移除等待节点前，先把超时或取消与迟到的 channel 交接线性化；如果
      // 交接已经赢得竞争，则保留交接结果。
      wait_state->Cancel();
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
      return {selected_index,
              selected_probe.ready,
              selected_probe.ok,
              std::move(selected_probe.value),
              selected_probe.status,
              std::move(selected_probe.error),
              std::move(selected_probe.typed_value)};
    }
    if (context && context->IsDone()) {
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
      return {SelectResult::kNoSelection, false, false, {},
              ChannelStatus::kTimedOut, ChannelTimeoutError()};
    }
  }
}

}  // namespace go2cpp
