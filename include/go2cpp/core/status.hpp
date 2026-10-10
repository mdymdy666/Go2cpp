#pragma once

namespace go2cpp::core {

/**
 * @brief 核心模块统一状态码。
 * @details 用于表示取消、截止时间、关闭和调度器生命周期结果；具体
 *          API 仍会同时返回更详细的 ErrorPtr。
 */
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
