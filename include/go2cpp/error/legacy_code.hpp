#pragma once

/**
 * @file legacy_code.hpp
 * @brief 旧 csvp/gRPC 16 码 ↔ Go2Cpp 新码的兼容映射。
 *
 * @details 旧词表（csvp 照搬 gRPC canonical codes）在 Go2Cpp 里只作为
 *          **兼容层**保留：它没有「类型错误 / 格式错误 / IO 错误 / 未就绪 /
 *          死锁」这些运行时常见分类，而且 `DATA_LOSS`、`DEADLINE_EXCEEDED`
 *          的名字与语义都不适合直接描述本地失败。本文件提供两个方向：
 *
 * - `FromLegacyCode()`：旧码 → 新码，**单射且无歧义**，旧的 16 个值各有
 *   一个确定的新码；
 * - `ToLegacyCode()`：新码 → 旧码，**有损**。旧词表装不下的新码会退化到
 *   语义最接近的旧码（例如类型错误/格式错误都退化为 INVALID_ARGUMENT），
 *   调用方可用 `IsLegacyLossless()` 判断某个新码能否无损往返。
 *
 * 数值契约：`LegacyErrorCode` 的取值与旧版 `ErrorCode`（以及 Bento 的
 * `bento::error::ErrorCode`）逐值一致，因此旧代码里的
 * `static_cast<LegacyErrorCode>(3)` 仍然表示 NOT_FOUND。新代码不要再用
 * 裸整数表示新码——两者数值不同，混用是静默错误。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/error/code.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace go2cpp::error {

/**
 * @brief 旧 csvp/gRPC 词表的 16 个码；仅用于跨版本/跨模块兼容。
 *
 * @note 新代码一律使用 `ErrorCode`；本枚举只出现在迁移边界。
 */
enum class LegacyErrorCode : std::int32_t {
    kOk = 0,                 ///< 成功。
    kUnknown = 1,            ///< 未知。
    kInvalidArgument = 2,    ///< 参数非法。
    kNotFound = 3,           ///< 不存在。
    kAlreadyExists = 4,      ///< 已存在。
    kPermissionDenied = 5,   ///< 权限不足。
    kResourceExhausted = 6,  ///< 资源耗尽。
    kFailedPrecondition = 7, ///< 前置条件不满足。
    kOutOfRange = 8,         ///< 越界。
    kUnimplemented = 9,      ///< 未实现。
    kInternal = 10,          ///< 内部错误。
    kUnavailable = 11,       ///< 不可用。
    kDataLoss = 12,          ///< 数据丢失/损坏。
    kCancelled = 13,         ///< 已取消。
    kDeadlineExceeded = 14,  ///< 超过期限。
    kWrappedError = 15,      ///< 包装节点。
};

/// @brief 旧词表的码数量（0–15）。
inline constexpr std::int32_t kLegacyCodeCount = 16;

/**
 * @brief 一条兼容映射记录。
 *
 * @details `note` 解释「为什么不是同名直译」，便于排查跨词表日志。
 */
struct LegacyCodeMapping {
    LegacyErrorCode legacy; ///< 旧码。
    ErrorCode current;      ///< 对应的 Go2Cpp 新码。
    std::string_view note;  ///< 语义说明。
};

/**
 * @brief 旧码 → 新码的完整映射表（按旧码数值升序，索引即旧码值）。
 *
 * @note 表是 `constexpr` 的，`FromLegacyCode()` 直接按索引取值；新增旧码
 *       必须同时扩充 `kLegacyCodeCount` 与静态断言，否则编译期失败。
 */
inline constexpr std::array<LegacyCodeMapping, kLegacyCodeCount> kLegacyCodeMap = {{
    {LegacyErrorCode::kOk, ErrorCode::kOk, "同义：成功"},
    {LegacyErrorCode::kUnknown, ErrorCode::kUnknown, "同义：无法归类"},
    {LegacyErrorCode::kInvalidArgument, ErrorCode::kInvalidArgument, "同义：参数非法"},
    {LegacyErrorCode::kNotFound, ErrorCode::kNotFound, "同义：不存在"},
    {LegacyErrorCode::kAlreadyExists, ErrorCode::kAlreadyExists, "同义：已存在"},
    {LegacyErrorCode::kPermissionDenied, ErrorCode::kPermissionDenied, "同义：权限不足"},
    {LegacyErrorCode::kResourceExhausted, ErrorCode::kResourceExhausted, "同义：资源耗尽"},
    {LegacyErrorCode::kFailedPrecondition, ErrorCode::kFailedPrecondition, "同义：前置条件"},
    {LegacyErrorCode::kOutOfRange, ErrorCode::kOutOfRange, "同义：越界"},
    {LegacyErrorCode::kUnimplemented, ErrorCode::kUnimplemented, "同义：未实现"},
    {LegacyErrorCode::kInternal, ErrorCode::kInternal, "同义：内部错误"},
    {LegacyErrorCode::kUnavailable, ErrorCode::kUnavailable, "同义：暂不可用"},
    {LegacyErrorCode::kDataLoss, ErrorCode::kCorruption, "改名：DATA_LOSS 在本地语义里就是数据损坏/校验失败"},
    {LegacyErrorCode::kCancelled, ErrorCode::kCancelled, "同义：已取消"},
    {LegacyErrorCode::kDeadlineExceeded, ErrorCode::kTimeout, "改名：deadline 是超时的一种，统一为 kTimeout"},
    {LegacyErrorCode::kWrappedError, ErrorCode::kWrappedError, "同义：包装节点"},
}};

