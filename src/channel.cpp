#include "go2cpp/channel.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace go2cpp {
namespace {

/// 函数功能：完成 make_channel_error 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] message 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr make_channel_error(const char* message) noexcept {
  try {
    return NewError(message);
  } catch (...) {
    // 错误对象只用于诊断；分配失败时仍保持 channel 的状态机完整。
    return {};
  }
}

}  // namespace

/// 函数功能：完成 ChannelClosedError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr ChannelClosedError() noexcept {
  static const ErrorPtr error = make_channel_error("channel closed");
  return error;
}

/// 函数功能：完成 ChannelAlreadyClosedError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr ChannelAlreadyClosedError() noexcept {
  static const ErrorPtr error = make_channel_error("channel already closed");
  return error;
}

/// 函数功能：完成 ChannelWouldBlockError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr ChannelWouldBlockError() noexcept {
  static const ErrorPtr error = make_channel_error("channel operation would block");
  return error;
}

/// 函数功能：完成 ChannelNilError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr ChannelNilError() noexcept {
  static const ErrorPtr error = make_channel_error("nil channel");
  return error;
}

/// 函数功能：完成 ChannelTimeoutError 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ErrorPtr ChannelTimeoutError() noexcept {
  static const ErrorPtr error = make_channel_error("channel operation timed out");
  return error;
}

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
                      static_cast<bool>(item.arm);
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

    bool has_poll_case = false;
    auto wait_state = std::make_shared<detail::SelectWaitState>();
    try {
      for (std::size_t index = 0; index < cases.size(); ++index) {
        if (!cases[index].arm) {
          has_poll_case = has_poll_case ||
                          (!cases[index].is_default && cases[index].probe);
          continue;
        }
        cases[index].arm(wait_state, index);
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
