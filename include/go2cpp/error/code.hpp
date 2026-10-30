#pragma once

/**
 * @file code.hpp
 * @brief Go2Cpp 自研错误码：23 个内置语义码 + 语义分类 + 稳定短名。
 *
 * @details 本枚举**不是** gRPC canonical codes 的副本。设计目标有两条：
 *
 * 1. **按失败原因分类**，而不是按 RPC 语义分类。Go2Cpp 是语言运行时
 *    （调度器、通道、IO、编解码、反射），最常见的失败是「类型不对」「参数
 *    非法」「超时」「取消」「资源耗尽」「并发冲突」，这些在 gRPC 词表里
 *    要么没有独立码（类型/格式/超时被并进 INVALID_ARGUMENT /
 *    DEADLINE_EXCEEDED），要么名字与运行时语义不符（DATA_LOSS）。
 * 2. **数字稳定**：内核/插件/监控按整数比较，码值一旦发布不再改。
 *    0 和 1 特意保留为 `kOk`/`kUnknown`，与旧词表、与「0 = 成功」的
 *    通用约定兼容。
 *
 * 取值布局（0–22 为内置码，1000 起为业务私有码）：
 *
 * | 区段 | 语义组 | 码 |
 * |------|--------|----|
 * | 0–1   | 基础   | kOk / kUnknown |
 * | 2–5   | 用法   | kTypeMismatch / kInvalidArgument / kOutOfRange / kFormatError |
 * | 6–7   | 查找   | kNotFound / kAlreadyExists |
 * | 8–9   | 访问   | kPermissionDenied / kUnauthenticated |
 * | 10–13 | 暂态   | kTimeout / kCancelled / kResourceExhausted / kUnavailable |
 * | 14–15 | 状态   | kFailedPrecondition / kAborted |
 * | 16–17 | 数据   | kIOError / kCorruption |
 * | 18–19 | 内部   | kInternal / kUnimplemented |
 * | 20–21 | 并发   | kWouldBlock / kDeadlock |
 * | 22    | 包装   | kWrappedError（WrappedError/JoinError 的 Code） |
 *
 * 业务私有码从 `kFirstCustomCode` 起，用 `static_cast<ErrorCode>` 携带；
 * 公共词表不替它解释语义，`ErrorCodeName()` 对私有码返回 `"UNKNOWN"`。
 *
 * ## 与旧词表的关系
 *
 * 旧版 `go2cpp::error::ErrorCode`（以及 Bento 的 `bento::error::ErrorCode`）
 * 是 csvp 直接照搬的 gRPC 16 码。本文件不再与旧值逐值对齐；跨词表转换统一
 * 走 `go2cpp/error/legacy_code.hpp` 的 `FromLegacyCode()` / `ToLegacyCode()`
 * （Bento 桥见 `go2cpp/bento/error/bridge.hpp`）。**反向映射是有损的**：
 * 类型错误/格式错误都会退化成 INVALID_ARGUMENT，这是旧词表表达能力不足
 * 的必然结果，不是实现缺陷。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstdint>
#include <string_view>

namespace go2cpp::error {

/**
 * @brief Go2Cpp 内置错误码。
 *
 * @details 每个码都对应一类**可被调用方程序化处理**的失败，而不是一句
 *          文本。判断「能不能重试」看 `ErrorCategoryOf()`；判断具体原因
 *          看码本身；需要携带业务语义时用 1000 以上的私有码。
 */
