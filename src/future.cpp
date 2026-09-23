#include "go2cpp/future.hpp"

#include <mutex>
#include <string>
#include <utility>

namespace go2cpp {

namespace {

const char* StatusName(FutureStatus status) noexcept {
    switch (status) {
    case FutureStatus::kReady:
        return "ready";
    case FutureStatus::kError:
        return "error";
    case FutureStatus::kException:
        return "exception";
    case FutureStatus::kCancelled:
        return "cancelled";
    case FutureStatus::kTimedOut:
        return "timed out";
    case FutureStatus::kDeadlineExceeded:
        return "deadline exceeded";
    case FutureStatus::kInvalid:
        return "invalid";
    }
    return "unknown";
}

}  // namespace

const char* FutureStatusName(FutureStatus status) noexcept {
    return StatusName(status);
}

FutureError::FutureError(FutureStatus status, ErrorPtr error)
    : std::runtime_error(error ? error->Message()
                               : std::string("go2cpp future error")),
      m_status(status),
      m_error(std::move(error)) {}

FutureError::~FutureError() = default;

ErrorPtr BrokenPromiseError() {
    // 共享不可变错误对象，避免每个未完成 Promise 析构时重复分配。
    static const ErrorPtr error =
        NewError("go2cpp future promise was abandoned");
    return error;
}

}  // namespace go2cpp