/// @brief 旧码的稳定短名；未知值返回 `"UNKNOWN"`。
constexpr std::string_view LegacyErrorCodeName(LegacyErrorCode code) noexcept {
    switch (code) {
        case LegacyErrorCode::kOk:
            return "OK";
        case LegacyErrorCode::kUnknown:
            return "UNKNOWN";
        case LegacyErrorCode::kInvalidArgument:
            return "INVALID_ARGUMENT";
        case LegacyErrorCode::kNotFound:
            return "NOT_FOUND";
        case LegacyErrorCode::kAlreadyExists:
            return "ALREADY_EXISTS";
        case LegacyErrorCode::kPermissionDenied:
            return "PERMISSION_DENIED";
        case LegacyErrorCode::kResourceExhausted:
            return "RESOURCE_EXHAUSTED";
        case LegacyErrorCode::kFailedPrecondition:
            return "FAILED_PRECONDITION";
        case LegacyErrorCode::kOutOfRange:
            return "OUT_OF_RANGE";
        case LegacyErrorCode::kUnimplemented:
            return "UNIMPLEMENTED";
        case LegacyErrorCode::kInternal:
            return "INTERNAL";
        case LegacyErrorCode::kUnavailable:
            return "UNAVAILABLE";
        case LegacyErrorCode::kDataLoss:
            return "DATA_LOSS";
        case LegacyErrorCode::kCancelled:
            return "CANCELLED";
        case LegacyErrorCode::kDeadlineExceeded:
            return "DEADLINE_EXCEEDED";
        case LegacyErrorCode::kWrappedError:
            return "WRAPPED";
    }
    return "UNKNOWN";
}

/**
 * @brief 旧码 → 新码。
 * @param code 旧码；越界枚举值按 `kUnknown` 处理，不抛异常。
 */
constexpr ErrorCode FromLegacyCode(LegacyErrorCode code) noexcept {
    const std::int32_t value = static_cast<std::int32_t>(code);
    if (value < 0 || value >= kLegacyCodeCount) {
        return ErrorCode::kUnknown;
    }
    return kLegacyCodeMap[static_cast<std::size_t>(value)].current;
}

/**
 * @brief 旧码数值 → 新码（从配置/线协议读到的裸整数走这个入口）。
 * @param code 旧词表数值；0–15 之外返回 `kUnknown`。
 * @note 参数是**旧**词表数值；新码数值请直接 `static_cast<ErrorCode>`。
 */
constexpr ErrorCode FromLegacyCode(std::int32_t code) noexcept {
    if (code < 0 || code >= kLegacyCodeCount) {
        return ErrorCode::kUnknown;
    }
    return kLegacyCodeMap[static_cast<std::size_t>(code)].current;
}

/**
 * @brief 新码 → 旧码（有损）。
 *
 * @details 旧词表没有独立分类的新码按语义就近退化：
 *          - 类型错误 / 格式错误 → INVALID_ARGUMENT（都是调用方用法问题）；
 *          - 未认证 → PERMISSION_DENIED（旧词表没有 UNAUTHENTICATED）；
 *          - IO 错误 → UNAVAILABLE（旧词表把 IO 失败视作暂不可用）；
 *          - 数据损坏 → DATA_LOSS；
 *          - 中止 / 死锁 → FAILED_PRECONDITION（状态冲突）；
 *          - 未就绪 → UNAVAILABLE（稍后重试）；
 *          - 包装节点 → WRAPPED_ERROR。
 *          往返无损的码见 `IsLegacyLossless()`。
 */
