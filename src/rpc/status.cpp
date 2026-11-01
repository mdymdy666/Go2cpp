/**
 * @file status.cpp
 * @brief `Status` / `RpcError` / 状态码名字的实现。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/rpc/status.hpp"

#include <utility>

namespace go2cpp::rpc {

const char* StatusCodeName(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::kOk:
            return "OK";
        case StatusCode::kCancelled:
            return "CANCELLED";
        case StatusCode::kUnknown:
            return "UNKNOWN";
        case StatusCode::kInvalidArgument:
            return "INVALID_ARGUMENT";
        case StatusCode::kDeadlineExceeded:
            return "DEADLINE_EXCEEDED";
        case StatusCode::kNotFound:
            return "NOT_FOUND";
        case StatusCode::kAlreadyExists:
            return "ALREADY_EXISTS";
        case StatusCode::kPermissionDenied:
            return "PERMISSION_DENIED";
        case StatusCode::kResourceExhausted:
            return "RESOURCE_EXHAUSTED";
        case StatusCode::kFailedPrecondition:
            return "FAILED_PRECONDITION";
        case StatusCode::kAborted:
            return "ABORTED";
        case StatusCode::kOutOfRange:
            return "OUT_OF_RANGE";
        case StatusCode::kUnimplemented:
            return "UNIMPLEMENTED";
        case StatusCode::kInternal:
            return "INTERNAL";
        case StatusCode::kUnavailable:
            return "UNAVAILABLE";
        case StatusCode::kDataLoss:
            return "DATA_LOSS";
    }
    return "UNKNOWN";
}

std::string Status::ToString() const {
    if (ok()) {
        return "OK";
    }
    std::string text(StatusCodeName(code));
    if (!message.empty()) {
        text += ": ";
        text += message;
    }
    return text;
}

RpcError::RpcError(Status status)
    : std::runtime_error(status.ToString()), m_status(std::move(status)) {}

}  // namespace go2cpp::rpc
