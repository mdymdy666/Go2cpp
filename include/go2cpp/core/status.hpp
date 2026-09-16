#pragma once

namespace go2cpp::core {

enum class StatusCode {
    kOk = 0,
    kCancelled,
    kDeadlineExceeded,
    kClosed,
    kAlreadyClosed,
    kInvalidState,
    kShutdown,
};

}  // namespace go2cpp::core
