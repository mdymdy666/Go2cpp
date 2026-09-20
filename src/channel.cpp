#include "go2cpp/channel.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace go2cpp {

ErrorPtr ChannelClosedError() {
  static const ErrorPtr error = NewError("channel closed");
  return error;
}

ErrorPtr ChannelAlreadyClosedError() {
  static const ErrorPtr error = NewError("channel already closed");
  return error;
}

ErrorPtr ChannelWouldBlockError() {
  static const ErrorPtr error = NewError("channel operation would block");
  return error;
}

ErrorPtr ChannelNilError() {
  static const ErrorPtr error = NewError("nil channel");
  return error;
}

ErrorPtr ChannelTimeoutError() {
  static const ErrorPtr error = NewError("channel operation timed out");
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
        item.disarm(state);
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
      auto probe = cases[index].probe();
      if (probe.ready) {
        return {index, true, probe.ok, std::move(probe.value), probe.status,
                std::move(probe.error)};
      }
    }

    // A channel operation that was ready competes with cancellation just as
    // it does in Go's select. Check cancellation before default or sleeping
    // when no channel case won this round.
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

    bool has_poll_case = false;
    auto wait_state = std::make_shared<detail::SelectWaitState>();
    for (std::size_t index = 0; index < cases.size(); ++index) {
      if (!cases[index].arm) {
        has_poll_case = has_poll_case ||
                        (!cases[index].is_default && cases[index].probe);
        continue;
      }
      cases[index].arm(wait_state, index);
    }

    std::size_t selected_index = SelectResult::kNoSelection;
    SelectProbe selected_probe;
    const auto take_selected = [&] {
      return wait_state->Take(&selected_index, &selected_probe);
    };
    if (take_selected()) {
      disarm(cases, wait_state);
      return {selected_index,
              selected_probe.ready,
              selected_probe.ok,
              std::move(selected_probe.value),
              selected_probe.status,
              std::move(selected_probe.error)};
    }

    DoneSignal::CallbackId callback_id = 0;
    if (context) {
      callback_id = context->Done().AddCallback([wait_state] {
        wait_state->Notify();
      });
    }

    bool custom_selected = false;
    bool channel_selected = false;
    for (;;) {
      if (take_selected()) {
        channel_selected = true;
        break;
      }

      // Armed channel cases must not be polled again: TryRecv/TrySend would
      // consume a transfer without publishing the shared selection state.
      for (std::size_t index = 0; index < cases.size(); ++index) {
        if (cases[index].is_default || cases[index].arm ||
            !cases[index].probe) {
          continue;
        }
        auto probe = cases[index].probe();
        if (probe.ready && wait_state->TrySelect(index, std::move(probe))) {
          custom_selected = true;
          break;
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
    }

    if (context && callback_id != 0) {
      context->Done().RemoveCallback(callback_id);
    }
    if (!channel_selected && !custom_selected) {
      // Linearize timeout/cancellation against a late channel handoff before
      // removing wait nodes. If the handoff won the race, preserve it.
      wait_state->Cancel();
      if (take_selected()) {
        channel_selected = true;
      }
    } else if (custom_selected) {
      channel_selected = take_selected();
    }
    disarm(cases, wait_state);

    if (channel_selected) {
      return {selected_index,
              selected_probe.ready,
              selected_probe.ok,
              std::move(selected_probe.value),
              selected_probe.status,
              std::move(selected_probe.error)};
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
