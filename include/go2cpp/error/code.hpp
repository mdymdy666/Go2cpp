#pragma once

/**
 * @file code.hpp
 * @brief 统一错误码：16 种稳定语义码，语义对齐 gRPC 状态码。
 *
 * @details 本枚举是 `go2cpp::error` 体系的公共词表，与参考实现 csvp 的
 *          `ErrorCode` 一一对应（含 `kWrappedError`）。取值 0–15 永不变更，
 *          业务私有码从 `kFirstCustomCode`（1000）起，通过
 *          `static_cast<ErrorCode>` 携带，`ErrorCodeName()` 对私有码返回
 *          `"UNKNOWN"`，由调用方自行解释。
 *
 *          `go2cpp::error::ErrorCode` 与 `go2cpp::bento::error::ErrorCode`
 *          数值完全一致（同样的 16 个名字），因此两侧互转是零成本的
 *          `static_cast`；这一约定由 `tests/error/test_error_enhanced.cpp`
 *          的静态断言锁定，任何一侧增删枚举值都会在编译期暴露。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstdint>
#include <string_view>

namespace go2cpp::error {

/**
 * @brief 通用错误码，语义对齐 gRPC 状态码。
 *
 * @details 0–14 与 gRPC 状态一一对应；15 保留给包装节点
 *          （`WrappedError`/`JoinError`）。业务私有码从 1000 起。
 */
enum class ErrorCode : std::int32_t {
    kOk = 0,                 ///< 成功（用于“无错误”的占位）。
    kUnknown = 1,            ///< 未知错误。
    kInvalidArgument = 2,    ///< 参数非法。
    kNotFound = 3,           ///< 目标不存在。
    kAlreadyExists = 4,      ///< 目标已存在。
    kPermissionDenied = 5,   ///< 权限不足。
    kResourceExhausted = 6,  ///< 资源耗尽（连接/内存/配额）。
    kFailedPrecondition = 7, ///< 当前状态不满足前置条件。
    kOutOfRange = 8,         ///< 越界（数值/索引/长度）。
    kUnimplemented = 9,      ///< 功能未实现。
    kInternal = 10,          ///< 内部错误。
    kUnavailable = 11,       ///< 服务/对端暂不可用。
    kDataLoss = 12,          ///< 数据损坏或丢失。
    kCancelled = 13,         ///< 调用方取消。
    kDeadlineExceeded = 14,  ///< 超时。
    kWrappedError = 15,      ///< 包装节点（WrappedError/JoinError 的 Code）。
};

/// @brief 私有错误码的起始值；0–15 由公共枚举占用，动态码从 1000 起。
inline constexpr std::int32_t kFirstCustomCode = 1000;

/**
 * @brief 枚举码的稳定短名（大写，不含 `k` 前缀）。
 *
 * @param code 错误码。
 * @return 已知码返回固定名字；私有码/未知值返回 `"UNKNOWN"`。
 * @note 名字是日志、JSON、监控标签的稳定契约，不要因为改名而破坏外部解析。
 */
inline std::string_view ErrorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kOk:
            return "OK";
        case ErrorCode::kUnknown:
            return "UNKNOWN";
        case ErrorCode::kInvalidArgument:
            return "INVALID_ARGUMENT";
        case ErrorCode::kNotFound:
            return "NOT_FOUND";
        case ErrorCode::kAlreadyExists:
            return "ALREADY_EXISTS";
        case ErrorCode::kPermissionDenied:
            return "PERMISSION_DENIED";
        case ErrorCode::kResourceExhausted:
            return "RESOURCE_EXHAUSTED";
        case ErrorCode::kFailedPrecondition:
            return "FAILED_PRECONDITION";
        case ErrorCode::kOutOfRange:
            return "OUT_OF_RANGE";
        case ErrorCode::kUnimplemented:
            return "UNIMPLEMENTED";
        case ErrorCode::kInternal:
            return "INTERNAL";
        case ErrorCode::kUnavailable:
            return "UNAVAILABLE";
        case ErrorCode::kDataLoss:
            return "DATA_LOSS";
        case ErrorCode::kCancelled:
            return "CANCELLED";
        case ErrorCode::kDeadlineExceeded:
            return "DEADLINE_EXCEEDED";
        case ErrorCode::kWrappedError:
            return "WRAPPED";
    }
    // 私有码与越界值统一归为 UNKNOWN：调用方用 `static_cast` 携带业务码时，
    // 公共词表无法也不应该替它解释语义。
    return "UNKNOWN";
}

/// @brief 是否表示成功。
inline constexpr bool IsOk(ErrorCode code) noexcept {
    return code == ErrorCode::kOk;
}

/// @brief 是否为包装节点码（`WrappedError`/`JoinError`）。
inline constexpr bool IsWrappedCode(ErrorCode code) noexcept {
    return code == ErrorCode::kWrappedError;
}

/// @brief 是否为公共枚举范围内的码（0–15）。
inline constexpr bool IsBuiltinCode(ErrorCode code) noexcept {
    return static_cast<std::int32_t>(code) >= 0 &&
           static_cast<std::int32_t>(code) < kFirstCustomCode;
}

} // namespace go2cpp::error