enum class ErrorCode : std::int32_t {
    kOk = 0,                 ///< 成功（“无错误”占位；空 ErrorPtr 也归为该值）。
    kUnknown = 1,            ///< 未知错误：确实无法归类时的兜底。
    kTypeMismatch = 2,       ///< 类型错误：期望 int 得到 string、variant 取错分支等。
    kInvalidArgument = 3,    ///< 参数非法：空值、格式不合法、长度不满足约束、组合矛盾。
    kOutOfRange = 4,         ///< 越界：数组下标、切片范围、数值超出允许区间。
    kFormatError = 5,        ///< 格式错误：解析失败（JSON/CSV/协议帧/文本转数值）。
    kNotFound = 6,           ///< 不存在：文件、键、类型、资源、记录。
    kAlreadyExists = 7,      ///< 已存在：重复注册、重复创建、唯一键冲突。
    kPermissionDenied = 8,   ///< 权限不足：已认证但无权访问（EACCES/EPERM）。
    kUnauthenticated = 9,    ///< 未认证：缺少凭证、凭证无效或已过期。
    kTimeout = 10,           ///< 超时：操作/等待超过期限（含 deadline）。
    kCancelled = 11,         ///< 已取消：context 取消、主动 abort、对端关闭。
    kResourceExhausted = 12, ///< 资源耗尽：内存、fd、句柄、线程/槽位配额。
    kUnavailable = 13,       ///< 暂不可用：服务未启动、依赖断开、对端拒绝连接。
    kFailedPrecondition = 14,///< 前置条件不满足：状态机顺序错误、依赖尚未就绪。
    kAborted = 15,           ///< 操作中止：并发冲突、版本/代际不匹配导致的放弃。
    kIOError = 16,           ///< IO 错误：读写失败、短写、设备错误（非超时/非取消）。
    kCorruption = 17,        ///< 数据损坏：校验和不符、截断、非法编码、内容被篡改。
    kInternal = 18,          ///< 内部错误：不变量被破坏、实现缺陷、不该发生的分支。
    kUnimplemented = 19,     ///< 未实现：接口存在但当前平台/版本未提供。
    kWouldBlock = 20,        ///< 未就绪：非阻塞操作当前无法完成，稍后重试即可。
    kDeadlock = 21,          ///< 死锁：检测到循环等待（锁顺序/等待图成环）。
    kWrappedError = 22,      ///< 包装节点：WrappedError/JoinError 自身的 Code。
};

/// @brief 最后一个内置码的数值；用于区分内置码与保留空洞/私有码。
inline constexpr std::int32_t kLastBuiltinCode = 22;

/// @brief 私有错误码起始值；0–22 归公共词表，23–999 为保留空洞。
inline constexpr std::int32_t kFirstCustomCode = 1000;

/**
 * @brief 错误码的语义分类。
 *
 * @details 分类解决「调用方只想知道怎么处理」的场景：用法错误要改代码，
 *          查找失败要换 key，暂态错误可以重试，内部错误要报 bug。分类
 *          比码更稳定——新增码时只需挂到已有分类上。
 */
enum class ErrorCategory : std::uint8_t {
    kOk = 0,        ///< 成功。
    kUnknown = 1,   ///< 无法归类。
    kUsage = 2,     ///< 调用方用法错误（类型/参数/越界/格式）：重试无用，要改调用。
    kLookup = 3,    ///< 查找类（不存在/已存在）。
    kAccess = 4,    ///< 访问控制（权限/认证）。
    kTransient = 5, ///< 暂态失败（超时/取消/资源/不可用）：可能可重试。
    kState = 6,     ///< 状态冲突（前置条件/中止/死锁）：需要外部干预或重排时序。
    kData = 7,      ///< 数据面（IO/损坏）：要检查存储、链路或输入内容。
    kInternal = 8,  ///< 实现问题（内部/未实现）。
    kWrapped = 9,   ///< 包装节点（不是失败本身，真正的码在原因里）。
};

/**
 * @brief 错误码 → 语义分类。
 * @param code 任意错误码（含私有码）。
 * @return 内置码返回固定分类；私有码/越界值统一返回 `kUnknown`。
 */
constexpr ErrorCategory ErrorCategoryOf(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kOk:
            return ErrorCategory::kOk;
        case ErrorCode::kUnknown:
            return ErrorCategory::kUnknown;
        case ErrorCode::kTypeMismatch:
        case ErrorCode::kInvalidArgument:
        case ErrorCode::kOutOfRange:
        case ErrorCode::kFormatError:
            return ErrorCategory::kUsage;
        case ErrorCode::kNotFound:
        case ErrorCode::kAlreadyExists:
            return ErrorCategory::kLookup;
        case ErrorCode::kPermissionDenied:
        case ErrorCode::kUnauthenticated:
            return ErrorCategory::kAccess;
        case ErrorCode::kTimeout:
        case ErrorCode::kCancelled:
        case ErrorCode::kResourceExhausted:
        case ErrorCode::kUnavailable:
        case ErrorCode::kWouldBlock:
            return ErrorCategory::kTransient;
        case ErrorCode::kFailedPrecondition:
        case ErrorCode::kAborted:
        case ErrorCode::kDeadlock:
            return ErrorCategory::kState;
        case ErrorCode::kIOError:
        case ErrorCode::kCorruption:
            return ErrorCategory::kData;
        case ErrorCode::kInternal:
        case ErrorCode::kUnimplemented:
            return ErrorCategory::kInternal;
        case ErrorCode::kWrappedError:
            return ErrorCategory::kWrapped;
    }
    // 私有码/越界值：公共词表不解释业务语义。
    return ErrorCategory::kUnknown;
}