constexpr LegacyErrorCode ToLegacyCode(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kOk:
            return LegacyErrorCode::kOk;
        case ErrorCode::kUnknown:
            return LegacyErrorCode::kUnknown;
        case ErrorCode::kTypeMismatch:
            return LegacyErrorCode::kInvalidArgument;
        case ErrorCode::kInvalidArgument:
            return LegacyErrorCode::kInvalidArgument;
        case ErrorCode::kOutOfRange:
            return LegacyErrorCode::kOutOfRange;
        case ErrorCode::kFormatError:
            return LegacyErrorCode::kInvalidArgument;
        case ErrorCode::kNotFound:
            return LegacyErrorCode::kNotFound;
        case ErrorCode::kAlreadyExists:
            return LegacyErrorCode::kAlreadyExists;
        case ErrorCode::kPermissionDenied:
            return LegacyErrorCode::kPermissionDenied;
        case ErrorCode::kUnauthenticated:
            return LegacyErrorCode::kPermissionDenied;
        case ErrorCode::kTimeout:
            return LegacyErrorCode::kDeadlineExceeded;
        case ErrorCode::kCancelled:
            return LegacyErrorCode::kCancelled;
        case ErrorCode::kResourceExhausted:
            return LegacyErrorCode::kResourceExhausted;
        case ErrorCode::kUnavailable:
            return LegacyErrorCode::kUnavailable;
        case ErrorCode::kFailedPrecondition:
            return LegacyErrorCode::kFailedPrecondition;
        case ErrorCode::kAborted:
            return LegacyErrorCode::kFailedPrecondition;
        case ErrorCode::kIOError:
            return LegacyErrorCode::kUnavailable;
        case ErrorCode::kCorruption:
            return LegacyErrorCode::kDataLoss;
        case ErrorCode::kInternal:
            return LegacyErrorCode::kInternal;
        case ErrorCode::kUnimplemented:
            return LegacyErrorCode::kUnimplemented;
        case ErrorCode::kWouldBlock:
            return LegacyErrorCode::kUnavailable;
        case ErrorCode::kDeadlock:
            return LegacyErrorCode::kFailedPrecondition;
        case ErrorCode::kWrappedError:
            return LegacyErrorCode::kWrappedError;
    }
    return LegacyErrorCode::kUnknown;
}

/**
 * @brief 新码能否经过旧词表无损往返（`ToLegacyCode` 后再 `FromLegacyCode`）。
 *
 * @details 为 true 的码在跨旧模块传递时不会丢语义；为 false 的码只能退化，
 *          调用方若要保留精度，应直接传新码而不是走旧通道。
 */
constexpr bool IsLegacyLossless(ErrorCode code) noexcept {
    return FromLegacyCode(ToLegacyCode(code)) == code;
}

namespace detail {

/// @brief 校验映射表按旧码数值排列（索引 == 旧码值）。
constexpr bool LegacyMapIsOrdered() noexcept {
    for (std::size_t index = 0; index < kLegacyCodeMap.size(); ++index) {
        if (static_cast<std::size_t>(static_cast<std::int32_t>(kLegacyCodeMap[index].legacy)) !=
            index) {
            return false;
        }
    }
    return true;
}

} // namespace detail

static_assert(detail::LegacyMapIsOrdered(),
              "kLegacyCodeMap 必须按旧码数值升序排列，FromLegacyCode 用索引查表");

// 旧码数值与旧版枚举逐值一致：这是兼容层的硬契约。
static_assert(static_cast<std::int32_t>(LegacyErrorCode::kDataLoss) == 12);
static_assert(static_cast<std::int32_t>(LegacyErrorCode::kDeadlineExceeded) == 14);
static_assert(static_cast<std::int32_t>(LegacyErrorCode::kWrappedError) == 15);

// 16 个旧码必须全部可无损往返（否则 FromLegacyCode 的单射性被破坏）。
static_assert(IsLegacyLossless(ErrorCode::kOk));
static_assert(IsLegacyLossless(ErrorCode::kUnknown));
static_assert(IsLegacyLossless(ErrorCode::kInvalidArgument));
static_assert(IsLegacyLossless(ErrorCode::kNotFound));
static_assert(IsLegacyLossless(ErrorCode::kAlreadyExists));
static_assert(IsLegacyLossless(ErrorCode::kPermissionDenied));
static_assert(IsLegacyLossless(ErrorCode::kResourceExhausted));
static_assert(IsLegacyLossless(ErrorCode::kFailedPrecondition));
static_assert(IsLegacyLossless(ErrorCode::kOutOfRange));
static_assert(IsLegacyLossless(ErrorCode::kUnimplemented));
static_assert(IsLegacyLossless(ErrorCode::kInternal));
static_assert(IsLegacyLossless(ErrorCode::kUnavailable));
static_assert(IsLegacyLossless(ErrorCode::kCorruption));
static_assert(IsLegacyLossless(ErrorCode::kCancelled));
static_assert(IsLegacyLossless(ErrorCode::kTimeout));
static_assert(IsLegacyLossless(ErrorCode::kWrappedError));

// 新词表独有、旧词表装不下的码必须被标记为有损。
static_assert(!IsLegacyLossless(ErrorCode::kTypeMismatch));
static_assert(!IsLegacyLossless(ErrorCode::kFormatError));
static_assert(!IsLegacyLossless(ErrorCode::kUnauthenticated));
static_assert(!IsLegacyLossless(ErrorCode::kAborted));
static_assert(!IsLegacyLossless(ErrorCode::kIOError));
static_assert(!IsLegacyLossless(ErrorCode::kWouldBlock));
static_assert(!IsLegacyLossless(ErrorCode::kDeadlock));

} // namespace go2cpp::error
