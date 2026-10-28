#pragma once

/**
 * @file retry.hpp
 * @brief app 层共用的指数退避等待（fetch / backup 共用一份实现）。
 * @details 下载与备份原本各写了一份同名同义的 `SleepBeforeRetry`，只有
 *          退避上限不同（下载 2s、备份 30s）。这里把「等待时长计算」收敛
 *          到一处，上限由调用方以参数传入，避免以后调整策略时只改一处。
 *
 *          `failed_attempt` 从 1 开始：第 1 次失败后等待 `base`，第 2 次
 *          等待 `2 * base`，依此类推，单次不超过 `cap`；`<= 0` 表示无需等待。
 *
 * @note 这是实现细节命名空间（`app::detail`），不属于公开 API 稳定性承诺。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <algorithm>
#include <chrono>
#include <thread>

namespace go2cpp::bento::app::detail {

/**
 * @brief 指数退避等待。
 * @param base 首次等待时长。
 * @param failed_attempt 已失败次数（1 表示第一次失败）。
 * @param cap 单次等待上限。
 */
inline void SleepBeforeRetry(std::chrono::milliseconds base, int failed_attempt,
                             std::chrono::milliseconds cap) {
    if (base.count() <= 0 || failed_attempt <= 0 || cap.count() <= 0) {
        return;
    }
    std::chrono::milliseconds delay = std::min(base, cap);
    for (int i = 1; i < failed_attempt && delay < cap; ++i) {
        delay = std::min(delay * 2, cap);
    }
    std::this_thread::sleep_for(delay);
}

}  // namespace go2cpp::bento::app::detail