/**
 * @brief 分类的稳定短名（大写），用于日志/指标标签。
 * @param category 语义分类。
 * @return 固定名字；未知枚举值返回 `"UNKNOWN"`。
 */
constexpr std::string_view ErrorCategoryName(ErrorCategory category) noexcept {
    switch (category) {
        case ErrorCategory::kOk:
            return "OK";
        case ErrorCategory::kUnknown:
            return "UNKNOWN";
        case ErrorCategory::kUsage:
            return "USAGE";
        case ErrorCategory::kLookup:
            return "LOOKUP";
        case ErrorCategory::kAccess:
            return "ACCESS";
        case ErrorCategory::kTransient:
            return "TRANSIENT";
        case ErrorCategory::kState:
            return "STATE";
        case ErrorCategory::kData:
            return "DATA";
        case ErrorCategory::kInternal:
            return "INTERNAL";
        case ErrorCategory::kWrapped:
            return "WRAPPED";
    }
    return "UNKNOWN";
}

/**
 * @brief 错误码的稳定短名（大写，不含 `k` 前缀）。
 *
 * @param code 错误码。
 * @return 内置码返回固定名字；私有码/越界值返回 `"UNKNOWN"`。
 * @note 名字是日志、JSON、监控标签的稳定契约；改名会破坏外部解析，
 *       新增码只能追加，不能替换已有名字。
 */
constexpr std::string_view ErrorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kOk:
            return "OK";
        case ErrorCode::kUnknown:
            return "UNKNOWN";
        case ErrorCode::kTypeMismatch:
            return "TYPE_MISMATCH";
        case ErrorCode::kInvalidArgument:
            return "INVALID_ARGUMENT";
        case ErrorCode::kOutOfRange:
            return "OUT_OF_RANGE";
        case ErrorCode::kFormatError:
            return "FORMAT_ERROR";
        case ErrorCode::kNotFound:
            return "NOT_FOUND";
        case ErrorCode::kAlreadyExists:
            return "ALREADY_EXISTS";
        case ErrorCode::kPermissionDenied:
            return "PERMISSION_DENIED";
        case ErrorCode::kUnauthenticated:
            return "UNAUTHENTICATED";
        case ErrorCode::kTimeout:
            return "TIMEOUT";
        case ErrorCode::kCancelled:
            return "CANCELLED";
        case ErrorCode::kResourceExhausted:
            return "RESOURCE_EXHAUSTED";
        case ErrorCode::kUnavailable:
            return "UNAVAILABLE";
        case ErrorCode::kFailedPrecondition:
            return "FAILED_PRECONDITION";
        case ErrorCode::kAborted:
            return "ABORTED";
        case ErrorCode::kIOError:
            return "IO_ERROR";
        case ErrorCode::kCorruption:
            return "CORRUPTION";
        case ErrorCode::kInternal:
            return "INTERNAL";
        case ErrorCode::kUnimplemented:
            return "UNIMPLEMENTED";
        case ErrorCode::kWouldBlock:
            return "WOULD_BLOCK";
        case ErrorCode::kDeadlock:
            return "DEADLOCK";
        case ErrorCode::kWrappedError:
            return "WRAPPED";
    }
    // 私有码与越界值统一归为 UNKNOWN：公共词表无法也不应该替它解释语义。
    return "UNKNOWN";
}

/// @brief 是否表示成功。
constexpr bool IsOk(ErrorCode code) noexcept { return code == ErrorCode::kOk; }

/// @brief 是否为包装节点码（`WrappedError`/`JoinError`）。
constexpr bool IsWrappedCode(ErrorCode code) noexcept {
    return code == ErrorCode::kWrappedError;
}

/// @brief 是否为内置码（0–22）；保留空洞与私有码都返回 false。
constexpr bool IsBuiltinCode(ErrorCode code) noexcept {
    const std::int32_t value = static_cast<std::int32_t>(code);
    return value >= 0 && value <= kLastBuiltinCode;
}

/// @brief 是否为业务私有码（>= 1000）。
constexpr bool IsCustomCode(ErrorCode code) noexcept {
    return static_cast<std::int32_t>(code) >= kFirstCustomCode;
}

} // namespace go2cpp::error
