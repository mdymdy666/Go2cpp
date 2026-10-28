/**
 * @file transport.cpp
 * @brief `ITransport` 的公共便利实现（`ReadFull`/`WriteFull`/`CodeName`）。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/transport.hpp"

#include <cerrno>
#include <chrono>

namespace go2cpp::bento::net {
namespace {

using Clock = std::chrono::steady_clock;

/// 负超时按 0 处理：调用方不应传负值，但接口不应因此挂死。
std::chrono::milliseconds NormalizeTimeout(std::chrono::milliseconds timeout) noexcept {
    return timeout.count() < 0 ? std::chrono::milliseconds(0) : timeout;
}

/**
 * @brief 距离 deadline 还剩多少毫秒（向上取整）。
 *
 * @details 向上取整而不是截断：剩余 0.3ms 时传 0 给 `poll` 会退化成忙轮询，
 *          传 1ms 才既不会提前超时，也不会让内核空转。deadline 已过返回 0。
 */
std::chrono::milliseconds RemainingMs(Clock::time_point deadline) noexcept {
    const Clock::duration left = deadline - Clock::now();
    if (left <= Clock::duration::zero()) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::ceil<std::chrono::milliseconds>(left);
}

}  // namespace

const char* CodeName(Code code) noexcept {
    switch (code) {
        case Code::kOk:
            return "Ok";
        case Code::kTimeout:
            return "Timeout";
        case Code::kClosed:
            return "Closed";
        case Code::kError:
            return "Error";
    }
    return "Unknown";
}

Result ITransport::ReadFull(std::uint8_t* buffer, std::size_t size,
                            std::chrono::milliseconds timeout) {
    Result total;
    if (size == 0) {
        return total;
    }
    if (buffer == nullptr) {
        return Result{Code::kError, EINVAL, 0};
    }

    const Clock::time_point deadline = Clock::now() + NormalizeTimeout(timeout);
    while (total.bytes < size) {
        const std::size_t want = size - total.bytes;
        const Result one = Read(buffer + total.bytes, want, RemainingMs(deadline));
        if (one.bytes > want) {
            // 派生类违约（返回超过请求的字节数）：按 EIO 处理，避免越界累加。
            return Result{Code::kError, EIO, total.bytes};
        }
        total.bytes += one.bytes;
        if (total.bytes == size) {
            break;
        }
        if (one.code != Code::kOk) {
            total.code = one.code;
            total.system_error = one.system_error;
            return total;
        }
        if (one.bytes == 0) {
            // Ok 但 0 字节会导致死循环；实现违约，报 EIO。
            return Result{Code::kError, EIO, total.bytes};
        }
    }
    total.code = Code::kOk;
    total.system_error = 0;
    return total;
}

Result ITransport::WriteFull(const std::uint8_t* data, std::size_t size,
                             std::chrono::milliseconds timeout) {
    Result total;
    if (size == 0) {
        return total;
    }
    if (data == nullptr) {
        return Result{Code::kError, EINVAL, 0};
    }

    const Clock::time_point deadline = Clock::now() + NormalizeTimeout(timeout);
    while (total.bytes < size) {
        const std::size_t want = size - total.bytes;
        const Result one = Write(data + total.bytes, want, RemainingMs(deadline));
        if (one.bytes > want) {
            return Result{Code::kError, EIO, total.bytes};
        }
        total.bytes += one.bytes;
        if (total.bytes == size) {
            break;
        }
        if (one.code != Code::kOk) {
            total.code = one.code;
            total.system_error = one.system_error;
            return total;
        }
        if (one.bytes == 0) {
            return Result{Code::kError, EIO, total.bytes};
        }
    }
    total.code = Code::kOk;
    total.system_error = 0;
    return total;
}

}  // namespace go2cpp::bento::net
